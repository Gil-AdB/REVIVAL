#pragma once

// Shared procedural-water machinery, factored out of CITY.CPP so chase (which
// has the same water.lwo planar mirror) can use it too.
//
//   - a small SEAMLESS tiling wave-slope field (built once from integer-
//     wavevector sines), sampled as two scrolling layers → the shared field
//     that drives BOTH the reflection-displacement ripple and the specular
//     glints, so they ripple coherently;
//   - a procedural caustic-network texture (Worley cell edges), warped by the
//     same field and modulated over the water;
//   - the screen-space post-pass that ray-casts the water plane per pixel and
//     lays the caustics + view-dependent specular glints onto the framebuffer.
//
// The field + texture are global (one set, scene-independent). The glint pass
// takes the calling scene's water PLANE (height + extent), and reads the engine
// view/framebuffer globals (View, VPage, ZPage16, CurScene->FZP, ...), so the
// caller must have SetCurrentScene + the camera globals live before calling.

#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>

#include <Threads.h>
#include <Base/FDS_VARS.H>
#include <RENDER/TailProf.h>

namespace pwater {

// ───────── Band dispatch for the two full-screen water passes ─────────
// Both glint passes scan every row of the framebuffer, but the rows are NOT
// equal work: everything above the horizon is a ~10-flop reject and everything
// below is a full wave-slope + caustic + specular evaluation. The original
// dispatch handed each worker one CONTIGUOUS block of rows, which is the worst
// possible split of that gradient — the top workers finish on rejects while the
// bottom workers carry the whole pass. This hands out SMALL contiguous row
// chunks from a shared atomic cursor instead: workers that drew cheap rows come
// back for more, so the tail is one chunk long instead of one band long. Rows
// are independent (each writes only its own VPage row), so this is bit-exact
// for any scheduling order.
//
// Also reports thrsum/effPar for the phase — without it the water rows were the
// only phases in the tree with no parallelism column, which is exactly the
// column that shows this imbalance.
template <class Band>
inline void runRowBands(const char* phase, const Band& band) {
	auto& tp = ThreadPool::instance();
	const int nT = (int)tp.size();
	if (nT < 2 || YRes < 64) { band(0, YRes); return; }
	constexpr int kChunk = 8;                       // rows per grab
	const TailProf::Stamp _st(phase);
	auto cursor    = std::make_shared<std::atomic<int>>(0);
	auto remaining = std::make_shared<std::atomic<int>>(0);
	for (int i = 0; i < nT; ++i) {
		remaining->fetch_add(1, std::memory_order_relaxed);
		tp.enqueue([band, cursor, remaining]() {
			const long long _tp = TailProf::nowNs();
			for (;;) {
				const int y0 = cursor->fetch_add(kChunk, std::memory_order_relaxed);
				if (y0 >= YRes) break;
				band(y0, std::min(y0 + kChunk, YRes));
			}
			TailProf::addBusy(_tp);
			remaining->fetch_sub(1, std::memory_order_release);
		});
	}
	while (remaining->load(std::memory_order_acquire) != 0) std::this_thread::yield();
	TailProf::drainSpun(phase, nT, _st, /*depth=*/3);
}


// Instrument (--water_census): classify every pixel the named pass scans into
// reject/live buckets, accumulate, print a [WCENSUS] table at exit. Separate
// classification-only sweep — the shading loops are untouched.
void Census(const char* tag, float waterY, bool useOcclusion, bool useFarCut);

// Build the wave normal-field + caustic texture. Call once per scene init.
void BuildField();

// Wave slope (nx,nz) at world XZ for the shared field — used by the reflection
// dispMap as well as the glint pass. `scale` = water_bump_scale; `t` = animated
// time (Timer * 0.02 * ripple_speed).
void WaveSlope(float wx, float wz, float t, float scale, float& bnx, float& bnz);

// Screen-space caustic-texture + specular-glint post-pass over VPage. `waterY`
// is the water plane height; [minX,maxX]/[minZ,maxZ] are the plane extent (used
// only as a "water set up yet" early-out). No-op when water_bump is off / extent
// unset / no View. Row-parallel across the thread pool.
void RenderGlints(float waterY, float minX, float maxX, float minZ, float maxZ);
// water_variation ON variant (chase): same pass with a low-frequency swell + a
// 3rd ripple octave on the wave field and multi-scale caustics so the sea reads
// varied, not a uniform repeating field. Separate fn (not a flag branch inside
// RenderGlints) so the default path stays byte-identical. Dispatched at the call
// site on FeatureFlags::water_variation().
void RenderGlintsVaried(float waterY, float minX, float maxX, float minZ, float maxZ);

// Caustic-cell modulation factors at world XZ — the EXACT formula of
// RenderGlints' texMix block (keep in lockstep; not shared with that hot loop
// so the screen pass keeps its single wave-slope evaluation per pixel), for
// callers that shade water OUTSIDE the screen pass (the city env-bake
// procedural water re-shade). On return the caller applies:
//     B = B*mod + blueAdd;  G = G*mod + blueAdd*0.40;  R = R*mod + blueAdd*0.08
// `t` is the wave clock, `scale` = water_bump_scale, texMix/texScale/texWarp =
// the water_albedo_mix/water_tex_scale/water_tex_warp values resolved by the
// caller, flowU/flowV = the caustic UV translation (0 for a frozen bake).
// Returns false (factors untouched) when texMix<=0 or BuildField hasn't run.
bool CausticModulation(float wx, float wz, float t, float scale,
                       float texMix, float texScale, float texWarp,
                       float flowU, float flowV,
                       float& mod, float& blueAdd);

}  // namespace pwater
