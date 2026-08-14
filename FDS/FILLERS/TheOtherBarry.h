#pragma once

#include <algorithm>
#include <cmath>

#include "Base/FDS_DECS.H"
#include "Base/RenderTarget.h"
#include "Base/CameraContext.h"
#include "RENDER/Hdr.h"  // HDR overlay reorg — ADDITIVE bolt accumulates into g_hdrBuf
#include "RENDER/EnvBake.h"  // --env_live_water: g_envLiveWater mask remap (per-face bind)
#include "F4Vec.h"
#include "ClipperTileRect.h"

#include "TheOtherBarry.h"

// Forward declaration — defined in Mekalele.cpp. Returns the txtr
// (mat32) plane pointer of `gb`, or nullptr if `gb` is null. Used by
// TheOtherBarry's outer entry to stamp the deferred-lighting sentinel
// without dragging the full meka::GBuffer definition into this header
// (which would create a circular include with Mekalele.h).
namespace meka {
	struct GBuffer;
	uint32_t* gbuffer_mat32_plane(GBuffer* gb);
}

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
	// Real per-pixel source-alpha blend: src·α + dst·(1−α), α = the texture's
	// authored alpha byte. Unlike TRANSPARENT (a fixed src + dst/2) this honours
	// per-pixel coverage — the primitive for soft-edged decals / partial-alpha
	// sprites. No consumer yet; instantiated below so the path is compiled.
	SRC_ALPHA,
};

enum class TTextureMode {
	NORMAL,
	// NORMAL with a 4-tap bilinear primary-texture sample instead of nearest.
	// Same texture setup as NORMAL (it's !=NONE, !=TEXTURETEXTURE everywhere);
	// only the per-pixel sample differs. Used by the fountain bolt glow, which
	// is magnified hugely — nearest stair-steps, bilinear stays smooth.
	NORMAL_BILINEAR,
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

// True iff every lane of m has its sign bit set. Used as the FULL-coverage
// fast-path detector in Mekalele's apply_exact: when all 8 lanes pass edge
// + Z, the per-lane scatter store can be replaced by unconditional vector
// stores into the G-buffer planes (~2-4 ms on city, dominant hot lines).
inline bool all_lanes_set(Vec8ib m) {
#if defined(__EMSCRIPTEN__)
	// AND-collapse the two 128-bit halves then all-true. Mirrors
	// any_lane_set's wasm fallback shape; sidesteps any simde 256-bit
	// horizontal_and gotcha by going through native v128 helpers.
	__m256i v = *(const __m256i*)(&m);
	v128_t lo = (v128_t)_mm256_castsi256_si128(v);
	v128_t hi = (v128_t)_mm256_extracti128_si256(v, 1);
	return wasm_i32x4_all_true(wasm_v128_and(lo, hi));
#else
	return horizontal_and(m);
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

// AlphaTest: when set (used only by the fountain portal/vortex), each pixel is
// discarded — no colour, no Z — where the texel's alpha byte is 0. The coverage
// is baked into the texture's alpha at generation time (Vortex_Distort clips the
// swirl to a circle and stamps alpha 0xFF inside / 0 in the corners), so the
// kernel just reads it. Discarding the transparent corners stamps no Z there, so
// the volumetric fog integral runs full-length behind them and no dark footprint
// frames the disc. The opaque swirl is kept and stamps Z → the ship is still
// occluded as it flies through.
template <barry::TBlendMode BlendMode, barry::TTextureMode TextureMode, bool WriteZ = true,
          bool AlphaTest = false, bool HDRAccum = false>
struct TileRasterizer {
	TileRasterizer(Vertex** V, byte* dstSurface, int32_t bpsl, int32_t xres, int32_t yres,
	               uint16_t* zpage16, float zScale,
	               uint32_t* gbufferMat32,
	               Texture* Txtr, int miplevel, uint32_t sentinel = 0xFFFFFFFFu)
		: V(V)
		, dstSurface(dstSurface)
		, bpsl(bpsl)
		, xres(xres)
		, yres(yres)
		, zpage16(zpage16)
		, zScale(zScale)
		, gbufferMat32(gbufferMat32)
		, sentinel(sentinel) {

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
	uint16_t* zpage16;     // was: ZPage16 global
	float zScale;          // was: g_zscale global
	// G-buffer mat32 plane (== rt.gbuffer->txtr.data() when running in a
	// deferred context; nullptr in forward-only runs). When non-null we
	// stamp 0xFFFFFFFF (sentinel = "forward filler wrote here, skip in
	// lighting kernel") into mat32 for every Z-winning pixel. Without
	// this stamp the deferred path's correctness depended on FList sort
	// order — a Mekalele-rendered surface behind us (rendered first)
	// would leave its real matID in mat32 and the lighting kernel would
	// re-shade it over our color. With the stamp, order doesn't matter.
	uint32_t* gbufferMat32;
	uint32_t  sentinel;     // mat32 value stamped for written lanes (Mat_HdrEmissive → 0xFFFFFFFE so the HDR lift boosts it; else 0xFFFFFFFF)
	struct TextureInfo {
		const dword* TextureAddr;
		const dword* TextureAddr1;
		int32_t LogWidth;
		int32_t LogHeight;
		// Second (reflection) texture dims for TEXTURETEXTURE. Default 10 =
		// the legacy 1024² equirect panorama (byte-identical to the old
		// hardcode); set per-face from F->ReflectionTexture at bind time so
		// env_cube's pow2 face textures (e.g. 512²) address correctly. Not
		// a per-pixel cost — constant per triangle.
		int32_t Log1Width = 10;
		int32_t Log1Height = 10;
		float UScaleFactor;
		float VScaleFactor;
		float du0zdx, du0zdy;
		float dv0zdx, dv0zdy;
		float du1zdx, du1zdy;
		float dv1zdx, dv1zdy;
		// --env_live_water (Face::LwDU/LwDV): the wave-slope tilt as a
		// per-face offset in SECOND-TEXTURE TEXELS, plus the coverage remap
		// (--env_live_water_mask_bias) the per-pixel weight goes through.
		// lwOn == false → not one instruction of it runs.
		float    lwDU = 0.0f, lwDV = 0.0f;
		float    lwBias = 0.0f, lwGain = 1.0f;
		uint32_t lwAlphaMin = 0;      // bias*255: coverage above it moves
		bool     lwOn = false;        // this face has a tilt to apply
		bool     lwAlphaMask = false; // the reflection texel's alpha is COVERAGE, not opacity
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
		// G-buffer mat32 stamp plane — same xres stride as span. Null
		// outside the deferred path; checked once per tile, branched
		// per row at near-zero cost.
		uint32_t *mat32_span = gbufferMat32
			? gbufferMat32 + tile.y * TILE_SIZE * xres + tile.x * TILE_SIZE
			: nullptr;
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

		int32_t t1_umask = (1 << t0.Log1Width) - 1;
		int32_t t1_vmask = (1 << t0.Log1Height) - 1;
		int32_t t1_umask_swizzled = swizzle_umask(t0.Log1Height, t1_umask);

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

		// Lane order is B,G,R,A to match the gathered texel's in-memory byte
		// order (textures are stored BGRA — stb loads RGBA, IMGCODE swaps
		// byte0↔byte2 on load). colorize() multiplies texel-byte[i] by light-
		// element[i], so the light channels must line up with the texel's
		// B,G,R,A bytes. The old r,g,b,a order paired blue-texel × red-light
		// — invisible under white light, but it tinted every colored-light
		// surface wrong (greets' omnis made reflections green where the
		// deferred kernel — which reads BGRA correctly — showed warm).
		auto color = v32_from_arith_seq(
			{ FixedPoint(tile.t0.b0), FixedPoint(tile.t0.g0), FixedPoint(tile.t0.r0), FixedPoint(tile.t0.a0) },
			{ FixedPoint(dbdx),	   FixedPoint(dgdx),		FixedPoint(drdx),		 FixedPoint(dadx) });

		//Vec16s rg
		for (int32_t y = 0; y != TILE_SIZE; ++y, a0 += tile.dady, b0 += tile.dbdy, c0 += tile.dcdy, span += bpsl_u32, zspan += xres, mat32_span += (mat32_span ? xres : 0)) {
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
					// WriteZ=false (the fountain bolt): Z-TEST still gates p_mask
					// above (so it's occluded by spires), but we don't STAMP Z —
					// a glow is strictly additive and must never occlude itself
					// or anything drawn after (dark bolt-quad-shaped patches).
					// AlphaTest defers the stamp until after the texel is sampled
					// (below) so the discard mask can fold into p_mask first.
					if constexpr (WriteZ && !AlphaTest)
						*(__m128i*)zspan = _mm_blendv_epi8(*(__m128i*)zspan, compress(z_candidate), compress(Vec8ui(p_mask)));
#endif

					// Untextured Gouraud (NONE) can carry a steep per-vertex
					// colour gradient; in partial tiles the rasterizer
					// extrapolates `color` past the triangle, going negative and
					// WRAPPING to garbage (the rainbow fringe on thin ribbon
					// edges). Clamp to [0,255] so the out-of-triangle fringe goes
					// black (= invisible under ADDITIVE). Textured paths use a
					// uniform colour (zero gradient) so they skip this entirely.
					Vec32us blend_color;
					if constexpr (TextureMode == barry::TTextureMode::NONE)
						blend_color = Vec32us(min(max(color, Vec32s(0)), Vec32s(255)));
					else
						blend_color = Vec32us(color);

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
						const Vec8f uf = p_uz * t0.UScaleFactor;
						const Vec8f vf = p_vz * t0.VScaleFactor;
#else
						const Vec8f uf = p_uz * p_z * t0.UScaleFactor;
						const Vec8f vf = p_vz * p_z * t0.VScaleFactor;
#endif
						if constexpr (TextureMode == barry::TTextureMode::NORMAL_BILINEAR) {
							// 4-tap bilinear. fu/fv shared; 2 u-addrs + 2 v-addrs
							// combined into the 4 corner offsets. Weights are
							// 0..255 and the two halves use (255-w) and w, so each
							// blend (s_a*(255-w) + s_b*w) <= 255*255 — fits 16-bit.
							Vec8i u0 = truncatei(uf), v0 = truncatei(vf);
							Vec8i u1 = u0 + 1,        v1 = v0 + 1;
							const Vec8i wu = truncatei((uf - to_float(u0)) * 255.0f);
							const Vec8i wv = truncatei((vf - to_float(v0)) * 255.0f);
							Vec8i tu0 = packed_tile_u(u0, t0.LogHeight, t0_umask_swizzled);
							Vec8i tu1 = packed_tile_u(u1, t0.LogHeight, t0_umask_swizzled);
							Vec8i tv0 = packed_tile_v(v0, t0_vmask);
							Vec8i tv1 = packed_tile_v(v1, t0_vmask);
							const Vec32us s00 = extend(Vec32uc(gather(Vec8ui(tu0+tv0), t0.TextureAddr, p_mask)));
							const Vec32us s10 = extend(Vec32uc(gather(Vec8ui(tu1+tv0), t0.TextureAddr, p_mask)));
							const Vec32us s01 = extend(Vec32uc(gather(Vec8ui(tu0+tv1), t0.TextureAddr, p_mask)));
							const Vec32us s11 = extend(Vec32uc(gather(Vec8ui(tu1+tv1), t0.TextureAddr, p_mask)));
							// Per-pixel weight replicated into all 4 bytes of the lane.
							const Vec32us wU = extend(Vec32uc(Vec8ui(wu) * Vec8ui(0x01010101u)));
							const Vec32us wV = extend(Vec32uc(Vec8ui(wv) * Vec8ui(0x01010101u)));
							const Vec32us iU = Vec32us(255) - wU;
							const Vec32us top = (s00*iU + s10*wU) >> 8;
							const Vec32us bot = (s01*iU + s11*wU) >> 8;
							const Vec32us iV = Vec32us(255) - wV;
							texture0_samples = Vec8ui(compress((top*iV + bot*wV) >> 8));
						} else {
							Vec8i u = roundi(uf);
							Vec8i v = roundi(vf);
							Vec8i tu = packed_tile_u(u, t0.LogHeight, t0_umask_swizzled);
							Vec8i tv = packed_tile_v(v, t0_vmask);
							auto p_offset = tu + tv;
#if BENCH_SKIP_TEXTURE
							// Stub: constant white instead of texture gather.
							texture0_samples = Vec8ui(0xFFFFFFFF);
#else
							texture0_samples = gather(Vec8ui(p_offset), t0.TextureAddr, p_mask);
#endif
						}
					}
					if constexpr (TextureMode == barry::TTextureMode::TEXTURETEXTURE) {
						const Vec8f u1f = p_u1z * p_z * float(1 << t0.Log1Width);
						const Vec8f v1f = p_v1z * p_z * float(1 << t0.Log1Height);
						Vec8i u1 = roundi(u1f);
						Vec8i v1 = roundi(v1f);

						// Sparse env sample (lane-pair UV dedup via permute8) was
						// tested 2026-05-31 expecting cache-line win on the
						// 2nd gather. Result: no measurable bench win (within
						// ±1ms noise on city deferred) AND visible
						// horizontal blur on every env-mapped surface (48k
						// pixels differ, 2.3% of frame). The 8-lane gather
						// already hits adjacent cache lines effectively for
						// most surfaces, and M-series L2 (16MB) trivially
						// holds the 4MB env tex. Removed.

						Vec8i tu1 = packed_tile_u(u1, t0.Log1Height, t1_umask_swizzled);
						Vec8i tv1 = packed_tile_v(v1, t1_vmask);

						auto p_offset1 = tu1 + tv1;
						auto texture1_samples = gather(Vec8ui(p_offset1), t0.TextureAddr1, p_mask);
						if (t0.lwAlphaMask) {
						if (t0.lwOn) {
							// --env_live_water, THE PER-PIXEL MASK READ. The
							// texel just fetched at the UNPERTURBED lookup
							// carries this direction's baked water coverage in
							// its alpha byte (DEMO/CITY.CPP stamps it into the
							// sheets); that is the weight the tilt is scaled
							// by, so a pixel reading the reflected skyline
							// gets 0 and does not move, however much water its
							// neighbours — or its own triangle's corners — are
							// reading. Second gather only; the arithmetic is
							// the same remap the deferred kernel runs
							// (EnvBake.h EnvLiveWater_Weight).
							const Vec8ui aI = Vec8ui(texture1_samples) >> 24;
							// w > 0 exactly when coverage > bias, so the "does
							// this lane move at all" test is an integer
							// compare on the alpha byte (lwAlphaMin =
							// bias·255), no float round trip on dry lanes.
							const auto wet = p_mask & (aI > Vec8ui(t0.lwAlphaMin));
							if (horizontal_or(wet)) {
								const Vec8f a = to_float(Vec8i(aI));
								Vec8f w = (a * (1.0f / 255.0f) - Vec8f(t0.lwBias)) * Vec8f(t0.lwGain);
								w = min(max(w, Vec8f(0.0f)), Vec8f(1.0f));
								Vec8i pu = roundi(mul_add(w, Vec8f(t0.lwDU), u1f));
								Vec8i pv = roundi(mul_add(w, Vec8f(t0.lwDV), v1f));
								const auto po = packed_tile_u(pu, t0.Log1Height, t1_umask_swizzled)
								              + packed_tile_v(pv, t1_vmask);
								const auto tilted = gather(Vec8ui(po), t0.TextureAddr1, wet);
								texture1_samples = select(wet, tilted, texture1_samples);
							}
						}
						// The alpha byte is the mask, not opacity — put the
						// bake's opaque 0xFF back before the composite so the
						// blend (and any AlphaTest user) sees exactly what it
						// saw before the flag existed.
						texture1_samples = Vec8ui(texture1_samples) | Vec8ui(0xFF000000u);
						}
						texture0_samples = Vec8ui(add_saturated(Vec32uc(texture1_samples), Vec32uc(texture0_samples) >> 1));
					}

					// Alpha test (portal/vortex only). Gate on the texel's alpha
					// byte — baked at texture-gen time (Vortex_Distort): 0 in the
					// circle-clipped corners, 0xFF in the swirl. Discard the
					// transparent corners by folding the coverage into p_mask, so
					// neither the colour maskstore (below) nor the Z stamp touch
					// them and the fog integrates full-length behind the disc. The
					// opaque swirl is kept and stamps Z → ship still occluded.
					if constexpr (AlphaTest) {
						p_mask = p_mask & ((texture0_samples & Vec8ui(0xFF000000)) != Vec8ui(0));
#if !BENCH_SKIP_Z
						if constexpr (WriteZ)
							*(__m128i*)zspan = _mm_blendv_epi8(*(__m128i*)zspan, compress(z_candidate),
							                                   compress(Vec8ui(p_mask)));
#endif
					}

#if BENCH_SKIP_COLOR
					// Stub: skip the gouraud color multiply blend. Texture
					// gather feeds the maskstore unmodified — isolates the
					// per-lane colorize cost.
					auto texture_samples = Vec32uc(texture0_samples);
#else
					auto texture_samples = colorize(Vec32uc(texture0_samples), blend_color);
#endif

					if constexpr (HDRAccum) {
						// HDR overlay reorg: additive source → g_hdrBuf (float,
						// uncapped) instead of the 8-bit add_saturated store, so
						// the bolt blooms + rolls off at the tonemap instead of
						// clipping at 255 over the flash. Scalar over the masked
						// lanes (bolt coverage is sparse). Screen x,y from the
						// tile origin; g_hdrBuf is B,G,R,(pad) per pixel at
						// XRes stride (== VPage stride; the deferred path relies
						// on that). The colour maskstore + mat32 stamp are
						// skipped — the bolt draws post-deferred, into HDR only.
						alignas(32) uint8_t hsrc[32];
						Vec32uc(texture_samples).store(hsrc);
						int hmask[8];
						select(p_mask, Vec8i(-1), Vec8i(0)).store(hmask);
						const int hpy  = tile.y * TILE_SIZE + y;
						const int hpx0 = tile.x * TILE_SIZE;
						fds::hdrf* hbuf = fds::g_hdrBuf.data();
						for (int k = 0; k < 8; ++k) {
							if (!hmask[k]) continue;
							fds::hdrf* h = hbuf + (size_t(hpy) * size_t(xres) + size_t(hpx0 + k)) * 4;
							h[0] = fds::HdrClamp(float(h[0]) + float(hsrc[k * 4 + 0]));   // B
							h[1] = fds::HdrClamp(float(h[1]) + float(hsrc[k * 4 + 1]));   // G
							h[2] = fds::HdrClamp(float(h[2]) + float(hsrc[k * 4 + 2]));   // R
						}
					} else {
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

					// Real per-pixel source-alpha blend: src·α + dst·(1−α). α is the
					// texture's authored alpha byte (per lane, broadcast across BGRA
					// the same way the bilinear sampler broadcasts its weight); src
					// is the colorized/lit texel. The lerp runs in 16-bit fixed point
					// (max 255·255 < 65536, so no saturation needed), mirroring the
					// bilinear filter's s·(255−w)+t·w >> 8.
					if constexpr (BlendMode == TBlendMode::SRC_ALPHA) {
						Vec32uc dst;
						dst.load_a(span);
						const Vec8ui  aPx = (texture0_samples >> 24) & Vec8ui(0xFF);
						const Vec32us a   = extend(Vec32uc(aPx * Vec8ui(0x01010101u)));
						const Vec32us ia  = Vec32us(255) - a;
						texture_samples   = compress((extend(texture_samples) * a + extend(dst) * ia) >> 8);
					}


#if BENCH_SKIP_MASKSTORE
					// Stub: unmasked 256-bit store. Writes pixels outside the
					// triangle (visually wrong) but isolates maskstore cost.
					_mm256_storeu_ps((float*)span, *(__m256*)(&texture_samples));
#else
					_mm256_maskstore_ps((float*)span, *(__m256i*)(&p_mask), *(__m256*)(&texture_samples));
#endif
					// In deferred mode, stamp mat32 = 0xFFFFFFFF (sentinel)
					// for the lanes we just wrote. The deferred lighting
					// kernel checks `matID >= matTable.count` (sentinel
					// matID=255 typically out of range) and skips — so the
					// forward filler's color survives, regardless of FList
					// dispatch order.
					if (mat32_span) {
						_mm256_maskstore_epi32(
							reinterpret_cast<int*>(mat32_span),
							*(__m256i*)(&p_mask),
							_mm256_set1_epi32(int(sentinel)));
					}
					}  // end else (non-HDRAccum store path)
				}
			}

			p_rz += Vec8f(drzdy);
			p_uz += Vec8f(t0.du0zdy);
			p_vz += Vec8f(t0.dv0zdy);
			if constexpr (TextureMode == barry::TTextureMode::TEXTURETEXTURE) {
				p_u1z += Vec8f(t0.du1zdy);
				p_v1z += Vec8f(t0.dv1zdy);
			}
			color += Vec32sFromVec4s({ FixedPoint(dbdy), FixedPoint(dgdy), FixedPoint(drdy), FixedPoint(dady) });  // B,G,R,A (see color init above)

			if constexpr (Coverage == barry::TCoverage::PARTIAL) {
				p_a += Vec8i(tile.dady);
				p_b += Vec8i(tile.dbdy);
				p_c += Vec8i(tile.dcdy);
			}
		}
	}

	
	void rasterize_triangle(const Vertex& v1, const Vertex& v2, const Vertex& v3) {
		// FIXME: raster conventions (it is doing floor right now)
		// Clamp to the OWNING clipper tile's range — see ClipperTileRect.h.
		// Without this, two adjacent clipper workers can both rasterize the
		// same 8x8 SIMD tile when a clipped vertex lands exactly on the
		// shared tile boundary -> blendv RMW race in apply_exact.
		const fds::ClipperTileRect& _ctr = fds::g_clipperTileRect;
		const int tile_mx = std::max(_ctr.tile_mx_lo,
			clampedX(std::min({ v1.PX, v2.PX, v3.PX })) / TILE_SIZE);
		const int tile_Mx = std::min(_ctr.tile_mx_hi,
			clampedX(std::max({ v1.PX, v2.PX, v3.PX })) / TILE_SIZE);
		const int tile_my = std::max(_ctr.tile_my_lo,
			clampedY(std::min({ v1.PY, v2.PY, v3.PY })) / TILE_SIZE);
		const int tile_My = std::min(_ctr.tile_my_hi,
			clampedY(std::max({ v1.PY, v2.PY, v3.PY })) / TILE_SIZE);

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

template <barry::TBlendMode BlendMode, barry::TTextureMode TextureMode = barry::TTextureMode::NORMAL, bool WriteZ = true,
          bool AlphaTest = false, bool HDRAccum = false>
void TheOtherBarry(Face* F, Vertex** V, dword numVerts, dword miplevel,
                   const fds::RenderTarget& rt,
                   const fds::CameraContext& cam) {
	//for (dword i = 0; i < numVerts; ++i) {
	//	float z = 1.0f / V[i]->RZ;
	//	V[i]->U = V[i]->UZ * z;
	//	V[i]->V = V[i]->VZ * z;
	//}
	// rt.gbuffer is non-null only on the deferred path; we pass its
	// txtr plane to the rasterizer so it can stamp the mat32 sentinel
	// for the lighting kernel. Null in forward — the rasterizer skips
	// the stamp without paying a per-pixel cost.
	//
	// Helper is defined in Mekalele.cpp (where meka::GBuffer is fully
	// visible) and forward-declared at the top of this header — keeps
	// the include graph straight (Mekalele.h includes TheOtherBarry.h,
	// not the other way round).
	uint32_t *gbufferMat32 = meka::gbuffer_mat32_plane(rt.gbuffer);
	// Mat_HdrEmissive (the forward env disco ball) stamps a DISTINCT sentinel so
	// the HDR lift boosts those pixels past 255 (over-bloom) — material-driven,
	// no hot-kernel cost. Plain forward surfaces keep 0xFFFFFFFF. The deferred
	// kernel skips both (sentinel matID 0xFF >= matTable.count).
	const uint32_t sentinel = (F->Txtr && (F->Txtr->Flags & Mat_HdrEmissive))
	                          ? 0xFFFFFFFEu : 0xFFFFFFFFu;
	barry::TileRasterizer<BlendMode, TextureMode, WriteZ, AlphaTest, HDRAccum> r(V,
	                                                 reinterpret_cast<byte*>(rt.vpage),
	                                                 rt.bytesPerScanline,
	                                                 rt.xres, rt.yres,
	                                                 rt.zpage16, cam.zScale,
	                                                 gbufferMat32,
	                                                 F->Txtr->Txtr, miplevel, sentinel);

	if constexpr (TextureMode == barry::TTextureMode::TEXTURETEXTURE) {
		r.t0.TextureAddr1 = (dword*)F->ReflectionTexture->Data;
		// Drive the second-texture fixed-point scale/mask/tiling from the
		// actual reflection texture (1024² equirect → Log 10, unchanged;
		// 512² env_cube face → Log 9). Reflection textures are Nomip.
		r.t0.Log1Width  = F->ReflectionTexture->LSizeX;
		r.t0.Log1Height = F->ReflectionTexture->LSizeY;
		// --env_live_water: exactly 0,0 unless Transform put a live-water
		// tilt on this face (city windows, flag on), so every other
		// TEXTURETEXTURE user — the greets disco ball, the city mirror
		// pass, the legacy equirect path — takes the same instructions it
		// always did.
		// The city sheets carry the water-coverage mask in their alpha byte
		// whenever --env_live_water baked one, whether or not THIS face ends
		// up tilting; the composite below must not see coverage where it
		// expects the bake's opaque 0xFF, or a face with no tilt would still
		// differ from the pre-flag build.
		r.t0.lwAlphaMask = fds::g_envLiveWater.active;
		if (F->LwDU != 0.0f || F->LwDV != 0.0f) {
			r.t0.lwOn   = true;
			r.t0.lwDU   = F->LwDU * float(1 << r.t0.Log1Width);
			r.t0.lwDV   = F->LwDV * float(1 << r.t0.Log1Height);
			r.t0.lwBias = fds::g_envLiveWater.maskBias;
			r.t0.lwGain = fds::g_envLiveWater.maskGain;
			r.t0.lwAlphaMin = uint32_t(fds::g_envLiveWater.maskBias * 255.0f);
		}
	}

	Vertex vc[12];

	for (dword i = 0; i < numVerts; ++i) {
		vc[i] = *V[i];

		// Scene fog flag is global — CurScene is set by the orchestrator
		// and is per-frame, not per-pass. Leave as global read; phase 4
		// only migrates render-target + camera context.
		if (CurScene->Flags & Scn_Fogged)
		{
			// Gate the sqrt argument BEFORE calling sqrtf — for vertices at
			// or beyond farZ the inner expression goes slightly negative
			// and sqrtf(negative) returns NaN, not a negative number, so
			// the post-hoc `if (fogRate < 0.0)` check is a no-op (NaN
			// compares false to anything). The NaN then propagates through
			// vertex color (NaN * LR = NaN), max(NaN, 10) = NaN on most
			// implementations, and the rasterizer interpolates garbage
			// across triangles at the horizon. -ffp-contract=fast made
			// this latent bug more visible because FMA fusion shifts the
			// argument by 1 ULP at exactly the boundary that flips
			// positive→negative. Mirrors the gate in
			// DeferredLighting.cpp:2099.
			const float fogArg = 1.0f - cam.invFarZ * V[i]->TPos_AOS.z;
			// sqrt(x) = x * rsqrt(x); fast_rsqrt uses arm64 vrsqrte +
			// one Newton-Raphson step (~3-4 cycles) vs ~10 for sqrtf.
			// Same pattern as the deferred fog kernel.
			float fogRate = fogArg > 0.0f ? fogArg * fast_rsqrt(fogArg) : 0.0f;
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
		// One reciprocal instead of four divides (item 1).
		const float invDet = 1.0f / det;
		const float im0 =  m[3] * invDet, im1 = -m[1] * invDet;
		const float im2 = -m[2] * invDet, im3 =  m[0] * invDet;

		// Every gradient is the same im[]·(da, db) solve; batch the
		// per-attribute deltas and evaluate 8 attributes per vector pass
		// (item 5). Lane map: 0 RZ | 1 UZ | 2 VZ | 3 LA | 4 LR | 5 LG | 6 LB |
		// 7 EUZ | 8 EVZ (env, TEXTURETEXTURE only; else 0).
		alignas(32) float da[16] = {0}, db[16] = {0};
		da[0] = v2.RZ - v1.RZ;             db[0] = v3.RZ - v1.RZ;
		da[1] = v2.UZ - v1.UZ;             db[1] = v3.UZ - v1.UZ;
		da[2] = v2.VZ - v1.VZ;             db[2] = v3.VZ - v1.VZ;
		da[3] = float(v2.LA) - float(v1.LA); db[3] = float(v3.LA) - float(v1.LA);
		da[4] = float(v2.LR) - float(v1.LR); db[4] = float(v3.LR) - float(v1.LR);
		da[5] = float(v2.LG) - float(v1.LG); db[5] = float(v3.LG) - float(v1.LG);
		da[6] = float(v2.LB) - float(v1.LB); db[6] = float(v3.LB) - float(v1.LB);
		if constexpr (TextureMode == barry::TTextureMode::TEXTURETEXTURE) {
			da[7] = v2.EUZ - v1.EUZ;       db[7] = v3.EUZ - v1.EUZ;
			da[8] = v2.EVZ - v1.EVZ;       db[8] = v3.EVZ - v1.EVZ;
		}
		alignas(32) float gdx[16], gdy[16];
		const Vec8f vim0(im0), vim1(im1), vim2(im2), vim3(im3);
		for (int k = 0; k < 16; k += 8) {
			Vec8f vda, vdb;
			vda.load_a(da + k);
			vdb.load_a(db + k);
			Vec8f rx = mul_add(vim0, vda, vim1 * vdb);
			Vec8f ry = mul_add(vim2, vda, vim3 * vdb);
			rx.store_a(gdx + k);
			ry.store_a(gdy + k);
		}
		r.drzdx = gdx[0]; r.drzdy = gdy[0];
		r.t0.du0zdx = gdx[1]; r.t0.du0zdy = gdy[1];
		r.t0.dv0zdx = gdx[2]; r.t0.dv0zdy = gdy[2];
		r.dadx = gdx[3]; r.dady = gdy[3];
		r.drdx = gdx[4]; r.drdy = gdy[4];
		r.dgdx = gdx[5]; r.dgdy = gdy[5];
		r.dbdx = gdx[6]; r.dbdy = gdy[6];
		if constexpr (TextureMode == barry::TTextureMode::TEXTURETEXTURE) {
			r.t0.du1zdx = gdx[7]; r.t0.du1zdy = gdy[7];
			r.t0.dv1zdx = gdx[8]; r.t0.dv1zdy = gdy[8];
		}
		r.umask = (1 << r.t0.LogWidth) - 1;
		r.vmask = (1 << r.t0.LogHeight) - 1;

		r.rasterize_triangle(v1, v2, v3);
	}
}

// Explicit-instantiation declarations: prevent every TU that includes
// this header from implicitly instantiating these template variants.
// The actual emitted bodies live in FDS/FILLERS/RasterizerInst.cpp,
// which is the single canonical TU that owns the symbol. This is what
// lets us safely compile RasterizerInst.cpp with -ffp-contract=fast
// (perf win in apply_exact) without creating an ODR mismatch with
// other TUs (FOUNTAIN.CPP, Snapshot.cpp, PREPROC.CPP, RenderInner.cpp,
// FillerTest.cpp) that historically would have emitted their own
// implicit instantiation with whatever -ffp-contract they were
// compiled under. See the long CMake comment in DEMO/CMakeLists.txt
// for the bug history.
extern template void TheOtherBarry<barry::TBlendMode::OVERWRITE,    barry::TTextureMode::NONE>          (Face*, Vertex**, dword, dword, const fds::RenderTarget&, const fds::CameraContext&);
extern template void TheOtherBarry<barry::TBlendMode::OVERWRITE,    barry::TTextureMode::TEXTURETEXTURE>(Face*, Vertex**, dword, dword, const fds::RenderTarget&, const fds::CameraContext&);
extern template void TheOtherBarry<barry::TBlendMode::OVERWRITE,    barry::TTextureMode::NORMAL>        (Face*, Vertex**, dword, dword, const fds::RenderTarget&, const fds::CameraContext&);
extern template void TheOtherBarry<barry::TBlendMode::TRANSPARENT,  barry::TTextureMode::NONE>          (Face*, Vertex**, dword, dword, const fds::RenderTarget&, const fds::CameraContext&);
extern template void TheOtherBarry<barry::TBlendMode::TRANSPARENT,  barry::TTextureMode::NORMAL>        (Face*, Vertex**, dword, dword, const fds::RenderTarget&, const fds::CameraContext&);
extern template void TheOtherBarry<barry::TBlendMode::ADDITIVE,     barry::TTextureMode::NORMAL>        (Face*, Vertex**, dword, dword, const fds::RenderTarget&, const fds::CameraContext&);
extern template void TheOtherBarry<barry::TBlendMode::ADDITIVE,     barry::TTextureMode::NORMAL_BILINEAR>(Face*, Vertex**, dword, dword, const fds::RenderTarget&, const fds::CameraContext&);
extern template void TheOtherBarry<barry::TBlendMode::ADDITIVE,     barry::TTextureMode::NONE>          (Face*, Vertex**, dword, dword, const fds::RenderTarget&, const fds::CameraContext&);
// Z-test-but-no-Z-write variants for the fountain bolt glow (strictly additive).
extern template void TheOtherBarry<barry::TBlendMode::ADDITIVE,     barry::TTextureMode::NONE,           false>(Face*, Vertex**, dword, dword, const fds::RenderTarget&, const fds::CameraContext&);
extern template void TheOtherBarry<barry::TBlendMode::ADDITIVE,     barry::TTextureMode::NORMAL_BILINEAR, false>(Face*, Vertex**, dword, dword, const fds::RenderTarget&, const fds::CameraContext&);
// HDR overlay reorg: bolt glow ribbon accumulating into g_hdrBuf (HDRAccum=true)
// so it blooms over the flash instead of clipping. Used when --hdr + g_hdrActive.
extern template void TheOtherBarry<barry::TBlendMode::ADDITIVE,     barry::TTextureMode::NONE,           false, false, true>(Face*, Vertex**, dword, dword, const fds::RenderTarget&, const fds::CameraContext&);
extern template void TheOtherBarry<barry::TBlendMode::ADDITIVE,     barry::TTextureMode::NORMAL_BILINEAR, false, false, true>(Face*, Vertex**, dword, dword, const fds::RenderTarget&, const fds::CameraContext&);
// Alpha-tested textured additive — the fountain portal/vortex (WriteZ=true,
// AlphaTest=true): discards transparent texels so the fog isn't truncated
// behind them, while the bright swirl still stamps Z and occludes the ship.
extern template void TheOtherBarry<barry::TBlendMode::ADDITIVE,     barry::TTextureMode::NORMAL,          true, true>(Face*, Vertex**, dword, dword, const fds::RenderTarget&, const fds::CameraContext&);
// Per-pixel source-alpha blend — reusable primitive (no consumer yet). Compiled
// here so it's ready; add BILINEAR / NONE / WriteZ=false variants when needed.
extern template void TheOtherBarry<barry::TBlendMode::SRC_ALPHA,    barry::TTextureMode::NORMAL>        (Face*, Vertex**, dword, dword, const fds::RenderTarget&, const fds::CameraContext&);

// Map a Material to its TheOtherBarry filler by flags — the single
// canonical picker for hand-built / regenerated geometry (SceneBuilder
// quads, shatter shards). Reflective faces route by Face::Flags &
// Face_Reflective, which a Material-only picker can't see: the caller
// stamps F.Filler directly for those.
inline RasterFunc PickFillerForMaterial(const Material *mat) {
	if (!mat) {
		return TheOtherBarry<barry::TBlendMode::OVERWRITE,
		                     barry::TTextureMode::NORMAL>;
	}
	if (mat->Flags & Mat_Transparent) {
		return TheOtherBarry<barry::TBlendMode::TRANSPARENT,
		                     barry::TTextureMode::NORMAL>;
	}
	return TheOtherBarry<barry::TBlendMode::OVERWRITE,
	                     barry::TTextureMode::NORMAL>;
}
