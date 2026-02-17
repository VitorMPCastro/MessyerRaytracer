#pragma once
// gpu_structs.h — BVH-specific GPU structures + re-export of shared GPU types.
//
// Shared GPU types (triangle, ray, intersection, push constants) live in
// api/gpu_types.h so that modules can include them without depending on gpu/.
// This header adds BVH-specific node formats used by GPURayCaster and
// compute shaders, and re-exports the shared types for convenience.
//
// IMPORTANT: BVH node formats will change in Phase 2 (TinyBVH integration).
// Only internal code (dispatch/, gpu/, godot/) should include this header.
// Module code should include api/gpu_types.h directly.

#include "api/gpu_types.h"

// ============================================================================
// GPU BVH Node — 32 bytes (standard format, used by rt_compositor_base)
// ============================================================================
//
// GLSL layout:
//   struct GPUBVHNode {
//       vec3 bounds_min; uint left_first;  // offset  0–15
//       vec3 bounds_max; uint count;       // offset 16–31
//   };
//
// Encoding:
//   count == 0: internal node. left_first = index of left child.
//               Right child is at left_first + 1 (implicit DFS ordering).
//   count >  0: leaf node. left_first = first triangle index. count = tri count.
struct GPUBVHNodePacked {
	float bounds_min[3]; uint32_t left_first;
	float bounds_max[3]; uint32_t count;
};
static_assert(sizeof(GPUBVHNodePacked) == 32, "GPUBVHNodePacked must be 32 bytes (std430)");

// ============================================================================
// GPU BVH Node — Aila-Laine format — 64 bytes
// ============================================================================
//
// Based on "Understanding the Efficiency of Ray Traversal on GPUs"
// (Aila & Laine, HPG 2009) and tinybvh's BVH_GPU layout.
//
// KEY INSIGHT: Store BOTH children's AABBs in the parent node.
// This means traversal needs only ONE memory fetch per step instead of TWO
// (no need to follow a pointer to the child just to read its AABB).
// On GPU, this halves memory latency per traversal step — the #1 bottleneck.
//
// GLSL layout:
//   struct GPUBVHNodeWide {
//       vec3 left_min;  uint left_idx;   // offset  0–15
//       vec3 left_max;  uint right_idx;  // offset 16–31
//       vec3 right_min; uint left_count; // offset 32–47
//       vec3 right_max; uint right_count;// offset 48–63
//   };
//
// Encoding:
//   count == 0: child is INTERNAL. *_idx = index into this node array.
//   count >  0: child is LEAF. *_idx = first triangle index. *_count = tri count.
struct GPUBVHNodeWide {
	float left_min[3];  uint32_t left_idx;
	float left_max[3];  uint32_t right_idx;
	float right_min[3]; uint32_t left_count;
	float right_max[3]; uint32_t right_count;
};
static_assert(sizeof(GPUBVHNodeWide) == 64, "GPUBVHNodeWide must be 64 bytes (std430)");
