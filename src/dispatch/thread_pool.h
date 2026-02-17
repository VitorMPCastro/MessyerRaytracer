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
//
// THREAD SAFETY:
//   NOT thread-safe for concurrent dispatch_and_wait() calls.
//   Only ONE thread may call dispatch_and_wait() at a time (the Godot main thread).
//   Worker threads are internal and never call public methods.

#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <cstdint>
#include <exception>

#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include "core/asserts.h"
#include "api/thread_dispatch.h"

class ThreadPool : public IThreadDispatch {
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
			workers_.emplace_back([this] { _worker_loop(); });
		}

		RT_ASSERT(thread_count_ > 0, "ThreadPool must have at least 1 worker thread");
		RT_ASSERT(workers_.size() == thread_count_, "Worker count must match thread_count_");
	}

	~ThreadPool() {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			shutdown_ = true;
		}
		cv_work_.notify_all();
		for (auto &t : workers_) {
			if (t.joinable()) { t.join(); }
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
			const std::function<void(int, int)> &func) override {
		RT_ASSERT(count >= 0, "dispatch_and_wait: count must be non-negative");
		RT_ASSERT(min_batch_size > 0, "dispatch_and_wait: min_batch_size must be positive");
		if (count <= 0) { return; }

		// For small batches, don't bother with threading overhead.
		if (count <= min_batch_size || thread_count_ == 0) {
			func(0, count);
			return;
		}

		// Split into chunks: one per worker + one for the calling thread.
		uint32_t num_chunks = thread_count_ + 1;
		int chunk_size = (count + num_chunks - 1) / num_chunks;

		// Submit work description + reset completion counter under the lock.
		// pending_chunks_ is a CV predicate — MUST be written under mutex_ (Rule 10).
		{
			std::lock_guard<std::mutex> lock(mutex_);
			work_func_ = &func;
			work_chunk_size_ = chunk_size;
			work_total_ = count;
			work_next_chunk_.store(1); // chunk 0 is for calling thread
			pending_chunks_ = num_chunks - 1; // workers handle (num_chunks - 1) chunks
			work_generation_++;
		}
		cv_work_.notify_all();

		// Calling thread processes chunk 0
		int start = 0;
		int end = std::min(chunk_size, count);
		// EXCEPTION SAFETY: try/catch is intentionally kept here despite our no-exceptions rule.
		// WHY: Worker lambdas interact with Godot arrays and user-provided data. If a lambda
		// throws (e.g., assertion-to-exception on some platforms, or undefined behavior in
		// third-party code), raw thread termination without catch would deadlock the pool
		// (pending_chunks_ never reaches 0) and freeze the entire editor. The catch logs the
		// error through ERR_PRINT (visible in the Godot Output panel) and lets the pool drain.
		try {
			func(start, end);
		} catch (const std::exception &e) {
			ERR_PRINT(godot::String("ThreadPool main-thread exception: ") + e.what());
		} catch (...) {
			ERR_PRINT("ThreadPool main-thread: unknown exception");
		}

		// Wait for all worker chunks to complete.
		// Safety: timeout after 30 seconds to prevent permanent freeze.
		{
			std::unique_lock<std::mutex> lock(mutex_);
			bool completed = cv_done_.wait_for(lock, std::chrono::seconds(30),
					[this] { return pending_chunks_ == 0; });
			if (!completed) {
				ERR_PRINT("[ThreadPool] WARNING: dispatch_and_wait timed out after 30s -- possible worker hang");
			}
		}
	}

	uint32_t thread_count() const override { return thread_count_; }

private:
	std::vector<std::thread> workers_;   // Owned. Joined in destructor.
	uint32_t thread_count_ = 0;

	std::mutex mutex_;  // Protects: work_func_, work_chunk_size_, work_total_,
	                    //           pending_chunks_, work_generation_, shutdown_.
	                    // Lock ordering: always acquired before cv_work_ / cv_done_ waits.

	std::condition_variable cv_work_;  // Predicate: work_generation_ > last_gen || shutdown_.
	                                   // Guarded by: mutex_.
	                                   // Notified by: dispatch_and_wait (new work) and destructor (shutdown).

	std::condition_variable cv_done_;  // Predicate: pending_chunks_ == 0.
	                                   // Guarded by: mutex_.
	                                   // Notified by: last worker to complete its chunk(s).
	bool shutdown_ = false;

	// Work description (protected by mutex_)
	const std::function<void(int, int)> *work_func_ = nullptr;
	int work_chunk_size_ = 0;
	int work_total_ = 0;

	std::atomic<uint32_t> work_next_chunk_{0};  // Lock-free chunk counter for work-stealing.
	                                             // Not a CV predicate — safe without mutex_.
	                                             // Workers atomically grab the next chunk index;
	                                             // no CV waits on this value.

	uint32_t pending_chunks_ = 0;  // Number of worker chunks not yet completed.
	                                // CV predicate for cv_done_ — MUST be read/written under mutex_.
	                                // WHY NOT atomic? An atomic store without the mutex causes
	                                // lost cv_done_ notifications (Rule 10). Plain uint32_t
	                                // under mutex is both correct and sufficient.

	uint64_t work_generation_ = 0;  // Monotonic counter — incremented each dispatch.
	                                 // CV predicate for cv_work_ — written under mutex_.

	void _worker_loop() {
		RT_ASSERT(thread_count_ > 0, "worker_loop: thread_count_ must be positive");
		RT_ASSERT(!shutdown_, "worker_loop: pool must not be shut down at entry");
		uint64_t last_gen = 0;

		while (true) {
			// Wait for new work
			{
				std::unique_lock<std::mutex> lock(mutex_);
				cv_work_.wait(lock, [this, last_gen] {
					return shutdown_ || work_generation_ > last_gen;
				});
				if (shutdown_) { return; }
				last_gen = work_generation_;
			}

			// Grab chunks until none remain.
			// work_next_chunk_ is atomic — multiple workers can grab concurrently
			// without holding mutex_. This is safe because work_next_chunk_ is NOT
			// a CV predicate (Rule 10 only requires mutex for CV predicates).
			while (true) {
				uint32_t chunk = work_next_chunk_.fetch_add(1);
				int start = chunk * work_chunk_size_;
				if (start >= work_total_) { break; }
				int end = std::min(start + work_chunk_size_, work_total_);

				// See dispatch_and_wait for WHY try/catch is justified here.
				try {
					(*work_func_)(start, end);
				} catch (const std::exception &e) {
					ERR_PRINT(godot::String("ThreadPool worker exception: ") + e.what());
				} catch (...) {
					ERR_PRINT("ThreadPool worker: unknown exception");
				}
			}

			// Signal completion — pending_chunks_ is a CV predicate, MUST be
			// decremented under mutex_ so the waiter cannot miss the notification.
			{
				std::lock_guard<std::mutex> lock(mutex_);
				RT_VERIFY(pending_chunks_ > 0,
						"ThreadPool: pending_chunks_ underflow — more workers "
						"completed than were dispatched");
				pending_chunks_--;
				if (pending_chunks_ == 0) {
					cv_done_.notify_one();
				}
			}
		}
	}
};
