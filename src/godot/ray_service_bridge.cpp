// ray_service_bridge.cpp — Bridge from IRayService to RayTracerServer.
//
// This is the ONLY file that includes both api/ray_service.h and
// godot/raytracer_server.h. All module code goes through the abstract
// interface and never sees the server's internals.

#include "api/ray_service.h"
#include "raytracer_server.h"

class RayServiceBridge final : public IRayService {
	RayTracerServer *s() const { return RayTracerServer::get_singleton(); }

public:
	// ======== Scene management ========

	int register_mesh(Node *mesh_instance) override {
		return s()->register_mesh(mesh_instance);
	}

	void unregister_mesh(int mesh_id) override {
		s()->unregister_mesh(mesh_id);
	}

	void build() override {
		s()->build();
	}

	void clear() override {
		s()->clear();
	}

	// ======== Single-ray casting ========

	Dictionary cast_ray(const Vector3 &origin, const Vector3 &direction,
			int layer_mask) override {
		return s()->cast_ray(origin, direction, layer_mask);
	}

	bool any_hit(const Vector3 &origin, const Vector3 &direction,
			float max_distance, int layer_mask) override {
		return s()->any_hit(origin, direction, max_distance, layer_mask);
	}

	// ======== Batch submission ========

	void submit(const RayQuery &query, RayQueryResult &result) override {
		s()->submit(query, result);
	}

	// ======== Backend control ========

	void set_backend(int mode) override {
		s()->set_backend(mode);
	}

	int get_backend() const override {
		return s()->get_backend();
	}

	bool is_gpu_available() const override {
		return s()->is_gpu_available();
	}

	bool using_gpu() const override {
		return s()->using_gpu();
	}

	// ======== Stats & info ========

	Dictionary get_last_stats() const override {
		return s()->get_last_stats();
	}

	float get_last_cast_ms() const override {
		return s()->get_last_cast_ms();
	}

	int get_triangle_count() const override {
		return s()->get_triangle_count();
	}

	int get_mesh_count() const override {
		return s()->get_mesh_count();
	}

	int get_bvh_node_count() const override {
		return s()->get_bvh_node_count();
	}

	int get_bvh_depth() const override {
		return s()->get_bvh_depth();
	}

	int get_thread_count() const override {
		return s()->get_thread_count();
	}

	SceneShadeData get_shade_data() const override {
		return s()->get_scene_shade_data();
	}
};

static RayServiceBridge g_bridge;

IRayService *get_ray_service() {
	return RayTracerServer::get_singleton() ? &g_bridge : nullptr;
}
