#pragma once
// gpu_structs.h — GPU-compatible packed structures matching GLSL std430 layout.
//
// These C++ structs MUST exactly match the GLSL struct layouts in
// bvh_traverse_comp.h. Any mismatch causes SILENT data corruption —
// the GPU reads garbage and produces wrong results with no error message.
//
// WHY explicit padding?
//   In std430 layout, vec3 has 16-byte alignment but only 12 bytes of data.
//   A scalar (float/uint) following vec3 fills the 4-byte gap perfectly,
//   making each vec3+scalar pair exactly 16 bytes — same as vec4.
//
// MEMORY MAP (verified against GLSL std430 rules):
//   GPUTrianglePacked:     64 bytes  (4 × vec3+scalar)
//   GPUBVHNodePacked:      32 bytes  (2 × vec3+uint)
//   GPURayPacked:          32 bytes  (2 × vec3+float)
//   GPUIntersectionPacked: 32 bytes  (2 × vec3+scalar)
//
// To verify alignment, each struct has a static_assert on its size.

#include <cstdint>

#include <godot_cpp/variant/vector3.hpp>

// Forward declarations — full definitions in ray/*.h
struct Ray;
struct Triangle;
struct Intersection;
struct BVHNode;

// ============================================================================
// GPU Triangle — 64 bytes
// ============================================================================
//
// GLSL layout:
//   struct GPUTriangle {
//       vec3 v0;     uint id;       // offset  0–15
//       vec3 edge1;  uint layers;   // offset 16–31
//       vec3 edge2;  float _pad2;   // offset 32–47
//       vec3 normal; float _pad3;   // offset 48–63
//   };
//
// The 'id' field replaces what would otherwise be dead padding after v0.
// It stores the triangle's original ID so the GPU can report which primitive
// was hit (matching the CPU path's behavior).
// The 'layers' field stores the visibility layer bitmask for per-triangle filtering.
struct GPUTrianglePacked {
	float v0[3];     uint32_t id;
	float edge1[3];  uint32_t layers;
	float edge2[3];  float _pad2;
	float normal[3]; float _pad3;
};
static_assert(sizeof(GPUTrianglePacked) == 64, "GPUTrianglePacked must be 64 bytes (std430)");

// ============================================================================
// GPU BVH Node — 32 bytes (legacy, kept for reference)
// ============================================================================
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

// ============================================================================
// GPU Ray — 32 bytes
// ============================================================================
//
// GLSL layout:
//   struct GPURay {
//       vec3 origin;    float t_max;  // offset  0–15
//       vec3 direction; float t_min;  // offset 16–31
//   };
//
// inv_direction and dir_sign are NOT uploaded — the compute shader computes
// them per-ray. This saves 16 bytes per ray of upload bandwidth and the GPU
// does it in parallel for free.
struct GPURayPacked {
	float origin[3];    float t_max;
	float direction[3]; float t_min;
};
static_assert(sizeof(GPURayPacked) == 32, "GPURayPacked must be 32 bytes (std430)");

// ============================================================================
// GPU Intersection Result — 32 bytes (compact)
// ============================================================================
//
// GLSL layout:
//   struct GPUIntersection {
//       float t; int prim_id; float bary_u; float bary_v;   // offset  0–15
//       vec3 normal; uint hit_layers;                       // offset 16–31
//   };
//
// BANDWIDTH OPTIMIZATION: Position is NOT stored — the CPU reconstructs it
// from ray origin + direction * t. This saves 12 bytes per result,
// reducing readback from 48 bytes to 32 bytes (33% bandwidth reduction).
// At 1280×960 that's 18.7MB saved per frame in GPU→CPU transfer.
//
// prim_id == -1 means "no hit" (the GPU equivalent of Intersection::NO_HIT).
struct GPUIntersectionPacked {
	float t;           int32_t prim_id;
	float bary_u;      float bary_v;
	float normal[3];   uint32_t hit_layers;
};
static_assert(sizeof(GPUIntersectionPacked) == 32, "GPUIntersectionPacked must be 32 bytes (std430)");

// ============================================================================
// Push Constants — 16 bytes
// ============================================================================
//
// Vulkan push constants are the fastest way to pass small uniform data.
// They live in GPU registers — no memory access needed.
// We need ray_count and query_mask; the rest is padding for alignment.
struct GPUPushConstants {
	uint32_t ray_count;
	uint32_t query_mask;
	uint32_t _pad[2];
};
static_assert(sizeof(GPUPushConstants) == 16, "GPUPushConstants must be 16 bytes");
