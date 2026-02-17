#pragma once
// light_data.h — Per-frame light parameters extracted from Godot light nodes.
//
// Populated each frame by RayRenderer from the scene tree (Godot-Native
// Principle).  Passed to shade_pass.h by const& — no Godot headers needed
// in the shading code.
//
// Supports three light types matching Godot's node hierarchy:
//   DirectionalLight3D → DIRECTIONAL  (infinite distance, no attenuation)
//   OmniLight3D        → POINT        (omnidirectional, range + attenuation)
//   SpotLight3D        → SPOT         (cone angle, range + attenuation)
//
// EXTENDING:
//   Phase 3+: area lights (Area → rectangular/disk emitters)

#include <godot_cpp/variant/vector3.hpp>

using godot::Vector3;

struct LightData {
	enum Type {
		DIRECTIONAL = 0,
		POINT = 1,
		SPOT = 2,
	};

	Type type = DIRECTIONAL;

	// Position in world space (unused for DIRECTIONAL).
	Vector3 position = Vector3(0, 0, 0);

	// Direction (normalized). For DIRECTIONAL: light direction (toward light).
	// For SPOT: axis of the cone (direction the spot points).
	Vector3 direction = Vector3(0, -1, 0);

	// Light color × energy (pre-multiplied).  Already in linear color space.
	Vector3 color = Vector3(1, 1, 1);

	// Maximum range in world units (POINT and SPOT only).
	// Beyond this distance, attenuation is clamped to zero.
	float range = 10.0f;

	// Attenuation exponent (from Light3D::PARAM_ATTENUATION).
	// Godot uses: attenuation = pow(max(1 - d/range, 0), attenuation_exp)
	float attenuation = 1.0f;

	// Spot cone angles in radians (SPOT only).
	// spot_angle: outer cone half-angle (full cutoff).
	// spot_angle_attenuation: controls the falloff from inner to outer cone edge.
	float spot_angle = 0.785398f;  // 45 degrees default
	float spot_angle_attenuation = 1.0f;

	// Shadow casting enabled for this light.
	bool cast_shadows = true;
};

/// Maximum number of lights supported per frame.
/// Keeps the shade loop bounded and avoids dynamic allocation.
static constexpr int MAX_SCENE_LIGHTS = 16;

/// Per-frame collection of extracted lights, passed to shade_pass.h.
struct SceneLightData {
	LightData lights[MAX_SCENE_LIGHTS];
	int light_count = 0;
};
