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
#include <atomic>
#include <thread>
#include <algorithm>

namespace TailProf {

inline bool enabled() {
	static const bool e = (std::getenv("FDS_TAIL_PROF") != nullptr);
	return e;
}

inline long long nowNs() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Pool-worker count estimate (mirrors the ThreadPool cap min(16, cores-2)).
// FDS_THREADS overrides the real pool; for the idle metric this is a close
// approximation of the divisor. Used only for the printed "ideal" / "idle".
inline int workers() {
	static const int w = [] {
		if (const char* e = std::getenv("FDS_THREADS")) { int n = std::atoi(e); if (n > 0) return n; }
		unsigned hc = std::thread::hardware_concurrency();
		return (int)std::min(16u, hc > 2 ? hc - 2 : 1u);
	}();
	return w;
}

// Per-wave busy accumulator (sum of tile work durations, ns). The tile lambda
// adds BEFORE its tileDone.release(), and the drain reads AFTER all permits are
// acquired, so the read sees every add — race-free (no lambda-dtor-vs-drain
// race). One global; waves are sequential, drain resets it.
inline std::atomic<long long>& busyAcc() {
	static std::atomic<long long> b{0};
	return b;
}
inline void addBusy(long long startNs) {
	if (enabled()) busyAcc().fetch_add(nowNs() - startNs, std::memory_order_relaxed);
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
	const double wallMs = std::chrono::duration<double, std::milli>(tLast - t0).count();
	(void)tFirst;
	// Real reclaimable idle = wall − busy/W (busy = Σ tile work; only waves that
	// call addBusy() before their release report it — race-free, see busyAcc()).
	// Waves without addBusy show busy=0 → idle prints as the wall (ignore those).
	const double busyMs = busyAcc().exchange(0, std::memory_order_relaxed) / 1e6;
	const double idealMs = busyMs / (double)workers();
	const double idleMs  = wallMs - idealMs;

	struct Acc { double wall = 0, busy = 0, idle = 0; int frames = 0, lastN = 0; };
	static std::map<std::string, Acc> accs;     // tick-thread only
	Acc& a = accs[wave];
	a.wall += wallMs; a.busy += busyMs; a.idle += idleMs; a.lastN = n; ++a.frames;
	if (a.frames >= 60) {
		const double f = (double)a.frames;
		if (a.busy > 0.0)
			// effPar = busy/wall = avg workers kept busy = effective parallelism.
			// effPar ≈ pool size → fully utilized (no reclaimable wave-tail idle);
			// effPar ≪ pool → imbalanced tail worth fusing. (W-independent.)
			std::fprintf(stderr,
				"[TAIL-PROF] %-12s wall=%6.2fms  busy=%7.2fms  effPar=%5.1f  (%2d tiles)\n",
				wave, a.wall / f, a.busy / f, (a.busy / f) / (a.wall / f), a.lastN);
		else
			std::fprintf(stderr,
				"[TAIL-PROF] %-12s wall=%6.2fms  (busy not instrumented)  (%2d tiles)\n",
				wave, a.wall / f, a.lastN);
		a.wall = a.busy = a.idle = 0; a.frames = 0;
	}
}

}  // namespace TailProf
