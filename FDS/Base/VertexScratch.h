#ifndef FDS_VERTEX_SCRATCH_H_INCLUDED
#define FDS_VERTEX_SCRATCH_H_INCLUDED

#include <unordered_map>
#include <vector>

#include "VertexFrame.h"

struct Face;
struct TriMesh;
struct Vertex;

namespace fds {

// Per-pass clone of the scene's mutable per-vertex state (Vertex::TPos_AOS,
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
    // SoA refactor (Phase 4): per-light SoA companion of this clone's
    // transformed-vertex output fields. Mirrors TriMesh::frame but
    // isolated to this per-light scratch so concurrent shadow passes
    // don't race. Sized to T->VIndex in cloneOf; written alongside the
    // AoS clone.verts in Transform_Objects' shadow path. Indexed by
    // Face::A_idx/B_idx/C_idx (same indices as the main path — clone
    // preserves the original index ordering). Held by value: the
    // unordered_map's node-based storage keeps the address stable;
    // VertexFrame is non-copyable but its default ctor + ensureSized
    // are enough for default-emplace use.
    VertexFrame         frame;

    // True once verts/faces have been populated for this TriMesh. The
    // first request for a TriMesh's clone copies + remaps; subsequent
    // requests in the same frame reuse the storage (Transform_Objects
    // rewrites the per-pass projection fields in place).
    bool initialized = false;

#if FDS_VIS_CENSUS
    // DIAGNOSTIC ONLY (census build, --xfrm_ablate bits 256/512/1024).
    // A compact 32-byte-per-vertex stand-in for the shadow pass's written
    // block (PX, PY, Flags, TPos.xyz, RZ + 4 B pad) so the per-vertex loop
    // can be re-timed writing DENSE instead of writing into the 140-byte
    // clone Vertex. Nothing reads it — the arms change pixels by
    // construction and exist only to price an out-array split before it is
    // built. Sized lazily on first ablation use, so a normal census run
    // allocates nothing.
    std::vector<float> diagOut;
#endif
};

struct VertexScratch {
    std::unordered_map<TriMesh*, PerTriMeshClone> clones;

    // One-entry cache for the most-recent (T, clone) pair. Transform_
    // Objects iterates TriMeshHead and submits all faces of one mesh
    // before moving on, so consecutive cloneOf() calls from the same
    // pass usually hit the same T → 99%+ cache hit rate. The hit path
    // is one pointer compare; miss falls through to the unordered_map
    // lookup. ~30-50 cycles → ~2 cycles per consecutive same-T call.
    TriMesh         *lastT     = nullptr;
    PerTriMeshClone *lastClone = nullptr;

    // Lazily build (or reuse) the clone for `T`. Returns a reference
    // valid until this VertexScratch is destroyed.
    PerTriMeshClone& cloneOf(TriMesh* T);

    // True when the call that just returned did the FIRST-USE copy for this
    // mesh (so the clone's read-only inputs are trivially in sync). Set on
    // both cloneOf paths; the staleness census reads it to skip fresh clones,
    // which cannot be stale by construction.
    bool lastFresh = false;
};

// ── CLONE INPUT STALENESS (--clone_stale_census / --clone_refresh_inputs) ──
//
// THE INVARIANT THIS MACHINERY GUARDS. `cloneOf` snapshots the WHOLE Vertex
// (and the whole Face) on first use and never refreshes it, but Transform_
// Objects only ever REWRITES the projection outputs (PX, PY, Flags, TPos_AOS,
// RZ, TN, TTangent — Vertex.h offsets [0,52)). Everything from offset 52 on —
// Pos, N, Tangent, BGRA, UZ/VZ, EUZ/EVZ, U/V, EU/EV, i, OrigBary, ShellH — is
// an INPUT the clone carries verbatim from the first bake forever. So:
//
//   any CPU write to TriMesh::Verts[i].<input field> or to TriMesh::Faces[j]
//   AFTER that mesh's first clone is invisible to every clone-backed pass.
//
// That is only safe because no shadow-casting mesh mutates those fields per
// frame today: rigid animation moves IPos / RotMat (which the transform reads
// from the TriMesh, not the clone), and the two greets meshes that DO rewrite
// vertex state every tick — the mirror clones (Pos, GreetsMirror.cpp
// UpdateMirror) and the disco ball (LR/LG/LB, GreetsDisco.cpp UpdateDiscoBall)
// — both carry Tri_NoShadowCast and are skipped by every shadow occluder pass.
// The invariant is UNENFORCED by the type system, so it is enforced by these
// two instruments instead. If you animate vertex data on a caster, or clear
// Tri_NoShadowCast on one of those two, --clone_stale_census is what tells you
// and --clone_refresh_inputs is what makes it correct while you decide.
#if FDS_VIS_CENSUS
// Compare a REUSED clone's input fields against the live mesh and accumulate
// per-mesh counters. Census build only; called from Transform_Objects right
// after cloneOf, where the Object name is in scope.
void CloneStale_Check(TriMesh *T, const char *name, const PerTriMeshClone &c);
// Print the cumulative report (one line per mesh that ever diverged, plus a
// totals line) and reset. Called every N main-view frames from
// Transform_Objects' census epilogue.
void CloneStale_Dump();
#endif

// Re-copy a reused clone's INPUT fields (and its Face array, A/B/C remapped)
// from the live mesh. Available in every build behind --clone_refresh_inputs
// so the "do stale reads reach pixels" question can be answered on a
// SHIPPING-shaped binary rather than only on the census one. Not a per-frame
// cost when the flag is off: the call site is one predicate.
void CloneRefreshInputs(TriMesh *T, PerTriMeshClone &c);
// The Face half of the same refresh (--clone_refresh_inputs=2). Split out
// because re-copying the Face array also resets LastMip, the per-face mip
// HYSTERESIS state the clone legitimately owns per pass — so folding it into
// level 1 would let a mip-flicker change masquerade as a staleness finding.
void CloneRefreshFaces(TriMesh *T, PerTriMeshClone &c);

} // namespace fds

#endif
