#pragma once

#include <atomic>
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


// Parallel memset across the ThreadPool. Splits the buffer into N chunks
// (one per worker) and waits for completion. For small buffers, falls back
// to a serial memset since the enqueue + condvar overhead (~50us per
// worker) would dwarf the actual memset cost.
inline void parallel_memset(void *p, int value, size_t n) {
	// Below this, single-threaded is faster — enqueue-and-wait overhead
	// is on the order of 100-400us, which is more than memset spends on a
	// few-hundred-KB clear.
	constexpr size_t SERIAL_THRESHOLD = 256 * 1024;
	auto &tp = ThreadPool::instance();
	const size_t numThreads = tp.size();
	if (n < SERIAL_THRESHOLD || numThreads < 2) {
		std::memset(p, value, n);
		return;
	}

	const size_t chunk = n / numThreads;
	std::atomic<size_t> remaining{numThreads};
	std::mutex doneM;
	std::condition_variable doneCV;
	for (size_t i = 0; i < numThreads; ++i) {
		const size_t start = i * chunk;
		const size_t end = (i + 1 == numThreads) ? n : start + chunk;
		tp.enqueue([p, value, start, end, &remaining, &doneM, &doneCV]() {
			std::memset(static_cast<unsigned char *>(p) + start, value, end - start);
			if (remaining.fetch_sub(1) == 1) {
				std::lock_guard<std::mutex> lock(doneM);
				doneCV.notify_one();
			}
		});
	}
	std::unique_lock<std::mutex> lock(doneM);
	doneCV.wait(lock, [&] { return remaining.load() == 0; });
}

