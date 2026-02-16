#version 450

// ============================================================================
// Workgroup configuration
// ============================================================================
// 128 threads: 4 warps (NVIDIA) or 2 wavefronts (AMD).
// More threads per workgroup = better latency hiding when threads stall on
// memory. Turing SM has 1024 max resident threads, so 128-thread workgroups
// allow 8 workgroups per SM (good occupancy).
layout(local_size_x = 128, local_size_y = 1, local_size_z = 1) in;

// ============================================================================
// Data structures — MUST match gpu_structs.h (std430 layout)
// ============================================================================

struct GPUTriangle {
    vec3 v0;     uint id;
    vec3 edge1;  uint layers;
    vec3 edge2;  float _pad2;
    vec3 normal; float _pad3;
};

// Aila-Laine dual-AABB BVH node (64 bytes).
// Both children's AABBs are stored in the parent, so traversal needs only
// ONE memory fetch per node instead of TWO. This halves the memory latency
// bottleneck during GPU traversal.
struct GPUBVHNodeWide {
    vec3 left_min;  uint left_idx;
    vec3 left_max;  uint right_idx;
    vec3 right_min; uint left_count;
    vec3 right_max; uint right_count;
};

struct GPURay {
    vec3 origin;    float t_max;
    vec3 direction; float t_min;
};

// Compact intersection — 32 bytes (position reconstructed on CPU from ray+t).
struct GPUIntersection {
    float t;       int prim_id;
    float bary_u;  float bary_v;
    vec3 normal;   uint hit_layers;
};

// ============================================================================
// Buffers (Storage Shader Buffer Objects — SSBOs)
// ============================================================================

layout(set = 0, binding = 0, std430) restrict readonly buffer TriangleBuffer {
    GPUTriangle triangles[];
};

layout(set = 0, binding = 1, std430) restrict readonly buffer BVHNodeBuffer {
    GPUBVHNodeWide bvh_nodes[];
};

layout(set = 0, binding = 2, std430) restrict readonly buffer RayBuffer {
    GPURay rays[];
};

layout(set = 0, binding = 3, std430) restrict writeonly buffer IntersectionBuffer {
    GPUIntersection results[];
};

// ============================================================================
// Push constants & specialization constants
// ============================================================================

layout(push_constant, std430) uniform PushConstants {
    uint ray_count;
    uint query_mask;
};

// RAY_MODE 0 = nearest hit: find closest intersection.
// RAY_MODE 1 = any hit: exit on first intersection (shadow queries).
// Specialization constant → compiler eliminates dead branch entirely.
layout(constant_id = 0) const uint RAY_MODE = 0u;

// ============================================================================
// AABB slab test (branchless, division-free with precomputed inv_dir)
// ============================================================================

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

bool ray_triangle(vec3 origin, vec3 direction, float t_min,
                  vec3 v0, vec3 edge1, vec3 edge2, vec3 tri_normal,
                  inout float best_t, out vec3 hit_normal,
                  out float out_u, out float out_v) {
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
    hit_normal = tri_normal;
    out_u = u;
    out_v = v;
    return true;
}

// ============================================================================
// Safe inverse direction (handles near-zero components)
// ============================================================================

vec3 safe_inv_direction(vec3 dir) {
    const float EPS = 1e-9;
    const float BIG = 1.0 / EPS;
    return vec3(
        abs(dir.x) > EPS ? 1.0 / dir.x : (dir.x >= 0.0 ? BIG : -BIG),
        abs(dir.y) > EPS ? 1.0 / dir.y : (dir.y >= 0.0 ? BIG : -BIG),
        abs(dir.z) > EPS ? 1.0 / dir.z : (dir.z >= 0.0 ? BIG : -BIG)
    );
}

// ============================================================================
// Shared memory traversal stack
// ============================================================================
// Each thread gets its own segment: [local_id * STACK_DEPTH .. (local_id+1) * STACK_DEPTH)
// Memory: 128 threads × 24 entries × 4 bytes × 2 arrays = 24 KB
// Turing SM has 64 KB shared memory → 24 KB allows 2 workgroups per SM.
//
// The tmin stack enables early exit: if best_t shrank since a node was pushed,
// we skip it entirely without loading the node data (saves a 64-byte fetch).

const uint STACK_DEPTH = 24u;
const uint MAX_ITERATIONS = 65536u;  // Safety: prevent infinite loops from any cause.
shared uint  shared_stack_node[128 * STACK_DEPTH];
shared float shared_stack_tmin[128 * STACK_DEPTH];

// ============================================================================
// Leaf triangle intersection helper (avoids code duplication)
// ============================================================================

#define INTERSECT_LEAF(first_tri, tri_count)                                      \
    for (uint _li = 0u; _li < (tri_count); _li++) {                              \
        uint tri_idx = (first_tri) + _li;                                         \
        if ((triangles[tri_idx].layers & query_mask) == 0u) continue;             \
        vec3 hn; float hu, hv;                                                    \
        if (ray_triangle(origin, direction, t_min,                                \
                         triangles[tri_idx].v0,                                   \
                         triangles[tri_idx].edge1,                                \
                         triangles[tri_idx].edge2,                                \
                         triangles[tri_idx].normal,                               \
                         best_t, hn, hu, hv)) {                                   \
            best_normal = hn;                                                     \
            best_u      = hu;                                                     \
            best_v      = hv;                                                     \
            best_prim   = int(triangles[tri_idx].id);                             \
            best_layers = triangles[tri_idx].layers;                              \
            if (RAY_MODE == 1u) {                                                 \
                results[ray_idx].t          = best_t;                             \
                results[ray_idx].prim_id    = best_prim;                          \
                results[ray_idx].bary_u     = best_u;                             \
                results[ray_idx].bary_v     = best_v;                             \
                results[ray_idx].normal     = best_normal;                        \
                results[ray_idx].hit_layers = best_layers;                        \
                return;                                                           \
            }                                                                     \
        }                                                                         \
    }

// ============================================================================
// Main — one thread per ray, Aila-Laine BVH traversal
// ============================================================================

void main() {
    uint ray_idx = gl_GlobalInvocationID.x;
    if (ray_idx >= ray_count) return;

    uint stack_base = gl_LocalInvocationID.x * STACK_DEPTH;

    // ---- Load ray data ----
    vec3 origin    = rays[ray_idx].origin;
    vec3 direction = rays[ray_idx].direction;
    float t_min    = rays[ray_idx].t_min;
    float t_max    = rays[ray_idx].t_max;
    vec3 inv_dir   = safe_inv_direction(direction);

    // ---- Initialize result as "no hit" ----
    float best_t      = t_max;
    vec3 best_normal   = vec3(0.0);
    int best_prim      = -1;
    float best_u       = 0.0;
    float best_v       = 0.0;
    uint best_layers   = 0u;

    // ---- Start traversal at root (node 0) ----
    // Root always has valid children data (leaf roots are wrapped as pseudo-internal
    // by the CPU-side converter in gpu_ray_caster.cpp::upload_scene).
    int sp = 0;
    shared_stack_node[stack_base]     = 0u;
    shared_stack_tmin[stack_base]     = -1e30;  // guaranteed < best_t
    sp = 1;

    uint iterations = 0u;
    while (sp > 0 && iterations < MAX_ITERATIONS) {
        iterations++;
        sp--;
        uint node_idx    = shared_stack_node[stack_base + uint(sp)];
        float entry_tmin = shared_stack_tmin[stack_base + uint(sp)];

        // Early exit: this subtree's nearest possible hit is farther
        // than current best. Skip without loading the node.
        if (entry_tmin > best_t) continue;

        // ---- Load wide node: ONE fetch → both children's AABBs ----
        // This is the Aila-Laine key insight: half the memory fetches
        // compared to standard BVH that requires loading each child node
        // separately to read its AABB.
        vec3 l_min   = bvh_nodes[node_idx].left_min;
        vec3 l_max   = bvh_nodes[node_idx].left_max;
        uint l_idx   = bvh_nodes[node_idx].left_idx;
        uint l_count = bvh_nodes[node_idx].left_count;

        vec3 r_min   = bvh_nodes[node_idx].right_min;
        vec3 r_max   = bvh_nodes[node_idx].right_max;
        uint r_idx   = bvh_nodes[node_idx].right_idx;
        uint r_count = bvh_nodes[node_idx].right_count;

        // ---- Test both children's AABBs ----
        float tmin_l, tmax_l;
        bool hit_l = ray_aabb(origin, inv_dir, t_min, best_t, l_min, l_max, tmin_l, tmax_l)
                     && (tmin_l <= best_t);

        float tmin_r, tmax_r;
        bool hit_r = ray_aabb(origin, inv_dir, t_min, best_t, r_min, r_max, tmin_r, tmax_r)
                     && (tmin_r <= best_t);

        // ---- Handle leaf children immediately (no stack push) ----
        if (hit_l && l_count > 0u) {
            INTERSECT_LEAF(l_idx, l_count)
            hit_l = false;  // Handled — don't push.
        }
        if (hit_r && r_count > 0u) {
            INTERSECT_LEAF(r_idx, r_count)
            hit_r = false;  // Handled — don't push.
        }

        // ---- Push internal children (near first for better pruning) ----
        bool push_l = hit_l && (l_count == 0u);
        bool push_r = hit_r && (r_count == 0u);

        if (push_l && push_r) {
            // Push far child first (LIFO → near child processed first).
            // Clamp sp to prevent shared memory corruption on pathological trees.
            if (tmin_l < tmin_r) {
                if (sp < int(STACK_DEPTH) - 1) {
                    shared_stack_node[stack_base + uint(sp)] = r_idx;
                    shared_stack_tmin[stack_base + uint(sp)] = tmin_r; sp++;
                    shared_stack_node[stack_base + uint(sp)] = l_idx;
                    shared_stack_tmin[stack_base + uint(sp)] = tmin_l; sp++;
                }
            } else {
                if (sp < int(STACK_DEPTH) - 1) {
                    shared_stack_node[stack_base + uint(sp)] = l_idx;
                    shared_stack_tmin[stack_base + uint(sp)] = tmin_l; sp++;
                    shared_stack_node[stack_base + uint(sp)] = r_idx;
                    shared_stack_tmin[stack_base + uint(sp)] = tmin_r; sp++;
                }
            }
        } else if (push_l) {
            if (sp < int(STACK_DEPTH)) {
                shared_stack_node[stack_base + uint(sp)] = l_idx;
                shared_stack_tmin[stack_base + uint(sp)] = tmin_l; sp++;
            }
        } else if (push_r) {
            if (sp < int(STACK_DEPTH)) {
                shared_stack_node[stack_base + uint(sp)] = r_idx;
                shared_stack_tmin[stack_base + uint(sp)] = tmin_r; sp++;
            }
        }
    }

    // ---- Write result ----
    results[ray_idx].t          = best_t;
    results[ray_idx].prim_id    = best_prim;
    results[ray_idx].bary_u     = best_u;
    results[ray_idx].bary_v     = best_v;
    results[ray_idx].normal     = best_normal;
    results[ray_idx].hit_layers = best_layers;
}
