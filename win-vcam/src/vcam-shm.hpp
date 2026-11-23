#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace awc::vcam {

constexpr uint32_t kShmMagic = 0x41574356u; // "AWCV"
constexpr uint32_t kShmVersion = 1u;
constexpr uint32_t kShmSlotCount = 2u;

// Maximum supported frame size in shared memory: 3840x2160 planar I420 (12,441,600 bytes).
constexpr uint32_t kMaxFrameWidth = 3840u;
constexpr uint32_t kMaxFrameHeight = 2160u;
constexpr uint32_t kMaxI420Bytes = (kMaxFrameWidth * kMaxFrameHeight * 3u) / 2u;
constexpr uint32_t kSlotStrideBytes = ((kMaxI420Bytes + 4095u) / 4096u) * 4096u;

// Default Windows kernel object names (Local\ works across same-session x64 & x86 processes without admin).
constexpr const char *kDefaultShmName = "Local\\AndroidWebCamera_VCam_v1";
constexpr const char *kDefaultEventName = "Local\\AndroidWebCamera_VCam_Event_v1";

// If the host hasn't updated heartbeat_ms within this window, the filter renders a standby placeholder.
constexpr uint64_t kStaleTimeoutMs = 2500u;

// Fixed-width, identical layout on both 32-bit (x86) and 64-bit (x64) builds.
struct alignas(8) VcamSlotMeta {
	uint32_t width;
	uint32_t height;
	uint32_t fps_num;
	uint32_t fps_den;
	uint32_t full_range; // 0 = limited (TV), 1 = full (PC)
	uint32_t data_size;  // width * height * 3 / 2
	int64_t pts_us;
	uint64_t frame_seq;
};

static_assert(sizeof(VcamSlotMeta) == 40, "VcamSlotMeta must have fixed 40-byte ABI layout");

struct alignas(8) VcamShmHeader {
	uint32_t magic;
	uint32_t version;
	uint32_t writer_pid;
	uint32_t active_slot;   // 0 or 1
	uint32_t writer_active; // 1 while host is actively streaming, 0 on clean shutdown
	uint32_t reserved0;
	uint64_t seq;          // Monotonic frame counter, incremented on publish
	uint64_t heartbeat_ms; // Monotonic/UTC ms timestamp updated on every frame & keepalive
	VcamSlotMeta slots[kShmSlotCount];
};

static_assert(sizeof(VcamShmHeader) == 120, "VcamShmHeader must have fixed 120-byte ABI layout");

constexpr size_t kShmDataOffset = 4096u;
constexpr size_t kShmTotalBytes = kShmDataOffset + static_cast<size_t>(kShmSlotCount) * kSlotStrideBytes;

struct I420FrameInput {
	const uint8_t *y = nullptr;
	const uint8_t *u = nullptr;
	const uint8_t *v = nullptr;
	uint32_t stride_y = 0;
	uint32_t stride_u = 0;
	uint32_t stride_v = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t fps_num = 30;
	uint32_t fps_den = 1;
	bool full_range = false;
	int64_t pts_us = 0;
};

struct I420FrameSnapshot {
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t fps_num = 30;
	uint32_t fps_den = 1;
	bool full_range = false;
	int64_t pts_us = 0;
	uint64_t seq = 0;
	std::vector<uint8_t> i420; // Tightly packed Y, U, V planes
};

/** Returns current wall-clock UTC milliseconds (shared across processes on the same machine). */
uint64_t CurrentTimeMs();

/** Validates dimensions and returns tightly packed I420 byte size, or 0 if invalid/too large. */
uint32_t CalcI420Size(uint32_t width, uint32_t height);

/**
 * Shared-memory producer used by awc-vcam.exe (and unit tests).
 * Supports both OS-backed shared memory (Windows CreateFileMapping) and in-memory buffers for tests.
 */
class VcamShmWriter {
public:
	VcamShmWriter() = default;
	~VcamShmWriter();

	VcamShmWriter(const VcamShmWriter &) = delete;
	VcamShmWriter &operator=(const VcamShmWriter &) = delete;

	/** Creates or opens OS shared memory (Windows). */
	bool Open(const std::string &shm_name, const std::string &event_name, std::string &out_error);

	/** Attaches to an externally allocated buffer of at least kShmTotalBytes (for unit tests). */
	bool AttachMemory(uint8_t *buffer, size_t size);

	void Close();
	bool IsOpen() const { return base_ != nullptr; }

	/** Copies a planar I420 frame into the inactive slot and atomically publishes it. */
	bool PublishFrame(const I420FrameInput &frame, uint64_t now_ms = 0);

	/** Updates heartbeat_ms without publishing a new video frame. */
	void TouchHeartbeat(uint64_t now_ms = 0);

	/** Marks writer_active = 0 so consumers immediately show the standby frame. */
	void MarkInactive();

private:
	uint8_t *base_ = nullptr;
	bool owns_mapping_ = false;
	uint64_t seq_ = 0;
	uint32_t next_slot_ = 0;
#ifdef _WIN32
	void *mapping_handle_ = nullptr;
	void *event_handle_ = nullptr;
#endif
};

/**
 * Shared-memory consumer used by the DirectShow filter DLL (and unit tests).
 */
class VcamShmReader {
public:
	VcamShmReader() = default;
	~VcamShmReader();

	VcamShmReader(const VcamShmReader &) = delete;
	VcamShmReader &operator=(const VcamShmReader &) = delete;

	/** Attempts to open existing OS shared memory (Windows). Safe to retry periodically. */
	bool TryOpen(const std::string &shm_name, const std::string &event_name);

	/** Attaches to an externally allocated buffer of at least kShmTotalBytes (for unit tests). */
	bool AttachMemory(const uint8_t *buffer, size_t size);

	void Close();
	bool IsOpen() const { return base_ != nullptr; }

	/**
	 * Copies the latest published frame into `out_frame` if the writer is active and fresh.
	 * Returns true if a valid live frame was copied; false if disconnected/stale/empty.
	 */
	bool ReadLatestFrame(I420FrameSnapshot &out_frame, uint64_t now_ms = 0,
			     uint64_t stale_timeout_ms = kStaleTimeoutMs) const;

	/** Waits up to `timeout_ms` for the producer event (Windows only; returns immediately elsewhere). */
	void WaitForFrame(uint32_t timeout_ms) const;

private:
	const uint8_t *base_ = nullptr;
	bool owns_mapping_ = false;
#ifdef _WIN32
	void *mapping_handle_ = nullptr;
	void *event_handle_ = nullptr;
#endif
};

} // namespace awc::vcam
