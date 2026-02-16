#version 450

// rt_composite.comp.glsl — Fresnel-weighted reflection composite into color buffer.
//
// Pass 4 of the RT reflection pipeline:
//   Blends denoised reflections into Godot's color buffer using physically-based
//   Fresnel-Schlick weighting and roughness falloff.

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

// ============================================================================
// Descriptor set 0: Scene textures
// ============================================================================

layout(set = 0, binding = 0) uniform sampler2D depth_sampler;
layout(set = 0, binding = 1) uniform sampler2D normal_roughness_sampler;

// ============================================================================
// Descriptor set 1: Reflection data + color buffer
// ============================================================================

layout(set = 1, binding = 0) uniform sampler2D reflection_sampler;
layout(set = 1, binding = 1, rgba16f) restrict uniform image2D color_buffer;

// ============================================================================
// Push constants
// ============================================================================

layout(push_constant, std430) uniform Params {
    mat4 inv_view;
    float roughness_threshold;  // Must match trace pass
    float reflection_intensity; // Overall intensity multiplier
    float f0;                   // Base reflectivity (default 0.04 for dielectrics)
    uint _pad;
};

// ============================================================================
// Normal decoding
// ============================================================================

vec3 octahedral_decode(vec2 f) {
    f = f * 2.0 - 1.0;
    vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = clamp(-n.z, 0.0, 1.0);
    n.xy += mix(vec2(t), vec2(-t), greaterThanEqual(n.xy, vec2(0.0)));
    return normalize(n);
}

// ============================================================================
// Fresnel-Schlick approximation
// ============================================================================

float fresnel_schlick(float cos_theta, float f0_val) {
    return f0_val + (1.0 - f0_val) * pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0);
}

// ============================================================================
// Main
// ============================================================================

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(color_buffer);

    if (pixel.x >= size.x || pixel.y >= size.y) return;

    vec2 uv = (vec2(pixel) + 0.5) / vec2(size);

    // ---- Read reflection ----
    vec4 reflection = texture(reflection_sampler, uv);

    // No reflection data — skip.
    if (reflection.a <= 0.001) return;

    // ---- Read depth + normal ----
    float depth = texture(depth_sampler, uv).r;
    if (depth >= 1.0) return; // Sky

    vec4 nr = texture(normal_roughness_sampler, uv);
    vec3 world_normal = octahedral_decode(nr.xy);
    float roughness = nr.z;

    // ---- Compute view direction ----
    vec3 camera_pos = inv_view[3].xyz;
    // Approximate view direction from UV (simplified — could use full projection)
    // For the composite pass this is good enough.
    vec3 view_dir = normalize(vec3(uv * 2.0 - 1.0, -1.0));
    // Transform to world space
    view_dir = normalize((inv_view * vec4(view_dir, 0.0)).xyz);

    float NdotV = max(dot(world_normal, -view_dir), 0.001);

    // ---- Fresnel-Schlick weighting ----
    float fresnel = fresnel_schlick(NdotV, f0);

    // ---- Roughness falloff ----
    // Smooth transition to zero as roughness approaches the threshold.
    float roughness_weight = 1.0 - smoothstep(0.0, roughness_threshold, roughness);

    // ---- Final blend ----
    float blend = fresnel * roughness_weight * reflection.a * reflection_intensity;
    blend = clamp(blend, 0.0, 1.0);

    // Read existing color, blend reflection in.
    vec4 existing_color = imageLoad(color_buffer, pixel);
    vec3 composited = mix(existing_color.rgb, reflection.rgb, blend);

    imageStore(color_buffer, pixel, vec4(composited, existing_color.a));
}
