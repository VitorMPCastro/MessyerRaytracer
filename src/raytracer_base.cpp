// raytracer_base.cpp — Implementation of RayTracerBase node.
//
// This file contains:
//   1. Mesh extraction: reads triangle data from MeshInstance3D children
//   2. Ray casting: traces rays against the extracted triangles
//   3. Debug visualization: draws rays, hit points, and normals in 3D
//
// All debug drawing uses ImmediateMesh, which lets you build geometry
// (lines, points) procedurally each frame. It's Godot's built-in way
// to draw debug overlays in 3D.

#include "raytracer_base.h"

#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

// ============================================================================
// Constructor / Destructor
// ============================================================================

RayTracerBase::RayTracerBase() {
}

RayTracerBase::~RayTracerBase() {
	// debug_mesh_instance is a Godot child node — Godot manages its memory.
	// Ref<> objects (debug_mesh, debug_material) are reference-counted and
	// automatically freed when no longer referenced.
}

// ============================================================================
// Godot binding — tells Godot what methods/properties this class exposes
// ============================================================================

void RayTracerBase::_bind_methods() {
	// Methods callable from GDScript
	ClassDB::bind_method(D_METHOD("build_scene"), &RayTracerBase::build_scene);
	ClassDB::bind_method(D_METHOD("cast_ray", "origin", "direction"), &RayTracerBase::cast_ray);
	ClassDB::bind_method(D_METHOD("cast_debug_rays", "origin", "forward", "grid_w", "grid_h", "fov_degrees"),
		&RayTracerBase::cast_debug_rays);
	ClassDB::bind_method(D_METHOD("clear_debug"), &RayTracerBase::clear_debug);
	ClassDB::bind_method(D_METHOD("get_triangle_count"), &RayTracerBase::get_triangle_count);

	// Properties (show up in Godot Inspector)
	ClassDB::bind_method(D_METHOD("set_debug_enabled", "enabled"), &RayTracerBase::set_debug_enabled);
	ClassDB::bind_method(D_METHOD("get_debug_enabled"), &RayTracerBase::get_debug_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug_enabled"), "set_debug_enabled", "get_debug_enabled");

	ClassDB::bind_method(D_METHOD("set_debug_ray_miss_length", "length"), &RayTracerBase::set_debug_ray_miss_length);
	ClassDB::bind_method(D_METHOD("get_debug_ray_miss_length"), &RayTracerBase::get_debug_ray_miss_length);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "debug_ray_miss_length"), "set_debug_ray_miss_length", "get_debug_ray_miss_length");

	ClassDB::bind_method(D_METHOD("set_debug_normal_length", "length"), &RayTracerBase::set_debug_normal_length);
	ClassDB::bind_method(D_METHOD("get_debug_normal_length"), &RayTracerBase::get_debug_normal_length);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "debug_normal_length"), "set_debug_normal_length", "get_debug_normal_length");
}

// ============================================================================
// Mesh extraction — reads triangles from MeshInstance3D children
// ============================================================================

void RayTracerBase::_extract_meshes_recursive(Node *node) {
	// Try to cast the node to MeshInstance3D.
	// dynamic_cast returns nullptr if the node isn't a MeshInstance3D.
	MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(node);

	if (mesh_instance != nullptr) {
		// Get the mesh resource (the 3D model data).
		Ref<Mesh> mesh = mesh_instance->get_mesh();
		if (mesh.is_null()) {
			return; // No mesh assigned — skip.
		}

		// Get the world transform of this mesh instance.
		// We need this to convert vertices from local space to world space.
		Transform3D xform = mesh_instance->get_global_transform();

		// A mesh can have multiple "surfaces" (submeshes with different materials).
		int surface_count = mesh->get_surface_count();

		for (int surf = 0; surf < surface_count; surf++) {
			// Get arrays: vertex positions, normals, indices, etc.
			// This returns a Godot Array where each element is a different data type.
			Array arrays = mesh->surface_get_arrays(surf);

			if (arrays.size() == 0) {
				continue;
			}

			// Element 0 = vertex positions (PackedVector3Array)
			PackedVector3Array vertices = arrays[Mesh::ARRAY_VERTEX];

			if (vertices.size() == 0) {
				continue;
			}

			// Element 12 = triangle indices (PackedInt32Array), may be empty for non-indexed meshes
			PackedInt32Array indices;
			if (arrays.size() > Mesh::ARRAY_INDEX && arrays[Mesh::ARRAY_INDEX].get_type() == Variant::PACKED_INT32_ARRAY) {
				indices = arrays[Mesh::ARRAY_INDEX];
			}

			uint32_t base_id = static_cast<uint32_t>(scene.triangles.size());

			if (indices.size() > 0) {
				// Indexed mesh: every 3 indices = one triangle
				for (int i = 0; i + 2 < indices.size(); i += 3) {
					Vector3 a = xform.xform(vertices[indices[i]]);
					Vector3 b = xform.xform(vertices[indices[i + 1]]);
					Vector3 c = xform.xform(vertices[indices[i + 2]]);
					scene.triangles.push_back(Triangle(a, b, c, base_id + (i / 3)));
				}
			} else {
				// Non-indexed mesh: every 3 vertices = one triangle
				for (int i = 0; i + 2 < vertices.size(); i += 3) {
					Vector3 a = xform.xform(vertices[i]);
					Vector3 b = xform.xform(vertices[i + 1]);
					Vector3 c = xform.xform(vertices[i + 2]);
					scene.triangles.push_back(Triangle(a, b, c, base_id + (i / 3)));
				}
			}
		}
	}

	// Recurse into children
	for (int i = 0; i < node->get_child_count(); i++) {
		_extract_meshes_recursive(node->get_child(i));
	}
}

void RayTracerBase::build_scene() {
	scene.clear();
	_extract_meshes_recursive(this);
	UtilityFunctions::print("[RayTracerBase] Built scene: ", scene.triangle_count(), " triangles");
}

// ============================================================================
// Ray casting
// ============================================================================

Dictionary RayTracerBase::cast_ray(const Vector3 &origin, const Vector3 &direction) const {
	Dictionary result;

	Ray r(origin, direction.normalized());
	Intersection hit = scene.cast_ray(r);

	result["hit"] = hit.hit();
	result["position"] = hit.position;
	result["normal"] = hit.normal;
	result["distance"] = hit.t;
	result["prim_id"] = static_cast<int>(hit.prim_id);

	return result;
}

// ============================================================================
// Debug visualization
// ============================================================================

void RayTracerBase::_ensure_debug_objects() {
	// Create the debug mesh instance (only once).
	if (debug_mesh_instance == nullptr) {
		debug_mesh_instance = memnew(MeshInstance3D);
		// Use global coordinates so debug lines appear in world space.
		debug_mesh_instance->set_as_top_level(true);
		add_child(debug_mesh_instance);
	}

	// Create the ImmediateMesh resource (only once).
	if (debug_mesh.is_null()) {
		debug_mesh.instantiate();
		debug_mesh_instance->set_mesh(debug_mesh);
	}

	// Create a material that uses vertex colors and no lighting.
	if (debug_material.is_null()) {
		debug_material.instantiate();
		debug_material->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
		debug_material->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
		// Make lines render on top of everything (no depth test).
		debug_material->set_flag(BaseMaterial3D::FLAG_DISABLE_DEPTH_TEST, true);
		// Enable transparency for subtle overlays.
		debug_material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
	}
}

void RayTracerBase::_draw_debug_ray(const Ray &r, const Intersection &hit) {
	if (hit.hit()) {
		// ---- HIT: green line from origin to hit point ----
		debug_mesh->surface_set_color(Color(0.0, 1.0, 0.0, 0.8));
		debug_mesh->surface_add_vertex(r.origin);
		debug_mesh->surface_set_color(Color(0.0, 1.0, 0.0, 0.4));
		debug_mesh->surface_add_vertex(hit.position);

		// ---- Hit marker: yellow cross at hit point ----
		float s = debug_hit_marker_size;
		Color yellow(1.0, 1.0, 0.0, 0.9);
		debug_mesh->surface_set_color(yellow);
		debug_mesh->surface_add_vertex(hit.position - Vector3(s, 0, 0));
		debug_mesh->surface_set_color(yellow);
		debug_mesh->surface_add_vertex(hit.position + Vector3(s, 0, 0));
		debug_mesh->surface_set_color(yellow);
		debug_mesh->surface_add_vertex(hit.position - Vector3(0, s, 0));
		debug_mesh->surface_set_color(yellow);
		debug_mesh->surface_add_vertex(hit.position + Vector3(0, s, 0));
		debug_mesh->surface_set_color(yellow);
		debug_mesh->surface_add_vertex(hit.position - Vector3(0, 0, s));
		debug_mesh->surface_set_color(yellow);
		debug_mesh->surface_add_vertex(hit.position + Vector3(0, 0, s));

		// ---- Normal arrow: cyan line from hit point along normal ----
		Color cyan(0.0, 1.0, 1.0, 0.9);
		debug_mesh->surface_set_color(cyan);
		debug_mesh->surface_add_vertex(hit.position);
		debug_mesh->surface_set_color(cyan);
		debug_mesh->surface_add_vertex(hit.position + hit.normal * debug_normal_length);
	} else {
		// ---- MISS: red line from origin along direction ----
		debug_mesh->surface_set_color(Color(1.0, 0.0, 0.0, 0.3));
		debug_mesh->surface_add_vertex(r.origin);
		debug_mesh->surface_set_color(Color(1.0, 0.0, 0.0, 0.1));
		debug_mesh->surface_add_vertex(r.origin + r.direction * debug_ray_miss_length);
	}
}

void RayTracerBase::cast_debug_rays(const Vector3 &origin, const Vector3 &forward,
		int grid_w, int grid_h, float fov_degrees) {
	if (!debug_enabled) {
		return;
	}

	_ensure_debug_objects();

	// Clear previous debug drawing.
	debug_mesh->clear_surfaces();

	// Begin drawing lines (PRIMITIVE_LINES: every 2 vertices = 1 line segment).
	debug_mesh->surface_begin(Mesh::PRIMITIVE_LINES, debug_material);

	// Compute camera basis vectors (right, up) from the forward direction.
	// This creates an orthonormal frame so we can spread rays across a grid.
	Vector3 fwd = forward.normalized();

	// Pick an "up" hint that isn't parallel to forward.
	Vector3 up_hint = Vector3(0, 1, 0);
	if (std::fabs(fwd.dot(up_hint)) > 0.99f) {
		up_hint = Vector3(1, 0, 0); // Forward is nearly vertical — use X as hint.
	}

	Vector3 right = fwd.cross(up_hint).normalized();
	Vector3 up = right.cross(fwd).normalized();

	// Compute the half-width of the view plane at distance 1.
	// tan(fov/2) gives the ratio of half-width to distance.
	float half_fov_rad = Math::deg_to_rad(fov_degrees * 0.5f);
	float half_w = std::tan(half_fov_rad);
	float half_h = half_w * (static_cast<float>(grid_h) / static_cast<float>(grid_w));

	int hit_count = 0;
	int total_rays = grid_w * grid_h;

	for (int y = 0; y < grid_h; y++) {
		for (int x = 0; x < grid_w; x++) {
			// Map pixel (x, y) to normalized coordinates [-1, 1].
			float u = (2.0f * (x + 0.5f) / grid_w - 1.0f) * half_w;
			float v = (2.0f * (y + 0.5f) / grid_h - 1.0f) * half_h;

			// Build ray direction: forward + offset in right and up directions.
			Vector3 dir = (fwd + right * u + up * v).normalized();

			Ray r(origin, dir);
			Intersection hit = scene.cast_ray(r);

			_draw_debug_ray(r, hit);

			if (hit.hit()) {
				hit_count++;
			}
		}
	}

	debug_mesh->surface_end();

	UtilityFunctions::print("[RayTracerBase] Cast ", total_rays, " rays — ",
		hit_count, " hits, ", total_rays - hit_count, " misses");
}

void RayTracerBase::clear_debug() {
	if (debug_mesh.is_valid()) {
		debug_mesh->clear_surfaces();
	}
}

// ============================================================================
// Properties
// ============================================================================

void RayTracerBase::set_debug_enabled(bool enabled) { debug_enabled = enabled; }
bool RayTracerBase::get_debug_enabled() const { return debug_enabled; }

void RayTracerBase::set_debug_ray_miss_length(float length) { debug_ray_miss_length = length; }
float RayTracerBase::get_debug_ray_miss_length() const { return debug_ray_miss_length; }

void RayTracerBase::set_debug_normal_length(float length) { debug_normal_length = length; }
float RayTracerBase::get_debug_normal_length() const { return debug_normal_length; }

int RayTracerBase::get_triangle_count() const { return scene.triangle_count(); }
