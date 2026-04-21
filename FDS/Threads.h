#pragma once

#include <atomic>
#include <functional>
#include <queue>
#include <thread>
#include <vector>
#include <mutex>


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
			pool.push_back(std::thread([this, initFunc]() {initFunc(); worker(); }));
		}
	}

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

