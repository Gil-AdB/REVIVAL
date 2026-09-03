#ifndef REVIVAL_MESHDUMP_H
#define REVIVAL_MESHDUMP_H

// --greets_mesh_dump: write the greets stone faces ('rooms', 'floor') as a
// Wavefront OBJ, once BEFORE the displacement bake (the authored mesh: authored
// vertex sharing, per-face UV, engine facing) and once AFTER it (the baked,
// tessellated, displaced mesh). Written 2026-09-03 so an EXTERNAL engine
// (Blender via the bpy module) can displace the same mesh with the same height
// map from the same camera and stand as an independent reference for what a
// displaced junction should look like - his ask after the in-tree reference
// renderer and the bake were both ruled broken at the failing poses.
//
// Conventions: positions as the engine holds them (world units); one OBJ
// vertex per engine Vertex (so authored sharing and T-junctions survive as
// authored); vt = (u, 1 - v) so an image sampled bottom-up in the reader hits
// the texel the engine samples top-down; corners ordered so the winding
// normal agrees with the engine's face normal (Face::N).

struct Scene;

namespace rev {

// Returns the number of faces written, 0 if nothing matched or the file could
// not be opened. `tag` goes into the OBJ header comment.
long long DumpStoneObj(Scene *Sc, const char *const *mats, int nMats, const char *path, const char *tag);

}  // namespace rev

#endif  // REVIVAL_MESHDUMP_H
