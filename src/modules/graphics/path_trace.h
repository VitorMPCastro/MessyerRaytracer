#pragma once
// path_trace.h — CPU path tracing: surface extraction, NEE, bounce sampling.
//
// WHAT:  Pure functions for multi-bounce path tracing on the CPU.
//        Extracts surface properties from hits, computes direct illumination
//        (Next Event Estimation), and samples bounce directions with
//        cosine-weighted hemisphere (diffuse) or GGX importance sampling
//        (specular).
//
// WHY:   Separates the iterative path tracing logic from the single-bounce
//        shade_pass.h pipeline.  Same Cook-Torrance BRDF functions are reused
//        (distribution_ggx, geometry_smith_ggx, fresnel_schlick) but structured
//        for throughput accumulation across bounces instead of writing to a
//        framebuffer.
//
// HOW:   All functions are Godot-header-free at the API level.  They receive
//        scene data by const reference (SurfaceInfo, EnvironmentData,
//        SceneLightData) and return computed values.  No allocations, no
//        virtual calls.
//
//        Sampling strategy:
//          Diffuse  — cosine-weighted hemisphere (Malley's method, PDF = cos/π)
//          Specular — GGX half-vector importance sampling (PDF = D·NdotH/(4·VdotH))
//          Lobe selection — probabilistic based on metallic + roughness
//          Russian roulette — after bounce 2, survival prob = max(throughput)
//
//        BRDF/PDF cancellation (pre-computed throughput weights):
//          Specular weight = F * G * VdotH / (NdotV * NdotH * spec_prob)
//          Diffuse  weight = albedo * (1-metallic) / (1 - spec_prob)
//
// REFERENCES:
//   Walter et al., "Microfacet Models for Refraction" (EGSR 2007) — GGX NDF
//   Duff et al., "Building an ONB, Revisited" (JCGT 2017) — branchless ONB

#include "modules/graphics/shade_pass.h"
#include "modules/graphics/path_state.h"
#include "core/asserts.h"

#include <cmath>
#include <algorithm>

namespace PathTrace {

static constexpr float PT_PI      = 3.14159265358979323846f;
static constexpr float PT_INV_PI  = 1.0f / PT_PI;
static constexpr float PT_EPSILON = 1e-7f;
static constexpr float PT_SHADOW_BIAS = 1e-3f;

// ========================================================================
// Type aliases — shared material extraction and lighting live in ShadePass
// ========================================================================
// SurfaceInfo, extract_surface(), and cook_torrance_multi_light() are defined
// in shade_pass.h and shared between the single-sample pipeline and the path
// tracer.  These aliases keep PathTrace call sites concise.

using SurfaceInfo = ShadePass::SurfaceInfo;

inline SurfaceInfo extract_surface(const Intersection &hit, const Ray &ray,
		const SceneShadeData &shade_data) {
	return ShadePass::extract_surface(hit, ray, shade_data);
}

/// Path-tracer NEE wrapper around ShadePass::cook_torrance_multi_light().
/// Same interface — exists so call sites read naturally as PathTrace::compute_direct_light().
inline void compute_direct_light(const SurfaceInfo &surf,
		const ShadePass::ShadowContext &shadows, int pixel_idx,
		const SceneLightData &lights,
		float &out_r, float &out_g, float &out_b) {
	ShadePass::cook_torrance_multi_light(surf, shadows, pixel_idx, lights, out_r, out_g, out_b);
}

// ========================================================================
// Orthonormal basis construction
// ========================================================================
// Duff et al., "Building an Orthonormal Basis, Revisited" (JCGT 2017).
// Branchless, singularity-free ONB from a single unit vector.
// Uses copysign trick: sign(n.z) ensures |sign + n.z| >= 1 for all normals,
// avoiding division by near-zero.

inline void construct_onb(const Vector3 &n, Vector3 &tangent, Vector3 &bitangent) {
	RT_ASSERT(n.is_finite(), "construct_onb: normal must be finite");
	RT_ASSERT(std::fabs(n.length_squared() - 1.0f) < 0.01f,
		"construct_onb: normal must be approximately unit length");

	const float sign = std::copysign(1.0f, n.z);
	const float a = -1.0f / (sign + n.z);
	const float b = n.x * n.y * a;
	tangent   = Vector3(1.0f + sign * n.x * n.x * a, sign * b, -sign * n.x);
	bitangent = Vector3(b, sign + n.y * n.y * a, -n.y);
}

// ========================================================================
// Cosine-weighted hemisphere sampling (Malley's method)
// ========================================================================
// Samples a direction with PDF = cos(θ) / π.  Optimal for Lambertian BRDF
// because BRDF * cos / PDF = albedo (constant), minimizing variance.
//
// Method: uniformly sample a disk (r = sqrt(u1)), project up to hemisphere.
// The z coordinate (cos θ) is sqrt(1 - u1).

inline Vector3 cosine_hemisphere_sample(const Vector3 &normal, PCG32 &rng) {
	RT_ASSERT(normal.is_finite(), "cosine_hemisphere_sample: normal must be finite");
	RT_ASSERT(std::fabs(normal.length_squared() - 1.0f) < 0.05f,
		"cosine_hemisphere_sample: normal must be approximately unit length");

	float u1 = rng.next_float();
	float u2 = rng.next_float();

	float r   = std::sqrt(u1);
	float phi = 2.0f * PT_PI * u2;

	float x = r * std::cos(phi);
	float y = r * std::sin(phi);
	float z = std::sqrt(std::max(0.0f, 1.0f - u1));

	Vector3 t, b;
	construct_onb(normal, t, b);

	return (t * x + b * y + normal * z).normalized();
}

// ========================================================================
// GGX importance sampling (Trowbridge-Reitz NDF)
// ========================================================================
// Samples the half-vector from the GGX distribution inverse CDF.
// cos²θ_h = (1 - u1) / (1 + (α² - 1) * u1)
// where α = roughness².
//
// The caller reflects the view direction around h to get the bounce dir:
//   ω_o = 2 * dot(V, h) * h - V

inline Vector3 ggx_sample_half(const Vector3 &normal, float roughness, PCG32 &rng) {
	RT_ASSERT(normal.is_finite(), "ggx_sample_half: normal must be finite");
	RT_ASSERT(roughness >= 0.0f && roughness <= 1.0f,
		"ggx_sample_half: roughness must be in [0,1]");

	float a  = roughness * roughness;
	float a2 = a * a;
	float u1 = rng.next_float();
	float u2 = rng.next_float();

	// Inverse CDF: sample cos(theta_h) from GGX distribution.
	float cos_theta = std::sqrt((1.0f - u1) / (1.0f + (a2 - 1.0f) * u1 + PT_EPSILON));
	float sin_theta = std::sqrt(std::max(0.0f, 1.0f - cos_theta * cos_theta));
	float phi = 2.0f * PT_PI * u2;

	float lx = sin_theta * std::cos(phi);
	float ly = sin_theta * std::sin(phi);
	float lz = cos_theta;

	Vector3 t, b;
	construct_onb(normal, t, b);

	return (t * lx + b * ly + normal * lz).normalized();
}

// ========================================================================
// BounceResult — output of bounce sampling
// ========================================================================
// Contains the new ray direction and the pre-computed throughput weight
// (BRDF * cos / pdf, with BRDF/PDF cancellations already applied).
// The caller multiplies path throughput by (weight_r, weight_g, weight_b).

struct BounceResult {
	Vector3 direction;                   // New ray direction (world space)
	float weight_r, weight_g, weight_b;  // = BRDF * cos(θ) / pdf — throughput multiplier
	bool valid;                          // False if sampling failed (below surface)
};

// ========================================================================
// sample_bounce — choose and sample a bounce direction
// ========================================================================
// Probabilistic lobe selection based on metallic + roughness:
//   spec_prob = metallic + (1-metallic) * (1-roughness) * 0.5
//   Clamped to [0.05, 0.95] to avoid zero-probability paths.
//
// For specular: GGX half-vector sampling, reflect V around h.
//   Weight = F * G * VdotH / (NdotV * NdotH * spec_prob)
//   (D term cancels between BRDF numerator and PDF denominator.)
//
// For diffuse: cosine-weighted hemisphere.
//   Weight = albedo * (1 - metallic) / (1 - spec_prob)
//   (cos/π in BRDF cancels with cos/π in PDF.)

inline BounceResult sample_bounce(const SurfaceInfo &surf, PCG32 &rng) {
	RT_ASSERT(surf.normal.is_finite(), "sample_bounce: surface normal must be finite");
	RT_ASSERT(surf.roughness >= 0.04f, "sample_bounce: roughness must be >= 0.04 (clamped)");

	BounceResult result;
	result.valid = false;
	result.weight_r = result.weight_g = result.weight_b = 0.0f;

	// ---- Lobe selection probability ----
	// Higher metallic → more specular.  Lower roughness → more specular.
	// This is a simple heuristic; full MIS (power heuristic over both lobes)
	// would reduce variance but adds complexity.  Good enough for real-time.
	float spec_prob = surf.metallic + (1.0f - surf.metallic) * (1.0f - surf.roughness) * 0.5f;
	spec_prob = std::max(std::min(spec_prob, 0.95f), 0.05f);

	bool do_specular = rng.next_float() < spec_prob;

	if (do_specular) {
		// ---- GGX importance sampling (specular lobe) ----
		Vector3 h = ggx_sample_half(surf.normal, surf.roughness, rng);
		float v_dot_h = std::max(surf.view_dir.dot(h), 0.0f);

		// Reflect view direction around half vector.
		result.direction = (h * (2.0f * v_dot_h) - surf.view_dir).normalized();

		float n_dot_l = surf.normal.dot(result.direction);
		if (n_dot_l <= 0.0f) { return result; } // Below surface — invalid sample.

		float n_dot_h = std::max(surf.normal.dot(h), 0.0f);

		// Throughput weight = F * G * VdotH / (NdotV * NdotH * spec_prob)
		// Derivation: BRDF = F*D*G/(4*NV*NL), PDF = D*NH/(4*VH)
		//   weight = BRDF * NL / (spec_prob * PDF)
		//          = F*D*G/(4*NV*NL) * NL / (spec_prob * D*NH/(4*VH))
		//          = F*G*VH / (NV*NH*spec_prob)
		// D cancels — numerically stable even for near-mirror surfaces.
		float g = ShadePass::geometry_smith_ggx(surf.n_dot_v, n_dot_l, surf.roughness);
		float fr = ShadePass::fresnel_schlick(v_dot_h, surf.f0_r);
		float fg = ShadePass::fresnel_schlick(v_dot_h, surf.f0_g);
		float fb = ShadePass::fresnel_schlick(v_dot_h, surf.f0_b);

		float common = g * v_dot_h / (surf.n_dot_v * n_dot_h * spec_prob + PT_EPSILON);
		result.weight_r = fr * common;
		result.weight_g = fg * common;
		result.weight_b = fb * common;
	} else {
		// ---- Cosine-weighted hemisphere sampling (diffuse lobe) ----
		result.direction = cosine_hemisphere_sample(surf.normal, rng);

		float n_dot_l = surf.normal.dot(result.direction);
		if (n_dot_l <= 0.0f) { return result; } // Below surface — invalid sample.

		// Throughput weight = diff_albedo / (1 - spec_prob)
		// Derivation: BRDF = diff/π, PDF = NdotL/π
		//   weight = BRDF * NdotL / ((1-spec_prob) * PDF)
		//          = (diff/π) * NL / ((1-p) * NL/π)
		//          = diff / (1 - spec_prob)
		// cos and π both cancel — zero variance for constant BRDF.
		float inv_prob = 1.0f / (1.0f - spec_prob);
		result.weight_r = surf.diff_r * inv_prob;
		result.weight_g = surf.diff_g * inv_prob;
		result.weight_b = surf.diff_b * inv_prob;
	}

	result.valid = true;
	return result;
}

} // namespace PathTrace
