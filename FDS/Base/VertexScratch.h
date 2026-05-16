#ifndef FDS_VERTEX_SCRATCH_H_INCLUDED
#define FDS_VERTEX_SCRATCH_H_INCLUDED

#include <unordered_map>
#include <vector>

struct Face;
struct TriMesh;
struct Vertex;

namespace fds {

// Per-pass clone of the scene's mutable per-vertex state (Vertex::TPos,
// PX, PY, RZ, UZ, VZ, Flags, TN, TTangent, EUZ, EVZ, BGRA).
//
// Transform_Objects writes ~10 fields per vertex into TriMesh::Verts[i]
// — the per-vertex projection scratch. Cross-light parallel shadow
// rendering needs one set of those per concurrent light pass; otherwise
// concurrent passes race on the same Vertex storage.
//
// Layout: one PerTriMeshClone per TriMesh (lazy-built on first request).
// The clone owns its own Vertex[] (copied from TriMesh::Verts) and its
// own Face[] (copied from TriMesh::Faces, with A/B/C pointers remapped
// to point into the cloned Vertex[]). Downstream code (clipper /
// rasterizer / lighting) reads Face::A->PX etc. and gets the clone's
// per-pass values, no further indirection needed.
//
// Memory: ~144 B/Vertex × VIndex per TriMesh, plus ~120 B/Face × FIndex.
// For a typical greets-class scene: ~12 MB per scratch; bounded by the
// number of concurrent shadow passes (≤ N_LIGHTS).
//
// Pass `VertexScratch *` to Transform_Objects to redirect its writes
// into the clone (and have it push the clone's Face* into FList).
// Nullptr (default) → writes into TriMesh::Verts as before — main pass
// keeps the no-allocation fast path.
struct PerTriMeshClone {
    std::vector<Vertex> verts;
    std::vector<Face>   faces;

    // True once verts/faces have been populated for this TriMesh. The
    // first request for a TriMesh's clone copies + remaps; subsequent
    // requests in the same frame reuse the storage (Transform_Objects
    // rewrites the per-pass projection fields in place).
    bool initialized = false;
};

struct VertexScratch {
    std::unordered_map<TriMesh*, PerTriMeshClone> clones;

    // Lazily build (or reuse) the clone for `T`. Returns a reference
    // valid until this VertexScratch is destroyed.
    PerTriMeshClone& cloneOf(TriMesh* T);
};

} // namespace fds

#endif
