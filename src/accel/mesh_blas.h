#pragma once
// mesh_blas.h — Bottom-Level Acceleration Structure for a single mesh (TinyBVH backend).
//
// WHAT: Holds object-space triangles + TinyBVH BVH for one mesh.
//       Multiple instances share a single BLAS — only the transform differs.
//
// DESIGN: Three BVH representations coexist:
//   1. tinybvh::BVH (BVH2) — base structure, input for all conversions.
//   2. tinybvh::BVH4_CPU — SSE 4-wide CPU traversal (fallback).
//   3. tinybvh::BVH8_CPU — AVX2 8-wide CPU traversal (preferred, ~40% faster).
//   Runtime detection (cpu_feature_detect.h) picks BVH4 or BVH8.
//
// BUILD FLOW:
//   1. Populate triangles (object space)
//   2. triangles_to_vertices() → bvhvec4 array
//   3. bvh2.Build(vertices) → BVH2
//   4. If AVX2: bvh8.Build(vertices)  else: bvh4.Build(vertices)
//
// INTERSECTION FLOW:
//   1. Convert our Ray → tinybvh::Ray
//   2. Call bvh8.Intersect(ray) or bvh4.Intersect(ray)
//   3. Convert tinybvh::Intersection → our Intersection
//   4. Lookup triangles[prim] for normal, layers
//
// WHY keep both our Triangle and bvhvec4 vertices?
//   - TinyBVH needs bvhvec4[3*N] for building but doesn't store normals/layers.
//   - Our shading pipeline needs normals, layers, edge1/edge2 from Triangle.
//   - Memory cost: ~128 bytes per triangle (80 Triangle + 48 vertices).
//     For 2M tris that's ~256MB — acceptable for a development raytracer.

#include "accel/tinybvh_adapter.h"
#include "core/triangle.h"
#include "core/ray.h"
#include "core/intersection.h"
#include "core/stats.h"
#include "core/asserts.h"
#include "dispatch/cpu_feature_detect.h"
#include <vector>

#ifndef TINYBVH_INST_IDX_BITS
#define TINYBVH_INST_IDX_BITS 32
#endif
#include "thirdparty/tinybvh/tiny_bvh.h"

struct MeshBLAS {
	// Non-copyable, non-movable: TinyBVH types (BVH, BVH4_CPU, BVH8_CPU) own raw
	// pointers freed in their destructors but lack custom copy/move operators.
	// A shallow memberwise copy/move causes double-free / heap corruption.
	// Stored as std::unique_ptr<MeshBLAS> in SceneTLAS to allow safe vector resizing.
	MeshBLAS() = default;
	MeshBLAS(const MeshBLAS &) = delete;
	MeshBLAS &operator=(const MeshBLAS &) = delete;
	MeshBLAS(MeshBLAS &&) = delete;
	MeshBLAS &operator=(MeshBLAS &&) = delete;

	// All triangles for this mesh (object space).
	// Needed for shading: normals, layers, UVs are looked up by prim_id.
	std::vector<Triangle> triangles;

	// Vertex data for TinyBVH builder: 3 × bvhvec4 per triangle.
	// Populated by build() from triangles.
	std::vector<tinybvh::bvhvec4> vertices;

	// Unique ID for this BLAS (assigned by SceneTLAS).
	uint32_t id = 0;

	// ---- TinyBVH acceleration structures ----
	// BVH2: base binary BVH. Also used by TinyBVH's TLAS builder (BVHBase*).
	tinybvh::BVH bvh2;

	// BVH4_CPU: SSE 4-wide traversal (always available on x86).
	tinybvh::BVH4_CPU bvh4;

	// BVH8_CPU: AVX2 8-wide traversal (only on AVX2+FMA CPUs).
	tinybvh::BVH8_CPU bvh8;

	// Whether to use BVH8 (AVX2) or BVH4 (SSE). Determined once at build time.
	bool use_avx2 = false;

	// Whether the BVH has been built.
	bool built = false;

	// Build the BVH from the current triangles.
	// Converts triangles to TinyBVH vertex format, builds BVH2, then
	// builds BVH4 or BVH8 depending on CPU capabilities.
	void build() {
		RT_ASSERT(!triangles.empty(), "MeshBLAS::build: cannot build from empty triangles");

		// 1. Convert our Triangle array to bvhvec4 vertex array.
		tinybvh_adapter::triangles_to_vertices(triangles.data(),
			static_cast<uint32_t>(triangles.size()), vertices);

		// 2. Build BVH2 (base binary BVH — also used by TLAS builder).
		bvh2.Build(vertices.data(), static_cast<uint32_t>(triangles.size()));

		// 3. Build wide BVH for CPU traversal based on CPU capabilities.
		use_avx2 = cpu_features::has_avx2();
		if (use_avx2) {
			bvh8.Build(vertices.data(), static_cast<uint32_t>(triangles.size()));
		} else {
			bvh4.Build(vertices.data(), static_cast<uint32_t>(triangles.size()));
		}

		built = true;
		RT_ASSERT(built, "MeshBLAS::build: must be built after build()");
	}

	// Cast a ray against this mesh's BVH (object-space ray).
	// Returns the closest intersection with normal + layers filled in.
	Intersection cast_ray(const Ray &r, RayStats *stats = nullptr) const {
		RT_ASSERT_VALID_RAY(r);
		RT_ASSERT(!triangles.empty(), "MeshBLAS::cast_ray called on empty mesh");

		if (stats) { stats->rays_cast++; }

		if (!built) {
			return _cast_ray_brute(r, stats);
		}

		// Convert our Ray to TinyBVH Ray and traverse.
		tinybvh::Ray tr = tinybvh_adapter::to_tinybvh_ray(r);

		if (use_avx2) {
			bvh8.Intersect(tr);
		} else {
			bvh4.Intersect(tr);
		}

		// Convert result back and fill shading data from Triangle lookup.
		Intersection hit = tinybvh_adapter::from_tinybvh_intersection(tr, r);
		if (hit.hit()) {
			tinybvh_adapter::fill_hit_details(hit, triangles.data(),
				static_cast<uint32_t>(triangles.size()));
			if (stats) { stats->hits++; }
		}

		return hit;
	}

	// Any-hit test (object-space ray). Returns true on first intersection.
	bool any_hit(const Ray &r, RayStats *stats = nullptr) const {
		RT_ASSERT_VALID_RAY(r);
		RT_ASSERT(!triangles.empty(), "MeshBLAS::any_hit called on empty mesh");

		if (stats) { stats->rays_cast++; }

		if (!built) {
			Intersection temp;
			for (const Triangle &tri : triangles) {
				if (tri.intersect(r, temp)) {
					if (stats) { stats->hits++; }
					return true;
				}
			}
			return false;
		}

		tinybvh::Ray tr = tinybvh_adapter::to_tinybvh_ray(r);
		bool occluded = use_avx2 ? bvh8.IsOccluded(tr) : bvh4.IsOccluded(tr);

		if (occluded && stats) { stats->hits++; }
		return occluded;
	}

	// AABB of the mesh in object space.
	godot::AABB object_bounds() const {
		RT_ASSERT(built, "MeshBLAS::object_bounds: BVH must be built first");
		if (built && bvh2.NodeCount() > 0) {
			const auto &root = bvh2.bvhNode[0];
			godot::Vector3 bmin(root.aabbMin.x, root.aabbMin.y, root.aabbMin.z);
			godot::Vector3 bmax(root.aabbMax.x, root.aabbMax.y, root.aabbMax.z);
			return godot::AABB(bmin, bmax - bmin);
		}
		// Compute from triangles as fallback.
		if (triangles.empty()) { return godot::AABB(); }
		godot::AABB bounds = triangles[0].aabb();
		for (size_t i = 1; i < triangles.size(); i++) {
			bounds = bounds.merge(triangles[i].aabb());
		}
		RT_ASSERT(bounds.size.x >= 0.0f && bounds.size.y >= 0.0f && bounds.size.z >= 0.0f,
			"Computed object bounds have negative size");
		return bounds;
	}

	int triangle_count() const {
		return static_cast<int>(triangles.size());
	}

	void clear() {
		RT_ASSERT(!built || !triangles.empty(),
			"MeshBLAS::clear: built flag set but no triangles");
		triangles.clear();
		vertices.clear();
		// NOTE: We intentionally do NOT reset TinyBVH objects (bvh2, bvh4, bvh8)
		// via copy assignment.  TinyBVH classes have user-defined destructors
		// (AlignedFree) but no custom copy/move operators — the compiler-generated
		// copy does a shallow pointer copy, leaking the old allocation.
		// TinyBVH's Build/PrepareBuild handles memory management internally.
		built = false;
		RT_ASSERT(triangles.empty(), "Triangles should be empty after MeshBLAS::clear()");
	}

private:
	// Brute force fallback when BVH isn't built.
	Intersection _cast_ray_brute(const Ray &r, RayStats *stats) const {
		RT_ASSERT_VALID_RAY(r);
		RT_ASSERT(!triangles.empty(), "_cast_ray_brute: no triangles to test");
		Intersection closest;
		for (const Triangle &tri : triangles) {
			if (stats) { stats->tri_tests++; }
			tri.intersect(r, closest);
		}
		if (stats && closest.hit()) { stats->hits++; }
		return closest;
	}
};
