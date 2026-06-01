#include "SceneBuilder.h"

#include <Base/FDS_DECS.H>
#include <Base/FDS_VARS.H>
#include <Base/Material.h>
#include <Base/Object.h>
#include <Base/Omni.h>
#include <Base/Scene.h>
#include <Base/Spline.h>
#include <Base/TriMesh.h>
#include <Base/Vertex.h>
#include <FILLERS/TheOtherBarry.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

// External engine entry points.
extern void Compute_FaceVertexIndices(TriMesh *T);
extern void Scene_RebuildMatTable(Scene *Sc);
// Sachletz is declared in FDS_DECS.H via dword (unsigned int) args.

namespace fds::scene_builder {

namespace {

// Stamp a single-key Spline so Spline_Calc_3D/_4D_Alt return the
// provided value at any frame without crashing on empty Keys. Picks
// up the Quaternion-is-W-first trap (see memory: project_engine_axis_
// quaternion_conventions).
void StampSingleKey(Spline &sp, float x, float y, float z, float w) {
    sp.NumKeys = 1;
    sp.CurKey  = 0;
    sp.Flags   = 0;
    sp.Keys    = (SplineKey*)std::calloc(1, sizeof(SplineKey));
    sp.Keys[0].Frame = 0.0f;
    sp.Keys[0].Pos.x = x; sp.Keys[0].Pos.y = y; sp.Keys[0].Pos.z = z; sp.Keys[0].Pos.W = w;
    sp.Keys[0].AA.x  = x; sp.Keys[0].AA.y  = y; sp.Keys[0].AA.z  = z; sp.Keys[0].AA.W  = w;
}

Scene *AllocateEmptyScene() {
    Scene *sc = (Scene*)getAlignedBlock(sizeof(Scene), 16);
    std::memset(sc, 0, sizeof(Scene));
    sc->NZP = 0.5f;
    sc->FZP = 200.0f;
    sc->Flags = Scn_ZBuffer;
    sc->Ambient.R = sc->Ambient.G = sc->Ambient.B = 160;
    sc->Ambient.A = 255;
    return sc;
}

}  // namespace

SceneBuilder::SceneBuilder() : sc_(AllocateEmptyScene()), ownsScene_(true) {}
SceneBuilder::SceneBuilder(Scene *attach) : sc_(attach), ownsScene_(false) {}

void SceneBuilder::SetNearFar(float nzp, float fzp) {
    if (!sc_) return;
    sc_->NZP = nzp;
    sc_->FZP = fzp;
}

void SceneBuilder::SetAmbient(int r, int g, int b) {
    if (!sc_) return;
    sc_->Ambient.R = float(r);
    sc_->Ambient.G = float(g);
    sc_->Ambient.B = float(b);
    sc_->Ambient.A = 255.0f;
}

void SceneBuilder::SetFlags(unsigned flags) {
    if (!sc_) return;
    sc_->Flags |= flags;
}

// ── Textures ───────────────────────────────────────────────────────

Texture *SceneBuilder::AddSolidColorTexture(int w, int h, std::uint32_t bgra) {
    if (w <= 0 || h <= 0) return nullptr;
    Texture *T = new Texture();
    std::memset(T, 0, sizeof(Texture));
    T->BPP = 32;
    T->SizeX = w; T->SizeY = h;
    auto log2i = [](int n) -> int { int k = 0; while ((1 << k) < n) ++k; return k; };
    T->LSizeX = log2i(w);
    T->LSizeY = log2i(h);
    T->Data = (byte*)_aligned_malloc(size_t(w) * size_t(h) * 4, 16);
    dword *px = (dword*)T->Data;
    for (int i = 0; i < w * h; ++i) px[i] = bgra;
    if (w > 1 && h > 1) Sachletz(px, dword(w), dword(h));
    T->Mipmap[0] = T->Data;
    T->numMipmaps = 1;
    T->Flags = Txtr_Nomip | Txtr_Tiled;
    return T;
}

Texture *SceneBuilder::AddAlphaTextTexture(const char *fileName) {
    constexpr int W = 16, H = 16;
    Texture *T = new Texture();
    std::memset(T, 0, sizeof(Texture));
    T->BPP = 32;
    T->SizeX = W; T->SizeY = H;
    T->LSizeX = 4; T->LSizeY = 4;  // log2(16)
    T->Data = (byte*)_aligned_malloc(W * H * 4, 16);
    dword *px = (dword*)T->Data;
    // Simple 'M' on transparent background.
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        const bool onLetter =
            (x == 3 && y >= 3 && y <= 12) ||
            (x == 12 && y >= 3 && y <= 12) ||
            ((x == 4 || x == 5) && y >= 4 && y <= 7) ||
            ((x == 10 || x == 11) && y >= 4 && y <= 7) ||
            ((x == 6 || x == 7) && y >= 5 && y <= 7) ||
            ((x == 8 || x == 9) && y >= 5 && y <= 7);
        px[y * W + x] = onLetter ? 0xFFFFFFFFu : 0x00000000u;
    }
    Sachletz(px, dword(W), dword(H));
    T->Mipmap[0] = T->Data;
    T->numMipmaps = 1;
    T->Flags = Txtr_Nomip | Txtr_Tiled;
    if (fileName) T->FileName = strdup(fileName);
    return T;
}

// ── Materials ──────────────────────────────────────────────────────

Material *SceneBuilder::AddMaterial(const char *name, Texture *tex,
                                     Color baseCol, unsigned flags) {
    if (!sc_) return nullptr;
    Material *M = getAlignedType<Material>(16);
    std::memset(M, 0, sizeof(Material));
    M->Txtr        = tex;
    M->BaseCol     = baseCol;
    M->Diffuse     = 1.0f;
    M->Luminosity  = 0.0f;
    M->Flags       = flags | Mat_RGBInterp;
    M->RelScene    = sc_;
    M->ID          = 0;
    if (name) M->Name = strdup(name);
    // Link to MatLib tail so material IDs assigned by Scene_RebuildMatTable
    // come out in insertion order (easier to read in diagnostics).
    M->Prev = nullptr;
    M->Next = MatLib;
    if (MatLib) MatLib->Prev = M;
    MatLib = M;
    return M;
}

// ── Filler picker ──────────────────────────────────────────────────

RasterFunc SceneBuilder::PickFillerForMaterial(const Material *mat) {
    if (!mat) {
        return TheOtherBarry<barry::TBlendMode::OVERWRITE,
                             barry::TTextureMode::NORMAL>;
    }
    // Transparent: blend mode TRANSPARENT, NORMAL texture lookup.
    if (mat->Flags & Mat_Transparent) {
        return TheOtherBarry<barry::TBlendMode::TRANSPARENT,
                             barry::TTextureMode::NORMAL>;
    }
    // (Reflective materials use Face::Flags & Face_Reflective rather
    // than a Material-level flag, so the picker can't route by mat
    // alone — caller must stamp F.Filler directly for those.)
    // Default opaque.
    return TheOtherBarry<barry::TBlendMode::OVERWRITE,
                         barry::TTextureMode::NORMAL>;
}

// ── Geometry ───────────────────────────────────────────────────────

TriMesh *SceneBuilder::AddQuad(const char *name, const Vector v[4],
                               Material *mat) {
    if (!sc_ || !v || !mat) return nullptr;
    TriMesh *T = (TriMesh*)getAlignedBlock(sizeof(TriMesh), 16);
    std::memset(T, 0, sizeof(TriMesh));
    Matrix_Copy(T->RotMat, Mat_ID);
    Matrix_Copy(T->UnscaledRotMat, Mat_ID);
    T->IPos   = {0.0f, 0.0f, 0.0f};
    T->IScale = {1.0f, 1.0f, 1.0f};
    T->IRot   = {0.0f, 0.0f, 0.0f, 1.0f};
    T->Flags  = HTrack_Visible;
    StampSingleKey(T->Pos,    0.0f, 0.0f, 0.0f, 0.0f);
    StampSingleKey(T->Scale,  1.0f, 1.0f, 1.0f, 0.0f);
    StampSingleKey(T->Rotate, 0.0f, 0.0f, 0.0f, 1.0f);

    T->VIndex = 4;
    T->Verts  = new Vertex[4];
    std::memset(T->Verts, 0, sizeof(Vertex) * 4);
    // Compute the quad normal from the first triangle; stamp on every
    // vertex so the rasterizer's view-space normalize doesn't NaN.
    Vector e1 = { v[1].x - v[0].x, v[1].y - v[0].y, v[1].z - v[0].z };
    Vector e2 = { v[2].x - v[0].x, v[2].y - v[0].y, v[2].z - v[0].z };
    Vector qn = { e1.y*e2.z - e1.z*e2.y,
                  e1.z*e2.x - e1.x*e2.z,
                  e1.x*e2.y - e1.y*e2.x };
    qn.normalize();
    for (int i = 0; i < 4; ++i) {
        T->Verts[i].Pos = v[i];
        T->Verts[i].N   = qn;
        T->Verts[i].TN  = qn;
        // Pre-fill per-vertex light so the rasterizer's pre-Lighting
        // reads don't garbage-color the first frame.
        T->Verts[i].LR  = T->Verts[i].LG = T->Verts[i].LB = 200;
        T->Verts[i].LA  = 255;
        T->Verts[i].U = (i == 1 || i == 2) ? 1.0f : 0.0f;
        T->Verts[i].V = (i >= 2) ? 1.0f : 0.0f;
    }
    T->FIndex = 2;
    T->Faces  = new Face[2];
    std::memset(T->Faces, 0, sizeof(Face) * 2);
    auto setupTri = [&](Face &F, int ai, int bi, int ci) {
        F.A = T->Verts + ai;
        F.B = T->Verts + bi;
        F.C = T->Verts + ci;
        F.Txtr = mat;
        F.U1 = F.A->U; F.V1 = F.A->V;
        F.U2 = F.B->U; F.V2 = F.B->V;
        F.U3 = F.C->U; F.V3 = F.C->V;
        F.N = qn;
        F.NormProd = -(F.N.x*F.A->Pos.x + F.N.y*F.A->Pos.y + F.N.z*F.A->Pos.z);
        F.Filler = PickFillerForMaterial(mat);
    };
    setupTri(T->Faces[0], 0, 1, 2);
    setupTri(T->Faces[1], 0, 2, 3);

    // Loose bsphere covering all 4 verts.
    Vector ctr = {0,0,0};
    for (int i = 0; i < 4; ++i) { ctr.x += v[i].x; ctr.y += v[i].y; ctr.z += v[i].z; }
    ctr.x *= 0.25f; ctr.y *= 0.25f; ctr.z *= 0.25f;
    float radSq = 0.0f;
    for (int i = 0; i < 4; ++i) {
        const float dx = v[i].x - ctr.x, dy = v[i].y - ctr.y, dz = v[i].z - ctr.z;
        const float r  = dx*dx + dy*dy + dz*dz;
        if (r > radSq) radSq = r;
    }
    T->BSphereCtr = ctr;
    T->BSphereRad = radSq;
    T->BSphereRadius = std::sqrt(radSq);
    Compute_FaceVertexIndices(T);

    Object *Obj = new Object();
    std::memset(Obj, 0, sizeof(Object));
    Obj->Type = Obj_TriMesh;
    Obj->Data = T;
    Obj->Pos  = &T->IPos;
    Obj->Rot  = &T->RotMat;
    if (name) Obj->Name = strdup(name);
    Obj->Next = sc_->ObjectHead;
    if (sc_->ObjectHead) sc_->ObjectHead->Prev = Obj;
    sc_->ObjectHead = Obj;
    T->Next = sc_->TriMeshHead;
    if (sc_->TriMeshHead) sc_->TriMeshHead->Prev = T;
    sc_->TriMeshHead = T;
    return T;
}

TriMesh *SceneBuilder::AddCube(const char *name, Vector ctr, float halfEdge,
                               Material *mat) {
    if (!sc_ || !mat) return nullptr;
    const float h = halfEdge;
    // Six faces, outward CCW. We just build six quads via repeated
    // AddQuad — cheaper to maintain than a hand-rolled 24-vert mesh.
    const Vector pos_x[4] = {
        { ctr.x+h, ctr.y-h, ctr.z+h }, { ctr.x+h, ctr.y-h, ctr.z-h },
        { ctr.x+h, ctr.y+h, ctr.z-h }, { ctr.x+h, ctr.y+h, ctr.z+h }
    };
    const Vector neg_x[4] = {
        { ctr.x-h, ctr.y-h, ctr.z-h }, { ctr.x-h, ctr.y-h, ctr.z+h },
        { ctr.x-h, ctr.y+h, ctr.z+h }, { ctr.x-h, ctr.y+h, ctr.z-h }
    };
    const Vector pos_y[4] = {
        { ctr.x-h, ctr.y+h, ctr.z+h }, { ctr.x+h, ctr.y+h, ctr.z+h },
        { ctr.x+h, ctr.y+h, ctr.z-h }, { ctr.x-h, ctr.y+h, ctr.z-h }
    };
    const Vector neg_y[4] = {
        { ctr.x-h, ctr.y-h, ctr.z-h }, { ctr.x+h, ctr.y-h, ctr.z-h },
        { ctr.x+h, ctr.y-h, ctr.z+h }, { ctr.x-h, ctr.y-h, ctr.z+h }
    };
    const Vector pos_z[4] = {
        { ctr.x-h, ctr.y-h, ctr.z+h }, { ctr.x+h, ctr.y-h, ctr.z+h },
        { ctr.x+h, ctr.y+h, ctr.z+h }, { ctr.x-h, ctr.y+h, ctr.z+h }
    };
    const Vector neg_z[4] = {
        { ctr.x+h, ctr.y-h, ctr.z-h }, { ctr.x-h, ctr.y-h, ctr.z-h },
        { ctr.x-h, ctr.y+h, ctr.z-h }, { ctr.x+h, ctr.y+h, ctr.z-h }
    };
    std::string base = name ? name : "cube";
    TriMesh *first = AddQuad((base + "_+x").c_str(), pos_x, mat);
    AddQuad((base + "_-x").c_str(), neg_x, mat);
    AddQuad((base + "_+y").c_str(), pos_y, mat);
    AddQuad((base + "_-y").c_str(), neg_y, mat);
    AddQuad((base + "_+z").c_str(), pos_z, mat);
    AddQuad((base + "_-z").c_str(), neg_z, mat);
    return first;  // returns the first quad's mesh as a convenience handle
}

// ── Lights ─────────────────────────────────────────────────────────

Omni *SceneBuilder::AddOmni(Vector pos, Color color, float intensity,
                            float range) {
    if (!sc_) return nullptr;
    Omni *O = (Omni*)getAlignedBlock(sizeof(Omni), 16);
    std::memset(O, 0, sizeof(Omni));
    O->IPos   = pos;
    O->IRange = range;
    O->rRange = (range > 0.0f) ? 1.0f / range : 0.0f;
    O->ISize  = intensity;
    O->L      = color;
    O->Type   = Light_Omni;
    O->Flags  = Omni_Active | Omni_Stationary;
    // Single-key splines so Animate_Objects / Lighting don't crash
    // on empty Keys.
    StampSingleKey(O->Pos,    pos.x, pos.y, pos.z, 0.0f);
    StampSingleKey(O->Size,   intensity, intensity, intensity, 0.0f);
    StampSingleKey(O->Range,  range, range, range, 0.0f);
    O->Prev = nullptr;
    O->Next = sc_->OmniHead;
    if (sc_->OmniHead) sc_->OmniHead->Prev = O;
    sc_->OmniHead = O;
    return O;
}

// ── Camera ─────────────────────────────────────────────────────────

void SceneBuilder::SetCamera(Vector eye, Vector lookAt, float fov) {
    if (!sc_) return;
    Camera *cam = sc_->CameraHead;
    if (!cam) {
        cam = (Camera*)getAlignedBlock(sizeof(Camera), 16);
        std::memset(cam, 0, sizeof(Camera));
        sc_->CameraHead = cam;
    }
    cam->ISource = eye;
    cam->IFOV    = fov;
    Kick_Camera(&cam->ISource, &lookAt, 0.0f, cam->Mat);
}

// ── Finalize ───────────────────────────────────────────────────────

void SceneBuilder::Finalize() {
    if (!sc_) return;
    // Register every material we've added with the deferred matTable
    // so per-pixel matID lookups resolve.
    Scene_RebuildMatTable(sc_);
}

}  // namespace fds::scene_builder
