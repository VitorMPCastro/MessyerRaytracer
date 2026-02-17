#pragma once
// triangle_tangents.h — Per-vertex tangent vectors for a single triangle.
//
// Stored as a parallel array alongside triangles, indexed by prim_id.
// Used for normal mapping: the tangent + bitangent + normal form the TBN
// matrix that transforms normal-map samples from tangent space to world space.
//
// Godot stores tangents as 4D vectors: (tx, ty, tz, sign).
// The sign component determines the bitangent direction:
//   bitangent = cross(normal, tangent) * sign
//
// When no vertex tangents are available, all three are set to zero —
// normal mapping is skipped and the smooth normal is used as-is.

#include <godot_cpp/variant/vector3.hpp>

using godot::Vector3;

struct TriangleTangents {
	Vector3 t0;   // Tangent at vertex 0 (xyz only, unit length)
	Vector3 t1;   // Tangent at vertex 1
	Vector3 t2;   // Tangent at vertex 2
	float sign0 = 0.0f;  // Bitangent sign at vertex 0 (+1 or -1, 0 = no tangent)
	float sign1 = 0.0f;  // Bitangent sign at vertex 1
	float sign2 = 0.0f;  // Bitangent sign at vertex 2

	/// Returns true if tangent data was provided (non-zero).
	inline bool has_tangents() const {
		return sign0 != 0.0f || sign1 != 0.0f || sign2 != 0.0f;
	}

	/// Interpolate tangent at a barycentric hit point.
	/// u, v are the Moller-Trumbore weights for v1, v2:
	///   result = normalize((1 - u - v) * t0 + u * t1 + v * t2)
	inline Vector3 interpolate_tangent(float u, float v) const {
		RT_ASSERT_FINITE(u);
		RT_ASSERT_FINITE(v);
		float w = 1.0f - u - v;
		Vector3 t = t0 * w + t1 * u + t2 * v;
		float len_sq = t.length_squared();
		// Guard against degenerate tangent (all-zero when no tangent data).
		if (len_sq < 1e-8f) { return Vector3(1.0f, 0.0f, 0.0f); }
		return t / std::sqrt(len_sq);
	}

	/// Interpolate bitangent sign at a barycentric hit point.
	/// Takes the sign from the nearest vertex (signs should be uniform per triangle,
	/// but we blend for robustness).
	inline float interpolate_sign(float u, float v) const {
		float w = 1.0f - u - v;
		float s = sign0 * w + sign1 * u + sign2 * v;
		return (s >= 0.0f) ? 1.0f : -1.0f;
	}
};
