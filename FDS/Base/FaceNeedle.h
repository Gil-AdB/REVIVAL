#ifndef FDS_FACE_NEEDLE_H_INCLUDED
#define FDS_FACE_NEEDLE_H_INCLUDED
#include "Compiler.h"   // FDS_NOINLINE

// CENSUS BUILD (compile-time, never defined in a shipping build), same shape
// and the same reason as FDS_REFLTN_CENSUS in Mekalele.h: the counters would
// otherwise be atomics on the FList-build path, and the question is about a
// build, not about a frame.
#ifndef FDS_NEEDLE_CENSUS
#define FDS_NEEDLE_CENSUS 0
#endif

#include "Face.h"

#include <math.h>
#if FDS_NEEDLE_CENSUS
#include <atomic>
#include <cstdio>
#endif

// --needle_cull (FeatureFlags.def, "sort"): the degenerate-face pre-reject at
// FList-build time. The threshold and the test live here so the four builders
// that need them — Transform_Objects (main / shadow / every offscreen pass) and
// the two hand-written mirror transforms in CHASE.CPP / CITY.CPP — cannot drift
// apart, and so the number is stated exactly once next to the rasterizer sites
// it copies.
//
// THE CONTRACT (identical in shape to FaceBBox.h's, re-derived for area):
//  - Every rasterizer rejects a fan triangle whose screen determinant is at or
//    below 0.01 before any setup: Mekalele.h `if (fabs(det) <= 0.01f) continue;`,
//    TheOtherBarry.h the same, ShadowMap.cpp the same. det is twice the signed
//    screen area, so the reject is "under 0.005 px^2".
//  - The clipper only ever SHRINKS the projected triangle (frustum + tile clip),
//    and the mip subdivision only ever PARTITIONS it. Both build every new
//    vertex through FrustumClipper::FInterpolator (FRUSTRUM.CPP), whose FIRST
//    line is `lerp2(&V->PX, &_IA->PX, &_IB->PX, t)` — PX/PY interpolated
//    LINEARLY IN SCREEN SPACE between two vertices of the polygon, i.e. a point
//    ON the projected edge. Every polygon either stage produces therefore lies
//    inside the projected triangle's convex hull, so every fan triangle handed
//    to a rasterizer has |det| no larger than the face's own. A face at or under
//    the threshold paints nothing in ANY tile, in ANY of those passes, and
//    dropping it at the push is exactly equivalent to letting all three rejects
//    fire downstream. (Measured, chase t=100: 1183 faces culled, the main pass's
//    rasterizer degenerate-rejects 1191 -> 8, and its ACCEPTED triangle count
//    19 420 -> 19 420. Not one accepted triangle came from a culled face.)
//  - VALID ONLY when all three verts are in FRONT of the pass's near plane.
//    Behind-near PX/PY are stale (the projection skips the divide there), so the
//    determinant computed from them means nothing; those faces are always kept,
//    the same escape FaceBBox_Stamp takes.
//  - NEVER applied to sprite/flare faces (A == B), where C is a float
//    (Face::FlareSize) and not a vertex pointer.
namespace fds {

// The rasterizers' own reject value, not a tuned one. Keeping it EQUAL rather
// than tighter is what makes the two sets identical instead of merely nested;
// the float slack in the "sub-triangle is no larger" step is ~1e-9 at this
// magnitude, nine orders below the threshold.
inline constexpr float kNeedleDetReject = 0.01f;

// Screen-determinant reject for the two hand-written mirror transforms. The
// main transform keeps a verbatim copy of this body inline for the reason
// FaceBBox.h documents about its own: a CALL inside that per-mesh face loop has
// moved the scene pins through -ffp-contract=fast + LTO. Keep the two in sync
// by hand — a divergence shows up as a pin move.
inline bool FaceNeedle_Reject(const Face* F, float nearZ) {
	if (F->A == F->B) return false;                   // sprite / flare
	const Vertex* va = F->A; const Vertex* vb = F->B; const Vertex* vc = F->C;
	if (!(va->TPos_AOS.z > nearZ && vb->TPos_AOS.z > nearZ && vc->TPos_AOS.z > nearZ))
		return false;                                 // behind-near: PX/PY stale
	const float e1x = vb->PX - va->PX, e1y = vb->PY - va->PY;
	const float e2x = vc->PX - va->PX, e2y = vc->PY - va->PY;
	const float det = e1x * e2y - e1y * e2x;
	return fabsf(det) <= kNeedleDetReject;
}

#if FDS_NEEDLE_CENSUS
// Which FList builder a count came from. The whole point of splitting them is
// that chase's reflected pass is a SECOND, demo-side transform (2026-08-16w) and
// carried half of that scene's rasterizer rejects.
enum { kNeedleMain = 0, kNeedleShadow = 1, kNeedleOffscreen = 2, kNeedleMirror = 3,
       kNeedlePassKinds = 4 };

struct NeedleCensusState {
	// Every face the pass WALKS, and how many of those are degenerate in OBJECT
	// space — the population a load-time collinearity scan would mark. Compare
	// with `accepted`: the gap is what the visibility + backface tests already
	// remove for free, and a zero-area face has a ZERO normal, so its backface
	// test is `0 < 0` = false and it is never pushed unless its material is
	// two-sided.
	std::atomic<long long> seen[kNeedlePassKinds]{};
	std::atomic<long long> seenZeroN[kNeedlePassKinds]{};
	std::atomic<long long> seenThin[kNeedlePassKinds]{};
	std::atomic<long long> tested[kNeedlePassKinds]{};
	std::atomic<long long> culled[kNeedlePassKinds]{};
	// Of the culled: how many are degenerate in OBJECT space, i.e. would also
	// be found by a load-time collinearity scan. `zeroN` is Compute_Face_Normals'
	// own test verbatim (|B-A x C-A| < 1e-6 — the faces whose N it deliberately
	// leaves un-normalized); `thin` is 2026-08-16v's needle test (longest edge ==
	// the sum of the other two, relatively). The remainder is the pose-dependent
	// part — edge-on quads and sub-pixel slivers, which no load-time scan sees.
	std::atomic<long long> culledZeroN[kNeedlePassKinds]{};
	std::atomic<long long> culledThin[kNeedlePassKinds]{};
	FDS_NOINLINE void report() const {
		static const char *nm[kNeedlePassKinds] = { "main", "shadow", "offscreen", "mirror" };
		for (int k = 0; k < kNeedlePassKinds; ++k) {
			const long long t = tested[k].load(), c = culled[k].load();
			if (!t && !c && !seen[k].load()) continue;
			std::fprintf(stderr, "[NEEDLE] %-9s seen=%lld (objZeroN=%lld objThin=%lld)"
			             " accepted=%lld culled=%lld (%.3f%%)"
			             " of which objZeroN=%lld objThin=%lld\n",
			             nm[k], seen[k].load(), seenZeroN[k].load(), seenThin[k].load(),
			             t, c, t ? 100.0 * double(c) / double(t) : 0.0,
			             culledZeroN[k].load(), culledThin[k].load());
		}
	}
	~NeedleCensusState() { report(); }
};
inline NeedleCensusState g_needleCensus;

// `zeroN` is Compute_Face_Normals' own test verbatim (|(B-A) x (C-A)| < 1e-6 —
// the faces whose N it deliberately leaves un-normalized); `thin` is
// 2026-08-16v's needle test (longest edge == the sum of the other two,
// relatively). Both read Pos, i.e. OBJECT space, i.e. what a load-time scan sees.
inline void NeedleCensus_Classify(const Face *F, bool &zeroN, bool &thin) {
	const float ux = F->B->Pos.x - F->A->Pos.x, uy = F->B->Pos.y - F->A->Pos.y,
	            uz = F->B->Pos.z - F->A->Pos.z;
	const float vx = F->C->Pos.x - F->A->Pos.x, vy = F->C->Pos.y - F->A->Pos.y,
	            vz = F->C->Pos.z - F->A->Pos.z;
	const float cx = vy * uz - vz * uy, cy = vz * ux - vx * uz, cz = vx * uy - vy * ux;
	zeroN = sqrtf(cx * cx + cy * cy + cz * cz) < 0.000001f;
	const float wx = F->C->Pos.x - F->B->Pos.x, wy = F->C->Pos.y - F->B->Pos.y,
	            wz = F->C->Pos.z - F->B->Pos.z;
	const float la = sqrtf(ux * ux + uy * uy + uz * uz);
	const float lb = sqrtf(vx * vx + vy * vy + vz * vz);
	const float lc = sqrtf(wx * wx + wy * wy + wz * wz);
	const float lmax = (la > lb) ? ((la > lc) ? la : lc) : ((lb > lc) ? lb : lc);
	const float rest = la + lb + lc - lmax;
	thin = lmax > 0.0f && (rest - lmax) <= 1e-6f * lmax;
}
inline void NeedleCensus_Seen(int k, const Face *F) {
	g_needleCensus.seen[k].fetch_add(1, std::memory_order_relaxed);
	if (F->A == F->B) return;   // sprite / flare: C is a float
	bool zeroN = false, thin = false;
	NeedleCensus_Classify(F, zeroN, thin);
	if (zeroN) g_needleCensus.seenZeroN[k].fetch_add(1, std::memory_order_relaxed);
	if (thin)  g_needleCensus.seenThin[k].fetch_add(1, std::memory_order_relaxed);
}
inline void NeedleCensus_Tested(int k) {
	g_needleCensus.tested[k].fetch_add(1, std::memory_order_relaxed);
}
inline void NeedleCensus_Cull(int k, const Face *F) {
	g_needleCensus.culled[k].fetch_add(1, std::memory_order_relaxed);
	bool zeroN = false, thin = false;
	NeedleCensus_Classify(F, zeroN, thin);
	if (zeroN) g_needleCensus.culledZeroN[k].fetch_add(1, std::memory_order_relaxed);
	if (thin)  g_needleCensus.culledThin[k].fetch_add(1, std::memory_order_relaxed);
}
#endif  // FDS_NEEDLE_CENSUS

} // namespace fds

#if FDS_NEEDLE_CENSUS
#define FDS_NEEDLE_COUNT_CULL()  fds::NeedleCensus_Cull(needleCensusKind, F)
#define FDS_NEEDLE_COUNT_TEST()  fds::NeedleCensus_Tested(needleCensusKind)
#define FDS_NEEDLE_COUNT_SEEN()  fds::NeedleCensus_Seen(needleCensusKind, F)
#else
#define FDS_NEEDLE_COUNT_CULL()  ((void)0)
#define FDS_NEEDLE_COUNT_TEST()  ((void)0)
#define FDS_NEEDLE_COUNT_SEEN()  ((void)0)
#endif

#endif
