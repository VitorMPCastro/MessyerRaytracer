#pragma once
// cpu_path_tracer.h — CPU multi-bounce path tracer implementing IPathTracer.
//
// WHAT:  Iterative bounce loop on the CPU.  For each bounce:
//          1. Trace rays (nearest-hit via IRayService)
//          2. Trace shadow rays for NEE (any-hit via IRayService)
//          3. Per-pixel: miss → sky, hit → NEE + emission + sample bounce
//        Writes tone-mapped + gamma-corrected RGBA to the output buffer.
//
// WHY:   Extracted from RayRenderer so that:
//          (a) RayRenderer delegates to IPathTracer without knowing the impl
//          (b) A future GPU path tracer can implement the same interface
//          (c) Bounce buffers are owned here, not polluting RayRenderer
//
// HOW:   All internal buffers (hits, shadow rays, path states) are pre-allocated
//        and reused across frames.  Resized only when resolution changes.
//        Ray submission goes through IRayService::submit() — the service routes
//        to the optimal backend (CPU or GPU) transparently.
//
//        Degenerate rays (t_min == t_max == 0) are used for inactive pixels.
//        TinyBVH exits immediately on degenerate rays — zero overhead.

#include "api/path_tracer.h"
#include "api/ray_query.h"
#include "modules/graphics/path_trace.h"
#include "modules/graphics/shade_pass.h"
#include "api/thread_dispatch.h"
#include "api/ray_service.h"
#include "core/ray.h"
#include "core/intersection.h"
#include "core/asserts.h"

#include <vector>
#include <atomic>
#include <cmath>
#include <algorithm>

/// CPU multi-bounce path tracer.
///
/// Thread-safety: NOT thread-safe for concurrent trace_frame() calls.
/// Worker threads are dispatched internally via the provided IThreadDispatch.
///
/// Owns all internal reuse buffers.  No per-frame allocations after the
/// first frame at a given resolution.
class CPUPathTracer final : public IPathTracer {
public:
	CPUPathTracer() = default;
	~CPUPathTracer() override = default;

	// Non-copyable, non-movable (owns reuse buffers, stateful).
	CPUPathTracer(const CPUPathTracer &) = delete;
	CPUPathTracer &operator=(const CPUPathTracer &) = delete;
	CPUPathTracer(CPUPathTracer &&) = delete;
	CPUPathTracer &operator=(CPUPathTracer &&) = delete;

	void trace_frame(const PathTraceParams &params,
		Ray *primary_rays,
		float *color_output,
		IRayService *svc,
		IThreadDispatch *pool) override {

		RT_ASSERT_NOT_NULL(primary_rays);
		RT_ASSERT_NOT_NULL(color_output);
		RT_ASSERT_NOT_NULL(svc);
		RT_ASSERT_NOT_NULL(pool);
		RT_ASSERT(params.width > 0 && params.height > 0, "Resolution must be positive for path tracing");

		const int w = params.width;
		const int h = params.height;
		const int count = w * h;
		if (count == 0) { return; }

		const ShadePass::EnvironmentData &env_data = params.env;
		const SceneShadeData &shade_data = params.shade;
		const SceneLightData &lights = params.lights;

		// Ensure reuse buffers are sized.
		hits_.resize(count);
		path_states_.resize(count);

		// Initialize per-pixel path states.
		pool->dispatch_and_wait(count, 1024, [this, &params](int start, int end) {
			for (int i = start; i < end; i++) {
				path_states_[i].init(static_cast<uint32_t>(i), params.sample_index);
			}
		});

		// ---- Bounce loop ----
		for (int bounce = 0; bounce <= params.max_bounces; bounce++) {
			// 1. Trace rays — coherent for primary, incoherent for bounces.
			_trace_rays(svc, primary_rays, count, /*coherent=*/bounce == 0);

			// 2. Trace shadow rays for NEE.
			_trace_shadow_rays(svc, pool, hits_.data(), primary_rays, count,
				lights, params.shadows_enabled);

			// Build shadow context from the pre-computed shadow mask.
			ShadePass::ShadowContext shadows;
			shadows.shadow_mask = shadow_mask_.data();
			shadows.count = count;
			shadows.light_count = lights.light_count;

			// 3. Process hits — parallel over pixels.
			std::atomic<int> active_count{0};

			const Intersection *hits_ptr = hits_.data();
			const Ray *rays_ptr = primary_rays;
			const int max_bounces = params.max_bounces;

			pool->dispatch_and_wait(count, 256, [&, hits_ptr, rays_ptr, shadows, env_data,
					bounce, max_bounces](int start, int end) {
				int local_active = 0;
				for (int i = start; i < end; i++) {
					PathTrace::PathState &ps = path_states_[i];
					if (!ps.active) { continue; }

					const Intersection &hit = hits_ptr[i];
					const Ray &ray = rays_ptr[i];

					// ---- Miss: accumulate sky color, deactivate ----
					if (!hit.hit()) {
						float sky_r, sky_g, sky_b;
						ShadePass::sky_color(ray.direction, env_data, sky_r, sky_g, sky_b);
						ps.accum_r += ps.throughput_r * sky_r;
						ps.accum_g += ps.throughput_g * sky_g;
						ps.accum_b += ps.throughput_b * sky_b;
						ps.active = false;
						primary_rays[i] = Ray(Vector3(0, 0, 0), Vector3(0, 1, 0), 0.0f, 0.0f);
						continue;
					}

					// ---- Hit: extract surface properties ----
					PathTrace::SurfaceInfo surf = PathTrace::extract_surface(hit, ray, shade_data);

					// Accumulate emission * throughput.
					ps.accum_r += ps.throughput_r * surf.emit_r;
					ps.accum_g += ps.throughput_g * surf.emit_g;
					ps.accum_b += ps.throughput_b * surf.emit_b;

					// ---- NEE: direct light contribution ----
					float direct_r, direct_g, direct_b;
					PathTrace::compute_direct_light(surf, shadows, i, lights,
						direct_r, direct_g, direct_b);
					ps.accum_r += ps.throughput_r * direct_r;
					ps.accum_g += ps.throughput_g * direct_g;
					ps.accum_b += ps.throughput_b * direct_b;

					// ---- Ambient contribution (first bounce only) ----
					if (bounce == 0) {
						ps.accum_r += ps.throughput_r * surf.diff_r * env_data.ambient_r * env_data.ambient_energy;
						ps.accum_g += ps.throughput_g * surf.diff_g * env_data.ambient_g * env_data.ambient_energy;
						ps.accum_b += ps.throughput_b * surf.diff_b * env_data.ambient_b * env_data.ambient_energy;
					}

					// ---- Last bounce: no more bouncing ----
					if (bounce == max_bounces) {
						ps.active = false;
						primary_rays[i] = Ray(Vector3(0, 0, 0), Vector3(0, 1, 0), 0.0f, 0.0f);
						continue;
					}

					PathTrace::BounceResult br = PathTrace::sample_bounce(surf, ps.rng);
					if (!br.valid) {
						ps.active = false;
						primary_rays[i] = Ray(Vector3(0, 0, 0), Vector3(0, 1, 0), 0.0f, 0.0f);
						continue;
					}

					// Update throughput.
					ps.throughput_r *= br.weight_r;
					ps.throughput_g *= br.weight_g;
					ps.throughput_b *= br.weight_b;

					// ---- Russian roulette (bounce >= 2) ----
					if (bounce >= 2) {
						float max_t = std::max({ps.throughput_r, ps.throughput_g, ps.throughput_b});
						float survival = std::min(max_t, 0.95f);
						if (ps.rng.next_float() >= survival) {
							ps.active = false;
							primary_rays[i] = Ray(Vector3(0, 0, 0), Vector3(0, 1, 0), 0.0f, 0.0f);
							continue;
						}
						float inv_survival = 1.0f / survival;
						ps.throughput_r *= inv_survival;
						ps.throughput_g *= inv_survival;
						ps.throughput_b *= inv_survival;
					}

					// Set next ray.
					Vector3 next_origin = surf.position + surf.normal * PathTrace::PT_SHADOW_BIAS;
					primary_rays[i] = Ray(next_origin, br.direction, 1e-4f, 1e30f);

					local_active++;
				}
				active_count.fetch_add(local_active, std::memory_order_relaxed);
			});

			if (active_count.load(std::memory_order_relaxed) == 0) { break; }
		}

		// ---- Write accumulated radiance to output (tone map + gamma) ----
		pool->dispatch_and_wait(count, 256, [this, color_output, env_data](int start, int end) {
			for (int i = start; i < end; i++) {
				const PathTrace::PathState &ps = path_states_[i];
				float r = ps.accum_r;
				float g = ps.accum_g;
				float b = ps.accum_b;

				ShadePass::tonemap_rgb(r, g, b, env_data.tonemap_mode);

				constexpr float GAMMA = 1.0f / 2.2f;
				r = std::pow(std::max(r, 0.0f), GAMMA);
				g = std::pow(std::max(g, 0.0f), GAMMA);
				b = std::pow(std::max(b, 0.0f), GAMMA);

				int base = i * 4;
				color_output[base + 0] = r;
				color_output[base + 1] = g;
				color_output[base + 2] = b;
				color_output[base + 3] = 1.0f;
			}
		});
	}

private:
	// ---- Internal reuse buffers ----
	// Resized on first use or when resolution changes.  Never shrink.
	std::vector<Intersection> hits_;               // Nearest-hit results per pixel.
	std::vector<Ray> shadow_rays_;                 // Shadow rays (per light * per pixel).
	std::vector<uint8_t> shadow_mask_;             // 0 = shadow, 1 = lit (per light * per pixel).
	std::vector<uint8_t> shadow_hit_flags_;        // Raw any-hit flags before inversion.
	std::vector<PathTrace::PathState> path_states_; // Per-pixel path accumulation state.

	/// Submit rays for nearest-hit tracing through the service.
	void _trace_rays(IRayService *svc, Ray *rays, int count, bool coherent) {
		RT_ASSERT_NOT_NULL(svc);
		RT_ASSERT(count > 0, "Ray count must be positive");

		hits_.resize(count);

		RayQuery query = RayQuery::nearest(rays, count);
		query.coherent = coherent;
		RayQueryResult result;
		result.hits = hits_.data();

		svc->submit(query, result);
	}

	/// Generate and trace shadow rays for all lights.
	void _trace_shadow_rays(IRayService *svc, IThreadDispatch *pool,
			const Intersection *hits, const Ray * /*unused_rays*/, int count,
			const SceneLightData &lights, bool shadows_enabled) {
		RT_ASSERT_NOT_NULL(svc);
		RT_ASSERT_NOT_NULL(pool);
		RT_ASSERT(count > 0, "Pixel count must be positive for shadow rays");

		const int num_lights = lights.light_count;
		const int total_shadow = num_lights * count;

		shadow_mask_.resize(total_shadow > 0 ? total_shadow : count);

		if (!shadows_enabled || num_lights == 0) {
			std::fill(shadow_mask_.begin(), shadow_mask_.end(), static_cast<uint8_t>(1));
			return;
		}

		static constexpr float SHADOW_BIAS = 1e-3f;
		static constexpr float DIR_LIGHT_MAX_DIST = 1000.0f;

		shadow_rays_.resize(total_shadow);

		for (int li = 0; li < num_lights; li++) {
			const LightData &ld = lights.lights[li];
			const int base = li * count;

			if (!ld.cast_shadows) {
				std::fill(shadow_mask_.begin() + base, shadow_mask_.begin() + base + count,
					static_cast<uint8_t>(1));
				pool->dispatch_and_wait(count, 256, [this, base](int start, int end) {
					for (int i = start; i < end; i++) {
						shadow_rays_[base + i] = Ray(Vector3(0, 0, 0), Vector3(0, 1, 0), 0.0f, 0.0f);
					}
				});
				continue;
			}

			pool->dispatch_and_wait(count, 256, [this, &ld, base, count, hits](int start, int end) {
				for (int i = start; i < end; i++) {
					if (hits[i].hit()) {
						Vector3 origin = hits[i].position + hits[i].normal * SHADOW_BIAS;
						Vector3 dir;
						float max_dist;

						if (ld.type == LightData::DIRECTIONAL) {
							dir = ld.direction;
							max_dist = DIR_LIGHT_MAX_DIST;
						} else {
							Vector3 to_light = ld.position - origin;
							float dist = to_light.length();
							if (dist < 1e-6f) {
								shadow_rays_[base + i] = Ray(Vector3(0, 0, 0), Vector3(0, 1, 0), 0.0f, 0.0f);
								continue;
							}
							dir = to_light / dist;
							max_dist = dist;
						}
						shadow_rays_[base + i] = Ray(origin, dir, 0.0f, max_dist);
					} else {
						shadow_rays_[base + i] = Ray(Vector3(0, 0, 0), Vector3(0, 1, 0), 0.0f, 0.0f);
					}
				}
			});
		}

		shadow_hit_flags_.resize(total_shadow);
		RayQuery query = RayQuery::any_hit(shadow_rays_.data(), total_shadow);
		query.coherent = false;
		RayQueryResult result;
		result.hit_flags = reinterpret_cast<bool *>(shadow_hit_flags_.data());

		svc->submit(query, result);

		pool->dispatch_and_wait(total_shadow, 1024, [this](int start, int end) {
			for (int i = start; i < end; i++) {
				shadow_mask_[i] = shadow_hit_flags_[i] ? 0 : 1;
			}
		});
	}
};
