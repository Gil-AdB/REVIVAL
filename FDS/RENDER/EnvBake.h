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

}  // namespace fds

#endif  // FDS_ENV_BAKE_H_INCLUDED
