#include "StdAfx.h"

#ifdef _DEBUG
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")
#endif

namespace DuiLib {

/////////////////////////////////////////////////////////////////////////////////////
//
//

void UILIB_API DUI__Trace(LPCTSTR pstrFormat, ...)
{
#ifdef _DEBUG
	TCHAR szBuffer[2048] = {0};
    va_list args;
    va_start(args, pstrFormat);
	_vsntprintf(szBuffer, 2048, pstrFormat, args); 
    va_end(args);
    
	CDuiString strMsg = szBuffer;
    strMsg += _T("\n");
    OutputDebugString(strMsg.GetData());

#endif
}

LPCTSTR DUI__TraceMsg(UINT uMsg)
{
#define MSGDEF(x) if(uMsg==x) return _T(#x)
    MSGDEF(WM_SETCURSOR);
    MSGDEF(WM_NCHITTEST);
    MSGDEF(WM_NCPAINT);
    MSGDEF(WM_PAINT);
    MSGDEF(WM_ERASEBKGND);
    MSGDEF(WM_NCMOUSEMOVE);  
    MSGDEF(WM_MOUSEMOVE);
    MSGDEF(WM_MOUSELEAVE);
    MSGDEF(WM_MOUSEHOVER);   
    MSGDEF(WM_NOTIFY);
    MSGDEF(WM_COMMAND);
    MSGDEF(WM_MEASUREITEM);
    MSGDEF(WM_DRAWITEM);   
    MSGDEF(WM_LBUTTONDOWN);
    MSGDEF(WM_LBUTTONUP);
    MSGDEF(WM_LBUTTONDBLCLK);
    MSGDEF(WM_RBUTTONDOWN);
    MSGDEF(WM_RBUTTONUP);
    MSGDEF(WM_RBUTTONDBLCLK);
    MSGDEF(WM_SETFOCUS);
    MSGDEF(WM_KILLFOCUS);  
    MSGDEF(WM_MOVE);
    MSGDEF(WM_SIZE);
    MSGDEF(WM_SIZING);
    MSGDEF(WM_MOVING);
    MSGDEF(WM_GETMINMAXINFO);
    MSGDEF(WM_CAPTURECHANGED);
    MSGDEF(WM_WINDOWPOSCHANGED);
    MSGDEF(WM_WINDOWPOSCHANGING);   
    MSGDEF(WM_NCCALCSIZE);
    MSGDEF(WM_NCCREATE);
    MSGDEF(WM_NCDESTROY);
    MSGDEF(WM_TIMER);
    MSGDEF(WM_KEYDOWN);
    MSGDEF(WM_KEYUP);
    MSGDEF(WM_CHAR);
    MSGDEF(WM_SYSKEYDOWN);
    MSGDEF(WM_SYSKEYUP);
    MSGDEF(WM_SYSCOMMAND);
    MSGDEF(WM_SYSCHAR);
    MSGDEF(WM_VSCROLL);
    MSGDEF(WM_HSCROLL);
    MSGDEF(WM_CHAR);
    MSGDEF(WM_SHOWWINDOW);
    MSGDEF(WM_PARENTNOTIFY);
    MSGDEF(WM_CREATE);
    MSGDEF(WM_NCACTIVATE);
    MSGDEF(WM_ACTIVATE);
    MSGDEF(WM_ACTIVATEAPP);   
    MSGDEF(WM_CLOSE);
    MSGDEF(WM_DESTROY);
    MSGDEF(WM_GETICON);   
    MSGDEF(WM_GETTEXT);
    MSGDEF(WM_GETTEXTLENGTH);   
    // [H2] C++17 thread_local buffer: fix 1-char overflow + cross-thread sharing
    thread_local TCHAR szMsg[16];
    ::_stprintf_s(szMsg, RTL_NUMBER_OF(szMsg), _T("0x%08X"), uMsg);
    return szMsg;
}

/////////////////////////////////////////////////////////////////////////////////////
//
//

//////////////////////////////////////////////////////////////////////////
//
DUI_BASE_BEGIN_MESSAGE_MAP(CNotifyPump)
DUI_END_MESSAGE_MAP()

static const DUI_MSGMAP_ENTRY* DuiFindMessageEntry(const DUI_MSGMAP_ENTRY* lpEntry,TNotifyUI& msg )
{
	CDuiString sMsgType = msg.sType;
	CDuiString sCtrlName = msg.pSender->GetName();
	const DUI_MSGMAP_ENTRY* pMsgTypeEntry = nullptr;
	while (lpEntry->nSig != static_cast<UINT>(DuiSig::DuiSig_end))
	{
		if(lpEntry->sMsgType==sMsgType)
		{
			if(!lpEntry->sCtrlName.IsEmpty())
			{
				if(lpEntry->sCtrlName==sCtrlName)
				{
					return lpEntry;
				}
			}
			else
			{
				pMsgTypeEntry = lpEntry;
			}
		}
		lpEntry++;
	}
	return pMsgTypeEntry;
}

bool CNotifyPump::AddVirtualWnd(CDuiString strName,CNotifyPump* pObject)
{
	if( m_VirtualWndMap.Find(strName) == nullptr )
	{
		m_VirtualWndMap.Insert(strName.GetData(),reinterpret_cast<LPVOID>(pObject));
		return true;
	}
	return false;
}

bool CNotifyPump::RemoveVirtualWnd(CDuiString strName)
{
	if( m_VirtualWndMap.Find(strName) != nullptr )
	{
		m_VirtualWndMap.Remove(strName);
		return true;
	}
	return false;
}

bool CNotifyPump::LoopDispatch(TNotifyUI& msg)
{
	const DUI_MSGMAP_ENTRY* lpEntry = nullptr;
	const DUI_MSGMAP* pMessageMap = nullptr;

#ifndef UILIB_STATIC
	for(pMessageMap = GetMessageMap(); pMessageMap!=nullptr; pMessageMap = (*pMessageMap->pfnGetBaseMap)())
#else
	for(pMessageMap = GetMessageMap(); pMessageMap!=nullptr; pMessageMap = pMessageMap->pBaseMap)
#endif
	{
#ifndef UILIB_STATIC
		ASSERT(pMessageMap != (*pMessageMap->pfnGetBaseMap)());
#else
		ASSERT(pMessageMap != pMessageMap->pBaseMap);
#endif
		lpEntry = DuiFindMessageEntry(pMessageMap->lpEntries, msg);
		if (lpEntry != nullptr) break;
	}
	if (lpEntry == nullptr) return false;
	// [S4/T4.2] 类型安全承载：先存入通用成员指针，再按 nSig 显式还原为对应签名后调用。
	// 不再依赖 union 的类型双关读取（语义与原实现等价）。
	DuiMessageMapFunctions mmf;
	mmf.store(lpEntry->pfn);

	bool bRet = false;
	DuiSig nSig = static_cast<DuiSig>(lpEntry->nSig);
	switch (nSig)
	{
	default:
		ASSERT(FALSE);
		break;
	case DuiSig::DuiSig_lwl:
		{
			LRESULT (CNotifyPump::*pfn_lwl)(WPARAM, LPARAM) = nullptr;
			mmf.load_lwl(pfn_lwl);
			(this->*pfn_lwl)(msg.wParam, msg.lParam);
			bRet = true;
		}
		break;
	case DuiSig::DuiSig_vn:
		{
			void (CNotifyPump::*pfn_vn)(TNotifyUI&) = nullptr;
			mmf.load_vn(pfn_vn);
			(this->*pfn_vn)(msg);
			bRet = true;
		}
		break;
	}
	return bRet;
}

void CNotifyPump::NotifyPump(TNotifyUI& msg)
{
	///遍历虚拟窗口
	if( !msg.sVirtualWnd.IsEmpty() ){
		for( int i = 0; i< m_VirtualWndMap.GetSize(); i++ ) {
			if( LPCTSTR key = m_VirtualWndMap.GetAt(i) ) {
				if( _tcsicmp(key, msg.sVirtualWnd.GetData()) == 0 ){
					CNotifyPump* pObject = static_cast<CNotifyPump*>(m_VirtualWndMap.Find(key, false));
					if( pObject && pObject->LoopDispatch(msg) )
						return;
				}
			}
		}
	}

	///
	//遍历主窗口
	LoopDispatch( msg );
}

//////////////////////////////////////////////////////////////////////////
///
CWindowWnd::CWindowWnd() : m_hWnd(nullptr), m_OldWndProc(::DefWindowProc), m_bSubclassed(false), m_bUnicode(false), m_bFakeModal(false)
{
}

HWND CWindowWnd::GetHWND() const 
{ 
    return m_hWnd; 
}

UINT CWindowWnd::GetClassStyle() const
{
    return 0;
}

LPCTSTR CWindowWnd::GetSuperClassName() const
{
    return nullptr;
}

CWindowWnd::operator HWND() const
{
    return m_hWnd;
}
void CWindowWnd::EnableUnicode()
{
	m_bUnicode = true;
}
HWND CWindowWnd::CreateDuiWindow( HWND hwndParent, LPCTSTR pstrWindowName,DWORD dwStyle /*=0*/, DWORD dwExStyle /*=0*/ )
{
	return Create(hwndParent,pstrWindowName,dwStyle,dwExStyle,0,0,0,0,nullptr);
}

HWND CWindowWnd::Create(HWND hwndParent, LPCTSTR pstrName, DWORD dwStyle, DWORD dwExStyle, const RECT rc, HMENU hMenu)
{
    return Create(hwndParent, pstrName, dwStyle, dwExStyle, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, hMenu);
}

HWND CWindowWnd::Create(HWND hwndParent, LPCTSTR pstrName, DWORD dwStyle, DWORD dwExStyle, int x, int y, int cx, int cy, HMENU hMenu)
{
    if( GetSuperClassName() != nullptr && !RegisterSuperclass() ) return nullptr;
    if( GetSuperClassName() == nullptr && !RegisterWindowClass() ) return nullptr;
	if(m_bUnicode) {
#ifndef UNICODE
		LPWSTR lpClassName = a2w(const_cast<char*>(GetWindowClassName()));
		LPWSTR lpName = a2w(const_cast<char*>(pstrName));
#else
		LPWSTR lpClassName = const_cast<LPWSTR>(GetWindowClassName());
		LPWSTR lpName = const_cast<LPWSTR>(pstrName);
#endif
		m_hWnd = ::CreateWindowExW(dwExStyle, lpClassName, lpName, dwStyle, x, y, cx, cy, hwndParent, hMenu, CPaintManagerUI::GetInstance(), this);
#ifndef UNICODE
		delete []lpClassName;
		delete []lpName;
#endif
	}
	else {
		m_hWnd = ::CreateWindowEx(dwExStyle, GetWindowClassName(), pstrName, dwStyle, x, y, cx, cy, hwndParent, hMenu, CPaintManagerUI::GetInstance(), this);
	}
    ASSERT(m_hWnd!=nullptr);
    return m_hWnd;
}

HWND CWindowWnd::Subclass(HWND hWnd)
{
    ASSERT(::IsWindow(hWnd));
    ASSERT(m_hWnd==nullptr);
    m_OldWndProc = SubclassWindow(hWnd, __WndProc);
    if( m_OldWndProc == nullptr ) return nullptr;
    m_bSubclassed = true;
    m_hWnd = hWnd;
    ::SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LPARAM>(this));
    return m_hWnd;
}

void CWindowWnd::Unsubclass()
{
    ASSERT(::IsWindow(m_hWnd));
    if( !::IsWindow(m_hWnd) ) return;
    if( !m_bSubclassed ) return;
    SubclassWindow(m_hWnd, m_OldWndProc);
    m_OldWndProc = ::DefWindowProc;
    m_bSubclassed = false;
}

void CWindowWnd::ShowWindow(bool bShow /*= true*/, bool bTakeFocus /*= false*/)
{
    ASSERT(::IsWindow(m_hWnd));
    if( !::IsWindow(m_hWnd) ) return;
    ::ShowWindow(m_hWnd, bShow ? (bTakeFocus ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE) : SW_HIDE);
}

UINT CWindowWnd::ShowModal()
{
    ASSERT(::IsWindow(m_hWnd));
    UINT nRet = 0;
    HWND hWndParent = GetWindowOwner(m_hWnd);
    ::ShowWindow(m_hWnd, SW_SHOWNORMAL);
    ::EnableWindow(hWndParent, FALSE);
    MSG msg = { 0 };
    while( ::IsWindow(m_hWnd) && ::GetMessage(&msg, nullptr, 0, 0) ) {
        if( msg.message == WM_CLOSE && msg.hwnd == m_hWnd ) {
            nRet = static_cast<int>(msg.wParam);
            ::EnableWindow(hWndParent, TRUE);
            ::SetFocus(hWndParent);
        }
        if( !CPaintManagerUI::TranslateMessage(&msg) ) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
        }
        if( msg.message == WM_QUIT ) break;
    }
    ::EnableWindow(hWndParent, TRUE);
    ::SetFocus(hWndParent);
    if( msg.message == WM_QUIT ) ::PostQuitMessage(static_cast<int>(msg.wParam));
    return nRet;
}

void CWindowWnd::ShowModalFake()
{
    ASSERT(::IsWindow(m_hWnd));
    auto p_hwnd = GetWindowOwner(m_hWnd);
    ASSERT(::IsWindow(p_hwnd));
    ASSERT(p_hwnd);
    ::EnableWindow(p_hwnd, FALSE);
    ShowWindow();
    m_bFakeModal = true;
}

void CWindowWnd::Close(UINT nRet)
{
    if (m_bFakeModal)
    {
        auto parent_hwnd = GetWindowOwner(m_hWnd);
        ASSERT(::IsWindow(parent_hwnd));
        ::EnableWindow(parent_hwnd, TRUE);
        ::SetFocus(parent_hwnd);
        m_bFakeModal = false;
    }

    ASSERT(::IsWindow(m_hWnd));
    if (!::IsWindow(m_hWnd)) return;
    PostMessage(WM_CLOSE, static_cast<WPARAM>(nRet), 0L);
}

void CWindowWnd::CenterWindow()
{
    ASSERT(::IsWindow(m_hWnd));
    ASSERT((GetWindowStyle(m_hWnd)&WS_CHILD)==0);
    RECT rcDlg = { 0 };
    ::GetWindowRect(m_hWnd, &rcDlg);
    RECT rcArea = { 0 };
    RECT rcCenter = { 0 };
	HWND hWnd=*this;
    HWND hWndParent = ::GetParent(m_hWnd);
    HWND hWndCenter = ::GetWindowOwner(m_hWnd);
	if (hWndCenter!=nullptr)
		hWnd=hWndCenter;

	// 处理多显示器模式下屏幕居中
	MONITORINFO oMonitor = {};
	oMonitor.cbSize = sizeof(oMonitor);
	::GetMonitorInfo(::MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST), &oMonitor);
	rcArea = oMonitor.rcWork;

    if( hWndCenter == nullptr )
		rcCenter = rcArea;
	else
		::GetWindowRect(hWndCenter, &rcCenter);

    int DlgWidth = rcDlg.right - rcDlg.left;
    int DlgHeight = rcDlg.bottom - rcDlg.top;

    // Find dialog's upper left based on rcCenter
    int xLeft = (rcCenter.left + rcCenter.right) / 2 - DlgWidth / 2;
    int yTop = (rcCenter.top + rcCenter.bottom) / 2 - DlgHeight / 2;

    // The dialog is outside the screen, move it inside
    if( xLeft < rcArea.left ) xLeft = rcArea.left;
    else if( xLeft + DlgWidth > rcArea.right ) xLeft = rcArea.right - DlgWidth;
    if( yTop < rcArea.top ) yTop = rcArea.top;
    else if( yTop + DlgHeight > rcArea.bottom ) yTop = rcArea.bottom - DlgHeight;
    ::SetWindowPos(m_hWnd, nullptr, xLeft, yTop, -1, -1, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void CWindowWnd::SetIcon(UINT nRes)
{
	HICON hIcon = reinterpret_cast<HICON>(::LoadImage(CPaintManagerUI::GetInstance(), MAKEINTRESOURCE(nRes), IMAGE_ICON,
		(::GetSystemMetrics(SM_CXICON) + 15) & ~15, (::GetSystemMetrics(SM_CYICON) + 15) & ~15,	// 防止高DPI下图标模糊
		LR_DEFAULTCOLOR));
	ASSERT(hIcon);
	::SendMessage(m_hWnd, WM_SETICON, static_cast<WPARAM>(TRUE), reinterpret_cast<LPARAM>(hIcon));

	hIcon = reinterpret_cast<HICON>(::LoadImage(CPaintManagerUI::GetInstance(), MAKEINTRESOURCE(nRes), IMAGE_ICON,
		(::GetSystemMetrics(SM_CXICON) + 15) & ~15, (::GetSystemMetrics(SM_CYICON) + 15) & ~15,	// 防止高DPI下图标模糊
		LR_DEFAULTCOLOR));
	ASSERT(hIcon);
	::SendMessage(m_hWnd, WM_SETICON, static_cast<WPARAM>(FALSE), reinterpret_cast<LPARAM>(hIcon));
}

bool CWindowWnd::RegisterWindowClass()
{
    WNDCLASS wc = { 0 };
    wc.style = GetClassStyle();
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hIcon = nullptr;
    wc.lpfnWndProc = CWindowWnd::__WndProc;
    wc.hInstance = CPaintManagerUI::GetInstance();
    wc.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszMenuName  = nullptr;
    wc.lpszClassName = GetWindowClassName();
    ATOM ret = ::RegisterClass(&wc);
    ASSERT(ret!=0 || ::GetLastError()==ERROR_CLASS_ALREADY_EXISTS);
    return ret != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool CWindowWnd::RegisterSuperclass()
{
	if(m_bUnicode) {
#ifndef UNICODE
		LPWSTR lpSuperClassName = a2w(const_cast<char*>(GetSuperClassName()));
		LPWSTR lpClassName = a2w(const_cast<char*>(GetWindowClassName()));
#else
		LPWSTR lpSuperClassName = const_cast<LPWSTR>(GetSuperClassName());
		LPWSTR lpClassName = const_cast<LPWSTR>(GetWindowClassName());
#endif

		// Get the class information from an existing
		// window so we can subclass it later on...
		WNDCLASSEXW wc = { 0 };
		wc.cbSize = sizeof(WNDCLASSEXW);
		if( !::GetClassInfoExW(nullptr, lpSuperClassName, &wc) ) {
			if( !::GetClassInfoExW(CPaintManagerUI::GetInstance(), lpSuperClassName, &wc) ) {
				ASSERT(!"Unable to locate window class");
				return false;
			}
		}
		m_OldWndProc = wc.lpfnWndProc;
		wc.lpfnWndProc = CWindowWnd::__ControlProc;
		wc.hInstance = CPaintManagerUI::GetInstance();
		wc.lpszClassName = lpClassName;
		ATOM ret = ::RegisterClassExW(&wc);
#ifndef UNICODE
		delete []lpClassName;
		delete []lpSuperClassName;
#endif
		ASSERT(ret!=0 || ::GetLastError()==ERROR_CLASS_ALREADY_EXISTS);
		return ret != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
	}
	else {
		// Get the class information from an existing
		// window so we can subclass it later on...
		WNDCLASSEX wc = { 0 };
		wc.cbSize = sizeof(WNDCLASSEX);
		if( !::GetClassInfoEx(nullptr, GetSuperClassName(), &wc) ) {
			if( !::GetClassInfoEx(CPaintManagerUI::GetInstance(), GetSuperClassName(), &wc) ) {
				ASSERT(!"Unable to locate window class");
				return false;
			}
		}
		m_OldWndProc = wc.lpfnWndProc;
		wc.lpfnWndProc = CWindowWnd::__ControlProc;
		wc.hInstance = CPaintManagerUI::GetInstance();
		wc.lpszClassName = GetWindowClassName();
		ATOM ret = ::RegisterClassEx(&wc);
		ASSERT(ret!=0 || ::GetLastError()==ERROR_CLASS_ALREADY_EXISTS);
		return ret != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
	}
}

LRESULT CALLBACK CWindowWnd::__WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    CWindowWnd* pThis = nullptr;
    if( uMsg == WM_NCCREATE ) {
        LPCREATESTRUCT lpcs = reinterpret_cast<LPCREATESTRUCT>(lParam);
        pThis = static_cast<CWindowWnd*>(lpcs->lpCreateParams);
        pThis->m_hWnd = hWnd;
        ::SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LPARAM>(pThis));
    } 
    else {
        pThis = reinterpret_cast<CWindowWnd*>(::GetWindowLongPtr(hWnd, GWLP_USERDATA));
        if( uMsg == WM_NCDESTROY && pThis != nullptr ) {
            LRESULT lRes = ::CallWindowProc(pThis->m_OldWndProc, hWnd, uMsg, wParam, lParam);
            ::SetWindowLongPtr(pThis->m_hWnd, GWLP_USERDATA, 0L);
            if( pThis->m_bSubclassed ) pThis->Unsubclass();
            pThis->m_hWnd = nullptr;
            pThis->OnFinalMessage(hWnd);
            return lRes;
        }
    }
    if( pThis != nullptr ) {
        return pThis->HandleMessage(uMsg, wParam, lParam);
    } 
    else {
        return ::DefWindowProc(hWnd, uMsg, wParam, lParam);
    }
}

LRESULT CALLBACK CWindowWnd::__ControlProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    CWindowWnd* pThis = nullptr;
    if( uMsg == WM_NCCREATE ) {
        LPCREATESTRUCT lpcs = reinterpret_cast<LPCREATESTRUCT>(lParam);
        pThis = static_cast<CWindowWnd*>(lpcs->lpCreateParams);
        ::SetProp(hWnd, _T("WndX"), reinterpret_cast<HANDLE>(pThis));
        pThis->m_hWnd = hWnd;
    } 
    else {
        pThis = reinterpret_cast<CWindowWnd*>(::GetProp(hWnd, _T("WndX")));
        if( uMsg == WM_NCDESTROY && pThis != nullptr ) {
            LRESULT lRes = ::CallWindowProc(pThis->m_OldWndProc, hWnd, uMsg, wParam, lParam);
            if( pThis->m_bSubclassed ) pThis->Unsubclass();
            ::SetProp(hWnd, _T("WndX"), nullptr);
            pThis->m_hWnd = nullptr;
            pThis->OnFinalMessage(hWnd);
            return lRes;
        }
    }
    if( pThis != nullptr ) {
        return pThis->HandleMessage(uMsg, wParam, lParam);
    } 
    else {
        return ::DefWindowProc(hWnd, uMsg, wParam, lParam);
    }
}

LRESULT CWindowWnd::SendMessage(UINT uMsg, WPARAM wParam /*= 0*/, LPARAM lParam /*= 0*/)
{
    ASSERT(::IsWindow(m_hWnd));
    return ::SendMessage(m_hWnd, uMsg, wParam, lParam);
} 

LRESULT CWindowWnd::PostMessage(UINT uMsg, WPARAM wParam /*= 0*/, LPARAM lParam /*= 0*/)
{
    ASSERT(::IsWindow(m_hWnd));
    return ::PostMessage(m_hWnd, uMsg, wParam, lParam);
}

void CWindowWnd::ResizeClient(int cx /*= -1*/, int cy /*= -1*/)
{
    ASSERT(::IsWindow(m_hWnd));
    RECT rc = { 0 };
    if( !::GetClientRect(m_hWnd, &rc) ) return;
    if( cx != -1 ) rc.right = cx;
    if( cy != -1 ) rc.bottom = cy;
    if( !::AdjustWindowRectEx(&rc, GetWindowStyle(m_hWnd), (!(GetWindowStyle(m_hWnd) & WS_CHILD) && (::GetMenu(m_hWnd) != nullptr)), GetWindowExStyle(m_hWnd)) ) return;
    ::SetWindowPos(m_hWnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
}

LRESULT CWindowWnd::HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    return ::CallWindowProc(m_OldWndProc, m_hWnd, uMsg, wParam, lParam);
}

void CWindowWnd::OnFinalMessage(HWND /*hWnd*/)
{
}

	//////////////////////////////////////////////////////////////////////////
	// [WIN11-01] Windows 11 native appearance
	//
	// Everything is resolved at runtime from dwmapi.dll / user32.dll. If the OS
	// or the SDK predates a capability we simply skip it: an unsupported
	// DwmSetWindowAttribute returns E_INVALIDARG, which is not an error for us.
	namespace
	{
		// Undocumented-but-stable DWM attribute ids. Declared locally so the code
		// compiles against SDKs that do not define them yet.
		enum : DWORD
		{
			kDwmUseImmersiveDarkMode = 20,
			kDwmWindowCornerPreference = 33,
			kDwmBorderColor = 34,
			kDwmSystemBackdropType = 38,
		};

		enum : DWORD
		{
			kCornerDefault = 0,
			kCornerDoNotRound = 1,
			kCornerRound = 2,
			kCornerRoundSmall = 3,
		};

		typedef HRESULT(WINAPI* PFN_DwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);

		PFN_DwmSetWindowAttribute ResolveDwmSetWindowAttribute()
		{
			static PFN_DwmSetWindowAttribute s_pfn = []() -> PFN_DwmSetWindowAttribute
			{
				HMODULE hDwm = ::LoadLibraryW(L"dwmapi.dll");
				if (hDwm == nullptr) return nullptr;
				return reinterpret_cast<PFN_DwmSetWindowAttribute>(
					::GetProcAddress(hDwm, "DwmSetWindowAttribute"));
			}();
			return s_pfn;
		}
	}

	UINT CWindowWnd::ApplyWin11Effects(bool bDark, int nBackdrop)
	{
		UINT uApplied = 0;
		if (m_hWnd == nullptr) return uApplied;

		PFN_DwmSetWindowAttribute pSet = ResolveDwmSetWindowAttribute();
		if (pSet == nullptr) return uApplied;   // pre-Vista / no DWM

		// 1) Rounded corners (Win11 22000+). Older builds reject the attribute.
		{
			DWORD dwPref = kCornerRound;
			if (SUCCEEDED(pSet(m_hWnd, kDwmWindowCornerPreference, &dwPref, sizeof(dwPref))))
				uApplied |= kWin11RoundedCorner;
		}

		// 2) Dark title bar (Win10 1903+ / Win11).
		{
			BOOL bUseDark = bDark ? TRUE : FALSE;
			if (SUCCEEDED(pSet(m_hWnd, kDwmUseImmersiveDarkMode, &bUseDark, sizeof(bUseDark))))
				uApplied |= kWin11DarkTitleBar;
		}

		// 3) Mica / Acrylic backdrop (Win11 22621+). Requires the window to be
		//    non-opaque for the material to show through; the caller keeps
		//    responsibility for its own paint mode.
		if (nBackdrop > 0)
		{
			int nType = (nBackdrop == 1) ? 2      // DWMSBT_MAINWINDOW  = Mica
				: (nBackdrop == 2) ? 3            // DWMSBT_TRANSIENTWINDOW = Acrylic
				: 4;                              // DWMSBT_TABBEDWINDOW
			if (SUCCEEDED(pSet(m_hWnd, kDwmSystemBackdropType, &nType, sizeof(nType))))
				uApplied |= kWin11Backdrop;
		}

		// 4) Border colour following the theme (Win11 22000+). Passing
		//    DWMWA_COLOR_DEFAULT restores the system value.
		{
			DWORD dwBorder = 0xFFFFFFFF;   // DWMWA_COLOR_DEFAULT
			if (bDark) dwBorder = 0xFF2B2B2B;
			if (SUCCEEDED(pSet(m_hWnd, kDwmBorderColor, &dwBorder, sizeof(dwBorder))))
				uApplied |= kWin11BorderColor;
		}

		return uApplied;
	}

} // namespace DuiLib
