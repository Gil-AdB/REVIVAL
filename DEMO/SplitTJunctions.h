#ifndef REVIVAL_SPLITTJUNCTIONS_H
#define REVIVAL_SPLITTJUNCTIONS_H

// SplitMeshTJunctions: pre-displacement topological repair for stone meshes.
//
// Detects authored T-junctions (a vertex of one face lying strictly inside an
// edge of another face) among target material faces and splits the host
// triangles so that all adjacent faces share conforming, watertight edges.
//
// In greets, the authored stone mesh (Piramid.lwo) contains 40 T-junctions
// along 17 host lines. Splitting them transforms 226 faces into 266 conforming
// faces without moving any vertex position.

struct Scene;

namespace rev {

// Returns the number of T-junction splits performed (0 if none found).
int SplitMeshTJunctions(Scene *Sc, const char *const *mats, int nMats, float eps = 1e-3f);

}  // namespace rev

#endif  // REVIVAL_SPLITTJUNCTIONS_H
