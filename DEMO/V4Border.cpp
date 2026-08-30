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

}  // namespace v4
}  // namespace fds
