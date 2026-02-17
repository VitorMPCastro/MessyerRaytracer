#pragma once
// ray_service.h — Abstract interface to the ray tracing system.
//
// Modules include ONLY this header — never raytracer_server.h.
// All methods delegate to RayTracerServer via a bridge (ray_service_bridge.cpp).
//
// This decouples modules from the base raytracer's internals:
//   - No BVH, RayDispatcher, GPURayCaster, or SceneTLAS in the include chain
//   - Modules see only the abstract interface + data types from api/ and core/
//   - Easy to mock for unit testing
//
// USAGE (inside a module):
//   #include "api/ray_service.h"
//
//   IRayService *svc = get_ray_service();
//   if (!svc) { /* server not ready */ return; }
//
//   RayQuery query = RayQuery::nearest(rays, count);
//   RayQueryResult result;
//   result.hits = hits;
//   svc->submit(query, result);
//
// OWNERSHIP:
//   The returned pointer is a global singleton — do NOT delete it.
//   It becomes valid after RayTracerServer is initialized (register_types.cpp)
//   and invalid after it is destroyed.

#include "api/ray_query.h"
#include "api/scene_shade_data.h"
#include "api/gpu_types.h"
#include "api/gpu_context.h"
#include "api/thread_dispatch.h"

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/vector3.hpp>

using namespace godot;

/// Abstract interface to the ray tracing system.
/// Modules depend on this — not on RayTracerServer directly.
class IRayService {
public:
	virtual ~IRayService() = default;

	// ======== Scene management ========

	/// Register a MeshInstance3D. Returns a mesh_id (or -1 on failure).
	virtual int register_mesh(Node *mesh_instance) = 0;

	/// Unregister a previously registered mesh. Call build() to apply.
	virtual void unregister_mesh(int mesh_id) = 0;

	/// Build or rebuild acceleration structures.
	/// Re-reads transforms from all registered meshes.
	virtual void build() = 0;

	/// Remove all registered meshes and clear acceleration structures.
	virtual void clear() = 0;

	// ======== Single-ray casting ========

	/// Cast a single ray. Returns a Dictionary with hit info.
	virtual Dictionary cast_ray(const Vector3 &origin, const Vector3 &direction,
		int layer_mask = 0x7FFFFFFF) = 0;

	/// Test if a ray hits anything within max_distance (early exit).
	virtual bool any_hit(const Vector3 &origin, const Vector3 &direction,
		float max_distance, int layer_mask = 0x7FFFFFFF) = 0;

	// ======== Batch submission (preferred for modules) ========

	/// Submit a structured RayQuery and receive results.
	/// This is the recommended entry point for all module code.
	virtual void submit(const RayQuery &query, RayQueryResult &result) = 0;

	// ======== Backend control ========

	/// Set the backend mode (0=CPU, 1=GPU, 2=AUTO).
	virtual void set_backend(int mode) = 0;

	/// Get the current backend mode.
	virtual int get_backend() const = 0;

	/// Check if GPU compute is available on this hardware.
	virtual bool is_gpu_available() const = 0;

	/// Check if the system is currently using the GPU for dispatch.
	virtual bool using_gpu() const = 0;

	// ======== Stats & info ========

	/// Get statistics from the last ray cast as a Dictionary.
	virtual Dictionary get_last_stats() const = 0;

	/// Get the wall-clock time (ms) of the last ray cast.
	virtual float get_last_cast_ms() const = 0;

	/// Get the total number of triangles in the scene.
	virtual int get_triangle_count() const = 0;

	/// Get the number of registered meshes.
	virtual int get_mesh_count() const = 0;

	/// Get the total number of BVH nodes.
	virtual int get_bvh_node_count() const = 0;

	/// Get the maximum depth of the BVH.
	virtual int get_bvh_depth() const = 0;

	/// Get the number of CPU worker threads.
	virtual int get_thread_count() const = 0;

	// ======== Thread dispatch ========

	/// Get the shared thread pool for parallel work dispatch.
	/// Modules should use this instead of creating their own pool —
	/// avoids double-subscribing CPU cores.
	/// The returned pointer is valid for the lifetime of the IRayService.
	virtual IThreadDispatch *get_thread_dispatch() = 0;

	// ======== Async GPU dispatch ========

	/// Submit rays for asynchronous GPU nearest-hit tracing.
	/// Call collect_nearest() later to retrieve results.
	/// Falls back to synchronous CPU dispatch if GPU is unavailable.
	/// Ray sorting is applied transparently for large batches.
	virtual void submit_async(const Ray *rays, int count) = 0;

	/// Collect nearest-hit results from a prior submit_async() call.
	/// Blocks until the GPU finishes.  Results are unshuffled automatically
	/// if the submit call applied ray sorting.
	virtual void collect_nearest(Intersection *results, int count) = 0;

	/// Submit rays for asynchronous GPU any-hit tracing.
	virtual void submit_async_any_hit(const Ray *rays, int count) = 0;

	/// Collect any-hit results from a prior submit_async_any_hit() call.
	virtual void collect_any_hit(bool *hit_results, int count) = 0;

	/// Check if an async GPU dispatch is still in flight.
	virtual bool has_async_pending() const = 0;

	// ======== Scene shading data ========

	/// Get a read-only view of the scene's material/UV data for shading.
	/// Valid after build().  Pointers are stable until the next build().
	virtual SceneShadeData get_shade_data() const = 0;

	// ======== Scene data (for GPU upload) ========

	/// Get pre-packed GPU scene data for compositor effects.
	/// Returns triangle and BVH node buffers ready for GPU upload.
	/// Valid after build(). Stable until the next build().
	///
	/// WHY NOT get_scene()?
	///   Returning const RayScene* leaked accel/ types through the api/ layer,
	///   forcing modules to include accel/ray_scene.h and gpu/gpu_structs.h.
	///   GPUSceneUpload is defined in api/gpu_types.h — no accel/ dependency.
	virtual GPUSceneUpload get_gpu_scene_data() const = 0;

	// ======== GPU context (shared RenderingDevice) ========

	/// Get the local RenderingDevice used for GPU ray tracing.
	/// Returns nullptr if the GPU backend is not initialized.
	/// The returned device is valid for the lifetime of the IRayService.
	/// Modules must NOT free or shut down this device.
	virtual godot::RenderingDevice *get_gpu_device() = 0;

	/// Get RID handles to GPU-resident scene buffers (CWBVH, triangles).
	/// Allows modules (e.g., GPUPathTracer) to bind existing BVH data into
	/// their own descriptor sets — avoiding duplicate GPU uploads.
	/// Valid after build(). RIDs are owned by the GPU caster.
	virtual GPUSceneBufferRIDs get_gpu_scene_buffer_rids() const = 0;
};

/// Returns the global ray service backed by RayTracerServer.
/// Returns nullptr if the server singleton hasn't been created yet.
IRayService *get_ray_service();
