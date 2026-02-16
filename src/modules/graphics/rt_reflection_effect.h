#pragma once
// rt_reflection_effect.h — RT-enhanced reflections via CompositorEffect.
//
// Replaces Godot's screen-space reflections with true ray-traced reflections
// using our BVH compute shader. Industry hybrid approach: rasterize primary
// visibility, enhance with sparse RT effects, denoise.
//
// 4-PASS PIPELINE (per frame):
//   Pass 1 — Ray gen + BVH trace (rt_reflections.comp.glsl)
//   Pass 2 — Spatial denoise (rt_denoise_spatial.comp.glsl)
//   Pass 3 — Temporal accumulation (rt_denoise_temporal.comp.glsl)
//   Pass 4 — Fresnel-weighted composite (rt_composite.comp.glsl)
//
// USAGE:
//   Attached to a Compositor resource, which is set on WorldEnvironment.
//   RaySceneSetup handles this automatically when rt_reflections_enabled = true.
//   Can also be created manually in GDScript:
//     var effect = RTReflectionEffect.new()
//     compositor.compositor_effects.append(effect)

#include "rt_compositor_base.h"

using namespace godot;

class RTReflectionEffect : public RTCompositorBase {
	GDCLASS(RTReflectionEffect, RTCompositorBase)

public:
	RTReflectionEffect();
	~RTReflectionEffect();

protected:
	static void _bind_methods();

	// RTCompositorBase overrides.
	void _on_initialize_render() override;
	void _on_render(RenderData *render_data,
					Ref<RenderSceneBuffersRD> scene_buffers,
					RenderSceneData *scene_data,
					const Vector2i &render_size) override;

private:
	// ---- Configuration ----
	float roughness_threshold_ = 0.3f;
	float ray_max_distance_ = 100.0f;
	float temporal_blend_ = 0.1f;  // 0.1 = 90% history, 10% new
	float reflection_intensity_ = 1.0f;
	float fresnel_f0_ = 0.04f;

	// Denoise params
	float denoise_depth_sigma_ = 1.0f;
	float denoise_normal_sigma_ = 128.0f;
	float denoise_color_sigma_ = 4.0f;
	int denoise_kernel_radius_ = 2;

	// ---- Shaders + Pipelines ----
	RID trace_shader_;
	RID trace_pipeline_;
	RID spatial_denoise_shader_;
	RID spatial_denoise_pipeline_;
	RID temporal_denoise_shader_;
	RID temporal_denoise_pipeline_;
	RID composite_shader_;
	RID composite_pipeline_;

	// ---- Frame state ----
	uint32_t frame_count_ = 0;
	Vector2i current_render_size_;

	// ---- Push constant structs (must match GLSL push_constant layouts) ----
	#pragma pack(push, 1)
	struct TracePushConstants {
		float inv_projection[16];
		float inv_view[16];
		float roughness_threshold;
		float ray_max_distance;
		uint32_t frame_count;
		uint32_t pad;
	};
	static_assert(sizeof(TracePushConstants) == 144, "TracePushConstants must be 144 bytes");

	struct SpatialDenoisePushConstants {
		float depth_sigma;
		float normal_sigma;
		float color_sigma;
		int32_t kernel_radius;
	};
	static_assert(sizeof(SpatialDenoisePushConstants) == 16, "SpatialDenoisePushConstants must be 16 bytes");

	struct TemporalDenoisePushConstants {
		float blend_factor;
		float depth_threshold;
		uint32_t frame_count;
		uint32_t pad;
	};
	static_assert(sizeof(TemporalDenoisePushConstants) == 16, "TemporalDenoisePushConstants must be 16 bytes");

	struct CompositePushConstants {
		float inv_view[16];
		float roughness_threshold;
		float reflection_intensity;
		float f0;
		uint32_t pad;
	};
	static_assert(sizeof(CompositePushConstants) == 80, "CompositePushConstants must be 80 bytes");
	#pragma pack(pop)

	// ---- Intermediate texture management (lazy-init to avoid static init before Godot) ----
	static const StringName &_ctx_rt_reflections();
	static const StringName &_tex_reflection_raw();
	static const StringName &_tex_reflection_denoised();
	static const StringName &_tex_reflection_history();

	void _ensure_textures(const Ref<RenderSceneBuffersRD> &scene_buffers, const Vector2i &size);
	void _pass_trace(const Ref<RenderSceneBuffersRD> &scene_buffers,
					 RenderSceneData *scene_data,
					 const Vector2i &size);
	void _pass_spatial_denoise(const Ref<RenderSceneBuffersRD> &scene_buffers,
							   const Vector2i &size);
	void _pass_temporal_denoise(const Ref<RenderSceneBuffersRD> &scene_buffers,
								const Vector2i &size);
	void _pass_composite(const Ref<RenderSceneBuffersRD> &scene_buffers,
						 RenderSceneData *scene_data,
						 const Vector2i &size);

	// Helper to convert Godot matrix to float[16] for push constants.
	static void _projection_to_floats(const Projection &proj, float out[16]);
	static void _transform_to_floats(const Transform3D &xform, float out[16]);

	void _cleanup_shaders();

public:
	// ---- GDScript-bindable property accessors ----
	void set_roughness_threshold(float threshold);
	float get_roughness_threshold() const;
	void set_ray_max_distance(float distance);
	float get_ray_max_distance() const;
	void set_temporal_blend(float blend);
	float get_temporal_blend() const;
	void set_reflection_intensity(float intensity);
	float get_reflection_intensity() const;
};
