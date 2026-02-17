#version 450

// ============================================================================
// pt_shade.comp.glsl — Wavefront Shade kernel for GPU path tracing.
//
// WHAT:  Evaluates surface interactions, generates shadow rays (NEE) and
//        bounce rays, manages path state (throughput, accumulation, RNG).
//        When bounce > max_bounces (finalize mode), applies the last pending
//        NEE contribution, tone maps, and writes final RGBA to accum_buffer.
//
// WHY:   The wavefront architecture separates shading from ray tracing
//        (Laine, Karras, Aila — "Megakernels Considered Harmful", HPG 2013).
//        This kernel handles all shading logic: surface extraction, PBR BRDF,
//        stochastic NEE with deferred shadow application, and bounce sampling.
//
// HOW:   Stochastic single-light NEE: each pixel randomly selects one light,
//        computes Cook-Torrance BRDF (unshadowed), stores the contribution
//        (pre-multiplied by throughput) in path_state.potential_nee.  The
//        Connect kernel traces the shadow ray.  In the NEXT Shade pass (or
//        finalize), potential_nee is multiplied by shadow visibility and
//        accumulated.  This decouples shadow tracing from radiance evaluation.
//
// PIPELINE STAGE:  Third kernel per bounce.
//   Generate → Extend → **Shade** → Connect → (repeat)
//   After last bounce: one extra Shade dispatch in finalize mode.
//
// REFERENCES:
//   Walter et al., "Microfacet Models for Refraction" (EGSR 2007) — GGX NDF
//   Duff et al., "Building an ONB, Revisited" (JCGT 2017) — branchless ONB
//   Hill, "A Closer Look at ACES" (2016) — fitted ACES tone mapping curve
// ============================================================================

layout(local_size_x = 128, local_size_y = 1, local_size_z = 1) in;

// ============================================================================
// Constants
// ============================================================================

const float PI      = 3.14159265358979323846;
const float INV_PI  = 1.0 / PI;
const float EPSILON = 1e-7;
const float SHADOW_BIAS = 1e-3;
const float DIR_LIGHT_MAX_DIST = 1000.0;

const uint LIGHT_DIRECTIONAL = 0;
const uint LIGHT_POINT = 1;
const uint LIGHT_SPOT = 2;

// ============================================================================
// Data structures — must match api/gpu_types.h (std430)
// ============================================================================

struct GPURay {
    vec3 origin;    float t_max;
    vec3 direction; float t_min;
};

struct GPUIntersection {
    float t;        int prim_id;
    float bary_u;   float bary_v;
    vec3 normal;    uint hit_layers;
};

struct GPUPathState {
    vec3  throughput; uint rng_state;
    vec3  accum;      uint flags;
    vec3  potential_nee; float _pad3;
};

struct GPUMaterial {
    vec3  albedo;    float metallic;
    vec3  emission;  float roughness;
    float specular;  float emission_energy;
    float normal_scale; uint tex_flags;
    int   albedo_tex_idx; int normal_tex_idx;
    int   tex_width;      int tex_height;
};

struct GPULight {
    vec3  position;  float range;
    vec3  direction; float attenuation;
    vec3  color;     float spot_angle;
    uint  type;      float spot_angle_attenuation;
    uint  cast_shadows; uint _pad;
};

struct GPUEnvironment {
    vec3  sky_zenith;   float ambient_energy;
    vec3  sky_horizon;  float ambient_r;
    vec3  sky_ground;   float ambient_g;
    float ambient_b;    int tonemap_mode;
    uint  has_panorama; uint _pad;
};

// Per-triangle scene data — tightly packed floats, NO vec3 alignment.
// These match the C++ struct layouts: TriangleUV (24B), TriangleNormals (36B),
// TriangleTangents (48B).  We access them as flat float arrays to avoid
// std430 vec3 alignment issues.

struct GPUTriangle {
    vec3  v0;     uint id;
    vec3  edge1;  uint layers;
    vec3  edge2;  float _pad2;
    vec3  normal; float _pad3;
};

// ============================================================================
// Buffers
// ============================================================================

layout(set = 0, binding = 0, std430) restrict buffer RayBuffer {
    GPURay rays[];
};

layout(set = 0, binding = 1, std430) restrict readonly buffer IntersectionBuffer {
    GPUIntersection intersections[];
};

layout(set = 0, binding = 2, std430) restrict buffer PathStateBuffer {
    GPUPathState path_states[];
};

layout(set = 0, binding = 3, std430) restrict writeonly buffer ShadowRayBuffer {
    GPURay shadow_rays[];
};

layout(set = 0, binding = 4, std430) restrict readonly buffer ShadowResultBuffer {
    uint shadow_results[];
};

layout(set = 0, binding = 5, std430) restrict writeonly buffer AccumBuffer {
    vec4 accum_out[];
};

layout(set = 0, binding = 6, std430) restrict readonly buffer MaterialBuffer {
    GPUMaterial materials[];
};

layout(set = 0, binding = 7, std430) restrict readonly buffer MaterialIdBuffer {
    uint material_ids[];
};

layout(set = 0, binding = 8, std430) restrict readonly buffer LightBuffer {
    GPULight lights[];
};

layout(set = 0, binding = 9, std430) restrict readonly buffer EnvironmentBuffer {
    GPUEnvironment env;
};

// Triangle UVs — 6 floats per triangle (uv0.x, uv0.y, uv1.x, uv1.y, uv2.x, uv2.y)
layout(set = 0, binding = 10, std430) restrict readonly buffer TriangleUVBuffer {
    float tri_uvs[];  // Indexed: tri_uvs[prim_id * 6 + component]
};

// Triangle normals — 9 floats per triangle (n0.x,.y,.z, n1.x,.y,.z, n2.x,.y,.z)
layout(set = 0, binding = 11, std430) restrict readonly buffer TriangleNormalBuffer {
    float tri_normals[];  // Indexed: tri_normals[prim_id * 9 + component]
};

// Triangle tangents — 12 floats per triangle (t0.xyz, t1.xyz, t2.xyz, sign0, sign1, sign2)
layout(set = 0, binding = 12, std430) restrict readonly buffer TriangleTangentBuffer {
    float tri_tangents[];  // Indexed: tri_tangents[prim_id * 12 + component]
};

layout(set = 0, binding = 13, std430) restrict readonly buffer SceneTriBuffer {
    GPUTriangle scene_tris[];
};

// ============================================================================
// Push constants
// ============================================================================

layout(push_constant, std430) uniform PushConstants {
    uint pixel_count;
    uint width;
    uint height;
    uint bounce;
    uint max_bounces;
    uint sample_index;
    uint light_count;
    uint shadows_enabled;
};

// ============================================================================
// PCG32 random number generator — matches pt_generate.comp.glsl / path_state.h
// ============================================================================

uint pcg32_next(inout uint state) {
    uint old = state;
    state = old * 747796405u + 2891336453u;
    uint word = ((old >> ((old >> 28u) + 4u)) ^ old) * 277803737u;
    return (word >> 22u) ^ word;
}

float pcg32_float(inout uint state) {
    return float(pcg32_next(state)) * (1.0 / 4294967296.0);
}

// ============================================================================
// Smooth normal interpolation from per-triangle vertex normals
// ============================================================================

vec3 interpolate_normal(uint prim_id, float u, float v) {
    uint base = prim_id * 9;
    float w = 1.0 - u - v;
    vec3 n0 = vec3(tri_normals[base + 0], tri_normals[base + 1], tri_normals[base + 2]);
    vec3 n1 = vec3(tri_normals[base + 3], tri_normals[base + 4], tri_normals[base + 5]);
    vec3 n2 = vec3(tri_normals[base + 6], tri_normals[base + 7], tri_normals[base + 8]);
    return normalize(n0 * w + n1 * u + n2 * v);
}

// ============================================================================
// UV interpolation
// ============================================================================

vec2 interpolate_uv(uint prim_id, float u, float v) {
    uint base = prim_id * 6;
    float w = 1.0 - u - v;
    vec2 uv0 = vec2(tri_uvs[base + 0], tri_uvs[base + 1]);
    vec2 uv1 = vec2(tri_uvs[base + 2], tri_uvs[base + 3]);
    vec2 uv2 = vec2(tri_uvs[base + 4], tri_uvs[base + 5]);
    return uv0 * w + uv1 * u + uv2 * v;
}

// ============================================================================
// Normal map perturbation (tangent-space to world-space)
// ============================================================================
// TODO: When Texture2DArray support is added, sample normal map texture here.
// For now, normal mapping is skipped on GPU — only smooth normals are used.

// ============================================================================
// Sky color — procedural gradient (matches shade_pass.h)
// ============================================================================

vec3 sky_color(vec3 dir) {
    // TODO: HDR panorama sampling (has_panorama flag).
    float t = dir.y * 0.5 + 0.5;  // [0,1]: 0=down, 1=up
    if (t > 0.5) {
        float s = (t - 0.5) * 2.0;
        return mix(env.sky_horizon, env.sky_zenith, s);
    } else {
        float s = t * 2.0;
        return mix(env.sky_ground, env.sky_horizon, s);
    }
}

// ============================================================================
// Cook-Torrance PBR BRDF building blocks
// ============================================================================

// GGX / Trowbridge-Reitz normal distribution function.
float distribution_ggx(float n_dot_h, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float denom = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom + EPSILON);
}

// Fresnel-Schlick approximation.
float fresnel_schlick(float cos_theta, float f0) {
    float t = 1.0 - cos_theta;
    float t2 = t * t;
    return f0 + (1.0 - f0) * (t2 * t2 * t);
}

// Smith's GGX height-correlated geometry function.
float geometry_smith_ggx(float n_dot_v, float n_dot_l, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float g1_v = 2.0 * n_dot_v / (n_dot_v + sqrt(a2 + (1.0 - a2) * n_dot_v * n_dot_v) + EPSILON);
    float g1_l = 2.0 * n_dot_l / (n_dot_l + sqrt(a2 + (1.0 - a2) * n_dot_l * n_dot_l) + EPSILON);
    return g1_v * g1_l;
}

// ============================================================================
// Light attenuation helpers (matches Godot's model)
// ============================================================================

float compute_distance_attenuation(float distance, float range, float atten_exp) {
    float ratio = distance / range;
    float base = max(1.0 - ratio * ratio, 0.0);
    return pow(base, atten_exp);
}

float compute_spot_attenuation(vec3 light_to_point_dir, vec3 spot_fwd,
        float spot_angle_rad, float spot_atten_exp) {
    float cos_outer = cos(spot_angle_rad);
    float cos_angle = dot(-light_to_point_dir, spot_fwd);
    if (cos_angle <= cos_outer) return 0.0;
    float t = (cos_angle - cos_outer) / (1.0 - cos_outer);
    return pow(max(t, 0.0), spot_atten_exp);
}

// ============================================================================
// SurfaceInfo — extracted material + geometry at a hit point
// ============================================================================

struct SurfaceInfo {
    vec3  normal;
    vec3  position;
    vec3  view_dir;
    float n_dot_v;
    vec3  albedo;
    float metallic;
    float roughness;
    float specular_scale;
    vec3  emission;
    vec3  f0;
    vec3  diff;
};

SurfaceInfo extract_surface(GPUIntersection hit, GPURay ray, uint prim_id) {
    SurfaceInfo s;
    s.position = ray.origin + ray.direction * hit.t;
    s.view_dir = normalize(-ray.direction);

    // Smooth normal from vertex normals.
    s.normal = interpolate_normal(prim_id, hit.bary_u, hit.bary_v);

    // TODO: Normal map perturbation when Texture2DArray is supported.

    s.n_dot_v = max(dot(s.normal, s.view_dir), 0.001);

    // Default material (gray Lambert).
    s.albedo = vec3(0.75);
    s.metallic = 0.0;
    s.roughness = 0.5;
    s.specular_scale = 0.5;
    s.emission = vec3(0.0);

    // Material lookup.
    uint mat_id = material_ids[prim_id];
    GPUMaterial mat = materials[mat_id];
    s.albedo = mat.albedo;
    s.metallic = mat.metallic;
    s.roughness = max(mat.roughness, 0.04);  // Clamp to avoid GGX singularity.
    s.specular_scale = mat.specular;

    // TODO: Albedo texture sampling from Texture2DArray.

    // Emission.
    if (mat.emission_energy > 0.0) {
        s.emission = mat.emission * mat.emission_energy;
    }

    // F0: reflectance at normal incidence.
    float dielectric_f0 = 0.04 * s.specular_scale * 2.0;
    s.f0 = vec3(dielectric_f0) * (1.0 - s.metallic) + s.albedo * s.metallic;

    // Diffuse albedo (metals have no diffuse).
    s.diff = s.albedo * (1.0 - s.metallic);

    return s;
}

// ============================================================================
// Cook-Torrance single-light evaluation (returns unshadowed contribution)
// ============================================================================

vec3 evaluate_light(SurfaceInfo surf, GPULight light) {
    vec3 light_dir;
    float atten = 1.0;

    if (light.type == LIGHT_DIRECTIONAL) {
        light_dir = light.direction;
    } else {
        vec3 to_light = light.position - surf.position;
        float dist = length(to_light);
        if (dist < 1e-6 || dist > light.range) return vec3(0.0);
        light_dir = to_light / dist;
        atten = compute_distance_attenuation(dist, light.range, light.attenuation);

        if (light.type == LIGHT_SPOT) {
            atten *= compute_spot_attenuation(
                -light_dir, light.direction, light.spot_angle, light.spot_angle_attenuation);
        }
    }

    if (atten < 1e-6) return vec3(0.0);

    float n_dot_l = dot(surf.normal, light_dir);
    if (n_dot_l <= 0.0) return vec3(0.0);

    vec3 h = normalize(surf.view_dir + light_dir);
    float n_dot_h = max(dot(surf.normal, h), 0.0);
    float v_dot_h = max(dot(surf.view_dir, h), 0.0);

    float d_term = distribution_ggx(n_dot_h, surf.roughness);
    float g_term = geometry_smith_ggx(surf.n_dot_v, n_dot_l, surf.roughness);
    vec3 f_term = vec3(
        fresnel_schlick(v_dot_h, surf.f0.r),
        fresnel_schlick(v_dot_h, surf.f0.g),
        fresnel_schlick(v_dot_h, surf.f0.b));

    float spec_denom = 4.0 * surf.n_dot_v * n_dot_l + EPSILON;
    float spec_scale = d_term * g_term / spec_denom;
    float diff_scale = INV_PI;

    vec3 lr = light.color * atten;

    return (surf.diff * (vec3(1.0) - f_term) * diff_scale + f_term * spec_scale) * lr * n_dot_l;
}

// ============================================================================
// Shadow ray construction for a single light
// ============================================================================

GPURay make_shadow_ray(vec3 surface_pos, vec3 surface_normal, GPULight light) {
    vec3 origin = surface_pos + surface_normal * SHADOW_BIAS;
    vec3 dir;
    float max_dist;

    if (light.type == LIGHT_DIRECTIONAL) {
        dir = light.direction;
        max_dist = DIR_LIGHT_MAX_DIST;
    } else {
        vec3 to_light = light.position - origin;
        float dist = length(to_light);
        if (dist < 1e-6) {
            // Degenerate — no shadow ray.
            return GPURay(vec3(0), 0.0, vec3(0, 1, 0), 0.0);
        }
        dir = to_light / dist;
        max_dist = dist;
    }

    return GPURay(origin, max_dist, dir, 0.0);
}

// ============================================================================
// ONB construction — Duff et al. (JCGT 2017), branchless
// ============================================================================

void construct_onb(vec3 n, out vec3 tangent, out vec3 bitangent) {
    float s = sign(n.z);
    // Avoid zero-division when n.z ≈ 0.
    if (abs(n.z) < 1e-6) s = 1.0;
    float a = -1.0 / (s + n.z);
    float b = n.x * n.y * a;
    tangent   = vec3(1.0 + s * n.x * n.x * a, s * b, -s * n.x);
    bitangent = vec3(b, s + n.y * n.y * a, -n.y);
}

// ============================================================================
// Cosine-weighted hemisphere sampling (Malley's method)
// ============================================================================
// PDF = cos(θ) / π.  For Lambertian BRDF: BRDF*cos/PDF = albedo (constant).

vec3 cosine_hemisphere_sample(vec3 normal, inout uint rng) {
    float u1 = pcg32_float(rng);
    float u2 = pcg32_float(rng);

    float r   = sqrt(u1);
    float phi = 2.0 * PI * u2;

    float x = r * cos(phi);
    float y = r * sin(phi);
    float z = sqrt(max(0.0, 1.0 - u1));

    vec3 t, b;
    construct_onb(normal, t, b);

    return normalize(t * x + b * y + normal * z);
}

// ============================================================================
// GGX importance sampling (half-vector from Trowbridge-Reitz NDF)
// ============================================================================

vec3 ggx_sample_half(vec3 normal, float roughness, inout uint rng) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float u1 = pcg32_float(rng);
    float u2 = pcg32_float(rng);

    float cos_theta = sqrt((1.0 - u1) / (1.0 + (a2 - 1.0) * u1 + EPSILON));
    float sin_theta = sqrt(max(0.0, 1.0 - cos_theta * cos_theta));
    float phi = 2.0 * PI * u2;

    float lx = sin_theta * cos(phi);
    float ly = sin_theta * sin(phi);
    float lz = cos_theta;

    vec3 t, b;
    construct_onb(normal, t, b);

    return normalize(t * lx + b * ly + normal * lz);
}

// ============================================================================
// Bounce sampling — probabilistic lobe selection (diffuse vs specular)
// ============================================================================
// Returns: bounce direction, throughput weight (BRDF*cos/PDF), validity.
// Uses stochastic lobe selection: spec_prob ~ metallic + (1-metallic)*(1-roughness)*0.5

struct BounceResult {
    vec3  direction;
    vec3  weight;
    bool  valid;
};

BounceResult sample_bounce(SurfaceInfo surf, inout uint rng) {
    BounceResult result;
    result.valid = false;
    result.weight = vec3(0.0);

    float spec_prob = surf.metallic + (1.0 - surf.metallic) * (1.0 - surf.roughness) * 0.5;
    spec_prob = clamp(spec_prob, 0.05, 0.95);

    bool do_specular = pcg32_float(rng) < spec_prob;

    if (do_specular) {
        vec3 h = ggx_sample_half(surf.normal, surf.roughness, rng);
        float v_dot_h = max(dot(surf.view_dir, h), 0.0);
        result.direction = normalize(h * (2.0 * v_dot_h) - surf.view_dir);

        float n_dot_l = dot(surf.normal, result.direction);
        if (n_dot_l <= 0.0) return result;

        float n_dot_h = max(dot(surf.normal, h), 0.0);
        float g = geometry_smith_ggx(surf.n_dot_v, n_dot_l, surf.roughness);
        float fr = fresnel_schlick(v_dot_h, surf.f0.r);
        float fg = fresnel_schlick(v_dot_h, surf.f0.g);
        float fb = fresnel_schlick(v_dot_h, surf.f0.b);

        // Weight = F * G * VdotH / (NdotV * NdotH * spec_prob)
        float common = g * v_dot_h / (surf.n_dot_v * n_dot_h * spec_prob + EPSILON);
        result.weight = vec3(fr, fg, fb) * common;
    } else {
        result.direction = cosine_hemisphere_sample(surf.normal, rng);

        float n_dot_l = dot(surf.normal, result.direction);
        if (n_dot_l <= 0.0) return result;

        // Weight = diff / (1 - spec_prob)
        float inv_prob = 1.0 / (1.0 - spec_prob);
        result.weight = surf.diff * inv_prob;
    }

    result.valid = true;
    return result;
}

// ============================================================================
// Tone mapping operators — matches Godot Environment.tonemapper enum
// ============================================================================
// 0=LINEAR, 1=REINHARDT, 2=FILMIC, 3=ACES, 4=AGX

float tonemap_reinhard(float c) { return c / (c + 1.0); }

float hable_partial(float x) {
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}
float tonemap_filmic(float c) {
    const float W = 11.2;
    return hable_partial(c) / hable_partial(W);
}

float tonemap_aces(float c) {
    const float a = 2.51, b = 0.03, cc = 2.43, d = 0.59, e = 0.14;
    float mapped = (c * (a * c + b)) / (c * (cc * c + d) + e);
    return clamp(mapped, 0.0, 1.0);
}

float tonemap_agx(float c) {
    float x = max(c, 0.0);
    float x2 = x * x;
    return clamp(x2 / (x2 + 0.09 * x + 0.0009), 0.0, 1.0);
}

vec3 tonemap_rgb(vec3 c, int mode) {
    switch (mode) {
        case 0: return c;  // LINEAR
        case 1: return vec3(tonemap_reinhard(c.r), tonemap_reinhard(c.g), tonemap_reinhard(c.b));
        case 2: return vec3(tonemap_filmic(c.r), tonemap_filmic(c.g), tonemap_filmic(c.b));
        case 3: return vec3(tonemap_aces(c.r), tonemap_aces(c.g), tonemap_aces(c.b));
        case 4: return vec3(tonemap_agx(c.r), tonemap_agx(c.g), tonemap_agx(c.b));
        default: return vec3(tonemap_aces(c.r), tonemap_aces(c.g), tonemap_aces(c.b));
    }
}

// ============================================================================
// Main — one thread per pixel
// ============================================================================

void main() {
    uint idx = gl_GlobalInvocationID.x;
    if (idx >= pixel_count) return;

    // ---- Read path state ----
    GPUPathState ps = path_states[idx];
    uint rng = ps.rng_state;
    bool is_active = (ps.flags & 1u) != 0u;

    // ================================================================
    // FINALIZE MODE: bounce > max_bounces
    // ================================================================
    // After the last bounce's Connect pass, we dispatch Shade one more
    // time to apply the final pending NEE and write tone-mapped output.

    if (bounce > max_bounces) {
        // Apply final pending NEE with shadow visibility.
        if (shadows_enabled != 0u && light_count > 0u) {
            float vis = (shadow_results[idx] == 0u) ? 1.0 : 0.0;
            ps.accum += ps.potential_nee * vis;
        } else {
            ps.accum += ps.potential_nee;
        }
        ps.potential_nee = vec3(0.0);

        // Tone map + gamma correction → final RGBA output.
        vec3 c = tonemap_rgb(max(ps.accum, vec3(0.0)), env.tonemap_mode);
        c = pow(max(c, vec3(0.0)), vec3(1.0 / 2.2));
        accum_out[idx] = vec4(c, 1.0);

        ps.rng_state = rng;
        path_states[idx] = ps;
        return;
    }

    // ================================================================
    // Step 1: Apply pending NEE from previous bounce's Connect pass
    // ================================================================

    if (bounce > 0u) {
        if (shadows_enabled != 0u && light_count > 0u) {
            float vis = (shadow_results[idx] == 0u) ? 1.0 : 0.0;
            ps.accum += ps.potential_nee * vis;
        } else {
            ps.accum += ps.potential_nee;
        }
        ps.potential_nee = vec3(0.0);
    }

    // ================================================================
    // Step 2: Skip inactive pixels (already terminated in previous bounce)
    // ================================================================

    if (!is_active) {
        // Write degenerate shadow ray so Connect doesn't read garbage.
        shadow_rays[idx] = GPURay(vec3(0), 0.0, vec3(0, 1, 0), 0.0);
        ps.rng_state = rng;
        path_states[idx] = ps;
        return;
    }

    // ================================================================
    // Step 3: Read intersection from Extend kernel
    // ================================================================

    GPUIntersection hit = intersections[idx];
    GPURay ray = rays[idx];

    // ================================================================
    // Step 4: Miss → accumulate sky, deactivate
    // ================================================================

    if (hit.prim_id < 0) {
        vec3 sky = sky_color(ray.direction);
        ps.accum += ps.throughput * sky;
        ps.flags &= ~1u;  // deactivate

        // Degenerate ray and shadow ray (no intersection, nothing to shade).
        rays[idx] = GPURay(vec3(0), 0.0, vec3(0, 1, 0), 0.0);
        shadow_rays[idx] = GPURay(vec3(0), 0.0, vec3(0, 1, 0), 0.0);
        ps.rng_state = rng;
        path_states[idx] = ps;
        return;
    }

    // ================================================================
    // Step 5: Hit — extract surface properties
    // ================================================================

    uint prim_id = uint(hit.prim_id);
    SurfaceInfo surf = extract_surface(hit, ray, prim_id);

    // ---- Accumulate emission ----
    ps.accum += ps.throughput * surf.emission;

    // ---- Ambient (first bounce only, matches CPU path tracer) ----
    if (bounce == 0u) {
        vec3 ambient = vec3(env.ambient_r, env.ambient_g, env.ambient_b);
        ps.accum += ps.throughput * surf.diff * ambient * env.ambient_energy;
    }

    // ================================================================
    // Step 6: Stochastic NEE — randomly select one light
    // ================================================================
    // Compute unshadowed direct illumination from one randomly chosen light.
    // Scale by N (number of lights) to get an unbiased estimator.
    // Store potential contribution in path_state.potential_nee for deferred
    // shadow application after the Connect kernel.

    if (light_count > 0u) {
        // Uniform random light selection.
        uint selected_light = pcg32_next(rng) % light_count;
        GPULight light = lights[selected_light];
        float light_weight = float(light_count);  // = 1/pdf = N

        vec3 nee = evaluate_light(surf, light) * light_weight;
        ps.potential_nee = ps.throughput * nee;

        // Generate shadow ray to the selected light.
        if (light.cast_shadows != 0u && shadows_enabled != 0u) {
            shadow_rays[idx] = make_shadow_ray(surf.position, surf.normal, light);
        } else {
            // No shadow for this light → use degenerate ray that
            // Connect reports as "not occluded" (t_min >= t_max → early exit).
            shadow_rays[idx] = GPURay(vec3(0), 0.0, vec3(0, 1, 0), 0.0);
        }
    } else {
        ps.potential_nee = vec3(0.0);
        shadow_rays[idx] = GPURay(vec3(0), 0.0, vec3(0, 1, 0), 0.0);
    }

    // ================================================================
    // Step 7: Last bounce — no more bouncing, but shadow rays were already
    //         generated for NEE. Deactivate; finalize pass will accumulate.
    // ================================================================

    if (bounce >= max_bounces) {
        ps.flags &= ~1u;  // deactivate
        rays[idx] = GPURay(vec3(0), 0.0, vec3(0, 1, 0), 0.0);
        ps.rng_state = rng;
        path_states[idx] = ps;
        return;
    }

    // ================================================================
    // Step 8: Sample bounce direction
    // ================================================================

    BounceResult br = sample_bounce(surf, rng);

    if (!br.valid) {
        ps.flags &= ~1u;  // deactivate
        rays[idx] = GPURay(vec3(0), 0.0, vec3(0, 1, 0), 0.0);
        ps.rng_state = rng;
        path_states[idx] = ps;
        return;
    }

    // Update throughput.
    ps.throughput *= br.weight;

    // ================================================================
    // Step 9: Russian roulette (bounce >= 2)
    // ================================================================

    if (bounce >= 2u) {
        float max_t = max(ps.throughput.r, max(ps.throughput.g, ps.throughput.b));
        float survival = min(max_t, 0.95);
        if (pcg32_float(rng) >= survival) {
            ps.flags &= ~1u;  // deactivate — path contribution is in potential_nee
            rays[idx] = GPURay(vec3(0), 0.0, vec3(0, 1, 0), 0.0);
            ps.rng_state = rng;
            path_states[idx] = ps;
            return;
        }
        ps.throughput /= survival;
    }

    // ================================================================
    // Step 10: Write bounce ray for next Extend pass
    // ================================================================

    vec3 bounce_origin = surf.position + surf.normal * SHADOW_BIAS;
    rays[idx] = GPURay(bounce_origin, 1e30, br.direction, 1e-4);

    // ---- Write back path state ----
    ps.rng_state = rng;
    path_states[idx] = ps;
}
