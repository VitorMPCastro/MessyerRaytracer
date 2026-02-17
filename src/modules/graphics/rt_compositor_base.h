#pragma once
// rt_compositor_base.h — Base class for ray-traced CompositorEffects.
//
// Encapsulates the boilerplate for all RT compositor effects:
//   - Shared RenderingDevice access (Godot's render thread)
//   - Sampler creation (nearest + linear)
//   - Shader compilation from embedded GLSL source
//   - BVH data upload to shared device (separate from GPURayCaster's local device)
//   - Intermediate texture management via RenderSceneBuffersRD
//   - Compute dispatch helpers with RenderDoc debug labels
//   - Uniform set caching via UniformSetCacheRD
//
// ARCHITECTURE:
//   CompositorEffect runs on the SHARED RenderingDevice (Godot's render thread).
//   GPURayCaster uses a LOCAL RenderingDevice (separate GPU queue).
//   Therefore BVH data must be uploaded separately to the shared device.
//   CPU-side triangle/BVH data comes from RayTracerServer::scene().
//
// SUBCLASS CONTRACT:
//   Override _on_initialize_render() — compile shaders, create pipelines
//   Override _on_render(render_data, size) — perform GPU work per frame
//
// REFERENCE:
//   Based on patterns from JFA_driven_motion_blur_addon (338★) and
//   compositor-effect-lens-effects (21★). See RESEARCH_FINDINGS.md.

#include <godot_cpp/classes/compositor_effect.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/render_data.hpp>
#include <godot_cpp/classes/render_data_rd.hpp>
#include <godot_cpp/classes/render_scene_buffers.hpp>
#include <godot_cpp/classes/render_scene_buffers_rd.hpp>
#include <godot_cpp/classes/render_scene_data.hpp>
#include <godot_cpp/classes/render_scene_data_rd.hpp>
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/uniform_set_cache_rd.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <vector>
#include <cstdint>

using namespace godot;

class RTCompositorBase : public CompositorEffect {
	GDCLASS(RTCompositorBase, CompositorEffect)

public:
	RTCompositorBase();
	virtual ~RTCompositorBase();

	// CompositorEffect virtual — called on Godot's render thread.
	virtual void _render_callback(int32_t p_effect_callback_type,
								  RenderData *p_render_data) override;

protected:
	static void _bind_methods();

	// ---- Subclass interface ----

	/// Called once on the render thread to compile shaders and create pipelines.
	/// rd_ is valid when this is called.
	virtual void _on_initialize_render() {}

	/// Called every frame. Subclass performs its compute dispatch here.
	/// @param render_data The render data from Godot's compositor
	/// @param scene_buffers The typed RD scene buffers (color, depth, normal_roughness)
	/// @param scene_data Camera / projection data
	/// @param render_size Internal render resolution
	virtual void _on_render(RenderData *render_data,
							Ref<RenderSceneBuffersRD> scene_buffers,
							RenderSceneData *scene_data,
							const Vector2i &render_size) {}

	// ---- Shader helpers ----

	/// Compile a compute shader from embedded GLSL source string.
	/// Returns the shader RID, or invalid RID on failure.
	RID compile_shader(const char *glsl_source, const String &debug_name);

	/// Create a compute pipeline from a shader RID.
	RID create_pipeline(const RID &shader);

	// ---- BVH data management ----

	/// Upload scene BVH data to the shared rendering device.
	/// Called from _render_callback when the scene has been rebuilt.
	/// Sources CPU-side data from RayTracerServer::scene().
	void upload_scene_to_shared_device();

	/// Free BVH buffers on the shared device.
	void free_scene_buffers();

	/// Check if BVH data is available on the shared device.
	bool has_scene_data() const { return scene_uploaded_; }

	// ---- Uniform helpers ----

	/// Create an RDUniform for a storage buffer.
	static Ref<RDUniform> make_storage_uniform(int binding, const RID &buffer);

	/// Create an RDUniform for a sampler + texture.
	static Ref<RDUniform> make_sampler_uniform(int binding, const RID &sampler, const RID &texture);

	/// Create an RDUniform for an image (read/write texture).
	static Ref<RDUniform> make_image_uniform(int binding, const RID &image);

	// ---- Dispatch helper ----

	/// Begin a labeled compute dispatch (for RenderDoc profiling).
	void begin_compute_label(const String &label);

	/// End a labeled section.
	void end_compute_label();

	// ---- Accessors ----

	RenderingDevice *rd() const { return rd_; }
	RID nearest_sampler() const { return nearest_sampler_; }
	RID linear_sampler() const { return linear_sampler_; }
	RID triangle_buffer() const { return shared_triangle_buffer_; }
	RID bvh_node_buffer() const { return shared_bvh_node_buffer_; }

private:
	// ---- Initialization state ----
	bool render_initialized_ = false;

	// ---- Shared RenderingDevice (Godot's render thread) ----
	RenderingDevice *rd_ = nullptr;

	// ---- Samplers ----
	RID nearest_sampler_;
	RID linear_sampler_;

	// ---- BVH data on shared device ----
	RID shared_triangle_buffer_;
	RID shared_bvh_node_buffer_;
	bool scene_uploaded_ = false;
	uint32_t uploaded_tri_count_ = 0;
	uint32_t uploaded_node_count_ = 0;

	// ---- Internal ----
	void _initialize_render();
	void _create_samplers();
	void _cleanup();

	// Track previous render size for resize detection.
	Vector2i last_render_size_;
};
