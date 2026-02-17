# Phase 2 Implementation Guide — TinyBVH Integration

> **Temporary working document.** Delete after Phase 2 is complete.
>
> **Goal:** Replace custom BVH with TinyBVH for 2–3× GPU speedup (CWBVH) and ~40% CPU speedup (BVH4/BVH8).

---

## Design Decisions (Locked In)

| Decision | Choice | Rationale |
|----------|--------|-----------|
| GPU format | **Direct to CWBVH** | Skip Aila-Laine intermediate; maximum perf from day one |
| CPU layout | **Both BVH4_CPU + BVH8_CPU** | Runtime AVX2 detection; fallback to SSE BVH4 |
| Scene model | **True TLAS/BLAS** | Proper instancing; no more flatten-to-world-space hack |
| Visibility | **Custom intersect callback** | Per-triangle `layers` bitmask filtering |

## TinyBVH Location

- **Cloned at:** `D:\Dev\workspace\raytracer\tinybvh\tinybvh\tiny_bvh.h`
- **Copied into project at:** `thirdparty/tinybvh/tiny_bvh.h`
- Single-header library; needs `#define TINYBVH_IMPLEMENTATION` in exactly one `.cpp`

---

## Phase 2A — Foundation (Todos 2–4)

### Step 1: Add TinyBVH to Project (Todo 2)

**Files to create/modify:**

1. **`thirdparty/tinybvh/tiny_bvh.h`** — Copy from cloned repo
2. **`src/accel/tinybvh_impl.cpp`** — Implementation translation unit
   ```cpp
   // tinybvh_impl.cpp — TinyBVH single-header implementation unit.
   #define TINYBVH_IMPLEMENTATION
   #include "thirdparty/tinybvh/tiny_bvh.h"
   ```
3. **`SConstruct`** — Two changes:
   - Add `thirdparty/` to `CPPPATH` (so `#include "thirdparty/tinybvh/tiny_bvh.h"` resolves)
   - Add `Glob("src/accel/*.cpp")` to sources list (accel/ is currently header-only)

**Verification:** Build compiles with TinyBVH linked in. No symbols used yet.

### Step 2: Create TinyBVH Adapter Layer (Todo 3)

**File:** `src/accel/tinybvh_adapter.h`

Conversions needed (all inline, header-only):

| From (Ours) | To (TinyBVH) | Notes |
|-------------|--------------|-------|
| `Triangle` → | `bvhvec4[3]` vertex array | Extract v0/v1/v2; w component unused |
| Our `Ray` → | `tinybvh::Ray` | Copy origin, direction; tinybvh computes rD internally |
| `tinybvh::Intersection` → | Our `Intersection` | Map t, u, v, prim fields |
| `godot::Transform3D` → | `tinybvh::bvhmat4` | Column-major 4×4 matrix conversion |
| `tinybvh::Ray` → | Our `Ray` | For reading back results |

Key mappings:
- **tinybvh Ray** (64B): `O, mask | D, instIdx | rD | hit(t,u,v,prim)`
- **Our Ray**: `origin, direction, inv_direction, dir_sign[3], t_min, t_max, flags`
- **tinybvh Intersection**: `t, u, v, prim` (+ `inst` when INST_IDX_BITS==32)
- **Our Intersection**: `t, position, normal, u, v, prim_id, hit_layers`

Triangle vertex extraction function:
```cpp
// Convert N Triangles to 3N bvhvec4 vertices for TinyBVH builder
inline void triangles_to_vertices(const Triangle *tris, uint32_t count,
                                  std::vector<tinybvh::bvhvec4> &out) {
    out.resize(count * 3);
    for (uint32_t i = 0; i < count; i++) {
        out[i * 3 + 0] = {tris[i].v0.x, tris[i].v0.y, tris[i].v0.z, 0};
        out[i * 3 + 1] = {tris[i].v1.x, tris[i].v1.y, tris[i].v1.z, 0};
        out[i * 3 + 2] = {tris[i].v2.x, tris[i].v2.y, tris[i].v2.z, 0};
    }
}
```

### Step 3: CPU Feature Detection (Todo 4)

**File:** `src/dispatch/cpu_feature_detect.h`

- Runtime AVX2 detection via `__cpuid` / `__cpuidex` (MSVC: `<intrin.h>`)
- Check EAX=7, ECX=0 → EBX bit 5 = AVX2
- Cache result in `static bool` (computed once)
- Used by MeshBLAS to choose BVH4_CPU vs BVH8_CPU at build time

```cpp
inline bool has_avx2() {
    static const bool result = [] {
        int info[4];
        __cpuidex(info, 7, 0);
        return (info[1] & (1 << 5)) != 0;  // EBX bit 5
    }();
    return result;
}
```

---

## Phase 2B — Acceleration Structures (Todos 5–6)

### Step 4: Rewrite MeshBLAS (Todo 5)

**File:** `src/accel/mesh_blas.h` (rewrite)

**Current state:** Owns `BVH bvh` (our custom binary BVH) + `std::vector<Triangle> triangles`.

**New design:**
```
MeshBLAS
├── std::vector<Triangle> triangles_         // Keep — needed for shading (normals, UVs, layers)
├── std::vector<bvhvec4> vertices_           // NEW — 3 × bvhvec4 per tri, feeds TinyBVH builder
├── tinybvh::BVH bvh_                        // NEW — base BVH2 (used as builder input)
├── tinybvh::BVH4_CPU bvh4_                  // NEW — SSE traversal (fallback)
├── tinybvh::BVH8_CPU bvh8_                  // NEW — AVX2 traversal (preferred)
├── bool use_avx2_                           // From cpu_feature_detect
├── uint32_t id_
└── Methods:
    ├── build()        // BVH2 → convert to BVH4/BVH8 based on CPU
    ├── cast_ray()     // Dispatch to bvh4_ or bvh8_
    ├── any_hit()      // Dispatch to bvh4_ or bvh8_
    └── refit()        // For dynamic meshes
```

**Build flow:**
1. `triangles_to_vertices(triangles_, vertices_)`
2. `bvh_.Build(vertices_.data(), tri_count)` — builds BVH2
3. If AVX2: `bvh8_.Build(bvh_)` (convert from BVH2)
4. Else: `bvh4_.Build(bvh_)` (convert from BVH2)

**Intersection flow:**
1. Convert our `Ray` → `tinybvh::Ray`
2. Call `bvh8_.Intersect(ray)` or `bvh4_.Intersect(ray)`
3. Read `ray.hit` (tinybvh::Intersection) → convert to our `Intersection`
4. Look up `triangles_[prim]` for normal, layers, etc.

**Layer filtering:** Custom intersect callback or post-filter:
- Option A: Set `ray.mask` and use TinyBVH instance masks (instance-level only)
- Option B: Custom callback that checks `triangles_[prim].layers & query_mask`
- **Chosen:** Custom intersect callback (per-triangle granularity)

### Step 5: Rewrite SceneTLAS (Todo 6)

**File:** `src/accel/scene_tlas.h` (rewrite)

**Current state:** Proxy triangle hack — creates degenerate triangles matching instance AABBs, traverses with flat BVH, then tests instances.

**New design:**
```
SceneTLAS
├── std::vector<BLASInstance> our_instances_      // Our instance data
├── std::vector<tinybvh::BLASInstance> tlas_instances_  // TinyBVH instances
├── tinybvh::BVH tlas_bvh_                        // TLAS built over instances
├── std::vector<MeshBLAS*> blases_                 // Registered BLASes
├── std::vector<tinybvh::BVHBase*> bvh_bases_      // For TinyBVH TLAS build
└── Methods:
    ├── build_tlas()   // Build TLAS from BLASInstances
    ├── cast_ray()     // TLAS traversal + BLAS intersection
    └── any_hit()      // Shadow queries
```

**TLAS build flow:**
1. For each instance: Create `tinybvh::BLASInstance` with transform, blasIdx
2. `instance.Update(&blases_[blasIdx]->bvh_)` — computes world AABB + inverse transform
3. `tlas_bvh_.Build(tlas_instances_.data(), count, bvh_bases_.data(), blas_count)`

**TLAS traversal:**
- TinyBVH's `BVH::Intersect(ray, BLASInstance*, blasCount)` handles TLAS+BLAS internally
- Our wrapper adds layer filtering via custom callback

**Key change:** `BLASInstance` uses `tinybvh::bvhmat4` (4×4) not `godot::Transform3D` (3×4). Adapter handles conversion.

---

## Phase 2C — GPU Pipeline (Todos 7–8)

### Step 6: Write CWBVH GLSL Shader (Todo 7)

**File:** `src/gpu/shaders/cwbvh_traverse.comp.glsl`

**Source:** Port from `traverse_cwbvh.cl` (570 lines OpenCL) in TinyBVH repo.

**OpenCL → Vulkan GLSL mapping:**

| OpenCL | Vulkan GLSL |
|--------|-------------|
| `__bfind(x)` | `findMSB(x)` |
| `__popc(x)` | `bitCount(x)` |
| `as_float(x)` / `as_uint(x)` | `uintBitsToFloat(x)` / `floatBitsToUint(x)` |
| `__global` | SSBO (`layout(std430)`) |
| `get_global_id(0)` | `gl_GlobalInvocationID.x` |
| `sign_extend_s8x4(x, n)` | Custom: `int(bitfieldExtract(x, n*8, 8))` |
| `native_recip(x)` | `1.0 / x` (or GLSL `inversesqrt` where applicable) |
| `fma(a,b,c)` | `fma(a,b,c)` (same in GLSL) |
| `select(a,b,c)` | Ternary or `mix()` with bool |

**SSBOs (4 bindings):**

| Binding | Content | Element Size |
|---------|---------|-------------|
| 0 | CWBVH nodes (`bvh8Data`) | 5 × vec4 = 80 bytes |
| 1 | CWBVH triangles (`bvh8Tris`) | 3 × vec4 = 48 bytes |
| 2 | Rays (`GPURayPacked`) | 32 bytes |
| 3 | Results (`GPUIntersectionPacked`) | 32 bytes |
| 4 | Triangle layers (`uint[]`) | 4 bytes — **NEW** |

**Specialization constant:** `RAY_MODE` (0 = nearest, 1 = any-hit / shadow).

**Push constants:** `ray_count`, `query_mask`.

**Key CWBVH node format (80 bytes = 5 × float4):**
```
n0: nodeLo.xyz, packed(exponents_x, exponents_y, exponents_z, imask)
n1: childBaseIdx, triBaseIdx, meta[0..3] packed, meta[4..7] packed  
n2: quantized child AABB lo_x[8], hi_x[8] (packed as 2 uints)
n3: quantized child AABB lo_y[8], hi_y[8]
n4: quantized child AABB lo_z[8], hi_z[8]
```

**Stack:** Per-thread private array `uint stack[CWBVH_STACK_SIZE]` (not shared memory).  
CWBVH uses single-entry stack (nodeGroup uint2 encoding), much smaller than Aila-Laine.

**Triangle format (non-compressed, 48 bytes = 3 × float4):**
```
t0: v0.x, v0.y, v0.z, (float)primIdx
t1: e1.x, e1.y, e1.z, 0
t2: e2.x, e2.y, e2.z, 0
```
Note: e1/e2 naming in TinyBVH is SWAPPED vs standard Möller-Trumbore. Check carefully.

**Layer filtering addition:**
After triangle hit, before accepting: `if ((layers[primIdx] & query_mask) == 0) continue;`

### Step 7: Update GPU Upload — CWBVH (Todo 8)

**File:** `src/gpu/gpu_ray_caster.cpp` (modify) + `src/gpu/gpu_types.h` (modify)

**New GPU structs:**

```cpp
// GPUCWBVHNode — 5 × vec4 = 80 bytes, matches bvh8Data layout
struct GPUCWBVHNode {
    float data[20];  // 5 × 4 floats
};
static_assert(sizeof(GPUCWBVHNode) == 80, "CWBVH node must be 80 bytes");

// GPUCWBVHTri — 3 × vec4 = 48 bytes, matches bvh8Tris layout  
struct GPUCWBVHTri {
    float data[12];  // 3 × 4 floats
};
static_assert(sizeof(GPUCWBVHTri) == 48, "CWBVH tri must be 48 bytes");
```

**Upload changes in `gpu_ray_caster.cpp`:**

Replace `upload_scene()`:
1. Build CWBVH: `cwbvh_.Build(vertices, tri_count)` (or receive pre-built from SceneTLAS)
2. Upload `cwbvh_.bvh8Data` → SSBO binding 0 (node count from `cwbvh_.usedBlocks`)
3. Upload `cwbvh_.bvh8Tris` → SSBO binding 1 (tri count from `cwbvh_.idxCount`)
4. Upload triangle layers → SSBO binding 4 (uint32 per triangle)
5. Update pipeline to use new shader (`cwbvh_traverse.comp.glsl`)

**Buffer sizing:**
- Nodes: `usedBlocks * 16` bytes (usedBlocks counts 16-byte blocks; 5 blocks per node)
- Triangles: `idxCount * 48` bytes (3 × float4 per tri)

**Remove:** `GPUBVHNodeWide` (Aila-Laine) struct and old upload path.

---

## Phase 2D — Integration (Todos 9–10)

### Step 8: Update RayDispatcher (Todo 9)

**File:** `src/dispatch/ray_dispatcher.h` (modify)

**Current:** Owns flat `RayScene scene_`. CPU path calls `scene_.cast_ray()`.

**New:** Owns `SceneTLAS tlas_` instead. CPU path calls `tlas_.cast_ray()`.

Changes:
- Replace `RayScene scene_` with `SceneTLAS tlas_`
- `build()` → calls `tlas_.build_tlas()`
- CPU `trace_nearest()` → `tlas_.cast_ray()`
- CPU `trace_shadow()` → `tlas_.any_hit()`
- GPU path → `gpu_caster_.upload_scene(tlas_)` (pass CWBVH data)
- Keep `ThreadPool`, tile dispatching, backend selection logic

### Step 9: Update RayTracerServer (Todo 10)

**File:** `src/godot/raytracer_server.cpp` (modify)

**Current `_rebuild_scene()` (lines 493–650):**
1. Builds SceneTLAS (proxy triangles)
2. **Flattens ALL BLAS triangles to world-space** → pushes to `dispatcher_.scene().triangles`
3. Builds flat-world BVH over everything

**New `_rebuild_scene()`:**
1. For each registered mesh: Build `MeshBLAS` (stores both our Triangles + TinyBVH vertices/BVH)
2. For each instance: Create `BLASInstance` with transform + blasIdx
3. Call `dispatcher_.tlas().build_tlas(instances, blases)`
4. **No world-space flattening.** Triangles stay in object space.
5. GPU upload: Pass CWBVH node/tri data from the flat BVH (for now, single-BLAS GPU path)

**Important:** TinyBVH TLAS on GPU requires CWBVH per-BLAS + TLAS traversal kernel. Phase 2 MVP may use a single flattened CWBVH for GPU (build CWBVH over all world-space triangles) and true TLAS only for CPU. GPU TLAS can be Phase 2.5.

---

## Phase 2E — Verification (Todo 11)

### Step 10: Build and Lint

```bash
scons platform=windows target=template_debug   # Must compile cleanly
python tools/lint.py                            # 0 violations required
```

### Verification Checklist

- [ ] Build compiles with 0 errors
- [ ] Lint passes with 0 violations
- [ ] Every new `.h` has `#pragma once` + `// filename.h — summary`
- [ ] Every non-trivial function has ≥ 2 assertions
- [ ] GPU structs have `static_assert(sizeof(...))` + GLSL mirror comments
- [ ] No `#include` of `godot/` or `modules/` from `accel/` or `dispatch/`
- [ ] No hardcoded scene values (Godot-Native principle)
- [ ] Old BVH code removed (bvh.h, old gpu_structs entries)
- [ ] Comments explain WHY, include numbers where relevant

---

## File Change Summary

| File | Action | Phase |
|------|--------|-------|
| `thirdparty/tinybvh/tiny_bvh.h` | Copy from clone | 2A |
| `src/accel/tinybvh_impl.cpp` | Create | 2A |
| `src/accel/tinybvh_adapter.h` | Create | 2A |
| `src/dispatch/cpu_feature_detect.h` | Create | 2A |
| `SConstruct` | Modify (CPPPATH + glob) | 2A |
| `src/accel/mesh_blas.h` | Rewrite | 2B |
| `src/accel/blas_instance.h` | Rewrite | 2B |
| `src/accel/scene_tlas.h` | Rewrite | 2B |
| `src/accel/ray_scene.h` | Delete or gut | 2B |
| `src/accel/bvh.h` | Delete (replaced by TinyBVH) | 2B |
| `src/gpu/shaders/cwbvh_traverse.comp.glsl` | Create | 2C |
| `src/gpu/gpu_types.h` | Modify (CWBVH structs) | 2C |
| `src/gpu/gpu_structs.h` | Modify (remove Aila-Laine) | 2C |
| `src/gpu/gpu_ray_caster.h` | Modify | 2C |
| `src/gpu/gpu_ray_caster.cpp` | Modify (CWBVH upload + new shader) | 2C |
| `src/dispatch/ray_dispatcher.h` | Modify (TLAS instead of flat scene) | 2D |
| `src/godot/raytracer_server.h` | Modify | 2D |
| `src/godot/raytracer_server.cpp` | Modify (no world-space flatten) | 2D |

---

## Risk Notes

1. **CWBVH shader is the hardest part.** The OpenCL kernel is 570 lines of bit manipulation. Port carefully, test incrementally. Consider keeping old Aila-Laine shader as fallback until CWBVH is verified.

2. **GPU TLAS traversal is complex.** Phase 2 MVP can use a single flattened CWBVH on GPU (all triangles in world space) while CPU uses true TLAS/BLAS. GPU TLAS (multiple CWBVH BLASes + instance traversal) can be Phase 2.5.

3. **TinyBVH triangle winding.** e1/e2 in TinyBVH CWBVH tris are SWAPPED vs standard Möller-Trumbore. The CWBVH kernel handles this internally, but if we write custom intersection code, we must match.

4. **INST_IDX_BITS.** TinyBVH Intersection struct layout depends on `TINYBVH_INST_IDX_BITS`. Default is not 32. If we need instance IDs in intersection results, we need `#define TINYBVH_INST_IDX_BITS 32` before including tiny_bvh.h.

5. **SConstruct accel/ glob.** Currently no `Glob("src/accel/*.cpp")`. Must add or `tinybvh_impl.cpp` won't compile.

6. **MSVC + TinyBVH.** TinyBVH uses SSE/AVX intrinsics. MSVC should handle these with `/arch:AVX2` for the BVH8 path. May need conditional compile flags.
