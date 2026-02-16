#version 450

// rt_reflections.comp.glsl — Ray-traced reflection pass.
//
// Pass 1 of the RT reflection pipeline:
//   1. Read depth buffer → reconstruct world-space position
//   2. Read normal_roughness → decode normal, extract roughness
//   3. Skip pixels with roughness > threshold
//   4. Compute reflection ray direction
//   5. Trace against BVH (same traversal as bvh_traverse.comp.glsl)
//   6. Write: hit color (sky or material albedo), hit distance → reflection texture

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

// ============================================================================
// BVH data structures — MUST match gpu_structs.h (std430 layout)
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

// ============================================================================
// Descriptor set 0: Scene textures (Godot's render output)
// ============================================================================

layout(set = 0, binding = 0) uniform sampler2D depth_sampler;
layout(set = 0, binding = 1) uniform sampler2D normal_roughness_sampler;

// ============================================================================
// Descriptor set 1: BVH data (uploaded to shared device)
// ============================================================================

layout(set = 1, binding = 0, std430) restrict readonly buffer TriangleBuffer {
    GPUTriangle triangles[];
};

layout(set = 1, binding = 1, std430) restrict readonly buffer BVHNodeBuffer {
    GPUBVHNode bvh_nodes[];
};

// ============================================================================
// Descriptor set 2: Output reflection texture
// ============================================================================

layout(set = 2, binding = 0, rgba16f) restrict writeonly uniform image2D reflection_output;

// ============================================================================
// Push constants — camera data + parameters
// ============================================================================

layout(push_constant, std430) uniform Params {
    mat4 inv_projection;
    mat4 inv_view;
    float roughness_threshold;  // Skip pixels above this roughness
    float ray_max_distance;     // Maximum trace distance
    uint frame_count;           // For temporal jitter
    uint _pad;
};

// ============================================================================
// Normal decoding — Godot stores normals in octahedral encoding
// ============================================================================

vec3 octahedral_decode(vec2 f) {
    f = f * 2.0 - 1.0;
    vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = clamp(-n.z, 0.0, 1.0);
    n.xy += mix(vec2(t), vec2(-t), greaterThanEqual(n.xy, vec2(0.0)));
    return normalize(n);
}

// ============================================================================
// Depth → world position reconstruction
// ============================================================================

vec3 reconstruct_world_position(vec2 uv, float depth) {
    // NDC (Vulkan: depth in [0,1], Y up)
    vec4 clip = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 view_pos = inv_projection * clip;
    view_pos /= view_pos.w;
    vec4 world_pos = inv_view * view_pos;
    return world_pos.xyz;
}

// ============================================================================
// AABB slab test (from bvh_traverse.comp.glsl)
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
// Moller-Trumbore triangle intersection (from bvh_traverse.comp.glsl)
// ============================================================================

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
// Safe inverse direction
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
// BVH traversal — iterative with local array stack
// ============================================================================
// Using local array stack: 16x16 workgroups = 256 threads.
// Shared memory for 256 threads would be 256*24*4*2 = 48KB — too much.
// Local arrays are fine at 16x16 since the stack is small per thread.

const uint STACK_DEPTH = 24u;

bool trace_bvh(vec3 origin, vec3 direction, float t_min, float t_max,
               out float hit_t, out vec3 hit_pos, out vec3 hit_normal) {
    vec3 inv_dir = safe_inv_direction(direction);
    float best_t = t_max;
    vec3 best_pos = vec3(0.0);
    vec3 best_normal = vec3(0.0);
    bool found_hit = false;

    // Test root AABB
    float root_tmin, root_tmax;
    if (!ray_aabb(origin, inv_dir, t_min, best_t,
                  bvh_nodes[0].bounds_min, bvh_nodes[0].bounds_max,
                  root_tmin, root_tmax)) {
        hit_t = t_max;
        hit_pos = vec3(0.0);
        hit_normal = vec3(0.0);
        return false;
    }

    // Traversal stack (local memory)
    uint stack_node[STACK_DEPTH];
    float stack_tmin[STACK_DEPTH];
    int sp = 0;
    stack_node[0] = 0u;
    stack_tmin[0] = root_tmin;
    sp = 1;

    while (sp > 0) {
        sp--;
        uint node_idx = stack_node[sp];
        float entry_tmin = stack_tmin[sp];

        if (entry_tmin > best_t) continue;

        uint node_left_first = bvh_nodes[node_idx].left_first;
        uint node_count = bvh_nodes[node_idx].count;

        if (node_count > 0u) {
            // Leaf: test triangles
            for (uint i = 0u; i < node_count; i++) {
                uint tri_idx = node_left_first + i;
                vec3 hp, hn;
                if (ray_triangle(origin, direction, t_min,
                                 triangles[tri_idx].v0,
                                 triangles[tri_idx].edge1,
                                 triangles[tri_idx].edge2,
                                 triangles[tri_idx].normal,
                                 best_t, hp, hn)) {
                    best_pos = hp;
                    best_normal = hn;
                    found_hit = true;
                }
            }
        } else {
            // Internal: test children
            uint left = node_idx + 1u;
            uint right = node_left_first;

            float tmin_l, tmax_l, tmin_r, tmax_r;
            bool hit_l = ray_aabb(origin, inv_dir, t_min, best_t,
                                  bvh_nodes[left].bounds_min, bvh_nodes[left].bounds_max,
                                  tmin_l, tmax_l);
            bool hit_r = ray_aabb(origin, inv_dir, t_min, best_t,
                                  bvh_nodes[right].bounds_min, bvh_nodes[right].bounds_max,
                                  tmin_r, tmax_r);

            hit_l = hit_l && (tmin_l <= best_t);
            hit_r = hit_r && (tmin_r <= best_t);

            if (hit_l && hit_r) {
                if (tmin_l < tmin_r) {
                    stack_node[sp] = right; stack_tmin[sp] = tmin_r; sp++;
                    stack_node[sp] = left;  stack_tmin[sp] = tmin_l; sp++;
                } else {
                    stack_node[sp] = left;  stack_tmin[sp] = tmin_l; sp++;
                    stack_node[sp] = right; stack_tmin[sp] = tmin_r; sp++;
                }
            } else if (hit_l) {
                stack_node[sp] = left; stack_tmin[sp] = tmin_l; sp++;
            } else if (hit_r) {
                stack_node[sp] = right; stack_tmin[sp] = tmin_r; sp++;
            }
        }
    }

    hit_t = best_t;
    hit_pos = best_pos;
    hit_normal = best_normal;
    return found_hit;
}

// ============================================================================
// Main
// ============================================================================

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(reflection_output);

    if (pixel.x >= size.x || pixel.y >= size.y) return;

    vec2 uv = (vec2(pixel) + 0.5) / vec2(size);

    // ---- Read depth ----
    float depth = texture(depth_sampler, uv).r;

    // Sky pixels (depth == 1.0 in Vulkan) — no reflection needed.
    if (depth >= 1.0) {
        imageStore(reflection_output, pixel, vec4(0.0, 0.0, 0.0, 0.0));
        return;
    }

    // ---- Read normal + roughness ----
    vec4 nr = texture(normal_roughness_sampler, uv);
    vec3 world_normal = octahedral_decode(nr.xy);
    float roughness = nr.z;

    // Skip rough surfaces — Godot's SSR handles those adequately.
    if (roughness > roughness_threshold) {
        imageStore(reflection_output, pixel, vec4(0.0, 0.0, 0.0, 0.0));
        return;
    }

    // ---- Reconstruct world position ----
    vec3 world_pos = reconstruct_world_position(uv, depth);

    // ---- Compute view direction and reflection ----
    vec3 camera_pos = inv_view[3].xyz;
    vec3 view_dir = normalize(world_pos - camera_pos);
    vec3 reflect_dir = reflect(view_dir, world_normal);

    // Small offset along normal to avoid self-intersection.
    vec3 ray_origin = world_pos + world_normal * 0.01;

    // ---- Trace reflection ray ----
    float hit_t;
    vec3 hit_pos, hit_normal;
    bool hit = trace_bvh(ray_origin, reflect_dir, 0.0, ray_max_distance,
                         hit_t, hit_pos, hit_normal);

    if (hit) {
        // Basic shading: use hit normal for a simple directional light approximation.
        // For now, output the hit normal encoded as color (will be enhanced with material data).
        float ndotl = max(dot(hit_normal, -view_dir), 0.0);
        vec3 color = vec3(0.5 + 0.5 * ndotl); // Gray-scale placeholder
        float confidence = 1.0 - (roughness / roughness_threshold); // Fade near threshold
        imageStore(reflection_output, pixel, vec4(color, confidence));
    } else {
        // Miss — write zero alpha so the composite pass knows to skip.
        imageStore(reflection_output, pixel, vec4(0.0, 0.0, 0.0, 0.0));
    }
}
