#pragma once
// intersection.h — The result of a ray hitting (or missing) geometry.
//
// After you cast a ray against the scene, you get an Intersection back.
// It tells you WHERE the ray hit, WHAT it hit, and the surface NORMAL.
//
// Key concept: "set_miss()" marks the intersection as empty.
// After testing all triangles, check hit() to see if anything was found.

#include <cstdint>
#include <cfloat>
#include <godot_cpp/variant/vector3.hpp>

using godot::Vector3;

struct Intersection {
	// How far along the ray the hit occurred: hit_point = ray.origin + ray.direction * t
	float t;

	// The exact world-space point where the ray hit the surface.
	Vector3 position;

	// The surface normal at the hit point (points "outward" from the surface).
	// Useful for shading (graphics), reflection (audio), and orientation (AI).
	Vector3 normal;

	// Which triangle/primitive was hit. Use this to look up material, mesh, etc.
	uint32_t prim_id;

	// Layer bitmask of the triangle that was hit (for filtering/queries).
	uint32_t hit_layers;

	// Sentinel value meaning "no hit". We use the max uint32 value.
	static constexpr uint32_t NO_HIT = UINT32_MAX;

	// Default: no hit.
	Intersection()
		: t(FLT_MAX), position(), normal(), prim_id(NO_HIT), hit_layers(0) {}

	// Reset to "no hit" state. Call this before testing a new ray.
	void set_miss() {
		t = FLT_MAX;
		prim_id = NO_HIT;
		hit_layers = 0;
	}

	// Did the ray hit something?
	bool hit() const {
		return prim_id != NO_HIT;
	}
};
