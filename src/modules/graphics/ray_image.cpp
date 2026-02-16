// ray_image.cpp — RayImage to_image conversion.

#include "modules/graphics/ray_image.h"
#include <algorithm>

Ref<Image> RayImage::to_image(Channel ch) const {
	if (pixel_count_ == 0) return Ref<Image>();

	// Create or recreate the cached image if resolution changed.
	if (cached_image_.is_null() || output_dirty_) {
		cached_image_ = Image::create_empty(width_, height_,
			false, Image::FORMAT_RGBA8);
		output_dirty_ = false;
	}

	// Direct write via ptrw() — zero-copy into the Image's byte buffer.
	uint8_t *dst = cached_image_->ptrw();
	const float *src = channels_[ch].data();

	for (int i = 0; i < pixel_count_; i++) {
		// Clamp [0, 1] → [0, 255].  The branch-free min/max compiles to
		// a handful of SSE instructions on x86.
		int base = i * 4;
		dst[base + 0] = static_cast<uint8_t>(
			std::min(255.0f, std::max(0.0f, src[base + 0] * 255.0f + 0.5f)));
		dst[base + 1] = static_cast<uint8_t>(
			std::min(255.0f, std::max(0.0f, src[base + 1] * 255.0f + 0.5f)));
		dst[base + 2] = static_cast<uint8_t>(
			std::min(255.0f, std::max(0.0f, src[base + 2] * 255.0f + 0.5f)));
		dst[base + 3] = static_cast<uint8_t>(
			std::min(255.0f, std::max(0.0f, src[base + 3] * 255.0f + 0.5f)));
	}

	return cached_image_;
}
