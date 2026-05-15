#pragma once

#include <algorithm>
#include <cmath>

#include "Base/FDS_DECS.H"
#include "Base/RenderTarget.h"
#include "Base/CameraContext.h"
#include "F4Vec.h"

#include "TheOtherBarry.h"

//#include <intrin.h>
#include "simde/x86/avx2.h"
#include <simd/vectorclass.h>
#include <cassert>
#include <array>
#include "SimdHelpers.h"

#if defined(__EMSCRIPTEN__)
#include <wasm_simd128.h>
#endif

#include "Base/Scene.h"


namespace barry {
constexpr const int32_t TILE_SIZE = 8;
using TScreenCoord = int32_t;

struct alignas(16) RVector4 {
	// screen space x, y
	// z is z in view space
	// w is 1/z in view space
	float x, y, z, w;

	static RVector4 fromVertex(const Vertex* v) {
		return RVector4{ v->PX, v->PY, 1.0f / v->RZ, v->RZ };
	}
};

struct TileTxtrInfo {
	float uz0, vz0, rz0;
	float r0, g0, b0, a0;
	float uz1, vz1, rz1;
	float uz2, vz2, rz2;
	float uz3, vz3, rz3;
};

struct Tile {
	int x, y;

	TScreenCoord a0, dadx, dady;
	TScreenCoord b0, dbdx, dbdy;
	TScreenCoord c0, dcdx, dcdy;

	float rz0;
	TileTxtrInfo t0;
};

using Triangle = RVector4[3];

constexpr const int8_t SUBPIXEL_BITS = 8;
constexpr const float SUBPIXEL_MULT = 256.0f;

enum class TInterpolationType {
	AFFINE,
	QUADRATIC
};

enum class TBlendMode {
	XOR,
	OVERWRITE,
	TRANSPARENT,
	ADDITIVE,
};

enum class TTextureMode {
	NORMAL,
	TEXTURETEXTURE,
	// Untextured Gouraud / flat fills. The TileRasterizer skips its
	// texture setup (avoids deref'ing a null Texture*) and the per-pixel
	// kernel uses a constant white sample so colorize() reduces to the
	// interpolated vertex color. Restores IX_Prefiller_FZ/GZ semantics
	// without the legacy code path.
	NONE,
};

// Per-tile coverage classification driving apply_exact's fast/slow path
// selection. PARTIAL is the conservative default and runs the per-row
// edge-mask test; FULL is selected when rasterize_triangle has proven (via
// min-edge corner check) that every pixel of the tile is inside the
// triangle, letting apply_exact skip ~9 SIMD ops per row.
enum class TCoverage {
	PARTIAL,
	FULL,
};

inline TScreenCoord orient2d(
	TScreenCoord ax, TScreenCoord ay,
	TScreenCoord bx, TScreenCoord by,
	TScreenCoord cx, TScreenCoord cy)
{
	return (int64_t(bx - ax) * int64_t(cy - ay) - int64_t(by - ay) * int64_t(cx - ax)) >> SUBPIXEL_BITS;
}

// True iff any lane of m has its sign bit set. Used as the per-tile-row
// "is any pixel inside the triangle / passes Z?" gate in the rasterizer.
//
// Native: vectorclass's horizontal_or compiles to !_mm256_testz_si256, which
// is fine on x86/arm64.
//
// Wasm: simde's _mm256_testz_si256 misses some low-lane-only mask cases (we
// observed lane 0 = 0xFFFFFFFF returning 0), silently dropping pixels along
// triangle edges whose 8-pixel SIMD row has only lane 0 inside the triangle.
// _mm256_movemask_epi8 is correct but expensive on wasm (no native 256-bit
// movemask, ~15 simd ops per call), and called per tile-row it dominates
// the rasterizer cost. Workaround: collapse the two 128-bit halves with OR
// and test via _mm_testz_si128 — that simde mapping routes through
// wasm_v128_any_true, which is a single instruction and isn't subject to
// the 256-bit testz bug.
inline bool any_lane_set(Vec8ib m) {
#if defined(__EMSCRIPTEN__)
	// Native wasm SIMD any-bit-set on each 128-bit half then OR — bypasses
	// simde's testz mappings (both 128- and 256-bit versions silently miss
	// some lane patterns and drop edge pixels). _mm256_movemask_epi8 is
	// also correct but expensive (~15 SIMD ops on wasm); this is 3 wasm
	// instructions total and verified against filler@t=0.
	__m256i v = *(const __m256i*)(&m);
	v128_t lo = (v128_t)_mm256_castsi256_si128(v);
	v128_t hi = (v128_t)_mm256_extracti128_si256(v, 1);
	return wasm_v128_any_true(wasm_v128_or(lo, hi));
#else
	return horizontal_or(m);
#endif
}

// block-tiling adjustment functions
// Example for 256x256 texture
//    3         2         1         0
//   10987654321098765432109876543210
// U 0000UUUUUU00000000uu0fffffffffff
// V 0000000000VVVVVVvv000fffffffffff

inline uint32_t tile_vmask(uint32_t vmask) {
	return 0x7ff | (vmask << 14);
}

inline uint32_t tile_v(uint32_t v, uint32_t vmask) {
	return (v & 0x7ff) | ((v << 3) & (vmask << 14));
}

inline uint32_t tile_dv(uint32_t v, uint32_t vmask) {
	return tile_v(v, vmask) | 0x3800;
}

inline uint32_t tile_umask(uint32_t vbits, uint32_t umask) {
	return 0x37ff | ((umask >> 2) << (14 + vbits));
}

inline uint32_t tile_u(uint32_t u, uint32_t vbits, uint32_t umask) {
	return (u & 0x7ff) | ((u & 0x1800) << 1) | ((u << (1 + vbits)) & ((umask >> 2) << (14 + vbits)));
}

inline uint32_t tile_du(uint32_t u, uint32_t vbits, uint32_t umask) {
	return tile_u(u, vbits, umask) | 0x800 | (((1 << vbits) - 1) << 14);
}

// Bench variants — set via -DBENCH_SKIP_*=1 to stub specific operations and
// measure their isolated cost. Default (all 0) is the production rasterizer;
// each #define can be flipped on independently to isolate one op. See the
// `bench-variants-*` Makefile targets.
#ifndef BENCH_SKIP_TEXTURE
#define BENCH_SKIP_TEXTURE 0
#endif
#ifndef BENCH_SKIP_PERSPECTIVE
#define BENCH_SKIP_PERSPECTIVE 0
#endif
#ifndef BENCH_SKIP_MASKSTORE
#define BENCH_SKIP_MASKSTORE 0
#endif
#ifndef BENCH_SKIP_Z
#define BENCH_SKIP_Z 0
#endif
#ifndef BENCH_SKIP_COLOR
#define BENCH_SKIP_COLOR 0
#endif

template <barry::TBlendMode BlendMode, barry::TTextureMode TextureMode>
struct TileRasterizer {
	TileRasterizer(Vertex** V, byte* dstSurface, int32_t bpsl, int32_t xres, int32_t yres,
	               uint16_t* zpage16, float zScale,
	               Texture* Txtr, int miplevel)
		: V(V)
		, dstSurface(dstSurface)
		, bpsl(bpsl)
		, xres(xres)
		, yres(yres)
		, zpage16(zpage16)
		, zScale(zScale) {

		if constexpr (TextureMode != barry::TTextureMode::NONE) {
			t0.LogWidth = Txtr->LSizeX - miplevel;
			t0.LogHeight = Txtr->LSizeY - miplevel;
			t0.TextureAddr = (dword*)Txtr->Mipmap[miplevel];

			t0.UScaleFactor = (1 << t0.LogWidth);
			t0.VScaleFactor = (1 << t0.LogHeight);
		}
	}

	Vertex** V;
	byte* dstSurface;
	int32_t bpsl;
	int32_t xres;
	int32_t yres;
	uint16_t* zpage16;   // was: ZPage16 global
	float zScale;        // was: g_zscale global
	struct TextureInfo {
		const dword* TextureAddr;
		const dword* TextureAddr1;
		int32_t LogWidth;
		int32_t LogHeight;
		float UScaleFactor;
		float VScaleFactor;
		float du0zdx, du0zdy;
		float dv0zdx, dv0zdy;
		float du1zdx, du1zdy;
		float dv1zdx, dv1zdy;
	};
	float drzdx, drzdy;
	float dadx, dady;
	float drdx, drdy;
	float dgdx, dgdy;
	float dbdx, dbdy;

	uint32_t umask;// = (1 << t0.LogWidth) - 1);
	uint32_t vmask;// = (1 << t0.LogHeight) - 1);
	TextureInfo t0;
	//size_t v1 = 0 , v2 = 0, v3 = 0;
	//void setVertexIndexes(size_t v1, size_t v2, size_t v3) {
	//	this->v1 = v1;
	//	this->v2 = v2;
	//	this->v3 = v3;
	//}

	int32_t clampedX(int32_t x) {
		return std::min(std::max(x, 0), xres - 1);
	}
	int32_t clampedY(int32_t y) {
		return std::min(std::max(y, 0), yres - 1);
	}

	inline int16_t FixedPoint(float f) {
		return int16_t(f);
	}

	// Coverage::FULL is the inside-tile fast path: rasterize_triangle has
	// proven (via min-edge-function) that every pixel of this tile is inside
	// the triangle. Drop the per-row edge-mask construction (3 SIMD adds
	// + 3 ORs + 1 compare) and the outer any-lane-set check; pixel writes are
	// gated only by Z-test. ~9 SIMD ops/row × 8 rows = 72 ops saved per tile.
	template <barry::TCoverage Coverage = barry::TCoverage::PARTIAL>
	void apply_exact(const barry::Tile& tile) {
		auto scanline = dstSurface + tile.y * TILE_SIZE * bpsl;
		// Z-buffer lives in its own allocation now (was: ZPage16 global,
		// passed through from the per-pass RenderTarget). Color stride
		// `bpsl` is in bytes; Z stride is `xres` u16 elements per row.
		auto zspan = zpage16 + tile.y * TILE_SIZE * xres + tile.x * TILE_SIZE;
		auto span = ((uint32_t*)scanline) + tile.x * TILE_SIZE;
		auto bpsl_u32 = bpsl / sizeof(uint32_t);

		TScreenCoord a0 = tile.a0;
		TScreenCoord b0 = tile.b0;
		TScreenCoord c0 = tile.c0;

		// Edge gradients only consumed by the partial-coverage path.
		Vec8i p_a, p_b, p_c;
		if constexpr (Coverage == barry::TCoverage::PARTIAL) {
			p_a = v8_from_arith_seq(a0, tile.dadx);
			p_b = v8_from_arith_seq(b0, tile.dbdx);
			p_c = v8_from_arith_seq(c0, tile.dcdx);
		}

		int32_t t0_umask = (1 << t0.LogWidth) - 1;
		int32_t t0_vmask = (1 << t0.LogHeight) - 1;
		int32_t t0_umask_swizzled = swizzle_umask(t0.LogHeight, t0_umask);

		int32_t t1_umask = (1 << 10) - 1;
		int32_t t1_vmask = (1 << 10) - 1;
		int32_t t1_umask_swizzled = swizzle_umask(10, t1_umask);

		Vec8f p_rz = v8_from_arith_seq(tile.rz0, drzdx);
		Vec8f p_uz = v8_from_arith_seq(tile.t0.uz0, t0.du0zdx);
		Vec8f p_vz = v8_from_arith_seq(tile.t0.vz0, t0.dv0zdx);

		Vec8f p_u1z;
		Vec8f p_v1z;
		if constexpr (TextureMode == barry::TTextureMode::TEXTURETEXTURE) { 
			p_u1z = v8_from_arith_seq(tile.t0.uz1, t0.du1zdx); 
			p_v1z = v8_from_arith_seq(tile.t0.vz1, t0.dv1zdx);
		}


		// we need two 256 registers to handle coloring 8 pixels
		// 256 / 16 bit per channel / 4 channels = 4 pixels
		// 0..15	16..31	32..47	48..63	64..79	80..95	96..111	112..127	128..143	144..159	160..175	176..191	192..207	208..223	224..239	240..255
		// r		g		b		a		r		g		b		a			r			g			b			a			r			g			b			a

		auto color = v32_from_arith_seq(
			{ FixedPoint(tile.t0.r0), FixedPoint(tile.t0.g0), FixedPoint(tile.t0.b0), FixedPoint(tile.t0.a0) },
			{ FixedPoint(drdx),	   FixedPoint(dgdx),		FixedPoint(dbdx),		 FixedPoint(dadx) });

		//Vec16s rg
		for (int32_t y = 0; y != TILE_SIZE; ++y, a0 += tile.dady, b0 += tile.dbdy, c0 += tile.dcdy, span += bpsl_u32, zspan += xres) {
			Vec8ib p_mask;
			bool row_has_pixels;
			if constexpr (Coverage == barry::TCoverage::FULL) {
				// All 8 lanes are inside the triangle by precondition.
				p_mask = Vec8i(0) == Vec8i(0);  // splat-true mask
				row_has_pixels = true;
			} else {
				p_mask = (p_a | p_b | p_c) >= 0;
				// horizontal_or(Vec8ib) compiles to !_mm256_testz_si256(a,a).
				// simde's wasm impl of testz misses some cases where only the
				// low lane has bits set (we observed lane 0 = 0xFFFFFFFF +
				// horizontal_or returning 0), which silently drops every pixel
				// on a triangle edge whose 8-pixel SIMD row has only lane 0
				// inside the triangle. _mm256_movemask_epi8 routes through a
				// different simde primitive that handles this correctly on
				// every target.
				row_has_pixels = any_lane_set(p_mask);
			}
			if (row_has_pixels) {
				Vec8f p_z = approx_recipr(p_rz);

#if BENCH_SKIP_Z
				// Stub: skip the Z-test chain (z_candidate compute, Z load,
				// extend/compare, mask blend, zspan store). p_z is still
				// computed above because the perspective UV mul needs it
				// (unless BENCH_SKIP_PERSPECTIVE is also on). Pixel block
				// still runs so we keep gather/maskstore in the timing.
				if (any_lane_set(p_mask)) {
#else
				auto z_candidate = (Vec8ui(0xFF80) - static_cast<Vec8ui>(roundi(zScale * p_z)));
				Vec8us z_existing_c;
				z_existing_c.load_a(zspan);
				auto z_existing = extend(z_existing_c);

				auto zmask = z_candidate > z_existing;

				p_mask &= zmask;

				if (any_lane_set(p_mask)) {

					// Always write Z, including for transparent + additive.
					// Forward path's pre-deferred behavior wrote Z for all
					// blend modes; matching this so the fountain vortex
					// (additive, SortPriorityBias=DrawFirst) writes Z and
					// is correctly occluded by closer surfaces drawn after.
					*(__m128i*)zspan = _mm_blendv_epi8(*(__m128i*)zspan, compress(z_candidate), compress(Vec8ui(p_mask)));
#endif

					auto blend_color = Vec32us(color);

					Vec8ui texture0_samples;
					if constexpr (TextureMode == barry::TTextureMode::NONE) {
						// Untextured: feed colorize() a white sample so the
						// only signal that survives is the interpolated
						// vertex color (Gouraud or flat).
						texture0_samples = Vec8ui(0xFFFFFFFF);
					} else {
#if BENCH_SKIP_PERSPECTIVE
						// Stub: linear UV (no `* p_z` perspective divide). Wrong UVs,
						// useful only for isolating the perspective compute cost.
						Vec8i u = roundi(p_uz * t0.UScaleFactor);
						Vec8i v = roundi(p_vz * t0.VScaleFactor);
#else
						Vec8i u = roundi(p_uz * p_z * t0.UScaleFactor);
						Vec8i v = roundi(p_vz * p_z * t0.VScaleFactor);
#endif

						Vec8i tu = packed_tile_u(u, t0.LogHeight, t0_umask_swizzled);
						Vec8i tv = packed_tile_v(v, t0_vmask);

						auto p_offset = tu + tv;

#if BENCH_SKIP_TEXTURE
						// Stub: constant white instead of texture gather. Isolates
						// gather/cache cost.
						texture0_samples = Vec8ui(0xFFFFFFFF);
#else
						texture0_samples = gather(Vec8ui(p_offset), t0.TextureAddr, p_mask);
#endif
					}
					if constexpr (TextureMode == barry::TTextureMode::TEXTURETEXTURE) {
						Vec8i u1 = roundi(p_u1z * p_z * 1024.0f);
						Vec8i v1 = roundi(p_v1z * p_z * 1024.0f);

						Vec8i tu1 = packed_tile_u(u1, 10, t1_umask_swizzled);
						Vec8i tv1 = packed_tile_v(v1, t1_vmask);

						auto p_offset1 = tu1 + tv1;
						auto texture1_samples = gather(Vec8ui(p_offset1), t0.TextureAddr1, p_mask);
						texture0_samples = Vec8ui(add_saturated(Vec32uc(texture1_samples), Vec32uc(texture0_samples) >> 1));
					}

#if BENCH_SKIP_COLOR
					// Stub: skip the gouraud color multiply blend. Texture
					// gather feeds the maskstore unmodified — isolates the
					// per-lane colorize cost.
					auto texture_samples = Vec32uc(texture0_samples);
#else
					auto texture_samples = colorize(Vec32uc(texture0_samples), blend_color);
#endif

					if constexpr (BlendMode == TBlendMode::TRANSPARENT) {
						Vec32uc dst;
						dst.load_a(span);
						texture_samples = add_saturated(texture_samples, dst >> 1);
					}

					if constexpr (BlendMode == TBlendMode::ADDITIVE) {
						Vec32uc dst;
						dst.load_a(span);
						texture_samples = add_saturated(texture_samples, dst);
					}


#if BENCH_SKIP_MASKSTORE
					// Stub: unmasked 256-bit store. Writes pixels outside the
					// triangle (visually wrong) but isolates maskstore cost.
					_mm256_storeu_ps((float*)span, *(__m256*)(&texture_samples));
#else
					_mm256_maskstore_ps((float*)span, *(__m256i*)(&p_mask), *(__m256*)(&texture_samples));
#endif
				}
			}

			p_rz += Vec8f(drzdy);
			p_uz += Vec8f(t0.du0zdy);
			p_vz += Vec8f(t0.dv0zdy);
			if constexpr (TextureMode == barry::TTextureMode::TEXTURETEXTURE) {
				p_u1z += Vec8f(t0.du1zdy);
				p_v1z += Vec8f(t0.dv1zdy);
			}
			color += Vec32sFromVec4s({ FixedPoint(drdy), FixedPoint(dgdy), FixedPoint(dbdy), FixedPoint(dady) });

			if constexpr (Coverage == barry::TCoverage::PARTIAL) {
				p_a += Vec8i(tile.dady);
				p_b += Vec8i(tile.dbdy);
				p_c += Vec8i(tile.dcdy);
			}
		}
	}

	
	void rasterize_triangle(const Vertex& v1, const Vertex& v2, const Vertex& v3) {
		// FIXME: raster conventions (it is doing floor right now)
		const int tile_mx = clampedX(std::min({ v1.PX, v2.PX, v3.PX })) / TILE_SIZE;
		const int tile_Mx = clampedX(std::max({ v1.PX, v2.PX, v3.PX })) / TILE_SIZE;
		const int tile_my = clampedY(std::min({ v1.PY, v2.PY, v3.PY })) / TILE_SIZE;
		const int tile_My = clampedY(std::max({ v1.PY, v2.PY, v3.PY })) / TILE_SIZE;

		// std::lroundf does round-half-away-from-zero in software, ignoring
		// the FPU rounding mode. The float `v.PX * SUBPIXEL_MULT` step is
		// still subject to FPCR on native (ROUND_UP) vs RTNE on wasm, but
		// doing the *round* in lroundf instead of `int(x + 0.5)` makes the
		// conversion stable even when the multiplication lands exactly on a
		// half-integer.
		TScreenCoord v1x = TScreenCoord(std::lroundf(v1.PX * SUBPIXEL_MULT));
		TScreenCoord v1y = TScreenCoord(std::lroundf(v1.PY * SUBPIXEL_MULT));
		TScreenCoord v2x = TScreenCoord(std::lroundf(v2.PX * SUBPIXEL_MULT));
		TScreenCoord v2y = TScreenCoord(std::lroundf(v2.PY * SUBPIXEL_MULT));
		TScreenCoord v3x = TScreenCoord(std::lroundf(v3.PX * SUBPIXEL_MULT));
		TScreenCoord v3y = TScreenCoord(std::lroundf(v3.PY * SUBPIXEL_MULT));

		TScreenCoord x0 = tile_mx * TILE_SIZE << SUBPIXEL_BITS;
		TScreenCoord y0 = tile_my * TILE_SIZE << SUBPIXEL_BITS;
		TScreenCoord _a0 = orient2d(v2x, v2y, v1x, v1y, x0, y0);
		TScreenCoord _b0 = orient2d(v3x, v3y, v2x, v2y, x0, y0);
		TScreenCoord _c0 = orient2d(v1x, v1y, v3x, v3y, x0, y0);

		TScreenCoord dadx = (v2y - v1y);
		TScreenCoord dady = (v1x - v2x);
		TScreenCoord dbdx = (v3y - v2y);
		TScreenCoord dbdy = (v2x - v3x);
		TScreenCoord dcdx = (v1y - v3y);
		TScreenCoord dcdy = (v3x - v1x);

		// flat without tiling
		//for (int y = tile_my * TILE_SIZE; y <= tile_My * TILE_SIZE + TILE_SIZE - 1; ++y) {
		//	byte* scanline = dstSurface + y * bpsl;
		//	for (int x = tile_mx * TILE_SIZE; x <= tile_Mx * TILE_SIZE + TILE_SIZE - 1; ++x) {
		//		TScreenCoord alpha = orient2d(v2x, v2y, v1x, v1y, x << SUBPIXEL_BITS, y << SUBPIXEL_BITS);
		//		TScreenCoord beta = orient2d(v3x, v3y, v2x, v2y, x << SUBPIXEL_BITS, y << SUBPIXEL_BITS);
		//		TScreenCoord gamma = orient2d(v1x, v1y, v3x, v3y, x << SUBPIXEL_BITS, y << SUBPIXEL_BITS);
		//		uint32_t& pixel = ((uint32_t*)scanline)[x];
		//		if (alpha >= 0 && beta >= 0 && gamma >= 0) {
		//			pixel = 0xcdefab;
		//		} /*else if (pixel == 0) {
		//			pixel = 0x123456;
		//		}*/
		//	}
		//}
	//		/*
			// this is constant across entire triangle
		int i = 0;
		float zoltek = 1.0f / (_a0 + _b0 + _c0);

		// Hierarchical traversal gating: only triangles spanning ≥2
		// super-tiles (4×TILE_SIZE = 32 px each) hit the outer super-tile
		// loop. Smaller triangles take the direct per-tile loop below to
		// avoid the super-tile setup overhead (3 orient2d calls + a
		// classify-or-skip per super-tile that has nothing to skip when
		// the bbox is contained).
		constexpr int SUPER = 4;
		constexpr int SUPER_PIXELS = SUPER * TILE_SIZE;
		const int super_mx = tile_mx / SUPER;
		const int super_Mx = tile_Mx / SUPER;
		const int super_my = tile_my / SUPER;
		const int super_My = tile_My / SUPER;
		const bool spans_multi_super = (super_mx != super_Mx) || (super_my != super_My);

		if (!spans_multi_super) {
		for (int y = tile_my; y <= tile_My; ++y, _a0 += TILE_SIZE * dady, _b0 += TILE_SIZE * dbdy, _c0 += TILE_SIZE * dcdy, ++i) {
			TScreenCoord a0 = _a0;
			TScreenCoord b0 = _b0;
			TScreenCoord c0 = _c0;
			for (int x = tile_mx; x <= tile_Mx; ++x, a0 += TILE_SIZE * dadx, b0 += TILE_SIZE * dbdx, c0 += TILE_SIZE * dcdx, ++i) {
				TScreenCoord max_a = a0 + ((dadx > 0) ? dadx * TILE_SIZE : 0) + ((dady > 0) ? dady * TILE_SIZE : 0);
				TScreenCoord max_b = b0 + ((dbdx > 0) ? dbdx * TILE_SIZE : 0) + ((dbdy > 0) ? dbdy * TILE_SIZE : 0);
				TScreenCoord max_c = c0 + ((dcdx > 0) ? dcdx * TILE_SIZE : 0) + ((dcdy > 0) ? dcdy * TILE_SIZE : 0);

				if ((max_a | max_b | max_c) >= 0) {
					// Inside-tile fast-path detection: edge function reaches its
					// minimum at the corner where each component's gradient is
					// negative (mirror of the max-corner logic above). If the
					// minimum is non-negative on all three edges, every pixel
					// of the tile is inside the triangle and we can skip per-row
					// edge-mask construction in apply_exact.
					TScreenCoord min_a = a0 + ((dadx < 0) ? dadx * TILE_SIZE : 0) + ((dady < 0) ? dady * TILE_SIZE : 0);
					TScreenCoord min_b = b0 + ((dbdx < 0) ? dbdx * TILE_SIZE : 0) + ((dbdy < 0) ? dbdy * TILE_SIZE : 0);
					TScreenCoord min_c = c0 + ((dcdx < 0) ? dcdx * TILE_SIZE : 0) + ((dcdy < 0) ? dcdy * TILE_SIZE : 0);
					const bool full_cover = (min_a >= 0) && (min_b >= 0) && (min_c >= 0);

					// FIXME: define outside and maintain
					Tile tile = {
						.x = x,
						.y = y,
						.a0 = a0,
						.dadx = dadx,
						.dady = dady,
						.b0 = b0,
						.dbdx = dbdx,
						.dbdy = dbdy,
						.c0 = c0,
						.dcdx = dcdx,
						.dcdy = dcdy,
						.rz0 = (v1.RZ + (x * TILE_SIZE - v1.PX) * drzdx + (y * TILE_SIZE - v1.PY) * drzdy),
						//.rz0 = (v1.RZ * b0 + v2.RZ * c0 + v3.RZ * a0) * zoltek,
						.t0 = {
							.uz0 = (v1.UZ + (x * TILE_SIZE - v1.PX) * t0.du0zdx + (y * TILE_SIZE - v1.PY) * t0.du0zdy),
							.vz0 = (v1.VZ + (x * TILE_SIZE - v1.PX) * t0.dv0zdx + (y * TILE_SIZE - v1.PY) * t0.dv0zdy),
							//.uz0 = (v1.UZ * b0 + v2.UZ * c0 + v3.UZ * a0) * zoltek,
							//.vz0 = (v1.VZ * b0 + v2.VZ * c0 + v3.VZ * a0) * zoltek,
							.r0 = (float(v1.LR) + float(x * TILE_SIZE - v1.PX) * this->drdx + float(y * TILE_SIZE - v1.PY) * this->drdy),
							.g0 = (float(v1.LG) + float(x * TILE_SIZE - v1.PX) * this->dgdx + float(y * TILE_SIZE - v1.PY) * this->dgdy),
							.b0 = (float(v1.LB) + float(x * TILE_SIZE - v1.PX) * this->dbdx + float(y * TILE_SIZE - v1.PY) * this->dbdy),
							.a0 = (float(v1.LA) + float(x * TILE_SIZE - v1.PX) * this->dadx + float(y * TILE_SIZE - v1.PY) * this->dady),
						}
					};

					if constexpr (TextureMode == barry::TTextureMode::TEXTURETEXTURE) {
						tile.t0.uz1 = (v1.EUZ + (x * TILE_SIZE - v1.PX) * t0.du1zdx + (y * TILE_SIZE - v1.PY) * t0.du1zdy);
						tile.t0.vz1 = (v1.EVZ + (x * TILE_SIZE - v1.PX) * t0.dv1zdx + (y * TILE_SIZE - v1.PY) * t0.dv1zdy);
					}

					if (full_cover) {
						apply_exact<barry::TCoverage::FULL>(tile);
					} else {
						apply_exact<barry::TCoverage::PARTIAL>(tile);
					}
				}
			}
		}
		return;
		}  // !spans_multi_super

		// Hierarchical (large-triangle) path.
		TScreenCoord ssx0 = super_mx * SUPER_PIXELS << SUBPIXEL_BITS;
		TScreenCoord ssy0 = super_my * SUPER_PIXELS << SUBPIXEL_BITS;
		TScreenCoord _sa0 = orient2d(v2x, v2y, v1x, v1y, ssx0, ssy0);
		TScreenCoord _sb0 = orient2d(v3x, v3y, v2x, v2y, ssx0, ssy0);
		TScreenCoord _sc0 = orient2d(v1x, v1y, v3x, v3y, ssx0, ssy0);

		for (int sy = super_my; sy <= super_My; ++sy,
				_sa0 += SUPER_PIXELS * dady, _sb0 += SUPER_PIXELS * dbdy, _sc0 += SUPER_PIXELS * dcdy) {
			TScreenCoord sa0 = _sa0, sb0 = _sb0, sc0 = _sc0;
			for (int sx = super_mx; sx <= super_Mx; ++sx,
					sa0 += SUPER_PIXELS * dadx, sb0 += SUPER_PIXELS * dbdx, sc0 += SUPER_PIXELS * dcdx) {
				TScreenCoord smax_a = sa0 + ((dadx > 0) ? dadx * SUPER_PIXELS : 0) + ((dady > 0) ? dady * SUPER_PIXELS : 0);
				TScreenCoord smax_b = sb0 + ((dbdx > 0) ? dbdx * SUPER_PIXELS : 0) + ((dbdy > 0) ? dbdy * SUPER_PIXELS : 0);
				TScreenCoord smax_c = sc0 + ((dcdx > 0) ? dcdx * SUPER_PIXELS : 0) + ((dcdy > 0) ? dcdy * SUPER_PIXELS : 0);
				if ((smax_a | smax_b | smax_c) < 0) continue;

				TScreenCoord smin_a = sa0 + ((dadx < 0) ? dadx * SUPER_PIXELS : 0) + ((dady < 0) ? dady * SUPER_PIXELS : 0);
				TScreenCoord smin_b = sb0 + ((dbdx < 0) ? dbdx * SUPER_PIXELS : 0) + ((dbdy < 0) ? dbdy * SUPER_PIXELS : 0);
				TScreenCoord smin_c = sc0 + ((dcdx < 0) ? dcdx * SUPER_PIXELS : 0) + ((dcdy < 0) ? dcdy * SUPER_PIXELS : 0);
				const bool super_full = (smin_a >= 0) && (smin_b >= 0) && (smin_c >= 0);

				const int ty_start = std::max(sy * SUPER, tile_my);
				const int ty_end   = std::min(sy * SUPER + SUPER - 1, tile_My);
				const int tx_start = std::max(sx * SUPER, tile_mx);
				const int tx_end   = std::min(sx * SUPER + SUPER - 1, tile_Mx);

				if (super_full) {
					// Coarse-acceptance fast path (Phase 3): every tile in this
					// super-tile is fully inside all three edges by precondition,
					// so per-tile max/min-corner edge tests would all pass.
					// Skip them and dispatch apply_exact<FULL> directly. Also
					// skip a0/b0/c0 stepping — apply_exact<FULL> doesn't read
					// Tile.a0/b0/c0 (the per-row p_mask path is dead code there).
					for (int y = ty_start; y <= ty_end; ++y, ++i) {
						for (int x = tx_start; x <= tx_end; ++x, ++i) {
							Tile tile = {
								.x = x,
								.y = y,
								.a0 = 0,
								.dadx = dadx,
								.dady = dady,
								.b0 = 0,
								.dbdx = dbdx,
								.dbdy = dbdy,
								.c0 = 0,
								.dcdx = dcdx,
								.dcdy = dcdy,
								.rz0 = (v1.RZ + (x * TILE_SIZE - v1.PX) * drzdx + (y * TILE_SIZE - v1.PY) * drzdy),
								.t0 = {
									.uz0 = (v1.UZ + (x * TILE_SIZE - v1.PX) * t0.du0zdx + (y * TILE_SIZE - v1.PY) * t0.du0zdy),
									.vz0 = (v1.VZ + (x * TILE_SIZE - v1.PX) * t0.dv0zdx + (y * TILE_SIZE - v1.PY) * t0.dv0zdy),
									.r0 = (float(v1.LR) + float(x * TILE_SIZE - v1.PX) * this->drdx + float(y * TILE_SIZE - v1.PY) * this->drdy),
									.g0 = (float(v1.LG) + float(x * TILE_SIZE - v1.PX) * this->dgdx + float(y * TILE_SIZE - v1.PY) * this->dgdy),
									.b0 = (float(v1.LB) + float(x * TILE_SIZE - v1.PX) * this->dbdx + float(y * TILE_SIZE - v1.PY) * this->dbdy),
									.a0 = (float(v1.LA) + float(x * TILE_SIZE - v1.PX) * this->dadx + float(y * TILE_SIZE - v1.PY) * this->dady),
								}
							};

							if constexpr (TextureMode == barry::TTextureMode::TEXTURETEXTURE) {
								tile.t0.uz1 = (v1.EUZ + (x * TILE_SIZE - v1.PX) * t0.du1zdx + (y * TILE_SIZE - v1.PY) * t0.du1zdy);
								tile.t0.vz1 = (v1.EVZ + (x * TILE_SIZE - v1.PX) * t0.dv1zdx + (y * TILE_SIZE - v1.PY) * t0.dv1zdy);
							}

							apply_exact<barry::TCoverage::FULL>(tile);
						}
					}
					continue;
				}

				TScreenCoord _ta0 = sa0 + (tx_start - sx * SUPER) * TILE_SIZE * dadx
				                       + (ty_start - sy * SUPER) * TILE_SIZE * dady;
				TScreenCoord _tb0 = sb0 + (tx_start - sx * SUPER) * TILE_SIZE * dbdx
				                       + (ty_start - sy * SUPER) * TILE_SIZE * dbdy;
				TScreenCoord _tc0 = sc0 + (tx_start - sx * SUPER) * TILE_SIZE * dcdx
				                       + (ty_start - sy * SUPER) * TILE_SIZE * dcdy;

				for (int y = ty_start; y <= ty_end; ++y, _ta0 += TILE_SIZE * dady, _tb0 += TILE_SIZE * dbdy, _tc0 += TILE_SIZE * dcdy, ++i) {
					TScreenCoord a0 = _ta0;
					TScreenCoord b0 = _tb0;
					TScreenCoord c0 = _tc0;
					for (int x = tx_start; x <= tx_end; ++x, a0 += TILE_SIZE * dadx, b0 += TILE_SIZE * dbdx, c0 += TILE_SIZE * dcdx, ++i) {
						TScreenCoord max_a = a0 + ((dadx > 0) ? dadx * TILE_SIZE : 0) + ((dady > 0) ? dady * TILE_SIZE : 0);
						TScreenCoord max_b = b0 + ((dbdx > 0) ? dbdx * TILE_SIZE : 0) + ((dbdy > 0) ? dbdy * TILE_SIZE : 0);
						TScreenCoord max_c = c0 + ((dcdx > 0) ? dcdx * TILE_SIZE : 0) + ((dcdy > 0) ? dcdy * TILE_SIZE : 0);

						if ((max_a | max_b | max_c) >= 0) {
							TScreenCoord min_a = a0 + ((dadx < 0) ? dadx * TILE_SIZE : 0) + ((dady < 0) ? dady * TILE_SIZE : 0);
							TScreenCoord min_b = b0 + ((dbdx < 0) ? dbdx * TILE_SIZE : 0) + ((dbdy < 0) ? dbdy * TILE_SIZE : 0);
							TScreenCoord min_c = c0 + ((dcdx < 0) ? dcdx * TILE_SIZE : 0) + ((dcdy < 0) ? dcdy * TILE_SIZE : 0);
							const bool full_cover = (min_a >= 0) && (min_b >= 0) && (min_c >= 0);

							Tile tile = {
								.x = x,
								.y = y,
								.a0 = a0,
								.dadx = dadx,
								.dady = dady,
								.b0 = b0,
								.dbdx = dbdx,
								.dbdy = dbdy,
								.c0 = c0,
								.dcdx = dcdx,
								.dcdy = dcdy,
								.rz0 = (v1.RZ + (x * TILE_SIZE - v1.PX) * drzdx + (y * TILE_SIZE - v1.PY) * drzdy),
								.t0 = {
									.uz0 = (v1.UZ + (x * TILE_SIZE - v1.PX) * t0.du0zdx + (y * TILE_SIZE - v1.PY) * t0.du0zdy),
									.vz0 = (v1.VZ + (x * TILE_SIZE - v1.PX) * t0.dv0zdx + (y * TILE_SIZE - v1.PY) * t0.dv0zdy),
									.r0 = (float(v1.LR) + float(x * TILE_SIZE - v1.PX) * this->drdx + float(y * TILE_SIZE - v1.PY) * this->drdy),
									.g0 = (float(v1.LG) + float(x * TILE_SIZE - v1.PX) * this->dgdx + float(y * TILE_SIZE - v1.PY) * this->dgdy),
									.b0 = (float(v1.LB) + float(x * TILE_SIZE - v1.PX) * this->dbdx + float(y * TILE_SIZE - v1.PY) * this->dbdy),
									.a0 = (float(v1.LA) + float(x * TILE_SIZE - v1.PX) * this->dadx + float(y * TILE_SIZE - v1.PY) * this->dady),
								}
							};

							if constexpr (TextureMode == barry::TTextureMode::TEXTURETEXTURE) {
								tile.t0.uz1 = (v1.EUZ + (x * TILE_SIZE - v1.PX) * t0.du1zdx + (y * TILE_SIZE - v1.PY) * t0.du1zdy);
								tile.t0.vz1 = (v1.EVZ + (x * TILE_SIZE - v1.PX) * t0.dv1zdx + (y * TILE_SIZE - v1.PY) * t0.dv1zdy);
							}

							if (full_cover) {
								apply_exact<barry::TCoverage::FULL>(tile);
							} else {
								apply_exact<barry::TCoverage::PARTIAL>(tile);
							}
						}
					}
				}
			}
		}
	}

};

} // namespace barry

template <barry::TBlendMode BlendMode, barry::TTextureMode TextureMode = barry::TTextureMode::NORMAL>
void TheOtherBarry(Face* F, Vertex** V, dword numVerts, dword miplevel,
                   const fds::RenderTarget& rt,
                   const fds::CameraContext& cam) {
	//for (dword i = 0; i < numVerts; ++i) {
	//	float z = 1.0f / V[i]->RZ;
	//	V[i]->U = V[i]->UZ * z;
	//	V[i]->V = V[i]->VZ * z;
	//}
	barry::TileRasterizer<BlendMode, TextureMode> r(V,
	                                                 reinterpret_cast<byte*>(rt.vpage),
	                                                 rt.bytesPerScanline,
	                                                 rt.xres, rt.yres,
	                                                 rt.zpage16, cam.zScale,
	                                                 F->Txtr->Txtr, miplevel);

	if constexpr (TextureMode == barry::TTextureMode::TEXTURETEXTURE) {
		r.t0.TextureAddr1 = (dword*)F->ReflectionTexture->Data;
	}

	Vertex vc[12];

	for (dword i = 0; i < numVerts; ++i) {
		vc[i] = *V[i];

		// Scene fog flag is global — CurScene is set by the orchestrator
		// and is per-frame, not per-pass. Leave as global read; phase 4
		// only migrates render-target + camera context.
		if (CurScene->Flags & Scn_Fogged)
		{
			float fogRate;
			fogRate = sqrtf(1.0 - cam.invFarZ * V[i]->TPos.z);
			if (fogRate < 0.0)
			{
				fogRate = 0.0;
			}
			auto r = std::max(vc[i].LR * fogRate, 10.0f);
			auto g = std::max(vc[i].LG * fogRate, 10.0f);
			auto b = std::max(vc[i].LB * fogRate, 10.0f);

			r = std::min(r, 253.0f);
			g = std::min(g, 253.0f);
			b = std::min(b, 253.0f);

			vc[i].LR = r;
			vc[i].LG = g;
			vc[i].LB = b;
		}
	}


	for (dword i = 2; i < numVerts; ++i) {
		//r.setVertexIndexes(0, i - 1, i);

		const auto& v1 = (vc[0]);
		const auto& v2 = (vc[i - 1]);
		const auto& v3 = (vc[i]);

		float m[4] = {
			v2.PX - v1.PX, v2.PY - v1.PY,
			v3.PX - v1.PX, v3.PY - v1.PY
		};
		const float det = m[0] * m[3] - m[1] * m[2];
		if (fabs(det) <= 0.01f) continue;
		const float im[4] = {
			 m[3] / det, -m[1] / det,
			-m[2] / det,  m[0] / det
		};
		r.drzdx = im[0] * (v2.RZ - v1.RZ) + im[1] * (v3.RZ - v1.RZ);
		r.drzdy = im[2] * (v2.RZ - v1.RZ) + im[3] * (v3.RZ - v1.RZ);
		r.t0.du0zdx = im[0] * (v2.UZ - v1.UZ) + im[1] * (v3.UZ - v1.UZ);
		r.t0.du0zdy = im[2] * (v2.UZ - v1.UZ) + im[3] * (v3.UZ - v1.UZ);
		r.t0.dv0zdx = im[0] * (v2.VZ - v1.VZ) + im[1] * (v3.VZ - v1.VZ);
		r.t0.dv0zdy = im[2] * (v2.VZ - v1.VZ) + im[3] * (v3.VZ - v1.VZ);

		if constexpr (TextureMode == barry::TTextureMode::TEXTURETEXTURE) {
			r.t0.du1zdx = im[0] * (v2.EUZ - v1.EUZ) + im[1] * (v3.EUZ - v1.EUZ);
			r.t0.du1zdy = im[2] * (v2.EUZ - v1.EUZ) + im[3] * (v3.EUZ - v1.EUZ);
			r.t0.dv1zdx = im[0] * (v2.EVZ - v1.EVZ) + im[1] * (v3.EVZ - v1.EVZ);
			r.t0.dv1zdy = im[2] * (v2.EVZ - v1.EVZ) + im[3] * (v3.EVZ - v1.EVZ);
		}

		r.dadx = (im[0] * (float(v2.LA) - float(v1.LA)) + im[1] * (float(v3.LA) - float(v1.LA)));
		r.dady = (im[2] * (float(v2.LA) - float(v1.LA)) + im[3] * (float(v3.LA) - float(v1.LA)));
		r.drdx = (im[0] * (float(v2.LR) - float(v1.LR)) + im[1] * (float(v3.LR) - float(v1.LR)));
		r.drdy = (im[2] * (float(v2.LR) - float(v1.LR)) + im[3] * (float(v3.LR) - float(v1.LR)));
		r.dgdx = (im[0] * (float(v2.LG) - float(v1.LG)) + im[1] * (float(v3.LG) - float(v1.LG)));
		r.dgdy = (im[2] * (float(v2.LG) - float(v1.LG)) + im[3] * (float(v3.LG) - float(v1.LG)));
		r.dbdx = (im[0] * (float(v2.LB) - float(v1.LB)) + im[1] * (float(v3.LB) - float(v1.LB)));
		r.dbdy = (im[2] * (float(v2.LB) - float(v1.LB)) + im[3] * (float(v3.LB) - float(v1.LB)));
		r.umask = (1 << r.t0.LogWidth) - 1;
		r.vmask = (1 << r.t0.LogHeight) - 1;

		r.rasterize_triangle(v1, v2, v3);
	}
}
