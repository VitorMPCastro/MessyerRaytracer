#pragma once
// ray_scene.h — Scene container with BVH-accelerated ray casting.
//
// Holds all triangles and a BVH acceleration structure.
// Call build() after adding triangles to construct the BVH.
// Then cast_ray() and any_hit() use the BVH for O(log N) performance.
//
// Brute-force fallback is available by setting use_bvh = false.
// Use it to validate BVH correctness: both methods should produce
// identical results for the same scene and rays.

#include "core/ray.h"
#include "core/intersection.h"
#include "core/triangle.h"
#include "accel/bvh.h"
#include "core/stats.h"
#include "core/asserts.h"
#include <vector>

struct RayScene {
	// All triangles in the scene.
	std::vector<Triangle> triangles;

	// BVH acceleration structure. Built by build().
	BVH bvh;

	// Toggle BVH on/off. Set to false for brute-force (useful for validation).
	bool use_bvh = true;

	// Build acceleration structure. Call after all triangles are added.
	// WARNING: reorders the triangles vector (required for BVH leaf contiguity).
	void build() {
		if (use_bvh && !triangles.empty()) {
			bvh.build(triangles);
			RT_ASSERT(bvh.is_built(), "BVH should be built after RayScene::build()");
		}
	}

	// Cast a ray, returning the closest intersection.
	// O(log N) with BVH, O(N) brute force.
	Intersection cast_ray(const Ray &r, RayStats *stats = nullptr) const {
		RT_ASSERT_VALID_RAY(r);
		if (use_bvh && bvh.is_built()) {
			return bvh.cast_ray(r, triangles, stats);
		}

		// Brute force fallback
		Intersection closest;
		if (stats) stats->rays_cast++;
		for (const Triangle &tri : triangles) {
			if (stats) stats->tri_tests++;
			tri.intersect(r, closest);
		}
		if (stats && closest.hit()) stats->hits++;
		return closest;
	}

	// Test if a ray hits ANYTHING (shadow/occlusion query).
	// Returns on first hit — no need to find the closest.
	bool any_hit(const Ray &r, RayStats *stats = nullptr) const {
		RT_ASSERT_VALID_RAY(r);
		if (use_bvh && bvh.is_built()) {
			return bvh.any_hit(r, triangles, stats);
		}

		// Brute force fallback
		if (stats) stats->rays_cast++;
		Intersection temp;
		for (const Triangle &tri : triangles) {
			if (stats) stats->tri_tests++;
			if (tri.intersect(r, temp)) {
				if (stats) stats->hits++;
				return true;
			}
		}
		return false;
	}

	// Cast multiple rays at once. More cache-friendly for batch operations.
	// Modules (graphics, audio, AI) will typically use this instead of
	// casting one ray at a time.
	void cast_rays(const Ray *rays, Intersection *results, int count,
			RayStats *stats = nullptr) const {
		RT_ASSERT_NOT_NULL(rays);
		RT_ASSERT_NOT_NULL(results);
		RT_ASSERT(count >= 0, "RayScene::cast_rays: count must be non-negative");
		for (int i = 0; i < count; i++) {
			results[i] = cast_ray(rays[i], stats);
		}
	}

	// Cast multiple rays using 4-ray packets for BVH coherence.
	// Processes rays in groups of 4 with packet AABB test, remainder uses single cast.
	// Falls back to sequential cast_ray if BVH isn't built.
	void cast_rays_packet(const Ray *rays, Intersection *results, int count,
			RayStats *stats = nullptr) const {
		RT_ASSERT_NOT_NULL(rays);
		RT_ASSERT_NOT_NULL(results);
		RT_ASSERT(count >= 0, "RayScene::cast_rays_packet: count must be non-negative");
#if RAYTRACER_PACKET_SSE
		if (use_bvh && bvh.is_built()) {
			int i = 0;
			// Process full packets of 4
			for (; i + 4 <= count; i += 4) {
				bvh.cast_ray_packet4(&rays[i], &results[i], 4, triangles, stats);
			}
			// Remaining rays (1–3) as a partial packet
			if (i < count) {
				bvh.cast_ray_packet4(&rays[i], &results[i], count - i, triangles, stats);
			}
			return;
		}
#endif
		// Fallback: sequential
		for (int i = 0; i < count; i++) {
			results[i] = cast_ray(rays[i], stats);
		}
	}

	// Batch any_hit: test multiple rays, returning bool per ray.
	// For shadow/occlusion queries where you only need hit/miss answers.
	void any_hit_rays(const Ray *rays, bool *hit_results, int count,
			RayStats *stats = nullptr) const {
		RT_ASSERT_NOT_NULL(rays);
		RT_ASSERT_NOT_NULL(hit_results);
		RT_ASSERT(count >= 0, "RayScene::any_hit_rays: count must be non-negative");
		for (int i = 0; i < count; i++) {
			hit_results[i] = any_hit(rays[i], stats);
		}
	}

	// Clear all geometry and reset BVH.
	void clear() {
		triangles.clear();
		bvh = BVH{};
		RT_ASSERT(triangles.empty(), "Triangles should be empty after clear()");
	}

	int triangle_count() const {
		return static_cast<int>(triangles.size());
	}
};
