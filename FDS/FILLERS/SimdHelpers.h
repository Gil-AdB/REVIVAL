#pragma once
#include <simd/vectorclass.h>
#include <array>

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
