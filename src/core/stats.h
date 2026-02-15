#pragma once
// stats.h — Performance counters for ray tracing operations.
//
// Tracks how the raytracer spends its time:
//   rays_cast:          Total rays traced
//   tri_tests:          Total triangle intersection tests performed
//   bvh_nodes_visited:  Total BVH nodes popped from the traversal stack
//   hits:               Rays that found at least one intersection
//
// Pass a RayStats* to cast_ray/cast_rays to collect data.
// Pass nullptr to skip counting (zero overhead — the branch is predicted away).
//
// INTERPRETING THE NUMBERS:
//   Brute force: tri_tests / rays_cast ≈ triangle_count (tests everything)
//   Good BVH:    tri_tests / rays_cast ≈ 5–20 (skips most triangles)
//   Perfect BVH: bvh_nodes_visited / rays_cast ≈ 2 * log2(triangle_count)

#include <cstdint>

struct RayStats {
	uint64_t rays_cast = 0;
	uint64_t tri_tests = 0;
	uint64_t bvh_nodes_visited = 0;
	uint64_t hits = 0;

	void reset() {
		rays_cast = 0;
		tri_tests = 0;
		bvh_nodes_visited = 0;
		hits = 0;
	}

	RayStats &operator+=(const RayStats &other) {
		rays_cast += other.rays_cast;
		tri_tests += other.tri_tests;
		bvh_nodes_visited += other.bvh_nodes_visited;
		hits += other.hits;
		return *this;
	}

	// Average triangle tests per ray (main BVH quality metric).
	float avg_tri_tests_per_ray() const {
		return (rays_cast > 0) ? static_cast<float>(tri_tests) / rays_cast : 0.0f;
	}

	// Average BVH nodes visited per ray.
	float avg_nodes_per_ray() const {
		return (rays_cast > 0) ? static_cast<float>(bvh_nodes_visited) / rays_cast : 0.0f;
	}

	// Hit rate as a percentage (0–100).
	float hit_rate_percent() const {
		return (rays_cast > 0) ? 100.0f * static_cast<float>(hits) / rays_cast : 0.0f;
	}
};
