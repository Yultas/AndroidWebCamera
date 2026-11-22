#pragma once

#include <string>
#include <vector>

namespace awc {

struct CodecInfo {
	std::string id;
	std::string name;
	bool supported = false;
	bool hardware = false;
	std::string stream_path;
	std::string encoder;
};

struct ResolutionInfo {
	int width = 0;
	int height = 0;
	std::vector<int> fps;
};

struct CameraInfo {
	std::string id;
	std::string name;
	std::string facing;
	bool has_flash = false;
	int sensor_orientation = 0;
	std::vector<ResolutionInfo> resolutions;
};

struct DeviceStatus {
	bool streaming = false;
	std::string camera_id;
	std::string camera_name;
	int width = 0;
	int height = 0;
	int fps = 0;
	int measured_fps = 0;
	std::string codec;
	std::string stream_path;
	int bitrate_kbps = 0;
	int jpeg_quality = 0;
	int port = 0;
	int rotation = 0;
	bool mirrored = false;
	bool torch_enabled = false;
	bool has_flash = false;
	int clients = 0;
	std::vector<CodecInfo> supported_codecs;
	std::vector<CameraInfo> cameras;
};

struct ConfigPatch {
	bool has_camera_id = false;
	std::string camera_id;

	bool has_width = false;
	int width = 0;

	bool has_height = false;
	int height = 0;

	bool has_fps = false;
	int fps = 0;

	bool has_codec = false;
	std::string codec;

	bool has_bitrate_kbps = false;
	int bitrate_kbps = 0;

	bool has_jpeg_quality = false;
	int jpeg_quality = 0;

	bool has_rotation = false;
	int rotation = 0;

	bool has_mirrored = false;
	bool mirrored = false;

	bool has_torch_enabled = false;
	bool torch_enabled = false;

	bool IsEmpty() const;
	std::string ToJson() const;
};

/** Canonicalizes codec name to "h264", "h265", "av1", or "mjpeg"; empty string if invalid. */
std::string NormalizeCodec(const std::string &codec);

/** Returns stream endpoint path ("/stream.h264", "/stream.h265", "/stream.av1", "/stream.mjpg"). */
std::string StreamPathForCodec(const std::string &codec);

/** Combines base URL and codec into full stream URL. */
std::string StreamUrlForCodec(const std::string &base_url, const std::string &codec);

/** Normalizes user-entered host/IP (strips scheme, trailing slash, optional :port override). */
std::string NormalizeHostInput(const std::string &raw_host, int *optional_port = nullptr);

/** Parses JSON response from /api/status or /api/config into DeviceStatus. */
bool ParseStatusJson(const std::string &json_str, DeviceStatus &out_status, std::string &out_error);

/** Cross-platform HTTP/1.1 client for AndroidWebCamera REST API. */
class HttpClient {
public:
	static bool GetStatus(const std::string &host, int port, int timeout_ms,
			      DeviceStatus &out_status, std::string &out_error);

	static bool UpdateConfig(const std::string &host, int port, const ConfigPatch &patch,
				 int timeout_ms, DeviceStatus &out_status, std::string &out_error);
};

} // namespace awc
