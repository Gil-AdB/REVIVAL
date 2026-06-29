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
#include <semaphore>

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

	for (int tj = 0; tj < numTilesY; ++tj) {
		for (int ti = 0; ti < numTilesX; ++ti) {
			const int x1 = ti * tsx, x2 = std::min(x1 + tsx, W);
			const int y1 = tj * tsy, y2 = std::min(y1 + tsy, H);
			ThreadPool::instance().enqueue([=]() {
				for (int y = y1; y < y2; ++y) {
					if (y == 0 || y == H - 1) continue;            // need ±W neighbours
					for (int x = x1; x < x2; ++x) {
						if (x == 0 || x == W - 1) continue;        // need ±1 neighbours
						const size_t i = size_t(y) * size_t(W) + size_t(x);
						const word ze = zEnc[i];
						if (ze == 0) continue;                     // sky: leave as-is
						const float z0 = float(0xFF80 - ze) * invZScale;
						// Normal-edge from the RAW packed oct code — no decode.
						// The u16 is (int8 octX) | (int8 octY)<<8; adjacent normals
						// have adjacent oct codes, so |Δqx|+|Δqy| is a linear angle
						// proxy (~127 L1 ≈ 90° crease → scale 1/127 ≈ old 1-N·N).
						// Octahedral fold seams can spike Δ for similar normals — a
						// harmless false-positive edge (slight blend), never a miss.
						const meka::u16 p0 = nrm[i];
						const int qx0 = int(int8_t(p0 & 0xff));
						const int qy0 = int(int8_t((p0 >> 8) & 0xff));

						// 4-neighbour edge metric: relative depth jump + normal
						// divergence. Each neighbour adds 0..~2; averaged below.
						const int off[4] = { -1, +1, -W, +W };
						float edge = 0.0f;
						for (int d = 0; d < 4; ++d) {
							const size_t j = i + size_t(off[d]);
							const word zn = zEnc[j];
							if (zn == 0) { edge += 1.0f; continue; }   // silhouette vs sky
							const float zj = float(0xFF80 - zn) * invZScale;
							const float denom = z0 > 1e-3f ? z0 : 1e-3f;
							const float zrel  = std::fabs(z0 - zj) / denom;
							edge += zrel > 0.04f ? 1.0f : zrel * 25.0f;  // depth edge
							const meka::u16 pj = nrm[j];
							const int dq = std::abs(qx0 - int(int8_t(pj & 0xff)))
							             + std::abs(qy0 - int(int8_t((pj >> 8) & 0xff)));
							edge += float(dq) * (1.0f / 127.0f);         // normal edge
						}
						edge *= 0.25f;
						if (edge < 0.05f) continue;                    // interior: untouched
						float w = edge > 1.0f ? 1.0f : edge;
						w *= strength;

						// 5-tap cross blend of the stable snapshot, per channel.
						const dword c  = src[i];
						const dword cl = src[i - 1],     cr = src[i + 1];
						const dword cu = src[i - size_t(W)], cd = src[i + size_t(W)];
						int ob[3];
						for (int k = 0; k < 3; ++k) {
							const int s = k * 8;
							const float ctr = float((c  >> s) & 0xFF);
							const float avg = (ctr
							                 + float((cl >> s) & 0xFF)
							                 + float((cr >> s) & 0xFF)
							                 + float((cu >> s) & 0xFF)
							                 + float((cd >> s) & 0xFF)) * 0.2f;
							int v = int(ctr + (avg - ctr) * w + 0.5f);
							if (v < 0) v = 0; else if (v > 255) v = 255;
							ob[k] = v;
						}
						out[i] = dword(ob[0]) | (dword(ob[1]) << 8) | (dword(ob[2]) << 16)
						       | (c & 0xFF000000u);
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
