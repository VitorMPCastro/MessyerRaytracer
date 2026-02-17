#pragma once
// ray_renderer.h — Godot Node3D that produces ray-traced images.
//
// RayRenderer is the user-facing class for the graphics module.
// It orchestrates the full pipeline:
//   1. Extract camera → RayCamera
//   2. Generate rays (batch, pure math)
//   3. Submit to IRayService (auto-routes CPU/GPU)
//   4. Shade intersections into RayImage AOV channels
//   5. Convert selected channel → Godot Image → ImageTexture
//
// USAGE FROM GDSCRIPT:
//   var renderer = $RayRenderer
//   renderer.camera = $Camera3D
//   renderer.resolution = Vector2i(640, 480)
//   renderer.render_channel = RayRenderer.CHANNEL_NORMAL
//   renderer.render_frame()
//   $TextureRect.texture = renderer.get_texture()
//
// PERFORMANCE:
//   At 320×240 (~77K rays) with GPU backend: typically < 5ms per frame.
//   At 1920×1080 (~2M rays): depends on scene complexity and backend.
//   Ray generation is always < 1% of total frame time.

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/omni_light3d.hpp>
#include <godot_cpp/classes/spot_light3d.hpp>
#include <godot_cpp/classes/world_environment.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/sky.hpp>
#include <godot_cpp/classes/procedural_sky_material.hpp>
#include <godot_cpp/classes/panorama_sky_material.hpp>
#include <godot_cpp/classes/light3d.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include "modules/graphics/ray_camera.h"
#include "modules/graphics/ray_image.h"
#include "modules/graphics/shade_pass.h"
#include "core/intersection.h"
#include "api/light_data.h"

#include <vector>
#include <memory>

// Forward declare — we use the abstract dispatch interface from api/.
class IThreadDispatch;
class IPathTracer;

using namespace godot;

// Forward declare — we access via the abstract interface, not the server.
class IRayService;

class RayRenderer : public Node3D {
	GDCLASS(RayRenderer, Node3D)

public:
	/// Render channel enum — matches RayImage::Channel but exposed to GDScript.
	enum RenderChannel {
		CHANNEL_COLOR       = 0,
		CHANNEL_NORMAL      = 1,
		CHANNEL_DEPTH       = 2,
		CHANNEL_BARYCENTRIC = 3,
		CHANNEL_POSITION    = 4,
		CHANNEL_PRIM_ID     = 5,
		CHANNEL_HIT_MASK    = 6,
		CHANNEL_ALBEDO      = 7,
		CHANNEL_WIREFRAME   = 8,
		CHANNEL_UV          = 9,
		CHANNEL_FRESNEL     = 10,
	};

	RayRenderer();
	~RayRenderer();

	// ======== Properties (GDScript-bindable) ========

	void set_camera_path(const NodePath &path);
	NodePath get_camera_path() const;

	void set_resolution(const Vector2i &res);
	Vector2i get_resolution() const;

	void set_render_channel(int ch);
	int get_render_channel() const;

	void set_light_path(const NodePath &path);
	NodePath get_light_path() const;

	void set_environment_path(const NodePath &path);
	NodePath get_environment_path() const;

	void set_position_range(float range);
	float get_position_range() const;

	void set_shadows_enabled(bool enabled);
	bool get_shadows_enabled() const;

	void set_aa_enabled(bool enabled);
	bool get_aa_enabled() const;

	void set_aa_max_samples(int max_samples);
	int get_aa_max_samples() const;

	void set_max_bounces(int bounces);
	int get_max_bounces() const;

	void set_path_tracing_enabled(bool enabled);
	bool get_path_tracing_enabled() const;

	/// Number of frames accumulated so far in the AA buffer.
	int get_accumulation_count() const;

	/// Force-reset the accumulation buffer (e.g. after scene changes).
	void reset_accumulation();

	// ======== Actions ========

	/// Perform a full render: generate rays → trace → shade → output.
	/// After this call, get_texture() returns the rendered image.
	void render_frame();

	/// Get the output texture (updated after render_frame).
	Ref<ImageTexture> get_texture() const;

	/// Get the raw output image (updated after render_frame).
	Ref<Image> get_image() const;

	/// Wall-clock time of the last render_frame() in milliseconds.
	float get_render_ms() const;

	/// Time breakdown: ray generation, tracing, shadow rays, shading, conversion.
	float get_raygen_ms() const;
	float get_trace_ms() const;
	float get_shadow_ms() const;
	float get_shade_ms() const;
	float get_convert_ms() const;

protected:
	static void _bind_methods();

private:
	// ---- Properties ----
	NodePath camera_path_;
	NodePath light_path_;         // Optional: explicit DirectionalLight3D binding
	NodePath environment_path_;   // Optional: explicit WorldEnvironment binding
	Vector2i resolution_ = Vector2i(320, 240);
	int render_channel_   = CHANNEL_COLOR;
	float position_range_ = 10.0f;    // Modulo range for position visualization
	bool shadows_enabled_  = true;
	bool aa_enabled_       = true;
	int aa_max_samples_    = 256;  // Stop accumulating after this many frames

	// ---- Path tracing (Phase 3) ----
	int max_bounces_            = 4;     // Maximum bounce depth for path tracing
	bool path_tracing_enabled_  = true;  // Use multi-bounce path tracing for COLOR

	// ---- Internal state ----
	RayCamera camera_;
	RayImage framebuffer_;
	std::vector<Ray> rays_;
	std::vector<Intersection> hits_;
	std::vector<Ray> shadow_rays_;        // Shadow rays toward sun from hit points
	std::vector<uint8_t> shadow_mask_;    // 0 = in shadow, 1 = lit (for shade pass) — per light
	std::vector<uint8_t> shadow_hit_flags_; // Reused buffer for any-hit results (avoids per-frame alloc)

	// ---- Temporal accumulation (anti-aliasing) ----
	std::vector<float> accum_buffer_;    // Accumulated RGB per pixel (3 floats each)
	int accum_count_  = 0;               // Number of accumulated samples
	Vector3 prev_cam_origin_;            // For detecting camera motion
	Basis prev_cam_basis_;               // For detecting camera rotation

	// ---- Path tracer (Phase 3 — multi-bounce CPU path tracing) ----
	std::unique_ptr<IPathTracer> path_tracer_;  // Owned.  Created lazily on first path-traced frame.
	                                             // Implements IPathTracer (currently CPUPathTracer).

	// ---- Cached HDR panorama (Phase 1.4 — Environment Map) ----
	// Converted to FORMAT_RGBAF once and cached. Re-fetched only when
	// the panorama Texture2D resource changes (detected via instance ID comparison).
	Ref<Image> cached_panorama_image_;           // RGBAF32 image data
	uint64_t cached_panorama_instance_id_ = 0;   // ObjectID of the last-seen panorama Texture2D
	Ref<ImageTexture> output_texture_;
	Ref<Image> output_image_;  // Cached for zero-alloc parallel conversion
	IThreadDispatch *pool_ = nullptr;  // Shared thread pool from IRayService (not owned)

	// ---- Timing (ms) ----
	float total_ms_    = 0.0f;
	float raygen_ms_   = 0.0f;
	float trace_ms_    = 0.0f;
	float shadow_ms_   = 0.0f;
	float shade_ms_    = 0.0f;
	float convert_ms_  = 0.0f;

	// ---- Service access ----
	IRayService *_get_service() const;

	// ---- Scene node resolution (Godot-Native Principle) ----
	Camera3D *_resolve_camera() const;
	DirectionalLight3D *_resolve_light() const;
	WorldEnvironment *_resolve_environment() const;

	/// Discover all lights in the scene (directional, omni, spot) and populate
	/// a SceneLightData struct.  Called once per frame.
	SceneLightData _resolve_all_lights() const;

	// ---- Internal pipeline stages ----
	void _generate_rays(Camera3D *cam);
	void _trace_rays(IRayService *svc, bool coherent = true);
	void _trace_shadow_rays(IRayService *svc, const SceneLightData &lights);
	void _shade_results(Camera3D *cam, const SceneLightData &lights, WorldEnvironment *world_env);
	void _convert_output();

	// ---- Path tracing (Phase 3) ----
	/// Extract environment data from WorldEnvironment.  Shared between
	/// _shade_results and path tracer to avoid code duplication.
	ShadePass::EnvironmentData _build_env_data(WorldEnvironment *world_env);
};

VARIANT_ENUM_CAST(RayRenderer::RenderChannel);
