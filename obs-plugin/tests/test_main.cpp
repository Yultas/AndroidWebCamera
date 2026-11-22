#include "awc-adb.hpp"
#include "awc-client.hpp"
#include "awc-decoder.hpp"
#include "mock_obs.hpp"

extern "C" {
void obs_module_set_pointer(obs_module_t *module);
uint32_t obs_module_ver(void);
}

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <direct.h>
#include <io.h>
#include <winsock2.h>
#include <ws2tcpip.h>
using SockType = SOCKET;
static constexpr SockType kInvalidSock = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SockType = int;
static constexpr SockType kInvalidSock = -1;
#endif

namespace {

#define CHECK(cond)                                                                             \
	do {                                                                                    \
		if (!(cond)) {                                                                  \
			std::cerr << "FAIL at " << __FILE__ << ":" << __LINE__ << ": " << #cond \
				  << std::endl;                                                 \
			std::exit(1);                                                           \
		}                                                                               \
	} while (0)

void CloseSock(SockType s)
{
	if (s == kInvalidSock)
		return;
#ifdef _WIN32
	closesocket(s);
#else
	close(s);
#endif
}

struct TestHttpResponse {
	int status_code = 200;
	std::string content_type = "application/json";
	std::string body;
};

class LocalTestServer {
public:
	using Handler = std::function<TestHttpResponse(const std::string &method,
						       const std::string &path,
						       const std::string &req_body)>;

	explicit LocalTestServer(Handler handler) : handler_(std::move(handler))
	{
#ifdef _WIN32
		WSADATA wsa{};
		WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
		listen_sock_ = socket(AF_INET, SOCK_STREAM, 0);
		CHECK(listen_sock_ != kInvalidSock);

		int yes = 1;
		setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR,
			   reinterpret_cast<const char *>(&yes), sizeof(yes));

		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = 0;
		CHECK(bind(listen_sock_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
		CHECK(listen(listen_sock_, 16) == 0);

		sockaddr_in bound{};
#ifdef _WIN32
		int len = sizeof(bound);
#else
		socklen_t len = sizeof(bound);
#endif
		CHECK(getsockname(listen_sock_, reinterpret_cast<sockaddr *>(&bound), &len) == 0);
		port_ = ntohs(bound.sin_port);

		thread_ = std::thread(&LocalTestServer::ServeLoop, this);
	}

	~LocalTestServer()
	{
		stop_.store(true);
		if (listen_sock_ != kInvalidSock) {
#ifdef _WIN32
			shutdown(listen_sock_, SD_BOTH);
#else
			shutdown(listen_sock_, SHUT_RDWR);
#endif
			CloseSock(listen_sock_);
			listen_sock_ = kInvalidSock;
		}
		if (thread_.joinable())
			thread_.join();
	}

	int Port() const { return port_; }
	std::string BaseUrl() const { return "http://127.0.0.1:" + std::to_string(port_); }

private:
	void ServeLoop()
	{
		while (!stop_.load()) {
			sockaddr_in cli{};
#ifdef _WIN32
			int cli_len = sizeof(cli);
#else
			socklen_t cli_len = sizeof(cli);
#endif
			SockType c =
				accept(listen_sock_, reinterpret_cast<sockaddr *>(&cli), &cli_len);
			if (c == kInvalidSock)
				break;
			std::thread([this, c]() { HandleClient(c); }).detach();
		}
	}

	void HandleClient(SockType c)
	{
		std::string req;
		char buf[4096];
		size_t hdr_end = std::string::npos;
		while (true) {
			int n = recv(c, buf, sizeof(buf), 0);
			if (n <= 0)
				break;
			req.append(buf, static_cast<size_t>(n));
			hdr_end = req.find("\r\n\r\n");
			if (hdr_end != std::string::npos)
				break;
		}
		if (hdr_end == std::string::npos) {
			CloseSock(c);
			return;
		}

		std::string hdrs = req.substr(0, hdr_end);
		std::string body = req.substr(hdr_end + 4);

		std::istringstream first_line(hdrs.substr(0, hdrs.find('\r')));
		std::string method, path;
		first_line >> method >> path;

		size_t content_len = 0;
		std::istringstream hstream(hdrs);
		std::string line;
		while (std::getline(hstream, line)) {
			if (line.rfind("Content-Length:", 0) == 0 ||
			    line.rfind("content-length:", 0) == 0) {
				content_len = static_cast<size_t>(
					std::strtoul(line.c_str() + 15, nullptr, 10));
			}
		}
		while (body.size() < content_len) {
			int n = recv(c, buf, sizeof(buf), 0);
			if (n <= 0)
				break;
			body.append(buf, static_cast<size_t>(n));
		}
		if (body.size() > content_len)
			body.resize(content_len);

		TestHttpResponse resp = handler_(method, path, body);
		std::ostringstream out;
		out << "HTTP/1.1 " << resp.status_code << (resp.status_code == 200 ? " OK" : " Error")
		    << "\r\nContent-Type: " << resp.content_type
		    << "\r\nContent-Length: " << resp.body.size() << "\r\nConnection: close\r\n\r\n";
		std::string hdr_bytes = out.str();
		send(c, hdr_bytes.data(), static_cast<int>(hdr_bytes.size()), 0);
		if (!resp.body.empty()) {
			size_t sent = 0;
			while (sent < resp.body.size()) {
				int n = send(c, resp.body.data() + sent,
					     static_cast<int>(resp.body.size() - sent), 0);
				if (n <= 0)
					break;
				sent += static_cast<size_t>(n);
			}
		}
		CloseSock(c);
	}

	Handler handler_;
	SockType listen_sock_ = kInvalidSock;
	int port_ = 0;
	std::atomic<bool> stop_{false};
	std::thread thread_;
};

std::string RunCommandCaptureBinary(const std::string &cmd)
{
#ifdef _WIN32
	FILE *fp = _popen(cmd.c_str(), "rb");
#else
	FILE *fp = popen(cmd.c_str(), "r");
#endif
	if (!fp)
		return "";
	std::string out;
	char buf[4096];
	while (true) {
		size_t n = std::fread(buf, 1, sizeof(buf), fp);
		if (n > 0)
			out.append(buf, n);
		if (n < sizeof(buf))
			break;
	}
#ifdef _WIN32
	int rc = _pclose(fp);
#else
	int rc = pclose(fp);
#endif
	if (rc != 0)
		return "";
	return out;
}

void TestCodecNormalizationAndHostHelpers()
{
	CHECK(awc::NormalizeCodec("h264") == "h264");
	CHECK(awc::NormalizeCodec(" AVC ") == "h264");
	CHECK(awc::NormalizeCodec("h265") == "h265");
	CHECK(awc::NormalizeCodec("HEVC") == "h265");
	CHECK(awc::NormalizeCodec("av1") == "av1");
	CHECK(awc::NormalizeCodec("AV01") == "av1");
	CHECK(awc::NormalizeCodec("mjpeg") == "mjpeg");
	CHECK(awc::NormalizeCodec("mjpg") == "mjpeg");
	CHECK(awc::NormalizeCodec("vp9") == "");

	CHECK(awc::StreamUrlForCodec("http://127.0.0.1:8080/", "h264") ==
	      "http://127.0.0.1:8080/stream.h264");
	CHECK(awc::StreamUrlForCodec("http://127.0.0.1:8080", "hevc") ==
	      "http://127.0.0.1:8080/stream.h265");
	CHECK(awc::StreamUrlForCodec("http://127.0.0.1:8080", "av1") ==
	      "http://127.0.0.1:8080/stream.av1");
	CHECK(awc::StreamUrlForCodec("http://127.0.0.1:8080", "mjpg") ==
	      "http://127.0.0.1:8080/stream.mjpg");

	int port = 8080;
	CHECK(awc::NormalizeHostInput("http://192.168.1.42:9090/", &port) == "192.168.1.42");
	CHECK(port == 9090);
	CHECK(awc::NormalizeHostInput("[fe80::1]:8181", &port) == "fe80::1");
	CHECK(port == 8181);
}

void TestJsonStatusAndConfigPatch()
{
	const std::string status_json = R"({
		"streaming": true,
		"cameraId": "0",
		"cameraName": "\u041e\u0441\u043d\u043e\u0432\u043d\u0430\u044f",
		"width": 1920,
		"height": 1080,
		"fps": 30,
		"measuredFps": 29,
		"codec": "h265",
		"streamPath": "/stream.h265",
		"bitrateKbps": 6000,
		"jpegQuality": 85,
		"port": 8080,
		"rotation": 90,
		"mirrored": true,
		"torchEnabled": true,
		"hasFlash": true,
		"clients": 2,
		"supportedCodecs": [
			{"id":"h264","name":"H.264","supported":true,"hardware":true,"streamPath":"/stream.h264","encoder":"c2.qti.avc.encoder"},
			{"id":"h265","name":"H.265","supported":true,"hardware":true,"streamPath":"/stream.h265","encoder":"c2.qti.hevc.encoder"}
		],
		"cameras": [
			{
				"id":"0",
				"name":"Back Main",
				"facing":"back",
				"hasFlash":true,
				"sensorOrientation":90,
				"resolutions":[
					{"width":1920,"height":1080,"fps":[30,60]},
					{"width":1280,"height":720,"fps":[15,30]}
				]
			}
		]
	})";

	awc::DeviceStatus st;
	std::string err;
	CHECK(awc::ParseStatusJson(status_json, st, err));
	CHECK(st.streaming);
	CHECK(st.camera_id == "0");
	CHECK(st.camera_name == "Основная");
	CHECK(st.width == 1920 && st.height == 1080);
	CHECK(st.fps == 30 && st.measured_fps == 29);
	CHECK(st.codec == "h265");
	CHECK(st.bitrate_kbps == 6000);
	CHECK(st.rotation == 90);
	CHECK(st.mirrored);
	CHECK(st.torch_enabled);
	CHECK(st.has_flash);
	CHECK(st.supported_codecs.size() == 2);
	CHECK(st.supported_codecs[1].hardware);
	CHECK(st.cameras.size() == 1);
	CHECK(st.cameras[0].resolutions.size() == 2);
	CHECK(st.cameras[0].resolutions[0].fps.size() == 2);
	CHECK(st.cameras[0].resolutions[0].fps[1] == 60);

	awc::ConfigPatch patch;
	CHECK(patch.IsEmpty());
	patch.has_camera_id = true;
	patch.camera_id = "1";
	patch.has_codec = true;
	patch.codec = "h264";
	patch.has_torch_enabled = true;
	patch.torch_enabled = true;
	CHECK(!patch.IsEmpty());
	const std::string encoded = patch.ToJson();
	CHECK(encoded.find("\"cameraId\":\"1\"") != std::string::npos);
	CHECK(encoded.find("\"codec\":\"h264\"") != std::string::npos);
	CHECK(encoded.find("\"torchEnabled\":true") != std::string::npos);
}

void TestHttpClientRequests()
{
	std::string last_post_body;
	LocalTestServer srv([&](const std::string &method, const std::string &path,
				const std::string &req_body) -> TestHttpResponse {
		if (method == "GET" && path == "/api/status") {
			return {200, "application/json",
				R"({"streaming":true,"cameraId":"0","width":1280,"height":720,"fps":30,"codec":"h264"})"};
		}
		if (method == "POST" && path == "/api/config") {
			last_post_body = req_body;
			return {200, "application/json",
				R"({"streaming":true,"cameraId":"1","width":1920,"height":1080,"fps":60,"codec":"h265","torchEnabled":true})"};
		}
		return {400, "application/json", R"({"error":"bad test request"})"};
	});

	awc::DeviceStatus st;
	std::string err;
	CHECK(awc::HttpClient::GetStatus("127.0.0.1", srv.Port(), 2000, st, err));
	CHECK(st.streaming && st.width == 1280 && st.codec == "h264");

	awc::ConfigPatch patch;
	patch.has_camera_id = true;
	patch.camera_id = "1";
	patch.has_codec = true;
	patch.codec = "h265";
	patch.has_torch_enabled = true;
	patch.torch_enabled = true;

	awc::DeviceStatus updated;
	CHECK(awc::HttpClient::UpdateConfig("127.0.0.1", srv.Port(), patch, 2000, updated, err));
	CHECK(updated.camera_id == "1" && updated.codec == "h265" && updated.torch_enabled);
	CHECK(last_post_body.find("\"codec\":\"h265\"") != std::string::npos);
}

void TestAdbParsingAndRunner()
{
	const std::string sample =
		"* daemon not running; starting now at tcp:5037\n"
		"List of devices attached\n"
		"ABC12345               device usb:1-2 product:panther model:Pixel_7 transport_id:1\n"
		"XYZ99999               unauthorized usb:1-3\n";
	auto devs = awc::ParseAdbDevices(sample);
	CHECK(devs.size() == 2);
	CHECK(devs[0].serial == "ABC12345" && devs[0].state == "device" &&
	      devs[0].model == "Pixel_7");
	CHECK(devs[1].serial == "XYZ99999" && devs[1].state == "unauthorized");

	awc::AdbDevice picked;
	std::string err;
	CHECK(awc::PickOnlineDevice(devs, "", picked, err));
	CHECK(picked.serial == "ABC12345");

	CHECK(!awc::PickOnlineDevice(devs, "XYZ99999", picked, err));
	CHECK(err.find("unauthorized") != std::string::npos);

	std::vector<awc::AdbDevice> multi = {{"DEV1", "device", "A"}, {"DEV2", "device", "B"}};
	CHECK(!awc::PickOnlineDevice(multi, "", picked, err));
	CHECK(awc::PickOnlineDevice(multi, "DEV2", picked, err));
	CHECK(picked.serial == "DEV2");

#ifndef _WIN32
	// Create temporary executable mock `adb` script to verify real process invocation.
	std::string tmp_dir = "/tmp/awc-obs-test-adb-" + std::to_string(getpid());
	mkdir(tmp_dir.c_str(), 0700);
	std::string script_path = tmp_dir + "/adb";
	std::string log_path = tmp_dir + "/adb.log";
	{
		std::ofstream ofs(script_path);
		ofs << "#!/bin/sh\n"
		    << "echo \"$@\" >> \"" << log_path << "\"\n"
		    << "if [ \"$1\" = \"devices\" ]; then\n"
		    << "  echo \"List of devices attached\"\n"
		    << "  echo \"MOCK1234\tdevice model:TestPhone\"\n"
		    << "fi\n"
		    << "exit 0\n";
	}
	chmod(script_path.c_str(), 0755);

	std::string find_err;
	CHECK(awc::FindAdbExecutable(script_path, find_err) == script_path);

	awc::AdbManager mgr(script_path);
	std::string resolved;
	CHECK(mgr.SetupForward("", 8080, 8080, resolved, err, 2000));
	CHECK(resolved == "MOCK1234");
	CHECK(mgr.RemoveForward(resolved, 8080, err, 2000));

	std::ifstream log_in(log_path);
	std::string log_all((std::istreambuf_iterator<char>(log_in)),
			    std::istreambuf_iterator<char>());
	CHECK(log_all.find("devices -l") != std::string::npos);
	CHECK(log_all.find("-s MOCK1234 forward tcp:8080 tcp:8080") != std::string::npos);
	CHECK(log_all.find("-s MOCK1234 forward --remove tcp:8080") != std::string::npos);

	unlink(log_path.c_str());
	unlink(script_path.c_str());
	rmdir(tmp_dir.c_str());
#endif
}

void TestTransformI420()
{
	// 4x2 Y plane, 2x1 U/V planes
	uint8_t y[8] = {1, 2, 3, 4, 5, 6, 7, 8};
	uint8_t u[2] = {10, 20};
	uint8_t v[2] = {30, 40};

	awc::DecodedFrameView src;
	src.y = y;
	src.u = u;
	src.v = v;
	src.stride_y = 4;
	src.stride_u = 2;
	src.stride_v = 2;
	src.width = 4;
	src.height = 2;
	src.full_range = true;

	std::vector<uint8_t> scratch;
	// 0 deg, no mirror -> zero-copy
	auto v0 = awc::TransformI420(src, 0, false, scratch);
	CHECK(v0.y == y && v0.width == 4 && v0.height == 2);

	// 0 deg + mirror
	auto v0m = awc::TransformI420(src, 0, true, scratch);
	CHECK(v0m.width == 4 && v0m.height == 2);
	CHECK(v0m.y[0] == 4 && v0m.y[1] == 3 && v0m.y[2] == 2 && v0m.y[3] == 1);
	CHECK(v0m.y[4] == 8 && v0m.y[7] == 5);
	CHECK(v0m.u[0] == 20 && v0m.u[1] == 10);

	// 90 deg CW: 4x2 -> 2x4
	// Row 0: [5, 1], Row 1: [6, 2], Row 2: [7, 3], Row 3: [8, 4]
	auto v90 = awc::TransformI420(src, 90, false, scratch);
	CHECK(v90.width == 2 && v90.height == 4);
	CHECK(v90.y[0] == 5 && v90.y[1] == 1);
	CHECK(v90.y[2] == 6 && v90.y[3] == 2);
	CHECK(v90.y[4] == 7 && v90.y[5] == 3);
	CHECK(v90.y[6] == 8 && v90.y[7] == 4);

	// 90 deg CW + mirror: Row 0: [1, 5], Row 1: [2, 6]...
	auto v90m = awc::TransformI420(src, 90, true, scratch);
	CHECK(v90m.width == 2 && v90m.height == 4);
	CHECK(v90m.y[0] == 1 && v90m.y[1] == 5);
	CHECK(v90m.y[6] == 4 && v90m.y[7] == 8);

	// 180 deg: Row 0: [8, 7, 6, 5], Row 1: [4, 3, 2, 1]
	auto v180 = awc::TransformI420(src, 180, false, scratch);
	CHECK(v180.width == 4 && v180.height == 2);
	CHECK(v180.y[0] == 8 && v180.y[3] == 5 && v180.y[4] == 4 && v180.y[7] == 1);

	// 270 deg CW (90 CCW): Row 0: [4, 8], Row 1: [3, 7], Row 2: [2, 6], Row 3: [1, 5]
	auto v270 = awc::TransformI420(src, 270, false, scratch);
	CHECK(v270.width == 2 && v270.height == 4);
	CHECK(v270.y[0] == 4 && v270.y[1] == 8);
	CHECK(v270.y[6] == 1 && v270.y[7] == 5);
}

struct EncodedStreams {
	std::string h264;
	std::string h265;
	std::string av1_ivf;
	std::string mjpeg_multipart;
};

EncodedStreams GenerateTestStreamsIfFfmpegAvailable()
{
	EncodedStreams streams;
#ifdef _WIN32
	std::string check = RunCommandCaptureBinary("where ffmpeg >nul 2>nul && echo OK");
#else
	std::string check = RunCommandCaptureBinary("which ffmpeg >/dev/null 2>&1 && echo OK");
#endif
	if (check.find("OK") == std::string::npos)
		return streams;

	streams.h264 = RunCommandCaptureBinary(
		"ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc=size=320x240:rate=15 "
		"-t 0.4 -c:v libx264 -profile:v baseline -pix_fmt yuv420p -f h264 pipe:1");
	streams.h265 = RunCommandCaptureBinary(
		"ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc=size=320x240:rate=15 "
		"-t 0.4 -c:v libx265 -pix_fmt yuv420p -f hevc pipe:1");
	streams.av1_ivf = RunCommandCaptureBinary(
		"ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc=size=320x240:rate=15 "
		"-t 0.4 -c:v libaom-av1 -cpu-used 8 -usage realtime -pix_fmt yuv420p -f ivf pipe:1");
	std::string jpeg = RunCommandCaptureBinary(
		"ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc=size=320x240:rate=15 "
		"-frames:v 1 -f image2 -vcodec mjpeg pipe:1");

	if (!jpeg.empty()) {
		std::ostringstream oss;
		for (int i = 0; i < 4; ++i) {
			oss << "--awcframe\r\nContent-Type: image/jpeg\r\nContent-Length: "
			    << jpeg.size() << "\r\n\r\n"
			    << jpeg << "\r\n";
		}
		streams.mjpeg_multipart = oss.str();
	}
	return streams;
}

void TestDecoderAndObsLifecycle()
{
	const EncodedStreams streams = GenerateTestStreamsIfFfmpegAvailable();
	if (streams.h264.empty()) {
		std::cout << "[WARN] ffmpeg CLI not found; skipping live FFmpeg stream decode test"
			  << std::endl;
		return;
	}

	std::mutex srv_mutex;
	std::string active_codec = "h264";
	bool torch_on = false;
	bool config_posted = false;

	LocalTestServer srv([&](const std::string &method, const std::string &path,
				const std::string &req_body) -> TestHttpResponse {
		if (method == "GET" && path == "/api/status") {
			std::lock_guard<std::mutex> lock(srv_mutex);
			std::ostringstream oss;
			oss << R"({"streaming":true,"cameraId":"0","cameraName":"Back","width":320,"height":240,"fps":15,"codec":")"
			    << active_codec << R"(","rotation":0,"torchEnabled":)"
			    << (torch_on ? "true" : "false")
			    << R"(,"hasFlash":true,"cameras":[{"id":"0","name":"Back","facing":"back","hasFlash":true,"resolutions":[{"width":320,"height":240,"fps":[15,30]}]}],"supportedCodecs":[{"id":"h264","name":"H.264","supported":true,"hardware":true},{"id":"h265","name":"H.265","supported":true,"hardware":true},{"id":"av1","name":"AV1","supported":true,"hardware":false},{"id":"mjpeg","name":"MJPEG","supported":true,"hardware":false}]})";
			return {200, "application/json", oss.str()};
		}
		if (method == "POST" && path == "/api/config") {
			std::lock_guard<std::mutex> lock(srv_mutex);
			config_posted = true;
			if (req_body.find("\"torchEnabled\":true") != std::string::npos)
				torch_on = true;
			std::ostringstream oss;
			oss << R"({"streaming":true,"cameraId":"0","cameraName":"Back","width":320,"height":240,"fps":15,"codec":")"
			    << active_codec << R"(","rotation":0,"torchEnabled":)"
			    << (torch_on ? "true" : "false") << "}";
			return {200, "application/json", oss.str()};
		}
		if (method == "GET" && path == "/stream.h264")
			return {200, "video/h264", streams.h264};
		if (method == "GET" && path == "/stream.h265")
			return {200, "video/hevc", streams.h265};
		if (method == "GET" && path == "/stream.av1")
			return {200, "video/x-ivf", streams.av1_ivf};
		if (method == "GET" && path == "/stream.mjpg")
			return {200, "multipart/x-mixed-replace; boundary=awcframe",
				streams.mjpeg_multipart};
		return {404, "application/json", R"({"error":"not found"})"};
	});

	// 1. Test AwcDecoder directly across all 4 codecs
	struct CodecCase {
		std::string codec;
		std::string path;
		int rot;
		bool mirror;
		uint32_t expected_w;
		uint32_t expected_h;
		bool available;
	};
	std::vector<CodecCase> cases = {
		{"h264", "/stream.h264", 0, false, 320, 240, !streams.h264.empty()},
		{"h265", "/stream.h265", 90, true, 240, 320, !streams.h265.empty()},
		{"av1", "/stream.av1", 180, false, 320, 240, !streams.av1_ivf.empty()},
		{"mjpeg", "/stream.mjpg", 270, false, 240, 320, !streams.mjpeg_multipart.empty()},
	};

	for (const auto &tc : cases) {
		if (!tc.available) {
			std::cout << "[WARN] Skipping codec " << tc.codec
				  << " (encoder unavailable in ffmpeg binary)" << std::endl;
			continue;
		}
		awc::AwcDecoder dec;
		awc::AwcDecoder::Options opts;
		opts.url = srv.BaseUrl() + tc.path;
		opts.codec = tc.codec;
		opts.initial_rotation = tc.rot;
		opts.initial_mirror = tc.mirror;

		uint32_t got_w = 0, got_h = 0;
		int64_t frames = 0;
		std::string dec_err;
		bool ok = dec.Run(
			opts,
			[&](const awc::DecodedFrameView &f) {
				got_w = f.width;
				got_h = f.height;
				CHECK(f.y != nullptr && f.u != nullptr && f.v != nullptr);
			},
			dec_err, &frames);
		if (!ok) {
			std::cerr << "Decoder failed for " << tc.codec << ": " << dec_err
				  << std::endl;
		}
		CHECK(ok);
		CHECK(frames >= 3);
		CHECK(got_w == tc.expected_w && got_h == tc.expected_h);
		std::cout << "  [OK] AwcDecoder(" << tc.codec << ") decoded " << frames
			  << " frames (" << got_w << "x" << got_h << ")" << std::endl;
	}

	// 2. Test full OBS module & source lifecycle via mock_obs
	mock_obs::Reset();
	obs_module_set_pointer(nullptr);
	CHECK(obs_module_ver() == LIBOBS_API_VER);
	CHECK(std::strcmp(obs_module_name(), "AndroidWebCamera") == 0);
	CHECK(obs_module_load());
	CHECK(mock_obs::HasRegisteredSource());

	const auto &info = mock_obs::GetRegisteredSource();
	CHECK(std::strcmp(info.id, "android_webcamera_source") == 0);
	CHECK(info.type == OBS_SOURCE_TYPE_INPUT);
	CHECK(info.output_flags == (OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE));
	CHECK(info.icon_type == OBS_ICON_TYPE_CAMERA);
	CHECK(std::strcmp(info.get_name(nullptr), "AndroidWebCamera") == 0);

	obs_data_t *settings = mock_obs::CreateData();
	info.get_defaults(settings);
	CHECK(std::strcmp(obs_data_get_string(settings, "connection_mode"), "usb") == 0);

	obs_data_set_string(settings, "connection_mode", "wifi");
	obs_data_set_string(settings, "wifi_host", "127.0.0.1");
	obs_data_set_int(settings, "remote_port", srv.Port());
	obs_data_set_bool(settings, "apply_remote_config", true);
	obs_data_set_bool(settings, "torch_enabled", true);
	obs_data_set_int(settings, "rotation", 0);

	obs_source_t *src = mock_obs::CreateMockSource();
	void *ctx = info.create(settings, src);
	CHECK(ctx != nullptr);

	CHECK(mock_obs::WaitForFrames(3, 5000));
	{
		MockFrameStats st = mock_obs::GetFrameStats();
		CHECK(st.last_format == VIDEO_FORMAT_I420);
		CHECK(st.last_width == 320 && st.last_height == 240);
		CHECK(st.last_timestamp > 0);
	}
	{
		std::lock_guard<std::mutex> lock(srv_mutex);
		CHECK(config_posted);
		CHECK(torch_on);
	}

	// Test dynamic Refresh button & properties UI population
	obs_properties_t *props = info.get_properties(ctx);
	CHECK(props != nullptr);
	const MockProperty *refresh_prop = mock_obs::FindProperty(props, "refresh_button");
	CHECK(refresh_prop != nullptr && refresh_prop->clicked_cb != nullptr);
	CHECK(refresh_prop->clicked_cb(props, const_cast<obs_property_t *>(
						      reinterpret_cast<const obs_property_t *>(
							      refresh_prop)),
				       ctx));
	const MockProperty *cam_prop = mock_obs::FindProperty(props, "camera_id");
	CHECK(cam_prop != nullptr && cam_prop->items.size() >= 2);

	const MockProperty *mode_prop = mock_obs::FindProperty(props, "connection_mode");
	CHECK(mode_prop != nullptr && mode_prop->modified_cb != nullptr);
	CHECK(mode_prop->modified_cb(props,
				     const_cast<obs_property_t *>(
					     reinterpret_cast<const obs_property_t *>(mode_prop)),
				     settings));
	CHECK(mock_obs::FindProperty(props, "wifi_host")->visible);
	CHECK(!mock_obs::FindProperty(props, "adb_serial")->visible);
	mock_obs::DestroyProperties(props);

	// Test live rotation update (0 -> 90) without pipeline failure
	obs_data_set_int(settings, "rotation", 90);
	info.update(ctx, settings);
	CHECK(mock_obs::WaitForDimensions(240, 320, 5000));

	info.deactivate(ctx);
	info.activate(ctx);
	info.destroy(ctx);
	const int count_after_destroy = mock_obs::GetFrameStats().count;
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	CHECK(mock_obs::GetFrameStats().count == count_after_destroy);

	mock_obs::DestroyMockSource(src);
	mock_obs::DestroyData(settings);
	obs_module_unload();
	std::cout << "  [OK] OBS plugin lifecycle & live video frame delivery verified" << std::endl;
}

} // namespace

int main()
{
	std::cout << "Running obs-android-webcamera test suite..." << std::endl;
	TestCodecNormalizationAndHostHelpers();
	std::cout << "[PASS] TestCodecNormalizationAndHostHelpers" << std::endl;

	TestJsonStatusAndConfigPatch();
	std::cout << "[PASS] TestJsonStatusAndConfigPatch" << std::endl;

	TestHttpClientRequests();
	std::cout << "[PASS] TestHttpClientRequests" << std::endl;

	TestAdbParsingAndRunner();
	std::cout << "[PASS] TestAdbParsingAndRunner" << std::endl;

	TestTransformI420();
	std::cout << "[PASS] TestTransformI420" << std::endl;

	TestDecoderAndObsLifecycle();
	std::cout << "[PASS] TestDecoderAndObsLifecycle" << std::endl;

	std::cout << "ALL TESTS PASSED" << std::endl;
	return 0;
}
