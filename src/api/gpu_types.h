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

// ============================================================================
// GPU BVH Node — 32 bytes (standard BVH2 format)
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
// GPUSceneUpload — opaque scene data for GPU upload (module-safe)
// ============================================================================
//
// Returned by IRayService::get_gpu_scene_data().  Contains pre-packed GPU
// buffers so that modules never include accel/ or gpu/ internal headers.
// The bridge (ray_service_bridge.cpp) populates this from RayScene internals.
//
// Pointers are valid after build() and stable until the next build().
// Modules treat this as read-only.
//
// WHY this exists:
//   CompositorEffects need triangle and BVH node data for GPU upload.
//   Previously they included accel/ray_scene.h directly (module boundary
//   violation).  GPUSceneUpload keeps accel/ types behind the api/ firewall.

struct GPUSceneUpload {
	const GPUTrianglePacked *triangles = nullptr;  // Pre-packed GPU triangles
	uint32_t triangle_count             = 0;

	const GPUBVHNodePacked *bvh_nodes   = nullptr;  // Pre-packed BVH2 nodes
	uint32_t bvh_node_count             = 0;

	bool valid                          = false;   // True if scene is built
};

// ============================================================================
// GPU Path State — 48 bytes (wavefront path tracing)
// ============================================================================
//
// GLSL layout:
//   struct GPUPathState {
//       vec3  throughput; uint rng_state;     // offset  0–15
//       vec3  accum;      uint flags;         // offset 16–31
//       vec3  potential_nee; float _pad3;     // offset 32–47
//   };
//
// flags layout (bitfield):
//   bit 0    = active (1 = path still bouncing)
//   bits 1-7 = bounce depth (0-127)
//   bits 8+  = reserved
//
// potential_nee: stores the pre-shadow NEE contribution (direct illumination ×
// throughput) computed in the Shade kernel.  Applied in the NEXT Shade pass
// (or the finalize pass) after the Connect kernel provides shadow visibility.
// This decouples shadow ray tracing from radiance accumulation in the wavefront
// pipeline.  The contribution is already multiplied by the throughput at the
// time of evaluation, so the deferred application is just:
//   accum += potential_nee * shadow_visibility
// See pt_shade.comp.glsl for the full deferred-NEE protocol.

struct GPUPathStatePacked {
	float throughput[3]; uint32_t rng_state;
	float accum[3];      uint32_t flags;
	float potential_nee[3]; float _pad3;
};
static_assert(sizeof(GPUPathStatePacked) == 48, "GPUPathStatePacked must be 48 bytes (std430)");

// ============================================================================
// GPU Material — 64 bytes (PBR surface parameters)
// ============================================================================
//
// GLSL layout:
//   struct GPUMaterial {
//       vec3  albedo;    float metallic;      // offset  0–15
//       vec3  emission;  float roughness;     // offset 16–31
//       float specular;  float emission_energy; float normal_scale; uint tex_flags;  // 32–47
//       int   albedo_tex_idx;  int normal_tex_idx;  int tex_width;  int tex_height;  // 48–63
//   };
//
// tex_flags layout (bitfield):
//   bit 0 = has_albedo_texture
//   bit 1 = has_normal_texture
//
// Texture indices point into the Texture2DArray layers.
// -1 means "no texture" (use base color).

struct GPUMaterialPacked {
	float albedo[3];       float metallic;
	float emission[3];     float roughness;
	float specular;        float emission_energy;
	float normal_scale;    uint32_t tex_flags;
	int32_t albedo_tex_idx;  int32_t normal_tex_idx;
	int32_t tex_width;       int32_t tex_height;
};
static_assert(sizeof(GPUMaterialPacked) == 64, "GPUMaterialPacked must be 64 bytes (std430)");

// ============================================================================
// GPU Light — 64 bytes (matches LightData on CPU)
// ============================================================================
//
// GLSL layout:
//   struct GPULight {
//       vec3  position;  float range;          // offset  0–15
//       vec3  direction; float attenuation;     // offset 16–31
//       vec3  color;     float spot_angle;      // offset 32–47
//       uint  type;      float spot_angle_atten; uint cast_shadows; uint _pad;  // 48–63
//   };
//
// type: 0 = DIRECTIONAL, 1 = POINT, 2 = SPOT

struct GPULightPacked {
	float position[3];     float range;
	float direction[3];    float attenuation;
	float color[3];        float spot_angle;
	uint32_t type;         float spot_angle_attenuation;
	uint32_t cast_shadows; uint32_t _pad;
};
static_assert(sizeof(GPULightPacked) == 64, "GPULightPacked must be 64 bytes (std430)");

// ============================================================================
// GPU Environment — 64 bytes (sky + ambient + tone mapping)
// ============================================================================
//
// GLSL layout:
//   struct GPUEnvironment {
//       vec3  sky_zenith;   float ambient_energy;   // offset  0–15
//       vec3  sky_horizon;  float ambient_r;         // offset 16–31
//       vec3  sky_ground;   float ambient_g;         // offset 32–47
//       float ambient_b;    int tonemap_mode; uint has_panorama; uint _pad;  // 48–63
//   };

struct GPUEnvironmentPacked {
	float sky_zenith[3];   float ambient_energy;
	float sky_horizon[3];  float ambient_r;
	float sky_ground[3];   float ambient_g;
	float ambient_b;       int32_t tonemap_mode;
	uint32_t has_panorama; uint32_t _pad;
};
static_assert(sizeof(GPUEnvironmentPacked) == 64, "GPUEnvironmentPacked must be 64 bytes (std430)");

// ============================================================================
// GPU Camera — 64 bytes (for primary ray generation)
// ============================================================================
//
// GLSL layout:
//   struct GPUCamera {
//       vec3  origin;      float fov_y_rad;        // offset  0–15
//       vec3  forward;     float aspect;            // offset 16–31
//       vec3  right;       float near_plane;        // offset 32–47
//       vec3  up;          float far_plane;         // offset 48–63
//   };

struct GPUCameraPacked {
	float origin[3];   float fov_y_rad;
	float forward[3];  float aspect;
	float right[3];    float near_plane;
	float up[3];       float far_plane;
};
static_assert(sizeof(GPUCameraPacked) == 64, "GPUCameraPacked must be 64 bytes (std430)");

// ============================================================================
// GPU Path Trace Push Constants — 32 bytes
// ============================================================================
//
// Passed as Vulkan push constants (fast, register-backed).
// Contains per-frame parameters that change every dispatch.

struct GPUPathTracePush {
	uint32_t pixel_count;     // Total rays (width × height)
	uint32_t width;           // Framebuffer width
	uint32_t height;          // Framebuffer height
	uint32_t bounce;          // Current bounce index (0 = primary)
	uint32_t max_bounces;     // Maximum bounce depth
	uint32_t sample_index;    // Temporal sample index (for RNG seeding)
	uint32_t light_count;     // Number of active lights
	uint32_t shadows_enabled; // 1 = trace shadow rays, 0 = skip
};
static_assert(sizeof(GPUPathTracePush) == 32, "GPUPathTracePush must be 32 bytes");

