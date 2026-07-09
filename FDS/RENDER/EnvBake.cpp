#include "EnvBake.h"
#include "EnvCube.h"
#include "OffscreenView.h"

#include <Base/FDS_VARS.H>   // MainSurf, View, FList/SList/CAll, CntrX, XRes...
#include <Base/FDS_DECS.H>   // Render, Radix_Sort, Transform_Objects, CalcPersp,
                             // VESA_Surface2Global, Sachletz, getAlignedType
#include <Base/FrameState.h> // g_mainCamera, g_mainFaces
#include <Base/Scene.h>
#include <Base/Camera.h>
#include <Base/FeatureFlags.h> // env_refl_res
#include <Base/Material.h>   // per-surface bakes: Reflection / MetallicMap / ID
#include <Base/Texture.h>
#include <Base/TriMesh.h>    // material centroids + scene AABB
#include <Base/Vector.h>
#include <FILLERS/Mekalele.h> // g_gbuffer->mirrorMask neutralization (bake)

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Build_YOffs_Table is commented out in FDS_DECS.H; declared extern wherever
// an offscreen surface is shaped (see DEMO/GreetsMirror.cpp).
extern void Build_YOffs_Table(VESA_Surface *VS);

namespace fds {

// Read by Transform_Objects (Transform.cpp): while true, meshes that fail the
// static-shadow-bake predicate (moving Pos/Rotate splines — the walking mech)
// are skipped, so the panorama doesn't freeze a moving object into the
// reflection at wherever it happened to stand on bake frame.
bool g_envBakeSkipDynamic = false;

// FACE-level self-exclusion state for the probe currently baking (empty
// outside a bake). The classic local-cubemap rule is "the reflector never
// appears in its own probe" — but the OLD implementation skipped every
// whole TriMesh containing ANY face of the baked material, and greets
// merges momy + the entire room into single ~9k-face meshes: baking momy's
// probe emptied the room out of it (near-black pano → "reflections have no
// effect", sibling momy instances black). Now Transform_Objects skips only
// the matching FACES (see EnvBake_FaceExcluded below).
//
// g_envBakeSkipMats: the baked material plus its same-surface clones
// ("momy" + "momy::mirUV") — a surface often renders through both.
// g_envBakeSkipR2 > 0: only exclude faces within that radius² of the bake
// point. Multi-instance surfaces (two mummies sharing one material) set
// this so the OTHER instance still renders in this instance's probe —
// pre-split behavior; the editor's split-instances gives exact per-instance
// materials and takes the R2 == 0 (exclude-whole-surface) path.
std::vector<const Material*> g_envBakeSkipMats;
float g_envBakeSkipR2 = 0.0f;
float g_envBakeSkipCX = 0.0f, g_envBakeSkipCY = 0.0f, g_envBakeSkipCZ = 0.0f;

// LEGACY (--no-env_bake_fix, the default): the old whole-mesh exclusion,
// kept byte-identical for the pinned city baseline (its vehicle-glass
// probes bake through this path). Null when the fix is active.
Material* g_envBakeLegacySkipMat = nullptr;

bool EnvBake_HasSkipFaces() { return !g_envBakeSkipMats.empty(); }

// Legacy whole-mesh exclusion test (Transform.cpp mesh loop). Exactly the
// pre-fix behavior: skip any mesh with >= 1 face of the baked material.
bool EnvBake_LegacyMeshExcluded(TriMesh* T)
{
    if (!g_envBakeLegacySkipMat) return false;
    for (DWord fi = 0; fi < T->FIndex; ++fi)
        if (T->Faces[fi].Txtr == g_envBakeLegacySkipMat) return true;
    return false;
}

// Called from Transform_Objects's face-submission loop during a bake render
// (hoisted bool guard — zero cost outside bakes). World position math only
// runs for faces of the baked surface itself.
bool EnvBake_FaceExcluded(const Face* F, TriMesh* T)
{
    const Material* M = F->Txtr;
    bool match = false;
    for (const Material* S : g_envBakeSkipMats)
        if (S == M) { match = true; break; }
    if (!match) return false;
    if (g_envBakeSkipR2 <= 0.0f || !F->A) return true;   // whole surface
    Vector w;
    MatrixXVector(T->RotMat, const_cast<Vector*>(&F->A->Pos), &w);
    Vector_SelfAdd(&w, &T->IPos);
    const float dx = w.x - g_envBakeSkipCX;
    const float dy = w.y - g_envBakeSkipCY;
    const float dz = w.z - g_envBakeSkipCZ;
    return dx*dx + dy*dy + dz*dz < g_envBakeSkipR2;
}

namespace {

// One cube face: an orthonormal (right, up, forward) basis. forward is the
// view direction; the engine reads Camera::Mat rows as [right; up; forward].
// Sampling uses the SAME basis it renders with, so the panorama is
// self-consistent regardless of the exact handedness chosen here.
struct CubeFace { Vector right, up, fwd; };
const CubeFace kCubeFaces[6] = {
    { { 0, 0,-1}, {0,1,0}, { 1, 0, 0} },  // +X
    { { 0, 0, 1}, {0,1,0}, {-1, 0, 0} },  // -X
    { { 1, 0, 0}, {0,0,-1},{ 0, 1, 0} },  // +Y
    { { 1, 0, 0}, {0,0,1}, { 0,-1, 0} },  // -Y
    { { 1, 0, 0}, {0,1,0}, { 0, 0, 1} },  // +Z
    { {-1, 0, 0}, {0,1,0}, { 0, 0,-1} },  // -Z
};

inline float dot(const Vector& a, const Vector& b) {
    return a.x*b.x + a.y*b.y + a.z*b.z;
}

inline int32_t iLog2(int32_t x) {
    union { uint32_t i; float f; } u{};
    u.f = float(x);
    return int32_t((u.i >> 23) - 127);
}

// Sample the six rendered cube faces in world direction d. Picks the
// dominant axis (the face whose forward maximises d·fwd) and projects
// through that face's 90°-FOV basis, matching the engine's screen mapping
// (screen y grows downward → the (1-b) flip below).
uint32_t sampleCube(const Vector& d, const std::vector<uint32_t> faces[6],
                    int res, uint32_t voidColor) {
    const float ax = std::fabs(d.x), ay = std::fabs(d.y), az = std::fabs(d.z);
    int f;
    if (ax >= ay && ax >= az)      f = d.x > 0 ? 0 : 1;
    else if (ay >= az)             f = d.y > 0 ? 2 : 3;
    else                           f = d.z > 0 ? 4 : 5;

    const CubeFace& cf = kCubeFaces[f];
    const float fwd = dot(d, cf.fwd);
    if (fwd <= 1e-6f) return voidColor;          // grazing / behind — shouldn't hit
    const float a = dot(d, cf.right) / fwd;      // [-1,1] across the face
    const float b = dot(d, cf.up)    / fwd;      // [-1,1] across the face
    int sx = int((a + 1.0f) * 0.5f * res);
    int sy = int((1.0f - b) * 0.5f * res);
    if (sx < 0) sx = 0; else if (sx >= res) sx = res - 1;
    if (sy < 0) sy = 0; else if (sy >= res) sy = res - 1;
    const uint32_t c = faces[f][size_t(sy) * size_t(res) + size_t(sx)];
    return (c & 0x00FFFFFFu) ? c : voidColor;    // empty (cleared) texel → void
}

}  // namespace

// Render the six axis-aligned cube faces from `center` at `fovDeg` full FOV
// into faces[6] (each res*res ARGB, row-major, screen-y down). fovDeg = 90
// gives the classic gutterless cube (equirect stitch); fovDeg =
// EnvCube_FaceFovDegrees() gives the padded overscan (env_cube). The (right,
// up, fwd) basis is EnvCube's kCubeFaces convention verbatim, so the same
// image both bakes sample. FDS_ENVCUBE_PAINT replaces the scene render with
// EnvCube_PaintDebugFace so orientation can be validated without geometry.
// Returns false on alloc failure.
// publishProj: republish the per-face projection into the engine globals
// (FOVX/FOVY from s_cam.PerspX/Y, AspectRatio ≈ 1 for square-pixel faces —
// the exact dance CITY.CPP's validated per-building bake does at :2074/:2188).
// Nothing else recomputes those during a bake — Animate_Objects only sets
// them for the MAIN camera — so without this every face rendered with the
// main window's perspective scale on a res-px surface: a few-degree
// TELEPHOTO sliver instead of a 90°+ cube face (auto-baked probes were
// mostly void with one magnified fragment; the editor's "reflection has no
// effect" / "wrong panorama"). Opt-in because the mirrortest gate's
// BakeEquirectPanorama baseline was captured with the legacy behavior.
static bool renderSixFaces(Scene* sc, const Vector& center, int res,
                           float fovDeg, uint32_t voidColor,
                           std::vector<uint32_t> faces[6], Material* skipMat,
                           float skipRadius = 0.0f, bool publishProj = false) {
    if (!sc) return false;
    static const bool sPaint = std::getenv("FDS_ENVCUBE_PAINT") != nullptr;

    // Face-level self-exclusion set: the baked material plus every clone of
    // the same SURFACE ("momy::mirUV" — a surface's mirrored-UV half renders
    // through its clone). Editor "#k" split clones are DIFFERENT instances
    // and keep their own names, so they stay in the capture.
    auto baseName = [](const char* n) {
        std::string s = n ? n : "";
        static const char suf[] = "::mirUV";
        const size_t sl = sizeof(suf) - 1;
        if (s.size() > sl && s.compare(s.size() - sl, sl, suf) == 0)
            s.resize(s.size() - sl);
        return s;
    };
    g_envBakeSkipMats.clear();
    g_envBakeLegacySkipMat = nullptr;
    if (skipMat && !publishProj) {
        // Legacy mode (--no-env_bake_fix): whole-mesh exclusion, unchanged.
        g_envBakeLegacySkipMat = skipMat;
    } else if (skipMat) {
        const std::string want = baseName(skipMat->Name);
        for (Material* m = MatLib; m; m = m->Next)
            if (m->RelScene == sc && m->Name && baseName(m->Name) == want)
                g_envBakeSkipMats.push_back(m);
        if (g_envBakeSkipMats.empty()) g_envBakeSkipMats.push_back(skipMat);
    }
    g_envBakeSkipR2 = skipRadius > 0.0f ? skipRadius * skipRadius : 0.0f;
    g_envBakeSkipCX = center.x; g_envBakeSkipCY = center.y; g_envBakeSkipCZ = center.z;

    // Offscreen render target for one cube face. Owns its own Data/Z16; the
    // OffscreenViewScope below points MainSurf here and republishes the
    // engine globals (XRes/VPage/ZPage16/CntrE*) to match.
    VESA_Surface surf = {};
    surf.X = res; surf.Y = res;
    surf.BPP = 32; surf.CPP = 4;
    surf.BPSL = res * 4;
    surf.PageSize = res * res * 4;
    surf.Data = (byte*)std::malloc(size_t(res) * res * 4);
    surf.Z16  = (byte*)std::malloc(size_t(res) * res * sizeof(word));
    surf.Flip = MainSurf ? MainSurf->Flip : nullptr;
    if (!surf.Data || !surf.Z16) { std::free(surf.Data); std::free(surf.Z16); return false; }
    Build_YOffs_Table(&surf);

    for (int i = 0; i < 6; ++i) faces[i].resize(size_t(res) * res);

    // Neutralize the planar-mirror pixel masks for the bake (greets
    // teleporter): StampMirrorMasks stamps gb.mirrorMask for the MAIN
    // camera's view, and Mekalele's behind-mirror lane cull would slice the
    // probe's room along that stale window footprint — every pano rendered
    // with the mirror active came out as fixed torn patches (most of the
    // room missing). Mask 0 = no cull for real faces, and mirror CLONE
    // faces (mirrorTag != 0 wants mask == tag) are fully rejected — clone
    // geometry must not appear in probes anyway. Restored after the loop.
    std::vector<uint8_t> savedMirrorMask;
    if (publishProj && g_gbuffer && !g_gbuffer->mirrorMask.empty()) {
        savedMirrorMask.assign(g_gbuffer->mirrorMask.begin(),
                               g_gbuffer->mirrorMask.end());
        std::fill(g_gbuffer->mirrorMask.begin(),
                  g_gbuffer->mirrorMask.end(), uint8_t(0));
    }

    {
        OffscreenViewScope view(sc, &surf);
        view.publishSurface();               // XRes/CntrX/... from surf
        static Camera s_cam;
        view.setView(&s_cam);
        // Square-pixel projection for the cube faces (CITY.CPP:2074 uses
        // the same 0.999). The scope's destructor restores AspectRatio.
        if (publishProj) AspectRatio = 0.999f;

        g_envBakeSkipDynamic = true;   // moving meshes stay out of the pano
        // (the reflector's own faces stay out too — g_envBakeSkipMats above)
        for (int i = 0; i < 6; ++i) {
            if (sPaint) {   // orientation self-check: painted debug faces
                fds::EnvCube_PaintDebugFace(i, faces[i].data(), res);
                continue;
            }
            std::memset(&s_cam, 0, sizeof(s_cam));
            s_cam.ISource = center;
            s_cam.IFOV = fovDeg;
            const CubeFace& cf = kCubeFaces[i];
            s_cam.Mat[0][0] = cf.right.x; s_cam.Mat[0][1] = cf.right.y; s_cam.Mat[0][2] = cf.right.z;
            s_cam.Mat[1][0] = cf.up.x;    s_cam.Mat[1][1] = cf.up.y;    s_cam.Mat[1][2] = cf.up.z;
            s_cam.Mat[2][0] = cf.fwd.x;   s_cam.Mat[2][1] = cf.fwd.y;   s_cam.Mat[2][2] = cf.fwd.z;
            CalcPersp(&s_cam);
            if (publishProj) {
                FOVX = s_cam.PerspX;   // Transform/clipper/kernel read the
                FOVY = s_cam.PerspY;   // globals; scope exit restores them
            }

            // Clear to void color (each byte of voidColor) + empty Z.
            std::memset(surf.Data, voidColor & 0xFF, size_t(res) * res * 4);
            std::memset(surf.Z16, 0, size_t(res) * res * sizeof(word));

            Transform_Objects(sc, fds::g_mainCamera, fds::g_mainFaces);
            if (CAll != 0) {
                Radix_Sort(FList, SList, CAll);
                // Full deferred pipeline for the bake: the pano should show
                // what the viewer sees (per-pixel materials, shadows, PBR
                // maps), not the Gouraud forward approximation — reflections
                // of the room looked flat-shaded next to the room itself.
                // One-time cost per surface, cube faces are small.
                // skipVolumetric: no SSAO/fog/cones in the pano, and (key)
                // no HDR activation — this is an LDR underlay; the kernel's
                // g_hdrBuf writes are already size-gated off (bake res !=
                // main res). Recursive FramePrep is impossible here: the
                // renderFrame entry trigger requires g_offscreenViewDepth==0
                // and we're inside an OffscreenViewScope.
                Render(RenderPath::ForceDeferred, /*skipVolumetric=*/true);
            }
            if (std::getenv("FDS_ENVBAKE_DUMP")) {
                // Coverage census: how much of the face the raster actually
                // touched (Z != 0) — distinguishes "faces culled upstream"
                // from "rasterized but dark".
                size_t zHit = 0;
                const word* zp = reinterpret_cast<const word*>(surf.Z16);
                for (size_t k = 0; k < size_t(res) * res; ++k)
                    if (zp[k]) ++zHit;
                std::fprintf(stderr, "[ENVBAKE] face %d: CAll=%d zCov=%.0f%% "
                    "NZP=%.2f FZP=%.2f FOVX=%.1f XRes=%d fwd=(%.0f,%.0f,%.0f)\n",
                    i, int(CAll), 100.0 * double(zHit) / (double(res) * res),
                    sc->NZP, sc->FZP, (double)FOVX, (int)XRes,
                    kCubeFaces[i].fwd.x, kCubeFaces[i].fwd.y, kCubeFaces[i].fwd.z);
            }
            std::memcpy(faces[i].data(), surf.Data, size_t(res) * res * 4);
        }
        g_envBakeSkipDynamic = false;
        g_envBakeSkipMats.clear();
        g_envBakeLegacySkipMat = nullptr;
        g_envBakeSkipR2 = 0.0f;
    }   // scope exit restores MainSurf/View/FOV/clip planes

    if (!savedMirrorMask.empty() && g_gbuffer
        && g_gbuffer->mirrorMask.size() == savedMirrorMask.size())
        std::copy(savedMirrorMask.begin(), savedMirrorMask.end(),
                  g_gbuffer->mirrorMask.begin());

    std::free(surf.Data);
    std::free(surf.Z16);
    return true;
}

// Shared core: render the six cube faces from `center` and stitch them into
// a LINEAR equirect panorama in `pano`. Returns false on alloc failure.
static bool renderCubeAndStitch(Scene* sc, const Vector& center,
                                const EnvBakeParams& params,
                                std::vector<uint32_t>& pano,
                                Material* skipMat = nullptr,
                                float skipRadius = 0.0f,
                                bool publishProj = false) {
    if (!sc) return false;
    const int res = params.cubeRes;
    const int W = params.panoWidth, H = params.panoHeight;

    std::vector<uint32_t> faces[6];
    if (!renderSixFaces(sc, center, res, 90.0f, params.voidColor, faces, skipMat,
                        skipRadius, publishProj))
        return false;

    // Stitch to equirectangular. The (eu,ev)→direction mapping is the exact
    // inverse of the reflective lookup in RENDER/Transform.cpp, so a face
    // reflecting world-direction d samples the scene content baked for d:
    //   lat = PI*(0.5 - ev),  lon = 2*PI*(eu - 0.5) - PI/2
    //   d = ( -cos(lon)cos(lat), sin(lat), -sin(lon)cos(lat) )
    pano.assign(size_t(W) * H, params.voidColor);
    for (int py = 0; py < H; ++py) {
        const float ev  = (float(py) + 0.5f) / float(H);
        const float lat = float(M_PI) * (0.5f - ev);
        const float cl  = std::cos(lat), sl = std::sin(lat);
        for (int px = 0; px < W; ++px) {
            const float eu  = (float(px) + 0.5f) / float(W);
            const float lon = 2.0f * float(M_PI) * (eu - 0.5f) - float(M_PI) * 0.5f;
            Vector d;
            d.x = -std::cos(lon) * cl;
            d.y = sl;
            d.z = -std::sin(lon) * cl;
            pano[size_t(py) * W + px] = sampleCube(d, faces, res, params.voidColor);
        }
    }

    // FDS_ENVBAKE_DUMP=1: write the stitched panorama (pre-Sachletz) to
    // /tmp for inspection.
    if (std::getenv("FDS_ENVBAKE_DUMP")) {
        if (FILE *f = std::fopen("/tmp/envbake_pano.ppm", "wb")) {
            std::fprintf(f, "P6\n%d %d\n255\n", W, H);
            for (int i = 0; i < W * H; ++i) {
                const uint32_t p = pano[size_t(i)];
                unsigned char rgb[3] = {
                    (unsigned char)((p >> 16) & 0xFF),
                    (unsigned char)((p >>  8) & 0xFF),
                    (unsigned char)((p      ) & 0xFF) };
                std::fwrite(rgb, 1, 3, f);
            }
            std::fclose(f);
        }
    }
    return true;
}

Texture* BakeEquirectPanorama(Scene* sc, const Vector& center,
                              const EnvBakeParams& params) {
    std::vector<uint32_t> pano;
    if (!renderCubeAndStitch(sc, center, params, pano)) return nullptr;
    const int W = params.panoWidth, H = params.panoHeight;

    // Materialize into a Sachletz-tiled Texture the forward env filler reads.
    Texture* T = getAlignedType<Texture>(16);
    T->Flags = Txtr_Nomip | Txtr_Tiled;
    T->Data  = (byte*)_aligned_malloc(size_t(W) * H * 4, 16);
    std::memcpy(T->Data, pano.data(), size_t(W) * H * 4);
    T->BPP = 32;
    T->SizeX = W; T->LSizeX = iLog2(W);
    T->SizeY = H; T->LSizeY = iLog2(H);
    Sachletz((dword*)T->Data, W, H);
    T->Mipmap[0] = T->Data;
    T->numMipmaps = 1;
    return T;
}

// ── Deferred env-specular registry (per reflective surface) ───────────────
namespace {
struct EnvPanoStore {
    std::vector<uint32_t> levels[EnvPanoLinear::kMaxMips];
    EnvPanoLinear view;
};
struct SceneEnv {
    // Owning stores + Material* → store index (materials with near-identical
    // centroids alias one store: ::mirUV clones, split instances).
    std::vector<std::unique_ptr<EnvPanoStore>> stores;
    std::map<const Material*, int> byMat;
    // matID-indexed view table the kernel reads (rebuilt each FramePrep —
    // matIDs shift when the editor rebuilds the mat table).
    const EnvPanoLinear* table[256] = {};
    // Scene AABB proxy (world), computed once.
    float boxMin[3] = {}, boxMax[3] = {};
    bool  boxValid = false;
};
std::map<Scene*, SceneEnv> g_envByScene;
bool g_envBakeInProgress = false;   // bake renders through Render() → guard

// --env-bake-res: explicit FACE resolution for the deferred env-reflection
// stores (bakeStore probes AND RegisterCubeFaces imports). Returns 0 when the
// user did NOT set it (CLI/env) — callers then keep their legacy sizing
// (env_refl_res/2 for probes, the caller's storeRes for imports), which keeps
// the pinned baselines byte-identical. Sanitized: clamped to [64,1024] and
// rounded DOWN to a power of two — the 4-level mip chain and the samplers'
// shift-indexed dims (envP->W >> lvl) need cleanly halvable sizes. The value
// is read at BAKE time only; every consumer reads dims from the store's own
// recorded W/H/numMips, so mixed-res stores coexist safely.
int envBakeResOverride() {
    if (!FeatureFlags::isSet(FeatureFlags::IntId::env_bake_res)) return 0;
    const int want = FeatureFlags::env_bake_res();
    const int clamped = want < 64 ? 64 : (want > 1024 ? 1024 : want);
    int p2 = 64;
    while (p2 * 2 <= clamped) p2 <<= 1;
    static int noted = INT_MIN;
    if (p2 != want && noted != want) {
        noted = want;
        std::fprintf(stderr, "[ENVREFL] env_bake_res=%d invalid (want a power"
                     " of two in 64..1024) — using %d\n", want, p2);
    }
    return p2;
}

// 2×2 box downsample (per-channel average), ARGB.
void boxDownsample(const std::vector<uint32_t>& src, int sw, int sh,
                   std::vector<uint32_t>& dst) {
    const int dw = sw >> 1, dh = sh >> 1;
    dst.resize(size_t(dw) * dh);
    for (int y = 0; y < dh; ++y)
        for (int x = 0; x < dw; ++x) {
            const uint32_t a = src[size_t(2*y)   * sw + 2*x];
            const uint32_t b = src[size_t(2*y)   * sw + 2*x + 1];
            const uint32_t c = src[size_t(2*y+1) * sw + 2*x];
            const uint32_t d = src[size_t(2*y+1) * sw + 2*x + 1];
            uint32_t out = 0;
            for (int sh8 = 0; sh8 < 32; sh8 += 8) {
                const uint32_t s = ((a >> sh8) & 0xFF) + ((b >> sh8) & 0xFF)
                                 + ((c >> sh8) & 0xFF) + ((d >> sh8) & 0xFF);
                out |= ((s + 2) >> 2) << sh8;
            }
            dst[size_t(y) * dw + x] = out;
        }
}

// Per-face 2×2 box downsample of a FACE-MAJOR cube buffer (6 faces of fr×fr →
// 6 faces of (fr/2)²). Faces are downsampled INDEPENDENTLY — the padding ring
// (D2) means the 2×2 box at a face edge already averages valid neighbour
// content, so we never cross a face boundary in the filter.
void boxDownsampleCube(const std::vector<uint32_t>& src, int fr,
                       std::vector<uint32_t>& dst) {
    const int dfr = fr >> 1;
    dst.assign(size_t(6) * dfr * dfr, 0);
    for (int f = 0; f < 6; ++f) {
        const uint32_t* sp = src.data() + size_t(f) * fr * fr;
        uint32_t* dp = dst.data() + size_t(f) * dfr * dfr;
        for (int y = 0; y < dfr; ++y)
            for (int x = 0; x < dfr; ++x) {
                const uint32_t a = sp[size_t(2*y)   * fr + 2*x];
                const uint32_t b = sp[size_t(2*y)   * fr + 2*x + 1];
                const uint32_t c = sp[size_t(2*y+1) * fr + 2*x];
                const uint32_t d = sp[size_t(2*y+1) * fr + 2*x + 1];
                uint32_t out = 0;
                for (int sh8 = 0; sh8 < 32; sh8 += 8) {
                    const uint32_t s = ((a >> sh8) & 0xFF) + ((b >> sh8) & 0xFF)
                                     + ((c >> sh8) & 0xFF) + ((d >> sh8) & 0xFF);
                    out |= ((s + 2) >> 2) << sh8;
                }
                dp[size_t(y) * dfr + x] = out;
            }
    }
}

// Render six PADDED faces (env_cube) and pack them FACE-MAJOR into level0
// (6 blocks of fr×fr). No equirect stitch — the sharp faces go straight to
// the kernel. faceRes := params.cubeRes. FDS_ENVBAKE_DUMP writes a 3×2 grid.
bool renderCubeFacesMajor(Scene* sc, const Vector& center,
                          const EnvBakeParams& params,
                          std::vector<uint32_t>& level0, int& faceRes,
                          Material* skipMat, float skipRadius) {
    const int fr = params.cubeRes;
    std::vector<uint32_t> faces[6];
    // publishProj (and with it the whole --env_bake_fix bundle inside
    // renderSixFaces: per-face projection, FACE-level self-exclusion,
    // mirror-mask neutralization) must be FLAG-GATED here exactly like the
    // equirect path in bakeStore — an unconditional `true` leaked the fixed
    // bake into the DEFAULT cube path (env_cube defaults ON) and broke the
    // pinned city baseline (its vehicle-glass/window probes auto-bake
    // through here with legacy projection).
    if (!renderSixFaces(sc, center, fr, fds::EnvCube_FaceFovDegrees(),
                        params.voidColor, faces, skipMat, skipRadius,
                        /*publishProj=*/fds::FeatureFlags::env_bake_fix()))
        return false;
    level0.resize(size_t(6) * fr * fr);
    for (int f = 0; f < 6; ++f)
        std::memcpy(level0.data() + size_t(f) * fr * fr, faces[f].data(),
                    size_t(fr) * fr * 4);
    faceRes = fr;
    if (std::getenv("FDS_ENVBAKE_DUMP")) {
        const int gw = fr * 3, gh = fr * 2;
        if (FILE* fp = std::fopen("/tmp/envbake_cube.ppm", "wb")) {
            std::fprintf(fp, "P6\n%d %d\n255\n", gw, gh);
            for (int y = 0; y < gh; ++y)
                for (int x = 0; x < gw; ++x) {
                    const int f = (y / fr) * 3 + (x / fr);
                    const uint32_t p = faces[f][size_t(y % fr) * fr + (x % fr)];
                    unsigned char rgb[3] = { (unsigned char)((p >> 16) & 0xFF),
                                             (unsigned char)((p >> 8) & 0xFF),
                                             (unsigned char)(p & 0xFF) };
                    std::fwrite(rgb, 1, 3, fp);
                }
            std::fclose(fp);
        }
    }
    return true;
}

// World-space centroid of every face using material M. False if none found
// (material exists but no faces reference it — nothing to reflect anyway).
// excludeRadius (out, optional): how far around the probe the bake's
// face-level self-exclusion should reach. 0 = unbounded (exclude every face
// of the surface — the single-instance case). For MULTI-instance surfaces
// (spatially separate clusters), a finite radius so the sibling instances
// still render in this instance's probe: fragments within 2×kR of the probe
// count as the probed instance; the nearest cluster beyond that is a
// sibling, and the exclusion stops just under halfway to it (0.45×). A
// heuristic — the editor's split-instances gives exact per-instance
// materials and doesn't rely on it.
bool materialCentroid(Scene* sc, const Material* M, Vector& out,
                      float* excludeRadius = nullptr) {
    if (excludeRadius) *excludeRadius = 0.0f;
    double sx = 0, sy = 0, sz = 0;
    long n = 0;
    for (TriMesh* T = sc->TriMeshHead; T; T = T->Next)
        for (DWord i = 0; i < T->FIndex; ++i) {
            const Face& F = T->Faces[i];
            if (F.Txtr != M) continue;
            const Vertex* vs[3] = { F.A, F.B, F.C };
            for (int k = 0; k < 3; ++k) {
                if (!vs[k]) continue;
                Vector w;
                MatrixXVector(T->RotMat, const_cast<Vector*>(&vs[k]->Pos), &w);
                Vector_SelfAdd(&w, &T->IPos);
                sx += w.x; sy += w.y; sz += w.z;
                ++n;
            }
        }
    if (!n) return false;
    out = { float(sx / n), float(sy / n), float(sz / n) };
    // Multi-instance surfaces (the two greets mummies share one material):
    // the global mean sits in the EMPTY SPACE BETWEEN instances — a probe
    // from nowhere. Greedy-cluster the vertices by distance to the running
    // cluster mean and re-centroid on the HEAVIEST cluster, so the probe is
    // at least correct for one instance (per-instance correctness needs the
    // editor's "split instances", which gives each its own material+probe).
    {
        struct Cl { double sx, sy, sz; long n; };
        std::vector<Cl> cls;
        const float kR2 = 8.0f * 8.0f;
        for (TriMesh* T = sc->TriMeshHead; T; T = T->Next)
            for (DWord i = 0; i < T->FIndex; ++i) {
                const Face& F = T->Faces[i];
                if (F.Txtr != M || !F.A) continue;
                Vector w;
                MatrixXVector(T->RotMat, const_cast<Vector*>(&F.A->Pos), &w);
                Vector_SelfAdd(&w, &T->IPos);
                Cl* best = nullptr;
                for (Cl& c : cls) {
                    const float dx = float(c.sx / c.n) - w.x, dy = float(c.sy / c.n) - w.y,
                                dz = float(c.sz / c.n) - w.z;
                    if (dx*dx + dy*dy + dz*dz < kR2) { best = &c; break; }
                }
                if (!best) { cls.push_back({0, 0, 0, 0}); best = &cls.back(); }
                best->sx += w.x; best->sy += w.y; best->sz += w.z; ++best->n;
            }
        if (cls.size() > 1) {
            const Cl* heavy = &cls[0];
            for (const Cl& c : cls) if (c.n > heavy->n) heavy = &c;
            out = { float(heavy->sx / heavy->n), float(heavy->sy / heavy->n),
                    float(heavy->sz / heavy->n) };
            if (excludeRadius) {
                // Nearest cluster mean beyond 2×kR of the probe = the nearest
                // SIBLING instance (nearer clusters are fragments of the
                // probed instance — the greedy clustering splinters a single
                // statue into several). Exclude out to just under halfway.
                const float kGroup2 = 16.0f * 16.0f;   // (2×kR)²
                float nearFar2 = 1e30f;
                for (const Cl& c : cls) {
                    const float dx = float(c.sx / c.n) - out.x;
                    const float dy = float(c.sy / c.n) - out.y;
                    const float dz = float(c.sz / c.n) - out.z;
                    const float d2 = dx*dx + dy*dy + dz*dz;
                    if (d2 > kGroup2 && d2 < nearFar2) nearFar2 = d2;
                }
                if (nearFar2 < 1e30f)
                    *excludeRadius = 0.45f * std::sqrt(nearFar2);
            }
            std::fprintf(stderr, "[ENVREFL] '%s': %zu instance clusters — probe at the"
                " largest (%.1f %.1f %.1f), self-exclusion radius %.1f (0 = whole"
                " surface); use split-instances for per-instance probes\n",
                M->Name ? M->Name : "?", cls.size(), out.x, out.y, out.z,
                excludeRadius ? *excludeRadius : 0.0f);
        }
    }
    return true;
}

void sceneAABB(Scene* sc, SceneEnv& env) {
    if (env.boxValid) return;
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (TriMesh* T = sc->TriMeshHead; T; T = T->Next)
        for (DWord v = 0; v < T->VIndex; ++v) {
            Vector w;
            MatrixXVector(T->RotMat, &T->Verts[v].Pos, &w);
            Vector_SelfAdd(&w, &T->IPos);
            const float p[3] = { w.x, w.y, w.z };
            for (int a = 0; a < 3; ++a) {
                if (p[a] < lo[a]) lo[a] = p[a];
                if (p[a] > hi[a]) hi[a] = p[a];
            }
        }
    for (int a = 0; a < 3; ++a) { env.boxMin[a] = lo[a]; env.boxMax[a] = hi[a]; }
    env.boxValid = true;
}

// Bake one panorama from `center` into a fresh store (mip chain + metadata).
std::unique_ptr<EnvPanoStore> bakeStore(Scene* sc, const SceneEnv& env,
                                        const Vector& center,
                                        Material* skipMat = nullptr,
                                        float skipRadius = 0.0f) {
    EnvBakeParams params;
    // Bake sizing: --env-bake-res (explicit FACE res, pow2 64..1024) wins;
    // else the legacy --env_refl_res sizing (default 512): reflections are
    // roughness-blurred by eye anyway, so half the CITY bake res reads fine.
    // Clamp: the mip chain needs res ≥ 64; cap at 1024.
    if (const int face = envBakeResOverride()) {
        params.cubeRes = face;                    // cube stores: W=H=face res
        params.panoWidth  = face * 2;             // equirect stores keep the
        params.panoHeight = face * 2;             // legacy 2×face pano size
    } else {
        int res = fds::FeatureFlags::env_refl_res();
        if (res < 64) res = 64;
        if (res > 1024) res = 1024;
        params.cubeRes = res / 2;
        params.panoWidth = res;
        params.panoHeight = res;
    }
    const bool useCube = fds::FeatureFlags::env_cube();
    // --env_bake_fix: the whole corrected-bake bundle (per-face projection
    // publish, face-level self-exclusion, metal neutralization, mirror-mask
    // neutralization). OFF by default so the pinned city baseline (vehicle
    // glass probes bake through here) stays byte-identical; the editor and
    // the metallic/sidecar import paths turn it on.
    const bool fix = fds::FeatureFlags::env_bake_fix();
    auto store = std::make_unique<EnvPanoStore>();
    EnvPanoLinear& v = store->view;
    g_envBakeInProgress = true;
    // HACK (accepted, documented): a metal surface whose OWN store hasn't
    // been baked/tabled yet renders BLACK inside another surface's probe —
    // metalness kills its diffuse and its env-specular term needs a pano the
    // kernel can't have during the first bake round (the two momy statues
    // reflecting each other as black holes). Neutralize the metalness of
    // exactly those materials for the duration of THIS bake so they show
    // their lit albedo instead. Bake-scoped mutation, restored before
    // return; materials already in the kernel table keep their metal look
    // (their reflections ride their own store — 1-bounce inter-reflection).
    std::vector<std::pair<Material*, Texture*>> metalOff;
    for (Material* m = fix ? MatLib : nullptr; m; m = m->Next) {
        if (m->RelScene != sc || !m->MetallicMap) continue;
        if (m->ID < 256 && env.table[m->ID]) continue;   // has a live store
        metalOff.emplace_back(m, m->MetallicMap);
        m->MetallicMap = nullptr;
    }
    auto restoreMetal = [&]() {
        for (auto& mt : metalOff) mt.first->MetallicMap = mt.second;
    };
    if (useCube) {
        // Padded 6-face cube (D2): no stitch. mip[k] is a face-major block of
        // six (faceRes>>k)² faces; W==H==faceRes; per-face box downsample.
        int faceRes = 0;
        const bool ok = renderCubeFacesMajor(sc, center, params,
                                             store->levels[0], faceRes, skipMat,
                                             skipRadius);
        restoreMetal();
        g_envBakeInProgress = false;
        if (!ok) return nullptr;
        v.isCube = true;
        v.W = v.H = faceRes;
        v.numMips = EnvPanoLinear::kMaxMips;
        int fr = faceRes;
        for (int k = 1; k < EnvPanoLinear::kMaxMips; ++k) {
            boxDownsampleCube(store->levels[k-1], fr, store->levels[k]);
            fr >>= 1;
        }
        for (int k = 0; k < EnvPanoLinear::kMaxMips; ++k)
            v.mip[k] = store->levels[k].data();
        v.bakeX = center.x; v.bakeY = center.y; v.bakeZ = center.z;
        v.boxMinX = env.boxMin[0]; v.boxMinY = env.boxMin[1]; v.boxMinZ = env.boxMin[2];
        v.boxMaxX = env.boxMax[0]; v.boxMaxY = env.boxMax[1]; v.boxMaxZ = env.boxMax[2];
        return store;
    }
    const bool ok = renderCubeAndStitch(sc, center, params, store->levels[0],
                                        skipMat, skipRadius,
                                        /*publishProj=*/fix);
    restoreMetal();
    g_envBakeInProgress = false;
    if (!ok) return nullptr;
    v.W = params.panoWidth;
    v.H = params.panoHeight;
    v.numMips = EnvPanoLinear::kMaxMips;
    int w = v.W, h = v.H;
    for (int k = 1; k < EnvPanoLinear::kMaxMips; ++k) {
        boxDownsample(store->levels[k-1], w, h, store->levels[k]);
        w >>= 1; h >>= 1;
    }
    for (int k = 0; k < EnvPanoLinear::kMaxMips; ++k)
        v.mip[k] = store->levels[k].data();
    v.bakeX = center.x; v.bakeY = center.y; v.bakeZ = center.z;
    v.boxMinX = env.boxMin[0]; v.boxMinY = env.boxMin[1]; v.boxMinZ = env.boxMin[2];
    v.boxMaxX = env.boxMax[0]; v.boxMaxY = env.boxMax[1]; v.boxMaxZ = env.boxMax[2];
    return store;
}
}   // namespace

// FDS_ENV_GRID=1 — measurement content: replace a baked cube store with a
// synthetic pattern so reflection MOTION is objectively trackable:
//   • per-face tint      → identifies which cube face a glass region samples
//   • 8-texel checker    → strong texture for cross-correlation flow tracking
//     (the city art is too low-contrast/aliased for reliable block matching)
//   • 2-texel white face border → face-seam lines show directly in the glass
// Applied to every level (the checker coarsens per mip, which is fine — the
// city glass samples mip 0-1).
static void fillEnvDebugGrid(EnvPanoStore& S) {
    if (!S.view.isCube) return;
    static const uint32_t tint[6] = {
        0xFFD05050, 0xFF50D050, 0xFF5050D0,     // +X red, -X green, +Y blue
        0xFFD0D050, 0xFFD050D0, 0xFF50D0D0 };   // -Y yellow, +Z magenta, -Z cyan
    for (int l = 0; l < S.view.numMips; ++l) {
        const int fr = S.view.W >> l;
        for (int f = 0; f < 6; ++f) {
            uint32_t* base = S.levels[l].data() + size_t(f) * fr * fr;
            for (int y = 0; y < fr; ++y)
                for (int x = 0; x < fr; ++x) {
                    const bool border = x < 2 || y < 2 || x >= fr-2 || y >= fr-2;
                    uint32_t c;
                    if (border) c = 0xFFFFFFFF;
                    else {
                        // APERIODIC random 4-texel cells (face-tinted): a
                        // periodic checker made block-matching measure flow
                        // MODULO the cell period — sawtooth artifacts of the
                        // instrument itself, not the renderer (same trap as
                        // the earlier 64px phase-correlation detector).
                        uint32_t h = (uint32_t(x >> 2) * 0x8DA6B343u)
                                   ^ (uint32_t(y >> 2) * 0xD8163841u)
                                   ^ (uint32_t(f) * 0xCB1AB31Fu);
                        h ^= h >> 13; h *= 0x2C1B3C6Du; h ^= h >> 15;
                        const uint32_t lum = 0x30 + (h & 0x9F);
                        const uint32_t tb = (tint[f]      ) & 0xFF;
                        const uint32_t tg = (tint[f] >>  8) & 0xFF;
                        const uint32_t tr = (tint[f] >> 16) & 0xFF;
                        c = 0xFF000000u
                          | (((tr * lum) >> 8) << 16)
                          | (((tg * lum) >> 8) <<  8)
                          |  ((tb * lum) >> 8);
                    }
                    base[size_t(y) * fr + x] = c;
                }
        }
    }
}

bool EnvReflection_FramePrep(Scene* sc) {
    if (!sc || g_envBakeInProgress) return false;
    SceneEnv& env = g_envByScene[sc];
    bool bakedAny = false;
    // Bake for every reflective material that lacks one; centroids within a
    // few world units share a store (clone materials, adjacent panels).
    for (Material* M = MatLib; M; M = M->Next) {
        if (M->RelScene != sc) continue;
        if (!(M->Reflection > 0.0f || M->MetallicMap)) continue;
        if (env.byMat.count(M)) continue;
        Vector c;
        float excludeR = 0.0f;
        if (!materialCentroid(sc, M, c, &excludeR)) { env.byMat[M] = -1; continue; }
        if (std::getenv("ENVDBG"))
            std::fprintf(stderr, "[ENVDBG] mat '%s' id=%u refl=%.0f metal=%d centroid (%.1f %.1f %.1f)\n",
                         M->Name ? M->Name : "?", (unsigned)M->ID, M->Reflection, M->MetallicMap ? 1 : 0,
                         c.x, c.y, c.z);
        sceneAABB(sc, env);
        int idx = -1;
        for (size_t i = 0; i < env.stores.size(); ++i) {
            const EnvPanoLinear& v = env.stores[i]->view;
            const float dx = v.bakeX - c.x, dy = v.bakeY - c.y, dz = v.bakeZ - c.z;
            if (dx*dx + dy*dy + dz*dz < 4.0f * 4.0f) { idx = int(i); break; }
        }
        if (idx < 0) {
            auto store = bakeStore(sc, env, c, M, excludeR);
            if (!store) { env.byMat[M] = -1; continue; }
            static const bool sGrid = std::getenv("FDS_ENV_GRID") != nullptr;
            if (sGrid) fillEnvDebugGrid(*store);
            std::fprintf(stderr, "[ENVREFL] baked %dx%d pano (+%d mips) for '%s' at its centroid (%.1f %.1f %.1f)\n",
                         store->view.W, store->view.H, store->view.numMips - 1,
                         M->Name ? M->Name : "?", c.x, c.y, c.z);
            env.stores.push_back(std::move(store));
            idx = int(env.stores.size()) - 1;
            bakedAny = true;
        }
        env.byMat[M] = idx;
        // Publish this store to the kernel table NOW (not just in the final
        // refresh below): later bakes in this same round then render this
        // material WITH its env reflections (1-bounce), and the metal-
        // neutralize hack in bakeStore stops firing for it. Part of the
        // --env_bake_fix bundle (bake order becomes content-relevant).
        if (fds::FeatureFlags::env_bake_fix() && idx >= 0 && M->ID < 256)
            env.table[M->ID] = &env.stores[size_t(idx)]->view;
    }
    // Refresh the matID table (IDs move when the editor rebuilds the table).
    std::memset(env.table, 0, sizeof(env.table));
    for (auto& [M, idx] : env.byMat)
        if (idx >= 0 && M->RelScene == sc && M->ID < 256) {
            env.table[M->ID] = &env.stores[size_t(idx)]->view;
            if (std::getenv("ENVDBG3") && M->Name && std::strstr(M->Name, "windows"))
                std::fprintf(stderr, "[ENVDBG3] table[%u] = store %d  mat=%p '%s'\n",
                             (unsigned)M->ID, idx, (void*)M, M->Name);
        }
    return bakedAny;
}

void EnvReflection_DrawViz(Scene* sc) {
    const int want = FeatureFlags::env_refl_viz();
    if (want <= 0 || !sc || !VPage || XRes <= 0 || YRes <= 0) return;
    // Never inside an offscreen render: the env bake itself runs the full
    // renderFrame per cube face, and the overlay was getting BAKED into
    // every probe rendered while the viewer was open.
    if (g_offscreenViewDepth > 0) return;
    auto it = g_envByScene.find(sc);
    if (it == g_envByScene.end() || it->second.stores.empty()) return;
    auto& stores = it->second.stores;
    const size_t idx = size_t(want - 1) < stores.size() ? size_t(want - 1)
                                                        : stores.size() - 1;
    const EnvPanoLinear& v = stores[idx]->view;
    if (!v.mip[0]) return;

    // Fit into a quarter of the frame (integer downscale), TOP-CENTER:
    // top-right hides under the editor's HTML side panel, top-left under
    // the profiler HUD. env_cube: lay the six faces out as a 3×2 grid.
    const int srcW = v.isCube ? v.W * 3 : v.W;
    const int srcH = v.isCube ? v.H * 2 : v.H;
    int scale = 1;
    while (srcW / scale > XRes / 2 || srcH / scale > YRes / 2) ++scale;
    const int dw = srcW / scale, dh = srcH / scale;
    const int x0 = (XRes - dw) / 2, y0 = 8;
    dword* out = reinterpret_cast<dword*>(VPage);
    auto srcAt = [&](int sx, int sy) -> uint32_t {
        if (v.isCube) {
            const int fr = v.W;
            const int f = (sy / fr) * 3 + (sx / fr);
            return v.mip[0][size_t(f) * fr * fr + size_t(sy % fr) * fr + (sx % fr)];
        }
        return v.mip[0][size_t(sy) * v.W + sx];
    };
    for (int y = 0; y < dh; ++y) {
        dword* row = out + size_t(y0 + y) * XRes + x0;
        for (int x = 0; x < dw; ++x)
            row[x] = srcAt(x * scale, y * scale) | 0xFF000000u;
    }
    // 1px frame so it reads as an overlay, not scene content.
    for (int x = -1; x <= dw; ++x) {
        out[size_t(y0 - 1) * XRes + x0 + x]  = 0xFF00FF00u;
        out[size_t(y0 + dh) * XRes + x0 + x] = 0xFF00FF00u;
    }
    for (int y = -1; y <= dh; ++y) {
        out[size_t(y0 + y) * XRes + x0 - 1]  = 0xFF00FF00u;
        out[size_t(y0 + y) * XRes + x0 + dw] = 0xFF00FF00u;
    }
}

int EnvReflection_RegisterCubeFaces(Scene* sc, Material* M,
                                    const uint32_t* faceMajor, int faceRes,
                                    int storeRes, const Vector& bakePoint,
                                    float pullOpt) {
    if (!sc || !M || !faceMajor || faceRes < 64) return -1;
    // --env-bake-res set explicitly: override the caller's store resolution
    // (CITY passes a hardcoded 256). Capped at faceRes below — the source
    // faces are what they are (CITY bakes 512²); we only ever downsample.
    if (const int face = envBakeResOverride()) {
        static bool noted = false;
        if (!noted && face != storeRes) {
            noted = true;
            std::fprintf(stderr, "[ENVREFL] env_bake_res=%d overrides"
                         " registered-store res (caller asked %d^2; capped at"
                         " the %d^2 source faces)\n", face, storeRes, faceRes);
        }
        storeRes = face;
    }
    if (storeRes > faceRes) storeRes = faceRes;
    SceneEnv& env = g_envByScene[sc];
    auto store = std::make_unique<EnvPanoStore>();
    EnvPanoLinear& v = store->view;
    // Downsample the source faces to storeRes (e.g. CITY 512 -> 256: the
    // env compose is roughness-blurred anyway and this keeps per-building
    // memory at the auto-bake tier), then chain mips from there.
    store->levels[0].assign(faceMajor, faceMajor + size_t(6) * faceRes * faceRes);
    int fr = faceRes;
    while (fr > storeRes) {
        std::vector<uint32_t> half;
        boxDownsampleCube(store->levels[0], fr, half);
        store->levels[0] = std::move(half);
        fr >>= 1;
    }
    v.isCube = true;
    v.noParallax = true;
    v.pullOpt = pullOpt;
    v.W = v.H = fr;
    v.numMips = EnvPanoLinear::kMaxMips;
    for (int k = 1; k < EnvPanoLinear::kMaxMips; ++k) {
        boxDownsampleCube(store->levels[k-1], fr, store->levels[k]);
        fr >>= 1;
    }
    for (int k = 0; k < EnvPanoLinear::kMaxMips; ++k)
        v.mip[k] = store->levels[k].data();
    v.bakeX = bakePoint.x; v.bakeY = bakePoint.y; v.bakeZ = bakePoint.z;
    {   // measurement content — see fillEnvDebugGrid (city building stores
        // register through HERE, not bakeStore: the panorama cache path).
        static const bool sGrid = std::getenv("FDS_ENV_GRID") != nullptr;
        if (sGrid) fillEnvDebugGrid(*store);
    }
    env.stores.push_back(std::move(store));
    const int idx = int(env.stores.size()) - 1;
    env.byMat[M] = idx;
    if (M->ID < 256) env.table[M->ID] = &env.stores[size_t(idx)]->view;
    return idx;
}

void EnvReflection_AliasMaterial(Scene* sc, Material* M, int storeIdx) {
    if (!sc || !M) return;
    SceneEnv& env = g_envByScene[sc];
    if (storeIdx < 0 || storeIdx >= int(env.stores.size())) return;
    env.byMat[M] = storeIdx;
    if (M->ID < 256) env.table[M->ID] = &env.stores[size_t(storeIdx)]->view;
}

int EnvReflection_Count(Scene* sc) {
    auto it = g_envByScene.find(sc);
    return it == g_envByScene.end() ? 0 : int(it->second.stores.size());
}

int EnvReflection_StoreIndex(Scene* sc, const Material* M) {
    if (!M) return -1;
    auto it = g_envByScene.find(sc);
    if (it == g_envByScene.end()) return -1;
    auto jt = it->second.byMat.find(M);
    return jt == it->second.byMat.end() ? -1 : jt->second;
}

const EnvPanoLinear* const* EnvReflection_Table(Scene* sc) {
    auto it = g_envByScene.find(sc);
    return it == g_envByScene.end() ? nullptr : it->second.table;
}

void EnvReflection_Invalidate(Scene* sc) { g_envByScene.erase(sc); }

}  // namespace fds
