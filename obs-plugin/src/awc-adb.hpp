#pragma once

#include <string>
#include <vector>

namespace awc {

struct AdbDevice {
	std::string serial;
	std::string state;
	std::string model;
};

/** Locates the adb binary from explicit path, PATH, or standard Android SDK locations. */
std::string FindAdbExecutable(const std::string &explicit_path, std::string &out_error);

/** Parses `adb devices -l` stdout into device descriptors. */
std::vector<AdbDevice> ParseAdbDevices(const std::string &output);

/** Selects an authorized online Android device or returns an actionable error message. */
bool PickOnlineDevice(const std::vector<AdbDevice> &devices, const std::string &preferred_serial,
		      AdbDevice &out_device, std::string &out_error);

/** Cross-platform wrapper around the `adb` CLI. */
class AdbManager {
public:
	explicit AdbManager(std::string adb_bin);

	const std::string &Binary() const { return adb_bin_; }

	bool ListDevices(std::vector<AdbDevice> &out_devices, std::string &out_error,
			 int timeout_ms = 5000) const;

	bool SetupForward(const std::string &preferred_serial, int local_port, int remote_port,
			  std::string &out_resolved_serial, std::string &out_error,
			  int timeout_ms = 5000) const;

	bool RemoveForward(const std::string &serial, int local_port, std::string &out_error,
			   int timeout_ms = 3000) const;

private:
	bool RunAdb(const std::vector<std::string> &args, int timeout_ms, std::string &out_stdout,
		    std::string &out_error) const;

	std::string adb_bin_;
};

} // namespace awc
