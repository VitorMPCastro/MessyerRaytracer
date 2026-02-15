#pragma once
// ray_scene.h — A simple scene that holds triangles and casts rays against them.
//
// This is the BRUTE FORCE version: for each ray, we test EVERY triangle.
// Cost: O(N) per ray where N = number of triangles.
//
// Later we'll add a BVH (Bounding Volume Hierarchy) to make this O(log N).
// But starting brute-force is important because:
//   1. It's simple and correct — easy to verify.
//   2. It becomes the reference to validate BVH results against.
//   3. For small scenes (<1000 triangles) it's fast enough.

#include "ray.h"
#include "intersection.h"
#include "tri.h"
#include <vector>

struct RayScene {
	// All triangles in the scene. Built from mesh data.
	std::vector<Triangle> triangles;

	// Cast a ray against ALL triangles. Returns the closest hit.
	// This is O(N) — tests every triangle. BVH will fix this later.
	Intersection cast_ray(const Ray &r) const {
		Intersection closest;  // starts as NO_HIT with t = FLT_MAX

		for (const Triangle &tri : triangles) {
			// intersect() only updates 'closest' if this hit is nearer.
			tri.intersect(r, closest);
		}

		return closest;
	}

	// Test if a ray hits ANYTHING (shadow/occlusion query).
	// Faster than cast_ray because it returns on first hit — doesn't need closest.
	bool any_hit(const Ray &r) const {
		Intersection temp;
		for (const Triangle &tri : triangles) {
			if (tri.intersect(r, temp)) {
				return true;
			}
		}
		return false;
	}

	// Clear all triangles.
	void clear() {
		triangles.clear();
	}

	// How many triangles are in the scene.
	int triangle_count() const {
		return static_cast<int>(triangles.size());
	}
};
