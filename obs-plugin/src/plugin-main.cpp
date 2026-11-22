#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "awc-adb.hpp"
#include "awc-client.hpp"
#include "awc-decoder.hpp"

extern "C" {
OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-android-webcamera", "en-US")
}

namespace {

constexpr const char *kSourceId = "android_webcamera_source";

constexpr const char *kPropConnectionMode = "connection_mode";
constexpr const char *kPropAdbSerial = "adb_serial";
constexpr const char *kPropAdbPath = "adb_path";
constexpr const char *kPropLocalPort = "local_port";
constexpr const char *kPropWifiHost = "wifi_host";
constexpr const char *kPropRemotePort = "remote_port";
constexpr const char *kPropRefresh = "refresh_button";
constexpr const char *kPropStatusInfo = "status_info";
constexpr const char *kPropApplyRemote = "apply_remote_config";
constexpr const char *kPropCameraId = "camera_id";
constexpr const char *kPropCodec = "codec";
constexpr const char *kPropResolution = "resolution";
constexpr const char *kPropFps = "fps";
constexpr const char *kPropBitrate = "bitrate_kbps";
constexpr const char *kPropJpegQuality = "jpeg_quality";
constexpr const char *kPropRotation = "rotation";
constexpr const char *kPropMirrored = "mirrored";
constexpr const char *kPropTorch = "torch_enabled";
constexpr const char *kPropDisconnectInactive = "disconnect_when_inactive";

constexpr const char *kModeUsb = "usb";
constexpr const char *kModeWifi = "wifi";

const char *TextOrFallback(const char *key, const char *fallback)
{
	const char *t = obs_module_text(key);
	if (!t || std::strcmp(t, key) == 0)
		return fallback;
	return t;
}

struct SourceSettings {
	std::string connection_mode = kModeUsb;
	std::string adb_serial;
	std::string adb_path;
	int local_port = 8080;
	std::string wifi_host = "192.168.1.100";
	int remote_port = 8080;

	bool apply_remote_config = true;
	std::string camera_id;
	std::string codec = "auto";
	std::string resolution;
	int fps = 0;
	int bitrate_kbps = 4000;
	int jpeg_quality = 80;
	int rotation = -1; // -1 = auto from phone status
	bool mirrored = false;
	bool torch_enabled = false;
	bool disconnect_when_inactive = false;
};

bool ParseResolutionString(const std::string &res, int &out_w, int &out_h)
{
	size_t x = res.find('x');
	if (x == std::string::npos)
		x = res.find('X');
	if (x == std::string::npos)
		return false;
	int w = std::atoi(res.substr(0, x).c_str());
	int h = std::atoi(res.substr(x + 1).c_str());
	if (w <= 0 || h <= 0)
		return false;
	out_w = w;
	out_h = h;
	return true;
}

std::string FormatResolutionString(int w, int h)
{
	return std::to_string(w) + "x" + std::to_string(h);
}

SourceSettings ReadSettingsFromObsData(obs_data_t *data)
{
	SourceSettings s;
	if (!data)
		return s;

	const char *mode = obs_data_get_string(data, kPropConnectionMode);
	s.connection_mode = (mode && std::strcmp(mode, kModeWifi) == 0) ? kModeWifi : kModeUsb;

	const char *serial = obs_data_get_string(data, kPropAdbSerial);
	s.adb_serial = serial ? serial : "";

	const char *adb_path = obs_data_get_string(data, kPropAdbPath);
	s.adb_path = adb_path ? adb_path : "";

	int lport = static_cast<int>(obs_data_get_int(data, kPropLocalPort));
	s.local_port = (lport >= 1024 && lport <= 65535) ? lport : 8080;

	const char *host = obs_data_get_string(data, kPropWifiHost);
	s.wifi_host = (host && *host) ? host : "192.168.1.100";

	int rport = static_cast<int>(obs_data_get_int(data, kPropRemotePort));
	s.remote_port = (rport >= 1024 && rport <= 65535) ? rport : 8080;

	s.apply_remote_config = obs_data_get_bool(data, kPropApplyRemote);

	const char *cam = obs_data_get_string(data, kPropCameraId);
	s.camera_id = cam ? cam : "";

	const char *codec = obs_data_get_string(data, kPropCodec);
	s.codec = (codec && *codec) ? codec : "auto";

	const char *res = obs_data_get_string(data, kPropResolution);
	s.resolution = res ? res : "";

	s.fps = static_cast<int>(obs_data_get_int(data, kPropFps));

	int br = static_cast<int>(obs_data_get_int(data, kPropBitrate));
	s.bitrate_kbps = (br >= 500 && br <= 20000) ? br : 4000;

	int jq = static_cast<int>(obs_data_get_int(data, kPropJpegQuality));
	s.jpeg_quality = (jq >= 40 && jq <= 95) ? jq : 80;

	s.rotation = static_cast<int>(obs_data_get_int(data, kPropRotation));
	s.mirrored = obs_data_get_bool(data, kPropMirrored);
	s.torch_enabled = obs_data_get_bool(data, kPropTorch);
	s.disconnect_when_inactive = obs_data_get_bool(data, kPropDisconnectInactive);
	return s;
}

bool PipelineSettingsChanged(const SourceSettings &a, const SourceSettings &b)
{
	return a.connection_mode != b.connection_mode || a.adb_serial != b.adb_serial ||
	       a.adb_path != b.adb_path || a.local_port != b.local_port ||
	       a.wifi_host != b.wifi_host || a.remote_port != b.remote_port ||
	       a.apply_remote_config != b.apply_remote_config || a.camera_id != b.camera_id ||
	       a.codec != b.codec || a.resolution != b.resolution || a.fps != b.fps ||
	       (a.codec == "mjpeg" && a.jpeg_quality != b.jpeg_quality);
}

awc::ConfigPatch BuildFullRemotePatch(const SourceSettings &s, const awc::DeviceStatus *current)
{
	awc::ConfigPatch patch;
	if (!s.apply_remote_config)
		return patch;

	if (!s.camera_id.empty() && (!current || current->camera_id != s.camera_id)) {
		patch.has_camera_id = true;
		patch.camera_id = s.camera_id;
	}

	std::string req_codec = awc::NormalizeCodec(s.codec);
	if (!req_codec.empty() && (!current || awc::NormalizeCodec(current->codec) != req_codec)) {
		patch.has_codec = true;
		patch.codec = req_codec;
	}

	int rw = 0, rh = 0;
	if (ParseResolutionString(s.resolution, rw, rh)) {
		if (!current || current->width != rw || current->height != rh) {
			patch.has_width = true;
			patch.width = rw;
			patch.has_height = true;
			patch.height = rh;
		}
	}

	if (s.fps > 0 && (!current || current->fps != s.fps)) {
		patch.has_fps = true;
		patch.fps = s.fps;
	}

	if (!current || current->bitrate_kbps != s.bitrate_kbps) {
		patch.has_bitrate_kbps = true;
		patch.bitrate_kbps = s.bitrate_kbps;
	}

	const std::string effective_codec =
		!req_codec.empty() ? req_codec : (current ? awc::NormalizeCodec(current->codec) : "");
	if (effective_codec == "mjpeg" && (!current || current->jpeg_quality != s.jpeg_quality)) {
		patch.has_jpeg_quality = true;
		patch.jpeg_quality = s.jpeg_quality;
	}

	if (!current || current->torch_enabled != s.torch_enabled) {
		patch.has_torch_enabled = true;
		patch.torch_enabled = s.torch_enabled;
	}

	return patch;
}

class AwcSourceContext {
public:
	AwcSourceContext(obs_data_t *settings, obs_source_t *source)
		: source_(source), settings_(ReadSettingsFromObsData(settings))
	{
		worker_thread_ = std::thread(&AwcSourceContext::WorkerLoop, this);
	}

	~AwcSourceContext()
	{
		StopAndJoin();
	}

	void StopAndJoin()
	{
		shutting_down_.store(true, std::memory_order_release);
		StopActiveDecoder();
		WakeWorker();
		if (worker_thread_.joinable()) {
			worker_thread_.join();
		}
	}

	void Update(obs_data_t *obs_settings)
	{
		SourceSettings next = ReadSettingsFromObsData(obs_settings);
		bool need_restart = false;
		int effective_rot = 0;
		bool effective_mirror = false;

		{
			std::lock_guard<std::mutex> lock(state_mutex_);
			SourceSettings prev = settings_;
			settings_ = next;
			need_restart = PipelineSettingsChanged(prev, next);

			effective_rot = ResolveEffectiveRotationLocked(next);
			effective_mirror = next.mirrored;

			if (!need_restart && next.apply_remote_config) {
				if (prev.torch_enabled != next.torch_enabled) {
					pending_live_patch_.has_torch_enabled = true;
					pending_live_patch_.torch_enabled = next.torch_enabled;
				}
				if (prev.bitrate_kbps != next.bitrate_kbps) {
					pending_live_patch_.has_bitrate_kbps = true;
					pending_live_patch_.bitrate_kbps = next.bitrate_kbps;
				}
			}
		}

		// Apply rotation & mirror immediately on active decoder without restarting stream.
		UpdateDecoderTransform(effective_rot, effective_mirror);

		if (need_restart) {
			config_epoch_.fetch_add(1, std::memory_order_acq_rel);
			StopActiveDecoder();
		}
		WakeWorker();
	}

	void Activate()
	{
		active_.store(true, std::memory_order_release);
		WakeWorker();
	}

	void Deactivate()
	{
		active_.store(false, std::memory_order_release);
		bool should_stop = false;
		{
			std::lock_guard<std::mutex> lock(state_mutex_);
			should_stop = settings_.disconnect_when_inactive;
		}
		if (should_stop) {
			StopActiveDecoder();
			WakeWorker();
		}
	}

	void RefreshFromUi(obs_data_t *obs_settings)
	{
		SourceSettings snap = obs_settings ? ReadSettingsFromObsData(obs_settings)
						   : GetSettingsSnapshot();

		std::vector<awc::AdbDevice> devs;
		awc::DeviceStatus st;
		bool got_status = false;
		std::string info_msg;

		if (snap.connection_mode == kModeUsb) {
			std::string adb_err;
			std::string adb_bin = awc::FindAdbExecutable(snap.adb_path, adb_err);
			if (adb_bin.empty()) {
				info_msg = adb_err;
			} else {
				awc::AdbManager mgr(adb_bin);
				std::string list_err;
				if (!mgr.ListDevices(devs, list_err, 4000)) {
					info_msg = list_err;
				} else {
					std::string resolved_serial;
					std::string fwd_err;
					if (!mgr.SetupForward(snap.adb_serial, snap.local_port,
							      snap.remote_port, resolved_serial,
							      fwd_err, 4000)) {
						info_msg = fwd_err;
					} else {
						std::string http_err;
						if (awc::HttpClient::GetStatus("127.0.0.1",
									       snap.local_port,
									       2500, st, http_err)) {
							got_status = true;
							info_msg = FormatConnectedStatus(
								st, "USB (" + resolved_serial + ")");
						} else {
							info_msg = "ADB forward OK (" +
								   resolved_serial + "), но " +
								   http_err;
						}
					}
				}
			}
		} else {
			int port = snap.remote_port;
			std::string host = awc::NormalizeHostInput(snap.wifi_host, &port);
			std::string http_err;
			if (awc::HttpClient::GetStatus(host, port, 2500, st, http_err)) {
				got_status = true;
				info_msg = FormatConnectedStatus(
					st, host + ":" + std::to_string(port));
			} else {
				info_msg = http_err;
			}
		}

		{
			std::lock_guard<std::mutex> lock(state_mutex_);
			if (!devs.empty() || snap.connection_mode == kModeUsb)
				cached_devices_ = std::move(devs);
			if (got_status) {
				cached_status_ = std::move(st);
				has_cached_status_ = true;
			}
			last_status_text_ = info_msg;
		}
	}

	void PopulateDynamicProperties(obs_properties_t *props, const SourceSettings &ui_settings)
	{
		std::vector<awc::AdbDevice> devs;
		awc::DeviceStatus st;
		bool has_st = false;
		std::string status_text;
		{
			std::lock_guard<std::mutex> lock(state_mutex_);
			devs = cached_devices_;
			st = cached_status_;
			has_st = has_cached_status_;
			status_text = last_status_text_;
		}

		const bool is_usb = (ui_settings.connection_mode == kModeUsb);
		if (obs_property_t *p = obs_properties_get(props, kPropAdbSerial)) {
			obs_property_set_visible(p, is_usb);
			obs_property_list_clear(p);
			obs_property_list_add_string(
				p, TextOrFallback("Awc.AdbDevice.Auto", "Автовыбор единственного устройства"),
				"");
			bool current_present = ui_settings.adb_serial.empty();
			for (const auto &d : devs) {
				std::string label =
					d.model.empty() ? d.serial : (d.model + " (" + d.serial + ")");
				if (d.state != "device")
					label += " [" + d.state + "]";
				obs_property_list_add_string(p, label.c_str(), d.serial.c_str());
				if (d.serial == ui_settings.adb_serial)
					current_present = true;
			}
			if (!current_present) {
				obs_property_list_add_string(p, ui_settings.adb_serial.c_str(),
							     ui_settings.adb_serial.c_str());
			}
		}
		if (obs_property_t *p = obs_properties_get(props, kPropAdbPath))
			obs_property_set_visible(p, is_usb);
		if (obs_property_t *p = obs_properties_get(props, kPropLocalPort))
			obs_property_set_visible(p, is_usb);
		if (obs_property_t *p = obs_properties_get(props, kPropWifiHost))
			obs_property_set_visible(p, !is_usb);

		if (obs_property_t *p = obs_properties_get(props, kPropCameraId)) {
			obs_property_list_clear(p);
			obs_property_list_add_string(
				p,
				TextOrFallback("Awc.Camera.KeepCurrent",
					       "Не менять (оставить как на телефоне)"),
				"");
			bool cam_found = ui_settings.camera_id.empty();
			if (has_st) {
				for (const auto &cam : st.cameras) {
					std::string label = cam.id + ": " + cam.name;
					if (!cam.facing.empty())
						label += " [" + cam.facing + "]";
					obs_property_list_add_string(p, label.c_str(),
								     cam.id.c_str());
					if (cam.id == ui_settings.camera_id)
						cam_found = true;
				}
			}
			if (!cam_found) {
				obs_property_list_add_string(p, ui_settings.camera_id.c_str(),
							     ui_settings.camera_id.c_str());
			}
		}

		if (obs_property_t *p = obs_properties_get(props, kPropCodec)) {
			obs_property_list_clear(p);
			obs_property_list_add_string(
				p, TextOrFallback("Awc.Codec.Auto", "Как на телефоне (авто)"), "auto");
			const char *default_ids[] = {"h264", "h265", "av1", "mjpeg"};
			const char *default_names[] = {"H.264", "H.265 (HEVC)", "AV1", "MJPEG"};
			for (int i = 0; i < 4; ++i) {
				std::string label = default_names[i];
				if (has_st) {
					for (const auto &ci : st.supported_codecs) {
						if (awc::NormalizeCodec(ci.id) == default_ids[i]) {
							if (!ci.supported)
								label += " (не поддерживается)";
							else if (ci.hardware)
								label += " (HW)";
							break;
						}
					}
				}
				obs_property_list_add_string(p, label.c_str(), default_ids[i]);
			}
		}

		const awc::CameraInfo *selected_cam = nullptr;
		if (has_st) {
			std::string target_cam =
				!ui_settings.camera_id.empty() ? ui_settings.camera_id : st.camera_id;
			for (const auto &cam : st.cameras) {
				if (cam.id == target_cam) {
					selected_cam = &cam;
					break;
				}
			}
		}

		if (obs_property_t *p = obs_properties_get(props, kPropResolution)) {
			obs_property_list_clear(p);
			obs_property_list_add_string(
				p,
				TextOrFallback("Awc.Resolution.KeepCurrent", "Как на телефоне"),
				"");
			bool res_found = ui_settings.resolution.empty();
			if (selected_cam) {
				for (const auto &res : selected_cam->resolutions) {
					std::string val =
						FormatResolutionString(res.width, res.height);
					obs_property_list_add_string(p, val.c_str(), val.c_str());
					if (val == ui_settings.resolution)
						res_found = true;
				}
			} else {
				const char *common_res[] = {"3840x2160", "2560x1440", "1920x1080",
							    "1280x720",  "960x540",   "640x480"};
				for (const char *r : common_res) {
					obs_property_list_add_string(p, r, r);
					if (ui_settings.resolution == r)
						res_found = true;
				}
			}
			if (!res_found) {
				obs_property_list_add_string(p, ui_settings.resolution.c_str(),
							     ui_settings.resolution.c_str());
			}
		}

		if (obs_property_t *p = obs_properties_get(props, kPropFps)) {
			obs_property_list_clear(p);
			obs_property_list_add_int(
				p, TextOrFallback("Awc.Fps.KeepCurrent", "Как на телефоне"), 0);
			std::vector<int> fps_choices = {15, 24, 30, 60};
			if (selected_cam) {
				int rw = 0, rh = 0;
				if (!ParseResolutionString(ui_settings.resolution, rw, rh) &&
				    has_st) {
					rw = st.width;
					rh = st.height;
				}
				for (const auto &res : selected_cam->resolutions) {
					if (res.width == rw && res.height == rh && !res.fps.empty()) {
						fps_choices = res.fps;
						break;
					}
				}
			}
			bool fps_found = (ui_settings.fps == 0);
			for (int f : fps_choices) {
				std::string label = std::to_string(f) + " fps";
				obs_property_list_add_int(p, label.c_str(), f);
				if (f == ui_settings.fps)
					fps_found = true;
			}
			if (!fps_found) {
				std::string label = std::to_string(ui_settings.fps) + " fps";
				obs_property_list_add_int(p, label.c_str(), ui_settings.fps);
			}
		}

		const bool remote_on = ui_settings.apply_remote_config;
		const std::string eff_codec =
			ui_settings.codec != "auto"
				? awc::NormalizeCodec(ui_settings.codec)
				: (has_st ? awc::NormalizeCodec(st.codec) : "");
		const bool is_mjpeg = (eff_codec == "mjpeg");

		auto set_remote_prop = [&](const char *name, bool vis = true) {
			if (obs_property_t *p = obs_properties_get(props, name)) {
				obs_property_set_enabled(p, remote_on);
				obs_property_set_visible(p, vis);
			}
		};
		set_remote_prop(kPropCameraId);
		set_remote_prop(kPropCodec);
		set_remote_prop(kPropResolution);
		set_remote_prop(kPropFps);
		set_remote_prop(kPropBitrate, !is_mjpeg);
		set_remote_prop(kPropJpegQuality, is_mjpeg);
		set_remote_prop(kPropTorch);

		if (obs_property_t *p = obs_properties_get(props, kPropStatusInfo)) {
			if (status_text.empty()) {
				status_text = "Ожидание подключения... Нажмите «Обновить» или запустите трансляцию на телефоне.";
			}
			obs_data_t *src_settings = nullptr;
			// Note: OBS_TEXT_INFO reads its value from settings[kPropStatusInfo].
			(void)src_settings;
			obs_property_set_long_description(p, status_text.c_str());
		}
	}

	std::string GetLastStatusText()
	{
		std::lock_guard<std::mutex> lock(state_mutex_);
		return last_status_text_;
	}

	SourceSettings GetSettingsSnapshot()
	{
		std::lock_guard<std::mutex> lock(state_mutex_);
		return settings_;
	}

	obs_source_t *Source() const { return source_; }

private:
	static std::string FormatConnectedStatus(const awc::DeviceStatus &st,
						 const std::string &endpoint)
	{
		std::ostringstream oss;
		oss << (st.streaming ? "Активно" : "Остановлено") << " | " << endpoint << " | "
		    << st.camera_name << " | " << st.width << "x" << st.height << " @ "
		    << (st.measured_fps > 0 ? st.measured_fps : st.fps) << " fps (" << st.codec
		    << ")";
		return oss.str();
	}

	int ResolveEffectiveRotationLocked(const SourceSettings &s) const
	{
		if (s.rotation >= 0)
			return awc::NormalizeRotationDegrees(s.rotation);
		if (has_cached_status_)
			return awc::NormalizeRotationDegrees(cached_status_.rotation);
		return 0;
	}

	void UpdateDecoderTransform(int rot, bool mirror)
	{
		std::lock_guard<std::mutex> lock(decoder_mutex_);
		if (active_decoder_)
			active_decoder_->SetTransform(rot, mirror);
	}

	void StopActiveDecoder()
	{
		std::lock_guard<std::mutex> lock(decoder_mutex_);
		if (active_decoder_)
			active_decoder_->RequestStop();
	}

	void WakeWorker()
	{
		wake_cv_.notify_all();
	}

	void WaitBeforeRetryOrPause(int ms)
	{
		std::unique_lock<std::mutex> lock(wake_mutex_);
		wake_cv_.wait_for(lock, std::chrono::milliseconds(ms), [this]() {
			return shutting_down_.load(std::memory_order_acquire);
		});
	}

	void SetStatusText(const std::string &msg)
	{
		std::lock_guard<std::mutex> lock(state_mutex_);
		last_status_text_ = msg;
	}

	void WorkerLoop()
	{
		std::thread live_patch_thread;
		while (!shutting_down_.load(std::memory_order_acquire)) {
			const uint64_t loop_epoch = config_epoch_.load(std::memory_order_acquire);
			SourceSettings s = GetSettingsSnapshot();

			if (s.disconnect_when_inactive && !active_.load(std::memory_order_acquire)) {
				SetStatusText("Источник скрыт (ожидание активации)");
				std::unique_lock<std::mutex> lock(wake_mutex_);
				wake_cv_.wait_for(lock, std::chrono::milliseconds(300), [this]() {
					return shutting_down_.load(std::memory_order_acquire) ||
					       active_.load(std::memory_order_acquire);
				});
				continue;
			}

			std::string host;
			int port = s.remote_port;
			std::string endpoint_label;

			if (s.connection_mode == kModeUsb) {
				std::string adb_err;
				std::string adb_bin = awc::FindAdbExecutable(s.adb_path, adb_err);
				if (adb_bin.empty()) {
					SetStatusText(adb_err);
					WaitBeforeRetryOrPause(1500);
					continue;
				}
				awc::AdbManager mgr(adb_bin);
				std::string resolved_serial;
				std::string fwd_err;
				if (!mgr.SetupForward(s.adb_serial, s.local_port, s.remote_port,
						      resolved_serial, fwd_err, 4000)) {
					SetStatusText(fwd_err);
					WaitBeforeRetryOrPause(1500);
					continue;
				}
				host = "127.0.0.1";
				port = s.local_port;
				endpoint_label = "USB (" + resolved_serial + ")";
			} else {
				host = awc::NormalizeHostInput(s.wifi_host, &port);
				endpoint_label = host + ":" + std::to_string(port);
			}

			if (shutting_down_.load(std::memory_order_acquire) ||
			    config_epoch_.load(std::memory_order_acquire) != loop_epoch) {
				continue;
			}

			awc::DeviceStatus st;
			std::string http_err;
			if (!awc::HttpClient::GetStatus(host, port, 2500, st, http_err)) {
				SetStatusText(http_err);
				WaitBeforeRetryOrPause(1000);
				continue;
			}

			if (s.apply_remote_config) {
				awc::ConfigPatch patch = BuildFullRemotePatch(s, &st);
				if (!patch.IsEmpty()) {
					awc::DeviceStatus updated_st;
					std::string cfg_err;
					if (awc::HttpClient::UpdateConfig(host, port, patch, 3500,
									  updated_st, cfg_err)) {
						st = std::move(updated_st);
					} else {
						blog(LOG_WARNING,
						     "[AndroidWebCamera] /api/config failed: %s",
						     cfg_err.c_str());
					}
				}
			}

			{
				std::lock_guard<std::mutex> lock(state_mutex_);
				cached_status_ = st;
				has_cached_status_ = true;
				pending_live_patch_ = awc::ConfigPatch{};
				if (!st.streaming) {
					last_status_text_ =
						"Подключено (" + endpoint_label +
						"), но трансляция на телефоне не запущена";
				} else {
					last_status_text_ =
						FormatConnectedStatus(st, endpoint_label);
				}
			}

			if (!st.streaming) {
				WaitBeforeRetryOrPause(800);
				continue;
			}

			std::string active_codec = awc::NormalizeCodec(st.codec);
			if (active_codec.empty())
				active_codec = "h264";

			const std::string base_url =
				"http://" + host + ":" + std::to_string(port);
			const std::string stream_url =
				awc::StreamUrlForCodec(base_url, active_codec);

			const int init_rot =
				(s.rotation >= 0) ? awc::NormalizeRotationDegrees(s.rotation)
						  : awc::NormalizeRotationDegrees(st.rotation);

			awc::AwcDecoder decoder;
			awc::AwcDecoder::Options dec_opts;
			dec_opts.url = stream_url;
			dec_opts.codec = active_codec;
			dec_opts.open_timeout_ms = 4000;
			dec_opts.read_timeout_ms = 3000;
			dec_opts.initial_rotation = init_rot;
			dec_opts.initial_mirror = s.mirrored;

			{
				std::lock_guard<std::mutex> lock(decoder_mutex_);
				active_decoder_ = &decoder;
			}

			// Background live-config sender so torch/bitrate updates apply without stopping decoder.
			std::atomic<bool> stream_running{true};
			live_patch_thread = std::thread([&]() {
				while (stream_running.load(std::memory_order_acquire) &&
				       !shutting_down_.load(std::memory_order_acquire)) {
					awc::ConfigPatch live_patch;
					{
						std::lock_guard<std::mutex> lock(state_mutex_);
						if (!pending_live_patch_.IsEmpty()) {
							live_patch = pending_live_patch_;
							pending_live_patch_ = awc::ConfigPatch{};
						}
					}
					if (!live_patch.IsEmpty()) {
						awc::DeviceStatus updated_st;
						std::string patch_err;
						if (awc::HttpClient::UpdateConfig(
							    host, port, live_patch, 2500,
							    updated_st, patch_err)) {
							std::lock_guard<std::mutex> lock(
								state_mutex_);
							cached_status_ = std::move(updated_st);
							has_cached_status_ = true;
						}
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(80));
				}
			});

			std::string dec_err;
			int64_t frame_count = 0;
			decoder.Run(
				dec_opts,
				[this](const awc::DecodedFrameView &view) {
					if (shutting_down_.load(std::memory_order_acquire))
						return;
					struct obs_source_frame obs_frame{};
					obs_frame.width = view.width;
					obs_frame.height = view.height;
					obs_frame.format = VIDEO_FORMAT_I420;
					obs_frame.data[0] = const_cast<uint8_t *>(view.y);
					obs_frame.data[1] = const_cast<uint8_t *>(view.u);
					obs_frame.data[2] = const_cast<uint8_t *>(view.v);
					obs_frame.linesize[0] = view.stride_y;
					obs_frame.linesize[1] = view.stride_u;
					obs_frame.linesize[2] = view.stride_v;
					obs_frame.full_range = view.full_range;
					obs_frame.timestamp = os_gettime_ns();
					video_format_get_parameters_for_format(
						VIDEO_CS_709,
						view.full_range ? VIDEO_RANGE_FULL
								: VIDEO_RANGE_PARTIAL,
						VIDEO_FORMAT_I420, obs_frame.color_matrix,
						obs_frame.color_range_min,
						obs_frame.color_range_max);
					obs_source_output_video(source_, &obs_frame);
				},
				dec_err, &frame_count);

			stream_running.store(false, std::memory_order_release);
			if (live_patch_thread.joinable())
				live_patch_thread.join();

			{
				std::lock_guard<std::mutex> lock(decoder_mutex_);
				active_decoder_ = nullptr;
			}

			if (!dec_err.empty() && !shutting_down_.load(std::memory_order_acquire) &&
			    config_epoch_.load(std::memory_order_acquire) == loop_epoch) {
				SetStatusText(dec_err);
				blog(LOG_WARNING, "[AndroidWebCamera] stream ended: %s",
				     dec_err.c_str());
				WaitBeforeRetryOrPause(500);
			}
		}
	}

	obs_source_t *source_ = nullptr;

	std::mutex state_mutex_;
	SourceSettings settings_;
	awc::DeviceStatus cached_status_;
	bool has_cached_status_ = false;
	std::vector<awc::AdbDevice> cached_devices_;
	std::string last_status_text_;
	awc::ConfigPatch pending_live_patch_;

	std::mutex decoder_mutex_;
	awc::AwcDecoder *active_decoder_ = nullptr;

	std::atomic<bool> shutting_down_{false};
	std::atomic<bool> active_{true};
	std::atomic<uint64_t> config_epoch_{1};

	std::mutex wake_mutex_;
	std::condition_variable wake_cv_;
	std::thread worker_thread_;
};

const char *AwcGetName(void *type_data)
{
	(void)type_data;
	return TextOrFallback("Awc.SourceName", "AndroidWebCamera");
}

void *AwcCreate(obs_data_t *settings, obs_source_t *source)
{
	return new AwcSourceContext(settings, source);
}

void AwcDestroy(void *data)
{
	delete static_cast<AwcSourceContext *>(data);
}

void AwcUpdate(void *data, obs_data_t *settings)
{
	auto *ctx = static_cast<AwcSourceContext *>(data);
	if (ctx)
		ctx->Update(settings);
}

void AwcActivate(void *data)
{
	auto *ctx = static_cast<AwcSourceContext *>(data);
	if (ctx)
		ctx->Activate();
}

void AwcDeactivate(void *data)
{
	auto *ctx = static_cast<AwcSourceContext *>(data);
	if (ctx)
		ctx->Deactivate();
}

void AwcGetDefaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, kPropConnectionMode, kModeUsb);
	obs_data_set_default_string(settings, kPropAdbSerial, "");
	obs_data_set_default_string(settings, kPropAdbPath, "");
	obs_data_set_default_int(settings, kPropLocalPort, 8080);
	obs_data_set_default_string(settings, kPropWifiHost, "192.168.1.100");
	obs_data_set_default_int(settings, kPropRemotePort, 8080);
	obs_data_set_default_string(
		settings, kPropStatusInfo,
		"Нажмите «Обновить» для поиска устройств и чтения параметров камеры.");
	obs_data_set_default_bool(settings, kPropApplyRemote, true);
	obs_data_set_default_string(settings, kPropCameraId, "");
	obs_data_set_default_string(settings, kPropCodec, "auto");
	obs_data_set_default_string(settings, kPropResolution, "");
	obs_data_set_default_int(settings, kPropFps, 0);
	obs_data_set_default_int(settings, kPropBitrate, 4000);
	obs_data_set_default_int(settings, kPropJpegQuality, 80);
	obs_data_set_default_int(settings, kPropRotation, -1);
	obs_data_set_default_bool(settings, kPropMirrored, false);
	obs_data_set_default_bool(settings, kPropTorch, false);
	obs_data_set_default_bool(settings, kPropDisconnectInactive, false);
}

bool OnConnectionModeModified(obs_properties_t *props, obs_property_t *property,
			      obs_data_t *settings)
{
	(void)property;
	const char *mode = obs_data_get_string(settings, kPropConnectionMode);
	const bool is_usb = (!mode || std::strcmp(mode, kModeWifi) != 0);
	if (obs_property_t *p = obs_properties_get(props, kPropAdbSerial))
		obs_property_set_visible(p, is_usb);
	if (obs_property_t *p = obs_properties_get(props, kPropAdbPath))
		obs_property_set_visible(p, is_usb);
	if (obs_property_t *p = obs_properties_get(props, kPropLocalPort))
		obs_property_set_visible(p, is_usb);
	if (obs_property_t *p = obs_properties_get(props, kPropWifiHost))
		obs_property_set_visible(p, !is_usb);
	return true;
}

bool OnRemoteToggleModified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	(void)property;
	const bool remote_on = obs_data_get_bool(settings, kPropApplyRemote);
	const char *codec_raw = obs_data_get_string(settings, kPropCodec);
	const std::string codec = codec_raw ? awc::NormalizeCodec(codec_raw) : "";
	const bool is_mjpeg = (codec == "mjpeg");

	auto set_en = [&](const char *name, bool vis = true) {
		if (obs_property_t *p = obs_properties_get(props, name)) {
			obs_property_set_enabled(p, remote_on);
			obs_property_set_visible(p, vis);
		}
	};
	set_en(kPropCameraId);
	set_en(kPropCodec);
	set_en(kPropResolution);
	set_en(kPropFps);
	set_en(kPropBitrate, !is_mjpeg);
	set_en(kPropJpegQuality, is_mjpeg);
	set_en(kPropTorch);
	return true;
}

bool OnRefreshButtonClicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	(void)property;
	auto *ctx = static_cast<AwcSourceContext *>(data);
	if (!ctx)
		return false;

	ctx->RefreshFromUi(nullptr);
	ctx->PopulateDynamicProperties(props, ctx->GetSettingsSnapshot());
	if (obs_property_t *p = obs_properties_get(props, kPropStatusInfo)) {
		std::string txt = ctx->GetLastStatusText();
		obs_property_set_long_description(p, txt.c_str());
	}
	return true;
}

obs_properties_t *AwcGetProperties(void *data)
{
	auto *ctx = static_cast<AwcSourceContext *>(data);
	obs_properties_t *props = obs_properties_create();

	obs_property_t *mode = obs_properties_add_list(
		props, kPropConnectionMode,
		TextOrFallback("Awc.ConnectionMode", "Способ подключения"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(
		mode, TextOrFallback("Awc.ConnectionMode.Usb", "USB (автонастройка ADB)"), kModeUsb);
	obs_property_list_add_string(
		mode, TextOrFallback("Awc.ConnectionMode.Wifi", "Wi-Fi / USB-модем (по IP)"),
		kModeWifi);
	obs_property_set_modified_callback(mode, OnConnectionModeModified);

	obs_properties_add_list(props, kPropAdbSerial,
				TextOrFallback("Awc.AdbDevice", "USB-устройство (ADB)"),
				OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_properties_add_text(props, kPropAdbPath,
				TextOrFallback("Awc.AdbPath", "Путь к adb (необязательно)"),
				OBS_TEXT_DEFAULT);
	obs_properties_add_int(props, kPropLocalPort,
			       TextOrFallback("Awc.LocalPort", "Локальный порт USB (adb forward)"),
			       1024, 65535, 1);

	obs_properties_add_text(props, kPropWifiHost,
				TextOrFallback("Awc.WifiHost", "IP или хост телефона"),
				OBS_TEXT_DEFAULT);
	obs_properties_add_int(props, kPropRemotePort,
			       TextOrFallback("Awc.RemotePort", "Порт сервера на телефоне"), 1024,
			       65535, 1);

	obs_properties_add_button(
		props, kPropRefresh,
		TextOrFallback("Awc.Refresh", "Обновить список камер и устройств"),
		OnRefreshButtonClicked);

	obs_properties_add_text(props, kPropStatusInfo,
				TextOrFallback("Awc.StatusInfo", "Состояние подключения"),
				OBS_TEXT_INFO);

	obs_property_t *apply_remote = obs_properties_add_bool(
		props, kPropApplyRemote,
		TextOrFallback("Awc.ApplyRemoteConfig", "Управлять параметрами камеры из OBS"));
	obs_property_set_modified_callback(apply_remote, OnRemoteToggleModified);

	obs_properties_add_list(props, kPropCameraId, TextOrFallback("Awc.Camera", "Камера"),
				OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	obs_property_t *codec_prop = obs_properties_add_list(
		props, kPropCodec, TextOrFallback("Awc.Codec", "Кодек видео"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	obs_property_set_modified_callback(codec_prop, OnRemoteToggleModified);

	obs_properties_add_list(props, kPropResolution,
				TextOrFallback("Awc.Resolution", "Разрешение"), OBS_COMBO_TYPE_LIST,
				OBS_COMBO_FORMAT_STRING);
	obs_properties_add_list(props, kPropFps,
				TextOrFallback("Awc.Fps", "Частота кадров (FPS)"),
				OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

	obs_properties_add_int_slider(props, kPropBitrate,
				      TextOrFallback("Awc.Bitrate", "Битрейт (Кбит/с)"), 500, 20000,
				      500);
	obs_properties_add_int_slider(props, kPropJpegQuality,
				      TextOrFallback("Awc.JpegQuality", "Качество MJPEG"), 40, 95,
				      5);

	obs_property_t *rot_prop = obs_properties_add_list(
		props, kPropRotation, TextOrFallback("Awc.Rotation", "Поворот видео"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(
		rot_prop, TextOrFallback("Awc.Rotation.Auto", "Авто (из настроек телефона)"), -1);
	obs_property_list_add_int(rot_prop, TextOrFallback("Awc.Rotation.0", "0° (без поворота)"),
				  0);
	obs_property_list_add_int(rot_prop, TextOrFallback("Awc.Rotation.90", "90° по часовой"),
				  90);
	obs_property_list_add_int(rot_prop, TextOrFallback("Awc.Rotation.180", "180°"), 180);
	obs_property_list_add_int(rot_prop, TextOrFallback("Awc.Rotation.270", "270° по часовой"),
				  270);

	obs_properties_add_bool(props, kPropMirrored,
				TextOrFallback("Awc.Mirrored", "Отразить по горизонтали (зеркало)"));
	obs_properties_add_bool(props, kPropTorch,
				TextOrFallback("Awc.Torch", "Фонарик / подсветка"));
	obs_properties_add_bool(
		props, kPropDisconnectInactive,
		TextOrFallback("Awc.DisconnectInactive", "Отключаться, когда источник скрыт"));

	if (ctx) {
		ctx->PopulateDynamicProperties(props, ctx->GetSettingsSnapshot());
	} else {
		if (obs_property_t *p = obs_properties_get(props, kPropWifiHost))
			obs_property_set_visible(p, false);
		if (obs_property_t *p = obs_properties_get(props, kPropJpegQuality))
			obs_property_set_visible(p, false);
	}

	return props;
}

struct obs_source_info CreateSourceInfo()
{
	struct obs_source_info info{};
	info.id = kSourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.get_name = AwcGetName;
	info.create = AwcCreate;
	info.destroy = AwcDestroy;
	info.get_defaults = AwcGetDefaults;
	info.get_properties = AwcGetProperties;
	info.update = AwcUpdate;
	info.activate = AwcActivate;
	info.deactivate = AwcDeactivate;
	info.icon_type = OBS_ICON_TYPE_CAMERA;
	return info;
}

} // namespace

extern "C" {

MODULE_EXPORT const char *obs_module_name(void)
{
	return "AndroidWebCamera";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "AndroidWebCamera low-latency USB (ADB) and Wi-Fi camera source";
}

MODULE_EXPORT bool obs_module_load(void)
{
	static struct obs_source_info awc_source_info = CreateSourceInfo();
	obs_register_source(&awc_source_info);
	blog(LOG_INFO, "[AndroidWebCamera] OBS plugin loaded (source id: %s)", kSourceId);
	return true;
}

MODULE_EXPORT void obs_module_unload(void)
{
	blog(LOG_INFO, "[AndroidWebCamera] OBS plugin unloaded");
}

} // extern "C"
