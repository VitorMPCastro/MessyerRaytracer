#pragma once
// gpu_path_tracer.h — GPU wavefront path tracer implementing IPathTracer.
//
// WHAT:  Runs the full multi-bounce path tracing pipeline on the GPU using
//        Vulkan compute shaders.  4 kernels per bounce (wavefront pattern):
//          1. Generate — primary camera rays + path state initialization
//          2. Extend  — CWBVH BVH traversal (nearest-hit)
//          3. Shade   — PBR BRDF evaluation + NEE + bounce sampling
//          4. Connect — Shadow ray traversal (any-hit, early exit)
//        All intermediate data stays GPU-resident between kernels — no
//        CPU↔GPU round-trips per bounce.
//
// WHY:   The CPU path tracer (CPUPathTracer) transfers rays to/from the GPU
//        on every bounce.  This is bandwidth-bound at high resolutions.
//        The GPU wavefront path tracer keeps all data on-device and only
//        reads back the final accumulated color buffer.
//
// HOW:   Shares the local RenderingDevice and BVH buffer RIDs from
//        GPURayCaster (via IRayService::get_gpu_device/get_gpu_scene_buffer_rids).
//        Owns its own buffers for rays, intersections, path state, shadow rays,
//        materials, lights, environment, and accumulation.
//
//        Generate and Shade are new compute shaders.
//        Extend and Connect reuse the existing cwbvh_traverse.comp.glsl
//        shader (with RAY_MODE specialization constant: 0=nearest, 1=any-hit).
//
// ARCHITECTURE (from Jacco Bikker / TinyBVH wavefront.cl):
//   Generate → for each bounce: Extend → Shade → Connect → end
//   Final: readback accumulation → tone map (on GPU) → CPU float buffer.
//
// THREAD-SAFETY:
//   NOT thread-safe.  Only one thread may call trace_frame() at a time
//   (the Godot main thread via RayRenderer::render_frame).
//
// OWNERSHIP:
//   Owned by RayRenderer as std::unique_ptr<IPathTracer>.
//   The shared RenderingDevice is NOT owned — it belongs to GPURayCaster.
//   All buffer RIDs created by this class are freed in cleanup().
//
// REFERENCES:
//   Laine, Karras, Aila — "Megakernels Considered Harmful" (HPG 2013)
//   Ylitie, Karras, Laine — "Efficient CWBVH Traversal" (HPG 2017)
//   Bikker — TinyBVH wavefront.cl reference implementation

#include "api/path_tracer.h"
#include "api/gpu_types.h"
#include "api/gpu_context.h"
#include "api/light_data.h"
#include "core/asserts.h"

#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <vector>
#include <cstdint>

namespace godot {
class RenderingDevice;
}

/// GPU wavefront path tracer.
///
/// Thread-safety: NOT thread-safe.  trace_frame() must be called from
/// the Godot main thread only.
///
/// Shares the local RenderingDevice from GPURayCaster (via IRayService).
/// All wavefront-specific GPU buffers are owned by this class.
class GPUPathTracer final : public IPathTracer {
public:
	GPUPathTracer() = default;
	~GPUPathTracer() override;

	// Non-copyable, non-movable (owns GPU resources via RIDs).
	GPUPathTracer(const GPUPathTracer &) = delete;
	GPUPathTracer &operator=(const GPUPathTracer &) = delete;
	GPUPathTracer(GPUPathTracer &&) = delete;
	GPUPathTracer &operator=(GPUPathTracer &&) = delete;

	/// Initialize the GPU path tracer.  Must be called before trace_frame().
	/// Returns true if all shaders compiled and pipelines are ready.
	/// Safe to call multiple times (returns immediately if already initialized).
	bool initialize(IRayService *svc);

	/// Is the GPU path tracer ready to trace frames?
	bool is_available() const { return initialized_ && rd_ != nullptr; }

	/// IPathTracer implementation — trace a complete frame.
	void trace_frame(const PathTraceParams &params,
		Ray *primary_rays,
		float *color_output,
		IRayService *svc,
		IThreadDispatch *pool) override;

	/// Free all GPU resources.  Safe to call multiple times.
	void cleanup();

private:
	// ---- Shared GPU context (NOT owned — belongs to GPURayCaster) ----
	godot::RenderingDevice *rd_ = nullptr;  // Shared local RD. Lifetime: GPURayCaster.

	// ---- Scene buffer RIDs (NOT owned — from GPURayCaster) ----
	GPUSceneBufferRIDs scene_rids_;  // CWBVH nodes/tris + scene tris.

	// ---- Generate shader + pipeline ----
	godot::RID generate_shader_;     // Compiled pt_generate.comp.glsl.
	godot::RID generate_pipeline_;   // Compute pipeline for Generate kernel.

	// ---- Shade shader + pipeline ----
	godot::RID shade_shader_;        // Compiled pt_shade.comp.glsl.
	godot::RID shade_pipeline_;      // Compute pipeline for Shade kernel.

	// ---- CWBVH traversal (reused from GPURayCaster's shader) ----
	// We compile our own copy because we need different descriptor set layouts
	// (the ray/result buffers are different RIDs from GPURayCaster's).
	godot::RID extend_shader_;       // Compiled cwbvh_traverse.comp.glsl (nearest-hit).
	godot::RID extend_pipeline_;     // RAY_MODE=0 (nearest hit).
	godot::RID connect_shader_;      // Same shader source, different specialization.
	godot::RID connect_pipeline_;    // RAY_MODE=1 (any-hit, early exit).

	// ---- Wavefront GPU buffers (OWNED — created and freed by this class) ----
	godot::RID ray_buffer_;          // GPURayPacked × pixel_count.
	godot::RID intersection_buffer_; // GPUIntersectionPacked × pixel_count.
	godot::RID path_state_buffer_;   // GPUPathStatePacked × pixel_count.
	godot::RID shadow_ray_buffer_;   // GPURayPacked × pixel_count (stochastic single-light NEE).
	godot::RID shadow_result_buffer_;// uint32_t × pixel_count (0=visible, 1=occluded).
	godot::RID accum_buffer_;        // float[4] × pixel_count (RGBA accumulation).

	// ---- Scene data buffers (OWNED — uploaded per build()) ----
	godot::RID material_buffer_;     // GPUMaterialPacked × material_count.
	godot::RID material_id_buffer_;  // uint32_t × triangle_count (per-tri mat index).
	godot::RID light_buffer_;        // GPULightPacked × MAX_SCENE_LIGHTS.
	godot::RID env_buffer_;          // GPUEnvironmentPacked × 1.
	godot::RID camera_buffer_;       // GPUCameraPacked × 1.

	// ---- Texture arrays (OWNED — albedo and normal map atlases) ----
	godot::RID albedo_tex_array_;    // Texture2DArray for albedo textures.
	godot::RID normal_tex_array_;    // Texture2DArray for normal map textures.
	godot::RID tex_sampler_;         // Linear + repeat sampler shared by both arrays.

	// ---- UV and tangent data buffers (OWNED) ----
	godot::RID triangle_uv_buffer_;      // Per-triangle UV data for texture sampling.
	godot::RID triangle_normal_buffer_;  // Per-triangle vertex normals for smooth shading.
	godot::RID triangle_tangent_buffer_; // Per-triangle tangents for normal mapping.

	// ---- Descriptor sets (rebuilt when buffers change) ----
	godot::RID generate_uniform_set_;
	godot::RID extend_uniform_set_;
	godot::RID shade_uniform_set_;
	godot::RID connect_uniform_set_;

	// ---- State ----
	bool initialized_ = false;       // True after successful initialize().
	uint32_t buffer_pixel_capacity_ = 0;  // Current pixel capacity of wavefront buffers.
	uint32_t uploaded_material_count_ = 0; // Materials currently on GPU.
	uint32_t uploaded_triangle_count_ = 0; // Triangles currently on GPU.

	// ---- Cached per-frame params (avoid re-packing push constants each dispatch) ----
	uint32_t cached_width_  = 0;           // Framebuffer width for current frame.
	uint32_t cached_height_ = 0;           // Framebuffer height for current frame.
	uint32_t cached_max_bounces_ = 4;      // Max bounce depth for current frame.
	uint32_t cached_sample_index_ = 0;     // Temporal sample index for current frame.
	uint32_t cached_light_count_ = 0;      // Active light count for current frame.
	bool     cached_shadows_enabled_ = true; // Shadow flag for current frame.

	// ---- Persistent cache (avoid per-frame heap allocation) ----
	godot::PackedByteArray push_data_cache_;
	godot::PackedByteArray upload_cache_;

	// ---- Internal helpers ----

	/// Compile a compute shader from embedded GLSL source.
	/// Returns an invalid RID on failure.
	godot::RID _compile_shader(const char *source, const char *name);

	/// Ensure wavefront buffers are large enough for the given pixel count.
	/// Recreates buffers if capacity is insufficient (grow-only).
	void _ensure_wavefront_buffers(uint32_t pixel_count);

	/// Upload material, UV, normal, and tangent data to GPU.
	/// Called when shade data changes (different triangle/material counts).
	void _upload_shade_data(const SceneShadeData &shade, const SceneLightData &lights);

	/// Upload per-frame dynamic data (camera, environment, lights).
	void _upload_frame_data(const PathTraceParams &params);

	/// Rebuild descriptor sets after buffer RID changes.
	void _rebuild_descriptor_sets();

	/// Dispatch the Generate kernel (primary ray creation).
	void _dispatch_generate(uint32_t pixel_count);

	/// Dispatch the Extend kernel (CWBVH nearest-hit traversal).
	void _dispatch_extend(uint32_t pixel_count);

	/// Dispatch the Shade kernel (PBR + NEE + bounce sampling).
	void _dispatch_shade(uint32_t pixel_count, uint32_t bounce);

	/// Dispatch the Connect kernel (shadow ray any-hit traversal).
	void _dispatch_connect(uint32_t pixel_count);

	/// Read back the accumulated color buffer from GPU to CPU.
	void _readback_accumulation(float *color_output, uint32_t pixel_count);

	/// Free wavefront-specific buffers (ray, intersection, path state, etc.).
	void _free_wavefront_buffers();

	/// Free scene data buffers (materials, UVs, lights, etc.).
	void _free_scene_buffers();

	/// Free shaders and pipelines.
	void _free_shaders();
};
