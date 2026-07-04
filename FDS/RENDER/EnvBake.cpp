#include "EnvBake.h"
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

#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <map>
#include <memory>
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

// The material whose probe is currently baking — its own mesh(es) are
// excluded from the capture (see Transform.cpp; classic local-cubemap
// self-exclusion; null outside a bake).
Material* g_envBakeSkipMaterial = nullptr;

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

// Shared core: render the six cube faces from `center` and stitch them into
// a LINEAR equirect panorama in `pano`. Returns false on alloc failure.
static bool renderCubeAndStitch(Scene* sc, const Vector& center,
                                const EnvBakeParams& params,
                                std::vector<uint32_t>& pano,
                                Material* skipMat = nullptr) {
    if (!sc) return false;
    const int res = params.cubeRes;
    const int W = params.panoWidth, H = params.panoHeight;

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

    std::vector<uint32_t> faces[6];
    for (auto& fc : faces) fc.resize(size_t(res) * res);

    {
        OffscreenViewScope view(sc, &surf);
        view.publishSurface();               // XRes/CntrX/... from surf
        static Camera s_cam;
        view.setView(&s_cam);

        g_envBakeSkipDynamic = true;   // moving meshes stay out of the pano
        g_envBakeSkipMaterial = skipMat;   // ...and so does the reflector itself
        for (int i = 0; i < 6; ++i) {
            std::memset(&s_cam, 0, sizeof(s_cam));
            s_cam.ISource = center;
            s_cam.IFOV = 90.0f;
            const CubeFace& cf = kCubeFaces[i];
            s_cam.Mat[0][0] = cf.right.x; s_cam.Mat[0][1] = cf.right.y; s_cam.Mat[0][2] = cf.right.z;
            s_cam.Mat[1][0] = cf.up.x;    s_cam.Mat[1][1] = cf.up.y;    s_cam.Mat[1][2] = cf.up.z;
            s_cam.Mat[2][0] = cf.fwd.x;   s_cam.Mat[2][1] = cf.fwd.y;   s_cam.Mat[2][2] = cf.fwd.z;
            CalcPersp(&s_cam);

            // Clear to void color (each byte of voidColor) + empty Z.
            std::memset(surf.Data, params.voidColor & 0xFF, size_t(res) * res * 4);
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
            if (std::getenv("FDS_ENVBAKE_DUMP"))
                std::fprintf(stderr, "[ENVBAKE] face %d: CAll=%d NZP=%.2f FZP=%.2f "
                    "fwd=(%.0f,%.0f,%.0f)\n", i, int(CAll), sc->NZP, sc->FZP,
                    kCubeFaces[i].fwd.x, kCubeFaces[i].fwd.y, kCubeFaces[i].fwd.z);
            std::memcpy(faces[i].data(), surf.Data, size_t(res) * res * 4);
        }
        g_envBakeSkipDynamic = false;
        g_envBakeSkipMaterial = nullptr;
    }   // scope exit restores MainSurf/View/FOV/clip planes

    std::free(surf.Data);
    std::free(surf.Z16);

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

// World-space centroid of every face using material M. False if none found
// (material exists but no faces reference it — nothing to reflect anyway).
bool materialCentroid(Scene* sc, const Material* M, Vector& out) {
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
                                        Material* skipMat = nullptr) {
    EnvBakeParams params;
    // --env_refl_res (default 512): reflections are roughness-blurred by eye
    // anyway, so half the CITY bake res reads fine. Clamp: the mip chain
    // needs res ≥ 64; cap at 1024.
    int res = fds::FeatureFlags::env_refl_res();
    if (res < 64) res = 64;
    if (res > 1024) res = 1024;
    params.cubeRes = res / 2;
    params.panoWidth = res;
    params.panoHeight = res;
    auto store = std::make_unique<EnvPanoStore>();
    g_envBakeInProgress = true;
    const bool ok = renderCubeAndStitch(sc, center, params, store->levels[0], skipMat);
    g_envBakeInProgress = false;
    if (!ok) return nullptr;
    EnvPanoLinear& v = store->view;
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
        if (!materialCentroid(sc, M, c)) { env.byMat[M] = -1; continue; }
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
            auto store = bakeStore(sc, env, c, M);
            if (!store) { env.byMat[M] = -1; continue; }
            std::fprintf(stderr, "[ENVREFL] baked %dx%d pano (+%d mips) for '%s' at its centroid (%.1f %.1f %.1f)\n",
                         store->view.W, store->view.H, store->view.numMips - 1,
                         M->Name ? M->Name : "?", c.x, c.y, c.z);
            env.stores.push_back(std::move(store));
            idx = int(env.stores.size()) - 1;
            bakedAny = true;
        }
        env.byMat[M] = idx;
    }
    // Refresh the matID table (IDs move when the editor rebuilds the table).
    std::memset(env.table, 0, sizeof(env.table));
    for (auto& [M, idx] : env.byMat)
        if (idx >= 0 && M->RelScene == sc && M->ID < 256)
            env.table[M->ID] = &env.stores[size_t(idx)]->view;
    return bakedAny;
}

void EnvReflection_DrawViz(Scene* sc) {
    const int want = FeatureFlags::env_refl_viz();
    if (want <= 0 || !sc || !VPage || XRes <= 0 || YRes <= 0) return;
    auto it = g_envByScene.find(sc);
    if (it == g_envByScene.end() || it->second.stores.empty()) return;
    auto& stores = it->second.stores;
    const size_t idx = size_t(want - 1) < stores.size() ? size_t(want - 1)
                                                        : stores.size() - 1;
    const EnvPanoLinear& v = stores[idx]->view;
    if (!v.mip[0]) return;
    // Fit into the top-right quarter of the frame (integer downscale).
    int scale = 1;
    while (v.W / scale > XRes / 2 || v.H / scale > YRes / 2) ++scale;
    const int dw = v.W / scale, dh = v.H / scale;
    const int x0 = XRes - dw - 8, y0 = 8;
    dword* out = reinterpret_cast<dword*>(VPage);
    for (int y = 0; y < dh; ++y) {
        dword* row = out + size_t(y0 + y) * XRes + x0;
        const uint32_t* src = v.mip[0] + size_t(y) * scale * v.W;
        for (int x = 0; x < dw; ++x)
            row[x] = src[size_t(x) * scale] | 0xFF000000u;
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

int EnvReflection_Count(Scene* sc) {
    auto it = g_envByScene.find(sc);
    return it == g_envByScene.end() ? 0 : int(it->second.stores.size());
}

const EnvPanoLinear* const* EnvReflection_Table(Scene* sc) {
    auto it = g_envByScene.find(sc);
    return it == g_envByScene.end() ? nullptr : it->second.table;
}

void EnvReflection_Invalidate(Scene* sc) { g_envByScene.erase(sc); }

}  // namespace fds
