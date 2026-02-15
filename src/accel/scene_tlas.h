#pragma once
// scene_tlas.h — Top-Level Acceleration Structure for multi-object scenes.
//
// WHAT IS A TLAS?
//   The TLAS is a BVH over instances (objects in the scene), not triangles.
//   Each instance references a BLAS (mesh) and has a world-space transform.
//
//   Traversal is two-level:
//     1. Traverse TLAS to find which instances the ray potentially hits
//     2. For each candidate instance:
//        a. Transform the ray into object space
//        b. Traverse the instance's BLAS to find triangle hits
//        c. Transform the hit back to world space
//
// WHEN TO USE TLAS/BLAS vs FLAT BVH?
//   TLAS/BLAS (SceneTLAS):
//     - Multiple objects that can move independently
//     - Instanced meshes (many copies of the same geometry)
//     - Dynamic scenes (moving characters, physics objects)
//
//   Flat BVH (RayScene):
//     - Single static mesh or terrain
//     - Simple scenes where rebuild is cheap
//     - Maximum single-mesh traversal performance (no transform overhead)
//
// REBUILD STRATEGY:
//   - BLASes: Built once per unique mesh. Never rebuilt unless mesh changes.
//   - TLAS: Rebuilt or refitted when objects move (see BVH refit).
//     Refitting is O(N) vs O(N log N) for full rebuild.

#include "accel/mesh_blas.h"
#include "accel/blas_instance.h"
#include "accel/bvh.h"
#include "core/stats.h"
#include "core/asserts.h"
#include <vector>
#include <cstdint>

class SceneTLAS {
public:
	// ========================================================================
	// Mesh management — add unique meshes (BLAS)
	// ========================================================================

	// Add a new mesh and return its BLAS ID.
	// After adding, populate blas_meshes_[id].triangles and call build_blas(id).
	uint32_t add_mesh() {
		uint32_t id = static_cast<uint32_t>(blas_meshes_.size());
		blas_meshes_.emplace_back();
		blas_meshes_.back().id = id;
		return id;
	}

	// Access a BLAS for populating triangles.
	MeshBLAS &mesh(uint32_t blas_id) {
		RT_ASSERT(blas_id < static_cast<uint32_t>(blas_meshes_.size()),
			"SceneTLAS::mesh: blas_id out of range");
		return blas_meshes_[blas_id];
	}
	const MeshBLAS &mesh(uint32_t blas_id) const {
		RT_ASSERT(blas_id < static_cast<uint32_t>(blas_meshes_.size()),
			"SceneTLAS::mesh const: blas_id out of range");
		return blas_meshes_[blas_id];
	}

	// Build the BVH for a specific mesh.
	void build_blas(uint32_t blas_id) {
		RT_ASSERT(blas_id < static_cast<uint32_t>(blas_meshes_.size()),
			"SceneTLAS::build_blas: blas_id out of range");
		blas_meshes_[blas_id].build();
	}

	// Build ALL BLASes that haven't been built yet.
	void build_all_blas() {
		for (auto &m : blas_meshes_) {
			if (!m.bvh.is_built() && !m.triangles.empty()) {
				m.build();
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
		uint32_t inst_id = static_cast<uint32_t>(instances_.size());
		BLASInstance inst;
		inst.blas_id = blas_id;
		inst.instance_id = inst_id;
		inst.transform = xform;
		inst.update_inverse();
		inst.compute_world_bounds(blas_meshes_[blas_id].object_bounds());
		instances_.push_back(inst);
		return inst_id;
	}

	// Update an instance's transform. Call rebuild_tlas() after all updates.
	void set_instance_transform(uint32_t inst_id, const godot::Transform3D &xform) {
		RT_ASSERT(inst_id < static_cast<uint32_t>(instances_.size()),
			"SceneTLAS::set_instance_transform: inst_id out of range");
		BLASInstance &inst = instances_[inst_id];
		inst.transform = xform;
		inst.update_inverse();
		inst.compute_world_bounds(blas_meshes_[inst.blas_id].object_bounds());
	}

	// ========================================================================
	// TLAS build — construct top-level BVH over instances
	// ========================================================================

	// Build the TLAS. Call after adding/moving instances.
	// Creates a BVH over instance world AABBs.
	void build_tlas() {
		if (instances_.empty()) return;

		// Create "proxy triangles" for the TLAS BVH builder.
		// Each proxy is a degenerate triangle whose AABB matches the instance's world bounds.
		// This is a pragmatic hack: we reuse the existing BVH builder (needs triangles)
		// instead of building a separate instance-aware BVH.
		proxy_tris_.resize(instances_.size());
		for (size_t i = 0; i < instances_.size(); i++) {
			const godot::AABB &wb = instances_[i].world_bounds;
			Vector3 center = wb.position + wb.size * 0.5f;

			// Create a degenerate triangle at the center, with edges spanning the AABB.
			// The BVH builder uses aabb() and centroid() from Triangle.
			// Our proxy triangle's aabb() won't match the instance's world_bounds exactly
			// (since it's just 3 points), so we use the AABB corners directly.
			Vector3 p0 = wb.position;
			Vector3 p1 = wb.position + Vector3(wb.size.x, 0, wb.size.z);
			Vector3 p2 = wb.position + Vector3(0, wb.size.y, 0);

			proxy_tris_[i] = Triangle(p0, p1, p2, static_cast<uint32_t>(i));
		}

		tlas_bvh_.build(proxy_tris_);
		built_ = true;
	}

	// Refit the TLAS BVH after instances have been moved.
	// O(N) — much cheaper than full rebuild. Call after set_instance_transform().
	//
	// IMPORTANT: Update proxy triangles first so refit uses current positions.
	void refit_tlas() {
		if (!built_ || instances_.empty()) return;

		// Update proxy triangles to match current instance world bounds.
		for (size_t i = 0; i < instances_.size(); i++) {
			const godot::AABB &wb = instances_[i].world_bounds;
			Vector3 p0 = wb.position;
			Vector3 p1 = wb.position + Vector3(wb.size.x, 0, wb.size.z);
			Vector3 p2 = wb.position + Vector3(0, wb.size.y, 0);
			proxy_tris_[i] = Triangle(p0, p1, p2, proxy_tris_[i].id);
		}

		// Refit BVH bounds bottom-up.
		tlas_bvh_.refit(proxy_tris_);
	}

	// ========================================================================
	// Ray casting — two-level traversal
	// ========================================================================

	// Cast a world-space ray. Traverses TLAS → BLAS for each candidate instance.
	Intersection cast_ray(const Ray &world_ray, RayStats *stats = nullptr) const {
		RT_ASSERT_VALID_RAY(world_ray);
		Intersection closest;

		if (!built_) {
			// No TLAS — brute force over all instances
			return cast_ray_brute(world_ray, stats);
		}

		if (stats) stats->rays_cast++;

		// Traverse TLAS BVH to find candidate instances.
		// Then for each candidate, transform ray into object space and test BLAS.
		float root_tmin, root_tmax;
		if (!ray_intersects_aabb(world_ray, tlas_bvh_.get_nodes()[0].bounds,
				root_tmin, root_tmax)) {
			return closest;
		}

		const auto &tlas_nodes = tlas_bvh_.get_nodes();

		struct StackEntry { uint32_t idx; float tmin; };
		StackEntry stack[64];
		int sp = 0;
		stack[sp++] = { 0, root_tmin };

		while (sp > 0) {
			RT_ASSERT(sp <= 64, "TLAS traversal stack overflow in cast_ray");
			StackEntry entry = stack[--sp];
			if (entry.tmin > closest.t) continue;

			if (stats) stats->bvh_nodes_visited++;
			const BVHNode &node = tlas_nodes[entry.idx];

			if (node.is_leaf()) {
				// Test each instance in this leaf
				for (uint32_t i = 0; i < node.count; i++) {
					uint32_t proxy_idx = node.left_first + i;
					uint32_t inst_id = proxy_tris_[proxy_idx].id;
					const BLASInstance &inst = instances_[inst_id];
					const MeshBLAS &mesh = blas_meshes_[inst.blas_id];

					// Quick AABB test before transforming ray
					float inst_tmin, inst_tmax;
					if (!ray_intersects_aabb(world_ray, inst.world_bounds,
							inst_tmin, inst_tmax)) {
						continue;
					}
					if (inst_tmin > closest.t) continue;

					// Transform ray to object space
					Ray obj_ray = inst.transform_ray_to_object(world_ray);
					obj_ray.t_max = closest.t; // Shrink t_max to current best

					// Cast against BLAS
					Intersection hit = mesh.cast_ray(obj_ray, stats);
					if (hit.hit() && hit.t < closest.t) {
						// Transform hit position/normal back to world space
						inst.transform_hit_to_world(hit);
						closest = hit;
					}
				}
			} else {
				uint32_t left = entry.idx + 1;
				uint32_t right = node.left_first;

				float tmin_l, tmax_l, tmin_r, tmax_r;
				bool hit_l = ray_intersects_aabb(world_ray, tlas_nodes[left].bounds,
						tmin_l, tmax_l) && tmin_l <= closest.t;
				bool hit_r = ray_intersects_aabb(world_ray, tlas_nodes[right].bounds,
						tmin_r, tmax_r) && tmin_r <= closest.t;

				if (hit_l && hit_r) {
					if (tmin_l < tmin_r) {
						stack[sp++] = { right, tmin_r };
						stack[sp++] = { left, tmin_l };
					} else {
						stack[sp++] = { left, tmin_l };
						stack[sp++] = { right, tmin_r };
					}
				} else if (hit_l) {
					stack[sp++] = { left, tmin_l };
				} else if (hit_r) {
					stack[sp++] = { right, tmin_r };
				}
			}
		}

		if (stats && closest.hit()) stats->hits++;
		return closest;
	}

	// Any-hit world-space ray (two-level traversal with early exit).
	bool any_hit(const Ray &world_ray, RayStats *stats = nullptr) const {
		RT_ASSERT_VALID_RAY(world_ray);
		if (!built_) return any_hit_brute(world_ray, stats);

		if (stats) stats->rays_cast++;

		float root_tmin, root_tmax;
		if (!ray_intersects_aabb(world_ray, tlas_bvh_.get_nodes()[0].bounds,
				root_tmin, root_tmax)) {
			return false;
		}

		const auto &tlas_nodes = tlas_bvh_.get_nodes();

		uint32_t stack[64];
		int sp = 0;
		stack[sp++] = 0;

		while (sp > 0) {
			uint32_t node_idx = stack[--sp];
			const BVHNode &node = tlas_nodes[node_idx];

			if (stats) stats->bvh_nodes_visited++;

			float tmin, tmax;
			if (!ray_intersects_aabb(world_ray, node.bounds, tmin, tmax)) continue;

			if (node.is_leaf()) {
				for (uint32_t i = 0; i < node.count; i++) {
					uint32_t proxy_idx = node.left_first + i;
					uint32_t inst_id = proxy_tris_[proxy_idx].id;
					const BLASInstance &inst = instances_[inst_id];
					const MeshBLAS &mesh = blas_meshes_[inst.blas_id];

					Ray obj_ray = inst.transform_ray_to_object(world_ray);
					if (mesh.any_hit(obj_ray, stats)) {
						if (stats) stats->hits++;
						return true;
					}
				}
			} else {
				stack[sp++] = node.left_first;
				stack[sp++] = node_idx + 1;
			}
		}

		return false;
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

	// ========================================================================
	// Accessors
	// ========================================================================

	int mesh_count() const { return static_cast<int>(blas_meshes_.size()); }
	int instance_count() const { return static_cast<int>(instances_.size()); }
	bool is_built() const { return built_; }

	const std::vector<BLASInstance> &instances() const { return instances_; }
	const std::vector<MeshBLAS> &meshes() const { return blas_meshes_; }

	// Total triangle count across all BLASes.
	int total_triangle_count() const {
		int total = 0;
		for (const auto &m : blas_meshes_) {
			total += m.triangle_count();
		}
		return total;
	}

	void clear() {
		blas_meshes_.clear();
		instances_.clear();
		proxy_tris_.clear();
		tlas_bvh_ = BVH{};
		built_ = false;
	}

private:
	std::vector<MeshBLAS> blas_meshes_;    // All unique meshes (BLAS)
	std::vector<BLASInstance> instances_;    // All placed instances
	std::vector<Triangle> proxy_tris_;      // Proxy triangles for TLAS BVH builder
	BVH tlas_bvh_;                          // Top-level BVH
	bool built_ = false;

	// AABB intersection for TLAS traversal (same as bvh.h's version).
	static bool ray_intersects_aabb(const Ray &r, const godot::AABB &box,
			float &out_tmin, float &out_tmax) {
		Vector3 bmin = box.position;
		Vector3 bmax = bmin + box.size;

		float tmin = (bmin.x - r.origin.x) * r.inv_direction.x;
		float tmax = (bmax.x - r.origin.x) * r.inv_direction.x;
		if (tmin > tmax) std::swap(tmin, tmax);

		float tymin = (bmin.y - r.origin.y) * r.inv_direction.y;
		float tymax = (bmax.y - r.origin.y) * r.inv_direction.y;
		if (tymin > tymax) std::swap(tymin, tymax);

		tmin = std::fmax(tmin, tymin);
		tmax = std::fmin(tmax, tymax);

		float tzmin = (bmin.z - r.origin.z) * r.inv_direction.z;
		float tzmax = (bmax.z - r.origin.z) * r.inv_direction.z;
		if (tzmin > tzmax) std::swap(tzmin, tzmax);

		tmin = std::fmax(tmin, tzmin);
		tmax = std::fmin(tmax, tzmax);

		tmin = std::fmax(tmin, r.t_min);
		tmax = std::fmin(tmax, r.t_max);

		out_tmin = tmin;
		out_tmax = tmax;
		return tmin <= tmax;
	}

	// Brute-force fallback (no TLAS built)
	Intersection cast_ray_brute(const Ray &world_ray, RayStats *stats) const {
		Intersection closest;
		if (stats) stats->rays_cast++;
		for (const auto &inst : instances_) {
			const MeshBLAS &m = blas_meshes_[inst.blas_id];
			Ray obj_ray = inst.transform_ray_to_object(world_ray);
			obj_ray.t_max = closest.t;
			Intersection hit = m.cast_ray(obj_ray, stats);
			if (hit.hit() && hit.t < closest.t) {
				inst.transform_hit_to_world(hit);
				closest = hit;
			}
		}
		if (stats && closest.hit()) stats->hits++;
		return closest;
	}

	bool any_hit_brute(const Ray &world_ray, RayStats *stats) const {
		if (stats) stats->rays_cast++;
		for (const auto &inst : instances_) {
			const MeshBLAS &m = blas_meshes_[inst.blas_id];
			Ray obj_ray = inst.transform_ray_to_object(world_ray);
			if (m.any_hit(obj_ray, stats)) {
				if (stats) stats->hits++;
				return true;
			}
		}
		return false;
	}
};
