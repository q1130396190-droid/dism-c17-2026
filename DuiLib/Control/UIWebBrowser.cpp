#include "StdAfx.h"

#include "S3AttrRegistry.h"

#include "UIWebBrowser.h"
#include <atlconv.h>
#include <atlcomcli.h>
#include "../Utils/downloadmgr.h"
#include <mshtml.h>
#include <cstdio>      // _snprintf_s for the opt-in [WV2-*] diagnostics

namespace DuiLib
{

	// [WV2-DIAG] Append one narrow ASCII line to the opt-in diagnostic file.
	//
	// Every controller call used by this class used to be fire-and-forget: put_Bounds
	// and put_IsVisible both discarded their HRESULT, so a rejected call was
	// indistinguishable from a successful one. That blind spot is what allowed
	// ERROR_INVALID_STATE to survive eleven rounds of investigation.
	//
	// Opt-in on purpose: without DUILIB_WV2_DIAG_LOG this does nothing at all, so the
	// production path pays no file I/O.
	static void Wv2DiagLog(const char* pszLine)
	{
		if (pszLine == nullptr) return;
		WCHAR szPath[MAX_PATH] = { 0 };
		if (::GetEnvironmentVariableW(L"DUILIB_WV2_DIAG_LOG", szPath, MAX_PATH) <= 0
			|| szPath[0] == L'\0')
			return;
		HANDLE hLog = ::CreateFileW(szPath, FILE_APPEND_DATA, FILE_SHARE_READ,
			nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (hLog == INVALID_HANDLE_VALUE) return;
		DWORD dwWritten = 0;
		::WriteFile(hLog, pszLine, static_cast<DWORD>(strlen(pszLine)), &dwWritten, nullptr);
		::CloseHandle(hLog);
	}



	////////////////////////////////////////////////////////////////////////
	//
	IMPLEMENT_DUICONTROL(CWebBrowserUI)

		CWebBrowserUI::CWebBrowserUI()
		: m_pWebBrowser2(nullptr)
		, _pHtmlWnd2(nullptr)
		, m_pWebBrowserEventHandler(nullptr)
		, m_bAutoNavi(false)
		, m_dwRef(0)
		, m_dwCookie(0)
#if defined(DUILIB_USE_WEBVIEW2) && DUILIB_USE_WEBVIEW2
		, m_bUseWebView2(false)
		, m_pWebView2Env(nullptr)
		, m_pWebView2Controller(nullptr)
		, m_pWebView2(nullptr)
		, m_uWebView2NotifyMsg(0)
		, m_nWebView2Generation(0)
		, m_bWebView2Creating(false)
		, m_pWebView2Lifetime(nullptr)
		, m_uWv2EventMask(0)
#endif
		, m_nWebView2Mode(-1)
	{
		m_clsid=CLSID_WebBrowser;
		m_sHomePage.Empty();
#if defined(DUILIB_USE_WEBVIEW2) && DUILIB_USE_WEBVIEW2
		// [WV2-LIFETIME] One token for this control; sinks take a counted reference.
		m_pWebView2Lifetime = new CWebView2Lifetime(this);
		// [WV2-TOKENS] Zero-initialise: 0 is the "not registered" sentinel, and
		// Wv2UnbindEvents uses m_uWv2EventMask to know which removals are valid.
		::ZeroMemory(&m_tokNavStarting, sizeof(m_tokNavStarting));
		::ZeroMemory(&m_tokNavCompleted, sizeof(m_tokNavCompleted));
		::ZeroMemory(&m_tokTitleChanged, sizeof(m_tokTitleChanged));
		::ZeroMemory(&m_tokNewWindow, sizeof(m_tokNewWindow));
		::ZeroMemory(&m_tokHistoryChanged, sizeof(m_tokHistoryChanged));
		::ZeroMemory(&m_tokWebMessage, sizeof(m_tokWebMessage));
#endif
#if defined(DUILIB_USE_WEBVIEW2) && DUILIB_USE_WEBVIEW2
		// 每个控件实例注册独立的通知消息，避免同一窗口下多个 WebView2 控件的
		// 异步回调消息被其他控件（消息过滤器）劫持，导致控制器绑定/导航错乱。
		// 消息名含实例地址 + 全局递增序号：地址复用也不会与旧实例的消息串扰。
		static LONG s_nSeq = 0;
		TCHAR szMsg[128] = { 0 };
		_stprintf_s(szMsg, _T("DuiLib_WebView2_Notify_%p_%d"), reinterpret_cast<void*>(this), static_cast<int>(::InterlockedIncrement(&s_nSeq)));
		m_uWebView2NotifyMsg = ::RegisterWindowMessage(szMsg);
		if (m_uWebView2NotifyMsg == 0)
			m_uWebView2NotifyMsg = ::RegisterWindowMessage(_T("DuiLib_WebView2_Notify"));	// 兜底：共用消息
#endif
	}

	bool CWebBrowserUI::DoCreateControl()
	{
#if defined(DUILIB_USE_WEBVIEW2) && DUILIB_USE_WEBVIEW2
		// Win10+ 且 WebView2 Runtime 可用 → 使用 WebView2（异步创建）
		if (m_nWebView2Mode != 0 && IsWebView2Supported()) {
			// [WV2-REENTRY] Creating a second controller for the same control leaves the
			// first one bound to the same HWND, and WebView2 then reports
			// ERROR_INVALID_STATE on every Navigate(). Measured: the framework log
			// contained TWO [WV2-CREATECTRL] lines for one control, and navigation
			// failed only in that configuration. Refuse re-entry outright.
			if (m_bWebView2Creating) {
				Wv2DiagLog("[WV2-REENTRY] DoCreateControl re-entered while a controller "
				           "creation is in flight -- ignored\n");
				return true;
			}
			m_bWebView2Creating = true;
			m_bUseWebView2 = true;
			m_bCreated = true;
			if (m_pManager != nullptr) m_pManager->AddMessageFilter(this);
			if (InitWebView2()) return true;
			m_bWebView2Creating = false;
			// [WV2-ROLLBACK] A synchronous failure used to leave m_bCreated and
			// m_bUseWebView2 set with no backend behind them: the control reported
			// "created, WebView2 mode" while nothing had been created. Undo the
			// state and fall through to the IE path so one of them really works.
			m_bCreated = false;
			m_bUseWebView2 = false;
			if (m_pManager != nullptr) m_pManager->RemoveMessageFilter(this);
			ReleaseWebView2();
		}
		m_bUseWebView2 = false;
#endif
		return DoCreateActiveXBrowser();
	}

	// 老系统 / 无 WebView2 Runtime：继续使用 ActiveX WebBrowser（IE），原逻辑不变
	bool CWebBrowserUI::DoCreateActiveXBrowser()
	{
		if (!CActiveXUI::DoCreateControl())
			return false;
		GetManager()->AddTranslateAccelerator(this);
		GetControl(IID_IWebBrowser2,reinterpret_cast<LPVOID*>(&m_pWebBrowser2));
		// 优先导航创建前挂起的地址；否则按 autoNavi 打开默认主页
		if (!m_sPendingUrl.IsEmpty()) {
			Navigate2(m_sPendingUrl);
			m_sPendingUrl.Empty();
		}
		else if ( m_bAutoNavi && !m_sHomePage.IsEmpty())
		{
			this->Navigate2(m_sHomePage);
		}
		RegisterEventHandler(TRUE);
		return true;
	}

	void CWebBrowserUI::ReleaseControl()
	{
		m_bCreated = false;
		if (m_pManager != nullptr) {
			m_pManager->RemoveTranslateAccelerator(this);
			m_pManager->RemoveMessageFilter(this);
		}
#if defined(DUILIB_USE_WEBVIEW2) && DUILIB_USE_WEBVIEW2
		// [WV2-LIFETIME] Invalidate FIRST, before freeing anything: from this point
		// on every sink resolves the owner to nullptr, so an in-flight event can no
		// longer reach a half-destroyed control.
		Wv2DiagLog(m_pWebView2Lifetime != nullptr
			? "[WV2-TEARDOWN] ReleaseControl: invalidating lifetime\n"
			: "[WV2-TEARDOWN] ReleaseControl: NO lifetime to invalidate\n");
		if (m_pWebView2Lifetime != nullptr) m_pWebView2Lifetime->Invalidate();
		const bool bWasWebView2 = m_bUseWebView2;
		if (bWasWebView2) {
			ReleaseWebView2();
			m_bUseWebView2 = false;
		}
		// Drop queued async messages and their COM references. Destroying the window
		// discards queued messages without any handler, so this is the only place
		// that can release them.
		Wv2DrainQueuedMessages(this);
		if (m_pWebView2Lifetime != nullptr) {
			m_pWebView2Lifetime->Release();
			m_pWebView2Lifetime = nullptr;
		}
		if (bWasWebView2) return;
#endif
		RegisterEventHandler(FALSE);
		CActiveXUI::ReleaseControl();
	}

	CWebBrowserUI::~CWebBrowserUI()
	{
		ReleaseControl();
	}

	STDMETHODIMP CWebBrowserUI::GetTypeInfoCount( UINT *iTInfo )
	{
		*iTInfo = 0;
		return E_NOTIMPL;
	}

	STDMETHODIMP CWebBrowserUI::GetTypeInfo( UINT iTInfo, LCID lcid, ITypeInfo **ppTInfo )
	{
		return E_NOTIMPL;
	}

	STDMETHODIMP CWebBrowserUI::GetIDsOfNames( REFIID riid, OLECHAR **rgszNames, UINT cNames, LCID lcid,DISPID *rgDispId )
	{
		return E_NOTIMPL;
	}

	STDMETHODIMP CWebBrowserUI::Invoke( DISPID dispIdMember, REFIID riid, LCID lcid,WORD wFlags, DISPPARAMS* pDispParams,VARIANT* pVarResult, EXCEPINFO* pExcepInfo,UINT* puArgErr )
	{
		if ((riid != IID_NULL))
			return E_INVALIDARG;

		switch(dispIdMember)
		{
		case DISPID_BEFORENAVIGATE2:
			BeforeNavigate2(
				pDispParams->rgvarg[6].pdispVal,
				pDispParams->rgvarg[5].pvarVal,
				pDispParams->rgvarg[4].pvarVal,
				pDispParams->rgvarg[3].pvarVal,
				pDispParams->rgvarg[2].pvarVal,
				pDispParams->rgvarg[1].pvarVal,
				pDispParams->rgvarg[0].pboolVal);
			break;
		case DISPID_COMMANDSTATECHANGE:
			CommandStateChange(
				pDispParams->rgvarg[1].lVal,
				pDispParams->rgvarg[0].boolVal);
			break;
		case DISPID_NAVIGATECOMPLETE2:
			NavigateComplete2(
				pDispParams->rgvarg[1].pdispVal,
				pDispParams->rgvarg[0].pvarVal);
			break;
		case DISPID_NAVIGATEERROR:
			NavigateError(
				pDispParams->rgvarg[4].pdispVal,
				pDispParams->rgvarg[3].pvarVal,
				pDispParams->rgvarg[2].pvarVal,
				pDispParams->rgvarg[1].pvarVal,
				pDispParams->rgvarg[0].pboolVal);
			break;
		case DISPID_STATUSTEXTCHANGE:
			break;
			//  	case DISPID_NEWWINDOW2:
			//  		break;
		case DISPID_NEWWINDOW3:
			NewWindow3(
				pDispParams->rgvarg[4].ppdispVal,
				pDispParams->rgvarg[3].pboolVal,
				pDispParams->rgvarg[2].uintVal,
				pDispParams->rgvarg[1].bstrVal,
				pDispParams->rgvarg[0].bstrVal);
			break;
		case DISPID_TITLECHANGE:
			{
				TitleChange(pDispParams->rgvarg[0].bstrVal);
				break;
			}
		case DISPID_DOCUMENTCOMPLETE:
			{
				DocumentComplete(
					pDispParams->rgvarg[1].pdispVal,
					pDispParams->rgvarg[0].pvarVal);

				break;
			}
		default:
			return DISP_E_MEMBERNOTFOUND;
		}
		return S_OK;
	}

	STDMETHODIMP CWebBrowserUI::QueryInterface( REFIID riid, LPVOID *ppvObject )
	{
		*ppvObject = nullptr;

		if( riid == IID_IDocHostUIHandler)
			*ppvObject = static_cast<IDocHostUIHandler*>(this);
		else if( riid == IID_IDispatch)
			*ppvObject = static_cast<IDispatch*>(this);
		else if( riid == IID_IServiceProvider)
			*ppvObject = static_cast<IServiceProvider*>(this);
		else if(riid == IID_IInternetSecurityManager ) {
			*ppvObject = static_cast<IInternetSecurityManager*>(this);
		}
		else if (riid == IID_IOleCommandTarget)
			*ppvObject = static_cast<IOleCommandTarget*>(this);

		if( *ppvObject != nullptr )
			AddRef();
		return *ppvObject == nullptr ? E_NOINTERFACE : S_OK;
	}

	STDMETHODIMP_(ULONG) CWebBrowserUI::AddRef()
	{
		InterlockedIncrement(&m_dwRef); 
		return m_dwRef;
	}

	STDMETHODIMP_(ULONG) CWebBrowserUI::Release()
	{
		ULONG ulRefCount = InterlockedDecrement(&m_dwRef);
		return ulRefCount; 
	}

	void CWebBrowserUI::Navigate2( LPCTSTR lpszUrl )
	{
		if (lpszUrl == nullptr)
			return;

		// 控件尚未创建（延迟创建 / WebView2 异步初始化中）：先挂起地址，创建完成后导航
		if (!m_bCreated) {
			m_sPendingUrl = lpszUrl;
			return;
		}

#if defined(DUILIB_USE_WEBVIEW2) && DUILIB_USE_WEBVIEW2
		if (m_bUseWebView2)
		{
			if (m_pWebView2 != nullptr)
				m_pWebView2->Navigate(CT2CW(lpszUrl));
			else
				m_sPendingUrl = lpszUrl;	// 环境/控制器尚未就绪，挂起待导航
			return;
		}
#endif
		if (m_pWebBrowser2)
		{
			CDuiVariant url;
			url.vt=VT_BSTR;
			url.bstrVal=T2BSTR(lpszUrl);
			HRESULT hr = m_pWebBrowser2->Navigate2(&url, nullptr, nullptr, nullptr, nullptr);
		}
	}

	void CWebBrowserUI::Refresh()
	{
#if defined(DUILIB_USE_WEBVIEW2) && DUILIB_USE_WEBVIEW2
		if (m_bUseWebView2) { if (m_pWebView2 != nullptr) m_pWebView2->Reload(); return; }
#endif
		if (m_pWebBrowser2)
		{
			m_pWebBrowser2->Refresh();
		}
	}
	void CWebBrowserUI::GoBack()
	{
#if defined(DUILIB_USE_WEBVIEW2) && DUILIB_USE_WEBVIEW2
		if (m_bUseWebView2) { if (m_pWebView2 != nullptr) m_pWebView2->GoBack(); return; }
#endif
		if (m_pWebBrowser2)
		{
			m_pWebBrowser2->GoBack();
		}
	}
	void CWebBrowserUI::GoForward()
	{
#if defined(DUILIB_USE_WEBVIEW2) && DUILIB_USE_WEBVIEW2
		if (m_bUseWebView2) { if (m_pWebView2 != nullptr) m_pWebView2->GoForward(); return; }
#endif
		if (m_pWebBrowser2)
		{
			m_pWebBrowser2->GoForward();
		}
	}
	/// DWebBrowserEvents2
	void CWebBrowserUI::BeforeNavigate2( IDispatch *pDisp,VARIANT *&url,VARIANT *&Flags,VARIANT *&TargetFrameName,VARIANT *&PostData,VARIANT *&Headers,VARIANT_BOOL *&Cancel )
	{
		if (m_pWebBrowserEventHandler)
		{
			m_pWebBrowserEventHandler->BeforeNavigate2(this, pDisp,url,Flags,TargetFrameName,PostData,Headers,Cancel);
		}
	}

	void CWebBrowserUI::NavigateError( IDispatch *pDisp,VARIANT * &url,VARIANT *&TargetFrameName,VARIANT *&StatusCode,VARIANT_BOOL *&Cancel )
	{
		if (m_pWebBrowserEventHandler)
		{
			m_pWebBrowserEventHandler->NavigateError(this, pDisp,url,TargetFrameName,StatusCode,Cancel);
		}
	}

	void CWebBrowserUI::NavigateComplete2( IDispatch *pDisp,VARIANT *&url )
	{
		CComPtr<IDispatch> spDoc;   
		m_pWebBrowser2->get_Document(&spDoc);   

		if (spDoc)
		{   
			CComQIPtr<ICustomDoc, &IID_ICustomDoc> spCustomDoc(spDoc);   
			if (spCustomDoc)   
				spCustomDoc->SetUIHandler(this);   
		}

		if (m_pWebBrowserEventHandler)
		{
			m_pWebBrowserEventHandler->NavigateComplete2(this, pDisp,url);
		}
	}

	void CWebBrowserUI::ProgressChange( LONG nProgress, LONG nProgressMax )
	{
		if (m_pWebBrowserEventHandler)
		{
			m_pWebBrowserEventHandler->ProgressChange(this, nProgress,nProgressMax);
		}
	}

	void CWebBrowserUI::NewWindow3( IDispatch **pDisp, VARIANT_BOOL *&Cancel, DWORD dwFlags, BSTR bstrUrlContext, BSTR bstrUrl )
	{
		if (m_pWebBrowserEventHandler)
		{
			m_pWebBrowserEventHandler->NewWindow3(this, pDisp,Cancel,dwFlags,bstrUrlContext,bstrUrl);
		}
	}
	void CWebBrowserUI::CommandStateChange(long Command,VARIANT_BOOL Enable)
	{
		if (m_pWebBrowserEventHandler)
		{
			m_pWebBrowserEventHandler->CommandStateChange(this, Command,Enable);
		}
	}

	void CWebBrowserUI::TitleChange(BSTR bstrTitle)
	{
		if (m_pWebBrowserEventHandler)
		{
			m_pWebBrowserEventHandler->TitleChange(this, bstrTitle);
		}
	}

	void CWebBrowserUI::DocumentComplete(IDispatch *pDisp, VARIANT *&url)
	{
		if (m_pWebBrowserEventHandler)
		{
			m_pWebBrowserEventHandler->DocumentComplete(this, pDisp, url);
		}
	}

	// IDownloadManager
	STDMETHODIMP CWebBrowserUI::Download( /* [in] */ IMoniker *pmk, /* [in] */ IBindCtx *pbc, /* [in] */ DWORD dwBindVerb, /* [in] */ LONG grfBINDF, /* [in] */ BINDINFO *pBindInfo, /* [in] */ LPCOLESTR pszHeaders, /* [in] */ LPCOLESTR pszRedir, /* [in] */ UINT uiCP )
	{
		if (m_pWebBrowserEventHandler)
		{
			return m_pWebBrowserEventHandler->Download(this, pmk,pbc,dwBindVerb,grfBINDF,pBindInfo,pszHeaders,pszRedir,uiCP);
		}
		return S_OK;
	}

	// IDocHostUIHandler
	STDMETHODIMP CWebBrowserUI::ShowContextMenu( DWORD dwID, POINT* pptPosition, IUnknown* pCommandTarget, IDispatch* pDispatchObjectHit )
	{
		if (m_pWebBrowserEventHandler)
		{
			return m_pWebBrowserEventHandler->ShowContextMenu(this, dwID,pptPosition,pCommandTarget,pDispatchObjectHit);
		}
		return S_FALSE;
	}

	STDMETHODIMP CWebBrowserUI::GetHostInfo( DOCHOSTUIINFO* pInfo )
	{
		if (pInfo != nullptr) {
			pInfo->dwFlags |= DOCHOSTUIFLAG_NO3DBORDER | DOCHOSTUIFLAG_NO3DOUTERBORDER;
		}
		if (m_pWebBrowserEventHandler) {
			return m_pWebBrowserEventHandler->GetHostInfo(this, pInfo);
		}
		return S_OK;
	}

	STDMETHODIMP CWebBrowserUI::ShowUI( DWORD dwID, IOleInPlaceActiveObject* pActiveObject, IOleCommandTarget* pCommandTarget, IOleInPlaceFrame* pFrame, IOleInPlaceUIWindow* pDoc )
	{
		if (m_pWebBrowserEventHandler)
		{
			return m_pWebBrowserEventHandler->ShowUI(this, dwID,pActiveObject,pCommandTarget,pFrame,pDoc);
		}
		return S_OK;
	}

	STDMETHODIMP CWebBrowserUI::HideUI()
	{
		if (m_pWebBrowserEventHandler)
		{
			return m_pWebBrowserEventHandler->HideUI(this);
		}
		return S_OK;
	}

	STDMETHODIMP CWebBrowserUI::UpdateUI()
	{
		if (m_pWebBrowserEventHandler)
		{
			return m_pWebBrowserEventHandler->UpdateUI(this);
		}
		return S_OK;
	}

	STDMETHODIMP CWebBrowserUI::EnableModeless( BOOL fEnable )
	{
		if (m_pWebBrowserEventHandler)
		{
			return m_pWebBrowserEventHandler->EnableModeless(this, fEnable);
		}
		return E_NOTIMPL;
	}

	STDMETHODIMP CWebBrowserUI::OnDocWindowActivate( BOOL fActivate )
	{
		if (m_pWebBrowserEventHandler)
		{
			return m_pWebBrowserEventHandler->OnDocWindowActivate(this, fActivate);
		}
		return E_NOTIMPL;
	}

	STDMETHODIMP CWebBrowserUI::OnFrameWindowActivate( BOOL fActivate )
	{
		if (m_pWebBrowserEventHandler)
		{
			return m_pWebBrowserEventHandler->OnFrameWindowActivate(this, fActivate);
		}
		return E_NOTIMPL;
	}

	STDMETHODIMP CWebBrowserUI::ResizeBorder( LPCRECT prcBorder, IOleInPlaceUIWindow* pUIWindow, BOOL fFrameWindow )
	{
		if (m_pWebBrowserEventHandler)
		{
			return m_pWebBrowserEventHandler->ResizeBorder(this, prcBorder,pUIWindow,fFrameWindow);
		}
		return E_NOTIMPL;
	}

	STDMETHODIMP CWebBrowserUI::TranslateAccelerator( LPMSG lpMsg, const GUID* pguidCmdGroup, DWORD nCmdID )
	{
		if (m_pWebBrowserEventHandler)
		{
			return m_pWebBrowserEventHandler->TranslateAccelerator(this, lpMsg,pguidCmdGroup,nCmdID);
		}
		return S_FALSE;
	}

	LRESULT CWebBrowserUI::TranslateAccelerator( MSG *pMsg )
	{
		if(pMsg->message < WM_KEYFIRST || pMsg->message > WM_KEYLAST)
			return S_FALSE;

		if( m_pWebBrowser2 == nullptr )
			return E_NOTIMPL;

		// 当前Web窗口不是焦点,不处理加速键
		BOOL bIsChild = FALSE;
		HWND hTempWnd = nullptr;
		HWND hWndFocus = ::GetFocus();

		hTempWnd = hWndFocus;
		while(hTempWnd != nullptr)
		{
			if(hTempWnd == m_hwndHost)
			{
				bIsChild = TRUE;
				break;
			}
			hTempWnd = ::GetParent(hTempWnd);
		}
		if(!bIsChild)
			return S_FALSE;

		IOleInPlaceActiveObject *pObj;
		if (FAILED(m_pWebBrowser2->QueryInterface(IID_IOleInPlaceActiveObject, reinterpret_cast<LPVOID*>(&pObj))))
			return S_FALSE;

		HRESULT hResult = pObj->TranslateAccelerator(pMsg);
		pObj->Release();
		return hResult;
	}

	STDMETHODIMP CWebBrowserUI::GetOptionKeyPath( LPOLESTR* pchKey, DWORD dwReserved )
	{
		if (m_pWebBrowserEventHandler)
		{
			return m_pWebBrowserEventHandler->GetOptionKeyPath(this, pchKey,dwReserved);
		}
		return E_NOTIMPL;
	}

	STDMETHODIMP CWebBrowserUI::GetDropTarget( IDropTarget* pDropTarget, IDropTarget** ppDropTarget )
	{
		if (m_pWebBrowserEventHandler)
		{
			return m_pWebBrowserEventHandler->GetDropTarget(this, pDropTarget,ppDropTarget);
		}
		return S_FALSE;	// 使用系统拖拽
	}

	STDMETHODIMP CWebBrowserUI::GetExternal( IDispatch** ppDispatch )
	{
		if (m_pWebBrowserEventHandler)
		{
			return m_pWebBrowserEventHandler->GetExternal(this, ppDispatch);
		}
		return S_FALSE;
	}

	STDMETHODIMP CWebBrowserUI::TranslateUrl( DWORD dwTranslate, OLECHAR* pchURLIn, OLECHAR** ppchURLOut )
	{
		if (m_pWebBrowserEventHandler)
		{
			return m_pWebBrowserEventHandler->TranslateUrl(this, dwTranslate,pchURLIn,ppchURLOut);
		}
		else
		{
			*ppchURLOut = 0;
			return E_NOTIMPL;
		}
	}

	STDMETHODIMP CWebBrowserUI::FilterDataObject( IDataObject* pDO, IDataObject** ppDORet )
	{
		if (m_pWebBrowserEventHandler)
		{
			return m_pWebBrowserEventHandler->FilterDataObject(this, pDO,ppDORet);
		}
		else
		{
			*ppDORet = 0;
			return E_NOTIMPL;
		}
	}

	void CWebBrowserUI::SetWebBrowserEventHandler( CWebBrowserEventHandler* pEventHandler )
	{
		if ( pEventHandler!=nullptr && m_pWebBrowserEventHandler!=pEventHandler)
			m_pWebBrowserEventHandler=pEventHandler;
	}

	void CWebBrowserUI::Refresh2( int Level )
	{
#if defined(DUILIB_USE_WEBVIEW2) && DUILIB_USE_WEBVIEW2
		// WebView2 无刷新级别概念，等价于 Reload
		if (m_bUseWebView2) { if (m_pWebView2 != nullptr) m_pWebView2->Reload(); return; }
#endif
		CDuiVariant vLevel;
		vLevel.vt=VT_I4;
		vLevel.intVal=Level;
		m_pWebBrowser2->Refresh2(&vLevel);
	}

	void CWebBrowserUI::SetAttribute( LPCTSTR pstrName, LPCTSTR pstrValue )
	{
		// ---- S3/T3.8 batch 2: fast path via compile-time hash registry ----
		// O(log n) lookup instead of O(n) string compares.
		// Falls through to the legacy chain below when unmatched, so
		// third-party custom attributes keep working unchanged (D3 open).
		switch( DuiLib::s3::lookup_prop_t(pstrName) ) {
		case DuiLib::s3::PropId::homepage: { SetHomePage(pstrValue); return; }
		case DuiLib::s3::PropId::autonavi: { m_bAutoNavi = (_tcsicmp(pstrValue, _T("true")) == 0); return; }
		case DuiLib::s3::PropId::webview2: { SetWebView2Enabled(_tcsicmp(pstrValue, _T("false")) != 0); return; }
		default: break;
		}

		// ---- legacy fallback chain (kept verbatim for compatibility) ----

		if (_tcsicmp(pstrName, _T("homepage")) == 0)
		{
			m_sHomePage = pstrValue;
		}
		else if (_tcsicmp(pstrName, _T("autonavi"))==0)
		{
			m_bAutoNavi = (_tcsicmp(pstrValue, _T("true")) == 0);
		}
		else if (_tcsicmp(pstrName, _T("webview2"))==0)
		{
			// webview2="false" 强制使用老 WebBrowser（IE），默认自动判断
#if defined(DUILIB_USE_WEBVIEW2) && DUILIB_USE_WEBVIEW2
			SetWebView2Enabled(_tcsicmp(pstrValue, _T("false")) != 0);
#endif
		}
		else
			CActiveXUI::SetAttribute(pstrName, pstrValue);
	}

	void CWebBrowserUI::NavigateHomePage()
	{
		if (!m_sHomePage.IsEmpty())
			this->NavigateUrl(m_sHomePage);
	}

	void CWebBrowserUI::NavigateUrl( LPCTSTR lpszUrl )
	{
		// 统一走 Navigate2：自动处理创建前挂起与 WebView2/IE 双路分发
		Navigate2(lpszUrl);
	}

	LPCTSTR CWebBrowserUI::GetClass() const
	{
		return _T("WebBrowserUI");
	}

	LPVOID CWebBrowserUI::GetInterface( LPCTSTR pstrName )
	{
		if( _tcsicmp(pstrName, DUI_CTR_WEBBROWSER) == 0 ) return static_cast<CWebBrowserUI*>(this);
		return CActiveXUI::GetInterface(pstrName);
	}

	void CWebBrowserUI::SetHomePage( LPCTSTR lpszUrl )
	{
		m_sHomePage.Format(_T("%s"),lpszUrl);
	}

	LPCTSTR CWebBrowserUI::GetHomePage()
	{
		return m_sHomePage;
	}

	void CWebBrowserUI::SetAutoNavigation( bool bAuto /*= TRUE*/ )
	{
		if (m_bAutoNavi==bAuto)	return;

		m_bAutoNavi=bAuto;
	}

	bool CWebBrowserUI::IsAutoNavigation()
	{
		return m_bAutoNavi;
	}

	STDMETHODIMP CWebBrowserUI::QueryService( REFGUID guidService, REFIID riid, void** ppvObject )
	{
		HRESULT hr = E_NOINTERFACE;
		*ppvObject = nullptr;

		if ( guidService == SID_SDownloadManager && riid == IID_IDownloadManager)
		{
			*ppvObject = this;
			return S_OK;
		}
		if(guidService == SID_SInternetSecurityManager && riid == IID_IInternetSecurityManager) {
			*ppvObject = this;
			return S_OK;
		}
		return hr;
	}

	HRESULT CWebBrowserUI::RegisterEventHandler( BOOL inAdvise )
	{
		CComPtr<IWebBrowser2> pWebBrowser;
		CComPtr<IConnectionPointContainer>  pCPC;
		CComPtr<IConnectionPoint> pCP;
		HRESULT hr = GetControl(IID_IWebBrowser2, (void**)&pWebBrowser);
		if (FAILED(hr))
			return hr;
		hr=pWebBrowser->QueryInterface(IID_IConnectionPointContainer,(void **)&pCPC);
		if (FAILED(hr))
			return hr;
		hr=pCPC->FindConnectionPoint(DIID_DWebBrowserEvents2,&pCP);
		if (FAILED(hr))
			return hr;

		if (inAdvise)
		{
			hr = pCP->Advise(static_cast<IDispatch*>(this), &m_dwCookie);
		}
		else
		{
			hr = pCP->Unadvise(m_dwCookie);
		}
		return hr; 
	}

	DISPID CWebBrowserUI::FindId( IDispatch *pObj, LPOLESTR pName )
	{
		DISPID id = 0;
		if(FAILED(pObj->GetIDsOfNames(IID_NULL,&pName,1,LOCALE_SYSTEM_DEFAULT,&id))) id = -1;
		return id;
	}

	HRESULT CWebBrowserUI::InvokeMethod( IDispatch *pObj, LPOLESTR pMehtod, VARIANT *pVarResult, VARIANT *ps, int cArgs )
	{
		DISPID dispid = FindId(pObj, pMehtod);
		if(dispid == -1) return E_FAIL;

		DISPPARAMS dispparams;
		dispparams.cArgs = cArgs;
		dispparams.rgvarg = ps;
		dispparams.cNamedArgs = 0;
		dispparams.rgdispidNamedArgs = nullptr;

		return pObj->Invoke(dispid, IID_NULL, LOCALE_SYSTEM_DEFAULT, DISPATCH_METHOD, &dispparams, pVarResult, nullptr, nullptr);
	}

	HRESULT CWebBrowserUI::GetProperty( IDispatch *pObj, LPOLESTR pName, VARIANT *pValue )
	{
		DISPID dispid = FindId(pObj, pName);
		if(dispid == -1) return E_FAIL;

		DISPPARAMS ps;
		ps.cArgs = 0;
		ps.rgvarg = nullptr;
		ps.cNamedArgs = 0;
		ps.rgdispidNamedArgs = nullptr;

		return pObj->Invoke(dispid, IID_NULL, LOCALE_SYSTEM_DEFAULT, DISPATCH_PROPERTYGET, &ps, pValue, nullptr, nullptr);
	}

	HRESULT CWebBrowserUI::SetProperty( IDispatch *pObj, LPOLESTR pName, VARIANT *pValue )
	{
		DISPID dispid = FindId(pObj, pName);
		if(dispid == -1) return E_FAIL;

		DISPPARAMS ps;
		ps.cArgs = 1;
		ps.rgvarg = pValue;
		ps.cNamedArgs = 0;
		ps.rgdispidNamedArgs = nullptr;

		return pObj->Invoke(dispid, IID_NULL, LOCALE_SYSTEM_DEFAULT, DISPATCH_PROPERTYPUT, &ps, nullptr, nullptr, nullptr);
	}

	IDispatch* CWebBrowserUI::GetHtmlWindow()
	{
		IDispatch* pDp =  nullptr;
		HRESULT hr = E_FAIL;	// WebView2 模式下 m_pWebBrowser2 为空，避免使用未初始化变量
		if (m_pWebBrowser2)
			hr=m_pWebBrowser2->get_Document(&pDp);

		if (FAILED(hr))
			return nullptr;

		CComQIPtr<IHTMLDocument2> pHtmlDoc2 = pDp;

		if (pHtmlDoc2 == nullptr)
			return nullptr;

		hr=pHtmlDoc2->get_parentWindow(&_pHtmlWnd2);

		if (FAILED(hr))
			return nullptr;

		IDispatch *pHtmlWindown = nullptr;
		hr=_pHtmlWnd2->QueryInterface(IID_IDispatch, (void**)&pHtmlWindown);
		if (FAILED(hr))
			return nullptr;

		return pHtmlWindown;
	}

	IWebBrowser2* CWebBrowserUI::GetWebBrowser2( void )
	{
		return m_pWebBrowser2;
	}

	HRESULT STDMETHODCALLTYPE CWebBrowserUI::QueryStatus( __RPC__in_opt const GUID *pguidCmdGroup, ULONG cCmds, __RPC__inout_ecount_full(cCmds ) OLECMD prgCmds[ ], __RPC__inout_opt OLECMDTEXT *pCmdText )
	{
		HRESULT hr = OLECMDERR_E_NOTSUPPORTED;
		return hr;
	}

	HRESULT STDMETHODCALLTYPE CWebBrowserUI::Exec( __RPC__in_opt const GUID *pguidCmdGroup, DWORD nCmdID, DWORD nCmdexecopt, __RPC__in_opt VARIANT *pvaIn, __RPC__inout_opt VARIANT *pvaOut )
	{
		HRESULT hr = S_OK;

		if (pguidCmdGroup && IsEqualGUID(*pguidCmdGroup, CGID_DocHostCommandHandler))
		{

			switch (nCmdID) 
			{

			case OLECMDID_SHOWSCRIPTERROR:
				{
					IHTMLDocument2*             pDoc = nullptr;
					IHTMLWindow2*               pWindow = nullptr;
					IHTMLEventObj*              pEventObj = nullptr;
					BSTR                        rgwszNames[5] = 
					{ 
						SysAllocString(L"errorLine"),
						SysAllocString(L"errorCharacter"),
						SysAllocString(L"errorCode"),
						SysAllocString(L"errorMessage"),
						SysAllocString(L"errorUrl")
					};
					DISPID                      rgDispIDs[5];
					VARIANT                     rgvaEventInfo[5];
					DISPPARAMS                  params;
					BOOL                        fContinueRunningScripts = true;
					int                         i;

					params.cArgs = 0;
					params.cNamedArgs = 0;

					// Get the document that is currently being viewed.
					hr = pvaIn->punkVal->QueryInterface(IID_IHTMLDocument2, (void **) &pDoc);    
					// Get document.parentWindow.
					hr = pDoc->get_parentWindow(&pWindow);
					pDoc->Release();
					// Get the window.event object.
					hr = pWindow->get_event(&pEventObj);
					// Get the error info from the window.event object.
					for (i = 0; i < 5; i++) 
					{  
						// Get the property's dispID.
						hr = pEventObj->GetIDsOfNames(IID_NULL, &rgwszNames[i], 1, 
							LOCALE_SYSTEM_DEFAULT, &rgDispIDs[i]);
						// Get the value of the property.
						hr = pEventObj->Invoke(rgDispIDs[i], IID_NULL,
							LOCALE_SYSTEM_DEFAULT,
							DISPATCH_PROPERTYGET, &params, &rgvaEventInfo[i],
							nullptr, nullptr);
						SysFreeString(rgwszNames[i]);
					}

					// At this point, you would normally alert the user with 
					// the information about the error, which is now contained
					// in rgvaEventInfo[]. Or, you could just exit silently.

					(*pvaOut).vt = VT_BOOL;
					if (fContinueRunningScripts)
					{
						// Continue running scripts on the page.
						(*pvaOut).boolVal = VARIANT_TRUE;
					}
					else
					{
						// Stop running scripts on the page.
						(*pvaOut).boolVal = VARIANT_FALSE;   
					} 
					break;
				}
			default:
				hr = OLECMDERR_E_NOTSUPPORTED;
				break;
			}
		}
		else
		{
			hr = OLECMDERR_E_UNKNOWNGROUP;
		}
		return (hr);
	}

#if defined(DUILIB_USE_WEBVIEW2) && DUILIB_USE_WEBVIEW2
	////////////////////////////////////////////////////////////////////////
	// ==================== WebView2 集成实现 ====================
	// Win10+ 且已安装 WebView2 Runtime 时启用；否则走原 ActiveX WebBrowser（IE）路径
	//

	namespace
	{
		// [WV2-TOKENS] Which registrations actually succeeded. Only a set bit may be
		// passed to the matching remove_* call during teardown.
		enum : unsigned
		{
			WV2_EVT_NAV_STARTING  = 1u << 0,
			WV2_EVT_NAV_COMPLETED = 1u << 1,
			WV2_EVT_TITLE_CHANGED = 1u << 2,
			WV2_EVT_NEW_WINDOW    = 1u << 3,
			WV2_EVT_HISTORY       = 1u << 4,
			WV2_EVT_WEB_MSG       = 1u << 5
		};

		// COM 事件 sink 公共基类：实现 IUnknown 引用计数与 QueryInterface
		template <typename T>
		class CWebView2SinkBase : public T
		{
		public:
			CWebView2SinkBase(HWND hNotifyWnd, CWebView2Lifetime* pLife)
				: m_hNotifyWnd(hNotifyWnd), m_pLife(pLife), m_dwRef(1)
			{
				// [WV2-LIFETIME] Hold a counted reference so the token outlives this
				// sink; the token object is freed when the last holder drops it.
				if (m_pLife != nullptr) m_pLife->AddRef();
			}
			virtual ~CWebView2SinkBase()
			{
				if (m_pLife != nullptr) { m_pLife->Release(); m_pLife = nullptr; }
			}

			STDMETHOD(QueryInterface)(REFIID riid, void** ppvObject) override
			{
				if (riid == IID_IUnknown || riid == __uuidof(T)) {
					*ppvObject = static_cast<T*>(this);
					AddRef();
					return S_OK;
				}
				*ppvObject = nullptr;
				return E_NOINTERFACE;
			}
			STDMETHOD_(ULONG, AddRef)() override { return ::InterlockedIncrement(&m_dwRef); }
			STDMETHOD_(ULONG, Release)() override
			{
				ULONG ulRef = ::InterlockedDecrement(&m_dwRef);
				if (ulRef == 0) delete this;
				return ulRef;
			}
		protected:
			HWND m_hNotifyWnd;	// 用于把异步回调切回 UI 线程
			CWebView2Lifetime* m_pLife;
		private:
			LONG m_dwRef;
		};

		// 环境创建完成回调（可能在其他线程触发，PostMessage 切回 UI 线程）
		class CWebView2EnvCreatedSink
			: public CWebView2SinkBase<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>
		{
		public:
			CWebView2EnvCreatedSink(HWND hNotifyWnd, CWebView2Lifetime* pLife, UINT uMsg, LONGLONG nGen)
				: CWebView2SinkBase<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(hNotifyWnd, pLife), m_uMsg(uMsg), m_nGen(nGen) {}
			STDMETHOD(Invoke)(HRESULT result, ICoreWebView2Environment* pEnv) override
			{
				// [WV2-GENERATION] If the owner is gone (token invalidated) or the
				// window is gone, drop the payload here instead of posting it into a
				// queue that will never be drained.
				if (m_pLife == nullptr || m_pLife->Get() == nullptr || !::IsWindow(m_hNotifyWnd)) {
					if (pEnv != nullptr) pEnv->Release();
					return S_OK;
				}
				if (SUCCEEDED(result) && pEnv != nullptr) {
					pEnv->AddRef();	// 指针所有权随消息移交 UI 线程
					if (!::PostMessage(m_hNotifyWnd, m_uMsg, CWebBrowserUI::WEBVIEW2_MSG_CREATE_CONTROLLER, reinterpret_cast<LPARAM>(pEnv)))
						pEnv->Release();
				}
				else {
					::PostMessage(m_hNotifyWnd, m_uMsg, CWebBrowserUI::WEBVIEW2_MSG_FAILED, 0);
				}
				return S_OK;
			}
			LONGLONG Generation() const { return m_nGen; }
		private:
			UINT m_uMsg;	// 所属控件的实例专用通知消息，避免多控件串扰
			LONGLONG m_nGen;
		};

		// 控制器创建完成回调（可能在其他线程触发，PostMessage 切回 UI 线程）
		class CWebView2CtrlCreatedSink
			: public CWebView2SinkBase<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>
		{
		public:
			CWebView2CtrlCreatedSink(HWND hNotifyWnd, CWebView2Lifetime* pLife, UINT uMsg, LONGLONG nGen)
				: CWebView2SinkBase<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(hNotifyWnd, pLife), m_uMsg(uMsg), m_nGen(nGen) {}
			STDMETHOD(Invoke)(HRESULT result, ICoreWebView2Controller* pController) override
			{
				CWebBrowserUI* pOwner = (m_pLife != nullptr) ? m_pLife->Get() : nullptr;
				if (pOwner == nullptr || !::IsWindow(m_hNotifyWnd)) {
					if (pController != nullptr) pController->Release();
					return S_OK;
				}
				// [WV2-GENERATION] A controller that arrives for a superseded create
				// cycle would otherwise be bound into the new state.
				if (pOwner->GetWebView2Generation() != m_nGen) {
					if (pController != nullptr) pController->Release();
					return S_OK;
				}
				if (SUCCEEDED(result) && pController != nullptr) {
					pController->AddRef();	// 指针所有权随消息移交 UI 线程
					if (!::PostMessage(m_hNotifyWnd, m_uMsg, CWebBrowserUI::WEBVIEW2_MSG_BIND, reinterpret_cast<LPARAM>(pController)))
						pController->Release();
				}
				else {
					::PostMessage(m_hNotifyWnd, m_uMsg, CWebBrowserUI::WEBVIEW2_MSG_FAILED, 0);
				}
				return S_OK;
			}
		private:
			UINT m_uMsg;	// 所属控件的实例专用通知消息，避免多控件串扰
			LONGLONG m_nGen;
		};

		// 以下事件回调由 WebView2 在 UI 线程（创建控制器的线程）直接触发
		class CWebView2NavStartingSink
			: public CWebView2SinkBase<ICoreWebView2NavigationStartingEventHandler>
		{
		public:
			CWebView2NavStartingSink(CWebView2Lifetime* pLife, HWND hNotifyWnd)
				: CWebView2SinkBase<ICoreWebView2NavigationStartingEventHandler>(hNotifyWnd, pLife) {}
			STDMETHOD(Invoke)(ICoreWebView2* pWebView, ICoreWebView2NavigationStartingEventArgs* pArgs) override
			{
				// [WV2-LIFETIME] Resolve the owner through the token. A null result
				// means the control was destroyed while this event was queued: do
				// nothing instead of touching freed memory.
				CWebBrowserUI* pOwner = (m_pLife != nullptr) ? m_pLife->Get() : nullptr;
				if (pOwner != nullptr) pOwner->OnWebView2NavigationStarting(pWebView, pArgs);
				return S_OK;
			}
		};

		class CWebView2NavCompletedSink
			: public CWebView2SinkBase<ICoreWebView2NavigationCompletedEventHandler>
		{
		public:
			CWebView2NavCompletedSink(CWebView2Lifetime* pLife, HWND hNotifyWnd)
				: CWebView2SinkBase<ICoreWebView2NavigationCompletedEventHandler>(hNotifyWnd, pLife) {}
			STDMETHOD(Invoke)(ICoreWebView2* pWebView, ICoreWebView2NavigationCompletedEventArgs* pArgs) override
			{
				// [WV2-LIFETIME] Resolve the owner through the token. A null result
				// means the control was destroyed while this event was queued: do
				// nothing instead of touching freed memory.
				CWebBrowserUI* pOwner = (m_pLife != nullptr) ? m_pLife->Get() : nullptr;
				Wv2DiagLog(pOwner != nullptr
					? "[WV2-SINK] NavigationCompleted -> owner LIVE\n"
					: "[WV2-SINK] NavigationCompleted -> owner invalid\n");
				if (pOwner != nullptr) pOwner->OnWebView2NavigationCompleted(pWebView, pArgs);
				return S_OK;
			}
		};

		class CWebView2TitleChangedSink
			: public CWebView2SinkBase<ICoreWebView2DocumentTitleChangedEventHandler>
		{
		public:
			CWebView2TitleChangedSink(CWebView2Lifetime* pLife, HWND hNotifyWnd)
				: CWebView2SinkBase<ICoreWebView2DocumentTitleChangedEventHandler>(hNotifyWnd, pLife) {}
			STDMETHOD(Invoke)(ICoreWebView2* pWebView, IUnknown* pArgs) override
			{
				// [WV2-LIFETIME] Resolve the owner through the token. A null result
				// means the control was destroyed while this event was queued: do
				// nothing instead of touching freed memory.
				CWebBrowserUI* pOwner = (m_pLife != nullptr) ? m_pLife->Get() : nullptr;
				if (pOwner != nullptr) pOwner->OnWebView2DocumentTitleChanged(pWebView);
				return S_OK;
			}
		};

		class CWebView2NewWindowSink
			: public CWebView2SinkBase<ICoreWebView2NewWindowRequestedEventHandler>
		{
		public:
			CWebView2NewWindowSink(CWebView2Lifetime* pLife, HWND hNotifyWnd)
				: CWebView2SinkBase<ICoreWebView2NewWindowRequestedEventHandler>(hNotifyWnd, pLife) {}
			STDMETHOD(Invoke)(ICoreWebView2* pWebView, ICoreWebView2NewWindowRequestedEventArgs* pArgs) override
			{
				// [WV2-LIFETIME] Resolve the owner through the token. A null result
				// means the control was destroyed while this event was queued: do
				// nothing instead of touching freed memory.
				CWebBrowserUI* pOwner = (m_pLife != nullptr) ? m_pLife->Get() : nullptr;
				if (pOwner != nullptr) pOwner->OnWebView2NewWindowRequested(pWebView, pArgs);
				return S_OK;
			}
		};

		class CWebView2HistoryChangedSink
			: public CWebView2SinkBase<ICoreWebView2HistoryChangedEventHandler>
		{
		public:
			CWebView2HistoryChangedSink(CWebView2Lifetime* pLife, HWND hNotifyWnd)
				: CWebView2SinkBase<ICoreWebView2HistoryChangedEventHandler>(hNotifyWnd, pLife) {}
			STDMETHOD(Invoke)(ICoreWebView2* pWebView, IUnknown* pArgs) override
			{
				// [WV2-LIFETIME] Resolve the owner through the token. A null result
				// means the control was destroyed while this event was queued: do
				// nothing instead of touching freed memory.
				CWebBrowserUI* pOwner = (m_pLife != nullptr) ? m_pLife->Get() : nullptr;
				Wv2DiagLog(pOwner != nullptr
					? "[WV2-SINK] HistoryChanged -> owner LIVE\n"
					: "[WV2-SINK] HistoryChanged -> owner invalid\n");
				if (pOwner != nullptr) pOwner->OnWebView2HistoryChanged(pWebView);
				return S_OK;
			}
		};

		class CWebView2WebMessageSink
			: public CWebView2SinkBase<ICoreWebView2WebMessageReceivedEventHandler>
		{
		public:
			CWebView2WebMessageSink(CWebView2Lifetime* pLife, HWND hNotifyWnd)
				: CWebView2SinkBase<ICoreWebView2WebMessageReceivedEventHandler>(hNotifyWnd, pLife) {}
			STDMETHOD(Invoke)(ICoreWebView2* pWebView, ICoreWebView2WebMessageReceivedEventArgs* pArgs) override
			{
				// [WV2-LIFETIME] Resolve the owner through the token. A null result
				// means the control was destroyed while this event was queued: do
				// nothing instead of touching freed memory.
				CWebBrowserUI* pOwner = (m_pLife != nullptr) ? m_pLife->Get() : nullptr;
				if (pOwner != nullptr) pOwner->OnWebView2WebMessageReceived(pWebView, pArgs);
				return S_OK;
			}
		};
	} // namespace

	UINT CWebBrowserUI::GetWebView2NotifyMessage()
	{
		static UINT s_uMsg = ::RegisterWindowMessage(_T("DuiLib_WebView2_Notify"));
		return s_uMsg;
	}

	bool CWebBrowserUI::IsWebView2Mode() const
	{
		return m_bUseWebView2;
	}

	void CWebBrowserUI::SetWebView2Enabled(bool bEnable)
	{
		// 需在控件创建（DoCreateControl）之前调用
		m_nWebView2Mode = bEnable ? 1 : 0;
	}

	bool CWebBrowserUI::IsWebView2Supported()
	{
		// 分层窗口（如带阴影的异形窗口）不支持 WebView2 子窗口渲染，回退到 WebBrowser
		if (m_pManager != nullptr && m_pManager->IsLayered()) return false;
		return WebView2Helper::IsWebView2Supported();
	}

	ICoreWebView2* CWebBrowserUI::GetWebView2() const
	{
		return m_pWebView2;
	}

	void CWebBrowserUI::Stop()
	{
		if (m_bUseWebView2) {
			if (m_pWebView2 != nullptr) m_pWebView2->Stop();
			return;
		}
		if (m_pWebBrowser2) m_pWebBrowser2->Stop();
	}

	void CWebBrowserUI::PostWebMessage(LPCTSTR lpszMessage)
	{
		if (!m_bUseWebView2 || lpszMessage == nullptr) return;
		if (m_pWebView2 != nullptr)
			m_pWebView2->PostWebMessageAsString(CT2CW(lpszMessage));
	}

	bool CWebBrowserUI::InitWebView2()
	{
		WebView2Helper::PFN_CreateCoreWebView2EnvironmentWithOptions pfnCreateEnv = WebView2Helper::GetCreateEnvironmentFunc();
		if (pfnCreateEnv == nullptr) return false;

		HWND hNotifyWnd = (m_pManager != nullptr) ? m_pManager->GetPaintWindow() : nullptr;
		if (hNotifyWnd == nullptr) return false;
		if (m_pWebView2Lifetime == nullptr) return false;

		// [WV2-GENERATION] Every initialisation gets a fresh generation. Sinks and
		// async payloads built for this cycle carry it, so results that arrive after
		// a release/recreate are recognised as stale instead of being bound.
		const LONGLONG nGen = ::InterlockedIncrement64(&m_nWebView2Generation);

		// [WV2-USERDATA] Supply an explicit, per-process user-data folder.
		//
		// Passing nullptr makes WebView2 derive the folder from the executable path.
		// When the exe sits somewhere the process cannot write (a read-only share, a
		// service context, or a probe whose own directory is under source control),
		// environment creation still reports success, the core interface binds, and
		// then EVERY Navigate() returns 0x8007139F (ERROR_INVALID_STATE) forever --
		// measured on this machine, including for about:blank. Naming a writable
		// folder removes that whole failure mode.
		WCHAR szUserData[MAX_PATH] = { 0 };
		if (::GetTempPathW(MAX_PATH, szUserData) > 0)
		{
			wcscat_s(szUserData, L"DuiLib_WebView2");
			::CreateDirectoryW(szUserData, nullptr);   // best effort; already-exists is fine
		}

		CWebView2EnvCreatedSink* pSink = new CWebView2EnvCreatedSink(hNotifyWnd, m_pWebView2Lifetime, m_uWebView2NotifyMsg, nGen);
		HRESULT hr = pfnCreateEnv(nullptr,
		                          (szUserData[0] != L'\0') ? szUserData : nullptr,
		                          nullptr, (IUnknown*)pSink);
		pSink->Release();
		if (FAILED(hr)) return false;
		return true;	// 环境异步创建，完成后经 WM_WEBVIEW2_NOTIFY 消息切回 UI 线程
	}

	void CWebBrowserUI::ReleaseWebView2()
	{
		m_bWebView2Creating = false;
		// [WV2-GENERATION] Bump on release: any environment/controller completion that
		// is still in flight now belongs to a dead cycle and must be discarded.
		::InterlockedIncrement64(&m_nWebView2Generation);

		// [WV2-TOKENS] Unbind before releasing the objects that own the event sources.
		// Previously the tokens were throwaway locals, so nothing could ever be
		// removed and the sinks stayed registered on a dying WebView.
		Wv2UnbindEvents(this);

		if (m_pWebView2 != nullptr) { m_pWebView2->Release(); m_pWebView2 = nullptr; }
		if (m_pWebView2Controller != nullptr) {
			m_pWebView2Controller->Close();
			m_pWebView2Controller->Release();
			m_pWebView2Controller = nullptr;
		}
		if (m_pWebView2Env != nullptr) { m_pWebView2Env->Release(); m_pWebView2Env = nullptr; }
		Wv2DrainQueuedMessages(this);
		m_sPendingUrl.Empty();
	}

	// [WV2-DIAG] Observable controller visibility, for probes and support.
	bool CWebBrowserUI::IsWebView2ControllerVisible() const
	{
		if (m_pWebView2Controller == nullptr) return false;
		BOOL bVisible = FALSE;
		if (FAILED(m_pWebView2Controller->get_IsVisible(&bVisible))) return false;
		return bVisible != FALSE;
	}


	void CWebBrowserUI::UpdateWebView2Bounds()
	{
		if (m_pWebView2Controller == nullptr) return;
		// [WV2-DIAG] Report the HRESULT instead of discarding it. A rejected
		// put_Bounds leaves the controller at its previous (possibly zero) size, and
		// a zero-size controller is one documented cause of Navigate() returning
		// ERROR_INVALID_STATE -- which would have been invisible while this return
		// value was dropped.
		HRESULT hr = m_pWebView2Controller->put_Bounds(m_rcItem);
		{
			char szBuf[256] = { 0 };
			_snprintf_s(szBuf, _countof(szBuf), _TRUNCATE,
				"[WV2-BOUNDS] hr=0x%08lX rect=(%ld,%ld,%ld,%ld)\n",
				static_cast<unsigned long>(hr),
				static_cast<long>(m_rcItem.left), static_cast<long>(m_rcItem.top),
				static_cast<long>(m_rcItem.right), static_cast<long>(m_rcItem.bottom));
			Wv2DiagLog(szBuf);
		}
	}

	void CWebBrowserUI::OnWebView2Notify(WPARAM wParam, LPARAM lParam)
	{
		// [WV2-PAYLOAD] Every branch below owns the incoming reference exactly once.
		// The previous code returned early on several paths (null manager window,
		// stale state), which leaked the COM object carried in lParam.
		switch (wParam)
		{
		case WEBVIEW2_MSG_CREATE_CONTROLLER:
			{
				ICoreWebView2Environment* pEnv = reinterpret_cast<ICoreWebView2Environment*>(lParam);
				if (pEnv == nullptr) return;
				// Stale or already-torn-down: drop the reference, do not bind.
				if (!m_bUseWebView2 || m_pWebView2Lifetime == nullptr) { pEnv->Release(); return; }
				if (m_pWebView2Env != nullptr) m_pWebView2Env->Release();
				m_pWebView2Env = pEnv;	// 已 AddRef，直接持有

				HWND hNotifyWnd = (m_pManager != nullptr) ? m_pManager->GetPaintWindow() : nullptr;
				if (hNotifyWnd == nullptr) { ReleaseWebView2(); m_bUseWebView2 = false; DoCreateActiveXBrowser(); return; }
				CWebView2CtrlCreatedSink* pSink = new CWebView2CtrlCreatedSink(
					hNotifyWnd, m_pWebView2Lifetime, m_uWebView2NotifyMsg, m_nWebView2Generation);
				HRESULT hr = m_pWebView2Env->CreateCoreWebView2Controller(hNotifyWnd, pSink);
				pSink->Release();
				{
					// [WV2-DIAG] Same observation point as the standalone control probe,
					// so the two paths can be compared line by line instead of inferred.
					char szBuf[192] = { 0 };
					_snprintf_s(szBuf, _countof(szBuf), _TRUNCATE,
						"[WV2-CREATECTRL] hr=0x%08lX hwnd=%p\n",
						static_cast<unsigned long>(hr), static_cast<void*>(hNotifyWnd));
					Wv2DiagLog(szBuf);
				}
				if (FAILED(hr)) { ReleaseWebView2(); m_bUseWebView2 = false; DoCreateActiveXBrowser(); }
				break;
			}
		case WEBVIEW2_MSG_BIND:
			{
				ICoreWebView2Controller* pController = reinterpret_cast<ICoreWebView2Controller*>(lParam);
				if (pController == nullptr) return;
				if (!m_bUseWebView2 || m_pWebView2Lifetime == nullptr) { pController->Release(); return; }
				if (m_pWebView2Controller != nullptr) m_pWebView2Controller->Release();
				m_pWebView2Controller = pController;	// 已 AddRef，直接持有
				OnWebView2Bind();
				break;
			}
		case WEBVIEW2_MSG_FAILED:
			{
				// WebView2 创建失败（如 Runtime 缺失/被卸载），回退到老 WebBrowser
				m_bWebView2Creating = false;
				if (!m_bUseWebView2) return;
				ReleaseWebView2();
				m_bUseWebView2 = false;
				DoCreateActiveXBrowser();
				break;
			}
		default:
			break;
		}
	}

	void CWebBrowserUI::OnWebView2Bind()
	{
		m_bWebView2Creating = false;   // creation finished (successfully or not)
		if (m_pWebView2Controller == nullptr || m_pWebView2 != nullptr) return;
		if (FAILED(m_pWebView2Controller->get_CoreWebView2(&m_pWebView2))) return;

		HWND hNotifyWnd = (m_pManager != nullptr) ? m_pManager->GetPaintWindow() : nullptr;
		if (hNotifyWnd == nullptr || m_pWebView2Lifetime == nullptr) return;

		// 注册事件（事件处理器在 UI 线程回调，直接转发给 CWebBrowserEventHandler）
		//
		// [WV2-TOKENS] Each registration gets its OWN token, stored on the control, and
		// the bit is only set when the call actually succeeded. The previous code put
		// every registration into one reused local, so nothing could be unbound later
		// and a partial failure was indistinguishable from full success.
		m_uWv2EventMask = 0;
		CWebView2NavStartingSink* pNavStart = new CWebView2NavStartingSink(m_pWebView2Lifetime, hNotifyWnd);
		if (SUCCEEDED(m_pWebView2->add_NavigationStarting(pNavStart, &m_tokNavStarting)))
			m_uWv2EventMask |= WV2_EVT_NAV_STARTING;
		pNavStart->Release();

		CWebView2NavCompletedSink* pNavDone = new CWebView2NavCompletedSink(m_pWebView2Lifetime, hNotifyWnd);
		if (SUCCEEDED(m_pWebView2->add_NavigationCompleted(pNavDone, &m_tokNavCompleted)))
			m_uWv2EventMask |= WV2_EVT_NAV_COMPLETED;
		pNavDone->Release();

		CWebView2TitleChangedSink* pTitle = new CWebView2TitleChangedSink(m_pWebView2Lifetime, hNotifyWnd);
		if (SUCCEEDED(m_pWebView2->add_DocumentTitleChanged(pTitle, &m_tokTitleChanged)))
			m_uWv2EventMask |= WV2_EVT_TITLE_CHANGED;
		pTitle->Release();

		CWebView2NewWindowSink* pNewWin = new CWebView2NewWindowSink(m_pWebView2Lifetime, hNotifyWnd);
		if (SUCCEEDED(m_pWebView2->add_NewWindowRequested(pNewWin, &m_tokNewWindow)))
			m_uWv2EventMask |= WV2_EVT_NEW_WINDOW;
		pNewWin->Release();

		CWebView2HistoryChangedSink* pHistory = new CWebView2HistoryChangedSink(m_pWebView2Lifetime, hNotifyWnd);
		if (SUCCEEDED(m_pWebView2->add_HistoryChanged(pHistory, &m_tokHistoryChanged)))
			m_uWv2EventMask |= WV2_EVT_HISTORY;
		pHistory->Release();

		CWebView2WebMessageSink* pMsg = new CWebView2WebMessageSink(m_pWebView2Lifetime, hNotifyWnd);
		if (SUCCEEDED(m_pWebView2->add_WebMessageReceived(pMsg, &m_tokWebMessage)))
			m_uWv2EventMask |= WV2_EVT_WEB_MSG;
		pMsg->Release();

		// [WV2-BIND-DIAG] Unconditional bind-time state dump.
		//
		// Navigate() returning ERROR_INVALID_STATE means WebView2 considers the
		// controller unusable, and the two host-side causes are an empty bound
		// rectangle and a hidden controller at bind time. This line is outside every
		// if-branch on purpose: when something is missing here, a guarded log would
		// stay silent and hide exactly the case it exists to catch.
		{
			// Narrow ASCII buffer on purpose: this file is compiled in both UNICODE and
			// MBCS configurations, and a TCHAR format string with _stprintf_s picks a
			// different function per configuration. Fixed narrow output avoids the
			// whole class of type mismatch while still being readable in a debugger.
			const RECT& rcBind = m_rcItem;
			char szBuf[256] = { 0 };
			_snprintf_s(szBuf, _countof(szBuf), _TRUNCATE,
				"[WV2-BIND] rect=(%ld,%ld,%ld,%ld) visible=%d hostVisible=%d\n",
				static_cast<long>(rcBind.left), static_cast<long>(rcBind.top),
				static_cast<long>(rcBind.right), static_cast<long>(rcBind.bottom),
				IsVisible() ? 1 : 0,
				(m_pManager != nullptr && ::IsWindowVisible(hNotifyWnd)) ? 1 : 0);
			::OutputDebugStringA(szBuf);

			// Also append to a file when the caller opts in.
			//
			// OutputDebugStringA is invisible without a debugger, and this project has
			// already shipped a diagnostic that existed but could never be observed.
			// The opt-in env var keeps production silent while making the line
			// retrievable from a plain console run.
			WCHAR szPath[MAX_PATH] = { 0 };
			if (::GetEnvironmentVariableW(L"DUILIB_WV2_DIAG_LOG", szPath, MAX_PATH) > 0 && szPath[0] != L'\0')
			{
				HANDLE hLog = ::CreateFileW(szPath, FILE_APPEND_DATA, FILE_SHARE_READ,
					nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (hLog != INVALID_HANDLE_VALUE)
				{
					DWORD dwWritten = 0;
					::WriteFile(hLog, szBuf, static_cast<DWORD>(strlen(szBuf)), &dwWritten, nullptr);
					::CloseHandle(hLog);
				}
			}
		}

		UpdateWebView2Bounds();
		{
			// [WV2-DIAG] Same reasoning as put_Bounds: the result was discarded, so a
			// rejected visibility change could not be told apart from a successful one.
			const BOOL bWantVisible = IsVisible() ? TRUE : FALSE;
			HRESULT hrVis = m_pWebView2Controller->put_IsVisible(bWantVisible);
			char szBuf[192] = { 0 };
			_snprintf_s(szBuf, _countof(szBuf), _TRUNCATE,
				"[WV2-VISIBLE] hr=0x%08lX want=%d\n",
				static_cast<unsigned long>(hrVis), static_cast<int>(bWantVisible));
			Wv2DiagLog(szBuf);
		}

		// 导航挂起地址或默认主页
		if (!m_sPendingUrl.IsEmpty()) {
			Navigate2(m_sPendingUrl);
			m_sPendingUrl.Empty();
		}
		else if (m_bAutoNavi && !m_sHomePage.IsEmpty()) {
			Navigate2(m_sHomePage);
		}
	}

	// [WV2-TOKENS] Unbind every event that was successfully registered.
	//
	// Idempotent: the mask is cleared as each removal is issued, so a second call
	// (ReleaseControl after ReleaseWebView2, or a destructor after both) removes
	// nothing. Removals are guarded by the mask because passing an uninitialised or
	// already-removed token to remove_* is undefined behaviour.
	void Wv2UnbindEvents(CWebBrowserUI* pThis)
	{
		if (pThis == nullptr) return;
		// Aliases keep the body readable; these are the class's own private fields,
		// reachable because Wv2UnbindEvents is declared a friend.
		ICoreWebView2*& m_pWebView2 = pThis->m_pWebView2;
		unsigned& m_uWv2EventMask = pThis->m_uWv2EventMask;
		if (m_pWebView2 == nullptr) { m_uWv2EventMask = 0; return; }

		EventRegistrationToken& m_tokNavStarting = pThis->m_tokNavStarting;
		EventRegistrationToken& m_tokNavCompleted = pThis->m_tokNavCompleted;
		EventRegistrationToken& m_tokTitleChanged = pThis->m_tokTitleChanged;
		EventRegistrationToken& m_tokNewWindow = pThis->m_tokNewWindow;
		EventRegistrationToken& m_tokHistoryChanged = pThis->m_tokHistoryChanged;
		EventRegistrationToken& m_tokWebMessage = pThis->m_tokWebMessage;

		if (m_uWv2EventMask & WV2_EVT_NAV_STARTING) {
			m_pWebView2->remove_NavigationStarting(m_tokNavStarting);
			m_uWv2EventMask &= ~static_cast<unsigned>(WV2_EVT_NAV_STARTING);
		}
		if (m_uWv2EventMask & WV2_EVT_NAV_COMPLETED) {
			m_pWebView2->remove_NavigationCompleted(m_tokNavCompleted);
			m_uWv2EventMask &= ~static_cast<unsigned>(WV2_EVT_NAV_COMPLETED);
		}
		if (m_uWv2EventMask & WV2_EVT_TITLE_CHANGED) {
			m_pWebView2->remove_DocumentTitleChanged(m_tokTitleChanged);
			m_uWv2EventMask &= ~static_cast<unsigned>(WV2_EVT_TITLE_CHANGED);
		}
		if (m_uWv2EventMask & WV2_EVT_NEW_WINDOW) {
			m_pWebView2->remove_NewWindowRequested(m_tokNewWindow);
			m_uWv2EventMask &= ~static_cast<unsigned>(WV2_EVT_NEW_WINDOW);
		}
		if (m_uWv2EventMask & WV2_EVT_HISTORY) {
			m_pWebView2->remove_HistoryChanged(m_tokHistoryChanged);
			m_uWv2EventMask &= ~static_cast<unsigned>(WV2_EVT_HISTORY);
		}
		if (m_uWv2EventMask & WV2_EVT_WEB_MSG) {
			m_pWebView2->remove_WebMessageReceived(m_tokWebMessage);
			m_uWv2EventMask &= ~static_cast<unsigned>(WV2_EVT_WEB_MSG);
		}
		m_uWv2EventMask = 0;
	}

	// [WV2-PAYLOAD] Release the COM reference carried by a queued async message.
	// Only the two payload-carrying messages own a reference; FAILED carries none.
	// File-local, because it needs no access to the control at all.
	static void Wv2ReleaseAsyncPayload(LPARAM lParam)
	{
		IUnknown* p = reinterpret_cast<IUnknown*>(lParam);
		if (p != nullptr) p->Release();
	}

	// [WV2-PAYLOAD] Drop messages that are queued but will never be dispatched.
	//
	// PostMessage succeeded, then the window was destroyed (or the control detached)
	// before the queue was pumped. Windows discards undelivered messages without any
	// handler, so the AddRef taken before posting is never balanced -- the COM object
	// leaks for the life of the process. Draining here is the only opportunity to
	// release it. PeekMessage with PM_REMOVE + a private message id is used rather
	// than PostQuitMessage-style tricks because the message id is per-instance.
	void Wv2DrainQueuedMessages(CWebBrowserUI* pThis)
	{
		if (pThis == nullptr) return;
		const UINT m_uWebView2NotifyMsg = pThis->m_uWebView2NotifyMsg;
		CPaintManagerUI* m_pManager = pThis->m_pManager;
		if (m_uWebView2NotifyMsg == 0) return;
		if (m_pManager == nullptr) return;
		HWND hWnd = m_pManager->GetPaintWindow();
		if (hWnd == nullptr || !::IsWindow(hWnd)) {
			// No window: the queued messages died with it and nothing can be
			// recovered from here. Documented rather than silently ignored.
			return;
		}
		MSG msg;
		while (::PeekMessage(&msg, hWnd, m_uWebView2NotifyMsg, m_uWebView2NotifyMsg, PM_REMOVE))
		{
			if (msg.message != m_uWebView2NotifyMsg) break;
			// The message codes are CWebBrowserUI enum members; a free function must
			// qualify them or they are not visible here (C2065).
			if (msg.wParam == CWebBrowserUI::WEBVIEW2_MSG_CREATE_CONTROLLER ||
			    msg.wParam == CWebBrowserUI::WEBVIEW2_MSG_BIND)
				Wv2ReleaseAsyncPayload(msg.lParam);
		}
	}

	////////////////////////////////////////////////////////////////////////
	// WebView2 子窗口的布局/可见性/消息分发（与 ActiveX 宿主窗口不同，需单独处理）
	//

	void CWebBrowserUI::SetPos(RECT rc, bool bNeedInvalidate)
	{
		if (m_bUseWebView2) {
			CControlUI::SetPos(rc, bNeedInvalidate);
			if (!m_bCreated) DoCreateControl();
			UpdateWebView2Bounds();
			return;
		}
		CActiveXUI::SetPos(rc, bNeedInvalidate);
	}

	void CWebBrowserUI::Move(SIZE szOffset, bool bNeedInvalidate)
	{
		if (m_bUseWebView2) {
			CControlUI::Move(szOffset, bNeedInvalidate);
			UpdateWebView2Bounds();
			return;
		}
		CActiveXUI::Move(szOffset, bNeedInvalidate);
	}

	void CWebBrowserUI::SetVisible(bool bVisible)
	{
		if (m_bUseWebView2) {
			CControlUI::SetVisible(bVisible);
			if (m_pWebView2Controller != nullptr)
				m_pWebView2Controller->put_IsVisible(IsVisible() ? TRUE : FALSE);
			return;
		}
		CActiveXUI::SetVisible(bVisible);
	}

	void CWebBrowserUI::SetInternVisible(bool bVisible)
	{
		if (m_bUseWebView2) {
			CControlUI::SetInternVisible(bVisible);
			if (m_pWebView2Controller != nullptr)
				m_pWebView2Controller->put_IsVisible(IsVisible() ? TRUE : FALSE);
			return;
		}
		CActiveXUI::SetInternVisible(bVisible);
	}

	LRESULT CWebBrowserUI::MessageHandler(UINT uMsg, WPARAM wParam, LPARAM lParam, bool& bHandled)
	{
		if (m_bUseWebView2) {
			// WebView2 子窗口自行接收鼠标/键盘消息；这里只处理本实例的异步创建回调
			if (uMsg == m_uWebView2NotifyMsg) {
				OnWebView2Notify(wParam, lParam);
				bHandled = true;
				return 0;
			}
			return 0;
		}
		return CActiveXUI::MessageHandler(uMsg, wParam, lParam, bHandled);
	}

	////////////////////////////////////////////////////////////////////////
	// WebView2 事件 -> CWebBrowserEventHandler 翻译（老的事件处理代码无需改动）
	//

	void CWebBrowserUI::OnWebView2NavigationStarting(ICoreWebView2* pWebView, ICoreWebView2NavigationStartingEventArgs* pArgs)
	{
		if (m_pWebBrowserEventHandler == nullptr || pArgs == nullptr) return;

		LPWSTR lpszUri = nullptr;
		pArgs->get_Uri(&lpszUri);

		VARIANT vUrl; VariantInit(&vUrl);
		vUrl.vt = VT_BSTR;
		vUrl.bstrVal = SysAllocString(lpszUri != nullptr ? lpszUri : L"");
		VARIANT vFlags; VariantInit(&vFlags); vFlags.vt = VT_I4; vFlags.lVal = 0;
		VARIANT vFrame; VariantInit(&vFrame); vFrame.vt = VT_BSTR; vFrame.bstrVal = nullptr;
		VARIANT vPost; VariantInit(&vPost); vPost.vt = VT_EMPTY;
		VARIANT vHeaders; VariantInit(&vHeaders); vHeaders.vt = VT_EMPTY;
		VARIANT* pUrl = &vUrl;
		VARIANT* pFlags = &vFlags;
		VARIANT* pFrame = &vFrame;
		VARIANT* pPost = &vPost;
		VARIANT* pHeaders = &vHeaders;
		VARIANT_BOOL bCancel = VARIANT_FALSE;
		VARIANT_BOOL* pCancel = &bCancel;

		m_pWebBrowserEventHandler->BeforeNavigate2(this, nullptr, pUrl, pFlags, pFrame, pPost, pHeaders, pCancel);
		if (bCancel == VARIANT_TRUE) {
			BOOL bCancelled = TRUE;
			pArgs->put_Cancel(bCancelled);
		}
		VariantClear(&vUrl);
		if (lpszUri != nullptr) ::CoTaskMemFree(lpszUri);
	}

	void CWebBrowserUI::OnWebView2NavigationCompleted(ICoreWebView2* pWebView, ICoreWebView2NavigationCompletedEventArgs* pArgs)
	{
		if (m_pWebBrowserEventHandler == nullptr || pArgs == nullptr) return;

		BOOL bIsSuccess = FALSE;
		pArgs->get_IsSuccess(&bIsSuccess);

		LPWSTR lpszSource = nullptr;
		if (pWebView != nullptr) pWebView->get_Source(&lpszSource);
		VARIANT vUrl; VariantInit(&vUrl);
		vUrl.vt = VT_BSTR;
		vUrl.bstrVal = SysAllocString(lpszSource != nullptr ? lpszSource : L"");
		VARIANT* pUrl = &vUrl;

		if (bIsSuccess) {
			m_pWebBrowserEventHandler->NavigateComplete2(this, nullptr, pUrl);
			m_pWebBrowserEventHandler->DocumentComplete(this, nullptr, pUrl);
		}
		else {
			COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
			pArgs->get_WebErrorStatus(&status);
			VARIANT vFrame; VariantInit(&vFrame); vFrame.vt = VT_EMPTY;
			VARIANT vStatus; VariantInit(&vStatus); vStatus.vt = VT_I4; vStatus.lVal = static_cast<LONG>(status);
			VARIANT* pFrame = &vFrame;
			VARIANT* pStatus = &vStatus;
			VARIANT_BOOL bCancel = VARIANT_FALSE;
			VARIANT_BOOL* pCancel = &bCancel;
			m_pWebBrowserEventHandler->NavigateError(this, nullptr, pUrl, pFrame, pStatus, pCancel);
		}
		VariantClear(&vUrl);
		if (lpszSource != nullptr) ::CoTaskMemFree(lpszSource);
	}

	void CWebBrowserUI::OnWebView2DocumentTitleChanged(ICoreWebView2* pWebView)
	{
		if (m_pWebBrowserEventHandler == nullptr || pWebView == nullptr) return;
		LPWSTR lpszTitle = nullptr;
		if (SUCCEEDED(pWebView->get_DocumentTitle(&lpszTitle)) && lpszTitle != nullptr) {
			m_pWebBrowserEventHandler->TitleChange(this, lpszTitle);
			::CoTaskMemFree(lpszTitle);
		}
	}

	void CWebBrowserUI::OnWebView2NewWindowRequested(ICoreWebView2* pWebView, ICoreWebView2NewWindowRequestedEventArgs* pArgs)
	{
		if (pArgs == nullptr) return;

		LPWSTR lpszUri = nullptr;
		pArgs->get_Uri(&lpszUri);

		VARIANT_BOOL bCancel = VARIANT_FALSE;
		if (m_pWebBrowserEventHandler != nullptr) {
			VARIANT_BOOL* pCancel = &bCancel;
			m_pWebBrowserEventHandler->NewWindow3(this, nullptr, pCancel, 0, nullptr, lpszUri);
		}
		// 一律拦截新窗口；默认在当前窗口打开，事件处理者置 Cancel 则不导航
		BOOL bHandled = TRUE;
		pArgs->put_Handled(bHandled);
		if (bCancel != VARIANT_TRUE && m_pWebView2 != nullptr && lpszUri != nullptr)
			m_pWebView2->Navigate(lpszUri);

		if (lpszUri != nullptr) ::CoTaskMemFree(lpszUri);
	}

	void CWebBrowserUI::OnWebView2HistoryChanged(ICoreWebView2* pWebView)
	{
		if (m_pWebBrowserEventHandler == nullptr || pWebView == nullptr) return;
		BOOL bCanBack = FALSE, bCanForward = FALSE;
		pWebView->get_CanGoBack(&bCanBack);
		pWebView->get_CanGoForward(&bCanForward);
		// 1=CSC_NAVIGATEBACK 2=CSC_NAVIGATEFORWARD（与 IE DWebBrowserEvents2 对齐）
		m_pWebBrowserEventHandler->CommandStateChange(this, 1, bCanBack ? VARIANT_TRUE : VARIANT_FALSE);
		m_pWebBrowserEventHandler->CommandStateChange(this, 2, bCanForward ? VARIANT_TRUE : VARIANT_FALSE);
	}

	void CWebBrowserUI::OnWebView2WebMessageReceived(ICoreWebView2* pWebView, ICoreWebView2WebMessageReceivedEventArgs* pArgs)
	{
		if (m_pWebBrowserEventHandler == nullptr || pArgs == nullptr) return;
		LPWSTR lpszMessage = nullptr;
		if (SUCCEEDED(pArgs->get_WebMessageAsJson(&lpszMessage)) && lpszMessage != nullptr) {
			m_pWebBrowserEventHandler->WebMessageReceived(this, lpszMessage);
			::CoTaskMemFree(lpszMessage);
		}
	}
#endif // DUILIB_USE_WEBVIEW2
}