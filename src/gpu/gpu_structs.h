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
// GPU BVH Node — 32 bytes
// ============================================================================
//
// GLSL layout:
//   struct GPUBVHNode {
//       vec3 bounds_min; uint left_first;  // offset  0–15
//       vec3 bounds_max; uint count;        // offset 16–31
//   };
//
// Encoding (same as CPU BVH):
//   count == 0: INTERNAL node. Left child = this_node + 1 (implicit DFS).
//               left_first = right child index.
//   count >  0: LEAF node. left_first = first triangle index. count = triangle count.
//
// NOTE: Godot stores AABB as (position, size). We convert to (min, max) for
// the GPU because the slab test needs min/max, not position/size.
struct GPUBVHNodePacked {
	float bounds_min[3]; uint32_t left_first;
	float bounds_max[3]; uint32_t count;
};
static_assert(sizeof(GPUBVHNodePacked) == 32, "GPUBVHNodePacked must be 32 bytes (std430)");

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
// GPU Intersection Result — 48 bytes
// ============================================================================
//
// GLSL layout:
//   struct GPUIntersection {
//       vec3 position; float t;                   // offset  0–15
//       vec3 normal;   int prim_id;               // offset 16–31
//       float bary_u;  float bary_v;
//       uint hit_layers; float _pad;              // offset 32–47
//   };
//
// prim_id == -1 means "no hit" (the GPU equivalent of Intersection::NO_HIT).
// bary_u, bary_v = Möller-Trumbore barycentric coordinates.
// hit_layers = visibility layer bitmask of the hit triangle.
struct GPUIntersectionPacked {
	float position[3]; float t;
	float normal[3];   int32_t prim_id;
	float bary_u;      float bary_v;
	uint32_t hit_layers; float _pad;
};
static_assert(sizeof(GPUIntersectionPacked) == 48, "GPUIntersectionPacked must be 48 bytes (std430)");

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
