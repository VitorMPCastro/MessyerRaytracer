// ray_service_bridge.cpp — Bridge from IRayService to RayTracerServer.
//
// This is the ONLY file that includes both api/ray_service.h and
// godot/raytracer_server.h. All module code goes through the abstract
// interface and never sees the server's internals.

#include "api/ray_service.h"
#include "raytracer_server.h"

class RayServiceBridge final : public IRayService {
	RayTracerServer *_s() const { return RayTracerServer::get_singleton(); }

public:
	// ======== Scene management ========

	int register_mesh(Node *mesh_instance) override {
		return _s()->register_mesh(mesh_instance);
	}

	void unregister_mesh(int mesh_id) override {
		_s()->unregister_mesh(mesh_id);
	}

	void build() override {
		_s()->build();
	}

	void clear() override {
		_s()->clear();
	}

	// ======== Single-ray casting ========

	Dictionary cast_ray(const Vector3 &origin, const Vector3 &direction,
			int layer_mask) override {
		return _s()->cast_ray(origin, direction, layer_mask);
	}

	bool any_hit(const Vector3 &origin, const Vector3 &direction,
			float max_distance, int layer_mask) override {
		return _s()->any_hit(origin, direction, max_distance, layer_mask);
	}

	// ======== Batch submission ========

	void submit(const RayQuery &query, RayQueryResult &result) override {
		_s()->submit(query, result);
	}

	// ======== Backend control ========

	void set_backend(int mode) override {
		_s()->set_backend(mode);
	}

	int get_backend() const override {
		return _s()->get_backend();
	}

	bool is_gpu_available() const override {
		return _s()->is_gpu_available();
	}

	bool using_gpu() const override {
		return _s()->using_gpu();
	}

	// ======== Stats & info ========

	Dictionary get_last_stats() const override {
		return _s()->get_last_stats();
	}

	float get_last_cast_ms() const override {
		return _s()->get_last_cast_ms();
	}

	int get_triangle_count() const override {
		return _s()->get_triangle_count();
	}

	int get_mesh_count() const override {
		return _s()->get_mesh_count();
	}

	int get_bvh_node_count() const override {
		return _s()->get_bvh_node_count();
	}

	int get_bvh_depth() const override {
		return _s()->get_bvh_depth();
	}

	int get_thread_count() const override {
		return _s()->get_thread_count();
	}

	SceneShadeData get_shade_data() const override {
		return _s()->get_scene_shade_data();
	}

	const RayScene *get_scene() const override {
		return _s() ? &_s()->scene() : nullptr;
	}
};

static RayServiceBridge g_bridge;

IRayService *get_ray_service() {
	return RayTracerServer::get_singleton() ? &g_bridge : nullptr;
}
