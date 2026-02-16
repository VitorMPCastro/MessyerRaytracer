#pragma once
// triangle_normals.h — Per-vertex normals for a single triangle.
//
// Stored as a parallel array alongside triangles, indexed by prim_id.
// Used for smooth shading: interpolate with barycentrics at the hit point
// to get a smooth surface normal instead of the flat face normal.
//
// When no vertex normals are available (e.g. CSG, generated meshes),
// all three normals are set to the face normal — the interpolation
// then gracefully degrades to flat shading.

#include <godot_cpp/variant/vector3.hpp>

using godot::Vector3;

struct TriangleNormals {
	Vector3 n0;  // Normal at vertex 0
	Vector3 n1;  // Normal at vertex 1
	Vector3 n2;  // Normal at vertex 2

	/// Interpolate normal at a barycentric hit point.
	/// u, v are the Moller-Trumbore weights for v1, v2:
	///   result = normalize((1 - u - v) * n0 + u * n1 + v * n2)
	inline Vector3 interpolate(float u, float v) const {
		float w = 1.0f - u - v;
		return (n0 * w + n1 * u + n2 * v).normalized();
	}
};
