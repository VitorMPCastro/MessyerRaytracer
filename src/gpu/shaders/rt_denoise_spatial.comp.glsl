#version 450

// rt_denoise_spatial.comp.glsl — Cross-bilateral spatial denoising filter.
//
// Pass 2 of the RT reflection pipeline:
//   Edge-preserving 5x5 bilateral filter using depth + normal as guide signals.
//   Reduces single-sample noise while keeping contact reflections sharp.

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

// ============================================================================
// Descriptor set 0: Input textures
// ============================================================================

layout(set = 0, binding = 0) uniform sampler2D depth_sampler;
layout(set = 0, binding = 1) uniform sampler2D normal_roughness_sampler;
layout(set = 0, binding = 2) uniform sampler2D reflection_input;

// ============================================================================
// Descriptor set 1: Output
// ============================================================================

layout(set = 1, binding = 0, rgba16f) restrict writeonly uniform image2D denoised_output;

// ============================================================================
// Push constants
// ============================================================================

layout(push_constant, std430) uniform Params {
    float depth_sigma;      // Depth weight falloff (default: 1.0)
    float normal_sigma;     // Normal weight falloff (default: 128.0)
    float color_sigma;      // Color weight falloff (default: 4.0)
    int kernel_radius;      // Filter radius (default: 2 for 5x5)
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
// Main
// ============================================================================

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(denoised_output);

    if (pixel.x >= size.x || pixel.y >= size.y) return;

    vec2 uv = (vec2(pixel) + 0.5) / vec2(size);
    vec2 texel_size = 1.0 / vec2(size);

    // ---- Read center pixel ----
    vec4 center_color = texture(reflection_input, uv);
    float center_depth = texture(depth_sampler, uv).r;
    vec4 center_nr = texture(normal_roughness_sampler, uv);
    vec3 center_normal = octahedral_decode(center_nr.xy);

    // No reflection data — pass through.
    if (center_color.a <= 0.0) {
        imageStore(denoised_output, pixel, vec4(0.0));
        return;
    }

    // Sky — pass through.
    if (center_depth >= 1.0) {
        imageStore(denoised_output, pixel, vec4(0.0));
        return;
    }

    // ---- Bilateral filter ----
    vec4 sum = vec4(0.0);
    float weight_sum = 0.0;

    int radius = clamp(kernel_radius, 1, 4);

    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            vec2 offset_uv = uv + vec2(float(dx), float(dy)) * texel_size;

            // Clamp to valid UV range.
            offset_uv = clamp(offset_uv, vec2(0.0), vec2(1.0));

            vec4 sample_color = texture(reflection_input, offset_uv);
            float sample_depth = texture(depth_sampler, offset_uv).r;
            vec4 sample_nr = texture(normal_roughness_sampler, offset_uv);
            vec3 sample_normal = octahedral_decode(sample_nr.xy);

            // Skip samples with no reflection data.
            if (sample_color.a <= 0.0) continue;

            // ---- Depth weight ----
            float depth_diff = abs(center_depth - sample_depth);
            float w_depth = exp(-depth_diff * depth_diff * depth_sigma);

            // ---- Normal weight ----
            float normal_dot = max(dot(center_normal, sample_normal), 0.0);
            float w_normal = pow(normal_dot, normal_sigma);

            // ---- Color weight ----
            vec3 color_diff = center_color.rgb - sample_color.rgb;
            float w_color = exp(-dot(color_diff, color_diff) * color_sigma);

            // ---- Spatial weight (Gaussian) ----
            float dist2 = float(dx * dx + dy * dy);
            float w_spatial = exp(-dist2 / (2.0 * float(radius * radius) + 0.001));

            float weight = w_depth * w_normal * w_color * w_spatial;
            sum += sample_color * weight;
            weight_sum += weight;
        }
    }

    vec4 result = (weight_sum > 0.001) ? sum / weight_sum : center_color;
    imageStore(denoised_output, pixel, result);
}
