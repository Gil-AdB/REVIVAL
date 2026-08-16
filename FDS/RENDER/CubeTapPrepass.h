#pragma once

// ─── The cube tap's PROLOGUE, 8-wide over PIXELS for a fixed light ──────────
//
// WHY THIS EXISTS. `CubeShadow_Sample` is 204 retired instructions per tap and
// 2.943 M taps a frame at greets t=5743 (his acceptance arm, 1512x848) — 0.599
// Gi/f, 33 % of `DeferredLighting-call`, the largest single row left in the
// deferred lighting kernel. The FDS_CUBE_ABLATE ladder (FILLERS/ShadowMap.h)
// splits that 204 as:
//
//     call frame + cubeIdx guard        23.4      the wrapper is gone (16g);
//     lightISource + 3 world subs       13.6      this is the frame that is left
//     CubeShadow_SelectFace             20.7   ┐
//     face map resolve                   8.8   │
//     viewToLight 3x3 matmul            30.9   │  125 instr = 61 % of the tap,
//     lz near reject                     5.4   │  and every one of them is pure
//     2 face-frustum rejects            22.4   │  SIMD: no branch a lane cannot
//     1/lz + smX/smY projection          5.1   │  mask, no data-dependent load
//     int trunc + iX/iY bounds reject   18.0   ┘
//     uniformity pyramid                19.0      dependent load, stays scalar
//     bilinear weights                   4.1
//     2x2 tap addressing                 6.8      only 18.9 % of taps get here
//     4 packed taps + accumulate        25.1      (--shadow_tap_census)
//
// The loop is PIXEL-major (per pixel, loop lights), so nothing in that 125 can
// go wide as written: for a fixed pixel the eight lights carry eight different
// cubes, eight different faces and therefore eight different 3x3 matrices, and
// gathering twelve floats per lane costs exactly what the scalar loads cost.
// Flip the axis — fixed LIGHT, eight PIXELS — and the matrix becomes a
// BROADCAST: twelve loads for eight lanes instead of twelve per lane, and the
// 18 flops of the matmul become 18 vector ops for eight lanes.
//
// WHAT IS AND IS NOT WIDE HERE. The face still varies per lane, so the
// projection runs once per DISTINCT face present in the eight (normally one —
// eight neighbouring pixels see a light from the same side). The tail — the
// uniformity pyramid and the 2x2 tap — is a dependent load and stays scalar
// and LAZY: it runs only for the pairs the omni loop's own early-outs let
// through, which is why the prologue can afford to be eager.
//
// BIT-EXACTNESS IS THE WHOLE POINT, and it is not free. Everything below is
// written as clang vector operators in THIS file, under FP_CONTRACT_OFF, with
// __builtin_elementwise_fma at exactly the sites the scalar's own codegen
// fuses — read from `otool -tvV` on the shipping binary, not guessed:
//
//     ldr s1,[m20]  ldr s2,[m21]  fmul s2, s4, s2       <- MIDDLE product plain
//     fmadd s1, s3, s1, s2                              <- first FUSED into it
//     ldr s2,[m22]  fmadd s1, s5, s2, s1                <- third FUSED
//     ldr s2,[off]  fadd s11, s2, s1                    <- offset NOT fused
//
//     fdiv s3, s0, s11                                  <- 1/lz, a real divide
//     fmul s1, s1, s5   fmadd s1, s3, s1, s4            <- smX = fma(inv, lx*px, cx)
//     fmul s2, s2, s5   fmsub s2, s3, s2, s4            <- smY = fma(-inv, ly*py, cy)
//
// simde's `_mm256_fnmadd_ps` is NOT an fma on arm64 (it is `-(a*b)+c` in a
// header, fused only by the build's -ffp-contract=fast, and a call-site pragma
// is lexically out of reach of it) — 2026-08-16e paid for that lesson with 84 %
// of 2.6 M taps disagreeing. Hence the native spelling.
//
// --deferred_cube_prepass_verify re-runs the WHOLE SCALAR TAP behind every
// cached one — with the pixel body's own view and world positions, not the row
// pass's — and compares the two answers as BIT PATTERNS. That is deliberately
// wider than "is the vector maths right": it also catches the row pass's
// duplicate reconstruction of those six floats drifting a ULP from the body's,
// which is the other way this design could move a pixel. Measured 0 mismatches
// in 47 M taps across the five acceptance poses, the greets t=1588 pin and the
// bench loop.
//
// WHAT IT DOES NOT BUY. This is a −4 % row, not a −20 % one: the ladder prices
// the prologue at 0.368 Gi/f and the mechanism nets 0.068, so the row pass
// costs ~0.30 Gi/f to fill ~5.4 M lane-slots. The eager/lazy ratio is the
// visible half of that (5.4 M slots filled for 2.943 M taps taken, because the
// prepass cannot afford the N·L reject) and per-slot overhead is the rest.

#include "Base/FPContract.h"
#include "FILLERS/ShadowMap.h"

#include <cstdint>
#include <cstring>

namespace fds {

// One pixel's resolved tap prologue for one light. 16 bytes, so a group of
// eight is two cache lines and a row's worth for one light stays in L1.
//   mapIdx >= 0 : READY — index into g_shadowMaps, smX/smY/iX/iY are live
//   mapIdx <  0 : the prologue's own early-out fired (near plane, face
//                 frustum, or off-map) and the tap's answer is 1.0f
struct CubeProSlot {
	float   smX, smY;
	int32_t mapIdx;
	int16_t iX, iY;
};
inline constexpr int32_t kCubeProLit = -1;

#if defined(__clang__)
#define FDS_CUBEPRO_VEC8 1

typedef float   ctf8 __attribute__((ext_vector_type(8)));
typedef int32_t cti8 __attribute__((ext_vector_type(8)));

static inline ctf8 ctLd8(const float *p) { ctf8 v; __builtin_memcpy(&v, p, sizeof v); return v; }
static inline ctf8 ctSpl(float x) { return ctf8(x); }
// a*b + c, GUARANTEED fused — the sites the scalar emits as fmadd/fmsub.
static inline ctf8 ctFma(ctf8 a, ctf8 b, ctf8 c) {
#if __has_builtin(__builtin_elementwise_fma)
	return __builtin_elementwise_fma(a, b, c);
#else
	FP_CONTRACT_FAST
	return a * b + c;
#endif
}

// 8-wide CubeShadow_SelectFace. Pure compare/select on the same three
// magnitudes and the same three sign tests, in the same nesting order, so a
// lane's answer is the scalar's answer for every input including -0.0 (which
// compares >= 0, as it does scalar) and NaN (every compare false, so the
// nesting falls through to the dz arm exactly as the scalar's `if` chain does).
static inline cti8 CubeFace8(ctf8 dx, ctf8 dy, ctf8 dz)
{
	const ctf8 ax = __builtin_elementwise_abs(dx);
	const ctf8 ay = __builtin_elementwise_abs(dy);
	const ctf8 az = __builtin_elementwise_abs(dz);
	const cti8 f45 = (dz >= 0.0f) ? cti8(4) : cti8(5);
	const cti8 f23 = (dy >= 0.0f) ? cti8(2) : cti8(3);
	const cti8 f01 = (dx >= 0.0f) ? cti8(0) : cti8(1);
	const cti8 fyz = (ay >= az)   ? f23 : f45;
	return ((ax >= ay) & (ax >= az)) ? f01 : fyz;
}

// The tap prologue for ONE light over EIGHT pixels. `wxs/wys/wzs` are the
// pixels' world positions and `vxs/vys/vzs` their view positions, both
// lane-contiguous; lane k's slot is written to `out[k * outStride]`.
//
// The stride is there because the slot plane is laid out LANE-major
// (`slots[lane * lightStride + n]`), not light-major. The producer scatters
// either way — it is a scalar store loop over eight lanes — but the CONSUMER
// is the pixel-major omni loop, which wants one pixel's whole light row
// contiguous: that makes the tap's cache read a linear walk and, more to the
// point, lets the pixel body hold ONE pointer instead of a base, a stride and
// a lane index. Live-set in that loop is not a detail — measured, the
// light-major layout cost +7.4 % of `lighting-w1` on the OFF arm alone.
//
// `cubeIdx` is assumed in range — the caller hoists that guard out of the
// group loop, which is the one thing the scalar tap does per (pixel x light)
// that has no per-pixel content at all.
static inline void CubeTap_Prologue8(int cubeIdx,
                                     const float *wxs, const float *wys, const float *wzs,
                                     const float *vxs, const float *vys, const float *vzs,
                                     CubeProSlot *out, int outStride)
{
	const CubeShadowRef &cr = g_cubeShadowRefs[cubeIdx];
	ctf8 dwx, dwy, dwz;
	{
		FP_CONTRACT_OFF
		dwx = ctLd8(wxs) - ctSpl(cr.lightISource.x);
		dwy = ctLd8(wys) - ctSpl(cr.lightISource.y);
		dwz = ctLd8(wzs) - ctSpl(cr.lightISource.z);
	}
	const cti8 faceV = CubeFace8(dwx, dwy, dwz);
	int32_t faces[8];
	__builtin_memcpy(faces, &faceV, sizeof faces);

	const ctf8 vx = ctLd8(vxs), vy = ctLd8(vys), vz = ctLd8(vzs);

	// One projection per DISTINCT face among the eight. Normally one pass —
	// eight neighbouring pixels see a given light from the same side unless
	// the group straddles the plane where the dominant axis of (P - light)
	// changes, which is a screen-space curve, not an area. FOUR shapes of this
	// loop and its store were measured at greets t=5743, all bit-exact:
	//   scan + per-lane predicate store          1.480 Gi/f   <- this one
	//   scan + verdict folded into a packed slot 1.508
	//   all-same fast path + packed slot         1.484
	//   all-same fast path + per-lane store      1.490
	// The reduce that tests "all eight on one face" costs more than the eight
	// -iteration scan it skips, and folding the reject verdict into `mapIdx`
	// with a vector select costs more than the per-lane branch it removes —
	// that branch is perfectly predicted, because a rejected lane is rare and
	// clustered. Neither was obvious; both were tried.
	unsigned todo = 0xFFu;
	while (todo) {
		const int lane0 = __builtin_ctz(todo);
		const int f = faces[lane0];
		unsigned sub = 0;
		for (int k = 0; k < 8; ++k)
			if (((todo >> k) & 1u) && faces[k] == f) sub |= 1u << k;
		todo &= ~sub;

		const int32_t mapIdx = cr.faceIdx[f];
		const ShadowMap &sm = g_shadowMaps[mapIdx];
		ctf8 lx, ly, lz, smX, smY;
		{
			FP_CONTRACT_OFF
			// Row order, product order and fusion sites copied from the
			// scalar's codegen (see the block comment at the top).
			lz = ctSpl(sm.viewToLightOffset.z)
			   + ctFma(vz, ctSpl(sm.viewToLight[2][2]),
			     ctFma(vx, ctSpl(sm.viewToLight[2][0]),
			           vy * ctSpl(sm.viewToLight[2][1])));
			lx = ctSpl(sm.viewToLightOffset.x)
			   + ctFma(vz, ctSpl(sm.viewToLight[0][2]),
			     ctFma(vx, ctSpl(sm.viewToLight[0][0]),
			           vy * ctSpl(sm.viewToLight[0][1])));
			ly = ctSpl(sm.viewToLightOffset.y)
			   + ctFma(vz, ctSpl(sm.viewToLight[1][2]),
			     ctFma(vx, ctSpl(sm.viewToLight[1][0]),
			           vy * ctSpl(sm.viewToLight[1][1])));
			const ctf8 inv = ctSpl(1.0f) / lz;
			smX = ctFma(inv, lx * ctSpl(sm.perspX), ctSpl(sm.cntrX));
			smY = ctFma(-inv, ly * ctSpl(sm.perspY), ctSpl(sm.cntrY));
		}
		// The three rejects, as masks. The scalar returns 1.0f at each; a lane
		// that trips any of them gets kCubeProLit and never reaches the tail.
		// `lz * 1.5f` / `lz * -1.5f` are the same two products the scalar forms
		// (fmov #1.5 / #-1.5 then fmul).
		const ctf8 lim  = lz * ctSpl(1.5f);
		const ctf8 nlim = lz * ctSpl(-1.5f);
		cti8 ok = (lz > ctSpl(0.05f))
		        & ~((lx > lim) | (lx < nlim) | (ly > lim) | (ly < nlim));
		// int(smX) is a truncation toward zero — fcvtzs, which also maps NaN
		// to 0 exactly as the scalar's cast does.
		const cti8 iX = __builtin_convertvector(smX, cti8);
		const cti8 iY = __builtin_convertvector(smY, cti8);
		ok &= (iX >= cti8(0)) & ((iX + cti8(1)) < cti8(sm.xres))
		    & (iY >= cti8(0)) & ((iY + cti8(1)) < cti8(sm.yres));
		float sxA[8], syA[8]; int32_t ixA[8], iyA[8], okA[8];
		__builtin_memcpy(sxA, &smX, sizeof sxA);
		__builtin_memcpy(syA, &smY, sizeof syA);
		__builtin_memcpy(ixA, &iX,  sizeof ixA);
		__builtin_memcpy(iyA, &iY,  sizeof iyA);
		__builtin_memcpy(okA, &ok,  sizeof okA);
		// Five stack arrays and a scalar store loop is the CHEAPEST of three
		// shapes measured here, which is not what it looks like. Folding the
		// verdict into `mapIdx` as a vector select and packing iX|iY<<16 so
		// the slot is four aligned words — the version that lets the loop
		// store unconditionally — reads 1.508 Gi/f against this one's 1.480:
		// the two extra vector ops per GROUP cost more than the per-lane
		// predicate they remove, because the predicate is perfectly predicted
		// (a rejected lane is rare and clustered).
		for (int k = 0; k < 8; ++k) {
			if (!((sub >> k) & 1u)) continue;
			CubeProSlot &s = out[size_t(k) * size_t(outStride)];
			if (!okA[k]) { s.mapIdx = kCubeProLit; continue; }
			s.smX = sxA[k]; s.smY = syA[k];
			s.iX = int16_t(ixA[k]); s.iY = int16_t(iyA[k]);
			s.mapIdx = mapIdx;
		}
	}
}
#endif  // __clang__

// The cached tap: the prologue's answer plus the SAME tail body the scalar
// `CubeShadow_Sample` runs. `noinline` is LOAD-BEARING, not hygiene:
// CubeShadow_Tail is always_inline (so that splitting it out of
// CubeShadow_Sample left that function's codegen byte-identical), and without
// the attribute here clang inlines ~100 instructions of pyramid + PCF into the
// omni loop at the tap site — where the register allocator then pays for them
// on EVERY pixel, flag on or off. Measured: it put +4 % of `lighting-w1` on
// the OFF arm. Keeping the call out of line is also exactly the shape 16g's
// win came from — the point of --deferred_cube_direct was a SMALLER live set
// at this call, not a bigger one.
//
// The three trailing arguments of CubeShadow_Tail are the Depth-mode ones.
// The prepass is gated on ShadowMode::PolyId with `surfaceShadowId >= 0`
// (see `cubeProTile` in DeferredSurfaceKernel.cpp), so that arm is
// unreachable here and the constants below are never read.
inline float CubeShadow_SampleCached(const CubeProSlot &s, int surfaceMatId)
{
	if (s.mapIdx < 0) return 1.0f;
	return CubeShadow_Tail(g_shadowMaps[s.mapIdx], s.smX, s.smY, s.iX, s.iY,
	                       surfaceMatId, /*dynamicOnly=*/false,
	                       /*lz=*/0.0f, /*constBias=*/0, /*slopeBiasInt=*/0);
}

}  // namespace fds
