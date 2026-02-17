#pragma once
// ray_sort.h — Sort rays by direction for BVH traversal coherence.
//
// WHY SORT RAYS?
//   Rays with similar directions traverse the same BVH nodes. If we sort
//   rays by direction before dispatch:
//     - GPU: neighboring threads in a warp follow the same path → less divergence
//     - CPU: ray packets contain truly coherent rays → more AABB culling
//
// HOW IT WORKS:
//   1. Quantize each ray's direction to a 3D grid (10 bits per axis)
//   2. Compute a 30-bit Morton code by interleaving the bits
//   3. Sort indices by Morton code (std::sort is sufficient — O(N log N))
//   4. Caller reorders rays using the sorted indices
//   5. After dispatch, results are unshuffled back to original order
//
// MORTON CODES:
//   A Morton code (Z-order curve) interleaves the bits of x, y, z coordinates:
//     x = 101, y = 011, z = 110  →  Morton = 101_011_110 → 100_010_111_011_010
//   This maps 3D proximity to 1D proximity — nearby directions get nearby codes.
//
// COST:
//   Sorting N rays is O(N log N). For 10K rays this is ~0.1ms on a modern CPU.
//   The traversal savings typically dwarf the sort cost for N > 1000.

#include "core/ray.h"
#include "core/intersection.h"
#include "core/asserts.h"
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cmath>

// ============================================================================
// Morton code helpers
// ============================================================================

// "Spread" a 10-bit integer to 30 bits by inserting 2 zero bits between each bit.
// Example: 0b1101 → 0b001_001_000_001
// This is step 1 of computing a 3D Morton code.
inline uint32_t morton_spread_10(uint32_t v) {
	RT_ASSERT((v & ~0x3FFu) == 0, "morton_spread_10: input must fit in 10 bits");
	v &= 0x000003FFu;                      // mask to 10 bits
	v = (v | (v << 16)) & 0x030000FFu;     // ---- ----  ---- ---- 0000 00-- ---- ----
	v = (v | (v <<  8)) & 0x0300F00Fu;     // 0000 00-- 0000 ---- 0000 00-- 0000 ----
	v = (v | (v <<  4)) & 0x030C30C3u;     // 00-- 00-- 00-- 00-- 00-- 00-- 00-- 00--
	v = (v | (v <<  2)) & 0x09249249u;     // 0--0 --0- -0-- 0--0 --0- -0-- 0--0 --0-
	RT_ASSERT((v & ~0x09249249u) == 0, "morton_spread_10: output bit pattern invalid");
	return v;
}

// Compute 30-bit 3D Morton code from three 10-bit coordinates.
inline uint32_t morton_encode_3d(uint32_t x, uint32_t y, uint32_t z) {
	return (morton_spread_10(x) << 2) |
	       (morton_spread_10(y) << 1) |
	        morton_spread_10(z);
}

// ============================================================================
// Compute Morton code for a ray direction
// ============================================================================
// Maps direction vector [-1,1]³ → [0,1023]³ → 30-bit Morton code.

inline uint32_t ray_direction_morton(const Vector3 &dir) {
	RT_ASSERT(dir.is_finite(), "ray_direction_morton: direction must be finite");
	RT_ASSERT(dir.length_squared() > 0.0f, "ray_direction_morton: direction must not be zero");
	// Map [-1, 1] → [0, 1023] (10 bits).
	// Clamp for safety (unit vectors should already be in range).
	auto quantize = [](float v) -> uint32_t {
		float n = (v + 1.0f) * 0.5f; // [-1,1] → [0,1]
		n = std::fmax(0.0f, std::fmin(1.0f, n));
		return static_cast<uint32_t>(n * 1023.0f);
	};

	return morton_encode_3d(quantize(dir.x), quantize(dir.y), quantize(dir.z));
}

// ============================================================================
// Sort ray indices by direction coherence
// ============================================================================

// Generates a permutation table that sorts rays by direction Morton code.
// sorted_indices[i] = index of the ray that should be in position i after sorting.
//
// Does NOT modify the ray array — caller applies the permutation.
// This allows sorting once and applying to both ray and result arrays.
inline void sort_rays_by_direction(const Ray *rays, int count,
		std::vector<uint32_t> &sorted_indices) {
	RT_ASSERT(count >= 0, "sort_rays_by_direction: count must be non-negative");
	RT_ASSERT(count == 0 || rays != nullptr, "sort_rays_by_direction: rays is null");
	// Compute Morton codes
	struct IndexCode {
		uint32_t index;
		uint32_t morton;
	};
	std::vector<IndexCode> entries(count);
	for (int i = 0; i < count; i++) {
		entries[i] = { static_cast<uint32_t>(i),
				ray_direction_morton(rays[i].direction) };
	}

	// Sort by Morton code (O(N log N))
	std::sort(entries.begin(), entries.end(),
			[](const IndexCode &a, const IndexCode &b) {
				return a.morton < b.morton;
			});

	// Output the permutation
	sorted_indices.resize(count);
	for (int i = 0; i < count; i++) {
		sorted_indices[i] = entries[i].index;
	}
}

// ============================================================================
// Apply / reverse a permutation
// ============================================================================

// Reorder rays into a new array according to sorted_indices.
// sorted_rays[i] = rays[sorted_indices[i]]
inline void apply_ray_permutation(const Ray *rays, const std::vector<uint32_t> &sorted_indices,
		std::vector<Ray> &sorted_rays) {
	RT_ASSERT_NOT_NULL(rays);
	RT_ASSERT(!sorted_indices.empty(), "apply_ray_permutation: empty permutation");
	sorted_rays.resize(sorted_indices.size());
	for (size_t i = 0; i < sorted_indices.size(); i++) {
		sorted_rays[i] = rays[sorted_indices[i]];
	}
}

// Unshuffle results back to original ray order.
// original_results[sorted_indices[i]] = sorted_results[i]
inline void unshuffle_intersections(const Intersection *sorted_results,
		const std::vector<uint32_t> &sorted_indices,
		Intersection *original_results) {
	RT_ASSERT_NOT_NULL(sorted_results);
	RT_ASSERT_NOT_NULL(original_results);
	for (size_t i = 0; i < sorted_indices.size(); i++) {
		original_results[sorted_indices[i]] = sorted_results[i];
	}
}

// Unshuffle bool results (for any_hit).
inline void unshuffle_bools(const bool *sorted_results,
		const std::vector<uint32_t> &sorted_indices,
		bool *original_results) {
	RT_ASSERT_NOT_NULL(sorted_results);
	RT_ASSERT_NOT_NULL(original_results);
	for (size_t i = 0; i < sorted_indices.size(); i++) {
		original_results[sorted_indices[i]] = sorted_results[i];
	}
}
