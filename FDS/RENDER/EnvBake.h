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

struct Scene;
struct Vector;
struct Texture;

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

// Drop every bake for the scene so the next FramePrep re-renders them
// (editor: after light/material edits that should show in reflections).
void EnvReflection_Invalidate(Scene* sc);

// --env_refl_viz=N: blit the Nth (1-based) baked panorama's mip 0 over the
// frame's top-right corner (downscaled to fit) — post-tonemap debug viewer.
// N beyond the store count clamps to the last one; 0 = off (no-op).
void EnvReflection_DrawViz(Scene* sc);

// Number of baked panorama stores for the scene (viewer paging bound).
int EnvReflection_Count(Scene* sc);

}  // namespace fds

#endif  // FDS_ENV_BAKE_H_INCLUDED
