#ifndef REVIVAL_LIGHTMAP_BAKE_H
#define REVIVAL_LIGHTMAP_BAKE_H

#include <cstdint>

// Static shadow-lightmap bake (Option A from docs/STATIC_SHADOW_LIGHTMAPS.md).
//
// For each cube-shadow-casting omni × each effectively-static TriMesh × each
// face, build a small barycentric atlas (default 16×16) of pre-baked shadow
// factors by sampling the omni's already-rasterized cube depth maps at every
// texel's world-space position. The lightmap hangs off TriMesh::staticShadowLM
// and is read at runtime by the deferred kernel to skip the per-pixel cube
// tap on static geometry.
//
// Call AFTER ShadowMaps_BakeStatic — the bake reads sm.packSD (static occluder
// depth) populated by that pass.
//
// No-op when --shadow-lightmap is off, when the scene has zero cube shadow
// casters, or when no mesh qualifies as "effectively static".

struct Scene;
struct CubeShadowRef;
struct Vector;

namespace fds {

// forceEnable: bake regardless of the global --shadow-lightmap flag. A scene
// that wants baked lightmaps (greets) passes true so the bake runs at init even
// though the per-pixel SAMPLE flag (shadow_lightmap) is left off until that
// scene actually renders — keeps the flag from leaking onto other scenes.
void LightmapBake_Static(Scene *Sc, bool forceEnable = false);

// The one side effect of LightmapBake_Static that is NOT about lightmaps, so
// that a scene which SKIPS the bake can still pay for it (it is microseconds).
// Stamps Face::MeshFaceIdx over exactly the mesh set the bake would keep.
// MeshFaceIdx's second consumer is tbrXparOrderLess (FILLERS.CPP:1876) — the
// camera-independent tie-break of the per-strip transparent sort — which is
// live on every frame whether or not any lightmap exists. Full rationale at
// the definition. Idempotent; safe to call alongside a bake that also stamps.
void LightmapStampFaceIndices(Scene *Sc);

// Stamp every static-mesh face's (A, B, C) vertices with their object-
// space barycentric weight on the face itself: A→(0,0), B→(1,0), C→(0,1).
// The clipper then interpolates these perspective-correctly to any
// clip-generated vertex, and the rasterizer reads per-pixel bary as a
// standard perspective-correct attribute (same machinery as UV). Must
// be called once per scene at init, before the first frame transforms
// vertices (so the stamps land on the actual Vertex objects the runtime
// will see, not on stale copies). No-op when --shadow-lightmap is off.
void LightmapStampOrigBary(Scene *Sc);

// Debug: bake-time sampler exposed for runtime comparison via the
// --shadow-lightmap-recompute-bake flag. Replaces sampleBilinear in
// resolveCubeAtten with a fresh per-pixel call to the same code path
// the bake used. If the output looks correct, the bake function is
// equivalent to the runtime cube tap and any visible lightmap bug is
// in the atlas storage / bary lookup. If the output looks wrong (same
// as the current lightmap output), the bake function itself diverges
// from CubeShadow_Sample — fix the bake math, not the bary path.
// Returns shadow factor in [0, 255]; 255 = fully lit.
uint8_t LightmapBake_DebugSampleAtWorld(int cubeIdx, float wx, float wy, float wz,
                                          int constBias, int slopeBiasInt);

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
void Render_NormalViz(Scene *Sc);   // --nmap_viz debug (post-tonemap, like LightmapViz)

}  // namespace fds

#endif  // REVIVAL_LIGHTMAP_BAKE_H
