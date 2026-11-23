#include "vcam-convert.hpp"

#include <algorithm>
#include <cstring>

namespace awc::vcam {

namespace {

inline uint8_t ClampByte(int v)
{
	if (v < 0) return 0;
	if (v > 255) return 255;
	return static_cast<uint8_t>(v);
}

inline void YuvToBgrLimited(int y, int u, int v, uint8_t *bgr)
{
	const int c = y - 16;
	const int d = u - 128;
	const int e = v - 128;
	const int y298 = 298 * c;
	bgr[0] = ClampByte((y298 + 516 * d + 128) >> 8);              // B
	bgr[1] = ClampByte((y298 - 100 * d - 208 * e + 128) >> 8);    // G
	bgr[2] = ClampByte((y298 + 409 * e + 128) >> 8);              // R
}

inline void YuvToBgrFull(int y, int u, int v, uint8_t *bgr)
{
	const int d = u - 128;
	const int e = v - 128;
	const int y256 = 256 * y;
	bgr[0] = ClampByte((y256 + 454 * d + 128) >> 8);              // B
	bgr[1] = ClampByte((y256 - 88 * d - 183 * e + 128) >> 8);     // G
	bgr[2] = ClampByte((y256 + 359 * e + 128) >> 8);              // R
}

struct FitRect {
	uint32_t x = 0;
	uint32_t y = 0;
	uint32_t w = 0;
	uint32_t h = 0;
};

FitRect ComputeAspectFit(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h)
{
	FitRect r{};
	if (src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0) {
		return r;
	}
	const uint64_t lhs = static_cast<uint64_t>(src_w) * dst_h;
	const uint64_t rhs = static_cast<uint64_t>(dst_w) * src_h;
	if (lhs >= rhs) {
		// Fit by width (letterbox top/bottom)
		r.w = dst_w;
		r.h = static_cast<uint32_t>((static_cast<uint64_t>(dst_w) * src_h) / src_w);
		if (r.h < 2) r.h = 2;
		if (r.h > dst_h) r.h = dst_h;
		r.x = 0;
		r.y = (dst_h - r.h) / 2u;
	} else {
		// Fit by height (pillarbox left/right)
		r.h = dst_h;
		r.w = static_cast<uint32_t>((static_cast<uint64_t>(dst_h) * src_w) / src_h);
		if (r.w < 2) r.w = 2;
		if (r.w > dst_w) r.w = dst_w;
		r.w &= ~1u; // Keep even width for YUY2 alignment
		if (r.w == 0) r.w = 2;
		r.x = ((dst_w - r.w) / 2u) & ~1u;
		r.y = 0;
	}
	return r;
}

} // namespace

void ConvertI420ToRgb24(const uint8_t *i420, uint32_t src_w, uint32_t src_h, bool full_range,
			uint8_t *dst, uint32_t dst_w, uint32_t dst_h, uint32_t dst_stride,
			bool bottom_up)
{
	if (!i420 || !dst || src_w < 2 || src_h < 2 || dst_w < 2 || dst_h < 2 ||
	    dst_stride < dst_w * 3u) {
		return;
	}

	const uint32_t cw = src_w / 2u;
	const uint32_t ch = src_h / 2u;
	const uint8_t *plane_y = i420;
	const uint8_t *plane_u = plane_y + static_cast<size_t>(src_w) * src_h;
	const uint8_t *plane_v = plane_u + static_cast<size_t>(cw) * ch;

	if (src_w == dst_w && src_h == dst_h) {
		for (uint32_t y = 0; y < dst_h; ++y) {
			const uint32_t dst_row = bottom_up ? (dst_h - 1u - y) : y;
			uint8_t *out = dst + static_cast<size_t>(dst_row) * dst_stride;
			const uint8_t *py = plane_y + static_cast<size_t>(y) * src_w;
			const uint8_t *pu = plane_u + static_cast<size_t>(y >> 1) * cw;
			const uint8_t *pv = plane_v + static_cast<size_t>(y >> 1) * cw;

			if (full_range) {
				for (uint32_t x = 0; x < dst_w; x += 2) {
					const int u = pu[x >> 1];
					const int v = pv[x >> 1];
					YuvToBgrFull(py[x], u, v, out + x * 3u);
					YuvToBgrFull(py[x + 1], u, v, out + (x + 1u) * 3u);
				}
			} else {
				for (uint32_t x = 0; x < dst_w; x += 2) {
					const int u = pu[x >> 1];
					const int v = pv[x >> 1];
					YuvToBgrLimited(py[x], u, v, out + x * 3u);
					YuvToBgrLimited(py[x + 1], u, v, out + (x + 1u) * 3u);
				}
			}
		}
		return;
	}

	// Clear entire frame to black for letterbox/pillarbox regions.
	for (uint32_t row = 0; row < dst_h; ++row) {
		std::memset(dst + static_cast<size_t>(row) * dst_stride, 0, dst_w * 3u);
	}

	const FitRect fit = ComputeAspectFit(src_w, src_h, dst_w, dst_h);
	if (fit.w == 0 || fit.h == 0) {
		return;
	}

	for (uint32_t dy = 0; dy < fit.h; ++dy) {
		const uint32_t sy = static_cast<uint32_t>((static_cast<uint64_t>(dy) * src_h) / fit.h);
		const uint32_t y_pos = fit.y + dy;
		const uint32_t dst_row = bottom_up ? (dst_h - 1u - y_pos) : y_pos;
		uint8_t *out = dst + static_cast<size_t>(dst_row) * dst_stride + static_cast<size_t>(fit.x) * 3u;

		const uint8_t *py = plane_y + static_cast<size_t>(sy) * src_w;
		const uint8_t *pu = plane_u + static_cast<size_t>(sy >> 1) * cw;
		const uint8_t *pv = plane_v + static_cast<size_t>(sy >> 1) * cw;

		if (full_range) {
			for (uint32_t dx = 0; dx < fit.w; ++dx) {
				const uint32_t sx = static_cast<uint32_t>((static_cast<uint64_t>(dx) * src_w) / fit.w);
				YuvToBgrFull(py[sx], pu[sx >> 1], pv[sx >> 1], out + dx * 3u);
			}
		} else {
			for (uint32_t dx = 0; dx < fit.w; ++dx) {
				const uint32_t sx = static_cast<uint32_t>((static_cast<uint64_t>(dx) * src_w) / fit.w);
				YuvToBgrLimited(py[sx], pu[sx >> 1], pv[sx >> 1], out + dx * 3u);
			}
		}
	}
}

void ConvertI420ToYuy2(const uint8_t *i420, uint32_t src_w, uint32_t src_h,
		       uint8_t *dst, uint32_t dst_w, uint32_t dst_h, uint32_t dst_stride)
{
	if (!i420 || !dst || src_w < 2 || src_h < 2 || dst_w < 2 || dst_h < 2 ||
	    (dst_w & 1u) != 0 || dst_stride < dst_w * 2u) {
		return;
	}

	const uint32_t cw = src_w / 2u;
	const uint32_t ch = src_h / 2u;
	const uint8_t *plane_y = i420;
	const uint8_t *plane_u = plane_y + static_cast<size_t>(src_w) * src_h;
	const uint8_t *plane_v = plane_u + static_cast<size_t>(cw) * ch;

	if (src_w == dst_w && src_h == dst_h) {
		for (uint32_t y = 0; y < dst_h; ++y) {
			uint8_t *out = dst + static_cast<size_t>(y) * dst_stride;
			const uint8_t *py = plane_y + static_cast<size_t>(y) * src_w;
			const uint8_t *pu = plane_u + static_cast<size_t>(y >> 1) * cw;
			const uint8_t *pv = plane_v + static_cast<size_t>(y >> 1) * cw;

			for (uint32_t x = 0; x < dst_w; x += 2) {
				const uint32_t cx = x >> 1;
				out[x * 2u + 0u] = py[x];
				out[x * 2u + 1u] = pu[cx];
				out[x * 2u + 2u] = py[x + 1u];
				out[x * 2u + 3u] = pv[cx];
			}
		}
		return;
	}

	// Fill destination with neutral black in YUY2 (Y=16, U=128, V=128).
	for (uint32_t row = 0; row < dst_h; ++row) {
		uint8_t *out = dst + static_cast<size_t>(row) * dst_stride;
		for (uint32_t x = 0; x < dst_w; x += 2) {
			out[x * 2u + 0u] = 16;
			out[x * 2u + 1u] = 128;
			out[x * 2u + 2u] = 16;
			out[x * 2u + 3u] = 128;
		}
	}

	const FitRect fit = ComputeAspectFit(src_w, src_h, dst_w, dst_h);
	if (fit.w < 2 || fit.h == 0) {
		return;
	}

	for (uint32_t dy = 0; dy < fit.h; ++dy) {
		const uint32_t sy = static_cast<uint32_t>((static_cast<uint64_t>(dy) * src_h) / fit.h);
		uint8_t *out = dst + static_cast<size_t>(fit.y + dy) * dst_stride + static_cast<size_t>(fit.x) * 2u;
		const uint8_t *py = plane_y + static_cast<size_t>(sy) * src_w;
		const uint8_t *pu = plane_u + static_cast<size_t>(sy >> 1) * cw;
		const uint8_t *pv = plane_v + static_cast<size_t>(sy >> 1) * cw;

		for (uint32_t dx = 0; dx + 1u < fit.w; dx += 2) {
			const uint32_t sx0 = static_cast<uint32_t>((static_cast<uint64_t>(dx) * src_w) / fit.w);
			const uint32_t sx1 = static_cast<uint32_t>((static_cast<uint64_t>(dx + 1u) * src_w) / fit.w);
			out[dx * 2u + 0u] = py[sx0];
			out[dx * 2u + 1u] = pu[sx0 >> 1];
			out[dx * 2u + 2u] = py[sx1];
			out[dx * 2u + 3u] = pv[sx0 >> 1];
		}
	}
}

void RenderStandbyRgb24(uint8_t *dst, uint32_t width, uint32_t height, uint32_t stride,
			uint64_t frame_index, bool bottom_up)
{
	if (!dst || width < 2 || height < 2 || stride < width * 3u) {
		return;
	}
	const uint32_t bar_y = static_cast<uint32_t>((frame_index * 4u) % height);
	const uint32_t bar_h = std::max<uint32_t>(4u, height / 60u);

	for (uint32_t y = 0; y < height; ++y) {
		const uint32_t dst_row = bottom_up ? (height - 1u - y) : y;
		uint8_t *out = dst + static_cast<size_t>(dst_row) * stride;
		const bool in_bar = (y >= bar_y && y < bar_y + bar_h);
		const uint8_t b = in_bar ? 88 : static_cast<uint8_t>(26 + (y * 14u) / height);
		const uint8_t g = in_bar ? 62 : static_cast<uint8_t>(20 + (y * 10u) / height);
		const uint8_t r = in_bar ? 42 : static_cast<uint8_t>(18 + (y * 8u) / height);
		for (uint32_t x = 0; x < width; ++x) {
			out[x * 3u + 0u] = b;
			out[x * 3u + 1u] = g;
			out[x * 3u + 2u] = r;
		}
	}
}

void RenderStandbyYuy2(uint8_t *dst, uint32_t width, uint32_t height, uint32_t stride,
		       uint64_t frame_index)
{
	if (!dst || width < 2 || height < 2 || (width & 1u) != 0 || stride < width * 2u) {
		return;
	}
	const uint32_t bar_y = static_cast<uint32_t>((frame_index * 4u) % height);
	const uint32_t bar_h = std::max<uint32_t>(4u, height / 60u);

	for (uint32_t y = 0; y < height; ++y) {
		uint8_t *out = dst + static_cast<size_t>(y) * stride;
		const bool in_bar = (y >= bar_y && y < bar_y + bar_h);
		const uint8_t lum = in_bar ? 68 : static_cast<uint8_t>(24 + (y * 10u) / height);
		const uint8_t u = in_bar ? 148 : 132;
		const uint8_t v = in_bar ? 116 : 126;
		for (uint32_t x = 0; x < width; x += 2) {
			out[x * 2u + 0u] = lum;
			out[x * 2u + 1u] = u;
			out[x * 2u + 2u] = lum;
			out[x * 2u + 3u] = v;
		}
	}
}

} // namespace awc::vcam
