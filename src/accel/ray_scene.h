#pragma once
// ray_scene.h — Flat scene container with TinyBVH-accelerated ray casting.
//
// Holds all triangles and a TinyBVH acceleration structure (BVH2 → BVH4/BVH8).
// Call build() after adding triangles to construct the BVH.
// Then cast_ray() and any_hit() use the BVH for O(log N) performance.
//
// DESIGN NOTE (Phase 2):
//   The primary traversal path is now SceneTLAS (two-level, instance-aware).
//   RayScene remains for:
//     - Single-mesh flat scenes (simple benchmarks)
//     - Legacy code paths during migration
//     - Brute-force validation mode (use_bvh = false)
//
// WHY NOT delete RayScene?
//   RayDispatcher and RayRenderer still reference it. Phase 2 will update
//   RayDispatcher to route through SceneTLAS, but RayScene stays available
//   for backward compatibility and flat-scene use cases.

#include "core/ray.h"
#include "core/intersection.h"
#include "core/triangle.h"
#include "accel/tinybvh_adapter.h"
#include "core/stats.h"
#include "core/asserts.h"
#include "dispatch/cpu_feature_detect.h"
#include <vector>

#ifndef TINYBVH_INST_IDX_BITS
#define TINYBVH_INST_IDX_BITS 32
#endif
#include "thirdparty/tinybvh/tiny_bvh.h"

struct RayScene {
	// Non-copyable: TinyBVH types (BVH, BVH4_CPU, BVH8_CPU, BVH8_CWBVH) own raw
	// pointers freed in their destructors but lack custom copy/move operators.
	// A shallow memberwise copy would cause double-free / heap corruption.
	RayScene() = default;
	RayScene(const RayScene &) = delete;
	RayScene &operator=(const RayScene &) = delete;
	RayScene(RayScene &&) = delete;
	RayScene &operator=(RayScene &&) = delete;

	// All triangles in the scene.
	std::vector<Triangle> triangles;

	// TinyBVH vertex data (3 × bvhvec4 per triangle).
	std::vector<tinybvh::bvhvec4> vertices;

	// TinyBVH acceleration structures.
	tinybvh::BVH bvh2;
	tinybvh::BVH4_CPU bvh4;
	tinybvh::BVH8_CPU bvh8;
	tinybvh::BVH8_CWBVH cwbvh;  // GPU-optimized compressed wide BVH (Phase 2).
	bool use_avx2 = false;
	bool built = false;

	// Toggle BVH on/off. Set to false for brute-force (useful for validation).
	bool use_bvh = true;

	// Build acceleration structure. Call after all triangles are added.
	void build() {
		if (!use_bvh || triangles.empty()) { return; }
		RT_ASSERT(!triangles.empty(), "RayScene::build: triangles must not be empty");

		const uint32_t tri_count = static_cast<uint32_t>(triangles.size());

		tinybvh_adapter::triangles_to_vertices(triangles.data(), tri_count, vertices);

		bvh2.Build(vertices.data(), tri_count);

		use_avx2 = cpu_features::has_avx2();
		if (use_avx2) {
			bvh8.Build(vertices.data(), tri_count);
		} else {
			bvh4.Build(vertices.data(), tri_count);
		}

		// Build CWBVH for GPU traversal (1.5-2× faster than Aila-Laine BVH2 on GPU).
		// CWBVH is a compressed 8-wide BVH with quantized child AABBs — its node layout
		// fits perfectly into GPU cache lines and enables single-fetch 8-child testing.
		cwbvh.Build(vertices.data(), tri_count);

		built = true;
		RT_ASSERT(built, "RayScene BVH should be built after build()");
	}

	// Cast a ray, returning the closest intersection.
	// O(log N) with BVH, O(N) brute force.
	Intersection cast_ray(const Ray &r, RayStats *stats = nullptr,
			uint32_t query_mask = 0xFFFFFFFF) const {
		RT_ASSERT_VALID_RAY(r);
		if (use_bvh && built) {
			if (stats) { stats->rays_cast++; }

			tinybvh::Ray tray = tinybvh_adapter::to_tinybvh_ray(r);

			if (use_avx2) {
				bvh8.Intersect(tray);
			} else {
				bvh4.Intersect(tray);
			}

			Intersection hit = tinybvh_adapter::from_tinybvh_intersection(tray, r);
			if (hit.hit()) {
				// Apply query mask filter.
				if (hit.prim_id < static_cast<uint32_t>(triangles.size())) {
					const Triangle &tri = triangles[hit.prim_id];
					if ((tri.layers & query_mask) == 0) {
						return Intersection{}; // Filtered out by mask.
					}
					hit.hit_layers = tri.layers;
					hit.normal = tri.normal;
				}
				if (stats) { stats->hits++; }
			}
			return hit;
		}

		// Brute force fallback
		Intersection closest;
		if (stats) { stats->rays_cast++; }
		for (const Triangle &tri : triangles) {
			if ((tri.layers & query_mask) == 0) { continue; }
			if (stats) { stats->tri_tests++; }
			if (tri.intersect(r, closest)) {
				closest.hit_layers = tri.layers;
			}
		}
		if (stats && closest.hit()) { stats->hits++; }
		return closest;
	}

	// Test if a ray hits ANYTHING (shadow/occlusion query).
	// Returns on first hit — no need to find the closest.
	bool any_hit(const Ray &r, RayStats *stats = nullptr,
			uint32_t query_mask = 0xFFFFFFFF) const {
		RT_ASSERT_VALID_RAY(r);
		if (use_bvh && built) {
			if (stats) { stats->rays_cast++; }
			// NOTE: TinyBVH IsOccluded doesn't support query_mask natively.
			// For masked queries, fall through to brute force.
			if (query_mask == 0xFFFFFFFF) {
				tinybvh::Ray tray = tinybvh_adapter::to_tinybvh_ray(r);
				bool occluded = use_avx2 ? bvh8.IsOccluded(tray) : bvh4.IsOccluded(tray);
				if (stats && occluded) { stats->hits++; }
				return occluded;
			}
		}

		// Brute force fallback (or masked query)
		if (stats) { stats->rays_cast++; }
		Intersection temp;
		for (const Triangle &tri : triangles) {
			if ((tri.layers & query_mask) == 0) { continue; }
			if (stats) { stats->tri_tests++; }
			if (tri.intersect(r, temp)) {
				if (stats) { stats->hits++; }
				return true;
			}
		}
		return false;
	}

	// Cast multiple rays at once.
	void cast_rays(const Ray *rays, Intersection *results, int count,
			RayStats *stats = nullptr, uint32_t query_mask = 0xFFFFFFFF) const {
		RT_ASSERT_NOT_NULL(rays);
		RT_ASSERT_NOT_NULL(results);
		RT_ASSERT(count >= 0, "RayScene::cast_rays: count must be non-negative");
		for (int i = 0; i < count; i++) {
			results[i] = cast_ray(rays[i], stats, query_mask);
		}
	}

	// Batch any_hit: test multiple rays, returning bool per ray.
	void any_hit_rays(const Ray *rays, bool *hit_results, int count,
			RayStats *stats = nullptr, uint32_t query_mask = 0xFFFFFFFF) const {
		RT_ASSERT_NOT_NULL(rays);
		RT_ASSERT_NOT_NULL(hit_results);
		RT_ASSERT(count >= 0, "RayScene::any_hit_rays: count must be non-negative");
		for (int i = 0; i < count; i++) {
			hit_results[i] = any_hit(rays[i], stats, query_mask);
		}
	}

	// Clear all geometry and reset BVH.
	//
	// NOTE: We intentionally do NOT reset the TinyBVH objects (bvh2, bvh4, bvh8, cwbvh)
	// via copy assignment (e.g. `bvh2 = BVH{}`).  TinyBVH classes have user-defined
	// destructors (AlignedFree) but no custom copy/move operators — the compiler-generated
	// copy assignment does a shallow pointer copy, leaking the old allocation and potentially
	// causing double-free / heap corruption (Rule of Five violation).
	//
	// TinyBVH's Build methods handle their own memory management: PrepareBuild checks
	// allocatedNodes and frees + reallocates as needed, so rebuilding over a previously-
	// built BVH is safe.  Setting `built = false` prevents stale data from being used.
	void clear() {
		RT_ASSERT(!built || !triangles.empty(),
			"RayScene::clear: built flag set but no triangles");
		triangles.clear();
		vertices.clear();
		built = false;
		RT_ASSERT(triangles.empty(), "Triangles should be empty after clear()");
	}

	int triangle_count() const {
		return static_cast<int>(triangles.size());
	}
};
