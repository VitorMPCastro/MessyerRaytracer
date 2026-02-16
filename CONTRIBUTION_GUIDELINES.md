# Contribution Guidelines

> For human contributors and AI assistants alike.
> Every convention below is already established in the codebase — follow what exists.

---

## Table of Contents

1. [Tiger Style Assertions](#tiger-style-assertions)
2. [File Header Convention](#file-header-convention)
3. [Comment Philosophy](#comment-philosophy)
4. [Naming Conventions](#naming-conventions)
5. [GPU Struct Rules](#gpu-struct-rules)
6. [Module Decoupling](#module-decoupling)
7. [Code Organization](#code-organization)
8. [Error Handling](#error-handling)
9. [Design Documentation](#design-documentation)
10. [Build & Test](#build--test)

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
- `core/*.h` — fundamental types (Ray, Intersection, Triangle)

```
✅  #include "api/ray_service.h"
❌  #include "raytracer_server.h"     // NEVER — breaks decoupling
❌  #include "accel/bvh.h"            // NEVER — internal implementation
❌  #include "dispatch/thread_pool.h"  // NEVER — internal implementation
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

## Build & Test

### Build System

SCons with godot-cpp. The canonical build command:

```bash
scons platform=windows target=template_debug
```

### Directory Structure

```
src/
  core/          # Fundamental types: Ray, Intersection, Triangle, asserts
  api/           # Abstract interfaces (IRayService) + query types
  accel/         # BVH builder + traversal
  dispatch/      # ThreadPool, RayDispatcher (CPU parallelism)
  gpu/           # GPU structs, GPURayCaster (Vulkan compute via local RD)
  modules/
    graphics/    # CompositorEffect pipeline, RaySceneSetup
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

- [ ] `python tools/lint.py` passes with 0 violations on changed files
- [ ] Every file has the standard header (`#pragma once` + `// filename — summary`)
- [ ] At least 2 assertions per non-trivial function
- [ ] Comments explain WHY, not WHAT
- [ ] Naming follows the conventions table
- [ ] GPU structs have `static_assert` and GLSL mirror comments
- [ ] Modules only include `api/` and `core/` — never server internals
- [ ] Resource-owning classes are non-copyable
- [ ] Design decisions document "WHY NOT X?"
- [ ] No exceptions — assertions + return values only
- [ ] Numbers and measurements cited where relevant
