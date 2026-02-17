#pragma once
// scene_tlas.h — Top-Level Acceleration Structure using TinyBVH native TLAS.
//
// WHAT: The TLAS is a BVH over instances (objects in the scene), not triangles.
//       Each instance references a MeshBLAS and has a world-space transform.
//
// DESIGN (Phase 2 — TinyBVH TLAS):
//   Previously used a proxy-triangle hack with our custom BVH. Now delegates
//   to TinyBVH's native TLAS builder and traversal:
//     - tinybvh::BVH::Build(BLASInstance*, instCount, BVHBase**, blasCount)
//     - tinybvh::BVH::Intersect(ray) auto-dispatches to IntersectTLAS
//     - TinyBVH handles ray↔object-space transforms internally
//     - TinyBVH dispatches to BVH4_CPU (SSE) or BVH8_CPU (AVX2) per BLAS
//
// WHY native TLAS instead of proxy triangles?
//   - Correct instance-AABB overlap (proxy triangles were approximate)
//   - TinyBVH handles ordered traversal, ray masking, and BLAS layout dispatch
//   - Eliminates our hand-written two-level stack traversal (~100 lines of code)
//   - Better SAH quality from instance AABBs vs degenerate proxy triangles
//
// TRAVERSAL FLOW:
//   1. Convert our Ray → tinybvh::Ray
//   2. tlas_bvh_.Intersect(tray)  ← handles TLAS + BLAS + ray transforms
//   3. Convert result back: tray.hit → our Intersection
//   4. Look up triangle from MeshBLAS[inst].triangles[prim] for normal/layers
//   5. Transform normal to world space via instance transform
//
// REBUILD STRATEGY:
//   - BLASes: Built once per unique mesh. Never rebuilt unless mesh changes.
//   - TLAS: Rebuilt when objects move. Refit support via TinyBVH::Refit().

#include "accel/mesh_blas.h"
#include "accel/blas_instance.h"
#include "accel/tinybvh_adapter.h"
#include "core/stats.h"
#include "core/asserts.h"
#include <vector>
#include <cstdint>
#include <memory>

#ifndef TINYBVH_INST_IDX_BITS
#define TINYBVH_INST_IDX_BITS 32
#endif
#include "thirdparty/tinybvh/tiny_bvh.h"

class SceneTLAS {
public:
	// Non-copyable, non-movable: contains tinybvh::BVH (tlas_bvh_) which owns
	// raw pointers freed in its destructor but lacks custom copy/move operators.
	SceneTLAS() = default;
	SceneTLAS(const SceneTLAS &) = delete;
	SceneTLAS &operator=(const SceneTLAS &) = delete;
	SceneTLAS(SceneTLAS &&) = delete;
	SceneTLAS &operator=(SceneTLAS &&) = delete;

	// ========================================================================
	// Mesh management — add unique meshes (BLAS)
	// ========================================================================

	// Add a new mesh and return its BLAS ID.
	// After adding, populate blas_meshes_[id].triangles and call build_blas(id).
	uint32_t add_mesh() {
		RT_ASSERT(blas_meshes_.size() < static_cast<size_t>(UINT32_MAX), "add_mesh: BLAS mesh count overflow");
		uint32_t id = static_cast<uint32_t>(blas_meshes_.size());
		blas_meshes_.push_back(std::make_unique<MeshBLAS>());
		RT_ASSERT_NOT_NULL(blas_meshes_.back().get());
		blas_meshes_.back()->id = id;
		return id;
	}

	// Access a BLAS for populating triangles.
	MeshBLAS &mesh(uint32_t blas_id) {
		RT_ASSERT(blas_id < static_cast<uint32_t>(blas_meshes_.size()),
			"SceneTLAS::mesh: blas_id out of range");
		RT_ASSERT_NOT_NULL(blas_meshes_[blas_id].get());
		return *blas_meshes_[blas_id];
	}
	const MeshBLAS &mesh(uint32_t blas_id) const {
		RT_ASSERT(blas_id < static_cast<uint32_t>(blas_meshes_.size()),
			"SceneTLAS::mesh const: blas_id out of range");
		RT_ASSERT_NOT_NULL(blas_meshes_[blas_id].get());
		return *blas_meshes_[blas_id];
	}

	// Build the BVH for a specific mesh.
	void build_blas(uint32_t blas_id) {
		RT_ASSERT(blas_id < static_cast<uint32_t>(blas_meshes_.size()),
			"SceneTLAS::build_blas: blas_id out of range");
		blas_meshes_[blas_id]->build();
	}

	// Build ALL BLASes that haven't been built yet.
	void build_all_blas() {
		for (auto &m : blas_meshes_) {
			RT_ASSERT_NOT_NULL(m.get());
			if (!m->built && !m->triangles.empty()) {
				m->build();
				RT_ASSERT(m->built, "build_all_blas: BVH build did not set built flag");
			}
		}
	}

	// ========================================================================
	// Instance management — place meshes in the scene
	// ========================================================================

	// Add an instance of a mesh at a given transform. Returns instance ID.
	uint32_t add_instance(uint32_t blas_id, const godot::Transform3D &xform) {
		RT_ASSERT(blas_id < static_cast<uint32_t>(blas_meshes_.size()),
			"SceneTLAS::add_instance: blas_id out of range");
		RT_ASSERT(!blas_meshes_[blas_id]->triangles.empty(),
			"SceneTLAS::add_instance: BLAS has no triangles");
		uint32_t inst_id = static_cast<uint32_t>(instances_.size());
		BLASInstance inst;
		inst.blas_id = blas_id;
		inst.instance_id = inst_id;
		inst.transform = xform;
		inst.update_inverse();
		inst.compute_world_bounds(blas_meshes_[blas_id]->object_bounds());
		instances_.push_back(inst);
		return inst_id;
	}

	// Update an instance's transform. Call build_tlas() after all updates.
	void set_instance_transform(uint32_t inst_id, const godot::Transform3D &xform) {
		RT_ASSERT(inst_id < static_cast<uint32_t>(instances_.size()),
			"SceneTLAS::set_instance_transform: inst_id out of range");
		BLASInstance &inst = instances_[inst_id];
		inst.transform = xform;
		inst.update_inverse();
		inst.compute_world_bounds(blas_meshes_[inst.blas_id]->object_bounds());
	}

	// ========================================================================
	// TLAS build — construct top-level BVH over instances via TinyBVH
	// ========================================================================

	// Build the TLAS. Call after adding/moving instances.
	// Uses TinyBVH's native TLAS builder with BLASInstance array.
	void build_tlas() {
		if (instances_.empty()) { return; }

		const uint32_t inst_count = static_cast<uint32_t>(instances_.size());
		const uint32_t blas_count = static_cast<uint32_t>(blas_meshes_.size());

		// 1. Build BVHBase* array — one per unique mesh.
		//    Points to BVH4 or BVH8 depending on CPU, used by IntersectTLAS dispatch.
		blas_ptrs_.resize(blas_count);
		for (uint32_t i = 0; i < blas_count; i++) {
			MeshBLAS &m = *blas_meshes_[i];
			RT_ASSERT(m.built, "SceneTLAS::build_tlas: all BLASes must be built before TLAS");
			if (m.use_avx2) {
				blas_ptrs_[i] = &m.bvh8;
			} else {
				blas_ptrs_[i] = &m.bvh4;
			}
		}

		// 2. Fill TinyBVH BLASInstance array from our instances.
		tinybvh_instances_.resize(inst_count);
		for (uint32_t i = 0; i < inst_count; i++) {
			const BLASInstance &our_inst = instances_[i];
			tinybvh::BLASInstance &ti = tinybvh_instances_[i];
			ti.blasIdx = our_inst.blas_id;
			ti.transform = tinybvh_adapter::to_bvhmat4(our_inst.transform);
			// Update computes invTransform and world AABB from the BLAS root bounds.
			ti.Update(blas_ptrs_[our_inst.blas_id]);
		}

		// 3. Build TLAS over instances.
		tlas_bvh_.Build(tinybvh_instances_.data(), inst_count,
			blas_ptrs_.data(), blas_count);

		built_ = true;
		RT_ASSERT(tlas_bvh_.NodeCount() > 0, "TLAS BVH should have nodes after build");
	}

	// Refit the TLAS BVH after instances have been moved.
	// O(N) — cheaper than full rebuild. Call after set_instance_transform().
	void refit_tlas() {
		if (!built_ || instances_.empty()) { return; }
		RT_ASSERT(tlas_bvh_.NodeCount() > 0, "Cannot refit TLAS that hasn't been built");
		RT_ASSERT(tinybvh_instances_.size() == instances_.size(),
			"refit_tlas: tinybvh instance count must match our instance count");

		// Update TinyBVH instances with current transforms.
		for (uint32_t i = 0; i < static_cast<uint32_t>(instances_.size()); i++) {
			const BLASInstance &our_inst = instances_[i];
			tinybvh::BLASInstance &ti = tinybvh_instances_[i];
			ti.transform = tinybvh_adapter::to_bvhmat4(our_inst.transform);
			ti.Update(blas_ptrs_[our_inst.blas_id]);
		}

		// Refit the TLAS BVH bounds bottom-up.
		tlas_bvh_.Refit();
	}

	// ========================================================================
	// Ray casting — TinyBVH handles two-level traversal internally
	// ========================================================================

	// Cast a world-space ray. TinyBVH traverses TLAS → BLAS internally.
	Intersection cast_ray(const Ray &world_ray, RayStats *stats = nullptr) const {
		RT_ASSERT_VALID_RAY(world_ray);
		Intersection closest;

		if (!built_) {
			return _cast_ray_brute(world_ray, stats);
		}

		if (stats) { stats->rays_cast++; }

		// Convert our Ray to TinyBVH Ray.
		tinybvh::Ray tray = tinybvh_adapter::to_tinybvh_ray(world_ray);

		// TinyBVH::Intersect auto-dispatches to IntersectTLAS when instList is set.
		// Internally handles: TLAS traversal, ray→object transform, BLAS dispatch.
		tlas_bvh_.Intersect(tray);

		// Convert TinyBVH result to our Intersection.
		if (tray.hit.t < world_ray.t_max) {
			closest.t = tray.hit.t;
			closest.u = tray.hit.u;
			closest.v = tray.hit.v;
			closest.prim_id = tray.hit.prim;

			// World-space position from world ray parameterization.
			// t is world-parameterized because TinyBVH doesn't renormalize direction.
			closest.position = world_ray.origin + world_ray.direction * closest.t;

			// Look up instance and BLAS for shading data.
			const uint32_t inst_idx = tray.hit.inst;
			RT_ASSERT(inst_idx < static_cast<uint32_t>(instances_.size()),
				"SceneTLAS::cast_ray: inst_idx out of range");
			const BLASInstance &inst = instances_[inst_idx];
			const MeshBLAS &blas = *blas_meshes_[inst.blas_id];

			// Get object-space normal and layers from the BLAS triangle.
			RT_ASSERT(closest.prim_id < static_cast<uint32_t>(blas.triangles.size()),
				"SceneTLAS::cast_ray: prim_id out of range in BLAS");
			const Triangle &tri = blas.triangles[closest.prim_id];
			closest.hit_layers = tri.layers;

			// Transform normal from object space to world space.
			closest.normal = inst.transform.basis.xform(tri.normal).normalized();

			if (stats) { stats->hits++; }
		}

		return closest;
	}

	// Any-hit world-space ray (shadow/occlusion query, early exit).
	bool any_hit(const Ray &world_ray, RayStats *stats = nullptr) const {
		RT_ASSERT_VALID_RAY(world_ray);
		RT_ASSERT(built_ || instances_.empty(),
			"SceneTLAS::any_hit: TLAS must be built or scene empty");
		if (!built_) { return _any_hit_brute(world_ray, stats); }

		if (stats) { stats->rays_cast++; }

		tinybvh::Ray tray = tinybvh_adapter::to_tinybvh_ray(world_ray);
		bool occluded = tlas_bvh_.IsOccluded(tray);

		if (stats && occluded) { stats->hits++; }
		return occluded;
	}

	// Batch cast rays.
	void cast_rays(const Ray *rays, Intersection *results, int count,
			RayStats *stats = nullptr) const {
		RT_ASSERT_NOT_NULL(rays);
		RT_ASSERT_NOT_NULL(results);
		RT_ASSERT(count >= 0, "SceneTLAS::cast_rays: count must be non-negative");
		for (int i = 0; i < count; i++) {
			results[i] = cast_ray(rays[i], stats);
		}
	}

	// Batch any-hit rays.
	void any_hit_rays(const Ray *rays, bool *hit_results, int count,
			RayStats *stats = nullptr) const {
		RT_ASSERT_NOT_NULL(rays);
		RT_ASSERT_NOT_NULL(hit_results);
		RT_ASSERT(count >= 0, "SceneTLAS::any_hit_rays: count must be non-negative");
		for (int i = 0; i < count; i++) {
			hit_results[i] = any_hit(rays[i], stats);
		}
	}

	// ========================================================================
	// Accessors
	// ========================================================================

	int mesh_count() const { return static_cast<int>(blas_meshes_.size()); }
	int instance_count() const { return static_cast<int>(instances_.size()); }
	bool is_built() const { return built_; }

	const std::vector<BLASInstance> &instances() const { return instances_; }

	// Access the TinyBVH TLAS (for GPU upload — CWBVH conversion needs it).
	const tinybvh::BVH &tlas_bvh() const { return tlas_bvh_; }

	// Access BLASBase pointers (for GPU CWBVH build).
	const std::vector<tinybvh::BVHBase *> &blas_ptrs() const { return blas_ptrs_; }

	// Total triangle count across all BLASes.
	int total_triangle_count() const {
		int total = 0;
		for (const auto &m : blas_meshes_) {
			RT_ASSERT_NOT_NULL(m.get());
			total += m->triangle_count();
		}
		RT_ASSERT(total >= 0, "total_triangle_count: integer overflow");
		return total;
	}

	void clear() {
		RT_ASSERT(!built_ || !instances_.empty(),
			"SceneTLAS::clear: built flag set but no instances");
		blas_meshes_.clear();   // unique_ptrs delete MeshBLAS objects automatically
		instances_.clear();
		tinybvh_instances_.clear();
		blas_ptrs_.clear();
		// NOTE: We intentionally do NOT reset tlas_bvh_ via copy assignment
		// (e.g. tlas_bvh_ = tinybvh::BVH{}). TinyBVH::BVH has a user-defined
		// destructor (AlignedFree) but no custom copy/move operators — the
		// compiler-generated copy assignment does a shallow pointer copy, leaking
		// the old allocation. TinyBVH's Build/PrepareBuild handles memory
		// management internally (frees + reallocates when needed).
		built_ = false;
		RT_ASSERT(blas_meshes_.empty() && instances_.empty(),
			"SceneTLAS::clear: containers should be empty");
	}

private:
	std::vector<std::unique_ptr<MeshBLAS>> blas_meshes_; // Heap-allocated BLASes (safe from vector reallocation)
	std::vector<BLASInstance> instances_;                // Our metadata per instance
	std::vector<tinybvh::BLASInstance> tinybvh_instances_; // TinyBVH instance array for TLAS
	std::vector<tinybvh::BVHBase *> blas_ptrs_;         // BVHBase* per BLAS for TinyBVH
	tinybvh::BVH tlas_bvh_;                             // Top-level BVH (TinyBVH native)
	bool built_ = false;

	// Brute-force fallback (no TLAS built).
	Intersection _cast_ray_brute(const Ray &world_ray, RayStats *stats) const {
		RT_ASSERT_VALID_RAY(world_ray);
		RT_ASSERT(!blas_meshes_.empty() || instances_.empty(),
			"_cast_ray_brute: instances exist but no meshes");
		Intersection closest;
		if (stats) { stats->rays_cast++; }
		for (const auto &inst : instances_) {
			const MeshBLAS &m = *blas_meshes_[inst.blas_id];
			Ray obj_ray = inst.transform_ray_to_object(world_ray);
			obj_ray.t_max = closest.t;
			Intersection hit = m.cast_ray(obj_ray, stats);
			if (hit.hit() && hit.t < closest.t) {
				inst.transform_hit_to_world(hit);
				closest = hit;
			}
		}
		if (stats && closest.hit()) { stats->hits++; }
		return closest;
	}

	bool _any_hit_brute(const Ray &world_ray, RayStats *stats) const {
		RT_ASSERT_VALID_RAY(world_ray);
		RT_ASSERT(!blas_meshes_.empty() || instances_.empty(),
			"_any_hit_brute: instances exist but no meshes");
		if (stats) { stats->rays_cast++; }
		for (const auto &inst : instances_) {
			const MeshBLAS &m = *blas_meshes_[inst.blas_id];
			Ray obj_ray = inst.transform_ray_to_object(world_ray);
			if (m.any_hit(obj_ray, stats)) {
				if (stats) { stats->hits++; }
				return true;
			}
		}
		return false;
	}
};
