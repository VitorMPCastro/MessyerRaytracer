#pragma once
// ray_query.h — Clean module-facing API for submitting ray work.
//
// This is the public interface that modules (AI, audio, graphics) link against.
// Modules NEVER include BVH, RayDispatcher, or GPURayCaster — only this header.
//
// DESIGN:
//   RayQuery is a plain-old-data request: "here are my rays, here's how I want
//   them processed." RayQueryResult is the response.
//
//   Submit queries via RayTracerServer::submit() — the server owns all dispatch
//   infrastructure and routes to the optimal backend transparently.
//
// USAGE (C++ module):
//   #include "api/ray_query.h"
//
//   // Build rays
//   std::vector<Ray> rays = build_boundary_rays(car_transform);
//
//   // Submit
//   RayQuery query;
//   query.rays        = rays.data();
//   query.count       = static_cast<int>(rays.size());
//   query.layer_mask  = 0x01;            // only layer 1
//   query.mode        = RayQuery::NEAREST;
//   query.collect_stats = true;
//
//   RayQueryResult result;
//   result.hits = new Intersection[rays.size()];
//
//   RayTracerServer::get_singleton()->submit(query, result);
//
//   // Use results
//   for (int i = 0; i < result.count; i++) {
//       if (result.hits[i].hit()) { ... }
//   }
//
// OWNERSHIP:
//   The caller owns all arrays (rays, hits, hit_flags).
//   RayQuery/RayQueryResult are stack-allocated value types — no heap, no ref-count.

#include "core/ray.h"
#include "core/intersection.h"
#include "core/stats.h"
#include <cstdint>
#include <chrono>

// ============================================================================
// RayQuery — describes a batch of rays to cast
// ============================================================================

struct RayQuery {
	/// How intersections should be resolved.
	enum Mode {
		NEAREST  = 0,  ///< Find closest intersection per ray (default).
		ANY_HIT  = 1,  ///< Early-exit on first intersection (shadow/occlusion).
	};

	// ---- Required ----
	const Ray *rays = nullptr;   ///< Pointer to ray array (caller-owned).
	int count       = 0;         ///< Number of rays.

	// ---- Filtering ----
	uint32_t layer_mask = 0xFFFFFFFF;  ///< Visibility layer bitmask.

	// ---- Mode ----
	Mode mode = NEAREST;

	// ---- Options ----
	bool collect_stats = false;  ///< Populate result.stats (CPU path only; small cost).

	/// Hint: rays are spatially coherent (e.g. primary camera rays).
	/// When true, the GPU dispatcher skips Morton-code sorting because
	/// adjacent rays already map to nearby directions. This eliminates
	/// the O(N log N) sort and 3 full-array copies that cost ~20-30ms at 1080p.
	bool coherent = false;

	/// Convenience: construct a nearest-hit query.
	static RayQuery nearest(const Ray *rays, int count,
			uint32_t layer_mask = 0xFFFFFFFF) {
		RayQuery q;
		q.rays       = rays;
		q.count      = count;
		q.layer_mask = layer_mask;
		q.mode       = NEAREST;
		return q;
	}

	/// Convenience: construct an any-hit (shadow) query.
	static RayQuery any_hit(const Ray *rays, int count,
			uint32_t layer_mask = 0xFFFFFFFF) {
		RayQuery q;
		q.rays       = rays;
		q.count      = count;
		q.layer_mask = layer_mask;
		q.mode       = ANY_HIT;
		return q;
	}
};

// ============================================================================
// RayQueryResult — output slot filled by the server
// ============================================================================

struct RayQueryResult {
	// ---- Nearest-hit output (mode == NEAREST) ----
	// Caller allocates: result.hits = new Intersection[query.count];
	Intersection *hits = nullptr;

	// ---- Any-hit output (mode == ANY_HIT) ----
	// Caller allocates: result.hit_flags = new bool[query.count];
	bool *hit_flags = nullptr;

	// ---- Filled by server ----
	int count         = 0;     ///< Number of results written (== query.count on success).
	RayStats stats;            ///< Populated if query.collect_stats was true.
	float elapsed_ms  = 0.0f;  ///< Wall-clock time for the dispatch.
};
