#pragma once
// ray.h — The fundamental Ray type used by ALL modules (graphics, audio, AI).
//
// A ray is a parametric line: P(t) = origin + direction * t
// where t is a scalar. t_min and t_max bound the valid range.
//
// We use Godot's Vector3 directly — no custom math lib needed.
// This file is header-only (no .cpp) so it compiles everywhere it's included.

#include <cstdint>
#include <cfloat>
#include <godot_cpp/variant/vector3.hpp>

// Bring Godot's Vector3 into our scope so we can write "Vector3" instead of "godot::Vector3".
// This is fine for a GDExtension project where all code lives inside Godot.
using godot::Vector3;

struct Ray {
	// Where the ray starts in world space.
	Vector3 origin;

	// The direction the ray travels. Does NOT need to be normalized
	// for intersection math to work, but normalized directions make
	// the 't' value equal to actual distance, which is useful.
	Vector3 direction;

	// Only report hits where t >= t_min and t <= t_max.
	// t_min > 0 avoids self-intersection (hitting the surface you just bounced off).
	// t_max = FLT_MAX means "infinitely far".
	float t_min;
	float t_max;

	// Bit flags for future use (e.g., ray type: primary/shadow/audio/AI).
	uint32_t flags;

	// Default constructor: ray at origin pointing nowhere, full range.
	Ray()
		: origin(), direction(), t_min(0.001f), t_max(FLT_MAX), flags(0) {}

	// Construct a ray from origin to direction with optional range.
	Ray(const Vector3 &o, const Vector3 &d, float t0 = 0.001f, float t1 = FLT_MAX)
		: origin(o), direction(d), t_min(t0), t_max(t1), flags(0) {}

	// Check that this ray has valid (non-NaN, non-inf) data.
	bool is_valid() const {
		return direction.is_finite() && origin.is_finite() && t_min <= t_max;
	}

	// Get the point along the ray at parameter t: P(t) = origin + direction * t
	Vector3 at(float t) const {
		return origin + direction * t;
	}
};