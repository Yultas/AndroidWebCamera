#pragma once

#include <cstdint>

namespace awc::vcam {

/** Computes the 4-byte aligned DIB row stride for 24-bit RGB (BGR24). */
inline uint32_t CalcRgb24Stride(uint32_t width)
{
	return ((width * 3u) + 3u) & ~3u;
}

/**
 * Converts tightly packed planar I420 to BGR24 (Windows MEDIASUBTYPE_RGB24).
 * - If `bottom_up` is true (standard DirectShow positive biHeight), row 0 in `dst` is the bottom scanline.
 * - If source and destination dimensions differ, preserves aspect ratio with black letterbox/pillarbox.
 */
void ConvertI420ToRgb24(const uint8_t *i420, uint32_t src_w, uint32_t src_h, bool full_range,
			uint8_t *dst, uint32_t dst_w, uint32_t dst_h, uint32_t dst_stride,
			bool bottom_up = true);

/**
 * Converts tightly packed planar I420 to packed YUY2 (Windows MEDIASUBTYPE_YUY2, top-down).
 * - `dst_w` must be even; `dst_stride` must be >= dst_w * 2.
 * - Preserves aspect ratio with black (Y=16, U=128, V=128) letterbox/pillarbox if sizes differ.
 */
void ConvertI420ToYuy2(const uint8_t *i420, uint32_t src_w, uint32_t src_h,
		       uint8_t *dst, uint32_t dst_w, uint32_t dst_h, uint32_t dst_stride);

/** Renders a clean dark standby frame (with subtle animated scan bar) in BGR24. */
void RenderStandbyRgb24(uint8_t *dst, uint32_t width, uint32_t height, uint32_t stride,
			uint64_t frame_index, bool bottom_up = true);

/** Renders a clean dark standby frame (with subtle animated scan bar) in YUY2. */
void RenderStandbyYuy2(uint8_t *dst, uint32_t width, uint32_t height, uint32_t stride,
		       uint64_t frame_index);

} // namespace awc::vcam
