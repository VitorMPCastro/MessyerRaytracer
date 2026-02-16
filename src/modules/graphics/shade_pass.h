#pragma once
// shade_pass.h — Per-pixel shading functions for AOV channels.
//
// All functions are inline and operate on a single Intersection + Ray pair.
// They write directly into the RayImage float buffer for zero overhead.
//
// No material system yet — these are procedural visualization modes similar
// to the existing debug draw modes but producing actual image data.
//
// ADDING A NEW CHANNEL:
//   1. Add an enum value in RayImage::Channel
//   2. Write a shade_xxx() function here
//   3. Call it from RayRenderer::_shade_results()

#include "core/intersection.h"
#include "core/ray.h"
#include "core/triangle_uv.h"
#include "modules/graphics/ray_image.h"
#include "modules/graphics/texture_sampler.h"
#include "api/scene_shade_data.h"

#include "core/asserts.h"

#include <cmath>
#include <cstdint>

namespace ShadePass {

// ========================================================================
// Normal visualization — map world-space normal to RGB [0,1]
// ========================================================================

inline void shade_normal(RayImage &fb, int idx, const Intersection &hit) {
	RT_ASSERT_BOUNDS(idx, fb.pixel_count());
	RT_ASSERT(fb.pixel_count() > 0, "shade_normal: framebuffer not initialized");

	if (!hit.hit()) {
		fb.write_pixel(RayImage::NORMAL, idx, 0.0f, 0.0f, 0.0f, 1.0f);
		return;
	}
	// Map [-1,1] → [0,1]
	float r = hit.normal.x * 0.5f + 0.5f;
	float g = hit.normal.y * 0.5f + 0.5f;
	float b = hit.normal.z * 0.5f + 0.5f;
	fb.write_pixel(RayImage::NORMAL, idx, r, g, b, 1.0f);
}

// ========================================================================
// Depth visualization — linear depth normalized to [0,1] range
// ========================================================================

inline void shade_depth(RayImage &fb, int idx, const Intersection &hit,
		float inv_depth_range) {
	if (!hit.hit()) {
		fb.write_pixel(RayImage::DEPTH, idx, 0.0f, 0.0f, 0.0f, 1.0f);
		return;
	}
	// inv_depth_range = 1.0 / (far - near), pre-computed per frame.
	float d = 1.0f - hit.t * inv_depth_range;
	d = (d < 0.0f) ? 0.0f : ((d > 1.0f) ? 1.0f : d);
	fb.write_pixel(RayImage::DEPTH, idx, d, d, d, 1.0f);
}

// ========================================================================
// Barycentric visualization — u, v, (1-u-v) as RGB
// ========================================================================

inline void shade_barycentric(RayImage &fb, int idx, const Intersection &hit) {
	if (!hit.hit()) {
		fb.write_pixel(RayImage::BARYCENTRIC, idx, 0.0f, 0.0f, 0.0f, 1.0f);
		return;
	}
	float w = 1.0f - hit.u - hit.v;
	fb.write_pixel(RayImage::BARYCENTRIC, idx, hit.u, hit.v, w, 1.0f);
}

// ========================================================================
// Flat color — simple Lambert shading with a fixed sun direction
// ========================================================================

inline void shade_flat(RayImage &fb, int idx, const Intersection &hit,
		const Vector3 &sun_dir) {
	if (!hit.hit()) {
		fb.write_pixel(RayImage::COLOR, idx, 0.05f, 0.05f, 0.08f, 1.0f);
		return;
	}
	// Lambert: max(0, N · L) with a small ambient term.
	float ndl = hit.normal.dot(sun_dir);
	ndl = (ndl < 0.0f) ? 0.0f : ndl;
	float ambient = 0.08f;
	float shade = ambient + ndl * 0.92f;
	// Slight warm tint: base color (0.85, 0.82, 0.78)
	fb.write_pixel(RayImage::COLOR, idx,
		shade * 0.85f, shade * 0.82f, shade * 0.78f, 1.0f);
}

// ========================================================================
// Material color — Lambert shading using extracted Godot material albedo
// ========================================================================

inline void shade_material(RayImage &fb, int idx, const Intersection &hit,
		const Vector3 &sun_dir, const SceneShadeData &shade_data) {
	if (!hit.hit()) {
		fb.write_pixel(RayImage::COLOR, idx, 0.05f, 0.05f, 0.08f, 1.0f);
		return;
	}
	// Look up material albedo for this triangle.
	float ar = 0.75f, ag = 0.75f, ab = 0.75f;
	if (shade_data.material_ids &&
			hit.prim_id < static_cast<uint32_t>(shade_data.triangle_count)) {
		uint32_t mat_id = shade_data.material_ids[hit.prim_id];
		if (mat_id < static_cast<uint32_t>(shade_data.material_count)) {
			const MaterialData &mat = shade_data.materials[mat_id];
			ar = mat.albedo.r;
			ag = mat.albedo.g;
			ab = mat.albedo.b;

			// Sample albedo texture if available (Phase 2).
			if (mat.has_albedo_texture && shade_data.triangle_uvs) {
				const TriangleUV &tri_uv = shade_data.triangle_uvs[hit.prim_id];
				Vector2 uv = tri_uv.interpolate(hit.u, hit.v);
				Color tex = TextureSampler::sample_bilinear(
						mat.albedo_texture.ptr(), uv.x, uv.y);
				ar *= tex.r;
				ag *= tex.g;
				ab *= tex.b;
			}
		}
	}
	// Lambert: N·L + ambient, modulated by material albedo.
	float ndl = hit.normal.dot(sun_dir);
	ndl = (ndl < 0.0f) ? 0.0f : ndl;
	float ambient = 0.08f;
	float shade = ambient + ndl * 0.92f;
	fb.write_pixel(RayImage::COLOR, idx, shade * ar, shade * ag, shade * ab, 1.0f);
}

// ========================================================================
// Albedo visualization — pure material color, no lighting
// ========================================================================

inline void shade_albedo(RayImage &fb, int idx, const Intersection &hit,
		const SceneShadeData &shade_data) {
	if (!hit.hit()) {
		fb.write_pixel(RayImage::ALBEDO, idx, 0.0f, 0.0f, 0.0f, 1.0f);
		return;
	}
	float r = 0.75f, g = 0.75f, b = 0.75f;
	if (shade_data.material_ids &&
			hit.prim_id < static_cast<uint32_t>(shade_data.triangle_count)) {
		uint32_t mat_id = shade_data.material_ids[hit.prim_id];
		if (mat_id < static_cast<uint32_t>(shade_data.material_count)) {
			const MaterialData &mat = shade_data.materials[mat_id];
			r = mat.albedo.r;
			g = mat.albedo.g;
			b = mat.albedo.b;

			// Sample albedo texture if available (Phase 2).
			if (mat.has_albedo_texture && shade_data.triangle_uvs) {
				const TriangleUV &tri_uv = shade_data.triangle_uvs[hit.prim_id];
				Vector2 uv = tri_uv.interpolate(hit.u, hit.v);
				Color tex = TextureSampler::sample_bilinear(
						mat.albedo_texture.ptr(), uv.x, uv.y);
				r *= tex.r;
				g *= tex.g;
				b *= tex.b;
			}
		}
	}
	fb.write_pixel(RayImage::ALBEDO, idx, r, g, b, 1.0f);
}

// ========================================================================
// World-space position — modulo mapping to [0,1] for visualization
// ========================================================================

inline void shade_position(RayImage &fb, int idx, const Intersection &hit,
		float inv_range) {
	if (!hit.hit()) {
		fb.write_pixel(RayImage::POSITION, idx, 0.0f, 0.0f, 0.0f, 1.0f);
		return;
	}
	// fmod to keep values in a visible range.  inv_range = 1.0 / range.
	auto wrap = [inv_range](float v) -> float {
		float f = v * inv_range;
		f = f - std::floor(f);  // fractional part [0,1)
		return f;
	};
	fb.write_pixel(RayImage::POSITION, idx,
		wrap(hit.position.x), wrap(hit.position.y), wrap(hit.position.z), 1.0f);
}

// ========================================================================
// Primitive ID — hash to a unique color per triangle
// ========================================================================

inline void shade_prim_id(RayImage &fb, int idx, const Intersection &hit) {
	RT_ASSERT_BOUNDS(idx, fb.pixel_count());
	RT_ASSERT(fb.pixel_count() > 0, "shade_prim_id: framebuffer not initialized");

	if (!hit.hit()) {
		fb.write_pixel(RayImage::PRIM_ID, idx, 0.0f, 0.0f, 0.0f, 1.0f);
		return;
	}
	// Simple hash: spread bits of prim_id into RGB.
	// Uses the "integer hash" trick for a well-distributed colormap.
	uint32_t h = hit.prim_id;
	h = ((h >> 16) ^ h) * 0x45d9f3bU;
	h = ((h >> 16) ^ h) * 0x45d9f3bU;
	h = (h >> 16) ^ h;

	float r = static_cast<float>((h >>  0) & 0xFF) / 255.0f;
	float g = static_cast<float>((h >>  8) & 0xFF) / 255.0f;
	float b = static_cast<float>((h >> 16) & 0xFF) / 255.0f;
	fb.write_pixel(RayImage::PRIM_ID, idx, r, g, b, 1.0f);
}

// ========================================================================
// Hit mask — white if hit, black background
// ========================================================================

inline void shade_hit_mask(RayImage &fb, int idx, const Intersection &hit) {
	float v = hit.hit() ? 1.0f : 0.0f;
	fb.write_pixel(RayImage::HIT_MASK, idx, v, v, v, 1.0f);
}

// ========================================================================
// Wireframe — highlight triangle edges using barycentric proximity
// ========================================================================
// Uses the common shader trick: if any barycentric coordinate (u, v, 1-u-v)
// is close to 0, the pixel is near an edge. The threshold controls the
// wire thickness; smoothstep gives anti-aliased edges.

inline void shade_wireframe(RayImage &fb, int idx, const Intersection &hit) {
	RT_ASSERT_BOUNDS(idx, fb.pixel_count());
	RT_ASSERT(fb.pixel_count() > 0, "shade_wireframe: framebuffer not initialized");

	if (!hit.hit()) {
		fb.write_pixel(RayImage::WIREFRAME, idx, 0.0f, 0.0f, 0.0f, 1.0f);
		return;
	}
	float w0 = 1.0f - hit.u - hit.v;
	float w1 = hit.u;
	float w2 = hit.v;
	// Distance to nearest edge = min of the three barycentric coords.
	float d = std::min({w0, w1, w2});
	// Smoothstep between 0.01 and 0.03 for anti-aliased lines.
	float lo = 0.01f, hi = 0.03f;
	float t = (d - lo) / (hi - lo);
	t = (t < 0.0f) ? 0.0f : ((t > 1.0f) ? 1.0f : t);
	float edge = 1.0f - t * t * (3.0f - 2.0f * t); // smoothstep inverse
	// Wire = white on dark gray background.
	float bg = 0.08f;
	float v = bg + edge * (1.0f - bg);
	fb.write_pixel(RayImage::WIREFRAME, idx, v, v, v, 1.0f);
}

// ========================================================================
// UV visualization — texture coordinates as Red/Green color
// ========================================================================
// Shows the UV parameterization of the mesh. Useful for spotting
// UV seams, stretching, and validating texture mapping.

inline void shade_uv(RayImage &fb, int idx, const Intersection &hit,
		const SceneShadeData &shade_data) {
	if (!hit.hit()) {
		fb.write_pixel(RayImage::UV, idx, 0.0f, 0.0f, 0.0f, 1.0f);
		return;
	}
	float r = 0.5f, g = 0.5f;
	if (shade_data.triangle_uvs &&
			hit.prim_id < static_cast<uint32_t>(shade_data.triangle_count)) {
		const TriangleUV &tri_uv = shade_data.triangle_uvs[hit.prim_id];
		Vector2 uv = tri_uv.interpolate(hit.u, hit.v);
		// Fractional part for tiling visualization.
		r = uv.x - std::floor(uv.x);
		g = uv.y - std::floor(uv.y);
	}
	fb.write_pixel(RayImage::UV, idx, r, g, 0.0f, 1.0f);
}

// ========================================================================
// Fresnel / facing ratio — |N · V| mapped to a blue-white gradient
// ========================================================================
// Shows the angle between surface normal and view direction.
// Surfaces facing the camera = bright, grazing angles = dark blue.
// Useful for debugging normals, detecting inverted faces, and previewing
// rim-light / Fresnel effects before full material shading.

inline void shade_fresnel(RayImage &fb, int idx, const Intersection &hit,
		const Ray &ray) {
	if (!hit.hit()) {
		fb.write_pixel(RayImage::FRESNEL, idx, 0.0f, 0.0f, 0.0f, 1.0f);
		return;
	}
	// View direction = -ray.direction (toward camera).
	Vector3 view_dir = -ray.direction;
	float n_dot_v = hit.normal.dot(view_dir);
	n_dot_v = (n_dot_v < 0.0f) ? 0.0f : ((n_dot_v > 1.0f) ? 1.0f : n_dot_v);
	// Map to a blue→white gradient: grazing = blue, facing = white.
	float r = n_dot_v;
	float g = n_dot_v;
	float b = 0.3f + n_dot_v * 0.7f;
	fb.write_pixel(RayImage::FRESNEL, idx, r, g, b, 1.0f);
}

// ========================================================================
// Shade ALL channels for a single pixel.
// This is the main entry point called per pixel from the render loop.
// One BVH traversal, 7 channel writes — the AOV pattern in action.
// ========================================================================

inline void shade_all(RayImage &fb, int idx, const Intersection &hit,
		const Ray &ray, const Vector3 &sun_dir,
		float inv_depth_range, float inv_pos_range,
		const SceneShadeData &shade_data) {
	shade_material(fb, idx, hit, sun_dir, shade_data);
	shade_normal(fb, idx, hit);
	shade_depth(fb, idx, hit, inv_depth_range);
	shade_barycentric(fb, idx, hit);
	shade_position(fb, idx, hit, inv_pos_range);
	shade_prim_id(fb, idx, hit);
	shade_hit_mask(fb, idx, hit);
	shade_albedo(fb, idx, hit, shade_data);
	shade_wireframe(fb, idx, hit);
	shade_uv(fb, idx, hit, shade_data);
	shade_fresnel(fb, idx, hit, ray);
}

// ========================================================================
// Shade a SINGLE channel for one pixel.
// When only one AOV is needed per frame, this is ~8× faster than shade_all.
// ========================================================================

inline void shade_channel(RayImage &fb, int idx, const Intersection &hit,
		const Ray &ray, const Vector3 &sun_dir,
		float inv_depth_range, float inv_pos_range,
		const SceneShadeData &shade_data,
		RayImage::Channel channel) {
	switch (channel) {
		case RayImage::COLOR:       shade_material(fb, idx, hit, sun_dir, shade_data); break;
		case RayImage::NORMAL:      shade_normal(fb, idx, hit); break;
		case RayImage::DEPTH:       shade_depth(fb, idx, hit, inv_depth_range); break;
		case RayImage::BARYCENTRIC: shade_barycentric(fb, idx, hit); break;
		case RayImage::POSITION:    shade_position(fb, idx, hit, inv_pos_range); break;
		case RayImage::PRIM_ID:     shade_prim_id(fb, idx, hit); break;
		case RayImage::HIT_MASK:    shade_hit_mask(fb, idx, hit); break;
		case RayImage::ALBEDO:      shade_albedo(fb, idx, hit, shade_data); break;
		case RayImage::WIREFRAME:   shade_wireframe(fb, idx, hit); break;
		case RayImage::UV:          shade_uv(fb, idx, hit, shade_data); break;
		case RayImage::FRESNEL:     shade_fresnel(fb, idx, hit, ray); break;
		default:                    shade_material(fb, idx, hit, sun_dir, shade_data); break;
	}
}

} // namespace ShadePass
