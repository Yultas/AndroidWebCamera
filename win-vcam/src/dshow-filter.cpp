#ifdef _WIN32

#include "dshow-filter.hpp"
#include "vcam-convert.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mmsystem.h>

namespace awc::vcam {

std::atomic<long> g_dll_ref_count{0};

namespace {

constexpr DWORD kFourccYuy2 = MAKEFOURCC('Y', 'U', 'Y', '2');

uint32_t FrameByteSize(const VideoMode &mode)
{
	if (mode.is_yuy2) {
		return mode.width * mode.height * 2u;
	}
	return CalcRgb24Stride(mode.width) * mode.height;
}

class PinEnumerator final : public IEnumPins {
public:
	PinEnumerator(IPin *pin, ULONG index = 0) : pin_(pin), index_(index)
	{
		if (pin_) {
			pin_->AddRef();
		}
		g_dll_ref_count.fetch_add(1, std::memory_order_relaxed);
	}

	~PinEnumerator()
	{
		if (pin_) {
			pin_->Release();
		}
		g_dll_ref_count.fetch_sub(1, std::memory_order_relaxed);
	}

	STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
	{
		if (!ppv) return E_POINTER;
		if (riid == IID_IUnknown || riid == IID_IEnumPins) {
			*ppv = static_cast<IEnumPins *>(this);
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}

	STDMETHODIMP_(ULONG) AddRef() override
	{
		return static_cast<ULONG>(ref_count_.fetch_add(1, std::memory_order_relaxed) + 1);
	}

	STDMETHODIMP_(ULONG) Release() override
	{
		const long c = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
		if (c == 0) {
			delete this;
		}
		return static_cast<ULONG>(c);
	}

	STDMETHODIMP Next(ULONG cPins, IPin **ppPins, ULONG *pcFetched) override
	{
		if (!ppPins) return E_POINTER;
		if (cPins > 1 && !pcFetched) return E_INVALIDARG;

		ULONG fetched = 0;
		if (index_ == 0 && cPins > 0 && pin_) {
			ppPins[0] = pin_;
			pin_->AddRef();
			index_ = 1;
			fetched = 1;
		}
		if (pcFetched) {
			*pcFetched = fetched;
		}
		return (fetched == cPins) ? S_OK : S_FALSE;
	}

	STDMETHODIMP Skip(ULONG cPins) override
	{
		index_ = (index_ + cPins > 1) ? 1 : (index_ + cPins);
		return (cPins == 0 || index_ <= 1) ? S_OK : S_FALSE;
	}

	STDMETHODIMP Reset() override
	{
		index_ = 0;
		return S_OK;
	}

	STDMETHODIMP Clone(IEnumPins **ppEnum) override
	{
		if (!ppEnum) return E_POINTER;
		*ppEnum = new PinEnumerator(pin_, index_);
		return S_OK;
	}

private:
	std::atomic<long> ref_count_{1};
	IPin *pin_ = nullptr;
	ULONG index_ = 0;
};

class MediaTypeEnumerator final : public IEnumMediaTypes {
public:
	MediaTypeEnumerator(std::vector<VideoMode> modes, size_t index = 0)
		: modes_(std::move(modes)), index_(index)
	{
		g_dll_ref_count.fetch_add(1, std::memory_order_relaxed);
	}

	~MediaTypeEnumerator()
	{
		g_dll_ref_count.fetch_sub(1, std::memory_order_relaxed);
	}

	STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
	{
		if (!ppv) return E_POINTER;
		if (riid == IID_IUnknown || riid == IID_IEnumMediaTypes) {
			*ppv = static_cast<IEnumMediaTypes *>(this);
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}

	STDMETHODIMP_(ULONG) AddRef() override
	{
		return static_cast<ULONG>(ref_count_.fetch_add(1, std::memory_order_relaxed) + 1);
	}

	STDMETHODIMP_(ULONG) Release() override
	{
		const long c = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
		if (c == 0) {
			delete this;
		}
		return static_cast<ULONG>(c);
	}

	STDMETHODIMP Next(ULONG cMediaTypes, AM_MEDIA_TYPE **ppMediaTypes, ULONG *pcFetched) override
	{
		if (!ppMediaTypes) return E_POINTER;
		if (cMediaTypes > 1 && !pcFetched) return E_INVALIDARG;

		ULONG fetched = 0;
		while (fetched < cMediaTypes && index_ < modes_.size()) {
			auto *mt = static_cast<AM_MEDIA_TYPE *>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
			if (!mt) {
				if (pcFetched) *pcFetched = fetched;
				return E_OUTOFMEMORY;
			}
			std::memset(mt, 0, sizeof(AM_MEDIA_TYPE));
			const HRESULT hr = AwcVirtualCamPin::BuildMediaType(modes_[index_], mt);
			if (FAILED(hr)) {
				CoTaskMemFree(mt);
				if (pcFetched) *pcFetched = fetched;
				return hr;
			}
			ppMediaTypes[fetched] = mt;
			++fetched;
			++index_;
		}
		if (pcFetched) {
			*pcFetched = fetched;
		}
		return (fetched == cMediaTypes) ? S_OK : S_FALSE;
	}

	STDMETHODIMP Skip(ULONG cMediaTypes) override
	{
		if (index_ + cMediaTypes > modes_.size()) {
			index_ = modes_.size();
			return S_FALSE;
		}
		index_ += cMediaTypes;
		return S_OK;
	}

	STDMETHODIMP Reset() override
	{
		index_ = 0;
		return S_OK;
	}

	STDMETHODIMP Clone(IEnumMediaTypes **ppEnum) override
	{
		if (!ppEnum) return E_POINTER;
		*ppEnum = new MediaTypeEnumerator(modes_, index_);
		return S_OK;
	}

private:
	std::atomic<long> ref_count_{1};
	std::vector<VideoMode> modes_;
	size_t index_ = 0;
};

} // namespace

// ============================================================================
// AwcVirtualCamFilter
// ============================================================================

AwcVirtualCamFilter::AwcVirtualCamFilter()
{
	g_dll_ref_count.fetch_add(1, std::memory_order_relaxed);
	pin_ = new AwcVirtualCamPin(this);
}

AwcVirtualCamFilter::~AwcVirtualCamFilter()
{
	if (pin_) {
		pin_->OnStop();
		delete pin_;
		pin_ = nullptr;
	}
	if (clock_) {
		clock_->Release();
		clock_ = nullptr;
	}
	g_dll_ref_count.fetch_sub(1, std::memory_order_relaxed);
}

STDMETHODIMP AwcVirtualCamFilter::QueryInterface(REFIID riid, void **ppv)
{
	if (!ppv) return E_POINTER;
	if (riid == IID_IUnknown || riid == IID_IPersist ||
	    riid == IID_IMediaFilter || riid == IID_IBaseFilter) {
		*ppv = static_cast<IBaseFilter *>(this);
		AddRef();
		return S_OK;
	}
	if (riid == IID_IAMFilterMiscFlags) {
		*ppv = static_cast<IAMFilterMiscFlags *>(this);
		AddRef();
		return S_OK;
	}
	*ppv = nullptr;
	return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) AwcVirtualCamFilter::AddRef()
{
	return static_cast<ULONG>(ref_count_.fetch_add(1, std::memory_order_relaxed) + 1);
}

STDMETHODIMP_(ULONG) AwcVirtualCamFilter::Release()
{
	const long c = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
	if (c == 0) {
		delete this;
	}
	return static_cast<ULONG>(c);
}

STDMETHODIMP AwcVirtualCamFilter::GetClassID(CLSID *pClassID)
{
	if (!pClassID) return E_POINTER;
	*pClassID = CLSID_AndroidWebCamera;
	return S_OK;
}

STDMETHODIMP AwcVirtualCamFilter::Stop()
{
	{
		std::lock_guard<std::mutex> lock(state_mutex_);
		state_ = State_Stopped;
	}
	if (pin_) {
		pin_->OnStop();
	}
	return S_OK;
}

STDMETHODIMP AwcVirtualCamFilter::Pause()
{
	FILTER_STATE prev = State_Stopped;
	{
		std::lock_guard<std::mutex> lock(state_mutex_);
		prev = state_;
		state_ = State_Paused;
	}
	if (pin_ && prev == State_Stopped) {
		const HRESULT hr = pin_->OnPause();
		if (FAILED(hr)) {
			return hr;
		}
	}
	return S_OK;
}

STDMETHODIMP AwcVirtualCamFilter::Run(REFERENCE_TIME /*tStart*/)
{
	{
		std::lock_guard<std::mutex> lock(state_mutex_);
		state_ = State_Running;
	}
	if (pin_) {
		return pin_->OnRun();
	}
	return S_OK;
}

STDMETHODIMP AwcVirtualCamFilter::GetState(DWORD /*dwMilliSecsTimeout*/, FILTER_STATE *State)
{
	if (!State) return E_POINTER;
	std::lock_guard<std::mutex> lock(state_mutex_);
	*State = state_;
	// Live push source returns VFW_S_CANT_CUE when paused per DirectShow spec.
	return (state_ == State_Paused) ? VFW_S_CANT_CUE : S_OK;
}

STDMETHODIMP AwcVirtualCamFilter::SetSyncSource(IReferenceClock *pClock)
{
	std::lock_guard<std::mutex> lock(state_mutex_);
	if (clock_) {
		clock_->Release();
	}
	clock_ = pClock;
	if (clock_) {
		clock_->AddRef();
	}
	return S_OK;
}

STDMETHODIMP AwcVirtualCamFilter::GetSyncSource(IReferenceClock **pClock)
{
	if (!pClock) return E_POINTER;
	std::lock_guard<std::mutex> lock(state_mutex_);
	*pClock = clock_;
	if (clock_) {
		clock_->AddRef();
	}
	return S_OK;
}

STDMETHODIMP AwcVirtualCamFilter::EnumPins(IEnumPins **ppEnum)
{
	if (!ppEnum) return E_POINTER;
	*ppEnum = new PinEnumerator(pin_, 0);
	return S_OK;
}

STDMETHODIMP AwcVirtualCamFilter::FindPin(LPCWSTR Id, IPin **ppPin)
{
	if (!ppPin) return E_POINTER;
	*ppPin = nullptr;
	if (!Id) return E_POINTER;
	if (wcscmp(Id, kCapturePinName) == 0 && pin_) {
		*ppPin = pin_;
		pin_->AddRef();
		return S_OK;
	}
	return VFW_E_NOT_FOUND;
}

STDMETHODIMP AwcVirtualCamFilter::QueryFilterInfo(FILTER_INFO *pInfo)
{
	if (!pInfo) return E_POINTER;
	std::lock_guard<std::mutex> lock(state_mutex_);
	wcsncpy_s(pInfo->achName, MAX_FILTER_NAME, filter_name_.c_str(), _TRUNCATE);
	pInfo->pGraph = graph_;
	if (graph_) {
		graph_->AddRef();
	}
	return S_OK;
}

STDMETHODIMP AwcVirtualCamFilter::JoinFilterGraph(IFilterGraph *pGraph, LPCWSTR pName)
{
	std::lock_guard<std::mutex> lock(state_mutex_);
	graph_ = pGraph;
	if (pName && pName[0] != L'\0') {
		filter_name_ = pName;
	} else {
		filter_name_ = kFilterFriendlyName;
	}
	return S_OK;
}

STDMETHODIMP AwcVirtualCamFilter::QueryVendorInfo(LPWSTR *pVendorInfo)
{
	if (!pVendorInfo) return E_POINTER;
	const wchar_t vendor[] = L"AndroidWebCamera";
	const size_t bytes = sizeof(vendor);
	auto *out = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
	if (!out) return E_OUTOFMEMORY;
	std::memcpy(out, vendor, bytes);
	*pVendorInfo = out;
	return S_OK;
}

STDMETHODIMP_(ULONG) AwcVirtualCamFilter::GetMiscFlags()
{
	return AM_FILTER_MISC_FLAGS_IS_SOURCE;
}

FILTER_STATE AwcVirtualCamFilter::CurrentState() const
{
	std::lock_guard<std::mutex> lock(state_mutex_);
	return state_;
}

// ============================================================================
// AwcVirtualCamPin
// ============================================================================

AwcVirtualCamPin::AwcVirtualCamPin(AwcVirtualCamFilter *filter) : filter_(filter)
{
	std::memset(&connected_mt_, 0, sizeof(connected_mt_));
}

AwcVirtualCamPin::~AwcVirtualCamPin()
{
	OnStop();
	Disconnect();
}

STDMETHODIMP AwcVirtualCamPin::QueryInterface(REFIID riid, void **ppv)
{
	if (!ppv) return E_POINTER;
	if (riid == IID_IUnknown || riid == IID_IPin) {
		*ppv = static_cast<IPin *>(this);
		AddRef();
		return S_OK;
	}
	if (riid == IID_IAMStreamConfig) {
		*ppv = static_cast<IAMStreamConfig *>(this);
		AddRef();
		return S_OK;
	}
	if (riid == IID_IKsPropertySet) {
		*ppv = static_cast<IKsPropertySet *>(this);
		AddRef();
		return S_OK;
	}
	if (riid == IID_IQualityControl) {
		*ppv = static_cast<IQualityControl *>(this);
		AddRef();
		return S_OK;
	}
	*ppv = nullptr;
	return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) AwcVirtualCamPin::AddRef()
{
	return filter_->AddRef();
}

STDMETHODIMP_(ULONG) AwcVirtualCamPin::Release()
{
	return filter_->Release();
}

std::vector<VideoMode> AwcVirtualCamPin::SupportedModes()
{
	// Ordered with VTube Studio / OpenSeeFace sweet-spot (720p60 RGB24) first.
	return {
		{1280, 720, 60, false},
		{1280, 720, 30, false},
		{1920, 1080, 30, false},
		{1920, 1080, 60, false},
		{3840, 2160, 30, false},
		{3840, 2160, 60, false},
		{640, 480, 60, false},
		{640, 480, 30, false},
		{1280, 720, 60, true},
		{1280, 720, 30, true},
		{1920, 1080, 30, true},
		{1920, 1080, 60, true},
		{3840, 2160, 30, true},
		{3840, 2160, 60, true},
		{640, 480, 60, true},
		{640, 480, 30, true},
	};
}

HRESULT AwcVirtualCamPin::BuildMediaType(const VideoMode &mode, AM_MEDIA_TYPE *pmt)
{
	if (!pmt) return E_POINTER;
	std::memset(pmt, 0, sizeof(AM_MEDIA_TYPE));

	auto *vih = static_cast<VIDEOINFOHEADER *>(CoTaskMemAlloc(sizeof(VIDEOINFOHEADER)));
	if (!vih) return E_OUTOFMEMORY;
	std::memset(vih, 0, sizeof(VIDEOINFOHEADER));

	const uint32_t fps = std::clamp<uint32_t>(mode.fps, 1, 120);
	const uint32_t frame_bytes = FrameByteSize(mode);

	vih->rcSource = {0, 0, static_cast<LONG>(mode.width), static_cast<LONG>(mode.height)};
	vih->rcTarget = {0, 0, static_cast<LONG>(mode.width), static_cast<LONG>(mode.height)};
	vih->AvgTimePerFrame = 10000000LL / static_cast<REFERENCE_TIME>(fps);
	vih->dwBitRate = static_cast<DWORD>(
		std::min<uint64_t>(static_cast<uint64_t>(frame_bytes) * 8ULL * fps, 0xFFFFFFFFULL));
	vih->dwBitErrorRate = 0;

	vih->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	vih->bmiHeader.biWidth = static_cast<LONG>(mode.width);
	vih->bmiHeader.biHeight = static_cast<LONG>(mode.height);
	vih->bmiHeader.biPlanes = 1;
	vih->bmiHeader.biBitCount = mode.is_yuy2 ? 16 : 24;
	vih->bmiHeader.biCompression = mode.is_yuy2 ? kFourccYuy2 : BI_RGB;
	vih->bmiHeader.biSizeImage = frame_bytes;

	pmt->majortype = MEDIATYPE_Video;
	pmt->subtype = mode.is_yuy2 ? MEDIASUBTYPE_YUY2 : MEDIASUBTYPE_RGB24;
	pmt->bFixedSizeSamples = TRUE;
	pmt->bTemporalCompression = FALSE;
	pmt->lSampleSize = frame_bytes;
	pmt->formattype = FORMAT_VideoInfo;
	pmt->pUnk = nullptr;
	pmt->cbFormat = sizeof(VIDEOINFOHEADER);
	pmt->pbFormat = reinterpret_cast<BYTE *>(vih);
	return S_OK;
}

bool AwcVirtualCamPin::ParseMediaType(const AM_MEDIA_TYPE *pmt, VideoMode &out_mode)
{
	if (!pmt) return false;
	if (pmt->majortype != MEDIATYPE_Video) return false;

	bool is_yuy2 = false;
	if (pmt->subtype == MEDIASUBTYPE_RGB24) {
		is_yuy2 = false;
	} else if (pmt->subtype == MEDIASUBTYPE_YUY2) {
		is_yuy2 = true;
	} else {
		return false;
	}

	const BITMAPINFOHEADER *bmi = nullptr;
	REFERENCE_TIME avg_time = 0;

	if (pmt->formattype == FORMAT_VideoInfo && pmt->pbFormat &&
	    pmt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
		const auto *vih = reinterpret_cast<const VIDEOINFOHEADER *>(pmt->pbFormat);
		bmi = &vih->bmiHeader;
		avg_time = vih->AvgTimePerFrame;
	} else if (pmt->formattype == FORMAT_VideoInfo2 && pmt->pbFormat &&
		   pmt->cbFormat >= sizeof(VIDEOINFOHEADER2)) {
		const auto *vih2 = reinterpret_cast<const VIDEOINFOHEADER2 *>(pmt->pbFormat);
		bmi = &vih2->bmiHeader;
		avg_time = vih2->AvgTimePerFrame;
	} else {
		return false;
	}

	const LONG w = bmi->biWidth;
	const LONG h = (bmi->biHeight < 0) ? -bmi->biHeight : bmi->biHeight;
	if (w < 160 || h < 120 || w > static_cast<LONG>(kMaxFrameDim) ||
	    h > static_cast<LONG>(kMaxFrameDim) ||
	    static_cast<uint64_t>(w) * static_cast<uint64_t>(h) > kMaxFramePixels ||
	    (w & 1) != 0 || (h & 1) != 0) {
		return false;
	}

	uint32_t fps = 30;
	if (avg_time > 0) {
		const int64_t calc = (10000000LL + avg_time / 2) / avg_time;
		fps = static_cast<uint32_t>(std::clamp<int64_t>(calc, 5, 120));
	}

	out_mode.width = static_cast<uint32_t>(w);
	out_mode.height = static_cast<uint32_t>(h);
	out_mode.fps = fps;
	out_mode.is_yuy2 = is_yuy2;
	return true;
}

void AwcVirtualCamPin::FreeMediaTypeContents(AM_MEDIA_TYPE &mt)
{
	if (mt.cbFormat != 0 && mt.pbFormat) {
		CoTaskMemFree(mt.pbFormat);
		mt.cbFormat = 0;
		mt.pbFormat = nullptr;
	}
	if (mt.pUnk) {
		mt.pUnk->Release();
		mt.pUnk = nullptr;
	}
}

HRESULT AwcVirtualCamPin::CopyMediaType(AM_MEDIA_TYPE *dst, const AM_MEDIA_TYPE *src)
{
	if (!dst || !src) return E_POINTER;
	*dst = *src;
	dst->pbFormat = nullptr;
	dst->pUnk = nullptr;
	if (src->cbFormat != 0 && src->pbFormat) {
		dst->pbFormat = static_cast<BYTE *>(CoTaskMemAlloc(src->cbFormat));
		if (!dst->pbFormat) {
			dst->cbFormat = 0;
			return E_OUTOFMEMORY;
		}
		std::memcpy(dst->pbFormat, src->pbFormat, src->cbFormat);
	}
	if (src->pUnk) {
		dst->pUnk = src->pUnk;
		dst->pUnk->AddRef();
	}
	return S_OK;
}

STDMETHODIMP AwcVirtualCamPin::Connect(IPin *pReceivePin, const AM_MEDIA_TYPE *pmt)
{
	if (!pReceivePin) return E_POINTER;
	if (filter_->CurrentState() != State_Stopped) {
		return VFW_E_NOT_STOPPED;
	}

	VideoMode chosen{};
	{
		std::lock_guard<std::mutex> lock(pin_mutex_);
		if (connected_pin_) {
			return VFW_E_ALREADY_CONNECTED;
		}
		chosen = current_mode_;
	}

	if (pmt) {
		VideoMode requested{};
		if (!ParseMediaType(pmt, requested)) {
			return VFW_E_TYPE_NOT_ACCEPTED;
		}
		chosen = requested;
	}

	IMemInputPin *mem_input = nullptr;
	HRESULT hr = pReceivePin->QueryInterface(IID_IMemInputPin, reinterpret_cast<void **>(&mem_input));
	if (FAILED(hr) || !mem_input) {
		return VFW_E_NO_TRANSPORT;
	}

	// Try chosen mode first, then fallback through all supported modes if pmt was null.
	std::vector<VideoMode> candidates;
	candidates.push_back(chosen);
	if (!pmt) {
		for (const auto &m : SupportedModes()) {
			candidates.push_back(m);
		}
	}

	AM_MEDIA_TYPE agreed_mt{};
	bool accepted = false;
	for (const auto &cand : candidates) {
		AM_MEDIA_TYPE try_mt{};
		if (FAILED(BuildMediaType(cand, &try_mt))) {
			continue;
		}
		hr = pReceivePin->ReceiveConnection(this, &try_mt);
		if (SUCCEEDED(hr)) {
			agreed_mt = try_mt;
			chosen = cand;
			accepted = true;
			break;
		}
		FreeMediaTypeContents(try_mt);
	}

	if (!accepted) {
		mem_input->Release();
		return VFW_E_NO_ACCEPTABLE_TYPES;
	}

	IMemAllocator *alloc = nullptr;
	hr = mem_input->GetAllocator(&alloc);
	if (FAILED(hr) || !alloc) {
		hr = CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER,
				      IID_IMemAllocator, reinterpret_cast<void **>(&alloc));
	}
	if (FAILED(hr) || !alloc) {
		FreeMediaTypeContents(agreed_mt);
		mem_input->Release();
		return hr;
	}

	ALLOCATOR_PROPERTIES req{};
	ALLOCATOR_PROPERTIES actual{};
	req.cBuffers = 3;
	req.cbBuffer = static_cast<long>(FrameByteSize(chosen));
	req.cbAlign = 1;
	req.cbPrefix = 0;

	hr = alloc->SetProperties(&req, &actual);
	if (SUCCEEDED(hr)) {
		hr = mem_input->NotifyAllocator(alloc, FALSE);
	}
	if (FAILED(hr)) {
		alloc->Release();
		FreeMediaTypeContents(agreed_mt);
		mem_input->Release();
		return hr;
	}

	{
		std::lock_guard<std::mutex> lock(pin_mutex_);
		connected_pin_ = pReceivePin;
		connected_pin_->AddRef();
		mem_input_ = mem_input;
		allocator_ = alloc;
		current_mode_ = chosen;
		FreeMediaTypeContents(connected_mt_);
		connected_mt_ = agreed_mt;
		has_connected_mt_ = true;
	}
	return S_OK;
}

STDMETHODIMP AwcVirtualCamPin::ReceiveConnection(IPin * /*pConnector*/, const AM_MEDIA_TYPE * /*pmt*/)
{
	return E_UNEXPECTED; // Output pin
}

STDMETHODIMP AwcVirtualCamPin::Disconnect()
{
	if (filter_->CurrentState() != State_Stopped) {
		return VFW_E_NOT_STOPPED;
	}
	std::lock_guard<std::mutex> lock(pin_mutex_);
	if (!connected_pin_) {
		return S_FALSE;
	}
	if (allocator_) {
		allocator_->Decommit();
		allocator_->Release();
		allocator_ = nullptr;
	}
	if (mem_input_) {
		mem_input_->Release();
		mem_input_ = nullptr;
	}
	connected_pin_->Release();
	connected_pin_ = nullptr;
	FreeMediaTypeContents(connected_mt_);
	has_connected_mt_ = false;
	return S_OK;
}

STDMETHODIMP AwcVirtualCamPin::ConnectedTo(IPin **pPin)
{
	if (!pPin) return E_POINTER;
	std::lock_guard<std::mutex> lock(pin_mutex_);
	if (!connected_pin_) {
		*pPin = nullptr;
		return VFW_E_NOT_CONNECTED;
	}
	*pPin = connected_pin_;
	connected_pin_->AddRef();
	return S_OK;
}

STDMETHODIMP AwcVirtualCamPin::ConnectionMediaType(AM_MEDIA_TYPE *pmt)
{
	if (!pmt) return E_POINTER;
	std::lock_guard<std::mutex> lock(pin_mutex_);
	if (!connected_pin_ || !has_connected_mt_) {
		return VFW_E_NOT_CONNECTED;
	}
	return CopyMediaType(pmt, &connected_mt_);
}

STDMETHODIMP AwcVirtualCamPin::QueryPinInfo(PIN_INFO *pInfo)
{
	if (!pInfo) return E_POINTER;
	pInfo->pFilter = filter_;
	filter_->AddRef();
	pInfo->dir = PINDIR_OUTPUT;
	wcsncpy_s(pInfo->achName, MAX_PIN_NAME, kCapturePinName, _TRUNCATE);
	return S_OK;
}

STDMETHODIMP AwcVirtualCamPin::QueryDirection(PIN_DIRECTION *pPinDir)
{
	if (!pPinDir) return E_POINTER;
	*pPinDir = PINDIR_OUTPUT;
	return S_OK;
}

STDMETHODIMP AwcVirtualCamPin::QueryId(LPWSTR *Id)
{
	if (!Id) return E_POINTER;
	const size_t bytes = sizeof(kCapturePinName);
	auto *out = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
	if (!out) return E_OUTOFMEMORY;
	std::memcpy(out, kCapturePinName, bytes);
	*Id = out;
	return S_OK;
}

STDMETHODIMP AwcVirtualCamPin::QueryAccept(const AM_MEDIA_TYPE *pmt)
{
	VideoMode m{};
	return ParseMediaType(pmt, m) ? S_OK : S_FALSE;
}

STDMETHODIMP AwcVirtualCamPin::EnumMediaTypes(IEnumMediaTypes **ppEnum)
{
	if (!ppEnum) return E_POINTER;
	std::vector<VideoMode> modes;
	{
		std::lock_guard<std::mutex> lock(pin_mutex_);
		modes.push_back(current_mode_);
	}
	for (const auto &m : SupportedModes()) {
		if (m.width == modes[0].width && m.height == modes[0].height &&
		    m.fps == modes[0].fps && m.is_yuy2 == modes[0].is_yuy2) {
			continue;
		}
		modes.push_back(m);
	}
	*ppEnum = new MediaTypeEnumerator(std::move(modes), 0);
	return S_OK;
}

STDMETHODIMP AwcVirtualCamPin::QueryInternalConnections(IPin ** /*apPin*/, ULONG * /*nPin*/)
{
	return E_NOTIMPL;
}

STDMETHODIMP AwcVirtualCamPin::EndOfStream() { return E_UNEXPECTED; }
STDMETHODIMP AwcVirtualCamPin::BeginFlush() { return E_UNEXPECTED; }
STDMETHODIMP AwcVirtualCamPin::EndFlush() { return E_UNEXPECTED; }
STDMETHODIMP AwcVirtualCamPin::NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) { return S_OK; }

// IAMStreamConfig
STDMETHODIMP AwcVirtualCamPin::SetFormat(AM_MEDIA_TYPE *pmt)
{
	std::lock_guard<std::mutex> lock(pin_mutex_);
	if (connected_pin_) {
		return VFW_E_ALREADY_CONNECTED;
	}
	if (!pmt) {
		current_mode_ = VideoMode{};
		return S_OK;
	}
	VideoMode mode{};
	if (!ParseMediaType(pmt, mode)) {
		return VFW_E_INVALIDMEDIATYPE;
	}
	current_mode_ = mode;
	return S_OK;
}

STDMETHODIMP AwcVirtualCamPin::GetFormat(AM_MEDIA_TYPE **ppmt)
{
	if (!ppmt) return E_POINTER;
	VideoMode mode{};
	{
		std::lock_guard<std::mutex> lock(pin_mutex_);
		mode = current_mode_;
	}
	auto *mt = static_cast<AM_MEDIA_TYPE *>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
	if (!mt) return E_OUTOFMEMORY;
	const HRESULT hr = BuildMediaType(mode, mt);
	if (FAILED(hr)) {
		CoTaskMemFree(mt);
		return hr;
	}
	*ppmt = mt;
	return S_OK;
}

STDMETHODIMP AwcVirtualCamPin::GetNumberOfCapabilities(int *piCount, int *piSize)
{
	if (!piCount || !piSize) return E_POINTER;
	*piCount = static_cast<int>(SupportedModes().size());
	*piSize = static_cast<int>(sizeof(VIDEO_STREAM_CONFIG_CAPS));
	return S_OK;
}

STDMETHODIMP AwcVirtualCamPin::GetStreamCaps(int iIndex, AM_MEDIA_TYPE **ppmt, BYTE *pSCC)
{
	if (!ppmt || !pSCC) return E_POINTER;
	const auto modes = SupportedModes();
	if (iIndex < 0 || static_cast<size_t>(iIndex) >= modes.size()) {
		return S_FALSE;
	}
	const VideoMode &m = modes[static_cast<size_t>(iIndex)];

	auto *mt = static_cast<AM_MEDIA_TYPE *>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
	if (!mt) return E_OUTOFMEMORY;
	const HRESULT hr = BuildMediaType(m, mt);
	if (FAILED(hr)) {
		CoTaskMemFree(mt);
		return hr;
	}
	*ppmt = mt;

	auto *caps = reinterpret_cast<VIDEO_STREAM_CONFIG_CAPS *>(pSCC);
	std::memset(caps, 0, sizeof(VIDEO_STREAM_CONFIG_CAPS));
	caps->guid = FORMAT_VideoInfo;
	caps->VideoStandard = 0;
	caps->InputSize = {static_cast<LONG>(m.width), static_cast<LONG>(m.height)};
	caps->MinCroppingSize = caps->InputSize;
	caps->MaxCroppingSize = caps->InputSize;
	caps->CropGranularityX = 2;
	caps->CropGranularityY = 2;
	caps->CropAlignX = 0;
	caps->CropAlignY = 0;
	caps->MinOutputSize = caps->InputSize;
	caps->MaxOutputSize = caps->InputSize;
	caps->OutputGranularityX = 2;
	caps->OutputGranularityY = 2;
	caps->StretchTapsX = 0;
	caps->StretchTapsY = 0;
	caps->ShrinkTapsX = 0;
	caps->ShrinkTapsY = 0;
	caps->MinFrameInterval = 10000000LL / 60;
	caps->MaxFrameInterval = 10000000LL / 15;
	const uint64_t frame_bytes = FrameByteSize(m);
	caps->MinBitsPerSecond = static_cast<LONG>(std::min<uint64_t>(frame_bytes * 8ULL * 15ULL, 0x7FFFFFFFULL));
	caps->MaxBitsPerSecond = static_cast<LONG>(std::min<uint64_t>(frame_bytes * 8ULL * 60ULL, 0x7FFFFFFFULL));
	return S_OK;
}

// IKsPropertySet
STDMETHODIMP AwcVirtualCamPin::Set(REFGUID, DWORD, LPVOID, DWORD, LPVOID, DWORD)
{
	return E_NOTIMPL;
}

STDMETHODIMP AwcVirtualCamPin::Get(REFGUID guidPropSet, DWORD dwPropID, LPVOID /*pInstanceData*/,
				   DWORD /*cbInstanceData*/, LPVOID pPropData, DWORD cbPropData,
				   DWORD *pcbReturned)
{
	if (guidPropSet != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
	if (dwPropID != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
	if (!pPropData && !pcbReturned) return E_POINTER;
	if (pcbReturned) *pcbReturned = sizeof(GUID);
	if (!pPropData) return S_OK;
	if (cbPropData < sizeof(GUID)) return E_UNEXPECTED;
	*static_cast<GUID *>(pPropData) = PIN_CATEGORY_CAPTURE;
	return S_OK;
}

STDMETHODIMP AwcVirtualCamPin::QuerySupported(REFGUID guidPropSet, DWORD dwPropID,
					      DWORD *pTypeSupport)
{
	if (guidPropSet != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
	if (dwPropID != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
	if (pTypeSupport) *pTypeSupport = KSPROPERTY_SUPPORT_GET;
	return S_OK;
}

// IQualityControl
STDMETHODIMP AwcVirtualCamPin::Notify(IBaseFilter *, Quality) { return S_OK; }
STDMETHODIMP AwcVirtualCamPin::SetSink(IQualityControl *) { return S_OK; }

// Lifecycle
HRESULT AwcVirtualCamPin::OnPause()
{
	std::lock_guard<std::mutex> lock(pin_mutex_);
	if (allocator_) {
		return allocator_->Commit();
	}
	return S_OK;
}

HRESULT AwcVirtualCamPin::OnRun()
{
	{
		std::lock_guard<std::mutex> lock(pin_mutex_);
		if (!connected_pin_ || !mem_input_ || !allocator_) {
			return S_OK;
		}
		allocator_->Commit();
	}
	if (!streaming_.exchange(true)) {
		if (stream_thread_.joinable()) {
			stream_thread_.join();
		}
		stream_thread_ = std::thread(&AwcVirtualCamPin::StreamLoop, this);
	}
	return S_OK;
}

HRESULT AwcVirtualCamPin::OnStop()
{
	streaming_.store(false);
	{
		std::lock_guard<std::mutex> lock(pin_mutex_);
		if (allocator_) {
			allocator_->Decommit();
		}
	}
	if (stream_thread_.joinable()) {
		stream_thread_.join();
	}
	return S_OK;
}

void AwcVirtualCamPin::StreamLoop()
{
	timeBeginPeriod(1);

	IMemInputPin *mem_input = nullptr;
	IMemAllocator *allocator = nullptr;
	VideoMode mode{};
	{
		std::lock_guard<std::mutex> lock(pin_mutex_);
		mem_input = mem_input_;
		allocator = allocator_;
		mode = current_mode_;
		if (mem_input) mem_input->AddRef();
		if (allocator) allocator->AddRef();
	}

	if (!mem_input || !allocator) {
		if (mem_input) mem_input->Release();
		if (allocator) allocator->Release();
		timeEndPeriod(1);
		return;
	}

	const uint32_t fps = std::clamp<uint32_t>(mode.fps, 5, 120);
	const REFERENCE_TIME frame_duration_100ns = 10000000LL / static_cast<REFERENCE_TIME>(fps);
	const uint32_t frame_bytes = FrameByteSize(mode);
	const uint32_t rgb_stride = CalcRgb24Stride(mode.width);
	const uint32_t yuy2_stride = mode.width * 2u;

	VcamShmReader shm;
	I420FrameSnapshot snapshot;
	std::vector<uint8_t> cached_converted(frame_bytes);
	uint64_t cached_seq = 0;
	uint64_t frame_index = 0;
	auto start_tp = std::chrono::steady_clock::now();

	while (streaming_.load()) {
		if (!shm.IsOpen()) {
			shm.TryOpen(kDefaultShmName, kDefaultEventName);
		}

		IMediaSample *sample = nullptr;
		HRESULT hr = allocator->GetBuffer(&sample, nullptr, nullptr, 0);
		if (FAILED(hr) || !sample) {
			break;
		}

		BYTE *buf = nullptr;
		const long buf_size = sample->GetSize();
		if (SUCCEEDED(sample->GetPointer(&buf)) && buf &&
		    buf_size >= static_cast<long>(frame_bytes)) {
			bool frame_updated = false;
			const bool has_live = shm.IsOpen() && shm.ReadFrameIfUpdated(snapshot, frame_updated);
			if (has_live && !snapshot.i420.empty()) {
				if (frame_updated || snapshot.seq != cached_seq) {
					if (mode.is_yuy2) {
						ConvertI420ToYuy2(snapshot.i420.data(), snapshot.width, snapshot.height,
								  cached_converted.data(), mode.width, mode.height,
								  yuy2_stride);
					} else {
						ConvertI420ToRgb24(snapshot.i420.data(), snapshot.width, snapshot.height,
								   snapshot.full_range, cached_converted.data(),
								   mode.width, mode.height, rgb_stride, true);
					}
					cached_seq = snapshot.seq;
				}
				std::memcpy(buf, cached_converted.data(), frame_bytes);
			} else {
				cached_seq = 0;
				if (mode.is_yuy2) {
					RenderStandbyYuy2(buf, mode.width, mode.height, yuy2_stride, frame_index);
				} else {
					RenderStandbyRgb24(buf, mode.width, mode.height, rgb_stride, frame_index, true);
				}
			}

			REFERENCE_TIME start_time = static_cast<REFERENCE_TIME>(frame_index) * frame_duration_100ns;
			REFERENCE_TIME end_time = start_time + frame_duration_100ns;
			sample->SetTime(&start_time, &end_time);
			sample->SetSyncPoint(TRUE);
			sample->SetDiscontinuity(frame_index == 0 ? TRUE : FALSE);
			sample->SetActualDataLength(static_cast<long>(frame_bytes));

			hr = mem_input->Receive(sample);
			sample->Release();
			if (hr != S_OK) {
				break;
			}
			++frame_index;
		} else {
			sample->Release();
		}

		const auto next_deadline =
			start_tp + std::chrono::nanoseconds(
					   (static_cast<int64_t>(frame_index) * 1000000000LL) /
					   static_cast<int64_t>(fps));
		auto now = std::chrono::steady_clock::now();
		if (now < next_deadline) {
			const auto rem_us =
				std::chrono::duration_cast<std::chrono::microseconds>(next_deadline - now).count();
			if (rem_us > 2000) {
				Sleep(static_cast<DWORD>((rem_us - 1000) / 1000));
			}
			while (streaming_.load() && std::chrono::steady_clock::now() < next_deadline) {
				SwitchToThread();
			}
		} else if (now - next_deadline > std::chrono::milliseconds(100)) {
			start_tp = now - std::chrono::nanoseconds(
						 (static_cast<int64_t>(frame_index) * 1000000000LL) /
						 static_cast<int64_t>(fps));
		}
	}

	mem_input->Release();
	allocator->Release();
	timeEndPeriod(1);
}

} // namespace awc::vcam

#endif // _WIN32
