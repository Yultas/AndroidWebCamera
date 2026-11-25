#pragma once

#include "awc-decoder.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace awc::vcam {

/**
 * Enhanced low-latency stream decoder for AndroidWebCamera Windows/Linux Virtual Camera Host:
 * - Framed `/ws/video` WebSocket client for H.264, H.265 (HEVC), and AV1:
 *   eliminates FFmpeg Annex-B start-code +1 frame holdback latency and preserves
 *   exact Camera2 hardware sensor timestamps (`ptsUs`).
 * - `avformat` HTTP/file fallback (used for MJPEG `/stream.mjpg` and local test streams)
 *   with a 4 MB `probesize` so 4K @ 30-40 Mbps keyframes never truncate on startup.
 * - Optional GPU hardware decoding (`auto`, `d3d11va`, `dxva2`, `cuda`, `qsv`, `vaapi`, or `off`)
 *   with fast `NV12 -> I420` plane conversion and automatic fallback to CPU decoding.
 */
class VcamDecoder {
public:
	struct Options {
		std::string host;
		int port = 8080;
		std::string url;
		std::string codec = "h264";
		std::string hwaccel = "auto"; // "auto", "off", "d3d11va", "dxva2", "cuda", "qsv", "vaapi"
		bool prefer_ws_video = true;
		int open_timeout_ms = 4000;
		int read_timeout_ms = 3000;
		int initial_rotation = 0;
		bool initial_mirror = false;
	};

	using FrameCallback = std::function<void(const awc::DecodedFrameView &frame)>;

	VcamDecoder() = default;
	~VcamDecoder() = default;

	VcamDecoder(const VcamDecoder &) = delete;
	VcamDecoder &operator=(const VcamDecoder &) = delete;

	void SetTransform(int rotation_degrees, bool mirror_horizontal);
	void RequestStop();
	bool StopRequested() const;

	/** Returns a short label describing the active decoder backend (e.g. "ws/video + d3d11va" or "http + cpu"). */
	std::string ActiveBackendLabel() const;

	bool Run(const Options &options, const FrameCallback &on_frame, std::string &out_error,
		 int64_t *out_frames_decoded = nullptr);

private:
	bool RunWebSocketVideo(const Options &options, const FrameCallback &on_frame,
			       std::string &out_error, int64_t *out_frames_decoded);

	bool RunAvFormatStream(const Options &options, const FrameCallback &on_frame,
			       std::string &out_error, int64_t *out_frames_decoded);

	std::atomic<bool> stop_requested_{false};
	std::atomic<int> rotation_degrees_{0};
	std::atomic<bool> mirror_horizontal_{false};
	std::string active_backend_;
};

} // namespace awc::vcam
