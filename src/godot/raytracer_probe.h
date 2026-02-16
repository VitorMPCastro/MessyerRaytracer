#pragma once
// raytracer_probe.h -- Lightweight Node3D for scene registration and ray casting.
//
// RayTracerProbe provides two conveniences over using RayTracerServer directly:
//   1. Auto-registration: optionally registers child MeshInstance3D nodes
//      with the server when the probe enters the scene tree.
//   2. Positional casting: cast_ray() uses the probe's world position as origin,
//      so you point the node and fire rays from its transform.
//
// USAGE:
//   - Add a RayTracerProbe to the scene tree.
//   - Add MeshInstance3D children and enable auto_register (or call register_children()).
//   - Call RayTracerServer.build() once to prepare acceleration structures.
//   - Call probe.cast_ray(direction) to trace from the probe's position.

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <vector>

using namespace godot;

class RayTracerProbe : public Node3D {
	GDCLASS(RayTracerProbe, Node3D)

private:
	// When true, child MeshInstance3D nodes are auto-registered on _ready.
	bool auto_register_ = false;

	// Visibility layer bitmask used when casting rays.
	// Only triangles whose layers overlap this mask will be tested.
	// Default 0x7FFFFFFF = all 31 usable bits (Godot Variant int is signed).
	int layer_mask_ = 0x7FFFFFFF;

	// IDs of meshes we registered (so we can unregister on exit).
	std::vector<int> registered_ids_;

protected:
	static void _bind_methods();

public:
	RayTracerProbe() = default;
	~RayTracerProbe() = default;

	// Called by Godot's notification system.
	void _notification(int p_what);

	// ---- Scene registration ----

	// Register all direct MeshInstance3D children with the server.
	void register_children();

	// Recursively register all MeshInstance3D descendants with the server.
	void register_children_recursive();

	// Unregister all meshes this probe previously registered.
	void unregister_children();

	// ---- Ray casting (from probe's world position) ----

	// Cast a ray from this node's global position in the given direction.
	Dictionary cast_ray(const Vector3 &direction);

	// Test if anything is hit from this node's global position.
	bool any_hit(const Vector3 &direction, float max_distance);

	// ---- Properties ----

	void set_auto_register(bool enabled);
	bool get_auto_register() const;

	void set_layer_mask(int mask);
	int get_layer_mask() const;

	// Number of meshes this probe has registered.
	int get_registered_count() const;
};
