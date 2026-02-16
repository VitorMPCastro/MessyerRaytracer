// ray_scene_setup.cpp — Implementation of RaySceneSetup Node3D.
//
// Programmatically creates and configures Godot's rendering environment
// for maximum visual quality. Manages WorldEnvironment, DirectionalLight3D,
// and quality presets from C++.

#include "ray_scene_setup.h"

#include <godot_cpp/classes/procedural_sky_material.hpp>
#include <godot_cpp/classes/panorama_sky_material.hpp>
#include <godot_cpp/classes/light3d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>

using namespace godot;

// ============================================================================
// Constructor / Destructor
// ============================================================================

RaySceneSetup::RaySceneSetup() {}

RaySceneSetup::~RaySceneSetup() {}

// ============================================================================
// Notification handler
// ============================================================================

void RaySceneSetup::_notification(int p_what) {
	if (p_what == NOTIFICATION_READY) {
		_ensure_nodes();
		apply();
	}
}

// ============================================================================
// _ensure_nodes — lazily create managed child nodes
// ============================================================================

void RaySceneSetup::_ensure_nodes() {
	// ---- Environment resource ----
	if (environment_.is_null()) {
		environment_.instantiate();
	}

	// ---- Sky resource ----
	if (sky_.is_null()) {
		sky_.instantiate();
	}

	// ---- Camera attributes ----
	if (camera_attrs_.is_null()) {
		camera_attrs_.instantiate();
	}

	// ---- WorldEnvironment child node ----
	if (!world_env_) {
		world_env_ = memnew(WorldEnvironment);
		world_env_->set_name("ManagedWorldEnvironment");
		add_child(world_env_);
	}

	// ---- DirectionalLight3D child node ----
	if (!sun_) {
		sun_ = memnew(DirectionalLight3D);
		sun_->set_name("ManagedSun");
		add_child(sun_);
	}
}

// ============================================================================
// apply — push all settings to managed nodes
// ============================================================================

void RaySceneSetup::apply() {
	_ensure_nodes();
	_apply_sky();
	_apply_environment();
	_apply_gi();
	_apply_tonemapping();
	_apply_ssr();
	_apply_ssao();
	_apply_ssil();
	_apply_glow();
	_apply_fog();
	_apply_camera_attributes();
	_apply_dof();
	_apply_sun();
	_apply_compositor();

	// Attach environment + camera attributes to WorldEnvironment.
	world_env_->set_environment(environment_);
	world_env_->set_camera_attributes(camera_attrs_);

	UtilityFunctions::print("[RaySceneSetup] Configuration applied.");
}

// ============================================================================
// apply_preset — configure all parameters for a hardware tier
// ============================================================================

void RaySceneSetup::apply_preset(int preset) {
	switch (static_cast<QualityPreset>(preset)) {

	case PRESET_LOW:
		// Integrated GPU / mobile — minimal effects
		gi_mode_ = GI_NONE;
		tonemap_mode_ = TONEMAP_FILMIC;
		tonemap_exposure_ = 1.0f;
		ssr_enabled_ = false;
		ssao_enabled_ = false;
		ssil_enabled_ = false;
		glow_enabled_ = false;
		fog_enabled_ = false;
		dof_enabled_ = false;
		auto_exposure_enabled_ = false;
		sun_shadows_ = true;
		sun_shadow_cascades_ = 2;
		sun_shadow_max_distance_ = 50.0f;
		rt_reflections_enabled_ = false;
		break;

	case PRESET_MEDIUM:
		// GTX 1650 Ti class — good baseline
		gi_mode_ = GI_SDFGI;
		sdfgi_cascades_ = 4;
		sdfgi_min_cell_size_ = 0.2f;
		sdfgi_use_occlusion_ = true;
		tonemap_mode_ = TONEMAP_ACES;
		tonemap_exposure_ = 1.0f;
		ssr_enabled_ = true;
		ssr_max_steps_ = 56;
		ssao_enabled_ = true;
		ssao_radius_ = 1.0f;
		ssao_intensity_ = 2.0f;
		ssil_enabled_ = false;
		glow_enabled_ = true;
		glow_intensity_ = 0.8f;
		glow_bloom_ = 0.1f;
		fog_enabled_ = false;
		dof_enabled_ = false;
		auto_exposure_enabled_ = false;
		sun_shadows_ = true;
		sun_shadow_cascades_ = 4;
		sun_shadow_max_distance_ = 100.0f;
		rt_reflections_enabled_ = false;
		break;

	case PRESET_HIGH:
		// RTX 3060+ class — high quality
		gi_mode_ = GI_SDFGI;
		sdfgi_cascades_ = 6;
		sdfgi_min_cell_size_ = 0.1f;
		sdfgi_use_occlusion_ = true;
		tonemap_mode_ = TONEMAP_AGX;
		tonemap_exposure_ = 1.0f;
		ssr_enabled_ = true;
		ssr_max_steps_ = 96;
		ssao_enabled_ = true;
		ssao_radius_ = 1.0f;
		ssao_intensity_ = 2.5f;
		ssil_enabled_ = true;
		ssil_radius_ = 5.0f;
		ssil_intensity_ = 1.0f;
		glow_enabled_ = true;
		glow_intensity_ = 0.8f;
		glow_bloom_ = 0.05f;
		fog_enabled_ = true;
		fog_density_ = 0.001f;
		dof_enabled_ = false;
		auto_exposure_enabled_ = true;
		auto_exposure_scale_ = 0.4f;
		sun_shadows_ = true;
		sun_shadow_cascades_ = 4;
		sun_shadow_max_distance_ = 200.0f;
		rt_reflections_enabled_ = false;
		break;

	case PRESET_ULTRA:
		// RTX 4070+ class — everything + RT effects
		gi_mode_ = GI_SDFGI;
		sdfgi_cascades_ = 6;
		sdfgi_min_cell_size_ = 0.1f;
		sdfgi_use_occlusion_ = true;
		tonemap_mode_ = TONEMAP_AGX;
		tonemap_exposure_ = 1.0f;
		ssr_enabled_ = false; // Replaced by RT reflections
		ssao_enabled_ = true;
		ssao_radius_ = 1.0f;
		ssao_intensity_ = 2.5f;
		ssil_enabled_ = true;
		ssil_radius_ = 5.0f;
		ssil_intensity_ = 1.0f;
		glow_enabled_ = true;
		glow_intensity_ = 0.8f;
		glow_bloom_ = 0.05f;
		fog_enabled_ = true;
		fog_density_ = 0.0005f;
		dof_enabled_ = true;
		dof_focus_distance_ = 10.0f;
		dof_blur_amount_ = 0.05f;
		auto_exposure_enabled_ = true;
		auto_exposure_scale_ = 0.4f;
		sun_shadows_ = true;
		sun_shadow_cascades_ = 4;
		sun_shadow_max_distance_ = 200.0f;
		rt_reflections_enabled_ = true;
		rt_reflections_roughness_threshold_ = 0.3f;
		break;

	default:
		UtilityFunctions::printerr("[RaySceneSetup] Unknown preset: ", preset);
		return;
	}

	apply();
	UtilityFunctions::print("[RaySceneSetup] Applied preset: ", preset);
}

// ============================================================================
// Internal: Apply sky settings
// ============================================================================

void RaySceneSetup::_apply_sky() {
	if (use_procedural_sky_) {
		Ref<ProceduralSkyMaterial> sky_mat;
		sky_mat.instantiate();
		sky_mat->set_sky_top_color(sky_top_color_);
		sky_mat->set_sky_horizon_color(sky_horizon_color_);
		sky_mat->set_ground_bottom_color(sky_bottom_color_);
		sky_mat->set_ground_horizon_color(sky_horizon_color_);
		sky_mat->set_sky_energy_multiplier(sky_energy_);
		sky_->set_material(sky_mat);
	} else if (sky_texture_.is_valid()) {
		Ref<PanoramaSkyMaterial> sky_mat;
		sky_mat.instantiate();
		sky_mat->set_panorama(sky_texture_);
		sky_->set_material(sky_mat);
	}

	sky_->set_process_mode(Sky::PROCESS_MODE_AUTOMATIC);
	environment_->set_sky(sky_);
	environment_->set_background(Environment::BG_SKY);
}

// ============================================================================
// Internal: Apply main environment settings
// ============================================================================

void RaySceneSetup::_apply_environment() {
	environment_->set_ambient_source(Environment::AMBIENT_SOURCE_SKY);
	environment_->set_ambient_light_color(ambient_color_);
	environment_->set_ambient_light_energy(ambient_energy_);
	environment_->set_ambient_light_sky_contribution(1.0f);
	environment_->set_reflection_source(Environment::REFLECTION_SOURCE_SKY);
}

// ============================================================================
// Internal: Apply GI settings
// ============================================================================

void RaySceneSetup::_apply_gi() {
	switch (gi_mode_) {
	case GI_SDFGI:
		environment_->set_sdfgi_enabled(true);
		environment_->set_sdfgi_cascades(sdfgi_cascades_);
		environment_->set_sdfgi_min_cell_size(sdfgi_min_cell_size_);
		environment_->set_sdfgi_use_occlusion(sdfgi_use_occlusion_);
		environment_->set_sdfgi_bounce_feedback(0.5f);
		environment_->set_sdfgi_read_sky_light(true);
		environment_->set_sdfgi_energy(1.0f);
		break;
	case GI_VOXEL_GI:
		environment_->set_sdfgi_enabled(false);
		// VoxelGI requires a VoxelGI node as a scene child — user must add it.
		break;
	case GI_NONE:
	default:
		environment_->set_sdfgi_enabled(false);
		break;
	}
}

// ============================================================================
// Internal: Apply tonemapping
// ============================================================================

void RaySceneSetup::_apply_tonemapping() {
	static const Environment::ToneMapper TONEMAP_MAP[] = {
		Environment::TONE_MAPPER_LINEAR,
		Environment::TONE_MAPPER_REINHARDT,
		Environment::TONE_MAPPER_FILMIC,
		Environment::TONE_MAPPER_ACES,
		Environment::TONE_MAPPER_AGX,
	};
	int idx = static_cast<int>(tonemap_mode_);
	if (idx >= 0 && idx < 5) {
		environment_->set_tonemapper(TONEMAP_MAP[idx]);
	}
	environment_->set_tonemap_exposure(tonemap_exposure_);
	environment_->set_tonemap_white(tonemap_white_);
}

// ============================================================================
// Internal: Apply SSR
// ============================================================================

void RaySceneSetup::_apply_ssr() {
	environment_->set_ssr_enabled(ssr_enabled_);
	if (ssr_enabled_) {
		environment_->set_ssr_max_steps(ssr_max_steps_);
		environment_->set_ssr_fade_in(ssr_fade_in_);
		environment_->set_ssr_fade_out(ssr_fade_out_);
		environment_->set_ssr_depth_tolerance(ssr_depth_tolerance_);
	}
}

// ============================================================================
// Internal: Apply SSAO
// ============================================================================

void RaySceneSetup::_apply_ssao() {
	environment_->set_ssao_enabled(ssao_enabled_);
	if (ssao_enabled_) {
		environment_->set_ssao_radius(ssao_radius_);
		environment_->set_ssao_intensity(ssao_intensity_);
		environment_->set_ssao_power(1.5f);
		environment_->set_ssao_detail(0.5f);
		environment_->set_ssao_horizon(0.06f);
	}
}

// ============================================================================
// Internal: Apply SSIL
// ============================================================================

void RaySceneSetup::_apply_ssil() {
	environment_->set_ssil_enabled(ssil_enabled_);
	if (ssil_enabled_) {
		environment_->set_ssil_radius(ssil_radius_);
		environment_->set_ssil_intensity(ssil_intensity_);
		environment_->set_ssil_sharpness(0.98f);
		environment_->set_ssil_normal_rejection(1.0f);
	}
}

// ============================================================================
// Internal: Apply glow / bloom
// ============================================================================

void RaySceneSetup::_apply_glow() {
	environment_->set_glow_enabled(glow_enabled_);
	if (glow_enabled_) {
		environment_->set_glow_intensity(glow_intensity_);
		environment_->set_glow_bloom(glow_bloom_);
		environment_->set_glow_blend_mode(Environment::GLOW_BLEND_MODE_SOFTLIGHT);
		environment_->set_glow_hdr_bleed_threshold(1.0f);
		environment_->set_glow_hdr_bleed_scale(2.0f);
		environment_->set_glow_hdr_luminance_cap(12.0f);
	}
}

// ============================================================================
// Internal: Apply fog
// ============================================================================

void RaySceneSetup::_apply_fog() {
	environment_->set_fog_enabled(fog_enabled_);
	if (fog_enabled_) {
		environment_->set_fog_light_color(fog_color_);
		environment_->set_fog_density(fog_density_);
		environment_->set_fog_aerial_perspective(0.5f);
		environment_->set_fog_sky_affect(1.0f);
	}
}

// ============================================================================
// Internal: Apply DOF (via CameraAttributesPractical)
// ============================================================================

void RaySceneSetup::_apply_dof() {
	if (dof_enabled_) {
		camera_attrs_->set_dof_blur_far_enabled(true);
		camera_attrs_->set_dof_blur_far_distance(dof_focus_distance_);
		camera_attrs_->set_dof_blur_far_transition(5.0f);
		camera_attrs_->set_dof_blur_near_enabled(true);
		camera_attrs_->set_dof_blur_near_distance(dof_focus_distance_ * 0.5f);
		camera_attrs_->set_dof_blur_near_transition(1.0f);
		camera_attrs_->set_dof_blur_amount(dof_blur_amount_);
	} else {
		camera_attrs_->set_dof_blur_far_enabled(false);
		camera_attrs_->set_dof_blur_near_enabled(false);
	}
}

// ============================================================================
// Internal: Apply camera attributes (auto exposure)
// ============================================================================

void RaySceneSetup::_apply_camera_attributes() {
	camera_attrs_->set_auto_exposure_enabled(auto_exposure_enabled_);
	if (auto_exposure_enabled_) {
		camera_attrs_->set_auto_exposure_scale(auto_exposure_scale_);
		camera_attrs_->set_auto_exposure_min_sensitivity(auto_exposure_min_ * 100.0f);
		camera_attrs_->set_auto_exposure_max_sensitivity(auto_exposure_max_ * 100.0f);
		camera_attrs_->set_auto_exposure_speed(0.5f);
	}
}

// ============================================================================
// Internal: Apply sun (DirectionalLight3D)
// ============================================================================

void RaySceneSetup::_apply_sun() {
	sun_->set_color(sun_color_);
	sun_->set_param(Light3D::PARAM_ENERGY, sun_energy_);
	sun_->set_param(Light3D::PARAM_SIZE, 0.5f); // Angular size for PCSS soft shadows
	sun_->set_param(Light3D::PARAM_SHADOW_MAX_DISTANCE, sun_shadow_max_distance_);
	sun_->set_param(Light3D::PARAM_SHADOW_BLUR, 1.0f);
	sun_->set_shadow(sun_shadows_);

	// Rotation: convert degrees to radians and apply.
	Vector3 rot_rad = sun_rotation_degrees_ * (Math_PI / 180.0f);
	sun_->set_rotation(rot_rad);

	// Shadow cascade mode.
	if (sun_shadow_cascades_ <= 2) {
		sun_->set_shadow_mode(DirectionalLight3D::SHADOW_PARALLEL_2_SPLITS);
	} else {
		sun_->set_shadow_mode(DirectionalLight3D::SHADOW_PARALLEL_4_SPLITS);
	}
}

// ============================================================================
// Internal: Apply compositor (for RT effects, Phase 5+)
// ============================================================================

void RaySceneSetup::_apply_compositor() {
	// Compositor integration will be implemented in Phase 5.
	// RTReflectionEffect will be attached here when rt_reflections_enabled_ is true.
}

// ============================================================================
// Property getters / setters
// ============================================================================

// GI
void RaySceneSetup::set_gi_mode(int mode) { gi_mode_ = static_cast<GIMode>(mode); }
int RaySceneSetup::get_gi_mode() const { return static_cast<int>(gi_mode_); }
void RaySceneSetup::set_sdfgi_cascades(int cascades) { sdfgi_cascades_ = cascades; }
int RaySceneSetup::get_sdfgi_cascades() const { return sdfgi_cascades_; }
void RaySceneSetup::set_sdfgi_min_cell_size(float size) { sdfgi_min_cell_size_ = size; }
float RaySceneSetup::get_sdfgi_min_cell_size() const { return sdfgi_min_cell_size_; }
void RaySceneSetup::set_sdfgi_use_occlusion(bool enable) { sdfgi_use_occlusion_ = enable; }
bool RaySceneSetup::get_sdfgi_use_occlusion() const { return sdfgi_use_occlusion_; }

// Sky
void RaySceneSetup::set_use_procedural_sky(bool enable) { use_procedural_sky_ = enable; }
bool RaySceneSetup::get_use_procedural_sky() const { return use_procedural_sky_; }
void RaySceneSetup::set_sky_texture(const Ref<Texture2D> &tex) { sky_texture_ = tex; }
Ref<Texture2D> RaySceneSetup::get_sky_texture() const { return sky_texture_; }
void RaySceneSetup::set_sky_energy(float energy) { sky_energy_ = energy; }
float RaySceneSetup::get_sky_energy() const { return sky_energy_; }

// Sun
void RaySceneSetup::set_sun_rotation_degrees(const Vector3 &rot) { sun_rotation_degrees_ = rot; }
Vector3 RaySceneSetup::get_sun_rotation_degrees() const { return sun_rotation_degrees_; }
void RaySceneSetup::set_sun_color(const Color &color) { sun_color_ = color; }
Color RaySceneSetup::get_sun_color() const { return sun_color_; }
void RaySceneSetup::set_sun_energy(float energy) { sun_energy_ = energy; }
float RaySceneSetup::get_sun_energy() const { return sun_energy_; }
void RaySceneSetup::set_sun_shadows(bool enable) { sun_shadows_ = enable; }
bool RaySceneSetup::get_sun_shadows() const { return sun_shadows_; }

// Tonemapping
void RaySceneSetup::set_tonemap_mode(int mode) { tonemap_mode_ = static_cast<TonemapMode>(mode); }
int RaySceneSetup::get_tonemap_mode() const { return static_cast<int>(tonemap_mode_); }
void RaySceneSetup::set_tonemap_exposure(float exposure) { tonemap_exposure_ = exposure; }
float RaySceneSetup::get_tonemap_exposure() const { return tonemap_exposure_; }

// SSR
void RaySceneSetup::set_ssr_enabled(bool enable) { ssr_enabled_ = enable; }
bool RaySceneSetup::get_ssr_enabled() const { return ssr_enabled_; }
void RaySceneSetup::set_ssr_max_steps(int steps) { ssr_max_steps_ = steps; }
int RaySceneSetup::get_ssr_max_steps() const { return ssr_max_steps_; }

// SSAO
void RaySceneSetup::set_ssao_enabled(bool enable) { ssao_enabled_ = enable; }
bool RaySceneSetup::get_ssao_enabled() const { return ssao_enabled_; }
void RaySceneSetup::set_ssao_radius(float radius) { ssao_radius_ = radius; }
float RaySceneSetup::get_ssao_radius() const { return ssao_radius_; }
void RaySceneSetup::set_ssao_intensity(float intensity) { ssao_intensity_ = intensity; }
float RaySceneSetup::get_ssao_intensity() const { return ssao_intensity_; }

// SSIL
void RaySceneSetup::set_ssil_enabled(bool enable) { ssil_enabled_ = enable; }
bool RaySceneSetup::get_ssil_enabled() const { return ssil_enabled_; }

// Glow
void RaySceneSetup::set_glow_enabled(bool enable) { glow_enabled_ = enable; }
bool RaySceneSetup::get_glow_enabled() const { return glow_enabled_; }
void RaySceneSetup::set_glow_intensity(float intensity) { glow_intensity_ = intensity; }
float RaySceneSetup::get_glow_intensity() const { return glow_intensity_; }

// Fog
void RaySceneSetup::set_fog_enabled(bool enable) { fog_enabled_ = enable; }
bool RaySceneSetup::get_fog_enabled() const { return fog_enabled_; }
void RaySceneSetup::set_fog_density(float density) { fog_density_ = density; }
float RaySceneSetup::get_fog_density() const { return fog_density_; }

// DOF
void RaySceneSetup::set_dof_enabled(bool enable) { dof_enabled_ = enable; }
bool RaySceneSetup::get_dof_enabled() const { return dof_enabled_; }
void RaySceneSetup::set_dof_focus_distance(float distance) { dof_focus_distance_ = distance; }
float RaySceneSetup::get_dof_focus_distance() const { return dof_focus_distance_; }
void RaySceneSetup::set_dof_blur_amount(float amount) { dof_blur_amount_ = amount; }
float RaySceneSetup::get_dof_blur_amount() const { return dof_blur_amount_; }

// Auto exposure
void RaySceneSetup::set_auto_exposure_enabled(bool enable) { auto_exposure_enabled_ = enable; }
bool RaySceneSetup::get_auto_exposure_enabled() const { return auto_exposure_enabled_; }

// RT effects
void RaySceneSetup::set_rt_reflections_enabled(bool enable) { rt_reflections_enabled_ = enable; }
bool RaySceneSetup::get_rt_reflections_enabled() const { return rt_reflections_enabled_; }
void RaySceneSetup::set_rt_reflections_roughness_threshold(float threshold) { rt_reflections_roughness_threshold_ = threshold; }
float RaySceneSetup::get_rt_reflections_roughness_threshold() const { return rt_reflections_roughness_threshold_; }

// ============================================================================
// _bind_methods — register properties and methods for GDScript
// ============================================================================

void RaySceneSetup::_bind_methods() {
	// ---- Methods ----
	ClassDB::bind_method(D_METHOD("apply"), &RaySceneSetup::apply);
	ClassDB::bind_method(D_METHOD("apply_preset", "preset"), &RaySceneSetup::apply_preset);

	// ---- GI ----
	ClassDB::bind_method(D_METHOD("set_gi_mode", "mode"), &RaySceneSetup::set_gi_mode);
	ClassDB::bind_method(D_METHOD("get_gi_mode"), &RaySceneSetup::get_gi_mode);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "gi_mode", PROPERTY_HINT_ENUM, "None,SDFGI,VoxelGI"), "set_gi_mode", "get_gi_mode");

	ClassDB::bind_method(D_METHOD("set_sdfgi_cascades", "cascades"), &RaySceneSetup::set_sdfgi_cascades);
	ClassDB::bind_method(D_METHOD("get_sdfgi_cascades"), &RaySceneSetup::get_sdfgi_cascades);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "sdfgi_cascades", PROPERTY_HINT_RANGE, "1,8,1"), "set_sdfgi_cascades", "get_sdfgi_cascades");

	ClassDB::bind_method(D_METHOD("set_sdfgi_min_cell_size", "size"), &RaySceneSetup::set_sdfgi_min_cell_size);
	ClassDB::bind_method(D_METHOD("get_sdfgi_min_cell_size"), &RaySceneSetup::get_sdfgi_min_cell_size);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sdfgi_min_cell_size", PROPERTY_HINT_RANGE, "0.01,1.0,0.01"), "set_sdfgi_min_cell_size", "get_sdfgi_min_cell_size");

	ClassDB::bind_method(D_METHOD("set_sdfgi_use_occlusion", "enable"), &RaySceneSetup::set_sdfgi_use_occlusion);
	ClassDB::bind_method(D_METHOD("get_sdfgi_use_occlusion"), &RaySceneSetup::get_sdfgi_use_occlusion);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "sdfgi_use_occlusion"), "set_sdfgi_use_occlusion", "get_sdfgi_use_occlusion");

	// ---- Sky ----
	ClassDB::bind_method(D_METHOD("set_use_procedural_sky", "enable"), &RaySceneSetup::set_use_procedural_sky);
	ClassDB::bind_method(D_METHOD("get_use_procedural_sky"), &RaySceneSetup::get_use_procedural_sky);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_procedural_sky"), "set_use_procedural_sky", "get_use_procedural_sky");

	ClassDB::bind_method(D_METHOD("set_sky_texture", "texture"), &RaySceneSetup::set_sky_texture);
	ClassDB::bind_method(D_METHOD("get_sky_texture"), &RaySceneSetup::get_sky_texture);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "sky_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_sky_texture", "get_sky_texture");

	ClassDB::bind_method(D_METHOD("set_sky_energy", "energy"), &RaySceneSetup::set_sky_energy);
	ClassDB::bind_method(D_METHOD("get_sky_energy"), &RaySceneSetup::get_sky_energy);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sky_energy", PROPERTY_HINT_RANGE, "0.0,16.0,0.01"), "set_sky_energy", "get_sky_energy");

	// ---- Sun ----
	ClassDB::bind_method(D_METHOD("set_sun_rotation_degrees", "rotation"), &RaySceneSetup::set_sun_rotation_degrees);
	ClassDB::bind_method(D_METHOD("get_sun_rotation_degrees"), &RaySceneSetup::get_sun_rotation_degrees);
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "sun_rotation_degrees"), "set_sun_rotation_degrees", "get_sun_rotation_degrees");

	ClassDB::bind_method(D_METHOD("set_sun_color", "color"), &RaySceneSetup::set_sun_color);
	ClassDB::bind_method(D_METHOD("get_sun_color"), &RaySceneSetup::get_sun_color);
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "sun_color"), "set_sun_color", "get_sun_color");

	ClassDB::bind_method(D_METHOD("set_sun_energy", "energy"), &RaySceneSetup::set_sun_energy);
	ClassDB::bind_method(D_METHOD("get_sun_energy"), &RaySceneSetup::get_sun_energy);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sun_energy", PROPERTY_HINT_RANGE, "0.0,16.0,0.01"), "set_sun_energy", "get_sun_energy");

	ClassDB::bind_method(D_METHOD("set_sun_shadows", "enable"), &RaySceneSetup::set_sun_shadows);
	ClassDB::bind_method(D_METHOD("get_sun_shadows"), &RaySceneSetup::get_sun_shadows);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "sun_shadows"), "set_sun_shadows", "get_sun_shadows");

	// ---- Tonemapping ----
	ClassDB::bind_method(D_METHOD("set_tonemap_mode", "mode"), &RaySceneSetup::set_tonemap_mode);
	ClassDB::bind_method(D_METHOD("get_tonemap_mode"), &RaySceneSetup::get_tonemap_mode);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "tonemap_mode", PROPERTY_HINT_ENUM, "Linear,Reinhard,Filmic,ACES,AgX"), "set_tonemap_mode", "get_tonemap_mode");

	ClassDB::bind_method(D_METHOD("set_tonemap_exposure", "exposure"), &RaySceneSetup::set_tonemap_exposure);
	ClassDB::bind_method(D_METHOD("get_tonemap_exposure"), &RaySceneSetup::get_tonemap_exposure);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tonemap_exposure", PROPERTY_HINT_RANGE, "0.01,16.0,0.01"), "set_tonemap_exposure", "get_tonemap_exposure");

	// ---- SSR ----
	ClassDB::bind_method(D_METHOD("set_ssr_enabled", "enable"), &RaySceneSetup::set_ssr_enabled);
	ClassDB::bind_method(D_METHOD("get_ssr_enabled"), &RaySceneSetup::get_ssr_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "ssr_enabled"), "set_ssr_enabled", "get_ssr_enabled");

	ClassDB::bind_method(D_METHOD("set_ssr_max_steps", "steps"), &RaySceneSetup::set_ssr_max_steps);
	ClassDB::bind_method(D_METHOD("get_ssr_max_steps"), &RaySceneSetup::get_ssr_max_steps);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "ssr_max_steps", PROPERTY_HINT_RANGE, "1,256,1"), "set_ssr_max_steps", "get_ssr_max_steps");

	// ---- SSAO ----
	ClassDB::bind_method(D_METHOD("set_ssao_enabled", "enable"), &RaySceneSetup::set_ssao_enabled);
	ClassDB::bind_method(D_METHOD("get_ssao_enabled"), &RaySceneSetup::get_ssao_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "ssao_enabled"), "set_ssao_enabled", "get_ssao_enabled");

	ClassDB::bind_method(D_METHOD("set_ssao_radius", "radius"), &RaySceneSetup::set_ssao_radius);
	ClassDB::bind_method(D_METHOD("get_ssao_radius"), &RaySceneSetup::get_ssao_radius);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ssao_radius", PROPERTY_HINT_RANGE, "0.01,16.0,0.01"), "set_ssao_radius", "get_ssao_radius");

	ClassDB::bind_method(D_METHOD("set_ssao_intensity", "intensity"), &RaySceneSetup::set_ssao_intensity);
	ClassDB::bind_method(D_METHOD("get_ssao_intensity"), &RaySceneSetup::get_ssao_intensity);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ssao_intensity", PROPERTY_HINT_RANGE, "0.0,16.0,0.01"), "set_ssao_intensity", "get_ssao_intensity");

	// ---- SSIL ----
	ClassDB::bind_method(D_METHOD("set_ssil_enabled", "enable"), &RaySceneSetup::set_ssil_enabled);
	ClassDB::bind_method(D_METHOD("get_ssil_enabled"), &RaySceneSetup::get_ssil_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "ssil_enabled"), "set_ssil_enabled", "get_ssil_enabled");

	// ---- Glow ----
	ClassDB::bind_method(D_METHOD("set_glow_enabled", "enable"), &RaySceneSetup::set_glow_enabled);
	ClassDB::bind_method(D_METHOD("get_glow_enabled"), &RaySceneSetup::get_glow_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "glow_enabled"), "set_glow_enabled", "get_glow_enabled");

	ClassDB::bind_method(D_METHOD("set_glow_intensity", "intensity"), &RaySceneSetup::set_glow_intensity);
	ClassDB::bind_method(D_METHOD("get_glow_intensity"), &RaySceneSetup::get_glow_intensity);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "glow_intensity", PROPERTY_HINT_RANGE, "0.0,8.0,0.01"), "set_glow_intensity", "get_glow_intensity");

	// ---- Fog ----
	ClassDB::bind_method(D_METHOD("set_fog_enabled", "enable"), &RaySceneSetup::set_fog_enabled);
	ClassDB::bind_method(D_METHOD("get_fog_enabled"), &RaySceneSetup::get_fog_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "fog_enabled"), "set_fog_enabled", "get_fog_enabled");

	ClassDB::bind_method(D_METHOD("set_fog_density", "density"), &RaySceneSetup::set_fog_density);
	ClassDB::bind_method(D_METHOD("get_fog_density"), &RaySceneSetup::get_fog_density);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fog_density", PROPERTY_HINT_RANGE, "0.0,1.0,0.0001"), "set_fog_density", "get_fog_density");

	// ---- DOF ----
	ClassDB::bind_method(D_METHOD("set_dof_enabled", "enable"), &RaySceneSetup::set_dof_enabled);
	ClassDB::bind_method(D_METHOD("get_dof_enabled"), &RaySceneSetup::get_dof_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "dof_enabled"), "set_dof_enabled", "get_dof_enabled");

	ClassDB::bind_method(D_METHOD("set_dof_focus_distance", "distance"), &RaySceneSetup::set_dof_focus_distance);
	ClassDB::bind_method(D_METHOD("get_dof_focus_distance"), &RaySceneSetup::get_dof_focus_distance);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "dof_focus_distance", PROPERTY_HINT_RANGE, "0.1,500.0,0.1"), "set_dof_focus_distance", "get_dof_focus_distance");

	ClassDB::bind_method(D_METHOD("set_dof_blur_amount", "amount"), &RaySceneSetup::set_dof_blur_amount);
	ClassDB::bind_method(D_METHOD("get_dof_blur_amount"), &RaySceneSetup::get_dof_blur_amount);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "dof_blur_amount", PROPERTY_HINT_RANGE, "0.0,1.0,0.01"), "set_dof_blur_amount", "get_dof_blur_amount");

	// ---- Auto exposure ----
	ClassDB::bind_method(D_METHOD("set_auto_exposure_enabled", "enable"), &RaySceneSetup::set_auto_exposure_enabled);
	ClassDB::bind_method(D_METHOD("get_auto_exposure_enabled"), &RaySceneSetup::get_auto_exposure_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_exposure_enabled"), "set_auto_exposure_enabled", "get_auto_exposure_enabled");

	// ---- RT effects ----
	ClassDB::bind_method(D_METHOD("set_rt_reflections_enabled", "enable"), &RaySceneSetup::set_rt_reflections_enabled);
	ClassDB::bind_method(D_METHOD("get_rt_reflections_enabled"), &RaySceneSetup::get_rt_reflections_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "rt_reflections_enabled"), "set_rt_reflections_enabled", "get_rt_reflections_enabled");

	ClassDB::bind_method(D_METHOD("set_rt_reflections_roughness_threshold", "threshold"), &RaySceneSetup::set_rt_reflections_roughness_threshold);
	ClassDB::bind_method(D_METHOD("get_rt_reflections_roughness_threshold"), &RaySceneSetup::get_rt_reflections_roughness_threshold);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rt_reflections_roughness_threshold", PROPERTY_HINT_RANGE, "0.0,1.0,0.01"), "set_rt_reflections_roughness_threshold", "get_rt_reflections_roughness_threshold");

	// ---- Enum constants ----
	BIND_ENUM_CONSTANT(PRESET_LOW);
	BIND_ENUM_CONSTANT(PRESET_MEDIUM);
	BIND_ENUM_CONSTANT(PRESET_HIGH);
	BIND_ENUM_CONSTANT(PRESET_ULTRA);

	BIND_ENUM_CONSTANT(GI_NONE);
	BIND_ENUM_CONSTANT(GI_SDFGI);
	BIND_ENUM_CONSTANT(GI_VOXEL_GI);

	BIND_ENUM_CONSTANT(TONEMAP_LINEAR);
	BIND_ENUM_CONSTANT(TONEMAP_REINHARD);
	BIND_ENUM_CONSTANT(TONEMAP_FILMIC);
	BIND_ENUM_CONSTANT(TONEMAP_ACES);
	BIND_ENUM_CONSTANT(TONEMAP_AGX);
}
