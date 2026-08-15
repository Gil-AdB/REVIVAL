#pragma once
// Per-phase instrumentation of the deferred frame — the "what is inside RNDR"
// instrument (docs/RENDER_DAG_SCOPING.md phase 1, extended 2026-08-08).
//
// Gated on `--deferred_prof` (FeatureFlags) or the legacy `FDS_TAIL_PROF=1`
// env; zero cost otherwise (one `if (enabled())` per phase boundary, never in a
// per-pixel or per-triangle loop).
//
// ── The two numbers, and why conflating them would be worse than nothing ────
//
//  * WALL   — elapsed time of the phase on the tick thread: for a parallel wave,
//             the drain from "tasks enqueued" to "last tile's permit acquired".
//             This is the number that adds up to the frame. Σ wall ≈ RNDR.
//  * THRSUM — Σ over tile tasks of that task's own duration ("busy"), summed at
//             the barrier. This is CORE-milliseconds, not frame-milliseconds; it
//             is ~W× larger than wall on a healthy wave. Only meaningful for
//             waves that call addBusy().
//  * effPar = THRSUM / WALL = average workers kept busy = effective parallelism.
//             effPar ≈ pool size → the wave is compute-bound and balanced.
//             effPar ≪ pool     → load-imbalanced tail; the wall is mostly idle
//                                 workers waiting on the slowest tile.
//
// ── Attribution: per FRAME, and MAIN-view separated from offscreen ──────────
//
// The old form printed a per-INVOCATION average every 60 invocations of each
// name. That silently lied on greets: `renderFrame` runs many times per tick
// (mirror-RTT bakes, shard bakes, env/SH probe cube faces), so a 60-sample
// window over the `gbuffer` drain mixed one 1920×1080 main pass with dozens of
// 64² offscreen passes and reported their MEAN — an order of magnitude below the
// main-view cost. Every number here is therefore normalised by MAIN-VIEW FRAMES
// (TailProf::NewFrame(), called once per scene tick from FrameProfiler), and
// each accumulator keeps MAIN and OFFSCREEN buckets apart. `calls/f` is printed
// so a phase that runs more than once per frame is visible rather than averaged
// away.
//
// A pass counts as MAIN only if it runs on the tick thread inside a PassScope
// that declared itself main. Anything on a pool worker (the inline-dispatch
// shard/probe renders) lands in the offscreen bucket by construction.
//
// ── Report ─────────────────────────────────────────────────────────────────
//
// `--deferred_prof=1` accumulates for the whole process and prints ONE table at
// exit (atexit, so --snapshot and --bench both get it with no plumbing).
// Legacy `FDS_TAIL_PROF=1` keeps the old rolling every-60-invocations prints.
// `depth` orders the table and drives the accounting line:
//   0 = renderFrame itself, 1 = a phase of it, 2 = detail inside a phase,
//   3 = outside renderFrame (scene tick / bake / mirror glue).
// OTHER = depth-0 wall − Σ depth-1 wall = the unattributed remainder, printed
// explicitly so the table can never quietly fail to add up.
#include <semaphore>
#include <climits>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <algorithm>

#include <Base/FeatureFlags.h>

#if defined(__APPLE__)
#include <libproc.h>
#include <unistd.h>
#include <os/signpost.h>
#endif

namespace TailProf {

// One-shot table mode (--deferred_prof) vs the legacy rolling prints
// (FDS_TAIL_PROF=1). Both imply enabled().
inline bool oneShot() {
	static const bool o = fds::FeatureFlags::deferred_prof() > 0;
	return o;
}

inline bool enabled() {
	// Cached: read once, on the first phase boundary of the first frame — long
	// after FeatureFlags::parseArgs(). Never a getenv in a hot loop.
	static const bool e = (std::getenv("FDS_TAIL_PROF") != nullptr) || oneShot();
	return e;
}

inline long long nowNs() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ─── --hw_prof: hardware counters + os_signpost on the SAME phase boundaries ─
//
// WHY THIS AND NOT INSTRUMENTS: `xctrace` ships with Xcode.app, which is not
// installed on this box (Command Line Tools only), and configuring the PMU
// directly (the `kpc.*` sysctls behind kperf.framework) requires root — writing
// `kpc.counting` returns EPERM. What IS reachable unprivileged is the kernel's
// per-task PMU accounting: proc_pid_rusage(RUSAGE_INFO_V4+) exposes
// ri_instructions and ri_cycles, both hardware counters, and it is LIVE — a
// mid-flight read of a task with 4 spinning threads returns ~4 core-seconds per
// wall-second, so a read pair around a parallel wave captures every worker's
// work, not just the tick thread's. That is exactly the attribution the phase
// table needs. Cache-miss counters are NOT reachable this way and no
// unprivileged substitute exists; see docs/HW_PROFILING.md for what IPC can and
// cannot stand in for.
//
// COST, measured with 12 threads spinning: proc_pid_rusage = 808 ns/call,
// thread_selfcounts = 256 ns/call. At ~40 phase boundaries x 2 reads that is
// 0.065 ms/frame, ~0.07% of a 90 ms greets frame. Default OFF = not one read.
//
// The counters are TASK-wide, so a phase's delta includes anything else the
// process did concurrently. With dummy SDL drivers the only other threads are
// idle, but a phase measured while a bake runs on a pool worker will over-count;
// the `calls/f` and effPar columns are what expose that.
inline bool hwEnabled() {
	static const bool h = fds::FeatureFlags::hw_prof();
	return h;
}

// Task-wide retired instructions + core cycles. Both are PMU-derived.
inline void hwRead(unsigned long long& instr, unsigned long long& cyc) {
#if defined(__APPLE__)
	struct rusage_info_v4 ri;
	if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t*)&ri) == 0) {
		instr = ri.ri_instructions;
		cyc   = ri.ri_cycles;
		return;
	}
#endif
	instr = 0; cyc = 0;
}

#if defined(__APPLE__)
// One process-wide log handle on the Points-of-Interest category, so Instruments
// picks the intervals up on its own track if this tree is ever opened on a box
// that has Xcode. Headless, `log show --signpost` reads the same records.
inline os_log_t spLog() {
	static os_log_t l = os_log_create("com.revival.fds", OS_LOG_CATEGORY_POINTS_OF_INTEREST);
	return l;
}
// os_signpost requires the interval NAME to be a compile-time literal, and every
// phase name here is a runtime `const char*` — so all intervals share the literal
// "phase" and carry the real name as the message. Instruments groups by message;
// `log show --signpost` prints it verbatim.
inline os_signpost_id_t spBegin(const char* name) {
	if (!hwEnabled()) return OS_SIGNPOST_ID_NULL;
	os_log_t l = spLog();
	if (!os_signpost_enabled(l)) return OS_SIGNPOST_ID_NULL;
	os_signpost_id_t sid = os_signpost_id_generate(l);
	os_signpost_interval_begin(l, sid, "phase", "%{public}s", name);
	return sid;
}
inline void spEnd(os_signpost_id_t sid, const char* name) {
	if (sid == OS_SIGNPOST_ID_NULL) return;
	os_signpost_interval_end(spLog(), sid, "phase", "%{public}s", name);
}
#else
using os_signpost_id_t = unsigned long long;
inline os_signpost_id_t spBegin(const char*) { return 0; }
inline void spEnd(os_signpost_id_t, const char*) {}
#endif

// Pool-worker count. Mirrors ThreadPool::init (Threads.h:42): one worker per
// hardware thread, with FDS_THREADS clamping it DOWN. Printed as the reference
// effPar can be read against — a wave at effPar ≈ workers() is compute-bound and
// balanced, so there is no barrier-tail idle left to reclaim.
// (The old form here subtracted 2 and capped at 16, which was never what the
// pool did — it made effPar look like it exceeded the pool.)
inline int workers() {
	static const int w = [] {
		unsigned hc = std::thread::hardware_concurrency();
		if (hc < 1) hc = 1;
		if (const char* e = std::getenv("FDS_THREADS")) {
			long v = std::atol(e);
			if (v >= 1 && v < long(hc)) hc = unsigned(v);
		}
		return (int)hc;
	}();
	return w;
}

// ─── registry ──────────────────────────────────────────────────────────────
// Keyed by phase name; two buckets (0 = main view, 1 = offscreen/RTT/probe).
// Mutex-protected: most phase boundaries are tick-thread-only, but a shard /
// env-probe render runs a WHOLE renderFrame on a pool worker, so the map can be
// touched concurrently. Take rate is ~40 per frame — irrelevant, and only when
// the instrument is on.
// Four buckets: (main | offscreen) x (steady | warmup). The warmup split is not
// cosmetic — it is the difference between a measurement and a fiction. The FIRST
// main-view frame pays every lazy one-shot: the env-reflection panorama bakes,
// the SH probe, mipmap first-touch. On greets that is ~230 ms inside one frame's
// renderFrame; averaged over a 30-frame bench it read as a steady 7.4 ms/frame
// "frame-prep" phase that does not exist. --deferred_prof=<N> excludes the first
// N frames; the warmup buckets are kept (not discarded) so a run too short to
// have any steady frame still reports, loudly labelled.
// instr/cyc are populated only under --hw_prof; 0 otherwise (and 0 for phases
// booked through mark()/addMs(), which have no counter stamp to difference
// against — those rows print "-" rather than a wrong number).
struct Bucket { double wall = 0, busy = 0; long long calls = 0; double instr = 0, cyc = 0; };
struct Acc {
	Bucket    b[4];
	int       lastN = 0;      // tiles in the most recent wave
	int       depth = 2;
	bool      isWave = false;
	// Per-FRAME min over steady main-view frames (all of this phase's calls in
	// that frame summed). The mean is the wrong statistic on a loaded machine —
	// one descheduled tile inflates it — so the report carries both and the min
	// is what a min-of-arm comparison across repetitions should use.
	double    curWall = 0, curBusy = 0;
	long long curCalls = 0;
	double    minWall = 1e30, minBusy = 0;
	// Legacy rolling-print state (FDS_TAIL_PROF, non-oneShot).
	double    rollWall = 0, rollBusy = 0;
	int       rollFrames = 0;
};

// Deliberately NEVER destroyed (leaked once, at process end). The report runs
// from an atexit handler, and whether these statics outlive it depends on which
// call site happened to construct them first — which varies by scene: the
// fountain bench builds them inside Initialize_City, before the first tick
// registers the handler, and the ordinary function-local form then destroyed the
// mutex first and aborted the process with "mutex lock failed: Invalid argument"
// after a clean run. A leaked singleton removes the ordering question entirely.
inline std::mutex& regMutex() { static std::mutex* m = new std::mutex(); return *m; }
inline std::map<std::string, Acc>& registry() {
	static auto* m = new std::map<std::string, Acc>(); return *m;
}
inline std::vector<std::string>& regOrder() {
	static auto* v = new std::vector<std::string>(); return *v;
}
inline long long& frameCount() { static long long f = 0; return f; }
// Synthetic accumulator for the per-frame unattributed remainder (see
// closeFrameLocked). Not in the registry — it is derived, not recorded.
inline Acc& otherAcc() { static auto* a = new Acc(); return *a; }

// Main-view frame counter + the identity of the thread that drives it. Set by
// NewFrame(); everything else compares against it so a render on a pool worker
// can never be mistaken for the main view.
inline std::thread::id& tickThread() { static std::thread::id id; return id; }

// Per-thread "this renderFrame is the main view" flag. Defaults true so the
// scene tick's own scopes (Tick-*, StampMasks, …) attribute to main; PassScope
// clears it for offscreen passes.
inline bool& passMainFlag() { static thread_local bool m = true; return m; }

// Frames excluded from the steady-state columns. --deferred_prof=N sets it; 1
// (a bare --deferred_prof) drops just the lazy-init frame.
inline int warmupFrames() {
	static const int w = oneShot() ? std::max(1, fds::FeatureFlags::deferred_prof()) : 0;
	return w;
}

inline int bucketIdx() {
	const bool main   = passMainFlag() && std::this_thread::get_id() == tickThread();
	const bool warmup = frameCount() <= (long long)warmupFrames();
	return (main ? 0 : 1) + (warmup ? 2 : 0);
}

// Roll the frame in flight into the per-phase min and reset it. Only frames the
// phase actually ran in are candidates, so a phase that fires on some frames
// (a batched bake) doesn't get a spurious 0 min.
inline void closeFrameLocked() {
	// True per-frame unattributed remainder: depth-0 minus Σ depth-1, computed
	// WITHIN the frame. (The report's residual-of-minima is not this — each
	// phase's min comes from its own best frame.) This row is the instrument's
	// own credibility gate: near zero = the frame is fully attributed and the
	// per-phase numbers can be trusted; several ms = the tick thread lost time
	// somewhere no scope covers (on a loaded machine, descheduled mid-phase),
	// and the breakdown should be re-taken.
	double d0 = 0.0, d1 = 0.0;
	bool   any = false;
	for (auto& kv : registry()) {
		const Acc& a = kv.second;
		if (a.curCalls == 0) continue;
		any = true;
		if (a.depth == 0) d0 += a.curWall;
		else if (a.depth == 1) d1 += a.curWall;
	}
	for (auto& kv : registry()) {
		Acc& a = kv.second;
		if (a.curCalls > 0 && a.curWall < a.minWall) {
			a.minWall = a.curWall;
			a.minBusy = a.curBusy;
		}
		a.curWall = a.curBusy = 0; a.curCalls = 0;
	}
	if (any && d0 > 0.0) {
		Acc& o = otherAcc();
		const double v = d0 - d1;
		o.b[0].wall += v; ++o.b[0].calls;
		if (v < o.minWall) o.minWall = v;
	}
}

inline void Report(const char* label);
inline void reportAtExit() { if (enabled() && oneShot()) Report("process exit"); }

// Called once per SCENE TICK (FrameProfiler::beginFrame) — the normaliser for
// every ms/frame figure in the report.
inline void NewFrame() {
	if (!enabled()) return;
	tickThread() = std::this_thread::get_id();
	if (oneShot()) {
		static const bool once = [] {
			(void)registry(); (void)regOrder();   // construct BEFORE we register
			std::atexit(&reportAtExit);           // → our handler runs first
			return true;
		}();
		(void)once;
	}
	std::lock_guard<std::mutex> lk(regMutex());
	closeFrameLocked();
	++frameCount();
}

// Record a completed phase. wallNs is elapsed; busyNs is Σ per-task time (0 =
// not instrumented). Prints the legacy rolling line when not in one-shot mode.
inline void record(const char* name, long long wallNs, double busyMs,
                   int depth, bool isWave, int nTiles,
                   double dInstr = 0.0, double dCyc = 0.0) {
	const double wallMs = double(wallNs) / 1e6;
	bool     print = false;
	double   pw = 0, pb = 0; int pf = 0, pn = 0;
	{
		std::lock_guard<std::mutex> lk(regMutex());
		auto& m = registry();
		auto  it = m.find(name);
		if (it == m.end()) {
			it = m.emplace(std::string(name), Acc{}).first;
			regOrder().push_back(name);
		}
		Acc& a = it->second;
		a.depth  = depth;
		a.isWave = a.isWave || isWave;
		if (nTiles) a.lastN = nTiles;
		const int bi = bucketIdx();
		Bucket& b = a.b[bi];
		b.wall += wallMs;
		b.busy += busyMs;
		b.instr += dInstr;
		b.cyc   += dCyc;
		++b.calls;
		if (bi == 0) {   // steady main-view: feeds the per-frame min
			a.curWall += wallMs; a.curBusy += busyMs; ++a.curCalls;
		}
		if (!oneShot()) {
			a.rollWall += wallMs; a.rollBusy += busyMs;
			if (++a.rollFrames >= 60) {
				print = true; pw = a.rollWall; pb = a.rollBusy;
				pf = a.rollFrames; pn = a.lastN;
				a.rollWall = a.rollBusy = 0; a.rollFrames = 0;
			}
		}
	}
	if (!print) return;
	const double f = double(pf);
	if (isWave) {
		if (pb > 0.0)
			std::fprintf(stderr,
				"[TAIL-PROF] %-12s wall=%6.2fms  busy=%7.2fms  effPar=%5.1f  (%2d tiles)\n",
				name, pw / f, pb / f, (pb / f) / (pw / f), pn);
		else
			std::fprintf(stderr,
				"[TAIL-PROF] %-12s wall=%6.2fms  (busy not instrumented)  (%2d tiles)\n",
				name, pw / f, pn);
	} else {
		std::fprintf(stderr, "[TAIL-PROF] serial %-18s %7.3fms/f\n", name, pw / f);
	}
}

// ─── per-wave busy (thread-sum) accumulator ────────────────────────────────
// The tile task adds its own duration here; the drain reads it after every
// permit is in. Two counters: the ns sum and the number of adds, so the drain
// can wait for stragglers whose add lands just AFTER their tileDone.release()
// (unavoidable for kernels that release from inside — the lighting and G-buffer
// tile kernels both do). The wait is bounded, so an uninstrumented wave (0 adds)
// or a partially-instrumented one can never hang.
inline std::atomic<long long>& busyAcc() { static std::atomic<long long> b{0}; return b; }
inline std::atomic<int>&       busyN()   { static std::atomic<int>       n{0}; return n; }
inline void addBusy(long long startNs) {
	if (!enabled()) return;
	busyAcc().fetch_add(nowNs() - startNs, std::memory_order_relaxed);
	busyN().fetch_add(1, std::memory_order_release);
}

// RAII timer for a phase / serial glue step. depth: see the header comment.
// Under --hw_prof it additionally brackets the phase with a task-wide counter
// pair and an os_signpost interval. Both are exact here: the scope's lifetime IS
// the phase (unlike a wave, whose work starts at the dispatch, not at the drain).
struct ScopeTimer {
	const char*        name;
	int                depth;
	long long          t0;
	unsigned long long i0 = 0, c0 = 0;
	os_signpost_id_t   sid = 0;
	explicit ScopeTimer(const char* n, int d = 2)
		: name(n), depth(d), t0(enabled() ? nowNs() : 0) {
		if (hwEnabled()) { hwRead(i0, c0); sid = spBegin(name); }
	}
	~ScopeTimer() {
		if (!enabled()) return;
		double di = 0, dc = 0;
		if (hwEnabled()) {
			unsigned long long i1 = 0, c1 = 0;
			hwRead(i1, c1);
			di = double(i1 - i0); dc = double(c1 - c0);
			spEnd(sid, name);
		}
		record(name, nowNs() - t0, 0.0, depth, false, 0, di, dc);
	}
};

// A phase-start stamp for a PARALLEL WAVE. The wave's work begins at the
// dispatch, not at the drain — TailProf.h's drain() contract already says the ns
// stamp must be taken before the enqueue, and the counter stamp and the signpost
// interval have exactly the same requirement, so all three are taken together
// here. Implicitly converts to `long long` so the existing `mark(name, stamp)`
// call sites keep compiling unchanged.
struct Stamp {
	long long          ns    = 0;
	unsigned long long instr = 0, cyc = 0;
	os_signpost_id_t   sid   = 0;
	const char*        name  = nullptr;
	Stamp() = default;
	explicit Stamp(const char* n) : name(n) {
		if (!enabled()) return;
		ns = nowNs();
		if (hwEnabled()) { hwRead(instr, cyc); sid = spBegin(n); }
	}
	operator long long() const { return ns; }
};

// Manual serial-region mark (for regions that can't be a clean RAII scope
// because they declare locals used later). Call with the start timestamp.
inline void mark(const char* name, long long startNs, int depth = 2) {
	if (!enabled()) return;
	record(name, nowNs() - startNs, 0.0, depth, false, 0);
}

// Book an already-measured duration (ms) — for a phase whose cost is summed
// across sub-steps by the caller (the transparent peel's per-batch clear /
// raster / composite split) rather than bracketed by one scope.
inline void addMs(const char* name, double ms, int depth = 2) {
	if (!enabled()) return;
	record(name, (long long)(ms * 1e6), 0.0, depth, false, 0);
}

// Declares whether the renderFrame we are entering is the MAIN view. Restores
// the previous value on scope exit so nested offscreen renders (mirror RTT, env
// probe, shard bake) can't leave the flag flipped for the rest of the frame.
struct PassScope {
	bool prev;
	explicit PassScope(bool isMain) : prev(passMainFlag()) {
		if (enabled()) passMainFlag() = prev && isMain;
	}
	~PassScope() { if (enabled()) passMainFlag() = prev; }
};

// Drain `n` permits from `sem`. Identical to a plain `for(n) sem.acquire()`
// loop when disabled; when enabled, measures the wave's wall + thread-sum.
//
// `startNs` MUST be taken BEFORE the dispatch that enqueues the tasks. Timing
// only the acquire loop measures a fiction: the pool starts consuming tiles the
// instant the first task is queued, so any time the tick thread spends inside
// the enqueue — or descheduled during it, which on a loaded machine is
// milliseconds — is work the wave did while unobserved, and the drain then
// returns almost immediately. That produced 0.3 ms readings for a G-buffer wave
// carrying 67 core-ms of work. Passing 0 keeps the old (wrong) behaviour and is
// only there so an un-updated call site still compiles.
inline void drain(std::counting_semaphore<INT_MAX>& sem, int n, const char* wave,
                  int depth = 2, long long startNs = 0, const Stamp* st = nullptr) {
	if (!enabled() || n <= 0) {
		for (int i = 0; i < n; ++i) sem.acquire();
		return;
	}
	const long long t0 = startNs ? startNs : nowNs();
	for (int i = 0; i < n; ++i) sem.acquire();
	const long long tLast = nowNs();
	// Straggler settle: a task whose addBusy lands just after its release. Wait
	// only while adds are still arriving, and never more than 200 us.
	if (busyN().load(std::memory_order_acquire) > 0) {
		while (busyN().load(std::memory_order_acquire) < n &&
		       nowNs() - tLast < 200000)
			std::this_thread::yield();
	}
	busyN().store(0, std::memory_order_relaxed);
	const double busyMs = busyAcc().exchange(0, std::memory_order_relaxed) / 1e6;
	double di = 0, dc = 0;
	if (hwEnabled() && st) {
		unsigned long long i1 = 0, c1 = 0;
		hwRead(i1, c1);
		di = double(i1 - st->instr); dc = double(c1 - st->cyc);
		spEnd(st->sid, wave);
	}
	record(wave, tLast - t0, busyMs, depth, true, n, di, dc);
}

// Wave form: the stamp carries the pre-dispatch ns AND counter reads, so the
// wave's instructions/cycles cover every worker from the enqueue onward.
inline void drain(std::counting_semaphore<INT_MAX>& sem, int n, const char* wave,
                  int depth, const Stamp& st) {
	drain(sem, n, wave, depth, st.ns, &st);
}

// Same bookkeeping for a wave that is joined by SPINNING on an atomic counter
// rather than acquiring a semaphore (the water passes, the dispMap resample —
// pool fan-outs that predate the semaphore drain). The caller has already
// joined; this only closes the counters and books the row, so the wave gets the
// thrsum/effPar columns instead of a blank.
inline void drainSpun(const char* wave, int n, const Stamp& st, int depth = 2) {
	if (!enabled() || n <= 0) return;
	const long long tLast = nowNs();
	if (busyN().load(std::memory_order_acquire) > 0) {
		while (busyN().load(std::memory_order_acquire) < n &&
		       nowNs() - tLast < 200000)
			std::this_thread::yield();
	}
	busyN().store(0, std::memory_order_relaxed);
	const double busyMs = busyAcc().exchange(0, std::memory_order_relaxed) / 1e6;
	double di = 0, dc = 0;
	if (hwEnabled()) {
		unsigned long long i1 = 0, c1 = 0;
		hwRead(i1, c1);
		di = double(i1 - st.instr); dc = double(c1 - st.cyc);
		spEnd(st.sid, wave);
	}
	record(wave, tLast - st.ns, busyMs, depth, true, n, di, dc);
}

// ─── the one-shot table ────────────────────────────────────────────────────
inline void Report(const char* label) {
	if (!enabled()) return;
	std::lock_guard<std::mutex> lk(regMutex());
	const long long nf     = frameCount();
	const long long warm   = std::min<long long>(nf, warmupFrames());
	const long long steady = nf - warm;
	if (nf <= 0) return;
	// No steady frame (a --snapshot pose, or a bench shorter than the warmup):
	// report the warmup buckets instead of nothing, and say so — those numbers
	// include every lazy one-shot the first frame paid.
	const bool useWarm = (steady <= 0);
	const int  bMain   = useWarm ? 2 : 0;
	const int  bOff    = useWarm ? 3 : 1;
	const double f     = double(useWarm ? warm : steady);
	std::fprintf(stderr,
		"\n[DPROF] ==== deferred phase breakdown (%s) ====\n", label);
	if (useWarm)
		std::fprintf(stderr,
			"[DPROF] *** WARMUP FRAMES ONLY (%lld frame(s), warmup=%d) — these include the\n"
			"[DPROF] *** one-shot env/SH probe bakes and every first-touch. NOT steady state.\n"
			"[DPROF] *** Use --bench=scene@scene=<s>,t=<t>,iters=N for a usable breakdown.\n",
			warm, warmupFrames());
	std::fprintf(stderr,
		"[DPROF] steady main-view frames=%lld (warmup %lld excluded)   pool workers=%d"
		"   all times ms PER FRAME\n", steady, warm, workers());
	if (hwEnabled())
		std::fprintf(stderr,
			"[DPROF] %-24s %7s %9s %9s %9s %7s | %10s %8s %8s\n",
			"phase", "calls/f", "wall_min", "wall_avg", "thrsum_avg", "effPar",
			"Ginstr/f", "Gcyc/f", "IPC");
	else
		std::fprintf(stderr,
			"[DPROF] %-24s %7s %9s %9s %9s %7s | %7s %9s\n",
			"phase", "calls/f", "wall_min", "wall_avg", "thrsum_avg", "effPar",
			"off c/f", "off wall");

	// Close the frame in flight so its samples reach the min (nothing calls
	// NewFrame after the last tick).
	closeFrameLocked();

	double sumDepth1 = 0.0, depth0 = 0.0, sumMin1 = 0.0, min0 = 0.0;
	auto& m = registry();
	// depth order, then first-seen order inside a depth — pipeline order.
	for (int d = 0; d <= 3; ++d) {
		for (const auto& nm : regOrder()) {
			auto it = m.find(nm);
			if (it == m.end() || it->second.depth != d) continue;
			const Acc& a = it->second;
			const Bucket& mainB = a.b[bMain];
			const Bucket& offB  = a.b[bOff];
			if (mainB.calls == 0 && offB.calls == 0) continue;
			const double mn = (a.minWall < 1e29) ? a.minWall : 0.0;
			if (d == 0) { depth0 += mainB.wall; min0 += mn; }
			if (d == 1) { sumDepth1 += mainB.wall; sumMin1 += mn; }
			char indent[16];
			const int ind = (d == 3) ? 0 : d * 2;
			for (int i = 0; i < ind; ++i) indent[i] = ' ';
			indent[ind] = 0;
			const double effPar = (mainB.wall > 0.0 && mainB.busy > 0.0)
			                      ? mainB.busy / mainB.wall : 0.0;
			std::fprintf(stderr, "[DPROF] %s%-*s %7.2f %9.3f %9.3f ",
				indent, 24 - ind, nm.c_str(),
				double(mainB.calls) / f, mn, mainB.wall / f);
			if (mainB.busy > 0.0) std::fprintf(stderr, "%9.3f %7.1f", mainB.busy / f, effPar);
			else                  std::fprintf(stderr, "%9s %7s", "-", "-");
			if (hwEnabled()) {
				// Ratios (IPC) are the load-robust number here: a descheduled
				// worker costs wall time but retires no instructions and burns
				// no cycles, so instr/cyc is far steadier under machine load
				// than any ms column. Blank for phases with no counter stamp.
				if (mainB.cyc > 0.0)
					std::fprintf(stderr, " | %10.3f %8.3f %8.3f",
						mainB.instr / f / 1e9, mainB.cyc / f / 1e9,
						mainB.instr / mainB.cyc);
				else
					std::fprintf(stderr, " | %10s %8s %8s", "-", "-", "-");
			} else if (offB.calls) {
				std::fprintf(stderr, " | %7.2f %9.3f",
					double(offB.calls) / f, offB.wall / f);
			}
			std::fprintf(stderr, "\n");
		}
		if (d == 1 && depth0 > 0.0) {
			const Acc& o = otherAcc();
			std::fprintf(stderr,
				"[DPROF]   %-24s %7s %9.3f %9.3f %9s %7s   (per-frame renderFrame - Σ depth-1)\n",
				"OTHER (unattributed)", "-",
				(o.minWall < 1e29) ? o.minWall : (min0 - sumMin1),
				(depth0 - sumDepth1) / f, "-", "-");
		}
	}
	std::fprintf(stderr,
		"[DPROF] depth 0 = renderFrame, 1 = its phases, 2 = detail inside a phase,"
		" 3 = outside renderFrame.\n"
		"[DPROF] wall = ELAPSED on the tick thread (sums to the frame); thrsum ="
		" Σ tile-task time = CORE-ms, ~W× wall; effPar = thrsum/wall = workers kept busy.\n"
		"[DPROF] wall_min = min over steady frames (use THIS for min-of-arm A/B);"
		" wall_avg = mean, inflated by machine load.\n"
		"[DPROF] Each phase's min is taken over its OWN best frame, so Σ mins ≤ the"
		" frame min; OTHER's min column is that residual, not a measured minimum.\n");
	if (hwEnabled())
		std::fprintf(stderr,
			"[DPROF] --hw_prof: Ginstr/Gcyc = TASK-WIDE hardware retired-instruction and"
			" core-cycle counts (proc_pid_rusage), billions per frame, summed over ALL"
			" threads — so a wave's counters cover every worker, like thrsum and unlike"
			" wall.\n"
			"[DPROF] IPC = instr/cyc is the LOAD-ROBUST column: compare IPC across arms,"
			" not ms. Calibrated on this box (docs/HW_PROFILING.md): ~2.4 = compute-bound,"
			" ~1.0 = L1-latency, ~0.14 = L2-latency, <0.05 = DRAM-latency.\n"
			"[DPROF] Cache-miss counters are NOT reachable unprivileged on Apple silicon"
			" (kpc.* sysctls are root-only); IPC is the substitute, and it is a ratio of"
			" two real PMU counters, not a model.\n");
	std::fflush(stderr);
}

}  // namespace TailProf
