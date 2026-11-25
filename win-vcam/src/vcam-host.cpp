#include "awc-adb.hpp"
#include "awc-client.hpp"
#include "awc-decoder.hpp"
#include "vcam-shm.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

std::atomic<bool> g_stop_requested{false};
awc::AwcDecoder *g_active_decoder = nullptr;

void TriggerStop()
{
	g_stop_requested.store(true);
	if (auto *dec = g_active_decoder) {
		dec->RequestStop();
	}
}

#ifdef _WIN32
BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl_type)
{
	switch (ctrl_type) {
	case CTRL_C_EVENT:
	case CTRL_BREAK_EVENT:
	case CTRL_CLOSE_EVENT:
	case CTRL_SHUTDOWN_EVENT:
		TriggerStop();
		return TRUE;
	default:
		return FALSE;
	}
}
#else
void PosixSignalHandler(int)
{
	TriggerStop();
}
#endif

std::filesystem::path GetExecutableDir()
{
	std::error_code ec;
#ifdef _WIN32
	char buf[MAX_PATH] = {};
	const DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
	if (len > 0 && len < MAX_PATH) {
		return std::filesystem::path(buf).parent_path();
	}
#else
	char buf[4096] = {};
	const ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (len > 0) {
		buf[len] = '\0';
		return std::filesystem::path(buf).parent_path();
	}
#endif
	return std::filesystem::current_path(ec);
}

std::string FindBundledAdbExecutable()
{
	const auto exe_dir = GetExecutableDir();
	std::error_code ec;
#ifdef _WIN32
	const std::filesystem::path candidates[] = {
		exe_dir / "adb.exe",
		exe_dir / "platform-tools" / "adb.exe",
	};
#else
	const std::filesystem::path candidates[] = {
		exe_dir / "adb",
		exe_dir / "platform-tools" / "adb",
	};
#endif
	for (const auto &p : candidates) {
		if (std::filesystem::is_regular_file(p, ec)) {
			return p.string();
		}
	}
	return {};
}

std::string Trim(const std::string &s)
{
	const size_t first = s.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return {};
	const size_t last = s.find_last_not_of(" \t\r\n");
	return s.substr(first, last - first + 1);
}

struct HostOptions {
	std::string mode = "usb"; // "usb" or "wifi"
	std::string host;
	int port = 8080;
	std::string serial;
	std::string adb_path;
	std::string codec = "auto";
	std::string rotate = "auto";
	std::string mirror = "auto";
	bool show_help = false;
	bool interactive = false;
};

struct SavedConfig {
	std::string mode = "usb";
	std::string host;
	int port = 8080;
};

std::filesystem::path ConfigFilePath()
{
	return GetExecutableDir() / "awc-vcam.cfg";
}

SavedConfig LoadSavedConfig()
{
	SavedConfig cfg;
	std::ifstream in(ConfigFilePath());
	if (!in.is_open()) {
		return cfg;
	}
	std::string line;
	while (std::getline(in, line)) {
		line = Trim(line);
		if (line.empty() || line[0] == '#') continue;
		const size_t eq = line.find('=');
		if (eq == std::string::npos) continue;
		const std::string key = Trim(line.substr(0, eq));
		const std::string val = Trim(line.substr(eq + 1));
		if (key == "mode" && (val == "usb" || val == "wifi")) {
			cfg.mode = val;
		} else if (key == "host") {
			cfg.host = val;
		} else if (key == "port") {
			try {
				const int p = std::stoi(val);
				if (p >= 1 && p <= 65535) cfg.port = p;
			} catch (...) {
			}
		}
	}
	return cfg;
}

void SaveConfig(const SavedConfig &cfg)
{
	std::ofstream out(ConfigFilePath(), std::ios::trunc);
	if (!out.is_open()) {
		return;
	}
	out << "mode=" << cfg.mode << "\n";
	out << "host=" << cfg.host << "\n";
	out << "port=" << cfg.port << "\n";
}

bool RunInteractiveMenu(HostOptions &opts)
{
	SavedConfig saved = LoadSavedConfig();
	const int default_choice = (saved.mode == "wifi" && !saved.host.empty()) ? 2 : 1;

	std::cout
		<< "====================================================\n"
		<< "  AndroidWebCamera - Windows Virtual Camera Host\n"
		<< "====================================================\n\n"
		<< "Select connection mode:\n"
		<< "  [1] USB (automatic ADB tunnel)\n"
		<< "  [2] Wi-Fi / USB Tethering (connect by IP)";
	if (!saved.host.empty()) {
		std::cout << " [last: " << saved.host << ":" << saved.port << "]";
	}
	std::cout
		<< "\n"
		<< "  [0] Exit\n\n";

	while (!g_stop_requested.load()) {
		std::cout << "Choice [" << default_choice << "]: " << std::flush;
		std::string line;
		if (!std::getline(std::cin, line)) {
			return false;
		}
		line = Trim(line);
		int choice = default_choice;
		if (!line.empty()) {
			if (line == "0" || line == "q" || line == "Q" || line == "exit") {
				return false;
			} else if (line == "1" || line == "usb") {
				choice = 1;
			} else if (line == "2" || line == "wifi") {
				choice = 2;
			} else {
				std::cout << "Please enter 1, 2, or 0.\n";
				continue;
			}
		}

		if (choice == 1) {
			opts.mode = "usb";
			opts.port = 8080;
			saved.mode = "usb";
			SaveConfig(saved);
			std::cout << "\n";
			return true;
		}

		// choice == 2: Wi-Fi / USB Tethering
		while (!g_stop_requested.load()) {
			if (!saved.host.empty()) {
				std::cout << "Phone IP or URL [" << saved.host << ":" << saved.port << "]: " << std::flush;
			} else {
				std::cout << "Phone IP or URL (e.g. 192.168.1.42): " << std::flush;
			}
			std::string ip_line;
			if (!std::getline(std::cin, ip_line)) {
				return false;
			}
			ip_line = Trim(ip_line);
			if (ip_line.empty()) {
				if (!saved.host.empty()) {
					opts.mode = "wifi";
					opts.host = saved.host;
					opts.port = saved.port;
					saved.mode = "wifi";
					SaveConfig(saved);
					std::cout << "\n";
					return true;
				}
				std::cout << "IP address cannot be empty.\n";
				continue;
			}

			int parsed_port = 0;
			const std::string norm_host = awc::NormalizeHostInput(ip_line, &parsed_port);
			if (norm_host.empty()) {
				std::cout << "Invalid IP address or URL. Try again (e.g. 192.168.1.42).\n";
				continue;
			}
			opts.mode = "wifi";
			opts.host = norm_host;
			opts.port = (parsed_port > 0) ? parsed_port : 8080;
			saved.mode = "wifi";
			saved.host = opts.host;
			saved.port = opts.port;
			SaveConfig(saved);
			std::cout << "\n";
			return true;
		}
	}
	return false;
}

void WaitBeforeExitIfInteractive(bool interactive)
{
	if (!interactive) {
		return;
	}
	std::cout << "\nPress Enter to close this window..." << std::flush;
	std::string dummy;
	std::getline(std::cin, dummy);
}

void PrintUsage(const char *argv0)
{
	std::cout
		<< "AndroidWebCamera Virtual Camera Host (awc-vcam)\n\n"
		<< "Usage:\n"
		<< "  " << argv0 << "                 (opens interactive TUI menu)\n"
		<< "  " << argv0 << " [options]\n\n"
		<< "Options:\n"
		<< "  --mode usb|wifi         Connection mode (default: usb)\n"
		<< "  --host <ip[:port]>      Phone IP address for wifi mode (e.g. 192.168.1.42)\n"
		<< "  --port <port>           Local/remote HTTP port (default: 8080)\n"
		<< "  --serial <id>           Preferred ADB device serial (usb mode)\n"
		<< "  --adb <path>            Explicit path to adb executable\n"
		<< "  --codec auto|h264|h265|av1|mjpeg\n"
		<< "                          Stream codec (default: auto from phone /api/status)\n"
		<< "  --rotate auto|0|90|180|270\n"
		<< "                          Frame rotation (default: auto from phone)\n"
		<< "  --mirror auto|on|off    Horizontal mirror (default: auto from phone)\n"
		<< "  -h, --help              Show this help message\n\n"
		<< "Examples:\n"
		<< "  " << argv0 << " --mode usb\n"
		<< "  " << argv0 << " --mode wifi --host 192.168.1.42\n";
}

bool ParseArgs(int argc, char **argv, HostOptions &opts, std::string &out_error)
{
	if (argc <= 1) {
		opts.interactive = true;
		return true;
	}

	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		auto need_val = [&](std::string &target) -> bool {
			if (i + 1 >= argc) {
				out_error = "Missing value for " + arg;
				return false;
			}
			target = argv[++i];
			return true;
		};

		if (arg == "-h" || arg == "--help") {
			opts.show_help = true;
			return true;
		} else if (arg == "--mode") {
			if (!need_val(opts.mode)) return false;
		} else if (arg == "--host") {
			if (!need_val(opts.host)) return false;
		} else if (arg == "--port") {
			std::string val;
			if (!need_val(val)) return false;
			try {
				opts.port = std::stoi(val);
			} catch (...) {
				out_error = "Invalid --port: " + val;
				return false;
			}
			if (opts.port < 1 || opts.port > 65535) {
				out_error = "Port out of range (1..65535): " + val;
				return false;
			}
		} else if (arg == "--serial") {
			if (!need_val(opts.serial)) return false;
		} else if (arg == "--adb") {
			if (!need_val(opts.adb_path)) return false;
		} else if (arg == "--codec") {
			if (!need_val(opts.codec)) return false;
		} else if (arg == "--rotate") {
			if (!need_val(opts.rotate)) return false;
		} else if (arg == "--mirror") {
			if (!need_val(opts.mirror)) return false;
		} else {
			out_error = "Unknown argument: " + arg;
			return false;
		}
	}

	if (opts.mode != "usb" && opts.mode != "wifi") {
		out_error = "Invalid --mode '" + opts.mode + "' (expected 'usb' or 'wifi')";
		return false;
	}
	if (opts.mode == "wifi") {
		int host_port = 0;
		opts.host = awc::NormalizeHostInput(opts.host, &host_port);
		if (host_port > 0) {
			opts.port = host_port;
		}
		if (opts.host.empty()) {
			out_error = "--host is required in wifi mode (e.g. --mode wifi --host 192.168.1.42)";
			return false;
		}
	}
	if (opts.codec != "auto" && awc::NormalizeCodec(opts.codec).empty()) {
		out_error = "Invalid --codec '" + opts.codec + "' (expected auto, h264, h265, av1, mjpeg)";
		return false;
	}
	return true;
}

int ResolveRotationSetting(const std::string &flag, int status_rotation)
{
	if (flag == "auto" || flag.empty()) {
		return awc::NormalizeRotationDegrees(status_rotation);
	}
	try {
		return awc::NormalizeRotationDegrees(std::stoi(flag));
	} catch (...) {
		return 0;
	}
}

bool ResolveMirrorSetting(const std::string &flag, bool status_mirrored)
{
	if (flag == "on" || flag == "true" || flag == "1") return true;
	if (flag == "off" || flag == "false" || flag == "0") return false;
	return status_mirrored;
}

void SleepInterruptibleMs(int total_ms)
{
	constexpr int kStepMs = 100;
	int remaining = total_ms;
	while (remaining > 0 && !g_stop_requested.load()) {
		const int chunk = (remaining < kStepMs) ? remaining : kStepMs;
		std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
		remaining -= chunk;
	}
}

} // namespace

int main(int argc, char **argv)
{
#ifdef _WIN32
	SetConsoleTitleA("AndroidWebCamera - Virtual Camera Host");
	SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#else
	std::signal(SIGINT, PosixSignalHandler);
	std::signal(SIGTERM, PosixSignalHandler);
#endif

	HostOptions opts;
	std::string err;
	if (!ParseArgs(argc, argv, opts, err)) {
		std::cerr << "[awc-vcam] Error: " << err << "\n";
		PrintUsage(argv[0]);
		return 1;
	}
	if (opts.show_help) {
		PrintUsage(argv[0]);
		return 0;
	}
	if (opts.interactive) {
		if (!RunInteractiveMenu(opts)) {
			return 0;
		}
	}

	awc::vcam::VcamShmWriter shm;
	if (!shm.Open(awc::vcam::kDefaultShmName, awc::vcam::kDefaultEventName, err)) {
		std::cerr << "[awc-vcam] Failed to initialize virtual camera shared memory: " << err << "\n";
		WaitBeforeExitIfInteractive(opts.interactive);
		return 1;
	}

	std::cout << "[awc-vcam] Virtual camera shared memory ready.\n"
		  << "[awc-vcam] Select 'AndroidWebCamera' in VTube Studio / OBS / Discord.\n"
		  << "[awc-vcam] Mode: " << opts.mode
		  << (opts.mode == "wifi" ? (" (" + opts.host + ":" + std::to_string(opts.port) + ")") : "")
		  << ". Press Ctrl+C to stop.\n";

	std::unique_ptr<awc::AdbManager> adb;
	std::string forwarded_serial;

	while (!g_stop_requested.load()) {
		std::string target_host = (opts.mode == "usb") ? "127.0.0.1" : opts.host;

		if (opts.mode == "usb") {
			if (!adb) {
				std::string adb_bin;
				std::string adb_err;
				if (opts.adb_path.empty()) {
					adb_bin = FindBundledAdbExecutable();
				}
				if (adb_bin.empty()) {
					adb_bin = awc::FindAdbExecutable(opts.adb_path, adb_err);
				}
				if (adb_bin.empty()) {
					std::cerr << "[awc-vcam] Waiting for adb: " << adb_err << "\n";
					SleepInterruptibleMs(2500);
					continue;
				}
				std::cout << "[awc-vcam] Using ADB: " << adb_bin << "\n";
				adb = std::make_unique<awc::AdbManager>(adb_bin);
			}

			std::string resolved_serial;
			std::string fwd_err;
			if (!adb->SetupForward(opts.serial, opts.port, 8080, resolved_serial, fwd_err)) {
				std::cerr << "[awc-vcam] USB waiting: " << fwd_err
					  << " (check USB Debugging prompt on phone)\n";
				shm.MarkInactive();
				SleepInterruptibleMs(2000);
				continue;
			}
			if (resolved_serial != forwarded_serial) {
				std::cout << "[awc-vcam] USB ADB forward active: device=" << resolved_serial
					  << " (127.0.0.1:" << opts.port << " -> phone:8080)\n";
				forwarded_serial = resolved_serial;
			}
		}

		// Query phone /api/status to get codec, fps, rotation, mirror.
		awc::DeviceStatus status{};
		std::string status_err;
		std::string active_codec = (opts.codec == "auto") ? "h264" : awc::NormalizeCodec(opts.codec);
		uint32_t stream_fps = 30;
		int active_rotation = ResolveRotationSetting(opts.rotate, 0);
		bool active_mirror = ResolveMirrorSetting(opts.mirror, false);

		if (awc::HttpClient::GetStatus(target_host, opts.port, 2500, status, status_err)) {
			if (opts.codec == "auto") {
				const std::string norm = awc::NormalizeCodec(status.codec);
				if (!norm.empty()) {
					active_codec = norm;
				}
			}
			if (status.fps > 0) {
				stream_fps = static_cast<uint32_t>(status.fps);
			}
			active_rotation = ResolveRotationSetting(opts.rotate, status.rotation);
			active_mirror = ResolveMirrorSetting(opts.mirror, status.mirrored);

			if (!status.streaming) {
				std::cerr << "[awc-vcam] Phone connected (" << target_host << ":" << opts.port
					  << "), waiting for streaming to start on Android...\n";
				shm.MarkInactive();
				SleepInterruptibleMs(1500);
				continue;
			}
		} else {
			std::cerr << "[awc-vcam] Cannot reach phone at http://" << target_host << ":" << opts.port
				  << "/api/status (" << status_err << "). Retrying...\n";
			shm.MarkInactive();
			SleepInterruptibleMs(2000);
			continue;
		}

		const std::string base_url = "http://" + target_host + ":" + std::to_string(opts.port);
		const std::string stream_url = awc::StreamUrlForCodec(base_url, active_codec);

		std::cout << "[awc-vcam] Connecting stream: " << stream_url
			  << " (codec=" << active_codec
			  << ", rotate=" << active_rotation
			  << ", mirror=" << (active_mirror ? "on" : "off")
			  << ", fps=" << stream_fps << ")\n";

		awc::AwcDecoder decoder;
		g_active_decoder = &decoder;

		awc::AwcDecoder::Options dec_opts;
		dec_opts.url = stream_url;
		dec_opts.codec = active_codec;
		dec_opts.open_timeout_ms = 4000;
		dec_opts.read_timeout_ms = 3000;
		dec_opts.initial_rotation = active_rotation;
		dec_opts.initial_mirror = active_mirror;

		int64_t decoded_frames = 0;
		std::string dec_err;
		bool printed_first_frame = false;

		decoder.Run(
			dec_opts,
			[&](const awc::DecodedFrameView &frame) {
				awc::vcam::I420FrameInput in{};
				in.y = frame.y;
				in.u = frame.u;
				in.v = frame.v;
				in.stride_y = frame.stride_y;
				in.stride_u = frame.stride_u;
				in.stride_v = frame.stride_v;
				in.width = frame.width;
				in.height = frame.height;
				in.fps_num = stream_fps;
				in.fps_den = 1;
				in.full_range = frame.full_range;
				in.pts_us = frame.pts_us;

				if (shm.PublishFrame(in) && !printed_first_frame) {
					printed_first_frame = true;
					std::cout << "[awc-vcam] Live! Streaming " << frame.width << "x"
						  << frame.height << " to virtual camera 'AndroidWebCamera'.\n";
				}
			},
			dec_err, &decoded_frames);

		g_active_decoder = nullptr;
		shm.MarkInactive();

		if (!g_stop_requested.load()) {
			std::cerr << "[awc-vcam] Stream interrupted after " << decoded_frames
				  << " frames (" << dec_err << "). Reconnecting...\n";
			SleepInterruptibleMs(1500);
		}
	}

	if (adb && !forwarded_serial.empty()) {
		std::string rm_err;
		adb->RemoveForward(forwarded_serial, opts.port, rm_err);
	}
	shm.Close();
	std::cout << "[awc-vcam] Stopped cleanly.\n";
	return 0;
}
