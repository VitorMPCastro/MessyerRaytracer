# Copilot Instructions for MessyerRaytracer

> These instructions are automatically included in every Copilot Chat session for this repo.
> They exist to prevent hallucinations and enforce project conventions.

## Mandatory Reading

Before writing ANY code, consult:
- `CONTRIBUTION_GUIDELINES.md` — full rules with examples
- `ROADMAP.md` — current phase and priorities

## Starting a New Phase

When beginning a new roadmap phase, **use the form/modal question tool (`ask_questions`)** to gather user input on non-trivial architectural and design decisions before writing code. Phases involve choices (e.g., data layout, API design, algorithm selection, shader architecture) that have long-lasting consequences — do not guess or assume a default. Present the options with brief trade-off descriptions and let the user decide.

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

### API Layer Conventions:
1. **Shared thread pool** — `IRayService::get_thread_dispatch()`. Never create your own `ThreadPool`.
2. **GPU scene data** — `IRayService::get_gpu_scene_data()` returns `GPUSceneUpload` (opaque packed buffers).
3. **Async GPU dispatch** — `submit_async()` / `collect_nearest()` for CPU/GPU overlap.
4. **Path tracing** — `IPathTracer` abstracts multi-bounce tracing. `RayRenderer` owns one (currently `CPUPathTracer`). A future GPU path tracer implements the same interface.

```cpp
// ✅ GOOD — use shared pool from service
IThreadDispatch *pool = svc->get_thread_dispatch();

// ❌ BAD — wastes CPU cores
auto pool = create_thread_dispatch();
```

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

## Rule 8: Demo Scene Convention

**Every feature gets a focused demo scene with imported assets and the modular UI system.**

### Concrete rules:
- One `.tscn` + `.gd` pair per feature family in `project/demos/`
- Meshes from `.obj`/`.glb` files in `project/assets/`, NOT inline sub_resources
- Textures are real image files in `project/assets/textures/`
- Procedural assets generated by `tools/generate_demo_assets.py` (reproducible)
- Standard scene tree: Camera3D, RayRenderer, DirectionalLight3D, WorldEnvironment, Display (CanvasLayer with %RenderView + %HUD)
- Use modular UI: `base_menu.tscn` + panels (`renderer_panel`, `debug_panel`, `layer_panel`)
- GDScript header: `# feature_demo.gd — summary` + WHAT/WHY/SCENE LAYOUT/CONTROLS
- Standard FPS camera controls (WASD, mouse, Q/E, TAB, R, B, F, +/-, L, J, ESC/P, F1)
- Register meshes in `_ready()` via `RayTracerServer.register_scene(self)` → `build()`
- Print load summary: `[DemoName] Registered N meshes, M triangles`

### File naming:
- Scene: `feature_demo.tscn`, Script: `feature_demo.gd`
- Root node: `PascalCaseDemo` (e.g., `NormalMapDemo`)
- Assets: `project/assets/meshes/`, `project/assets/textures/`, `project/assets/environments/`

## Rule 9: Build & Verify

```bash
scons platform=windows target=template_debug  # build
python tools/lint.py                           # 0 violations required
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

## Rule 10: TinyBVH Safety (CRITICAL)

**Never copy or move TinyBVH-containing types. Use `std::unique_ptr` in containers.**

TinyBVH classes (`BVH`, `BVH4_CPU`, `BVH8_CPU`, `BVH8_CWBVH`, `MBVH<M>`) have
user-defined destructors (`AlignedFree`) but **no custom copy/move constructors or
assignment operators**. Per the C++ Rule of Five, the compiler suppresses implicit
move and falls back to shallow copy — causing double-free / heap corruption.

### Concrete rules:
- Any struct containing TinyBVH types (e.g., `MeshBLAS`, `RayScene`, `SceneTLAS`) **must** be non-copyable AND non-movable
- Store such structs in `std::vector<std::unique_ptr<T>>`, never `std::vector<T>` — vector reallocation moves/copies elements
- Never assign TinyBVH objects: `bvh = BVH{}` is a shallow copy that leaks the old allocation
- TinyBVH's `Build()`/`PrepareBuild()` manage memory internally — safe to rebuild in-place without clearing

### Pattern:
```cpp
// ✅ GOOD — non-copyable struct + unique_ptr storage
struct MeshBLAS {
    MeshBLAS() = default;
    MeshBLAS(const MeshBLAS &) = delete;
    MeshBLAS &operator=(const MeshBLAS &) = delete;
    MeshBLAS(MeshBLAS &&) = delete;
    MeshBLAS &operator=(MeshBLAS &&) = delete;
    tinybvh::BVH bvh2;
    // ...
};

std::vector<std::unique_ptr<MeshBLAS>> blas_meshes_;  // safe reallocation
blas_meshes_.push_back(std::make_unique<MeshBLAS>());  // object never moves
```

### NEVER do this:
```cpp
std::vector<MeshBLAS> meshes;     // ❌ reallocation shallow-copies, double-free
meshes.reserve(n);                // ❌ band-aid, not a compile-time guarantee
bvh2 = tinybvh::BVH{};            // ❌ shallow copy leaks old allocation
```

## Rule 11: Correctness Over Cleverness (CRITICAL)

**Always use the textbook-correct pattern. Never rely on timing, platform scheduling, or "it works in practice."**

This is a multi-threaded real-time system. Subtle incorrectness causes intermittent freezes that are nearly impossible to reproduce under a debugger.

### Concrete rules:
- Condition variable predicates → mutate under the SAME mutex the waiter holds (atomic store without lock = lost notification)
- RAII for all resource ownership — threads, GPU buffers, file handles
- No data races, even "benign" ones — two threads writing the same non-atomic = UB
- Lock ordering documented and consistent
- Prefer compile-time guarantees over runtime checks (`= delete`, `unique_ptr`, `static_assert`)
- No timing-dependent correctness — if correctness depends on scheduling, it's a bug

### Pattern:
```cpp
// ✅ GOOD — predicate mutation under the same lock the waiter holds
{
    std::lock_guard<std::mutex> lock(mutex_);
    pending_--;
    if (pending_ == 0) {
        cv_done_.notify_one();
    }
}
```

### NEVER do this:
```cpp
// ❌ atomic store without lock, CV notification can be lost
if (pending_.fetch_sub(1) == 1) {   // no lock held!
    cv_done_.notify_one();            // may arrive before waiter sleeps
}
```

## Rule 12: Document Invariants

**Every synchronization primitive, ownership boundary, and lifetime contract must have an inline comment documenting its invariant.**

### Concrete rules:
- Every `std::mutex` → comment listing what it protects + lock ordering
- Every `std::condition_variable` → comment stating its predicate and guard
- Every `std::atomic` → comment explaining why it doesn't need a mutex
- Every owning pointer (`unique_ptr`, `Ref<>`) → comment stating lifetime
- Class docblock → states thread-safety guarantees

### Pattern:
```cpp
std::mutex mutex_;  // Protects: work_func_, pending_, work_generation_, shutdown_.
                    // Lock ordering: acquired before cv_work_ / cv_done_ waits.

std::condition_variable cv_done_;  // Predicate: pending_ == 0.
                                   // Guarded by: mutex_.

std::atomic<uint32_t> work_next_chunk_{0};  // Lock-free chunk counter.
                                             // Not a CV predicate — safe without mutex_.
```

## Rule 13: Living Documentation (MANDATORY)

**Keep `ROADMAP.md`, `CONTRIBUTION_GUIDELINES.md`, and `.github/copilot-instructions.md` up to date as work progresses.**

These documents are the project's institutional memory. They prevent future contributors (human or AI) from repeating mistakes, violating established patterns, or losing hard-won architectural decisions. Stale documentation is worse than no documentation — it actively misleads.

### When to update:
- **After completing a roadmap phase or sub-phase** → Mark it ✅, update status text, record actual metrics (perf numbers, file counts, lines of code)
- **After discovering a new convention or pitfall** → Add it to the guidelines and copilot-instructions so it applies to all future work
- **After adding new files, modules, or APIs** → Update the directory tree in the roadmap, add the file to relevant "New files" lists
- **After making an architectural decision** → Document the decision AND the alternatives considered ("WHY NOT X?") in the roadmap
- **After a bug is found and fixed** → If the root cause reveals a pattern that should be a rule, add it (e.g., TinyBVH Safety, Correctness Over Cleverness — both came from real bugs)
- **After performance measurements** → Record numbers with hardware, resolution, scene, and date in the roadmap

### What to update:
| Document | What to keep current |
|----------|---------------------|
| `ROADMAP.md` | Phase status (✅/🔧/Future), architecture diagrams, file trees, performance tables, "What We Have Today", Known Deficiencies |
| `CONTRIBUTION_GUIDELINES.md` | Rules, patterns, anti-patterns, checklists, examples — anything a contributor needs to write correct code |
| `.github/copilot-instructions.md` | Condensed rules for AI context window — must mirror the guidelines but stay concise |

### Concrete rules:
- **Never leave a completed phase marked as "Future"** — update the heading, add ✅, write what was actually built
- **New rules get a number** — append to the existing numbering (Rule 13, Rule 14, ...) in both copilot-instructions and guidelines
- **Anti-Hallucination Checklist stays in sync** — if you add a new rule, add a corresponding checklist item
- **Directory trees must match reality** — if you add `gpu_path_tracer.h`, it appears in the roadmap's file tree
- **Performance tables use real measurements** — no estimates, no "expected" numbers for completed work

### NEVER do this:
- Complete a phase without updating the roadmap → future AI sessions will try to re-implement it
- Add a new convention learned from a bug without documenting it → the bug will recur
- Leave stale "TODO" or "Future" markers on completed work → misleads priority decisions

## Anti-Hallucination Checklist

Before generating ANY code, ask yourself:

1. Does this value already exist on a Godot scene node? → Read it, don't invent it
2. Am I creating a new property that duplicates Godot state? → Stop, use NodePath
3. Am I using a magic number for something the user configured in the editor? → Read from scene
4. Is this the most performant way to do this? → No allocations in hot paths, batch over scatter, branchless inner loops
5. Have I added at least 2 assertions to this function? → Add them
6. Does this module include files from `godot/` or `modules/`? → Fix the dependency
7. Did I run the build and lint? → Do it before declaring done
8. Does this struct contain TinyBVH types? → Make it non-copyable, use `unique_ptr` in containers
9. Am I mutating a CV predicate without holding the waiter's mutex? → Fix it (Rule 11)
10. Does every mutex, CV, atomic, and owning pointer have an invariant comment? → Add them (Rule 12)
11. Am I creating a new ThreadPool or IThreadDispatch? → Use `svc->get_thread_dispatch()` instead
12. Am I accessing RayScene or BVH types from a module? → Use `svc->get_gpu_scene_data()` instead
13. Did I update the roadmap, guidelines, and copilot-instructions to reflect this work? → Do it (Rule 13)
