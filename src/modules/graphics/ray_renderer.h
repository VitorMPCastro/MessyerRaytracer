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
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include "modules/graphics/ray_camera.h"
#include "modules/graphics/ray_image.h"
#include "core/intersection.h"

#include <memory>
#include <vector>

// Forward declare — definition in dispatch/thread_pool.h, included in .cpp only.
class ThreadPool;

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

	void set_depth_range(float range);
	float get_depth_range() const;

	void set_position_range(float range);
	float get_position_range() const;

	void set_sun_direction(const Vector3 &dir);
	Vector3 get_sun_direction() const;

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

	/// Time breakdown: ray generation, tracing, shading, conversion.
	float get_raygen_ms() const;
	float get_trace_ms() const;
	float get_shade_ms() const;
	float get_convert_ms() const;

protected:
	static void _bind_methods();

private:
	// ---- Properties ----
	NodePath camera_path_;
	Vector2i resolution_ = Vector2i(320, 240);
	int render_channel_   = CHANNEL_COLOR;
	float depth_range_    = 100.0f;   // Max depth for depth visualization
	float position_range_ = 10.0f;    // Modulo range for position visualization
	Vector3 sun_direction_ = Vector3(0.5f, 0.8f, 0.3f).normalized();

	// ---- Internal state ----
	RayCamera camera_;
	RayImage framebuffer_;
	std::vector<Ray> rays_;
	std::vector<Intersection> hits_;
	Ref<ImageTexture> output_texture_;
	Ref<Image> output_image_;  // Cached for zero-alloc parallel conversion
	std::unique_ptr<ThreadPool> pool_;  // Parallel raygen / shade / convert

	// ---- Timing (ms) ----
	float total_ms_   = 0.0f;
	float raygen_ms_  = 0.0f;
	float trace_ms_   = 0.0f;
	float shade_ms_   = 0.0f;
	float convert_ms_ = 0.0f;

	// ---- Service access ----
	IRayService *_get_service() const;

	// ---- Internal pipeline stages ----
	Camera3D *_resolve_camera() const;
	void _generate_rays(Camera3D *cam);
	void _trace_rays(IRayService *svc);
	void _shade_results();
	void _convert_output();
};

VARIANT_ENUM_CAST(RayRenderer::RenderChannel);
