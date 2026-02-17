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

#include "api/material_data.h"
#include "core/triangle_uv.h"
#include "core/triangle_normals.h"
#include "core/triangle_tangents.h"
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

	/// Per-triangle vertex normals.  Indexed by prim_id.
	/// Interpolate at hit point: tri_normals.interpolate(hit.u, hit.v) → Vector3.
	const TriangleNormals *triangle_normals = nullptr;

	/// Per-triangle vertex tangents.  Indexed by prim_id.
	/// Used with normal maps to build the TBN matrix for tangent→world transform.
	/// May be nullptr if no mesh has tangent data.
	const TriangleTangents *triangle_tangents = nullptr;
};
