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
#include "bvh_traverse_comp.h"

#include "core/ray.h"
#include "core/intersection.h"
#include "core/triangle.h"
#include "accel/bvh.h"
#include "core/asserts.h"

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

#include <cstring>
#include <vector>

using namespace godot;

// Workgroup size — must match local_size_x in the GLSL shader.
static constexpr uint32_t WORKGROUP_SIZE = 64;

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
	if (initialized_) return true;

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

void GPURayCaster::upload_scene(const std::vector<Triangle> &triangles, const BVH &bvh) {
	if (!initialized_ || triangles.empty() || !bvh.is_built()) return;
	RT_ASSERT(initialized_, "GPU must be initialized before upload_scene");
	RT_ASSERT(!triangles.empty(), "Cannot upload empty triangle array");

	// ---- Convert triangles to GPU format ----
	std::vector<GPUTrianglePacked> gpu_tris(triangles.size());
	for (size_t i = 0; i < triangles.size(); i++) {
		const Triangle &t = triangles[i];
		GPUTrianglePacked &g = gpu_tris[i];
		g.v0[0] = t.v0.x; g.v0[1] = t.v0.y; g.v0[2] = t.v0.z;
		g.id = t.id;
		g.edge1[0] = t.edge1.x; g.edge1[1] = t.edge1.y; g.edge1[2] = t.edge1.z;
		g._pad1 = 0.0f;
		g.edge2[0] = t.edge2.x; g.edge2[1] = t.edge2.y; g.edge2[2] = t.edge2.z;
		g._pad2 = 0.0f;
		g.normal[0] = t.normal.x; g.normal[1] = t.normal.y; g.normal[2] = t.normal.z;
		g._pad3 = 0.0f;
	}

	// ---- Convert BVH nodes to GPU format ----
	const std::vector<BVHNode> &nodes = bvh.get_nodes();
	std::vector<GPUBVHNodePacked> gpu_nodes(nodes.size());
	for (size_t i = 0; i < nodes.size(); i++) {
		const BVHNode &n = nodes[i];
		GPUBVHNodePacked &g = gpu_nodes[i];
		// Godot AABB: position = min corner, size = extent
		Vector3 bmin = n.bounds.position;
		Vector3 bmax = bmin + n.bounds.size;
		g.bounds_min[0] = bmin.x; g.bounds_min[1] = bmin.y; g.bounds_min[2] = bmin.z;
		g.left_first = n.left_first;
		g.bounds_max[0] = bmax.x; g.bounds_max[1] = bmax.y; g.bounds_max[2] = bmax.z;
		g.count = n.count;
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

	// ---- Upload BVH node buffer ----
	{
		PackedByteArray data;
		uint32_t byte_size = static_cast<uint32_t>(gpu_nodes.size() * sizeof(GPUBVHNodePacked));
		data.resize(byte_size);
		memcpy(data.ptrw(), gpu_nodes.data(), byte_size);
		bvh_node_buffer_ = rd_->storage_buffer_create(byte_size, data);
	}

	uniform_set_dirty_ = true;
	scene_uploaded_ = true;

	UtilityFunctions::print("[GPU RayCaster] Uploaded: ",
		static_cast<int>(triangles.size()), " triangles, ",
		static_cast<int>(nodes.size()), " BVH nodes");
}

// ============================================================================
// Cast rays (nearest hit) — find closest intersection per ray
// ============================================================================

void GPURayCaster::cast_rays(const Ray *rays, Intersection *results, int count) {
	if (!is_available() || count <= 0) return;
	RT_ASSERT_NOT_NULL(rays);
	RT_ASSERT_NOT_NULL(results);
	RT_ASSERT(count > 0, "GPURayCaster::cast_rays: count must be positive");

	dispatch_rays_internal(rays, count, pipeline_);

	// ---- Read back results ----
	uint32_t result_bytes = static_cast<uint32_t>(count * sizeof(GPUIntersectionPacked));
	PackedByteArray result_data = rd_->buffer_get_data(result_buffer_, 0, result_bytes);

	const GPUIntersectionPacked *gpu_results =
		reinterpret_cast<const GPUIntersectionPacked *>(result_data.ptr());

	// ---- Convert GPU results back to Intersection structs ----
	for (int i = 0; i < count; i++) {
		const GPUIntersectionPacked &g = gpu_results[i];
		Intersection &hit = results[i];
		hit.t = g.t;
		hit.position = Vector3(g.position[0], g.position[1], g.position[2]);
		hit.normal = Vector3(g.normal[0], g.normal[1], g.normal[2]);
		if (g.prim_id >= 0) {
			hit.prim_id = static_cast<uint32_t>(g.prim_id);
		} else {
			hit.set_miss();
		}
	}
}

// ============================================================================
// Cast rays (any hit) — shadow/occlusion queries, early exit on first hit
// ============================================================================

void GPURayCaster::cast_rays_any_hit(const Ray *rays, bool *hit_results, int count) {
	if (!is_available() || count <= 0) return;
	RT_ASSERT_NOT_NULL(rays);
	RT_ASSERT_NOT_NULL(hit_results);
	RT_ASSERT(count > 0, "GPURayCaster::cast_rays_any_hit: count must be positive");

	dispatch_rays_internal(rays, count, pipeline_any_hit_);

	// ---- Read back results and extract hit/miss booleans ----
	uint32_t result_bytes = static_cast<uint32_t>(count * sizeof(GPUIntersectionPacked));
	PackedByteArray result_data = rd_->buffer_get_data(result_buffer_, 0, result_bytes);

	const GPUIntersectionPacked *gpu_results =
		reinterpret_cast<const GPUIntersectionPacked *>(result_data.ptr());

	for (int i = 0; i < count; i++) {
		hit_results[i] = (gpu_results[i].prim_id >= 0);
	}
}

// ============================================================================
// Internal: shared dispatch logic for both ray casting modes
// ============================================================================

void GPURayCaster::dispatch_rays_internal(const Ray *rays, int count, const RID &pipeline) {
	dispatch_rays_no_sync(rays, count, pipeline);
	rd_->sync();
}

// ============================================================================
// Async dispatch — submit without blocking
// ============================================================================

void GPURayCaster::submit_async(const Ray *rays, int count) {
	if (!is_available() || count <= 0) return;
	RT_ASSERT(!pending_async_, "submit_async called while previous dispatch is still pending");
	RT_ASSERT_NOT_NULL(rays);
	dispatch_rays_no_sync(rays, count, pipeline_);
	pending_count_ = count;
	pending_async_ = true;
}

void GPURayCaster::submit_async_any_hit(const Ray *rays, int count) {
	if (!is_available() || count <= 0) return;
	RT_ASSERT(!pending_async_, "submit_async_any_hit called while previous dispatch is still pending");
	RT_ASSERT_NOT_NULL(rays);
	dispatch_rays_no_sync(rays, count, pipeline_any_hit_);
	pending_count_ = count;
	pending_async_ = true;
}

void GPURayCaster::collect_nearest(Intersection *results, int count) {
	if (!pending_async_ || !rd_) return;
	RT_ASSERT(pending_async_, "collect_nearest called without pending async dispatch");
	RT_ASSERT_NOT_NULL(results);

	rd_->sync();
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
		hit.position = Vector3(g.position[0], g.position[1], g.position[2]);
		hit.normal = Vector3(g.normal[0], g.normal[1], g.normal[2]);
		if (g.prim_id >= 0) {
			hit.prim_id = static_cast<uint32_t>(g.prim_id);
		} else {
			hit.set_miss();
		}
	}
}

void GPURayCaster::collect_any_hit(bool *hit_results, int count) {
	if (!pending_async_ || !rd_) return;
	RT_ASSERT(pending_async_, "collect_any_hit called without pending async dispatch");
	RT_ASSERT_NOT_NULL(hit_results);

	rd_->sync();
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

void GPURayCaster::dispatch_rays_no_sync(const Ray *rays, int count, const RID &pipeline) {
	RT_ASSERT(count > 0, "dispatch_rays_no_sync: count must be positive");
	RT_ASSERT_NOT_NULL(rays);
	RT_ASSERT(rd_ != nullptr, "dispatch_rays_no_sync: rendering device is null");
	// ---- Convert rays to GPU format ----
	std::vector<GPURayPacked> gpu_rays(count);
	for (int i = 0; i < count; i++) {
		const Ray &r = rays[i];
		GPURayPacked &g = gpu_rays[i];
		g.origin[0] = r.origin.x; g.origin[1] = r.origin.y; g.origin[2] = r.origin.z;
		g.t_max = r.t_max;
		g.direction[0] = r.direction.x; g.direction[1] = r.direction.y; g.direction[2] = r.direction.z;
		g.t_min = r.t_min;
	}

	// ---- Ensure ray/result buffers are large enough ----
	ensure_ray_buffers(static_cast<uint32_t>(count));

	// ---- Upload ray data ----
	{
		PackedByteArray data;
		uint32_t byte_size = static_cast<uint32_t>(count * sizeof(GPURayPacked));
		data.resize(byte_size);
		memcpy(data.ptrw(), gpu_rays.data(), byte_size);
		rd_->buffer_update(ray_buffer_, 0, byte_size, data);
	}

	// ---- Rebuild uniform set if needed ----
	rebuild_uniform_set();

	// ---- Push constants ----
	GPUPushConstants push{};
	push.ray_count = static_cast<uint32_t>(count);
	PackedByteArray push_data;
	push_data.resize(sizeof(GPUPushConstants));
	memcpy(push_data.ptrw(), &push, sizeof(GPUPushConstants));

	// ---- Dispatch compute shader ----
	uint32_t groups_x = (static_cast<uint32_t>(count) + WORKGROUP_SIZE - 1) / WORKGROUP_SIZE;

	int64_t compute_list = rd_->compute_list_begin();
	rd_->compute_list_bind_compute_pipeline(compute_list, pipeline);
	rd_->compute_list_bind_uniform_set(compute_list, uniform_set_, 0);
	rd_->compute_list_set_push_constant(compute_list, push_data, sizeof(GPUPushConstants));
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
		free_ray_buffers();
		free_scene_buffers();

		if (uniform_set_.is_valid())      { rd_->free_rid(uniform_set_);      uniform_set_ = RID(); }
		if (pipeline_any_hit_.is_valid())  { rd_->free_rid(pipeline_any_hit_);  pipeline_any_hit_ = RID(); }
		if (pipeline_.is_valid())          { rd_->free_rid(pipeline_);          pipeline_ = RID(); }
		if (shader_.is_valid())            { rd_->free_rid(shader_);            shader_ = RID(); }

		memdelete(rd_);
		rd_ = nullptr;
	}

	initialized_ = false;
	scene_uploaded_ = false;
	uniform_set_dirty_ = true;
	ray_buffer_capacity_ = 0;
}

// ============================================================================
// Internal helpers
// ============================================================================

void GPURayCaster::free_scene_buffers() {
	if (!rd_) return;
	if (triangle_buffer_.is_valid())  { rd_->free_rid(triangle_buffer_);  triangle_buffer_ = RID(); }
	if (bvh_node_buffer_.is_valid())  { rd_->free_rid(bvh_node_buffer_);  bvh_node_buffer_ = RID(); }
	uniform_set_dirty_ = true;
	scene_uploaded_ = false;
}

void GPURayCaster::free_ray_buffers() {
	if (!rd_) return;
	if (ray_buffer_.is_valid())    { rd_->free_rid(ray_buffer_);    ray_buffer_ = RID(); }
	if (result_buffer_.is_valid()) { rd_->free_rid(result_buffer_);  result_buffer_ = RID(); }
	uniform_set_dirty_ = true;
	ray_buffer_capacity_ = 0;
}

void GPURayCaster::ensure_ray_buffers(uint32_t ray_count) {
	if (ray_count <= ray_buffer_capacity_) return;

	// Grow with 1.5x factor to reduce reallocation frequency.
	uint32_t new_capacity = ray_count;
	if (ray_buffer_capacity_ > 0) {
		new_capacity = static_cast<uint32_t>(ray_count * 1.5f);
	}

	free_ray_buffers();

	uint32_t ray_bytes = new_capacity * sizeof(GPURayPacked);
	uint32_t result_bytes = new_capacity * sizeof(GPUIntersectionPacked);

	// Create with empty data — we'll fill via buffer_update before dispatch.
	ray_buffer_ = rd_->storage_buffer_create(ray_bytes);
	result_buffer_ = rd_->storage_buffer_create(result_bytes);

	ray_buffer_capacity_ = new_capacity;
	uniform_set_dirty_ = true;
}

void GPURayCaster::rebuild_uniform_set() {
	if (!uniform_set_dirty_) return;

	if (uniform_set_.is_valid()) {
		rd_->free_rid(uniform_set_);
		uniform_set_ = RID();
	}

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
	uniform_set_dirty_ = false;
}
