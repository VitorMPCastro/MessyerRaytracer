// gpu_ray_caster.cpp — GPU compute shader ray caster implementation.
//
// This is where the Godot RenderingDevice API calls happen:
//   - Shader compilation (GLSL → SPIR-V → pipeline)
//   - Buffer management (create, update, read back)
//   - Compute dispatch (bind pipeline → bind buffers → dispatch → sync)
//
// All GPU operations use a LOCAL RenderingDevice, which means they don't
// interfere with Godot's main rendering pipeline.

#include "gpu_ray_caster.h"
#include "gpu_structs.h"
#include "shaders/bvh_traverse.gen.h"
#include "shaders/cwbvh_traverse.gen.h"

#include "core/ray.h"
#include "core/intersection.h"
#include "core/triangle.h"
#include "core/asserts.h"

// TinyBVH headers for BVH2 node access during GPU upload.
#ifndef TINYBVH_INST_IDX_BITS
#define TINYBVH_INST_IDX_BITS 32
#endif
#include "thirdparty/tinybvh/tiny_bvh.h"

#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/rd_pipeline_specialization_constant.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/core/memory.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <vector>

using namespace godot;

// Workgroup size — must match local_size_x in the GLSL shader.
// 128 gives better occupancy/latency hiding on Turing+ GPUs vs 64,
// while staying under shared memory limits (128 × 24 × 4B × 2 = 24KB < 48KB).
static constexpr uint32_t WORKGROUP_SIZE = 128;

// Maximum rays per GPU dispatch batch.  Keeps individual dispatches well under
// the Windows TDR timeout (~2 s) and prevents oversized staging-buffer uploads
// from causing internal Vulkan flushes inside Godot's RenderingDevice.
// 512K rays × 32 B/ray = 16 MB upload per batch — comfortably under typical
// 64–256 MB staging buffer limits.
static constexpr int MAX_GPU_BATCH_SIZE = 512 * 1024;

// ============================================================================
// Constructor / Destructor
// ============================================================================

GPURayCaster::GPURayCaster() {}

GPURayCaster::~GPURayCaster() {
	cleanup();
}

// ============================================================================
// Initialize — create device, compile shader, build pipeline
// ============================================================================

bool GPURayCaster::initialize() {
	if (initialized_) { return true; }

	// 1. Create a local rendering device (separate from the main renderer).
	RenderingServer *rs = RenderingServer::get_singleton();
	if (!rs) {
		UtilityFunctions::print("[GPU RayCaster] RenderingServer not available");
		return false;
	}

	rd_ = rs->create_local_rendering_device();
	if (!rd_) {
		UtilityFunctions::print("[GPU RayCaster] Failed to create local rendering device (no Vulkan/D3D12?)");
		return false;
	}

	// 2. Set up shader source (GLSL compute stage).
	Ref<RDShaderSource> source;
	source.instantiate();
	source->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, String(BVH_TRAVERSE_GLSL));
	source->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);

	// 3. Compile GLSL to SPIR-V.
	Ref<RDShaderSPIRV> spirv = rd_->shader_compile_spirv_from_source(source);
	if (spirv.is_null()) {
		UtilityFunctions::print("[GPU RayCaster] Shader compilation returned null");
		cleanup();
		return false;
	}

	String compile_error = spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE);
	if (!compile_error.is_empty()) {
		UtilityFunctions::print("[GPU RayCaster] Shader compile error: ", compile_error);
		cleanup();
		return false;
	}

	// 4. Create shader module from SPIR-V.
	shader_ = rd_->shader_create_from_spirv(spirv);
	if (!shader_.is_valid()) {
		UtilityFunctions::print("[GPU RayCaster] Failed to create shader from SPIR-V");
		cleanup();
		return false;
	}

	// 5. Create compute pipelines.
	// Nearest-hit pipeline (default: RAY_MODE=0).
	pipeline_ = rd_->compute_pipeline_create(shader_);
	if (!pipeline_.is_valid()) {
		UtilityFunctions::print("[GPU RayCaster] Failed to create nearest-hit compute pipeline");
		cleanup();
		return false;
	}

	// Any-hit pipeline (RAY_MODE=1): early exit on first intersection.
	// Uses a specialization constant — the compiler eliminates the unused branch entirely.
	{
		TypedArray<RDPipelineSpecializationConstant> spec;
		Ref<RDPipelineSpecializationConstant> sc;
		sc.instantiate();
		sc->set_constant_id(0);
		sc->set_value(1); // RAY_MODE = 1 (any_hit)
		spec.push_back(sc);
		pipeline_any_hit_ = rd_->compute_pipeline_create(shader_, spec);
		if (!pipeline_any_hit_.is_valid()) {
			UtilityFunctions::print("[GPU RayCaster] Failed to create any-hit compute pipeline");
			cleanup();
			return false;
		}
	}

	RT_ASSERT(shader_.is_valid(), "initialize: shader must be valid after successful creation");
	RT_ASSERT(pipeline_.is_valid(), "initialize: nearest-hit pipeline must be valid after creation");
	RT_ASSERT(pipeline_any_hit_.is_valid(), "initialize: any-hit pipeline must be valid after creation");

	// ---- CWBVH shader + pipelines (optional — log warning but don't fail) ----
	{
		Ref<RDShaderSource> cwbvh_source;
		cwbvh_source.instantiate();
		cwbvh_source->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, String(CWBVH_TRAVERSE_GLSL));
		cwbvh_source->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);

		Ref<RDShaderSPIRV> cwbvh_spirv = rd_->shader_compile_spirv_from_source(cwbvh_source);
		bool cwbvh_ok = false;
		if (!cwbvh_spirv.is_null()) {
			String err = cwbvh_spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE);
			if (err.is_empty()) {
				cwbvh_shader_ = rd_->shader_create_from_spirv(cwbvh_spirv);
				if (cwbvh_shader_.is_valid()) {
					cwbvh_pipeline_ = rd_->compute_pipeline_create(cwbvh_shader_);
					TypedArray<RDPipelineSpecializationConstant> cwbvh_spec;
					Ref<RDPipelineSpecializationConstant> cwbvh_sc;
					cwbvh_sc.instantiate();
					cwbvh_sc->set_constant_id(0);
					cwbvh_sc->set_value(1);
					cwbvh_spec.push_back(cwbvh_sc);
					cwbvh_pipeline_any_hit_ = rd_->compute_pipeline_create(cwbvh_shader_, cwbvh_spec);
					cwbvh_ok = cwbvh_pipeline_.is_valid() && cwbvh_pipeline_any_hit_.is_valid();
				}
			} else {
				WARN_PRINT_ONCE(String("[GPU RayCaster] CWBVH shader compile error: ") + err);
			}
		}
		if (!cwbvh_ok) {
			WARN_PRINT_ONCE("[GPU RayCaster] CWBVH shader not available — will use Aila-Laine fallback");
		}
	}

	initialized_ = true;
	UtilityFunctions::print("[GPU RayCaster] Initialized successfully");
	return true;
}

bool GPURayCaster::is_available() const {
	return initialized_ && scene_uploaded_;
}

// ============================================================================
// Upload scene — convert and transfer triangle + BVH data to GPU
// ============================================================================

void GPURayCaster::upload_scene(const std::vector<Triangle> &triangles, const tinybvh::BVH &bvh2) {
	if (!initialized_ || triangles.empty()) { return; }
	RT_ASSERT(initialized_, "GPU must be initialized before upload_scene");
	RT_ASSERT(!triangles.empty(), "Cannot upload empty triangle array");

	// Drain any pending async dispatch before freeing buffers.
	if (pending_async_ && rd_) {
		rd_->sync();
		pending_async_ = false;
	}

	// ---- Convert triangles to GPU format ----
	std::vector<GPUTrianglePacked> gpu_tris(triangles.size());
	for (size_t i = 0; i < triangles.size(); i++) {
		const Triangle &t = triangles[i];
		GPUTrianglePacked &g = gpu_tris[i];
		g.v0[0] = t.v0.x; g.v0[1] = t.v0.y; g.v0[2] = t.v0.z;
		g.id = t.id;
		g.edge1[0] = t.edge1.x; g.edge1[1] = t.edge1.y; g.edge1[2] = t.edge1.z;
		g.layers = t.layers;
		g.edge2[0] = t.edge2.x; g.edge2[1] = t.edge2.y; g.edge2[2] = t.edge2.z;
		g._pad2 = 0.0f;
		g.normal[0] = t.normal.x; g.normal[1] = t.normal.y; g.normal[2] = t.normal.z;
		g._pad3 = 0.0f;
	}

	// ---- Convert TinyBVH BVH2 to Aila-Laine dual-AABB format ----
	// Based on "Understanding the Efficiency of Ray Traversal on GPUs"
	// (Aila & Laine, HPG 2009) and tinybvh's BVH_GPU layout.
	//
	// KEY OPTIMIZATION: Each node stores both children's AABBs.
	// During traversal, one memory fetch gives both children's bounds → test
	// both children and decide which to visit, all from one cache line.
	// The standard format requires TWO fetches (load child0, load child1).
	//
	// This halves memory latency during traversal — the #1 GPU bottleneck.
	//
	// TinyBVH BVH2 node layout (32 bytes, DFS order):
	//   bvhvec3 aabbMin; uint32_t leftFirst;  // isLeaf when triCount > 0
	//   bvhvec3 aabbMax; uint32_t triCount;
	//   Internal: left = leftFirst, right = leftFirst + 1
	//   Leaf: leftFirst = first tri index, triCount = # tris
	const uint32_t node_count = static_cast<uint32_t>(bvh2.NodeCount());
	RT_ASSERT(node_count > 0, "upload_scene: BVH2 must have nodes");
	const tinybvh::BVH::BVHNode *nodes = bvh2.bvhNode;

	std::vector<GPUBVHNodeWide> gpu_nodes;
	gpu_nodes.reserve(node_count);

	// Build a mapping from BVH2 node indices to wide-node indices.
	// Every node gets a slot so parent pointers stay valid.
	std::vector<uint32_t> node_map(node_count, 0);
	uint32_t wide_count = 0;
	for (uint32_t i = 0; i < node_count; i++) {
		node_map[i] = wide_count++;
	}
	gpu_nodes.resize(wide_count);

	for (uint32_t i = 0; i < node_count; i++) {
		const tinybvh::BVH::BVHNode &n = nodes[i];
		GPUBVHNodeWide &g = gpu_nodes[node_map[i]];

		if (n.isLeaf()) {
			if (i == 0) {
				// Root-as-leaf: small scene. Wrap as pseudo-internal so shader
				// traversal works (shader always processes root as internal).
				g.left_min[0] = n.aabbMin.x; g.left_min[1] = n.aabbMin.y; g.left_min[2] = n.aabbMin.z;
				g.left_max[0] = n.aabbMax.x; g.left_max[1] = n.aabbMax.y; g.left_max[2] = n.aabbMax.z;
				g.left_idx = n.leftFirst;
				g.left_count = n.triCount;
				// Unreachable right child: NaN AABB (IEEE 754: NaN comparisons → false).
				// NOTE: inverted AABB (min>max) does NOT work because the slab test's
				// min/max swap undoes the inversion. NaN is the only safe sentinel.
				const float qnan = std::numeric_limits<float>::quiet_NaN();
				g.right_min[0] = qnan; g.right_min[1] = qnan; g.right_min[2] = qnan;
				g.right_max[0] = qnan; g.right_max[1] = qnan; g.right_max[2] = qnan;
				g.right_idx = 0;
				g.right_count = 0;
			} else {
				// Non-root leaf: its parent already stores its AABB + tri info.
				// This entry is never read during traversal — fill with safe zeros.
				g = {};
			}
			continue;
		}

		// Internal node: TinyBVH DFS layout → left child = leftFirst, right child = leftFirst + 1
		uint32_t left = n.leftFirst;
		uint32_t right = n.leftFirst + 1;
		RT_ASSERT(left < node_count, "Left child out of bounds");
		RT_ASSERT(right < node_count, "Right child out of bounds");

		const tinybvh::BVH::BVHNode &lc = nodes[left];
		const tinybvh::BVH::BVHNode &rc = nodes[right];

		// Left child AABB
		g.left_min[0] = lc.aabbMin.x; g.left_min[1] = lc.aabbMin.y; g.left_min[2] = lc.aabbMin.z;
		g.left_max[0] = lc.aabbMax.x; g.left_max[1] = lc.aabbMax.y; g.left_max[2] = lc.aabbMax.z;

		// Right child AABB
		g.right_min[0] = rc.aabbMin.x; g.right_min[1] = rc.aabbMin.y; g.right_min[2] = rc.aabbMin.z;
		g.right_max[0] = rc.aabbMax.x; g.right_max[1] = rc.aabbMax.y; g.right_max[2] = rc.aabbMax.z;

		// Child info: if leaf, store first_tri + count. If internal, store node index.
		if (lc.isLeaf()) {
			g.left_idx = lc.leftFirst;
			g.left_count = lc.triCount;
		} else {
			g.left_idx = node_map[left];
			g.left_count = 0;
		}

		if (rc.isLeaf()) {
			g.right_idx = rc.leftFirst;
			g.right_count = rc.triCount;
		} else {
			g.right_idx = node_map[right];
			g.right_count = 0;
		}
	}

	// ---- Free old scene buffers ----
	free_scene_buffers();

	// ---- Upload triangle buffer ----
	{
		PackedByteArray data;
		uint32_t byte_size = static_cast<uint32_t>(gpu_tris.size() * sizeof(GPUTrianglePacked));
		data.resize(byte_size);
		memcpy(data.ptrw(), gpu_tris.data(), byte_size);
		triangle_buffer_ = rd_->storage_buffer_create(byte_size, data);
	}

	// ---- Upload Aila-Laine BVH node buffer ----
	{
		PackedByteArray data;
		uint32_t byte_size = static_cast<uint32_t>(gpu_nodes.size() * sizeof(GPUBVHNodeWide));
		data.resize(byte_size);
		memcpy(data.ptrw(), gpu_nodes.data(), byte_size);
		bvh_node_buffer_ = rd_->storage_buffer_create(byte_size, data);
	}

	uniform_set_dirty_ = true;
	scene_uploaded_ = true;

	UtilityFunctions::print("[GPU RayCaster] Uploaded: ",
		static_cast<int>(triangles.size()), " triangles, ",
		static_cast<int>(gpu_nodes.size()), " Aila-Laine BVH nodes");
}

// ============================================================================
// Upload CWBVH — ~1.5-2× faster GPU traversal via compressed wide BVH
// ============================================================================
// CWBVH (Ylitie et al. 2017) uses 8-wide BVH with quantized child AABBs.
// Node data: 80 bytes/node (5 × bvhvec4). Triangle data: 48 bytes/tri (3 × bvhvec4).
// The CWBVH shader also reads GPUTrianglePacked for layer mask + normal lookup,
// so upload_scene() MUST be called first.

void GPURayCaster::upload_cwbvh(const tinybvh::BVH8_CWBVH &cwbvh) {
	if (!initialized_ || !scene_uploaded_) {
		WARN_PRINT_ONCE("[GPU RayCaster] upload_cwbvh: scene must be uploaded first via upload_scene()");
		return;
	}
	if (!cwbvh_shader_.is_valid()) {
		WARN_PRINT_ONCE("[GPU RayCaster] upload_cwbvh: CWBVH shader not available");
		return;
	}
	RT_ASSERT(triangle_buffer_.is_valid(), "upload_cwbvh: triangle_buffer_ must be valid (call upload_scene first)");

	// Guard against unbuilt or corrupted CWBVH — null pointers would segfault in memcpy.
	if (cwbvh.bvh8Data == nullptr || cwbvh.bvh8Tris == nullptr || cwbvh.usedBlocks == 0 || cwbvh.triCount == 0) {
		WARN_PRINT_ONCE("[GPU RayCaster] upload_cwbvh: CWBVH not built or has null data — skipping");
		return;
	}

	// Drain any pending async dispatch before freeing buffers.
	if (pending_async_ && rd_) {
		rd_->sync();
		pending_async_ = false;
	}

	free_cwbvh_buffers();

	// ---- Upload CWBVH node data (raw bvhvec4 array) ----
	// usedBlocks = total bvhvec4 entries in bvh8Data (NOT node count).
	// Each node = 5 consecutive bvhvec4 = 80 bytes.  Node count = usedBlocks / 5.
	// Verified against TinyBVH Save(): s.write((char*)bvh8Data, usedBlocks * 16).
	RT_ASSERT(cwbvh.usedBlocks > 0, "upload_cwbvh: CWBVH must have at least one node block");
	RT_ASSERT(cwbvh.usedBlocks % 5 == 0, "upload_cwbvh: usedBlocks must be a multiple of 5");
	{
		uint32_t byte_size = static_cast<uint32_t>(cwbvh.usedBlocks * sizeof(tinybvh::bvhvec4));
		PackedByteArray data;
		data.resize(byte_size);
		memcpy(data.ptrw(), cwbvh.bvh8Data, byte_size);
		cwbvh_node_buffer_ = rd_->storage_buffer_create(byte_size, data);
	}

	// ---- Upload CWBVH triangle data (raw bvhvec4 array) ----
	// Each triangle = 3 × bvhvec4 = 48 bytes.  bvh8Tris is a separate allocation
	// with (triCount × 3) usable bvhvec4 entries (zeroed padding beyond that).
	// Without spatial splits (SBVH), triCount == idxCount == number of scene prims.
	{
		uint32_t tri_count = cwbvh.triCount;
		RT_ASSERT(tri_count > 0, "upload_cwbvh: CWBVH must have at least one triangle");
		uint32_t byte_size = static_cast<uint32_t>(tri_count * 3 * sizeof(tinybvh::bvhvec4));
		PackedByteArray data;
		data.resize(byte_size);
		memcpy(data.ptrw(), cwbvh.bvh8Tris, byte_size);
		cwbvh_tri_buffer_ = rd_->storage_buffer_create(byte_size, data);
	}

	cwbvh_uniform_set_dirty_ = true;
	cwbvh_uploaded_ = true;

	UtilityFunctions::print("[GPU RayCaster] Uploaded CWBVH: ",
		static_cast<int>(cwbvh.usedBlocks / 5), " nodes (",
		static_cast<int>(cwbvh.usedBlocks), " vec4 blocks), ",
		static_cast<int>(cwbvh.triCount), " triangles");
}

// ============================================================================
// Cast rays (nearest hit) — find closest intersection per ray
// ============================================================================

void GPURayCaster::cast_rays(const Ray *rays, Intersection *results, int count,
		uint32_t query_mask) {
	if (!is_available() || count <= 0) { return; }
	RT_ASSERT_NOT_NULL(rays);
	RT_ASSERT_NOT_NULL(results);
	RT_ASSERT(count > 0, "GPURayCaster::cast_rays: count must be positive");

	// Batched dispatch — keeps individual GPU submissions under TDR timeout
	// and avoids oversized staging-buffer uploads that can trigger internal
	// Vulkan flushes inside Godot's RenderingDevice.
	for (int offset = 0; offset < count; offset += MAX_GPU_BATCH_SIZE) {
		int batch = std::min(count - offset, MAX_GPU_BATCH_SIZE);

		dispatch_rays_internal(rays + offset, batch, pipeline_, query_mask);

		// ---- Read back batch results ----
		uint32_t result_bytes = static_cast<uint32_t>(batch * sizeof(GPUIntersectionPacked));
		PackedByteArray result_data = rd_->buffer_get_data(result_buffer_, 0, result_bytes);

		const GPUIntersectionPacked *gpu_results =
			reinterpret_cast<const GPUIntersectionPacked *>(result_data.ptr());

		// ---- Convert GPU results back to Intersection structs ----
		// Position is reconstructed from ray origin + direction * t instead of
		// being stored in the GPU output. This saves 33% readback bandwidth.
		for (int i = 0; i < batch; i++) {
			const GPUIntersectionPacked &g = gpu_results[i];
			Intersection &hit = results[offset + i];
			hit.t = g.t;
			hit.normal = Vector3(g.normal[0], g.normal[1], g.normal[2]);
			hit.u = g.bary_u;
			hit.v = g.bary_v;
			if (g.prim_id >= 0) {
				hit.prim_id = static_cast<uint32_t>(g.prim_id);
				hit.hit_layers = g.hit_layers;
				hit.position = rays[offset + i].origin + rays[offset + i].direction * g.t;
			} else {
				hit.set_miss();
			}
		}
	}
}

// ============================================================================
// Cast rays (any hit) — shadow/occlusion queries, early exit on first hit
// ============================================================================

void GPURayCaster::cast_rays_any_hit(const Ray *rays, bool *hit_results, int count,
		uint32_t query_mask) {
	if (!is_available() || count <= 0) { return; }
	RT_ASSERT_NOT_NULL(rays);
	RT_ASSERT_NOT_NULL(hit_results);
	RT_ASSERT(count > 0, "GPURayCaster::cast_rays_any_hit: count must be positive");

	// Batched dispatch — same rationale as cast_rays.
	for (int offset = 0; offset < count; offset += MAX_GPU_BATCH_SIZE) {
		int batch = std::min(count - offset, MAX_GPU_BATCH_SIZE);

		dispatch_rays_internal(rays + offset, batch, pipeline_any_hit_, query_mask);

		// ---- Read back batch results and extract hit/miss booleans ----
		uint32_t result_bytes = static_cast<uint32_t>(batch * sizeof(GPUIntersectionPacked));
		PackedByteArray result_data = rd_->buffer_get_data(result_buffer_, 0, result_bytes);

		const GPUIntersectionPacked *gpu_results =
			reinterpret_cast<const GPUIntersectionPacked *>(result_data.ptr());

		for (int i = 0; i < batch; i++) {
			hit_results[offset + i] = (gpu_results[i].prim_id >= 0);
		}
	}
}

// ============================================================================
// Internal: shared dispatch logic for both ray casting modes
// ============================================================================

void GPURayCaster::dispatch_rays_internal(const Ray *rays, int count, const RID &pipeline,
		uint32_t query_mask) {
	dispatch_rays_no_sync(rays, count, pipeline, query_mask);

	// Breadcrumb: printed BEFORE sync so that if the sync blocks forever,
	// this is the last line in stdout — pinpointing the hang.
	// Only logged when count changes (i.e. resolution change) or periodically.
	static int last_logged_count = 0;
	static int sync_counter = 0;
	bool should_log = (count != last_logged_count) || (sync_counter % 300 == 0);
	if (should_log) {
		UtilityFunctions::print("[GPU RayCaster] >>> sync (",
				String::num(count), " rays, capacity=",
				String::num(ray_buffer_capacity_), ")");
		last_logged_count = count;
	}
	sync_counter++;

	// Timed sync — detect GPU stalls that could indicate driver/TDR issues.
	// The Windows TDR (Timeout Detection and Recovery) kills GPU work taking
	// longer than ~2 seconds (TdrDelay registry value). If sync approaches that
	// threshold, something is seriously wrong (corrupt BVH data, driver bug, etc.).
	auto sync_start = std::chrono::high_resolution_clock::now();
	rd_->sync();
	auto sync_end = std::chrono::high_resolution_clock::now();
	float sync_ms = std::chrono::duration<float, std::milli>(sync_end - sync_start).count();

	if (should_log) {
		UtilityFunctions::print("[GPU RayCaster] <<< sync done (",
				String::num(sync_ms, 1), " ms)");
	}

	if (sync_ms > 200.0f) {
		WARN_PRINT(String("[GPU RayCaster] rd_->sync() took ") + String::num(sync_ms, 1)
				+ " ms (" + String::num(count) + " rays) — approaching TDR timeout!");
	}
}

// ============================================================================
// Async dispatch — submit without blocking
// ============================================================================

void GPURayCaster::submit_async(const Ray *rays, int count, uint32_t query_mask) {
	if (!is_available() || count <= 0) { return; }
	RT_ASSERT(!pending_async_, "submit_async called while previous dispatch is still pending");
	RT_ASSERT_NOT_NULL(rays);
	dispatch_rays_no_sync(rays, count, pipeline_, query_mask);
	pending_count_ = count;
	pending_rays_ = rays;
	pending_async_ = true;
}

void GPURayCaster::submit_async_any_hit(const Ray *rays, int count, uint32_t query_mask) {
	if (!is_available() || count <= 0) { return; }
	RT_ASSERT(!pending_async_, "submit_async_any_hit called while previous dispatch is still pending");
	RT_ASSERT_NOT_NULL(rays);
	dispatch_rays_no_sync(rays, count, pipeline_any_hit_, query_mask);
	pending_count_ = count;
	pending_rays_ = rays;
	pending_async_ = true;
}

void GPURayCaster::collect_nearest(Intersection *results, int count) {
	if (!pending_async_ || !rd_) { return; }
	RT_ASSERT(pending_async_, "collect_nearest called without pending async dispatch");
	RT_ASSERT_NOT_NULL(results);
	RT_ASSERT_NOT_NULL(pending_rays_);

	auto sync_start = std::chrono::high_resolution_clock::now();
	rd_->sync();
	auto sync_end = std::chrono::high_resolution_clock::now();
	float sync_ms = std::chrono::duration<float, std::milli>(sync_end - sync_start).count();
	if (sync_ms > 500.0f) {
		WARN_PRINT(String("[GPU RayCaster] collect_nearest sync took ") + String::num(sync_ms, 1)
				+ " ms (" + String::num(pending_count_) + " rays) — approaching TDR timeout!");
	}
	pending_async_ = false;

	int read_count = (count < pending_count_) ? count : pending_count_;
	uint32_t result_bytes = static_cast<uint32_t>(read_count * sizeof(GPUIntersectionPacked));
	PackedByteArray result_data = rd_->buffer_get_data(result_buffer_, 0, result_bytes);

	const GPUIntersectionPacked *gpu_results =
		reinterpret_cast<const GPUIntersectionPacked *>(result_data.ptr());

	for (int i = 0; i < read_count; i++) {
		const GPUIntersectionPacked &g = gpu_results[i];
		Intersection &hit = results[i];
		hit.t = g.t;
		hit.normal = Vector3(g.normal[0], g.normal[1], g.normal[2]);
		hit.u = g.bary_u;
		hit.v = g.bary_v;
		if (g.prim_id >= 0) {
			hit.prim_id = static_cast<uint32_t>(g.prim_id);
			hit.hit_layers = g.hit_layers;
			hit.position = pending_rays_[i].origin + pending_rays_[i].direction * g.t;
		} else {
			hit.set_miss();
		}
	}

	pending_rays_ = nullptr;
}

void GPURayCaster::collect_any_hit(bool *hit_results, int count) {
	if (!pending_async_ || !rd_) { return; }
	RT_ASSERT(pending_async_, "collect_any_hit called without pending async dispatch");
	RT_ASSERT_NOT_NULL(hit_results);

	auto sync_start = std::chrono::high_resolution_clock::now();
	rd_->sync();
	auto sync_end = std::chrono::high_resolution_clock::now();
	float sync_ms = std::chrono::duration<float, std::milli>(sync_end - sync_start).count();
	if (sync_ms > 500.0f) {
		WARN_PRINT(String("[GPU RayCaster] collect_any_hit sync took ") + String::num(sync_ms, 1)
				+ " ms (" + String::num(pending_count_) + " rays) — approaching TDR timeout!");
	}
	pending_async_ = false;

	int read_count = (count < pending_count_) ? count : pending_count_;
	uint32_t result_bytes = static_cast<uint32_t>(read_count * sizeof(GPUIntersectionPacked));
	PackedByteArray result_data = rd_->buffer_get_data(result_buffer_, 0, result_bytes);

	const GPUIntersectionPacked *gpu_results =
		reinterpret_cast<const GPUIntersectionPacked *>(result_data.ptr());

	for (int i = 0; i < read_count; i++) {
		hit_results[i] = (gpu_results[i].prim_id >= 0);
	}
}

// ============================================================================
// Internal: dispatch without sync (used by both sync and async paths)
// ============================================================================

void GPURayCaster::dispatch_rays_no_sync(const Ray *rays, int count, const RID &pipeline,
		uint32_t query_mask) {
	RT_ASSERT(count > 0, "dispatch_rays_no_sync: count must be positive");
	RT_ASSERT_NOT_NULL(rays);
	RT_ASSERT(rd_ != nullptr, "dispatch_rays_no_sync: rendering device is null");

	// ---- Ensure ray/result buffers are large enough ----
	ensure_ray_buffers(static_cast<uint32_t>(count));

	// ---- Convert rays directly into upload buffer (single copy, no intermediate) ----
	{
		uint32_t byte_size = static_cast<uint32_t>(count * sizeof(GPURayPacked));
		upload_cache_.resize(byte_size);
		GPURayPacked *dst = reinterpret_cast<GPURayPacked *>(upload_cache_.ptrw());
		for (int i = 0; i < count; i++) {
			const Ray &r = rays[i];
			GPURayPacked &g = dst[i];
			g.origin[0] = r.origin.x; g.origin[1] = r.origin.y; g.origin[2] = r.origin.z;
			g.t_max = r.t_max;
			g.direction[0] = r.direction.x; g.direction[1] = r.direction.y; g.direction[2] = r.direction.z;
			g.t_min = r.t_min;
		}
		rd_->buffer_update(ray_buffer_, 0, byte_size, upload_cache_);
	}

	// ---- Select CWBVH or Aila-Laine pipeline + uniform set ----
	// CWBVH is ~1.5-2× faster; use it when available, fall back to Aila-Laine.
	RID actual_pipeline = pipeline;
	RID actual_uniform_set;

	if (cwbvh_uploaded_ && cwbvh_pipeline_.is_valid()) {
		// Map the Aila-Laine pipeline to its CWBVH equivalent.
		if (pipeline == pipeline_) {
			actual_pipeline = cwbvh_pipeline_;
		} else if (pipeline == pipeline_any_hit_) {
			actual_pipeline = cwbvh_pipeline_any_hit_;
		}
		rebuild_cwbvh_uniform_set();
		actual_uniform_set = cwbvh_uniform_set_;
	} else {
		rebuild_uniform_set();
		actual_uniform_set = uniform_set_;
	}

	// ---- Push constants (reuse cached PackedByteArray) ----
	GPUPushConstants push{};
	push.ray_count = static_cast<uint32_t>(count);
	push.query_mask = query_mask;
	if (push_data_cache_.size() != sizeof(GPUPushConstants)) {
		push_data_cache_.resize(sizeof(GPUPushConstants));
	}
	memcpy(push_data_cache_.ptrw(), &push, sizeof(GPUPushConstants));

	// ---- Dispatch compute shader ----
	uint32_t groups_x = (static_cast<uint32_t>(count) + WORKGROUP_SIZE - 1) / WORKGROUP_SIZE;

	int64_t compute_list = rd_->compute_list_begin();
	rd_->compute_list_bind_compute_pipeline(compute_list, actual_pipeline);
	rd_->compute_list_bind_uniform_set(compute_list, actual_uniform_set, 0);
	rd_->compute_list_set_push_constant(compute_list, push_data_cache_, sizeof(GPUPushConstants));
	rd_->compute_list_dispatch(compute_list, groups_x, 1, 1);
	rd_->compute_list_end();

	// Submit but do NOT sync — caller is responsible for sync + readback.
	rd_->submit();
}

// ============================================================================
// Cleanup — free all GPU resources
// ============================================================================

void GPURayCaster::cleanup() {
	if (rd_) {
		RT_ASSERT(rd_ != nullptr, "cleanup: rendering device must be valid before freeing resources");
		free_ray_buffers();
		free_cwbvh_buffers();
		free_scene_buffers();

		if (uniform_set_.is_valid())      { rd_->free_rid(uniform_set_);      uniform_set_ = RID(); }
		if (pipeline_any_hit_.is_valid())  { rd_->free_rid(pipeline_any_hit_);  pipeline_any_hit_ = RID(); }
		if (pipeline_.is_valid())          { rd_->free_rid(pipeline_);          pipeline_ = RID(); }
		if (shader_.is_valid())            { rd_->free_rid(shader_);            shader_ = RID(); }

		// CWBVH pipeline/shader cleanup
		if (cwbvh_uniform_set_.is_valid())       { rd_->free_rid(cwbvh_uniform_set_);       cwbvh_uniform_set_ = RID(); }
		if (cwbvh_pipeline_any_hit_.is_valid())   { rd_->free_rid(cwbvh_pipeline_any_hit_);   cwbvh_pipeline_any_hit_ = RID(); }
		if (cwbvh_pipeline_.is_valid())           { rd_->free_rid(cwbvh_pipeline_);           cwbvh_pipeline_ = RID(); }
		if (cwbvh_shader_.is_valid())             { rd_->free_rid(cwbvh_shader_);             cwbvh_shader_ = RID(); }

		memdelete(rd_);
		rd_ = nullptr;
	}

	initialized_ = false;
	scene_uploaded_ = false;
	cwbvh_uploaded_ = false;
	uniform_set_dirty_ = true;
	cwbvh_uniform_set_dirty_ = true;
	ray_buffer_capacity_ = 0;

	RT_ASSERT(rd_ == nullptr, "cleanup: rendering device must be null after cleanup");
	RT_ASSERT(!initialized_, "cleanup: initialized_ must be false after cleanup");
}

// ============================================================================
// Internal helpers
// ============================================================================

void GPURayCaster::free_scene_buffers() {
	if (!rd_) { return; }
	RT_ASSERT(rd_ != nullptr, "free_scene_buffers: rendering device must be valid");
	// CWBVH uniform set references triangle_buffer_ — must free first.
	free_cwbvh_buffers();
	// Free uniform set FIRST — it holds references to these buffers.
	if (uniform_set_.is_valid())       { rd_->free_rid(uniform_set_);       uniform_set_ = RID(); }
	if (triangle_buffer_.is_valid())   { rd_->free_rid(triangle_buffer_);   triangle_buffer_ = RID(); }
	if (bvh_node_buffer_.is_valid())   { rd_->free_rid(bvh_node_buffer_);   bvh_node_buffer_ = RID(); }
	uniform_set_dirty_ = true;
	scene_uploaded_ = false;
	RT_ASSERT(!scene_uploaded_, "free_scene_buffers: scene_uploaded_ must be false after cleanup");
}

void GPURayCaster::free_cwbvh_buffers() {
	if (!rd_) { return; }
	RT_ASSERT_NOT_NULL(rd_);
	if (cwbvh_uniform_set_.is_valid())  { rd_->free_rid(cwbvh_uniform_set_);  cwbvh_uniform_set_ = RID(); }
	if (cwbvh_node_buffer_.is_valid()) { rd_->free_rid(cwbvh_node_buffer_); cwbvh_node_buffer_ = RID(); }
	if (cwbvh_tri_buffer_.is_valid())  { rd_->free_rid(cwbvh_tri_buffer_);  cwbvh_tri_buffer_ = RID(); }
	cwbvh_uniform_set_dirty_ = true;
	cwbvh_uploaded_ = false;
	RT_ASSERT(!cwbvh_uploaded_ && cwbvh_uniform_set_dirty_, "free_cwbvh_buffers: post-condition failed");
}

void GPURayCaster::free_ray_buffers() {
	if (!rd_) { return; }
	RT_ASSERT(rd_ != nullptr, "free_ray_buffers: rendering device must be valid");
	// Free BOTH uniform sets — they reference ray/result buffers.
	if (uniform_set_.is_valid())       { rd_->free_rid(uniform_set_);       uniform_set_ = RID(); }
	if (cwbvh_uniform_set_.is_valid()) { rd_->free_rid(cwbvh_uniform_set_); cwbvh_uniform_set_ = RID(); }
	if (ray_buffer_.is_valid())        { rd_->free_rid(ray_buffer_);        ray_buffer_ = RID(); }
	if (result_buffer_.is_valid())     { rd_->free_rid(result_buffer_);     result_buffer_ = RID(); }
	uniform_set_dirty_ = true;
	cwbvh_uniform_set_dirty_ = true;
	ray_buffer_capacity_ = 0;
	RT_ASSERT(ray_buffer_capacity_ == 0, "free_ray_buffers: capacity must be zero after cleanup");
}

void GPURayCaster::ensure_ray_buffers(uint32_t ray_count) {
	if (ray_count <= ray_buffer_capacity_) { return; }
	RT_ASSERT(ray_count > 0, "ensure_ray_buffers: ray_count must be positive");
	RT_ASSERT(rd_ != nullptr, "ensure_ray_buffers: rendering device must be valid");

	// Grow with 1.5x factor to reduce reallocation frequency.
	uint32_t new_capacity = ray_count;
	if (ray_buffer_capacity_ > 0) {
		new_capacity = static_cast<uint32_t>(ray_count * 1.5f);
	}

	uint32_t old_capacity = ray_buffer_capacity_;

	free_ray_buffers();

	// Flush deferred resource deletions before creating new buffers.
	// Godot's RenderingDevice may defer free_rid() processing until the next
	// submit/sync cycle. Without this flush, the Vulkan memory allocator could
	// reuse the same GPU memory for new buffers while old descriptors still
	// reference it — causing corruption or driver hangs on some GPUs.
	rd_->submit();
	rd_->sync();

	uint32_t ray_bytes = new_capacity * sizeof(GPURayPacked);
	uint32_t result_bytes = new_capacity * sizeof(GPUIntersectionPacked);

	// Create with empty data — we'll fill via buffer_update before dispatch.
	ray_buffer_ = rd_->storage_buffer_create(ray_bytes);
	result_buffer_ = rd_->storage_buffer_create(result_bytes);

	RT_ASSERT(ray_buffer_.is_valid(), "ensure_ray_buffers: ray buffer creation failed");
	RT_ASSERT(result_buffer_.is_valid(), "ensure_ray_buffers: result buffer creation failed");

	ray_buffer_capacity_ = new_capacity;
	uniform_set_dirty_ = true;
	cwbvh_uniform_set_dirty_ = true;

	UtilityFunctions::print("[GPU RayCaster] Buffer realloc: ",
			old_capacity, " → ", new_capacity,
			" rays (", String::num(ray_bytes / 1024), " KB ray + ",
			String::num(result_bytes / 1024), " KB result)");
}

void GPURayCaster::rebuild_uniform_set() {
	if (!uniform_set_dirty_) { return; }

	if (uniform_set_.is_valid()) {
		rd_->free_rid(uniform_set_);
		uniform_set_ = RID();
	}

	RT_ASSERT(rd_ != nullptr, "rebuild_uniform_set: rendering device must be valid");
	RT_ASSERT(shader_.is_valid(), "rebuild_uniform_set: shader must be valid to create uniform set");

	// All 4 buffers must be valid to create the uniform set.
	if (!triangle_buffer_.is_valid() || !bvh_node_buffer_.is_valid() ||
		!ray_buffer_.is_valid() || !result_buffer_.is_valid()) {
		return;
	}

	TypedArray<RDUniform> uniforms;

	// Binding 0: triangles (readonly)
	{
		Ref<RDUniform> u;
		u.instantiate();
		u->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
		u->set_binding(0);
		u->add_id(triangle_buffer_);
		uniforms.push_back(u);
	}

	// Binding 1: BVH nodes (readonly)
	{
		Ref<RDUniform> u;
		u.instantiate();
		u->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
		u->set_binding(1);
		u->add_id(bvh_node_buffer_);
		uniforms.push_back(u);
	}

	// Binding 2: rays (readonly)
	{
		Ref<RDUniform> u;
		u.instantiate();
		u->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
		u->set_binding(2);
		u->add_id(ray_buffer_);
		uniforms.push_back(u);
	}

	// Binding 3: results (writeonly)
	{
		Ref<RDUniform> u;
		u.instantiate();
		u->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
		u->set_binding(3);
		u->add_id(result_buffer_);
		uniforms.push_back(u);
	}

	uniform_set_ = rd_->uniform_set_create(uniforms, shader_, 0);
	RT_ASSERT(uniform_set_.is_valid(), "rebuild_uniform_set: uniform set creation failed");
	uniform_set_dirty_ = false;
}

void GPURayCaster::rebuild_cwbvh_uniform_set() {
	if (!cwbvh_uniform_set_dirty_) { return; }

	if (cwbvh_uniform_set_.is_valid()) {
		rd_->free_rid(cwbvh_uniform_set_);
		cwbvh_uniform_set_ = RID();
	}

	RT_ASSERT(rd_ != nullptr, "rebuild_cwbvh_uniform_set: rendering device must be valid");
	RT_ASSERT(cwbvh_shader_.is_valid(), "rebuild_cwbvh_uniform_set: CWBVH shader must be valid");

	// All 5 buffers must be valid.
	if (!cwbvh_node_buffer_.is_valid() || !cwbvh_tri_buffer_.is_valid() ||
		!triangle_buffer_.is_valid() || !ray_buffer_.is_valid() || !result_buffer_.is_valid()) {
		return;
	}

	TypedArray<RDUniform> uniforms;

	// Binding 0: CWBVH node data (raw vec4 array, 5 per node)
	{
		Ref<RDUniform> u;
		u.instantiate();
		u->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
		u->set_binding(0);
		u->add_id(cwbvh_node_buffer_);
		uniforms.push_back(u);
	}

	// Binding 1: CWBVH triangle data (raw vec4 array, 3 per tri)
	{
		Ref<RDUniform> u;
		u.instantiate();
		u->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
		u->set_binding(1);
		u->add_id(cwbvh_tri_buffer_);
		uniforms.push_back(u);
	}

	// Binding 2: Scene triangles (GPUTrianglePacked, for layer mask + normal lookup)
	{
		Ref<RDUniform> u;
		u.instantiate();
		u->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
		u->set_binding(2);
		u->add_id(triangle_buffer_);
		uniforms.push_back(u);
	}

	// Binding 3: Rays (readonly)
	{
		Ref<RDUniform> u;
		u.instantiate();
		u->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
		u->set_binding(3);
		u->add_id(ray_buffer_);
		uniforms.push_back(u);
	}

	// Binding 4: Results (writeonly)
	{
		Ref<RDUniform> u;
		u.instantiate();
		u->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
		u->set_binding(4);
		u->add_id(result_buffer_);
		uniforms.push_back(u);
	}

	cwbvh_uniform_set_ = rd_->uniform_set_create(uniforms, cwbvh_shader_, 0);
	RT_ASSERT(cwbvh_uniform_set_.is_valid(), "rebuild_cwbvh_uniform_set: uniform set creation failed");
	cwbvh_uniform_set_dirty_ = false;
}
