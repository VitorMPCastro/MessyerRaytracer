# Messyer's Godot Raytracer

A high-performance custom raytracer built as a Godot 4.6 GDExtension (C++). Traces rays against raw triangle geometry extracted from `MeshInstance3D` nodes — independent of Godot's physics engine.

## Features

- **BVH Acceleration** — Binned SAH (Surface Area Heuristic) with 12 bins, DFS-reordered nodes, implicit left child. Reduces per-ray cost from O(N) to ~O(log N).
- **CPU Backend** — Multithreaded via thread pool. SSE 4.1 SIMD: tests 4 triangles simultaneously. 4-ray packet BVH traversal for coherent rays. Morton-code ray sorting for cache coherence.
- **GPU Backend** — Vulkan compute shader on a local `RenderingDevice` (doesn't stall Godot's renderer). GLSL→SPIR-V compiled at runtime. Shared-memory stack BVH traversal, workgroup size 64. Async submit/collect for CPU-GPU overlap.
- **6 Debug Visualization Modes** — Rays, normals, distance heatmap, cost heatmap, overheat, BVH wireframe. All drawn with `ImmediateMesh`.
- **Inspector Integration** — Grouped properties (Debug, Acceleration), enum dropdowns for draw mode and backend, range sliders for all parameters.
- **Tiger Style Assertions** — 80+ assertions across the codebase for defensive programming. Three tiers: `RT_ASSERT` (debug), `RT_VERIFY` (production), `RT_SLOW_ASSERT` (expensive).

## Quick Start

```gdscript
# Attach this to a script on any node in a scene with MeshInstance3D children:
func _ready():
    var rt = $RayTracerBase
    rt.backend = RayTracerBase.BACKEND_AUTO  # Use GPU if available
    rt.build_scene()
    print("Built scene with %d triangles" % rt.get_triangle_count())

func _input(event):
    if event.is_action_pressed("cast_rays"):
        var cam = get_viewport().get_camera_3d()
        $RayTracerBase.cast_debug_rays(
            cam.global_position,
            -cam.global_basis.z,
            16, 12,    # 192 rays in a 16x12 grid
            60.0       # 60° horizontal FOV
        )
```

## Building

Requires: **Python 3**, **SCons**, **MSVC** (Visual Studio 2022+), **Godot 4.6**.

```bash
# Clone with submodule
git clone --recurse-submodules <repo-url>
cd MessyerRaytracer

# Build (debug, Windows x64)
scons

# The DLL is output to bin/windows/ and copied to project/bin/windows/
```

To generate `compile_commands.json` for IDE support:

```bash
scons compiledb=yes
```

## Project Structure

```
src/
├── core/           Core data types and utilities
│   ├── asserts.h         Tiger Style assertion macros (3 tiers + convenience)
│   ├── ray.h             Ray struct (origin, direction, inv_direction, t range)
│   ├── intersection.h    Intersection result (position, normal, t, prim_id)
│   ├── triangle.h        Triangle with Möller-Trumbore intersection
│   ├── aabb_intersect.h  Slab-method ray-AABB test (division-free)
│   └── stats.h           Per-cast statistics accumulator
│
├── accel/          Acceleration structures
│   ├── bvh.h             Binned SAH BVH (build, traverse, refit)
│   ├── ray_scene.h       Scene container (triangles + BVH, batch cast)
│   ├── mesh_blas.h       Per-mesh bottom-level acceleration structure
│   ├── blas_instance.h   Instanced BLAS with world transform
│   └── scene_tlas.h      Top-level acceleration structure (multi-mesh)
│
├── simd/           SIMD-accelerated kernels
│   ├── simd_tri.h        SSE 4.1 4-wide triangle intersection
│   └── ray_packet.h      4-ray packet for coherent BVH traversal
│
├── dispatch/       Ray dispatch and parallelism
│   ├── ray_dispatcher.h  Routes rays to CPU/GPU backend
│   ├── thread_pool.h     Lock-free thread pool for CPU parallelism
│   └── ray_sort.h        Morton-code ray sorting for coherence
│
├── gpu/            GPU compute backend
│   ├── gpu_ray_caster.h/.cpp   Vulkan compute pipeline (local RenderingDevice)
│   ├── gpu_structs.h           GPU-compatible packed structs (std430 layout)
│   └── bvh_traverse_comp.h     GLSL compute shader (embedded as string)
│
├── godot/          Godot integration layer
│   ├── raytracer_base.h/.cpp   The main RayTracerBase node
│   ├── register_types.h/.cpp   GDExtension registration
│   └── example_class.h/.cpp    Template example class
│
└── gen/            Auto-generated
    └── doc_data.gen.cpp         Compiled class documentation
```

## Architecture

```
                    ┌─────────────────────┐
                    │    RayTracerBase     │  Godot Node3D
                    │  (godot/ layer)      │  Extracts meshes, draws debug
                    └─────────┬───────────┘
                              │
                    ┌─────────▼───────────┐
                    │    RayDispatcher     │  Routes to best backend
                    │  CPU / GPU / Auto   │  Manages thread pool + GPU caster
                    └───┬─────────────┬───┘
                        │             │
           ┌────────────▼──┐    ┌─────▼────────────┐
           │   CPU Path    │    │    GPU Path       │
           │  ThreadPool   │    │  GPURayCaster     │
           │  RayScene     │    │  Local RD         │
           │  BVH traverse │    │  Compute shader   │
           │  SIMD packets │    │  SPIR-V pipeline  │
           └───────────────┘    └──────────────────┘
```

### Data Flow

1. **build_scene()** — Walks child nodes, extracts triangles from `MeshInstance3D` surfaces, builds BVH via binned SAH, uploads to GPU if active.
2. **cast_ray()** — Single ray through the dispatcher. Returns hit dictionary.
3. **cast_debug_rays()** — Generates ray grid from camera parameters, dispatches batch, draws debug lines via `ImmediateMesh`.

### BVH Details

- **Construction**: Binned SAH with 12 candidate split planes per axis. Greedy top-down, `MAX_LEAF_SIZE=4` triangles. O(N log N) build.
- **Memory layout**: DFS-ordered, left child is always `node + 1` (implicit, saves 4 bytes per node). Right child index stored in `left_first` for internal nodes.
- **Traversal**: Stack-based, front-to-back child ordering using `ray.dir_sign`. Stack depth capped at 64.
- **SIMD**: 4-wide Möller-Trumbore via SSE 4.1 intrinsics. Ray packets test 4 rays against each AABB simultaneously.

### GPU Compute

- Local `RenderingDevice` — isolated from Godot's main renderer.
- GLSL shader compiled to SPIR-V at runtime (embedded in `bvh_traverse_comp.h`).
- 4 storage buffers: triangles, BVH nodes, rays, results (all `std430` layout).
- Specialization constant for `any_hit` mode (compiler eliminates dead branch).
- Scene buffers uploaded once; ray/result buffers grow-only (no per-frame reallocation).
- Async API: `submit_async()` → do CPU work → `collect_nearest()`.

## Debug Draw Modes

| Mode | Enum | Description |
|------|------|-------------|
| Rays | `DRAW_RAYS` | Green=hit, red=miss, yellow cross=hitpoint, cyan=normal |
| Normals | `DRAW_NORMALS` | RGB color from surface normal XYZ |
| Distance | `DRAW_DISTANCE` | White→yellow→red→dark red by hit distance |
| Heatmap | `DRAW_HEATMAP` | Blue→green→yellow→red by triangle test count |
| Overheat | `DRAW_OVERHEAT` | Like heatmap but highlights expensive rays in white |
| BVH | `DRAW_BVH` | Bounding box wireframes at selected depth level |

## Requirements

- **Godot 4.6** (stable, mono or standard)
- **Vulkan 1.x** capable GPU (for GPU backend; CPU backend works without)
- **Windows x64** (Linux/macOS builds supported by SCons but not tested)
- **C++17** compiler (MSVC 2022+, GCC 11+, Clang 14+)

## License

See [LICENSE](LICENSE).
