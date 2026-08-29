#pragma once
// ── TBR interior attribution (PERF_STATE 00u) ───────────────────────────────
// city's `TBR-render` measured 6.9-8.0 ms while its three DECLARED children
// (xpar-clear / xpar-raster / xpar-composite) all read 0.000. Those accumulators
// are written only inside RENDER.CPP's `unifiedTbr=false` LEGACY block; city
// runs the UNIFIED path, so on that path they are structurally dead and the
// row's whole interior was unattributed.
//
// The outer split (FILLERS.CPP, TBR_Render) put 97.6 % of the interior in ONE
// callee: RenderXparClumpInStrip. This header is the second layer, splitting
// that callee into the three phases the dead legacy names already describe.
//
// WHY THREAD-SUMMED NANOSECONDS AND NOT COUNTERS: TailProf::hwRead() is
// proc_pid_rusage(getpid()) -- PROCESS-WIDE -- so a nested per-worker
// instruction/cycle read is meaningless here (concurrent strips interleave).
// Per-block Ginstr/Gcyc has to come from ablation differencing at row level.
// What is sound per block, and free when the profiler is off, is core-ns.
//
// INSTRUMENT ONLY. Changes no pixel. The accumulators live here rather than in
// DeferredSurfaceKernel.cpp so that file takes one #include and five timer
// lines and nothing else -- it is owned by the greets lighting round.
#include <atomic>
#include <cstdint>
#include "RENDER/TailProf.h"

namespace tbrsplit {

inline std::atomic<uint64_t>& clearNs() { static std::atomic<uint64_t> v{0}; return v; }
inline std::atomic<uint64_t>& rasterNs(){ static std::atomic<uint64_t> v{0}; return v; }
inline std::atomic<uint64_t>& compNs()  { static std::atomic<uint64_t> v{0}; return v; }
inline std::atomic<uint64_t>& rasterFaces() { static std::atomic<uint64_t> v{0}; return v; }

// Adds its own lifetime to an atomic on destruction, so an early `return` out
// of the timed region still books exactly once.
struct AccNs {
	std::atomic<uint64_t>* acc; long long t0; bool on;
	explicit AccNs(std::atomic<uint64_t>& a)
		: acc(&a), t0(TailProf::enabled() ? TailProf::nowNs() : 0),
		  on(TailProf::enabled()) {}
	~AccNs() {
		if (on) acc->fetch_add(uint64_t(TailProf::nowNs() - t0),
		                       std::memory_order_relaxed);
	}
};

inline void resetAll() {
	clearNs().store(0, std::memory_order_relaxed);
	rasterNs().store(0, std::memory_order_relaxed);
	compNs().store(0, std::memory_order_relaxed);
	rasterFaces().store(0, std::memory_order_relaxed);
}

// Book the three phases as CORE-ms (thread-summed). They sum to tbr-xparflush,
// and are booked at depth 3 because the table prints d=0..3 only (TailProf.h).
// not to the parent TBR-render WALL row.
inline void book() {
	if (!TailProf::enabled()) return;
	TailProf::addMs("xflush-composite", double(compNs().load(std::memory_order_relaxed))   / 1e6, 3);
	TailProf::addMs("xflush-raster",    double(rasterNs().load(std::memory_order_relaxed)) / 1e6, 3);
	TailProf::addMs("xflush-clear",     double(clearNs().load(std::memory_order_relaxed))  / 1e6, 3);
}

}  // namespace tbrsplit
