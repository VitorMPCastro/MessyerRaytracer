#pragma once
// gpu_types.h — GPU-compatible packed structures matching GLSL std430 layout.
//
// These C++ structs MUST exactly match the GLSL struct layouts in compute
// shaders. Any mismatch causes SILENT data corruption — the GPU reads
// garbage and produces wrong results with no error message.
//
// WHY in api/?
//   Modules (graphics CompositorEffects) need these types for GPU upload
//   and readback. Placing them in api/ keeps modules decoupled from gpu/.
//   The gpu/ layer includes this header for its own internal use.
//
// WHY explicit padding?
//   In std430 layout, vec3 has 16-byte alignment but only 12 bytes of data.
//   A scalar (float/uint) following vec3 fills the 4-byte gap perfectly,
//   making each vec3+scalar pair exactly 16 bytes — same as vec4.
//
// MEMORY MAP (verified against GLSL std430 rules):
//   GPUTrianglePacked:     64 bytes  (4 × vec3+scalar)
//   GPURayPacked:          32 bytes  (2 × vec3+float)
//   GPUIntersectionPacked: 32 bytes  (2 × vec3+scalar)
//   GPUPushConstants:      16 bytes  (4 × uint32_t)
//
// To verify alignment, each struct has a static_assert on its size.

#include <cstdint>

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
