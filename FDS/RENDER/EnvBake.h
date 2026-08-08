#ifndef FDS_ENV_BAKE_H_INCLUDED
#define FDS_ENV_BAKE_H_INCLUDED

// Equirectangular environment baking. Renders a scene as six axis-aligned
// cube faces from a point, stitches them into one equirectangular panorama,
// and hands back a Sachletz-tiled Texture ready to drop into
// Face::ReflectionTexture / Material::EnvTexture (the forward reflective
// filler — TheOtherBarry<OVERWRITE, TEXTURETEXTURE> — samples exactly this
// layout, matching the convention in RENDER/Transform.cpp's reflective
// vertex pass).
//
// The CITY per-building cube-map bake (DEMO/CITY.CPP) and the disco-ball
// panorama predate this and hand-roll the same dance — candidates for
// adoption once this has proven out on the mirror shards.

#include <cstdint>

struct Scene;
struct Vector;
struct Texture;
struct Material;

namespace fds {

// Parameters for a single panorama bake. Defaults give a 512² cube → 1024²
// panorama, the resolution CITY uses for building reflections.
struct EnvBakeParams {
    int   cubeRes    = 512;          // per-face square render resolution
    int   panoWidth  = 1024;         // equirectangular output width
    int   panoHeight = 1024;         // equirectangular output height
    // Empty directions (no geometry) resolve to this ARGB color rather than
    // pure black — a dim ambient reads better than holes in a reflection.
    unsigned voidColor = 0xFF202020u;
};

// Bake a 360° panorama of `sc` as seen from `center`. Renders through an
// OffscreenViewScope (locks out EngineResize, swaps + restores MainSurf /
// View / FOV / clip planes), so it is safe to call from a scene's tick on
// the render thread. The caller is responsible for hiding any geometry that
// must not appear in the reflection (the reflector itself, mirror clones)
// before calling and restoring it after.
//
// Returns a newly-allocated Texture* (caller owns it; lives for the scene).
Texture* BakeEquirectPanorama(Scene* sc, const Vector& center,
                              const EnvBakeParams& params = {});

// ── Deferred env-specular reflection (--env_refl) ─────────────────────────
// PER-SURFACE linear (unswizzled) equirect panoramas the deferred kernel
// samples for materials with Reflection > 0 / a metalness map. Each
// reflective material gets a panorama baked from ITS OWN centroid (a pano
// baked from the camera is a picture from the wrong place for everything
// else — the editor's far orbit pose made reflections look broken), and the
// lookup is PARALLAX-CORRECTED: the reflected ray is intersected with the
// scene's AABB proxy from the pixel's WORLD position, and the direction to
// that hit point (from the bake centroid) indexes the pano. That's what
// makes a floor/wall reflection track position instead of being pasted on —
// the "local cubemap" standard; per-texel lightmap-style capture is overkill.
struct EnvPanoLinear {
    // Pre-filtered mip chain: mip[0] = full W×H, each next level a 2×2 box
    // downsample (half dims) — the kernel picks the level from the pixel's
    // ROUGHNESS, so rough surfaces reflect a blurred environment instead of
    // a dimmed sharp one (the physically-right look).
    static constexpr int kMaxMips = 4;
    const unsigned* mip[kMaxMips] = {};
    int W = 0, H = 0;               // mip 0 dims (level k = W>>k × H>>k)
    int numMips = 0;
    // env_cube mode (D2): when true, each mip[k] is a FACE-MAJOR block of six
    // padded cube faces, each (W>>k)×(W>>k) (W==H==faceRes), face f at offset
    // f*(W>>k)*(W>>k). Sampled via EnvCube_DirToFaceUV — no equirect wrap, no
    // trig. When false, mip[k] is the legacy equirect image (W>>k × H>>k).
    bool isCube = false;
    // Skip the AABB parallax correction for this store (direction-only,
    // env-at-infinity). Set for stores whose proxy box would be a bad fit
    // (CITY: the whole-city AABB drew moving exit-face bands across
    // facades — the per-pixel experiment's "garbled and jumpy").
    bool noParallax = false;
    // Forward-path cv-pull damping, ported per-pixel (CITY glass): when > 0,
    // the reflection eye is pulled toward the bake point with the SAME
    // pow(dist-opt, 0.8)+opt law Transform.cpp's Face_Reflective block uses.
    // The authored city look depends on this: it slows reflection sweep to
    // ~1/3 of physical at overview distances (full-rate per-pixel motion
    // read as "jumps like crazy" against it). Value = the building bsphere
    // radius (the forward hack's optimalDistFromPlane analog). 0 = physical.
    float pullOpt = 0.0f;
    // Capture point (world) + the scene AABB proxy for parallax correction.
    float bakeX = 0, bakeY = 0, bakeZ = 0;
    float boxMinX = 0, boxMinY = 0, boxMinZ = 0;
    float boxMaxX = 0, boxMaxY = 0, boxMaxZ = 0;
};

// Per-frame prep, called from renderFrame's MAIN-camera pass (never from a
// nested OffscreenViewScope — the bake takes the non-reentrant engine-surface
// mutex): computes the scene AABB once, bakes a panorama from the centroid of
// every reflective material that lacks one (deduped — materials whose
// centroids sit within a few units share a bake, e.g. ::mirUV clones), and
// refreshes the matID→pano table. Returns true if any bake ran this call
// (the caller must re-Transform the frame — the bake clobbered it).
bool EnvReflection_FramePrep(Scene* sc);

// matID-indexed pano table (256 entries; null = material not reflective /
// not yet baked). Rebuilt by FramePrep; stable within a frame.
const EnvPanoLinear* const* EnvReflection_Table(Scene* sc);

// ── Dynamic-mesh reflection overlay (--env_dynamic, ENVDYN Workstream A3) ──
// For each env probe flagged Material::EnvDynamic (A1) whose static capture
// was retained (A2), re-render the scene's DYNAMIC meshes (the mech) into the
// probe's touched cube faces and composite them over the static master, so
// movers appear LIVE in the reflection. Ordered like RenderSecondOrderMirrors
// — call from the scene tick BEFORE the main Transform, on the main-camera
// path (NOT inside FramePrep). Per-frame budget = env_dynamic_budget flagged
// probes. No-op unless --env_dynamic (+ --env_refl / --env_cube) and at least
// one flagged, on-screen, mech-relevant probe exists — byte-null otherwise.
void EnvDynamic_Overlay(Scene* sc);

// ── SH irradiance ambient (--sh_ambient) ──────────────────────────────────
// One-shot per-scene bake of a 9-coefficient L2 spherical-harmonic RGB
// irradiance probe. Renders a small (kSHFaceRes²) environment cube from the
// scene's AABB centre through the SAME deferred cube-face path the env-
// reflection probes use, then projects it into 27 floats with the standard
// cosine-convolution A_l coefficients folded in (divided by pi, so a uniform
// environment of colour C evaluates back to C — magnitude-comparable to the
// flat Sc->Ambient it replaces). Coefficient layout is channel-major:
// c[0..8] = B basis 0..8, c[9..17] = G, c[18..26] = R. Basis order is
// Y00, Y1-1(y), Y10(z), Y11(x), Y2-2(xy), Y2-1(yz), Y20(3z²-1), Y21(xz),
// Y22(x²-y²) with the usual normalisation constants (see the kernel's shEval).
//
// EnsureBaked returns true only on the frame it actually baked (renders six
// cube faces → clobbers the transformed frame state, so the caller must
// re-Transform + re-sort, exactly like EnvReflection_FramePrep). Cached
// afterwards. Coeffs returns the 27 floats, or null if not yet baked / the
// bake failed. Called from renderFrame's main-camera pass only (never nested).
bool SHAmbient_EnsureBaked(Scene* sc);
const float* SHAmbient_Coeffs(Scene* sc);

// Drop every bake for the scene so the next FramePrep re-renders them
// (editor: after light/material edits that should show in reflections).
// Whole-scene — use only for the "rebake all" button and global flag changes
// that really do affect every probe.
void EnvReflection_Invalidate(Scene* sc);

// TARGETED invalidate: drop ONLY the probe store material M maps to, plus
// every material that SHARES that store (the FramePrep 4-world-unit sharing
// group — ::mirUV clones, co-located panels), so the next FramePrep re-bakes
// just that one probe. Per-surface editor edits (metallic import/reset,
// envRefl/envBakeRes) call this instead of the whole-scene drop: in the 4GB
// wasm editor, greets now carries many reflective surfaces and re-baking them
// ALL at once (each probe = a color surface + Z16 + 6 face buffers) OOM'd the
// browser. One store re-bake instead of N. No-op if M has no store yet
// (FramePrep bakes it fresh anyway). The dropped store's own surface still
// re-bakes — the "metallic has no effect" fix is preserved.
void EnvReflection_InvalidateSurface(Scene* sc, const Material* M);

// --env_refl_viz=N: blit the Nth (1-based) baked panorama's mip 0 over the
// frame's top-right corner (downscaled to fit) — post-tonemap debug viewer.
// N beyond the store count clamps to the last one; 0 = off (no-op).
void EnvReflection_DrawViz(Scene* sc);

// ── THE ENV-MAP INSPECTOR (--env_map_viz) ─────────────────────────────────
// A LOOK-AT-THE-PROBES viewer for the interactive window. Until this existed
// the only way to see a baked probe was FDS_ENVBAKE_DUMP writing PPMs to /tmp:
// offline, one file per material, and you had to know it was there. It cost a
// whole investigation's worth of time (docs/SHADING_CONTRACT.md §11, which
// argues from probe dumps throughout) to build the instrument mid-flight.
//
// It is a VIEWER: it reads the stores EnvReflection_FramePrep already built,
// re-bakes nothing, perturbs no frame state, and paints VPage in the same
// post-tonemap overlay slot as EnvReflection_DrawViz. Default off, so it is
// byte-null on a shipping frame by construction (one integer compare).
//
// Modes are --env_map_viz (1 faces / 2 mip chain / 3 CPU|GPU); the probe is
// --env_map_probe, paged live with F / Shift+F. Both are ordinary FeatureFlags
// so the X-key viz cycle offers the modes like every other viz.
void EnvMap_DrawViz(Scene* sc);

// F / Shift+F from the SDL event pump: step --env_map_probe by `dir`, wrapping
// on the probe count the last drawn frame published. Writes the flag through
// FeatureFlags::setParamFromText — same path as the cycle and the console —
// and prints nothing itself; the render thread reports the new probe's
// identity when it draws it, which is also where the store is safe to read.
void EnvMap_StepProbe(int dir);

// Availability probe for the viz cycle, defined beside the painter whose
// early-outs it mirrors (the VizCycle.cpp convention). True when the CURRENT
// scene has at least one probe store WITH pixel data.
bool EnvMapViz_Available();

// True when at least one GpuBench --dump_env_cube atlas is present on disk for
// the current scene's probes — the availability probe for --env_map_viz=3.
bool EnvMapGpuViz_Available();

// Number of baked panorama stores for the scene (viewer paging bound).
int EnvReflection_Count(Scene* sc);

// Store index a material maps to (-1 = none / not reflective / not yet
// baked). Editor UI: the surface panel's env-reflection state indicator and
// the pano viewer's jump-to-selected-surface both key off this.
int EnvReflection_StoreIndex(Scene* sc, const Material* M);

// Register an EXTERNALLY baked padded-cube store for material M: takes the
// face-major linear RGBA faces (6 x faceRes^2, EnvCube convention — e.g.
// CITY's per-building bake, already disk-cached), box-downsamples to
// storeRes if smaller, builds the mip chain, marks the store noParallax,
// and maps M to it (FramePrep then skips M — no redundant centroid bake).
// Returns the store index for AliasMaterial, or -1 on failure.
int EnvReflection_RegisterCubeFaces(Scene* sc, Material* M,
                                    const uint32_t* faceMajor, int faceRes,
                                    int storeRes, const Vector& bakePoint,
                                    float pullOpt = 0.0f);

// Map another material to an existing store (same building, second windows
// base-mat clone) without duplicating the pixel data.
void EnvReflection_AliasMaterial(Scene* sc, Material* M, int storeIdx);

// ── Live water in baked env reflections (--env_live_water) ────────────────
// The city env bakes are one-shot statics: the water region in every
// building's cube faces is a FROZEN flat-mirror image, visibly dead against
// the live main-view water (ripple dispMap + glints). Instead of touching
// the baked texels (two consumer representations, occlusion-unaware water
// masks, per-frame texel churn — evaluated and rejected), the SAMPLERS
// perturb the lookup direction: when the ray from the store's bake point
// hits the water plane, the direction is tilted by the same animated
// wave-slope field the main-view reflection ripple uses (pwater::WaveSlope
// — the scene publishes a function pointer, FDS cannot link DEMO code).
// Exactly the dispMap trick, in env space.
//
// The scene's tick publishes this every frame BEFORE renderFrame (same
// thread ordering as the rest of the frame state; tiles only read it).
// t derives from g_FrameTime → fixed-t snapshots are md5-stable. Scenes
// that never publish leave active=false and every consumer no-ops.
struct EnvLiveWaterState {
    bool  active = false;
    float waterY = 0.0f;
    float minX = 0.0f, maxX = 0.0f, minZ = 0.0f, maxZ = 0.0f;
    float amp = 0.0f;          // direction-perturb amplitude (env_live_water_amp)
    float t = 0.0f;            // wave clock: g_FrameTime * 0.02 * ripple_speed
    float scale = 1.0f;        // wave spatial scale: water_bump_scale
    void (*slopeFn)(float wx, float wz, float t, float scale,
                    float& sx, float& sz) = nullptr;
};
extern EnvLiveWaterState g_envLiveWater;

// Perturb an env lookup direction (world space, any scale) in place when it
// hits the live-water plane. bakeX/Y/Z = the capture point the content was
// baked from (EnvPanoLinear::bake* for the deferred stores, the building
// probe center for the forward sheets). Inline: the callers are per-pixel /
// per-vertex hot paths and the inactive case must stay a single branch.
inline void EnvLiveWater_PerturbDir(float bakeX, float bakeY, float bakeZ,
                                    float& dx, float& dy, float& dz)
{
    const EnvLiveWaterState& lw = g_envLiveWater;
    if (!lw.active) return;
    if (dy >= -1e-6f) return;                       // above horizon
    if (bakeY <= lw.waterY) return;                 // probe under water: n/a
    // Plane hit from the bake point — UNBOUNDED, like the main view's
    // dispMap ripple (updateRippleDispMap ray-casts every pixel to the
    // plane with no extent test; the flooded city reads as water in every
    // below-horizon direction). Baked non-water content below the horizon
    // (bridges, piers) wobbles too — accepted approximation: the bake has
    // no depth / water mask to tell them apart, and the wobble amplitude
    // is a few texels. dx/dy/dz may be UNNORMALIZED (the cube lookup is
    // scale-invariant): the hit point is scale-invariant (t·d), and the
    // tilt k = amp·|dy| scales WITH the vector so the angular perturbation
    // is scale-invariant as well.
    const float t = (lw.waterY - bakeY) / dy;       // dy<0 → t>0
    const float hx = bakeX + t * dx;
    const float hz = bakeZ + t * dz;
    float sx = 0.0f, sz = 0.0f;
    lw.slopeFn(hx, hz, lw.t, lw.scale, sx, sz);
    // Tilt ∝ |dy| so grazing rays (huge t, tiny dy) don't overshoot: the
    // same slope reads as a gentler direction change near the horizon,
    // which also fades the effect exactly where the plane hit is least
    // certain (occluders far from the probe).
    const float k = lw.amp * -dy;
    dx += k * sx;
    dz += k * sz;
}

}  // namespace fds

#endif  // FDS_ENV_BAKE_H_INCLUDED
