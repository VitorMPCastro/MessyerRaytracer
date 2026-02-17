// thread_dispatch_factory.cpp — Factory for IThreadDispatch (backed by ThreadPool).

#include "api/thread_dispatch.h"
#include "dispatch/thread_pool.h"

IThreadDispatch *create_thread_dispatch(uint32_t num_threads) {
	return new ThreadPool(num_threads);
}
