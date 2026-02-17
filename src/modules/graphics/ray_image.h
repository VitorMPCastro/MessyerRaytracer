#pragma once
// ray_image.h — Multi-channel AOV framebuffer for ray-traced rendering.
//
// DESIGN:
//   The frame buffer stores pixel data as 4-component floats (RGBA) per channel.
//   Multiple AOV channels (normal, depth, barycentric, etc.) can coexist.
//   At display time, the selected channel is tone-mapped to a Godot Image
//   (FORMAT_RGBA8) for GPU texture upload via ImageTexture::update().
//
//   Internal float storage avoids format conversion during the render loop.
//   Conversion to Image happens once at the end of the frame.
//
// MEMORY LAYOUT:
//   Each channel is a contiguous float array: [r0, g0, b0, a0, r1, g1, b1, a1, ...]
//   Stride = 4 floats per pixel. Total = width * height * 4 floats per channel.
//
// USAGE:
//   RayImage fb;
//   fb.resize(320, 240);
//   fb.clear();
//   float *color = fb.channel(RayImage::COLOR);
//   color[pixel_idx * 4 + 0] = r;
//   ...
//   Ref<Image> img = fb.to_image(RayImage::COLOR);

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/color.hpp>
#include "core/asserts.h"

#include <vector>
#include <cstring>
#include <algorithm>

using namespace godot;

class RayImage {
public:
	RayImage() = default;
	RayImage(const RayImage &) = delete;
	RayImage &operator=(const RayImage &) = delete;

	/// AOV channel identifiers. Each channel stores RGBA float data.
	enum Channel {
		COLOR       = 0,  ///< Shaded color (Lambert, etc.)
		NORMAL      = 1,  ///< World-space normal mapped to [0,1]
		DEPTH       = 2,  ///< Linear depth (alpha = 1 if hit)
		BARYCENTRIC = 3,  ///< u, v, 1-u-v as RGB
		POSITION    = 4,  ///< World-space position (modulo range)
		PRIM_ID     = 5,  ///< Hash of primitive ID → unique color
		HIT_MASK    = 6,  ///< White if hit, black if miss
		ALBEDO      = 7,  ///< Pure material color (no lighting)
		WIREFRAME   = 8,  ///< Triangle edges via barycentric proximity
		UV          = 9,  ///< Texture coordinates as RG color
		FRESNEL     = 10, ///< Facing ratio |N·V| (edge glow / silhouette)
		CHANNEL_COUNT
	};

	// ====================================================================
	// Lifecycle
	// ====================================================================

	/// Allocate (or reallocate) all channels for the given resolution.
	void resize(int width, int height) {
		RT_ASSERT(width > 0 && height > 0, "RayImage::resize: dimensions must be positive");
		RT_ASSERT(static_cast<int64_t>(width) * height <= (1 << 26),
			"RayImage::resize: resolution exceeds safe limit");

		if (width == width_ && height == height_) { return; }
		width_  = width;
		height_ = height;
		pixel_count_ = width * height;

		for (int ch = 0; ch < CHANNEL_COUNT; ch++) {
			channels_[ch].resize(static_cast<size_t>(pixel_count_) * 4);
		}

		// Lazily create the output image at first use (to_image).
		output_dirty_ = true;
	}

	/// Clear all channels to black (0, 0, 0, 0).
	void clear() {
		RT_ASSERT(pixel_count_ > 0, "clear: image not initialized");
		for (int ch = 0; ch < CHANNEL_COUNT; ch++) {
			RT_ASSERT(!channels_[ch].empty(), "clear: channel buffer not allocated");
			std::memset(channels_[ch].data(), 0,
				channels_[ch].size() * sizeof(float));
		}
	}

	/// Clear a single channel.
	void clear_channel(Channel ch) {
		std::memset(channels_[ch].data(), 0,
			channels_[ch].size() * sizeof(float));
	}

	// ====================================================================
	// Pixel access — inlined for hot-path shading
	// ====================================================================

	/// Get writable pointer to a channel's float data.
	/// Layout: [r0, g0, b0, a0, r1, g1, b1, a1, ...]
	float *channel(Channel ch) { return channels_[ch].data(); }
	const float *channel(Channel ch) const { return channels_[ch].data(); }

	/// Write a pixel to a channel. idx = y * width + x.
	inline void write_pixel(Channel ch, int idx, float r, float g, float b, float a = 1.0f) {
		RT_ASSERT_BOUNDS(idx, pixel_count_);
		RT_ASSERT(ch >= 0 && ch < CHANNEL_COUNT, "write_pixel: invalid channel");
		float *p = channels_[ch].data() + static_cast<ptrdiff_t>(idx) * 4;
		p[0] = r;
		p[1] = g;
		p[2] = b;
		p[3] = a;
	}

	/// Write a pixel from a Color.
	inline void write_pixel(Channel ch, int idx, const Color &c) {
		RT_ASSERT_BOUNDS(idx, pixel_count_);
		RT_ASSERT(ch >= 0 && ch < CHANNEL_COUNT, "write_pixel: invalid channel");
		float *p = channels_[ch].data() + static_cast<ptrdiff_t>(idx) * 4;
		p[0] = c.r;
		p[1] = c.g;
		p[2] = c.b;
		p[3] = c.a;
	}

	/// Read a pixel from a channel.
	inline Color read_pixel(Channel ch, int idx) const {
		const float *p = channels_[ch].data() + static_cast<ptrdiff_t>(idx) * 4;
		return Color(p[0], p[1], p[2], p[3]);
	}

	// ====================================================================
	// Conversion to Godot Image for display
	// ====================================================================

	/// Convert the specified channel to a Godot Image (FORMAT_RGBA8).
	/// Clamps float values to [0, 1] and maps to [0, 255].
	/// Reuses the Image allocation across frames for zero-alloc streaming.
	Ref<Image> to_image(Channel ch) const;

	// ====================================================================
	// Accessors
	// ====================================================================

	int width() const { return width_; }
	int height() const { return height_; }
	int pixel_count() const { return pixel_count_; }

private:
	int width_  = 0;
	int height_ = 0;
	int pixel_count_ = 0;

	std::vector<float> channels_[CHANNEL_COUNT];

	// Cached output image — reused across frames to avoid re-allocation.
	mutable Ref<Image> cached_image_;
	mutable bool output_dirty_ = true;
};
