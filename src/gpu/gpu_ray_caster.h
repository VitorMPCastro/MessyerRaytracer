#pragma once
// gpu_ray_caster.h — GPU compute shader ray caster using Godot's RenderingDevice.
//
// ARCHITECTURE:
//   GPURayCaster owns a LOCAL RenderingDevice (separate from Godot's renderer).
//   This means GPU ray casting runs independently — it won't stall rendering.
//
//   The BVH is built on CPU (we already have a good SAH builder), then
//   uploaded to the GPU as storage buffers. The compute shader does the
//   traversal in parallel — one thread per ray.
//
// LIFECYCLE:
//   1. initialize()      — Create local device, compile shader, create pipeline.
//   2. upload_scene()     — Upload triangle + BVH data to GPU buffers.
//   3. cast_rays()        — Upload rays, dispatch compute, read back results.
//   4. cleanup()          — Free all GPU resources (also called by destructor).
//
// FALLBACK:
//   If initialization fails (no Vulkan, shader compile error, etc.),
//   is_available() returns false and the caller should use the CPU path.
//   The GPU path is a transparent optimization — never required.
//
// BUFFER REUSE:
//   Scene buffers (triangles, BVH) are uploaded once and reused across frames.
//   Ray/result buffers grow as needed (never shrink) to avoid reallocation.
//   The uniform set (descriptor set) is rebuilt whenever buffer RIDs change.

#include <cstdint>
#include <vector>

#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

// Forward declarations — avoid pulling heavy Godot headers into every TU
namespace godot {
class RenderingDevice;
}

struct Ray;
struct Triangle;
struct Intersection;
struct BVHNode;
struct GPURayPacked;
struct GPUIntersectionPacked;
class BVH;

class GPURayCaster {
public:
	GPURayCaster();
	~GPURayCaster();

	// Create local rendering device, compile shader, create compute pipeline.
	// Returns true on success, false if GPU compute is not available.
	// Safe to call multiple times — returns immediately if already initialized.
	bool initialize();

	// Is the GPU compute pipeline ready to use? (initialized + scene uploaded)
	bool is_available() const;

	// Is the GPU pipeline initialized? (shader compiled, pipeline created)
	// True even before scene upload — use this to decide whether to upload.
	bool is_initialized() const { return initialized_; }

	// Upload scene geometry + BVH to GPU storage buffers.
	// Call after building the BVH on CPU. Scene data stays on GPU until
	// upload_scene() is called again or cleanup() is called.
	void upload_scene(const std::vector<Triangle> &triangles, const BVH &bvh);

	// Cast rays on the GPU (nearest hit). Writes results into the 'results' array.
	// Each result corresponds to the ray at the same index.
	// 'count' is the number of rays (and results).
	void cast_rays(const Ray *rays, Intersection *results, int count,
			uint32_t query_mask = 0xFFFFFFFF);

	// Cast rays using any_hit mode (early exit on first intersection).
	// For shadow/occlusion queries. Sets hit_results[i] = true if ray i hits anything.
	// Much faster than nearest-hit when you only need yes/no answers.
	void cast_rays_any_hit(const Ray *rays, bool *hit_results, int count,
			uint32_t query_mask = 0xFFFFFFFF);

	// ---- Async dispatch (submit / collect pattern) ----
	// Allows the caller to overlap CPU work with GPU computation.
	//
	// Usage:
	//   caster.submit_async(rays, count);         // Non-blocking: GPU starts work
	//   /* ... do CPU work here (prepare next batch, process audio, etc.) ... */
	//   caster.collect_nearest(results, count);    // Blocks until GPU is done, reads back
	//
	// WARNING: Do not call submit_async again before collecting results.

	// Upload rays and dispatch compute shader. Returns immediately after submit().
	// The GPU begins working in the background.
	void submit_async(const Ray *rays, int count, uint32_t query_mask = 0xFFFFFFFF);
	void submit_async_any_hit(const Ray *rays, int count, uint32_t query_mask = 0xFFFFFFFF);

	// Block until the GPU finishes, then read back results.
	// Must be called exactly once after each submit_async/submit_async_any_hit.
	void collect_nearest(Intersection *results, int count);
	void collect_any_hit(bool *hit_results, int count);

	// Is there a pending async dispatch that hasn't been collected yet?
	bool has_pending() const { return pending_async_; }

	// Free all GPU resources (buffers, pipeline, shader, device).
	// Safe to call multiple times.
	void cleanup();

private:
	// ---- Godot rendering device (owned) ----
	godot::RenderingDevice *rd_ = nullptr;

	// ---- Shader & pipeline ----
	godot::RID shader_;
	godot::RID pipeline_;           // nearest-hit mode (RAY_MODE=0)
	godot::RID pipeline_any_hit_;   // any-hit mode (RAY_MODE=1, early exit)

	// ---- Scene buffers (uploaded once per build_scene) ----
	godot::RID triangle_buffer_;
	godot::RID bvh_node_buffer_;

	// ---- Per-dispatch buffers (grow as needed) ----
	godot::RID ray_buffer_;
	godot::RID result_buffer_;
	uint32_t ray_buffer_capacity_ = 0; // Current capacity in # of rays

	// ---- Descriptor set ----
	godot::RID uniform_set_;
	bool uniform_set_dirty_ = true; // Rebuild when any buffer RID changes

	// ---- State ----
	bool initialized_ = false;
	bool scene_uploaded_ = false;
	bool pending_async_ = false; // True between submit_async and collect

	// ---- Persistent cache (avoid per-frame heap allocation) ----
	// At 1280×960 these save ~130MB of alloc+free per frame.
	godot::PackedByteArray upload_cache_;                 // Upload staging buffer
	godot::PackedByteArray push_data_cache_;              // Push constants (16 bytes, reused)

	// Stored for position reconstruction in collect_nearest.
	// The new compact GPUIntersectionPacked (32 bytes) omits position to save
	// 33% bandwidth. We reconstruct: position = origin + direction * t.
	const Ray *pending_rays_ = nullptr;

	// ---- Internal helpers ----
	void free_scene_buffers();
	void free_ray_buffers();
	void ensure_ray_buffers(uint32_t ray_count);
	void rebuild_uniform_set();

	// Upload rays, dispatch compute with given pipeline, submit+sync.
	// After return, result_buffer_ contains GPU output ready for readback.
	void dispatch_rays_internal(const Ray *rays, int count, const godot::RID &pipeline,
			uint32_t query_mask);

	// Upload rays, dispatch compute, submit but do NOT sync.
	// Used by async dispatch. Caller must call rd_->sync() before readback.
	void dispatch_rays_no_sync(const Ray *rays, int count, const godot::RID &pipeline,
			uint32_t query_mask);

	// Number of rays in the last async submission (for readback sizing).
	int pending_count_ = 0;
};
