#include "vcam-decoder.hpp"

#include "awc-client.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/buffer.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace awc::vcam {
namespace {

constexpr size_t kMaxWsPayloadBytes = 32u * 1024u * 1024u; // 32 MB safety cap for 4K keyframes

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

void EnsureSocketsInitialized()
{
	static std::once_flag once;
	std::call_once(once, []() {
		WSADATA wsa{};
		WSAStartup(MAKEWORD(2, 2), &wsa);
	});
}

void CloseSocketHandle(SocketHandle s)
{
	if (s != kInvalidSocket) {
		closesocket(s);
	}
}

bool SetNonBlocking(SocketHandle s, bool non_blocking)
{
	u_long mode = non_blocking ? 1ul : 0ul;
	return ioctlsocket(s, FIONBIO, &mode) == 0;
}

bool WouldBlockError()
{
	const int e = WSAGetLastError();
	return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
}
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;

void EnsureSocketsInitialized() {}

void CloseSocketHandle(SocketHandle s)
{
	if (s != kInvalidSocket) {
		close(s);
	}
}

bool SetNonBlocking(SocketHandle s, bool non_blocking)
{
	const int flags = fcntl(s, F_GETFL, 0);
	if (flags < 0) {
		return false;
	}
	return fcntl(s, F_SETFL, non_blocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0;
}

bool WouldBlockError()
{
	return errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EAGAIN;
}
#endif

void EnsureFfmpegNetworkInit()
{
	static std::once_flag once;
	std::call_once(once, []() { avformat_network_init(); });
}

std::string ToLowerCopy(const std::string &s)
{
	std::string out = s;
	for (char &c : out) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return out;
}

std::string AvErrorString(int errnum)
{
	char buf[AV_ERROR_MAX_STRING_SIZE]{};
	av_strerror(errnum, buf, sizeof(buf));
	return std::string(buf);
}

int64_t NowSteadyMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		       std::chrono::steady_clock::now().time_since_epoch())
		.count();
}

struct InterruptState {
	const std::atomic<bool> *stop_requested = nullptr;
	std::atomic<int64_t> deadline_ms{0};
};

int FfmpegInterruptCb(void *opaque)
{
	const auto *state = static_cast<const InterruptState *>(opaque);
	if (!state) {
		return 0;
	}
	if (state->stop_requested && state->stop_requested->load(std::memory_order_relaxed)) {
		return 1;
	}
	const int64_t deadline = state->deadline_ms.load(std::memory_order_relaxed);
	if (deadline > 0 && NowSteadyMs() >= deadline) {
		return 1;
	}
	return 0;
}

const char *DemuxerNameForCodec(const std::string &codec)
{
	const std::string canon = awc::NormalizeCodec(codec);
	if (canon == "h265") return "hevc";
	if (canon == "av1") return "ivf";
	if (canon == "mjpeg") return "mpjpeg";
	return "h264";
}

AVCodecID CodecIdForWsId(uint8_t ws_codec_id, std::string &out_canon)
{
	switch (ws_codec_id) {
	case 1:
		out_canon = "h264";
		return AV_CODEC_ID_H264;
	case 2:
		out_canon = "h265";
		return AV_CODEC_ID_HEVC;
	case 3:
		out_canon = "av1";
		return AV_CODEC_ID_AV1;
	default:
		out_canon.clear();
		return AV_CODEC_ID_NONE;
	}
}

bool WaitSocketReady(SocketHandle sock, bool for_write, int timeout_ms,
		     const std::atomic<bool> &stop_requested, std::string &out_error)
{
	const int64_t deadline = NowSteadyMs() + std::max(timeout_ms, 200);
	while (!stop_requested.load(std::memory_order_relaxed)) {
		const int64_t now = NowSteadyMs();
		if (now >= deadline) {
			out_error = "Socket timeout";
			return false;
		}
		const int64_t slice_ms = std::min<int64_t>(200, deadline - now);
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(sock, &fds);

		timeval tv{};
		tv.tv_sec = static_cast<long>(slice_ms / 1000);
		tv.tv_usec = static_cast<long>((slice_ms % 1000) * 1000);

		const int nfds =
#ifdef _WIN32
			0;
#else
			sock + 1;
#endif
		const int rc = for_write ? select(nfds, nullptr, &fds, nullptr, &tv)
					 : select(nfds, &fds, nullptr, nullptr, &tv);
		if (rc > 0) {
			return true;
		}
		if (rc < 0) {
#ifndef _WIN32
			if (errno == EINTR) {
				continue;
			}
#endif
			out_error = "Socket select() failed";
			return false;
		}
	}
	out_error = "Stopped";
	return false;
}

SocketHandle ConnectTcpWithTimeout(const std::string &host, int port, int timeout_ms,
				   const std::atomic<bool> &stop_requested, std::string &out_error)
{
	EnsureSocketsInitialized();

	addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	addrinfo *res = nullptr;
	const std::string port_str = std::to_string(port);
	if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res) {
		out_error = "Failed to resolve host " + host;
		return kInvalidSocket;
	}

	SocketHandle sock = kInvalidSocket;
	for (addrinfo *ai = res; ai != nullptr; ai = ai->ai_next) {
		if (stop_requested.load(std::memory_order_relaxed)) {
			break;
		}
		SocketHandle cand = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (cand == kInvalidSocket) {
			continue;
		}
		SetNonBlocking(cand, true);

		const int rc = connect(cand, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
		bool connected = (rc == 0);
		if (!connected && WouldBlockError()) {
			if (WaitSocketReady(cand, true, timeout_ms, stop_requested, out_error)) {
				int so_err = 0;
#ifdef _WIN32
				int opt_len = sizeof(so_err);
				getsockopt(cand, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&so_err), &opt_len);
#else
				socklen_t opt_len = sizeof(so_err);
				getsockopt(cand, SOL_SOCKET, SO_ERROR, &so_err, &opt_len);
#endif
				connected = (so_err == 0);
			}
		}

		if (connected) {
			SetNonBlocking(cand, false);
			int one = 1;
#ifdef _WIN32
			setsockopt(cand, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&one), sizeof(one));
#else
			setsockopt(cand, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#endif
			sock = cand;
			break;
		}
		CloseSocketHandle(cand);
	}

	freeaddrinfo(res);
	if (sock == kInvalidSocket && out_error.empty()) {
		out_error = "Failed to connect to " + host + ":" + port_str;
	}
	return sock;
}

class BufferedSocket {
public:
	BufferedSocket(SocketHandle sock, const std::atomic<bool> &stop_requested)
		: sock_(sock), stop_requested_(stop_requested)
	{
		rx_buf_.reserve(65536);
	}

	~BufferedSocket() { CloseSocketHandle(sock_); }

	bool SendAll(const uint8_t *data, size_t len, int timeout_ms, std::string &out_error)
	{
		size_t sent_total = 0;
		while (sent_total < len) {
			if (!WaitSocketReady(sock_, true, timeout_ms, stop_requested_, out_error)) {
				return false;
			}
			const int chunk = static_cast<int>(std::min<size_t>(len - sent_total, 65536));
			const int n = send(sock_, reinterpret_cast<const char *>(data + sent_total), chunk, 0);
			if (n <= 0) {
				out_error = "Socket send failed";
				return false;
			}
			sent_total += static_cast<size_t>(n);
		}
		return true;
	}

	bool ReadHttpUpgradeResponse(int timeout_ms, std::string &out_headers, std::string &out_error)
	{
		while (!stop_requested_.load(std::memory_order_relaxed)) {
			if (rx_buf_.size() > 16384) {
				out_error = "Oversized HTTP upgrade response";
				return false;
			}
			for (size_t i = rx_pos_; i + 3 < rx_buf_.size(); ++i) {
				if (rx_buf_[i] == '\r' && rx_buf_[i + 1] == '\n' &&
				    rx_buf_[i + 2] == '\r' && rx_buf_[i + 3] == '\n') {
					out_headers.assign(reinterpret_cast<const char *>(rx_buf_.data() + rx_pos_),
							   (i + 4) - rx_pos_);
					rx_pos_ = i + 4;
					CompactIfNeeded();
					return true;
				}
			}
			if (!FillOnce(timeout_ms, out_error)) {
				return false;
			}
		}
		out_error = "Stopped";
		return false;
	}

	bool ReadExact(uint8_t *dst, size_t len, int timeout_ms, std::string &out_error)
	{
		size_t copied = 0;
		while (copied < len) {
			const size_t avail = rx_buf_.size() - rx_pos_;
			if (avail > 0) {
				const size_t take = std::min(avail, len - copied);
				std::memcpy(dst + copied, rx_buf_.data() + rx_pos_, take);
				rx_pos_ += take;
				copied += take;
				CompactIfNeeded();
				continue;
			}
			if (!FillOnce(timeout_ms, out_error)) {
				return false;
			}
		}
		return true;
	}

	bool SendMaskedWsControlFrame(uint8_t opcode, const uint8_t *payload, size_t len, int timeout_ms)
	{
		if (len > 125) {
			len = 125;
		}
		const uint8_t mask[4] = {0x37, 0xFA, 0x21, 0x3D};
		uint8_t frame[2 + 4 + 125]{};
		frame[0] = static_cast<uint8_t>(0x80u | (opcode & 0x0Fu));
		frame[1] = static_cast<uint8_t>(0x80u | static_cast<uint8_t>(len));
		std::memcpy(frame + 2, mask, 4);
		for (size_t i = 0; i < len; ++i) {
			frame[6 + i] = static_cast<uint8_t>(payload[i] ^ mask[i & 3u]);
		}
		std::string dummy_err;
		return SendAll(frame, 6 + len, timeout_ms, dummy_err);
	}

private:
	bool FillOnce(int timeout_ms, std::string &out_error)
	{
		if (!WaitSocketReady(sock_, false, timeout_ms, stop_requested_, out_error)) {
			return false;
		}
		uint8_t tmp[32768];
		const int n = recv(sock_, reinterpret_cast<char *>(tmp), sizeof(tmp), 0);
		if (n == 0) {
			out_error = "EOF";
			return false;
		}
		if (n < 0) {
			out_error = "Socket recv failed";
			return false;
		}
		rx_buf_.insert(rx_buf_.end(), tmp, tmp + n);
		return true;
	}

	void CompactIfNeeded()
	{
		if (rx_pos_ == rx_buf_.size()) {
			rx_buf_.clear();
			rx_pos_ = 0;
		} else if (rx_pos_ >= 65536) {
			rx_buf_.erase(rx_buf_.begin(), rx_buf_.begin() + static_cast<ptrdiff_t>(rx_pos_));
			rx_pos_ = 0;
		}
	}

	SocketHandle sock_ = kInvalidSocket;
	const std::atomic<bool> &stop_requested_;
	std::vector<uint8_t> rx_buf_;
	size_t rx_pos_ = 0;
};

bool ReadNextWsBinaryMessage(BufferedSocket &ws, int timeout_ms, std::vector<uint8_t> &out_msg,
			     bool &out_clean_close, std::string &out_error)
{
	out_msg.clear();
	out_clean_close = false;
	bool collecting = false;

	for (;;) {
		uint8_t hdr[2]{};
		if (!ws.ReadExact(hdr, 2, timeout_ms, out_error)) {
			if (out_error == "EOF") {
				out_clean_close = true;
			}
			return false;
		}

		const bool fin = (hdr[0] & 0x80u) != 0;
		const uint8_t opcode = static_cast<uint8_t>(hdr[0] & 0x0Fu);
		const bool masked = (hdr[1] & 0x80u) != 0;
		uint64_t payload_len = static_cast<uint64_t>(hdr[1] & 0x7Fu);

		if (payload_len == 126u) {
			uint8_t ext[2]{};
			if (!ws.ReadExact(ext, 2, timeout_ms, out_error)) {
				return false;
			}
			payload_len = (static_cast<uint64_t>(ext[0]) << 8) | static_cast<uint64_t>(ext[1]);
		} else if (payload_len == 127u) {
			uint8_t ext[8]{};
			if (!ws.ReadExact(ext, 8, timeout_ms, out_error)) {
				return false;
			}
			payload_len = 0;
			for (int i = 0; i < 8; ++i) {
				payload_len = (payload_len << 8) | static_cast<uint64_t>(ext[i]);
			}
		}

		if (payload_len > kMaxWsPayloadBytes ||
		    out_msg.size() + static_cast<size_t>(payload_len) > kMaxWsPayloadBytes) {
			out_error = "WebSocket frame exceeds 32 MB limit";
			return false;
		}

		uint8_t mask_key[4]{};
		if (masked && !ws.ReadExact(mask_key, 4, timeout_ms, out_error)) {
			return false;
		}

		if (opcode >= 0x8u) {
			// Control frame (CLOSE=0x8, PING=0x9, PONG=0xA)
			std::vector<uint8_t> ctrl(static_cast<size_t>(payload_len));
			if (payload_len > 0 && !ws.ReadExact(ctrl.data(), ctrl.size(), timeout_ms, out_error)) {
				return false;
			}
			if (masked) {
				for (size_t i = 0; i < ctrl.size(); ++i) {
					ctrl[i] ^= mask_key[i & 3u];
				}
			}
			if (opcode == 0x8u) {
				ws.SendMaskedWsControlFrame(0x8u, nullptr, 0, 500);
				out_clean_close = true;
				return false;
			}
			if (opcode == 0x9u) {
				ws.SendMaskedWsControlFrame(0xAu, ctrl.data(), ctrl.size(), 1000);
			}
			continue;
		}

		if (opcode == 0x2u || opcode == 0x1u) {
			out_msg.clear();
			collecting = true;
		} else if (opcode == 0x0u && !collecting) {
			out_error = "Unexpected WebSocket continuation frame";
			return false;
		}

		const size_t offset = out_msg.size();
		out_msg.resize(offset + static_cast<size_t>(payload_len));
		if (payload_len > 0 &&
		    !ws.ReadExact(out_msg.data() + offset, static_cast<size_t>(payload_len), timeout_ms, out_error)) {
			return false;
		}
		if (masked) {
			for (size_t i = 0; i < static_cast<size_t>(payload_len); ++i) {
				out_msg[offset + i] ^= mask_key[i & 3u];
			}
		}

		if (fin) {
			if (opcode == 0x2u || (opcode == 0x0u && collecting)) {
				return true;
			}
			collecting = false;
			out_msg.clear();
		}
	}
}

struct HwFormatNegotiator {
	AVPixelFormat hw_pix_fmt = AV_PIX_FMT_NONE;
};

AVPixelFormat NegotiateHwPixelFormat(AVCodecContext *ctx, const AVPixelFormat *pix_fmts)
{
	const auto *neg = static_cast<const HwFormatNegotiator *>(ctx->opaque);
	if (neg && neg->hw_pix_fmt != AV_PIX_FMT_NONE) {
		for (const AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
			if (*p == neg->hw_pix_fmt) {
				return *p;
			}
		}
	}
	// Fall back to the first software format offered by FFmpeg so decoding still succeeds.
	for (const AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
		const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(*p);
		if (desc && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) == 0) {
			return *p;
		}
	}
	return pix_fmts[0];
}

std::vector<AVHWDeviceType> CandidateHwDeviceTypes(const std::string &hwaccel_pref)
{
	const std::string pref = ToLowerCopy(hwaccel_pref);
	if (pref.empty() || pref == "off" || pref == "none" || pref == "false" || pref == "cpu") {
		return {};
	}
	if (pref != "auto") {
		const AVHWDeviceType explicit_type = av_hwdevice_find_type_by_name(pref.c_str());
		if (explicit_type != AV_HWDEVICE_TYPE_NONE) {
			return {explicit_type};
		}
		return {};
	}

	std::vector<AVHWDeviceType> types;
	auto push_if_known = [&](const char *name) {
		const AVHWDeviceType t = av_hwdevice_find_type_by_name(name);
		if (t != AV_HWDEVICE_TYPE_NONE) {
			types.push_back(t);
		}
	};
#ifdef _WIN32
	push_if_known("d3d11va");
	push_if_known("dxva2");
	push_if_known("cuda");
	push_if_known("qsv");
#else
	push_if_known("vaapi");
	push_if_known("cuda");
#endif
	return types;
}

class CodecSession {
public:
	CodecSession() = default;
	~CodecSession() { Close(); }

	CodecSession(const CodecSession &) = delete;
	CodecSession &operator=(const CodecSession &) = delete;

	void Close()
	{
		if (sws_ctx_) {
			sws_freeContext(sws_ctx_);
			sws_ctx_ = nullptr;
		}
		if (i420_frame_) {
			av_frame_free(&i420_frame_);
		}
		if (sw_frame_) {
			av_frame_free(&sw_frame_);
		}
		if (frame_) {
			av_frame_free(&frame_);
		}
		if (codec_ctx_) {
			avcodec_free_context(&codec_ctx_);
		}
		if (hw_device_ctx_) {
			av_buffer_unref(&hw_device_ctx_);
		}
		negotiator_.hw_pix_fmt = AV_PIX_FMT_NONE;
		hw_backend_name_ = "cpu";
		sws_w_ = 0;
		sws_h_ = 0;
		sws_fmt_ = AV_PIX_FMT_NONE;
	}

	bool Open(AVCodecID codec_id, const AVCodecParameters *stream_params,
		  const std::vector<uint8_t> &extradata, const std::string &hwaccel_pref,
		  std::string &out_error)
	{
		Close();

		const AVCodec *decoder = avcodec_find_decoder(codec_id);
		if (!decoder) {
			out_error = "FFmpeg decoder not found";
			return false;
		}

		codec_ctx_ = avcodec_alloc_context3(decoder);
		if (!codec_ctx_) {
			out_error = "Failed to allocate AVCodecContext";
			return false;
		}

		if (stream_params) {
			const int rc = avcodec_parameters_to_context(codec_ctx_, stream_params);
			if (rc < 0) {
				out_error = "avcodec_parameters_to_context failed: " + AvErrorString(rc);
				Close();
				return false;
			}
		} else if (!extradata.empty()) {
			codec_ctx_->extradata = static_cast<uint8_t *>(
				av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
			if (!codec_ctx_->extradata) {
				out_error = "Out of memory allocating extradata";
				Close();
				return false;
			}
			std::memcpy(codec_ctx_->extradata, extradata.data(), extradata.size());
			codec_ctx_->extradata_size = static_cast<int>(extradata.size());
		}

		codec_ctx_->pkt_timebase = AVRational{1, 1000000};
		codec_ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
		codec_ctx_->flags2 |= AV_CODEC_FLAG2_FAST;

		// Try GPU hardware acceleration when requested and supported by this decoder.
		for (AVHWDeviceType cand_type : CandidateHwDeviceTypes(hwaccel_pref)) {
			AVPixelFormat matched_pix = AV_PIX_FMT_NONE;
			for (int i = 0;; ++i) {
				const AVCodecHWConfig *cfg = avcodec_get_hw_config(decoder, i);
				if (!cfg) {
					break;
				}
				if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0 &&
				    cfg->device_type == cand_type) {
					matched_pix = cfg->pix_fmt;
					break;
				}
			}
			if (matched_pix == AV_PIX_FMT_NONE) {
				continue;
			}

			AVBufferRef *dev_ref = nullptr;
			if (av_hwdevice_ctx_create(&dev_ref, cand_type, nullptr, nullptr, 0) == 0 && dev_ref) {
				hw_device_ctx_ = dev_ref;
				negotiator_.hw_pix_fmt = matched_pix;
				codec_ctx_->opaque = &negotiator_;
				codec_ctx_->get_format = &NegotiateHwPixelFormat;
				codec_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
				const char *tname = av_hwdevice_get_type_name(cand_type);
				hw_backend_name_ = tname ? tname : "hw";
				break;
			}
		}

		if (!hw_device_ctx_) {
			codec_ctx_->thread_type = FF_THREAD_SLICE;
			codec_ctx_->thread_count = 0;
			hw_backend_name_ = "cpu";
		}

		const int rc = avcodec_open2(codec_ctx_, decoder, nullptr);
		if (rc < 0) {
			// If opening with hwaccel failed, retry cleanly on CPU before failing.
			if (hw_device_ctx_) {
				return Open(codec_id, stream_params, extradata, "off", out_error);
			}
			out_error = "avcodec_open2 failed: " + AvErrorString(rc);
			Close();
			return false;
		}

		frame_ = av_frame_alloc();
		sw_frame_ = av_frame_alloc();
		i420_frame_ = av_frame_alloc();
		if (!frame_ || !sw_frame_ || !i420_frame_) {
			out_error = "Failed to allocate AVFrames";
			Close();
			return false;
		}
		return true;
	}

	bool IsHardwareActive() const { return hw_device_ctx_ != nullptr; }
	const std::string &BackendName() const { return hw_backend_name_; }

	/**
	 * Sends an AVPacket (or nullptr to flush) and emits all decoded frames.
	 * Sets `out_hw_failed = true` if hardware decoding/transfer failed so caller can fall back to CPU.
	 */
	bool DecodePacket(AVPacket *pkt, AVRational stream_tb, int rotation_deg, bool mirror_horiz,
			  const VcamDecoder::FrameCallback &on_frame, int64_t &io_decoded_count,
			  bool &out_hw_failed)
	{
		out_hw_failed = false;
		const int rc = avcodec_send_packet(codec_ctx_, pkt);
		if (rc < 0 && rc != AVERROR(EAGAIN) && rc != AVERROR_EOF) {
			if (hw_device_ctx_ && io_decoded_count == 0) {
				out_hw_failed = true;
			}
			return false;
		}

		for (;;) {
			const int frc = avcodec_receive_frame(codec_ctx_, frame_);
			if (frc == AVERROR(EAGAIN) || frc == AVERROR_EOF) {
				break;
			}
			if (frc < 0) {
				if (hw_device_ctx_ && io_decoded_count == 0) {
					out_hw_failed = true;
				}
				return false;
			}

			AVFrame *cpu_src = frame_;
			if (negotiator_.hw_pix_fmt != AV_PIX_FMT_NONE &&
			    static_cast<AVPixelFormat>(frame_->format) == negotiator_.hw_pix_fmt) {
				av_frame_unref(sw_frame_);
				if (av_hwframe_transfer_data(sw_frame_, frame_, 0) < 0) {
					av_frame_unref(frame_);
					out_hw_failed = true;
					return false;
				}
				sw_frame_->best_effort_timestamp = frame_->best_effort_timestamp;
				sw_frame_->pts = frame_->pts;
				sw_frame_->color_range = frame_->color_range;
				cpu_src = sw_frame_;
			}

			EmitCpuFrame(cpu_src, stream_tb, rotation_deg, mirror_horiz, on_frame, io_decoded_count);
			av_frame_unref(frame_);
		}
		return true;
	}

private:
	void ConvertNv12ToPackedI420(const AVFrame *nv12, uint32_t w, uint32_t h,
				     awc::DecodedFrameView &out_view)
	{
		const uint32_t cw = (w + 1u) / 2u;
		const uint32_t ch = (h + 1u) / 2u;
		const size_t y_bytes = static_cast<size_t>(w) * h;
		const size_t c_bytes = static_cast<size_t>(cw) * ch;
		nv12_scratch_.resize(y_bytes + 2u * c_bytes);

		uint8_t *dst_y = nv12_scratch_.data();
		uint8_t *dst_u = dst_y + y_bytes;
		uint8_t *dst_v = dst_u + c_bytes;

		const uint8_t *src_y = nv12->data[0];
		const int src_stride_y = nv12->linesize[0];
		for (uint32_t r = 0; r < h; ++r) {
			std::memcpy(dst_y + static_cast<size_t>(r) * w,
				    src_y + static_cast<ptrdiff_t>(r) * src_stride_y, w);
		}

		const uint8_t *src_uv = nv12->data[1];
		const int src_stride_uv = nv12->linesize[1];
		for (uint32_t r = 0; r < ch; ++r) {
			const uint8_t *uv_row = src_uv + static_cast<ptrdiff_t>(r) * src_stride_uv;
			uint8_t *u_row = dst_u + static_cast<size_t>(r) * cw;
			uint8_t *v_row = dst_v + static_cast<size_t>(r) * cw;
			for (uint32_t c = 0; c < cw; ++c) {
				u_row[c] = uv_row[2u * c];
				v_row[c] = uv_row[2u * c + 1u];
			}
		}

		out_view.y = dst_y;
		out_view.u = dst_u;
		out_view.v = dst_v;
		out_view.stride_y = w;
		out_view.stride_u = cw;
		out_view.stride_v = cw;
		out_view.width = w;
		out_view.height = h;
	}

	void EmitCpuFrame(AVFrame *f, AVRational stream_tb, int rotation_deg, bool mirror_horiz,
			  const VcamDecoder::FrameCallback &on_frame, int64_t &io_decoded_count)
	{
		if (!f || f->width <= 0 || f->height <= 0) {
			return;
		}
		const AVPixelFormat pix = static_cast<AVPixelFormat>(f->format);
		const bool full_range = (f->color_range == AVCOL_RANGE_JPEG ||
					 pix == AV_PIX_FMT_YUVJ420P ||
					 pix == AV_PIX_FMT_YUVJ422P ||
					 pix == AV_PIX_FMT_YUVJ444P);

		awc::DecodedFrameView view{};
		view.full_range = full_range;
		const int64_t raw_ts =
			(f->best_effort_timestamp != AV_NOPTS_VALUE) ? f->best_effort_timestamp : f->pts;
		view.pts_us = (raw_ts != AV_NOPTS_VALUE)
				      ? av_rescale_q(raw_ts, stream_tb, AVRational{1, 1000000})
				      : 0;

		if (pix == AV_PIX_FMT_YUV420P || pix == AV_PIX_FMT_YUVJ420P) {
			view.y = f->data[0];
			view.u = f->data[1];
			view.v = f->data[2];
			view.stride_y = static_cast<uint32_t>(f->linesize[0]);
			view.stride_u = static_cast<uint32_t>(f->linesize[1]);
			view.stride_v = static_cast<uint32_t>(f->linesize[2]);
			view.width = static_cast<uint32_t>(f->width);
			view.height = static_cast<uint32_t>(f->height);
		} else if (pix == AV_PIX_FMT_NV12) {
			ConvertNv12ToPackedI420(f, static_cast<uint32_t>(f->width),
						static_cast<uint32_t>(f->height), view);
		} else {
			AVPixelFormat src_fmt = pix;
			if (pix == AV_PIX_FMT_YUVJ422P) src_fmt = AV_PIX_FMT_YUV422P;
			else if (pix == AV_PIX_FMT_YUVJ444P) src_fmt = AV_PIX_FMT_YUV444P;

			if (sws_w_ != f->width || sws_h_ != f->height || sws_fmt_ != src_fmt) {
				if (i420_frame_->data[0]) {
					av_frame_unref(i420_frame_);
				}
				i420_frame_->format = AV_PIX_FMT_YUV420P;
				i420_frame_->width = f->width;
				i420_frame_->height = f->height;
				if (av_frame_get_buffer(i420_frame_, 32) < 0) {
					return;
				}
				sws_ctx_ = sws_getCachedContext(sws_ctx_, f->width, f->height, src_fmt,
								f->width, f->height, AV_PIX_FMT_YUV420P,
								SWS_BILINEAR, nullptr, nullptr, nullptr);
				if (!sws_ctx_) {
					return;
				}
				const int range_flag = full_range ? 1 : 0;
				const int *coefs = sws_getCoefficients(SWS_CS_ITU601);
				sws_setColorspaceDetails(sws_ctx_, coefs, range_flag, coefs, range_flag,
							 0, 1 << 16, 1 << 16);
				sws_w_ = f->width;
				sws_h_ = f->height;
				sws_fmt_ = src_fmt;
			}
			sws_scale(sws_ctx_, f->data, f->linesize, 0, f->height, i420_frame_->data,
				  i420_frame_->linesize);

			view.y = i420_frame_->data[0];
			view.u = i420_frame_->data[1];
			view.v = i420_frame_->data[2];
			view.stride_y = static_cast<uint32_t>(i420_frame_->linesize[0]);
			view.stride_u = static_cast<uint32_t>(i420_frame_->linesize[1]);
			view.stride_v = static_cast<uint32_t>(i420_frame_->linesize[2]);
			view.width = static_cast<uint32_t>(f->width);
			view.height = static_cast<uint32_t>(f->height);
		}

		awc::DecodedFrameView final_view =
			awc::TransformI420(view, rotation_deg, mirror_horiz, transform_scratch_);
		++io_decoded_count;
		if (on_frame) {
			on_frame(final_view);
		}
	}

	AVCodecContext *codec_ctx_ = nullptr;
	AVBufferRef *hw_device_ctx_ = nullptr;
	HwFormatNegotiator negotiator_{};
	std::string hw_backend_name_ = "cpu";
	AVFrame *frame_ = nullptr;
	AVFrame *sw_frame_ = nullptr;
	AVFrame *i420_frame_ = nullptr;
	SwsContext *sws_ctx_ = nullptr;
	int sws_w_ = 0;
	int sws_h_ = 0;
	AVPixelFormat sws_fmt_ = AV_PIX_FMT_NONE;
	std::vector<uint8_t> nv12_scratch_;
	std::vector<uint8_t> transform_scratch_;
};

} // namespace

void VcamDecoder::SetTransform(int rotation_degrees, bool mirror_horizontal)
{
	rotation_degrees_.store(awc::NormalizeRotationDegrees(rotation_degrees), std::memory_order_relaxed);
	mirror_horizontal_.store(mirror_horizontal, std::memory_order_relaxed);
}

void VcamDecoder::RequestStop()
{
	stop_requested_.store(true, std::memory_order_release);
}

bool VcamDecoder::StopRequested() const
{
	return stop_requested_.load(std::memory_order_acquire);
}

std::string VcamDecoder::ActiveBackendLabel() const
{
	return active_backend_;
}

bool VcamDecoder::Run(const Options &options, const FrameCallback &on_frame, std::string &out_error,
		      int64_t *out_frames_decoded)
{
	stop_requested_.store(false, std::memory_order_release);
	SetTransform(options.initial_rotation, options.initial_mirror);
	if (out_frames_decoded) {
		*out_frames_decoded = 0;
	}

	const std::string canon_codec = awc::NormalizeCodec(options.codec);
	const bool can_use_ws = options.prefer_ws_video && !options.host.empty() && options.port > 0 &&
				(canon_codec == "h264" || canon_codec == "h265" || canon_codec == "av1");

	if (can_use_ws) {
		int64_t ws_frames = 0;
		std::string ws_err;
		const bool ok = RunWebSocketVideo(options, on_frame, ws_err, &ws_frames);
		if (out_frames_decoded) {
			*out_frames_decoded = ws_frames;
		}
		if (ok || StopRequested() || ws_frames > 0) {
			out_error = ws_err;
			return ok;
		}
		// If /ws/video handshake failed before decoding any frames and a fallback HTTP URL exists,
		// fall through to the 4 MB-probed HTTP stream seamlessly.
		if (options.url.empty()) {
			out_error = ws_err;
			return false;
		}
	}

	return RunAvFormatStream(options, on_frame, out_error, out_frames_decoded);
}

bool VcamDecoder::RunWebSocketVideo(const Options &options, const FrameCallback &on_frame,
				    std::string &out_error, int64_t *out_frames_decoded)
{
	SocketHandle sock = ConnectTcpWithTimeout(options.host, options.port, options.open_timeout_ms,
						  stop_requested_, out_error);
	if (sock == kInvalidSocket) {
		return false;
	}

	BufferedSocket ws(sock, stop_requested_);
	const std::string req =
		"GET /ws/video HTTP/1.1\r\n"
		"Host: " + options.host + ":" + std::to_string(options.port) + "\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
		"Sec-WebSocket-Version: 13\r\n\r\n";

	if (!ws.SendAll(reinterpret_cast<const uint8_t *>(req.data()), req.size(),
			options.open_timeout_ms, out_error)) {
		return false;
	}

	std::string resp_headers;
	if (!ws.ReadHttpUpgradeResponse(options.open_timeout_ms, resp_headers, out_error)) {
		return false;
	}
	const size_t eol = resp_headers.find("\r\n");
	const std::string status_line = (eol != std::string::npos) ? resp_headers.substr(0, eol) : resp_headers;
	if (status_line.find(" 101 ") == std::string::npos) {
		out_error = "WebSocket /ws/video upgrade rejected: " + status_line;
		return false;
	}

	CodecSession session;
	bool session_open = false;
	uint8_t active_ws_codec = 0;
	AVCodecID active_codec_id = AV_CODEC_ID_NONE;
	std::string active_canon_codec;
	std::vector<uint8_t> config_bytes;
	bool seen_keyframe = false;
	std::string hw_pref = options.hwaccel;

	AVPacket *pkt = av_packet_alloc();
	if (!pkt) {
		out_error = "Failed to allocate AVPacket";
		return false;
	}

	std::vector<uint8_t> msg;
	int64_t decoded_count = 0;
	bool clean_close = false;

	auto fill_and_send = [&](const uint8_t *payload, size_t payload_len, bool prepend_config,
				 int64_t pts_us, bool &out_hw_failed) -> bool {
		const size_t prefix_len =
			(prepend_config && !config_bytes.empty()) ? config_bytes.size() : 0u;
		const size_t total_len = prefix_len + payload_len;
		if (total_len == 0) {
			out_hw_failed = false;
			return true;
		}
		av_packet_unref(pkt);
		if (av_new_packet(pkt, static_cast<int>(total_len)) < 0) {
			out_hw_failed = false;
			return false;
		}
		if (prefix_len > 0) {
			std::memcpy(pkt->data, config_bytes.data(), prefix_len);
		}
		std::memcpy(pkt->data + prefix_len, payload, payload_len);
		pkt->pts = pts_us;
		pkt->dts = pts_us;

		const int rot = rotation_degrees_.load(std::memory_order_relaxed);
		const bool mir = mirror_horizontal_.load(std::memory_order_relaxed);
		const bool ok = session.DecodePacket(pkt, AVRational{1, 1000000}, rot, mir, on_frame,
						     decoded_count, out_hw_failed);
		av_packet_unref(pkt);
		return ok;
	};

	while (!StopRequested()) {
		if (!ReadNextWsBinaryMessage(ws, options.read_timeout_ms, msg, clean_close, out_error)) {
			break;
		}
		if (msg.size() <= 10) {
			continue;
		}

		const uint8_t ws_codec_id = msg[0];
		const uint8_t pkt_type = msg[1];
		uint64_t pts_u64 = 0;
		for (int i = 0; i < 8; ++i) {
			pts_u64 = (pts_u64 << 8) | static_cast<uint64_t>(msg[2 + i]);
		}
		const int64_t pts_us = static_cast<int64_t>(pts_u64);
		const uint8_t *payload = msg.data() + 10;
		const size_t payload_len = msg.size() - 10;

		if (active_ws_codec != 0 && ws_codec_id != active_ws_codec) {
			// Codec switched dynamically on Android; exit cleanly to refresh status & stream.
			clean_close = true;
			break;
		}

		if (active_ws_codec == 0) {
			active_codec_id = CodecIdForWsId(ws_codec_id, active_canon_codec);
			if (active_codec_id == AV_CODEC_ID_NONE) {
				out_error = "Unknown /ws/video codec ID " + std::to_string(ws_codec_id);
				break;
			}
			active_ws_codec = ws_codec_id;
		}

		if (pkt_type == 1) {
			// Codec configuration packet (SPS/PPS, VPS/SPS/PPS, or AV1 Sequence Header OBU).
			config_bytes.assign(payload, payload + payload_len);
			if (session_open && (active_canon_codec == "h264" || active_canon_codec == "h265")) {
				bool hw_failed = false;
				fill_and_send(payload, payload_len, false, pts_us, hw_failed);
			}
			continue;
		}

		if (!seen_keyframe) {
			if (pkt_type != 2) {
				continue;
			}
			seen_keyframe = true;
		}

		if (!session_open) {
			if (!session.Open(active_codec_id, nullptr, config_bytes, hw_pref, out_error)) {
				break;
			}
			session_open = true;
			active_backend_ = "ws/video + " + session.BackendName();
		}

		const bool prepend_cfg =
			(pkt_type == 2) && (active_canon_codec == "h264" || active_canon_codec == "h265");
		bool hw_failed = false;
		if (!fill_and_send(payload, payload_len, prepend_cfg, pts_us, hw_failed) && hw_failed) {
			// Hardware decoder rejected stream profile/size (e.g. 4K60 HEVC on older GPU);
			// immediately reopen on CPU and decode the same keyframe without dropping connection.
			hw_pref = "off";
			std::string cpu_err;
			if (!session.Open(active_codec_id, nullptr, config_bytes, hw_pref, cpu_err)) {
				out_error = cpu_err;
				break;
			}
			active_backend_ = "ws/video + " + session.BackendName();
			if (pkt_type == 2) {
				bool dummy_hw = false;
				fill_and_send(payload, payload_len, prepend_cfg, pts_us, dummy_hw);
			} else {
				seen_keyframe = false;
				session_open = false;
			}
		}
	}

	if (clean_close && session_open && !StopRequested()) {
		bool dummy_hw = false;
		const int rot = rotation_degrees_.load(std::memory_order_relaxed);
		const bool mir = mirror_horizontal_.load(std::memory_order_relaxed);
		session.DecodePacket(nullptr, AVRational{1, 1000000}, rot, mir, on_frame,
				     decoded_count, dummy_hw);
	}

	av_packet_free(&pkt);
	if (out_frames_decoded) {
		*out_frames_decoded = decoded_count;
	}
	if (StopRequested()) {
		return true;
	}
	return clean_close && decoded_count > 0;
}

bool VcamDecoder::RunAvFormatStream(const Options &options, const FrameCallback &on_frame,
				    std::string &out_error, int64_t *out_frames_decoded)
{
	EnsureFfmpegNetworkInit();
	if (options.url.empty()) {
		out_error = "Empty stream URL";
		return false;
	}

	InterruptState intr{};
	intr.stop_requested = &stop_requested_;
	intr.deadline_ms.store(NowSteadyMs() + std::max(options.open_timeout_ms, 500),
			       std::memory_order_relaxed);

	AVFormatContext *fmt_ctx = avformat_alloc_context();
	if (!fmt_ctx) {
		out_error = "Failed to allocate AVFormatContext";
		return false;
	}
	fmt_ctx->interrupt_callback.callback = &FfmpegInterruptCb;
	fmt_ctx->interrupt_callback.opaque = &intr;

	const std::string canon_codec = awc::NormalizeCodec(options.codec);
	const char *demux_name = DemuxerNameForCodec(canon_codec);
	const AVInputFormat *input_fmt = av_find_input_format(demux_name);
	if (!input_fmt) {
		avformat_free_context(fmt_ctx);
		out_error = std::string("FFmpeg demuxer not found: ") + demux_name;
		return false;
	}

	AVDictionary *opts = nullptr;
	const std::string rw_timeout_us =
		std::to_string(static_cast<int64_t>(std::max(options.read_timeout_ms, 500)) * 1000);
	if (options.url.rfind("http://", 0) == 0 || options.url.rfind("https://", 0) == 0) {
		av_dict_set(&opts, "rw_timeout", rw_timeout_us.c_str(), 0);
		av_dict_set(&opts, "reconnect", "0", 0);
	}
	// 4 MB probesize handles 4K @ 30-40 Mbps IDR/JPEG frames without truncating sequence headers,
	// while fps_probe_size=0 below ensures zero multi-frame probe delay.
	av_dict_set(&opts, "probesize", "4194304", 0);
	av_dict_set(&opts, "analyzeduration", "1000000", 0);

	int rc = avformat_open_input(&fmt_ctx, options.url.c_str(), input_fmt, &opts);
	av_dict_free(&opts);
	if (rc < 0) {
		if (fmt_ctx) {
			avformat_close_input(&fmt_ctx);
		}
		if (StopRequested()) {
			return true;
		}
		out_error = "Failed to open stream (" + options.url + "): " + AvErrorString(rc);
		return false;
	}

	intr.deadline_ms.store(NowSteadyMs() + std::max(options.open_timeout_ms, 500),
			       std::memory_order_relaxed);
	fmt_ctx->fps_probe_size = 0;
	rc = avformat_find_stream_info(fmt_ctx, nullptr);
	if (rc < 0) {
		avformat_close_input(&fmt_ctx);
		if (StopRequested()) {
			return true;
		}
		out_error = "avformat_find_stream_info failed: " + AvErrorString(rc);
		return false;
	}

	fmt_ctx->flags |= AVFMT_FLAG_NOBUFFER;

	const int stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (stream_idx < 0) {
		avformat_close_input(&fmt_ctx);
		out_error = "No video stream found";
		return false;
	}
	AVStream *video_stream = fmt_ctx->streams[stream_idx];

	CodecSession session;
	std::vector<uint8_t> empty_extra;
	const std::string hw_pref = (canon_codec == "mjpeg") ? "off" : options.hwaccel;
	if (!session.Open(video_stream->codecpar->codec_id, video_stream->codecpar, empty_extra,
			  hw_pref, out_error)) {
		avformat_close_input(&fmt_ctx);
		return false;
	}
	active_backend_ = "http + " + session.BackendName();

	AVPacket *pkt = av_packet_alloc();
	int64_t decoded_count = 0;
	bool clean_eof = false;

	while (!StopRequested()) {
		intr.deadline_ms.store(NowSteadyMs() + std::max(options.read_timeout_ms, 500),
				       std::memory_order_relaxed);
		rc = av_read_frame(fmt_ctx, pkt);
		if (rc == AVERROR_EOF) {
			clean_eof = true;
			break;
		}
		if (rc < 0) {
			if (!StopRequested()) {
				out_error = "Stream interrupted: " + AvErrorString(rc);
			}
			break;
		}

		if (pkt->stream_index == stream_idx) {
			const int rot = rotation_degrees_.load(std::memory_order_relaxed);
			const bool mir = mirror_horizontal_.load(std::memory_order_relaxed);
			bool hw_failed = false;
			if (!session.DecodePacket(pkt, video_stream->time_base, rot, mir, on_frame,
						  decoded_count, hw_failed) &&
			    hw_failed) {
				std::string cpu_err;
				if (session.Open(video_stream->codecpar->codec_id, video_stream->codecpar,
						 empty_extra, "off", cpu_err)) {
					active_backend_ = "http + " + session.BackendName();
					bool dummy_hw = false;
					session.DecodePacket(pkt, video_stream->time_base, rot, mir,
							     on_frame, decoded_count, dummy_hw);
				}
			}
		}
		av_packet_unref(pkt);
	}

	if (clean_eof && !StopRequested()) {
		const int rot = rotation_degrees_.load(std::memory_order_relaxed);
		const bool mir = mirror_horizontal_.load(std::memory_order_relaxed);
		bool dummy_hw = false;
		session.DecodePacket(nullptr, video_stream->time_base, rot, mir, on_frame,
				     decoded_count, dummy_hw);
	}

	av_packet_free(&pkt);
	avformat_close_input(&fmt_ctx);
	if (out_frames_decoded) {
		*out_frames_decoded = decoded_count;
	}
	if (StopRequested()) {
		return true;
	}
	return clean_eof && decoded_count > 0;
}

} // namespace awc::vcam
