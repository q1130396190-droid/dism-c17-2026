#include "StdAfx.h"
#include "DPI.h"
#include "VersionHelpers.h"
namespace DuiLib
{
	//96 DPI = 100% scaling
	//120 DPI = 125% scaling
	//144 DPI = 150% scaling
	//168 DPI = 175% scaling
	//192 DPI = 200% scaling

	using LPSetProcessDpiAwareness = HRESULT(WINAPI*)(
		_In_ PROCESS_DPI_AWARENESS value
		);

	using LPGetProcessDpiAwareness = HRESULT(WINAPI*)(
		_In_  HANDLE                hprocess,
		_Out_ PROCESS_DPI_AWARENESS *value
		);


	using LPGetDpiForMonitor = HRESULT(WINAPI*)(
		_In_  HMONITOR         hmonitor,
		_In_  MONITOR_DPI_TYPE dpiType,
		_Out_ UINT             *dpiX,
		_Out_ UINT             *dpiY
		);


	CDPI::CDPI()
	{
		m_nScaleFactor = 0;
		m_nScaleFactorSDA = 0;
		m_Awareness = PROCESS_PER_MONITOR_DPI_AWARE;

		SetScale(96);

	}

	int CDPI::GetDPIOfMonitor(HMONITOR hMonitor)
	{
		UINT dpix = 96, dpiy = 96;
		if (IsWindows8Point1OrGreater()) {
			HRESULT  hr = E_FAIL;
			HMODULE hModule =::LoadLibrary(_T("Shcore.dll"));
			if(hModule != nullptr) {
				LPGetDpiForMonitor GetDpiForMonitor = reinterpret_cast<LPGetDpiForMonitor>(GetProcAddress(hModule, "GetDpiForMonitor"));
				if (GetDpiForMonitor != nullptr && GetDpiForMonitor(hMonitor,MDT_EFFECTIVE_DPI, &dpix, &dpiy) != S_OK) {
					// [SEC-22] A modal box here blocks headless/service processes
					// forever; falling back to 96 DPI is already the correct
					// behaviour, so only trace the condition.
					::OutputDebugStringA("[DuiLib] GetDpiForMonitor failed, falling back to 96 DPI\n");
					return 96;
				}
			}
		}
		else {
			HDC screen = GetDC(0);
			dpix = GetDeviceCaps(screen, LOGPIXELSX);
			ReleaseDC(0, screen);
		}
		return dpix;
	}

	int CDPI::GetDPIOfMonitorNearestToPoint(POINT pt)
	{
		HMONITOR hMonitor;
		hMonitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
		return GetDPIOfMonitor(hMonitor);
	}

	int CDPI::GetMainMonitorDPI()
	{
		POINT    pt;
		// Get the DPI for the main monitor
		pt.x = 1;
		pt.y = 1;
		return GetDPIOfMonitorNearestToPoint(pt);
	}

	PROCESS_DPI_AWARENESS CDPI::GetDPIAwareness()
	{
		if (IsWindows8Point1OrGreater()) {
			HMODULE hModule =::LoadLibrary(_T("Shcore.dll"));
			if(hModule != nullptr) {
				LPGetProcessDpiAwareness GetProcessDpiAwareness = reinterpret_cast<LPGetProcessDpiAwareness>(GetProcAddress(hModule, "GetProcessDpiAwareness"));
				if(GetProcessDpiAwareness != nullptr) {
					HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, false, GetCurrentProcessId());
					if(GetProcessDpiAwareness(hProcess, &m_Awareness) == S_OK) {
					}
				}
			}
		}

		return m_Awareness;
	}

	BOOL CDPI::SetDPIAwareness(PROCESS_DPI_AWARENESS Awareness)
	{
		BOOL bRet = FALSE;
		if (IsWindows8Point1OrGreater()) {
			HMODULE hModule =::LoadLibrary(_T("Shcore.dll"));
			if(hModule != nullptr) {
				LPSetProcessDpiAwareness SetProcessDpiAwareness = reinterpret_cast<LPSetProcessDpiAwareness>(GetProcAddress(hModule, "SetProcessDpiAwareness"));
				if (SetProcessDpiAwareness != nullptr && SetProcessDpiAwareness(Awareness) == S_OK) {
					m_Awareness = Awareness;
					bRet = TRUE;
				}
			}
		}
		else {
			m_Awareness = Awareness;
		}
		return bRet;
	}

	UINT DuiLib::CDPI::GetDPI()
	{
		if (m_Awareness == PROCESS_DPI_UNAWARE) {
			return 96;
		}

		if (m_Awareness == PROCESS_SYSTEM_DPI_AWARE) {
			return MulDiv(m_nScaleFactorSDA, 96, 100);
		}

		return MulDiv(m_nScaleFactor, 96, 100);
	}

	UINT CDPI::GetScale()
	{
		if (m_Awareness == PROCESS_DPI_UNAWARE) {
			return 100;
		}
		if (m_Awareness == PROCESS_SYSTEM_DPI_AWARE) {
			return m_nScaleFactorSDA;
		}
		return m_nScaleFactor;
	}


	// [H4] C++17 defensive: SetScale(0) would compute MulDiv(0,100,96)=0 and break DPI math.
	void CDPI::SetScale(UINT uDPI)
	{
		if (uDPI == 0) {
			DUI_LOGW("CDPI::SetScale(0) -> fallback 96");
			uDPI = 96;
		}
		m_nScaleFactor = MulDiv(uDPI, 100, 96);
		if (m_nScaleFactorSDA == 0) {
			m_nScaleFactorSDA = m_nScaleFactor;
		}
	}

	int  CDPI::Scale(int iValue)
	{
		int iResult = iValue;
		if (m_Awareness == PROCESS_DPI_UNAWARE) {
			return iValue;
		}
		else if (m_Awareness == PROCESS_SYSTEM_DPI_AWARE) {
			iResult = MulDiv(iValue, m_nScaleFactorSDA, 100);
		}
		else{
			iResult = MulDiv(iValue, m_nScaleFactor, 100);
		}
		if(iValue > 0 && iResult == 0) iResult = 1;
		if(iValue < 0 && iResult == 0) iResult = -1;
		return iResult;
	}

	// [H4] C++17 defensive: guard ScaleBack divide-by-zero on SDA/per-monitor scale factors.
	int  CDPI::ScaleBack(int iValue) {

		if (m_Awareness == PROCESS_DPI_UNAWARE) {
			return iValue;
		}
		if (m_Awareness == PROCESS_SYSTEM_DPI_AWARE) {
			if (m_nScaleFactorSDA <= 0) { DUI_LOGW("CDPI::ScaleBack SDA scale<=0"); return iValue; }
			return MulDiv(iValue,  100, m_nScaleFactorSDA);
		}
		if (m_nScaleFactor <= 0) { DUI_LOGW("CDPI::ScaleBack scale<=0"); return iValue; }
		return MulDiv(iValue, 100, m_nScaleFactor);
	}

	RECT CDPI::Scale(RECT rcRect)
	{
		RECT rcScale = rcRect;
		int sw = Scale(rcRect.right - rcRect.left);
		int sh = Scale(rcRect.bottom - rcRect.top);
		rcScale.left = Scale(rcRect.left);
		rcScale.top = Scale(rcRect.top);
		rcScale.right = rcScale.left + sw;
		rcScale.bottom = rcScale.top + sh;
		return rcScale;
	}

	void CDPI::Scale(RECT *pRect)
	{
		int sw = Scale(pRect->right - pRect->left);
		int sh = Scale(pRect->bottom - pRect->top);
		pRect->left = Scale(pRect->left);
		pRect->top = Scale(pRect->top);
		pRect->right = pRect->left + sw;
		pRect->bottom = pRect->top + sh;
	}

	void CDPI::ScaleBack(RECT *pRect)
	{
		int sw = ScaleBack(pRect->right - pRect->left);
		int sh = ScaleBack(pRect->bottom - pRect->top);
		pRect->left = ScaleBack(pRect->left);
		pRect->top = ScaleBack(pRect->top);
		pRect->right = pRect->left + sw;
		pRect->bottom = pRect->top + sh;
	}

	void CDPI::Scale(POINT *pPoint)
	{
		pPoint->x = Scale(pPoint->x);
		pPoint->y = Scale(pPoint->y);
	}

	POINT CDPI::Scale(POINT ptPoint)
	{
		POINT ptScale = ptPoint;
		ptScale.x = Scale(ptPoint.x);
		ptScale.y = Scale(ptPoint.y);
		return ptScale;
	}

	void CDPI::Scale(SIZE *pSize)
	{
		pSize->cx = Scale(pSize->cx);
		pSize->cy = Scale(pSize->cy);
	}

	SIZE CDPI::Scale(SIZE szSize)
	{
		SIZE szScale = szSize;
		szScale.cx = Scale(szSize.cx);
		szScale.cy = Scale(szSize.cy);
		return szScale;
	}

	// [WIN11-02] Per-Monitor V2 DPI awareness with graceful fallback.
	//
	// user32!SetProcessDpiAwarenessContext exists from Win10 1703. Resolved at
	// runtime so the library keeps loading on older systems and older SDKs.
	int CDPI::SetPerMonitorV2Awareness()
	{
		typedef BOOL(WINAPI* PFN_SetProcessDpiAwarenessContext)(HANDLE);
		static PFN_SetProcessDpiAwarenessContext s_pfn = []() -> PFN_SetProcessDpiAwarenessContext
		{
			HMODULE hUser32 = ::GetModuleHandleW(L"user32.dll");
			if (hUser32 == nullptr) hUser32 = ::LoadLibraryW(L"user32.dll");
			if (hUser32 == nullptr) return nullptr;
			return reinterpret_cast<PFN_SetProcessDpiAwarenessContext>(
				::GetProcAddress(hUser32, "SetProcessDpiAwarenessContext"));
		}();

		// DPI_AWARENESS_CONTEXT values are pseudo-handles; declared locally to
		// avoid depending on the SDK's dpi-awareness header being new enough.
		auto context = [](INT_PTR v) { return reinterpret_cast<HANDLE>(v); };
		const INT_PTR kPerMonitorV2 = -4;
		const INT_PTR kPerMonitor = -3;
		const INT_PTR kSystemAware = -2;
		const INT_PTR kUnaware = -1;

		if (s_pfn != nullptr)
		{
			if (s_pfn(context(kPerMonitorV2)) != FALSE) return 3;
			if (s_pfn(context(kPerMonitor)) != FALSE) return 2;
			if (s_pfn(context(kSystemAware)) != FALSE) return 1;
			if (s_pfn(context(kUnaware)) != FALSE) return 0;
		}

		// Fallback for Windows 8.1 - 10 1607: Shcore!SetProcessDpiAwareness.
		typedef HRESULT(WINAPI* PFN_SetProcessDpiAwareness)(int);
		HMODULE hShcore = ::LoadLibraryW(L"Shcore.dll");
		if (hShcore != nullptr)
		{
			auto pfn = reinterpret_cast<PFN_SetProcessDpiAwareness>(
				::GetProcAddress(hShcore, "SetProcessDpiAwareness"));
			if (pfn != nullptr)
			{
				// 2 = PROCESS_PER_MONITOR_DPI_AWARE, 1 = SYSTEM_DPI_AWARE, 0 = UNAWARE
				if (SUCCEEDED(pfn(2))) return 2;
				if (SUCCEEDED(pfn(1))) return 1;
				if (SUCCEEDED(pfn(0))) return 0;
			}
			::FreeLibrary(hShcore);
		}

		return -1; // nothing applied (pre-8.1); caller keeps its own default
	}
}