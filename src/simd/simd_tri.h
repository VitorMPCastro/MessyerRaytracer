#pragma once
// simd_tri.h — SIMD-accelerated triangle intersection (SSE 4.1).
//
// Tests 1–4 triangles simultaneously against a single ray using the
// Möller-Trumbore algorithm with 128-bit SSE intrinsics.
//
// WHEN DOES THIS HELP?
//   Each BVH leaf contains up to MAX_LEAF_SIZE=4 triangles.
//   The scalar code tests them sequentially (4 full Möller-Trumbore passes).
//   The SIMD code tests all 4 in parallel — same arithmetic, 4× throughput.
//
// HOW IT WORKS:
//   1. Gather triangle edge/vertex data from AoS layout into SoA SSE registers
//   2. Compute cross products, dot products, determinants for all 4 simultaneously
//   3. Build a validity mask (determinant ok, barycentric coords in range, t in range)
//   4. Find the closest valid hit using horizontal min
//
// PLATFORM SUPPORT:
//   - x86_64 (Windows/Linux): SSE 4.1 (available on all x86_64 CPUs since ~2008)
//   - ARM/macOS: Falls back to scalar code (NEON version could be added later)
//   - The fallback is identical in behavior — just slower.
//
// MEMORY NOTE:
//   Gathering from AoS (Triangle structs) into SoA (SSE registers) costs
//   ~36 scalar loads for 4 triangles. With pre-packed SoA data this could
//   be reduced to 9 aligned SIMD loads, but the AoS approach is simpler
//   and the arithmetic savings dominate for non-trivial BVH depths.

#include "core/ray.h"
#include "core/intersection.h"
#include "core/triangle.h"
#include "core/asserts.h"

// Detect SSE support
#if defined(_M_X64) || defined(_M_AMD64) || defined(__x86_64__)
	#define RAYTRACER_SIMD_SSE 1
	#ifdef _MSC_VER
		#include <intrin.h>
	#else
		#include <immintrin.h>
	#endif
#else
	#define RAYTRACER_SIMD_SSE 0
#endif

#if RAYTRACER_SIMD_SSE

// ============================================================================
// SSE helper: 3-component cross product on packed 4-wide vectors
// ============================================================================
// cross(a, b) = (ay*bz - az*by, az*bx - ax*bz, ax*by - ay*bx)
// Each parameter represents one component across 4 "lanes" (e.g., ax = {a0.x, a1.x, a2.x, a3.x})
static inline void simd_cross(
		__m128 ax, __m128 ay, __m128 az,
		__m128 bx, __m128 by, __m128 bz,
		__m128 &rx, __m128 &ry, __m128 &rz) {
	rx = _mm_sub_ps(_mm_mul_ps(ay, bz), _mm_mul_ps(az, by));
	ry = _mm_sub_ps(_mm_mul_ps(az, bx), _mm_mul_ps(ax, bz));
	rz = _mm_sub_ps(_mm_mul_ps(ax, by), _mm_mul_ps(ay, bx));
}

// ============================================================================
// SSE helper: dot product on packed 4-wide vectors
// ============================================================================
static inline __m128 simd_dot(
		__m128 ax, __m128 ay, __m128 az,
		__m128 bx, __m128 by, __m128 bz) {
	return _mm_add_ps(_mm_add_ps(
			_mm_mul_ps(ax, bx), _mm_mul_ps(ay, by)), _mm_mul_ps(az, bz));
}

// ============================================================================
// Test up to 4 triangles against one ray (nearest hit, SSE 4.1)
// ============================================================================
//
// 'tris' must point to at least 'count' contiguous Triangle objects.
// 'count' must be 1–4. Unused lanes are padded with zeros (degenerate = no hit).
//
// Updates 'out' if any triangle is closer than the current best hit.
// Returns true if 'out' was updated.
inline bool simd_intersect_nearest(const Ray &r, const Triangle *tris, int count,
		Intersection &out) {
	RT_ASSERT(count >= 1 && count <= 4, "SIMD nearest: count must be 1-4");
	RT_ASSERT_NOT_NULL(tris);
	RT_ASSERT_VALID_RAY(r);
	// Broadcast ray data to all 4 lanes.
	__m128 ox = _mm_set1_ps(r.origin.x);
	__m128 oy = _mm_set1_ps(r.origin.y);
	__m128 oz = _mm_set1_ps(r.origin.z);
	__m128 dx = _mm_set1_ps(r.direction.x);
	__m128 dy = _mm_set1_ps(r.direction.y);
	__m128 dz = _mm_set1_ps(r.direction.z);
	__m128 ray_tmin = _mm_set1_ps(r.t_min);
	__m128 best_t = _mm_set1_ps(out.t);

	// ---- Gather AoS → SoA ----
	// Load triangle edges and v0 into local arrays, pad unused lanes with 0.
	// Zero-edge triangles have det=0 and are rejected by the epsilon check.
	alignas(16) float ae1x[4] = {}, ae1y[4] = {}, ae1z[4] = {};
	alignas(16) float ae2x[4] = {}, ae2y[4] = {}, ae2z[4] = {};
	alignas(16) float av0x[4] = {}, av0y[4] = {}, av0z[4] = {};

	for (int i = 0; i < count; i++) {
		ae1x[i] = tris[i].edge1.x; ae1y[i] = tris[i].edge1.y; ae1z[i] = tris[i].edge1.z;
		ae2x[i] = tris[i].edge2.x; ae2y[i] = tris[i].edge2.y; ae2z[i] = tris[i].edge2.z;
		av0x[i] = tris[i].v0.x;    av0y[i] = tris[i].v0.y;    av0z[i] = tris[i].v0.z;
	}

	// NOLINTBEGIN(readability-identifier-naming) — SIMD math uses standard notation
	__m128 E1x = _mm_load_ps(ae1x), E1y = _mm_load_ps(ae1y), E1z = _mm_load_ps(ae1z);
	__m128 E2x = _mm_load_ps(ae2x), E2y = _mm_load_ps(ae2y), E2z = _mm_load_ps(ae2z);
	__m128 V0x = _mm_load_ps(av0x), V0y = _mm_load_ps(av0y), V0z = _mm_load_ps(av0z);
	// NOLINTEND(readability-identifier-naming)

	// ---- Step 1: pvec = direction × edge2 ----
	__m128 px, py, pz;
	simd_cross(dx, dy, dz, E2x, E2y, E2z, px, py, pz);

	// ---- Step 2: determinant = edge1 · pvec ----
	__m128 det = simd_dot(E1x, E1y, E1z, px, py, pz);

	// Reject near-parallel triangles (|det| < epsilon).
	__m128 eps = _mm_set1_ps(1e-8f);
	__m128 abs_det = _mm_andnot_ps(_mm_set1_ps(-0.0f), det); // fabs via sign-bit clear
	__m128 valid = _mm_cmpgt_ps(abs_det, eps);

	__m128 inv_det = _mm_div_ps(_mm_set1_ps(1.0f), det);

	// ---- Step 3: barycentric u ----
	__m128 tx = _mm_sub_ps(ox, V0x);
	__m128 ty = _mm_sub_ps(oy, V0y);
	__m128 tz = _mm_sub_ps(oz, V0z);

	__m128 u = _mm_mul_ps(simd_dot(tx, ty, tz, px, py, pz), inv_det);
	valid = _mm_and_ps(valid, _mm_cmpge_ps(u, _mm_setzero_ps()));
	valid = _mm_and_ps(valid, _mm_cmple_ps(u, _mm_set1_ps(1.0f)));

	// ---- Step 4: barycentric v ----
	__m128 qx, qy, qz;
	simd_cross(tx, ty, tz, E1x, E1y, E1z, qx, qy, qz);

	__m128 v = _mm_mul_ps(simd_dot(dx, dy, dz, qx, qy, qz), inv_det);
	valid = _mm_and_ps(valid, _mm_cmpge_ps(v, _mm_setzero_ps()));
	valid = _mm_and_ps(valid, _mm_cmple_ps(_mm_add_ps(u, v), _mm_set1_ps(1.0f)));

	// ---- Step 5: intersection distance t ----
	__m128 t = _mm_mul_ps(simd_dot(E2x, E2y, E2z, qx, qy, qz), inv_det);
	valid = _mm_and_ps(valid, _mm_cmpge_ps(t, ray_tmin));
	valid = _mm_and_ps(valid, _mm_cmplt_ps(t, best_t));

	// ---- Find closest valid hit ----
	int mask = _mm_movemask_ps(valid);
	if (mask == 0) { return false; }

	// Extract t values, pick the smallest valid lane.
	// Masked lanes are set to a large value so they lose the comparison.
	__m128 big = _mm_set1_ps(1e30f);
	__m128 masked_t = _mm_blendv_ps(big, t, valid); // SSE 4.1

	alignas(16) float t_vals[4];
	_mm_store_ps(t_vals, masked_t);

	int best_lane = -1;
	float lane_t = out.t;
	for (int i = 0; i < count; i++) {
		if ((mask & (1 << i)) && t_vals[i] < lane_t) {
			lane_t = t_vals[i];
			best_lane = i;
		}
	}

	if (best_lane >= 0) {
		out.t = lane_t;
		out.position = r.at(lane_t);
		out.normal = tris[best_lane].normal;
		out.prim_id = tris[best_lane].id;
		return true;
	}

	return false;
}

// ============================================================================
// Test up to 4 triangles against one ray (any hit, SSE 4.1)
// ============================================================================
//
// Returns true as soon as ANY valid intersection is found.
// Skips the "find closest" step — ideal for shadow/occlusion queries.
inline bool simd_intersect_any(const Ray &r, const Triangle *tris, int count) {
	RT_ASSERT(count >= 1 && count <= 4, "SIMD any: count must be 1-4");
	RT_ASSERT_NOT_NULL(tris);
	RT_ASSERT_VALID_RAY(r);
	__m128 ox = _mm_set1_ps(r.origin.x);
	__m128 oy = _mm_set1_ps(r.origin.y);
	__m128 oz = _mm_set1_ps(r.origin.z);
	__m128 dx = _mm_set1_ps(r.direction.x);
	__m128 dy = _mm_set1_ps(r.direction.y);
	__m128 dz = _mm_set1_ps(r.direction.z);
	__m128 ray_tmin = _mm_set1_ps(r.t_min);
	__m128 ray_tmax = _mm_set1_ps(r.t_max);

	alignas(16) float ae1x[4] = {}, ae1y[4] = {}, ae1z[4] = {};
	alignas(16) float ae2x[4] = {}, ae2y[4] = {}, ae2z[4] = {};
	alignas(16) float av0x[4] = {}, av0y[4] = {}, av0z[4] = {};

	for (int i = 0; i < count; i++) {
		ae1x[i] = tris[i].edge1.x; ae1y[i] = tris[i].edge1.y; ae1z[i] = tris[i].edge1.z;
		ae2x[i] = tris[i].edge2.x; ae2y[i] = tris[i].edge2.y; ae2z[i] = tris[i].edge2.z;
		av0x[i] = tris[i].v0.x;    av0y[i] = tris[i].v0.y;    av0z[i] = tris[i].v0.z;
	}

	// NOLINTBEGIN(readability-identifier-naming) — SIMD math uses standard notation
	__m128 E1x = _mm_load_ps(ae1x), E1y = _mm_load_ps(ae1y), E1z = _mm_load_ps(ae1z);
	__m128 E2x = _mm_load_ps(ae2x), E2y = _mm_load_ps(ae2y), E2z = _mm_load_ps(ae2z);
	__m128 V0x = _mm_load_ps(av0x), V0y = _mm_load_ps(av0y), V0z = _mm_load_ps(av0z);
	// NOLINTEND(readability-identifier-naming)

	__m128 px, py, pz;
	simd_cross(dx, dy, dz, E2x, E2y, E2z, px, py, pz);

	__m128 det = simd_dot(E1x, E1y, E1z, px, py, pz);
	__m128 eps = _mm_set1_ps(1e-8f);
	__m128 abs_det = _mm_andnot_ps(_mm_set1_ps(-0.0f), det);
	__m128 valid = _mm_cmpgt_ps(abs_det, eps);

	__m128 inv_det = _mm_div_ps(_mm_set1_ps(1.0f), det);

	__m128 tx = _mm_sub_ps(ox, V0x);
	__m128 ty = _mm_sub_ps(oy, V0y);
	__m128 tz = _mm_sub_ps(oz, V0z);

	__m128 u = _mm_mul_ps(simd_dot(tx, ty, tz, px, py, pz), inv_det);
	valid = _mm_and_ps(valid, _mm_cmpge_ps(u, _mm_setzero_ps()));
	valid = _mm_and_ps(valid, _mm_cmple_ps(u, _mm_set1_ps(1.0f)));

	__m128 qx, qy, qz;
	simd_cross(tx, ty, tz, E1x, E1y, E1z, qx, qy, qz);

	__m128 v = _mm_mul_ps(simd_dot(dx, dy, dz, qx, qy, qz), inv_det);
	valid = _mm_and_ps(valid, _mm_cmpge_ps(v, _mm_setzero_ps()));
	valid = _mm_and_ps(valid, _mm_cmple_ps(_mm_add_ps(u, v), _mm_set1_ps(1.0f)));

	__m128 t = _mm_mul_ps(simd_dot(E2x, E2y, E2z, qx, qy, qz), inv_det);
	valid = _mm_and_ps(valid, _mm_cmpge_ps(t, ray_tmin));
	valid = _mm_and_ps(valid, _mm_cmplt_ps(t, ray_tmax));

	return _mm_movemask_ps(valid) != 0;
}

#endif // RAYTRACER_SIMD_SSE

// ============================================================================
// Dispatch: picks SIMD or scalar based on platform
// ============================================================================

// Nearest-hit leaf test. Tests up to 4 contiguous triangles against a ray.
// Returns true if a closer hit was found (out is updated).
inline bool intersect_leaf_nearest(const Ray &r, const Triangle *tris, int count,
		Intersection &out) {
	RT_ASSERT(count >= 1 && count <= 4, "Leaf nearest: count must be 1-4");
	RT_ASSERT_NOT_NULL(tris);
#if RAYTRACER_SIMD_SSE
	return simd_intersect_nearest(r, tris, count, out);
#else
	// Scalar fallback — identical behavior, just slower.
	bool any_closer = false;
	for (int i = 0; i < count; i++) {
		if (tris[i].intersect(r, out)) {
			any_closer = true;
		}
	}
	return any_closer;
#endif
}

// Any-hit leaf test. Returns true if ANY triangle is hit.
inline bool intersect_leaf_any(const Ray &r, const Triangle *tris, int count) {
	RT_ASSERT(count >= 1 && count <= 4, "Leaf any: count must be 1-4");
	RT_ASSERT_NOT_NULL(tris);
#if RAYTRACER_SIMD_SSE
	return simd_intersect_any(r, tris, count);
#else
	Intersection temp;
	for (int i = 0; i < count; i++) {
		if (tris[i].intersect(r, temp)) return true;
	}
	return false;
#endif
}
