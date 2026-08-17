#include "ProceduralWater.h"
#include <Base/Compiler.h>   // FDS_FMAF

#include "Rev.h"
#include <Threads.h>
#include <Base/FeatureFlags.h>
#include <Base/FPContract.h>   // waterWaveSlope8 pins its own contraction — see
                               // the block comment on the 8-wide form.
#include <RENDER/TailProf.h>   // --deferred_prof: the water passes run OUTSIDE
                               // renderFrame, so they had no phase row at all.


#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

namespace pwater {

// ───────── Procedural water surface field (precomputed tiling normal map) ─────────
// THE shared water field: returns the wave SLOPE (nx,nz) at world XZ. Both the
// reflection displacement and the specular glints derive from this, so they
// ripple to the same waves (coherent water). The field is a small SEAMLESS
// tiling slope map (built once from integer-wavevector sines), sampled as TWO
// scrolling layers at different scale/rotation — the layers break each other's
// tiling into something organic + non-repetitive, and the whole thing is a
// couple of texture taps per pixel instead of ~9 trig calls (the optimization).
// WNRM_S = the STORAGE stride, one texel wider/taller than the tile. The map
// is periodic, so column WNRM is a verbatim copy of column 0 and row WNRM a
// verbatim copy of row 0 — a HALO. The point is the sampler: with the halo,
// the "+1 neighbour" of the last texel is the texel next to it in memory, so
// sampleWaterNrm's two `(x + 1) % WNRM` wraps disappear. clang cannot fold
// those into an AND (it must handle a negative left operand), so each cost
// add/and/negs/and/csneg = 5 instructions, ×4 per waterWaveSlope call (two
// axes × two layers) = 20 of the function's 161 — in the 4th-hottest symbol
// of the city frame. The halo also makes the (i0,i0+1) pair CONTIGUOUS, which
// lets the corner loads pair.
// BIT-EXACT by construction: every fetched value is the same float the modulo
// addressed, and the halo is copied AFTER the normalize so it is bit-identical
// to its source texel.
static std::vector<float> g_waterNrm;       // WNRM_S*WNRM_S*2 floats: slope (nx,nz), tiling
static constexpr int      WNRM   = 128;     // tile period
static constexpr int      WNRM_S = WNRM + 1;// storage stride (halo)

static void buildWaterNormalMap() {
	g_waterNrm.assign(size_t(WNRM_S) * size_t(WNRM_S) * 2, 0.0f);
	struct W { int kx, kz; float a, ph; };
	const W ws[8] = {
		{1, 0, 1.00f, 0.0f}, {0, 1, 0.85f, 1.3f}, {1, 1, 0.70f, 2.1f}, {2,-1, 0.55f, 0.7f},
		{-1,2, 0.45f, 3.4f}, {2, 2, 0.35f, 1.8f}, {3, 1, 0.28f, 0.4f}, {1,-3, 0.22f, 2.7f},
	};
	const float twoPi = 6.2831853f;
	float maxAbs = 1e-6f;
	for (int j = 0; j < WNRM; ++j) for (int i = 0; i < WNRM; ++i) {
		float nx = 0.0f, nz = 0.0f;
		for (const W& w : ws) {
			const float ph = twoPi * (w.kx * float(i) + w.kz * float(j)) / float(WNRM) + w.ph;
			const float c = std::cos(ph);                       // slope ∝ k·cos(phase)
			nx += w.a * (twoPi * float(w.kx) / float(WNRM)) * c;
			nz += w.a * (twoPi * float(w.kz) / float(WNRM)) * c;
		}
		float* p = &g_waterNrm[(size_t(j) * WNRM_S + i) * 2];
		p[0] = nx; p[1] = nz;
		if (std::fabs(nx) > maxAbs) maxAbs = std::fabs(nx);
		if (std::fabs(nz) > maxAbs) maxAbs = std::fabs(nz);
	}
	const float inv = 1.0f / maxAbs;                            // normalize slope to ~[-1,1]
	for (int j = 0; j < WNRM; ++j) for (int i = 0; i < WNRM; ++i) {
		float* p = &g_waterNrm[(size_t(j) * WNRM_S + i) * 2];
		p[0] *= inv; p[1] *= inv;
	}
	// Wrap halo: column WNRM == column 0, row WNRM == row 0 (the map is
	// periodic), copied AFTER the scale so each halo texel is bit-identical to
	// the texel the old `% WNRM` would have addressed.
	for (int j = 0; j < WNRM; ++j) {
		const float* s = &g_waterNrm[(size_t(j) * WNRM_S + 0) * 2];
		float*       d = &g_waterNrm[(size_t(j) * WNRM_S + WNRM) * 2];
		d[0] = s[0]; d[1] = s[1];
	}
	for (int i = 0; i <= WNRM; ++i) {
		const float* s = &g_waterNrm[(size_t(0)    * WNRM_S + i) * 2];
		float*       d = &g_waterNrm[(size_t(WNRM) * WNRM_S + i) * 2];
		d[0] = s[0]; d[1] = s[1];
	}
}

static inline void sampleWaterNrm(float u, float v, float& nx, float& nz) {
	const int N = WNRM;
	float fu = u - std::floor(u / N) * N;                       // wrap to [0,N)
	float fv = v - std::floor(v / N) * N;
	int i0 = int(fu), j0 = int(fv); if (i0 >= N) i0 = 0; if (j0 >= N) j0 = 0;
	const float du = fu - i0, dv = fv - j0;
	// i0,j0 are in [0, N-1] (fu,fv in [0,N) by the wrap, clamped above), so
	// the +1 neighbour is in [1, N] — inside the HALO, no modulo. See the
	// comment on WNRM_S.
	const float* a = &g_waterNrm[(size_t(j0)*WNRM_S + i0)*2];
	const float* b = a + 2;                                     // (i0+1, j0)
	const float* c = a + size_t(WNRM_S)*2;                      // (i0,   j0+1)
	const float* d = c + 2;                                     // (i0+1, j0+1)
	nx = (a[0]*(1-du)+b[0]*du)*(1-dv) + (c[0]*(1-du)+d[0]*du)*dv;
	nz = (a[1]*(1-du)+b[1]*du)*(1-dv) + (c[1]*(1-du)+d[1]*du)*dv;
}

// nOct kept for call-site compatibility; the texture bakes all detail, so both
// the reflection and the glints get the same two-layer sample (coherent).
static inline void waterWaveSlope(float wx, float wz, float t, float scale, int /*nOct*/, float& bnx, float& bnz) {
	if (g_waterNrm.empty()) { bnx = bnz = 0.0f; return; }
	const float base = 0.02f * scale;                          // world→texel frequency
	// Scroll the two layers across the 128-texel map. The terms are in TEXEL
	// units, so they must be big (~tens) to move a meaningful fraction of the
	// map per second — small values (the first cut's ~0.7) crawl ≈1 cycle/90s,
	// i.e. frozen ("no movement"). ~16 → the dominant wave drifts ~0.3 Hz.
	float n1x, n1z; sampleWaterNrm(wx*base + t*16.0f, wz*base - t*12.0f, n1x, n1z);
	const float ca = 0.87f, sa = 0.5f;                         // ~30° rotated 2nd layer
	const float rx = wx*ca - wz*sa, rz = wx*sa + wz*ca;
	float n2x, n2z; sampleWaterNrm(rx*base*1.7f - t*10.0f, rz*base*1.7f + t*14.0f, n2x, n2z);
	bnx = (n1x + n2x*0.6f) * 1.6f;                             // ~match the old trig slope range
	bnz = (n1z + n2z*0.6f) * 1.6f;
}

// ───────── 8-wide waterWaveSlope (--water_slope_vec8) ─────────
// The scalar above is the 4th-hottest symbol of the city acceptance frame and
// it is NOT inlined into either hot consumer — both `updateRippleDispMap`'s
// band and `RenderGlints`' band `bl` the one out-of-line copy at
// `pwater::waterWaveSlope`. That is what makes an 8-wide form safe to write
// BIT-EXACTLY: there is exactly ONE scalar codegen to match, not one per
// inline context.
//
// WHAT THE SCALAR ACTUALLY COMPILES TO (arm64, -O3 -flto -ffp-contract=fast,
// read off the disassembly — this is the contract this function reproduces):
//   * clang has ALREADY SLP-vectorized it 2-wide: the two scroll layers live in
//     the two lanes of `.2s` vectors. So "8-wide" buys 4x here, not 8x.
//   * rx/rz are FUSED:  rx = fmsub(wx, .87, wz*.5)   (`fnmsub`)
//                       rz = fmadd(wx, .5,  wz*.87)  (`fmadd`)
//   * the four scroll coords are NOT fused — each is (world product) ± (t*k)
//     with both products separately rounded, then a plain fadd/fsub:
//       u1 = wx*base + 16t     v1 = wz*base - 12t
//       u2 = 1.7*(rx*base) - 10t   v2 = 1.7*(rz*base) + 14t
//   * the wrap IS fused: fu = u - floor(u*(1/128))*128 via `fmls`.
//   * `if (i0 >= N) i0 = 0` is branchless `cmge #127` + `and`.
//   * THE BILINEAR USES NO FMA AT ALL — plain `fmul`/`fadd`:
//       p = (1-du)*a + du*b ;  q = (1-du)*c + du*d ;  n = (1-dv)*p + dv*q
//     This is the single most important line here. The 2026-08-16b modulo
//     round moved 4 px purely because an edit let clang RE-contract this
//     chain; writing the vector form with plain mul/add under an explicit
//     FP_CONTRACT_OFF is what makes this change byte-null instead.
//   * the layer mix IS fused: bn = fmadd(0.6, n2, n1) * 1.6.
// FP_CONTRACT_OFF below pins all of that: nothing fuses unless it is written
// as an explicit fma intrinsic, and every explicit fma corresponds to a fused
// instruction in the scalar disassembly.
//
// The gather is the only structural difference, and the 129-stride halo
// (2026-08-16b) is what makes it cheap: (i0, i0+1) are CONTIGUOUS and the two
// components are interleaved, so one corner PAIR of a row is 4 consecutive
// floats — a single 128-bit load. Eight lanes therefore cost 4 quad-loads +
// an 8x4 transpose per row instead of 32 scalar loads, and the whole tap is
// 32 loads + 4 transposes instead of 8 x (8 loads + address math).
#if defined(__clang__)
#define PWATER_VEC8 1
#endif

#ifdef PWATER_VEC8
// WHY CLANG VECTOR EXTENSIONS AND NOT THE simde `_mm256_*` SPELLING THE REST OF
// THE TREE USES: `#pragma clang fp contract` is LEXICALLY scoped, and every
// `_mm256_mul_ps` / `_mm256_add_ps` is a function body inside simde's headers,
// compiled under the file-scope -ffp-contract=fast. A pragma at the CALL site
// therefore does not reach the arithmetic — measured: the bilinear below still
// came out as `fmla.4s` with FP_CONTRACT_OFF wrapped around it, and the tap
// disagreed with the scalar on 84 % of pixels, some by a whole neighbouring
// texel (6e-4, not a rounding). Written as native vector operators in THIS
// file, the pragma binds to the arithmetic and the contraction is actually
// pinned. Same reason the fused sites use __builtin_elementwise_fma rather
// than simde's fmadd: simde's is `a*b + c` in a header, i.e. fused only by
// grace of the build flag.
typedef float   pwf4 __attribute__((ext_vector_type(4)));
typedef float   pwf8 __attribute__((ext_vector_type(8)));
typedef int32_t pwi8 __attribute__((ext_vector_type(8)));

static inline pwf4 pwLd4(const float* p) { pwf4 v; __builtin_memcpy(&v, p, sizeof v); return v; }
static inline pwf8 pwLd8(const float* p) { pwf8 v; __builtin_memcpy(&v, p, sizeof v); return v; }
static inline void pwSt8(float* p, pwf8 v) { __builtin_memcpy(p, &v, sizeof v); }
// a*b + c, GUARANTEED fused (the sites the scalar emits as fmla/fmls/fnmsub).
static inline pwf8 pwFma(pwf8 a, pwf8 b, pwf8 c) {
#if __has_builtin(__builtin_elementwise_fma)
	return __builtin_elementwise_fma(a, b, c);
#else
	FP_CONTRACT_FAST
	return a * b + c;
#endif
}

// Load 4 lanes' corner quads {a0,a1,b0,b1} and transpose to lane-major. Pure
// data movement — no arithmetic, so nothing here can perturb a bit.
static inline void wnQuad4(const float* fld, const int32_t* ofs,
                           pwf4& A0, pwf4& A1, pwf4& B0, pwf4& B1) {
	const pwf4 r0 = pwLd4(fld + ofs[0]);
	const pwf4 r1 = pwLd4(fld + ofs[1]);
	const pwf4 r2 = pwLd4(fld + ofs[2]);
	const pwf4 r3 = pwLd4(fld + ofs[3]);
	const pwf4 t0 = __builtin_shufflevector(r0, r1, 0, 4, 1, 5);
	const pwf4 t1 = __builtin_shufflevector(r2, r3, 0, 4, 1, 5);
	const pwf4 t2 = __builtin_shufflevector(r0, r1, 2, 6, 3, 7);
	const pwf4 t3 = __builtin_shufflevector(r2, r3, 2, 6, 3, 7);
	A0 = __builtin_shufflevector(t0, t1, 0, 1, 4, 5);
	A1 = __builtin_shufflevector(t0, t1, 2, 3, 6, 7);
	B0 = __builtin_shufflevector(t2, t3, 0, 1, 4, 5);
	B1 = __builtin_shufflevector(t2, t3, 2, 3, 6, 7);
}
static inline pwf8 wnJoin(pwf4 lo, pwf4 hi) {
	return __builtin_shufflevector(lo, hi, 0, 1, 2, 3, 4, 5, 6, 7);
}

// 8-wide sampleWaterNrm. Same wrap, same clamp, same bilinear order.
static inline void sampleWaterNrm8(pwf8 u, pwf8 v, pwf8& nx, pwf8& nz) {
	const pwf8 kN   = float(WNRM);
	const pwf8 kOne = 1.0f;
	// fu = u - floor(u/N)*N — the scalar's FUSED `fmls`.
	pwf8 fu, fv;
	{
		FP_CONTRACT_OFF
		fu = pwFma(-__builtin_elementwise_floor(u * (1.0f / float(WNRM))), kN, u);
		fv = pwFma(-__builtin_elementwise_floor(v * (1.0f / float(WNRM))), kN, v);
	}
	// i0 = int(fu); if (i0 >= N) i0 = 0 — the scalar's `cmge #127` + `and`.
	pwi8 i0 = __builtin_convertvector(fu, pwi8);
	pwi8 j0 = __builtin_convertvector(fv, pwi8);
	i0 &= (i0 < WNRM);
	j0 &= (j0 < WNRM);
	// float index of corner a = (j0*WNRM_S + i0)*2; j0*129 = (j0<<7)+j0, the
	// scalar's `add x9, x9, x9, lsl #7`.
	const pwi8 idx = (((j0 << 7) + j0) + i0) << 1;
	int32_t ofsA[8], ofsC[8];
	__builtin_memcpy(ofsA, &idx, sizeof ofsA);
	for (int k = 0; k < 8; ++k) ofsC[k] = ofsA[k] + WNRM_S * 2;   // row j0+1
	const float* const fld = g_waterNrm.data();
	pwf4 a0l,a1l,b0l,b1l, a0h,a1h,b0h,b1h, c0l,c1l,d0l,d1l, c0h,c1h,d0h,d1h;
	wnQuad4(fld, ofsA + 0, a0l, a1l, b0l, b1l);
	wnQuad4(fld, ofsA + 4, a0h, a1h, b0h, b1h);
	wnQuad4(fld, ofsC + 0, c0l, c1l, d0l, d1l);
	wnQuad4(fld, ofsC + 4, c0h, c1h, d0h, d1h);
	// THE BILINEAR — plain mul/add, exactly the scalar's fmul/fmul/fadd shape.
	// This is the block that must NOT fuse; the 2026-08-16b modulo round moved
	// 4 px purely because an edit let clang re-contract this chain.
	{
		FP_CONTRACT_OFF
		const pwf8 du   = fu - __builtin_convertvector(i0, pwf8);
		const pwf8 dv   = fv - __builtin_convertvector(j0, pwf8);
		const pwf8 omdu = kOne - du;
		const pwf8 omdv = kOne - dv;
		{
			const pwf8 A = wnJoin(a0l, a0h), B = wnJoin(b0l, b0h);
			const pwf8 C = wnJoin(c0l, c0h), D = wnJoin(d0l, d0h);
			const pwf8 p = omdu * A + du * B;
			const pwf8 q = omdu * C + du * D;
			nx = omdv * p + dv * q;
		}
		{
			const pwf8 A = wnJoin(a1l, a1h), B = wnJoin(b1l, b1h);
			const pwf8 C = wnJoin(c1l, c1h), D = wnJoin(d1l, d1h);
			const pwf8 p = omdu * A + du * B;
			const pwf8 q = omdu * C + du * D;
			nz = omdv * p + dv * q;
		}
	}
}

// 8-wide waterWaveSlope. `wxs`/`wzs` are 8 world positions; lanes are independent.
static inline void waterWaveSlope8(const float* wxs, const float* wzs,
                                   float t, float scale,
                                   float* bnxs, float* bnzs) {
	const float base = 0.02f * scale;
	const pwf8 kBase = base;
	const pwf8 kT16  = t * 16.0f;
	const pwf8 kT12  = t * 12.0f;
	const pwf8 kT10  = t * 10.0f;
	const pwf8 kT14  = t * 14.0f;
	const pwf8 k17   = 1.7f;
	const pwf8 wx = pwLd8(wxs);
	const pwf8 wz = pwLd8(wzs);
	pwf8 u1, v1, u2, v2;
	{
		FP_CONTRACT_OFF
		// rx = wx*ca - wz*sa  /  rz = wx*sa + wz*ca — both FUSED in the scalar
		// (`fnmsub` / `fmadd`), with only the SECOND product rounded.
		const pwf8 kCa = 0.87f, kSa = 0.5f;
		const pwf8 rx = pwFma(wx, kCa, -(wz * kSa));
		const pwf8 rz = pwFma(wx, kSa,   wz * kCa);
		// The four scroll coords — NOT fused in the scalar: each is
		// (world product) +/- (t product) with BOTH separately rounded.
		u1 = wx * kBase + kT16;
		v1 = wz * kBase - kT12;
		u2 = k17 * (rx * kBase) - kT10;
		v2 = k17 * (rz * kBase) + kT14;
	}
	pwf8 n1x, n1z, n2x, n2z;
	sampleWaterNrm8(u1, v1, n1x, n1z);
	sampleWaterNrm8(u2, v2, n2x, n2z);
	// bn = (n1 + n2*0.6) * 1.6 — the mix IS fused in the scalar (`fmla`).
	{
		FP_CONTRACT_OFF
		const pwf8 k06 = 0.6f;
		pwSt8(bnxs, pwFma(k06, n2x, n1x) * 1.6f);
		pwSt8(bnzs, pwFma(k06, n2z, n1z) * 1.6f);
	}
}
#endif  // PWATER_VEC8

// The dispatcher both screen passes call. `n` is 1..8 and every array is 8
// floats wide regardless. Lanes >= n are PADDED with lane 0 rather than left
// stale: a garbage float there would sail through the wrap (floor(inf) -> inf,
// cvttps -> INT_MIN, which the `i0 < N` clamp KEEPS because it is negative) and
// index the field out of bounds. Cheap insurance, paid only on a row's tail.
static inline void waveSlopeBatch(const float* wxs, const float* wzs, int n,
                                  float t, float scale, float* bnxs, float* bnzs) {
	if (g_waterNrm.empty()) {
		for (int k = 0; k < n; ++k) { bnxs[k] = 0.0f; bnzs[k] = 0.0f; }
		return;
	}
#ifdef PWATER_VEC8
	if (fds::FeatureFlags::water_slope_vec8()) {
		if (n >= 8) {
			waterWaveSlope8(wxs, wzs, t, scale, bnxs, bnzs);
		} else {
			float px[8], pz[8], qx[8], qz[8];
			for (int k = 0; k < 8; ++k) {
				const int s = k < n ? k : 0;
				px[k] = wxs[s]; pz[k] = wzs[s];
			}
			waterWaveSlope8(px, pz, t, scale, qx, qz);
			for (int k = 0; k < n; ++k) { bnxs[k] = qx[k]; bnzs[k] = qz[k]; }
		}
		return;
	}
#endif
	for (int k = 0; k < n; ++k)
		waterWaveSlope(wxs[k], wzs[k], t, scale, /*nOct=*/0, bnxs[k], bnzs[k]);
}

// VARIED wave slope (water_variation ON — chase only). A SEPARATE function from
// the byte-identical waterWaveSlope() above (touching that reshapes its fmadd
// chain under -ffp-contract=fast+LTO → city moves). Adds a low-frequency world
// SWELL + a 3rd rotated ripple octave the 128-texel tile can't hold, so the sea
// gets large-scale structure and stops reading as a uniform repeating field.
static inline void waterWaveSlopeVaried(float wx, float wz, float t, float scale,
                                        float& bnx, float& bnz) {
	if (g_waterNrm.empty()) { bnx = bnz = 0.0f; return; }
	const float base = 0.02f * scale;
	float n1x, n1z; sampleWaterNrm(wx*base + t*16.0f, wz*base - t*12.0f, n1x, n1z);
	const float ca = 0.87f, sa = 0.5f;
	const float rx = wx*ca - wz*sa, rz = wx*sa + wz*ca;
	float n2x, n2z; sampleWaterNrm(rx*base*1.7f - t*10.0f, rz*base*1.7f + t*14.0f, n2x, n2z);
	bnx = (n1x + n2x*0.6f) * 1.6f;
	bnz = (n1z + n2z*0.6f) * 1.6f;
	// (1) Large-scale swell (wavelengths of thousands of units, slow drift).
	const float td = t * 0.5f;
	bnx += 0.85f * std::cos(wx*0.00034f + wz*0.00019f + td*1.6f);
	bnz += 0.85f * std::cos(wz*0.00050f - wx*0.00024f - td*1.3f);
	// a slower, longer macro roll (~6-9k unit wavelength).
	bnx += 0.55f * std::cos(wx*0.00015f - wz*0.00011f + td*0.7f);
	bnz += 0.55f * std::sin(wz*0.00013f + wx*0.00009f - td*0.85f);
	// (2) a 3rd ripple octave at a different scale + rotation (off the tile period).
	const float e = base * 2.55f;
	const float qx = wx*0.62f - wz*0.78f, qz = wx*0.78f + wz*0.62f;   // ~51 deg
	float n3x, n3z; sampleWaterNrm(qx*e + t*23.0f, qz*e - t*17.0f, n3x, n3z);
	bnx += n3x * 0.5f;
	bnz += n3z * 0.5f;
}

// Procedural water-detail texture (row-major), sampled by the screen-space water
// pass at a field-WARPED UV per pixel. Generated at init (buildWaterDetail) —
// replaces de-swizzling the original low-res albedo (no swizzle dependence, no
// blockiness).
static std::vector<dword> g_waterTex;
// EVERY consumer of the detail texture collapses it the same way, on the very
// next line: `cell = (cb + cg + cr) * (1/765)`. The per-channel colour is never
// used. So the sampler bilinearly interpolated THREE channels — three sets of
// four byte-extracts, four int->float converts and seven flops — to produce one
// scalar that a single lerp over the pre-summed plane gives directly. The corner
// sums are exact (three bytes, <= 765, exactly representable), so only the lerp
// itself reassociates. The dword plane is kept for the FDS_WATERTEX_DUMP debug.
static std::vector<float> g_waterCell;
static int g_waterTexW = 0, g_waterTexH = 0;

static void buildWaterDetail() {
	// Procedural, high-res, row-major → no swizzle dependence (no V1/V2 trap) and
	// no blockiness (the original albedo was low-res). A soft tiling Worley cell
	// field tinted blue-cyan; the water pass samples it field-warped for coherence.
	const int N = 256;
	g_waterTex.assign(size_t(N) * size_t(N), 0u);
	g_waterCell.assign(size_t(N) * size_t(N), 0.0f);
	g_waterTexW = g_waterTexH = N;
	const int G = 14;                                       // feature-cell grid (tiles seamlessly via %G)
	auto fp = [](int gx, int gy, int ax) -> float {
		uint32_t h = uint32_t(gx)*73856093u ^ uint32_t(gy)*19349663u ^ uint32_t(ax+1)*2654435761u;
		h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
		return float(h & 0xFFFFu) * (1.0f/65535.0f);
	};
	const float cs = float(N) / float(G);
	for (int py = 0; py < N; ++py) for (int px = 0; px < N; ++px) {
		const float fx = (px + 0.5f) / cs, fy = (py + 0.5f) / cs;
		const int cx = int(fx), cy = int(fy);
		float d1 = 1e9f, d2 = 1e9f;                              // nearest two feature dists
		for (int oy = -1; oy <= 1; ++oy) for (int ox = -1; ox <= 1; ++ox) {
			const int gx = ((cx+ox) % G + G) % G, gy = ((cy+oy) % G + G) % G;
			const float dx = (cx+ox + fp(gx,gy,0)) - fx;
			const float dy = (cy+oy + fp(gx,gy,1)) - fy;
			const float d = dx*dx + dy*dy;
			if (d < d1) { d2 = d1; d1 = d; } else if (d < d2) { d2 = d; }
		}
		// Caustic NETWORK: bright thin lines along cell boundaries (where the two
		// nearest features are equidistant → F2-F1 ≈ 0) — the recognizable water look.
		const float edge = std::sqrt(d2) - std::sqrt(d1);
		float c = 1.0f - edge / 0.38f; if (c < 0) c = 0; c = c*c;
		int B = int(40 + c*175), Gn = int(52 + c*190), R = int(30 + c*120);
		if (B>255)B=255; if (Gn>255)Gn=255; if (R>255)R=255;
		g_waterTex[size_t(py)*N + px] = dword(B) | (dword(Gn)<<8) | (dword(R)<<16) | 0xFF000000u;
		g_waterCell[size_t(py)*N + px] = float(B + Gn + R);   // the only value anyone reads
	}
	if (std::getenv("FDS_WATERTEX_DUMP")) {                  // debug: see the texture itself
		FILE* f = fopen("/tmp/water_tex.ppm", "wb");
		if (f) {
			fprintf(f, "P6\n%d %d\n255\n", N, N);
			for (int i = 0; i < N*N; ++i) {
				const dword c = g_waterTex[i];
				const unsigned char rgb[3] = { (unsigned char)((c>>16)&0xFF), (unsigned char)((c>>8)&0xFF), (unsigned char)(c&0xFF) };
				fwrite(rgb, 1, 3, f);
			}
			fclose(f);
		}
	}
}

// Bilinear tap on the pre-summed cell plane. Returns cb+cg+cr directly — see
// g_waterCell for why the three channels never had to be interpolated apart.
static inline float sampleWaterCell(float u, float v) {
	const int W = g_waterTexW, H = g_waterTexH;
	float fu = u - std::floor(u / W) * W;                       // wrap
	float fv = v - std::floor(v / H) * H;
	int i0 = int(fu), j0 = int(fv); if (i0 >= W) i0 = 0; if (j0 >= H) j0 = 0;
	const float du = fu - i0, dv = fv - j0;
	const int i1 = (i0 + 1) % W, j1 = (j0 + 1) % H;
	const float A = g_waterCell[size_t(j0)*W + i0], B = g_waterCell[size_t(j0)*W + i1];
	const float C = g_waterCell[size_t(j1)*W + i0], D = g_waterCell[size_t(j1)*W + i1];
	return (A*(1-du)+B*du)*(1-dv) + (C*(1-du)+D*du)*dv;
}

// RenderGlints' OWN cell tap, with the interpolation's fma grouping PINNED.
// Same arithmetic as sampleWaterCell above and deliberately a separate copy —
// the shared one stays exactly as chase's causticCellVaried and the env-bake
// CausticModulation compiled it, which is the same "separate function so the
// default path stays byte-identical" rule RenderGlintsVaried already follows.
//
// WHY IT HAS TO EXIST: batching the wave slope moves this tap into an outlined
// flush, and clang then contracts the final lerp the other way round —
// `fma(dv, q, p*(1-dv))` instead of the pre-batch `fma(1-dv, p, dv*q)`. Same
// algebra, opposite choice of which product keeps its extra bits, and it lands
// on `int(colour*mod + blueLine)` where a 1-LSB shift is a changed pixel.
static inline float sampleWaterCellGlints(float u, float v) {
	const int W = g_waterTexW, H = g_waterTexH;
	float fu = u - std::floor(u / W) * W;                       // wrap
	float fv = v - std::floor(v / H) * H;
	int i0 = int(fu), j0 = int(fv); if (i0 >= W) i0 = 0; if (j0 >= H) j0 = 0;
	const float du = fu - i0, dv = fv - j0;
	const int i1 = (i0 + 1) % W, j1 = (j0 + 1) % H;
	const float A = g_waterCell[size_t(j0)*W + i0], B = g_waterCell[size_t(j0)*W + i1];
	const float C = g_waterCell[size_t(j1)*W + i0], D = g_waterCell[size_t(j1)*W + i1];
	{
		FP_CONTRACT_OFF
		const float omdu = 1.0f - du, omdv = 1.0f - dv;
		const float p = FDS_FMAF(B, du, A * omdu);
		const float q = FDS_FMAF(D, du, C * omdu);
		return FDS_FMAF(omdv, p, dv * q);
	}
}

// VARIED caustic cell value (water_variation ON — chase only). SEPARATE from the
// callers' byte-identical inline. Sums THREE scales/rotations (base + a finer
// rotated octave + a coarse macro octave) so the Worley network stops visibly
// tiling at texScale.
static inline float causticCellVaried(float wx, float wz, float bnx, float bnz,
                                      float texScale, float texWarp,
                                      float flowU, float flowV) {
	const float cell = sampleWaterCell(wx*texScale + bnx*texWarp + flowU,
	                                   wz*texScale + bnz*texWarp + flowV) * (1.0f/765.0f);
	const float ca = 0.80f, sa = 0.60f;
	const float rx = wx*ca - wz*sa, rz = wx*sa + wz*ca;
	const float c2 = sampleWaterCell(rx*texScale*2.3f + bnx*texWarp*0.7f + flowU*1.6f,
	                                 rz*texScale*2.3f + bnz*texWarp*0.7f - flowV*1.2f) * (1.0f/765.0f);
	const float c3 = sampleWaterCell(wx*texScale*0.45f + bnx*texWarp*1.6f - flowU*0.5f,
	                                 wz*texScale*0.45f + bnz*texWarp*1.6f + flowV*0.5f) * (1.0f/765.0f);
	return cell*0.5f + c2*0.28f + c3*0.22f;
}

// ───────── Instrument: the pixel census (--water_census) ─────────
// Both water passes scan the WHOLE framebuffer and ray-cast every pixel to the
// water plane. This counts what that scan actually finds, so every "does this
// work produce anything visible" question has a denominator. Deliberately a
// SEPARATE sweep (classification math only, no shading) so the shading loops
// stay untouched — the census arm and the timing arm are different runs anyway.
namespace {
struct CensusAcc {
	const char* tag = nullptr;
	long long frames = 0, total = 0, rejD = 0, rejNear = 0, rejFar = 0, rejOccl = 0, live = 0;
};
CensusAcc g_census[4];
bool g_censusPrinted = false;

void censusPrint() {
	if (g_censusPrinted) return;
	g_censusPrinted = true;
	bool any = false;
	for (const CensusAcc& a : g_census) if (a.tag && a.frames) { any = true; break; }
	if (!any) return;
	printf("[WCENSUS] ==== water-pass pixel classification (per frame) ====\n");
	printf("[WCENSUS] %-14s %8s %12s %9s %9s %9s %9s %11s %7s\n",
	       "pass", "frames", "scanned/f", "rej_D", "rej_near", "rej_far", "rej_occl", "LIVE/f", "live%");
	for (const CensusAcc& a : g_census) {
		if (!a.tag || !a.frames) continue;
		const double f = double(a.frames);
		printf("[WCENSUS] %-14s %8lld %12.0f %9.0f %9.0f %9.0f %9.0f %11.0f %6.2f%%\n",
		       a.tag, a.frames, double(a.total)/f, double(a.rejD)/f, double(a.rejNear)/f,
		       double(a.rejFar)/f, double(a.rejOccl)/f, double(a.live)/f,
		       a.total ? 100.0*double(a.live)/double(a.total) : 0.0);
	}
	printf("[WCENSUS] rej_near = ray misses the plane in front of the eye (sd<=1) — the ABOVE-HORIZON half.\n");
	printf("[WCENSUS] rej_far  = plane hit past the far plane. rej_occl = opaque geometry in front (glints only;\n");
	printf("[WCENSUS]            the city ripple map has NO occlusion test, so its LIVE count includes pixels\n");
	printf("[WCENSUS]            covered by buildings whose dispMap entry is never read).\n");
	printf("[WCENSUS] LIVE = pixels that get a full wave-slope evaluation (+ caustics + specular for glints).\n");
}
CensusAcc* censusSlot(const char* tag) {
	for (CensusAcc& a : g_census) {
		if (a.tag && std::strcmp(a.tag, tag) == 0) return &a;
		if (!a.tag) { a.tag = tag; static bool reg = (std::atexit(censusPrint), true); (void)reg; return &a; }
	}
	return nullptr;
}
}  // namespace

void Census(const char* tag, float waterY, bool useOcclusion, bool useFarCut) {
	if (!fds::FeatureFlags::water_census() || !View) return;
	CensusAcc* a = censusSlot(tag);
	if (!a) return;
	const float invZScale = (g_zscale != 0.0f) ? 1.0f / g_zscale : 1.0f;
	const float invFX = (FOVX != 0.0f) ? 1.0f / FOVX : 0.0f;
	const float invFY = (FOVY != 0.0f) ? 1.0f / FOVY : 0.0f;
	const float cex = CntrEX, cey = CntrEY;
	const float m01=View->Mat[0][1], m11=View->Mat[1][1], m21=View->Mat[2][1];
	const float ey=View->ISource.y;
	const float fzp = useFarCut && CurScene && CurScene->FZP > 0.0f ? CurScene->FZP : 1e30f;
	const uint16_t* const oz = ZPage16;
	++a->frames;
	for (int y = 0; y < YRes; ++y) {
		const uint16_t* orow = oz + size_t(y) * size_t(XRes);
		for (int x = 0; x < XRes; ++x) {
			++a->total;
			const float xn = (float(x) - cex) * invFX;
			const float yn = (cey - float(y)) * invFY;
			const float D = m01*xn + m11*yn + m21;
			if (D == 0.0f) { ++a->rejD; continue; }
			const float sd = (waterY - ey) / D;
			if (sd <= 1.0f)  { ++a->rejNear; continue; }
			if (sd >= fzp)   { ++a->rejFar;  continue; }
			if (useOcclusion) {
				const uint16_t oe = orow[x];
				if (oe != 0 && float(0xFF80 - int(oe)) * invZScale < sd) { ++a->rejOccl; continue; }
			}
			++a->live;
		}
	}
}

// ───────── Public API ─────────

void BuildField() {
	buildWaterNormalMap();
	buildWaterDetail();
}

void WaveSlope(float wx, float wz, float t, float scale, float& bnx, float& bnz) {
	waterWaveSlope(wx, wz, t, scale, /*nOct=*/0, bnx, bnz);
}

void WaveSlope8(const float* wxs, const float* wzs, int n,
                float t, float scale, float* bnxs, float* bnzs) {
	waveSlopeBatch(wxs, wzs, n, t, scale, bnxs, bnzs);
}

// See ProceduralWater.h — RenderGlints' texMix block as a standalone helper
// (same constants, same all-octaves slope warp) for the city env-bake water
// re-shade. Kept OUT of RenderGlints' loop so the screen pass still computes
// its wave slope once per pixel for both the warp and the glint normal.
bool CausticModulation(float wx, float wz, float t, float scale,
                       float texMix, float texScale, float texWarp,
                       float flowU, float flowV,
                       float& mod, float& blueAdd)
{
	if (texMix <= 0.0f || g_waterTexW <= 0 || g_waterNrm.empty()) return false;
	float bnx, bnz;
	waterWaveSlope(wx, wz, t, scale, /*nOct=*/6, bnx, bnz);
	const float cell = sampleWaterCell(wx*texScale + bnx*texWarp + flowU,
	                                   wz*texScale + bnz*texWarp + flowV) * (1.0f/765.0f);
	const float cellHi = cell - 0.40f;                     // + on lines, - in base
	mod     = 1.0f + cellHi * 2.0f * texMix * 0.6f;
	blueAdd = (cellHi > 0.0f ? cellHi : 0.0f) * texMix * 220.0f;
	return true;
}

// Bump-mapped specular GLINTS on the water surface — the "bump" proper, vs the
// reflection-displacement ripple. The deferred kernel skips lighting on the
// water, so it has no specular response; this adds it. Per pixel we RAY-CAST
// the view ray to the known water plane (y = waterY) and keep the hit only if
// it isn't occluded by opaque geometry (ZPage16). This is peel-INDEPENDENT —
// unlike reading the transparent G-buffer's front layer, which doesn't hold the
// water under a multi-pass xpar peel. The wave normal is built in WORLD space
// (the hit's XZ) → grounded, perspective-correct, organic; the highlight is
// view-dependent (half-vector). 8-bit VPage; row-parallel.
void RenderGlints(float waterY, float minX, float maxX, float minZ, float /*maxZ*/) {
	const float strength = fds::FeatureFlags::water_bump_strength();
	if (strength <= 0.0f || !View) return;
	if (maxX <= minX) return;                      // water extent not set yet
	Census("glints", waterY, /*useOcclusion=*/true, /*useFarCut=*/true);
	const float shin  = std::max(1.0f, fds::FeatureFlags::water_bump_shininess());
	const float scale = fds::FeatureFlags::water_bump_scale();
	// Field-warped albedo texture (only in procedural mode): the SAME field
	// distorts the texture sample → the texture ripples with the waves.
	// WaterProceduralEffective = the water material's per-surface tri-state
	// (sidecar 'waterProcedural') with the global flag as the auto fallback —
	// keeps this pass in lockstep with the deferred kernel's waterProc hoist.
	const float texMix   = WaterProceduralEffective() ? fds::FeatureFlags::water_albedo_mix() : 0.0f;
	const float texScale = fds::FeatureFlags::water_tex_scale();
	const float texWarp  = fds::FeatureFlags::water_tex_warp();
	const float t = (float)Timer * 0.02f * fds::FeatureFlags::water_ripple_speed();
	// Texture FLOW: translate the caustic UV at the wave field's world velocity
	// (the field scrolls g_waterNrm by t*16/-12 texels at frequency `base`; the
	// texture is at frequency `texScale`, so texScale/base matches world speed).
	// water_tex_flow scales it (1 = ride the glints, 0 = warp-only/in-place).
	const float wnBase = 0.02f * scale;                  // mirrors waterWaveSlope()
	const float flowK  = (wnBase > 0.0f) ? (texScale / wnBase) * fds::FeatureFlags::water_tex_flow() : 0.0f;
	const float flowU  =  t * 16.0f * flowK;
	const float flowV  = -t * 12.0f * flowK;
	const float invZScale = (g_zscale != 0.0f) ? 1.0f / g_zscale : 1.0f;
	const float invFX = (FOVX != 0.0f) ? 1.0f / FOVX : 0.0f;
	const float invFY = (FOVY != 0.0f) ? 1.0f / FOVY : 0.0f;
	const float cex = CntrEX, cey = CntrEY;
	// camera world->view rotation (rows = camera axes) + eye → view-to-world.
	const float m00=View->Mat[0][0], m10=View->Mat[1][0], m20=View->Mat[2][0];
	const float m01=View->Mat[0][1], m11=View->Mat[1][1], m21=View->Mat[2][1];
	const float m02=View->Mat[0][2], m12=View->Mat[1][2], m22=View->Mat[2][2];
	const float ex=View->ISource.x, ey=View->ISource.y, ez=View->ISource.z;
	// Key light (world): glint where the wave normal faces the V+L half-vector.
	float Lx=0.35f, Ly=0.82f, Lz=0.42f; { const float l=std::sqrt(Lx*Lx+Ly*Ly+Lz*Lz); Lx/=l;Ly/=l;Lz/=l; }
	// Wave SLOPE comes from waterWaveSlope() — the SAME field the reflection
	// ripple uses, so glints and reflection agree (coherent water).
	const float bumpAmp = 1.7f;
	const float wYplane = waterY;
	// The specular lobe cannot brighten a pixel below a KNOWN half-vector
	// threshold, and powf is the single most expensive operation in this loop —
	// a libm CALL, taken on every pixel that reaches it. The tail is exact, not
	// approximate: the write is `add = int(g*255 + 0.5)` with
	// g = pow(ndh,shin)*strength*distFade and distFade <= 1, so add == 0 — i.e.
	// `continue` — for every ndh with pow(ndh,shin)*strength < 0.5/255.
	// Inverting that once per pass turns it into a plain compare that provably
	// skips only pixels whose output was already zero. BIT-EXACT.
	const float ndhMin = std::pow(0.5f / (255.0f * strength), 1.0f / shin) * 0.99999f;
	// Cut glints off at the far plane and fade them over the last stretch —
	// otherwise the grazing water near the horizon spikes the specular into
	// a bright artifact band.
	const float fzp = (CurScene && CurScene->FZP > 0.0f) ? CurScene->FZP : 1e30f;
	const float fadeStart = fzp * 0.55f;
	const float invFadeRange = 1.0f / std::max(1.0f, fzp - fadeStart);
	dword* const vp = (dword*)VPage;
	const uint16_t* const oz = ZPage16;
	const int xr = XRes;
	auto band = [=](int y0, int y1) {
		// The wave slope is evaluated 8 pixels at a time (see waterWaveSlope8).
		// Only pixels that SURVIVE all three rejects enter a batch — 44.6 % of
		// the scan at city t=1961 ([WCENSUS]) — so the vector call never pays
		// for the above-horizon half or for pixels behind buildings. Everything
		// downstream of the slope stays per-pixel and verbatim.
		int   bIdx[8];
		float bWx[8] = {0}, bWz[8] = {0}, bFade[8] = {0}, bNx[8], bNz[8];
		int   nB = 0;
		for (int y = y0; y < y1; ++y) {
			dword* row = vp + size_t(y) * size_t(xr);
			const uint16_t* orow = oz + size_t(y) * size_t(xr);
			// The per-pixel tail, unchanged — each pixel touches only its own
			// row[x], so deferring it to the flush cannot reorder any write.
			auto shade = [&](int x, float wx, float wz, float distFade,
			                 float bnx, float bnz) {
				// Field-warped texture: sample the generated cell texture at a UV
				// distorted by the SAME wave slope → it ripples with the waves
				// (coherent). MODULATE the water brightness by the cell value
				// (rather than lerp a cyan colour over cyan water — invisible) so
				// the cells show as light/dark detail ON TOP of the reflection,
				// not hiding it.
				if (texMix > 0.0f && g_waterTexW > 0) {
					// Warp = ripple-in-place (wave slope); flow = translate with the
					// field at its world velocity (same scroll as the glints) so the
					// caustics ride the waves instead of standing still.
					const float cell = sampleWaterCellGlints(wx*texScale + bnx*texWarp + flowU,
					                                         wz*texScale + bnz*texWarp + flowV) * (1.0f/765.0f);
					const float cellHi = cell - 0.40f;                   // + on lines, - in base
					// Uniform (hue-preserving) brightness contrast keeps the base blue
					// instead of warming it...
					const float mod = 1.0f + cellHi * 2.0f * texMix * 0.6f;
					// ...plus an ADDITIVE blue tint on the bright caustic lines: only
					// bluens (never warms), so the caustics read distinctly blue.
					const float blueLine = (cellHi > 0.0f ? cellHi : 0.0f) * texMix * 220.0f;
					const dword p = row[x];
					int nb = int(float(p & 0xFFu)     * mod + blueLine);
					int ng = int(float((p>>8)&0xFFu)  * mod + blueLine * 0.40f);
					int nr = int(float((p>>16)&0xFFu) * mod + blueLine * 0.08f);
					if (nb<0)nb=0; if (ng<0)ng=0; if (nr<0)nr=0;
					if (nb>255)nb=255; if (ng>255)ng=255; if (nr>255)nr=255;
					row[x] = dword(nb) | (dword(ng)<<8) | (dword(nr)<<16) | 0xFF000000u;
				}
				float Nx = bnx * bumpAmp, Ny = 1.0f, Nz = bnz * bumpAmp;
				const float nInv = 1.0f / std::sqrt(Nx*Nx + Ny*Ny + Nz*Nz);
				Nx*=nInv; Ny*=nInv; Nz*=nInv;
				float Vx = ex-wx, Vy = ey-wYplane, Vz = ez-wz;   // view dir (worldPos.y = plane)
				// PINNED CONTRACTION — do NOT "simplify" this back to
				// `Vx*Vx + Vy*Vy + Vz*Vz`. Vy is loop-INVARIANT (ey and the water
				// plane are both constants of the pass). The pre-batch loop
				// compiled that sum to `fmul Vx,Vx` + `fmadd Vy,Vy,·` +
				// `fmadd Vz,Vz,·` — only the FIRST product rounded, the other two
				// absorbed by -ffp-contract=fast. Once the slope moved to a batch
				// flush, LICM hoisted the invariant `Vy*Vy` out of the loop, which
				// ROUNDS it and leaves `Vx*Vx` fused instead. Same algebra,
				// swapped rounding — and it moved 143 px by 1 LSB through the
				// `int(g*255 + 0.5)` truncation below. Spelling the chain out with
				// contraction off restores the pre-batch grouping exactly, which
				// is what makes this pass byte-null.
				float vLen2;
				{ FP_CONTRACT_OFF vLen2 = FDS_FMAF(Vz, Vz, FDS_FMAF(Vy, Vy, Vx*Vx)); }
				const float vInv = 1.0f / std::sqrt(vLen2);
				Vx*=vInv; Vy*=vInv; Vz*=vInv;
				float Hx = Vx+Lx, Hy = Vy+Ly, Hz = Vz+Lz;
				const float hInv = 1.0f / std::sqrt(Hx*Hx + Hy*Hy + Hz*Hz);
				const float ndh = (Nx*Hx + Ny*Hy + Nz*Hz) * hInv;
				if (ndh < ndhMin) return;      // provably add == 0 (see ndhMin)
				const float g = std::pow(ndh, shin) * strength * distFade;
				int add = int(g * 255.0f + 0.5f); if (add <= 0) return; if (add > 255) add = 255;
				const dword p = row[x];
				int B = int(p & 0xFFu) + add, G = int((p>>8)&0xFFu) + add, R = int((p>>16)&0xFFu) + add;
				if (B>255)B=255; if (G>255)G=255; if (R>255)R=255;
				row[x] = dword(B) | (dword(G)<<8) | (dword(R)<<16) | 0xFF000000u;
			};
			auto flush = [&]() {
				waveSlopeBatch(bWx, bWz, nB, t, scale, bNx, bNz);   // all octaves (fine detail)
				for (int k = 0; k < nB; ++k)
					shade(bIdx[k], bWx[k], bWz[k], bFade[k], bNx[k], bNz[k]);
				nB = 0;
			};
			for (int x = 0; x < xr; ++x) {
				// Ray-cast this pixel to the water plane — a peel-independent mask
				// (the transparent G-buffer's front layer doesn't hold the water
				// under a multi-pass xpar peel). View ray dir per unit depth:
				const float xn = (float(x) - cex) * invFX;
				const float yn = (cey - float(y)) * invFY;
				const float D = m01*xn + m11*yn + m21;            // d(world.y)/d(view-z)
				if (D == 0.0f) continue;
				const float sd = (wYplane - ey) / D;              // view-z of the plane hit
				if (sd <= 1.0f || sd >= fzp) continue;            // behind the eye / past the far plane
				// NB: the water mesh's local bounds don't map to its world
				// placement, and the city sits in the water (everything below
				// the horizon that isn't opaque is water), so we gate on the
				// plane hit + opaque occlusion rather than an XZ rect.
				const uint16_t oe = orow[x];                      // opaque-Z occlusion
				if (oe != 0 && float(0xFF80 - int(oe)) * invZScale < sd) continue;   // opaque in front
				bFade[nB] = sd <= fadeStart ? 1.0f : (fzp - sd) * invFadeRange;
				bWx[nB]   = ex + sd * (m00*xn + m10*yn + m20);
				bWz[nB]   = ez + sd * (m02*xn + m12*yn + m22);
				bIdx[nB]  = x;
				if (++nB == 8) flush();
			}
			if (nB) flush();   // rows are independent; never carry a batch over
		}
	};
	runRowBands("water-glints", band);
}

// water_variation ON path (chase). A full COPY of RenderGlints() that swaps the
// two per-pixel operations for their varied forms (waterWaveSlopeVaried +
// causticCellVaried) — kept as a separate function, dispatched at the call site,
// so RenderGlints() above stays byte-for-byte identical (city/fountain gate).
void RenderGlintsVaried(float waterY, float minX, float maxX, float minZ, float /*maxZ*/) {
	const float strength = fds::FeatureFlags::water_bump_strength();
	if (strength <= 0.0f || !View) return;
	if (maxX <= minX) return;
	Census("glintsVaried", waterY, /*useOcclusion=*/true, /*useFarCut=*/true);
	const float shin  = std::max(1.0f, fds::FeatureFlags::water_bump_shininess());
	const float scale = fds::FeatureFlags::water_bump_scale();
	const float texMix   = WaterProceduralEffective() ? fds::FeatureFlags::water_albedo_mix() : 0.0f;
	const float texScale = fds::FeatureFlags::water_tex_scale();
	const float texWarp  = fds::FeatureFlags::water_tex_warp();
	const float t = (float)Timer * 0.02f * fds::FeatureFlags::water_ripple_speed();
	const float wnBase = 0.02f * scale;
	const float flowK  = (wnBase > 0.0f) ? (texScale / wnBase) * fds::FeatureFlags::water_tex_flow() : 0.0f;
	const float flowU  =  t * 16.0f * flowK;
	const float flowV  = -t * 12.0f * flowK;
	const float invZScale = (g_zscale != 0.0f) ? 1.0f / g_zscale : 1.0f;
	const float invFX = (FOVX != 0.0f) ? 1.0f / FOVX : 0.0f;
	const float invFY = (FOVY != 0.0f) ? 1.0f / FOVY : 0.0f;
	const float cex = CntrEX, cey = CntrEY;
	const float m00=View->Mat[0][0], m10=View->Mat[1][0], m20=View->Mat[2][0];
	const float m01=View->Mat[0][1], m11=View->Mat[1][1], m21=View->Mat[2][1];
	const float m02=View->Mat[0][2], m12=View->Mat[1][2], m22=View->Mat[2][2];
	const float ex=View->ISource.x, ey=View->ISource.y, ez=View->ISource.z;
	float Lx=0.35f, Ly=0.82f, Lz=0.42f; { const float l=std::sqrt(Lx*Lx+Ly*Ly+Lz*Lz); Lx/=l;Ly/=l;Lz/=l; }
	const float bumpAmp = 1.7f;
	const float wYplane = waterY;
	// The specular lobe cannot brighten a pixel below a KNOWN half-vector
	// threshold, and powf is the single most expensive operation in this loop —
	// a libm CALL, taken on every pixel that reaches it. The tail is exact, not
	// approximate: the write is `add = int(g*255 + 0.5)` with
	// g = pow(ndh,shin)*strength*distFade and distFade <= 1, so add == 0 — i.e.
	// `continue` — for every ndh with pow(ndh,shin)*strength < 0.5/255.
	// Inverting that once per pass turns it into a plain compare that provably
	// skips only pixels whose output was already zero. BIT-EXACT.
	const float ndhMin = std::pow(0.5f / (255.0f * strength), 1.0f / shin) * 0.99999f;
	const float fzp = (CurScene && CurScene->FZP > 0.0f) ? CurScene->FZP : 1e30f;
	const float fadeStart = fzp * 0.55f;
	const float invFadeRange = 1.0f / std::max(1.0f, fzp - fadeStart);
	dword* const vp = (dword*)VPage;
	const uint16_t* const oz = ZPage16;
	const int xr = XRes;
	auto band = [=](int y0, int y1) {
		for (int y = y0; y < y1; ++y) {
			dword* row = vp + size_t(y) * size_t(xr);
			const uint16_t* orow = oz + size_t(y) * size_t(xr);
			for (int x = 0; x < xr; ++x) {
				const float xn = (float(x) - cex) * invFX;
				const float yn = (cey - float(y)) * invFY;
				const float D = m01*xn + m11*yn + m21;
				if (D == 0.0f) continue;
				const float sd = (wYplane - ey) / D;
				if (sd <= 1.0f || sd >= fzp) continue;
				const float distFade = sd <= fadeStart ? 1.0f : (fzp - sd) * invFadeRange;
				const float wx = ex + sd * (m00*xn + m10*yn + m20);
				const float wz = ez + sd * (m02*xn + m12*yn + m22);
				const uint16_t oe = orow[x];
				if (oe != 0 && float(0xFF80 - int(oe)) * invZScale < sd) continue;
				float bnx, bnz;
				waterWaveSlopeVaried(wx, wz, t, scale, bnx, bnz);   // swell + 3rd octave
				if (texMix > 0.0f && g_waterTexW > 0) {
					// multi-scale caustics → breaks the tiling repeat.
					const float cell = causticCellVaried(wx, wz, bnx, bnz, texScale, texWarp, flowU, flowV);
					const float cellHi = cell - 0.40f;
					const float mod = 1.0f + cellHi * 2.0f * texMix * 0.6f;
					const float blueLine = (cellHi > 0.0f ? cellHi : 0.0f) * texMix * 220.0f;
					const dword p = row[x];
					int nb = int(float(p & 0xFFu)     * mod + blueLine);
					int ng = int(float((p>>8)&0xFFu)  * mod + blueLine * 0.40f);
					int nr = int(float((p>>16)&0xFFu) * mod + blueLine * 0.08f);
					if (nb<0)nb=0; if (ng<0)ng=0; if (nr<0)nr=0;
					if (nb>255)nb=255; if (ng>255)ng=255; if (nr>255)nr=255;
					row[x] = dword(nb) | (dword(ng)<<8) | (dword(nr)<<16) | 0xFF000000u;
				}
				float Nx = bnx * bumpAmp, Ny = 1.0f, Nz = bnz * bumpAmp;
				const float nInv = 1.0f / std::sqrt(Nx*Nx + Ny*Ny + Nz*Nz);
				Nx*=nInv; Ny*=nInv; Nz*=nInv;
				float Vx = ex-wx, Vy = ey-wYplane, Vz = ez-wz;
				const float vInv = 1.0f / std::sqrt(Vx*Vx + Vy*Vy + Vz*Vz);
				Vx*=vInv; Vy*=vInv; Vz*=vInv;
				float Hx = Vx+Lx, Hy = Vy+Ly, Hz = Vz+Lz;
				const float hInv = 1.0f / std::sqrt(Hx*Hx + Hy*Hy + Hz*Hz);
				const float ndh = (Nx*Hx + Ny*Hy + Nz*Hz) * hInv;
				if (ndh < ndhMin) continue;    // provably add == 0 (see ndhMin)
				const float g = std::pow(ndh, shin) * strength * distFade;
				int add = int(g * 255.0f + 0.5f); if (add <= 0) continue; if (add > 255) add = 255;
				const dword p = row[x];
				int B = int(p & 0xFFu) + add, G = int((p>>8)&0xFFu) + add, R = int((p>>16)&0xFFu) + add;
				if (B>255)B=255; if (G>255)G=255; if (R>255)R=255;
				row[x] = dword(B) | (dword(G)<<8) | (dword(R)<<16) | 0xFF000000u;
			}
		}
	};
	runRowBands("water-glints", band);
}

}  // namespace pwater
