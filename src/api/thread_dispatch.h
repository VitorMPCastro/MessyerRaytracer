#pragma once
// thread_dispatch.h — Abstract interface for parallel work dispatch.
//
// Provides a minimal interface for splitting work [0, count) across threads.
// Modules include this instead of dispatch/thread_pool.h, keeping them
// decoupled from the threading implementation.
//
// WHY NOT just expose ThreadPool?
//   ThreadPool lives in dispatch/ — internal infrastructure.
//   Modules (graphics, audio, AI) should depend only on api/ and core/.
//   This interface gives modules parallel dispatch without coupling them
//   to the concrete thread pool implementation.
//
// USAGE:
//   #include "api/thread_dispatch.h"
//
//   IThreadDispatch *dispatch = create_thread_dispatch();
//   dispatch->dispatch_and_wait(pixel_count, 256, [](int start, int end) {
//       for (int i = start; i < end; i++) { /* shade pixel i */ }
//   });

#include <functional>
#include <cstdint>

/// Abstract interface for parallel range dispatch.
/// Implementations must be thread-safe for concurrent dispatch_and_wait calls
/// from different threads (though only one dispatch is active at a time per instance).
class IThreadDispatch {
public:
	virtual ~IThreadDispatch() = default;

	/// Split [0, count) into chunks and dispatch across worker threads.
	/// Blocks until ALL chunks are complete.
	/// If count <= min_batch_size, runs on the calling thread (no overhead).
	virtual void dispatch_and_wait(int count, int min_batch_size,
			const std::function<void(int, int)> &func) = 0;

	/// Number of worker threads in the pool.
	virtual uint32_t thread_count() const = 0;
};

/// Create a concrete thread dispatch backed by a persistent thread pool.
/// @param num_threads  0 = auto-detect (hardware_concurrency - 1, minimum 1).
/// Caller owns the returned pointer and must delete it.
IThreadDispatch *create_thread_dispatch(uint32_t num_threads = 0);
