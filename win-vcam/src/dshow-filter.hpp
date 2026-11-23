#pragma once

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <dshow.h>
#include <dvdmedia.h>
#include <ks.h>
#include <ksmedia.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "vcam-shm.hpp"

namespace awc::vcam {

// Unique CLSID for AndroidWebCamera DirectShow virtual camera:
// {A7C8E4B1-5F29-4D3A-9C1E-8B4E7F2A1031}
// clang-format off
static constexpr GUID CLSID_AndroidWebCamera = {
	0xa7c8e4b1, 0x5f29, 0x4d3a, {0x9c, 0x1e, 0x8b, 0x4e, 0x7f, 0x2a, 0x10, 0x31}
};
// clang-format on

static constexpr wchar_t kFilterFriendlyName[] = L"AndroidWebCamera";
static constexpr wchar_t kCapturePinName[] = L"Capture";

extern std::atomic<long> g_dll_ref_count;

struct VideoMode {
	uint32_t width = 1280;
	uint32_t height = 720;
	uint32_t fps = 60;
	bool is_yuy2 = false; // false = RGB24, true = YUY2
};

class AwcVirtualCamPin;

class AwcVirtualCamFilter final : public IBaseFilter, public IAMFilterMiscFlags {
public:
	AwcVirtualCamFilter();
	~AwcVirtualCamFilter();

	// IUnknown
	STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
	STDMETHODIMP_(ULONG) AddRef() override;
	STDMETHODIMP_(ULONG) Release() override;

	// IPersist
	STDMETHODIMP GetClassID(CLSID *pClassID) override;

	// IMediaFilter
	STDMETHODIMP Stop() override;
	STDMETHODIMP Pause() override;
	STDMETHODIMP Run(REFERENCE_TIME tStart) override;
	STDMETHODIMP GetState(DWORD dwMilliSecsTimeout, FILTER_STATE *State) override;
	STDMETHODIMP SetSyncSource(IReferenceClock *pClock) override;
	STDMETHODIMP GetSyncSource(IReferenceClock **pClock) override;

	// IBaseFilter
	STDMETHODIMP EnumPins(IEnumPins **ppEnum) override;
	STDMETHODIMP FindPin(LPCWSTR Id, IPin **ppPin) override;
	STDMETHODIMP QueryFilterInfo(FILTER_INFO *pInfo) override;
	STDMETHODIMP JoinFilterGraph(IFilterGraph *pGraph, LPCWSTR pName) override;
	STDMETHODIMP QueryVendorInfo(LPWSTR *pVendorInfo) override;

	// IAMFilterMiscFlags
	STDMETHODIMP_(ULONG) GetMiscFlags() override;

	FILTER_STATE CurrentState() const;
	std::mutex &StateMutex() { return state_mutex_; }

private:
	std::atomic<long> ref_count_{1};
	mutable std::mutex state_mutex_;
	FILTER_STATE state_ = State_Stopped;
	IFilterGraph *graph_ = nullptr; // Weak reference per DirectShow contract
	IReferenceClock *clock_ = nullptr;
	std::wstring filter_name_ = kFilterFriendlyName;
	AwcVirtualCamPin *pin_ = nullptr;
};

class AwcVirtualCamPin final : public IPin,
			       public IAMStreamConfig,
			       public IKsPropertySet,
			       public IQualityControl {
public:
	explicit AwcVirtualCamPin(AwcVirtualCamFilter *filter);
	~AwcVirtualCamPin();

	// IUnknown
	STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
	STDMETHODIMP_(ULONG) AddRef() override;
	STDMETHODIMP_(ULONG) Release() override;

	// IPin
	STDMETHODIMP Connect(IPin *pReceivePin, const AM_MEDIA_TYPE *pmt) override;
	STDMETHODIMP ReceiveConnection(IPin *pConnector, const AM_MEDIA_TYPE *pmt) override;
	STDMETHODIMP Disconnect() override;
	STDMETHODIMP ConnectedTo(IPin **pPin) override;
	STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE *pmt) override;
	STDMETHODIMP QueryPinInfo(PIN_INFO *pInfo) override;
	STDMETHODIMP QueryDirection(PIN_DIRECTION *pPinDir) override;
	STDMETHODIMP QueryId(LPWSTR *Id) override;
	STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE *pmt) override;
	STDMETHODIMP EnumMediaTypes(IEnumMediaTypes **ppEnum) override;
	STDMETHODIMP QueryInternalConnections(IPin **apPin, ULONG *nPin) override;
	STDMETHODIMP EndOfStream() override;
	STDMETHODIMP BeginFlush() override;
	STDMETHODIMP EndFlush() override;
	STDMETHODIMP NewSegment(REFERENCE_TIME tStart, REFERENCE_TIME tStop, double dRate) override;

	// IAMStreamConfig
	STDMETHODIMP SetFormat(AM_MEDIA_TYPE *pmt) override;
	STDMETHODIMP GetFormat(AM_MEDIA_TYPE **ppmt) override;
	STDMETHODIMP GetNumberOfCapabilities(int *piCount, int *piSize) override;
	STDMETHODIMP GetStreamCaps(int iIndex, AM_MEDIA_TYPE **ppmt, BYTE *pSCC) override;

	// IKsPropertySet
	STDMETHODIMP Set(REFGUID guidPropSet, DWORD dwPropID, LPVOID pInstanceData,
			 DWORD cbInstanceData, LPVOID pPropData, DWORD cbPropData) override;
	STDMETHODIMP Get(REFGUID guidPropSet, DWORD dwPropID, LPVOID pInstanceData,
			 DWORD cbInstanceData, LPVOID pPropData, DWORD cbPropData,
			 DWORD *pcbReturned) override;
	STDMETHODIMP QuerySupported(REFGUID guidPropSet, DWORD dwPropID,
				    DWORD *pTypeSupport) override;

	// IQualityControl
	STDMETHODIMP Notify(IBaseFilter *pSelf, Quality q) override;
	STDMETHODIMP SetSink(IQualityControl *piqc) override;

	// Filter lifecycle helpers
	HRESULT OnPause();
	HRESULT OnRun();
	HRESULT OnStop();

	// Media type helpers
	static std::vector<VideoMode> SupportedModes();
	static HRESULT BuildMediaType(const VideoMode &mode, AM_MEDIA_TYPE *pmt);
	static bool ParseMediaType(const AM_MEDIA_TYPE *pmt, VideoMode &out_mode);
	static void FreeMediaTypeContents(AM_MEDIA_TYPE &mt);
	static HRESULT CopyMediaType(AM_MEDIA_TYPE *dst, const AM_MEDIA_TYPE *src);

private:
	void StreamLoop();

	AwcVirtualCamFilter *filter_ = nullptr; // Owning filter (pin lifetime tied to filter)

	mutable std::mutex pin_mutex_;
	IPin *connected_pin_ = nullptr;
	IMemInputPin *mem_input_ = nullptr;
	IMemAllocator *allocator_ = nullptr;
	VideoMode current_mode_{};
	bool has_connected_mt_ = false;
	AM_MEDIA_TYPE connected_mt_{};

	std::atomic<bool> streaming_{false};
	std::thread stream_thread_;
};

} // namespace awc::vcam

#endif // _WIN32
