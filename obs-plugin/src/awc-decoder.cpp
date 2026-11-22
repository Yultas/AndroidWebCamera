#include "awc-decoder.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>

#include "awc-client.hpp"

namespace awc {
namespace {

void EnsureFfmpegNetworkInit()
{
	static std::once_flag once;
	std::call_once(once, []() { avformat_network_init(); });
}

std::string AvErrorString(int errnum)
{
	char buf[AV_ERROR_MAX_STRING_SIZE]{};
	av_strerror(errnum, buf, sizeof(buf));
	return std::string(buf);
}

struct InterruptState {
	std::atomic<bool> *stop_requested = nullptr;
	std::atomic<int64_t> deadline_ms{0};
};

int64_t NowSteadyMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		       std::chrono::steady_clock::now().time_since_epoch())
		.count();
}

int FfmpegInterruptCb(void *opaque)
{
	auto *state = static_cast<InterruptState *>(opaque);
	if (!state)
		return 0;
	if (state->stop_requested && state->stop_requested->load(std::memory_order_relaxed))
		return 1;
	int64_t deadline = state->deadline_ms.load(std::memory_order_relaxed);
	if (deadline > 0 && NowSteadyMs() >= deadline)
		return 1;
	return 0;
}

void TransformPlane(const uint8_t *src, uint32_t src_stride, uint32_t src_w, uint32_t src_h,
		    uint8_t *dst, uint32_t dst_stride, int rot, bool mirror)
{
	if (rot == 0 && !mirror) {
		for (uint32_t y = 0; y < src_h; ++y) {
			std::memcpy(dst + y * dst_stride, src + y * src_stride, src_w);
		}
		return;
	}

	if (rot == 0 && mirror) {
		for (uint32_t y = 0; y < src_h; ++y) {
			const uint8_t *srow = src + y * src_stride;
			uint8_t *drow = dst + y * dst_stride;
			for (uint32_t x = 0; x < src_w; ++x) {
				drow[x] = srow[src_w - 1 - x];
			}
		}
		return;
	}

	if (rot == 180 && !mirror) {
		for (uint32_t y = 0; y < src_h; ++y) {
			const uint8_t *srow = src + (src_h - 1 - y) * src_stride;
			uint8_t *drow = dst + y * dst_stride;
			for (uint32_t x = 0; x < src_w; ++x) {
				drow[x] = srow[src_w - 1 - x];
			}
		}
		return;
	}

	if (rot == 180 && mirror) {
		for (uint32_t y = 0; y < src_h; ++y) {
			const uint8_t *srow = src + (src_h - 1 - y) * src_stride;
			std::memcpy(dst + y * dst_stride, srow, src_w);
		}
		return;
	}

	const uint32_t dst_w = src_h;
	const uint32_t dst_h = src_w;

	if (rot == 90 && !mirror) {
		for (uint32_t dy = 0; dy < dst_h; ++dy) {
			uint8_t *drow = dst + dy * dst_stride;
			for (uint32_t dx = 0; dx < dst_w; ++dx) {
				drow[dx] = src[(src_h - 1 - dx) * src_stride + dy];
			}
		}
		return;
	}

	if (rot == 90 && mirror) {
		for (uint32_t dy = 0; dy < dst_h; ++dy) {
			uint8_t *drow = dst + dy * dst_stride;
			for (uint32_t dx = 0; dx < dst_w; ++dx) {
				drow[dx] = src[dx * src_stride + dy];
			}
		}
		return;
	}

	if (rot == 270 && !mirror) {
		for (uint32_t dy = 0; dy < dst_h; ++dy) {
			uint8_t *drow = dst + dy * dst_stride;
			const uint32_t sx = src_w - 1 - dy;
			for (uint32_t dx = 0; dx < dst_w; ++dx) {
				drow[dx] = src[dx * src_stride + sx];
			}
		}
		return;
	}

	// rot == 270 && mirror
	for (uint32_t dy = 0; dy < dst_h; ++dy) {
		uint8_t *drow = dst + dy * dst_stride;
		const uint32_t sx = src_w - 1 - dy;
		for (uint32_t dx = 0; dx < dst_w; ++dx) {
			drow[dx] = src[(src_h - 1 - dx) * src_stride + sx];
		}
	}
}

const char *DemuxerNameForCodec(const std::string &codec)
{
	const std::string canon = NormalizeCodec(codec);
	if (canon == "h265")
		return "hevc";
	if (canon == "av1")
		return "ivf";
	if (canon == "mjpeg")
		return "mpjpeg";
	return "h264";
}

} // namespace

int NormalizeRotationDegrees(int rotation_degrees)
{
	int norm = ((rotation_degrees % 360) + 360) % 360;
	if (norm == 90 || norm == 180 || norm == 270)
		return norm;
	return 0;
}

DecodedFrameView TransformI420(const DecodedFrameView &src, int rotation_degrees,
			       bool mirror_horizontal, std::vector<uint8_t> &scratch)
{
	const int rot = NormalizeRotationDegrees(rotation_degrees);
	if (rot == 0 && !mirror_horizontal)
		return src;

	const uint32_t dst_w = (rot == 90 || rot == 270) ? src.height : src.width;
	const uint32_t dst_h = (rot == 90 || rot == 270) ? src.width : src.height;
	const uint32_t src_cw = (src.width + 1) / 2;
	const uint32_t src_ch = (src.height + 1) / 2;
	const uint32_t dst_cw = (dst_w + 1) / 2;
	const uint32_t dst_ch = (dst_h + 1) / 2;

	const size_t y_bytes = static_cast<size_t>(dst_w) * dst_h;
	const size_t c_bytes = static_cast<size_t>(dst_cw) * dst_ch;
	scratch.resize(y_bytes + 2 * c_bytes);

	uint8_t *dst_y = scratch.data();
	uint8_t *dst_u = dst_y + y_bytes;
	uint8_t *dst_v = dst_u + c_bytes;

	TransformPlane(src.y, src.stride_y, src.width, src.height, dst_y, dst_w, rot,
		       mirror_horizontal);
	TransformPlane(src.u, src.stride_u, src_cw, src_ch, dst_u, dst_cw, rot, mirror_horizontal);
	TransformPlane(src.v, src.stride_v, src_cw, src_ch, dst_v, dst_cw, rot, mirror_horizontal);

	DecodedFrameView out;
	out.y = dst_y;
	out.u = dst_u;
	out.v = dst_v;
	out.stride_y = dst_w;
	out.stride_u = dst_cw;
	out.stride_v = dst_cw;
	out.width = dst_w;
	out.height = dst_h;
	out.full_range = src.full_range;
	out.pts_us = src.pts_us;
	return out;
}

void AwcDecoder::SetTransform(int rotation_degrees, bool mirror_horizontal)
{
	rotation_degrees_.store(NormalizeRotationDegrees(rotation_degrees),
				std::memory_order_relaxed);
	mirror_horizontal_.store(mirror_horizontal, std::memory_order_relaxed);
}

void AwcDecoder::RequestStop()
{
	stop_requested_.store(true, std::memory_order_release);
}

bool AwcDecoder::StopRequested() const
{
	return stop_requested_.load(std::memory_order_acquire);
}

bool AwcDecoder::Run(const Options &options, const FrameCallback &on_frame, std::string &out_error,
		     int64_t *out_frames_decoded)
{
	EnsureFfmpegNetworkInit();
	stop_requested_.store(false, std::memory_order_release);
	SetTransform(options.initial_rotation, options.initial_mirror);

	if (out_frames_decoded)
		*out_frames_decoded = 0;

	if (options.url.empty()) {
		out_error = "Пустой URL видеопотока";
		return false;
	}

	InterruptState intr{};
	intr.stop_requested = &stop_requested_;
	intr.deadline_ms.store(NowSteadyMs() + std::max(options.open_timeout_ms, 500),
			       std::memory_order_relaxed);

	AVFormatContext *fmt_ctx = avformat_alloc_context();
	if (!fmt_ctx) {
		out_error = "Не удалось выделить AVFormatContext";
		return false;
	}
	fmt_ctx->interrupt_callback.callback = &FfmpegInterruptCb;
	fmt_ctx->interrupt_callback.opaque = &intr;

	const std::string canon_codec = NormalizeCodec(options.codec);
	const char *demux_name = DemuxerNameForCodec(canon_codec);
	const AVInputFormat *input_fmt = av_find_input_format(demux_name);
	if (!input_fmt) {
		avformat_free_context(fmt_ctx);
		out_error = std::string("Демуксер FFmpeg не найден: ") + demux_name;
		return false;
	}

	AVDictionary *opts = nullptr;
	const std::string rw_timeout_us =
		std::to_string(static_cast<int64_t>(std::max(options.read_timeout_ms, 500)) * 1000);
	if (options.url.rfind("http://", 0) == 0 || options.url.rfind("https://", 0) == 0) {
		av_dict_set(&opts, "rw_timeout", rw_timeout_us.c_str(), 0);
		av_dict_set(&opts, "reconnect", "0", 0);
	}
	av_dict_set(&opts, "probesize", canon_codec == "av1" ? "262144" : "131072", 0);
	av_dict_set(&opts, "analyzeduration", "500000", 0);

	int rc = avformat_open_input(&fmt_ctx, options.url.c_str(), input_fmt, &opts);
	av_dict_free(&opts);
	if (rc < 0) {
		if (fmt_ctx)
			avformat_close_input(&fmt_ctx);
		if (StopRequested())
			return true;
		out_error = "Ошибка открытия потока (" + options.url + "): " + AvErrorString(rc);
		return false;
	}

	intr.deadline_ms.store(NowSteadyMs() + std::max(options.open_timeout_ms, 500),
			       std::memory_order_relaxed);
	fmt_ctx->fps_probe_size = 0;
	rc = avformat_find_stream_info(fmt_ctx, nullptr);
	if (rc < 0) {
		avformat_close_input(&fmt_ctx);
		if (StopRequested())
			return true;
		out_error = "Не удалось определить параметры потока: " + AvErrorString(rc);
		return false;
	}

	// Enable zero-buffering after probe completes so IVF/Annex-B sequence headers were parsed cleanly.
	fmt_ctx->flags |= AVFMT_FLAG_NOBUFFER;

	const AVCodec *decoder = nullptr;
	int stream_idx =
		av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
	if (stream_idx < 0) {
		avformat_close_input(&fmt_ctx);
		out_error = "Видеодорожка в потоке не найдена";
		return false;
	}

	AVStream *video_stream = fmt_ctx->streams[stream_idx];
	if (!decoder) {
		decoder = avcodec_find_decoder(video_stream->codecpar->codec_id);
	}
	if (!decoder) {
		avformat_close_input(&fmt_ctx);
		out_error = "Декодер FFmpeg не найден для кодека " + canon_codec;
		return false;
	}

	AVCodecContext *codec_ctx = avcodec_alloc_context3(decoder);
	if (!codec_ctx) {
		avformat_close_input(&fmt_ctx);
		out_error = "Не удалось выделить AVCodecContext";
		return false;
	}

	rc = avcodec_parameters_to_context(codec_ctx, video_stream->codecpar);
	if (rc < 0) {
		avcodec_free_context(&codec_ctx);
		avformat_close_input(&fmt_ctx);
		out_error = "Ошибка копирования параметров кодека: " + AvErrorString(rc);
		return false;
	}

	codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
	codec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
	// Avoid multi-frame buffering delay; allow slice threading where supported.
	codec_ctx->thread_type = FF_THREAD_SLICE;
	codec_ctx->thread_count = 0;

	rc = avcodec_open2(codec_ctx, decoder, nullptr);
	if (rc < 0) {
		avcodec_free_context(&codec_ctx);
		avformat_close_input(&fmt_ctx);
		out_error = "Не удалось открыть декодер: " + AvErrorString(rc);
		return false;
	}

	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	AVFrame *i420_frame = av_frame_alloc();
	SwsContext *sws_ctx = nullptr;
	int sws_w = 0, sws_h = 0;
	AVPixelFormat sws_fmt = AV_PIX_FMT_NONE;
	std::vector<uint8_t> transform_scratch;
	int64_t decoded_count = 0;

	auto emit_decoded_frame = [&](AVFrame *f) {
		if (!f || f->width <= 0 || f->height <= 0)
			return;
		AVPixelFormat pix = static_cast<AVPixelFormat>(f->format);
		bool full_range = (f->color_range == AVCOL_RANGE_JPEG ||
				   pix == AV_PIX_FMT_YUVJ420P ||
				   pix == AV_PIX_FMT_YUVJ422P ||
				   pix == AV_PIX_FMT_YUVJ444P);

		AVPixelFormat src_fmt = pix;
		if (pix == AV_PIX_FMT_YUVJ422P)
			src_fmt = AV_PIX_FMT_YUV422P;
		else if (pix == AV_PIX_FMT_YUVJ444P)
			src_fmt = AV_PIX_FMT_YUV444P;
		else if (pix == AV_PIX_FMT_YUVJ420P)
			src_fmt = AV_PIX_FMT_YUV420P;

		AVFrame *src_i420 = f;
		if (src_fmt != AV_PIX_FMT_YUV420P) {
			if (sws_w != f->width || sws_h != f->height || sws_fmt != src_fmt) {
				if (i420_frame->data[0]) {
					av_frame_unref(i420_frame);
				}
				i420_frame->format = AV_PIX_FMT_YUV420P;
				i420_frame->width = f->width;
				i420_frame->height = f->height;
				if (av_frame_get_buffer(i420_frame, 32) < 0)
					return;
				sws_ctx = sws_getCachedContext(sws_ctx, f->width, f->height,
							       src_fmt, f->width, f->height,
							       AV_PIX_FMT_YUV420P, SWS_BILINEAR,
							       nullptr, nullptr, nullptr);
				if (!sws_ctx)
					return;
				const int range_flag = full_range ? 1 : 0;
				const int *coefs = sws_getCoefficients(SWS_CS_ITU601);
				sws_setColorspaceDetails(sws_ctx, coefs, range_flag, coefs,
							 range_flag, 0, 1 << 16, 1 << 16);
				sws_w = f->width;
				sws_h = f->height;
				sws_fmt = src_fmt;
			}
			sws_scale(sws_ctx, f->data, f->linesize, 0, f->height, i420_frame->data,
				  i420_frame->linesize);
			src_i420 = i420_frame;
		}

		DecodedFrameView view;
		view.y = src_i420->data[0];
		view.u = src_i420->data[1];
		view.v = src_i420->data[2];
		view.stride_y = static_cast<uint32_t>(src_i420->linesize[0]);
		view.stride_u = static_cast<uint32_t>(src_i420->linesize[1]);
		view.stride_v = static_cast<uint32_t>(src_i420->linesize[2]);
		view.width = static_cast<uint32_t>(f->width);
		view.height = static_cast<uint32_t>(f->height);
		view.full_range = full_range;
		view.pts_us = (f->best_effort_timestamp != AV_NOPTS_VALUE)
				      ? av_rescale_q(f->best_effort_timestamp,
						     video_stream->time_base,
						     AVRational{1, 1000000})
				      : 0;

		const int rot = rotation_degrees_.load(std::memory_order_relaxed);
		const bool mir = mirror_horizontal_.load(std::memory_order_relaxed);
		DecodedFrameView final_view = TransformI420(view, rot, mir, transform_scratch);
		++decoded_count;
		if (on_frame)
			on_frame(final_view);
	};

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
				out_error = "Поток прервался: " + AvErrorString(rc);
			}
			break;
		}

		if (pkt->stream_index == stream_idx) {
			rc = avcodec_send_packet(codec_ctx, pkt);
			if (rc >= 0 || rc == AVERROR(EAGAIN)) {
				while (!StopRequested()) {
					int frc = avcodec_receive_frame(codec_ctx, frame);
					if (frc == AVERROR(EAGAIN) || frc == AVERROR_EOF)
						break;
					if (frc < 0)
						break;
					emit_decoded_frame(frame);
					av_frame_unref(frame);
				}
			}
		}
		av_packet_unref(pkt);
	}

	if (clean_eof && !StopRequested()) {
		if (avcodec_send_packet(codec_ctx, nullptr) >= 0) {
			while (avcodec_receive_frame(codec_ctx, frame) >= 0) {
				emit_decoded_frame(frame);
				av_frame_unref(frame);
			}
		}
	}

	if (out_frames_decoded)
		*out_frames_decoded = decoded_count;

	if (sws_ctx)
		sws_freeContext(sws_ctx);
	av_frame_free(&i420_frame);
	av_frame_free(&frame);
	av_packet_free(&pkt);
	avcodec_free_context(&codec_ctx);
	avformat_close_input(&fmt_ctx);

	if (StopRequested())
		return true;
	return clean_eof && decoded_count > 0;
}

} // namespace awc
