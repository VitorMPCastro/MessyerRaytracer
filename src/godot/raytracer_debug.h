#pragma once
// raytracer_debug.h — Debug visualization node for ray tracing.
//
// WHAT:  Draws debug ray visualizations (intersections, normals, heatmaps, BVH
//        wireframes) using ImmediateMesh, querying RayTracerServer for scene data.
//
// WHY:   Visual verification of BVH quality, ray coverage, traversal cost, and
//        layer assignments.  Essential for profiling and diagnosing ray tracing
//        issues without a full render pipeline.
//
// HOW:   cast_debug_rays() generates a camera-ray grid, dispatches via
//        RayTracerServer::cast_rays_batch(), then draws per-ray geometry into an
//        ImmediateMesh in the selected draw mode.  The MeshInstance3D child is
//        created on ENTER_TREE and destroyed on EXIT_TREE to guarantee a clean
//        lifecycle — no dangling pointers, no stale GPU resources.
//
// WHY NOT draw directly with RenderingServer?
//   ImmediateMesh is simpler, well-suited for debug line geometry, and integrates
//   with Godot's scene tree visibility (show/hide in editor, toggle via script).
//   RenderingServer would be faster for massive line counts but adds complexity
//   that isn't justified for a debug tool.
//
// DRAW MODES:
//   0 = DRAW_RAYS     — Green=hit, Red=miss, Yellow cross=hitpoint, Cyan=normal
//   1 = DRAW_NORMALS  — Rays colored by surface normal direction (RGB = XYZ)
//   2 = DRAW_DISTANCE — Heatmap: close=white → far=red → very far=dark red
//   3 = DRAW_HEATMAP  — Tri-test heatmap: few tests=blue → many tests=red
//   4 = DRAW_OVERHEAT — Like HEATMAP but highlights expensive rays (>threshold)
//   5 = DRAW_BVH      — Wireframe of BVH bounding boxes at a chosen depth level
//   6 = DRAW_LAYERS   — Color rays by which visibility layer the hit triangle is on
//
// USAGE (GDScript):
//   var debug := $RayTracerDebug
//   debug.debug_draw_mode = RayTracerDebug.DRAW_NORMALS
//   debug.cast_debug_rays(cam.global_position, -cam.global_basis.z, 32, 24, cam.fov)

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/immediate_mesh.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include "core/ray.h"
#include "core/intersection.h"
#include "core/stats.h"

#include <vector>

using namespace godot;

class RayTracerDebug : public Node3D {
	GDCLASS(RayTracerDebug, Node3D)

public:
	enum DebugDrawMode {
		DRAW_RAYS     = 0,
		DRAW_NORMALS  = 1,
		DRAW_DISTANCE = 2,
		DRAW_HEATMAP  = 3,
		DRAW_OVERHEAT = 4,
		DRAW_BVH      = 5,
		DRAW_LAYERS   = 6,
	};

private:
	// ---- Debug settings ----
	bool debug_enabled_ = true;
	DebugDrawMode draw_mode_ = DRAW_RAYS;
	float ray_miss_length_ = 20.0f;
	float normal_length_ = 0.3f;
	float hit_marker_size_ = 0.05f;
	float heatmap_max_distance_ = 50.0f;
	int heatmap_max_cost_ = 50;
	int bvh_depth_ = 0;

	// Visibility layer bitmask for debug ray casting.
	int layer_mask_ = 0x7FFFFFFF;

	// ---- Godot objects for drawing ----
	// Owned as a scene-tree child — created on ENTER_TREE, destroyed on EXIT_TREE.
	// The pointer is ONLY valid between those two notifications.
	MeshInstance3D *mesh_instance_ = nullptr;
	Ref<ImmediateMesh> mesh_;
	Ref<StandardMaterial3D> material_;

	// ---- Per-ray heatmap data (reused across frames) ----
	std::vector<int> per_ray_tri_tests_;

	// ---- Stats from last debug cast ----
	RayStats last_stats_;
	float last_cast_ms_ = 0.0f;

	// ---- Lifecycle helpers ----
	// Create the MeshInstance3D child, ImmediateMesh, and material.
	// Called on NOTIFICATION_ENTER_TREE.
	void _create_debug_objects();

	// Remove and free the MeshInstance3D child, release refs.
	// Called on NOTIFICATION_EXIT_TREE.
	void _destroy_debug_objects();

	// ---- Internal drawing helpers ----
	void _draw_debug_ray(const Ray &r, const Intersection &hit, int ray_index);
	void _draw_ray_classic(const Ray &r, const Intersection &hit);
	void _draw_ray_normals(const Ray &r, const Intersection &hit);
	void _draw_ray_distance(const Ray &r, const Intersection &hit);
	void _draw_ray_heatmap(const Ray &r, const Intersection &hit, int tri_test_count);
	void _draw_bvh_wireframe();
	void _draw_aabb_wireframe(const godot::AABB &box, const Color &color);
	void _draw_ray_layers(const Ray &r, const Intersection &hit);

protected:
	static void _bind_methods();

public:
	// Non-copyable — owns a MeshInstance3D child node.
	// GDCLASS already deletes copy constructor and defines operator= (void return).
	RayTracerDebug() = default;
	~RayTracerDebug() = default;

	// Godot lifecycle (ENTER_TREE, EXIT_TREE, VISIBILITY_CHANGED).
	void _notification(int p_what);

	// ---- Main API ----

	// Cast a grid of debug rays and visualize the results.
	// No-op when not in tree, not visible, or debug_enabled is false.
	void cast_debug_rays(const Vector3 &origin, const Vector3 &forward,
		int grid_w, int grid_h, float fov_degrees);

	// Clear all drawn debug geometry.
	void clear_debug();

	// ---- Properties ----

	void set_debug_enabled(bool enabled);
	bool get_debug_enabled() const;

	void set_draw_mode(int mode);
	int get_draw_mode() const;

	void set_ray_miss_length(float length);
	float get_ray_miss_length() const;

	void set_normal_length(float length);
	float get_normal_length() const;

	void set_heatmap_max_distance(float dist);
	float get_heatmap_max_distance() const;

	void set_heatmap_max_cost(int cost);
	int get_heatmap_max_cost() const;

	void set_bvh_depth(int depth);
	int get_bvh_depth() const;

	void set_layer_mask(int mask);
	int get_layer_mask() const;

	// ---- Stats ----

	Dictionary get_last_stats() const;
	float get_last_cast_ms() const;
};

VARIANT_ENUM_CAST(RayTracerDebug::DebugDrawMode);
