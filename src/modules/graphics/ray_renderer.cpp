// ray_renderer.cpp — RayRenderer implementation.

#include "modules/graphics/ray_renderer.h"
#include "modules/graphics/shade_pass.h"
#include "api/ray_service.h"
#include "dispatch/thread_pool.h"
#include "core/asserts.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <chrono>
#include <algorithm>

using namespace godot;

// ============================================================================
// Lifecycle
// ============================================================================

RayRenderer::RayRenderer()
	: pool_(std::make_unique<ThreadPool>()) {}
RayRenderer::~RayRenderer() = default;

// ============================================================================
// Property accessors
// ============================================================================

void RayRenderer::set_camera_path(const NodePath &path) { camera_path_ = path; }
NodePath RayRenderer::get_camera_path() const { return camera_path_; }

void RayRenderer::set_resolution(const Vector2i &res) {
	resolution_ = Vector2i(
		(res.x > 0) ? res.x : 1,
		(res.y > 0) ? res.y : 1
	);
}
Vector2i RayRenderer::get_resolution() const { return resolution_; }

void RayRenderer::set_render_channel(int ch) {
	render_channel_ = (ch >= 0 && ch < RayImage::CHANNEL_COUNT) ? ch : 0;
}
int RayRenderer::get_render_channel() const { return render_channel_; }

void RayRenderer::set_depth_range(float range) {
	depth_range_ = (range > 0.001f) ? range : 0.001f;
}
float RayRenderer::get_depth_range() const { return depth_range_; }

void RayRenderer::set_position_range(float range) {
	position_range_ = (range > 0.001f) ? range : 0.001f;
}
float RayRenderer::get_position_range() const { return position_range_; }

void RayRenderer::set_sun_direction(const Vector3 &dir) {
	sun_direction_ = dir.normalized();
}
Vector3 RayRenderer::get_sun_direction() const { return sun_direction_; }

// ============================================================================
// Timing accessors
// ============================================================================

float RayRenderer::get_render_ms() const { return total_ms_; }
float RayRenderer::get_raygen_ms() const { return raygen_ms_; }
float RayRenderer::get_trace_ms() const { return trace_ms_; }
float RayRenderer::get_shade_ms() const { return shade_ms_; }
float RayRenderer::get_convert_ms() const { return convert_ms_; }

// ============================================================================
// Texture output
// ============================================================================

Ref<ImageTexture> RayRenderer::get_texture() const { return output_texture_; }

Ref<Image> RayRenderer::get_image() const {
	return framebuffer_.to_image(static_cast<RayImage::Channel>(render_channel_));
}

// ============================================================================
// render_frame — the main pipeline
// ============================================================================

void RayRenderer::render_frame() {
	RT_ASSERT(resolution_.x > 0 && resolution_.y > 0, "Resolution must be positive");
	RT_ASSERT_NOT_NULL(pool_.get());

	using Clock = std::chrono::high_resolution_clock;
	auto t_start = Clock::now();

	// Resolve dependencies.
	IRayService *svc = _get_service();
	if (!svc) {
		ERR_FAIL_MSG("RayRenderer: RayTracerServer not available.");
	}

	Camera3D *cam = _resolve_camera();
	if (!cam) {
		ERR_FAIL_MSG("RayRenderer: No valid Camera3D at camera_path.");
	}

	// 1. Generate rays
	auto t0 = Clock::now();
	_generate_rays(cam);
	auto t1 = Clock::now();

	// 2. Trace
	_trace_rays(svc);
	auto t2 = Clock::now();

	// 3. Shade all AOV channels
	_shade_results();
	auto t3 = Clock::now();

	// 4. Convert selected channel → Image → ImageTexture
	_convert_output();
	auto t4 = Clock::now();

	// Record timing.
	auto to_ms = [](auto dur) {
		return std::chrono::duration<float, std::milli>(dur).count();
	};
	raygen_ms_  = to_ms(t1 - t0);
	trace_ms_   = to_ms(t2 - t1);
	shade_ms_   = to_ms(t3 - t2);
	convert_ms_ = to_ms(t4 - t3);
	total_ms_   = to_ms(t4 - t_start);

	emit_signal("frame_completed");
}

// ============================================================================
// Internal pipeline stages
// ============================================================================

IRayService *RayRenderer::_get_service() const {
	return get_ray_service();
}

Camera3D *RayRenderer::_resolve_camera() const {
	if (camera_path_.is_empty()) { return nullptr; }

	Node *node = const_cast<RayRenderer *>(this)->get_node_or_null(camera_path_);
	if (!node) { return nullptr; }

	return Object::cast_to<Camera3D>(node);
}

void RayRenderer::_generate_rays(Camera3D *cam) {
	RT_ASSERT_NOT_NULL(cam);
	RT_ASSERT(resolution_.x > 0 && resolution_.y > 0, "Resolution must be positive for ray generation");

	int w = resolution_.x;
	int h = resolution_.y;

	// Setup camera projection (extracts params once).
	camera_.setup(cam, w, h);

	// Resize ray buffer if needed.
	rays_.resize(static_cast<size_t>(w) * h);

	// Parallel ray generation — split by row chunks.
	// Each thread generates a contiguous strip of rows using the tile API.
	// generate_ray() is pure math on read-only camera state → thread-safe.
	pool_->dispatch_and_wait(h, 16, [this, w](int y_start, int y_end) {
		camera_.generate_rays_tile(
			rays_.data() + static_cast<ptrdiff_t>(y_start) * w,
			0, y_start, w, y_end);
	});
}

void RayRenderer::_trace_rays(IRayService *svc) {
	RT_ASSERT_NOT_NULL(svc);
	RT_ASSERT(!rays_.empty(), "Ray buffer must not be empty before tracing");

	int count = static_cast<int>(rays_.size());
	hits_.resize(count);

	// Submit through the service interface — routes to CPU or GPU transparently.
	// Primary camera rays are spatially coherent — adjacent pixels have nearby
	// directions. Mark coherent=true to skip the expensive Morton-code sort on GPU.
	RayQuery query = RayQuery::nearest(rays_.data(), count);
	query.coherent = true;
	RayQueryResult result;
	result.hits = hits_.data();

	svc->submit(query, result);
}

void RayRenderer::_shade_results() {
	RT_ASSERT(!hits_.empty(), "Hits buffer must not be empty before shading");
	RT_ASSERT(resolution_.x > 0 && resolution_.y > 0, "Resolution must be positive for shading");

	int count = static_cast<int>(hits_.size());
	int w = resolution_.x;
	int h = resolution_.y;

	// Ensure the framebuffer is sized.
	framebuffer_.resize(w, h);
	// No clear needed — every shade function writes every pixel (hit or miss path).

	// Pre-compute per-frame shading parameters.
	float inv_depth_range = 1.0f / depth_range_;
	float inv_pos_range   = 1.0f / position_range_;

	// Fetch material data from the scene (populated at build time).
	IRayService *svc = _get_service();
	SceneShadeData shade_data;
	if (svc) {
		shade_data = svc->get_shade_data();
	}

	// Shade ONLY the active channel — ~8× faster than shade_all().
	RayImage::Channel ch = static_cast<RayImage::Channel>(render_channel_);

	// Parallel shading — each thread processes a chunk of pixels.
	// write_pixel() targets independent indices → no data race.
	const Ray *rays_ptr = rays_.data();
	const Intersection *hits_ptr = hits_.data();
	Vector3 sun = sun_direction_;

	pool_->dispatch_and_wait(count, 256, [&, ch, inv_depth_range, inv_pos_range, sun,
			rays_ptr, hits_ptr](int start, int end) {
		for (int i = start; i < end; i++) {
			ShadePass::shade_channel(framebuffer_, i, hits_ptr[i], rays_ptr[i],
				sun, inv_depth_range, inv_pos_range, shade_data, ch);
		}
	});
}

void RayRenderer::_convert_output() {
	RT_ASSERT_BOUNDS(render_channel_, static_cast<int>(RayImage::CHANNEL_COUNT));
	RT_ASSERT(resolution_.x > 0 && resolution_.y > 0, "Resolution must be positive for output conversion");

	RayImage::Channel ch = static_cast<RayImage::Channel>(render_channel_);
	int w = resolution_.x;
	int h = resolution_.y;
	int count = w * h;
	if (count == 0) { return; }

	// Ensure the cached output image has the right dimensions.
	if (output_image_.is_null() ||
		output_image_->get_width() != w ||
		output_image_->get_height() != h) {
		output_image_ = Image::create_empty(w, h, false, Image::FORMAT_RGBA8);
	}

	// Parallel float→uint8 conversion — each thread processes a disjoint pixel range.
	uint8_t *dst = output_image_->ptrw();
	const float *src = framebuffer_.channel(ch);

	pool_->dispatch_and_wait(count, 1024, [dst, src](int start, int end) {
		for (int i = start; i < end; i++) {
			int base = i * 4;
			dst[base + 0] = static_cast<uint8_t>(
				std::min(255.0f, std::max(0.0f, src[base + 0] * 255.0f + 0.5f)));
			dst[base + 1] = static_cast<uint8_t>(
				std::min(255.0f, std::max(0.0f, src[base + 1] * 255.0f + 0.5f)));
			dst[base + 2] = static_cast<uint8_t>(
				std::min(255.0f, std::max(0.0f, src[base + 2] * 255.0f + 0.5f)));
			dst[base + 3] = static_cast<uint8_t>(
				std::min(255.0f, std::max(0.0f, src[base + 3] * 255.0f + 0.5f)));
		}
	});

	// Create or update the ImageTexture.
	if (output_texture_.is_null() ||
		output_texture_->get_width() != w ||
		output_texture_->get_height() != h) {
		output_texture_ = ImageTexture::create_from_image(output_image_);
	} else {
		output_texture_->update(output_image_);
	}
}

// ============================================================================
// GDScript bindings
// ============================================================================

void RayRenderer::_bind_methods() {
	// ---- Properties ----
	ClassDB::bind_method(D_METHOD("set_camera_path", "path"), &RayRenderer::set_camera_path);
	ClassDB::bind_method(D_METHOD("get_camera_path"), &RayRenderer::get_camera_path);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "camera_path"), "set_camera_path", "get_camera_path");

	ClassDB::bind_method(D_METHOD("set_resolution", "resolution"), &RayRenderer::set_resolution);
	ClassDB::bind_method(D_METHOD("get_resolution"), &RayRenderer::get_resolution);
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2I, "resolution"), "set_resolution", "get_resolution");

	ClassDB::bind_method(D_METHOD("set_render_channel", "channel"), &RayRenderer::set_render_channel);
	ClassDB::bind_method(D_METHOD("get_render_channel"), &RayRenderer::get_render_channel);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "render_channel", PROPERTY_HINT_ENUM,
		"Color,Normal,Depth,Barycentric,Position,PrimID,HitMask,Albedo,Wireframe,UV,Fresnel"),
		"set_render_channel", "get_render_channel");

	ClassDB::bind_method(D_METHOD("set_depth_range", "range"), &RayRenderer::set_depth_range);
	ClassDB::bind_method(D_METHOD("get_depth_range"), &RayRenderer::get_depth_range);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "depth_range", PROPERTY_HINT_RANGE, "0.01,10000,0.1"),
		"set_depth_range", "get_depth_range");

	ClassDB::bind_method(D_METHOD("set_position_range", "range"), &RayRenderer::set_position_range);
	ClassDB::bind_method(D_METHOD("get_position_range"), &RayRenderer::get_position_range);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "position_range", PROPERTY_HINT_RANGE, "0.01,10000,0.1"),
		"set_position_range", "get_position_range");

	ClassDB::bind_method(D_METHOD("set_sun_direction", "direction"), &RayRenderer::set_sun_direction);
	ClassDB::bind_method(D_METHOD("get_sun_direction"), &RayRenderer::get_sun_direction);
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "sun_direction"),
		"set_sun_direction", "get_sun_direction");

	// ---- Actions ----
	ClassDB::bind_method(D_METHOD("render_frame"), &RayRenderer::render_frame);
	ClassDB::bind_method(D_METHOD("get_texture"), &RayRenderer::get_texture);
	ClassDB::bind_method(D_METHOD("get_image"), &RayRenderer::get_image);

	// ---- Timing ----
	ClassDB::bind_method(D_METHOD("get_render_ms"), &RayRenderer::get_render_ms);
	ClassDB::bind_method(D_METHOD("get_raygen_ms"), &RayRenderer::get_raygen_ms);
	ClassDB::bind_method(D_METHOD("get_trace_ms"), &RayRenderer::get_trace_ms);
	ClassDB::bind_method(D_METHOD("get_shade_ms"), &RayRenderer::get_shade_ms);
	ClassDB::bind_method(D_METHOD("get_convert_ms"), &RayRenderer::get_convert_ms);

	// ---- Signal ----
	ADD_SIGNAL(MethodInfo("frame_completed"));

	// ---- Enum constants ----
	BIND_ENUM_CONSTANT(CHANNEL_COLOR);
	BIND_ENUM_CONSTANT(CHANNEL_NORMAL);
	BIND_ENUM_CONSTANT(CHANNEL_DEPTH);
	BIND_ENUM_CONSTANT(CHANNEL_BARYCENTRIC);
	BIND_ENUM_CONSTANT(CHANNEL_POSITION);
	BIND_ENUM_CONSTANT(CHANNEL_PRIM_ID);
	BIND_ENUM_CONSTANT(CHANNEL_HIT_MASK);
	BIND_ENUM_CONSTANT(CHANNEL_ALBEDO);
	BIND_ENUM_CONSTANT(CHANNEL_WIREFRAME);
	BIND_ENUM_CONSTANT(CHANNEL_UV);
	BIND_ENUM_CONSTANT(CHANNEL_FRESNEL);
}
