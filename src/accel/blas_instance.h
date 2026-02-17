#pragma once
// blas_instance.h — Instance of a BLAS with a world-space transform.
//
// An instance is a lightweight reference to a MeshBLAS combined with a
// world-space transform. Many instances can share the same BLAS (instancing).
//
// RAY TRANSFORMATION:
//   To test a world-space ray against an instance:
//   1. Transform the ray INTO object space using inv_transform
//   2. Cast the ray against the instance's BLAS
//   3. Transform the hit position and normal BACK to world space
//
//   This is cheaper than transforming all triangles, especially for
//   instanced meshes where N_instances >> N_unique_meshes.

#include "core/ray.h"
#include "core/intersection.h"
#include "core/asserts.h"
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <cmath>

struct BLASInstance {
	// Index into SceneTLAS::blas_meshes_ array.
	uint32_t blas_id = 0;

	// Object → World transform.
	godot::Transform3D transform;

	// World → Object transform (cached for ray transformation).
	// Must be kept in sync with 'transform'. Call update_inverse() after changing.
	godot::Transform3D inv_transform;

	// AABB of the instance in WORLD space (for TLAS BVH construction).
	// Computed from the BLAS object-space bounds + the transform.
	godot::AABB world_bounds;

	// Unique instance ID (assigned by SceneTLAS).
	uint32_t instance_id = 0;

	// Update cached inverse transform. Call after modifying 'transform'.
	void update_inverse() {
		inv_transform = transform.affine_inverse();
	}

	// Transform a world-space ray into this instance's object space.
	Ray transform_ray_to_object(const Ray &world_ray) const {
		RT_ASSERT_VALID_RAY(world_ray);
		Ray obj;
		obj.origin = inv_transform.xform(world_ray.origin);
		// Direction is transformed by the basis only (not translation).
		// Do NOT normalized — we need the scaling factor for correct t values.
		obj.direction = inv_transform.basis.xform(world_ray.direction);
		obj.t_min = world_ray.t_min;
		obj.t_max = world_ray.t_max;
		RT_ASSERT_VALID_RAY(obj);
		return obj;
	}

	// Transform a hit result from object space back to world space.
	void transform_hit_to_world(Intersection &hit) const {
		if (!hit.hit()) { return; }
		RT_ASSERT(hit.t >= 0.0f, "Hit t must be non-negative before world transform");
		hit.position = transform.xform(hit.position);
		// Normal is transformed by the transpose of the inverse of the upper-left 3x3.
		// For uniform scaling, this simplifies to basis.xform(normal).normalized().
		hit.normal = transform.basis.xform(hit.normal).normalized();
		RT_ASSERT(hit.normal.length_squared() > 0.0f, "Normal must not be degenerate after world transform");
	}

	// Compute world_bounds from a given object-space AABB.
	// Call after setting transform + the BLAS's object_bounds.
	void compute_world_bounds(const godot::AABB &object_aabb) {
		RT_ASSERT(object_aabb.size.x >= 0.0f && object_aabb.size.y >= 0.0f && object_aabb.size.z >= 0.0f,
			"Object AABB must have non-negative size");
		// Transform all 8 corners of the object AABB and build a world AABB.
		Vector3 corners[8];
		Vector3 pos = object_aabb.position;
		Vector3 sz = object_aabb.size;

		corners[0] = pos;
		corners[1] = pos + Vector3(sz.x, 0, 0);
		corners[2] = pos + Vector3(0, sz.y, 0);
		corners[3] = pos + Vector3(0, 0, sz.z);
		corners[4] = pos + Vector3(sz.x, sz.y, 0);
		corners[5] = pos + Vector3(sz.x, 0, sz.z);
		corners[6] = pos + Vector3(0, sz.y, sz.z);
		corners[7] = pos + sz;

		Vector3 wmin = transform.xform(corners[0]);
		Vector3 wmax = wmin;

		for (int i = 1; i < 8; i++) {
			Vector3 wc = transform.xform(corners[i]);
			wmin.x = std::fmin(wmin.x, wc.x);
			wmin.y = std::fmin(wmin.y, wc.y);
			wmin.z = std::fmin(wmin.z, wc.z);
			wmax.x = std::fmax(wmax.x, wc.x);
			wmax.y = std::fmax(wmax.y, wc.y);
			wmax.z = std::fmax(wmax.z, wc.z);
		}

		world_bounds = godot::AABB(wmin, wmax - wmin);
		RT_ASSERT(world_bounds.size.x >= 0.0f && world_bounds.size.y >= 0.0f && world_bounds.size.z >= 0.0f,
			"Computed world bounds must have non-negative size");
	}
};
