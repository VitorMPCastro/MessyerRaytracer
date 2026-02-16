# Community Project Research — Synthesis

> **Temporary working document.** Captures patterns and lessons learned from
> studying open-source Godot ray tracing and CompositorEffect projects on GitHub.
> Information here feeds directly into our Phase 5-7 CompositorEffect implementation.

## Projects Studied

| Project | Stars | Approach | Key Value |
|---------|-------|----------|-----------|
| **sphynx-owner/JFA_driven_motion_blur_addon** | 338 | GDScript CompositorEffect + GLSL compute | Best production CompositorEffect reference, multi-pass pipeline, published paper |
| **ARez2/compositor-effect-lens-effects** | 21 | GDScript CompositorEffect + GLSL compute | Cleaner base class, scene data UBO, normal/roughness buffer access |
| **HK-SHAO/Godot-RayTracing-Demo** | 65 | GDShader canvas_item (PBR ray tracing) | Full PBR model, SDF-based, temporal accumulation, DOF |
| **gvvim/Godot4-Raytracing** | 18 | C# + GLSL compute (local RenderingDevice) | Mesh extraction pipeline, material system, scene graph traversal |
| **Nordup/godot-path-tracing** | 33 | GDScript + GLSL 460 compute (local RenderingDevice) | Clean GPU compute abstractions, full path tracer |

### Other Notable Finds (not deep-dived)
- **godotengine/tps-demo** (1.3K★): High-quality assets/lighting demo with volumetric fog — reference for Godot renderer settings
- **Reddit photorealistic post** (2.2K votes): Proved photogrammetry + baked lighting + heavy post-processing is the community standard for photorealism in Godot, not real-time RT

---

## Critical Architecture Decision: CompositorEffect vs Local RenderingDevice

Two patterns exist in the community. Our project **must use CompositorEffect** (Pattern A).

### Pattern A: CompositorEffect (JFA, Lens Effects) ✅ OUR APPROACH
```
CompositorEffect._init()
  → RenderingServer.call_on_render_thread(_initialize_compute)
  → rd = RenderingServer.get_rendering_device()  // SHARED device
  
_render_callback()
  → render_scene_buffers = render_data.get_render_scene_buffers()
  → Direct access to color/depth/normal buffers (zero-copy)
  → Dispatch compute → result written directly into Godot's framebuffer
```
**Advantages**: Zero-copy buffer access, no CPU readback, runs in Godot's render thread, can write directly to color buffer.

### Pattern B: Local RenderingDevice (gvvim, Nordup) ❌ NOT FOR US
```
rd = RenderingServer.CreateLocalRenderingDevice()  // SEPARATE device
  → Manual buffer upload (vertices, normals, materials)
  → rd.Submit() → rd.Sync() → rd.TextureGetData()  // CPU READBACK
  → Display via Sprite2D/ImageTexture
```
**Disadvantages**: CPU readback bottleneck, can't access Godot's G-buffer, separate GPU queues, no integration with Godot's render pipeline.

---

## CompositorEffect API Reference (from JFA + Lens Effects)

### Initialization Pattern
```
_init():
  1. Set effect_callback_type (EFFECT_CALLBACK_TYPE_POST_TRANSPARENT)
  2. RenderingServer.call_on_render_thread(_initialize_compute)

_initialize_compute():
  1. rd = RenderingServer.get_rendering_device()
  2. Create samplers (nearest + linear)
  3. Load GLSL → get_spirv() → shader_create_from_spirv() → compute_pipeline_create()
```

### Render Callback Pattern
```
_render_callback(effect_callback_type, render_data):
  1. render_scene_buffers = render_data.get_render_scene_buffers()  [RenderSceneBuffersRD]
  2. render_scene_data = render_data.get_render_scene_data()        [RenderSceneDataRD]
  3. render_size = render_scene_buffers.get_internal_size()
  4. Calculate workgroups: ceil((size - 1) / workgroup_size + 1)
  5. For each view (VR support):
     - Get color: render_scene_buffers.get_color_layer(view)
     - Get depth: render_scene_buffers.get_depth_layer(view)
     - Get normal: render_scene_buffers.get_texture("forward_clustered", "normal_roughness")
     - Dispatch compute shader
```

### G-Buffer Access (CRITICAL for our RT effects)

| Buffer | How to Access | Format |
|--------|--------------|--------|
| Color | `render_scene_buffers.get_color_layer(view)` | R16G16B16A16_SFLOAT |
| Depth | `render_scene_buffers.get_depth_layer(view)` | R32_SFLOAT (0=near, 1=far) |
| Normal + Roughness | `render_scene_buffers.get_texture("forward_clustered", "normal_roughness")` | RGBA8 (oct-encoded normal in RG, roughness in B) |
| Scene Data UBO | `render_scene_data.get_uniform_buffer()` | Contains camera matrices, projection, etc. |

### Texture Management
```
render_scene_buffers.create_texture(context, name, format, usage_bits, samples, size, layers, mipmaps, unique)
render_scene_buffers.get_texture_slice(context, name, layer, mipmap, layers, mipmaps)
render_scene_buffers.has_texture(context, name)
render_scene_buffers.clear_context(context)  // on resize
```

### Compute Dispatch Pattern
```
compute_list = rd.compute_list_begin()
rd.compute_list_bind_compute_pipeline(compute_list, pipeline)
rd.compute_list_bind_uniform_set(compute_list, uniform_set, set_index)
rd.compute_list_set_push_constant(compute_list, bytes, size)
rd.compute_list_dispatch(compute_list, groups_x, groups_y, 1)
rd.compute_list_end()
```

### Uniform Set Caching
```
// Avoids recreating uniform sets every frame — use this!
uniform_set = UniformSetCacheRD.get_cache(shader, set_index, uniforms_array)
```

---

## PBR Ray Tracing Patterns (from HK-SHAO + path tracers)

### Fresnel-Schlick
```glsl
float fresnel_schlick(float NoI, float F0) {
    return mix(pow(abs(1.0 + NoI), 5.0), 1.0, F0);
}
```

### Hemisphere Sampling (cosine-weighted)
```glsl
vec3 hemispheric_sampling(vec3 n, inout random rand) {
    float ra = TAU * noise(rand);
    float rb = noise(rand);
    float rz = sqrt(rb);
    vec2 rxy = sqrt(1.0 - rb) * vec2(cos(ra), sin(ra));
    return TBN(n) * vec3(rxy, rz);
}
```

### Roughness-weighted sampling (GGX-like)
```glsl
vec3 hemispheric_sampling_roughness(vec3 n, float roughness, inout random rand) {
    float shiny = pow5(roughness);
    float rz = sqrt((1.0 - rb) / (1.0 + (shiny - 1.0) * rb));
    // ... same TBN construction
}
```

### Russian Roulette (path termination)
```glsl
float inv_pdf = exp(float(i) * light_quality);
float roulette_prob = 1.0 - (1.0 / inv_pdf);
if (noise(rand) < roulette_prob) { break; }
```

### Temporal Accumulation (every project uses this)
```glsl
// Running average across frames
vec4 prev = imageLoad(rendered_image, uvi);
vec4 accumulated = prev * (frame - 1) / frame + new_sample / frame;
imageStore(rendered_image, uvi, accumulated);
```

---

## Material / Scene Data Patterns (from gvvim)

### GPU Material Struct (gvvim's approach)
```csharp
struct MaterialData {  // 52 bytes
    float albedoR, albedoG, albedoB;
    int textureID;
    float specularity;
    float emissiveR, emissiveG, emissiveB;
    int emissiveTextureID;
    float roughness;
    int roughnessTextureID;
    float alpha;
    int alphaTextureID;
}
```
gvvim uploads ALL geometry (vertices, normals, UVs, indices) + materials as storage buffers.
We don't need this for reflections/AO — we only need our BVH + basic material info.

### Surface Descriptor Pattern
```csharp
struct SurfaceDescriptor {  // 72 bytes
    vec3 position, rotation, scale;
    vec3 boxMin, boxMax;  // AABB
    int materialID;
    int indexStart, indexEnd;
}
```

---

## Performance Insights

### Workgroup Sizes
- **Screen-space effects** (motion blur, lens flares, reflections): 16x16 workgroups
- **Path tracing** (heavier per-pixel): 8x8 workgroups
- Our reflections/AO: Start with **16x16** since BVH traversal is bounded (not full path)

### Temporal Strategies
- HK-SHAO: Progressive refinement with frame count → converges to clean image when camera stops
- gvvim: Configurable `recentFrameBias` for exponential moving average
- JFA motion blur: No temporal (per-frame effect by nature)
- **Our approach**: Exponential moving average with motion rejection for reflections/AO

### Memory/Buffer Management
- JFA: Uses `RenderSceneBuffersRD.create_texture()` for intermediate textures (auto-managed lifetime)
- Lens effects: Same, plus `clear_context()` on resize
- gvvim: Manual buffer creation with `rd.StorageBufferCreate()` (not recommended for CompositorEffect)
- **Our approach**: Use `create_texture()` for intermediate RT results, `get_color_layer()`/`get_depth_layer()` for G-buffer reads

---

## Actionable Lessons for Our Implementation

### 1. C++ CompositorEffect Base Class
Create a C++ equivalent of the GDScript base classes from JFA and lens effects:
```cpp
class RTCompositorEffectBase : public CompositorEffect {
    GDCLASS(RTCompositorEffectBase, CompositorEffect);
protected:
    RenderingDevice* rd_ = nullptr;
    RID nearest_sampler_;
    RID linear_sampler_;
    
    // Virtual interface
    virtual void _initialize_render();
    virtual void _render_view(int view, RID color, RID depth, RID normal_roughness, Vector2i size);
    
    // Helpers
    RID compile_shader(const String& glsl_path);
    void dispatch_compute(RID pipeline, const TypedArray<RDUniform>& uniforms, Vector3i groups, const PackedByteArray& push_constants);
    RID ensure_texture(const StringName& context, const StringName& name, RenderingDevice::DataFormat format, Vector2i size);
};
```

### 2. GLSL Shader Structure
```glsl
#[compute]
#version 450
layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

// Set 0: Scene data
layout(set = 0, binding = 0) uniform SceneData { ... };                    // Godot's UBO
layout(set = 0, binding = 1, rgba16f) uniform image2D color_buffer;        // Read/write
layout(set = 0, binding = 2) uniform sampler2D depth_sampler;              // Read
layout(set = 0, binding = 3) uniform sampler2D normal_roughness_sampler;   // Read

// Set 1: Our BVH data (already on GPU from GPURayCaster)
layout(set = 1, binding = 0, std430) readonly buffer BVHNodes { ... };
layout(set = 1, binding = 1, std430) readonly buffer Triangles { ... };

// Push constants
layout(push_constant, std430) uniform Params {
    mat4 inv_projection;
    mat4 inv_view;
    float roughness_threshold;
    int frame_count;
};
```

### 3. Multi-Pass Pipeline (if needed)
For reflections with denoise:
1. **Ray generation + trace**: Read G-buffer → generate reflection rays → trace BVH → write hit color to intermediate texture
2. **Spatial denoise**: Cross-bilateral filter using depth/normal to guide edge preservation
3. **Temporal accumulate**: Blend with previous frame's result (with motion rejection)
4. **Composite**: Blend denoised reflection with color buffer using Fresnel weights

### 4. Normal/Roughness Decoding
The `"forward_clustered"/"normal_roughness"` texture uses octahedral encoding:
```glsl
vec3 decode_normal(vec2 encoded) {
    vec3 n;
    n.z = 1.0 - abs(encoded.x) - abs(encoded.y);
    n.xy = n.z >= 0.0 ? encoded.xy : (1.0 - abs(encoded.yx)) * sign(encoded.xy);
    return normalize(n);
}
```

### 5. Key Godot Classes to Bind in C++ (godot-cpp)
```cpp
#include <godot_cpp/classes/compositor_effect.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rd_shader_file.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/rd_pipeline_specialization_constant.hpp>
#include <godot_cpp/classes/render_scene_buffers_rd.hpp>
#include <godot_cpp/classes/render_scene_data_rd.hpp>
#include <godot_cpp/classes/uniform_set_cache_rd.hpp>
```

---

## Risk Assessment

| Risk | Mitigation |
|------|------------|
| `get_texture("forward_clustered", "normal_roughness")` may change between Godot versions | Pin to Godot 4.4+, wrap in compatibility check |
| BVH data lives on GPU via `GPURayCaster`, needs to be accessible from CompositorEffect's shader | Both use `RenderingServer.get_rendering_device()` — same device, shared memory space |
| Push constant size limit (128 bytes typical Vulkan minimum) | Use UBO for larger data, push constants only for per-frame camera data |
| 1 spp reflections will be noisy | Temporal accumulation + spatial denoise (bilateral) — proven pattern across all studied projects |
| GTX 1650 Ti may not handle 1080p reflections at 60fps | Roughness threshold culling, half-res trace option, checkerboard rendering |
