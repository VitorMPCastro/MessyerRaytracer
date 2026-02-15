#pragma once
// ray_packet.h — 4-ray packet for coherent BVH traversal.
//
// WHAT IS A RAY PACKET?
//   Instead of tracing rays one-by-one through the BVH, a "packet" sends
//   4 rays together. At each BVH node, we test all 4 rays against the AABB
//   simultaneously using SIMD. If ALL rays miss the AABB, the subtree is
//   skipped in one check.
//
// WHEN DOES THIS HELP?
//   Coherent rays — rays that have similar origins and directions — tend to
//   enter the same BVH nodes. Primary camera rays, regular grid samples, and
//   audio impulse response rays from the same source are all coherent.
//   For coherent batches, packets reduce AABB tests by ~3–4× versus
//   individual rays.
//
// WHEN DOESN'T IT HELP?
//   Incoherent rays (random bounce directions, point lights with scattered
//   shadow rays) diverge early and gain little from packet traversal.
//   The fallback to per-ray traversal handles these cases.
//
// DESIGN:
//   RayPacket4 stores 4 rays in SoA (Structure-of-Arrays) layout for SIMD:
//     ox = {ray0.origin.x, ray1.origin.x, ray2.origin.x, ray3.origin.x}
//     oy = {ray0.origin.y, ...}, etc.
//
//   Precomputed 1/direction for fast AABB slab tests (avoids per-node division).
//
//   The packet carries:
//     - Ray data in SoA layout
//     - Per-ray closest-t (updated as hits are found, enabling tighter culling)
//     - Active mask (rays that haven't been fully resolved)

#include "core/ray.h"
#include "core/intersection.h"
#include "core/asserts.h"
#include <godot_cpp/variant/aabb.hpp>

// Detect SSE support — must match simd_tri.h
#if defined(_M_X64) || defined(_M_AMD64) || defined(__x86_64__)
	#define RAYTRACER_PACKET_SSE 1
	#ifdef _MSC_VER
		#include <intrin.h>
	#else
		#include <immintrin.h>
	#endif
#else
	#define RAYTRACER_PACKET_SSE 0
#endif

#if RAYTRACER_PACKET_SSE

// ============================================================================
// RayPacket4 — 4 rays stored for SIMD
// ============================================================================
struct RayPacket4 {
	// Origins (SoA)
	__m128 ox, oy, oz;

	// Directions (SoA)
	__m128 dx, dy, dz;

	// 1/direction for AABB slab test (precomputed once to avoid repeated division).
	// Handles infinities correctly — IEEE 754 guarantees 1/0 = ±inf.
	__m128 inv_dx, inv_dy, inv_dz;

	// Per-ray t range
	__m128 t_min;

	// Per-ray current closest-t (shrinks as hits are found → tighter AABB culling).
	__m128 best_t;

	// Build a packet from 1–4 rays. Unused lanes are padded with
	// rays that won't hit anything (origin=0, direction=0, t_min > t_max).
	static RayPacket4 build(const Ray *rays, int count) {
		RT_ASSERT(count >= 1 && count <= 4, "RayPacket4::build: count must be 1-4");
		RT_ASSERT_NOT_NULL(rays);
		RayPacket4 p;

		// Pad unused lanes with sentinel values.
		alignas(16) float aox[4] = {}, aoy[4] = {}, aoz[4] = {};
		alignas(16) float adx[4] = {}, ady[4] = {}, adz[4] = {};
		alignas(16) float atmin[4] = {1.0f, 1.0f, 1.0f, 1.0f};
		alignas(16) float abestt[4] = {-1.0f, -1.0f, -1.0f, -1.0f}; // t_min > best_t → no hit

		for (int i = 0; i < count; i++) {
			aox[i] = rays[i].origin.x;    aoy[i] = rays[i].origin.y;    aoz[i] = rays[i].origin.z;
			adx[i] = rays[i].direction.x;  ady[i] = rays[i].direction.y;  adz[i] = rays[i].direction.z;
			atmin[i] = rays[i].t_min;
			abestt[i] = rays[i].t_max;
		}

		p.ox = _mm_load_ps(aox); p.oy = _mm_load_ps(aoy); p.oz = _mm_load_ps(aoz);
		p.dx = _mm_load_ps(adx); p.dy = _mm_load_ps(ady); p.dz = _mm_load_ps(adz);
		p.t_min = _mm_load_ps(atmin);
		p.best_t = _mm_load_ps(abestt);

		// Precompute 1/direction. If direction=0, IEEE gives ±inf which is correct
		// for the slab test (the slab interval becomes [±inf, ±inf] → empty → miss).
		__m128 one = _mm_set1_ps(1.0f);
		p.inv_dx = _mm_div_ps(one, p.dx);
		p.inv_dy = _mm_div_ps(one, p.dy);
		p.inv_dz = _mm_div_ps(one, p.dz);

		return p;
	}

	// Update best_t for a specific lane from an Intersection result.
	void update_best_t(int lane, float new_t) {
		RT_ASSERT(lane >= 0 && lane < 4, "RayPacket4::update_best_t: lane must be 0-3");
		RT_ASSERT_FINITE(new_t);
		// Use a union to modify one lane without expensive insert intrinsics.
		alignas(16) float vals[4];
		_mm_store_ps(vals, best_t);
		vals[lane] = new_t;
		best_t = _mm_load_ps(vals);
	}
};

// ============================================================================
// Packet AABB test — test 4 rays against one AABB simultaneously
// ============================================================================
//
// Returns a 4-bit mask where bit i is set if ray i intersects the AABB AND
// the entry distance is less than ray i's current best_t.
//
// Uses the slab method with precomputed 1/direction.
inline int packet_intersects_aabb(const RayPacket4 &p,
		const godot::AABB &box) {
	__m128 bmin_x = _mm_set1_ps(static_cast<float>(box.position.x));
	__m128 bmin_y = _mm_set1_ps(static_cast<float>(box.position.y));
	__m128 bmin_z = _mm_set1_ps(static_cast<float>(box.position.z));
	__m128 bmax_x = _mm_set1_ps(static_cast<float>(box.position.x + box.size.x));
	__m128 bmax_y = _mm_set1_ps(static_cast<float>(box.position.y + box.size.y));
	__m128 bmax_z = _mm_set1_ps(static_cast<float>(box.position.z + box.size.z));

	// X axis slabs
	__m128 t1x = _mm_mul_ps(_mm_sub_ps(bmin_x, p.ox), p.inv_dx);
	__m128 t2x = _mm_mul_ps(_mm_sub_ps(bmax_x, p.ox), p.inv_dx);
	__m128 tmin = _mm_min_ps(t1x, t2x);
	__m128 tmax = _mm_max_ps(t1x, t2x);

	// Y axis slabs
	__m128 t1y = _mm_mul_ps(_mm_sub_ps(bmin_y, p.oy), p.inv_dy);
	__m128 t2y = _mm_mul_ps(_mm_sub_ps(bmax_y, p.oy), p.inv_dy);
	tmin = _mm_max_ps(tmin, _mm_min_ps(t1y, t2y));
	tmax = _mm_min_ps(tmax, _mm_max_ps(t1y, t2y));

	// Z axis slabs
	__m128 t1z = _mm_mul_ps(_mm_sub_ps(bmin_z, p.oz), p.inv_dz);
	__m128 t2z = _mm_mul_ps(_mm_sub_ps(bmax_z, p.oz), p.inv_dz);
	tmin = _mm_max_ps(tmin, _mm_min_ps(t1z, t2z));
	tmax = _mm_min_ps(tmax, _mm_max_ps(t1z, t2z));

	// Clamp to ray bounds: tmin >= t_min, tmax <= best_t
	tmin = _mm_max_ps(tmin, p.t_min);
	tmax = _mm_min_ps(tmax, p.best_t);

	// Valid intersection: tmin <= tmax
	return _mm_movemask_ps(_mm_cmple_ps(tmin, tmax));
}

#endif // RAYTRACER_PACKET_SSE
