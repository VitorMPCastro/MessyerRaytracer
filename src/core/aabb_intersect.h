#pragma once
// aabb_intersect.h — Axis-Aligned Bounding Box ray intersection (slab test).
//
// The performance-critical inner loop of BVH traversal.
// For each BVH node, this determines if the ray enters the bounding box.
// If it misses, ALL triangles in that subtree are skipped.
//
// OPTIMIZATION: Uses ray.inv_direction (precomputed 1.0/direction).
// This replaces 3 divisions with 3 multiplications per AABB test.
// Since a single ray tests dozens of AABBs during BVH traversal,
// this saves significant time.
//
// Uses Godot's AABB class for storage (position + size).

#include "core/ray.h"
#include "core/asserts.h"
#include <cmath>
#include <godot_cpp/variant/aabb.hpp>

// Fast ray-AABB intersection using the slab method.
//
// Returns true if the ray hits the box.
// tmin_out = where the ray ENTERS the box (closest point).
// tmax_out = where the ray EXITS the box (farthest point).
//
// Division-free: uses ray.inv_direction precomputed in Ray constructor.
inline bool ray_intersects_aabb(const Ray &r, const godot::AABB &p_box,
		float &tmin_out, float &tmax_out) {
	RT_ASSERT_VALID_RAY(r);
	RT_ASSERT(p_box.size.x >= 0.0f && p_box.size.y >= 0.0f && p_box.size.z >= 0.0f,
		"AABB size must be non-negative");
	Vector3 box_min = p_box.position;
	Vector3 box_max = p_box.position + p_box.size;

	// For each axis: compute where ray crosses the two bounding planes.
	// t = (plane_pos - ray_origin) * inv_direction   [no division!]
	float t1 = (box_min.x - r.origin.x) * r.inv_direction.x;
	float t2 = (box_max.x - r.origin.x) * r.inv_direction.x;
	float tmin = std::fmin(t1, t2);
	float tmax = std::fmax(t1, t2);

	t1 = (box_min.y - r.origin.y) * r.inv_direction.y;
	t2 = (box_max.y - r.origin.y) * r.inv_direction.y;
	tmin = std::fmax(tmin, std::fmin(t1, t2));
	tmax = std::fmin(tmax, std::fmax(t1, t2));

	t1 = (box_min.z - r.origin.z) * r.inv_direction.z;
	t2 = (box_max.z - r.origin.z) * r.inv_direction.z;
	tmin = std::fmax(tmin, std::fmin(t1, t2));
	tmax = std::fmin(tmax, std::fmax(t1, t2));

	tmin_out = tmin;
	tmax_out = tmax;

	// Ray hits if all three axis intervals overlap and the box is in front.
	return tmax >= std::fmax(tmin, 0.0f);
}
