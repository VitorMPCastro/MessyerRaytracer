#pragma once
// ray_batch.h — GDScript-friendly batch ray casting wrapper.
//
// RayBatch is a RefCounted resource that lets GDScript users build a list
// of rays, submit them through RayTracerServer, and read results back —
// all without per-ray Dictionary overhead.
//
// USAGE FROM GDSCRIPT:
//   var batch = RayBatch.new()
//   batch.layer_mask = 0x03
//   batch.add_ray(origin, direction)
//   batch.add_ray(origin2, direction2)
//   batch.cast()                          # dispatches through server
//
//   for i in batch.get_hit_count():
//       if batch.is_hit(i):
//           print(batch.get_position(i))
//
// The batch can be reused: call clear() then add_ray() again.

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include "api/ray_query.h"
#include <vector>

using namespace godot;

class RayBatch : public RefCounted {
	GDCLASS(RayBatch, RefCounted)

public:
	// Query mode — mirrors RayQuery::Mode for GDScript.
	enum Mode {
		MODE_NEAREST = 0,
		MODE_ANY_HIT = 1,
	};

private:
	// Input rays
	std::vector<Ray> rays_;

	// Output (nearest)
	std::vector<Intersection> hits_;

	// Output (any-hit) — plain vector<uint8_t> instead of vector<bool>
	// because vector<bool> is bit-packed and has no .data() pointer.
	std::vector<uint8_t> hit_flags_;

	// Settings
	Mode mode_ = MODE_NEAREST;
	int layer_mask_ = 0x7FFFFFFF;
	bool collect_stats_ = false;

	// Result metadata
	float elapsed_ms_ = 0.0f;
	RayStats stats_;

protected:
	static void _bind_methods();

public:
	RayBatch() = default;
	~RayBatch() = default;

	// ---- Building ----

	// Add a ray (direction is normalized internally).
	void add_ray(const Vector3 &origin, const Vector3 &direction);

	// Add a ray with custom t_min / t_max.
	void add_ray_ex(const Vector3 &origin, const Vector3 &direction,
		float t_min, float t_max);

	// Remove all rays and results.
	void clear();

	// Number of rays currently queued.
	int get_ray_count() const;

	// ---- Dispatch ----

	// Cast all queued rays through RayTracerServer.
	// After this call, results are available via get_hit_count(), is_hit(), etc.
	void cast();

	// ---- Results (NEAREST mode) ----

	// Number of results (== ray count after cast).
	int get_hit_count() const;

	// Did ray i hit something?
	bool is_hit(int index) const;

	// World-space hit position.
	Vector3 get_position(int index) const;

	// World-space surface normal at hit.
	Vector3 get_normal(int index) const;

	// Distance along ray to hit point.
	float get_distance(int index) const;

	// Primitive (triangle) ID.
	int get_prim_id(int index) const;

	// Layer bitmask of the hit triangle.
	int get_hit_layers(int index) const;

	// Full result for ray i as a Dictionary (for compatibility with cast_ray).
	Dictionary get_result(int index) const;

	// ---- Results (ANY_HIT mode) ----

	bool get_any_hit(int index) const;

	// ---- Properties ----

	void set_mode(int mode);
	int get_mode() const;

	void set_layer_mask(int mask);
	int get_layer_mask() const;

	void set_collect_stats(bool enabled);
	bool get_collect_stats() const;

	// ---- Stats ----

	float get_elapsed_ms() const;
	Dictionary get_stats() const;
};

VARIANT_ENUM_CAST(RayBatch::Mode);
