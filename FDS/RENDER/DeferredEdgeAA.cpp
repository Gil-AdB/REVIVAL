// ── Edge-aware anti-aliasing (deferred path) ──────────────────────────────
//
// Minimal post-process AA tuned to the deferred G-buffer. It detects geometry
// edges from per-pixel NORMAL + DEPTH discontinuities (creases via the normal,
// silhouettes via the depth jump — including geometry-vs-sky) and blends a 5-tap
// cross only across those edges. Interiors and textures are untouched, so it
// removes jaggies without the global softening of a luma-only FXAA.
//
// Runs AFTER the tonemap, on the final LDR image in VPage (HDR-agnostic — VPage
// already holds the tonemapped result). One tiled wave over the same 6×4
// threadpool grid the other deferred post-passes use. Gated --aa; default off.
//
// Reconstruction matches the surface kernel: z = (0xFF80 - zEnc) * invZScale,
// zEnc == 0 marks sky / no surface.

#include <vector>
#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <semaphore>

#include <arm_neon.h>
#include "simde/x86/fma.h"   // pulls in avx2; gives _mm256_* on arm64 + x86

#include "Base/FDS_VARS.H"
#include "Base/FeatureFlags.h"
#include "FILLERS/Mekalele.h"        // meka::oct_decode_u16, meka::GBuffer
#include "RENDER/DeferredCommon.h"
#include "Threads.h"

// Shared tile-drain semaphore (defined in DeferredFastFog.cpp). AA never runs
// concurrently with SSAO/fog, so sharing the counter is safe.
namespace renderns { extern std::counting_semaphore<INT_MAX> tileDone; }

// Last frame's AA wall time in ms, for at-a-glance attribution.
double g_aaLastMs = 0.0;

namespace {
// Stable snapshot of VPage so parallel tiles read neighbour colours that aren't
// being overwritten by an adjacent tile mid-pass.
std::vector<dword> g_aaSrc;
}

void Render_EdgeAA() {
	if (!fds::FeatureFlags::aa()) return;
	if (!g_gbuffer) return;
	const int    W = (int)XRes, H = (int)YRes;
	const size_t N = size_t(W) * size_t(H);
	if (g_gbuffer->normal.size() < N) return;          // forward path: no gbuffer

	const auto t0 = std::chrono::steady_clock::now();

	float strength = fds::FeatureFlags::aa_strength();  // 0..1 max blend at an edge
	if (strength < 0.0f) strength = 0.0f;
	if (strength > 1.0f) strength = 1.0f;
	const bool viz = fds::FeatureFlags::aa_viz();   // edge heatmap instead of blend
	const float invZScale = (g_zscale != 0.0f) ? 1.0f / g_zscale : 1.0f;
	const meka::u16* nrm = g_gbuffer->normal.data();
	const word*      zEnc = ZPage16;
	dword*           out  = reinterpret_cast<dword*>(VPage);

	if (g_aaSrc.size() < N) g_aaSrc.resize(N);
	dword* src = g_aaSrc.data();
	std::memcpy(src, out, N * sizeof(dword));

	constexpr int numTilesX = 6, numTilesY = 4;
	const int tsx = (W + numTilesX - 1) / numTilesX;
	const int tsy = (H + numTilesY - 1) / numTilesY;

	// Edge metric for one pixel (0 = flat interior … ~2 = strong edge), or -1 for
	// a sky pixel (skip). Normal edge from the RAW packed oct code — no decode:
	// the u16 is (int8 octX)|(int8 octY)<<8, adjacent normals have adjacent oct
	// codes, so |Δqx|+|Δqy| is a linear angle proxy (~127 L1 ≈ 90° crease →
	// scale 1/127 ≈ old 1-N·N). Fold seams can spike Δ for similar normals — a
	// harmless false-positive edge, never a miss. This is the SCALAR reference;
	// the SIMD interior below computes the identical metric 8-wide.
	auto edgeAt = [&](int x, int y) -> float {
		const size_t i = size_t(y) * size_t(W) + size_t(x);
		const word ze = zEnc[i];
		if (ze == 0) return -1.0f;
		const float z0 = float(0xFF80 - ze) * invZScale;
		const meka::u16 p0 = nrm[i];
		const int qx0 = int(int8_t(p0 & 0xff));
		const int qy0 = int(int8_t((p0 >> 8) & 0xff));
		const int off[4] = { -1, +1, -W, +W };
		float edge = 0.0f;
		for (int d = 0; d < 4; ++d) {
			const size_t j = i + size_t(off[d]);
			const word zn = zEnc[j];
			if (zn == 0) { edge += 1.0f; continue; }       // silhouette vs sky
			const float zj = float(0xFF80 - zn) * invZScale;
			const float denom = z0 > 1e-3f ? z0 : 1e-3f;
			const float zrel  = std::fabs(z0 - zj) / denom;
			edge += zrel > 0.04f ? 1.0f : zrel * 25.0f;    // depth edge
			const meka::u16 pj = nrm[j];
			const int dq = std::abs(qx0 - int(int8_t(pj & 0xff)))
			             + std::abs(qy0 - int(int8_t((pj >> 8) & 0xff)));
			edge += float(dq) * (1.0f / 127.0f);           // normal edge
		}
		return edge * 0.25f;
	};

	// Apply the 5-tap cross blend at pixel i given its edge metric.
	auto blendAt = [&](size_t i, float edge) {
		float w = (edge > 1.0f ? 1.0f : edge) * strength;
		const dword c  = src[i];
		const dword cl = src[i - 1],         cr = src[i + 1];
		const dword cu = src[i - size_t(W)], cd = src[i + size_t(W)];
		int ob[3];
		for (int k = 0; k < 3; ++k) {
			const int s = k * 8;
			const float ctr = float((c >> s) & 0xFF);
			const float avg = (ctr + float((cl >> s) & 0xFF) + float((cr >> s) & 0xFF)
			                       + float((cu >> s) & 0xFF) + float((cd >> s) & 0xFF)) * 0.2f;
			int v = int(ctr + (avg - ctr) * w + 0.5f);
			if (v < 0) v = 0; else if (v > 255) v = 255;
			ob[k] = v;
		}
		out[i] = dword(ob[0]) | (dword(ob[1]) << 8) | (dword(ob[2]) << 16) | (c & 0xFF000000u);
	};

	for (int tj = 0; tj < numTilesY; ++tj) {
		for (int ti = 0; ti < numTilesX; ++ti) {
			const int x1 = ti * tsx, x2 = std::min(x1 + tsx, W);
			const int y1 = tj * tsy, y2 = std::min(y1 + tsy, H);
			ThreadPool::instance().enqueue([=]() {
				// VIZ: green edge heatmap over a dimmed frame. All-scalar (rare;
				// for inspection, perf irrelevant). Matches the offline heatmap.
				if (viz) {
					for (int y = y1; y < y2; ++y) {
						for (int x = x1; x < x2; ++x) {
							const size_t i = size_t(y) * size_t(W) + size_t(x);
							const dword c = src[i];
							const bool border = (x == 0 || x == W - 1 || y == 0 || y == H - 1);
							const float e = border ? -1.0f : edgeAt(x, y);
							if (e >= 0.05f) { out[i] = 0x0000FF00u | (c & 0xFF000000u); }   // green
							else {                                                          // dim 45%
								const dword b = ((c & 0xFF) * 115 >> 8);
								const dword g = (((c >> 8) & 0xFF) * 115 >> 8);
								const dword r = (((c >> 16) & 0xFF) * 115 >> 8);
								out[i] = b | (g << 8) | (r << 16) | (c & 0xFF000000u);
							}
						}
					}
					renderns::tileDone.release();
					return;
				}

				const __m256i vFF80 = _mm256_set1_epi32(0xFF80);
				const __m256i vZeroI = _mm256_setzero_si256();
				const __m256  vInvZ  = _mm256_set1_ps(invZScale);
				const __m256  vOne   = _mm256_set1_ps(1.0f);
				const __m256  vEps   = _mm256_set1_ps(1e-3f);
				const __m256  v25    = _mm256_set1_ps(25.0f);
				const __m256  vInv127= _mm256_set1_ps(1.0f / 127.0f);
				const int off[4] = { -1, +1, -W, +W };

				for (int y = std::max(y1, 1); y < std::min(y2, H - 1); ++y) {
					const int xa = std::max(x1, 1), xb = std::min(x2, W - 1);
					int x = xa;
					// 8-wide interior fast path: vectorize the edge metric for 8
					// pixels, then scalar-blend only the (rare) edge lanes.
					for (; x + 8 <= xb; x += 8) {
						const size_t i = size_t(y) * size_t(W) + size_t(x);
						const __m256i ze = _mm256_cvtepu16_epi32(_mm_loadu_si128((const __m128i*)(zEnc + i)));
						const __m256  z0 = _mm256_mul_ps(
							_mm256_cvtepi32_ps(_mm256_sub_epi32(vFF80, ze)), vInvZ);
						const __m256  centerSky = _mm256_castsi256_ps(_mm256_cmpeq_epi32(ze, vZeroI));
						const __m256i nv = _mm256_cvtepu16_epi32(_mm_loadu_si128((const __m128i*)(nrm + i)));
						const __m256i qx0 = _mm256_srai_epi32(_mm256_slli_epi32(nv, 24), 24);
						const __m256i qy0 = _mm256_srai_epi32(_mm256_slli_epi32(nv, 16), 24);
						const __m256  denom = _mm256_max_ps(z0, vEps);

						__m256 edge = _mm256_setzero_ps();
						for (int d = 0; d < 4; ++d) {
							const size_t j = i + size_t(off[d]);
							const __m256i zn = _mm256_cvtepu16_epi32(_mm_loadu_si128((const __m128i*)(zEnc + j)));
							const __m256  skyN = _mm256_castsi256_ps(_mm256_cmpeq_epi32(zn, vZeroI));
							const __m256  zj = _mm256_mul_ps(
								_mm256_cvtepi32_ps(_mm256_sub_epi32(vFF80, zn)), vInvZ);
							const __m256  zrel = _mm256_div_ps(
								_mm256_andnot_ps(_mm256_set1_ps(-0.0f), _mm256_sub_ps(z0, zj)), denom);
							const __m256  depthE = _mm256_min_ps(vOne, _mm256_mul_ps(zrel, v25));
							const __m256i nj = _mm256_cvtepu16_epi32(_mm_loadu_si128((const __m128i*)(nrm + j)));
							const __m256i qxj = _mm256_srai_epi32(_mm256_slli_epi32(nj, 24), 24);
							const __m256i qyj = _mm256_srai_epi32(_mm256_slli_epi32(nj, 16), 24);
							const __m256i dq = _mm256_add_epi32(
								_mm256_abs_epi32(_mm256_sub_epi32(qx0, qxj)),
								_mm256_abs_epi32(_mm256_sub_epi32(qy0, qyj)));
							const __m256  normE = _mm256_mul_ps(_mm256_cvtepi32_ps(dq), vInv127);
							// sky neighbour → contribute exactly 1.0 (no normal term)
							const __m256  contrib = _mm256_blendv_ps(_mm256_add_ps(depthE, normE), vOne, skyN);
							edge = _mm256_add_ps(edge, contrib);
						}
						edge = _mm256_mul_ps(edge, _mm256_set1_ps(0.25f));
						// Mask out sky centers (-1 equiv: just skip).
						alignas(32) float eA[8]; _mm256_store_ps(eA, edge);
						alignas(32) float skyA[8]; _mm256_store_ps(skyA, centerSky);
						for (int l = 0; l < 8; ++l) {
							if (skyA[l] != 0.0f) continue;             // sky center
							if (eA[l] < 0.05f) continue;               // interior
							blendAt(i + size_t(l), eA[l]);
						}
					}
					// scalar remainder + left/right border of this tile row
					for (; x < xb; ++x) {
						const float e = edgeAt(x, y);
						if (e >= 0.05f) blendAt(size_t(y) * size_t(W) + size_t(x), e);
					}
				}
				renderns::tileDone.release();
			});
		}
	}
	for (int n = numTilesX * numTilesY, k = 0; k < n; ++k) renderns::tileDone.acquire();

	g_aaLastMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - t0).count();
	if (std::getenv("FDS_AA_PROF")) {
		static int c = 0;
		if ((c++ & 63) == 0)
			std::fprintf(stderr, "[aa] %dx%d str=%.2f: %.3f ms\n", W, H, strength, g_aaLastMs);
	}
}
