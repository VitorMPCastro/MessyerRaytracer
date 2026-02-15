#pragma once
// ray.h — The fundamental Ray type used by ALL modules (graphics, audio, AI).
//
// A ray is a parametric line: P(t) = origin + direction * t
// where t is a scalar. t_min and t_max bound the valid range.
//
// OPTIMIZATIONS over the naive version:
//   1. inv_direction — precomputed 1.0/direction per component.
//      Avoids 3 divisions in EVERY AABB slab test. A single ray tests
//      dozens of AABBs during BVH traversal, so this is a big win.
//   2. dir_sign[3] — 0 if direction component is positive, 1 if negative.
//      Used by BVH traversal for front-to-back child ordering.
//
// We use Godot's Vector3 directly — no custom math lib.
// Header-only (no .cpp) so it compiles everywhere it's included.

#include <cstdint>
#include <cfloat>
#include <cmath>
#include <godot_cpp/variant/vector3.hpp>
#include "core/asserts.h"

using godot::Vector3;

struct Ray {
	// Where the ray starts in world space.
	Vector3 origin;

	// The direction the ray travels. Should be normalized so 't' equals distance.
	Vector3 direction;

	// Precomputed 1.0 / direction for each component.
	// Used by AABB slab test to replace division with multiplication.
	// Safe: near-zero components are replaced with ±1e9 (huge but finite).
	Vector3 inv_direction;

	// Sign of each direction component: 0 = positive, 1 = negative.
	// Used by BVH traversal to pick near/far child without branching.
	int dir_sign[3];

	// Only report hits where t >= t_min and t <= t_max.
	// t_min > 0 prevents self-intersection ("shadow acne").
	// t_max = FLT_MAX means "infinitely far".
	float t_min;
	float t_max;

	// Bit flags for future use (ray type: primary/shadow/audio/AI).
	uint32_t flags;

	// Default constructor: ray at origin pointing nowhere.
	Ray()
		: origin(), direction(), inv_direction(),
		  t_min(0.001f), t_max(FLT_MAX), flags(0) {
		dir_sign[0] = dir_sign[1] = dir_sign[2] = 0;
	}

	// Construct a ray with precomputed acceleration data.
	Ray(const Vector3 &o, const Vector3 &d, float t0 = 0.001f, float t1 = FLT_MAX)
		: origin(o), direction(d), t_min(t0), t_max(t1), flags(0) {
		RT_ASSERT(d.is_finite(), "Ray direction must be finite");
		RT_ASSERT(t0 >= 0.0f, "Ray t_min must be non-negative");
		RT_ASSERT(t0 <= t1, "Ray t_min must be <= t_max");
		_precompute();
	}

	// Check that this ray has valid (non-NaN, non-Inf) data.
	bool is_valid() const {
		return direction.is_finite() && origin.is_finite() && t_min <= t_max;
	}

	// Get the point along the ray at parameter t: P(t) = origin + direction * t
	Vector3 at(float t) const {
		RT_ASSERT_FINITE(t);
		return origin + direction * t;
	}

private:
	// Compute cached values from direction. Called once in constructor.
	void _precompute() {
		const float EPS = 1e-9f;
		auto safe_inv = [EPS](float d) -> float {
			return (std::fabs(d) < EPS)
				? ((d < 0.0f) ? (-1.0f / EPS) : (1.0f / EPS))
				: (1.0f / d);
		};
		inv_direction = Vector3(
			safe_inv(direction.x),
			safe_inv(direction.y),
			safe_inv(direction.z)
		);
		dir_sign[0] = (direction.x < 0.0f) ? 1 : 0;
		dir_sign[1] = (direction.y < 0.0f) ? 1 : 0;
		dir_sign[2] = (direction.z < 0.0f) ? 1 : 0;
	}
};
