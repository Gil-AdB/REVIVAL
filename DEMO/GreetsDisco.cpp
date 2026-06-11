#include "GreetsDisco.h"

#include <Base/FDS_DECS.H>  // Matrix_Copy + Mat_ID + getAligned* + Render
#include <Base/FDS_VARS.H>  // MatrixXVector + FList/SList/CAll + FOVX/...
#include <Base/Face.h>
#include <Base/FeatureFlags.h>
#include <Base/FrameState.h>  // fds::g_mainCamera / g_mainFaces
#include <Base/Material.h>
#include <Base/Object.h>
#include <Base/Omni.h>
#include <Base/Scene.h>
#include <Base/TriMesh.h>
#include <Base/Vertex.h>
#include <RENDER/OffscreenView.h>

#include "SpotlightCones.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Provided by MISC/PREPROC.CPP — stamps F->A_idx/B_idx/C_idx for SoA.
extern void Compute_FaceVertexIndices(TriMesh *T);
extern void Build_YOffs_Table(VESA_Surface *VS);

namespace fds {

namespace {

constexpr int   kRings          = 10;   // latitude bands
constexpr int   kSegs           = 14;   // longitude segments
constexpr float kRadius         = 1.0f;
constexpr int   kSpotCount      = 6;    // rotating dot cones
constexpr float kSpinRadPerTick = 0.008f;  // ~1 rev / 8 s at 100 ticks/s
constexpr float kBobAmp         = 0.12f;   // gentle vertical sway
constexpr float kBobRadPerTick  = 0.012f;
constexpr int   kCubeRes        = 256;  // bake cube-face resolution
// TheOtherBarry's TEXTURETEXTURE env sampler hardcodes the env
// texture to 1024² (t1_umask = (1<<10)-1, ×1024.0f) — any other size
// samples out of bounds (black/garbage facets). cuberefl's panorama
// is 1024² for the same reason.
constexpr int   kPanoRes        = 1024;
constexpr float kPI             = 3.14159265f;

TriMesh *s_ball = nullptr;
Texture *s_panoTex = nullptr;
Vector   s_ballPos = {0, 0, 0};
Omni    *s_spots[kSpotCount] = {};
Vector   s_spotBase[kSpotCount];
bool     s_panoBaked = false;

// Cube-face camera bases (right, up, forward). The equirect resample
// below projects through the SAME bases, so the pair is self-
// consistent by construction — only the panorama↔direction formula
// has to match Transform.cpp's env lookup:
//   lat = asin(d.y)            ev = 0.5 − lat/π
//   lon = atan2(−d.z, −d.x)    eu = 0.5 + 0.5·(lon + π/2)/π
// ⇒ d = ( −cos·cos(lon), sin(lat), −cos·sin(lon) ).
struct CubeBasis { Vector right, up, fwd; };
const CubeBasis kCube[6] = {
    { { 0, 0, 1}, {0, 1, 0}, { 1, 0, 0} },   // +x
    { { 0, 0,-1}, {0, 1, 0}, {-1, 0, 0} },   // -x
    { { 1, 0, 0}, {0, 0, 1}, { 0, 1, 0} },   // +y
    { { 1, 0, 0}, {0, 0,-1}, { 0,-1, 0} },   // -y
    { {-1, 0, 0}, {0, 1, 0}, { 0, 0, 1} },   // +z
    { { 1, 0, 0}, {0, 1, 0}, { 0, 0,-1} },   // -z
};

void BakePanorama(Scene *sc);

}  // namespace

bool BuildDiscoBall(Scene *sc)
{
    if (!FeatureFlags::greets_disco()) return false;
    s_ball = nullptr;
    s_panoBaked = false;

    // ── Placement: room bbox of the loader meshes ───────────────────
    Vector bbMin = { 1e30f, 1e30f, 1e30f};
    Vector bbMax = {-1e30f,-1e30f,-1e30f};
    for (Object *Obj = sc->ObjectHead; Obj; Obj = Obj->Next) {
        if (Obj->Type != Obj_TriMesh || !Obj->Data) continue;
        TriMesh *T = (TriMesh*)Obj->Data;
        if (!T->Verts) continue;
        for (DWord vi = 0; vi < T->VIndex; ++vi) {
            Vector lp = T->Verts[vi].Pos, wp;
            MatrixXVector(T->RotMat, &lp, &wp);
            wp.x += T->IPos.x; wp.y += T->IPos.y; wp.z += T->IPos.z;
            bbMin.x = std::min(bbMin.x, wp.x); bbMax.x = std::max(bbMax.x, wp.x);
            bbMin.y = std::min(bbMin.y, wp.y); bbMax.y = std::max(bbMax.y, wp.y);
            bbMin.z = std::min(bbMin.z, wp.z); bbMax.z = std::max(bbMax.z, wp.z);
        }
    }
    const float R = kRadius;
    // Corridor center, camera-band height + headroom. The scene bbox
    // (logged below) is skewed by outlying geometry (roof apex at
    // y=18.5, side rooms out to x=49) — the flight corridor itself is
    // x≈0, z∈±21, so the spot is pinned rather than derived.
    // FDS_DISCO_POS="x,y,z" overrides for tuning.
    s_ballPos = { 0.0f, 6.5f, 0.0f };
    if (const char *e = std::getenv("FDS_DISCO_POS")) {
        float px, py, pz;
        if (std::sscanf(e, "%f,%f,%f", &px, &py, &pz) == 3)
            s_ballPos = { px, py, pz };
    }
    std::fprintf(stderr,
        "[DISCO] room bbox (%.1f %.1f %.1f)-(%.1f %.1f %.1f) ball at (%.2f %.2f %.2f) r=%.2f\n",
        bbMin.x, bbMin.y, bbMin.z, bbMax.x, bbMax.y, bbMax.z,
        s_ballPos.x, s_ballPos.y, s_ballPos.z, R);

    // ── Panorama + base textures, chrome material ───────────────────
    // Exactly the cuberefl harness recipe (DEMO/Snapshot.cpp): black
    // 16² base so TheOtherBarry<TEXTURETEXTURE>'s `env + base/2`
    // colorize shows pure panorama, Txtr_Nomip|Txtr_Tiled + Sachletz.
    // The panorama starts mid-gray; BakePanorama overwrites it with a
    // real render of the room from the ball's position on the first
    // tick (everything renderable by then — init is too early).
    s_panoTex = new Texture;
    std::memset(s_panoTex, 0, sizeof(Texture));
    uint32_t *panoData = (uint32_t*)_aligned_malloc(kPanoRes * kPanoRes * 4, 16);
    for (int i = 0; i < kPanoRes * kPanoRes; ++i) panoData[i] = 0xFF505050u;
    s_panoTex->Data   = (byte*)panoData;
    s_panoTex->BPP    = 32;
    s_panoTex->SizeX  = kPanoRes; s_panoTex->LSizeX = 10;
    s_panoTex->SizeY  = kPanoRes; s_panoTex->LSizeY = 10;
    s_panoTex->Flags  = Txtr_Nomip | Txtr_Tiled;
    s_panoTex->Mipmap[0]  = s_panoTex->Data;
    s_panoTex->numMipmaps = 1;

    Texture *baseTex = new Texture;
    std::memset(baseTex, 0, sizeof(Texture));
    constexpr int BASE_SZ = 16;
    uint32_t *baseData = (uint32_t*)_aligned_malloc(BASE_SZ * BASE_SZ * 4, 16);
    for (int i = 0; i < BASE_SZ * BASE_SZ; ++i) baseData[i] = 0xFF000000u;
    baseTex->Data   = (byte*)baseData;
    baseTex->BPP    = 32;
    baseTex->SizeX  = BASE_SZ; baseTex->LSizeX = 4;
    baseTex->SizeY  = BASE_SZ; baseTex->LSizeY = 4;
    baseTex->Flags  = Txtr_Nomip | Txtr_Tiled;
    Sachletz((dword*)baseTex->Data, BASE_SZ, BASE_SZ);
    baseTex->Mipmap[0]  = baseTex->Data;
    baseTex->numMipmaps = 1;

    Material *M = getAlignedType<Material>(16);
    M->Txtr       = baseTex;
    M->EnvTexture = s_panoTex;
    M->BaseCol.B  = M->BaseCol.G = M->BaseCol.R = 255;
    M->BaseCol.A  = 255;
    M->Diffuse    = 1.0f;
    M->Reflection = 100.0f;
    M->Luminosity = 0.0f;
    M->Flags      = Mat_RGBInterp;  // NOT TwoSided: the ball backface-culls
    M->RelScene   = sc;
    M->ID         = 0;  // forward env path — never commits a G-buffer matID
    M->Name       = strdup("disco_ball");

    // ── Faceted sphere mesh (local space, centered at origin) ───────
    const int maxVerts = kRings * kSegs * 4;
    const int maxFaces = kRings * kSegs * 2;
    TriMesh *MM = getAlignedType<TriMesh>(16);
    std::memset(MM, 0, sizeof(TriMesh));
    MM->Verts = (Vertex*)getAlignedBlock(sizeof(Vertex) * size_t(maxVerts), 16);
    MM->Faces = (Face*)getAlignedBlock(sizeof(Face) * size_t(maxFaces), 16);
    std::memset(MM->Verts, 0, sizeof(Vertex) * size_t(maxVerts));
    std::memset(MM->Faces, 0, sizeof(Face) * size_t(maxFaces));
    Matrix_Copy(MM->RotMat, Mat_ID);
    Matrix_Copy(MM->UnscaledRotMat, Mat_ID);
    MM->IPos   = s_ballPos;
    MM->IScale = {1.0f, 1.0f, 1.0f};
    MM->IRot   = {0.0f, 0.0f, 0.0f, 1.0f};

    DWord vOfs = 0, fOfs = 0;
    for (int r = 0; r < kRings; ++r) {
        const float la0 = -0.5f * kPI + kPI * float(r)     / kRings;
        const float la1 = -0.5f * kPI + kPI * float(r + 1) / kRings;
        for (int s2 = 0; s2 < kSegs; ++s2) {
            const float lo0 = 2.0f * kPI * float(s2)     / kSegs;
            const float lo1 = 2.0f * kPI * float(s2 + 1) / kSegs;
            auto sph = [&](float la, float lo) -> Vector {
                return { R * std::cos(la) * std::cos(lo),
                         R * std::sin(la),
                         R * std::cos(la) * std::sin(lo) };
            };
            // Quad corners, CCW seen from outside.
            const Vector p00 = sph(la0, lo0), p01 = sph(la0, lo1);
            const Vector p11 = sph(la1, lo1), p10 = sph(la1, lo0);
            // Flat facet normal (outward).
            Vector e1 = { p01.x - p00.x, p01.y - p00.y, p01.z - p00.z };
            Vector e2 = { p10.x - p00.x, p10.y - p00.y, p10.z - p00.z };
            Vector out = { e1.y * e2.z - e1.z * e2.y,
                           e1.z * e2.x - e1.x * e2.z,
                           e1.x * e2.y - e1.y * e2.x };
            // Degenerate at the poles (p00==p01 or p10==p11): use the
            // facet-center direction instead.
            float ol = std::sqrt(out.x*out.x + out.y*out.y + out.z*out.z);
            if (ol < 1e-6f) {
                out = { 0.25f * (p00.x+p01.x+p10.x+p11.x),
                        0.25f * (p00.y+p01.y+p10.y+p11.y),
                        0.25f * (p00.z+p01.z+p10.z+p11.z) };
                ol = std::sqrt(out.x*out.x + out.y*out.y + out.z*out.z);
            }
            out.x /= ol; out.y /= ol; out.z /= ol;
            const float ctrDot = out.x * (p00.x + p11.x) +
                                 out.y * (p00.y + p11.y) +
                                 out.z * (p00.z + p11.z);
            if (ctrDot < 0.0f) { out.x = -out.x; out.y = -out.y; out.z = -out.z; }
            Vector tan = { -std::sin(0.5f * (lo0 + lo1)), 0.0f,
                            std::cos(0.5f * (lo0 + lo1)) };

            const DWord base = vOfs;
            const Vector corner[4] = { p00, p01, p11, p10 };
            const float  cu[4] = { 0.06f, 0.94f, 0.94f, 0.06f };
            const float  cv[4] = { 0.94f, 0.94f, 0.06f, 0.06f };
            for (int c = 0; c < 4; ++c) {
                Vertex &V = MM->Verts[vOfs];
                V.Pos = corner[c];
                V.N = out;            // vertex normals OUTWARD (faceted)
                V.Tangent = tan;
                V.U = cu[c]; V.V = cv[c];
                V.LR = V.LG = V.LB = 250;  // full-bright mirror facets;
                V.LA = 255;                // Tri_Noshading keeps them
                V.i = -1;
                ++vOfs;
            }
            auto emit = [&](DWord a, DWord b, DWord c) {
                Face &CF = MM->Faces[fOfs];
                CF.A = MM->Verts + base + a;
                CF.B = MM->Verts + base + b;
                CF.C = MM->Verts + base + c;
                CF.uvFromVertices();
                CF.Txtr = M;
                CF.ReflectionTexture = s_panoTex;
                CF.Flags = Face_Reflective;
                // Cull normal: empirically OUTWARD here (with inward
                // normals the far-side interior rendered — see-through
                // bowl). NormProd = -(N·A) as everywhere.
                CF.N = { out.x, out.y, out.z };
                CF.NormProd = -(CF.N.x * CF.A->Pos.x +
                                CF.N.y * CF.A->Pos.y +
                                CF.N.z * CF.A->Pos.z);
                ++fOfs;
            };
            if (r > 0)          emit(0, 1, 2);  // bottom band: p00==p01
            if (r < kRings - 1) emit(0, 2, 3);  // top band: p11==p10
        }
    }
    MM->VIndex = vOfs;
    MM->FIndex = fOfs;

    MM->BSphereCtr       = {0.0f, 0.0f, 0.0f};
    MM->BSphereRadius    = R;
    MM->BSphereRad       = R * R;
    MM->BSphereScreenPos = {0.0f, 0.0f, 0.0f};

    // Tri_Possessed: Animate_Objects must not touch this mesh — the
    // tick stamps IPos/RotMat directly (spin + bob). Tri_Noshading:
    // facets stay at the prelit full-bright above (a mirror has no
    // diffuse term of its own). The splines exist only for the
    // spline-extent "is dynamic" probes (mirror clone tracking +
    // static shadow bake): two Pos keys > 0.1 apart mark the ball
    // dynamic, so the teleporter mirror re-mirrors it every frame and
    // the static bake leaves it out.
    MM->Flags |= HTrack_Visible | Tri_Possessed | Tri_Noshading;
    auto stampKeys = [](Spline &sp, int n) {
        sp.NumKeys = DWord(n); sp.CurKey = 0; sp.Flags = 0;
        sp.Keys = (SplineKey*)std::calloc(size_t(n), sizeof(SplineKey));
        for (int i = 0; i < n; ++i) sp.Keys[i].Frame = float(i * 100);
    };
    stampKeys(MM->Pos, 2);
    MM->Pos.Keys[0].Pos.x = s_ballPos.x;        MM->Pos.Keys[0].Pos.y = s_ballPos.y;
    MM->Pos.Keys[0].Pos.z = s_ballPos.z;
    MM->Pos.Keys[1].Pos.x = s_ballPos.x + 0.2f; MM->Pos.Keys[1].Pos.y = s_ballPos.y;
    MM->Pos.Keys[1].Pos.z = s_ballPos.z;
    stampKeys(MM->Scale, 1);
    MM->Scale.Keys[0].Pos.x = 1.0f; MM->Scale.Keys[0].Pos.y = 1.0f;
    MM->Scale.Keys[0].Pos.z = 1.0f;
    stampKeys(MM->Rotate, 1);
    MM->Rotate.Keys[0].Pos.W = 1.0f;  // identity quaternion, W-first

    Compute_FaceVertexIndices(MM);

    Object *MObj = getAlignedType<Object>(16);
    std::memset(MObj, 0, sizeof(Object));
    MObj->Type = Obj_TriMesh;
    MObj->Data = MM;
    MObj->Pos  = &MM->IPos;
    MObj->Rot  = &MM->RotMat;
    MObj->Name = strdup("__discoBall");
    MObj->Next = sc->ObjectHead;
    if (sc->ObjectHead) sc->ObjectHead->Prev = MObj;
    sc->ObjectHead = MObj;
    MM->Next = sc->TriMeshHead;
    if (sc->TriMeshHead) sc->TriMeshHead->Prev = MM;
    sc->TriMeshHead = MM;
    s_ball = MM;

    // ── Rotating dot cones ──────────────────────────────────────────
    // Six narrow spots around the ball, alternating steep/shallow
    // downward tilt so the dots sweep both floor and walls.
    for (int i = 0; i < kSpotCount; ++i) {
        const float az = 2.0f * kPI * float(i) / kSpotCount;
        const float tilt = (i & 1) ? -0.65f : -0.25f;  // radians
        s_spotBase[i] = { std::cos(tilt) * std::cos(az),
                          std::sin(tilt),
                          std::cos(tilt) * std::sin(az) };
        s_spots[i] = MakeSpotLight(sc,
                                   1.0f, 0.95f, 0.85f,   // warm white
                                   2.2f,                  // intensity
                                   38.0f,                 // range
                                   s_ballPos, s_spotBase[i],
                                   1.2f, 3.5f,            // deg in/out
                                   0, /*castsShadow=*/false);
    }
    std::fprintf(stderr, "[DISCO] ball: %u verts %u faces, %d spots\n",
                 MM->VIndex, MM->FIndex, kSpotCount);
    return true;
}

namespace {

// One-shot: render the greets room from the ball's position into 6
// cube faces (forward path, same sequence as the mirror RTT pass),
// then resample to the equirect panorama the env filler consumes.
void BakePanorama(Scene *sc)
{
    static VESA_Surface surf = {};
    if (!surf.Data) {
        surf.X = kCubeRes; surf.Y = kCubeRes;
        surf.BPP = 32; surf.CPP = 4;
        surf.BPSL = kCubeRes * 4;
        surf.PageSize = kCubeRes * kCubeRes * 4;
        surf.Data = (byte*)std::malloc(size_t(kCubeRes) * kCubeRes * 4);
        surf.Z16  = (byte*)std::malloc(sizeof(word) * size_t(kCubeRes) * kCubeRes);
        if (!surf.Data || !surf.Z16) return;
        surf.Flip = MainSurf ? MainSurf->Flip : nullptr;
        Build_YOffs_Table(&surf);
    }
    static uint32_t *cube[6] = {};
    for (int f = 0; f < 6; ++f)
        if (!cube[f]) cube[f] = (uint32_t*)std::malloc(size_t(kCubeRes) * kCubeRes * 4);

    OffscreenViewScope view(sc, &surf);
    view.publishSurface();
    // The camera sits at the ball's center — hide the ball for its own
    // bake or every face would see the inside of the sphere.
    const DWord savedFlags = s_ball->Flags;
    s_ball->Flags &= ~HTrack_Visible;
    // Vertex colors for the forward render (clone-skip keeps it cheap;
    // the tick's own Lighting() hasn't run yet on the first frame).
    Lighting(sc);

    static Camera cam;
    view.setView(&cam);
    view.setNearZ(0.25f);
    const float kFov = float(kCubeRes) * 0.5f;  // 90° faces
    for (int f = 0; f < 6; ++f) {
        const CubeBasis &b = kCube[f];
        std::memset(&cam, 0, sizeof(cam));
        cam.ISource = s_ball->IPos;
        cam.Mat[0][0] = b.right.x; cam.Mat[0][1] = b.right.y; cam.Mat[0][2] = b.right.z;
        cam.Mat[1][0] = b.up.x;    cam.Mat[1][1] = b.up.y;    cam.Mat[1][2] = b.up.z;
        cam.Mat[2][0] = b.fwd.x;   cam.Mat[2][1] = b.fwd.y;   cam.Mat[2][2] = b.fwd.z;
        FOVX = kFov; FOVY = kFov;
        CntrEX = kFov; CntrEY = kFov;
        CntrX = int32_t(kFov); CntrY = int32_t(kFov);
        std::memset(surf.Data, 0, size_t(surf.PageSize));
        std::memset(surf.Z16, 0, sizeof(word) * size_t(kCubeRes) * kCubeRes);
        Transform_Objects(sc, fds::g_mainCamera, fds::g_mainFaces);
        if (CAll != 0) {
            Radix_Sort(FList, SList, CAll);
            Render(RenderPath::ForceForward);
        }
        std::memcpy(cube[f], surf.Data, size_t(kCubeRes) * kCubeRes * 4);
    }
    s_ball->Flags = savedFlags;

    // Equirect resample through the inverse of Transform.cpp's lookup.
    uint32_t *pano = (uint32_t*)s_panoTex->Data;
    for (int y = 0; y < kPanoRes; ++y) {
        const float ev = (float(y) + 0.5f) / kPanoRes;
        const float lat = kPI * (0.5f - ev);
        const float cl = std::cos(lat), sl = std::sin(lat);
        for (int x = 0; x < kPanoRes; ++x) {
            const float eu = (float(x) + 0.5f) / kPanoRes;
            const float lon = 2.0f * kPI * (eu - 0.5f) - 0.5f * kPI;
            const Vector d = { -cl * std::cos(lon), sl, -cl * std::sin(lon) };
            // Dominant axis → cube face.
            const float ax = std::fabs(d.x), ay = std::fabs(d.y), az = std::fabs(d.z);
            int f;
            if (ax >= ay && ax >= az)      f = (d.x > 0) ? 0 : 1;
            else if (ay >= az)             f = (d.y > 0) ? 2 : 3;
            else                           f = (d.z > 0) ? 4 : 5;
            const CubeBasis &b = kCube[f];
            const float dz = d.x*b.fwd.x   + d.y*b.fwd.y   + d.z*b.fwd.z;
            const float dr = d.x*b.right.x + d.y*b.right.y + d.z*b.right.z;
            const float du = d.x*b.up.x    + d.y*b.up.y    + d.z*b.up.z;
            int px = int(kFov * dr / dz + kFov);
            int py = int(-kFov * du / dz + kFov);
            px = std::min(std::max(px, 0), kCubeRes - 1);
            py = std::min(std::max(py, 0), kCubeRes - 1);
            pano[y * kPanoRes + x] = cube[f][py * kCubeRes + px] | 0xFF000000u;
        }
    }
    if (std::getenv("FDS_DISCO_DUMP")) {
        FILE *fp = std::fopen("disco_pano.ppm", "wb");
        if (fp) {
            std::fprintf(fp, "P6\n%d %d\n255\n", kPanoRes, kPanoRes);
            for (int i = 0; i < kPanoRes * kPanoRes; ++i) {
                const uint32_t c = pano[i];
                const byte rgb[3] = { byte(c >> 16), byte(c >> 8), byte(c) };
                std::fwrite(rgb, 1, 3, fp);
            }
            std::fclose(fp);
        }
    }
    // FDS_DISCO_SYNTH=1: replace the bake with direction-coded solid
    // quadrants (cuberefl's diagnostic): +x RED, -x BLUE, +z YELLOW,
    // -z GREEN, +y(down) MAGENTA, -y(up) WHITE. Clean solid facets →
    // sampling is healthy and any artifact is in the baked data.
    if (std::getenv("FDS_DISCO_SYNTH")) {
        for (int y = 0; y < kPanoRes; ++y) {
            const float ev = (float(y) + 0.5f) / kPanoRes;
            const float lat = kPI * (0.5f - ev);
            for (int x = 0; x < kPanoRes; ++x) {
                const float eu = (float(x) + 0.5f) / kPanoRes;
                const float lon = 2.0f * kPI * (eu - 0.5f) - 0.5f * kPI;
                const float dx = -std::cos(lat) * std::cos(lon);
                const float dy = std::sin(lat);
                const float dz = -std::cos(lat) * std::sin(lon);
                const float ax = std::fabs(dx), ay = std::fabs(dy), az = std::fabs(dz);
                uint32_t c;
                if (ay > ax && ay > az) c = dy > 0 ? 0x00FF00FFu : 0x00FFFFFFu;
                else if (ax > az)       c = dx > 0 ? 0x00FF0000u : 0x000000FFu;
                else                    c = dz > 0 ? 0x00FFFF00u : 0x0000FF00u;
                pano[y * kPanoRes + x] = c | 0xFF000000u;
            }
        }
    }
    Sachletz((dword*)s_panoTex->Data, kPanoRes, kPanoRes);
    std::fprintf(stderr, "[DISCO] panorama baked at (%.2f %.2f %.2f)\n",
                 s_ball->IPos.x, s_ball->IPos.y, s_ball->IPos.z);
}

}  // namespace

void UpdateDiscoBall(Scene *sc, float t)
{
    if (!s_ball) return;
    if (!s_panoBaked) {
        BakePanorama(sc);
        s_panoBaked = true;
    }
    const float a = t * kSpinRadPerTick;
    const float c = std::cos(a), s = std::sin(a);
    // v_world = RotMat × v_local (rows), Y-axis spin.
    s_ball->RotMat[0][0] = c;    s_ball->RotMat[0][1] = 0.0f; s_ball->RotMat[0][2] = s;
    s_ball->RotMat[1][0] = 0.0f; s_ball->RotMat[1][1] = 1.0f; s_ball->RotMat[1][2] = 0.0f;
    s_ball->RotMat[2][0] = -s;   s_ball->RotMat[2][1] = 0.0f; s_ball->RotMat[2][2] = c;
    Matrix_Copy(s_ball->UnscaledRotMat, s_ball->RotMat);
    s_ball->IPos = s_ballPos;
    s_ball->IPos.y += kBobAmp * std::sin(t * kBobRadPerTick);

    for (int i = 0; i < kSpotCount; ++i) {
        if (!s_spots[i]) continue;
        const Vector &b = s_spotBase[i];
        s_spots[i]->IPos = s_ball->IPos;
        s_spots[i]->IDir = { c * b.x + s * b.z, b.y, -s * b.x + c * b.z };
    }
}

}  // namespace fds
