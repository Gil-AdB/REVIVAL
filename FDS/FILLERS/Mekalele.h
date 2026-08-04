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

// Reverse-peel mode flag for the transparent rasterizer (see the documented
// extern further down). Forward-declared here so meka::TileRasterizer's
// apply_exact, defined inside the namespace below, can read it.
extern thread_local bool g_xparPeelReverse;

#include <atomic>
#include <cstdlib>

namespace meka {

// Diagnostic (FDS_MIRROR_CLAMP_STATS=1): lanes rejected by the clone
// wall-depth clamp this frame + clone lanes that passed the tag gate at
// all (denominator — distinguishes "clamp inert" from "no clones here").
inline std::atomic<long long> g_mirrorClampCount{0};
inline std::atomic<long long> g_mirrorCloneLanes{0};
inline const bool g_mirrorClampStats = std::getenv("FDS_MIRROR_CLAMP_STATS") != nullptr;
// A/B escape (validation): disable the clone wall-depth clamp.
inline const bool g_mirrorNoWallZClamp = std::getenv("FDS_NO_MIRROR_WALLZ_CLAMP") != nullptr;

#if FDS_DEV
// --pom_shell_census (DIAGNOSTIC, default OFF, DEV BUILD ONLY): which height-map
// MIP each shell face's march actually sampled. The bake's mip is a flag (one
// number); the shell's is per FACE — the albedo miplevel the mipmap-via-
// subdivision clipper chose, unless --pom_height_mip pins it — so it can only be
// counted at raster time. Printed once at teardown; silent when nothing was
// counted. Kept behind FDS_DEV because even a flag-gated branch in this
// dispatcher perturbs the shell march's last bit through the inliner (measured:
// 5 px at 1/255 at t=6097), and the release build must stay byte-exact.
struct PomShellMipHist {
	std::atomic<long long> n[16][16] = {};
	~PomShellMipHist() {
		long long tot = 0;
		for (int m = 0; m < 16; ++m)
			for (int k = 0; k < 16; ++k) tot += n[m][k].load();
		if (!tot) return;
		for (int m = 0; m < 16; ++m) {
			long long sub = 0;
			for (int k = 0; k < 16; ++k) sub += n[m][k].load();
			if (!sub) continue;
			std::fprintf(stderr, "[POM-SHELL-CENSUS-MIP] matID=%d shell faces=%lld by height mip:", m, sub);
			for (int k = 0; k < 16; ++k) {
				const long long c = n[m][k].load();
				if (c) std::fprintf(stderr, " mip%d=%lld(%.1f%%)", k, c, 100.0*double(c)/double(sub));
			}
			std::fprintf(stderr, "\n");
		}
	}
};
inline PomShellMipHist g_pomShellMipHist;
#endif

using u8  = uint8_t;
using u16 = uint16_t;
using i32 = int32_t;
using u32 = uint32_t;
constexpr const i32 TILE_SIZE = 8;

// Continuous per-face mip FRACTION (∈ [0,1] toward mip+1), exported by
// FrustumClipper::MiplevelClipper before each filler() call. The fixed
// RasterFunc signature can't carry it, so it rides a thread_local (one
// clipper instance per tile worker thread, matching the threading model).
// Read by MekaleleImpl into TileRasterizerCtx::mipFrac for trilinear
// (FDS_TEXTURE_FILTER == 2) blending. 0.0 when unknown / non-mip paths.
extern thread_local float g_tlsMipFrac;
using TScreenCoord = i32;

struct GBuffer {
	// Octahedral-packed shading normal: 16-bit x, 16-bit y (oct 16.16),
	// decoded to (nx, ny, nz) on a unit sphere in the lighting pass. The
	// lighting pass reconstructs view-space position from ZPage16 +
	// screen XY, so we don't carry position here.
	// Was 8.8 (u16): a ~0.8deg quantization cell is fine for diffuse N.L
	// but NOT for specular reflection under camera ROTATION — the view-
	// space code freezes inside its cell while viewToWorld keeps turning,
	// so the reconstructed WORLD normal drifts at rotation rate and snaps
	// back a cell later. The reflected ray doubles the error: measured
	// 1.2–3.1deg/frame sawtooth on city glass (ENVTRACE dNv=0 rows with
	// dRW>0) ≈ 25–65 px/frame of reflection judder. 16.16 cells are
	// ~0.003deg — numerically gone.
	std::vector<u32> normal;
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
	// Per-pixel FILTERED diffuse albedo (BGRA8), sampled at RASTER time
	// with the sub-texel UV fraction that only exists before the swizzle
	// pack in `txtr`. Written by Mekalele's apply_exact only when
	// FDS_TEXTURE_FILTER > 0 (bilinear / trilinear); the deferred lighting
	// kernel reads this instead of point-sampling the packed texel address,
	// which kills the sub-texel facade crawl. Empty (point-sample) by
	// default — allocated by EngineGBuffer_Resize only when the flag is on.
	// Metal/rough/AO/normal maps keep using the `txtr` suv address.
	std::vector<u32> albedo;
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
	// DIAGNOSTIC per-pixel FACE identity (--face_id_dump, default OFF; empty
	// otherwise so the hot loop skips the write). matID is far too coarse to
	// answer "which surface owns this pixel" — every greets wall shares one id,
	// so a (matID before, matID after) classification cannot distinguish "the
	// neighbouring wall won the pixel" from "the same wall shaded differently",
	// and it certainly cannot say whether the winner is a face that ought to
	// have been occluded. This plane carries a stable per-TRIANGLE key:
	//   bits 31..4 = the Face*'s address >> 4 (28 bits, unique per authored
	//                polygon for any heap inside a 4 GB window)
	//   bits  3..0 = the fan sub-triangle index within that Face
	// The Face pointer survives the frustum clipper AND the mipmap poly-split
	// (both pass Face* through unchanged), so the key names the AUTHORED polygon
	// no matter how it was subdivided on the way to the tile. Snapshot dumps it
	// beside the z16/matID planes and prints a resolution table (key -> material,
	// mesh, face index, world plane, vertex positions) so a suspect pixel can be
	// traced to a real polygon, with key collisions reported explicitly.
	std::vector<u32> faceId;
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
	// Per-pixel wall depth for the mirror gate, same 16-bit encoding
	// as the Z buffer (0xFF80 - z*zscale; larger = closer). Written by
	// StampMirrorMasks alongside mirrorMask. The behind-mirror cull
	// rejects a real face's pixel only when it lies BEYOND the wall
	// surface at that pixel — without the depth, the 2D stamp (which
	// is z-ignorant) carved behind-plane geometry out of real walls
	// even when the mirror itself was fully occluded by a nearer wall
	// ("mirror visible through walls"). 0 = no wall here.
	std::vector<u16> mirrorMaskZ;
};

// Octahedral encode: unit vector (nx, ny, nz) -> 16-bit (8.8 signed).
// Quantization error is sub-degree at 8 bits per axis, well below
// what visibly matters for diffuse lighting. The lighting pass does
// the inverse via oct_decode_u16.
// NB: no live callers today (oct_encode_u16_x8 is the hot path); kept as the
// scalar reference / fallback. It uses the SAME single-lane reciprocal estimate
// (_mm_rcp_ss, portable via simde) as the x8 path's _mm256_rcp_ps — proven
// bit-identical over the unit-normal domain — so the two encode paths emit
// identical oct bytes (no path-dependent seam if the scalar fallback is reused).
inline u16 oct_encode_u16(float nx, float ny, float nz) {
	const float sumAbs = std::fabs(nx) + std::fabs(ny) + std::fabs(nz);
	float invL1 = _mm_cvtss_f32(_mm_rcp_ss(_mm_set_ss(sumAbs)));
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
    // invL1 = raw approximate reciprocal (~12-bit), no Newton-Raphson. sumAbs is
    // well-conditioned (∈ [1, √3] for a unit normal); the estimate quantizes to
    // within ~1 LSB of the exact-divide oct byte (measured clean-room: greets
    // max 2/255 on 0.001%, city max 2 on 0.016%, chase max 1 — nothing visible).
    // The scalar oct_encode_u16 reference uses the SAME estimate (_mm_rcp_ss,
    // proven bit-identical to _mm256_rcp_ps over the domain) so both paths agree.
    const __m256 invL1  = _mm256_rcp_ps(sumAbs);
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

// 8-wide oct encode at 16.16 (shading NORMAL plane). Same fold as the u16
// path; two differences: x32767 quantize into full 32-bit lane codes
// (qx:16 | qy:16), and one Newton-Raphson refinement on the L1 reciprocal —
// the raw ~12-bit rcp estimate was fine for 8-bit codes but would eat the
// extra precision here (±0.0004 relative ≈ ±13 of 32767 codes).
// Tangents stay on oct_encode_u16_x8 (nmap-only, 8.8 is plenty).
inline __m256i oct_encode_u32_x8(__m256 nx, __m256 ny, __m256 nz) {
    const __m256 vSignMask = _mm256_set1_ps(-0.0f);
    const __m256 vZero     = _mm256_setzero_ps();
    const __m256 vOne      = _mm256_set1_ps(1.0f);
    const __m256 vNegOne   = _mm256_set1_ps(-1.0f);
    const __m256 vQ        = _mm256_set1_ps(32767.0f);

    const __m256 absX = _mm256_andnot_ps(vSignMask, nx);
    const __m256 absY = _mm256_andnot_ps(vSignMask, ny);
    const __m256 absZ = _mm256_andnot_ps(vSignMask, nz);
    const __m256 sumAbs = _mm256_add_ps(absX, _mm256_add_ps(absY, absZ));
    __m256 invL1 = _mm256_rcp_ps(sumAbs);
    // One NR step: invL1 *= (2 - sumAbs*invL1)  → ~23-bit accurate.
    invL1 = _mm256_mul_ps(invL1,
        _mm256_fnmadd_ps(sumAbs, invL1, _mm256_set1_ps(2.0f)));
    __m256 ox = _mm256_mul_ps(nx, invL1);
    __m256 oy = _mm256_mul_ps(ny, invL1);

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

    __m256i qx = _mm256_cvtps_epi32(_mm256_mul_ps(ox, vQ));
    __m256i qy = _mm256_cvtps_epi32(_mm256_mul_ps(oy, vQ));
    const __m256i cmin = _mm256_set1_epi32(-32768);
    const __m256i cmax = _mm256_set1_epi32(32767);
    qx = _mm256_max_epi32(qx, cmin);
    qx = _mm256_min_epi32(qx, cmax);
    qy = _mm256_max_epi32(qy, cmin);
    qy = _mm256_min_epi32(qy, cmax);
    const __m256i mask16 = _mm256_set1_epi32(0xFFFF);
    qx = _mm256_and_si256(qx, mask16);
    qy = _mm256_and_si256(qy, mask16);
    return _mm256_or_si256(qx, _mm256_slli_epi32(qy, 16));
}

// Inverse of oct_encode_u32_x8's per-lane code (normal plane).
inline void oct_decode_u32(u32 packed, float &nx, float &ny, float &nz) {
	int qx = int16_t(packed & 0xffff);
	int qy = int16_t((packed >> 16) & 0xffff);
	float ox = qx * (1.0f / 32767.0f);
	float oy = qy * (1.0f / 32767.0f);
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

// Inverse of oct_encode_u16. Output is unit-length (mod quantization
// error). Used by the TANGENT plane and legacy callers.
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
	// Per-tile origin of ShellH*RZ (--pom_shell). Same family as uz0/vz0:
	// interpolated linearly across the tile, divided by per-pixel Z to recover
	// the shell ENTRY height at the pixel.
	float shz0;
};

struct TileRasterizerCtx {
	Vertex** V;
	i32 xres, yres;
	Texture* Txtr;     // for LogWidth/LogHeight UV scaling math
	dword matID;       // packed into mat32; what the deferred lighting pass uses to look up Material*. NOT Texture::ID — different per-scene number space.
	dword miplevel;
	u16 *zbuffer;
	float zScale;      // was: g_zscale global. Per-pass depth scalar.
	// Parallax (offset) mapping (--parallax). heightData = the material's
	// HeightMap mip[miplevel] (same tiled layout as Txtr, so the same swizzled
	// texel address indexes it), or null = parallax off. The apply loop
	// reconstructs the view dir from cntrE*/invFOV* + per-pixel screen pos to
	// nudge the UV before the texel pack. 8-bit single-channel (1 byte/texel).
	const byte *heightData = nullptr;
	float parallaxStrength  = 0.0f;
	// --parallax_max_offset: clamp the final parallax UV offset (post single-
	// shift / post march) to at most this many TEXELS from the geometric UV.
	// 0 = no clamp (byte-identical). Bounds the diagonal-streak over-drive at
	// strength >> the tuned value. Set by the dispatcher from the flag.
	float parallaxMaxOffset = 0.0f;
	float cntrEX = 0.0f, cntrEY = 0.0f, invFOVX = 0.0f, invFOVY = 0.0f;
	// Depth-peel floor (transparent passes only; nullptr for opaque). A
	// fragment is accepted only when z_candidate < peelFloor[i] — strictly
	// farther than the nearest already-peeled layer — so successive passes
	// walk away from the camera. nullptr or all-0xFFFF = no gate (legacy).
	u16 *peelFloor = nullptr;

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
	// Bitmask of mirror ids this face is BEHIND (Face::behindMirrorMask).
	// The commit rejects any lane whose gb.mirrorMask id has its bit set
	// here, so behind-mirror real geometry can't leak through a
	// (transparent, no-opaque-Z) mirror's footprint. 0 for the common
	// case (face not behind any mirror) → the gate is a no-op.
	u32 faceBehindMirrorMask = 0;

	// Per-face: does this face's material carry a tangent-space normal map?
	// Only then does the deferred kernel read gb.tangent, so faces without
	// one skip BOTH the per-face tangent-gradient setup and the per-pixel
	// tangent G-buffer write (the kernel reads the matID-gated material, so a
	// stale tangent left in the plane is never sampled). Set by the dispatcher.
	bool writeTangent = true;

	// Continuous mip fraction for trilinear albedo filtering (snapshot of
	// meka::g_tlsMipFrac at dispatch — ∈ [0,1] toward mip+1). 0 outside the
	// trilinear path / when unknown.
	float mipFrac = 0.0f;

	// Material carries a heightmap (parallax offset mapping). When set, the
	// texture-filter albedo plane is NOT written for this face and the
	// deferred kernel keeps its suv-based point fetch — the rasterizer
	// parallax path shifts the UV before the swizzle pack, and pre-resolving
	// a filtered albedo here would change greets' parallax look. Filtered
	// parallax (bilinear at the shifted UV) is a follow-up. City has no
	// heightmaps, so the shimmer target is unaffected.
	bool materialHasHeightMap = false;

	// Naive linear occlusion march step count (the default --parallax_pom path;
	// the dispatcher sets this to parallax_pom() unless --parallax_pom_cone). 0 =
	// single-shift (byte-identical); N>0 = N-tap linear march of the tangent-space
	// view ray that records the first rayH<=Hs crossing (anchored, no swim).
	int pomSpikeSteps = 0;

	// --pom_ref_march (DIAGNOSTIC, default OFF): step count of the CONVERGED
	// BRUTE-FORCE reference march. 0 = off. When > 0 it REPLACES both the naive
	// and the cone march: N uniform steps down the whole slab plus a secant
	// solve on the bracketing pair, no LOD fade, no quarter-res sharing, no cone
	// map. It exists to build a ground-truth image of the height field's true
	// surface that neither the shipping march nor the tessellation bake is, so
	// both can be measured against something instead of against each other.
	int pomRefSteps = 0;

	// Tier-2 cone-step POM (--parallax_pom). coneData = the material's ConeMap
	// mip[miplevel] (8-bit, SAME tiled layout as heightData → same swizzled
	// address). pomSteps = the flag's max cone-safe steps (0 = off). When both
	// are set the march advances by the cone-safe distance c·gap/(c+dlen) each
	// step, converging onto the surface in a few taps. Takes precedence over the
	// naive pomSpikeSteps path. Null coneData / pomSteps==0 → single-shift.
	const byte *coneData = nullptr;
	int pomSteps = 0;
	// STEP-3 LOD: view-Z (units) past which the cone-march result fades toward
	// the tier-0 single shift; pure single-shift at ~2× this. 0 = no fade.
	float pomLodDist = 0.0f;
	// STEP-2 quarter-res offset field: march only even lanes/rows, reuse the UV
	// offset for the odd neighbours. 0 = full-res march.
	int pomQuarter = 0;
	// Cone march: binary-search refinement iterations after the cone steps
	// bracket the first ray/height crossing. Each iteration is one height
	// gather (no cone gather). 0 = no refine (return the 'below' bracket sample).
	int pomRefine = 6;
	// Cone relaxation: multiply the baked (conservative) cone ratio to widen the
	// steps so the ray BRACKETS the crossing in fewer taps. >1 trades a small
	// skip risk for speed; the binary refine recovers the crossing inside any
	// bracket it lands. 1 = the raw conservative bake.
	float pomRelax = 1.0f;
	// Decode scale for one ConeMap byte: byte × coneUnit = the cone ratio the
	// march steps by (UV distance per unit normalized height), relaxation
	// already folded in. The legacy coarse bake (MakeConeMap) encodes over
	// [0, kPomConeMax]; the exact per-texel bake (--pom_cone_exact) encodes over
	// [0, kPomConeExactMax], which is ~an order finer because the real cone
	// ratios of a 1024² stone map live near 1/1024 per unit height, not near 4.
	// One field so the kernel never has to know WHICH bake produced the map.
	float coneUnit = kPomConeMax * (1.0f / 255.0f);
	// --pom_cone_min_step (default 0 = off): floor on the cone step, expressed
	// as TEXELS of lateral advance. A cone byte of 0 makes the cone step
	// c·gap/(c+dlen) exactly ZERO, so the march FREEZES and the lane falls back
	// to the un-shifted entry UV (for the shell: the flat lid). A minimum
	// lateral advance is the EGSR'24 "artifact-free minimum step size" and
	// bounds the number of steps needed to cross the slab.
	float pomConeMinStepTexels = 0.0f;
	// --pom_march_earlyout: break the march loop once EVERY lane has bracketed
	// its crossing. Byte-exact by construction (a bracketed lane's dt is already
	// forced to 0 and all of its bracket state is frozen behind `search`), so
	// this is purely the cost of the steps nobody needed.
	bool pomEarlyOut = false;
	// --pom_depth_write (S1a, docs/DISPLACEMENT_RESEARCH.md): write the marched
	// intersection's view depth to the Z buffer instead of the flat plane's.
	// Set by the dispatcher ONLY when the flag is on AND a march is configured
	// for this face (heightData + naive or cone steps), so apply_exact can key
	// its deferred Z store off this alone. The per-face world scale lives in
	// TileRasterizer::pomDepthWorldAmp (set per triangle with the gradients).
	bool pomDepthWrite = false;
	// --pom_shell (S1b, docs/S1_PIXEL_DISPLACEMENT_PLAN.md): this face belongs
	// to a POM SHELL. The rastered surface is the LID (top of the relief slab,
	// Vertex::ShellH = 1); the march runs from the per-pixel interpolated entry
	// height DOWN through the slab along the TRUE view ray, and a lane whose
	// ray leaves the authored face's UV domain before crossing the height field
	// is DISCARDED (the silhouette). Set only when the material carries a
	// PomShellUvAmp (i.e. PomShell_Build ran) and a march is configured; implies
	// pomDepthWrite. false = the legacy centered (h−0.5) march, byte-identical.
	bool  pomShell = false;
	// The slab's amplitude for the full 0..1 height range in UV units, straight
	// from Material::PomShellUvAmp — the amplitude the GEOMETRY was built with,
	// so the lid height and the march's height range agree by construction. Its
	// world equivalent is pomShellUvAmp × world-per-UV = the per-triangle
	// pomDepthWorldAmp, which the shell depth write reuses.
	float pomShellUvAmp = 0.0f;
	// --pom_shell_cap: bound on 1/(V·N) in the true-ray march (grazing guard).
	float pomShellCap = 8.0f;
	// --pom_shell_domain: off = lid + marched depth but NO lateral-exit
	// discard. The on/off pair IS the discard viz (the changed pixels are
	// exactly the discarded ones) at zero hot-loop cost.
	bool  pomShellDomain = true;
	// --pom_shell_base_clip: BASE-FOOTPRINT clip (the lid-overhang fix). The lid
	// is a rigid outward translation of the patch, so at a patch border it
	// rasterizes screen area the AUTHORED plane never covered, and rays there
	// march INWARD and hit legally — stone floating past the wall's end. This
	// tests the view ray's crossing of the AUTHORED plane (h = 0.5) against the
	// same UV domain: outside ⇒ this pixel is lid overhang ⇒ kill it. Matches the
	// tessellation bake, which pins its patch-border verts to zero displacement
	// and therefore never covers past the authored footprint either.
	bool  pomShellBaseClip = true;
	// --pom_normal / --pom_normal_strength: replace the G-buffer normal with the
	// marched height field's own surface normal (see the write site).
	bool  pomNormal = false;
	float pomNormalStrength = 1.0f;
	// --pom_shell_base_clip_raw: use the UNCAPPED 1/(V·N) for the base-clip ray
	// instead of the march's capped one. Diagnostic A/B (see the flag).
	bool  pomShellBaseClipRaw = false;
	// AUTHORED face UV bounding box (from Face::U1..V3 — NOT the post-clip /
	// post-poly-split vertex set, both of which pass the Face through
	// unchanged, so this stays the authored patch's domain at every
	// subdivision level). The shell's lateral-exit test compares the marched
	// hit UV against it. For an axis-aligned rectangular UV chart either
	// triangle of the quad spans the whole quad's box, so no quad pairing is
	// needed and the shared diagonal is never treated as a boundary.
	float shellUMin = 0.0f, shellUMax = 0.0f;
	float shellVMin = 0.0f, shellVMax = 0.0f;
	// --pom_shell_merge_uv: the SIBLING boxes of this face's patch — the other
	// patches on the same plane whose UV rects abut it (4 floats each,
	// uMin,uMax,vMin,vMax; own box excluded, it is shellU/VMin/Max above). The
	// lateral-exit test passes if the hit is inside the own box OR any sibling,
	// i.e. the domain is the UNION of the boxes and NOT their bounding box — an
	// authoring cut through one physical surface (greets' doorway thresholds)
	// stops discarding, a real opening between coplanar patches still does.
	// Evaluated only for lanes that failed the own box (horizontal_and early-out).
	const float *shellSibs = nullptr;
	int shellSibCount = 0;
	// --pom_viz: replace the albedo with the height field sampled at the FINAL
	// (post-march) UV, grayscale — the parallax result made directly visible
	// (block domes, mortar cuts, march terracing/banding). Debug only; rides
	// the texture-filter albedo path, so it needs --texture_filter >= 1.
	bool pomViz = false;
	// --pom_mip_viz: tint the albedo by THIS face's miplevel (color hash) so
	// the mipmap-via-subdivision sub-face boundaries — the diagonal-seam cause —
	// read directly. Rides the same filtered-albedo path as pomViz.
	bool pomMipViz = false;

	// Height-map addressing for the parallax gathers, threaded SEPARATELY from
	// the albedo mip (ctx.Txtr / ctx.miplevel). The height map shares the
	// albedo's tiled layout but --pom_height_mip can PIN it to a fixed level so
	// the sampled height stays continuous across sub-faces at different albedo
	// mips (the diagonal-seam fix). Legacy (pin off) sets these EXACTLY to the
	// albedo mip's LogWidth/LogHeight/scale/mask → byte-identical. Used by every
	// height (and cone) gather; the final albedo/normal fetch still uses the
	// albedo mip. Only meaningful when heightData != nullptr.
	int32_t  heightLogW  = 0;
	int32_t  heightLogH  = 0;
	float    heightUScale = 0.0f;
	float    heightVScale = 0.0f;
	int32_t  heightUmaskSwizzled = 0;
	int32_t  heightVmask = 0;
};

// Debug (snapshot FDS_DUMP_TXTR path): optional per-pixel dump of the finalized
// parallax UV (uf,vf), 2 floats/pixel, row stride g_pomDbgStride px. nullptr =
// off. Lets a headless A/B measure the MARCH output (which UV each pixel landed
// on) directly, bypassing lighting/post amplification. Set by the snapshot.
extern float *g_pomDbgUV;
extern int    g_pomDbgStride;
extern int    g_pomDbgH;

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
	u32 *normal;
	u16 *tangent;
	u32 *txtr;
	u32 *albedo;      // filtered BGRA (nullptr when FDS_TEXTURE_FILTER off)
	u32 *lightmapMF;
	u16 *lightmapST;
	u16 *shadowMatID;
	u32 *faceId;      // DIAGNOSTIC per-triangle key (nullptr = --face_id_dump off)
	u8  *mirrorId;
	const u8 *mirrorMask;
	const u16 *mirrorMaskZ;
	u16 *zbuffer;
	u16 *peelFloor;   // depth-peel floor (nullptr = no gate)

	GBufferSpan &operator+=(i32 offset) {
		normal += offset;
		tangent += offset;
		txtr += offset;
		if (albedo) albedo += offset;
		if (lightmapMF) lightmapMF += offset;
		if (lightmapST) lightmapST += offset;
		if (shadowMatID) shadowMatID += offset;
		if (faceId) faceId += offset;
		if (mirrorId) mirrorId += offset;
		if (mirrorMask) mirrorMask += offset;
		if (mirrorMaskZ) mirrorMaskZ += offset;
		zbuffer += offset;
		if (peelFloor) peelFloor += offset;
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
		// Filtered-albedo plane: optional (allocated only when
		// FDS_TEXTURE_FILTER > 0). nullptr → inner loop skips the write.
		u32 *albedoPtr = gbuffer.albedo.empty()
			? nullptr
			: gbuffer.albedo.data() + offset;
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
		// DIAGNOSTIC face-id plane: allocated only under --face_id_dump.
		u32 *faceIdPtr = gbuffer.faceId.empty()
			? nullptr
			: gbuffer.faceId.data() + offset;
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
		const u16 *mirrorMaskZPtr = gbuffer.mirrorMaskZ.empty()
			? nullptr
			: gbuffer.mirrorMaskZ.data() + offset;
		return {
			gbuffer.normal.data() + offset,
			tangentPtr,
			gbuffer.txtr.data() + offset,
			albedoPtr,
			lmMFPtr,
			lmSTPtr,
			shadowMatIDPtr,
			faceIdPtr,
			mirrorIdPtr,
			mirrorMaskPtr,
			mirrorMaskZPtr,
			ctx.zbuffer + offset,
			ctx.peelFloor ? ctx.peelFloor + offset : nullptr
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

// 8-wide 4-tap bilinear texel fetch in the swizzled tile layout. Mirrors
// TheOtherBarry's NORMAL_BILINEAR block: floor(u,v) + fractional weights,
// 4 corner gathers via packed_tile_u/packed_tile_v (which wrap at the
// texture edge through their &vmask / &swizzled_umask masking, exactly as
// the point-sample roundi path does), blended in 16-bit. `ufTexels`/
// `vfTexels` are UV already scaled to texel units for THIS mip. Returns
// packed BGRA8 per lane. `vbits` = LogHeight for this mip.
inline Vec8ui bilinear_sample_x8(Vec8f ufTexels, Vec8f vfTexels,
                                 int32_t vbits, uint32_t umaskSwizzled,
                                 int32_t vmask, const void *texData,
                                 Vec8ib p_mask) {
	Vec8i u0 = truncatei(ufTexels), v0 = truncatei(vfTexels);
	Vec8i u1 = u0 + 1,              v1 = v0 + 1;
	const Vec8i wu = truncatei((ufTexels - to_float(u0)) * 255.0f);
	const Vec8i wv = truncatei((vfTexels - to_float(v0)) * 255.0f);
	Vec8i tu0 = packed_tile_u(u0, vbits, umaskSwizzled);
	Vec8i tu1 = packed_tile_u(u1, vbits, umaskSwizzled);
	Vec8i tv0 = packed_tile_v(v0, (uint32_t)vmask);
	Vec8i tv1 = packed_tile_v(v1, (uint32_t)vmask);
	const Vec32us s00 = extend(Vec32uc(gather(Vec8ui(tu0 + tv0), texData, p_mask)));
	const Vec32us s10 = extend(Vec32uc(gather(Vec8ui(tu1 + tv0), texData, p_mask)));
	const Vec32us s01 = extend(Vec32uc(gather(Vec8ui(tu0 + tv1), texData, p_mask)));
	const Vec32us s11 = extend(Vec32uc(gather(Vec8ui(tu1 + tv1), texData, p_mask)));
	// Weights 0..255 replicated to all 4 bytes; each blend s*(255-w)+s*w
	// stays <= 255*255 → fits u16.
	const Vec32us wU = extend(Vec32uc(Vec8ui(wu) * Vec8ui(0x01010101u)));
	const Vec32us wV = extend(Vec32uc(Vec8ui(wv) * Vec8ui(0x01010101u)));
	const Vec32us iU = Vec32us(255) - wU;
	const Vec32us top = (s00 * iU + s10 * wU) >> 8;
	const Vec32us bot = (s01 * iU + s11 * wU) >> 8;
	const Vec32us iV = Vec32us(255) - wV;
	return Vec8ui(compress((top * iV + bot * wV) >> 8));
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

		// Texture filtering (FDS_TEXTURE_FILTER: 0 point, 1 bilinear, 2
		// trilinear). When on, apply_exact samples the diffuse texel with
		// the sub-texel fraction and writes the blended BGRA into
		// gbuffer.albedo. albedoTex0 is THIS mip's texel table (dword
		// BGRA); albedoTex1 is mip+1 for the trilinear cross-mip lerp.
		texFilter  = fds::FeatureFlags::texture_filter();
		albedoTex0 = reinterpret_cast<const u32*>(ctx.Txtr->Mipmap[ctx.miplevel]);
		LogWidth1  = LogWidth  - 1;
		LogHeight1 = LogHeight - 1;
		albedoTex1 = ((ctx.miplevel + 1) < ctx.Txtr->numMipmaps
		              && LogWidth1 >= 0 && LogHeight1 >= 0)
		    ? reinterpret_cast<const u32*>(ctx.Txtr->Mipmap[ctx.miplevel + 1])
		    : nullptr;
		mipFrac = std::min(std::max(ctx.mipFrac, 0.0f), 1.0f);
	}

	// miplevel (4 bits) | txtr id (8 bit) | zeroes (20 bit)
	Vec8i v8_TxtrIdMask;
	int32_t LogWidth;
	int32_t LogHeight;
	float UScaleFactor;
	float VScaleFactor;
	// Texture filtering state (see constructor).
	int texFilter = 0;
	const u32 *albedoTex0 = nullptr;
	const u32 *albedoTex1 = nullptr;
	int32_t LogWidth1 = 0, LogHeight1 = 0;
	float mipFrac = 0.0f;
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
	// Per-pixel shell-entry-height gradients (--pom_shell). Perspective-correct
	// like UZ/VZ: transport ShellH*RZ linearly in screen space, divide by
	// per-pixel RZ. Zero when the face isn't a shell face.
	float dshzdx, dshzdy;

	// --pom_depth_write: the marched relief's WORLD amplitude for the FULL
	// height range 0..1, per face = ctx.parallaxStrength (UV units, incl. the
	// material's ParallaxScale) × the face's world-units-per-UV-tile density
	// (geometric mean of |dP/du|, |dP/dv| from the triangle's view positions +
	// UVs — the same Lengyel solve tangents come from). Zero when the depth
	// write is off or the face's UV mapping is degenerate → flat depth.
	float pomDepthWorldAmp = 0.0f;
	// --poly_viz: this TRIANGLE's ownership colour (0 = viz off). Material id in
	// the hue, shell-lid vs not in the brightness, per-triangle hash in a small
	// jitter so triangle boundaries read. Set per triangle beside the amp above.
	uint32_t polyVizColor = 0;
	// --face_id_dump (DIAGNOSTIC, default OFF): this TRIANGLE's stable
	// identity key, written per pixel into GBuffer::faceId. bits 31..4 =
	// Face* >> 4, bits 3..0 = the fan sub-triangle index. 0 = plane not
	// allocated / write skipped. Set per triangle beside polyVizColor.
	uint32_t faceIdKey = 0;
	// --pom_shell: the slab amplitude expressed in UV units for THIS triangle
	// = Material::PomShellUvAmp × (world-per-UV-tile). The march's lateral travel
	// per unit height drop is this × the tangent-space view direction, so the
	// UV the march samples and the world height the geometry carries stay
	// consistent even where texel density varies between faces. Falls back to
	// ctx.parallaxStrength when the UV mapping is degenerate (w = 0).
	float pomShellUvAmp = 0.0f;

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
		// DIAGNOSTIC per-triangle face identity (--face_id_dump). Written on
		// exactly the same p_mask as every other G-buffer plane, so a lane the
		// shell discard killed leaves the id of whoever really won the pixel.
		const bool wantFaceId = (span.faceId != nullptr) && (faceIdKey != 0);
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

		// Texture filtering: sample the diffuse texel with the sub-texel
		// fraction and write the blended BGRA to gbuffer.albedo. Only when
		// the flag is on, the plane is allocated, and this mip has texels.
		// Heightmap (parallax) materials are INCLUDED (Tier 1, filtered
		// parallax): the parallax block above shifts uf/vf BEFORE this block,
		// so the bilinear sample below lands at the parallax-SHIFTED UV — the
		// float fraction is only present here at raster time. The kernel reads
		// gb.albedo for these too (its !Mat->HeightMap exclusion was dropped in
		// lockstep). texFilter==0 → this whole block is off → byte-identical.
		const bool wantAlbedo = ((texFilter > 0) || polyVizColor)
		                        && (span.albedo != nullptr)
		                        && (albedoTex0 != nullptr);
		// mip+1 masks for the trilinear cross-mip blend.
		const bool wantTri = wantAlbedo && (texFilter >= 2)
		                     && (albedoTex1 != nullptr) && (mipFrac > 0.0f);
		int32_t t1_vmask = wantTri ? ((1 << LogHeight1) - 1) : 0;
		int32_t t1_umask_swizzled = wantTri
		    ? swizzle_umask(LogHeight1, (1 << LogWidth1) - 1) : 0;
		// Per-mip trilinear weight replicated to all 4 bytes (0..255).
		const Vec32us triW = wantTri
		    ? extend(Vec32uc(Vec8ui(uint32_t(mipFrac * 255.0f) * 0x01010101u)))
		    : Vec32us(0);
		const Vec32us triIW = Vec32us(255) - triW;

		Vec8f p_rz = v8_from_arith_seq(tile.rz0, drzdx);
		Vec8f p_uz = v8_from_arith_seq(tile.uz0, duzdx);
		Vec8f p_vz = v8_from_arith_seq(tile.vz0, dvzdx);
		Vec8f p_nx = v8_from_arith_seq(tile.nx0, dnxdx);
		Vec8f p_ny = v8_from_arith_seq(tile.ny0, dnydx);
		Vec8f p_nz = v8_from_arith_seq(tile.nz0, dnzdx);
		const bool wantTangent = (span.tangent != nullptr) && ctx.writeTangent;
		// --pom_depth_write (S1a): the march below replaces the flat plane's Z
		// with the marched intersection depth. ctx.pomDepthWrite already
		// implies heightData + a configured march; wantTangent is the same
		// gate the march block itself runs under, so pomZ true ⟺ the march
		// runs ⟺ the Z store is DEFERRED to the end of the parallax block.
		const bool pomZ = ctx.pomDepthWrite && (ctx.heightData != nullptr)
		                  && wantTangent;
		// --pom_shell (S1b): the rastered surface is the shell LID. Same gate
		// as pomZ plus the material's shell arm; the dispatcher already forced
		// pomDepthWrite on for shell faces, so shell ⟹ pomZ.
		const bool shell = ctx.pomShell && pomZ;
		// ShellH*RZ across the tile row (only shell faces pay for it).
		Vec8f p_shz = shell ? v8_from_arith_seq(tile.shz0, dshzdx) : Vec8f(0.0f);
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

				// Depth-peel floor: accept only fragments STRICTLY farther
				// than the nearest already-peeled layer (z_candidate < floor),
				// so successive transparent passes walk away from the camera.
				// peelFloor is nullptr for opaque (skipped) and all-0xFFFF on
				// the first/only pass (no-op: z_candidate <= 0xFF80 < 0xFFFF).
				if (span.peelFloor) {
					Vec8us bound_c;
					bound_c.load_a(span.peelFloor);
					if (g_xparPeelReverse) {
						// Reverse depth peel (passes >= 2): keep the FARTHEST
						// fragment (smallest z_candidate; layer Z pre-cleared to
						// 0xFFFF), and only those NEARER than the layer already
						// composited this batch (z_candidate > ceiling). Successive
						// passes walk toward the camera; each composites over the
						// last → far-to-near WITHIN the (mesh, side) batch, which
						// is exactly the order the facing split + sort key expect.
						zmask = (z_candidate < z_existing) & (z_candidate > extend(bound_c));
					} else {
						// Legacy floor gate — no-op at passes==1 (all-0xFFFF).
						zmask &= (z_candidate < extend(bound_c));
					}
				}

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
					if (g_mirrorClampStats)
						g_mirrorCloneLanes.fetch_add(
							horizontal_count(Vec8ib(p_mask)),
							std::memory_order_relaxed);
					// Wall-depth clamp: a reflection lives BEHIND the
					// glass, so a clone lane strictly CLOSER than the
					// mirror wall's surface at that pixel (z encoding:
					// larger = closer) is physically impossible
					// reflected content. It happens when clone geometry
					// lands on the viewer's side of the plane — e.g. a
					// real wall that CROSSES the mirror plane reflects
					// back ONTO ITSELF, and the coincident twin Z-ties
					// with the original per raster order (the live
					// shimmer/moire "mirror seen through the wall" at
					// greets' corridor walls). Small tolerance (~4 cm at
					// greets' z scale) keeps content lying ON the glass
					// plane through both paths' interpolation rounding.
					if (span.mirrorMaskZ != nullptr && !g_mirrorNoWallZClamp) {
						Vec8us wallZ_c;
						wallZ_c.load(span.mirrorMaskZ);
						const Vec8ui wallZ = extend(wallZ_c);
						const Vec8ib nearer = Vec8ib(z_candidate > wallZ + 16);
						// FDS_MIRROR_CLAMP_STATS=1: count clamped lanes (was
						// the lane alive before the clamp?). Diagnostic only.
						if (g_mirrorClampStats)
							g_mirrorClampCount.fetch_add(
								horizontal_count(Vec8ib(p_mask) & nearer),
								std::memory_order_relaxed);
						p_mask &= ~nearer;
					}
				}
				// Behind-mirror clip for ORIGINAL faces. If this face is
				// behind one or more mirror planes (faceBehindMirrorMask
				// bits set), reject any lane sitting inside one of those
				// mirrors' screen footprints — the (transparent, no-
				// opaque-Z) mirror surface can't occlude it, so without
				// this it leaks through and beats the reflected clones
				// on Z. Per lane: bit[pixelMirrorId] of the face's mask.
				// _mm256_srlv_epi32 variable-shifts the broadcast mask by
				// each lane's id; for ids ≥ 32 the shift yields 0 (never
				// rejected), and id 0 (no mirror) tests bit 0 which is
				// never set since mirror ids start at 1.
				if (ctx.faceBehindMirrorMask != 0 && span.mirrorMask != nullptr) {
					alignas(8) u8 maskBytes[8];
					std::memcpy(maskBytes, span.mirrorMask, 8);
					const __m128i packed = _mm_loadl_epi64((const __m128i*)maskBytes);
					const Vec8i pixelIds = Vec8i(_mm256_cvtepu8_epi32(packed));
					const __m256i bcast = _mm256_set1_epi32(int(ctx.faceBehindMirrorMask));
					const __m256i bit   = _mm256_and_si256(
						_mm256_srlv_epi32(bcast, __m256i(pixelIds)),
						_mm256_set1_epi32(1));
					// behind == (bit != 0); keep lanes where bit == 0.
					Vec8ib cull = (Vec8i(bit) != Vec8i(0));
					// Depth-qualify the cull: only reject the lane if
					// the face pixel lies BEYOND the wall surface at
					// that pixel (z encoding: larger = closer, so
					// beyond == z_candidate < wallZ). The 2D stamp is
					// z-ignorant — a footprint can land on real walls
					// standing IN FRONT of an occluded mirror, and an
					// unconditional cull carved see-through holes in
					// them ("mirror visible through walls").
					if (span.mirrorMaskZ != nullptr) {
						Vec8us wallZ_c;
						wallZ_c.load(span.mirrorMaskZ);
						const Vec8ui wallZ = extend(wallZ_c);
						cull &= Vec8ib(z_candidate < wallZ);
					}
					p_mask &= ~cull;
				}

				if (barry::any_lane_set(p_mask)) {
					// pomZ defers this store to the end of the parallax block
					// below, where the flat z is replaced by the marched relief
					// depth. Same value+mask semantics otherwise; the Z TEST and
					// every gate above already ran against the flat z_candidate.
					if (!pomZ)
						*(__m128i*)span.zbuffer = _mm_blendv_epi8(*(__m128i*)span.zbuffer, compress(z_candidate), compress(Vec8ui(p_mask)));
					// UV (pre-scale, [0,1) tiled). Parallax (--parallax) nudges it
					// along the tangent-space view ray before the texel pack, so
					// albedo/normal/AO all sample the shifted texel.
					Vec8f uf = p_uz * p_z;
					Vec8f vf = p_vz * p_z;
					// --pom_normal: the G-buffer normal this pixel hands to the
					// deferred kernel. Defaults to the interpolated GEOMETRIC normal
					// (byte-identical); the march block below replaces it with the
					// marched height field's own surface normal when the flag is on.
					Vec8f pnX = p_nx, pnY = p_ny, pnZ = p_nz;
					if (ctx.heightData && wantTangent) {
						// Geometric (un-shifted) UV, kept for --parallax_max_offset:
						// the clamp bounds |final - geometric| in texels below.
						const Vec8f ufGeo = uf, vfGeo = vf;
						// Sample height at the un-offset UV. The height map has its
						// OWN mip addressing (ctx.height*), threaded separately from
						// the albedo mip so --pom_height_mip can pin it to a fixed
						// level (seam fix); legacy = the albedo mip's params exactly.
						// Low byte = grayscale height; scalar gather (no sub-32 SIMD).
						Vec8i hu0 = roundi(uf * ctx.heightUScale);
						Vec8i hv0 = roundi(vf * ctx.heightVScale);
						Vec8i haddr = packed_tile_u(hu0, ctx.heightLogH, ctx.heightUmaskSwizzled)
						            + packed_tile_v(hv0, ctx.heightVmask);
						alignas(32) int32_t aA[8]; haddr.store_a(aA);
						alignas(32) float hA[8];
						for (int k = 0; k < 8; ++k)
							hA[k] = float(ctx.heightData[aA[k]]) * (1.0f / 255.0f);
						Vec8f H; H.load_a(hA);
						// View-space position per lane (same reconstruction as the
						// deferred kernel): screen x = tile.x*TILE_SIZE + lane,
						// y = tile.y*TILE_SIZE + row. V points toward the camera.
						Vec8f sx = v8_from_arith_seq(float(tile.x * TILE_SIZE), 1.0f);
						float syf = float(tile.y * TILE_SIZE + y);
						Vec8f X = (sx - Vec8f(ctx.cntrEX)) * Vec8f(ctx.invFOVX) * p_z;
						Vec8f Y = (Vec8f(ctx.cntrEY) - Vec8f(syf)) * Vec8f(ctx.invFOVY) * p_z;
						Vec8f Z = p_z;
						Vec8f vinv = approx_rsqrt(X*X + Y*Y + Z*Z);
						Vec8f Vx = X * (-vinv), Vy = Y * (-vinv), Vz = Z * (-vinv);
						// Normalize interpolated view N and T; B = N×T.
						Vec8f nl = approx_rsqrt(p_nx*p_nx + p_ny*p_ny + p_nz*p_nz);
						Vec8f Nx = p_nx*nl, Ny = p_ny*nl, Nz = p_nz*nl;
						Vec8f tl = approx_rsqrt(p_tx*p_tx + p_ty*p_ty + p_tz*p_tz);
						Vec8f Tx = p_tx*tl, Ty = p_ty*tl, Tz = p_tz*tl;
						Vec8f Bx = Ny*Tz - Nz*Ty, By = Nz*Tx - Nx*Tz, Bz = Nx*Ty - Ny*Tx;
						// Tangent-space view xy (offset-limiting: no /Vz → stable at
						// grazing). Centre at h=0.5 so mid-height = no shift.
						Vec8f VtT = Vx*Tx + Vy*Ty + Vz*Tz;
						Vec8f VtB = Vx*Bx + Vy*By + Vz*Bz;
						// V·N per lane. Hoisted out of the depth-write block at the
						// end (which used to compute it) because the SHELL march needs
						// it for the true-ray direction. Same expression, same value.
						const Vec8f VtN = Vx*Nx + Vy*Ny + Vz*Nz;
						// ── shell parametrization (--pom_shell, S1b) ──────────
						// hEnter = the height the RASTERED surface sits at inside the
						// slab, interpolated PER PIXEL (1 = lid, 0.5 = a pinned border,
						// so a tapered lid marches from its true geometric height).
						// Legacy = the authored plane, 0.5, constant. hStart = where the
						// march begins: the entry point itself for the shell (the ray
						// enters through the lid), the field top for the legacy centered
						// march. rayScale = UV travelled per unit height DROP; the shell
						// uses the TRUE view ray (÷V·N, capped by --pom_shell_cap)
						// because offset limiting bounds lateral travel at `strength`
						// whatever the angle, and grazing lateral travel is exactly what
						// silhouettes are made of. Its amplitude comes from the GEOMETRY
						// (Material::PomShellUvAmp × world-per-UV), never from the live
						// strength flag, so march and lid can't disagree.
						const Vec8f hEnter = shell ? (p_shz * p_z) : Vec8f(0.5f);
						const Vec8f hStart = shell ? hEnter : Vec8f(1.0f);
						// UNCAPPED 1/(V·N): the geometry's true lateral travel per
						// unit height, which --pom_shell_base_clip needs (the lid's
						// screen overhang is pure geometry and knows nothing about
						// the march's grazing cap; capping it would under-clip
						// exactly where the overhang is worst).
						const Vec8f invVtNRaw = shell
						    ? (Vec8f(1.0f) / max(VtN, Vec8f(1.0f / 64.0f)))
						    : Vec8f(1.0f);
						const Vec8f invVtN = shell
						    ? min(invVtNRaw, Vec8f(ctx.pomShellCap))
						    : Vec8f(1.0f);
						const Vec8f rayScale = shell
						    ? (Vec8f(ctx.pomShellUvAmp) * invVtN)
						    : Vec8f(ctx.parallaxStrength);
						Vec8f hc  = (H - hEnter) * rayScale;
						uf += VtT * hc;
						vf += VtB * hc;
						// --pom_depth_write: crossing height the march landed on
						// (0.5 = the flat plane; <0.5 recess, >0.5 protrusion; for the
						// shell, hEnter = the lid it entered through). Stays at the
						// ENTRY height (flat depth) when no march ran on this row
						// (rowFar LOD skip) or a lane never bracketed a crossing.
						Vec8f pomHitH = hEnter;
						// --pom_shell: did this lane's ray actually CROSS the height
						// field inside the slab? Seeded true so a row that skipped the
						// march entirely (LOD rowFar) is never discarded.
						Vec8fb pomCrossed = Vec8fb(true);
						// Tier-2 CONE-STEP POM (--parallax_pom, docs/HEIGHTMAP_POM_PLAN.md):
						// relaxed cone stepping (Policarpo, GPU Gems 3 ch.18). Advance the
						// tangent-space view ray by the cone-safe distance c*gap/(c+dlen)
						// each step until it BRACKETS the first ray/height crossing (first
						// sample with rayH <= Hs), then binary-search-refine between the
						// last-above and first-below sample to LAND on the crossing. Fixes
						// the old under-converged march (which returned the final cone
						// position with no crossing detection -> collapsed to the single
						// shift). Same centered +-0.5*strength envelope + base recovery as
						// the naive spike -> directly comparable; converges to the naive-inf
						// crossing in far fewer taps than the naive linear march.
						// ── CONVERGED REFERENCE MARCH (--pom_ref_march) ──────
						// DIAGNOSTIC ONLY, default OFF. Neither the shipping march
						// nor the tessellation bake is ground truth for the relief:
						// the cone march converges in few taps but through a baked,
						// approximate cone map with a step budget; the bake carries
						// the height field only at its subdivision lattice, samples
						// it at a low mip, and pins patch borders to zero. So the
						// inventory needs a third thing to measure BOTH against —
						// this: N uniform steps down the entire slab, no cone map,
						// no LOD fade, no quarter-res lane sharing, plus one secant
						// solve on the bracketing pair so the landing is exact to
						// float precision rather than to a step. At N in the high
						// hundreds the residual is the height map's own point
						// sampling, not the march. Cost is ~N gathers per covered
						// pixel — which is exactly why it is a reference, not a mode.
						if (ctx.pomRefSteps > 0) {
							const int   N     = ctx.pomRefSteps;
							const float invNf = 1.0f / float(N);
							const Vec8f dU = VtT * rayScale;
							const Vec8f dV = VtB * rayScale;
							const Vec8f baseU = uf - VtT * hc;   // un-shifted geometric UV
							const Vec8f baseV = vf - VtB * hc;
							Vec8f curU = baseU + dU * (hStart - hEnter);
							Vec8f curV = baseV + dV * (hStart - hEnter);
							Vec8f rayH = hStart;
							const Vec8f stepH = hStart * Vec8f(invNf);
							auto sampleH = [&](const Vec8f &U, const Vec8f &V) {
								Vec8i mu = roundi(U * ctx.heightUScale);
								Vec8i mv = roundi(V * ctx.heightVScale);
								Vec8i ma = packed_tile_u(mu, ctx.heightLogH, ctx.heightUmaskSwizzled)
								         + packed_tile_v(mv, ctx.heightVmask);
								alignas(32) int32_t mAd[8]; ma.store_a(mAd);
								alignas(32) float mH[8];
								for (int k = 0; k < 8; ++k)
									mH[k] = float(ctx.heightData[mAd[k]]) * (1.0f / 255.0f);
								Vec8f Hs; Hs.load_a(mH);
								return Hs;
							};
							// f = rayH - h(uv): > 0 above the surface, <= 0 inside it.
							Vec8f prevU = curU, prevV = curV, prevH = rayH;
							Vec8f prevF = rayH - sampleH(curU, curV);
							Vec8f hitU = curU, hitV = curV, hitH = rayH;
							Vec8fb found = Vec8fb(false);
							for (int s = 0; s < N; ++s) {
								curU -= dU * stepH;
								curV -= dV * stepH;
								rayH -= stepH;
								const Vec8f f = rayH - sampleH(curU, curV);
								const Vec8fb hitNow = (f <= Vec8f(0.0f)) & (~found);
								// Secant on the bracketing pair: both endpoints lie on
								// the SAME ray, so a linear solve in the step parameter
								// is simultaneously the solve in UV and in height.
								const Vec8f den = max(prevF - f, Vec8f(1e-12f));
								const Vec8f tt  = min(max(prevF / den, Vec8f(0.0f)), Vec8f(1.0f));
								hitU = select(hitNow, prevU - dU * stepH * tt, hitU);
								hitV = select(hitNow, prevV - dV * stepH * tt, hitV);
								hitH = select(hitNow, prevH - stepH * tt, hitH);
								found |= hitNow;
								const Vec8fb go = ~found;
								prevU = select(go, curU, prevU);
								prevV = select(go, curV, prevV);
								prevH = select(go, rayH, prevH);
								prevF = select(go, f, prevF);
								// --pom_march_earlyout, BYTE-EXACT: with every lane found,
								// hitNow is false and `go` is false, so hit*/prev* are all
								// frozen and cur*/rayH are never read after the loop.
								if (ctx.pomEarlyOut && horizontal_and(found)) break;
							}
							uf = hitU;
							vf = hitV;
							if (pomZ)
								pomHitH = select(found, hitH, hEnter);
							// A uniform march over the WHOLE slab that never crossed has
							// genuinely passed under all the stone — unlike the cone march
							// there is no "ran out of steps" ambiguity here, which is
							// precisely what makes this usable as a reference.
							if (shell)
								pomCrossed = found;
						} else if (ctx.pomSteps > 0 && ctx.coneData) {
							// STEP-3 LOD: uf/vf currently hold the tier-0 single shift = the
							// FAR target. Skip the whole march on a row entirely past the fade
							// (all lanes >= 2x lodDist) -> far/oblique parallax pixels pay
							// nothing; near rows march + blend per lane.
							const Vec8f ssU = uf, ssV = vf;
							const float lod = ctx.pomLodDist;
							const bool rowFar = lod > 0.0f &&
								horizontal_and(p_z >= Vec8f(2.0f * lod));
							if (!rowFar) {
							const int   N   = ctx.pomSteps;
							const Vec8f dU  = VtT * rayScale;
							const Vec8f dV  = VtB * rayScale;
							const Vec8f dlen = sqrt(dU*dU + dV*dV);   // UV per unit rayH (exact)
							const Vec8f baseU = ssU - VtT * hc;       // un-shifted geometric UV
							const Vec8f baseV = ssV - VtB * hc;
							// Start of the ray: the field top (h=1) for the legacy centered
							// march; the ENTRY POINT ITSELF for the shell (hStart == hEnter,
							// so the offset below vanishes and the march begins exactly at
							// the interpolated lid UV — the per-pixel texture-space entry).
							Vec8f curU = baseU + dU * (hStart - hEnter);
							Vec8f curV = baseV + dV * (hStart - hEnter);
							Vec8f rayH = hStart;
							const Vec8f coneScale = Vec8f(ctx.coneUnit * ctx.pomRelax);
								// --pom_cone_min_step (default 0 = off): minimum LATERAL
								// advance per cone step, in TEXELS, converted to a height
								// step by dividing by the lateral texels travelled per unit
								// height (dlen × texels-per-UV). A cone byte of 0 makes the
								// step c·gap/(c+dlen) exactly ZERO and the march FREEZES —
								// the lane then keeps the un-shifted entry UV, which for the
								// shell is the flat lid. This is the EGSR'24 "artifact-free
								// minimum step size"; it also bounds the step count needed to
								// cross the slab. dlen==0 (perpendicular view) travels no
								// texels at all and already takes the whole gap in one step,
								// so the floor is switched off there rather than made huge.
								const bool useMinStep = ctx.pomConeMinStepTexels > 0.0f;
								const Vec8f minStepH = useMinStep
								    ? Vec8f(ctx.pomConeMinStepTexels)
								      / max(dlen * Vec8f(ctx.heightUScale), Vec8f(1e-6f))
								    : Vec8f(0.0f);
							// Bracket of the first crossing. abo* = last sample confirmed ABOVE
							// the surface (init = field top, always above since H<=1); bel* =
							// first sample AT/below the surface. A lane that never crosses keeps
							// bel* at the top start (minimal shift = the naive no-hit fallback).
							Vec8f aboU = curU, aboV = curV, aboH = rayH;
							Vec8f belU = curU, belV = curV, belH = rayH;
							Vec8fb found = Vec8fb(false);
							// STEP-2 quarter-res offset field: with --parallax_pom_quarter,
							// gather height+cone only on EVEN lanes and share to the odd
							// neighbour -> ~half the gather traffic. The odd lane keeps its own
							// view geometry and only borrows the sampled DEPTH, so the parallax
							// OFFSET is subsampled (smooth), not the color (fetched full-res
							// below). Rides the raster 8-wide grid, not the deferred_quarter
							// lighting grid: parallax runs at G-buffer fill where the smooth
							// float UV exists. Small 1-texel slip at silhouettes.
							const int qStep = ctx.pomQuarter > 0 ? 2 : 1;
							for (int s = 0; s < N; ++s) {
								Vec8i mu = roundi(curU * ctx.heightUScale);
								Vec8i mv = roundi(curV * ctx.heightVScale);
								Vec8i ma = packed_tile_u(mu, ctx.heightLogH, ctx.heightUmaskSwizzled)
								         + packed_tile_v(mv, ctx.heightVmask);
								alignas(32) int32_t mAd[8]; ma.store_a(mAd);
								alignas(32) float mH[8], mC[8];
								for (int k = 0; k < 8; k += qStep) {
									const int a = mAd[k];
									mH[k] = float(ctx.heightData[a]) * (1.0f / 255.0f);
									mC[k] = float(ctx.coneData[a]);
									if (qStep == 2) { mH[k + 1] = mH[k]; mC[k + 1] = mC[k]; }
								}
								Vec8f Hs; Hs.load_a(mH);
								Vec8f Cb; Cb.load_a(mC);
								const Vec8fb hitNow = (rayH <= Hs) & (~found);
								// First crossing: record this sample as the below bracket end.
								belU = select(hitNow, curU, belU);
								belV = select(hitNow, curV, belV);
								belH = select(hitNow, rayH, belH);
								found |= (rayH <= Hs);
								// Lanes still searching: this above-sample is the new hi (above)
								// bracket end for the refinement below.
								const Vec8fb search = ~found;
								aboU = select(search, curU, aboU);
								aboV = select(search, curV, aboV);
								aboH = select(search, rayH, aboH);
								const Vec8f cratio = Cb * coneScale;  // cone ratio in [0,kPomConeMax]
								// gap>0 while above surface; clamp>=0 freezes a crossed lane.
								const Vec8f gap = max(rayH - Hs, Vec8f(0.0f));
								// exact divide (no rcp approx - parallax hard rule). c=0 near a tall
								// feature -> dt=0 (blocked); dlen=0 (perp view) -> dt=gap.
								Vec8f dt = cratio * gap / (cratio + dlen + Vec8f(1e-6f));
								// --pom_cone_min_step: floor the step at one minimum lateral
								// advance, but never further than the gap the ray is standing
								// over, so the floor only ever overrides a FROZEN (byte-0)
								// cone and never steps deeper into the surface than the cone
								// itself would have allowed. Off by default -> dt unchanged.
								if (useMinStep) dt = max(dt, min(minStepH, gap));
								dt = select(search, dt, Vec8f(0.0f));   // frozen once bracketed
								curU -= dU * dt;
								curV -= dV * dt;
								rayH -= dt;
								// --pom_march_earlyout, BYTE-EXACT: with every lane bracketed,
								// each remaining iteration computes hitNow = false, search =
								// false and dt = 0, so it leaves every bracket variable and
								// every ray variable that is read after the loop untouched.
								if (ctx.pomEarlyOut && horizontal_and(found)) break;
							}
							// Binary search between the above/below bracket ends (both on the
							// same ray, so the UV midpoint is exact). Each iteration samples the
							// midpoint height and halves the interval; the below end converges to
							// the true first crossing (sub-texel).
							for (int r = 0; r < ctx.pomRefine; ++r) {
								Vec8f mU = (aboU + belU) * Vec8f(0.5f);
								Vec8f mV = (aboV + belV) * Vec8f(0.5f);
								Vec8f mMH = (aboH + belH) * Vec8f(0.5f);
								Vec8i mu = roundi(mU * ctx.heightUScale);
								Vec8i mv = roundi(mV * ctx.heightVScale);
								Vec8i ma = packed_tile_u(mu, ctx.heightLogH, ctx.heightUmaskSwizzled)
								         + packed_tile_v(mv, ctx.heightVmask);
								alignas(32) int32_t mAd[8]; ma.store_a(mAd);
								alignas(32) float mHs[8];
								for (int k = 0; k < 8; k += qStep) {
									mHs[k] = float(ctx.heightData[mAd[k]]) * (1.0f / 255.0f);
									if (qStep == 2) mHs[k + 1] = mHs[k];
								}
								Vec8f Hs; Hs.load_a(mHs);
								const Vec8fb below = mMH <= Hs;
								belU = select(below, mU, belU);
								belV = select(below, mV, belV);
								belH = select(below, mMH, belH);
								aboU = select(below, aboU, mU);
								aboV = select(below, aboV, mV);
								aboH = select(below, aboH, mMH);
							}
							// The refined below sample is the first crossing (matches the naive
							// spike "first rayH<=Hs" convention, now sub-texel accurate).
							const Vec8f resU = belU, resV = belV;
							// --pom_depth_write: the refined below-bracket height IS the
							// crossing height. Lanes that never bracketed (found==false —
							// rare non-converged march) keep the flat 0.5: their bel* is
							// still the field-top start, and writing a half-band-CLOSER
							// depth for a lane whose texel fell back to minimal shift
							// would punch a false near-plateau into the Z relief.
							if (pomZ)
								pomHitH = select(found, belH, hEnter);
							// --pom_shell: only a ray that ran the WHOLE slab without
							// crossing has genuinely passed under all the stone. A cone
							// march that simply ran out of steps (rayH still inside the
							// slab) is UNRESOLVED, not a miss — discarding it would punch
							// holes wherever the bracket search is slow (grazing rays,
							// which travel farthest). Unresolved lanes keep the entry UV
							// (bel* never moved), i.e. they degrade to no-shift.
							if (shell)
								pomCrossed = found | (rayH > Vec8f(0.0f));
							// STEP-3 LOD blend: continuous fade cone->single-shift as view-Z
							// grows from lodDist (full cone) to 2x lodDist (pure single-shift).
							// lod==0 -> full cone everywhere (fade=0).
							if (lod > 0.0f) {
								const Vec8f fade = min(max(
									(p_z - Vec8f(lod)) * Vec8f(1.0f / lod),
									Vec8f(0.0f)), Vec8f(1.0f));
								// --pom_shell + LOD: a lane faded ALL the way to the
								// single shift shows the flat lid, so it must never be
								// discarded (no relief left to make a silhouette out of).
								if (shell)
									pomCrossed |= (fade >= Vec8f(1.0f));
								uf = resU + (ssU - resU) * fade;
								vf = resV + (ssV - resV) * fade;
								// Depth fades in lockstep with the UV blend: pure
								// single-shift (fade=1) writes the flat plane again.
								if (pomZ)
									pomHitH += (hEnter - pomHitH) * fade;
							} else {
								uf = resU;
								vf = resV;
							}
							}  // !rowFar
						} else if (ctx.pomSpikeSteps > 0) {
							// LOD (--parallax_pom_lod): single-shift (ssU/ssV) is the FAR
							// target. Skip the whole march on a row entirely past 2x the fade
							// distance, and blend march->single-shift as view-Z grows from lod
							// to 2*lod. Far/oblique parallax pixels (most of a deep view) pay
							// little/nothing. lod==0 (default) -> rowFar false + no blend =
							// byte-identical to the plain march.
							const Vec8f ssU = uf, ssV = vf;
							const float lod = ctx.pomLodDist;
							const bool rowFar = lod > 0.0f &&
								horizontal_and(p_z >= Vec8f(2.0f * lod));
							if (!rowFar) {
							const int   N     = ctx.pomSpikeSteps;
							const float invNf = 1.0f / float(N);
							const Vec8f dU = VtT * rayScale;
							const Vec8f dV = VtB * rayScale;
							// base (un-shifted) UV recovered from the single shift.
							const Vec8f baseU = ssU - VtT * hc;
							const Vec8f baseV = ssV - VtB * hc;
							// Legacy: start at the field top (h=1). Shell: start AT the
							// interpolated entry point on the lid (hStart == hEnter) and
							// spread the same N steps over the slab below it, so the
							// sampling density per unit height is unchanged.
							Vec8f curU  = baseU + dU * (hStart - hEnter);
							Vec8f curV  = baseV + dV * (hStart - hEnter);
							Vec8f rayH  = hStart;
							const Vec8f stepH = hStart * Vec8f(invNf);
							Vec8f foundU = curU, foundV = curV;
							Vec8fb found = Vec8fb(false);
							for (int s = 0; s < N; ++s) {
								curU -= dU * stepH;
								curV -= dV * stepH;
								rayH -= stepH;
								Vec8i mu = roundi(curU * ctx.heightUScale);
								Vec8i mv = roundi(curV * ctx.heightVScale);
								Vec8i ma = packed_tile_u(mu, ctx.heightLogH, ctx.heightUmaskSwizzled)
								         + packed_tile_v(mv, ctx.heightVmask);
								alignas(32) int32_t mAd[8]; ma.store_a(mAd);
								alignas(32) float mH[8];
								for (int k = 0; k < 8; ++k)
									mH[k] = float(ctx.heightData[mAd[k]]) * (1.0f / 255.0f);
								Vec8f Hs; Hs.load_a(mH);
								Vec8fb hit = Vec8fb(rayH <= Hs) & (~found);
								foundU = select(hit, curU, foundU);
								foundV = select(hit, curV, foundV);
								// --pom_depth_write: the rayH at the first crossing
								// sample is the marched height. No-hit lanes (only
								// possible through float residue at rayH≈0) keep 0.5.
								if (pomZ)
									pomHitH = select(hit, rayH, pomHitH);
								found |= hit;
								if (shell)
									pomCrossed = found;
								// --pom_march_earlyout, BYTE-EXACT: every later iteration
								// computes hit = false and leaves foundU/foundV/pomHitH/
								// pomCrossed exactly as they stand.
								if (ctx.pomEarlyOut && horizontal_and(found)) break;
							}
							if (lod > 0.0f) {
								const Vec8f fade = min(max(
									(p_z - Vec8f(lod)) * Vec8f(1.0f / lod),
									Vec8f(0.0f)), Vec8f(1.0f));
								// --pom_shell + LOD: a lane faded ALL the way to the
								// single shift shows the flat lid, so it must never be
								// discarded (no relief left to make a silhouette out of).
								if (shell)
									pomCrossed |= (fade >= Vec8f(1.0f));
								uf = foundU + (ssU - foundU) * fade;
								vf = foundV + (ssV - foundV) * fade;
								// Depth fades with the UV blend (flat at fade=1).
								if (pomZ)
									pomHitH += (hEnter - pomHitH) * fade;
							} else {
								uf = foundU;
								vf = foundV;
							}
							}  // !rowFar
						}
						// --parallax_max_offset: bound the final offset to N TEXELS
						// from the geometric UV (single-shift AND march). At strength
						// >> the tuned value the offset over-drives and the stone
						// blocks tear into diagonal smears; this caps the streak
						// length without disabling the effect. 0 = no clamp (byte-
						// identical). Isotropic in texel space; U and V scaled by the
						// same factor to keep the offset direction. Before the debug/
						// pom_viz taps below so they see the clamped UV.
						if (ctx.parallaxMaxOffset > 0.0f) {
							const Vec8f duT = (uf - ufGeo) * Vec8f(UScaleFactor);
							const Vec8f dvT = (vf - vfGeo) * Vec8f(VScaleFactor);
							const Vec8f len = sqrt(duT*duT + dvT*dvT);
							const Vec8f maxT = Vec8f(ctx.parallaxMaxOffset);
							const Vec8f s = select(len > maxT,
								maxT / max(len, Vec8f(1e-6f)), Vec8f(1.0f));
							uf = ufGeo + (uf - ufGeo) * s;
							vf = vfGeo + (vf - vfGeo) * s;
						}
						// --pom_normal (S1e): hand the deferred kernel the HEIGHT FIELD'S
						// OWN surface normal at the marched hit instead of the flat
						// polygon normal.
						//
						// Why this is a MISSING TERM and not a nicety - measured at greets
						// t=5780 with --no-nmap (plan S1e): the TESSELLATION path still
						// shows fully shaped, bevelled blocks with dark mortar, because its
						// geometry carries the height map's low band and therefore tilts
						// the shading normal; the per-pixel path shows a FLAT wall, because
						// the march only moves UVs - nothing in it ever tilts a normal -
						// and the material's normal map carries the fine grain, not the
						// block-scale relief. That difference is the whole of the user's
						// "the tessellated version's grooves sit in visibly deeper shadow".
						//
						// Central differences over +-1 texel of the height map's own mip.
						// The world-per-UV cancels EXACTLY, so no per-triangle density term
						// is needed: one texel of u spans w/heightUScale world and a unit of
						// h spans A_uv*w world, hence dH/dU_world = dh * A_uv * heightUScale.
						// A_uv is the SAME amplitude the march travelled with (the
						// geometry's for a shell, the strength flag's otherwise), so the
						// normal can never disagree with the parallax or the depth.
						// N' = normalize(N - sU*T - sV*B) in the view-space TBN already
						// built above; the encode below normalizes.
						//
						// Cost: 4 extra height gathers per covered pixel (the march itself
						// runs 8-14) - not free, see the plan's measured ms. The bumped
						// normal is also what --pom_horizon then builds its azimuth frame
						// from, exactly as the tessellation path's bumped geometric normal
						// already does, so the two paths stay consistent rather than
						// diverging.
						if (ctx.pomNormal) {
							const Vec8f amp = shell ? Vec8f(ctx.pomShellUvAmp)
							                        : Vec8f(ctx.parallaxStrength);
							const Vec8f tU = uf * ctx.heightUScale;
							const Vec8f tV = vf * ctx.heightVScale;
								Vec8i muC = roundi(tU), mvC = roundi(tV);
								Vec8i muP = roundi(tU + Vec8f(1.0f));
								Vec8i muM = roundi(tU - Vec8f(1.0f));
								Vec8i mvP = roundi(tV + Vec8f(1.0f));
								Vec8i mvM = roundi(tV - Vec8f(1.0f));
								const Vec8i tvC = packed_tile_v(mvC, ctx.heightVmask);
								const Vec8i tuC = packed_tile_u(muC, ctx.heightLogH,
								                                ctx.heightUmaskSwizzled);
								alignas(32) int32_t aUp[8], aUm[8], aVp[8], aVm[8];
								(packed_tile_u(muP, ctx.heightLogH, ctx.heightUmaskSwizzled)
								 + tvC).store_a(aUp);
								(packed_tile_u(muM, ctx.heightLogH, ctx.heightUmaskSwizzled)
								 + tvC).store_a(aUm);
								(tuC + packed_tile_v(mvP, ctx.heightVmask)).store_a(aVp);
								(tuC + packed_tile_v(mvM, ctx.heightVmask)).store_a(aVm);
							alignas(32) float dUa[8], dVa[8];
							for (int q = 0; q < 8; ++q) {
								dUa[q] = float(int(ctx.heightData[aUp[q]])
								             - int(ctx.heightData[aUm[q]]));
								dVa[q] = float(int(ctx.heightData[aVp[q]])
								             - int(ctx.heightData[aVm[q]]));
							}
							Vec8f gU; gU.load_a(dUa);
							Vec8f gV; gV.load_a(dVa);
							const Vec8f kk = amp * Vec8f(ctx.pomNormalStrength)
							               * Vec8f(0.5f / 255.0f);
							const Vec8f sU = gU * kk * Vec8f(ctx.heightUScale);
							const Vec8f sV = gV * kk * Vec8f(ctx.heightVScale);
							pnX = Nx - Tx * sU - Bx * sV;
							pnY = Ny - Ty * sU - By * sV;
							pnZ = Nz - Tz * sU - Bz * sV;
						}
						// --pom_depth_write (S1a): store the MARCHED depth instead of the
						// flat plane's (the store at the top of the row was skipped when
						// pomZ). The landed crossing (uf,vf,pomHitH) lies on the offset-
						// limited march ray, so the true 3D point it names is
						//   P_hit = P0 + T·w·Δu + B·w·Δv + N·(h−0.5)·A
						// with Δu,Δv = strength·VtT/VtB·(h−0.5), w = world-per-UV-tile,
						// A = strength·w (the world amplitude the tuned strength implies
						// at this face's texel density). Its z-component collapses via
						// V = VtT·T + VtB·B + VtN·N (so Tz·VtT + Bz·VtB = Vz − VtN·Nz) to
						//   Δz = (h−0.5) · strength·w · (Vz + Nz·(1−VtN))
						// — no divide, bounded (|Δz| ≤ A) at ANY angle because the march
						// itself is offset-limited: depth stays exactly consistent with
						// the texels shown, and grazing cannot explode it. h uses the
						// CENTERED convention (0.5 = authored plane, matching the
						// zero-mean geometric bake): mortar (h<0.5) writes DEEPER z,
						// block tops (h>0.5) write slightly CLOSER z.
						//
						// ORDERING HAZARD (documented deliberately): the tiled raster is
						// front-to-back with Z-early-reject, and this store happens at
						// G-buffer fill time — a face rasterized LATER that lies between
						// the flat wall plane and the recessed relief (inside the ±A/2
						// band, e.g. a trim/jamb/prop face abutting the wall) will now
						// PASS the Z test at groove pixels and win them. For wall
						// content that is usually CORRECT — it is exactly what true
						// displacement geometry would do (the bake path shows the same
						// thing) — but it is a semantic change to Z: intersections of
						// props with relief walls (momy statues, letters near walls)
						// resolve per-pixel against the relief, not the plane. The
						// reverse hazard is bounded the same way: a protruding block
						// writes at most A/2 closer, so it can steal at most that band
						// from later coplanar geometry. Within ONE face there is no
						// hazard (each pixel is owned by one face via the edge mask);
						// across the two triangles of a quad the height field is
						// continuous, so seam pixels agree. Z consumers (SSAO/GTAO,
						// fog, DoF, quarter-res reconstruction, z-dumps) simply see the
						// relief — that is the point of the flag.
						// --pom_shell (S1b): THE SILHOUETTE. Kill every lane whose ray
						// left the authored patch before crossing the height field. A
						// downward ray inside a height field always crosses it (h >= 0
						// everywhere), so a plain "miss" is not what opens silhouettes —
						// LATERAL EXIT is. The march is a straight line in UV and the
						// authored UV box is convex, so the FIRST crossing lies inside
						// the box iff the ray crossed before exiting: one test on the
						// FINAL uv, no per-step work. The lid covers more screen than the
						// authored plane (it is offset by A/2 along N), and it is exactly
						// in that extra band that discarded lanes let the geometry BEHIND
						// win the pixel — the jagged block-edge see-through the
						// tessellation bake gets from protruding verts.
						//
						// The kill is folded into p_mask HERE, before the deferred Z store
						// below and before every G-buffer plane store further down (all of
						// them are p_mask-gated, and the full-row vector-store path is
						// gated on all_lanes_set, so it self-disables on a partial kill).
						// Z is left untouched for killed lanes, which is what lets a
						// FARTHER face rasterized later win them (front-to-back order).
						if (shell) {
							Vec8fb keep = pomCrossed;
							// Inside the patch DOMAIN = inside its own UV box OR any
							// sibling patch's (ctx.shellSibs, --pom_shell_merge_uv):
							// the UNION of the boxes, never their bounding box.
							// Siblings are skipped while every lane is already inside
							// its own box — the overwhelming majority of covered
							// pixels — so the multi-box domain costs nothing away
							// from a border.
							auto inDomain = [&](const Vec8f &u, const Vec8f &v) {
								Vec8fb ins = (u >= Vec8f(ctx.shellUMin))
								           & (u <= Vec8f(ctx.shellUMax))
								           & (v >= Vec8f(ctx.shellVMin))
								           & (v <= Vec8f(ctx.shellVMax));
								for (int sb = 0; sb < ctx.shellSibCount
								     && !horizontal_and(ins); ++sb) {
									const float *bx = ctx.shellSibs + 4 * sb;
									ins |= (u >= Vec8f(bx[0])) & (u <= Vec8f(bx[1]))
									     & (v >= Vec8f(bx[2])) & (v <= Vec8f(bx[3]));
								}
								return ins;
							};
							if (ctx.pomShellDomain)
								keep &= inDomain(uf, vf);
							// --pom_shell_base_clip (the lid-overhang fix): the lid is
							// a rigid outward translation of the patch, so at a patch
							// BORDER it covers screen the authored plane never did,
							// and rays entering there march INWARD, hit legally and
							// paint stone over whatever is really behind — the user's
							// "pixels ran away to the left from the edge, floating mid
							// air", measured as 12 162 px of over-coverage vs the
							// tessellation reference at t=6097 and 19 335 at t=5780.
							// The test: where does THIS pixel's view ray cross the
							// AUTHORED plane (h = 0.5)? Same affine UV chart, so
							//   uv_base = uv_lid + (Vt_T,Vt_B)·(0.5 − hEnter)·A/(V·N)
							// with A = the slab's UV amplitude and the UNCAPPED
							// 1/(V·N) (see invVtNRaw). Outside the domain ⇒ the flat
							// wall does not cover this pixel ⇒ it is lid overhang ⇒
							// kill it. That reproduces the tessellation bake's
							// convention exactly: it pins patch-border verts to zero
							// displacement, so its relief never crosses the authored
							// footprint either. Costs 2 FMAs + one compare group per
							// pixel, no per-step work.
							if (ctx.pomShellBaseClip) {
								const Vec8f s = Vec8f(ctx.pomShellUvAmp)
								              * (ctx.pomShellBaseClipRaw ? invVtNRaw : invVtN)
								              * (Vec8f(0.5f) - hEnter);
								keep &= inDomain(ufGeo + VtT * s, vfGeo + VtB * s);
							}
							p_mask &= Vec8ib(_mm256_castps_si256(__m256(keep)));
						}
						if (pomZ) {
							// Shell depth uses the TRUE-ray form: the hit sits Δh·A below
							// the entry along the view ray, so Δz = Δh·A·Vz/(V·N) with the
							// same capped 1/(V·N) the march travelled with — depth and
							// texels stay consistent, and the cap bounds |Δz| ≤ A·cap.
							const Vec8f dz  = shell
							    ? ((pomHitH - hEnter) * Vec8f(pomDepthWorldAmp) * Vz * invVtN)
							    : ((pomHitH - Vec8f(0.5f))
							                * Vec8f(pomDepthWorldAmp)
							                * (Vz + Nz * (Vec8f(1.0f) - VtN)));
							// Near guard: a protrusion written from a wall the camera
							// is nearly touching must not cross z<=0 (encode wrap /
							// negative-z reconstruction downstream). Clamp the DEPTH,
							// not the offset, so the recess side is untouched.
							const Vec8f zRelief = max(p_z + dz, Vec8f(1.0f / 128.0f));
							const auto zMarched = (Vec8ui(0xFF80)
								- static_cast<Vec8ui>(roundi(ctx.zScale * zRelief)));
							*(__m128i*)span.zbuffer = _mm_blendv_epi8(
								*(__m128i*)span.zbuffer, compress(zMarched),
								compress(Vec8ui(p_mask)));
						}
						// Debug (FDS_DUMP_TXTR): record the finalized parallax UV for covered
						// lanes so a headless A/B can diff the MARCH output directly.
						if (g_pomDbgUV) {
							alignas(32) float dbgU[8], dbgV[8];
							uf.store_a(dbgU); vf.store_a(dbgV);
							alignas(32) int32_t dbgM[8]; Vec8i(p_mask).store_a(dbgM);
							const int dbgBaseX = tile.x * TILE_SIZE;
							const int dbgPy = tile.y * TILE_SIZE + y;
							if (dbgPy < g_pomDbgH) for (int k = 0; k < 8; ++k) {
								const int px = dbgBaseX + k;
								if (dbgM[k] && px < g_pomDbgStride) {
									const size_t di = (size_t(dbgPy) * g_pomDbgStride + px) * 2;
									g_pomDbgUV[di] = dbgU[k]; g_pomDbgUV[di + 1] = dbgV[k];
								}
							}
						}
					}
					Vec8i u = roundi(uf * UScaleFactor);
					Vec8i v = roundi(vf * VScaleFactor);

					Vec8i tu = packed_tile_u(u, LogHeight, t_umask_swizzled);
					Vec8i tv = packed_tile_v(v, t_vmask);

					auto p_offset = tu + tv;
					auto packedTxtrData = v8_TxtrIdMask | p_offset;
					_mm256_maskstore_ps(span.txtr, *(__m256i*)(&p_mask), *(__m256*)(&packedTxtrData));

					// Texture filtering: sample the diffuse texel with the
					// sub-texel FRACTION (only present here, pre-swizzle-pack)
					// and write the blended BGRA to gbuffer.albedo. The suv in
					// span.txtr above is left untouched (roundi/point) so the
					// kernel's metal/rough/AO map lookups are unchanged. Note
					// uf/vf are the SAME (post-parallax) UV the point path packs.
					if (wantAlbedo) {
						const Vec8f ufTexels = uf * UScaleFactor;
						const Vec8f vfTexels = vf * VScaleFactor;
						Vec8ui albedoCol = bilinear_sample_x8(
							ufTexels, vfTexels, LogHeight,
							(uint32_t)t_umask_swizzled, t_vmask,
							albedoTex0, p_mask);
						if (wantTri) {
							// mip+1 is half-res → texel coords ×0.5.
							const Vec8ui albedoCol1 = bilinear_sample_x8(
								ufTexels * 0.5f, vfTexels * 0.5f, LogHeight1,
								(uint32_t)t1_umask_swizzled, t1_vmask,
								albedoTex1, p_mask);
							const Vec32us c0e = extend(Vec32uc(albedoCol));
							const Vec32us c1e = extend(Vec32uc(albedoCol1));
							albedoCol = Vec8ui(compress(
								(c0e * triIW + c1e * triW) >> 8));
						}
						// --pom_viz: swap the albedo for the height field at the
						// FINAL (post-march) UV — the parallax result rendered
						// directly (domes/mortar/march terracing). Debug only.
						// Sampled through the height map's OWN addressing (ctx.height*)
						// so a pinned mip (--pom_height_mip) is visualised faithfully.
						if (ctx.pomViz && ctx.heightData) {
							Vec8i hu = roundi(uf * ctx.heightUScale);
							Vec8i hv = roundi(vf * ctx.heightVScale);
							Vec8i hva = packed_tile_u(hu, ctx.heightLogH, ctx.heightUmaskSwizzled)
							          + packed_tile_v(hv, ctx.heightVmask);
							alignas(32) int32_t hAd[8]; hva.store_a(hAd);
							alignas(32) uint32_t g8[8];
							for (int k = 0; k < 8; ++k) {
								// Contrast-stretch around the calibrated midband
								// (the shipping maps are ~mean 140 / sigma 27 in
								// 8-bit) so the narrow height histogram reads.
								const int32_t raw = int32_t(ctx.heightData[hAd[k]]);
								int32_t g = (raw - 140) * 3 + 128;
								if (g < 0) g = 0; else if (g > 255) g = 255;
								g8[k] = 0xFF000000u | (uint32_t(g) << 16)
								      | (uint32_t(g) << 8) | uint32_t(g);
							}
							albedoCol.load_a(g8);
						}
						// --pom_mip_viz: tint by THIS face's miplevel so the
						// mipmap-via-subdivision sub-face boundaries (the diagonal
						// seams' cause) read directly. A tiny fixed palette keyed on
						// ctx.miplevel; applies to every textured face.
						// --poly_viz: WHO OWNS THIS PIXEL. A rasterizer can only write
						// inside the triangle it fills, so when a fragment of foreign
						// texture appears mid-surface there are exactly two
						// possibilities — a triangle of ANOTHER surface genuinely
						// covers that screen area (geometry), or the surface's OWN
						// triangle is shading wrong (texturing). This viz separates
						// them by eye in one render:
						//   HUE       = material id (a fixed 12-entry palette), so a
						//               foreign fragment is a different colour from
						//               the surface it sits on ⇒ geometry;
						//   BRIGHT    = the face is a --pom_shell LID face,
						//   DARK      = it is not (45 %);
						//   ±jitter   = per-TRIANGLE hash, so every triangle boundary
						//               is visible and "which triangle" is answerable.
						// Replaces the albedo only; lighting still runs, so the shape
						// reads. Default OFF.
						if (polyVizColor) {
							alignas(32) uint32_t t8[8];
							for (int k = 0; k < 8; ++k) t8[k] = polyVizColor;
							albedoCol.load_a(t8);
						}
						if (ctx.pomMipViz) {
							static const uint32_t kMipPal[8] = {
								0xFFE03030u, 0xFF30E030u, 0xFF3060E0u, 0xFFE0E030u,
								0xFFE030E0u, 0xFF30E0E0u, 0xFFE08030u, 0xFF808080u };
							const uint32_t c = kMipPal[ctx.miplevel & 7];
							alignas(32) uint32_t t8[8];
							for (int k = 0; k < 8; ++k) t8[k] = c;
							albedoCol.load_a(t8);
						}
						_mm256_maskstore_ps((float*)span.albedo,
							*(__m256i*)(&p_mask), *(__m256*)(&albedoCol));
					}

					// Per-pixel nlerp + octahedral pack. Vec normalize +
					// vec oct_encode_u16_x8 — formerly scalar per-lane.
					// Tangent: vec Gram-Schmidt + vec encode, with a
					// degenerate-lane mask that zeros tangent for lanes
					// where T became parallel to N after interpolation.
					const Vec8f n2 = pnX*pnX + pnY*pnY + pnZ*pnZ;
					const Vec8f vInvN = approx_rsqrt(n2);
					const Vec8f vnx = pnX * vInvN;
					const Vec8f vny = pnY * vInvN;
					const Vec8f vnz = pnZ * vInvN;
					alignas(32) uint32_t normalEnc[8];
					_mm256_store_si256((__m256i*)normalEnc,
						oct_encode_u32_x8(*(const __m256*)&vnx,
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
						// Normal plane is 32-bit (oct 16.16): full-lane store.
						_mm256_storeu_si256((__m256i*)span.normal,
							_mm256_load_si256((const __m256i*)normalEnc));
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
						if (wantFaceId) {
							_mm256_storeu_si256((__m256i*)span.faceId,
								_mm256_set1_epi32(int32_t(faceIdKey)));
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
							span.normal[lane] = normalEnc[lane];
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
							if (wantFaceId) {
								span.faceId[lane] = faceIdKey;
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
			if (shell) {
				p_shz += Vec8f(dshzdy);
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
						.uz0 = v1.UZ + dx * duzdx + dy * duzdy,
						.vz0 = v1.VZ + dx * dvzdx + dy * dvzdy,
						.rz0 = v1.RZ + dx * drzdx + dy * drzdy,
						.nx0 = v1.TN.x + dx * dnxdx + dy * dnxdy,
						.ny0 = v1.TN.y + dx * dnydx + dy * dnydy,
						.nz0 = v1.TN.z + dx * dnzdx + dy * dnzdy,
						.tx0 = v1.TTangent.x + dx * dtxdx + dy * dtxdy,
						.ty0 = v1.TTangent.y + dx * dtydx + dy * dtydy,
						.tz0 = v1.TTangent.z + dx * dtzdx + dy * dtzdy,
						.obBZ0 = v1.OrigBaryB * v1.RZ + dx * dobBdx + dy * dobBdy,
						.obCZ0 = v1.OrigBaryC * v1.RZ + dx * dobCdx + dy * dobCdy,
						.shz0  = v1.ShellH * v1.RZ + dx * dshzdx + dy * dshzdy,
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
extern uint16_t      *g_xparPeelFloor; // depth-peel floor/ceiling buffer (per-pass bound)
// When true, the transparent rasterizer runs in reverse-peel mode: keep the
// FARTHEST fragment nearer than g_xparPeelFloor (the ceiling), with the layer
// Z pre-cleared to 0xFFFF. Set per (mesh, side) batch by the K-pass dispatch
// for xpar_peel_passes >= 2; false elsewhere (legacy keep-nearest front/back).
// thread_local: TBR strips raster on parallel worker threads, and the legacy
// peel rasters on the threadpool too, so each worker sets its own copy right
// before its raster (the synchronous clipper.Render reads it on the same
// thread). Opaque ignores it (null peelFloor short-circuits the gate).
extern thread_local bool g_xparPeelReverse;
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

// The UV amplitude this FACE's shell march runs at. Default = the material's
// single PomShellUvAmp, so the WORLD depth of the relief follows each chart's
// world-per-UV (measured on greets: 'rooms' 0.180..0.226 world, 'floor' 1.113 —
// docs/S1_DISCREPANCY_INVENTORY.md §8). With --pom_shell_world_amp on,
// PomShell_Build publishes a PER-PATCH amplitude (worldAmp / that patch's
// world-per-UV; a patch is coplanar by construction, so its density is one
// number) and this picks it up. Doing it here, once per face, is deliberate: the
// per-triangle and per-pixel code paths are not touched at all, which is what
// keeps the default path's codegen — and therefore its last bit — identical.
inline float PomShellFaceUvAmp(const Face *F) {
	const Material *M = F->Txtr;
	if (M->PomShellPatchUvAmp && F->PomShellGroup != 0
	    && F->PomShellGroup <= M->PomShellDomainCount)
		return M->PomShellPatchUvAmp[F->PomShellGroup - 1];
	return M->PomShellUvAmp;
}

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
	uint16_t *peelFloorPtr = nullptr;   // transparent only; gates deeper peel passes
	if constexpr (Target == MekaleleTarget::Opaque) {
		gb   = rt.gbuffer;
		zbuf = rt.zpage16;
	} else if constexpr (Target == MekaleleTarget::TransparentFront) {
		gb   = rt.gbufferTransparent;
		zbuf = rt.xparZ;
		peelFloorPtr = rt.xparPeelFloor;
	} else {  // TransparentBack
		gb   = rt.gbufferTransparentBack;
		zbuf = rt.xparZBack;
		peelFloorPtr = rt.xparPeelFloor;
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
	// Parallax: resolve the material's height mip. Legacy = THIS face's albedo
	// miplevel (the height shares the albedo tiled layout). --pom_height_mip>=0
	// PINS the height mip to a fixed level for EVERY face instead — the fix for
	// the diagonal seams: adjacent sub-faces (mipmap-via-subdivision, diagonal
	// edges) otherwise sample the height at different mip resolutions, so the
	// parallax UV offset jumps across the boundary. Pinning keeps the sampled
	// height continuous (height is low-frequency). The gathers use the height
	// map's OWN dims/mask/scale at heightMipUsed (threaded via ctx below), so
	// a pinned level with different dims than the face's albedo mip is fine.
	const byte *heightData = nullptr;
	dword heightMipUsed = miplevel;
	const int pomHeightPin = fds::FeatureFlags::pom_height_mip();
	if (fds::FeatureFlags::parallax() && F->Txtr->HeightMap) {
		Texture *hm = F->Txtr->HeightMap;
		dword lvl = miplevel;
		if (pomHeightPin >= 0) {
			lvl = (dword)pomHeightPin;
			if (lvl >= hm->numMipmaps) lvl = hm->numMipmaps ? hm->numMipmaps - 1 : 0;
		}
		if (lvl < hm->numMipmaps && hm->Mipmap[lvl]) {
			heightData = reinterpret_cast<const byte*>(hm->Mipmap[lvl]);
			heightMipUsed = lvl;
		}
	}
	// Tier-2 cone-step POM: resolve the cone mip at the SAME level as the height
	// (same tiled layout → same swizzled address). Only when --parallax_pom>0
	// AND the material carries a baked ConeMap; else the march stays single-shift.
	const byte *coneData = nullptr;
	const int pomSteps = fds::FeatureFlags::parallax_pom();
	if (heightData && pomSteps > 0 && F->Txtr->ConeMap) {
		Texture *cm = F->Txtr->ConeMap;
		if (heightMipUsed < cm->numMipmaps && cm->Mipmap[heightMipUsed])
			coneData = reinterpret_cast<const byte*>(cm->Mipmap[heightMipUsed]);
	}
	// Height-map addressing threaded to the rasterizer's parallax gathers,
	// independent of the albedo mip. Legacy (pin off) = the albedo mip's exact
	// LogWidth/LogHeight/scale/mask (ctx.Txtr = F->Txtr->Txtr, ctx.miplevel) →
	// byte-identical to before. Pinned = the HeightMap's own dims at
	// heightMipUsed. Only read when heightData != nullptr.
	int32_t heightLogW = 0, heightLogH = 0;
	float   heightUScale = 0.0f, heightVScale = 0.0f;
	int32_t heightUmaskSwizzled = 0, heightVmask = 0;
	if (heightData) {
		if (pomHeightPin < 0) {
			heightLogW = F->Txtr->Txtr->LSizeX - (int32_t)miplevel;
			heightLogH = F->Txtr->Txtr->LSizeY - (int32_t)miplevel;
		} else {
			Texture *hm = F->Txtr->HeightMap;
			heightLogW = hm->LSizeX - (int32_t)heightMipUsed;
			heightLogH = hm->LSizeY - (int32_t)heightMipUsed;
		}
		heightUScale = float(1 << heightLogW);
		heightVScale = float(1 << heightLogH);
		heightVmask  = (1 << heightLogH) - 1;
		heightUmaskSwizzled = (int32_t)swizzle_umask(heightLogH, (1 << heightLogW) - 1);
	}
	// Routing: --parallax_pom drives the NAIVE occlusion march by default (it
	// records the first rayH<=Hs crossing -> features anchored to true depth, no
	// swim). --parallax_pom_cone selects the relaxed CONE march (cone-step bracket
	// + binary-search refine, see the march block) which converges to the SAME
	// crossing in fewer taps. --parallax_pom_refine = cone bisection count,
	// --parallax_pom_relax = cone step-width relax factor.
	// --pom_march_steps_auto (S1 P1, default 0 = off): DERIVE this face's march
	// budget from the measured step rule instead of taking one global number.
	//
	// The rule, measured (docs/S1_DISCREPANCY_INVENTORY.md §9): what a march has
	// to resolve is TEXELS OF LATERAL TRAVEL down the slab, and the quality knee
	// sits at a fixed number of texels PER STEP. The travel is
	//     T = uvAmp × texels-per-UV-tile × tan(incidence)
	// and the shell's grazing cap bounds the last factor at sqrt(cap² − 1), so a
	// face's worst case is computable at setup from quantities it already owns.
	// The flag's value IS the texels-per-step target, so N = ceil(T / value).
	//
	// This is what makes the budget follow the SURFACE rather than a hand-set
	// global: greets' floor tiles its map over ~15 world units and the walls over
	// 6, and the floor's slab is 6.2× deeper, so the floor genuinely needs a
	// bigger budget than the wall beside it — and a distant face, marching a
	// higher height mip with half the texels, automatically needs half.
	// Pair it with --pom_march_earlyout: the budget is then a CEILING that only
	// the pixels that need it pay for.
	const float autoStepTexels = fds::FeatureFlags::pom_march_steps_auto();
	int pomStepsFace = pomSteps;
	if (autoStepTexels > 0.0f && heightData && pomSteps > 0) {
		const bool shellFace = fds::FeatureFlags::pom_shell()
		                       && F->Txtr->PomShellUvAmp > 0.0f;
		// The legacy centered march offset-limits its lateral travel at
		// `parallax_strength` whatever the angle, so its travel does not grow
		// with incidence at all; the shell marches the TRUE ray, capped.
		const float cap  = fds::FeatureFlags::pom_shell_cap();
		const float tanMax = (shellFace && cap > 1.0f)
		                     ? std::sqrt(cap * cap - 1.0f) : 1.0f;
		const float uvAmp = shellFace ? PomShellFaceUvAmp(F)
		                              : fds::FeatureFlags::parallax_strength()
		                                * F->Txtr->ParallaxScale;
		const float travelTexels = uvAmp * float(1 << heightLogW) * tanMax;
		int n = int(travelTexels / autoStepTexels) + 1;
		if (n < 4)   n = 4;
		if (n > 256) n = 256;
		pomStepsFace = n;
	}
	const bool useCone    = fds::FeatureFlags::parallax_pom_cone();
	// --pom_ref_march (DIAGNOSTIC, default OFF): the converged brute-force
	// reference. It takes precedence over BOTH shipping marches — the point of a
	// reference is that none of their approximations are in it.
	const bool useRef     = fds::FeatureFlags::pom_ref_march() && (heightData != nullptr);
	const int  refSteps   = useRef ? std::max(1, fds::FeatureFlags::pom_ref_steps()) : 0;
	const int  naiveSteps = (useCone || useRef) ? 0 : pomStepsFace;
	const int  coneSteps  = (useCone && coneData && !useRef) ? pomStepsFace : 0;
	// --pom_depth_write (S1a): armed only when a march is actually configured
	// for this face — the depth written must be the MARCHED crossing, and
	// single-shift-only faces have no crossing to write. apply_exact keys its
	// deferred Z store off ctx.pomDepthWrite alone (plus its own tangent gate).
	// --pom_shell (S1b): this face is part of a shell only if the geometry was
	// actually built as one (Material::PomShellUvAmp > 0, stamped by
	// PomShell_Build at scene init) AND a march is configured. The shell needs
	// the marched depth by construction (the lid's own plane depth is A/2 in
	// front of everything), so it ARMS pom_depth_write implicitly.
	const bool marchArmed = (heightData != nullptr)
	    && (refSteps > 0 || naiveSteps > 0 || (coneSteps > 0 && coneData != nullptr));
	const bool pomShellFace = fds::FeatureFlags::pom_shell() && marchArmed
	    && (F->Txtr->PomShellUvAmp > 0.0f);
	const bool pomDepthWrite = (fds::FeatureFlags::pom_depth_write() || pomShellFace)
	    && marchArmed;
#if FDS_DEV
	if (pomShellFace && fds::FeatureFlags::pom_shell_census())
		meka::g_pomShellMipHist.n[F->Txtr->ID & 15][heightMipUsed & 15]
			.fetch_add(1, std::memory_order_relaxed);
#endif
	// Patch domain for the lateral-exit test (see Face::PomShellGroup), plus the
	// patch's SIBLING boxes — the other patches on the same plane whose UV rects
	// abut it (Material::PomShellSibBoxes, built when --pom_shell_merge_uv > 0).
	// The domain is the UNION OF THOSE BOXES, not their bounding box: a floor cut
	// into patches by doorway thresholds stops discarding across the cuts, while a
	// genuine opening between two coplanar patches still discards.
	const float *pomShellDom = nullptr;
	const float *pomShellSibs = nullptr;
	int pomShellSibCount = 0;
	if (pomShellFace && F->PomShellGroup != 0 && F->Txtr->PomShellDomains
	    && F->PomShellGroup <= F->Txtr->PomShellDomainCount) {
		pomShellDom = F->Txtr->PomShellDomains + 4 * (F->PomShellGroup - 1);
		if (F->Txtr->PomShellSibBoxes && F->Txtr->PomShellSibOfs) {
			const uint32_t o0 = F->Txtr->PomShellSibOfs[F->PomShellGroup - 1];
			const uint32_t o1 = F->Txtr->PomShellSibOfs[F->PomShellGroup];
			pomShellSibs = F->Txtr->PomShellSibBoxes + 4 * o0;
			pomShellSibCount = int(o1 - o0);
		}
	}
	// Per-pixel tangent (TBN) is needed by: the deferred kernel's normal-map
	// path (reads gb.tangent only when Mat->NormalMap), AND the rasterizer's
	// parallax UV offset (needs tangent-space view dir). Skip the tangent
	// gradient + G-buffer write otherwise (item 2). The dev nmap_from_diffuse
	// diagnostic forces the normal-map path for every textured face.
	bool writeTangent = (F->Txtr->NormalMap != nullptr) || (heightData != nullptr);
#if FDS_DEV
	if (fds::FeatureFlags::nmap_from_diffuse() && F->Txtr->Txtr) writeTangent = true;
#endif
	meka::TileRasterizerCtx ctx = {
		.V = V,
		.xres = rt.xres,
		.yres = rt.yres,
		.Txtr = F->Txtr->Txtr,
		.matID = F->Txtr->ID,
		.miplevel = miplevel,
		.zbuffer = zbuf,
		.zScale = cam.zScale,
		.heightData = heightData,
		.parallaxStrength = fds::FeatureFlags::parallax_strength() * F->Txtr->ParallaxScale,
		.parallaxMaxOffset = fds::FeatureFlags::parallax_max_offset(),
		.cntrEX = cam.cntrEX,
		.cntrEY = cam.cntrEY,
		.invFOVX = (cam.fovX != 0.0f) ? 1.0f / cam.fovX : 0.0f,
		.invFOVY = (cam.fovY != 0.0f) ? 1.0f / cam.fovY : 0.0f,
		.peelFloor = peelFloorPtr,
		.lmMeshId  = lmMeshId,
		.lmFaceIdx = lmFaceIdx,
		.shadowMatId = shadowMatId,
		.mirrorTag = F->mirrorMaskTag,
		.faceOwnerMirrorId = F->ownerMirrorId,
		.faceBehindMirrorMask = F->behindMirrorMask,
		.writeTangent = writeTangent,
		.mipFrac = meka::g_tlsMipFrac,
		.materialHasHeightMap = (F->Txtr->HeightMap != nullptr),
		.pomSpikeSteps = naiveSteps,
		.pomRefSteps = refSteps,
		.coneData = coneData,
		.pomSteps = coneSteps,
		.pomLodDist = fds::FeatureFlags::parallax_pom_lod(),
		.pomQuarter = fds::FeatureFlags::parallax_pom_quarter(),
		.pomRefine = fds::FeatureFlags::parallax_pom_refine(),
		.pomRelax = fds::FeatureFlags::parallax_pom_relax(),
		// Decode scale for one ConeMap byte. Which encode the resident map uses
		// is decided ONCE, at scene setup, by --pom_cone_exact (the same flag
		// that chose the bake), so the runtime never has to inspect the map.
		// Flag off -> kPomConeMax * 1/255, the identical constant expression the
		// kernel used to fold in-line.
		.coneUnit = (fds::FeatureFlags::pom_cone_exact() > 0
		             ? kPomConeExactMax : kPomConeMax) * (1.0f / 255.0f),
		.pomConeMinStepTexels = fds::FeatureFlags::pom_cone_min_step(),
		.pomEarlyOut = fds::FeatureFlags::pom_march_earlyout(),
		.pomDepthWrite = pomDepthWrite,
		.pomShell = pomShellFace,
		.pomShellUvAmp = pomShellFace ? PomShellFaceUvAmp(F) : 0.0f,
		.pomShellCap = fds::FeatureFlags::pom_shell_cap(),
		.pomShellDomain = fds::FeatureFlags::pom_shell_domain(),
		.pomShellBaseClip = fds::FeatureFlags::pom_shell_base_clip(),
		.pomNormal = fds::FeatureFlags::pom_normal() && (heightData != nullptr)
		             && marchArmed,
		.pomNormalStrength = fds::FeatureFlags::pom_normal_strength(),
		.pomShellBaseClipRaw = fds::FeatureFlags::pom_shell_base_clip_raw(),
		// The lateral-exit domain: the PATCH's UV box (Face::PomShellGroup ->
		// Material::PomShellDomains) when PomShell_Build grouped this face,
		// else the authored face's own box. Either way it comes off the Face,
		// which the frustum clipper and the mipmap poly-split pass through
		// unchanged — so it stays the authored domain at every subdivision.
		.shellUMin = pomShellDom ? pomShellDom[0] : (pomShellFace ? std::min({F->U1, F->U2, F->U3}) : 0.0f),
		.shellUMax = pomShellDom ? pomShellDom[1] : (pomShellFace ? std::max({F->U1, F->U2, F->U3}) : 0.0f),
		.shellVMin = pomShellDom ? pomShellDom[2] : (pomShellFace ? std::min({F->V1, F->V2, F->V3}) : 0.0f),
		.shellVMax = pomShellDom ? pomShellDom[3] : (pomShellFace ? std::max({F->V1, F->V2, F->V3}) : 0.0f),
		.shellSibs = pomShellSibs,
		.shellSibCount = pomShellSibCount,
		.pomViz = fds::FeatureFlags::pom_viz(),
		.pomMipViz = fds::FeatureFlags::pom_mip_viz(),
		.heightLogW = heightLogW,
		.heightLogH = heightLogH,
		.heightUScale = heightUScale,
		.heightVScale = heightVScale,
		.heightUmaskSwizzled = heightUmaskSwizzled,
		.heightVmask = heightVmask,
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
		// One reciprocal instead of four divides (item 1). The inverse
		// Jacobian maps screen-space (dx, dy) deltas to (1/A, 1/B) face-edge
		// barycentric rates.
		const float invDet = 1.0f / det;
		const float im0 =  m[3] * invDet, im1 = -m[1] * invDet;
		const float im2 = -m[2] * invDet, im3 =  m[0] * invDet;

		// Every per-attribute gradient is the SAME solve:
		//   ddx = im0*da + im1*db,  ddy = im2*da + im3*db
		// with da = attr(v2)-attr(v1), db = attr(v3)-attr(v1). Pack the
		// deltas into lane-parallel arrays and evaluate 8 attributes per
		// vector pass (item 5) — the scalar loop was ~22 separate FMA pairs.
		// Lane map: 0 RZ | 1 UZ | 2 VZ | 3 nx | 4 ny | 5 nz |
		//           6 tx | 7 ty | 8 tz | 9 obB | 10 obC  (11..15 unused = 0).
		// Tangent lanes are filled only when the face's material has a normal
		// map (item 2); OrigBary lanes only when the face is lightmapped (item
		// 3) — both gate the per-pixel write/read downstream, and zeroing the
		// gradient keeps the unused tile origin deterministic.
		const bool faceWantTangent = ctx.writeTangent;
		const bool faceWantLm      = (lmMeshId != 0);
		alignas(32) float da[16] = {0}, db[16] = {0};
		da[0] = v2.RZ   - v1.RZ;   db[0] = v3.RZ   - v1.RZ;
		da[1] = v2.UZ   - v1.UZ;   db[1] = v3.UZ   - v1.UZ;
		da[2] = v2.VZ   - v1.VZ;   db[2] = v3.VZ   - v1.VZ;
		da[3] = v2.TN.x - v1.TN.x; db[3] = v3.TN.x - v1.TN.x;
		da[4] = v2.TN.y - v1.TN.y; db[4] = v3.TN.y - v1.TN.y;
		da[5] = v2.TN.z - v1.TN.z; db[5] = v3.TN.z - v1.TN.z;
		if (faceWantTangent) {
			da[6] = v2.TTangent.x - v1.TTangent.x; db[6] = v3.TTangent.x - v1.TTangent.x;
			da[7] = v2.TTangent.y - v1.TTangent.y; db[7] = v3.TTangent.y - v1.TTangent.y;
			da[8] = v2.TTangent.z - v1.TTangent.z; db[8] = v3.TTangent.z - v1.TTangent.z;
		}
		if (ctx.pomShell) {
			// S1b shell entry height, perspective-correct like UZ/VZ: transport
			// ShellH*RZ linearly in screen space, divide by per-pixel RZ.
			const float shZ1 = v1.ShellH * v1.RZ;
			da[11] = v2.ShellH * v2.RZ - shZ1;
			db[11] = v3.ShellH * v3.RZ - shZ1;
		}
		if (faceWantLm) {
			// OrigBary*RZ (perspective-correct transport). Stamped at scene
			// init (A→(0,0), B→(1,0), C→(0,1)); the kernel divides by per-pixel
			// RZ to recover object-space bary on the original (pre-clip) face.
			const float obBZ1 = v1.OrigBaryB * v1.RZ, obBZ2 = v2.OrigBaryB * v2.RZ, obBZ3 = v3.OrigBaryB * v3.RZ;
			const float obCZ1 = v1.OrigBaryC * v1.RZ, obCZ2 = v2.OrigBaryC * v2.RZ, obCZ3 = v3.OrigBaryC * v3.RZ;
			da[9]  = obBZ2 - obBZ1; db[9]  = obBZ3 - obBZ1;
			da[10] = obCZ2 - obCZ1; db[10] = obCZ3 - obCZ1;
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
		r.drzdx = gdx[0];  r.drzdy = gdy[0];
		r.duzdx = gdx[1];  r.duzdy = gdy[1];
		r.dvzdx = gdx[2];  r.dvzdy = gdy[2];
		r.dnxdx = gdx[3];  r.dnxdy = gdy[3];
		r.dnydx = gdx[4];  r.dnydy = gdy[4];
		r.dnzdx = gdx[5];  r.dnzdy = gdy[5];
		r.dtxdx = gdx[6];  r.dtxdy = gdy[6];
		r.dtydx = gdx[7];  r.dtydy = gdy[7];
		r.dtzdx = gdx[8];  r.dtzdy = gdy[8];
		r.dobBdx = gdx[9];  r.dobBdy = gdy[9];
		r.dobCdx = gdx[10]; r.dobCdy = gdy[10];
		r.dshzdx = gdx[11]; r.dshzdy = gdy[11];

		r.umask = (1 << r.LogWidth) - 1;
		r.vmask = (1 << r.LogHeight) - 1;

		// --pom_depth_write: per-triangle world-per-UV-tile density for the
		// depth write's world amplitude (A = parallaxStrength × w). Recover
		// view-space positions + UVs from the raster inputs (PX/PY/RZ, UZ/VZ —
		// valid for ANY post-clip vertex, unlike the transient AoS position),
		// then the Lengyel tangent solve: |dP/du| and |dP/dv| are the world
		// lengths of one UV tile along each axis; geometric mean because the
		// march applies one strength to both axes. Degenerate mapping (zero UV
		// area, behind-camera junk) → w = 0 → flat depth for this triangle.
		// --poly_viz (see the tint site): per-triangle ownership colour.
		if (fds::FeatureFlags::poly_viz()) {
			static const uint32_t kMatHue[12] = {
				0xFFE04040u, 0xFF40E040u, 0xFF4060E0u, 0xFFE0E040u,
				0xFFE040E0u, 0xFF40E0E0u, 0xFFE09030u, 0xFF9040E0u,
				0xFF40A070u, 0xFFB06060u, 0xFF6090B0u, 0xFFC0C0C0u };
			uint32_t c = kMatHue[ctx.matID % 12u];
			// per-triangle hash -> ±20 % brightness so boundaries read
			uint32_t h = (uint32_t(uintptr_t(F) >> 4) * 2654435761u)
			           + (uint32_t(i) * 40503u);
			const int jit = 80 + int((h >> 24) & 0x3F);          // 80..143 of 128
			const int sc  = (ctx.pomShell ? 128 : 58) * jit / 128;  // lid vs not
			uint32_t o = 0xFF000000u;
			for (int ch = 0; ch < 3; ++ch) {
				int v = int((c >> (8 * ch)) & 0xFFu) * sc / 128;
				if (v > 255) v = 255;
				o |= uint32_t(v) << (8 * ch);
			}
			r.polyVizColor = o;
		}
		// --face_id_dump (DIAGNOSTIC, default OFF): stable per-TRIANGLE key.
		// The Face* survives the frustum clipper and the mipmap poly-split
		// unchanged, so bits 31..4 name the AUTHORED polygon however it was
		// subdivided; bits 3..0 name the fan sub-triangle. Snapshot prints the
		// key -> (material, mesh, face index, world plane) table, and reports
		// any key collision rather than hiding it.
		if (fds::FeatureFlags::face_id_dump()) {
			const uint32_t fkey = uint32_t(uintptr_t(F) >> 4) & 0x0FFFFFFFu;
			r.faceIdKey = (fkey << 4) | uint32_t(i & 0xF);
		}
		if (ctx.pomDepthWrite || ctx.pomShell) {
			float w = 0.0f, wAniso = 1.0f;
			if (v1.RZ > 0.0f && v2.RZ > 0.0f && v3.RZ > 0.0f) {
				const float z1 = 1.0f / v1.RZ, z2 = 1.0f / v2.RZ, z3 = 1.0f / v3.RZ;
				const float x1 = (v1.PX - ctx.cntrEX) * ctx.invFOVX * z1;
				const float y1 = (ctx.cntrEY - v1.PY) * ctx.invFOVY * z1;
				const float x2 = (v2.PX - ctx.cntrEX) * ctx.invFOVX * z2;
				const float y2 = (ctx.cntrEY - v2.PY) * ctx.invFOVY * z2;
				const float x3 = (v3.PX - ctx.cntrEX) * ctx.invFOVX * z3;
				const float y3 = (ctx.cntrEY - v3.PY) * ctx.invFOVY * z3;
				const float du1 = v2.UZ * z2 - v1.UZ * z1, dv1 = v2.VZ * z2 - v1.VZ * z1;
				const float du2 = v3.UZ * z3 - v1.UZ * z1, dv2 = v3.VZ * z3 - v1.VZ * z1;
				const float uvDet = du1 * dv2 - du2 * dv1;
				if (std::fabs(uvDet) > 1e-12f) {
					const float inv = 1.0f / uvDet;
					const float tx = ((x2 - x1) * dv2 - (x3 - x1) * dv1) * inv;  // dP/du
					const float ty = ((y2 - y1) * dv2 - (y3 - y1) * dv1) * inv;
					const float tz = ((z2 - z1) * dv2 - (z3 - z1) * dv1) * inv;
					const float bx = ((x3 - x1) * du1 - (x2 - x1) * du2) * inv;  // dP/dv
					const float by = ((y3 - y1) * du1 - (y2 - y1) * du2) * inv;
					const float bz = ((z3 - z1) * du1 - (z2 - z1) * du2) * inv;
					const float t2 = tx * tx + ty * ty + tz * tz;
					const float b2 = bx * bx + by * by + bz * bz;
					w = std::sqrt(std::sqrt(t2 * b2));
					wAniso = (b2 > 1e-20f) ? std::sqrt(std::sqrt(t2 / b2)) : 1.0f;
				}
			}
			// Shell faces take the amplitude from the GEOMETRY (the UV amp the
			// lid was built with) instead of the live strength flag, so the
			// depth the march writes is exactly the slab the lid stands on and
			// a live --parallax_strength change can never desync the march from
			// the built geometry.
			r.pomDepthWorldAmp = (ctx.pomShell ? ctx.pomShellUvAmp
			                                   : ctx.parallaxStrength) * w;
			// --pom_shell_stats: w is the one term that can blow up here (a
			// near-degenerate post-clip sliver divides by a vanishing UV
			// determinant), and it scales BOTH the shell depth and the march,
			// so this is the first thing to check when either looks unbounded.
			if (ctx.pomShell && fds::FeatureFlags::pom_shell_stats()) {
				static std::atomic<int> shown{0};
				static std::atomic<uint32_t> wLo{0x7f7fffffu}, wHi{0};
				const uint32_t wb = *reinterpret_cast<const uint32_t*>(&w);
				uint32_t prev = wLo.load(std::memory_order_relaxed);
				while (wb < prev && !wLo.compare_exchange_weak(prev, wb)) {}
				prev = wHi.load(std::memory_order_relaxed);
				while (wb > prev && !wHi.compare_exchange_weak(prev, wb)) {}
				const int n = shown.fetch_add(1, std::memory_order_relaxed);
				if ((n < 24) || (n % 20000 == 0)) {
					const uint32_t loB = wLo.load(), hiB = wHi.load();
					const float lo = *reinterpret_cast<const float*>(&loB);
					const float hi = *reinterpret_cast<const float*>(&hiB);
					std::fprintf(stderr, "[POM-SHELL-STATS] #%d mat=%s w=%.4f aniso=%.3f A=%.4f "
						"(running w %.4f..%.4f) dom u[%.3f..%.3f] v[%.3f..%.3f] "
						"screenArea=%.0f\n", n,
						(F->Txtr && F->Txtr->Name) ? F->Txtr->Name : "?",
						(double)w, (double)wAniso, (double)r.pomDepthWorldAmp, (double)lo, (double)hi,
						(double)ctx.shellUMin, (double)ctx.shellUMax,
						(double)ctx.shellVMin, (double)ctx.shellVMax,
						(double)std::fabs(det) * 0.5);
				}
			}
		}

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
