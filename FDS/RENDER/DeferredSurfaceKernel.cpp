// Deferred surface-lighting kernels + per-frame orchestrator — the
// kernel half of the former monolithic DeferredLighting.cpp (see
// DeferredCommon.h for the split layout; light culling, volumetric and
// fog passes live in their own TUs).
//
// All variants of the per-pixel tile kernel live here (opaque scalar,
// transparent front/back peel, outer-vec, checkerboard/quarter fill),
// the gloss squaring-chain dispatch, the per-frame view-space light
// build, and the TBR-strip xpar dispatcher RenderXparClumpInStrip.
//
// Public symbols consumed from RENDER.CPP's renderFrame orchestrator:
//   Render_DeferredLighting()        — full deferred pass (tile dispatch)
//   renderDeferredTransparentTile_Front/Back() — wrappers for the per-tile
//                                       transparent-layer composite that
//                                       renderFrame calls inside a runTilePass
//                                       lambda. The template
//                                       stay in this TU.
//   RenderXparClumpInStrip()         — TBR-strip xpar batch dispatch

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <memory.h>
#include <Base/Vector.h>
#include <memory>
#include <vector>
#include <algorithm>
#include <map>
#include <limits>
#include <chrono>
#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#endif
#include "simde/x86/fma.h"

#include "Base/FDS_DEFS.H"
#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"

// FRUSTRUM.CPP — file-scope; forward declare for libm-pow fallback.
extern float fastLog2(float x);
extern float fastPow2(float x);
#include "Base/FeatureFlags.h"
#include "Base/Scene.h"
#include "Base/StaticShadowLightmap.h"
#include "Base/TriMesh.h"
#include "Base/Vertex.h"
#include "Base/Face.h"
#include "Base/Omni.h"
#include "Base/Camera.h"
#include "Base/Material.h"
#include "Base/SpotLight.h"
#include "RenderPipeline.h"
#include "RENDER/DeferredCommon.h"
#include "RENDER/DeferredShadowSampling.h"
#include "RENDER/LightmapBake.h"
#include "RENDER/Hdr.h"  // HDR overlay reorg — xpar peel composites into g_hdrBuf
#include "RENDER/EnvBake.h"  // --env_refl: per-scene panorama for env-specular
#include "TailProf.h"     // phase-1 barrier-tail instrumentation (FDS_TAIL_PROF)
#include "FILLERS/Mekalele.h"
#include "FILLERS/ShadowMap.h"
#include "FILLERS/FILLERS.H"
#include "FRUSTRUM.H"
#include "Threads.h"

// Defined in Transform.cpp; used by RenderXparClumpInStrip.
bool IsFrontFacingInViewSpace(const Face* F);
// Defined in RENDER.CPP.
extern int g_deferredWaterMatID;
// Tile-counter synchronisation primitives, defined in RENDER.CPP.
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <semaphore>
#include <climits>

// RenderContext migration: this TU is GLOBAL-CLEAN — every render pass
// reads its target/projection state from the DeferredLightingCtx param
// (or, in the orchestrator, from the sanctioned MainRenderTargetFromGlobals
// / fds::g_mainCamera accessors on the ov==nullptr path), never from the
// engine globals. The poison makes the compiler enforce it (any new use
// of these names in this file is a build error; read ctx, or take a param).
#pragma GCC poison XRes YRes VPage ZPage16 FOVX FOVY CntrEX CntrEY CurScene VESA_BPSL g_zscale
namespace renderns {
	extern std::counting_semaphore<INT_MAX> tileDone;
	extern std::mutex                tileCounterMutex;
	extern std::atomic<int>          tileCounter;
	extern std::condition_variable   condition;
}

// Cache-line transition stats for shadow-map sampling (gated on
// --shadow_prof_cache). Atomic accumulation across tile workers with
// relaxed ordering; a thread-local tracks the last sample's cache-line
// address so we can count cross-line transitions. Reset and dumped in
// Render_DeferredLighting after the tile barrier.
//
// A "transition" is when the next shadow sample lands on a different
// 64-byte cache line than the previous one on the same thread. Compared
// to total samples this approximates spatial locality: low ratio = most
// samples reuse a recently-touched line; high ratio = lots of misses.
static std::atomic<uint64_t> g_shadowProfSamples{0};
static std::atomic<uint64_t> g_shadowProfLineTransitions{0};
thread_local uintptr_t s_shadowProfLastLine = 0;
// Runtime mip-level debug knobs. Defined here so FDS doesn't need a
// symbol from DEMO. Declared in Rev.h; toggled by N / Shift+N keys.
std::atomic<int>  g_forceMipLevel{-1};
std::atomic<bool> g_vizMipLevel{false};


// Fast scalar 1/sqrt(x) via NEON's frsqrte + one Newton-Raphson step.
// arm64 fsqrt+fdiv is ~24 cycles serial; this is ~5. Accuracy is around
// fast_rsqrt moved to FDS/FILLERS/SimdHelpers.h (shared with Mekalele).

// x^N via binary exponentiation, resolved at compile time. Bit-exact
// vs std::pow for integer N (within float precision; each squaring
// loses ~½ ULP). Optimal op count: 5 squarings + 1 mul for N=48, 6
// squarings for N=64, 7 for N=128, etc. — TMP unrolls the recursion
// into a flat sequence of fmuls. Works for any N ≥ 1; the scalar and
// SIMD overloads of sq()/fmul() let one template body cover both
// float and __m256.
// Decode a tangent-space normal texel → (nmX, nmY, nmZ) in [-1,1]. Branches on
// the texture format (BPP): 32-bit BGRA reads R/G/B directly (unchanged); 16-bit
// RG (MakeNormal16) reads X,Y from the two bytes and RECONSTRUCTS Z = √(1-x²-y²)
// — half the memory/cache, visually equivalent (RG are the same 8-bit values; a
// unit normal's Z is determined by X,Y). Returns false if the mip is absent.
static inline bool decodeNormalTexel(const Texture *t, uint32_t miplevel, uint32_t uv,
                                     float &nmX, float &nmY, float &nmZ) {
	const void *mip = t->Mipmap[miplevel];
	if (!mip) return false;
	if (t->BPP == 16) {
		const uint16_t px = static_cast<const uint16_t *>(mip)[uv];   // R | G<<8
		nmX = (float( px        & 0xFF) * (1.0f/255.0f)) * 2.0f - 1.0f;
		nmY = (float((px >> 8)  & 0xFF) * (1.0f/255.0f)) * 2.0f - 1.0f;
		float z2 = 1.0f - nmX*nmX - nmY*nmY;
		nmZ = z2 > 0.0f ? std::sqrt(z2) : 0.0f;                       // reconstruct
	} else {
		const uint32_t px = static_cast<const uint32_t *>(mip)[uv];   // BGRA: R>>16, G>>8, B
		nmX = (float((px >> 16) & 0xFF) * (1.0f/255.0f)) * 2.0f - 1.0f;
		nmY = (float((px >>  8) & 0xFF) * (1.0f/255.0f)) * 2.0f - 1.0f;
		nmZ = (float( px        & 0xFF) * (1.0f/255.0f)) * 2.0f - 1.0f;
	}
	return true;
}

static inline float  sq(float  x) { return x * x; }
static inline __m256 sq(__m256 x) { return _mm256_mul_ps(x, x); }
static inline float  fmul(float  a, float  b) { return a * b; }
static inline __m256 fmul(__m256 a, __m256 b) { return _mm256_mul_ps(a, b); }

template<int N, typename T>
static inline T pow_squaring(T x) {
	static_assert(N >= 1, "pow_squaring: N must be a positive integer");
	if constexpr (N == 1)         return x;
	else if constexpr (N % 2 == 0) return sq(pow_squaring<N/2,   T>(x));
	else                           return fmul(x, pow_squaring<N-1, T>(x));
}

// Runtime dispatch on Glossiness. The TMP template makes adding a new
// case trivial: just list the gloss value here. Falls back to std::pow
// for anything unlisted (FDS_DEFERRED_GLOSS_STATS will flag it).
static inline float pow_glossClass(float x, unsigned gloss) {
	switch (gloss) {
		case 4:   return pow_squaring<4,   float>(x);
		case 8:   return pow_squaring<8,   float>(x);
		case 16:  return pow_squaring<16,  float>(x);
		case 32:  return pow_squaring<32,  float>(x);
		case 48:  return pow_squaring<48,  float>(x);
		case 64:  return pow_squaring<64,  float>(x);
		case 128: return pow_squaring<128, float>(x);
		default:  return std::pow(x, float(gloss));
	}
}

// Vec8f spec accumulator: walks the per-tile light list, computes
// Blinn-Phong half-vector + N·H, raises to the gloss power via
// pow_squaring<Gloss>, accumulates into sB/sG/sR with the same range
// + falloff masks the diffuse loop uses. Called only when wantSpecular
// is true and Mat->Glossiness matches one of our specialized gloss
// constants (the kernel switches on it; default falls back to scalar
// libm pow). Replaces the scalar spec loop the vec path used before.
//
// Shadow attenuation: the vec diffuse loop now folds the full per-light
// shadow term into `coneShadowAtten` — own cube (resolveCubeAtten) plus
// the mirror-clone source map/cube and own 2-D spot map (computeMapShadowAtten),
// matching the scalar body. This spec loop multiplies that `coneShadowAtten`
// (`csa`) into specStrength, so shadowed surfaces no longer leak specular
// highlights through shadows.
template<int Gloss>
static inline void run_vec_spec_loop(const TileLights &tl,
                                      float x, float y, float z,
                                      float nx, float ny, float nz,
                                      float vx, float vy, float vz,
                                      float matSpec,
                                      uint32_t pmid,
                                      const float *coneShadowAtten,
                                      float &sB, float &sG, float &sR) {
	__m256 vx_v   = _mm256_set1_ps(x);
	__m256 vy_v   = _mm256_set1_ps(y);
	__m256 vz_v   = _mm256_set1_ps(z);
	__m256 vnx_v  = _mm256_set1_ps(nx);
	__m256 vny_v  = _mm256_set1_ps(ny);
	__m256 vnz_v  = _mm256_set1_ps(nz);
	__m256 vvx_v  = _mm256_set1_ps(vx);
	__m256 vvy_v  = _mm256_set1_ps(vy);
	__m256 vvz_v  = _mm256_set1_ps(vz);
	__m256 vSpec  = _mm256_set1_ps(matSpec);
	__m256 vZero  = _mm256_setzero_ps();
	__m256 vOne   = _mm256_set1_ps(1.0f);
	__m256i pmid_v = _mm256_set1_epi32((int)pmid);
	__m256 accB   = _mm256_setzero_ps();
	__m256 accG   = _mm256_setzero_ps();
	__m256 accR   = _mm256_setzero_ps();

	for (int slot = 0; slot < tl.paddedCount; slot += 8) {
		__m256 lpx = _mm256_load_ps(tl.posX   + slot);
		__m256 lpy = _mm256_load_ps(tl.posY   + slot);
		__m256 lpz = _mm256_load_ps(tl.posZ   + slot);
		__m256 lcb = _mm256_load_ps(tl.colB   + slot);
		__m256 lcg = _mm256_load_ps(tl.colG   + slot);
		__m256 lcr = _mm256_load_ps(tl.colR   + slot);
		__m256 lr2 = _mm256_load_ps(tl.range2 + slot);
		__m256 lrr = _mm256_load_ps(tl.rRange + slot);
		// Mirror filter: contrib only lights whose mirrorId matches.
		__m256i lmid = _mm256_load_si256(
			(const __m256i*)(tl.mirrorId + slot));
		__m256 mirrorMask = _mm256_castsi256_ps(
			_mm256_cmpeq_epi32(lmid, pmid_v));

		__m256 wx = _mm256_sub_ps(lpx, vx_v);
		__m256 wy = _mm256_sub_ps(lpy, vy_v);
		__m256 wz = _mm256_sub_ps(lpz, vz_v);
		__m256 dot = _mm256_fmadd_ps(wx, vnx_v,
		              _mm256_fmadd_ps(wy, vny_v,
		               _mm256_mul_ps(wz, vnz_v)));
		__m256 len2 = _mm256_fmadd_ps(wx, wx,
		               _mm256_fmadd_ps(wy, wy,
		                _mm256_mul_ps(wz, wz)));

		__m256 mask_range = _mm256_cmp_ps(len2, lr2,   _CMP_LE_OQ);
		__m256 mask_dot   = _mm256_cmp_ps(dot,  vZero, _CMP_GE_OQ);
		__m256 mask_pos   = _mm256_cmp_ps(len2, vZero, _CMP_GT_OQ);
		__m256 mask       = _mm256_and_ps(mask_range,
		                     _mm256_and_ps(mask_dot,
		                      _mm256_and_ps(mask_pos, mirrorMask)));

		__m256 safe_len2 = _mm256_blendv_ps(vOne, len2, mask);
		__m256 lenInv    = _mm256_rsqrt_ps(safe_len2);
		__m256 dist      = _mm256_mul_ps(safe_len2, lenInv);
		__m256 falloff   = _mm256_sub_ps(vOne, _mm256_mul_ps(dist, lrr));

		// Light direction (normalized w). half = L + V, renormalize.
		__m256 ldx = _mm256_mul_ps(wx, lenInv);
		__m256 ldy = _mm256_mul_ps(wy, lenInv);
		__m256 ldz = _mm256_mul_ps(wz, lenInv);
		__m256 hx  = _mm256_add_ps(ldx, vvx_v);
		__m256 hy  = _mm256_add_ps(ldy, vvy_v);
		__m256 hz  = _mm256_add_ps(ldz, vvz_v);
		__m256 hLen2 = _mm256_fmadd_ps(hx, hx,
		                _mm256_fmadd_ps(hy, hy,
		                 _mm256_mul_ps(hz, hz)));
		__m256 mask_hpos = _mm256_cmp_ps(hLen2, vZero, _CMP_GT_OQ);
		__m256 safe_h2   = _mm256_blendv_ps(vOne, hLen2, mask_hpos);
		__m256 hLenInv   = _mm256_rsqrt_ps(safe_h2);
		// Fold renorm into the N·H dot — 3 fewer fmuls per 8-pixel batch
		// vs scaling H component-wise then dotting. Matches the scalar
		// path's identical fold below.
		__m256 NdotH_raw = _mm256_fmadd_ps(hx, vnx_v,
		                    _mm256_fmadd_ps(hy, vny_v,
		                     _mm256_mul_ps(hz, vnz_v)));
		__m256 NdotH = _mm256_mul_ps(NdotH_raw, hLenInv);
		__m256 mask_nh = _mm256_cmp_ps(NdotH, vZero, _CMP_GT_OQ);
		// Clamp NdotH to [0,1] before squaring so the chain is stable
		// even for masked-out lanes.
		__m256 safeNH = _mm256_max_ps(NdotH, vZero);
		safeNH        = _mm256_min_ps(safeNH, vOne);
		__m256 spec   = pow_squaring<Gloss, __m256>(safeNH);
		// Per-light spot-cone × cube-shadow attenuation, computed once in the
		// diffuse loop and shared here so specular respects shadows + cones
		// (was leaking through both — the documented vec-spec gap).
		__m256 csa = _mm256_load_ps(coneShadowAtten + slot);
		__m256 strength = _mm256_mul_ps(_mm256_mul_ps(_mm256_mul_ps(spec, vSpec), falloff), csa);
		// Apply combined mask (range + dot ≥ 0 + h positive + NdotH > 0)
		__m256 fullMask = _mm256_and_ps(mask, _mm256_and_ps(mask_hpos, mask_nh));
		__m256 contrib  = _mm256_blendv_ps(vZero, strength, fullMask);

		accB = _mm256_fmadd_ps(contrib, lcb, accB);
		accG = _mm256_fmadd_ps(contrib, lcg, accG);
		accR = _mm256_fmadd_ps(contrib, lcr, accR);
	}
	alignas(32) float bufB[8], bufG[8], bufR[8];
	_mm256_store_ps(bufB, accB);
	_mm256_store_ps(bufG, accG);
	_mm256_store_ps(bufR, accR);
	for (int i = 0; i < 8; ++i) {
		sB += bufB[i];
		sG += bufG[i];
		sR += bufR[i];
	}
}

// --pbr TEST: Cook-Torrance microfacet specular (GGX NDF + Smith-Schlick
// geometry + Schlick Fresnel), 8 lights wide, replacing the Blinn-Phong
// run_vec_spec_loop. This is a COST/quality probe — wired into the vec path so
// we can measure SIMD PBR per-light against the existing Blinn-Phong term.
// Divides use _mm256_rcp_ps (fast approx, matches the engine's rcp/rsqrt
// style). roughness ∈ (0,1], F0 = dielectric 0.04 (metallic workflow TBD).
// Specular only — diffuse is accumulated by the caller's existing loop.
static inline void run_vec_ggx_loop(const TileLights &tl,
                                     float x, float y, float z,
                                     float nx, float ny, float nz,
                                     float vx, float vy, float vz,
                                     float matSpec, float roughness,
                                     uint32_t pmid,
                                     const float *coneShadowAtten,
                                     float &sB, float &sG, float &sR) {
	const __m256 vx_v  = _mm256_set1_ps(x),  vy_v = _mm256_set1_ps(y),  vz_v = _mm256_set1_ps(z);
	const __m256 vnx_v = _mm256_set1_ps(nx), vny_v = _mm256_set1_ps(ny), vnz_v = _mm256_set1_ps(nz);
	const __m256 vvx_v = _mm256_set1_ps(vx), vvy_v = _mm256_set1_ps(vy), vvz_v = _mm256_set1_ps(vz);
	const __m256 vSpec = _mm256_set1_ps(matSpec);
	const __m256 vZero = _mm256_setzero_ps();
	const __m256 vOne  = _mm256_set1_ps(1.0f);
	const __m256i pmid_v = _mm256_set1_epi32((int)pmid);
	// GGX constants. a = roughness²; Smith k = a/2 (IBL-less direct-light form).
	const float a  = roughness * roughness;
	const __m256 va2 = _mm256_set1_ps(a * a);
	const __m256 vk  = _mm256_set1_ps(a * 0.5f);
	const __m256 vInvPi = _mm256_set1_ps(0.31830989f);
	const __m256 vF0 = _mm256_set1_ps(0.04f);     // dielectric base reflectance
	// N·V (view-independent of light) — clamp to avoid div blow-up at grazing.
	const float ndotvf = std::max(nx*vx + ny*vy + nz*vz, 1e-3f);
	const __m256 vNdotV = _mm256_set1_ps(ndotvf);
	__m256 accB = _mm256_setzero_ps(), accG = _mm256_setzero_ps(), accR = _mm256_setzero_ps();

	for (int slot = 0; slot < tl.paddedCount; slot += 8) {
		__m256 lpx = _mm256_load_ps(tl.posX + slot), lpy = _mm256_load_ps(tl.posY + slot), lpz = _mm256_load_ps(tl.posZ + slot);
		__m256 lcb = _mm256_load_ps(tl.colB + slot), lcg = _mm256_load_ps(tl.colG + slot), lcr = _mm256_load_ps(tl.colR + slot);
		__m256 lr2 = _mm256_load_ps(tl.range2 + slot), lrr = _mm256_load_ps(tl.rRange + slot);
		__m256i lmid = _mm256_load_si256((const __m256i*)(tl.mirrorId + slot));
		__m256 mirrorMask = _mm256_castsi256_ps(_mm256_cmpeq_epi32(lmid, pmid_v));

		__m256 wx = _mm256_sub_ps(lpx, vx_v), wy = _mm256_sub_ps(lpy, vy_v), wz = _mm256_sub_ps(lpz, vz_v);
		__m256 dot  = _mm256_fmadd_ps(wx, vnx_v, _mm256_fmadd_ps(wy, vny_v, _mm256_mul_ps(wz, vnz_v)));
		__m256 len2 = _mm256_fmadd_ps(wx, wx, _mm256_fmadd_ps(wy, wy, _mm256_mul_ps(wz, wz)));
		__m256 mask = _mm256_and_ps(_mm256_cmp_ps(len2, lr2, _CMP_LE_OQ),
		               _mm256_and_ps(_mm256_cmp_ps(dot, vZero, _CMP_GT_OQ),
		                _mm256_and_ps(_mm256_cmp_ps(len2, vZero, _CMP_GT_OQ), mirrorMask)));
		__m256 safe_len2 = _mm256_blendv_ps(vOne, len2, mask);
		__m256 lenInv = _mm256_rsqrt_ps(safe_len2);
		__m256 dist   = _mm256_mul_ps(safe_len2, lenInv);
		__m256 falloff = _mm256_sub_ps(vOne, _mm256_mul_ps(dist, lrr));

		__m256 ldx = _mm256_mul_ps(wx, lenInv), ldy = _mm256_mul_ps(wy, lenInv), ldz = _mm256_mul_ps(wz, lenInv);
		__m256 NdotL = _mm256_mul_ps(dot, lenInv);   // dot is N·w (unnormalized) → ·lenInv
		// Half vector, normalized.
		__m256 hx = _mm256_add_ps(ldx, vvx_v), hy = _mm256_add_ps(ldy, vvy_v), hz = _mm256_add_ps(ldz, vvz_v);
		__m256 hLen2 = _mm256_fmadd_ps(hx, hx, _mm256_fmadd_ps(hy, hy, _mm256_mul_ps(hz, hz)));
		__m256 hInv = _mm256_rsqrt_ps(_mm256_max_ps(hLen2, _mm256_set1_ps(1e-12f)));
		__m256 NdotH = _mm256_mul_ps(_mm256_fmadd_ps(hx, vnx_v, _mm256_fmadd_ps(hy, vny_v, _mm256_mul_ps(hz, vnz_v))), hInv);
		__m256 VdotH = _mm256_mul_ps(_mm256_fmadd_ps(hx, vvx_v, _mm256_fmadd_ps(hy, vvy_v, _mm256_mul_ps(hz, vvz_v))), hInv);
		NdotH = _mm256_max_ps(NdotH, vZero);
		VdotH = _mm256_max_ps(VdotH, vZero);

		// D (GGX): a² / (π · (NdotH²·(a²-1)+1)²)
		__m256 nh2 = _mm256_mul_ps(NdotH, NdotH);
		__m256 denomD = _mm256_fmadd_ps(nh2, _mm256_sub_ps(va2, vOne), vOne);   // NdotH²(a²-1)+1
		denomD = _mm256_mul_ps(denomD, denomD);                                 // squared
		__m256 D = _mm256_mul_ps(_mm256_mul_ps(va2, vInvPi), _mm256_rcp_ps(_mm256_max_ps(denomD, _mm256_set1_ps(1e-6f))));

		// G (Smith, Schlick-GGX): Gv·Gl, Gx = Ndotx / (Ndotx·(1-k)+k)
		__m256 oneMinusK = _mm256_sub_ps(vOne, vk);
		__m256 Gv = _mm256_mul_ps(vNdotV, _mm256_rcp_ps(_mm256_fmadd_ps(vNdotV, oneMinusK, vk)));
		__m256 Gl = _mm256_mul_ps(NdotL,  _mm256_rcp_ps(_mm256_fmadd_ps(NdotL,  oneMinusK, vk)));
		__m256 G  = _mm256_mul_ps(Gv, Gl);

		// F (Schlick): F0 + (1-F0)·(1-VdotH)^5
		__m256 om = _mm256_sub_ps(vOne, VdotH);
		__m256 om2 = _mm256_mul_ps(om, om);
		__m256 om5 = _mm256_mul_ps(_mm256_mul_ps(om2, om2), om);
		__m256 F = _mm256_fmadd_ps(_mm256_sub_ps(vOne, vF0), om5, vF0);

		// spec = D·G·F / (4·NdotV·NdotL) · NdotL (radiance) = D·G·F/(4·NdotV)
		__m256 specBRDF = _mm256_mul_ps(_mm256_mul_ps(D, G), F);
		__m256 denom = _mm256_mul_ps(_mm256_set1_ps(4.0f), vNdotV);
		__m256 spec = _mm256_mul_ps(specBRDF, _mm256_rcp_ps(denom));   // ·NdotL folded out (radiance) and NdotL/NdotL cancels one
		// spot-cone × cube-shadow attenuation (shared from the diffuse loop).
		__m256 csa = _mm256_load_ps(coneShadowAtten + slot);
		__m256 strength = _mm256_mul_ps(_mm256_mul_ps(_mm256_mul_ps(spec, vSpec), falloff), csa);
		__m256 contrib = _mm256_blendv_ps(vZero, strength, mask);

		accB = _mm256_fmadd_ps(contrib, lcb, accB);
		accG = _mm256_fmadd_ps(contrib, lcg, accG);
		accR = _mm256_fmadd_ps(contrib, lcr, accR);
	}
	alignas(32) float bufB[8], bufG[8], bufR[8];
	_mm256_store_ps(bufB, accB); _mm256_store_ps(bufG, accG); _mm256_store_ps(bufR, accR);
	for (int i = 0; i < 8; ++i) { sB += bufB[i]; sG += bufG[i]; sR += bufR[i]; }
}

// FDS_DEFERRED_VEC=1 in env switches the per-pixel inner loop from the
// scalar branch-predicted early-out to a Vec8f SoA accumulation. Stays
// available for A/B benchmarking — on arm64-via-simde it's slower than
// scalar (Vec8f decomposes to two 128-bit NEON ops with full per-lane
// work, no early-out), but the scaffolding is in place to revisit on
// AVX2 / native NEON / wider lanes.
static bool deferredLightingVecEnabled() {
	return fds::FeatureFlags::deferred_vec();
}

// FDS_DEFERRED_OUTER_VEC=1 selects the outer-SIMD kernel which
// processes 8 pixels per row simultaneously rather than 8 omnis per
// pixel. The setup work (Z-decode, screen→view position reconstruct,
// oct normal decode, ambient compute) amortizes across a vec lane;
// the per-pixel scalar mat-table chase and texel gather stay scalar
// since arm64-via-simde has no usable vgather. The omni loop becomes
// "1 omni × 8 pixels per iter" — each omni's position/color is a
// broadcast, the per-pixel normal/pos/color stay in vec registers.
// Env override: FDS_DEFERRED_OUTER_VEC=1 forces on, =0 forces off,
// unset = follow the per-scene Scene::PreferOuterVec policy. Each
// Initialize_<scene> sets the flag based on whether the scene is
// dominated by matte materials (city, fountain, crash) or by spec/nmap
// shiny ones (greets — outer kernel pays per-lane nmap + scalar
// fallback for most pixels).
static bool deferredLightingOuterVecEnabled(const Scene *sc) {
	// Tri-state: when set explicitly (CLI or env), the flag wins; otherwise
	// fall back to the scene's PreferOuterVec policy. Greets sets it off
	// because the outer kernel's per-lane nmap costs dominate most pixels.
	if (fds::FeatureFlags::isSet(fds::FeatureFlags::BoolId::deferred_outer_vec))
		return fds::FeatureFlags::deferred_outer_vec();
	return sc && sc->PreferOuterVec != 0;
}

// FDS_DEFERRED_CHECKERBOARD=1 enables half-rate lighting: only pixels
// where (px + py) & 1 == 0 (the "even" cells of a checkerboard) get the
// full per-pixel omni evaluation; the odd cells are filled in a second
// wave by averaging two of their already-shaded neighbors when those
// share matID, falling back to full shade when they don't (e.g. across
// material edges). ~50% of pixels skip the omni loop entirely.
static bool deferredLightingCheckerboardEnabled() {
	return fds::FeatureFlags::deferred_checkerboard();
}

// FDS_DEFERRED_QUARTER=1 enables quarter-rate lighting: only pixels
// where (px&1)==0 AND (py&1)==0 (one corner of every 2×2 block) get
// the full kernel; the other 3 of each 2×2 are filled in wave 2 by
// direction-specific interpolation:
//   (odd, even) → horizontal: avg of left+right shaded neighbors
//   (even, odd) → vertical:   avg of top+bottom shaded neighbors
//   (odd,  odd) → diagonal:   avg of 4 corners (top-left, top-right,
//                                              bottom-left, bottom-right)
// matID-mismatch detection at each pattern → fall back to a full
// shade for that pixel. ~75% of pixels skip the omni loop. Mutually
// exclusive with checkerboard (this one wins when both are set).
static bool deferredLightingQuarterEnabled() {
	return fds::FeatureFlags::deferred_quarter();
}


// Shared 2-D-shadow-map attenuation for a single light `n`, extracted from the
// scalar lighting body so both the scalar and the 8-omni SIMD path compute the
// SAME shadowing for mirror-clone source maps (srcSm), mirror-clone source
// cubes (srcCube), and the light's own 2-D spot map (smIdx + PCF). It does NOT
// cover the light's OWN cube (cubeShadowIdx) — that stays at the call site via
// resolveCubeAtten (the scalar block's own-cube path and the vec lane loop both
// already handle it). Returns the combined srcSm×srcCube×smIdx attenuation in
// [0,1]; 0.0 means fully shadowed (callers early-out). wx/wy/wz/lenInv are the
// light→pixel vector + 1/|w| (used only by the smIdx slope bias). Byte-identical
// to the inlined scalar code it replaced.
static inline float computeMapShadowAtten(const TileLights& tl, int n,
                                          const DeferredLightingCtx& ctx,
                                          float x, float y, float z,
                                          float wx, float wy, float wz,
                                          float lenInv,
                                          float nGeoX, float nGeoY, float nGeoZ,
                                          int surfaceShadowId,
                                          int kShadowBiasG, int kSlopeBiasG,
                                          bool profShadowCache)
{
	const int32_t smIdx = tl.shadowMapIdx[n];
	float shadowAtten = 1.0f;
	// Clone light (mirror reflection): its visibility of this pixel equals
	// the SOURCE light's visibility of the pixel REFLECTED across the mirror
	// plane — sample the source's existing map there. Zero extra bakes;
	// single tap (no PCF — reflected dot/beam shadows are soft).
	const int32_t srcSm = tl.srcShadowMapIdx[n];
	if (srcSm >= 0 && size_t(srcSm) < g_shadowMaps.size()) {
		// A clone/bounce light borrows its SOURCE spot's 2-D map: reflect this
		// pixel across the mirror plane and sample the source's map.
		//
		// BOUNCE spots (clampBounce) own no map and may only relight what the
		// source actually illuminates, so they default to DARK and are lit only
		// where the reflected point lands inside the source's cone AND the
		// source sees it — out-of-cone / occluded stay dark. Without this the
		// bounce spilled disco light onto walls the source could never reach
		// (the through-mirror bleed).
		//
		// Mirror CLONES keep the established default-lit behaviour
		// (occluded → dark).
		const bool clampBounce = (tl.bounceClamp[n] != 0u);
		if (clampBounce) shadowAtten = 0.0f;
		const ShadowMap& sm = g_shadowMaps[srcSm];
		const float wpx = ctx.viewToWorld[0][0]*x + ctx.viewToWorld[0][1]*y + ctx.viewToWorld[0][2]*z + ctx.cameraWorldX;
		const float wpy = ctx.viewToWorld[1][0]*x + ctx.viewToWorld[1][1]*y + ctx.viewToWorld[1][2]*z + ctx.cameraWorldY;
		const float wpz = ctx.viewToWorld[2][0]*x + ctx.viewToWorld[2][1]*y + ctx.viewToWorld[2][2]*z + ctx.cameraWorldZ;
		const float sd2 = 2.0f * (tl.mirNX[n]*wpx + tl.mirNY[n]*wpy + tl.mirNZ[n]*wpz + tl.mirD[n]);
		const float rwx = wpx - sd2 * tl.mirNX[n];
		const float rwy = wpy - sd2 * tl.mirNY[n];
		const float rwz = wpz - sd2 * tl.mirNZ[n];
		const float ddx = rwx - ctx.cameraWorldX;
		const float ddy = rwy - ctx.cameraWorldY;
		const float ddz = rwz - ctx.cameraWorldZ;
		// worldToView = transpose(viewToWorld)
		const float rvx = ctx.viewToWorld[0][0]*ddx + ctx.viewToWorld[1][0]*ddy + ctx.viewToWorld[2][0]*ddz;
		const float rvy = ctx.viewToWorld[0][1]*ddx + ctx.viewToWorld[1][1]*ddy + ctx.viewToWorld[2][1]*ddz;
		const float rvz = ctx.viewToWorld[0][2]*ddx + ctx.viewToWorld[1][2]*ddy + ctx.viewToWorld[2][2]*ddz;
		const float lx = sm.viewToLight[0][0]*rvx + sm.viewToLight[0][1]*rvy + sm.viewToLight[0][2]*rvz + sm.viewToLightOffset.x;
		const float ly = sm.viewToLight[1][0]*rvx + sm.viewToLight[1][1]*rvy + sm.viewToLight[1][2]*rvz + sm.viewToLightOffset.y;
		const float lz = sm.viewToLight[2][0]*rvx + sm.viewToLight[2][1]*rvy + sm.viewToLight[2][2]*rvz + sm.viewToLightOffset.z;
		if (lz > 0.0f) {
			const float invLZ = 1.0f / lz;
			const int iX = int(sm.cntrX + sm.perspX * lx * invLZ);
			const int iY = int(sm.cntrY - sm.perspY * ly * invLZ);
			if (iX >= 0 && iX < sm.xres && iY >= 0 && iY < sm.yres) {
				const size_t o = size_t(iY) * size_t(sm.xres) + size_t(iX);
				const uint16_t zS = std::max(sm.depth[o], sm.depth_dynamic[o]);
				int pixZ = 0xFF80 - int(lz * sm.zScale);
				if (pixZ < 0) pixZ = 0;
				if (clampBounce) {
					// In-cone + the source sees this point (not behind a closer
					// occluder) → bounce lights it; everything else stays dark
					// (set above).
					if (pixZ + 128 >= int(zS)) shadowAtten = 1.0f;
				} else {
					// Clone: default lit, darken only where the source's map
					// shows a closer occluder.
					if (pixZ + 128 < int(zS)) shadowAtten = 0.0f;
				}
			}
		}
	}
	// Clone omni with a CUBE source: borrow the SOURCE omni's cube and sample
	// it at this pixel REFLECTED across the mirror plane — the cube analogue of
	// the srcShadowMapIdx path above. PolyId mode (surfaceShadowId): the
	// reflected receiver lands on the SOURCE surface (the clone is that geometry
	// flipped) and clones share the source's matID, so the identity test matches
	// — same acne-free path as the rest of greets. No clone geometry is baked.
	const int32_t srcCube = tl.srcCubeShadowIdx[n];
	if (srcCube >= 0) {
		const float wpx = ctx.viewToWorld[0][0]*x + ctx.viewToWorld[0][1]*y + ctx.viewToWorld[0][2]*z + ctx.cameraWorldX;
		const float wpy = ctx.viewToWorld[1][0]*x + ctx.viewToWorld[1][1]*y + ctx.viewToWorld[1][2]*z + ctx.cameraWorldY;
		const float wpz = ctx.viewToWorld[2][0]*x + ctx.viewToWorld[2][1]*y + ctx.viewToWorld[2][2]*z + ctx.cameraWorldZ;
		const float sd2 = 2.0f * (tl.mirNX[n]*wpx + tl.mirNY[n]*wpy + tl.mirNZ[n]*wpz + tl.mirD[n]);
		const float rwx = wpx - sd2 * tl.mirNX[n];
		const float rwy = wpy - sd2 * tl.mirNY[n];
		const float rwz = wpz - sd2 * tl.mirNZ[n];
		const float ddx = rwx - ctx.cameraWorldX, ddy = rwy - ctx.cameraWorldY, ddz = rwz - ctx.cameraWorldZ;
		const float rvx = ctx.viewToWorld[0][0]*ddx + ctx.viewToWorld[1][0]*ddy + ctx.viewToWorld[2][0]*ddz;
		const float rvy = ctx.viewToWorld[0][1]*ddx + ctx.viewToWorld[1][1]*ddy + ctx.viewToWorld[2][1]*ddz;
		const float rvz = ctx.viewToWorld[0][2]*ddx + ctx.viewToWorld[1][2]*ddy + ctx.viewToWorld[2][2]*ddz;
		shadowAtten *= CubeShadow_Sample(srcCube, rwx, rwy, rwz, rvx, rvy, rvz,
		                                 kShadowBiasG, kSlopeBiasG, surfaceShadowId);
	}
	if (smIdx >= 0 && size_t(smIdx) < g_shadowMaps.size()) {
		const ShadowMap& sm = g_shadowMaps[smIdx];
		const float lx = sm.viewToLight[0][0] * x +
		                 sm.viewToLight[0][1] * y +
		                 sm.viewToLight[0][2] * z +
		                 sm.viewToLightOffset.x;
		const float ly = sm.viewToLight[1][0] * x +
		                 sm.viewToLight[1][1] * y +
		                 sm.viewToLight[1][2] * z +
		                 sm.viewToLightOffset.y;
		const float lz = sm.viewToLight[2][0] * x +
		                 sm.viewToLight[2][1] * y +
		                 sm.viewToLight[2][2] * z +
		                 sm.viewToLightOffset.z;
		if (lz > 0.0f) {
			const float invLZ = 1.0f / lz;
			const float smX = sm.cntrX + sm.perspX * lx * invLZ;
			const float smY = sm.cntrY - sm.perspY * ly * invLZ;
			const int iX = int(smX);
			const int iY = int(smY);
			if (iX >= 0 && iX + 1 < sm.xres &&
			    iY >= 0 && iY + 1 < sm.yres) {
				// Tap addressing: linear row-major, or 8×8-tiled under
				// --shadow-swizzle (see CubeShadow_Sample / ShadowSwzOffset;
				// halves the cache lines a 2×2 footprint touches). Falls back
				// to linear when the tiled copies aren't built — both layouts
				// hold identical values.
				const bool swz = fds::FeatureFlags::shadow_swizzle()
				              && !sm.depthSw.empty() && !sm.depthDynSw.empty()
				              && !sm.polyIdSw.empty();
				size_t o00, o10, o01, o11;
				const uint16_t *zsB, *zdB, *idB;
				if (swz) {
					const ShadowSwzShape &shp = ShadowSwzGetShape();
					const int tpr = ShadowSwzTilesPerRow(sm.xres, shp);
					o00 = ShadowSwzOffset(iX,     iY,     tpr, shp);
					o10 = ShadowSwzOffset(iX + 1, iY,     tpr, shp);
					o01 = ShadowSwzOffset(iX,     iY + 1, tpr, shp);
					o11 = ShadowSwzOffset(iX + 1, iY + 1, tpr, shp);
					zsB = sm.depthSw.data(); zdB = sm.depthDynSw.data();
					idB = sm.polyIdSw.data();
				} else {
					const size_t rowOfs = size_t(iY) * size_t(sm.xres);
					o00 = rowOfs + size_t(iX);   o10 = o00 + 1;
					o01 = o00 + size_t(sm.xres); o11 = o01 + 1;
					zsB = sm.depth.data(); zdB = sm.depth_dynamic.data();
					idB = sm.polyId.data();
				}
				// Per-tap closest-occluder. Static buffer holds the once-baked
				// statics; dynamic holds animated meshes (zero when off).
				// max() wins on whichever caster is closer.
				const uint16_t z00 = std::max(zsB[o00], zdB[o00]);
				const uint16_t z10 = std::max(zsB[o10], zdB[o10]);
				const uint16_t z01 = std::max(zsB[o01], zdB[o01]);
				const uint16_t z11 = std::max(zsB[o11], zdB[o11]);
				if (profShadowCache) {
					// One PCF check = one tracked sample. Use the (00) tap's
					// cache-line address — adjacent shadow checks on the same
					// thread that share this line are hits.
					const uintptr_t line =
						reinterpret_cast<uintptr_t>(&zsB[o00]) >> 6;
					if (s_shadowProfLastLine != line) {
						g_shadowProfLineTransitions.fetch_add(1, std::memory_order_relaxed);
						s_shadowProfLastLine = line;
					}
					g_shadowProfSamples.fetch_add(1, std::memory_order_relaxed);
				}
				const float fx = smX - float(iX);
				const float fy = smY - float(iY);
				const float w00 = (1.0f - fx) * (1.0f - fy);
				const float w10 =         fx  * (1.0f - fy);
				const float w01 = (1.0f - fx) *         fy;
				const float w11 =         fx  *         fy;
				const ShadowMode mode = g_shadowMode.load(std::memory_order_relaxed);
				float occ = 0.0f;
				if (mode == ShadowMode::PolyId) {
					// Surface matID extracted from gb.txtr's packed
					// (miplevel:4 | matID:8 | swizzledUV:20). Shadow buffer
					// stores matID+1 of the closest occluder; +1 here too so the
					// comparison uses the same offset, and 0 stays as the
					// "no occluder" sentinel.
					const uint16_t surfaceId = uint16_t(surfaceShadowId);
					if (idB[o00] != surfaceId && idB[o00] != 0) occ += w00;
					if (idB[o10] != surfaceId && idB[o10] != 0) occ += w10;
					if (idB[o01] != surfaceId && idB[o01] != 0) occ += w01;
					if (idB[o11] != surfaceId && idB[o11] != 0) occ += w11;
				} else {
					int pixZenc = 0xFF80 - int(lz * sm.zScale);
					if (pixZenc < 0) pixZenc = 0;
					if (pixZenc > 0xFFFF) pixZenc = 0xFFFF;
					// Constant + slope-scale bias. See the inlined scalar comment
					// for the rationale; the slope grows as the surface meets the
					// light at shallow angles, plateauing at the 0.2 clamp.
					const int kShadowBias = kShadowBiasG;
					const int kSlopeBias  = kSlopeBiasG;
					const float dotGeo = wx*nGeoX + wy*nGeoY + wz*nGeoZ;
					const float nDotL = dotGeo * lenInv;
					const float invNdotL = 1.0f / (nDotL > 0.2f ? nDotL : 0.2f);
					const int slopeBias = int(float(kSlopeBias) * (invNdotL - 1.0f));
					const int biased = pixZenc + kShadowBias + slopeBias;
					if (biased < int(z00)) occ += w00;
					if (biased < int(z10)) occ += w10;
					if (biased < int(z01)) occ += w01;
					if (biased < int(z11)) occ += w11;
				}
				if (occ >= 1.0f) return 0.0f;       // fully shadowed
				shadowAtten = 1.0f - occ;
			}
		}
	}
	return shadowAtten;
}

static void Render_DeferredLighting_Tile(const DeferredLightingCtx &ctx,
                                          int tileIndex,
                                          int x1, int y1, int x2, int y2)
{
	// Render-target addressing from ctx, not globals (RenderContext
	// migration). Locals shadow the engine globals so the body is untouched.
	const int xres = ctx.xres;
	byte *const vpage = ctx.vpage;
	word *const zpage16 = ctx.zpage16;
	const float cntrEX = ctx.cntrEX, cntrEY = ctx.cntrEY;
	const meka::GBuffer &gb = *ctx.gb;
	dword *out = reinterpret_cast<dword *>(vpage);
	// Hoist mode/global queries once per tile — a `getenv()`-backed
	// cached bool still costs a function call + load + test that the
	// compiler can't see through the std::function/lambda boundary.
	// 2M function calls/frame adds up.
	const bool quarter        = deferredLightingQuarterEnabled();
	const bool checker        = deferredLightingCheckerboardEnabled() && !quarter;
	const bool useVec         = deferredLightingVecEnabled();
	const bool sVecForce      = fds::FeatureFlags::deferred_vec_force();
	const bool specGlobalOn   = Specular_Factor > 0.0f;
	const bool profShadowCache = fds::FeatureFlags::shadow_prof_cache();
	// HDR Phase 3 B1: route the opaque lit radiance into g_hdrBuf UNCLAMPED so
	// bright surfaces bloom/roll-off at the tonemap instead of clipping at the
	// 8-bit vpage. The composite reads this back (coverage flag in h[3]) rather
	// than lifting the already-clamped vpage. Gated on hdr(): when off, no write,
	// LDR byte-identical; when on but the froxel composite doesn't run, g_hdrActive
	// stays false and the tonemap no-ops, so the buffer is harmlessly ignored.
	// Hdr_WritableFor: only the MAIN pass (g_hdrBuf sized for these dims) may
	// write it — the order-2 mirror RTT calls this kernel directly with its own
	// (smaller) dims and never ran Hdr_BeginFrame, so g_hdrBuf is unsized there
	// (writing it = null deref / wrong-res index).
	const bool hdrWrite = fds::FeatureFlags::hdr() && fds::Hdr_WritableFor(ctx.xres, ctx.yres);
	// HDR Phase 3 B2: when --hdr_linear, do the lighting math in LINEAR space —
	// square the (normalized) albedo and let light enter at power 1 (B1/gamma
	// effectively squares the light too). Store the result re-encoded to gamma
	// (sqrt) so the buffer stays gamma-coherent with the composite + overlays +
	// the gamma tonemap-decode that --hdr_linear already applies; the decode
	// recovers the true linear radiance. Contained to this write — no composite/
	// overlay/tonemap change. Off → B1 gamma radiance.
	const bool hdrLinear = hdrWrite && fds::FeatureFlags::hdr_linear();

	// Per-stage ablation gates. Set one of these on to short-circuit the
	// stage so a bench harness can measure its cost from the frame-time
	// delta (see `--bench=scene@...`).
	const bool profNoTex    = fds::FeatureFlags::prof_no_tex();
	const bool profNoLights = fds::FeatureFlags::prof_no_lights();
	const bool profNoSpec   = fds::FeatureFlags::prof_no_spec();
	const bool profNoFog    = fds::FeatureFlags::prof_no_fog();

	// Hot-loop flag cache. These flags were being queried per-pixel +
	// per-light + per-PCF-tap, racking up ~1.5% CPU in profile data on
	// greets@t=2500. FeatureFlags::get is a cached load each, but a
	// per-pixel load × 1920×1080 × 4 lights × 4 PCF taps adds up. Hoist
	// to function entry.
#if FDS_DEV
	const bool nmapFromDiffuseG = fds::FeatureFlags::nmap_from_diffuse();
	const bool aoFromDiffuseG   = fds::FeatureFlags::ao_from_diffuse();
#else
	constexpr bool nmapFromDiffuseG = false;
	constexpr bool aoFromDiffuseG   = false;
#endif
	const bool  roughMapOnG     = fds::FeatureFlags::roughness_map();
	const float roughStrengthG  = fds::FeatureFlags::roughness_strength();
	const bool  aoMapOnG        = fds::FeatureFlags::ao_map();
	const float aoStrengthG     = fds::FeatureFlags::ao_map_strength();
	const bool nmapDisabledG    = fds::FeatureFlags::no_nmap();
	const bool deferredNoSpecG  = fds::FeatureFlags::deferred_no_spec();
	const bool sPbr             = fds::FeatureFlags::pbr();
	const float sPbrRoughFixed  = fds::FeatureFlags::pbr_roughness();
	// Env-specular reflection (--env_refl): per-SURFACE baked panoramas,
	// matID-indexed (each reflective material's pano is captured from its own
	// centroid; the lookup is parallax-corrected against the scene AABB).
	const fds::EnvPanoLinear *const *envTabG = fds::FeatureFlags::env_refl()
		? fds::EnvReflection_Table(ctx.Sc) : nullptr;
	// Raw gain: the material's Reflection% now enters through Fresnel F0
	// (F0 = Reflection/100), so the gain is a plain multiplier on top.
	const float envReflGainG    = fds::FeatureFlags::env_refl_gain();
	const bool  metalMapOnG     = fds::FeatureFlags::metal_map();
	const int  kShadowBiasG     = fds::FeatureFlags::shadow_bias();
	const int  kSlopeBiasG      = fds::FeatureFlags::shadow_slope_bias();
	// Lightmap kernel gate. Historically --shadow-dynamic disabled the
	// lightmap fast path ENTIRELY (the dynamic buffers are invisible to the
	// static atlas), forcing full cube taps for every static light at every
	// pixel — measured 2.6ms of lighting-w1 on greets. resolveCubeAtten has
	// since grown the proper composite (lightmap static factor × dynamic-only
	// cube tap, gated per-face on ShadowMap::dynBaked), so the lightmap stays
	// on. --no-shadow_lm_dynamic restores the old full-tap fallback for A/B.
	const bool lmKernelEnabled  = !fds::FeatureFlags::shadow_dynamic()
	                            || fds::FeatureFlags::shadow_lm_dynamic();
	// Normal-map LOD fade. The texture mip-chain averages cleanly, but
	// averaged normals shorten + rotate toward the surface average, so
	// at distance the bump's perturbation becomes high-frequency lighting
	// noise (hard edges on the floor especially). Fade nmX/nmY linearly
	// toward zero at high mips; nmZ unchanged so perturbed N smoothly
	// converges to the geometric N. Per-pixel cost: one int compare +
	// one float mul + one max — cheap vs the full TBN block.
	const int   nmapFadeStart = fds::FeatureFlags::nmap_lod_fade_start();
	const float nmapFadeStep  = fds::FeatureFlags::nmap_lod_fade_step();
	// Runtime mip-level debug knobs (toggled via N / Shift+N in REV.CPP).
	// Defined here and declared extern in Rev.h. Read once per tile entry
	// to avoid the atomic load on every pixel.
	const int  forceMipLevel = ::g_forceMipLevel.load(std::memory_order_relaxed);
	const bool vizMipLevel   = ::g_vizMipLevel.load(std::memory_order_relaxed);
	// Cube-tap flag bundle. resolveCubeAtten was reading 6 flags + 1
	// atomic per cube tap (1.44M taps/frame at greets t=500). Hoist to
	// tile-level: ~10 micros/frame back across all tile workers, and
	// removes a tail of L1 cold reads from the inner loop.
	const CubeAttenFlags caFlags{
	    /*shadowDynamicOn      */ fds::FeatureFlags::shadow_dynamic(),
	    /*lightmapPlanar       */ fds::FeatureFlags::shadow_lightmap_planar(),
	    /*lightmapNearest      */ fds::FeatureFlags::shadow_lightmap_nearest(),
	    /*lightmapRecomputeBake*/ fds::FeatureFlags::shadow_lightmap_recompute_bake(),
	    /*lightmapRecomputeBary*/ fds::FeatureFlags::shadow_lightmap_recompute_at_bary(),
	    /*profNoCubeTap        */ fds::FeatureFlags::prof_no_cube_tap(),
	    /*shadowMode           */ g_shadowMode.load(std::memory_order_relaxed),
	};

	// [DIAG] FDS_CONTRIB_CULL: per-light MAX linear diffuse contribution over this
	// tile, matching the HDR-linear accumulation below (albedo²·intensity·color).
	// After the pixel loop we count lights whose max contribution is below visible
	// thresholds = the contribution-cull potential. Diffuse-only (spec excluded);
	// linear-radiance units, PRE-tonemap. Load-independent (it's a count). Gated;
	// negligible when off. Per-tile stack array → no cross-thread race; atomics
	// aggregate at the end. Covers the scalar path (= all of greets: nmap forces
	// scalar); vec-path pixels aren't tracked.
	static const bool s_contribProf = (std::getenv("FDS_CONTRIB_CULL") != nullptr);
	alignas(64) float contribMax[DEFERRED_MAX_LIGHTS];
	const int contribN = s_contribProf
	    ? std::min(ctx.tileLights[tileIndex].count, int(DEFERRED_MAX_LIGHTS)) : 0;
	for (int ci = 0; ci < contribN; ++ci) contribMax[ci] = 0.0f;

	for (int py = y1; py < y2; ++py) {
		for (int px = x1; px < x2; ++px) {
			// Wave-1 of checkerboard: skip odd cells (filled by the
			// fill-pass after all wave-1 tiles complete).
			if (checker && ((px ^ py) & 1)) continue;
			if (quarter && ((px | py) & 1)) continue;  // shade only (even, even)

			const size_t i = size_t(py) * xres + px;
			const word zEnc = zpage16[i];
			if (zEnc == 0) continue;  // pixel not touched by Mekalele

			// Per-pixel mirror id (0 = original world, >0 = mirror N's
			// reflected world). The light-loop filters below skip any
			// omni whose mirrorId disagrees, so clone surfaces only
			// receive light from their own cloned omnis. If the mirror
			// id plane was never allocated (no mirrors in the scene),
			// pmid stays 0 and the filter is a no-op for the originals.
			const uint32_t pmid = gb.mirrorId.empty()
			    ? 0u : uint32_t(gb.mirrorId[i]);

			// Decode mat32 → matID, miplevel, swizzledUV.
			const uint32_t mat32 = gb.txtr[i];
			const uint32_t miplevel    = (mat32 >> 28) & 0xF;
			const uint32_t matID       = (mat32 >> 20) & 0xFF;
			const uint32_t swizzledUV  = mat32 & 0xFFFFF;
			if (matID >= ctx.matTable.count) continue;
			Material *Mat = ctx.matTable.data[matID];
			if (!Mat || !Mat->Txtr) continue;
			// Force-mip (N key): override the value used for nmap-fade
			// math only. Texture sampling still uses the rasterizer-chosen
			// mip — overriding it there would break swizzledUV (which is
			// encoded against the chosen mip's dimensions) and produce
			// garbage. This way, the user can verify whether the nmap LOD
			// fade is wired correctly by forcing "as if mip 5" and seeing
			// whether the bump effect drops to zero on screen.
			const uint32_t miplevelForFade = (forceMipLevel >= 0)
			    ? uint32_t(forceMipLevel & 7) : miplevel;
			// Mip-level viz (Shift+N): paint each pixel by its raw
			// (rasterizer-chosen) miplevel. Suppresses texturing +
			// lighting; pure color = mip indicator.
			if (vizMipLevel) {
				static constexpr dword kMipPalette[8] = {
				    0xFFFF0000u, // 0 red
				    0xFFFF8000u, // 1 orange
				    0xFFFFFF00u, // 2 yellow
				    0xFF00FF00u, // 3 green
				    0xFF0080FFu, // 4 blue
				    0xFF4B00FFu, // 5 indigo
				    0xFF8000FFu, // 6 violet
				    0xFFFFFFFFu, // 7 white
				};
				out[i] = kMipPalette[miplevel & 7];
				continue;
			}

			// Resolved 16-bit ShadowMatID for the cube polyId path.
			// Source of truth is the per-pixel `gb.shadowMatID` plane
			// stamped by Mekalele (per-face resolution of
			// F->ShadowMatID / F->Txtr->ShadowMatID / Txtr->ID+1).
			// When the plane is empty (non-opaque renderers, or scenes
			// where the plane wasn't allocated), fall back to the
			// legacy uint16_t(matID+1) decoded from `txtr`.
			const int surfaceShadowId = gb.shadowMatID.empty()
			    ? int(matID + 1)
			    : int(gb.shadowMatID[i]);

			// Texture sample: Mekalele's apply_exact already wrote a
			// swizzled offset into mat32, so it's a direct lookup into
			// the mip's tile-major data. Mipmap[k] is byte*; texels are
			// dword (B,G,R,A in low→high bytes).
			// Cached-once-per-frame debug viz switches (FeatureFlags reads
			// env at startup; per-pixel cost is one bool load each).
#if FDS_DEV
			static const bool sVizTangent     = fds::FeatureFlags::viz_tangent();
			static const bool sVizNormal      = fds::FeatureFlags::viz_normal();
			static const bool sVizGeoNormal   = fds::FeatureFlags::viz_geonormal();
			static const bool sVizMatID       = fds::FeatureFlags::viz_matid();
			static const bool sVizPmid        = fds::FeatureFlags::viz_pmid();
			static const bool sNmapAsDiffuse  = fds::FeatureFlags::nmap_as_diffuse();
#else
			constexpr bool sVizTangent    = false;
			constexpr bool sVizNormal     = false;
			constexpr bool sVizGeoNormal  = false;
			constexpr bool sVizMatID      = false;
			constexpr bool sVizPmid       = false;
			constexpr bool sNmapAsDiffuse = false;
#endif
			float texB, texG, texR;
			float texA = 255.0f;   // albedo alpha (Mat_AoInAlpha packs AO here)
			if (profNoTex) {
				texB = texG = texR = 128.0f;
			} else {
				const Texture *srcTex = Mat->Txtr;
				if (sNmapAsDiffuse && Mat->NormalMap) srcTex = Mat->NormalMap;
				const dword *texData = (const dword *)srcTex->Mipmap[miplevel];
				if (!texData) continue;
				const dword texel = texData[swizzledUV];
				texB = float(texel & 0xFF);
				texG = float((texel >> 8) & 0xFF);
				texR = float((texel >> 16) & 0xFF);
				texA = float((texel >> 24) & 0xFF);
			}

			// Static-shadow lightmap address for this pixel — resolved
			// once, used by all cube-shadow taps in the per-omni loops
			// below via resolveCubeAtten().
			const PixelLightmap pixelLM = resolvePixelLightmap(gb, i, ctx.Sc);
			// Lightmap kernel branch is disabled when the dynamic-mesh
			// shadow pass is on: the lightmap only encodes the static-
			// occluder polyId, but `--shadow-dynamic` puts moving meshes
			// into a parallel buffer that only the runtime cube tap
			// reads. Until we composite both, fall back to cube tap
			// when dynamic is on so robot shadows on the floor still
			// render. (Set once per tile, see lmKernelEnabled below.)

			// Decode normal (view-space, unit-length).
			float nx, ny, nz;
			meka::oct_decode_u16(gb.normal[i], nx, ny, nz);
			// Save the geometric (vertex-interpolated, un-perturbed)
			// normal before the normal-map block below mutates nx/ny/nz.
			// Used by the shadow slope-bias so per-pixel bump-induced
			// N·L variance doesn't pollute the slope decision (which is
			// about the underlying surface geometry, not the apparent
			// micro-roughness from the bump map).
			const float nGeoX = nx, nGeoY = ny, nGeoZ = nz;

			// Object-space normal map: if the material ships one, sample
			// it via the same swizzled UV the diffuse uses, decode the
			// (R,G,B) bytes to a unit vector in world space, and replace
			// the geometric N with it. Static meshes only — for dynamic
			// meshes we'd need to transform from mesh-local to world via
			// a per-mesh RotMat, which we don't have at lighting time.
			//
			// FDS_NMAP_FROM_DIFFUSE=1: re-purposes the diffuse texel as
			// the normal map. Visually wrong but lets us measure the
			// per-pixel cost of the normal-map code path on existing
			// scenes without authoring or baking any new textures.
			Texture* nmTex = nmapDisabledG ? nullptr : Mat->NormalMap;
			if (!nmTex && nmapFromDiffuseG) nmTex = Mat->Txtr;
			if (nmTex) {
				float nmX, nmY, nmZ;
				if (decodeNormalTexel(nmTex, miplevel, swizzledUV, nmX, nmY, nmZ)) {
					// LOD-aware bump fade: scale (nmX, nmY) toward zero at
					// high mip. Uses miplevelForFade so the N-key override
					// can simulate "as if at higher mip" without touching
					// texture sampling. Fade starts AT nmapFadeStart so
					// picking start=0 starts cutting at mip 0 (1-step).
					if (int(miplevelForFade) >= nmapFadeStart) {
						const int   over = int(miplevelForFade) - nmapFadeStart + 1;
						const float fade = 1.0f - float(over) * nmapFadeStep;
						const float s = fade > 0.0f ? fade : 0.0f;
						nmX *= s; nmY *= s;
					}
					// Tier B tangent-space normal map. Reconstruct view-space
					// N from TBN where T is the per-pixel interpolated tangent
					// from the rasterizer's tangent G-buffer (Gram-Schmidt'd
					// vs N at write time; we re-orthogonalize defensively after
					// nlerp + oct-quant round-trip). When the tangent slot is
					// empty (transparent layers, or vertex tangent collapsed)
					// fall back to a Mikkelsen-style on-the-fly tangent so the
					// effect is still visible — visually wrong rotation but
					// the right per-pixel cost.
					float tx = 0, ty = 0, tz = 0;
					bool tangentValid = false;
					if (!gb.tangent.empty()) {
						const meka::u16 packedT = gb.tangent[i];
						if (packedT != 0) {
							meka::oct_decode_u16(packedT, tx, ty, tz);
							// Re-orthogonalize after the lossy oct round-trip.
							const float tDotN = tx*nx + ty*ny + tz*nz;
							tx -= nx * tDotN;
							ty -= ny * tDotN;
							tz -= nz * tDotN;
							const float tLen2 = tx*tx + ty*ty + tz*tz;
							if (tLen2 > 1e-12f) {
								const float invTLen = fast_rsqrt(tLen2);
								tx *= invTLen; ty *= invTLen; tz *= invTLen;
								tangentValid = true;
							}
						}
					}
					if (!tangentValid) {
						// Mikkelsen fallback: pick a reference axis not
						// parallel to N, T = normalize(cross(ref, N)).
						float refx, refy, refz;
						if (std::fabs(ny) < 0.9f) { refx = 0; refy = 1; refz = 0; }
						else                       { refx = 1; refy = 0; refz = 0; }
						tx = refy * nz - refz * ny;
						ty = refz * nx - refx * nz;
						tz = refx * ny - refy * nx;
						const float invTLen = fast_rsqrt(tx*tx + ty*ty + tz*tz);
						tx *= invTLen; ty *= invTLen; tz *= invTLen;
					}
					// B = handedness · (N × T). The sign comes from the
					// material (faces with mirrored UVs are split onto a
					// handedness=-1 clone) — without it, the fixed-sign cross
					// product inverts the normal map's V detail on mirrored
					// faces, seaming against their non-mirrored neighbours.
					const float hsign = Mat->TbnHandedness;
					const float bx = (ny * tz - nz * ty) * hsign;
					const float by = (nz * tx - nx * tz) * hsign;
					const float bz = (nx * ty - ny * tx) * hsign;
					// new_N = T * nmX + B * nmY + N * nmZ (view space)
					float vnx = tx * nmX + bx * nmY + nx * nmZ;
					float vny = ty * nmX + by * nmY + ny * nmZ;
					float vnz = tz * nmX + bz * nmY + nz * nmZ;
					float invLen = fast_rsqrt(vnx*vnx + vny*vny + vnz*vnz);
					nx = vnx * invLen;
					ny = vny * invLen;
					nz = vnz * invLen;
				}
			}

			// Reconstruct view-space position. zpage16 stores
			// 0xFF80 - round(g_zscale * z), so:
			//   z = (0xFF80 - zEnc) / g_zscale
			const float z = float(0xFF80 - zEnc) * ctx.invZScale;
			const float x = (float(px) - cntrEX) * z * ctx.invFOVX;
			const float y = (cntrEY - float(py)) * z * ctx.invFOVY;

			// Ambient. Same expression as forward Lighting() — except
			// here Mat is per-pixel, not per-mesh-Mat[0].
			float lB, lG, lR;
			if (Mat->Txtr) {
				lB = Mat->Luminosity * 255.0f + Mat->Diffuse * ctx.Sc->Ambient.B;
				lG = Mat->Luminosity * 255.0f + Mat->Diffuse * ctx.Sc->Ambient.G;
				lR = Mat->Luminosity * 255.0f + Mat->Diffuse * ctx.Sc->Ambient.R;
			} else {
				lB = Mat->Luminosity * Mat->BaseCol.B + Mat->Diffuse * ctx.Sc->Ambient.B;
				lG = Mat->Luminosity * Mat->BaseCol.G + Mat->Diffuse * ctx.Sc->Ambient.G;
				lR = Mat->Luminosity * Mat->BaseCol.R + Mat->Diffuse * ctx.Sc->Ambient.R;
			}

			// View direction (pixel -> camera) in view space. Camera is at
			// the view-space origin, so view_dir = -pos / |pos|. Used by
			// the Blinn-Phong specular term below; cheap to skip for matte
			// materials. Glossiness == 0 + Specular > 0 means the FLD
			// authored a specular value but didn't set a Phong exponent;
			// fall back to a sensible mid-sharpness default so we still
			// see a highlight rather than a no-op.
			//
			// Specular_Factor gates the entire highlight pass, mirroring
			// forward's per-scene tone control (CITY/GREETS set it to 0
			// to suppress specular; FOUNTAIN/CRASH leave it nonzero). The
			// deferred path was previously emitting Blinn-Phong even when
			// the scene had explicitly disabled it, which over-brightened
			// City windows + Greets marble against the forward reference.
			// FDS_DEFERRED_NO_SPEC=1: force-disable all deferred specular,
			// regardless of per-material Mat->Specular. Diagnostic for
			// specular firefly / bump-map speckle isolation.
			const bool wantSpecular = !deferredNoSpecG
				&& specGlobalOn && (Mat->Specular > 0.0f) && !profNoSpec;
			const float gloss = Mat->Glossiness > 0
				? float(Mat->Glossiness)
				: 32.0f;
			float vx = 0, vy = 0, vz = 0;
			if (wantSpecular) {
				const float vlen2 = x*x + y*y + z*z;
				const float vlenInv = fast_rsqrt(vlen2);
				vx = -x * vlenInv;
				vy = -y * vlenInv;
				vz = -z * vlenInv;
			}
			// --pbr per-pixel constants (hoisted out of the per-light loop so the
			// scalar GGX cost is measured fairly). roughness fixed or gloss-derived;
			// a=rough², Smith k=a/2, F0=0.04 dielectric; NdotV clamped.
			float pbrA2 = 0, pbrK = 0, pbrNdotV = 0;
			if (wantSpecular && sPbr) {
				float rough = sPbrRoughFixed;
				if (rough <= 0.0f) rough = std::sqrt(2.0f / (gloss + 2.0f));
				if (rough < 0.04f) rough = 0.04f;
				if (rough > 1.0f)  rough = 1.0f;
				const float a = rough * rough;
				pbrA2 = a * a;
				pbrK  = a * 0.5f;
				pbrNdotV = nx*vx + ny*vy + nz*vz;
				if (pbrNdotV < 1e-3f) pbrNdotV = 1e-3f;
			}

			// Specular accumulator — kept separate from diffuse so it can
			// be added AFTER texture modulation (highlights are independent
			// of base-color tint, which is the standard model).
			float sB = 0, sG = 0, sR = 0;

			// Water gets ambient-only (no omni accumulation, no spec).
			// Forward water mesh's BSphereRadius is tiny (Reflective_
			// Surface_Setup tessellates and scales the verts but never
			// recomputes the bsphere) and forward Lighting() skips any
			// omni whose `BSphereRadius + Range < |omni-bsphere_ctr|` —
			// which is basically all of them — so forward water vertex
			// light is just `Mat->Diffuse * Sc->Ambient`. Per-pixel
			// distance tests in the deferred path don't have that cull,
			// so without this skip every nearby city light slams into
			// every water pixel and saturates lB at 250.
			const bool isWater = (int(matID) == ctx.waterMatID);

			// Per-tile filtered light list — only omnis whose screen-
			// space bounding sphere overlaps this tile. For City with
			// ~100 omnis spread across the city, each tile typically
			// sees 10-20.
			const TileLights &tl = ctx.tileLights[tileIndex];

			// Per-pixel normal-map sampling lives only in the scalar
			// path. Force scalar for any pixel whose material has a
			// normal map (or when FDS_NMAP_FROM_DIFFUSE forces it for
			// every textured material). The geometric N from the
			// G-buffer would otherwise drive vec lighting and the
			// normal-map perturbation would be lost.
			const bool hasNormalMap = Mat->NormalMap ||
				(nmapFromDiffuseG && Mat->Txtr);
			// Per-material AO sources, in priority order:
			//   1. Mat_AoInAlpha  — AO baked into the albedo's alpha (texA,
			//      already fetched above → FREE; the recommended PBR packing).
			//   2. Mat->AoMap     — a separate AO texture (escape hatch for
			//      materials that need albedo-alpha for cutout).
			//   3. ao_from_diffuse (dev) — diffuse luminance stand-in.
			// All but #1's vec-compat live in the scalar path (extra texture
			// fetch), so force scalar when a separate map is used.
			const bool aoInAlpha = (Mat->Flags & Mat_AoInAlpha) != 0;
			const bool hasAoMap  = aoMapOnG &&
				(aoInAlpha || Mat->AoMap || (aoFromDiffuseG && Mat->Txtr));
			// Env reflection + metalness live in the shared per-pixel COMPOSE
			// (after the light loop), so they do NOT force the scalar light
			// loop — only nmap/AO do (they change the normal the loop uses).
			// Metals get env even at Reflection == 0 (a metal with no
			// reflection is just black — its "diffuse" IS the reflection).
			const bool hasMetal   = metalMapOnG && Mat->MetallicMap;
			const fds::EnvPanoLinear *envP =
				(envTabG && (Mat->Reflection > 0.0f || hasMetal)) ? envTabG[matID] : nullptr;
			const bool hasEnvRefl = envP != nullptr;
			const bool useVecHere = useVec
				&& (sVecForce || (!hasNormalMap && !(hasAoMap && !aoInAlpha)));

			// Ambient occlusion: darken ONLY the ambient term (lB/lG/lR before
			// the direct-light loop adds to them) — direct light is occluded by
			// shadows, not AO.
			if (hasAoMap) {
				float ao;
				if (aoInAlpha) {
					ao = texA * (1.0f/255.0f);            // free (albedo alpha)
				} else {
					const Texture *aoTex = Mat->AoMap ? Mat->AoMap : Mat->Txtr;
					const dword *aoData = (const dword *)aoTex->Mipmap[miplevel];
					const dword aoTexel = aoData ? aoData[swizzledUV] : 0xFFFFFFFFu;
					ao = (float(aoTexel & 0xFF)         * 0.114f
					    + float((aoTexel >> 8)  & 0xFF)  * 0.587f
					    + float((aoTexel >> 16) & 0xFF)  * 0.299f) * (1.0f/255.0f);
				}
				// Global dial × per-material dial (Mat->AoStrength defaults 1 —
				// the ParallaxScale pattern).
				ao = 1.0f - aoStrengthG * Mat->AoStrength * (1.0f - ao);
				lB *= ao; lG *= ao; lR *= ao;
			}

			// Sample's world-space position. Computed here (outside the
			// vec/scalar split) so both light paths can use it for cube
			// shadow lookup. One 3×3 transform + 3 adds per pixel; cached
			// across all light evaluations for this pixel.
			const float sampleWorldX =
				ctx.viewToWorld[0][0]*x + ctx.viewToWorld[0][1]*y +
				ctx.viewToWorld[0][2]*z + ctx.cameraWorldX;
			const float sampleWorldY =
				ctx.viewToWorld[1][0]*x + ctx.viewToWorld[1][1]*y +
				ctx.viewToWorld[1][2]*z + ctx.cameraWorldY;
			const float sampleWorldZ =
				ctx.viewToWorld[2][0]*x + ctx.viewToWorld[2][1]*y +
				ctx.viewToWorld[2][2]*z + ctx.cameraWorldZ;

			if (!isWater && !profNoLights) {
				if (useVecHere) {
					// 8-wide SIMD inner loop — written directly against
					// simde's _mm256_* intrinsics so we bypass
					// vectorclass's Vec8fb byte-packed mask layout.
					// vectorclass needs 6 NEON instructions per compare
					// to coerce the natural uint32x4_t mask into its
					// portable byte form (xtn → uzp1 → zip2 → ushll →
					// shl → cmlt) plus cross-domain GPR roundtrips for
					// `&`. simde's intrinsics produce masks in the
					// native AVX2 layout (all-1s/all-0s lanes, same as
					// what NEON `vcgeq_f32` returns), so `_mm256_and_ps`
					// is a direct AND with no conversion. simde lowers
					// each intrinsic to native NEON on arm64 / native
					// AVX2 on x86, giving us a single cross-platform
					// hot path.
					__m256 vx_v   = _mm256_set1_ps(x);
					__m256 vy_v   = _mm256_set1_ps(y);
					__m256 vz_v   = _mm256_set1_ps(z);
					__m256 vnx_v  = _mm256_set1_ps(nx);
					__m256 vny_v  = _mm256_set1_ps(ny);
					__m256 vnz_v  = _mm256_set1_ps(nz);
					__m256 vDiff  = _mm256_set1_ps(Mat->Diffuse);
					__m256 vZero  = _mm256_setzero_ps();
					__m256 vOne   = _mm256_set1_ps(1.0f);
					__m256 accB   = _mm256_setzero_ps();
					__m256 accG   = _mm256_setzero_ps();
					__m256 accR   = _mm256_setzero_ps();

					// Per-light spot-cone × cube-shadow attenuation, captured here
					// so the (separate, templated) spec loop can apply the SAME
					// shadow + cone the diffuse loop computes — fixing the vec-spec
					// "leaks through shadows/cones" gap. Only populated when spec is
					// wanted (one vector store per slot).
					alignas(32) float coneShadowAtten[DEFERRED_MAX_VIEW_LIGHTS];

					__m256i pmid_v_main = _mm256_set1_epi32((int)pmid);
					for (int slot = 0; slot < tl.paddedCount; slot += 8) {
						__m256 lpx = _mm256_load_ps(tl.posX   + slot);
						__m256 lpy = _mm256_load_ps(tl.posY   + slot);
						__m256 lpz = _mm256_load_ps(tl.posZ   + slot);
						__m256 lcb = _mm256_load_ps(tl.colB   + slot);
						__m256 lcg = _mm256_load_ps(tl.colG   + slot);
						__m256 lcr = _mm256_load_ps(tl.colR   + slot);
						__m256 lr2 = _mm256_load_ps(tl.range2 + slot);
						__m256 lrr = _mm256_load_ps(tl.rRange + slot);
						// Mirror filter: only lights whose mirrorId matches
						// the pixel's contribute. Padded slots carry
						// 0xffffffff so they never match (pmid is < 256).
						__m256i lmid = _mm256_load_si256(
							(const __m256i*)(tl.mirrorId + slot));
						__m256 mirrorMask = _mm256_castsi256_ps(
							_mm256_cmpeq_epi32(lmid, pmid_v_main));

						__m256 wx = _mm256_sub_ps(lpx, vx_v);
						__m256 wy = _mm256_sub_ps(lpy, vy_v);
						__m256 wz = _mm256_sub_ps(lpz, vz_v);
						__m256 dot = _mm256_fmadd_ps(wx, vnx_v,
						              _mm256_fmadd_ps(wy, vny_v,
						               _mm256_mul_ps(wz, vnz_v)));
						__m256 len2 = _mm256_fmadd_ps(wx, wx,
						               _mm256_fmadd_ps(wy, wy,
						                _mm256_mul_ps(wz, wz)));

						__m256 mask_range = _mm256_cmp_ps(len2, lr2,   _CMP_LE_OQ);
						__m256 mask_dot   = _mm256_cmp_ps(dot,  vZero, _CMP_GE_OQ);
						__m256 mask_pos   = _mm256_cmp_ps(len2, vZero, _CMP_GT_OQ);
						__m256 mask = _mm256_and_ps(mask_range,
						               _mm256_and_ps(mask_dot,
						                _mm256_and_ps(mask_pos, mirrorMask)));

						__m256 safe_len2 = _mm256_blendv_ps(vOne, len2, mask);
						__m256 lenInv = _mm256_rsqrt_ps(safe_len2);
						__m256 dist   = _mm256_mul_ps(safe_len2, lenInv);
						__m256 falloff = _mm256_sub_ps(vOne, _mm256_mul_ps(dist, lrr));
						__m256 k = _mm256_mul_ps(_mm256_mul_ps(dot, lenInv), falloff);

						// Spot cone attenuation. For lanes where isSpot==0
						// coneAtten=1 (omni). For spot lanes: smoothstep
						// from cosOuter→cosInner of cosTheta=-dot(Dir,w)/|w|.
						__m256 ldx = _mm256_load_ps(tl.dirX     + slot);
						__m256 ldy = _mm256_load_ps(tl.dirY     + slot);
						__m256 ldz = _mm256_load_ps(tl.dirZ     + slot);
						__m256 lci = _mm256_load_ps(tl.cosInner + slot);
						__m256 lco = _mm256_load_ps(tl.cosOuter + slot);
						__m256i lis = _mm256_load_si256((const __m256i*)(tl.isSpot + slot));
						__m256 dirDotW = _mm256_fmadd_ps(ldx, wx,
						                  _mm256_fmadd_ps(ldy, wy,
						                   _mm256_mul_ps(ldz, wz)));
						__m256 cosTheta = _mm256_mul_ps(
						                    _mm256_sub_ps(vZero, dirDotW),
						                    lenInv);
						__m256 maskInside = _mm256_cmp_ps(cosTheta, lco, _CMP_GT_OQ);
						__m256 rangeRcp = _mm256_div_ps(vOne, _mm256_sub_ps(lci, lco));
						__m256 t = _mm256_mul_ps(_mm256_sub_ps(cosTheta, lco), rangeRcp);
						t = _mm256_min_ps(_mm256_max_ps(t, vZero), vOne);
						// smoothstep(t) = t*t*(3-2t)
						__m256 smooth = _mm256_mul_ps(_mm256_mul_ps(t, t),
						                  _mm256_sub_ps(_mm256_set1_ps(3.0f),
						                                _mm256_mul_ps(_mm256_set1_ps(2.0f), t)));
						// coneAtten = isSpot ? (maskInside ? smooth : 0) : 1
						__m256 spotAtten = _mm256_and_ps(maskInside, smooth);
						__m256 isSpotMask = _mm256_castsi256_ps(
						                     _mm256_cmpgt_epi32(lis, _mm256_setzero_si256()));
						__m256 coneAtten = _mm256_blendv_ps(vOne, spotAtten, isSpotMask);
						k = _mm256_mul_ps(k, coneAtten);
						// Stash cone atten for the spec loop; cube shadow is folded
						// in per-lane below. (Spec computes its own falloff + N·H, so
						// only cone × shadow needs sharing.)
						if (wantSpecular) _mm256_store_ps(coneShadowAtten + slot, coneAtten);

						// Per-lane shadow attenuation (scalarized). Covers the
						// light's OWN cube (resolveCubeAtten) AND, mirroring the
						// scalar body, mirror-clone source maps / cubes / the own
						// 2-D spot map via computeMapShadowAtten. The loop fires
						// for a lane if ANY of those four index planes is set;
						// most lanes have none, so we quick-check the SoA before
						// paying the lookup cost. Per-pixel sampleWorld is already
						// computed above for both light paths.
						__m256i cubeIdxV = _mm256_load_si256(
							(const __m256i*)(tl.cubeShadowIdx + slot));
						__m256i smIdxV = _mm256_load_si256(
							(const __m256i*)(tl.shadowMapIdx + slot));
						__m256i srcSmV = _mm256_load_si256(
							(const __m256i*)(tl.srcShadowMapIdx + slot));
						__m256i srcCubeV = _mm256_load_si256(
							(const __m256i*)(tl.srcCubeShadowIdx + slot));
						__m256i negOne = _mm256_set1_epi32(-1);
						__m256i anyShadow = _mm256_or_si256(
							_mm256_or_si256(_mm256_cmpgt_epi32(cubeIdxV, negOne),
							                _mm256_cmpgt_epi32(smIdxV,   negOne)),
							_mm256_or_si256(_mm256_cmpgt_epi32(srcSmV,   negOne),
							                _mm256_cmpgt_epi32(srcCubeV, negOne)));
						if (_mm256_movemask_epi8(anyShadow) != 0) {
							alignas(32) int32_t cubeArr[8];
							_mm256_store_si256((__m256i*)cubeArr, cubeIdxV);
							alignas(32) float kArr[8];
							_mm256_store_ps(kArr, k);
							alignas(32) float wxArr[8], wyArr[8], wzArr[8], liArr[8];
							_mm256_store_ps(wxArr, wx);
							_mm256_store_ps(wyArr, wy);
							_mm256_store_ps(wzArr, wz);
							_mm256_store_ps(liArr, lenInv);
							const int kSB = kShadowBiasG;
							const int kSL = kSlopeBiasG;
							for (int lane = 0; lane < 8; ++lane) {
								if (kArr[lane] <= 0.0f) continue;
								const int gi = slot + lane;
								// Own cube (default 1.0 when this lane carries none).
								float atten = 1.0f;
								if (cubeArr[lane] >= 0) {
									atten = resolveCubeAtten(
										pixelLM, cubeArr[lane], lmKernelEnabled, caFlags,
										wxArr[lane], wyArr[lane], wzArr[lane], liArr[lane],
										nGeoX, nGeoY, nGeoZ,
										sampleWorldX, sampleWorldY, sampleWorldZ,
										x, y, z, kSB, kSL,
										surfaceShadowId);
								}
								// Mirror-clone source map/cube + own 2-D spot map
								// (returns 1.0 when none of those apply for this lane).
								atten *= computeMapShadowAtten(
									tl, gi, ctx, x, y, z,
									wxArr[lane], wyArr[lane], wzArr[lane], liArr[lane],
									nGeoX, nGeoY, nGeoZ, surfaceShadowId,
									kSB, kSL, profShadowCache);
								kArr[lane] *= atten;
								if (wantSpecular) coneShadowAtten[gi] *= atten;
							}
							k = _mm256_load_ps(kArr);
						}

						__m256 intensity = _mm256_blendv_ps(vZero,
						                    _mm256_mul_ps(k, vDiff), mask);

						accB = _mm256_fmadd_ps(intensity, lcb, accB);
						accG = _mm256_fmadd_ps(intensity, lcg, accG);
						accR = _mm256_fmadd_ps(intensity, lcr, accR);
					}
					// Horizontal reduction: AVX2 has no scalar reduce;
					// store + scalar add is what vectorclass does
					// internally too. 24 scalar adds total per pixel
					// — negligible vs the omni loop body.
					alignas(32) float bufB[8], bufG[8], bufR[8];
					_mm256_store_ps(bufB, accB);
					_mm256_store_ps(bufG, accG);
					_mm256_store_ps(bufR, accR);
					for (int i = 0; i < 8; ++i) {
						lB += bufB[i];
						lG += bufG[i];
						lR += bufR[i];
					}
					// Vec-spec path: replaces the previous scalar pow loop.
					// Dispatch on Mat->Glossiness so the inner loop uses a
					// constant squaring sequence (bit-exact pow for integer
					// gloss, ~5-7 fmuls vs ~50-80 cycles for libm pow).
					// FDS_DEFERRED_GLOSS_STATS confirms our FLDs only ship
					// gloss ∈ {48, 64}; the other cases here are defensive
					// for spectest's authored values and future FLDs.
					if (wantSpecular && sPbr) {
						// --pbr TEST: Cook-Torrance instead of Blinn-Phong, same
						// 8-lights-wide vec path. Roughness fixed (--pbr_roughness)
						// or derived from Glossiness (roughness=sqrt(2/(gloss+2))).
						float rough = sPbrRoughFixed;
						if (rough <= 0.0f)
							rough = std::sqrt(2.0f / (float(Mat->Glossiness > 0 ? Mat->Glossiness : 32) + 2.0f));
						if (rough < 0.04f) rough = 0.04f;
						if (rough > 1.0f)  rough = 1.0f;
						run_vec_ggx_loop(tl, x,y,z, nx,ny,nz, vx,vy,vz, Mat->Specular, rough, pmid, coneShadowAtten, sB,sG,sR);
					} else if (wantSpecular) {
						switch (Mat->Glossiness) {
							case 4:
								run_vec_spec_loop<4>  (tl, x,y,z, nx,ny,nz, vx,vy,vz, Mat->Specular, pmid, coneShadowAtten, sB,sG,sR); break;
							case 8:
								run_vec_spec_loop<8>  (tl, x,y,z, nx,ny,nz, vx,vy,vz, Mat->Specular, pmid, coneShadowAtten, sB,sG,sR); break;
							case 16:
								run_vec_spec_loop<16> (tl, x,y,z, nx,ny,nz, vx,vy,vz, Mat->Specular, pmid, coneShadowAtten, sB,sG,sR); break;
							case 32:
								run_vec_spec_loop<32> (tl, x,y,z, nx,ny,nz, vx,vy,vz, Mat->Specular, pmid, coneShadowAtten, sB,sG,sR); break;
							case 48:
								run_vec_spec_loop<48> (tl, x,y,z, nx,ny,nz, vx,vy,vz, Mat->Specular, pmid, coneShadowAtten, sB,sG,sR); break;
							case 64:
								run_vec_spec_loop<64> (tl, x,y,z, nx,ny,nz, vx,vy,vz, Mat->Specular, pmid, coneShadowAtten, sB,sG,sR); break;
							case 128:
								run_vec_spec_loop<128>(tl, x,y,z, nx,ny,nz, vx,vy,vz, Mat->Specular, pmid, coneShadowAtten, sB,sG,sR); break;
							default:
								// Unknown gloss — defensive scalar libm path
								// (matches old behavior). If this fires on a
								// hot scene, add a `case` for the value.
								for (int n = 0; n < tl.count; ++n) {
									if (tl.mirrorId[n] != pmid) continue;
									const float Lpx = tl.posX[n];
									const float Lpy = tl.posY[n];
									const float Lpz = tl.posZ[n];
									const float wx = Lpx - x;
									const float wy = Lpy - y;
									const float wz = Lpz - z;
									const float dot = wx*nx + wy*ny + wz*nz;
									if (dot < 0.0f) continue;
									const float len2 = wx*wx + wy*wy + wz*wz;
									const float r2 = tl.range2[n];
									if (len2 > r2 || len2 == 0.0f) continue;
									const float lenInv = fast_rsqrt(len2);
									const float dist   = len2 * lenInv;
									const float ldx = wx * lenInv;
									const float ldy = wy * lenInv;
									const float ldz = wz * lenInv;
									const float hx = ldx + vx, hy = ldy + vy, hz = ldz + vz;
									const float hLen2 = hx*hx + hy*hy + hz*hz;
									if (hLen2 <= 0.0f) continue;
									// Fold renorm into the dot (same trick as
									// the nmap path + vec spec loop).
									const float NdotH_raw = nx*hx + ny*hy + nz*hz;
									if (NdotH_raw <= 0.0f) continue;
									const float NdotH = NdotH_raw * fast_rsqrt(hLen2);
									// pow(NdotH, gloss) via LUT-based log2/exp2.
									// std::pow on arm64 libm = ~50-100 cycles
									// per call; fastPow2(gloss*fastLog2(NdotH))
									// is ~10. Used only when Mat->Glossiness
									// falls outside the templated values {4,
									// 8, 16, 32, 48, 64, 128}.
									const float spec = fastPow2(gloss * fastLog2(NdotH));
									const float rRange = tl.rRange[n];
									const float specStrength = spec * Mat->Specular *
										(1.0f - dist * rRange);
									sB += specStrength * tl.colB[n];
									sG += specStrength * tl.colG[n];
									sR += specStrength * tl.colR[n];
								}
								break;
						}
					}
				} else {
					// Scalar path (default). Per-omni early-out on
					// dot/range tests; branch predictor learns the
					// "most filtered omnis still out of range" pattern.
					// `dist = len2 * lenInv` (= sqrt(len2)) avoids a
					// second fdiv vs `1/lenInv` — fdiv on arm64 is
					// ~20 cycles vs fmul's 4. The compiler doesn't do
					// this rewrite under strict FP, so spell it out.
					// sampleWorldX/Y/Z hoisted to the parent scope —
					// used by cube shadow sampling for any omni with
					// cubeShadowIdx >= 0.
					for (int n = 0; n < tl.count; ++n) {
						if (tl.mirrorId[n] != pmid) continue;
						const float Lpx = tl.posX[n];
						const float Lpy = tl.posY[n];
						const float Lpz = tl.posZ[n];
						const float wx = Lpx - x;
						const float wy = Lpy - y;
						const float wz = Lpz - z;
						const float dot = wx * nx + wy * ny + wz * nz;
						if (dot < 0.0f) continue;
						const float len2 = wx*wx + wy*wy + wz*wz;
						const float r2 = tl.range2[n];
						if (len2 > r2) continue;
						// Mirror-bounce window portal: a bounce spot's apex
						// is behind the glass; only light it onto this pixel
						// if the apex→pixel segment passes through the mirror
						// window. Gate keeps non-bounce lights free.
						if (tl.winMinX[n] <= tl.winMaxX[n] &&
						    bouncePortalReject(tl, n, sampleWorldX, sampleWorldY, sampleWorldZ))
							continue;
						const float lenInv = fast_rsqrt(len2);
						const float dist   = len2 * lenInv;
						const float rRange = tl.rRange[n];
						float k = dot * lenInv * (1.0f - dist * rRange);
						// Spot cone: matches the vec body's coneAtten so nmap
						// pixels (which all flow through this scalar path)
						// don't render the robot spotlight as an omni.
						if (tl.isSpot[n]) {
							const float cosTheta = -(tl.dirX[n]*wx + tl.dirY[n]*wy + tl.dirZ[n]*wz) * lenInv;
							if (cosTheta <= tl.cosOuter[n]) continue;
							if (cosTheta < tl.cosInner[n]) {
								const float ct = (cosTheta - tl.cosOuter[n]) / (tl.cosInner[n] - tl.cosOuter[n]);
								k *= ct * ct * (3.0f - 2.0f * ct);
							}
						}
						// Shadow test. Project pixel view-space pos to the
						// shadow map's screen+depth; attenuate the light
						// by the occlusion fraction. Cheap branch when
						// smIdx<0 (no shadow).
						//
						// PCF with bilinear-weighted 2×2: sample 4 neighbouring
						// shadow-map texels and blend the per-texel
						// occlusion by fractional (smX, smY) — continuous
						// in light-space position, no integer-snap when
						// the light camera moves a sub-texel between
						// frames. Without bilinear weighting whole
						// polygons that project to a single texel flicker
						// in/out as the texel-grid shifts under them.
						// Cost: 4 loads + 4 compares + 4 muls + 4 adds.
						float shadowAtten = computeMapShadowAtten(
							tl, n, ctx, x, y, z, wx, wy, wz, lenInv,
							nGeoX, nGeoY, nGeoZ, surfaceShadowId,
							kShadowBiasG, kSlopeBiasG, profShadowCache);
						// Combined srcSm×srcCube×smIdx; 0 = fully shadowed (the
						// old `if (occ >= 1.0f) continue;` early-out).
						if (shadowAtten <= 0.0f) continue;

						// Cube shadow (omni shadow caster). Two paths:
						//  - Static-mesh pixel + lightmap baked for this
						//    cubeIdx → bilinear sample from the lightmap.
						//  - Otherwise → per-pixel cube tap.
						const int32_t cubeIdx = tl.cubeShadowIdx[n];
						if (cubeIdx >= 0) {
							const float cubeAtten = resolveCubeAtten(
								pixelLM, cubeIdx, lmKernelEnabled, caFlags,
								wx, wy, wz, lenInv,
								nGeoX, nGeoY, nGeoZ,
								sampleWorldX, sampleWorldY, sampleWorldZ,
								x, y, z, kShadowBiasG, kSlopeBiasG,
								surfaceShadowId);
							if (cubeAtten <= 0.0f) continue;
							shadowAtten *= cubeAtten;
						}
						const float intensity = k * Mat->Diffuse * shadowAtten;
						const float Lcb = tl.colB[n];
						const float Lcg = tl.colG[n];
						const float Lcr = tl.colR[n];
						lB += intensity * Lcb;
						lG += intensity * Lcg;
						lR += intensity * Lcr;
						if (s_contribProf) {   // max linear diffuse contrib of light n over the tile
							const float kN = 1.0f/255.0f;
							const float aB=texB*kN, aG=texG*kN, aR=texR*kN;
							float cc = aB*aB*intensity*Lcb;
							const float cg = aG*aG*intensity*Lcg; if (cg>cc) cc=cg;
							const float cr = aR*aR*intensity*Lcr; if (cr>cc) cc=cr;
							if (cc > contribMax[n]) contribMax[n] = cc;
						}

						if (wantSpecular) {
							const float ldx = wx * lenInv;
							const float ldy = wy * lenInv;
							const float ldz = wz * lenInv;
							const float hx = ldx + vx;
							const float hy = ldy + vy;
							const float hz = ldz + vz;
							const float hLen2 = hx*hx + hy*hy + hz*hz;
							// dot(N, H_unit) = dot(N, H_raw) * rsqrt(|H_raw|²).
							// Saves 3 muls per lit pixel vs renormalizing H
							// first; positive rsqrt preserves NdotH's sign so
							// the > 0 cull still works.
							const float NdotH_raw = nx*hx + ny*hy + nz*hz;
							if (hLen2 > 0.0f && NdotH_raw > 0.0f) {
								const float hInv  = fast_rsqrt(hLen2);
								const float NdotH = NdotH_raw * hInv;
								float spec;
								if (sPbr) {
									// Scalar Cook-Torrance (GGX D + Smith-Schlick G +
									// Schlick F), mirroring run_vec_ggx_loop. NdotL =
									// dot·lenInv; VdotH = (V·H)·hInv. spec = D·G·F/(4·NdotV).
									const float NdotL = dot * lenInv;
									const float VdotH = (vx*hx + vy*hy + vz*hz) * hInv;
									const float d1 = NdotH*NdotH*(pbrA2 - 1.0f) + 1.0f;
									const float D = pbrA2 * 0.31830989f / (d1*d1 + 1e-6f);
									const float omk = 1.0f - pbrK;
									const float Gv = pbrNdotV / (pbrNdotV*omk + pbrK);
									const float Gl = NdotL / (NdotL*omk + pbrK);
									const float om = 1.0f - (VdotH > 0.0f ? VdotH : 0.0f);
									const float om2 = om*om;
									const float F = 0.04f + 0.96f * (om2*om2*om);
									spec = D * Gv * Gl * F / (4.0f * pbrNdotV);
								} else {
									spec = pow_glossClass(NdotH, Mat->Glossiness);
								}
								{
									// Multiply by shadowAtten so shadowed pixels don't
									// leak specular highlights — was a visible bug at
									// bumped-mortar pixels inside shadow regions, where
									// the bumped N satisfies the sharp Gloss=48 lobe
									// while diffuse was correctly killed.
									const float specStrength = spec * Mat->Specular *
										(1.0f - dist * rRange) * shadowAtten;
									sB += specStrength * Lcb;
									sG += specStrength * Lcg;
									sR += specStrength * Lcr;
								}
							}
						}
					}
				}
			}

			// Saturation cap (matches forward at 250). The 250/255 caps are
			// 8-bit rollover guards; in HDR the radiance lands in a float buffer
			// that can't roll over, so we lift the UPPER cap (else bright lit
			// surfaces max at ~250 and never bloom). Lower clamp stays — negative
			// light is nonsense in either path.
			if (!hdrWrite) {
				if (lB > 250.0f) lB = 250.0f;
				if (lG > 250.0f) lG = 250.0f;
				if (lR > 250.0f) lR = 250.0f;
			}
			if (lB < 0.0f)   lB = 0.0f;
			if (lG < 0.0f)   lG = 0.0f;
			if (lR < 0.0f)   lR = 0.0f;

			// Fog moved to Render_DeferredFogPass — a single post-lighting
			// full-screen pass keeps the lighting kernel SIMD-clean and
			// drops the per-pixel sqrt. Forward TheOtherBarry still
			// applies its own per-vertex fog (TheOtherBarry.h:716-734)
			// for reflective windows / additive fountain vortex; those
			// pixels are skipped by the fog pass via the mat32 sentinel.

			// Diffuse modulation: pixel = texel * light / 256.
			// Reflective faces are dispatched to TheOtherBarry<OVERWRITE,
			// TEXTURETEXTURE> in RenderInnerMekalele and never reach this
			// code path — they're rendered by the forward filler (which
			// does the env+tex/2 composite using forward's per-vertex
			// interpolated eu/ev) and skipped here via the mat32 sentinel.
			float fdB = (texB * lB) * (1.0f / 256.0f);
			float fdG = (texG * lG) * (1.0f / 256.0f);
			float fdR = (texR * lR) * (1.0f / 256.0f);
			// Metalness (--metal_map): m=1 pixels are conductors — no diffuse
			// (the albedo becomes the REFLECTION tint below), and analytic
			// highlights tint by the albedo instead of staying light-colored.
			float metalM = 0.0f;
			if (metalMapOnG && Mat->MetallicMap) {
				const byte *md = (miplevel < Mat->MetallicMap->numMipmaps)
					? reinterpret_cast<const byte*>(Mat->MetallicMap->Mipmap[miplevel]) : nullptr;
				if (md) metalM = float(md[swizzledUV]) * (1.0f/255.0f);
				if (metalM > 0.0f) {
					const float dk = 1.0f - metalM;
					fdB *= dk; fdG *= dk; fdR *= dk;
				}
			}
			// Roughness map (cheap tier): per-pixel specular INTENSITY. White =
			// rough → dimmer highlight, so the highlight breaks up across the
			// surface (matte mortar vs glinty stone). 8-bit gather at the same
			// (parallax-shifted) swizzled UV; only when there's a highlight.
			// The env reflection is added AFTER this block: rough surfaces get
			// a BLURRED reflection (pre-filtered pano mip), not a dimmed one.
			if (roughMapOnG && Mat->RoughnessMap && (sB != 0.0f || sG != 0.0f || sR != 0.0f)) {
				const byte *rd = (miplevel < Mat->RoughnessMap->numMipmaps)
					? reinterpret_cast<const byte*>(Mat->RoughnessMap->Mipmap[miplevel]) : nullptr;
				if (rd) {
					float specMul = 1.0f - roughStrengthG * (float(rd[swizzledUV]) * (1.0f/255.0f));
					if (specMul < 0.0f) specMul = 0.0f;
					sB *= specMul; sG *= specMul; sR *= specMul;
				}
			}
			// Metals: tint the accumulated analytic highlights by the albedo.
			if (metalM > 0.0f) {
				const float inv255 = 1.0f / 255.0f;
				sB *= 1.0f - metalM + metalM * texB * inv255;
				sG *= 1.0f - metalM + metalM * texG * inv255;
				sR *= 1.0f - metalM + metalM * texR * inv255;
			}
			// Env-specular reflection (--env_refl): sample this surface's
			// panorama along the reflected view ray. Fresnel-weighted
			// (Schlick): F0 = the authored Reflection% (0.04 dielectric
			// floor), pulled toward 1 by metalness. PARALLAX-CORRECTED: the
			// pano is only exact at its bake point, so the reflected ray is
			// intersected with the scene-AABB proxy from the pixel's WORLD
			// position and the direction to that hit (from the bake point)
			// indexes the pano — floors/walls track position instead of
			// wearing a pasted-on picture.
			if (hasEnvRefl) {
				// Incident ray d = pixel direction (view space, camera at
				// origin); reflect about the (possibly nmap-perturbed) N.
				const float dInv = fast_rsqrt(x*x + y*y + z*z);
				const float dx = x * dInv, dy = y * dInv, dz = z * dInv;
				const float dDotN = dx*nx + dy*ny + dz*nz;
				const float rvx = dx - 2.0f * dDotN * nx;
				const float rvy = dy - 2.0f * dDotN * ny;
				const float rvz = dz - 2.0f * dDotN * nz;
				// View → world rotation (viewToWorld is the transpose of the
				// camera rotation; direction ⇒ no translation).
				float rwx = ctx.viewToWorld[0][0]*rvx + ctx.viewToWorld[0][1]*rvy + ctx.viewToWorld[0][2]*rvz;
				float rwy = ctx.viewToWorld[1][0]*rvx + ctx.viewToWorld[1][1]*rvy + ctx.viewToWorld[1][2]*rvz;
				float rwz = ctx.viewToWorld[2][0]*rvx + ctx.viewToWorld[2][1]*rvy + ctx.viewToWorld[2][2]*rvz;
				// Parallax correction: exit-t of ray sampleWorld + t·R against
				// the AABB (slab method, per-axis far plane), hit point → the
				// lookup direction from the BAKE point. t ≤ 0 (pixel outside
				// the proxy) falls back to the uncorrected direction.
				{
					const float bigT = 1e30f;
					const float tx_ = rwx > 1e-6f ? (envP->boxMaxX - sampleWorldX) / rwx
					                : rwx < -1e-6f ? (envP->boxMinX - sampleWorldX) / rwx : bigT;
					const float ty_ = rwy > 1e-6f ? (envP->boxMaxY - sampleWorldY) / rwy
					                : rwy < -1e-6f ? (envP->boxMinY - sampleWorldY) / rwy : bigT;
					const float tz_ = rwz > 1e-6f ? (envP->boxMaxZ - sampleWorldZ) / rwz
					                : rwz < -1e-6f ? (envP->boxMinZ - sampleWorldZ) / rwz : bigT;
					float t = tx_ < ty_ ? tx_ : ty_;
					if (tz_ < t) t = tz_;
					if (t > 0.0f && t < bigT) {
						const float hx_ = sampleWorldX + t * rwx - envP->bakeX;
						const float hy_ = sampleWorldY + t * rwy - envP->bakeY;
						const float hz_ = sampleWorldZ + t * rwz - envP->bakeZ;
						const float hInv_ = fast_rsqrt(hx_*hx_ + hy_*hy_ + hz_*hz_ + 1e-12f);
						rwx = hx_ * hInv_; rwy = hy_ * hInv_; rwz = hz_ * hInv_;
					}
				}
				// Equirect lookup — the exact inverse of EnvBake's stitch
				// mapping: lon = atan2(-z, -x), lat = asin(y). Polynomial
				// approximations (SimdHelpers) — libm atan2f/asinf here were
				// the bulk of the env cost (~5ms on a cockpit-sized surface).
				const float lon = atan2_approx(-rwz, -rwx);
				float sy_ = rwy; if (sy_ > 1.0f) sy_ = 1.0f; if (sy_ < -1.0f) sy_ = -1.0f;
				const float lat = asin_approx(sy_);
				float eu = (lon + 1.57079632679f) * (1.0f / 6.28318530718f) + 0.5f;
				eu -= std::floor(eu);
				float evv = 0.5f - lat * (1.0f / 3.14159265359f);
				if (evv < 0.0f) evv = 0.0f; if (evv > 0.9999f) evv = 0.9999f;
				// Pre-filtered mip by per-pixel roughness (map texel, else the
				// gloss-derived roughness the --pbr path uses).
				float rough;
				if (roughMapOnG && Mat->RoughnessMap && miplevel < Mat->RoughnessMap->numMipmaps
				    && Mat->RoughnessMap->Mipmap[miplevel]) {
					rough = float(reinterpret_cast<const byte*>(
						Mat->RoughnessMap->Mipmap[miplevel])[swizzledUV]) * (1.0f/255.0f);
				} else {
					rough = std::sqrt(2.0f / (gloss + 2.0f));
				}
				// Trilinear across the blur chain: nearest-level select on a
				// NOISY roughness map made adjacent pixels flip between sharp
				// and blurred mips (speckle). Lerp the two straddling levels.
				float lvlF = rough * float(envP->numMips - 1);
				if (lvlF < 0.0f) lvlF = 0.0f;
				if (lvlF > float(envP->numMips - 1)) lvlF = float(envP->numMips - 1);
				const int lvl0 = int(lvlF);
				const int lvl1 = lvl0 + 1 < envP->numMips ? lvl0 + 1 : lvl0;
				const float lf = lvlF - float(lvl0);
				// ENV_NOFETCH=1: constant color instead of the pano loads —
				// cost-attribution experiment (fetch-bound vs math-bound).
				static const bool sNoFetch = std::getenv("ENV_NOFETCH") != nullptr;
				auto fetchLvl = [&](int lvl) -> uint32_t {
					if (sNoFetch) return 0xFF808080u;
					const int lw = envP->W >> lvl, lh = envP->H >> lvl;
					const int epx = int(eu * float(lw)) % lw;
					const int epy_ = int(evv * float(lh));
					return envP->mip[lvl][size_t(epy_) * lw + epx];
				};
				const uint32_t c0 = fetchLvl(lvl0);
				const uint32_t c1 = lvl1 != lvl0 ? fetchLvl(lvl1) : c0;
				const float ecB = float(c0 & 0xFF)         + lf * (float(c1 & 0xFF)         - float(c0 & 0xFF));
				const float ecG = float((c0 >> 8) & 0xFF)  + lf * (float((c1 >> 8) & 0xFF)  - float((c0 >> 8) & 0xFF));
				const float ecR = float((c0 >> 16) & 0xFF) + lf * (float((c1 >> 16) & 0xFF) - float((c0 >> 16) & 0xFF));
				// Schlick Fresnel. NdotV = -d·N (front-facing pixels have
				// d·N < 0). F0 = authored Reflection% with the dielectric
				// floor, pulled to ~1 by metalness. F90 (the grazing limit)
				// is attenuated by roughness — the standard rough-Fresnel
				// trick; without it a noisy normal map turns every grazing
				// texel into a white spark (pow5 amplifies the nmap noise).
				float ndv = -dDotN;
				if (ndv < 0.0f) ndv = 0.0f; if (ndv > 1.0f) ndv = 1.0f;
				float f0 = Mat->Reflection * 0.01f;
				if (f0 < 0.04f) f0 = 0.04f;
				f0 = f0 + (0.98f - f0) * metalM;
				float f90 = 1.0f - rough;
				if (f90 < f0) f90 = f0;
				const float omv = 1.0f - ndv;
				const float omv2 = omv * omv;
				const float fres = f0 + (f90 - f0) * omv2 * omv2 * omv;
				const float ek = fres * envReflGainG;
				const float inv255 = 1.0f / 255.0f;
				// Metal tint: reflection takes the albedo's color.
				const float tB = 1.0f - metalM + metalM * texB * inv255;
				const float tG = 1.0f - metalM + metalM * texG * inv255;
				const float tR = 1.0f - metalM + metalM * texR * inv255;
				sB += ecB * ek * tB;
				sG += ecG * ek * tG;
				sR += ecR * ek * tR;
			}
			int outB = int(fdB) + int(sB);
			int outG = int(fdG) + int(sG);
			int outR = int(fdR) + int(sR);
			// HDR B1: unclamped float radiance (same gamma-space value, no 8-bit
			// truncation/clamp), accumulated through the water blend below and
			// written to g_hdrBuf before the debug-viz stomp.
			float hB = fdB + sB, hG = fdG + sG, hR = fdR + sR;

			// Water-mesh transparent blend. Forward draws the water plane
			// with TheOtherBarry<TRANSPARENT> after a pass-1 mirrored-world
			// draw + dispMap distortion has populated vpage with a wavy
			// reflection of the city. The transparent filler does
			//   pixel = saturate(lit_water_texel + existing_VPage/2)
			// (TheOtherBarry.h:392). We reproduce the same blend here:
			// the existing vpage value at this pixel is the reflection
			// preserved across the inter-pass Z-clear (which lets pass-2
			// deferred shading skip non-water-mesh pixels via zEnc check
			// on the freshly-cleared depth buffer).
			if (isWater) {
				const dword existing = out[i];
				const int rB = int(existing & 0xFF);
				const int rG = int((existing >> 8) & 0xFF);
				const int rR = int((existing >> 16) & 0xFF);
				outB += rB >> 1;
				outG += rG >> 1;
				outR += rR >> 1;
				hB += float(rB) * 0.5f;
				hG += float(rG) * 0.5f;
				hR += float(rR) * 0.5f;
			}

			// HDR B1/B2: stash the unclamped opaque radiance + coverage flag
			// before the debug-viz stomp, so viz only affects the displayed (LDR)
			// vpage. The froxel composite reads h[3] to take the scene from here
			// (opaque) vs the vpage (sky/forward content the kernel never wrote).
			if (hdrWrite) {
				fds::hdrf* h = fds::g_hdrBuf.data() + i * 4;
				if (hdrLinear) {
					// B2 + full coherence: linear lighting. albedo² (gamma-2.0
					// decode) × light at power 1; specular is reflected light → a
					// linear add. Store LINEAR radiance directly (the tonemap no
					// longer decodes; T·scene + in-scatter compose in linear).
					const float kN = 1.0f / 255.0f;
					const float aB = texB*kN, aG = texG*kN, aR = texR*kN;
					float rlB = aB*aB*lB + sB, rlG = aG*aG*lG + sG, rlR = aR*aR*lR + sR;
					if (isWater) {            // reflection underlay is gamma → linearize
						const dword e = out[i];
						const float wB=float(e&0xFF)*kN, wG=float((e>>8)&0xFF)*kN, wR=float((e>>16)&0xFF)*kN;
						rlB += wB*wB*255.0f*0.5f; rlG += wG*wG*255.0f*0.5f; rlR += wR*wR*255.0f*0.5f;
					}
					h[0] = fds::HdrClamp(rlB); h[1] = fds::HdrClamp(rlG); h[2] = fds::HdrClamp(rlR);
				} else {
					h[0] = fds::HdrClamp(hB); h[1] = fds::HdrClamp(hG); h[2] = fds::HdrClamp(hR);   // B1 gamma radiance
				}
				h[3] = 1.0f;
			}

			// FDS_VIZ_NORMAL / FDS_VIZ_TANGENT: stomp final output with
			// a (vec+1)*127.5 visualization. nx/ny/nz here is post-TBN
			// (perturbed by the normal map); per-pixel tangent is decoded
			// fresh from the G-buffer (the in-kernel `tx`/`ty`/`tz` is
			// scoped inside the nmap branch). Use to verify the per-vertex
			// / per-pixel tangent path produces a coherent field.
			if (sVizNormal) {
				outR = int((nx + 1.0f) * 127.5f);
				outG = int((ny + 1.0f) * 127.5f);
				outB = int((nz + 1.0f) * 127.5f);
			}
			if (sVizGeoNormal) {
				// Pre-nmap geometry normal (nGeoX/Y/Z saved before the
				// normal-map block). A flat wall → one solid colour; any
				// diagonal/variation = the vertex-normal interpolation crease.
				outR = int((nGeoX + 1.0f) * 127.5f);
				outG = int((nGeoY + 1.0f) * 127.5f);
				outB = int((nGeoZ + 1.0f) * 127.5f);
			}
			if (sVizMatID) {
				// Hash matID to a distinct colour. matID is already in
				// scope from the mat32 unpack above; for matID==0 we
				// keep pure black so untouched/sentinel pixels read
				// distinctly. Multiplication by 3 large primes mixes
				// adjacent ids into clearly different colours.
				const uint32_t h = uint32_t(matID);
				outR = int((h * 73u + 41u) & 0xFFu);
				outG = int((h * 151u + 13u) & 0xFFu);
				outB = int((h * 211u + 97u) & 0xFFu);
			}
			if (sVizPmid) {
				// Hash pmid (gb.mirrorId post-commit, = which mirror
				// context owns this pixel). pmid==0 → black (originals);
				// nonzero → per-mirror colour. Lets us distinguish the
				// reflected floor (pmid > 0) from the original floor
				// (pmid == 0) — matID can't, since clones share Mat.
				const uint32_t h = uint32_t(pmid);
				if (h == 0u) {
					outR = outG = outB = 0;
				} else {
					outR = int((h * 73u + 41u) & 0xFFu);
					outG = int((h * 151u + 13u) & 0xFFu);
					outB = int((h * 211u + 97u) & 0xFFu);
				}
			}
			if (sVizTangent) {
				if (!gb.tangent.empty()) {
					const meka::u16 packedT = gb.tangent[i];
					if (packedT != 0) {
						float vtx, vty, vtz;
						meka::oct_decode_u16(packedT, vtx, vty, vtz);
						outR = int((vtx + 1.0f) * 127.5f);
						outG = int((vty + 1.0f) * 127.5f);
						outB = int((vtz + 1.0f) * 127.5f);
					} else {
						outR = outG = outB = 0;   // degenerate tangent → black
					}
				}
			}

			if (outB > 255) outB = 255;
			if (outG > 255) outG = 255;
			if (outR > 255) outR = 255;
			if (outB < 0)   outB = 0;
			if (outG < 0)   outG = 0;
			if (outR < 0)   outR = 0;

			out[i] = dword(outB) | (dword(outG) << 8) | (dword(outR) << 16) | 0xFF000000u;
		}
	}

	if (s_contribProf) {   // [DIAG] count this tile's lights with sub-visible max contribution
		int d05=0, d1=0, d2=0;
		for (int ci = 0; ci < contribN; ++ci) {
			if (contribMax[ci] < 0.5f) ++d05;
			if (contribMax[ci] < 1.0f) ++d1;
			if (contribMax[ci] < 2.0f) ++d2;
		}
		static std::atomic<long long> aTiles{0}, aN{0}, a05{0}, a1{0}, a2{0};
		aTiles.fetch_add(1, std::memory_order_relaxed);
		aN.fetch_add(contribN, std::memory_order_relaxed);
		a05.fetch_add(d05, std::memory_order_relaxed);
		a1.fetch_add(d1, std::memory_order_relaxed);
		a2.fetch_add(d2, std::memory_order_relaxed);
		const long long t = aTiles.load(std::memory_order_relaxed);
		if (t > 0 && (t % 2000) == 0) {
			const double td = double(t);
			std::fprintf(stderr,
			    "[CONTRIB-CULL] lights/tile=%.1f  droppable (max linear diffuse contrib < thr): "
			    "<0.5=%.1f  <1.0=%.1f  <2.0=%.1f  (per tile, %lld tiles)\n",
			    double(aN.load())/td, double(a05.load())/td,
			    double(a1.load())/td, double(a2.load())/td, t);
		}
	}

	// One permit per completed tile (see renderns::tileDone in RENDER.CPP).
	renderns::tileDone.release();
}

// Per-pixel deferred lighting for transparent (front-facing) surfaces.
// Reads the transparent G-buffer (mat32 + normal + xpr-Z, populated by
// MekaleleTransparent in RenderInnerDeferredTransparent for the closest
// front-facing transparent at each pixel). Computes per-pixel ambient
// + Lambertian + fog, modulates the texture sample, alpha-blends 50/50
// onto vpage (matches TheOtherBarry<TRANSPARENT>'s blend rule). Specular
// + checkerboard / quarter-rate / vec paths omitted — transparent
// coverage is small, scalar is fine until profiling says otherwise.
// Templated on which transparent layer to light: Front reads
// g_gbufferTransparent + g_xparZ; Back reads g_gbufferTransparentBack +
// g_xparZBack. Both kernels are identical apart from the buffer pointers
// — kept as one function body so they don't drift.
enum class XparLayer { Front, Back };

template <XparLayer Layer>
static void Render_DeferredTransparentLighting_Tile(const DeferredLightingCtx &ctx,
                                                     int tileIndex,
                                                     int x1, int y1, int x2, int y2)
{
	// Addressing from ctx, not globals (RenderContext migration). The
	// transparent-layer pointers (g_gbufferTransparent*/g_xparZ*) stay global
	// for now — they migrate with the rest of the xpar path.
	const int xres = ctx.xres;
	byte *const vpage = ctx.vpage;
	word *const zpage16 = ctx.zpage16;
	const float cntrEX = ctx.cntrEX, cntrEY = ctx.cntrEY;
	meka::GBuffer *gbPtr;
	uint16_t      *zPtr;
	if constexpr (Layer == XparLayer::Front) {
		gbPtr = g_gbufferTransparent;
		zPtr  = g_xparZ;
	} else {
		gbPtr = g_gbufferTransparentBack;
		zPtr  = g_xparZBack;
	}
	if (!gbPtr || !zPtr) return;
	const meka::GBuffer &gbX = *gbPtr;
	uint16_t *xparZ = zPtr;
	dword *out = reinterpret_cast<dword *>(vpage);
	// Conservative per-pixel light cull: bypasses the per-tile filter,
	// useful for diagnosing whether observed lighting steps come from
	// culling drift at tile seams.
	const bool lightAll = fds::FeatureFlags::xpar_light_all();
	const bool hdrBufReady = fds::Hdr_WritableFor(ctx.xres, ctx.yres);  // main pass only (not the mirror RTT)
	const bool hdrLinear = hdrBufReady && fds::FeatureFlags::hdr() && fds::FeatureFlags::hdr_linear();  // HDR C2
	// Procedural water composite (Stage B) — fresnel mix of a deep colour and the
	// reflection underlay, for the water matID only. Hoisted; per-pixel fresnel below.
	const bool  waterProcOn = fds::FeatureFlags::water_procedural();
	const float wDeepB = fds::FeatureFlags::water_deep_b();
	const float wDeepG = fds::FeatureFlags::water_deep_g();
	const float wDeepR = fds::FeatureFlags::water_deep_r();
	const float wRefl  = fds::FeatureFlags::water_reflectivity();
	const float wFresBase = fds::FeatureFlags::water_fresnel_base();   // reflection floor looking down
	const TileLights &tlTile = ctx.tileLights[tileIndex];
	const ViewLightsSoA *vlAll = ctx.lights;
	const int allCount = ctx.numLights;

	for (int py = y1; py < y2; ++py) {
		for (int px = x1; px < x2; ++px) {
			const size_t i = size_t(py) * xres + px;
			const uint32_t mat32 = gbX.txtr[i];
			if (mat32 == 0xFFFFFFFFu) continue;  // no transparent front
			const word zEnc = xparZ[i];
			if (zEnc == 0) continue;
			// Per-pixel mirror id from the transparent layer's plane.
			// Used to gate the light loops below (clone surfaces should
			// see only their own mirror's omnis).
			const uint32_t pmid = gbX.mirrorId.empty()
			    ? 0u : uint32_t(gbX.mirrorId[i]);
			// Opaque-Z occlusion: if opaque zpage16 has a larger z_candidate
			// at this pixel, the opaque surface is CLOSER to camera than
			// the transparent we wrote into xpr. Reject the transparent —
			// it's hidden behind opaque. MekaleleTransparent doesn't see
			// opaque Z during raster (its zbuffer points at xpr-Z) so we
			// have to gate here. Without this, transparent surfaces show
			// through opaque walls (Greets banding behind marble).
			const word zOpaque = zpage16[i];
			if (zOpaque > zEnc) continue;

			const uint32_t miplevel   = (mat32 >> 28) & 0xF;
			const uint32_t matID      = (mat32 >> 20) & 0xFF;
			const uint32_t swizzledUV = mat32 & 0xFFFFF;
			if (matID >= ctx.matTable.count) continue;
			Material *Mat = ctx.matTable.data[matID];
			if (!Mat || !Mat->Txtr) continue;
			// Negative XparBlendAlpha sentinel: skip this pixel
			// entirely so out[i] (= the opaque shading already written
			// by the opaque pass) is preserved. Used by mirror walls
			// — the wall rasterised into xpar only to bound the mask
			// + xparZ, but contributes no colour and must NOT pass
			// through the legacy `litRGB + dst/2` composition (which
			// halves the reflected clones behind it).
			if (Mat->XparBlendAlpha < 0.0f) continue;

			// Resolved 16-bit ShadowMatID — see scalar path comment above.
			// The xpar G-buffer (gbX) does not currently allocate the
			// shadowMatID plane (xpar surfaces don't participate in
			// PolyId shadows much), so this falls through to the
			// matID+1 path unless someone adds the plane later.
			const int surfaceShadowId = gbX.shadowMatID.empty()
			    ? int(matID + 1)
			    : int(gbX.shadowMatID[i]);

			const dword *texData = (const dword *)Mat->Txtr->Mipmap[miplevel];
			if (!texData) continue;
			const dword texel = texData[swizzledUV];
			const float texB = float(texel & 0xFF);
			const float texG = float((texel >> 8) & 0xFF);
			const float texR = float((texel >> 16) & 0xFF);

			float nx, ny, nz;
			meka::oct_decode_u16(gbX.normal[i], nx, ny, nz);

			const float z = float(0xFF80 - zEnc) * ctx.invZScale;
			const float x = (float(px) - cntrEX) * z * ctx.invFOVX;
			const float y = (cntrEY - float(py)) * z * ctx.invFOVY;

			float lB = Mat->Luminosity * 255.0f + Mat->Diffuse * ctx.Sc->Ambient.B;
			float lG = Mat->Luminosity * 255.0f + Mat->Diffuse * ctx.Sc->Ambient.G;
			float lR = Mat->Luminosity * 255.0f + Mat->Diffuse * ctx.Sc->Ambient.R;

			// Crystal specular for transparent surfaces (e.g. fountain
			// mizraka glass). Same Blinn-Phong as the opaque kernel; gated
			// on Mat->Specular > 0 so non-spec transparents (water, spire
			// orbs) pay zero. Accumulated into sB/sG/sR alongside diffuse
			// and added on top of the texture-modulated lit color (spec
			// highlights aren't tinted by the base texture — standard
			// model).
			const bool wantSpecular = (Mat->Specular > 0.0f);
			const float gloss = Mat->Glossiness > 0 ? float(Mat->Glossiness) : 32.0f;
			float vx_v = 0, vy_v = 0, vz_v = 0;
			if (wantSpecular) {
				const float vlen2 = x*x + y*y + z*z;
				const float vlenInv = fast_rsqrt(vlen2);
				vx_v = -x * vlenInv;
				vy_v = -y * vlenInv;
				vz_v = -z * vlenInv;
			}
			float sB = 0, sG = 0, sR = 0;

			// Water gets ambient-only — forward Lighting() culls every omni
			// (water mesh has a tiny BSphereRadius from Reflective_Surface_
			// Setup's tessellation, so range+radius < |omni-bsphere| for all
			// city omnis). Without skipping the omni loop here, every nearby
			// city light slams into every water pixel and saturates the
			// channel → cyan/turquoise water instead of forward's deep blue.
			// Mirrors the same skip in Render_DeferredLighting_Tile.
			const bool isWater = (int(matID) == ctx.waterMatID);

			// Procedural water: Schlick fresnel from the view angle (the wave-
			// normal micro detail is the screen-space glint pass; this is the
			// macro deep-vs-reflection mix). vDotN = view·N in view space.
			const bool waterProc = isWater && waterProcOn;
			float wFres = 0.0f;
			if (waterProc) {
				const float vl2 = x*x + y*y + z*z;
				const float vDotN = -(nx*x + ny*y + nz*z) * fast_rsqrt(vl2 > 0.0f ? vl2 : 1.0f);
				float c = 1.0f - (vDotN > 0.0f ? vDotN : 0.0f);
				const float c5 = c*c*c*c*c;
				wFres = wFresBase + (wRefl - wFresBase) * c5;
				if (wFres < 0.0f) wFres = 0.0f; if (wFres > 1.0f) wFres = 1.0f;
			}

			// Light loop. Default: per-tile compacted list. Diagnostic
			// mode: full scene omni list (FDS_XPAR_LIGHT_ALL=1).
			if (isWater) {
				// Skip omni accumulation — matches the opaque kernel's
				// isWater guard.
			} else if (lightAll) {
				for (int n = 0; n < allCount; ++n) {
					if (vlAll->mirrorId[n] != pmid) continue;
					const float wx = vlAll->posX[n] - x;
					const float wy = vlAll->posY[n] - y;
					const float wz = vlAll->posZ[n] - z;
					const float dot = wx * nx + wy * ny + wz * nz;
					if (dot < 0.0f) continue;
					const float len2 = wx*wx + wy*wy + wz*wz;
					if (len2 > vlAll->range2[n]) continue;
					const float lenInv = fast_rsqrt(len2);
					const float dist   = len2 * lenInv;
					const float falloff = (1.0f - dist * vlAll->rRange[n]);
					float k = dot * lenInv * falloff;
					if (vlAll->isSpot[n]) {
						const float cosTheta = -(vlAll->dirX[n]*wx + vlAll->dirY[n]*wy + vlAll->dirZ[n]*wz) * lenInv;
						if (cosTheta <= vlAll->cosOuter[n]) continue;
						if (cosTheta < vlAll->cosInner[n]) {
							float t = (cosTheta - vlAll->cosOuter[n]) / (vlAll->cosInner[n] - vlAll->cosOuter[n]);
							k *= t * t * (3.0f - 2.0f * t);
						}
					}
					const float intensity = k * Mat->Diffuse;
					lB += intensity * vlAll->colB[n];
					lG += intensity * vlAll->colG[n];
					lR += intensity * vlAll->colR[n];

					if (wantSpecular) {
						const float ldx = wx * lenInv;
						const float ldy = wy * lenInv;
						const float ldz = wz * lenInv;
						float hx = ldx + vx_v, hy = ldy + vy_v, hz = ldz + vz_v;
						const float hLen2 = hx*hx + hy*hy + hz*hz;
						if (hLen2 <= 0.0f) continue;
						const float hLenInv = fast_rsqrt(hLen2);
						hx *= hLenInv; hy *= hLenInv; hz *= hLenInv;
						const float NdotH = nx*hx + ny*hy + nz*hz;
						if (NdotH <= 0.0f) continue;
						const float specStrength = std::pow(NdotH, gloss) *
							Mat->Specular * falloff;
						sB += specStrength * vlAll->colB[n];
						sG += specStrength * vlAll->colG[n];
						sR += specStrength * vlAll->colR[n];
					}
				}
			} else {
				for (int n = 0; n < tlTile.count; ++n) {
					if (tlTile.mirrorId[n] != pmid) continue;
					const float wx = tlTile.posX[n] - x;
					const float wy = tlTile.posY[n] - y;
					const float wz = tlTile.posZ[n] - z;
					const float dot = wx * nx + wy * ny + wz * nz;
					if (dot < 0.0f) continue;
					const float len2 = wx*wx + wy*wy + wz*wz;
					if (len2 > tlTile.range2[n]) continue;
					const float lenInv = fast_rsqrt(len2);
					const float dist   = len2 * lenInv;
					const float falloff = (1.0f - dist * tlTile.rRange[n]);
					float k = dot * lenInv * falloff;
					if (tlTile.isSpot[n]) {
						const float cosTheta = -(tlTile.dirX[n]*wx + tlTile.dirY[n]*wy + tlTile.dirZ[n]*wz) * lenInv;
						if (cosTheta <= tlTile.cosOuter[n]) continue;
						if (cosTheta < tlTile.cosInner[n]) {
							float t = (cosTheta - tlTile.cosOuter[n]) / (tlTile.cosInner[n] - tlTile.cosOuter[n]);
							k *= t * t * (3.0f - 2.0f * t);
						}
					}
					const float intensity = k * Mat->Diffuse;
					lB += intensity * tlTile.colB[n];
					lG += intensity * tlTile.colG[n];
					lR += intensity * tlTile.colR[n];

					if (wantSpecular) {
						const float ldx = wx * lenInv;
						const float ldy = wy * lenInv;
						const float ldz = wz * lenInv;
						float hx = ldx + vx_v, hy = ldy + vy_v, hz = ldz + vz_v;
						const float hLen2 = hx*hx + hy*hy + hz*hz;
						if (hLen2 <= 0.0f) continue;
						const float hLenInv = fast_rsqrt(hLen2);
						hx *= hLenInv; hy *= hLenInv; hz *= hLenInv;
						const float NdotH = nx*hx + ny*hy + nz*hz;
						if (NdotH <= 0.0f) continue;
						const float specStrength = std::pow(NdotH, gloss) *
							Mat->Specular * falloff;
						sB += specStrength * tlTile.colB[n];
						sG += specStrength * tlTile.colG[n];
						sR += specStrength * tlTile.colR[n];
					}
				}
			}

			if (lB > 250.0f) lB = 250.0f;
			if (lG > 250.0f) lG = 250.0f;
			if (lR > 250.0f) lR = 250.0f;
			if (lB < 0.0f) lB = 0.0f;
			if (lG < 0.0f) lG = 0.0f;
			if (lR < 0.0f) lR = 0.0f;

			// Fast-fog active this frame (froxel grid or screen-space): fog
			// is applied to the FINAL lit color below (lit·T(z) + acc(z))
			// instead of the legacy per-light sqrt ramp here — with the
			// background already fully fogged by the fast-fog composite,
			// the exact blend is out = α·(C·T + acc) + (1−α)·Bg.
			const bool froxelFog = FastFog_XparActive();
			const bool ssFog     = !froxelFog && FastFog_SSActive();
			if (!froxelFog && !ssFog && (ctx.Sc->Flags & Scn_Fogged)) {
				// sqrt(t) via fast_rsqrt: sqrt(t) = t * rsqrt(t).
				// Guarded against t<=0 (rsqrt undefined at 0).
				const float t = 1.0f - z * (1.0f / ctx.Sc->FZP);
				const float fogRate = t > 0.0f ? t * fast_rsqrt(t) : 0.0f;
				lB = std::min(std::max(lB * fogRate, 10.0f), 253.0f);
				lG = std::min(std::max(lG * fogRate, 10.0f), 253.0f);
				lR = std::min(std::max(lR * fogRate, 10.0f), 253.0f);
			}

			// HDR mirror reflection (b): the deferred RTT baked this panel's FLOAT
			// reflection radiance (gain·reflection + linear text) into Mat->hdrRefl.
			// Sample it emissively so reflected highlights exceed 255 and bloom
			// through the main tonemap, instead of clamping at the 8-bit panel
			// texture. The G-buffer carries a swizzled mip-0 texel index, so invert
			// the Sachletz (4x4 block, X-outer/Y-inner) back to row-major iu/iv;
			// mip>0 (distant panel) falls back to the 8-bit albedo path below.
			// pmid == 0: only the DIRECT (original) panel samples its float
			// reflection. The same material is shared by clone copies (order-2 /
			// portal reflections, pmid > 0) whose G-buffer texel index belongs to
			// a different render context — sampling hdrRefl there reads float
			// radiance into the wrong place and blows out. Clones fall back to the
			// 8-bit panel texture (order-2 HDR reflection is a later extension).
			const bool isHdrRefl = (Mat->Flags & Mat_HdrReflection) && Mat->hdrRefl
			                       && fds::g_hdrActive && miplevel == 0 && pmid == 0
			                       && Mat->hdrReflW > 0 && Mat->hdrReflH > 0;
			int litB, litG, litR;
			if (isHdrRefl) {
				const int W = Mat->hdrReflW, H = Mat->hdrReflH;
				const int within = int(swizzledUV) & 15, blk = int(swizzledUV) >> 4;
				const int blocksY = H >> 2;
				int iu = (blk / blocksY) * 4 + (within & 3);
				int iv = (blk % blocksY) * 4 + ((within >> 2) & 3);
				if (iu >= W) iu = W - 1;
				if (iv >= H) iv = H - 1;
				const float* hr = Mat->hdrRefl + (size_t(iv) * size_t(W) + size_t(iu)) * 3;
				litB = int(hr[0]); litG = int(hr[1]); litR = int(hr[2]);
			} else if (hdrLinear) {
				// Full coherence: linearize the transparent albedo (gamma-2.0
				// square) so it composes in the linear buffer like the opaque
				// kernel (B2); light stays linear.
				const float kN = 1.0f/255.0f;
				litB = int((texB*kN)*(texB*kN)*lB);
				litG = int((texG*kN)*(texG*kN)*lG);
				litR = int((texR*kN)*(texR*kN)*lR);
			} else {
				litB = int((texB * lB) * (1.0f / 256.0f));
				litG = int((texG * lG) * (1.0f / 256.0f));
				litR = int((texR * lR) * (1.0f / 256.0f));
			}
			// Procedural water: deep colour x (1-fresnel); the reflection underlay
			// gets weight = fresnel in the blend below -> out ~ lerp(deep, refl,
			// fresnel). Fog applies to both afterward, so it stays correct.
			if (waterProc) {
				// Deep colour only here; the coherent (field-warped) albedo texture
				// is blended in the screen-space water pass, not the swizzled kernel.
				const float k = 1.0f - wFres;
				litB = int(wDeepB * k); litG = int(wDeepG * k); litR = int(wDeepR * k);
			}
			// Specular added on top — independent of base color tint. Skipped for
			// the HDR-reflection path (hdrRefl already carries the full radiance).
			if (wantSpecular && !isHdrRefl) {
				float fogScale = 1.0f;
				if (!froxelFog && !ssFog && (ctx.Sc->Flags & Scn_Fogged)) {
					const float t = 1.0f - z * (1.0f / ctx.Sc->FZP);
					fogScale = t > 0.0f ? t * fast_rsqrt(t) : 0.0f;
				}
				litB += int(sB * fogScale);
				litG += int(sG * fogScale);
				litR += int(sR * fogScale);
			}
			if ((froxelFog || ssFog) && !isHdrRefl) {
				float aR_, aG_, aB_, T_;
				if (froxelFog) FastFog_SampleGrid(px, py, z, aR_, aG_, aB_, T_);
				else           FastFog_SSSample(px, py, z, aR_, aG_, aB_, T_);
				// In-scatter weight per blend rule. The path's fog [0,z] must
				// appear ONCE in the final pixel: the blend keeps dstWeight of
				// the background (which already carries that fog), so the
				// layer contributes acc·(1−dstWeight)·srcWeightInv. α-blend
				// (dst (1−α), src ×α later) → acc·1 here. Legacy additive
				// (dst/2, src ×1) → acc·0.5 — full acc double-counts and
				// blew the city water out white.
				const float accW = (Mat->XparBlendAlpha > 0.0f) ? 1.0f : 0.5f;
				litR = int(float(litR)*T_ + aR_*accW);
				litG = int(float(litG)*T_ + aG_*accW);
				litB = int(float(litB)*T_ + aB_*accW);
			}
			if (!fds::g_hdrActive) {     // HDR: leave unclamped so transparents bloom too
				if (litB > 255) litB = 255;
				if (litG > 255) litG = 255;
				if (litR > 255) litR = 255;
			}
			if (litB < 0) litB = 0;
			if (litG < 0) litG = 0;
			if (litR < 0) litR = 0;

			// Alpha-blend onto the destination. Default: `litRGB + dst/2`
			// saturated — matches forward TheOtherBarry<TRANSPARENT>
			// (source-full + dest-halved, NOT 50/50).
			// Opt-in per-material (Mat->XparBlendAlpha > 0): linear
			// interpolate `litRGB * α + dst * (1-α)`, no saturation
			// cap. Fountain orb glass uses α=0.4 for a more
			// see-through look; other transparents in the same scene
			// (e.g. fountain spires) keep the legacy formula.
			//
			// HDR overlay reorg: in HDR mode the dst is the float radiance in
			// g_hdrBuf (lit+fog, unclamped — this runs after the froxel
			// composite), and the composite stays in float with no 255 cap so
			// the bolt/flash/fog visible THROUGH transparents blooms and rolls
			// off at the tonemap instead of clipping. Gated on g_hdrActive so
			// the LDR path (flag off, fog-off scenes) is byte-identical.
			if (fds::g_hdrActive && hdrBufReady) {   // hdrBufReady: skip in the mirror RTT
				fds::hdrf* h = fds::g_hdrBuf.data() + i * 4;
				const float dB = h[0], dG = h[1], dR = h[2];
				if (Mat->XparBlendAlpha > 0.0f) {
					const float a = Mat->XparBlendAlpha;
					const float ia = 1.0f - a;
					h[0] = float(litB) * a + dB * ia;
					h[1] = float(litG) * a + dG * ia;
					h[2] = float(litR) * a + dR * ia;
				} else {
					const float dw = waterProc ? wFres
					: (Mat->Transparency > 0.0f ? Mat->Transparency * 0.01f : 0.5f);
				// water: reflection at fresnel weight. Legacy additive blend
				// (out = lit + behind*dw): dw follows the AUTHORED transparency
				// (LWO TRAN, 0-100%%) so the editor slider has a real effect —
				// the shipped screens author 50%% -> dw 0.5, byte-identical.
				// Hand-built xpar materials (mirrors) author Transparency=0 and
				// keep the legacy 0.5.
					h[0] = float(litB) + dB * dw;
					h[1] = float(litG) + dG * dw;
					h[2] = float(litR) + dR * dw;
				}
			} else {
			const dword existing = out[i];
			const int dB = int((existing      ) & 0xFF);
			const int dG = int((existing >>  8) & 0xFF);
			const int dR = int((existing >> 16) & 0xFF);
			int outB, outG, outR;
			if (Mat->XparBlendAlpha > 0.0f) {
				const float a = Mat->XparBlendAlpha;
				const float ia = 1.0f - a;
				outB = int(float(litB) * a + float(dB) * ia);
				outG = int(float(litG) * a + float(dG) * ia);
				outR = int(float(litR) * a + float(dR) * ia);
			} else {
				const float dw = waterProc ? wFres
					: (Mat->Transparency > 0.0f ? Mat->Transparency * 0.01f : 0.5f);
				// water: reflection at fresnel weight. Legacy additive blend
				// (out = lit + behind*dw): dw follows the AUTHORED transparency
				// (LWO TRAN, 0-100%%) so the editor slider has a real effect —
				// the shipped screens author 50%% -> dw 0.5, byte-identical.
				// Hand-built xpar materials (mirrors) author Transparency=0 and
				// keep the legacy 0.5.
				outB = litB + int(float(dB) * dw);
				outG = litG + int(float(dG) * dw);
				outR = litR + int(float(dR) * dw);
				if (outB > 255) outB = 255;
				if (outG > 255) outG = 255;
				if (outR > 255) outR = 255;
			}
			out[i] = dword(outB) | (dword(outG) << 8) | (dword(outR) << 16) | 0xFF000000u;
			}
		}
	}
	// See the Front-layer variant above for why no release() here.
}

// FDS_DEFERRED_UNIFIED_TBR=1 routes the deferred transparent + sprite
// rendering through the unified per-strip TBR walk (task #9). When off
// (default during validation), the existing per-clump xpar peel runs
// alongside the standalone sprite TBR — the legacy behaviour.
//
// Also requires the current scene to have called TBR_Init (so the per-
// strip linked-list storage exists). Scenes without a TBR — city,
// greets — fall back to the legacy peel; the unified path with no
// SBufferHead would silently drop every transparent face, since
// InsertTransparentToTBR bails when NumTiles==0. Only fountain
// currently calls TBR_Init.
bool deferredUnifiedTbrEnabled(const Scene *sc) {
	if (!fds::FeatureFlags::deferred_unified_tbr()) return false;
	if (!sc) return false;
	if (!sc->SBufferHead || sc->NumTiles == 0) return false;
	// Offscreen pass (mirror RTT, cube bake): the YRes global is swapped
	// but the strip arrays are main-sized — fall back to the legacy peel
	// there (see TBR_MatchesTarget).
	if (!TBR_MatchesTarget(sc)) return false;
	return true;
}

// Effective transparent depth-peel passes (per side): the CLI/env flag wins
// when explicitly set, otherwise the current scene's per-scene default
// (Scene::XparPeelPasses; 0 → legacy 1). Used by both xpar dispatch paths.
int xparPeelPassesEffective(const Scene *sc) {
	if (fds::FeatureFlags::isSet(fds::FeatureFlags::IntId::xpar_peel_passes)) {
		const int f = fds::FeatureFlags::xpar_peel_passes();
		return f < 1 ? 1 : f;
	}
	const int p = sc ? int(sc->XparPeelPasses) : 0;
	return p < 1 ? 1 : p;
}

// Per-strip xpar render helper, called from TBR_Render's per-strip walk
// for each clump of consecutive same-(mesh, frontFacing) transparent
// faces in the sorted item list. The strip covers rows [strip_y,
// strip_y+strip_h) (strip_h is normally TILESIZE=8, but may be smaller
// for the last strip if YRes isn't a multiple of 8).
//
// One xpar G-buffer layer is dirtied per clump (back layer for back-
// facing tris, front layer for front-facing tris). We clear only the
// strip's slice (61 KB per layer at 1920 wide), raster the clump's
// faces with clipper extents = strip rect, then composite the strip
// rows via Render_DeferredTransparentLighting_Tile<Layer>.
void RenderXparClumpInStrip(const DeferredLightingCtx &dctx,
                             Face** faces, int count, bool front,
                             int strip_y, int strip_h)
{
	const size_t rowStart  = size_t(strip_y) * size_t(dctx.xres);
	const size_t rowCount  = size_t(strip_h) * size_t(dctx.xres);
	const int peelPasses = xparPeelPassesEffective(dctx.Sc);
	meka::GBuffer* sideGB = front ? dctx.gbXpar : g_gbufferTransparentBack;
	uint16_t*      sideZ  = front ? dctx.xparZ  : dctx.xparZBack;

	// Raster the clump's faces into the side layer, then composite the strip
	// rows. Clipper extents pin the rasterizer to the strip; faces route to
	// MekaleleTransparent (front) / MekaleleTransparentBack (back). The
	// composite uses a ctx variant whose tileLights -> g_stripLights so the
	// strip gets exactly the lights overlapping its 8 rows.
	auto rasterAndComposite = [&]() {
		FrustumClipper clipper;
		clipper.InitViewport(dctx.Sc);
		clipper.SetClippingExtents(0.0f, float(strip_y),
		                            float(dctx.xres), float(strip_y + strip_h));
		const auto rt  = fds::MainRenderTargetFromGlobals();
		const auto& cam = fds::g_mainCamera;
		// Strip-clamp the rasterizer: a clipped tri whose max PY snapped to
		// strip_y + strip_h would compute tile_My one tile-row past the strip
		// and write pixels the neighbouring strip's clear (concurrent) wipes.
		const int stripTileRow = strip_y >> 3;  // TILELOG=3
		const meka::RasterStripClamp savedClamp = meka::g_rasterStripClamp;
		meka::g_rasterStripClamp = { stripTileRow, stripTileRow };
		for (int i = 0; i < count; ++i) {
			Face* F = faces[i];
			if (!F) continue;
			if (front) clipper.Render(F, MekaleleTransparent,     false, rt, cam);
			else       clipper.Render(F, MekaleleTransparentBack, false, rt, cam);
		}
		meka::g_rasterStripClamp = savedClamp;

		const int stripIdx = strip_y >> 3;  // TILELOG=3
		DeferredLightingCtx stripCtx = dctx;
		stripCtx.tileLights = g_stripLights;
		if (front) {
			Render_DeferredTransparentLighting_Tile<XparLayer::Front>(
				stripCtx, stripIdx, 0, strip_y, dctx.xres, strip_y + strip_h);
		} else {
			Render_DeferredTransparentLighting_Tile<XparLayer::Back>(
				stripCtx, stripIdx, 0, strip_y, dctx.xres, strip_y + strip_h);
		}
	};

	// thread_local: this strip runs on its own worker; the synchronous
	// clipper.Render above reads g_xparPeelReverse on the same thread.
	g_xparPeelReverse = (peelPasses > 1);

	if (peelPasses <= 1) {
		// Legacy single-fragment peel — byte-identical. Clear the strip's
		// slice of the side layer (txtr=empty, Z=0) and render once.
		if (sideGB) std::memset(sideGB->txtr.data() + rowStart, 0xFF, rowCount * sizeof(uint32_t));
		if (sideZ)  std::memset(sideZ + rowStart, 0, rowCount * sizeof(uint16_t));
		rasterAndComposite();
		return;
	}

	// Reverse depth peel WITHIN this (clump, side), K passes deep, over the
	// strip's row slice — farthest-first, compositing each over the last.
	for (int pass = 0; pass < peelPasses; ++pass) {
		// Per-pass ceiling: pass 0 accepts all (0); later passes accept only
		// fragments nearer than the previous pass's peeled Z.
		if (pass == 0) {
			if (g_xparPeelFloor) std::memset(g_xparPeelFloor + rowStart, 0, rowCount * sizeof(uint16_t));
		} else if (g_xparPeelFloor && sideZ) {
			std::memcpy(g_xparPeelFloor + rowStart, sideZ + rowStart, rowCount * sizeof(uint16_t));
		}
		// Clear side layer slice: Z to 0xFFFF (keep-farthest init), txtr empty.
		if (sideGB) std::memset(sideGB->txtr.data() + rowStart, 0xFF, rowCount * sizeof(uint32_t));
		if (sideZ)  std::memset(sideZ + rowStart, 0xFF, rowCount * sizeof(uint16_t));
		rasterAndComposite();
	}
}

// Wave-2 of checkerboard: fills the odd (skipped) cells. For each odd
// pixel: if both horizontal neighbors share the same matID, average
// their already-shaded VPage values (~5 cycles per pixel — 2 loads,
// 3 lane averages, 1 store). If matID disagrees (material edge), fall
// back to full deferred shading for that pixel via the same kernel as
// wave-1. The mismatch rate on City is ~5% per the snapshot pixel
// distribution — most of the frame is large continuous surfaces.
//
// Outer-SIMD diffuse kernel: 8 pixels per row in vec registers. The
// scalar gather (matTable → Material* → Mipmap[mip] → texel + mat
// scalars) happens once per lane and stays scalar — arm64-via-simde
// has no usable vgather. Everything after — texel byte-extract, oct
// normal decode, view-space pos reconstruct, per-omni dot/range/k
// accumulate, saturate, modulate — runs 8-wide. Specular and water
// fall back to scalar tail when needed; checkerboard is handled by
// dropping odd lanes via the alive mask.
static void Render_DeferredLighting_Tile_OuterVec(const DeferredLightingCtx &ctx,
                                                   int tileIndex,
                                                   int x1, int y1, int x2, int y2)
{
	// Addressing from ctx, not globals (RenderContext migration).
	const int xres = ctx.xres;
	byte *const vpage = ctx.vpage;
	word *const zpage16 = ctx.zpage16;
	const float cntrEX = ctx.cntrEX, cntrEY = ctx.cntrEY;
	const meka::GBuffer &gb = *ctx.gb;
	dword *out = reinterpret_cast<dword *>(vpage);
	const bool   quarter      = deferredLightingQuarterEnabled();
	const bool   checker      = deferredLightingCheckerboardEnabled() && !quarter;
	const bool   specGlobalOn = Specular_Factor > 0.0f;
	const TileLights &tl = ctx.tileLights[tileIndex];
	const float  ambB_sc = float(ctx.Sc->Ambient.B);
	const float  ambG_sc = float(ctx.Sc->Ambient.G);
	const float  ambR_sc = float(ctx.Sc->Ambient.R);

	// Pre-extract per-lane data into 8-aligned scratch buffers so we
	// can do gather work scalar-per-lane and load_a back into vec.
	alignas(32) uint32_t lane_mat32[8];
	alignas(32) uint32_t lane_alive[8];
	alignas(32) float    lane_texB[8], lane_texG[8], lane_texR[8];
	alignas(32) float    lane_ambB[8], lane_ambG[8], lane_ambR[8];
	alignas(32) float    lane_diffuse[8];
	alignas(32) float    lane_specular[8];
	alignas(32) float    lane_gloss[8];
	alignas(32) uint32_t lane_wantSpec[8];
	alignas(32) uint32_t lane_isWater[8];
	// Per-lane mirror id, widened uint8→uint32 once per 8-pixel block.
	// The omni loop builds a per-lane mask against broadcast(tl.mirrorId[n]).
	alignas(32) uint32_t lane_mirrorId[8];

	for (int py = y1; py < y2; ++py) {
		// vec body: groups of 8 pixels
		int px = x1;
		// Align start to 8-pixel boundary so loads from gb arrays are
		// naturally aligned (gb.txtr/gb.normal are aligned at frame
		// allocation; zpage16 aligned by parallel_memset).
		const int x_vec_start = (x1 + 7) & ~7;
		// Scalar lead-in for unaligned pixels at tile start.
		while (px < x_vec_start && px < x2) {
			// Tail: just defer to scalar; rare since tile boundaries
			// are 8-aligned for our 1920x1080 / 12x8 grid (160-wide tiles).
			++px;
		}

		for (; px < x2; px += 8) {
			const size_t i = size_t(py) * xres + px;
			// Per-lane mirror id snapshot for this 8-pixel block. Used
			// by the omni loop below to mask off lights whose mirrorId
			// disagrees with the lane's. Plane is byte-sized, widened
			// to uint32 here so cmpeq lines up with tl.mirrorId.
			if (gb.mirrorId.empty()) {
				for (int k = 0; k < 8; ++k) lane_mirrorId[k] = 0u;
			} else {
				for (int k = 0; k < 8; ++k)
					lane_mirrorId[k] = uint32_t(gb.mirrorId[i + k]);
			}

			// Load 8 lanes of Z (u16 → s32 zero-extend → float for the
			// cmp; later use the int form too)
			__m128i z16 = _mm_loadu_si128((const __m128i*)(zpage16 + i));
			__m256i zEncI = _mm256_cvtepu16_epi32(z16);

			// Alive mask: zEnc != 0
			__m256i mask_alive = _mm256_cmpgt_epi32(zEncI, _mm256_setzero_si256());

			// Lane-in-range mask: when this iteration straddles the
			// right tile edge (x2 - px < 8), lanes that fall in the
			// NEXT tile must be masked off so we don't write past x2.
			// For full groups (x2 - px >= 8) every lane is in range
			// and the AND is a no-op. Tile widths at non-multiple-of-8
			// resolutions (e.g., HiDPI 1512/12 = 126) would otherwise
			// leave the trailing 6 columns of each tile-column
			// unrendered — visible as vertical dark bands at every
			// tile-X boundary.
			{
				__m256i lane_idx2 = _mm256_setr_epi32(0,1,2,3,4,5,6,7);
				__m256i in_range  = _mm256_cmpgt_epi32(
					_mm256_set1_epi32(x2 - px), lane_idx2);
				mask_alive = _mm256_and_si256(mask_alive, in_range);
			}

			// Checkerboard: drop odd cells in wave-1.
			if (checker) {
				// (px+lane + py) & 1 == 0 → keep
				__m256i lane_idx = _mm256_setr_epi32(0,1,2,3,4,5,6,7);
				__m256i px_lane  = _mm256_add_epi32(_mm256_set1_epi32(px), lane_idx);
				__m256i parity   = _mm256_and_si256(
					_mm256_xor_si256(px_lane, _mm256_set1_epi32(py)),
					_mm256_set1_epi32(1));
				__m256i keep = _mm256_cmpeq_epi32(parity, _mm256_setzero_si256());
				mask_alive = _mm256_and_si256(mask_alive, keep);
			}
			// Quarter-rate: keep only lanes where both px AND py even.
			if (quarter) {
				if (py & 1) continue;  // entire row skipped in wave-1
				__m256i lane_idx = _mm256_setr_epi32(0,1,2,3,4,5,6,7);
				__m256i px_lane  = _mm256_add_epi32(_mm256_set1_epi32(px), lane_idx);
				__m256i parity_x = _mm256_and_si256(px_lane, _mm256_set1_epi32(1));
				__m256i keep = _mm256_cmpeq_epi32(parity_x, _mm256_setzero_si256());
				mask_alive = _mm256_and_si256(mask_alive, keep);
			}

			// Load mat32
			__m256i mat32v = _mm256_loadu_si256((const __m256i*)(gb.txtr.data() + i));
			__m256i matIDv = _mm256_and_si256(_mm256_srli_epi32(mat32v, 20),
			                                   _mm256_set1_epi32(0xFF));
			// Mask: matID < count
			__m256i mask_id = _mm256_cmpgt_epi32(_mm256_set1_epi32(int(ctx.matTable.count)),
			                                     matIDv);
			mask_alive = _mm256_and_si256(mask_alive, mask_id);

			// Stash for scalar lane work
			_mm256_store_si256((__m256i*)lane_mat32, mat32v);
			_mm256_store_si256((__m256i*)lane_alive, mask_alive);

			// Per-lane scalar gather: resolve Material*, gather texel,
			// fill ambient + spec/diffuse scratch.
			bool any_alive = false;
			for (int k = 0; k < 8; ++k) {
				if (!lane_alive[k]) {
					lane_texB[k] = lane_texG[k] = lane_texR[k] = 0;
					lane_ambB[k] = lane_ambG[k] = lane_ambR[k] = 0;
					lane_diffuse[k] = lane_specular[k] = 0;
					lane_gloss[k] = 32.0f;
					lane_wantSpec[k] = 0;
					lane_isWater[k] = 0;
					continue;
				}
				const uint32_t m = lane_mat32[k];
				const uint32_t matID = (m >> 20) & 0xFF;
				const uint32_t mip   = (m >> 28) & 0xF;
				const uint32_t uv    = m & 0xFFFFF;
				Material *Mat = ctx.matTable.data[matID];
				if (!Mat || !Mat->Txtr) {
					lane_alive[k] = 0;
					lane_texB[k] = lane_texG[k] = lane_texR[k] = 0;
					lane_ambB[k] = lane_ambG[k] = lane_ambR[k] = 0;
					lane_diffuse[k] = lane_specular[k] = 0;
					lane_gloss[k] = 32.0f;
					lane_wantSpec[k] = 0;
					lane_isWater[k] = 0;
					continue;
				}
				const dword *texData = (const dword*)Mat->Txtr->Mipmap[mip];
				if (!texData) {
					lane_alive[k] = 0;
					lane_texB[k] = lane_texG[k] = lane_texR[k] = 0;
					lane_ambB[k] = lane_ambG[k] = lane_ambR[k] = 0;
					lane_diffuse[k] = lane_specular[k] = 0;
					lane_gloss[k] = 32.0f;
					lane_wantSpec[k] = 0;
					lane_isWater[k] = 0;
					continue;
				}
				const dword tx = texData[uv];
				lane_texB[k] = float(tx & 0xFF);
				lane_texG[k] = float((tx >> 8) & 0xFF);
				lane_texR[k] = float((tx >> 16) & 0xFF);
				const float Lumin = Mat->Luminosity;
				const float Diff  = Mat->Diffuse;
				lane_ambB[k]    = Lumin * 255.0f + Diff * ambB_sc;
				lane_ambG[k]    = Lumin * 255.0f + Diff * ambG_sc;
				lane_ambR[k]    = Lumin * 255.0f + Diff * ambR_sc;
				lane_diffuse[k] = Diff;
				lane_specular[k]= Mat->Specular;
				lane_gloss[k]   = Mat->Glossiness > 0 ? float(Mat->Glossiness) : 32.0f;
				lane_wantSpec[k]= (Mat->Specular > 0.0f && specGlobalOn) ? 0xFFFFFFFFu : 0u;
				lane_isWater[k] = (int(matID) == ctx.waterMatID) ? 0xFFFFFFFFu : 0u;
				any_alive = true;
			}
			if (!any_alive) continue;
			// Refresh alive mask from scratch (some lanes may have been
			// killed by mip-data null check above).
			__m256i mask_alive_fresh = _mm256_load_si256((const __m256i*)lane_alive);

			// Decode 8 normals in parallel. oct_decode_u16 form:
			//   qx = sign-extend(low byte), qy = sign-extend(high byte)
			//   ox = qx * (1/127), oy = qy * (1/127)
			//   az = 1 - |ox| - |oy|
			//   if (az < 0) fold ...
			//   then normalize.
			__m128i nrm16 = _mm_loadu_si128((const __m128i*)(gb.normal.data() + i));
			// Split into low-byte and high-byte signed expansion.
			__m128i nrm_qx_8 = _mm_and_si128(nrm16, _mm_set1_epi16(0xFF));
			__m128i nrm_qy_8 = _mm_srli_epi16(nrm16, 8);
			// sign-extend 8-bit to 16
			nrm_qx_8 = _mm_slli_epi16(nrm_qx_8, 8);
			nrm_qx_8 = _mm_srai_epi16(nrm_qx_8, 8);
			nrm_qy_8 = _mm_slli_epi16(nrm_qy_8, 8);
			nrm_qy_8 = _mm_srai_epi16(nrm_qy_8, 8);
			__m256i qx32 = _mm256_cvtepi16_epi32(nrm_qx_8);
			__m256i qy32 = _mm256_cvtepi16_epi32(nrm_qy_8);
			__m256 inv127 = _mm256_set1_ps(1.0f / 127.0f);
			__m256 ox = _mm256_mul_ps(_mm256_cvtepi32_ps(qx32), inv127);
			__m256 oy = _mm256_mul_ps(_mm256_cvtepi32_ps(qy32), inv127);
			__m256 absox = _mm256_andnot_ps(_mm256_set1_ps(-0.0f), ox);
			__m256 absoy = _mm256_andnot_ps(_mm256_set1_ps(-0.0f), oy);
			__m256 az = _mm256_sub_ps(_mm256_sub_ps(_mm256_set1_ps(1.0f), absox), absoy);
			// Branchless fold: t = max(-az, 0); ox -= copysign(t, ox); oy -= copysign(t, oy)
			__m256 t = _mm256_max_ps(_mm256_sub_ps(_mm256_setzero_ps(), az),
			                          _mm256_setzero_ps());
			__m256 sign_ox = _mm256_and_ps(ox, _mm256_set1_ps(-0.0f));
			__m256 sign_oy = _mm256_and_ps(oy, _mm256_set1_ps(-0.0f));
			ox = _mm256_sub_ps(ox, _mm256_or_ps(t, sign_ox));
			oy = _mm256_sub_ps(oy, _mm256_or_ps(t, sign_oy));
			// Normalize via approx_rsqrt
			__m256 lenSq = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(ox, ox),
			                                            _mm256_mul_ps(oy, oy)),
			                              _mm256_mul_ps(az, az));
			__m256 invLenN = _mm256_rsqrt_ps(lenSq);
			__m256 nx = _mm256_mul_ps(ox, invLenN);
			__m256 ny = _mm256_mul_ps(oy, invLenN);
			__m256 nz = _mm256_mul_ps(az, invLenN);

			// Per-lane normal-map sampling. Stores the geometric N back
			// to scratch, then re-runs the wave-1 kernel's Tier-B nmap +
			// TBN reconstruction for each lane whose material has a
			// normal map. Loads the perturbed N back to nx/ny/nz so the
			// vec light loop and the scalar fallback both pick it up.
			// Without this, OuterVec rendered nmap surfaces flat, which
			// looked like horizontal banding on greets's hex floor.
			alignas(32) float nx_lane[8], ny_lane[8], nz_lane[8];
			_mm256_store_ps(nx_lane, nx);
			_mm256_store_ps(ny_lane, ny);
			_mm256_store_ps(nz_lane, nz);
			for (int k = 0; k < 8; ++k) {
				if (!lane_alive[k]) continue;
				const uint32_t m = lane_mat32[k];
				const uint32_t mid  = (m >> 20) & 0xFF;
				const uint32_t mipL = (m >> 28) & 0xF;
				const uint32_t uvL  = m & 0xFFFFF;
				Material *MatN = ctx.matTable.data[mid];
				if (!MatN || !MatN->NormalMap) continue;
				float nmX, nmY, nmZ;
				if (!decodeNormalTexel(MatN->NormalMap, mipL, uvL, nmX, nmY, nmZ)) continue;
				float lnx = nx_lane[k], lny = ny_lane[k], lnz = nz_lane[k];
				float tx = 0, ty = 0, tz = 0;
				bool tangentValid = false;
				if (!gb.tangent.empty()) {
					const meka::u16 packedT = gb.tangent[i + k];
					if (packedT != 0) {
						meka::oct_decode_u16(packedT, tx, ty, tz);
						const float tDotN = tx*lnx + ty*lny + tz*lnz;
						tx -= lnx * tDotN;
						ty -= lny * tDotN;
						tz -= lnz * tDotN;
						const float tLen2 = tx*tx + ty*ty + tz*tz;
						if (tLen2 > 1e-12f) {
							const float invTLen = fast_rsqrt(tLen2);
							tx *= invTLen; ty *= invTLen; tz *= invTLen;
							tangentValid = true;
						}
					}
				}
				if (!tangentValid) {
					float refx, refy, refz;
					if (std::fabs(lny) < 0.9f) { refx = 0; refy = 1; refz = 0; }
					else                        { refx = 1; refy = 0; refz = 0; }
					tx = refy * lnz - refz * lny;
					ty = refz * lnx - refx * lnz;
					tz = refx * lny - refy * lnx;
					const float invTLen = fast_rsqrt(tx*tx + ty*ty + tz*tz);
					tx *= invTLen; ty *= invTLen; tz *= invTLen;
				}
				const float hsign = MatN->TbnHandedness;  // mirrored-UV bitangent flip
				const float bx = (lny * tz - lnz * ty) * hsign;
				const float by = (lnz * tx - lnx * tz) * hsign;
				const float bz = (lnx * ty - lny * tx) * hsign;
				float vnx = tx * nmX + bx * nmY + lnx * nmZ;
				float vny = ty * nmX + by * nmY + lny * nmZ;
				float vnz = tz * nmX + bz * nmY + lnz * nmZ;
				float invLen = fast_rsqrt(vnx*vnx + vny*vny + vnz*vnz);
				nx_lane[k] = vnx * invLen;
				ny_lane[k] = vny * invLen;
				nz_lane[k] = vnz * invLen;
			}
			nx = _mm256_load_ps(nx_lane);
			ny = _mm256_load_ps(ny_lane);
			nz = _mm256_load_ps(nz_lane);

			// Reconstruct view-space pos for 8 lanes.
			// z = (0xFF80 - zEnc) * invZScale
			__m256 zEncF = _mm256_cvtepi32_ps(zEncI);
			__m256 zv = _mm256_mul_ps(_mm256_sub_ps(_mm256_set1_ps(float(0xFF80)), zEncF),
			                           _mm256_set1_ps(ctx.invZScale));
			// x = (px+lane - cntrEX) * z * invFOVX
			__m256 px_lane_f = _mm256_add_ps(_mm256_set1_ps(float(px)),
			                                  _mm256_setr_ps(0,1,2,3,4,5,6,7));
			__m256 xv = _mm256_mul_ps(_mm256_mul_ps(_mm256_sub_ps(px_lane_f,
			                                                       _mm256_set1_ps(cntrEX)),
			                                         zv),
			                           _mm256_set1_ps(ctx.invFOVX));
			__m256 yv = _mm256_mul_ps(_mm256_mul_ps(_mm256_sub_ps(_mm256_set1_ps(cntrEY),
			                                                       _mm256_set1_ps(float(py))),
			                                         zv),
			                           _mm256_set1_ps(ctx.invFOVY));

			// Texel + ambient as vec
			__m256 texB = _mm256_load_ps(lane_texB);
			__m256 texG = _mm256_load_ps(lane_texG);
			__m256 texR = _mm256_load_ps(lane_texR);
			__m256 lB   = _mm256_load_ps(lane_ambB);
			__m256 lG   = _mm256_load_ps(lane_ambG);
			__m256 lR   = _mm256_load_ps(lane_ambR);
			__m256 vDiff = _mm256_load_ps(lane_diffuse);

			// Water lanes don't accumulate omnis.
			__m256i waterMask = _mm256_load_si256((const __m256i*)lane_isWater);
			// "alive AND not water" lanes accumulate
			__m256i omniMask  = _mm256_andnot_si256(waterMask, mask_alive_fresh);
			__m256  omniMaskF = _mm256_castsi256_ps(omniMask);

			// Early-out optimization: if every alive lane needs the
			// scalar fallback (wantSpec or isWater), the vec omni loop
			// is wasted — the fallback redoes the lighting with full
			// spec accumulation, and vfB/vfG/vfR get overridden lane-by-
			// lane in the pack loop. Skip the body entirely. For greets
			// the floor + walls are mostly spec/nmap, so this fires
			// often and recovers the regression introduced by adding
			// nmap sampling above.
			__m256i wantSpecMask = _mm256_load_si256((const __m256i*)lane_wantSpec);
			__m256i needScalar   = _mm256_or_si256(wantSpecMask, waterMask);
			__m256i needVec      = _mm256_andnot_si256(needScalar, mask_alive_fresh);
			const bool anyVecLane = !_mm256_testz_si256(needVec, needVec);

			// Per-omni accumulate. Each omni broadcast, 8 pixels in vec.
			const bool profNoLights = fds::FeatureFlags::prof_no_lights();
			const int omniLoopN = (profNoLights || !anyVecLane) ? 0 : tl.count;
			__m256i lane_mirror_v = _mm256_load_si256((const __m256i*)lane_mirrorId);
			for (int n = 0; n < omniLoopN; ++n) {
				__m256 wx = _mm256_sub_ps(_mm256_set1_ps(tl.posX[n]), xv);
				__m256 wy = _mm256_sub_ps(_mm256_set1_ps(tl.posY[n]), yv);
				__m256 wz = _mm256_sub_ps(_mm256_set1_ps(tl.posZ[n]), zv);
				__m256 dot = _mm256_fmadd_ps(wx, nx,
				              _mm256_fmadd_ps(wy, ny,
				               _mm256_mul_ps(wz, nz)));
				__m256 len2 = _mm256_fmadd_ps(wx, wx,
				               _mm256_fmadd_ps(wy, wy,
				                _mm256_mul_ps(wz, wz)));
				__m256 mask_dot   = _mm256_cmp_ps(dot,  _mm256_setzero_ps(), _CMP_GE_OQ);
				__m256 mask_range = _mm256_cmp_ps(len2, _mm256_set1_ps(tl.range2[n]), _CMP_LE_OQ);
				__m256 mask_pos   = _mm256_cmp_ps(len2, _mm256_setzero_ps(), _CMP_GT_OQ);
				// Per-lane mirror filter: light's mirrorId must equal
				// the pixel's. tl.mirrorId[n] is the light's id; lane_
				// mirror_v holds the 8 lanes' pixel ids.
				__m256 mirrorMask = _mm256_castsi256_ps(
					_mm256_cmpeq_epi32(lane_mirror_v,
					                    _mm256_set1_epi32((int)tl.mirrorId[n])));
				__m256 omni_lane  = _mm256_and_ps(_mm256_and_ps(mask_dot, mask_range),
				                                   _mm256_and_ps(_mm256_and_ps(mask_pos, omniMaskF),
				                                                  mirrorMask));
				// safe_len2: 1.0 when masked off
				__m256 safe_len2 = _mm256_blendv_ps(_mm256_set1_ps(1.0f), len2, omni_lane);
				__m256 lenInv = _mm256_rsqrt_ps(safe_len2);
				__m256 dist   = _mm256_mul_ps(safe_len2, lenInv);
				__m256 falloff = _mm256_sub_ps(_mm256_set1_ps(1.0f),
				                                _mm256_mul_ps(dist, _mm256_set1_ps(tl.rRange[n])));
				__m256 k = _mm256_mul_ps(_mm256_mul_ps(dot, lenInv), falloff);

				// Spot cone attenuation (only fires when isSpot[n] is set).
				// Matches the standard kernel exactly so omni pixels under
				// greets's robot spotlight get the same cone falloff.
				if (tl.isSpot[n]) {
					__m256 ldx = _mm256_set1_ps(tl.dirX[n]);
					__m256 ldy = _mm256_set1_ps(tl.dirY[n]);
					__m256 ldz = _mm256_set1_ps(tl.dirZ[n]);
					__m256 lci = _mm256_set1_ps(tl.cosInner[n]);
					__m256 lco = _mm256_set1_ps(tl.cosOuter[n]);
					__m256 dirDotW = _mm256_fmadd_ps(ldx, wx,
					                  _mm256_fmadd_ps(ldy, wy,
					                   _mm256_mul_ps(ldz, wz)));
					__m256 cosTheta = _mm256_mul_ps(_mm256_sub_ps(_mm256_setzero_ps(), dirDotW),
					                                 lenInv);
					__m256 maskInside = _mm256_cmp_ps(cosTheta, lco, _CMP_GT_OQ);
					__m256 rangeRcp = _mm256_div_ps(_mm256_set1_ps(1.0f),
					                                 _mm256_sub_ps(lci, lco));
					__m256 t = _mm256_mul_ps(_mm256_sub_ps(cosTheta, lco), rangeRcp);
					t = _mm256_min_ps(_mm256_max_ps(t, _mm256_setzero_ps()),
					                  _mm256_set1_ps(1.0f));
					__m256 smooth = _mm256_mul_ps(_mm256_mul_ps(t, t),
					                  _mm256_sub_ps(_mm256_set1_ps(3.0f),
					                                _mm256_mul_ps(_mm256_set1_ps(2.0f), t)));
					__m256 coneAtten = _mm256_blendv_ps(_mm256_setzero_ps(), smooth, maskInside);
					k = _mm256_mul_ps(k, coneAtten);
				}

				__m256 intensity = _mm256_blendv_ps(_mm256_setzero_ps(),
				                                     _mm256_mul_ps(k, vDiff),
				                                     omni_lane);
				lB = _mm256_fmadd_ps(intensity, _mm256_set1_ps(tl.colB[n]), lB);
				lG = _mm256_fmadd_ps(intensity, _mm256_set1_ps(tl.colG[n]), lG);
				lR = _mm256_fmadd_ps(intensity, _mm256_set1_ps(tl.colR[n]), lR);
			}

			// Saturate to 250
			__m256 sat = _mm256_set1_ps(250.0f);
			__m256 zero = _mm256_setzero_ps();
			lB = _mm256_max_ps(zero, _mm256_min_ps(lB, sat));
			lG = _mm256_max_ps(zero, _mm256_min_ps(lG, sat));
			lR = _mm256_max_ps(zero, _mm256_min_ps(lR, sat));

			// Fog moved to Render_DeferredFogPass (post-lighting).

			// pixel = texel * lit / 256
			__m256 inv256 = _mm256_set1_ps(1.0f / 256.0f);
			__m256 fdB = _mm256_mul_ps(_mm256_mul_ps(texB, lB), inv256);
			__m256 fdG = _mm256_mul_ps(_mm256_mul_ps(texG, lG), inv256);
			__m256 fdR = _mm256_mul_ps(_mm256_mul_ps(texR, lR), inv256);

			// Spec / water blend / pow are not handled in vec — for any
			// alive lane that needs spec or water-mat, fall back to
			// scalar. Detect:
			//   needsScalar = wantSpec_lane | isWater_lane
			alignas(32) uint32_t lane_needs_scalar[8];
			__m256i needsScalar = _mm256_or_si256(
				_mm256_load_si256((const __m256i*)lane_wantSpec),
				_mm256_load_si256((const __m256i*)lane_isWater));
			_mm256_store_si256((__m256i*)lane_needs_scalar, needsScalar);

			alignas(32) float vfB[8], vfG[8], vfR[8];
			_mm256_store_ps(vfB, fdB);
			_mm256_store_ps(vfG, fdG);
			_mm256_store_ps(vfR, fdR);

			alignas(32) int32_t lane_alive_now[8];
			_mm256_store_si256((__m256i*)lane_alive_now, mask_alive_fresh);

			// Pack lanes into pixel dwords; for spec/water lanes, redo
			// scalar shading (rare on most City pixels).
			alignas(32) float ax[8], ay[8], az_lane[8];
			_mm256_store_ps(ax, xv);
			_mm256_store_ps(ay, yv);
			_mm256_store_ps(az_lane, zv);
			alignas(32) float anx[8], any_l[8], anz[8];
			_mm256_store_ps(anx, nx);
			_mm256_store_ps(any_l, ny);
			_mm256_store_ps(anz, nz);

			for (int k = 0; k < 8; ++k) {
				if (!lane_alive_now[k]) continue;
				int outB, outG, outR;
				if (lane_needs_scalar[k]) {
					// Scalar fallback: redo lighting for this pixel
					// including spec / water blend. Reuses tl + cached
					// texel; ambient already in lane_ambB/G/R.
					float lBs = lane_ambB[k];
					float lGs = lane_ambG[k];
					float lRs = lane_ambR[k];
					float sBs = 0, sGs = 0, sRs = 0;
					float xs = ax[k], ys = ay[k], zs = az_lane[k];
					float nxs = anx[k], nys = any_l[k], nzs = anz[k];
					float vxd = 0, vyd = 0, vzd = 0;
					const bool wantSpec = lane_wantSpec[k] != 0;
					if (wantSpec) {
						float vlen2 = xs*xs + ys*ys + zs*zs;
						float vlenInv = fast_rsqrt(vlen2);
						vxd = -xs * vlenInv;
						vyd = -ys * vlenInv;
						vzd = -zs * vlenInv;
					}
					const bool isWater = lane_isWater[k] != 0;
					// Per-lane mirror id: skip lights belonging to a
					// different mirror context than this pixel's.
					const uint32_t pmid_s = gb.mirrorId.empty()
					    ? 0u : uint32_t(gb.mirrorId[i + k]);
					if (!isWater) {
						const float matDiff = lane_diffuse[k];
						const float matSpec = lane_specular[k];
						const float gloss   = lane_gloss[k];
						for (int n = 0; n < tl.count; ++n) {
							if (tl.mirrorId[n] != pmid_s) continue;
							float wxs = tl.posX[n] - xs;
							float wys = tl.posY[n] - ys;
							float wzs = tl.posZ[n] - zs;
							float dots = wxs*nxs + wys*nys + wzs*nzs;
							if (dots < 0.0f) continue;
							float len2s = wxs*wxs + wys*wys + wzs*wzs;
							if (len2s > tl.range2[n] || len2s == 0.0f) continue;
							float lenInvS = fast_rsqrt(len2s);
							float distS   = len2s * lenInvS;
							float ks = dots * lenInvS * (1.0f - distS * tl.rRange[n]);
							// Spot cone (matches std vec body + std scalar fix).
							if (tl.isSpot[n]) {
								float cosTheta = -(tl.dirX[n]*wxs + tl.dirY[n]*wys + tl.dirZ[n]*wzs) * lenInvS;
								if (cosTheta <= tl.cosOuter[n]) continue;
								if (cosTheta < tl.cosInner[n]) {
									float ct = (cosTheta - tl.cosOuter[n]) / (tl.cosInner[n] - tl.cosOuter[n]);
									ks *= ct * ct * (3.0f - 2.0f * ct);
								}
							}
							float ints = ks * matDiff;
							lBs += ints * tl.colB[n];
							lGs += ints * tl.colG[n];
							lRs += ints * tl.colR[n];
							if (wantSpec) {
								float ldx = wxs * lenInvS, ldy = wys * lenInvS, ldz = wzs * lenInvS;
								float hx = ldx + vxd, hy = ldy + vyd, hz = ldz + vzd;
								float hLen2 = hx*hx + hy*hy + hz*hz;
								if (hLen2 <= 0.0f) continue;
								float hInv = fast_rsqrt(hLen2);
								hx *= hInv; hy *= hInv; hz *= hInv;
								float NdotH = nxs*hx + nys*hy + nzs*hz;
								if (NdotH <= 0.0f) continue;
								float spec = std::pow(NdotH, gloss);
								float ss = spec * matSpec * (1.0f - distS * tl.rRange[n]);
								sBs += ss * tl.colB[n];
								sGs += ss * tl.colG[n];
								sRs += ss * tl.colR[n];
							}
						}
					}
					if (lBs > 250.0f) lBs = 250.0f; if (lBs < 0) lBs = 0;
					if (lGs > 250.0f) lGs = 250.0f; if (lGs < 0) lGs = 0;
					if (lRs > 250.0f) lRs = 250.0f; if (lRs < 0) lRs = 0;
					float fdBs = lane_texB[k] * lBs * (1.0f / 256.0f);
					float fdGs = lane_texG[k] * lGs * (1.0f / 256.0f);
					float fdRs = lane_texR[k] * lRs * (1.0f / 256.0f);
					outB = int(fdBs) + int(sBs);
					outG = int(fdGs) + int(sGs);
					outR = int(fdRs) + int(sRs);
					if (isWater) {
						const dword existing = out[i + k];
						outB += int(existing & 0xFF) >> 1;
						outG += int((existing >> 8) & 0xFF) >> 1;
						outR += int((existing >> 16) & 0xFF) >> 1;
					}
				} else {
					outB = int(vfB[k]);
					outG = int(vfG[k]);
					outR = int(vfR[k]);
				}
				if (outB > 255) outB = 255; if (outB < 0) outB = 0;
				if (outG > 255) outG = 255; if (outG < 0) outG = 0;
				if (outR > 255) outR = 255; if (outR < 0) outR = 0;
				out[i + k] = dword(outB) | (dword(outG) << 8) | (dword(outR) << 16) | 0xFF000000u;
			}
		}

		// Tail: scalar for remaining 1-7 pixels
		for (; px < x2; ++px) {
			// Lazy: just dispatch back to the per-tile inner-loop body.
			// For simplicity we use a no-op since tile widths divide
			// evenly by 8 in our typical 1920×1080 / 12×8 grid.
			// (160-wide tiles → no tail.) If tile widths ever stop
			// being multiples of 8 this needs a real scalar fallback.
			(void)px;
			break;
		}
	}

	// One permit per completed tile (see renderns::tileDone in RENDER.CPP).
	renderns::tileDone.release();
}

// Race-free because wave-2 reads only what wave-1 wrote; tile-job
// dispatch waits on tileCounter between waves. Tile-edge odd pixels
// (px==x1 or px==x2-1) read the next/prev tile's shaded result, which
// is safe because that neighbor tile finished its wave-1 already.
static void Render_DeferredLighting_TileFill(const DeferredLightingCtx &ctx,
                                              int tileIndex,
                                              int x1, int y1, int x2, int y2)
{
	// Addressing from ctx, not globals (RenderContext migration).
	const int xres = ctx.xres, yres = ctx.yres;
	byte *const vpage = ctx.vpage;
	word *const zpage16 = ctx.zpage16;
	const float cntrEX = ctx.cntrEX, cntrEY = ctx.cntrEY;
	const meka::GBuffer &gb = *ctx.gb;
	dword *out = reinterpret_cast<dword *>(vpage);
	const bool specGlobalOn = Specular_Factor > 0.0f;
	const bool  roughMapOnG    = fds::FeatureFlags::roughness_map();   // see main kernel
	const float roughStrengthG = fds::FeatureFlags::roughness_strength();
	const bool quarter      = deferredLightingQuarterEnabled();
	const bool checker      = deferredLightingCheckerboardEnabled() && !quarter;
	const bool hdrWrite     = fds::FeatureFlags::hdr() && fds::Hdr_WritableFor(ctx.xres, ctx.yres);   // HDR B1: see main kernel
	const bool hdrLinear    = hdrWrite && fds::FeatureFlags::hdr_linear();  // HDR B2
	// Normal-similarity threshold for the quarter fill predicate. matID
	// equality alone is too loose — same hull material on a curved
	// surface gives wildly different shading at adjacent pixels, and
	// the average produces the "cartoonish" robot look. Require neighbor
	// normals to be within ~18° (cos > 0.95 default) before averaging.
	// Center normal decoded once per pixel and reused.
	// Wave-2 fill predicate flags. Used by BOTH quarter and checkerboard
	// paths (same adaptive partial averaging shape). Flag name kept as
	// `quarter_normal_cos` for back-compat.
	const float quarterNormalCos = (quarter || checker)
	    ? fds::FeatureFlags::quarter_normal_cos() : 0.0f;
	const bool  quarterNormalCheck = (quarter || checker) && quarterNormalCos > 0.0f;
	// Z-discontinuity threshold. Catches silhouettes / creases that
	// share matID + normal but have an actual depth step (e.g. a hull
	// panel meeting another panel at a sharp angle: same matID, same
	// material normal-map base normal, but the *geometric* surfaces
	// are angled — and at distance even small angle gives a measurable
	// per-pixel Z jump). Without this, those edges blur in quarter.
	const float quarterZJump  = (quarter || checker)
	    ? fds::FeatureFlags::quarter_z_jump() : 0.0f;
	const bool  quarterZCheck = (quarter || checker) && quarterZJump > 0.0f;

	for (int py = y1; py < y2; ++py) {
		for (int px = x1; px < x2; ++px) {
			if (quarter) {
				if (((px | py) & 1) == 0) continue;  // wave-1 shaded
			} else {
				if (((px ^ py) & 1) == 0) continue;
			}

			const size_t i = size_t(py) * xres + px;
			const word zEnc = zpage16[i];
			if (zEnc == 0) continue;

			// Per-pixel mirror id — used by the wave-2 fallback shading
			// to filter lights matching the pixel's mirror context.
			const uint32_t pmid = gb.mirrorId.empty()
			    ? 0u : uint32_t(gb.mirrorId[i]);

			const uint32_t mat32  = gb.txtr[i];
			// Forward content (reflective env disco ball, additive) writes Z
			// but a sentinel matID -- wave-1 skips it, so it stays uncovered and
			// is lifted from vpage in HDR. The fill must skip it too, else it
			// shades the sentinel as garbage into g_hdrBuf at the wave-2 cells
			// (deferred-quarter checker over the forward surface, HDR only; in
			// LDR the forward filler overwrites vpage so it is invisible -> gate
			// on hdrWrite to keep the LDR fill byte-exact).
			if (hdrWrite && (mat32 == 0xFFFFFFFFu || mat32 == 0xFFFFFFFEu)) continue;  // both forward sentinels
			const uint32_t matIDc = (mat32 >> 20) & 0xFF;

			// Center normal decoded once; reused by every fill pattern's
			// neighbor-similarity test below. Cheap enough vs the avoided
			// full shading that we always decode (even if matID fails).
			float ncX = 0, ncY = 0, ncZ = 0;
			if (quarterNormalCheck) {
				meka::oct_decode_u16(gb.normal[i], ncX, ncY, ncZ);
			}
			auto neighborNormalOk = [&](size_t ni) -> bool {
				if (!quarterNormalCheck) return true;
				float nx, ny, nz;
				meka::oct_decode_u16(gb.normal[ni], nx, ny, nz);
				return (ncX*nx + ncY*ny + ncZ*nz) >= quarterNormalCos;
			};
			// Z-discontinuity check: relative depth diff vs center.
			// Center zEnc loaded above (`zEnc`). Compared against neighbor
			// zEnc via a single signed compare and bound. Zero neighbor
			// zEnc (= sky/empty) always fails — averaging in sky pixels
			// would smear edges.
			auto neighborZOk = [&](size_t ni) -> bool {
				if (!quarterZCheck) return true;
				const word zN = zpage16[ni];
				if (zN == 0) return false;
				const int diff = int(zN) - int(zEnc);
				const int absDiff = diff < 0 ? -diff : diff;
				return float(absDiff) <= quarterZJump * float(zEnc);
			};
			// Combined predicate: same matID + normal-similar + Z-similar.
			// Adaptive partial averaging uses this to pick which neighbors
			// to blend; unmatched neighbors are dropped from the average
			// (instead of falling back to full shading for the whole
			// pixel). Eliminates the per-face-edge outline artifact —
			// previously when 1 of 4 corners failed the cos check, the
			// whole pixel switched to full shading and looked visibly
			// different from surrounding interpolated pixels.
			auto neighborCompatible = [&](size_t ni, uint32_t matIDc_) -> bool {
				const uint32_t mID = (gb.txtr[ni] >> 20) & 0xFF;
				if (mID != matIDc_) return false;
				if (!neighborNormalOk(ni)) return false;
				if (!neighborZOk(ni)) return false;
				return true;
			};

			// C (texture/lighting decouple): fetch a pixel's own point-sampled
			// texel (B,G,R). Lets the fill reconstruct each neighbour's LIGHTING
			// (final ÷ own texel) and re-apply THIS pixel's own texel — so the
			// far floor keeps its texture detail instead of averaging into mush.
			// Returns false for untextured/missing (caller falls back to the
			// plain colour average). texel fetch is ~free (measured).
			auto fetchTexel = [&](size_t j, float &tb, float &tg, float &tr) -> bool {
				const uint32_t m32 = gb.txtr[j];
				const uint32_t mid = (m32 >> 20) & 0xFF;
				if (mid >= ctx.matTable.count) return false;
				const Material *M = ctx.matTable.data[mid];
				if (!M || !M->Txtr) return false;
				const dword *td = (const dword *)M->Txtr->Mipmap[(m32 >> 28) & 0xF];
				if (!td) return false;
				const dword t = td[m32 & 0xFFFFF];
				tb = float(t & 0xFF); tg = float((t >> 8) & 0xFF); tr = float((t >> 16) & 0xFF);
				return true;
			};
			const bool sTexSharp = fds::FeatureFlags::quarter_tex_sharp();

			bool matched = false;
			if (quarter) {
				// Adaptive partial averaging: per-neighbor compatibility
				// test (matID + normal + Z), then average ONLY the passing
				// neighbors. Avoids the per-face-edge outline that the
				// all-or-nothing fallback produces: when 1 of 4 corners
				// (or 1 of 2 sides) fails the test, that pixel previously
				// switched to full shading and looked visibly different
				// from surrounding averaged pixels — that visible step IS
				// the outline. Partial averaging blends smoothly instead.
				//
				// Per-channel sum + division by N. N is 0..4; division by
				// 3 isn't bit-shiftable but at most 25% of pixels hit it
				// and the rest use shift fast paths. Still cheaper by ~5×
				// than the full-shading fallback per pixel.
				const bool odd_x = px & 1;
				const bool odd_y = py & 1;
				size_t nidx[4];
				int    nc = 0;
				if (odd_x && !odd_y) {
					if (px > 0)        nidx[nc++] = i - 1;
					if (px < xres - 1) nidx[nc++] = i + 1;
				} else if (!odd_x && odd_y) {
					if (py > 0)        nidx[nc++] = i - xres;
					if (py < yres - 1) nidx[nc++] = i + xres;
				} else {
					if (px > 0 && py > 0)               nidx[nc++] = i - xres - 1;
					if (px < xres - 1 && py > 0)        nidx[nc++] = i - xres + 1;
					if (px > 0 && py < yres - 1)        nidx[nc++] = i + xres - 1;
					if (px < xres - 1 && py < yres - 1) nidx[nc++] = i + xres + 1;
				}
				int sumR = 0, sumG = 0, sumB = 0;
				float hsB = 0, hsG = 0, hsR = 0;   // HDR: parallel float-radiance average
				float ownB=0, ownG=0, ownR=0;
				const bool haveOwn = sTexSharp && fetchTexel(i, ownB, ownG, ownR);
				float slB=0, slG=0, slR=0;                 // LDR reconstructed lighting
				float ahB=0, ahG=0, ahR=0; int nsharp=0;   // HDR reconstructed radiance
				int n = 0;
				for (int k = 0; k < nc; ++k) {
					if (!neighborCompatible(nidx[k], matIDc)) continue;
					const dword p = out[nidx[k]];
					sumB += int(p & 0xFF);
					sumG += int((p >> 8) & 0xFF);
					sumR += int((p >> 16) & 0xFF);
					const fds::hdrf* nh = hdrWrite ? (fds::g_hdrBuf.data() + nidx[k]*4) : nullptr;
					if (nh) { hsB += nh[0]; hsG += nh[1]; hsR += nh[2]; }
					if (haveOwn) {
						float nb, ng, nr;
						// Trust region: the divide-out model (R ∝ texel^exp × smooth
						// lighting) only holds where the neighbour's texel is bright
						// enough to be a reliable lighting probe. When own ≫ neighbour
						// (an LED dot over its panel's black background: 255/1, then
						// squared to ×65025 under hdr_linear) the division amplifies
						// whatever non-texture-modulated radiance (spec, ambient floor)
						// the dark texel carries and blows the pixel out far past the
						// full-rate result. Such a neighbour can't contribute to the
						// sharp reconstruction; it still counts in the plain radiance
						// average, which handles the pixel when no neighbour qualifies.
						float nrB, nrG, nrR;
						if (fetchTexel(nidx[k], nb, ng, nr) &&
						    (nrB = ownB / std::max(nb, 1.0f)) <= 4.0f &&
						    (nrG = ownG / std::max(ng, 1.0f)) <= 4.0f &&
						    (nrR = ownR / std::max(nr, 1.0f)) <= 4.0f) {
							slB += float(p & 0xFF)        * 256.0f / std::max(nb, 1.0f);
							slG += float((p >> 8) & 0xFF)  * 256.0f / std::max(ng, 1.0f);
							slR += float((p >> 16) & 0xFF) * 256.0f / std::max(nr, 1.0f);
							if (nh) {
								// radiance ∝ texel^exp (2 = hdr_linear albedo², 1 = gamma);
								// re-apply own texel: R_i = R_n·(texel_i/texel_n)^exp.
								float rB=nrB, rG=nrG, rR=nrR;
								if (hdrLinear) { rB*=rB; rG*=rG; rR*=rR; }
								ahB += nh[0]*rB; ahG += nh[1]*rG; ahR += nh[2]*rR;
							}
							++nsharp;
						}
					}
					++n;
				}
				if (n > 0) {
					if (haveOwn && nsharp > 0 && !hdrWrite) {
						const float inv = 1.0f / (float(nsharp) * 256.0f);
						int oB = int(ownB * slB * inv + 0.5f); if (oB > 255) oB = 255;
						int oG = int(ownG * slG * inv + 0.5f); if (oG > 255) oG = 255;
						int oR = int(ownR * slR * inv + 0.5f); if (oR > 255) oR = 255;
						out[i] = dword(oB) | (dword(oG) << 8) | (dword(oR) << 16) | 0xFF000000u;
					} else {
						int aR, aG, aB;
						if (n == 1)      { aB = sumB;       aG = sumG;       aR = sumR;       }
						else if (n == 2) { aB = sumB >> 1;  aG = sumG >> 1;  aR = sumR >> 1;  }
						else if (n == 4) { aB = sumB >> 2;  aG = sumG >> 2;  aR = sumR >> 2;  }
						else /* n == 3 */ { aB = sumB / 3;  aG = sumG / 3;   aR = sumR / 3;   }
						out[i] = dword(aB) | (dword(aG) << 8) | (dword(aR) << 16) | 0xFF000000u;
					}
					if (hdrWrite) {
						fds::hdrf* h = fds::g_hdrBuf.data() + i*4;
						if (haveOwn && nsharp > 0) {
							const float invn = 1.0f / float(nsharp);
							h[0] = fds::HdrClamp(ahB*invn); h[1] = fds::HdrClamp(ahG*invn); h[2] = fds::HdrClamp(ahR*invn);
						} else {
							const float inv = 1.0f / float(n);
							h[0] = fds::HdrClamp(hsB*inv); h[1] = fds::HdrClamp(hsG*inv); h[2] = fds::HdrClamp(hsR*inv);
						}
						h[3] = 1.0f;
					}
					matched = true;
				}
			} else {
				// Checkerboard: same adaptive partial averaging shape as
				// quarter's horizontal pattern. Neighbors are L and R; each
				// individually tested for matID + normal + Z compatibility.
				// Average passers only; fall back to full shading when both
				// fail. Same fix as quarter for face-edge outlines.
				size_t nidx[2];
				int    nc = 0;
				if (px > 0)        nidx[nc++] = i - 1;
				if (px < xres - 1) nidx[nc++] = i + 1;
				if (nc == 0) continue;
				int sumR = 0, sumG = 0, sumB = 0;
				float hsB = 0, hsG = 0, hsR = 0;   // HDR: parallel float-radiance average
				// C (texture/lighting decouple) — same divide-out + trust region
				// as the quarter path above; without it the checker fill plain-
				// averages neighbour colours and mushes far-floor texture detail
				// (measured |checker-full| 1.32 vs quarter+C 0.31 on greets).
				float ownB=0, ownG=0, ownR=0;
				const bool haveOwn = sTexSharp && fetchTexel(i, ownB, ownG, ownR);
				float slB=0, slG=0, slR=0;                 // LDR reconstructed lighting
				float ahB=0, ahG=0, ahR=0; int nsharp=0;   // HDR reconstructed radiance
				int n = 0;
				for (int k = 0; k < nc; ++k) {
					if (!neighborCompatible(nidx[k], matIDc)) continue;
					const dword p = out[nidx[k]];
					sumB += int(p & 0xFF);
					sumG += int((p >> 8) & 0xFF);
					sumR += int((p >> 16) & 0xFF);
					const fds::hdrf* nh = hdrWrite ? (fds::g_hdrBuf.data() + nidx[k]*4) : nullptr;
					if (nh) { hsB += nh[0]; hsG += nh[1]; hsR += nh[2]; }
					if (haveOwn) {
						float nb, ng, nr, nrB, nrG, nrR;
						if (fetchTexel(nidx[k], nb, ng, nr) &&
						    (nrB = ownB / std::max(nb, 1.0f)) <= 4.0f &&
						    (nrG = ownG / std::max(ng, 1.0f)) <= 4.0f &&
						    (nrR = ownR / std::max(nr, 1.0f)) <= 4.0f) {
							slB += float(p & 0xFF)        * 256.0f / std::max(nb, 1.0f);
							slG += float((p >> 8) & 0xFF)  * 256.0f / std::max(ng, 1.0f);
							slR += float((p >> 16) & 0xFF) * 256.0f / std::max(nr, 1.0f);
							if (nh) {
								// radiance ∝ texel^exp; re-apply own texel (see quarter path)
								float rB=nrB, rG=nrG, rR=nrR;
								if (hdrLinear) { rB*=rB; rG*=rG; rR*=rR; }
								ahB += nh[0]*rB; ahG += nh[1]*rG; ahR += nh[2]*rR;
							}
							++nsharp;
						}
					}
					++n;
				}
				if (n > 0) {
					if (haveOwn && nsharp > 0 && !hdrWrite) {
						const float inv = 1.0f / (float(nsharp) * 256.0f);
						int oB = int(ownB * slB * inv + 0.5f); if (oB > 255) oB = 255;
						int oG = int(ownG * slG * inv + 0.5f); if (oG > 255) oG = 255;
						int oR = int(ownR * slR * inv + 0.5f); if (oR > 255) oR = 255;
						out[i] = dword(oB) | (dword(oG) << 8) | (dword(oR) << 16) | 0xFF000000u;
					} else {
						int aR, aG, aB;
						if (n == 1)      { aB = sumB;       aG = sumG;       aR = sumR;       }
						else /* n == 2 */ { aB = sumB >> 1; aG = sumG >> 1;  aR = sumR >> 1; }
						out[i] = dword(aB) | (dword(aG) << 8) | (dword(aR) << 16) | 0xFF000000u;
					}
					if (hdrWrite) {   // HDR float-radiance average (see quarter path)
						fds::hdrf* h = fds::g_hdrBuf.data() + i*4;
						if (haveOwn && nsharp > 0) {
							const float invn = 1.0f / float(nsharp);
							h[0] = fds::HdrClamp(ahB*invn); h[1] = fds::HdrClamp(ahG*invn); h[2] = fds::HdrClamp(ahR*invn);
						} else {
							const float inv = 1.0f / float(n);
							h[0] = fds::HdrClamp(hsB*inv); h[1] = fds::HdrClamp(hsG*inv); h[2] = fds::HdrClamp(hsR*inv);
						}
						h[3] = 1.0f;
					}
					matched = true;
				}
			}
			if (matched) continue;

			// Fallback path — replays the wave-1 kernel for this pixel.
			// We don't expect this branch to be hot (most frame area is
			// continuous surface), so the simpler scalar-only fallback
			// keeps the code small even if we're in vec mode otherwise.
			const uint32_t miplevel    = (mat32 >> 28) & 0xF;
			const uint32_t matID       = matIDc;
			const uint32_t swizzledUV  = mat32 & 0xFFFFF;
			if (matID >= ctx.matTable.count) continue;
			Material *Mat = ctx.matTable.data[matID];
			if (!Mat || !Mat->Txtr) continue;
			const dword *texData = (const dword *)Mat->Txtr->Mipmap[miplevel];
			if (!texData) continue;
			const dword texel = texData[swizzledUV];
			const float texB = float(texel & 0xFF);
			const float texG = float((texel >> 8) & 0xFF);
			const float texR = float((texel >> 16) & 0xFF);

			float nx, ny, nz;
			meka::oct_decode_u16(gb.normal[i], nx, ny, nz);

			// Normal map sampling — mirrors the wave-1 kernel exactly so
			// nmap materials that fall through the surface-similar test
			// produce the same result as if they'd been shaded in wave-1.
			// Without this the fallback's geometric N differed from
			// surrounding interpolated pixels and broke quarter mode
			// silently. Tier B per-vertex tangent + Mikkelsen fallback.
			if (Mat->NormalMap) {
				float nmX, nmY, nmZ;
				if (decodeNormalTexel(Mat->NormalMap, miplevel, swizzledUV, nmX, nmY, nmZ)) {
					float tx = 0, ty = 0, tz = 0;
					bool tangentValid = false;
					if (!gb.tangent.empty()) {
						const meka::u16 packedT = gb.tangent[i];
						if (packedT != 0) {
							meka::oct_decode_u16(packedT, tx, ty, tz);
							const float tDotN = tx*nx + ty*ny + tz*nz;
							tx -= nx * tDotN; ty -= ny * tDotN; tz -= nz * tDotN;
							const float tLen2 = tx*tx + ty*ty + tz*tz;
							if (tLen2 > 1e-12f) {
								const float invTLen = fast_rsqrt(tLen2);
								tx *= invTLen; ty *= invTLen; tz *= invTLen;
								tangentValid = true;
							}
						}
					}
					if (!tangentValid) {
						float refx, refy, refz;
						if (std::fabs(ny) < 0.9f) { refx = 0; refy = 1; refz = 0; }
						else                       { refx = 1; refy = 0; refz = 0; }
						tx = refy * nz - refz * ny;
						ty = refz * nx - refx * nz;
						tz = refx * ny - refy * nx;
						const float invTLen = fast_rsqrt(tx*tx + ty*ty + tz*tz);
						tx *= invTLen; ty *= invTLen; tz *= invTLen;
					}
					const float hsign = Mat->TbnHandedness;  // mirrored-UV bitangent flip
					const float bx = (ny * tz - nz * ty) * hsign;
					const float by = (nz * tx - nx * tz) * hsign;
					const float bz = (nx * ty - ny * tx) * hsign;
					float vnx = tx * nmX + bx * nmY + nx * nmZ;
					float vny = ty * nmX + by * nmY + ny * nmZ;
					float vnz = tz * nmX + bz * nmY + nz * nmZ;
					float invLen = fast_rsqrt(vnx*vnx + vny*vny + vnz*vnz);
					nx = vnx * invLen;
					ny = vny * invLen;
					nz = vnz * invLen;
				}
			}

			const float z = float(0xFF80 - zEnc) * ctx.invZScale;
			const float x = (float(px) - cntrEX) * z * ctx.invFOVX;
			const float y = (cntrEY - float(py)) * z * ctx.invFOVY;

			float lB, lG, lR;
			if (Mat->Txtr) {
				lB = Mat->Luminosity * 255.0f + Mat->Diffuse * ctx.Sc->Ambient.B;
				lG = Mat->Luminosity * 255.0f + Mat->Diffuse * ctx.Sc->Ambient.G;
				lR = Mat->Luminosity * 255.0f + Mat->Diffuse * ctx.Sc->Ambient.R;
			} else {
				lB = Mat->Luminosity * Mat->BaseCol.B + Mat->Diffuse * ctx.Sc->Ambient.B;
				lG = Mat->Luminosity * Mat->BaseCol.G + Mat->Diffuse * ctx.Sc->Ambient.G;
				lR = Mat->Luminosity * Mat->BaseCol.R + Mat->Diffuse * ctx.Sc->Ambient.R;
			}

			const bool isWater = (int(matID) == ctx.waterMatID);
			const TileLights &tl = ctx.tileLights[tileIndex];

			float sB = 0, sG = 0, sR = 0;
			float vx = 0, vy = 0, vz = 0;
			const bool wantSpecular = specGlobalOn && (Mat->Specular > 0.0f);
			const float gloss = Mat->Glossiness > 0 ? float(Mat->Glossiness) : 32.0f;
			if (wantSpecular) {
				const float vlen2 = x*x + y*y + z*z;
				const float vlenInv = fast_rsqrt(vlen2);
				vx = -x * vlenInv;
				vy = -y * vlenInv;
				vz = -z * vlenInv;
			}

			for (int n = 0; !isWater && n < tl.count; ++n) {
				if (tl.mirrorId[n] != pmid) continue;
				const float wx = tl.posX[n] - x;
				const float wy = tl.posY[n] - y;
				const float wz = tl.posZ[n] - z;
				const float dot = wx*nx + wy*ny + wz*nz;
				if (dot < 0.0f) continue;
				const float len2 = wx*wx + wy*wy + wz*wz;
								if (len2 > tl.range2[n]) continue;
					// Mirror-bounce window portal (see scalar main kernel). World
					// pos computed lazily — bounce lights are rare and this is the
					// sub-rate fill path.
					if (tl.winMinX[n] <= tl.winMaxX[n]) {
						const float Pwx = ctx.viewToWorld[0][0]*x + ctx.viewToWorld[0][1]*y + ctx.viewToWorld[0][2]*z + ctx.cameraWorldX;
						const float Pwy = ctx.viewToWorld[1][0]*x + ctx.viewToWorld[1][1]*y + ctx.viewToWorld[1][2]*z + ctx.cameraWorldY;
						const float Pwz = ctx.viewToWorld[2][0]*x + ctx.viewToWorld[2][1]*y + ctx.viewToWorld[2][2]*z + ctx.cameraWorldZ;
						if (bouncePortalReject(tl, n, Pwx, Pwy, Pwz)) continue;
					}	const float lenInv = fast_rsqrt(len2);
				const float dist   = len2 * lenInv;
				float k = dot * lenInv * (1.0f - dist * tl.rRange[n]);
				// Spot cone gate. coneAtten in [0,1] modulates BOTH diffuse
				// and specular contributions so they stay coherent — the
				// spot just darkens the light past the inner cone, not
				// just its diffuse channel.
				float coneAtten = 1.0f;
				if (tl.isSpot[n]) {
					const float cosTheta = -(tl.dirX[n]*wx + tl.dirY[n]*wy + tl.dirZ[n]*wz) * lenInv;
					if (cosTheta <= tl.cosOuter[n]) continue;
					if (cosTheta < tl.cosInner[n]) {
						float t = (cosTheta - tl.cosOuter[n]) / (tl.cosInner[n] - tl.cosOuter[n]);
						coneAtten = t * t * (3.0f - 2.0f * t);
						k *= coneAtten;
					}
				}
				const float intensity = k * Mat->Diffuse;
				lB += intensity * tl.colB[n];
				lG += intensity * tl.colG[n];
				lR += intensity * tl.colR[n];

				if (wantSpecular) {
					const float ldx = wx * lenInv;
					const float ldy = wy * lenInv;
					const float ldz = wz * lenInv;
					float hx = ldx + vx, hy = ldy + vy, hz = ldz + vz;
					const float hLen2 = hx*hx + hy*hy + hz*hz;
					if (hLen2 > 0.0f) {
						const float hLenInv = fast_rsqrt(hLen2);
						hx *= hLenInv; hy *= hLenInv; hz *= hLenInv;
						const float NdotH = nx*hx + ny*hy + nz*hz;
						if (NdotH > 0.0f) {
							const float spec = std::pow(NdotH, gloss);
							const float specStrength = spec * Mat->Specular * (1.0f - dist * tl.rRange[n]) * coneAtten;
							sB += specStrength * tl.colB[n];
							sG += specStrength * tl.colG[n];
							sR += specStrength * tl.colR[n];
						}
					}
				}
			}

			if (!hdrWrite) {               // 8-bit rollover guard only (see main kernel)
				if (lB > 250.0f) lB = 250.0f;
				if (lG > 250.0f) lG = 250.0f;
				if (lR > 250.0f) lR = 250.0f;
			}
			if (lB < 0.0f) lB = 0.0f;
			if (lG < 0.0f) lG = 0.0f;
			if (lR < 0.0f) lR = 0.0f;

			// Fog moved to Render_DeferredFogPass (post-lighting).

			float fdB = (texB * lB) * (1.0f / 256.0f);
			float fdG = (texG * lG) * (1.0f / 256.0f);
			float fdR = (texR * lR) * (1.0f / 256.0f);
			// Roughness map (cheap tier): per-pixel specular intensity (see main path).
			if (roughMapOnG && Mat->RoughnessMap && (sB != 0.0f || sG != 0.0f || sR != 0.0f)) {
				const byte *rd = (miplevel < Mat->RoughnessMap->numMipmaps)
					? reinterpret_cast<const byte*>(Mat->RoughnessMap->Mipmap[miplevel]) : nullptr;
				if (rd) {
					float specMul = 1.0f - roughStrengthG * (float(rd[swizzledUV]) * (1.0f/255.0f));
					if (specMul < 0.0f) specMul = 0.0f;
					sB *= specMul; sG *= specMul; sR *= specMul;
				}
			}
			int outB = int(fdB) + int(sB);
			int outG = int(fdG) + int(sG);
			int outR = int(fdR) + int(sR);
			float hB = fdB + sB, hG = fdG + sG, hR = fdR + sR;   // HDR B1 unclamped radiance
			if (isWater) {
				const dword existing = out[i];
				const int rB = int(existing & 0xFF);
				const int rG = int((existing >> 8) & 0xFF);
				const int rR = int((existing >> 16) & 0xFF);
				outB += rB >> 1;
				outG += rG >> 1;
				outR += rR >> 1;
				hB += float(rB) * 0.5f;
				hG += float(rG) * 0.5f;
				hR += float(rR) * 0.5f;
			}
			if (hdrWrite) {
				fds::hdrf* h = fds::g_hdrBuf.data() + i * 4;
				if (hdrLinear) {            // B2 + full coherence — see main kernel
					const float kN = 1.0f / 255.0f;
					const float aB = texB*kN, aG = texG*kN, aR = texR*kN;
					float rlB = aB*aB*lB + sB, rlG = aG*aG*lG + sG, rlR = aR*aR*lR + sR;
					if (isWater) {
						const dword e = out[i];
						const float wB=float(e&0xFF)*kN, wG=float((e>>8)&0xFF)*kN, wR=float((e>>16)&0xFF)*kN;
						rlB += wB*wB*255.0f*0.5f; rlG += wG*wG*255.0f*0.5f; rlR += wR*wR*255.0f*0.5f;
					}
					h[0] = fds::HdrClamp(rlB); h[1] = fds::HdrClamp(rlG); h[2] = fds::HdrClamp(rlR);   // store LINEAR
				} else {
					h[0] = fds::HdrClamp(hB); h[1] = fds::HdrClamp(hG); h[2] = fds::HdrClamp(hR);   // B1 gamma radiance
				}
				h[3] = 1.0f;   // coverage for the composite
			}
			if (outB > 255) outB = 255;
			if (outG > 255) outG = 255;
			if (outR > 255) outR = 255;
			if (outB < 0) outB = 0;
			if (outG < 0) outG = 0;
			if (outR < 0) outR = 0;
			out[i] = dword(outB) | (dword(outG) << 8) | (dword(outR) << 16) | 0xFF000000u;
		}
	}

	// One permit per completed tile (see renderns::tileDone in RENDER.CPP).
	renderns::tileDone.release();
}

// Per-frame setup + dispatch tile jobs across the ThreadPool. Same 6×4
// split the rasterizer uses; tiles are independent (each writes a
// disjoint slice of vpage). Reuses the renderns::tileCounter +
// condition variable that Render() already uses for the rasterizer
// pass — fine because we wait synchronously between Render's tile
// dispatch and our own.
void Render_DeferredLighting(DeferredLightingCtx &ctx, const DeferredOverride *ov) {
	// Override-or-main addressing. ov=nullptr (main frame) → the sanctioned
	// main-target accessors (MainRenderTargetFromGlobals / fds::g_mainCamera —
	// the same state, under names this poisoned TU may use), byte-identical.
	// ov!=nullptr (offscreen shard bake) → ov's own G-buffer / camera /
	// scratch buffers, so N bakes run concurrently on the pool.
	// CALLER CONTRACT: ctx.Sc must be pre-filled with the scene to light
	// (renderFrame and the offscreen bakes set it before calling).
	const fds::RenderTarget mainRt =
		ov ? fds::RenderTarget{} : fds::MainRenderTargetFromGlobals();
	const fds::CameraContext &mainCam = fds::g_mainCamera;
	meka::GBuffer *const gbPtr = (ov && ov->gb) ? ov->gb : mainRt.gbuffer;
	byte *const vpage   = ov ? ov->vpage   : reinterpret_cast<byte*>(mainRt.vpage);
	word *const zpage16 = ov ? ov->zpage16 : mainRt.zpage16;
	if (!gbPtr || !zpage16 || !vpage) return;
	const meka::GBuffer &gb = *gbPtr;
	meka::GBuffer *const gbuf = gbPtr;                 // mirrorMask read below
	Camera  *const view  = (ov && ov->cam) ? ov->cam->view   : mainCam.view;
	const float fovX     = (ov && ov->cam) ? ov->cam->fovX   : mainCam.fovX;
	const float fovY     = (ov && ov->cam) ? ov->cam->fovY   : mainCam.fovY;
	const float cntrEX   = (ov && ov->cam) ? ov->cam->cntrEX : mainCam.cntrEX;
	const float cntrEY   = (ov && ov->cam) ? ov->cam->cntrEY : mainCam.cntrEY;
	const float zscale   = (ov && ov->cam) ? ov->cam->zScale : mainCam.zScale;
	const int32_t xres   = ov ? ov->xres : mainRt.xres;
	const int32_t yres   = ov ? ov->yres : mainRt.yres;
	meka::GBuffer *const gbXpar = ov ? ov->gbXpar   : mainRt.gbufferTransparent;
	word *const xparZ           = ov ? ov->xparZ    : mainRt.xparZ;
	word *const xparZBack       = ov ? ov->xparZBack : mainRt.xparZBack;
	const size_t numPixels = size_t(xres) * size_t(yres);
	if (gb.normal.size() < numPixels || gb.txtr.size() < numPixels) return;

	Scene *Sc = ctx.Sc;
	if (!Sc) return;

	MatTable matTable = Scene_GetMatTable(Sc);
	if (!matTable.data || matTable.count == 0) return;

	if (std::getenv("FDS_DUMP_MATS")) {
		static bool dumped = false;
		if (!dumped) {
			dumped = true;
			for (dword mi = 0; mi < matTable.count; ++mi) {
				Material *M = matTable.data[mi];
				std::fprintf(stderr, "[MAT %u] '%s' Lum=%.2f Diff=%.2f flags=0x%x tex=%s\n",
					mi, M && M->Name ? M->Name : "?",
					M ? M->Luminosity : -1.f, M ? M->Diffuse : -1.f,
					M ? unsigned(M->Flags) : 0u,
					(M && M->Txtr && M->Txtr->FileName) ? M->Txtr->FileName : "-");
			}
		}
	}

	// FDS_DEFERRED_GLOSS_STATS=1: dump distinct Mat->Glossiness values
	// (and material count per value) for materials with Specular > 0 in
	// the current scene. Run once per scene change; lets us confirm the
	// squaring-dispatch switch in the lighting kernel covers everything
	// in use, and warn if an FLD ships a gloss value we don't specialize.
	if (fds::FeatureFlags::deferred_gloss_stats()) {
		static Scene* lastScene = nullptr;
		if (Sc != lastScene) {
			lastScene = Sc;
			std::map<unsigned short, int> glossHisto;
			int specMats = 0;
			for (size_t i = 0; i < matTable.count; ++i) {
				Material *M = matTable.data[i];
				if (!M || M->Specular <= 0.0f) continue;
				++specMats;
				++glossHisto[M->Glossiness];
			}
			std::fprintf(stderr, "[GLOSS-STATS] scene=%p specMats=%d distinct={",
				(void*)Sc, specMats);
			bool first = true;
			for (const auto &kv : glossHisto) {
				std::fprintf(stderr, "%s%u:%d", first ? "" : ",", kv.first, kv.second);
				first = false;
			}
			std::fprintf(stderr, "}\n");
		}
	}

	// Build view-space omni list (per-frame for the main pass; per-target when
	// ov supplies its own scratch, so concurrent offscreen bakes don't race on
	// the function-static).
	static ViewLightsSoA s_lights;  // function-static; lifetime spans dispatch + wait
	ViewLightsSoA &lights = (ov && ov->lights) ? *ov->lights : s_lights;
	// Optional Range clamp — if FDS_DEFERRED_MAX_RANGE is set, any
	// per-omni Range above that ceiling is clamped *for culling
	// purposes only* (the falloff math still uses the real Range, so
	// pixel brightness is barely affected — at the clamp boundary
	// contribution is `1 - clamp/Range_real` ≈ 1, dropping to 0 at the
	// clamp edge). Used to estimate the perf upside of dialing in
	// effective ranges scene-wide.
	const float maxRange = fds::FeatureFlags::deferred_max_range();

	const long long _llist = TailProf::nowNs();
	std::memset(&lights, 0, sizeof(lights));
	int numLights = 0;
	for (Omni *O = Sc->OmniHead; O && numLights < DEFERRED_MAX_VIEW_LIGHTS; O = O->Next) {
		if (!(O->Flags & Omni_Active)) continue;
		Vector u, w;
		Vector_Sub(&O->IPos, &view->ISource, &u);
		MatrixXVector(view->Mat, &u, &w);
		float Range = O->IRange;
		if (maxRange > 0.0f && Range > maxRange) Range = maxRange;
		lights.posX[numLights]   = w.x;
		lights.posY[numLights]   = w.y;
		lights.posZ[numLights]   = w.z;
		lights.posWorldX[numLights] = O->IPos.x;
		lights.posWorldY[numLights] = O->IPos.y;
		lights.posWorldZ[numLights] = O->IPos.z;
		lights.forceCone[numLights] = (O->Flags & Omni_ForceVolCone) ? 1u : 0u;
		{
			const float co = (O->Type == Light_SpotLight) ? O->FallOff : 1.0f;
			const float s2 = 1.0f - co * co;
			lights.sinOuter[numLights] = (s2 > 0.0f) ? std::sqrt(s2) : 0.0f;
		}
		lights.colB[numLights]   = O->L.B * O->ISize;
		lights.colG[numLights]   = O->L.G * O->ISize;
		lights.colR[numLights]   = O->L.R * O->ISize;
		lights.range2[numLights] = Range * Range;
		// Note: rRange uses the *real* range so falloff math is
		// preserved. Only the cull radius shrinks.
		const float realRange = O->IRange;
		lights.rRange[numLights] = (realRange > 0.0f) ? 1.0f / realRange : 0.0f;
		// Spot light: rotate the cone axis (world space) into view space
		// using the same view->Mat that placed posX/Y/Z. Pure rotation,
		// no translation — IDir is a direction not a point.
		if (O->Type == Light_SpotLight) {
			Vector dirView;
			MatrixXVector(view->Mat, (Vector*)&O->IDir, &dirView);
			Vector_Norm(&dirView);
			lights.dirX[numLights] = dirView.x;
			lights.dirY[numLights] = dirView.y;
			lights.dirZ[numLights] = dirView.z;
			lights.cosInner[numLights] = O->HotSpot;
			lights.cosOuter[numLights] = O->FallOff;
			lights.isSpot[numLights] = 1u;
		} else {
			lights.dirX[numLights] = 0.0f;
			lights.dirY[numLights] = 0.0f;
			lights.dirZ[numLights] = 0.0f;
			lights.cosInner[numLights] = -2.0f;
			lights.cosOuter[numLights] = -2.0f;
			lights.isSpot[numLights] = 0u;
		}
		// Map this light to its shadow map (or -1). Only set when
		// shadows are enabled — otherwise the kernel must skip the
		// shadow test (and the shadow maps are stale-empty anyway).
		// Spot path: scan g_shadowMaps for a 2D entry (cubeFace<0).
		// Cube-omni path: scan g_cubeShadowRefs for the matching omni.
		int32_t smIdx     = -1;
		int32_t cubeIdx   = -1;
		if (fds::FeatureFlags::shadows() && (O->Flags & Omni_CastsShadow)) {
			if (O->Type == Light_SpotLight) {
				for (size_t i = 0; i < g_shadowMaps.size(); ++i) {
					if (g_shadowMaps[i].omni == O && g_shadowMaps[i].cubeFace < 0) {
						smIdx = int32_t(i);
						break;
					}
				}
			} else if (O->Type == Light_Omni) {
				for (size_t i = 0; i < g_cubeShadowRefs.size(); ++i) {
					if (g_cubeShadowRefs[i].omni == O) {
						cubeIdx = int32_t(i);
						break;
					}
				}
			}
		}
		lights.shadowMapIdx[numLights]  = smIdx;
		lights.cubeShadowIdx[numLights] = cubeIdx;
		lights.isFlash[numLights]       = (O->Flags & Omni_FogTransient) ? 1u : 0u;
		// Clone OR bounce light with a shadow-casting SPOT source →
		// mirrored shadow sampling against the source's 2-D map. Bounce
		// spots own no map (castsShadow=false); they borrow the source
		// disco spot's reflected map exactly like clones do.
		int32_t srcSm = -1;
		if ((O->Flags & (Omni_MirrorClone | Omni_BounceCone)) && O->mirrorSrcOmni &&
		    fds::FeatureFlags::shadows() &&
		    (O->mirrorSrcOmni->Flags & Omni_CastsShadow) &&
		    O->mirrorSrcOmni->Type == Light_SpotLight) {
			for (size_t i = 0; i < g_shadowMaps.size(); ++i) {
				if (g_shadowMaps[i].omni == O->mirrorSrcOmni &&
				    g_shadowMaps[i].cubeFace < 0) {
					srcSm = int32_t(i);
					break;
				}
			}
		}
		lights.srcShadowMapIdx[numLights] = srcSm;
		// Clone omni whose SOURCE is a CUBE omni: borrow the source's cube and
		// sample it at the receiver reflected across the mirror plane (kernel
		// reflects via mirN/mirD). Cube analogue of srcShadowMapIdx — reflected
		// shadows from the real baked geometry, no clone bake (clones carry
		// Tri_NoShadowCast).
		int32_t srcCube = -1;
		if ((O->Flags & Omni_MirrorClone) && O->mirrorSrcOmni &&
		    fds::FeatureFlags::shadows() &&
		    (O->mirrorSrcOmni->Flags & Omni_CastsShadow) &&
		    O->mirrorSrcOmni->Type == Light_Omni) {
			for (size_t i = 0; i < g_cubeShadowRefs.size(); ++i) {
				if (g_cubeShadowRefs[i].omni == O->mirrorSrcOmni) { srcCube = int32_t(i); break; }
			}
		}
		lights.srcCubeShadowIdx[numLights] = srcCube;
		lights.bounceClamp[numLights] = (O->Flags & Omni_BounceCone) ? 1u : 0u;
		lights.mirNX[numLights] = O->mirrorPlaneN.x;
		lights.mirNY[numLights] = O->mirrorPlaneN.y;
		lights.mirNZ[numLights] = O->mirrorPlaneN.z;
		lights.mirD [numLights] = O->mirrorPlaneD;
		// Window AABB for the surface portal test. Bounce spots carry the
		// real window; every other light gets an INVERTED AABB so the
		// kernel's `winMinX <= winMaxX` gate is false and it skips the test.
		if (O->Flags & Omni_BounceCone) {
			lights.winMinX[numLights] = O->mirrorWinMin.x;
			lights.winMinY[numLights] = O->mirrorWinMin.y;
			lights.winMinZ[numLights] = O->mirrorWinMin.z;
			lights.winMaxX[numLights] = O->mirrorWinMax.x;
			lights.winMaxY[numLights] = O->mirrorWinMax.y;
			lights.winMaxZ[numLights] = O->mirrorWinMax.z;
		} else {
			lights.winMinX[numLights] =  1e30f;
			lights.winMinY[numLights] =  1e30f;
			lights.winMinZ[numLights] =  1e30f;
			lights.winMaxX[numLights] = -1e30f;
			lights.winMaxY[numLights] = -1e30f;
			lights.winMaxZ[numLights] = -1e30f;
		}
		// Per-omni halo controls. 0 → "use legacy default":
		//   HaloIntensity = 0 → 1.0 (multiplier no-op)
		//   HaloRange     = 0 → IRange (same as surface lighting)
		// Range resolution chain (later overrides earlier):
		//   IRange  ->  HaloRange  ->  × omni_halo_range_mult
		//                          OR  omni_halo_force_range (hard set)
		const float haloMul    = (O->HaloIntensity > 0.0f) ? O->HaloIntensity : 1.0f;
		const float forceRange = fds::FeatureFlags::omni_halo_force_range();
		float       haloRange;
		if (forceRange > 0.0f) {
			haloRange = forceRange;
		} else {
			const float baseRange = (O->HaloRange > 0.0f) ? O->HaloRange : O->IRange;
			const float rangeMult = fds::FeatureFlags::omni_halo_range_mult();
			haloRange = baseRange * (rangeMult > 0.0f ? rangeMult : 1.0f);
		}
		lights.haloDensityMul[numLights] = haloMul;
		lights.haloRange     [numLights] = haloRange;
		lights.haloRange2    [numLights] = haloRange * haloRange;
		lights.haloRRange    [numLights] = (haloRange > 0.0f) ? 1.0f / haloRange : 0.0f;
		// Mirror id: 0 for originals, 1..N for clones from GreetsMirror.
		// Surface lighting kernels gate per pixel against gb.mirrorId.
		lights.mirrorId      [numLights] = O->mirrorId;
		++numLights;
	}
	TailProf::mark("light-list", _llist);   // SoA build: per-light xform + linear shadow-map scans

	constexpr int numTilesX = DEFERRED_NUM_TILES_X;
	constexpr int numTilesY = DEFERRED_NUM_TILES_Y;
	// Round tileSizeX up to the next multiple of 8 so the OuterVec
	// kernel's 8-wide vec loop sees aligned tiles for all-but-the-last
	// tile column. The last tile width can still be unaligned at non-
	// multiple-of-8 xres (rare on real displays — most are 8-aligned)
	// — handled by the lane-in-range mask inside OuterVec.
	// tileSizeY doesn't need this because the kernels iterate rows one
	// at a time, not in 8-tall groups.
	const int rawTileX  = (xres + (numTilesX - 1)) / numTilesX;
	const int tileSizeX = (rawTileX + 7) & ~7;
	const int tileSizeY = (yres + (numTilesY - 1)) / numTilesY;

	// Per-tile light culling: project each omni's bounding sphere
	// into screen space, find which tiles it overlaps, populate
	// indices[] per tile.
	static TileLights s_tileLights[DEFERRED_NUM_TILES];
	TileLights *const tileLights = (ov && ov->tileLights) ? ov->tileLights : s_tileLights;
	const float invZScale = 1.0f / float(zscale);
	const long long _lsetup = TailProf::nowNs();
	computeTileDepthBounds(tileLights, numTilesX, numTilesY,
	                       tileSizeX, tileSizeY, xres, yres,
	                       invZScale, reinterpret_cast<const uint16_t*>(zpage16));
	TailProf::mark("depth-bounds", _lsetup);   // per-tile z-buffer scan (par-able)
	// Mirror-footprint presence per tile/strip, for the clone-light
	// cull in the list builders. Only computed when a scene actually
	// activated the mask plane (GreetsMirror::BuildMirror).
	const uint8_t *mirrorMaskPlane =
		(gbuf && gbuf->mirrorMask.size() >= numPixels)
		? gbuf->mirrorMask.data() : nullptr;
	// Main-frame only: an offscreen shard bake has no mirrorMask in its
	// G-buffer, so mirrorMaskPlane is null below and this stays untouched.
	static uint32_t tileMirrorPresence[DEFERRED_NUM_TILES];
	const uint32_t *tilePresence = nullptr;
	if (mirrorMaskPlane) {
		const long long _mgrid = TailProf::nowNs();
		computeMirrorPresenceGrid(mirrorMaskPlane, xres, yres,
		                          tileSizeX, tileSizeY,
		                          numTilesX, numTilesY,
		                          tileMirrorPresence);
		tilePresence = tileMirrorPresence;
		TailProf::mark("mirror-grid", _mgrid);   // full-res mirrorMask scan
	}
	const fds::CameraContext &camCtx = (ov && ov->cam) ? *ov->cam : fds::g_mainCamera;
	const long long _tcull = TailProf::nowNs();
	buildTileLightLists(tileLights, numTilesX, numTilesY,
	                    tileSizeX, tileSizeY, xres, yres,
	                    lights, numLights, tilePresence, camCtx);
	TailProf::mark("tile-cull", _tcull);       // light-major append (harder to par)

	// Diagnostic (FDS_TILE_LIGHT_PROF=1): avg/max surviving lights per tile
	// after the cone cull, main frame only. Decides whether the deferred-kernel
	// cost is "many lights survive" (→ contribution culling) or "few lights but
	// heavy per-light math" (→ specular). Per-frame at the dispatcher, not hot.
	static const bool s_tileLightProf = (std::getenv("FDS_TILE_LIGHT_PROF") != nullptr);
	if (s_tileLightProf && !ov) {
		static long long accFrames = 0, accSum = 0, accTiles = 0;
		static int       accMax = 0, accTilesWith = 0, accNonEmpty = 0;
		const int nT = numTilesX * numTilesY;
		int sum = 0, mx = 0, nonEmpty = 0;
		for (int t = 0; t < nT; ++t) {
			const int c = tileLights[t].count;
			sum += c; if (c > mx) mx = c; if (c > 0) ++nonEmpty;
		}
		accFrames++; accSum += sum; accTiles += nT; accNonEmpty += nonEmpty;
		if (mx > accMax) accMax = mx;
		if (accFrames % 60 == 0) {
			std::fprintf(stderr,
				"[TILE-LIGHT-PROF] tiles=%d  avg/tile=%.1f  avg/non-empty=%.1f  max=%d  (numLights=%d, %lld frames)\n",
				nT, double(accSum)/double(accTiles),
				accNonEmpty ? double(accSum)/double(accNonEmpty) : 0.0,
				accMax, numLights, accFrames);
		}
	}

	// Per-strip light lists for the unified-TBR transparent path's
	// RenderXparClumpInStrip. 1D Y-strips of TILESIZE rows; built only
	// when the unified path is active to avoid the per-frame cost on
	// the legacy path. (Strip count = ceil(yres / TILESIZE), capped at
	// DEFERRED_MAX_STRIPS=512.)
	if (!ov && deferredUnifiedTbrEnabled(Sc)) {   // strips are main-frame xpar only
		constexpr int STRIP_H = 1 << 3;  // TILESIZE from FILLERS.CPP
		const int numStrips = (yres + STRIP_H - 1) >> 3;
		static uint32_t stripMirrorPresence[DEFERRED_MAX_STRIPS];  // main-frame only (see above)
		const uint32_t *stripPresence = nullptr;
		if (mirrorMaskPlane) {
			computeMirrorPresenceGrid(mirrorMaskPlane, xres, yres,
			                          xres, STRIP_H,
			                          1, std::min(numStrips, DEFERRED_MAX_STRIPS),
			                          stripMirrorPresence);
			stripPresence = stripMirrorPresence;
		}
		const long long _slist = TailProf::nowNs();
		buildStripLightLists(numStrips, STRIP_H, yres, lights, numLights,
		                     stripPresence, fds::g_mainCamera);
		TailProf::mark("strip-lists", _slist);
	}
	if (fds::FeatureFlags::deferred_tile_stats()) {
		int total = 0, tmin = INT_MAX, tmax = 0;
		for (int t = 0; t < DEFERRED_NUM_TILES; ++t) {
			total += tileLights[t].count;
			tmin = std::min(tmin, tileLights[t].count);
			tmax = std::max(tmax, tileLights[t].count);
		}
		fprintf(stderr, "[TILE-LIGHTS] numLights=%d tiles avg=%.1f min=%d max=%d\n",
			numLights, total / float(DEFERRED_NUM_TILES), tmin, tmax);
	}

	// File-scope static: also read by the transparent-lighting kernel
	// (Render_DeferredTransparentLighting_Tile) — same per-frame setup,
	// no point rebuilding it. The kernels never run concurrently with
	// each other, so the single instance is safe.
	ctx.gb         = &gb;
	ctx.matTable   = matTable;
	ctx.lights     = &lights;
	ctx.numLights  = numLights;
	ctx.tileLights = tileLights;
	ctx.hasMirrorPresence = (tilePresence != nullptr);
	if (tilePresence)
		std::memcpy(ctx.tileMirrorPresence, tilePresence,
		            sizeof(ctx.tileMirrorPresence));
	ctx.invFOVX    = 1.0f / fovX;
	ctx.invFOVY    = 1.0f / fovY;
	ctx.invZScale  = 1.0f / float(zscale);
	ctx.Sc         = Sc;
	ctx.waterMatID = g_deferredWaterMatID;
	// view → world (transpose of view rotation + camera origin). Used
	// per pixel for cube shadow sampling to convert view-space sample
	// to world for face selection. view.Mat is a pure rotation, so
	// transpose == inverse.
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 3; ++c)
			ctx.viewToWorld[r][c] = view->Mat[c][r];
	ctx.cameraWorldX = view->ISource.x;
	ctx.cameraWorldY = view->ISource.y;
	ctx.cameraWorldZ = view->ISource.z;
	// Render-target addressing for the tile kernels (RenderContext migration):
	// snapshot the globals here so the kernels read ctx, not the globals.
	ctx.xres       = xres;
	ctx.yres       = yres;
	ctx.vpage      = vpage;
	ctx.zpage16    = zpage16;
	ctx.cntrEX     = cntrEX;
	ctx.cntrEY     = cntrEY;
	ctx.fovX       = fovX;
	ctx.fovY       = fovY;
	ctx.zscale     = float(zscale);
	ctx.gbXpar     = gbXpar;
	ctx.xparZ      = xparZ;
	ctx.xparZBack  = xparZBack;

	// Wave 1: shade even cells (full deferred kernel). When checkerboard
	// is off, this is the entire pass and odd-cell skip is a no-op.
	// Dispatch: main frame tiles across the pool; an offscreen bake
	// (ov->inlineDispatch) runs the tiles on the calling worker thread (it is
	// itself a pool thread doing a WHOLE render — no nested enqueue). The tile
	// kernel releases renderns::tileDone at its end, so the inline path drains
	// it per tile to stay net-zero on the shared semaphore.
	const bool useOuterVec = deferredLightingOuterVecEnabled(Sc);
	const bool inlineDispatch = ov && ov->inlineDispatch;
	if (!inlineDispatch) renderns::tileCounter = 0;
	const long long _w1q = TailProf::nowNs();
	constexpr int nTiles = DEFERRED_NUM_TILES;
	auto tileBounds = [tileSizeX, tileSizeY, xres, yres](int t, int &x1, int &y1, int &x2, int &y2) {
		const int j = t / numTilesX, i = t - j * numTilesX;
		y1 = tileSizeY * j; y2 = std::min(y1 + tileSizeY, yres);
		x1 = tileSizeX * i; x2 = std::min(x1 + tileSizeX, xres);
	};
	if (inlineDispatch) {
		for (int t = 0; t < nTiles; ++t) {
			int x1, y1, x2, y2; tileBounds(t, x1, y1, x2, y2);
			if (useOuterVec) Render_DeferredLighting_Tile_OuterVec(ctx, t, x1, y1, x2, y2);
			else             Render_DeferredLighting_Tile(ctx, t, x1, y1, x2, y2);
			renderns::tileDone.acquire();
		}
	} else {
		// Work-stealing chunk dispatch via dispatchIndexed (the tile kernel
		// releases tileDone itself → done=nullptr; drain unchanged). The old
		// task-per-tile loop cost ~1.2 ms SERIAL per wave (96 ×
		// mutex+notify_one, and each woken worker contends the same queue
		// mutex to pop). Tiles write disjoint regions in any order →
		// byte-identical.
		dispatchIndexed(nTiles, nullptr, [&ctx, useOuterVec, tileBounds](int t) {
			int x1, y1, x2, y2; tileBounds(t, x1, y1, x2, y2);
			if (useOuterVec) Render_DeferredLighting_Tile_OuterVec(ctx, t, x1, y1, x2, y2);
			else             Render_DeferredLighting_Tile(ctx, t, x1, y1, x2, y2);
		});
		TailProf::mark("w1-enqueue", _w1q);
		TailProf::drain(renderns::tileDone, nTiles, "lighting-w1");
	}

	// Wave 2: fill odd cells via 2-tap interpolation (with full-shade
	// fallback at material edges). Skip entirely when checkerboard is
	// off — wave 1 already covered everything.
	if (deferredLightingCheckerboardEnabled() || deferredLightingQuarterEnabled()) {
		if (!inlineDispatch) renderns::tileCounter = 0;
		if (inlineDispatch) {
			for (int t = 0; t < nTiles; ++t) {
				int x1, y1, x2, y2; tileBounds(t, x1, y1, x2, y2);
				Render_DeferredLighting_TileFill(ctx, t, x1, y1, x2, y2);
				renderns::tileDone.acquire();
			}
		} else {
			// Same dispatchIndexed shape as wave 1 (see above).
			dispatchIndexed(nTiles, nullptr, [&ctx, tileBounds](int t) {
				int x1, y1, x2, y2; tileBounds(t, x1, y1, x2, y2);
				Render_DeferredLighting_TileFill(ctx, t, x1, y1, x2, y2);
			});
			TailProf::drain(renderns::tileDone, nTiles, "lighting-w2");
		}
	}

	// Dump cache-line transition stats accumulated by shadow sampling
	// during this frame's tile work. Reset to zero for the next frame.
	if (fds::FeatureFlags::shadow_prof_cache()) {
		const uint64_t s = g_shadowProfSamples.exchange(0, std::memory_order_relaxed);
		const uint64_t t = g_shadowProfLineTransitions.exchange(0, std::memory_order_relaxed);
		const double pct = s ? 100.0 * double(t) / double(s) : 0.0;
		std::fprintf(stderr,
			"[SHADOW-CACHE] samples=%llu line-transitions=%llu (%.2f%%)\n",
			(unsigned long long)s, (unsigned long long)t, pct);
	}
}

// ─── Wrappers for the renderFrame orchestrator ───────────────────────────
// renderFrame in RENDER.CPP dispatches transparent-layer composites in a
// tile-job lambda; the template lives here, so we expose
// front/back wrappers it can forward into without seeing the template.

void renderDeferredTransparentTile_Front(const DeferredLightingCtx &ctx,
                                          int tileIdx, int x1, int y1, int x2, int y2) {
	Render_DeferredTransparentLighting_Tile<XparLayer::Front>(
		ctx, tileIdx, x1, y1, x2, y2);
	// Release the renderns::tileDone permit on behalf of the inner
	// template — see the comment in that template's body for why the
	// release lives here instead of inside.
	renderns::tileDone.release();
}
void renderDeferredTransparentTile_Back(const DeferredLightingCtx &ctx,
                                         int tileIdx, int x1, int y1, int x2, int y2) {
	Render_DeferredTransparentLighting_Tile<XparLayer::Back>(
		ctx, tileIdx, x1, y1, x2, y2);
	renderns::tileDone.release();
}

