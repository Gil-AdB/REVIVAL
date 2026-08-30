#pragma once

// ═══════════════════════════════════════════════════════════════════════════
// v4 stone-displacement bake — PHASE 1 (docs/DISPLACEMENT_V4_DESIGN.md §2a,
// §2b, §6 P1).  CENSUS ONLY.
//
// This translation unit builds, over the SAME authored stone face set the old
// bake targets and at the SAME point in greets init (DEMO/GREETS.CPP ~2019,
// i.e. BEFORE the ::mirUV clone split at ~2868):
//
//   (a) an exact-equality vertex stitch + half-edge, boundaries carried as a
//       null FACE and never a null twin, material and coplanarity as EDGE
//       ATTRIBUTES and never as topology, edge endpoints canonically ordered
//       by world position before anything is derived from an edge;
//   (b) a material-blind chart registry (region growing under a normal budget)
//       across ALL meshes, plus the chart-pair junction table with each
//       junction's dihedral φ, convex/concave/smooth class and length.
//
// It prints two stderr census blocks, `[V4-STITCH]` and `[V4-CHARTS]`, which
// `tools/v4_census.py` turns into numbers.  It allocates nothing in the scene,
// mutates NOTHING (every Scene/TriMesh/Face/Vertex pointer it holds is const),
// and therefore cannot move a pixel in any arm.  The old bake runs after it,
// unchanged.
//
// Entry point is gated by the CALLER on
//     --greets_displace_v4 && --v4_census
// (both BOOL, both default OFF).
// ═══════════════════════════════════════════════════════════════════════════

struct Scene;

namespace fds {
namespace v4 {

// Runs stage (a) + stage (b) over the faces of `Sc` whose material name is one
// of `mats[0..nMats)` and prints the two census blocks.  Read-only on `Sc`.
void RunP1Census(const Scene *Sc, const char *const *mats, int nMats);

}  // namespace v4
}  // namespace fds
