#include "EnvBake.h"
#include "EnvCube.h"
#include "Hdr.h"
#include "OffscreenView.h"
#include "WorldAabb.h"

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
#include <cctype>
#include <chrono>
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

// ENVDYN A3: the INVERSE filter — read by Transform_Objects while the dynamic-
// mesh env overlay renders. True → static meshes are skipped and ONLY dynamic
// meshes (the mech) are submitted, so the overlay draws just the movers over
// the probe's retained static capture. Set/cleared around the overlay's face
// renders; never both this and g_envBakeSkipDynamic true at once.
bool g_envOverlayDynamicOnly = false;

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

// Live-water env perturbation state (--env_live_water). Published by the
// scene tick (CITY) before renderFrame; consumed read-only by the deferred
// env compose + the forward reflective vertex pass. See EnvBake.h.
EnvLiveWaterState g_envLiveWater;

// LEGACY (--no-env_bake_fix, the default): the old whole-mesh exclusion,
// kept byte-identical for the pinned city baseline (its vehicle-glass
// probes bake through this path). Null when the fix is active.
Material* g_envBakeLegacySkipMat = nullptr;

bool EnvBake_HasSkipFaces() { return !g_envBakeSkipMats.empty(); }

// Mirror-clone geometry must be INVISIBLE to the env-probe machinery.
// BuildMirror (GreetsMirror.cpp) wires a reflected copy of every mesh into
// the scene as an Object named "__mirrorClone_*" whose faces reference the
// ORIGINAL materials. Left in, the clones break the probes three ways
// (the editor's "borked pano" the day mirrors defaulted ON there):
//   • materialCentroid's instance clustering counts the clone statues as
//     extra "instances" and can pick a CLONE cluster as the heaviest — the
//     probe then bakes from BEHIND the mirror wall, outside the room, and
//     every face comes out mostly void with floating fragments of the room
//     interior seen through the walls' backfaces (measured: momy probe at
//     z=-77 / amudim at z=-91 for a room at z≈-21; faces 0–32% coverage
//     vs 100% with mirrors off);
//   • sceneAABB inflates the parallax proxy box with the mirrored
//     half-space (~2× the room), skewing every parallax-corrected lookup;
//   • during the bake render itself the clones are at best per-pixel
//     mask-rejected (wasted raster), at worst committed (stale/absent mask).
// True while a --env_bake_fix probe bake renders its cube faces:
// Transform_Objects then skips clone meshes wholesale (bake-scoped; the
// legacy bake path and the disco-ball pano keep their behavior).
bool g_envBakeSkipMirrorClones = false;

bool EnvBake_IsMirrorCloneObj(const Object* O)
{
    return O && O->Name && std::strncmp(O->Name, "__mirrorClone_", 14) == 0;
}

namespace {
// The TriMeshes owned by "__mirrorClone_*" objects (empty when mirrors are
// off — every non-mirror scene pays one null ObjectHead walk).
void collectMirrorCloneMeshes(Scene* sc, std::vector<const TriMesh*>& out)
{
    out.clear();
    for (Object* o = sc->ObjectHead; o; o = o->Next)
        if (o->Type == Obj_TriMesh && o->Data && EnvBake_IsMirrorCloneObj(o))
            out.push_back(static_cast<const TriMesh*>(o->Data));
}
}  // namespace

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
                           float skipRadius = 0.0f, bool publishProj = false,
                           std::vector<uint16_t>* facesZOut = nullptr,
                           bool dynamicOnly = false,
                           const bool* faceMask = nullptr,
                           bool linearCapture = false) {
    // facesZOut (ENVDYN A2): optional per-face depth capture (6 vectors of
    // res², filled from surf.Z16 before it is freed). nullptr = no capture,
    // byte-identical to the legacy bake.
    // dynamicOnly (ENVDYN A3): render ONLY dynamic meshes (the overlay) — the
    // inverse of the static-only bake. faceMask (6 bools, nullptr = all faces):
    // render only the touched faces. Both default off → the legacy bake path.
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

        // Static-only bake (default) vs dynamic-only overlay (ENVDYN A3). Never
        // both true at once — the overlay skips STATIC meshes, keeping the
        // movers, and the static capture already lives in the store (A2).
        g_envBakeSkipDynamic     = !dynamicOnly;
        g_envOverlayDynamicOnly  =  dynamicOnly;
        // (the reflector's own faces stay out too — g_envBakeSkipMats above)
        // Fix-bundle probes: mirror-clone meshes stay out STRUCTURALLY
        // (see g_envBakeSkipMirrorClones above). publishProj-gated so the
        // legacy bake path and the disco-ball pano stay byte-identical.
        g_envBakeSkipMirrorClones = publishProj;
        for (int i = 0; i < 6; ++i) {
            if (faceMask && !faceMask[i]) continue;   // overlay: touched faces only
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
            // LINEAR CAPTURE: replace the LDR VPage capture with the kernel's
            // own LINEAR radiance for this face. The bake's nested renderFrame
            // already called Hdr_BeginFrame at the FACE res (RENDER.CPP:648
            // runs inside this Render()), so g_hdrBuf holds this face's linear
            // radiance on the 0-255 scale — the same scale EnvSpecComposeScalar
            // and the SH projection read the store on. Storing it in the same
            // 8-bit face buffer keeps every consumer unchanged. h[3] is the
            // kernel's coverage flag: uncovered texels (sky / void / forward
            // content the deferred kernel never wrote) keep the LDR value.
            //
            // The gate is the CALLER's, not a flag read here: this one routine
            // feeds two different consumers — the REFLECTION probes
            // (--env_bake_linear) and the scene-centre SH AMBIENT probe
            // (--sh_bake_linear, SHAmbient_EnsureBaked) — and they were a
            // single flag until 2026-08-08, which hid the fact that the SH half
            // owns most of the whole-frame move (docs/SHADING_CONTRACT.md §8
            // row M2). Split so the two looks can be judged apart.
            if (linearCapture && fds::Hdr_WritableFor(res, res)) {
                uint32_t* dstf = faces[i].data();
                const fds::hdrf* hb = fds::g_hdrBuf.data();
                for (size_t k = 0; k < size_t(res) * res; ++k) {
                    const fds::hdrf* h = hb + k * 4;
                    if (float(h[3]) <= 0.0f) continue;      // no coverage
                    auto q = [](float v) -> uint32_t {
                        if (!(v > 0.0f)) return 0u;
                        return v >= 255.0f ? 255u : uint32_t(v + 0.5f);
                    };
                    dstf[k] = 0xFF000000u | (q(float(h[2])) << 16)
                                          | (q(float(h[1])) <<  8)
                                          |  q(float(h[0]));
                }
            }
            if (facesZOut) {
                const word* zp = reinterpret_cast<const word*>(surf.Z16);
                facesZOut[i].assign(zp, zp + size_t(res) * res);
            }
        }
        g_envBakeSkipDynamic = false;
        g_envOverlayDynamicOnly = false;
        g_envBakeSkipMirrorClones = false;
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
                        skipRadius, publishProj, /*facesZOut=*/nullptr,
                        /*dynamicOnly=*/false, /*faceMask=*/nullptr,
                        /*linearCapture=*/fds::FeatureFlags::env_bake_linear()))
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
    // Bake provenance, for the largest-wish-wins res upgrade of SHARED
    // stores (FramePrep): the original probe material + self-exclusion
    // radius, so a re-bake at a bigger face res reproduces the original
    // content exactly. `imported` marks RegisterCubeFaces stores (city
    // per-building) — their content came from a caller import and cannot
    // be re-baked here.
    Material* bakedSkipMat = nullptr;
    float     bakedSkipR   = 0.0f;
    bool      imported     = false;

    // ── ENVDYN Workstream A2: static retention for flagged probes ─────────
    // Allocated ONLY when the owning material has Material::EnvDynamic (an
    // authored envDynamic RVSF flag) AND the store is a padded cube store.
    // The dynamic-mesh overlay (A3) composites the live mech OVER this
    // pristine static capture each frame, so the static content must survive
    // the writeback into levels[0]. staticColorMaster = the bake's face-major
    // colour (a copy of levels[0] at bake time); staticFaceZ = the per-face
    // depth the bake would otherwise FREE (renderSixFaces), face-major
    // (6·faceRes² u16), used to occlude the mech behind static geometry.
    bool                  envDynamic = false;
    std::vector<uint32_t> staticColorMaster;   // 6·faceRes² (cube stores)
    std::vector<uint16_t> staticFaceZ;         // 6·faceRes² (cube stores)

    // ── ENVDYN A3 owner-visibility gate fix ───────────────────────────────
    // Tight world AABB of bakedSkipMat's OWN faces — NOT the whole-MESH union
    // WorldAabb_ForMaterial returns. Greets merges every reflective surface
    // (momy / stairs / screen / room) into ONE chunked Piramid mesh, so the
    // whole-mesh union is the entire scene box for EVERY flagged material: the
    // owner gate could then never cull an off-screen probe, and off-screen
    // probes stole the per-frame overlay budget from the one actually in view.
    // Computed once (the reflective owner geometry is static) and cached.
    WorldAabb             ownerFaceAabb;
    bool                  ownerFaceAabbDone = false;

    // ── ENVDYN A4: screen-priority scheduling + the interval instrument ───
    // lastOverlay is the SceneEnv::dynFrame index of the last composite; the
    // scheduler's ageing term and the --env_dyn_stats histogram both read it.
    // `everOverlaid` false means "never updated", which must outrank every
    // aged probe (a probe showing no mech at all is the worst staleness).
    uint32_t lastOverlay   = 0;
    bool     everOverlaid  = false;
    // --env_dyn_stats accumulators, reset at every report. stderr only.
    uint32_t stWant        = 0;   // frames flagged + a mover in some face
    uint32_t stWantVisible = 0;   // ...and the owner was on screen
    uint32_t stUpdates     = 0;   // frames actually composited
    uint32_t stFaces       = 0;   // faces rendered
    uint32_t stGapSum      = 0;   // Σ interval, over stUpdates-1 intervals
    uint32_t stGapMax      = 0;
    uint32_t stVisStreak   = 0;   // current run of visible-want-but-no-update
    uint32_t stVisStale    = 0;   // worst such run
    std::vector<uint32_t> stGaps; // every interval, for the p50
    // THE POP. The metric "jumpy" actually names: how stale a probe's content
    // is at the INSTANT its owner comes back on screen. A plain max-interval is
    // censored (a probe that goes off screen and never returns before the
    // window ends never closes its interval, so the legacy path's off-screen
    // staleness does not appear in it at all), and a mean is dominated by the
    // frames where the probe is visible and refreshing every frame anyway.
    bool     stPrevVisible = false;
    uint32_t stPopN = 0, stPopSum = 0, stPopMax = 0;
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
    // ENVDYN A4: EnvDynamic_Overlay call counter (the scheduler's clock) and
    // the --env_dyn_stats reporting window.
    uint32_t dynFrame = 0;
    uint32_t statFrom = 0;
    double   statMs = 0.0;
};
std::map<Scene*, SceneEnv> g_envByScene;
bool g_envBakeInProgress = false;   // bake renders through Render() → guard

// Clamp to [64,1024] + round DOWN to a power of two — the 4-level mip chain
// and the samplers' shift-indexed dims (envP->W >> lvl) need cleanly
// halvable sizes.
static int sanitizeFaceRes(int want) {
    const int clamped = want < 64 ? 64 : (want > 1024 ? 1024 : want);
    int p2 = 64;
    while (p2 * 2 <= clamped) p2 <<= 1;
    return p2;
}

// Editor/wasm bake-res safety ceiling (env_bake_res_cap). The wasm editor is
// capped at 4GB and greets sits near it; re-baking every reflective surface at
// 512²+ can spike a probe's transient (~30·res² bytes: color surface + Z16 +
// six face buffers) past the ceiling → wasm "memory access out of bounds".
// Under wasm we clamp every probe's FACE res to a memory-safe ceiling; native
// is uncapped by default (no 4GB limit → the pinned baselines stay
// byte-identical, since capFaceRes/the bakeStore clamp become no-ops), but the
// flag can force a cap natively for testing. Returns 0 = no cap.
static int bakeResCap() {
    if (FeatureFlags::isSet(FeatureFlags::IntId::env_bake_res_cap)) {
        const int c = FeatureFlags::env_bake_res_cap();
        return c > 0 ? sanitizeFaceRes(c) : 0;
    }
#ifdef __EMSCRIPTEN__
    return 256;   // wasm default ceiling (editor + web demo) ≈ 2MB/probe
#else
    return 0;     // native default: uncapped
#endif
}

// Clamp a face-res value to the cap (0 wish = "no explicit size", passed
// through untouched so the legacy sizing inside bakeStore still applies —
// bakeStore caps its own final params.cubeRes as the authoritative net).
static int capFaceRes(int face) {
    const int cap = bakeResCap();
    return (cap > 0 && face > cap) ? cap : face;
}

// Global/scene FACE resolution for the deferred env-reflection stores
// (bakeStore probes AND RegisterCubeFaces imports). THE SIZING CHAIN (top
// wins; the per-surface Material::EnvBakeRes wish sits above all of it in
// envFaceResForMat):
//   1. explicit --env-bake-res            (CLI/env/editor — isSet-gated)
//   2. explicit --env-bake-res-scene      (live/CLI override of the scene
//                                          default; isSet-gated, 0 cancels)
//   3. Scene::EnvBakeResScene             (AUTHORED — FLD scene header from
//                                          the LWS 'FdsSceneEnvBakeRes'
//                                          keyword; 0 = not authored)
//   4. return 0 = the legacy sizing       (env_refl_res/2 for probes, the
//                                          caller's storeRes for imports),
//                                          which keeps the pinned baselines
//                                          byte-identical.
// The value is read at BAKE time only; every consumer reads dims from the
// store's own recorded W/H/numMips, so mixed-res stores coexist safely.
int envBakeResOverride(const Scene* sc) {
    int want = 0;
    if (FeatureFlags::isSet(FeatureFlags::IntId::env_bake_res))
        want = FeatureFlags::env_bake_res();
    else if (FeatureFlags::isSet(FeatureFlags::IntId::env_bake_res_scene))
        want = FeatureFlags::env_bake_res_scene();
    else if (sc && sc->EnvBakeResScene > 0)
        want = (int)sc->EnvBakeResScene;
    if (want <= 0) return 0;
    const int p2 = sanitizeFaceRes(want);
    static int noted = INT_MIN;
    if (p2 != want && noted != want) {
        noted = want;
        std::fprintf(stderr, "[ENVREFL] env bake res %d invalid (want a power"
                     " of two in 64..1024) — using %d\n", want, p2);
    }
    return p2;
}

// Per-material FACE-res wish: Material::EnvBakeRes wins over the global/
// scene chain (most specific first); 0/unset defers to envBakeResOverride
// (which returns 0 when nothing applies — the legacy sizing chain).
// EnvBakeRes is sanitized to a pow2 in 64..1024 at set time
// (MaterialImport_SetSurfaceProp); re-sanitized here defensively since the
// mip chain would crash on a rogue value.
int envFaceResForMat(const Material* M) {
    if (!M || M->EnvBakeRes <= 0) return envBakeResOverride(M ? M->RelScene : nullptr);
    return sanitizeFaceRes(M->EnvBakeRes);
}

// Effective env-reflection mode for a material: the per-surface tri-state
// (Material::EnvReflMode) when set, else the SCENE-WIDE default — the
// env_refl_scene_mode flag when explicitly set (live/CLI), else the
// AUTHORED Scene::EnvReflSceneMode (FLD scene header, LWS 'FdsSceneEnvRefl').
// -1 = never bake/publish, 0 = the historical auto qualification rule
// (Reflection > 0 || MetallicMap), 1 = force-bake. Nothing authored/set →
// 0 everywhere → byte-identical legacy behavior.
static int envEffModeFor(const Material* M) {
    if (!M) return 0;
    if (M->EnvReflMode != 0) return M->EnvReflMode;
    int s;
    if (FeatureFlags::isSet(FeatureFlags::IntId::env_refl_scene_mode))
        s = FeatureFlags::env_refl_scene_mode();
    else
        s = M->RelScene ? (int)M->RelScene->EnvReflSceneMode : 0;
    return s < 0 ? -1 : s > 0 ? 1 : 0;
}

// ENVDYN A2/A3: does this material's probe opt into the live dynamic-mesh
// overlay? The authored per-surface Material::EnvDynamic flag, gated to the
// padded cube-store path (env_cube) — the overlay renders/composites cube
// faces, and legacy equirect / imported (city) stores have no retained depth.
static bool envDynamicForMat(const Material* M) {
    return M && M->EnvDynamic && fds::FeatureFlags::env_cube();
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
                          Material* skipMat, float skipRadius,
                          std::vector<uint16_t>* faceZMajorOut = nullptr) {
    const int fr = params.cubeRes;
    std::vector<uint32_t> faces[6];
    // faceZMajorOut (ENVDYN A2): when non-null, capture the per-face depth and
    // pack it FACE-MAJOR (6·fr² u16) alongside level0's colour — the overlay's
    // mech-vs-static occlusion test needs it.
    std::vector<uint16_t> facesZ[6];
    // publishProj (and with it the whole --env_bake_fix bundle inside
    // renderSixFaces: per-face projection, FACE-level self-exclusion,
    // mirror-mask neutralization) must be FLAG-GATED here exactly like the
    // equirect path in bakeStore — an unconditional `true` leaked the fixed
    // bake into the DEFAULT cube path (env_cube defaults ON) and broke the
    // pinned city baseline (its vehicle-glass/window probes auto-bake
    // through here with legacy projection).
    if (!renderSixFaces(sc, center, fr, fds::EnvCube_FaceFovDegrees(),
                        params.voidColor, faces, skipMat, skipRadius,
                        /*publishProj=*/fds::FeatureFlags::env_bake_fix(),
                        faceZMajorOut ? facesZ : nullptr,
                        /*dynamicOnly=*/false, /*faceMask=*/nullptr,
                        /*linearCapture=*/fds::FeatureFlags::env_bake_linear()))
        return false;
    level0.resize(size_t(6) * fr * fr);
    for (int f = 0; f < 6; ++f)
        std::memcpy(level0.data() + size_t(f) * fr * fr, faces[f].data(),
                    size_t(fr) * fr * 4);
    if (faceZMajorOut) {
        faceZMajorOut->resize(size_t(6) * fr * fr);
        for (int f = 0; f < 6; ++f)
            std::memcpy(faceZMajorOut->data() + size_t(f) * fr * fr,
                        facesZ[f].data(), size_t(fr) * fr * sizeof(uint16_t));
    }
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

// ── Per-probe FACE CENSUS + atlas dump (FDS_ENVBAKE_DUMP=1) ───────────────
// The pre-existing dump inside renderCubeFacesMajor writes ONE fixed path, so
// with six probes in flight the file that survives is whichever baked last —
// useless for comparing a NAMED probe against the GPU arm's same-named one.
// This writes `/tmp/envbake_<material>.ppm` (3x2 face atlas, face order
// +X -X +Y -Y +Z -Z) and prints a per-face census so the comparison can be
// made from the log alone. The store's texels are the LDR VPage the bake
// render produced, i.e. the SAME 0-255 scale the kernel's env compose adds
// into sB — so these numbers are directly comparable to any other arm's probe
// content expressed on the 0-255 radiance scale.
void dumpCubeStoreCensus(const char* matName,
                         const std::vector<uint32_t>& level0, int fr) {
    if (!std::getenv("FDS_ENVBAKE_DUMP")) return;
    if (fr <= 0 || level0.size() < size_t(6) * fr * fr) return;
    std::string safe = matName ? matName : "unnamed";
    for (char& c : safe) if (!std::isalnum((unsigned char)c)) c = '_';
    static const char* kFaceName[6] = { "+X", "-X", "+Y", "-Y", "+Z", "-Z" };
    for (int f = 0; f < 6; ++f) {
        const uint32_t* p = level0.data() + size_t(f) * fr * fr;
        double sB = 0, sG = 0, sR = 0, sY = 0;
        std::vector<float> ys; ys.reserve(size_t(fr) * fr);
        size_t nonVoid = 0;
        for (int i = 0; i < fr * fr; ++i) {
            const float B = float(p[i] & 0xFF), G = float((p[i] >> 8) & 0xFF),
                        R = float((p[i] >> 16) & 0xFF);
            const float Y = 0.299f * R + 0.587f * G + 0.114f * B;
            sB += B; sG += G; sR += R; sY += Y;
            ys.push_back(Y);
            if (p[i] & 0x00FFFFFFu) ++nonVoid;
        }
        std::sort(ys.begin(), ys.end());
        const double n = double(fr) * fr;
        std::fprintf(stderr, "[ENVBAKE-FACE] '%s' face %d %s res %d  "
            "meanBGR %.2f/%.2f/%.2f  meanY %.2f  p50 %.1f p95 %.1f max %.1f  "
            "nonvoid %.1f%%\n", matName ? matName : "?", f, kFaceName[f], fr,
            sB / n, sG / n, sR / n, sY / n,
            double(ys[size_t(0.50 * (n - 1))]), double(ys[size_t(0.95 * (n - 1))]),
            double(ys.back()), 100.0 * double(nonVoid) / n);
    }
    const std::string path = "/tmp/envbake_" + safe + ".ppm";
    if (FILE* fp = std::fopen(path.c_str(), "wb")) {
        const int gw = fr * 3, gh = fr * 2;
        std::fprintf(fp, "P6\n%d %d\n255\n", gw, gh);
        for (int y = 0; y < gh; ++y)
            for (int x = 0; x < gw; ++x) {
                const int f = (y / fr) * 3 + (x / fr);
                const uint32_t p = level0[size_t(f) * fr * fr
                                          + size_t(y % fr) * fr + (x % fr)];
                unsigned char rgb[3] = { (unsigned char)((p >> 16) & 0xFF),
                                         (unsigned char)((p >> 8) & 0xFF),
                                         (unsigned char)(p & 0xFF) };
                std::fwrite(rgb, 1, 3, fp);
            }
        std::fclose(fp);
        std::fprintf(stderr, "[ENVBAKE-FACE] wrote %s (%dx%d atlas)\n",
                     path.c_str(), gw, gh);
    }
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
    // Mirror-clone meshes reference the ORIGINAL materials — without this
    // skip the instance clustering below counts the mirrored copies as
    // extra instances and can put the probe in the mirrored half-space
    // (outside the room → mostly-void pano). See g_envBakeSkipMirrorClones.
    std::vector<const TriMesh*> cloneMeshes;
    collectMirrorCloneMeshes(sc, cloneMeshes);
    auto isClone = [&](const TriMesh* T) {
        return std::find(cloneMeshes.begin(), cloneMeshes.end(), T)
               != cloneMeshes.end();
    };
    double sx = 0, sy = 0, sz = 0;
    long n = 0;
    for (TriMesh* T = sc->TriMeshHead; T; T = T->Next) {
        if (isClone(T)) continue;
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
        for (TriMesh* T = sc->TriMeshHead; T; T = T->Next) {
            if (isClone(T)) continue;
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
            if (std::getenv("ENVDBG"))
                for (size_t ci = 0; ci < cls.size(); ++ci)
                    std::fprintf(stderr, "[ENVDBG]   '%s' cluster %zu: n=%ld centroid"
                        " (%.1f %.1f %.1f)%s\n", M->Name ? M->Name : "?", ci,
                        cls[ci].n, float(cls[ci].sx/cls[ci].n),
                        float(cls[ci].sy/cls[ci].n), float(cls[ci].sz/cls[ci].n),
                        (&cls[ci] == heavy) ? "  <-- PROBE (heaviest)" : "");
        }
    }
    return true;
}

void sceneAABB(Scene* sc, SceneEnv& env) {
    if (env.boxValid) return;
    // Parallax proxy box: real geometry only — mirror-clone meshes would
    // stretch it across the mirrored half-space (~2× the room) and skew
    // every parallax-corrected env lookup.
    std::vector<const TriMesh*> cloneMeshes;
    collectMirrorCloneMeshes(sc, cloneMeshes);
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (TriMesh* T = sc->TriMeshHead; T; T = T->Next) {
        if (std::find(cloneMeshes.begin(), cloneMeshes.end(), T)
            != cloneMeshes.end()) continue;
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
    }
    for (int a = 0; a < 3; ++a) { env.boxMin[a] = lo[a]; env.boxMax[a] = hi[a]; }
    env.boxValid = true;
}

// Bake one panorama from `center` into a fresh store (mip chain + metadata).
// faceResWant > 0 = the per-material Material::EnvBakeRes wish (already a
// sanitized pow2) — it wins the sizing chain below.
std::unique_ptr<EnvPanoStore> bakeStore(Scene* sc, const SceneEnv& env,
                                        const Vector& center,
                                        Material* skipMat = nullptr,
                                        float skipRadius = 0.0f,
                                        int faceResWant = 0,
                                        bool retainStatic = false) {
    // retainStatic (ENVDYN A2): keep a pristine static colour master + the
    // per-face depth for the dynamic-mesh overlay (A3). Cube stores only;
    // ignored for the legacy equirect path (the overlay renders cube faces).
    EnvBakeParams params;
    // Bake sizing: per-surface EnvBakeRes wish, then --env-bake-res (explicit
    // global FACE res, pow2 64..1024); else the legacy --env_refl_res sizing
    // (default 512): reflections are roughness-blurred by eye anyway, so half
    // the CITY bake res reads fine. Clamp: the mip chain needs res ≥ 64; cap
    // at 1024.
    if (faceResWant > 0) {
        params.cubeRes    = faceResWant;
        params.panoWidth  = faceResWant * 2;   // legacy equirect stores keep
        params.panoHeight = faceResWant * 2;   // the 2×face pano sizing
    } else if (const int face = envBakeResOverride(sc)) {
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
    // Editor/wasm safety ceiling (authoritative net for every sizing branch,
    // including the legacy path above and any per-surface wish). pano dims are
    // 2×cubeRes in all branches, so keep that ratio when clamping. No-op on
    // native (cap 0) → pinned baselines unaffected.
    if (const int cap = bakeResCap(); cap > 0 && params.cubeRes > cap) {
        params.cubeRes    = cap;
        params.panoWidth  = cap * 2;
        params.panoHeight = cap * 2;
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
                                             skipRadius,
                                             retainStatic ? &store->staticFaceZ
                                                          : nullptr);
        restoreMetal();
        g_envBakeInProgress = false;
        if (!ok) return nullptr;
        v.isCube = true;
        v.W = v.H = faceRes;
        v.numMips = EnvPanoLinear::kMaxMips;
        // A2 retention: snapshot the pristine static colour (level0) BEFORE the
        // overlay ever writes back; staticFaceZ was filled by the bake above.
        if (retainStatic) {
            store->envDynamic = true;
            store->staticColorMaster = store->levels[0];
        }
        dumpCubeStoreCensus(skipMat ? skipMat->Name : nullptr,
                            store->levels[0], faceRes);
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
    // ── --env_bake_sh_first ────────────────────────────────────────────────
    // WHAT AMBIENT DOES A REFLECTION PROBE CONTAIN? By default: the FLAT
    // Sc->Ambient constant, because renderFrame calls this function
    // (RENDER.CPP:491) BEFORE SHAmbient_EnsureBaked (RENDER.CPP:511), and
    // SHAmbient_Coeffs returns null until SHProbe::baked is set — so every
    // face below is shaded through the kernel's flat-ambient fallback
    // (DeferredSurfaceKernel.cpp:1799) instead of its SH branch (:1789).
    // Greets authors that constant as (32,32,32), an ACHROMATIC grey, while
    // the shipped frame's ambient is coloured and directional. That is a
    // per-channel difference in what the probe holds, and it is upstream of
    // every compose-side conductor fix.
    //
    // The order is incidental: SHAmbient_EnsureBaked reads nothing this
    // function produces (it derives its own scene AABB), and the recursion
    // guards are symmetric, so it is legal either way. Flipping it here rather
    // than in RENDER.CPP keeps the change inside the bake's own file.
    if (fds::FeatureFlags::env_bake_sh_first() && fds::FeatureFlags::sh_ambient()) {
        // Its own return value matters: if it baked, the scene's transform
        // state has been through a nested render, and the caller's post-bake
        // Transform_Objects/Radix_Sort re-run is keyed on OUR return value.
        // Fold it in, or a scene whose reflection probes are all cached would
        // do the SH bake and skip the re-transform.
        if (SHAmbient_EnsureBaked(sc)) bakedAny = true;
    }
    // Bake for every reflective material that lacks one; centroids within a
    // few world units share a store (clone materials, adjacent panels).
    for (Material* M = MatLib; M; M = M->Next) {
        if (M->RelScene != sc) continue;
        // EFFECTIVE tri-state (envEffModeFor): the per-material override
        // (Material::EnvReflMode) when set, else the scene-wide default
        // (env_refl_scene_mode flag / FLD-authored Scene::EnvReflSceneMode).
        // 1 forces a bake, -1 suppresses it, 0 keeps the historical rule.
        const int effMode = envEffModeFor(M);
        bool qualifies = (effMode > 0) ||
                         (effMode == 0 && (M->Reflection > 0.0f || M->MetallicMap));
        // ── ENVDYN screen-emiter UX gap (deliverable 3) ───────────────────
        // A surface the user flagged 'dynamic env' (Material::EnvDynamic) but
        // that does NOT otherwise qualify for a probe (no envRefl force, no
        // Reflection, no metallic map) would silently get no probe → the
        // dynamic overlay has nothing to composite into → the flag is a no-op.
        // envDynamic MEANS "I want the mech reflected here", so it implies an
        // env-refl opt-in: treat it like a force-bake (effMode>0), which is
        // exactly EnvReflMode==1's semantics — the env term's strength still
        // comes from Reflection/metallic, so forcing a probe on a flat surface
        // costs a bake but changes nothing visible until reflectivity is dialed
        // in (same caveat as the force-bake path). Gated on --env_dynamic so
        // the flag stays fully inert (byte-null) when the feature is off. The
        // ONE case we do NOT override: env-refl EXPLICITLY suppressed
        // (effMode<0) — that is a real authoring conflict, so we honour the
        // off and warn instead of baking behind the user's back.
        if (!qualifies && M->EnvDynamic) {
            static std::vector<const Material*> warned;   // one line per material
            const bool first = std::find(warned.begin(), warned.end(), M) == warned.end();
            if (first) warned.push_back(M);
            if (effMode >= 0 && fds::FeatureFlags::env_dynamic()) {
                qualifies = true;   // envDynamic implies a probe
                if (first)
                    std::fprintf(stderr, "[ENVDYN] '%s': envDynamic set on a "
                        "surface with no envRefl/Reflection/metallic — treating "
                        "the flag as an env-refl opt-in (forcing a probe) so the "
                        "dynamic overlay has a target\n", M->Name ? M->Name : "?");
            } else if (first) {
                std::fprintf(stderr, "[ENVDYN] WARNING: envDynamic set but no "
                    "probe%s — flag ignored: '%s' (add envRefl / a Reflection or "
                    "metallic map, or run --env_dynamic)\n",
                    effMode < 0 ? " (env-refl explicitly OFF for this surface)" : "",
                    M->Name ? M->Name : "?");
            }
        }
        if (!qualifies)
            continue;
        if (env.byMat.count(M)) continue;
        Vector c;
        float excludeR = 0.0f;
        if (!materialCentroid(sc, M, c, &excludeR)) { env.byMat[M] = -1; continue; }
        if (std::getenv("ENVDBG"))
            std::fprintf(stderr, "[ENVDBG] mat '%s' id=%u refl=%.0f metal=%d envDyn=%d reflMode=%d effMode=%d centroid (%.1f %.1f %.1f)\n",
                         M->Name ? M->Name : "?", (unsigned)M->ID, M->Reflection, M->MetallicMap ? 1 : 0,
                         (int)M->EnvDynamic, (int)M->EnvReflMode, envEffModeFor(M),
                         c.x, c.y, c.z);
        sceneAABB(sc, env);
        // Per-surface face-res wish (Material::EnvBakeRes, else the explicit
        // global env_bake_res; 0 = legacy sizing inside bakeStore). Cap it to
        // the editor/wasm ceiling BEFORE the largest-wins comparison below:
        // bakeStore records the capped size, so an uncapped wish would read as
        // "wish > have" every frame and re-bake the shared store forever.
        const int wantFace = capFaceRes(envFaceResForMat(M));
        int idx = -1;
        for (size_t i = 0; i < env.stores.size(); ++i) {
            const EnvPanoLinear& v = env.stores[i]->view;
            const float dx = v.bakeX - c.x, dy = v.bakeY - c.y, dz = v.bakeZ - c.z;
            if (dx*dx + dy*dy + dz*dz < 4.0f * 4.0f) { idx = int(i); break; }
        }
        // Probe SHARING vs per-surface res: stores are shared within 4 world
        // units, so two surfaces can wish DIFFERENT resolutions onto one
        // store — the LARGEST wins. A shared store smaller than this
        // material's wish is re-baked in place at the bigger size: same
        // center, same original self-exclusion (recorded provenance), only
        // the res changes. Imported stores (city per-building registrations)
        // keep their content — their res is decided at registration.
        if (idx >= 0 && wantFace > 0 && !env.stores[size_t(idx)]->imported) {
            EnvPanoStore& S = *env.stores[size_t(idx)];
            const int haveFace = S.view.isCube ? S.view.W : S.view.W / 2;
            if (wantFace > haveFace) {
                const Vector bc = { S.view.bakeX, S.view.bakeY, S.view.bakeZ };
                // Preserve the group's dynamic retention across the res upgrade
                // (S already flagged, or M flags it now). (ENVDYN A2)
                auto bigger = bakeStore(sc, env, bc, S.bakedSkipMat,
                                        S.bakedSkipR, wantFace,
                                        S.envDynamic || envDynamicForMat(M));
                if (bigger) {
                    bigger->bakedSkipMat = S.bakedSkipMat;
                    bigger->bakedSkipR   = S.bakedSkipR;
                    static const bool sGrid2 = std::getenv("FDS_ENV_GRID") != nullptr;
                    if (sGrid2) fillEnvDebugGrid(*bigger);
                    std::fprintf(stderr, "[ENVREFL] shared store re-baked %d->%d"
                                 " face res for '%s' (largest per-surface wish"
                                 " wins)\n", haveFace, wantFace,
                                 M->Name ? M->Name : "?");
                    env.stores[size_t(idx)] = std::move(bigger);
                    bakedAny = true;
                    // The replacement invalidated &view pointers published
                    // for THIS index earlier in the round (env_bake_fix's
                    // 1-bounce publication) — repoint them before any later
                    // bake reads the table. The full refresh below fixes the
                    // rest.
                    for (auto& [M2, i2] : env.byMat)
                        if (i2 == idx && M2->ID < 256 && envEffModeFor(M2) >= 0
                            && env.table[M2->ID])
                            env.table[M2->ID] = &env.stores[size_t(idx)]->view;
                }
            }
        }
        if (idx < 0) {
            auto store = bakeStore(sc, env, c, M, excludeR, wantFace,
                                   envDynamicForMat(M));   // ENVDYN A2 retention
            if (!store) { env.byMat[M] = -1; continue; }
            store->bakedSkipMat = M;
            store->bakedSkipR   = excludeR;
            if (store->envDynamic)
                std::fprintf(stderr, "[ENVDYN] retained static Z+colour master "
                             "for flagged probe '%s' (%zu KB)\n",
                             M->Name ? M->Name : "?",
                             (store->staticFaceZ.size() * sizeof(uint16_t) +
                              store->staticColorMaster.size() * sizeof(uint32_t)) / 1024);
            static const bool sGrid = std::getenv("FDS_ENV_GRID") != nullptr;
            if (sGrid) fillEnvDebugGrid(*store);
            // Say WHICH STORAGE was baked. The old wording ("baked NxN pano")
            // read as "equirect panorama" for every store, including the six
            // padded CUBE faces --env_cube (default ON) actually produces —
            // and two investigations chased a lat-long/cubemap mismatch that
            // did not exist because of this line.
            std::fprintf(stderr, "[ENVREFL] baked %s for '%s' at its centroid (%.1f %.1f %.1f)\n",
                         store->view.isCube
                            ? (std::string("6 padded CUBE faces of ")
                               + std::to_string(store->view.W) + "x"
                               + std::to_string(store->view.H) + " (+"
                               + std::to_string(store->view.numMips - 1)
                               + " mips)").c_str()
                            : (std::string("a ") + std::to_string(store->view.W)
                               + "x" + std::to_string(store->view.H)
                               + " EQUIRECT pano (+"
                               + std::to_string(store->view.numMips - 1)
                               + " mips)").c_str(),
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
    // Effective mode < 0 (forced off per-surface OR by the scene-wide
    // default) never publishes — even when a stale store for this material
    // still sits in byMat from before the override landed — so the kernel
    // sees no env term for it at all.
    std::memset(env.table, 0, sizeof(env.table));
    for (auto& [M, idx] : env.byMat)
        if (idx >= 0 && M->RelScene == sc && M->ID < 256 && envEffModeFor(M) >= 0) {
            env.table[M->ID] = &env.stores[size_t(idx)]->view;
            if (std::getenv("ENVDBG3") && M->Name && std::strstr(M->Name, "windows"))
                std::fprintf(stderr, "[ENVDBG3] table[%u] = store %d  mat=%p '%s'\n",
                             (unsigned)M->ID, idx, (void*)M, M->Name);
        }
    return bakedAny;
}

// ── Dynamic-mesh reflection overlay (--env_dynamic, ENVDYN A3) ─────────────
namespace {

// Tight world AABB of just the FACES that use material M — the owner-
// visibility gate input (fix for the whole-chunked-mesh WorldAabb_ForMaterial
// problem, see EnvPanoStore::ownerFaceAabb). Mirror-clone meshes excluded (the
// probe bake excludes them too). Same face walk + world transform as
// materialCentroid, so the bounds line up exactly with the probe's own
// geometry. Empty (valid=false) if no live face references M.
WorldAabb materialFaceAabb(Scene* sc, const Material* M) {
    WorldAabb out;
    if (!sc || !M) return out;
    std::vector<const TriMesh*> cloneMeshes;
    collectMirrorCloneMeshes(sc, cloneMeshes);
    float lo[3] = {  1e30f,  1e30f,  1e30f };
    float hi[3] = { -1e30f, -1e30f, -1e30f };
    for (TriMesh* T = sc->TriMeshHead; T; T = T->Next) {
        if (std::find(cloneMeshes.begin(), cloneMeshes.end(), T)
            != cloneMeshes.end()) continue;
        for (DWord i = 0; i < T->FIndex; ++i) {
            const Face& F = T->Faces[i];
            if (F.Txtr != M) continue;
            const Vertex* vs[3] = { F.A, F.B, F.C };
            for (int k = 0; k < 3; ++k) {
                if (!vs[k]) continue;
                Vector w;
                MatrixXVector(T->RotMat, const_cast<Vector*>(&vs[k]->Pos), &w);
                Vector_SelfAdd(&w, &T->IPos);
                const float p[3] = { w.x, w.y, w.z };
                for (int a = 0; a < 3; ++a) {
                    if (p[a] < lo[a]) lo[a] = p[a];
                    if (p[a] > hi[a]) hi[a] = p[a];
                }
                out.valid = true;
            }
        }
    }
    if (out.valid)
        for (int a = 0; a < 3; ++a) { out.mn[a] = lo[a]; out.mx[a] = hi[a]; }
    return out;
}

// ENVDYN A4 — the scheduler's PRIORITY INPUT: how much of the screen the
// probe's owner covers. Projects the owner faces' world AABB through the MAIN
// camera (the same 8-corner projection --draw_aabbs uses, so no new pass and
// no new convention) and returns the screen bbox area as a fraction of the
// viewport. This is an OVER-estimate twice over — an AABB around the faces,
// then a screen bbox around its projection — which is the right direction for
// a priority: it never under-rates a probe the camera is looking at.
// Straddling the near plane means the owner is effectively on top of the
// camera, so that case returns the whole screen rather than a meaningless box.
float ownerScreenAreaFrac(Scene* sc, const WorldAabb& b) {
    if (!b.valid || !View || XRes <= 0 || YRes <= 0) return 0.0f;
    const Matrix& Mat = View->Mat;
    const Vector  P   = View->ISource;
    const float nearZ = (sc && sc->NZP > 0.01f) ? sc->NZP : 0.01f;
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    int behind = 0;
    for (int ci = 0; ci < 8; ++ci) {
        const Vector w = { b.mn[0] + ((ci & 1) ? (b.mx[0] - b.mn[0]) : 0.0f),
                           b.mn[1] + ((ci & 2) ? (b.mx[1] - b.mn[1]) : 0.0f),
                           b.mn[2] + ((ci & 4) ? (b.mx[2] - b.mn[2]) : 0.0f) };
        Vector d = { w.x - P.x, w.y - P.y, w.z - P.z }, s;
        MatrixXVector(Mat, &d, &s);
        if (s.z <= nearZ) { ++behind; continue; }
        const float sx = CntrEX + FOVX * s.x / s.z;
        const float sy = CntrEY - FOVY * s.y / s.z;
        if (sx < x0) x0 = sx;  if (sx > x1) x1 = sx;
        if (sy < y0) y0 = sy;  if (sy > y1) y1 = sy;
    }
    if (behind == 8) return 0.0f;
    if (behind > 0)  return 1.0f;
    if (x0 < 0.0f) x0 = 0.0f;  if (x1 > float(XRes)) x1 = float(XRes);
    if (y0 < 0.0f) y0 = 0.0f;  if (y1 > float(YRes)) y1 = float(YRes);
    const float w = x1 - x0, h = y1 - y0;
    if (w <= 0.0f || h <= 0.0f) return 0.0f;
    return (w * h) / (float(XRes) * float(YRes));
}

// ENVDYN A4 — the deliverable's own instrument (--env_dyn_stats=N). "Jumpy" is
// a complaint about TIME, so the metric a scheduler change has to move is the
// distribution of UPDATE INTERVALS, not a frame diff. Prints one row per
// flagged probe every N overlay frames and resets. stderr only.
void envDynStatsMaybeReport(SceneEnv& env,
                            const std::chrono::high_resolution_clock::time_point& t0,
                            int statEvery, int sched, int budget) {
    if (statEvery <= 0) return;
    env.statMs += std::chrono::duration<double, std::milli>(
        std::chrono::high_resolution_clock::now() - t0).count();
    if (env.dynFrame - env.statFrom < uint32_t(statEvery)) return;
    const uint32_t span = env.dynFrame - env.statFrom;
    std::fprintf(stderr, "[ENVDYN-STATS] %u overlay frame(s), sched=%d "
                 "budget=%d %s, total overlay %.2f ms (%.3f ms/frame)\n",
                 span, sched, budget, sched >= 1 ? "face(s)/frame" : "probe(s)/frame",
                 env.statMs, env.statMs / double(span ? span : 1));
    for (auto& sp : env.stores) {
        EnvPanoStore& S = *sp;
        if (!S.envDynamic || !S.view.isCube || S.staticColorMaster.empty()) continue;
        std::sort(S.stGaps.begin(), S.stGaps.end());
        const double meanGap = S.stGaps.empty() ? 0.0
            : double(S.stGapSum) / double(S.stGaps.size());
        const uint32_t p50 = S.stGaps.empty() ? 0
            : S.stGaps[S.stGaps.size() / 2];
        const uint32_t p95 = S.stGaps.empty() ? 0
            : S.stGaps[size_t(0.95 * double(S.stGaps.size() - 1))];
        // CONTENT HASH over every mip level of the store. This is what proves
        // the touched-faces-only mip refilter in overlayComposite is
        // bit-identical to the old whole-cube one: run the same sweep with
        // both and the hashes must match probe for probe. A frame diff cannot
        // do that job — one snapshot exercises one overlay, and the divergence
        // (if any) would only appear after many partial updates.
        uint64_t hsh = 1469598103934665603ull;
        for (int k = 0; k < S.view.numMips; ++k)
            for (uint32_t t : S.levels[k]) {
                hsh ^= t; hsh *= 1099511628211ull;
            }
        std::fprintf(stderr, "[ENVDYN-STATS]   %-22s want %4u (vis %4u)  upd %4u"
            "  faces %4u  interval mean %5.2f p50 %3u p95 %3u max %4u"
            "  visStall %2u  POP n %3u mean %6.1f MAX %4u  store %016llx\n",
            S.bakedSkipMat && S.bakedSkipMat->Name ? S.bakedSkipMat->Name : "?",
            S.stWant, S.stWantVisible, S.stUpdates, S.stFaces,
            meanGap, p50, p95, S.stGapMax, S.stVisStale,
            S.stPopN, S.stPopN ? double(S.stPopSum) / double(S.stPopN) : 0.0,
            S.stPopMax, (unsigned long long)hsh);
        S.stWant = S.stWantVisible = S.stUpdates = S.stFaces = 0;
        S.stGapSum = S.stGapMax = S.stVisStale = 0;
        S.stPopN = S.stPopSum = S.stPopMax = 0;
        S.stGaps.clear();
    }
    env.statFrom = env.dynFrame;
    env.statMs = 0.0;
}

// Render the dynamic meshes into store S's touched cube faces and composite
// them over S's retained static master (A2). Occlusion vs static geometry is
// the per-face depth compare (ZPage16: larger zEnc = nearer, 0 = untouched).
// Returns the count of texels where the live mech won the depth compare and
// was composited over the static master — the headless "is the mech actually
// in the reflection now?" metric (0 = nothing composited).
int overlayComposite(Scene* sc, EnvPanoStore& S, const bool faceMask[6]) {
    const int fr = S.view.W;
    const Vector bake = { S.view.bakeX, S.view.bakeY, S.view.bakeZ };
    std::vector<uint32_t> mech[6];
    std::vector<uint16_t> mechZ[6];
    // g_envBakeInProgress: block any re-entrant FramePrep/SH bake while the
    // overlay renders (belt-and-braces atop the g_offscreenViewDepth gate).
    g_envBakeInProgress = true;
    const bool ok = renderSixFaces(sc, bake, fr, fds::EnvCube_FaceFovDegrees(),
                                   0xFF202020u, mech,
                                   /*skipMat=*/nullptr, /*skipRadius=*/0.0f,
                                   /*publishProj=*/true, mechZ,
                                   /*dynamicOnly=*/true, faceMask,
                                   /*linearCapture=*/fds::FeatureFlags::env_bake_linear());
    g_envBakeInProgress = false;
    if (!ok) return 0;
    int mechTexels = 0, mechRendered = 0;
    for (int f = 0; f < 6; ++f) {
        if (!faceMask[f] || mechZ[f].empty()) continue;
        uint32_t* dst          = S.levels[0].data()          + size_t(f) * fr * fr;
        const uint32_t* master = S.staticColorMaster.data()  + size_t(f) * fr * fr;
        const uint16_t* sZ     = S.staticFaceZ.data()        + size_t(f) * fr * fr;
        const uint32_t* mc     = mech[f].data();
        const uint16_t* mZ     = mechZ[f].data();
        for (int i = 0; i < fr * fr; ++i) {
            const bool rendered = (mZ[i] != 0);
            const bool win = (rendered && mZ[i] >= sZ[i]);
            dst[i] = win ? mc[i] : master[i];
            mechTexels   += win ? 1 : 0;
            mechRendered += rendered ? 1 : 0;
        }
    }
    // Distinguish "mech never rasterised into the probe" (relevance/coverage)
    // from "mech rendered but lost the depth test" (occluded by static geometry
    // between the probe and the mech).
    static const bool sProf = std::getenv("FDS_ENVDYN_PROF") != nullptr;
    if (sProf && mechRendered > mechTexels)
        std::fprintf(stderr, "[ENVDYN-WHY]   ...mech rasterised %d texel(s) but "
            "%d were OCCLUDED by static geometry nearer the probe (%d survived)\n",
            mechRendered, mechRendered - mechTexels, mechTexels);
    // Per-face mip refilter (REQUIRED — rough surfaces sample mips; without it
    // the mech vanishes on rough metals). level0 was composited in place;
    // rebuild 1..3 and re-point the view pointers.
    //
    // ONLY THE TOUCHED FACES. boxDownsampleCube filters each face
    // independently (the padding ring means the 2x2 box at a face edge already
    // averages valid neighbour content, so the filter never crosses a face
    // boundary) — therefore level k of face f depends ONLY on level k-1 of
    // face f, and refiltering an untouched face reproduces the texels already
    // there — proved, not asserted: --env_dyn_stats prints an FNV hash of every
    // store's whole mip chain, and over a 400-frame greets sweep (t=3800..5400,
    // 346 probe updates, 931 faces, five probes) all five hashes are IDENTICAL
    // to the whole-cube version. A frame diff could not have shown that; one
    // snapshot exercises one overlay.
    //
    // Worth MEASURED 3.372 -> 3.076 ms/frame of overlay on that sweep, i.e.
    // 0.34 ms per probe update — real, but note it does NOT make the refilter
    // the dominant cost: at 2.7 faces per update the face RENDER is, which is
    // why the scheduler's budget is a FACE budget.
    {
        int frl = fr;
        for (int k = 1; k < EnvPanoLinear::kMaxMips; ++k) {
            const int dfr = frl >> 1;
            if (S.levels[k].size() != size_t(6) * dfr * dfr)
                S.levels[k].assign(size_t(6) * dfr * dfr, 0);
            for (int f = 0; f < 6; ++f) {
                if (!faceMask[f]) continue;
                const uint32_t* sp = S.levels[k-1].data() + size_t(f) * frl * frl;
                uint32_t* dp = S.levels[k].data() + size_t(f) * dfr * dfr;
                for (int y = 0; y < dfr; ++y)
                    for (int x = 0; x < dfr; ++x) {
                        const uint32_t a = sp[size_t(2*y)   * frl + 2*x];
                        const uint32_t b = sp[size_t(2*y)   * frl + 2*x + 1];
                        const uint32_t c = sp[size_t(2*y+1) * frl + 2*x];
                        const uint32_t d = sp[size_t(2*y+1) * frl + 2*x + 1];
                        uint32_t out = 0;
                        for (int sh8 = 0; sh8 < 32; sh8 += 8) {
                            const uint32_t s = ((a >> sh8) & 0xFF) + ((b >> sh8) & 0xFF)
                                             + ((c >> sh8) & 0xFF) + ((d >> sh8) & 0xFF);
                            out |= ((s + 2) >> 2) << sh8;
                        }
                        dp[size_t(y) * dfr + x] = out;
                    }
            }
            frl = dfr;
        }
    }
    for (int k = 0; k < EnvPanoLinear::kMaxMips; ++k)
        S.view.mip[k] = S.levels[k].data();
    return mechTexels;
}

}  // namespace

void EnvDynamic_Overlay(Scene* sc) {
    if (!sc || g_envBakeInProgress) return;
    if (!fds::FeatureFlags::env_dynamic()) return;
    if (g_offscreenViewDepth > 0) return;             // main-camera path only
    const int budget = fds::FeatureFlags::env_dynamic_budget();
    if (budget <= 0) return;
    auto it = g_envByScene.find(sc);
    if (it == g_envByScene.end() || it->second.stores.empty()) return;
    SceneEnv& env = it->second;

    // FDS_ENVDYN_PROF (deliverable 1): per-frame WHY trace — for every authored
    // envDynamic material, log why its probe did / did not overlay this frame.
    // Gated on the existing env var (no new FeatureFlags entry): stderr only,
    // zero effect on rendered output → the flag-off byte-null contract holds.
    static const bool sProf = std::getenv("FDS_ENVDYN_PROF") != nullptr;
    if (sProf) {
        for (Material* M = MatLib; M; M = M->Next) {
            if (M->RelScene != sc || !M->EnvDynamic) continue;
            const char* nm = M->Name ? M->Name : "?";
            auto jt = env.byMat.find(M);
            if (jt == env.byMat.end()) {
                std::fprintf(stderr, "[ENVDYN-WHY] '%s': NO-STORE (envDynamic set but"
                    " no probe — did the material qualify for env-refl? envRefl/"
                    "Reflection>0/metallic, and --env_cube on?)\n", nm);
                continue;
            }
            if (jt->second < 0) {
                std::fprintf(stderr, "[ENVDYN-WHY] '%s': NO-STORE (no centroid / no"
                    " faces reference this material)\n", nm);
                continue;
            }
            const EnvPanoStore& S = *env.stores[size_t(jt->second)];
            const char* bad =
                !S.view.isCube                 ? "NOT-RETAINED (equirect/imported store — no cube faces / retained depth)"
              : !S.envDynamic                  ? "NOT-RETAINED (store owner is a non-flagged material sharing this centroid — dedup owner mismatch)"
              : S.staticColorMaster.empty()    ? "NOT-RETAINED (static colour master empty)"
              : S.staticFaceZ.empty()          ? "NOT-RETAINED (static depth empty)"
              : nullptr;
            if (bad)
                std::fprintf(stderr, "[ENVDYN-WHY] '%s': store %d %s\n", nm, jt->second, bad);
            else
                std::fprintf(stderr, "[ENVDYN-WHY] '%s': store %d RETAINED, "
                    "bake@(%.1f,%.1f,%.1f) faceRes %d — relevance evaluated below\n",
                    nm, jt->second, S.view.bakeX, S.view.bakeY, S.view.bakeZ, S.view.W);
        }
    }

    // Cheap pre-scan: any flagged, retained cube store? (byte-null fast exit
    // when the scene has none — nothing authored.)
    bool anyFlagged = false;
    for (auto& s : env.stores)
        if (s->envDynamic && s->view.isCube && !s->staticColorMaster.empty()) {
            anyFlagged = true; break;
        }
    if (!anyFlagged) return;

    // Gather the movers' world-space bspheres (same dynamic predicate as F /
    // the bakes). ObjectHead walk for the parent-chain test.
    struct DynBS { float c[3]; float r; };
    std::vector<DynBS> movers;
    for (Object* Obj = sc->ObjectHead; Obj; Obj = Obj->Next) {
        if (Obj->Type != Obj_TriMesh) continue;
        TriMesh* T = (TriMesh*)Obj->Data;
        if (!T || T->FIndex == 0) continue;
        if (!fds::WorldAabb_MeshIsDynamic(Obj)) continue;
        Vector wc;
        MatrixXVector(T->RotMat, &T->BSphereCtr, &wc);
        wc.x += T->IPos.x; wc.y += T->IPos.y; wc.z += T->IPos.z;
        movers.push_back({ { wc.x, wc.y, wc.z }, T->BSphereRadius });
        if (sProf)
            std::fprintf(stderr, "[ENVDYN-WHY] mover '%s' bsphere c=(%.1f,%.1f,%.1f) r=%.1f\n",
                Obj->Name ? Obj->Name : "?", wc.x, wc.y, wc.z, T->BSphereRadius);
    }
    if (movers.empty()) {
        if (sProf) std::fprintf(stderr, "[ENVDYN-WHY] NO MOVERS this frame — "
            "no mesh passed WorldAabb_MeshIsDynamic (mech Pos/Rotate spline static?)\n");
        return;   // no movers this frame → nothing to reflect
    }

    // Owner-visibility gate needs current world AABBs + the main frustum.
    fds::WorldAabb_UpdateScene(sc);
    const Frustum cam = fds::Frustum_FromMainCamera(sc);

    // Per-face pyramid far range: the scene box diagonal (the mech is always
    // inside the scene). The 4 side planes do the angular cull; the far plane
    // only bounds pathological far spheres.
    sceneAABB(sc, env);
    float diag2 = 0.0f;
    for (int a = 0; a < 3; ++a) { const float e = env.boxMax[a] - env.boxMin[a]; diag2 += e * e; }
    const float range = std::sqrt(diag2) * 2.0f + 1.0f;

    const auto tProf0 = std::chrono::high_resolution_clock::now();
    ++env.dynFrame;
    const int  sched     = fds::FeatureFlags::env_dyn_sched();
    const int  statEvery = fds::FeatureFlags::env_dyn_stats();

    static int cursor = 0;   // round-robin start so >budget probes share frames
    const size_t N = env.stores.size();
    int processed = 0, facesTotal = 0;

    // ── ENVDYN A4: SCREEN-PRIORITY FACE SCHEDULING (--env_dyn_sched=1) ─────
    // The legacy path below is left byte-for-byte intact and is still the
    // default. See the flag help for why the legacy one updates unevenly:
    // the cursor walks ALL stores (not just the flagged ones) and advances by
    // processed+1, an off-screen owner is skipped OUTRIGHT (unbounded
    // staleness → a pop when the camera turns onto it), and the budget is
    // whole probes regardless of how much screen the owner covers.
    if (sched >= 1) {
        const int faceBudget = fds::FeatureFlags::env_dyn_face_budget();
        const int maxStall   = fds::FeatureFlags::env_dyn_max_stall();
        const int offPeriod  = fds::FeatureFlags::env_dyn_offscreen_period();
        struct Cand {
            size_t   si;
            int      tier;     // 0 = never overlaid, 1 = visible+stalled,
                               // 2 = visible, 3 = off-screen refresh
            float    key;      // ordering WITHIN the tier, descending
            float    area;
            uint32_t gap;
            int      touched;
            bool     onScreen;
            bool     mask[6];
        };
        std::vector<Cand> cands;
        cands.reserve(N);
        for (size_t si = 0; si < N; ++si) {
            EnvPanoStore& S = *env.stores[si];
            if (!S.envDynamic || !S.view.isCube || S.staticColorMaster.empty()) continue;
            const char* nm = S.bakedSkipMat && S.bakedSkipMat->Name ? S.bakedSkipMat->Name : "?";
            if (S.bakedSkipMat && !S.ownerFaceAabbDone) {
                S.ownerFaceAabb = materialFaceAabb(sc, S.bakedSkipMat);
                S.ownerFaceAabbDone = true;
            }
            const WorldAabb& owner = S.ownerFaceAabb;
            // NOT a hard gate any more — off-screen only demotes.
            const bool onScreen = !(owner.valid && fds::Frustum_CullsAabb(cam, owner));
            // Mover relevance + touched faces. A probe no mover reaches has
            // nothing to overlay, so it is not a candidate at any priority.
            Cand c{}; c.si = si; c.onScreen = onScreen;
            for (int f = 0; f < 6; ++f) {
                const Frustum fp = fds::Frustum_FromProbeFace(
                    (const float[3]){ S.view.bakeX, S.view.bakeY, S.view.bakeZ }, f, range);
                for (const auto& m : movers)
                    if (!fds::Frustum_CullsSphere(fp, m.c, m.r)) {
                        c.mask[f] = true; ++c.touched; break;
                    }
            }
            if (!c.touched) continue;
            ++S.stWant;
            if (onScreen) ++S.stWantVisible;
            if (onScreen && !S.stPrevVisible && S.everOverlaid) {
                const uint32_t pop = env.dynFrame - S.lastOverlay;
                ++S.stPopN; S.stPopSum += pop;
                if (pop > S.stPopMax) S.stPopMax = pop;
            }
            S.stPrevVisible = onScreen;
            // Priority: TIER first, then the tier's own key. Screen area is the
            // owner's world AABB projected through the MAIN camera.
            c.area = onScreen ? ownerScreenAreaFrac(sc, owner) : 0.0f;
            c.gap  = S.everOverlaid ? (env.dynFrame - S.lastOverlay) : 0u;
            if (!S.everOverlaid) {
                c.tier = 0; c.key = 0.0f;                    // must run once
            } else if (onScreen && maxStall > 0 && int(c.gap) >= maxStall) {
                c.tier = 1; c.key = float(c.gap);            // starvation guard
            } else if (onScreen) {
                // Proportional share: in steady state gap ends up inversely
                // proportional to area, i.e. a probe with 4x the screen gets
                // 4x the updates — which is exactly "spend the budget where
                // it is visible" without ever being the ONLY rule (tier 1 caps
                // what that costs the small ones).
                c.tier = 2; c.key = c.area * float(c.gap);
            } else if (offPeriod > 0 && int(c.gap) >= offPeriod) {
                c.tier = 3; c.key = float(c.gap);            // bounded staleness
            } else {
                continue;    // off-screen and fresh enough — NOT a candidate
            }
            if (sProf) std::fprintf(stderr, "[ENVDYN-SCHED] '%s' (store %zu): "
                "onScreen=%d area=%.5f gap=%u touched=%d -> tier %d key %.6f\n",
                nm, si, int(onScreen), double(c.area), c.gap, c.touched,
                c.tier, double(c.key));
            cands.push_back(c);
        }
        // Lowest tier first, then highest key; ties broken by store index
        // (stable_sort over an index-ordered vector), so the schedule is a pure
        // function of the frame and reproduces run to run.
        std::stable_sort(cands.begin(), cands.end(),
                         [](const Cand& a, const Cand& b) {
                             if (a.tier != b.tier) return a.tier < b.tier;
                             return a.key > b.key;
                         });
        int remaining = faceBudget;
        for (const Cand& c : cands) {
            // The PROBE cap (env_dynamic_budget) still binds alongside the face
            // budget. Keeping it is what makes this scheduler spend the SAME
            // budget as the legacy path and change only WHERE it goes — which
            // is the whole claim, and it has to be true for the before/after
            // interval comparison to mean anything.
            if (processed >= budget) break;
            // Whole-probe-at-a-time: a partially updated cube would show the
            // mech at TWO positions across its own faces. touched <= 6 <= the
            // default face budget, so the top-priority probe is never truncated.
            if (c.touched > remaining) continue;
            EnvPanoStore& S = *env.stores[c.si];
            const int mechTexels = overlayComposite(sc, S, c.mask);
            remaining -= c.touched;
            ++processed;
            facesTotal += c.touched;
            if (S.everOverlaid) {
                const uint32_t gap = env.dynFrame - S.lastOverlay;
                S.stGapSum += gap;
                if (gap > S.stGapMax) S.stGapMax = gap;
                S.stGaps.push_back(gap);
            }
            S.lastOverlay = env.dynFrame;
            S.everOverlaid = true;
            ++S.stUpdates;
            S.stFaces += uint32_t(c.touched);
            S.stVisStreak = 0;
            if (sProf) std::fprintf(stderr, "[ENVDYN-SCHED] '%s' (store %zu): OK — "
                "%d face(s), %d mech texel(s), %d face budget left\n",
                S.bakedSkipMat && S.bakedSkipMat->Name ? S.bakedSkipMat->Name : "?",
                c.si, c.touched, mechTexels, remaining);
        }
        // Worst-case bookkeeping: a run of frames where the owner was ON SCREEN
        // and wanted an update but did not get one is exactly what a viewer
        // sees as a stale reflection.
        for (const Cand& c : cands) {
            EnvPanoStore& S = *env.stores[c.si];
            if (!c.onScreen) continue;
            if (S.lastOverlay == env.dynFrame) continue;
            if (++S.stVisStreak > S.stVisStale) S.stVisStale = S.stVisStreak;
        }
        envDynStatsMaybeReport(env, tProf0, statEvery, sched, faceBudget);
        return;
    }

    // Loop bound: normally stop once the budget is spent (byte-identical to the
    // original). Under sProf keep scanning so BUDGET-DEFERRED probes are still
    // reported — the extra iterations only run the (cheap) gates, never
    // overlayComposite, so the rendered output is unchanged. `statEvery` widens
    // it the same way, so the legacy arm's histogram counts the frames a probe
    // WANTED an update as well as the ones it got.
    for (size_t k = 0; k < N && (processed < budget || sProf || statEvery > 0); ++k) {
        const size_t si = (size_t(cursor) + k) % N;
        EnvPanoStore& S = *env.stores[si];
        if (!S.envDynamic || !S.view.isCube || S.staticColorMaster.empty()) continue;
        const char* nm = S.bakedSkipMat && S.bakedSkipMat->Name ? S.bakedSkipMat->Name : "?";
        bool ownerOff = false;   // --env_dyn_stats: deferred owner-gate skip

        // (1) owner-visibility gate: the OWNING FACES' world AABB vs the camera
        // frustum — offscreen owner ⇒ skip (so the budget goes to the probes
        // the camera can actually see). Uses the per-faces bounds, NOT
        // WorldAabb_ForMaterial's whole-chunked-mesh union which is the entire
        // scene box for every greets flagged material (see ownerFaceAabb).
        // Cached: the reflective owner geometry is static.
        if (S.bakedSkipMat) {
            if (!S.ownerFaceAabbDone) {
                S.ownerFaceAabb = materialFaceAabb(sc, S.bakedSkipMat);
                S.ownerFaceAabbDone = true;
            }
            const WorldAabb& owner = S.ownerFaceAabb;
            if (sProf && owner.valid) std::fprintf(stderr, "[ENVDYN-WHY] '%s' "
                "(store %zu): owner-faces AABB [%.1f,%.1f,%.1f]..[%.1f,%.1f,%.1f] "
                "(extent %.1f x %.1f x %.1f)\n", nm, si,
                owner.mn[0], owner.mn[1], owner.mn[2],
                owner.mx[0], owner.mx[1], owner.mx[2],
                owner.mx[0]-owner.mn[0], owner.mx[1]-owner.mn[1], owner.mx[2]-owner.mn[2]);
            if (owner.valid && fds::Frustum_CullsAabb(cam, owner)) {
                if (sProf) std::fprintf(stderr, "[ENVDYN-WHY] '%s' (store %zu): "
                    "OWNER-OFFSCREEN — owner faces outside the camera frustum, skip\n", nm, si);
                // Under --env_dyn_stats the skip is DEFERRED past the relevance
                // test so the histogram counts the frames this probe WANTED an
                // update as well as the ones it got — otherwise the legacy arm
                // and the scheduler arm would have different denominators and
                // the before/after comparison would be meaningless. The probe
                // is still never composited here: `ownerOff` skips below.
                if (statEvery <= 0) continue;
                ownerOff = true;
            }
        }

        // (2) mech relevance + which cube faces the movers touch (padded-face
        // pyramid cull, Foundation F).
        const float bake[3] = { S.view.bakeX, S.view.bakeY, S.view.bakeZ };
        bool faceMask[6] = {}; bool anyTouch = false;
        for (int f = 0; f < 6; ++f) {
            const Frustum fp = fds::Frustum_FromProbeFace(bake, f, range);
            for (const auto& m : movers)
                if (!fds::Frustum_CullsSphere(fp, m.c, m.r)) {
                    faceMask[f] = true; anyTouch = true; break;
                }
        }
        if (!anyTouch) {
            if (sProf) std::fprintf(stderr, "[ENVDYN-WHY] '%s' (store %zu): "
                "NO-MOVER-RELEVANCE — no mover sphere falls in any of the probe's "
                "6 padded-face pyramids from bake@(%.1f,%.1f,%.1f), skip\n",
                nm, si, bake[0], bake[1], bake[2]);
            continue;
        }
        if (statEvery > 0) {
            ++S.stWant;
            if (!ownerOff) ++S.stWantVisible;
            if (!ownerOff && !S.stPrevVisible && S.everOverlaid) {
                const uint32_t pop = env.dynFrame - S.lastOverlay;
                ++S.stPopN; S.stPopSum += pop;
                if (pop > S.stPopMax) S.stPopMax = pop;
            }
            S.stPrevVisible = !ownerOff;
        }
        if (ownerOff) continue;   // deferred owner-gate skip (stats only)

        // (3) render dynamic-only into the touched faces + composite + refilter.
        // Budget cap: when spent, later relevant probes wait for a future frame
        // (round-robin). Under sProf we still reach here to REPORT the defer.
        if (processed >= budget) {
            if (sProf) std::fprintf(stderr, "[ENVDYN-WHY] '%s' (store %zu): "
                "BUDGET-DEFERRED — relevant but budget %d already spent this "
                "frame (round-robins in next frame)\n", nm, si, budget);
            if (statEvery > 0 && ++S.stVisStreak > S.stVisStale)
                S.stVisStale = S.stVisStreak;
            continue;
        }
        const int mechTexels = overlayComposite(sc, S, faceMask);
        ++processed;
        int nf = 0; for (int f = 0; f < 6; ++f) nf += faceMask[f] ? 1 : 0;
        facesTotal += nf;
        if (statEvery > 0) {
            if (S.everOverlaid) {
                const uint32_t gap = env.dynFrame - S.lastOverlay;
                S.stGapSum += gap;
                if (gap > S.stGapMax) S.stGapMax = gap;
                S.stGaps.push_back(gap);
            }
            ++S.stUpdates;
            S.stFaces += uint32_t(nf);
            S.stVisStreak = 0;
        }
        S.lastOverlay  = env.dynFrame;
        S.everOverlaid = true;
        if (sProf) std::fprintf(stderr, "[ENVDYN-WHY] '%s' (store %zu): OK — "
            "overlaid the mech into %d touched face(s), %d mech texel(s) "
            "composited over static%s\n", nm, si, nf, mechTexels,
            mechTexels == 0 ? " (mech occluded / off-probe — nothing visible)" : "");
    }
    if (N) cursor = int((size_t(cursor) + size_t(processed) + 1) % N);

    if (sProf && processed > 0) {
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::high_resolution_clock::now() - tProf0).count();
        std::fprintf(stderr, "[ENVDYN-PROF] overlay: %d probe(s), %d face(s), "
                     "%d mover(s), faceRes %d -> %.3f ms\n",
                     processed, facesTotal, (int)movers.size(),
                     env.stores.empty() ? 0 : env.stores[0]->view.W, ms);
    }
    envDynStatsMaybeReport(env, tProf0, statEvery, sched, budget);
}

// Availability probe for the runtime viz cycle (FDS/RENDER/VizCycle.cpp):
// mirrors DrawViz's own early-outs, so the cycle never offers the pano viewer
// when this run baked no panorama (--env_refl on is not sufficient).
bool EnvReflectionViz_Available() {
    if (!CurScene) return false;
    auto it = g_envByScene.find(CurScene);
    if (it == g_envByScene.end() || it->second.stores.empty()) return false;
    return it->second.stores[0]->view.mip[0] != nullptr;
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
    // Res override precedence: the per-surface Material::EnvBakeRes wish,
    // then an explicit global --env-bake-res, then the caller's storeRes
    // (CITY passes a hardcoded 256). Everything is capped at faceRes below —
    // the source faces are what they are (CITY bakes 512²); we only ever
    // downsample. NOTE these stores are per-BUILDING and materials map n:1
    // onto them: the FIRST windows clone registering a building sizes its
    // store; later clones of the same building alias it
    // (EnvReflection_AliasMaterial) regardless of their own wish — in
    // practice the clones inherit one base surface's EnvBakeRes anyway.
    if (M->EnvBakeRes > 0) {
        const int face = envFaceResForMat(M);
        static bool notedMat = false;
        if (!notedMat && face != storeRes) {
            notedMat = true;
            std::fprintf(stderr, "[ENVREFL] envBakeRes=%d on '%s' overrides"
                         " registered-store res (caller asked %d^2; capped at"
                         " the %d^2 source faces)\n", face,
                         M->Name ? M->Name : "?", storeRes, faceRes);
        }
        storeRes = face;
    } else if (const int face = envBakeResOverride(sc)) {
        static bool noted = false;
        if (!noted && face != storeRes) {
            noted = true;
            std::fprintf(stderr, "[ENVREFL] env bake res %d overrides"
                         " registered-store res (caller asked %d^2; capped at"
                         " the %d^2 source faces)\n", face, storeRes, faceRes);
        }
        storeRes = face;
    }
    if (storeRes > faceRes) storeRes = faceRes;
    SceneEnv& env = g_envByScene[sc];
    auto store = std::make_unique<EnvPanoStore>();
    store->imported = true;   // caller-provided faces — never re-baked here
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

// ── SH irradiance ambient (--sh_ambient) ──────────────────────────────────
namespace {

struct SHProbe {
    bool  baked = false;
    float c[27] = {};   // channel-major: B[0..8], G[9..17], R[18..26]
};
std::map<Scene*, SHProbe> g_shByScene;

// Face resolution for the SH capture. SH-L2 is a very low-frequency signal —
// a 32² cube is far more than the 9 coefficients can represent, so this is
// plenty (and the six full-pipeline face renders are the cost, not the
// projection). One-shot per scene, cached.
constexpr int kSHFaceRes = 32;

// Real L2 SH basis (with normalisation constants), evaluated at unit dir
// (x,y,z). Order matches SHAmbient_Coeffs / the kernel's shEval.
inline void shBasis9(float x, float y, float z, float Y[9]) {
    Y[0] = 0.282095f;
    Y[1] = 0.488603f * y;
    Y[2] = 0.488603f * z;
    Y[3] = 0.488603f * x;
    Y[4] = 1.092548f * (x * y);
    Y[5] = 1.092548f * (y * z);
    Y[6] = 0.315392f * (3.0f * z * z - 1.0f);
    Y[7] = 1.092548f * (x * z);
    Y[8] = 0.546274f * (x * x - y * y);
}

}  // namespace

const float* SHAmbient_Coeffs(Scene* sc) {
    auto it = g_shByScene.find(sc);
    return (it != g_shByScene.end() && it->second.baked) ? it->second.c : nullptr;
}

bool SHAmbient_EnsureBaked(Scene* sc) {
    if (!sc || g_envBakeInProgress) return false;
    SHProbe& p = g_shByScene[sc];
    if (p.baked) return false;

    // Probe centre = scene AABB centre (reuses the tested clone-aware AABB the
    // env-reflection parallax proxy uses). An interior room's centre sees the
    // floor below and walls/ceiling around — exactly the directional split SH
    // captures.
    SceneEnv& env = g_envByScene[sc];
    sceneAABB(sc, env);
    Vector center;
    center.x = (env.boxMin[0] + env.boxMax[0]) * 0.5f;
    center.y = (env.boxMin[1] + env.boxMax[1]) * 0.5f;
    center.z = (env.boxMin[2] + env.boxMax[2]) * 0.5f;

    EnvBakeParams params;   // default voidColor for untouched (sky-less) texels
    std::vector<uint32_t> faces[6];
    g_envBakeInProgress = true;
    // publishProj=true → each face rendered with a real 90° projection (not
    // the main window's telephoto FOV); skipMat=null → whole scene captured.
    // linearCapture is --sh_bake_linear, NOT --env_bake_linear: this probe is
    // the SH AMBIENT source, and until 2026-08-08 the two shared one flag (see
    // renderSixFaces' linearCapture comment and SHADING_CONTRACT §8 row M2).
    const bool ok = renderSixFaces(sc, center, kSHFaceRes, 90.0f,
                                   params.voidColor, faces,
                                   /*skipMat=*/nullptr, /*skipRadius=*/0.0f,
                                   /*publishProj=*/true, /*facesZOut=*/nullptr,
                                   /*dynamicOnly=*/false, /*faceMask=*/nullptr,
                                   /*linearCapture=*/fds::FeatureFlags::sh_bake_linear());
    g_envBakeInProgress = false;
    if (!ok) return false;

    // Project the six faces to 9 RGB SH coefficients. Per texel: direction is
    // the inverse of sampleCube's (sx,sy)→(a,b) mapping; the differential
    // solid angle ∝ (1+a²+b²)^-1.5 (orthonormal face basis), normalised so the
    // total over the cube is 4π.
    const int res = kSHFaceRes;
    double acc[27] = {};
    double wsum = 0.0;
    for (int f = 0; f < 6; ++f) {
        const CubeFace& cf = kCubeFaces[f];
        for (int sy = 0; sy < res; ++sy) {
            const float b = 1.0f - 2.0f * (float(sy) + 0.5f) / float(res);
            for (int sx = 0; sx < res; ++sx) {
                const float a = 2.0f * (float(sx) + 0.5f) / float(res) - 1.0f;
                float dx = cf.fwd.x + a * cf.right.x + b * cf.up.x;
                float dy = cf.fwd.y + a * cf.right.y + b * cf.up.y;
                float dz = cf.fwd.z + a * cf.right.z + b * cf.up.z;
                const float ilen = 1.0f / std::sqrt(dx * dx + dy * dy + dz * dz);
                dx *= ilen; dy *= ilen; dz *= ilen;
                const float w = ilen * ilen * ilen;   // (1+a²+b²)^-1.5
                const uint32_t col = faces[f][size_t(sy) * res + sx];
                const float B = float(col & 0xFF);
                const float G = float((col >> 8) & 0xFF);
                const float R = float((col >> 16) & 0xFF);
                float Y[9];
                shBasis9(dx, dy, dz, Y);
                for (int i = 0; i < 9; ++i) {
                    const double wy = double(w) * double(Y[i]);
                    acc[i]      += double(B) * wy;
                    acc[9 + i]  += double(G) * wy;
                    acc[18 + i] += double(R) * wy;
                }
                wsum += double(w);
            }
        }
    }
    // Normalise the solid-angle weights to sum 4π, then fold the cosine-
    // convolution A_l divided by π (A0/π=1, A1/π=2/3, A2/π=1/4) into each
    // coefficient so E(n)=Σ c_i Y_i(n) is a direct, env-colour-scaled
    // irradiance (uniform env → its own colour).
    const double norm = wsum > 0.0 ? (4.0 * M_PI) / wsum : 0.0;
    static const double Al[9] = { 1.0,
                                  2.0 / 3.0, 2.0 / 3.0, 2.0 / 3.0,
                                  0.25, 0.25, 0.25, 0.25, 0.25 };
    for (int ch = 0; ch < 3; ++ch)
        for (int i = 0; i < 9; ++i)
            p.c[ch * 9 + i] = float(acc[ch * 9 + i] * norm * Al[i]);
    p.baked = true;

    std::fprintf(stderr, "[SHAMB] baked scene-center SH probe at (%.1f %.1f %.1f)"
                 " res %d²×6; DC ambient B/G/R = %.1f/%.1f/%.1f (flat was"
                 " %.0f/%.0f/%.0f)\n", center.x, center.y, center.z, res,
                 p.c[0], p.c[9], p.c[18],
                 (double)sc->Ambient.B, (double)sc->Ambient.G, (double)sc->Ambient.R);
    return true;
}

void EnvReflection_Invalidate(Scene* sc) { g_envByScene.erase(sc); g_shByScene.erase(sc); }

void EnvReflection_InvalidateSurface(Scene* sc, const Material* M) {
    if (!sc || !M) return;
    auto it = g_envByScene.find(sc);
    if (it == g_envByScene.end()) return;           // no bakes yet
    SceneEnv& env = it->second;
    auto jt = env.byMat.find(M);
    if (jt == env.byMat.end()) return;              // not baked — FramePrep bakes it fresh
    const int si = jt->second;
    if (si < 0) { env.byMat.erase(jt); return; }    // stale "no centroid" marker — clear + retry

    // Every material mapped to store si shares that one probe (::mirUV clone,
    // co-located panel within the 4-unit sharing radius): dropping the store
    // for one member means ALL of them must re-bake together — they read the
    // same panorama. Collect + drop the whole group from byMat and null their
    // kernel-table slots (the store's view pointer is about to dangle).
    // Surviving stores keep their heap addresses (unique_ptr owns them), so
    // their table entries stay valid; FramePrep re-publishes the table anyway.
    int groupN = 0;
    for (auto k = env.byMat.begin(); k != env.byMat.end(); ) {
        if (k->second == si) {
            if (k->first->ID < 256) env.table[k->first->ID] = nullptr;
            k = env.byMat.erase(k);
            ++groupN;
        } else {
            ++k;
        }
    }
    // Remove the store; every byMat index after it shifts down by one.
    env.stores.erase(env.stores.begin() + si);
    for (auto& kv : env.byMat) if (kv.second > si) --kv.second;

    std::fprintf(stderr, "[ENVREFL] targeted invalidate: '%s' (id %u) dropped "
                 "store %d shared by %d surface%s; %zu store%s kept — re-bakes "
                 "next FramePrep (vs whole-scene drop of all)\n",
                 M->Name ? M->Name : "?", (unsigned)M->ID, si, groupN,
                 groupN == 1 ? "" : "s", env.stores.size(),
                 env.stores.size() == 1 ? "" : "s");
}

}  // namespace fds
