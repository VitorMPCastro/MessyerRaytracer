// ray_service_bridge.cpp — Bridge from IRayService to RayTracerServer.
//
// This is the ONLY file that includes both api/ray_service.h and
// godot/raytracer_server.h. All module code goes through the abstract
// interface and never sees the server's internals.
//
// GPU scene data conversion (Triangle → GPUTrianglePacked, BVHNode →
// GPUBVHNodePacked) lives here because the bridge IS the coupling point.
// Modules receive pre-packed buffers via GPUSceneUpload and never see
// accel/ or TinyBVH types.

#include "api/ray_service.h"
#include "raytracer_server.h"
#include "accel/ray_scene.h"   // Bridge coupling: RayScene → GPUSceneUpload conversion
#include "core/triangle.h"     // Bridge coupling: Triangle fields for GPU packing

#include <vector>

class RayServiceBridge final : public IRayService {
	RayTracerServer *_s() const { return RayTracerServer::get_singleton(); }

	// ---- GPU scene data cache ----
	// Owned by this bridge.  Rebuilt lazily in get_gpu_scene_data() when the
	// scene's triangle/node count changes (indicates a rebuild occurred).
	// Mutable because caching is transparent to the const interface.
	// Pointers into these vectors are returned via GPUSceneUpload and remain
	// valid until the next build() triggers a re-pack.
	mutable std::vector<GPUTrianglePacked> gpu_tris_cache_;   // Pre-packed GPU triangles.
	mutable std::vector<GPUBVHNodePacked> gpu_nodes_cache_;   // Pre-packed BVH2 nodes.
	mutable uint32_t cached_tri_count_  = 0;  // Triangle count at last cache fill.
	mutable uint32_t cached_node_count_ = 0;  // Node count at last cache fill.

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

	IThreadDispatch *get_thread_dispatch() override {
		return _s() ? &_s()->dispatcher().thread_pool() : nullptr;
	}

	// ======== Async GPU dispatch ========

	void submit_async(const Ray *rays, int count) override {
		_s()->dispatcher().submit_gpu_async(rays, count);
	}

	void collect_nearest(Intersection *results, int count) override {
		_s()->dispatcher().collect_gpu_nearest(results, count);
	}

	void submit_async_any_hit(const Ray *rays, int count) override {
		_s()->dispatcher().submit_gpu_async_any_hit(rays, count);
	}

	void collect_any_hit(bool *hit_results, int count) override {
		_s()->dispatcher().collect_gpu_any_hit(hit_results, count);
	}

	bool has_async_pending() const override {
		return _s()->dispatcher().has_gpu_pending();
	}

	SceneShadeData get_shade_data() const override {
		return _s()->get_scene_shade_data();
	}

	godot::RenderingDevice *get_gpu_device() override {
		if (!_s()) { return nullptr; }
		return _s()->dispatcher().gpu_caster().get_rendering_device();
	}

	GPUSceneBufferRIDs get_gpu_scene_buffer_rids() const override {
		GPUSceneBufferRIDs rids;
		if (!_s()) { return rids; }
		const GPURayCaster &gc = _s()->dispatcher().gpu_caster();
		rids.scene_tris = gc.get_scene_tri_buffer();
		rids.cwbvh_nodes = gc.get_cwbvh_node_buffer();
		rids.cwbvh_tris = gc.get_cwbvh_tri_buffer();
		rids.scene_valid = gc.is_scene_uploaded();
		rids.cwbvh_valid = gc.is_cwbvh_uploaded();
		return rids;
	}

	GPUSceneUpload get_gpu_scene_data() const override {
		GPUSceneUpload upload;
		if (!_s()) { return upload; }

		const RayScene &scene = _s()->scene();
		if (scene.triangles.empty() || !scene.built) { return upload; }

		const uint32_t tri_count = static_cast<uint32_t>(scene.triangles.size());
		const uint32_t node_count = static_cast<uint32_t>(scene.bvh2.NodeCount());

		// Rebuild cache if scene has changed (count mismatch means build() was called).
		if (tri_count != cached_tri_count_ || node_count != cached_node_count_) {
			// ---- Convert triangles to GPU format ----
			gpu_tris_cache_.resize(tri_count);
			for (uint32_t i = 0; i < tri_count; i++) {
				const Triangle &t = scene.triangles[i];
				GPUTrianglePacked &g = gpu_tris_cache_[i];
				g.v0[0] = t.v0.x; g.v0[1] = t.v0.y; g.v0[2] = t.v0.z;
				g.id = t.id;
				g.edge1[0] = t.edge1.x; g.edge1[1] = t.edge1.y; g.edge1[2] = t.edge1.z;
				g.layers = t.layers;
				g.edge2[0] = t.edge2.x; g.edge2[1] = t.edge2.y; g.edge2[2] = t.edge2.z;
				g._pad2 = 0.0f;
				g.normal[0] = t.normal.x; g.normal[1] = t.normal.y; g.normal[2] = t.normal.z;
				g._pad3 = 0.0f;
			}

			// ---- Convert TinyBVH BVH2 nodes to GPU format ----
			// TinyBVH BVH2 node layout (32 bytes):
			//   bvhvec3 aabbMin; uint32_t leftFirst;
			//   bvhvec3 aabbMax; uint32_t triCount;
			// Maps directly to GPUBVHNodePacked (same semantic, same 32 bytes).
			const tinybvh::BVH::BVHNode *nodes = scene.bvh2.bvhNode;
			gpu_nodes_cache_.resize(node_count);
			for (uint32_t i = 0; i < node_count; i++) {
				const tinybvh::BVH::BVHNode &n = nodes[i];
				GPUBVHNodePacked &g = gpu_nodes_cache_[i];
				g.bounds_min[0] = n.aabbMin.x; g.bounds_min[1] = n.aabbMin.y; g.bounds_min[2] = n.aabbMin.z;
				g.left_first = n.leftFirst;
				g.bounds_max[0] = n.aabbMax.x; g.bounds_max[1] = n.aabbMax.y; g.bounds_max[2] = n.aabbMax.z;
				g.count = n.triCount;
			}

			cached_tri_count_ = tri_count;
			cached_node_count_ = node_count;
		}

		upload.triangles = gpu_tris_cache_.data();
		upload.triangle_count = cached_tri_count_;
		upload.bvh_nodes = gpu_nodes_cache_.data();
		upload.bvh_node_count = cached_node_count_;
		upload.valid = true;
		return upload;
	}
};

static RayServiceBridge g_bridge;

IRayService *get_ray_service() {
	return RayTracerServer::get_singleton() ? &g_bridge : nullptr;
}
