// raytracer_probe.cpp -- RayTracerProbe implementation.

#include "raytracer_probe.h"
#include "raytracer_server.h"
#include "core/asserts.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

// ============================================================================
// Godot binding
// ============================================================================

void RayTracerProbe::_bind_methods() {
	// Scene registration
	ClassDB::bind_method(D_METHOD("register_children"), &RayTracerProbe::register_children);
	ClassDB::bind_method(D_METHOD("register_children_recursive"), &RayTracerProbe::register_children_recursive);
	ClassDB::bind_method(D_METHOD("unregister_children"), &RayTracerProbe::unregister_children);

	// Ray casting
	ClassDB::bind_method(D_METHOD("cast_ray", "direction"), &RayTracerProbe::cast_ray);
	ClassDB::bind_method(D_METHOD("any_hit", "direction", "max_distance"), &RayTracerProbe::any_hit);

	// Properties
	ClassDB::bind_method(D_METHOD("set_auto_register", "enabled"), &RayTracerProbe::set_auto_register);
	ClassDB::bind_method(D_METHOD("get_auto_register"), &RayTracerProbe::get_auto_register);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_register"),
		"set_auto_register", "get_auto_register");

	ClassDB::bind_method(D_METHOD("set_layer_mask", "mask"), &RayTracerProbe::set_layer_mask);
	ClassDB::bind_method(D_METHOD("get_layer_mask"), &RayTracerProbe::get_layer_mask);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "layer_mask", PROPERTY_HINT_LAYERS_3D_RENDER),
		"set_layer_mask", "get_layer_mask");

	ClassDB::bind_method(D_METHOD("get_registered_count"), &RayTracerProbe::get_registered_count);
}

// ============================================================================
// Lifecycle
// ============================================================================

void RayTracerProbe::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY:
			RT_ASSERT(is_inside_tree(), "_notification(READY): expected to be inside scene tree");
			if (auto_register_) {
				register_children();
			}
			break;

		case NOTIFICATION_EXIT_TREE:
			RT_ASSERT(is_inside_tree(), "_notification(EXIT_TREE): expected to be inside scene tree");
			unregister_children();
			break;

		default:
			break;
	}
}

// ============================================================================
// Scene registration
// ============================================================================

void RayTracerProbe::register_children() {
	RT_ASSERT(is_inside_tree(), "register_children: must be inside scene tree");

	RayTracerServer *server = RayTracerServer::get_singleton();
	if (!server) {
		UtilityFunctions::print("[RayTracerProbe] Server not available");
		return;
	}
	RT_ASSERT_NOT_NULL(server);

	for (int i = 0; i < get_child_count(); i++) {
		MeshInstance3D *mesh = Object::cast_to<MeshInstance3D>(get_child(i));
		if (mesh) {
			int id = server->register_mesh(mesh);
			if (id >= 0) {
				registered_ids_.push_back(id);
			}
		}
	}
}

static void register_recursive_helper(Node *node, RayTracerServer *server,
		std::vector<int> &ids) {
	MeshInstance3D *mesh = Object::cast_to<MeshInstance3D>(node);
	if (mesh) {
		int id = server->register_mesh(mesh);
		if (id >= 0) {
			ids.push_back(id);
		}
	}
	for (int i = 0; i < node->get_child_count(); i++) {
		register_recursive_helper(node->get_child(i), server, ids);
	}
}

void RayTracerProbe::register_children_recursive() {
	RayTracerServer *server = RayTracerServer::get_singleton();
	if (!server) {
		UtilityFunctions::print("[RayTracerProbe] Server not available");
		return;
	}

	for (int i = 0; i < get_child_count(); i++) {
		register_recursive_helper(get_child(i), server, registered_ids_);
	}
}

void RayTracerProbe::unregister_children() {
	RayTracerServer *server = RayTracerServer::get_singleton();
	if (!server) { return; }

	for (int id : registered_ids_) {
		server->unregister_mesh(id);
	}
	registered_ids_.clear();
}

// ============================================================================
// Ray casting
// ============================================================================

Dictionary RayTracerProbe::cast_ray(const Vector3 &direction) {
	RayTracerServer *server = RayTracerServer::get_singleton();
	if (!server) {
		Dictionary empty;
		empty["hit"] = false;
		return empty;
	}
	return server->cast_ray(get_global_position(), direction, layer_mask_);
}

bool RayTracerProbe::any_hit(const Vector3 &direction, float max_distance) {
	RayTracerServer *server = RayTracerServer::get_singleton();
	if (!server) { return false; }
	return server->any_hit(get_global_position(), direction, max_distance, layer_mask_);
}

// ============================================================================
// Properties
// ============================================================================

void RayTracerProbe::set_auto_register(bool enabled) { auto_register_ = enabled; }
bool RayTracerProbe::get_auto_register() const { return auto_register_; }

void RayTracerProbe::set_layer_mask(int mask) { layer_mask_ = mask; }
int RayTracerProbe::get_layer_mask() const { return layer_mask_; }

int RayTracerProbe::get_registered_count() const {
	return static_cast<int>(registered_ids_.size());
}
