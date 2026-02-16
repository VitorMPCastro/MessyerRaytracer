#pragma once
// texture_sampler.h — CPU texture sampling utilities.
//
// Provides nearest and bilinear sampling from a Godot Image.
// Used by the shade pass to map UV coordinates to texture colors.
//
// Performance note: Image::get_pixel() goes through Godot's binding layer.
// For a production offline renderer this would use raw pixel data.
// For our real-time preview at moderate resolutions this is adequate.

#include "core/asserts.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/color.hpp>
#include <cmath>

using godot::Image;
using godot::Color;

namespace TextureSampler {

/// Nearest-neighbor sampling with repeat wrapping.
inline Color sample_nearest(const Image *img, float u, float v) {
	RT_ASSERT_NOT_NULL(img);
	RT_ASSERT_FINITE(u);
	RT_ASSERT_FINITE(v);

	int w = img->get_width();
	int h = img->get_height();
	if (w <= 0 || h <= 0) { return Color(1.0f, 0.0f, 1.0f); } // magenta = error

	// Repeat wrap.
	u = u - std::floor(u);
	v = v - std::floor(v);

	int x = static_cast<int>(u * w) % w;
	int y = static_cast<int>(v * h) % h;
	if (x < 0) { x += w; }
	if (y < 0) { y += h; }

	return img->get_pixel(x, y);
}

/// Bilinear sampling with repeat wrapping.
inline Color sample_bilinear(const Image *img, float u, float v) {
	RT_ASSERT_NOT_NULL(img);
	RT_ASSERT_FINITE(u);
	RT_ASSERT_FINITE(v);

	int w = img->get_width();
	int h = img->get_height();
	if (w <= 0 || h <= 0) { return Color(1.0f, 0.0f, 1.0f); }

	// Repeat wrap.
	u = u - std::floor(u);
	v = v - std::floor(v);

	// Map to pixel coordinates (pixel centers at 0.5).
	float fx = u * w - 0.5f;
	float fy = v * h - 0.5f;

	int x0 = static_cast<int>(std::floor(fx));
	int y0 = static_cast<int>(std::floor(fy));

	float sx = fx - x0;
	float sy = fy - y0;

	// Wrap with repeat.
	auto wrap = [](int coord, int size) -> int {
		coord = coord % size;
		return (coord < 0) ? coord + size : coord;
	};

	int x1 = wrap(x0 + 1, w);
	int y1 = wrap(y0 + 1, h);
	x0 = wrap(x0, w);
	y0 = wrap(y0, h);

	Color c00 = img->get_pixel(x0, y0);
	Color c10 = img->get_pixel(x1, y0);
	Color c01 = img->get_pixel(x0, y1);
	Color c11 = img->get_pixel(x1, y1);

	// Bilinear interpolation.
	Color top = c00.lerp(c10, sx);
	Color bot = c01.lerp(c11, sx);
	return top.lerp(bot, sy);
}

} // namespace TextureSampler
