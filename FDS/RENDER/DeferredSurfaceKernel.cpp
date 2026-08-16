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
#include <atomic>
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
#include "RENDER/EnvCube.h"  // --env_cube: trig-free padded cube-face lookup
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
namespace renderns {
	extern std::counting_semaphore<INT_MAX> tileDone;
	extern std::mutex                tileCounterMutex;
	extern std::atomic<int>          tileCounter;
	extern std::condition_variable   condition;
}

// ── Glass refraction (--glass_refract) — LAYERED / per-layer snapshot support ──
//
// LEGACY per-mesh peel: renderFrame re-snapshots the accumulated background
// into fds::g_glassRefrHdr / g_glassRefrLdr right BEFORE each GLASS (normal-
// mapped) transparent batch composites, so each glass layer refracts a stable
// buffer that already holds everything behind it (opaque + earlier transparent
// layers). The batch loop is serial with a tile-drain barrier between batches,
// so the snapshot is taken on a quiescent buffer on the main thread → race-free.
//
// UNIFIED-TBR peel: each strip composites its own clumps back-to-front on its
// OWN worker thread — there is no global pre-layer barrier. A full-frame per-
// clump snapshot would race (other strips are mid-composite in their bands) and
// be non-deterministic. Instead, before a GLASS clump composites, the strip
// snapshots ONLY its own row band into these thread_local buffers (the strip
// exclusively owns those rows → race-free, deterministic). The refraction
// sampler then reads the band snapshot for in-band rows (correct layering =
// everything behind this clump in this strip) and the immutable pre-TBR opaque
// snapshot (g_glassRefrHdr/Ldr) for out-of-band rows (large vertical offsets
// crossing into a neighbour strip, which is being composited concurrently and
// cannot be read safely). Both sources are stable → 0% frame-to-frame diff.
static thread_local std::vector<fds::hdrf> t_glassBandHdr;   // strip band HDR (B,G,R,cov ×N) — matches g_hdrBuf storage
static thread_local std::vector<uint32_t> t_glassBandLdr;   // strip band VPage (BGRA ×N)
static thread_local int t_glassBandY0 = 0;                  // inclusive
static thread_local int t_glassBandY1 = 0;                  // exclusive; ==Y0 ⇒ inactive (legacy path)

// Snapshot the strip's row band of the accumulated background just BEFORE a
// glass clump composites. Sets the band bounds the sampler branches on.
static void glassBandSnapshotBegin(int strip_y, int strip_h) {
	const int y0 = strip_y < 0 ? 0 : strip_y;
	const int y1 = std::min(strip_y + strip_h, int(YRes));
	if (y1 <= y0) { t_glassBandY0 = t_glassBandY1 = 0; return; }
	const size_t bandRows = size_t(y1 - y0);
	const size_t bandPx   = bandRows * size_t(XRes);
	const size_t fullN4   = size_t(YRes) * size_t(XRes) * 4;
	if (fds::g_hdrActive && fds::g_hdrBuf.size() >= fullN4) {
		if (t_glassBandHdr.size() < bandPx * 4) t_glassBandHdr.resize(bandPx * 4);
		std::memcpy(t_glassBandHdr.data(),
		            fds::g_hdrBuf.data() + size_t(y0) * size_t(XRes) * 4,
		            bandPx * 4 * sizeof(fds::hdrf));
	}
	if (VPage) {
		if (t_glassBandLdr.size() < bandPx) t_glassBandLdr.resize(bandPx);
		std::memcpy(t_glassBandLdr.data(),
		            reinterpret_cast<const uint32_t*>(VPage) + size_t(y0) * size_t(XRes),
		            bandPx * sizeof(uint32_t));
	}
	t_glassBandY0 = y0; t_glassBandY1 = y1;
}
static void glassBandSnapshotEnd() { t_glassBandY0 = t_glassBandY1 = 0; }

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

// --sh_ambient: evaluate the scene's baked L2 SH irradiance along a
// WORLD-space normal (nx,ny,nz). `c` is the 27-float channel-major coefficient
// block from SHAmbient_Coeffs (B[0..8], G[9..17], R[18..26]); the A_l cosine
// convolution / π is already folded in at bake, so this is a direct
// env-colour-scaled irradiance. ~9 FMAs/channel, no texture read. Output is
// clamped >=0 (low-order SH can ring negative). Basis order matches the bake:
// Y00, Y1-1(y), Y10(z), Y11(x), Y2-2(xy), Y2-1(yz), Y20(3z²-1), Y21(xz),
// Y22(x²-y²).
static inline void shEvalIrradiance(const float* c, float x, float y, float z,
                                    float& outB, float& outG, float& outR) {
	const float b0 = 0.282095f;
	const float b1 = 0.488603f * y;
	const float b2 = 0.488603f * z;
	const float b3 = 0.488603f * x;
	const float b4 = 1.092548f * (x * y);
	const float b5 = 1.092548f * (y * z);
	const float b6 = 0.315392f * (3.0f * z * z - 1.0f);
	const float b7 = 1.092548f * (x * z);
	const float b8 = 0.546274f * (x * x - y * y);
	float B = c[0]*b0 + c[1]*b1 + c[2]*b2 + c[3]*b3 + c[4]*b4
	        + c[5]*b5 + c[6]*b6 + c[7]*b7 + c[8]*b8;
	float G = c[9]*b0 + c[10]*b1 + c[11]*b2 + c[12]*b3 + c[13]*b4
	        + c[14]*b5 + c[15]*b6 + c[16]*b7 + c[17]*b8;
	float R = c[18]*b0 + c[19]*b1 + c[20]*b2 + c[21]*b3 + c[22]*b4
	        + c[23]*b5 + c[24]*b6 + c[25]*b7 + c[26]*b8;
	outB = B > 0.0f ? B : 0.0f;
	outG = G > 0.0f ? G : 0.0f;
	outR = R > 0.0f ? R : 0.0f;
}

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

// ── refined 8-wide reciprocal / reciprocal-sqrt ───────────────────────────
// Estimate + one Newton-Raphson step. The BARE _mm256_rcp_ps/_mm256_rsqrt_ps
// are ~12-bit on x86, and simde lowers them to vrecpeq/vrsqrteq on arm64 —
// COARSER still. That precision difference is not cosmetic in a GGX lobe: the
// D term divides by (NdotH²(a²-1)+1)², which at low roughness is a small
// number squared, so the relative error compounds. Measured (audit a1b2408,
// greets t=4871, --deferred_vec vs scalar): rough 0.2 -> mean |dY| 8.379,
// max 178, 5 091 px >10 luma; rough 0.5 -> 0.421; rough 1.0 -> 0.374, where
// the denominator is exactly 1 and the gap vanishes into the float floor.
// Since --deferred_vec defaults ON for x86 and OFF for arm64, that made the
// two architectures ship DIFFERENT IMAGES.
//
// rsqrt8_nr already existed further down this file (the OuterVec normal
// decode, with a comment naming this exact failure); it is hoisted here so
// the GGX loop can use it too, and rcp8_nr is its missing twin.
static inline __m256 rsqrt8_nr(__m256 x)
{
	__m256 e = _mm256_rsqrt_ps(x);
	const __m256 xe2 = _mm256_mul_ps(_mm256_mul_ps(x, e), e);
	return _mm256_mul_ps(e, _mm256_fnmadd_ps(_mm256_set1_ps(0.5f), xe2,
	                                          _mm256_set1_ps(1.5f)));
}
// r1 = r0·(2 - x·r0), the standard NR step for 1/x.
static inline __m256 rcp8_nr(__m256 x)
{
	__m256 r = _mm256_rcp_ps(x);
	return _mm256_mul_ps(r, _mm256_fnmadd_ps(x, r, _mm256_set1_ps(2.0f)));
}

// --pbr TEST: Cook-Torrance microfacet specular (GGX NDF + Smith-Schlick
// geometry + Schlick Fresnel), 8 lights wide, replacing the Blinn-Phong
// run_vec_spec_loop. This is a COST/quality probe — wired into the vec path so
// we can measure SIMD PBR per-light against the existing Blinn-Phong term.
// Divides go through rcp8_nr/rsqrt8_nr under --vec_ggx_refine (default ON);
// see that flag for why the bare estimates were an architecture-visible bug.
// roughness ∈ (0,1], F0 = dielectric 0.04 (metallic workflow TBD).
// Specular only — diffuse is accumulated by the caller's existing loop.
static inline void run_vec_ggx_loop(const TileLights &tl,
                                     float x, float y, float z,
                                     float nx, float ny, float nz,
                                     float vx, float vy, float vz,
                                     float matSpec, float roughness,
                                     uint32_t pmid,
                                     const float *coneShadowAtten,
                                     float &sB, float &sG, float &sR) {
	// --vec_ggx_refine: one NR step on every reciprocal in the lobe. Read once
	// here — it is a flag, not a per-light decision.
	const bool ggxRefine = fds::FeatureFlags::vec_ggx_refine();
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
		__m256 lenInv = ggxRefine ? rsqrt8_nr(safe_len2) : _mm256_rsqrt_ps(safe_len2);
		__m256 dist   = _mm256_mul_ps(safe_len2, lenInv);
		__m256 falloff = _mm256_sub_ps(vOne, _mm256_mul_ps(dist, lrr));

		__m256 ldx = _mm256_mul_ps(wx, lenInv), ldy = _mm256_mul_ps(wy, lenInv), ldz = _mm256_mul_ps(wz, lenInv);
		__m256 NdotL = _mm256_mul_ps(dot, lenInv);   // dot is N·w (unnormalized) → ·lenInv
		// Half vector, normalized.
		__m256 hx = _mm256_add_ps(ldx, vvx_v), hy = _mm256_add_ps(ldy, vvy_v), hz = _mm256_add_ps(ldz, vvz_v);
		__m256 hLen2 = _mm256_fmadd_ps(hx, hx, _mm256_fmadd_ps(hy, hy, _mm256_mul_ps(hz, hz)));
		const __m256 hL2s = _mm256_max_ps(hLen2, _mm256_set1_ps(1e-12f));
		__m256 hInv = ggxRefine ? rsqrt8_nr(hL2s) : _mm256_rsqrt_ps(hL2s);
		__m256 NdotH = _mm256_mul_ps(_mm256_fmadd_ps(hx, vnx_v, _mm256_fmadd_ps(hy, vny_v, _mm256_mul_ps(hz, vnz_v))), hInv);
		__m256 VdotH = _mm256_mul_ps(_mm256_fmadd_ps(hx, vvx_v, _mm256_fmadd_ps(hy, vvy_v, _mm256_mul_ps(hz, vvz_v))), hInv);
		NdotH = _mm256_max_ps(NdotH, vZero);
		VdotH = _mm256_max_ps(VdotH, vZero);

		// D (GGX): a² / (π · (NdotH²·(a²-1)+1)²)
		__m256 nh2 = _mm256_mul_ps(NdotH, NdotH);
		__m256 denomD = _mm256_fmadd_ps(nh2, _mm256_sub_ps(va2, vOne), vOne);   // NdotH²(a²-1)+1
		denomD = _mm256_mul_ps(denomD, denomD);                                 // squared
		const __m256 dD = _mm256_max_ps(denomD, _mm256_set1_ps(1e-6f));
		__m256 D = _mm256_mul_ps(_mm256_mul_ps(va2, vInvPi), ggxRefine ? rcp8_nr(dD) : _mm256_rcp_ps(dD));

		// G (Smith, Schlick-GGX): Gv·Gl, Gx = Ndotx / (Ndotx·(1-k)+k)
		__m256 oneMinusK = _mm256_sub_ps(vOne, vk);
		const __m256 dGv = _mm256_fmadd_ps(vNdotV, oneMinusK, vk);
		__m256 Gv = _mm256_mul_ps(vNdotV, ggxRefine ? rcp8_nr(dGv) : _mm256_rcp_ps(dGv));
		const __m256 dGl = _mm256_fmadd_ps(NdotL, oneMinusK, vk);
		__m256 Gl = _mm256_mul_ps(NdotL, ggxRefine ? rcp8_nr(dGl) : _mm256_rcp_ps(dGl));
		__m256 G  = _mm256_mul_ps(Gv, Gl);

		// F (Schlick): F0 + (1-F0)·(1-VdotH)^5
		__m256 om = _mm256_sub_ps(vOne, VdotH);
		__m256 om2 = _mm256_mul_ps(om, om);
		__m256 om5 = _mm256_mul_ps(_mm256_mul_ps(om2, om2), om);
		__m256 F = _mm256_fmadd_ps(_mm256_sub_ps(vOne, vF0), om5, vF0);

		// spec = D·G·F / (4·NdotV·NdotL) · NdotL (radiance) = D·G·F/(4·NdotV)
		__m256 specBRDF = _mm256_mul_ps(_mm256_mul_ps(D, G), F);
		__m256 denom = _mm256_mul_ps(_mm256_set1_ps(4.0f), vNdotV);
		__m256 spec = _mm256_mul_ps(specBRDF, ggxRefine ? rcp8_nr(denom) : _mm256_rcp_ps(denom));   // ·NdotL folded out (radiance) and NdotL/NdotL cancels one
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
static bool deferredLightingOuterVecEnabled() {
	// Tri-state: when set explicitly (CLI or env), the flag wins; otherwise
	// fall back to the scene's PreferOuterVec policy. Greets sets it off
	// because the outer kernel's per-lane nmap costs dominate most pixels.
	if (fds::FeatureFlags::isSet(fds::FeatureFlags::BoolId::deferred_outer_vec))
		return fds::FeatureFlags::deferred_outer_vec();
	return CurScene && CurScene->PreferOuterVec != 0;
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
				const uint16_t zS = std::max(ShadowTexZ(sm.packSD[o]),
				                             ShadowTexZ(sm.packDyn[o]));
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
				// ─── 8×8 PolyId uniformity pyramid, the 2-D SPOT tap ─────
				// The same uniSD lookup CubeShadow_Sample already takes, on
				// the other tap that reads these maps. The pyramid is built
				// for EVERY map the bake writes — spot maps included, since
				// they are in the same swzMaps list (Shadows.cpp) — so the
				// ten greets spot pyramids were being rebuilt every frame
				// with no reader. This is that reader.
				//
				// THE UNIFORMITY CONDITION IS SIMPLER HERE THAN IN THE CUBE.
				// The cube tap's PolyId arm resolves closest-by-z ACROSS both
				// planes, so it can only be settled without a read when the
				// dynamic apron is uniformly empty (uD == 0). This tap's
				// PolyId arm reads `psB` and only `psB` — the four
				// ShadowTexId loads below all come from the static plane, the
				// dynamic plane appearing only in the z00..z11 quartet that
				// the depth arm uses. So uniSD alone is the whole verdict,
				// whatever the dynamic plane holds, and no uniDyn read is
				// needed. (z00..z11 are computed unconditionally today and
				// are dead in PolyId mode — the fast path drops those eight
				// packed loads as well as the four id loads.)
				//
				// NO STRADDLE CASE. The range test just above is
				// `iX + 1 < xres && iY + 1 < yres`, the same footprint the
				// 9×9 apron summarises, so the block index (iX>>3, iY>>3) is
				// sufficient on its own. See ShadowMap.h.
				//
				// PLACED ABOVE THE ADDRESSING BLOCK, NOT INSIDE THE TAP.
				// Three refutations (0b85e5df) say a new predicate in the
				// tap's innermost body costs more than it removes; 91891249
				// measured the same lookup worth an extra −0.054 Gi/f purely
				// for sitting above the swizzle test / row offset / four
				// texel offsets / two base pointers rather than below them.
				// Both arms skip that block whole.
				//
				// BYTE-NULL BY CONSTRUCTION. Uniform-LIT: every `occ += w`
				// below is skipped, occ stays at its 0.0f initialiser, the
				// tail computes `1.0f - 0.0f` and this is the last statement
				// before `return shadowAtten` — so `return 1.0f` is the same
				// bit pattern. Uniform-OCCLUDING: all four fire, so it falls
				// through to the SHARED weight block and sums those same four
				// floats in the same order (recomputing them in a second
				// place would be a bet on clang contracting
				// (1.0f-fx)*(1.0f-fy) into the same fnmsub twice). Depth mode
				// never enters — `polyId` carries the mode test the tail's
				// own branch used to make, hoisted to one load.
				//
				// --shadow_prof_cache's line-transition counter lives inside
				// the addressing block and is therefore not bumped for a
				// skipped tap. That is the truthful accounting: a skipped tap
				// touches no shadow cache line.
				const ShadowMode mode = g_shadowMode.load(std::memory_order_relaxed);
				// surfaceShadowId < 0 = non-casting receiver under
				// --shadow_noncaster_depth; the identity test is unsatisfiable
				// for it, so it takes the biased depth branch and the pyramid
				// (an id summary) cannot speak for it.
				const bool polyId = (mode == ShadowMode::PolyId && surfaceShadowId >= 0);
				uint32_t uniC = kShadowUniMixed;
#if FDS_SHADOW_TAP_CENSUS
				if (polyId) {
					FDS_PYR_CENSUS(spotReached);
					if (sm.uniSD.empty()) FDS_PYR_CENSUS(spotNoPyr);
				}
#endif
				if (polyId && !sm.uniSD.empty()) {
					const size_t b = size_t(iY >> kShadowUniShift) * size_t(sm.uniW)
					               + size_t(iX >> kShadowUniShift);
					uniC = sm.uniSD[b];
					if (uniC == kShadowUniMixed) { FDS_PYR_CENSUS(spotMixed); }
					else if (uniC == 0u || uint16_t(uniC) == uint16_t(surfaceShadowId)) {
						FDS_PYR_CENSUS(spotFastLit);
						return 1.0f;   // occ would never leave its 0.0f initialiser
					} else {
						FDS_PYR_CENSUS(spotFastOcc);
					}
				}
				const float fx = smX - float(iX);
				const float fy = smY - float(iY);
				const float w00 = (1.0f - fx) * (1.0f - fy);
				const float w10 =         fx  * (1.0f - fy);
				const float w01 = (1.0f - fx) *         fy;
				const float w11 =         fx  *         fy;
				// Uniform OCCLUDING block: all four `occ += w` fire, in this
				// order, over these same four floats.
				if (uniC != kShadowUniMixed) {
					float occU = 0.0f;
					occU += w00; occU += w10; occU += w01; occU += w11;
					return (occU >= 1.0f) ? 0.0f : (1.0f - occU);
				}
				// Tap addressing: linear row-major, or 8×8-tiled under
				// --shadow-swizzle (see CubeShadow_Sample / ShadowSwzOffset;
				// halves the cache lines a 2×2 footprint touches). Falls back
				// to linear when the tiled copies aren't built — both layouts
				// hold identical values.
				const bool swz = fds::FeatureFlags::shadow_swizzle()
				              && !sm.packSDSw.empty() && !sm.packDynSw.empty();
				size_t o00, o10, o01, o11;
				const uint32_t *psB, *pdB;
				if (swz) {
					const ShadowSwzShape &shp = ShadowSwzGetShape();
					const int tpr = ShadowSwzTilesPerRow(sm.xres, shp);
					o00 = ShadowSwzOffset(iX,     iY,     tpr, shp);
					o10 = ShadowSwzOffset(iX + 1, iY,     tpr, shp);
					o01 = ShadowSwzOffset(iX,     iY + 1, tpr, shp);
					o11 = ShadowSwzOffset(iX + 1, iY + 1, tpr, shp);
					psB = sm.packSDSw.data(); pdB = sm.packDynSw.data();
				} else {
					const size_t rowOfs = size_t(iY) * size_t(sm.xres);
					o00 = rowOfs + size_t(iX);   o10 = o00 + 1;
					o01 = o00 + size_t(sm.xres); o11 = o01 + 1;
					psB = sm.packSD.data(); pdB = sm.packDyn.data();
				}
				// Per-tap closest-occluder. Static plane holds the once-baked
				// statics; dynamic holds animated meshes (zero when off).
				// max() wins on whichever caster is closer. One 32-bit load
				// per texel per plane yields both halves.
				const uint16_t z00 = std::max(ShadowTexZ(psB[o00]), ShadowTexZ(pdB[o00]));
				const uint16_t z10 = std::max(ShadowTexZ(psB[o10]), ShadowTexZ(pdB[o10]));
				const uint16_t z01 = std::max(ShadowTexZ(psB[o01]), ShadowTexZ(pdB[o01]));
				const uint16_t z11 = std::max(ShadowTexZ(psB[o11]), ShadowTexZ(pdB[o11]));
				if (profShadowCache) {
					// One PCF check = one tracked sample. Use the (00) tap's
					// cache-line address — adjacent shadow checks on the same
					// thread that share this line are hits.
					const uintptr_t line =
						reinterpret_cast<uintptr_t>(&psB[o00]) >> 6;
					if (s_shadowProfLastLine != line) {
						g_shadowProfLineTransitions.fetch_add(1, std::memory_order_relaxed);
						s_shadowProfLastLine = line;
					}
					g_shadowProfSamples.fetch_add(1, std::memory_order_relaxed);
				}
				float occ = 0.0f;
				// The four PCF weights and the `polyId` mode test were both
				// hoisted above the addressing block by the uniformity fast
				// path — computed once, read here.
				if (polyId) {
					// Surface matID extracted from gb.txtr's packed
					// (miplevel:4 | matID:8 | swizzledUV:20). Shadow buffer
					// stores matID+1 of the closest occluder; +1 here too so the
					// comparison uses the same offset, and 0 stays as the
					// "no occluder" sentinel.
					const uint16_t surfaceId = uint16_t(surfaceShadowId);
					const uint16_t i00 = ShadowTexId(psB[o00]);
					const uint16_t i10 = ShadowTexId(psB[o10]);
					const uint16_t i01 = ShadowTexId(psB[o01]);
					const uint16_t i11 = ShadowTexId(psB[o11]);
					if (i00 != surfaceId && i00 != 0) occ += w00;
					if (i10 != surfaceId && i10 != 0) occ += w10;
					if (i01 != surfaceId && i01 != 0) occ += w01;
					if (i11 != surfaceId && i11 != 0) occ += w11;
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

// Env-specular reflection compose (--env_refl): sample the surface's baked
// panorama along the reflected view ray and add the Fresnel-weighted,
// roughness-mip-blurred, metal-tinted contribution into the spec
// accumulators. Extracted VERBATIM from the wave-1 scalar compose so the
// wave-2 quarter/checker fill FALLBACK shades identically — the fallback
// previously had no env block at all, which rendered reflections as a
// 1-of-4 dot grid on any surface whose neighbors fail the fill's
// normal-similarity test (curved cockpit glass under --deferred-quarter).
// FDS_ENVTRACE="sx,sy,r": diagnostic. Once per kernel invocation, print the
// FULL env-compose term chain (world pos, normal, pull gate/k, reflected dir,
// face/uv, mip, Fresnel, fetched color, contribution) for the first composed
// pixel whose reconstructed screen position lands within r px of (sx,sy) —
// the term that flips frame-to-frame while the camera is monotone is the bug.
// Claimed atomically so exactly one line prints per invocation across tile
// threads; the invocation counter is bumped in the per-frame ctx setup, so a
// second setup line per frame would expose a second kernel pass.
struct EnvTraceCfg { float sx, sy, r; float minY; };
static std::atomic<uint32_t> g_envTraceInvoke{0};
static std::atomic<uint32_t> g_envTraceClaim{~0u};
// Per-invocation compose census (FDS_ENVTRACE only): how many pixels ran the
// env compose, how many had mirrored-world positions (y<0), and the screen
// bbox — printed and reset at the next setup, so each frame shows where each
// pass actually composes.
static std::atomic<uint32_t> g_envCensusN{0}, g_envCensusNegY{0};
static std::atomic<int> g_envCensusX0{1<<30}, g_envCensusX1{-(1<<30)};
static std::atomic<int> g_envCensusY0{1<<30}, g_envCensusY1{-(1<<30)};
static inline void envCensusAdd(float scrX, float scrY, float worldY)
{
	g_envCensusN.fetch_add(1, std::memory_order_relaxed);
	if (worldY < 0.0f) g_envCensusNegY.fetch_add(1, std::memory_order_relaxed);
	const int xi = int(scrX), yi = int(scrY);
	int v = g_envCensusX0.load(std::memory_order_relaxed);
	while (xi < v && !g_envCensusX0.compare_exchange_weak(v, xi)) {}
	v = g_envCensusX1.load(std::memory_order_relaxed);
	while (xi > v && !g_envCensusX1.compare_exchange_weak(v, xi)) {}
	v = g_envCensusY0.load(std::memory_order_relaxed);
	while (yi < v && !g_envCensusY0.compare_exchange_weak(v, yi)) {}
	v = g_envCensusY1.load(std::memory_order_relaxed);
	while (yi > v && !g_envCensusY1.compare_exchange_weak(v, yi)) {}
}
static const EnvTraceCfg* EnvTraceGet()
{
	static EnvTraceCfg cfg;
	static int state = 0;   // 0 = unparsed, 1 = off, 2 = on
	if (state == 0) {
		const char* s = std::getenv("FDS_ENVTRACE");
		state = (s && std::sscanf(s, "%f,%f,%f", &cfg.sx, &cfg.sy, &cfg.r) == 3)
		        ? 2 : 1;
		// FDS_ENVTRACE_MINY: only claim pixels with world y above this —
		// city's mirrored-world pass (y < 0) otherwise wins the per-
		// invocation claim on most frames and starves the real facade.
		cfg.minY = -1e30f;
		if (const char* my = std::getenv("FDS_ENVTRACE_MINY"))
			cfg.minY = float(std::atof(my));
	}
	return state == 2 ? &cfg : nullptr;
}

// FDS_ENVVEC_STATS=1: per-callsite env-compose counters, printed every 64
// kernel setups. Answers "which kernel actually composes the glass" —
// the vec front-end only covers the OPAQUE OuterVec env-only lanes.
static std::atomic<uint64_t> g_envCntWave1{0}, g_envCntXpar{0},
	g_envCntOvScalar{0}, g_envCntOvEnvScalar{0}, g_envCntOvEnvVec{0};
static const bool g_envVecStats = std::getenv("FDS_ENVVEC_STATS") != nullptr;

// Face-major BILINEAR fetch from a padded-cube env store (in-face clamped —
// the D2 overscan padding makes that seam-free). Shared by the scalar
// compose and the OuterVec 8-wide env front-end so the two paths cannot
// drift in sampling convention.
static inline void EnvCubeFetchBil(const fds::EnvPanoLinear* envP, int lvl,
                                   int face, float u, float v,
                                   float& B, float& G, float& R)
{
	const int fr = envP->W >> lvl;
	float px = u * float(fr) - 0.5f;
	float py = v * float(fr) - 0.5f;
	if (px < 0.0f) px = 0.0f;
	if (py < 0.0f) py = 0.0f;
	int x0 = int(px), y0 = int(py);
	if (x0 > fr - 2) x0 = fr - 2;
	if (y0 > fr - 2) y0 = fr - 2;
	const float ax = px - float(x0), ay = py - float(y0);
	const uint32_t* base = envP->mip[lvl] + size_t(face) * fr * fr;
	const uint32_t p00 = base[size_t(y0) * fr + x0];
	const uint32_t p10 = base[size_t(y0) * fr + x0 + 1];
	const uint32_t p01 = base[size_t(y0 + 1) * fr + x0];
	const uint32_t p11 = base[size_t(y0 + 1) * fr + x0 + 1];
	auto ch = [&](int sh) -> float {
		const float t0 = float((p00 >> sh) & 0xFF)
		               + ax * (float((p10 >> sh) & 0xFF) - float((p00 >> sh) & 0xFF));
		const float t1 = float((p01 >> sh) & 0xFF)
		               + ax * (float((p11 >> sh) & 0xFF) - float((p01 >> sh) & 0xFF));
		return t0 + ay * (t1 - t0);
	};
	B = ch(0); G = ch(8); R = ch(16);
}

// --env_live_water's mask weight, taken at an ALREADY-PROJECTED direction.
//
// EnvBake.h's EnvLiveWater_Weight starts by calling EnvCube_DirToFaceUV on the
// unperturbed lookup direction — the SAME call, on the SAME operands, that the
// cube colour fetch makes one line later. Two dominant-axis selects and two
// divides for one projection, on every env pixel of the city glass. This form
// takes the face/u/v the caller already has, so the projection is paid once
// when the tilt does not fire and twice only when it does (a tilted direction
// is a different direction and must be re-projected).
//
// BIT-EXACT: the guard order and the texel pick below are a verbatim move of
// EnvLiveWater_Weight / EnvLiveWater_MaskBit (868ba5d8's one-bit-per-texel
// point-sampled form) minus the leading projection.
// It lives HERE rather than in EnvBake.h because the reflection-bake mask
// machinery in that header is under concurrent change; EnvBake.h stays the
// source of truth and this must be kept in step with it — if MaskBit's texel
// pick or guard set changes, change this too.
static inline float EnvLiveWaterWeightAtFaceUV(float bakeY, float dy,
                                               const uint8_t* mask, int maskRes,
                                               int face, float u, float v)
{
	const fds::EnvLiveWaterState& lw = fds::g_envLiveWater;
	if (!lw.active) return 0.0f;
	if (dy >= -1e-6f) return 0.0f;                  // above horizon
	if (bakeY <= lw.waterY) return 0.0f;            // probe under water: n/a
	if (!mask || maskRes < 8) return 0.0f;          // no water mask → no tilt
	int x = int(u * float(maskRes));
	int y = int(v * float(maskRes));
	if (x < 0) x = 0; else if (x > maskRes - 1) x = maskRes - 1;
	if (y < 0) y = 0; else if (y > maskRes - 1) y = maskRes - 1;
	const size_t bit = (size_t(face) * size_t(maskRes) + size_t(y)) * size_t(maskRes)
	                 + size_t(x);
	return ((mask[bit >> 3] >> (bit & 7)) & 1u) != 0u ? 1.0f : 0.0f;
}

// --env_mip_chain (§11 row E7): the VIRTUAL chain depth the roughness→level
// select divides by. 0 / <=1 = unset → the store's real numMips, so the
// default is byte-identical. Clamped to 2..16 (a depth of 1 would make every
// roughness select level 0, which is not a lobe width anyone wants and is
// already reachable with --no-env_refl). The RESULT is still clamped to the
// real chain by the caller — this widens the lobe for a given roughness, it
// does not invent levels the store does not have.
static inline int envMipChainDepth(int realMips) {
	const int v = fds::FeatureFlags::env_mip_chain();
	if (v <= 1) return realMips;
	return v > 16 ? 16 : v;
}

// --env_metal_tint_linear, GATED ON THE FRAME'S OWN COMPOSITE.
//
// The flag squares the conductor's albedo before using it as a reflectance on
// the env lobe. The reason it is right is that the --hdr_linear composite
// squares the albedo everywhere else (`rlB = aB*aB*lB + sB`) while this lobe's
// output lands in sB UNSQUARED — so in a linear frame a gamma reflectance is a
// per-channel error. That argument is entirely conditional on the frame being
// linear, and the flag did not carry the condition: it squared unconditionally,
// including in the GAMMA composite every scene but greets runs
// (GREETS.CPP setDefaults hdr_linear; the global default is 0).
//
// The standalone Metal oracle — the thing this flag was validated against —
// carries the condition explicitly: `S.baseColor = (u.hdrMode.x > 0.5) ?
// alb*alb : alb` (GpuBench/shaders/deferred.metal:456), and its conductor tint
// is `mix(1, S.baseColor, metal)` (:751). hdrMode.x IS hdr_linear
// (GpuBench/Deferred.mm:934). So gating here moves the CPU TOWARD the oracle in
// gamma frames and leaves the linear frame — greets, where the parity was
// measured (saturation 0.687 -> 0.840, the Metal arm's value exactly, commit
// 1782351) — bit-for-bit unchanged.
//
// One helper, three call sites (wave-1 scalar, outer-vec, tile fill) so they
// cannot drift apart.
static inline bool MetalTintLinearActive(const DeferredLightingCtx &ctx) {
	return fds::FeatureFlags::env_metal_tint_linear()
	    && fds::FeatureFlags::hdr() && ctx.hdrBuf != nullptr
	    && fds::FeatureFlags::hdr_linear();
}

static inline void EnvSpecComposeScalar(
	const DeferredLightingCtx &ctx, const fds::EnvPanoLinear *envP,
	const Material *Mat, uint32_t miplevel, uint32_t swizzledUV,
	float x, float y, float z, float nx, float ny, float nz,
	float sampleWorldX, float sampleWorldY, float sampleWorldZ,
	float texB, float texG, float texR,
	float gloss, float metalM, bool roughMapOn, float envReflGain,
	bool envBrdfAnalytic, bool multiScatter, bool metalTintLinear,
	float &sB, float &sG, float &sR,
	float *fresOut = nullptr)
{
	// FDS_ENV_SKIP_NEGY=1 (diagnostic A/B): skip the compose entirely for
	// pixels whose world y is negative — in CITY that is exactly the
	// mirrored-world pass's geometry. If facades change at all with this
	// set, mirror-pass compose output is leaking into the visible image.
	static const bool sSkipNegY = std::getenv("FDS_ENV_SKIP_NEGY") != nullptr;
	if (sSkipNegY && sampleWorldY < 0.0f) return;
	// ── FDS_ENVTRACE claim (diagnostic, off = one static load + branch) ──
	bool trWant = false;
	float trScrX = 0.0f, trScrY = 0.0f;
	if (const EnvTraceCfg* trc = EnvTraceGet()) {
		if (z > 0.001f) {
			trScrX = ctx.cntrEX + x / (z * ctx.invFOVX);
			trScrY = ctx.cntrEY + y / (z * ctx.invFOVY);
			envCensusAdd(trScrX, trScrY, sampleWorldY);
			if (sampleWorldY >= trc->minY &&
			    std::fabs(trScrX - trc->sx) <= trc->r &&
			    std::fabs(trScrY - trc->sy) <= trc->r) {
				const uint32_t inv = g_envTraceInvoke.load(std::memory_order_relaxed);
				uint32_t cur = g_envTraceClaim.load(std::memory_order_relaxed);
				if (cur != inv &&
				    g_envTraceClaim.compare_exchange_strong(cur, inv))
					trWant = true;
			}
		}
	}
	int   trPulled = -1;    // -1 = no-pull store, 0 = gate skipped, 1 = pulled
	float trPlaneD = 0, trOptD = 0, trStep = 0, trK = 0;
	float trEpX = 0, trEpY = 0, trEpZ = 0;
	// Incident ray d = pixel direction (view space, camera at
	// origin); reflect about the (possibly nmap-perturbed) N.
	float dInv = fast_rsqrt(x*x + y*y + z*z);
	float dx = x * dInv, dy = y * dInv, dz = z * dInv;
	// Viewer-side normal (forward parity: Transform.cpp's nSide flip). The
	// G-buffer normal's SIGN is unstable on double-sided glass — the z-fight
	// winner alternates per frame, and ENVTRACE showed the composed color
	// flapping with it (ndv 0.37<->0.00, Fresnel 0.61<->0.72, pull k
	// +0.85<->-0.75 on the same surface). The reflect below is invariant
	// under n->-n; only Fresnel and the cv-pull see the sign — flip to the
	// camera side so an upstream sign flip cannot change the output.
	{
		const float dn0 = dx*nx + dy*ny + dz*nz;
		if (dn0 > 0.0f) { nx = -nx; ny = -ny; nz = -nz; }
	}
	float dDotN;
	float rvx, rvy, rvz;
	if (envP->pullOpt > 0.0f) {
		// cv-pull damping, FAITHFUL port of the forward hack (Transform.cpp
		// Face_Reflective): distances measured TO THE SURFACE PLANE (per-
		// pixel normal), pull applied along eye->bakePoint, and the forward
		// path's degeneracy guard (skip when the pull direction is nearly
		// parallel to the plane). The first port used POINT distance to the
		// bake center, which has a closest-approach singularity: as the
		// scripted camera flies PAST a tower, unit(E-B) sweeps up to 180deg
		// and the pulled eye whips around the building -> the glass
		// reflections lurch frame-to-frame ("textures jump back and forth"),
		// per building, asynchronously. Plane distance is smooth through a
		// flyby; the guard covers the remaining ill-conditioned geometry --
		// both invariants are documented in the forward code for exactly
		// this failure ("wildly wrong reflections that swing with small
		// camera motions").
		const float nwx = ctx.viewToWorld[0][0]*nx + ctx.viewToWorld[0][1]*ny + ctx.viewToWorld[0][2]*nz;
		const float nwy = ctx.viewToWorld[1][0]*nx + ctx.viewToWorld[1][1]*ny + ctx.viewToWorld[1][2]*nz;
		const float nwz = ctx.viewToWorld[2][0]*nx + ctx.viewToWorld[2][1]*ny + ctx.viewToWorld[2][2]*nz;
		const float swx = ctx.viewToWorld[0][0]*x + ctx.viewToWorld[0][1]*y + ctx.viewToWorld[0][2]*z + ctx.cameraWorldX;
		const float swy = ctx.viewToWorld[1][0]*x + ctx.viewToWorld[1][1]*y + ctx.viewToWorld[1][2]*z + ctx.cameraWorldY;
		const float swz = ctx.viewToWorld[2][0]*x + ctx.viewToWorld[2][1]*y + ctx.viewToWorld[2][2]*z + ctx.cameraWorldZ;
		float epx = ctx.cameraWorldX, epy = ctx.cameraWorldY, epz = ctx.cameraWorldZ;
		{
			const float ux = envP->bakeX - epx, uy = envP->bakeY - epy,
			            uz = envP->bakeZ - epz;
			// Per-pixel plane distances (plane through sampleWorld, normal n).
			const float planeD = std::fabs((epx - swx)*nwx + (epy - swy)*nwy
			                             + (epz - swz)*nwz);
			const float optD   = std::fabs((envP->bakeX - swx)*nwx
			                             + (envP->bakeY - swy)*nwy
			                             + (envP->bakeZ - swz)*nwz);
			const float step = ux*nwx + uy*nwy + uz*nwz;
			const float uLen2 = ux*ux + uy*uy + uz*uz;
			trPulled = 0; trPlaneD = planeD; trOptD = optD; trStep = step;
			// Forward-path guard: skip when pullDir is within ~5.7deg of the
			// plane (step^2 <= 0.01*|u|^2) -- dividing by a tiny step blows
			// the pulled eye out and the reflections swing.
			if (planeD > optD + 1.0f && step*step > 0.01f * uLen2) {
				// x^0.8 via exp2/log2-free approx: x^0.75 = sqrt(x*sqrt(x))
				// (two Newton rsqrts, ~8 cycles vs ~30ns powf per pixel).
				// Slightly stronger damping than the authored 0.8 -- visually
				// equivalent for a stabilizer.
				const float d0 = planeD - optD;
				const float sq = d0 * fast_rsqrt(d0);            // sqrt(d0)
				const float t  = d0 * sq;                        // d0^1.5
				const float hackD = t * fast_rsqrt(t) + optD;    // d0^0.75 + optD
				float k = (hackD - planeD) / step;
				// The planeD gate is continuous (k -> 0 at the threshold), but
				// the step^2 guard is not: forward skips BINARY, and when the
				// per-pixel plane drifts across the boundary the whole facade
				// pops between pulled and physical in one frame (measured as
				// PULLGATE chatter in ENVTRACE). Ramp k to zero across
				// [1x..4x] of the guard threshold instead.
				const float g = step*step, gMin = 0.01f * uLen2;
				if (g < 4.0f * gMin)
					k *= (g - gMin) / (3.0f * gMin);
				epx += k * ux; epy += k * uy; epz += k * uz;
				trPulled = 1; trK = k;
			}
		}
		trEpX = epx; trEpY = epy; trEpZ = epz;
		float wx = swx - epx, wy = swy - epy, wz = swz - epz;
		const float wInv = fast_rsqrt(wx*wx + wy*wy + wz*wz + 1e-12f);
		wx *= wInv; wy *= wInv; wz *= wInv;
		const float wDotN = wx*nwx + wy*nwy + wz*nwz;
		// Store WORLD-space reflection in rv (the view->world rotate below
		// must be skipped for this path); dDotN keeps the Fresnel sign
		// convention (front-facing < 0).
		rvx = wx - 2.0f * wDotN * nwx;
		rvy = wy - 2.0f * wDotN * nwy;
		rvz = wz - 2.0f * wDotN * nwz;
		dDotN = wDotN;
		// Overwrite the view-space d with the world normal so ENVPROBE's
		// normal-probe substitution below still yields a world direction.
		dx = nwx; dy = nwy; dz = nwz;   // (only used by sProbe)
	} else {
		dDotN = dx*nx + dy*ny + dz*nz;
		rvx = dx - 2.0f * dDotN * nx;
		rvy = dy - 2.0f * dDotN * ny;
		rvz = dz - 2.0f * dDotN * nz;
	}
	// ENVPROBE=1: sample along the surface NORMAL instead of the
	// reflection — turns any env surface into a direction probe for
	// validating the pano/equirect/world mapping (diagnostic only).
	static const bool sProbe = std::getenv("ENVPROBE") != nullptr;
	// View → world rotation (viewToWorld is the transpose of the
	// camera rotation; direction ⇒ no translation).
	float rwx, rwy, rwz;
	if (envP->pullOpt > 0.0f) {
		// Pull path computed rv (and the probe normal, stashed in d) in
		// WORLD space already — no rotation here.
		rwx = sProbe ? dx : rvx; rwy = sProbe ? dy : rvy; rwz = sProbe ? dz : rvz;
	} else {
	const float pvx = sProbe ? nx : rvx, pvy = sProbe ? ny : rvy, pvz = sProbe ? nz : rvz;
	rwx = ctx.viewToWorld[0][0]*pvx + ctx.viewToWorld[0][1]*pvy + ctx.viewToWorld[0][2]*pvz;
	rwy = ctx.viewToWorld[1][0]*pvx + ctx.viewToWorld[1][1]*pvy + ctx.viewToWorld[1][2]*pvz;
	rwz = ctx.viewToWorld[2][0]*pvx + ctx.viewToWorld[2][1]*pvy + ctx.viewToWorld[2][2]*pvz;
	}
	// Parallax correction: exit-t of ray sampleWorld + t·R against
	// the AABB (slab method, per-axis far plane), hit point → the
	// lookup direction from the BAKE point. t ≤ 0 (pixel outside
	// the proxy) falls back to the uncorrected direction.
	static const bool sNoPara = std::getenv("ENVNOPARA") != nullptr;
	if (!sNoPara && !envP->noParallax) {
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
	// Sphere-proxy parallax for centroid-anchored DIRECTION-ONLY stores
	// (city: noParallax — the whole-city AABB proxy above is disabled for
	// them; docs/BACKLOG_PLANS.md #1). A mid-height bake can never index
	// what is LOW in the world by direction alone. Intersect the reflected
	// ray FROM THE PIXEL with a sphere of radius R around the bake point
	// and look up toward the hit: low pixels aim low (neighbor bases,
	// waterline), and a sphere is edge-free so the correction is
	// continuous everywhere — no AABB exit-face bands, no plane-horizon
	// tearing (both prior attempts failed exactly there). Exact sqrtf on
	// purpose: an approximate root here would re-introduce plateau
	// stepping into the reflection chain (the cee927d lesson).
	if (envP->noParallax) {
		const float sphR = fds::FeatureFlags::env_sphere_parallax();
		if (sphR > 0.0f) {
			const float px_ = sampleWorldX - envP->bakeX;
			const float py_ = sampleWorldY - envP->bakeY;
			const float pz_ = sampleWorldZ - envP->bakeZ;
			const float dd = rwx*rwx + rwy*rwy + rwz*rwz;
			const float dp = rwx*px_ + rwy*py_ + rwz*pz_;
			const float pp = px_*px_ + py_*py_ + pz_*pz_;
			const float R2 = sphR * sphR;
			const float disc = dp*dp - dd*(pp - R2);
			// Pixel inside the proxy (always true for sane R ≥ building
			// extents) → one positive root; direction stays unnormalized
			// (the cube lookup is scale-invariant).
			if (disc > 0.0f && pp < R2) {
				const float t = (-dp + std::sqrt(disc)) / dd;
				rwx = px_ + t * rwx;
				rwy = py_ + t * rwy;
				rwz = pz_ + t * rwz;
			}
		}
	}
	// Equirect lookup — the exact inverse of EnvBake's stitch
	// mapping: lon = atan2(-z, -x), lat = asin(y). Polynomial
	// approximations (SimdHelpers) — libm atan2f/asinf here were
	// the bulk of the env cost (~5ms on a cockpit-sized surface).
	// ENVFLIP=xyz axis-flip diagnostic (any subset, e.g. ENVFLIP=y).
	static const char* sFlip = std::getenv("ENVFLIP");
	if (sFlip) {
		if (std::strchr(sFlip, 'x')) rwx = -rwx;
		if (std::strchr(sFlip, 'y')) rwy = -rwy;
		if (std::strchr(sFlip, 'z')) rwz = -rwz;
	}
	// Live water (--env_live_water): rays that read the bake's WATER region
	// get the lookup direction tilted by the animated wave slope — the frozen
	// mirror content in the bake undulates in sync with the main-view ripple,
	// while the reflected skyline above the waterline stays put (the store's
	// baked coverage mask decides which is which). Inactive = one branch
	// inside the helper.
	// Direction → lookup coords. env_cube: trig-free dominant-axis face select
	// + gnomonic UV (EnvCube.h). Equirect: the atan2/asin panorama lookup
	// (inverse of EnvBake's stitch). ONE branch on the bake's mode, hoisted to
	// the same altitude the equirect math ran; ENVPROBE/ENVFLIP already applied
	// to rwx/rwy/rwz above, so both diagnostics work in cube mode too.
	const bool envIsCube = envP->isCube;
	float eu = 0.0f, evv = 0.0f;
	int   cubeFace = 0; float cubeU = 0.0f, cubeV = 0.0f;
	if (envIsCube) {
		// --env_live_water and the colour fetch BOTH start from the same
		// dominant-axis projection of the same UNPERTURBED direction (the
		// mask is read unperturbed by construction — a perturbed read would
		// be circular), so the pair cost two face selects and two divides for
		// one projection. Project once, hand the result to the mask, and
		// re-project ONLY when the tilt actually fires (then it is a genuinely
		// different direction). Bit-exact: EnvLiveWater_MaskAt's own
		// EnvCube_DirToFaceUV call is the identical expression on the
		// identical operands. Flag off / no mask / above the waterline = the
		// second projection never happens at all.
		fds::EnvCube_DirToFaceUV(rwx, rwy, rwz, cubeFace, cubeU, cubeV);
		const float lwW = EnvLiveWaterWeightAtFaceUV(
			envP->bakeY, rwy, envP->waterMask, envP->waterMaskRes,
			cubeFace, cubeU, cubeV);
		if (lwW > 0.0f) {
			fds::EnvLiveWater_TiltDir(envP->bakeX, envP->bakeY, envP->bakeZ,
			                          rwx, rwy, rwz, lwW);
			fds::EnvCube_DirToFaceUV(rwx, rwy, rwz, cubeFace, cubeU, cubeV);
		}
	} else {
		// Equirect stores keep the combined helper: their colour lookup is
		// atan2/asin, so there is no shared projection to hoist.
		fds::EnvLiveWater_PerturbDir(envP->bakeX, envP->bakeY, envP->bakeZ,
		                             rwx, rwy, rwz,
		                             envP->waterMask, envP->waterMaskRes);
		const float lon = atan2_approx(-rwz, -rwx);
		float sy_ = rwy; if (sy_ > 1.0f) sy_ = 1.0f; if (sy_ < -1.0f) sy_ = -1.0f;
		const float lat = asin_approx(sy_);
		eu = (lon + 1.57079632679f) * (1.0f / 6.28318530718f) + 0.5f;
		eu -= std::floor(eu);
		evv = 0.5f - lat * (1.0f / 3.14159265359f);
		if (evv < 0.0f) evv = 0.0f; if (evv > 0.9999f) evv = 0.9999f;
	}
	// Pre-filtered mip by per-pixel roughness (map texel, else the
	// gloss-derived roughness the --pbr path uses).
	float rough;
	if (roughMapOn && Mat->RoughnessMap && miplevel < Mat->RoughnessMap->numMipmaps
	    && Mat->RoughnessMap->Mipmap[miplevel]) {
		rough = float(reinterpret_cast<const byte*>(
			Mat->RoughnessMap->Mipmap[miplevel])[swizzledUV]) * (1.0f/255.0f);
	} else {
		rough = std::sqrt(2.0f / (gloss + 2.0f));
	}
	// Trilinear across the blur chain: nearest-level select on a
	// NOISY roughness map made adjacent pixels flip between sharp
	// and blurred mips (speckle). Lerp the two straddling levels.
	//
	// --env_mip_chain (§11 row E7, default 0 = unset = byte-null): the
	// DIVISOR here is the store's fixed 4-level chain, while the GPU arm's
	// is its full hardware chain — the same `rough` therefore selects a ~3x
	// wider lobe over there. The flag substitutes a VIRTUAL chain depth so
	// the CPU's effective lobe width can be matched; the result still clamps
	// to the real chain, so the reachable floor is faceRes>>(numMips-1).
	float lvlF = rough * float(envMipChainDepth(envP->numMips) - 1);
	if (lvlF < 0.0f) lvlF = 0.0f;
	if (lvlF > float(envP->numMips - 1)) lvlF = float(envP->numMips - 1);
	const int lvl0 = int(lvlF);
	const int lvl1 = lvl0 + 1 < envP->numMips ? lvl0 + 1 : lvl0;
	const float lf = lvlF - float(lvl0);
	// ENV_NOFETCH=1: constant color instead of the pano loads —
	// cost-attribution experiment (fetch-bound vs math-bound).
	static const bool sNoFetch = std::getenv("ENV_NOFETCH") != nullptr;
	float ecB, ecG, ecR;
	if (sNoFetch) {
		ecB = ecG = ecR = 128.0f;
	} else if (envIsCube) {
		// Face-major BILINEAR fetch: per-pixel reflected dirs sweep
		// continuously under camera motion, and nearest sampling made
		// high-frequency store content shimmer frame-to-frame (the city
		// per-pixel experiment's residual "jumpy"). Shared helper (also
		// used by the OuterVec 8-wide env front-end).
		auto fcBil = [&](int lvl, float& B, float& G, float& R) {
			EnvCubeFetchBil(envP, lvl, cubeFace, cubeU, cubeV, B, G, R);
		};
		float b0, g0, r0;
		fcBil(lvl0, b0, g0, r0);
		if (lvl1 != lvl0) {
			float b1, g1, r1;
			fcBil(lvl1, b1, g1, r1);
			ecB = b0 + lf * (b1 - b0);
			ecG = g0 + lf * (g1 - g0);
			ecR = r0 + lf * (r1 - r0);
		} else {
			ecB = b0; ecG = g0; ecR = r0;
		}
	} else {
		auto fq = [&](int lvl) -> uint32_t {
			const int lw = envP->W >> lvl, lh = envP->H >> lvl;
			const int epx = int(eu * float(lw)) % lw;
			const int epy_ = int(evv * float(lh));
			return envP->mip[lvl][size_t(epy_) * lw + epx];
		};
		const uint32_t c0 = fq(lvl0);
		const uint32_t c1 = lvl1 != lvl0 ? fq(lvl1) : c0;
		ecB = float(c0 & 0xFF)         + lf * (float(c1 & 0xFF)         - float(c0 & 0xFF));
		ecG = float((c0 >> 8) & 0xFF)  + lf * (float((c1 >> 8) & 0xFF)  - float((c0 >> 8) & 0xFF));
		ecR = float((c0 >> 16) & 0xFF) + lf * (float((c1 >> 16) & 0xFF) - float((c0 >> 16) & 0xFF));
	}
	// ── Short-range SCREEN-SPACE reflection (--env_ssr) ─────────────────
	// Self-reflections (a building's own ledges / wings) are absent from the
	// cube stores: the per-building bakes EXCLUDE the own building and are
	// centroid-anchored, so a building can never see itself. March the
	// reflected VIEW ray a few steps in screen space against the depth buffer;
	// on the first crossing, blend the PREVIOUS-FRAME color into ec* HERE —
	// before the Fresnel/roughness/metal weighting below — so the SSR hit
	// rides exactly the same energy law as the cube it replaces. The cube (+
	// sphere fake) stays as the fallback: SSR wins on hit and fades back to it
	// at every failure mode (edge exit, thickness reject, max steps), each via
	// a CONTINUOUS weight so the hit/miss boundary never seams. All math in
	// view space, scalar; the reflection is computed LOCALLY (the world-space
	// rv above may be cv-pulled). Exact div/sqrt only — an approximate rsqrt
	// without an NR step lurched reflections here once (cee927d); fast_rsqrt is
	// NR-corrected.
	{
		const int ssrSteps = fds::FeatureFlags::env_ssr();
		if (ssrSteps > 0 && g_ssrPrevColor.data() &&
		    g_ssrPrevW == ctx.xres && g_ssrPrevH == ctx.yres && z > 0.5f) {
			// Reflect the VIEW incident dir about the VIEW normal n (n was
			// already flipped to the camera side above → idn <= 0).
			const float dvi = fast_rsqrt(x*x + y*y + z*z);   // NR-corrected
			const float ivx = x * dvi, ivy = y * dvi, ivz = z * dvi;
			const float idn = ivx*nx + ivy*ny + ivz*nz;
			const float Rvx = ivx - 2.0f*idn*nx;
			const float Rvy = ivy - 2.0f*idn*ny;
			const float Rvz = ivz - 2.0f*idn*nz;
			// Skip when the reflected ray heads back toward the camera: its
			// screen projection immediately leaves the geometry (nothing to
			// march), and forward t would cross the near plane at once.
			if (Rvz > 1e-3f) {
				const float stride = fds::FeatureFlags::env_ssr_stride();
				const float thick  = fds::FeatureFlags::env_ssr_thick();
				const float invZ0  = 1.0f / z;
				// Screen-space projection gradient at t=0 → a constant view-
				// space dt so each step advances ~`stride` px. FOVX==1/invFOVX.
				const float gx = (Rvx*z - x*Rvz) * (invZ0*invZ0) / ctx.invFOVX;
				const float gy = (Rvy*z - y*Rvz) * (invZ0*invZ0) / ctx.invFOVY;
				const float gmag = std::sqrt(gx*gx + gy*gy);
				if (gmag > 1e-4f && thick > 1e-3f) {
					const float dt = stride / gmag;
					const float invThick = 1.0f / thick;
					const float bx = ctx.cntrEX, by = ctx.cntrEY;
					const int   W = ctx.xres, H = ctx.yres;
					const word* zp = ctx.zpage16;
					const dword* prev =
						reinterpret_cast<const dword*>(g_ssrPrevColor.data());
					constexpr float edge = 48.0f;   // border-fade width (px)
					float ssrB=0, ssrG=0, ssrR=0, ssrW=0;
					// Self-occlusion guard: start ~1.5*stride out so the ray
					// clears its own surface before the first depth test.
					for (int s = 0; s < ssrSteps; ++s) {
						const float t = (1.5f + float(s)) * dt;
						const float Sx = x + t*Rvx, Sy = y + t*Rvy, Sz = z + t*Rvz;
						if (Sz < 1.0f) break;                 // behind near plane → miss
						const float invSz = 1.0f / Sz;
						const float sxf = bx + Sx*invSz / ctx.invFOVX;
						const float syf = by - Sy*invSz / ctx.invFOVY;
						const int sxi = int(sxf), syi = int(syf);
						if (sxi < 0 || sxi >= W || syi < 0 || syi >= H) break; // edge exit → miss
						const word zEnc = zp[size_t(syi)*W + sxi];
						if (zEnc == 0) continue;              // sky sample — keep marching
						const float surfZ = float(0xFF80 - int(zEnc)) * ctx.invZScale;
						const float dz = Sz - surfZ;          // >0: ray is behind the surface
						if (dz > 0.0f && dz < thick) {
							// HIT. Three CONTINUOUS fades so every failure mode
							// (thickness edge, last step, screen border) drives
							// the contribution to zero without a hard switch.
							const float wThick = 1.0f - dz*invThick;                 // →0 at thickness edge
							const float wStep  = 1.0f - float(s)/float(ssrSteps);    // later hits → cube
							float wb = std::min(std::min(sxf, float(W-1)-sxf),
							                    std::min(syf, float(H-1)-syf)) * (1.0f/edge);
							if (wb < 0.0f) wb = 0.0f;
							if (wb > 1.0f) wb = 1.0f;
							const float w = wThick * wStep * wb;
							if (w > 0.0f) {
								const dword c = prev[size_t(syi)*W + sxi];
								ssrB = float( c        & 0xFF);
								ssrG = float((c >> 8)  & 0xFF);
								ssrR = float((c >> 16) & 0xFF);
								ssrW = w;
							}
							break;    // first crossing wins
						}
					}
					if (ssrW > 0.0f) {
						ecB += (ssrB - ecB) * ssrW;
						ecG += (ssrG - ecG) * ssrW;
						ecR += (ssrR - ecR) * ssrW;
					}
				}
			}
		}
	}
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
	float fres, ek;
	if (envBrdfAnalytic) {
		// Analytic split-sum env-BRDF (Karis "Mobile" approximation,
		// SIGGRAPH 2014): a polynomial fit to the pre-integrated GGX
		// environment BRDF LUT. Replaces the ad-hoc f90=1-rough Schlick
		// weight in the else-branch with the split-sum scale+bias
		// envBrdf = f0*A + B, so rough reflections carry the correct
		// roughness-dependent grazing response (energy-correct rough IBL)
		// instead of a hand-tuned Fresnel falloff. fres is set to envBrdf
		// so the ENVTRACE dump below and the metal-tinted sB/sG/sR add
		// (which read fres/ek) stay valid. f0 already = Reflection% pulled
		// toward ~0.98 by metalness above.
		const float rx = -1.0f*rough + 1.0f,    ry = -0.0275f*rough + 0.0425f;
		const float rz = -0.572f*rough + 1.04f, rw =  0.022f*rough + -0.04f;
		const float a004 = std::min(rx*rx, std::exp2(-9.28f*ndv))*rx + ry;
		const float A = -1.04f*a004 + rz,  B = 1.04f*a004 + rw;
		const float envBrdf = f0*A + B;
		fres = envBrdf;
		ek   = envBrdf * envReflGain;
		// Multi-scatter energy compensation (Fdez-Aguera 2019,
		// --pbr_multiscatter). The split-sum env-BRDF above is SINGLE-scatter
		// only — it drops the energy that repeated microfacet bounces would
		// return, so rough metals read too dark. Add it back from the SAME A,B
		// this branch already computed (a few ALU ops, no new gather): Ess=A+B
		// is the single-scatter energy (the split-sum integrated at F0=1); Favg
		// is Fresnel's hemispherical average; Fms is the multi-scatter
		// multiplier. Scales ONLY the specular energy (ek), by
		// 1 + Fms*(1-Ess)/Ess. fres (single-scatter, handed to --diffuse_energy)
		// is left untouched. No-op unless envBrdfAnalytic is on (needs A,B) — so
		// the flag is inert without --env_brdf_analytic, and OFF here is
		// byte-identical (ek unchanged). Ess in [~0.45,1], denom >= ~0.45 for
		// this A,B polynomial (F0<=1) → no divide guard needed.
		if (multiScatter) {
			const float Ess  = A + B;
			const float Favg = f0 + (1.0f - f0) * (1.0f / 21.0f);
			const float Fms  = Favg * Ess / (1.0f - Favg * (1.0f - Ess));
			ek *= 1.0f + Fms * (1.0f - Ess) / Ess;
		}
	} else {
		float f90 = 1.0f - rough;
		if (f90 < f0) f90 = f0;
		const float omv = 1.0f - ndv;
		const float omv2 = omv * omv;
		fres = f0 + (f90 - f0) * omv2 * omv2 * omv;
		ek   = fres * envReflGain;
	}
	// (1-F) diffuse energy conservation (--diffuse_energy): hand the caller
	// this exact per-pixel Fresnel so it can scale the diffuse accumulator by
	// (1-fres). Set only when requested; the caller inits its own to 0 so an
	// early return (sSkipNegY diagnostic, no env added) leaves diffuse at full.
	if (fresOut) *fresOut = fres;
	const float inv255 = 1.0f / 255.0f;
	// Metal tint: reflection takes the albedo's colour — i.e. the albedo is
	// used here as a REFLECTANCE, a linear multiplier on the probe radiance.
	//
	// --env_metal_tint_linear (docs/SHADING_CONTRACT.md §8 row S-d): texB is
	// the GAMMA 0-255 texel. The --hdr_linear composite squares the albedo
	// everywhere else (`aB*aB*lB`, :2708) and this lobe's output lands in sB,
	// which enters that composite unsquared — so a gamma reflectance in a
	// linear frame is a per-CHANNEL error, not a brightness one: it pulls the
	// channels together and DESATURATES the conductor. ON squares the
	// normalised albedo, matching --metal_spec_f0's treatment of the direct
	// lobe and the GPU oracle's `mix(1, S.baseColor, metal)` where
	// S.baseColor is `alb*alb`. Dielectrics (metalM == 0) are bit-unchanged.
	const float aBn = texB * inv255, aGn = texG * inv255, aRn = texR * inv255;
	const float rB = metalTintLinear ? aBn * aBn : aBn;
	const float rG = metalTintLinear ? aGn * aGn : aGn;
	const float rR = metalTintLinear ? aRn * aRn : aRn;
	const float tB = 1.0f - metalM + metalM * rB;
	const float tG = 1.0f - metalM + metalM * rG;
	const float tR = 1.0f - metalM + metalM * rR;
	sB += ecB * ek * tB;
	sG += ecG * ek * tG;
	sR += ecR * ek * tR;
	if (trWant) {
		std::fprintf(stderr,
		    "[ENVTRACE] inv=%u t=%.1f scr=(%.0f,%.0f) cam=(%.4f,%.4f,%.4f) "
		    "vp=(%.4f,%.4f,%.4f) sw=(%.4f,%.4f,%.4f) nV=(%.6f,%.6f,%.6f) "
		    "pull=%d planeD=%.4f optD=%.4f step=%.4f k=%.7f ep=(%.4f,%.4f,%.4f) "
		    "rw=(%.6f,%.6f,%.6f) face=%d fuv=(%.6f,%.6f) eq=(%.4f,%.4f) "
		    "lvlF=%.2f mip=%u suv=%05x ndv=%.3f fres=%.3f ec=(%.0f,%.0f,%.0f) "
		    "add=(%.1f,%.1f,%.1f) envP=%p bake=(%.0f,%.0f,%.0f)\n",
		    g_envTraceInvoke.load(std::memory_order_relaxed), double(Timer),
		    double(trScrX), double(trScrY),
		    double(ctx.cameraWorldX), double(ctx.cameraWorldY), double(ctx.cameraWorldZ),
		    double(x), double(y), double(z),
		    double(sampleWorldX), double(sampleWorldY), double(sampleWorldZ),
		    double(nx), double(ny), double(nz),
		    trPulled, double(trPlaneD), double(trOptD), double(trStep), double(trK),
		    double(trEpX), double(trEpY), double(trEpZ),
		    double(rwx), double(rwy), double(rwz),
		    cubeFace, double(cubeU), double(cubeV), double(eu), double(evv),
		    double(lvlF), miplevel, swizzledUV,
		    double(ndv), double(fres), double(ecB), double(ecG), double(ecR),
		    double(ecB * ek * tB), double(ecG * ek * tG), double(ecR * ek * tR),
		    (const void*)envP,
		    double(envP->bakeX), double(envP->bakeY), double(envP->bakeZ));
	}
}

// --deferred_checker_edge_full (docs/SHADING_CONTRACT.md §12).
//
// THE SHARED PREDICATE. The wave-2 fill averages a dropped cell from its
// already-shaded neighbours when they are compatible (same matID + similar
// normal + similar Z); when NO neighbour is compatible it falls through to its
// own REDUCED scalar re-shade (no --pbr GGX lobe, no shadow term of any kind,
// no AO, no normal-map LOD fade, no --hdr_metal_kill; and it applies the spot
// cone to specular where wave 1 does not). That is the SAME defect
// --deferred_checker_env_full closed for env-reflective pixels, on a different
// pixel set: every material / normal / depth EDGE, i.e. silhouettes.
//
// The predicate reads ONLY G-buffer + ZPage state, so wave 1 can evaluate it
// exactly as the fill will. One function, two call sites — they cannot drift.
// Returns true iff the fill would take its full-shade fallback here.
static inline bool fillFallsBackHere(const meka::GBuffer &gb, const word *ZPage16,
                                     int XRes, int YRes, int px, int py,
                                     bool quarter,
                                     bool normalCheck, float normalCos,
                                     bool zCheck, float zJump)
{
	const size_t i = size_t(py) * XRes + px;
	const word zEnc = ZPage16[i];
	if (zEnc == 0) return false;              // fill skips it entirely
	const uint32_t matIDc = (gb.txtr[i] >> 20) & 0xFF;
	// Centre normal decoded LAZILY — the matID compare rejects most neighbours
	// first, and on a continuous surface the very first neighbour passes, so
	// paying the oct decode up front would tax every dropped cell in the frame.
	float ncX = 0, ncY = 0, ncZ = 0;
	bool  ncDone = false;
	// Mirrors TileFill's neighborCompatible() term for term.
	auto compatible = [&](size_t ni) -> bool {
		if (((gb.txtr[ni] >> 20) & 0xFF) != matIDc) return false;
		if (normalCheck) {
			if (!ncDone) { meka::oct_decode_u32(gb.normal[i], ncX, ncY, ncZ); ncDone = true; }
			float nx, ny, nz;
			meka::oct_decode_u32(gb.normal[ni], nx, ny, nz);
			if ((ncX*nx + ncY*ny + ncZ*nz) < normalCos) return false;
		}
		if (zCheck) {
			const word zN = ZPage16[ni];
			if (zN == 0) return false;
			const int diff = int(zN) - int(zEnc);
			const int absDiff = diff < 0 ? -diff : diff;
			if (float(absDiff) > zJump * float(zEnc)) return false;
		}
		return true;
	};
	if (quarter) {
		const bool odd_x = (px & 1) != 0, odd_y = (py & 1) != 0;
		if (odd_x && !odd_y) {
			if (px > 0        && compatible(i - 1)) return false;
			if (px < XRes - 1 && compatible(i + 1)) return false;
		} else if (!odd_x && odd_y) {
			if (py > 0        && compatible(i - size_t(XRes))) return false;
			if (py < YRes - 1 && compatible(i + size_t(XRes))) return false;
		} else {
			if (px > 0 && py > 0               && compatible(i - size_t(XRes) - 1)) return false;
			if (px < XRes - 1 && py > 0        && compatible(i - size_t(XRes) + 1)) return false;
			if (px > 0 && py < YRes - 1        && compatible(i + size_t(XRes) - 1)) return false;
			if (px < XRes - 1 && py < YRes - 1 && compatible(i + size_t(XRes) + 1)) return false;
		}
	} else {
		if (px > 0        && compatible(i - 1)) return false;
		if (px < XRes - 1 && compatible(i + 1)) return false;
	}
	return true;
}

// ─── Ablation ladder for the deferred OMNI LOOP (the scalar per-light body) ──
// Same instrument shape as FDS_CONE_ABLATE in DeferredVolumetric.cpp, aimed at
// the other ~100 %-self monolith: the per-light loop inside
// Render_DeferredLighting_Tile, which is what greets/fountain/chase actually
// run (PreferOuterVec is off for them; deferred_vec is off on arm64).
// COMPILE-TIME only: build the TU with -DFDS_OMNI_ABLATE=n and the per-light
// loop `continue`s at staged depth n, so the Ginstr/f difference between two
// builds prices exactly one stage. n=0 (shipping) emits literally nothing.
//
//    1  cut at the top of the per-light loop      -> per-pixel loop floor
//    2  keep + the mirrorId test
//    3  keep + light vector w, N·L dot, dot<0 reject
//    4  keep + len2 and the range reject
//    5  keep + the mirror-bounce window portal test
//    6  keep + rsqrt/dist/attenuation k
//    7  keep + the spot-cone test and its smoothstep
//    8  keep + computeMapShadowAtten (2-D spot maps, clone src map/cube)
//    9  keep + resolveCubeAtten (the cube tap / shadow lightmap)
//   10  keep + the POM relief-horizon term
//   11  keep + the DIFFUSE accumulate  (so full - 11 = the specular lobe)
//    0  full loop
//
// The cut sinks the values it retains into a per-TILE scalar accumulator that
// drains to one volatile store per tile call — otherwise the retained work is
// dead and the compiler deletes exactly what we are trying to price. Sink cost
// is 1-2 instructions per (pixel x light); the shipping build pays none.
#ifndef FDS_OMNI_ABLATE
#define FDS_OMNI_ABLATE 0
#endif
#ifndef FDS_PIX_ABLATE
#define FDS_PIX_ABLATE 0
#endif
#if FDS_OMNI_ABLATE || FDS_PIX_ABLATE
#define FDS_ABL_SINK 1
static volatile float g_omniAblSink = 0.0f;
#else
#define FDS_ABL_SINK 0
#endif
#if FDS_OMNI_ABLATE
// NB: no do/while(0) wrapper — the `continue` has to reach the per-light `for`.
#define OMNI_ABL_CUT(stage, expr) \
    if constexpr ((FDS_OMNI_ABLATE) == (stage)) { ablSink += (expr); continue; }
#define OMNI_ABL_CUT_BARE(stage) \
    if constexpr ((FDS_OMNI_ABLATE) == (stage)) { continue; }
#else
#define OMNI_ABL_CUT(stage, expr)  ((void)0)
#define OMNI_ABL_CUT_BARE(stage)   ((void)0)
#endif

// ─── Ablation ladder for the PER-PIXEL BODY (the omni loop's "floor") ────────
// Same shape, aimed at the 0.473 Gi/f the omni ladder's stage 1 leaves behind:
// everything the pixel body does BEFORE the per-light loop, plus the compose
// after it. Cuts `continue` the PIXEL loop, so stage n = the cost of everything
// above cut n (the light loop and the compose are cut away with it).
//    1  z alive test               2  + mirrorId, mat32 decode, shadowMatID
//    3  + albedo fetch + tint      4  + the static-lightmap ADDRESS resolve
//    5  + normal decode / TBN      6  + view pos + SH ambient
//    7  + view dir + PBR consts    8  + AO fetch
//    9  + sample world position   10  + POM horizon record
//    0  full (light loop + compose)
#if FDS_PIX_ABLATE
// The sink is CUMULATIVE: each boundary adds its own outputs to a running
// per-pixel accumulator and the cut sinks that, so a late cut cannot let the
// compiler dead-strip an early stage's work. Costs one fadd per boundary
// crossed, which is why every stage is quoted against the same ladder.
#define PIX_ABL_CUT(stage, expr) \
    ablKeep += (expr); \
    if constexpr ((FDS_PIX_ABLATE) == (stage)) { ablSink += ablKeep; continue; }
#else
#define PIX_ABL_CUT(stage, expr)  ((void)0)
#endif

// ─── Ablation ladder for the WAVE-2 CHECKERBOARD FILL (lighting-w2) ──────────
// Same cumulative-sink shape as PIX_ABL_CUT, aimed at
// Render_DeferredLighting_TileFill's per-cell body. Cuts `continue` the pixel
// loop, so stage n prices everything above cut n.
//    1  parity test only (the w2 loop skeleton)
//    2  + z alive test
//    3  + mirrorId, mat32, forward sentinel, matIDc
//    4  + envForceFull / checker_env_full drop
//    5  + centre normal decode
//    6  + neighbour index setup
//    7  + centre fetchTexel (haveOwn)
//    8  + the neighbour compatibility + accumulate loop
//    9  + the average write-out (so full - 9 = the FULL-SHADE FALLBACK)
//    0  full
#ifndef FDS_W2_ABLATE
#define FDS_W2_ABLATE 0
#endif
#if FDS_W2_ABLATE
static volatile float g_w2AblSink = 0.0f;
#define W2_ABL_CUT(stage, expr) \
    w2Keep += (expr); \
    if constexpr ((FDS_W2_ABLATE) == (stage)) { w2Sink += w2Keep; continue; }
#else
#define W2_ABL_CUT(stage, expr)  ((void)0)
#endif

// ─── Ablation for WAVE 1's 8-bit LDR compose chain (lighting-w1) ────────────
// Prices the `fdB/fdG/fdR` → `outB/outG/outR` → `out[i]` chain in
// Render_DeferredLighting_Tile, which under `--hdr --hdr-linear` feeds only the
// VPage (the linear radiance `rlB` at the HDR store is built from `dlB`, the raw
// light accumulator, never from `fdB`). Unlike the PIX/OMNI ladders this is a
// REMOVAL, not a `continue` — everything above the compose stays live because
// the HDR store still consumes texB/lB/sB.
//    1  force `ldrSkip` on: the whole chain (fd and its metal + diffuse-energy
//       scalings, out*, hB/hG/hR, the isWater gamma blend, the viz stomps, the
//       8-bit clamps and the `out[i]` store) with NO flag read and NO predicate,
//       so the number is the chain alone. Byte-CHANGING under any arm but
//       --hdr --hdr-linear; a cost instrument, never a ship configuration.
// Measured c0ccb40d: 1.570 -> 1.537 Gi/f at t=5743 (-2.10 %), 1.457 -> 1.425 at
// t=2845 (-2.20 %). A stage 2 (tail only, arithmetic kept alive by a sink) was
// tried and read ABOVE stage 0 — its sink cost more than the tail — so it is
// gone and no split of the 0.033 is quotable.
#ifndef FDS_W1LDR_ABLATE
#define FDS_W1LDR_ABLATE 0
#endif

// ─── Wave-2 fill census — where each odd cell goes ───────────────────────────
// Build with -DFDS_W2_CENSUS=ON; runtime gate --omni_census (shared).
#ifndef FDS_W2_CENSUS
#define FDS_W2_CENSUS 0
#endif
// [0] cells entered (parity passed), [1] z==0, [2] forward sentinel,
// [3] env-force-full early drop, [4] no neighbour (border), [5] AVERAGED,
// [6] FULL-SHADE FALLBACK, [7] haveOwn, [8] compatible neighbours,
// [9] sharp-usable neighbours, [10] fallback bailed (no material/texture),
// [11] env-reflective cells seen, [12] edge-full drops.
static constexpr int W2_CEN_N = 16;
#if FDS_W2_CENSUS
static std::atomic<uint64_t> g_w2Cen[W2_CEN_N];
static void W2Census_Report()
{
	uint64_t c[W2_CEN_N];
	for (int i = 0; i < W2_CEN_N; ++i) c[i] = g_w2Cen[i].load(std::memory_order_relaxed);
	if (c[0] == 0) return;
	const double e = double(c[0]);
	std::fprintf(stderr,
	    "[W2-CENSUS] cells %.0f  z0 %.2f%%  sentinel %.2f%%  envdrop %.2f%%  border %.2f%%  "
	    "AVERAGED %.0f (%.2f%%)  FULLSHADE %.0f (%.2f%%)  bailed %.0f\n"
	    "[W2-CENSUS]   haveOwn %.2f%% of entered; compatible neighbours %.3f/averaged; "
	    "sharp %.3f/averaged; env cells %.0f; edgefull drops %.0f\n",
	    e, 100.0*double(c[1])/e, 100.0*double(c[2])/e, 100.0*double(c[3])/e,
	    100.0*double(c[4])/e, double(c[5]), 100.0*double(c[5])/e,
	    double(c[6]), 100.0*double(c[6])/e, double(c[10]),
	    100.0*double(c[7])/e,
	    double(c[8])/double(c[5] ? c[5] : 1), double(c[9])/double(c[5] ? c[5] : 1),
	    double(c[11]), double(c[12]));
	std::fprintf(stderr,
	    "[W2-CENSUS]   neighbour normal checks %.0f\n", double(c[13]));
	for (int i = 0; i < W2_CEN_N; ++i) g_w2Cen[i].store(0, std::memory_order_relaxed);
}
#define W2_CEN(idx) do { ++w2cn[(idx)]; } while (0)
#define W2_CEN_ADD(idx, v) do { w2cn[(idx)] += uint64_t(v); } while (0)
#else
#define W2_CEN(idx)         ((void)0)
#define W2_CEN_ADD(idx, v)  ((void)0)
#endif

// ─── Per-PIXEL live-light census — the companion instrument to the ladder ────
// The shadow-tap census (43ac3456) counts per (tile x light). This counts per
// (pixel x light): where in the reject chain each of a tile's lights dies, and
// the histogram of how many lights actually reach the accumulate for a pixel.
// Build with -DFDS_OMNI_CENSUS=ON; runtime gate --omni_census.
#ifndef FDS_OMNI_CENSUS
#define FDS_OMNI_CENSUS 0
#endif
// [0] pairs entered, [1] mirrorId reject, [2] dot<0 reject, [3] range reject,
// [4] portal reject, [5] cone reject, [6] map-shadow reject, [7] cube reject,
// [8] horizon reject, [9] reached the accumulate, [10] shaded pixels,
// [11..11+N] live-light histogram (clamped at the last bucket).
// Sizes live OUTSIDE the #if so the per-tile counter array declares either way
// and the hooks dead-strip on `constexpr bool omniCensus = false` instead of
// needing an #if at every one of the ten call sites.
// [44] pairs reaching computeMapShadowAtten, [45] .. with smIdx>=0,
// [46] .. with srcShadowMapIdx>=0, [47] .. with srcCubeShadowIdx>=0,
// [48] .. with NONE of those three (the call can only return 1.0f),
// [49] pairs reaching the cube stage with cubeShadowIdx>=0.
static constexpr int OMNI_CEN_HIST = 33;
static constexpr int OMNI_CEN_BR   = 11 + OMNI_CEN_HIST;   // = 44
static constexpr int OMNI_CEN_N    = OMNI_CEN_BR + 6;
#if FDS_OMNI_CENSUS
static std::atomic<uint64_t> g_omniCen[OMNI_CEN_N];
void OmniCensus_Report()
{
	uint64_t c[OMNI_CEN_N];
	for (int i = 0; i < OMNI_CEN_N; ++i) c[i] = g_omniCen[i].load(std::memory_order_relaxed);
	if (c[0] == 0) return;
	const double px = double(c[10] ? c[10] : 1);
	const double pr = double(c[0]);
	std::fprintf(stderr,
	    "[OMNI-CENSUS] shaded px %.3f M, (px x light) pairs %.3f M = %.2f lights/px entered\n"
	    "[OMNI-CENSUS]   mirrorId %.2f%%  N.L<0 %.2f%%  range %.2f%%  portal %.2f%%  cone %.2f%%  "
	    "mapshadow %.2f%%  cube %.2f%%  horizon %.2f%%  ACCUMULATED %.2f%% (%.2f lights/px)\n",
	    double(c[10])/1e6, pr/1e6, pr/px,
	    100.0*double(c[1])/pr, 100.0*double(c[2])/pr, 100.0*double(c[3])/pr,
	    100.0*double(c[4])/pr, 100.0*double(c[5])/pr, 100.0*double(c[6])/pr,
	    100.0*double(c[7])/pr, 100.0*double(c[8])/pr,
	    100.0*double(c[9])/pr, double(c[9])/px);
	std::fprintf(stderr, "[OMNI-CENSUS]   live-lights/pixel histogram (%% of shaded px):");
	for (int b = 0; b < OMNI_CEN_HIST; ++b) {
		const uint64_t v = c[11 + b];
		if (v == 0) continue;
		std::fprintf(stderr, " %d:%.2f%%", b, 100.0*double(v)/px);
	}
	std::fprintf(stderr, "\n");
	const double mp = double(c[OMNI_CEN_BR] ? c[OMNI_CEN_BR] : 1);
	std::fprintf(stderr,
	    "[OMNI-CENSUS]   computeMapShadowAtten calls %.3f M: smIdx %.2f%%  srcSm %.2f%%  "
	    "srcCube %.2f%%  NONE-OF-THREE %.2f%% (returns 1.0f for free)\n"
	    "[OMNI-CENSUS]   cube-stage pairs with cubeShadowIdx>=0: %.3f M\n",
	    mp/1e6, 100.0*double(c[OMNI_CEN_BR+1])/mp, 100.0*double(c[OMNI_CEN_BR+2])/mp,
	    100.0*double(c[OMNI_CEN_BR+3])/mp, 100.0*double(c[OMNI_CEN_BR+4])/mp,
	    double(c[OMNI_CEN_BR+5])/1e6);
	for (int i = 0; i < OMNI_CEN_N; ++i) g_omniCen[i].store(0, std::memory_order_relaxed);
}
#else
void OmniCensus_Report() {}
#endif

// --pom_horizon's per-pixel relief record, gathered so the pixel body carries
// ONE pointer instead of seven locals across the per-light loop.
struct HzFrame { const unsigned char *texel; float tx, ty, tz, bx, by, bz; };

static void Render_DeferredLighting_Tile(const DeferredLightingCtx &ctx,
                                          int tileIndex,
                                          int x1, int y1, int x2, int y2)
{
#if FDS_ABL_SINK
	float ablSink = 0.0f;
#endif
	// Render-target addressing from ctx, not globals (RenderContext
	// migration). Locals shadow the engine globals so the body is untouched.
	const int XRes = ctx.xres;
	byte *const VPage = ctx.vpage;
	word *const ZPage16 = ctx.zpage16;
	const float CntrEX = ctx.cntrEX, CntrEY = ctx.cntrEY;
	const meka::GBuffer &gb = *ctx.gb;
	dword *out = reinterpret_cast<dword *>(VPage);
	// Hoist mode/global queries once per tile — a `getenv()`-backed
	// cached bool still costs a function call + load + test that the
	// compiler can't see through the std::function/lambda boundary.
	// 2M function calls/frame adds up.
	const bool quarter        = deferredLightingQuarterEnabled();
	const bool checker        = deferredLightingCheckerboardEnabled() && !quarter;
	// --deferred_checker_env_full: see the drop test in the pixel loop.
	const bool checkerEnvFull = (checker || quarter)
	    && fds::FeatureFlags::deferred_checker_env_full();
	// --deferred_checker_edge_full: same defect as the env row, on the fill's
	// OTHER full-shade trigger — the material/normal/Z EDGE. Predicate below.
	const bool checkerEdgeFull = (checker || quarter)
	    && fds::FeatureFlags::deferred_checker_edge_full();
	const float w1QNormalCos   = checkerEdgeFull ? fds::FeatureFlags::quarter_normal_cos() : 0.0f;
	const bool  w1QNormalCheck = checkerEdgeFull && w1QNormalCos > 0.0f;
	const float w1QZJump       = checkerEdgeFull ? fds::FeatureFlags::quarter_z_jump() : 0.0f;
	const bool  w1QZCheck      = checkerEdgeFull && w1QZJump > 0.0f;
	const bool useVec         = deferredLightingVecEnabled();
	const bool sVecForce      = fds::FeatureFlags::deferred_vec_force();
	const bool specGlobalOn   = Specular_Factor > 0.0f;
	const bool profShadowCache = fds::FeatureFlags::shadow_prof_cache();
	// HDR Phase 3 B1: route the opaque lit radiance into g_hdrBuf UNCLAMPED so
	// bright surfaces bloom/roll-off at the tonemap instead of clipping at the
	// 8-bit VPage. The composite reads this back (coverage flag in h[3]) rather
	// than lifting the already-clamped VPage. Gated on hdr(): when off, no write,
	// LDR byte-identical; when on but the froxel composite doesn't run, g_hdrActive
	// stays false and the tonemap no-ops, so the buffer is harmlessly ignored.
	// ctx.hdrBuf is THIS pass's radiance target (null = no HDR write). It used
	// to be `Hdr_WritableFor(ctx.xres, ctx.yres)` — "g_hdrBuf happens to be
	// sized like me" standing in for "am I the main pass?" — which silently sent
	// every offscreen bake at another resolution down the LDR combine, onto a
	// different transfer function from the frame it feeds. The orchestrator now
	// hands each pass its own buffer (main: g_hdrBuf under exactly the old
	// predicate; shard bake: the calling worker's own).
	const bool hdrWrite = fds::FeatureFlags::hdr() && ctx.hdrBuf != nullptr;
	// HDR Phase 3 B2: when --hdr_linear, do the lighting math in LINEAR space —
	// square the (normalized) albedo and let light enter at power 1 (B1/gamma
	// effectively squares the light too). Store the result re-encoded to gamma
	// (sqrt) so the buffer stays gamma-coherent with the composite + overlays +
	// the gamma tonemap-decode that --hdr_linear already applies; the decode
	// recovers the true linear radiance. Contained to this write — no composite/
	// overlay/tonemap change. Off → B1 gamma radiance.
	const bool hdrLinear = hdrWrite && fds::FeatureFlags::hdr_linear();
	// --deferred_shade_ldr_skip: wave 1's 8-bit VPage compose — fdB/fdG/fdR,
	// outB/outG/outR, the isWater gamma blend, the debug-viz stomps, the clamps
	// and the `out[i]` store — writes a value nothing reads when BOTH hold:
	//   * `ctx.ldrDiscarded` — renderFrame's promise, built from the same three
	//     gates the tonemap sits behind. Render_TonemapToVPage rewrites EVERY
	//     VPage pixel from hdrBuf unconditionally (Hdr.cpp: no coverage test in
	//     the tile body), so a VPage write issued before it is discarded. Same
	//     promise as the wave-2 sibling --deferred_fill_ldr_skip; `hdrWrite`
	//     alone would be a BUG — see that flag's text for CITY.CPP:3823.
	//   * `hdrLinear` — this is the term wave 2 does NOT need. Under
	//     --hdr_linear the shipped radiance `rlB` is built from `dlB`, the raw
	//     light accumulator, and never from `fdB`. Under plain --hdr (B1 gamma)
	//     `hB = fdB + sB` IS the radiance: the chain is live and must not be
	//     skipped.
	// The debug-viz stomps write out* and nothing else, so any of them forces
	// the chain back on (constexpr-false unless FDS_DEV).
#if FDS_DEV
	const bool vizAnyTile = fds::FeatureFlags::viz_tangent() || fds::FeatureFlags::viz_normal()
	    || fds::FeatureFlags::viz_geonormal() || fds::FeatureFlags::viz_matid()
	    || fds::FeatureFlags::viz_pmid();
#else
	constexpr bool vizAnyTile = false;
#endif
#if FDS_W1LDR_ABLATE == 1
	// Instrument (see the FDS_W1LDR_ABLATE block above): force the skip on
	// regardless of the predicate, to price the chain with no flag read and no
	// predicate at all. Byte-CHANGING under any arm but --hdr --hdr-linear.
	constexpr bool ldrSkip = true;
#else
	const bool ldrSkip = fds::FeatureFlags::deferred_shade_ldr_skip()
	    && hdrLinear && ctx.ldrDiscarded && !vizAnyTile;
#endif

	// Per-stage ablation gates. Set one of these on to short-circuit the
	// stage so a bench harness can measure its cost from the frame-time
	// delta (see `--bench=scene@...`).
	const bool profNoTex    = fds::FeatureFlags::prof_no_tex();
	// Texture filtering (FDS_TEXTURE_FILTER > 0): the Mekalele raster pass
	// wrote a filtered BGRA into gb.albedo; read that instead of point-
	// sampling the packed texel address. Metal/rough/AO/normal maps keep
	// using the suv address decoded from `txtr`.
	// --poly_viz rides the same plane (the rasterizer writes its per-triangle
	// ownership colour there), so it must be read here too or the viz is inert.
	const bool texFilterOn  = (fds::FeatureFlags::texture_filter() > 0
	                           || fds::FeatureFlags::poly_viz())
	                          && !gb.albedo.empty();
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
	// S4b: move the AO multiply from ambient-only to the final combined diffuse.
	const bool  aoDirectG       = fds::FeatureFlags::ao_direct();
	const float aoDirectStrG    = fds::FeatureFlags::ao_direct_strength();
	const bool nmapDisabledG    = fds::FeatureFlags::no_nmap();
	// S1c relief self-shadow (--pom_horizon). hzInvSoft2 turns the softness
	// half-width into the smoothstep's 1/(hi-lo) so the inner loop needs one
	// multiply-add; a softness of 0 degenerates to a hard step (huge slope).
	const bool  pomHorizonOnG   = fds::FeatureFlags::pom_horizon();
	const bool  pomHorizonVizG  = pomHorizonOnG && fds::FeatureFlags::pom_horizon_viz();
	const float hzStrengthG     = fds::FeatureFlags::pom_horizon_strength();
	const float hzSoftG         = fds::FeatureFlags::pom_horizon_soft();
	const float hzInvSoft2      = hzSoftG > 1e-4f ? (0.5f / hzSoftG) : 1e4f;
	const bool deferredNoSpecG  = fds::FeatureFlags::deferred_no_spec();
	const bool sPbr             = fds::FeatureFlags::pbr();
	const float sPbrRoughFixed  = fds::FeatureFlags::pbr_roughness();
	// --metal_spec_f0: a conductor's DIRECT-lobe F0 is its albedo, not the
	// dielectric 0.04. Off = byte-null (the scalar Schlick below is untouched).
	const bool metalSpecF0G     = fds::FeatureFlags::metal_spec_f0();
	// Env-specular reflection (--env_refl): per-SURFACE baked panoramas,
	// matID-indexed (each reflective material's pano is captured from its own
	// centroid; the lookup is parallax-corrected against the scene AABB).
	const fds::EnvPanoLinear *const *envTabG = fds::FeatureFlags::env_refl()
		? fds::EnvReflection_Table(ctx.Sc) : nullptr;
	// Raw gain: the material's Reflection% now enters through Fresnel F0
	// (F0 = Reflection/100), so the gain is a plain multiplier on top.
	const float envReflGainG    = fds::FeatureFlags::env_refl_gain();
	const bool  envBrdfAnalyticG = fds::FeatureFlags::env_brdf_analytic();
	const bool  multiScatterG   = fds::FeatureFlags::pbr_multiscatter();
	// --env_metal_tint_linear: the ENV lobe's conductor tint as a LINEAR
	// reflectance (contract §8 row S-d), active only in a LINEAR frame —
	// see MetalTintLinearActive.
	const bool  metalTintLinG   = MetalTintLinearActive(ctx);
	// --shadow_noncaster_depth: PolyId's identity test is unsatisfiable for a
	// receiver that is excluded from the CASTER set. Default OFF = byte-null.
	const bool  noncasterDepthG = fds::FeatureFlags::shadow_noncaster_depth();
	const bool  metalMapOnG     = fds::FeatureFlags::metal_map();
	const bool  diffuseEnergyG  = fds::FeatureFlags::diffuse_energy();
	// --sh_ambient: per-scene L2 SH irradiance coefficients (null = flag off /
	// not yet baked → the flat Sc->Ambient path below runs byte-identically).
	const float* shCoefG        = fds::FeatureFlags::sh_ambient()
		? fds::SHAmbient_Coeffs(ctx.Sc) : nullptr;
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
	// --deferred_lm_addr_skip / --deferred_cube_direct:
	// hoisted here so the per-pixel body reads a register, not the flag array.
	const bool lmAddrSkip       = fds::FeatureFlags::deferred_lm_addr_skip();
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
	// --deferred_cube_direct: the tile half of the predicate. resolveCubeAtten
	// reduces to one CubeShadow_Sample call — same arguments, no bias math —
	// exactly when the lightmap arm is unreachable (lmKernelEnabled off), the
	// tap is not short-circuited, and the mode is PolyId. The per-pixel half is
	// `surfaceShadowId >= 0`, which is the same test the wrapper makes.
	const bool cubeDirectTile = fds::FeatureFlags::deferred_cube_direct()
	    && !lmKernelEnabled
	    && !caFlags.profNoCubeTap
	    && caFlags.shadowMode == ShadowMode::PolyId;

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

	// ── --shadow_tap_census: the CEILING measurement for per-tile shadow
	// early-outs (docs/OPTIMIZATION_BACKLOG.md, the parked "per-tile
	// light/shadow early-out" item). Default OFF, changes no pixel.
	//
	// Two numbers, both per (tile × light-slot), aggregated over the run:
	//   (1) `inRange` — pixels of this tile for which this light survived the
	//       per-pixel mirror/dot/range gate. An entry with inRange == 0 is a
	//       light the tile-list builder admitted that CANNOT light one pixel
	//       of the tile: pure per-pixel loop-prologue waste, and its removal
	//       is byte-null by construction. This is variant 1's exact ceiling.
	//   (2) `taps` / `lit` / `shad` — the cube-shadow taps this entry ran and
	//       how they resolved (1.0 / 0.0 / partial). An entry whose taps are
	//       ALL lit or ALL shadowed could have been answered once per tile;
	//       Σtaps over those entries is variant 2's exact ceiling, measured
	//       with a perfect oracle rather than a corner-test approximation.
	// Per-tile stack arrays → no cross-thread race (one worker per tile);
	// atomics aggregate at the end, same shape as FDS_CONTRIB_CULL above.
	// Scalar path only — which is all of greets (nmap forces scalar).
	// Main view only: the offscreen bakes (env cube faces, mirror shards, RTT
	// slots) dispatch the SAME 96-tile grid over a 64x64 or 256x256 target, so
	// counting them would let hundreds of ~4000-px "frames" swamp the ~1.3 Mpx
	// one we are measuring. 640 px wide is far above every bake target and far
	// below every shipping main resolution.
#if FDS_SHADOW_TAP_CENSUS
	const bool tapCensus = fds::FeatureFlags::shadow_tap_census() && ctx.xres >= 640;
#else
	// Hooks compiled out (CMake option FDS_SHADOW_TAP_CENSUS, default OFF) —
	// see the comment on that option for the 1 % that buys back. `constexpr`
	// so every census site below is dead-stripped, not merely never taken.
	constexpr bool tapCensus = false;
	if (fds::FeatureFlags::shadow_tap_census()) {
		static std::atomic<int> warned{0};
		if (warned.fetch_add(1, std::memory_order_relaxed) == 0)
			std::fprintf(stderr, "[SHADOW-TAP-CENSUS] this binary was built without the "
			    "hooks. Rebuild with: cmake -S . -B build-census -G Ninja "
			    "-DFDS_SHADOW_TAP_CENSUS=ON\n");
	}
#endif
	// ONE base pointer, not four: as four separate arrays the hot loop kept
	// four extra stack bases live and cost 0.9 % of lighting-w1's instructions
	// with the census OFF. Lane order per light: 0 inRange, 1 taps, 2 lit, 3 shadowed.
	alignas(64) uint32_t tcL[DEFERRED_MAX_LIGHTS][4];
	const int tcN = tapCensus
	    ? std::min(ctx.tileLights[tileIndex].count, int(DEFERRED_MAX_LIGHTS)) : 0;
	for (int ci = 0; ci < tcN; ++ci) {
		tcL[ci][0] = 0; tcL[ci][1] = 0; tcL[ci][2] = 0; tcL[ci][3] = 0;
	}
	uint32_t tcPixels = 0;
	// --shadow_tap_census_block=B: the same uniformity question asked per BxB
	// pixel block instead of per tile. Heap-backed and per-thread so the block
	// count can go as fine as B=4 without a 300 KB stack frame; only the
	// (blocks x lights) prefix actually in use is cleared per tile.
	const int tcB       = tapCensus ? std::max(0, fds::FeatureFlags::shadow_tap_census_block()) : 0;
	const int tcBlocksX = tcB > 0 ? ((x2 - x1) + tcB - 1) / tcB : 0;
	const int tcBlocksY = tcB > 0 ? ((y2 - y1) + tcB - 1) / tcB : 0;
	const int tcBlocks  = tcBlocksX * tcBlocksY;
	static thread_local std::vector<uint32_t> tcBlkBuf;
	uint32_t *tcBlk = nullptr;
	if (tcBlocks > 0 && tcN > 0) {
		const size_t need = size_t(tcBlocks) * size_t(tcN) * 3u;
		if (tcBlkBuf.size() < need) tcBlkBuf.resize(need);
		tcBlk = tcBlkBuf.data();
		std::memset(tcBlk, 0, need * sizeof(uint32_t));
	}
#if FDS_OMNI_CENSUS
	// Tile-local counters; one atomic drain per tile call. Same reason the tap
	// census keeps tcL on the stack — an atomic in the innermost body is not a
	// measurement, it is a different program.
	const bool omniCensus = fds::FeatureFlags::omni_census() && ctx.xres >= 640;
#else
	constexpr bool omniCensus = false;
	if (fds::FeatureFlags::omni_census()) {
		static std::atomic<int> warnedOc{0};
		if (warnedOc.fetch_add(1, std::memory_order_relaxed) == 0)
			std::fprintf(stderr, "[OMNI-CENSUS] this binary was built without the hooks. "
			    "Rebuild with: cmake -S . -B build-census -G Ninja -DFDS_OMNI_CENSUS=ON\n");
	}
#endif
	uint64_t ocn[OMNI_CEN_N] = {0};

	for (int py = y1; py < y2; ++py) {
		for (int px = x1; px < x2; ++px) {
			// Wave-1 of checkerboard: skip odd cells (filled by the
			// fill-pass after all wave-1 tiles complete).
			//
			// --deferred_checker_env_full (default OFF = byte-null): an
			// ENV-REFLECTIVE pixel is NOT dropped. The fill refuses to
			// average those (`envForceFull`, TileFill) and re-shades them
			// with the scalar fallback — which is a REDUCED kernel (no
			// --pbr GGX lobe, no shadow maps, no AO, no nmap LOD fade), so
			// shading alternate pixels of a reflective surface with two
			// different BRDFs prints a STATIC lattice. Shading them here
			// costs no extra shaded PIXELS (the fill was already
			// full-shading exactly this set), only the terms the fallback
			// was skipping. Test costs one extra G-buffer load per dropped
			// cell, and only when the flag is on.
			if (checker || quarter) {
				const bool drop = checker ? (((px ^ py) & 1) != 0)
				                          : (((px | py) & 1) != 0);
				if (drop) {
					bool envHere = false;
					if (envTabG && (checkerEnvFull || checkerEdgeFull)) {
						const uint32_t m32e = gb.txtr[size_t(py) * XRes + px];
						envHere = envTabG[(m32e >> 20) & 0xFF] != nullptr;
					}
					bool keep = checkerEnvFull && envHere;
					// --deferred_checker_edge_full: the fill has a SECOND
					// full-shade trigger — "no compatible neighbour", i.e. every
					// material/normal/depth edge — and it re-shades those with
					// the same REDUCED kernel the env row was about. Shade them
					// here instead, with the real one. Env pixels are excluded:
					// the fill force-fulls those by a different rule and they
					// belong to --deferred_checker_env_full, not to this flag.
					if (!keep && checkerEdgeFull && !envHere)
						keep = fillFallsBackHere(gb, ZPage16, XRes, ctx.yres,
						                         px, py, quarter,
						                         w1QNormalCheck, w1QNormalCos,
						                         w1QZCheck, w1QZJump);
					if (!keep) continue;
				}
			}

			const size_t i = size_t(py) * XRes + px;
			const word zEnc = ZPage16[i];
			if (zEnc == 0) continue;  // pixel not touched by Mekalele
#if FDS_PIX_ABLATE
			float ablKeep = 0.0f;
#endif
			PIX_ABL_CUT(1, float(zEnc));

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
			//
			// --shadow_noncaster_depth: a material EXCLUDED from the shadow
			// bake (Shadow_MaterialSkipsCasting — Transparent/Additive/SkipZ
			// or a name containing "lamp"/"emi") never writes its own id into
			// any cube, so the PolyId identity test "the closest thing to the
			// light along this ray must be ME" is unsatisfiable for it and it
			// is shadowed FOR EVER by whatever the bake did rasterise behind
			// it. Resolve such a receiver to -1, the documented "force Depth
			// semantics" sentinel of resolveCubeAtten / CubeShadow_Sample.
			int surfaceShadowId = gb.shadowMatID.empty()
			    ? int(matID + 1)
			    : int(gb.shadowMatID[i]);
			// Per-frame hoist: the predicate depends only on the Material*,
			// which matID selects, so it is read from ctx.shadowSkipMask (one
			// load + shift + test) instead of an out-of-line call whose
			// function-local atomic cache cost 3.44 % of steady-state samples.
			if (noncasterDepthG
			    && ((ctx.shadowSkipMask[matID >> 6] >> (matID & 63)) & 1u))
				surfaceShadowId = -1;
			// --deferred_cube_direct (see cubeDirectTile).
			const bool cubeDirect = cubeDirectTile && surfaceShadowId >= 0;
			PIX_ABL_CUT(2, float(surfaceShadowId) + float(pmid) + float(miplevelForFade));

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
				// Filtered albedo (bilinear/trilinear) when enabled — the
				// nmap-as-diffuse dev viz still point-samples (the raster
				// pass filtered the DIFFUSE, not the normal map). Heightmap
				// (parallax) materials are INCLUDED (Tier 1, filtered
				// parallax): the rasterizer bilinear-samples at the
				// parallax-SHIFTED uf/vf and writes gb.albedo for them too, so
				// the kernel reads the filtered-at-shifted-UV texel here. The
				// metal/rough/AO/normal map fetches below still use swizzledUV
				// (point, parallax-shifted). texFilter==0 → byte-identical.
				const dword texel =
					(texFilterOn && !gb.albedo.empty()
					 && !(sNmapAsDiffuse && Mat->NormalMap))
					? gb.albedo[i]
					: texData[swizzledUV];
				texB = float(texel & 0xFF);
				texG = float((texel >> 8) & 0xFF);
				texR = float((texel >> 16) & 0xFF);
				texA = float((texel >> 24) & 0xFF);
			}
			// Editor albedo tint — per-MATERIAL so surfaces sharing one
			// deduped Texture* don't bleed into each other (mutating the
			// texture pixels did; MECH_HUL.JPG serves hull+canons+legs).
			// x*1.0f is bit-exact, so untinted materials are unchanged.
			texB *= Mat->TintB; texG *= Mat->TintG; texR *= Mat->TintR;
			PIX_ABL_CUT(3, texB + texG + texR + texA);

			// Static-shadow lightmap address for this pixel — resolved
			// once, used by all cube-shadow taps in the per-omni loops
			// below via resolveCubeAtten().
			// --deferred_lm_addr_skip: the address is read ONLY behind
			// resolveCubeAtten's `useLightmap && pl.lm` guard and useLightmap is
			// lmKernelEnabled, so with the lightmap kernel off this whole resolve
			// (two G-buffer planes, a scene-table index, two pointer chases) is
			// dead — and its four results otherwise stay live across the entire
			// pixel body. Byte-null by construction.
			const PixelLightmap pixelLM = (lmKernelEnabled || !lmAddrSkip)
			    ? resolvePixelLightmap(gb, i, ctx.Sc)
			    : PixelLightmap{};
			PIX_ABL_CUT(4, float(pixelLM.faceIdx) + float(pixelLM.sB) + float(pixelLM.tB)
			                + float(pixelLM.lm != nullptr));
			// Lightmap kernel branch is disabled when the dynamic-mesh
			// shadow pass is on: the lightmap only encodes the static-
			// occluder polyId, but `--shadow-dynamic` puts moving meshes
			// into a parallel buffer that only the runtime cube tap
			// reads. Until we composite both, fall back to cube tap
			// when dynamic is on so robot shadows on the floor still
			// render. (Set once per tile, see lmKernelEnabled below.)

			// Decode normal (view-space, unit-length).
			float nx, ny, nz;
			meka::oct_decode_u32(gb.normal[i], nx, ny, nz);
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

			PIX_ABL_CUT(5, nx + ny + nz + nGeoX + nGeoY + nGeoZ);

			// Reconstruct view-space position. ZPage16 stores
			// 0xFF80 - round(g_zscale * z), so:
			//   z = (0xFF80 - zEnc) / g_zscale
			const float z = float(0xFF80 - zEnc) * ctx.invZScale;
			const float x = (float(px) - CntrEX) * z * ctx.invFOVX;
			const float y = (CntrEY - float(py)) * z * ctx.invFOVY;

			// Ambient. Same expression as forward Lighting() — except
			// here Mat is per-pixel, not per-mesh-Mat[0].
			float lB, lG, lR;
			if (shCoefG) {
				// --sh_ambient: directional SH irradiance along the (post
				// normal-map) shading normal, in place of the flat
				// Sc->Ambient constant. World normal = viewToWorld·viewN
				// (rotation only). Mirrors the flat branches below exactly,
				// only Sc->Ambient.{B,G,R} → E(n).{B,G,R}.
				const float wnx = ctx.viewToWorld[0][0]*nx + ctx.viewToWorld[0][1]*ny + ctx.viewToWorld[0][2]*nz;
				const float wny = ctx.viewToWorld[1][0]*nx + ctx.viewToWorld[1][1]*ny + ctx.viewToWorld[1][2]*nz;
				const float wnz = ctx.viewToWorld[2][0]*nx + ctx.viewToWorld[2][1]*ny + ctx.viewToWorld[2][2]*nz;
				float eB, eG, eR;
				shEvalIrradiance(shCoefG, wnx, wny, wnz, eB, eG, eR);
				if (Mat->Txtr) {
					lB = Mat->Luminosity * 255.0f + Mat->Diffuse * eB;
					lG = Mat->Luminosity * 255.0f + Mat->Diffuse * eG;
					lR = Mat->Luminosity * 255.0f + Mat->Diffuse * eR;
				} else {
					lB = Mat->Luminosity * Mat->BaseCol.B + Mat->Diffuse * eB;
					lG = Mat->Luminosity * Mat->BaseCol.G + Mat->Diffuse * eG;
					lR = Mat->Luminosity * Mat->BaseCol.R + Mat->Diffuse * eR;
				}
			} else if (Mat->Txtr) {
				lB = Mat->Luminosity * 255.0f + Mat->Diffuse * ctx.Sc->Ambient.B;
				lG = Mat->Luminosity * 255.0f + Mat->Diffuse * ctx.Sc->Ambient.G;
				lR = Mat->Luminosity * 255.0f + Mat->Diffuse * ctx.Sc->Ambient.R;
			} else {
				lB = Mat->Luminosity * Mat->BaseCol.B + Mat->Diffuse * ctx.Sc->Ambient.B;
				lG = Mat->Luminosity * Mat->BaseCol.G + Mat->Diffuse * ctx.Sc->Ambient.G;
				lR = Mat->Luminosity * Mat->BaseCol.R + Mat->Diffuse * ctx.Sc->Ambient.R;
			}
			PIX_ABL_CUT(6, lB + lG + lR + x + y + z);

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
			PIX_ABL_CUT(7, pbrA2 + pbrK + pbrNdotV + vx + vy + vz + gloss);

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

			// --metal_spec_f0: the DIRECT lobe needs the metalness BEFORE the
			// light loop (F0 is a per-light Fresnel input), whereas the compose
			// below fetches it after. Hoisted ONLY when the flag is on, so the
			// default path keeps the single post-loop fetch it always had and
			// stays byte-identical. Same texel, same mip, same swizzled UV.
			float metalF0B = 0.04f, metalF0G = 0.04f, metalF0R = 0.04f;
			bool  metalF0On = false;
			if (metalSpecF0G && hasMetal) {
				const byte *mdEarly = (miplevel < Mat->MetallicMap->numMipmaps)
					? reinterpret_cast<const byte*>(Mat->MetallicMap->Mipmap[miplevel]) : nullptr;
				const float mEarly = mdEarly ? float(mdEarly[swizzledUV]) * (1.0f/255.0f) : 0.0f;
				if (mEarly > 0.0f) {
					// F0 = mix(0.04, albedo, metalness), per channel, on the
					// LINEAR albedo — the --hdr_linear composite squares the
					// albedo everywhere else, so the reflectance has to be the
					// squared value too or the highlight is authored in a
					// different space from the surface it sits on.
					const float kNf = 1.0f/255.0f;
					const float lb0 = texB*kNf*texB*kNf;
					const float lg0 = texG*kNf*texG*kNf;
					const float lr0 = texR*kNf*texR*kNf;
					metalF0B = 0.04f + (lb0 - 0.04f) * mEarly;
					metalF0G = 0.04f + (lg0 - 0.04f) * mEarly;
					metalF0R = 0.04f + (lr0 - 0.04f) * mEarly;
					metalF0On = true;
				}
			}

			// Ambient occlusion. DEFAULT: darken ONLY the ambient term (lB/lG/lR
			// before the direct-light loop adds to them) — direct light is
			// occluded by shadows, not AO. S4b (--ao_direct): MOVE the multiply
			// off the ambient-only term to the FINAL combined diffuse after the
			// light loop (below) — a static, acne-free stand-in for the per-block
			// mortar self-shadow the single-shadow-id collapse removed. Here we
			// only fetch the raw occlusion; the two applications share it.
			float aoRaw = 1.0f;                       // map occlusion [0,1] (1=open)
			if (hasAoMap) {
				if (aoInAlpha) {
					aoRaw = texA * (1.0f/255.0f);        // free (albedo alpha)
				} else {
					// AO maps arrive from the importer as SINGLE-CHANNEL 8-BIT
					// (MakeHeight8, like height/roughness/metallic), so they must
					// be indexed as BYTES. Reading them as dwords put the fetch at
					// byte offset 4×swizzledUV inside a 1-byte-per-texel
					// allocation — measured 3,981,420 into a 1 MiB mip, i.e. 3.8 MB
					// out of bounds — returning heap bytes that differ per process.
					// That was the greets render-nondeterminism root cause: with
					// ao_map_strength 2.0 the garbage drove `ao` to -2.22, the
					// ambient term went negative and clamped to 0, and the
					// --sh_ambient probe bake turned ~30 such pixels into
					// frame-wide ambient drift. The mip bound is checked for the
					// same reason every sibling map fetch checks it (Mipmap[]
					// past numMipmaps is unowned). The `ao_from_diffuse` dev
					// fallback samples the 32-bit albedo, so both widths stay
					// live. Mirrors the transparent kernel's AO fetch.
					const Texture *aoTex = Mat->AoMap ? Mat->AoMap : Mat->Txtr;
					const byte *aoMip = (miplevel < aoTex->numMipmaps)
						? reinterpret_cast<const byte *>(aoTex->Mipmap[miplevel])
						: nullptr;
					if (!aoMip) {
						aoRaw = 1.0f;                      // no such level → unoccluded
					} else if (aoTex->BPP == 8) {
						aoRaw = float(aoMip[swizzledUV]) * (1.0f/255.0f);
					} else {
						const dword aoTexel =
							reinterpret_cast<const dword *>(aoMip)[swizzledUV];
						aoRaw = (float(aoTexel & 0xFF)         * 0.114f
						       + float((aoTexel >> 8)  & 0xFF)  * 0.587f
						       + float((aoTexel >> 16) & 0xFF)  * 0.299f) * (1.0f/255.0f);
					}
				}
				if (!aoDirectG) {
					// ambient-only (canonical): Global dial × per-material dial
					// (Mat->AoStrength defaults 1 — the ParallaxScale pattern).
					const float ao = 1.0f - aoStrengthG * Mat->AoStrength * (1.0f - aoRaw);
					lB *= ao; lG *= ao; lR *= ao;
				}
			}
			PIX_ABL_CUT(8, aoRaw + lB + float(isWater) + float(hasEnvRefl)
			                + float(useVecHere) + float(hasAoMap) + float(tl.count));

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
			PIX_ABL_CUT(9, sampleWorldX + sampleWorldY + sampleWorldZ);

			// ── S1c HORIZON-MAP RELIEF SELF-SHADOW (--pom_horizon) ──────────
			// Resolve this pixel's horizon record (8 azimuths of u8
			// sin(horizon), addressed by the SAME swizzled index + mip as the
			// albedo) and the tangent frame the azimuths are measured in.
			//
			// The frame is built from the GEOMETRIC normal, not the
			// normal-mapped one: the bake's azimuths are defined on the
			// authored surface's UV axes, and re-basing them on a per-texel
			// perturbed normal would rotate the lookup by the bump. The tangent
			// comes from the same G-buffer channel the normal-map path reads,
			// decoded independently here so that path stays untouched (this
			// whole block is dead with the flag off).
			//
			// Azimuth 0 is +U (= T), azimuth 2 is +V (= handedness·(N×T)) —
			// the same axes the bake walks, and the handedness flip is what
			// keeps the mirrored-UV clones' shadows from running backwards.
			// The relief-horizon record, behind ONE pointer instead of seven
			// per-pixel locals. It used to resolve Mat->PomHorizon and set six
			// tangent-frame floats plus a texel pointer for EVERY shaded pixel,
			// and all seven then stayed live across the whole per-light loop in
			// a body the register allocator is already spilling. MEASURED by the
			// per-pixel ablation ladder at greets t=5743: 0.034 Gi/f — 1.7 % of
			// the entire DeferredLighting call — for a flag (--pom_horizon) that
			// is OFF in every shipping scene. No hatch: nothing reads what this
			// stops materialising, so there is no arm to compare.
			HzFrame hzF;
			const HzFrame *hzRec = nullptr;
			if (pomHorizonOnG && !gb.tangent.empty()) {
				const PomHorizonMap *hzMap = Mat->PomHorizon;
				if (hzMap && miplevel < hzMap->numMipmaps && hzMap->data) {
					const meka::u16 packedT = gb.tangent[i];
					if (packedT != 0) {
						float tx, ty, tz;
						meka::oct_decode_u16(packedT, tx, ty, tz);
						const float tDotN = tx*nGeoX + ty*nGeoY + tz*nGeoZ;
						tx -= nGeoX * tDotN; ty -= nGeoY * tDotN; tz -= nGeoZ * tDotN;
						const float tLen2 = tx*tx + ty*ty + tz*tz;
						if (tLen2 > 1e-12f) {
							const float inv = fast_rsqrt(tLen2);
							hzF.tx = tx*inv; hzF.ty = ty*inv; hzF.tz = tz*inv;
							const float hs = Mat->TbnHandedness;
							hzF.bx = (nGeoY*hzF.tz - nGeoZ*hzF.ty) * hs;
							hzF.by = (nGeoZ*hzF.tx - nGeoX*hzF.tz) * hs;
							hzF.bz = (nGeoX*hzF.ty - nGeoY*hzF.tx) * hs;
							hzF.texel = hzMap->data
							          + (hzMap->mipOfs[miplevel] + swizzledUV) * kPomHorizonAzimuths;
							hzRec = &hzF;
						}
					}
				}
			}
			// --pom_horizon_viz: accumulate the term itself over the lights that
			// actually reach this pixel, so the shadow's shape (and its motion
			// with the light) is visible without albedo or ambient hiding it.
			float hzVizSum = 0.0f, hzVizWeight = 0.0f;
			PIX_ABL_CUT(10, hzTx + hzTy + hzTz + hzBx + hzBy + hzBz
			                 + float(hzTexel != nullptr));

			if (tapCensus) ++tcPixels;
			int nLiveOmni = 0;
			if (omniCensus) ++ocn[10];
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
						if (omniCensus) ++ocn[0];
						OMNI_ABL_CUT_BARE(1);
						if (tl.mirrorId[n] != pmid) { if (omniCensus) ++ocn[1]; continue; }
						OMNI_ABL_CUT(2, 1.0f);
						const float Lpx = tl.posX[n];
						const float Lpy = tl.posY[n];
						const float Lpz = tl.posZ[n];
						const float wx = Lpx - x;
						const float wy = Lpy - y;
						const float wz = Lpz - z;
						const float dot = wx * nx + wy * ny + wz * nz;
						if (dot < 0.0f) { if (omniCensus) ++ocn[2]; continue; }
						OMNI_ABL_CUT(3, dot);
						const float len2 = wx*wx + wy*wy + wz*wz;
						const float r2 = tl.range2[n];
						if (len2 > r2) { if (omniCensus) ++ocn[3]; continue; }
						OMNI_ABL_CUT(4, len2);
						if (tapCensus) ++tcL[n][0];
						// Mirror-bounce window portal: a bounce spot's apex
						// is behind the glass; only light it onto this pixel
						// if the apex→pixel segment passes through the mirror
						// window. Gate keeps non-bounce lights free.
						if (tl.winMinX[n] <= tl.winMaxX[n] &&
						    bouncePortalReject(tl, n, sampleWorldX, sampleWorldY, sampleWorldZ))
							{ if (omniCensus) ++ocn[4]; continue; }
						OMNI_ABL_CUT(5, dot + len2);
						const float lenInv = fast_rsqrt(len2);
						const float dist   = len2 * lenInv;
						const float rRange = tl.rRange[n];
						float k = dot * lenInv * (1.0f - dist * rRange);
						OMNI_ABL_CUT(6, k);
						// Spot cone: matches the vec body's coneAtten so nmap
						// pixels (which all flow through this scalar path)
						// don't render the robot spotlight as an omni.
						if (tl.isSpot[n]) {
							const float cosTheta = -(tl.dirX[n]*wx + tl.dirY[n]*wy + tl.dirZ[n]*wz) * lenInv;
							if (cosTheta <= tl.cosOuter[n]) { if (omniCensus) ++ocn[5]; continue; }
							if (cosTheta < tl.cosInner[n]) {
								const float ct = (cosTheta - tl.cosOuter[n]) / (tl.cosInner[n] - tl.cosOuter[n]);
								k *= ct * ct * (3.0f - 2.0f * ct);
							}
						}
						OMNI_ABL_CUT(7, k);
						if (omniCensus) {
							++ocn[OMNI_CEN_BR];
							const bool a = tl.shadowMapIdx[n]     >= 0;
							const bool b = tl.srcShadowMapIdx[n]  >= 0;
							const bool cc = tl.srcCubeShadowIdx[n] >= 0;
							if (a)  ++ocn[OMNI_CEN_BR+1];
							if (b)  ++ocn[OMNI_CEN_BR+2];
							if (cc) ++ocn[OMNI_CEN_BR+3];
							if (!a && !b && !cc) ++ocn[OMNI_CEN_BR+4];
							if (tl.cubeShadowIdx[n] >= 0) ++ocn[OMNI_CEN_BR+5];
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
						// THE CALL IS THE COST, NOT THE MAPS. computeMapShadowAtten
						// reads exactly three index planes — the light's own 2-D
						// spot map, a mirror clone's SOURCE map, a mirror clone's
						// SOURCE cube — and every one of its three bodies is
						// guarded on that index being >= 0. With all three
						// negative it can only `return shadowAtten` still holding
						// its 1.0f initialiser. It is nonetheless an OUT-OF-LINE
						// function with a 176-byte frame and TEN callee-save
						// stp/ldp pairs, invoked once per (pixel x light) that
						// clears the cone test: 4.749 M times a frame at greets
						// t=5743 — of which --omni_census says **99.50 % carry
						// none of the three** (his pose t=3122: 32.43 %). Measured
						// by the FDS_OMNI_ABLATE ladder, that stage is 0.405 Gi/f,
						// 16.3 % of the whole omni loop, to compute the constant
						// 1.0f four and a half million times.
						//
						// The guard is the same predicate the function's own three
						// `if`s already form, hoisted to the call site: AND of the
						// three int32s has bit 31 set iff ALL of them are negative
						// (absent indices are -1), so `>= 0` means "at least one
						// index is live, the call can do something". BIT-EXACT by
						// construction — it can only skip calls whose return value
						// is 1.0f, and 1.0f fails the `<= 0.0f` test below either
						// way. The vec path needs no equivalent: it already tests
						// the same planes 8-wide (`anyShadow`) before its lane loop.
						float shadowAtten = 1.0f;
						if ((tl.shadowMapIdx[n] & tl.srcShadowMapIdx[n]
						                        & tl.srcCubeShadowIdx[n]) >= 0) {
							shadowAtten = computeMapShadowAtten(
								tl, n, ctx, x, y, z, wx, wy, wz, lenInv,
								nGeoX, nGeoY, nGeoZ, surfaceShadowId,
								kShadowBiasG, kSlopeBiasG, profShadowCache);
							// Combined srcSm×srcCube×smIdx; 0 = fully shadowed (the
							// old `if (occ >= 1.0f) continue;` early-out).
							if (shadowAtten <= 0.0f) { if (omniCensus) ++ocn[6]; continue; }
						}
						OMNI_ABL_CUT(8, k + shadowAtten);

						// Cube shadow (omni shadow caster). Two paths:
						//  - Static-mesh pixel + lightmap baked for this
						//    cubeIdx → bilinear sample from the lightmap.
						//  - Otherwise → per-pixel cube tap.
						const int32_t cubeIdx = tl.cubeShadowIdx[n];
						if (cubeIdx >= 0) {
							// --deferred_cube_direct: this IS resolveCubeAtten's
							// PolyId arm, called with the arguments it forwards,
							// without the 20-argument frame around it.
							const float cubeAtten = cubeDirect
							    ? CubeShadow_Sample(cubeIdx,
								sampleWorldX, sampleWorldY, sampleWorldZ,
								x, y, z, /*constBias=*/0, /*slopeBias=*/0,
								surfaceShadowId)
							    : resolveCubeAtten(
								pixelLM, cubeIdx, lmKernelEnabled, caFlags,
								wx, wy, wz, lenInv,
								nGeoX, nGeoY, nGeoZ,
								sampleWorldX, sampleWorldY, sampleWorldZ,
								x, y, z, kShadowBiasG, kSlopeBiasG,
								surfaceShadowId);
							if (tapCensus) {
								++tcL[n][1];
								if (cubeAtten >= 1.0f)      ++tcL[n][2];
								else if (cubeAtten <= 0.0f) ++tcL[n][3];
								if (tcBlk) {
									// Block index resolved HERE, not per pixel: two
									// integer divides by a runtime B at the top of the
									// pixel body kept tcB/tcBlocksX live across the
									// whole loop and cost 2 % of lighting-w1's
									// instructions with the census OFF (measured
									// parent-vs-instrumented, greets t=3122).
									const int tcBlkIdx =
										((py - y1) / tcB) * tcBlocksX + (px - x1) / tcB;
									uint32_t *b = tcBlk + (size_t(tcBlkIdx) * size_t(tcN) + size_t(n)) * 3u;
									++b[0];
									if (cubeAtten >= 1.0f)      ++b[1];
									else if (cubeAtten <= 0.0f) ++b[2];
								}
							}
							if (cubeAtten <= 0.0f) { if (omniCensus) ++ocn[7]; continue; }
							shadowAtten *= cubeAtten;
						}
						OMNI_ABL_CUT(9, k + shadowAtten);
						// S1c: relief self-shadow. Take the (already unit-length
						// after ×lenInv) pixel→light direction into the surface's
						// tangent frame; its Z is sin(elevation), its XY give the
						// azimuth. The azimuth picks the two adjacent baked
						// channels and a blend weight, and the light is faded out
						// where its elevation drops below that interpolated
						// horizon. This is the ONLY intra-wall shadow either
						// displacement path can have (the PolyId shadow test is
						// identity-only, one id per authored wall plane), and
						// being a shading term it works the same for the
						// tessellation bake and the per-pixel shell.
						//
						// atan2 is avoided: for 8 evenly spaced azimuths the
						// octant follows from two signs and one magnitude
						// compare, and the in-octant weight from the ratio of
						// the smaller to the larger component. Using the ratio
						// where the angle wants atan(ratio)/45° is at most ~4%
						// of an octant off — invisible under a term whose whole
						// point is a soft edge, and it costs one divide instead
						// of a transcendental.
						if (hzRec) {
							const float lx = wx * lenInv, ly = wy * lenInv, lzv = wz * lenInv;
							const float hzTx = hzRec->tx, hzTy = hzRec->ty, hzTz = hzRec->tz;
							const float hzBx = hzRec->bx, hzBy = hzRec->by, hzBz = hzRec->bz;
							const unsigned char *const hzTexel = hzRec->texel;
							const float sinElev = lx*nGeoX + ly*nGeoY + lzv*nGeoZ;
							if (sinElev <= 0.0f) { if (omniCensus) ++ocn[8]; continue; }   // below the plane
							const float ltx = lx*hzTx + ly*hzTy + lzv*hzTz;
							const float lbv = lx*hzBx + ly*hzBy + lzv*hzBz;
							const float ax = std::fabs(ltx), ay = std::fabs(lbv);
							const float mx = ax > ay ? ax : ay;
							const float mn = ax > ay ? ay : ax;
							const float rat = mn / (mx + 1e-20f);
							int k0; float w;
							if (ltx >= 0.0f) {
								if (lbv >= 0.0f) { if (ax >= ay) { k0 = 0; w = rat; }
								                   else          { k0 = 1; w = 1.0f - rat; } }
								else             { if (ax >= ay) { k0 = 7; w = 1.0f - rat; }
								                   else          { k0 = 6; w = rat; } }
							} else {
								if (lbv >= 0.0f) { if (ay >= ax) { k0 = 2; w = rat; }
								                   else          { k0 = 3; w = 1.0f - rat; } }
								else             { if (ay >= ax) { k0 = 5; w = 1.0f - rat; }
								                   else          { k0 = 4; w = rat; } }
							}
							const float h0 = float(hzTexel[k0]);
							const float h1 = float(hzTexel[(k0 + 1) & (kPomHorizonAzimuths - 1)]);
							const float sinHor = (h0 + (h1 - h0) * w) * (1.0f / 255.0f);
							// smoothstep over [sinHor-soft, sinHor+soft].
							float t = (sinElev - sinHor) * hzInvSoft2 + 0.5f;
							t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
							const float vis = t * t * (3.0f - 2.0f * t);
							const float hzAtten = 1.0f - hzStrengthG * (1.0f - vis);
							if (pomHorizonVizG) { hzVizSum += hzAtten * k; hzVizWeight += k; }
							shadowAtten *= hzAtten;
							if (shadowAtten <= 0.0f) { if (omniCensus) ++ocn[8]; continue; }
						}
						OMNI_ABL_CUT(10, k + shadowAtten);
						if (omniCensus) { ++ocn[9]; ++nLiveOmni; }
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
						OMNI_ABL_CUT_BARE(11);

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
								// --metal_spec_f0 only: the lobe WITHOUT Fresnel, and the
								// Schlick (1-VoH)^5 factor, so F can be re-evaluated per
								// CHANNEL against a conductor's albedo F0. Filled inside the
								// flag's own branch so the default lobe below keeps its exact
								// arithmetic (reassociating it would not be byte-null).
								float specNoF = 0.0f, specOm5 = 0.0f;
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
									if (metalF0On) {
										specOm5 = om2*om2*om;
										specNoF = D * Gv * Gl / (4.0f * pbrNdotV);
									}
								} else {
									spec = pow_glossClass(NdotH, Mat->Glossiness);
								}
								{
									// Multiply by shadowAtten so shadowed pixels don't
									// leak specular highlights — was a visible bug at
									// bumped-mortar pixels inside shadow regions, where
									// the bumped N satisfies the sharp Gloss=48 lobe
									// while diffuse was correctly killed.
									if (metalF0On && sPbr) {
										// Conductor: F0 = mix(0.04, LINEAR albedo, metalness),
										// Schlick evaluated per channel. The post-loop gamma metal
										// tint is SKIPPED under this flag (see the compose) — the
										// albedo is already in F0 and would otherwise count twice.
										const float base = specNoF * Mat->Specular *
											(1.0f - dist * rRange) * shadowAtten;
										sB += base * (metalF0B + (1.0f - metalF0B) * specOm5) * Lcb;
										sG += base * (metalF0G + (1.0f - metalF0G) * specOm5) * Lcg;
										sR += base * (metalF0R + (1.0f - metalF0R) * specOm5) * Lcr;
									} else {
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
			}

			if (omniCensus)
				++ocn[11 + (nLiveOmni < OMNI_CEN_HIST ? nLiveOmni : OMNI_CEN_HIST - 1)];

			// S4b: AO on the FINAL combined diffuse (ambient + direct). Only when
			// --ao_direct moved it here off the ambient-only term above. Uses its
			// own dial (ao_direct_strength); specular (sB/sG/sR, added at the
			// compose) is a highlight and is left un-occluded — the shadow-proxy
			// intent is diffuse groove occlusion, not dulled highlights.
			if (hasAoMap && aoDirectG) {
				const float aoD = 1.0f - aoDirectStrG * Mat->AoStrength * (1.0f - aoRaw);
				lB *= aoD; lG *= aoD; lR *= aoD;
			}

			// --pom_horizon_viz: replace the surface with the horizon TERM,
			// N·L-weighted over the lights that actually reached this pixel, so
			// the relief shadow's shape and its motion with the light read
			// directly. A horizon pixel no light reaches shows mid grey; a
			// non-horizon surface is left alone so the scene stays legible.
			if (pomHorizonVizG && hzRec) {
				const float v = hzVizWeight > 0.0f ? (hzVizSum / hzVizWeight) : 0.5f;
				lB = lG = lR = 256.0f * v;
				sB = sG = sR = 0.0f;
				texB = texG = texR = 255.0f;   // neutral albedo: the TERM, alone
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

			// Diffuse modulation `fd = texel * light / 256` USED to be computed
			// here and scaled in place by the metal and diffuse-energy blocks
			// below. It now lives in the `if (!ldrSkip)` compose after them
			// (--deferred_shade_ldr_skip) — the two scalings are applied there in
			// the SAME order, and neither `texB/texG/texR`, `lB/lG/lR`, `metalM`
			// nor `fresEC` is touched by anything in between (EnvSpecComposeScalar
			// takes the texels and metalM BY VALUE and returns through sB/sG/sR),
			// so moving it is a float identity, not a reordering.
			// Metalness (--metal_map): m=1 pixels are conductors — no diffuse
			// (the albedo becomes the REFLECTION tint below), and analytic
			// highlights tint by the albedo instead of staying light-colored.
			float metalM = 0.0f;
			if (metalMapOnG && Mat->MetallicMap) {
				const byte *md = (miplevel < Mat->MetallicMap->numMipmaps)
					? reinterpret_cast<const byte*>(Mat->MetallicMap->Mipmap[miplevel]) : nullptr;
				if (md) metalM = float(md[swizzledUV]) * (1.0f/255.0f);
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
			// SKIPPED under --metal_spec_f0, which puts the albedo into the
			// direct lobe's Fresnel F0 instead (the physically correct place);
			// applying both would count the conductor's albedo twice. NOTE the
			// templated/vec spec loop does NOT implement --metal_spec_f0 —
			// greets runs the scalar path (nmap pixels force it) and the vec
			// path is off for this scene by policy.
			if (metalM > 0.0f && !metalF0On) {
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
			// (1-F) diffuse energy conservation reads this after the compose
			// moved down; declared here so it survives the block.
			float fresEC = 0.0f;
			if (hasEnvRefl) {
				if (g_envVecStats) g_envCntWave1.fetch_add(1, std::memory_order_relaxed);
				EnvSpecComposeScalar(ctx, envP, Mat, miplevel, swizzledUV,
				                     x, y, z, nx, ny, nz,
				                     sampleWorldX, sampleWorldY, sampleWorldZ,
				                     texB, texG, texR, gloss, metalM,
				                     roughMapOnG, envReflGainG,
				                     envBrdfAnalyticG, multiScatterG, metalTintLinG, sB, sG, sR, &fresEC);
			}
			// Per-material specular response multiplier (Material::SpecMul,
			// RVSF 0x800, editor 'specMul'): scales the FINAL specular —
			// analytic highlights AND the env compose above — after the
			// roughness/metal modulation, so it never distorts roughness.
			// Default 1.0f is skipped (and would be an exact float identity).
			if (Mat->SpecMul != 1.0f) {
				sB *= Mat->SpecMul; sG *= Mat->SpecMul; sR *= Mat->SpecMul;
			}
			// HDR B2 (--hdr_linear): stash the unclamped opaque radiance +
			// coverage flag. This arm is built from `dlB` — the raw light
			// accumulator — and never from `fdB`, which is exactly why the LDR
			// compose below can be skipped under it. It reads `out[i]` for the
			// water underlay; this pixel has not been written yet either way, so
			// sitting above the LDR block rather than below it reads the same
			// word. The froxel composite reads h[3] to take the scene from here
			// (opaque) vs the VPage (sky/forward content the kernel never wrote).
			if (hdrWrite && hdrLinear) {
				fds::hdrf* h = ctx.hdrBuf + i * 4;
				// B2 + full coherence: linear lighting. albedo² (gamma-2.0
				// decode) × light at power 1; specular is reflected light → a
				// linear add. Store LINEAR radiance directly (the tonemap no
				// longer decodes; T·scene + in-scatter compose in linear).
				const float kN = 1.0f / 255.0f;
				const float aB = texB*kN, aG = texG*kN, aR = texR*kN;
				// --hdr_metal_kill (docs/SHADING_CONTRACT.md D1): a conductor
				// has no diffuse lobe. The kernel computes that kill in the LDR
				// compose below (`fdB *= 1-metalM`) but it lands on the LDR
				// combine only, while THIS — the shipped HDR frame — is built
				// from the raw accumulator, so metals keep a diffuse term they
				// should not have. Mode 2 spares the emissive the accumulator
				// was seeded with (see the lB seed); mode 1 is the blunt
				// whole-accumulator form. Mode 0 is the historical behaviour and
				// the old byte-pins; the shipping default is 2 since 2026-08-08
				// (FeatureFlags.def has the evidence).
				float dlB = lB, dlG = lG, dlR = lR;
				const int metalKill = fds::FeatureFlags::hdr_metal_kill();
				if (metalKill > 0 && metalM > 0.0f) {
					const float dkH = 1.0f - metalM;
					if (metalKill >= 2) {
						const float lum = Mat->Luminosity;
						const float eB2 = lum * (Mat->Txtr ? 255.0f : Mat->BaseCol.B);
						const float eG2 = lum * (Mat->Txtr ? 255.0f : Mat->BaseCol.G);
						const float eR2 = lum * (Mat->Txtr ? 255.0f : Mat->BaseCol.R);
						dlB = eB2 + (lB - eB2) * dkH;
						dlG = eG2 + (lG - eG2) * dkH;
						dlR = eR2 + (lR - eR2) * dkH;
					} else {
						dlB = lB * dkH; dlG = lG * dkH; dlR = lR * dkH;
					}
				}
				float rlB = aB*aB*dlB + sB, rlG = aG*aG*dlG + sG, rlR = aR*aR*dlR + sR;
				if (isWater) {            // reflection underlay is gamma → linearize
					const dword e = out[i];
					const float wB=float(e&0xFF)*kN, wG=float((e>>8)&0xFF)*kN, wR=float((e>>16)&0xFF)*kN;
					rlB += wB*wB*255.0f*0.5f; rlG += wG*wG*255.0f*0.5f; rlR += wR*wR*255.0f*0.5f;
				}
				h[0] = fds::HdrClamp(rlB); h[1] = fds::HdrClamp(rlG); h[2] = fds::HdrClamp(rlR);
				h[3] = 1.0f;
			}
			// ── The 8-bit LDR compose ────────────────────────────────────────
			// Everything from here to `out[i]` feeds the VPage and, through
			// hB/hG/hR, the B1 GAMMA radiance. `ldrSkip` (hoisted per tile) is
			// true only when the pass provably ends in the tonemap AND the
			// radiance the tonemap reads is the LINEAR one, which is built from
			// `dlB` above and never from `fdB` — see the hoist for the argument.
			// The B2 store is deliberately ABOVE this block rather than after
			// it: with nothing live across the branch the skip is a single
			// forward jump, which is worth ~0.3 % of w1 over the shape that
			// zero-initialised out*/h* outside it.
			if (!ldrSkip) {
				// Diffuse modulation: pixel = texel * light / 256.
				// Reflective faces are dispatched to TheOtherBarry<OVERWRITE,
				// TEXTURETEXTURE> in RenderInnerMekalele and never reach this
				// code path — they're rendered by the forward filler (which
				// does the env+tex/2 composite using forward's per-vertex
				// interpolated eu/ev) and skipped here via the mat32 sentinel.
				float fdB = (texB * lB) * (1.0f / 256.0f);
				float fdG = (texG * lG) * (1.0f / 256.0f);
				float fdR = (texR * lR) * (1.0f / 256.0f);
				// Metalness kill (--metal_map): a conductor has no diffuse lobe.
				if (metalM > 0.0f) {
					const float dk = 1.0f - metalM;
					fdB *= dk; fdG *= dk; fdR *= dk;
				}
				// (1-F) diffuse energy conservation: the Fresnel-reflected
				// fraction can't also diffuse. Scales BOTH the LDR combine
				// (int(fdB)) and the B1 gamma radiance (fdB+sB) below — the
				// LINEAR arm has never applied it (rlB is built from dlB).
				if (hasEnvRefl && diffuseEnergyG) {
					const float dc = 1.0f - fresEC;
					fdB *= dc; fdG *= dc; fdR *= dc;
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
				// draw + dispMap distortion has populated VPage with a wavy
				// reflection of the city. The transparent filler does
				//   pixel = saturate(lit_water_texel + existing_VPage/2)
				// (TheOtherBarry.h:392). We reproduce the same blend here:
				// the existing VPage value at this pixel is the reflection
				// preserved across the inter-pass Z-clear (which lets pass-2
				// deferred shading skip non-water-mesh pixels via zEnc check
				// on the freshly-cleared depth buffer). The LINEAR arm reads the
				// same VPage word itself, below, so this blend is LDR/B1 only.
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

				// HDR B1 (gamma, i.e. NOT --hdr_linear): hB/hG/hR IS the shipped
				// radiance on this arm — which is why `ldrSkip` requires
				// hdrLinear and this store is inside the LDR block, not beside
				// the B2 one. Written before the debug-viz stomp, so viz only
				// affects the displayed (LDR) VPage.
				if (hdrWrite && !hdrLinear) {
					fds::hdrf* h = ctx.hdrBuf + i * 4;
					h[0] = fds::HdrClamp(hB); h[1] = fds::HdrClamp(hG); h[2] = fds::HdrClamp(hR);
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
			}   // !ldrSkip
		}
	}

	if (tapCensus) {   // see the declaration block above for what each column means
		const TileLights &tlc = ctx.tileLights[tileIndex];
		long long e = 0, eDead = 0, eCube = 0, eCubeDead = 0;
		long long taps = 0, lit = 0, shad = 0;
		long long uniLitE = 0, uniShadE = 0, mixedE = 0;
		long long uniLitT = 0, uniShadT = 0, mixedT = 0;
		long long deadPx = 0;
		for (int ci = 0; ci < tcN; ++ci) {
			++e;
			const bool hasCube = tlc.cubeShadowIdx[ci] >= 0;
			if (hasCube) ++eCube;
			if (tcL[ci][0] == 0) {
				++eDead;
				deadPx += tcPixels;          // prologue iterations this entry cost
				if (hasCube) ++eCubeDead;
				continue;
			}
			taps += tcL[ci][1]; lit += tcL[ci][2]; shad += tcL[ci][3];
			if (tcL[ci][1] == 0) continue;
			if (tcL[ci][2] == tcL[ci][1])      { ++uniLitE;  uniLitT  += tcL[ci][1]; }
			else if (tcL[ci][3] == tcL[ci][1]) { ++uniShadE; uniShadT += tcL[ci][1]; }
			else                               { ++mixedE;   mixedT   += tcL[ci][1]; }
		}
		// Same question at BxB block granularity (--shadow_tap_census_block).
		long long bUniT = 0, bMixT = 0, bUniB = 0, bMixB = 0;
		if (tcBlk) {
			for (int bi = 0; bi < tcBlocks; ++bi) {
				const uint32_t *b = tcBlk + size_t(bi) * size_t(tcN) * 3u;
				for (int ci = 0; ci < tcN; ++ci) {
					const uint32_t tp = b[ci*3+0];
					if (tp == 0) continue;
					if (b[ci*3+1] == tp || b[ci*3+2] == tp) { ++bUniB; bUniT += tp; }
					else                                    { ++bMixB; bMixT += tp; }
				}
			}
		}
		static std::atomic<long long> aTiles{0}, aPx{0}, aE{0}, aEDead{0}, aECube{0},
			aECubeDead{0}, aTaps{0}, aLit{0}, aShad{0}, aUniLitE{0}, aUniShadE{0},
			aMixedE{0}, aUniLitT{0}, aUniShadT{0}, aMixedT{0}, aDeadPx{0},
			aBUniT{0}, aBMixT{0}, aBUniB{0}, aBMixB{0};
		const auto ADD = [](std::atomic<long long> &a, long long v) {
			a.fetch_add(v, std::memory_order_relaxed);
		};
		ADD(aPx, tcPixels);   ADD(aE, e);           ADD(aEDead, eDead);
		ADD(aECube, eCube);   ADD(aECubeDead, eCubeDead);
		ADD(aTaps, taps);     ADD(aLit, lit);       ADD(aShad, shad);
		ADD(aUniLitE, uniLitE);   ADD(aUniShadE, uniShadE); ADD(aMixedE, mixedE);
		ADD(aUniLitT, uniLitT);   ADD(aUniShadT, uniShadT); ADD(aMixedT, mixedT);
		ADD(aDeadPx, deadPx);
		ADD(aBUniT, bUniT); ADD(aBMixT, bMixT); ADD(aBUniB, bUniB); ADD(aBMixB, bMixB);
		const long long t = aTiles.fetch_add(1, std::memory_order_relaxed) + 1;
		if (t > 0 && (t % (DEFERRED_NUM_TILES * 8)) == 0) {
			// DELTA since the previous report, not a running mean: the first
			// main-view frames of a bench carry lazy first-touch work and a
			// cumulative mean would take the whole run to shed them.
			static long long pTiles=0, pPx=0, pE=0, pEDead=0, pECube=0, pECubeDead=0,
				pTaps=0, pLit=0, pShad=0, pUniLitE=0, pUniShadE=0, pMixedE=0,
				pUniLitT=0, pUniShadT=0, pMixedT=0, pDeadPx=0,
				pBUniT=0, pBMixT=0, pBUniB=0, pBMixB=0;
			const auto D = [](std::atomic<long long> &a, long long &prev) {
				const long long v = a.load(std::memory_order_relaxed);
				const long long d = v - prev; prev = v; return d;
			};
			const long long dTiles=D(aTiles,pTiles);
			const long long dPx=D(aPx,pPx), dE=D(aE,pE), dEDead=D(aEDead,pEDead);
			const long long dECube=D(aECube,pECube), dECubeDead=D(aECubeDead,pECubeDead);
			const long long dTaps=D(aTaps,pTaps), dLit=D(aLit,pLit), dShad=D(aShad,pShad);
			const long long dUniLitE=D(aUniLitE,pUniLitE), dUniShadE=D(aUniShadE,pUniShadE);
			const long long dMixedE=D(aMixedE,pMixedE);
			const long long dUniLitT=D(aUniLitT,pUniLitT), dUniShadT=D(aUniShadT,pUniShadT);
			const long long dMixedT=D(aMixedT,pMixedT), dDeadPx=D(aDeadPx,pDeadPx);
			const long long dBUniT=D(aBUniT,pBUniT), dBMixT=D(aBMixT,pBMixT);
			const long long dBUniB=D(aBUniB,pBUniB), dBMixB=D(aBMixB,pBMixB);
			(void)dECube;
			const double f  = std::max(1.0, double(dTiles) / double(DEFERRED_NUM_TILES));
			const double ee = std::max(1.0, double(dE));
			const double tt = double(dTaps);
			std::fprintf(stderr,
			  "[SHADOW-TAP-CENSUS] %.0f main-view frames (%lld tiles) since last report\n"
			  "  px/frame            %.0f      tile-light entries/frame %.1f (%.2f lights/tile)\n"
			  "  DEAD entries        %.1f/frame = %.1f%% of entries   (0 in-range px; %.1f of them carry a cube)\n"
			  "  dead prologue px    %.2f M/frame  (entries x tile px they were iterated over)\n"
			  "  cube taps           %.3f M/frame   lit %.1f%%  shadowed %.1f%%  partial %.1f%%\n"
			  "  tile-UNIFORM taps   all-lit %.3f M (%.1f%%)  all-shadowed %.3f M (%.1f%%)  mixed %.3f M (%.1f%%)\n"
			  "  entries by class    uni-lit %.1f  uni-shad %.1f  mixed %.1f  per frame\n",
			  f, dTiles,
			  double(dPx)/f, ee/f, ee/std::max(1.0, double(dTiles)),
			  double(dEDead)/f, 100.0*double(dEDead)/ee,
			  double(dECubeDead)/f,
			  double(dDeadPx)/f/1e6,
			  tt/f/1e6,
			  tt > 0 ? 100.0*double(dLit)/tt : 0.0,
			  tt > 0 ? 100.0*double(dShad)/tt : 0.0,
			  tt > 0 ? 100.0*(tt - double(dLit) - double(dShad))/tt : 0.0,
			  double(dUniLitT)/f/1e6,  tt > 0 ? 100.0*double(dUniLitT)/tt : 0.0,
			  double(dUniShadT)/f/1e6, tt > 0 ? 100.0*double(dUniShadT)/tt : 0.0,
			  double(dMixedT)/f/1e6,   tt > 0 ? 100.0*double(dMixedT)/tt : 0.0,
			  double(dUniLitE)/f, double(dUniShadE)/f, double(dMixedE)/f);
#if FDS_SHADOW_TAP_CENSUS
			// PolyId 8x8 uniformity pyramid: where the taps that reached it
			// went. Counted per-thread inside CubeShadow_Sample, summed here so
			// it shares this report's frame accounting; DELTA since the last
			// report, same as every row above.
			{
				static ShadowPyrCensus pPyr{};
				ShadowPyrCensus cur; ShadowPyrCensusTotals(cur);
				const double dR   = double(cur.reached - pPyr.reached);
				const double dNP  = double(cur.noPyr   - pPyr.noPyr);
				const double dFL  = double(cur.fastLit - pPyr.fastLit);
				const double dFO  = double(cur.fastOcc - pPyr.fastOcc);
				const double dMX  = double(cur.mixed   - pPyr.mixed);
				const double dDO  = double(cur.dynOnly - pPyr.dynOnly);
				const double dSR  = double(cur.spotReached - pPyr.spotReached);
				const double dSNP = double(cur.spotNoPyr   - pPyr.spotNoPyr);
				const double dSFL = double(cur.spotFastLit - pPyr.spotFastLit);
				const double dSFO = double(cur.spotFastOcc - pPyr.spotFastOcc);
				const double dSMX = double(cur.spotMixed   - pPyr.spotMixed);
				pPyr = cur;
				const double rr = std::max(1.0, dR);
				const double sr = std::max(1.0, dSR);
				std::fprintf(stderr,
				  "  PYRAMID cube tap    reached %.3f M/f (dyn-only form %.1f%%)  no pyramid %.3f M\n"
				  "    SKIPPED           uniform-lit %.3f M (%.1f%%)  uniform-occ %.3f M (%.1f%%)"
				  "   => %.1f%% of taps take NO texel read\n"
				  "    tapped            mixed %.3f M (%.1f%%)\n"
				  "  PYRAMID spot tap    reached %.3f M/f  no pyramid %.3f M\n"
				  "    SKIPPED           uniform-lit %.3f M (%.1f%%)  uniform-occ %.3f M (%.1f%%)"
				  "   => %.1f%% of taps take NO texel read\n"
				  "    tapped            mixed %.3f M (%.1f%%)\n",
				  dR/f/1e6, 100.0*dDO/rr, dNP/f/1e6,
				  dFL/f/1e6, 100.0*dFL/rr, dFO/f/1e6, 100.0*dFO/rr,
				  100.0*(dFL+dFO)/rr,
				  dMX/f/1e6, 100.0*dMX/rr,
				  dSR/f/1e6, dSNP/f/1e6,
				  dSFL/f/1e6, 100.0*dSFL/sr, dSFO/f/1e6, 100.0*dSFO/sr,
				  100.0*(dSFL+dSFO)/sr,
				  dSMX/f/1e6, 100.0*dSMX/sr);
			}
#endif
			if (tcB > 0) {
				const double bt = double(dBUniT + dBMixT);
				std::fprintf(stderr,
				  "  block %dx%d          UNIFORM taps %.3f M (%.1f%%)  mixed %.3f M (%.1f%%)"
				  "   uniform blocks %.0f%% of (block x light) pairs\n",
				  tcB, tcB, double(dBUniT)/f/1e6, bt > 0 ? 100.0*double(dBUniT)/bt : 0.0,
				  double(dBMixT)/f/1e6, bt > 0 ? 100.0*double(dBMixT)/bt : 0.0,
				  (dBUniB + dBMixB) > 0 ? 100.0*double(dBUniB)/double(dBUniB + dBMixB) : 0.0);
			}
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

#if FDS_OMNI_CENSUS
	if (omniCensus) {
		for (int i = 0; i < OMNI_CEN_N; ++i)
			if (ocn[i]) g_omniCen[i].fetch_add(ocn[i], std::memory_order_relaxed);
	}
#endif
#if FDS_ABL_SINK
	g_omniAblSink = ablSink;   // one volatile store per tile call
#endif

	// One permit per completed tile (see renderns::tileDone in RENDER.CPP).
	// SKIPPED for an INLINE (offscreen-bake) dispatch: the calling thread would
	// immediately re-acquire its own permit, and that shared semaphore is
	// contended by every pool thread — 3.4-4.0 us of CORE time per round trip
	// at 12 threads, paid 96x per 64x64 shard cell. See
	// DeferredLightingCtx::inlineDispatch.
	if (!ctx.inlineDispatch) renderns::tileDone.release();
}

// Per-pixel deferred lighting for transparent (front-facing) surfaces.
// Reads the transparent G-buffer (mat32 + normal + xpr-Z, populated by
// MekaleleTransparent in RenderInnerDeferredTransparent for the closest
// front-facing transparent at each pixel). Computes per-pixel ambient
// + Lambertian + fog, modulates the texture sample, alpha-blends 50/50
// onto VPage (matches TheOtherBarry<TRANSPARENT>'s blend rule). Specular
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
	const int XRes = ctx.xres;
	byte *const VPage = ctx.vpage;
	word *const ZPage16 = ctx.zpage16;
	const float CntrEX = ctx.cntrEX, CntrEY = ctx.cntrEY;
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
	dword *out = reinterpret_cast<dword *>(VPage);
	// Conservative per-pixel light cull: bypasses the per-tile filter,
	// useful for diagnosing whether observed lighting steps come from
	// culling drift at tile seams.
	const bool lightAll = fds::FeatureFlags::xpar_light_all();
	const bool hdrBufReady = fds::Hdr_WritableFor(ctx.xres, ctx.yres);  // main pass only (not the mirror RTT)
	const bool hdrLinear = hdrBufReady && fds::FeatureFlags::hdr() && fds::FeatureFlags::hdr_linear();  // HDR C2
	// Procedural water composite (Stage B) — fresnel mix of a deep colour and the
	// reflection underlay, for the water matID only. Hoisted; per-pixel fresnel below.
	// Per-SURFACE opt-in/out: the water material's tri-state WaterProcMode
	// (sidecar 'waterProcedural': -1 off / 0 auto / 1 on) wins; auto falls back
	// to the global --water_procedural flag (byte-identical when no sidecar
	// line exists). Same tri-state pattern as EnvReflMode.
	// The fresnel deep-colour composite is a separate opt-out from the
	// procedural field: chase keeps the field (ripple/glints/caustics) but
	// takes the forward-style lit-texel blend — its night-sky reflection
	// underlay is black, so the deep colour reads as a dark view-angle band
	// (the "chase water dark band"; soa-vertex 9902349).
	bool waterProcOn = fds::FeatureFlags::water_procedural() &&
	                   fds::FeatureFlags::water_fresnel_composite();
	if (ctx.waterMatID >= 0 && dword(ctx.waterMatID) < ctx.matTable.count) {
		if (const Material *WM = ctx.matTable.data[ctx.waterMatID]) {
			if      (WM->WaterProcMode > 0) waterProcOn = true;
			else if (WM->WaterProcMode < 0) waterProcOn = false;
		}
	}
	const float wDeepB = fds::FeatureFlags::water_deep_b();
	const float wDeepG = fds::FeatureFlags::water_deep_g();
	const float wDeepR = fds::FeatureFlags::water_deep_r();
	const float wRefl  = fds::FeatureFlags::water_reflectivity();
	const float wFresBase = fds::FeatureFlags::water_fresnel_base();   // reflection floor looking down
	// ── --xpar-pbr: PBR-shaded transparents (see the per-pixel block below).
	//    Hoisted once per tile like the opaque kernel's flag cache; when the
	//    flag is off every gate below is false and the path costs nothing. ──
	const bool  xparPbrOn       = fds::FeatureFlags::xpar_pbr();
	const bool  xpNmapOn        = xparPbrOn && !fds::FeatureFlags::no_nmap();
	const int   xpNmapFadeStart = fds::FeatureFlags::nmap_lod_fade_start();
	const float xpNmapFadeStep  = fds::FeatureFlags::nmap_lod_fade_step();
	const bool  xpAoOn          = xparPbrOn && fds::FeatureFlags::ao_map();
	const float xpAoK           = fds::FeatureFlags::ao_map_strength();
	const bool  xpRoughOn       = xparPbrOn && fds::FeatureFlags::roughness_map();
	const float xpRoughK        = fds::FeatureFlags::roughness_strength();
	const bool  xpMetalOn       = xparPbrOn && fds::FeatureFlags::metal_map();
	// ── LIGHT TILE RESOLUTION (see DeferredLightingCtx::ltNumX) ───────────────
	// `tileIndex` is this job's ordinal in renderFrame's FRAME tiler
	// (RENDER.CPP runTilePass, NT = numTilesX*numTilesY, default 6x5 = 30 and
	// tunable via --frame_tile_x/y). ctx.tileLights[] is built on the deferred
	// 12x8 LIGHTING grid (96 entries). Subscripting the second array with the
	// first grid's ordinal is a category error that never trips a bounds check
	// (30 < 96) and never crashes — it just lights every transparent pixel from
	// a FOREIGN region of the screen. The mis-map is constant within a
	// frame-tile row, so at 1512x848 it lands as horizontal bands of wrong
	// lighting exactly tileSizeY = (848+4)/5 & ~7 = 168 rows tall, visible only
	// on transparent surfaces — i.e. on the greets mirror.
	// Resolve the light tile from the PIXEL instead. A default 6x5 frame tile is
	// 248x168 px and a 12x8 light tile is 128x106, so one dispatch rect spans
	// several light tiles: the lookup has to be per pixel, not per job.
	// THIS IS ALSO WHY --frame_tile_x/y CANNOT DESYNC THE LIGHT GRID: the fixed
	// path reads ltNumX/ltSizeY, which travel with the light ARRAY (set from the
	// 12x8 grid at the bottom of Render_DeferredLighting, and from the strip
	// geometry on the TBR path), never from the frame tiler. Only the LEGACY
	// --no-xpar_tile_lights fallback below reads the frame ordinal, and a grid
	// finer than 96 tiles makes its clamp collapse every tile past 95 onto the
	// last light list — one more reason that arm is not the shipping one.
	const bool xparTileFix = fds::FeatureFlags::xpar_tile_lights();
	const int  ltNumX  = ctx.ltNumX  > 0 ? ctx.ltNumX  : 1;
	const int  ltNumY  = ctx.ltNumY  > 0 ? ctx.ltNumY  : 1;
	const int  ltSizeX = ctx.ltSizeX > 0 ? ctx.ltSizeX : (XRes > 0 ? XRes : 1);
	const int  ltSizeY = ctx.ltSizeY > 0 ? ctx.ltSizeY : (ctx.yres > 0 ? ctx.yres : 1);
	const int  ltLegacy = (tileIndex < 0) ? 0
	                    : (tileIndex >= DEFERRED_NUM_TILES ? DEFERRED_NUM_TILES - 1 : tileIndex);
	auto lightTileAt = [&](int px, int py) -> const TileLights & {
		if (!xparTileFix) return ctx.tileLights[ltLegacy];
		int ti = px / ltSizeX; if (ti < 0) ti = 0; if (ti >= ltNumX) ti = ltNumX - 1;
		int tj = py / ltSizeY; if (tj < 0) tj = 0; if (tj >= ltNumY) tj = ltNumY - 1;
		return ctx.tileLights[tj * ltNumX + ti];
	};
	const ViewLightsSoA *vlAll = ctx.lights;
	const int allCount = ctx.numLights;

	// ── Screen-space glass refraction (--glass_refract). When off, glassRefrOn
	//    is false and NONE of the offset math runs — the composite below reads
	//    the background straight-through (byte-identical). See FeatureFlags.def. ──
	const int   YRes         = ctx.yres;
	const float glassRefr    = fds::FeatureFlags::glass_refract();
	const bool  glassRefrOn  = glassRefr > 0.0f;
	const float glassIor     = fds::FeatureFlags::glass_refract_ior();
	const float glassEta     = glassIor > 0.0f ? 1.0f / glassIor : 1.0f;
	const float glassMax     = fds::FeatureFlags::glass_refract_max();
	const float glassBlur    = fds::FeatureFlags::glass_refract_rough_blur();
	const float glassF0lin   = (glassIor - 1.0f) / (glassIor + 1.0f);
	const float glassF0      = glassF0lin * glassF0lin;
	// Per-material IOR override (Material::RefractIor > 0, editor/sidecar
	// 'refractIor'). eta/F0 are memoized on the last glass material seen so a
	// facet's pixel run pays the divide once, not per pixel; materials with no
	// override recompute the same 1/ior + Schlick F0 expressions from the
	// global IOR — identical inputs, identical ops → byte-identical output.
	const Material *glassIorMat = nullptr;
	float glassEtaPx = glassEta, glassF0Px = glassF0;
	const float glassFOVX    = ctx.invFOVX != 0.0f ? 1.0f / ctx.invFOVX : 0.0f;
	const float glassFOVY    = ctx.invFOVY != 0.0f ? 1.0f / ctx.invFOVY : 0.0f;
	auto glassClampX = [XRes](int v) { return v < 0 ? 0 : (v >= XRes ? XRes - 1 : v); };
	auto glassClampY = [YRes](int v) { return v < 0 ? 0 : (v >= YRes ? YRes - 1 : v); };
	// Refracted background fetch (optionally roughness-blurred), HDR float path.
	// LEGACY peel: refract the per-layer snapshot renderFrame captured before this
	// glass batch (g_glassRefrHdr) — a stable full-frame buffer holding everything
	// behind this layer. TBR peel: refract the strip's own per-clump band snapshot
	// (t_glassBandHdr, layered) for in-band rows and the immutable pre-TBR opaque
	// snapshot for out-of-band rows. Reading the live buffer at an offset would race
	// the concurrent peel → flicker, so both sources are stable snapshots.
	const fds::hdrf *glassBgHdr = fds::g_glassRefrHdr.empty()
	                          ? fds::g_hdrBuf.data() : fds::g_glassRefrHdr.data();
	const dword *glassBgLdr = fds::g_glassRefrLdr.empty()
	                          ? out : fds::g_glassRefrLdr.data();
	// TBR strip-band snapshot (see glassBandSnapshotBegin). Inactive on the legacy
	// path (thread_local bounds are 0 there), so its sampler code is byte-identical.
	const bool   glassBandOn  = (t_glassBandY1 > t_glassBandY0);
	const int    glassBandY0  = t_glassBandY0;
	const int    glassBandY1  = t_glassBandY1;
	const fds::hdrf *glassBandHdr = glassBandOn ? t_glassBandHdr.data() : nullptr;
	const dword *glassBandLdr = glassBandOn ? t_glassBandLdr.data() : nullptr;
	// Per-tap address in TBR band mode. The layered background only exists for
	// THIS strip's rows (the neighbour strips are being composited concurrently
	// and can't be read); refracting a vertical offset that leaves the strip would
	// fall back to the OPAQUE snapshot and, because in-band=layered vs
	// out-of-band=opaque differ, paint a hard 8px seam (visible horizontal
	// striping). So CLAMP the vertical sample into the strip band: horizontal
	// refraction is unrestricted (full-width layered), vertical refraction is
	// capped at the strip so the fetch stays continuous and layered (inner shows
	// through outer) with NO seam. This is the deterministic, artefact-free TBR
	// approximation; full-range vertical layering needs the barrier the per-strip
	// parallel peel doesn't have (see notes above / the report).
	auto glassBandClampY = [&](int sy) {
		if (sy < glassBandY0) return glassBandY0;
		if (sy >= glassBandY1) return glassBandY1 - 1;
		return sy;
	};
	auto glassTapHdr = [&](int sx, int sy) -> const fds::hdrf* {
		sx = glassClampX(sx); sy = glassBandClampY(sy);
		return glassBandHdr + (size_t(sy - glassBandY0) * XRes + sx) * 4;
	};
	auto glassTapLdr = [&](int sx, int sy) -> dword {
		sx = glassClampX(sx); sy = glassBandClampY(sy);
		return glassBandLdr[size_t(sy - glassBandY0) * XRes + sx];
	};
	auto sampleBgHdr = [&](int sx, int sy, float r, float &oB, float &oG, float &oR) {
		if (glassBandOn) {
			const int ri = int(r + 0.5f);
			if (ri <= 0) { const fds::hdrf *p = glassTapHdr(sx, sy); oB = p[0]; oG = p[1]; oR = p[2]; return; }
			const int ox[5] = {0, -ri, ri, 0, 0}, oy[5] = {0, 0, 0, -ri, ri};
			float aB = 0, aG = 0, aR = 0;
			for (int t = 0; t < 5; ++t) { const fds::hdrf *p = glassTapHdr(sx + ox[t], sy + oy[t]); aB += p[0]; aG += p[1]; aR += p[2]; }
			oB = aB * 0.2f; oG = aG * 0.2f; oR = aR * 0.2f; return;
		}
		const fds::hdrf *base = glassBgHdr;
		const int ri = int(r + 0.5f);
		if (ri <= 0) {
			const fds::hdrf *p = base + (size_t(sy) * XRes + sx) * 4;
			oB = p[0]; oG = p[1]; oR = p[2]; return;
		}
		const int ox[5] = {0, -ri, ri, 0, 0}, oy[5] = {0, 0, 0, -ri, ri};
		float aB = 0, aG = 0, aR = 0;
		for (int t = 0; t < 5; ++t) {
			const fds::hdrf *p = base + (size_t(glassClampY(sy + oy[t])) * XRes + glassClampX(sx + ox[t])) * 4;
			aB += p[0]; aG += p[1]; aR += p[2];
		}
		oB = aB * 0.2f; oG = aG * 0.2f; oR = aR * 0.2f;
	};
	// Refracted background fetch (optionally roughness-blurred), LDR VPage path.
	auto sampleBgLdr = [&](int sx, int sy, float r, int &oB, int &oG, int &oR) {
		if (glassBandOn) {
			const int ri = int(r + 0.5f);
			if (ri <= 0) { const dword e = glassTapLdr(sx, sy); oB = int(e & 0xFF); oG = int((e >> 8) & 0xFF); oR = int((e >> 16) & 0xFF); return; }
			const int ox[5] = {0, -ri, ri, 0, 0}, oy[5] = {0, 0, 0, -ri, ri};
			int aB = 0, aG = 0, aR = 0;
			for (int t = 0; t < 5; ++t) { const dword e = glassTapLdr(sx + ox[t], sy + oy[t]); aB += int(e & 0xFF); aG += int((e >> 8) & 0xFF); aR += int((e >> 16) & 0xFF); }
			oB = aB / 5; oG = aG / 5; oR = aR / 5; return;
		}
		const int ri = int(r + 0.5f);
		if (ri <= 0) {
			const dword e = glassBgLdr[size_t(sy) * XRes + sx];
			oB = int(e & 0xFF); oG = int((e >> 8) & 0xFF); oR = int((e >> 16) & 0xFF); return;
		}
		const int ox[5] = {0, -ri, ri, 0, 0}, oy[5] = {0, 0, 0, -ri, ri};
		int aB = 0, aG = 0, aR = 0;
		for (int t = 0; t < 5; ++t) {
			const dword e = glassBgLdr[size_t(glassClampY(sy + oy[t])) * XRes + glassClampX(sx + ox[t])];
			aB += int(e & 0xFF); aG += int((e >> 8) & 0xFF); aR += int((e >> 16) & 0xFF);
		}
		oB = aB / 5; oG = aG / 5; oR = aR / 5;
	};

	for (int py = y1; py < y2; ++py) {
		for (int px = x1; px < x2; ++px) {
			const size_t i = size_t(py) * XRes + px;
			const uint32_t mat32 = gbX.txtr[i];
			if (mat32 == 0xFFFFFFFFu) continue;  // no transparent front
			const word zEnc = xparZ[i];
			if (zEnc == 0) continue;
			// Per-pixel mirror id from the transparent layer's plane.
			// Used to gate the light loops below (clone surfaces should
			// see only their own mirror's omnis).
			const uint32_t pmid = gbX.mirrorId.empty()
			    ? 0u : uint32_t(gbX.mirrorId[i]);
			// Opaque-Z occlusion: if opaque ZPage16 has a larger z_candidate
			// at this pixel, the opaque surface is CLOSER to camera than
			// the transparent we wrote into xpr. Reject the transparent —
			// it's hidden behind opaque. MekaleleTransparent doesn't see
			// opaque Z during raster (its zbuffer points at xpr-Z) so we
			// have to gate here. Without this, transparent surfaces show
			// through opaque walls (Greets banding behind marble).
			const word zOpaque = ZPage16[i];
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
			const float texB = float(texel & 0xFF)         * Mat->TintB;  // editor tint (see main kernel)
			const float texG = float((texel >> 8) & 0xFF)  * Mat->TintG;
			const float texR = float((texel >> 16) & 0xFF) * Mat->TintR;

			float nx, ny, nz;
			meka::oct_decode_u32(gbX.normal[i], nx, ny, nz);

			const float z = float(0xFF80 - zEnc) * ctx.invZScale;
			const float x = (float(px) - CntrEX) * z * ctx.invFOVX;
			const float y = (CntrEY - float(py)) * z * ctx.invFOVY;

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

			// ── --xpar-pbr: PBR-shaded transparents (default OFF = byte-
			//    identical). Bring the opaque kernel's per-pixel PBR stack to
			//    the transparent LIT inputs — the alpha/additive blend math
			//    below is untouched. Here: (1) NormalMap perturbs the shading
			//    normal used by the light loop + specular (same decode + LOD
			//    fade + TBN as the opaque kernel; tangent is the Mikkelsen
			//    fallback since the xpar G-buffer carries no tangent plane);
			//    (2) AO (albedo-alpha or separate AoMap) attenuates the
			//    ambient term BEFORE the omni loop adds direct light — the
			//    opaque convention (direct light is shadowed, not AO'd).
			//    Roughness/metallic follow after the light loop. Water keeps
			//    its dedicated path (fresnel/deep-colour, no PBR maps). ──
			const bool xparPbrPx = xparPbrOn && !isWater;
			bool xparPbrNormApplied = false;
			if (xparPbrPx) {
				if (xpNmapOn && Mat->NormalMap) {
					float nmX, nmY, nmZ;
					if (decodeNormalTexel(Mat->NormalMap, miplevel, swizzledUV, nmX, nmY, nmZ)) {
						// LOD-aware bump fade — mirrors the opaque kernel
						// (averaged normal mips turn into lighting noise).
						if (int(miplevel) >= xpNmapFadeStart) {
							const int   over = int(miplevel) - xpNmapFadeStart + 1;
							const float fade = 1.0f - float(over) * xpNmapFadeStep;
							const float s = fade > 0.0f ? fade : 0.0f;
							nmX *= s; nmY *= s;
						}
						// Mikkelsen on-the-fly tangent (the opaque kernel's
						// fallback; same reconstruction the glass-refraction
						// block uses — which reuses THIS perturbed normal
						// instead of re-deriving, see xparPbrNormApplied).
						float rfx, rfy, rfz;
						if (std::fabs(ny) < 0.9f) { rfx = 0.0f; rfy = 1.0f; rfz = 0.0f; }
						else                      { rfx = 1.0f; rfy = 0.0f; rfz = 0.0f; }
						float tx = rfy*nz - rfz*ny, ty = rfz*nx - rfx*nz, tz = rfx*ny - rfy*nx;
						const float tinv = fast_rsqrt(tx*tx + ty*ty + tz*tz);
						tx *= tinv; ty *= tinv; tz *= tinv;
						// B = handedness·(N×T) — matches the opaque kernel's
						// mirrored-UV handling (rooms/floor clones carry -1).
						const float hsign = Mat->TbnHandedness;
						const float bx = (ny*tz - nz*ty) * hsign;
						const float by = (nz*tx - nx*tz) * hsign;
						const float bz = (nx*ty - ny*tx) * hsign;
						float vnx = tx*nmX + bx*nmY + nx*nmZ;
						float vny = ty*nmX + by*nmY + ny*nmZ;
						float vnz = tz*nmX + bz*nmY + nz*nmZ;
						const float vinv = fast_rsqrt(vnx*vnx + vny*vny + vnz*vnz);
						nx = vnx*vinv; ny = vny*vinv; nz = vnz*vinv;
						xparPbrNormApplied = true;
					}
				}
				if (xpAoOn) {
					const bool aoInAlpha = (Mat->Flags & Mat_AoInAlpha) != 0;
					float ao = 1.0f;
					bool haveAo = false;
					if (aoInAlpha) {
						ao = float((texel >> 24) & 0xFF) * (1.0f/255.0f);  // free (albedo alpha)
						haveAo = true;
					} else if (Mat->AoMap && miplevel < Mat->AoMap->numMipmaps
					           && Mat->AoMap->Mipmap[miplevel]) {
						// Separate AoMap: 8-bit (MakeHeight8 import) reads the
						// byte directly; 32-bit BGRA takes the luminance like
						// the opaque kernel. Same swizzledUV/miplevel as the
						// diffuse (import enforces matching dims); a missing
						// mip skips gracefully (haveAo stays false).
						if (Mat->AoMap->BPP == 8) {
							ao = float(reinterpret_cast<const byte*>(
								Mat->AoMap->Mipmap[miplevel])[swizzledUV]) * (1.0f/255.0f);
						} else {
							const dword aoTexel = reinterpret_cast<const dword*>(
								Mat->AoMap->Mipmap[miplevel])[swizzledUV];
							ao = (float(aoTexel & 0xFF)         * 0.114f
							    + float((aoTexel >> 8)  & 0xFF) * 0.587f
							    + float((aoTexel >> 16) & 0xFF) * 0.299f) * (1.0f/255.0f);
						}
						haveAo = true;
					}
					if (haveAo) {
						// Global dial × per-material dial — opaque convention.
						ao = 1.0f - xpAoK * Mat->AoStrength * (1.0f - ao);
						lB *= ao; lG *= ao; lR *= ao;
					}
				}
			}

			// ── Glass refraction: pick the background sample pixel for this
			//    fragment. Off / non-glass → straight through (byte-identical
			//    composite below). See FeatureFlags::glass_refract. ──
			int   refrSX = px, refrSY = py;
			float refrBlurRad = 0.0f;
			// Refract EVERY non-water transparent (flat OR normal-mapped). A flat
			// glass facet (no NormalMap — e.g. greets 'amudim' screens) refracts
			// through its GEOMETRIC normal: a uniform screen offset across the whole
			// facet by its tilt, exactly what flat glass should do. Normal-mapped
			// glass adds the relief ripple on top. Both stay LAYERED because the
			// per-layer snapshot (legacy: renderFrame's per-batch snapshotGlassBg;
			// TBR: the barrier-per-layer full-frame snapshot) captures everything
			// already composited behind this layer. (The earlier NormalMap gate was
			// an interim fix for the pre-layered single-opaque-snapshot collapse.)
			// Refraction is OPT-IN per material (Mat_Refractive): only surfaces the
			// scene marked as real glass (fountain 'mizraka glass' + 'f_sphere' orb
			// shells, greets 'amudim' panels) refract. Every OTHER transparent —
			// notably the fountain's fiery 'f in shpere' orb core (plain transparent,
			// no Mat_Additive) — composites/blends normally so it GLOWS layered
			// through the glass instead of refracting the box behind it. Water and
			// additive glows never carried Mat_Refractive, so they never refract.
			// NOTE: the deeper foreground-occlusion guard (a displaced fetch can grab
			// a pixel IN FRONT of the glass, which a plain transparent's z-test would
			// hide) is a separate depth-guard TODO.
			const bool glassRefrPx = glassRefrOn && !isWater
			                         && (Mat->Flags & Mat_Refractive) != 0;
			if (glassRefrPx) {
				// Per-material IOR: Mat->RefractIor > 0 overrides the global
				// glass_refract_ior (see the memo comment at the hoist above).
				if (Mat != glassIorMat) {
					glassIorMat = Mat;
					const float ior = Mat->RefractIor > 0.0f ? Mat->RefractIor : glassIor;
					glassEtaPx = ior > 0.0f ? 1.0f / ior : 1.0f;
					const float f0l = (ior - 1.0f) / (ior + 1.0f);
					glassF0Px = f0l * f0l;
				}
				// Perturbed view-space normal from the material normal map via an
				// on-the-fly (Mikkelsen) tangent — the transparent G-buffer carries
				// no tangent plane, and the exact tangent rotation is irrelevant to
				// the relief-warp magnitude. No normal map → geometric normal (flat
				// facets refract uniformly through the facet tilt).
				// --xpar-pbr already perturbed nx/ny/nz with the same
				// reconstruction (xparPbrNormApplied) — reuse it, don't
				// perturb twice. Flag off → false → byte-identical.
				float pnx = nx, pny = ny, pnz = nz;
				if (Mat->NormalMap && !xparPbrNormApplied) {
					float nmX, nmY, nmZ;
					if (decodeNormalTexel(Mat->NormalMap, miplevel, swizzledUV, nmX, nmY, nmZ)) {
						float rfx, rfy, rfz;
						if (std::fabs(ny) < 0.9f) { rfx = 0.0f; rfy = 1.0f; rfz = 0.0f; }
						else                      { rfx = 1.0f; rfy = 0.0f; rfz = 0.0f; }
						float tx = rfy*nz - rfz*ny, ty = rfz*nx - rfx*nz, tz = rfx*ny - rfy*nx;
						const float tinv = fast_rsqrt(tx*tx + ty*ty + tz*tz);
						tx *= tinv; ty *= tinv; tz *= tinv;
						const float bx = ny*tz - nz*ty, by = nz*tx - nx*tz, bz = nx*ty - ny*tx;
						float vnx = tx*nmX + bx*nmY + nx*nmZ;
						float vny = ty*nmX + by*nmY + ny*nmZ;
						float vnz = tz*nmX + bz*nmY + nz*nmZ;
						const float vinv = fast_rsqrt(vnx*vnx + vny*vny + vnz*vnz);
						pnx = vnx*vinv; pny = vny*vinv; pnz = vnz*vinv;
					}
				}
				// Unit view ray (eye → pixel; view-space camera at the origin).
				const float vl2  = x*x + y*y + z*z;
				const float dinv = fast_rsqrt(vl2 > 0.0f ? vl2 : 1.0f);
				const float dx = x*dinv, dy = y*dinv, dz = z*dinv;
				// Viewer-side normal (dot(N,d) < 0), matching the env-compose flip.
				if (pnx*dx + pny*dy + pnz*dz > 0.0f) { pnx = -pnx; pny = -pny; pnz = -pnz; }
				const float cosi = -(pnx*dx + pny*dy + pnz*dz);            // ≥ 0
				const float kk = 1.0f - glassEtaPx*glassEtaPx*(1.0f - cosi*cosi);
				if (kk > 0.0f && z > 1e-3f) {                              // else TIR → straight
					const float nds = glassEtaPx*cosi - std::sqrt(kk);
					const float tX = glassEtaPx*dx + nds*pnx;
					const float tY = glassEtaPx*dy + nds*pny;
					const float tZ = glassEtaPx*dz + nds*pnz;
					if (tZ > 1e-3f) {
						// Schlick Fresnel (F0 from IOR): scale the offset by 1-F so
						// head-on refracts most, grazing least (reflection dominates).
						const float om   = 1.0f - cosi;
						const float fres = glassF0Px + (1.0f - glassF0Px) * (om*om*om*om*om);
						const float sc   = glassRefr * (1.0f - fres);
						// Offset = (pixel the refracted ray points at) − (this pixel,
						// = the straight-through ray). Depth-independent angular
						// projection: px = CntrEX + FOVX·(dx/dz).
						float dpx =  glassFOVX * (tX/tZ - dx/dz) * sc;
						float dpy = -glassFOVY * (tY/tZ - dy/dz) * sc;
						if (dpx >  glassMax) dpx =  glassMax; else if (dpx < -glassMax) dpx = -glassMax;
						if (dpy >  glassMax) dpy =  glassMax; else if (dpy < -glassMax) dpy = -glassMax;
						refrSX = glassClampX(px + int(dpx));
						refrSY = glassClampY(py + int(dpy));
						// Frosted-glass blur radius, scaled by per-pixel roughness.
						if (glassBlur > 0.0f) {
							float rough = 1.0f;
							if (Mat->RoughnessMap && miplevel < Mat->RoughnessMap->numMipmaps
							    && Mat->RoughnessMap->Mipmap[miplevel]) {
								rough = float(reinterpret_cast<const byte*>(
									Mat->RoughnessMap->Mipmap[miplevel])[swizzledUV]) * (1.0f/255.0f);
							}
							refrBlurRad = glassBlur * rough;
						}
					}
				}
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
				const TileLights &tlTile = lightTileAt(px, py);
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

			// Saturation cap — 8-bit rollover guard only, same rule as the
			// main kernel: when this pixel's composite lands in the float HDR
			// buffer (the write at the bottom gates on g_hdrActive &&
			// hdrBufReady) the UPPER cap is lifted so emissive transparents
			// (luminosity > 1 screens/glass) exceed 255 and bloom; the mirror
			// RTT / LDR path keeps the cap (byte-identical).
			if (!(fds::g_hdrActive && hdrBufReady)) {
				if (lB > 250.0f) lB = 250.0f;
				if (lG > 250.0f) lG = 250.0f;
				if (lR > 250.0f) lR = 250.0f;
			}
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
			// ── --xpar-pbr (continued): metallic + roughness on the lit
			//    inputs, matching the opaque kernel's order — metal kills
			//    diffuse (litRGB here = the texture-modulated lit colour,
			//    both LDR and hdr-linear encodes), roughness dims the
			//    specular accumulator, then metal tints the surviving spec
			//    by the albedo. The opaque kernel's THIRD metal term (env
			//    reflection tinted at F0→1) has no counterpart here — the
			//    xpar composite carries no env term — so a full metal
			//    transparent is diffuse-killed + albedo-tinted spec only.
			//    Skipped for the HDR-mirror panels (hdrRefl already carries
			//    the full baked radiance) and water (own path). Same
			//    swizzledUV/miplevel + numMipmaps guards as the opaque
			//    fetches — absent maps/mips skip gracefully. ──
			if (xparPbrPx && !isHdrRefl) {
				float metalM = 0.0f;
				if (xpMetalOn && Mat->MetallicMap
				    && miplevel < Mat->MetallicMap->numMipmaps
				    && Mat->MetallicMap->Mipmap[miplevel]) {
					metalM = float(reinterpret_cast<const byte*>(
						Mat->MetallicMap->Mipmap[miplevel])[swizzledUV]) * (1.0f/255.0f);
					if (metalM > 0.0f) {
						const float dk = 1.0f - metalM;
						litB = int(float(litB) * dk);
						litG = int(float(litG) * dk);
						litR = int(float(litR) * dk);
					}
				}
				if (xpRoughOn && Mat->RoughnessMap
				    && (sB != 0.0f || sG != 0.0f || sR != 0.0f)
				    && miplevel < Mat->RoughnessMap->numMipmaps
				    && Mat->RoughnessMap->Mipmap[miplevel]) {
					float specMul = 1.0f - xpRoughK * (float(reinterpret_cast<const byte*>(
						Mat->RoughnessMap->Mipmap[miplevel])[swizzledUV]) * (1.0f/255.0f));
					if (specMul < 0.0f) specMul = 0.0f;
					sB *= specMul; sG *= specMul; sR *= specMul;
				}
				if (metalM > 0.0f) {
					const float inv255 = 1.0f / 255.0f;
					sB *= 1.0f - metalM + metalM * texB * inv255;
					sG *= 1.0f - metalM + metalM * texG * inv255;
					sR *= 1.0f - metalM + metalM * texR * inv255;
				}
				// Per-material specular response multiplier (see wave-1);
				// scales every later consumer (LDR add + HDR radiance).
				if (Mat->SpecMul != 1.0f) {
					sB *= Mat->SpecMul; sG *= Mat->SpecMul; sR *= Mat->SpecMul;
				}
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
				fds::hdrf* h = fds::g_hdrBuf.data() + i * 4;   // write target stays at this pixel
				// Background (dst) = the finished opaque radiance, read at the
				// refracted offset for glass (straight-through when the flag is off).
				// hdrf->float on read; the writes below store float->hdrf as before.
				float dB, dG, dR;
				if (glassRefrPx) sampleBgHdr(refrSX, refrSY, refrBlurRad, dB, dG, dR);
				else { dB = h[0]; dG = h[1]; dR = h[2]; }
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
			// Background (dst) read at the refracted offset for glass
			// (straight-through when the flag is off → byte-identical).
			int dB, dG, dR;
			if (glassRefrPx) {
				sampleBgLdr(refrSX, refrSY, refrBlurRad, dB, dG, dR);
			} else {
				const dword existing = out[i];
				dB = int((existing      ) & 0xFF);
				dG = int((existing >>  8) & 0xFF);
				dR = int((existing >> 16) & 0xFF);
			}
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
bool deferredUnifiedTbrEnabled() {
	if (!fds::FeatureFlags::deferred_unified_tbr()) return false;
	if (!CurScene) return false;
	if (!CurScene->SBufferHead || CurScene->NumTiles == 0) return false;
	// Offscreen pass (mirror RTT, cube bake): the YRes global is swapped
	// but the strip arrays are main-sized — fall back to the legacy peel
	// there (see TBR_MatchesTarget).
	if (!TBR_MatchesTarget(CurScene)) return false;
	return true;
}

// Effective transparent depth-peel passes (per side): the CLI/env flag wins
// when explicitly set, otherwise the current scene's per-scene default
// (Scene::XparPeelPasses; 0 → legacy 1). Used by both xpar dispatch paths.
int xparPeelPassesEffective() {
	if (fds::FeatureFlags::isSet(fds::FeatureFlags::IntId::xpar_peel_passes)) {
		const int f = fds::FeatureFlags::xpar_peel_passes();
		return f < 1 ? 1 : f;
	}
	const int p = CurScene ? int(CurScene->XparPeelPasses) : 0;
	return p < 1 ? 1 : p;
}

// ── FDS_XPAR_TRACE: live transparent-composite tracer (dev diagnostic) ──
// When the env var is set, every frame records — lock-free, each strip's
// worker writes only its own row — the per-strip clump composite SEQUENCE
// (material, side, mirror tag, faces, filled pixels). Xtrace_WriteFile then
// snapshots the LAST COMPLETED frame to a file; GREETS's F9 dump calls it so
// a dumped frame carries its own composite census. Silent in the hot path
// (a printf here would serialize the workers and mask scheduling effects).
static constexpr int XT_STRIPS = 512, XT_SLOTS = 64;
static bool        xt_on = false;
static const char* xt_mat[XT_STRIPS][XT_SLOTS];
static uint8_t     xt_front[XT_STRIPS][XT_SLOTS];
static int16_t     xt_mtag[XT_STRIPS][XT_SLOTS];
static uint16_t    xt_nfaces[XT_STRIPS][XT_SLOTS];
static uint32_t    xt_filled[XT_STRIPS][XT_SLOTS];
static int         xt_count[XT_STRIPS];
void Xtrace_FrameBegin() {
	static const bool want = std::getenv("FDS_XPAR_TRACE") != nullptr;
	xt_on = want;
	if (xt_on) std::memset(xt_count, 0, sizeof(xt_count));
}
void Xtrace_WriteFile(const char *path) {
	if (!xt_on) { std::fprintf(stderr, "[XTRACE] not armed (set FDS_XPAR_TRACE=1)\n"); return; }
	FILE *f = std::fopen(path, "wt");
	if (!f) return;
	for (int s = 0; s < XT_STRIPS; ++s)
		for (int i = 0; i < xt_count[s]; ++i)
			std::fprintf(f, "strip=%d seq=%d mat='%s' front=%d mtag=%d nfaces=%u filled=%u\n",
			             s, i, xt_mat[s][i], int(xt_front[s][i]),
			             int(xt_mtag[s][i]), unsigned(xt_nfaces[s][i]),
			             unsigned(xt_filled[s][i]));
	std::fclose(f);
	std::fprintf(stderr, "[XTRACE] wrote %s\n", path);
}

// ── Per-strip dirty-column tracking for the xpar layer slices ─────────────
// A clump flushes on every (mesh, side) change AND on every interleaved
// sprite, so a strip runs many clumps a frame (fountain t=1200: 3224 flushes
// over 135 strips, x4 peel passes). Each of those was clearing and re-scanning
// the FULL 1920 px strip width for a clump that typically covers a handful of
// tile columns.
//
// The invariant that makes bounding both byte-null: outside `dirty`, a strip's
// slice of a side layer is in its CLEARED state (txtr = 0xFFFFFFFF, Z = the
// path's init), and the shared peel floor is zero outside `dirtyFloor`. Each
// clump clears exactly the dirty range (restoring "clean everywhere"), rasters,
// records the tile-column extent its own raster touched, and re-marks that as
// the new dirty range. The composite then scans exactly that extent: every
// column it skips holds 0xFFFFFFFF, which the kernel's first per-pixel test
// (`mat32 == 0xFFFFFFFF -> continue`) would have skipped anyway.
//
// One worker owns a strip for the whole of its task and the strips partition
// the screen, so plain (non-atomic) per-strip entries cannot race. Both TBR
// schedulers mark every strip fully dirty before dispatch, which covers
// whatever the previous frame, the legacy peel or a resolution change left.
struct XparSliceDirty { int32_t x0; int32_t x1; };   // [x0, x1), empty when x0 >= x1
static XparSliceDirty g_xparDirtySide[2][DEFERRED_MAX_STRIPS];   // [front] [strip]
static XparSliceDirty g_xparDirtyFloor[DEFERRED_MAX_STRIPS];     // g_xparPeelFloor is shared by both sides

void XparStripSlices_MarkAllDirty()
{
	for (int i = 0; i < DEFERRED_MAX_STRIPS; ++i) {
		g_xparDirtySide[0][i] = g_xparDirtySide[1][i] = { 0, INT32_MAX };
		g_xparDirtyFloor[i] = { 0, INT32_MAX };
	}
}

// Per-frame census of what the bound buys (--xpar_extent_census), split by
// side (front / back facing layer — the two Render_DeferredTransparentLighting_
// Tile<> instantiations) and by peel pass, so "which layer is empty" is a
// measurement rather than an assumption.
static constexpr int XCE_MAXPASS = 8;
static std::atomic<uint64_t> g_xceFlushes{0}, g_xcePasses{0},
                             g_xcePxFull{0}, g_xcePxBound{0}, g_xcePxLive{0};
static std::atomic<uint64_t> g_xcePassN[2][XCE_MAXPASS];       // invocations
static std::atomic<uint64_t> g_xcePassLive[2][XCE_MAXPASS];    // live fragments
static std::atomic<uint64_t> g_xcePassBnd[2][XCE_MAXPASS];     // scanned px under the bound
static std::atomic<uint64_t> g_xcePassEmpty[2][XCE_MAXPASS];   // invocations with zero live px

void XparExtentCensus_Report()
{
	if (!fds::FeatureFlags::xpar_extent_census()) return;
	const uint64_t fl = g_xceFlushes.exchange(0), pa = g_xcePasses.exchange(0);
	const uint64_t full = g_xcePxFull.exchange(0), bnd = g_xcePxBound.exchange(0),
	               live = g_xcePxLive.exchange(0);
	if (!pa) return;
	std::fprintf(stderr,
		"[XPAR-CENSUS] flushes=%llu passes=%llu  px full=%.2fM bound=%.2fM (%.2f%%) "
		"live=%.2fM (%.3f%% of full, %.2f%% of bound)\n",
		(unsigned long long)fl, (unsigned long long)pa,
		double(full) / 1e6, double(bnd) / 1e6,
		full ? 100.0 * double(bnd) / double(full) : 0.0,
		double(live) / 1e6,
		full ? 100.0 * double(live) / double(full) : 0.0,
		bnd ? 100.0 * double(live) / double(bnd) : 0.0);
	for (int s = 1; s >= 0; --s)
		for (int p = 0; p < XCE_MAXPASS; ++p) {
			const uint64_t n = g_xcePassN[s][p].exchange(0);
			const uint64_t lv = g_xcePassLive[s][p].exchange(0);
			const uint64_t bd = g_xcePassBnd[s][p].exchange(0);
			const uint64_t em = g_xcePassEmpty[s][p].exchange(0);
			if (!n) continue;
			std::fprintf(stderr,
				"[XPAR-CENSUS]   %-5s peel%d  calls=%6llu  empty=%6llu (%5.1f%%)  "
				"bound=%7.3fM  live=%7.3fM (%5.2f%% of bound)\n",
				s ? "front" : "back", p, (unsigned long long)n,
				(unsigned long long)em, 100.0 * double(em) / double(n),
				double(bd) / 1e6, double(lv) / 1e6,
				bd ? 100.0 * double(lv) / double(bd) : 0.0);
		}
}

// Per-strip xpar render helper, called from TBR_Render's per-strip walk
// for each clump of consecutive same-(mesh, frontFacing) transparent
// faces in the sorted item list. The strip covers rows [strip_y,
// strip_y+strip_h) (strip_h is normally TILESIZE=8, but may be smaller
// for the last strip if YRes isn't a multiple of 8).
//
// One xpar G-buffer layer is dirtied per clump (back layer for back-
// facing tris, front layer for front-facing tris). We clear only the
// strip's slice (61 KB per layer at 1920 wide, or just the dirty columns
// under --xpar_strip_extent), raster the clump's faces with clipper
// extents = strip rect, then composite the strip rows via
// Render_DeferredTransparentLighting_Tile<Layer>.
void RenderXparClumpInStrip(const DeferredLightingCtx &dctx,
                             Face** faces, int count, bool front,
                             int strip_y, int strip_h, bool bandSnapshot)
{
	const size_t rowStart  = size_t(strip_y) * size_t(XRes);
	const size_t rowCount  = size_t(strip_h) * size_t(XRes);
	const int peelPasses = xparPeelPassesEffective();
	meka::GBuffer* sideGB = front ? g_gbufferTransparent : g_gbufferTransparentBack;
	uint16_t*      sideZ  = front ? g_xparZ              : g_xparZBack;

	// ── Column bound (see XparSliceDirty above). OFF → every range below is
	//    the full strip width and the memsets stay the single contiguous ones. ──
	const int stripIdx0 = strip_y >> 3;   // TILELOG=3
	const bool extentOn = fds::FeatureFlags::xpar_strip_extent() &&
	                      stripIdx0 >= 0 && stripIdx0 < DEFERRED_MAX_STRIPS;
	const bool censusOn = fds::FeatureFlags::xpar_extent_census();
	XparSliceDirty *const dSide  = extentOn ? &g_xparDirtySide[front ? 1 : 0][stripIdx0] : nullptr;
	XparSliceDirty *const dFloor = extentOn ? &g_xparDirtyFloor[stripIdx0] : nullptr;
	// Clamp a recorded range into this frame's screen width.
	auto clamped = [&](const XparSliceDirty *d, int &x0, int &x1) {
		if (!d) { x0 = 0; x1 = XRes; return; }
		x0 = d->x0 < 0 ? 0 : d->x0;
		x1 = d->x1 > XRes ? XRes : d->x1;
		if (x1 < x0) x1 = x0;
	};
	// Column-bounded fills. The full-width case keeps the original single
	// contiguous memset so the flag's OFF arm is instruction-for-instruction
	// what the parent did.
	auto fillSide = [&](int x0, int x1, int zByte) {
		if (x0 >= x1) return;
		if (x0 == 0 && x1 == XRes) {
			if (sideGB) std::memset(sideGB->txtr.data() + rowStart, 0xFF, rowCount * sizeof(uint32_t));
			if (sideZ)  std::memset(sideZ + rowStart, zByte, rowCount * sizeof(uint16_t));
			return;
		}
		const size_t w = size_t(x1 - x0);
		for (int r = 0; r < strip_h; ++r) {
			const size_t off = size_t(strip_y + r) * size_t(XRes) + size_t(x0);
			if (sideGB) std::memset(sideGB->txtr.data() + off, 0xFF, w * sizeof(uint32_t));
			if (sideZ)  std::memset(sideZ + off, zByte, w * sizeof(uint16_t));
		}
	};
	auto fillFloor = [&](int x0, int x1) {          // zero the peel floor
		if (!g_xparPeelFloor || x0 >= x1) return;
		// Same invariant break as the legacy peel loop's memset — see
		// XparPeel_ResetAll. The strip path zeroes/copies only columns, but
		// "not all 0xFFFF" is all the next single-pass reader cares about.
		g_xparPeelFloorDirty.store(true, std::memory_order_relaxed);
		if (x0 == 0 && x1 == XRes) {
			std::memset(g_xparPeelFloor + rowStart, 0, rowCount * sizeof(uint16_t));
			return;
		}
		const size_t w = size_t(x1 - x0);
		for (int r = 0; r < strip_h; ++r)
			std::memset(g_xparPeelFloor + size_t(strip_y + r) * size_t(XRes) + size_t(x0),
			            0, w * sizeof(uint16_t));
	};
	const bool peelEarlyOut = fds::FeatureFlags::xpar_peel_early_out();
	// "Did the last pass commit anything in this clump's columns?" — a committed
	// fragment's stored Z is always < 0xFFFF (pass 0's own accept mask requires
	// z_candidate < z_existing with z_existing pre-cleared to 0xFFFF), so an
	// all-0xFFFF window is an exact "nothing here". An empty window counts as
	// untouched: a clump whose raster reached no tile at all commits nothing.
	auto sideZUntouched = [&](int x0, int x1) -> bool {
		if (!sideZ) return true;
		if (x0 >= x1) return true;
		for (int r = 0; r < strip_h; ++r) {
			const uint16_t *p = sideZ + size_t(strip_y + r) * size_t(XRes) + size_t(x0);
			for (int k = x0; k < x1; ++k, ++p) if (*p != 0xFFFFu) return false;
		}
		return true;
	};
	auto copyFloor = [&](int x0, int x1) {          // peel floor <- this side's Z
		if (!g_xparPeelFloor || !sideZ || x0 >= x1) return;
		g_xparPeelFloorDirty.store(true, std::memory_order_relaxed);
		if (x0 == 0 && x1 == XRes) {
			std::memcpy(g_xparPeelFloor + rowStart, sideZ + rowStart, rowCount * sizeof(uint16_t));
			return;
		}
		const size_t w = size_t(x1 - x0);
		for (int r = 0; r < strip_h; ++r) {
			const size_t off = size_t(strip_y + r) * size_t(XRes) + size_t(x0);
			std::memcpy(g_xparPeelFloor + off, sideZ + off, w * sizeof(uint16_t));
		}
	};

	// LEGACY per-strip band snapshot (blocky): only used when bandSnapshot=true.
	// The barrier-per-layer TBR scheduler (TBR_Render_GlassLayered) passes
	// bandSnapshot=false — glass then refracts the WHOLE-FRAME g_glassRefr* snapshot
	// taken at the per-band barrier (coherent, no per-strip vertical clamp → no 8px
	// stair-stepping). glassBandOn stays false, so the kernel sampler reads the
	// full-frame snapshot. See TBR_Render_GlassLayered for the design.
	bool glassClump = false;
	if (bandSnapshot && fds::FeatureFlags::glass_refract() > 0.0f) {
		for (int i = 0; i < count; ++i) {
			Face* F = faces[i];
			if (F && F->Txtr && F->Txtr->NormalMap) { glassClump = true; break; }
		}
	}
	if (glassClump) glassBandSnapshotBegin(strip_y, strip_h);

	// Raster the clump's faces into the side layer, then composite the strip
	// rows. Clipper extents pin the rasterizer to the strip; faces route to
	// MekaleleTransparent (front) / MekaleleTransparentBack (back). The
	// composite uses a ctx variant whose tileLights -> g_stripLights so the
	// strip gets exactly the lights overlapping its 8 rows.
	auto rasterAndComposite = [&](int passIdx) {
		FrustumClipper clipper;
		clipper.InitViewport(CurScene);
		clipper.SetClippingExtents(0.0f, float(strip_y),
		                            float(XRes), float(strip_y + strip_h));
		const auto rt  = fds::MainRenderTargetFromGlobals();
		const auto& cam = fds::g_mainCamera;
		// Strip-clamp the rasterizer: a clipped tri whose max PY snapped to
		// strip_y + strip_h would compute tile_My one tile-row past the strip
		// and write pixels the neighbouring strip's clear (concurrent) wipes.
		const int stripTileRow = strip_y >> 3;  // TILELOG=3
		const meka::RasterStripClamp savedClamp = meka::g_rasterStripClamp;
		meka::g_rasterStripClamp = { stripTileRow, stripTileRow };
		meka::g_rasterXExtent = { INT32_MAX, -1 };   // arm the column recorder
		for (int i = 0; i < count; ++i) {
			Face* F = faces[i];
			if (!F) continue;
			if (front) clipper.Render(F, MekaleleTransparent,     false, rt, cam);
			else       clipper.Render(F, MekaleleTransparentBack, false, rt, cam);
		}
		const meka::RasterXExtent xext = meka::g_rasterXExtent;
		meka::g_rasterStripClamp = savedClamp;

		// Columns this clump's raster can have written. Empty when it wrote
		// nothing at all (fully clipped / backfacing / z-rejected clump), in
		// which case there is nothing to composite and nothing left dirty.
		int cx0 = 0, cx1 = XRes;
		if (extentOn) {
			if (xext.hi < xext.lo) { cx0 = 0; cx1 = 0; }
			else {
				cx0 = xext.lo << 3;                       // TILE_SIZE=8
				cx1 = (xext.hi + 1) << 3;
				if (cx0 < 0) cx0 = 0;
				if (cx1 > XRes) cx1 = XRes;
				if (cx1 < cx0) cx1 = cx0;
			}
			dSide->x0 = cx0; dSide->x1 = cx1;
		}

		// FDS_XPAR_TRACE census: record this clump in the strip's composite
		// sequence (lock-free: this strip runs on one worker; slot row is ours).
		if (xt_on || censusOn) {
			size_t filled = 0;
			if (sideGB && cx1 > cx0) {
				const uint32_t* tx = sideGB->txtr.data();
				for (int r = 0; r < strip_h; ++r) {
					const size_t off = size_t(strip_y + r) * size_t(XRes);
					for (int k = cx0; k < cx1; ++k) if (tx[off + size_t(k)] != 0xFFFFFFFFu) ++filled;
				}
			}
			if (censusOn) {
				const uint64_t bnd = uint64_t(strip_h) * uint64_t(cx1 > cx0 ? cx1 - cx0 : 0);
				g_xcePasses.fetch_add(1, std::memory_order_relaxed);
				g_xcePxFull.fetch_add(rowCount, std::memory_order_relaxed);
				g_xcePxBound.fetch_add(bnd, std::memory_order_relaxed);
				g_xcePxLive.fetch_add(filled, std::memory_order_relaxed);
				const int sd = front ? 1 : 0;
				const int p  = passIdx < XCE_MAXPASS ? passIdx : XCE_MAXPASS - 1;
				g_xcePassN[sd][p].fetch_add(1, std::memory_order_relaxed);
				g_xcePassLive[sd][p].fetch_add(filled, std::memory_order_relaxed);
				g_xcePassBnd[sd][p].fetch_add(bnd, std::memory_order_relaxed);
				if (!filled) g_xcePassEmpty[sd][p].fetch_add(1, std::memory_order_relaxed);
			}
			const int s = strip_y >> 3;
			if (xt_on && s >= 0 && s < XT_STRIPS && xt_count[s] < XT_SLOTS) {
				int &n = xt_count[s];
				xt_mat[s][n]    = (count && faces[0] && faces[0]->Txtr && faces[0]->Txtr->Name)
				                  ? faces[0]->Txtr->Name : "?";
				xt_front[s][n]  = uint8_t(front);
				xt_mtag[s][n]   = int16_t(count && faces[0] ? faces[0]->mirrorMaskTag : -1);
				xt_nfaces[s][n] = uint16_t(count);
				xt_filled[s][n] = uint32_t(filled);
				++n;
			}
		}

		if (cx1 <= cx0) return;
		const int stripIdx = strip_y >> 3;  // TILELOG=3
		DeferredLightingCtx stripCtx = dctx;
		stripCtx.tileLights = g_stripLights;
		// ── THE LIGHT GRID MUST TRAVEL WITH THE LIGHT ARRAY ───────────────
		// da286d8b made the transparent kernel resolve its light tile from
		// the PIXEL, via ctx.lt{NumX,NumY,SizeX,SizeY}. Those describe the
		// deferred 12x8 LIGHTING grid, which is the right grid for the
		// runTilePass path — but this path swaps tileLights to g_stripLights,
		// a 1 x numStrips grid of TILESIZE-row Y strips. Leaving the 12x8
		// geometry in place makes lightTileAt() subscript the STRIP array as
		// (py/ltSizeY)*12 + px/ltSizeX, i.e. it lights a strip from a light
		// list built for a completely different band of the screen: exactly
		// the category error da286d8b fixed, one dispatch level down.
		// (Verified live: fountain t=1200 differs between --xpar_tile_lights
		// and --no-xpar_tile_lights, 845 px / max |d| 19, and the OFF arm —
		// ctx.tileLights[stripIdx] — is the correct one.)
		// Describing the strip array to the kernel makes the per-pixel lookup
		// land on py>>3, which for a rect inside this strip is stripIdx: the
		// same list the pre-da286d8b ordinal subscript picked, and the right
		// one for strips >= DEFERRED_NUM_TILES too (the legacy subscript
		// clamped those to tile 95, i.e. every strip below row 768 shared one
		// list — visible only above 768 px of height).
		{
			int numStrips = ((stripCtx.yres > 0 ? stripCtx.yres : YRes) + 7) >> 3;
			if (numStrips > DEFERRED_MAX_STRIPS) numStrips = DEFERRED_MAX_STRIPS;
			if (numStrips < 1) numStrips = 1;
			stripCtx.ltNumX  = 1;
			stripCtx.ltNumY  = numStrips;
			stripCtx.ltSizeX = XRes > 0 ? XRes : 1;
			stripCtx.ltSizeY = 1 << 3;   // TILESIZE
		}
		if (front) {
			Render_DeferredTransparentLighting_Tile<XparLayer::Front>(
				stripCtx, stripIdx, cx0, strip_y, cx1, strip_y + strip_h);
		} else {
			Render_DeferredTransparentLighting_Tile<XparLayer::Back>(
				stripCtx, stripIdx, cx0, strip_y, cx1, strip_y + strip_h);
		}
	};

	// thread_local: this strip runs on its own worker; the synchronous
	// clipper.Render above reads g_xparPeelReverse on the same thread.
	g_xparPeelReverse = (peelPasses > 1);

	if (censusOn) g_xceFlushes.fetch_add(1, std::memory_order_relaxed);

	if (peelPasses <= 1) {
		// Legacy single-fragment peel — byte-identical. Clear the strip's
		// slice of the side layer (txtr=empty, Z=0) and render once. Only the
		// columns the previous clump dirtied need clearing; the rest already
		// hold the cleared state.
		int c0, c1; clamped(dSide, c0, c1);
		fillSide(c0, c1, 0);
		rasterAndComposite(0);
		if (glassClump) glassBandSnapshotEnd();
		return;
	}

	// Reverse depth peel WITHIN this (clump, side), K passes deep, over the
	// strip's row slice — farthest-first, compositing each over the last.
	// Under the column bound: pass 0 clears whatever the previous clump left
	// dirty, and passes 1..K-1 clear (and floor-copy) exactly this clump's own
	// extent — the tile columns are a purely geometric bound, identical in
	// every pass of one clump, so pass 0's recorded extent covers them all.
	for (int pass = 0; pass < peelPasses; ++pass) {
		// Per-pass ceiling: pass 0 accepts all (0); later passes accept only
		// fragments nearer than the previous pass's peeled Z.
		if (pass == 0) {
			int f0, f1; clamped(dFloor, f0, f1);
			fillFloor(f0, f1);                       // floor is now zero everywhere
			if (dFloor) *dFloor = { 0, 0 };
		} else {
			int f0, f1; clamped(dSide, f0, f1);
			// --xpar_peel_early_out: the previous pass left its whole extent at
			// the 0xFFFF clear value, so this pass's ceiling is 0xFFFF there and
			// its accept mask `(z < 0xFFFF) & (z > 0xFFFF)` is empty — as is
			// every later pass's, each inheriting an untouched layer in turn.
			if (peelEarlyOut && sideZUntouched(f0, f1)) break;
			copyFloor(f0, f1);
			if (dFloor) *dFloor = { f0, f1 };
		}
		// Clear side layer slice: Z to 0xFFFF (keep-farthest init), txtr empty.
		int c0, c1; clamped(dSide, c0, c1);
		fillSide(c0, c1, 0xFF);
		rasterAndComposite(pass);
	}
	if (glassClump) glassBandSnapshotEnd();
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
// (rsqrt8_nr lives beside rcp8_nr further up, so the GGX loop shares it.)

// 8-wide front-end of the env-specular compose for the OuterVec env-only
// lanes — the CITY city_env_pixel case only: one UNIFORM cube store across
// the group, noParallax, cv-pull on, no roughness/metal maps, env
// diagnostics off (anything else falls back to EnvSpecComposeScalar).
// Vectorizes the math-heavy chain: view-ray handling, viewer-side flip,
// both view->world rotations, the full cv-pull (three rsqrts) with its
// guard fade, the reflect, roughness->mip, and Schlick Fresnel. The face
// pick + bilinear fetch stay on the scalar EnvCube helpers in the caller —
// they are gathers either way, and sharing the helpers keeps sampling
// convention parity with the scalar compose.
//
// Outputs per lane: world reflect dir (rvx/y/z), Fresnel*gain weight (ek),
// and the float mip level (lvlF). Lanes without env produce junk — the
// caller only consumes lanes with lane_envP set.
static inline void EnvComposeCityVec8(const DeferredLightingCtx &ctx,
	const fds::EnvPanoLinear* envP,
	const float* ax, const float* ay, const float* az,
	const float* anx, const float* any_, const float* anz,
	const float* laneGloss, const float* laneF0, float envReflGain,
	float* outRvx, float* outRvy, float* outRvz,
	float* outEk, float* outLvlF, float* outFres = nullptr)
{
	const __m256 x = _mm256_load_ps(ax);
	const __m256 y = _mm256_load_ps(ay);
	const __m256 z = _mm256_load_ps(az);
	__m256 nx = _mm256_load_ps(anx);
	__m256 ny = _mm256_load_ps(any_);
	__m256 nz = _mm256_load_ps(anz);
	const __m256 zero = _mm256_setzero_ps();
	const __m256 one  = _mm256_set1_ps(1.0f);

	// Viewer-side flip: sign(d.N) == sign(viewpos.N) (dInv > 0), so no
	// normalize needed for the test.
	{
		const __m256 vDotN = _mm256_fmadd_ps(x, nx,
			_mm256_fmadd_ps(y, ny, _mm256_mul_ps(z, nz)));
		const __m256 flip = _mm256_cmp_ps(vDotN, zero, _CMP_GT_OQ);
		const __m256 sgn  = _mm256_blendv_ps(one, _mm256_set1_ps(-1.0f), flip);
		nx = _mm256_mul_ps(nx, sgn);
		ny = _mm256_mul_ps(ny, sgn);
		nz = _mm256_mul_ps(nz, sgn);
	}

	// View -> world rotations (viewToWorld broadcast).
	const __m256 r00 = _mm256_set1_ps(ctx.viewToWorld[0][0]);
	const __m256 r01 = _mm256_set1_ps(ctx.viewToWorld[0][1]);
	const __m256 r02 = _mm256_set1_ps(ctx.viewToWorld[0][2]);
	const __m256 r10 = _mm256_set1_ps(ctx.viewToWorld[1][0]);
	const __m256 r11 = _mm256_set1_ps(ctx.viewToWorld[1][1]);
	const __m256 r12 = _mm256_set1_ps(ctx.viewToWorld[1][2]);
	const __m256 r20 = _mm256_set1_ps(ctx.viewToWorld[2][0]);
	const __m256 r21 = _mm256_set1_ps(ctx.viewToWorld[2][1]);
	const __m256 r22 = _mm256_set1_ps(ctx.viewToWorld[2][2]);
	const __m256 nwx = _mm256_fmadd_ps(r00, nx, _mm256_fmadd_ps(r01, ny, _mm256_mul_ps(r02, nz)));
	const __m256 nwy = _mm256_fmadd_ps(r10, nx, _mm256_fmadd_ps(r11, ny, _mm256_mul_ps(r12, nz)));
	const __m256 nwz = _mm256_fmadd_ps(r20, nx, _mm256_fmadd_ps(r21, ny, _mm256_mul_ps(r22, nz)));
	// Rotated view position rp = R^T * viewpos = sampleWorld - camera.
	const __m256 rpx = _mm256_fmadd_ps(r00, x, _mm256_fmadd_ps(r01, y, _mm256_mul_ps(r02, z)));
	const __m256 rpy = _mm256_fmadd_ps(r10, x, _mm256_fmadd_ps(r11, y, _mm256_mul_ps(r12, z)));
	const __m256 rpz = _mm256_fmadd_ps(r20, x, _mm256_fmadd_ps(r21, y, _mm256_mul_ps(r22, z)));

	// cv-pull (scalar-compose parity; see EnvSpecComposeScalar). u =
	// bakePoint - camera is UNIFORM across the group.
	const float uxs = envP->bakeX - ctx.cameraWorldX;
	const float uys = envP->bakeY - ctx.cameraWorldY;
	const float uzs = envP->bakeZ - ctx.cameraWorldZ;
	const float uLen2s = uxs*uxs + uys*uys + uzs*uzs;
	const __m256 ux = _mm256_set1_ps(uxs), uy = _mm256_set1_ps(uys), uz = _mm256_set1_ps(uzs);
	const __m256 signMask = _mm256_set1_ps(-0.0f);
	// planeD = |(cam - sw).nw| = |rp.nw| ; optD = |(u - rp).nw|
	const __m256 rpDotN = _mm256_fmadd_ps(rpx, nwx, _mm256_fmadd_ps(rpy, nwy, _mm256_mul_ps(rpz, nwz)));
	const __m256 planeD = _mm256_andnot_ps(signMask, rpDotN);
	const __m256 step   = _mm256_fmadd_ps(ux, nwx, _mm256_fmadd_ps(uy, nwy, _mm256_mul_ps(uz, nwz)));
	const __m256 optD   = _mm256_andnot_ps(signMask, _mm256_sub_ps(step, rpDotN));
	const __m256 gMin   = _mm256_set1_ps(0.01f * uLen2s);
	const __m256 g      = _mm256_mul_ps(step, step);
	const __m256 gate   = _mm256_and_ps(
		_mm256_cmp_ps(planeD, _mm256_add_ps(optD, one), _CMP_GT_OQ),
		_mm256_cmp_ps(g, gMin, _CMP_GT_OQ));
	// d0^0.75 via two NR rsqrts (scalar-compose parity); inputs clamped so
	// masked-off lanes stay finite (blended away below).
	const __m256 d0  = _mm256_max_ps(_mm256_sub_ps(planeD, optD), one);
	const __m256 sq  = _mm256_mul_ps(d0, rsqrt8_nr(d0));            // sqrt(d0)
	const __m256 t   = _mm256_mul_ps(d0, sq);                       // d0^1.5
	const __m256 hackD = _mm256_fmadd_ps(t, rsqrt8_nr(t), optD);    // d0^0.75 + optD
	const __m256 safeStep = _mm256_blendv_ps(one, step, gate);
	__m256 k = _mm256_div_ps(_mm256_sub_ps(hackD, planeD), safeStep);
	// Guard fade: k *= clamp((g - gMin) / (3*gMin), 0, 1).
	const __m256 fade = _mm256_min_ps(one, _mm256_max_ps(zero,
		_mm256_div_ps(_mm256_sub_ps(g, gMin), _mm256_mul_ps(_mm256_set1_ps(3.0f), gMin))));
	k = _mm256_mul_ps(k, fade);
	k = _mm256_and_ps(k, gate);
	// w = sw - pulledEye = rp - k*u ; normalize; reflect about nw.
	__m256 wx = _mm256_fnmadd_ps(k, ux, rpx);
	__m256 wy = _mm256_fnmadd_ps(k, uy, rpy);
	__m256 wz = _mm256_fnmadd_ps(k, uz, rpz);
	const __m256 wLen2 = _mm256_fmadd_ps(wx, wx,
		_mm256_fmadd_ps(wy, wy, _mm256_fmadd_ps(wz, wz, _mm256_set1_ps(1e-12f))));
	const __m256 wInv = rsqrt8_nr(wLen2);
	wx = _mm256_mul_ps(wx, wInv);
	wy = _mm256_mul_ps(wy, wInv);
	wz = _mm256_mul_ps(wz, wInv);
	const __m256 wDotN = _mm256_fmadd_ps(wx, nwx, _mm256_fmadd_ps(wy, nwy, _mm256_mul_ps(wz, nwz)));
	const __m256 twoWDotN = _mm256_add_ps(wDotN, wDotN);
	_mm256_store_ps(outRvx, _mm256_fnmadd_ps(twoWDotN, nwx, wx));
	_mm256_store_ps(outRvy, _mm256_fnmadd_ps(twoWDotN, nwy, wy));
	_mm256_store_ps(outRvz, _mm256_fnmadd_ps(twoWDotN, nwz, wz));

	// Roughness -> mip (no rough map on this path: gloss-derived).
	const __m256 gloss = _mm256_load_ps(laneGloss);
	const __m256 rough = rsqrt8_nr(_mm256_mul_ps(
		_mm256_add_ps(gloss, _mm256_set1_ps(2.0f)), _mm256_set1_ps(0.5f)));
	const __m256 maxLvl = _mm256_set1_ps(float(envP->numMips - 1));
	// --env_mip_chain: virtual chain depth as the multiplier (§11 row E7);
	// the CLAMP stays on the real chain. Unset → mulLvl == maxLvl, byte-null.
	const __m256 mulLvl = _mm256_set1_ps(float(envMipChainDepth(envP->numMips) - 1));
	_mm256_store_ps(outLvlF,
		_mm256_min_ps(maxLvl, _mm256_max_ps(zero, _mm256_mul_ps(rough, mulLvl))));

	// Schlick Fresnel (metal = 0 on this path), ek = fres * gain.
	const __m256 ndv = _mm256_min_ps(one, _mm256_max_ps(zero,
		_mm256_sub_ps(zero, wDotN)));
	const __m256 f0  = _mm256_load_ps(laneF0);
	const __m256 f90 = _mm256_max_ps(_mm256_sub_ps(one, rough), f0);
	const __m256 omv = _mm256_sub_ps(one, ndv);
	const __m256 omv2 = _mm256_mul_ps(omv, omv);
	const __m256 omv5 = _mm256_mul_ps(_mm256_mul_ps(omv2, omv2), omv);
	const __m256 fres = _mm256_fmadd_ps(_mm256_sub_ps(f90, f0), omv5, f0);
	_mm256_store_ps(outEk, _mm256_mul_ps(fres, _mm256_set1_ps(envReflGain)));
	// (1-F) diffuse energy conservation (--diffuse_energy): expose the raw
	// per-lane Fresnel so the caller can scale diffuse by (1-fres).
	if (outFres) _mm256_store_ps(outFres, fres);
}

static void Render_DeferredLighting_Tile_OuterVec(const DeferredLightingCtx &ctx,
                                                   int tileIndex,
                                                   int x1, int y1, int x2, int y2)
{
	// Addressing from ctx, not globals (RenderContext migration).
	const int XRes = ctx.xres;
	byte *const VPage = ctx.vpage;
	word *const ZPage16 = ctx.zpage16;
	const float CntrEX = ctx.cntrEX, CntrEY = ctx.cntrEY;
	const meka::GBuffer &gb = *ctx.gb;
	dword *out = reinterpret_cast<dword *>(VPage);
	const bool   quarter      = deferredLightingQuarterEnabled();
	const bool   checker      = deferredLightingCheckerboardEnabled() && !quarter;
	const bool   specGlobalOn = Specular_Factor > 0.0f;
	// Texture filtering: read the raster-time filtered albedo (see the
	// scalar kernel). Only the diffuse texel moves to gb.albedo; the
	// normal-map chase below keeps its suv address.
	const bool   texFilterOn  = (fds::FeatureFlags::texture_filter() > 0
	                             || fds::FeatureFlags::poly_viz())
	                            && !gb.albedo.empty();
	// Env-specular state (--env_refl): OuterVec had NO env compose at all —
	// --env_refl was silently inert on every PreferOuterVec scene (city).
	// Env lanes ride the existing spec/water per-lane scalar fallback, which
	// is where the shared EnvSpecComposeScalar is invoked (wave-1 parity).
	// envTabG == null (env off) → lane_envP stays null → needsScalar mask
	// unchanged → byte-identical to the pre-env kernel by construction.
	const fds::EnvPanoLinear *const *envTabG = fds::FeatureFlags::env_refl()
	    ? fds::EnvReflection_Table(ctx.Sc) : nullptr;
	const float envReflGainG = fds::FeatureFlags::env_refl_gain();
	const bool  envBrdfAnalyticG = fds::FeatureFlags::env_brdf_analytic();
	const bool  multiScatterG  = fds::FeatureFlags::pbr_multiscatter();
	const bool  metalTintLinG  = MetalTintLinearActive(ctx);   // linear frame only
	const bool  roughMapOnG  = fds::FeatureFlags::roughness_map();
	const float roughStrengthG = fds::FeatureFlags::roughness_strength();  // redo-lane rough attenuation (see wave-1)
	const bool  metalMapOnG  = fds::FeatureFlags::metal_map();
	const bool  diffuseEnergyG = fds::FeatureFlags::diffuse_energy();
	// --sh_ambient: SH irradiance coefficients (null = off / not baked). See
	// the lane_ambB rewrite after the normal decode below.
	const float* shCoefG     = fds::FeatureFlags::sh_ambient()
		? fds::SHAmbient_Coeffs(ctx.Sc) : nullptr;
	// HDR: same rule as the scalar kernel — the 250 lit-cap is an 8-bit
	// rollover guard, lifted under HDR so luminosity > 1 isn't flattened
	// before the texel multiply. NOTE this kernel still stores 8-bit only
	// (the final pack clamps at 255 and Hdr_ActivateNoFog seeds the float
	// buffer FROM that); radiance > 255 on PreferOuterVec scenes needs the
	// scalar kernel. Lifting the cap here still recovers the 250→255 band
	// and keeps vec/scalar lit math consistent pre-pack.
	//
	// ⚠ DO NOT PORT --deferred_shade_ldr_skip HERE. The scalar kernel's LDR
	// compose is skippable because its --hdr_linear store already put the
	// radiance in g_hdrBuf; THIS kernel writes no HDR at all, so its 8-bit pack
	// IS the HDR transport — Hdr_ActivateNoFog lifts it (h[3] is left 0 here) on
	// every PreferOuterVec scene (city, crash). Skipping the pack would tonemap
	// those frames from a cleared buffer. The saving looks identical from the
	// source; it is not the same code.
	const bool  hdrWrite = fds::FeatureFlags::hdr() && ctx.hdrBuf != nullptr;
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
	// Per-lane specular response multiplier (Material::SpecMul, RVSF 0x800):
	// scales the lane's FINAL specular — analytic redo accumulators and the
	// env compose adds — 1.0f = exact float identity (byte-null default).
	alignas(32) float    lane_specMul[8];
	alignas(32) float    lane_gloss[8];
	alignas(32) uint32_t lane_wantSpec[8];
	alignas(32) uint32_t lane_isWater[8];
	const fds::EnvPanoLinear* lane_envP[8];
	alignas(32) uint32_t lane_hasEnv[8];
	// Per-lane mirror id, widened uint8→uint32 once per 8-pixel block.
	// The omni loop builds a per-lane mask against broadcast(tl.mirrorId[n]).
	alignas(32) uint32_t lane_mirrorId[8];

	// --deferred_checker_env_full parity with the scalar wave-1 kernel. THE BUG
	// this closes: the wave-2 fill skips an env-reflective dropped cell when the
	// flag is on (`envForceFull && checkerEnvFullG`, TileFill) because the SCALAR
	// wave 1 keeps it — but THIS kernel dropped it on parity alone, so under
	// `--deferred_outer_vec --deferred_checkerboard` those cells were shaded by
	// NEITHER wave. MEASURED on greets t=4871: 17,897 px, mean |dY| 111.6, max
	// 220 (i.e. unshaded holes) between the flag on and off. Latent at the
	// shipped defaults — no scene sets both — hence no new flag: with the flag
	// off, or with checker/quarter off, `envFullKeep` is the identity.
	const bool checkerEnvFullOV = (checker || quarter)
	    && fds::FeatureFlags::deferred_checker_env_full() && envTabG != nullptr;
	auto envFullKeep = [&](__m256i keep, size_t base) -> __m256i {
		if (!checkerEnvFullOV) return keep;
		alignas(32) int32_t kArr[8];
		_mm256_store_si256((__m256i*)kArr, keep);
		bool any = false;
		for (int k = 0; k < 8; ++k) {
			if (kArr[k]) continue;
			const uint32_t mid = (gb.txtr[base + k] >> 20) & 0xFF;
			if (envTabG[mid] != nullptr) { kArr[k] = -1; any = true; }
		}
		return any ? _mm256_load_si256((const __m256i*)kArr) : keep;
	};

	for (int py = y1; py < y2; ++py) {
		// vec body: groups of 8 pixels
		int px = x1;
		// Align start to 8-pixel boundary so loads from gb arrays are
		// naturally aligned (gb.txtr/gb.normal are aligned at frame
		// allocation; ZPage16 aligned by parallel_memset).
		const int x_vec_start = (x1 + 7) & ~7;
		// Scalar lead-in for unaligned pixels at tile start.
		while (px < x_vec_start && px < x2) {
			// Tail: just defer to scalar; rare since tile boundaries
			// are 8-aligned for our 1920x1080 / 12x8 grid (160-wide tiles).
			++px;
		}

		for (; px < x2; px += 8) {
			const size_t i = size_t(py) * XRes + px;
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
			__m128i z16 = _mm_loadu_si128((const __m128i*)(ZPage16 + i));
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
				keep = envFullKeep(keep, i);   // see envFullKeep
				mask_alive = _mm256_and_si256(mask_alive, keep);
			}
			// Quarter-rate: keep only lanes where both px AND py even.
			if (quarter) {
				// Odd rows are entirely wave-2 — but under
				// --deferred_checker_env_full the fill SKIPS env-reflective
				// cells expecting wave 1 to have taken them, so the row can
				// only be skipped wholesale when that flag is off.
				if ((py & 1) && !checkerEnvFullOV) continue;
				__m256i lane_idx = _mm256_setr_epi32(0,1,2,3,4,5,6,7);
				__m256i px_lane  = _mm256_add_epi32(_mm256_set1_epi32(px), lane_idx);
				__m256i parity_x = _mm256_and_si256(px_lane, _mm256_set1_epi32(1));
				__m256i keep = (py & 1) ? _mm256_setzero_si256()
				                        : _mm256_cmpeq_epi32(parity_x, _mm256_setzero_si256());
				keep = envFullKeep(keep, i);
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
					lane_specMul[k] = 1.0f;
					lane_gloss[k] = 32.0f;
					lane_wantSpec[k] = 0;
					lane_isWater[k] = 0;
					lane_envP[k] = nullptr; lane_hasEnv[k] = 0;
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
					lane_specMul[k] = 1.0f;
					lane_gloss[k] = 32.0f;
					lane_wantSpec[k] = 0;
					lane_isWater[k] = 0;
					lane_envP[k] = nullptr; lane_hasEnv[k] = 0;
					continue;
				}
				const dword *texData = (const dword*)Mat->Txtr->Mipmap[mip];
				if (!texData) {
					lane_alive[k] = 0;
					lane_texB[k] = lane_texG[k] = lane_texR[k] = 0;
					lane_ambB[k] = lane_ambG[k] = lane_ambR[k] = 0;
					lane_diffuse[k] = lane_specular[k] = 0;
					lane_specMul[k] = 1.0f;
					lane_gloss[k] = 32.0f;
					lane_wantSpec[k] = 0;
					lane_isWater[k] = 0;
					lane_envP[k] = nullptr; lane_hasEnv[k] = 0;
					continue;
				}
				// Heightmap (parallax) materials are INCLUDED now (Tier 1,
				// filtered parallax) — the raster albedo plane holds the
				// bilinear sample at the parallax-shifted UV (see scalar kernel).
				const dword tx = texFilterOn
					? gb.albedo[i + k] : texData[uv];
				lane_texB[k] = float(tx & 0xFF)         * Mat->TintB;  // editor tint (see main kernel)
				lane_texG[k] = float((tx >> 8) & 0xFF)  * Mat->TintG;
				lane_texR[k] = float((tx >> 16) & 0xFF) * Mat->TintR;
				const float Lumin = Mat->Luminosity;
				const float Diff  = Mat->Diffuse;
				lane_ambB[k]    = Lumin * 255.0f + Diff * ambB_sc;
				lane_ambG[k]    = Lumin * 255.0f + Diff * ambG_sc;
				lane_ambR[k]    = Lumin * 255.0f + Diff * ambR_sc;
				lane_diffuse[k] = Diff;
				lane_specular[k]= Mat->Specular;
				lane_specMul[k] = Mat->SpecMul;
				lane_gloss[k]   = Mat->Glossiness > 0 ? float(Mat->Glossiness) : 32.0f;
				lane_wantSpec[k]= (Mat->Specular > 0.0f && specGlobalOn) ? 0xFFFFFFFFu : 0u;
				lane_isWater[k] = (int(matID) == ctx.waterMatID) ? 0xFFFFFFFFu : 0u;
				// Env-specular lane (wave-1 gate parity: Reflection > 0 or a
				// metal map). Joins the scalar fallback below.
				lane_envP[k] = (envTabG && (Mat->Reflection > 0.0f
				               || (metalMapOnG && Mat->MetallicMap)))
				               ? envTabG[matID] : nullptr;
				lane_hasEnv[k] = lane_envP[k] ? 0xFFFFFFFFu : 0u;
				any_alive = true;
			}
			if (!any_alive) continue;
			// Refresh alive mask from scratch (some lanes may have been
			// killed by mip-data null check above).
			__m256i mask_alive_fresh = _mm256_load_si256((const __m256i*)lane_alive);

			// Decode 8 normals in parallel. oct_decode_u32 form (16.16):
			//   qx = sign-extend(low 16), qy = sign-extend(high 16)
			//   ox = qx * (1/32767), oy = qy * (1/32767)
			//   az = 1 - |ox| - |oy|
			//   if (az < 0) fold ...
			//   then normalize.
			__m256i nrm32 = _mm256_loadu_si256((const __m256i*)(gb.normal.data() + i));
			__m256i qx32 = _mm256_srai_epi32(_mm256_slli_epi32(nrm32, 16), 16);
			__m256i qy32 = _mm256_srai_epi32(nrm32, 16);
			__m256 invQ = _mm256_set1_ps(1.0f / 32767.0f);
			__m256 ox = _mm256_mul_ps(_mm256_cvtepi32_ps(qx32), invQ);
			__m256 oy = _mm256_mul_ps(_mm256_cvtepi32_ps(qy32), invQ);
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
			// Normalize via approx_rsqrt + ONE Newton-Raphson step. The raw
			// 12-bit rsqrt is ±0.3% and PIECEWISE-CONSTANT in its LUT — the
			// stepping normal LENGTH couples into reflect()'s 2(w·n)n term
			// (scales by |n|²) and snapped the reflected direction ~0.35°
			// per LUT-cell crossing: THE per-facade "window reflections
			// jump" on the authored city flight (root-caused by micro-dolly
			// bisection, 2026-07: normal DIRECTION moved one oct cell,
			// 0.0024°, while rw lurched 0.354° — 60x the reflect() bound;
			// the ±0.3% |n| plateaus were the only term that fit). One NR
			// brings |n| to ±6e-5, same as the scalar path's fast_rsqrt.
			// Diffuse shading only ever saw this as ±0.3% brightness.
			__m256 lenSq = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(ox, ox),
			                                            _mm256_mul_ps(oy, oy)),
			                              _mm256_mul_ps(az, az));
			__m256 invLenN = _mm256_rsqrt_ps(lenSq);
			invLenN = _mm256_mul_ps(invLenN,
			    _mm256_fnmadd_ps(_mm256_mul_ps(lenSq, _mm256_set1_ps(0.5f)),
			                     _mm256_mul_ps(invLenN, invLenN),
			                     _mm256_set1_ps(1.5f)));
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

			// --sh_ambient: rewrite the flat Diff*Sc->Ambient term baked into
			// lane_ambB (Lumin*255 + Diff*ambB_sc) as Lumin*255 + Diff*E(n),
			// where E(n) is the SH irradiance along the per-lane (post nmap)
			// world-space shading normal. Both the vec load (lB below) and the
			// per-lane scalar fallback (lBs = lane_ambB[k]) then pick it up.
			// Null coeffs (flag off) → skipped → byte-identical.
			if (shCoefG) {
				for (int k = 0; k < 8; ++k) {
					if (!lane_alive[k]) continue;
					const float lnx = nx_lane[k], lny = ny_lane[k], lnz = nz_lane[k];
					const float wnx = ctx.viewToWorld[0][0]*lnx + ctx.viewToWorld[0][1]*lny + ctx.viewToWorld[0][2]*lnz;
					const float wny = ctx.viewToWorld[1][0]*lnx + ctx.viewToWorld[1][1]*lny + ctx.viewToWorld[1][2]*lnz;
					const float wnz = ctx.viewToWorld[2][0]*lnx + ctx.viewToWorld[2][1]*lny + ctx.viewToWorld[2][2]*lnz;
					float eB, eG, eR;
					shEvalIrradiance(shCoefG, wnx, wny, wnz, eB, eG, eR);
					const float d = lane_diffuse[k];
					lane_ambB[k] += d * (eB - ambB_sc);
					lane_ambG[k] += d * (eG - ambG_sc);
					lane_ambR[k] += d * (eR - ambR_sc);
				}
			}

			// Reconstruct view-space pos for 8 lanes.
			// z = (0xFF80 - zEnc) * invZScale
			__m256 zEncF = _mm256_cvtepi32_ps(zEncI);
			__m256 zv = _mm256_mul_ps(_mm256_sub_ps(_mm256_set1_ps(float(0xFF80)), zEncF),
			                           _mm256_set1_ps(ctx.invZScale));
			// x = (px+lane - CntrEX) * z * invFOVX
			__m256 px_lane_f = _mm256_add_ps(_mm256_set1_ps(float(px)),
			                                  _mm256_setr_ps(0,1,2,3,4,5,6,7));
			__m256 xv = _mm256_mul_ps(_mm256_mul_ps(_mm256_sub_ps(px_lane_f,
			                                                       _mm256_set1_ps(CntrEX)),
			                                         zv),
			                           _mm256_set1_ps(ctx.invFOVX));
			__m256 yv = _mm256_mul_ps(_mm256_mul_ps(_mm256_sub_ps(_mm256_set1_ps(CntrEY),
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

			// Saturate to 250 — 8-bit rollover guard; upper cap lifted under
			// HDR (see hdrWrite above), lower 0-clamp always.
			__m256 zero = _mm256_setzero_ps();
			if (!hdrWrite) {
				__m256 sat = _mm256_set1_ps(250.0f);
				lB = _mm256_min_ps(lB, sat);
				lG = _mm256_min_ps(lG, sat);
				lR = _mm256_min_ps(lR, sat);
			}
			lB = _mm256_max_ps(zero, lB);
			lG = _mm256_max_ps(zero, lG);
			lR = _mm256_max_ps(zero, lR);

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

			// 8-wide env front-end (city case): engage when every env lane
			// in this group shares ONE cube store with the city shape
			// (noParallax + cv-pull) and no per-pixel map/diagnostic
			// forces the scalar compose. See EnvComposeCityVec8.
			static const bool sEnvVecDiagOff =
				!std::getenv("ENVPROBE") && !std::getenv("ENVFLIP") &&
				!std::getenv("ENV_NOFETCH") && !std::getenv("FDS_ENV_SKIP_NEGY") &&
				!EnvTraceGet();
			// Position-aware lookup fakes run scalar-only for now (see
			// EnvSpecComposeScalar; vectorizing them is the follow-up if
			// the look is approved) — disengage the vec compose when on.
			const bool envPosFakesOff =
				fds::FeatureFlags::env_sphere_parallax() <= 0.0f &&
				fds::FeatureFlags::env_ssr() <= 0 &&   // SSR march is scalar-only
				!envBrdfAnalyticG;                     // analytic env-BRDF is scalar-only (no AVX2 exp2)
			bool envVecReady = false;
			alignas(32) float envRvx[8], envRvy[8], envRvz[8];
			alignas(32) float envEk[8], envLvlF[8], envF0[8];
			alignas(32) float envFres[8] = {0};   // (1-F) diffuse energy conservation
			if (sEnvVecDiagOff && envPosFakesOff) {
				const fds::EnvPanoLinear* uni = nullptr;
				bool uniform = true;
				for (int k = 0; k < 8; ++k) {
					if (!lane_alive_now[k] || !lane_envP[k]) continue;
					if (!uni) uni = lane_envP[k];
					else if (uni != lane_envP[k]) { uniform = false; break; }
				}
				if (uni && uniform && uni->isCube && uni->noParallax
				    && uni->pullOpt > 0.0f) {
					// Per-MATERIAL map check — the rough/metal GLOBAL flags
					// default ON; what matters is whether these lanes'
					// materials carry maps (city glass doesn't).
					bool mapsOff = true;
					for (int k = 0; k < 8; ++k) {
						float f0 = 0.04f;
						if (lane_envP[k]) {
							const Material* M =
								ctx.matTable.data[(lane_mat32[k] >> 20) & 0xFF];
							if ((roughMapOnG && M->RoughnessMap) ||
							    (metalMapOnG && M->MetallicMap)) {
								mapsOff = false;
								break;
							}
							f0 = M->Reflection * 0.01f;
							if (f0 < 0.04f) f0 = 0.04f;
						}
						envF0[k] = f0;
					}
					if (mapsOff) {
						EnvComposeCityVec8(ctx, uni, ax, ay, az_lane,
						                   anx, any_l, anz, lane_gloss, envF0,
						                   envReflGainG,
						                   envRvx, envRvy, envRvz, envEk, envLvlF,
						                   envFres);
						envVecReady = true;
					}
				}
			}

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
					// Same rollover-guard rule as the vec body: upper cap
					// lifted under HDR, 0-clamp always.
					if (!hdrWrite) {
						if (lBs > 250.0f) lBs = 250.0f;
						if (lGs > 250.0f) lGs = 250.0f;
						if (lRs > 250.0f) lRs = 250.0f;
					}
					if (lBs < 0) lBs = 0;
					if (lGs < 0) lGs = 0;
					if (lRs < 0) lRs = 0;
					float fdBs = lane_texB[k] * lBs * (1.0f / 256.0f);
					float fdGs = lane_texG[k] * lGs * (1.0f / 256.0f);
					float fdRs = lane_texR[k] * lRs * (1.0f / 256.0f);
					// Roughness map (cheap tier): per-pixel specular intensity
					// attenuation — same block + same position as wave-1
					// (after the analytic accumulation, BEFORE the env
					// compose). The redo lane previously skipped it, so a
					// rough-mapped material's analytic highlight was NOT
					// broken up on OuterVec scenes; scenes without rough maps
					// (stock city) are byte-untouched.
					if (roughMapOnG && (sBs != 0.0f || sGs != 0.0f || sRs != 0.0f)) {
						Material *MatR_ = ctx.matTable.data[(lane_mat32[k] >> 20) & 0xFF];
						if (MatR_ && MatR_->RoughnessMap) {
							const uint32_t mipR_ = (lane_mat32[k] >> 28) & 0xF;
							const byte *rd_ = (mipR_ < MatR_->RoughnessMap->numMipmaps)
								? reinterpret_cast<const byte*>(MatR_->RoughnessMap->Mipmap[mipR_]) : nullptr;
							if (rd_) {
								float rAtt = 1.0f - roughStrengthG *
									(float(rd_[lane_mat32[k] & 0xFFFFF]) * (1.0f/255.0f));
								if (rAtt < 0.0f) rAtt = 0.0f;
								sBs *= rAtt; sGs *= rAtt; sRs *= rAtt;
							}
						}
					}
					// Env-specular compose (--env_refl): same shared helper +
					// same position as wave-1 (after analytic spec, before the
					// water blend). Third consumer of EnvSpecComposeScalar.
					if (lane_envP[k]) {
						if (g_envVecStats) g_envCntOvScalar.fetch_add(1, std::memory_order_relaxed);
						const uint32_t m_   = lane_mat32[k];
						const uint32_t mid_ = (m_ >> 20) & 0xFF;
						const uint32_t mip_ = (m_ >> 28) & 0xF;
						const uint32_t suv_ = m_ & 0xFFFFF;
						Material *Mat_ = ctx.matTable.data[mid_];
						float metalM_ = 0.0f;
						if (metalMapOnG && Mat_->MetallicMap) {
							const byte *md_ = (mip_ < Mat_->MetallicMap->numMipmaps)
								? reinterpret_cast<const byte*>(Mat_->MetallicMap->Mipmap[mip_]) : nullptr;
							if (md_) metalM_ = float(md_[suv_]) * (1.0f/255.0f);
						}
						const float swx_ = ctx.viewToWorld[0][0]*xs + ctx.viewToWorld[0][1]*ys
						                 + ctx.viewToWorld[0][2]*zs + ctx.cameraWorldX;
						const float swy_ = ctx.viewToWorld[1][0]*xs + ctx.viewToWorld[1][1]*ys
						                 + ctx.viewToWorld[1][2]*zs + ctx.cameraWorldY;
						const float swz_ = ctx.viewToWorld[2][0]*xs + ctx.viewToWorld[2][1]*ys
						                 + ctx.viewToWorld[2][2]*zs + ctx.cameraWorldZ;
						float fresEC = 0.0f;
						EnvSpecComposeScalar(ctx, lane_envP[k], Mat_, mip_, suv_,
						                     xs, ys, zs, nxs, nys, nzs,
						                     swx_, swy_, swz_,
						                     lane_texB[k], lane_texG[k], lane_texR[k],
						                     lane_gloss[k], metalM_,
						                     roughMapOnG, envReflGainG,
						                     envBrdfAnalyticG, multiScatterG, metalTintLinG, sBs, sGs, sRs, &fresEC);
						// (1-F) diffuse energy conservation (see wave-1).
						if (diffuseEnergyG) {
							const float dc = 1.0f - fresEC;
							fdBs *= dc; fdGs *= dc; fdRs *= dc;
						}
					}
					// Per-material specular response multiplier (see wave-1):
					// covers the redo lane's analytic + env-composed spec.
					if (lane_specMul[k] != 1.0f) {
						sBs *= lane_specMul[k]; sGs *= lane_specMul[k]; sRs *= lane_specMul[k];
					}
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
					// (1-F) diffuse energy conservation: capture this lane's
					// Fresnel from whichever env path runs, apply below.
					float fresLane = 0.0f;
					// Env-only lane (Reflection > 0, no spec, not water —
					// city windows): the vec light loop already produced the
					// diffuse; add ONLY the env-specular compose on top.
					// Same shared helper + insertion point as wave-1; forcing
					// these lanes through the full scalar-light redo instead
					// measured 289 ms/iter on city (vs ~77 baseline) for
					// nothing — the redo duplicated the vec loop's work.
					if (lane_envP[k] && envVecReady) {
						if (g_envVecStats) g_envCntOvEnvVec.fetch_add(1, std::memory_order_relaxed);
						// Vec front-end already produced this lane's world
						// reflect dir, Fresnel weight and mip; finish with
						// the shared scalar face pick + bilinear fetch.
						const fds::EnvPanoLinear* envP_ = lane_envP[k];
						// Live water: same lookup-dir perturbation as the
						// scalar compose (parity between the two paths), with
						// the same single-projection form — see
						// EnvLiveWaterWeightAtFaceUV. This lane IS the city
						// glass, so it is where the duplicate projection was
						// actually being paid.
						int face; float cu, cvv;
						fds::EnvCube_DirToFaceUV(envRvx[k], envRvy[k],
						                         envRvz[k], face, cu, cvv);
						{
							const float lwW = EnvLiveWaterWeightAtFaceUV(
								envP_->bakeY, envRvy[k],
								envP_->waterMask, envP_->waterMaskRes,
								face, cu, cvv);
							if (lwW > 0.0f) {
								fds::EnvLiveWater_TiltDir(
									envP_->bakeX, envP_->bakeY, envP_->bakeZ,
									envRvx[k], envRvy[k], envRvz[k], lwW);
								fds::EnvCube_DirToFaceUV(envRvx[k], envRvy[k],
								                         envRvz[k], face, cu, cvv);
							}
						}
						const float lvlF = envLvlF[k];
						const int lvl0 = int(lvlF);
						const int lvl1 = lvl0 + 1 < envP_->numMips ? lvl0 + 1 : lvl0;
						const float lf = lvlF - float(lvl0);
						float b0, g0, r0;
						EnvCubeFetchBil(envP_, lvl0, face, cu, cvv, b0, g0, r0);
						if (lvl1 != lvl0) {
							float b1, g1, r1;
							EnvCubeFetchBil(envP_, lvl1, face, cu, cvv, b1, g1, r1);
							b0 += lf * (b1 - b0);
							g0 += lf * (g1 - g0);
							r0 += lf * (r1 - r0);
						}
						// ×lane_specMul: the env-specular IS this lane's whole
						// specular term (×1.0f = exact identity, byte-null).
						const float ekS = envEk[k] * lane_specMul[k];
						outB += int(b0 * ekS);
						outG += int(g0 * ekS);
						outR += int(r0 * ekS);
						fresLane = envFres[k];
					} else if (lane_envP[k]) {
						if (g_envVecStats) g_envCntOvEnvScalar.fetch_add(1, std::memory_order_relaxed);
						const uint32_t m_   = lane_mat32[k];
						const uint32_t mid_ = (m_ >> 20) & 0xFF;
						const uint32_t mip_ = (m_ >> 28) & 0xF;
						const uint32_t suv_ = m_ & 0xFFFFF;
						Material *Mat_ = ctx.matTable.data[mid_];
						float metalM_ = 0.0f;
						if (metalMapOnG && Mat_->MetallicMap) {
							const byte *md_ = (mip_ < Mat_->MetallicMap->numMipmaps)
								? reinterpret_cast<const byte*>(Mat_->MetallicMap->Mipmap[mip_]) : nullptr;
							if (md_) metalM_ = float(md_[suv_]) * (1.0f/255.0f);
						}
						const float xs = ax[k], ys = ay[k], zs = az_lane[k];
						const float swx_ = ctx.viewToWorld[0][0]*xs + ctx.viewToWorld[0][1]*ys
						                 + ctx.viewToWorld[0][2]*zs + ctx.cameraWorldX;
						const float swy_ = ctx.viewToWorld[1][0]*xs + ctx.viewToWorld[1][1]*ys
						                 + ctx.viewToWorld[1][2]*zs + ctx.cameraWorldY;
						const float swz_ = ctx.viewToWorld[2][0]*xs + ctx.viewToWorld[2][1]*ys
						                 + ctx.viewToWorld[2][2]*zs + ctx.cameraWorldZ;
						float sBs = 0, sGs = 0, sRs = 0;
						EnvSpecComposeScalar(ctx, lane_envP[k], Mat_, mip_, suv_,
						                     xs, ys, zs, anx[k], any_l[k], anz[k],
						                     swx_, swy_, swz_,
						                     lane_texB[k], lane_texG[k], lane_texR[k],
						                     lane_gloss[k], metalM_,
						                     roughMapOnG, envReflGainG,
						                     envBrdfAnalyticG, multiScatterG, metalTintLinG, sBs, sGs, sRs, &fresLane);
						// ×lane_specMul (see the vec env lane above).
						outB += int(sBs * lane_specMul[k]);
						outG += int(sGs * lane_specMul[k]);
						outR += int(sRs * lane_specMul[k]);
					}
					// (1-F) diffuse energy conservation: the diffuse landed in
					// out* as int(vf*[k]) above; re-weight it by (1-fres) with
					// an additive integer correction so the OFF path (gated) is
					// untouched byte-for-byte. Only env lanes carry a Fresnel.
					if (diffuseEnergyG && lane_envP[k]) {
						const float dc = 1.0f - fresLane;
						outB += int(vfB[k] * dc) - int(vfB[k]);
						outG += int(vfG[k] * dc) - int(vfG[k]);
						outR += int(vfR[k] * dc) - int(vfR[k]);
					}
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
	// SKIPPED for an INLINE (offscreen-bake) dispatch: the calling thread would
	// immediately re-acquire its own permit, and that shared semaphore is
	// contended by every pool thread — 3.4-4.0 us of CORE time per round trip
	// at 12 threads, paid 96x per 64x64 shard cell. See
	// DeferredLightingCtx::inlineDispatch.
	if (!ctx.inlineDispatch) renderns::tileDone.release();
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
	const int XRes = ctx.xres, YRes = ctx.yres;
	byte *const VPage = ctx.vpage;
	word *const ZPage16 = ctx.zpage16;
	const float CntrEX = ctx.cntrEX, CntrEY = ctx.cntrEY;
	const meka::GBuffer &gb = *ctx.gb;
	dword *out = reinterpret_cast<dword *>(VPage);
	const bool specGlobalOn = Specular_Factor > 0.0f;
	const bool  roughMapOnG    = fds::FeatureFlags::roughness_map();   // see main kernel
	const float roughStrengthG = fds::FeatureFlags::roughness_strength();
	// Env-reflection + metalness state — the fallback below replays the
	// wave-1 kernel and must include the env compose (see EnvSpecComposeScalar;
	// its absence here was the quarter-mode "reflection = dot grid" bug).
	const fds::EnvPanoLinear *const *envTabG = fds::FeatureFlags::env_refl()
	    ? fds::EnvReflection_Table(ctx.Sc) : nullptr;
	const float envReflGainG = fds::FeatureFlags::env_refl_gain();
	const bool  envBrdfAnalyticG = fds::FeatureFlags::env_brdf_analytic();
	const bool  multiScatterG  = fds::FeatureFlags::pbr_multiscatter();
	const bool  metalTintLinG  = MetalTintLinearActive(ctx);   // linear frame only
	const bool  metalMapOnG  = fds::FeatureFlags::metal_map();
	const bool  diffuseEnergyG = fds::FeatureFlags::diffuse_energy();
	// --sh_ambient: SH irradiance coefficients (null = off / not baked).
	const float* shCoefG     = fds::FeatureFlags::sh_ambient()
		? fds::SHAmbient_Coeffs(ctx.Sc) : nullptr;
	const bool quarter      = deferredLightingQuarterEnabled();
	const bool checker      = deferredLightingCheckerboardEnabled() && !quarter;
	// --deferred_checker_env_full: env-reflective cells were shaded at full
	// rate in wave 1, so this fill must leave them alone.
	const bool checkerEnvFullG = (checker || quarter)
	    && fds::FeatureFlags::deferred_checker_env_full();
	// --deferred_checker_edge_full: wave 1 already shaded every cell whose
	// neighbours are all incompatible (the fill's OTHER full-shade trigger).
	// Only the SCALAR wave-1 kernel evaluates that predicate — the OuterVec
	// kernel drops on parity alone — so skipping here under OuterVec would
	// leave those cells shaded by neither wave (the hole class this commit
	// closes for the env row). Fall back to the reduced re-shade there.
	const bool checkerEdgeFullG = (checker || quarter)
	    && fds::FeatureFlags::deferred_checker_edge_full()
	    && !deferredLightingOuterVecEnabled();
	const bool hdrWrite     = fds::FeatureFlags::hdr() && ctx.hdrBuf != nullptr;   // HDR B1: see main kernel
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
	// --deferred_fill_hdr_skip: hoist --quarter_tex_sharp (it was read once per
	// FILLED PIXEL) and gate the LDR sharp-reconstruction accumulators, which
	// are three divisions per compatible neighbour that only the !hdrWrite arm
	// below ever reads.
	const bool sTexSharp   = fds::FeatureFlags::quarter_tex_sharp();
	const bool fillLdrSharp = !(fds::FeatureFlags::deferred_fill_hdr_skip() && hdrWrite);
	// --deferred_fill_ldr_skip: when this pass ends in the tonemap, every VPage
	// pixel is rewritten from hdrBuf, so the fill's 8-bit average is a value
	// nothing reads. ctx.ldrDiscarded is the promise (see DeferredCommon.h) —
	// hdrWrite alone is NOT sufficient and using it is a bug.
	const bool fillLdrSkip = fds::FeatureFlags::deferred_fill_ldr_skip()
	    && hdrWrite && ctx.ldrDiscarded;
#if FDS_W2_ABLATE
	float w2Sink = 0.0f;
#endif
#if FDS_W2_CENSUS
	uint64_t w2cn[W2_CEN_N] = {0};
	const bool w2Census = fds::FeatureFlags::omni_census() && ctx.xres >= 640;
#endif
	for (int py = y1; py < y2; ++py) {
		for (int px = x1; px < x2; ++px) {
			if (quarter) {
				if (((px | py) & 1) == 0) continue;  // wave-1 shaded
			} else {
				if (((px ^ py) & 1) == 0) continue;
			}
#if FDS_W2_ABLATE
			float w2Keep = 0.0f;
#endif
			W2_ABL_CUT(1, float(px));
			W2_CEN(0);

			const size_t i = size_t(py) * XRes + px;
			const word zEnc = ZPage16[i];
			if (zEnc == 0) { W2_CEN(1); continue; }
			W2_ABL_CUT(2, float(zEnc));

			// Per-pixel mirror id — used by the wave-2 fallback shading
			// to filter lights matching the pixel's mirror context.
			const uint32_t pmid = gb.mirrorId.empty()
			    ? 0u : uint32_t(gb.mirrorId[i]);

			const uint32_t mat32  = gb.txtr[i];
			// Forward content (reflective env disco ball, additive) writes Z
			// but a sentinel matID -- wave-1 skips it, so it stays uncovered and
			// is lifted from VPage in HDR. The fill must skip it too, else it
			// shades the sentinel as garbage into g_hdrBuf at the wave-2 cells
			// (deferred-quarter checker over the forward surface, HDR only; in
			// LDR the forward filler overwrites VPage so it is invisible -> gate
			// on hdrWrite to keep the LDR fill byte-exact).
			if (hdrWrite && (mat32 == 0xFFFFFFFFu || mat32 == 0xFFFFFFFEu)) { W2_CEN(2); continue; }  // both forward sentinels
			const uint32_t matIDc = (mat32 >> 20) & 0xFF;
			W2_ABL_CUT(3, float(matIDc + pmid));

			// Env-reflective materials always take the full-shade fallback:
			// BOTH averaging models break on them. The plain average carries
			// env only from compatible neighbors, and the texture-decouple
			// reconstruction scales neighbor radiance by (texel_i/texel_n)^exp
			// — a model for albedo-modulated diffuse that CRUSHES the env
			// term (albedo-independent spec) wherever the pixel's texel is
			// darker than its neighbor's. On the cockpit glass this rendered
			// reflections as alternating lit/dark columns. Reflective pixels
			// are a small fraction of the frame; full shading them is cheap.
			const bool envForceFull = envTabG && envTabG[matIDc] != nullptr;
			if (envForceFull) W2_CEN(11);
			// --deferred_checker_env_full: wave 1 already shaded this
			// pixel with the REAL kernel (see the drop test there), so
			// the reduced fallback below must not overwrite it.
			if (envForceFull && checkerEnvFullG) { W2_CEN(3); continue; }
			W2_ABL_CUT(4, float(envForceFull));

			// Center normal decoded once; reused by every fill pattern's
			// neighbor-similarity test below. Cheap enough vs the avoided
			// full shading that we always decode (even if matID fails).
			float ncX = 0, ncY = 0, ncZ = 0;
			if (quarterNormalCheck) {
				meka::oct_decode_u32(gb.normal[i], ncX, ncY, ncZ);
			}
			W2_ABL_CUT(5, ncX + ncY + ncZ);
			auto neighborNormalOk = [&](size_t ni) -> bool {
				if (!quarterNormalCheck) return true;
				W2_CEN(13);
				float nx, ny, nz;
				meka::oct_decode_u32(gb.normal[ni], nx, ny, nz);
				return (ncX*nx + ncY*ny + ncZ*nz) >= quarterNormalCos;
			};
			// Z-discontinuity check: relative depth diff vs center.
			// Center zEnc loaded above (`zEnc`). Compared against neighbor
			// zEnc via a single signed compare and bound. Zero neighbor
			// zEnc (= sky/empty) always fails — averaging in sky pixels
			// would smear edges.
			auto neighborZOk = [&](size_t ni) -> bool {
				if (!quarterZCheck) return true;
				const word zN = ZPage16[ni];
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
			const Texture *ownTexture = nullptr;   // resolved by the CENTRE fetch
			auto fetchTexel = [&](size_t j, float &tb, float &tg, float &tr) -> bool {
				const uint32_t m32 = gb.txtr[j];
				const uint32_t mid = (m32 >> 20) & 0xFF;
				if (mid >= ctx.matTable.count) return false;
				const Material *M = ctx.matTable.data[mid];
				if (!M || !M->Txtr) return false;
				ownTexture = M->Txtr;
				const dword *td = (const dword *)M->Txtr->Mipmap[(m32 >> 28) & 0xF];
				if (!td) return false;
				const dword t = td[m32 & 0xFFFFF];
				tb = float(t & 0xFF); tg = float((t >> 8) & 0xFF); tr = float((t >> 16) & 0xFF);
				return true;
			};
			// The NEIGHBOUR form. Only ever called
			// under `haveOwn` (the centre fetch above returned true, so
			// `ownTexture` is that material's non-null Texture) and only after
			// neighborCompatible has proved `mID == matIDc` — i.e. the
			// neighbour's material IS the centre's, and re-resolving it costs a
			// bounds check, a table load and two null tests for a pointer we
			// already hold. Same texture object, same mip word, same texel.
			auto fetchTexelNb = [&](size_t j, float &tb, float &tg, float &tr) -> bool {
				const uint32_t m32 = gb.txtr[j];
				const dword *td = (const dword *)ownTexture->Mipmap[(m32 >> 28) & 0xF];
				if (!td) return false;
				const dword t = td[m32 & 0xFFFFF];
				tb = float(t & 0xFF); tg = float((t >> 8) & 0xFF); tr = float((t >> 16) & 0xFF);
				return true;
			};
			bool matched = false;
			if (envForceFull) {
				// fall through to the full-shade fallback below
			} else if (quarter) {
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
					if (px < XRes - 1) nidx[nc++] = i + 1;
				} else if (!odd_x && odd_y) {
					if (py > 0)        nidx[nc++] = i - XRes;
					if (py < YRes - 1) nidx[nc++] = i + XRes;
				} else {
					if (px > 0 && py > 0)               nidx[nc++] = i - XRes - 1;
					if (px < XRes - 1 && py > 0)        nidx[nc++] = i - XRes + 1;
					if (px > 0 && py < YRes - 1)        nidx[nc++] = i + XRes - 1;
					if (px < XRes - 1 && py < YRes - 1) nidx[nc++] = i + XRes + 1;
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
					dword p = 0;
					if (!fillLdrSkip) {   // --deferred_fill_ldr_skip
						p = out[nidx[k]];
						sumB += int(p & 0xFF);
						sumG += int((p >> 8) & 0xFF);
						sumR += int((p >> 16) & 0xFF);
					}
					const fds::hdrf* nh = hdrWrite ? (ctx.hdrBuf + nidx[k]*4) : nullptr;
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
						if (fetchTexelNb(nidx[k], nb, ng, nr) &&
						    (nrB = ownB / std::max(nb, 1.0f)) <= 4.0f &&
						    (nrG = ownG / std::max(ng, 1.0f)) <= 4.0f &&
						    (nrR = ownR / std::max(nr, 1.0f)) <= 4.0f) {
							// --deferred_fill_hdr_skip: these three DIVISIONS feed
							// only the `!hdrWrite` arm below.
							if (fillLdrSharp) {
							slB += float(p & 0xFF)        * 256.0f / std::max(nb, 1.0f);
							slG += float((p >> 8) & 0xFF)  * 256.0f / std::max(ng, 1.0f);
							slR += float((p >> 16) & 0xFF) * 256.0f / std::max(nr, 1.0f);
							}
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
					if (fillLdrSkip) {
						// --deferred_fill_ldr_skip: the tonemap overwrites it.
					} else if (haveOwn && nsharp > 0 && !hdrWrite) {
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
						fds::hdrf* h = ctx.hdrBuf + i*4;
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
				if (px < XRes - 1) nidx[nc++] = i + 1;
				if (nc == 0) { W2_CEN(4); continue; }
				W2_ABL_CUT(6, float(nc));
				int sumR = 0, sumG = 0, sumB = 0;
				float hsB = 0, hsG = 0, hsR = 0;   // HDR: parallel float-radiance average
				// C (texture/lighting decouple) — same divide-out + trust region
				// as the quarter path above; without it the checker fill plain-
				// averages neighbour colours and mushes far-floor texture detail
				// (measured |checker-full| 1.32 vs quarter+C 0.31 on greets).
				float ownB=0, ownG=0, ownR=0;
				const bool haveOwn = sTexSharp && fetchTexel(i, ownB, ownG, ownR);
				if (haveOwn) W2_CEN(7);
				W2_ABL_CUT(7, ownB + ownG + ownR);
				float slB=0, slG=0, slR=0;                 // LDR reconstructed lighting
				float ahB=0, ahG=0, ahR=0; int nsharp=0;   // HDR reconstructed radiance
				int n = 0;
				for (int k = 0; k < nc; ++k) {
					if (!neighborCompatible(nidx[k], matIDc)) continue;
					dword p = 0;
					if (!fillLdrSkip) {   // --deferred_fill_ldr_skip
						p = out[nidx[k]];
						sumB += int(p & 0xFF);
						sumG += int((p >> 8) & 0xFF);
						sumR += int((p >> 16) & 0xFF);
					}
					const fds::hdrf* nh = hdrWrite ? (ctx.hdrBuf + nidx[k]*4) : nullptr;
					if (nh) { hsB += nh[0]; hsG += nh[1]; hsR += nh[2]; }
					if (haveOwn) {
						float nb, ng, nr, nrB, nrG, nrR;
						if (fetchTexelNb(nidx[k], nb, ng, nr) &&
						    (nrB = ownB / std::max(nb, 1.0f)) <= 4.0f &&
						    (nrG = ownG / std::max(ng, 1.0f)) <= 4.0f &&
						    (nrR = ownR / std::max(nr, 1.0f)) <= 4.0f) {
							// --deferred_fill_hdr_skip: these three DIVISIONS feed
							// only the `!hdrWrite` arm below.
							if (fillLdrSharp) {
							slB += float(p & 0xFF)        * 256.0f / std::max(nb, 1.0f);
							slG += float((p >> 8) & 0xFF)  * 256.0f / std::max(ng, 1.0f);
							slR += float((p >> 16) & 0xFF) * 256.0f / std::max(nr, 1.0f);
							}
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
				W2_ABL_CUT(8, float(n + nsharp) + hsB + ahB + slB + float(sumB));
				if (n > 0) {
					W2_CEN(5); W2_CEN_ADD(8, n); W2_CEN_ADD(9, nsharp);
					if (fillLdrSkip) {
						// --deferred_fill_ldr_skip: the tonemap overwrites it.
					} else if (haveOwn && nsharp > 0 && !hdrWrite) {
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
						fds::hdrf* h = ctx.hdrBuf + i*4;
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
			W2_ABL_CUT(9, 1.0f);
			// --deferred_checker_edge_full: reaching here means NO neighbour was
			// compatible — the exact condition wave 1's fillFallsBackHere()
			// predicts — so wave 1 already shaded this pixel with the REAL
			// kernel. `!envForceFull` because an env pixel arrives here without
			// the averaging having run at all (it is force-full by a different
			// rule), and wave 1 leaves those to --deferred_checker_env_full.
			// Free: no second predicate evaluation, `matched` IS the predicate.
			if (checkerEdgeFullG && !envForceFull) { W2_CEN(12); continue; }
			W2_CEN(6);

			// Fallback path — replays the wave-1 kernel for this pixel.
			// We don't expect this branch to be hot (most frame area is
			// continuous surface), so the simpler scalar-only fallback
			// keeps the code small even if we're in vec mode otherwise.
			const uint32_t miplevel    = (mat32 >> 28) & 0xF;
			const uint32_t matID       = matIDc;
			const uint32_t swizzledUV  = mat32 & 0xFFFFF;
			if (matID >= ctx.matTable.count) { W2_CEN(10); continue; }
			Material *Mat = ctx.matTable.data[matID];
			if (!Mat || !Mat->Txtr) { W2_CEN(10); continue; }
			const dword *texData = (const dword *)Mat->Txtr->Mipmap[miplevel];
			if (!texData) { W2_CEN(10); continue; }
			const dword texel = texData[swizzledUV];
			const float texB = float(texel & 0xFF)         * Mat->TintB;  // editor tint (see main kernel)
			const float texG = float((texel >> 8) & 0xFF)  * Mat->TintG;
			const float texR = float((texel >> 16) & 0xFF) * Mat->TintR;

			float nx, ny, nz;
			meka::oct_decode_u32(gb.normal[i], nx, ny, nz);

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
			const float x = (float(px) - CntrEX) * z * ctx.invFOVX;
			const float y = (CntrEY - float(py)) * z * ctx.invFOVY;

			float lB, lG, lR;
			if (shCoefG) {
				// --sh_ambient (see main kernel): directional SH irradiance
				// along the shading normal in place of the flat Sc->Ambient.
				const float wnx = ctx.viewToWorld[0][0]*nx + ctx.viewToWorld[0][1]*ny + ctx.viewToWorld[0][2]*nz;
				const float wny = ctx.viewToWorld[1][0]*nx + ctx.viewToWorld[1][1]*ny + ctx.viewToWorld[1][2]*nz;
				const float wnz = ctx.viewToWorld[2][0]*nx + ctx.viewToWorld[2][1]*ny + ctx.viewToWorld[2][2]*nz;
				float eB, eG, eR;
				shEvalIrradiance(shCoefG, wnx, wny, wnz, eB, eG, eR);
				if (Mat->Txtr) {
					lB = Mat->Luminosity * 255.0f + Mat->Diffuse * eB;
					lG = Mat->Luminosity * 255.0f + Mat->Diffuse * eG;
					lR = Mat->Luminosity * 255.0f + Mat->Diffuse * eR;
				} else {
					lB = Mat->Luminosity * Mat->BaseCol.B + Mat->Diffuse * eB;
					lG = Mat->Luminosity * Mat->BaseCol.G + Mat->Diffuse * eG;
					lR = Mat->Luminosity * Mat->BaseCol.R + Mat->Diffuse * eR;
				}
			} else if (Mat->Txtr) {
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
			// Metalness (see main kernel): conductors get no diffuse.
			float metalM = 0.0f;
			const bool hasMetal = metalMapOnG && Mat->MetallicMap;
			if (hasMetal) {
				const byte *md = (miplevel < Mat->MetallicMap->numMipmaps)
					? reinterpret_cast<const byte*>(Mat->MetallicMap->Mipmap[miplevel]) : nullptr;
				if (md) metalM = float(md[swizzledUV]) * (1.0f/255.0f);
				if (metalM > 0.0f) {
					const float dk = 1.0f - metalM;
					fdB *= dk; fdG *= dk; fdR *= dk;
				}
			}
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
			// Metals: tint the accumulated analytic highlights by the albedo.
			if (metalM > 0.0f) {
				const float inv255 = 1.0f / 255.0f;
				sB *= 1.0f - metalM + metalM * texB * inv255;
				sG *= 1.0f - metalM + metalM * texG * inv255;
				sR *= 1.0f - metalM + metalM * texR * inv255;
			}
			// Env-specular reflection — the SAME compose as wave-1 (shared
			// helper), so fallback-shaded fill pixels carry reflections.
			{
				const fds::EnvPanoLinear *envP =
					(envTabG && (Mat->Reflection > 0.0f || hasMetal)) ? envTabG[matID] : nullptr;
				if (envP) {
					const float sampleWorldX =
						ctx.viewToWorld[0][0]*x + ctx.viewToWorld[0][1]*y +
						ctx.viewToWorld[0][2]*z + ctx.cameraWorldX;
					const float sampleWorldY =
						ctx.viewToWorld[1][0]*x + ctx.viewToWorld[1][1]*y +
						ctx.viewToWorld[1][2]*z + ctx.cameraWorldY;
					const float sampleWorldZ =
						ctx.viewToWorld[2][0]*x + ctx.viewToWorld[2][1]*y +
						ctx.viewToWorld[2][2]*z + ctx.cameraWorldZ;
					if (g_envVecStats) g_envCntXpar.fetch_add(1, std::memory_order_relaxed);
					float fresEC = 0.0f;
					EnvSpecComposeScalar(ctx, envP, Mat, miplevel, swizzledUV,
					                     x, y, z, nx, ny, nz,
					                     sampleWorldX, sampleWorldY, sampleWorldZ,
					                     texB, texG, texR, gloss, metalM,
					                     roughMapOnG, envReflGainG,
					                     envBrdfAnalyticG, multiScatterG, metalTintLinG, sB, sG, sR, &fresEC);
					// (1-F) diffuse energy conservation (see wave-1); scales
					// the LDR combine and the HDR radiance below.
					if (diffuseEnergyG) {
						const float dc = 1.0f - fresEC;
						fdB *= dc; fdG *= dc; fdR *= dc;
					}
				}
			}
			// Per-material specular response multiplier (see wave-1).
			if (Mat->SpecMul != 1.0f) {
				sB *= Mat->SpecMul; sG *= Mat->SpecMul; sR *= Mat->SpecMul;
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
				fds::hdrf* h = ctx.hdrBuf + i * 4;
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
#if FDS_W2_CENSUS
	if (w2Census) {
		for (int i = 0; i < W2_CEN_N; ++i)
			if (w2cn[i]) g_w2Cen[i].fetch_add(w2cn[i], std::memory_order_relaxed);
	}
#endif
#if FDS_W2_ABLATE
	g_w2AblSink = w2Sink;   // one volatile store per tile call
#endif

	// One permit per completed tile (see renderns::tileDone in RENDER.CPP).
	// SKIPPED for an INLINE (offscreen-bake) dispatch: the calling thread would
	// immediately re-acquire its own permit, and that shared semaphore is
	// contended by every pool thread — 3.4-4.0 us of CORE time per round trip
	// at 12 threads, paid 96x per 64x64 shard cell. See
	// DeferredLightingCtx::inlineDispatch.
	if (!ctx.inlineDispatch) renderns::tileDone.release();
}

// Per-frame setup + dispatch tile jobs across the ThreadPool. Same 6×4
// split the rasterizer uses; tiles are independent (each writes a
// disjoint slice of VPage). Reuses the renderns::tileCounter +
// condition variable that Render() already uses for the rasterizer
// pass — fine because we wait synchronously between Render's tile
// dispatch and our own.
void Render_DeferredLighting(DeferredLightingCtx &ctx, const DeferredOverride *ov) {
	// Override-or-global addressing. ov=nullptr (main frame) → engine globals,
	// byte-identical. ov!=nullptr (offscreen shard bake) → ov's own G-buffer /
	// camera / scratch buffers, so N bakes run concurrently on the pool. Shadow
	// the legacy global names so the body below is unchanged.
	meka::GBuffer *const gbPtr = (ov && ov->gb) ? ov->gb : g_gbuffer;
	byte *const VPage   = ov ? ov->vpage   : ::VPage;
	word *const ZPage16 = ov ? ov->zpage16 : ::ZPage16;
	if (!gbPtr || !ZPage16 || !VPage) return;
	const meka::GBuffer &gb = *gbPtr;
	meka::GBuffer *const g_gbuffer = gbPtr;            // mirrorMask read below
	Camera  *const View  = (ov && ov->cam) ? ov->cam->view   : ::View;
	const float FOVX     = (ov && ov->cam) ? ov->cam->fovX   : ::FOVX;
	const float FOVY     = (ov && ov->cam) ? ov->cam->fovY   : ::FOVY;
	const float CntrEX   = (ov && ov->cam) ? ov->cam->cntrEX : ::CntrEX;
	const float CntrEY   = (ov && ov->cam) ? ov->cam->cntrEY : ::CntrEY;
	const float g_zscale = (ov && ov->cam) ? ov->cam->zScale : float(::g_zscale);
	const int32_t XRes   = ov ? ov->xres : ::XRes;
	const int32_t YRes   = ov ? ov->yres : ::YRes;
	meka::GBuffer *const g_gbufferTransparent = ov ? ov->gbXpar   : ::g_gbufferTransparent;
	word *const g_xparZ                       = ov ? ov->xparZ    : ::g_xparZ;
	word *const g_xparZBack                   = ov ? ov->xparZBack : ::g_xparZBack;
	const size_t numPixels = size_t(XRes) * size_t(YRes);
	if (gb.normal.size() < numPixels || gb.txtr.size() < numPixels) return;

	Scene *Sc = CurScene;
	if (!Sc) return;

#if FDS_SHARD_BAKE_LAB
	// LAB-ONLY sub-attribution of this call, for the offscreen (ov) path only:
	// which part of the per-INVOCATION work dominates when the target is a 64²
	// shard cell. See FrameState.h for why it is compile-time gated.
	static const bool sDlProf = (std::getenv("FDS_SHARD_REFL_PROF") != nullptr);
	const bool dlProf = sDlProf && ov;
	auto dlNow = [&]() {
		return dlProf ? std::chrono::steady_clock::now()
		              : std::chrono::steady_clock::time_point{};
	};
	auto dlAdd = [&](double &acc, const std::chrono::steady_clock::time_point &a,
	                 const std::chrono::steady_clock::time_point &b) {
		if (dlProf) acc += std::chrono::duration<double, std::milli>(b - a).count();
	};
	if (dlProf) ++fds::g_phDlCalls;
	auto _dlA = dlNow();
#endif
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
		Vector_Sub(&O->IPos, &View->ISource, &u);
		MatrixXVector(View->Mat, &u, &w);
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
		// using the same View->Mat that placed posX/Y/Z. Pure rotation,
		// no translation — IDir is a direction not a point.
		if (O->Type == Light_SpotLight) {
			Vector dirView;
			MatrixXVector(View->Mat, (Vector*)&O->IDir, &dirView);
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
		// Cone sibling of haloMul: per-spot volumetric beam gain (authored
		// via LWS VolumetricLightIntensity → Omni::VolBeamGain). 0 = unset
		// → 1.0, so code-created beams (greets disco, bounce pool) and
		// legacy content are bit-exact (×1.0 is exact in float).
		lights.coneGain      [numLights] = (O->VolBeamGain > 0.0f) ? O->VolBeamGain : 1.0f;
		lights.haloRange     [numLights] = haloRange;
		lights.haloRange2    [numLights] = haloRange * haloRange;
		lights.haloRRange    [numLights] = (haloRange > 0.0f) ? 1.0f / haloRange : 0.0f;
		// Mirror id: 0 for originals, 1..N for clones from GreetsMirror.
		// Surface lighting kernels gate per pixel against gb.mirrorId.
		lights.mirrorId      [numLights] = O->mirrorId;
		++numLights;
	}
	TailProf::mark("light-list", _llist);   // SoA build: per-light xform + linear shadow-map scans
#if FDS_SHARD_BAKE_LAB
	{ const auto _b = dlNow(); dlAdd(fds::g_phDlLights, _dlA, _b); _dlA = _b;
	  if (dlProf) fds::g_phDlLightN += uint64_t(numLights); }
#endif

	// Tile grid. The main frame keeps the engine-wide 12×8. An OFFSCREEN target
	// (ov) sizes its grid to ITSELF: 12×8 over a 64² shard cell means 8×8-pixel
	// tiles — 96 tile-kernel invocations for 4 096 pixels, 32 of them entirely
	// off the right edge — and the per-tile fixed cost is then a third of the
	// whole pass. --deferred_offscreen_tile_px is the minimum tile edge in
	// pixels (0 = legacy: use the main grid offscreen too). See the flag text
	// for the measured sweep; the reflection atlas is byte-identical at every
	// grid tried, 12×8 through 1×1.
	int numTilesX = DEFERRED_NUM_TILES_X;
	int numTilesY = DEFERRED_NUM_TILES_Y;
	if (ov) {
		const int minPx = fds::FeatureFlags::deferred_offscreen_tile_px();
		if (minPx > 0) {
			numTilesX = std::clamp(XRes / minPx, 1, DEFERRED_NUM_TILES_X);
			numTilesY = std::clamp(YRes / minPx, 1, DEFERRED_NUM_TILES_Y);
		}
	}
	// Round tileSizeX up to the next multiple of 8 so the OuterVec
	// kernel's 8-wide vec loop sees aligned tiles for all-but-the-last
	// tile column. The last tile width can still be unaligned at non-
	// multiple-of-8 XRes (rare on real displays — most are 8-aligned)
	// — handled by the lane-in-range mask inside OuterVec.
	// tileSizeY doesn't need this because the kernels iterate rows one
	// at a time, not in 8-tall groups.
	const int rawTileX  = (XRes + (numTilesX - 1)) / numTilesX;
	const int tileSizeX = (rawTileX + 7) & ~7;
	const int tileSizeY = (YRes + (numTilesY - 1)) / numTilesY;

	// Per-tile light culling: project each omni's bounding sphere
	// into screen space, find which tiles it overlaps, populate
	// indices[] per tile.
	static TileLights s_tileLights[DEFERRED_NUM_TILES];
	TileLights *const tileLights = (ov && ov->tileLights) ? ov->tileLights : s_tileLights;
	const float invZScale = 1.0f / float(g_zscale);
	const long long _lsetup = TailProf::nowNs();
	computeTileDepthBounds(tileLights, numTilesX, numTilesY,
	                       tileSizeX, tileSizeY, XRes, YRes,
	                       invZScale, reinterpret_cast<const uint16_t*>(ZPage16));
	TailProf::mark("depth-bounds", _lsetup);   // per-tile z-buffer scan (par-able)
#if FDS_SHARD_BAKE_LAB
	{ const auto _b = dlNow(); dlAdd(fds::g_phDlDepth, _dlA, _b); _dlA = _b; }
#endif
	// Mirror-footprint presence per tile/strip, for the clone-light
	// cull in the list builders. Only computed when a scene actually
	// activated the mask plane (GreetsMirror::BuildMirror).
	const uint8_t *mirrorMaskPlane =
		(g_gbuffer && g_gbuffer->mirrorMask.size() >= numPixels)
		? g_gbuffer->mirrorMask.data() : nullptr;
	// Main-frame only: an offscreen shard bake has no mirrorMask in its
	// G-buffer, so mirrorMaskPlane is null below and this stays untouched.
	static uint32_t tileMirrorPresence[DEFERRED_NUM_TILES];
	const uint32_t *tilePresence = nullptr;
	if (mirrorMaskPlane) {
		const long long _mgrid = TailProf::nowNs();
		computeMirrorPresenceGrid(mirrorMaskPlane, XRes, YRes,
		                          tileSizeX, tileSizeY,
		                          numTilesX, numTilesY,
		                          tileMirrorPresence);
		tilePresence = tileMirrorPresence;
		TailProf::mark("mirror-grid", _mgrid);   // full-res mirrorMask scan
	}
	const fds::CameraContext &camCtx = (ov && ov->cam) ? *ov->cam : fds::g_mainCamera;
	const long long _tcull = TailProf::nowNs();
	buildTileLightLists(tileLights, numTilesX, numTilesY,
	                    tileSizeX, tileSizeY, XRes, YRes,
	                    lights, numLights, tilePresence, camCtx);
	TailProf::mark("tile-cull", _tcull);       // light-major append (harder to par)
#if FDS_SHARD_BAKE_LAB
	{ const auto _b = dlNow(); dlAdd(fds::g_phDlBin, _dlA, _b); _dlA = _b; }
#endif

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
	// the legacy path. (Strip count = ceil(YRes / TILESIZE), capped at
	// DEFERRED_MAX_STRIPS=512.)
	if (!ov && deferredUnifiedTbrEnabled()) {   // strips are main-frame xpar only
		constexpr int STRIP_H = 1 << 3;  // TILESIZE from FILLERS.CPP
		const int numStrips = (YRes + STRIP_H - 1) >> 3;
		static uint32_t stripMirrorPresence[DEFERRED_MAX_STRIPS];  // main-frame only (see above)
		const uint32_t *stripPresence = nullptr;
		if (mirrorMaskPlane) {
			computeMirrorPresenceGrid(mirrorMaskPlane, XRes, YRes,
			                          XRes, STRIP_H,
			                          1, std::min(numStrips, DEFERRED_MAX_STRIPS),
			                          stripMirrorPresence);
			stripPresence = stripMirrorPresence;
		}
		const long long _slist = TailProf::nowNs();
		buildStripLightLists(numStrips, STRIP_H, YRes, lights, numLights,
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
	// Hoist the per-PIXEL Shadow_MaterialSkipsCasting call to one pass over the
	// material table per frame (see DeferredLightingCtx::shadowSkipMask). The
	// kernel's per-pixel guard is `matID >= matTable.count -> continue` and
	// `!Mat -> continue`, both of which run BEFORE the predicate, so entries
	// this loop cannot reach (or that are null) are never consulted — the
	// mask reproduces the old call's answer for every pixel that asks.
	std::memset(ctx.shadowSkipMask, 0, sizeof(ctx.shadowSkipMask));
	{
		const dword nMat = matTable.count < 256u ? matTable.count : 256u;
		for (dword mi = 0; mi < nMat; ++mi) {
			Material *M = matTable.data[mi];
			if (M && Shadow_MaterialSkipsCasting(M))
				ctx.shadowSkipMask[mi >> 6] |= (uint64_t(1) << (mi & 63));
		}
	}
	ctx.tileLights = tileLights;
	// See DeferredLightingCtx::ltNumX — the transparent composite resolves its
	// light tile from the pixel, so it needs this pass's actual grid.
	ctx.ltNumX  = numTilesX;  ctx.ltNumY  = numTilesY;
	ctx.ltSizeX = tileSizeX;  ctx.ltSizeY = tileSizeY;
	ctx.hasMirrorPresence = (tilePresence != nullptr);
	if (tilePresence)
		std::memcpy(ctx.tileMirrorPresence, tilePresence,
		            sizeof(ctx.tileMirrorPresence));
	ctx.invFOVX    = 1.0f / FOVX;
	ctx.invFOVY    = 1.0f / FOVY;
	ctx.invZScale  = 1.0f / float(g_zscale);
	ctx.Sc         = Sc;
	ctx.waterMatID = g_deferredWaterMatID;
	// View → world (transpose of view rotation + camera origin). Used
	// per pixel for cube shadow sampling to convert view-space sample
	// to world for face selection. View.Mat is a pure rotation, so
	// transpose == inverse.
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 3; ++c)
			ctx.viewToWorld[r][c] = View->Mat[c][r];
	if (g_envVecStats) {
		static int sN = 0;
		if ((++sN & 63) == 0)
			std::fprintf(stderr, "[ENVVEC] wave1=%llu fill=%llu ovScalar=%llu "
			    "ovEnvScalar=%llu ovEnvVec=%llu\n",
			    (unsigned long long)g_envCntWave1.load(),
			    (unsigned long long)g_envCntXpar.load(),
			    (unsigned long long)g_envCntOvScalar.load(),
			    (unsigned long long)g_envCntOvEnvScalar.load(),
			    (unsigned long long)g_envCntOvEnvVec.load());
	}
	// FDS_ENVTRACE: one invocation id per kernel setup — if the setup line
	// prints twice for one Timer value, the kernel runs two passes per frame
	// and the trace lines say which pass composed the traced pixel.
	if (EnvTraceGet()) {
		const uint32_t prev = g_envTraceInvoke.load(std::memory_order_relaxed);
		const uint32_t n = g_envCensusN.exchange(0, std::memory_order_relaxed);
		const uint32_t ny = g_envCensusNegY.exchange(0, std::memory_order_relaxed);
		const int x0 = g_envCensusX0.exchange(1<<30);
		const int x1 = g_envCensusX1.exchange(-(1<<30));
		const int y0 = g_envCensusY0.exchange(1<<30);
		const int y1 = g_envCensusY1.exchange(-(1<<30));
		if (n)
			std::fprintf(stderr, "[ENVCENSUS] inv=%u n=%u negY=%u "
			    "bbox=(%d..%d,%d..%d)\n", prev, n, ny, x0, x1, y0, y1);
		const uint32_t inv = g_envTraceInvoke.fetch_add(1,
			std::memory_order_relaxed) + 1;
		std::fprintf(stderr, "[ENVTRACE-SETUP] inv=%u ov=%d t=%.1f cam=(%.2f,%.2f,%.2f) "
		    "fwd=(%.4f,%.4f,%.4f)\n", inv, ov ? 1 : 0, double(Timer),
		    View->ISource.x, View->ISource.y, View->ISource.z,
		    View->Mat[2][0], View->Mat[2][1], View->Mat[2][2]);
	}
	if (std::getenv("FDS_CAMLOG")) {
		static int sCamLogN = 0;
		if (sCamLogN < 400)
			std::fprintf(stderr, "[CAMLOG] n=%d t=%.1f cf=%.2f view=%p "
			    "pos=(%.2f,%.2f,%.2f) fwd=(%.4f,%.4f,%.4f) fovx=%.2f cex=%.1f\n",
			    sCamLogN++, double(Timer), double(CurFrame), (void*)View,
			    View->ISource.x, View->ISource.y, View->ISource.z,
			    View->Mat[2][0], View->Mat[2][1], View->Mat[2][2],
			    double(FOVX), double(CntrEX));
	}
	if (std::getenv("ENVDBG4")) {
		const Camera* gv = fds::g_mainCamera.view;
		if (gv && (gv != View
		    || gv->ISource.x != View->ISource.x
		    || gv->Mat[2][0] != View->Mat[2][0]
		    || gv->Mat[2][2] != View->Mat[2][2]))
			std::fprintf(stderr, "[ENVDBG4] ctx-vs-geometry camera MISMATCH: "
			    "View=%p src(%.2f,%.2f,%.2f) fwd(%.3f,%.3f,%.3f) | "
			    "gMain=%p src(%.2f,%.2f,%.2f) fwd(%.3f,%.3f,%.3f)\n",
			    (void*)View, View->ISource.x, View->ISource.y, View->ISource.z,
			    View->Mat[2][0], View->Mat[2][1], View->Mat[2][2],
			    (void*)gv, gv->ISource.x, gv->ISource.y, gv->ISource.z,
			    gv->Mat[2][0], gv->Mat[2][1], gv->Mat[2][2]);
	}
	ctx.cameraWorldX = View->ISource.x;
	ctx.cameraWorldY = View->ISource.y;
	ctx.cameraWorldZ = View->ISource.z;
	// Render-target addressing for the tile kernels (RenderContext migration):
	// snapshot the globals here so the kernels read ctx, not the globals.
	ctx.xres       = XRes;
	ctx.yres       = YRes;
	ctx.vpage      = VPage;
	ctx.zpage16    = ZPage16;
	ctx.cntrEX     = CntrEX;
	ctx.cntrEY     = CntrEY;
	ctx.fovX       = FOVX;
	ctx.fovY       = FOVY;
	ctx.zscale     = float(g_zscale);
	ctx.gbXpar     = g_gbufferTransparent;
	ctx.xparZ      = g_xparZ;
	ctx.xparZBack  = g_xparZBack;
	// HDR radiance target for this pass. Main frame: the global under EXACTLY
	// the predicate the kernels used to evaluate themselves, so it is
	// byte-identical. Offscreen: the override's own (per-worker) buffer, which
	// is what lets a shard bake tonemap like the frame it is composited into
	// instead of falling through to the LDR combine.
	// An override that brings its OWN buffer wins; everything else falls back to
	// the old predicate EVALUATED ON THIS PASS'S DIMS, which is what keeps the
	// mirror RTT working: it brackets its bake with Hdr_BeginFramePass(texW,texH),
	// so g_hdrBuf really is sized for it and Hdr_WritableFor(XRes,YRes) is true —
	// reading `ov ? ov->hdr : …` instead would have silently dropped the RTT off
	// the HDR path. The shard bake with --shard_hdr off lands on nullptr the same
	// way it always did (nothing sized g_hdrBuf to 64²).
	ctx.hdrBuf = (ov && ov->hdr)
	               ? ov->hdr
	               : (fds::Hdr_WritableFor(XRes, YRes) ? fds::g_hdrBuf.data()
	                                                   : nullptr);

	// Wave 1: shade even cells (full deferred kernel). When checkerboard
	// is off, this is the entire pass and odd-cell skip is a no-op.
	// Dispatch: main frame tiles across the pool; an offscreen bake
	// (ov->inlineDispatch) runs the tiles on the calling worker thread (it is
	// itself a pool thread doing a WHOLE render — no nested enqueue). The tile
	// kernel posts a renderns::tileDone permit at its end for the POOL path; on
	// the inline path it posts none and nothing is drained, because the poster
	// and the taker would be the same thread and that semaphore is shared with
	// every other worker (--deferred_inline_tile_sem restores the round trip).
#if FDS_SHARD_BAKE_LAB
	{ const auto _b = dlNow(); dlAdd(fds::g_phDlCtx, _dlA, _b); _dlA = _b; }
#endif
	const bool useOuterVec = deferredLightingOuterVecEnabled();
	const bool inlineDispatch = ov && ov->inlineDispatch;
	// The tile kernels read this to decide whether to post a tileDone permit.
	// Inline = this thread runs the tiles itself, so the permit would come
	// straight back — and the semaphore is shared with every pool thread.
	// --deferred_inline_tile_sem=1 restores the legacy round trip (revert
	// lever + A/B arm); see the flag's own text for the measurement.
	const bool legacyTileSem = fds::FeatureFlags::deferred_inline_tile_sem();
	ctx.inlineDispatch = inlineDispatch && !legacyTileSem;
	if (!inlineDispatch) renderns::tileCounter = 0;
	const TailProf::Stamp _w1q("lighting-w1");
	const int nTiles = numTilesX * numTilesY;
	auto tileBounds = [tileSizeX, tileSizeY, XRes, YRes, numTilesX](int t, int &x1, int &y1, int &x2, int &y2) {
		const int j = t / numTilesX, i = t - j * numTilesX;
		y1 = tileSizeY * j; y2 = std::min(y1 + tileSizeY, YRes);
		x1 = tileSizeX * i; x2 = std::min(x1 + tileSizeX, XRes);
	};
	if (inlineDispatch) {
		for (int t = 0; t < nTiles; ++t) {
			int x1, y1, x2, y2; tileBounds(t, x1, y1, x2, y2);
			if (useOuterVec) Render_DeferredLighting_Tile_OuterVec(ctx, t, x1, y1, x2, y2);
			else             Render_DeferredLighting_Tile(ctx, t, x1, y1, x2, y2);
			// Acquire ONLY under the legacy arm — with ctx.inlineDispatch the
			// kernel posted no permit, so there is nothing to drain.
			if (legacyTileSem) renderns::tileDone.acquire();
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
			// --deferred_prof thread-sum (the kernel releases tileDone from
			// inside, so the add can trail the release — drain() settles it).
			const long long _tp = TailProf::enabled() ? TailProf::nowNs() : 0;
			if (useOuterVec) Render_DeferredLighting_Tile_OuterVec(ctx, t, x1, y1, x2, y2);
			else             Render_DeferredLighting_Tile(ctx, t, x1, y1, x2, y2);
			TailProf::addBusy(_tp);
		});
		TailProf::mark("w1-enqueue", _w1q);
		TailProf::drain(renderns::tileDone, nTiles, "lighting-w1", 2, _w1q);
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
				if (legacyTileSem) renderns::tileDone.acquire();   // see wave 1
			}
		} else {
			// Same dispatchIndexed shape as wave 1 (see above).
			const TailProf::Stamp _w2q("lighting-w2");
			dispatchIndexed(nTiles, nullptr, [&ctx, tileBounds](int t) {
				int x1, y1, x2, y2; tileBounds(t, x1, y1, x2, y2);
				const long long _tp = TailProf::enabled() ? TailProf::nowNs() : 0;
				Render_DeferredLighting_TileFill(ctx, t, x1, y1, x2, y2);
				TailProf::addBusy(_tp);
			});
			TailProf::drain(renderns::tileDone, nTiles, "lighting-w2", 2, _w2q);
		}
#if FDS_W2_CENSUS
		if (fds::FeatureFlags::omni_census() && !inlineDispatch && ctx.xres >= 640)
			W2Census_Report();
#endif
	}

#if FDS_OMNI_CENSUS
	// One line per main-view frame (offscreen bakes never reach the counters —
	// the per-tile gate is xres >= 640, same rule as the tap census).
	if (fds::FeatureFlags::omni_census() && !inlineDispatch && ctx.xres >= 640)
		OmniCensus_Report();
#endif

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
#if FDS_SHARD_BAKE_LAB
	dlAdd(fds::g_phDlTiles, _dlA, dlNow());
#endif
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

