#pragma once
// shade_pass.h — Per-pixel shading functions for AOV channels.
//
// All scene-dependent values (sky colors, ambient energy, tone mapping mode)
// are read from Godot scene nodes by the caller (RayRenderer) and passed
// through the EnvironmentData struct.  No hardcoded scene values here.
//
// Godot-Native Principle: "Read from the scene tree. Never maintain parallel
// state."  This file contains ZERO constexpr sky/ambient/tonemapping values.
//
// Phase 1 shading pipeline:
//   - Smooth vertex normals (interpolated from per-vertex data)
//   - Cook-Torrance PBR (GGX + Fresnel-Schlick + Smith GGX)
//   - Shadow rays (hard shadows from directional sun light)
//   - Sky gradient from WorldEnvironment → ProceduralSkyMaterial
//   - HDR equirectangular panorama from PanoramaSkyMaterial (Phase 1.4)
//   - Tone mapping matching Environment.tonemapper
//
// ADDING A NEW CHANNEL:
//   1. Add an enum value in RayImage::Channel
//   2. Write a shade_xxx() function here
//   3. Call it from RayRenderer::_shade_results()

#include "core/intersection.h"
#include "core/ray.h"
#include "core/triangle_uv.h"
#include "core/triangle_normals.h"
#include "core/triangle_tangents.h"
#include "modules/graphics/ray_image.h"
#include "modules/graphics/texture_sampler.h"
#include "api/scene_shade_data.h"
#include "api/light_data.h"

#include "core/asserts.h"

#include <cmath>
#include <cstdint>
#include <algorithm>

namespace ShadePass {

// ========================================================================
// Constants
// ========================================================================

static constexpr float PI = 3.14159265358979323846f;

// ========================================================================
// EnvironmentData — per-frame scene reads passed from RayRenderer
// ========================================================================
// All values are read from Godot scene nodes each frame (Godot-Native Principle).
// RayRenderer reads DirectionalLight3D, WorldEnvironment, Environment,
// ProceduralSkyMaterial, and Camera3D, then populates this struct.
// Defaults below are last-resort fallbacks (logged with WARN_PRINT_ONCE).

struct EnvironmentData {
	// Sky gradient colors (from ProceduralSkyMaterial)
	float sky_zenith_r = 0.15f, sky_zenith_g = 0.25f, sky_zenith_b = 0.55f;
	float sky_horizon_r = 0.6f, sky_horizon_g = 0.7f, sky_horizon_b = 0.85f;
	float sky_ground_r = 0.15f, sky_ground_g = 0.12f, sky_ground_b = 0.1f;

	// Ambient light (from Environment.ambient_light_energy / color)
	float ambient_energy = 0.15f;
	float ambient_r = 1.0f, ambient_g = 1.0f, ambient_b = 1.0f;

	// Tone mapping mode (from Environment.tonemapper)
	// 0=LINEAR, 1=REINHARDT, 2=FILMIC, 3=ACES, 4=AGX
	int tonemap_mode = 3; // Default ACES — matches Godot 4.x default

	// HDR panorama environment map (from PanoramaSkyMaterial, Phase 1.4).
	// When panorama_data != nullptr, sky_color() samples the equirectangular
	// panorama instead of the analytic ProceduralSky gradient.
	// Layout: RGBAF32 — 4 floats per pixel (R, G, B, A), row-major, top-to-bottom.
	// panorama_energy multiplies the sampled color (from PanoramaSkyMaterial.energy_multiplier).
	const float *panorama_data   = nullptr;
	int panorama_width            = 0;
	int panorama_height           = 0;
	float panorama_energy         = 1.0f;
};

// ========================================================================
// Smooth normal helper
// ========================================================================

/// Retrieve the interpolated smooth normal for a hit. Falls back to
/// the flat face normal from the Intersection if no vertex normals exist.
inline Vector3 get_smooth_normal(const Intersection &hit,
		const SceneShadeData &shade_data) {
	if (shade_data.triangle_normals &&
			hit.prim_id < static_cast<uint32_t>(shade_data.triangle_count)) {
		return shade_data.triangle_normals[hit.prim_id].interpolate(hit.u, hit.v);
	}
	return hit.normal;
}

// ========================================================================
// Normal map perturbation (Phase 1 — Normal Map Support)
// ========================================================================
// Constructs the TBN (Tangent-Bitangent-Normal) matrix from per-vertex tangent
// data, samples the normal map texture, and transforms the tangent-space normal
// to world space.  The result replaces the smooth vertex normal for shading.
//
// Convention: OpenGL normal maps (R=+X, G=+Y, B=+Z).  A flat normal map
// sample is (0.5, 0.5, 1.0) which maps to tangent-space (0, 0, 1) after
// the [0,1]→[-1,1] rescale.
//
// normal_scale controls perturbation strength: 0 = use smooth normal as-is,
// 1.0 = full perturbation.  Values > 1 exaggerate the effect.

inline Vector3 perturb_normal(const Vector3 &smooth_normal,
		const Intersection &hit,
		const SceneShadeData &shade_data,
		const MaterialData &mat) {
	RT_ASSERT(smooth_normal.is_finite(), "perturb_normal: smooth_normal must be finite");
	RT_ASSERT(mat.has_normal_texture, "perturb_normal: must only be called when normal texture exists");

	// Check that tangent data is available for this triangle.
	if (!shade_data.triangle_tangents ||
			hit.prim_id >= static_cast<uint32_t>(shade_data.triangle_count)) {
		return smooth_normal;
	}

	const TriangleTangents &tri_t = shade_data.triangle_tangents[hit.prim_id];
	if (!tri_t.has_tangents()) { return smooth_normal; }

	// Interpolate tangent and bitangent sign at hit point.
	Vector3 T = tri_t.interpolate_tangent(hit.u, hit.v);
	float bsign = tri_t.interpolate_sign(hit.u, hit.v);

	// Re-orthogonalize tangent with respect to the smooth normal (Gram-Schmidt).
	// This ensures the TBN matrix is orthonormal even if tangent data drifted
	// due to mesh deformation or interpolation.
	Vector3 N = smooth_normal;
	T = (T - N * N.dot(T)).normalized();
	Vector3 B = N.cross(T) * bsign;

	// Sample the normal map texture at the hit UV.
	if (!shade_data.triangle_uvs ||
			hit.prim_id >= static_cast<uint32_t>(shade_data.triangle_count)) {
		return smooth_normal;
	}
	const TriangleUV &tri_uv = shade_data.triangle_uvs[hit.prim_id];
	Vector2 uv = tri_uv.interpolate(hit.u, hit.v);
	Color nm_sample = TextureSampler::sample_bilinear(
			mat.normal_texture.ptr(), uv.x, uv.y);

	// Decode from [0,1] to [-1,1] tangent-space normal.
	float ts_x = nm_sample.r * 2.0f - 1.0f;
	float ts_y = nm_sample.g * 2.0f - 1.0f;
	float ts_z = nm_sample.b * 2.0f - 1.0f;

	// Apply normal_scale to the X/Y components (Z stays as-is to preserve normalization target).
	ts_x *= mat.normal_scale;
	ts_y *= mat.normal_scale;

	// Transform from tangent space to world space via TBN matrix.
	// world_normal = T * ts_x + B * ts_y + N * ts_z
	Vector3 perturbed = T * ts_x + B * ts_y + N * ts_z;
	float len_sq = perturbed.length_squared();
	if (len_sq < 1e-8f) { return smooth_normal; }
	return perturbed / std::sqrt(len_sq);
}

// ========================================================================
// Equirectangular panorama sampling (Phase 1.4 — Environment Map)
// ========================================================================
// Maps a 3D direction to a UV coordinate on an equirectangular HDR panorama,
// then performs bilinear interpolation on the raw RGBAF32 pixel data.
//
// Coordinate convention:
//   u = atan2(dir.x, dir.z) / (2π) + 0.5   — longitude [0,1]
//   v = acos(clamp(dir.y, -1, 1)) / π       — latitude  [0,1], 0=top (+Y), 1=bottom (-Y)
//
// WHY bilinear here instead of using TextureSampler?
//   TextureSampler uses Godot's Image::get_pixel(), which has virtual call overhead
//   and per-pixel Variant construction. For the hot sky sampling path (every miss ray
//   AND every ambient IBL lookup), we operate on the raw float pointer directly.
//   At 1920×1080, miss rays can be 30-60% of all rays — this path must be fast.

inline void sample_panorama(const float *data, int width, int height,
		float u, float v, float &r, float &g, float &b) {
	RT_ASSERT(data != nullptr, "Panorama data must not be null");
	RT_ASSERT(width > 0 && height > 0, "Panorama dimensions must be positive");

	// Wrap u to [0,1), clamp v to [0,1].
	u = u - std::floor(u);
	v = std::max(0.0f, std::min(1.0f, v));

	// Pixel coordinates (continuous).
	float fx = u * static_cast<float>(width)  - 0.5f;
	float fy = v * static_cast<float>(height) - 0.5f;

	// Integer coordinates and fractional parts for bilinear interpolation.
	int x0 = static_cast<int>(std::floor(fx));
	int y0 = static_cast<int>(std::floor(fy));
	float sx = fx - static_cast<float>(x0);
	float sy = fy - static_cast<float>(y0);

	// Wrap x horizontally (panorama is cyclic), clamp y vertically.
	auto wrap_x = [width](int x) -> int {
		return ((x % width) + width) % width;
	};
	auto clamp_y = [height](int y) -> int {
		return std::max(0, std::min(y, height - 1));
	};

	int x1 = wrap_x(x0 + 1);
	x0 = wrap_x(x0);
	int y1 = clamp_y(y0 + 1);
	y0 = clamp_y(y0);

	// Fetch four texels (RGBAF32 = 4 floats per pixel, stride = width * 4).
	int stride = width * 4;
	const float *p00 = data + y0 * stride + x0 * 4;
	const float *p10 = data + y0 * stride + x1 * 4;
	const float *p01 = data + y1 * stride + x0 * 4;
	const float *p11 = data + y1 * stride + x1 * 4;

	// Bilinear blend: lerp(lerp(p00, p10, sx), lerp(p01, p11, sx), sy).
	float inv_sx = 1.0f - sx;
	float inv_sy = 1.0f - sy;
	float w00 = inv_sx * inv_sy;
	float w10 = sx * inv_sy;
	float w01 = inv_sx * sy;
	float w11 = sx * sy;

	r = p00[0] * w00 + p10[0] * w10 + p01[0] * w01 + p11[0] * w11;
	g = p00[1] * w00 + p10[1] * w10 + p01[1] * w01 + p11[1] * w11;
	b = p00[2] * w00 + p10[2] * w10 + p01[2] * w01 + p11[2] * w11;
}

/// Direction → equirectangular UV mapping.
inline void direction_to_equirect_uv(const Vector3 &dir, float &u, float &v) {
	// atan2(x, z) gives longitude; acos(y) gives latitude.
	u = std::atan2(dir.x, dir.z) * (0.5f / PI) + 0.5f;
	v = std::acos(std::max(-1.0f, std::min(1.0f, dir.y))) * (1.0f / PI);
}

// ========================================================================
// Sky color for miss rays — panorama or analytic gradient
// ========================================================================

inline void sky_color(const Vector3 &dir, const EnvironmentData &env,
		float &r, float &g, float &b) {
	RT_ASSERT(dir.is_finite(), "sky_color: direction must be finite");
	RT_ASSERT(env.tonemap_mode >= 0 && env.tonemap_mode <= 4, "sky_color: invalid tonemap mode");
	// Phase 1.4: if an HDR panorama is available, sample it instead.
	if (env.panorama_data) {
		float u, v;
		direction_to_equirect_uv(dir, u, v);
		sample_panorama(env.panorama_data, env.panorama_width, env.panorama_height,
				u, v, r, g, b);
		r *= env.panorama_energy;
		g *= env.panorama_energy;
		b *= env.panorama_energy;
		return;
	}

	// Analytic gradient from ProceduralSkyMaterial.
	// Y-axis gradient: zenith → horizon → ground
	float t = dir.y * 0.5f + 0.5f; // [0,1]: 0 = down, 1 = up
	if (t > 0.5f) {
		// Horizon → zenith
		float s = (t - 0.5f) * 2.0f;
		r = env.sky_horizon_r + (env.sky_zenith_r - env.sky_horizon_r) * s;
		g = env.sky_horizon_g + (env.sky_zenith_g - env.sky_horizon_g) * s;
		b = env.sky_horizon_b + (env.sky_zenith_b - env.sky_horizon_b) * s;
	} else {
		// Ground → horizon
		float s = t * 2.0f;
		r = env.sky_ground_r + (env.sky_horizon_r - env.sky_ground_r) * s;
		g = env.sky_ground_g + (env.sky_horizon_g - env.sky_ground_g) * s;
		b = env.sky_ground_b + (env.sky_horizon_b - env.sky_ground_b) * s;
	}
}

// ========================================================================
// PBR building blocks — Cook-Torrance microfacet BRDF
// ========================================================================

/// GGX / Trowbridge-Reitz normal distribution function.
///   D(h) = alpha^2 / (pi * ((n·h)^2 * (alpha^2 - 1) + 1)^2)
inline float distribution_ggx(float n_dot_h, float roughness) {
	RT_ASSERT_FINITE(n_dot_h);
	RT_ASSERT(roughness >= 0.0f && roughness <= 1.0f, "distribution_ggx: roughness must be in [0,1]");
	float a = roughness * roughness;
	float a2 = a * a;
	float denom = n_dot_h * n_dot_h * (a2 - 1.0f) + 1.0f;
	return a2 / (PI * denom * denom + 1e-7f);
}

/// Fresnel-Schlick approximation.
///   F(h,v) = F0 + (1 - F0) * (1 - h·v)^5
inline float fresnel_schlick(float cos_theta, float f0) {
	float t = 1.0f - cos_theta;
	float t2 = t * t;
	return f0 + (1.0f - f0) * (t2 * t2 * t);
}

/// Smith's geometry function (GGX height-correlated).
///   G1(v) = 2 * n·v / (n·v + sqrt(alpha^2 + (1 - alpha^2) * (n·v)^2))
inline float geometry_smith_ggx(float n_dot_v, float n_dot_l, float roughness) {
	RT_ASSERT_FINITE(n_dot_v);
	RT_ASSERT(roughness >= 0.0f && roughness <= 1.0f, "geometry_smith_ggx: roughness must be in [0,1]");
	float a = roughness * roughness;
	float a2 = a * a;
	auto g1 = [a2](float ndx) -> float {
		return 2.0f * ndx / (ndx + std::sqrt(a2 + (1.0f - a2) * ndx * ndx) + 1e-7f);
	};
	return g1(n_dot_v) * g1(n_dot_l);
}

// ========================================================================
// Shadow context — carries per-pixel, per-light shadow results
// ========================================================================
// The shadow_mask array is populated by the renderer before shade_pass runs.
// Layout: shadow_mask[light_index * count + pixel_index].
// 0 = in shadow, 1 = lit.  If null, all pixels are treated as lit.

struct ShadowContext {
	const uint8_t *shadow_mask = nullptr;
	int count = 0;       // Number of pixels per light
	int light_count = 0; // Number of lights

	/// Check if pixel idx is lit by light li.
	bool is_lit(int idx, int li = 0) const {
		if (!shadow_mask) { return true; }
		if (li < 0 || li >= light_count) { return true; }
		return shadow_mask[li * count + idx] != 0;
	}
};

// ========================================================================
// Normal visualization — map world-space smooth normal to RGB [0,1]
// ========================================================================

inline void shade_normal(RayImage &fb, int idx, const Intersection &hit,
		const SceneShadeData &shade_data) {
	RT_ASSERT_BOUNDS(idx, fb.pixel_count());

	if (!hit.hit()) {
		fb.write_pixel(RayImage::NORMAL, idx, 0.0f, 0.0f, 0.0f, 1.0f);
		return;
	}
	Vector3 n = get_smooth_normal(hit, shade_data);

	// Apply normal map perturbation if available (matches shade_material behavior).
	if (shade_data.material_ids &&
			hit.prim_id < static_cast<uint32_t>(shade_data.triangle_count)) {
		uint32_t nmap_mat_id = shade_data.material_ids[hit.prim_id];
		if (nmap_mat_id < static_cast<uint32_t>(shade_data.material_count)) {
			const MaterialData &nmap_mat = shade_data.materials[nmap_mat_id];
			if (nmap_mat.has_normal_texture) {
				n = perturb_normal(n, hit, shade_data, nmap_mat);
			}
		}
	}

	float r = n.x * 0.5f + 0.5f;
	float g = n.y * 0.5f + 0.5f;
	float b = n.z * 0.5f + 0.5f;
	fb.write_pixel(RayImage::NORMAL, idx, r, g, b, 1.0f);
}

// ========================================================================
// Depth visualization — linear depth normalized to [0,1] range
// ========================================================================

inline void shade_depth(RayImage &fb, int idx, const Intersection &hit,
		float inv_depth_range) {
	RT_ASSERT_BOUNDS(idx, fb.pixel_count());
	RT_ASSERT_FINITE(inv_depth_range);
	if (!hit.hit()) {
		fb.write_pixel(RayImage::DEPTH, idx, 0.0f, 0.0f, 0.0f, 1.0f);
		return;
	}
	float d = 1.0f - hit.t * inv_depth_range;
	d = (d < 0.0f) ? 0.0f : ((d > 1.0f) ? 1.0f : d);
	fb.write_pixel(RayImage::DEPTH, idx, d, d, d, 1.0f);
}

// ========================================================================
// Barycentric visualization — u, v, (1-u-v) as RGB
// ========================================================================

inline void shade_barycentric(RayImage &fb, int idx, const Intersection &hit) {
	RT_ASSERT_BOUNDS(idx, fb.pixel_count());
	RT_ASSERT(fb.pixel_count() > 0, "Framebuffer must have at least one pixel");
	if (!hit.hit()) {
		fb.write_pixel(RayImage::BARYCENTRIC, idx, 0.0f, 0.0f, 0.0f, 1.0f);
		return;
	}
	float w = 1.0f - hit.u - hit.v;
	fb.write_pixel(RayImage::BARYCENTRIC, idx, hit.u, hit.v, w, 1.0f);
}

// ========================================================================
// Tone mapping operators — matches Godot Environment.tonemapper enum
// ========================================================================
// 0=LINEAR, 1=REINHARDT, 2=FILMIC, 3=ACES, 4=AGX
// These must produce visually similar results to Godot's built-in operators
// so the CPU raytracer output matches the rasterized viewport.

inline float tonemap_reinhard(float c) { return c / (c + 1.0f); }

/// Hable/Uncharted 2 filmic curve (John Hable, GDC 2010).
inline float _hable_partial(float x) {
	constexpr float A = 0.15f, B = 0.50f, C = 0.10f, D = 0.20f, E = 0.02f, F = 0.30f;
	return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}
inline float tonemap_filmic(float c) {
	constexpr float W = 11.2f; // linear white point
	return _hable_partial(c) / _hable_partial(W);
}

/// ACES fitted approximation (Stephen Hill, "A Closer Look…", 2016).
inline float tonemap_aces(float c) {
	constexpr float a = 2.51f, b = 0.03f, cc = 2.43f, d = 0.59f, e = 0.14f;
	float mapped = (c * (a * c + b)) / (c * (cc * c + d) + e);
	return (mapped < 0.0f) ? 0.0f : ((mapped > 1.0f) ? 1.0f : mapped);
}

/// AgX base contrast curve (simplified S-curve approximation).
inline float tonemap_agx(float c) {
	RT_ASSERT_FINITE(c);
	// Attempt at AgX tone-mapping; a power-based sigmoid.
	float x = std::max(c, 0.0f);
	float x2 = x * x;
	float mapped = x2 / (x2 + 0.09f * x + 0.0009f);
	RT_ASSERT(mapped >= 0.0f && mapped <= 1.0f + 1e-6f, "tonemap_agx: output out of [0,1] range");
	return (mapped > 1.0f) ? 1.0f : mapped;
}

/// Apply tone mapping per the mode read from Environment.tonemapper.
inline void tonemap_rgb(float &r, float &g, float &b, int mode) {
	RT_ASSERT(mode >= 0 && mode <= 4, "Tone map mode must be 0-4 (LINEAR..AGX)");
	RT_ASSERT(std::isfinite(r) && std::isfinite(g) && std::isfinite(b), "Tone map input must be finite");

	switch (mode) {
		case 0: break; // LINEAR — no-op
		case 1: r = tonemap_reinhard(r); g = tonemap_reinhard(g); b = tonemap_reinhard(b); break;
		case 2: r = tonemap_filmic(r); g = tonemap_filmic(g); b = tonemap_filmic(b); break;
		case 3: r = tonemap_aces(r); g = tonemap_aces(g); b = tonemap_aces(b); break;
		case 4: r = tonemap_agx(r); g = tonemap_agx(g); b = tonemap_agx(b); break;
		default: r = tonemap_aces(r); g = tonemap_aces(g); b = tonemap_aces(b); break;
	}
}

// ========================================================================
// Light attenuation helpers (Godot-matching)
// ========================================================================

/// Godot's distance attenuation for OmniLight3D / SpotLight3D:
///   atten = pow(max(1 - (distance / range)^2, 0), attenuation_exp)
/// The squared distance ratio gives a smoother falloff than linear at the edge.
inline float compute_distance_attenuation(float distance, float range, float attenuation_exp) {
	float ratio = distance / range;
	float base = std::max(1.0f - ratio * ratio, 0.0f);
	return std::pow(base, attenuation_exp);
}

/// Spot light cone attenuation:
///   cos_angle = dot(-light_dir_to_point, spot_forward)
///   If cos_angle < cos(spot_angle): outside cone → 0
///   Otherwise: pow((cos_angle - cos(spot_angle)) / (1 - cos(spot_angle)), spot_attenuation)
inline float compute_spot_attenuation(const Vector3 &light_to_point_dir,
		const Vector3 &spot_forward, float spot_angle_rad, float spot_attenuation_exp) {
	float cos_outer = std::cos(spot_angle_rad);
	float cos_angle = (-light_to_point_dir).dot(spot_forward);
	if (cos_angle <= cos_outer) { return 0.0f; }
	float t = (cos_angle - cos_outer) / (1.0f - cos_outer);
	return std::pow(std::max(t, 0.0f), spot_attenuation_exp);
}

// ========================================================================
// PBR material shading — Cook-Torrance + multi-light + shadow + sky
// ========================================================================

inline void shade_material(RayImage &fb, int idx, const Intersection &hit,
		const Ray &ray, const Vector3 &sun_dir, const Vector3 &sun_color,
		const SceneShadeData &shade_data, const ShadowContext &shadows,
		const EnvironmentData &env, const SceneLightData &lights) {
	RT_ASSERT_BOUNDS(idx, fb.pixel_count());
	RT_ASSERT(sun_dir.is_finite(), "shade_material: sun direction must be finite");
	if (!hit.hit()) {
		// Miss → sky color (read from WorldEnvironment → ProceduralSkyMaterial)
		float r, g, b;
		sky_color(ray.direction, env, r, g, b);
		fb.write_pixel(RayImage::COLOR, idx, r, g, b, 1.0f);
		return;
	}

	// ---- Smooth shading normal ----
	Vector3 n = get_smooth_normal(hit, shade_data);

	// ---- Normal map perturbation (Phase 1) ----
	// If the material has a normal map and tangent data is available,
	// perturb the smooth normal using the TBN matrix + normal map sample.
	if (shade_data.material_ids &&
			hit.prim_id < static_cast<uint32_t>(shade_data.triangle_count)) {
		uint32_t nmap_mat_id = shade_data.material_ids[hit.prim_id];
		if (nmap_mat_id < static_cast<uint32_t>(shade_data.material_count)) {
			const MaterialData &nmap_mat = shade_data.materials[nmap_mat_id];
			if (nmap_mat.has_normal_texture) {
				n = perturb_normal(n, hit, shade_data, nmap_mat);
			}
		}
	}

	// ---- View direction ----
	Vector3 view_dir = (-ray.direction).normalized();
	float n_dot_v = std::max(n.dot(view_dir), 0.001f);

	// ---- Material lookup ----
	float ar = 0.75f, ag = 0.75f, ab = 0.75f;
	float metallic = 0.0f;
	float roughness = 0.5f;
	float specular_scale = 0.5f;
	float emit_r = 0.0f, emit_g = 0.0f, emit_b = 0.0f;

	if (shade_data.material_ids &&
			hit.prim_id < static_cast<uint32_t>(shade_data.triangle_count)) {
		uint32_t mat_id = shade_data.material_ids[hit.prim_id];
		if (mat_id < static_cast<uint32_t>(shade_data.material_count)) {
			const MaterialData &mat = shade_data.materials[mat_id];
			ar = mat.albedo.r;
			ag = mat.albedo.g;
			ab = mat.albedo.b;
			metallic = mat.metallic;
			roughness = std::max(mat.roughness, 0.04f); // clamp to avoid singularity
			specular_scale = mat.specular;

			// Albedo texture sampling
			if (mat.has_albedo_texture && shade_data.triangle_uvs) {
				const TriangleUV &tri_uv = shade_data.triangle_uvs[hit.prim_id];
				Vector2 uv = tri_uv.interpolate(hit.u, hit.v);
				Color tex = TextureSampler::sample_bilinear(
						mat.albedo_texture.ptr(), uv.x, uv.y);
				ar *= tex.r;
				ag *= tex.g;
				ab *= tex.b;
			}

			// Emission
			if (mat.emission_energy > 0.0f) {
				emit_r = mat.emission.r * mat.emission_energy;
				emit_g = mat.emission.g * mat.emission_energy;
				emit_b = mat.emission.b * mat.emission_energy;
			}
		}
	}

	// ---- F0: reflectance at normal incidence ----
	// For dielectrics: 0.04 * specular_scale.  For metals: albedo color.
	float dielectric_f0 = 0.04f * specular_scale * 2.0f;
	float f0_r = dielectric_f0 * (1.0f - metallic) + ar * metallic;
	float f0_g = dielectric_f0 * (1.0f - metallic) + ag * metallic;
	float f0_b = dielectric_f0 * (1.0f - metallic) + ab * metallic;

	// ---- Diffuse albedo (metals have no diffuse) ----
	float diff_r = ar * (1.0f - metallic);
	float diff_g = ag * (1.0f - metallic);
	float diff_b = ab * (1.0f - metallic);

	// ---- Multi-light direct illumination (Cook-Torrance) ----
	float out_r = 0.0f, out_g = 0.0f, out_b = 0.0f;

	for (int li = 0; li < lights.light_count; li++) {
		const LightData &ld = lights.lights[li];

		// Compute light direction and attenuation based on light type.
		Vector3 light_dir;   // Normalized direction FROM surface TOWARD light
		float atten = 1.0f;  // Combined distance + spot attenuation

		if (ld.type == LightData::DIRECTIONAL) {
			light_dir = ld.direction;
		} else {
			// Point or spot: compute direction and distance attenuation.
			Vector3 hit_pos = hit.position;
			Vector3 to_light = ld.position - hit_pos;
			float dist = to_light.length();
			if (dist < 1e-6f || dist > ld.range) { continue; }
			light_dir = to_light / dist;
			atten = compute_distance_attenuation(dist, ld.range, ld.attenuation);

			// Spot cone attenuation (in addition to distance).
			if (ld.type == LightData::SPOT) {
				float spot_atten = compute_spot_attenuation(
					-light_dir, ld.direction, ld.spot_angle, ld.spot_angle_attenuation);
				atten *= spot_atten;
			}
		}

		if (atten < 1e-6f) { continue; }

		float n_dot_l = n.dot(light_dir);
		if (n_dot_l <= 0.0f) { continue; }
		if (!shadows.is_lit(idx, li)) { continue; }

		Vector3 h = (view_dir + light_dir).normalized();
		float n_dot_h = std::max(n.dot(h), 0.0f);
		float v_dot_h = std::max(view_dir.dot(h), 0.0f);

		// Cook-Torrance specular BRDF
		float d_term = distribution_ggx(n_dot_h, roughness);
		float g_term = geometry_smith_ggx(n_dot_v, n_dot_l, roughness);
		float fr = fresnel_schlick(v_dot_h, f0_r);
		float fg = fresnel_schlick(v_dot_h, f0_g);
		float fb_val = fresnel_schlick(v_dot_h, f0_b);

		float spec_denom = 4.0f * n_dot_v * n_dot_l + 1e-7f;
		float spec_scale = d_term * g_term / spec_denom;

		// Diffuse: Lambert BRDF = albedo / PI.  The rendering equation
		// cosine factor (n_dot_l) is applied once in the final accumulation
		// below — do NOT include it here, or it gets squared.
		float diff_scale = 1.0f / PI;

		float lr = ld.color.x * atten;
		float lg = ld.color.y * atten;
		float lb = ld.color.z * atten;

		out_r += (diff_r * (1.0f - fr) * diff_scale + fr * spec_scale) * lr * n_dot_l;
		out_g += (diff_g * (1.0f - fg) * diff_scale + fg * spec_scale) * lg * n_dot_l;
		out_b += (diff_b * (1.0f - fb_val) * diff_scale + fb_val * spec_scale) * lb * n_dot_l;
	}

	// ---- Ambient / indirect approximation ----
	// When an HDR panorama is available, sample it in the normal direction for
	// a basic diffuse IBL approximation. Otherwise fall back to the hemisphere
	// sky/ground gradient from ProceduralSkyMaterial.
	// Full importance-sampled IBL (prefiltered specular + irradiance convolution)
	// is deferred to Phase 2+. This single-sample approach captures the dominant
	// ambient color direction without the cost of a cubemap convolution.
	float amb_r, amb_g, amb_b;
	if (env.panorama_data) {
		float u, v;
		direction_to_equirect_uv(n, u, v);
		sample_panorama(env.panorama_data, env.panorama_width, env.panorama_height,
				u, v, amb_r, amb_g, amb_b);
		amb_r *= env.panorama_energy;
		amb_g *= env.panorama_energy;
		amb_b *= env.panorama_energy;
	} else {
		// Hemisphere ambient from sky/ground colors (read from Environment).
		// Up-facing surfaces get sky color, down-facing get ground bounce.
		float sky_blend = n.y * 0.5f + 0.5f; // [0,1]: 0 = down, 1 = up
		amb_r = env.sky_ground_r + (env.sky_zenith_r - env.sky_ground_r) * sky_blend;
		amb_g = env.sky_ground_g + (env.sky_zenith_g - env.sky_ground_g) * sky_blend;
		amb_b = env.sky_ground_b + (env.sky_zenith_b - env.sky_ground_b) * sky_blend;
	}
	// Tint by ambient light color and scale by ambient energy (from Environment).
	float ambient_strength = env.ambient_energy;

	out_r += diff_r * amb_r * env.ambient_r * ambient_strength;
	out_g += diff_g * amb_g * env.ambient_g * ambient_strength;
	out_b += diff_b * amb_b * env.ambient_b * ambient_strength;

	// ---- Emission ----
	out_r += emit_r;
	out_g += emit_g;
	out_b += emit_b;

	// ---- Tone mapping (matches Environment.tonemapper setting) ----
	tonemap_rgb(out_r, out_g, out_b, env.tonemap_mode);

	// ---- Gamma correction (linear → sRGB approx) ----
	auto to_srgb = [](float c) -> float {
		return std::pow(std::max(c, 0.0f), 1.0f / 2.2f);
	};
	fb.write_pixel(RayImage::COLOR, idx, to_srgb(out_r), to_srgb(out_g), to_srgb(out_b), 1.0f);
}

// ========================================================================
// Albedo visualization — pure material color, no lighting
// ========================================================================

inline void shade_albedo(RayImage &fb, int idx, const Intersection &hit,
		const SceneShadeData &shade_data) {
	RT_ASSERT_BOUNDS(idx, fb.pixel_count());
	RT_ASSERT(fb.pixel_count() > 0, "Framebuffer must have at least one pixel");
	if (!hit.hit()) {
		fb.write_pixel(RayImage::ALBEDO, idx, 0.0f, 0.0f, 0.0f, 1.0f);
		return;
	}
	float r = 0.75f, g = 0.75f, b = 0.75f;
	if (shade_data.material_ids &&
			hit.prim_id < static_cast<uint32_t>(shade_data.triangle_count)) {
		uint32_t mat_id = shade_data.material_ids[hit.prim_id];
		if (mat_id < static_cast<uint32_t>(shade_data.material_count)) {
			const MaterialData &mat = shade_data.materials[mat_id];
			r = mat.albedo.r;
			g = mat.albedo.g;
			b = mat.albedo.b;
			if (mat.has_albedo_texture && shade_data.triangle_uvs) {
				const TriangleUV &tri_uv = shade_data.triangle_uvs[hit.prim_id];
				Vector2 uv = tri_uv.interpolate(hit.u, hit.v);
				Color tex = TextureSampler::sample_bilinear(
						mat.albedo_texture.ptr(), uv.x, uv.y);
				r *= tex.r;
				g *= tex.g;
				b *= tex.b;
			}
		}
	}
	fb.write_pixel(RayImage::ALBEDO, idx, r, g, b, 1.0f);
}

// ========================================================================
// World-space position — modulo mapping to [0,1] for visualization
// ========================================================================

inline void shade_position(RayImage &fb, int idx, const Intersection &hit,
		float inv_range) {
	RT_ASSERT_BOUNDS(idx, fb.pixel_count());
	RT_ASSERT_FINITE(inv_range);
	if (!hit.hit()) {
		fb.write_pixel(RayImage::POSITION, idx, 0.0f, 0.0f, 0.0f, 1.0f);
		return;
	}
	auto wrap = [inv_range](float v) -> float {
		float f = v * inv_range;
		f = f - std::floor(f);
		return f;
	};
	fb.write_pixel(RayImage::POSITION, idx,
		wrap(hit.position.x), wrap(hit.position.y), wrap(hit.position.z), 1.0f);
}

// ========================================================================
// Primitive ID — hash to a unique color per triangle
// ========================================================================

inline void shade_prim_id(RayImage &fb, int idx, const Intersection &hit) {
	RT_ASSERT_BOUNDS(idx, fb.pixel_count());
	RT_ASSERT(fb.pixel_count() > 0, "Framebuffer must have at least one pixel");

	if (!hit.hit()) {
		fb.write_pixel(RayImage::PRIM_ID, idx, 0.0f, 0.0f, 0.0f, 1.0f);
		return;
	}
	uint32_t h = hit.prim_id;
	h = ((h >> 16) ^ h) * 0x45d9f3bU;
	h = ((h >> 16) ^ h) * 0x45d9f3bU;
	h = (h >> 16) ^ h;

	float r = static_cast<float>((h >>  0) & 0xFF) / 255.0f;
	float g = static_cast<float>((h >>  8) & 0xFF) / 255.0f;
	float b = static_cast<float>((h >> 16) & 0xFF) / 255.0f;
	fb.write_pixel(RayImage::PRIM_ID, idx, r, g, b, 1.0f);
}

// ========================================================================
// Hit mask — white if hit, black background
// ========================================================================

inline void shade_hit_mask(RayImage &fb, int idx, const Intersection &hit) {
	float v = hit.hit() ? 1.0f : 0.0f;
	fb.write_pixel(RayImage::HIT_MASK, idx, v, v, v, 1.0f);
}

// ========================================================================
// Wireframe — highlight triangle edges using barycentric proximity
// ========================================================================

inline void shade_wireframe(RayImage &fb, int idx, const Intersection &hit) {
	RT_ASSERT_BOUNDS(idx, fb.pixel_count());
	RT_ASSERT(fb.pixel_count() > 0, "Framebuffer must have at least one pixel");

	if (!hit.hit()) {
		fb.write_pixel(RayImage::WIREFRAME, idx, 0.0f, 0.0f, 0.0f, 1.0f);
		return;
	}
	float w0 = 1.0f - hit.u - hit.v;
	float w1 = hit.u;
	float w2 = hit.v;
	float d = std::min({w0, w1, w2});
	float lo = 0.01f, hi = 0.03f;
	float t = (d - lo) / (hi - lo);
	t = (t < 0.0f) ? 0.0f : ((t > 1.0f) ? 1.0f : t);
	float edge = 1.0f - t * t * (3.0f - 2.0f * t);
	float bg = 0.08f;
	float v = bg + edge * (1.0f - bg);
	fb.write_pixel(RayImage::WIREFRAME, idx, v, v, v, 1.0f);
}

// ========================================================================
// UV visualization — texture coordinates as Red/Green color
// ========================================================================

inline void shade_uv(RayImage &fb, int idx, const Intersection &hit,
		const SceneShadeData &shade_data) {
	RT_ASSERT_BOUNDS(idx, fb.pixel_count());
	RT_ASSERT(fb.pixel_count() > 0, "Framebuffer must have at least one pixel");
	if (!hit.hit()) {
		fb.write_pixel(RayImage::UV, idx, 0.0f, 0.0f, 0.0f, 1.0f);
		return;
	}
	float r = 0.5f, g = 0.5f;
	if (shade_data.triangle_uvs &&
			hit.prim_id < static_cast<uint32_t>(shade_data.triangle_count)) {
		const TriangleUV &tri_uv = shade_data.triangle_uvs[hit.prim_id];
		Vector2 uv = tri_uv.interpolate(hit.u, hit.v);
		r = uv.x - std::floor(uv.x);
		g = uv.y - std::floor(uv.y);
	}
	fb.write_pixel(RayImage::UV, idx, r, g, 0.0f, 1.0f);
}

// ========================================================================
// Fresnel / facing ratio — uses smooth normals
// ========================================================================

inline void shade_fresnel(RayImage &fb, int idx, const Intersection &hit,
		const Ray &ray, const SceneShadeData &shade_data) {
	RT_ASSERT_BOUNDS(idx, fb.pixel_count());
	RT_ASSERT_VALID_RAY(ray);
	if (!hit.hit()) {
		fb.write_pixel(RayImage::FRESNEL, idx, 0.0f, 0.0f, 0.0f, 1.0f);
		return;
	}
	Vector3 n = get_smooth_normal(hit, shade_data);
	Vector3 view_dir = -ray.direction;
	float n_dot_v = n.dot(view_dir);
	n_dot_v = (n_dot_v < 0.0f) ? 0.0f : ((n_dot_v > 1.0f) ? 1.0f : n_dot_v);
	float r = n_dot_v;
	float g = n_dot_v;
	float b = 0.3f + n_dot_v * 0.7f;
	fb.write_pixel(RayImage::FRESNEL, idx, r, g, b, 1.0f);
}

// ========================================================================
// Shade ALL channels for a single pixel.
// ========================================================================

inline void shade_all(RayImage &fb, int idx, const Intersection &hit,
		const Ray &ray, const Vector3 &sun_dir, const Vector3 &sun_color,
		float inv_depth_range, float inv_pos_range,
		const SceneShadeData &shade_data, const ShadowContext &shadows,
		const EnvironmentData &env, const SceneLightData &lights) {
	shade_material(fb, idx, hit, ray, sun_dir, sun_color, shade_data, shadows, env, lights);
	shade_normal(fb, idx, hit, shade_data);
	shade_depth(fb, idx, hit, inv_depth_range);
	shade_barycentric(fb, idx, hit);
	shade_position(fb, idx, hit, inv_pos_range);
	shade_prim_id(fb, idx, hit);
	shade_hit_mask(fb, idx, hit);
	shade_albedo(fb, idx, hit, shade_data);
	shade_wireframe(fb, idx, hit);
	shade_uv(fb, idx, hit, shade_data);
	shade_fresnel(fb, idx, hit, ray, shade_data);
}

// ========================================================================
// Shade a SINGLE channel for one pixel.
// ========================================================================

inline void shade_channel(RayImage &fb, int idx, const Intersection &hit,
		const Ray &ray, const Vector3 &sun_dir, const Vector3 &sun_color,
		float inv_depth_range, float inv_pos_range,
		const SceneShadeData &shade_data, const ShadowContext &shadows,
		const EnvironmentData &env,
		const SceneLightData &lights,
		RayImage::Channel channel) {
	switch (channel) {
		case RayImage::COLOR:       shade_material(fb, idx, hit, ray, sun_dir, sun_color, shade_data, shadows, env, lights); break;
		case RayImage::NORMAL:      shade_normal(fb, idx, hit, shade_data); break;
		case RayImage::DEPTH:       shade_depth(fb, idx, hit, inv_depth_range); break;
		case RayImage::BARYCENTRIC: shade_barycentric(fb, idx, hit); break;
		case RayImage::POSITION:    shade_position(fb, idx, hit, inv_pos_range); break;
		case RayImage::PRIM_ID:     shade_prim_id(fb, idx, hit); break;
		case RayImage::HIT_MASK:    shade_hit_mask(fb, idx, hit); break;
		case RayImage::ALBEDO:      shade_albedo(fb, idx, hit, shade_data); break;
		case RayImage::WIREFRAME:   shade_wireframe(fb, idx, hit); break;
		case RayImage::UV:          shade_uv(fb, idx, hit, shade_data); break;
		case RayImage::FRESNEL:     shade_fresnel(fb, idx, hit, ray, shade_data); break;
		default:                    shade_material(fb, idx, hit, ray, sun_dir, sun_color, shade_data, shadows, env, lights); break;
	}
}

} // namespace ShadePass
