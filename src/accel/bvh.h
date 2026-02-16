#pragma once
// bvh.h — Bounding Volume Hierarchy for O(log N) ray tracing.
//
// WHAT IT IS:
//   A binary tree that partitions triangles into spatially-coherent groups.
//   Each node has an AABB (bounding box) that tightly encloses its children.
//   During traversal, if a ray misses a node's AABB, ALL triangles in that
//   subtree are skipped — this is what makes it O(log N) instead of O(N).
//
// BUILD (done once per scene):
//   Top-down recursive. At each node, we use the Surface Area Heuristic (SAH)
//   to find the optimal split. SAH estimates the cost of each possible split
//   by considering how likely a random ray is to hit each child's AABB.
//
//   "Binned" SAH: instead of testing every possible split position (O(N²)),
//   we divide each axis into 12 bins and evaluate splits between bins: O(N).
//
// TRAVERSE (done per ray):
//   Iterative stack-based with front-to-back ordering:
//   1. Pop node from stack
//   2. If leaf: test all triangles, update closest hit
//   3. If internal: test children's AABBs, push hits (near child on top)
//
//   Stored tmin per stack entry enables cheap early termination:
//   if the nearest possible hit in a subtree is farther than the current
//   closest hit, skip the entire subtree without re-testing the AABB.

#include "core/ray.h"
#include "core/intersection.h"
#include "core/triangle.h"
#include "simd/ray_packet.h"
#include "core/aabb_intersect.h"
#include "core/stats.h"
#include "core/asserts.h"

#include <vector>
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <godot_cpp/variant/aabb.hpp>

// A single node in the BVH tree.
// Memory layout: 24 bytes (AABB) + 4 + 4 = 32 bytes per node.
//
// Encoding:
//   count == 0: INTERNAL node. Left child = this_node + 1 (implicit DFS order).
//               left_first = right child index.
//   count >  0: LEAF node. left_first = index of first triangle. count = # triangles.
struct BVHNode {
	godot::AABB bounds;
	uint32_t left_first = 0;
	uint32_t count = 0;
	uint32_t subtree_layer_mask = 0xFFFFFFFF; // Union of all descendant triangle layer masks

	bool is_leaf() const { return count > 0; }
};

class BVH {
public:
	// Max triangles per leaf node.
	// Smaller = deeper tree (more AABB tests, fewer tri tests per leaf).
	// Larger = shallower tree (fewer AABB tests, more tri tests per leaf).
	// 4 is the standard sweet spot for CPU raytracers.
	static constexpr uint32_t MAX_LEAF_SIZE = 4;

	// Number of bins for SAH cost evaluation per axis.
	// 12 is standard (used by pbrt, Embree, Intel OSPRay).
	static constexpr int NUM_BINS = 12;

	// ========================================================================
	// Build
	// ========================================================================

	// Construct BVH from a triangle array.
	// WARNING: REORDERS the triangles vector for leaf contiguity!
	// After build, triangles[leaf.left_first .. +count-1] are that leaf's tris.
	void build(std::vector<Triangle> &triangles) {
		tri_count_ = static_cast<uint32_t>(triangles.size());
		if (tri_count_ == 0) {
			built_ = false;
			return;
		}
		RT_ASSERT(!triangles.empty(), "BVH::build called with empty triangle array after size check");

		tris_ = triangles.data();
		depth_ = 0;

		// Pre-allocate worst-case node count: 2N - 1 for N leaves, +1 for safety.
		nodes_.resize(2 * tri_count_);
		node_count_ = 0;

		// Create root node encompassing all triangles.
		uint32_t root = alloc_node();
		nodes_[root].left_first = 0;
		nodes_[root].count = tri_count_;
		compute_bounds(root);

		// Recursively subdivide using SAH.
		subdivide(root, 0);

		// Reorder nodes into DFS layout for cache-friendly traversal.
		// After this: left child = parent_index + 1 (implicit), left_first = right child index.
		reorder_dfs();

		// Compute per-node subtree layer masks (bottom-up) for layer-based culling.
		compute_subtree_masks(triangles);

		// Trim unused node slots.
		nodes_.resize(node_count_);
		tris_ = nullptr;
		built_ = true;
		RT_ASSERT(built_, "BVH should be built after successful build()");
		RT_ASSERT(node_count_ > 0, "BVH must have at least one node after build");
	}

	// ========================================================================
	// Traversal — cast_ray (nearest hit)
	// ========================================================================

	// Trace a ray through the BVH, returning the closest intersection.
	// Each AABB is tested exactly once. Stored tmin enables O(1) early exit.
	Intersection cast_ray(const Ray &r, const std::vector<Triangle> &triangles,
			RayStats *stats = nullptr, uint32_t query_mask = 0xFFFFFFFF) const {
		Intersection closest;
		if (!built_ || nodes_.empty()) return closest;
		RT_ASSERT_VALID_RAY(r);
		RT_ASSERT(!triangles.empty(), "BVH::cast_ray called with empty triangle array");

		if (stats) stats->rays_cast++;

		// Test root AABB — rays that miss the entire scene exit here.
		float root_tmin, root_tmax;
		if (!ray_intersects_aabb(r, nodes_[0].bounds, root_tmin, root_tmax)) {
			return closest;
		}

		const Triangle *all_tris = triangles.data();

		// Stack stores (node_index, entry_distance) pairs.
		// 64 entries supports trees with up to 2^64 nodes (far beyond any scene).
		struct StackEntry { uint32_t idx; float tmin; };
		StackEntry stack[64];
		int sp = 0;
		stack[sp++] = { 0, root_tmin };

		while (sp > 0) {
			RT_ASSERT(sp <= 64, "BVH traversal stack overflow in cast_ray");
			StackEntry entry = stack[--sp];

			// EARLY EXIT: this subtree's nearest possible hit is farther
			// than what we've already found. Skip without re-testing AABB.
			if (entry.tmin > closest.t) continue;

			if (stats) stats->bvh_nodes_visited++;

			const BVHNode &node = nodes_[entry.idx];

		if (node.is_leaf()) {
				// Test each triangle in the leaf, filtering by layer mask.
				for (uint32_t j = 0; j < node.count; j++) {
					const Triangle &tri = all_tris[node.left_first + j];
					if ((tri.layers & query_mask) == 0) continue;
					if (stats) stats->tri_tests++;
					if (tri.intersect(r, closest)) {
						closest.hit_layers = tri.layers;
					}
				}
			} else {
				// Test both children's AABBs. Push only those the ray enters.
				uint32_t left = entry.idx + 1;      // Implicit: left child is next in DFS order
				uint32_t right = node.left_first;    // Explicit: right child index stored in node

				float tmin_l, tmax_l, tmin_r, tmax_r;
				bool hit_l = (nodes_[left].subtree_layer_mask & query_mask) != 0 &&
				             ray_intersects_aabb(r, nodes_[left].bounds, tmin_l, tmax_l);
				bool hit_r = (nodes_[right].subtree_layer_mask & query_mask) != 0 &&
				             ray_intersects_aabb(r, nodes_[right].bounds, tmin_r, tmax_r);

				// Further filter: skip children that can't beat current closest.
				hit_l = hit_l && (tmin_l <= closest.t);
				hit_r = hit_r && (tmin_r <= closest.t);

				if (hit_l && hit_r) {
					// Push FAR child first so NEAR child is popped first (LIFO).
					// This maximizes early termination: nearby hits are found first,
					// causing far subtrees to be skipped via the tmin > closest.t check.
					if (tmin_l < tmin_r) {
						stack[sp++] = { right, tmin_r };
						stack[sp++] = { left, tmin_l };
					} else {
						stack[sp++] = { left, tmin_l };
						stack[sp++] = { right, tmin_r };
					}
				} else if (hit_l) {
					stack[sp++] = { left, tmin_l };
				} else if (hit_r) {
					stack[sp++] = { right, tmin_r };
				}
			}
		}

		if (stats && closest.hit()) stats->hits++;
		return closest;
	}

	// ========================================================================
	// Traversal — any_hit (shadow/occlusion query)
	// ========================================================================

	// Returns true if the ray hits ANY geometry. Exits on first hit.
	// Used for shadow rays, line-of-sight checks, audio occlusion.
	bool any_hit(const Ray &r, const std::vector<Triangle> &triangles,
			RayStats *stats = nullptr, uint32_t query_mask = 0xFFFFFFFF) const {
		if (!built_ || nodes_.empty()) return false;
		RT_ASSERT_VALID_RAY(r);
		RT_ASSERT(!triangles.empty(), "BVH::any_hit called with empty triangle array");

		if (stats) stats->rays_cast++;

		float root_tmin, root_tmax;
		if (!ray_intersects_aabb(r, nodes_[0].bounds, root_tmin, root_tmax)) {
			return false;
		}

		const Triangle *all_tris = triangles.data();

		uint32_t stack[64];
		int sp = 0;
		stack[sp++] = 0;

		while (sp > 0) {
			RT_ASSERT(sp <= 64, "BVH traversal stack overflow in any_hit");
			uint32_t node_idx = stack[--sp];
			const BVHNode &node = nodes_[node_idx];

			if (stats) stats->bvh_nodes_visited++;

			float tmin, tmax;
			if (!ray_intersects_aabb(r, node.bounds, tmin, tmax)) continue;

			if (node.is_leaf()) {
				// Test each triangle, filtering by layer mask.
				for (uint32_t j = 0; j < node.count; j++) {
					const Triangle &tri = all_tris[node.left_first + j];
					if ((tri.layers & query_mask) == 0) continue;
					if (stats) stats->tri_tests++;
					Intersection temp;
					if (tri.intersect(r, temp)) {
						if (stats) stats->hits++;
						return true; // Early exit!
					}
				}
			} else {
				// No front-to-back ordering needed for any_hit.
				// We just need to find ANY hit as fast as possible.
				// Skip children whose subtree has no matching layers.
				uint32_t right_idx = node.left_first;
				uint32_t left_idx = node_idx + 1;
				if ((nodes_[right_idx].subtree_layer_mask & query_mask) != 0)
					stack[sp++] = right_idx;
				if ((nodes_[left_idx].subtree_layer_mask & query_mask) != 0)
					stack[sp++] = left_idx;
			}
		}

		return false;
	}

	// ========================================================================
	// Traversal — cast_ray_packet4 (4-ray coherent traversal)
	// ========================================================================

	// Trace 4 rays through the BVH simultaneously.
	// Amortizes AABB tests: one SIMD check per node tests all 4 rays.
	// Works best with coherent rays (primary camera rays, audio impulse grids).
	//
	// 'count' is the number of valid rays (1–4). Unused result slots are untouched.
#if RAYTRACER_PACKET_SSE
	void cast_ray_packet4(const Ray *rays, Intersection *results, int count,
			const std::vector<Triangle> &triangles, RayStats *stats = nullptr,
			uint32_t query_mask = 0xFFFFFFFF) const {
		if (!built_ || nodes_.empty() || count <= 0) return;
		RT_ASSERT(count >= 1 && count <= 4, "BVH::cast_ray_packet4: count must be 1-4");
		RT_ASSERT_NOT_NULL(rays);
		RT_ASSERT_NOT_NULL(results);

		if (stats) stats->rays_cast += count;

		const Triangle *all_tris = triangles.data();
		RayPacket4 packet = RayPacket4::build(rays, count);

		// Initialize results' t values so the packet's best_t tracks them.
		// (best_t was already set to t_max in build, which is correct for fresh results)

		// Stack-based traversal, same as single-ray but with packet AABB test.
		uint32_t stack[64];
		int sp = 0;
		stack[sp++] = 0; // root

		while (sp > 0) {
			uint32_t node_idx = stack[--sp];
			const BVHNode &node = nodes_[node_idx];

			// Packet AABB test: returns bitmask of which rays hit this node.
			int hit_mask = packet_intersects_aabb(packet, node.bounds);
			if (hit_mask == 0) continue;

			if (stats) stats->bvh_nodes_visited++;

			if (node.is_leaf()) {
				// Test each active ray against the leaf's triangles, with layer filtering.
				for (int i = 0; i < count; i++) {
					if (!(hit_mask & (1 << i))) continue;
					for (uint32_t j = 0; j < node.count; j++) {
						const Triangle &tri = all_tris[node.left_first + j];
						if ((tri.layers & query_mask) == 0) continue;
						if (stats) stats->tri_tests++;
						if (tri.intersect(rays[i], results[i])) {
							results[i].hit_layers = tri.layers;
							packet.update_best_t(i, results[i].t);
						}
					}
				}
			} else {
				// Push both children. Could sort by average tmin but the
				// single-ray front-to-back order doesn't apply cleanly to packets.
				// Simple near-far heuristic: push right first (popped second).
				uint32_t left = node_idx + 1;
				uint32_t right = node.left_first;
				stack[sp++] = right;
				stack[sp++] = left;
			}
		}

		if (stats) {
			for (int i = 0; i < count; i++) {
				if (results[i].hit()) stats->hits++;
			}
		}
	}
#endif // RAYTRACER_PACKET_SSE

	// ========================================================================
	// Refit — update AABBs after geometry/transforms change (O(N))
	// ========================================================================

	// Recompute all bounding boxes without changing the tree topology.
	// Use this when the underlying triangles have been transformed or deformed.
	//
	// MUCH cheaper than rebuild: O(N) vs O(N log N).
	// The tree structure stays the same — only the AABBs are updated.
	//
	// WHEN TO USE:
	//   - TLAS refit when instances move (world_bounds changed)
	//   - BLAS refit when mesh vertices are deformed (cloth, morphing)
	//   - NOT suitable when topology should change (e.g., objects clustering)
	//
	// ALGORITHM (DFS-ordered BVH):
	//   Walk nodes in REVERSE order. In DFS layout:
	//     - Leaf nodes: recompute bounds from triangles
	//     - Internal nodes: bounds = union of children's bounds
	//   Since children always come after parents in DFS order,
	//   reverse iteration processes children before parents.
	void refit(const std::vector<Triangle> &triangles) {
		if (!built_ || nodes_.empty()) return;
		RT_ASSERT(!triangles.empty(), "BVH::refit called with empty triangle array");

		// Process nodes in reverse DFS order (children before parents).
		for (int i = static_cast<int>(node_count_) - 1; i >= 0; i--) {
			BVHNode &node = nodes_[i];

			if (node.is_leaf()) {
				// Recompute bounds from leaf's triangles.
				if (node.count > 0) {
					node.bounds = triangles[node.left_first].aabb();
					uint32_t mask = 0;
					for (uint32_t j = 0; j < node.count; j++) {
						if (j > 0) {
							node.bounds = node.bounds.merge(
									triangles[node.left_first + j].aabb());
						}
						mask |= triangles[node.left_first + j].layers;
					}
					node.subtree_layer_mask = mask;
				}
			} else {
				// Internal node: union of children's bounds.
				uint32_t left = static_cast<uint32_t>(i) + 1;      // Implicit DFS left child
				uint32_t right = node.left_first;                    // Stored right child
				node.bounds = nodes_[left].bounds.merge(nodes_[right].bounds);
				node.subtree_layer_mask = nodes_[left].subtree_layer_mask |
				                          nodes_[right].subtree_layer_mask;
			}
		}
	}

	// ========================================================================
	// Accessors
	// ========================================================================

	uint32_t get_node_count() const { return node_count_; }
	uint32_t get_depth() const { return depth_; }
	bool is_built() const { return built_; }

	// Access the internal node array (needed by GPURayCaster to upload to GPU).
	const std::vector<BVHNode> &get_nodes() const { return nodes_; }

private:
	std::vector<BVHNode> nodes_;
	uint32_t node_count_ = 0;
	uint32_t tri_count_ = 0;
	uint32_t depth_ = 0;
	Triangle *tris_ = nullptr; // Non-owning pointer, valid only during build()
	bool built_ = false;

	// ---- Node allocation ----

	uint32_t alloc_node() {
		RT_ASSERT(node_count_ < nodes_.size(), "BVH node pool exhausted");
		uint32_t idx = node_count_++;
		nodes_[idx] = BVHNode{};
		return idx;
	}

	// ---- DFS reorder (cache-friendly layout) ----

	// Reorder BVH nodes into depth-first order so that traversal accesses
	// memory sequentially. After reorder, the left child of any internal node
	// is always at index parent + 1 (implicit), and left_first stores the
	// right child index. Leaf nodes keep left_first as the first triangle index.
	//
	// WHY: The original build allocates children as consecutive pairs (left, right)
	// before recursing, so the memory layout interleaves subtrees. After DFS reorder,
	// a left-first traversal (the common case with front-to-back ordering) accesses
	// nodes in sequential memory order, maximizing L1/L2 cache hits.
	void reorder_dfs() {
		if (node_count_ < 3) return; // Need at least root + 2 children

		std::vector<BVHNode> new_nodes(node_count_);
		std::vector<uint32_t> remap(node_count_, UINT32_MAX);

		// Iterative DFS: push right child first so left child is popped first.
		// This produces the exact memory order where left child = parent + 1.
		uint32_t new_idx = 0;
		std::vector<uint32_t> stack;
		stack.reserve(depth_ * 2 + 4);
		stack.push_back(0);

		while (!stack.empty()) {
			uint32_t old_idx = stack.back();
			stack.pop_back();

			remap[old_idx] = new_idx;
			new_nodes[new_idx] = nodes_[old_idx];
			new_idx++;

			if (!nodes_[old_idx].is_leaf()) {
				uint32_t old_left = nodes_[old_idx].left_first;
				uint32_t old_right = old_left + 1;
				stack.push_back(old_right); // Push right first (popped second)
				stack.push_back(old_left);  // Push left second (popped first)
			}
		}

		RT_ASSERT(new_idx == node_count_, "DFS reorder visited wrong number of nodes");

		// Remap internal node child indices.
		// After reorder: left child = parent + 1 (implicit), left_first = right child index.
		for (uint32_t i = 0; i < node_count_; i++) {
			if (!new_nodes[i].is_leaf()) {
				uint32_t old_left = new_nodes[i].left_first;  // Still has old left child index
				uint32_t old_right = old_left + 1;

				RT_ASSERT(remap[old_left] == i + 1,
					"DFS reorder: left child not at parent+1 (implicit invariant broken)");

				new_nodes[i].left_first = remap[old_right];
			}
		}

		nodes_ = std::move(new_nodes);
	}

	// ---- Bounds computation ----

	// Compute subtree layer masks bottom-up after DFS reorder.
	// Each leaf's mask = OR of its triangle layer masks.
	// Each internal node's mask = OR of both children's masks.
	// This lets traversal skip entire subtrees that have no matching layers.
	void compute_subtree_masks(const std::vector<Triangle> &triangles) {
		for (int i = static_cast<int>(node_count_) - 1; i >= 0; i--) {
			BVHNode &node = nodes_[i];
			if (node.is_leaf()) {
				uint32_t mask = 0;
				for (uint32_t j = 0; j < node.count; j++) {
					mask |= triangles[node.left_first + j].layers;
				}
				node.subtree_layer_mask = mask;
			} else {
				uint32_t left = static_cast<uint32_t>(i) + 1;
				uint32_t right = node.left_first;
				node.subtree_layer_mask = nodes_[left].subtree_layer_mask |
				                          nodes_[right].subtree_layer_mask;
			}
		}
	}

	// Compute tight AABB for all triangles belonging to a node.
	void compute_bounds(uint32_t node_idx) {
		BVHNode &node = nodes_[node_idx];
		RT_ASSERT(node.count > 0, "Cannot compute bounds for empty node");

		Vector3 mn = tris_[node.left_first].v0;
		Vector3 mx = mn;

		for (uint32_t i = 0; i < node.count; i++) {
			const Triangle &t = tris_[node.left_first + i];
			mn.x = std::fmin(mn.x, std::fmin(t.v0.x, std::fmin(t.v1.x, t.v2.x)));
			mn.y = std::fmin(mn.y, std::fmin(t.v0.y, std::fmin(t.v1.y, t.v2.y)));
			mn.z = std::fmin(mn.z, std::fmin(t.v0.z, std::fmin(t.v1.z, t.v2.z)));
			mx.x = std::fmax(mx.x, std::fmax(t.v0.x, std::fmax(t.v1.x, t.v2.x)));
			mx.y = std::fmax(mx.y, std::fmax(t.v0.y, std::fmax(t.v1.y, t.v2.y)));
			mx.z = std::fmax(mx.z, std::fmax(t.v0.z, std::fmax(t.v1.z, t.v2.z)));
		}

		// Pad zero-size dimensions slightly to avoid degenerate AABBs
		Vector3 size = mx - mn;
		const float PAD = 1e-5f;
		if (size.x < PAD) { mn.x -= PAD; mx.x += PAD; }
		if (size.y < PAD) { mn.y -= PAD; mx.y += PAD; }
		if (size.z < PAD) { mn.z -= PAD; mx.z += PAD; }

		node.bounds = godot::AABB(mn, mx - mn);
	}

	// Surface area of an AABB. Used by SAH to estimate ray-hit probability.
	// SA(box) / SA(parent) = probability that a random ray entering parent hits box.
	static float surface_area(const godot::AABB &box) {
		Vector3 s = box.size;
		return 2.0f * (s.x * s.y + s.y * s.z + s.z * s.x);
	}

	// ---- SAH split finding ----

	struct SplitResult {
		int axis = -1;       // -1 = no beneficial split found
		float pos = 0.0f;    // Split position along the axis
		float cost = FLT_MAX; // SAH cost of this split
	};

	// Find the best axis and position to split a node's triangles.
	// Uses binned SAH: O(N) per node instead of O(N²).
	SplitResult find_best_split(uint32_t node_idx) {
		const BVHNode &node = nodes_[node_idx];
		SplitResult best;

		// Compute centroid bounds (range of triangle centers).
		// We split based on centroids, not vertex positions, because
		// centroids better represent where triangles "are" in space.
		Vector3 cmin = tris_[node.left_first].centroid();
		Vector3 cmax = cmin;
		for (uint32_t i = 1; i < node.count; i++) {
			Vector3 c = tris_[node.left_first + i].centroid();
			cmin.x = std::fmin(cmin.x, c.x);
			cmin.y = std::fmin(cmin.y, c.y);
			cmin.z = std::fmin(cmin.z, c.z);
			cmax.x = std::fmax(cmax.x, c.x);
			cmax.y = std::fmax(cmax.y, c.y);
			cmax.z = std::fmax(cmax.z, c.z);
		}

		float parent_sa = surface_area(node.bounds);
		if (parent_sa <= 0.0f) return best;
		float inv_parent_sa = 1.0f / parent_sa;

		// Try splitting along each axis (X=0, Y=1, Z=2).
		for (int axis = 0; axis < 3; axis++) {
			float axis_min = (axis == 0) ? cmin.x : (axis == 1) ? cmin.y : cmin.z;
			float axis_max = (axis == 0) ? cmax.x : (axis == 1) ? cmax.y : cmax.z;

			if (axis_max - axis_min < 1e-6f) continue; // Flat along this axis.

			// ---- Assign triangles to bins based on centroid position ----
			struct Bin {
				godot::AABB bounds;
				uint32_t count = 0;
				bool valid = false; // Prevents merging with default AABB at origin
			};
			Bin bins[NUM_BINS];

			float scale = static_cast<float>(NUM_BINS) / (axis_max - axis_min);

			for (uint32_t i = 0; i < node.count; i++) {
				Vector3 c = tris_[node.left_first + i].centroid();
				float cv = (axis == 0) ? c.x : (axis == 1) ? c.y : c.z;
				int bin_idx = static_cast<int>((cv - axis_min) * scale);
				bin_idx = std::max(0, std::min(bin_idx, NUM_BINS - 1));

				godot::AABB tri_box = tris_[node.left_first + i].aabb();
				if (!bins[bin_idx].valid) {
					bins[bin_idx].bounds = tri_box;
					bins[bin_idx].valid = true;
				} else {
					bins[bin_idx].bounds = bins[bin_idx].bounds.merge(tri_box);
				}
				bins[bin_idx].count++;
			}

			// ---- Sweep left-to-right and right-to-left ----
			// Compute cumulative AABB and count for each possible split.
			float left_area[NUM_BINS - 1];
			float right_area[NUM_BINS - 1];
			uint32_t left_count[NUM_BINS - 1];
			uint32_t right_count[NUM_BINS - 1];

			// Left sweep: accumulate bins[0..i]
			{
				godot::AABB running;
				bool init = false;
				uint32_t count = 0;
				for (int i = 0; i < NUM_BINS - 1; i++) {
					if (bins[i].valid) {
						running = init ? running.merge(bins[i].bounds) : bins[i].bounds;
						init = true;
					}
					count += bins[i].count;
					left_area[i] = init ? surface_area(running) : 0.0f;
					left_count[i] = count;
				}
			}

			// Right sweep: accumulate bins[NUM_BINS-1..i+1]
			{
				godot::AABB running;
				bool init = false;
				uint32_t count = 0;
				for (int i = NUM_BINS - 1; i > 0; i--) {
					if (bins[i].valid) {
						running = init ? running.merge(bins[i].bounds) : bins[i].bounds;
						init = true;
					}
					count += bins[i].count;
					right_area[i - 1] = init ? surface_area(running) : 0.0f;
					right_count[i - 1] = count;
				}
			}

			// ---- Evaluate SAH cost for each split position ----
			for (int i = 0; i < NUM_BINS - 1; i++) {
				if (left_count[i] == 0 || right_count[i] == 0) continue;

				// SAH: C = C_trav + (SA_L/SA_P * N_L + SA_R/SA_P * N_R) * C_isect
				// With C_trav = 1.0, C_isect = 1.0:
				float cost = 1.0f + (left_area[i] * left_count[i] +
									  right_area[i] * right_count[i]) * inv_parent_sa;

				if (cost < best.cost) {
					best.cost = cost;
					best.axis = axis;
					best.pos = axis_min + (i + 1.0f) * (axis_max - axis_min) / NUM_BINS;
				}
			}
		}

		return best;
	}

	// ---- Recursive subdivision ----

	void subdivide(uint32_t node_idx, uint32_t depth) {
		BVHNode &node = nodes_[node_idx];

		// Track max depth for diagnostics.
		if (depth > depth_) depth_ = depth;

		// Base case: few enough triangles to make a leaf.
		if (node.count <= MAX_LEAF_SIZE) return;

		SplitResult split = find_best_split(node_idx);

		// If no beneficial split found (SAH says leaf is cheaper), stop.
		float leaf_cost = static_cast<float>(node.count);
		if (split.axis == -1 || split.cost >= leaf_cost) return;

		// ---- Partition triangles (Lomuto partition scheme) ----
		// Triangles with centroid < split.pos go to left child.
		// Triangles with centroid >= split.pos go to right child.
		int axis = split.axis;
		uint32_t first = node.left_first;
		uint32_t last = first + node.count;
		uint32_t mid = first;

		for (uint32_t i = first; i < last; i++) {
			Vector3 c = tris_[i].centroid();
			float cv = (axis == 0) ? c.x : (axis == 1) ? c.y : c.z;
			if (cv < split.pos) {
				std::swap(tris_[i], tris_[mid]);
				mid++;
			}
		}

		// Safety: if partition is degenerate (all on one side), force midpoint.
		if (mid == first || mid == last) {
			mid = first + node.count / 2;
		}

		// ---- Create child nodes (allocated as a consecutive pair) ----
		uint32_t left_idx = alloc_node();
		uint32_t right_idx = alloc_node();
		RT_ASSERT(right_idx == left_idx + 1,
			"BVH children must be consecutive (got non-consecutive allocation)");

		uint32_t left_count = mid - first;
		uint32_t right_count = node.count - left_count;

		nodes_[left_idx].left_first = first;
		nodes_[left_idx].count = left_count;
		compute_bounds(left_idx);

		nodes_[right_idx].left_first = mid;
		nodes_[right_idx].count = right_count;
		compute_bounds(right_idx);

		// Convert this node from leaf to internal.
		node.left_first = left_idx;
		node.count = 0;

		// Recurse into children.
		subdivide(left_idx, depth + 1);
		subdivide(right_idx, depth + 1);
	}
};
