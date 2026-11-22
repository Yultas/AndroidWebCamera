#include "awc-client.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using SocketHandle = SOCKET;
static constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
using SocketHandle = int;
static constexpr SocketHandle kInvalidSocket = -1;
#endif

namespace awc {
namespace {

std::string Trim(const std::string &s)
{
	size_t start = 0;
	while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
		++start;
	}
	size_t end = s.size();
	while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
		--end;
	}
	return s.substr(start, end - start);
}

std::string ToLower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return s;
}

std::string EscapeJsonString(const std::string &s)
{
	std::string out;
	out.reserve(s.size() + 8);
	for (unsigned char c : s) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\b':
			out += "\\b";
			break;
		case '\f':
			out += "\\f";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (c < 0x20) {
				char buf[8];
				std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
				out += buf;
			} else {
				out.push_back(static_cast<char>(c));
			}
			break;
		}
	}
	return out;
}

// Lightweight recursive-descent JSON AST for /api/status and /api/config payloads.
struct JsonValue {
	enum class Type { Null, Bool, Number, String, Array, Object };

	Type type = Type::Null;
	bool bool_val = false;
	double num_val = 0.0;
	std::string str_val;
	std::vector<JsonValue> arr_val;
	std::map<std::string, JsonValue> obj_val;

	const JsonValue *Get(const std::string &key) const
	{
		if (type != Type::Object)
			return nullptr;
		auto it = obj_val.find(key);
		return it == obj_val.end() ? nullptr : &it->second;
	}

	std::string AsString(const std::string &def = "") const
	{
		return type == Type::String ? str_val : def;
	}

	bool AsBool(bool def = false) const
	{
		return type == Type::Bool ? bool_val : def;
	}

	int AsInt(int def = 0) const
	{
		return type == Type::Number ? static_cast<int>(num_val) : def;
	}
};

class JsonParser {
public:
	explicit JsonParser(const std::string &input) : src_(input) {}

	bool Parse(JsonValue &out, std::string &err)
	{
		SkipWs();
		if (!ParseValue(out, err, 0))
			return false;
		SkipWs();
		if (pos_ != src_.size()) {
			err = "Unexpected trailing characters in JSON";
			return false;
		}
		return true;
	}

private:
	static constexpr int kMaxDepth = 32;
	const std::string &src_;
	size_t pos_ = 0;

	void SkipWs()
	{
		while (pos_ < src_.size() &&
		       std::isspace(static_cast<unsigned char>(src_[pos_]))) {
			++pos_;
		}
	}

	bool Match(char expected)
	{
		if (pos_ < src_.size() && src_[pos_] == expected) {
			++pos_;
			return true;
		}
		return false;
	}

	bool ParseValue(JsonValue &out, std::string &err, int depth)
	{
		if (depth > kMaxDepth) {
			err = "JSON nesting too deep";
			return false;
		}
		SkipWs();
		if (pos_ >= src_.size()) {
			err = "Unexpected end of JSON";
			return false;
		}
		char c = src_[pos_];
		if (c == '{')
			return ParseObject(out, err, depth + 1);
		if (c == '[')
			return ParseArray(out, err, depth + 1);
		if (c == '"') {
			out = JsonValue{};
			out.type = JsonValue::Type::String;
			return ParseString(out.str_val, err);
		}
		if (c == 't' && src_.compare(pos_, 4, "true") == 0) {
			pos_ += 4;
			out = JsonValue{};
			out.type = JsonValue::Type::Bool;
			out.bool_val = true;
			return true;
		}
		if (c == 'f' && src_.compare(pos_, 5, "false") == 0) {
			pos_ += 5;
			out = JsonValue{};
			out.type = JsonValue::Type::Bool;
			out.bool_val = false;
			return true;
		}
		if (c == 'n' && src_.compare(pos_, 4, "null") == 0) {
			pos_ += 4;
			out = JsonValue{};
			out.type = JsonValue::Type::Null;
			return true;
		}
		if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
			return ParseNumber(out, err);
		}
		err = std::string("Invalid JSON token near '") + c + "'";
		return false;
	}

	static void AppendUtf8(uint32_t cp, std::string &out)
	{
		if (cp <= 0x7F) {
			out.push_back(static_cast<char>(cp));
		} else if (cp <= 0x7FF) {
			out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		} else if (cp <= 0xFFFF) {
			out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		} else {
			out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		}
	}

	bool ParseHex4(uint32_t &out_cp)
	{
		if (pos_ + 4 > src_.size())
			return false;
		uint32_t v = 0;
		for (int i = 0; i < 4; ++i) {
			char ch = src_[pos_++];
			v <<= 4;
			if (ch >= '0' && ch <= '9')
				v |= static_cast<uint32_t>(ch - '0');
			else if (ch >= 'a' && ch <= 'f')
				v |= static_cast<uint32_t>(10 + ch - 'a');
			else if (ch >= 'A' && ch <= 'F')
				v |= static_cast<uint32_t>(10 + ch - 'A');
			else
				return false;
		}
		out_cp = v;
		return true;
	}

	bool ParseString(std::string &out, std::string &err)
	{
		if (!Match('"')) {
			err = "Expected '\"'";
			return false;
		}
		out.clear();
		while (pos_ < src_.size()) {
			char c = src_[pos_++];
			if (c == '"')
				return true;
			if (c == '\\') {
				if (pos_ >= src_.size()) {
					err = "Unterminated escape in string";
					return false;
				}
				char esc = src_[pos_++];
				switch (esc) {
				case '"':
				case '\\':
				case '/':
					out.push_back(esc);
					break;
				case 'b':
					out.push_back('\b');
					break;
				case 'f':
					out.push_back('\f');
					break;
				case 'n':
					out.push_back('\n');
					break;
				case 'r':
					out.push_back('\r');
					break;
				case 't':
					out.push_back('\t');
					break;
				case 'u': {
					uint32_t cp = 0;
					if (!ParseHex4(cp)) {
						err = "Invalid \\uXXXX escape";
						return false;
					}
					if (cp >= 0xD800 && cp <= 0xDBFF &&
					    pos_ + 6 <= src_.size() && src_[pos_] == '\\' &&
					    src_[pos_ + 1] == 'u') {
						size_t saved = pos_;
						pos_ += 2;
						uint32_t low = 0;
						if (ParseHex4(low) && low >= 0xDC00 &&
						    low <= 0xDFFF) {
							cp = 0x10000 + (((cp - 0xD800) << 10) |
									(low - 0xDC00));
						} else {
							pos_ = saved;
						}
					}
					AppendUtf8(cp, out);
					break;
				}
				default:
					err = "Invalid JSON escape";
					return false;
				}
			} else {
				out.push_back(c);
			}
		}
		err = "Unterminated JSON string";
		return false;
	}

	bool ParseNumber(JsonValue &out, std::string &err)
	{
		size_t start = pos_;
		if (src_[pos_] == '-')
			++pos_;
		if (pos_ >= src_.size() || !std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
			err = "Invalid JSON number";
			return false;
		}
		while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_])))
			++pos_;
		if (pos_ < src_.size() && src_[pos_] == '.') {
			++pos_;
			if (pos_ >= src_.size() ||
			    !std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
				err = "Invalid JSON fraction";
				return false;
			}
			while (pos_ < src_.size() &&
			       std::isdigit(static_cast<unsigned char>(src_[pos_])))
				++pos_;
		}
		if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
			++pos_;
			if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-'))
				++pos_;
			if (pos_ >= src_.size() ||
			    !std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
				err = "Invalid JSON exponent";
				return false;
			}
			while (pos_ < src_.size() &&
			       std::isdigit(static_cast<unsigned char>(src_[pos_])))
				++pos_;
		}
		const std::string token = src_.substr(start, pos_ - start);
		char *endptr = nullptr;
		double val = std::strtod(token.c_str(), &endptr);
		if (endptr == token.c_str()) {
			err = "Failed to parse JSON number";
			return false;
		}
		out = JsonValue{};
		out.type = JsonValue::Type::Number;
		out.num_val = val;
		return true;
	}

	bool ParseArray(JsonValue &out, std::string &err, int depth)
	{
		if (!Match('[')) {
			err = "Expected '['";
			return false;
		}
		out = JsonValue{};
		out.type = JsonValue::Type::Array;
		SkipWs();
		if (Match(']'))
			return true;
		while (true) {
			JsonValue item;
			if (!ParseValue(item, err, depth))
				return false;
			out.arr_val.push_back(std::move(item));
			SkipWs();
			if (Match(']'))
				return true;
			if (!Match(',')) {
				err = "Expected ',' or ']' in array";
				return false;
			}
		}
	}

	bool ParseObject(JsonValue &out, std::string &err, int depth)
	{
		if (!Match('{')) {
			err = "Expected '{'";
			return false;
		}
		out = JsonValue{};
		out.type = JsonValue::Type::Object;
		SkipWs();
		if (Match('}'))
			return true;
		while (true) {
			SkipWs();
			std::string key;
			if (!ParseString(key, err))
				return false;
			SkipWs();
			if (!Match(':')) {
				err = "Expected ':' after object key";
				return false;
			}
			JsonValue val;
			if (!ParseValue(val, err, depth))
				return false;
			out.obj_val[std::move(key)] = std::move(val);
			SkipWs();
			if (Match('}'))
				return true;
			if (!Match(',')) {
				err = "Expected ',' or '}' in object";
				return false;
			}
		}
	}
};

#ifdef _WIN32
struct WinsockInit {
	WinsockInit()
	{
		WSADATA wsa{};
		WSAStartup(MAKEWORD(2, 2), &wsa);
	}
};
static void EnsureWinsock()
{
	static WinsockInit init;
}
#endif

void CloseSocket(SocketHandle sock)
{
	if (sock == kInvalidSocket)
		return;
#ifdef _WIN32
	closesocket(sock);
#else
	close(sock);
#endif
}

bool SetNonBlocking(SocketHandle sock, bool nb)
{
#ifdef _WIN32
	u_long mode = nb ? 1UL : 0UL;
	return ioctlsocket(sock, FIONBIO, &mode) == 0;
#else
	int flags = fcntl(sock, F_GETFL, 0);
	if (flags < 0)
		return false;
	return fcntl(sock, F_SETFL, nb ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0;
#endif
}

void SetSocketTimeouts(SocketHandle sock, int timeout_ms)
{
#ifdef _WIN32
	DWORD tv = static_cast<DWORD>(std::max(timeout_ms, 100));
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&tv), sizeof(tv));
	setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&tv), sizeof(tv));
#else
	struct timeval tv{};
	int ms = std::max(timeout_ms, 100);
	tv.tv_sec = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

SocketHandle ConnectWithTimeout(const std::string &host, int port, int timeout_ms,
				std::string &out_error)
{
#ifdef _WIN32
	EnsureWinsock();
#endif
	struct addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	char port_buf[16];
	std::snprintf(port_buf, sizeof(port_buf), "%d", port);

	struct addrinfo *res = nullptr;
	int gai = getaddrinfo(host.c_str(), port_buf, &hints, &res);
	if (gai != 0 || !res) {
		out_error = "Не удалось разрешить адрес " + host + ":" + port_buf;
		return kInvalidSocket;
	}

	SocketHandle connected = kInvalidSocket;
	for (struct addrinfo *p = res; p != nullptr; p = p->ai_next) {
		SocketHandle sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (sock == kInvalidSocket)
			continue;

		SetNonBlocking(sock, true);
		int rc = connect(sock, p->ai_addr, static_cast<int>(p->ai_addrlen));
		bool ok = false;
		if (rc == 0) {
			ok = true;
		} else {
#ifdef _WIN32
			int err = WSAGetLastError();
			if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS) {
				fd_set wfds;
				FD_ZERO(&wfds);
				FD_SET(sock, &wfds);
				struct timeval tv{};
				int ms = std::max(timeout_ms, 100);
				tv.tv_sec = ms / 1000;
				tv.tv_usec = (ms % 1000) * 1000;
				if (select(0, nullptr, &wfds, nullptr, &tv) > 0) {
					int so_err = 0;
					int len = sizeof(so_err);
					getsockopt(sock, SOL_SOCKET, SO_ERROR,
						   reinterpret_cast<char *>(&so_err), &len);
					ok = (so_err == 0);
				}
			}
#else
			if (errno == EINPROGRESS) {
				struct pollfd pfd{};
				pfd.fd = sock;
				pfd.events = POLLOUT;
				if (poll(&pfd, 1, std::max(timeout_ms, 100)) > 0) {
					int so_err = 0;
					socklen_t len = sizeof(so_err);
					getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_err, &len);
					ok = (so_err == 0);
				}
			}
#endif
		}

		if (ok) {
			SetNonBlocking(sock, false);
			SetSocketTimeouts(sock, timeout_ms);
			connected = sock;
			break;
		}
		CloseSocket(sock);
	}
	freeaddrinfo(res);

	if (connected == kInvalidSocket) {
		out_error = "Не удалось подключиться к " + host + ":" + port_buf;
	}
	return connected;
}

bool SendAll(SocketHandle sock, const char *data, size_t len)
{
	size_t sent = 0;
	while (sent < len) {
		int n = send(sock, data + sent, static_cast<int>(len - sent), 0);
		if (n <= 0)
			return false;
		sent += static_cast<size_t>(n);
	}
	return true;
}

bool DecodeChunkedBody(const std::string &raw, std::string &out_body)
{
	out_body.clear();
	size_t pos = 0;
	while (pos < raw.size()) {
		size_t line_end = raw.find("\r\n", pos);
		if (line_end == std::string::npos)
			return false;
		std::string size_str = raw.substr(pos, line_end - pos);
		size_t semi = size_str.find(';');
		if (semi != std::string::npos)
			size_str.resize(semi);
		unsigned long chunk_len = std::strtoul(size_str.c_str(), nullptr, 16);
		pos = line_end + 2;
		if (chunk_len == 0)
			return true;
		if (pos + chunk_len > raw.size())
			return false;
		out_body.append(raw, pos, chunk_len);
		pos += chunk_len;
		if (pos + 2 <= raw.size() && raw[pos] == '\r' && raw[pos + 1] == '\n')
			pos += 2;
	}
	return true;
}

bool PerformHttpRequest(const std::string &method, const std::string &host, int port,
			const std::string &path, const std::string &body, int timeout_ms,
			int &out_status_code, std::string &out_body, std::string &out_error)
{
	std::string clean_host = NormalizeHostInput(host);
	if (clean_host.empty()) {
		out_error = "Пустой IP/хост устройства";
		return false;
	}

	SocketHandle sock = ConnectWithTimeout(clean_host, port, timeout_ms, out_error);
	if (sock == kInvalidSocket)
		return false;

	std::ostringstream req;
	req << method << " " << path << " HTTP/1.1\r\n"
	    << "Host: " << clean_host << ":" << port << "\r\n"
	    << "Connection: close\r\n";
	if (!body.empty() || method == "POST") {
		req << "Content-Type: application/json\r\n"
		    << "Content-Length: " << body.size() << "\r\n";
	}
	req << "\r\n" << body;

	const std::string req_str = req.str();
	if (!SendAll(sock, req_str.data(), req_str.size())) {
		CloseSocket(sock);
		out_error = "Ошибка отправки HTTP-запроса";
		return false;
	}

	std::string raw_resp;
	char buf[4096];
	constexpr size_t kMaxRespBytes = 2 * 1024 * 1024;
	while (raw_resp.size() < kMaxRespBytes) {
		int n = recv(sock, buf, sizeof(buf), 0);
		if (n <= 0)
			break;
		raw_resp.append(buf, static_cast<size_t>(n));
	}
	CloseSocket(sock);

	size_t hdr_end = raw_resp.find("\r\n\r\n");
	size_t sep_len = 4;
	if (hdr_end == std::string::npos) {
		hdr_end = raw_resp.find("\n\n");
		sep_len = 2;
	}
	if (hdr_end == std::string::npos) {
		out_error = "Некорректный HTTP-ответ от телефона";
		return false;
	}

	const std::string hdrs = raw_resp.substr(0, hdr_end);
	std::string resp_body = raw_resp.substr(hdr_end + sep_len);

	size_t first_eol = hdrs.find('\n');
	std::string status_line = first_eol == std::string::npos ? hdrs : hdrs.substr(0, first_eol);
	size_t sp = status_line.find(' ');
	if (sp == std::string::npos) {
		out_error = "Некорректная строка статуса HTTP";
		return false;
	}
	out_status_code = std::atoi(status_line.c_str() + sp + 1);

	std::string lower_hdrs = ToLower(hdrs);
	if (lower_hdrs.find("transfer-encoding: chunked") != std::string::npos) {
		std::string decoded;
		if (DecodeChunkedBody(resp_body, decoded)) {
			resp_body = std::move(decoded);
		}
	}

	out_body = std::move(resp_body);
	return true;
}

std::string ExtractJsonError(const std::string &body)
{
	JsonValue root;
	std::string parse_err;
	JsonParser parser(body);
	if (parser.Parse(root, parse_err)) {
		if (const JsonValue *e = root.Get("error")) {
			if (e->type == JsonValue::Type::String && !e->str_val.empty())
				return e->str_val;
		}
	}
	return Trim(body);
}

} // namespace

bool ConfigPatch::IsEmpty() const
{
	return !has_camera_id && !has_width && !has_height && !has_fps && !has_codec &&
	       !has_bitrate_kbps && !has_jpeg_quality && !has_rotation && !has_mirrored &&
	       !has_torch_enabled;
}

std::string ConfigPatch::ToJson() const
{
	std::ostringstream oss;
	oss << '{';
	bool first = true;
	auto comma = [&]() {
		if (!first)
			oss << ',';
		first = false;
	};

	if (has_camera_id) {
		comma();
		oss << "\"cameraId\":\"" << EscapeJsonString(camera_id) << "\"";
	}
	if (has_width) {
		comma();
		oss << "\"width\":" << width;
	}
	if (has_height) {
		comma();
		oss << "\"height\":" << height;
	}
	if (has_fps) {
		comma();
		oss << "\"fps\":" << fps;
	}
	if (has_codec) {
		comma();
		oss << "\"codec\":\"" << EscapeJsonString(codec) << "\"";
	}
	if (has_bitrate_kbps) {
		comma();
		oss << "\"bitrateKbps\":" << bitrate_kbps;
	}
	if (has_jpeg_quality) {
		comma();
		oss << "\"jpegQuality\":" << jpeg_quality;
	}
	if (has_rotation) {
		comma();
		oss << "\"rotation\":" << rotation;
	}
	if (has_mirrored) {
		comma();
		oss << "\"mirrored\":" << (mirrored ? "true" : "false");
	}
	if (has_torch_enabled) {
		comma();
		oss << "\"torchEnabled\":" << (torch_enabled ? "true" : "false");
	}

	oss << '}';
	return oss.str();
}

std::string NormalizeCodec(const std::string &codec)
{
	const std::string c = ToLower(Trim(codec));
	if (c == "h264" || c == "avc")
		return "h264";
	if (c == "h265" || c == "hevc")
		return "h265";
	if (c == "av1" || c == "av01")
		return "av1";
	if (c == "mjpeg" || c == "mjpg")
		return "mjpeg";
	return "";
}

std::string StreamPathForCodec(const std::string &codec)
{
	const std::string canon = NormalizeCodec(codec);
	if (canon == "h265")
		return "/stream.h265";
	if (canon == "av1")
		return "/stream.av1";
	if (canon == "mjpeg")
		return "/stream.mjpg";
	return "/stream.h264";
}

std::string StreamUrlForCodec(const std::string &base_url, const std::string &codec)
{
	std::string base = base_url;
	while (!base.empty() && base.back() == '/') {
		base.pop_back();
	}
	return base + StreamPathForCodec(codec);
}

std::string NormalizeHostInput(const std::string &raw_host, int *optional_port)
{
	std::string s = Trim(raw_host);
	if (s.rfind("http://", 0) == 0)
		s = s.substr(7);
	else if (s.rfind("https://", 0) == 0)
		s = s.substr(8);

	size_t slash = s.find('/');
	if (slash != std::string::npos)
		s = s.substr(0, slash);

	// Extract optional :port only if not a bare bracketed IPv6 literal without trailing port.
	if (!s.empty() && s.front() == '[') {
		size_t close_br = s.find(']');
		if (close_br != std::string::npos) {
			if (optional_port && close_br + 1 < s.size() && s[close_br + 1] == ':') {
				int p = std::atoi(s.c_str() + close_br + 2);
				if (p > 0 && p <= 65535)
					*optional_port = p;
			}
			return s.substr(1, close_br - 1);
		}
	}

	size_t colon = s.find(':');
	if (colon != std::string::npos && s.find(':', colon + 1) == std::string::npos) {
		if (optional_port) {
			int p = std::atoi(s.c_str() + colon + 1);
			if (p > 0 && p <= 65535)
				*optional_port = p;
		}
		s = s.substr(0, colon);
	}
	return Trim(s);
}

bool ParseStatusJson(const std::string &json_str, DeviceStatus &out_status, std::string &out_error)
{
	JsonValue root;
	JsonParser parser(json_str);
	if (!parser.Parse(root, out_error)) {
		out_error = "Некорректный JSON от сервера: " + out_error;
		return false;
	}
	if (root.type != JsonValue::Type::Object) {
		out_error = "Ожидался JSON-объект от /api/status";
		return false;
	}

	DeviceStatus st;
	if (const JsonValue *v = root.Get("streaming"))
		st.streaming = v->AsBool();
	if (const JsonValue *v = root.Get("cameraId"))
		st.camera_id = v->AsString();
	if (const JsonValue *v = root.Get("cameraName"))
		st.camera_name = v->AsString();
	if (const JsonValue *v = root.Get("width"))
		st.width = v->AsInt();
	if (const JsonValue *v = root.Get("height"))
		st.height = v->AsInt();
	if (const JsonValue *v = root.Get("fps"))
		st.fps = v->AsInt();
	if (const JsonValue *v = root.Get("measuredFps"))
		st.measured_fps = v->AsInt();
	if (const JsonValue *v = root.Get("codec"))
		st.codec = v->AsString();
	if (const JsonValue *v = root.Get("streamPath"))
		st.stream_path = v->AsString();
	if (const JsonValue *v = root.Get("bitrateKbps"))
		st.bitrate_kbps = v->AsInt();
	if (const JsonValue *v = root.Get("jpegQuality"))
		st.jpeg_quality = v->AsInt();
	if (const JsonValue *v = root.Get("port"))
		st.port = v->AsInt();
	if (const JsonValue *v = root.Get("rotation"))
		st.rotation = v->AsInt();
	if (const JsonValue *v = root.Get("mirrored"))
		st.mirrored = v->AsBool();
	if (const JsonValue *v = root.Get("torchEnabled"))
		st.torch_enabled = v->AsBool();
	if (const JsonValue *v = root.Get("hasFlash"))
		st.has_flash = v->AsBool();
	if (const JsonValue *v = root.Get("clients"))
		st.clients = v->AsInt();

	if (const JsonValue *arr = root.Get("supportedCodecs");
	    arr && arr->type == JsonValue::Type::Array) {
		for (const auto &item : arr->arr_val) {
			if (item.type != JsonValue::Type::Object)
				continue;
			CodecInfo ci;
			if (const JsonValue *v = item.Get("id"))
				ci.id = v->AsString();
			if (const JsonValue *v = item.Get("name"))
				ci.name = v->AsString();
			if (const JsonValue *v = item.Get("supported"))
				ci.supported = v->AsBool();
			if (const JsonValue *v = item.Get("hardware"))
				ci.hardware = v->AsBool();
			if (const JsonValue *v = item.Get("streamPath"))
				ci.stream_path = v->AsString();
			if (const JsonValue *v = item.Get("encoder"))
				ci.encoder = v->AsString();
			st.supported_codecs.push_back(std::move(ci));
		}
	}

	if (const JsonValue *cams = root.Get("cameras");
	    cams && cams->type == JsonValue::Type::Array) {
		for (const auto &cam_val : cams->arr_val) {
			if (cam_val.type != JsonValue::Type::Object)
				continue;
			CameraInfo cam;
			if (const JsonValue *v = cam_val.Get("id"))
				cam.id = v->AsString();
			if (const JsonValue *v = cam_val.Get("name"))
				cam.name = v->AsString();
			if (const JsonValue *v = cam_val.Get("facing"))
				cam.facing = v->AsString();
			if (const JsonValue *v = cam_val.Get("hasFlash"))
				cam.has_flash = v->AsBool();
			if (const JsonValue *v = cam_val.Get("sensorOrientation"))
				cam.sensor_orientation = v->AsInt();

			if (const JsonValue *res_arr = cam_val.Get("resolutions");
			    res_arr && res_arr->type == JsonValue::Type::Array) {
				for (const auto &res_val : res_arr->arr_val) {
					if (res_val.type != JsonValue::Type::Object)
						continue;
					ResolutionInfo res;
					if (const JsonValue *v = res_val.Get("width"))
						res.width = v->AsInt();
					if (const JsonValue *v = res_val.Get("height"))
						res.height = v->AsInt();
					if (const JsonValue *fps_arr = res_val.Get("fps");
					    fps_arr && fps_arr->type == JsonValue::Type::Array) {
						for (const auto &f : fps_arr->arr_val) {
							if (f.type == JsonValue::Type::Number)
								res.fps.push_back(f.AsInt());
						}
					}
					cam.resolutions.push_back(std::move(res));
				}
			}
			st.cameras.push_back(std::move(cam));
		}
	}

	out_status = std::move(st);
	return true;
}

bool HttpClient::GetStatus(const std::string &host, int port, int timeout_ms,
			   DeviceStatus &out_status, std::string &out_error)
{
	int status_code = 0;
	std::string body;
	if (!PerformHttpRequest("GET", host, port, "/api/status", "", timeout_ms, status_code,
				body, out_error)) {
		return false;
	}
	if (status_code != 200) {
		out_error = "HTTP " + std::to_string(status_code) + ": " + ExtractJsonError(body);
		return false;
	}
	return ParseStatusJson(body, out_status, out_error);
}

bool HttpClient::UpdateConfig(const std::string &host, int port, const ConfigPatch &patch,
			      int timeout_ms, DeviceStatus &out_status, std::string &out_error)
{
	int status_code = 0;
	std::string body;
	if (!PerformHttpRequest("POST", host, port, "/api/config", patch.ToJson(), timeout_ms,
				status_code, body, out_error)) {
		return false;
	}
	if (status_code != 200) {
		out_error = "HTTP " + std::to_string(status_code) + ": " + ExtractJsonError(body);
		return false;
	}
	return ParseStatusJson(body, out_status, out_error);
}

} // namespace awc
