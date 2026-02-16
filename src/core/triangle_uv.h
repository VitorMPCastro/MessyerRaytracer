#pragma once
// triangle_uv.h — Per-vertex UV coordinates for a single triangle.
//
// Stored as a parallel array alongside triangles, indexed by prim_id.
// Used for texture mapping: interpolate with barycentrics at the hit point
// to get the UV coordinate for texture sampling.
//
// UV layout matches Godot convention:
//   (0,0) = top-left, (1,1) = bottom-right of the texture.

#include <godot_cpp/variant/vector2.hpp>

using godot::Vector2;

struct TriangleUV {
	Vector2 uv0;  // UV at vertex 0
	Vector2 uv1;  // UV at vertex 1
	Vector2 uv2;  // UV at vertex 2

	/// Interpolate UV at a barycentric hit point.
	/// u, v are the Möller-Trumbore weights for v1, v2:
	///   result = (1 - u - v) * uv0 + u * uv1 + v * uv2
	inline Vector2 interpolate(float u, float v) const {
		float w = 1.0f - u - v;
		return uv0 * w + uv1 * u + uv2 * v;
	}
};
