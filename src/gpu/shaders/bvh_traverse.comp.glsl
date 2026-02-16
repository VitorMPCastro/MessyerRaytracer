#version 450

// ============================================================================
// Workgroup configuration
// ============================================================================
// 64 threads per workgroup is the standard choice for compute shaders.
// NVIDIA GPUs execute in "warps" of 32 threads — 64 = 2 warps, which keeps
// the SM busy even when some threads are stalled on memory.
// AMD GPUs use "wavefronts" of 64 threads, so 64 maps perfectly to 1 wave.
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

// ============================================================================
// Data structures — MUST match gpu_structs.h (std430 layout)
// ============================================================================

struct GPUTriangle {
    vec3 v0;     uint id;
    vec3 edge1;  uint layers;
    vec3 edge2;  float _pad2;
    vec3 normal; float _pad3;
};

struct GPUBVHNode {
    vec3 bounds_min; uint left_first;
    vec3 bounds_max; uint count;
};

struct GPURay {
    vec3 origin;    float t_max;
    vec3 direction; float t_min;
};

struct GPUIntersection {
    vec3 position; float t;
    vec3 normal;   int prim_id;
};

// ============================================================================
// Buffers (Storage Shader Buffer Objects — SSBOs)
// ============================================================================
// set = 0: all buffers share a single descriptor set for simplicity.
// restrict: tells the compiler these buffers don't alias (enables optimization).
// readonly/writeonly: allows the driver to optimize memory access patterns.

layout(set = 0, binding = 0, std430) restrict readonly buffer TriangleBuffer {
    GPUTriangle triangles[];
};

layout(set = 0, binding = 1, std430) restrict readonly buffer BVHNodeBuffer {
    GPUBVHNode bvh_nodes[];
};

layout(set = 0, binding = 2, std430) restrict readonly buffer RayBuffer {
    GPURay rays[];
};

layout(set = 0, binding = 3, std430) restrict writeonly buffer IntersectionBuffer {
    GPUIntersection results[];
};

// ============================================================================
// Push constants — fastest way to pass small uniform data (GPU registers)
// ============================================================================

layout(push_constant, std430) uniform PushConstants {
    uint ray_count;
    uint query_mask;
};

// ============================================================================
// Specialization constants — resolved at pipeline creation (zero cost at runtime)
// ============================================================================
// RAY_MODE 0 = nearest hit (default): find the closest intersection along the ray.
// RAY_MODE 1 = any hit: exit on FIRST intersection (shadow/occlusion queries).
// Using a specialization constant means the compiler eliminates the unused branch
// entirely — no runtime if/else, no thread divergence. Two pipelines, one shader.
layout(constant_id = 0) const uint RAY_MODE = 0u;

// ============================================================================
// AABB slab test (division-free)
// ============================================================================
// Same algorithm as CPU aabb.h.
// Uses precomputed inv_dir to replace division with multiplication.
// min/max intrinsics compile to single GPU instructions — branchless.

bool ray_aabb(vec3 origin, vec3 inv_dir, float t_min, float t_max,
              vec3 box_min, vec3 box_max,
              out float out_tmin, out float out_tmax) {
    vec3 t0s = (box_min - origin) * inv_dir;
    vec3 t1s = (box_max - origin) * inv_dir;

    vec3 tsmaller = min(t0s, t1s);
    vec3 tbigger  = max(t0s, t1s);

    float tmin = max(max(tsmaller.x, tsmaller.y), max(tsmaller.z, t_min));
    float tmax = min(min(tbigger.x, tbigger.y), min(tbigger.z, t_max));

    out_tmin = tmin;
    out_tmax = tmax;
    return tmin <= tmax;
}

// ============================================================================
// Moller-Trumbore triangle intersection
// ============================================================================
// Same algorithm as CPU tri.h.
// Uses precomputed edge1/edge2/normal from the GPUTriangle struct.
// Updates best_t in-place and outputs hit position + normal.

bool ray_triangle(vec3 origin, vec3 direction, float t_min,
                  vec3 v0, vec3 edge1, vec3 edge2, vec3 tri_normal,
                  inout float best_t, out vec3 hit_pos, out vec3 hit_normal) {
    vec3 pvec = cross(direction, edge2);
    float det = dot(edge1, pvec);

    if (abs(det) < 1e-8) return false;

    float inv_det = 1.0 / det;
    vec3 tvec = origin - v0;
    float u = dot(tvec, pvec) * inv_det;
    if (u < 0.0 || u > 1.0) return false;

    vec3 qvec = cross(tvec, edge1);
    float v = dot(direction, qvec) * inv_det;
    if (v < 0.0 || u + v > 1.0) return false;

    float t = dot(edge2, qvec) * inv_det;
    if (t < t_min || t >= best_t) return false;

    best_t = t;
    hit_pos = origin + direction * t;
    hit_normal = tri_normal;
    return true;
}

// ============================================================================
// Safe inverse direction (matches CPU ray.h logic for near-zero components)
// ============================================================================

vec3 safe_inv_direction(vec3 dir) {
    const float EPS = 1e-9;
    const float BIG = 1.0 / EPS; // 1e9
    return vec3(
        abs(dir.x) > EPS ? 1.0 / dir.x : (dir.x >= 0.0 ? BIG : -BIG),
        abs(dir.y) > EPS ? 1.0 / dir.y : (dir.y >= 0.0 ? BIG : -BIG),
        abs(dir.z) > EPS ? 1.0 / dir.z : (dir.z >= 0.0 ? BIG : -BIG)
    );
}

// ============================================================================
// Shared memory traversal stack
// ============================================================================
// Each thread gets its own stack segment in workgroup shared memory.
// Why shared memory instead of local arrays?
//   - GLSL local arrays compile to "local memory" which is actually device DRAM.
//   - Shared memory is on-chip SRAM: 5-10x lower latency than device memory.
//   - This frees registers for ray/intersection data, improving occupancy.
//
// Memory budget: 64 threads x 24 entries x 4 bytes x 2 arrays = 12 KB
// GTX 1650 Ti (Turing) has 64 KB shared memory — 12 KB allows 5 workgroups per SM.
// Depth 24 supports BVH with up to 2^24 (~16M) nodes — far beyond any scene.

const uint STACK_DEPTH = 24u;
shared uint  shared_stack_node[64 * STACK_DEPTH];
shared float shared_stack_tmin[64 * STACK_DEPTH];

// ============================================================================
// Main — one thread per ray, iterative BVH traversal
// ============================================================================

void main() {
    uint ray_idx = gl_GlobalInvocationID.x;
    if (ray_idx >= ray_count) return;

    // Per-thread shared memory stack offset.
    // Thread i within the workgroup gets stack entries at [i*STACK_DEPTH .. (i+1)*STACK_DEPTH).
    uint stack_base = gl_LocalInvocationID.x * STACK_DEPTH;

    // ---- Load ray data ----
    vec3 origin    = rays[ray_idx].origin;
    vec3 direction = rays[ray_idx].direction;
    float t_min    = rays[ray_idx].t_min;
    float t_max    = rays[ray_idx].t_max;

    // Precompute inverse direction once per ray.
    // On CPU we do this in the Ray constructor.
    // On GPU we do it here — the parallel threads make it free.
    vec3 inv_dir = safe_inv_direction(direction);

    // ---- Initialize result as "no hit" ----
    float best_t      = t_max;
    vec3 best_pos      = vec3(0.0);
    vec3 best_normal   = vec3(0.0);
    int best_prim      = -1;

    // ---- Test root AABB ----
    float root_tmin, root_tmax;
    if (!ray_aabb(origin, inv_dir, t_min, best_t,
                  bvh_nodes[0].bounds_min, bvh_nodes[0].bounds_max,
                  root_tmin, root_tmax)) {
        // Ray misses the entire scene — early exit.
        results[ray_idx].position = vec3(0.0);
        results[ray_idx].t = t_max;
        results[ray_idx].normal = vec3(0.0);
        results[ray_idx].prim_id = -1;
        return;
    }

    // ---- Iterative traversal stack (shared memory) ----
    // Stack lives in per-thread segment of workgroup shared memory.
    // Access: shared_stack_node[stack_base + sp], shared_stack_tmin[stack_base + sp]
    int sp = 0;

    shared_stack_node[stack_base]     = 0u;
    shared_stack_tmin[stack_base]     = root_tmin;
    sp = 1;

    while (sp > 0) {
        sp--;
        uint node_idx    = shared_stack_node[stack_base + uint(sp)];
        float entry_tmin = shared_stack_tmin[stack_base + uint(sp)];

        // EARLY EXIT: this subtree's nearest possible hit is farther
        // than what we've already found. Skip without re-testing AABB.
        if (entry_tmin > best_t) continue;

        // Load node data.
        uint node_left_first = bvh_nodes[node_idx].left_first;
        uint node_count      = bvh_nodes[node_idx].count;

        if (node_count > 0u) {
            // ---- LEAF NODE: test all triangles ----
            for (uint i = 0u; i < node_count; i++) {
                uint tri_idx = node_left_first + i;
                // Skip triangles not on any queried layer.
                if ((triangles[tri_idx].layers & query_mask) == 0u) continue;
                vec3 hp, hn;
                if (ray_triangle(origin, direction, t_min,
                                 triangles[tri_idx].v0,
                                 triangles[tri_idx].edge1,
                                 triangles[tri_idx].edge2,
                                 triangles[tri_idx].normal,
                                 best_t, hp, hn)) {
                    best_pos    = hp;
                    best_normal = hn;
                    best_prim   = int(triangles[tri_idx].id);

                    // ANY_HIT: Return immediately on first intersection.
                    // No need to find the closest — just report that something was hit.
                    // The compiler eliminates this block entirely for nearest-hit pipelines.
                    if (RAY_MODE == 1u) {
                        results[ray_idx].t        = best_t;
                        results[ray_idx].position = best_pos;
                        results[ray_idx].normal   = best_normal;
                        results[ray_idx].prim_id  = best_prim;
                        return;
                    }
                }
            }
        } else {
            // ---- INTERNAL NODE: test both children's AABBs ----
            // DFS layout: left child is always at node_idx + 1 (implicit).
            // left_first stores the right child index.
            uint left  = node_idx + 1u;
            uint right = node_left_first;

            float tmin_l, tmax_l, tmin_r, tmax_r;
            bool hit_l = ray_aabb(origin, inv_dir, t_min, best_t,
                                  bvh_nodes[left].bounds_min, bvh_nodes[left].bounds_max,
                                  tmin_l, tmax_l);
            bool hit_r = ray_aabb(origin, inv_dir, t_min, best_t,
                                  bvh_nodes[right].bounds_min, bvh_nodes[right].bounds_max,
                                  tmin_r, tmax_r);

            // Further filter: can this child beat the current closest hit?
            hit_l = hit_l && (tmin_l <= best_t);
            hit_r = hit_r && (tmin_r <= best_t);

            if (hit_l && hit_r) {
                // Push FAR child first so NEAR child is popped first (LIFO).
                // This finds nearby hits quickly, enabling more early exits.
                if (tmin_l < tmin_r) {
                    shared_stack_node[stack_base + uint(sp)] = right;
                    shared_stack_tmin[stack_base + uint(sp)] = tmin_r; sp++;
                    shared_stack_node[stack_base + uint(sp)] = left;
                    shared_stack_tmin[stack_base + uint(sp)] = tmin_l; sp++;
                } else {
                    shared_stack_node[stack_base + uint(sp)] = left;
                    shared_stack_tmin[stack_base + uint(sp)] = tmin_l; sp++;
                    shared_stack_node[stack_base + uint(sp)] = right;
                    shared_stack_tmin[stack_base + uint(sp)] = tmin_r; sp++;
                }
            } else if (hit_l) {
                shared_stack_node[stack_base + uint(sp)] = left;
                shared_stack_tmin[stack_base + uint(sp)] = tmin_l; sp++;
            } else if (hit_r) {
                shared_stack_node[stack_base + uint(sp)] = right;
                shared_stack_tmin[stack_base + uint(sp)] = tmin_r; sp++;
            }
        }
    }

    // ---- Write result ----
    results[ray_idx].t        = best_t;
    results[ray_idx].position = best_pos;
    results[ray_idx].normal   = best_normal;
    results[ray_idx].prim_id  = best_prim;
}
