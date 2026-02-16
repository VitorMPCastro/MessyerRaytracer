// ray_batch.cpp — RayBatch implementation.

#include "ray_batch.h"
#include "raytracer_server.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

// ============================================================================
// Godot binding
// ============================================================================

void RayBatch::_bind_methods() {
	// Enum constants
	BIND_ENUM_CONSTANT(MODE_NEAREST);
	BIND_ENUM_CONSTANT(MODE_ANY_HIT);

	// Building
	ClassDB::bind_method(D_METHOD("add_ray", "origin", "direction"), &RayBatch::add_ray);
	ClassDB::bind_method(D_METHOD("add_ray_ex", "origin", "direction", "t_min", "t_max"), &RayBatch::add_ray_ex);
	ClassDB::bind_method(D_METHOD("clear"), &RayBatch::clear);
	ClassDB::bind_method(D_METHOD("get_ray_count"), &RayBatch::get_ray_count);

	// Dispatch
	ClassDB::bind_method(D_METHOD("cast"), &RayBatch::cast);

	// Results (NEAREST)
	ClassDB::bind_method(D_METHOD("get_hit_count"), &RayBatch::get_hit_count);
	ClassDB::bind_method(D_METHOD("is_hit", "index"), &RayBatch::is_hit);
	ClassDB::bind_method(D_METHOD("get_position", "index"), &RayBatch::get_position);
	ClassDB::bind_method(D_METHOD("get_normal", "index"), &RayBatch::get_normal);
	ClassDB::bind_method(D_METHOD("get_distance", "index"), &RayBatch::get_distance);
	ClassDB::bind_method(D_METHOD("get_prim_id", "index"), &RayBatch::get_prim_id);
	ClassDB::bind_method(D_METHOD("get_hit_layers", "index"), &RayBatch::get_hit_layers);
	ClassDB::bind_method(D_METHOD("get_result", "index"), &RayBatch::get_result);

	// Results (ANY_HIT)
	ClassDB::bind_method(D_METHOD("get_any_hit", "index"), &RayBatch::get_any_hit);

	// Properties
	ClassDB::bind_method(D_METHOD("set_mode", "mode"), &RayBatch::set_mode);
	ClassDB::bind_method(D_METHOD("get_mode"), &RayBatch::get_mode);
	ClassDB::bind_method(D_METHOD("set_layer_mask", "mask"), &RayBatch::set_layer_mask);
	ClassDB::bind_method(D_METHOD("get_layer_mask"), &RayBatch::get_layer_mask);
	ClassDB::bind_method(D_METHOD("set_collect_stats", "enabled"), &RayBatch::set_collect_stats);
	ClassDB::bind_method(D_METHOD("get_collect_stats"), &RayBatch::get_collect_stats);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "mode", PROPERTY_HINT_ENUM, "Nearest,AnyHit"),
		"set_mode", "get_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "layer_mask", PROPERTY_HINT_LAYERS_3D_RENDER),
		"set_layer_mask", "get_layer_mask");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "collect_stats"),
		"set_collect_stats", "get_collect_stats");

	// Stats
	ClassDB::bind_method(D_METHOD("get_elapsed_ms"), &RayBatch::get_elapsed_ms);
	ClassDB::bind_method(D_METHOD("get_stats"), &RayBatch::get_stats);
}

// ============================================================================
// Building
// ============================================================================

void RayBatch::add_ray(const Vector3 &origin, const Vector3 &direction) {
	ERR_FAIL_COND_MSG(direction.length_squared() < 1e-12f,
		"RayBatch::add_ray: direction is near-zero");
	rays_.emplace_back(origin, direction.normalized());
}

void RayBatch::add_ray_ex(const Vector3 &origin, const Vector3 &direction,
		float t_min, float t_max) {
	ERR_FAIL_COND_MSG(direction.length_squared() < 1e-12f,
		"RayBatch::add_ray_ex: direction is near-zero");
	ERR_FAIL_COND_MSG(t_min < 0.0f, "RayBatch::add_ray_ex: t_min must be >= 0");
	ERR_FAIL_COND_MSG(t_min > t_max, "RayBatch::add_ray_ex: t_min must be <= t_max");
	rays_.emplace_back(origin, direction.normalized(), t_min, t_max);
}

void RayBatch::clear() {
	rays_.clear();
	hits_.clear();
	hit_flags_.clear();
	elapsed_ms_ = 0.0f;
	stats_.reset();
}

int RayBatch::get_ray_count() const {
	return static_cast<int>(rays_.size());
}

// ============================================================================
// Dispatch
// ============================================================================

void RayBatch::cast() {
	RayTracerServer *server = RayTracerServer::get_singleton();
	ERR_FAIL_NULL_MSG(server, "RayBatch::cast: RayTracerServer singleton is null");
	ERR_FAIL_COND_MSG(rays_.empty(), "RayBatch::cast: no rays added");

	int count = static_cast<int>(rays_.size());

	RayQuery query;
	query.rays          = rays_.data();
	query.count         = count;
	query.layer_mask    = static_cast<uint32_t>(layer_mask_);
	query.collect_stats = collect_stats_;

	RayQueryResult result;

	if (mode_ == MODE_NEAREST) {
		query.mode = RayQuery::NEAREST;
		hits_.resize(count);
		result.hits = hits_.data();
	} else {
		query.mode = RayQuery::ANY_HIT;
		// RayQuery expects bool* but vector<bool> is bit-packed without .data().
		// Use a temporary plain bool array and copy to our uint8_t storage.
		bool *tmp_flags = new bool[count];
		result.hit_flags = tmp_flags;

		server->submit(query, result);

		hit_flags_.resize(count);
		for (int i = 0; i < count; i++) {
			hit_flags_[i] = tmp_flags[i] ? 1 : 0;
		}
		delete[] tmp_flags;

		elapsed_ms_ = result.elapsed_ms;
		stats_ = result.stats;
		return;
	}

	server->submit(query, result);

	elapsed_ms_ = result.elapsed_ms;
	stats_ = result.stats;
}

// ============================================================================
// Results — NEAREST mode
// ============================================================================

int RayBatch::get_hit_count() const {
	return static_cast<int>(hits_.size());
}

bool RayBatch::is_hit(int index) const {
	ERR_FAIL_INDEX_V(index, static_cast<int>(hits_.size()), false);
	return hits_[index].hit();
}

Vector3 RayBatch::get_position(int index) const {
	ERR_FAIL_INDEX_V(index, static_cast<int>(hits_.size()), Vector3());
	return hits_[index].position;
}

Vector3 RayBatch::get_normal(int index) const {
	ERR_FAIL_INDEX_V(index, static_cast<int>(hits_.size()), Vector3());
	return hits_[index].normal;
}

float RayBatch::get_distance(int index) const {
	ERR_FAIL_INDEX_V(index, static_cast<int>(hits_.size()), -1.0f);
	return hits_[index].t;
}

int RayBatch::get_prim_id(int index) const {
	ERR_FAIL_INDEX_V(index, static_cast<int>(hits_.size()), -1);
	return static_cast<int>(hits_[index].prim_id);
}

int RayBatch::get_hit_layers(int index) const {
	ERR_FAIL_INDEX_V(index, static_cast<int>(hits_.size()), 0);
	return static_cast<int>(hits_[index].hit_layers);
}

Dictionary RayBatch::get_result(int index) const {
	ERR_FAIL_INDEX_V(index, static_cast<int>(hits_.size()), Dictionary());
	const Intersection &h = hits_[index];
	Dictionary d;
	d["hit"] = h.hit();
	d["position"] = h.position;
	d["normal"] = h.normal;
	d["distance"] = h.t;
	d["prim_id"] = static_cast<int>(h.prim_id);
	d["hit_layers"] = static_cast<int>(h.hit_layers);
	return d;
}

// ============================================================================
// Results — ANY_HIT mode
// ============================================================================

bool RayBatch::get_any_hit(int index) const {
	ERR_FAIL_INDEX_V(index, static_cast<int>(hit_flags_.size()), false);
	return hit_flags_[index] != 0;
}

// ============================================================================
// Properties
// ============================================================================

void RayBatch::set_mode(int mode) {
	if (mode >= 0 && mode <= static_cast<int>(MODE_ANY_HIT)) {
		mode_ = static_cast<Mode>(mode);
	}
}

int RayBatch::get_mode() const {
	return static_cast<int>(mode_);
}

void RayBatch::set_layer_mask(int mask) {
	layer_mask_ = mask;
}

int RayBatch::get_layer_mask() const {
	return layer_mask_;
}

void RayBatch::set_collect_stats(bool enabled) {
	collect_stats_ = enabled;
}

bool RayBatch::get_collect_stats() const {
	return collect_stats_;
}

// ============================================================================
// Stats
// ============================================================================

float RayBatch::get_elapsed_ms() const {
	return elapsed_ms_;
}

Dictionary RayBatch::get_stats() const {
	Dictionary d;
	d["rays_cast"] = static_cast<int>(stats_.rays_cast);
	d["tri_tests"] = static_cast<int>(stats_.tri_tests);
	d["bvh_nodes_visited"] = static_cast<int>(stats_.bvh_nodes_visited);
	d["hits"] = static_cast<int>(stats_.hits);
	d["avg_tri_tests_per_ray"] = stats_.avg_tri_tests_per_ray();
	d["hit_rate_percent"] = stats_.hit_rate_percent();
	return d;
}
