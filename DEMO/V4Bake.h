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

// ═══════════════════════════════════════════════════════════════════════════
// PHASE 2 (docs/DISPLACEMENT_V4_DESIGN.md §2c, §6 P2) — the UNDISPLACED
// lattice.  Builds the per-chart UV-aligned lattice with the amplitude forced
// to 0 and SWAPS it in for the authored stone faces; the old bake does not run
// on those faces in this arm (GREETS.CPP chooses one or the other).
//
//   * breaklines on the mortar centrelines and block-edge pairs, from the
//     height map's row/column profiles — MeshOps_FindStoneGrooveGrid, the
//     finding extracted out of DisplaceStoneSubdiv so both bakes run the same
//     code (the per-line REP heights are the half v4 drops);
//   * plateau nodes at least 4 level-0 texels inside the block (the shoulder
//     pad is 1.25 mip-2 texels = 5 level-0, so a node on or past a pad line
//     already clears it);
//   * interior density from --v4_cpb, groove bands --v4_groove_refine levels
//     finer, adjacent cells never differing by more than one level;
//   * R1/R2/R3 on every shared border: the border owns its sample count
//     (derived from its two endpoints alone), the parameters are exact i/n in
//     integer arithmetic (BorderSample below, -ffp-contract=off), each border
//     vertex is created ONCE and indexed from both sides, and an edge's
//     endpoints are ordered by world position before anything is derived.
//
// Prints [V4-LATTICE] and [V4-OUT] under --v4_census; tools/v4_census.py reads
// them.  --v4_flat emits the authored triangles through the identical path
// with no lattice at all: that is the CONTROL arm the phase's byte-identity
// gate compares against (same downstream pipeline, only the tessellation
// differs).
// PHASE 3 (§2d, §2e, §6 P3) rides in the same entry point: with `amp` non-zero
// every INTERIOR lattice node is displaced along its CHART PLANE NORMAL by
// d(u,v) = amp*(h(u,v) − mipMean), the plateau nodes reading the max-pyramid,
// the groove nodes the min-pyramid and the bevel nodes the bilinear field
// (§2d).  Every vertex that lies on an AUTHORED EDGE — corner, shared-border
// sample, abutment sample — stays PINNED at 0: the junction rings are §2e's
// offset-plane solve and that is phase P4, so P3 must not move a vertex two
// faces share.  `amp` comes from the caller (--greets_displace_amp) unless
// --v4_amp overrides it; --v4_amp=0 reproduces the P2 lattice exactly and is
// the control arm of every P3 measurement.
void RunP2Bake(Scene *Sc, const char *const *mats, int nMats, int mip, float amp);

// §WorldUV (--v4_world_uv, default OFF, byte-null off) — his ruling
// e54d2bd9b654.  Replaces the authored UV of every face whose material is one
// of `mats[0..nMats)` (and which the old bake would bake) with the WORLD-SPACE
// PLANAR PROJECTION of its plane's dominant axis, so albedo, height and the
// v4 lattice's breaklines all read ONE coordinate system that is continuous
// across every same-plane face boundary.  Scale, sign and phase are fitted
// per material from the authored UVs; the full rule and the reason it runs
// where it runs are in the banner over the definition in V4Bake.cpp.
//
// CALL IT BEFORE CaptureStoneProxySnapshot in greets init: that snapshot
// copies whole Face/Vertex structs and is the one consumer that caches UVs
// across the bake (the offscreen mirror-RTT / env-probe proxy).  Returns the
// number of faces rewritten; prints [V4-WORLDUV] (the per-plane fit and the
// cost the fit could not absorb) under --v4_census.
long long WorldUV_Apply(Scene *Sc, const char *const *mats, int nMats);

// One shared-border sample position.  Defined in DEMO/V4Border.cpp, which is
// compiled with -ffp-contract=off; see the banner there.  `A`/`B` are the
// edge's endpoints in canonical world-position order, `i` in [0,n].
void BorderSample(const double A[3], const double B[3], int i, int n, double out[3]);

}  // namespace v4
}  // namespace fds
