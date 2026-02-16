// ray_renderer.cpp — RayRenderer implementation.

#include "modules/graphics/ray_renderer.h"
#include "modules/graphics/shade_pass.h"
#include "api/ray_service.h"
// NOTE: dispatch/thread_pool.h is included here despite the module decoupling rule
// (modules should only include api/ and core/). This is a justified exception:
// RayRenderer parallelizes per-pixel shading across CPU cores, which is module-level
// work (not ray tracing). IRayService handles ray batch dispatch but not pixel
// shading. Exposing ThreadPool through an API abstraction would add complexity
// with no decoupling benefit — only RayRenderer uses it for this purpose.
#include "dispatch/thread_pool.h"
#include "core/asserts.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/scene_tree.hpp>

#include <chrono>
#include <algorithm>
#include <cstring>

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

void RayRenderer::set_light_path(const NodePath &path) { light_path_ = path; }
NodePath RayRenderer::get_light_path() const { return light_path_; }

void RayRenderer::set_environment_path(const NodePath &path) { environment_path_ = path; }
NodePath RayRenderer::get_environment_path() const { return environment_path_; }

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

void RayRenderer::set_position_range(float range) {
	position_range_ = (range > 0.001f) ? range : 0.001f;
}
float RayRenderer::get_position_range() const { return position_range_; }

void RayRenderer::set_shadows_enabled(bool enabled) {
	shadows_enabled_ = enabled;
}
bool RayRenderer::get_shadows_enabled() const { return shadows_enabled_; }

void RayRenderer::set_aa_enabled(bool enabled) {
	aa_enabled_ = enabled;
	if (!enabled) { accum_count_ = 0; }
}
bool RayRenderer::get_aa_enabled() const { return aa_enabled_; }

void RayRenderer::set_aa_max_samples(int max_samples) {
	aa_max_samples_ = (max_samples > 1) ? max_samples : 1;
}
int RayRenderer::get_aa_max_samples() const { return aa_max_samples_; }

int RayRenderer::get_accumulation_count() const { return accum_count_; }
void RayRenderer::reset_accumulation() { accum_count_ = 0; }

// ============================================================================
// Timing accessors
// ============================================================================

float RayRenderer::get_render_ms() const { return total_ms_; }
float RayRenderer::get_raygen_ms() const { return raygen_ms_; }
float RayRenderer::get_trace_ms() const { return trace_ms_; }
float RayRenderer::get_shadow_ms() const { return shadow_ms_; }
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

	// Resolve scene nodes (Godot-Native Principle: read from scene tree).
	DirectionalLight3D *light = _resolve_light();
	WorldEnvironment *world_env = _resolve_environment();

	// Read sun direction from the DirectionalLight3D (its -Z axis in world space).
	// Fallback: straight down if no light found.
	Vector3 sun_dir = Vector3(0.0f, -1.0f, 0.0f);
	if (light) {
		sun_dir = -light->get_global_transform().basis.get_column(2);
		sun_dir = sun_dir.normalized();
	}

	// 1. Generate rays
	auto t0 = Clock::now();
	_generate_rays(cam);
	auto t1 = Clock::now();

	// 2. Trace primary rays
	_trace_rays(svc);
	auto t2 = Clock::now();

	// 3. Trace shadow rays (sun direction, any-hit)
	_trace_shadow_rays(svc, sun_dir);
	auto t3 = Clock::now();

	// 4. Shade all AOV channels (passes resolved scene nodes for Godot-native reads)
	_shade_results(cam, light, world_env);
	auto t4 = Clock::now();

	// 5. Accumulate (anti-aliasing temporal blend) and convert → Image → ImageTexture
	_convert_output();
	auto t5 = Clock::now();

	// Record timing.
	auto to_ms = [](auto dur) {
		return std::chrono::duration<float, std::milli>(dur).count();
	};
	raygen_ms_  = to_ms(t1 - t0);
	trace_ms_   = to_ms(t2 - t1);
	shadow_ms_  = to_ms(t3 - t2);
	shade_ms_   = to_ms(t4 - t3);
	convert_ms_ = to_ms(t5 - t4);
	total_ms_   = to_ms(t5 - t_start);

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

/// Resolve DirectionalLight3D: explicit path first, then auto-discover.
DirectionalLight3D *RayRenderer::_resolve_light() const {
	RT_ASSERT(is_inside_tree(), "_resolve_light called before node entered tree");
	RT_ASSERT(get_tree() != nullptr, "SceneTree must be available for light resolution");

	// 1. Explicit NodePath binding (preferred).
	if (!light_path_.is_empty()) {
		Node *node = const_cast<RayRenderer *>(this)->get_node_or_null(light_path_);
		if (node) {
			DirectionalLight3D *light = Object::cast_to<DirectionalLight3D>(node);
			if (light) { return light; }
		}
		WARN_PRINT_ONCE("RayRenderer: light_path does not point to a DirectionalLight3D — auto-discovering.");
	}

	// 2. Auto-discover: walk up to the scene root, find first DirectionalLight3D.
	Node *root = const_cast<RayRenderer *>(this)->get_tree()
		? const_cast<RayRenderer *>(this)->get_tree()->get_current_scene()
		: nullptr;
	if (root) {
		TypedArray<Node> lights = root->find_children("*", "DirectionalLight3D", true, false);
		if (lights.size() > 0) {
			return Object::cast_to<DirectionalLight3D>(Object::cast_to<Node>(lights[0]));
		}
	}

	WARN_PRINT_ONCE("RayRenderer: No DirectionalLight3D found in scene — using fallback sun direction.");
	return nullptr;
}

/// Resolve WorldEnvironment: explicit path first, then auto-discover.
WorldEnvironment *RayRenderer::_resolve_environment() const {
	RT_ASSERT(is_inside_tree(), "_resolve_environment called before node entered tree");
	RT_ASSERT(get_tree() != nullptr, "SceneTree must be available for environment resolution");

	// 1. Explicit NodePath binding (preferred).
	if (!environment_path_.is_empty()) {
		Node *node = const_cast<RayRenderer *>(this)->get_node_or_null(environment_path_);
		if (node) {
			WorldEnvironment *env = Object::cast_to<WorldEnvironment>(node);
			if (env) { return env; }
		}
		WARN_PRINT_ONCE("RayRenderer: environment_path does not point to a WorldEnvironment — auto-discovering.");
	}

	// 2. Auto-discover: find first WorldEnvironment in scene tree.
	Node *root = const_cast<RayRenderer *>(this)->get_tree()
		? const_cast<RayRenderer *>(this)->get_tree()->get_current_scene()
		: nullptr;
	if (root) {
		TypedArray<Node> envs = root->find_children("*", "WorldEnvironment", true, false);
		if (envs.size() > 0) {
			return Object::cast_to<WorldEnvironment>(Object::cast_to<Node>(envs[0]));
		}
	}

	WARN_PRINT_ONCE("RayRenderer: No WorldEnvironment found in scene — using fallback sky/tonemapping.");
	return nullptr;
}

void RayRenderer::_generate_rays(Camera3D *cam) {
	RT_ASSERT_NOT_NULL(cam);
	RT_ASSERT(resolution_.x > 0 && resolution_.y > 0, "Resolution must be positive for ray generation");

	int w = resolution_.x;
	int h = resolution_.y;

	// Setup camera projection (extracts params once).
	camera_.setup(cam, w, h);

	// ---- Camera motion detection for AA accumulation ----
	// Compare current camera state with previous frame.
	// If camera moved or rotated, reset accumulation.
	Vector3 cur_origin = camera_.origin();
	Basis cur_basis = camera_.basis();

	if (aa_enabled_) {
		// Threshold: ~0.1mm position change or ~0.001 rad rotation.
		static constexpr float POS_EPS = 1e-4f;
		static constexpr float ROT_EPS = 1e-3f;

		bool moved = (cur_origin - prev_cam_origin_).length_squared() > POS_EPS * POS_EPS;
		bool rotated = false;
		if (!moved) {
			// Compare basis columns (quick and sufficient).
			for (int c = 0; c < 3 && !rotated; c++) {
				rotated = (cur_basis.get_column(c) - prev_cam_basis_.get_column(c)).length_squared() > ROT_EPS * ROT_EPS;
			}
		}

		if (moved || rotated || accum_count_ == 0) {
			accum_count_ = 0;
		}
	} else {
		accum_count_ = 0;
	}

	prev_cam_origin_ = cur_origin;
	prev_cam_basis_ = cur_basis;

	// Resize ray buffer if needed.
	rays_.resize(static_cast<size_t>(w) * h);

	if (aa_enabled_ && accum_count_ < aa_max_samples_) {
		// ---- Jittered ray generation (Halton 2,3 quasi-random sequence) ----
		// Different jitter per frame → different sub-pixel samples.
		// Halton base-2 and base-3 give low-discrepancy coverage of [0,1)^2.
		int sample_idx = accum_count_;
		// Halton base-2: reverse bits of sample_idx
		auto halton2 = [](int idx) -> float {
			float f = 1.0f, r = 0.0f;
			int i = idx;
			while (i > 0) {
				f *= 0.5f;
				r += f * static_cast<float>(i & 1);
				i >>= 1;
			}
			return r;
		};
		// Halton base-3
		auto halton3 = [](int idx) -> float {
			float f = 1.0f, r = 0.0f;
			int i = idx;
			static constexpr float INV3 = 1.0f / 3.0f;
			while (i > 0) {
				f *= INV3;
				r += f * static_cast<float>(i % 3);
				i /= 3;
			}
			return r;
		};

		float jx = halton2(sample_idx + 1); // +1 to avoid (0,0) on first sample
		float jy = halton3(sample_idx + 1);

		pool_->dispatch_and_wait(h, 16, [this, w, jx, jy](int y_start, int y_end) {
			camera_.generate_rays_tile_jittered(
				rays_.data() + static_cast<ptrdiff_t>(y_start) * w,
				0, y_start, w, y_end, jx, jy);
		});
	} else {
		// Non-jittered (AA disabled or max samples reached) — pixel centers.
		pool_->dispatch_and_wait(h, 16, [this, w](int y_start, int y_end) {
			camera_.generate_rays_tile(
				rays_.data() + static_cast<ptrdiff_t>(y_start) * w,
				0, y_start, w, y_end);
		});
	}
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

void RayRenderer::_trace_shadow_rays(IRayService *svc, const Vector3 &sun_dir) {
	RT_ASSERT_NOT_NULL(svc);
	RT_ASSERT(sun_dir.is_finite(), "Shadow ray sun direction must be finite");

	int count = static_cast<int>(hits_.size());
	shadow_mask_.resize(count);

	// If shadows are disabled, mark everything as lit.
	if (!shadows_enabled_) {
		std::fill(shadow_mask_.begin(), shadow_mask_.end(), static_cast<uint8_t>(1));
		return;
	}

	// Build shadow rays: from hit point toward the sun.
	// Offset origin along the geometric normal to avoid self-intersection.
	static constexpr float SHADOW_BIAS = 1e-3f;
	static constexpr float SHADOW_MAX_DIST = 1000.0f;

	shadow_rays_.resize(count);

	pool_->dispatch_and_wait(count, 256, [this, sun_dir, count](int start, int end) {
		for (int i = start; i < end; i++) {
			if (hits_[i].hit()) {
				Vector3 origin = hits_[i].position + hits_[i].normal * SHADOW_BIAS;
				shadow_rays_[i] = Ray(origin, sun_dir, 0.0f, SHADOW_MAX_DIST);
			} else {
				// Miss pixels — create a degenerate ray that won't hit anything.
				shadow_rays_[i] = Ray(Vector3(0, 0, 0), Vector3(0, 1, 0), 0.0f, 0.0f);
			}
		}
	});

	// Batch any-hit query for shadow occlusion.
	std::unique_ptr<bool[]> hit_flags(new bool[count]);
	RayQuery query = RayQuery::any_hit(shadow_rays_.data(), count);
	query.coherent = false; // Shadow rays are incoherent — divergent origins.
	RayQueryResult result;
	result.hit_flags = hit_flags.get();

	svc->submit(query, result);

	// Convert: hit_flags (true = occluded) → shadow_mask (0 = shadow, 1 = lit).
	pool_->dispatch_and_wait(count, 1024, [this, &hit_flags](int start, int end) {
		for (int i = start; i < end; i++) {
			shadow_mask_[i] = hit_flags[i] ? 0 : 1;
		}
	});
}

void RayRenderer::_shade_results(Camera3D *cam, DirectionalLight3D *light, WorldEnvironment *world_env) {
	RT_ASSERT(!hits_.empty(), "Hits buffer must not be empty before shading");
	RT_ASSERT(resolution_.x > 0 && resolution_.y > 0, "Resolution must be positive for shading");

	int count = static_cast<int>(hits_.size());
	int w = resolution_.x;
	int h = resolution_.y;

	// Ensure the framebuffer is sized.
	framebuffer_.resize(w, h);
	// No clear needed — every shade function writes every pixel (hit or miss path).

	// =========================================================================
	// Godot-Native: read sun from DirectionalLight3D (per-frame scene read)
	// =========================================================================
	Vector3 sun_dir = Vector3(0.0f, -1.0f, 0.0f);
	Vector3 sun_col = Vector3(1.0f, 1.0f, 1.0f);
	if (light) {
		sun_dir = -light->get_global_transform().basis.get_column(2);
		sun_dir = sun_dir.normalized();
		Color lc = light->get_color();
		float energy = light->get_param(Light3D::PARAM_ENERGY);
		sun_col = Vector3(lc.r, lc.g, lc.b) * energy;
	}

	// =========================================================================
	// Godot-Native: read sky, ambient, tone mapping from WorldEnvironment
	// =========================================================================
	ShadePass::EnvironmentData env_data; // defaults: neutral sky, Reinhard, 0.15 ambient
	if (world_env) {
		Ref<Environment> env_res = world_env->get_environment();
		if (env_res.is_valid()) {
			// Tone mapping mode — match what the user configured in the editor.
			env_data.tonemap_mode = static_cast<int>(env_res->get_tonemapper());

			// Ambient light energy and color.
			env_data.ambient_energy = env_res->get_ambient_light_energy();
			Color amb_col = env_res->get_ambient_light_color();
			env_data.ambient_r = amb_col.r;
			env_data.ambient_g = amb_col.g;
			env_data.ambient_b = amb_col.b;

			// Sky material: ProceduralSkyMaterial (analytic gradient) or
			// PanoramaSkyMaterial (HDR equirectangular map, Phase 1.4).
			Ref<Sky> sky = env_res->get_sky();
			if (sky.is_valid()) {
				Ref<Material> sky_mat_base = sky->get_material();

				// Try ProceduralSkyMaterial first (analytic gradient).
				ProceduralSkyMaterial *sky_mat = Object::cast_to<ProceduralSkyMaterial>(sky_mat_base.ptr());
				if (sky_mat) {
					Color top = sky_mat->get_sky_top_color();
					Color horizon = sky_mat->get_sky_horizon_color();
					Color ground = sky_mat->get_ground_bottom_color();
					env_data.sky_zenith_r = top.r;    env_data.sky_zenith_g = top.g;    env_data.sky_zenith_b = top.b;
					env_data.sky_horizon_r = horizon.r; env_data.sky_horizon_g = horizon.g; env_data.sky_horizon_b = horizon.b;
					env_data.sky_ground_r = ground.r;  env_data.sky_ground_g = ground.g;  env_data.sky_ground_b = ground.b;
					// Clear panorama — analytic sky takes priority when both are somehow set.
					env_data.panorama_data = nullptr;
				}

				// Try PanoramaSkyMaterial (HDR equirectangular environment map).
				PanoramaSkyMaterial *pano_mat = Object::cast_to<PanoramaSkyMaterial>(sky_mat_base.ptr());
				if (pano_mat) {
					Ref<Texture2D> pano_tex = pano_mat->get_panorama();
					if (pano_tex.is_valid()) {
						// Cache the RGBAF32 image — only re-fetch if the texture resource changed.
						uint64_t tex_id = pano_tex->get_instance_id();
						if (tex_id != cached_panorama_instance_id_ || cached_panorama_image_.is_null()) {
							Ref<Image> img = pano_tex->get_image();
							if (img.is_valid()) {
								// Convert to RGBAF32 for fast raw-pointer sampling in the shade pass.
								// This allocation happens only when the panorama changes — not per frame.
								if (img->get_format() != Image::FORMAT_RGBAF) {
									img->convert(Image::FORMAT_RGBAF);
								}
								cached_panorama_image_ = img;
								cached_panorama_instance_id_ = tex_id;
							}
						}
						// Pass raw float pointer to shade pass (no Godot headers needed there).
						if (cached_panorama_image_.is_valid()) {
							env_data.panorama_data = reinterpret_cast<const float *>(cached_panorama_image_->ptr());
							env_data.panorama_width = cached_panorama_image_->get_width();
							env_data.panorama_height = cached_panorama_image_->get_height();
							env_data.panorama_energy = pano_mat->get_energy_multiplier();
						}
					}
				}
			}
		}
	}

	// =========================================================================
	// Godot-Native: derive depth_range from Camera3D near/far
	// =========================================================================
	float depth_range = 100.0f; // fallback if no camera (shouldn't happen)
	if (cam) {
		depth_range = cam->get_far() - cam->get_near();
		if (depth_range < 0.001f) { depth_range = 100.0f; }
	}
	float inv_depth_range = 1.0f / depth_range;
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

	// Build shadow context from the pre-computed shadow mask.
	ShadePass::ShadowContext shadows;
	shadows.shadow_mask = shadow_mask_.data();
	shadows.count = count;

	pool_->dispatch_and_wait(count, 256, [&, ch, inv_depth_range, inv_pos_range,
			sun_dir, sun_col, env_data, rays_ptr, hits_ptr, shadows](int start, int end) {
		for (int i = start; i < end; i++) {
			ShadePass::shade_channel(framebuffer_, i, hits_ptr[i], rays_ptr[i],
				sun_dir, sun_col, inv_depth_range, inv_pos_range,
				shade_data, shadows, env_data, ch);
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

	const float *src = framebuffer_.channel(ch);

	// ---- Temporal accumulation (AA) ----
	// When AA is enabled and camera is stationary, blend new frame into running
	// average: accum = accum * (n/(n+1)) + new * (1/(n+1))
	// This produces a progressive refinement with no extra full-frame storage
	// beyond accum_buffer_ (RGBA floats, same layout as framebuffer channel).
	if (aa_enabled_ && accum_count_ < aa_max_samples_) {
		size_t buf_size = static_cast<size_t>(count) * 4;
		if (accum_buffer_.size() != buf_size) {
			// Resolution changed — reset accumulation.
			accum_buffer_.resize(buf_size);
			accum_count_ = 0;
		}

		if (accum_count_ == 0) {
			// First sample — copy directly.
			std::memcpy(accum_buffer_.data(), src, buf_size * sizeof(float));
		} else {
			// Incremental running average: new_avg = old_avg + (sample - old_avg) / (n+1)
			// This is numerically more stable than separate sum + divide.
			float inv_n = 1.0f / static_cast<float>(accum_count_ + 1);
			float *acc = accum_buffer_.data();

			pool_->dispatch_and_wait(count, 256, [acc, src, inv_n](int start, int end) {
				for (int i = start; i < end; i++) {
					int base = i * 4;
					acc[base + 0] += (src[base + 0] - acc[base + 0]) * inv_n;
					acc[base + 1] += (src[base + 1] - acc[base + 1]) * inv_n;
					acc[base + 2] += (src[base + 2] - acc[base + 2]) * inv_n;
					acc[base + 3] += (src[base + 3] - acc[base + 3]) * inv_n;
				}
			});
		}

		accum_count_++;
		// Read from accumulation buffer instead of raw framebuffer.
		src = accum_buffer_.data();
	}

	// Ensure the cached output image has the right dimensions.
	if (output_image_.is_null() ||
		output_image_->get_width() != w ||
		output_image_->get_height() != h) {
		output_image_ = Image::create_empty(w, h, false, Image::FORMAT_RGBA8);
	}

	// Parallel float→uint8 conversion — each thread processes a disjoint pixel range.
	uint8_t *dst = output_image_->ptrw();

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
	// ---- Scene binding properties (Godot-Native Principle) ----
	ClassDB::bind_method(D_METHOD("set_camera_path", "path"), &RayRenderer::set_camera_path);
	ClassDB::bind_method(D_METHOD("get_camera_path"), &RayRenderer::get_camera_path);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "camera_path"), "set_camera_path", "get_camera_path");

	ClassDB::bind_method(D_METHOD("set_light_path", "path"), &RayRenderer::set_light_path);
	ClassDB::bind_method(D_METHOD("get_light_path"), &RayRenderer::get_light_path);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "light_path"), "set_light_path", "get_light_path");

	ClassDB::bind_method(D_METHOD("set_environment_path", "path"), &RayRenderer::set_environment_path);
	ClassDB::bind_method(D_METHOD("get_environment_path"), &RayRenderer::get_environment_path);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "environment_path"), "set_environment_path", "get_environment_path");

	ClassDB::bind_method(D_METHOD("set_resolution", "resolution"), &RayRenderer::set_resolution);
	ClassDB::bind_method(D_METHOD("get_resolution"), &RayRenderer::get_resolution);
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2I, "resolution"), "set_resolution", "get_resolution");

	ClassDB::bind_method(D_METHOD("set_render_channel", "channel"), &RayRenderer::set_render_channel);
	ClassDB::bind_method(D_METHOD("get_render_channel"), &RayRenderer::get_render_channel);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "render_channel", PROPERTY_HINT_ENUM,
		"Color,Normal,Depth,Barycentric,Position,PrimID,HitMask,Albedo,Wireframe,UV,Fresnel"),
		"set_render_channel", "get_render_channel");

	ClassDB::bind_method(D_METHOD("set_position_range", "range"), &RayRenderer::set_position_range);
	ClassDB::bind_method(D_METHOD("get_position_range"), &RayRenderer::get_position_range);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "position_range", PROPERTY_HINT_RANGE, "0.01,10000,0.1"),
		"set_position_range", "get_position_range");

	ClassDB::bind_method(D_METHOD("set_shadows_enabled", "enabled"), &RayRenderer::set_shadows_enabled);
	ClassDB::bind_method(D_METHOD("get_shadows_enabled"), &RayRenderer::get_shadows_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "shadows_enabled"),
		"set_shadows_enabled", "get_shadows_enabled");

	ClassDB::bind_method(D_METHOD("set_aa_enabled", "enabled"), &RayRenderer::set_aa_enabled);
	ClassDB::bind_method(D_METHOD("get_aa_enabled"), &RayRenderer::get_aa_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "aa_enabled"),
		"set_aa_enabled", "get_aa_enabled");

	ClassDB::bind_method(D_METHOD("set_aa_max_samples", "max_samples"), &RayRenderer::set_aa_max_samples);
	ClassDB::bind_method(D_METHOD("get_aa_max_samples"), &RayRenderer::get_aa_max_samples);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "aa_max_samples", PROPERTY_HINT_RANGE, "1,4096,1"),
		"set_aa_max_samples", "get_aa_max_samples");

	ClassDB::bind_method(D_METHOD("get_accumulation_count"), &RayRenderer::get_accumulation_count);
	ClassDB::bind_method(D_METHOD("reset_accumulation"), &RayRenderer::reset_accumulation);

	// ---- Actions ----
	ClassDB::bind_method(D_METHOD("render_frame"), &RayRenderer::render_frame);
	ClassDB::bind_method(D_METHOD("get_texture"), &RayRenderer::get_texture);
	ClassDB::bind_method(D_METHOD("get_image"), &RayRenderer::get_image);

	// ---- Timing ----
	ClassDB::bind_method(D_METHOD("get_render_ms"), &RayRenderer::get_render_ms);
	ClassDB::bind_method(D_METHOD("get_raygen_ms"), &RayRenderer::get_raygen_ms);
	ClassDB::bind_method(D_METHOD("get_trace_ms"), &RayRenderer::get_trace_ms);
	ClassDB::bind_method(D_METHOD("get_shadow_ms"), &RayRenderer::get_shadow_ms);
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
