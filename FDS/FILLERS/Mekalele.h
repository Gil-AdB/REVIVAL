#pragma once

#include <algorithm>

#include "Base/FDS_DECS.H"
#include "Base/FDS_VARS.H"
#include "Base/RenderTarget.h"
#include "Base/CameraContext.h"
#include "F4Vec.h"

#include "TheOtherBarry.h"
#include "ClipperTileRect.h"

//#include <intrin.h>
#include "simde/x86/avx2.h"
#include <simd/vectorclass.h>
#include <cassert>
#include <array>
#include <vector>
#include <iostream>
#include "SimdHelpers.h"

#include "Base/Scene.h"
#include "Base/FeatureFlags.h"

namespace meka {
using u8  = uint8_t;
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
	// Static-shadow lightmap address: meshLMId(16) | faceIdx(16).
	// meshLMId is an index into Scene::staticLMTable (0 = no lightmap,
	// sentinel; the deferred kernel falls back to per-pixel cube tap).
	// Mekalele writes a constant value per face (same meshLMId & faceIdx
	// for every pixel in the triangle).
	std::vector<u32> lightmapMF;
	// Static-shadow lightmap barycentric (s, t): s(8) | t(8). Encoded as
	// fixed-point fraction of barycentric weight on vertex B (s) and C
	// (t). Per-pixel; kernel does bilinear lookup into the lightmap.
	std::vector<u16> lightmapST;
	// Per-pixel 16-bit ShadowMatID — the receiver identity the deferred
	// kernel feeds to CubeShadow_Sample's PolyId path. Mekalele stamps
	// the same value across every pixel of one face (resolved per-face
	// from F->ShadowMatID / F->Txtr->ShadowMatID / Txtr->ID+1). Lets
	// greets's wall split push ~2600 distinct shadow groups through
	// without exceeding the 8-bit matID encoded in `txtr`. Optional —
	// when empty, the receiver falls back to `uint16_t(matID + 1)`.
	std::vector<u16> shadowMatID;
	// Per-pixel 8-bit planar-mirror identity. Used by DEMO/GreetsMirror's
	// Per-pixel mirror ownership, WRITTEN by Mekalele's commit path
	// from ctx.faceOwnerMirrorId. Read by the deferred lighting pass
	// to gate omnis per pixel (originals only light pmid=0, base
	// clones only light pmid=baseId, compound clones only light
	// pmid=compoundId). Foreground geometry committing inside a
	// mirror's projected mask overrides this back to 0 so it's lit
	// normally — that's the z-correct way to fix occluded mirror
	// walls. Optional plane (empty on scenes with no mirrors).
	std::vector<u8> mirrorId;
	// Per-pixel mirror GATE mask. Filled by the pre-pass (StampMirror-
	// Masks) and held IMMUTABLE during rasterization — the per-pixel
	// inner loop tests gb.mirrorMask against Face::mirrorMaskTag to
	// decide whether a clone face is allowed to commit at this pixel.
	// Kept separate from mirrorId because the commit path mutates
	// mirrorId; if the gate read mirrorId we'd get order-dependent
	// behaviour where a later face's gate test sees an earlier face's
	// ownerMirrorId instead of the original pre-stamp, and A's clones
	// could fail their gate after a compound clone committed there
	// (visible as "objects in mirror disappear, but floor remains").
	std::vector<u8> mirrorMask;
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

// 8-wide oct_encode. Mirrors the scalar oct_encode_u16 exactly (same
// L1 normalize, z<0 fold, ×127 quantize, [-128,127] clamp). Output is
// 8 packed u16 values in the low 16 bits of each 32-bit lane; callers
// store via `_mm256_store_si256` then cast/copy to u16 per lane (or
// pack down further if writing densely is needed).
//
// Replaces the per-lane scalar call from Mekalele's apply_exact hot
// loop — that scalar fallback was the #1 hot line in the rasterizer
// profile (~10% of total CPU on default city; ~30% of Mekalele).
inline __m256i oct_encode_u16_x8(__m256 nx, __m256 ny, __m256 nz) {
    const __m256 vSignMask = _mm256_set1_ps(-0.0f);
    const __m256 vZero     = _mm256_setzero_ps();
    const __m256 vOne      = _mm256_set1_ps(1.0f);
    const __m256 vNegOne   = _mm256_set1_ps(-1.0f);
    const __m256 v127      = _mm256_set1_ps(127.0f);

    const __m256 absX = _mm256_andnot_ps(vSignMask, nx);
    const __m256 absY = _mm256_andnot_ps(vSignMask, ny);
    const __m256 absZ = _mm256_andnot_ps(vSignMask, nz);
    const __m256 sumAbs = _mm256_add_ps(absX, _mm256_add_ps(absY, absZ));
    const __m256 invL1  = _mm256_div_ps(vOne, sumAbs);
    __m256 ox = _mm256_mul_ps(nx, invL1);
    __m256 oy = _mm256_mul_ps(ny, invL1);

    // z<0 fold: ox/oy reshuffled into the "outside" octant pattern.
    const __m256 zNeg   = _mm256_cmp_ps(nz, vZero, _CMP_LT_OQ);
    const __m256 absOX  = _mm256_andnot_ps(vSignMask, ox);
    const __m256 absOY  = _mm256_andnot_ps(vSignMask, oy);
    const __m256 sgnX   = _mm256_blendv_ps(vNegOne, vOne,
                            _mm256_cmp_ps(ox, vZero, _CMP_GE_OQ));
    const __m256 sgnY   = _mm256_blendv_ps(vNegOne, vOne,
                            _mm256_cmp_ps(oy, vZero, _CMP_GE_OQ));
    const __m256 fx = _mm256_mul_ps(_mm256_sub_ps(vOne, absOY), sgnX);
    const __m256 fy = _mm256_mul_ps(_mm256_sub_ps(vOne, absOX), sgnY);
    ox = _mm256_blendv_ps(ox, fx, zNeg);
    oy = _mm256_blendv_ps(oy, fy, zNeg);

    // Quantize: _mm256_cvtps_epi32 rounds to nearest under default
    // MXCSR (matches std::round-via-int for the values produced here:
    // ox/oy ∈ [-1,1] × 127 → [-127, 127], well below int32 round-half
    // ambiguity). Then clamp into the signed byte range.
    __m256i qx = _mm256_cvtps_epi32(_mm256_mul_ps(ox, v127));
    __m256i qy = _mm256_cvtps_epi32(_mm256_mul_ps(oy, v127));
    const __m256i cmin = _mm256_set1_epi32(-128);
    const __m256i cmax = _mm256_set1_epi32(127);
    qx = _mm256_max_epi32(qx, cmin);
    qx = _mm256_min_epi32(qx, cmax);
    qy = _mm256_max_epi32(qy, cmin);
    qy = _mm256_min_epi32(qy, cmax);

    // Pack: u16((qx & 0xff) | ((qy & 0xff) << 8)) per lane, kept in
    // the low 16 bits of each 32-bit lane for easy scalar extract.
    const __m256i mask8 = _mm256_set1_epi32(0xFF);
    qx = _mm256_and_si256(qx, mask8);
    qy = _mm256_and_si256(qy, mask8);
    return _mm256_or_si256(qx, _mm256_slli_epi32(qy, 8));
}

// Pack the low 16 bits of 8 lanes of a __m256i (uint32-per-lane) into 8
// contiguous uint16s in a __m128i. Used in the FULL-coverage store path
// of apply_exact: oct-encoded normals / tangents and packed lightmap ST
// all live in the low 16 bits of an 8-wide uint32 lane, and the G-buffer
// planes they target are uint16[]. _mm_packus_epi32 saturates to uint16
// which is fine here — every producer already clamps to its valid range
// before this packs.
inline __m128i pack_lo16_x8(__m256i v) {
    __m128i lo = _mm256_castsi256_si128(v);
    __m128i hi = _mm256_extracti128_si256(v, 1);
    return _mm_packus_epi32(lo, hi);
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
	// Per-tile origin of OrigBary*RZ (lightmap path). Same family as uz0/vz0:
	// linearly interpolated across the tile then divided by per-pixel Z to
	// recover object-space bary on the original face's (A, B, C).
	float obBZ0, obCZ0;
};

struct TileRasterizerCtx {
	Vertex** V;
	i32 xres, yres;
	Texture* Txtr;     // for LogWidth/LogHeight UV scaling math
	dword matID;       // packed into mat32; what the deferred lighting pass uses to look up Material*. NOT Texture::ID — different per-scene number space.
	dword miplevel;
	u16 *zbuffer;
	float zScale;      // was: g_zscale global. Per-pass depth scalar.

	// Static-shadow lightmap addressing (zero = no lightmap → skip the
	// G-buffer plane writes; deferred kernel falls back to per-pixel
	// cube shadow tap). Set per-face by the Mekalele dispatcher:
	//   lmMeshId  = F->ParentTri->staticLMMeshId       (0..65535)
	//   lmFaceIdx = (F - F->ParentTri->Faces)          (0..65535)
	u16 lmMeshId  = 0;
	u16 lmFaceIdx = 0;

	// Per-face resolved 16-bit ShadowMatID. Stamped into every pixel of
	// this face's GBuffer::shadowMatID plane so the deferred lighting
	// kernel's PolyId cube-shadow path can compare receiver identity
	// without bouncing through the 8-bit matID in `txtr`. Resolved by
	// the Mekalele dispatcher with priority:
	//   1. F->ShadowMatID  (per-face override — greets wall split)
	//   2. F->Txtr->ShadowMatID (per-material override — greets hull
	//      merge)
	//   3. uint16_t(F->Txtr->ID + 1) (legacy matID+1 fallback)
	u16 shadowMatId = 0;

	// Per-face planar-mirror tag, broadcast from Face::mirrorMaskTag.
	// Roles depending on how the face was dispatched:
	//   * 0  → not a mirror face. Inner loop ignores the mirrorId plane
	//          entirely; rasterizer writes normally.
	//   * 1..255, face is a CLONE face: inner loop tests
	//          gb.mirrorId[pixel] == mirrorTag and masks off any pixel
	//          where the owning mirror's wall hasn't been stamped.
	//   * 1..255, face is the mirror SURFACE (stamped by the mask-only
	//          pre-pass dispatcher): inner loop WRITES this value into
	//          gb.mirrorId[pixel] instead of doing the masked-write
	//          normal path. Controlled by the rasterizer dispatch (see
	//          MekaleleMaskOnly), not by a separate field on the ctx.
	u8 mirrorTag = 0;
	// Per-face mirror owner id, broadcast from Face::ownerMirrorId.
	// The commit path writes this byte into gb.mirrorId for every
	// pixel that survives p_mask, so the post-rasterization plane
	// reflects which mirror context actually won at each pixel —
	// foreground commits land 0 over the 2D pre-stamp, clones land
	// their m.id. Read by the deferred lighting kernel's per-pixel
	// light filter.
	u8 faceOwnerMirrorId = 0;
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
	u32 *lightmapMF;
	u16 *lightmapST;
	u16 *shadowMatID;
	u8  *mirrorId;
	const u8 *mirrorMask;
	u16 *zbuffer;

	GBufferSpan &operator+=(i32 offset) {
		normal += offset;
		tangent += offset;
		txtr += offset;
		if (lightmapMF) lightmapMF += offset;
		if (lightmapST) lightmapST += offset;
		if (shadowMatID) shadowMatID += offset;
		if (mirrorId) mirrorId += offset;
		if (mirrorMask) mirrorMask += offset;
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
		// Lightmap planes are also optional — allocated only when
		// --shadow-lightmap is on (or when any scene has populated a
		// staticLMTable). Inner loop checks before writing.
		u32 *lmMFPtr = gbuffer.lightmapMF.empty()
			? nullptr
			: gbuffer.lightmapMF.data() + offset;
		u16 *lmSTPtr = gbuffer.lightmapST.empty()
			? nullptr
			: gbuffer.lightmapST.data() + offset;
		// shadowMatID plane is optional (allocated when shadows or
		// lightmaps are on). When null, the deferred kernel falls back
		// to `uint16_t(matID + 1)` decoded from `txtr`.
		u16 *shadowMatIDPtr = gbuffer.shadowMatID.empty()
			? nullptr
			: gbuffer.shadowMatID.data() + offset;
		// mirrorId plane: allocated only when a scene actually uses
		// planar mirrors (DEMO/GreetsMirror's allocator). When null,
		// the inner-loop mask check below short-circuits and clone
		// faces commit normally.
		u8 *mirrorIdPtr = gbuffer.mirrorId.empty()
			? nullptr
			: gbuffer.mirrorId.data() + offset;
		const u8 *mirrorMaskPtr = gbuffer.mirrorMask.empty()
			? nullptr
			: gbuffer.mirrorMask.data() + offset;
		return {
			gbuffer.normal.data() + offset,
			tangentPtr,
			gbuffer.txtr.data() + offset,
			lmMFPtr,
			lmSTPtr,
			shadowMatIDPtr,
			mirrorIdPtr,
			mirrorMaskPtr,
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
	// Per-pixel OrigBary*RZ gradients (lightmap path). Same perspective-
	// correct interpolation as UZ/VZ: transport (OrigBary * RZ) linearly
	// in screen space, divide by per-pixel RZ to recover object-space
	// bary on the original face. Stamped at scene init + interpolated by
	// the clipper, so for any post-clip triangle the rasterizer recovers
	// the correct bary on the face's (A, B, C) without needing F's screen
	// positions (which can be junk if a vertex is behind the camera).
	float dobBdx, dobBdy;
	float dobCdx, dobCdy;

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
	// Inside=true: dispatcher proved that every pixel in this 8x8 tile is
	// strictly inside the triangle — no need for the per-row edge mask, and
	// the per-pixel p_a/p_b/p_c counters can be skipped entirely. The Z test
	// still runs (it depends on the depth buffer); when Z passes for every
	// lane we additionally take a FULL-row vector-store path that replaces
	// the per-lane scatter into the G-buffer planes with one 128-bit (u16
	// planes) or 256-bit (lightmapMF u32 plane) store per plane.
	template<bool Inside>
	void apply_exact(const meka::Tile& tile) {
		auto span = GBufferSpan::of(gbuffer, ctx, tile.x * TILE_SIZE, tile.y * TILE_SIZE);

		TScreenCoord a0 = tile.a0;
		TScreenCoord b0 = tile.b0;
		TScreenCoord c0 = tile.c0;

		// Pre-compute reciprocal of the per-triangle barycentric sum
		// (= 2A in subpixel units; constant across the whole triangle,
		// so any pixel works — tile origin used here for convenience).
		// Pixels' barycentric weights are p_a/sum (A), p_b/sum (B),
		// p_c/sum (C). s, t map to weights of B and C respectively.
		const int32_t triSum = int32_t(a0) + int32_t(b0) + int32_t(c0);
		const float invTriSum = (triSum != 0) ? (1.0f / float(triSum)) : 0.0f;
		const u32 packedLmMF = (u32(ctx.lmMeshId) << 16) | u32(ctx.lmFaceIdx);
		const bool wantLm = (ctx.lmMeshId != 0) && (span.lightmapMF != nullptr)
		                    && (span.lightmapST != nullptr);
		// Per-face shadow mat id stamp. Same value across every pixel
		// of this face — precomputed once in the Mekalele dispatcher
		// (see MekaleleImpl) and broadcast here. Plane is optional;
		// kernel falls back to matID+1 when the plane is empty.
		const u16 packedShadowMatId = ctx.shadowMatId;
		const bool wantShadowMatId = (span.shadowMatID != nullptr);
		// Diagnostic gates. Cached once at function entry so the per-row
		// hot loop reads a register, not the flag registry. See
		// FeatureFlags.def for the on/off contract.
		const bool useFullStore = fds::FeatureFlags::rast_full_store();

		// Edge counters are only used for the per-row mask. Inside=true
		// skips them entirely; the compiler removes the increments below.
		Vec8i p_a, p_b, p_c;
		if constexpr (!Inside) {
			p_a = v8_from_arith_seq(a0, tile.dadx);
			p_b = v8_from_arith_seq(b0, tile.dbdx);
			p_c = v8_from_arith_seq(c0, tile.dcdx);
		}

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
		// OrigBary*RZ across the tile row. Divided by per-pixel Z below
		// (reusing p_z computed for UV) to recover object-space bary on
		// the original face. Zero-init when not in lightmap path.
		Vec8f p_obBZ = wantLm ? v8_from_arith_seq(tile.obBZ0, dobBdx) : Vec8f(0.0f);
		Vec8f p_obCZ = wantLm ? v8_from_arith_seq(tile.obCZ0, dobCdx) : Vec8f(0.0f);

		for (int32_t y = 0; y != TILE_SIZE; ++y, a0 += tile.dady, b0 += tile.dbdy, c0 += tile.dcdy, span += ctx.xres) {
			// Inside=true skips the edge mask entirely. p_mask seeded
			// all-ones; subsequent `&= zmask` does the work.
			Vec8ib p_mask;
			if constexpr (Inside) {
				p_mask = Vec8ib(true);
			} else {
				p_mask = (p_a | p_b | p_c) >= 0;
			}
			const bool entered_row = Inside ? true : barry::any_lane_set(p_mask);
			if (entered_row) {
				Vec8f p_z = approx_recipr(p_rz);

				auto z_candidate = (Vec8ui(0xFF80) - static_cast<Vec8ui>(roundi(ctx.zScale * p_z)));
				Vec8us z_existing_c;
				z_existing_c.load_a(span.zbuffer);
				auto z_existing = extend(z_existing_c);

				auto zmask = z_candidate > z_existing;

				p_mask &= zmask;

				// Planar-mirror per-pixel clip. When this face is a
				// clone tagged with a mirror id, mask off any lane
				// whose gb.mirrorId doesn't match — the wall pre-pass
				// only stamped IDs at pixels covered by that mirror's
				// wall, so clone pixels outside the wall's screen
				// footprint get rejected here. Compiled into the same
				// p_mask the rest of the path already respects; no
				// extra branch needed downstream.
				if (ctx.mirrorTag != 0 && span.mirrorMask != nullptr) {
					// Gate against the IMMUTABLE pre-stamp plane, not
					// the mutable ownership plane: a previously-
					// committed face's ownerMirrorId on mirrorId would
					// flip the gate on us mid-tile and reject A's
					// clones after a compound clone committed first.
					alignas(8) u8 maskBytes[8];
					std::memcpy(maskBytes, span.mirrorMask, 8);
					const __m128i packed = _mm_loadl_epi64((const __m128i*)maskBytes);
					const Vec8i pixelIds = Vec8i(_mm256_cvtepu8_epi32(packed));
					const Vec8ib mirrorMask = (pixelIds == Vec8i(int(ctx.mirrorTag)));
					p_mask &= mirrorMask;
				}

				if (barry::any_lane_set(p_mask)) {
					*(__m128i*)span.zbuffer = _mm_blendv_epi8(*(__m128i*)span.zbuffer, compress(z_candidate), compress(Vec8ui(p_mask)));
					Vec8i u = roundi(p_uz * p_z * UScaleFactor);
					Vec8i v = roundi(p_vz * p_z * VScaleFactor);

					Vec8i tu = packed_tile_u(u, LogHeight, t_umask_swizzled);
					Vec8i tv = packed_tile_v(v, t_vmask);

					auto p_offset = tu + tv;
					auto packedTxtrData = v8_TxtrIdMask | p_offset;
					_mm256_maskstore_ps(span.txtr, *(__m256i*)(&p_mask), *(__m256*)(&packedTxtrData));

					// Per-pixel nlerp + octahedral pack. Vec normalize +
					// vec oct_encode_u16_x8 — formerly scalar per-lane.
					// Tangent: vec Gram-Schmidt + vec encode, with a
					// degenerate-lane mask that zeros tangent for lanes
					// where T became parallel to N after interpolation.
					const Vec8f n2 = p_nx*p_nx + p_ny*p_ny + p_nz*p_nz;
					const Vec8f vInvN = approx_rsqrt(n2);
					const Vec8f vnx = p_nx * vInvN;
					const Vec8f vny = p_ny * vInvN;
					const Vec8f vnz = p_nz * vInvN;
					alignas(32) uint32_t normalEnc[8];
					_mm256_store_si256((__m256i*)normalEnc,
						oct_encode_u16_x8(*(const __m256*)&vnx,
						                  *(const __m256*)&vny,
						                  *(const __m256*)&vnz));
					alignas(32) uint32_t tangentEnc[8];
					alignas(32) int32_t  tValid[8];
					if (wantTangent) {
						const Vec8f tDotN = p_tx*vnx + p_ty*vny + p_tz*vnz;
						Vec8f vtx = p_tx - vnx * tDotN;
						Vec8f vty = p_ty - vny * tDotN;
						Vec8f vtz = p_tz - vnz * tDotN;
						const Vec8f tLen2 = vtx*vtx + vty*vty + vtz*vtz;
						const Vec8f vInvT = approx_rsqrt(tLen2);
						vtx = vtx * vInvT;
						vty = vty * vInvT;
						vtz = vtz * vInvT;
						_mm256_store_si256((__m256i*)tangentEnc,
							oct_encode_u16_x8(*(const __m256*)&vtx,
							                  *(const __m256*)&vty,
							                  *(const __m256*)&vtz));
						// Lane-valid mask: tLen2 > 1e-12. Degenerate lanes
						// get tangent = 0 (lighting kernel falls back to
						// an arbitrary ⟂N reference).
						const Vec8f vEps = 1e-12f;
						Vec8i(tLen2 > vEps).store_a(tValid);
					}
					// Lightmap (s, t) per-lane: divide the perspective-correct
					// transport (OrigBary*RZ) by per-pixel Z to recover the
					// object-space bary on the original face. Same machinery
					// as UV (UZ/VZ + per-pixel /Z), so the per-pixel cost is
					// 8-wide SIMD across the lanes. OrigBary is stamped at
					// scene init + clipper-interpolated perspective-correctly,
					// so this is correct for clipped sub-triangles too — no
					// dependency on F->A/B/C screen positions (which can be
					// junk when a face vertex sits behind the camera).
					if (useFullStore && barry::all_lanes_set(p_mask)) {
						// FULL-coverage row: replace 8x per-lane scatter
						// with one vector store per G-buffer plane. This is
						// the dominant body of any tile that covers a wall
						// or floor; partial-coverage stays on the scatter
						// path below.
						const __m128i n16 = pack_lo16_x8(
							_mm256_load_si256((const __m256i*)normalEnc));
						_mm_storeu_si128((__m128i*)span.normal, n16);
						if (wantTangent) {
							const __m128i t16Raw = pack_lo16_x8(
								_mm256_load_si256((const __m256i*)tangentEnc));
							// tValid is Vec8i (32-bit per lane); sign-saturate
							// pack to 8x16 — true (-1) -> 0xFFFF, false -> 0.
							const __m256i tValid32 =
								_mm256_load_si256((const __m256i*)tValid);
							const __m128i tValid16 = _mm_packs_epi32(
								_mm256_castsi256_si128(tValid32),
								_mm256_extracti128_si256(tValid32, 1));
							_mm_storeu_si128((__m128i*)span.tangent,
								_mm_and_si128(t16Raw, tValid16));
						}
						if (wantLm) {
							// SIMD version of the per-lane sB/tB clamp +
							// pack. roundi rounds to nearest under the
							// engine-wide RTNE mode (mode is fixed in
							// FPU_LPrecision); diverges from the scalar
							// `int(x*255 + 0.5)` only at exact .5 cases.
							const Vec8i v255(255);
							const Vec8i v0(0);
							Vec8i sBv = roundi(p_obBZ * p_z * 255.0f);
							Vec8i tBv = roundi(p_obCZ * p_z * 255.0f);
							sBv = max(v0, min(v255, sBv));
							tBv = max(v0, min(v255, tBv));
							const Vec8i stv = sBv | (tBv << 8);
							_mm_storeu_si128((__m128i*)span.lightmapST,
								pack_lo16_x8(stv));
							_mm256_storeu_si256((__m256i*)span.lightmapMF,
								_mm256_set1_epi32(int32_t(packedLmMF)));
						}
						if (wantShadowMatId) {
							_mm_storeu_si128((__m128i*)span.shadowMatID,
								_mm_set1_epi16(int16_t(packedShadowMatId)));
						}
						if (span.mirrorId) {
							// 8 bytes (one per lane). The committed value
							// overrides the 2D pre-stamped mask at every
							// pixel Mekalele actually rasterizes, so the
							// post-raster plane is z-correct face
							// ownership for the deferred light filter.
							_mm_storel_epi64((__m128i*)span.mirrorId,
								_mm_set1_epi8((char)ctx.faceOwnerMirrorId));
						}
					} else {
						alignas(32) int32_t mask_l[8];
						Vec8i(p_mask).store_a(mask_l);
						alignas(32) float obBLane[8], obCLane[8];
						if (wantLm) {
							(p_obBZ * p_z).store_a(obBLane);
							(p_obCZ * p_z).store_a(obCLane);
						}
						for (int lane = 0; lane < 8; ++lane) {
							if (!mask_l[lane]) continue;
							span.normal[lane] = uint16_t(normalEnc[lane]);
							if (wantTangent) {
								span.tangent[lane] = tValid[lane]
									? uint16_t(tangentEnc[lane]) : uint16_t(0);
							}
							if (wantLm) {
								int sB = int(obBLane[lane] * 255.0f + 0.5f);
								int tB = int(obCLane[lane] * 255.0f + 0.5f);
								if (sB < 0)   sB = 0;
								if (sB > 255) sB = 255;
								if (tB < 0)   tB = 0;
								if (tB > 255) tB = 255;
								span.lightmapMF[lane] = packedLmMF;
								span.lightmapST[lane] = uint16_t(sB | (tB << 8));
							}
							if (wantShadowMatId) {
								span.shadowMatID[lane] = packedShadowMatId;
							}
							if (span.mirrorId) {
								span.mirrorId[lane] = ctx.faceOwnerMirrorId;
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
			if (wantLm) {
				p_obBZ += Vec8f(dobBdy);
				p_obCZ += Vec8f(dobCdy);
			}

			if constexpr (!Inside) {
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
		int tile_my = std::max(_ctr.tile_my_lo,
			clampedY(std::min({ v1.PY, v2.PY, v3.PY })) / TILE_SIZE);
		int tile_My = std::min(_ctr.tile_my_hi,
			clampedY(std::max({ v1.PY, v2.PY, v3.PY })) / TILE_SIZE);
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
					// Conservative tile-inside test: take the corner that
					// minimises each edge function over the 8x8 tile and
					// check all three are non-negative. Uses TILE_SIZE
					// (not TILE_SIZE-1) for symmetry with max_*; the only
					// cost is a few extra Inside=false specializations on
					// near-edge tiles — never a correctness risk.
					//
					// Sign-bit pack via OR (mirror of the max_* check):
					// `(x | y | z) >= 0` ⟺ all three sign bits are zero
					// ⟺ all three are non-negative. AND would only fire
					// when ALL THREE are negative, which is the wrong
					// direction (it would silently flag boundary tiles
					// — where only one edge is partially crossed — as
					// Inside and skip their per-row edge mask).
					TScreenCoord min_a = a0 + ((dadx < 0) ? dadx * TILE_SIZE : 0) + ((dady < 0) ? dady * TILE_SIZE : 0);
					TScreenCoord min_b = b0 + ((dbdx < 0) ? dbdx * TILE_SIZE : 0) + ((dbdy < 0) ? dbdy * TILE_SIZE : 0);
					TScreenCoord min_c = c0 + ((dcdx < 0) ? dcdx * TILE_SIZE : 0) + ((dcdy < 0) ? dcdy * TILE_SIZE : 0);
					const bool tile_inside = (min_a | min_b | min_c) >= 0;
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
						.obBZ0 = v1.OrigBaryB * v1.RZ + dx * dobBdx + dy * dobBdy,
						.obCZ0 = v1.OrigBaryC * v1.RZ + dx * dobCdx + dy * dobCdy,
					};
					const bool useInsideTpl = fds::FeatureFlags::rast_inside_template();
					if (tile_inside && useInsideTpl) apply_exact<true>(tile);
					else                              apply_exact<false>(tile);
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
	// Per-face static-shadow lightmap addressing. ParentTri is set in
	// Transform_Objects when the face hits FList; staticLMMeshId is set
	// by LightmapBake_Static (0 = dynamic mesh, no lightmap). The face
	// index is stamped on F itself (F->MeshFaceIdx) because the F we
	// see here is the FList clone, not the original T->Faces[i].
	//
	// Clip handling: OrigBary is stamped per-vertex at scene init and
	// interpolated perspective-correctly by the clipper, so the rasterizer
	// recovers correct per-pixel bary on the original face for ANY
	// post-clip vertex set. No clipped/unclipped distinction needed here.
	const uint16_t lmMeshId  = F->ParentTri ? F->ParentTri->staticLMMeshId : uint16_t(0);
	const uint16_t lmFaceIdx = F->MeshFaceIdx;
	// Per-face shadow mat id — priority mirrors ShadowMap's resolution
	// (F->ShadowMatID > F->Txtr->ShadowMatID > Txtr->ID+1). Stamped
	// into gb.shadowMatID across every pixel of this face so the
	// deferred PolyId cube-shadow path reads it without going through
	// the 8-bit matID encoded in `txtr`.
	uint16_t shadowMatId;
	if (F->ShadowMatID != 0) {
		shadowMatId = F->ShadowMatID;
	} else if (F->Txtr->ShadowMatID != 0) {
		shadowMatId = F->Txtr->ShadowMatID;
	} else {
		shadowMatId = uint16_t(F->Txtr->ID + 1);
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
		.lmMeshId  = lmMeshId,
		.lmFaceIdx = lmFaceIdx,
		.shadowMatId = shadowMatId,
		.mirrorTag = F->mirrorMaskTag,
		.faceOwnerMirrorId = F->ownerMirrorId,
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
		// OrigBary*RZ gradient — same im[] solve as UZ/VZ. OrigBary is
		// stamped at scene init (A→(0,0), B→(1,0), C→(0,1)) and clip-
		// interpolated perspective-correctly, so for any (v1, v2, v3)
		// triangle the rasterizer sees post-clipping, the linear function
		// (OrigBary*RZ)(x, y) is exact. Divide by per-pixel RZ to get
		// object-space bary on the original face.
		const float obBZ1 = v1.OrigBaryB * v1.RZ;
		const float obBZ2 = v2.OrigBaryB * v2.RZ;
		const float obBZ3 = v3.OrigBaryB * v3.RZ;
		const float obCZ1 = v1.OrigBaryC * v1.RZ;
		const float obCZ2 = v2.OrigBaryC * v2.RZ;
		const float obCZ3 = v3.OrigBaryC * v3.RZ;
		r.dobBdx = im[0] * (obBZ2 - obBZ1) + im[1] * (obBZ3 - obBZ1);
		r.dobBdy = im[2] * (obBZ2 - obBZ1) + im[3] * (obBZ3 - obBZ1);
		r.dobCdx = im[0] * (obCZ2 - obCZ1) + im[1] * (obCZ3 - obCZ1);
		r.dobCdy = im[2] * (obCZ2 - obCZ1) + im[3] * (obCZ3 - obCZ1);

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

// Explicit-instantiation declarations — see RasterizerInst.cpp for the
// canonical TU and the bug-history comment. Forces consumers to link
// against the one centrally-emitted body instead of generating per-TU
// copies that can diverge under per-file -ffp-contract differences.
extern template void MekaleleImpl<MekaleleTarget::Opaque>           (Face*, Vertex**, dword, dword, const fds::RenderTarget&, const fds::CameraContext&);
extern template void MekaleleImpl<MekaleleTarget::TransparentFront> (Face*, Vertex**, dword, dword, const fds::RenderTarget&, const fds::CameraContext&);
extern template void MekaleleImpl<MekaleleTarget::TransparentBack>  (Face*, Vertex**, dword, dword, const fds::RenderTarget&, const fds::CameraContext&);
