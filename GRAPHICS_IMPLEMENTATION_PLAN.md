# Realistic Graphics Pipeline — Implementation Plan

> **Temporary working document.** Delete after all phases are complete.
> Each phase is self-contained: build + verify after each one.

## Architecture Pivot (Feb 2026)

After completing Phases 1-2 (CPU-side material/UV extraction + texture sampling), research
confirmed that **primary rendering must use Godot's GPU rasterizer** — duplicating shading
on the CPU cannot approach real-time at useful resolutions. This is the universal industry
approach: every shipping RT game uses **hybrid rendering** (rasterize primary, enhance with
sparse RT effects, denoise).

### New Strategy

| Layer | Owner | Why |
|-------|-------|-----|
| Primary visibility + PBR shading | **Godot Forward+ renderer** | GPU rasterizer, already built, hardware-optimized |
| Global illumination | **Godot GI** (SDFGI / VoxelGI / LightmapGI) | Mature, tuned for real-time |
| Screen-space effects | **Godot built-in** (SSR, SSAO, SSIL) | Cheap, good baseline quality |
| Post-processing | **Godot built-in** (tonemapping, glow, fog, DOF) | Zero-cost integration |
| RT reflections / AO / shadows | **Our raytracer via CompositorEffect** | Exact BVH queries beat screen-space limits |
| Gameplay ray queries | **Our raytracer (core strength)** | LOS, ballistics, AI perception, audio occlusion |

### Key Decisions

- **Godot's renderer handles all pixel shading** — materials, lights, shadows, GI, post-processing.
- **Graphics module becomes a scene configurator** — `RaySceneSetup` node programmatically creates/configures `WorldEnvironment`, `DirectionalLight3D`, GI nodes, and sets optimal rendering settings.
- **CompositorEffect integration** — Our GPU BVH compute shader reads Godot's depth/normal buffers, traces secondary rays (reflections, AO, shadows), writes results back into the color buffer. This is injected at `EFFECT_CALLBACK_TYPE_POST_TRANSPARENT`.
- **RayRenderer kept as debug/preview tool** — The existing CPU pipeline (Phases 1-2) remains useful for AOV visualization (normals, depth, barycentrics, prim_id, albedo) in the editor panel.
- **Phase 1-2 data extraction preserved** — `MaterialData`, `TriangleUV`, `SceneShadeData` feed both the debug renderer and future CompositorEffect material lookups.

---

## Phase 1: Material Data Extraction + Albedo Shading ✅ COMPLETE

### New Files
- [x] `src/core/material_data.h` — `MaterialData` POD struct (albedo Color, metallic, roughness, specular, emission Color+energy)
- [x] `src/api/scene_shade_data.h` — `SceneShadeData` lightweight view struct (pointers to material arrays)

### Modified Files
- [x] `src/api/ray_service.h` — Added `virtual SceneShadeData get_shade_data() const = 0` to `IRayService`
- [x] `src/godot/raytracer_server.h` — Added material storage to `RegisteredMesh`, scene-level material arrays, `get_scene_shade_data()` method, updated `_extract_object_triangles` signature
- [x] `src/godot/raytracer_server.cpp` — Material extraction via `BaseMaterial3D` in `_extract_object_triangles`, parallel material flattening in `_rebuild_scene`, `get_scene_shade_data()` implementation
- [x] `src/godot/ray_service_bridge.cpp` — Bridged `get_shade_data()` to `get_scene_shade_data()`
- [x] `src/modules/graphics/ray_image.h` — Added `ALBEDO = 7` channel (CHANNEL_COUNT now 8)
- [x] `src/modules/graphics/shade_pass.h` — Added `shade_material()` (Lambert with material albedo), `shade_albedo()` (pure color), updated `shade_all()` to accept `SceneShadeData`
- [x] `src/modules/graphics/ray_renderer.h` — Added `CHANNEL_ALBEDO = 7` to enum
- [x] `src/modules/graphics/ray_renderer.cpp` — Fetches `SceneShadeData` via `_get_service()->get_shade_data()`, passes to `ShadePass::shade_all()`, updated bindings
- [x] Build + verify ✅

---

## Phase 2: UV Storage + Texture Sampling ✅ COMPLETE

### New Files
- [x] `src/core/triangle_uv.h` — `TriangleUV` struct with `Vector2 uv0, uv1, uv2` + `interpolate(u, v)` helper
- [x] `src/modules/graphics/texture_sampler.h` — `TextureSampler` namespace: `sample_nearest()` and `sample_bilinear()` with repeat wrapping

### Modified Files
- [x] `src/api/scene_shade_data.h` — Added `const TriangleUV* triangle_uvs` field
- [x] `src/core/material_data.h` — Added `Ref<Image> albedo_texture`, `tex_width`, `tex_height`, `has_albedo_texture`
- [x] `src/godot/raytracer_server.h` — Added `scene_triangle_uvs_` vector, `object_triangle_uvs` in RegisteredMesh, updated `_extract_object_triangles` signature
- [x] `src/godot/raytracer_server.cpp` — Extracts `ARRAY_TEX_UV` per surface, `BaseMaterial3D::get_texture(TEXTURE_ALBEDO)` → decompress → store in MaterialData. Flattens UVs in `_rebuild_scene`, exposes via `get_scene_shade_data()`
- [x] `src/modules/graphics/shade_pass.h` — `shade_material()` and `shade_albedo()` interpolate UV from barycentrics, sample texture with bilinear filtering, multiply with albedo color
- [x] Build + verify ✅

---

## Phase 3: Godot Rendering Environment Setup

> Make the scene look as realistic as possible using Godot's built-in renderer.
> No custom ray tracing here — pure Godot node configuration from C++.

### New Files
- [ ] `src/modules/graphics/ray_scene_setup.h` — Header for `RaySceneSetup` Node3D
- [ ] `src/modules/graphics/ray_scene_setup.cpp` — Implementation

### `RaySceneSetup` Node3D
A GDScript-configurable node that programmatically creates and manages the rendering environment:

**Child nodes it creates/manages:**
- `WorldEnvironment` with `Environment` resource
- `DirectionalLight3D` (sun)
- GI node (one of: `VoxelGI`, `SDFGI` via Environment, or `LightmapGI`)

**Exposed properties (GDScript-bindable):**
- `gi_mode` enum: `SDFGI`, `VOXEL_GI`, `LIGHTMAP_GI`, `NONE`
- `sky_texture` (panorama HDR)
- `sun_direction`, `sun_color`, `sun_energy`
- `ambient_mode`, `ambient_color`, `ambient_energy`
- `tonemap_mode` enum: `LINEAR`, `REINHARD`, `FILMIC`, `ACES`, `AGX`
- `tonemap_exposure`, `tonemap_white`
- `ssr_enabled`, `ssr_max_steps`
- `ssao_enabled`, `ssao_radius`, `ssao_intensity`
- `ssil_enabled`, `ssil_radius`, `ssil_intensity`
- `sdfgi_cascades`, `sdfgi_min_cell_size`, `sdfgi_use_occlusion`
- `glow_enabled`, `glow_intensity`, `glow_bloom`
- `fog_enabled`, `fog_density`, `fog_height`
- `dof_enabled`, `dof_focus_distance`, `dof_blur_amount`
- `auto_exposure_enabled`, `auto_exposure_scale`

**Methods:**
- `apply()` — Creates/updates child nodes + Environment resource from current properties
- `apply_preset(preset: int)` — Apply a quality preset (LOW, MEDIUM, HIGH, ULTRA)

### Implementation Notes
- On `_ready()` or `apply()`, creates `WorldEnvironment` + `Environment` resource
- Sets `Environment::set_sdfgi_enabled()`, `::set_ssr_enabled()`, `::set_ssao_enabled()` etc.
- Manages `DirectionalLight3D` child: shadow mode, soft shadow size, cascade settings
- For VoxelGI mode: creates `VoxelGI` child, auto-bakes on `apply()`
- Quality presets tune all parameters together for target hardware

### Key Includes
```cpp
#include <godot_cpp/classes/world_environment.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/sky.hpp>
#include <godot_cpp/classes/panorama_sky_material.hpp>
#include <godot_cpp/classes/procedural_sky_material.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/voxel_gi.hpp>
#include <godot_cpp/classes/camera_attributes_practical.hpp>
```

### Modified Files
- [ ] `src/register_types.cpp` — Register `RaySceneSetup` class
- [ ] Build + verify

---

## Phase 4: Quality Presets + Auto-Configuration

> Pre-tuned parameter sets for different hardware tiers + a GDScript-friendly API.

### Modified Files
- [ ] `src/modules/graphics/ray_scene_setup.cpp` — Add preset system

### Presets

**LOW** (integrated GPU / low-end):
- GI: None (ambient color only)
- SSR: off, SSAO: off, SSIL: off
- Tonemapping: Filmic
- Shadows: 2 cascades, hard shadows
- Glow: off

**MEDIUM** (GTX 1650 Ti class):
- GI: SDFGI (4 cascades, default cell size)
- SSR: on (half-res), SSAO: on (half-res), SSIL: off
- Tonemapping: ACES
- Shadows: 4 cascades, soft shadows (PCSS)
- Glow: on (bloom 0.1)

**HIGH** (RTX 3060+ class):
- GI: SDFGI (6 cascades) + SSIL on
- SSR: on (full-res), SSAO: on (full-res), SSIL: on
- Tonemapping: AgX
- Shadows: 4 cascades, PCSS + shadowmask if LightmapGI
- Glow: on, volumetric fog: on
- DOF: available

**ULTRA** (RTX 4070+ class):
- Everything in HIGH + CompositorEffect RT passes (Phase 5-6)

### GDScript Usage
```gdscript
var setup = $RaySceneSetup
setup.apply_preset(RaySceneSetup.PRESET_MEDIUM)
# Override individual settings after preset:
setup.sun_energy = 1.5
setup.apply()
```

- [ ] Build + verify

---

## Phase 5: RT-Enhanced Reflections via CompositorEffect

> Replace Godot's screen-space reflections with true ray-traced reflections
> using our BVH compute shader. This is the industry hybrid approach.
>
> **Reference implementations studied**: sphynx-owner/JFA_driven_motion_blur_addon (338★),
> ARez2/compositor-effect-lens-effects (21★). See RESEARCH_FINDINGS.md for full analysis.

### New Files
- [ ] `src/modules/graphics/rt_compositor_base.h` — Base class for all RT CompositorEffects (C++ equivalent of community `BaseCompositorEffect`/`enhanced_compositor_effect.gd`)
- [ ] `src/modules/graphics/rt_compositor_base.cpp` — Implementation: RD init, sampler creation, shader compilation, texture management, dispatch helpers, uniform set caching
- [ ] `src/modules/graphics/rt_reflection_effect.h` — Header for the reflection CompositorEffect
- [ ] `src/modules/graphics/rt_reflection_effect.cpp` — Implementation: multi-pass reflection pipeline
- [ ] `src/gpu/shaders/rt_reflections.glsl` — Compute shader: ray generation + BVH trace
- [ ] `src/gpu/shaders/rt_denoise_spatial.glsl` — Compute shader: cross-bilateral spatial filter
- [ ] `src/gpu/shaders/rt_denoise_temporal.glsl` — Compute shader: temporal accumulation with motion rejection
- [ ] `src/gpu/shaders/rt_composite.glsl` — Compute shader: Fresnel-weighted blend into color buffer

### RTCompositorBase (C++ base class)
Encapsulates the CompositorEffect boilerplate pattern discovered across all studied projects:

**Initialization** (mirrors community pattern):
```cpp
_init() → RenderingServer::call_on_render_thread(_initialize_render)
_initialize_render():
  rd_ = RenderingServer::get_rendering_device()
  nearest_sampler_ = create_sampler(SAMPLER_FILTER_NEAREST)
  linear_sampler_  = create_sampler(SAMPLER_FILTER_LINEAR)
```

**Key helpers** (from JFA + lens effects base classes):
- `compile_shader(path)` — Load RDShaderFile → get_spirv → shader_create_from_spirv → compute_pipeline_create
- `ensure_texture(context, name, format, size)` — Create/cache intermediate textures via `render_scene_buffers.create_texture()`
- `dispatch_compute(pipeline, uniform_sets, groups, push_constants)` — compute_list_begin → bind → dispatch → end, with `rd.draw_command_begin_label()` for RenderDoc debugging
- `get_color_image(view)` — `render_scene_buffers.get_color_layer(view)` (R16G16B16A16_SFLOAT)
- `get_depth_sampler(view)` — `render_scene_buffers.get_depth_layer(view)`
- `get_normal_roughness_sampler(view)` — `render_scene_buffers.get_texture("forward_clustered", "normal_roughness")`
- `get_scene_data_ubo()` — `render_scene_data.get_uniform_buffer()` (camera matrices, projection)
- Uniform set caching via `UniformSetCacheRD::get_cache()`

**Virtual interface** (subclasses override):
- `_on_initialize_render()` — Compile shaders, create pipelines
- `_on_render_setup(render_size)` — Recreate textures on resize
- `_on_render_view(view, color, depth, normal_roughness, size)` — Per-view dispatch

### How It Works (4-pass pipeline)
1. **Pass 1 — Ray generation + BVH trace** (`rt_reflections.glsl`):
   - Read depth buffer → reconstruct world-space position via inverse projection/view
   - Read normal_roughness → decode octahedral normal, extract roughness
   - Skip pixels with roughness > threshold (configurable, default 0.3)
   - Compute `reflect(-view_dir, world_normal)` as ray direction
   - Trace against our BVH (shared GPU memory with `GPURayCaster`)
   - Write: hit color (from material albedo), hit distance, hit normal → intermediate texture
   - Uses 16x16 workgroups (proven optimal for screen-space effects)

2. **Pass 2 — Spatial denoise** (`rt_denoise_spatial.glsl`):
   - Cross-bilateral filter guided by depth + normal (preserves edges)
   - 5x5 kernel with depth/normal-aware weights
   - Reduces single-sample noise while keeping contact reflections sharp

3. **Pass 3 — Temporal accumulation** (`rt_denoise_temporal.glsl`):
   - Exponential moving average with previous frame's result
   - Motion rejection: compare reprojected position against current depth (discard stale samples)
   - Configurable blend factor (default 0.1 = 90% history, 10% new)
   - Every studied RT project uses temporal accumulation — proven essential at 1 spp

4. **Pass 4 — Composite** (`rt_composite.glsl`):
   - Fresnel-Schlick weighting: `F = F0 + (1 - F0) * pow(1 - NdotV, 5)`
   - Roughness falloff: smooth blend to zero as roughness approaches threshold
   - Write blended result directly into Godot's color buffer via `imageStore()`

### GLSL Shader Layout
```glsl
#[compute]
#version 450
layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

// Set 0: Godot scene data (consistent binding layout across all RT shaders)
layout(set = 0, binding = 0) uniform SceneData { /* Godot's built-in UBO */ };
layout(set = 0, binding = 1, rgba16f) uniform image2D color_buffer;
layout(set = 0, binding = 2) uniform sampler2D depth_sampler;
layout(set = 0, binding = 3) uniform sampler2D normal_roughness_sampler;

// Set 1: BVH data (shared from GPURayCaster — same RenderingDevice)
layout(set = 1, binding = 0, std430) readonly buffer BVHNodes { ... };
layout(set = 1, binding = 1, std430) readonly buffer Triangles { ... };
layout(set = 1, binding = 2, std430) readonly buffer Materials { ... };

// Set 2: Intermediate textures
layout(set = 2, binding = 0, rgba16f) uniform image2D reflection_result;
layout(set = 2, binding = 1, rgba16f) uniform image2D prev_frame_result;

// Push constants (camera + per-frame params, fits in 128-byte Vulkan minimum)
layout(push_constant, std430) uniform Params {
    mat4 inv_projection;
    mat4 inv_view;
    float roughness_threshold;
    float temporal_blend;
    int frame_count;
    int pad;
};
```

### Normal/Roughness Decoding (from Godot's forward_clustered pipeline)
```glsl
// Godot stores normals in octahedral encoding in the RG channels
vec3 octahedral_decode(vec2 f) {
    vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = clamp(-n.z, 0.0, 1.0);
    n.xy += mix(vec2(t), vec2(-t), greaterThanEqual(n.xy, vec2(0.0)));
    return normalize(n);
}
```

### Key Design Notes
- Only trace reflections for pixels with roughness < 0.3 (configurable threshold)
- 1 ray per pixel (1 spp) — sparse sampling, rely on temporal + spatial denoise
- Fall back to Godot's SSR for rough surfaces (blurry reflections are cheap to approximate)
- Our BVH is already on the GPU via `GPURayCaster` — same `RenderingDevice`, shared memory space
- Push constants for per-frame data (128 bytes), UBOs for larger data blocks
- `UniformSetCacheRD::get_cache()` for efficient per-frame uniform binding (avoid recreation)
- `rd.draw_command_begin_label()` / `end_label()` for RenderDoc profiling (from JFA addon)
- Handle `render_size` changes by clearing texture context (from lens effects)

### Performance Budget (GTX 1650 Ti @ 1080p)
- ~500K reflection rays (roughness threshold culls ~75% of pixels)
- BVH traversal: ~2-3ms for 500K rays (based on current benchmarks)
- Spatial denoise: ~0.5ms (5x5 bilateral filter)
- Temporal accumulation: ~0.2ms (simple blend)
- Composite: ~0.2ms (per-pixel Fresnel blend)
- Total overhead: ~3-4ms target (keeps total frame time under 16ms for 60fps)

### Modified Files
- [ ] `src/register_types.cpp` — Register `RTCompositorBase` and `RTReflectionEffect`
- [ ] `src/modules/graphics/ray_scene_setup.cpp` — Add `rt_reflections_enabled` property, attach effect to Compositor when enabled
- [ ] Build + verify

---

## Phase 6: RT-Enhanced Ambient Occlusion via CompositorEffect

> More accurate AO than SSAO — traces short rays in the hemisphere above each surface point.
> Reuses `RTCompositorBase` infrastructure from Phase 5.

### New Files
- [ ] `src/modules/graphics/rt_ao_effect.h` — Header for the AO CompositorEffect (extends `RTCompositorBase`)
- [ ] `src/modules/graphics/rt_ao_effect.cpp` — Implementation
- [ ] `src/gpu/shaders/rt_ao.glsl` — Compute shader: hemisphere ray generation + BVH occlusion test
- [ ] `src/gpu/shaders/rt_ao_denoise.glsl` — Compute shader: spatial + temporal denoise (can reuse denoise shaders from Phase 5 with specialization constants)

### How It Works (3-pass pipeline)
1. **Pass 1 — AO ray generation + occlusion test** (`rt_ao.glsl`):
   - Read depth → reconstruct world position
   - Read normal_roughness → decode surface normal
   - Generate 1-4 cosine-weighted hemisphere rays per pixel (using noise from interleaved gradient noise — proven in Guertin motion blur shader)
   - Trace against BVH with `t_max` = AO radius (small, ~1-2 world units)
   - `ANY_HIT` mode: early termination on first intersection (fastest BVH traversal type)
   - Write: occlusion factor (0.0 = fully lit, 1.0 = fully occluded) → intermediate texture

2. **Pass 2 — Denoise** (`rt_ao_denoise.glsl`):
   - Reuse spatial bilateral filter from Phase 5 (parameterized via specialization constants for kernel size, edge sensitivity)
   - Temporal accumulation with exponential moving average (same pattern as reflections)
   - Depth-aware + normal-aware edge stopping

3. **Pass 3 — Apply AO**:
   - Multiply Godot's color buffer by `(1.0 - occlusion * intensity * ao_power)`
   - Can use the composite pass from Phase 5 with different blend mode (specialization constant)

### GLSL Hemisphere Sampling (from HK-SHAO's proven PBR implementation)
```glsl
// Cosine-weighted hemisphere sampling with TBN matrix construction
vec3 cosine_hemisphere_sample(vec3 normal, inout uint rng_state) {
    float ra = TAU * random_float(rng_state);
    float rb = random_float(rng_state);
    float rz = sqrt(rb);
    vec2 rxy = sqrt(1.0 - rb) * vec2(cos(ra), sin(ra));
    return construct_TBN(normal) * vec3(rxy, rz);
}
```

### Key Design Notes
- Lower priority than Phase 5 — SSAO is already decent for most cases
- Main advantage: handles off-screen occluders and thin geometry that SSAO misses
- 1-4 spp per pixel: configurable via export property, default 2
- Shares `RTCompositorBase` base class, same uniform binding layout, same denoise infrastructure
- Interleaved gradient noise for sample jitter (from Guertin shader, proven low-discrepancy noise)
- Short AO rays terminate early in BVH → cheaper than reflection rays

### Performance Budget
- Even at 1 spp, AO rays are very short (early termination) → fast BVH traversal
- Target: ~2ms overhead at 1080p (1 spp), ~4ms at 4 spp

### Modified Files
- [ ] `src/register_types.cpp` — Register `RTAOEffect`
- [ ] `src/modules/graphics/ray_scene_setup.cpp` — Add `rt_ao_enabled`, `rt_ao_radius`, `rt_ao_intensity`, `rt_ao_samples` properties
- [ ] Build + verify

---

## Phase 7: RT-Enhanced Shadows (optional, stretch goal)

> Trace shadow rays from surface points toward lights for pixel-perfect soft shadows
> with correct penumbra from area light sizes.

### New Files
- [ ] `src/modules/graphics/rt_shadow_effect.h`
- [ ] `src/modules/graphics/rt_shadow_effect.cpp`
- [ ] `src/gpu/shaders/rt_shadows.glsl`

### How It Works
1. Read depth buffer → reconstruct world position
2. For the primary `DirectionalLight3D`, trace 1 shadow ray per pixel toward the sun (with slight random offset for soft shadows)
3. Any-hit traversal against BVH — very fast, early termination
4. Darken pixels where shadow ray is blocked
5. Temporal denoise for soft shadow stability

### Key Design Notes
- Godot's cascaded shadow maps are already quite good — this phase provides marginal visual improvement
- Main benefit: contact-hardening penumbra and long-range shadows beyond cascade distance
- Uses `ANY_HIT` mode which is the fastest BVH traversal type
- Implement only if performance budget allows after Phase 5-6

### Modified Files
- [ ] `src/register_types.cpp` — Register `RTShadowEffect`
- [ ] `src/modules/graphics/ray_scene_setup.cpp` — Add `rt_shadows_enabled` property
- [ ] Build + verify

---

## File Impact Summary

| File | Phases |
|------|--------|
| `src/core/material_data.h` | Phase 1 (new), Phase 2 (extended) |
| `src/core/triangle_uv.h` | Phase 2 (new) |
| `src/api/scene_shade_data.h` | Phase 1 (new), Phase 2 (extended) |
| `src/api/ray_service.h` | Phase 1 |
| `src/godot/raytracer_server.h` | Phase 1, 2 |
| `src/godot/raytracer_server.cpp` | Phase 1, 2 |
| `src/godot/ray_service_bridge.cpp` | Phase 1 |
| `src/modules/graphics/texture_sampler.h` | Phase 2 (new) |
| `src/modules/graphics/shade_pass.h` | Phase 1, 2 |
| `src/modules/graphics/ray_image.h` | Phase 1 |
| `src/modules/graphics/ray_renderer.h` | Phase 1 |
| `src/modules/graphics/ray_renderer.cpp` | Phase 1 |
| `src/modules/graphics/ray_scene_setup.h` | Phase 3 (new) |
| `src/modules/graphics/ray_scene_setup.cpp` | Phase 3 (new), Phase 4, 5, 6, 7 |
| `src/modules/graphics/rt_reflection_effect.h` | Phase 5 (new) |
| `src/modules/graphics/rt_reflection_effect.cpp` | Phase 5 (new) |
| `src/modules/graphics/rt_ao_effect.h` | Phase 6 (new) |
| `src/modules/graphics/rt_ao_effect.cpp` | Phase 6 (new) |
| `src/modules/graphics/rt_shadow_effect.h` | Phase 7 (new, optional) |
| `src/modules/graphics/rt_shadow_effect.cpp` | Phase 7 (new, optional) |
| `src/gpu/shaders/rt_reflections.glsl` | Phase 5 (new) |
| `src/gpu/shaders/rt_ao.glsl` | Phase 6 (new) |
| `src/gpu/shaders/rt_shadows.glsl` | Phase 7 (new, optional) |
| `src/register_types.cpp` | Phase 3, 5, 6, 7 |

---

## Progress Tracker

- [x] **Phase 1** — MaterialData + extraction + albedo shading + ALBEDO channel
- [x] **Phase 2** — UVs + texture sampling
- [x] **Phase 3** — Godot rendering environment setup (`RaySceneSetup`)
- [x] **Phase 4** — Quality presets + auto-configuration
- [x] **Phase 5** — RT reflections via CompositorEffect ← **highest impact**
- [ ] **Phase 6** — RT ambient occlusion via CompositorEffect
- [ ] **Phase 7** — RT shadows via CompositorEffect (stretch goal)
- [ ] **Delete this file**
