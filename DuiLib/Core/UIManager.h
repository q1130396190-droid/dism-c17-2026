#ifndef __UIMANAGER_H__
#define __UIMANAGER_H__

// [T5.43] Suppress C4091: C++03-style 'typedef enum X' triggers warning in MSVC /W4.
// C++11+ prefers plain 'enum X', but retained for ABI compatibility with older callers.
// See docs/t541-w4-full-rescan.json.
#pragma warning(disable: 4091)
#pragma once
namespace DuiLib {
	/////////////////////////////////////////////////////////////////////////////////////
	//

	class CControlUI;
	class CRichEditUI;
	class CIDropTarget;

	/////////////////////////////////////////////////////////////////////////////////////
	//
	enum UILIB_RESTYPE
	{
		UILIB_FILE=1,		// 来自磁盘文件
		UILIB_ZIP,			// 来自磁盘zip压缩包
		UILIB_RESOURCE,		// 来自资源
		UILIB_ZIPRESOURCE,	// 来自资源的zip压缩包
	};
	/////////////////////////////////////////////////////////////////////////////////////
	//

	enum EVENTTYPE_UI
	{
		UIEVENT__FIRST = 1,
		UIEVENT__KEYBEGIN,
		UIEVENT_KEYDOWN,
		UIEVENT_KEYUP,
		UIEVENT_CHAR,
		UIEVENT_SYSKEY,
		UIEVENT__KEYEND,
		UIEVENT__MOUSEBEGIN,
		UIEVENT_MOUSEMOVE,
		UIEVENT_MOUSELEAVE,
		UIEVENT_MOUSEENTER,
		UIEVENT_MOUSEHOVER,
		UIEVENT_BUTTONDOWN,
		UIEVENT_BUTTONUP,
		UIEVENT_RBUTTONDOWN,
		UIEVENT_RBUTTONUP,
		UIEVENT_MBUTTONDOWN,
		UIEVENT_MBUTTONUP,
		UIEVENT_DBLCLICK,
		UIEVENT_CONTEXTMENU,
		UIEVENT_SCROLLWHEEL,
		UIEVENT__MOUSEEND,
		UIEVENT_KILLFOCUS,
		UIEVENT_SETFOCUS,
		UIEVENT_WINDOWSIZE,
		UIEVENT_SETCURSOR,
		UIEVENT_TIMER,
		UIEVENT__LAST,
	};


	/////////////////////////////////////////////////////////////////////////////////////
	//
	// 内部保留的消息
	enum MSGTYPE_UI
	{
		UIMSG_TRAYICON = WM_USER + 1,// 托盘消息
		UIMSG_SET_DPI,				 // DPI
		WM_MENUCLICK,				 // 菜单消息
		UIMSG_USER = WM_USER + 100,	 // 程序自定义消息
	};

	/////////////////////////////////////////////////////////////////////////////////////
	//

	// Flags for CControlUI::GetControlFlags()
inline constexpr UINT UIFLAG_TABSTOP = 0x00000001;
inline constexpr UINT UIFLAG_SETCURSOR = 0x00000002;
inline constexpr UINT UIFLAG_WANTRETURN = 0x00000004;

	// Flags for FindControl()
inline constexpr UINT UIFIND_ALL = 0x00000000;
inline constexpr UINT UIFIND_VISIBLE = 0x00000001;
inline constexpr UINT UIFIND_ENABLED = 0x00000002;
inline constexpr UINT UIFIND_HITTEST = 0x00000004;
inline constexpr UINT UIFIND_UPDATETEST = 0x00000008;
inline constexpr UINT UIFIND_TOP_FIRST = 0x00000010;
inline constexpr UINT UIFIND_ME_FIRST = 0x80000000;

	// Flags used for controlling the paint
inline constexpr UINT UISTATE_FOCUSED = 0x00000001;
inline constexpr UINT UISTATE_SELECTED = 0x00000002;
inline constexpr UINT UISTATE_DISABLED = 0x00000004;
inline constexpr UINT UISTATE_HOT = 0x00000008;
inline constexpr UINT UISTATE_PUSHED = 0x00000010;
inline constexpr UINT UISTATE_READONLY = 0x00000020;
inline constexpr UINT UISTATE_CAPTURED = 0x00000040;

	/////////////////////////////////////////////////////////////////////////////////////
	//

	struct UILIB_API TFontInfo
	{
		HFONT hFont;
		CDuiString sFontName;
		int iSize;
		bool bBold;
		bool bUnderline;
		bool bItalic;
		bool bStrikeout;
		TEXTMETRIC tm;
	};
	struct UILIB_API TImageInfo
	{
		TImageInfo();
		Gdiplus::Image* pImage;
		HBITMAP hBitmap;
		LPBYTE pBits;
		LPBYTE pSrcBits;
		int nX;
		int nY;
		bool bAlpha;
		bool bUseHSL;
		CDuiString sResType;
		DWORD dwMask;

	};
	struct UILIB_API TDrawInfo
	{
		TDrawInfo();
		void Parse(LPCTSTR pStrImage, LPCTSTR pStrModify, CPaintManagerUI *pManager);
		void Clear();

		CDuiString sDrawString;
		CDuiString sDrawModify;
		CDuiString sImageName;
		CDuiString sResType;
		RECT rcDest;
		RECT rcSource;
		RECT rcCorner;
		DWORD dwMask;
		UINT uFade;
		UINT uRotate;
		bool bHole;
		bool bTiledX;
		bool bTiledY;
		bool bHSL;
		bool bGdiplus;

		CDuiSize szImage;
		RECT rcPadding;
		CDuiString sAlign;
	};
	struct UILIB_API TPercentInfo
	{
		double left;
		double top;
		double right;
		double bottom;
	};
	struct UILIB_API TResInfo
	{
		DWORD m_dwDefaultDisabledColor;
		DWORD m_dwDefaultFontColor;
		DWORD m_dwDefaultLinkFontColor;
		DWORD m_dwDefaultLinkHoverFontColor;
		DWORD m_dwDefaultSelectedBkColor;
		TFontInfo m_DefaultFontInfo;
		CStdStringPtrMap m_CustomFonts;
		CStdStringPtrMap m_ImageHash;
		CStdStringPtrMap m_AttrHash;
		CStdStringPtrMap m_StyleHash;
		CStdStringPtrMap m_DrawInfoHash;
	};
	// Structure for notifications from the system
	// to the control implementation.
	struct UILIB_API TEventUI
	{
		int Type;
		CControlUI* pSender;
		DWORD dwTimestamp;
		POINT ptMouse;
		TCHAR chKey;
		WORD wKeyState;
		WPARAM wParam;
		LPARAM lParam;
	};
	// Drag&Drop control
	const TCHAR* const CF_MOVECONTROL = _T("CF_MOVECONTROL");

	struct UILIB_API TCFMoveUI
	{
		CControlUI* pControl;
	};
	// Listener interface
	class INotifyUI
	{
	public:
		virtual void Notify(TNotifyUI& msg) = 0;
	};

	// MessageFilter interface
	class IMessageFilterUI
	{
	public:
		virtual LRESULT MessageHandler(UINT uMsg, WPARAM wParam, LPARAM lParam, bool& bHandled) = 0;
	};

	class ITranslateAccelerator
	{
	public:
		virtual LRESULT TranslateAccelerator(MSG *pMsg) = 0;
	};

	class IDragDropUI
	{
	public:
		virtual bool OnDragDrop(CControlUI* pControl) { return false; }
	};
	/////////////////////////////////////////////////////////////////////////////////////
	//
	using LPCREATECONTROL = CControlUI* (*)(LPCTSTR pstrType);

	class UILIB_API CPaintManagerUI : public CIDropTarget
	{
	public:
		CPaintManagerUI();
		~CPaintManagerUI();
		// [SAFE-1] Owns HDC/HBITMAP handles plus three child containers and releases them in the destructor (ReapObjects). Copying would release the same GDI objects twice. CComboWnd embeds one as a member and is itself only ever heap-allocated and never copied, so this compiles everywhere it is used.
		CPaintManagerUI(const CPaintManagerUI&) = delete;
		CPaintManagerUI& operator=(const CPaintManagerUI&) = delete;

	public:
		void Init(HWND hWnd, LPCTSTR pstrName = nullptr);
		bool IsUpdateNeeded() const;
		void NeedUpdate();
		void Invalidate();
		void Invalidate(RECT& rcItem);

		LPCTSTR GetName() const;
		HDC GetPaintDC() const;
		HWND GetPaintWindow() const;
		HWND GetTooltipWindow() const;
		int GetHoverTime() const;
		void SetHoverTime(int iTime);

		POINT GetMousePos() const;
		SIZE GetClientSize() const;
		SIZE GetInitSize();
		void SetInitSize(int cx, int cy);
		RECT GetSizeBox();
		void SetSizeBox(RECT& rcSizeBox);
		RECT GetCaptionRect();
		void SetCaptionRect(RECT& rcCaption);
		SIZE GetRoundCorner();
		void SetRoundCorner(int cx, int cy);
		SIZE GetMinInfo();
		void SetMinInfo(int cx, int cy);
		SIZE GetMaxInfo();
		void SetMaxInfo(int cx, int cy);
		bool IsShowUpdateRect();
		void SetShowUpdateRect(bool show);
		bool IsNoActivate();
		void SetNoActivate(bool bNoActivate);

		BYTE GetOpacity() const;
		void SetOpacity(BYTE nOpacity);

		bool IsLayered();
		void SetLayered(bool bLayered);
		RECT& GetLayeredInset();
		void SetLayeredInset(RECT& rcLayeredInset);
		BYTE GetLayeredOpacity();
		void SetLayeredOpacity(BYTE nOpacity);
		LPCTSTR GetLayeredImage();
		void SetLayeredImage(LPCTSTR pstrImage);

		CShadowUI* GetShadow();

		void SetUseGdiplusText(bool bUse);
		bool IsUseGdiplusText() const;
		void SetGdiplusTextRenderingHint(int trh);
		int GetGdiplusTextRenderingHint() const;

		static HINSTANCE GetInstance();
		static CDuiString GetInstancePath();
		static CDuiString GetCurrentPath();
		static HINSTANCE GetResourceDll();
		static const CDuiString& GetResourcePath();
		static const CDuiString& GetResourceZip();
		static const CDuiString& GetResourceZipPwd();
		static bool IsCachedResourceZip();
		static HANDLE GetResourceZipHandle();
		static void SetInstance(HINSTANCE hInst);
		static void SetCurrentPath(LPCTSTR pStrPath);
		static void SetResourceDll(HINSTANCE hInst);
		static void SetResourcePath(LPCTSTR pStrPath);
		static void SetResourceZip(LPVOID pVoid, unsigned int len, LPCTSTR password = nullptr);
		static void SetResourceZip(LPCTSTR pstrZip, bool bCachedResourceZip = false, LPCTSTR password = nullptr);
		static void SetResourceType(int nType);
		static int GetResourceType();
		static bool GetHSL(short* H, short* S, short* L);
		static void SetHSL(bool bUseHSL, short H, short S, short L); // H:0~360, S:0~200, L:0~200 
		static void ReloadSkin();
		static CPaintManagerUI* GetPaintManager(LPCTSTR pstrName);
		static CStdPtrArray* GetPaintManagers();
		static bool LoadPlugin(LPCTSTR pstrModuleName);
		static CStdPtrArray* GetPlugins();

		bool IsForceUseSharedRes() const;
		void SetForceUseSharedRes(bool bForce);

		// N1/T5.6: attribute-name runtime validation switch.
		// Debug-only (gated by _DEBUG); a Release build contains no method
		// body and no switch member for it, so the release path is unchanged.
		// Default off, which keeps existing behaviour bit-for-bit.
		void SetAttributeValidationEnabled(bool bEnable);
		bool IsAttributeValidationEnabled() const;
		static void SetAttributeValidationEnabledForAll(bool bEnable);
		// [R44] Process-wide default for managers created later.
		// Why needed: skin XML is parsed inside WindowImplBase::Create, i.e. long
		// before an application can reach its own post-create code. Calling
		// SetAttributeValidationEnabledForAll() there is useless -- no manager
		// exists yet. This sets the value every new CPaintManagerUI starts with,
		// so a command-line switch can turn validation on before parsing begins.
		static void SetAttributeValidationDefault(bool bEnable);
		static bool GetAttributeValidationDefault();
		static bool IsAttributeValidationEnabledForAny();
		// Returns true and records pstrName on first sight; false on repeat.
		static bool ReportAttributeValidationUnknown(LPCTSTR pstrName);
		static int  GetAttributeValidationUnknownCount();
		// [R44] Enumerate the recorded names. A count alone is not actionable:
		// "/AttrCheck said 1" does not tell anyone which attribute to fix.
		// Returns nullptr past the end. Names are owned by the library.
		static LPCTSTR GetAttributeValidationUnknownName(int iIndex);
		static void ResetAttributeValidationWarnedCount();
		// 注意：只支持简单类型指针，因为只释放内存，不会调用类对象的析构函数
		void DeletePtr(void* ptr);

		DWORD GetDefaultDisabledColor() const;
		void SetDefaultDisabledColor(DWORD dwColor, bool bShared = false);
		DWORD GetDefaultFontColor() const;
		void SetDefaultFontColor(DWORD dwColor, bool bShared = false);
		DWORD GetDefaultLinkFontColor() const;
		void SetDefaultLinkFontColor(DWORD dwColor, bool bShared = false);
		DWORD GetDefaultLinkHoverFontColor() const;
		void SetDefaultLinkHoverFontColor(DWORD dwColor, bool bShared = false);
		DWORD GetDefaultSelectedBkColor() const;
		void SetDefaultSelectedBkColor(DWORD dwColor, bool bShared = false);
		TFontInfo* GetDefaultFontInfo();
		void SetDefaultFont(LPCTSTR pStrFontName, int nSize, bool bBold, bool bUnderline, bool bItalic, bool bStrikeout, bool bShared = false);
		DWORD GetCustomFontCount(bool bShared = false) const;
		void AddFontArray(LPCTSTR pstrPath);
		HFONT AddFont(int id, LPCTSTR pStrFontName, int nSize, bool bBold, bool bUnderline, bool bItalic, bool bStrikeout, bool bShared = false);
		HFONT GetFont(int id);
		HFONT GetFont(LPCTSTR pStrFontName, int nSize, bool bBold, bool bUnderline, bool bItalic, bool bStrikeout);
		int GetFontIndex(HFONT hFont, bool bShared = false);
		int GetFontIndex(LPCTSTR pStrFontName, int nSize, bool bBold, bool bUnderline, bool bItalic, bool bStrikeout, bool bShared = false);
		void RemoveFont(HFONT hFont, bool bShared = false);
		void RemoveFont(int id, bool bShared = false);
		void RemoveAllFonts(bool bShared = false);
		TFontInfo* GetFontInfo(int id);
		TFontInfo* GetFontInfo(HFONT hFont);

		const TImageInfo* GetImage(LPCTSTR bitmap);
		const TImageInfo* GetImageEx(LPCTSTR bitmap, LPCTSTR type = nullptr, DWORD mask = 0, bool bUseHSL = false, bool bGdiplus = false, HINSTANCE instance = nullptr);
		const TImageInfo* AddImage(LPCTSTR bitmap, LPCTSTR type = nullptr, DWORD mask = 0, bool bUseHSL = false, bool bGdiplus = false, bool bShared = false, HINSTANCE instance = nullptr);
		const TImageInfo* AddImage(LPCTSTR bitmap, HBITMAP hBitmap, int iWidth, int iHeight, bool bAlpha, bool bShared = false);
		void RemoveImage(LPCTSTR bitmap, bool bShared = false);
		void RemoveAllImages(bool bShared = false);
		static void ReloadSharedImages();
		void ReloadImages();

		const TDrawInfo* GetDrawInfo(LPCTSTR pStrImage, LPCTSTR pStrModify);
		void RemoveDrawInfo(LPCTSTR pStrImage, LPCTSTR pStrModify);
		void RemoveAllDrawInfos();

		void AddDefaultAttributeList(LPCTSTR pStrControlName, LPCTSTR pStrControlAttrList, bool bShared = false);
		LPCTSTR GetDefaultAttributeList(LPCTSTR pStrControlName) const;
		bool RemoveDefaultAttributeList(LPCTSTR pStrControlName, bool bShared = false);
		void RemoveAllDefaultAttributeList(bool bShared = false);

		void AddWindowCustomAttribute(LPCTSTR pstrName, LPCTSTR pstrAttr);
		LPCTSTR GetWindowCustomAttribute(LPCTSTR pstrName) const;
		bool RemoveWindowCustomAttribute(LPCTSTR pstrName);
		void RemoveAllWindowCustomAttribute();

		// 样式管理
		void AddStyle(LPCTSTR pName, LPCTSTR pStyle, bool bShared = false);
		LPCTSTR GetStyle(LPCTSTR pName) const;
		BOOL RemoveStyle(LPCTSTR pName, bool bShared = false);
		const CStdStringPtrMap& GetStyles(bool bShared = false) const;
		void RemoveAllStyle(bool bShared = false);

		const TImageInfo* GetImageString(LPCTSTR pStrImage, LPCTSTR pStrModify = nullptr);

		// 初始化拖拽
		bool EnableDragDrop(bool bEnable);
		void SetDragDrop(IDragDropUI* pDragDrop);
		virtual bool OnDrop(FORMATETC* pFmtEtc, STGMEDIUM& medium,DWORD *pdwEffect);

		bool IsValid();
		bool AttachDialog(CControlUI* pControl);
		bool InitControls(CControlUI* pControl, CControlUI* pParent = nullptr);
		void ReapObjects(CControlUI* pControl);

		bool AddOptionGroup(LPCTSTR pStrGroupName, CControlUI* pControl);
		CStdPtrArray* GetOptionGroup(LPCTSTR pStrGroupName);
		void RemoveOptionGroup(LPCTSTR pStrGroupName, CControlUI* pControl);
		void RemoveAllOptionGroups();

		CControlUI* GetFocus() const;
		void SetFocus(CControlUI* pControl);
		void SetFocusNeeded(CControlUI* pControl);

		// Move focus away from a control that is about to become unusable
		// (hidden, disabled, or no longer keyboard-enabled) and hand it to the
		// next visible/enabled tab-stop. Returns true when focus was moved.
		bool RelocateFocusFrom(CControlUI* pControl);

		bool SetNextTabControl(bool bForward = true);

		bool SetTimer(CControlUI* pControl, UINT nTimerID, UINT uElapse);
		bool KillTimer(CControlUI* pControl, UINT nTimerID);
		void KillTimer(CControlUI* pControl);
		void RemoveAllTimers();

		void SetCapture();
		void ReleaseCapture();
		bool IsCaptured();

		bool IsPainting();
		void SetPainting(bool bIsPainting);

		bool AddNotifier(INotifyUI* pControl);
		bool RemoveNotifier(INotifyUI* pControl);   
		void SendNotify(TNotifyUI& Msg, bool bAsync = false);
		void SendNotify(CControlUI* pControl, LPCTSTR pstrMessage, WPARAM wParam = 0, LPARAM lParam = 0, bool bAsync = false);

		// [R-07] The supported way to touch the UI from a worker thread.
		// Runs pfnTask on the UI thread (immediately if already there), returns
		// false when the request could not be queued -- the caller must treat
		// false as "not delivered", never as "maybe later".
		// pContext is owned by the caller: it is passed through untouched and
		// freed after pfnTask returns. Queued tasks that are still pending when
		// the manager is destroyed are dropped, and every dropped task is
		// counted in DroppedUiTasks() so the loss is observable rather than
		// silent.
		typedef void (*UiTaskProc)(void* pContext);
		bool PostToUiThread(UiTaskProc pfnTask, void* pContext);
		LONG DroppedUiTasks() const;

		bool AddPreMessageFilter(IMessageFilterUI* pFilter);
		bool RemovePreMessageFilter(IMessageFilterUI* pFilter);

		bool AddMessageFilter(IMessageFilterUI* pFilter);
		bool RemoveMessageFilter(IMessageFilterUI* pFilter);

		int GetPostPaintCount() const;
		bool IsPostPaint(CControlUI* pControl);
		bool AddPostPaint(CControlUI* pControl);
		bool RemovePostPaint(CControlUI* pControl);
		bool SetPostPaintIndex(CControlUI* pControl, int iIndex);

		int GetNativeWindowCount() const;
		RECT GetNativeWindowRect(HWND hChildWnd);
		bool AddNativeWindow(CControlUI* pControl, HWND hChildWnd);
		bool RemoveNativeWindow(HWND hChildWnd);

		void AddDelayedCleanup(CControlUI* pControl);
		void AddMouseLeaveNeeded(CControlUI* pControl);
		bool RemoveMouseLeaveNeeded(CControlUI* pControl);

		bool AddTranslateAccelerator(ITranslateAccelerator *pTranslateAccelerator);
		bool RemoveTranslateAccelerator(ITranslateAccelerator *pTranslateAccelerator);
		bool TranslateAccelerator(LPMSG pMsg);

		CControlUI* GetRoot() const;
		CControlUI* FindControl(POINT pt) const;
		CControlUI* FindControl(LPCTSTR pstrName) const;
		CControlUI* FindSubControlByPoint(CControlUI* pParent, POINT pt) const;
		CControlUI* FindSubControlByName(CControlUI* pParent, LPCTSTR pstrName) const;
		CControlUI* FindSubControlByClass(CControlUI* pParent, LPCTSTR pstrClass, int iIndex = 0);
		CStdPtrArray* FindSubControlsByClass(CControlUI* pParent, LPCTSTR pstrClass);

		static void MessageLoop();
		static bool TranslateMessage(const LPMSG pMsg);
		static void Term();

		CDPI* GetDPIObj();
		void ResetDPIAssets();
		void RebuildFont(TFontInfo* pFontInfo);
		void SetDPI(int iDPI);
		static void SetAllDPI(int iDPI);

		bool MessageHandler(UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT& lRes);
		bool PreMessageHandler(UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT& lRes);
		void UsedVirtualWnd(bool bUsed);

	private:
		CStdPtrArray* GetFoundControls();
		static CControlUI* CALLBACK __FindControlFromNameHash(CControlUI* pThis, LPVOID pData);
		static CControlUI* CALLBACK __FindControlFromCount(CControlUI* pThis, LPVOID pData);
		static CControlUI* CALLBACK __FindControlFromPoint(CControlUI* pThis, LPVOID pData);
		static CControlUI* CALLBACK __FindControlFromTab(CControlUI* pThis, LPVOID pData);
		static CControlUI* CALLBACK __FindControlFromShortcut(CControlUI* pThis, LPVOID pData);
		static CControlUI* CALLBACK __FindControlFromName(CControlUI* pThis, LPVOID pData);
		static CControlUI* CALLBACK __FindControlFromClass(CControlUI* pThis, LPVOID pData);
		static CControlUI* CALLBACK __FindControlsFromClass(CControlUI* pThis, LPVOID pData);
		static CControlUI* CALLBACK __FindControlsFromUpdate(CControlUI* pThis, LPVOID pData);

		static void AdjustSharedImagesHSL();
		void AdjustImagesHSL();
		void PostAsyncNotify();

	private:
		CDuiString m_sName;
		HWND m_hWndPaint;	//所附加的窗体的句柄
		HDC m_hDcPaint;
		HDC m_hDcOffscreen;
		HDC m_hDcBackground;
		HBITMAP m_hbmpOffscreen;
		BYTE* m_pOffscreenBits;
		HBITMAP m_hbmpBackground;
		COLORREF* m_pBackgroundBits;

		// 提示信息
		HWND m_hwndTooltip;
		TOOLINFO m_ToolTip;
		int m_iHoverTime;
		bool m_bNoActivate;
		bool m_bShowUpdateRect;

		//
		CControlUI* m_pRoot;
		CControlUI* m_pFocus;
		CControlUI* m_pEventHover;
        CControlUI* m_pEventClick;
        CControlUI* m_pEventRClick;
		CControlUI* m_pEventKey;
		CControlUI* m_pLastToolTip;
		//
		POINT m_ptLastMousePos;
		SIZE m_szMinWindow;
		SIZE m_szMaxWindow;
		SIZE m_szInitWindowSize;
		RECT m_rcSizeBox;
		SIZE m_szRoundCorner;
		RECT m_rcCaption;
		UINT m_uTimerID;
		bool m_bFirstLayout;
		bool m_bUpdateNeeded;
		bool m_bFocusNeeded;
		bool m_bOffscreenPaint;
		
		BYTE m_nOpacity;
		bool m_bLayered;
		RECT m_rcLayeredInset;
		bool m_bLayeredChanged;
		RECT m_rcLayeredUpdate;
		TDrawInfo m_diLayered;

		bool m_bMouseTracking;
		bool m_bMouseCapture;
		bool m_bIsPainting;
		bool m_bUsedVirtualWnd;
		bool m_bAsyncNotifyPosted;

		//
		CStdPtrArray m_aNotifiers;
		CStdPtrArray m_aTimers;
		CStdPtrArray m_aTranslateAccelerator;
		CStdPtrArray m_aPreMessageFilters;
		CStdPtrArray m_aMessageFilters;
		CStdPtrArray m_aPostPaintControls;
		CStdPtrArray m_aNativeWindow;
		CStdPtrArray m_aNativeWindowControl;
		// [PERF-12] Per-native-child offscreen surface, reused across paints.
		// NOTE: this ADDS a member to an exported class. Any module that holds a
		// CPaintManagerUI by value (WindowImplBase does) MUST be recompiled
		// together with the DLL, otherwise sizeof differs and it corrupts the
		// stack. This caused a real 0xC0000005 during round 5 (see report).
		struct TNATIVE_SURFACE
		{
			HDC      hMemDC = nullptr;
			HBITMAP  hBitmap = nullptr;
			COLORREF* pBits = nullptr;
			int      nCx = 0;
			int      nCy = 0;
		};
		std::map<HWND, TNATIVE_SURFACE> m_mNativeSurfaces;
		TNATIVE_SURFACE& GetNativeSurface(HWND hChildWnd, int nCx, int nCy);
		void ReleaseNativeSurfaces();

		CStdPtrArray m_aDelayedCleanup;
		CStdPtrArray m_aAsyncNotify;
		CStdPtrArray m_aFoundControls;
		CStdPtrArray m_aFonts;
		CStdPtrArray m_aNeedMouseLeaveNeeded;
		CStdStringPtrMap m_mNameHash;
		CStdStringPtrMap m_mWindowCustomAttrHash;
		CStdStringPtrMap m_mOptionGroup;
		
		bool m_bForceUseSharedRes;
		TResInfo m_ResInfo;
		
		// 窗口阴影
		CShadowUI m_shadow;
		
		// DPI管理器
		CDPI* m_pDPI;
		// 是否开启Gdiplus
		bool m_bUseGdiplusText;
		int m_trh;
		ULONG_PTR m_gdiplusToken;
		Gdiplus::GdiplusStartupInput *m_pGdiplusStartupInput;

		// 拖拽
		bool m_bDragDrop;
		bool m_bDragMode;
		HBITMAP m_hDragBitmap;
		IDragDropUI *m_pDragDrop;

		// 触摸
		POINT m_ptLastTouch;
		bool m_bTouchSupported;
		
		//
		// Attribute-name runtime validation switch.
		// [R43] No longer Debug-only: the Release binary is what users run, and
		// keeping the whole diagnostic path behind _DEBUG meant a misspelled or
		// misplaced attribute was silent in exactly the build that ships.
		// Still defaults to false, so nothing is paid unless explicitly enabled.
		bool m_bAttributeValidation;

		//
		static HINSTANCE m_hInstance;
		static HINSTANCE m_hResourceInstance;
		static CDuiString m_pStrResourcePath;
		static CDuiString m_pStrResourceZip;
		static CDuiString m_pStrResourceZipPwd;
		static HANDLE m_hResourceZip;
        static BYTE* m_cbZipBuf;

		static bool m_bCachedResourceZip;
		static int m_nResType;
		static TResInfo m_SharedResInfo;
		static bool m_bUseHSL;
		static short m_H;
		static short m_S;
		static short m_L;
		static CStdPtrArray m_aPreMessages;
		static CStdPtrArray m_aPlugins;
	};

} // namespace DuiLib

#endif // __UIMANAGER_H__
