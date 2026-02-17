#pragma once
// ray_dispatcher.h — Unified ray dispatch interface for all modules.
//
// Modules (graphics, audio, AI) call RayDispatcher instead of directly
// using RayScene or GPURayCaster. The dispatcher transparently routes
// to the best available backend.
//
// WORKFLOW:
//   1. Populate triangles:  dispatcher.scene().triangles.push_back(...)
//   2. Build BVH:           dispatcher.build()
//   3. Cast rays:           dispatcher.cast_rays(rays, results, count)
//
// BACKEND SELECTION:
//   CPU  — Always use BVH traversal on CPU
//   GPU  — Use GPU compute shader (falls back to CPU if unavailable)
//   AUTO — Use GPU if initialized and scene uploaded, else CPU
//
// THREADING:
//   CPU batches are automatically parallelized across cores using a
//   persistent thread pool. The pool is created once (hardware_concurrency - 1
//   workers) and reused for every dispatch. Each worker processes a chunk
//   of the ray array independently.
//
//   For stats gathering, each thread accumulates into its own RayStats,
//   merged into the caller's stats after all chunks complete.
//   This avoids any synchronization during traversal.

#include "accel/ray_scene.h"
#include "accel/scene_tlas.h"
#include "gpu/gpu_ray_caster.h"
#include "core/stats.h"
#include "core/asserts.h"
#include "dispatch/thread_pool.h"
#include "dispatch/ray_sort.h"
#include <vector>
#include <atomic>

class RayDispatcher {
public:
	enum class Backend {
		CPU,    // Always use CPU (BVH traversal)
		GPU,    // Prefer GPU compute shader (graceful CPU fallback)
		AUTO    // Use GPU if ready, else CPU
	};

	// ========================================================================
	// Scene access — for triangle population before build()
	// ========================================================================

	RayScene &scene() { return scene_; }
	const RayScene &scene() const { return scene_; }

	// ========================================================================
	// TLAS — set by server for CPU two-level traversal (Phase 2)
	// ========================================================================

	void set_tlas(SceneTLAS *tlas) { tlas_ = tlas; }
	const SceneTLAS *get_tlas() const { return tlas_; }
	bool has_tlas() const { return tlas_ != nullptr && tlas_->is_built(); }

	// ========================================================================
	// Build — construct BVH + optionally upload to GPU
	// ========================================================================

	// Build acceleration structure from populated triangles.
	// If GPU backend is active, also uploads scene data to GPU.
	void build() {
		RT_ASSERT(scene_.triangle_count() >= 0, "build: invalid triangle count");
		scene_.build();
		RT_ASSERT(scene_.built || scene_.triangles.empty(), "build: build did not complete");

		// Upload to GPU if the pipeline is initialized (not is_available(),
		// which also requires scene_uploaded_ — chicken-and-egg on first build).
		if (_should_use_gpu() && gpu_caster_.is_initialized()) {
			gpu_caster_.upload_scene(scene_.triangles, scene_.bvh2);
			// CWBVH gives ~1.5-2× GPU speedup via 8-wide compressed BVH traversal.
			// upload_cwbvh() is a no-op if the CWBVH shader didn't compile.
			gpu_caster_.upload_cwbvh(scene_.cwbvh);
		}
	}

	// ========================================================================
	// Backend control
	// ========================================================================

	void set_backend(Backend b) { backend_ = b; }
	Backend get_backend() const { return backend_; }

	// Is the GPU compute pipeline initialized and scene uploaded?
	bool gpu_available() const { return gpu_caster_.is_available(); }

	// Is the GPU pipeline initialized (shader ready), even if no scene uploaded yet?
	bool gpu_initialized() const { return gpu_caster_.is_initialized(); }

	// Initialize the GPU compute pipeline (shader compile, pipeline create).
	// Returns true on success. Safe to call multiple times.
	bool initialize_gpu() { return gpu_caster_.initialize(); }

	// Upload current scene to GPU. Call after build() if GPU wasn't ready during build.
	void upload_to_gpu() {
		RT_ASSERT(scene_.triangle_count() >= 0, "upload_to_gpu: invalid triangle count");
		if (gpu_caster_.is_initialized() && scene_.built) {
			RT_ASSERT(!scene_.triangles.empty(), "upload_to_gpu: built scene has no triangles");
			gpu_caster_.upload_scene(scene_.triangles, scene_.bvh2);
			gpu_caster_.upload_cwbvh(scene_.cwbvh);
		}
	}

	// Will the next dispatch actually use the GPU?
	// Useful for choosing which stats format to print.
	bool using_gpu() const {
		return _should_use_gpu() && gpu_caster_.is_available();
	}

	// ========================================================================
	// Dispatch — nearest hit (batch)
	// ========================================================================

	// Find closest intersection per ray. Routes to GPU or CPU transparently.
	// Stats are only populated on the CPU path (GPU doesn't count per-node/tri stats).
	//
	// CPU path is automatically parallelized: the ray array is split across
	// worker threads, each traversing the BVH independently.
	void cast_rays(const Ray *rays, Intersection *results, int count,
			RayStats *stats = nullptr, uint32_t query_mask = 0xFFFFFFFF,
			bool coherent = false) {
		RT_ASSERT(count >= 0, "RayDispatcher::cast_rays: count must be non-negative");
		RT_ASSERT(count == 0 || rays != nullptr, "RayDispatcher::cast_rays: rays is null");
		RT_ASSERT(count == 0 || results != nullptr, "RayDispatcher::cast_rays: results is null");
		if (using_gpu()) {
			// Skip sort for coherent rays (e.g. primary camera rays).
			// Primary rays from a camera already have spatial coherence —
			// adjacent pixels map to nearby directions. Sorting them is pure
			// waste: O(N log N) sort + 3 full-array copies for zero benefit.
			if (!coherent && count >= MIN_BATCH_FOR_SORTING) {
				sync_perm_.clear();
				sort_rays_by_direction(rays, count, sync_perm_);

				sync_sorted_rays_.resize(count);
				for (int i = 0; i < count; i++) {
					sync_sorted_rays_[i] = rays[sync_perm_[i]];
				}

				sync_sorted_results_.resize(count);
				gpu_caster_.cast_rays(sync_sorted_rays_.data(), sync_sorted_results_.data(), count, query_mask);
				unshuffle_intersections(sync_sorted_results_.data(), sync_perm_, results);
			} else {
				gpu_caster_.cast_rays(rays, results, count, query_mask);
			}
			return;
		}

		if (!stats) {
			// No stats — parallel dispatch per chunk.
			// CPU routes through TLAS (two-level, BVH4/BVH8 per BLAS) when available,
			// falls back to flat RayScene otherwise.
			pool_.dispatch_and_wait(count, MIN_BATCH_FOR_THREADING,
					[&](int start, int end) {
						_cpu_cast_rays(&rays[start], &results[start],
								end - start, nullptr, query_mask);
					});
		} else {
			// With stats — each chunk accumulates locally, merge after.
			uint32_t num_slots = pool_.thread_count() + 1;
			std::vector<RayStats> chunk_stats(num_slots);
			std::atomic<uint32_t> slot_counter{0};

			pool_.dispatch_and_wait(count, MIN_BATCH_FOR_THREADING,
					[&](int start, int end) {
						uint32_t slot = slot_counter.fetch_add(1,
								std::memory_order_relaxed);
						RayStats &local = chunk_stats[slot];
						_cpu_cast_rays(&rays[start], &results[start],
								end - start, &local, query_mask);
					});

			for (const auto &cs : chunk_stats) {
				*stats += cs;
			}
		}
	}

	// ========================================================================
	// Dispatch — any hit (batch, shadow/occlusion)
	// ========================================================================

	// Test if rays hit anything. Sets hit_results[i] = true if ray i intersects geometry.
	// Uses early-exit shader on GPU — significantly faster than nearest-hit for yes/no.
	//
	// CPU path is automatically parallelized (same as cast_rays).
	void any_hit_rays(const Ray *rays, bool *hit_results, int count,
			RayStats *stats = nullptr, uint32_t query_mask = 0xFFFFFFFF,
			bool coherent = false) {
		RT_ASSERT(count >= 0, "RayDispatcher::any_hit_rays: count must be non-negative");
		RT_ASSERT(count == 0 || rays != nullptr, "RayDispatcher::any_hit_rays: rays is null");
		RT_ASSERT(count == 0 || hit_results != nullptr, "RayDispatcher::any_hit_rays: hit_results is null");
		if (using_gpu()) {
			if (!coherent && count >= MIN_BATCH_FOR_SORTING) {
				sync_perm_.clear();
				sort_rays_by_direction(rays, count, sync_perm_);

				sync_sorted_rays_.resize(count);
				for (int i = 0; i < count; i++) {
					sync_sorted_rays_[i] = rays[sync_perm_[i]];
				}

				sync_any_hit_buf_.resize(count);
				bool *hit_buf = reinterpret_cast<bool *>(sync_any_hit_buf_.data());
				gpu_caster_.cast_rays_any_hit(sync_sorted_rays_.data(), hit_buf, count, query_mask);
				unshuffle_bools(hit_buf, sync_perm_, hit_results);
			} else {
				gpu_caster_.cast_rays_any_hit(rays, hit_results, count, query_mask);
			}
			return;
		}

		if (!stats) {
			pool_.dispatch_and_wait(count, MIN_BATCH_FOR_THREADING,
					[&](int start, int end) {
						_cpu_any_hit_rays(&rays[start], &hit_results[start],
								end - start, nullptr, query_mask);
					});
		} else {
			uint32_t num_slots = pool_.thread_count() + 1;
			std::vector<RayStats> chunk_stats(num_slots);
			std::atomic<uint32_t> slot_counter{0};

			pool_.dispatch_and_wait(count, MIN_BATCH_FOR_THREADING,
					[&](int start, int end) {
						uint32_t slot = slot_counter.fetch_add(1,
								std::memory_order_relaxed);
						RayStats &local = chunk_stats[slot];
						_cpu_any_hit_rays(&rays[start], &hit_results[start],
								end - start, &local, query_mask);
					});

			for (const auto &cs : chunk_stats) {
				*stats += cs;
			}
		}
	}

	// ========================================================================
	// Dispatch — single ray convenience
	// ========================================================================

	Intersection cast_ray(const Ray &ray, RayStats *stats = nullptr,
			uint32_t query_mask = 0xFFFFFFFF) {
		RT_ASSERT_VALID_RAY(ray);
		if (using_gpu()) {
			Intersection result;
			gpu_caster_.cast_rays(&ray, &result, 1, query_mask);
			return result;
		}
		if (has_tlas()) {
			return tlas_->cast_ray(ray, stats);
		}
		return scene_.cast_ray(ray, stats, query_mask);
	}

	bool any_hit(const Ray &ray, RayStats *stats = nullptr,
			uint32_t query_mask = 0xFFFFFFFF) {
		RT_ASSERT_VALID_RAY(ray);
		if (using_gpu()) {
			bool result;
			gpu_caster_.cast_rays_any_hit(&ray, &result, 1, query_mask);
			return result;
		}
		if (has_tlas()) {
			return tlas_->any_hit(ray, stats);
		}
		return scene_.any_hit(ray, stats, query_mask);
	}

	// ========================================================================
	// Async dispatch — submit / collect pattern
	// ========================================================================
	// Allows modules to overlap CPU work with GPU computation.
	//
	// Example:
	//   dispatcher.submit_gpu_async(rays, count);
	//   /* ... do CPU work (audio processing, AI decisions) ... */
	//   dispatcher.collect_gpu_nearest(results, count);
	//
	// Only useful when GPU backend is active. Falls back to synchronous CPU dispatch
	// if GPU is unavailable (results are ready immediately after submit).
	//
	// Ray sorting is applied automatically for batches >= MIN_BATCH_FOR_SORTING.

	void submit_gpu_async(const Ray *rays, int count) {
		RT_ASSERT(count >= 0, "submit_gpu_async: count must be non-negative");
		RT_ASSERT_NOT_NULL(rays);
		if (using_gpu()) {
			if (count >= MIN_BATCH_FOR_SORTING) {
				async_perm_.clear();
				sort_rays_by_direction(rays, count, async_perm_);
				async_sorted_rays_.resize(count);
				for (int i = 0; i < count; i++) {
					async_sorted_rays_[i] = rays[async_perm_[i]];
				}
				gpu_caster_.submit_async(async_sorted_rays_.data(), count);
				async_sorted_ = true;
			} else {
				gpu_caster_.submit_async(rays, count);
				async_sorted_ = false;
			}
		}
	}

	void collect_gpu_nearest(Intersection *results, int count) {
		RT_ASSERT(count >= 0, "collect_gpu_nearest: count must be non-negative");
		RT_ASSERT_NOT_NULL(results);
		if (async_sorted_ && !async_perm_.empty()) {
			async_sorted_results_.resize(count);
			gpu_caster_.collect_nearest(async_sorted_results_.data(), count);
			unshuffle_intersections(async_sorted_results_.data(), async_perm_, results);
			async_sorted_ = false;
		} else {
			gpu_caster_.collect_nearest(results, count);
		}
	}

	void submit_gpu_async_any_hit(const Ray *rays, int count) {
		RT_ASSERT_NOT_NULL(rays);
		RT_ASSERT(count >= 0, "submit_gpu_async_any_hit: count must be non-negative");
		if (using_gpu()) {
			if (count >= MIN_BATCH_FOR_SORTING) {
				async_perm_.clear();
				sort_rays_by_direction(rays, count, async_perm_);
				async_sorted_rays_.resize(count);
				for (int i = 0; i < count; i++) {
					async_sorted_rays_[i] = rays[async_perm_[i]];
				}
				gpu_caster_.submit_async_any_hit(async_sorted_rays_.data(), count);
				async_sorted_ = true;
			} else {
				gpu_caster_.submit_async_any_hit(rays, count);
				async_sorted_ = false;
			}
		}
	}

	void collect_gpu_any_hit(bool *hit_results, int count) {
		RT_ASSERT(count >= 0, "collect_gpu_any_hit: count must be non-negative");
		RT_ASSERT_NOT_NULL(hit_results);
		if (async_sorted_ && !async_perm_.empty()) {
			async_any_hit_buf_.resize(count);
			gpu_caster_.collect_any_hit(reinterpret_cast<bool *>(async_any_hit_buf_.data()), count);
			unshuffle_bools(reinterpret_cast<bool *>(async_any_hit_buf_.data()), async_perm_, hit_results);
			async_sorted_ = false;
		} else {
			gpu_caster_.collect_any_hit(hit_results, count);
		}
	}

	bool has_gpu_pending() const { return gpu_caster_.has_pending(); }

	// ========================================================================
	// Scene info
	// ========================================================================

	int triangle_count() const { return scene_.triangle_count(); }

	int bvh_node_count() const {
		if (has_tlas()) {
			int count = static_cast<int>(tlas_->tlas_bvh().NodeCount());
			RT_ASSERT(count >= 0, "bvh_node_count: TLAS node count overflow");
			return count;
		}
		int count = scene_.built ? static_cast<int>(scene_.bvh2.NodeCount()) : 0;
		RT_ASSERT(count >= 0, "bvh_node_count: node count overflow");
		return count;
	}

	int bvh_depth() const {
		// TinyBVH doesn't expose tree depth directly. Approximate as log2(nodes).
		int nodes = bvh_node_count();
		RT_ASSERT(nodes >= 0, "bvh_depth: node count must be non-negative");
		if (nodes <= 0) { return 0; }
		int depth = 0;
		while ((1 << depth) < nodes) { depth++; }
		RT_ASSERT(depth >= 0 && depth < 64, "bvh_depth: depth out of reasonable range");
		return depth;
	}

	// Number of worker threads in the thread pool (excludes main thread).
	uint32_t thread_count() const { return pool_.thread_count(); }

	/// Access the thread pool as the abstract IThreadDispatch interface.
	/// Used by IRayService::get_thread_dispatch() so modules share this pool
	/// instead of creating their own.
	IThreadDispatch &thread_pool() { return pool_; }

	/// Access the GPU caster for shared RenderingDevice and buffer RIDs.
	/// Used by IRayService bridge to expose GPU context to modules.
	GPURayCaster &gpu_caster() { return gpu_caster_; }
	const GPURayCaster &gpu_caster() const { return gpu_caster_; }

private:
	RayScene scene_;            // Flat scene for GPU staging (world-space triangles + BVH2)
	SceneTLAS *tlas_ = nullptr; // Two-level BVH for CPU path (set by server)
	GPURayCaster gpu_caster_;
	ThreadPool pool_;            // Persistent worker threads for CPU dispatch
	Backend backend_ = Backend::CPU;

	// Synchronous dispatch reuse buffers (avoid per-frame heap allocation).
	// At 1280×960 these save ~140 MB of alloc+free per frame.
	std::vector<uint32_t> sync_perm_;
	std::vector<Ray> sync_sorted_rays_;
	std::vector<Intersection> sync_sorted_results_;
	std::vector<uint8_t> sync_any_hit_buf_;

	// Async dispatch state (reused to avoid per-frame allocation)
	std::vector<uint32_t> async_perm_;
	std::vector<Ray> async_sorted_rays_;
	std::vector<Intersection> async_sorted_results_;
	std::vector<uint8_t> async_any_hit_buf_;
	bool async_sorted_ = false;

	// Don't bother threading for tiny batches — the synchronization overhead
	// would exceed any traversal savings. 128 rays is roughly the break-even
	// point on modern CPUs with 100-500ns per BVH traversal.
	static constexpr int MIN_BATCH_FOR_THREADING = 128;

	// Minimum ray count before sorting is worthwhile.
	// The O(N log N) sort cost needs enough rays for warp coherence savings to pay off.
	static constexpr int MIN_BATCH_FOR_SORTING = 256;

	bool _should_use_gpu() const {
		RT_ASSERT(backend_ == Backend::CPU || backend_ == Backend::GPU || backend_ == Backend::AUTO,
			"_should_use_gpu: invalid backend enum value");
		switch (backend_) {
			case Backend::GPU:  return true;
			case Backend::AUTO: return gpu_caster_.is_available();
			case Backend::CPU:  return false;
		}
		RT_UNREACHABLE("_should_use_gpu: unhandled backend case");
		return false;
	}

	// Internal CPU dispatch helpers — route through TLAS (two-level, instance-aware)
	// when available, fall back to flat RayScene otherwise.
	void _cpu_cast_rays(const Ray *rays, Intersection *results, int count,
			RayStats *stats, uint32_t query_mask) const {
		RT_ASSERT_NOT_NULL(rays);
		RT_ASSERT_NOT_NULL(results);
		if (has_tlas()) {
			tlas_->cast_rays(rays, results, count, stats);
		} else {
			scene_.cast_rays(rays, results, count, stats, query_mask);
		}
	}

	void _cpu_any_hit_rays(const Ray *rays, bool *hit_results, int count,
			RayStats *stats, uint32_t query_mask) const {
		RT_ASSERT_NOT_NULL(rays);
		RT_ASSERT_NOT_NULL(hit_results);
		if (has_tlas()) {
			tlas_->any_hit_rays(rays, hit_results, count, stats);
		} else {
			scene_.any_hit_rays(rays, hit_results, count, stats, query_mask);
		}
	}
};
