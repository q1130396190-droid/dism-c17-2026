#include "StdAfx.h"
#include "Core/PerfProbe.h"
#include "Core/ThreadAffinity.h"
#include "S3AttrRegistry.h"
#include "AttrParse.h"   // [round517] attr::ParseRect4/ParseSize2 used below (was missing -> C2039)
#include <zmouse.h>
#include <algorithm> // std::min/std::max
#include <memory> // [R4] std::unique_ptr
#include <map>    // [PERF-12] native child surface cache
#include <mutex>  // [SEC-21] shared resource init
#include <cstring>  // [MIG] memcpy for the owned copies of unknown attribute names
#include <cstdlib>  // [MIG] malloc/free for the same


// [T5.36] W4 batch-4: suppress variable-hiding warnings (C4456/C4457/C4458).
// DuiLib conventionally uses local variables that shadow outer-scope / member
// names (e.g. m_rcTextPadding shadow, LPTSTR pstr = nullptr redeclarations in
// switch cases). All shadowing variables are properly initialized at declaration
// and used within their own scope. Not UB; documented in
// docs/t535-w4-b4-uninit-audit.json. Modernization path: rename to m_local_*
// prefix or restructure scopes.
#pragma warning(disable: 4456 4457 4458)
// [T5.36] end


#pragma warning(push)
#pragma warning(disable:4838 4244)


namespace DuiLib {
	// [SEC-20] implemented in UIControl.cpp (live paint-manager registry)
	void DuiLibRegisterManager(const void* p);
	void DuiLibUnregisterManager(const void* p);

	// [R-07] The ONE definition of thread-affinity state in the process.
	//
	// This must live in a .cpp, not in ThreadAffinity.h: a header-defined
	// variable is duplicated per module, so a DLL consumer would read its own
	// empty copy. Measured before this fix -- the probe saw "unclaimed", ran the
	// worker's task inline on the worker thread, and reported that the UI thread
	// never executed it.
	namespace {
		// Function-local static: no static-init-order dependency, and the block is
		// zero-initialised (SRWLOCK zero == SRWLOCK_INIT) before any call reaches it.
		TThreadAffinityState* AffinityState()
		{
			static TThreadAffinityState s_state = {};
			return &s_state;
		}
		int FindOwnerSlot(TThreadAffinityState* p, const CPaintManagerUI* pManager)
		{
			for (int i = 0; i < TThreadAffinityState::kMaxManagers; ++i) {
				if (p->owner[i].pManager == pManager) return i;
			}
			return -1;
		}
	}

	UILIB_API TThreadAffinityState* DuiLibThreadAffinityState()
	{
		return AffinityState();
	}

	bool CThreadAffinity::RegisterUiThread(const CPaintManagerUI* pManager, DWORD dwThreadId)
	{
		TThreadAffinityState* p = AffinityState();
		if (pManager == nullptr) { ClaimUiThread(dwThreadId); return true; }
		::AcquireSRWLockExclusive(&p->lock);
		bool bOk = true;
		int i = FindOwnerSlot(p, pManager);
		if (i < 0) {
			for (int k = 0; k < TThreadAffinityState::kMaxManagers; ++k) {
				if (p->owner[k].pManager == nullptr) { i = k; break; }
			}
			if (i >= 0) { p->owner[i].pManager = pManager; p->owner[i].dwThreadId = 0; }
		}
		if (i >= 0) {
			if (p->owner[i].dwThreadId == 0) p->owner[i].dwThreadId = dwThreadId;
			else if (p->owner[i].dwThreadId != dwThreadId) bOk = false;  // keep first owner
		}
		::ReleaseSRWLockExclusive(&p->lock);
		return bOk;
	}

	void CThreadAffinity::UnregisterUiThread(const CPaintManagerUI* pManager)
	{
		if (pManager == nullptr) return;
		TThreadAffinityState* p = AffinityState();
		::AcquireSRWLockExclusive(&p->lock);
		const int i = FindOwnerSlot(p, pManager);
		if (i >= 0) { p->owner[i].pManager = nullptr; p->owner[i].dwThreadId = 0; }
		::ReleaseSRWLockExclusive(&p->lock);
	}

	DWORD CThreadAffinity::OwnerOf(const CPaintManagerUI* pManager)
	{
		TThreadAffinityState* p = AffinityState();
		if (pManager == nullptr) return p->dwUiThread;
		::AcquireSRWLockShared(&p->lock);
		const int i = FindOwnerSlot(p, pManager);
		const DWORD dw = (i >= 0) ? p->owner[i].dwThreadId : 0;
		::ReleaseSRWLockShared(&p->lock);
		return dw;
	}
	// [R-07] Defined in the anonymous namespace far below but called from the
	// destructor above it. Without this forward declaration the name is not
	// visible at the call site and the build fails with C3861.
	namespace { void DiscardTaskQueue(const CPaintManagerUI* p); }

	/////////////////////////////////////////////////////////////////////////////////////
	//
	//

	static void GetChildWndRect(HWND hWnd, HWND hChildWnd, RECT& rcChildWnd)
	{
		::GetWindowRect(hChildWnd, &rcChildWnd);

		POINT pt;
		pt.x = rcChildWnd.left;
		pt.y = rcChildWnd.top;
		::ScreenToClient(hWnd, &pt);
		rcChildWnd.left = pt.x;
		rcChildWnd.top = pt.y;

		pt.x = rcChildWnd.right;
		pt.y = rcChildWnd.bottom;
		::ScreenToClient(hWnd, &pt);
		rcChildWnd.right = pt.x;
		rcChildWnd.bottom = pt.y;
	}

	static UINT MapKeyState()
	{
		UINT uState = 0;
		if( ::GetKeyState(VK_CONTROL) < 0 ) uState |= MK_CONTROL;
		if( ::GetKeyState(VK_LBUTTON) < 0 ) uState |= MK_LBUTTON;
		if( ::GetKeyState(VK_RBUTTON) < 0 ) uState |= MK_RBUTTON;
		if( ::GetKeyState(VK_SHIFT) < 0 ) uState |= MK_SHIFT;
		if( ::GetKeyState(VK_MENU) < 0 ) uState |= MK_ALT;
		return uState;
	}

	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	///
	struct FINDTABINFO
	{
		CControlUI* pFocus;
		CControlUI* pLast;
		bool bForward;
		bool bNextIsIt;
	};

	struct FINDSHORTCUT
	{
		TCHAR ch;
		bool bPickNext;
	};

	struct TIMERINFO
	{
		CControlUI* pSender;
		UINT nLocalID;
		HWND hWnd;
		UINT uWinTimer;
		bool bKilled;
	};


	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	///
	TImageInfo::TImageInfo()
	{
		pImage = nullptr;
		hBitmap = nullptr;
	}

	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	///
	TDrawInfo::TDrawInfo()
	{
		Clear();
	}

	void TDrawInfo::Parse(LPCTSTR pStrImage, LPCTSTR pStrModify,CPaintManagerUI *pManager)
	{
		// 1、aaa.jpg
		// 2、file='aaa.jpg' res='' restype='0' dest='0,0,0,0' source='0,0,0,0' corner='0,0,0,0' 
		// mask='#FF0000' fade='255' hole='false' xtiled='false' ytiled='false'
		sDrawString = pStrImage;
		sDrawModify = pStrModify;
		sImageName = pStrImage;

		CDuiString sItem;
		CDuiString sValue;
		LPTSTR pstr = nullptr;
		for( int i = 0; i < 2; ++i ) {
			if( i == 1) pStrImage = pStrModify;
			if( !pStrImage ) continue;
			while( *pStrImage != _T('\0') ) {
				sItem.Empty();
				sValue.Empty();
				while( *pStrImage > _T('\0') && *pStrImage <= _T(' ') ) pStrImage = ::CharNext(pStrImage);
				while( *pStrImage != _T('\0') && *pStrImage != _T('=') && *pStrImage > _T(' ') ) {
					LPTSTR pstrTemp = ::CharNext(pStrImage);
					while( pStrImage < pstrTemp) {
						sItem += *pStrImage++;
					}
				}
				while( *pStrImage > _T('\0') && *pStrImage <= _T(' ') ) pStrImage = ::CharNext(pStrImage);
				if( *pStrImage++ != _T('=') ) break;
				while( *pStrImage > _T('\0') && *pStrImage <= _T(' ') ) pStrImage = ::CharNext(pStrImage);
				if( *pStrImage++ != _T('\'') ) break;
				while( *pStrImage != _T('\0') && *pStrImage != _T('\'') ) {
					LPTSTR pstrTemp = ::CharNext(pStrImage);
					while( pStrImage < pstrTemp) {
						sValue += *pStrImage++;
					}
				}
				if( *pStrImage++ != _T('\'') ) break;
				if( !sValue.IsEmpty() ) {
					if( sItem == _T("file") || sItem == _T("res") ) {
						sImageName = sValue;
					}
					else if( sItem == _T("restype") ) {
						sResType = sValue;
					}
					else if( sItem == _T("dest") ) {
						LPCTSTR pv = sValue.GetData();
						if( !DuiLib::attr::ParseRect4(pv, rcDest) ) { DUI_LOGW("attr 'dest' bad value='%s'", sValue.GetData()); }
						//if(pManager != nullptr) pManager->GetDPIObj()->Scale(&rcDest);
					}
					else if( sItem == _T("source") ) {
						LPCTSTR pv = sValue.GetData();
						if( !DuiLib::attr::ParseRect4(pv, rcSource) ) { DUI_LOGW("attr 'source' bad value='%s'", sValue.GetData()); }
					}
					else if( sItem == _T("corner") ) {
						LPCTSTR pv = sValue.GetData();
						if( !DuiLib::attr::ParseRect4(pv, rcCorner) ) { DUI_LOGW("attr 'corner' bad value='%s'", sValue.GetData()); }
					}
					else if( sItem == _T("mask") ) {
						if( sValue[0] == _T('#')) dwMask = _tcstoul(sValue.GetData() + 1, &pstr, 16);
						else dwMask = _tcstoul(sValue.GetData(), &pstr, 16);
					}
					else if( sItem == _T("fade") ) {
						uFade = static_cast<UINT>(_tcstoul(sValue.GetData(), &pstr, 10));
					}
					else if( sItem == _T("rotate") ) {
						uRotate = static_cast<UINT>(_tcstoul(sValue.GetData(), &pstr, 10));
						bGdiplus = true;
					}
					else if( sItem == _T("gdiplus") ) {
						bGdiplus = (_tcsicmp(sValue.GetData(), _T("true")) == 0);
					}
					else if( sItem == _T("hole") ) {
						bHole = (_tcsicmp(sValue.GetData(), _T("true")) == 0);
					}
					else if( sItem == _T("xtiled") ) {
						bTiledX = (_tcsicmp(sValue.GetData(), _T("true")) == 0);
					}
					else if( sItem == _T("ytiled") ) {
						bTiledY = (_tcsicmp(sValue.GetData(), _T("true")) == 0);
					}
					else if( sItem == _T("hsl") ) {
						bHSL = (_tcsicmp(sValue.GetData(), _T("true")) == 0);
					}
					else if( sItem == _T("size") ) {
						LPCTSTR pv = sValue.GetData();
						if( !DuiLib::attr::ParseSize2(pv, szImage) ) { DUI_LOGW("attr 'size' bad value='%s'", sValue.GetData()); }
					}
					else if( sItem == _T("align") ) {
						sAlign = sValue;
					}
					else if( sItem == _T("padding") ) {
						LPCTSTR pv = sValue.GetData();
						if( !DuiLib::attr::ParseRect4(pv, rcPadding) ) { DUI_LOGW("attr 'padding' bad value='%s'", sValue.GetData()); }
						//if(pManager != nullptr) pManager->GetDPIObj()->Scale(&rcPadding);
					}
				}
				if( *pStrImage++ != _T(' ') ) break;
			}
		}

		// 调整DPI资源
		if (pManager != nullptr && pManager->GetDPIObj()->GetScale() != 100) {
			CDuiString sScale;
			sScale.Format(_T("@%d."), pManager->GetDPIObj()->GetScale());
			sImageName.Replace(_T("."), sScale);
		}
	}
	void TDrawInfo::Clear()
	{
		sDrawString.Empty();
		sDrawModify.Empty();
		sImageName.Empty();

		memset(&rcDest, 0, sizeof(RECT));
		memset(&rcSource, 0, sizeof(RECT));
		memset(&rcCorner, 0, sizeof(RECT));
		dwMask = 0;
		uFade = 255;
		uRotate = 0;
		bHole = false;
		bTiledX = false;
		bTiledY = false;
		bHSL = false;
		bGdiplus = false;

		szImage.cx = szImage.cy = 0;
		sAlign.Empty();
		memset(&rcPadding, 0, sizeof(RECT));
	}

	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	///
	using PFUNCUPDATELAYEREDWINDOW = BOOL (__stdcall *)(HWND, HDC, POINT*, SIZE*, HDC, POINT*, COLORREF, BLENDFUNCTION*, DWORD);
	PFUNCUPDATELAYEREDWINDOW g_fUpdateLayeredWindow = nullptr;
	// [PERF-6] UpdateLayeredWindowIndirect can upload only the changed rectangle.
	// The plain variant always uploads the full window, so every small repaint of
	// a large layered window paid for a full-surface copy.
	using PFUNCUPDATELAYEREDWINDOWINDIRECT = BOOL (__stdcall *)(HWND, const UPDATELAYEREDWINDOWINFO*);
	PFUNCUPDATELAYEREDWINDOWINDIRECT g_fUpdateLayeredWindowIndirect = nullptr;

	HPEN m_hUpdateRectPen = nullptr;

	HINSTANCE CPaintManagerUI::m_hResourceInstance = nullptr;
	CDuiString CPaintManagerUI::m_pStrResourcePath;
	CDuiString CPaintManagerUI::m_pStrResourceZip;
	CDuiString CPaintManagerUI::m_pStrResourceZipPwd;
	HANDLE CPaintManagerUI::m_hResourceZip = nullptr;
	bool CPaintManagerUI::m_bCachedResourceZip = true;
	BYTE* CPaintManagerUI::m_cbZipBuf = nullptr;
	int CPaintManagerUI::m_nResType = UILIB_FILE;
	TResInfo CPaintManagerUI::m_SharedResInfo;
	// [SEC-21] Guards the one-time initialisation of m_SharedResInfo. Without it
	// two threads creating their first CPaintManagerUI could both observe the
	// empty font name and race on the shared HFONT (one leaked, the other used
	// after deletion).
	static std::once_flag g_sharedResOnce;
	HINSTANCE CPaintManagerUI::m_hInstance = nullptr;
	bool CPaintManagerUI::m_bUseHSL = false;
	short CPaintManagerUI::m_H = 180;
	short CPaintManagerUI::m_S = 100;
	short CPaintManagerUI::m_L = 100;
	CStdPtrArray CPaintManagerUI::m_aPreMessages;
	CStdPtrArray CPaintManagerUI::m_aPlugins;

	////////////////////////////////////////////////////////////////////////////////////////////
	// 触摸API函数指针（动态加载，避免XP链接错误）
	using PFN_REGISTERTOUCHWINDOW = BOOL (__stdcall*)(HWND, ULONG);
	using PFN_GETTOUCHINPUTINFO = BOOL (__stdcall*)(HTOUCHINPUT, UINT, PTOUCHINPUT, int);
	using PFN_CLOSETouchINPUTHANDLE = BOOL (__stdcall*)(HTOUCHINPUT);

	static PFN_REGISTERTOUCHWINDOW pRegisterTouchWindow = nullptr;
	static PFN_GETTOUCHINPUTINFO pGetTouchInputInfo = nullptr;
	static PFN_CLOSETouchINPUTHANDLE pCloseTouchInputHandle = nullptr;

	// 初始化触摸API（仅Vista+加载）
	static void InitTouchAPI() {
		if (!IsWindowsVistaOrGreater()) return;
		HMODULE hUser32 = GetModuleHandle(_T("user32.dll"));
		if (!hUser32) return;
		pRegisterTouchWindow = reinterpret_cast<PFN_REGISTERTOUCHWINDOW>(GetProcAddress(hUser32, "RegisterTouchWindow"));
		pGetTouchInputInfo = reinterpret_cast<PFN_GETTOUCHINPUTINFO>(GetProcAddress(hUser32, "GetTouchInputInfo"));
		pCloseTouchInputHandle = reinterpret_cast<PFN_CLOSETouchINPUTHANDLE>(GetProcAddress(hUser32, "CloseTouchInputHandle"));
	}


	// [SEC-20] implemented in UIControl.cpp (live paint-manager registry)
	////////////////////////////////////////////////////////////////////////////////////////////
	CPaintManagerUI::CPaintManagerUI() :
	m_hWndPaint(nullptr),
		m_hDcPaint(nullptr),
		m_hDcOffscreen(nullptr),
		m_hDcBackground(nullptr),
		m_bOffscreenPaint(true),
		m_hbmpOffscreen(nullptr),
		m_pOffscreenBits(nullptr),
		m_hbmpBackground(nullptr),
		m_pBackgroundBits(nullptr),
		m_hwndTooltip(nullptr),
		m_uTimerID(0x1000),
		m_pRoot(nullptr),
		m_pFocus(nullptr),
		m_pEventHover(nullptr),
		m_pEventClick(nullptr),
		m_pEventRClick(nullptr),
		m_pEventKey(nullptr),
		m_bFirstLayout(true),
		m_bFocusNeeded(false),
		m_bUpdateNeeded(false),
		m_bMouseTracking(false),
		m_bMouseCapture(false),
		m_bAsyncNotifyPosted(false),
		m_bUsedVirtualWnd(false),
		m_bForceUseSharedRes(false),
		// [R43] Initialised in ALL builds. While this lived inside #ifdef _DEBUG,
		// Release left the member uninitialised -- and after R43 made the flag
		// actually readable in Release, an indeterminate value could randomly
		// enable or disable validation. The probe passing would have been luck,
		// not correctness.
		m_bAttributeValidation(GetAttributeValidationDefault()),
		m_nOpacity(0xFF),
		m_bLayered(false),
		m_bLayeredChanged(false),
		m_bShowUpdateRect(false),
		m_bUseGdiplusText(false),
		m_trh(0),
		m_bDragDrop(false),
		m_bDragMode(false),
		m_hDragBitmap(nullptr),
		m_pDPI(nullptr),
		m_iHoverTime(400UL)
	{
		// [PERF-22] new CFrameWnd showed 26-36ms while its own ctor body only
		// accounts for ~4ms. The bases run first: this ctor performs several
		// one-time OS initialisations, so time them explicitly.
		LARGE_INTEGER qpcPmCtor = DuiLib::perfprobe::CClock::Instance().Now();
		auto PmCtorMark = [](const char* pszTag, LARGE_INTEGER t0) {
			if (!DuiLib::perfprobe::Enabled()) return;
			DuiLib::perfprobe::Mark(pszTag, t0);
		};
		DuiLibRegisterManager(this); // [SEC-20]

		PmCtorMark("  pmctor:register", qpcPmCtor); qpcPmCtor = DuiLib::perfprobe::CClock::Instance().Now();
		// [H1] auto-install crash guard once (idempotent, C++17 std::call_once)
		UICrashGuard::Install();
		CUiAutomation::Install(this); // [A11Y-2] MSAA bridge for own-drawn controls
		PmCtorMark("  pmctor:guard+a11y", qpcPmCtor); qpcPmCtor = DuiLib::perfprobe::CClock::Instance().Now();
		// 初始化触摸坐标
		m_ptLastTouch.x = m_ptLastTouch.y = 0;

		std::call_once(g_sharedResOnce, []()
		{
			m_SharedResInfo.m_dwDefaultDisabledColor = 0xFFA7A6AA;
			m_SharedResInfo.m_dwDefaultFontColor = 0xFF000000;
			m_SharedResInfo.m_dwDefaultLinkFontColor = 0xFF0000FF;
			m_SharedResInfo.m_dwDefaultLinkHoverFontColor = 0xFFD3215F;
			m_SharedResInfo.m_dwDefaultSelectedBkColor = 0xFFBAE4FF;

			LOGFONT lf = { 0 };
			::GetObject(::GetStockObject(DEFAULT_GUI_FONT), sizeof(LOGFONT), &lf);
			lf.lfCharSet = DEFAULT_CHARSET;
			HFONT hDefaultFont = ::CreateFontIndirect(&lf);
			m_SharedResInfo.m_DefaultFontInfo.hFont = hDefaultFont;
			m_SharedResInfo.m_DefaultFontInfo.sFontName = lf.lfFaceName;
			m_SharedResInfo.m_DefaultFontInfo.iSize = -lf.lfHeight;
			m_SharedResInfo.m_DefaultFontInfo.bBold = (lf.lfWeight >= FW_BOLD);
			m_SharedResInfo.m_DefaultFontInfo.bUnderline = (lf.lfUnderline == TRUE);
			m_SharedResInfo.m_DefaultFontInfo.bItalic = (lf.lfItalic == TRUE);
			::ZeroMemory(&m_SharedResInfo.m_DefaultFontInfo.tm, sizeof(m_SharedResInfo.m_DefaultFontInfo.tm));
		});

		m_ResInfo.m_dwDefaultDisabledColor = m_SharedResInfo.m_dwDefaultDisabledColor;
		m_ResInfo.m_dwDefaultFontColor = m_SharedResInfo.m_dwDefaultFontColor;
		m_ResInfo.m_dwDefaultLinkFontColor = m_SharedResInfo.m_dwDefaultLinkFontColor;
		m_ResInfo.m_dwDefaultLinkHoverFontColor = m_SharedResInfo.m_dwDefaultLinkHoverFontColor;
		m_ResInfo.m_dwDefaultSelectedBkColor = m_SharedResInfo.m_dwDefaultSelectedBkColor;
		PmCtorMark("  pmctor:sharedres", qpcPmCtor); qpcPmCtor = DuiLib::perfprobe::CClock::Instance().Now();

		if( m_hUpdateRectPen == nullptr ) {
			m_hUpdateRectPen = ::CreatePen(PS_SOLID, 1, RGB(220, 0, 0));
			// Boot Windows Common Controls (for the ToolTip control)
			::InitCommonControls();
			::LoadLibrary(_T("msimg32.dll"));
			PmCtorMark("  pmctor:commonctl", qpcPmCtor); qpcPmCtor = DuiLib::perfprobe::CClock::Instance().Now();
		}

		m_szMinWindow.cx = 0;
		m_szMinWindow.cy = 0;
		m_szMaxWindow.cx = 0;
		m_szMaxWindow.cy = 0;
		m_szInitWindowSize.cx = 0;
		m_szInitWindowSize.cy = 0;
		m_szRoundCorner.cx = m_szRoundCorner.cy = 0;
		::ZeroMemory(&m_rcSizeBox, sizeof(m_rcSizeBox));
		::ZeroMemory(&m_rcCaption, sizeof(m_rcCaption));
		::ZeroMemory(&m_rcLayeredInset, sizeof(m_rcLayeredInset));
		::ZeroMemory(&m_rcLayeredUpdate, sizeof(m_rcLayeredUpdate));
		m_ptLastMousePos.x = m_ptLastMousePos.y = -1;

		m_pGdiplusStartupInput = new Gdiplus::GdiplusStartupInput;
		Gdiplus::GdiplusStartup( &m_gdiplusToken, m_pGdiplusStartupInput, nullptr); // 加载GDI接口

		CShadowUI::Initialize(m_hInstance);

		m_pDragDrop = nullptr;
	}

	CPaintManagerUI::~CPaintManagerUI()
	{
		// [UIA-LIFETIME] Retire this manager's accessibility objects BEFORE the control
		// tree is deleted. Providers handed to an assistive-technology client hold raw
		// CControlUI* / CPaintManagerUI*; without this call they stay registered and a
		// client querying them after the window closes dereferences freed memory.
		// Uninstall() had no caller anywhere in the codebase until now.
		CUiAutomation::Uninstall(this);

		DuiLibUnregisterManager(this); // [SEC-20] controls may outlive the manager
		// [R-07] Drop this manager's thread claim. Without it the registry keeps a
		// dangling pointer, and a later manager allocated at the same address
		// would inherit the previous window's owner thread.
		CThreadAffinity::UnregisterUiThread(this);
		// [R-07] Drop any task still queued for this manager. A task posted just
		// before shutdown would otherwise run against a destroyed manager; every
		// dropped task is counted so the loss is visible via DroppedUiTasks().
		DiscardTaskQueue(this);

		// Delete the control-tree structures
		for( int i = 0; i < m_aDelayedCleanup.GetSize(); i++ ) delete static_cast<CControlUI*>(m_aDelayedCleanup[i]);
		m_aDelayedCleanup.Resize(0);
		for( int i = 0; i < m_aAsyncNotify.GetSize(); i++ ) delete static_cast<TNotifyUI*>(m_aAsyncNotify[i]);
		m_aAsyncNotify.Resize(0);

		m_mNameHash.Resize(0);
		if( m_pRoot != nullptr ) delete m_pRoot;

		::DeleteObject(m_ResInfo.m_DefaultFontInfo.hFont);
		RemoveAllFonts();
		RemoveAllImages();
		RemoveAllStyle();
		RemoveAllDefaultAttributeList();
		RemoveAllWindowCustomAttribute();
		RemoveAllOptionGroups();
		RemoveAllTimers();
		RemoveAllDrawInfos();

		if( m_hwndTooltip != nullptr ) {
			::DestroyWindow(m_hwndTooltip);
			m_hwndTooltip = nullptr;
		}
		if (!m_aFonts.IsEmpty()) {
			for (int i = 0; i < m_aFonts.GetSize();++i)
			{
				HANDLE handle = static_cast<HANDLE>(m_aFonts.GetAt(i));
				::RemoveFontMemResourceEx(handle);
			}
		}
		ReleaseNativeSurfaces(); // [PERF-12]
		if( m_hDcOffscreen != nullptr ) ::DeleteDC(m_hDcOffscreen);
		if( m_hDcBackground != nullptr ) ::DeleteDC(m_hDcBackground);
		if( m_hbmpOffscreen != nullptr ) ::DeleteObject(m_hbmpOffscreen);
		if( m_hbmpBackground != nullptr ) ::DeleteObject(m_hbmpBackground);
		if( m_hDcPaint != nullptr ) ::ReleaseDC(m_hWndPaint, m_hDcPaint);
		m_aPreMessages.Remove(m_aPreMessages.Find(this));
		// 销毁拖拽图片
		if( m_hDragBitmap != nullptr ) ::DeleteObject(m_hDragBitmap);
		//卸载GDIPlus
		Gdiplus::GdiplusShutdown(m_gdiplusToken);
		delete m_pGdiplusStartupInput;
		// DPI管理对象
		if (m_pDPI != nullptr) {
			delete m_pDPI;
			m_pDPI = nullptr;
		}
	}

	void CPaintManagerUI::Init(HWND hWnd, LPCTSTR pstrName)
	{
		ASSERT(::IsWindow(hWnd));

		m_mNameHash.Resize(0);
		RemoveAllFonts();
		RemoveAllImages();
		RemoveAllStyle();
		RemoveAllDefaultAttributeList();
		RemoveAllWindowCustomAttribute();
		RemoveAllOptionGroups();
		RemoveAllTimers();

		m_sName.Empty();
		if( pstrName != nullptr ) m_sName = pstrName;

		if( m_hWndPaint != hWnd ) {
			m_hWndPaint = hWnd;
			m_hDcPaint = ::GetDC(hWnd);
			m_aPreMessages.Add(this);

			m_ptLastTouch.x = m_ptLastTouch.y = 0;
			m_bTouchSupported = false;
			if (IsWindowsVistaOrGreater()) {
				InitTouchAPI(); // 动态加载触摸API
				if (pRegisterTouchWindow) {
					// 注册触摸窗口（TWF_FINETOUCH：高精度触摸）
					pRegisterTouchWindow(hWnd, TWF_FINETOUCH);
					m_bTouchSupported = true;
				}
			}
		}
	}

	void CPaintManagerUI::DeletePtr(void* ptr)
	{
		if(ptr) {delete ptr; ptr = nullptr;}
	}

	HINSTANCE CPaintManagerUI::GetInstance()
	{
		return m_hInstance;
	}

	CDuiString CPaintManagerUI::GetInstancePath()
	{
		if( m_hInstance == nullptr ) return _T('\0');

		TCHAR tszModule[MAX_PATH + 1] = { 0 };
		::GetModuleFileName(m_hInstance, tszModule, MAX_PATH);
		CDuiString sInstancePath = tszModule;
		int pos = sInstancePath.ReverseFind(_T('\\'));
		if( pos >= 0 ) sInstancePath = sInstancePath.Left(pos + 1);
		return sInstancePath;
	}

	CDuiString CPaintManagerUI::GetCurrentPath()
	{
		TCHAR tszModule[MAX_PATH + 1] = { 0 };
		::GetCurrentDirectory(MAX_PATH, tszModule);
		return tszModule;
	}

	HINSTANCE CPaintManagerUI::GetResourceDll()
	{
		if( m_hResourceInstance == nullptr ) return m_hInstance;
		return m_hResourceInstance;
	}

	const CDuiString& CPaintManagerUI::GetResourcePath()
	{
		return m_pStrResourcePath;
	}

	const CDuiString& CPaintManagerUI::GetResourceZip()
	{
		return m_pStrResourceZip;
	}

	const CDuiString& CPaintManagerUI::GetResourceZipPwd()
	{
		return m_pStrResourceZipPwd;
	}

	bool CPaintManagerUI::IsCachedResourceZip()
	{
		return m_bCachedResourceZip;
	}

	HANDLE CPaintManagerUI::GetResourceZipHandle()
	{
		return m_hResourceZip;
	}

	void CPaintManagerUI::SetInstance(HINSTANCE hInst)
	{
		m_hInstance = hInst;
	}

	void CPaintManagerUI::SetCurrentPath(LPCTSTR pStrPath)
	{
		::SetCurrentDirectory(pStrPath);
	}

	void CPaintManagerUI::SetResourceDll(HINSTANCE hInst)
	{
		m_hResourceInstance = hInst;
	}

	void CPaintManagerUI::SetResourcePath(LPCTSTR pStrPath)
	{
		m_pStrResourcePath = pStrPath;
		if( m_pStrResourcePath.IsEmpty() ) return;
		TCHAR cEnd = m_pStrResourcePath.GetAt(m_pStrResourcePath.GetLength() - 1);
		if( cEnd != _T('\\') && cEnd != _T('/') ) m_pStrResourcePath += _T('\\');
	}

	void CPaintManagerUI::SetResourceZip(LPVOID pVoid, unsigned int len, LPCTSTR password)
	{
		if( m_pStrResourceZip == _T("membuffer") ) return;
		if( m_bCachedResourceZip && m_hResourceZip != nullptr ) {
			CloseZip(reinterpret_cast<HZIP>(m_hResourceZip));
			m_hResourceZip = nullptr;
		}
		m_pStrResourceZip = _T("membuffer");
        if (len == 0 || pVoid == nullptr) { // [M7] guard null/empty buffer
            DUI_LOGE("SetResourceZip(membuffer): null/empty buffer (len=%u)", len);
            return;
        }
        if (m_cbZipBuf)
        {
            delete[] m_cbZipBuf;
            m_cbZipBuf = nullptr;
        }
        m_cbZipBuf = new(std::nothrow) BYTE[len]; // [M7] nothrow allocation
        if (m_cbZipBuf == nullptr) {
            DUI_LOGF("SetResourceZip: allocation failed (len=%u)", len);
            m_pStrResourceZip.Empty();
            m_bCachedResourceZip = false;
            return;
        }
        memcpy(m_cbZipBuf, pVoid, len);

		m_bCachedResourceZip = true;
		m_pStrResourceZipPwd = password;  //Garfield 20160325 带密码zip包解密
		if( m_bCachedResourceZip ) 
		{
#ifdef UNICODE
			std::unique_ptr<char[]> pwd{ w2a(const_cast<wchar_t*>(password)) }; // [R4] RAII instead of manual new[]/delete[]
			m_hResourceZip = reinterpret_cast<HANDLE>(OpenZip(m_cbZipBuf, len, pwd.get()));
#else
			m_hResourceZip = reinterpret_cast<HANDLE>(OpenZip(m_cbZipBuf, len, password));
#endif
		}
	}

	void CPaintManagerUI::SetResourceZip(LPCTSTR pStrPath, bool bCachedResourceZip, LPCTSTR password)
	{
		if( m_pStrResourceZip == pStrPath && m_bCachedResourceZip == bCachedResourceZip ) return;
		if( m_bCachedResourceZip && m_hResourceZip != nullptr ) {
			CloseZip(reinterpret_cast<HZIP>(m_hResourceZip));
			m_hResourceZip = nullptr;
		}
		m_pStrResourceZip = pStrPath;
		m_bCachedResourceZip = bCachedResourceZip;
		m_pStrResourceZipPwd = password;
		if( m_bCachedResourceZip ) {
			CDuiString sFile = CPaintManagerUI::GetResourcePath();
			sFile += CPaintManagerUI::GetResourceZip();
#ifdef UNICODE
			std::unique_ptr<char[]> pwd{ w2a(const_cast<wchar_t*>(password)) }; // [R4] RAII instead of manual new[]/delete[]
			m_hResourceZip = reinterpret_cast<HANDLE>(OpenZip(sFile.GetData(), pwd.get()));
#else
			m_hResourceZip = reinterpret_cast<HANDLE>(OpenZip(sFile.GetData(), password));
#endif
		}
	}

	void CPaintManagerUI::SetResourceType(int nType)
	{
		m_nResType = nType;
	}

	int CPaintManagerUI::GetResourceType()
	{
		return m_nResType;
	}

	bool CPaintManagerUI::GetHSL(short* H, short* S, short* L)
	{
		*H = m_H;
		*S = m_S;
		*L = m_L;
		return m_bUseHSL;
	}

	void CPaintManagerUI::SetHSL(bool bUseHSL, short H, short S, short L)
	{
		if( m_bUseHSL || m_bUseHSL != bUseHSL ) {
			m_bUseHSL = bUseHSL;
			if( H == m_H && S == m_S && L == m_L ) return;
			m_H = CLAMP(H, 0, 360);
			m_S = CLAMP(S, 0, 200);
			m_L = CLAMP(L, 0, 200);
			AdjustSharedImagesHSL();
			for( int i = 0; i < m_aPreMessages.GetSize(); i++ ) {
				CPaintManagerUI* pManager = static_cast<CPaintManagerUI*>(m_aPreMessages[i]);
				if( pManager != nullptr ) pManager->AdjustImagesHSL();
			}
		}
	}

	void CPaintManagerUI::ReloadSkin()
	{
		ReloadSharedImages();
		for( int i = 0; i < m_aPreMessages.GetSize(); i++ ) {
			CPaintManagerUI* pManager = static_cast<CPaintManagerUI*>(m_aPreMessages[i]);
			pManager->ReloadImages();
		}
	}

	CPaintManagerUI* CPaintManagerUI::GetPaintManager(LPCTSTR pstrName)
	{
		if( pstrName == nullptr ) return nullptr;
		CDuiString sName = pstrName;
		if( sName.IsEmpty() ) return nullptr;
		for( int i = 0; i < m_aPreMessages.GetSize(); i++ ) {
			CPaintManagerUI* pManager = static_cast<CPaintManagerUI*>(m_aPreMessages[i]);
			if( pManager != nullptr && sName == pManager->GetName() ) return pManager;
		}
		return nullptr;
	}

	CStdPtrArray* CPaintManagerUI::GetPaintManagers()
	{
		return &m_aPreMessages;
	}

	bool CPaintManagerUI::LoadPlugin(LPCTSTR pstrModuleName)
	{
		ASSERT( !::IsBadStringPtr(pstrModuleName,-1) || pstrModuleName == nullptr );
		if( pstrModuleName == nullptr ) return false;
		HMODULE hModule = ::LoadLibrary(pstrModuleName);
		if( hModule != nullptr ) {
			LPCREATECONTROL lpCreateControl = (LPCREATECONTROL)::GetProcAddress(hModule, "CreateControl");
			if( lpCreateControl != nullptr ) {
				if( m_aPlugins.Find(lpCreateControl) >= 0 ) return true;
				m_aPlugins.Add(lpCreateControl);
				return true;
			}
		}
		return false;
	}

	CStdPtrArray* CPaintManagerUI::GetPlugins()
	{
		return &m_aPlugins;
	}

	HWND CPaintManagerUI::GetPaintWindow() const
	{
		return m_hWndPaint;
	}

	HWND CPaintManagerUI::GetTooltipWindow() const
	{
		return m_hwndTooltip;
	}
	int CPaintManagerUI::GetHoverTime() const
	{
		return m_iHoverTime;
	}

	void CPaintManagerUI::SetHoverTime(int iTime)
	{
		m_iHoverTime = iTime;
	}

	LPCTSTR CPaintManagerUI::GetName() const
	{
		return m_sName;
	}

	HDC CPaintManagerUI::GetPaintDC() const
	{
		return m_hDcPaint;
	}

	POINT CPaintManagerUI::GetMousePos() const
	{
		return m_ptLastMousePos;
	}

	SIZE CPaintManagerUI::GetClientSize() const
	{
		RECT rcClient = { 0 };
		::GetClientRect(m_hWndPaint, &rcClient);
		return CDuiSize(rcClient.right - rcClient.left, rcClient.bottom - rcClient.top);
	}

	SIZE CPaintManagerUI::GetInitSize()
	{
		return m_szInitWindowSize;
	}

	void CPaintManagerUI::SetInitSize(int cx, int cy)
	{
		m_szInitWindowSize.cx = cx;
		m_szInitWindowSize.cy = cy;
		if( m_pRoot == nullptr && m_hWndPaint != nullptr ) {
			::SetWindowPos(m_hWndPaint, nullptr, 0, 0, cx, cy, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
		}
	}

	RECT CPaintManagerUI::GetSizeBox()
	{
		return GetDPIObj()->Scale(m_rcSizeBox);
	}

	void CPaintManagerUI::SetSizeBox(RECT& rcSizeBox)
	{
		m_rcSizeBox = rcSizeBox;
	}

	RECT CPaintManagerUI::GetCaptionRect()
	{
		return GetDPIObj()->Scale(m_rcCaption);
	}

	void CPaintManagerUI::SetCaptionRect(RECT& rcCaption)
	{
		m_rcCaption = rcCaption;
	}

	SIZE CPaintManagerUI::GetRoundCorner()
	{
		return GetDPIObj()->Scale(m_szRoundCorner);
	}

	void CPaintManagerUI::SetRoundCorner(int cx, int cy)
	{
		m_szRoundCorner.cx = cx;
		m_szRoundCorner.cy = cy;
	}

	SIZE CPaintManagerUI::GetMinInfo()
	{
		return GetDPIObj()->Scale(m_szMinWindow);
	}

	void CPaintManagerUI::SetMinInfo(int cx, int cy)
	{
		ASSERT(cx>=0 && cy>=0);
		m_szMinWindow.cx = cx;
		m_szMinWindow.cy = cy;
	}

	SIZE CPaintManagerUI::GetMaxInfo()
	{
		return GetDPIObj()->Scale(m_szMaxWindow);
	}

	void CPaintManagerUI::SetMaxInfo(int cx, int cy)
	{
		ASSERT(cx>=0 && cy>=0);
		m_szMaxWindow.cx = cx;
		m_szMaxWindow.cy = cy;
	}

	bool CPaintManagerUI::IsShowUpdateRect()
	{
		return m_bShowUpdateRect;
	}

	void CPaintManagerUI::SetShowUpdateRect(bool show)
	{
		m_bShowUpdateRect = show;
	}

	bool CPaintManagerUI::IsNoActivate()
	{
		return m_bNoActivate;
	}

	void CPaintManagerUI::SetNoActivate(bool bNoActivate)
	{
		m_bNoActivate = bNoActivate;
	}

	BYTE CPaintManagerUI::GetOpacity() const
	{
		return m_nOpacity;
	}

	void CPaintManagerUI::SetOpacity(BYTE nOpacity)
	{
		m_nOpacity = nOpacity;
		if( m_hWndPaint != nullptr ) {
			using PFUNCSETLAYEREDWINDOWATTR = BOOL (__stdcall *)(HWND, COLORREF, BYTE, DWORD);;
			PFUNCSETLAYEREDWINDOWATTR fSetLayeredWindowAttributes = nullptr;

			HMODULE hUser32 = ::GetModuleHandle(_T("User32.dll"));
			if (hUser32)
			{
				fSetLayeredWindowAttributes = 
					reinterpret_cast<PFUNCSETLAYEREDWINDOWATTR>(::GetProcAddress(hUser32, "SetLayeredWindowAttributes"));
				if( fSetLayeredWindowAttributes == nullptr ) return;
			}

			LONG_PTR dwStyle = ::GetWindowLongPtr(m_hWndPaint, GWL_EXSTYLE);
			LONG_PTR dwNewStyle = dwStyle;
			if( nOpacity >= 0 && nOpacity < 256 ) dwNewStyle |= WS_EX_LAYERED;
			else dwNewStyle &= ~WS_EX_LAYERED;
			if(dwStyle != dwNewStyle) ::SetWindowLongPtr(m_hWndPaint, GWL_EXSTYLE, dwNewStyle);
			fSetLayeredWindowAttributes(m_hWndPaint, 0, nOpacity, LWA_ALPHA);
		}
	}

	bool CPaintManagerUI::IsLayered()
	{
		return m_bLayered;
	}

	void CPaintManagerUI::SetLayered(bool bLayered)
	{
		if( m_hWndPaint != nullptr && bLayered != m_bLayered ) {
			UINT uStyle = GetWindowStyle(m_hWndPaint);
			if( (uStyle & WS_CHILD) != 0 ) return;
			if( g_fUpdateLayeredWindow == nullptr ) {
				HMODULE hUser32 = ::GetModuleHandle(_T("User32.dll"));
				if (hUser32) {
					g_fUpdateLayeredWindow = 
						reinterpret_cast<PFUNCUPDATELAYEREDWINDOW>(::GetProcAddress(hUser32, "UpdateLayeredWindow"));
					g_fUpdateLayeredWindowIndirect =
						reinterpret_cast<PFUNCUPDATELAYEREDWINDOWINDIRECT>(::GetProcAddress(hUser32, "UpdateLayeredWindowIndirect"));
					if( g_fUpdateLayeredWindow == nullptr ) return;
				}
			}
			m_bLayered = bLayered;
			if( m_pRoot != nullptr ) m_pRoot->NeedUpdate();
			Invalidate();
		}
	}

	RECT& CPaintManagerUI::GetLayeredInset()
	{
		return m_rcLayeredInset;
	}

	void CPaintManagerUI::SetLayeredInset(RECT& rcLayeredInset)
	{
		m_rcLayeredInset = rcLayeredInset;
		m_bLayeredChanged = true;
		Invalidate();
	}

	BYTE CPaintManagerUI::GetLayeredOpacity()
	{
		return m_nOpacity;
	}

	void CPaintManagerUI::SetLayeredOpacity(BYTE nOpacity)
	{
		m_nOpacity = nOpacity;
		m_bLayeredChanged = true;
		Invalidate();
	}

	LPCTSTR CPaintManagerUI::GetLayeredImage()
	{
		return m_diLayered.sDrawString;
	}

	void CPaintManagerUI::SetLayeredImage(LPCTSTR pstrImage)
	{
		m_diLayered.sDrawString = pstrImage;
		RECT rcNull = {0};
		CRenderEngine::DrawImageInfo(nullptr, this, rcNull, rcNull, &m_diLayered);
		m_bLayeredChanged = true;
		Invalidate();
	}

	CShadowUI* CPaintManagerUI::GetShadow()
	{
		return &m_shadow;
	}

	void CPaintManagerUI::SetUseGdiplusText(bool bUse)
	{
		m_bUseGdiplusText = bUse;
	}

	bool CPaintManagerUI::IsUseGdiplusText() const
	{
		return m_bUseGdiplusText;
	}

	void CPaintManagerUI::SetGdiplusTextRenderingHint(int trh)
	{
		m_trh = trh;
	}

	int CPaintManagerUI::GetGdiplusTextRenderingHint() const
	{
		return m_trh;
	}

	bool CPaintManagerUI::PreMessageHandler(UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT& lRes)
	{
		for( int i = 0; i < m_aPreMessageFilters.GetSize(); i++ ) 
		{
			bool bHandled = false;
			LRESULT lResult = static_cast<IMessageFilterUI*>(m_aPreMessageFilters[i])->MessageHandler(uMsg, wParam, lParam, bHandled);
			if( bHandled ) {
				lRes = lResult;
				return true;
			}
		}
		switch( uMsg ) {
		case WM_KEYDOWN:
			{
				// Tabbing between controls
				if( wParam == VK_TAB ) {
					if( m_pFocus && m_pFocus->IsVisible() && m_pFocus->IsEnabled() && _tcsstr(m_pFocus->GetClass(), _T("RichEditUI")) != nullptr ) {
						if( static_cast<CRichEditUI*>(m_pFocus)->IsWantTab() ) return false;
					}
					if( m_pFocus && m_pFocus->IsVisible() && m_pFocus->IsEnabled() && _tcsstr(m_pFocus->GetClass(), _T("WkeWebkitUI")) != nullptr ) {
						return false;
					}
					SetNextTabControl(::GetKeyState(VK_SHIFT) >= 0);
					return false;
				}
			}
			break;
		case WM_SYSCHAR:
			{
				// Handle ALT-shortcut key-combinations
				FINDSHORTCUT fs = { 0 };
				fs.ch = toupper(static_cast<int>(wParam));
				CControlUI* pControl = m_pRoot->FindControl(__FindControlFromShortcut, &fs, UIFIND_ENABLED | UIFIND_ME_FIRST | UIFIND_TOP_FIRST);
				if( pControl != nullptr ) {
					pControl->SetFocus();
					pControl->Activate();
					return true;
				}
			}
			break;
		case WM_SYSKEYDOWN:
			{
				if( m_pFocus != nullptr ) {
					TEventUI event = { 0 };
					event.Type = UIEVENT_SYSKEY;
					event.chKey = static_cast<TCHAR>(wParam);
					event.ptMouse = m_ptLastMousePos;
					event.wKeyState = MapKeyState();
					event.dwTimestamp = ::GetTickCount();
					m_pFocus->Event(event);
				}
			}
			break;
		}
		return false;
	}

// [R76] Release-visible paint-thread check. Kept as a file-local function so the
// exported surface does not grow and so it is obvious that nothing else calls it.
// Static state on purpose: a paint from a second thread is reported once, not on
// every frame, which is what keeps the diagnostic out of the hot path's cost as
// well as out of the log's noise.
namespace
{
	void RecordPaintThread()
	{
		// Up to 4 distinct foreign threads are reported. Round 76 stopped at the first
		// one, which meant a second, different misuse in the same process stayed
		// invisible. Four is enough to see a pattern and small enough that a broken
		// host cannot turn this into a per-frame log.
		static bool  s_bSeen = false;
		static DWORD s_dwFirst = 0;
		static DWORD s_aForeign[4] = { 0, 0, 0, 0 };
		static int   s_nForeign = 0;
		const DWORD dwNow = ::GetCurrentThreadId();
		if( !s_bSeen ) { s_bSeen = true; s_dwFirst = dwNow; return; }
		if( dwNow == s_dwFirst ) return;
		for( int i = 0; i < s_nForeign; ++i ) {
			if( s_aForeign[i] == dwNow ) return;   // already reported this thread
		}
		if( s_nForeign >= 4 ) return;            // cap reached, stop measuring
		s_aForeign[s_nForeign++] = dwNow;
		char szMsg[200] = { 0 };
		::sprintf_s(szMsg, "[paint] painted from tid=%lu, first paint was tid=%lu (%d of 4)",
			static_cast<unsigned long>(dwNow),
			static_cast<unsigned long>(s_dwFirst),
			s_nForeign);
		::OutputDebugStringA(szMsg);
		::OutputDebugStringA("\r\n");
	}
}

	// [R-07] Cross-thread task queue for PostToUiThread.
	//
	// Lives at file scope, NOT as a member: adding a data member to
	// CPaintManagerUI would change its layout, and WindowImplBase holds one BY
	// VALUE -- the ABI layout guard exists precisely because that once shipped a
	// crash (DLL rebuilt, EXE not). A side table keyed by manager pointer keeps the
	// class untouched.
	//
	// One queue per manager, each guarded by its own lock. The lock is only taken
	// when a worker posts or the UI thread drains -- never on a plain paint.
	namespace
	{
		struct UiTask { CPaintManagerUI::UiTaskProc pfn; void* pCtx; };
		struct UiTaskQueue
		{
			const CPaintManagerUI* pOwner;
			SRWLOCK               lock;
			UiTask*               pItems;
			int                   nCount;
			int                   nCap;
			LONG                  nDropped;
		};
		const int kMaxTaskQueues = 16;
		UiTaskQueue g_taskQueues[kMaxTaskQueues];
		SRWLOCK     g_taskQueuesLock = SRWLOCK_INIT;

		UiTaskQueue* FindTaskQueue(const CPaintManagerUI* p, bool bCreate)
		{
			::AcquireSRWLockExclusive(&g_taskQueuesLock);
			UiTaskQueue* pFree = nullptr;
			for (int i = 0; i < kMaxTaskQueues; ++i) {
				if (g_taskQueues[i].pOwner == p) {
					::ReleaseSRWLockExclusive(&g_taskQueuesLock);
					return &g_taskQueues[i];
				}
				if (g_taskQueues[i].pOwner == nullptr && pFree == nullptr) pFree = &g_taskQueues[i];
			}
			if (bCreate && pFree != nullptr) {
				pFree->pOwner = p;
				pFree->nCount = 0;
				pFree->nDropped = 0;
				if (pFree->pItems == nullptr) {
					pFree->lock = SRWLOCK_INIT;
				}
			}
			else if (bCreate) {
				pFree = nullptr;
			}
			::ReleaseSRWLockExclusive(&g_taskQueuesLock);
			return pFree;
		}

		// Drops everything still queued and frees the buffer. Called from the
		// manager destructor: a task posted just before shutdown would otherwise run
		// against a destroyed manager.
		void DiscardTaskQueue(const CPaintManagerUI* p)
		{
			UiTaskQueue* q = FindTaskQueue(p, false);
			if (q == nullptr) return;
			::AcquireSRWLockExclusive(&q->lock);
			q->nDropped += q->nCount;
			q->nCount = 0;
			delete[] q->pItems;
			q->pItems = nullptr;
			q->nCap = 0;
			::ReleaseSRWLockExclusive(&q->lock);
			::AcquireSRWLockExclusive(&g_taskQueuesLock);
			q->pOwner = nullptr;
			::ReleaseSRWLockExclusive(&g_taskQueuesLock);
		}
	}

	bool CPaintManagerUI::MessageHandler(UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT& lRes)
	{
		if( m_hWndPaint == nullptr ) return false;
		// Cycle through listeners
		for( int i = 0; i < m_aMessageFilters.GetSize(); i++ ) 
		{
			bool bHandled = false;
			LRESULT lResult = static_cast<IMessageFilterUI*>(m_aMessageFilters[i])->MessageHandler(uMsg, wParam, lParam, bHandled);
			if( bHandled ) {
				lRes = lResult;
				switch( uMsg ) {
				case WM_MOUSEMOVE:
				case WM_LBUTTONDOWN:
				case WM_LBUTTONDBLCLK:
				case WM_LBUTTONUP:
					{
						POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
						m_ptLastMousePos = pt;
					}
					break;
				case WM_CONTEXTMENU:
				case WM_MOUSEWHEEL:
					{
						POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
						::ScreenToClient(m_hWndPaint, &pt);
						m_ptLastMousePos = pt;
					}
					break;
				}
				return true;
			}
		}

		if( m_bLayered ) {
			switch( uMsg ) {
			case WM_NCACTIVATE:
				if( !::IsIconic(m_hWndPaint) ) {
					lRes = (wParam == 0) ? TRUE : FALSE;
					return true;
				}
				break;
			case WM_NCCALCSIZE:
			case WM_NCPAINT:
				lRes = 0;
				return true;
			}
		}
		// Custom handling of events
		switch( uMsg ) {
		// [R-07] Cross-thread task drain. A separate message from WM_APP+1 on
		// purpose: that one also flushes delayed cleanup and async notify, and
		// coalesces its post, so a task would have been silently dropped while a
		// notify was already pending.
		case WM_APP + 2:
			{
				UiTaskQueue* q = FindTaskQueue(this, false);
				if (q != nullptr) {
					::AcquireSRWLockExclusive(&q->lock);
					UiTask* pBatch = q->pItems;
					const int nBatch = q->nCount;
					q->pItems = nullptr;
					q->nCount = 0;
					q->nCap = 0;
					::ReleaseSRWLockExclusive(&q->lock);
					// Run OUTSIDE the lock: a task may post another task.
					for (int i = 0; i < nBatch; ++i) {
						if (pBatch[i].pfn != nullptr) pBatch[i].pfn(pBatch[i].pCtx);
					}
					delete[] pBatch;
				}
				lRes = 0;
				return true;
			}
		case WM_APP + 1:
			{
				for( int i = 0; i < m_aDelayedCleanup.GetSize(); i++ ) 
					delete static_cast<CControlUI*>(m_aDelayedCleanup[i]);
				m_aDelayedCleanup.Empty();
				
				m_bAsyncNotifyPosted = false;

				TNotifyUI* pMsg = nullptr;
				while( (pMsg = static_cast<TNotifyUI*>(m_aAsyncNotify.GetAt(0))) ) {
					m_aAsyncNotify.Remove(0);
					if( pMsg->pSender != nullptr ) {
						if( pMsg->pSender->OnNotify ) pMsg->pSender->OnNotify(pMsg);
					}
					for( int j = 0; j < m_aNotifiers.GetSize(); j++ ) {
						static_cast<INotifyUI*>(m_aNotifiers[j])->Notify(*pMsg);
					}
					delete pMsg;
				}
			}
			break;
		case WM_CLOSE:
			{
				// Make sure all matching "closing" events are sent
				TEventUI event = { 0 };
				event.ptMouse = m_ptLastMousePos;
				event.wKeyState = MapKeyState();
				event.dwTimestamp = ::GetTickCount();
				if( m_pEventHover != nullptr ) {
					event.Type = UIEVENT_MOUSELEAVE;
					event.pSender = m_pEventHover;
					m_pEventHover->Event(event);
				}
				if( m_pEventClick != nullptr ) {
					event.Type = UIEVENT_BUTTONUP;
					event.pSender = m_pEventClick;
					m_pEventClick->Event(event);
				}
                if (m_pEventRClick != nullptr)
                {
                    event.Type = UIEVENT_RBUTTONUP;
                    event.pSender = m_pEventClick;
                    m_pEventRClick->Event(event);
                }

				SetFocus(nullptr);

				if( ::GetActiveWindow() == m_hWndPaint ) {
					HWND hwndParent = GetWindowOwner(m_hWndPaint);
					if( hwndParent != nullptr ) ::SetFocus(hwndParent);
				}

				if (m_hwndTooltip != nullptr) {
					::DestroyWindow(m_hwndTooltip);
					m_hwndTooltip = nullptr;
				}
			}
			break;
		case WM_ERASEBKGND:
			{
				// We'll do the painting here...
				lRes = 1;
			}
			return true;
		case WM_PAINT:
			{
				// [R74] The one funnel every paint goes through. Round 71 declined to
				// put DUI_ASSERT_UI_THREAD() on all 37 CRenderEngine draw entries (127
				// call sites, all on the paint hot path) and theorised that guarding a
				// single place instead would be equivalent. Round 74 checked: the
				// backend seam (IRenderBackend) is conditional -- it is only taken when
				// g_pRenderBackend is installed, which it is not by default -- so it is
				// NOT a funnel. WM_PAINT is: every control paint is reached from here,
				// including the m_pRoot==nullptr early path below. This is the one place.
				DUI_ASSERT_UI_THREAD_FOR(this);
				// [R76] The Debug assertion above compiles to nothing in Release, so a
				// shipping build still cannot report a paint from the wrong thread. This
				// is the zero-cost half: perfprobe::Enabled() is a cached environment
				// check, and when it is off the only added work is one predictable branch
				// on an already-loaded static bool.
				// It records the FIRST painting thread and compares every later paint
				// against it, printing only on a mismatch. That answers the question a
				// field report actually raises -- did two threads paint the same window --
				// without any per-paint logging.
				if( DuiLib::perfprobe::Enabled() ) RecordPaintThread();
				if( m_pRoot == nullptr ) {
					PAINTSTRUCT ps = { 0 };
					::BeginPaint(m_hWndPaint, &ps);
					CRenderEngine::DrawColor(m_hDcPaint, ps.rcPaint, 0xFF000000);
					::EndPaint(m_hWndPaint, &ps);
					return true;
				}

				RECT rcClient = { 0 };
				::GetClientRect(m_hWndPaint, &rcClient);

				RECT rcPaint = { 0 };
				if( !::GetUpdateRect(m_hWndPaint, &rcPaint, FALSE) ) return true;

				//if( m_bLayered ) {
				//	m_bOffscreenPaint = true;
				//	rcPaint = m_rcLayeredUpdate;
				//	if( ::IsRectEmpty(&m_rcLayeredUpdate) ) {
				//		PAINTSTRUCT ps = { 0 };
				//		::BeginPaint(m_hWndPaint, &ps);
				//		::EndPaint(m_hWndPaint, &ps);
				//		return true;
				//	}
				//	if( rcPaint.right > rcClient.right ) rcPaint.right = rcClient.right;
				//	if( rcPaint.bottom > rcClient.bottom ) rcPaint.bottom = rcClient.bottom;
				//	::ZeroMemory(&m_rcLayeredUpdate, sizeof(m_rcLayeredUpdate));
				//}
				//else {
				//	if( !::GetUpdateRect(m_hWndPaint, &rcPaint, FALSE) ) return true;
				//}

				// Set focus to first control?
				if( m_bFocusNeeded ) {
					SetNextTabControl();
				}

				SetPainting(true);

				bool bNeedSizeMsg = false;
				DWORD dwWidth = rcClient.right - rcClient.left;
				DWORD dwHeight = rcClient.bottom - rcClient.top;

				SetPainting(true);
				if( m_bUpdateNeeded ) {
					m_bUpdateNeeded = false;
					if( !::IsRectEmpty(&rcClient) && !::IsIconic(m_hWndPaint) ) {
						if( m_pRoot->IsUpdateNeeded() ) {
							RECT rcRoot = rcClient;
							if( m_hDcOffscreen != nullptr ) ::DeleteDC(m_hDcOffscreen);
							if( m_hDcBackground != nullptr ) ::DeleteDC(m_hDcBackground);
							if( m_hbmpOffscreen != nullptr ) ::DeleteObject(m_hbmpOffscreen);
							if( m_hbmpBackground != nullptr ) ::DeleteObject(m_hbmpBackground);
							m_hDcOffscreen = nullptr;
							m_hDcBackground = nullptr;
							m_hbmpOffscreen = nullptr;
							m_hbmpBackground = nullptr;
							if( m_bLayered ) {
								rcRoot.left += m_rcLayeredInset.left;
								rcRoot.top += m_rcLayeredInset.top;
								rcRoot.right -= m_rcLayeredInset.right;
								rcRoot.bottom -= m_rcLayeredInset.bottom;
							}
							m_pRoot->SetPos(rcRoot, true);
							bNeedSizeMsg = true;
						}
						else {
							CControlUI* pControl = nullptr;
							m_aFoundControls.Empty();
							m_pRoot->FindControl(__FindControlsFromUpdate, nullptr, UIFIND_VISIBLE | UIFIND_ME_FIRST | UIFIND_UPDATETEST);
							for( int it = 0; it < m_aFoundControls.GetSize(); it++ ) {
								pControl = static_cast<CControlUI*>(m_aFoundControls[it]);
								pControl->SetPos(pControl->GetPos(), true);
							}
							bNeedSizeMsg = true;
						}
						// We'll want to notify the window when it is first initialized
						// with the correct layout. The window form would take the time
						// to submit swipes/animations.
						if( m_bFirstLayout ) {
							m_bFirstLayout = false;
							SendNotify(m_pRoot, DUI_MSGTYPE_WINDOWINIT,  0, 0, false);
							if( m_bLayered && m_bLayeredChanged ) {
								Invalidate();
								SetPainting(false);
								return true;
							}
							// 更新阴影窗口显示
							m_shadow.Update(m_hWndPaint);
						}
					}
				}
				else if( m_bLayered && m_bLayeredChanged ) {
					RECT rcRoot = rcClient;
					if( m_pOffscreenBits ) ::ZeroMemory(m_pOffscreenBits, (rcRoot.right - rcRoot.left) 
						* (rcRoot.bottom - rcRoot.top) * 4);
					rcRoot.left += m_rcLayeredInset.left;
					rcRoot.top += m_rcLayeredInset.top;
					rcRoot.right -= m_rcLayeredInset.right;
					rcRoot.bottom -= m_rcLayeredInset.bottom;
					m_pRoot->SetPos(rcRoot, true);
				}

				if( m_bLayered ) {
					LONG_PTR dwExStyle = ::GetWindowLongPtr(m_hWndPaint, GWL_EXSTYLE);
					LONG_PTR dwNewExStyle = dwExStyle | WS_EX_LAYERED;
					if(dwExStyle != dwNewExStyle) ::SetWindowLongPtr(m_hWndPaint, GWL_EXSTYLE, dwNewExStyle);
					m_bOffscreenPaint = true;
					UnionRect(&rcPaint, &rcPaint, &m_rcLayeredUpdate);
					if( rcPaint.right > rcClient.right ) rcPaint.right = rcClient.right;
					if( rcPaint.bottom > rcClient.bottom ) rcPaint.bottom = rcClient.bottom;
					::ZeroMemory(&m_rcLayeredUpdate, sizeof(m_rcLayeredUpdate));
				}

				//
				// Render screen
				//
				// Prepare offscreen bitmap
				if( m_bOffscreenPaint && m_hbmpOffscreen == nullptr ) {
					m_hDcOffscreen = ::CreateCompatibleDC(m_hDcPaint);
					m_hbmpOffscreen = CRenderEngine::CreateARGB32Bitmap(m_hDcPaint, dwWidth, dwHeight, (LPBYTE*)&m_pOffscreenBits); 
					ASSERT(m_hDcOffscreen);
					ASSERT(m_hbmpOffscreen);
				}
				// Begin Windows paint
				PAINTSTRUCT ps = { 0 };
				::BeginPaint(m_hWndPaint, &ps);
				if( m_bOffscreenPaint ) {
					HBITMAP hOldBitmap = reinterpret_cast<HBITMAP>(::SelectObject(m_hDcOffscreen, m_hbmpOffscreen));
					int iSaveDC = ::SaveDC(m_hDcOffscreen);
					if (m_bLayered) {
						// [PERF-01] The dirty band was cleared with a nested per-pixel loop.
						// Measured (tools/probes/render_bench.cpp, 1920x1080):
						//   per-pixel loop : 22.11 ms
						//   ZeroMemory     :  1.31 ms   (16.8x faster)
						// The band spans whole rows, so clear it row-wise instead.
						LONG yTop = rcClient.bottom - rcPaint.bottom;
						LONG yBottom = rcClient.bottom - rcPaint.top;
						if (yTop < 0) yTop = 0;
						if (yBottom > static_cast<LONG>(m_szInitWindowSize.cy)) yBottom = static_cast<LONG>(m_szInitWindowSize.cy);
						if (yBottom > yTop) {
							const size_t nRowBytes = static_cast<size_t>(dwWidth) * 4;
							BYTE* pRow = m_pOffscreenBits + static_cast<size_t>(yTop) * nRowBytes;
							::ZeroMemory(pRow, static_cast<size_t>(yBottom - yTop) * nRowBytes);
						}
					}
					m_pRoot->Paint(m_hDcOffscreen, rcPaint, nullptr);

					if( m_bLayered ) {
						for( int i = 0; i < m_aNativeWindow.GetSize(); ) {
							HWND hChildWnd = static_cast<HWND>(m_aNativeWindow[i]);
							if (!::IsWindow(hChildWnd)) {
								m_aNativeWindow.Remove(i);
								m_aNativeWindowControl.Remove(i);
								continue;
							}
							++i;
							if (!::IsWindowVisible(hChildWnd)) continue;
							RECT rcChildWnd = GetNativeWindowRect(hChildWnd);
							RECT rcTemp = { 0 };
							if( !::IntersectRect(&rcTemp, &rcPaint, &rcChildWnd) ) continue;

							// [PERF-12] Reuse the per-child DC/DIB across paints and fix alpha
							// only inside the dirty rectangle. Creating a DC + ARGB bitmap and
							// running a full-surface alpha pass on every paint (per visible
							// native child) was the expensive part.
							const int nCx = rcChildWnd.right - rcChildWnd.left;
							const int nCy = rcChildWnd.bottom - rcChildWnd.top;
							if( nCx <= 0 || nCy <= 0 ) continue;
							TNATIVE_SURFACE& surf = GetNativeSurface(hChildWnd, nCx, nCy);
							if( surf.hMemDC == nullptr || surf.pBits == nullptr ) continue;
							::ZeroMemory(surf.pBits, (size_t)nCx * (size_t)nCy * 4);
							HBITMAP hOldChildBitmap = reinterpret_cast<HBITMAP>(::SelectObject(surf.hMemDC, surf.hBitmap));
							::SendMessage(hChildWnd, WM_PRINT, reinterpret_cast<WPARAM>(surf.hMemDC),static_cast<LPARAM>((PRF_CHECKVISIBLE|PRF_CHILDREN|PRF_CLIENT|PRF_OWNED)));
							RECT rcOverlap = { 0 };
							::IntersectRect(&rcOverlap, &rcPaint, &rcChildWnd);
							const LONG x0 = std::max(0L, rcOverlap.left - rcChildWnd.left);
							const LONG y0 = std::max(0L, rcOverlap.top - rcChildWnd.top);
							const LONG x1 = std::min((LONG)nCx, rcOverlap.right - rcChildWnd.left);
							const LONG y1 = std::min((LONG)nCy, rcOverlap.bottom - rcChildWnd.top);
							for( LONG y = y0; y < y1; y++ ) {
								COLORREF* pRow = surf.pBits + (size_t)y * (size_t)nCx;
								for( LONG x = x0; x < x1; x++ ) {
									if( pRow[x] != 0x00000000 ) pRow[x] |= 0xff000000;
								}
							}
							::BitBlt(m_hDcOffscreen, rcChildWnd.left, rcChildWnd.top, nCx,
								nCy, surf.hMemDC, 0, 0, SRCCOPY);
							::SelectObject(surf.hMemDC, hOldChildBitmap);
						}
					}

					for( int i = 0; i < m_aPostPaintControls.GetSize(); i++ ) {
						CControlUI* pPostPaintControl = static_cast<CControlUI*>(m_aPostPaintControls[i]);
						pPostPaintControl->DoPostPaint(m_hDcOffscreen, rcPaint);
					}

					::RestoreDC(m_hDcOffscreen, iSaveDC);

					if( m_bLayered ) {
						RECT rcWnd = { 0 };
						::GetWindowRect(m_hWndPaint, &rcWnd);
						if(!m_diLayered.sDrawString.IsEmpty()) {
							DWORD dwWidth = rcClient.right - rcClient.left;
							DWORD dwHeight = rcClient.bottom - rcClient.top;
							RECT rcLayeredClient = rcClient;
							rcLayeredClient.left += m_rcLayeredInset.left;
							rcLayeredClient.top += m_rcLayeredInset.top;
							rcLayeredClient.right -= m_rcLayeredInset.right;
							rcLayeredClient.bottom -= m_rcLayeredInset.bottom;

							COLORREF* pOffscreenBits = reinterpret_cast<COLORREF*>(m_pOffscreenBits);
							COLORREF* pBackgroundBits = m_pBackgroundBits;
							BYTE A = 0;
							BYTE R = 0;
							BYTE G = 0;
							BYTE B = 0;
							if (!m_diLayered.sDrawString.IsEmpty()) {
								if( m_hbmpBackground == nullptr) {
									m_hDcBackground = ::CreateCompatibleDC(m_hDcPaint);
									m_hbmpBackground = CRenderEngine::CreateARGB32Bitmap(m_hDcPaint, dwWidth, dwHeight, (BYTE**)&m_pBackgroundBits); 
									::ZeroMemory(m_pBackgroundBits, dwWidth * dwHeight * 4);
									::SelectObject(m_hDcBackground, m_hbmpBackground);
									CRenderClip clip;
									CRenderClip::GenerateClip(m_hDcBackground, rcLayeredClient, clip);
									CRenderEngine::DrawImageInfo(m_hDcBackground, this, rcLayeredClient, rcLayeredClient, &m_diLayered);
								}
								else if( m_bLayeredChanged ) {
									::ZeroMemory(m_pBackgroundBits, dwWidth * dwHeight * 4);
									CRenderClip clip;
									CRenderClip::GenerateClip(m_hDcBackground, rcLayeredClient, clip);
									CRenderEngine::DrawImageInfo(m_hDcBackground, this, rcLayeredClient, rcLayeredClient, &m_diLayered);
								}
								for( LONG y = rcClient.bottom - rcPaint.bottom; y < rcClient.bottom - rcPaint.top; ++y ) {
									for( LONG x = rcPaint.left; x < rcPaint.right; ++x ) {
										pOffscreenBits = reinterpret_cast<COLORREF*>(m_pOffscreenBits + y * dwWidth + x);
										pBackgroundBits = m_pBackgroundBits + y * dwWidth + x;
										A = static_cast<BYTE>(((*pBackgroundBits) >> 24));
										R = static_cast<BYTE>(((*pOffscreenBits) >> 16)) * A / 255;
										G = static_cast<BYTE>(((*pOffscreenBits) >> 8)) * A / 255;
										B = static_cast<BYTE>((*pOffscreenBits)) * A / 255;
										*pOffscreenBits = RGB(B, G, R) + ((DWORD)A << 24);
									}
								}
							}
						}
						else {
							for( LONG y = rcClient.bottom - rcPaint.bottom; y < rcClient.bottom - rcPaint.top; ++y ) {
								for( LONG x = rcPaint.left; x < rcPaint.right; ++x ) {
									int i = (y * dwWidth + x) * 4;
									if((m_pOffscreenBits[i + 3] == 0)&& (m_pOffscreenBits[i + 0] != 0 || m_pOffscreenBits[i + 1] != 0|| m_pOffscreenBits[i + 2] != 0))
										m_pOffscreenBits[i + 3] = 255;
								}
							}
						}

						BLENDFUNCTION bf = { AC_SRC_OVER, 0, m_nOpacity, AC_SRC_ALPHA };
						POINT ptPos   = { rcWnd.left, rcWnd.top };
						SIZE sizeWnd  = { dwWidth, dwHeight };
						POINT ptSrc   = { 0, 0 };
						// [PERF-6] If nothing but a sub-rectangle changed, upload only
						// that rectangle. Falls back to the full-surface call when the
						// indirect entry point is missing (XP-era user32) or when the
						// update area covers everything.
						bool bPartial = false;
						if( g_fUpdateLayeredWindowIndirect != nullptr ) {
							RECT rcDirty = rcPaint;
							RECT rcClientRect = { 0, 0, dwWidth, dwHeight };
							if( ::IntersectRect(&rcDirty, &rcDirty, &rcClientRect) ) {
								const LONG lArea = (rcDirty.right - rcDirty.left) * (rcDirty.bottom - rcDirty.top);
								const LONG lFull = rcClientRect.right * rcClientRect.bottom;
								bPartial = (lArea > 0 && lArea < lFull);
							}
						}
						if( bPartial ) {
							UPDATELAYEREDWINDOWINFO info;
							::ZeroMemory(&info, sizeof(info));
							info.cbSize      = sizeof(info);
							info.hdcDst      = m_hDcPaint;
							info.pptDst      = &ptPos;
							info.psize       = &sizeWnd;
							info.hdcSrc      = m_hDcOffscreen;
							info.pptSrc      = &ptSrc;
							info.pblend      = &bf;
							info.dwFlags     = ULW_ALPHA;
							info.prcDirty    = &rcPaint;
							if( !g_fUpdateLayeredWindowIndirect(m_hWndPaint, &info) ) {
								g_fUpdateLayeredWindow(m_hWndPaint, m_hDcPaint, &ptPos, &sizeWnd, m_hDcOffscreen, &ptSrc, 0, &bf, ULW_ALPHA);
							}
						}
						else {
							g_fUpdateLayeredWindow(m_hWndPaint, m_hDcPaint, &ptPos, &sizeWnd, m_hDcOffscreen, &ptSrc, 0, &bf, ULW_ALPHA);
						}
					}
					else {
						::BitBlt(m_hDcPaint, rcPaint.left, rcPaint.top, rcPaint.right - rcPaint.left, rcPaint.bottom - rcPaint.top, m_hDcOffscreen, rcPaint.left, rcPaint.top, SRCCOPY);
					}
					::SelectObject(m_hDcOffscreen, hOldBitmap);

					if( m_bShowUpdateRect && !m_bLayered ) {
						HPEN hOldPen = reinterpret_cast<HPEN>(::SelectObject(m_hDcPaint, m_hUpdateRectPen));
						::SelectObject(m_hDcPaint, ::GetStockObject(HOLLOW_BRUSH));
						::Rectangle(m_hDcPaint, rcPaint.left, rcPaint.top, rcPaint.right, rcPaint.bottom);
						::SelectObject(m_hDcPaint, hOldPen);
					}
				}
				else {
					// A standard paint job
					int iSaveDC = ::SaveDC(m_hDcPaint);
					m_pRoot->Paint(m_hDcPaint, rcPaint, nullptr);
					for( int i = 0; i < m_aPostPaintControls.GetSize(); i++ ) {
						CControlUI* pPostPaintControl = static_cast<CControlUI*>(m_aPostPaintControls[i]);
						pPostPaintControl->DoPostPaint(m_hDcPaint, rcPaint);
					}
					::RestoreDC(m_hDcPaint, iSaveDC);
				}
				// All Done!
				::EndPaint(m_hWndPaint, &ps);

				// 绘制结束
				SetPainting(false);
				m_bLayeredChanged = false;
				if( m_bUpdateNeeded ) Invalidate();

				// 发送窗口大小改变消息
				if(bNeedSizeMsg) {
					this->SendNotify(m_pRoot, DUI_MSGTYPE_WINDOWSIZE, 0, 0, true);
				}
				return true;
			}
		case WM_PRINTCLIENT:
			{
				if( m_pRoot == nullptr ) break;
				RECT rcClient;
				::GetClientRect(m_hWndPaint, &rcClient);
				HDC hDC = reinterpret_cast<HDC>(wParam);
				int save = ::SaveDC(hDC);
				m_pRoot->Paint(hDC, rcClient, nullptr);
				if( (lParam & PRF_CHILDREN) != 0 ) {
					HWND hWndChild = ::GetWindow(m_hWndPaint, GW_CHILD);
					while( hWndChild != nullptr ) {
						RECT rcPos = { 0 };
						::GetWindowRect(hWndChild, &rcPos);
						::MapWindowPoints(HWND_DESKTOP, m_hWndPaint, reinterpret_cast<LPPOINT>(&rcPos), 2);
						::SetWindowOrgEx(hDC, -rcPos.left, -rcPos.top, nullptr);
						::SendMessage(hWndChild, WM_PRINT, wParam, lParam | PRF_NONCLIENT);
						hWndChild = ::GetWindow(hWndChild, GW_HWNDNEXT);
					}
				}
				::RestoreDC(hDC, save);
			}
			break;
		case WM_GETMINMAXINFO:
			{
				MONITORINFO Monitor = {};
				Monitor.cbSize = sizeof(Monitor);
				::GetMonitorInfo(::MonitorFromWindow(m_hWndPaint, MONITOR_DEFAULTTOPRIMARY), &Monitor);
				RECT rcWork = Monitor.rcWork;
				if( Monitor.dwFlags != MONITORINFOF_PRIMARY ) {
					::OffsetRect(&rcWork, -rcWork.left, -rcWork.top);
				}

				LPMINMAXINFO lpMMI = reinterpret_cast<LPMINMAXINFO>(lParam);
				if( m_szMinWindow.cx > 0 ) lpMMI->ptMinTrackSize.x = m_szMinWindow.cx;
				if( m_szMinWindow.cy > 0 ) lpMMI->ptMinTrackSize.y = m_szMinWindow.cy;
				if( m_szMaxWindow.cx > 0 ) lpMMI->ptMaxTrackSize.x = m_szMaxWindow.cx;
				if( m_szMaxWindow.cy > 0 ) lpMMI->ptMaxTrackSize.y = m_szMaxWindow.cy;
				if( m_szMaxWindow.cx > 0 ) lpMMI->ptMaxSize.x = m_szMaxWindow.cx;
				if( m_szMaxWindow.cy > 0 ) lpMMI->ptMaxSize.y = m_szMaxWindow.cy;
			}
			break;
		case WM_GETOBJECT:
			{
				// [A11Y-2] Self-painted controls are invisible to screen readers
				// unless the window answers OBJID_CLIENT. Hand the request to the
				// accessibility bridge; anything else falls through untouched.
				bool bHandled = false;
				// This branch must WRITE lRes and return true. An earlier version
				// shadowed the output parameter with a local of the same name, so it
				// compiled fine but handed 0 back to the caller and no accessible
				// object ever reached the screen reader.
				LRESULT lAcc = CUiAutomation::OnGetObject(this, wParam, lParam, &bHandled);
				if( bHandled ) {
					lRes = lAcc;
					return true;
				}
			}
			break;

		case WM_DPICHANGED:
			{
				// [DPI-1] Use the suggested window rectangle from the system instead
				// of scaling the old rect proportionally. Proportional scaling drifts
				// when the monitors differ in work area, and it ignores the exact
				// non-client metrics the system wants for the new DPI.
				const UINT uNewDpi = LOWORD(wParam);
				if( uNewDpi >= 48 ) {
					RECT* prcSuggested = reinterpret_cast<RECT*>(lParam);
					const int nOldScale = GetDPIObj()->GetScale();
					GetDPIObj()->SetScale(static_cast<int>(uNewDpi));
					ResetDPIAssets();
					if( prcSuggested != nullptr && !::IsZoomed(m_hWndPaint) ) {
						::SetWindowPos(m_hWndPaint, nullptr,
							prcSuggested->left, prcSuggested->top,
							prcSuggested->right - prcSuggested->left,
							prcSuggested->bottom - prcSuggested->top,
							SWP_NOZORDER | SWP_NOACTIVATE);
					}
					if( GetRoot() != nullptr ) GetRoot()->NeedUpdate();
					DUI_LOGI("WM_DPICHANGED: %d -> %u", nOldScale, uNewDpi);
				}
			}
			break;
		case WM_THEMECHANGED:
		case WM_SYSCOLORCHANGE:
		case WM_SETTINGCHANGE:
			{
				// [A11Y-1] System colours and high-contrast state can change while the
				// app runs. Nothing in the framework reacted to these messages, so a
				// self-painted UI kept stale colours until restart. Drop the cached
				// surfaces and repaint once; host code can also listen to this message.
				if( GetRoot() != nullptr ) {
					GetRoot()->NeedUpdate();
					GetRoot()->Invalidate();
				}
			}
			break;

		case WM_SIZE:
			{
				if( m_pFocus != nullptr ) {
					TEventUI event = { 0 };
					event.Type = UIEVENT_WINDOWSIZE;
					event.pSender = m_pFocus;
					event.dwTimestamp = ::GetTickCount();
					m_pFocus->Event(event);
				}
				if( m_pRoot != nullptr ) m_pRoot->NeedUpdate();
			}
			return true;
		case WM_TIMER:
			{
				for( int i = 0; i < m_aTimers.GetSize(); i++ ) {
					const TIMERINFO* pTimer = static_cast<TIMERINFO*>(m_aTimers[i]);
					if(pTimer->hWnd == m_hWndPaint && 
						pTimer->uWinTimer == LOWORD(wParam) && 
						pTimer->bKilled == false)
					{
						TEventUI event = { 0 };
						event.Type = UIEVENT_TIMER;
						event.pSender = pTimer->pSender;
						event.dwTimestamp = ::GetTickCount();
						event.ptMouse = m_ptLastMousePos;
						event.wKeyState = MapKeyState();
						event.wParam = pTimer->nLocalID;
						event.lParam = lParam;
						pTimer->pSender->Event(event);
						break;
					}
				}
			}
			break;
		case WM_MOUSEHOVER:
			{
				m_bMouseTracking = false;
				POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
				CControlUI* pHover = FindControl(pt);
				if( pHover == nullptr ) break;
				// Generate mouse hover event
				if( m_pEventHover != nullptr ) {
					TEventUI event = { 0 };
					event.Type = UIEVENT_MOUSEHOVER;
					event.pSender = m_pEventHover;
					event.wParam = wParam;
					event.lParam = lParam;
					event.dwTimestamp = ::GetTickCount();
					event.ptMouse = pt;
					event.wKeyState = MapKeyState();
					m_pEventHover->Event(event);
				}
				// Create tooltip information
				CDuiString sToolTip = pHover->GetToolTip();
				if( sToolTip.IsEmpty() ) return true;
				::ZeroMemory(&m_ToolTip, sizeof(TOOLINFO));
				m_ToolTip.cbSize = sizeof(TOOLINFO);
				m_ToolTip.uFlags = TTF_IDISHWND;
				m_ToolTip.hwnd = m_hWndPaint;
				m_ToolTip.uId = reinterpret_cast<UINT_PTR>(m_hWndPaint);
				m_ToolTip.hinst = m_hInstance;
				m_ToolTip.lpszText = const_cast<LPTSTR>(sToolTip.GetData());
				m_ToolTip.rect = pHover->GetPos();
				if( m_hwndTooltip == nullptr ) {
					m_hwndTooltip = ::CreateWindowEx(0, TOOLTIPS_CLASS, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, m_hWndPaint, nullptr, m_hInstance, nullptr);
					::SendMessage(m_hwndTooltip, TTM_ADDTOOL, 0, reinterpret_cast<LPARAM>(&m_ToolTip));
					::SendMessage(m_hwndTooltip,TTM_SETMAXTIPWIDTH,0, pHover->GetToolTipWidth());
				}
				if(!::IsWindowVisible(m_hwndTooltip))
				{
					::SendMessage(m_hwndTooltip, TTM_SETTOOLINFO, 0, reinterpret_cast<LPARAM>(&m_ToolTip));
					::SendMessage(m_hwndTooltip, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&m_ToolTip));
				}
			}
			return true;
		case WM_MOUSELEAVE:
			{
				if( m_hwndTooltip != nullptr ) ::SendMessage(m_hwndTooltip, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&m_ToolTip));
				if( m_bMouseTracking ) {
					POINT pt = { 0 };
					RECT rcWnd = { 0 };
					::GetCursorPos(&pt);
					::GetWindowRect(m_hWndPaint, &rcWnd);
					if( !::IsIconic(m_hWndPaint) && ::GetActiveWindow() == m_hWndPaint && ::PtInRect(&rcWnd, pt) ) {
						if( ::SendMessage(m_hWndPaint, WM_NCHITTEST, 0, MAKELPARAM(pt.x, pt.y)) == HTCLIENT ) {
							::ScreenToClient(m_hWndPaint, &pt);
							::SendMessage(m_hWndPaint, WM_MOUSEMOVE, 0, MAKELPARAM(pt.x, pt.y));
						}
						else 
							::SendMessage(m_hWndPaint, WM_MOUSEMOVE, 0, (LPARAM)-1);
					}
					else 
						::SendMessage(m_hWndPaint, WM_MOUSEMOVE, 0, (LPARAM)-1);
				}
				m_bMouseTracking = false;
			}
			break;
		case WM_MOUSEMOVE:
			{
				// Start tracking this entire window again...
				if( !m_bMouseTracking ) {
					TRACKMOUSEEVENT tme = { 0 };
					tme.cbSize = sizeof(TRACKMOUSEEVENT);
					tme.dwFlags = TME_HOVER | TME_LEAVE;
					tme.hwndTrack = m_hWndPaint;
					tme.dwHoverTime = m_hwndTooltip == nullptr ? m_iHoverTime : static_cast<DWORD>(::SendMessage(m_hwndTooltip, TTM_GETDELAYTIME, TTDT_INITIAL, 0L));
					_TrackMouseEvent(&tme);
					m_bMouseTracking = true;
				}

				// Generate the appropriate mouse messages
				POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
				// 是否移动
				bool bNeedDrag = true;
				if(m_ptLastMousePos.x == pt.x && m_ptLastMousePos.y == pt.y) {
					bNeedDrag = false;
				}
				// 记录鼠标位置
				m_ptLastMousePos = pt;
				CControlUI* pNewHover = FindControl(pt);
				if( pNewHover != nullptr && pNewHover->GetManager() != this ) break;

				// 拖拽事件
				if(bNeedDrag && m_bDragMode && wParam == MK_LBUTTON)
				{
					// 释放Capture
					::ReleaseCapture();
					// 接口
					if(m_pDragDrop != nullptr && m_pDragDrop->OnDragDrop(m_pEventClick)) {

						m_bDragMode = false;
						break;
					}

					CIDropSource* pdsrc = new CIDropSource;
					if(pdsrc == nullptr) return 0;
					pdsrc->AddRef();

					CIDataObject* pdobj = new CIDataObject(pdsrc);
					if(pdobj == nullptr) return 0;
					pdobj->AddRef();

					FORMATETC fmtetc = {0};
					STGMEDIUM medium = {0};
					fmtetc.dwAspect = DVASPECT_CONTENT;
					fmtetc.lindex = -1;
					fmtetc.cfFormat = CF_BITMAP;
					fmtetc.tymed = TYMED_GDI;

					//////////////////////////////////////
					HBITMAP hBitmap = reinterpret_cast<HBITMAP>(OleDuplicateData(m_hDragBitmap, fmtetc.cfFormat, DROPEFFECT_NONE));
					medium.hBitmap = hBitmap;
					pdobj->SetData(&fmtetc, &medium, FALSE);

					//////////////////////////////////////
					BITMAP bmap;
					GetObject(hBitmap, sizeof(BITMAP), &bmap);
					RECT rc={0, 0, bmap.bmWidth, bmap.bmHeight};
					fmtetc.cfFormat = CF_ENHMETAFILE;
					fmtetc.tymed = TYMED_ENHMF;
					HDC hMetaDC = CreateEnhMetaFile(m_hDcPaint, nullptr, nullptr, nullptr);
					HDC hdcMem = CreateCompatibleDC(m_hDcPaint);
					HGDIOBJ hOldBmp = ::SelectObject(hdcMem, hBitmap);
					::BitBlt(hMetaDC, 0, 0, rc.right, rc.bottom, hdcMem, 0, 0, SRCCOPY);
					::SelectObject(hdcMem, hOldBmp);
					medium.hEnhMetaFile = CloseEnhMetaFile(hMetaDC);
					DeleteDC(hdcMem);
					medium.tymed = TYMED_ENHMF;
					pdobj->SetData(&fmtetc, &medium, TRUE);
					//////////////////////////////////////
					CDragSourceHelper dragSrcHelper;
					POINT ptDrag = {0};
					ptDrag.x = bmap.bmWidth / 2;
					ptDrag.y = bmap.bmHeight / 2;
					dragSrcHelper.InitializeFromBitmap(hBitmap, ptDrag, rc, pdobj); //will own the bmp
					DWORD dwEffect = DROPEFFECT_NONE;
					HRESULT hr = ::DoDragDrop(pdobj, pdsrc, DROPEFFECT_COPY | DROPEFFECT_MOVE, &dwEffect);
					if(dwEffect ) pdsrc->Release();
					else delete pdsrc;
					pdobj->Release();
					m_bDragMode = false;
					break;
				}

				TEventUI event = { 0 };
				event.ptMouse = pt;
				event.wParam = wParam;
				event.lParam = lParam;
				event.dwTimestamp = ::GetTickCount();
				event.wKeyState = MapKeyState();
				if( !IsCaptured() ) {
					pNewHover = FindControl(pt);
					if( pNewHover != nullptr && pNewHover->GetManager() != this ) break;
					if( pNewHover != m_pEventHover && m_pEventHover != nullptr ) {
						event.Type = UIEVENT_MOUSELEAVE;
						event.pSender = m_pEventHover;

						CStdPtrArray aNeedMouseLeaveNeeded(m_aNeedMouseLeaveNeeded.GetSize());
						aNeedMouseLeaveNeeded.Resize(m_aNeedMouseLeaveNeeded.GetSize());
						::CopyMemory(aNeedMouseLeaveNeeded.GetData(), m_aNeedMouseLeaveNeeded.GetData(), m_aNeedMouseLeaveNeeded.GetSize() * sizeof(LPVOID));
						for( int i = 0; i < aNeedMouseLeaveNeeded.GetSize(); i++ ) {
							static_cast<CControlUI*>(aNeedMouseLeaveNeeded[i])->Event(event);
						}

						m_pEventHover->Event(event);
						m_pEventHover = nullptr;
						if( m_hwndTooltip != nullptr ) ::SendMessage(m_hwndTooltip, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&m_ToolTip));
					}
					if( pNewHover != m_pEventHover && pNewHover != nullptr ) {
						event.Type = UIEVENT_MOUSEENTER;
						event.pSender = pNewHover;
						pNewHover->Event(event);
						m_pEventHover = pNewHover;
					}
				}
				if( m_pEventClick != nullptr ) {
					event.Type = UIEVENT_MOUSEMOVE;
					event.pSender = m_pEventClick;
					m_pEventClick->Event(event);
				}
				else if( pNewHover != nullptr ) {
					event.Type = UIEVENT_MOUSEMOVE;
					event.pSender = pNewHover;
					pNewHover->Event(event);
				}
			}
			break;
		case WM_LBUTTONDOWN:
			{
				// We alway set focus back to our app (this helps
				// when Win32 child windows are placed on the dialog
				// and we need to remove them on focus change).
				if (!m_bNoActivate) ::SetFocus(m_hWndPaint);
				if( m_pRoot == nullptr ) break;
				// 查找控件
				POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
				m_ptLastMousePos = pt;
				CControlUI* pControl = FindControl(pt);
				if( pControl == nullptr ) break;
				if( pControl->GetManager() != this ) break;

				// 准备拖拽
				if(m_bDragDrop && pControl->IsDragEnabled()) {
					m_bDragMode = true;
					if( m_hDragBitmap != nullptr ) {
						::DeleteObject(m_hDragBitmap);
						m_hDragBitmap = nullptr;
					}
					m_hDragBitmap = CRenderEngine::GenerateBitmap(this, pControl, pControl->GetPos());
				}

				// 开启捕获
				SetCapture();
				// 事件处理
				m_pEventClick = pControl;
				pControl->SetFocus();

				TEventUI event = { 0 };
				event.Type = UIEVENT_BUTTONDOWN;
				event.pSender = pControl;
				event.wParam = wParam;
				event.lParam = lParam;
				event.ptMouse = pt;
				event.wKeyState = static_cast<WORD>(wParam);
				event.dwTimestamp = ::GetTickCount();
				pControl->Event(event);
			}
			break;
		case WM_LBUTTONDBLCLK:
			{
				if (!m_bNoActivate) ::SetFocus(m_hWndPaint);

				POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
				m_ptLastMousePos = pt;
				CControlUI* pControl = FindControl(pt);
				if( pControl == nullptr ) break;
				if( pControl->GetManager() != this ) break;
				//SetCapture();
				TEventUI event = { 0 };
				event.Type = UIEVENT_DBLCLICK;
				event.pSender = pControl;
				event.ptMouse = pt;
				event.wParam = wParam;
				event.lParam = lParam;
				event.wKeyState = static_cast<WORD>(wParam);
				event.dwTimestamp = ::GetTickCount();
				pControl->Event(event);
				m_pEventClick = pControl;
			}
			break;
		case WM_LBUTTONUP:
			{
				POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
				m_ptLastMousePos = pt;
				if( m_pEventClick == nullptr ) break;
				ReleaseCapture();
				TEventUI event = { 0 };
				event.Type = UIEVENT_BUTTONUP;
				event.pSender = m_pEventClick;
				event.wParam = wParam;
				event.lParam = lParam;
				event.ptMouse = pt;
				event.wKeyState = static_cast<WORD>(wParam);
				event.dwTimestamp = ::GetTickCount();

				CControlUI* pClick = m_pEventClick;
				m_pEventClick = nullptr;
				pClick->Event(event);
			}
			break;
		case WM_RBUTTONDOWN:
			{
				if (!m_bNoActivate) ::SetFocus(m_hWndPaint);
				POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
				m_ptLastMousePos = pt;
				CControlUI* pControl = FindControl(pt);
				if( pControl == nullptr ) break;
				if( pControl->GetManager() != this ) break;
				pControl->SetFocus();
				SetCapture();
				TEventUI event = { 0 };
				event.Type = UIEVENT_RBUTTONDOWN;
				event.pSender = pControl;
				event.wParam = wParam;
				event.lParam = lParam;
				event.ptMouse = pt;
				event.wKeyState = static_cast<WORD>(wParam);
				event.dwTimestamp = ::GetTickCount();
				pControl->Event(event);
				m_pEventRClick = pControl;
			}
			break;
		case WM_RBUTTONUP:
			{
				if(m_bMouseCapture) ReleaseCapture();

				POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
				m_ptLastMousePos = pt;
				m_pEventRClick = FindControl(pt);
				if(m_pEventRClick == nullptr) break;

				TEventUI event = { 0 };
				event.Type = UIEVENT_RBUTTONUP;
				event.pSender = m_pEventRClick;
				event.wParam = wParam;
				event.lParam = lParam;
				event.ptMouse = pt;
				event.wKeyState = static_cast<WORD>(wParam);
				event.dwTimestamp = ::GetTickCount();
				m_pEventRClick->Event(event);
			}
			break;
		case WM_MBUTTONDOWN:
			{
				if (!m_bNoActivate) ::SetFocus(m_hWndPaint);
				POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
				m_ptLastMousePos = pt;
				CControlUI* pControl = FindControl(pt);
				if( pControl == nullptr ) break;
				if( pControl->GetManager() != this ) break;
				pControl->SetFocus();
				SetCapture();
				TEventUI event = { 0 };
				event.Type = UIEVENT_MBUTTONDOWN;
				event.pSender = pControl;
				event.wParam = wParam;
				event.lParam = lParam;
				event.ptMouse = pt;
				event.wKeyState = static_cast<WORD>(wParam);
				event.dwTimestamp = ::GetTickCount();
				pControl->Event(event);
				m_pEventClick = pControl;
			}
			break;
		case WM_MBUTTONUP:
			{
				if(m_bMouseCapture) ReleaseCapture();
				POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
				m_ptLastMousePos = pt;
				m_pEventClick = FindControl(pt);
				if(m_pEventClick == nullptr) break;

				TEventUI event = { 0 };
				event.Type = UIEVENT_MBUTTONUP;
				event.pSender = m_pEventClick;
				event.wParam = wParam;
				event.lParam = lParam;
				event.ptMouse = pt;
				event.wKeyState = static_cast<WORD>(wParam);
				event.dwTimestamp = ::GetTickCount();
				m_pEventClick->Event(event);
			}
			break;
		case WM_CONTEXTMENU:
			{
				if( m_pRoot == nullptr ) break;
				POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
				::ScreenToClient(m_hWndPaint, &pt);
				m_ptLastMousePos = pt;
				if( m_pEventRClick == nullptr ) break;
				ReleaseCapture();
				TEventUI event = { 0 };
				event.Type = UIEVENT_CONTEXTMENU;
				event.pSender = m_pEventRClick;
				event.wParam = wParam;
				event.lParam = lParam;
				event.ptMouse = pt;
				event.wKeyState = static_cast<WORD>(wParam);
				event.lParam = reinterpret_cast<LPARAM>(m_pEventRClick);
				event.dwTimestamp = ::GetTickCount();
				m_pEventRClick->Event(event);
				m_pEventRClick = nullptr;
			}
			break;
		case WM_TOUCH:
			{
				if(!m_bTouchSupported) {
					return 0;
				}

				UINT nTouchCount = LOWORD(wParam); // 触摸点数量
				PTOUCHINPUT pTouchInputs = new TOUCHINPUT[nTouchCount];
				if (!pTouchInputs) {
					if (pCloseTouchInputHandle) pCloseTouchInputHandle(reinterpret_cast<HTOUCHINPUT>(lParam));
					return 0;
				}

				LRESULT lResult = 0;
				// 动态调用GetTouchInputInfo（避免XP下未定义）
				if (pGetTouchInputInfo(reinterpret_cast<HTOUCHINPUT>(lParam), nTouchCount, pTouchInputs, sizeof(TOUCHINPUT)))
				{
					for (UINT i = 0; i < nTouchCount; ++i)
					{
						const TOUCHINPUT& ti = pTouchInputs[i];

						// 仅处理"触摸移动"事件（忽略按下/抬起/取消）
						if (!(ti.dwFlags & TOUCHEVENTF_MOVE))
							continue;

						// 1. 转换触摸坐标：触摸坐标单位是「百分之一像素」，需转为普通像素
						POINT ptTouch = {
							GET_X_LPARAM(ti.x) / 100,
							GET_Y_LPARAM(ti.y) / 100
						};
						// 2. 屏幕坐标转客户区坐标（与鼠标消息坐标统一）
						ScreenToClient(m_hWndPaint, &ptTouch);

						// 3. 首次触摸：记录坐标后跳过
						if (m_ptLastTouch.x == 0 && m_ptLastTouch.y == 0)
						{
							m_ptLastTouch = ptTouch;
							continue;
						}

						// 4. 计算垂直移动增量（过滤<8像素的微小移动，避免误触发）
						int nDeltaY = m_ptLastTouch.y - ptTouch.y;
						if (abs(nDeltaY) < 8)
							continue;

						// 5. 转换为滚轮增量（WHEEL_DELTA=120是系统默认滚轮步长）
						int nWheelDelta = (nDeltaY > 0) ? WHEEL_DELTA : -WHEEL_DELTA;

						// 6. 构造WM_MOUSEWHEEL消息参数
						WPARAM wWheelParam = MAKEWPARAM(0, nWheelDelta); // 高16位=滚轮增量，低16位=按键状态
						LPARAM lWheelParam = MAKELPARAM(ptTouch.x, ptTouch.y); // 客户区坐标

						// 7. 发送滚轮消息到绘制窗口
						::SendMessage(m_hWndPaint, WM_MOUSEWHEEL, wWheelParam, lWheelParam);

						// 8. 更新最后触摸坐标
						m_ptLastTouch = ptTouch;
					}

					// 释放触摸输入句柄（关键：否则会内存泄漏）
					pCloseTouchInputHandle(reinterpret_cast<HTOUCHINPUT>(lParam));
				}

				// 释放触摸数据缓冲区
				delete[] pTouchInputs;
				break;
			}
		case WM_MOUSEWHEEL:
			{
				if( m_pRoot == nullptr ) break;
				POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
				::ScreenToClient(m_hWndPaint, &pt);
				m_ptLastMousePos = pt;
				CControlUI* pControl = FindControl(pt);
				if( pControl == nullptr ) break;
				if( pControl->GetManager() != this ) break;
				int zDelta = static_cast<int>(static_cast<short>(HIWORD(wParam)));
				TEventUI event = { 0 };
				event.Type = UIEVENT_SCROLLWHEEL;
				event.pSender = pControl;
				event.wParam = MAKEWPARAM(zDelta < 0 ? SB_LINEDOWN : SB_LINEUP, zDelta);
				event.lParam = lParam;
				event.ptMouse = pt;
				event.wKeyState = MapKeyState();
				event.dwTimestamp = ::GetTickCount();
				pControl->Event(event);

				// Let's make sure that the scroll item below the cursor is the same as before...
				::SendMessage(m_hWndPaint, WM_MOUSEMOVE, 0, static_cast<LPARAM>(MAKELPARAM(m_ptLastMousePos.x, m_ptLastMousePos.y)));
			}
			break;
		case WM_CHAR:
			{
				if( m_pRoot == nullptr ) break;
				if( m_pFocus == nullptr ) break;
				TEventUI event = { 0 };
				event.Type = UIEVENT_CHAR;
				event.pSender = m_pFocus;
				event.wParam = wParam;
				event.lParam = lParam;
				event.chKey = static_cast<TCHAR>(wParam);
				event.ptMouse = m_ptLastMousePos;
				event.wKeyState = MapKeyState();
				event.dwTimestamp = ::GetTickCount();
				m_pFocus->Event(event);
			}
			break;
		case WM_KEYDOWN:
			{
				if( m_pRoot == nullptr ) break;
				if( m_pFocus == nullptr ) break;
				TEventUI event = { 0 };
				event.Type = UIEVENT_KEYDOWN;
				event.pSender = m_pFocus;
				event.wParam = wParam;
				event.lParam = lParam;
				event.chKey = static_cast<TCHAR>(wParam);
				event.ptMouse = m_ptLastMousePos;
				event.wKeyState = MapKeyState();
				event.dwTimestamp = ::GetTickCount();
				m_pFocus->Event(event);
				m_pEventKey = m_pFocus;
			}
			break;
		case WM_KEYUP:
			{
				if( m_pRoot == nullptr ) break;
				if( m_pEventKey == nullptr ) break;
				TEventUI event = { 0 };
				event.Type = UIEVENT_KEYUP;
				event.pSender = m_pEventKey;
				event.wParam = wParam;
				event.lParam = lParam;
				event.chKey = static_cast<TCHAR>(wParam);
				event.ptMouse = m_ptLastMousePos;
				event.wKeyState = MapKeyState();
				event.dwTimestamp = ::GetTickCount();
				m_pEventKey->Event(event);
				m_pEventKey = nullptr;
			}
			break;
		case WM_SETCURSOR:
			{
				if( m_pRoot == nullptr ) break;
				if( LOWORD(lParam) != HTCLIENT ) break;
				if( m_bMouseCapture ) return true;

				POINT pt = { 0 };
				::GetCursorPos(&pt);
				::ScreenToClient(m_hWndPaint, &pt);
				CControlUI* pControl = FindControl(pt);
				if( pControl == nullptr ) break;
				if( (pControl->GetControlFlags() & UIFLAG_SETCURSOR) == 0 ) break;
				TEventUI event = { 0 };
				event.Type = UIEVENT_SETCURSOR;
				event.pSender = pControl;
				event.wParam = wParam;
				event.lParam = lParam;
				event.ptMouse = pt;
				event.wKeyState = MapKeyState();
				event.dwTimestamp = ::GetTickCount();
				pControl->Event(event);
			}
			return true;
		case WM_SETFOCUS:
			{
				if( m_pFocus != nullptr ) {
					TEventUI event = { 0 };
					event.Type = UIEVENT_SETFOCUS;
					event.wParam = wParam;
					event.lParam = lParam;
					event.pSender = m_pFocus;
					event.dwTimestamp = ::GetTickCount();
					m_pFocus->Event(event);
				}
				break;
			}
		case WM_KILLFOCUS:
			{
				if(IsCaptured()) ReleaseCapture();
				break;
			}
		case WM_NOTIFY:
			{
				if( lParam == 0 ) break;
				LPNMHDR lpNMHDR = reinterpret_cast<LPNMHDR>(lParam);
				if( lpNMHDR != nullptr ) lRes = ::SendMessage(lpNMHDR->hwndFrom, OCM__BASE + uMsg, wParam, lParam);
				return true;
			}
			break;
		case WM_COMMAND:
			{
				if( lParam == 0 ) break;
				HWND hWndChild = reinterpret_cast<HWND>(lParam);
				lRes = ::SendMessage(hWndChild, OCM__BASE + uMsg, wParam, lParam);
				if(lRes != 0) return true;
			}
			break;
		case WM_CTLCOLOREDIT:
		case WM_CTLCOLORSTATIC:
			{
				// Refer To: http://msdn.microsoft.com/en-us/library/bb761691(v=vs.85).aspx
				// Read-only or disabled edit controls do not send the WM_CTLCOLOREDIT message; instead, they send the WM_CTLCOLORSTATIC message.
				if( lParam == 0 ) break;
				HWND hWndChild = reinterpret_cast<HWND>(lParam);
				lRes = ::SendMessage(hWndChild, OCM__BASE + uMsg, wParam, lParam);
				if(lRes != 0) return true;
			}
			break;
		default:
			break;
		}
		return false;
	}

	bool CPaintManagerUI::IsUpdateNeeded() const
	{
		return m_bUpdateNeeded;
	}

	void CPaintManagerUI::NeedUpdate()
	{
		m_bUpdateNeeded = true;
	}

	void CPaintManagerUI::Invalidate()
	{
		DUI_ASSERT_UI_THREAD_FOR(this);
		RECT rcClient = { 0 };
		::GetClientRect(m_hWndPaint, &rcClient);
		::UnionRect(&m_rcLayeredUpdate, &m_rcLayeredUpdate, &rcClient);
		::InvalidateRect(m_hWndPaint, nullptr, FALSE);
	}

	void CPaintManagerUI::Invalidate(RECT& rcItem)
	{
		// [R72] The other overload has always had this guard; this one did not.
		// Guarding both is what makes "the invalidate path is UI-thread-only"
		// true rather than half-true: a background thread calling the rect
		// overload reaches the same m_rcLayeredUpdate members and the same
		// InvalidateRect, and used to do so with nothing watching.
		DUI_ASSERT_UI_THREAD_FOR(this);
		if( rcItem.left < 0 ) rcItem.left = 0;
		if( rcItem .top < 0 ) rcItem.top = 0;
		if( rcItem.right < rcItem.left ) rcItem.right = rcItem.left;
		if( rcItem.bottom < rcItem.top ) rcItem.bottom = rcItem.top;
		::UnionRect(&m_rcLayeredUpdate, &m_rcLayeredUpdate, &rcItem);
		::InvalidateRect(m_hWndPaint, &rcItem, FALSE);
	}

	bool CPaintManagerUI::IsValid()
	{
		return m_hWndPaint != nullptr && m_pRoot != nullptr;
	}

	bool CPaintManagerUI::AttachDialog(CControlUI* pControl)
	{
		ASSERT(::IsWindow(m_hWndPaint));
		// [THREAD-1] First attach declares which thread owns the UI. Every later
		// UI-thread-only entry point is checked against this.
		CThreadAffinity::ClaimUiThread(::GetCurrentThreadId());
		// [R-07] Remember which thread owns THIS manager. With only a process-wide
		// owner, a second window on a second UI thread was accused of violating
		// its own contract -- measured before the fix. The process-wide claim
		// above stays as the headless fallback only.
		if (!CThreadAffinity::RegisterUiThread(this, ::GetCurrentThreadId())) {
			// Two threads claimed one window: never legitimate. Record it but keep
			// the first owner, because reassigning would hide the bug.
			CThreadAffinity::RecordViolation();
		}
		
		// 创建阴影窗口
		m_shadow.Create(this);

		// Reset any previous attachment
		SetFocus(nullptr);
		m_pEventKey = nullptr;
		m_pEventHover = nullptr;
        m_pEventClick = nullptr;
        m_pEventRClick = nullptr;
		// Remove the existing control-tree. We might have gotten inside this function as
		// a result of an event fired or similar, so we cannot just delete the objects and
		// pull the internal memory of the calling code. We'll delay the cleanup.
		if( m_pRoot != nullptr ) {
			m_aPostPaintControls.Empty();
			AddDelayedCleanup(m_pRoot);
		}
		// Set the dialog root element
		m_pRoot = pControl;
		// Go ahead...
		m_bUpdateNeeded = true;
		m_bFirstLayout = true;
		m_bFocusNeeded = true;
		// Initiate all control
		return InitControls(pControl);
	}

	bool CPaintManagerUI::InitControls(CControlUI* pControl, CControlUI* pParent /*= nullptr*/)
	{
		ASSERT(pControl);
		if( pControl == nullptr ) return false;
		pControl->SetManager(this, pParent != nullptr ? pParent : pControl->GetParent(), true);
		pControl->FindControl(__FindControlFromNameHash, this, UIFIND_ALL);
		return true;
	}

	void CPaintManagerUI::ReapObjects(CControlUI* pControl)
	{
		if( pControl == m_pEventKey ) m_pEventKey = nullptr;
		if( pControl == m_pEventHover ) m_pEventHover = nullptr;
        if (pControl == m_pEventClick) m_pEventClick = nullptr;
        if (pControl == m_pEventRClick) m_pEventRClick = nullptr;
		if( pControl == m_pFocus ) m_pFocus = nullptr;
		KillTimer(pControl);
		const CDuiString& sName = pControl->GetName();
		if( !sName.IsEmpty() ) {
			if( pControl == FindControl(sName) ) m_mNameHash.Remove(sName);
		}
		for( int i = 0; i < m_aAsyncNotify.GetSize(); i++ ) {
			TNotifyUI* pMsg = static_cast<TNotifyUI*>(m_aAsyncNotify[i]);
			if( pMsg->pSender == pControl ) pMsg->pSender = nullptr;
		}    
	}

	bool CPaintManagerUI::AddOptionGroup(LPCTSTR pStrGroupName, CControlUI* pControl)
	{
		LPVOID lp = m_mOptionGroup.Find(pStrGroupName);
		if( lp ) {
			CStdPtrArray* aOptionGroup = static_cast<CStdPtrArray*>(lp);
			for( int i = 0; i < aOptionGroup->GetSize(); i++ ) {
				if( static_cast<CControlUI*>(aOptionGroup->GetAt(i)) == pControl ) {
					return false;
				}
			}
			aOptionGroup->Add(pControl);
		}
		else {
			CStdPtrArray* aOptionGroup = new CStdPtrArray(6);
			aOptionGroup->Add(pControl);
			m_mOptionGroup.Insert(pStrGroupName, aOptionGroup);
		}
		return true;
	}

	CStdPtrArray* CPaintManagerUI::GetOptionGroup(LPCTSTR pStrGroupName)
	{
		LPVOID lp = m_mOptionGroup.Find(pStrGroupName);
		if( lp ) return static_cast<CStdPtrArray*>(lp);
		return nullptr;
	}

	void CPaintManagerUI::RemoveOptionGroup(LPCTSTR pStrGroupName, CControlUI* pControl)
	{
		LPVOID lp = m_mOptionGroup.Find(pStrGroupName);
		if( lp ) {
			CStdPtrArray* aOptionGroup = static_cast<CStdPtrArray*>(lp);
			if( aOptionGroup == nullptr ) return;
			for( int i = 0; i < aOptionGroup->GetSize(); i++ ) {
				if( static_cast<CControlUI*>(aOptionGroup->GetAt(i)) == pControl ) {
					aOptionGroup->Remove(i);
					break;
				}
			}
			if( aOptionGroup->IsEmpty() ) {
				delete aOptionGroup;
				m_mOptionGroup.Remove(pStrGroupName);
			}
		}
	}

	void CPaintManagerUI::RemoveAllOptionGroups()
	{
		CStdPtrArray* aOptionGroup;
		for( int i = 0; i< m_mOptionGroup.GetSize(); i++ ) {
			if(LPCTSTR key = m_mOptionGroup.GetAt(i)) {
				aOptionGroup = static_cast<CStdPtrArray*>(m_mOptionGroup.Find(key));
				delete aOptionGroup;
			}
		}
		m_mOptionGroup.RemoveAll();
	}

	void CPaintManagerUI::MessageLoop()
	{
		MSG msg = { 0 };
		while( ::GetMessage(&msg, nullptr, 0, 0) ) {
			if( !CPaintManagerUI::TranslateMessage(&msg) ) {
				::TranslateMessage(&msg);
				try{
					::DispatchMessage(&msg);
				} catch(...) {
					// [SEC-25] Re-throwing a string literal from the message loop
					// escapes every catch(const std::exception&) in the host and
					// terminates the process. An exception raised while dispatching
					// one message must not kill the loop: log it and continue.
					DUITRACE(_T("EXCEPTION in DispatchMessage: %s(%d)\n"), __FILET__, __LINE__);
					::OutputDebugStringA("[DuiLib] exception escaped DispatchMessage (handled)\n");
				}
			}
		}
	}

	void CPaintManagerUI::Term()
	{
		// 销毁资源管理器
		CResourceManager::ReleaseScope(nullptr);
		CControlFactory::GetInstance()->Release();

		// 清理共享资源
		// 图片
		TImageInfo* data;
		for( int i = 0; i< m_SharedResInfo.m_ImageHash.GetSize(); i++ ) {
			if(LPCTSTR key = m_SharedResInfo.m_ImageHash.GetAt(i)) {
				data = static_cast<TImageInfo*>(m_SharedResInfo.m_ImageHash.Find(key, false));
				if (data) {
					CRenderEngine::FreeImage(data);
					data = nullptr;
				}
			}
		}
		m_SharedResInfo.m_ImageHash.RemoveAll();
		// 字体
		TFontInfo* pFontInfo;
		for( int i = 0; i< m_SharedResInfo.m_CustomFonts.GetSize(); i++ ) {
			if(LPCTSTR key = m_SharedResInfo.m_CustomFonts.GetAt(i)) {
				pFontInfo = static_cast<TFontInfo*>(m_SharedResInfo.m_CustomFonts.Find(key, false));
				if (pFontInfo) {
					::DeleteObject(pFontInfo->hFont);
					delete pFontInfo;
					pFontInfo = nullptr;
				}
			}
		}
		m_SharedResInfo.m_CustomFonts.RemoveAll();
		// 默认字体
		if(m_SharedResInfo.m_DefaultFontInfo.hFont != nullptr) {
			::DeleteObject(m_SharedResInfo.m_DefaultFontInfo.hFont);
		}
		// 样式
		CDuiString* pStyle;
		for( int i = 0; i< m_SharedResInfo.m_StyleHash.GetSize(); i++ ) {
			if(LPCTSTR key = m_SharedResInfo.m_StyleHash.GetAt(i)) {
				pStyle = static_cast<CDuiString*>(m_SharedResInfo.m_StyleHash.Find(key, false));
				if (pStyle) {
					delete pStyle;
					pStyle = nullptr;
				}
			}
		}
		m_SharedResInfo.m_StyleHash.RemoveAll();

		// 样式
		CDuiString* pAttr;
		for( int i = 0; i< m_SharedResInfo.m_AttrHash.GetSize(); i++ ) {
			if(LPCTSTR key = m_SharedResInfo.m_AttrHash.GetAt(i)) {
				pAttr = static_cast<CDuiString*>(m_SharedResInfo.m_AttrHash.Find(key, false));
				if (pAttr) {
					delete pAttr;
					pAttr = nullptr;
				}
			}
		}
		m_SharedResInfo.m_AttrHash.RemoveAll();

		// 关闭ZIP
		if( m_bCachedResourceZip && m_hResourceZip != nullptr ) {
			CloseZip(reinterpret_cast<HZIP>(m_hResourceZip));
			m_hResourceZip = nullptr;
		}

        if (m_cbZipBuf)
        {
            delete[] m_cbZipBuf;
            m_cbZipBuf = nullptr;
        }
	}

	CDPI * DuiLib::CPaintManagerUI::GetDPIObj()
	{
		if (m_pDPI == nullptr) {
			m_pDPI = new CDPI;
		}
		return m_pDPI;
	}

	void DuiLib::CPaintManagerUI::SetDPI(int iDPI)
	{
		// [H3] C++17 defensive: guard integer divide-by-zero on scale1/scale2
		int scale1 = GetDPIObj()->GetScale();
		GetDPIObj()->SetScale(iDPI);
		int scale2 = GetDPIObj()->GetScale();
		if (scale1 <= 0) { DUI_LOGW("SetDPI: invalid old scale %d, fallback 100", scale1); scale1 = 100; }
		if (scale2 <= 0) { DUI_LOGW("SetDPI: invalid new scale %d, fallback scale1", scale2); scale2 = scale1; }
		ResetDPIAssets();
		RECT rcWnd = {0};
		::GetWindowRect(GetPaintWindow(), &rcWnd);
		RECT*  prcNewWindow = &rcWnd;
		if (!::IsZoomed(GetPaintWindow())) {
			RECT rc = rcWnd;
			rc.right = rcWnd.left + (rcWnd.right - rcWnd.left) * scale2 / scale1;
			rc.bottom = rcWnd.top + (rcWnd.bottom - rcWnd.top) * scale2 / scale1;
			prcNewWindow = &rc;
		}
		SetWindowPos(GetPaintWindow(), nullptr, prcNewWindow->left, prcNewWindow->top, prcNewWindow->right - prcNewWindow->left, prcNewWindow->bottom - prcNewWindow->top, SWP_NOZORDER | SWP_NOACTIVATE);
		if (GetRoot() != nullptr) GetRoot()->NeedUpdate();
		::PostMessage(GetPaintWindow(), UIMSG_SET_DPI, 0, 0);
	}

	void DuiLib::CPaintManagerUI::SetAllDPI(int iDPI)
	{
		for (int i = 0; i < m_aPreMessages.GetSize(); i++) {
			CPaintManagerUI* pManager = static_cast<CPaintManagerUI*>(m_aPreMessages[i]);
			pManager->SetDPI(iDPI);
		}
	}

	void DuiLib::CPaintManagerUI::ResetDPIAssets()
	{
		RemoveAllDrawInfos();
		RemoveAllImages();;

		for (int it = 0; it < m_ResInfo.m_CustomFonts.GetSize(); it++) {
			TFontInfo* pFontInfo = static_cast<TFontInfo*>(m_ResInfo.m_CustomFonts.Find(m_ResInfo.m_CustomFonts[it]));
			RebuildFont(pFontInfo);
		}
		RebuildFont(&m_ResInfo.m_DefaultFontInfo);

		for (int it = 0; it < m_SharedResInfo.m_CustomFonts.GetSize(); it++) {
			TFontInfo* pFontInfo = static_cast<TFontInfo*>(m_SharedResInfo.m_CustomFonts.Find(m_SharedResInfo.m_CustomFonts[it]));
			RebuildFont(pFontInfo);
		}
		RebuildFont(&m_SharedResInfo.m_DefaultFontInfo);

		CStdPtrArray *richEditList = FindSubControlsByClass(GetRoot(), _T("RichEditUI"));
		for (int i = 0; i < richEditList->GetSize(); i++)
		{
			CRichEditUI* pT = static_cast<CRichEditUI*>((*richEditList)[i]);
			pT->SetFont(pT->GetFont());

		}
	}

	void DuiLib::CPaintManagerUI::RebuildFont(TFontInfo * pFontInfo)
	{
		::DeleteObject(pFontInfo->hFont);
		LOGFONT lf = { 0 };
		::GetObject(::GetStockObject(DEFAULT_GUI_FONT), sizeof(LOGFONT), &lf);
		_tcsncpy(lf.lfFaceName, pFontInfo->sFontName, LF_FACESIZE);
		lf.lfCharSet = DEFAULT_CHARSET;
		lf.lfHeight = -GetDPIObj()->Scale(pFontInfo->iSize);
		lf.lfQuality = CLEARTYPE_QUALITY;
		if (pFontInfo->bBold) lf.lfWeight += FW_BOLD;
		if (pFontInfo->bUnderline) lf.lfUnderline = TRUE;
		if (pFontInfo->bItalic) lf.lfItalic = TRUE;
		HFONT hFont = ::CreateFontIndirect(&lf);
		pFontInfo->hFont = hFont;
		::ZeroMemory(&(pFontInfo->tm), sizeof(pFontInfo->tm));
		if (m_hDcPaint) {
			HFONT hOldFont = reinterpret_cast<HFONT>(::SelectObject(m_hDcPaint, hFont));
			::GetTextMetrics(m_hDcPaint, &pFontInfo->tm);
			::SelectObject(m_hDcPaint, hOldFont);
		}
	}

	CControlUI* CPaintManagerUI::GetFocus() const
	{
		return m_pFocus;
	}

	void CPaintManagerUI::SetFocus(CControlUI* pControl)
	{
		DUI_ASSERT_UI_THREAD_FOR(this);
		// Paint manager window has focus?
		HWND hFocusWnd = ::GetFocus();
		if( hFocusWnd != m_hWndPaint && pControl != m_pFocus ) ::SetFocus(m_hWndPaint);
		// Already has focus?
		if( pControl == m_pFocus ) return;
		// Remove focus from old control
		if( m_pFocus != nullptr ) 
		{
			TEventUI event = { 0 };
			event.Type = UIEVENT_KILLFOCUS;
			event.pSender = pControl;
			event.dwTimestamp = ::GetTickCount();
			m_pFocus->Event(event);
			SendNotify(m_pFocus, DUI_MSGTYPE_KILLFOCUS);
			m_pFocus = nullptr;
		}
		if( pControl == nullptr ) return;
		// Set focus to new control
		if( pControl != nullptr 
			&& pControl->GetManager() == this 
			&& pControl->IsVisible() 
			&& pControl->IsEnabled() 
			&& (pControl->GetControlFlags() & UIFLAG_TABSTOP) != 0 ) 
		{
			m_pFocus = pControl;
			TEventUI event = { 0 };
			event.Type = UIEVENT_SETFOCUS;
			event.pSender = pControl;
			event.dwTimestamp = ::GetTickCount();
			m_pFocus->Event(event);
			SendNotify(m_pFocus, DUI_MSGTYPE_SETFOCUS);
		}
	}

	bool CPaintManagerUI::RelocateFocusFrom(CControlUI* pControl)
	{
		if (pControl == nullptr || m_pFocus != pControl || m_pRoot == nullptr) return false;

		// Walk the tab order starting at the affected control. Any candidate must
		// still be usable, so a hidden/disabled control cannot inherit focus.
		FINDTABINFO info = { 0 };
		info.pFocus = pControl;
		info.bForward = true;
		CControlUI* pNext = m_pRoot->FindControl(__FindControlFromTab, &info, UIFIND_VISIBLE | UIFIND_ENABLED | UIFIND_ME_FIRST);
		if (pNext == nullptr) {
			FINDTABINFO wrap = { 0 };
			wrap.pFocus = nullptr;
			wrap.bForward = true;
			pNext = m_pRoot->FindControl(__FindControlFromTab, &wrap, UIFIND_VISIBLE | UIFIND_ENABLED | UIFIND_ME_FIRST);
		}
		if (pNext != nullptr && pNext != pControl
			&& pNext->IsVisible() && pNext->IsEnabled() && pNext->IsKeyboardEnabled()) {
			SetFocus(pNext);
			return true;
		}
		SetFocus(nullptr);
		return false;
	}

	void CPaintManagerUI::SetFocusNeeded(CControlUI* pControl)
	{
		::SetFocus(m_hWndPaint);
		if( pControl == nullptr ) return;
		if( m_pFocus != nullptr ) {
			TEventUI event = { 0 };
			event.Type = UIEVENT_KILLFOCUS;
			event.pSender = pControl;
			event.dwTimestamp = ::GetTickCount();
			m_pFocus->Event(event);
			SendNotify(m_pFocus, DUI_MSGTYPE_KILLFOCUS);
			m_pFocus = nullptr;
		}
		FINDTABINFO info = { 0 };
		info.pFocus = pControl;
		info.bForward = false;
		m_pFocus = m_pRoot->FindControl(__FindControlFromTab, &info, UIFIND_VISIBLE | UIFIND_ENABLED | UIFIND_ME_FIRST);
		m_bFocusNeeded = true;
		if( m_pRoot != nullptr ) m_pRoot->NeedUpdate();
	}

	bool CPaintManagerUI::SetTimer(CControlUI* pControl, UINT nTimerID, UINT uElapse)
	{
		ASSERT(pControl!=nullptr);
		ASSERT(uElapse>0);
		for( int i = 0; i< m_aTimers.GetSize(); i++ ) {
			TIMERINFO* pTimer = static_cast<TIMERINFO*>(m_aTimers[i]);
			if( pTimer->pSender == pControl
				&& pTimer->hWnd == m_hWndPaint
				&& pTimer->nLocalID == nTimerID ) {
					if( pTimer->bKilled == true ) {
						if( ::SetTimer(m_hWndPaint, pTimer->uWinTimer, uElapse, nullptr) ) {
							pTimer->bKilled = false;
							return true;
						}
						return false;
					}
					return false;
			}
		}

		m_uTimerID = (++m_uTimerID) % 0xF0; //0xf1-0xfe特殊用途
		if( !::SetTimer(m_hWndPaint, m_uTimerID, uElapse, nullptr) ) return FALSE;
		TIMERINFO* pTimer = new TIMERINFO;
		if( pTimer == nullptr ) return FALSE;
		pTimer->hWnd = m_hWndPaint;
		pTimer->pSender = pControl;
		pTimer->nLocalID = nTimerID;
		pTimer->uWinTimer = m_uTimerID;
		pTimer->bKilled = false;
		return m_aTimers.Add(pTimer);
	}

	bool CPaintManagerUI::KillTimer(CControlUI* pControl, UINT nTimerID)
	{
		ASSERT(pControl!=nullptr);
		for( int i = 0; i< m_aTimers.GetSize(); i++ ) {
			TIMERINFO* pTimer = static_cast<TIMERINFO*>(m_aTimers[i]);
			if( pTimer->pSender == pControl
				&& pTimer->hWnd == m_hWndPaint
				&& pTimer->nLocalID == nTimerID )
			{
				if( pTimer->bKilled == false ) {
					if( ::IsWindow(m_hWndPaint) ) ::KillTimer(pTimer->hWnd, pTimer->uWinTimer);
					pTimer->bKilled = true;
					return true;
				}
			}
		}
		return false;
	}

	void CPaintManagerUI::KillTimer(CControlUI* pControl)
	{
		ASSERT(pControl!=nullptr);
		int count = m_aTimers.GetSize();
		for( int i = 0, j = 0; i < count; i++ ) {
			TIMERINFO* pTimer = static_cast<TIMERINFO*>(m_aTimers[i - j]);
			if( pTimer->pSender == pControl && pTimer->hWnd == m_hWndPaint ) {
				if( pTimer->bKilled == false ) ::KillTimer(pTimer->hWnd, pTimer->uWinTimer);
				delete pTimer;
				m_aTimers.Remove(i - j);
				j++;
			}
		}
	}

	void CPaintManagerUI::RemoveAllTimers()
	{
		for( int i = 0; i < m_aTimers.GetSize(); i++ ) {
			TIMERINFO* pTimer = static_cast<TIMERINFO*>(m_aTimers[i]);
			if( pTimer->hWnd == m_hWndPaint ) {
				if( pTimer->bKilled == false ) {
					if( ::IsWindow(m_hWndPaint) ) ::KillTimer(m_hWndPaint, pTimer->uWinTimer);
				}
				delete pTimer;
			}
		}

		m_aTimers.Empty();
	}

	void CPaintManagerUI::SetCapture()
	{
		::SetCapture(m_hWndPaint);
		m_bMouseCapture = true;
	}

	void CPaintManagerUI::ReleaseCapture()
	{
		::ReleaseCapture();
		m_bMouseCapture = false;
		m_bDragMode = false;
	}

	bool CPaintManagerUI::IsCaptured()
	{
		return m_bMouseCapture;
	}

	bool CPaintManagerUI::IsPainting()
	{
		return m_bIsPainting;
	}

	void CPaintManagerUI::SetPainting(bool bIsPainting)
	{
		m_bIsPainting = bIsPainting;
	}

	bool CPaintManagerUI::SetNextTabControl(bool bForward)
	{
		// If we're in the process of restructuring the layout we can delay the
		// focus calulation until the next repaint.
		if( m_bUpdateNeeded && bForward ) {
			m_bFocusNeeded = true;
			::InvalidateRect(m_hWndPaint, nullptr, FALSE);
			return true;
		}
		// Find next/previous tabbable control
		FINDTABINFO info1 = { 0 };
		info1.pFocus = m_pFocus;
		info1.bForward = bForward;
		CControlUI* pControl = m_pRoot->FindControl(__FindControlFromTab, &info1, UIFIND_VISIBLE | UIFIND_ENABLED | UIFIND_ME_FIRST);
		if( pControl == nullptr ) {  
			if( bForward ) {
				// Wrap around
				FINDTABINFO info2 = { 0 };
				info2.pFocus = bForward ? nullptr : info1.pLast;
				info2.bForward = bForward;
				pControl = m_pRoot->FindControl(__FindControlFromTab, &info2, UIFIND_VISIBLE | UIFIND_ENABLED | UIFIND_ME_FIRST);
			}
			else {
				pControl = info1.pLast;
			}
		}
		if( pControl != nullptr ) SetFocus(pControl);
		m_bFocusNeeded = false;
		return true;
	}

	bool CPaintManagerUI::AddNotifier(INotifyUI* pNotifier)
	{
		ASSERT(m_aNotifiers.Find(pNotifier)<0);
		return m_aNotifiers.Add(pNotifier);
	}

	bool CPaintManagerUI::RemoveNotifier(INotifyUI* pNotifier)
	{
		for( int i = 0; i < m_aNotifiers.GetSize(); i++ ) {
			if( static_cast<INotifyUI*>(m_aNotifiers[i]) == pNotifier ) {
				return m_aNotifiers.Remove(i);
			}
		}
		return false;
	}

	bool CPaintManagerUI::AddPreMessageFilter(IMessageFilterUI* pFilter)
	{
		ASSERT(m_aPreMessageFilters.Find(pFilter)<0);
		return m_aPreMessageFilters.Add(pFilter);
	}

	bool CPaintManagerUI::RemovePreMessageFilter(IMessageFilterUI* pFilter)
	{
		for( int i = 0; i < m_aPreMessageFilters.GetSize(); i++ ) {
			if( static_cast<IMessageFilterUI*>(m_aPreMessageFilters[i]) == pFilter ) {
				return m_aPreMessageFilters.Remove(i);
			}
		}
		return false;
	}

	bool CPaintManagerUI::AddMessageFilter(IMessageFilterUI* pFilter)
	{
		ASSERT(m_aMessageFilters.Find(pFilter)<0);
		return m_aMessageFilters.Add(pFilter);
	}

	bool CPaintManagerUI::RemoveMessageFilter(IMessageFilterUI* pFilter)
	{
		for( int i = 0; i < m_aMessageFilters.GetSize(); i++ ) {
			if( static_cast<IMessageFilterUI*>(m_aMessageFilters[i]) == pFilter ) {
				return m_aMessageFilters.Remove(i);
			}
		}
		return false;
	}

	int CPaintManagerUI::GetPostPaintCount() const
	{
		return m_aPostPaintControls.GetSize();
	}

	bool CPaintManagerUI::IsPostPaint(CControlUI* pControl)
	{
		return m_aPostPaintControls.Find(pControl) >= 0;
	}

	bool CPaintManagerUI::AddPostPaint(CControlUI* pControl)
	{
		ASSERT(m_aPostPaintControls.Find(pControl) < 0);
		return m_aPostPaintControls.Add(pControl);
	}

	bool CPaintManagerUI::RemovePostPaint(CControlUI* pControl)
	{
		for( int i = 0; i < m_aPostPaintControls.GetSize(); i++ ) {
			if( static_cast<CControlUI*>(m_aPostPaintControls[i]) == pControl ) {
				return m_aPostPaintControls.Remove(i);
			}
		}
		return false;
	}

	bool CPaintManagerUI::SetPostPaintIndex(CControlUI* pControl, int iIndex)
	{
		RemovePostPaint(pControl);
		return m_aPostPaintControls.InsertAt(iIndex, pControl);
	}

	int CPaintManagerUI::GetNativeWindowCount() const
	{
		return m_aNativeWindow.GetSize();
	}

	// [PERF-12] One cached surface per native child, rebuilt only when its size
	// changes. See UIManager.h for the ABI note: adding this member requires
	// recompiling every module that holds CPaintManagerUI by value.
	CPaintManagerUI::TNATIVE_SURFACE& CPaintManagerUI::GetNativeSurface(HWND hChildWnd, int nCx, int nCy)
	{
		TNATIVE_SURFACE& surf = m_mNativeSurfaces[hChildWnd];
		if( surf.hMemDC != nullptr && surf.nCx == nCx && surf.nCy == nCy ) return surf;

		if( surf.hMemDC != nullptr ) ::DeleteDC(surf.hMemDC);
		if( surf.hBitmap != nullptr ) ::DeleteObject(surf.hBitmap);
		surf = TNATIVE_SURFACE();

		surf.hMemDC = ::CreateCompatibleDC(nullptr);
		if( surf.hMemDC == nullptr ) return surf;
		surf.hBitmap = CRenderEngine::CreateARGB32Bitmap(surf.hMemDC, nCx, nCy,
		                                                 reinterpret_cast<BYTE**>(&surf.pBits));
		if( surf.hBitmap == nullptr ) {
			::DeleteDC(surf.hMemDC);
			surf.hMemDC = nullptr;
			return surf;
		}
		surf.nCx = nCx;
		surf.nCy = nCy;
		return surf;
	}

	void CPaintManagerUI::ReleaseNativeSurfaces()
	{
		for( auto& kv : m_mNativeSurfaces ) {
			if( kv.second.hBitmap != nullptr ) ::DeleteObject(kv.second.hBitmap);
			if( kv.second.hMemDC != nullptr ) ::DeleteDC(kv.second.hMemDC);
		}
		m_mNativeSurfaces.clear();
	}

	bool CPaintManagerUI::AddNativeWindow(CControlUI* pControl, HWND hChildWnd)
	{
		if (pControl == nullptr || hChildWnd == nullptr) return false;

		RECT rcChildWnd = GetNativeWindowRect(hChildWnd);
		Invalidate(rcChildWnd);

		if (m_aNativeWindow.Find(hChildWnd) >= 0) return false;
		if (m_aNativeWindow.Add(hChildWnd)) {
			m_aNativeWindowControl.Add(pControl);
			return true;
		}
		return false;
	}

	bool CPaintManagerUI::RemoveNativeWindow(HWND hChildWnd)
	{
		for( int i = 0; i < m_aNativeWindow.GetSize(); i++ ) {
			if( static_cast<HWND>(m_aNativeWindow[i]) == hChildWnd ) {
				if( m_aNativeWindow.Remove(i) ) {
					m_aNativeWindowControl.Remove(i);
					return true;
				}
				return false;
			}
		}
		return false;
	}

	RECT CPaintManagerUI::GetNativeWindowRect(HWND hChildWnd)
	{
		RECT rcChildWnd;
		::GetWindowRect(hChildWnd, &rcChildWnd);
		::ScreenToClient(m_hWndPaint, reinterpret_cast<LPPOINT>((&rcChildWnd)));
		::ScreenToClient(m_hWndPaint, reinterpret_cast<LPPOINT>((&rcChildWnd))+1);
		return rcChildWnd;
	}

	void CPaintManagerUI::AddDelayedCleanup(CControlUI* pControl)
	{
		if (pControl == nullptr) return;
		pControl->SetManager(this, nullptr, false);
		m_aDelayedCleanup.Add(pControl);
		PostAsyncNotify();
	}

	void CPaintManagerUI::AddMouseLeaveNeeded(CControlUI* pControl)
	{
		if (pControl == nullptr) return;
		for( int i = 0; i < m_aNeedMouseLeaveNeeded.GetSize(); i++ ) {
			if( static_cast<CControlUI*>(m_aNeedMouseLeaveNeeded[i]) == pControl ) {
				return;
			}
		}
		m_aNeedMouseLeaveNeeded.Add(pControl);
	}

	bool CPaintManagerUI::RemoveMouseLeaveNeeded(CControlUI* pControl)
	{
		if (pControl == nullptr) return false;
		for( int i = 0; i < m_aNeedMouseLeaveNeeded.GetSize(); i++ ) {
			if( static_cast<CControlUI*>(m_aNeedMouseLeaveNeeded[i]) == pControl ) {
				return m_aNeedMouseLeaveNeeded.Remove(i);
			}
		}
		return false;
	}

	void CPaintManagerUI::SendNotify(CControlUI* pControl, LPCTSTR pstrMessage, WPARAM wParam /*= 0*/, LPARAM lParam /*= 0*/, bool bAsync /*= false*/)
	{
		TNotifyUI Msg;
		Msg.pSender = pControl;
		Msg.sType = pstrMessage;
		Msg.wParam = wParam;
		Msg.lParam = lParam;
		SendNotify(Msg, bAsync);
	}

	void CPaintManagerUI::SendNotify(TNotifyUI& Msg, bool bAsync /*= false*/)
	{
		Msg.ptMouse = m_ptLastMousePos;
		Msg.dwTimestamp = ::GetTickCount();
		if( m_bUsedVirtualWnd )
		{
			Msg.sVirtualWnd = Msg.pSender->GetVirtualWnd();
		}

		if( !bAsync ) {
			// Send to all listeners
			if( Msg.pSender != nullptr ) {
				if( Msg.pSender->OnNotify ) Msg.pSender->OnNotify(&Msg);
			}
			for( int i = 0; i < m_aNotifiers.GetSize(); i++ ) {
				static_cast<INotifyUI*>(m_aNotifiers[i])->Notify(Msg);
			}
		}
		else {
			TNotifyUI *pMsg = new TNotifyUI;
			pMsg->sVirtualWnd = Msg.sVirtualWnd;
			pMsg->pSender = Msg.pSender;
			pMsg->sType = Msg.sType;
			pMsg->wParam = Msg.wParam;
			pMsg->lParam = Msg.lParam;
			pMsg->ptMouse = Msg.ptMouse;
			pMsg->dwTimestamp = Msg.dwTimestamp;
			m_aAsyncNotify.Add(pMsg);

			PostAsyncNotify();
		}
	}

	bool CPaintManagerUI::IsForceUseSharedRes() const
	{
		return m_bForceUseSharedRes;
	}

	void CPaintManagerUI::SetForceUseSharedRes(bool bForce)
	{
		m_bForceUseSharedRes = bForce;
	}

	DWORD CPaintManagerUI::GetDefaultDisabledColor() const
	{
		return m_ResInfo.m_dwDefaultDisabledColor;
	}

	// [R44] Process-wide default. Lives in a function-local static so the
	// initialisation order is well defined regardless of translation unit
	// layout (a namespace-scope bool would be a static-init-order hazard,
	// since constructors of CPaintManagerUI may run during app startup).
	static bool& AttrValidationDefault()
	{
		static bool s_bDefault = false;
		return s_bDefault;
	}

	void CPaintManagerUI::SetAttributeValidationDefault(bool bEnable)
	{
		AttrValidationDefault() = bEnable;
	}

	bool CPaintManagerUI::GetAttributeValidationDefault()
	{
		return AttrValidationDefault();
	}

// [R44] Single definition shared by both build flavours. It used to be
// declared once inside the _DEBUG branch and once in the Release branch,
// which made the name enumerator awkward to place: it needs the array but
// must not live behind the conditional. Hoisting the array removes the
// duplication and lets one accessor serve both.
static CStdPtrArray s_attr_val_warned;

// [R44] Enumerate recorded names. A count alone is not actionable: knowing
// "/AttrCheck found 1 problem" does not tell anyone which attribute to fix.
LPCTSTR CPaintManagerUI::GetAttributeValidationUnknownName(int iIndex)
{
	if (iIndex < 0 || iIndex >= s_attr_val_warned.GetSize()) return nullptr;
	return static_cast<LPCTSTR>(s_attr_val_warned.GetAt(iIndex));
}

#ifdef _DEBUG
	// N1: process-wide de-duplication. A single static CStdPtrArray holds the
	// already-warned attribute names so one misspelling is reported once per
	// process instead of once per parse cycle (see design note R3). Owned by
	// the library and released at shutdown is unnecessary: it is Debug-only
	// and at most a handful of entries.

	void CPaintManagerUI::SetAttributeValidationEnabled(bool bEnable)
	{
		m_bAttributeValidation = bEnable;
	}

	bool CPaintManagerUI::IsAttributeValidationEnabled() const
	{
		return m_bAttributeValidation;
	}

	void CPaintManagerUI::SetAttributeValidationEnabledForAll(bool bEnable)
	{
		CStdPtrArray* pManagers = GetPaintManagers();
		if (pManagers == nullptr) return;
		for (int i = 0; i < pManagers->GetSize(); ++i)
		{
			CPaintManagerUI* pManager = static_cast<CPaintManagerUI*>(pManagers->GetAt(i));
			if (pManager != nullptr) pManager->SetAttributeValidationEnabled(bEnable);
		}
	}

	bool CPaintManagerUI::IsAttributeValidationEnabledForAny()
	{
		CStdPtrArray* pManagers = GetPaintManagers();
		if (pManagers == nullptr) return false;
		for (int i = 0; i < pManagers->GetSize(); ++i)
		{
			CPaintManagerUI* pManager = static_cast<CPaintManagerUI*>(pManagers->GetAt(i));
			if (pManager != nullptr && pManager->IsAttributeValidationEnabled()) return true;
		}
		return false;
	}

	// Returns true and records the name on first sight; returns false for
	// every subsequent occurrence of the same attribute name.
	bool CPaintManagerUI::ReportAttributeValidationUnknown(LPCTSTR pstrName)
	{
		if (pstrName == nullptr || pstrName[0] == _T('\0')) return false;
		for (int i = 0; i < s_attr_val_warned.GetSize(); ++i)
		{
			LPCTSTR pstrPrev = static_cast<LPCTSTR>(s_attr_val_warned.GetAt(i));
			if (pstrPrev != nullptr && _tcsicmp(pstrPrev, pstrName) == 0) return false;
		}
		// [MIG] Store an owned copy, not the caller's pointer.
		// pstrName points into the skin parser's temporary buffer; keeping that
		// pointer meant the de-duplication compare and the public enumerator both
		// read reused memory, so the report printed a count with an empty name.
		const size_t cbName = (_tcslen(pstrName) + 1) * sizeof(TCHAR);
		TCHAR* pszCopy = static_cast<TCHAR*>(malloc(cbName));
		if (pszCopy == nullptr) return false;
		memcpy(pszCopy, pstrName, cbName);
		s_attr_val_warned.Add(pszCopy);
		return true;
	}

	int CPaintManagerUI::GetAttributeValidationUnknownCount()
	{
		return s_attr_val_warned.GetSize();
	}

	void CPaintManagerUI::ResetAttributeValidationWarnedCount()
	{
		// Entries are owned copies (see ReportAttributeValidationUnknown); free
		// them before dropping the array so a reset does not leak.
		for (int i = 0; i < s_attr_val_warned.GetSize(); ++i)
			free(s_attr_val_warned.GetAt(i));
		s_attr_val_warned.Empty();
	}

#else
	// [R43] Release used to have NO validation code at all -- every entry point
	// was an empty stub returning false/0. That made the class of defect this
	// project spent R35-R37 chasing structurally invisible: a skin could write
	// inset on a CTextUI, or misspell bkcolor as bkc0lor, and the shipped
	// (Release) binary stayed completely silent. Debug builds saw it; the build
	// users actually run did not.
	//
	// The fix keeps the original opt-in contract (default OFF, so normal runs
	// pay nothing) but makes the code compile in Release too. Cost when
	// enabled: one hash probe per distinct attribute name, then de-duplicated.

	void CPaintManagerUI::SetAttributeValidationEnabled(bool bEnable)
	{
		m_bAttributeValidation = bEnable;
	}

	bool CPaintManagerUI::IsAttributeValidationEnabled() const
	{
		return m_bAttributeValidation;
	}

	void CPaintManagerUI::SetAttributeValidationEnabledForAll(bool bEnable)
	{
		CStdPtrArray* pManagers = GetPaintManagers();
		if (pManagers == nullptr) return;
		for (int i = 0; i < pManagers->GetSize(); ++i)
		{
			CPaintManagerUI* pManager = static_cast<CPaintManagerUI*>(pManagers->GetAt(i));
			if (pManager != nullptr) pManager->SetAttributeValidationEnabled(bEnable);
		}
	}

	bool CPaintManagerUI::IsAttributeValidationEnabledForAny()
	{
		CStdPtrArray* pManagers = GetPaintManagers();
		if (pManagers == nullptr) return false;
		for (int i = 0; i < pManagers->GetSize(); ++i)
		{
			CPaintManagerUI* pManager = static_cast<CPaintManagerUI*>(pManagers->GetAt(i));
			if (pManager != nullptr && pManager->IsAttributeValidationEnabled()) return true;
		}
		return false;
	}

	bool CPaintManagerUI::ReportAttributeValidationUnknown(LPCTSTR pstrName)
	{
		if (pstrName == nullptr || pstrName[0] == _T('\0')) return false;
		for (int i = 0; i < s_attr_val_warned.GetSize(); ++i)
		{
			LPCTSTR pstrPrev = static_cast<LPCTSTR>(s_attr_val_warned.GetAt(i));
			if (pstrPrev != nullptr && _tcsicmp(pstrPrev, pstrName) == 0) return false;
		}
		// [MIG] Store an owned copy, not the caller's pointer.
		// pstrName points into the skin parser's temporary buffer; keeping that
		// pointer meant the de-duplication compare and the public enumerator both
		// read reused memory, so the report printed a count with an empty name.
		const size_t cbName = (_tcslen(pstrName) + 1) * sizeof(TCHAR);
		TCHAR* pszCopy = static_cast<TCHAR*>(malloc(cbName));
		if (pszCopy == nullptr) return false;
		memcpy(pszCopy, pstrName, cbName);
		s_attr_val_warned.Add(pszCopy);
		return true;
	}

	int CPaintManagerUI::GetAttributeValidationUnknownCount()
	{
		return s_attr_val_warned.GetSize();
	}

	void CPaintManagerUI::ResetAttributeValidationWarnedCount()
	{
		// Entries are owned copies (see ReportAttributeValidationUnknown); free
		// them before dropping the array so a reset does not leak.
		for (int i = 0; i < s_attr_val_warned.GetSize(); ++i)
			free(s_attr_val_warned.GetAt(i));
		s_attr_val_warned.Empty();
	}
#endif


	void CPaintManagerUI::SetDefaultDisabledColor(DWORD dwColor, bool bShared)
	{
		if (bShared)
		{
			if (m_ResInfo.m_dwDefaultDisabledColor == m_SharedResInfo.m_dwDefaultDisabledColor)
				m_ResInfo.m_dwDefaultDisabledColor = dwColor;
			m_SharedResInfo.m_dwDefaultDisabledColor = dwColor;
		}
		else
		{
			m_ResInfo.m_dwDefaultDisabledColor = dwColor;
		}
	}

	DWORD CPaintManagerUI::GetDefaultFontColor() const
	{
		return m_ResInfo.m_dwDefaultFontColor;
	}

	void CPaintManagerUI::SetDefaultFontColor(DWORD dwColor, bool bShared)
	{
		if (bShared)
		{
			if (m_ResInfo.m_dwDefaultFontColor == m_SharedResInfo.m_dwDefaultFontColor)
				m_ResInfo.m_dwDefaultFontColor = dwColor;
			m_SharedResInfo.m_dwDefaultFontColor = dwColor;
		}
		else
		{
			m_ResInfo.m_dwDefaultFontColor = dwColor;
		}
	}

	DWORD CPaintManagerUI::GetDefaultLinkFontColor() const
	{
		return m_ResInfo.m_dwDefaultLinkFontColor;
	}

	void CPaintManagerUI::SetDefaultLinkFontColor(DWORD dwColor, bool bShared)
	{
		if (bShared)
		{
			if (m_ResInfo.m_dwDefaultLinkFontColor == m_SharedResInfo.m_dwDefaultLinkFontColor)
				m_ResInfo.m_dwDefaultLinkFontColor = dwColor;
			m_SharedResInfo.m_dwDefaultLinkFontColor = dwColor;
		}
		else
		{
			m_ResInfo.m_dwDefaultLinkFontColor = dwColor;
		}
	}

	DWORD CPaintManagerUI::GetDefaultLinkHoverFontColor() const
	{
		return m_ResInfo.m_dwDefaultLinkHoverFontColor;
	}

	void CPaintManagerUI::SetDefaultLinkHoverFontColor(DWORD dwColor, bool bShared)
	{
		if (bShared)
		{
			if (m_ResInfo.m_dwDefaultLinkHoverFontColor == m_SharedResInfo.m_dwDefaultLinkHoverFontColor)
				m_ResInfo.m_dwDefaultLinkHoverFontColor = dwColor;
			m_SharedResInfo.m_dwDefaultLinkHoverFontColor = dwColor;
		}
		else
		{
			m_ResInfo.m_dwDefaultLinkHoverFontColor = dwColor;
		}
	}

	DWORD CPaintManagerUI::GetDefaultSelectedBkColor() const
	{
		return m_ResInfo.m_dwDefaultSelectedBkColor;
	}

	void CPaintManagerUI::SetDefaultSelectedBkColor(DWORD dwColor, bool bShared)
	{
		if (bShared)
		{
			if (m_ResInfo.m_dwDefaultSelectedBkColor == m_SharedResInfo.m_dwDefaultSelectedBkColor)
				m_ResInfo.m_dwDefaultSelectedBkColor = dwColor;
			m_SharedResInfo.m_dwDefaultSelectedBkColor = dwColor;
		}
		else
		{
			m_ResInfo.m_dwDefaultSelectedBkColor = dwColor;
		}
	}

	// [PERF-4] Measure a font once and remember it.
	//
	// The previous version only filled tm when tm.tmHeight == 0, but measured on
	// m_hDcPaint. When that DC is absent (manager without a paint window, or a
	// measurement taken before Init) SelectObject fails, tmHeight stays 0, and
	// EVERY call repeats SelectObject + GetTextMetrics. On the icon draw path that
	// measured ~110 us per call -- more than the glyph itself.
	static void MeasureFontOnceImpl(TFontInfo& fi, HDC hPaintDc)
	{
		if( fi.tm.tmHeight != 0 || fi.hFont == nullptr ) return;
		if( hPaintDc != nullptr ) {
			HFONT hOldFont = reinterpret_cast<HFONT>(::SelectObject(hPaintDc, fi.hFont));
			::GetTextMetrics(hPaintDc, &fi.tm);
			::SelectObject(hPaintDc, hOldFont);
		}
		if( fi.tm.tmHeight == 0 ) {
			HDC hScreen = ::GetDC(nullptr);
			if( hScreen != nullptr ) {
				HDC hMem = ::CreateCompatibleDC(hScreen);
				if( hMem != nullptr ) {
					HFONT hOldFont = reinterpret_cast<HFONT>(::SelectObject(hMem, fi.hFont));
					::GetTextMetrics(hMem, &fi.tm);
					::SelectObject(hMem, hOldFont);
					::DeleteDC(hMem);
				}
				::ReleaseDC(nullptr, hScreen);
			}
		}
	}
	TFontInfo* CPaintManagerUI::GetDefaultFontInfo()
	{
		if (m_ResInfo.m_DefaultFontInfo.sFontName.IsEmpty())
		{
			MeasureFontOnceImpl(m_SharedResInfo.m_DefaultFontInfo, m_hDcPaint);
			return &m_SharedResInfo.m_DefaultFontInfo;
		}
		else
		{
			MeasureFontOnceImpl(m_ResInfo.m_DefaultFontInfo, m_hDcPaint);
			return &m_ResInfo.m_DefaultFontInfo;
		}
	}

	void CPaintManagerUI::SetDefaultFont(LPCTSTR pStrFontName, int nSize, bool bBold, bool bUnderline, bool bItalic, bool bStrikeout, bool bShared)
	{
		LOGFONT lf = { 0 };
		::GetObject(::GetStockObject(DEFAULT_GUI_FONT), sizeof(LOGFONT), &lf);
		if(lstrlen(pStrFontName) > 0) {
			TCHAR szFaceName[32] = {0};//_T("@");
			_tcsncat(szFaceName, pStrFontName, LF_FACESIZE);
			_tcsncpy(lf.lfFaceName, szFaceName, LF_FACESIZE);
		}
		lf.lfCharSet = DEFAULT_CHARSET;
		lf.lfHeight = -GetDPIObj()->Scale(nSize);;
		if( bBold ) lf.lfWeight += FW_BOLD;
		if( bUnderline ) lf.lfUnderline = TRUE;
		if( bItalic ) lf.lfItalic = TRUE;
		if( bStrikeout ) lf.lfStrikeOut = TRUE;

		HFONT hFont = ::CreateFontIndirect(&lf);
		if( hFont == nullptr ) return;

		if (bShared)
		{
			::DeleteObject(m_SharedResInfo.m_DefaultFontInfo.hFont);
			m_SharedResInfo.m_DefaultFontInfo.hFont = hFont;
			m_SharedResInfo.m_DefaultFontInfo.sFontName = lf.lfFaceName;
			m_SharedResInfo.m_DefaultFontInfo.iSize =nSize;
			m_SharedResInfo.m_DefaultFontInfo.bBold = bBold;
			m_SharedResInfo.m_DefaultFontInfo.bUnderline = bUnderline;
			m_SharedResInfo.m_DefaultFontInfo.bItalic = bItalic;
			m_SharedResInfo.m_DefaultFontInfo.bStrikeout = bStrikeout;
			::ZeroMemory(&m_SharedResInfo.m_DefaultFontInfo.tm, sizeof(m_SharedResInfo.m_DefaultFontInfo.tm));
			if( m_hDcPaint ) {
				HFONT hOldFont = reinterpret_cast<HFONT>(::SelectObject(m_hDcPaint, hFont));
				::GetTextMetrics(m_hDcPaint, &m_SharedResInfo.m_DefaultFontInfo.tm);
				::SelectObject(m_hDcPaint, hOldFont);
			}
		}
		else
		{
			::DeleteObject(m_ResInfo.m_DefaultFontInfo.hFont);
			m_ResInfo.m_DefaultFontInfo.hFont = hFont;
			m_ResInfo.m_DefaultFontInfo.sFontName = lf.lfFaceName;
			m_ResInfo.m_DefaultFontInfo.iSize = nSize;
			m_ResInfo.m_DefaultFontInfo.bBold = bBold;
			m_ResInfo.m_DefaultFontInfo.bUnderline = bUnderline;
			m_ResInfo.m_DefaultFontInfo.bItalic = bItalic;
			m_ResInfo.m_DefaultFontInfo.bStrikeout = bStrikeout;
			::ZeroMemory(&m_ResInfo.m_DefaultFontInfo.tm, sizeof(m_ResInfo.m_DefaultFontInfo.tm));
			if( m_hDcPaint ) {
				HFONT hOldFont = reinterpret_cast<HFONT>(::SelectObject(m_hDcPaint, hFont));
				::GetTextMetrics(m_hDcPaint, &m_ResInfo.m_DefaultFontInfo.tm);
				::SelectObject(m_hDcPaint, hOldFont);
			}
		}
	}

	DWORD CPaintManagerUI::GetCustomFontCount(bool bShared) const
	{
		if (bShared)
			return m_SharedResInfo.m_CustomFonts.GetSize();
		else
			return m_ResInfo.m_CustomFonts.GetSize();
	}

	HFONT CPaintManagerUI::AddFont(int id, LPCTSTR pStrFontName, int nSize, bool bBold, bool bUnderline, bool bItalic, bool bStrikeout, bool bShared)
	{
		LOGFONT lf = { 0 };
		::GetObject(::GetStockObject(DEFAULT_GUI_FONT), sizeof(LOGFONT), &lf);
		if(lstrlen(pStrFontName) > 0) {
			TCHAR szFaceName[32] = {0};//_T("@");
			_tcsncat(szFaceName, pStrFontName, LF_FACESIZE);
			_tcsncpy(lf.lfFaceName, szFaceName, LF_FACESIZE);
		}
		lf.lfCharSet = DEFAULT_CHARSET;
		lf.lfHeight = -GetDPIObj()->Scale(nSize);
		// 字体抗锯齿：与 RebuildFont 保持一致，使用 ClearType 保证中文清晰不发虚
		lf.lfQuality = CLEARTYPE_QUALITY;
		if( bBold ) lf.lfWeight = FW_BOLD;
		if( bUnderline ) lf.lfUnderline = TRUE;
		if( bItalic ) lf.lfItalic = TRUE;
		if( bStrikeout ) lf.lfStrikeOut = TRUE;
		HFONT hFont = ::CreateFontIndirect(&lf);
		if( hFont == nullptr ) return nullptr;

		TFontInfo* pFontInfo = new TFontInfo;
		if( !pFontInfo ) return nullptr;
		::ZeroMemory(pFontInfo, sizeof(TFontInfo));
		pFontInfo->hFont = hFont;
		pFontInfo->sFontName = lf.lfFaceName;
		pFontInfo->iSize = nSize;
		pFontInfo->bBold = bBold;
		pFontInfo->bUnderline = bUnderline;
		pFontInfo->bItalic = bItalic;
		pFontInfo->bStrikeout = bStrikeout;
		if( m_hDcPaint ) {
			HFONT hOldFont = reinterpret_cast<HFONT>(::SelectObject(m_hDcPaint, hFont));
			::GetTextMetrics(m_hDcPaint, &pFontInfo->tm);
			::SelectObject(m_hDcPaint, hOldFont);
		}
		TCHAR idBuffer[16];
		::ZeroMemory(idBuffer, sizeof(idBuffer));
		_itot(id, idBuffer, 10);
		if (bShared || m_bForceUseSharedRes)
		{
			TFontInfo* pOldFontInfo = static_cast<TFontInfo*>(m_SharedResInfo.m_CustomFonts.Find(idBuffer));
			if (pOldFontInfo)
			{
				::DeleteObject(pOldFontInfo->hFont);
				delete pOldFontInfo;
				m_SharedResInfo.m_CustomFonts.Remove(idBuffer);
			}

			if( !m_SharedResInfo.m_CustomFonts.Insert(idBuffer, pFontInfo) ) 
			{
				::DeleteObject(hFont);
				delete pFontInfo;
				return nullptr;
			}
		}
		else
		{
			TFontInfo* pOldFontInfo = static_cast<TFontInfo*>(m_ResInfo.m_CustomFonts.Find(idBuffer));
			if (pOldFontInfo)
			{
				::DeleteObject(pOldFontInfo->hFont);
				delete pOldFontInfo;
				m_ResInfo.m_CustomFonts.Remove(idBuffer);
			}

			if( !m_ResInfo.m_CustomFonts.Insert(idBuffer, pFontInfo) ) 
			{
				::DeleteObject(hFont);
				delete pFontInfo;
				return nullptr;
			}
		}

		return hFont;
	}
	// [SEC-09] Resource files are read into a size-proportional allocation.
	// UIRender's zip path already enforces a 64 MB ceiling (kMaxImageBytes);
	// this path and the zip branch below did not, so a corrupt or hostile
	// resource pack could request an arbitrary allocation (OOM / DoS).
	static constexpr DWORD kMaxFontFileBytes = 64u * 1024u * 1024u;

	void CPaintManagerUI::AddFontArray(LPCTSTR pstrPath) {
		LPBYTE pData = nullptr;
		DWORD dwSize = 0;
		do
		{
			CDuiString sFile = CPaintManagerUI::GetResourcePath();
			if (CPaintManagerUI::GetResourceZip().IsEmpty()) {
				sFile += pstrPath;
				HANDLE hFile = ::CreateFile(sFile.GetData(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, \
					FILE_ATTRIBUTE_NORMAL, nullptr);
				if (hFile == INVALID_HANDLE_VALUE) break;
				dwSize = ::GetFileSize(hFile, nullptr);
				if (dwSize == 0) break;
				if (dwSize > kMaxFontFileBytes) { // [SEC-09]
					::CloseHandle(hFile);
					DUI_LOGE("AddFontArray: '%s' too large (%u bytes)", pstrPath, dwSize);
					break;
				}

				DWORD dwRead = 0;
				pData = new BYTE[dwSize];
				::ReadFile(hFile, pData, dwSize, &dwRead, nullptr);
				::CloseHandle(hFile);

				if (dwRead != dwSize) {
					delete[] pData;
					pData = nullptr;
					break;
				}
			}
			else {
				sFile += CPaintManagerUI::GetResourceZip();
				HZIP hz = nullptr;
				if (CPaintManagerUI::IsCachedResourceZip()) hz = reinterpret_cast<HZIP>(CPaintManagerUI::GetResourceZipHandle());
				else {
					CDuiString sFilePwd = CPaintManagerUI::GetResourceZipPwd();
#ifdef UNICODE
					std::unique_ptr<char[]> pwd{ w2a(const_cast<wchar_t*>(sFilePwd.GetData())) }; // [R4] RAII
					hz = OpenZip(sFile.GetData(), pwd.get());
#else
					hz = OpenZip(sFile.GetData(), sFilePwd.GetData());
#endif
				}
				if (hz == nullptr) break;
				ZIPENTRY ze;
				int i = 0;
				CDuiString key = pstrPath;
				key.Replace(_T("\\"), _T("/"));
				if (FindZipItem(hz, key, true, &i, &ze) != 0) {
					if (!CPaintManagerUI::IsCachedResourceZip()) CloseZip(hz);
					break;
				}
				dwSize = ze.unc_size;
				if (dwSize == 0) {
					if (!CPaintManagerUI::IsCachedResourceZip()) CloseZip(hz);
					break;
				}
				if (dwSize > kMaxFontFileBytes) { // [SEC-09]
					DUI_LOGE("AddFontArray: zip entry '%s' too large (%u bytes)", key.GetData(), dwSize);
					if (!CPaintManagerUI::IsCachedResourceZip()) CloseZip(hz);
					break;
				}
				pData = new BYTE[dwSize];
				int res = UnzipItem(hz, i, pData, dwSize);
				if (res != 0x00000000 && res != 0x00000600) {
					delete[] pData;
					pData = nullptr;
					if (!CPaintManagerUI::IsCachedResourceZip()) CloseZip(hz);
					break;
				}
				if (!CPaintManagerUI::IsCachedResourceZip()) CloseZip(hz);
			}

		} while (0);

		while (!pData)
		{
			//读不到图片, 则直接去读取bitmap.m_lpstr指向的路径
			HANDLE hFile = ::CreateFile(pstrPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (hFile == INVALID_HANDLE_VALUE) break;
			dwSize = ::GetFileSize(hFile, nullptr);
			if (dwSize == 0) break;

			DWORD dwRead = 0;
			pData = new BYTE[dwSize];
			::ReadFile(hFile, pData, dwSize, &dwRead, nullptr);
			::CloseHandle(hFile);

			if (dwRead != dwSize) {
				delete[] pData;
				pData = nullptr;
			}
			break;
		}
		DWORD nFonts = 0;
		HANDLE hFont = ::AddFontMemResourceEx(pData, dwSize, nullptr, &nFonts);
		delete[] pData;
		pData = nullptr;
		m_aFonts.Add(hFont);
	}
	HFONT CPaintManagerUI::GetFont(int id)
	{
		if (id < 0) return GetDefaultFontInfo()->hFont;

		TCHAR idBuffer[16];
		::ZeroMemory(idBuffer, sizeof(idBuffer));
		_itot(id, idBuffer, 10);
		TFontInfo* pFontInfo = static_cast<TFontInfo*>(m_ResInfo.m_CustomFonts.Find(idBuffer));
		if( !pFontInfo ) pFontInfo = static_cast<TFontInfo*>(m_SharedResInfo.m_CustomFonts.Find(idBuffer));
		if (!pFontInfo) return GetDefaultFontInfo()->hFont;
		MeasureFontOnceImpl(*pFontInfo, m_hDcPaint);
		return pFontInfo->hFont;
	}

	HFONT CPaintManagerUI::GetFont(LPCTSTR pStrFontName, int nSize, bool bBold, bool bUnderline, bool bItalic, bool bStrikeout)
	{
		TFontInfo* pFontInfo = nullptr;
		for( int i = 0; i< m_ResInfo.m_CustomFonts.GetSize(); i++ ) {
			if(LPCTSTR key = m_ResInfo.m_CustomFonts.GetAt(i)) {
				pFontInfo = static_cast<TFontInfo*>(m_ResInfo.m_CustomFonts.Find(key));
				if (pFontInfo && pFontInfo->sFontName == pStrFontName && pFontInfo->iSize == nSize && 
					pFontInfo->bBold == bBold && pFontInfo->bUnderline == bUnderline && pFontInfo->bItalic == bItalic
					&& pFontInfo->bStrikeout == bStrikeout)
					return pFontInfo->hFont;
			}
		}
		for( int i = 0; i< m_SharedResInfo.m_CustomFonts.GetSize(); i++ ) {
			if(LPCTSTR key = m_SharedResInfo.m_CustomFonts.GetAt(i)) {
				pFontInfo = static_cast<TFontInfo*>(m_SharedResInfo.m_CustomFonts.Find(key));
				if (pFontInfo && pFontInfo->sFontName == pStrFontName && pFontInfo->iSize == nSize && 
					pFontInfo->bBold == bBold && pFontInfo->bUnderline == bUnderline && pFontInfo->bItalic == bItalic
					&& pFontInfo->bStrikeout == bStrikeout)
					return pFontInfo->hFont;
			}
		}

		return nullptr;
	}

	int CPaintManagerUI::GetFontIndex(HFONT hFont, bool bShared)
	{
		TFontInfo* pFontInfo = nullptr;
		if (bShared)
		{
			for( int i = 0; i< m_SharedResInfo.m_CustomFonts.GetSize(); i++ ) {
				if(LPCTSTR key = m_SharedResInfo.m_CustomFonts.GetAt(i)) {
					pFontInfo = static_cast<TFontInfo*>(m_SharedResInfo.m_CustomFonts.Find(key));
					if (pFontInfo && pFontInfo->hFont == hFont) return _ttoi(key);
				}
			}
		}
		else
		{
			for( int i = 0; i< m_ResInfo.m_CustomFonts.GetSize(); i++ ) {
				if(LPCTSTR key = m_ResInfo.m_CustomFonts.GetAt(i)) {
					pFontInfo = static_cast<TFontInfo*>(m_ResInfo.m_CustomFonts.Find(key));
					if (pFontInfo && pFontInfo->hFont == hFont) return _ttoi(key);
				}
			}
		}

		return -1;
	}

	int CPaintManagerUI::GetFontIndex(LPCTSTR pStrFontName, int nSize, bool bBold, bool bUnderline, bool bItalic, bool bStrikeout, bool bShared)
	{
		TFontInfo* pFontInfo = nullptr;
		if (bShared)
		{
			for( int i = 0; i< m_SharedResInfo.m_CustomFonts.GetSize(); i++ ) {
				if(LPCTSTR key = m_SharedResInfo.m_CustomFonts.GetAt(i)) {
					pFontInfo = static_cast<TFontInfo*>(m_SharedResInfo.m_CustomFonts.Find(key));
					if (pFontInfo && pFontInfo->sFontName == pStrFontName && pFontInfo->iSize == nSize && 
						pFontInfo->bBold == bBold && pFontInfo->bUnderline == bUnderline && pFontInfo->bItalic == bItalic
						&& pFontInfo->bStrikeout == bStrikeout)
						return _ttoi(key);
				}
			}
		}
		else
		{
			for( int i = 0; i< m_ResInfo.m_CustomFonts.GetSize(); i++ ) {
				if(LPCTSTR key = m_ResInfo.m_CustomFonts.GetAt(i)) {
					pFontInfo = static_cast<TFontInfo*>(m_ResInfo.m_CustomFonts.Find(key));
					if (pFontInfo && pFontInfo->sFontName == pStrFontName && pFontInfo->iSize == nSize && 
						pFontInfo->bBold == bBold && pFontInfo->bUnderline == bUnderline && pFontInfo->bItalic == bItalic
						&& pFontInfo->bStrikeout == bStrikeout)
						return _ttoi(key);
				}
			}
		}

		return -1;
	}

	void CPaintManagerUI::RemoveFont(HFONT hFont, bool bShared)
	{
		TFontInfo* pFontInfo = nullptr;
		if (bShared)
		{
			for( int i = 0; i < m_SharedResInfo.m_CustomFonts.GetSize(); i++ ) 
			{
				if(LPCTSTR key = m_SharedResInfo.m_CustomFonts.GetAt(i)) 
				{
					pFontInfo = static_cast<TFontInfo*>(m_SharedResInfo.m_CustomFonts.Find(key));
					if (pFontInfo && pFontInfo->hFont == hFont) 
					{
						::DeleteObject(pFontInfo->hFont);
						delete pFontInfo;
						m_SharedResInfo.m_CustomFonts.Remove(key);
						return;
					}
				}
			}
		}
		else
		{
			for( int i = 0; i < m_ResInfo.m_CustomFonts.GetSize(); i++ ) 
			{
				if(LPCTSTR key = m_ResInfo.m_CustomFonts.GetAt(i)) 
				{
					pFontInfo = static_cast<TFontInfo*>(m_ResInfo.m_CustomFonts.Find(key));
					if (pFontInfo && pFontInfo->hFont == hFont) 
					{
						::DeleteObject(pFontInfo->hFont);
						delete pFontInfo;
						m_ResInfo.m_CustomFonts.Remove(key);
						return;
					}
				}
			}
		}
	}

	void CPaintManagerUI::RemoveFont(int id, bool bShared)
	{
		TCHAR idBuffer[16];
		::ZeroMemory(idBuffer, sizeof(idBuffer));
		_itot(id, idBuffer, 10);

		TFontInfo* pFontInfo = nullptr;
		if (bShared)
		{
			pFontInfo = static_cast<TFontInfo*>(m_SharedResInfo.m_CustomFonts.Find(idBuffer));
			if (pFontInfo)
			{
				::DeleteObject(pFontInfo->hFont);
				delete pFontInfo;
				m_SharedResInfo.m_CustomFonts.Remove(idBuffer);
			}
		}
		else
		{
			pFontInfo = static_cast<TFontInfo*>(m_ResInfo.m_CustomFonts.Find(idBuffer));
			if (pFontInfo)
			{
				::DeleteObject(pFontInfo->hFont);
				delete pFontInfo;
				m_ResInfo.m_CustomFonts.Remove(idBuffer);
			}
		}
	}

	void CPaintManagerUI::RemoveAllFonts(bool bShared)
	{
		TFontInfo* pFontInfo;
		if (bShared)
		{
			for( int i = 0; i< m_SharedResInfo.m_CustomFonts.GetSize(); i++ ) {
				if(LPCTSTR key = m_SharedResInfo.m_CustomFonts.GetAt(i)) {
					pFontInfo = static_cast<TFontInfo*>(m_SharedResInfo.m_CustomFonts.Find(key, false));
					if (pFontInfo) {
						::DeleteObject(pFontInfo->hFont);
						delete pFontInfo;
					}
				}
			}
			m_SharedResInfo.m_CustomFonts.RemoveAll();
		}
		else
		{
			for( int i = 0; i< m_ResInfo.m_CustomFonts.GetSize(); i++ ) {
				if(LPCTSTR key = m_ResInfo.m_CustomFonts.GetAt(i)) {
					pFontInfo = static_cast<TFontInfo*>(m_ResInfo.m_CustomFonts.Find(key, false));
					if (pFontInfo) {
						::DeleteObject(pFontInfo->hFont);
						delete pFontInfo;
					}
				}
			}
			m_ResInfo.m_CustomFonts.RemoveAll();
		}
	}

	TFontInfo* CPaintManagerUI::GetFontInfo(int id)
	{
		if (id < 0) return GetDefaultFontInfo();

		TCHAR idBuffer[16];
		::ZeroMemory(idBuffer, sizeof(idBuffer));
		_itot(id, idBuffer, 10);
		TFontInfo* pFontInfo = static_cast<TFontInfo*>(m_ResInfo.m_CustomFonts.Find(idBuffer));
		if (!pFontInfo) pFontInfo = static_cast<TFontInfo*>(m_SharedResInfo.m_CustomFonts.Find(idBuffer));
		if (!pFontInfo) pFontInfo = GetDefaultFontInfo();
		if (pFontInfo->tm.tmHeight == 0) 
		{
			HFONT hOldFont = reinterpret_cast<HFONT>(::SelectObject(m_hDcPaint, pFontInfo->hFont));
			::GetTextMetrics(m_hDcPaint, &pFontInfo->tm);
			::SelectObject(m_hDcPaint, hOldFont);
		}
		return pFontInfo;
	}

	TFontInfo* CPaintManagerUI::GetFontInfo(HFONT hFont)
	{
		TFontInfo* pFontInfo = nullptr;
		for( int i = 0; i< m_ResInfo.m_CustomFonts.GetSize(); i++ ) 
		{
			if(LPCTSTR key = m_ResInfo.m_CustomFonts.GetAt(i)) 
			{
				pFontInfo = static_cast<TFontInfo*>(m_ResInfo.m_CustomFonts.Find(key));
				if (pFontInfo && pFontInfo->hFont == hFont) break;
			}
		}
		if (!pFontInfo)
		{
			for( int i = 0; i< m_SharedResInfo.m_CustomFonts.GetSize(); i++ ) 
			{
				if(LPCTSTR key = m_SharedResInfo.m_CustomFonts.GetAt(i)) 
				{
					pFontInfo = static_cast<TFontInfo*>(m_SharedResInfo.m_CustomFonts.Find(key));
					if (pFontInfo && pFontInfo->hFont == hFont) break;
				}
			}
		}
		if (!pFontInfo) pFontInfo = GetDefaultFontInfo();
		if( pFontInfo->tm.tmHeight == 0 ) {
			HFONT hOldFont = reinterpret_cast<HFONT>(::SelectObject(m_hDcPaint, pFontInfo->hFont));
			::GetTextMetrics(m_hDcPaint, &pFontInfo->tm);
			::SelectObject(m_hDcPaint, hOldFont);
		}
		return pFontInfo;
	}

	const TImageInfo* CPaintManagerUI::GetImage(LPCTSTR bitmap)
	{
		TImageInfo* data = static_cast<TImageInfo*>(m_ResInfo.m_ImageHash.Find(bitmap));
		if( !data ) data = static_cast<TImageInfo*>(m_SharedResInfo.m_ImageHash.Find(bitmap));
		return data;
	}

	const TImageInfo* CPaintManagerUI::GetImageEx(LPCTSTR bitmap, LPCTSTR type, DWORD mask, bool bUseHSL, bool bGdiplus, HINSTANCE instance)
	{
		const TImageInfo* data = GetImage(bitmap);
		if( !data ) {
			if( AddImage(bitmap, type, mask, bUseHSL, bGdiplus, false, instance) ) {
				if (m_bForceUseSharedRes) data = static_cast<TImageInfo*>(m_SharedResInfo.m_ImageHash.Find(bitmap));
				else data = static_cast<TImageInfo*>(m_ResInfo.m_ImageHash.Find(bitmap)); 
			}
		}

		return data;
	}

	const TImageInfo* CPaintManagerUI::AddImage(LPCTSTR bitmap, LPCTSTR type, DWORD mask, bool bUseHSL, bool bGdiplus, bool bShared, HINSTANCE instance)
	{
		if( bitmap == nullptr || bitmap[0] == _T('\0') ) return nullptr;

		TImageInfo* data = nullptr;
		if( type != nullptr && lstrlen(type) > 0) {
			if( isdigit(*bitmap) ) {
				LPTSTR pstr = nullptr;
				int iIndex = _tcstol(bitmap, &pstr, 10);
				
				data = bGdiplus ? CRenderEngine::GdiplusLoadImage(iIndex, type, mask, instance) : CRenderEngine::LoadImage(iIndex, type, mask, instance);
			}
		}
		else {
			data = bGdiplus ? CRenderEngine::GdiplusLoadImage(bitmap, nullptr, mask, instance) : CRenderEngine::LoadImage(bitmap, nullptr, mask, instance);
			if(!data) {
				CDuiString sImageName = bitmap;
				int iAtIdx = sImageName.ReverseFind(_T('@'));
				int iDotIdx = sImageName.ReverseFind(_T('.'));
				if(iAtIdx != -1 && iDotIdx != -1) {
					CDuiString sExe = sImageName.Mid(iDotIdx);
					sImageName = sImageName.Left(iAtIdx) + sExe;
					data = bGdiplus ? CRenderEngine::GdiplusLoadImage(sImageName.GetData(), nullptr, mask, instance) : CRenderEngine::LoadImage(sImageName.GetData(), nullptr, mask, instance);
				}
			}

		}

		if( data == nullptr ) {
			return nullptr;
		}
		data->bUseHSL = bUseHSL;
		if( type != nullptr ) data->sResType = type;
		data->dwMask = mask;
		if( data->bUseHSL ) {
			data->pSrcBits = new BYTE[data->nX * data->nY * 4];
			::CopyMemory(data->pSrcBits, data->pBits, data->nX * data->nY * 4);
		}
		else data->pSrcBits = nullptr;
		if( m_bUseHSL ) CRenderEngine::AdjustImage(true, data, m_H, m_S, m_L);
		if (data)
		{
			if (bShared || m_bForceUseSharedRes)
			{
				TImageInfo* pOldImageInfo = static_cast<TImageInfo*>(m_SharedResInfo.m_ImageHash.Find(bitmap));
				if (pOldImageInfo)
				{
					CRenderEngine::FreeImage(pOldImageInfo);
					m_SharedResInfo.m_ImageHash.Remove(bitmap);
				}

				if( !m_SharedResInfo.m_ImageHash.Insert(bitmap, data) ) {
					CRenderEngine::FreeImage(data);
					data = nullptr;
				}
			}
			else
			{
				TImageInfo* pOldImageInfo = static_cast<TImageInfo*>(m_ResInfo.m_ImageHash.Find(bitmap));
				if (pOldImageInfo)
				{
					CRenderEngine::FreeImage(pOldImageInfo);
					m_ResInfo.m_ImageHash.Remove(bitmap);
				}

				if( !m_ResInfo.m_ImageHash.Insert(bitmap, data) ) {
					CRenderEngine::FreeImage(data);
					data = nullptr;
				}
			}
		}

		return data;
	}

	const TImageInfo* CPaintManagerUI::AddImage(LPCTSTR bitmap, HBITMAP hBitmap, int iWidth, int iHeight, bool bAlpha, bool bShared)
	{
		// 因无法确定外部HBITMAP格式，不能使用hsl调整
		if( bitmap == nullptr || bitmap[0] == _T('\0') ) return nullptr;
		if( hBitmap == nullptr || iWidth <= 0 || iHeight <= 0 ) return nullptr;

		TImageInfo* data = new TImageInfo;
		data->pBits = nullptr;
		data->pSrcBits = nullptr;
		data->hBitmap = hBitmap;
		data->pBits = nullptr;
		data->nX = iWidth;
		data->nY = iHeight;
		data->bAlpha = bAlpha;
		data->bUseHSL = false;
		data->pSrcBits = nullptr;
		data->dwMask = 0;

		if (bShared || m_bForceUseSharedRes)
		{
			if( !m_SharedResInfo.m_ImageHash.Insert(bitmap, data) ) {
				CRenderEngine::FreeImage(data);
				data = nullptr;
			}
		}
		else
		{
			if( !m_ResInfo.m_ImageHash.Insert(bitmap, data) ) {
				CRenderEngine::FreeImage(data);
				data = nullptr;
			}
		}

		return data;
	}

	void CPaintManagerUI::RemoveImage(LPCTSTR bitmap, bool bShared)
	{
		TImageInfo* data = nullptr;
		if (bShared) 
		{
			data = static_cast<TImageInfo*>(m_SharedResInfo.m_ImageHash.Find(bitmap));
			if (data)
			{
				CRenderEngine::FreeImage(data) ;
				m_SharedResInfo.m_ImageHash.Remove(bitmap);
			}
		}
		else
		{
			data = static_cast<TImageInfo*>(m_ResInfo.m_ImageHash.Find(bitmap));
			if (data)
			{
				CRenderEngine::FreeImage(data) ;
				m_ResInfo.m_ImageHash.Remove(bitmap);
			}
		}
	}

	void CPaintManagerUI::RemoveAllImages(bool bShared)
	{
		if (bShared)
		{
			TImageInfo* data;
			for( int i = 0; i< m_SharedResInfo.m_ImageHash.GetSize(); i++ ) {
				if(LPCTSTR key = m_SharedResInfo.m_ImageHash.GetAt(i)) {
					data = static_cast<TImageInfo*>(m_SharedResInfo.m_ImageHash.Find(key, false));
					if (data) {
						CRenderEngine::FreeImage(data);
					}
				}
			}
			m_SharedResInfo.m_ImageHash.RemoveAll();
		}
		else
		{
			TImageInfo* data;
			for( int i = 0; i< m_ResInfo.m_ImageHash.GetSize(); i++ ) {
				if(LPCTSTR key = m_ResInfo.m_ImageHash.GetAt(i)) {
					data = static_cast<TImageInfo*>(m_ResInfo.m_ImageHash.Find(key, false));
					if (data) {
						CRenderEngine::FreeImage(data);
					}
				}
			}
			m_ResInfo.m_ImageHash.RemoveAll();
		}
	}

	void CPaintManagerUI::AdjustSharedImagesHSL()
	{
		TImageInfo* data;
		for( int i = 0; i< m_SharedResInfo.m_ImageHash.GetSize(); i++ ) {
			if(LPCTSTR key = m_SharedResInfo.m_ImageHash.GetAt(i)) {
				data = static_cast<TImageInfo*>(m_SharedResInfo.m_ImageHash.Find(key));
				if( data && data->bUseHSL ) {
					CRenderEngine::AdjustImage(m_bUseHSL, data, m_H, m_S, m_L);
				}
			}
		}
	}

	void CPaintManagerUI::AdjustImagesHSL()
	{
		TImageInfo* data;
		for( int i = 0; i< m_ResInfo.m_ImageHash.GetSize(); i++ ) {
			if(LPCTSTR key = m_ResInfo.m_ImageHash.GetAt(i)) {
				data = static_cast<TImageInfo*>(m_ResInfo.m_ImageHash.Find(key));
				if( data && data->bUseHSL ) {
					CRenderEngine::AdjustImage(m_bUseHSL, data, m_H, m_S, m_L);
				}
			}
		}
		Invalidate();
	}

	void CPaintManagerUI::PostAsyncNotify()
	{
		if (!m_bAsyncNotifyPosted) {
			::PostMessage(m_hWndPaint, WM_APP + 1, 0, 0L);
			m_bAsyncNotifyPosted = true;
		}
	}
	bool CPaintManagerUI::PostToUiThread(UiTaskProc pfnTask, void* pContext)
	{
		if (pfnTask == nullptr) return false;
		// Already on the owning thread: run inline. Posting here would reorder the
		// task behind whatever the caller does next, which is never what a caller
		// expects from "run this on the UI thread".
		if (IsUiThreadForManager(this)) {
			pfnTask(pContext);
			return true;
		}
		if (m_hWndPaint == nullptr || !::IsWindow(m_hWndPaint)) return false;
		UiTaskQueue* q = FindTaskQueue(this, true);
		if (q == nullptr) return false;   // registry full: report, do not silently drop
		::AcquireSRWLockExclusive(&q->lock);
		if (q->nCount == q->nCap) {
			const int nNewCap = (q->nCap == 0) ? 8 : q->nCap * 2;
			UiTask* pNew = new(std::nothrow) UiTask[nNewCap];
			if (pNew == nullptr) {
				q->nDropped++;
				::ReleaseSRWLockExclusive(&q->lock);
				return false;
			}
			for (int i = 0; i < q->nCount; ++i) pNew[i] = q->pItems[i];
			delete[] q->pItems;
			q->pItems = pNew;
			q->nCap = nNewCap;
		}
		q->pItems[q->nCount].pfn = pfnTask;
		q->pItems[q->nCount].pCtx = pContext;
		q->nCount++;
		::ReleaseSRWLockExclusive(&q->lock);
		// Posted AFTER the task is queued: posting first could deliver a message
		// that finds an empty queue and the task would wait for the next post.
		if (!::PostMessage(m_hWndPaint, WM_APP + 2, 0, 0L)) return false;
		return true;
	}

	LONG CPaintManagerUI::DroppedUiTasks() const
	{
		UiTaskQueue* q = FindTaskQueue(this, false);
		if (q == nullptr) return 0;
		return q->nDropped;
	}

	void CPaintManagerUI::ReloadSharedImages()
	{
		TImageInfo* data = nullptr;
		TImageInfo* pNewData = nullptr;
		for( int i = 0; i< m_SharedResInfo.m_ImageHash.GetSize(); i++ ) {
			if(LPCTSTR bitmap = m_SharedResInfo.m_ImageHash.GetAt(i)) {
				data = static_cast<TImageInfo*>(m_SharedResInfo.m_ImageHash.Find(bitmap));
				if( data != nullptr ) {
					if( !data->sResType.IsEmpty() ) {
						if( isdigit(*bitmap) ) {
							LPTSTR pstr = nullptr;
							int iIndex = _tcstol(bitmap, &pstr, 10);
							pNewData = CRenderEngine::LoadImage(iIndex, data->sResType.GetData(), data->dwMask);
						}
					}
					else {
						pNewData = CRenderEngine::LoadImage(bitmap, nullptr, data->dwMask);
					}
					if( pNewData == nullptr ) continue;

					CRenderEngine::FreeImage(data, false);
					data->hBitmap = pNewData->hBitmap;
					data->pImage = pNewData->pImage;
					data->pBits = pNewData->pBits;
					data->nX = pNewData->nX;
					data->nY = pNewData->nY;
					data->bAlpha = pNewData->bAlpha;
					data->pSrcBits = nullptr;
					if( data->bUseHSL ) {
						data->pSrcBits = new BYTE[data->nX * data->nY * 4];
						::CopyMemory(data->pSrcBits, data->pBits, data->nX * data->nY * 4);
					}
					else data->pSrcBits = nullptr;
					if( m_bUseHSL ) CRenderEngine::AdjustImage(true, data, m_H, m_S, m_L);

					delete pNewData;
				}
			}
		}
	}

	void CPaintManagerUI::ReloadImages()
	{
		RemoveAllDrawInfos();

		TImageInfo* data = nullptr;
		TImageInfo* pNewData = nullptr;
		for( int i = 0; i< m_ResInfo.m_ImageHash.GetSize(); i++ ) {
			if(LPCTSTR bitmap = m_ResInfo.m_ImageHash.GetAt(i)) {
				data = static_cast<TImageInfo*>(m_ResInfo.m_ImageHash.Find(bitmap));
				if( data != nullptr ) {
					if( !data->sResType.IsEmpty() ) {
						if( isdigit(*bitmap) ) {
							LPTSTR pstr = nullptr;
							int iIndex = _tcstol(bitmap, &pstr, 10);
							pNewData = CRenderEngine::LoadImage(iIndex, data->sResType.GetData(), data->dwMask);
						}
					}
					else {
						pNewData = CRenderEngine::LoadImage(bitmap, nullptr, data->dwMask);
					}

					CRenderEngine::FreeImage(data, false);
					if( pNewData == nullptr ) {
						m_ResInfo.m_ImageHash.Remove(bitmap);
						continue;
					}
					data->hBitmap = pNewData->hBitmap;
					data->pBits = pNewData->pBits;
					data->nX = pNewData->nX;
					data->nY = pNewData->nY;
					data->bAlpha = pNewData->bAlpha;
					data->pSrcBits = nullptr;
					if( data->bUseHSL ) {
						data->pSrcBits = new BYTE[data->nX * data->nY * 4];
						::CopyMemory(data->pSrcBits, data->pBits, data->nX * data->nY * 4);
					}
					else data->pSrcBits = nullptr;
					if( m_bUseHSL ) CRenderEngine::AdjustImage(true, data, m_H, m_S, m_L);

					delete pNewData;
				}
			}
		}

		if( m_pRoot ) m_pRoot->Invalidate();
	}

	const TDrawInfo* CPaintManagerUI::GetDrawInfo(LPCTSTR pStrImage, LPCTSTR pStrModify)
	{
		CDuiString sKey;
		sKey.Format(_T("%s%s"), pStrImage == nullptr ? _T("") : pStrImage, pStrModify == nullptr ? _T("") : pStrModify);
		TDrawInfo* pDrawInfo = static_cast<TDrawInfo*>(m_ResInfo.m_DrawInfoHash.Find(sKey));
		if(pDrawInfo == nullptr && !sKey.IsEmpty()) {
			pDrawInfo = new TDrawInfo();
			pDrawInfo->Parse(pStrImage, pStrModify,this);
			m_ResInfo.m_DrawInfoHash.Insert(sKey, pDrawInfo);
		}
		return pDrawInfo;
	}

	void CPaintManagerUI::RemoveDrawInfo(LPCTSTR pStrImage, LPCTSTR pStrModify)
	{
		CDuiString sKey;
		sKey.Format(_T("%s%s"), pStrImage == nullptr ? _T("") : pStrImage, pStrModify == nullptr ? _T("") : pStrModify);
		TDrawInfo* pDrawInfo = static_cast<TDrawInfo*>(m_ResInfo.m_DrawInfoHash.Find(sKey));
		if(pDrawInfo != nullptr) {
			m_ResInfo.m_DrawInfoHash.Remove(sKey);
			delete pDrawInfo;
			pDrawInfo = nullptr;
		}
	}

	void CPaintManagerUI::RemoveAllDrawInfos()
	{
		TDrawInfo* pDrawInfo = nullptr;
		for( int i = 0; i< m_ResInfo.m_DrawInfoHash.GetSize(); i++ ) {
			LPCTSTR key = m_ResInfo.m_DrawInfoHash.GetAt(i);
			if(key != nullptr) {
				pDrawInfo = static_cast<TDrawInfo*>(m_ResInfo.m_DrawInfoHash.Find(key, false));
				if (pDrawInfo) {
					delete pDrawInfo;
					pDrawInfo = nullptr;
				}
			}
		}
		m_ResInfo.m_DrawInfoHash.RemoveAll();
	}

	void CPaintManagerUI::AddDefaultAttributeList(LPCTSTR pStrControlName, LPCTSTR pStrControlAttrList, bool bShared)
	{
		if (bShared || m_bForceUseSharedRes)
		{
			CDuiString* pDefaultAttr = new CDuiString(pStrControlAttrList);
			if (pDefaultAttr != nullptr)
			{
				CDuiString* pOldDefaultAttr = static_cast<CDuiString*>(m_SharedResInfo.m_AttrHash.Set(pStrControlName, static_cast<LPVOID>(pDefaultAttr)));
				if (pOldDefaultAttr) delete pOldDefaultAttr;
			}
		}
		else
		{
			CDuiString* pDefaultAttr = new CDuiString(pStrControlAttrList);
			if (pDefaultAttr != nullptr)
			{
				CDuiString* pOldDefaultAttr = static_cast<CDuiString*>(m_ResInfo.m_AttrHash.Set(pStrControlName, static_cast<LPVOID>(pDefaultAttr)));
				if (pOldDefaultAttr) delete pOldDefaultAttr;
			}
		}
	}

	LPCTSTR CPaintManagerUI::GetDefaultAttributeList(LPCTSTR pStrControlName) const
	{
		CDuiString* pDefaultAttr = static_cast<CDuiString*>(m_ResInfo.m_AttrHash.Find(pStrControlName));
		if( !pDefaultAttr ) pDefaultAttr = static_cast<CDuiString*>(m_SharedResInfo.m_AttrHash.Find(pStrControlName));
		if (pDefaultAttr) return pDefaultAttr->GetData();
		return nullptr;
	}

	bool CPaintManagerUI::RemoveDefaultAttributeList(LPCTSTR pStrControlName, bool bShared)
	{
		if (bShared)
		{
			CDuiString* pDefaultAttr = static_cast<CDuiString*>(m_SharedResInfo.m_AttrHash.Find(pStrControlName));
			if( !pDefaultAttr ) return false;

			delete pDefaultAttr;
			return m_SharedResInfo.m_AttrHash.Remove(pStrControlName);
		}
		else
		{
			CDuiString* pDefaultAttr = static_cast<CDuiString*>(m_ResInfo.m_AttrHash.Find(pStrControlName));
			if( !pDefaultAttr ) return false;

			delete pDefaultAttr;
			return m_ResInfo.m_AttrHash.Remove(pStrControlName);
		}
	}

	void CPaintManagerUI::RemoveAllDefaultAttributeList(bool bShared)
	{
		if (bShared)
		{
			CDuiString* pDefaultAttr;
			for( int i = 0; i< m_SharedResInfo.m_AttrHash.GetSize(); i++ ) {
				if(LPCTSTR key = m_SharedResInfo.m_AttrHash.GetAt(i)) {
					pDefaultAttr = static_cast<CDuiString*>(m_SharedResInfo.m_AttrHash.Find(key));
					if (pDefaultAttr) delete pDefaultAttr;
				}
			}
			m_SharedResInfo.m_AttrHash.RemoveAll();
		}
		else
		{
			CDuiString* pDefaultAttr;
			for( int i = 0; i< m_ResInfo.m_AttrHash.GetSize(); i++ ) {
				if(LPCTSTR key = m_ResInfo.m_AttrHash.GetAt(i)) {
					pDefaultAttr = static_cast<CDuiString*>(m_ResInfo.m_AttrHash.Find(key));
					if (pDefaultAttr) delete pDefaultAttr;
				}
			}
			m_ResInfo.m_AttrHash.RemoveAll();
		}
	}

	void CPaintManagerUI::AddWindowCustomAttribute(LPCTSTR pstrName, LPCTSTR pstrAttr)
	{
		if( pstrName == nullptr || pstrName[0] == _T('\0') || pstrAttr == nullptr || pstrAttr[0] == _T('\0') ) return;
		CDuiString* pCostomAttr = new CDuiString(pstrAttr);
		if (pCostomAttr != nullptr) {
			if (m_mWindowCustomAttrHash.Find(pstrName) == nullptr)
				m_mWindowCustomAttrHash.Set(pstrName, static_cast<LPVOID>(pCostomAttr));
			else
				delete pCostomAttr;
		}
	}

	LPCTSTR CPaintManagerUI::GetWindowCustomAttribute(LPCTSTR pstrName) const
	{
		if( pstrName == nullptr || pstrName[0] == _T('\0') ) return nullptr;
		CDuiString* pCostomAttr = static_cast<CDuiString*>(m_mWindowCustomAttrHash.Find(pstrName));
		if( pCostomAttr ) return pCostomAttr->GetData();
		return nullptr;
	}

	bool CPaintManagerUI::RemoveWindowCustomAttribute(LPCTSTR pstrName)
	{
		if( pstrName == nullptr || pstrName[0] == _T('\0') ) return false;
		CDuiString* pCostomAttr = static_cast<CDuiString*>(m_mWindowCustomAttrHash.Find(pstrName));
		if( !pCostomAttr ) return false;

		delete pCostomAttr;
		return m_mWindowCustomAttrHash.Remove(pstrName);
	}

	void CPaintManagerUI::RemoveAllWindowCustomAttribute()
	{
		CDuiString* pCostomAttr;
		for( int i = 0; i< m_mWindowCustomAttrHash.GetSize(); i++ ) {
			if(LPCTSTR key = m_mWindowCustomAttrHash.GetAt(i)) {
				pCostomAttr = static_cast<CDuiString*>(m_mWindowCustomAttrHash.Find(key));
				delete pCostomAttr;
			}
		}
		m_mWindowCustomAttrHash.Resize();
	}

	CControlUI* CPaintManagerUI::GetRoot() const
	{
		ASSERT(m_pRoot);
		return m_pRoot;
	}

	CControlUI* CPaintManagerUI::FindControl(POINT pt) const
	{
		ASSERT(m_pRoot);
		return m_pRoot->FindControl(__FindControlFromPoint, &pt, UIFIND_VISIBLE | UIFIND_HITTEST | UIFIND_TOP_FIRST);
	}

	CControlUI* CPaintManagerUI::FindControl(LPCTSTR pstrName) const
	{
		ASSERT(m_pRoot);
		return static_cast<CControlUI*>(m_mNameHash.Find(pstrName));
	}

	CControlUI* CPaintManagerUI::FindSubControlByPoint(CControlUI* pParent, POINT pt) const
	{
		if( pParent == nullptr ) pParent = GetRoot();
		ASSERT(pParent);
		return pParent->FindControl(__FindControlFromPoint, &pt, UIFIND_VISIBLE | UIFIND_HITTEST | UIFIND_TOP_FIRST);
	}

	CControlUI* CPaintManagerUI::FindSubControlByName(CControlUI* pParent, LPCTSTR pstrName) const
	{
		if( pParent == nullptr ) pParent = GetRoot();
		ASSERT(pParent);
		return pParent->FindControl(__FindControlFromName, const_cast<TCHAR*>(pstrName), UIFIND_ALL);
	}

	CControlUI* CPaintManagerUI::FindSubControlByClass(CControlUI* pParent, LPCTSTR pstrClass, int iIndex)
	{
		if( pParent == nullptr ) pParent = GetRoot();
		ASSERT(pParent);
		m_aFoundControls.Resize(iIndex + 1);
		return pParent->FindControl(__FindControlFromClass, const_cast<TCHAR*>(pstrClass), UIFIND_ALL);
	}

	CStdPtrArray* CPaintManagerUI::FindSubControlsByClass(CControlUI* pParent, LPCTSTR pstrClass)
	{
		if( pParent == nullptr ) pParent = GetRoot();
		ASSERT(pParent);
		m_aFoundControls.Empty();
		pParent->FindControl(__FindControlsFromClass, const_cast<TCHAR*>(pstrClass), UIFIND_ALL);
		return &m_aFoundControls;
	}

	CStdPtrArray* CPaintManagerUI::GetFoundControls()
	{
		return &m_aFoundControls;
	}

	CControlUI* CALLBACK CPaintManagerUI::__FindControlFromNameHash(CControlUI* pThis, LPVOID pData)
	{
		CPaintManagerUI* pManager = static_cast<CPaintManagerUI*>(pData);
		const CDuiString& sName = pThis->GetName();
		if( sName.IsEmpty() ) return nullptr;
		// Add this control to the hash list
		pManager->m_mNameHash.Set(sName, pThis);
		return nullptr; // Attempt to add all controls
	}

	CControlUI* CALLBACK CPaintManagerUI::__FindControlFromCount(CControlUI* /*pThis*/, LPVOID pData)
	{
		int* pnCount = static_cast<int*>(pData);
		(*pnCount)++;
		return nullptr;  // Count all controls
	}

	CControlUI* CALLBACK CPaintManagerUI::__FindControlFromPoint(CControlUI* pThis, LPVOID pData)
	{
		LPPOINT pPoint = static_cast<LPPOINT>(pData);
		return ::PtInRect(&pThis->GetPos(), *pPoint) ? pThis : nullptr;
	}

	CControlUI* CALLBACK CPaintManagerUI::__FindControlFromTab(CControlUI* pThis, LPVOID pData)
	{
		FINDTABINFO* pInfo = static_cast<FINDTABINFO*>(pData);
		if( pInfo->pFocus == pThis ) {
			if( pInfo->bForward ) pInfo->bNextIsIt = true;
			return pInfo->bForward ? nullptr : pInfo->pLast;
		}
		if( (pThis->GetControlFlags() & UIFLAG_TABSTOP) == 0 ) return nullptr;
		pInfo->pLast = pThis;
		if( pInfo->bNextIsIt ) return pThis;
		if( pInfo->pFocus == nullptr ) return pThis;
		return nullptr;  // Examine all controls
	}

	CControlUI* CALLBACK CPaintManagerUI::__FindControlFromShortcut(CControlUI* pThis, LPVOID pData)
	{
		if( !pThis->IsVisible() ) return nullptr; 
		FINDSHORTCUT* pFS = static_cast<FINDSHORTCUT*>(pData);
		if( pFS->ch == toupper(pThis->GetShortcut()) ) pFS->bPickNext = true;
		if( _tcsstr(pThis->GetClass(), _T("LabelUI")) != nullptr ) return nullptr;   // Labels never get focus!
		return pFS->bPickNext ? pThis : nullptr;
	}

	CControlUI* CALLBACK CPaintManagerUI::__FindControlFromName(CControlUI* pThis, LPVOID pData)
	{
		LPCTSTR pstrName = static_cast<LPCTSTR>(pData);
		const CDuiString& sName = pThis->GetName();
		if( sName.IsEmpty() ) return nullptr;
		return (_tcsicmp(sName, pstrName) == 0) ? pThis : nullptr;
	}

	CControlUI* CALLBACK CPaintManagerUI::__FindControlFromClass(CControlUI* pThis, LPVOID pData)
	{
		LPCTSTR pstrType = static_cast<LPCTSTR>(pData);
		LPCTSTR pType = pThis->GetClass();
		CStdPtrArray* pFoundControls = pThis->GetManager()->GetFoundControls();
		if( _tcscmp(pstrType, _T("*")) == 0 || _tcscmp(pstrType, pType) == 0 ) {
			int iIndex = -1;
			while( pFoundControls->GetAt(++iIndex) != nullptr ) ;
			if( iIndex < pFoundControls->GetSize() ) pFoundControls->SetAt(iIndex, pThis);
		}
		if( pFoundControls->GetAt(pFoundControls->GetSize() - 1) != nullptr ) return pThis; 
		return nullptr;
	}

	CControlUI* CALLBACK CPaintManagerUI::__FindControlsFromClass(CControlUI* pThis, LPVOID pData)
	{
		LPCTSTR pstrType = static_cast<LPCTSTR>(pData);
		LPCTSTR pType = pThis->GetClass();
		if( _tcscmp(pstrType, _T("*")) == 0 || _tcscmp(pstrType, pType) == 0 ) 
			pThis->GetManager()->GetFoundControls()->Add(static_cast<LPVOID>(pThis));
		return nullptr;
	}

	CControlUI* CALLBACK CPaintManagerUI::__FindControlsFromUpdate(CControlUI* pThis, LPVOID pData)
	{
		if( pThis->IsUpdateNeeded() ) {
			pThis->GetManager()->GetFoundControls()->Add(static_cast<LPVOID>(pThis));
			return pThis;
		}
		return nullptr;
	}

	bool CPaintManagerUI::TranslateAccelerator(LPMSG pMsg)
	{
		for (int i = 0; i < m_aTranslateAccelerator.GetSize(); i++)
		{
			LRESULT lResult = static_cast<ITranslateAccelerator *>(m_aTranslateAccelerator[i])->TranslateAccelerator(pMsg);
			if( lResult == S_OK ) return true;
		}
		return false;
	}

	bool CPaintManagerUI::TranslateMessage(const LPMSG pMsg)
	{
		// Pretranslate Message takes care of system-wide messages, such as
		// tabbing and shortcut key-combos. We'll look for all messages for
		// each window and any child control attached.
		UINT uStyle = GetWindowStyle(pMsg->hwnd);
		UINT uChildRes = uStyle & WS_CHILD;	
		LRESULT lRes = 0;
		if (uChildRes != 0)
		{
			HWND hWndParent = ::GetParent(pMsg->hwnd);

			for( int i = 0; i < m_aPreMessages.GetSize(); i++ ) 
			{
				CPaintManagerUI* pT = static_cast<CPaintManagerUI*>(m_aPreMessages[i]);        
				HWND hTempParent = hWndParent;
				while(hTempParent)
				{
					if(pMsg->hwnd == pT->GetPaintWindow() || hTempParent == pT->GetPaintWindow())
					{
						if (pT->TranslateAccelerator(pMsg))
							return true;

						pT->PreMessageHandler(pMsg->message, pMsg->wParam, pMsg->lParam, lRes);
					}
					hTempParent = GetParent(hTempParent);
				}
			}
		}
		else
		{
			for( int i = 0; i < m_aPreMessages.GetSize(); i++ ) 
			{
				CPaintManagerUI* pT = static_cast<CPaintManagerUI*>(m_aPreMessages[i]);
				if(pMsg->hwnd == pT->GetPaintWindow())
				{
					if (pT->TranslateAccelerator(pMsg))
						return true;

					if( pT->PreMessageHandler(pMsg->message, pMsg->wParam, pMsg->lParam, lRes) ) 
						return true;

					return false;
				}
			}
		}
		return false;
	}

	bool CPaintManagerUI::AddTranslateAccelerator(ITranslateAccelerator *pTranslateAccelerator)
	{
		ASSERT(m_aTranslateAccelerator.Find(pTranslateAccelerator) < 0);
		return m_aTranslateAccelerator.Add(pTranslateAccelerator);
	}

	bool CPaintManagerUI::RemoveTranslateAccelerator(ITranslateAccelerator *pTranslateAccelerator)
	{
		for (int i = 0; i < m_aTranslateAccelerator.GetSize(); i++)
		{
			if (static_cast<ITranslateAccelerator *>(m_aTranslateAccelerator[i]) == pTranslateAccelerator)
			{
				return m_aTranslateAccelerator.Remove(i);
			}
		}
		return false;
	}

	void CPaintManagerUI::UsedVirtualWnd(bool bUsed)
	{
		m_bUsedVirtualWnd = bUsed;
	}

	// 样式管理
	void CPaintManagerUI::AddStyle(LPCTSTR pName, LPCTSTR pDeclarationList, bool bShared)
	{
		CDuiString* pStyle = new CDuiString(pDeclarationList);

		if(bShared || m_bForceUseSharedRes){
			if( !m_SharedResInfo.m_StyleHash.Insert(pName, pStyle) ) {
				delete pStyle;
			}
		}
		else
		{
			if( !m_ResInfo.m_StyleHash.Insert(pName, pStyle) ) {
				delete pStyle;
			}
		}
	}

	LPCTSTR CPaintManagerUI::GetStyle(LPCTSTR pName) const
	{
		CDuiString* pStyle = static_cast<CDuiString*>(m_ResInfo.m_StyleHash.Find(pName));
		if( !pStyle ) pStyle = static_cast<CDuiString*>(m_SharedResInfo.m_StyleHash.Find(pName));
		if( pStyle ) return pStyle->GetData();
		else return nullptr;
	}

	BOOL CPaintManagerUI::RemoveStyle(LPCTSTR pName, bool bShared)
	{
		CDuiString* pStyle = nullptr;
		if (bShared) 
		{
			pStyle = static_cast<CDuiString*>(m_SharedResInfo.m_StyleHash.Find(pName));
			if (pStyle)
			{
				delete pStyle;
				m_SharedResInfo.m_StyleHash.Remove(pName);
			}
		}
		else
		{
			pStyle = static_cast<CDuiString*>(m_ResInfo.m_StyleHash.Find(pName));
			if (pStyle)
			{
				delete pStyle;
				m_ResInfo.m_StyleHash.Remove(pName);
			}
		}
		return true;
	}

	const CStdStringPtrMap& CPaintManagerUI::GetStyles(bool bShared) const
	{
		if(bShared) return m_SharedResInfo.m_StyleHash;
		else return m_ResInfo.m_StyleHash;
	}

	void CPaintManagerUI::RemoveAllStyle(bool bShared)
	{
		if (bShared)
		{
			CDuiString* pStyle;
			for( int i = 0; i< m_SharedResInfo.m_StyleHash.GetSize(); i++ ) {
				if(LPCTSTR key = m_SharedResInfo.m_StyleHash.GetAt(i)) {
					pStyle = static_cast<CDuiString*>(m_SharedResInfo.m_StyleHash.Find(key));
					delete pStyle;
				}
			}
			m_SharedResInfo.m_StyleHash.RemoveAll();
		}
		else
		{
			CDuiString* pStyle;
			for( int i = 0; i< m_ResInfo.m_StyleHash.GetSize(); i++ ) {
				if(LPCTSTR key = m_ResInfo.m_StyleHash.GetAt(i)) {
					pStyle = static_cast<CDuiString*>(m_ResInfo.m_StyleHash.Find(key));
					delete pStyle;
				}
			}
			m_ResInfo.m_StyleHash.RemoveAll();
		}
	}

	const TImageInfo* CPaintManagerUI::GetImageString(LPCTSTR pStrImage, LPCTSTR pStrModify)
	{
		CDuiString sImageName = pStrImage;
		CDuiString sImageResType = _T("");
		DWORD dwMask = 0;
		CDuiString sItem;
		CDuiString sValue;
		LPTSTR pstr = nullptr;

		for( int i = 0; i < 2; ++i) {
			if( i == 1)
				pStrImage = pStrModify;

			if( !pStrImage ) continue;

			while( *pStrImage != _T('\0') ) {
				sItem.Empty();
				sValue.Empty();
				while( *pStrImage > _T('\0') && *pStrImage <= _T(' ') ) pStrImage = ::CharNext(pStrImage);
				while( *pStrImage != _T('\0') && *pStrImage != _T('=') && *pStrImage > _T(' ') ) {
					LPTSTR pstrTemp = ::CharNext(pStrImage);
					while( pStrImage < pstrTemp) {
						sItem += *pStrImage++;
					}
				}
				while( *pStrImage > _T('\0') && *pStrImage <= _T(' ') ) pStrImage = ::CharNext(pStrImage);
				if( *pStrImage++ != _T('=') ) break;
				while( *pStrImage > _T('\0') && *pStrImage <= _T(' ') ) pStrImage = ::CharNext(pStrImage);
				if( *pStrImage++ != _T('\'') ) break;
				while( *pStrImage != _T('\0') && *pStrImage != _T('\'') ) {
					LPTSTR pstrTemp = ::CharNext(pStrImage);
					while( pStrImage < pstrTemp) {
						sValue += *pStrImage++;
					}
				}
				if( *pStrImage++ != _T('\'') ) break;
				if( !sValue.IsEmpty() ) {
					if( sItem == _T("file") || sItem == _T("res") ) {
						sImageName = sValue;
					}
					else if( sItem == _T("restype") ) {
						sImageResType = sValue;
					}
					else if( sItem == _T("mask") ) 
					{
						if( sValue[0] == _T('#')) dwMask = _tcstoul(sValue.GetData() + 1, &pstr, 16);
						else dwMask = _tcstoul(sValue.GetData(), &pstr, 16);
					}

				}
				if( *pStrImage++ != _T(' ') ) break;
			}
		}
		return GetImageEx(sImageName, sImageResType, dwMask);
	}

	bool CPaintManagerUI::EnableDragDrop(bool bEnable)
	{
		if(m_bDragDrop == bEnable) return false;
		m_bDragDrop = bEnable;

		if(bEnable) {
			AddRef();

			if(FAILED(RegisterDragDrop(m_hWndPaint, this))) {
				return false;
			}

			SetTargetWnd(m_hWndPaint);

			FORMATETC ftetc={0};
			ftetc.cfFormat = CF_BITMAP;
			ftetc.dwAspect = DVASPECT_CONTENT;
			ftetc.lindex = -1;
			ftetc.tymed = TYMED_GDI;
			AddSuportedFormat(ftetc);
			ftetc.cfFormat = CF_DIB;
			ftetc.tymed = TYMED_HGLOBAL;
			AddSuportedFormat(ftetc);
			ftetc.cfFormat = CF_HDROP;
			ftetc.tymed = TYMED_HGLOBAL;
			AddSuportedFormat(ftetc);
			ftetc.cfFormat = CF_ENHMETAFILE;
			ftetc.tymed = TYMED_ENHMF;
			AddSuportedFormat(ftetc);
		}
		else{
			Release();
			if(FAILED(RevokeDragDrop(m_hWndPaint))) {
				return false;
			}
		}
		return true;
	}

	void CPaintManagerUI::SetDragDrop(IDragDropUI* pDragDrop)
	{
		m_pDragDrop = pDragDrop;
	}

	static WORD DIBNumColors(void* pv) 
	{     
		int bits = 0;     
		LPBITMAPINFOHEADER  lpbi;     
		LPBITMAPCOREHEADER  lpbc;      
		lpbi = (reinterpret_cast<LPBITMAPINFOHEADER>(pv));     
		lpbc = (reinterpret_cast<LPBITMAPCOREHEADER>(pv));      
		/*  With the BITMAPINFO format headers, the size of the palette 
		*  is in biClrUsed, whereas in the BITMAPCORE - style headers, it      
		*  is dependent on the bits per pixel ( = 2 raised to the power of      
		*  bits/pixel).
		*/     
		if (lpbi->biSize != sizeof(BITMAPCOREHEADER))
		{         
			if (lpbi->biClrUsed != 0)
				return static_cast<WORD>(lpbi->biClrUsed);         
			bits = lpbi->biBitCount;     
		}     
		else         
			bits = lpbc->bcBitCount;
		switch (bits)
		{         
		case 1:                 
			return 2;         
		case 4:                 
			return 16;         
		case 8:       
			return 256;
		default:
			/* A 24 bitcount DIB has no color table */                 
			return 0;
		} 
	} 
	//code taken from SEEDIB MSDN sample
	static WORD ColorTableSize(LPVOID lpv)
	{
		LPBITMAPINFOHEADER lpbih = reinterpret_cast<LPBITMAPINFOHEADER>(lpv);

		if (lpbih->biSize != sizeof(BITMAPCOREHEADER))
		{
			if ((reinterpret_cast<LPBITMAPINFOHEADER>((lpbih)))->biCompression == BI_BITFIELDS)
				/* Remember that 16/32bpp dibs can still have a color table */
				return (sizeof(DWORD) * 3) + (DIBNumColors (lpbih) * sizeof (RGBQUAD));
			else
				return static_cast<WORD>(DIBNumColors(lpbih) * sizeof(RGBQUAD));
		}
		else
			return static_cast<WORD>(DIBNumColors(lpbih) * sizeof(RGBTRIPLE));
	}

	bool CPaintManagerUI::OnDrop(FORMATETC* pFmtEtc, STGMEDIUM& medium,DWORD *pdwEffect)
	{
		POINT ptMouse = {0};
		GetCursorPos(&ptMouse);
		::SendMessage(m_hTargetWnd, WM_LBUTTONUP, 0, MAKELPARAM(ptMouse.x, ptMouse.y));

		if(pFmtEtc->cfFormat == CF_DIB && medium.tymed == TYMED_HGLOBAL)
		{
			if(medium.hGlobal != nullptr)
			{
				LPBITMAPINFOHEADER  lpbi = reinterpret_cast<BITMAPINFOHEADER*>(GlobalLock(medium.hGlobal));
				if(lpbi != nullptr)
				{
					HBITMAP hbm = nullptr;
					HDC hdc = GetDC(nullptr);
					if(hdc != nullptr)
					{
						int i = (reinterpret_cast<BITMAPFILEHEADER *>(lpbi))->bfOffBits;
						hbm = CreateDIBitmap(hdc,reinterpret_cast<LPBITMAPINFOHEADER>(lpbi),
							static_cast<LONG>(CBM_INIT),
							reinterpret_cast<LPSTR>(lpbi) + lpbi->biSize + ColorTableSize(lpbi),
							reinterpret_cast<LPBITMAPINFO>(lpbi),DIB_RGB_COLORS);

						::ReleaseDC(nullptr,hdc);
					}
					GlobalUnlock(medium.hGlobal);
					if(hbm != nullptr)
						hbm = reinterpret_cast<HBITMAP>(SendMessage(m_hTargetWnd, STM_SETIMAGE, IMAGE_BITMAP, reinterpret_cast<LPARAM>(hbm)));
					if(hbm != nullptr)
						DeleteObject(hbm);
					return true; //release the medium
				}
			}
		}
		if(pFmtEtc->cfFormat == CF_BITMAP && medium.tymed == TYMED_GDI)
		{
			if(medium.hBitmap != nullptr)
			{
				HBITMAP hBmp = reinterpret_cast<HBITMAP>(SendMessage(m_hTargetWnd, STM_SETIMAGE, IMAGE_BITMAP, reinterpret_cast<LPARAM>(medium.hBitmap)));
				if(hBmp != nullptr)
					DeleteObject(hBmp);
				return false; //don't free the bitmap
			}
		}
		if(pFmtEtc->cfFormat == CF_ENHMETAFILE && medium.tymed == TYMED_ENHMF)
		{
			ENHMETAHEADER emh;
			GetEnhMetaFileHeader(medium.hEnhMetaFile, sizeof(ENHMETAHEADER),&emh);
			RECT rc;//={0,0,EnhMetaHdr.rclBounds.right-EnhMetaHdr.rclBounds.left, EnhMetaHdr.rclBounds.bottom-EnhMetaHdr.rclBounds.top};
			HDC hDC= GetDC(m_hTargetWnd);
			//start code: taken from ENHMETA.EXE MSDN Sample
			//*ALSO NEED to GET the pallete (select and RealizePalette it, but i was too lazy*
			// Get the characteristics of the output device
			float PixelsX = static_cast<float>(GetDeviceCaps( hDC, HORZRES ));
			float PixelsY = static_cast<float>(GetDeviceCaps( hDC, VERTRES ));
			float MMX = static_cast<float>(GetDeviceCaps( hDC, HORZSIZE ));
			float MMY = static_cast<float>(GetDeviceCaps( hDC, VERTSIZE ));
			// Calculate the rect in which to draw the metafile based on the
			// intended size and the current output device resolution
			// Remember that the intended size is given in 0.01mm units, so
			// convert those to device units on the target device
			rc.top = static_cast<int>(static_cast<float>(emh.rclFrame.top) * PixelsY / (MMY*100.0f));
			rc.left = static_cast<int>(static_cast<float>(emh.rclFrame.left) * PixelsX / (MMX*100.0f));
			rc.right = static_cast<int>(static_cast<float>(emh.rclFrame.right) * PixelsX / (MMX*100.0f));
			rc.bottom = static_cast<int>(static_cast<float>(emh.rclFrame.bottom) * PixelsY / (MMY*100.0f));
			//end code: taken from ENHMETA.EXE MSDN Sample

			HDC hdcMem = CreateCompatibleDC(hDC);
			HGDIOBJ hBmpMem = CreateCompatibleBitmap(hDC, emh.rclBounds.right, emh.rclBounds.bottom);
			HGDIOBJ hOldBmp = ::SelectObject(hdcMem, hBmpMem);
			PlayEnhMetaFile(hdcMem,medium.hEnhMetaFile,&rc);
			HBITMAP hBmp = reinterpret_cast<HBITMAP>(::SelectObject(hdcMem, hOldBmp));
			DeleteDC(hdcMem);
			ReleaseDC(m_hTargetWnd,hDC);
			hBmp = reinterpret_cast<HBITMAP>(SendMessage(m_hTargetWnd, STM_SETIMAGE, IMAGE_BITMAP, reinterpret_cast<LPARAM>(hBmp)));
			if(hBmp != nullptr)
				DeleteObject(hBmp);
			return true;
		}
		if(pFmtEtc->cfFormat == CF_HDROP && medium.tymed == TYMED_HGLOBAL)
		{
			HDROP hDrop = reinterpret_cast<HDROP>(GlobalLock(medium.hGlobal));
			if(hDrop != nullptr)
			{
				TCHAR szFileName[MAX_PATH];
				UINT cFiles = DragQueryFile(hDrop, 0xFFFFFFFF, nullptr, 0); 
				if(cFiles > 0)
				{
					DragQueryFile(hDrop, 0, szFileName, sizeof(szFileName)); 
					HBITMAP hBitmap = reinterpret_cast<HBITMAP>(LoadImage(nullptr, szFileName,IMAGE_BITMAP,0,0,LR_DEFAULTSIZE|LR_LOADFROMFILE));
					if(hBitmap)
					{
						HBITMAP hBmp = reinterpret_cast<HBITMAP>(SendMessage(m_hTargetWnd, STM_SETIMAGE, IMAGE_BITMAP, reinterpret_cast<LPARAM>(hBitmap)));
						if(hBmp != nullptr)
							DeleteObject(hBmp);
					}
				}
			}
			GlobalUnlock(medium.hGlobal);
		}
		return true; //let base free the medium
	}
} // namespace DuiLib

#pragma warning(pop)