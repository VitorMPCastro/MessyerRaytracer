#pragma once
// material_data.h — PBR material parameters extracted from Godot materials.
//
// Pulled from BaseMaterial3D at mesh registration time.  Stored per unique
// material (one per surface per mesh).  Indexed at shade time via
// scene_material_ids[prim_id] → materials[mat_id].
//
// Phase 1: albedo color (Lambert shading).
// Phase 2: albedo texture (Ref<Image>), sampled at shade time.
// Phase 5 will use: metallic, roughness, specular for Cook-Torrance.

#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/classes/image.hpp>

using godot::Color;
using godot::Image;
using godot::Ref;

struct MaterialData {
	/// Base color of the surface (no texture applied yet).
	Color albedo = Color(0.75f, 0.75f, 0.75f);

	/// PBR metallic parameter [0..1].
	float metallic = 0.0f;

	/// PBR roughness parameter [0..1].
	float roughness = 0.5f;

	/// Specular reflectance at normal incidence [0..1].
	float specular = 0.5f;

	/// Emission color (additive glow).
	Color emission = Color(0.0f, 0.0f, 0.0f);

	/// Emission energy multiplier.
	float emission_energy = 0.0f;

	// ---- Texture data (Phase 2) ----

	/// Decompressed albedo texture image.  Null if no texture is assigned.
	/// Ref keeps the image alive; may be shared across materials.
	Ref<Image> albedo_texture;

	/// Cached texture dimensions (avoids virtual calls during shading).
	int tex_width = 0;
	int tex_height = 0;

	/// Quick check: true if albedo_texture is valid and ready for sampling.
	bool has_albedo_texture = false;
};
