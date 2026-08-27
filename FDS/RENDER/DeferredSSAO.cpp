// ── Screen-space ambient occlusion (deferred path) ────────────────────────
//
// A Crysis-style view-space SSAO post-pass over the deferred G-buffer. It runs
// AFTER Render_DeferredLighting and BEFORE the fog / volumetric / transparent
// passes, darkening surface creases by sampling a hemisphere of neighbour
// depths and counting how many sit in front of the shaded point.
//
// OUTPUT TARGET (auto):
//   - --hdr on  : multiply the LINEAR radiance in g_hdrBuf (B,G,R floats) — the
//                 physically-correct place (AO scales radiance before ACES), and
//                 it survives the end-of-pipeline tonemap. Only covered opaque
//                 pixels the kernel wrote are touched; glow added later
//                 (cones/bolt/transparents/bloom) is NOT occluded.
//   - --hdr off : multiply the 8-bit lit colour already in VPage.
//
// RESOLUTION (--ssao_downscale 1|2|3|4): compute AO on a W/d × H/d grid.
//   - d == 1 : full-res; optional depth-aware box blur denoise; direct apply.
//   - d  > 1 : reduced-res compute, then a JOINT BILATERAL UPSAMPLE at apply —
//              each full-res pixel gathers a (2R+1)² low-res neighbourhood
//              weighted by spatial gaussian × full-res-vs-cell DEPTH similarity
//              × NORMAL similarity. The normal term is what keeps crease detail
//              (panel gaps are normal discontinuities, not depth ones) crisp,
//              and the multi-tap gather denoises the per-cell rotation grain —
//              so it both upsamples and denoises in one edge-aware step (a flat
//              low-res box blur smears ±R*d full-res pixels and washes detail).
//
// View-space reconstruction is byte-for-byte the surface kernel's:
//   z = (0xFF80 - zEnc) * invZScale
//   x = (px - CntrEX) * z * invFOVX ,  y = (CntrEY - py) * z * invFOVY
// inverse (re-project a view point to a pixel):
//   px = CntrEX + (X/Z) * FOVX ,  py = CntrEY - (Y/Z) * FOVY.
// The hemisphere always samples the full-res depth buffer; only the set of
// shaded points and the AO storage are reduced.
//
// All behind --ssao* FeatureFlags; default off. Tiled waves over the same 6×4
// threadpool grid the rest of the deferred post-passes use.

#include <math.h>
#include <vector>
#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <semaphore>

// arm64 only — the NEON fast paths below are already `#if defined(__aarch64__)`
// gated and every one has a scalar fallback, so an x86-64 build simply takes
// the scalar road. Guard spelling matches FDS/FILLERS/SimdHelpers.h.
#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#endif
#include "simde/x86/fma.h"

#include "Base/FDS_VARS.H"
#include "Base/FeatureFlags.h"
#include "Base/Compiler.h"   // FDS_NOINLINE / FDS_ALWAYS_INLINE / FDS_PRINTF_FMT
#include "Base/MemCensus.h"
#include "FILLERS/Mekalele.h"
#include "RENDER/DeferredCommon.h"
#include "RENDER/Hdr.h"
#include "Threads.h"

// Shared tile-drain semaphore (defined in DeferredFastFog.cpp). Reused here;
// SSAO and fog never run concurrently, so sharing the counter is safe.
namespace renderns { extern std::counting_semaphore<INT_MAX> tileDone; }

// Last frame's SSAO wall time in ms, for at-a-glance attribution.
double g_ssaoLastMs = 0.0;

namespace {

// Hemisphere sample kernel, regenerated only when the sample count changes.
// Golden-angle spiral, cosine-weighted, lengths clustered toward the origin so
// occlusion is dominated by near neighbours (standard SSAO kernel shaping).
int                g_kernelN = 0;
std::vector<float> g_kx, g_ky, g_kz;

void buildKernel(int n) {
	if (n == g_kernelN) return;
	g_kx.resize(n); g_ky.resize(n); g_kz.resize(n);
	const float golden = 2.39996323f;
	for (int i = 0; i < n; ++i) {
		const float u    = (float(i) + 0.5f) / float(n);
		const float cosT = sqrtf(1.0f - u);
		const float sinT = sqrtf(u);
		const float phi  = float(i) * golden;
		const float t    = float(i) / float(n);
		const float scale = 0.1f + 0.9f * t * t;
		g_kx[i] = sinT * cosf(phi) * scale;
		g_ky[i] = sinT * sinf(phi) * scale;
		g_kz[i] = cosT * scale;
	}
	g_kernelN = n;
}

// 4×4 TILING rotation table (cos, sin), filled once on the main thread. 16
// golden-ratio-distributed angles, indexed by the low-res cell's (x,y) mod 4.
// The kernel-rotation noise REPEATS every 4 cells, so a matched 4×4 box blur
// averages exactly one full period of the 16 rotations and cancels the per-cell
// rotation variance (the diagonal "hatch") on flat surfaces. A continuous
// per-pixel noise (the old interleaved-gradient angle) never repeats, so no
// finite blur fully resolves it — that residual was the floor banding.
float g_rotCos[16], g_rotSin[16];
// Temporal (--ssao_temporal): per-frame offset into the 16-entry rotation
// table so successive frames sample different directions and the history
// blend converges toward the full-quality field. 0 when temporal is off —
// the static image stays bit-stable for gates.
int g_ssaoRotPhase = 0;
// Ping-pong low-res history (blended AO + its view-z) and the camera the
// history was rendered from. Written serially between the tile waves.
static std::vector<float> g_aoHistBuf[2], g_aoHistZBuf[2];
static int    g_histIdx   = 0;
static bool   g_histValid = false;
static int    g_histW = 0, g_histH = 0;
static Matrix g_histMat;
static Vector g_histPos;
void buildRot() {
	static bool done = false;
	if (done) return;                 // main-thread only (called before dispatch)
	for (int i = 0; i < 16; ++i) {
		float f = float(i) * 0.6180339887f; f -= floorf(f);   // golden-ratio low-discrepancy
		float a = f * 6.2831853f;
		g_rotCos[i] = cosf(a); g_rotSin[i] = sinf(a);
	}
	done = true;
}

// GTAO slice azimuth table — the per-cell cosf/sinf that WEREN'T per-cell.
// The slice direction is phi = (s + jit) * (PI/slices), and `jit` is
// g_rotCos[ri]*0.5+0.5 with ri drawn from the SAME 16-entry 4x4 tiling table
// above. So phi takes only slices*16 distinct values in the whole frame, yet
// the loop evaluated cosf+sinf per (cell x slice): 2 slices x 1.28 M cells =
// 5.1 M libm calls a frame at full res, every one of them a repeat of one of 32.
// Built once per frame on the main thread from the identical expression, so the
// float fed to cosf/sinf is bit-for-bit the one the loop used. Sized for the
// flag's own clamps (slices <= 8, 16 rotations).
float g_sliceCos[8][16], g_sliceSin[8][16];
void buildSliceTrig(int slices) {
	const float kPI = 3.14159265f;
	for (int s = 0; s < slices; ++s)
		for (int ri = 0; ri < 16; ++ri) {
			const float jit = g_rotCos[ri] * 0.5f + 0.5f;
			const float phi = (float(s) + jit) * (kPI / float(slices));
			g_sliceCos[s][ri] = cosf(phi);
			g_sliceSin[s][ri] = sinf(phi);
		}
}

// Fast acos (GTAO, Eberly fit), ~0.18° max error — cheap vs std::acos in the
// per-sample horizon loop. Input clamped to [-1,1] by the caller's dot/rsqrt.
inline float gtaoAcos(float x) {
	if (x < -1.0f) x = -1.0f; else if (x > 1.0f) x = 1.0f;
	const float ax = fabsf(x);
	const float r = (-0.156583f * ax + 1.57079633f) * sqrtf(1.0f - ax);
	return x >= 0.0f ? r : 3.14159265f - r;
}

// 8-wide gtaoAcos — same Eberly fit, exact same value per lane as the scalar
// (so the SIMD GTAO path matches the scalar reference). x clamped to [-1,1].
inline __m256 gtaoAcos_x8(__m256 x) {
	const __m256 sgn = _mm256_set1_ps(-0.0f);
	const __m256 one = _mm256_set1_ps(1.0f);
	__m256 ax = _mm256_min_ps(_mm256_andnot_ps(sgn, x), one);           // |x| clamped
	__m256 r  = _mm256_mul_ps(
		_mm256_fmadd_ps(_mm256_set1_ps(-0.156583f), ax, _mm256_set1_ps(1.57079633f)),
		_mm256_sqrt_ps(_mm256_sub_ps(one, ax)));
	__m256 neg = _mm256_cmp_ps(x, _mm256_setzero_ps(), _CMP_LT_OQ);
	return _mm256_blendv_ps(r, _mm256_sub_ps(_mm256_set1_ps(3.14159265f), r), neg);
}

// Low-res scratch (sized lowW*lowH). aoZ holds the representative view-space Z
// per cell (< 0 == sky / no surface).
std::vector<float> g_aoRaw, g_aoBlur, g_aoZ;

// --ssao_dump (DIAGNOSTIC, default off). Full-res plane holding the AO
// MULTIPLIER the apply pass used, so the CPU's AO field can be differenced
// against the GPU arm's (GpuBench --ssao_dump writes the identical file). Empty
// unless the flag is on; see the flag's help for the instrument-state note.
std::vector<float> g_aoDumpPlane, g_aoDumpZ, g_aoDumpN;

// noinline so the writer never inlines into the apply's tile lambda and so the
// dump is a leaf the profile can attribute; called once per frame, off the hot
// path, after every tile has drained.
FDS_NOINLINE
void WriteAoDump(const float* plane, const float* zplane, const float* nplane,
                 int w, int h) {
	const char* env = std::getenv("FDS_SSAO_DUMP_PATH");
	const char* path = (env && *env) ? env : "/tmp/fds_ssao_ao.f32";
	FILE* f = fopen(path, "wb");
	if (!f) { fprintf(stderr, "[ssao-dump] cannot open %s\n", path); return; }
	const size_t n = size_t(w) * size_t(h);
	const int32_t hdr[2] = { int32_t(w), int32_t(h) };
	fwrite("AOF3", 1, 4, f);
	fwrite(hdr, sizeof(int32_t), 2, f);
	fwrite(plane, sizeof(float), n, f);
	fwrite(zplane, sizeof(float), n, f);
	fwrite(nplane, sizeof(float), n * 3, f);          // nx, ny, nz interleaved
	fclose(f);
	double sum = 0.0; size_t occ = 0, cov = 0;
	for (size_t i = 0; i < n; ++i)
		if (zplane[i] >= 0.0f) { ++cov; sum += plane[i]; if (plane[i] < 0.9f) ++occ; }
	fprintf(stderr, "[ssao-dump] wrote %s (%dx%d f32)  covered=%.1f%%  "
	        "meanAO(covered)=%.4f  ao<0.9=%.1f%%\n", path, w, h,
	        100.0 * double(cov) / double(n), cov ? sum / double(cov) : 1.0,
	        cov ? 100.0 * double(occ) / double(cov) : 0.0);
}

} // namespace

void Render_SSAO() {
	if (!fds::FeatureFlags::ssao()) return;
	if (!g_gbuffer) return;

	const int    W = (int)XRes, H = (int)YRes;
	const size_t N = size_t(W) * size_t(H);
	if (g_gbuffer->normal.size() < N) return;        // gbuffer not sized (forward path)

	const auto t0 = std::chrono::steady_clock::now();

	// Tunables (cached FeatureFlags reads — hot-loop safe).
	int nSamp = fds::FeatureFlags::ssao_samples(); nSamp = std::max(1, std::min(64, nSamp));
	if (fds::FeatureFlags::ssao_temporal()) nSamp = std::max(1, nSamp >> 1);
	int blurR = fds::FeatureFlags::ssao_blur();    blurR = std::max(0, std::min(8, blurR));
	int down  = fds::FeatureFlags::ssao_downscale(); down = std::max(1, std::min(4, down));
	// Temporal accumulation: halve the per-frame sampling (the history blend
	// funds the quality back) and advance the rotation phase (7 is coprime
	// with 16 so all 16 tilings cycle).
	const bool temporal = fds::FeatureFlags::ssao_temporal();
	static uint32_t sTemporalFrame = 0;
	g_ssaoRotPhase = temporal ? int(((++sTemporalFrame) * 7u) & 15u) : 0;
	// --ssao_radius_zfloor (DEFAULT 48 since 2026-08-25, Gil-Ad's order: "the
	// moire was fixed - we need to pin the per-scene radius"): raise the
	// EFFECTIVE AO radius to a floor proportional to the ZPage16 depth QUANTUM,
	// 1/g_zscale.  --ssao_radius_zfloor=0 turns it off and is the exact
	// pre-flip arm.
	// The AO radius is authored in view units but the depth it integrates is a
	// 16-bit LINEAR encoding of [0, FZP*1.1], so the radius's real resolving
	// power is radius/quantum -- and that ratio is per-SCENE, not per-flag.
	// Below ~20 quanta the horizon march reads a depth STAIRCASE instead of a
	// surface and the AO field turns to moire. This floor is what makes
	// --ssao_radius mean the same thing in every scene, and it is what PINS the
	// per-scene radius; at k=48 and --ssao_radius=4.0 the pin is
	//   greets   FZP   150  quantum 0.00252757  1582.6 quanta ->  4.000000 INERT
	//   crash          2000          0.03370098   118.7       ->  4.000000 INERT
	//   fountain       5000          0.08425245    47.5       ->  4.044117
	//   city           7500          0.12637867    31.6       ->  6.066176
	//   chase         50000          0.84252453     4.75      -> 40.441177
	// so it is byte-null in greets and crash BY CONSTRUCTION. It adds no
	// samples (the march step count is unchanged, only its world extent), and
	// re-measured at the flip -- chase t=1105, 1920x1080, --ssao_downscale=2,
	// 14 runs per arm INTERLEAVED -- the two arms are INDISTINGUISHABLE:
	// k=0 min 4.15 / median 4.25 / mean 4.264 ms, k=48 min 4.07 / median 4.26 /
	// mean 4.238; delta min -0.08, median +0.01, mean -0.03, sign flipping
	// between statistics. Not "free" as a claim -- "below what 14x14 can
	// resolve". The hunt's earlier +0.17 ms min figure does not reproduce.
	// Do NOT time this pass from an --ssao_dump run: the dump forces the scalar
	// apply loop and inflates it to 13.7-16.0 ms, ~3.5x the real cost.
	float radius         = fds::FeatureFlags::ssao_radius();
	{
		const float zq = (g_zscale != 0.0f) ? 1.0f / g_zscale : 0.0f;
		const float k  = fds::FeatureFlags::ssao_radius_zfloor();
		if (k > 0.0f && zq > 0.0f && k * zq > radius) radius = k * zq;
	}
	const float strength = fds::FeatureFlags::ssao_strength();
	const float bias     = fds::FeatureFlags::ssao_bias();
	const float power    = fds::FeatureFlags::ssao_power();
	const bool  dbg      = fds::FeatureFlags::ssao_debug();
	// --ssao_dump: materialise the applied full-res AO so it can be differenced
	// against the GPU arm's. Pre-filled with 1.0 because the apply SKIPS sky
	// (`continue`), and 1.0 is what "skipped" means as a multiplier.
	const bool  dumpAo   = fds::FeatureFlags::ssao_dump();
	if (dumpAo) {
		g_aoDumpPlane.assign(N, 1.0f);
		g_aoDumpZ.assign(N, -1.0f);
		g_aoDumpN.assign(N * 3, 0.0f);
	}
	float* aoDump = dumpAo ? g_aoDumpPlane.data() : nullptr;
	float* aoDumpZ = dumpAo ? g_aoDumpZ.data() : nullptr;
	float* aoDumpN = dumpAo ? g_aoDumpN.data() : nullptr;

	buildKernel(nSamp);
	buildRot();

	const int    lowW = (W + down - 1) / down;
	const int    lowH = (H + down - 1) / down;
	const size_t lowN = size_t(lowW) * size_t(lowH);
	if (g_aoRaw.size()  < lowN) g_aoRaw.resize(lowN);
	if (g_aoBlur.size() < lowN) g_aoBlur.resize(lowN);
	if (g_aoZ.size()    < lowN) g_aoZ.resize(lowN);

	const float invZScale = (g_zscale != 0.0f) ? 1.0f / g_zscale : 1.0f;
	const float invFOVX = 1.0f / FOVX, invFOVY = 1.0f / FOVY;
	const float fovX = FOVX, fovY = FOVY;
	const float cx = CntrEX, cy = CntrEY;
	const word*  zEnc = ZPage16;
	const meka::u32* nrm = g_gbuffer->normal.data();
	const float* kx = g_kx.data(); const float* ky = g_ky.data(); const float* kz = g_kz.data();
	float* aoRaw  = g_aoRaw.data();
	float* aoBlur = g_aoBlur.data();
	float* aoZ    = g_aoZ.data();
	const int half = down >> 1;

	// WHICH BUFFER CARRIES THIS FRAME'S RADIANCE — the SSAO apply must modulate
	// that one, and only that one.
	//
	// HDR: AO multiplies linear radiance in g_hdrBuf (correct + survives the
	// tonemap). Gate on Hdr_WritableFor (buffer sized for THIS view) not
	// g_hdrActive — at our call site the kernel has written opaque radiance but
	// activation runs later. LDR: multiply VPage in place.
	//
	// ...but "sized for this view" is NOT "holds radiance". On a PreferOuterVec
	// scene (city / fountain / crash) the OUTER-VEC lighting kernel stores 8-bit
	// VPage ONLY and leaves the coverage lane 0 — its pack IS the HDR transport,
	// lifted afterwards by the froxel composite (`h[3] > 0 ? h : VPage`) or by
	// Hdr_ActivateNoFog. Between the kernel and that lift g_hdrBuf is sized and
	// CLEARED. Taking the HDR arm there multiplied a buffer of zeros and the lift
	// then seeded it from the UN-OCCLUDED VPage: measured at city t=1961 and
	// fountain t=2500, `--deferred --hdr --hdr-linear --ssao --ssao-gtao` was
	// byte-identical to `--no-ssao`, to `--ssao_strength=8` and to
	// `--ssao_radius=200` while the pass still cost ~5 ms
	// (docs/OPTIMIZATION_BACKLOG.md 2026-08-25b). Same class as the wave-2 fill
	// kernel's "⚠ WAVE-1 TRANSPORT MUST MATCH" warning, one call site further on.
	// --no-ssao_hdr_transport restores the old (broken) predicate exactly.
	const bool kernelHdr = Deferred_KernelWritesHdrRadiance()
	                       || !fds::FeatureFlags::ssao_hdr_transport();
	const bool useHdr = fds::FeatureFlags::hdr() && fds::Hdr_WritableFor(W, H) && kernelHdr;
	fds::hdrf* hbuf = useHdr ? fds::g_hdrBuf.data() : nullptr;
	dword* out  = reinterpret_cast<dword*>(VPage);

	constexpr int numTilesX = 12, numTilesY = 8;

	const bool gtao = fds::FeatureFlags::ssao_gtao();

	// ── Pass 1: compute the AO field on the low-res grid ───────────────────
	// Two interchangeable producers writing the same aoRaw[lo] (AO 0..1) +
	// aoZ[lo] (view-Z, <0 = sky); the denoise + apply downstream don't care
	// which ran. GTAO path = horizon/bitmask (--ssao_gtao); else the hemisphere
	// point-sampler. Both use the 4×4-tiling rotation so the matched box denoise
	// cancels their per-cell noise.
	if (gtao) {
		// GTAO + Visibility Bitmask (Therrien & Levesque 2023,
		// cdrinmatane.github.io/posts/ssaovb-code). Per slice: march a screen
		// direction (both ways), turn each occluder sample into a [front,back]
		// horizon-angle range (back = front offset by `thickness` along −V, so
		// light passes behind thin occluders), set those sectors in a 32-bit
		// mask; AO = 1 − occludedSectors/32, averaged over slices. Horizon
		// integration occludes only by the occluder's true angular extent, so it
		// has none of the hemisphere sampler's over-occlusion halo / bright rim.
		// Scalar (opt-in); low-res grid keeps it cheap. SECTOR_COUNT = 32.
		int slices = fds::FeatureFlags::ssao_gtao_slices(); slices = std::max(1, std::min(8, slices));
		int steps  = fds::FeatureFlags::ssao_gtao_steps();  steps  = std::max(1, std::min(16, steps));
		if (temporal) steps = std::max(1, steps >> 1);
		const float thickness = fds::FeatureFlags::ssao_gtao_thickness();
		const float r2max = radius * radius;
		const float kPI = 3.14159265f, kHalfPI = 1.57079633f;
		// SECTOR SCALE — the horizon angles are consumed ONLY as h*32 (32-sector
		// visibility bitmask), and h itself is (halfPI - nAng)/PI - sgn*a/PI. So
		// the whole (divide by PI, add the per-slice offset, multiply by 32) chain
		// collapses into one FNMADD against a per-(slice,direction) constant:
		//   S = kang - a*cSgn,  kang = (halfPI-nAng)*32/PI,  cSgn = sgn*32/PI.
		// That removes TWO full-precision vector divides per sample from the
		// innermost loop (2 x 8 lanes x 16 samples x 1.28 M cells at his arm's
		// full-res SSAO), which on this core are the only non-pipelined FP ops in
		// the chain besides the two sqrts inside gtaoAcos.
		const float kSec = 32.0f / kPI;
		buildSliceTrig(slices);          // main thread, before the dispatch
		const bool noSimd = std::getenv("FDS_SSAO_NOSIMD") != nullptr;   // A/B validation escape
		const int tsx = (lowW + numTilesX - 1) / numTilesX;
		const int tsy = (lowH + numTilesY - 1) / numTilesY;
		dispatchIndexed(numTilesX * numTilesY, nullptr, [=](int _t) {
			const int tj = _t / numTilesX, ti = _t - tj * numTilesX;
			const int ly1 = tsy * tj, ly2 = std::min(ly1 + tsy, lowH);
			const int lx1 = tsx * ti, lx2 = std::min(lx1 + tsx, lowW);
			{
					// Scalar per-cell GTAO (reference + 8-wide remainder tail).
					auto gtaoCell = [&](int lx, int ly) {
						const size_t lo = size_t(ly) * size_t(lowW) + size_t(lx);
						const int px = std::min(lx * down + half, W - 1);
						const int py = std::min(ly * down + half, H - 1);
						const size_t i = size_t(py) * size_t(W) + size_t(px);
						const word ze = zEnc[i];
						if (ze == 0) { aoRaw[lo] = 1.0f; aoZ[lo] = -1.0f; return; }
						const float z = float(0xFF80 - ze) * invZScale;
						aoZ[lo] = z;
						const float Px = (float(px) - cx) * z * invFOVX;
						const float Py = (cy - float(py)) * z * invFOVY;
						const float Pz = z;
						float Nx, Ny, Nz; meka::oct_decode_u32(nrm[i], Nx, Ny, Nz);
						if (Nx*Px + Ny*Py + Nz*Pz > 0.0f) { Nx=-Nx; Ny=-Ny; Nz=-Nz; }
						const float vinv = fast_rsqrt(Px*Px + Py*Py + Pz*Pz + 1e-12f);
						const float Vx = -Px*vinv, Vy = -Py*vinv, Vz = -Pz*vinv;
						float srad = radius * fovX / z;
						if (srad < 2.0f) srad = 2.0f; else if (srad > 256.0f) srad = 256.0f;
						const int ri = ((ly & 3) * 4 + (lx & 3) + g_ssaoRotPhase) & 15;
						const float jit = g_rotCos[ri] * 0.5f + 0.5f;
						float vis = 0.0f;
						for (int s = 0; s < slices; ++s) {
							const float dcx = g_sliceCos[s][ri], dsy = g_sliceSin[s][ri];
							const float d3x = dcx, d3y = -dsy;
							float snx = d3y*Vz, sny = -d3x*Vz, snz = d3x*Vy - d3y*Vx;
							const float snl = fast_rsqrt(snx*snx + sny*sny + snz*snz + 1e-12f);
							snx*=snl; sny*=snl; snz*=snl;
							const float ndotsn = Nx*snx + Ny*sny + Nz*snz;
							const float pnx = Nx - snx*ndotsn, pny = Ny - sny*ndotsn, pnz = Nz - snz*ndotsn;
							const float tx = sny*Vz - snz*Vy, ty = snz*Vx - snx*Vz, tz = snx*Vy - sny*Vx;
							const float nAng = atan2_approx(pnx*tx + pny*ty + pnz*tz, pnx*Vx + pny*Vy + pnz*Vz);
							// SECTOR UNITS (see the SIMD twin): the horizon angle is
							// only ever consumed as h*32, so carry 32/PI into the
							// per-slice constant and the per-sample work becomes one
							// FNMADD instead of a divide by PI plus a multiply by 32.
							const float kang = (kHalfPI - nAng) * kSec;
							uint32_t mask = 0u;
							for (int sgn = -1; sgn <= 1; sgn += 2) {
								const float cSgn = float(sgn) * kSec;
								for (int j = 0; j < steps; ++j) {
									const float t = (float(j) + 0.5f + jit*0.5f) / float(steps) * srad;
									const int sx = px + int(float(sgn)*dcx*t + 0.5f);
									const int sy = py + int(float(sgn)*dsy*t + 0.5f);
									if ((unsigned)sx >= (unsigned)W || (unsigned)sy >= (unsigned)H) continue;
									const word ze2 = zEnc[size_t(sy)*size_t(W)+size_t(sx)];
									if (ze2 == 0) continue;
									const float sz = float(0xFF80 - ze2) * invZScale;
									const float dx = (float(sx)-cx)*sz*invFOVX - Px;
									const float dy = (cy-float(sy))*sz*invFOVY - Py;
									const float dz = sz - Pz;
									const float dl2 = dx*dx + dy*dy + dz*dz;
									if (dl2 > r2max || dl2 < 1e-8f) continue;
									const float dinv = fast_rsqrt(dl2);
									const float bx = dx - Vx*thickness, by = dy - Vy*thickness, bz = dz - Vz*thickness;
									const float binv = fast_rsqrt(bx*bx + by*by + bz*bz + 1e-12f);
									const float fa = gtaoAcos((dx*Vx + dy*Vy + dz*Vz) * dinv);
									const float ba = gtaoAcos((bx*Vx + by*Vy + bz*Vz) * binv);
									float h0 = kang - fa*cSgn;      // sector units, 0..32
									float h1 = kang - ba*cSgn;
									h0 = h0<0?0:(h0>32.0f?32.0f:h0); h1 = h1<0?0:(h1>32.0f?32.0f:h1);
									const float mn = h0<h1?h0:h1, mx = h0<h1?h1:h0;
									uint32_t startBit = (uint32_t)mn;
									int angBits = (int)ceilf(mx - mn);
									if (angBits > 0) {
										if (startBit > 31) startBit = 31;
										const uint32_t bf = (angBits >= 32) ? 0xFFFFFFFFu : (0xFFFFFFFFu >> (32 - angBits));
										mask |= bf << startBit;
									}
								}
							}
							vis += 1.0f - float(FDS_POPCOUNT(mask)) / 32.0f;
						}
						vis /= float(slices);
						float ao = 1.0f - (1.0f - vis) * strength;
						if (ao < 0.0f) ao = 0.0f; else if (ao > 1.0f) ao = 1.0f;
						if (power != 1.0f) ao = powf(ao, power);
						aoRaw[lo] = ao;
					};

					// 8-wide GTAO over 8 cells of a row: the per-sample arithmetic
					// (reconstruct, 2× gtaoAcos, horizon, bitmask build) is vector;
					// the depth gather and the per-slice popcount stay scalar (8×).
					// Per-lane slice setup (trig) is scalar but only slices×8 per
					// group. Matches the scalar gtaoCell within rsqrt/cvt rounding.
					auto gtaoRow8 = [&](int lx, int ly) {
						const int py = std::min(ly * down + half, H - 1);
						alignas(32) float aPx[8], aPy[8], aPz[8], aVx[8], aVy[8], aVz[8];
						alignas(32) float aNx[8], aNy[8], aNz[8], aSr[8], aJit[8], aPxi[8];
						alignas(32) int   aPxInt[8];
						int   aRi[8];
						bool valid[8];
						for (int k = 0; k < 8; ++k) {
							const int cx_ = lx + k;
							const int px = std::min(cx_ * down + half, W - 1);
							const size_t lo = size_t(ly)*size_t(lowW) + size_t(cx_);
							const size_t i = size_t(py)*size_t(W) + size_t(px);
							const word ze = zEnc[i];
							aPxInt[k] = px; aPxi[k] = float(px);
							if (ze == 0) { valid[k]=false; aoRaw[lo]=1.0f; aoZ[lo]=-1.0f;
							               aPx[k]=aPy[k]=aPz[k]=0; aVx[k]=aVy[k]=0; aVz[k]=1;
							               aNx[k]=aNy[k]=0; aNz[k]=1; aSr[k]=2; aJit[k]=0; aRi[k]=0; continue; }
							const float z = float(0xFF80 - ze) * invZScale;
							aoZ[lo] = z;
							const float Px=(float(px)-cx)*z*invFOVX, Py=(cy-float(py))*z*invFOVY, Pz=z;
							float Nx,Ny,Nz; meka::oct_decode_u32(nrm[i],Nx,Ny,Nz);
							if (Nx*Px+Ny*Py+Nz*Pz>0.0f){Nx=-Nx;Ny=-Ny;Nz=-Nz;}
							const float vinv=fast_rsqrt(Px*Px+Py*Py+Pz*Pz+1e-12f);
							float sr=radius*fovX/z; if(sr<2.0f)sr=2.0f; else if(sr>256.0f)sr=256.0f;
							const int ri=((ly&3)*4+(cx_&3)+g_ssaoRotPhase)&15;
							aPx[k]=Px;aPy[k]=Py;aPz[k]=Pz; aVx[k]=-Px*vinv;aVy[k]=-Py*vinv;aVz[k]=-Pz*vinv;
							aNx[k]=Nx;aNy[k]=Ny;aNz[k]=Nz; aSr[k]=sr; aJit[k]=g_rotCos[ri]*0.5f+0.5f;
							aRi[k]=ri;
							valid[k]=true;
						}
						const __m256 PxV=_mm256_load_ps(aPx),PyV=_mm256_load_ps(aPy),PzV=_mm256_load_ps(aPz);
						const __m256 VxV=_mm256_load_ps(aVx),VyV=_mm256_load_ps(aVy),VzV=_mm256_load_ps(aVz);
						const __m256 pxiV=_mm256_load_ps(aPxi), pyV=_mm256_set1_ps(float(py));
						const __m256 sradV=_mm256_load_ps(aSr);
						alignas(32) float visAcc[8] = {0,0,0,0,0,0,0,0};
						const __m256 vHalf=_mm256_set1_ps(0.5f), vZero=_mm256_setzero_ps();
						const __m256 vThick=_mm256_set1_ps(thickness), vR2=_mm256_set1_ps(r2max), vEps=_mm256_set1_ps(1e-8f);
						const __m256 v32=_mm256_set1_ps(32.0f);
						const __m256i i32=_mm256_set1_epi32(32), iAll=_mm256_set1_epi32(-1);
						for (int s = 0; s < slices; ++s) {
							// per-lane slice setup (scalar trig; slices×8 only)
							alignas(32) float aDcx[8],aDsy[8],aNang[8];
							for (int k=0;k<8;++k){
								// phi = (s+jit)*(PI/slices) has only slices*16 values —
								// tabled once a frame (buildSliceTrig), not 5.1 M libm
								// calls. Invalid lanes take entry 0 and are discarded.
								const float dcx=g_sliceCos[s][aRi[k]],dsy=g_sliceSin[s][aRi[k]];
								aDcx[k]=dcx;aDsy[k]=dsy;
								const float d3x=dcx,d3y=-dsy;
								float snx=d3y*aVz[k],sny=-d3x*aVz[k],snz=d3x*aVy[k]-d3y*aVx[k];
								const float snl=fast_rsqrt(snx*snx+sny*sny+snz*snz+1e-12f); snx*=snl;sny*=snl;snz*=snl;
								const float nd=aNx[k]*snx+aNy[k]*sny+aNz[k]*snz;
								const float pnx=aNx[k]-snx*nd,pny=aNy[k]-sny*nd,pnz=aNz[k]-snz*nd;
								const float tx=sny*aVz[k]-snz*aVy[k],ty=snz*aVx[k]-snx*aVz[k],tz=snx*aVy[k]-sny*aVx[k];
								aNang[k]=(kHalfPI-atan2_approx(pnx*tx+pny*ty+pnz*tz,
								                               pnx*aVx[k]+pny*aVy[k]+pnz*aVz[k]))*kSec;
							}
							const __m256 dcxV=_mm256_load_ps(aDcx), dsyV=_mm256_load_ps(aDsy), kangV=_mm256_load_ps(aNang);
							const __m256 jitV=_mm256_load_ps(aJit);
							const __m256 srStep=_mm256_mul_ps(sradV,_mm256_set1_ps(1.0f/float(steps)));
							__m256i maskV=_mm256_setzero_si256();
							for (int sgn=-1; sgn<=1; sgn+=2) {
								const __m256 sgnV=_mm256_set1_ps(float(sgn));
								const __m256 cSgnV=_mm256_set1_ps(float(sgn)*kSec);
								for (int j=0;j<steps;++j) {
									// t = ((j+0.5)+jit*0.5) * srad/steps
									__m256 tV=_mm256_mul_ps(_mm256_add_ps(_mm256_set1_ps(float(j)+0.5f),
									           _mm256_mul_ps(jitV,vHalf)), srStep);
									// sample int pos: px + int(sgn*dcx*t + 0.5)
									__m256 offx=_mm256_mul_ps(_mm256_mul_ps(sgnV,dcxV),tV);
									__m256 offy=_mm256_mul_ps(_mm256_mul_ps(sgnV,dsyV),tV);
									__m256i sxi=_mm256_add_epi32(_mm256_load_si256((const __m256i*)aPxInt),
									              _mm256_cvttps_epi32(_mm256_add_ps(offx,vHalf)));
									__m256i syi=_mm256_add_epi32(_mm256_set1_epi32(py),
									              _mm256_cvttps_epi32(_mm256_add_ps(offy,vHalf)));
									alignas(32) int sxA[8], syA[8]; alignas(32) float szA[8];
									_mm256_store_si256((__m256i*)sxA,sxi);
									_mm256_store_si256((__m256i*)syA,syi);
									bool any=false;
									for (int k=0;k<8;++k){
										const int sx=sxA[k], sy=syA[k];
										if ((unsigned)sx>=(unsigned)W||(unsigned)sy>=(unsigned)H){ szA[k]=0.0f; continue; }
										const word z2=zEnc[size_t(sy)*size_t(W)+size_t(sx)];
										if (!z2){ szA[k]=0.0f; continue; }
										szA[k]=float(0xFF80-z2)*invZScale; any=true;
									}
									if (!any) continue;
									const __m256 szV=_mm256_load_ps(szA);
									// gather-valid = sz>0 (sky/offscreen lanes set sz=0); proper all-ones mask
									__m256 gmask=_mm256_cmp_ps(szV,vZero,_CMP_GT_OQ);
									const __m256 sxf=_mm256_cvtepi32_ps(sxi), syf=_mm256_cvtepi32_ps(syi);
									const __m256 dx=_mm256_sub_ps(_mm256_mul_ps(_mm256_mul_ps(_mm256_sub_ps(sxf,_mm256_set1_ps(cx)),szV),_mm256_set1_ps(invFOVX)),PxV);
									const __m256 dy=_mm256_sub_ps(_mm256_mul_ps(_mm256_mul_ps(_mm256_sub_ps(_mm256_set1_ps(cy),syf),szV),_mm256_set1_ps(invFOVY)),PyV);
									const __m256 dz=_mm256_sub_ps(szV,PzV);
									const __m256 dl2=_mm256_fmadd_ps(dx,dx,_mm256_fmadd_ps(dy,dy,_mm256_mul_ps(dz,dz)));
									// range valid: dl2<=r2max && dl2>=eps  (AND the gather-valid mask)
									__m256 rok=_mm256_and_ps(_mm256_cmp_ps(dl2,vR2,_CMP_LE_OQ),_mm256_cmp_ps(dl2,vEps,_CMP_GE_OQ));
									gmask=_mm256_and_ps(gmask,rok);
									const __m256 dinv=_mm256_rsqrt_ps(dl2);
									const __m256 bx=_mm256_fnmadd_ps(VxV,vThick,dx), by=_mm256_fnmadd_ps(VyV,vThick,dy), bz=_mm256_fnmadd_ps(VzV,vThick,dz);
									const __m256 binv=_mm256_rsqrt_ps(_mm256_fmadd_ps(bx,bx,_mm256_fmadd_ps(by,by,_mm256_fmadd_ps(bz,bz,_mm256_set1_ps(1e-12f)))));
									const __m256 fdot=_mm256_mul_ps(_mm256_fmadd_ps(dx,VxV,_mm256_fmadd_ps(dy,VyV,_mm256_mul_ps(dz,VzV))),dinv);
									const __m256 bdot=_mm256_mul_ps(_mm256_fmadd_ps(bx,VxV,_mm256_fmadd_ps(by,VyV,_mm256_mul_ps(bz,VzV))),binv);
									const __m256 fa=gtaoAcos_x8(fdot), ba=gtaoAcos_x8(bdot);
									// h*32 = kang - a*(sgn*32/PI), one FNMADD each: the
									// two _mm256_div_ps by PI and the two multiplies by
									// 32 are folded into the per-slice constant above.
									__m256 h0=_mm256_fnmadd_ps(fa,cSgnV,kangV);
									__m256 h1=_mm256_fnmadd_ps(ba,cSgnV,kangV);
									h0=_mm256_min_ps(_mm256_max_ps(h0,vZero),v32); h1=_mm256_min_ps(_mm256_max_ps(h1,vZero),v32);
									const __m256 mn=_mm256_min_ps(h0,h1), mx=_mm256_max_ps(h0,h1);
									__m256i startB=_mm256_min_epi32(_mm256_cvttps_epi32(mn),_mm256_set1_epi32(31));
									__m256i angB=_mm256_min_epi32(_mm256_max_epi32(_mm256_cvttps_epi32(_mm256_ceil_ps(_mm256_sub_ps(mx,mn))),_mm256_setzero_si256()),i32);
									// base = 0xFFFFFFFF >> (32-angB);  contrib = base << startB
									__m256i base=_mm256_srlv_epi32(iAll,_mm256_sub_epi32(i32,angB));
									__m256i contrib=_mm256_sllv_epi32(base,startB);
									contrib=_mm256_and_si256(contrib,_mm256_castps_si256(gmask));
									maskV=_mm256_or_si256(maskV,contrib);
								}
							}
							alignas(32) uint32_t mk[8]; _mm256_store_si256((__m256i*)mk,maskV);
							for (int k=0;k<8;++k) visAcc[k]+=1.0f-float(FDS_POPCOUNT(mk[k]))/32.0f;
						}
						const float invS=1.0f/float(slices);
						for (int k=0;k<8;++k){
							if (!valid[k]) continue;
							float ao=1.0f-(1.0f-visAcc[k]*invS)*strength;
							if (ao<0)ao=0; else if(ao>1)ao=1;
							if (power!=1.0f) ao=powf(ao,power);
							aoRaw[size_t(ly)*size_t(lowW)+size_t(lx+k)]=ao;
						}
					};

					// THE TAIL WAS 5 % OF THE CELLS AND A QUARTER OF THE PASS.
					// The tile grid is 12 x 8 over the low-res plane, so at his
					// 1512x848 / --ssao_downscale=1 a tile row is 126 cells: 15
					// vector groups and then SIX cells down the scalar reference,
					// which costs ~8x per cell. Rather than widen the grid (that
					// moves the tile boundaries and the load balance), the tail is
					// covered by ONE MORE VECTOR GROUP anchored at lx2-8 — it
					// recomputes up to 7 cells that the previous group already did,
					// writing them the identical value from the identical code, and
					// the overlap never leaves this tile's own [lx1,lx2) so no other
					// worker's cells are touched. Cells that used to take the scalar
					// path now take the vector one, which is a ROUNDING difference
					// (rsqrt/cvt), not a different algorithm — quantified in the
					// commit. Tiles narrower than a vector (or --ssao_downscale=4 on
					// a small view) still fall back to the scalar reference.
					for (int ly = ly1; ly < ly2; ++ly) {
						int lx = lx1;
						if (!noSimd) {
							for (; lx + 8 <= lx2; lx += 8) gtaoRow8(lx, ly);
							if (lx < lx2 && lx2 - lx1 >= 8) { gtaoRow8(lx2 - 8, ly); lx = lx2; }
						}
						for (; lx < lx2; ++lx) gtaoCell(lx, ly);
					}
					renderns::tileDone.release();
				}
		});
		for (int n = numTilesX * numTilesY, k = 0; k < n; ++k) renderns::tileDone.acquire();
	} else
	{
		const int tsx = (lowW + numTilesX - 1) / numTilesX;
		const int tsy = (lowH + numTilesY - 1) / numTilesY;
		const float invN = 1.0f / float(nSamp);
		dispatchIndexed(numTilesX * numTilesY, nullptr, [=](int _t) {
			const int tj = _t / numTilesX, ti = _t - tj * numTilesX;
			const int ly1 = tsy * tj, ly2 = std::min(ly1 + tsy, lowH);
			const int lx1 = tsx * ti, lx2 = std::min(lx1 + tsx, lowW);
			{
					// Per-pixel setup: writes aoZ; returns false for sky. ca/sa is
					// the per-cell 4×4-tiling kernel rotation (see g_rotCos/Sin).
					struct Setup { float x,y,z, Tx,Ty,Tz, Bx,By,Bz, nx,ny,nz; };
					auto setup = [&](int px, int py, float ca, float sa, size_t lo, Setup& S) -> bool {
						const size_t i = size_t(py) * size_t(W) + size_t(px);
						const word ze = zEnc[i];
						if (ze == 0) { aoRaw[lo] = 1.0f; aoZ[lo] = -1.0f; return false; }
						const float z = float(0xFF80 - ze) * invZScale;
						S.z = z;
						S.x = (float(px) - cx) * z * invFOVX;
						S.y = (cy - float(py)) * z * invFOVY;
						aoZ[lo] = z;
						float nx, ny, nz;
						meka::oct_decode_u32(nrm[i], nx, ny, nz);
						if (nx*S.x + ny*S.y + nz*z > 0.0f) { nx = -nx; ny = -ny; nz = -nz; }
						float hx = 0.0f, hy = 0.0f, hz = 1.0f;
						if (fabsf(nz) > 0.999f) { hx = 1.0f; hz = 0.0f; }
						float t0x = hy*nz - hz*ny, t0y = hz*nx - hx*nz, t0z = hx*ny - hy*nx;
						float tl = fast_rsqrt(t0x*t0x + t0y*t0y + t0z*t0z + 1e-12f);  // approx 1/sqrt, no divide
						t0x *= tl; t0y *= tl; t0z *= tl;
						float b0x = ny*t0z - nz*t0y, b0y = nz*t0x - nx*t0z, b0z = nx*t0y - ny*t0x;
						S.Tx = t0x*ca + b0x*sa; S.Ty = t0y*ca + b0y*sa; S.Tz = t0z*ca + b0z*sa;
						S.Bx = -t0x*sa + b0x*ca; S.By = -t0y*sa + b0y*ca; S.Bz = -t0z*sa + b0z*ca;
						S.nx = nx; S.ny = ny; S.nz = nz;
						return true;
					};
					auto finalize = [&](float occ) -> float {
						float ao = 1.0f - (occ * invN) * strength;
						if (ao < 0.0f) ao = 0.0f; else if (ao > 1.0f) ao = 1.0f;
						if (power != 1.0f) ao = powf(ao, power);
						return ao;
					};

					const __m256 vcx = _mm256_set1_ps(cx),   vcy = _mm256_set1_ps(cy);
					const __m256 vfx = _mm256_set1_ps(fovX),  vfy = _mm256_set1_ps(fovY);
					const __m256 vrad = _mm256_set1_ps(radius), vbias = _mm256_set1_ps(bias);
					const __m256 vEps = _mm256_set1_ps(1e-4f), vOne = _mm256_set1_ps(1.0f);
					const __m256 vSign = _mm256_set1_ps(-0.0f);

					for (int ly = ly1; ly < ly2; ++ly) {
						const int py = std::min(ly * down + half, H - 1);
						const size_t rowLo = size_t(ly) * size_t(lowW);
						int lx = lx1;

						// ---- SIMD: 8 low-res pixels at a time ----
						for (; lx + 8 <= lx2; lx += 8) {
							float aX[8], aY[8], aZ[8], aTx[8], aTy[8], aTz[8];
							float aBx[8], aBy[8], aBz[8], aNx[8], aNy[8], aNz[8];
							bool  valid[8];
							for (int k = 0; k < 8; ++k) {
								Setup S{};
								const int cellX = lx + k;
								const int px = std::min(cellX * down + half, W - 1);
								const int ri = ((ly & 3) * 4 + (cellX & 3) + g_ssaoRotPhase) & 15;   // 4×4 tiling rotation
								valid[k] = setup(px, py, g_rotCos[ri], g_rotSin[ri], rowLo + size_t(cellX), S);
								aX[k]=S.x; aY[k]=S.y; aZ[k]=S.z;
								aTx[k]=S.Tx; aTy[k]=S.Ty; aTz[k]=S.Tz;
								aBx[k]=S.Bx; aBy[k]=S.By; aBz[k]=S.Bz;
								aNx[k]=S.nx; aNy[k]=S.ny; aNz[k]=S.nz;
							}
							const __m256 X=_mm256_loadu_ps(aX), Y=_mm256_loadu_ps(aY), Z=_mm256_loadu_ps(aZ);
							const __m256 Tx=_mm256_loadu_ps(aTx), Ty=_mm256_loadu_ps(aTy), Tz=_mm256_loadu_ps(aTz);
							const __m256 Bx=_mm256_loadu_ps(aBx), By=_mm256_loadu_ps(aBy), Bz=_mm256_loadu_ps(aBz);
							const __m256 Nx=_mm256_loadu_ps(aNx), Ny=_mm256_loadu_ps(aNy), Nz=_mm256_loadu_ps(aNz);
							__m256 occ = _mm256_setzero_ps();

							for (int s = 0; s < nSamp; ++s) {
								const __m256 ksx=_mm256_set1_ps(kx[s]), ksy=_mm256_set1_ps(ky[s]), ksz=_mm256_set1_ps(kz[s]);
								__m256 ox = _mm256_mul_ps(_mm256_fmadd_ps(Tx,ksx,_mm256_fmadd_ps(Bx,ksy,_mm256_mul_ps(Nx,ksz))), vrad);
								__m256 oy = _mm256_mul_ps(_mm256_fmadd_ps(Ty,ksx,_mm256_fmadd_ps(By,ksy,_mm256_mul_ps(Ny,ksz))), vrad);
								__m256 oz = _mm256_mul_ps(_mm256_fmadd_ps(Tz,ksx,_mm256_fmadd_ps(Bz,ksy,_mm256_mul_ps(Nz,ksz))), vrad);
								__m256 sX=_mm256_add_ps(X,ox), sY=_mm256_add_ps(Y,oy), sZ=_mm256_add_ps(Z,oz);

								// invSZ = raw rcp(sZ). The ~12-bit error is ~0.25px of
								// tap drift at 1080p, which rounds to the same depth
								// pixel — a Newton-Raphson refine was measured
								// byte-~identical (max 1-3/255) and free-but-pointless,
								// so it's dropped.
								__m256 invSZ = _mm256_rcp_ps(sZ);
								__m256 fxp = _mm256_fmadd_ps(_mm256_mul_ps(sX, invSZ), vfx, vcx);
								__m256 fyp = _mm256_fnmadd_ps(_mm256_mul_ps(sY, invSZ), vfy, vcy);
								__m256i ix = _mm256_cvtps_epi32(fxp);
								__m256i iy = _mm256_cvtps_epi32(fyp);
								alignas(32) int sxA[8], syA[8]; float sZa[8], sceneZf[8];
								_mm256_store_si256((__m256i*)sxA, ix);
								_mm256_store_si256((__m256i*)syA, iy);
								_mm256_storeu_ps(sZa, sZ);
								for (int k = 0; k < 8; ++k) {                 // scalar depth gather
									float val = 1e30f;
									if (sZa[k] > 1e-3f) {
										const int sx = sxA[k], sy = syA[k];
										if ((unsigned)sx < (unsigned)W && (unsigned)sy < (unsigned)H) {
											const word z2 = zEnc[size_t(sy)*size_t(W)+size_t(sx)];
											if (z2) val = float(0xFF80 - z2) * invZScale;
										}
									}
									sceneZf[k] = val;
								}
								__m256 sceneZ = _mm256_loadu_ps(sceneZf);
								__m256 occl = _mm256_cmp_ps(sceneZ, _mm256_sub_ps(sZ, vbias), _CMP_LE_OQ);
								__m256 dz  = _mm256_andnot_ps(vSign, _mm256_sub_ps(Z, sceneZ));   // |z - sceneZ|
								__m256 rc  = _mm256_min_ps(_mm256_mul_ps(vrad, _mm256_rcp_ps(_mm256_add_ps(dz, vEps))), vOne);
								occ = _mm256_add_ps(occ, _mm256_and_ps(rc, occl));
							}

							alignas(32) float occA[8];
							_mm256_store_ps(occA, occ);
							for (int k = 0; k < 8; ++k)
								if (valid[k]) aoRaw[rowLo + size_t(lx + k)] = finalize(occA[k]);
						}

						// ---- scalar tail ----
						for (; lx < lx2; ++lx) {
							const size_t lo = rowLo + size_t(lx);
							const int px = std::min(lx * down + half, W - 1);
							const int ri = ((ly & 3) * 4 + (lx & 3) + g_ssaoRotPhase) & 15;   // 4×4 tiling rotation
							Setup S{};
							if (!setup(px, py, g_rotCos[ri], g_rotSin[ri], lo, S)) continue;
							float occ = 0.0f;
							for (int s = 0; s < nSamp; ++s) {
								const float ox = (S.Tx*kx[s] + S.Bx*ky[s] + S.nx*kz[s]) * radius;
								const float oy = (S.Ty*kx[s] + S.By*ky[s] + S.ny*kz[s]) * radius;
								const float oz = (S.Tz*kx[s] + S.Bz*ky[s] + S.nz*kz[s]) * radius;
								const float sX = S.x + ox, sY = S.y + oy, sZ = S.z + oz;
								if (sZ <= 1e-3f) continue;
								const int spx = (int)(cx + (sX / sZ) * fovX + 0.5f);
								const int spy = (int)(cy - (sY / sZ) * fovY + 0.5f);
								if ((unsigned)spx >= (unsigned)W || (unsigned)spy >= (unsigned)H) continue;
								const word ze2 = zEnc[size_t(spy)*size_t(W)+size_t(spx)];
								if (ze2 == 0) continue;
								const float sceneZ = float(0xFF80 - ze2) * invZScale;
								if (sceneZ <= sZ - bias) {
									const float dz = fabsf(S.z - sceneZ);
									float rc = radius / (dz + 1e-4f); if (rc > 1.0f) rc = 1.0f;
									occ += rc;
								}
							}
							aoRaw[lo] = finalize(occ);
						}
					}
					renderns::tileDone.release();
				}
		});
		for (int n = numTilesX * numTilesY, k = 0; k < n; ++k) renderns::tileDone.acquire();
	}
	const auto tP1 = std::chrono::steady_clock::now();

	// ── Pass 2: MATCHED 4×4 depth-aware box denoise ────────────────────────
	// Fixed 4×4 box (offsets -2..+1) = exactly the 4×4 tiling-rotation period,
	// so it averages each of the 16 rotations once → cancels the rotation
	// "hatch" on flat surfaces (a 5×5 or continuous-noise blur leaves a residual
	// — that was the floor banding). DEPTH-only weight: a geometric-normal
	// PLANE-distance weight (iq's trick) was measured and dropped — zero hatch
	// improvement here and ~2× the denoise cost. The cos^4 SHADING-normal weight
	// is also gone (it banded on the normal-mapped floor). blurR>0 enables it;
	// window is fixed at 4 (matched to the noise period), not blurR-scaled.
	const float* aoSrc = aoRaw;
	if (blurR > 0) {
		const int tsx = (lowW + numTilesX - 1) / numTilesX;
		const int tsy = (lowH + numTilesY - 1) / numTilesY;
		const float depthSig = std::max(radius, 1.0f);
		const float invDepthK = 1.0f / (4.0f * depthSig * depthSig);  // divide-free depth falloff
		dispatchIndexed(numTilesX * numTilesY, nullptr, [=](int _t) {
			const int tj = _t / numTilesX, ti = _t - tj * numTilesX;
			const int ly1 = tsy * tj, ly2 = std::min(ly1 + tsy, lowH);
			const int lx1 = tsx * ti, lx2 = std::min(lx1 + tsx, lowW);
			{
					// Scalar reference cell — the border columns (the 4x4 box's x
					// window would run off the plane) and any tile narrower than a
					// vector still take this.
					auto blurCell = [&](int lx, int ly, int by0, int by1) {
						const size_t lo = size_t(ly) * size_t(lowW) + size_t(lx);
						const float zc = aoZ[lo];
						if (zc < 0.0f) { aoBlur[lo] = aoRaw[lo]; return; }
						const int bx0 = std::max(0, lx - 2), bx1 = std::min(lowW - 1, lx + 1);
						float sum = 0.0f, wsum = 0.0f;
						for (int yy = by0; yy <= by1; ++yy) {
							const size_t r2 = size_t(yy) * size_t(lowW);
							for (int xx = bx0; xx <= bx1; ++xx) {
								const size_t o = r2 + size_t(xx);
								const float zt = aoZ[o];
								if (zt < 0.0f) continue;
								const float dz = zc - zt;
								float w = 1.0f - dz * dz * invDepthK;   // depth-only (divide-free)
								if (w <= 0.0f) continue;
								sum += aoRaw[o] * w; wsum += w;
							}
						}
						aoBlur[lo] = wsum > 1e-6f ? sum / wsum : aoRaw[lo];
					};

					// 8-wide denoise over 8 OUTPUT cells of a row. The 4x4 box is a
					// fixed stencil, so the 16 taps are 16 unaligned 8-wide loads of
					// each plane and every lane keeps the scalar's tap ORDER (yy
					// outer, xx -2..+1 inner). The scalar's two `continue`s become a
					// weight mask: a rejected tap contributes w = +0.0, and
					// `sum + aoRaw*0.0` and `wsum + 0.0` are EXACT no-ops in IEEE, so
					// this is the same sum in the same order — bit-exact, not
					// approximately equal. The whole point is the branch: the scalar
					// loop is 16 unpredictable tests per pixel over 1.28 M pixels at
					// --ssao_downscale=1, which is what his arm runs.
					const __m256 vZeroB = _mm256_setzero_ps(), vOneB = _mm256_set1_ps(1.0f);
					const __m256 vInvK  = _mm256_set1_ps(invDepthK);
					const __m256 vWEps  = _mm256_set1_ps(1e-6f);
					const int maxStart  = std::min(lx2 - 8, lowW - 9);
					for (int ly = ly1; ly < ly2; ++ly) {
						const int by0 = std::max(0, ly - 2), by1 = std::min(lowH - 1, ly + 1);  // 4×4 box
						const size_t rowLo = size_t(ly) * size_t(lowW);
						int lx = lx1;
						for (; lx < lx2 && lx < 2; ++lx) blurCell(lx, ly, by0, by1);
						for (; lx <= maxStart; lx += 8) {
							const __m256 zc  = _mm256_loadu_ps(aoZ   + rowLo + size_t(lx));
							const __m256 raw = _mm256_loadu_ps(aoRaw + rowLo + size_t(lx));
							__m256 sum = _mm256_setzero_ps(), wsum = _mm256_setzero_ps();
							for (int yy = by0; yy <= by1; ++yy) {
								const float* zr = aoZ   + size_t(yy) * size_t(lowW) + size_t(lx);
								const float* ar = aoRaw + size_t(yy) * size_t(lowW) + size_t(lx);
								for (int dx = -2; dx <= 1; ++dx) {
									const __m256 zt = _mm256_loadu_ps(zr + dx);
									const __m256 dz = _mm256_sub_ps(zc, zt);
									__m256 w = _mm256_fnmadd_ps(_mm256_mul_ps(dz, dz), vInvK, vOneB);
									const __m256 keep = _mm256_and_ps(
										_mm256_cmp_ps(zt, vZeroB, _CMP_GE_OQ),
										_mm256_cmp_ps(w,  vZeroB, _CMP_GT_OQ));
									w = _mm256_and_ps(w, keep);
									sum  = _mm256_add_ps(sum, _mm256_mul_ps(_mm256_loadu_ps(ar + dx), w));
									wsum = _mm256_add_ps(wsum, w);
								}
							}
							__m256 res = _mm256_blendv_ps(raw, _mm256_div_ps(sum, wsum),
							                              _mm256_cmp_ps(wsum, vWEps, _CMP_GT_OQ));
							res = _mm256_blendv_ps(res, raw, _mm256_cmp_ps(zc, vZeroB, _CMP_LT_OQ));
							_mm256_storeu_ps(aoBlur + rowLo + size_t(lx), res);
						}
						for (; lx < lx2; ++lx) blurCell(lx, ly, by0, by1);
					}
					renderns::tileDone.release();
				}
		});
		for (int n = numTilesX * numTilesY, k = 0; k < n; ++k) renderns::tileDone.acquire();
		aoSrc = aoBlur;
	}
	const auto tP2 = std::chrono::steady_clock::now();

	// ── Pass 2.5: temporal accumulation (--ssao_temporal) ───────────────
	// Reproject each low-res cell's view point through the camera delta into
	// last frame's blended history, depth-reject against the history's own
	// view-z, and blend. The blended field becomes both this frame's aoSrc
	// and the next frame's history. Off: bit-identical to the non-temporal
	// pipeline (phase pinned to 0, sampling un-halved).
	if (temporal) {
		if (g_histW != lowW || g_histH != lowH) {
			g_histValid = false; g_histW = lowW; g_histH = lowH;
		}
		const int wrIdx = g_histIdx ^ 1;
		auto &rdBuf = g_aoHistBuf[g_histIdx]; auto &rdZ = g_aoHistZBuf[g_histIdx];
		auto &wrBuf = g_aoHistBuf[wrIdx];     auto &wrZ = g_aoHistZBuf[wrIdx];
		if (wrBuf.size() < lowN) { wrBuf.resize(lowN); wrZ.resize(lowN); }
		const bool histOk = g_histValid && rdBuf.size() >= lowN && View != nullptr;
		const float *histRd  = histOk ? rdBuf.data() : nullptr;
		const float *histZRd = histOk ? rdZ.data()   : nullptr;
		float *histWr = wrBuf.data(); float *histZWr = wrZ.data();
		float blendW = fds::FeatureFlags::ssao_temporal_blend();
		if (blendW < 0.0f) blendW = 0.0f;
		if (blendW > 0.98f) blendW = 0.98f;
		// Current camera: world = Mat^T · viewPos + ISource. History camera:
		// viewPos' = histMat · (world − histPos). Matrix is a C array — copy
		// the elements into a struct the tile lambdas capture by value.
		struct Cam9 { float c[9]; float pm[9]; };
		Cam9 cams = {};
		Vector camPos{0,0,0}, prevPos = g_histPos;
		if (View) {
			for (int r = 0; r < 3; ++r) for (int q = 0; q < 3; ++q) {
				cams.c[r*3+q]  = View->Mat[r][q];
				cams.pm[r*3+q] = g_histMat[r][q];
			}
			camPos = View->ISource;
		}
		const float invDownT = 1.0f / float(down);
		const float depthSigT = std::max(radius, 1.0f);
		const float invDepthKT = 1.0f / (4.0f * depthSigT * depthSigT);
		const float *aoIn = aoSrc;
		const int tsx = (lowW + numTilesX - 1) / numTilesX;
		const int tsy = (lowH + numTilesY - 1) / numTilesY;
		dispatchIndexed(numTilesX * numTilesY, nullptr, [=](int _t) {
			const int tj = _t / numTilesX, ti = _t - tj * numTilesX;
			const int ly1 = tsy * tj, ly2 = std::min(ly1 + tsy, lowH);
			const int lx1 = tsx * ti, lx2 = std::min(lx1 + tsx, lowW);
			{
					for (int ly = ly1; ly < ly2; ++ly) {
						for (int lx = lx1; lx < lx2; ++lx) {
							const size_t lo = size_t(ly) * size_t(lowW) + size_t(lx);
							const float aoNew = aoIn[lo];
							const float z = aoZ[lo];
							float outAo = aoNew;
							if (histRd && z > 0.0f) {
								const int px = std::min(lx * down + half, W - 1);
								const int py = std::min(ly * down + half, H - 1);
								const float Px = (float(px) - cx) * z * invFOVX;
								const float Py = (cy - float(py)) * z * invFOVY;
								const float Wx = cams.c[0]*Px + cams.c[3]*Py + cams.c[6]*z + camPos.x;
								const float Wy = cams.c[1]*Px + cams.c[4]*Py + cams.c[7]*z + camPos.y;
								const float Wz = cams.c[2]*Px + cams.c[5]*Py + cams.c[8]*z + camPos.z;
								const float dx = Wx - prevPos.x, dy = Wy - prevPos.y, dz = Wz - prevPos.z;
								const float P2x = cams.pm[0]*dx + cams.pm[1]*dy + cams.pm[2]*dz;
								const float P2y = cams.pm[3]*dx + cams.pm[4]*dy + cams.pm[5]*dz;
								const float P2z = cams.pm[6]*dx + cams.pm[7]*dy + cams.pm[8]*dz;
								if (P2z > 0.5f) {
									const float spx = cx + P2x * fovX / P2z;
									const float spy = cy - P2y * fovY / P2z;
									const float gx = (spx - float(half)) * invDownT;
									const float gy = (spy - float(half)) * invDownT;
									int x0 = (int)floorf(gx), y0 = (int)floorf(gy);
									const float fxs = gx - float(x0), fys = gy - float(y0);
									const int x1c = x0 + 1, y1c = y0 + 1;
									if (x0 >= 0 && y0 >= 0 && x1c < lowW && y1c < lowH) {
										const size_t o00 = size_t(y0 )*lowW + x0, o10 = size_t(y0 )*lowW + x1c;
										const size_t o01 = size_t(y1c)*lowW + x0, o11 = size_t(y1c)*lowW + x1c;
										const float bw00 = (1-fxs)*(1-fys), bw10 = fxs*(1-fys);
										const float bw01 = (1-fxs)*fys,     bw11 = fxs*fys;
										float sum = 0.0f, wsum = 0.0f;
#define SSAO_TTAP(O, BW) { const float zt = histZRd[O]; \
	if (zt >= 0.0f) { const float dzr = P2z - zt; \
		const float wd = 1.0f - dzr*dzr*invDepthKT; \
		if (wd > 0.0f) { const float w = (BW)*wd; \
			sum += histRd[O]*w; wsum += w; } } }
										SSAO_TTAP(o00, bw00) SSAO_TTAP(o10, bw10)
										SSAO_TTAP(o01, bw01) SSAO_TTAP(o11, bw11)
#undef SSAO_TTAP
										if (wsum > 1e-4f)
											outAo = (sum / wsum) * blendW + aoNew * (1.0f - blendW);
									}
								}
							}
							histWr[lo] = outAo;
							histZWr[lo] = z;
						}
					}
					renderns::tileDone.release();
				}
		});
		for (int n = numTilesX * numTilesY, k = 0; k < n; ++k) renderns::tileDone.acquire();
		g_histIdx = wrIdx;
		g_histValid = true;
		if (View) {
			for (int r = 0; r < 3; ++r) for (int q = 0; q < 3; ++q) g_histMat[r][q] = View->Mat[r][q];
			g_histPos = View->ISource;
		}
		aoSrc = histWr;
	}

	// ── Pass 3: apply (full-res). down==1: direct. down>1: cheap 4-tap
	//    depth-aware bilinear upsample (the low-res denoise already did the
	//    edge-aware work, so the upsample stays light — 4 taps, no decode). ──
	{
		const int tsx = (W + numTilesX - 1) / numTilesX;
		const int tsy = (H + numTilesY - 1) / numTilesY;
		const float invDown  = 1.0f / float(down);
		const float depthSig = std::max(radius, 1.0f);
		const float invDepthK = 1.0f / (4.0f * depthSig * depthSig);  // divide-free depth falloff
		dispatchIndexed(numTilesX * numTilesY, nullptr, [=](int _t) {
			const int tj = _t / numTilesX, ti = _t - tj * numTilesX;
			const int y1 = tsy * tj, y2 = std::min(y1 + tsy, H);
			const int x1 = tsx * ti, x2 = std::min(x1 + tsx, W);
			{
					// FULL-RES HDR FAST PATH — the shape his arm actually runs
					// (--ssao_downscale=1 + --hdr). g_hdrBuf is INTERLEAVED
					// B,G,R,coverage f16, so the scalar body was three
					// load / fcvt / fmul / fcvt / store chains per pixel behind an
					// unpredictable sky branch, over every pixel of the frame.
					// vld4/vst4 deinterleaves four pixels in one go and the branch
					// becomes a SELECT (ao = 1.0f on sky) — x*1.0f is exact in
					// IEEE and the f16 round-trip is the same round-to-nearest, so
					// this is bit-identical to the branch, not an approximation of
					// it. Coverage (lane 3) is left alone, as before.
#if defined(__aarch64__) && !defined(FDS_HDR_F32)
					// !dumpAo: the vld4/vst4 path has nowhere to write the dump
					// plane, so the instrument takes the generic loop below. The
					// two are bit-identical by construction (see the commit that
					// introduced this path), so the dumped field is the field the
					// un-instrumented frame applies.
					if (down == 1 && useHdr && !dbg && !dumpAo) {
						for (int py = y1; py < y2; ++py) {
							const size_t row = size_t(py) * size_t(W);
							int px = x1;
							for (; px + 4 <= x2; px += 4) {
								const size_t i = row + size_t(px);
								float a4[4];
								for (int k = 0; k < 4; ++k)
									a4[k] = zEnc[i + k] ? aoSrc[i + k] : 1.0f;
								const float32x4_t ao = vld1q_f32(a4);
								float16x4x4_t h = vld4_f16(reinterpret_cast<const __fp16*>(hbuf + i * 4));
								h.val[0] = vcvt_f16_f32(vmulq_f32(vcvt_f32_f16(h.val[0]), ao));
								h.val[1] = vcvt_f16_f32(vmulq_f32(vcvt_f32_f16(h.val[1]), ao));
								h.val[2] = vcvt_f16_f32(vmulq_f32(vcvt_f32_f16(h.val[2]), ao));
								vst4_f16(reinterpret_cast<__fp16*>(hbuf + i * 4), h);
							}
							for (; px < x2; ++px) {
								const size_t i = row + size_t(px);
								if (zEnc[i] == 0) continue;
								const float ao = aoSrc[i];
								fds::hdrf* h = hbuf + i * 4;
								h[0] *= ao; h[1] *= ao; h[2] *= ao;
							}
						}
						renderns::tileDone.release();
						return;
					}
#endif
					for (int py = y1; py < y2; ++py) {
						const size_t row = size_t(py) * size_t(W);
						for (int px = x1; px < x2; ++px) {
							const size_t i = row + size_t(px);
							const word ze = zEnc[i];
							if (ze == 0) continue;                          // leave sky

							float ao;
							if (down == 1) {
								ao = aoSrc[i];
							} else {
								const float zf = float(0xFF80 - ze) * invZScale;
								const float gx = (float(px) - float(half)) * invDown;
								const float gy = (float(py) - float(half)) * invDown;
								int x0 = (int)floorf(gx), y0 = (int)floorf(gy);
								const float fxs = gx - float(x0), fys = gy - float(y0);
								int x1c = x0 + 1, y1c = y0 + 1;
								x0 = std::max(0, std::min(lowW - 1, x0)); x1c = std::max(0, std::min(lowW - 1, x1c));
								y0 = std::max(0, std::min(lowH - 1, y0)); y1c = std::max(0, std::min(lowH - 1, y1c));
								const size_t o00 = size_t(y0)*lowW+x0, o10 = size_t(y0)*lowW+x1c;
								const size_t o01 = size_t(y1c)*lowW+x0, o11 = size_t(y1c)*lowW+x1c;
								const float bw00 = (1-fxs)*(1-fys), bw10 = fxs*(1-fys);
								const float bw01 = (1-fxs)*fys,     bw11 = fxs*fys;
								float sum = 0.0f, wsum = 0.0f;
								#define SSAO_TAP(O, BW) { float zt = aoZ[O]; if (zt >= 0.0f) { \
									float dz = zf - zt; float wd = 1.0f - dz*dz*invDepthK; \
									if (wd > 0.0f) { float w = (BW)*wd; sum += aoSrc[O]*w; wsum += w; } } }
								SSAO_TAP(o00, bw00) SSAO_TAP(o10, bw10)
								SSAO_TAP(o01, bw01) SSAO_TAP(o11, bw11)
								#undef SSAO_TAP
								ao = wsum > 1e-6f ? sum / wsum : 1.0f;
							}

							if (aoDump) {
								aoDump[i] = ao;
								aoDumpZ[i] = float(0xFF80 - ze) * invZScale;
								// The RAW stored G-buffer normal, NOT sign-flipped
								// — the flip toward the viewer is the AO kernel's
								// own step and the GPU arm must apply its own, or
								// the comparison would be pre-agreeing on it.
								meka::oct_decode_u32(nrm[i], aoDumpN[i * 3],
								                     aoDumpN[i * 3 + 1], aoDumpN[i * 3 + 2]);
							}
							if (dbg) {
								int g = (int)(ao * 255.0f + 0.5f);
								const byte gb = (byte)(g < 0 ? 0 : (g > 255 ? 255 : g));
								out[i] = (dword(gb) << 16) | (dword(gb) << 8) | dword(gb) | 0xFF000000u;
							} else if (useHdr) {
								fds::hdrf* h = hbuf + i * 4;
								h[0] *= ao; h[1] *= ao; h[2] *= ao;       // B,G,R linear radiance
							} else {
								const dword pix = out[i];
								int Rr = (int)(float((pix >> 16) & 0xFFu) * ao);
								int Gg = (int)(float((pix >>  8) & 0xFFu) * ao);
								int Bb = (int)(float( pix        & 0xFFu) * ao);
								out[i] = (dword(Rr) << 16) | (dword(Gg) << 8) | dword(Bb) | 0xFF000000u;
							}
						}
					}
					renderns::tileDone.release();
				}
		});
		for (int n = numTilesX * numTilesY, k = 0; k < n; ++k) renderns::tileDone.acquire();
	}

	if (dumpAo) WriteAoDump(g_aoDumpPlane.data(), g_aoDumpZ.data(),
	                        g_aoDumpN.data(), W, H);

	const auto t1 = std::chrono::steady_clock::now();
	g_ssaoLastMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

	static int frame = 0;
	static const bool sEveryFrame = getenv("FDS_SSAO_EVERY") != nullptr;
	if (sEveryFrame || ((frame++) % 120) == 0) {
		fprintf(stderr, "[ssao] %dx%d /%d (%dx%d), %s, %s: %.2f ms\n",
		        W, H, down, lowW, lowH,
		        gtao ? "GTAO+bitmask" : "hemisphere",
		        useHdr ? "HDR g_hdrBuf" : "LDR VPage", g_ssaoLastMs);
		if (getenv("FDS_SSAO_STATS")) {
			using ms = std::chrono::duration<double, std::milli>;
			fprintf(stderr, "[ssao-pass] compute=%.2f  denoise=%.2f  apply=%.2f ms\n",
			        ms(tP1 - t0).count(), ms(tP2 - tP1).count(), ms(t1 - tP2).count());
			double zmin = 1e30, zmax = -1e30, aoSum = 0; long touched = 0, occluded = 0;
			for (size_t lo = 0; lo < lowN; ++lo) {
				if (aoZ[lo] < 0.0f) continue;
				if (aoZ[lo] < zmin) zmin = aoZ[lo]; if (aoZ[lo] > zmax) zmax = aoZ[lo];
				aoSum += aoRaw[lo]; ++touched;
				if (aoRaw[lo] < 0.9f) ++occluded;
			}
			fprintf(stderr, "[ssao-stats] viewZ [%.1f .. %.1f]  radius=%.1f  cells=%ld  ao<0.9=%.1f%%  meanAO=%.3f\n",
			        zmin, zmax, radius, touched, touched ? 100.0 * occluded / touched : 0.0,
			        touched ? aoSum / touched : 1.0);
		}
	}
}

// ── --mem_census: the SSAO low-res planes ─────────────────────────────────
// Everything here is (XRes/down) x (YRes/down), so --ssao_downscale is a
// quadratic lever on both the taps and the bytes.
static void MemCensus_SSAO() {
	const size_t p = (g_aoRaw.capacity() + g_aoBlur.capacity() + g_aoZ.capacity())
	               * sizeof(float);
	size_t hist = 0;
	for (int i = 0; i < 2; ++i)
		hist += (g_aoHistBuf[i].capacity() + g_aoHistZBuf[i].capacity()) * sizeof(float);
	fds::MemCensus::add("ssao", "raw/blur/z planes", p, false,
		"3 parallel float planes x lowW*lowH (= W*H / ssao_downscale^2)");
	fds::MemCensus::add("ssao", "temporal history (ping-pong ao + z)", hist, false,
		"2 x (ao + viewZ) float planes at lowW*lowH; 0 unless --ssao_temporal");
}
FDS_MEMCENSUS_REPORTER(MemCensus_SSAO);
