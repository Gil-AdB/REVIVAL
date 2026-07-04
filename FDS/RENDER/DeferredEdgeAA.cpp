// ── Edge-aware anti-aliasing (deferred path) ──────────────────────────────
//
// Minimal post-process AA tuned to the deferred G-buffer. It detects geometry
// edges and blends a 5-tap cross only across them; interiors and textures are
// untouched, so it removes jaggies without the global softening of a luma FXAA.
//
// Edge metric (per X/Y axis, from the two opposite neighbours):
//   - depth: a step (large first-difference on one side) AND curvature (the two
//     sides not co-linear). This pair distinguishes a real silhouette/crease
//     (step + curvature) from a smoothly-receding GRAZING surface (big step, ~0
//     curvature → NOT flagged) and from bumpy-but-shallow water (curvature, no
//     step → NOT flagged). A first-difference-only depth test wrongly flagged
//     the whole grazing greets floor.
//   - normal: packed-oct byte-distance to the max-divergence neighbour (no
//     decode). Catches creases between equal-depth surfaces.
//   - sky neighbour on an axis → that axis is a hard silhouette.
//
// Runs AFTER the tonemap, on the final LDR image in VPage (HDR-agnostic). One
// tiled wave over the 6×4 post-pass grid; SIMD 8-wide interior (byte-identical
// to the scalar reference) + scalar borders/blend. Gated --aa; default off.
// Debug: --aa_viz (green edge map), FDS_AA_VIZ_SPLIT (red=depth/blue=normal),
// FDS_AA_SCALAR (force scalar), FDS_AA_PROF (pass timing).
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

// RenderContext migration: this TU is GLOBAL-CLEAN — every render pass
// reads its target/projection state from the DeferredLightingCtx param,
// never from the engine globals. The poison makes the compiler enforce
// it (any new use of these names in this file is a build error; read
// ctx, or take a param).
#pragma GCC poison XRes YRes VPage ZPage16 FOVX FOVY CntrEX CntrEY CurScene VESA_BPSL

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

void Render_EdgeAA(const DeferredLightingCtx &ctx) {
	if (!fds::FeatureFlags::aa()) return;
	if (!ctx.gb) return;
	const int    W = ctx.xres, H = ctx.yres;
	const size_t N = size_t(W) * size_t(H);
	if (ctx.gb->normal.size() < N) return;             // forward path: no gbuffer

	const auto t0 = std::chrono::steady_clock::now();

	float strength = fds::FeatureFlags::aa_strength();  // 0..1 max blend at an edge
	if (strength < 0.0f) strength = 0.0f;
	if (strength > 1.0f) strength = 1.0f;
	const bool viz = fds::FeatureFlags::aa_viz();   // edge heatmap instead of blend
	const float invZScale = (ctx.zscale != 0.0f) ? 1.0f / ctx.zscale : 1.0f;
	const meka::u16* nrm = ctx.gb->normal.data();
	const word*      zEnc = ctx.zpage16;
	dword*           out  = reinterpret_cast<dword*>(ctx.vpage);

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
		const float denom = z0 > 1e-3f ? z0 : 1e-3f;
		const meka::u16 p0 = nrm[i];
		const int qx0 = int(int8_t(p0 & 0xff));
		const int qy0 = int(int8_t((p0 >> 8) & 0xff));
		// Per axis: depth edge needs a STEP (large first-diff on at least one
		// side) AND CURVATURE (the two sides not co-linear) → a real
		// silhouette/crease. A grazing floor is a straight depth ramp (big
		// first-diff, ~zero curvature) → not flagged. Water is bumpy but
		// shallow (curvature without a big step) → not flagged. Normal edge is
		// the packed-oct first-difference (catches creases at equal depth).
		const int ax[2][2] = { { -1, +1 }, { -W, +W } };
		float edge = 0.0f;
		for (int a = 0; a < 2; ++a) {
			const word zA = zEnc[i + size_t(ax[a][0])], zB = zEnc[i + size_t(ax[a][1])];
			const meka::u16 pA = nrm[i + size_t(ax[a][0])], pB = nrm[i + size_t(ax[a][1])];
			if (zA == 0 || zB == 0) { edge += 2.0f; continue; }   // silhouette vs sky
			const float za = float(0xFF80 - zA) * invZScale, zb = float(0xFF80 - zB) * invZScale;
			const float step = std::max(std::fabs(z0 - za), std::fabs(z0 - zb)) / denom;
			const float curv = std::fabs(2.0f * z0 - za - zb) / denom;
			// both must clear their knee: step>4%, curvature>1.5%.
			const float sT = step > 0.04f ? 1.0f : step * 25.0f;
			const float cT = curv > 0.015f ? 1.0f : curv * 66.7f;
			edge += std::min(sT, cT);                              // depth edge (this axis)
			const int dqa = std::abs(qx0 - int(int8_t(pA & 0xff))) + std::abs(qy0 - int(int8_t((pA >> 8) & 0xff)));
			const int dqb = std::abs(qx0 - int(int8_t(pB & 0xff))) + std::abs(qy0 - int(int8_t((pB >> 8) & 0xff)));
			edge += float(std::max(dqa, dqb)) * (1.0f / 127.0f);  // normal edge (this axis)
		}
		return edge * 0.25f;
	};

	// Diagnostic split: step×curvature depth + normal edge, returned separately.
	auto edgeSplit = [&](int x, int y, float &dE, float &nE) -> bool {
		const size_t i = size_t(y) * size_t(W) + size_t(x);
		const word ze = zEnc[i];
		if (ze == 0) { dE = nE = 0; return false; }
		const float z0 = float(0xFF80 - ze) * invZScale;
		const float denom = z0 > 1e-3f ? z0 : 1e-3f;
		const meka::u16 p0 = nrm[i];
		const int qx0 = int(int8_t(p0 & 0xff)), qy0 = int(int8_t((p0 >> 8) & 0xff));
		const int ax[2][2] = { { -1, +1 }, { -W, +W } };
		dE = nE = 0.0f;
		for (int a = 0; a < 2; ++a) {
			const word zA = zEnc[i + size_t(ax[a][0])], zB = zEnc[i + size_t(ax[a][1])];
			const meka::u16 pA = nrm[i + size_t(ax[a][0])], pB = nrm[i + size_t(ax[a][1])];
			if (zA == 0 || zB == 0) { dE += 2.0f; continue; }
			const float za = float(0xFF80 - zA) * invZScale, zb = float(0xFF80 - zB) * invZScale;
			const float step = std::max(std::fabs(z0 - za), std::fabs(z0 - zb)) / denom;
			const float curv = std::fabs(2.0f * z0 - za - zb) / denom;
			const float sT = step > 0.04f ? 1.0f : step * 25.0f;
			const float cT = curv > 0.015f ? 1.0f : curv * 66.7f;
			dE += std::min(sT, cT);
			const int dqa = std::abs(qx0 - int(int8_t(pA & 0xff))) + std::abs(qy0 - int(int8_t((pA >> 8) & 0xff)));
			const int dqb = std::abs(qx0 - int(int8_t(pB & 0xff))) + std::abs(qy0 - int(int8_t((pB >> 8) & 0xff)));
			nE += float(std::max(dqa, dqb)) * (1.0f / 127.0f);
		}
		dE *= 0.25f; nE *= 0.25f; return true;
	};
	const bool vizSplit = std::getenv("FDS_AA_VIZ_SPLIT") != nullptr;

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

	dispatchIndexed(numTilesX * numTilesY, nullptr, [=](int _t) {
		const int tj = _t / numTilesX, ti = _t - tj * numTilesX;
		const int y1 = tj * tsy, y2 = std::min(y1 + tsy, H);
		const int x1 = ti * tsx, x2 = std::min(x1 + tsx, W);
		{
				// VIZ: green edge heatmap over a dimmed frame. All-scalar (rare;
				// for inspection, perf irrelevant). Matches the offline heatmap.
				if (viz) {
					for (int y = y1; y < y2; ++y) {
						for (int x = x1; x < x2; ++x) {
							const size_t i = size_t(y) * size_t(W) + size_t(x);
							const dword c = src[i];
							const bool border = (x == 0 || x == W - 1 || y == 0 || y == H - 1);
							if (vizSplit && !border) {
								float dE, nE; edgeSplit(x, y, dE, nE);
								if (dE + nE >= 0.05f) {                 // red=depth, blue=normal
									int rr = int(std::min(1.0f, dE) * 255.0f), bb = int(std::min(1.0f, nE) * 255.0f);
									out[i] = dword(bb) | (dword(rr) << 16) | (c & 0xFF000000u);
									continue;
								}
							}
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

				const bool useSimd = !std::getenv("FDS_AA_SCALAR");
				for (int y = std::max(y1, 1); y < std::min(y2, H - 1); ++y) {
					const int xa = std::max(x1, 1), xb = std::min(x2, W - 1);
					int x = xa;
					// 8-wide interior fast path: vectorize the edge metric for 8
					// pixels, then scalar-blend only the (rare) edge lanes.
					for (; useSimd && x + 8 <= xb; x += 8) {
						const size_t i = size_t(y) * size_t(W) + size_t(x);
						const __m256i ze = _mm256_cvtepu16_epi32(_mm_loadu_si128((const __m128i*)(zEnc + i)));
						const __m256  z0 = _mm256_mul_ps(
							_mm256_cvtepi32_ps(_mm256_sub_epi32(vFF80, ze)), vInvZ);
						const __m256  centerSky = _mm256_castsi256_ps(_mm256_cmpeq_epi32(ze, vZeroI));
						const __m256i nv = _mm256_cvtepu16_epi32(_mm_loadu_si128((const __m128i*)(nrm + i)));
						const __m256i qx0 = _mm256_srai_epi32(_mm256_slli_epi32(nv, 24), 24);
						const __m256i qy0 = _mm256_srai_epi32(_mm256_slli_epi32(nv, 16), 24);
						const __m256  denom = _mm256_max_ps(z0, vEps);

						// Per-axis step×curvature depth + max-side normal edge — the
						// same metric as scalar edgeAt (8-wide). axis 0 = {-1,+1},
						// axis 1 = {-W,+W}.
						const __m256  absMask = _mm256_set1_ps(-0.0f);
						const __m256  vTwo  = _mm256_set1_ps(2.0f);
						const __m256  v66_7 = _mm256_set1_ps(66.7f);
						__m256 edge = _mm256_setzero_ps();
						for (int a = 0; a < 2; ++a) {
							const size_t jm = i + size_t(off[a*2+0]), jp = i + size_t(off[a*2+1]);
							const __m256i zAi = _mm256_cvtepu16_epi32(_mm_loadu_si128((const __m128i*)(zEnc + jm)));
							const __m256i zBi = _mm256_cvtepu16_epi32(_mm_loadu_si128((const __m128i*)(zEnc + jp)));
							const __m256  axisSky = _mm256_or_ps(
								_mm256_castsi256_ps(_mm256_cmpeq_epi32(zAi, vZeroI)),
								_mm256_castsi256_ps(_mm256_cmpeq_epi32(zBi, vZeroI)));
							const __m256  za = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_sub_epi32(vFF80, zAi)), vInvZ);
							const __m256  zb = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_sub_epi32(vFF80, zBi)), vInvZ);
							const __m256  da = _mm256_andnot_ps(absMask, _mm256_sub_ps(z0, za));
							const __m256  db = _mm256_andnot_ps(absMask, _mm256_sub_ps(z0, zb));
							const __m256  step = _mm256_div_ps(_mm256_max_ps(da, db), denom);
							const __m256  curv = _mm256_div_ps(
								_mm256_andnot_ps(absMask,
									_mm256_sub_ps(_mm256_sub_ps(_mm256_mul_ps(vTwo, z0), za), zb)), denom);
							const __m256  sT = _mm256_min_ps(vOne, _mm256_mul_ps(step, v25));
							const __m256  cT = _mm256_min_ps(vOne, _mm256_mul_ps(curv, v66_7));
							const __m256  depthA = _mm256_min_ps(sT, cT);
							const __m256i nAi = _mm256_cvtepu16_epi32(_mm_loadu_si128((const __m128i*)(nrm + jm)));
							const __m256i nBi = _mm256_cvtepu16_epi32(_mm_loadu_si128((const __m128i*)(nrm + jp)));
							const __m256i qxa = _mm256_srai_epi32(_mm256_slli_epi32(nAi, 24), 24);
							const __m256i qya = _mm256_srai_epi32(_mm256_slli_epi32(nAi, 16), 24);
							const __m256i qxb = _mm256_srai_epi32(_mm256_slli_epi32(nBi, 24), 24);
							const __m256i qyb = _mm256_srai_epi32(_mm256_slli_epi32(nBi, 16), 24);
							const __m256i dqa = _mm256_add_epi32(_mm256_abs_epi32(_mm256_sub_epi32(qx0, qxa)),
							                                     _mm256_abs_epi32(_mm256_sub_epi32(qy0, qya)));
							const __m256i dqb = _mm256_add_epi32(_mm256_abs_epi32(_mm256_sub_epi32(qx0, qxb)),
							                                     _mm256_abs_epi32(_mm256_sub_epi32(qy0, qyb)));
							const __m256  normA = _mm256_mul_ps(
								_mm256_cvtepi32_ps(_mm256_max_epi32(dqa, dqb)), vInv127);
							// sky on this axis → contribute exactly 2.0 (silhouette)
							const __m256  contrib = _mm256_blendv_ps(_mm256_add_ps(depthA, normA), vTwo, axisSky);
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
			}
	});
	for (int n = numTilesX * numTilesY, k = 0; k < n; ++k) renderns::tileDone.acquire();

	g_aaLastMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - t0).count();
	if (std::getenv("FDS_AA_PROF")) {
		static int c = 0;
		if ((c++ & 63) == 0)
			std::fprintf(stderr, "[aa] %dx%d str=%.2f: %.3f ms\n", W, H, strength, g_aaLastMs);
	}
}
