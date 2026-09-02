// ═══════════════════════════════════════════════════════════════════════════
// v4 bake — SHARED-BORDER POSITIONS.  One function, one canonical operand
// order, compiled with -ffp-contract=off (see DEMO/CMakeLists.txt).
//
// This TU exists for exactly one reason.  Trap 607c7cd2c810: with
// -ffp-contract=fast the compiler contracted a DUPLICATED expression
// differently at its two call sites and the two sides of a shared border
// stopped agreeing to the bit — two pins moved.  R1 of the survey (§A) says a
// border point must EXIST ONCE and be referenced from both sides; this file is
// the belt to that braces, so that even a future caller which recomputes rather
// than reuses cannot drift.
//
// R2 (DiagSplit): `n` is the border's OWN sample count, derived by the caller
// from the edge alone — never from either chart's interior.
// R3 (D3D11.3 §11.7.13/.14): the parameter is the exact rational i/n formed in
// INTEGER arithmetic and converted once, so `t` and `1-t` are exact
// complements at i and n-i, and A,B are passed in the edge's canonical
// world-position order (the caller's vertex ids ARE that order).
// ═══════════════════════════════════════════════════════════════════════════

#include "V4Bake.h"

namespace fds {
namespace v4 {

void BorderSample(const double A[3], const double B[3], int i, int n, double out[3])
{
	// i/n exactly: both are small integers, so the quotient is the correctly
	// rounded double of the exact rational.  s = 1-t is exact for these values
	// (t in [0,1] with n a small integer), and using it rather than (1-t)
	// recomputed at each use keeps the two endpoints symmetric.
	const double t = double(i) / double(n);
	const double s = 1.0 - t;
	out[0] = A[0] * s + B[0] * t;
	out[1] = A[1] * s + B[1] * t;
	out[2] = A[2] * s + B[2] * t;
}

// P4 (--v4_ring_grooves): the same position, at a parameter that is NOT a
// rational i/n — a mortar run boundary crossing the edge lands wherever the
// height map's grid puts it.  R3's exactness argument (01c55f739bc3: "exact i/n
// in integer arithmetic", max deviation from the authored line 1.42e-14 u) does
// not reach these, so what they get instead is the guarantee that actually
// seals the mesh: the parameter is formed ONCE per edge in the edge's canonical
// a→b direction, this function is the ONE place a position is derived from it,
// in the same operand order and the same -ffp-contract=off translation unit,
// and the caller creates the vertex once and indexes it from BOTH faces — so
// the two sides cannot hold two different points even in principle.  The
// deviation from the exact authored line is re-measured on the P4 census
// (`border_max_dev`) rather than inherited.
void BorderSampleT(const double A[3], const double B[3], double t, double out[3])
{
	const double s = 1.0 - t;
	out[0] = A[0] * s + B[0] * t;
	out[1] = A[1] * s + B[1] * t;
	out[2] = A[2] * s + B[2] * t;
}

}  // namespace v4
}  // namespace fds
