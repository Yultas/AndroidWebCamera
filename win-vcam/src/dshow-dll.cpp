#ifdef _WIN32

#include "dshow-filter.hpp"

#include <objbase.h>
#include <string>

namespace {

HMODULE g_module = nullptr;

class AwcClassFactory final : public IClassFactory {
public:
	AwcClassFactory()
	{
		awc::vcam::g_dll_ref_count.fetch_add(1, std::memory_order_relaxed);
	}

	~AwcClassFactory()
	{
		awc::vcam::g_dll_ref_count.fetch_sub(1, std::memory_order_relaxed);
	}

	STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
	{
		if (!ppv) return E_POINTER;
		if (riid == IID_IUnknown || riid == IID_IClassFactory) {
			*ppv = static_cast<IClassFactory *>(this);
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

	STDMETHODIMP CreateInstance(IUnknown *pUnkOuter, REFIID riid, void **ppv) override
	{
		if (!ppv) return E_POINTER;
		*ppv = nullptr;
		if (pUnkOuter) return CLASS_E_NOAGGREGATION;

		auto *filter = new awc::vcam::AwcVirtualCamFilter();
		const HRESULT hr = filter->QueryInterface(riid, ppv);
		filter->Release();
		return hr;
	}

	STDMETHODIMP LockServer(BOOL fLock) override
	{
		if (fLock) {
			awc::vcam::g_dll_ref_count.fetch_add(1, std::memory_order_relaxed);
		} else {
			awc::vcam::g_dll_ref_count.fetch_sub(1, std::memory_order_relaxed);
		}
		return S_OK;
	}

private:
	std::atomic<long> ref_count_{1};
};

std::wstring GuidToString(REFGUID guid)
{
	wchar_t buf[64] = {};
	StringFromGUID2(guid, buf, 64);
	return std::wstring(buf);
}

HRESULT RegisterClsidInRegistry()
{
	wchar_t dll_path[MAX_PATH] = {};
	const DWORD len = GetModuleFileNameW(g_module, dll_path, MAX_PATH);
	if (len == 0 || len >= MAX_PATH) {
		return HRESULT_FROM_WIN32(GetLastError());
	}

	const std::wstring clsid_key = L"CLSID\\" + GuidToString(awc::vcam::CLSID_AndroidWebCamera);
	const std::wstring inproc_key = clsid_key + L"\\InprocServer32";

	HKEY hkey = nullptr;
	LONG rc = RegCreateKeyExW(HKEY_CLASSES_ROOT, clsid_key.c_str(), 0, nullptr,
				  REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hkey, nullptr);
	if (rc != ERROR_SUCCESS) {
		return HRESULT_FROM_WIN32(rc);
	}
	rc = RegSetValueExW(hkey, nullptr, 0, REG_SZ,
			    reinterpret_cast<const BYTE *>(awc::vcam::kFilterFriendlyName),
			    static_cast<DWORD>(sizeof(awc::vcam::kFilterFriendlyName)));
	RegCloseKey(hkey);
	if (rc != ERROR_SUCCESS) {
		return HRESULT_FROM_WIN32(rc);
	}

	rc = RegCreateKeyExW(HKEY_CLASSES_ROOT, inproc_key.c_str(), 0, nullptr,
			     REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hkey, nullptr);
	if (rc != ERROR_SUCCESS) {
		return HRESULT_FROM_WIN32(rc);
	}
	const DWORD path_bytes = static_cast<DWORD>((wcslen(dll_path) + 1) * sizeof(wchar_t));
	rc = RegSetValueExW(hkey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE *>(dll_path), path_bytes);
	if (rc == ERROR_SUCCESS) {
		const wchar_t threading_model[] = L"Both";
		rc = RegSetValueExW(hkey, L"ThreadingModel", 0, REG_SZ,
				    reinterpret_cast<const BYTE *>(threading_model),
				    static_cast<DWORD>(sizeof(threading_model)));
	}
	RegCloseKey(hkey);
	return (rc == ERROR_SUCCESS) ? S_OK : HRESULT_FROM_WIN32(rc);
}

void UnregisterClsidFromRegistry()
{
	const std::wstring clsid_key = L"CLSID\\" + GuidToString(awc::vcam::CLSID_AndroidWebCamera);
	RegDeleteTreeW(HKEY_CLASSES_ROOT, clsid_key.c_str());
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID /*lpvReserved*/)
{
	if (fdwReason == DLL_PROCESS_ATTACH) {
		g_module = hinstDLL;
		DisableThreadLibraryCalls(hinstDLL);
	}
	return TRUE;
}

STDAPI DllCanUnloadNow(void)
{
	return (awc::vcam::g_dll_ref_count.load(std::memory_order_acquire) == 0) ? S_OK : S_FALSE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppv)
{
	if (!ppv) return E_POINTER;
	*ppv = nullptr;
	if (rclsid != awc::vcam::CLSID_AndroidWebCamera) {
		return CLASS_E_CLASSNOTAVAILABLE;
	}
	auto *factory = new AwcClassFactory();
	const HRESULT hr = factory->QueryInterface(riid, ppv);
	factory->Release();
	return hr;
}

STDAPI DllRegisterServer(void)
{
	HRESULT hr = RegisterClsidInRegistry();
	if (FAILED(hr)) {
		return hr;
	}

	const HRESULT co_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	const bool did_init = SUCCEEDED(co_hr);

	IFilterMapper2 *mapper = nullptr;
	hr = CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER,
			      IID_IFilterMapper2, reinterpret_cast<void **>(&mapper));
	if (SUCCEEDED(hr) && mapper) {
		static const REGPINTYPES kPinTypes[2] = {
			{&MEDIATYPE_Video, &MEDIASUBTYPE_RGB24},
			{&MEDIATYPE_Video, &MEDIASUBTYPE_YUY2},
		};
		static const REGFILTERPINS2 kPinReg = {
			REG_PINFLAG_B_OUTPUT,
			1,
			2,
			kPinTypes,
			0,
			nullptr,
			&PIN_CATEGORY_CAPTURE,
		};
		REGFILTER2 rf2{};
		rf2.dwVersion = 2;
		rf2.dwMerit = MERIT_DO_NOT_USE;
		rf2.cPins2 = 1;
		rf2.rgPins2 = &kPinReg;

		IMoniker *moniker = nullptr;
		hr = mapper->RegisterFilter(awc::vcam::CLSID_AndroidWebCamera,
					    awc::vcam::kFilterFriendlyName,
					    &moniker,
					    &CLSID_VideoInputDeviceCategory,
					    nullptr,
					    &rf2);
		if (moniker) {
			moniker->Release();
		}
		mapper->Release();
	}

	if (did_init) {
		CoUninitialize();
	}
	return hr;
}

STDAPI DllUnregisterServer(void)
{
	const HRESULT co_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	const bool did_init = SUCCEEDED(co_hr);

	IFilterMapper2 *mapper = nullptr;
	HRESULT hr = CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER,
				      IID_IFilterMapper2, reinterpret_cast<void **>(&mapper));
	if (SUCCEEDED(hr) && mapper) {
		mapper->UnregisterFilter(&CLSID_VideoInputDeviceCategory, nullptr,
					 awc::vcam::CLSID_AndroidWebCamera);
		mapper->Release();
	}

	UnregisterClsidFromRegistry();

	if (did_init) {
		CoUninitialize();
	}
	return S_OK;
}

#endif // _WIN32
