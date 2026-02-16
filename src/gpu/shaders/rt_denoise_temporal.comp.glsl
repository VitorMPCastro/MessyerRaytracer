#version 450

// rt_denoise_temporal.comp.glsl — Temporal accumulation with motion rejection.
//
// Pass 3 of the RT reflection pipeline:
//   Exponential moving average between current and previous frame's result.
//   Discards stale history based on depth differences (motion rejection).

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

// ============================================================================
// Descriptor set 0: Current frame data
// ============================================================================

layout(set = 0, binding = 0) uniform sampler2D current_reflection;
layout(set = 0, binding = 1) uniform sampler2D depth_sampler;

// ============================================================================
// Descriptor set 1: Previous frame history + output
// ============================================================================

layout(set = 1, binding = 0) uniform sampler2D prev_reflection;
layout(set = 1, binding = 1, rgba16f) restrict writeonly uniform image2D temporal_output;

// ============================================================================
// Push constants
// ============================================================================

layout(push_constant, std430) uniform Params {
    float blend_factor;     // Current frame contribution (default: 0.1 = 90% history)
    float depth_threshold;  // Depth difference rejection threshold
    uint frame_count;       // Frame counter (for first frame detection)
    uint _pad;
};

// ============================================================================
// Main
// ============================================================================

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(temporal_output);

    if (pixel.x >= size.x || pixel.y >= size.y) return;

    vec2 uv = (vec2(pixel) + 0.5) / vec2(size);

    vec4 current = texture(current_reflection, uv);
    vec4 history = texture(prev_reflection, uv);

    // First frame: no history available, use current directly.
    if (frame_count == 0u) {
        imageStore(temporal_output, pixel, current);
        return;
    }

    // No reflection in current frame — decay history.
    if (current.a <= 0.0) {
        vec4 decayed = history * 0.9;
        imageStore(temporal_output, pixel, decayed);
        return;
    }

    // No history — use current.
    if (history.a <= 0.0) {
        imageStore(temporal_output, pixel, current);
        return;
    }

    // ---- Motion rejection via depth comparison ----
    // If depth has changed significantly, the pixel likely belongs to a different
    // surface — reject history to avoid ghosting.
    float current_depth = texture(depth_sampler, uv).r;
    // We approximate previous depth from history alpha (could be improved with motion vectors).
    float depth_diff = abs(current.a - history.a);
    float rejection = step(depth_threshold, depth_diff);

    // Blend factor: use full current frame for rejected pixels, otherwise EMA.
    float alpha = mix(blend_factor, 1.0, rejection);

    vec4 result = mix(history, current, alpha);
    imageStore(temporal_output, pixel, result);
}
