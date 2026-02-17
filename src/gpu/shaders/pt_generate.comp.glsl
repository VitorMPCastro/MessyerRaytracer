#version 450

// ============================================================================
// pt_generate.comp.glsl — Primary ray generation for GPU wavefront path tracing.
//
// WHAT:  Generates camera rays and initializes per-pixel path state for the
//        wavefront path tracing pipeline.  Each thread handles one pixel.
//
// HOW:   Reconstructs camera ray from pixel coordinates + camera parameters
//        (origin, forward, right, up, FOV).  Initializes path state with
//        throughput=1, accumulated_radiance=0, and a PCG32 RNG seed unique
//        per pixel+frame for decorrelated noise.
//
// PIPELINE STAGE:  First kernel in the wavefront loop.
//   Generate → Extend → Shade → Connect → (repeat per bounce)
//
// REFERENCES:
//   Laine, Karras, Aila — "Megakernels Considered Harmful" (HPG 2013)
// ============================================================================

layout(local_size_x = 128, local_size_y = 1, local_size_z = 1) in;

// ============================================================================
// Data structures — must match api/gpu_types.h (std430)
// ============================================================================

struct GPURay {
    vec3 origin;    float t_max;
    vec3 direction; float t_min;
};

struct GPUPathState {
    vec3  throughput; uint rng_state;
    vec3  accum;      uint flags;
    vec3  potential_nee; float _pad3;
};

struct GPUCamera {
    vec3  origin;  float fov_y_rad;
    vec3  forward; float aspect;
    vec3  right;   float near_plane;
    vec3  up;      float far_plane;
};

// ============================================================================
// Buffers
// ============================================================================

layout(set = 0, binding = 0, std430) restrict writeonly buffer RayBuffer {
    GPURay rays[];
};

layout(set = 0, binding = 1, std430) restrict writeonly buffer PathStateBuffer {
    GPUPathState path_states[];
};

layout(set = 0, binding = 2, std430) restrict readonly buffer CameraBuffer {
    GPUCamera camera;
};

// ============================================================================
// Push constants
// ============================================================================

layout(push_constant, std430) uniform PushConstants {
    uint pixel_count;    // Total pixels (width × height)
    uint width;          // Framebuffer width
    uint height;         // Framebuffer height
    uint bounce;         // Unused in Generate (always 0)
    uint max_bounces;    // Unused in Generate
    uint sample_index;   // Temporal sample index for RNG seeding
    uint light_count;    // Unused in Generate
    uint shadows_enabled; // Unused in Generate
};

// ============================================================================
// PCG32 random number generator
// ============================================================================
// Minimal permuted congruential generator (O'Neill 2014).
// 32-bit state, period 2^32, excellent distribution.
// Matches CPU implementation in path_state.h.

uint pcg32_next(inout uint state) {
    uint old = state;
    state = old * 747796405u + 2891336453u;
    uint word = ((old >> ((old >> 28u) + 4u)) ^ old) * 277803737u;
    return (word >> 22u) ^ word;
}

float pcg32_float(inout uint state) {
    return float(pcg32_next(state)) * (1.0 / 4294967296.0);
}

uint pcg32_seed(uint pixel_index, uint frame) {
    // Match CPU seeding: pixel_index * 1009 + frame * 6529 + 7
    uint s = pixel_index * 1009u + frame * 6529u + 7u;
    // Two calls to mix the seed (same as CPU PCG32::seed).
    uint state = 0u;
    state = state * 747796405u + 2891336453u;  // advance past zero
    state += s;
    state = state * 747796405u + 2891336453u;  // mix the seed
    return state;
}

// ============================================================================
// Main — one thread per pixel
// ============================================================================

void main() {
    uint pixel_idx = gl_GlobalInvocationID.x;
    if (pixel_idx >= pixel_count) return;

    // ---- Pixel coordinates ----
    uint px = pixel_idx % width;
    uint py = pixel_idx / width;

    // ---- Anti-aliasing jitter (sub-pixel offset) ----
    uint rng = pcg32_seed(pixel_idx, sample_index);
    float jitter_x = pcg32_float(rng) - 0.5;
    float jitter_y = pcg32_float(rng) - 0.5;

    // Convert pixel to normalized device coordinates [-1, 1].
    // Y is flipped (screen Y=0 is top, camera up is positive Y).
    float ndc_x = (2.0 * (float(px) + 0.5 + jitter_x) / float(width) - 1.0);
    float ndc_y = -(2.0 * (float(py) + 0.5 + jitter_y) / float(height) - 1.0);

    // ---- Camera ray construction ----
    // half_height = tan(fov_y / 2), half_width = half_height * aspect
    float half_height = tan(camera.fov_y_rad * 0.5);
    float half_width  = half_height * camera.aspect;

    vec3 direction = normalize(
        camera.forward
        + camera.right * (ndc_x * half_width)
        + camera.up    * (ndc_y * half_height)
    );

    // ---- Write ray ----
    rays[pixel_idx].origin    = camera.origin;
    rays[pixel_idx].direction = direction;
    rays[pixel_idx].t_min     = camera.near_plane;
    rays[pixel_idx].t_max     = camera.far_plane;

    // ---- Initialize path state ----
    path_states[pixel_idx].throughput = vec3(1.0, 1.0, 1.0);
    path_states[pixel_idx].rng_state  = rng;
    path_states[pixel_idx].accum      = vec3(0.0, 0.0, 0.0);
    path_states[pixel_idx].flags      = 1u;  // bit 0 = active
    path_states[pixel_idx].potential_nee = vec3(0.0, 0.0, 0.0);
    path_states[pixel_idx]._pad3      = 0.0;
}
