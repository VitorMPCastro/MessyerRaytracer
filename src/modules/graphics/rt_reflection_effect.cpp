// rt_reflection_effect.cpp — RT-enhanced reflections via CompositorEffect.
//
// 4-pass compute pipeline:
//   1. Ray generation + BVH trace → raw reflection buffer
//   2. Cross-bilateral spatial denoise → denoised buffer
//   3. Temporal accumulation with motion rejection → stable output
//   4. Fresnel-weighted composite into Godot's color buffer

#include "rt_reflection_effect.h"

// Embedded shader sources (generated from .glsl by SConstruct).
#include "gpu/shaders/rt_reflections.gen.h"
#include "gpu/shaders/rt_denoise_spatial.gen.h"
#include "gpu/shaders/rt_denoise_temporal.gen.h"
#include "gpu/shaders/rt_composite.gen.h"

#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/uniform_set_cache_rd.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include "core/asserts.h"

#include <cstring>

using namespace godot;

// ============================================================================
// Lazy-initialized string names (function-local statics avoid construction
// during DLL load, before Godot's memory/string systems are ready).
// ============================================================================

const StringName &RTReflectionEffect::ctx_rt_reflections() {
	static const StringName s("rt_reflections");
	return s;
}
const StringName &RTReflectionEffect::tex_reflection_raw() {
	static const StringName s("raw");
	return s;
}
const StringName &RTReflectionEffect::tex_reflection_denoised() {
	static const StringName s("denoised");
	return s;
}
const StringName &RTReflectionEffect::tex_reflection_history() {
	static const StringName s("history");
	return s;
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

RTReflectionEffect::RTReflectionEffect() {
	// Ensure we request access to depth + normal_roughness buffers.
	set_access_resolved_color(true);
	set_access_resolved_depth(true);
	set_needs_normal_roughness(true);
}

RTReflectionEffect::~RTReflectionEffect() {
	_cleanup_shaders();
}

// ============================================================================
// _on_initialize_render — compile all 4 shaders + pipelines
// ============================================================================

void RTReflectionEffect::_on_initialize_render() {
	RT_ASSERT(rd() != nullptr, "RD must be valid for shader compilation");

	// Pass 1: Ray trace
	trace_shader_ = compile_shader(RT_REFLECTIONS_GLSL, "rt_reflections");
	if (trace_shader_.is_valid()) {
		trace_pipeline_ = create_pipeline(trace_shader_);
	}

	// Pass 2: Spatial denoise
	spatial_denoise_shader_ = compile_shader(RT_DENOISE_SPATIAL_GLSL, "rt_denoise_spatial");
	if (spatial_denoise_shader_.is_valid()) {
		spatial_denoise_pipeline_ = create_pipeline(spatial_denoise_shader_);
	}

	// Pass 3: Temporal denoise
	temporal_denoise_shader_ = compile_shader(RT_DENOISE_TEMPORAL_GLSL, "rt_denoise_temporal");
	if (temporal_denoise_shader_.is_valid()) {
		temporal_denoise_pipeline_ = create_pipeline(temporal_denoise_shader_);
	}

	// Pass 4: Composite
	composite_shader_ = compile_shader(RT_COMPOSITE_GLSL, "rt_composite");
	if (composite_shader_.is_valid()) {
		composite_pipeline_ = create_pipeline(composite_shader_);
	}

	bool all_valid = trace_pipeline_.is_valid() &&
					 spatial_denoise_pipeline_.is_valid() &&
					 temporal_denoise_pipeline_.is_valid() &&
					 composite_pipeline_.is_valid();

	if (all_valid) {
		UtilityFunctions::print("[RTReflectionEffect] All 4 pipelines compiled successfully");
	} else {
		UtilityFunctions::printerr("[RTReflectionEffect] Some pipelines failed to compile — effect disabled");
		set_enabled(false);
	}
}

// ============================================================================
// _on_render — dispatch the 4-pass pipeline
// ============================================================================

void RTReflectionEffect::_on_render(RenderData *render_data,
									Ref<RenderSceneBuffersRD> scene_buffers,
									RenderSceneData *scene_data,
									const Vector2i &render_size) {
	// Skip if BVH not uploaded or pipelines invalid.
	if (!has_scene_data()) return;
	if (!trace_pipeline_.is_valid()) return;

	// Handle render size changes.
	if (render_size != current_render_size_) {
		scene_buffers->clear_context(ctx_rt_reflections());
		current_render_size_ = render_size;
		frame_count_ = 0; // Reset temporal accumulation on resize.
	}

	// Ensure intermediate textures exist.
	_ensure_textures(scene_buffers, render_size);

	// Execute the 4-pass pipeline.
	_pass_trace(scene_buffers, scene_data, render_size);
	_pass_spatial_denoise(scene_buffers, render_size);
	_pass_temporal_denoise(scene_buffers, render_size);
	_pass_composite(scene_buffers, scene_data, render_size);

	frame_count_++;
}

// ============================================================================
// _ensure_textures — create intermediate textures via RenderSceneBuffersRD
// ============================================================================

void RTReflectionEffect::_ensure_textures(Ref<RenderSceneBuffersRD> scene_buffers,
										  const Vector2i &size) {
	// Use RGBA16F for all intermediate textures (HDR-capable).
	// Usage bits: SAMPLING (read as sampler) + STORAGE (write as image) + CAN_COPY (for history swap).
	const RenderingDevice::DataFormat format = RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT;
	const uint32_t usage = RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
						   RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
						   RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT |
						   RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT;

	if (!scene_buffers->has_texture(ctx_rt_reflections(), tex_reflection_raw())) {
		scene_buffers->create_texture(ctx_rt_reflections(), tex_reflection_raw(),
			format, usage, RenderingDevice::TEXTURE_SAMPLES_1,
			size, 1, 1, true, false);
	}

	if (!scene_buffers->has_texture(ctx_rt_reflections(), tex_reflection_denoised())) {
		scene_buffers->create_texture(ctx_rt_reflections(), tex_reflection_denoised(),
			format, usage, RenderingDevice::TEXTURE_SAMPLES_1,
			size, 1, 1, true, false);
	}

	if (!scene_buffers->has_texture(ctx_rt_reflections(), tex_reflection_history())) {
		scene_buffers->create_texture(ctx_rt_reflections(), tex_reflection_history(),
			format, usage, RenderingDevice::TEXTURE_SAMPLES_1,
			size, 1, 1, true, false);
	}
}

// ============================================================================
// Pass 1: Ray generation + BVH trace
// ============================================================================

void RTReflectionEffect::_pass_trace(Ref<RenderSceneBuffersRD> scene_buffers,
									 RenderSceneData *scene_data,
									 const Vector2i &size) {
	begin_compute_label("RT Reflections: Trace");

	RenderingDevice *device = rd();

	// ---- Get G-buffer textures ----
	RID depth_tex = scene_buffers->get_depth_layer(0);
	RID normal_roughness_tex = scene_buffers->get_texture(
		StringName("forward_clustered"), StringName("normal_roughness"));
	RID raw_output = scene_buffers->get_texture_slice(
		ctx_rt_reflections(), tex_reflection_raw(), 0, 0, 1, 1);

	// ---- Build uniform sets ----
	// Set 0: Scene textures (depth + normal_roughness)
	TypedArray<RDUniform> set0_uniforms;
	set0_uniforms.push_back(make_sampler_uniform(0, nearest_sampler(), depth_tex));
	set0_uniforms.push_back(make_sampler_uniform(1, nearest_sampler(), normal_roughness_tex));
	RID set0 = UniformSetCacheRD::get_cache(trace_shader_, 0, set0_uniforms);

	// Set 1: BVH data
	TypedArray<RDUniform> set1_uniforms;
	set1_uniforms.push_back(make_storage_uniform(0, triangle_buffer()));
	set1_uniforms.push_back(make_storage_uniform(1, bvh_node_buffer()));
	RID set1 = UniformSetCacheRD::get_cache(trace_shader_, 1, set1_uniforms);

	// Set 2: Output texture
	TypedArray<RDUniform> set2_uniforms;
	set2_uniforms.push_back(make_image_uniform(0, raw_output));
	RID set2 = UniformSetCacheRD::get_cache(trace_shader_, 2, set2_uniforms);

	// ---- Push constants: camera matrices + parameters ----
	TracePushConstants push{};
	Projection inv_proj = scene_data->get_cam_projection().inverse();
	Transform3D inv_view = scene_data->get_cam_transform();
	projection_to_floats(inv_proj, push.inv_projection);
	transform_to_floats(inv_view, push.inv_view);
	push.roughness_threshold = roughness_threshold_;
	push.ray_max_distance = ray_max_distance_;
	push.frame_count = frame_count_;
	push._pad = 0;

	PackedByteArray push_data;
	push_data.resize(sizeof(TracePushConstants));
	memcpy(push_data.ptrw(), &push, sizeof(TracePushConstants));

	// ---- Dispatch ----
	uint32_t groups_x = (size.x + 15) / 16;
	uint32_t groups_y = (size.y + 15) / 16;

	int64_t compute_list = device->compute_list_begin();
	device->compute_list_bind_compute_pipeline(compute_list, trace_pipeline_);
	device->compute_list_bind_uniform_set(compute_list, set0, 0);
	device->compute_list_bind_uniform_set(compute_list, set1, 1);
	device->compute_list_bind_uniform_set(compute_list, set2, 2);
	device->compute_list_set_push_constant(compute_list, push_data, sizeof(TracePushConstants));
	device->compute_list_dispatch(compute_list, groups_x, groups_y, 1);
	device->compute_list_end();

	end_compute_label();
}

// ============================================================================
// Pass 2: Spatial denoise
// ============================================================================

void RTReflectionEffect::_pass_spatial_denoise(Ref<RenderSceneBuffersRD> scene_buffers,
											   const Vector2i &size) {
	begin_compute_label("RT Reflections: Spatial Denoise");

	RenderingDevice *device = rd();

	RID depth_tex = scene_buffers->get_depth_layer(0);
	RID normal_roughness_tex = scene_buffers->get_texture(
		StringName("forward_clustered"), StringName("normal_roughness"));
	RID raw_tex = scene_buffers->get_texture_slice(
		ctx_rt_reflections(), tex_reflection_raw(), 0, 0, 1, 1);
	RID denoised_output = scene_buffers->get_texture_slice(
		ctx_rt_reflections(), tex_reflection_denoised(), 0, 0, 1, 1);

	// Set 0: Inputs
	TypedArray<RDUniform> set0_uniforms;
	set0_uniforms.push_back(make_sampler_uniform(0, nearest_sampler(), depth_tex));
	set0_uniforms.push_back(make_sampler_uniform(1, nearest_sampler(), normal_roughness_tex));
	set0_uniforms.push_back(make_sampler_uniform(2, linear_sampler(), raw_tex));
	RID set0 = UniformSetCacheRD::get_cache(spatial_denoise_shader_, 0, set0_uniforms);

	// Set 1: Output
	TypedArray<RDUniform> set1_uniforms;
	set1_uniforms.push_back(make_image_uniform(0, denoised_output));
	RID set1 = UniformSetCacheRD::get_cache(spatial_denoise_shader_, 1, set1_uniforms);

	// Push constants
	SpatialDenoisePushConstants push{};
	push.depth_sigma = denoise_depth_sigma_;
	push.normal_sigma = denoise_normal_sigma_;
	push.color_sigma = denoise_color_sigma_;
	push.kernel_radius = denoise_kernel_radius_;

	PackedByteArray push_data;
	push_data.resize(sizeof(SpatialDenoisePushConstants));
	memcpy(push_data.ptrw(), &push, sizeof(SpatialDenoisePushConstants));

	uint32_t groups_x = (size.x + 15) / 16;
	uint32_t groups_y = (size.y + 15) / 16;

	int64_t compute_list = device->compute_list_begin();
	device->compute_list_bind_compute_pipeline(compute_list, spatial_denoise_pipeline_);
	device->compute_list_bind_uniform_set(compute_list, set0, 0);
	device->compute_list_bind_uniform_set(compute_list, set1, 1);
	device->compute_list_set_push_constant(compute_list, push_data, sizeof(SpatialDenoisePushConstants));
	device->compute_list_dispatch(compute_list, groups_x, groups_y, 1);
	device->compute_list_end();

	end_compute_label();
}

// ============================================================================
// Pass 3: Temporal accumulation
// ============================================================================

void RTReflectionEffect::_pass_temporal_denoise(Ref<RenderSceneBuffersRD> scene_buffers,
												const Vector2i &size) {
	begin_compute_label("RT Reflections: Temporal Denoise");

	RenderingDevice *device = rd();

	RID depth_tex = scene_buffers->get_depth_layer(0);
	RID denoised_tex = scene_buffers->get_texture_slice(
		ctx_rt_reflections(), tex_reflection_denoised(), 0, 0, 1, 1);
	RID history_tex = scene_buffers->get_texture_slice(
		ctx_rt_reflections(), tex_reflection_history(), 0, 0, 1, 1);

	// We write the temporal result back to the "denoised" texture and then
	// copy denoised → history for the next frame. This avoids needing a 4th texture.
	// For the first implementation, we use the history texture as both input and output
	// by writing to a separate location.

	// Set 0: Current frame inputs
	TypedArray<RDUniform> set0_uniforms;
	set0_uniforms.push_back(make_sampler_uniform(0, linear_sampler(), denoised_tex));
	set0_uniforms.push_back(make_sampler_uniform(1, nearest_sampler(), depth_tex));
	RID set0 = UniformSetCacheRD::get_cache(temporal_denoise_shader_, 0, set0_uniforms);

	// Set 1: History + output (writes back to raw buffer as temp storage)
	RID raw_output = scene_buffers->get_texture_slice(
		ctx_rt_reflections(), tex_reflection_raw(), 0, 0, 1, 1);
	TypedArray<RDUniform> set1_uniforms;
	set1_uniforms.push_back(make_sampler_uniform(0, linear_sampler(), history_tex));
	set1_uniforms.push_back(make_image_uniform(1, raw_output));
	RID set1 = UniformSetCacheRD::get_cache(temporal_denoise_shader_, 1, set1_uniforms);

	// Push constants
	TemporalDenoisePushConstants push{};
	push.blend_factor = temporal_blend_;
	push.depth_threshold = 0.1f;
	push.frame_count = frame_count_;
	push._pad = 0;

	PackedByteArray push_data;
	push_data.resize(sizeof(TemporalDenoisePushConstants));
	memcpy(push_data.ptrw(), &push, sizeof(TemporalDenoisePushConstants));

	uint32_t groups_x = (size.x + 15) / 16;
	uint32_t groups_y = (size.y + 15) / 16;

	int64_t compute_list = device->compute_list_begin();
	device->compute_list_bind_compute_pipeline(compute_list, temporal_denoise_pipeline_);
	device->compute_list_bind_uniform_set(compute_list, set0, 0);
	device->compute_list_bind_uniform_set(compute_list, set1, 1);
	device->compute_list_set_push_constant(compute_list, push_data, sizeof(TemporalDenoisePushConstants));
	device->compute_list_dispatch(compute_list, groups_x, groups_y, 1);
	device->compute_list_end();

	// Copy temporal output (raw) → history for next frame.
	// This uses a texture copy so history stays valid across frames.
	device->texture_copy(raw_output, history_tex,
		Vector3(0, 0, 0), Vector3(0, 0, 0),
		Vector3(size.x, size.y, 1),
		0, 0, 0, 0);

	end_compute_label();
}

// ============================================================================
// Pass 4: Composite into color buffer
// ============================================================================

void RTReflectionEffect::_pass_composite(Ref<RenderSceneBuffersRD> scene_buffers,
										 RenderSceneData *scene_data,
										 const Vector2i &size) {
	begin_compute_label("RT Reflections: Composite");

	RenderingDevice *device = rd();

	RID depth_tex = scene_buffers->get_depth_layer(0);
	RID normal_roughness_tex = scene_buffers->get_texture(
		StringName("forward_clustered"), StringName("normal_roughness"));
	// Use the temporal output (stored in raw after pass 3)
	RID reflection_tex = scene_buffers->get_texture_slice(
		ctx_rt_reflections(), tex_reflection_raw(), 0, 0, 1, 1);
	RID color_tex = scene_buffers->get_color_layer(0);

	// Set 0: Scene textures
	TypedArray<RDUniform> set0_uniforms;
	set0_uniforms.push_back(make_sampler_uniform(0, nearest_sampler(), depth_tex));
	set0_uniforms.push_back(make_sampler_uniform(1, nearest_sampler(), normal_roughness_tex));
	RID set0 = UniformSetCacheRD::get_cache(composite_shader_, 0, set0_uniforms);

	// Set 1: Reflection + color buffer
	TypedArray<RDUniform> set1_uniforms;
	set1_uniforms.push_back(make_sampler_uniform(0, linear_sampler(), reflection_tex));
	set1_uniforms.push_back(make_image_uniform(1, color_tex));
	RID set1 = UniformSetCacheRD::get_cache(composite_shader_, 1, set1_uniforms);

	// Push constants
	CompositePushConstants push{};
	Transform3D inv_view = scene_data->get_cam_transform();
	transform_to_floats(inv_view, push.inv_view);
	push.roughness_threshold = roughness_threshold_;
	push.reflection_intensity = reflection_intensity_;
	push.f0 = fresnel_f0_;
	push._pad = 0;

	PackedByteArray push_data;
	push_data.resize(sizeof(CompositePushConstants));
	memcpy(push_data.ptrw(), &push, sizeof(CompositePushConstants));

	uint32_t groups_x = (size.x + 15) / 16;
	uint32_t groups_y = (size.y + 15) / 16;

	int64_t compute_list = device->compute_list_begin();
	device->compute_list_bind_compute_pipeline(compute_list, composite_pipeline_);
	device->compute_list_bind_uniform_set(compute_list, set0, 0);
	device->compute_list_bind_uniform_set(compute_list, set1, 1);
	device->compute_list_set_push_constant(compute_list, push_data, sizeof(CompositePushConstants));
	device->compute_list_dispatch(compute_list, groups_x, groups_y, 1);
	device->compute_list_end();

	end_compute_label();
}

// ============================================================================
// Matrix conversion helpers
// ============================================================================

void RTReflectionEffect::projection_to_floats(const Projection &proj, float out[16]) {
	// Projection stores as columns[4], each is Vector4.
	// GLSL mat4 is column-major: out[0..3] = column 0, etc.
	for (int col = 0; col < 4; col++) {
		const Vector4 &c = proj.columns[col];
		out[col * 4 + 0] = c.x;
		out[col * 4 + 1] = c.y;
		out[col * 4 + 2] = c.z;
		out[col * 4 + 3] = c.w;
	}
}

void RTReflectionEffect::transform_to_floats(const Transform3D &xform, float out[16]) {
	// Transform3D has basis (3x3) + origin. Convert to 4x4 column-major.
	// Column 0 = basis.get_column(0), Column 3 = origin
	const Basis &b = xform.basis;
	const Vector3 &o = xform.origin;

	// Column 0
	out[0] = b[0][0]; out[1] = b[1][0]; out[2] = b[2][0]; out[3] = 0.0f;
	// Column 1
	out[4] = b[0][1]; out[5] = b[1][1]; out[6] = b[2][1]; out[7] = 0.0f;
	// Column 2
	out[8] = b[0][2]; out[9] = b[1][2]; out[10] = b[2][2]; out[11] = 0.0f;
	// Column 3
	out[12] = o.x; out[13] = o.y; out[14] = o.z; out[15] = 1.0f;
}

// ============================================================================
// _cleanup_shaders
// ============================================================================

void RTReflectionEffect::_cleanup_shaders() {
	RenderingDevice *device = rd();
	if (!device) return;

	if (composite_pipeline_.is_valid())         { device->free_rid(composite_pipeline_); composite_pipeline_ = RID(); }
	if (temporal_denoise_pipeline_.is_valid())   { device->free_rid(temporal_denoise_pipeline_); temporal_denoise_pipeline_ = RID(); }
	if (spatial_denoise_pipeline_.is_valid())    { device->free_rid(spatial_denoise_pipeline_); spatial_denoise_pipeline_ = RID(); }
	if (trace_pipeline_.is_valid())              { device->free_rid(trace_pipeline_); trace_pipeline_ = RID(); }
	if (composite_shader_.is_valid())            { device->free_rid(composite_shader_); composite_shader_ = RID(); }
	if (temporal_denoise_shader_.is_valid())     { device->free_rid(temporal_denoise_shader_); temporal_denoise_shader_ = RID(); }
	if (spatial_denoise_shader_.is_valid())      { device->free_rid(spatial_denoise_shader_); spatial_denoise_shader_ = RID(); }
	if (trace_shader_.is_valid())                { device->free_rid(trace_shader_); trace_shader_ = RID(); }
}

// ============================================================================
// _bind_methods — GDScript properties for tuning
// ============================================================================

void RTReflectionEffect::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_roughness_threshold", "threshold"),
		&RTReflectionEffect::set_roughness_threshold);
	ClassDB::bind_method(D_METHOD("get_roughness_threshold"),
		&RTReflectionEffect::get_roughness_threshold);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "roughness_threshold",
		PROPERTY_HINT_RANGE, "0.0,1.0,0.01"),
		"set_roughness_threshold", "get_roughness_threshold");

	ClassDB::bind_method(D_METHOD("set_ray_max_distance", "distance"),
		&RTReflectionEffect::set_ray_max_distance);
	ClassDB::bind_method(D_METHOD("get_ray_max_distance"),
		&RTReflectionEffect::get_ray_max_distance);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ray_max_distance",
		PROPERTY_HINT_RANGE, "1.0,1000.0,1.0"),
		"set_ray_max_distance", "get_ray_max_distance");

	ClassDB::bind_method(D_METHOD("set_temporal_blend", "blend"),
		&RTReflectionEffect::set_temporal_blend);
	ClassDB::bind_method(D_METHOD("get_temporal_blend"),
		&RTReflectionEffect::get_temporal_blend);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "temporal_blend",
		PROPERTY_HINT_RANGE, "0.01,1.0,0.01"),
		"set_temporal_blend", "get_temporal_blend");

	ClassDB::bind_method(D_METHOD("set_reflection_intensity", "intensity"),
		&RTReflectionEffect::set_reflection_intensity);
	ClassDB::bind_method(D_METHOD("get_reflection_intensity"),
		&RTReflectionEffect::get_reflection_intensity);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "reflection_intensity",
		PROPERTY_HINT_RANGE, "0.0,4.0,0.01"),
		"set_reflection_intensity", "get_reflection_intensity");
}

// We need getter/setter definitions since they're referenced in _bind_methods
// but not declared in the header. Let's add them inline:
void RTReflectionEffect::set_roughness_threshold(float v) { roughness_threshold_ = v; }
float RTReflectionEffect::get_roughness_threshold() const { return roughness_threshold_; }
void RTReflectionEffect::set_ray_max_distance(float v) { ray_max_distance_ = v; }
float RTReflectionEffect::get_ray_max_distance() const { return ray_max_distance_; }
void RTReflectionEffect::set_temporal_blend(float v) { temporal_blend_ = v; }
float RTReflectionEffect::get_temporal_blend() const { return temporal_blend_; }
void RTReflectionEffect::set_reflection_intensity(float v) { reflection_intensity_ = v; }
float RTReflectionEffect::get_reflection_intensity() const { return reflection_intensity_; }
