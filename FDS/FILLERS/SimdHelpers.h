#pragma once
#include <simd/vectorclass.h>
#include <simde/x86/avx2.h>
#include <simde/x86/fma.h>
#include <array>
#include <cmath>

#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#endif

// Scalar approximate rsqrt — ~12 bits via 1 NR step. Far below visible
// threshold for shading math. Roughly 3-5 cycles vs ~24 for 1.0f/sqrtf
// on arm64. Falls back to 1.0f/sqrtf on non-arm64 (Clang on x86-64 with
// -msse will emit RSQRTSS + 1 NR for equivalent cost).
static inline float fast_rsqrt(float x) {
#if defined(__ARM_NEON) || defined(__aarch64__)
	float32x2_t v = vdup_n_f32(x);
	float32x2_t e = vrsqrte_f32(v);
	e = vmul_f32(vrsqrts_f32(vmul_f32(e, e), v), e);
	return vget_lane_f32(e, 0);
#else
	return 1.0f / std::sqrt(x);
#endif
}

// atan polynomial approximation — ~10 vec ops, ~0.001 rad max error.
// Used by the analytic halo path (FDS_VOL_HALO_ANALYTIC) to compute
// ∫1/(αz²+βz+γ)dz = (2/D)·arctan(...) in closed form without paying
// libm's scalar atan cost (~30 cycles per call × millions of calls/frame).
//
// Range reduction via atan(x) = sign(x)·π/2 − atan(1/x) for |x|>1, all
// branchless via blendv. Polynomial coefficients are a 5-term minimax
// fit to atan over [-1,1] (Chebyshev-derived, ~0.001 rad worst-case;
// well below visible threshold for a glow effect).
inline float atan_approx_unit(float x) {
    // x ∈ [-1, 1]. 4 muls + 4 adds via FMA.
    const float x2 = x * x;
    return x * (1.0f + x2 * (-0.330299f + x2 * (0.180142f
              + x2 * (-0.085133f + x2 * 0.020835f))));
}

inline float atan_approx(float x) {
    const float ax = std::fabs(x);
    if (ax <= 1.0f) return atan_approx_unit(x);
    const float inv = 1.0f / x;
    const float halfPi = 1.5707963267948966f;
    return (x > 0 ? halfPi : -halfPi) - atan_approx_unit(inv);
}

// atan2(y, x) — quadrant-corrected polynomial atan. Reduces to
// atan(t) on the smaller-magnitude side so |t| ≤ 1 (no range-reduce
// inside atan_approx_unit) and adds the quadrant offset. Used in
// Transform.cpp's reflective-face equirectangular EU/EV stamp where
// libm atan2 is the per-vertex dominator on city's windows.
inline float atan2_approx(float y, float x) {
    constexpr float kPi     = 3.14159265358979323846f;
    constexpr float kHalfPi = 1.57079632679489661923f;
    const float ax = std::fabs(x);
    const float ay = std::fabs(y);
    float r;
    if (ax >= ay) {
        // |y| ≤ |x| → atan(y/x), in (-π/2, π/2). Then adjust for x<0.
        const float a = (ax > 0.0f) ? (y / x) : 0.0f;
        r = atan_approx_unit(a);
        if (x < 0.0f) {
            // x<0: result is in upper / lower half. Add ±π depending
            // on sign of y to get the full atan2 result in (-π, π].
            r += (y >= 0.0f) ? kPi : -kPi;
        }
    } else {
        // |y| > |x| → π/2 − atan(x/y), avoiding the y=0 division.
        const float a = x / y;
        r = (y >= 0.0f ? kHalfPi : -kHalfPi) - atan_approx_unit(a);
    }
    return r;
}

// asin(x) for |x| ≤ 1 via the identity asin(x) = atan(x / sqrt(1−x²)).
// 1/sqrt(1−x²) becomes fast_rsqrt(eps + 1−x²). Adds an epsilon so
// |x|→1 doesn't blow up rsqrt (and the boundary mirrors libm's
// asin(±1) = ±π/2 within polynomial error). For |x|>1 returns the
// boundary value — callers normalize their input direction first.
inline float asin_approx(float x) {
    constexpr float kHalfPi = 1.57079632679489661923f;
    if (x >=  1.0f) return  kHalfPi;
    if (x <= -1.0f) return -kHalfPi;
    const float s    = 1.0f - x * x + 1e-12f;
    const float invR = fast_rsqrt(s);
    return atan_approx(x * invR);
}

#if INSTRSET >= 8
// 8-wide branchless variant. Per lane: 1 abs, 1 cmp, 1 div, 1 mul,
// 5 fmadd (polynomial), 1 fmadd (range-reduce subtract), 2 blendv.
// ~12 vec ops total — vs scalar libm atan at ~30 cycles per call,
// 8× lane speedup ≈ 20× wall-clock improvement at full SIMD width.
inline __m256 atan_approx_x8(__m256 x) {
    const __m256 vSign   = _mm256_set1_ps(-0.0f);
    const __m256 vOne    = _mm256_set1_ps(1.0f);
    const __m256 vHalfPi = _mm256_set1_ps(1.5707963267948966f);
    const __m256 vNegHalfPi = _mm256_set1_ps(-1.5707963267948966f);

    const __m256 absX = _mm256_andnot_ps(vSign, x);
    const __m256 big  = _mm256_cmp_ps(absX, vOne, _CMP_GT_OQ);

    // For |x|>1 lanes: use 1/x instead. rcp_ps is ~3 cycles, 12-bit
    // precision — plenty for atan (~0.001 rad polynomial error already
    // dominates). div_ps would be 10-20 cycles per lane.
    const __m256 invX = _mm256_rcp_ps(x);
    const __m256 t    = _mm256_blendv_ps(x, invX, big);

    // Polynomial: t * (1 + t²·(c1 + t²·(c2 + t²·(c3 + t²·c4))))
    const __m256 t2 = _mm256_mul_ps(t, t);
    __m256 poly = _mm256_set1_ps(0.020835f);
    poly = _mm256_fmadd_ps(poly, t2, _mm256_set1_ps(-0.085133f));
    poly = _mm256_fmadd_ps(poly, t2, _mm256_set1_ps(0.180142f));
    poly = _mm256_fmadd_ps(poly, t2, _mm256_set1_ps(-0.330299f));
    poly = _mm256_fmadd_ps(poly, t2, vOne);
    poly = _mm256_mul_ps(poly, t);

    // For |x|>1 lanes: result = sign(x)·π/2 − poly.
    const __m256 xPositive = _mm256_cmp_ps(x, _mm256_setzero_ps(), _CMP_GE_OQ);
    const __m256 signedHalfPi = _mm256_blendv_ps(vNegHalfPi, vHalfPi, xPositive);
    const __m256 reduced = _mm256_sub_ps(signedHalfPi, poly);
    return _mm256_blendv_ps(poly, reduced, big);
}
#endif

// block-tiling adjustment functions, V2
// Example for 256x256 texture
//    3         2         1         0
//   10987654321098765432109876543210
// U 0000000000000000UUUUUU00000000uu
// V 0000000000000000000000VVVVVVvv00

inline Vec8i packed_tile_v(Vec8i& v, uint32_t vmask) {
	return (v & vmask) << 2;
}

inline uint32_t swizzle_umask(int32_t vbits, uint32_t umask) {
	return (umask >> 2) << (2 + vbits);
}

inline Vec8i packed_tile_u(Vec8i& u, int32_t vbits, uint32_t swizzled_umask) {
	return (u & 3) | ((u << vbits) & swizzled_umask);
}


template <typename T>
struct v8_trait {};

template <>
struct v8_trait<float> {
	using value_type = Vec8f;
	inline static const auto arith_seq_mult = value_type(0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f);
};

template <>
struct v8_trait<int32_t> {
	using value_type = Vec8i;
	inline static const auto arith_seq_mult = value_type(0, 1, 2, 3, 4, 5, 6, 7);
};

template <>
struct v8_trait<uint32_t> {
	using value_type = Vec8ui;
	inline static const auto arith_seq_mult = value_type(0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u);
};

template <typename V>
using v8_type = typename v8_trait<V>::value_type;

inline Vec8i mul_add(Vec8i a, Vec8i b, Vec8i x) {
	return a * b + x;
}

inline Vec32us mul_add(Vec32us a, Vec32us b, Vec32us x) {
	return a * b + x;
}

inline Vec32s mul_add(Vec32s a, Vec32s b, Vec32s x) {
    return a * b + x;
}

template < typename T>
inline v8_type<T> v8_from_arith_seq(T x_, T d_) {
	auto x = v8_type<T>{ x_ };
	auto d = v8_type<T>{ d_ };
	return mul_add(d, v8_trait<T>::arith_seq_mult, x);
}

inline static const auto v32_arith_seq_mult = Vec32us(0, 0, 0, 0,
	1, 1, 1, 1,
	2, 2, 2, 2,
	3, 3, 3, 3,
	4, 4, 4, 4,
	5, 5, 5, 5,
	6, 6, 6, 6,
	7, 7, 7, 7);

inline static const auto v32s_arith_seq_mult = Vec32s(0, 0, 0, 0,
	1, 1, 1, 1,
	2, 2, 2, 2,
	3, 3, 3, 3,
	4, 4, 4, 4,
	5, 5, 5, 5,
	6, 6, 6, 6,
	7, 7, 7, 7);


inline Vec32s Vec32sFromVec4s(std::array<int16_t, 4> x_) {
	return Vec32s{ x_[0], x_[1], x_[2], x_[3],
				   x_[0], x_[1], x_[2], x_[3],
				   x_[0], x_[1], x_[2], x_[3],
				   x_[0], x_[1], x_[2], x_[3],
				   x_[0], x_[1], x_[2], x_[3],
				   x_[0], x_[1], x_[2], x_[3],
				   x_[0], x_[1], x_[2], x_[3],
				   x_[0], x_[1], x_[2], x_[3], };
}

inline Vec32s v32_from_arith_seq(std::array<int16_t, 4> x_, std::array<int16_t, 4> d_) {
	auto x = Vec32sFromVec4s(x_);
	auto d = Vec32sFromVec4s(d_);
	return mul_add(d, v32s_arith_seq_mult, x);
}

inline Vec32s Vec32sFromVec8s(Vec8s x_) {
	return Vec32s{ Vec16s{ x_, x_ }, Vec16s{ x_, x_ } };
}

inline Vec32s v32_from_arith_seq(Vec8s x_, Vec8s d_) {
	auto x = Vec32sFromVec8s(x_);
	auto d = Vec32sFromVec8s(d_);
	return mul_add(d, v32s_arith_seq_mult, x);
}

template <uint8_t Shift = 0>
inline Vec32uc colorize(Vec32uc color1, Vec32us color2) {
	return compress((extend(color1) * (color2 >> Shift)) >> 8);
}

inline Vec8ui gather(const Vec8ui index, void const* table, Vec8ib mask) {
#if INSTRSET >= 8
	return (_mm256_mask_i32gather_epi32(Vec8ui(0), (const int *)table, static_cast<__m256i>(index), *(__m256i *)(&mask)/*static_cast<__m256i>(mask)*/, 4));
#else
	auto t = (const uint32_t*)table;
	uint32_t ind[8];
	index.store(ind);
	uint32_t m[8];
	mask.store(m);

	//return Vec8ui(t[ind[0]], t[ind[1]], t[ind[2]], t[ind[3]],
	//			  t[ind[4]], t[ind[5]], t[ind[6]], t[ind[7]]); // ignore mask

	return Vec8ui(m[0] ? t[ind[0]] : 0, m[1] ? t[ind[1]] : 0, m[2] ? t[ind[2]] : 0, m[3] ? t[ind[3]] : 0,
		m[4] ? t[ind[4]] : 0, m[5] ? t[ind[5]] : 0, m[6] ? t[ind[6]] : 0, m[7] ? t[ind[7]] : 0);
#endif
}

inline Vec8ui gather(const Vec8ui index, void const* table) {
#if INSTRSET >= 8
	return _mm256_i32gather_epi32((const int*)table, static_cast<__m256i>(index), 4);
#else
	auto t = (const uint32_t*)table;
	uint32_t ind[8];
	index.store(ind);

	return Vec8ui(t[ind[0]], t[ind[1]], t[ind[2]], t[ind[3]], t[ind[4]], t[ind[5]], t[ind[6]], t[ind[7]]);
#endif
}

inline Vec8ui m256i_from_arith_seq_tiled(uint32_t x0, uint32_t dx, uint32_t mask) {
	const uint32_t x1 = (x0 + dx) & mask;
	const uint32_t x2 = (x1 + dx) & mask;
	const uint32_t x3 = (x2 + dx) & mask;
	const uint32_t x4 = (x3 + dx) & mask;
	const uint32_t x5 = (x4 + dx) & mask;
	const uint32_t x6 = (x5 + dx) & mask;
	const uint32_t x7 = (x6 + dx) & mask;
	return Vec8ui{ x0, x1, x2, x3, x4, x5, x6, x7 };
}
