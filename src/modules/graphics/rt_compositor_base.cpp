// rt_compositor_base.cpp — Base class for ray-traced CompositorEffects.
//
// Handles shared RenderingDevice init, sampler creation, BVH upload,
// and provides helpers for subclass shader dispatch.

#include "rt_compositor_base.h"

#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/core/class_db.hpp>

#include "api/ray_service.h"
#include "core/asserts.h"

#include <cstring>

using namespace godot;

// ============================================================================
// Constructor / Destructor
// ============================================================================

RTCompositorBase::RTCompositorBase() {
	// Default configuration for RT compositor effects.
	set_effect_callback_type(EFFECT_CALLBACK_TYPE_POST_TRANSPARENT);
	set_access_resolved_color(true);
	set_access_resolved_depth(true);
	set_needs_normal_roughness(true);
	set_needs_motion_vectors(false);
	set_needs_separate_specular(false);
}

RTCompositorBase::~RTCompositorBase() {
	_cleanup();
}

// ============================================================================
// _render_callback — CompositorEffect virtual entry point
// ============================================================================

void RTCompositorBase::_render_callback(int32_t p_effect_callback_type,
										RenderData *p_render_data) {
	if (!get_enabled() || !p_render_data) { return; }

	// ---- Lazy initialization on first render ----
	if (!render_initialized_) {
		_initialize_render();
		if (!render_initialized_) { return; } // Init failed
	}

	// ---- Get typed scene buffers ----
	Ref<RenderSceneBuffers> buffers_base = p_render_data->get_render_scene_buffers();
	if (buffers_base.is_null()) { return; }

	Ref<RenderSceneBuffersRD> scene_buffers = Object::cast_to<RenderSceneBuffersRD>(buffers_base.ptr());
	if (scene_buffers.is_null()) { return; }

	// ---- Get scene data (camera, projection) ----
	RenderSceneData *scene_data = p_render_data->get_render_scene_data();
	if (!scene_data) { return; }

	// ---- Get render size ----
	Vector2i render_size = scene_buffers->get_internal_size();
	if (render_size.x <= 0 || render_size.y <= 0) { return; }

	// ---- Upload BVH if needed ----
	upload_scene_to_shared_device();

	// ---- Delegate to subclass ----
	_on_render(p_render_data, scene_buffers, scene_data, render_size);
}

// ============================================================================
// _initialize_render — one-time setup on the render thread
// ============================================================================

void RTCompositorBase::_initialize_render() {
	RT_ASSERT(!render_initialized_, "_initialize_render called but already initialized");

	// Get the shared rendering device (Godot's render thread).
	RenderingServer *rs = RenderingServer::get_singleton();
	if (!rs) {
		UtilityFunctions::printerr("[RTCompositorBase] RenderingServer not available");
		return;
	}

	rd_ = rs->get_rendering_device();
	if (!rd_) {
		UtilityFunctions::printerr("[RTCompositorBase] Shared RenderingDevice not available");
		return;
	}

	// Create samplers.
	_create_samplers();

	// Let subclass compile shaders and create pipelines.
	_on_initialize_render();

	render_initialized_ = true;
	RT_ASSERT_NOT_NULL(rd_);
	UtilityFunctions::print("[RTCompositorBase] Render initialized on shared device");
}

// ============================================================================
// _create_samplers
// ============================================================================

void RTCompositorBase::_create_samplers() {
	RT_ASSERT(rd_ != nullptr, "RD must be set before creating samplers");
	RT_ASSERT(!nearest_sampler_.is_valid(), "Samplers already created");

	// Nearest-neighbor sampler.
	{
		Ref<RDSamplerState> state;
		state.instantiate();
		state->set_mag_filter(RenderingDevice::SAMPLER_FILTER_NEAREST);
		state->set_min_filter(RenderingDevice::SAMPLER_FILTER_NEAREST);
		nearest_sampler_ = rd_->sampler_create(state);
	}

	// Linear (bilinear) sampler.
	{
		Ref<RDSamplerState> state;
		state.instantiate();
		state->set_mag_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
		state->set_min_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
		linear_sampler_ = rd_->sampler_create(state);
	}
}

// ============================================================================
// compile_shader — compile GLSL compute source to shader RID
// ============================================================================

RID RTCompositorBase::compile_shader(const char *glsl_source, const String &debug_name) {
	RT_ASSERT(rd_ != nullptr, "RD must be initialized before compiling shaders");
	RT_ASSERT(glsl_source != nullptr, "GLSL source must not be null");

	Ref<RDShaderSource> source;
	source.instantiate();
	source->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, String(glsl_source));
	source->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);

	Ref<RDShaderSPIRV> spirv = rd_->shader_compile_spirv_from_source(source);
	if (spirv.is_null()) {
		UtilityFunctions::printerr("[RTCompositorBase] Shader compile returned null: ", debug_name);
		return RID();
	}

	String error = spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE);
	if (!error.is_empty()) {
		UtilityFunctions::printerr("[RTCompositorBase] Shader error (", debug_name, "): ", error);
		return RID();
	}

	RID shader = rd_->shader_create_from_spirv(spirv, debug_name);
	if (!shader.is_valid()) {
		UtilityFunctions::printerr("[RTCompositorBase] Failed to create shader: ", debug_name);
		return RID();
	}

	UtilityFunctions::print("[RTCompositorBase] Compiled shader: ", debug_name);
	return shader;
}

// ============================================================================
// create_pipeline — wrap compute_pipeline_create
// ============================================================================

RID RTCompositorBase::create_pipeline(const RID &shader) {
	RT_ASSERT(rd_ != nullptr, "RD must be initialized");
	RT_ASSERT(shader.is_valid(), "Shader must be valid to create pipeline");

	RID pipeline = rd_->compute_pipeline_create(shader);
	if (!pipeline.is_valid()) {
		UtilityFunctions::printerr("[RTCompositorBase] Failed to create compute pipeline");
	}
	return pipeline;
}

// ============================================================================
// upload_scene_to_shared_device — upload BVH data via IRayService
// ============================================================================

void RTCompositorBase::upload_scene_to_shared_device() {
	RT_ASSERT(render_initialized_, "Cannot upload scene before render initialization");
	if (!rd_) { return; }

	IRayService *svc = get_ray_service();
	if (!svc) { return; }

	GPUSceneUpload scene_data = svc->get_gpu_scene_data();
	if (!scene_data.valid) { return; }

	// Only re-upload if scene has changed.
	if (scene_uploaded_ && scene_data.triangle_count == uploaded_tri_count_ && scene_data.bvh_node_count == uploaded_node_count_) {
		return;
	}

	// Free old buffers.
	free_scene_buffers();

	// ---- Upload triangle buffer ----
	{
		uint32_t byte_size = scene_data.triangle_count * sizeof(GPUTrianglePacked);
		PackedByteArray data;
		data.resize(byte_size);
		memcpy(data.ptrw(), scene_data.triangles, byte_size);
		shared_triangle_buffer_ = rd_->storage_buffer_create(byte_size, data);
	}

	// ---- Upload BVH node buffer ----
	{
		uint32_t byte_size = scene_data.bvh_node_count * sizeof(GPUBVHNodePacked);
		PackedByteArray data;
		data.resize(byte_size);
		memcpy(data.ptrw(), scene_data.bvh_nodes, byte_size);
		shared_bvh_node_buffer_ = rd_->storage_buffer_create(byte_size, data);
	}

	uploaded_tri_count_ = scene_data.triangle_count;
	uploaded_node_count_ = scene_data.bvh_node_count;
	scene_uploaded_ = true;
	RT_ASSERT(shared_triangle_buffer_.is_valid(), "Triangle buffer must be valid after upload");
	RT_ASSERT(shared_bvh_node_buffer_.is_valid(), "BVH node buffer must be valid after upload");

	UtilityFunctions::print("[RTCompositorBase] Uploaded to shared device: ",
		static_cast<int>(scene_data.triangle_count), " tris, ",
		static_cast<int>(scene_data.bvh_node_count), " BVH nodes");
}

// ============================================================================
// free_scene_buffers
// ============================================================================

void RTCompositorBase::free_scene_buffers() {
	// rd_ can legitimately be null if the effect was never rendered (destructor
	// path: _cleanup() → free_scene_buffers() runs before rd_ is ever assigned).
	// This is a normal lifecycle — not an error.
	if (!rd_) { return; }
	RT_ASSERT_NOT_NULL(rd_);
	if (shared_triangle_buffer_.is_valid()) {
		rd_->free_rid(shared_triangle_buffer_);
		shared_triangle_buffer_ = RID();
	}
	if (shared_bvh_node_buffer_.is_valid()) {
		rd_->free_rid(shared_bvh_node_buffer_);
		shared_bvh_node_buffer_ = RID();
	}
	scene_uploaded_ = false;
	uploaded_tri_count_ = 0;
	uploaded_node_count_ = 0;

	RT_ASSERT(!shared_triangle_buffer_.is_valid(), "Triangle buffer must be freed");
	RT_ASSERT(!shared_bvh_node_buffer_.is_valid(), "BVH node buffer must be freed");
}

// ============================================================================
// Uniform helpers — static convenience methods
// ============================================================================

Ref<RDUniform> RTCompositorBase::make_storage_uniform(int binding, const RID &buffer) {
	RT_ASSERT(binding >= 0, "make_storage_uniform: binding must be non-negative");
	RT_ASSERT(buffer.is_valid(), "make_storage_uniform: buffer RID must be valid");
	Ref<RDUniform> u;
	u.instantiate();
	u->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	u->set_binding(binding);
	u->add_id(buffer);
	return u;
}

Ref<RDUniform> RTCompositorBase::make_sampler_uniform(int binding, const RID &sampler,
													  const RID &texture) {
	Ref<RDUniform> u;
	u.instantiate();
	u->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	u->set_binding(binding);
	u->add_id(sampler);
	u->add_id(texture);
	return u;
}

Ref<RDUniform> RTCompositorBase::make_image_uniform(int binding, const RID &image) {
	RT_ASSERT(binding >= 0, "make_image_uniform: binding must be non-negative");
	RT_ASSERT(image.is_valid(), "make_image_uniform: image RID must be valid");
	Ref<RDUniform> u;
	u.instantiate();
	u->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
	u->set_binding(binding);
	u->add_id(image);
	return u;
}

// ============================================================================
// Debug label helpers
// ============================================================================

void RTCompositorBase::begin_compute_label(const String &label) {
	if (rd_) {
		rd_->draw_command_begin_label(label, Color(0.3f, 0.8f, 0.3f));
	}
}

void RTCompositorBase::end_compute_label() {
	if (rd_) {
		rd_->draw_command_end_label();
	}
}

// ============================================================================
// _cleanup — free all shared resources
// ============================================================================

void RTCompositorBase::_cleanup() {
	RT_ASSERT(!render_initialized_ || rd_ != nullptr, "_cleanup: initialized but no RenderingDevice");
	free_scene_buffers();

	if (rd_) {
		if (nearest_sampler_.is_valid()) { rd_->free_rid(nearest_sampler_); nearest_sampler_ = RID(); }
		if (linear_sampler_.is_valid())  { rd_->free_rid(linear_sampler_);  linear_sampler_ = RID(); }
	}

	// NOTE: We do NOT memdelete rd_ — it's Godot's shared device, not ours.
	rd_ = nullptr;
	render_initialized_ = false;
	RT_ASSERT(!render_initialized_, "_cleanup: render_initialized_ must be false after cleanup");
}

// ============================================================================
// _bind_methods
// ============================================================================

void RTCompositorBase::_bind_methods() {
	// RTCompositorBase is an abstract base — no GDScript-exposed methods.
	// Subclasses register their own properties.
}
