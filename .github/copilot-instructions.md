# Copilot Instructions for MessyerRaytracer

> These instructions are automatically included in every Copilot Chat session for this repo.
> They exist to prevent hallucinations and enforce project conventions.

## Mandatory Reading

Before writing ANY code, consult:
- `CONTRIBUTION_GUIDELINES.md` — full rules with examples
- `ROADMAP.md` — current phase and priorities

## Rule 1: Godot-Native Principle (MOST IMPORTANT)

**Read from the scene tree. Never maintain parallel state.**

We are a GDExtension, not a standalone engine. If a value exists on a Godot node, READ it — do not hardcode it, do not create a parallel property.

### Concrete rules:
- Sun direction/color/energy → read from `DirectionalLight3D` via NodePath
- Sky colors → read from `WorldEnvironment` → `Environment` → `ProceduralSkyMaterial`
- Tone mapping mode → read from `Environment.tonemapper`
- Ambient light → read from `Environment.ambient_light_energy`
- Camera near/far → read from `Camera3D.get_near()`/`get_far()`
- Materials → already correctly read from `BaseMaterial3D` (keep this pattern)

### Pattern:
```cpp
// Expose NodePath, resolve per frame, read values
DirectionalLight3D *light = Object::cast_to<DirectionalLight3D>(get_node_or_null(light_path_));
if (light) {
    sun_dir = -light->get_global_transform().basis.get_column(2);
    sun_col = Vector3(light->get_color().r, ...) * light->get_param(Light3D::PARAM_ENERGY);
}
```

### NEVER do this:
```cpp
Vector3 sun_direction_ = Vector3(0.5f, 0.8f, 0.3f).normalized(); // ❌ hardcoded
constexpr Vector3 SKY_ZENITH(0.3f, 0.5f, 0.9f);                  // ❌ hardcoded
float depth_range_ = 100.0f;                                       // ❌ parallel state
```

### What IS uniquely ours (OK to own):
- BVH parameters (leaf size, SAH bins)
- Debug channel / AOV selection
- Backend selection (CPU/GPU)
- Ray query API (RayQuery, RayQueryResult)
- Performance tuning (thread count, tile size)

### Node Resolution Pattern (three-tier):
1. Explicit NodePath (user set in inspector) → fastest, deterministic
2. Auto-discover via `root->find_children("*", "ClassName", true, false)` → convenient fallback
3. `WARN_PRINT_ONCE` + return nullptr → alerts user

```cpp
// ✅ GOOD — three-tier resolve (from ray_renderer.cpp)
DirectionalLight3D *RayRenderer::_resolve_light() const {
    if (!light_path_.is_empty()) {
        auto *light = Object::cast_to<DirectionalLight3D>(get_node_or_null(light_path_));
        if (light) { return light; }
        WARN_PRINT_ONCE("light_path invalid — auto-discovering.");
    }
    Node *root = get_tree()->get_current_scene();
    if (root) {
        TypedArray<Node> lights = root->find_children("*", "DirectionalLight3D", true, false);
        if (lights.size() > 0) return Object::cast_to<DirectionalLight3D>(Object::cast_to<Node>(lights[0]));
    }
    WARN_PRINT_ONCE("No DirectionalLight3D found.");
    return nullptr;
}
```

### Scene Data Structs (Godot-agnostic shading):
Batch scene reads into plain structs, pass by `const &` to pure functions:
```cpp
struct EnvironmentData {
    float sky_zenith_r, sky_zenith_g, sky_zenith_b;
    float sky_horizon_r, sky_horizon_g, sky_horizon_b;
    float sky_ground_r, sky_ground_g, sky_ground_b;
    float ambient_energy, ambient_r, ambient_g, ambient_b;
    int tonemap_mode;  // 0=Linear, 1=Reinhard, 2=Filmic, 3=ACES, 4=AgX
};
// Populated per frame from WorldEnvironment → Environment → ProceduralSkyMaterial.
// shade_pass.h never includes Godot headers.
```

## Rule 2: Performance Principle (CRITICAL)

**Always choose the most performant path. This is a real-time ray tracer.**

Every line runs millions of times per frame. When two approaches are equally correct, choose the faster one.

### Concrete rules:
- Hot paths (intersection, traversal, shading) → no allocations, no virtual calls, no avoidable branches
- Data layout → SoA over AoS, cache-line aligned GPU structs, tightly packed buffers
- Batching → process rays in tiles (256–1024) via `ThreadPool::dispatch_and_wait()`
- No allocation in render loop → pre-allocate buffers, resize only on resolution change
- Branchless inner loops → conditional moves, min/max, sign tricks over if/else
- SIMD where measured → `Ray4` packets for coherent rays, SSE slab tests
- Precompute → inverse direction, reciprocals, sign bits computed once per ray
- Minimize GPU↔CPU transfer → pack results, document bandwidth savings
- Const-ref for structs, value for scalars

### NEVER do this:
```cpp
for (int i = 0; i < ray_count; i++) {
    std::vector<float> temp(3);           // ❌ malloc per ray
    result[i] = service->trace(rays[i]);  // ❌ virtual call per ray
}
```

### Always do this:
```cpp
// ✅ Pre-allocated buffer, single batch submission
service->submit(query, result);  // one call, millions of rays
```

## Rule 3: Tiger Style Assertions

Every non-trivial function needs at least 2 assertions.

| Macro | When | Use |
|-------|------|-----|
| `RT_ASSERT(cond, msg)` | Debug only | Development assumptions |
| `RT_VERIFY(cond, msg)` | Always | Invariants (data corruption) |
| `RT_SLOW_ASSERT(cond, msg)` | `RT_SLOW_CHECKS` only | Expensive validations |

Also: `RT_ASSERT_VALID_RAY`, `RT_ASSERT_FINITE`, `RT_ASSERT_NOT_NULL`, `RT_ASSERT_INDEX`.

## Rule 4: Module Decoupling

```
src/
  core/      — fundamental types (Ray, Intersection, Triangle, asserts)
  api/       — abstract interfaces (IRayService) + query types
  accel/     — BVH builder + traversal
  dispatch/  — ThreadPool, RayDispatcher
  gpu/       — GPU structs, GPURayCaster (Vulkan compute)
  modules/   — CompositorEffect, RayRenderer (graphics pipeline)
  godot/     — Godot-bound server (RaytracerServer, scene extraction)
```

**Modules only include `api/` and `core/`** — never server internals.

## Rule 5: File Convention

Every `.h` file starts with:
```cpp
#pragma once
// filename.h — one-line summary
```

## Rule 6: Naming

| Element | Style | Example |
|---------|-------|---------|
| Classes | PascalCase | `RayRenderer` |
| Functions | snake_case | `trace_rays()` |
| Members | trailing_ | `bvh_nodes_` |
| Constants | UPPER_SNAKE | `MAX_LEAF_SIZE` |
| GPU structs | GPU prefix | `GPUBVHNode` |

## Rule 7: No Exceptions

Use assertions + return values. No `throw`, no `try/catch`.

## Rule 8: Build & Verify

```bash
scons platform=windows target=template_debug  # build
python tools/lint.py                           # 0 violations required
```

## Anti-Hallucination Checklist

Before generating ANY code, ask yourself:

1. Does this value already exist on a Godot scene node? → Read it, don't invent it
2. Am I creating a new property that duplicates Godot state? → Stop, use NodePath
3. Am I using a magic number for something the user configured in the editor? → Read from scene
4. Is this the most performant way to do this? → No allocations in hot paths, batch over scatter, branchless inner loops
5. Have I added at least 2 assertions to this function? → Add them
6. Does this module include files from `godot/` or `modules/`? → Fix the dependency
7. Did I run the build and lint? → Do it before declaring done
