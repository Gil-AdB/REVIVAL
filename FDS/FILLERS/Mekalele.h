#pragma once

#include <algorithm>

#include "Base/FDS_DECS.H"
#include "Base/FDS_VARS.H"
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

struct alignas(16) Vector4f {
	// NOTE: can use w to store 1/z.
	float x, y, z, w;

	static Vector4f fromVertex(const Vertex* v) {
		return { v->PX, v->PY, 1.0f / v->RZ, v->RZ };
	}
};

struct GBuffer {
	// Interpolated position in camera space
	std::vector<Vector4f> position;
	// Interpolated normal in camera space
	std::vector<Vector4f> normal;
	// packed: txtr id - 7 bit, swizzled u+v - 20 bit, miplevel - 3-4 bit
	std::vector<u32> txtr;
};

struct Tile {
	int x, y;

	TScreenCoord a0, dadx, dady;
	TScreenCoord b0, dbdx, dbdy;
	TScreenCoord c0, dcdx, dcdy;

	float uz0, vz0, rz0;
};

struct TileRasterizerCtx {
	Vertex** V;
	i32 xres, yres;
	Texture* Txtr;
	dword miplevel;
	u16 *zbuffer;
};

struct GBufferSpan {
	Vector4f *position;
	Vector4f *normal;
	u32 *txtr;
	u16 *zbuffer;
	
	GBufferSpan &operator+=(i32 offset) {
		position += offset;
		normal += offset;
		txtr += offset;
		zbuffer += offset;
		return *this;
	}

	static GBufferSpan of(GBuffer &gbuffer, const TileRasterizerCtx &ctx, u32 x, u32 y) {
		u32 offset = x + y * ctx.xres;
		return {
			gbuffer.position.data() + offset,
			gbuffer.normal.data() + offset,
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
		if (ctx.Txtr->ID >= 256) {
			std::cerr << "TxtrID out of range: " << ctx.Txtr->ID << std::endl;
			abort();
		}
		u32 TxtrIdMask = ((u32)ctx.miplevel << 28) | ((u32)ctx.Txtr->ID << 20);
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

		for (int32_t y = 0; y != TILE_SIZE; ++y, a0 += tile.dady, b0 += tile.dbdy, c0 += tile.dcdy, span += ctx.xres) {
			auto p_mask = (p_a | p_b | p_c) >= 0;
			if (horizontal_or(p_mask)) {
				Vec8f p_z = approx_recipr(p_rz);

				auto z_candidate = (Vec8ui(0xFF80) - static_cast<Vec8ui>(roundi(g_zscale * p_z)));
				Vec8us z_existing_c;
				z_existing_c.load_a(span.zbuffer);
				auto z_existing = extend(z_existing_c);

				auto zmask = z_candidate > z_existing;

				p_mask &= zmask;

				if (horizontal_or(p_mask)) {
					*(__m128i*)span.zbuffer = _mm_blendv_epi8(*(__m128i*)span.zbuffer, compress(z_candidate), compress(Vec8ui(p_mask)));
					Vec8i u = roundi(p_uz * p_z * UScaleFactor);
					Vec8i v = roundi(p_vz * p_z * VScaleFactor);

					Vec8i tu = packed_tile_u(u, LogHeight, t_umask_swizzled);
					Vec8i tv = packed_tile_v(v, t_vmask);

					auto p_offset = tu + tv;
					auto packedTxtrData = v8_TxtrIdMask | p_offset;
					_mm256_maskstore_ps(span.txtr, *(__m256i*)(&p_mask), *(__m256*)(&packedTxtrData));

					// TODO: Pack texture const(id, miplevel), uv in u32 and write to gbuffer
					// Calculate normals
					// Calculate world space position
					// auto texture0_samples = gather(Vec8ui(p_offset), TextureAddr, p_mask);
					// _mm256_maskstore_ps((float*)span, *(__m256i*)(&p_mask), *(__m256*)(&texture_samples));
				}
			}

			p_rz += Vec8f(drzdy);
			p_uz += Vec8f(duzdy);
			p_vz += Vec8f(dvzdy);

			p_a += Vec8i(tile.dady);
			p_b += Vec8i(tile.dbdy);
			p_c += Vec8i(tile.dcdy);
		}
	}

	
	void rasterize_triangle(const Vertex& v1, const Vertex& v2, const Vertex& v3) {
		// FIXME: raster conventions (it is doing floor right now)
		const int tile_mx = clampedX(std::min({ v1.PX, v2.PX, v3.PX })) / TILE_SIZE;
		const int tile_Mx = clampedX(std::max({ v1.PX, v2.PX, v3.PX })) / TILE_SIZE;
		const int tile_my = clampedY(std::min({ v1.PY, v2.PY, v3.PY })) / TILE_SIZE;
		const int tile_My = clampedY(std::max({ v1.PY, v2.PY, v3.PY })) / TILE_SIZE;

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
						.uz0 = (v1.UZ + (x * TILE_SIZE - v1.PX) * duzdx + (y * TILE_SIZE - v1.PY) * duzdy),
						.vz0 = (v1.VZ + (x * TILE_SIZE - v1.PX) * dvzdx + (y * TILE_SIZE - v1.PY) * dvzdy)
					};
					apply_exact(tile);
				}
			}
		}
	}

};

} // namespace meka

extern meka::GBuffer *g_gbuffer;

inline void SetGBuffer(meka::GBuffer *gbuffer) {
	// Initalize GBuffer
	g_gbuffer = gbuffer;
}

inline void Mekalele(Face* F, Vertex** V, dword numVerts, dword miplevel) {
	//for (dword i = 0; i < numVerts; ++i) {
	//	float z = 1.0f / V[i]->RZ;
	//	V[i]->U = V[i]->UZ * z;
	//	V[i]->V = V[i]->VZ * z;
	//}
	meka::TileRasterizerCtx ctx = {
		.miplevel = miplevel,
		.Txtr = F->Txtr->Txtr,
		.V = V,
		.xres = XRes,
		.yres = YRes,
		.zbuffer = reinterpret_cast<uint16_t *>(VPage + PageSize),
	};
	meka::TileRasterizer r(*g_gbuffer, ctx);

	Vertex vc[12];

	for (dword i = 0; i < numVerts; ++i) {
		vc[i] = *V[i];
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
		r.duzdx = im[0] * (v2.UZ - v1.UZ) + im[1] * (v3.UZ - v1.UZ);
		r.duzdy = im[2] * (v2.UZ - v1.UZ) + im[3] * (v3.UZ - v1.UZ);
		r.dvzdx = im[0] * (v2.VZ - v1.VZ) + im[1] * (v3.VZ - v1.VZ);
		r.dvzdy = im[2] * (v2.VZ - v1.VZ) + im[3] * (v3.VZ - v1.VZ);

		r.umask = (1 << r.LogWidth) - 1;
		r.vmask = (1 << r.LogHeight) - 1;

		r.rasterize_triangle(v1, v2, v3);
	}
}
