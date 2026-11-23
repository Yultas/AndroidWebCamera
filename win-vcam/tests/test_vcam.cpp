#include "awc-decoder.hpp"
#include "vcam-convert.hpp"
#include "vcam-shm.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                \
	do {                                                                                       \
		if (!(cond)) {                                                                     \
			std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " (" #cond ")\n"; \
			++g_failures;                                                              \
		}                                                                                  \
	} while (0)

std::vector<uint8_t> MakeSolidI420(uint32_t w, uint32_t h, uint8_t y_val, uint8_t u_val, uint8_t v_val)
{
	const uint32_t sz = awc::vcam::CalcI420Size(w, h);
	std::vector<uint8_t> buf(sz);
	const size_t y_len = static_cast<size_t>(w) * h;
	const size_t c_len = (static_cast<size_t>(w) / 2u) * (static_cast<size_t>(h) / 2u);
	std::fill(buf.begin(), buf.begin() + static_cast<ptrdiff_t>(y_len), y_val);
	std::fill(buf.begin() + static_cast<ptrdiff_t>(y_len),
		  buf.begin() + static_cast<ptrdiff_t>(y_len + c_len), u_val);
	std::fill(buf.begin() + static_cast<ptrdiff_t>(y_len + c_len), buf.end(), v_val);
	return buf;
}

void TestShmRoundTripAndStaleness()
{
	std::vector<uint8_t> backing(awc::vcam::kShmTotalBytes, 0);

	awc::vcam::VcamShmWriter writer;
	awc::vcam::VcamShmReader reader;
	CHECK(writer.AttachMemory(backing.data(), backing.size()));
	CHECK(reader.AttachMemory(backing.data(), backing.size()));

	// Before any frame is published, ReadLatestFrame returns false.
	awc::vcam::I420FrameSnapshot snap;
	CHECK(!reader.ReadLatestFrame(snap, 10000));

	// Publish frame 1 (64x48)
	auto frame1 = MakeSolidI420(64, 48, 200, 110, 140);
	awc::vcam::I420FrameInput in1{};
	in1.y = frame1.data();
	in1.u = frame1.data() + 64 * 48;
	in1.v = in1.u + 32 * 24;
	in1.stride_y = 64;
	in1.stride_u = 32;
	in1.stride_v = 32;
	in1.width = 64;
	in1.height = 48;
	in1.fps_num = 60;
	in1.fps_den = 1;
	in1.full_range = true;
	in1.pts_us = 16667;

	CHECK(writer.PublishFrame(in1, 10000));
	CHECK(reader.ReadLatestFrame(snap, 10100));
	CHECK(snap.width == 64);
	CHECK(snap.height == 48);
	CHECK(snap.fps_num == 60);
	CHECK(snap.full_range == true);
	CHECK(snap.seq == 1);
	CHECK(snap.i420.size() == frame1.size());
	CHECK(snap.i420[0] == 200);
	CHECK(snap.i420[64 * 48] == 110);
	CHECK(snap.i420[64 * 48 + 32 * 24] == 140);

	// Publish frame 2 into alternate slot (128x96)
	auto frame2 = MakeSolidI420(128, 96, 80, 160, 90);
	awc::vcam::I420FrameInput in2{};
	in2.y = frame2.data();
	in2.u = frame2.data() + 128 * 96;
	in2.v = in2.u + 64 * 48;
	in2.stride_y = 128;
	in2.stride_u = 64;
	in2.stride_v = 64;
	in2.width = 128;
	in2.height = 96;
	in2.fps_num = 30;
	in2.fps_den = 1;
	in2.full_range = false;
	in2.pts_us = 33333;

	CHECK(writer.PublishFrame(in2, 10200));
	CHECK(reader.ReadLatestFrame(snap, 10300));
	CHECK(snap.width == 128);
	CHECK(snap.height == 96);
	CHECK(snap.seq == 2);
	CHECK(snap.full_range == false);
	CHECK(snap.i420[0] == 80);

	// Stale heartbeat should return false.
	CHECK(!reader.ReadLatestFrame(snap, 10200 + awc::vcam::kStaleTimeoutMs + 500));

	// TouchHeartbeat revives freshness.
	writer.TouchHeartbeat(20000);
	CHECK(reader.ReadLatestFrame(snap, 20100));

	// MarkInactive immediately stops returning live frames.
	writer.MarkInactive();
	CHECK(!reader.ReadLatestFrame(snap, 20100));
}

void TestI420ToRgb24AndScaling()
{
	// 1. Neutral white in limited-range BT.601: Y=235, U=128, V=128 -> B=255, G=255, R=255.
	auto white = MakeSolidI420(16, 16, 235, 128, 128);
	const uint32_t stride = awc::vcam::CalcRgb24Stride(16);
	std::vector<uint8_t> rgb(stride * 16, 0);
	awc::vcam::ConvertI420ToRgb24(white.data(), 16, 16, false, rgb.data(), 16, 16, stride, true);
	CHECK(rgb[0] >= 250 && rgb[1] >= 250 && rgb[2] >= 250);

	// 2. Bottom-up orientation check: top half bright (Y=235), bottom half dark (Y=16).
	auto split = MakeSolidI420(16, 16, 16, 128, 128);
	std::fill(split.begin(), split.begin() + 16 * 8, 235);
	awc::vcam::ConvertI420ToRgb24(split.data(), 16, 16, false, rgb.data(), 16, 16, stride, true);
	// In bottom-up DIB, row 0 in memory is the visual bottom (dark), row 15 is visual top (bright).
	CHECK(rgb[0] < 10);
	CHECK(rgb[15 * stride] > 245);

	// 3. Portrait 64x128 into landscape 128x64 -> pillarbox left & right black, center bright.
	auto portrait = MakeSolidI420(64, 128, 235, 128, 128);
	const uint32_t dst_stride = awc::vcam::CalcRgb24Stride(128);
	std::vector<uint8_t> dst(dst_stride * 64, 99);
	awc::vcam::ConvertI420ToRgb24(portrait.data(), 64, 128, false, dst.data(), 128, 64, dst_stride, true);
	// Left edge (x=0, y=32) should be black pillarbox (0,0,0)
	const uint8_t *mid_row = dst.data() + 32 * dst_stride;
	CHECK(mid_row[0] == 0 && mid_row[1] == 0 && mid_row[2] == 0);
	// Center pixel (x=64, y=32) should be white
	CHECK(mid_row[64 * 3] > 245 && mid_row[64 * 3 + 1] > 245 && mid_row[64 * 3 + 2] > 245);
}

void TestI420ToYuy2AndStandby()
{
	auto frame = MakeSolidI420(16, 16, 180, 100, 150);
	std::vector<uint8_t> yuy2(16 * 16 * 2, 0);
	awc::vcam::ConvertI420ToYuy2(frame.data(), 16, 16, yuy2.data(), 16, 16, 16 * 2);
	CHECK(yuy2[0] == 180);
	CHECK(yuy2[1] == 100);
	CHECK(yuy2[2] == 180);
	CHECK(yuy2[3] == 150);

	// Scaling portrait into landscape in YUY2
	auto portrait = MakeSolidI420(64, 128, 210, 120, 130);
	std::vector<uint8_t> yuy2_scaled(128 * 64 * 2, 0);
	awc::vcam::ConvertI420ToYuy2(portrait.data(), 64, 128, yuy2_scaled.data(), 128, 64, 128 * 2);
	const uint8_t *mid_row = yuy2_scaled.data() + 32 * (128 * 2);
	CHECK(mid_row[0] == 16 && mid_row[1] == 128); // black pillarbox
	CHECK(mid_row[64 * 2] == 210);                // active center

	// Standby frames animate cleanly
	const uint32_t s_rgb = awc::vcam::CalcRgb24Stride(64);
	std::vector<uint8_t> st0(s_rgb * 64, 0), st1(s_rgb * 64, 0);
	awc::vcam::RenderStandbyRgb24(st0.data(), 64, 64, s_rgb, 0, true);
	awc::vcam::RenderStandbyRgb24(st1.data(), 64, 64, s_rgb, 5, true);
	CHECK(st0 != st1);

	std::vector<uint8_t> sy0(64 * 64 * 2, 0), sy1(64 * 64 * 2, 0);
	awc::vcam::RenderStandbyYuy2(sy0.data(), 64, 64, 64 * 2, 0);
	awc::vcam::RenderStandbyYuy2(sy1.data(), 64, 64, 64 * 2, 5);
	CHECK(sy0 != sy1);
}

void TestFfmpegE2EPipeline()
{
	const auto tmp_dir = std::filesystem::temp_directory_path();
	const auto h264_path = tmp_dir / "awc_vcam_e2e_test.h264";
	std::error_code ec;
	std::filesystem::remove(h264_path, ec);

	const std::string cmd =
		"ffmpeg -y -hide_banner -loglevel error -f lavfi -i testsrc=duration=0.4:size=320x240:rate=30 "
		"-pix_fmt yuv420p -c:v libx264 -tune zerolatency -profile:v baseline -f h264 \"" +
		h264_path.string() + "\"";
	const int rc = std::system(cmd.c_str());
	if (rc != 0 || !std::filesystem::exists(h264_path)) {
		std::cout << "[SKIP] ffmpeg CLI with libx264 unavailable; skipping E2E decode test.\n";
		return;
	}

	std::vector<uint8_t> backing(awc::vcam::kShmTotalBytes, 0);
	awc::vcam::VcamShmWriter writer;
	awc::vcam::VcamShmReader reader;
	CHECK(writer.AttachMemory(backing.data(), backing.size()));
	CHECK(reader.AttachMemory(backing.data(), backing.size()));

	awc::AwcDecoder decoder;
	awc::AwcDecoder::Options opts;
	opts.url = h264_path.string();
	opts.codec = "h264";
	opts.open_timeout_ms = 3000;
	opts.read_timeout_ms = 3000;

	int published_count = 0;
	int64_t decoded_count = 0;
	std::string err;

	const bool ok = decoder.Run(
		opts,
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
			in.fps_num = 30;
			in.fps_den = 1;
			in.full_range = frame.full_range;
			in.pts_us = frame.pts_us;
			if (writer.PublishFrame(in)) {
				++published_count;
			}
		},
		err, &decoded_count);

	CHECK(ok);
	CHECK(decoded_count >= 5);
	CHECK(published_count == decoded_count);

	awc::vcam::I420FrameSnapshot snap;
	CHECK(reader.ReadLatestFrame(snap));
	CHECK(snap.width == 320);
	CHECK(snap.height == 240);

	// Convert decoded 320x240 I420 to 1280x720 RGB24 & YUY2 (as VTube Studio negotiates)
	const uint32_t rgb_stride = awc::vcam::CalcRgb24Stride(1280);
	std::vector<uint8_t> rgb720(rgb_stride * 720, 0);
	awc::vcam::ConvertI420ToRgb24(snap.i420.data(), snap.width, snap.height, snap.full_range,
				      rgb720.data(), 1280, 720, rgb_stride, true);
	uint64_t non_zero = 0;
	for (uint8_t b : rgb720) {
		if (b != 0) ++non_zero;
	}
	CHECK(non_zero > 10000);

	std::vector<uint8_t> yuy2_720(1280 * 720 * 2, 0);
	awc::vcam::ConvertI420ToYuy2(snap.i420.data(), snap.width, snap.height,
				     yuy2_720.data(), 1280, 720, 1280 * 2);
	CHECK(yuy2_720.size() == 1280u * 720u * 2u);

	std::filesystem::remove(h264_path, ec);
}

} // namespace

int main()
{
	TestShmRoundTripAndStaleness();
	TestI420ToRgb24AndScaling();
	TestI420ToYuy2AndStandby();
	TestFfmpegE2EPipeline();

	if (g_failures > 0) {
		std::cerr << "FAILED: " << g_failures << " assertion(s) failed.\n";
		return 1;
	}
	std::cout << "ALL win-vcam unit & E2E tests PASSED!\n";
	return 0;
}
