// raytracer_debug.cpp — Debug visualization for ray tracing.
//
// Implements all 7 draw modes, lifecycle management (ENTER/EXIT_TREE),
// visibility-aware computation skip, and BVH wireframe traversal.

#include "raytracer_debug.h"
#include "raytracer_server.h"
#include "core/asserts.h"

// TinyBVH headers needed for BVH wireframe visualization.
#ifndef TINYBVH_INST_IDX_BITS
#define TINYBVH_INST_IDX_BITS 32
#endif
#include "thirdparty/tinybvh/tiny_bvh.h"

#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <chrono>
#include <cmath>
#include <queue>
#include <vector>

using namespace godot;

// ============================================================================
// Godot binding
// ============================================================================

void RayTracerDebug::_bind_methods() {
	// Enum constants
	BIND_ENUM_CONSTANT(DRAW_RAYS);
	BIND_ENUM_CONSTANT(DRAW_NORMALS);
	BIND_ENUM_CONSTANT(DRAW_DISTANCE);
	BIND_ENUM_CONSTANT(DRAW_HEATMAP);
	BIND_ENUM_CONSTANT(DRAW_OVERHEAT);
	BIND_ENUM_CONSTANT(DRAW_BVH);
	BIND_ENUM_CONSTANT(DRAW_LAYERS);

	// Methods
	ClassDB::bind_method(D_METHOD("cast_debug_rays", "origin", "forward",
		"grid_w", "grid_h", "fov_degrees"), &RayTracerDebug::cast_debug_rays);
	ClassDB::bind_method(D_METHOD("clear_debug"), &RayTracerDebug::clear_debug);

	// Properties
	ADD_GROUP("Debug", "debug_");

	ClassDB::bind_method(D_METHOD("set_debug_enabled", "enabled"), &RayTracerDebug::set_debug_enabled);
	ClassDB::bind_method(D_METHOD("get_debug_enabled"), &RayTracerDebug::get_debug_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug_enabled"),
		"set_debug_enabled", "get_debug_enabled");

	ClassDB::bind_method(D_METHOD("set_draw_mode", "mode"), &RayTracerDebug::set_draw_mode);
	ClassDB::bind_method(D_METHOD("get_draw_mode"), &RayTracerDebug::get_draw_mode);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "debug_draw_mode", PROPERTY_HINT_ENUM,
		"Rays,Normals,Distance,Heatmap,Overheat,BVH,Layers"),
		"set_draw_mode", "get_draw_mode");

	ClassDB::bind_method(D_METHOD("set_ray_miss_length", "length"), &RayTracerDebug::set_ray_miss_length);
	ClassDB::bind_method(D_METHOD("get_ray_miss_length"), &RayTracerDebug::get_ray_miss_length);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "debug_ray_miss_length", PROPERTY_HINT_RANGE, "0.1,200.0,0.1"),
		"set_ray_miss_length", "get_ray_miss_length");

	ClassDB::bind_method(D_METHOD("set_normal_length", "length"), &RayTracerDebug::set_normal_length);
	ClassDB::bind_method(D_METHOD("get_normal_length"), &RayTracerDebug::get_normal_length);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "debug_normal_length", PROPERTY_HINT_RANGE, "0.01,5.0,0.01"),
		"set_normal_length", "get_normal_length");

	ClassDB::bind_method(D_METHOD("set_heatmap_max_distance", "dist"), &RayTracerDebug::set_heatmap_max_distance);
	ClassDB::bind_method(D_METHOD("get_heatmap_max_distance"), &RayTracerDebug::get_heatmap_max_distance);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "debug_heatmap_max_distance", PROPERTY_HINT_RANGE, "1.0,500.0,1.0"),
		"set_heatmap_max_distance", "get_heatmap_max_distance");

	ClassDB::bind_method(D_METHOD("set_heatmap_max_cost", "cost"), &RayTracerDebug::set_heatmap_max_cost);
	ClassDB::bind_method(D_METHOD("get_heatmap_max_cost"), &RayTracerDebug::get_heatmap_max_cost);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "debug_heatmap_max_cost", PROPERTY_HINT_RANGE, "1,500,1"),
		"set_heatmap_max_cost", "get_heatmap_max_cost");

	ClassDB::bind_method(D_METHOD("set_bvh_depth", "depth"), &RayTracerDebug::set_bvh_depth);
	ClassDB::bind_method(D_METHOD("get_bvh_depth"), &RayTracerDebug::get_bvh_depth);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "debug_bvh_depth", PROPERTY_HINT_RANGE, "-1,32,1"),
		"set_bvh_depth", "get_bvh_depth");

	ClassDB::bind_method(D_METHOD("set_layer_mask", "mask"), &RayTracerDebug::set_layer_mask);
	ClassDB::bind_method(D_METHOD("get_layer_mask"), &RayTracerDebug::get_layer_mask);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "debug_layer_mask", PROPERTY_HINT_LAYERS_3D_RENDER),
		"set_layer_mask", "get_layer_mask");

	// Stats
	ClassDB::bind_method(D_METHOD("get_last_stats"), &RayTracerDebug::get_last_stats);
	ClassDB::bind_method(D_METHOD("get_last_cast_ms"), &RayTracerDebug::get_last_cast_ms);
}

// ============================================================================
// Lifecycle — ENTER_TREE / EXIT_TREE / VISIBILITY_CHANGED
// ============================================================================

void RayTracerDebug::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE:
			RT_ASSERT(is_inside_tree(), "_notification(ENTER_TREE): must be inside tree");
			_create_debug_objects();
			break;

		case NOTIFICATION_EXIT_TREE:
			RT_ASSERT(is_inside_tree(), "_notification(EXIT_TREE): must be inside tree");
			_destroy_debug_objects();
			break;

		case NOTIFICATION_VISIBILITY_CHANGED:
			// Clear drawn geometry when hidden to avoid stale visuals.
			if (!is_visible_in_tree()) {
				clear_debug();
			}
			break;

		default:
			break;
	}
}

// ============================================================================
// Debug object lifecycle
// ============================================================================

void RayTracerDebug::_create_debug_objects() {
	RT_ASSERT(is_inside_tree(), "_create_debug_objects: must be inside tree");
	RT_ASSERT(mesh_instance_ == nullptr, "_create_debug_objects: mesh_instance_ must be null on enter");

	mesh_instance_ = memnew(MeshInstance3D);
	// Top-level so the debug lines draw in world space, not relative to parent.
	mesh_instance_->set_as_top_level(true);
	add_child(mesh_instance_);

	mesh_.instantiate();
	mesh_instance_->set_mesh(mesh_);

	material_.instantiate();
	material_->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
	material_->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
	material_->set_flag(BaseMaterial3D::FLAG_DISABLE_DEPTH_TEST, true);
	material_->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);

	RT_ASSERT(mesh_instance_ != nullptr, "_create_debug_objects: mesh_instance_ must be valid");
	RT_ASSERT(mesh_.is_valid(), "_create_debug_objects: mesh must be valid");
	RT_ASSERT(material_.is_valid(), "_create_debug_objects: material must be valid");
}

void RayTracerDebug::_destroy_debug_objects() {
	// Precondition: if mesh_instance_ is set, it must still be our child.
	RT_ASSERT(mesh_instance_ == nullptr || mesh_instance_->get_parent() == this,
		"_destroy_debug_objects: mesh_instance_ parent mismatch");

	if (mesh_instance_ != nullptr) {
		// Remove the child and free it so no dangling pointer persists.
		remove_child(mesh_instance_);
		memdelete(mesh_instance_);
		mesh_instance_ = nullptr;
	}

	// Release Ref<> resources.  They are reference-counted and may still be
	// alive if something else holds a ref, but we are done with them.
	mesh_.unref();
	material_.unref();

	RT_ASSERT(mesh_instance_ == nullptr, "_destroy_debug_objects: pointer must be null");
}

// ============================================================================
// Individual draw mode implementations
// ============================================================================

// Mode 0: DRAW_RAYS — classic green/red/yellow/cyan
void RayTracerDebug::_draw_ray_classic(const Ray &r, const Intersection &hit) {
	RT_ASSERT(mesh_.is_valid(), "_draw_ray_classic: mesh must be valid");
	RT_ASSERT_POSITIVE(ray_miss_length_);

	if (hit.hit()) {
		// Green line: origin → hit
		mesh_->surface_set_color(Color(0.0, 1.0, 0.0, 0.8));
		mesh_->surface_add_vertex(r.origin);
		mesh_->surface_set_color(Color(0.0, 1.0, 0.0, 0.4));
		mesh_->surface_add_vertex(hit.position);

		// Yellow cross at hit point
		float s = hit_marker_size_;
		Color yellow(1.0, 1.0, 0.0, 0.9);
		mesh_->surface_set_color(yellow);
		mesh_->surface_add_vertex(hit.position - Vector3(s, 0, 0));
		mesh_->surface_set_color(yellow);
		mesh_->surface_add_vertex(hit.position + Vector3(s, 0, 0));
		mesh_->surface_set_color(yellow);
		mesh_->surface_add_vertex(hit.position - Vector3(0, s, 0));
		mesh_->surface_set_color(yellow);
		mesh_->surface_add_vertex(hit.position + Vector3(0, s, 0));
		mesh_->surface_set_color(yellow);
		mesh_->surface_add_vertex(hit.position - Vector3(0, 0, s));
		mesh_->surface_set_color(yellow);
		mesh_->surface_add_vertex(hit.position + Vector3(0, 0, s));

		// Cyan normal arrow
		Color cyan(0.0, 1.0, 1.0, 0.9);
		mesh_->surface_set_color(cyan);
		mesh_->surface_add_vertex(hit.position);
		mesh_->surface_set_color(cyan);
		mesh_->surface_add_vertex(hit.position + hit.normal * normal_length_);
	} else {
		// Red line: miss
		mesh_->surface_set_color(Color(1.0, 0.0, 0.0, 0.3));
		mesh_->surface_add_vertex(r.origin);
		mesh_->surface_set_color(Color(1.0, 0.0, 0.0, 0.1));
		mesh_->surface_add_vertex(r.origin + r.direction * ray_miss_length_);
	}
}

// Mode 1: DRAW_NORMALS — color rays by surface normal (RGB = XYZ mapped 0..1)
void RayTracerDebug::_draw_ray_normals(const Ray &r, const Intersection &hit) {
	RT_ASSERT(mesh_.is_valid(), "_draw_ray_normals: mesh must be valid");
	RT_ASSERT_POSITIVE(ray_miss_length_);

	if (hit.hit()) {
		Color nc(
			hit.normal.x * 0.5f + 0.5f,
			hit.normal.y * 0.5f + 0.5f,
			hit.normal.z * 0.5f + 0.5f,
			0.8f
		);
		mesh_->surface_set_color(Color(0.5, 0.5, 0.5, 0.3));
		mesh_->surface_add_vertex(r.origin);
		mesh_->surface_set_color(nc);
		mesh_->surface_add_vertex(hit.position);

		mesh_->surface_set_color(nc);
		mesh_->surface_add_vertex(hit.position);
		mesh_->surface_set_color(nc);
		mesh_->surface_add_vertex(hit.position + hit.normal * normal_length_);
	} else {
		mesh_->surface_set_color(Color(0.2, 0.2, 0.2, 0.1));
		mesh_->surface_add_vertex(r.origin);
		mesh_->surface_set_color(Color(0.2, 0.2, 0.2, 0.05));
		mesh_->surface_add_vertex(r.origin + r.direction * ray_miss_length_);
	}
}

// Mode 2: DRAW_DISTANCE — distance heatmap (close=white, far=red, very far=dark)
void RayTracerDebug::_draw_ray_distance(const Ray &r, const Intersection &hit) {
	RT_ASSERT(mesh_.is_valid(), "_draw_ray_distance: mesh must be valid");
	RT_ASSERT_POSITIVE(heatmap_max_distance_);

	if (hit.hit()) {
		float t_norm = hit.t / heatmap_max_distance_;
		if (t_norm > 1.0f) { t_norm = 1.0f; }

		// Three-band gradient: white → yellow → red → dark red.
		Color c;
		if (t_norm < 0.33f) {
			float f = t_norm / 0.33f;
			c = Color(1.0, 1.0, 1.0 - f, 0.8);
		} else if (t_norm < 0.66f) {
			float f = (t_norm - 0.33f) / 0.33f;
			c = Color(1.0, 1.0 - f, 0.0, 0.8);
		} else {
			float f = (t_norm - 0.66f) / 0.34f;
			c = Color(1.0 - 0.5f * f, 0.0, 0.0, 0.8);
		}

		mesh_->surface_set_color(Color(0.5, 0.5, 0.5, 0.2));
		mesh_->surface_add_vertex(r.origin);
		mesh_->surface_set_color(c);
		mesh_->surface_add_vertex(hit.position);
	} else {
		mesh_->surface_set_color(Color(0.0, 0.0, 0.0, 0.1));
		mesh_->surface_add_vertex(r.origin);
		mesh_->surface_set_color(Color(0.0, 0.0, 0.0, 0.05));
		mesh_->surface_add_vertex(r.origin + r.direction * ray_miss_length_);
	}
}

// Mode 3 & 4: DRAW_HEATMAP / DRAW_OVERHEAT — color by triangle test count
void RayTracerDebug::_draw_ray_heatmap(const Ray &r, const Intersection &hit,
		int tri_test_count) {
	RT_ASSERT(mesh_.is_valid(), "_draw_ray_heatmap: mesh must be valid");
	RT_ASSERT_POSITIVE(heatmap_max_cost_);

	// Four-band gradient (blue → cyan → green → yellow → red).
	float cost = static_cast<float>(tri_test_count) / static_cast<float>(heatmap_max_cost_);
	if (cost > 1.0f) { cost = 1.0f; }

	Color c;
	if (cost < 0.25f) {
		float f = cost / 0.25f;
		c = Color(0.0, f, 1.0, 0.8);
	} else if (cost < 0.5f) {
		float f = (cost - 0.25f) / 0.25f;
		c = Color(0.0, 1.0, 1.0 - f, 0.8);
	} else if (cost < 0.75f) {
		float f = (cost - 0.5f) / 0.25f;
		c = Color(f, 1.0, 0.0, 0.8);
	} else {
		float f = (cost - 0.75f) / 0.25f;
		c = Color(1.0, 1.0 - f, 0.0, 0.8);
	}

	// Overheat: magenta highlight for rays that exceed the cost threshold.
	if (draw_mode_ == DRAW_OVERHEAT && tri_test_count > heatmap_max_cost_) {
		c = Color(1.0, 0.0, 1.0, 1.0);
	}

	if (hit.hit()) {
		mesh_->surface_set_color(Color(c.r, c.g, c.b, 0.2));
		mesh_->surface_add_vertex(r.origin);
		mesh_->surface_set_color(c);
		mesh_->surface_add_vertex(hit.position);
	} else {
		mesh_->surface_set_color(Color(c.r, c.g, c.b, 0.15));
		mesh_->surface_add_vertex(r.origin);
		mesh_->surface_set_color(Color(c.r, c.g, c.b, 0.05));
		mesh_->surface_add_vertex(r.origin + r.direction * ray_miss_length_);
	}
}

// Mode 6: DRAW_LAYERS — color rays by the visibility layer of the hit triangle.
// Each of the 20 Godot render layers gets a distinct hue so you can visually
// verify which geometry is assigned to which layer.
void RayTracerDebug::_draw_ray_layers(const Ray &r, const Intersection &hit) {
	RT_ASSERT(mesh_.is_valid(), "_draw_ray_layers: mesh must be valid");
	RT_ASSERT_POSITIVE(hit_marker_size_);

	if (hit.hit()) {
		// Find the lowest set bit to pick a deterministic color.
		uint32_t mask = hit.hit_layers;
		int layer_idx = 0;
		if (mask != 0) {
			while ((mask & 1u) == 0) {
				mask >>= 1;
				layer_idx++;
			}
		}
		// Map layer index to a unique hue (20 layers spread evenly across the wheel).
		float hue = std::fmod(layer_idx * 0.05f, 1.0f);
		Color c = Color::from_hsv(hue, 0.85f, 1.0f, 0.85f);

		mesh_->surface_set_color(Color(c.r, c.g, c.b, 0.2));
		mesh_->surface_add_vertex(r.origin);
		mesh_->surface_set_color(c);
		mesh_->surface_add_vertex(hit.position);

		// Small cross at hit point in the same color.
		float s = hit_marker_size_;
		mesh_->surface_set_color(c);
		mesh_->surface_add_vertex(hit.position - Vector3(s, 0, 0));
		mesh_->surface_set_color(c);
		mesh_->surface_add_vertex(hit.position + Vector3(s, 0, 0));
		mesh_->surface_set_color(c);
		mesh_->surface_add_vertex(hit.position - Vector3(0, s, 0));
		mesh_->surface_set_color(c);
		mesh_->surface_add_vertex(hit.position + Vector3(0, s, 0));
	} else {
		mesh_->surface_set_color(Color(0.3, 0.3, 0.3, 0.1));
		mesh_->surface_add_vertex(r.origin);
		mesh_->surface_set_color(Color(0.3, 0.3, 0.3, 0.03));
		mesh_->surface_add_vertex(r.origin + r.direction * ray_miss_length_);
	}
}

// ============================================================================
// Draw dispatch — routes to the correct mode handler
// ============================================================================

void RayTracerDebug::_draw_debug_ray(const Ray &r, const Intersection &hit, int ray_index) {
	RT_ASSERT(mesh_.is_valid(), "_draw_debug_ray: mesh must be valid");
	RT_ASSERT(draw_mode_ >= DRAW_RAYS && draw_mode_ <= DRAW_LAYERS,
		"_draw_debug_ray: invalid draw mode");

	switch (draw_mode_) {
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
			int cost = (ray_index >= 0 && ray_index < static_cast<int>(per_ray_tri_tests_.size()))
				? per_ray_tri_tests_[ray_index] : 0;
			_draw_ray_heatmap(r, hit, cost);
			break;
		}

		case DRAW_BVH:
			// In BVH mode, still draw faint rays to show coverage.
			if (hit.hit()) {
				mesh_->surface_set_color(Color(1.0, 1.0, 1.0, 0.15));
				mesh_->surface_add_vertex(r.origin);
				mesh_->surface_set_color(Color(1.0, 1.0, 1.0, 0.05));
				mesh_->surface_add_vertex(hit.position);
			}
			break;

		case DRAW_LAYERS:
			_draw_ray_layers(r, hit);
			break;

		default:
			RT_UNREACHABLE("_draw_debug_ray: unhandled draw mode");
			break;
	}
}

// ============================================================================
// BVH wireframe drawing
// ============================================================================

void RayTracerDebug::_draw_aabb_wireframe(const godot::AABB &box, const Color &color) {
	RT_ASSERT(mesh_.is_valid(), "_draw_aabb_wireframe: mesh must be valid");
	RT_ASSERT(box.size.x >= 0 && box.size.y >= 0 && box.size.z >= 0,
		"_draw_aabb_wireframe: AABB must have non-negative size");

	Vector3 p = box.position;
	Vector3 s = box.size;

	// 8 corners of the axis-aligned box.
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

	// 12 edges connecting the corners.
	static const int edges[12][2] = {
		{0,1}, {1,2}, {2,3}, {3,0},   // bottom face
		{4,5}, {5,6}, {6,7}, {7,4},   // top face
		{0,4}, {1,5}, {2,6}, {3,7},   // vertical pillars
	};

	for (int e = 0; e < 12; e++) {
		mesh_->surface_set_color(color);
		mesh_->surface_add_vertex(corners[edges[e][0]]);
		mesh_->surface_set_color(color);
		mesh_->surface_add_vertex(corners[edges[e][1]]);
	}
}

void RayTracerDebug::_draw_bvh_wireframe() {
	RayTracerServer *server = RayTracerServer::get_singleton();
	if (!server) { return; }

	const auto &sc = server->scene();
	if (!sc.use_bvh || !sc.built) {
		UtilityFunctions::print("[RayTracerDebug] BVH not built — nothing to draw");
		return;
	}

	const int32_t node_count = sc.bvh2.NodeCount();
	const auto *nodes = sc.bvh2.bvhNode;
	if (node_count <= 0 || !nodes) { return; }

	RT_ASSERT(node_count > 0, "_draw_bvh_wireframe: BVH nodes must not be empty after guard");
	RT_ASSERT(bvh_depth_ >= -1, "_draw_bvh_wireframe: bvh_depth must be >= -1");

	int target_depth = bvh_depth_;

	// Color-code by depth: each level gets a different hue.
	auto depth_color = [](int depth) -> Color {
		float hue = std::fmod(depth * 0.15f, 1.0f);
		return Color::from_hsv(hue, 0.8f, 1.0f, 0.5f);
	};

	// Convert TinyBVH aabbMin/aabbMax to Godot AABB for wireframe drawing.
	auto node_to_aabb = [](const tinybvh::BVH::BVHNode &n) -> AABB {
		Vector3 bmin(n.aabbMin.x, n.aabbMin.y, n.aabbMin.z);
		Vector3 bmax(n.aabbMax.x, n.aabbMax.y, n.aabbMax.z);
		return AABB(bmin, bmax - bmin);
	};

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

		if (idx >= static_cast<uint32_t>(node_count)) { continue; }
		const auto &node = nodes[idx];

		if (depth > max_observed_depth) { max_observed_depth = depth; }

		// target_depth == -1 means "draw leaf nodes only".
		if (target_depth == -1) {
			if (node.isLeaf()) {
				_draw_aabb_wireframe(node_to_aabb(node), depth_color(depth));
				boxes_drawn++;
			}
		} else if (depth == target_depth) {
			_draw_aabb_wireframe(node_to_aabb(node), depth_color(depth));
			boxes_drawn++;
			continue;   // Don't descend past target depth.
		}

		if (!node.isLeaf()) {
			uint32_t left = node.leftFirst;
			uint32_t right = node.leftFirst + 1;
			if (target_depth == -1 || depth < target_depth) {
				queue.push({ left, depth + 1 });
				queue.push({ right, depth + 1 });
			}
		}
	}

	UtilityFunctions::print("[RayTracerDebug] BVH wireframe: drew ", boxes_drawn,
		" boxes (depth=", target_depth == -1 ? "leaves" : String::num_int64(target_depth),
		", max_depth=", max_observed_depth, ")");
}

// ============================================================================
// cast_debug_rays — the main entry point
// ============================================================================

void RayTracerDebug::cast_debug_rays(const Vector3 &origin, const Vector3 &forward,
		int grid_w, int grid_h, float fov_degrees) {
	RT_ASSERT(grid_w > 0 && grid_h > 0, "Debug ray grid dimensions must be positive");
	RT_ASSERT(fov_degrees > 0.0f && fov_degrees < 180.0f, "FOV must be in (0, 180) degrees");

	// ---- Early-out guards ----
	// Skip computation when disabled, not in tree, or not visible.
	// This directly fixes the "debug renderer calculated when not visible" bug.
	if (!debug_enabled_) { return; }
	if (!is_inside_tree()) { return; }
	if (!is_visible_in_tree()) { return; }

	RayTracerServer *server = RayTracerServer::get_singleton();
	if (!server) {
		WARN_PRINT_ONCE("[RayTracerDebug] RayTracerServer not available");
		return;
	}

	// Ensure drawing objects exist (they should already from ENTER_TREE,
	// but guard against calls before the node has entered the tree).
	if (mesh_instance_ == nullptr || mesh_.is_null() || material_.is_null()) {
		WARN_PRINT_ONCE("[RayTracerDebug] Debug objects not ready — is node in the tree?");
		return;
	}

	mesh_->clear_surfaces();
	mesh_->surface_begin(Mesh::PRIMITIVE_LINES, material_);

	// BVH wireframe mode — draw boxes before rays.
	if (draw_mode_ == DRAW_BVH) {
		_draw_bvh_wireframe();
	}

	// ---- Compute camera basis ----
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
	last_stats_.reset();

	bool need_per_ray_cost = (draw_mode_ == DRAW_HEATMAP || draw_mode_ == DRAW_OVERHEAT);
	bool used_gpu = server->using_gpu();

	auto t0 = std::chrono::high_resolution_clock::now();

	if (need_per_ray_cost && !used_gpu) {
		// CPU per-ray stats: cast individually for per-ray cost tracking.
		per_ray_tri_tests_.resize(total_rays);
		const auto &scene = server->scene();
		uint32_t qmask = static_cast<uint32_t>(layer_mask_);
		for (int i = 0; i < total_rays; i++) {
			RayStats per;
			per.reset();
			results[i] = scene.cast_ray(rays[i], &per, qmask);
			per_ray_tri_tests_[i] = static_cast<int>(per.tri_tests);
			last_stats_ += per;
		}
	} else {
		// Batch dispatch (fast path).
		server->cast_rays_batch(rays.data(), results.data(), total_rays, &last_stats_,
			static_cast<uint32_t>(layer_mask_));

		if (need_per_ray_cost) {
			// GPU doesn't provide per-ray cost — approximate by averaging.
			per_ray_tri_tests_.resize(total_rays);
			int avg = (last_stats_.rays_cast > 0)
				? static_cast<int>(last_stats_.tri_tests / last_stats_.rays_cast) : 0;
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

	mesh_->surface_end();

	// ---- 4. Print performance summary ----
	static const char *MODE_NAMES[] = {
		"Rays", "Normals", "Distance", "Heatmap", "Overheat", "BVH", "Layers"
	};
	RT_ASSERT(static_cast<int>(draw_mode_) < 7, "draw_mode_ out of range for MODE_NAMES");
	const char *mode_name = MODE_NAMES[static_cast<int>(draw_mode_)];

	if (used_gpu) {
		UtilityFunctions::print(
			"[RayTracerDebug] GPU [", mode_name, "]: Cast ", total_rays,
			" rays — ", hit_count, " hits (",
			String::num(total_rays > 0 ? 100.0f * hit_count / total_rays : 0.0f, 1),
			"%) in ", String::num(last_cast_ms_, 3), " ms");
	} else {
		UtilityFunctions::print(
			"[RayTracerDebug] CPU [", mode_name, "]: Cast ", total_rays,
			" rays — ", hit_count, " hits (",
			String::num(last_stats_.hit_rate_percent(), 1),
			"%) | ", String::num(last_stats_.avg_tri_tests_per_ray(), 1),
			" tri/ray, ", String::num(last_stats_.avg_nodes_per_ray(), 1),
			" nodes/ray | ", String::num(last_cast_ms_, 3), " ms");
	}
}

void RayTracerDebug::clear_debug() {
	if (mesh_.is_valid()) {
		mesh_->clear_surfaces();
	}
}

// ============================================================================
// Properties
// ============================================================================

void RayTracerDebug::set_debug_enabled(bool enabled) { debug_enabled_ = enabled; }
bool RayTracerDebug::get_debug_enabled() const { return debug_enabled_; }

void RayTracerDebug::set_draw_mode(int mode) {
	RT_ASSERT(mode >= 0, "set_draw_mode: mode must be >= 0");
	if (mode >= 0 && mode <= static_cast<int>(DRAW_LAYERS)) {
		draw_mode_ = static_cast<DebugDrawMode>(mode);
	}
}
int RayTracerDebug::get_draw_mode() const { return static_cast<int>(draw_mode_); }

void RayTracerDebug::set_ray_miss_length(float length) {
	RT_ASSERT(length > 0.0f, "set_ray_miss_length: length must be positive");
	ray_miss_length_ = length > 0.0f ? length : 0.1f;
}
float RayTracerDebug::get_ray_miss_length() const { return ray_miss_length_; }

void RayTracerDebug::set_normal_length(float length) {
	RT_ASSERT(length > 0.0f, "set_normal_length: length must be positive");
	normal_length_ = length > 0.0f ? length : 0.01f;
}
float RayTracerDebug::get_normal_length() const { return normal_length_; }

void RayTracerDebug::set_heatmap_max_distance(float dist) {
	RT_ASSERT(dist > 0.0f, "set_heatmap_max_distance: distance must be positive");
	heatmap_max_distance_ = dist > 0.0f ? dist : 1.0f;
}
float RayTracerDebug::get_heatmap_max_distance() const { return heatmap_max_distance_; }

void RayTracerDebug::set_heatmap_max_cost(int cost) {
	RT_ASSERT(cost > 0, "set_heatmap_max_cost: cost must be positive");
	heatmap_max_cost_ = cost > 0 ? cost : 1;
}
int RayTracerDebug::get_heatmap_max_cost() const { return heatmap_max_cost_; }

void RayTracerDebug::set_bvh_depth(int depth) {
	RT_ASSERT(depth >= -1, "set_bvh_depth: depth must be >= -1");
	bvh_depth_ = depth;
}
int RayTracerDebug::get_bvh_depth() const { return bvh_depth_; }

void RayTracerDebug::set_layer_mask(int mask) { layer_mask_ = mask; }
int RayTracerDebug::get_layer_mask() const { return layer_mask_; }

// ============================================================================
// Stats
// ============================================================================

Dictionary RayTracerDebug::get_last_stats() const {
	RT_ASSERT(last_stats_.hits <= last_stats_.rays_cast || last_stats_.rays_cast == 0,
		"get_last_stats: hits must not exceed rays_cast");
	RT_ASSERT_FINITE(last_cast_ms_);

	Dictionary d;
	d["rays_cast"] = static_cast<int64_t>(last_stats_.rays_cast);
	d["tri_tests"] = static_cast<int64_t>(last_stats_.tri_tests);
	d["bvh_nodes_visited"] = static_cast<int64_t>(last_stats_.bvh_nodes_visited);
	d["hits"] = static_cast<int64_t>(last_stats_.hits);
	d["avg_tri_tests_per_ray"] = last_stats_.avg_tri_tests_per_ray();
	d["avg_nodes_per_ray"] = last_stats_.avg_nodes_per_ray();
	d["hit_rate_percent"] = last_stats_.hit_rate_percent();
	d["cast_time_ms"] = last_cast_ms_;
	return d;
}

float RayTracerDebug::get_last_cast_ms() const { return last_cast_ms_; }
