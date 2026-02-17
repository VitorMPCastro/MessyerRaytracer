#version 450

// ============================================================================
// cwbvh_traverse.comp.glsl — CWBVH (Compressed Wide BVH) traversal shader.
//
// WHAT:  GPU ray traversal using TinyBVH's CWBVH data layout (Ylitie et al. 2017).
//        8-wide BVH with quantized child AABBs → 80 bytes/node, ~1.5-2× faster
//        than Aila-Laine BVH2 on modern GPUs.
//
// HOW:   Ported from TinyBVH's BVH8_CWBVH::Intersect() (tiny_bvh.h).
//        Key operations: findMSB→__bfind, bitCount→__popc, floatBitsToUint→as_uint.
//        8 children tested in two batches of 4 using quantized 8-bit AABBs.
//
// REF:   "Efficient Incoherent Ray Traversal on GPUs Through Compressed Wide BVHs"
//        (Ylitie, Karras, Laine, 2017)
// ============================================================================

layout(local_size_x = 128, local_size_y = 1, local_size_z = 1) in;

// ============================================================================
// Data structures — ray/intersection must match gpu_structs.h (std430)
// ============================================================================

struct GPURay {
    vec3 origin;    float t_max;
    vec3 direction; float t_min;
};

// Compact intersection — 32 bytes.
struct GPUIntersection {
    float t;       int prim_id;
    float bary_u;  float bary_v;
    vec3 normal;   uint hit_layers;
};

// ============================================================================
// Buffers
// ============================================================================

// CWBVH node data: array of vec4. Each node = 5 consecutive vec4 (80 bytes).
layout(set = 0, binding = 0, std430) restrict readonly buffer CWBVHNodeBuffer {
    vec4 cwbvh_nodes[];
};

// CWBVH triangle data: array of vec4. Each triangle = 3 consecutive vec4 (48 bytes).
// tri[0] = edge2 (v2-v0), tri[1] = edge1 (v1-v0), tri[2] = v0 (w = prim index as float bits).
layout(set = 0, binding = 1, std430) restrict readonly buffer CWBVHTriBuffer {
    vec4 cwbvh_tris[];
};

// Scene triangles (for layer mask filtering and normal lookup).
layout(set = 0, binding = 2, std430) restrict readonly buffer TriangleBuffer {
    // Each triangle: v0(3f) id(1u) edge1(3f) layers(1u) edge2(3f) pad(1f) normal(3f) pad(1f) = 64 bytes
    vec4 scene_tris[];  // 4 vec4 per triangle
};

layout(set = 0, binding = 3, std430) restrict readonly buffer RayBuffer {
    GPURay rays[];
};

layout(set = 0, binding = 4, std430) restrict writeonly buffer IntersectionBuffer {
    GPUIntersection results[];
};

// ============================================================================
// Push constants
// ============================================================================

layout(push_constant, std430) uniform PushConstants {
    uint ray_count;
    uint query_mask;
};

// RAY_MODE 0 = nearest hit, 1 = any hit (early exit).
layout(constant_id = 0) const uint RAY_MODE = 0u;

// ============================================================================
// Constants
// ============================================================================

const uint STACK_DEPTH = 24u;
const uint MAX_ITERATIONS = 65536u;

// ============================================================================
// Helper functions — ported from TinyBVH
// ============================================================================

// Extract byte n (0-3) from packed uint32.
uint extract_byte(uint val, uint n) {
    return (val >> (n * 8u)) & 0xFFu;
}

// Sign-extend the MSB of each packed byte to fill all bits in that byte.
// Input: 4 packed bytes where bit 7 of each is the sign bit of interest.
// Output: each byte is either 0x00 or 0xFF depending on its bit 7.
// Equivalent to NVIDIA PTX prmt.b32 with BA98 selector.
uint sign_extend_s8x4(uint i) {
    uint b0 = ((i & 0x80000000u) != 0u) ? 0xFF000000u : 0u;
    uint b1 = ((i & 0x00800000u) != 0u) ? 0x00FF0000u : 0u;
    uint b2 = ((i & 0x00008000u) != 0u) ? 0x0000FF00u : 0u;
    uint b3 = ((i & 0x00000080u) != 0u) ? 0x000000FFu : 0u;
    return b0 + b1 + b2 + b3;
}

// ============================================================================
// Traversal stack in shared memory
// ============================================================================
// Each entry is uvec2: (child_node_base_index, hit_mask | imask).
// 128 threads × 24 entries × 8 bytes = 24 KB.
// Turing SM has 64 KB shared memory → 24 KB allows 2 workgroups per SM.
// CWBVH's 8-way branching produces shallower trees than BVH2, so 24 is ample.

shared uvec2 shared_stack[128 * STACK_DEPTH];

// ============================================================================
// Moller-Trumbore ray-triangle intersection (CWBVH triangle format)
// ============================================================================
// CWBVH triangles store: tri[0]=edge2, tri[1]=edge1, tri[2]=v0 (w=primIdx as float bits).

bool cwbvh_ray_tri(vec3 origin, vec3 direction, float t_min, inout float best_t,
                   vec3 e2, vec3 e1, vec3 v0,
                   out float out_u, out float out_v) {
    vec3 h = cross(direction, e2);
    float a = dot(e1, h);
    if (abs(a) < 1e-8) return false;
    float f = 1.0 / a;
    vec3 s = origin - v0;
    float u = f * dot(s, h);
    if (u < 0.0 || u > 1.0) return false;
    vec3 q = cross(s, e1);
    float v = f * dot(direction, q);
    if (v < 0.0 || u + v > 1.0) return false;
    float t = f * dot(e2, q);
    if (t < t_min || t >= best_t) return false;
    best_t = t;
    out_u = u;
    out_v = v;
    return true;
}

// ============================================================================
// Main — CWBVH traversal, one thread per ray
// ============================================================================

void main() {
    uint ray_idx = gl_GlobalInvocationID.x;
    if (ray_idx >= ray_count) return;

    uint stack_base = gl_LocalInvocationID.x * STACK_DEPTH;

    // ---- Load ray ----
    vec3 origin    = rays[ray_idx].origin;
    vec3 direction = rays[ray_idx].direction;
    float t_min    = rays[ray_idx].t_min;
    float t_max    = rays[ray_idx].t_max;

    // ---- Early exit for degenerate rays (t_min >= t_max) ----
    // Common for shadow rays from miss pixels (t_min = t_max = 0).
    // Without this, the ray would traverse CWBVH nodes whose AABB contains
    // the origin, wasting thousands of GPU iterations per degenerate ray.
    if (t_min >= t_max) {
        results[ray_idx].t          = t_max;
        results[ray_idx].prim_id    = -1;
        results[ray_idx].bary_u     = 0.0;
        results[ray_idx].bary_v     = 0.0;
        results[ray_idx].normal     = vec3(0.0);
        results[ray_idx].hit_layers = 0u;
        return;
    }

    // Precompute reciprocal direction for slab tests.
    const float EPS = 1e-9;
    const float BIG = 1.0 / EPS;
    vec3 rD = vec3(
        abs(direction.x) > EPS ? 1.0 / direction.x : (direction.x >= 0.0 ? BIG : -BIG),
        abs(direction.y) > EPS ? 1.0 / direction.y : (direction.y >= 0.0 ? BIG : -BIG),
        abs(direction.z) > EPS ? 1.0 / direction.z : (direction.z >= 0.0 ? BIG : -BIG)
    );

    // ---- Initialize result as "no hit" ----
    float best_t    = t_max;
    int best_prim   = -1;
    float best_u    = 0.0;
    float best_v    = 0.0;

    // ---- Octant-based child ordering ----
    // octinv maps ray direction signs to child slot reordering.
    uint octinv = (7u - ((direction.x < 0.0 ? 4u : 0u) | (direction.y < 0.0 ? 2u : 0u) | (direction.z < 0.0 ? 1u : 0u))) * 0x01010101u;

    // ngroup: node group. x = child_node_base_index, y = hit_mask (high byte) | imask
    // tgroup: triangle group. x = triangle_base_index, y = triangle_hit_mask
    uvec2 ngroup = uvec2(0u, 0x80000000u);  // Start at root
    uvec2 tgroup = uvec2(0u, 0u);

    int sp = 0;
    uint iterations = 0u;

    // ---- Main traversal loop ----
    bool running = true;
    while (running) {
        iterations++;
        if (iterations >= MAX_ITERATIONS) break;

        // ---- Process internal node children ----
        if (ngroup.y > 0x00FFFFFFu) {
            uint hits = ngroup.y;
            uint imask_val = ngroup.y;
            uint child_bit_index = findMSB(hits);
            uint child_node_base_index = ngroup.x;

            // Clear the processed child bit.
            ngroup.y &= ~(1u << child_bit_index);

            // If more children remain, push current state to stack.
            if (ngroup.y > 0x00FFFFFFu) {
                if (sp < int(STACK_DEPTH)) {
                    shared_stack[stack_base + uint(sp)] = ngroup;
                    sp++;
                }
            }

            // Compute the actual child node index.
            uint slot_index = (child_bit_index - 24u) ^ (octinv & 255u);
            uint relative_index = bitCount(imask_val & ~(0xFFFFFFFFu << slot_index));
            uint child_node_index = child_node_base_index + relative_index;

            // Load the 5 vec4s of this CWBVH node (80 bytes).
            vec4 n0 = cwbvh_nodes[child_node_index * 5u + 0u];
            vec4 n1 = cwbvh_nodes[child_node_index * 5u + 1u];
            vec4 n2 = cwbvh_nodes[child_node_index * 5u + 2u];
            vec4 n3 = cwbvh_nodes[child_node_index * 5u + 3u];
            vec4 n4 = cwbvh_nodes[child_node_index * 5u + 4u];

            // Decode quantization exponents from n0.w (3 signed int8s + imask byte).
            // Byte layout: [ex:8][ey:8][ez:8][imask:8] (little-endian).
            // bitfieldExtract(int, offset, bits) sign-extends automatically.
            uint n0w_bits = floatBitsToUint(n0.w);
            int ex = bitfieldExtract(int(n0w_bits), 0, 8);
            int ey = bitfieldExtract(int(n0w_bits), 8, 8);
            int ez = bitfieldExtract(int(n0w_bits), 16, 8);

            // New child/triangle base indices for this node's children.
            ngroup.x = floatBitsToUint(n1.x);
            tgroup.x = floatBitsToUint(n1.y);
            tgroup.y = 0u;

            uint hitmask = 0u;

            // Adjusted inverse direction for quantized AABB reconstruction.
            // The exponent represents the AABB extent scale: 2^(ex+127) as float.
            float adjusted_idirx = uintBitsToFloat(uint((ex + 127) << 23)) * rD.x;
            float adjusted_idiry = uintBitsToFloat(uint((ey + 127) << 23)) * rD.y;
            float adjusted_idirz = uintBitsToFloat(uint((ez + 127) << 23)) * rD.z;

            // Translated origin for quantized test.
            float origx = -(origin.x - n0.x) * rD.x;
            float origy = -(origin.y - n0.y) * rD.y;
            float origz = -(origin.z - n0.z) * rD.z;

            // ---- Test first 4 children ----
            {
                uint meta4 = floatBitsToUint(n1.z);
                uint is_inner4 = (meta4 & (meta4 << 1u)) & 0x10101010u;
                uint inner_mask4 = sign_extend_s8x4(is_inner4 << 3u);
                uint bit_index4 = (meta4 ^ (octinv & inner_mask4)) & 0x1F1F1F1Fu;
                uint child_bits4 = (meta4 >> 5u) & 0x07070707u;

                // Quantized AABB bytes: select lo/hi based on ray direction sign (swizzle).
                uint swizzledLox = (direction.x < 0.0) ? floatBitsToUint(n3.z) : floatBitsToUint(n2.x);
                uint swizzledHix = (direction.x < 0.0) ? floatBitsToUint(n2.x) : floatBitsToUint(n3.z);
                uint swizzledLoy = (direction.y < 0.0) ? floatBitsToUint(n4.x) : floatBitsToUint(n2.z);
                uint swizzledHiy = (direction.y < 0.0) ? floatBitsToUint(n2.z) : floatBitsToUint(n4.x);
                uint swizzledLoz = (direction.z < 0.0) ? floatBitsToUint(n4.z) : floatBitsToUint(n3.x);
                uint swizzledHiz = (direction.z < 0.0) ? floatBitsToUint(n3.x) : floatBitsToUint(n4.z);

                for (int i = 0; i < 4; i++) {
                    float tminx = float((swizzledLox >> (i * 8)) & 0xFFu) * adjusted_idirx + origx;
                    float tminy = float((swizzledLoy >> (i * 8)) & 0xFFu) * adjusted_idiry + origy;
                    float tminz = float((swizzledLoz >> (i * 8)) & 0xFFu) * adjusted_idirz + origz;
                    float tmaxx = float((swizzledHix >> (i * 8)) & 0xFFu) * adjusted_idirx + origx;
                    float tmaxy = float((swizzledHiy >> (i * 8)) & 0xFFu) * adjusted_idiry + origy;
                    float tmaxz = float((swizzledHiz >> (i * 8)) & 0xFFu) * adjusted_idirz + origz;

                    float cmin = max(max(max(tminx, tminy), tminz), t_min);
                    float cmax = min(min(min(tmaxx, tmaxy), tmaxz), best_t);

                    if (cmin <= cmax) {
                        hitmask |= extract_byte(child_bits4, uint(i)) << extract_byte(bit_index4, uint(i));
                    }
                }
            }

            // ---- Test second 4 children ----
            {
                uint meta4 = floatBitsToUint(n1.w);
                uint is_inner4 = (meta4 & (meta4 << 1u)) & 0x10101010u;
                uint inner_mask4 = sign_extend_s8x4(is_inner4 << 3u);
                uint bit_index4 = (meta4 ^ (octinv & inner_mask4)) & 0x1F1F1F1Fu;
                uint child_bits4 = (meta4 >> 5u) & 0x07070707u;

                uint swizzledLox = (direction.x < 0.0) ? floatBitsToUint(n3.w) : floatBitsToUint(n2.y);
                uint swizzledHix = (direction.x < 0.0) ? floatBitsToUint(n2.y) : floatBitsToUint(n3.w);
                uint swizzledLoy = (direction.y < 0.0) ? floatBitsToUint(n4.y) : floatBitsToUint(n2.w);
                uint swizzledHiy = (direction.y < 0.0) ? floatBitsToUint(n2.w) : floatBitsToUint(n4.y);
                uint swizzledLoz = (direction.z < 0.0) ? floatBitsToUint(n4.w) : floatBitsToUint(n3.y);
                uint swizzledHiz = (direction.z < 0.0) ? floatBitsToUint(n3.y) : floatBitsToUint(n4.w);

                for (int i = 0; i < 4; i++) {
                    float tminx = float((swizzledLox >> (i * 8)) & 0xFFu) * adjusted_idirx + origx;
                    float tminy = float((swizzledLoy >> (i * 8)) & 0xFFu) * adjusted_idiry + origy;
                    float tminz = float((swizzledLoz >> (i * 8)) & 0xFFu) * adjusted_idirz + origz;
                    float tmaxx = float((swizzledHix >> (i * 8)) & 0xFFu) * adjusted_idirx + origx;
                    float tmaxy = float((swizzledHiy >> (i * 8)) & 0xFFu) * adjusted_idiry + origy;
                    float tmaxz = float((swizzledHiz >> (i * 8)) & 0xFFu) * adjusted_idirz + origz;

                    float cmin = max(max(max(tminx, tminy), tminz), t_min);
                    float cmax = min(min(min(tmaxx, tmaxy), tmaxz), best_t);

                    if (cmin <= cmax) {
                        hitmask |= extract_byte(child_bits4, uint(i)) << extract_byte(bit_index4, uint(i));
                    }
                }
            }

            // Merge: high byte = internal node hits + imask, low 24 bits = triangle hits.
            ngroup.y = (hitmask & 0xFF000000u) | (n0w_bits >> 24u);
            tgroup.y = hitmask & 0x00FFFFFFu;
        }
        else {
            // No internal children — switch to triangle group.
            tgroup = ngroup;
            ngroup = uvec2(0u, 0u);
        }

        // ---- Process triangle group ----
        while (tgroup.y != 0u) {
            uint triangleIndex = findMSB(tgroup.y);
            tgroup.y &= ~(1u << triangleIndex);

            // Each CWBVH triangle = 3 consecutive vec4 in cwbvh_tris[].
            int triAddr = int(tgroup.x) + int(triangleIndex) * 3;
            vec3 e2 = cwbvh_tris[triAddr + 0].xyz;
            vec3 e1 = cwbvh_tris[triAddr + 1].xyz;
            vec3 v0 = cwbvh_tris[triAddr + 2].xyz;
            uint hitPrim = floatBitsToUint(cwbvh_tris[triAddr + 2].w);

            float hu, hv;
            if (cwbvh_ray_tri(origin, direction, t_min, best_t, e2, e1, v0, hu, hv)) {
                // Look up layer mask from GPUTrianglePacked scene data (4 vec4 / 64B per tri).
                // Layout: [v0,id] [edge1,layers] [edge2,pad] [normal,pad]
                // Layers is the .w of the second vec4 (index 1), stored as uint bits.
                uint tri_layers = floatBitsToUint(scene_tris[hitPrim * 4u + 1u].w);

                if ((tri_layers & query_mask) != 0u) {
                    best_prim = int(hitPrim);
                    best_u = hu;
                    best_v = hv;

                    // Any-hit mode: write result and exit immediately.
                    if (RAY_MODE == 1u) {
                        vec3 nrm = scene_tris[hitPrim * 4u + 3u].xyz;
                        results[ray_idx].t          = best_t;
                        results[ray_idx].prim_id    = best_prim;
                        results[ray_idx].bary_u     = best_u;
                        results[ray_idx].bary_v     = best_v;
                        results[ray_idx].normal     = nrm;
                        results[ray_idx].hit_layers = tri_layers;
                        return;
                    }
                }
            }
        }

        // ---- Continue with more internal children or pop from stack ----
        if (ngroup.y > 0x00FFFFFFu) continue;

        if (sp > 0) {
            sp--;
            ngroup = shared_stack[stack_base + uint(sp)];
        } else {
            running = false;
        }
    }

    // ---- Write final result ----
    results[ray_idx].t       = best_t;
    results[ray_idx].prim_id = best_prim;
    results[ray_idx].bary_u  = best_u;
    results[ray_idx].bary_v  = best_v;

    if (best_prim >= 0) {
        results[ray_idx].normal     = scene_tris[uint(best_prim) * 4u + 3u].xyz;
        results[ray_idx].hit_layers = floatBitsToUint(scene_tris[uint(best_prim) * 4u + 1u].w);
    } else {
        results[ray_idx].normal     = vec3(0.0);
        results[ray_idx].hit_layers = 0u;
    }
}
