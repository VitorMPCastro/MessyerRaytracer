#pragma once
// scene_shade_data.h — Lightweight read-only view of scene shading data.
//
// Returned by IRayService::get_shade_data().  Contains pointers to the
// material and (later) UV / light arrays owned by RayTracerServer.
//
// Modules read this struct once per frame; the server populates the
// underlying arrays at build() time.
//
// EXTENDING (future phases):
//   Phase 2: added const TriangleUV* triangle_uvs  ✓
//   Phase 3: add  const SceneLights* lights
//   Phase 4: add  const SkySampler*  sky

#include "core/material_data.h"
#include "core/triangle_uv.h"
#include <cstdint>

struct SceneShadeData {
	/// Flat array of unique materials (one per surface per mesh).
	const MaterialData *materials = nullptr;
	int material_count = 0;

	/// Per-triangle material index.  Indexed by prim_id (global triangle ID).
	/// scene.material_ids[hit.prim_id] → index into materials[].
	const uint32_t *material_ids = nullptr;
	int triangle_count = 0;

	/// Per-triangle UV coordinates.  Indexed by prim_id.
	/// Interpolate at hit point: tri_uv.interpolate(hit.u, hit.v) → Vector2.
	const TriangleUV *triangle_uvs = nullptr;
};
