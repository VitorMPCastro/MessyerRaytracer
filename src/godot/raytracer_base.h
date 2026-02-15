#pragma once
// raytracer_base.h — A Godot Node3D that provides raytracing + visual debug.
//
// HOW TO USE IN GODOT:
//   1. Add a RayTracerBase node to your scene.
//   2. Add MeshInstance3D children with visible meshes (boxes, planes, etc.).
//   3. Call build_scene() to extract triangles from those meshes.
//   4. Call cast_ray(origin, direction) to trace a single ray.
//   5. Call cast_debug_rays() to visually see rays in the 3D viewport.
//
// DEBUG VISUALIZATION MODES (controlled by debug_draw_mode property):
//   0 = RAYS     — Green=hit, Red=miss, Yellow cross=hitpoint, Cyan=normal
//   1 = NORMALS  — Rays colored by surface normal direction (RGB = XYZ)
//   2 = DISTANCE — Heatmap: close=white → far=red → very far=dark red
//   3 = HEATMAP  — Tri-test heatmap: few tests=blue → many tests=red
//   4 = OVERHEAT — Like HEATMAP but highlights expensive rays (>50 tri tests)
//   5 = BVH      — Wireframe of BVH bounding boxes at a chosen depth level
//
// This node does NOT use Godot's RayCast3D or physics engine.
// It traces rays against raw triangle data extracted from meshes.

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/immediate_mesh.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

#include "dispatch/ray_dispatcher.h"
#include "core/stats.h"

using namespace godot;

class RayTracerBase : public Node3D {
	GDCLASS(RayTracerBase, Node3D)

public:
	// Debug draw modes — selectable in the Inspector.
	enum DebugDrawMode {
		DRAW_RAYS     = 0, // Classic ray visualization (hit/miss/normal)
		DRAW_NORMALS  = 1, // Color rays by surface normal direction
		DRAW_DISTANCE = 2, // Distance heatmap (close=white, far=red)
		DRAW_HEATMAP  = 3, // Tri-test count heatmap (blue=few, red=many)
		DRAW_OVERHEAT = 4, // Highlight expensive rays (>threshold)
		DRAW_BVH      = 5, // BVH bounding box wireframe
	};

	// Backend selection — replaces the old use_gpu checkbox.
	enum BackendMode {
		BACKEND_CPU  = 0,
		BACKEND_GPU  = 1,
		BACKEND_AUTO = 2,
	};

private:
	// ---- Ray dispatch (owns scene + GPU caster + thread pool) ----
	RayDispatcher dispatcher_;
	RayStats last_stats; // Stats from most recent cast_debug_rays() call

	// ---- Debug visualization settings ----
	bool debug_enabled = true;
	DebugDrawMode debug_draw_mode = DRAW_RAYS;
	float debug_ray_miss_length = 20.0f; // How long to draw missed ray lines
	float debug_normal_length = 0.3f;    // How long to draw normal arrows
	float debug_hit_marker_size = 0.05f; // Size of hit point crosses

	// Heatmap settings
	float debug_heatmap_max_distance = 50.0f; // Max distance for distance heatmap
	int debug_heatmap_max_cost = 50;          // Max tri-tests for cost heatmap

	// BVH debug settings
	int debug_bvh_depth = 0; // Which BVH depth level to draw (0=root, -1=leaves)

	// Backend
	BackendMode backend_mode_ = BACKEND_CPU;

	// Godot objects for drawing debug lines in 3D
	MeshInstance3D *debug_mesh_instance = nullptr;
	Ref<ImmediateMesh> debug_mesh;
	Ref<StandardMaterial3D> debug_material;

	// Last cast timing for stats
	float last_cast_ms_ = 0.0f;

	// ---- Internal helpers ----
	void _extract_meshes_recursive(Node *node);
	void _ensure_debug_objects();

	// Draw a single debug ray — dispatches to the active draw mode.
	void _draw_debug_ray(const Ray &r, const Intersection &hit, int ray_index);

	// Individual draw mode implementations.
	void _draw_ray_classic(const Ray &r, const Intersection &hit);
	void _draw_ray_normals(const Ray &r, const Intersection &hit);
	void _draw_ray_distance(const Ray &r, const Intersection &hit);
	void _draw_ray_heatmap(const Ray &r, const Intersection &hit, int tri_test_count);

	// BVH wireframe: draw bounding boxes at a specific depth level.
	void _draw_bvh_wireframe();
	void _draw_aabb_wireframe(const godot::AABB &box, const Color &color);

	// Helper: store per-ray cost data for heatmap modes.
	std::vector<int> per_ray_tri_tests_;

protected:
	static void _bind_methods();

public:
	RayTracerBase();
	~RayTracerBase();

	// ---- Main API (callable from GDScript) ----
	void build_scene();

	Dictionary cast_ray(const Vector3 &origin, const Vector3 &direction);

	void cast_debug_rays(const Vector3 &origin, const Vector3 &forward,
		int grid_w, int grid_h, float fov_degrees);

	void clear_debug();

	// ---- Properties ----
	void set_debug_enabled(bool enabled);
	bool get_debug_enabled() const;

	void set_debug_draw_mode(int mode);
	int get_debug_draw_mode() const;

	void set_debug_ray_miss_length(float length);
	float get_debug_ray_miss_length() const;

	void set_debug_normal_length(float length);
	float get_debug_normal_length() const;

	void set_debug_heatmap_max_distance(float dist);
	float get_debug_heatmap_max_distance() const;

	void set_debug_heatmap_max_cost(int cost);
	int get_debug_heatmap_max_cost() const;

	void set_debug_bvh_depth(int depth);
	int get_debug_bvh_depth() const;

	// ---- BVH control ----
	void set_use_bvh(bool enabled);
	bool get_use_bvh() const;

	// ---- Backend control (replaces old use_gpu toggle) ----
	void set_backend(int mode);
	int get_backend() const;
	bool get_gpu_available() const;

	// ---- Stats ----
	Dictionary get_last_stats() const;
	float get_last_cast_ms() const;

	// ---- Info ----
	int get_triangle_count() const;
	int get_bvh_node_count() const;
	int get_bvh_depth() const;
	int get_thread_count() const;
};

// Register enums with Godot's type system so they work as properties
// and can be used with BIND_ENUM_CONSTANT in _bind_methods.
VARIANT_ENUM_CAST(RayTracerBase::DebugDrawMode);
VARIANT_ENUM_CAST(RayTracerBase::BackendMode);
