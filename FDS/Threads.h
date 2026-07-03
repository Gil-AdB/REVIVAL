#pragma once

#include <atomic>
#include <algorithm>
#include <climits>
#include <memory>
#include <semaphore>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <queue>
#include <thread>
#include <vector>
#include <mutex>

#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#endif

// Bumps the calling thread to USER_INTERACTIVE QoS on macOS so the
// scheduler picks performance (P) cores instead of efficiency (E) cores.
// No-op everywhere else — wasm/Linux/Windows have no equivalent (the
// browser/OS picks the core).
inline void HintHighPerfThread() {
#if defined(__APPLE__)
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}


class ThreadPool {
public:
	static ThreadPool& instance() {
		static ThreadPool inst;
		return inst;
	}

	using item_t = std::function<void()>;

	void init(item_t initFunc) {
		auto numThreads = std::thread::hardware_concurrency();
		// Debug override: FDS_THREADS=N caps the pool (FDS_THREADS=1 makes
		// every tile-job sequence effectively serial — the fastest way to
		// confirm a suspected thread race; parallel_memset etc. fall back
		// to serial below 2 workers).
		if (const char* e = getenv("FDS_THREADS")) {
			const long v = strtol(e, nullptr, 10);
			if (v >= 1 && v < long(numThreads)) numThreads = unsigned(v);
		}
		for (size_t ii = 0; ii < numThreads; ii++) {
			pool.push_back(std::thread([this, initFunc]() {
				HintHighPerfThread();
				initFunc();
				worker();
			}));
		}
	}

	// Number of workers in the pool — used by parallel_memset/parallel_fn
	// to size their splits.
	size_t size() const { return pool.size(); }

	void enqueue(item_t f) {
		{
			std::unique_lock lock(qm);
			q.push(f);
		}
		condition.notify_one();
	}

	void close() {
		terminate = true;
		condition.notify_all();
		for (auto& t : pool) {
			t.join();
		}
	}

private:
	void worker() {
		while (!terminate) {
			std::function<void()> item;
			{
				std::unique_lock<std::mutex> lock(qm);
				condition.wait(lock, [this] {return !q.empty() || terminate; });
				if (terminate) {
					break;
				}
				item = std::move(q.front());
				q.pop();
			}
			item();
		}
	}

	std::mutex qm;
	std::condition_variable condition;
	std::queue<item_t> q;
	std::atomic<bool> terminate = false;
	std::vector<std::thread> pool;
};


// Work-stealing indexed dispatch: run fn(i) for every i in [0, jobs) across
// the pool via W chunk tasks pulling indices off an atomic cursor, instead of
// one enqueue per index. Each enqueue costs ~12 µs serial on the producer
// (queue mutex + notify_one while workers park, then the woken workers
// contend the same mutex to pop) — at 96-tile waves that was ~1.2 ms/frame
// of tick-thread time per wave (measured, greets ts=491).
//
// Completion contract: `done` (when non-null) is released ONCE PER INDEX by
// the chunk task right after fn(i) returns — callers keep their existing
// `jobs`-permit drain. If fn itself releases the semaphore (some tile
// kernels do), pass done=nullptr.
//
// LIFETIME (two traps, both hit during development — do not "simplify"):
//   1. fn is usually a TEMPORARY closure: it dies when the dispatchIndexed()
//      call expression ends — BEFORE the caller's drain. So each chunk task
//      copies fn BY VALUE. Copying a [&] closure copies only the references,
//      which point at the caller's stack — valid until the caller's drain
//      completes, and fn is only invoked before that index's release.
//   2. After the last index's release, the caller's drain can return and its
//      stack die while a STRAGGLER worker evaluates the while-condition one
//      final time. So everything the condition touches (cursor, jobs) is
//      captured by value (cursor via shared_ptr); the straggler destroys its
//      fn copy without invoking it.
template <class Fn>
inline void dispatchIndexed(int jobs, std::counting_semaphore<INT_MAX>* done, Fn fn) {
	auto& tp = ThreadPool::instance();
	const int nTasks = (int)std::min<size_t>(tp.size(), (size_t)(jobs > 0 ? jobs : 0));
	if (nTasks <= 1) {
		for (int i = 0; i < jobs; ++i) { fn(i); if (done) done->release(); }
		return;
	}
	auto cursor = std::make_shared<std::atomic<int>>(0);
	for (int k = 0; k < nTasks; ++k) {
		tp.enqueue([fn, cursor, jobs, done]() {
			int i;
			while ((i = cursor->fetch_add(1, std::memory_order_relaxed)) < jobs) {
				fn(i);
				if (done) done->release();
			}
		});
	}
}

// Parallel memset across the ThreadPool. Splits the buffer into N chunks
// (one per worker) and waits for completion. For small buffers, falls back
// to a serial memset since the enqueue + atomic-wait overhead would dwarf
// the actual memset cost.
//
// Sync uses a shared_ptr<atomic<size_t>>: workers capture it by value, the
// caller polls .load() and yields until 0. shared_ptr keeps the counter
// alive until the last worker's lambda is done — capturing by reference
// would UAF the caller's stack as soon as the predicate hit 0 (the last
// worker's notify_one would land in whatever stack frame replaced ours).
inline void parallel_memset(void *p, int value, size_t n) {
	constexpr size_t SERIAL_THRESHOLD = 256 * 1024;
	auto &tp = ThreadPool::instance();
	const size_t numThreads = tp.size();
	if (n < SERIAL_THRESHOLD || numThreads < 2) {
		std::memset(p, value, n);
		return;
	}

	const size_t chunk = n / numThreads;
	auto remaining = std::make_shared<std::atomic<size_t>>(numThreads);
	for (size_t i = 0; i < numThreads; ++i) {
		const size_t start = i * chunk;
		const size_t end = (i + 1 == numThreads) ? n : start + chunk;
		tp.enqueue([p, value, start, end, remaining]() {
			std::memset(static_cast<unsigned char *>(p) + start, value, end - start);
			remaining->fetch_sub(1, std::memory_order_release);
		});
	}
	// Caller spins on yield — fast since memset is bandwidth-limited and
	// finishes in well under a millisecond at HiDPI sizes.
	while (remaining->load(std::memory_order_acquire) != 0) {
		std::this_thread::yield();
	}
}

