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

// The env-map inspector's --env_map_viz=3 lists GpuBench's dumped probe
// atlases. POSIX dirent, present on macOS and in the emscripten sysroot (the
// wasm build finds an empty /tmp and the mode simply reports none — no
// native-only call, so `make wasm` keeps building).
#include <dirent.h>

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
        //
        // NB this flag is NOT just "skip animated meshes": Transform.cpp reads
        // it in THREE places — the animated-mesh skip (:1274), the legacy
        // whole-mesh exclusion (:1549) and the reflector's OWN-FACE skip
        // (:2396). --env_bake_include_animated therefore hooks the first of
        // those and only that one; clearing this global would also let the
        // reflector's own canopy glass back into its own probe (measured: the
        // +Y face goes 91 % VOID and the probe mean 100.31 → 49.11).
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

    // ── ENVDYN perf: the REFLECTED-HEMISPHERE face mask ───────────────────
    // Which of the six cube faces a reflection off THIS probe's own owner
    // surface can ever sample, for ANY eye position. Static (owner geometry is
    // static), so it is computed once. See reflectedFaceReach() for the proof;
    // all-true when the owner is closed/two-sided, which makes the cull a
    // no-op rather than a risk. `viewerFaceCount` is the instrument.
    bool                  reflFaceMask[6] = { true, true, true, true, true, true };
    bool                  reflFaceMaskDone = false;
    int                   reflFaceCount = 6;

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

    // ── --env_probe_follow_owner: OWNER-TRANSFORM DIRTY TRACKING ───────────
    // A probe's capture point is derived from its OWNER SURFACE's geometry
    // ONCE, at bake time, and byMat then makes FramePrep skip that material
    // for ever (`if (env.byMat.count(M)) continue;`). Move the owner — in the
    // editor, or because it is a spline-animated mesh — and the store keeps
    // capturing from where the owner USED to be. The dynamic overlay inherits
    // the same staleness: overlayComposite renders the movers from
    // S.view.bake*, the STATIC bake point, so it re-renders the mech every
    // frame from a viewpoint that is no longer on the reflector.
    //
    // Tracked per MATERIAL, not per store, so the record survives the store
    // drop the re-bake goes through (EnvReflection_InvalidateSurface erases
    // stores, not this map).
    struct OwnerTrack {
        std::vector<const TriMesh*> meshes;   // meshes carrying a face of M
        bool     meshesDone   = false;
        bool     splineAnim   = false;  // any owner mesh is spline-animated
        uint64_t sig          = 0;      // owner transform signature, last seen
        bool     sigValid     = false;
        uint32_t lastFollow   = 0;      // followFrame of the last re-bake
        bool     everFollowed = false;
        uint32_t stalledSince = 0;      // followFrame the drift was first seen
        uint32_t lastMove     = 0;      // followFrame the owner last drifted
        bool     dirty        = false;  // drift > eps, waiting for the budget
    };
    std::map<const Material*, OwnerTrack> ownerTrack;
    uint32_t followFrame = 0;
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
//
// `tag` names the population: "envbake" = the STATIC capture (the historical
// FDS_ENVBAKE_DUMP=1 path), "envdyn" = the LIVE store after --env_dynamic's
// overlay composited the movers into it (--env_dyn_dump=N). The static dump
// has no mover in it by construction, so it cannot answer "is the mech in
// this probe's up-looking face" — only the live one can.
void dumpCubeStoreCensus(const char* matName,
                         const std::vector<uint32_t>& level0, int fr,
                         const char* tag = "envbake") {
    if (!std::strcmp(tag, "envbake") && !std::getenv("FDS_ENVBAKE_DUMP")) return;
    if (fr <= 0 || level0.size() < size_t(6) * fr * fr) return;
    std::string safe = matName ? matName : "unnamed";
    for (char& c : safe) if (!std::isalnum((unsigned char)c)) c = '_';
    static const char* kFaceName[6] = { "+X", "-X", "+Y", "-Y", "+Z", "-Z" };
    std::string tagUp = tag;
    for (char& c : tagUp) c = char(std::toupper((unsigned char)c));
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
        std::fprintf(stderr, "[%s-FACE] '%s' face %d %s res %d  "
            "meanBGR %.2f/%.2f/%.2f  meanY %.2f  p50 %.1f p95 %.1f max %.1f  "
            "nonvoid %.1f%%\n", tagUp.c_str(), matName ? matName : "?", f, kFaceName[f], fr,
            sB / n, sG / n, sR / n, sY / n,
            double(ys[size_t(0.50 * (n - 1))]), double(ys[size_t(0.95 * (n - 1))]),
            double(ys.back()), 100.0 * double(nonVoid) / n);
    }
    const std::string path = std::string("/tmp/") + tag + "_" + safe + ".ppm";
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
        std::fprintf(stderr, "[%s-FACE] wrote %s (%dx%d atlas)\n",
                     tagUp.c_str(), path.c_str(), gw, gh);
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
// --env_probe_center: world AREA of one face, and its own centroid. The
// AREA weight is the whole point — the legacy accumulator below adds one
// sample per VERTEX, so a surface's capture point is dragged wherever its
// mesh happens to be finely tessellated rather than to where its reflective
// surface actually is.
inline float faceAreaAndCentroid(const TriMesh* T, const Face& F, Vector& c) {
    const Vertex* vs[3] = { F.A, F.B, F.C };
    Vector w[3];
    for (int k = 0; k < 3; ++k) {
        if (!vs[k]) return 0.0f;
        MatrixXVector(T->RotMat, const_cast<Vector*>(&vs[k]->Pos), &w[k]);
        Vector_SelfAdd(&w[k], &T->IPos);
    }
    const float ux = w[1].x - w[0].x, uy = w[1].y - w[0].y, uz = w[1].z - w[0].z;
    const float vx = w[2].x - w[0].x, vy = w[2].y - w[0].y, vz = w[2].z - w[0].z;
    const float nx = uy * vz - uz * vy;
    const float ny = uz * vx - ux * vz;
    const float nz = ux * vy - uy * vx;
    c = { (w[0].x + w[1].x + w[2].x) * (1.0f / 3.0f),
          (w[0].y + w[1].y + w[2].y) * (1.0f / 3.0f),
          (w[0].z + w[1].z + w[2].z) * (1.0f / 3.0f) };
    return 0.5f * std::sqrt(nx * nx + ny * ny + nz * nz);
}

// areaModeOverride: -1 (default) = read --env_probe_center, 0/1 = force the
// legacy vertex-mean / the area+instance-union derivation regardless of the
// flag. The AUTO-CENTER button (EnvReflection_AutoCenterOffset) needs BOTH
// answers in one frame to compute their difference, and a probe query must
// not be able to move a rendered pixel, so `quiet` also silences the two
// per-call [ENVREFL] lines — a query is not a bake.
bool materialCentroid(Scene* sc, const Material* M, Vector& out,
                      float* excludeRadius = nullptr,
                      int areaModeOverride = -1, bool quiet = false) {
    if (excludeRadius) *excludeRadius = 0.0f;
    const bool areaMode = areaModeOverride >= 0
                        ? (areaModeOverride != 0)
                        : fds::FeatureFlags::env_probe_center();
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
    double sx = 0, sy = 0, sz = 0;          // vertex-mean accumulator (legacy)
    double ax = 0, ay = 0, az = 0, aw = 0;  // area-weighted accumulator
    long n = 0;
    for (TriMesh* T = sc->TriMeshHead; T; T = T->Next) {
        if (isClone(T)) continue;
        for (DWord i = 0; i < T->FIndex; ++i) {
            const Face& F = T->Faces[i];
            if (F.Txtr != M) continue;
            if (areaMode) {
                Vector fc;
                const float a = faceAreaAndCentroid(T, F, fc);
                if (a > 0.0f) {
                    ax += double(a) * fc.x; ay += double(a) * fc.y;
                    az += double(a) * fc.z; aw += a;
                }
            }
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
    // Zero total area (every face degenerate) falls back to the vertex mean
    // rather than dividing by zero — "I could not measure it" must never
    // become a probe at the origin.
    out = (areaMode && aw > 0.0)
        ? Vector{ float(ax / aw), float(ay / aw), float(az / aw) }
        : Vector{ float(sx / n),  float(sy / n),  float(sz / n)  };
    // Multi-instance surfaces (the two greets mummies share one material):
    // the global mean sits in the EMPTY SPACE BETWEEN instances — a probe
    // from nowhere. Greedy-cluster the vertices by distance to the running
    // cluster mean and re-centroid on the HEAVIEST cluster, so the probe is
    // at least correct for one instance (per-instance correctness needs the
    // editor's "split instances", which gives each its own material+probe).
    {
        // Cl::sx/sy/sz/n are the CLUSTERING accumulator and stay vertex-keyed
        // in both modes, so cluster membership — i.e. instance DETECTION — is
        // bit-identical under --env_probe_center. ax/ay/az/aw are the parallel
        // AREA accumulator the new capture point is read from.
        struct Cl { double sx, sy, sz; long n; double ax, ay, az, aw; };
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
                if (!best) { cls.push_back({0, 0, 0, 0, 0, 0, 0, 0}); best = &cls.back(); }
                best->sx += w.x; best->sy += w.y; best->sz += w.z; ++best->n;
                if (areaMode) {
                    Vector fc;
                    const float a = faceAreaAndCentroid(T, F, fc);
                    if (a > 0.0f) {
                        best->ax += double(a) * fc.x; best->ay += double(a) * fc.y;
                        best->az += double(a) * fc.z; best->aw += a;
                    }
                }
            }
        }
        if (cls.size() > 1) {
            const Cl* heavy = &cls[0];
            for (const Cl& c : cls) if (c.n > heavy->n) heavy = &c;
            out = { float(heavy->sx / heavy->n), float(heavy->sy / heavy->n),
                    float(heavy->sz / heavy->n) };
            // ── --env_probe_center: THE INSTANCE-GROUP UNION ──────────────
            // The heaviest cluster is not an instance, it is whatever fragment
            // of one the 8-unit greedy pass happened to close last. greets
            // 'stairs' is a single 9.5-u flight and 9.5 > 8, so it splinters
            // into a top (n=22) and a bottom (n=8) cluster and "heaviest"
            // parks the probe on the top landing END of its own footprint.
            //
            // The exclusion logic just below ALREADY states the right rule —
            // clusters within 2xkR are "fragments of the probed instance",
            // only what lies BEYOND that is a sibling. This makes the capture
            // point obey the same statement instead of contradicting it:
            // grow the group transitively from the heaviest cluster over the
            // same 2xkR link distance, then take the AREA centroid of the
            // union. Genuinely separate instances (the two greets mummies,
            // 24 u apart) never link, so they still get a per-instance probe.
            std::vector<char> inGroup(cls.size(), 0);
            if (areaMode) {
                const float kLink2 = 16.0f * 16.0f;   // (2×kR)²
                inGroup[size_t(heavy - cls.data())] = 1;
                for (bool grew = true; grew; ) {
                    grew = false;
                    for (size_t i = 0; i < cls.size(); ++i) {
                        if (inGroup[i]) continue;
                        const float ix = float(cls[i].sx / cls[i].n);
                        const float iy = float(cls[i].sy / cls[i].n);
                        const float iz = float(cls[i].sz / cls[i].n);
                        for (size_t j = 0; j < cls.size(); ++j) {
                            if (!inGroup[j]) continue;
                            const float dx = ix - float(cls[j].sx / cls[j].n);
                            const float dy = iy - float(cls[j].sy / cls[j].n);
                            const float dz = iz - float(cls[j].sz / cls[j].n);
                            if (dx*dx + dy*dy + dz*dz <= kLink2) {
                                inGroup[i] = 1; grew = true; break;
                            }
                        }
                    }
                }
                double gx = 0, gy = 0, gz = 0, gw = 0;
                long gn = 0; size_t gc = 0;
                for (size_t i = 0; i < cls.size(); ++i) {
                    if (!inGroup[i]) continue;
                    gx += cls[i].ax; gy += cls[i].ay; gz += cls[i].az; gw += cls[i].aw;
                    gn += cls[i].n; ++gc;
                }
                if (gw > 0.0) {
                    out = { float(gx / gw), float(gy / gw), float(gz / gw) };
                    if (!quiet)
                    std::fprintf(stderr, "[ENVREFL] '%s': --env_probe_center — %zu of"
                        " %zu cluster(s) are ONE instance (%ld verts, area %.1f);"
                        " capture point at their AREA centroid (%.1f %.1f %.1f)\n",
                        M->Name ? M->Name : "?", gc, cls.size(), gn, gw,
                        out.x, out.y, out.z);
                }
            }
            if (excludeRadius) {
                // Nearest cluster mean beyond 2×kR of the probe = the nearest
                // SIBLING instance (nearer clusters are fragments of the
                // probed instance — the greedy clustering splinters a single
                // statue into several). Exclude out to just under halfway.
                const float kGroup2 = 16.0f * 16.0f;   // (2×kR)²
                float nearFar2 = 1e30f;
                for (size_t ci = 0; ci < cls.size(); ++ci) {
                    const Cl& c = cls[ci];
                    // --env_probe_center: "sibling" is now decided by the
                    // group membership computed above, not by a fresh
                    // distance test against a capture point that has since
                    // MOVED to the group's area centroid — a re-test from
                    // there can call the group's own far end a sibling and
                    // leave the reflector's own geometry in its reflection.
                    if (areaMode ? bool(inGroup[ci])
                                 : false) continue;
                    const float dx = float(c.sx / c.n) - out.x;
                    const float dy = float(c.sy / c.n) - out.y;
                    const float dz = float(c.sz / c.n) - out.z;
                    const float d2 = dx*dx + dy*dy + dz*dz;
                    if (areaMode) { if (d2 < nearFar2) nearFar2 = d2; }
                    else if (d2 > kGroup2 && d2 < nearFar2) nearFar2 = d2;
                }
                if (nearFar2 < 1e30f)
                    *excludeRadius = 0.45f * std::sqrt(nearFar2);
            }
            if (!quiet)
            std::fprintf(stderr, "[ENVREFL] '%s': %zu instance clusters — probe at the"
                " largest (%.1f %.1f %.1f), self-exclusion radius %.1f (0 = whole"
                " surface); use split-instances for per-instance probes\n",
                M->Name ? M->Name : "?", cls.size(), out.x, out.y, out.z,
                excludeRadius ? *excludeRadius : 0.0f);
            if (!quiet && std::getenv("ENVDBG"))
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

// --env_probe_follow_owner (defined below, next to the scheduler inputs it
// reuses): detect probe owners whose transform has changed since their store
// was baked and DROP the stale stores, so the loop below re-derives their
// capture point and re-bakes them. Forward-declared because it needs
// ownerScreenAreaFrac / materialFaceAabb, which live with the overlay.
namespace { void envFollowOwnerMoves(Scene* sc, SceneEnv& env); }

bool EnvReflection_FramePrep(Scene* sc) {
    if (!sc || g_envBakeInProgress) return false;
    SceneEnv& env = g_envByScene[sc];
    bool bakedAny = false;
    // BEFORE the bake loop: a probe whose owner has MOVED holds a store baked
    // from where the owner used to be. Dropping it here (the same targeted
    // drop the editor's probe-offset boxes use) is all that is needed — the
    // loop below then misses in byMat, re-runs materialCentroid + the authored
    // EnvBakeOfs, and re-bakes from the CURRENT point. No-op, and not even
    // evaluated, unless --env_probe_follow_owner.
    if (fds::FeatureFlags::env_probe_follow_owner()) envFollowOwnerMoves(sc, env);
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
        sceneAABB(sc, env);
        // Authored per-surface capture-point offset (Material::EnvBakeOfs,
        // the editor's "probe offset"). Applied on TOP of whichever
        // derivation ran, so the two compose and the authored value always
        // wins the last word. All-zero = unset → byte-null.
        if (M->EnvBakeOfs[0] != 0.0f || M->EnvBakeOfs[1] != 0.0f
            || M->EnvBakeOfs[2] != 0.0f) {
            std::fprintf(stderr, "[ENVREFL] '%s': authored probe offset "
                "(%+.2f %+.2f %+.2f) — capture point (%.1f %.1f %.1f) -> "
                "(%.1f %.1f %.1f)\n", M->Name ? M->Name : "?",
                M->EnvBakeOfs[0], M->EnvBakeOfs[1], M->EnvBakeOfs[2],
                c.x, c.y, c.z, c.x + M->EnvBakeOfs[0], c.y + M->EnvBakeOfs[1],
                c.z + M->EnvBakeOfs[2]);
            c.x += M->EnvBakeOfs[0]; c.y += M->EnvBakeOfs[1]; c.z += M->EnvBakeOfs[2];
            // ── "the bake got borked" ─────────────────────────────────────
            // The offset itself is never wrong — it is three floats added to
            // a point, and the sign is carried correctly by every stage (the
            // editor box, the RVSF write/read, the FLD, this add). What GOES
            // wrong is where the point LANDS. A derived capture point sits on
            // the reflector, and a reflector usually sits on the floor, so
            // there is far less room BELOW it than above: on greets 'stairs'
            // the point is at y=2.3 with the room floor at y=0, and any
            // downward offset past 2.3 puts the camera UNDER THE WORLD. The
            // resulting cube is not corrupt, it is a faithful photograph of
            // the outside of the level — the -Y face comes back 100% clear
            // colour and +Y is the underside of the floor. That reads as
            // "the bake got borked" and is impossible to diagnose from the
            // picture, so SAY IT, on the frame it happens, naming the axis
            // and the distance. stderr only; nothing rendered changes.
            const float lo[3] = { env.boxMin[0], env.boxMin[1], env.boxMin[2] };
            const float hi[3] = { env.boxMax[0], env.boxMax[1], env.boxMax[2] };
            const float p[3]  = { c.x, c.y, c.z };
            static const char* kAx = "XYZ";
            char why[192]; size_t wp = 0;
            for (int a = 0; a < 3 && env.boxValid; ++a) {
                float d = 0.0f;
                if (p[a] < lo[a]) d = lo[a] - p[a];
                else if (p[a] > hi[a]) d = p[a] - hi[a];
                if (d > 0.0f && wp + 40 < sizeof why)
                    wp += size_t(std::snprintf(why + wp, sizeof why - wp,
                        "%s%c by %.2f u (scene %c is %.1f..%.1f)", wp ? ", " : "",
                        kAx[a], double(d), kAx[a], double(lo[a]), double(hi[a])));
            }
            if (wp)
                std::fprintf(stderr, "[ENVREFL] WARNING: '%s' probe offset puts "
                    "the capture point OUTSIDE the scene bounds — %s. The bake "
                    "will succeed and look empty/black: the camera is outside "
                    "the level looking in, so faces pointing away come back as "
                    "flat clear colour. This is a BAD VIEWPOINT, not a corrupt "
                    "bake — shrink the offset (a probe derived on the floor has "
                    "much less room below it than above).\n",
                    M->Name ? M->Name : "?", why);
        }
        if (std::getenv("ENVDBG"))
            std::fprintf(stderr, "[ENVDBG] mat '%s' id=%u refl=%.0f metal=%d envDyn=%d reflMode=%d effMode=%d centroid (%.1f %.1f %.1f)\n",
                         M->Name ? M->Name : "?", (unsigned)M->ID, M->Reflection, M->MetallicMap ? 1 : 0,
                         (int)M->EnvDynamic, (int)M->EnvReflMode, envEffModeFor(M),
                         c.x, c.y, c.z);
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

// ── ENVDYN perf: the REFLECTED-HEMISPHERE face cull ───────────────────────
// THE PROPERTY (the user's: "the env map won't show any dynamic data from the
// camera pos"). A probe is only ever READ through its owner surface, by the
// deferred kernel's env lookup at pixels of that surface. So a cube face no
// reflection off that surface can reach is a face whose dynamic overlay
// NOBODY CAN SEE, and rendering the mech into it is pure cost.
//
// THE BOUND, and it is exact and camera-INDEPENDENT. Let n be a surface
// normal and v the unit vector from the surface point to the eye. The pixel is
// only shaded at all if the face is front-facing, i.e. n·v > 0. The reflected
// direction is r = 2(n·v)n - v, so
//
//        n·r = 2(n·v) - (n·v) = n·v > 0.
//
// EVERY reflected direction lies in the OPEN HEMISPHERE on the outward side of
// its own normal, for every eye position there is. No camera term, so nothing
// here can pop when the camera moves — which is the failure mode a
// camera-dependent cull has to be defended against and this one cannot have.
//
// Face f covers the direction set { fwd + a*right + b*up : |a|,|b| <= pad }
// (unnormalised; normalising cannot change a sign). So SOME direction of face
// f satisfies n·d > 0 iff
//
//        n·fwd + pad*(|n·right| + |n·up|) > 0,
//
// three dots and two abs per (normal, face). The mask is the OR over every
// owner normal, so a curved or two-sided owner simply reaches everything and
// the cull becomes a no-op — the safe direction.
//
// THE SLACK, and why it is not optional. Two things sit between r and the
// direction actually used:
//   (1) PARALLAX. The kernel does not index with r; it indexes with
//       (hit - B), hit = P + t*r, P the pixel's world position on the owner,
//       B the store's bake point. n·(hit - B) = n·(P - B) + t*(n·r). For a
//       PLANAR owner B is on the plane and n·(P-B) = 0, so the sign carries
//       over exactly. For a non-planar owner it does not, and the error is
//       bounded by the owner's own extent over the parallax hit distance.
//   (2) --env_live_water tilts the direction by at most `amp`.
// Both are angular, so both are absorbed by widening the normal cone by
// env_dyn_face_cull_slack degrees. The widened test admits a normal n' within
// sigma of n, and max|d(value)/d(n')| <= sqrt(1 + 2*pad^2), giving the
// threshold below. Default 15 degrees, which on greets covers the momy panels'
// curvature (measured owner extents are a few units against parallax hit
// distances of tens).
void reflectedFaceReach(Scene* sc, const SceneEnv& env, int storeIdx,
                        EnvPanoStore& S) {
    if (S.reflFaceMaskDone) return;
    S.reflFaceMaskDone = true;
    for (int f = 0; f < 6; ++f) S.reflFaceMask[f] = false;

    const float sigma = fds::FeatureFlags::env_dyn_face_cull_slack()
                      * 3.14159265358979323846f / 180.0f;
    const float grad  = std::sqrt(1.0f + 2.0f * kEnvCubePad * kEnvCubePad);
    const float thresh = -std::sin(sigma < 0.0f ? 0.0f : sigma) * grad;

    std::vector<const TriMesh*> cloneMeshes;
    collectMirrorCloneMeshes(sc, cloneMeshes);
    long nNormals = 0;
    for (TriMesh* T = sc->TriMeshHead; T; T = T->Next) {
        if (std::find(cloneMeshes.begin(), cloneMeshes.end(), T)
            != cloneMeshes.end()) continue;
        for (DWord i = 0; i < T->FIndex; ++i) {
            const Face& F = T->Faces[i];
            // EVERY material mapped to this store, not just bakedSkipMat:
            // ::mirUV clones and co-located panels share one store (the 4-unit
            // dedup), and a mask built from one of them would be a mask for
            // the wrong geometry.
            auto jt = env.byMat.find(F.Txtr);
            if (jt == env.byMat.end() || jt->second != storeIdx) continue;
            Vector n;
            MatrixXVector(T->RotMat, const_cast<Vector*>(&F.N), &n);   // rotation only
            const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
            if (len < 1e-12f) continue;
            const float inv = 1.0f / len;
            const float nx = n.x * inv, ny = n.y * inv, nz = n.z * inv;
            ++nNormals;
            for (int f = 0; f < 6; ++f) {
                if (S.reflFaceMask[f]) continue;
                const EnvCubeBasisT& B = EnvCube_Basis(f);
                const float df = nx * B.fwd[0]   + ny * B.fwd[1]   + nz * B.fwd[2];
                const float dr = nx * B.right[0] + ny * B.right[1] + nz * B.right[2];
                const float du = nx * B.up[0]    + ny * B.up[1]    + nz * B.up[2];
                if (df + kEnvCubePad * (std::fabs(dr) + std::fabs(du)) > thresh)
                    S.reflFaceMask[f] = true;
            }
        }
    }
    // No owner geometry found at all (a material with no live faces): reach
    // everything. "I could not measure it" must never become "cull it".
    if (nNormals == 0) for (int f = 0; f < 6; ++f) S.reflFaceMask[f] = true;
    S.reflFaceCount = 0;
    for (int f = 0; f < 6; ++f) S.reflFaceCount += S.reflFaceMask[f] ? 1 : 0;
    static const char* kFN[6] = { "+X", "-X", "+Y", "-Y", "+Z", "-Z" };
    char reach[32] = {}; size_t p = 0;
    for (int f = 0; f < 6; ++f)
        if (S.reflFaceMask[f] && p + 3 < sizeof reach)
            p += size_t(std::snprintf(reach + p, sizeof reach - p, "%s%s", p ? " " : "", kFN[f]));
    std::fprintf(stderr, "[ENVDYN-CULL] store %d '%s': %ld owner normal(s) -> "
                 "%d/6 face(s) a reflection off it can EVER reach {%s}, slack %.1f deg\n",
                 storeIdx, S.bakedSkipMat && S.bakedSkipMat->Name ? S.bakedSkipMat->Name : "?",
                 nNormals, S.reflFaceCount, p ? reach : "none",
                 double(fds::FeatureFlags::env_dyn_face_cull_slack()));
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

// ── --env_probe_follow_owner: PROBES WHOSE OWNER MOVED ────────────────────
// THE DEFECT (the user: "env dynamic bakes don't take into account that the
// object itself can move — the static meshes need to be re-baked, where the
// dynamic not (?)"). Half right, and the half that is wrong is the expensive
// one. A probe's capture point is derived from its owner surface's geometry
// ONCE (materialCentroid + the authored EnvBakeOfs), and FramePrep's
// `if (env.byMat.count(M)) continue;` then never looks at that material again.
// So:
//   • the STATIC cube keeps the room as seen from where the owner used to be;
//   • the DYNAMIC overlay is NOT exempt — overlayComposite renders the movers
//     from S.view.bake*, the same frozen point, so it faithfully re-renders
//     the mech every frame into a cube captured from the wrong place.
// Both halves want the same fix, and it is the one the editor's probe-offset
// boxes already use: drop the store, let FramePrep re-derive and re-bake.
//
// TWO-STAGE DIRTY TEST, cheap first. Stage 1 is a transform SIGNATURE over the
// owner meshes' RotMat+IPos (12 floats each, a handful of meshes) — nothing
// happens on a frame where nothing moved. Only when that changes does stage 2
// re-run materialCentroid (an O(faces-of-this-material) walk) and compare the
// new capture point against the one the live store actually holds. A re-bake
// is requested only past env_probe_follow_eps world units of DRIFT, because
// the cost being throttled is a full cube bake, not a comparison.
//
// EDITOR MOVES ARE IMMEDIATE, RUNTIME MOVERS ARE THROTTLED, and the
// discriminator is the engine's existing one: WorldAabb_MeshIsDynamic, the
// same predicate the overlay uses to decide what a "mover" is. An owner no
// spline animates can only have been moved by a person, once, and waiting a
// frame to show them the result would be a worse tool. An owner that is
// spline-animated moves every frame and is throttled by
// env_probe_follow_budget re-bakes per frame, ordered by the SAME priority the
// --env_dyn_sched=1 scheduler uses so there is one notion of "which probe
// matters most" in the file and not two: tier 0 = never followed yet, then
// visible ordered by ownerScreenAreaFrac × wait, then off-screen by wait.
uint64_t ownerXformSig(const SceneEnv::OwnerTrack& t) {
    // FNV-1a over the raw bits of each owner mesh's rotation + position. A
    // HASH, not a comparison: the point is to notice a change, and any change
    // of any owner's transform changes it.
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](float f) {
        uint32_t u; std::memcpy(&u, &f, 4);
        for (int b = 0; b < 4; ++b) {
            h ^= uint64_t((u >> (b * 8)) & 0xFF);
            h *= 1099511628211ull;
        }
    };
    for (const TriMesh* T : t.meshes) {
        if (!T) continue;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) mix(T->RotMat[r][c]);
        mix(T->IPos.x); mix(T->IPos.y); mix(T->IPos.z);
    }
    return h;
}

// Which meshes carry a face of M, and is any of them spline-animated? Mirror
// clones excluded — the bake and materialCentroid exclude them too, so a
// clone's transform must not be able to dirty a probe. Cached: the owner mesh
// SET is topology, which the editor's own paths already invalidate.
void collectOwnerMeshes(Scene* sc, const Material* M, SceneEnv::OwnerTrack& t) {
    if (t.meshesDone) return;
    t.meshesDone = true;
    t.meshes.clear();
    t.splineAnim = false;
    std::vector<const TriMesh*> cloneMeshes;
    collectMirrorCloneMeshes(sc, cloneMeshes);
    for (Object* Obj = sc->ObjectHead; Obj; Obj = Obj->Next) {
        if (Obj->Type != Obj_TriMesh) continue;
        TriMesh* T = (TriMesh*)Obj->Data;
        if (!T || T->FIndex == 0) continue;
        if (std::find(cloneMeshes.begin(), cloneMeshes.end(), T) != cloneMeshes.end())
            continue;
        bool uses = false;
        for (DWord i = 0; i < T->FIndex && !uses; ++i)
            if (T->Faces[i].Txtr == M) uses = true;
        if (!uses) continue;
        t.meshes.push_back(T);
        if (fds::WorldAabb_MeshIsDynamic(Obj)) t.splineAnim = true;
    }
}

void envFollowOwnerMoves(Scene* sc, SceneEnv& env) {
    if (!sc || env.stores.empty()) return;
    ++env.followFrame;
    const float eps    = fds::FeatureFlags::env_probe_follow_eps();
    const int   budget = fds::FeatureFlags::env_probe_follow_budget();
    static const bool sProf = std::getenv("FDS_ENVDYN_PROF") != nullptr;

    struct Moved {
        const Material* M;
        int      si;
        bool     animated;
        int      tier;
        float    key;
        float    drift;
        Vector   want;
    };
    std::vector<Moved> moved;

    for (auto& [M, si] : env.byMat) {
        if (si < 0 || size_t(si) >= env.stores.size()) continue;
        EnvPanoStore& S = *env.stores[size_t(si)];
        // Imported stores (CITY's per-building cube faces) carry their own
        // bake point from the registration side and cannot be re-baked here.
        if (S.imported || !S.bakedSkipMat) continue;
        // One record per STORE, kept on the store's OWN material: a sharing
        // group (::mirUV clones, co-located panels) re-bakes as a unit, and
        // tracking each member would ask the same question several times.
        if (M != S.bakedSkipMat) continue;
        SceneEnv::OwnerTrack& t = env.ownerTrack[M];
        collectOwnerMeshes(sc, M, t);
        if (t.meshes.empty()) continue;
        const uint64_t sig = ownerXformSig(t);
        if (!t.sigValid) { t.sig = sig; t.sigValid = true; continue; }
        if (sig == t.sig && !t.dirty) continue;      // stage 1: nothing moved
        t.sig = sig;
        // Stage 2: has the CAPTURE POINT actually drifted? Re-derive exactly
        // as the bake would — the active derivation plus the authored offset —
        // and compare against the point the live store was baked from.
        Vector c;
        if (!materialCentroid(sc, M, c, nullptr, -1, /*quiet=*/true)) continue;
        c.x += M->EnvBakeOfs[0]; c.y += M->EnvBakeOfs[1]; c.z += M->EnvBakeOfs[2];
        const float dx = c.x - S.view.bakeX, dy = c.y - S.view.bakeY,
                    dz = c.z - S.view.bakeZ;
        const float drift = std::sqrt(dx*dx + dy*dy + dz*dz);
        if (drift <= eps) { t.dirty = false; continue; }
        if (!t.dirty) { t.dirty = true; t.stalledSince = env.followFrame; }
        // ── EDITOR MOVE vs RUNTIME MOVER, and why it takes TWO tests ───────
        // WorldAabb_MeshIsDynamic reads the object's Pos/Rotate SPLINE keys.
        // That is the engine's own definition of a mover and it is correct for
        // everything measured here (the greets mech, all five city vehicle
        // glass probes). It is not, however, EXHAUSTIVE: nothing stops scene
        // code from writing IPos directly on a spline-less object, and such an
        // owner would be classified an "editor move" and re-bake UNCAPPED
        // every frame — the exact cost blow-up the budget exists to prevent.
        // The second test closes that by construction and needs no predicate:
        // something that also moved on the PREVIOUS follow frame is still
        // moving, whatever its splines say. An editor nudge is a one-shot — it
        // moves once and stops — so it keeps its immediate, uncapped re-bake,
        // which is the whole point of the distinction. The FIRST movement of a
        // runtime mover is immediate too, and that is correct: tier 0 (never
        // followed) outranks everything anyway.
        const bool stillMoving = t.lastMove != 0 && t.lastMove + 1 >= env.followFrame;
        t.lastMove = env.followFrame;
        Moved mv{};
        mv.M = M; mv.si = si; mv.animated = t.splineAnim || stillMoving;
        mv.drift = drift; mv.want = c;
        // The owner MOVED, so its cached face AABB is stale — recompute it
        // before it is used as a priority input (and clear the flag so the
        // overlay's own gate recomputes it too).
        S.ownerFaceAabb = materialFaceAabb(sc, M);
        S.ownerFaceAabbDone = true;
        S.reflFaceMaskDone = false;
        const bool onScreen = S.ownerFaceAabb.valid;
        const float area = onScreen ? ownerScreenAreaFrac(sc, S.ownerFaceAabb) : 0.0f;
        const uint32_t wait = env.followFrame - t.stalledSince + 1u;
        if (!t.everFollowed)      { mv.tier = 0; mv.key = 0.0f; }
        else if (area > 0.0f)     { mv.tier = 2; mv.key = area * float(wait); }
        else                      { mv.tier = 3; mv.key = float(wait); }
        moved.push_back(mv);
        if (sProf)
            std::fprintf(stderr, "[ENVFOLLOW] '%s' (store %d): owner %s, capture "
                "point drifted %.2f u — (%.1f %.1f %.1f) -> (%.1f %.1f %.1f), "
                "tier %d key %.6f area %.5f wait %u\n",
                M->Name ? M->Name : "?", si,
                t.splineAnim ? "SPLINE-ANIMATED" : stillMoving ? "STILL-MOVING"
                                                              : "moved once (editor)",
                double(drift), S.view.bakeX, S.view.bakeY, S.view.bakeZ,
                c.x, c.y, c.z, mv.tier, double(mv.key), double(area), wait);
    }
    if (moved.empty()) return;

    // Editor moves first and ALL of them (a person is waiting), then the
    // spline-animated ones by the shared priority, capped by the budget.
    std::stable_sort(moved.begin(), moved.end(), [](const Moved& a, const Moved& b) {
        if (a.animated != b.animated) return !a.animated;
        if (a.tier != b.tier) return a.tier < b.tier;
        return a.key > b.key;
    });
    int spent = 0;
    for (const Moved& mv : moved) {
        if (mv.animated && budget > 0 && spent >= budget) {
            if (sProf) std::fprintf(stderr, "[ENVFOLLOW] '%s': BUDGET-DEFERRED "
                "(%d/frame already re-baked; stays dirty, re-ranked next frame)\n",
                mv.M->Name ? mv.M->Name : "?", budget);
            continue;
        }
        if (mv.animated && budget <= 0) continue;
        SceneEnv::OwnerTrack& t = env.ownerTrack[mv.M];
        t.dirty = false;
        t.everFollowed = true;
        t.lastFollow = env.followFrame;
        std::fprintf(stderr, "[ENVFOLLOW] '%s': %s owner moved %.2f u past the "
            "%.2f u threshold — dropping its store and re-baking from "
            "(%.1f %.1f %.1f)\n", mv.M->Name ? mv.M->Name : "?",
            mv.animated ? "RUNTIME" : "EDITOR", double(mv.drift), double(eps),
            mv.want.x, mv.want.y, mv.want.z);
        // The SAME targeted drop the editor's probe-offset boxes use: the
        // whole sharing group leaves byMat, the store is erased, and the bake
        // loop that follows re-derives and re-bakes it from the current point.
        if (mv.animated) ++spent;
        fds::EnvReflection_InvalidateSurface(sc, mv.M);
    }
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

// --env_dyn_dump=N (1-based store index): write the LIVE, post-overlay mip-0
// cube of store N as the standard 3x2 atlas. Separate from the composite
// itself so the caller owns the index test and this stays a pure reader.
void dumpLiveStore(const EnvPanoStore& S, size_t storeIdx) {
    const int want = fds::FeatureFlags::env_dyn_dump();
    if (want <= 0 || size_t(want - 1) != storeIdx) return;
    if (!S.view.isCube || S.levels[0].empty()) return;
    dumpCubeStoreCensus(S.bakedSkipMat && S.bakedSkipMat->Name
                            ? S.bakedSkipMat->Name : "unnamed",
                        S.levels[0], S.view.W, "envdyn");
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
    const bool faceCull  = fds::FeatureFlags::env_dyn_face_cull();

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
            // The reflected-hemisphere mask ANDs into the mover mask: a face
            // no reflection off this owner can reach is a face nobody can see
            // the overlay in. Camera-independent, so no pop.
            if (faceCull) reflectedFaceReach(sc, env, int(si), S);
            for (int f = 0; f < 6; ++f) {
                if (faceCull && !S.reflFaceMask[f]) continue;
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
            dumpLiveStore(S, c.si);
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
        if (faceCull) reflectedFaceReach(sc, env, int(si), S);
        for (int f = 0; f < 6; ++f) {
            if (faceCull && !S.reflFaceMask[f]) continue;
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
        dumpLiveStore(S, si);
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

// ═══ THE ENV-MAP INSPECTOR (--env_map_viz) ════════════════════════════════
// Everything below is a READER of the stores above. It never bakes, never
// touches a store's bytes, and writes only VPage — so the frame the user is
// flying is exactly the frame he would have had, plus an overlay.
namespace {

// Face CELL order of the 3x2 atlas. Identical to renderCubeFacesMajor's
// FDS_ENVBAKE_DUMP grid, to dumpCubeStoreCensus's per-material atlas and to
// GpuBench --dump_env_cube's — so "the face with the mech in it" names the
// same cell on screen, in a CPU PPM and in a GPU PPM. Do not reorder.
const char* const kFaceLabel[6] = { "+X", "-X", "+Y", "-Y", "+Z", "-Z" };

// Probe count the LAST drawn frame saw, published by the render thread for the
// SDL event thread's F-key stepper. The stepper must not walk g_envByScene
// itself: FramePrep can push_back into it on the render thread, and a
// std::vector growing under a concurrent size() read is a real race, not a
// tolerable one. An aligned int is the same latitude every other
// Keyboard-driven toggle here already takes (see the note in VizCycle.h).
int g_envMapProbeCount = 0;

int emFontScale() { return g_fontScale > 0 ? g_fontScale : 1; }
int emGlyphH()    { return (Active_Font && Active_Font->Y > 0 ? Active_Font->Y : 8) * emFontScale(); }
int emPitch()     { const int g = emGlyphH(), f = emFontScale();
                    return g + 5 * f > 14 * f ? g + 5 * f : 14 * f; }

void emFill(int x0, int y0, int x1, int y1, uint32_t rgb) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > XRes) x1 = XRes;
    if (y1 > YRes) y1 = YRes;
    dword* out = reinterpret_cast<dword*>(VPage);
    const dword c = 0xFF000000u | (rgb & 0x00FFFFFFu);
    for (int y = y0; y < y1; ++y) {
        dword* row = out + size_t(y) * size_t(XRes);
        for (int x = x0; x < x1; ++x) row[x] = c;
    }
}

// Multiply a rect down so white text reads over a bright frame — the same
// trick (and the same keep/256 scale) VizCycle's legend panel uses.
void emDim(int x0, int y0, int x1, int y1, uint32_t keep256) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > XRes) x1 = XRes;
    if (y1 > YRes) y1 = YRes;
    dword* out = reinterpret_cast<dword*>(VPage);
    for (int y = y0; y < y1; ++y) {
        dword* row = out + size_t(y) * size_t(XRes);
        for (int x = x0; x < x1; ++x) {
            const dword d = row[x];
            const uint32_t r = (((d >> 16) & 0xFFu) * keep256) >> 8;
            const uint32_t g = (((d >>  8) & 0xFFu) * keep256) >> 8;
            const uint32_t b = (( d        & 0xFFu) * keep256) >> 8;
            row[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

void emFrame(int x0, int y0, int w, int h, uint32_t rgb) {
    emFill(x0 - 1, y0 - 1, x0 + w + 1, y0,         rgb);
    emFill(x0 - 1, y0 + h, x0 + w + 1, y0 + h + 1, rgb);
    emFill(x0 - 1, y0,     x0,         y0 + h,     rgb);
    emFill(x0 + w, y0,     x0 + w + 1, y0 + h,     rgb);
}

// ── one source image, cube or equirect, behind a single accessor ──────────
// The whole point of routing both through this is that the CUBE case is the
// normal one (env_cube defaults ON) but the legacy equirect store still
// exists, and drawing an equirect image as if it were a 3x2 cube atlas would
// produce a confident, wrong picture. So the layout question is asked once,
// here, and every painter below just reads w()/h()/at().
struct AtlasSrc {
    const uint32_t* px = nullptr;
    int  fr = 0;           // cube: per-face res
    int  W = 0, H = 0;     // equirect: image dims
    bool cube = false;
    bool valid() const { return px && (cube ? fr > 0 : (W > 0 && H > 0)); }
    int  w() const { return cube ? fr * 3 : W; }
    int  h() const { return cube ? fr * 2 : H; }
    uint32_t at(int x, int y) const {
        if (!cube) return px[size_t(y) * size_t(W) + size_t(x)];
        const int f = (y / fr) * 3 + (x / fr);
        return px[size_t(f) * size_t(fr) * size_t(fr)
                  + size_t(y % fr) * size_t(fr) + size_t(x % fr)];
    }
};

AtlasSrc srcForLevel(const EnvPanoLinear& v, int level) {
    AtlasSrc s;
    if (level < 0 || level >= v.numMips || !v.mip[level]) return s;
    s.px = v.mip[level];
    s.cube = v.isCube;
    if (v.isCube) s.fr = v.W >> level;
    else { s.W = v.W >> level; s.H = v.H >> level; }
    return s;
}

// Integer-only scaling, both ways: > 0 magnifies by `scale`, < 0 minifies by
// -scale. Nearest neighbour on purpose — a resampled probe is a picture of a
// filter, and the question this viewer answers ("what is actually IN this
// texel") needs the texels themselves.
int emFitScale(int sw, int sh, int maxW, int maxH) {
    if (sw <= 0 || sh <= 0 || maxW <= 0 || maxH <= 0) return 1;
    if (sw <= maxW && sh <= maxH) {
        int up = 1;
        while (up < 8 && sw * (up + 1) <= maxW && sh * (up + 1) <= maxH) ++up;
        return up;
    }
    int down = 2;
    while (down < 64 && ((sw + down - 1) / down > maxW ||
                         (sh + down - 1) / down > maxH)) ++down;
    return -down;
}

void emScaledDims(const AtlasSrc& s, int scale, int& dw, int& dh) {
    if (scale >= 1) { dw = s.w() * scale; dh = s.h() * scale; }
    else            { const int d = -scale;
                      dw = (s.w() + d - 1) / d; dh = (s.h() + d - 1) / d; }
}

void emBlit(const AtlasSrc& s, int x0, int y0, int scale) {
    int dw, dh; emScaledDims(s, scale, dw, dh);
    dword* out = reinterpret_cast<dword*>(VPage);
    const int up = scale >= 1 ? scale : 1;
    const int dn = scale >= 1 ? 1 : -scale;
    const int sw = s.w(), sh = s.h();
    for (int y = 0; y < dh; ++y) {
        const int oy = y0 + y;
        if (oy < 0 || oy >= YRes) continue;
        int sy = (y / up) * dn;
        if (sy >= sh) sy = sh - 1;
        dword* row = out + size_t(oy) * size_t(XRes);
        for (int x = 0; x < dw; ++x) {
            const int ox = x0 + x;
            if (ox < 0 || ox >= XRes) continue;
            int sx = (x / up) * dn;
            if (sx >= sw) sx = sw - 1;
            row[ox] = s.at(sx, sy) | 0xFF000000u;
        }
    }
}

// Cell grid lines + the +X/-X/... label in every cell. Requirement, not
// decoration: a probe investigation this week turned on WHICH face held the
// mech, and an unlabelled 3x2 grid cannot answer that.
void emLabelCubeCells(int x0, int y0, int cellW, int cellH) {
    const int fs = emFontScale();
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 3; ++c) {
            const int cx = x0 + c * cellW, cy = y0 + r * cellH;
            emFill(cx, cy, cx + cellW, cy + 1, 0x00303030u);          // cell top
            emFill(cx, cy, cx + 1, cy + cellH, 0x00303030u);          // cell left
            const int lw = 3 * 8 * fs, lh = emGlyphH() + 2 * fs;
            emDim(cx + 1, cy + 1, cx + 1 + lw, cy + 1 + lh, 40);
            OutTextXY(VPage, cx + 3 * fs, cy + 2 * fs, kFaceLabel[r * 3 + c], 255);
        }
}

// ── GpuBench --dump_env_cube atlases (mode 3) ─────────────────────────────
// GpuBench writes <dir>/gpuenv_<material>.ppm: a P6 3x2 FACE ATLAS in the SAME
// cell order as ours, sqrt-encoded (Deferred.mm ~:2950) because the GPU store
// is linear RGBA16F. Reading it back here turns the CPU-vs-GPU probe-content
// comparison — the one docs/SHADING_CONTRACT.md §11 row E6 had to script — into
// a keypress. Nothing else in FDS depends on this; if the file is absent the
// mode says so.
const char kGpuEnvDir[]    = "/tmp";
const char kGpuEnvPrefix[] = "gpuenv_";

std::string emSafeName(const char* n) {
    std::string s = (n && *n) ? n : "unnamed";
    for (char& c : s) if (!std::isalnum((unsigned char)c)) c = '_';
    return s;
}

struct PpmImage {
    std::vector<uint32_t> px;
    int W = 0, H = 0;
    bool ok() const { return W > 0 && H > 0 && px.size() == size_t(W) * size_t(H); }
};

bool emReadPpmP6(const char* path, PpmImage& out) {
    FILE* fp = std::fopen(path, "rb");
    if (!fp) return false;
    auto tok = [&](long& v) -> bool {          // whitespace + '#' comments
        int c;
        for (;;) {
            do { c = std::fgetc(fp); } while (c == ' ' || c == '\t' || c == '\n' || c == '\r');
            if (c == '#') { while (c != '\n' && c != EOF) c = std::fgetc(fp); continue; }
            break;
        }
        if (c < '0' || c > '9') return false;
        v = 0;
        while (c >= '0' && c <= '9') { v = v * 10 + (c - '0'); c = std::fgetc(fp); }
        return true;
    };
    char magic[2] = {};
    if (std::fread(magic, 1, 2, fp) != 2 || magic[0] != 'P' || magic[1] != '6') {
        std::fclose(fp); return false;
    }
    long W = 0, H = 0, maxv = 0;
    if (!tok(W) || !tok(H) || !tok(maxv) || W <= 0 || H <= 0 || maxv != 255 ||
        W > 16384 || H > 16384) { std::fclose(fp); return false; }
    std::vector<unsigned char> raw(size_t(W) * size_t(H) * 3);
    const bool full = std::fread(raw.data(), 1, raw.size(), fp) == raw.size();
    std::fclose(fp);
    if (!full) return false;
    out.W = int(W); out.H = int(H);
    out.px.resize(size_t(W) * size_t(H));
    for (size_t i = 0; i < out.px.size(); ++i)
        out.px[i] = 0xFF000000u | (uint32_t(raw[i * 3 + 0]) << 16)
                                | (uint32_t(raw[i * 3 + 1]) << 8)
                                |  uint32_t(raw[i * 3 + 2]);
    return true;
}

// Every gpuenv_*.ppm stem currently on disk. Cheap (one readdir), and the
// result is what mode 3 prints when it cannot match — "no GPU atlas" is only
// useful next to "here is what there IS".
std::vector<std::string> emGpuAtlasStems() {
    std::vector<std::string> v;
    if (DIR* d = ::opendir(kGpuEnvDir)) {
        while (struct dirent* e = ::readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() <= sizeof(kGpuEnvPrefix) - 1 + 4) continue;
            if (n.compare(0, sizeof(kGpuEnvPrefix) - 1, kGpuEnvPrefix) != 0) continue;
            if (n.compare(n.size() - 4, 4, ".ppm") != 0) continue;
            v.push_back(n.substr(sizeof(kGpuEnvPrefix) - 1,
                                 n.size() - (sizeof(kGpuEnvPrefix) - 1) - 4));
        }
        ::closedir(d);
    }
    std::sort(v.begin(), v.end());
    return v;
}

// The two arms do NOT agree on probe names: the CPU store is owned by e.g.
// `Hull.lwo::cockpit_upper::mirUV` while GpuBench names the same probe
// `cockpit` (measured — docs/SHADING_CONTRACT.md §11.2 prints both). So: exact
// mangled name, then the last `::` segment, then any stem that is a substring
// of the CPU name. Returns "" when nothing matches — and mode 3 then SAYS
// nothing matched rather than showing an unrelated probe.
std::string emMatchGpuStem(const char* cpuName, const std::vector<std::string>& stems) {
    if (stems.empty()) return std::string();
    const std::string full = emSafeName(cpuName);
    for (const std::string& s : stems) if (s == full) return s;
    std::string tail = cpuName ? cpuName : "";
    const size_t sep = tail.rfind("::");
    if (sep != std::string::npos) tail = tail.substr(sep + 2);
    const std::string tailSafe = emSafeName(tail.c_str());
    for (const std::string& s : stems) if (s == tailSafe) return s;
    std::string best;
    for (const std::string& s : stems)
        if (s.size() >= 3 && full.find(s) != std::string::npos && s.size() > best.size())
            best = s;
    return best;
}

// ── the probe's identity ─────────────────────────────────────────────────
// Requirement 2: name what he is looking at. Materials come from the byMat
// map (an n:1 mapping — ::mirUV clones and co-located panels share a store),
// the bake point and dims from the store's own recorded view, so nothing here
// can drift from what the kernel actually samples.
struct ProbeIdent {
    const EnvPanoStore* S = nullptr;
    const char* primary = "(no material mapped)";
    int   nMats = 0;
    char  matList[160] = {};
};

bool emProbeIdent(const SceneEnv& env, int idx, ProbeIdent& out) {
    if (idx < 0 || idx >= int(env.stores.size())) return false;
    out.S = env.stores[size_t(idx)].get();
    if (!out.S) return false;
    size_t p = 0;
    for (const auto& kv : env.byMat) {
        if (kv.second != idx) continue;
        const char* nm = (kv.first && kv.first->Name) ? kv.first->Name : "?";
        ++out.nMats;
        if (p + 2 < sizeof out.matList)
            p += size_t(std::snprintf(out.matList + p, sizeof out.matList - p,
                                      "%s%s", p ? ", " : "", nm));
        if (p >= sizeof out.matList) { p = sizeof out.matList - 1; break; }
    }
    // Primary name: the material the store was BAKED from when there is one
    // (bakeStore records it for the res-upgrade path), else the first mapped.
    if (out.S->bakedSkipMat && out.S->bakedSkipMat->Name)
        out.primary = out.S->bakedSkipMat->Name;
    else
        for (const auto& kv : env.byMat)
            if (kv.second == idx && kv.first && kv.first->Name) {
                out.primary = kv.first->Name; break;
            }
    return true;
}

// Mip chain as text: "4 levels (256/128/64/32)". The chain DEPTH is what
// --env_mip_chain exists to argue about (SHADING_CONTRACT §11 row E7: the same
// roughness picks a 3x wider lobe on the GPU purely because its chain is
// deeper), so the viewer states the real depth AND the virtual one the
// roughness select is actually dividing by.
void emChainText(const EnvPanoLinear& v, char* buf, size_t n) {
    size_t p = size_t(std::snprintf(buf, n, "mips %d (", v.numMips));
    for (int k = 0; k < v.numMips && p + 8 < n; ++k)
        p += size_t(std::snprintf(buf + p, n - p, "%s%d", k ? "/" : "",
                                  v.isCube ? (v.W >> k) : (v.W >> k)));
    const int virt = FeatureFlags::env_mip_chain();
    if (p + 4 < n) {
        if (virt >= 2)
            std::snprintf(buf + p, n - p, ")  select divides by N=%d (--env_mip_chain)", virt);
        else
            std::snprintf(buf + p, n - p, ")  select divides by N=%d (the real depth)", v.numMips);
    }
}

// Width OutTextXY will actually consume — its own per-glyph advance
// ((Len+2)*scale), not an 8px guess. Same computation as VizCycle's legend
// panel, and for the same reason: a backing panel sized by guess either bands
// the screen or lets the tail of the line run out over the frame unreadable
// (which is exactly what the first cut of this viewer did to its mode-3 note).
int emTextWidth(const char* s) {
    const int fs = emFontScale();
    const Font* F = Active_Font;
    if (!F || !F->Len) return int(std::strlen(s)) * 8 * fs;
    int w = 0;
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p)
        w += (int(F->Len[*p & 0x7F]) + 2) * fs;
    return w;
}

// Text line with a dim backing sized to the TEXT, left-aligned at x, truncated
// (with an ellipsis) to whatever room is left before the right edge. Returns
// the next y.
int emTextLine(int x, int y, int /*wMax*/, const char* s) {
    const int fs = emFontScale();
    const int room = XRes - x - 4 * fs;      // the frame edge, nothing tighter:
                                             // truncating to the image panel's
                                             // width would drop real data to
                                             // keep a rectangle tidy.
    char buf[288];
    std::snprintf(buf, sizeof buf, "%s", s);
    for (size_t n = std::strlen(buf); n > 3 && emTextWidth(buf) > room; ) {
        buf[--n] = 0;
        if (n > 3) { buf[n - 1] = '.'; buf[n - 2] = '.'; buf[n - 3] = '.'; }
    }
    const int w = emTextWidth(buf);
    emDim(x - 3 * fs, y - 1, x + w + 3 * fs, y + emGlyphH() + 2, 60);
    OutTextXY(VPage, x, y, buf, 255);
    return y + emPitch();
}

// The whole header block. Drawn UNDER the image so it never pushes the image
// off the top, and left-aligned with the image so the two read as one panel.
void emDrawHeader(int x, int y, int wMax, int idx, int count,
                  const ProbeIdent& id, const EnvPanoLinear& v, int mode,
                  const char* extraLine) {
    char line[256];
    std::snprintf(line, sizeof line, "ENV PROBE %d/%d  '%s'   [X = mode, F / Shift+F = probe]",
                  idx + 1, count, id.primary);
    y = emTextLine(x, y, wMax, line);

    char chain[128]; emChainText(v, chain, sizeof chain);
    if (v.isCube)
        std::snprintf(line, sizeof line, "CUBE  6 faces %d^2 (padded, pad %.2f)  %s",
                      v.W, double(kEnvCubePad), chain);
    else
        std::snprintf(line, sizeof line, "EQUIRECT PANORAMA %dx%d (legacy --no-env_cube path)  %s",
                      v.W, v.H, chain);
    y = emTextLine(x, y, wMax, line);

    std::snprintf(line, sizeof line, "bake point (%.2f %.2f %.2f)   parallax %s   %s",
                  double(v.bakeX), double(v.bakeY), double(v.bakeZ),
                  v.noParallax ? "OFF (direction-only)" : "ON (AABB proxy)",
                  id.S->imported ? "IMPORTED faces (RegisterCubeFaces)" : "baked here");
    y = emTextLine(x, y, wMax, line);

    const bool live = id.S->envDynamic && FeatureFlags::env_dynamic();
    std::snprintf(line, sizeof line, "%s   %d material(s): %s",
                  live ? (id.S->everOverlaid
                            ? "LIVE - refreshed by --env_dynamic"
                            : "LIVE-flagged - --env_dynamic has not refreshed it YET")
                       : (id.S->envDynamic
                            ? "STATIC (envDynamic flagged, but --env_dynamic is off)"
                            : "STATIC capture"),
                  id.nMats, id.matList[0] ? id.matList : "(none)");
    y = emTextLine(x, y, wMax, line);

    if (extraLine && *extraLine) y = emTextLine(x, y, wMax, extraLine);
    (void)mode;
}

// A probe with no pixel data gets an EMPTY FRAMED PANEL that says so. This
// project has been bitten three separate times by a diagnostic that rendered
// "no data" as a legitimate-looking value; a black 3x2 grid would be the
// fourth. Nothing is blitted here at all — there is nothing to blit.
void emDrawNoData(int x0, int y0, int w, int h, const char* why) {
    emDim(x0, y0, x0 + w, y0 + h, 30);
    emFrame(x0, y0, w, h, 0x00FF00FFu);
    const int fs = emFontScale();
    OutTextXY(VPage, x0 + 8 * fs, y0 + h / 2 - emGlyphH(), "NO PIXEL DATA", 255);
    OutTextXY(VPage, x0 + 8 * fs, y0 + h / 2 + 2 * fs, why, 255);
}

}  // namespace

bool EnvMapViz_Available() {
    if (!CurScene) return false;
    auto it = g_envByScene.find(CurScene);
    if (it == g_envByScene.end()) return false;
    for (const auto& s : it->second.stores)
        if (s && s->view.mip[0]) return true;
    return false;
}

bool EnvMapGpuViz_Available() {
    if (!EnvMapViz_Available()) return false;
    return !emGpuAtlasStems().empty();
}

void EnvMap_StepProbe(int dir) {
    const int n = g_envMapProbeCount;
    if (n <= 0) {
        std::fprintf(stderr, "[ENVMAP] no env probes in this scene (nothing to page "
                     "through). Needs --env_refl and a reflective surface.\n");
        return;
    }
    const int cur = FeatureFlags::env_map_probe();
    const int cur0 = cur > 0 ? ((cur - 1) % n) : 0;
    const int nxt0 = ((cur0 + (dir >= 0 ? 1 : -1)) % n + n) % n;
    char v[16];
    std::snprintf(v, sizeof v, "%d", nxt0 + 1);
    FeatureFlags::setParamFromText("env_map_probe", v);
    if (FeatureFlags::env_map_viz() <= 0)
        std::fprintf(stderr, "[ENVMAP] probe %d/%d selected, but the inspector is OFF "
                     "— press X to cycle to 'ENV probe faces' (needs --viz_arm on a "
                     "dev build), or pass --env_map_viz=1.\n", nxt0 + 1, n);
}

void EnvMap_DrawViz(Scene* sc) {
    if (!sc || !VPage || XRes <= 0 || YRes <= 0) return;
    // Never inside an offscreen render. Same reason EnvReflection_DrawViz has
    // this guard: the env bake runs a full renderFrame per cube face, and an
    // overlay drawn there gets BAKED INTO the probe being viewed.
    if (g_offscreenViewDepth > 0) return;

    auto it = g_envByScene.find(sc);
    const int count = (it == g_envByScene.end()) ? 0 : int(it->second.stores.size());
    g_envMapProbeCount = count;          // publish for the F-key stepper

    const int mode = FeatureFlags::env_map_viz();
    if (mode <= 0) return;               // ← the byte-null early-out

    const int fs = emFontScale();
    const int pitch = emPitch();
    if (count <= 0) {
        // NOT a silent return. The viz CYCLE drops the mode when there are no
        // probes (EnvMapViz_Available), but --env_map_viz=1 straight off the
        // command line under --no-env_refl reaches here, and an instrument that
        // answers "nothing" by drawing nothing is the exact failure this tree
        // has been bitten by three times. Say it, on screen.
        const int w = XRes / 2, h = 6 * pitch;
        const int x = (XRes - w) / 2, y = 8 * fs;
        emDrawNoData(x, y, w, h, "no env probe exists in this scene");
        char why[192];
        std::snprintf(why, sizeof why,
            "--env_refl is %s. A probe needs a surface with Reflection > 0 or a "
            "metalness map.", FeatureFlags::env_refl() ? "ON" : "OFF");
        emTextLine(x, y + h + pitch, w, why);
        static bool noted = false;
        if (!noted) {
            noted = true;
            std::fprintf(stderr, "[ENVMAP] --env_map_viz=%d but this scene has NO env "
                         "probes (env_refl=%d). Nothing to inspect.\n",
                         mode, (int)FeatureFlags::env_refl());
        }
        return;
    }

    const SceneEnv& env = it->second;
    const int want = FeatureFlags::env_map_probe();
    const int idx  = want > 0 ? ((want - 1) % count) : 0;

    ProbeIdent id;
    if (!emProbeIdent(env, idx, id)) return;
    const EnvPanoLinear& v = id.S->view;

    const int headerLines = 5;
    const int margin = 8 * fs;
    // Vertical budget: the frame minus a top margin, the header block under
    // the image, and the bottom strip the viz legend + mode name own
    // (VizCycle draws those up from YRes - 20*g_fontScale).
    const int maxW = XRes - 2 * margin;
    const int maxH = YRes - margin - headerLines * pitch - 26 * fs;

    // ── report the selection once, on stderr, in full ────────────────────
    // Printed from the RENDER thread (where reading the store is safe), not
    // from the key handler.
    {
        static const Scene* lastScene = nullptr;
        static int lastIdx = -1, lastMode = -1;
        if (idx != lastIdx || mode != lastMode || sc != lastScene) {
            lastIdx = idx; lastMode = mode; lastScene = sc;
            char chain[128]; emChainText(v, chain, sizeof chain);
            std::fprintf(stderr,
                "[ENVMAP] probe %d/%d '%s' — %s, %s, bake (%.2f %.2f %.2f), "
                "parallax %s, %s, %d material(s): %s%s\n",
                idx + 1, count, id.primary,
                v.isCube ? "padded CUBE" : "equirect PANORAMA",
                chain, double(v.bakeX), double(v.bakeY), double(v.bakeZ),
                v.noParallax ? "off" : "on",
                (id.S->envDynamic && FeatureFlags::env_dynamic()) ? "LIVE (--env_dynamic)"
                                                                  : "static",
                id.nMats, id.matList[0] ? id.matList : "(none)",
                v.mip[0] ? "" : "  *** NO PIXEL DATA ***");
        }
    }

    // ── mode 2: THE MIP CHAIN ────────────────────────────────────────────
    if (mode == 2) {
        // Levels left-to-right at NATIVE RELATIVE size. The halving IS the
        // information: --env_mip_chain exists because the chain's depth (and
        // therefore how far a given roughness walks down it) is the mechanism
        // behind "the GPU melts the ribs into a mirror sweep".
        int srcW = 0, srcH = 0, nLv = 0;
        for (int k = 0; k < v.numMips; ++k) {
            const AtlasSrc s = srcForLevel(v, k);
            if (!s.valid()) break;
            srcW += s.w() + 4; if (s.h() > srcH) srcH = s.h();
            ++nLv;
        }
        if (nLv == 0) {
            const int w = maxW / 2, h = maxH / 3;
            const int x0 = (XRes - w) / 2, y0 = margin;
            emDrawNoData(x0, y0, w, h, "this probe has no baked levels");
            emDrawHeader(x0, y0 + h + pitch, w, idx, count, id, v, mode, nullptr);
            return;
        }
        const int scale = emFitScale(srcW, srcH, maxW, maxH);
        int x = (XRes - (scale >= 1 ? srcW * scale : (srcW - 1) / (-scale) + 1)) / 2;
        if (x < margin) x = margin;
        const int y0 = margin + pitch;   // room for the per-level label above
        int bottom = y0;
        const int xStart = x;
        for (int k = 0; k < nLv; ++k) {
            const AtlasSrc s = srcForLevel(v, k);
            int dw, dh; emScaledDims(s, scale, dw, dh);
            emBlit(s, x, y0, scale);
            emFrame(x, y0, dw, dh, 0x0000C0FFu);
            if (s.cube) emLabelCubeCells(x, y0, dw / 3, dh / 2);
            char lab[64];
            std::snprintf(lab, sizeof lab, "L%d  %d%s", k, s.cube ? s.fr : s.W,
                          s.cube ? "^2 x6" : "");
            emDim(x, y0 - pitch, x + dw, y0 - 1, 60);
            OutTextXY(VPage, x + 2 * fs, y0 - pitch + fs, lab, 255);
            if (y0 + dh > bottom) bottom = y0 + dh;
            x += dw + (scale >= 1 ? 4 * scale : 4);
        }
        char extra[192];
        std::snprintf(extra, sizeof extra,
            "level select: lvlF = roughness * (N-1); a mirror reads L0, roughness 1 reads L%d",
            v.numMips - 1);
        emDrawHeader(xStart, bottom + pitch, maxW, idx, count, id, v, mode, extra);
        return;
    }

    // ── modes 1 and 3: THE SIX FACES ─────────────────────────────────────
    const AtlasSrc s0 = srcForLevel(v, 0);
    const bool wantGpu = (mode == 3);

    // Mode 3 pairs our atlas with GpuBench's for the same material.
    PpmImage gpu;
    std::string gpuStem;
    std::vector<std::string> stems;
    if (wantGpu) {
        stems = emGpuAtlasStems();
        gpuStem = emMatchGpuStem(id.primary, stems);
        if (!gpuStem.empty()) {
            const std::string p = std::string(kGpuEnvDir) + "/" + kGpuEnvPrefix
                                + gpuStem + ".ppm";
            if (!emReadPpmP6(p.c_str(), gpu)) gpu = PpmImage();
        }
    }

    if (!s0.valid()) {
        const int w = maxW / 2, h = maxH / 2;
        const int x0 = (XRes - w) / 2, y0 = margin;
        emDrawNoData(x0, y0, w, h,
                     "store exists but mip[0] is null - not baked yet, or the bake failed");
        emDrawHeader(x0, y0 + h + pitch, w, idx, count, id, v, mode, nullptr);
        return;
    }

    // Two panels side by side in mode 3, one otherwise. Both panels are drawn
    // at the SAME displayed size so the eye compares content, not scale.
    const int panels = (wantGpu ? 2 : 1);
    const int gapX   = 10 * fs;
    const int panelMaxW = (maxW - (panels - 1) * gapX) / panels;
    const int scale  = emFitScale(s0.w(), s0.h(), panelMaxW, maxH - pitch);
    int dw, dh; emScaledDims(s0, scale, dw, dh);
    const int totalW = panels * dw + (panels - 1) * gapX;
    int x0 = (XRes - totalW) / 2; if (x0 < margin) x0 = margin;
    const int y0 = margin + pitch;

    emBlit(s0, x0, y0, scale);
    emFrame(x0, y0, dw, dh, 0x0000FF00u);
    if (s0.cube) emLabelCubeCells(x0, y0, dw / 3, dh / 2);
    {
        char lab[96];
        std::snprintf(lab, sizeof lab, "CPU  %s  mip 0",
                      s0.cube ? "3x2 cube atlas (+X -X +Y / -Y +Z -Z)"
                              : "equirect panorama");
        emDim(x0, y0 - pitch, x0 + dw, y0 - 1, 60);
        OutTextXY(VPage, x0 + 2 * fs, y0 - pitch + fs, lab, 255);
    }

    char extra[224] = {};
    if (wantGpu) {
        const int gx = x0 + dw + gapX;
        if (gpu.ok()) {
            AtlasSrc gs;
            // GpuBench's atlas is a flat 3x2 image; treat it as a cube atlas
            // when its aspect says so, so the SAME cell labels apply.
            if (gpu.W == gpu.H * 3 / 2 && (gpu.W % 3) == 0) {
                gs.px = gpu.px.data(); gs.cube = true; gs.fr = gpu.W / 3;
            } else {
                gs.px = gpu.px.data(); gs.W = gpu.W; gs.H = gpu.H;
            }
            const int gscale = emFitScale(gs.w(), gs.h(), dw, dh);
            int gdw, gdh; emScaledDims(gs, gscale, gdw, gdh);
            emBlit(gs, gx, y0, gscale);
            emFrame(gx, y0, gdw, gdh, 0x00FF8000u);
            if (gs.cube) emLabelCubeCells(gx, y0, gdw / 3, gdh / 2);
            char lab[96];
            std::snprintf(lab, sizeof lab, "GPU  gpuenv_%s.ppm  %dx%d  (sqrt-encoded)",
                          gpuStem.c_str(), gpu.W, gpu.H);
            emDim(gx, y0 - pitch, gx + gdw, y0 - 1, 60);
            OutTextXY(VPage, gx + 2 * fs, y0 - pitch + fs, lab, 255);
            std::snprintf(extra, sizeof extra,
                "CPU faces are 8-bit LDR bake texels; the GPU atlas is linear RGBA16F "
                "sqrt-encoded: compare STRUCTURE, not absolute brightness");
        } else {
            emDrawNoData(gx, y0, dw, dh,
                         gpuStem.empty() ? "no gpuenv_*.ppm matches this material"
                                         : "gpuenv atlas present but unreadable");
            std::string have;
            for (size_t i = 0; i < stems.size() && have.size() < 90; ++i)
                have += (i ? ", " : "") + stems[i];
            std::snprintf(extra, sizeof extra,
                "GPU atlases in /tmp: %s   (make them: GpuBench --dump_env_cube)",
                have.empty() ? "NONE" : have.c_str());
        }
    }

    emDrawHeader(x0, y0 + dh + pitch, totalW, idx, count, id, v, mode,
                 extra[0] ? extra : nullptr);
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

// ── THE AUTO-CENTER BUTTON ────────────────────────────────────────────────
// out = (--env_probe_center's corrected point) - (the point the derivation
// that is ACTUALLY RUNNING right now produces). Written into
// Material::EnvBakeOfs — which the bake adds on top of whichever derivation
// ran — that difference lands the capture point on the corrected point in
// BOTH flag states, which is the whole reason it is a difference and not the
// corrected point itself: with --env_probe_center already on, the two
// derivations agree and the button correctly writes (0,0,0).
//
// It DELIBERATELY does not read the live store's bake point. The store may be
// shared, may have been baked at a different flag state, and may not exist
// yet; the geometry is the ground truth in every one of those cases.
bool EnvReflection_AutoCenterOffset(Scene* sc, const Material* M, float out[3]) {
    if (!sc || !M || !out) return false;
    Vector corrected, active;
    if (!materialCentroid(sc, M, corrected, nullptr, /*areaMode=*/1, /*quiet=*/true))
        return false;
    if (!materialCentroid(sc, M, active,    nullptr, /*areaMode=*/-1, /*quiet=*/true))
        return false;
    out[0] = corrected.x - active.x;
    out[1] = corrected.y - active.y;
    out[2] = corrected.z - active.z;
    std::fprintf(stderr, "[ENVREFL] '%s': auto-center — active derivation "
        "(%.2f %.2f %.2f), corrected (%.2f %.2f %.2f) -> probe offset "
        "(%+.2f %+.2f %+.2f)%s\n", M->Name ? M->Name : "?",
        active.x, active.y, active.z, corrected.x, corrected.y, corrected.z,
        out[0], out[1], out[2],
        fds::FeatureFlags::env_probe_center()
            ? "  (--env_probe_center is ON, so the derivation already lands there)"
            : "");
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
