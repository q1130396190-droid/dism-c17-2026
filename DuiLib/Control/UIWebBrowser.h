#ifndef __UIWEBBROWSER_H__
#define __UIWEBBROWSER_H__

#pragma once

#include <MsHTML.h>
#include "Utils/WebBrowserEventHandler.h"
#include <ExDisp.h>

// ============================================================================
// WebView2 集成（Win10 及以上系统优先使用，低版本系统自动回退 WebBrowser/IE）
// ----------------------------------------------------------------------------
// 编译开关 DUILIB_USE_WEBVIEW2 自动判定：
//   - 编译器为 VS2015 及以上（_MSC_VER >= 1900，WebView2 头文件最低要求）
//   - 能找到 WebView2 SDK 头文件（仓库已将 SDK 头文件复制到 DuiLib/ 目录，
//     该目录位于默认 include 路径中，无需额外配置；外部工程也可自行提供更新版本）
// 运行时判定（CWebBrowserUI::IsWebView2Supported）：
//   - 系统为 Win10 及以上（Utils/VersionHelpers.h）
//   - 已安装 WebView2 Runtime（通过动态加载 WebView2Loader.dll 检测）
//   - 窗口非分层（WebView2 子窗口不支持在分层窗口上渲染）
// 不满足时 CWebBrowserUI 走原有 ActiveX WebBrowser（IE）路径，老系统完全兼容。
// ============================================================================
// [WV2-MACRO] 只接受“未定义”或“定义为 1”两种情况：
//   - 未定义：按工具集与头文件可用性自动探测（历史行为）；
//   - 定义为 0：显式关闭（CMake compat 轨道），此时不得被下面的探测重新打开。
#if defined(DUILIB_USE_WEBVIEW2) && (DUILIB_USE_WEBVIEW2 == 0)
#  undef DUILIB_USE_WEBVIEW2
#elif !defined(DUILIB_USE_WEBVIEW2)
#  if defined(_MSC_VER) && (_MSC_VER >= 1900)
#    if defined(__has_include)
#      if __has_include("../WebView2.h") && __has_include("../WebView2EnvironmentOptions.h")
#        define DUILIB_USE_WEBVIEW2 1
#      endif
#    endif
#  endif
#endif

#if defined(DUILIB_USE_WEBVIEW2) && DUILIB_USE_WEBVIEW2
// WebView2 SDK 头文件在现代 Windows SDK 下要求 NTDDI_VERSION >= Vista（wrl/def.h 检查），
// 老工程 StdAfx.h 只定义了 _WIN32_WINNT，这里补充定义（WebView2 本身要求 Win10+）
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000000	// NTDDI_WIN10
#endif
#include "WebView2.h"
#include "WebView2EnvironmentOptions.h"
#include "Utils/WebView2Helper.h"
#endif

namespace DuiLib
{
#if defined(DUILIB_USE_WEBVIEW2) && DUILIB_USE_WEBVIEW2
	class CWebBrowserUI;

	// [WV2-LIFETIME] Non-owning lifetime token shared between the control and every
	// WebView2 event sink.
	//
	// Why: the sinks used to hold a raw CWebBrowserUI* and the control never unbound
	// them. When WebView2 delivered a queued event after the control was destroyed,
	// Invoke() dereferenced freed memory. The token lets a sink ask "is the owner
	// still alive?" and get nullptr instead of a dangling pointer, without keeping
	// the control alive (that would leak the control instead).
	//
	// Lifetime: owned by the control (one ref) plus one ref per live sink. The
	// control calls Invalidate() during teardown; the token object itself is
	// released only after the last sink drops its ref, so no lock is needed.
	class CWebView2Lifetime
	{
	public:
		explicit CWebView2Lifetime(CWebBrowserUI* pOwner)
			: m_pvOwner(pOwner), m_nRef(1) {}
		void AddRef() { ::InterlockedIncrement(&m_nRef); }
		void Release() { if (::InterlockedDecrement(&m_nRef) == 0) delete this; }
		// Called once, on the UI thread, before the control's WebView2 state is torn down.
		// Stored as PVOID so the Interlocked pointer APIs accept it directly: holding it
		// as CWebBrowserUI* volatile forces a cast that drops const in the const
		// accessor (C2440) and buys nothing, since the value is only ever compared.
		void Invalidate() { ::InterlockedExchangePointer(&m_pvOwner, nullptr); }
		// Returns nullptr once invalidated. Safe to call from an event callback.
		CWebBrowserUI* Get() const
		{
			// const_cast: InterlockedCompareExchangePointer's first parameter is
			// "volatile PVOID*", which a const member function cannot produce for a
			// member field. The exchange itself is a read (comparand 0 / new 0), so
			// dropping const here does not let any caller mutate the member.
			PVOID volatile* pp = const_cast<PVOID volatile*>(&m_pvOwner);
			return static_cast<CWebBrowserUI*>(
				::InterlockedCompareExchangePointer(pp, nullptr, nullptr));
		}
	private:
		PVOID volatile m_pvOwner;
		LONG m_nRef;
	};

	// [WV2-PAYLOAD] Free functions, not members: CWebBrowserUI is exported as a whole
	// class, so a private member becomes an exported symbol. Declared here so the
	// friend declarations inside the class name these exact functions.
	void Wv2DrainQueuedMessages(CWebBrowserUI* pThis);
	void Wv2UnbindEvents(CWebBrowserUI* pThis);
#endif

	class UILIB_API CWebBrowserUI
		: public CActiveXUI
		, public IDocHostUIHandler
		, public IServiceProvider
		, public IOleCommandTarget
		, public IDispatch
		, public ITranslateAccelerator
		, public IInternetSecurityManager 
	{
		DECLARE_DUICONTROL(CWebBrowserUI)
	public:
		/// 构造函数
		CWebBrowserUI();
		virtual ~CWebBrowserUI();

		void SetHomePage(LPCTSTR lpszUrl);
		LPCTSTR GetHomePage();

		void SetAutoNavigation(bool bAuto = TRUE);
		bool IsAutoNavigation();

		void SetWebBrowserEventHandler(CWebBrowserEventHandler* pEventHandler);
		void Navigate2(LPCTSTR lpszUrl);
		void Refresh();
		void Refresh2(int Level);
		void GoBack();
		void GoForward();
		void NavigateHomePage();
		void NavigateUrl(LPCTSTR lpszUrl);
		virtual bool DoCreateControl();
		IWebBrowser2* GetWebBrowser2(void);
		IDispatch*		   GetHtmlWindow();
		static DISPID FindId(IDispatch *pObj, LPOLESTR pName);
		static HRESULT InvokeMethod(IDispatch *pObj, LPOLESTR pMehtod, VARIANT *pVarResult, VARIANT *ps, int cArgs);
		static HRESULT GetProperty(IDispatch *pObj, LPOLESTR pName, VARIANT *pValue);
		static HRESULT SetProperty(IDispatch *pObj, LPOLESTR pName, VARIANT *pValue);

		// ==================== WebView2 集成接口 ====================
#if defined(DUILIB_USE_WEBVIEW2) && DUILIB_USE_WEBVIEW2
		// 当前是否运行在 WebView2 模式（Win10+ 且 WebView2 Runtime 可用）
		bool IsWebView2Mode() const;
		// 强制开关 WebView2（默认自动判断）：SetWebView2Enabled(false) 强制使用老 WebBrowser
		void SetWebView2Enabled(bool bEnable);
		// 停止加载
		void Stop();
		// 向网页发送消息（网页端通过 window.chrome.webview.addEventListener("message", ...) 接收）
		void PostWebMessage(LPCTSTR lpszMessage);
		// 当前系统是否支持 WebView2（Win10+ 且 Runtime 已装 且窗口非分层）
		bool IsWebView2Supported();
		// 获取底层 ICoreWebView2 指针（仅 WebView2 模式有效，否则返回 nullptr）
		ICoreWebView2* GetWebView2() const;
		// [WV2-DIAG] Is the underlying controller actually visible? Navigate() fails
		// with ERROR_INVALID_STATE on a hidden controller, so a probe needs to tell
		// "sink broken" apart from "controller hidden". Returns the controller's own
		// get_IsVisible() value, or false when there is no controller.
		bool IsWebView2ControllerVisible() const;
		// WebView2 异步创建回调消息（供内部事件 sink 使用）
		enum { WEBVIEW2_MSG_FAILED = 1, WEBVIEW2_MSG_CREATE_CONTROLLER = 2, WEBVIEW2_MSG_BIND = 3 };
		static UINT GetWebView2NotifyMessage();
		// 布局/可见性/消息分发（WebView2 子窗口与 ActiveX 宿主窗口不同，需要单独处理）
		virtual void SetPos(RECT rc, bool bNeedInvalidate = true);
		virtual void Move(SIZE szOffset, bool bNeedInvalidate = true);
		virtual void SetVisible(bool bVisible = true);
		virtual void SetInternVisible(bool bVisible = true);
		virtual LRESULT MessageHandler(UINT uMsg, WPARAM wParam, LPARAM lParam, bool& bHandled);
		// ==================== WebView2 模式内部实现 ====================
		// 以下方法供 WebView2 事件 sink（定义于 UIWebBrowser.cpp）回调，故置于 public
		bool InitWebView2();			// 创建 WebView2 环境（异步，创建完成后切回 UI 线程）
		void ReleaseWebView2();			// 释放 WebView2 资源
		void UpdateWebView2Bounds();	// 同步 WebView2 子窗口位置
		void OnWebView2Notify(WPARAM wParam, LPARAM lParam);	// 处理异步回调消息
		void OnWebView2Bind();			// 环境+控制器就绪后：注册事件、设置边界、导航
		// WebView2 事件 -> CWebBrowserEventHandler 翻译
		void OnWebView2NavigationStarting(ICoreWebView2* pWebView, ICoreWebView2NavigationStartingEventArgs* pArgs);
		void OnWebView2NavigationCompleted(ICoreWebView2* pWebView, ICoreWebView2NavigationCompletedEventArgs* pArgs);
		void OnWebView2DocumentTitleChanged(ICoreWebView2* pWebView);
		void OnWebView2NewWindowRequested(ICoreWebView2* pWebView, ICoreWebView2NewWindowRequestedEventArgs* pArgs);
		void OnWebView2HistoryChanged(ICoreWebView2* pWebView);
		void OnWebView2WebMessageReceived(ICoreWebView2* pWebView, ICoreWebView2WebMessageReceivedEventArgs* pArgs);
		// [WV2-GENERATION] Monotonic per-initialisation counter. A sink captures the
		// value in force when it was registered; a callback whose value no longer
		// matches belongs to a previous create/release cycle and must be ignored.
		LONGLONG GetWebView2Generation() const { return m_nWebView2Generation; }
#endif

	protected:
		IWebBrowser2*			m_pWebBrowser2; //浏览器指针
		IHTMLWindow2*		_pHtmlWnd2;
		LONG m_dwRef;
		DWORD m_dwCookie;
		virtual void ReleaseControl();
		HRESULT RegisterEventHandler(BOOL inAdvise);
		virtual void SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue);
		CDuiString m_sHomePage;	// 默认页面
		bool m_bAutoNavi;	// 是否启动时打开默认页面
		CWebBrowserEventHandler* m_pWebBrowserEventHandler;	//浏览器事件处理

		// DWebBrowserEvents2
		void BeforeNavigate2( IDispatch *pDisp,VARIANT *&url,VARIANT *&Flags,VARIANT *&TargetFrameName,VARIANT *&PostData,VARIANT *&Headers,VARIANT_BOOL *&Cancel );
		void NavigateError(IDispatch *pDisp,VARIANT * &url,VARIANT *&TargetFrameName,VARIANT *&StatusCode,VARIANT_BOOL *&Cancel);
		void NavigateComplete2(IDispatch *pDisp,VARIANT *&url);
		void ProgressChange(LONG nProgress, LONG nProgressMax);
		void NewWindow3(IDispatch **pDisp, VARIANT_BOOL *&Cancel, DWORD dwFlags, BSTR bstrUrlContext, BSTR bstrUrl);
		void CommandStateChange(long Command,VARIANT_BOOL Enable);
		void TitleChange(BSTR bstrTitle);
		void DocumentComplete(IDispatch *pDisp,VARIANT *&url);

		// 老系统路径：创建 ActiveX WebBrowser（原逻辑）；两种模式都需声明
		bool DoCreateActiveXBrowser();

	public:
		virtual LPCTSTR GetClass() const;
		virtual LPVOID GetInterface( LPCTSTR pstrName );

		// IUnknown
		STDMETHOD_(ULONG,AddRef)();
		STDMETHOD_(ULONG,Release)();
		STDMETHOD(QueryInterface)(REFIID riid, LPVOID *ppvObject);

		// IDispatch
		virtual HRESULT STDMETHODCALLTYPE GetTypeInfoCount( __RPC__out UINT *pctinfo );
		virtual HRESULT STDMETHODCALLTYPE GetTypeInfo( UINT iTInfo, LCID lcid, __RPC__deref_out_opt ITypeInfo **ppTInfo );
		virtual HRESULT STDMETHODCALLTYPE GetIDsOfNames( __RPC__in REFIID riid, __RPC__in_ecount_full(cNames ) LPOLESTR *rgszNames, UINT cNames, LCID lcid, __RPC__out_ecount_full(cNames) DISPID *rgDispId);
		virtual HRESULT STDMETHODCALLTYPE Invoke( DISPID dispIdMember, REFIID riid, LCID lcid, WORD wFlags, DISPPARAMS *pDispParams, VARIANT *pVarResult, EXCEPINFO *pExcepInfo, UINT *puArgErr );

		// IDocHostUIHandler
		STDMETHOD(ShowContextMenu)(DWORD dwID, POINT* pptPosition, IUnknown* pCommandTarget, IDispatch* pDispatchObjectHit);
		STDMETHOD(GetHostInfo)(DOCHOSTUIINFO* pInfo);
		STDMETHOD(ShowUI)(DWORD dwID, IOleInPlaceActiveObject* pActiveObject, IOleCommandTarget* pCommandTarget, IOleInPlaceFrame* pFrame, IOleInPlaceUIWindow* pDoc);
		STDMETHOD(HideUI)();
		STDMETHOD(UpdateUI)();
		STDMETHOD(EnableModeless)(BOOL fEnable);
		STDMETHOD(OnDocWindowActivate)(BOOL fActivate);
		STDMETHOD(OnFrameWindowActivate)(BOOL fActivate);
		STDMETHOD(ResizeBorder)(LPCRECT prcBorder, IOleInPlaceUIWindow* pUIWindow, BOOL fFrameWindow);
		STDMETHOD(TranslateAccelerator)(LPMSG lpMsg, const GUID* pguidCmdGroup, DWORD nCmdID);	//浏览器消息过滤
		STDMETHOD(GetOptionKeyPath)(LPOLESTR* pchKey, DWORD dwReserved);
		STDMETHOD(GetDropTarget)(IDropTarget* pDropTarget, IDropTarget** ppDropTarget);
		STDMETHOD(GetExternal)(IDispatch** ppDispatch);
		STDMETHOD(TranslateUrl)(DWORD dwTranslate, OLECHAR* pchURLIn, OLECHAR** ppchURLOut);
		STDMETHOD(FilterDataObject)(IDataObject* pDO, IDataObject** ppDORet);

		// IServiceProvider
		STDMETHOD(QueryService)(REFGUID guidService, REFIID riid, void** ppvObject);

		// IOleCommandTarget
		virtual HRESULT STDMETHODCALLTYPE QueryStatus( __RPC__in_opt const GUID *pguidCmdGroup, ULONG cCmds, __RPC__inout_ecount_full(cCmds ) OLECMD prgCmds[ ], __RPC__inout_opt OLECMDTEXT *pCmdText);
		virtual HRESULT STDMETHODCALLTYPE Exec( __RPC__in_opt const GUID *pguidCmdGroup, DWORD nCmdID, DWORD nCmdexecopt, __RPC__in_opt VARIANT *pvaIn, __RPC__inout_opt VARIANT *pvaOut );

		// IDownloadManager
		STDMETHOD(Download)( 
			/* [in] */ IMoniker *pmk,
			/* [in] */ IBindCtx *pbc,
			/* [in] */ DWORD dwBindVerb,
			/* [in] */ LONG grfBINDF,
			/* [in] */ BINDINFO *pBindInfo,
			/* [in] */ LPCOLESTR pszHeaders,
			/* [in] */ LPCOLESTR pszRedir,
			/* [in] */ UINT uiCP);

		virtual HRESULT STDMETHODCALLTYPE SetSecuritySite( 
            /* [unique][in] */ __RPC__in_opt IInternetSecurityMgrSite *pSite){return S_OK;}
        
        virtual HRESULT STDMETHODCALLTYPE GetSecuritySite( 
            /* [out] */ __RPC__deref_out_opt IInternetSecurityMgrSite **ppSite){return S_OK;}
        
        virtual HRESULT STDMETHODCALLTYPE MapUrlToZone( 
            /* [in] */ __RPC__in LPCWSTR pwszUrl,
            /* [out] */ __RPC__out DWORD *pdwZone,
			/* [in] */ DWORD dwFlags) {return S_OK;}
        
        virtual HRESULT STDMETHODCALLTYPE GetSecurityId( 
            /* [in] */ __RPC__in LPCWSTR pwszUrl,
            /* [size_is][out] */ __RPC__out_ecount_full(*pcbSecurityId) BYTE *pbSecurityId,
            /* [out][in] */ __RPC__inout DWORD *pcbSecurityId,
            /* [in] */ DWORD_PTR dwReserved) {return S_OK;}
        
        virtual HRESULT STDMETHODCALLTYPE ProcessUrlAction( 
            /* [in] */ __RPC__in LPCWSTR pwszUrl,
            /* [in] */ DWORD dwAction,
            /* [size_is][out] */ __RPC__out_ecount_full(cbPolicy) BYTE *pPolicy,
            /* [in] */ DWORD cbPolicy,
            /* [unique][in] */ __RPC__in_opt BYTE *pContext,
            /* [in] */ DWORD cbContext,
            /* [in] */ DWORD dwFlags,
			/* [in] */ DWORD dwReserved)
		{
			return S_OK;
		}
        
        virtual HRESULT STDMETHODCALLTYPE QueryCustomPolicy( 
            /* [in] */ __RPC__in LPCWSTR pwszUrl,
            /* [in] */ __RPC__in REFGUID guidKey,
            /* [size_is][size_is][out] */ __RPC__deref_out_ecount_full_opt(*pcbPolicy) BYTE **ppPolicy,
            /* [out] */ __RPC__out DWORD *pcbPolicy,
            /* [in] */ __RPC__in BYTE *pContext,
            /* [in] */ DWORD cbContext,
            /* [in] */ DWORD dwReserved) {return S_OK;}
        
        virtual HRESULT STDMETHODCALLTYPE SetZoneMapping( 
            /* [in] */ DWORD dwZone,
            /* [in] */ __RPC__in LPCWSTR lpszPattern,
            /* [in] */ DWORD dwFlags) {return S_OK;}
        
        virtual HRESULT STDMETHODCALLTYPE GetZoneMappings( 
            /* [in] */ DWORD dwZone,
            /* [out] */ __RPC__deref_out_opt IEnumString **ppenumString,
            /* [in] */ DWORD dwFlags) {return S_OK;}
		// ITranslateAccelerator
		// Duilib消息分发给WebBrowser
		virtual LRESULT TranslateAccelerator( MSG *pMsg );

	private:
#if defined(DUILIB_USE_WEBVIEW2) && DUILIB_USE_WEBVIEW2
		bool m_bUseWebView2;						// 当前是否 WebView2 模式
		ICoreWebView2Environment* m_pWebView2Env;		// WebView2 环境
		ICoreWebView2Controller* m_pWebView2Controller;	// WebView2 控制器
		ICoreWebView2* m_pWebView2;					// WebView2 内核
		UINT m_uWebView2NotifyMsg;					// 本控件实例专用的异步回调消息（避免同窗口多控件串扰）
		// [WV2-GENERATION] Bumped on every create and every release. Async payloads and
		// event sinks carry the value they were created with; stale ones are dropped.
		volatile LONGLONG m_nWebView2Generation;
		// [WV2-REENTRY] True between the DoCreateControl entry and the controller
		// callback. A second DoCreateControl inside that window creates a second
		// controller bound to the same HWND, after which every Navigate() returns
		// ERROR_INVALID_STATE (observed: two [WV2-CREATECTRL] lines for one control).
		bool m_bWebView2Creating;
		// [WV2-LIFETIME] Shared with sinks so a late callback sees nullptr, not a
		// destroyed control.
		CWebView2Lifetime* m_pWebView2Lifetime;
		// [WV2-TOKENS] Every registered event is remembered so teardown can unbind it.
		// A single reused local token (the previous code) made removal impossible.
		EventRegistrationToken m_tokNavStarting;
		EventRegistrationToken m_tokNavCompleted;
		EventRegistrationToken m_tokTitleChanged;
		EventRegistrationToken m_tokNewWindow;
		EventRegistrationToken m_tokHistoryChanged;
		EventRegistrationToken m_tokWebMessage;
		unsigned m_uWv2EventMask;					// bit set == that token was registered
		// [WV2-PAYLOAD] The drain/unbind helpers are deliberately NOT members.
		//
		// CWebBrowserUI is exported as a whole class (class UILIB_API), so every member
		// -- including private ones -- lands in the DLL export table. Adding three
		// private helpers raised the non-public export count 147 -> 150 and tripped the
		// export-surface ratchet, which is only allowed to go down. They are free
		// functions in the .cpp instead: same behaviour, no ABI growth.
		//
		// Friends are used rather than accessors because a friend declaration is not a
		// member and therefore emits no symbol: it grants access without widening the
		// exported surface we just measured.
		friend void Wv2DrainQueuedMessages(CWebBrowserUI* pThis);
		friend void Wv2UnbindEvents(CWebBrowserUI* pThis);
#endif
		CDuiString m_sPendingUrl;					// 控件创建完成前挂起的待导航地址（IE/WebView2 通用）
		int m_nWebView2Mode;						// -1 自动 / 0 强制 IE / 1 强制 WebView2
	};
} // namespace DuiLib
#endif // __UIWEBBROWSER_H__
