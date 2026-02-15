#pragma once
// mesh_blas.h — Bottom-Level Acceleration Structure for a single mesh.
//
// WHAT IS A BLAS?
//   A BLAS (Bottom-Level Acceleration Structure) holds the BVH and triangles
//   for one mesh in OBJECT space. Multiple instances of the same mesh can
//   share a single BLAS — only the transform differs.
//
// EXAMPLE:
//   A forest scene with 1000 trees but 3 unique tree meshes: build 3 BLASes,
//   create 1000 instances. BVH build cost is proportional to unique meshes,
//   not total objects.
//
// LIFECYCLE:
//   1. Add triangles (in object space)
//   2. Call build() to construct the BVH
//   3. Create BLASInstance(s) referencing this BLAS with different transforms
//   4. Add instances to SceneTLAS
//
// IMPORTANT:
//   Triangles are in OBJECT SPACE. When ray-testing, the ray is transformed
//   INTO object space using the instance's inverse transform. The hit is then
//   transformed back to world space.

#include "accel/bvh.h"
#include "core/triangle.h"
#include "core/ray.h"
#include "core/intersection.h"
#include "core/stats.h"
#include "core/asserts.h"
#include <vector>

struct MeshBLAS {
	// All triangles for this mesh (object space).
	std::vector<Triangle> triangles;

	// BVH over the triangles.
	BVH bvh;

	// Unique ID for this BLAS (assigned by SceneTLAS).
	uint32_t id = 0;

	// Build the BVH from the current triangles.
	// WARNING: reorders the triangles array (BVH requires contiguous leaves).
	void build() {
		if (!triangles.empty()) {
			bvh.build(triangles);
			RT_ASSERT(bvh.is_built(), "MeshBLAS::build should succeed for non-empty mesh");
		}
	}

	// Cast a ray against this mesh's BVH (object-space ray).
	Intersection cast_ray(const Ray &r, RayStats *stats = nullptr) const {
		RT_ASSERT_VALID_RAY(r);
		if (bvh.is_built()) {
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

	// Any-hit test (object-space ray).
	bool any_hit(const Ray &r, RayStats *stats = nullptr) const {
		RT_ASSERT_VALID_RAY(r);
		if (bvh.is_built()) {
			return bvh.any_hit(r, triangles, stats);
		}

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

	// AABB of the mesh in object space.
	godot::AABB object_bounds() const {
		if (bvh.is_built() && !bvh.get_nodes().empty()) {
			return bvh.get_nodes()[0].bounds;
		}
		// Compute from triangles
		if (triangles.empty()) return godot::AABB();
		godot::AABB bounds = triangles[0].aabb();
		for (size_t i = 1; i < triangles.size(); i++) {
			bounds = bounds.merge(triangles[i].aabb());
		}
		return bounds;
	}

	int triangle_count() const {
		return static_cast<int>(triangles.size());
	}

	void clear() {
		triangles.clear();
		bvh = BVH{};
		RT_ASSERT(triangles.empty(), "Triangles should be empty after MeshBLAS::clear()");
	}
};
