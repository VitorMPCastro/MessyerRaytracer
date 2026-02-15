#pragma once
// aabb.h — Axis-Aligned Bounding Box ray intersection test.
//
// An AABB is just a box whose sides are aligned with the X, Y, Z axes.
// It's the fundamental building block of BVH acceleration structures:
// before testing expensive triangle intersections, you first test if the
// ray even enters the bounding box. If it misses the box, you skip ALL
// triangles inside it — this is what makes BVH fast (O(log N) instead of O(N)).
//
// We use Godot's built-in AABB class for storage (it has position + size),
// and provide our own fast "slab test" that returns tmin/tmax values
// (Godot's built-in AABB ray test doesn't return these, and we need them
// for BVH traversal to decide which child to visit first).
//
// The slab test works by computing where the ray enters and exits each
// pair of parallel planes (X, Y, Z). The ray hits the box only if ALL
// three intervals overlap.

#include "ray.h"
#include <cmath>
#include <godot_cpp/variant/aabb.hpp>

// Fast ray-AABB intersection using the slab method.
// Returns true if the ray hits the box, and outputs tmin/tmax.
// tmin = where the ray enters the box, tmax = where it exits.
//
// p_box: Godot's AABB (has .position and .size members)
// r: the ray to test
// tmin_out, tmax_out: filled with entry/exit distances
inline bool ray_intersects_aabb(const Ray &r, const godot::AABB &p_box, float &tmin_out, float &tmax_out) {
	// Compute the min and max corners of the box.
	// Godot's AABB stores position (min corner) and size.
	Vector3 box_min = p_box.position;
	Vector3 box_max = p_box.position + p_box.size;

	// Safe inverse direction: avoid division by zero.
	// If a direction component is ~0, replace with a tiny value.
	const float EPS = 1e-9f;
	auto safe_inv = [EPS](float d) -> float {
		if (std::fabs(d) < EPS) {
			return (d < 0.0f) ? (-1.0f / EPS) : (1.0f / EPS);
		}
		return 1.0f / d;
	};

	float inv_x = safe_inv(r.direction.x);
	float inv_y = safe_inv(r.direction.y);
	float inv_z = safe_inv(r.direction.z);

	// For each axis, compute where the ray crosses the two planes.
	// Then find the overlap of all three intervals.
	float t1 = (box_min.x - r.origin.x) * inv_x;
	float t2 = (box_max.x - r.origin.x) * inv_x;
	float tmin = std::fmin(t1, t2);
	float tmax = std::fmax(t1, t2);

	t1 = (box_min.y - r.origin.y) * inv_y;
	t2 = (box_max.y - r.origin.y) * inv_y;
	tmin = std::fmax(tmin, std::fmin(t1, t2));
	tmax = std::fmin(tmax, std::fmax(t1, t2));

	t1 = (box_min.z - r.origin.z) * inv_z;
	t2 = (box_max.z - r.origin.z) * inv_z;
	tmin = std::fmax(tmin, std::fmin(t1, t2));
	tmax = std::fmin(tmax, std::fmax(t1, t2));

	tmin_out = tmin;
	tmax_out = tmax;

	// The ray hits the box if the intervals overlap and the box is in front of us.
	return tmax >= std::fmax(tmin, 0.0f);
}
