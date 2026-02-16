#pragma once
// triangle.h — Triangle primitive with ray-triangle intersection.
//
// Implements the Möller-Trumbore algorithm.
//
// OPTIMIZATIONS over the naive version:
//   1. edge1, edge2, normal are PRECOMPUTED in the constructor.
//      Previously recomputed for every intersect() call.
//      With 4238 triangles × 192 rays = 813,696 tests, this eliminates
//      813,696 redundant vector subtractions and cross products.
//   2. aabb() and centroid() methods support BVH construction.
//
// Memory trade-off: each Triangle is larger (stores 3 extra Vector3s)
// but intersect() is faster. This is the standard CPU raytracer trade-off.

#include "core/ray.h"
#include "core/intersection.h"
#include "core/asserts.h"
#include <cmath>
#include <godot_cpp/variant/aabb.hpp>

struct Triangle {
	// The three vertices of the triangle in world space.
	Vector3 v0, v1, v2;

	// Precomputed in constructor: edges from v0, and face normal.
	Vector3 edge1;  // v1 - v0
	Vector3 edge2;  // v2 - v0
	Vector3 normal; // normalized face normal (edge1 × edge2).normalized()

	// Unique ID so we can identify which triangle was hit.
	uint32_t id;

	// Visibility layer bitmask (matches Godot's VisualInstance3D.layers).
	// 0xFFFFFFFF = visible on all layers (default).
	uint32_t layers;

	Triangle()
		: v0(), v1(), v2(), edge1(), edge2(), normal(), id(0), layers(0xFFFFFFFF) {}

	Triangle(const Vector3 &a, const Vector3 &b, const Vector3 &c, uint32_t _id,
			uint32_t _layers = 0xFFFFFFFF)
		: v0(a), v1(b), v2(c), id(_id), layers(_layers) {
		RT_ASSERT(a.is_finite() && b.is_finite() && c.is_finite(),
			"Triangle vertices must be finite");
		edge1 = v1 - v0;
		edge2 = v2 - v0;
		RT_ASSERT(edge1.length_squared() > 1e-16f || edge2.length_squared() > 1e-16f,
			"Degenerate triangle: both edges are near-zero length");
		normal = edge1.cross(edge2).normalized();
	}

	// Test if ray 'r' hits this triangle.
	// If it does AND it's closer than the current best hit in 'out', update 'out'.
	// Returns true if 'out' was updated (new closest hit).
	//
	// Uses precomputed edge1/edge2/normal — no redundant work per call.
	bool intersect(const Ray &r, Intersection &out) const {
		RT_ASSERT_VALID_RAY(r);
		RT_ASSERT(out.t >= 0.0f, "Intersection t must be non-negative before test");
		// Step 1: pvec = direction × edge2 (used for determinant and barycentric u)
		Vector3 pvec = r.direction.cross(edge2);

		// Step 2: determinant — if ~0, ray is parallel to triangle plane
		float det = edge1.dot(pvec);
		if (std::fabs(det) < 1e-8f) {
			return false;
		}

		float inv_det = 1.0f / det;

		// Step 3: barycentric coordinate u
		Vector3 tvec = r.origin - v0;
		float u = tvec.dot(pvec) * inv_det;
		if (u < 0.0f || u > 1.0f) {
			return false;
		}

		// Step 4: barycentric coordinate v
		Vector3 qvec = tvec.cross(edge1);
		float v = r.direction.dot(qvec) * inv_det;
		if (v < 0.0f || u + v > 1.0f) {
			return false;
		}

		// Step 5: distance along ray
		float t = edge2.dot(qvec) * inv_det;
		if (t < r.t_min || t > r.t_max) {
			return false;
		}

		// Only update if this hit is closer than any previous hit
		if (t < out.t) {
			out.t = t;
			out.position = r.at(t);
			out.normal = normal; // Precomputed! Was: edge1.cross(edge2).normalized()
			out.u = u;           // Barycentric coord for v1 (already computed above)
			out.v = v;           // Barycentric coord for v2 (already computed above)
			out.prim_id = id;
			return true;
		}

		return false;
	}

	// Face normal (just returns the precomputed value).
	Vector3 face_normal() const {
		return normal;
	}

	// Compute the axis-aligned bounding box for this triangle.
	// Used during BVH construction.
	godot::AABB aabb() const {
		Vector3 mn(
			std::fmin(v0.x, std::fmin(v1.x, v2.x)),
			std::fmin(v0.y, std::fmin(v1.y, v2.y)),
			std::fmin(v0.z, std::fmin(v1.z, v2.z))
		);
		Vector3 mx(
			std::fmax(v0.x, std::fmax(v1.x, v2.x)),
			std::fmax(v0.y, std::fmax(v1.y, v2.y)),
			std::fmax(v0.z, std::fmax(v1.z, v2.z))
		);
		return godot::AABB(mn, mx - mn);
	}

	// Centroid of the triangle (used for BVH splitting decisions).
	Vector3 centroid() const {
		return (v0 + v1 + v2) * (1.0f / 3.0f);
	}
};
