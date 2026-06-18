#pragma once
// Phase-1 barrier-tail instrumentation for the render-DAG campaign
// (docs/RENDER_DAG_SCOPING.md). Gated on FDS_TAIL_PROF=1; zero cost otherwise.
//
// Wraps a parallel wave's `tileDone`/`shadowDone` drain. The orchestrator
// acquires permits in completion order, so the time from the FIRST permit to
// the LAST permit ≈ the spread of tile-completion times = the barrier TAIL
// (the idle the early-finishing workers eat waiting for the slowest tile).
// `wall` is the whole drain (first tile's compute + the tail). A fat `tail`
// relative to `wall` = a wave worth fusing (load-imbalanced). Prints per-wave
// averages to stderr every 60 frames.
//
// No per-task cost: only the drain loop is touched, and only when enabled.
// All drains run on the tick thread (sequential), so the static accumulator
// needs no lock.
#include <semaphore>
#include <climits>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

namespace TailProf {

inline bool enabled() {
	static const bool e = (std::getenv("FDS_TAIL_PROF") != nullptr);
	return e;
}

// Drain `n` permits from `sem`. Identical to a plain `for(n) sem.acquire()`
// loop when disabled; when enabled, measures wall + tail and prints per wave.
inline void drain(std::counting_semaphore<INT_MAX>& sem, int n, const char* wave) {
	if (!enabled() || n <= 0) {
		for (int i = 0; i < n; ++i) sem.acquire();
		return;
	}
	using clk = std::chrono::steady_clock;
	const auto t0 = clk::now();
	sem.acquire();                              // block until the first tile is done
	const auto tFirst = clk::now();
	for (int i = 1; i < n; ++i) sem.acquire();  // drain the rest as they complete
	const auto tLast = clk::now();
	const double tailMs = std::chrono::duration<double, std::milli>(tLast - tFirst).count();
	const double wallMs = std::chrono::duration<double, std::milli>(tLast - t0).count();

	// NOTE: `spread` (first→last permit) is NOT reclaimable idle — with N tiles
	// on W workers, permits trickle in over ~(N−W)/N of the wave even when
	// perfectly balanced, so spread≈90% at N=96/W=8 regardless of imbalance.
	// Use `wall` for the per-wave ranking; true idle (= wall − busy/W) needs a
	// per-tile busy accumulator (TODO: add race-free, before the tileDone
	// release, not in a lambda dtor that races the drain).
	struct Acc { double spread = 0, wall = 0; int frames = 0, lastN = 0; };
	static std::map<std::string, Acc> accs;     // tick-thread only
	Acc& a = accs[wave];
	a.spread += tailMs; a.wall += wallMs; a.lastN = n; ++a.frames;
	if (a.frames >= 60) {
		std::fprintf(stderr,
			"[TAIL-PROF] %-12s wall=%6.2fms  spread=%6.2fms  (%2d tiles, avg/60f)\n",
			wave, a.wall / a.frames, a.spread / a.frames, a.lastN);
		a.spread = a.wall = 0; a.frames = 0;
	}
}

}  // namespace TailProf
