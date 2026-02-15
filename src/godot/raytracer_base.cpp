// raytracer_base.cpp — Implementation of RayTracerBase node.
//
// This file contains:
//   1. Mesh extraction: reads triangle data from MeshInstance3D children
//   2. Ray casting: traces rays against the extracted triangles
//   3. Debug visualization (6 modes): rays, normals, distance, heatmap,
//      overheat, BVH wireframe — all drawn via ImmediateMesh.
//
// All debug drawing uses ImmediateMesh, which lets you build geometry
// (lines, points) procedurally each frame. It's Godot's built-in way
// to draw debug overlays in 3D.

#include "raytracer_base.h"
#include "core/asserts.h"

#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <cstdio>
#include <chrono>
#include <vector>
#include <queue>

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
	// ---- Enum constants (visible in GDScript as RayTracerBase.DRAW_RAYS etc.) ----
	BIND_ENUM_CONSTANT(DRAW_RAYS);
	BIND_ENUM_CONSTANT(DRAW_NORMALS);
	BIND_ENUM_CONSTANT(DRAW_DISTANCE);
	BIND_ENUM_CONSTANT(DRAW_HEATMAP);
	BIND_ENUM_CONSTANT(DRAW_OVERHEAT);
	BIND_ENUM_CONSTANT(DRAW_BVH);

	BIND_ENUM_CONSTANT(BACKEND_CPU);
	BIND_ENUM_CONSTANT(BACKEND_GPU);
	BIND_ENUM_CONSTANT(BACKEND_AUTO);

	// ---- Methods callable from GDScript ----
	ClassDB::bind_method(D_METHOD("build_scene"), &RayTracerBase::build_scene);
	ClassDB::bind_method(D_METHOD("cast_ray", "origin", "direction"), &RayTracerBase::cast_ray);
	ClassDB::bind_method(D_METHOD("cast_debug_rays", "origin", "forward", "grid_w", "grid_h", "fov_degrees"),
		&RayTracerBase::cast_debug_rays);
	ClassDB::bind_method(D_METHOD("clear_debug"), &RayTracerBase::clear_debug);
	ClassDB::bind_method(D_METHOD("get_triangle_count"), &RayTracerBase::get_triangle_count);
	ClassDB::bind_method(D_METHOD("get_thread_count"), &RayTracerBase::get_thread_count);

	// ======== Property group: Debug ========
	ADD_GROUP("Debug", "debug_");

	ClassDB::bind_method(D_METHOD("set_debug_enabled", "enabled"), &RayTracerBase::set_debug_enabled);
	ClassDB::bind_method(D_METHOD("get_debug_enabled"), &RayTracerBase::get_debug_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug_enabled"), "set_debug_enabled", "get_debug_enabled");

	ClassDB::bind_method(D_METHOD("set_debug_draw_mode", "mode"), &RayTracerBase::set_debug_draw_mode);
	ClassDB::bind_method(D_METHOD("get_debug_draw_mode"), &RayTracerBase::get_debug_draw_mode);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "debug_draw_mode", PROPERTY_HINT_ENUM,
		"Rays,Normals,Distance,Heatmap,Overheat,BVH"),
		"set_debug_draw_mode", "get_debug_draw_mode");

	ClassDB::bind_method(D_METHOD("set_debug_ray_miss_length", "length"), &RayTracerBase::set_debug_ray_miss_length);
	ClassDB::bind_method(D_METHOD("get_debug_ray_miss_length"), &RayTracerBase::get_debug_ray_miss_length);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "debug_ray_miss_length", PROPERTY_HINT_RANGE, "0.1,200.0,0.1"),
		"set_debug_ray_miss_length", "get_debug_ray_miss_length");

	ClassDB::bind_method(D_METHOD("set_debug_normal_length", "length"), &RayTracerBase::set_debug_normal_length);
	ClassDB::bind_method(D_METHOD("get_debug_normal_length"), &RayTracerBase::get_debug_normal_length);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "debug_normal_length", PROPERTY_HINT_RANGE, "0.01,5.0,0.01"),
		"set_debug_normal_length", "get_debug_normal_length");

	ClassDB::bind_method(D_METHOD("set_debug_heatmap_max_distance", "dist"), &RayTracerBase::set_debug_heatmap_max_distance);
	ClassDB::bind_method(D_METHOD("get_debug_heatmap_max_distance"), &RayTracerBase::get_debug_heatmap_max_distance);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "debug_heatmap_max_distance", PROPERTY_HINT_RANGE, "1.0,500.0,1.0"),
		"set_debug_heatmap_max_distance", "get_debug_heatmap_max_distance");

	ClassDB::bind_method(D_METHOD("set_debug_heatmap_max_cost", "cost"), &RayTracerBase::set_debug_heatmap_max_cost);
	ClassDB::bind_method(D_METHOD("get_debug_heatmap_max_cost"), &RayTracerBase::get_debug_heatmap_max_cost);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "debug_heatmap_max_cost", PROPERTY_HINT_RANGE, "1,500,1"),
		"set_debug_heatmap_max_cost", "get_debug_heatmap_max_cost");

	ClassDB::bind_method(D_METHOD("set_debug_bvh_depth", "depth"), &RayTracerBase::set_debug_bvh_depth);
	ClassDB::bind_method(D_METHOD("get_debug_bvh_depth"), &RayTracerBase::get_debug_bvh_depth);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "debug_bvh_depth", PROPERTY_HINT_RANGE, "-1,32,1"),
		"set_debug_bvh_depth", "get_debug_bvh_depth");

	// ======== Property group: Acceleration ========
	ADD_GROUP("Acceleration", "");

	ClassDB::bind_method(D_METHOD("set_use_bvh", "enabled"), &RayTracerBase::set_use_bvh);
	ClassDB::bind_method(D_METHOD("get_use_bvh"), &RayTracerBase::get_use_bvh);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_bvh"), "set_use_bvh", "get_use_bvh");

	ClassDB::bind_method(D_METHOD("set_backend", "mode"), &RayTracerBase::set_backend);
	ClassDB::bind_method(D_METHOD("get_backend"), &RayTracerBase::get_backend);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "backend", PROPERTY_HINT_ENUM,
		"CPU,GPU,Auto"),
		"set_backend", "get_backend");

	ClassDB::bind_method(D_METHOD("get_gpu_available"), &RayTracerBase::get_gpu_available);

	// ======== Stats & info (read-only, not saved) ========
	ClassDB::bind_method(D_METHOD("get_last_stats"), &RayTracerBase::get_last_stats);
	ClassDB::bind_method(D_METHOD("get_last_cast_ms"), &RayTracerBase::get_last_cast_ms);
	ClassDB::bind_method(D_METHOD("get_bvh_node_count"), &RayTracerBase::get_bvh_node_count);
	ClassDB::bind_method(D_METHOD("get_bvh_depth"), &RayTracerBase::get_bvh_depth);
}

// ============================================================================
// Mesh extraction — reads triangles from MeshInstance3D children
// ============================================================================

void RayTracerBase::_extract_meshes_recursive(Node *node) {
	RT_ASSERT_NOT_NULL(node);
	MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(node);

	if (mesh_instance != nullptr) {
		Ref<Mesh> mesh = mesh_instance->get_mesh();
		if (mesh.is_null()) {
			return;
		}

		Transform3D xform = mesh_instance->get_global_transform();
		int surface_count = mesh->get_surface_count();

		for (int surf = 0; surf < surface_count; surf++) {
			Array arrays = mesh->surface_get_arrays(surf);
			if (arrays.size() == 0) {
				continue;
			}

			PackedVector3Array vertices = arrays[Mesh::ARRAY_VERTEX];
			if (vertices.size() == 0) {
				continue;
			}

			PackedInt32Array indices;
			if (arrays.size() > Mesh::ARRAY_INDEX && arrays[Mesh::ARRAY_INDEX].get_type() == Variant::PACKED_INT32_ARRAY) {
				indices = arrays[Mesh::ARRAY_INDEX];
			}

			uint32_t base_id = static_cast<uint32_t>(dispatcher_.scene().triangles.size());

			if (indices.size() > 0) {
				for (int i = 0; i + 2 < indices.size(); i += 3) {
					Vector3 a = xform.xform(vertices[indices[i]]);
					Vector3 b = xform.xform(vertices[indices[i + 1]]);
					Vector3 c = xform.xform(vertices[indices[i + 2]]);
					dispatcher_.scene().triangles.push_back(Triangle(a, b, c, base_id + (i / 3)));
				}
			} else {
				for (int i = 0; i + 2 < vertices.size(); i += 3) {
					Vector3 a = xform.xform(vertices[i]);
					Vector3 b = xform.xform(vertices[i + 1]);
					Vector3 c = xform.xform(vertices[i + 2]);
					dispatcher_.scene().triangles.push_back(Triangle(a, b, c, base_id + (i / 3)));
				}
			}
		}
	}

	for (int i = 0; i < node->get_child_count(); i++) {
		_extract_meshes_recursive(node->get_child(i));
	}
}

void RayTracerBase::build_scene() {
	dispatcher_.scene().clear();
	_extract_meshes_recursive(this);
	dispatcher_.build();

	const auto &sc = dispatcher_.scene();
	const char *backend_names[] = { "CPU", "GPU", "Auto" };
	const char *be = backend_names[static_cast<int>(backend_mode_)];

	if (sc.use_bvh && sc.bvh.is_built()) {
		UtilityFunctions::print("[RayTracerBase] Built scene: ",
			dispatcher_.triangle_count(), " triangles, ",
			dispatcher_.bvh_node_count(), " BVH nodes (depth ",
			dispatcher_.bvh_depth(), ") -- backend: ", be);
	} else {
		UtilityFunctions::print("[RayTracerBase] Built scene: ",
			dispatcher_.triangle_count(), " triangles (brute force) -- backend: ", be);
	}
}

// ============================================================================
// Ray casting
// ============================================================================

Dictionary RayTracerBase::cast_ray(const Vector3 &origin, const Vector3 &direction) {
	Dictionary result;

	Ray r(origin, direction.normalized());
	Intersection hit = dispatcher_.cast_ray(r);

	result["hit"] = hit.hit();
	result["position"] = hit.position;
	result["normal"] = hit.normal;
	result["distance"] = hit.t;
	result["prim_id"] = static_cast<int>(hit.prim_id);

	return result;
}

// ============================================================================
// Debug visualization — setup
// ============================================================================

void RayTracerBase::_ensure_debug_objects() {
	if (debug_mesh_instance == nullptr) {
		debug_mesh_instance = memnew(MeshInstance3D);
		debug_mesh_instance->set_as_top_level(true);
		add_child(debug_mesh_instance);
	}

	if (debug_mesh.is_null()) {
		debug_mesh.instantiate();
		debug_mesh_instance->set_mesh(debug_mesh);
	}

	if (debug_material.is_null()) {
		debug_material.instantiate();
		debug_material->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
		debug_material->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
		debug_material->set_flag(BaseMaterial3D::FLAG_DISABLE_DEPTH_TEST, true);
		debug_material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
	}
}

// ============================================================================
// Debug visualization — individual draw modes
// ============================================================================

// Mode 0: DRAW_RAYS — classic green/red/yellow/cyan
void RayTracerBase::_draw_ray_classic(const Ray &r, const Intersection &hit) {
	if (hit.hit()) {
		// Green line: origin → hit
		debug_mesh->surface_set_color(Color(0.0, 1.0, 0.0, 0.8));
		debug_mesh->surface_add_vertex(r.origin);
		debug_mesh->surface_set_color(Color(0.0, 1.0, 0.0, 0.4));
		debug_mesh->surface_add_vertex(hit.position);

		// Yellow cross at hit point
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

		// Cyan normal arrow
		Color cyan(0.0, 1.0, 1.0, 0.9);
		debug_mesh->surface_set_color(cyan);
		debug_mesh->surface_add_vertex(hit.position);
		debug_mesh->surface_set_color(cyan);
		debug_mesh->surface_add_vertex(hit.position + hit.normal * debug_normal_length);
	} else {
		// Red line: miss
		debug_mesh->surface_set_color(Color(1.0, 0.0, 0.0, 0.3));
		debug_mesh->surface_add_vertex(r.origin);
		debug_mesh->surface_set_color(Color(1.0, 0.0, 0.0, 0.1));
		debug_mesh->surface_add_vertex(r.origin + r.direction * debug_ray_miss_length);
	}
}

// Mode 1: DRAW_NORMALS — color rays by surface normal (RGB = XYZ mapped 0..1)
void RayTracerBase::_draw_ray_normals(const Ray &r, const Intersection &hit) {
	if (hit.hit()) {
		// Map normal components from [-1,1] to [0,1] for color.
		// This lets you "see" which direction surfaces face:
		//   X+ = red, Y+ = green, Z+ = blue
		Color nc(
			hit.normal.x * 0.5f + 0.5f,
			hit.normal.y * 0.5f + 0.5f,
			hit.normal.z * 0.5f + 0.5f,
			0.8f
		);
		debug_mesh->surface_set_color(Color(0.5, 0.5, 0.5, 0.3));
		debug_mesh->surface_add_vertex(r.origin);
		debug_mesh->surface_set_color(nc);
		debug_mesh->surface_add_vertex(hit.position);

		// Normal arrow in the same color
		debug_mesh->surface_set_color(nc);
		debug_mesh->surface_add_vertex(hit.position);
		debug_mesh->surface_set_color(nc);
		debug_mesh->surface_add_vertex(hit.position + hit.normal * debug_normal_length);
	} else {
		debug_mesh->surface_set_color(Color(0.2, 0.2, 0.2, 0.1));
		debug_mesh->surface_add_vertex(r.origin);
		debug_mesh->surface_set_color(Color(0.2, 0.2, 0.2, 0.05));
		debug_mesh->surface_add_vertex(r.origin + r.direction * debug_ray_miss_length);
	}
}

// Mode 2: DRAW_DISTANCE — distance heatmap (close = white, far = red, very far = dark)
void RayTracerBase::_draw_ray_distance(const Ray &r, const Intersection &hit) {
	if (hit.hit()) {
		// Normalize distance to [0..1] range using max distance setting.
		float t_norm = hit.t / debug_heatmap_max_distance;
		if (t_norm > 1.0f) t_norm = 1.0f;

		// Color ramp: white → yellow → red → dark red
		Color c;
		if (t_norm < 0.33f) {
			float f = t_norm / 0.33f;
			c = Color(1.0, 1.0, 1.0 - f, 0.8); // white → yellow
		} else if (t_norm < 0.66f) {
			float f = (t_norm - 0.33f) / 0.33f;
			c = Color(1.0, 1.0 - f, 0.0, 0.8);  // yellow → red
		} else {
			float f = (t_norm - 0.66f) / 0.34f;
			c = Color(1.0 - 0.5f * f, 0.0, 0.0, 0.8); // red → dark red
		}

		debug_mesh->surface_set_color(Color(0.5, 0.5, 0.5, 0.2));
		debug_mesh->surface_add_vertex(r.origin);
		debug_mesh->surface_set_color(c);
		debug_mesh->surface_add_vertex(hit.position);
	} else {
		debug_mesh->surface_set_color(Color(0.0, 0.0, 0.0, 0.1));
		debug_mesh->surface_add_vertex(r.origin);
		debug_mesh->surface_set_color(Color(0.0, 0.0, 0.0, 0.05));
		debug_mesh->surface_add_vertex(r.origin + r.direction * debug_ray_miss_length);
	}
}

// Mode 3 & 4: DRAW_HEATMAP / DRAW_OVERHEAT — color by triangle test count
// tri_test_count is fetched from per-ray stats collected during dispatch.
void RayTracerBase::_draw_ray_heatmap(const Ray &r, const Intersection &hit, int tri_test_count) {
	// Normalize cost to [0..1]. Higher = more expensive (more triangle tests).
	float cost = static_cast<float>(tri_test_count) / static_cast<float>(debug_heatmap_max_cost);
	if (cost > 1.0f) cost = 1.0f;

	// Cold→hot color ramp: blue → cyan → green → yellow → red
	Color c;
	if (cost < 0.25f) {
		float f = cost / 0.25f;
		c = Color(0.0, f, 1.0, 0.8);       // blue → cyan
	} else if (cost < 0.5f) {
		float f = (cost - 0.25f) / 0.25f;
		c = Color(0.0, 1.0, 1.0 - f, 0.8); // cyan → green
	} else if (cost < 0.75f) {
		float f = (cost - 0.5f) / 0.25f;
		c = Color(f, 1.0, 0.0, 0.8);        // green → yellow
	} else {
		float f = (cost - 0.75f) / 0.25f;
		c = Color(1.0, 1.0 - f, 0.0, 0.8);  // yellow → red
	}

	// In OVERHEAT mode, rays exceeding the threshold glow bright magenta.
	if (debug_draw_mode == DRAW_OVERHEAT && tri_test_count > debug_heatmap_max_cost) {
		c = Color(1.0, 0.0, 1.0, 1.0); // magenta = over budget
	}

	if (hit.hit()) {
		debug_mesh->surface_set_color(Color(c.r, c.g, c.b, 0.2));
		debug_mesh->surface_add_vertex(r.origin);
		debug_mesh->surface_set_color(c);
		debug_mesh->surface_add_vertex(hit.position);
	} else {
		debug_mesh->surface_set_color(Color(c.r, c.g, c.b, 0.15));
		debug_mesh->surface_add_vertex(r.origin);
		debug_mesh->surface_set_color(Color(c.r, c.g, c.b, 0.05));
		debug_mesh->surface_add_vertex(r.origin + r.direction * debug_ray_miss_length);
	}
}

// ============================================================================
// Debug visualization — dispatch to active mode
// ============================================================================

void RayTracerBase::_draw_debug_ray(const Ray &r, const Intersection &hit, int ray_index) {
	switch (debug_draw_mode) {
		case DRAW_RAYS:
			_draw_ray_classic(r, hit);
			break;

		case DRAW_NORMALS:
			_draw_ray_normals(r, hit);
			break;

		case DRAW_DISTANCE:
			_draw_ray_distance(r, hit);
			break;

		case DRAW_HEATMAP:
		case DRAW_OVERHEAT: {
			// Get per-ray cost from the collected stats.
			int cost = (ray_index >= 0 && ray_index < static_cast<int>(per_ray_tri_tests_.size()))
				? per_ray_tri_tests_[ray_index] : 0;
			_draw_ray_heatmap(r, hit, cost);
			break;
		}

		case DRAW_BVH:
			// BVH mode draws wireframes, not rays.
			// We still draw a faint ray for context.
			if (hit.hit()) {
				debug_mesh->surface_set_color(Color(1.0, 1.0, 1.0, 0.15));
				debug_mesh->surface_add_vertex(r.origin);
				debug_mesh->surface_set_color(Color(1.0, 1.0, 1.0, 0.05));
				debug_mesh->surface_add_vertex(hit.position);
			}
			break;
	}
}

// ============================================================================
// Debug visualization — BVH wireframe drawing
// ============================================================================

void RayTracerBase::_draw_aabb_wireframe(const godot::AABB &box, const Color &color) {
	// An AABB has 12 edges. Draw each as a line segment.
	// This is the standard way to visualize bounding boxes.
	Vector3 p = box.position;
	Vector3 s = box.size;

	// 8 corner points
	Vector3 corners[8] = {
		p,
		p + Vector3(s.x, 0, 0),
		p + Vector3(s.x, s.y, 0),
		p + Vector3(0, s.y, 0),
		p + Vector3(0, 0, s.z),
		p + Vector3(s.x, 0, s.z),
		p + Vector3(s.x, s.y, s.z),
		p + Vector3(0, s.y, s.z),
	};

	// 12 edges: bottom face (0-1, 1-2, 2-3, 3-0), top face (4-5, 5-6, 6-7, 7-4),
	// verticals (0-4, 1-5, 2-6, 3-7)
	static const int edges[12][2] = {
		{0,1}, {1,2}, {2,3}, {3,0},
		{4,5}, {5,6}, {6,7}, {7,4},
		{0,4}, {1,5}, {2,6}, {3,7},
	};

	for (int e = 0; e < 12; e++) {
		debug_mesh->surface_set_color(color);
		debug_mesh->surface_add_vertex(corners[edges[e][0]]);
		debug_mesh->surface_set_color(color);
		debug_mesh->surface_add_vertex(corners[edges[e][1]]);
	}
}

void RayTracerBase::_draw_bvh_wireframe() {
	const auto &sc = dispatcher_.scene();
	if (!sc.use_bvh || !sc.bvh.is_built()) {
		UtilityFunctions::print("[RayTracerBase] BVH not built -- nothing to draw");
		return;
	}

	const auto &nodes = sc.bvh.get_nodes();
	if (nodes.empty()) return;

	// We do a breadth-first traversal and draw nodes at the requested depth.
	// depth == -1 means draw ALL leaf nodes, regardless of depth.
	// depth == 0 means root only, depth == 1 means root's children, etc.
	//
	// BVH LAYOUT REMINDER (DFS order with implicit left child):
	//   Internal node at index i: left child = i + 1, right child = left_first
	//   Leaf node: left_first = first triangle index, count > 0

	int target_depth = debug_bvh_depth;

	// Depth-first coloring: use different hues per depth level for clarity.
	// We generate colors from HSV with constant saturation/value.
	auto depth_color = [](int depth) -> Color {
		// Cycle hue around the color wheel as depth increases.
		float hue = std::fmod(depth * 0.15f, 1.0f);
		// Convert HSV to RGB (simplified — just Godot's Color constructor).
		return Color::from_hsv(hue, 0.8f, 1.0f, 0.5f);
	};

	// BFS using a queue of (node_index, depth).
	struct NodeEntry {
		uint32_t idx;
		int depth;
	};
	std::queue<NodeEntry> queue;
	queue.push({ 0, 0 });

	int boxes_drawn = 0;
	int max_observed_depth = 0;

	while (!queue.empty()) {
		auto [idx, depth] = queue.front();
		queue.pop();

		if (idx >= nodes.size()) continue;
		const auto &node = nodes[idx];

		if (depth > max_observed_depth) max_observed_depth = depth;

		if (target_depth == -1) {
			// Draw ALL leaves
			if (node.is_leaf()) {
				_draw_aabb_wireframe(node.bounds, depth_color(depth));
				boxes_drawn++;
			}
		} else if (depth == target_depth) {
			// Draw nodes at exactly this depth
			_draw_aabb_wireframe(node.bounds, depth_color(depth));
			boxes_drawn++;
			continue; // Don't descend further
		}

		// Descend into children if not a leaf and we haven't reached target depth
		if (!node.is_leaf()) {
			uint32_t left = idx + 1;        // Implicit left child (DFS order)
			uint32_t right = node.left_first; // Explicit right child
			if (target_depth == -1 || depth < target_depth) {
				queue.push({ left, depth + 1 });
				queue.push({ right, depth + 1 });
			}
		}
	}

	UtilityFunctions::print("[RayTracerBase] BVH wireframe: drew ", boxes_drawn,
		" boxes (depth=", target_depth == -1 ? "leaves" : String::num_int64(target_depth),
		", max_depth=", max_observed_depth, ")");
}

// ============================================================================
// cast_debug_rays — the main debug entry point
// ============================================================================

void RayTracerBase::cast_debug_rays(const Vector3 &origin, const Vector3 &forward,
		int grid_w, int grid_h, float fov_degrees) {
	RT_ASSERT(grid_w > 0 && grid_h > 0, "Debug ray grid dimensions must be positive");
	RT_ASSERT(fov_degrees > 0.0f && fov_degrees < 180.0f, "FOV must be in (0, 180) degrees");
	if (!debug_enabled) {
		return;
	}

	_ensure_debug_objects();
	debug_mesh->clear_surfaces();
	debug_mesh->surface_begin(Mesh::PRIMITIVE_LINES, debug_material);

	// ---- BVH wireframe mode: only draws boxes, then adds faint ray overlay ----
	if (debug_draw_mode == DRAW_BVH) {
		_draw_bvh_wireframe();
	}

	// ---- Compute camera basis (same as before) ----
	Vector3 fwd = forward.normalized();
	Vector3 up_hint = Vector3(0, 1, 0);
	if (std::fabs(fwd.dot(up_hint)) > 0.99f) {
		up_hint = Vector3(1, 0, 0);
	}
	Vector3 right = fwd.cross(up_hint).normalized();
	Vector3 up = right.cross(fwd).normalized();

	float half_fov_rad = Math::deg_to_rad(fov_degrees * 0.5f);
	float half_w = std::tan(half_fov_rad);
	float half_h = half_w * (static_cast<float>(grid_h) / static_cast<float>(grid_w));

	int total_rays = grid_w * grid_h;

	// ---- 1. Generate all rays ----
	std::vector<Ray> rays(total_rays);
	for (int y = 0; y < grid_h; y++) {
		for (int x = 0; x < grid_w; x++) {
			float u = (2.0f * (x + 0.5f) / grid_w - 1.0f) * half_w;
			float v = (2.0f * (y + 0.5f) / grid_h - 1.0f) * half_h;
			Vector3 dir = (fwd + right * u + up * v).normalized();
			rays[y * grid_w + x] = Ray(origin, dir);
		}
	}

	// ---- 2. Cast all rays with timing ----
	std::vector<Intersection> results(total_rays);
	last_stats.reset();

	// For heatmap modes, we need per-ray stats. We cast rays individually
	// on CPU to collect per-ray tri_tests. On GPU, we use batch and
	// approximate from the aggregate stats (GPU doesn't track per-ray).
	bool need_per_ray_cost = (debug_draw_mode == DRAW_HEATMAP || debug_draw_mode == DRAW_OVERHEAT);
	bool used_gpu = dispatcher_.using_gpu();

	auto t0 = std::chrono::high_resolution_clock::now();

	if (need_per_ray_cost && !used_gpu) {
		// CPU per-ray stats: cast individually to accumulate per-ray cost.
		per_ray_tri_tests_.resize(total_rays);
		for (int i = 0; i < total_rays; i++) {
			RayStats per;
			per.reset();
			results[i] = dispatcher_.scene().cast_ray(rays[i], &per);
			per_ray_tri_tests_[i] = static_cast<int>(per.tri_tests);
			last_stats += per;
		}
	} else {
		// Batch dispatch (fast path — uses threading, SIMD, GPU, etc.)
		dispatcher_.cast_rays(rays.data(), results.data(), total_rays, &last_stats);

		if (need_per_ray_cost) {
			// GPU doesn't have per-ray cost. Approximate: spread evenly.
			per_ray_tri_tests_.resize(total_rays);
			int avg = (last_stats.rays_cast > 0)
				? static_cast<int>(last_stats.tri_tests / last_stats.rays_cast) : 0;
			std::fill(per_ray_tri_tests_.begin(), per_ray_tri_tests_.end(), avg);
		}
	}

	auto t1 = std::chrono::high_resolution_clock::now();
	last_cast_ms_ = std::chrono::duration<float, std::milli>(t1 - t0).count();

	// ---- 3. Draw debug visualization ----
	int hit_count = 0;
	for (int i = 0; i < total_rays; i++) {
		_draw_debug_ray(rays[i], results[i], i);
		if (results[i].hit()) {
			hit_count++;
		}
	}

	debug_mesh->surface_end();

	// ---- 4. Print performance summary ----
	const char *mode_names[] = { "Rays", "Normals", "Distance", "Heatmap", "Overheat", "BVH" };
	const char *mode_name = mode_names[static_cast<int>(debug_draw_mode)];

	char buf[512];
	if (used_gpu) {
		std::snprintf(buf, sizeof(buf),
			"[RayTracerBase] GPU [%s]: Cast %d rays -- %d hits (%.1f%%) in %.3f ms",
			mode_name, total_rays, hit_count,
			total_rays > 0 ? 100.0f * hit_count / total_rays : 0.0f,
			last_cast_ms_);
	} else {
		std::snprintf(buf, sizeof(buf),
			"[RayTracerBase] CPU [%s]: Cast %d rays -- %d hits (%.1f%%) | %.1f tri/ray, %.1f nodes/ray | %.3f ms",
			mode_name, total_rays, hit_count, last_stats.hit_rate_percent(),
			last_stats.avg_tri_tests_per_ray(), last_stats.avg_nodes_per_ray(),
			last_cast_ms_);
	}
	UtilityFunctions::print(buf);
}

void RayTracerBase::clear_debug() {
	if (debug_mesh.is_valid()) {
		debug_mesh->clear_surfaces();
	}
}

// ============================================================================
// Properties — getters/setters
// ============================================================================

void RayTracerBase::set_debug_enabled(bool enabled) { debug_enabled = enabled; }
bool RayTracerBase::get_debug_enabled() const { return debug_enabled; }

void RayTracerBase::set_debug_draw_mode(int mode) {
	if (mode >= 0 && mode <= static_cast<int>(DRAW_BVH)) {
		debug_draw_mode = static_cast<DebugDrawMode>(mode);
	}
}
int RayTracerBase::get_debug_draw_mode() const { return static_cast<int>(debug_draw_mode); }

void RayTracerBase::set_debug_ray_miss_length(float length) { debug_ray_miss_length = length; }
float RayTracerBase::get_debug_ray_miss_length() const { return debug_ray_miss_length; }

void RayTracerBase::set_debug_normal_length(float length) { debug_normal_length = length; }
float RayTracerBase::get_debug_normal_length() const { return debug_normal_length; }

void RayTracerBase::set_debug_heatmap_max_distance(float dist) { debug_heatmap_max_distance = dist; }
float RayTracerBase::get_debug_heatmap_max_distance() const { return debug_heatmap_max_distance; }

void RayTracerBase::set_debug_heatmap_max_cost(int cost) { debug_heatmap_max_cost = cost > 0 ? cost : 1; }
int RayTracerBase::get_debug_heatmap_max_cost() const { return debug_heatmap_max_cost; }

void RayTracerBase::set_debug_bvh_depth(int depth) { debug_bvh_depth = depth; }
int RayTracerBase::get_debug_bvh_depth() const { return debug_bvh_depth; }

void RayTracerBase::set_use_bvh(bool enabled) { dispatcher_.scene().use_bvh = enabled; }
bool RayTracerBase::get_use_bvh() const { return dispatcher_.scene().use_bvh; }

void RayTracerBase::set_backend(int mode) {
	if (mode < 0 || mode > static_cast<int>(BACKEND_AUTO)) return;
	backend_mode_ = static_cast<BackendMode>(mode);

	// Map our enum to the dispatcher's Backend enum.
	switch (backend_mode_) {
		case BACKEND_CPU:
			dispatcher_.set_backend(RayDispatcher::Backend::CPU);
			break;

		case BACKEND_GPU:
			dispatcher_.set_backend(RayDispatcher::Backend::GPU);
			// Lazy GPU init: initialize on first selection.
			if (!dispatcher_.gpu_available()) {
				if (!dispatcher_.initialize_gpu()) {
					UtilityFunctions::print("[RayTracerBase] GPU init failed -- falling back to CPU");
					dispatcher_.set_backend(RayDispatcher::Backend::CPU);
					backend_mode_ = BACKEND_CPU;
					return;
				}
				dispatcher_.upload_to_gpu();
			}
			break;

		case BACKEND_AUTO:
			dispatcher_.set_backend(RayDispatcher::Backend::AUTO);
			// Auto mode: try GPU, fall back to CPU silently.
			if (!dispatcher_.gpu_available()) {
				dispatcher_.initialize_gpu(); // Best-effort, no error if fails
			}
			break;
	}
}
int RayTracerBase::get_backend() const { return static_cast<int>(backend_mode_); }

bool RayTracerBase::get_gpu_available() const { return dispatcher_.gpu_available(); }

Dictionary RayTracerBase::get_last_stats() const {
	Dictionary d;
	d["rays_cast"] = static_cast<int64_t>(last_stats.rays_cast);
	d["tri_tests"] = static_cast<int64_t>(last_stats.tri_tests);
	d["bvh_nodes_visited"] = static_cast<int64_t>(last_stats.bvh_nodes_visited);
	d["hits"] = static_cast<int64_t>(last_stats.hits);
	d["avg_tri_tests_per_ray"] = last_stats.avg_tri_tests_per_ray();
	d["avg_nodes_per_ray"] = last_stats.avg_nodes_per_ray();
	d["hit_rate_percent"] = last_stats.hit_rate_percent();
	d["cast_time_ms"] = last_cast_ms_;
	return d;
}

float RayTracerBase::get_last_cast_ms() const { return last_cast_ms_; }

int RayTracerBase::get_triangle_count() const { return dispatcher_.triangle_count(); }
int RayTracerBase::get_bvh_node_count() const { return dispatcher_.bvh_node_count(); }
int RayTracerBase::get_bvh_depth() const { return dispatcher_.bvh_depth(); }
int RayTracerBase::get_thread_count() const { return static_cast<int>(dispatcher_.thread_count()); }
