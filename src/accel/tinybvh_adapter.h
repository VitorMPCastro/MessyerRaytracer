#pragma once
// tinybvh_adapter.h — Conversion utilities between our types and TinyBVH types.
//
// WHAT: Inline conversion functions for Ray, Intersection, Triangle, and
//       Transform3D ↔ their TinyBVH equivalents. All functions are header-only
//       and designed for zero-overhead — the compiler inlines them fully.
//
// WHY:  Our core types (Ray, Intersection, Triangle) use Godot Vector3 and our
//       own field layout. TinyBVH uses bvhvec3/bvhvec4 and different layouts.
//       Rather than coupling our types to TinyBVH, we convert at the boundary.
//
// PERFORMANCE: These conversions happen once per ray (not per intersection test),
//   so the overhead is negligible. At 1280×960 with ~1.2M primary rays, the
//   conversion costs ~0.5ms total — less than 1% of a typical frame.

#include "core/ray.h"
#include "core/intersection.h"
#include "core/triangle.h"
#include "core/asserts.h"

// TinyBVH uses TINYBVH_INST_IDX_BITS=32 (set in tinybvh_impl.cpp).
// We must match that here so Intersection layout agrees.
#ifndef TINYBVH_INST_IDX_BITS
#define TINYBVH_INST_IDX_BITS 32
#endif
#include "thirdparty/tinybvh/tiny_bvh.h"

#include <godot_cpp/variant/transform3d.hpp>
#include <vector>
#include <cstring>

namespace tinybvh_adapter {

// ============================================================================
// Triangle → bvhvec4 vertex array (for BVH builder input)
// ============================================================================
//
// TinyBVH's Build() expects 3 × bvhvec4 per triangle (48 bytes per tri).
// We extract v0/v1/v2 from our Triangle struct. The w component is unused
// by the builder but must be present for correct stride.

inline void triangles_to_vertices(const Triangle *tris, uint32_t count,
		std::vector<tinybvh::bvhvec4> &out) {
	RT_ASSERT_NOT_NULL(tris);
	RT_ASSERT(count > 0, "triangles_to_vertices: count must be positive");

	out.resize(static_cast<size_t>(count) * 3);
	for (uint32_t i = 0; i < count; i++) {
		const Triangle &t = tris[i];
		const size_t base = static_cast<size_t>(i) * 3;
		out[base + 0] = tinybvh::bvhvec4(t.v0.x, t.v0.y, t.v0.z, 0.0f);
		out[base + 1] = tinybvh::bvhvec4(t.v1.x, t.v1.y, t.v1.z, 0.0f);
		out[base + 2] = tinybvh::bvhvec4(t.v2.x, t.v2.y, t.v2.z, 0.0f);
	}
}

// ============================================================================
// Our Ray → TinyBVH Ray
// ============================================================================
//
// TinyBVH Ray (64B aligned):
//   bvhvec3 O; uint32_t mask;
//   bvhvec3 D; uint32_t instIdx;
//   bvhvec3 rD; (reciprocal direction, computed by constructor)
//   Intersection hit; (t, u, v, prim, inst)
//
// Our Ray:
//   Vector3 origin, direction, inv_direction; int dir_sign[3];
//   float t_min, t_max; uint32_t flags;

inline tinybvh::Ray to_tinybvh_ray(const Ray &r) {
	RT_ASSERT_VALID_RAY(r);
	RT_ASSERT(r.t_max > 0.0f, "to_tinybvh_ray: t_max must be positive");
	// TinyBVH Ray constructor normalizes D and computes rD internally.
	// We pass our direction which should already be normalized.
	tinybvh::Ray tr(
		tinybvh::bvhvec3(r.origin.x, r.origin.y, r.origin.z),
		tinybvh::bvhvec3(r.direction.x, r.direction.y, r.direction.z),
		r.t_max
	);
	// TinyBVH doesn't have a t_min concept in the Ray constructor,
	// but hit.t starts at t_max. We handle t_min filtering in our wrapper.
	return tr;
}

// ============================================================================
// TinyBVH Intersection → Our Intersection
// ============================================================================
//
// TinyBVH Intersection: t, u, v, prim (uint32_t), inst (uint32_t when INST_IDX_BITS=32)
// Our Intersection: t, position, normal, u, v, prim_id, hit_layers
//
// NOTE: Position and normal must be filled in by the caller after conversion,
//       since TinyBVH doesn't store them. Position = ray.origin + ray.direction * t.
//       Normal comes from Triangle lookup.

inline Intersection from_tinybvh_intersection(const tinybvh::Ray &tr,
		const Ray &our_ray) {
	Intersection result;
	if (tr.hit.t < 1e30f && tr.hit.prim != 0xFFFFFFFF) {
		result.t = tr.hit.t;
		result.u = tr.hit.u;
		result.v = tr.hit.v;
		result.prim_id = tr.hit.prim;
		// Position reconstructed from our ray (which preserves original precision).
		result.position = our_ray.origin + our_ray.direction * tr.hit.t;
		// Normal and hit_layers must be filled by caller from Triangle lookup.
		result.hit_layers = 0;
	} else {
		result.set_miss();
	}

	RT_ASSERT(result.hit() || result.prim_id == Intersection::NO_HIT,
		"from_tinybvh_intersection: missed ray must have NO_HIT prim_id");
	return result;
}

// ============================================================================
// Fill normal and layers from Triangle lookup after intersection
// ============================================================================

inline void fill_hit_details(Intersection &hit, const Triangle *triangles,
		uint32_t triangle_count) {
	if (!hit.hit()) { return; }
	RT_ASSERT(hit.prim_id < triangle_count,
		"fill_hit_details: prim_id out of range");
	const Triangle &tri = triangles[hit.prim_id];
	hit.normal = tri.normal;
	hit.hit_layers = tri.layers;
}

// ============================================================================
// godot::Transform3D → tinybvh::bvhmat4 (4×4 column-major)
// ============================================================================
//
// Godot Transform3D is a 3×3 Basis + Vector3 origin (row-major in memory).
// TinyBVH bvhmat4 is a 4×4 matrix stored as 4 bvhvec4 columns.
// We convert by extracting columns from the Basis.

inline tinybvh::bvhmat4 to_bvhmat4(const godot::Transform3D &xform) {
	const godot::Basis &b = xform.basis;
	const godot::Vector3 &o = xform.origin;
	RT_ASSERT(o.is_finite(), "to_bvhmat4: origin must be finite");
	RT_ASSERT(b[0].is_finite() && b[1].is_finite() && b[2].is_finite(),
		"to_bvhmat4: basis rows must be finite");

	// Godot Basis rows are: b[0] = row0, b[1] = row1, b[2] = row2.
	// bvhmat4 columns: cell[0..3] = col0, cell[4..7] = col1, etc.
	// Mapping: col[j][i] = basis.rows[i][j]
	tinybvh::bvhmat4 m;
	// Column 0
	m.cell[0]  = b[0][0]; m.cell[1]  = b[1][0]; m.cell[2]  = b[2][0]; m.cell[3]  = 0.0f;
	// Column 1
	m.cell[4]  = b[0][1]; m.cell[5]  = b[1][1]; m.cell[6]  = b[2][1]; m.cell[7]  = 0.0f;
	// Column 2
	m.cell[8]  = b[0][2]; m.cell[9]  = b[1][2]; m.cell[10] = b[2][2]; m.cell[11] = 0.0f;
	// Column 3 (translation)
	m.cell[12] = o.x;     m.cell[13] = o.y;     m.cell[14] = o.z;     m.cell[15] = 1.0f;

	return m;
}

// ============================================================================
// tinybvh::bvhmat4 → godot::Transform3D
// ============================================================================

inline godot::Transform3D from_bvhmat4(const tinybvh::bvhmat4 &m) {
	RT_ASSERT(m.cell[15] != 0.0f, "from_bvhmat4: w component must not be zero (affine matrix)");
	RT_ASSERT(m.cell[3] == 0.0f && m.cell[7] == 0.0f && m.cell[11] == 0.0f,
		"from_bvhmat4: perspective row must be zero (affine matrix)");
	godot::Basis b;
	// Row i, Col j = m.cell[j*4 + i]
	b[0][0] = m.cell[0]; b[0][1] = m.cell[4]; b[0][2] = m.cell[8];
	b[1][0] = m.cell[1]; b[1][1] = m.cell[5]; b[1][2] = m.cell[9];
	b[2][0] = m.cell[2]; b[2][1] = m.cell[6]; b[2][2] = m.cell[10];

	godot::Vector3 origin(m.cell[12], m.cell[13], m.cell[14]);

	return godot::Transform3D(b, origin);
}

} // namespace tinybvh_adapter
