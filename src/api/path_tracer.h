#pragma once
// path_tracer.h — Abstract interface for multi-bounce path tracing.
//
// WHAT:  Defines IPathTracer, the abstraction boundary between
//        RayRenderer (frame orchestration) and the actual path tracing
//        implementation (CPU today, GPU later).
//
// WHY:   RayRenderer should not hard-code CPU bounce logic.  When a GPU
//        path tracer is added, it implements the same interface and
//        RayRenderer switches transparently at runtime.
//
// USAGE:
//   PathTraceParams params;
//   params.width  = 1280;  params.height = 960;
//   // ... fill remaining fields ...
//
//   path_tracer->trace_frame(params, primary_rays, color_output, svc, pool);
//
// OWNERSHIP:
//   RayRenderer owns the IPathTracer instance (unique_ptr).
//   The path tracer does NOT own the ray service or thread pool — they are
//   passed per call and must remain valid for the duration of trace_frame().

#include "api/scene_shade_data.h"
#include "api/light_data.h"
#include "modules/graphics/shade_pass.h"  // EnvironmentData

class Ray;
class Intersection;
class IRayService;
class IThreadDispatch;

/// Camera parameters for GPU ray generation.
/// Populated by RayRenderer from Camera3D each frame.
/// Plain floats (no Godot types) so this struct can be memcpy'd to GPU.
struct CameraParams {
	float origin[3]  = {};         // Camera position in world space.
	float forward[3] = {};         // Forward direction (-Z in camera local).
	float right[3]   = {};         // Right direction   (X in camera local).
	float up[3]      = {};         // Up direction      (Y in camera local).
	float fov_y_rad  = 0.0f;      // Vertical FOV in radians (perspective only).
	float aspect     = 1.0f;      // Width / height.
	float near_plane = 0.05f;     // Camera near clip.
	float far_plane  = 4000.0f;   // Camera far clip.
};

/// Parameters for a single path-traced frame.
/// Populated by RayRenderer from scene node reads and passed by const ref.
struct PathTraceParams {
	int width   = 0;               // Framebuffer width in pixels.
	int height  = 0;               // Framebuffer height in pixels.
	int max_bounces = 4;           // Maximum bounce depth (0 = direct only).
	uint32_t sample_index = 0;     // Temporal sample index for RNG seeding.
	bool shadows_enabled = true;   // Whether to trace shadow rays for NEE.
	CameraParams camera;                // Camera transform + projection (for GPU Generate).
	ShadePass::EnvironmentData env;     // Sky, ambient, tone mapping (from WorldEnvironment).
	SceneShadeData shade;               // Material/UV/normal data (from build()).
	SceneLightData lights;              // All lights in the scene (from _resolve_all_lights()).
};

/// Abstract interface for multi-bounce path tracing.
///
/// Thread-safety: NOT thread-safe.  Only one thread may call trace_frame()
/// at a time (the Godot main thread via RayRenderer::render_frame).
///
/// WHY NOT a free function?
///   Implementations own internal reuse buffers (hits, shadow rays, path states).
///   A class keeps those alive across frames, avoiding per-frame allocation.
class IPathTracer {
public:
	virtual ~IPathTracer() = default;

	/// Trace a complete frame of path-traced color.
	///
	/// @param params       Frame parameters (resolution, bounces, scene data).
	/// @param primary_rays Camera rays (width*height).  The path tracer may
	///                     overwrite these for subsequent bounces — the caller
	///                     must treat them as consumed after this call.
	/// @param color_output RGBA float buffer (width*height*4), pre-allocated
	///                     by the caller.  Tone-mapped + gamma-corrected output.
	/// @param svc          Ray service for submitting trace queries.
	/// @param pool         Thread pool for parallel work dispatch.
	virtual void trace_frame(const PathTraceParams &params,
		Ray *primary_rays,
		float *color_output,
		IRayService *svc,
		IThreadDispatch *pool) = 0;
};
