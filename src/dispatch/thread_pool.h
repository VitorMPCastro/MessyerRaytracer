#pragma once
// thread_pool.h — Lightweight thread pool for parallel ray batch processing.
//
// Spawns N worker threads (typically CPU core count - 1) at construction.
// Work is submitted as batches that are split across workers automatically.
//
// DESIGN CHOICES:
//   - Fixed thread count (no dynamic scaling) — simpler, no allocation during dispatch
//   - Uses std::thread + std::mutex + std::condition_variable — no external deps
//   - Batch-oriented: submit a range [0, count) and a lambda(start, end)
//   - Barrier-based: dispatch_and_wait() blocks until all chunks are done
//
// WHY NOT std::async or OpenMP?
//   std::async creates/destroys threads per call — too much overhead for per-frame ray batches.
//   OpenMP requires compiler flags and isn't always available with MSVC + SCons.
//   A persistent thread pool amortizes thread creation across the entire application lifetime.

#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <cstdint>

class ThreadPool {
public:
	// Create a thread pool with 'num_threads' workers.
	// 0 = auto-detect (hardware_concurrency - 1, minimum 1).
	explicit ThreadPool(uint32_t num_threads = 0) {
		if (num_threads == 0) {
			uint32_t hw = std::thread::hardware_concurrency();
			num_threads = (hw > 1) ? hw - 1 : 1;
		}
		thread_count_ = num_threads;

		workers_.reserve(num_threads);
		for (uint32_t i = 0; i < num_threads; i++) {
			workers_.emplace_back([this] { worker_loop(); });
		}
	}

	~ThreadPool() {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			shutdown_ = true;
		}
		cv_work_.notify_all();
		for (auto &t : workers_) {
			if (t.joinable()) t.join();
		}
	}

	// Non-copyable
	ThreadPool(const ThreadPool &) = delete;
	ThreadPool &operator=(const ThreadPool &) = delete;

	// Split [0, count) into chunks and dispatch across workers.
	// 'func' is called as func(start_index, end_index) for each chunk.
	// Blocks until ALL chunks are complete.
	//
	// If count <= min_batch_size, runs on calling thread (no threading overhead).
	void dispatch_and_wait(int count, int min_batch_size,
			const std::function<void(int, int)> &func) {
		if (count <= 0) return;

		// For small batches, don't bother with threading overhead.
		if (count <= min_batch_size || thread_count_ == 0) {
			func(0, count);
			return;
		}

		// Split into chunks: one per worker + one for the calling thread.
		uint32_t num_chunks = thread_count_ + 1;
		int chunk_size = (count + num_chunks - 1) / num_chunks;

		// Reset completion counter
		pending_chunks_.store(num_chunks - 1); // workers handle (num_chunks - 1) chunks

		// Submit chunks to worker threads
		{
			std::lock_guard<std::mutex> lock(mutex_);
			work_func_ = &func;
			work_chunk_size_ = chunk_size;
			work_total_ = count;
			work_next_chunk_.store(1); // chunk 0 is for calling thread
			work_generation_++;
		}
		cv_work_.notify_all();

		// Calling thread processes chunk 0
		int start = 0;
		int end = std::min(chunk_size, count);
		func(start, end);

		// Wait for all worker chunks to complete
		{
			std::unique_lock<std::mutex> lock(mutex_);
			cv_done_.wait(lock, [this] { return pending_chunks_.load() == 0; });
		}
	}

	uint32_t thread_count() const { return thread_count_; }

private:
	std::vector<std::thread> workers_;
	uint32_t thread_count_ = 0;

	std::mutex mutex_;
	std::condition_variable cv_work_;
	std::condition_variable cv_done_;
	bool shutdown_ = false;

	// Work description (protected by mutex_)
	const std::function<void(int, int)> *work_func_ = nullptr;
	int work_chunk_size_ = 0;
	int work_total_ = 0;
	std::atomic<uint32_t> work_next_chunk_{0};
	std::atomic<uint32_t> pending_chunks_{0};
	uint64_t work_generation_ = 0;

	void worker_loop() {
		uint64_t last_gen = 0;

		while (true) {
			// Wait for new work
			{
				std::unique_lock<std::mutex> lock(mutex_);
				cv_work_.wait(lock, [this, last_gen] {
					return shutdown_ || work_generation_ > last_gen;
				});
				if (shutdown_) return;
				last_gen = work_generation_;
			}

			// Grab chunks until none remain
			while (true) {
				uint32_t chunk = work_next_chunk_.fetch_add(1);
				int start = chunk * work_chunk_size_;
				if (start >= work_total_) break;
				int end = std::min(start + work_chunk_size_, work_total_);

				(*work_func_)(start, end);
			}

			// Signal completion
			if (pending_chunks_.fetch_sub(1) == 1) {
				cv_done_.notify_one();
			}
		}
	}
};
