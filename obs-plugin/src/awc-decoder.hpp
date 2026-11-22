#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace awc {

struct DecodedFrameView {
	const uint8_t *y = nullptr;
	const uint8_t *u = nullptr;
	const uint8_t *v = nullptr;
	uint32_t stride_y = 0;
	uint32_t stride_u = 0;
	uint32_t stride_v = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	bool full_range = false;
	int64_t pts_us = 0;
};

/** Normalizes any angle in degrees to 0, 90, 180, or 270 (defaults to 0 if unsupported). */
int NormalizeRotationDegrees(int rotation_degrees);

/**
 * Applies clockwise rotation (0, 90, 180, 270) and optional horizontal mirror to a planar I420 frame.
 * When rotation == 0 and !mirror_horizontal, returns direct pointers into `src` with zero copies.
 * Otherwise writes packed I420 planes into `scratch` and returns pointers into `scratch`.
 */
DecodedFrameView TransformI420(const DecodedFrameView &src, int rotation_degrees,
			       bool mirror_horizontal, std::vector<uint8_t> &scratch);

/**
 * Low-latency FFmpeg network stream decoder for AndroidWebCamera endpoints:
 * - /stream.h264 (raw H.264 Annex-B)
 * - /stream.h265 (raw H.265/HEVC Annex-B)
 * - /stream.av1  (AV1 in IVF container)
 * - /stream.mjpg (multipart MJPEG)
 */
class AwcDecoder {
public:
	struct Options {
		std::string url;
		std::string codec = "h264";
		int open_timeout_ms = 4000;
		int read_timeout_ms = 3000;
		int initial_rotation = 0;
		bool initial_mirror = false;
	};

	using FrameCallback = std::function<void(const DecodedFrameView &frame)>;

	AwcDecoder() = default;
	~AwcDecoder() = default;

	AwcDecoder(const AwcDecoder &) = delete;
	AwcDecoder &operator=(const AwcDecoder &) = delete;

	void SetTransform(int rotation_degrees, bool mirror_horizontal);
	void RequestStop();
	bool StopRequested() const;

	/**
	 * Opens `options.url`, decodes frames until EOF, network timeout, error, or RequestStop().
	 * Returns true if stopped cleanly or reached clean EOF after decoding >= 1 frame.
	 */
	bool Run(const Options &options, const FrameCallback &on_frame, std::string &out_error,
		 int64_t *out_frames_decoded = nullptr);

private:
	std::atomic<bool> stop_requested_{false};
	std::atomic<int> rotation_degrees_{0};
	std::atomic<bool> mirror_horizontal_{false};
};

} // namespace awc
