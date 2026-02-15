#pragma once
// tri.h — Triangle primitive with ray-triangle intersection.
//
// This implements the Möller-Trumbore algorithm, which is the standard
// fast ray-triangle intersection test used in virtually all raytracers.
//
// How it works (simplified):
//   1. Compute two edge vectors of the triangle.
//   2. Use the cross product of ray direction and one edge to get "pvec".
//   3. Compute the determinant — if ~0, the ray is parallel to the triangle.
//   4. Compute barycentric coordinates (u, v) to check if the hit point
//      is inside the triangle (u >= 0, v >= 0, u + v <= 1).
//   5. Compute t (distance along ray). If t is within [t_min, t_max], it's a hit.
//
// This is O(1) per triangle — constant-time work per test.

#include "ray.h"
#include "intersection.h"
#include <cmath>

struct Triangle {
	// The three vertices of the triangle in world space.
	Vector3 v0, v1, v2;

	// Unique ID so we can identify which triangle was hit.
	uint32_t id;

	Triangle()
		: v0(), v1(), v2(), id(0) {}

	Triangle(const Vector3 &a, const Vector3 &b, const Vector3 &c, uint32_t _id)
		: v0(a), v1(b), v2(c), id(_id) {}

	// Test if ray 'r' hits this triangle.
	// If it does AND it's closer than the current best hit in 'out', update 'out'.
	// Returns true if 'out' was updated (new closest hit).
	bool intersect(const Ray &r, Intersection &out) const {
		// Step 1: Compute edge vectors from v0
		Vector3 edge1 = v1 - v0;
		Vector3 edge2 = v2 - v0;

		// Step 2: Cross product of ray direction and edge2
		// This is used to compute both the determinant and barycentric u.
		Vector3 pvec = r.direction.cross(edge2);

		// Step 3: Determinant — if close to 0, ray is parallel to triangle
		float det = edge1.dot(pvec);
		if (std::fabs(det) < 1e-8f) {
			return false; // Ray and triangle are parallel — no intersection
		}

		float inv_det = 1.0f / det;

		// Step 4: Compute barycentric coordinate U
		Vector3 tvec = r.origin - v0;
		float u = tvec.dot(pvec) * inv_det;
		if (u < 0.0f || u > 1.0f) {
			return false; // Hit point is outside the triangle
		}

		// Step 5: Compute barycentric coordinate V
		Vector3 qvec = tvec.cross(edge1);
		float v = r.direction.dot(qvec) * inv_det;
		if (v < 0.0f || u + v > 1.0f) {
			return false; // Hit point is outside the triangle
		}

		// Step 6: Compute t (distance along ray)
		float t = edge2.dot(qvec) * inv_det;

		// Check if t is within the ray's valid range
		if (t < r.t_min || t > r.t_max) {
			return false; // Hit is behind the ray or beyond max distance
		}

		// Only update if this hit is closer than any previous hit
		if (t < out.t) {
			out.t = t;
			out.position = r.at(t);
			out.normal = edge1.cross(edge2).normalized();
			out.prim_id = id;
			return true;
		}

		return false;
	}

	// Compute the face normal of this triangle (unit vector perpendicular to surface).
	Vector3 face_normal() const {
		return (v1 - v0).cross(v2 - v0).normalized();
	}
};
