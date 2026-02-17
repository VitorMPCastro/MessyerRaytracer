#pragma once
// ray_scene_setup.h — Node3D that configures Godot's rendering environment.
//
// RaySceneSetup programmatically creates and manages:
//   - WorldEnvironment with Environment resource (sky, GI, tonemapping, post-FX)
//   - DirectionalLight3D (sun) with shadow configuration
//   - Compositor with attached RT CompositorEffects (Phase 5+)
//
// USAGE FROM GDSCRIPT:
//   var setup = $RaySceneSetup
//   setup.apply_preset(RaySceneSetup.PRESET_MEDIUM)
//   setup.sun_energy = 1.5
//   setup.apply()
//
// DESIGN:
//   All child nodes are created lazily on first apply() or _ready().
//   Properties are stored locally and pushed to Godot nodes on apply().
//   Quality presets configure all parameters together for target hardware.

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/world_environment.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/sky.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/camera_attributes_practical.hpp>
#include <godot_cpp/classes/compositor.hpp>
#include <godot_cpp/classes/compositor_effect.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/voxel_gi.hpp>
#include <godot_cpp/classes/voxel_gi_data.hpp>

using namespace godot;

class RaySceneSetup : public Node3D {
	GDCLASS(RaySceneSetup, Node3D)

public:
	// Quality presets — target different hardware tiers.
	enum QualityPreset {
		PRESET_LOW    = 0,  // Integrated GPU / low-end
		PRESET_MEDIUM = 1,  // GTX 1650 Ti class
		PRESET_HIGH   = 2,  // RTX 3060+ class
		PRESET_ULTRA  = 3,  // RTX 4070+ class (enables RT effects)
	};

	// GI mode selection.
	enum GIMode {
		GI_NONE      = 0,
		GI_SDFGI     = 1,
		GI_VOXEL_GI  = 2,
	};

	// Tonemapping mode.
	enum TonemapMode {
		TONEMAP_LINEAR   = 0,
		TONEMAP_REINHARD = 1,
		TONEMAP_FILMIC   = 2,
		TONEMAP_ACES     = 3,
		TONEMAP_AGX      = 4,
	};

private:
	// ---- Managed child nodes ----
	WorldEnvironment *world_env_ = nullptr;
	DirectionalLight3D *sun_ = nullptr;
	Ref<Environment> environment_;
	Ref<Sky> sky_;
	Ref<CameraAttributesPractical> camera_attrs_;
	Ref<Compositor> compositor_;

	// ---- GI settings ----
	GIMode gi_mode_ = GI_SDFGI;
	int sdfgi_cascades_ = 4;
	float sdfgi_min_cell_size_ = 0.1f;
	bool sdfgi_use_occlusion_ = true;

	// ---- VoxelGI settings ----
	VoxelGI *voxel_gi_ = nullptr;  // Auto-created or existing child.  Lifetime: valid after _ensure_nodes() if gi_mode_ == GI_VOXEL_GI.
	int voxel_gi_subdiv_ = 1;          // VoxelGI::SUBDIV_128 — good balance of quality/speed
	Vector3 voxel_gi_extents_ = Vector3(10.0f, 10.0f, 10.0f);  // Half-size of probe volume
	float voxel_gi_energy_ = 1.0f;
	float voxel_gi_propagation_ = 0.7f;
	float voxel_gi_bias_ = 1.5f;
	float voxel_gi_normal_bias_ = 1.0f;
	bool voxel_gi_interior_ = false;
	bool voxel_gi_two_bounces_ = true;

	// ---- Sky settings ----
	bool use_procedural_sky_ = true;
	Ref<Texture2D> sky_texture_;
	Color sky_top_color_ = Color(0.385f, 0.454f, 0.55f);
	Color sky_horizon_color_ = Color(0.646f, 0.654f, 0.67f);
	Color sky_bottom_color_ = Color(0.2f, 0.169f, 0.133f);
	float sky_energy_ = 1.0f;

	// ---- Sun settings ----
	Vector3 sun_rotation_degrees_ = Vector3(-45.0f, -30.0f, 0.0f);
	Color sun_color_ = Color(1.0f, 0.96f, 0.89f);
	float sun_energy_ = 1.0f;
	bool sun_shadows_ = true;
	int sun_shadow_cascades_ = 4;
	float sun_shadow_max_distance_ = 100.0f;

	// ---- Ambient settings ----
	Color ambient_color_ = Color(0.2f, 0.2f, 0.2f);
	float ambient_energy_ = 0.5f;

	// ---- Tonemapping ----
	TonemapMode tonemap_mode_ = TONEMAP_ACES;
	float tonemap_exposure_ = 1.0f;
	float tonemap_white_ = 6.0f;

	// ---- SSR ----
	bool ssr_enabled_ = false;
	int ssr_max_steps_ = 64;
	float ssr_fade_in_ = 0.15f;
	float ssr_fade_out_ = 2.0f;
	float ssr_depth_tolerance_ = 0.2f;

	// ---- SSAO ----
	bool ssao_enabled_ = false;
	float ssao_radius_ = 1.0f;
	float ssao_intensity_ = 2.0f;

	// ---- SSIL ----
	bool ssil_enabled_ = false;
	float ssil_radius_ = 5.0f;
	float ssil_intensity_ = 1.0f;

	// ---- Glow ----
	bool glow_enabled_ = false;
	float glow_intensity_ = 0.8f;
	float glow_bloom_ = 0.0f;

	// ---- Fog ----
	bool fog_enabled_ = false;
	float fog_density_ = 0.001f;
	Color fog_color_ = Color(0.8f, 0.85f, 0.9f);

	// ---- DOF ----
	bool dof_enabled_ = false;
	float dof_focus_distance_ = 10.0f;
	float dof_blur_amount_ = 0.1f;

	// ---- Auto exposure ----
	bool auto_exposure_enabled_ = false;
	float auto_exposure_scale_ = 0.4f;
	float auto_exposure_min_ = 0.05f;
	float auto_exposure_max_ = 8.0f;

	// ---- RT effects (Phase 5+) ----
	bool rt_reflections_enabled_ = false;
	float rt_reflections_roughness_threshold_ = 0.3f;

	// ---- Internal helpers ----
	void _ensure_nodes();
	VoxelGI *_resolve_voxel_gi();  // Three-tier resolve: explicit child → auto-discover → create.
	void _apply_environment();
	void _apply_sky();
	void _apply_sun();
	void _apply_tonemapping();
	void _apply_ssr();
	void _apply_ssao();
	void _apply_ssil();
	void _apply_glow();
	void _apply_fog();
	void _apply_dof();
	void _apply_camera_attributes();
	void _apply_gi();
	void _apply_compositor();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	RaySceneSetup();
	~RaySceneSetup();

	// Apply all current settings to the managed nodes.
	void apply();

	// Apply a quality preset (sets all parameters, then calls apply()).
	void apply_preset(int preset);

	// ---- GI ----
	void set_gi_mode(int mode);
	int get_gi_mode() const;
	void set_sdfgi_cascades(int cascades);
	int get_sdfgi_cascades() const;
	void set_sdfgi_min_cell_size(float size);
	float get_sdfgi_min_cell_size() const;
	void set_sdfgi_use_occlusion(bool enable);
	bool get_sdfgi_use_occlusion() const;

	// ---- VoxelGI ----
	void set_voxel_gi_subdiv(int subdiv);
	int get_voxel_gi_subdiv() const;
	void set_voxel_gi_extents(const Vector3 &extents);
	Vector3 get_voxel_gi_extents() const;
	void set_voxel_gi_energy(float energy);
	float get_voxel_gi_energy() const;
	void set_voxel_gi_propagation(float propagation);
	float get_voxel_gi_propagation() const;
	void set_voxel_gi_bias(float bias);
	float get_voxel_gi_bias() const;
	void set_voxel_gi_normal_bias(float bias);
	float get_voxel_gi_normal_bias() const;
	void set_voxel_gi_interior(bool enable);
	bool get_voxel_gi_interior() const;
	void set_voxel_gi_two_bounces(bool enable);
	bool get_voxel_gi_two_bounces() const;

	// ---- Sky ----
	void set_use_procedural_sky(bool enable);
	bool get_use_procedural_sky() const;
	void set_sky_texture(const Ref<Texture2D> &tex);
	Ref<Texture2D> get_sky_texture() const;
	void set_sky_energy(float energy);
	float get_sky_energy() const;

	// ---- Sun ----
	void set_sun_rotation_degrees(const Vector3 &rot);
	Vector3 get_sun_rotation_degrees() const;
	void set_sun_color(const Color &color);
	Color get_sun_color() const;
	void set_sun_energy(float energy);
	float get_sun_energy() const;
	void set_sun_shadows(bool enable);
	bool get_sun_shadows() const;

	// ---- Tonemapping ----
	void set_tonemap_mode(int mode);
	int get_tonemap_mode() const;
	void set_tonemap_exposure(float exposure);
	float get_tonemap_exposure() const;

	// ---- SSR ----
	void set_ssr_enabled(bool enable);
	bool get_ssr_enabled() const;
	void set_ssr_max_steps(int steps);
	int get_ssr_max_steps() const;

	// ---- SSAO ----
	void set_ssao_enabled(bool enable);
	bool get_ssao_enabled() const;
	void set_ssao_radius(float radius);
	float get_ssao_radius() const;
	void set_ssao_intensity(float intensity);
	float get_ssao_intensity() const;

	// ---- SSIL ----
	void set_ssil_enabled(bool enable);
	bool get_ssil_enabled() const;

	// ---- Glow ----
	void set_glow_enabled(bool enable);
	bool get_glow_enabled() const;
	void set_glow_intensity(float intensity);
	float get_glow_intensity() const;

	// ---- Fog ----
	void set_fog_enabled(bool enable);
	bool get_fog_enabled() const;
	void set_fog_density(float density);
	float get_fog_density() const;

	// ---- DOF ----
	void set_dof_enabled(bool enable);
	bool get_dof_enabled() const;
	void set_dof_focus_distance(float distance);
	float get_dof_focus_distance() const;
	void set_dof_blur_amount(float amount);
	float get_dof_blur_amount() const;

	// ---- Auto exposure ----
	void set_auto_exposure_enabled(bool enable);
	bool get_auto_exposure_enabled() const;

	// ---- RT effects ----
	void set_rt_reflections_enabled(bool enable);
	bool get_rt_reflections_enabled() const;
	void set_rt_reflections_roughness_threshold(float threshold);
	float get_rt_reflections_roughness_threshold() const;
};

VARIANT_ENUM_CAST(RaySceneSetup::QualityPreset);
VARIANT_ENUM_CAST(RaySceneSetup::GIMode);
VARIANT_ENUM_CAST(RaySceneSetup::TonemapMode);
