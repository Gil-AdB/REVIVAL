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
#include <semaphore>

#include <arm_neon.h>
#include "simde/x86/fma.h"

#include "Base/FDS_VARS.H"
#include "Base/FeatureFlags.h"
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

// Jorge Jimenez interleaved-gradient noise -> per-pixel decorrelation angle.
inline float ignAngle(int px, int py) {
	float v = 52.9829189f * fmodf(0.06711056f * float(px) + 0.00583715f * float(py), 1.0f);
	return (v - floorf(v)) * 6.2831853f;
}

// Low-res scratch (sized lowW*lowH). aoZ holds the representative view-space Z
// per cell (< 0 == sky / no surface).
std::vector<float> g_aoRaw, g_aoBlur, g_aoZ;

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
	int blurR = fds::FeatureFlags::ssao_blur();    blurR = std::max(0, std::min(8, blurR));
	int down  = fds::FeatureFlags::ssao_downscale(); down = std::max(1, std::min(4, down));
	const float radius   = fds::FeatureFlags::ssao_radius();
	const float strength = fds::FeatureFlags::ssao_strength();
	const float bias     = fds::FeatureFlags::ssao_bias();
	const float power    = fds::FeatureFlags::ssao_power();
	const bool  dbg      = fds::FeatureFlags::ssao_debug();

	buildKernel(nSamp);

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
	const meka::u16* nrm = g_gbuffer->normal.data();
	const float* kx = g_kx.data(); const float* ky = g_ky.data(); const float* kz = g_kz.data();
	float* aoRaw  = g_aoRaw.data();
	float* aoBlur = g_aoBlur.data();
	float* aoZ    = g_aoZ.data();
	const int half = down >> 1;

	// HDR: AO multiplies linear radiance in g_hdrBuf (correct + survives the
	// tonemap). Gate on Hdr_WritableFor (buffer sized for THIS view) not
	// g_hdrActive — at our call site the kernel has written opaque radiance but
	// activation runs later. LDR: multiply VPage in place.
	const bool useHdr = fds::FeatureFlags::hdr() && fds::Hdr_WritableFor(W, H);
	float* hbuf = useHdr ? fds::g_hdrBuf.data() : nullptr;
	dword* out  = reinterpret_cast<dword*>(VPage);

	constexpr int numTilesX = 6, numTilesY = 4;

	// ── Pass 1: compute AO + guide (depth, normal) on the low-res grid ──────
	// Per-pixel SETUP (reconstruct, decode normal, build the rotated TBN basis)
	// is scalar — it's once-per-pixel and cheap next to the sample loop. The
	// SAMPLE LOOP is the hot path: vectorized 8 pixels wide, with approximate
	// reciprocals (_mm256_rcp_ps) for the per-sample divides — one NR-refined
	// rcp for the projection (tap position needs accuracy) and a raw rcp for
	// the range term (it feeds min(1,·)+average, so 12 bits is plenty). The
	// per-sample depth lookup stays a scalar gather (re-projected address); the
	// arithmetic around it is what SIMD recovers. Scalar tail for the <8 remnant.
	{
		const int tsx = (lowW + numTilesX - 1) / numTilesX;
		const int tsy = (lowH + numTilesY - 1) / numTilesY;
		const float invN = 1.0f / float(nSamp);
		for (int tj = 0; tj < numTilesY; ++tj) {
			const int ly1 = tsy * tj, ly2 = std::min(ly1 + tsy, lowH);
			for (int ti = 0; ti < numTilesX; ++ti) {
				const int lx1 = tsx * ti, lx2 = std::min(lx1 + tsx, lowW);
				ThreadPool::instance().enqueue([=]() {
					// Per-pixel setup: writes aoZ; returns false for sky.
					struct Setup { float x,y,z, Tx,Ty,Tz, Bx,By,Bz, nx,ny,nz; };
					auto setup = [&](int px, int py, size_t lo, Setup& S) -> bool {
						const size_t i = size_t(py) * size_t(W) + size_t(px);
						const word ze = zEnc[i];
						if (ze == 0) { aoRaw[lo] = 1.0f; aoZ[lo] = -1.0f; return false; }
						const float z = float(0xFF80 - ze) * invZScale;
						S.z = z;
						S.x = (float(px) - cx) * z * invFOVX;
						S.y = (cy - float(py)) * z * invFOVY;
						aoZ[lo] = z;
						float nx, ny, nz;
						meka::oct_decode_u16(nrm[i], nx, ny, nz);
						if (nx*S.x + ny*S.y + nz*z > 0.0f) { nx = -nx; ny = -ny; nz = -nz; }
						float hx = 0.0f, hy = 0.0f, hz = 1.0f;
						if (fabsf(nz) > 0.999f) { hx = 1.0f; hz = 0.0f; }
						float t0x = hy*nz - hz*ny, t0y = hz*nx - hx*nz, t0z = hx*ny - hy*nx;
						float tl = fast_rsqrt(t0x*t0x + t0y*t0y + t0z*t0z + 1e-12f);  // approx 1/sqrt, no divide
						t0x *= tl; t0y *= tl; t0z *= tl;
						float b0x = ny*t0z - nz*t0y, b0y = nz*t0x - nx*t0z, b0z = nx*t0y - ny*t0x;
						const float a = ignAngle(px, py);
						const float ca = cosf(a), sa = sinf(a);
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
								const int px = std::min((lx + k) * down + half, W - 1);
								valid[k] = setup(px, py, rowLo + size_t(lx + k), S);
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
							Setup S{};
							if (!setup(px, py, lo, S)) continue;
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
				});
			}
		}
		for (int n = numTilesX * numTilesY, k = 0; k < n; ++k) renderns::tileDone.acquire();
	}
	const auto tP1 = std::chrono::steady_clock::now();

	// ── Pass 2: low-res DEPTH-aware bilateral denoise ──────────────────────
	// Resolves the per-pixel interleaved-gradient dither into smooth AO. DEPTH
	// only — a previous cos^4(normal) term was REMOVED: on normal-mapped
	// surfaces (the greets floor) the bump-perturbed shading normal makes
	// neighbouring cells' normals disagree, so the cos^4 down-weighted the
	// denoise's own taps and the dither survived as a diagonal "hatch"/banding.
	// Depth alone denoises flat-but-bumpy surfaces correctly. (Crease
	// preservation would need a smooth geometric normal, which the G-buffer
	// doesn't store — see docs/GRAPHICS_PIPELINE.md.)
	const float* aoSrc = aoRaw;
	if (blurR > 0) {
		const int tsx = (lowW + numTilesX - 1) / numTilesX;
		const int tsy = (lowH + numTilesY - 1) / numTilesY;
		const float depthSig = std::max(radius, 1.0f);
		// Divide-free depth falloff: max(0, 1 - dz²·invDepthK), cutoff at
		// 2·depthSig. Replaces a per-tap reciprocal (25 divides/pixel at R=2)
		// with a multiply — the denoise is the dominant full-res cost.
		const float invDepthK = 1.0f / (4.0f * depthSig * depthSig);
		for (int tj = 0; tj < numTilesY; ++tj) {
			const int ly1 = tsy * tj, ly2 = std::min(ly1 + tsy, lowH);
			for (int ti = 0; ti < numTilesX; ++ti) {
				const int lx1 = tsx * ti, lx2 = std::min(lx1 + tsx, lowW);
				ThreadPool::instance().enqueue([=]() {
					for (int ly = ly1; ly < ly2; ++ly) {
						const int by0 = std::max(0, ly - blurR), by1 = std::min(lowH - 1, ly + blurR);
						for (int lx = lx1; lx < lx2; ++lx) {
							const size_t lo = size_t(ly) * size_t(lowW) + size_t(lx);
							const float zc = aoZ[lo];
							if (zc < 0.0f) { aoBlur[lo] = aoRaw[lo]; continue; }
							const int bx0 = std::max(0, lx - blurR), bx1 = std::min(lowW - 1, lx + blurR);
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
						}
					}
					renderns::tileDone.release();
				});
			}
		}
		for (int n = numTilesX * numTilesY, k = 0; k < n; ++k) renderns::tileDone.acquire();
		aoSrc = aoBlur;
	}
	const auto tP2 = std::chrono::steady_clock::now();

	// ── Pass 3: apply (full-res). down==1: direct. down>1: cheap 4-tap
	//    depth-aware bilinear upsample (the low-res denoise already did the
	//    edge-aware work, so the upsample stays light — 4 taps, no decode). ──
	{
		const int tsx = (W + numTilesX - 1) / numTilesX;
		const int tsy = (H + numTilesY - 1) / numTilesY;
		const float invDown  = 1.0f / float(down);
		const float depthSig = std::max(radius, 1.0f);
		const float invDepthK = 1.0f / (4.0f * depthSig * depthSig);  // divide-free depth falloff
		for (int tj = 0; tj < numTilesY; ++tj) {
			const int y1 = tsy * tj, y2 = std::min(y1 + tsy, H);
			for (int ti = 0; ti < numTilesX; ++ti) {
				const int x1 = tsx * ti, x2 = std::min(x1 + tsx, W);
				ThreadPool::instance().enqueue([=]() {
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

							if (dbg) {
								int g = (int)(ao * 255.0f + 0.5f);
								const byte gb = (byte)(g < 0 ? 0 : (g > 255 ? 255 : g));
								out[i] = (dword(gb) << 16) | (dword(gb) << 8) | dword(gb) | 0xFF000000u;
							} else if (useHdr) {
								float* h = hbuf + i * 4;
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
				});
			}
		}
		for (int n = numTilesX * numTilesY, k = 0; k < n; ++k) renderns::tileDone.acquire();
	}

	const auto t1 = std::chrono::steady_clock::now();
	g_ssaoLastMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

	static int frame = 0;
	if (((frame++) % 120) == 0) {
		fprintf(stderr, "[ssao] %dx%d /%d (%dx%d), %d samples, %s, %s: %.2f ms\n",
		        W, H, down, lowW, lowH, nSamp,
		        down == 1 ? "box-blur denoise" : "bilateral upsample",
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
