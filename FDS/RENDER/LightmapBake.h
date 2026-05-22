#ifndef REVIVAL_LIGHTMAP_BAKE_H
#define REVIVAL_LIGHTMAP_BAKE_H

// Static shadow-lightmap bake (Option A from docs/STATIC_SHADOW_LIGHTMAPS.md).
//
// For each cube-shadow-casting omni × each effectively-static TriMesh × each
// face, build a small barycentric atlas (default 16×16) of pre-baked shadow
// factors by sampling the omni's already-rasterized cube depth maps at every
// texel's world-space position. The lightmap hangs off TriMesh::staticShadowLM
// and is read at runtime by the deferred kernel to skip the per-pixel cube
// tap on static geometry.
//
// Call AFTER ShadowMaps_BakeStatic — the bake reads sm.depth (static occluder
// depth) populated by that pass.
//
// No-op when --shadow-lightmap is off, when the scene has zero cube shadow
// casters, or when no mesh qualifies as "effectively static".

struct Scene;

namespace fds {

void LightmapBake_Static(Scene *Sc);

// Post-deferred-lighting debug viz. Reads the new G-buffer lightmap
// planes (lightmapMF, lightmapST) plus the scene's staticLMTable and
// overrides VPage according to --shadow-lightmap-viz:
//   1 = mesh ID (greyscale; 0 = red overlay for "no lightmap")
//   2 = face ID (color hash)
//   3 = bary s in red→green
//   4 = bary t in red→blue
//   5 = baked shadow factor for omni 0 (greyscale)
// 0 (default) = no-op.
void Render_LightmapViz(Scene *Sc);

}  // namespace fds

#endif  // REVIVAL_LIGHTMAP_BAKE_H
