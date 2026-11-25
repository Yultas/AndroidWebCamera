#include "vcam-shm.hpp"

#include <atomic>
#include <chrono>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace awc::vcam {

namespace {

uint32_t CurrentProcessId()
{
#ifdef _WIN32
	return static_cast<uint32_t>(GetCurrentProcessId());
#else
	return static_cast<uint32_t>(getpid());
#endif
}

template <typename T>
T VolatileLoad(const T *ptr)
{
	T v = *reinterpret_cast<const volatile T *>(ptr);
	std::atomic_thread_fence(std::memory_order_acquire);
	return v;
}

template <typename T>
void VolatileStore(T *ptr, T value)
{
	std::atomic_thread_fence(std::memory_order_release);
	*reinterpret_cast<volatile T *>(ptr) = value;
}

} // namespace

uint64_t CurrentTimeMs()
{
	const auto now = std::chrono::system_clock::now().time_since_epoch();
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

uint32_t CalcI420Size(uint32_t width, uint32_t height)
{
	if (width < 2 || height < 2 || (width & 1u) != 0 || (height & 1u) != 0) {
		return 0;
	}
	if (width > kMaxFrameDim || height > kMaxFrameDim) {
		return 0;
	}
	const uint64_t y_bytes = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
	if (y_bytes > kMaxFramePixels) {
		return 0;
	}
	const uint64_t total = (y_bytes * 3u) / 2u;
	if (total == 0 || total > kMaxI420Bytes) {
		return 0;
	}
	return static_cast<uint32_t>(total);
}

VcamShmWriter::~VcamShmWriter()
{
	Close();
}

bool VcamShmWriter::Open(const std::string &shm_name, const std::string &event_name, std::string &out_error)
{
	Close();
#ifdef _WIN32
	const DWORD size_high = static_cast<DWORD>((static_cast<uint64_t>(kShmTotalBytes) >> 32) & 0xFFFFFFFFu);
	const DWORD size_low = static_cast<DWORD>(static_cast<uint64_t>(kShmTotalBytes) & 0xFFFFFFFFu);

	HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
					    size_high, size_low, shm_name.c_str());
	if (!mapping) {
		out_error = "CreateFileMappingA failed (err=" + std::to_string(GetLastError()) + ")";
		return false;
	}

	void *view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, kShmTotalBytes);
	if (!view) {
		out_error = "MapViewOfFile failed (err=" + std::to_string(GetLastError()) + ")";
		CloseHandle(mapping);
		return false;
	}

	HANDLE ev = nullptr;
	if (!event_name.empty()) {
		ev = CreateEventA(nullptr, FALSE, FALSE, event_name.c_str());
	}

	mapping_handle_ = mapping;
	event_handle_ = ev;
	base_ = static_cast<uint8_t *>(view);
	owns_mapping_ = true;

	auto *hdr = reinterpret_cast<VcamShmHeader *>(base_);
	std::memset(hdr, 0, sizeof(VcamShmHeader));
	hdr->magic = kShmMagic;
	hdr->version = kShmVersion;
	hdr->writer_pid = CurrentProcessId();
	hdr->active_slot = 0;
	hdr->seq = 0;
	hdr->heartbeat_ms = CurrentTimeMs();
	VolatileStore(&hdr->writer_active, 1u);
	seq_ = 0;
	next_slot_ = 0;
	return true;
#else
	(void)shm_name;
	(void)event_name;
	out_error = "OS-backed VCam shared memory is only available on Windows (use AttachMemory for tests)";
	return false;
#endif
}

bool VcamShmWriter::AttachMemory(uint8_t *buffer, size_t size)
{
	Close();
	if (!buffer || size < kShmTotalBytes) {
		return false;
	}
	base_ = buffer;
	owns_mapping_ = false;
	auto *hdr = reinterpret_cast<VcamShmHeader *>(base_);
	std::memset(hdr, 0, sizeof(VcamShmHeader));
	hdr->magic = kShmMagic;
	hdr->version = kShmVersion;
	hdr->writer_pid = CurrentProcessId();
	hdr->active_slot = 0;
	hdr->seq = 0;
	hdr->heartbeat_ms = CurrentTimeMs();
	VolatileStore(&hdr->writer_active, 1u);
	seq_ = 0;
	next_slot_ = 0;
	return true;
}

void VcamShmWriter::Close()
{
	if (base_) {
		MarkInactive();
	}
#ifdef _WIN32
	if (owns_mapping_ && base_) {
		UnmapViewOfFile(base_);
	}
	if (event_handle_) {
		CloseHandle(static_cast<HANDLE>(event_handle_));
		event_handle_ = nullptr;
	}
	if (mapping_handle_) {
		CloseHandle(static_cast<HANDLE>(mapping_handle_));
		mapping_handle_ = nullptr;
	}
#endif
	base_ = nullptr;
	owns_mapping_ = false;
	seq_ = 0;
	next_slot_ = 0;
}

bool VcamShmWriter::PublishFrame(const I420FrameInput &frame, uint64_t now_ms)
{
	if (!base_ || !frame.y || !frame.u || !frame.v) {
		return false;
	}
	const uint32_t data_size = CalcI420Size(frame.width, frame.height);
	if (data_size == 0) {
		return false;
	}
	const uint32_t w = frame.width;
	const uint32_t h = frame.height;
	const uint32_t cw = w / 2u;
	const uint32_t ch = h / 2u;
	if (frame.stride_y < w || frame.stride_u < cw || frame.stride_v < cw) {
		return false;
	}

	auto *hdr = reinterpret_cast<VcamShmHeader *>(base_);
	const uint32_t slot = next_slot_ % kShmSlotCount;
	uint8_t *dst = base_ + kShmDataOffset + static_cast<size_t>(slot) * kSlotStrideBytes;

	uint8_t *dst_y = dst;
	uint8_t *dst_u = dst_y + static_cast<size_t>(w) * h;
	uint8_t *dst_v = dst_u + static_cast<size_t>(cw) * ch;

	for (uint32_t row = 0; row < h; ++row) {
		std::memcpy(dst_y + static_cast<size_t>(row) * w,
			    frame.y + static_cast<size_t>(row) * frame.stride_y, w);
	}
	for (uint32_t row = 0; row < ch; ++row) {
		std::memcpy(dst_u + static_cast<size_t>(row) * cw,
			    frame.u + static_cast<size_t>(row) * frame.stride_u, cw);
		std::memcpy(dst_v + static_cast<size_t>(row) * cw,
			    frame.v + static_cast<size_t>(row) * frame.stride_v, cw);
	}

	const uint64_t new_seq = seq_ + 1u;
	VcamSlotMeta &meta = hdr->slots[slot];
	meta.width = w;
	meta.height = h;
	meta.fps_num = frame.fps_num > 0 ? frame.fps_num : 30u;
	meta.fps_den = frame.fps_den > 0 ? frame.fps_den : 1u;
	meta.full_range = frame.full_range ? 1u : 0u;
	meta.data_size = data_size;
	meta.pts_us = frame.pts_us;
	meta.frame_seq = new_seq;

	const uint64_t ts = (now_ms > 0) ? now_ms : CurrentTimeMs();
	VolatileStore(&hdr->heartbeat_ms, ts);
	VolatileStore(&hdr->writer_active, 1u);
	VolatileStore(&hdr->active_slot, slot);
	VolatileStore(&hdr->seq, new_seq);

	seq_ = new_seq;
	next_slot_ = (slot + 1u) % kShmSlotCount;

#ifdef _WIN32
	if (event_handle_) {
		SetEvent(static_cast<HANDLE>(event_handle_));
	}
#endif
	return true;
}

void VcamShmWriter::TouchHeartbeat(uint64_t now_ms)
{
	if (!base_) {
		return;
	}
	auto *hdr = reinterpret_cast<VcamShmHeader *>(base_);
	const uint64_t ts = (now_ms > 0) ? now_ms : CurrentTimeMs();
	VolatileStore(&hdr->heartbeat_ms, ts);
}

void VcamShmWriter::MarkInactive()
{
	if (!base_) {
		return;
	}
	auto *hdr = reinterpret_cast<VcamShmHeader *>(base_);
	VolatileStore(&hdr->writer_active, 0u);
#ifdef _WIN32
	if (event_handle_) {
		SetEvent(static_cast<HANDLE>(event_handle_));
	}
#endif
}

VcamShmReader::~VcamShmReader()
{
	Close();
}

bool VcamShmReader::TryOpen(const std::string &shm_name, const std::string &event_name)
{
	if (base_) {
		return true;
	}
#ifdef _WIN32
	HANDLE mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, shm_name.c_str());
	if (!mapping) {
		return false;
	}
	const void *view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, kShmTotalBytes);
	if (!view) {
		CloseHandle(mapping);
		return false;
	}
	HANDLE ev = nullptr;
	if (!event_name.empty()) {
		ev = OpenEventA(SYNCHRONIZE, FALSE, event_name.c_str());
	}
	mapping_handle_ = mapping;
	event_handle_ = ev;
	base_ = static_cast<const uint8_t *>(view);
	owns_mapping_ = true;
	return true;
#else
	(void)shm_name;
	(void)event_name;
	return false;
#endif
}

bool VcamShmReader::AttachMemory(const uint8_t *buffer, size_t size)
{
	Close();
	if (!buffer || size < kShmTotalBytes) {
		return false;
	}
	base_ = buffer;
	owns_mapping_ = false;
	return true;
}

void VcamShmReader::Close()
{
#ifdef _WIN32
	if (owns_mapping_ && base_) {
		UnmapViewOfFile(base_);
	}
	if (event_handle_) {
		CloseHandle(static_cast<HANDLE>(event_handle_));
		event_handle_ = nullptr;
	}
	if (mapping_handle_) {
		CloseHandle(static_cast<HANDLE>(mapping_handle_));
		mapping_handle_ = nullptr;
	}
#endif
	base_ = nullptr;
	owns_mapping_ = false;
}

bool VcamShmReader::ReadLatestFrame(I420FrameSnapshot &out_frame, uint64_t now_ms,
				    uint64_t stale_timeout_ms) const
{
	if (!base_) {
		return false;
	}
	const auto *hdr = reinterpret_cast<const VcamShmHeader *>(base_);
	if (VolatileLoad(&hdr->magic) != kShmMagic || VolatileLoad(&hdr->version) != kShmVersion) {
		return false;
	}
	if (VolatileLoad(&hdr->writer_active) == 0u) {
		return false;
	}

	const uint64_t ts_now = (now_ms > 0) ? now_ms : CurrentTimeMs();
	const uint64_t hb = VolatileLoad(&hdr->heartbeat_ms);
	if (hb == 0 || (ts_now > hb && (ts_now - hb) > stale_timeout_ms)) {
		return false;
	}

	// Retry up to 3 times if the writer flipped slots concurrently.
	for (int attempt = 0; attempt < 3; ++attempt) {
		const uint64_t seq_before = VolatileLoad(&hdr->seq);
		if (seq_before == 0) {
			return false;
		}
		const uint32_t slot = VolatileLoad(&hdr->active_slot);
		if (slot >= kShmSlotCount) {
			return false;
		}
		const VcamSlotMeta meta = hdr->slots[slot];
		const uint32_t expected_size = CalcI420Size(meta.width, meta.height);
		if (expected_size == 0 || meta.data_size != expected_size || meta.frame_seq != seq_before) {
			continue;
		}

		out_frame.i420.resize(expected_size);
		const uint8_t *src = base_ + kShmDataOffset + static_cast<size_t>(slot) * kSlotStrideBytes;
		std::memcpy(out_frame.i420.data(), src, expected_size);

		std::atomic_thread_fence(std::memory_order_acquire);
		const uint64_t seq_after = VolatileLoad(&hdr->seq);
		const uint32_t slot_after = VolatileLoad(&hdr->active_slot);
		if (seq_after == seq_before || (slot_after != slot && seq_after == seq_before + 1u)) {
			// Either no new frame arrived, or the writer only touched the *other* slot.
			out_frame.width = meta.width;
			out_frame.height = meta.height;
			out_frame.fps_num = meta.fps_num > 0 ? meta.fps_num : 30u;
			out_frame.fps_den = meta.fps_den > 0 ? meta.fps_den : 1u;
			out_frame.full_range = meta.full_range != 0;
			out_frame.pts_us = meta.pts_us;
			out_frame.seq = seq_before;
			return true;
		}
	}
	return false;
}

bool VcamShmReader::ReadFrameIfUpdated(I420FrameSnapshot &io_frame, bool &out_updated,
				       uint64_t now_ms, uint64_t stale_timeout_ms) const
{
	out_updated = false;
	if (!base_) {
		return false;
	}
	const auto *hdr = reinterpret_cast<const VcamShmHeader *>(base_);
	if (VolatileLoad(&hdr->magic) != kShmMagic || VolatileLoad(&hdr->version) != kShmVersion) {
		return false;
	}
	if (VolatileLoad(&hdr->writer_active) == 0u) {
		return false;
	}

	const uint64_t ts_now = (now_ms > 0) ? now_ms : CurrentTimeMs();
	const uint64_t hb = VolatileLoad(&hdr->heartbeat_ms);
	if (hb == 0 || (ts_now > hb && (ts_now - hb) > stale_timeout_ms)) {
		return false;
	}

	const uint64_t seq_cur = VolatileLoad(&hdr->seq);
	if (seq_cur == 0) {
		return false;
	}
	if (seq_cur == io_frame.seq && !io_frame.i420.empty()) {
		return true;
	}

	if (ReadLatestFrame(io_frame, ts_now, stale_timeout_ms)) {
		out_updated = true;
		return true;
	}
	return false;
}

void VcamShmReader::WaitForFrame(uint32_t timeout_ms) const
{
#ifdef _WIN32
	if (event_handle_) {
		WaitForSingleObject(static_cast<HANDLE>(event_handle_), timeout_ms);
	}
#else
	(void)timeout_ms;
#endif
}

} // namespace awc::vcam
