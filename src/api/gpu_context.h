#pragma once
// gpu_context.h — GPU resource handles for shared RenderingDevice access.
//
// WHAT:  Provides opaque RID handles to GPU-resident scene buffers (CWBVH
//        nodes, CWBVH triangles, scene triangles) and access to the shared
//        local RenderingDevice.
//
// WHY:   GPUPathTracer (in modules/) needs to share the GPU BVH data already
//        uploaded by GPURayCaster (in gpu/), but cannot include gpu/ headers
//        due to module decoupling.  This header defines a struct of RIDs that
//        the bridge populates from GPURayCaster's internals.
//
// HOW:   IRayService::get_gpu_device() returns the local RenderingDevice.
//        IRayService::get_gpu_scene_buffer_rids() returns RID handles.
//        Both are implemented in ray_service_bridge.cpp — the only file that
//        couples api/ to gpu/.
//
// LIFETIME:
//   - RIDs are valid after build() and stable until the next build() or cleanup().
//   - The RenderingDevice pointer is valid for the IRayService's lifetime.
//   - Callers must NOT free these RIDs — they are owned by GPURayCaster.

#include <godot_cpp/variant/rid.hpp>

namespace godot {
class RenderingDevice;
}

/// Opaque handles to GPU-resident BVH and triangle buffers.
///
/// Populated by the bridge from GPURayCaster's internal state.
/// Modules use these RIDs to bind existing scene data into their own
/// descriptor sets — avoiding duplicate BVH uploads.
struct GPUSceneBufferRIDs {
	/// CWBVH compressed wide BVH node buffer (array of vec4, 5 per node = 80B).
	/// Used by CWBVH traversal shaders for 8-wide intersection testing.
	godot::RID cwbvh_nodes;

	/// CWBVH triangle buffer (array of vec4, 3 per tri = 48B).
	/// Pre-ordered for CWBVH leaf traversal (different layout from scene_tris).
	godot::RID cwbvh_tris;

	/// Scene triangle buffer (GPUTrianglePacked, 64B per tri).
	/// Used for layer mask checking and geometric normal lookup after intersection.
	godot::RID scene_tris;

	/// True if CWBVH data has been uploaded to GPU (all three RIDs are valid).
	/// When false, only scene_tris may be valid (Aila-Laine path).
	bool cwbvh_valid = false;

	/// True if at least scene_tris is uploaded (Aila-Laine BVH2 path).
	bool scene_valid = false;
};
