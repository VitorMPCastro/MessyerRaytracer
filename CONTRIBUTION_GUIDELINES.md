# Contribution Guidelines

> For human contributors and AI assistants alike.
> Every convention below is already established in the codebase — follow what exists.

---

## Table of Contents

1. [Godot-Native Principle](#godot-native-principle)
2. [Performance Principle](#performance-principle)
3. [Tiger Style Assertions](#tiger-style-assertions)
4. [File Header Convention](#file-header-convention)
5. [Comment Philosophy](#comment-philosophy)
6. [Naming Conventions](#naming-conventions)
7. [GPU Struct Rules](#gpu-struct-rules)
8. [Module Decoupling](#module-decoupling)
9. [Code Organization](#code-organization)
10. [TinyBVH Third-Party Safety](#tinybvh-third-party-safety)
11. [Correctness Over Cleverness](#correctness-over-cleverness)
12. [Document Invariants](#document-invariants)
13. [Error Handling](#error-handling)
14. [Design Documentation](#design-documentation)
15. [Living Documentation](#living-documentation)
16. [Demo Scene Convention](#demo-scene-convention)
17. [Build & Test](#build--test)

---

## Godot-Native Principle

> **Read from the scene tree. Never maintain parallel state.**

This is the single most important architectural rule in the project. We are a GDExtension — not a standalone engine. Godot already provides scene nodes for lights, cameras, environments, materials, and post-processing. **Our job is to read from them, not to reimplement or shadow them.**

When a user places a `DirectionalLight3D` in their scene, our raytracer must read its direction, color, and energy. When they configure a `WorldEnvironment` with sky colors, tone mapping, and ambient light, we must respect those values. If we maintain our own `sun_direction_` property that is disconnected from the scene, we are forcing the user to configure the same thing twice — that is a bug, not a feature.

### The Rules

1. **Scene nodes are the source of truth.** If Godot has a node or resource for it (`DirectionalLight3D`, `WorldEnvironment`, `Camera3D`, `BaseMaterial3D`), read from it. Do not create a parallel property on our nodes.

2. **NodePath references for explicit binding.** When our node needs data from another node, expose a `NodePath` property (e.g., `light_path`, `environment_path`). Follow the `camera_path` pattern already established in `RayRenderer`.

3. **Auto-discovery as fallback.** When no explicit NodePath is set, walk the scene tree for the relevant node type (e.g., find the first `DirectionalLight3D`, find the `WorldEnvironment`). Document the discovery order.

4. **Fallback defaults only as last resort.** If no scene node is found, use sensible defaults — but log a warning via `WARN_PRINT` so the user knows the binding is missing.

5. **Never hardcode values that exist in the scene.** Sun direction, sun color, sky gradient, ambient energy, tone mapping mode, camera near/far — all of these exist on Godot nodes. Hardcoded `constexpr` values for these are a violation of this rule.

6. **Per-frame reads for dynamic data.** Light transforms, environment parameters, and camera state change at runtime. Read them every frame in `render_frame()`, not once at `_ready()`.

7. **Cache invalidation is the user's problem.** If the user changes a material or moves a mesh, they call `build()`. We don't poll for changes. But *reading* current transforms and light parameters each frame is free and expected.

### Why This Matters

- **Professional GDExtensions sit flush with the engine.** Users set up their scene with standard Godot nodes and our extension "just sees" them. No manual synchronization. No duplicate knobs. No drift between what Godot's rasterizer shows and what our raytracer computes.
- **It eliminates entire categories of bugs.** If sun direction comes from `DirectionalLight3D.global_transform`, it's impossible for the shadow direction to disagree with the visible light gizmo.
- **It reduces API surface.** Fewer properties to expose, document, and maintain. The user already knows how to use `DirectionalLight3D` — they don't need to learn our custom `sun_direction` property.
- **It makes AI-generated code more reliable.** When an AI assistant follows "read from Godot" instead of "invent a value," it cannot hallucinate physically incorrect defaults.

### What NOT to Read from Godot

Some things are genuinely ours:

- **BVH parameters** (leaf size, SAH bin count) — Godot has no equivalent.
- **Debug channel selection** — our AOV system is unique to our raytracer.
- **Backend selection** (CPU vs GPU) — our dispatch concern.
- **Ray query API** (`RayQuery`, `RayQueryResult`) — gameplay-facing, not scene data.
- **Performance tuning** (thread count, tile size, max iterations) — engine internals.

### Checklist

Before writing any code that uses a magic number or creates a new property:

- [ ] Does this value already exist on a Godot node in the scene tree?
- [ ] If yes → add a NodePath to that node and read the value per frame
- [ ] If no → is there a Godot resource or setting for it? (e.g., `ProjectSettings`, `Environment`)
- [ ] If still no → only then create a custom property with a documented default

### Examples

```cpp
// ❌ BAD — parallel state, disconnected from scene
Vector3 sun_direction_ = Vector3(0.5f, 0.8f, 0.3f).normalized();
Vector3 sun_color_     = Vector3(2.5f, 2.4f, 2.2f);

// ✅ GOOD — read from scene tree every frame
DirectionalLight3D *light = Object::cast_to<DirectionalLight3D>(get_node_or_null(light_path_));
if (light) {
    sun_direction = -light->get_global_transform().basis.get_column(2);
    sun_color     = Vector3(light->get_color().r, light->get_color().g, light->get_color().b)
                    * light->get_param(Light3D::PARAM_ENERGY);
}
```

```cpp
// ❌ BAD — hardcoded tone mapping, ignores user's Environment
out_r = out_r / (out_r + 1.0f);  // Reinhard, always

// ✅ GOOD — match what the user configured
switch (tonemap_mode) {
    case Environment::TONE_MAPPER_LINEAR:  break;  // no-op
    case Environment::TONE_MAPPER_REINHARD: out = out / (out + 1.0f); break;
    case Environment::TONE_MAPPER_FILMIC:   out = filmic(out); break;
    case Environment::TONE_MAPPER_ACES:     out = aces(out); break;
}
```

```cpp
// ❌ BAD — depth range is a manual knob
float depth_range_ = 100.0f;

// ✅ GOOD — derive from Camera3D
float depth_range = camera->get_far() - camera->get_near();
```

### Node Resolution Pattern

When resolving a scene node, follow this three-tier pattern (established in `RayRenderer::_resolve_light()` and `_resolve_environment()`):

1. **Explicit NodePath** (user set it in the inspector) — fastest, most deterministic.
2. **Auto-discover** via `root->find_children("*", "ClassName", true, false)` — convenient fallback.
3. **`WARN_PRINT_ONCE` + return nullptr** — alerts user that something is missing.

```cpp
// ✅ GOOD — three-tier resolve pattern (from ray_renderer.cpp)
DirectionalLight3D *RayRenderer::_resolve_light() const {
    // 1. Explicit NodePath binding (preferred).
    if (!light_path_.is_empty()) {
        Node *node = get_node_or_null(light_path_);
        auto *light = Object::cast_to<DirectionalLight3D>(node);
        if (light) { return light; }
        WARN_PRINT_ONCE("light_path does not point to a DirectionalLight3D — auto-discovering.");
    }
    // 2. Auto-discover from scene root.
    Node *root = get_tree()->get_current_scene();
    if (root) {
        TypedArray<Node> lights = root->find_children("*", "DirectionalLight3D", true, false);
        if (lights.size() > 0) {
            return Object::cast_to<DirectionalLight3D>(Object::cast_to<Node>(lights[0]));
        }
    }
    // 3. Fallback — log and return null.
    WARN_PRINT_ONCE("No DirectionalLight3D found — using fallback.");
    return nullptr;
}
```

### Scene Data Structs

When passing scene-sourced data into pure functions (e.g., shading), batch it into a plain struct so the function stays Godot-agnostic. The struct is populated once per frame in the Godot-facing code and passed by `const &`.

```cpp
// ✅ GOOD — EnvironmentData carries per-frame scene reads (from shade_pass.h)
struct EnvironmentData {
    float sky_zenith_r, sky_zenith_g, sky_zenith_b;
    float sky_horizon_r, sky_horizon_g, sky_horizon_b;
    float sky_ground_r, sky_ground_g, sky_ground_b;
    float ambient_energy;
    float ambient_r, ambient_g, ambient_b;
    int tonemap_mode;  // 0=Linear, 1=Reinhard, 2=Filmic, 3=ACES, 4=AgX
};
// Populated from WorldEnvironment → Environment → ProceduralSkyMaterial in render_frame().
// shade_pass.h never includes Godot headers — it receives EnvironmentData by const ref.
```

---

## Performance Principle

> **Always choose the most performant path. This is a real-time ray tracer.**

Every function we write runs millions of times per frame. Performance is not an afterthought — it is a design constraint equal to correctness. When two approaches are equally correct, always choose the faster one. When in doubt, measure.

### The Rules

1. **Hot paths are sacred.** Ray-triangle intersection, BVH traversal, and shading run per-pixel per-frame. Every instruction counts. No allocations, no virtual calls, no branches that can be avoided.

2. **Data layout drives performance.** Prefer SoA (Structure of Arrays) over AoS when traversing large datasets. Pack GPU structs to minimize cache misses. Align to cache lines (64 bytes) when it matters.

3. **Batch, don't scatter.** Process rays in tiles/batches for cache coherence. Dispatch work through `ThreadPool::dispatch_and_wait()` with tile sizes tuned to L1/L2 cache (256–1024 rays per tile).

4. **Avoid allocation in the render loop.** Pre-allocate buffers (`rays_`, `hits_`, `shadow_rays_`, `shadow_mask_`) and resize only when resolution changes. `std::vector::resize()` in a per-frame function is a red flag.

5. **Prefer branchless code in inner loops.** Use conditional moves, `min`/`max`, sign tricks, and bitwise operations over `if/else` in hot paths. Branches cause pipeline stalls on modern CPUs.

6. **SIMD where it matters.** SSE/AVX ray-AABB and ray-triangle tests exist for a reason. Use the `Ray4` packet path for coherent primary rays. Don't SIMD-ify code that isn't a bottleneck.

7. **Measure before optimizing.** Use `std::chrono` timing (already in `render_frame()`) or platform profilers. Document measured improvements in comments with numbers:
   ```cpp
   // Branchless slab test: 1.8x faster than branching version (measured on i7-10750H,
   // 1920×1080, Sponza scene, 2.1M tris: 14.2ms → 7.9ms per frame).
   ```

8. **Precompute what you can.** Inverse ray direction, reciprocals, and sign bits are computed once per ray in the constructor — not per intersection test.

9. **Minimize GPU↔CPU transfers.** When using the GPU backend, pack results tightly. Document bandwidth savings:
   ```cpp
   // Position NOT stored — reconstructed from ray origin + direction * t.
   // Saves 12 bytes/result = 18.7MB/frame at 1280×960 (33% bandwidth reduction).
   ```

10. **Const-ref for structs, value for scalars.** Pass `const Ray &`, `const Intersection &`, `const EnvironmentData &` by reference. Pass `float`, `int`, `uint32_t` by value.

### What IS Acceptable Overhead

- **Scene node reads** (once per frame): `get_global_transform()`, `get_color()`, `get_param()` — microsecond-level, called once, not per-pixel.
- **BVH build** (on explicit `build()` call): O(n log n) is fine — it's user-triggered, not per-frame.
- **Assertions in debug builds**: `RT_ASSERT` compiles away in release. `RT_VERIFY` stays but should be cheap.

### Anti-Patterns

```cpp
// ❌ BAD — allocation in hot path
for (int i = 0; i < ray_count; i++) {
    std::vector<float> temp(3);  // malloc per ray!
    ...
}

// ✅ GOOD — pre-allocated, reused buffer
std::vector<float> temp(ray_count * 3);
for (int i = 0; i < ray_count; i++) {
    // use temp[i*3 .. i*3+2]
}
```

```cpp
// ❌ BAD — virtual call in inner loop
for (int i = 0; i < ray_count; i++) {
    result[i] = service->trace(rays[i]);  // vtable lookup per ray
}

// ✅ GOOD — batch submission, single virtual call
service->submit(query, result);  // one call, millions of rays
```

---

## Tiger Style Assertions

We follow [Tiger Style](https://github.com/tigerbeetle/tigerbeetle/blob/main/docs/TIGER_STYLE.md) — specifically, **programming the negative space**. Assert what should NOT happen. A failed assertion caught early is always better than silent data corruption.

### The Three Tiers

| Macro | When Active | Use For |
|-------|-------------|---------|
| `RT_ASSERT(cond, msg)` | Debug only (`NDEBUG` strips it) | Development-time assumptions |
| `RT_VERIFY(cond, msg)` | **Always**, including release | Invariants whose violation means data corruption |
| `RT_SLOW_ASSERT(cond, msg)` | Only when `RT_SLOW_CHECKS` is defined | Expensive validations (full BVH integrity, etc.) |

### Convenience Macros

| Macro | What It Checks |
|-------|----------------|
| `RT_ASSERT_VALID_RAY(ray)` | Ray fields are finite, `t_min <= t_max` |
| `RT_ASSERT_FINITE(value)` | No NaN or Inf |
| `RT_ASSERT_NOT_NULL(ptr)` | Pointer is not null |
| `RT_ASSERT_BOUNDS(idx, size)` | `0 <= idx < size` (signed) |
| `RT_ASSERT_BOUNDS_U(idx, size)` | `idx < size` (unsigned) |
| `RT_ASSERT_POSITIVE(val)` | `val > 0` |
| `RT_ASSERT_NORMALIZED(vec, eps)` | Vector is approximately unit length |
| `RT_UNREACHABLE(msg)` | Code path that should never execute (always active) |

### Rules

1. **At least 2 runtime assertions per non-trivial function** (NASA Power of 10, Rule #5).
2. Validate inputs at function entry — origin, direction, indices, pointers.
3. Validate outputs before returning — "did I compute something sane?"
4. Assert loop bounds before entering loops.
5. Use `RT_VERIFY` for anything that would cause silent corruption if violated.
6. Use `RT_UNREACHABLE` for impossible switch cases and default branches.
7. Assertions are documentation — they tell the next reader what's true at that point.

### Example

```cpp
Vector3 Ray::at(float t) const {
    RT_ASSERT_FINITE(t);               // input validation
    return origin + direction * t;
}

Ray::Ray(const Vector3 &o, const Vector3 &d, float t0, float t1)
    : origin(o), direction(d), t_min(t0), t_max(t1), flags(0) {
    RT_ASSERT(d.is_finite(), "Ray direction must be finite");
    RT_ASSERT(t0 >= 0.0f, "Ray t_min must be non-negative");
    RT_ASSERT(t0 <= t1, "Ray t_min must be <= t_max");
    _precompute();
}
```

---

## File Header Convention

Every `.h` and `.cpp` file starts with this pattern:

```cpp
#pragma once
// filename.h — One-line summary of what this file IS.
//
// Longer block comment explaining:
//   WHAT:  What this component does (1-2 sentences)
//   WHY:   Why it exists / what problem it solves
//   HOW:   Key algorithms or design decisions (with numbers)
//   USAGE: Code example showing typical use (for API-facing headers)
```

### Rules

- `#pragma once` on the very first line (no include guards).
- Second line: `// filename.h — ` followed by a single sentence.
- Block comment starts on line 3 with `//`.
- Use indented lists under labeled sections (`WHAT:`, `WHY:`, `HOW:`, `USAGE:`).
- GDScript examples in `USAGE:` for Godot-exposed classes.
- Cite paper names and star counts for algorithms from research ("Aila & Laine, HPG 2009").

### Example

```cpp
#pragma once
// thread_pool.h — Lightweight thread pool for parallel ray batch processing.
//
// Spawns N worker threads (typically CPU core count - 1) at construction.
// Work is submitted as batches that are split across workers automatically.
//
// DESIGN CHOICES:
//   - Fixed thread count (no dynamic scaling) — simpler, no allocation during dispatch
//   - Uses std::thread + std::mutex + std::condition_variable — no external deps
//   - Batch-oriented: submit a range [0, count) and a lambda(start, end)
//
// WHY NOT std::async or OpenMP?
//   std::async creates/destroys threads per call — too much overhead for per-frame ray batches.
//   OpenMP requires compiler flags and isn't always available with MSVC + SCons.
```

---

## Comment Philosophy

### Document WHY, Not WHAT

```cpp
// BAD — restates the code:
// Set t to FLT_MAX
t = FLT_MAX;

// GOOD — explains the WHY:
// FLT_MAX means "no hit" — any real intersection will have t < FLT_MAX.
t = FLT_MAX;
```

### Include Numbers and Rationale

```cpp
// At 1280×960 that's 18.7MB saved per frame in GPU→CPU transfer.
// 12 SAH bins — more doesn't improve quality measurably (tested 8, 12, 16, 32).
// 128 threads per workgroup — occupancy sweet spot for Turing GPUs.
```

### Inline Comments for Non-Obvious Lines

Use sparingly, only when the line is surprising or performance-critical:

```cpp
auto safe_inv = [EPS](float d) -> float {
    return (std::fabs(d) < EPS)
        ? ((d < 0.0f) ? (-1.0f / EPS) : (1.0f / EPS))  // huge but finite
        : (1.0f / d);
};
```

---

## Naming Conventions

| Element | Convention | Example |
|---------|-----------|---------|
| Files | `snake_case.h`, `snake_case.cpp` | `gpu_structs.h`, `ray_service.h` |
| Classes / Structs | `PascalCase` | `ThreadPool`, `GPURayCaster`, `BVHNode` |
| Functions / Methods | `snake_case` | `dispatch_and_wait()`, `set_miss()` |
| Member variables | `snake_case_` (trailing underscore) | `thread_count_`, `node_count_` |
| Local variables | `snake_case` | `hit_point`, `ray_count` |
| Constants / Sentinels | `UPPER_SNAKE_CASE` | `NO_HIT`, `MAX_LEAF_SIZE` |
| Enum values | `UPPER_SNAKE_CASE` | `PRESET_LOW`, `GI_SDFGI` |
| Enum types | `PascalCase` | `QualityPreset`, `GIMode` |
| Macros | `RT_` prefix + `UPPER_SNAKE_CASE` | `RT_ASSERT`, `RT_VERIFY` |
| Godot-bound methods | `snake_case` with `_` prefix for virtuals | `_ready()`, `_process()` |
| GPU structs | `GPU` prefix + `PascalCase` + `Packed` suffix | `GPUTrianglePacked` |

### Prefixes

- `RT_` for all raytracer macros (avoids collision with Godot/system macros).
- `GPU` for GPU-side data structures.
- `I` for abstract interfaces (`IRayService`).
- `_` prefix for Godot virtual overrides (`_ready`, `_process`, `_bind_methods`).

---

## GPU Struct Rules

GPU structs live in `src/gpu/gpu_structs.h` and must match GLSL `std430` layout exactly. Mismatches cause **silent data corruption** — the GPU reads garbage with no error.

### Rules

1. **Every GPU struct has a `static_assert` on `sizeof`.**
   ```cpp
   static_assert(sizeof(GPURayPacked) == 32, "GPURayPacked must be 32 bytes (std430)");
   ```

2. **Document the GLSL mirror layout** in a comment block above the struct:
   ```cpp
   // GLSL layout:
   //   struct GPURay {
   //       vec3 origin;    float t_max;   // offset  0–15
   //       vec3 direction; float t_min;   // offset 16–31
   //   };
   ```

3. **Use explicit padding** — `vec3` in std430 has 16-byte alignment. Pack a scalar after each `vec3`:
   ```cpp
   float origin[3];    float t_max;    // vec3 + float = 16 bytes
   float direction[3]; float t_min;    // vec3 + float = 16 bytes
   ```

4. **Use `float[3]` not `Vector3`** — `Vector3` may have different alignment/padding.

5. **Name padding fields** `_padN` and document why they exist.

6. **Document byte offsets** for every field group.

7. **If you change a C++ struct, update the GLSL mirror, and vice versa.** Always.

---

## Module Decoupling

### The IRayService Boundary

Modules (graphics, audio, AI) never include server internals. They include only:

- `api/ray_service.h` — the abstract `IRayService` interface
- `api/ray_query.h` — query/result types
- `api/path_tracer.h` — the abstract `IPathTracer` interface
- `api/gpu_types.h` — GPU-compatible packed structs (`GPUTrianglePacked`, `GPUBVHNodePacked`, `GPUSceneUpload`)
- `api/thread_dispatch.h` — the abstract `IThreadDispatch` interface
- `api/light_data.h` — `LightData` + `SceneLightData`
- `api/scene_shade_data.h` — shade data view
- `core/*.h` — fundamental types (Ray, Intersection, Triangle)

```
✅  #include "api/ray_service.h"
✅  #include "api/path_tracer.h"
✅  #include "api/gpu_types.h"
❌  #include "raytracer_server.h"     // NEVER — breaks decoupling
❌  #include "accel/bvh.h"            // NEVER — internal implementation
❌  #include "accel/ray_scene.h"      // NEVER — use get_gpu_scene_data() instead
❌  #include "dispatch/thread_pool.h"  // NEVER — use get_thread_dispatch() instead
❌  #include "gpu/gpu_structs.h"      // NEVER — use api/gpu_types.h instead
```

### API Layer Conventions

The `api/` layer provides several key services beyond ray submission:

1. **Shared thread pool** — `IRayService::get_thread_dispatch()` returns the server's thread pool.
   Modules must NOT create their own `ThreadPool` — that would double-subscribe CPU cores.
   ```cpp
   IThreadDispatch *pool = svc->get_thread_dispatch();  // ✅ shared
   auto pool = create_thread_dispatch();                 // ❌ wastes cores
   ```

2. **GPU scene data** — `IRayService::get_gpu_scene_data()` returns pre-packed buffers
   for GPU upload via `GPUSceneUpload`. Modules never access `RayScene` or TinyBVH types.
   ```cpp
   GPUSceneUpload upload = svc->get_gpu_scene_data();  // ✅ opaque packed data
   const RayScene &scene = svc->get_scene();            // ❌ leaks accel/ types
   ```

3. **Async GPU dispatch** — `submit_async()` / `collect_nearest()` for overlapping CPU
   work with GPU computation. Also `submit_async_any_hit()` / `collect_any_hit()`.
   ```cpp
   svc->submit_async(rays, count);
   /* ... do CPU work while GPU traces ... */
   svc->collect_nearest(results, count);  // blocks until GPU done
   ```

4. **Path tracing** — `IPathTracer` abstracts the multi-bounce path tracing implementation.
   `RayRenderer` owns an `IPathTracer` instance (currently `CPUPathTracer`).
   A future GPU wavefront path tracer will implement the same interface.
   ```cpp
   path_tracer_->trace_frame(params, primary_rays, color_output, svc, pool);
   ```

### Why?

- Modules see only the abstract interface + data types
- No BVH, RayDispatcher, GPURayCaster, or SceneTLAS in the include chain
- Easy to mock for unit testing
- Compile-time firewall — module changes don't trigger full rebuilds

---

## Code Organization

### Header-Only vs. Header + Source

| Use header-only when... | Use .h + .cpp when... |
|------------------------|----------------------|
| Small POD structs (Ray, Intersection, Triangle) | Classes with complex logic (BVH, ThreadPool) |
| Inline functions with 1-5 lines | Anything touching RenderingDevice |
| Template code | Godot-bound classes (GDCLASS) |

### Resource-Owning Classes

Classes that own resources (threads, GPU buffers, RIDs) must be **non-copyable**:

```cpp
ThreadPool(const ThreadPool &) = delete;
ThreadPool &operator=(const ThreadPool &) = delete;
```

### POD Structs for Core Types

Core data types (`Ray`, `Intersection`, `Triangle`, `BVHNode`) are plain structs:

- No virtual functions
- No inheritance
- Constructors for initialization + assertions only
- Sentinel values as `static constexpr` members (`Intersection::NO_HIT = UINT32_MAX`)

### Sentinel Values

```cpp
// CPU side — compile-time constant
static constexpr uint32_t NO_HIT = UINT32_MAX;

// GPU side — document the equivalent
// prim_id == -1 means "no hit" (the GPU equivalent of Intersection::NO_HIT).
```

Always document the CPU↔GPU sentinel mapping.

---

## TinyBVH Third-Party Safety

> **Never copy or move TinyBVH-containing types. Use `std::unique_ptr` in containers.**

TinyBVH is a single-header BVH library (`thirdparty/tinybvh/tiny_bvh.h`). Its classes — `BVH`, `BVH4_CPU`, `BVH8_CPU`, `BVH8_CWBVH`, `MBVH<M>` — have **user-defined destructors** that call `AlignedFree` on internal node/triangle arrays. However, they have **no custom copy constructor, move constructor, copy-assignment, or move-assignment operators**.

By the **C++ Rule of Five**, when a class defines a destructor, the compiler suppresses implicit move operations. This means:
- `std::vector<MeshBLAS>` reallocation falls back to the implicit **copy constructor** (shallow pointer copy)
- The old elements' destructors free the node arrays
- The new copies hold **dangling pointers** → heap corruption on next access

This is the root cause of the `0xC0000374` (STATUS_HEAP_CORRUPTION) crash we diagnosed and fixed.

### The Rules

1. **Any struct containing TinyBVH types must be non-copyable AND non-movable:**
   ```cpp
   struct MeshBLAS {
       MeshBLAS() = default;
       MeshBLAS(const MeshBLAS &) = delete;
       MeshBLAS &operator=(const MeshBLAS &) = delete;
       MeshBLAS(MeshBLAS &&) = delete;
       MeshBLAS &operator=(MeshBLAS &&) = delete;
       tinybvh::BVH bvh2;
       // ...
   };
   ```

2. **Store such structs via `std::unique_ptr` in vectors** — reallocation moves only the pointer, never the object:
   ```cpp
   std::vector<std::unique_ptr<MeshBLAS>> blas_meshes_;  // ✅ safe
   blas_meshes_.push_back(std::make_unique<MeshBLAS>());  // object never moves
   ```

3. **Never use `std::vector<T>` where T contains TinyBVH** — even with `reserve()`. Reserve is a runtime band-aid, not a compile-time guarantee:
   ```cpp
   std::vector<MeshBLAS> meshes;  // ❌ reallocation shallow-copies, double-free
   meshes.reserve(n);              // ❌ still not safe if you forget or miscalculate
   ```

4. **Never assign TinyBVH objects** — `bvh = BVH{}` does a shallow copy that overwrites the valid pointers with nulls, leaking the old allocation:
   ```cpp
   bvh2 = tinybvh::BVH{};    // ❌ memory leak (and potential double-free)
   ```

5. **TinyBVH `Build()`/`PrepareBuild()` manage memory internally** — they check `allocatedNodes`, free if needed, and reallocate. It is safe to rebuild in-place without clearing.

### Affected Types in This Project

| Type | Contains TinyBVH | Protection |
|------|------------------|------------|
| `MeshBLAS` | `BVH`, `BVH4_CPU`, `BVH8_CPU` | Non-copyable, stored as `unique_ptr` in `SceneTLAS` |
| `RayScene` | `BVH`, `BVH4_CPU`, `BVH8_CPU`, `BVH8_CWBVH` | Non-copyable, owned by `RayDispatcher` |
| `SceneTLAS` | `BVH` (tlas_bvh_) | Non-copyable, owned by `RayTracerServer` |

### WHY NOT just `reserve()`?

We previously used `reserve_meshes()` to pre-allocate the vector before building. This technically worked, but:
- It required a two-pass pattern (count → reserve → build) in every caller
- If any caller forgot or miscounted, silent heap corruption resulted
- The bug was invisible until a later allocation exposed it (e.g., CWBVH build)

`std::unique_ptr` provides a **compile-time guarantee** — the `MeshBLAS` objects live on the heap and never move, regardless of vector operations.

---

## Correctness Over Cleverness

> **Always use the textbook-correct pattern. Never rely on timing, platform scheduling, or "it works in practice" arguments.**

This is a multi-threaded real-time system. Subtle incorrectness causes intermittent freezes, data corruption, and bugs that are nearly impossible to reproduce under a debugger. When cppreference or the C++ standard documents a required usage pattern, follow it exactly — even if a shortcut appears to work on your machine.

### The Rules

1. **Condition variables require mutex-guarded predicates.** The predicate variable must be written while holding the same mutex the waiter holds. An atomic store without the mutex causes lost notifications — the classic CV race. See the `dispatch_and_wait` / `_worker_loop` pattern in `thread_pool.h` for the correct implementation.

2. **RAII for all resource ownership.** Threads, GPU buffers, file handles — wrap them in an owning type with a destructor. Never rely on manual cleanup sequences.

3. **No data races, even benign ones.** Two threads accessing the same non-atomic variable where at least one writes is undefined behavior. "It works on x86" is not a justification — the compiler may reorder or elide the access.

4. **Lock ordering must be documented and consistent.** If code acquires multiple mutexes, always acquire in the same order. Document the order in the mutex declaration comment.

5. **Prefer compile-time guarantees over runtime checks.** `= delete` over "don't call this". `unique_ptr` over "remember to reserve". `static_assert` over comments that say "must be 32 bytes".

6. **No timing-dependent correctness.** Code must not assume thread scheduling order, sleep durations, or that "the worker will finish before we check". If correctness depends on timing, it's a race condition.

### Examples

```cpp
// ❌ BAD — atomic store without lock, CV notification can be lost
if (pending_.fetch_sub(1) == 1) {   // no lock held
    cv_done_.notify_one();            // may arrive before waiter sleeps
}

// ✅ GOOD — predicate mutation under the same lock the waiter holds
{
    std::lock_guard<std::mutex> lock(mutex_);
    pending_--;
    if (pending_ == 0) {
        cv_done_.notify_one();
    }
}
```

```cpp
// ❌ BAD — relies on timing assumption
workers_done_.store(true);  // "workers always finish within 1ms"
std::this_thread::sleep_for(std::chrono::milliseconds(2));
// use results...

// ✅ GOOD — explicit synchronization
{
    std::unique_lock<std::mutex> lock(mutex_);
    cv_done_.wait(lock, [this] { return pending_ == 0; });
}
// use results...
```

### WHY NOT "it works on my machine"?

Atomics without mutex protection technically avoid data races (no UB), but they create **liveness bugs** — forever-blocked waits that freeze the application. These bugs are:
- **Intermittent**: require a ~nanosecond scheduling window that hits once every few seconds
- **Unreproducible under debugger**: debugger serializes threads, hiding the race
- **Platform-dependent**: different OS schedulers trigger different interleavings
- **Invisible to sanitizers**: no UB, no data race — just a lost CV notification

The 30-second ThreadPool freeze diagnosed in this project was exactly this class of bug.

---

## Document Invariants

> **Every synchronization primitive, ownership boundary, and lifetime contract must have an inline comment documenting its invariant.**

Code review cannot catch concurrency bugs if the reviewer doesn't know what each mutex protects. Lifetime bugs are invisible if ownership isn't stated. This rule makes implicit contracts explicit.

### The Rules

1. **Every mutex declaration states what it protects:**
   ```cpp
   std::mutex mutex_;  // Protects: work_func_, work_chunk_size_, work_total_,
                       //           pending_, work_generation_, shutdown_.
                       // Lock ordering: always acquired before cv_work_ / cv_done_ waits.
   ```

2. **Every condition_variable states its predicate and which mutex guards it:**
   ```cpp
   std::condition_variable cv_done_;  // Predicate: pending_ == 0.
                                      // Guarded by: mutex_.
                                      // Notified by: last worker to complete a chunk.
   ```

3. **Every atomic states WHY it doesn't need a mutex:**
   ```cpp
   std::atomic<uint32_t> work_next_chunk_{0};  // Lock-free chunk counter.
                                                // Not connected to any CV predicate —
                                                // safe to use without mutex_.
   ```

4. **Every `unique_ptr`, `Ref<>`, or raw pointer states who owns the pointee and when it's valid:**
   ```cpp
   std::unique_ptr<IThreadDispatch> pool_;  // Owned. Created in constructor, destroyed in destructor.
                                             // Valid for the entire lifetime of RayRenderer.
   ```

5. **Thread-safety of public methods is documented in the class docblock:**
   ```cpp
   // ThreadPool — NOT thread-safe for concurrent dispatch_and_wait calls.
   // Only ONE thread may call dispatch_and_wait at a time (the Godot main thread).
   // Worker threads are internal and never call public methods.
   ```

6. **Struct/class-level lifetime annotations for non-obvious validity windows:**
   ```cpp
   // SceneShadeData — valid only between get_shade_data() and the next build() call.
   // The caller must not store this across frames.
   ```

### Checklist

Before submitting any code with concurrency or ownership:

- [ ] Every `std::mutex` has a comment listing what it protects
- [ ] Every `std::condition_variable` has its predicate and guard documented
- [ ] Every `std::atomic` explains why it doesn't need a mutex
- [ ] Every owning pointer (`unique_ptr`, `Ref<>`) states lifetime
- [ ] Class docblock states thread-safety guarantees

---

## Error Handling

### Prefer Assertions Over Exceptions

This is a real-time ray tracer. We don't use exceptions. The error strategy is:

1. **Assertions** for programmer errors and invariant violations (see [Tiger Style](#tiger-style-assertions)).
2. **Godot `ERR_PRINT` / `WARN_PRINT`** for recoverable runtime conditions (missing mesh, invalid parameter from GDScript).
3. **Return values** for expected failure paths (`register_mesh()` returns `-1` on failure).
4. **Never silently continue** with corrupt state.

### Godot Integration

- Assertion failures go through `ERR_PRINT` so they appear in the Godot editor Output panel (raw `stderr` is invisible on Windows).
- Then `GENERATE_TRAP()` halts execution.

---

## Design Documentation

### "WHY NOT X?" Sections

When you choose one approach over another, document the alternative and why it was rejected:

```cpp
// WHY NOT std::async or OpenMP?
//   std::async creates/destroys threads per call — too much overhead for per-frame ray batches.
//   OpenMP requires compiler flags and isn't always available with MSVC + SCons.
```

### Performance / Phase Annotations

Mark fields and features that are planned for future phases:

```cpp
float metallic;     // Phase 2: Used when PBR shading is implemented
float roughness;    // Phase 2: Cook-Torrance microfacet model
```

### Bandwidth / Memory Notes

When making a size or layout decision, document the impact:

```cpp
// BANDWIDTH OPTIMIZATION: Position is NOT stored — the CPU reconstructs it
// from ray origin + direction * t. This saves 12 bytes per result,
// reducing readback from 48 bytes to 32 bytes (33% bandwidth reduction).
// At 1280×960 that's 18.7MB saved per frame in GPU→CPU transfer.
```

---

## Automated Linting

Two layers of automated enforcement:

### Project Convention Linter (`tools/lint.py`)

A custom Python script that checks conventions no off-the-shelf tool can:

```bash
# Check all source files
python tools/lint.py

# Check specific files
python tools/lint.py src/core/ray.h src/gpu/gpu_structs.h

# Show rule-by-rule summary
python tools/lint.py --summary

# Filter to one rule family
python tools/lint.py --rule tiger
python tools/lint.py --rule header
python tools/lint.py --rule gpu
python tools/lint.py --rule module
```

#### Rules Checked

| Rule | What It Enforces |
|------|-----------------|
| `header/pragma-once` | Headers must start with `#pragma once` |
| `header/description` | Line 2 must be `// filename — description` |
| `gpu/static-assert` | GPU structs need `static_assert(sizeof(...))` |
| `module/boundary` | Modules must not include server internals |
| `tiger/assertion-density` | Non-trivial functions need >= 2 assertions |
| `naming/class-pascal` | Class/struct names must be PascalCase |

The linter runs in CI on every push and pull request. **All checks must pass before merging.**

### clang-tidy (`.clang-tidy`)

Standard C++ static analysis and naming enforcement via clang-tidy. Picked up automatically by clangd in VS Code. Checks include:

- `bugprone-*` — Common bug patterns
- `readability-identifier-naming` — PascalCase classes, snake_case functions/variables, UPPER_CASE constants, trailing `_` on private members
- `modernize-use-override` — Missing `override` specifier
- `performance-*` — Unnecessary copies, implicit conversions

### clang-format (`.clang-format`)

Formatting is handled by the existing `.clang-format` config (Godot style, LLVM base).

```bash
clang-format -i src/**/*.h src/**/*.cpp
```

---

## Living Documentation

> **Keep `ROADMAP.md`, `CONTRIBUTION_GUIDELINES.md`, and `.github/copilot-instructions.md` up to date as work progresses.**

These three documents are the project's institutional memory. They prevent future contributors (human or AI) from repeating mistakes, violating established patterns, or losing hard-won architectural decisions. Stale documentation is worse than no documentation — it actively misleads.

### Why This Matters

- **AI assistants start fresh each session.** Copilot reads these files at the beginning of every conversation. If your completed work isn't reflected in the roadmap, the AI will try to re-implement it. If a new convention isn't in the guidelines, the AI won't follow it.
- **Human contributors skim before diving in.** Accurate phase status and convention lists save hours of "wait, is this already done?" questions.
- **Rules come from bugs.** TinyBVH Safety (Rule 10), Correctness Over Cleverness (Rule 11), and Document Invariants (Rule 12) all came from real bugs. If we hadn't documented them, those bugs would have recurred.

### When to Update

| Trigger | What to update | Where |
|---------|---------------|-------|
| Completed a roadmap phase or sub-phase | Mark ✅, update status text, record actual metrics (perf numbers, file counts) | `ROADMAP.md` |
| Discovered a new convention or pitfall | Add it with a rule number and examples | `CONTRIBUTION_GUIDELINES.md` + `.github/copilot-instructions.md` |
| Added new files, modules, or APIs | Update directory trees, "New files" lists | `ROADMAP.md` |
| Made an architectural decision | Document the decision AND rejected alternatives ("WHY NOT X?") | `ROADMAP.md` |
| Found and fixed a bug with a generalizable root cause | Promote the fix to a project rule | `CONTRIBUTION_GUIDELINES.md` + `.github/copilot-instructions.md` |
| Measured performance | Record numbers with hardware, resolution, scene, date | `ROADMAP.md` |

### What Lives Where

| Document | Purpose | Audience |
|----------|---------|----------|
| `ROADMAP.md` | Phase status (✅/🔧/Future), architecture diagrams, file trees, performance tables, "What We Have Today", Known Deficiencies | Everyone |
| `CONTRIBUTION_GUIDELINES.md` | Full rules with examples, anti-patterns, checklists, WHY sections | Human contributors, AI for deep reference |
| `.github/copilot-instructions.md` | Condensed rules for AI context window — must mirror the guidelines but stay concise | AI assistants (auto-included in every chat) |

### Concrete Rules

1. **Never leave a completed phase marked as "Future"** — update the heading, add ✅, write what was actually built.
2. **New rules get a number** — append to the existing numbering (Rule 13, Rule 14, ...) in both copilot-instructions and guidelines.
3. **Anti-Hallucination Checklist stays in sync** — if you add a new rule, add a corresponding checklist item in `.github/copilot-instructions.md`.
4. **Directory trees must match reality** — if you add `gpu_path_tracer.h`, it appears in the roadmap's file tree.
5. **Performance tables use real measurements** — no estimates, no "expected" numbers for completed work.
6. **The three documents must agree** — a rule in the guidelines must have a condensed version in copilot-instructions and (if phase-relevant) a mention in the roadmap.

### Anti-Patterns

```
❌ Complete a phase without updating the roadmap
   → future AI sessions will try to re-implement it

❌ Add a convention learned from a bug without documenting it
   → the bug will recur in the next PR

❌ Leave stale "TODO" or "Future" markers on completed work
   → misleads priority decisions and wastes effort

❌ Update guidelines but not copilot-instructions (or vice versa)
   → AI sees one version, human sees another — drift causes inconsistency
```

### Checklist

Before declaring any task complete:

- [ ] Does the roadmap reflect the current state of this phase?
- [ ] Are any new files listed in the roadmap's directory tree?
- [ ] Did this work reveal a new convention or pitfall? If so, is it in the guidelines?
- [ ] Is the copilot-instructions file in sync with the guidelines?
- [ ] Are performance numbers recorded (if applicable)?

---

## Demo Scene Convention

> **Every feature gets a focused demo scene. Demo scenes use imported assets, the modular UI system, and a consistent structure.**

Demo scenes live in `project/demos/` and serve two purposes: (1) regression-test features visually, (2) document usage for users and AI assistants. They are the project's integration tests.

### The Rules

1. **One scene per feature family.** Each demo targets a specific feature: `normal_map_demo`, `lighting_demo`, `pbr_demo`, `panorama_demo`. This keeps scenes focused and makes it easy to isolate regressions.

2. **Imported assets over inline resources.** Meshes come from `.obj`/`.glb` files in `project/assets/`. Textures (albedo, normal, roughness maps) are real image files, not Godot sub_resources. This is how real users work — they import models, not hand-edit `.tscn` XML.

3. **Asset generation is reproducible.** Procedural assets (test spheres, tiling normal maps, gradient panoramas) are generated by `tools/generate_demo_assets.py`. Run it once to populate `project/assets/`. Generated files are committed to the repo so cloning works immediately.

4. **Consistent scene tree structure.** Every demo scene follows this layout:

   ```
   FeatureDemo (Node3D, script = feature_demo.gd)
   ├── Camera3D             (FPS camera, fov 65-75)
   ├── RayRenderer          (camera_path = ../Camera3D)
   ├── DirectionalLight3D   (sun — read by raytracer per frame)
   ├── WorldEnvironment     (sky, ambient, tone mapping)
   ├── Display (CanvasLayer)
   │   ├── %RenderView (TextureRect, stretch_mode=5, texture_filter=1)
   │   └── %HUD (Label, yellow, font_size 16)
   └── ... geometry (MeshInstance3D nodes)
   ```

   Add `OmniLight3D` / `SpotLight3D` only in scenes that specifically test them.

5. **Use the modular UI system.** All demos instantiate `base_menu.tscn` + the appropriate panels (`renderer_panel`, `debug_panel`, `layer_panel`). Never build UI from scratch.

6. **GDScript file header.** Every `.gd` file starts with a structured comment:

   ```gdscript
   # feature_demo.gd — One-line summary.
   #
   # WHAT:  What this demo showcases (1-2 sentences).
   # WHY:   Why it exists / what feature it validates.
   #
   # SCENE LAYOUT:
   #   Node3D (this script)
   #   ├── Camera3D
   #   ├── RayRenderer
   #   └── ... (geometry)
   #
   # CONTROLS:
   #   WASD / Arrow keys — Move camera
   #   ... (all keybindings)
   ```

7. **Standard controls.** Every demo supports this baseline (via the existing FPS camera pattern):

   | Key | Action |
   |-----|--------|
   | WASD | Move camera |
   | Mouse | Look around |
   | Q / E | Down / Up |
   | TAB | Cycle render channel |
   | R | Render frame |
   | B | Cycle backend |
   | F | Toggle auto-render |
   | +/- | Change resolution |
   | L | Toggle shadows |
   | J | Toggle anti-aliasing |
   | H | Toggle render view |
   | ESC / P | Settings menu |
   | F1 | Toggle help overlay |

   Feature-specific keys are added per demo and documented in the tooltip.

8. **Mesh registration in `_ready()`.** Call `RayTracerServer.register_scene(self)` to auto-discover all `MeshInstance3D` nodes in the subtree, then call `RayTracerServer.build()`.

9. **Print a load summary.** `_ready()` prints registered mesh/triangle count and available controls:
   ```
   [NormalMapDemo] Registered 5 meshes, 2340 triangles
   ```

### File Naming

| File | Convention | Example |
|------|-----------|----------|
| Scene | `feature_demo.tscn` | `normal_map_demo.tscn` |
| Script | `feature_demo.gd` | `normal_map_demo.gd` |
| Root node name | `PascalCase` + `Demo` | `NormalMapDemo` |
| Assets | `project/assets/<category>/` | `project/assets/textures/brick_normal.png` |

### Asset Directory

```
project/
  assets/
    meshes/         # .obj / .glb imported 3D models
    textures/       # .png / .jpg / .exr texture maps
    environments/   # .hdr / .exr panorama sky images
  demos/
    feature_demo.gd
    feature_demo.tscn
    ui/             # Modular menu system (shared)
```

### Checklist for New Demos

- [ ] `.tscn` + `.gd` pair in `project/demos/`
- [ ] Meshes from imported files in `project/assets/`, not inline sub_resources
- [ ] Scene tree follows the standard layout (Camera3D, RayRenderer, lights, WorldEnvironment)
- [ ] Uses modular UI (base_menu + panels)
- [ ] GDScript header with WHAT/WHY/SCENE LAYOUT/CONTROLS
- [ ] Standard controls + feature-specific keys documented in tooltip
- [ ] Print summary in `_ready()`
- [ ] Tested: loads without errors, renders correctly, no stalls

---

## Build & Test

### Build System

SCons with godot-cpp. The canonical build command:

```bash
scons platform=windows target=template_debug
```

### Running the Game (for testing)

Run the demo scene directly from terminal without the editor UI:

```bash
& "<godot-executable-path>" --path "project" --rendering-method forward_plus
```

This launches the project in `project/` with the Vulkan Forward+ renderer.
To capture output, redirect stdout/stderr to files. `stderr` is typically
empty on Windows (Godot routes to stdout). Use `Start-Process -RedirectStandardOutput`
in PowerShell for background testing with a timeout.

### Directory Structure

```
src/
  core/          # Fundamental types: Ray, Intersection, Triangle, asserts
  api/           # Abstract interfaces (IRayService) + query types
  accel/         # BVH builder + traversal
  dispatch/      # ThreadPool, RayDispatcher (CPU parallelism)
  gpu/           # GPU structs, GPURayCaster (Vulkan compute via local RD)
  modules/
    graphics/    # CompositorEffect pipeline, RayRenderer
  gen/           # Auto-generated (doc_data, etc.)
  register_types.cpp/.h  # GDExtension entry points
```

### Adding a New File

1. Create `src/<subsystem>/new_file.h` with the [file header convention](#file-header-convention).
2. Add `src/<subsystem>/new_file.cpp` to the SCons build (it picks up `src/**/*.cpp` automatically).
3. Add Tiger Style assertions to every non-trivial function.
4. If it's a GPU struct, add `static_assert(sizeof(...))` and the GLSL mirror comment.
5. If it's a Godot-bound class, add `GDCLASS(...)` and `_bind_methods()`.
6. Register in `register_types.cpp` if it needs to be available to GDScript/editor.

---

## Summary Checklist

Before submitting code (or before an AI considers a change complete):

- [ ] **Godot-Native**: No hardcoded values that exist on scene nodes — read from the tree
- [ ] `python tools/lint.py` passes with 0 violations on changed files
- [ ] Every file has the standard header (`#pragma once` + `// filename — summary`)
- [ ] At least 2 assertions per non-trivial function
- [ ] Comments explain WHY, not WHAT
- [ ] Naming follows the conventions table
- [ ] GPU structs have `static_assert` and GLSL mirror comments
- [ ] Modules only include `api/` and `core/` — never server internals
- [ ] Resource-owning classes are non-copyable
- [ ] TinyBVH-containing types use `unique_ptr` in containers (see [TinyBVH Safety](#tinybvh-third-party-safety))
- [ ] Synchronization uses textbook patterns — no timing-dependent correctness (see [Correctness Over Cleverness](#correctness-over-cleverness))
- [ ] Every mutex, CV, atomic, and owning pointer has an invariant comment (see [Document Invariants](#document-invariants))
- [ ] Design decisions document "WHY NOT X?"
- [ ] No exceptions — assertions + return values only
- [ ] Numbers and measurements cited where relevant
- [ ] **Living Documentation**: Roadmap, guidelines, and copilot-instructions updated to reflect this work (see [Living Documentation](#living-documentation))
