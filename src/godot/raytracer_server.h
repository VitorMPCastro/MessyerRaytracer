#pragma once
// raytracer_server.h -- Singleton server that owns all ray tracing infrastructure.
//
// RayTracerServer is the central hub for the ray tracing system:
//   - Manages registered meshes via a two-level acceleration structure (TLAS/BLAS)
//   - Owns the RayDispatcher (CPU thread pool + GPU compute pipeline)
//   - Exposes ray casting to GDScript and other C++ classes
//
// USAGE FROM GDSCRIPT:
//   var server = RayTracerServer.get_singleton()
//   var id = server.register_mesh($MyMesh)
//   server.build()
//   var result = server.cast_ray(origin, direction)
//
// DESIGN:
//   SceneTLAS stores per-mesh object-space triangles (BLAS) and per-instance
//   world transforms. On build(), the TLAS is constructed, then all triangles
//   are flattened to world space and fed into RayDispatcher's flat RayScene.
//   This preserves SIMD packet traversal on CPU and the existing GPU shader.
//   True two-level GPU traversal is a future optimization.

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/visual_instance3d.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include "accel/scene_tlas.h"
#include "dispatch/ray_dispatcher.h"
#include "api/ray_query.h"
#include "api/scene_shade_data.h"
#include "core/stats.h"
#include "api/material_data.h"
#include "core/triangle_uv.h"
#include "core/triangle_normals.h"
#include "core/triangle_tangents.h"

#include <vector>
#include <cstdint>
#include <shared_mutex>

using namespace godot;

class RayTracerServer : public Object {
	GDCLASS(RayTracerServer, Object)

public:
	// Backend selection -- exposed to GDScript as RayTracerServer.BACKEND_CPU etc.
	enum BackendMode {
		BACKEND_CPU  = 0,
		BACKEND_GPU  = 1,
		BACKEND_AUTO = 2,
	};

private:
	static RayTracerServer *singleton_; // NOLINT(readability-identifier-naming)

	// ---- Registered mesh data ----
	struct RegisteredMesh {
		uint64_t node_id = 0;                  // ObjectID of the MeshInstance3D
		std::vector<Triangle> object_tris;     // Object-space triangles (extracted once)
		std::vector<MaterialData> object_materials;   // Per-surface materials
		std::vector<uint32_t> object_material_ids;    // Per-triangle material index (into object_materials)
		std::vector<TriangleUV> object_triangle_uvs;  // Per-triangle UV coordinates
		std::vector<TriangleNormals> object_triangle_normals; // Per-vertex smooth normals
		std::vector<TriangleTangents> object_triangle_tangents; // Per-vertex tangents (for normal mapping)
		uint32_t layer_mask = 0xFFFFFFFF;      // Godot VisualInstance3D.layers bitmask
		bool valid = false;
	};
	std::vector<RegisteredMesh> meshes_;

	// ---- Acceleration structures ----
	SceneTLAS tlas_;             // Two-level BVH (bookkeeping + future CPU two-level)
	RayDispatcher dispatcher_;   // Flat dispatch (CPU packets + GPU compute)
	bool scene_dirty_ = true;

	// ---- Scene shade data (populated at build time) ----
	std::vector<MaterialData> scene_materials_;
	std::vector<uint32_t> scene_material_ids_;
	std::vector<TriangleUV> scene_triangle_uvs_;
	std::vector<TriangleNormals> scene_triangle_normals_;
	std::vector<TriangleTangents> scene_triangle_tangents_;

	// ---- Backend ----
	BackendMode backend_mode_ = BACKEND_CPU;

	// ---- Stats ----
	RayStats last_stats_;
	float last_cast_ms_ = 0.0f;

	// ---- Thread safety ----
	// Exclusive lock for build/register/unregister/clear.
	// Shared lock for cast_ray/any_hit/cast_rays_batch (concurrent reads).
	mutable std::shared_mutex scene_mutex_;

	// ---- Internal helpers ----
	void _extract_object_triangles(MeshInstance3D *mesh_inst,
		std::vector<Triangle> &out_tris,
		std::vector<MaterialData> &out_materials,
		std::vector<uint32_t> &out_material_ids,
		std::vector<TriangleUV> &out_uvs,
		std::vector<TriangleNormals> &out_normals,
		std::vector<TriangleTangents> &out_tangents);
	void _rebuild_scene();

protected:
	static void _bind_methods();

public:
	static RayTracerServer *get_singleton() { return singleton_; }

	RayTracerServer();
	~RayTracerServer();

	// ======== Scene management ========

	// Register a MeshInstance3D. Extracts object-space triangles once.
	// Returns a mesh_id for later unregistration. Returns -1 on failure.
	int register_mesh(Node *mesh_instance);

	// Remove a previously registered mesh. Call build() to apply changes.
	void unregister_mesh(int mesh_id);

	// Build or rebuild all acceleration structures.
	// Re-reads global transforms from registered nodes.
	// Must be called after register/unregister and whenever meshes move.
	void build();

	// Remove all registered meshes and clear acceleration structures.
	void clear();

	// ======== Scene auto-discovery ========

	// Walk the subtree rooted at `root_node` and register every MeshInstance3D
	// found.  Returns the number of newly registered meshes.  Previously
	// registered meshes (matched by ObjectID) are skipped.
	//
	// Typical usage:
	//   RayTracerServer.register_scene(get_tree().current_scene)
	//   RayTracerServer.build()
	//
	// The manual register_mesh() / unregister_mesh() API still works for
	// fine-grained control.
	int register_scene(Node *root_node);

	// ======== Ray casting (GDScript-callable) ========

	// Cast a single ray, returning a Dictionary with hit info:
	//   { "hit": bool, "position": Vector3, "normal": Vector3,
	//     "distance": float, "prim_id": int }
	Dictionary cast_ray(const Vector3 &origin, const Vector3 &direction,
		int layer_mask = 0x7FFFFFFF);

	// Test if a ray hits anything within max_distance. Fast early-exit path.
	bool any_hit(const Vector3 &origin, const Vector3 &direction, float max_distance,
		int layer_mask = 0x7FFFFFFF);

	// ======== Batch ray casting (C++ internal, used by Probe/Debug) ========

	void cast_rays_batch(const Ray *rays, Intersection *results, int count,
		RayStats *stats = nullptr, uint32_t query_mask = 0xFFFFFFFF);

	// ======== Module API — structured query interface ========

	// Submit a RayQuery and receive results.  This is the preferred C++ entry
	// point for modules (AI, audio, etc.).  Routes to the optimal backend,
	// handles threading, fills stats and timing automatically.
	//
	// The caller must pre-allocate the appropriate output array:
	//   NEAREST  → result.hits      (Intersection[query.count])
	//   ANY_HIT  → result.hit_flags (bool[query.count])
	void submit(const RayQuery &query, RayQueryResult &result);

	// ======== Backend control ========

	void set_backend(int mode);
	int get_backend() const;
	bool is_gpu_available() const;

	// ======== Stats & info ========

	Dictionary get_last_stats() const;
	float get_last_cast_ms() const;
	int get_triangle_count() const;
	int get_mesh_count() const;
	int get_bvh_node_count() const;
	int get_bvh_depth() const;
	int get_thread_count() const;

	// ======== Internal accessors (for Debug/Probe, not GDScript) ========

	const RayScene &scene() const { return dispatcher_.scene(); }
	RayDispatcher &dispatcher() { return dispatcher_; }
	const RayDispatcher &dispatcher_const() const { return dispatcher_; }
	bool using_gpu() const { return dispatcher_.using_gpu(); }

	/// Read-only view of scene material data for the renderer.
	SceneShadeData get_scene_shade_data() const;
};

VARIANT_ENUM_CAST(RayTracerServer::BackendMode);
