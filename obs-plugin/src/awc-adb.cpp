#include "awc-adb.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <sys/stat.h>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace awc {
namespace {

std::string Trim(const std::string &s)
{
	size_t start = 0;
	while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])))
		++start;
	size_t end = s.size();
	while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
		--end;
	return s.substr(start, end - start);
}

bool IsExecutableFile(const std::string &path)
{
	if (path.empty())
		return false;
#ifdef _WIN32
	DWORD attrs = GetFileAttributesA(path.c_str());
	return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
#else
	struct stat st{};
	if (stat(path.c_str(), &st) != 0)
		return false;
	if (!S_ISREG(st.st_mode))
		return false;
	return access(path.c_str(), X_OK) == 0;
#endif
}

std::string JoinPath(const std::string &a, const std::string &b)
{
	if (a.empty())
		return b;
#ifdef _WIN32
	const char sep = '\\';
#else
	const char sep = '/';
#endif
	if (a.back() == '/' || a.back() == '\\')
		return a + b;
	return a + sep + b;
}

std::string GetEnvString(const char *name)
{
	const char *v = std::getenv(name);
	return v ? std::string(v) : std::string();
}

#ifdef _WIN32
std::string QuoteCommandArgWin(const std::string &arg)
{
	if (arg.find_first_of(" \t\n\v\"") == std::string::npos && !arg.empty())
		return arg;
	std::string out = "\"";
	for (size_t i = 0; i < arg.size(); ++i) {
		size_t backslashes = 0;
		while (i < arg.size() && arg[i] == '\\') {
			++backslashes;
			++i;
		}
		if (i == arg.size()) {
			out.append(backslashes * 2, '\\');
			break;
		}
		if (arg[i] == '"') {
			out.append(backslashes * 2 + 1, '\\');
			out.push_back('"');
		} else {
			out.append(backslashes, '\\');
			out.push_back(arg[i]);
		}
	}
	out.push_back('"');
	return out;
}
#endif

} // namespace

std::string FindAdbExecutable(const std::string &explicit_path, std::string &out_error)
{
	std::string cleaned = Trim(explicit_path);
	if (!cleaned.empty()) {
		if (IsExecutableFile(cleaned))
			return cleaned;
		out_error = "adb не найден по указанному пути: " + cleaned;
		return "";
	}

	const std::string path_env = GetEnvString("PATH");
#ifdef _WIN32
	const char sep = ';';
	const std::string exe_name = "adb.exe";
#else
	const char sep = ':';
	const std::string exe_name = "adb";
#endif
	if (!path_env.empty()) {
		std::stringstream ss(path_env);
		std::string dir;
		while (std::getline(ss, dir, sep)) {
			if (dir.empty())
				continue;
			std::string candidate = JoinPath(dir, exe_name);
			if (IsExecutableFile(candidate))
				return candidate;
		}
	}

	std::vector<std::string> candidates;
#ifdef _WIN32
	std::string local_app_data = GetEnvString("LOCALAPPDATA");
	if (!local_app_data.empty()) {
		candidates.push_back(local_app_data + "\\Android\\Sdk\\platform-tools\\adb.exe");
	}
	std::string user_profile = GetEnvString("USERPROFILE");
	if (!user_profile.empty()) {
		candidates.push_back(user_profile +
				     "\\AppData\\Local\\Android\\Sdk\\platform-tools\\adb.exe");
	}
	std::string sdk_root = GetEnvString("ANDROID_SDK_ROOT");
	if (!sdk_root.empty())
		candidates.push_back(JoinPath(sdk_root, "platform-tools\\adb.exe"));
	std::string android_home = GetEnvString("ANDROID_HOME");
	if (!android_home.empty())
		candidates.push_back(JoinPath(android_home, "platform-tools\\adb.exe"));
#else
	std::string sdk_root = GetEnvString("ANDROID_SDK_ROOT");
	if (!sdk_root.empty())
		candidates.push_back(JoinPath(sdk_root, "platform-tools/adb"));
	std::string android_home = GetEnvString("ANDROID_HOME");
	if (!android_home.empty())
		candidates.push_back(JoinPath(android_home, "platform-tools/adb"));
	std::string home = GetEnvString("HOME");
	if (!home.empty())
		candidates.push_back(home + "/Android/Sdk/platform-tools/adb");
	candidates.push_back("/usr/bin/adb");
	candidates.push_back("/usr/local/bin/adb");
#endif

	for (const auto &c : candidates) {
		if (IsExecutableFile(c))
			return c;
	}

	out_error = "утилита adb не найдена в PATH (установите android-tools / Android "
		    "Platform-Tools или укажите путь в настройках)";
	return "";
}

std::vector<AdbDevice> ParseAdbDevices(const std::string &output)
{
	std::vector<AdbDevice> devices;
	std::istringstream iss(output);
	std::string line;
	while (std::getline(iss, line)) {
		line = Trim(line);
		if (line.empty())
			continue;
		if (line.rfind("List of devices", 0) == 0 || line.front() == '*')
			continue;

		std::istringstream ls(line);
		std::string serial;
		std::string state;
		if (!(ls >> serial >> state))
			continue;

		AdbDevice dev;
		dev.serial = std::move(serial);
		dev.state = std::move(state);
		std::string token;
		while (ls >> token) {
			if (token.rfind("model:", 0) == 0) {
				dev.model = token.substr(6);
				break;
			}
		}
		devices.push_back(std::move(dev));
	}
	return devices;
}

bool PickOnlineDevice(const std::vector<AdbDevice> &devices, const std::string &preferred_serial,
		      AdbDevice &out_device, std::string &out_error)
{
	if (devices.empty()) {
		out_error = "Android-устройство не найдено по USB (проверьте кабель и включите "
			    "Отладку по USB)";
		return false;
	}

	if (!preferred_serial.empty()) {
		for (const auto &d : devices) {
			if (d.serial == preferred_serial) {
				if (d.state != "device") {
					out_error = "устройство " + d.serial + " имеет статус '" +
						    d.state +
						    "' (подтвердите отладку по USB на телефоне)";
					return false;
				}
				out_device = d;
				return true;
			}
		}
		out_error = "устройство с serial '" + preferred_serial + "' не найдено";
		return false;
	}

	std::vector<AdbDevice> online;
	for (const auto &d : devices) {
		if (d.state == "device")
			online.push_back(d);
	}
	if (online.empty()) {
		out_error = "устройство найдено (" + devices[0].serial +
			    "), но не авторизовано (статус: " + devices[0].state +
			    "). Разблокируйте телефон и разрешите отладку по USB";
		return false;
	}
	if (online.size() > 1) {
		std::string serials;
		for (size_t i = 0; i < online.size(); ++i) {
			if (i > 0)
				serials += ", ";
			serials += online[i].serial;
		}
		out_error = "подключено несколько устройств (" + serials +
			    "); выберите нужное в списке USB-устройств";
		return false;
	}

	out_device = online[0];
	return true;
}

AdbManager::AdbManager(std::string adb_bin) : adb_bin_(std::move(adb_bin)) {}

bool AdbManager::RunAdb(const std::vector<std::string> &args, int timeout_ms,
			std::string &out_stdout, std::string &out_error) const
{
	out_stdout.clear();
	if (adb_bin_.empty()) {
		out_error = "Путь к adb не задан";
		return false;
	}

#ifdef _WIN32
	SECURITY_ATTRIBUTES sa{};
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;

	HANDLE out_read = nullptr, out_write = nullptr;
	HANDLE err_read = nullptr, err_write = nullptr;
	if (!CreatePipe(&out_read, &out_write, &sa, 0) ||
	    !CreatePipe(&err_read, &err_write, &sa, 0)) {
		if (out_read)
			CloseHandle(out_read);
		if (out_write)
			CloseHandle(out_write);
		if (err_read)
			CloseHandle(err_read);
		if (err_write)
			CloseHandle(err_write);
		out_error = "Не удалось создать pipe для adb";
		return false;
	}
	SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);

	std::string cmdline = QuoteCommandArgWin(adb_bin_);
	for (const auto &a : args) {
		cmdline += ' ';
		cmdline += QuoteCommandArgWin(a);
	}
	std::vector<char> mutable_cmd(cmdline.begin(), cmdline.end());
	mutable_cmd.push_back('\0');

	STARTUPINFOA si{};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	si.hStdOutput = out_write;
	si.hStdError = err_write;
	si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

	PROCESS_INFORMATION pi{};
	BOOL created = CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE,
				      CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
	CloseHandle(out_write);
	CloseHandle(err_write);

	if (!created) {
		CloseHandle(out_read);
		CloseHandle(err_read);
		out_error = "Не удалось запустить adb: код " + std::to_string(GetLastError());
		return false;
	}

	auto deadline =
		std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(timeout_ms, 500));
	std::string stderr_str;
	char buf[1024];
	bool timed_out = false;

	auto drain_pipe = [&](HANDLE h, std::string &dst) {
		DWORD avail = 0;
		while (PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
			DWORD read_bytes = 0;
			DWORD to_read = std::min<DWORD>(avail, sizeof(buf));
			if (!ReadFile(h, buf, to_read, &read_bytes, nullptr) || read_bytes == 0)
				break;
			dst.append(buf, read_bytes);
		}
	};

	while (true) {
		drain_pipe(out_read, out_stdout);
		drain_pipe(err_read, stderr_str);
		DWORD wait_rc = WaitForSingleObject(pi.hProcess, 25);
		if (wait_rc == WAIT_OBJECT_0)
			break;
		if (std::chrono::steady_clock::now() >= deadline) {
			timed_out = true;
			TerminateProcess(pi.hProcess, 1);
			break;
		}
	}
	drain_pipe(out_read, out_stdout);
	drain_pipe(err_read, stderr_str);

	DWORD exit_code = 1;
	GetExitCodeProcess(pi.hProcess, &exit_code);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	CloseHandle(out_read);
	CloseHandle(err_read);

	if (timed_out) {
		out_error = "Таймаут выполнения adb";
		return false;
	}
	if (exit_code != 0) {
		std::string msg = Trim(stderr_str);
		if (msg.empty())
			msg = "код завершения " + std::to_string(exit_code);
		out_error = "adb: " + msg;
		return false;
	}
	return true;
#else
	int out_pipe[2] = {-1, -1};
	int err_pipe[2] = {-1, -1};
	if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
		if (out_pipe[0] >= 0) {
			close(out_pipe[0]);
			close(out_pipe[1]);
		}
		if (err_pipe[0] >= 0) {
			close(err_pipe[0]);
			close(err_pipe[1]);
		}
		out_error = "Не удалось создать pipe для adb";
		return false;
	}

	pid_t pid = fork();
	if (pid < 0) {
		close(out_pipe[0]);
		close(out_pipe[1]);
		close(err_pipe[0]);
		close(err_pipe[1]);
		out_error = "Не удалось запустить процесс adb (fork failed)";
		return false;
	}

	if (pid == 0) {
		dup2(out_pipe[1], STDOUT_FILENO);
		dup2(err_pipe[1], STDERR_FILENO);
		close(out_pipe[0]);
		close(out_pipe[1]);
		close(err_pipe[0]);
		close(err_pipe[1]);

		std::vector<char *> argv;
		argv.reserve(args.size() + 2);
		argv.push_back(const_cast<char *>(adb_bin_.c_str()));
		for (const auto &a : args) {
			argv.push_back(const_cast<char *>(a.c_str()));
		}
		argv.push_back(nullptr);
		execvp(adb_bin_.c_str(), argv.data());
		_exit(127);
	}

	close(out_pipe[1]);
	close(err_pipe[1]);

	fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);
	fcntl(err_pipe[0], F_SETFL, O_NONBLOCK);

	std::string stderr_str;
	bool out_open = true;
	bool err_open = true;
	bool timed_out = false;
	auto deadline =
		std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(timeout_ms, 500));

	while (out_open || err_open) {
		auto now = std::chrono::steady_clock::now();
		if (now >= deadline) {
			timed_out = true;
			kill(pid, SIGKILL);
			break;
		}
		int remain_ms = static_cast<int>(
			std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());

		struct pollfd fds[2]{};
		nfds_t nfds = 0;
		if (out_open) {
			fds[nfds].fd = out_pipe[0];
			fds[nfds].events = POLLIN;
			++nfds;
		}
		if (err_open) {
			fds[nfds].fd = err_pipe[0];
			fds[nfds].events = POLLIN;
			++nfds;
		}

		int rc = poll(fds, nfds, std::min(remain_ms, 100));
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		char buf[1024];
		for (nfds_t i = 0; i < nfds; ++i) {
			if (fds[i].revents & (POLLIN | POLLHUP)) {
				ssize_t n = read(fds[i].fd, buf, sizeof(buf));
				if (n > 0) {
					if (fds[i].fd == out_pipe[0])
						out_stdout.append(buf, static_cast<size_t>(n));
					else
						stderr_str.append(buf, static_cast<size_t>(n));
				} else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
					if (fds[i].fd == out_pipe[0])
						out_open = false;
					else
						err_open = false;
				}
			}
		}
	}

	close(out_pipe[0]);
	close(err_pipe[0]);

	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
	}

	if (timed_out) {
		out_error = "Таймаут выполнения adb";
		return false;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		std::string msg = Trim(stderr_str);
		if (msg.empty()) {
			msg = WIFEXITED(status) ? ("код завершения " + std::to_string(WEXITSTATUS(status)))
						: "процесс завершён сигналом";
		}
		out_error = "adb: " + msg;
		return false;
	}
	return true;
#endif
}

bool AdbManager::ListDevices(std::vector<AdbDevice> &out_devices, std::string &out_error,
			     int timeout_ms) const
{
	std::string stdout_str;
	if (!RunAdb({"devices", "-l"}, timeout_ms, stdout_str, out_error))
		return false;
	out_devices = ParseAdbDevices(stdout_str);
	return true;
}

bool AdbManager::SetupForward(const std::string &preferred_serial, int local_port, int remote_port,
			      std::string &out_resolved_serial, std::string &out_error,
			      int timeout_ms) const
{
	std::vector<AdbDevice> devices;
	if (!ListDevices(devices, out_error, timeout_ms))
		return false;

	AdbDevice dev;
	if (!PickOnlineDevice(devices, preferred_serial, dev, out_error))
		return false;

	std::vector<std::string> args = {"-s", dev.serial, "forward",
					 "tcp:" + std::to_string(local_port),
					 "tcp:" + std::to_string(remote_port)};
	std::string stdout_str;
	if (!RunAdb(args, timeout_ms, stdout_str, out_error))
		return false;

	out_resolved_serial = dev.serial;
	return true;
}

bool AdbManager::RemoveForward(const std::string &serial, int local_port, std::string &out_error,
			       int timeout_ms) const
{
	std::vector<std::string> args;
	if (!serial.empty()) {
		args.push_back("-s");
		args.push_back(serial);
	}
	args.push_back("forward");
	args.push_back("--remove");
	args.push_back("tcp:" + std::to_string(local_port));
	std::string stdout_str;
	return RunAdb(args, timeout_ms, stdout_str, out_error);
}

} // namespace awc
