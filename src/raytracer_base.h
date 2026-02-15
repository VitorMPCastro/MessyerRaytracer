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
// DEBUG VISUALIZATION (self-contained, no other modules needed):
//   - Green lines: rays that HIT something
//   - Red lines: rays that MISSED
//   - Yellow crosses: hit points
//   - Cyan arrows: surface normals at hit points
//
// This node does NOT use Godot's RayCast3D or physics engine.
// It traces rays against raw triangle data extracted from meshes.

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/immediate_mesh.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

#include "ray/ray_scene.h"

using namespace godot;

class RayTracerBase : public Node3D {
	GDCLASS(RayTracerBase, Node3D)

private:
	// ---- Ray scene data ----
	RayScene scene;

	// ---- Debug visualization ----
	bool debug_enabled = true;
	float debug_ray_miss_length = 20.0f; // How long to draw missed ray lines
	float debug_normal_length = 0.3f;    // How long to draw normal arrows
	float debug_hit_marker_size = 0.05f; // Size of hit point crosses

	// Godot objects for drawing debug lines in 3D
	MeshInstance3D *debug_mesh_instance = nullptr;
	Ref<ImmediateMesh> debug_mesh;
	Ref<StandardMaterial3D> debug_material;

	// ---- Internal helpers ----

	// Recursively find all MeshInstance3D nodes under 'node'
	// and extract their triangles into 'scene'.
	void _extract_meshes_recursive(Node *node);

	// Set up the debug drawing objects (creates MeshInstance3D + ImmediateMesh).
	void _ensure_debug_objects();

	// Draw a single debug ray (line + hit marker + normal).
	void _draw_debug_ray(const Ray &r, const Intersection &hit);

protected:
	// Required by Godot — registers methods/properties so GDScript can use them.
	static void _bind_methods();

public:
	RayTracerBase();
	~RayTracerBase();

	// ---- Main API (callable from GDScript) ----

	// Extract triangle data from all MeshInstance3D children.
	// Must be called before casting rays.
	void build_scene();

	// Cast a single ray. Returns a Dictionary with hit info:
	//   { "hit": bool, "position": Vector3, "normal": Vector3,
	//     "distance": float, "prim_id": int }
	Dictionary cast_ray(const Vector3 &origin, const Vector3 &direction) const;

	// Cast a grid of rays from 'origin' looking toward 'forward' direction.
	// Draws debug visualization for each ray.
	// grid_w x grid_h: how many rays (e.g., 16x12 = 192 rays)
	// fov_degrees: field of view in degrees (e.g., 90.0)
	void cast_debug_rays(const Vector3 &origin, const Vector3 &forward,
		int grid_w, int grid_h, float fov_degrees);

	// Remove all debug visualization lines.
	void clear_debug();

	// ---- Properties ----
	void set_debug_enabled(bool enabled);
	bool get_debug_enabled() const;

	void set_debug_ray_miss_length(float length);
	float get_debug_ray_miss_length() const;

	void set_debug_normal_length(float length);
	float get_debug_normal_length() const;

	// ---- Info ----
	int get_triangle_count() const;
};
