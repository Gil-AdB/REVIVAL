#pragma once

#include <algorithm>

#include "Base/FDS_DECS.H"
#include "Base/FDS_VARS.H"
#include "Base/RenderTarget.h"
#include "Base/CameraContext.h"
#include "F4Vec.h"

#include "TheOtherBarry.h"

//#include <intrin.h>
#include "simde/x86/avx2.h"
#include <simd/vectorclass.h>
#include <cassert>
#include <array>
#include <vector>
#include <iostream>
#include "SimdHelpers.h"

#include "Base/Scene.h"

namespace meka {
using u16 = uint16_t;
using i32 = int32_t;
using u32 = uint32_t;
constexpr const i32 TILE_SIZE = 8;
using TScreenCoord = i32;

struct GBuffer {
	// Octahedral-packed shading normal: 8-bit x, 8-bit y, decoded to
	// (nx, ny, nz) on a unit sphere in the lighting pass. The lighting
	// pass reconstructs view-space position from ZPage16 + screen XY,
	// so we don't carry position here.
	std::vector<u16> normal;
	// Octahedral-packed view-space tangent (Tier B normal map support).
	// Same encoding as `normal`. The lighting kernel reads tangent only
	// for normal-mapped materials; everything else ignores it. Optional
	// — buffer is allocated to `numPixels` but a renderer that doesn't
	// care about tangent-space maps can leave the data as garbage.
	std::vector<u16> tangent;
	// packed: miplevel:4 | matID:8 | swizzled UV:20.
	// The matID byte doubles as the per-pixel surface ID for poly-ID
	// shadow mode (compared against ShadowMap::polyId, which stores
	// matID+1 of the closest occluder).
	std::vector<u32> txtr;
};

// Octahedral encode: unit vector (nx, ny, nz) -> 16-bit (8.8 signed).
// Quantization error is sub-degree at 8 bits per axis, well below
// what visibly matters for diffuse lighting. The lighting pass does
// the inverse via oct_decode_u16.
inline u16 oct_encode_u16(float nx, float ny, float nz) {
	float invL1 = 1.0f / (std::fabs(nx) + std::fabs(ny) + std::fabs(nz));
	float ox = nx * invL1;
	float oy = ny * invL1;
	if (nz < 0.0f) {
		float fx = (1.0f - std::fabs(oy)) * (ox >= 0.0f ? 1.0f : -1.0f);
		float fy = (1.0f - std::fabs(ox)) * (oy >= 0.0f ? 1.0f : -1.0f);
		ox = fx; oy = fy;
	}
	int qx = int(std::round(ox * 127.0f));
	int qy = int(std::round(oy * 127.0f));
	if (qx < -128) qx = -128; if (qx > 127) qx = 127;
	if (qy < -128) qy = -128; if (qy > 127) qy = 127;
	return u16((qx & 0xff) | ((qy & 0xff) << 8));
}

// Inverse of oct_encode_u16. Output is unit-length (mod quantization
// error). Used by the lighting pass and the debug visualization.
inline void oct_decode_u16(u16 packed, float &nx, float &ny, float &nz) {
	int qx = int8_t(packed & 0xff);
	int qy = int8_t((packed >> 8) & 0xff);
	float ox = qx * (1.0f / 127.0f);
	float oy = qy * (1.0f / 127.0f);
	float az = 1.0f - std::fabs(ox) - std::fabs(oy);
	if (az < 0.0f) {
		float fx = (1.0f - std::fabs(oy)) * (ox >= 0.0f ? 1.0f : -1.0f);
		float fy = (1.0f - std::fabs(ox)) * (oy >= 0.0f ? 1.0f : -1.0f);
		ox = fx; oy = fy;
	}
	float invLen = fast_rsqrt(ox*ox + oy*oy + az*az);
	nx = ox * invLen;
	ny = oy * invLen;
	nz = az * invLen;
}

struct Tile {
	int x, y;

	TScreenCoord a0, dadx, dady;
	TScreenCoord b0, dbdx, dbdy;
	TScreenCoord c0, dcdx, dcdy;

	float uz0, vz0, rz0;
	// Per-tile origin of the interpolated shading normal. The rasterizer
	// adds dnxdx/dnydx etc. across the 8 lanes of a row and dnxdy/.. down
	// the rows; per-pixel we renormalize via rsqrt to recover unit length.
	float nx0, ny0, nz0;
	// Per-tile origin of the interpolated view-space tangent. Same nlerp
	// treatment as the shading normal — the lighting pass Gram-Schmidts
	// it against the (also-interpolated) N before building TBN.
	float tx0, ty0, tz0;
};

struct TileRasterizerCtx {
	Vertex** V;
	i32 xres, yres;
	Texture* Txtr;     // for LogWidth/LogHeight UV scaling math
	dword matID;       // packed into mat32; what the deferred lighting pass uses to look up Material*. NOT Texture::ID — different per-scene number space.
	dword miplevel;
	u16 *zbuffer;
	float zScale;      // was: g_zscale global. Per-pass depth scalar.
};

// Strip clamp for the unified-TBR per-strip xpar dispatch. When set,
// the rasterizer further clamps tile_my/tile_My to [tileYMin, tileYMax]
// (inclusive, tile-row indices), so a clipped triangle whose max PY
// snapped to the strip's bottom edge can't iterate INTO the next
// strip's tile row and write a pixel that the next strip will then
// clear. Default (-1, INT32_MAX) is "no clamp" — every non-strip
// raster path leaves it unset.
struct RasterStripClamp {
	int tileYMin = -1;
	int tileYMax = INT32_MAX;
};
inline thread_local RasterStripClamp g_rasterStripClamp;

struct GBufferSpan {
	u16 *normal;
	u16 *tangent;
	u32 *txtr;
	u16 *zbuffer;

	GBufferSpan &operator+=(i32 offset) {
		normal += offset;
		tangent += offset;
		txtr += offset;
		zbuffer += offset;
		return *this;
	}

	static GBufferSpan of(GBuffer &gbuffer, const TileRasterizerCtx &ctx, u32 x, u32 y) {
		u32 offset = x + y * ctx.xres;
		// tangent buffer is optional (may be empty for renderers that
		// don't care). Return nullptr in that case; the rasterizer
		// inner loop checks before writing.
		u16 *tangentPtr = gbuffer.tangent.empty()
			? nullptr
			: gbuffer.tangent.data() + offset;
		return {
			gbuffer.normal.data() + offset,
			tangentPtr,
			gbuffer.txtr.data() + offset,
			ctx.zbuffer + offset
		};
	}
};


constexpr const int8_t SUBPIXEL_BITS = 8;
constexpr const float SUBPIXEL_MULT = 1 << SUBPIXEL_BITS;

inline TScreenCoord orient2d(
	TScreenCoord ax, TScreenCoord ay,
	TScreenCoord bx, TScreenCoord by,
	TScreenCoord cx, TScreenCoord cy)
{
	return (int64_t(bx - ax) * int64_t(cy - ay) - int64_t(by - ay) * int64_t(cx - ax)) >> SUBPIXEL_BITS;
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

struct TileRasterizer {
	GBuffer &gbuffer;
	TileRasterizerCtx ctx; 
	TileRasterizer(GBuffer &gbuffer, TileRasterizerCtx ctx)
		: gbuffer(gbuffer)
		, ctx(ctx) {
		LogWidth = ctx.Txtr->LSizeX - ctx.miplevel;
		LogHeight = ctx.Txtr->LSizeY - ctx.miplevel;
		if (ctx.matID >= 256) {
			std::cerr << "matID out of range: " << ctx.matID << std::endl;
			abort();
		}
		// mat32: miplevel:4 | matID:8 | swizzledUV:20.
		// matID indexes the per-scene Material* table (Scene_GetMatTable),
		// NOT Texture::ID. The deferred lighting pass dereferences the
		// material to get diffuse/luminosity/basecol AND its texture.
		u32 TxtrIdMask = ((u32)ctx.miplevel << 28) | ((u32)ctx.matID << 20);
		v8_TxtrIdMask = Vec8i((i32)TxtrIdMask);

		UScaleFactor = (1 << LogWidth);
		VScaleFactor = (1 << LogHeight);
	}

	// miplevel (4 bits) | txtr id (8 bit) | zeroes (20 bit)
	Vec8i v8_TxtrIdMask;
	int32_t LogWidth;
	int32_t LogHeight;
	float UScaleFactor;
	float VScaleFactor;
	float duzdx, duzdy;
	float dvzdx, dvzdy;
	float drzdx, drzdy;
	// Per-pixel shading-normal gradients. Linear-in-screen interpolation
	// (nlerp): renormalize per-pixel after summing lane offsets.
	float dnxdx, dnxdy;
	float dnydx, dnydy;
	float dnzdx, dnzdy;
	// Per-pixel view-space tangent gradients (Tier B tangent-space normal
	// maps). Same linear-in-screen interpolation as the shading normal —
	// the lighting kernel handles renormalization + Gram-Schmidt vs N.
	float dtxdx, dtxdy;
	float dtydx, dtydy;
	float dtzdx, dtzdy;

	uint32_t umask;// = (1 << LogWidth) - 1);
	uint32_t vmask;// = (1 << LogHeight) - 1);
	//size_t v1 = 0 , v2 = 0, v3 = 0;
	//void setVertexIndexes(size_t v1, size_t v2, size_t v3) {
	//	this->v1 = v1;
	//	this->v2 = v2;
	//	this->v3 = v3;
	//}

	int32_t clampedX(int32_t x) {
		return std::min(std::max(x, 0), ctx.xres - 1);
	}
	int32_t clampedY(int32_t y) {
		return std::min(std::max(y, 0), ctx.yres - 1);
	}
	void apply_exact(const meka::Tile& tile) {
		auto span = GBufferSpan::of(gbuffer, ctx, tile.x * TILE_SIZE, tile.y * TILE_SIZE);

		TScreenCoord a0 = tile.a0;
		TScreenCoord b0 = tile.b0;
		TScreenCoord c0 = tile.c0;

		Vec8i p_a = v8_from_arith_seq(a0, tile.dadx);
		Vec8i p_b = v8_from_arith_seq(b0, tile.dbdx);
		Vec8i p_c = v8_from_arith_seq(c0, tile.dcdx);

		int32_t t_umask = (1 << LogWidth) - 1;
		int32_t t_vmask = (1 << LogHeight) - 1;
		int32_t t_umask_swizzled = swizzle_umask(LogHeight, t_umask);

		Vec8f p_rz = v8_from_arith_seq(tile.rz0, drzdx);
		Vec8f p_uz = v8_from_arith_seq(tile.uz0, duzdx);
		Vec8f p_vz = v8_from_arith_seq(tile.vz0, dvzdx);
		Vec8f p_nx = v8_from_arith_seq(tile.nx0, dnxdx);
		Vec8f p_ny = v8_from_arith_seq(tile.ny0, dnydx);
		Vec8f p_nz = v8_from_arith_seq(tile.nz0, dnzdx);
		const bool wantTangent = (span.tangent != nullptr);
		Vec8f p_tx = wantTangent ? v8_from_arith_seq(tile.tx0, dtxdx) : Vec8f(0.0f);
		Vec8f p_ty = wantTangent ? v8_from_arith_seq(tile.ty0, dtydx) : Vec8f(0.0f);
		Vec8f p_tz = wantTangent ? v8_from_arith_seq(tile.tz0, dtzdx) : Vec8f(0.0f);

		for (int32_t y = 0; y != TILE_SIZE; ++y, a0 += tile.dady, b0 += tile.dbdy, c0 += tile.dcdy, span += ctx.xres) {
			auto p_mask = (p_a | p_b | p_c) >= 0;
			if (barry::any_lane_set(p_mask)) {
				Vec8f p_z = approx_recipr(p_rz);

				auto z_candidate = (Vec8ui(0xFF80) - static_cast<Vec8ui>(roundi(ctx.zScale * p_z)));
				Vec8us z_existing_c;
				z_existing_c.load_a(span.zbuffer);
				auto z_existing = extend(z_existing_c);

				auto zmask = z_candidate > z_existing;

				p_mask &= zmask;

				if (barry::any_lane_set(p_mask)) {
					*(__m128i*)span.zbuffer = _mm_blendv_epi8(*(__m128i*)span.zbuffer, compress(z_candidate), compress(Vec8ui(p_mask)));
					Vec8i u = roundi(p_uz * p_z * UScaleFactor);
					Vec8i v = roundi(p_vz * p_z * VScaleFactor);

					Vec8i tu = packed_tile_u(u, LogHeight, t_umask_swizzled);
					Vec8i tv = packed_tile_v(v, t_vmask);

					auto p_offset = tu + tv;
					auto packedTxtrData = v8_TxtrIdMask | p_offset;
					_mm256_maskstore_ps(span.txtr, *(__m256i*)(&p_mask), *(__m256*)(&packedTxtrData));

					// Per-pixel nlerp + octahedral pack. Scalar fallback for
					// now — the encode has a couple of branches (sign tests
					// in the z<0 fold) that are awkward to vectorize cleanly,
					// and the pass is rare-pixel-rate compared to the texture
					// fetch in the lighting pass. Vectorize once we can show
					// it on a profile.
					alignas(32) float nx_l[8], ny_l[8], nz_l[8];
					p_nx.store_a(nx_l);
					p_ny.store_a(ny_l);
					p_nz.store_a(nz_l);
					alignas(32) float tx_l[8], ty_l[8], tz_l[8];
					if (wantTangent) {
						p_tx.store_a(tx_l);
						p_ty.store_a(ty_l);
						p_tz.store_a(tz_l);
					}
					alignas(32) int32_t mask_l[8];
					Vec8i(p_mask).store_a(mask_l);
					for (int lane = 0; lane < 8; ++lane) {
						if (!mask_l[lane]) continue;
						float nx = nx_l[lane], ny = ny_l[lane], nz = nz_l[lane];
						float invLen = fast_rsqrt(nx*nx + ny*ny + nz*nz);
						nx *= invLen; ny *= invLen; nz *= invLen;
						span.normal[lane] = oct_encode_u16(nx, ny, nz);
						if (wantTangent) {
							// Gram-Schmidt the interpolated tangent against the
							// per-pixel renormalized N. Without this the TBN
							// frame skews near triangle interior pixels where
							// nlerp doesn't preserve the original ⟂(N,T)
							// relationship from the vertex shader.
							float tx = tx_l[lane], ty = ty_l[lane], tz = tz_l[lane];
							const float tDotN = tx*nx + ty*ny + tz*nz;
							tx -= nx * tDotN;
							ty -= ny * tDotN;
							tz -= nz * tDotN;
							const float tLen2 = tx*tx + ty*ty + tz*tz;
							if (tLen2 > 1e-12f) {
								const float invTLen = fast_rsqrt(tLen2);
								span.tangent[lane] = oct_encode_u16(tx*invTLen, ty*invTLen, tz*invTLen);
							} else {
								// Degenerate tangent (parallel to N after
								// interpolation). Fall back to an arbitrary
								// ⟂N reference — the lighting kernel will
								// still produce a sane TBN frame.
								span.tangent[lane] = 0;
							}
						}
					}
				}
			}

			p_rz += Vec8f(drzdy);
			p_uz += Vec8f(duzdy);
			p_vz += Vec8f(dvzdy);
			p_nx += Vec8f(dnxdy);
			p_ny += Vec8f(dnydy);
			p_nz += Vec8f(dnzdy);
			if (wantTangent) {
				p_tx += Vec8f(dtxdy);
				p_ty += Vec8f(dtydy);
				p_tz += Vec8f(dtzdy);
			}

			p_a += Vec8i(tile.dady);
			p_b += Vec8i(tile.dbdy);
			p_c += Vec8i(tile.dcdy);
		}
	}

	
	void rasterize_triangle(const Vertex& v1, const Vertex& v2, const Vertex& v3) {
		// FIXME: raster conventions (it is doing floor right now)
		const int tile_mx = clampedX(std::min({ v1.PX, v2.PX, v3.PX })) / TILE_SIZE;
		const int tile_Mx = clampedX(std::max({ v1.PX, v2.PX, v3.PX })) / TILE_SIZE;
		int tile_my = clampedY(std::min({ v1.PY, v2.PY, v3.PY })) / TILE_SIZE;
		int tile_My = clampedY(std::max({ v1.PY, v2.PY, v3.PY })) / TILE_SIZE;
		// Strip clamp (unified TBR): keep this rasterization within the
		// strip's tile row. Without it, a triangle clipped to PY=strip_y_max
		// computes tile_My one row past the strip — the rasterizer writes
		// those out-of-strip pixels (bary hits zero on the bottom edge,
		// passes >= 0), then the neighbouring strip's clear (concurrent
		// in the threadpool) wipes them. Result: 1-row horizontal stripe
		// every TILE_SIZE rows, exactly at strip boundaries.
		if (g_rasterStripClamp.tileYMax < INT32_MAX) {
			if (tile_My > g_rasterStripClamp.tileYMax) tile_My = g_rasterStripClamp.tileYMax;
			if (tile_my < g_rasterStripClamp.tileYMin) tile_my = g_rasterStripClamp.tileYMin;
			if (tile_my > tile_My) return;
		}

		TScreenCoord v1x = TScreenCoord(v1.PX * SUBPIXEL_MULT + 0.5);
		TScreenCoord v1y = TScreenCoord(v1.PY * SUBPIXEL_MULT + 0.5);
		TScreenCoord v2x = TScreenCoord(v2.PX * SUBPIXEL_MULT + 0.5);
		TScreenCoord v2y = TScreenCoord(v2.PY * SUBPIXEL_MULT + 0.5);
		TScreenCoord v3x = TScreenCoord(v3.PX * SUBPIXEL_MULT + 0.5);
		TScreenCoord v3y = TScreenCoord(v3.PY * SUBPIXEL_MULT + 0.5);

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
		for (int y = tile_my; y <= tile_My; ++y, _a0 += TILE_SIZE * dady, _b0 += TILE_SIZE * dbdy, _c0 += TILE_SIZE * dcdy, ++i) {
			TScreenCoord a0 = _a0;
			TScreenCoord b0 = _b0;
			TScreenCoord c0 = _c0;
			for (int x = tile_mx; x <= tile_Mx; ++x, a0 += TILE_SIZE * dadx, b0 += TILE_SIZE * dbdx, c0 += TILE_SIZE * dcdx, ++i) {
				TScreenCoord max_a = a0 + ((dadx > 0) ? dadx * TILE_SIZE : 0) + ((dady > 0) ? dady * TILE_SIZE : 0);
				TScreenCoord max_b = b0 + ((dbdx > 0) ? dbdx * TILE_SIZE : 0) + ((dbdy > 0) ? dbdy * TILE_SIZE : 0);
				TScreenCoord max_c = c0 + ((dcdx > 0) ? dcdx * TILE_SIZE : 0) + ((dcdy > 0) ? dcdy * TILE_SIZE : 0);

				if ((max_a | max_b | max_c) >= 0) {
					// FIXME: define outside and maintain
					const float dx = float(x) * TILE_SIZE - v1.PX;
					const float dy = float(y) * TILE_SIZE - v1.PY;
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
						.rz0 = v1.RZ + dx * drzdx + dy * drzdy,
						.uz0 = v1.UZ + dx * duzdx + dy * duzdy,
						.vz0 = v1.VZ + dx * dvzdx + dy * dvzdy,
						.nx0 = v1.TN.x + dx * dnxdx + dy * dnxdy,
						.ny0 = v1.TN.y + dx * dnydx + dy * dnydy,
						.nz0 = v1.TN.z + dx * dnzdx + dy * dnzdy,
						.tx0 = v1.TTangent.x + dx * dtxdx + dy * dtxdy,
						.ty0 = v1.TTangent.y + dx * dtydx + dy * dtydy,
						.tz0 = v1.TTangent.z + dx * dtzdx + dy * dtzdy,
					};
					apply_exact(tile);
				}
			}
		}
	}

};

} // namespace meka

extern meka::GBuffer *g_gbuffer;
extern meka::GBuffer *g_gbufferTransparent;
extern uint16_t      *g_xparZ;        // separate Z-buffer for closest-front transparent
extern int            g_xparZCount;

inline void SetGBuffer(meka::GBuffer *gbuffer) {
	// Initalize GBuffer
	g_gbuffer = gbuffer;
}

// Engine G-buffer lifecycle — sized to current framebuffer (in pixels) on
// boot and every resize. Owns the static opaque + transparent GBuffers
// and the transparent Z-buffer; sets g_gbuffer / g_gbufferTransparent /
// g_xparZ to point at them. Called from V_Create (DEMO/SDL2.cpp) for the
// live engine surface, and from initSnapshotEnvironment (DEMO/Snapshot.cpp)
// for the headless snapshot path.
void EngineGBuffer_Resize(int X, int Y);

// Which deferred buffer set a Mekalele dispatch writes into.
//   Opaque           — opaque G-buffer + ZPage16
//   TransparentFront — front-layer xpar G-buffer + g_xparZ; the closest
//                      front-facing transparent surface per pixel.
//   TransparentBack  — back-layer xpar G-buffer + g_xparZBack; the
//                      back-facing surface paired with TransparentFront
//                      for 2-deep transparent rendering of convex
//                      transparent objects (glass cube entry+exit).
enum class MekaleleTarget {
	Opaque,
	TransparentFront,
	TransparentBack,
};

extern meka::GBuffer *g_gbufferTransparentBack;
extern uint16_t      *g_xparZBack;

// Mekalele rasterizer entry point, templated on which buffer set to
// write into. Body is identical across targets — only the G-buffer
// pointer and zbuffer pointer differ. Three instantiations below
// provide the concrete `RasterFunc`-compatible function pointers
// `Mekalele`, `MekaleleTransparent`, `MekaleleTransparentBack` that
// the rest of the engine references.
template <MekaleleTarget Target>
inline void MekaleleImpl(Face* F, Vertex** V, dword numVerts, dword miplevel,
                         const fds::RenderTarget& rt,
                         const fds::CameraContext& cam) {
	meka::GBuffer *gb;
	uint16_t *zbuf;
	if constexpr (Target == MekaleleTarget::Opaque) {
		gb   = rt.gbuffer;
		zbuf = rt.zpage16;
	} else if constexpr (Target == MekaleleTarget::TransparentFront) {
		gb   = rt.gbufferTransparent;
		zbuf = rt.xparZ;
	} else {  // TransparentBack
		gb   = rt.gbufferTransparentBack;
		zbuf = rt.xparZBack;
	}
	meka::TileRasterizerCtx ctx = {
		.V = V,
		.xres = rt.xres,
		.yres = rt.yres,
		.Txtr = F->Txtr->Txtr,
		.matID = F->Txtr->ID,
		.miplevel = miplevel,
		.zbuffer = zbuf,
		.zScale = cam.zScale,
	};
	meka::TileRasterizer r(*gb, ctx);

	Vertex vc[12];
	for (dword i = 0; i < numVerts; ++i) {
		vc[i] = *V[i];
	}
	for (dword i = 2; i < numVerts; ++i) {
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
		r.duzdx = im[0] * (v2.UZ - v1.UZ) + im[1] * (v3.UZ - v1.UZ);
		r.duzdy = im[2] * (v2.UZ - v1.UZ) + im[3] * (v3.UZ - v1.UZ);
		r.dvzdx = im[0] * (v2.VZ - v1.VZ) + im[1] * (v3.VZ - v1.VZ);
		r.dvzdy = im[2] * (v2.VZ - v1.VZ) + im[3] * (v3.VZ - v1.VZ);
		// Per-vertex shading-normal gradients. Same screen-space
		// inverse-Jacobian (`im`) as UV — the rasterizer interpolates
		// linearly across the triangle and renormalizes per-pixel (nlerp).
		r.dnxdx = im[0] * (v2.TN.x - v1.TN.x) + im[1] * (v3.TN.x - v1.TN.x);
		r.dnxdy = im[2] * (v2.TN.x - v1.TN.x) + im[3] * (v3.TN.x - v1.TN.x);
		r.dnydx = im[0] * (v2.TN.y - v1.TN.y) + im[1] * (v3.TN.y - v1.TN.y);
		r.dnydy = im[2] * (v2.TN.y - v1.TN.y) + im[3] * (v3.TN.y - v1.TN.y);
		r.dnzdx = im[0] * (v2.TN.z - v1.TN.z) + im[1] * (v3.TN.z - v1.TN.z);
		r.dnzdy = im[2] * (v2.TN.z - v1.TN.z) + im[3] * (v3.TN.z - v1.TN.z);
		// Per-vertex view-space tangent gradients (Tier B). Same im[]
		// inverse-Jacobian as N/UV. Used by the inner loop only when
		// span.tangent != nullptr — the transparent G-buffer paths
		// leave it null and skip the tangent write entirely.
		r.dtxdx = im[0] * (v2.TTangent.x - v1.TTangent.x) + im[1] * (v3.TTangent.x - v1.TTangent.x);
		r.dtxdy = im[2] * (v2.TTangent.x - v1.TTangent.x) + im[3] * (v3.TTangent.x - v1.TTangent.x);
		r.dtydx = im[0] * (v2.TTangent.y - v1.TTangent.y) + im[1] * (v3.TTangent.y - v1.TTangent.y);
		r.dtydy = im[2] * (v2.TTangent.y - v1.TTangent.y) + im[3] * (v3.TTangent.y - v1.TTangent.y);
		r.dtzdx = im[0] * (v2.TTangent.z - v1.TTangent.z) + im[1] * (v3.TTangent.z - v1.TTangent.z);
		r.dtzdy = im[2] * (v2.TTangent.z - v1.TTangent.z) + im[3] * (v3.TTangent.z - v1.TTangent.z);

		r.umask = (1 << r.LogWidth) - 1;
		r.vmask = (1 << r.LogHeight) - 1;

		r.rasterize_triangle(v1, v2, v3);
	}
}

inline void Mekalele(Face* F, Vertex** V, dword numVerts, dword miplevel,
                     const fds::RenderTarget& rt,
                     const fds::CameraContext& cam) {
	MekaleleImpl<MekaleleTarget::Opaque>(F, V, numVerts, miplevel, rt, cam);
}
inline void MekaleleTransparent(Face* F, Vertex** V, dword numVerts, dword miplevel,
                                const fds::RenderTarget& rt,
                                const fds::CameraContext& cam) {
	MekaleleImpl<MekaleleTarget::TransparentFront>(F, V, numVerts, miplevel, rt, cam);
}
inline void MekaleleTransparentBack(Face* F, Vertex** V, dword numVerts, dword miplevel,
                                    const fds::RenderTarget& rt,
                                    const fds::CameraContext& cam) {
	MekaleleImpl<MekaleleTarget::TransparentBack>(F, V, numVerts, miplevel, rt, cam);
}
