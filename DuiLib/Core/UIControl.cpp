#include "StdAfx.h"
#include "Core/ThreadAffinity.h"
#include "S3AttrRegistry.h"
#include "ExtRegistry.h"
#include "AttrParse.h"

#include <mutex> // [SEC-20] live-manager registry


// [T5.40] W4 batch-6: suppress data-truncation warnings (C4244/C4302).
// DuiLib sits at the GDI+/Win32 API boundary and relies on narrow types
// (int/BYTE/WORD) at call sites where value domains are provably bounded:
//   * LONG/LONG_PTR/UINT_PTR/WPARAM -> int  (UI coords/handles, int32 safe)
//   * LONG -> Gdiplus::REAL             (GDI+ API requires float coords)
//   * int  -> BYTE / float               (opacity / percentage within range)
//   * LPVOID -> int                     (CStdPtrArray int-as-pointer borrow,
//                                       see docs/t532-w4-b3-pointer-cast-audit.json)
//   * LPWSTR -> WORD/TCHAR              (16-bit unicode char truncation by design)
// Modernization path: migrate CStdPtrArray<LPVOID> to std::vector<int> /
// std::vector<COLORREF>, and use explicit width/height structs instead of
// mixing LONG with Gdiplus::REAL.  See docs/t538 and docs/t539 scans.
#pragma warning(disable: 4244 4302)
// [T5.40] end


namespace DuiLib {
	void DuiLibRegisterManager(const void* p);
	void DuiLibUnregisterManager(const void* p);
	bool DuiLibIsManagerAlive(const void* p);
	IMPLEMENT_DUICONTROL(CControlUI)

	// [SEC-20] Live paint-manager registry.
	//
	// CControlUI stores a non-owning CPaintManagerUI* back-pointer and calls
	// ReapObjects() from its destructor. When the manager dies first (control
	// outlives it, or teardown order differs), that call wrote into freed
	// memory: ASan reports heap-use-after-free in CPaintManagerUI::ReapObjects.
	//
	// A raw pointer cannot be validated, so managers register themselves while
	// alive. The registry lives here (file scope, no new exports) so the class
	// layout and DUILIB_ABI_TAG stay untouched.
	namespace {
		std::mutex g_managerMutex;
		const void* g_pManagers[64] = {};
		int g_nManagers = 0;

		void RegisterManager(const void* p)
		{
			std::lock_guard<std::mutex> lock(g_managerMutex);
			for (int i = 0; i < g_nManagers; ++i) if (g_pManagers[i] == p) return;
			if (g_nManagers < 64) g_pManagers[g_nManagers++] = p;
		}

		void UnregisterManager(const void* p)
		{
			std::lock_guard<std::mutex> lock(g_managerMutex);
			for (int i = 0; i < g_nManagers; ++i) {
				if (g_pManagers[i] == p) {
					g_pManagers[i] = g_pManagers[--g_nManagers];
					g_pManagers[g_nManagers] = nullptr;
					return;
				}
			}
		}

		bool IsManagerAlive(const void* p)
		{
			std::lock_guard<std::mutex> lock(g_managerMutex);
			for (int i = 0; i < g_nManagers; ++i) if (g_pManagers[i] == p) return true;
			return false;
		}
	} // namespace

	void DuiLibRegisterManager(const void* p) { RegisterManager(p); }
	void DuiLibUnregisterManager(const void* p) { UnregisterManager(p); }
	bool DuiLibIsManagerAlive(const void* p) { return IsManagerAlive(p); }

		CControlUI::CControlUI()
		:m_pManager(nullptr), 
		m_pParent(nullptr), 
		m_bUpdateNeeded(true),
		m_bMenuUsed(false),
		m_bVisible(true), 
		m_bInternVisible(true),
		m_bFocused(false),
		m_bEnabled(true),
		m_bMouseEnabled(true),
		m_bKeyboardEnabled(true),
		m_bFloat(false),
		m_uFloatAlign(0),
		m_bSetPos(false),
		m_bRichEvent(false),
		m_bDragEnabled(false),
		m_bDropEnabled(false),
		m_bResourceText(false),
		m_chShortcut('\0'),
		m_pTag(0),
		m_dwBackColor(0),
		m_dwBackColor2(0),
		m_dwBackColor3(0),
		m_dwForeColor(0),
		m_dwBorderColor(0),
		m_dwFocusBorderColor(0),
		m_bColorHSL(false),
		m_nBorderSize(0),
		m_nBorderStyle(PS_SOLID),
		m_nTooltipWidth(300),
		m_wCursor(0),
		m_instance(nullptr)
	{
		m_cXY.cx = m_cXY.cy = 0;
		m_cxyFixed.cx = m_cxyFixed.cy = 0;
		m_cxyMin.cx = m_cxyMin.cy = 0;
		m_cxyMax.cx = m_cxyMax.cy = 9999;
		m_cxyBorderRound.cx = m_cxyBorderRound.cy = 0;

		::ZeroMemory(&m_rcPadding, sizeof(RECT));
		::ZeroMemory(&m_rcItem, sizeof(RECT));
		::ZeroMemory(&m_rcPaint, sizeof(RECT));
		::ZeroMemory(&m_rcBorderSize,sizeof(RECT));
		m_piFloatPercent.left = m_piFloatPercent.top = m_piFloatPercent.right = m_piFloatPercent.bottom = 0.0f;
	}

	CControlUI::~CControlUI()
	{
		if( OnDestroy ) OnDestroy(this);
		RemoveAllCustomAttribute();	
		// [SEC-20] m_pManager is a non-owning back-pointer. If the paint manager
		// was destroyed first (control outlives it, e.g. a control handed to
		// AttachDialog and later released by the caller, or a manager torn down
		// during shutdown), this call dereferenced freed memory: ASan reports
		// heap-use-after-free in CPaintManagerUI::ReapObjects.
		// A live manager always clears its back-pointers before it dies (see
		// CPaintManagerUI::~CPaintManagerUI -> ReapObjects), so a null check is
		// the correct guard here; IsWindow would cost an extra syscall.
		CPaintManagerUI* pManager = m_pManager;
		m_pManager = nullptr;
		if( pManager != nullptr && DuiLibIsManagerAlive(pManager) ) pManager->ReapObjects(this);
	}

	const CDuiString& CControlUI::GetName() const
	{
		return m_sName;
	}

	void CControlUI::SetName(LPCTSTR pstrName)
	{
		m_sName = pstrName;
	}

	LPVOID CControlUI::GetInterface(LPCTSTR pstrName)
	{
		if( _tcsicmp(pstrName, DUI_CTR_CONTROL) == 0 ) return this;
		return nullptr;
	}

	LPCTSTR CControlUI::GetClass() const
	{
		return _T("ControlUI");
	}

	UINT CControlUI::GetControlFlags() const
	{
		return 0;
	}

	bool CControlUI::Activate()
	{
		if( !IsVisible() ) return false;
		if( !IsEnabled() ) return false;
		return true;
	}

	CPaintManagerUI* CControlUI::GetManager() const
	{
		return m_pManager;
	}

	void CControlUI::SetManager(CPaintManagerUI* pManager, CControlUI* pParent, bool bInit)
	{
		m_pManager = pManager;
		m_pParent = pParent;
		if( bInit && m_pParent ) Init();
	}

	CControlUI* CControlUI::GetParent() const
	{
		return m_pParent;
	}

	bool CControlUI::SetTimer(UINT nTimerID, UINT nElapse)
	{
		if(m_pManager == nullptr) return false;

		return m_pManager->SetTimer(this, nTimerID, nElapse);
	}

	void CControlUI::KillTimer(UINT nTimerID)
	{
		if(m_pManager == nullptr) return;

		m_pManager->KillTimer(this, nTimerID);
	}

	CDuiString CControlUI::GetText() const
	{
		if (!IsResourceText()) return m_sText;
		return CResourceManager::GetInstance()->GetText(m_sText);
	}

	void CControlUI::SetText(LPCTSTR pstrText)
	{
		// [THREAD-1] UI-thread only; no-op in Release and when affinity
		// has not been claimed yet (controls built before AttachDialog).
		DUI_ASSERT_UI_THREAD_FOR(m_pManager);
		if( m_sText == pstrText ) return;

		m_sText = pstrText;
		// 解析xml换行符
		m_sText.Replace(_T("{\\n}"), _T("\n"));
		Invalidate();
	}

	bool CControlUI::IsResourceText() const
	{
		return m_bResourceText;
	}

	void CControlUI::SetResourceText(bool bResource)
	{
		if( m_bResourceText == bResource ) return;
		m_bResourceText = bResource;
		Invalidate();
	}

	bool CControlUI::IsDragEnabled() const
	{
		return m_bDragEnabled;
	}

	void CControlUI::SetDragEnable(bool bDrag)
	{
		m_bDragEnabled = bDrag;
	}

	bool CControlUI::IsDropEnabled() const
	{
		return m_bDropEnabled;
	}

	void CControlUI::SetDropEnable(bool bDrop)
	{
		m_bDropEnabled = bDrop;
	}


	bool CControlUI::IsRichEvent() const
	{
		return m_bRichEvent;
	}

	void CControlUI::SetRichEvent(bool bEnable)
	{
		m_bRichEvent = bEnable;
	}

	LPCTSTR CControlUI::GetGradient()
	{
		return m_sGradient;
	}

	void CControlUI::SetGradient(LPCTSTR pStrImage)
	{
		if( m_sGradient == pStrImage ) return;

		m_sGradient = pStrImage;
		Invalidate();
	}

	DWORD CControlUI::GetBkColor() const
	{
		return m_dwBackColor;
	}

	void CControlUI::SetBkColor(DWORD dwBackColor)
	{
		if( m_dwBackColor == dwBackColor ) return;

		m_dwBackColor = dwBackColor;
		Invalidate();
	}

	DWORD CControlUI::GetBkColor2() const
	{
		return m_dwBackColor2;
	}

	void CControlUI::SetBkColor2(DWORD dwBackColor)
	{
		if( m_dwBackColor2 == dwBackColor ) return;

		m_dwBackColor2 = dwBackColor;
		Invalidate();
	}

	DWORD CControlUI::GetBkColor3() const
	{
		return m_dwBackColor3;
	}

	void CControlUI::SetBkColor3(DWORD dwBackColor)
	{
		if( m_dwBackColor3 == dwBackColor ) return;

		m_dwBackColor3 = dwBackColor;
		Invalidate();
	}

	DWORD CControlUI::GetForeColor() const
	{
		return m_dwForeColor;
	}

	void CControlUI::SetForeColor(DWORD dwForeColor)
	{
		if( m_dwForeColor == dwForeColor ) return;

		m_dwForeColor = dwForeColor;
		Invalidate();
	}

	LPCTSTR CControlUI::GetBkImage()
	{
		return m_sBkImage;
	}

	void CControlUI::SetBkImage(LPCTSTR pStrImage)
	{
		if( m_sBkImage == pStrImage ) return;

		m_sBkImage = pStrImage;
		Invalidate();
	}
	
	LPCTSTR CControlUI::GetForeImage() const
	{
		return m_sForeImage;
	}

	void CControlUI::SetForeImage(LPCTSTR pStrImage)
	{
		if( m_sForeImage == pStrImage ) return;

		m_sForeImage = pStrImage;
		Invalidate();
	}

	DWORD CControlUI::GetBorderColor() const
	{
		return m_dwBorderColor;
	}

	void CControlUI::SetBorderColor(DWORD dwBorderColor)
	{
		if( m_dwBorderColor == dwBorderColor ) return;

		m_dwBorderColor = dwBorderColor;
		Invalidate();
	}

	DWORD CControlUI::GetFocusBorderColor() const
	{
		return m_dwFocusBorderColor;
	}

	void CControlUI::SetFocusBorderColor(DWORD dwBorderColor)
	{
		if( m_dwFocusBorderColor == dwBorderColor ) return;

		m_dwFocusBorderColor = dwBorderColor;
		Invalidate();
	}

	bool CControlUI::IsColorHSL() const
	{
		return m_bColorHSL;
	}

	void CControlUI::SetColorHSL(bool bColorHSL)
	{
		if( m_bColorHSL == bColorHSL ) return;

		m_bColorHSL = bColorHSL;
		Invalidate();
	}

	int CControlUI::GetBorderSize() const
	{
		if(m_pManager != nullptr) return m_pManager->GetDPIObj()->Scale(m_nBorderSize);
		return m_nBorderSize;
	}

	void CControlUI::SetBorderSize(int nSize)
	{
		if( m_nBorderSize == nSize ) return;

		m_nBorderSize = nSize;
		Invalidate();
	}

	RECT CControlUI::GetBorderRectSize() const
	{
		RECT rcBorderSize = m_rcBorderSize;
		if(m_pManager != nullptr) m_pManager->GetDPIObj()->Scale(&rcBorderSize);
		return rcBorderSize;
	}

	void CControlUI::SetBorderSize( RECT rc )
	{
		m_rcBorderSize = rc;
		Invalidate();
	}

	SIZE CControlUI::GetBorderRound() const
	{
		SIZE cxyBorderRound = m_cxyBorderRound;
		if(m_pManager != nullptr) m_pManager->GetDPIObj()->Scale(&cxyBorderRound);
		return cxyBorderRound;
	}

	void CControlUI::SetBorderRound(SIZE cxyRound)
	{
		m_cxyBorderRound = cxyRound;
		Invalidate();
	}

	// [WIN11-ADAPT] 渲染期使用的圆角半径：按当前 DPI 缩放。
	// 为什么必须单独取：m_cxyBorderRound 保存的是皮肤/代码里写的设计值
	// （如 4），它不是物理像素。原实现里只有 UIGroupBox 自己做了 Scale，
	// 按钮的填充与描边都用原始值，于是 150%/200% 缩放下圆角维持 4 物理
	// 像素、控件却已放大，观感上圆角"变尖"；同时组框与按钮在同一界面
	// 上的圆角也不一致。统一到本函数后两者行为一致。
	SIZE CControlUI::GetBorderRoundScaled() const
	{
		SIZE cxy = m_cxyBorderRound;
		if (m_pManager != nullptr) m_pManager->GetDPIObj()->Scale(&cxy);
		return cxy;
	}

	bool CControlUI::DrawImage(HDC hDC, LPCTSTR pStrImage, LPCTSTR pStrModify)
	{
		return CRenderEngine::DrawImageString(hDC, m_pManager, m_rcItem, m_rcPaint, pStrImage, pStrModify, m_instance);
	}

	const RECT& CControlUI::GetPos() const
	{
		return m_rcItem;
	}

	RECT CControlUI::GetRelativePos() const
	{
		CControlUI* pParent = GetParent();
		if( pParent != nullptr ) {
			RECT rcParentPos = pParent->GetPos();
			CDuiRect rcRelativePos(m_rcItem);
			rcRelativePos.Offset(-rcParentPos.left, -rcParentPos.top);
			return rcRelativePos;
		}
		else {
			return CDuiRect(0, 0, 0, 0);
		}
	}

	RECT CControlUI::GetClientPos() const 
	{
		return m_rcItem;
	}

	void CControlUI::SetPos(RECT rc, bool bNeedInvalidate)
	{
		// [THREAD-1] UI-thread only; no-op in Release and when affinity
		// has not been claimed yet (controls built before AttachDialog).
		DUI_ASSERT_UI_THREAD_FOR(m_pManager);
		if( rc.right < rc.left ) rc.right = rc.left;
		if( rc.bottom < rc.top ) rc.bottom = rc.top;

		// [PERF-3] Layout runs per message and usually recomputes the same rects.
		// Returning early avoids an O(depth) ancestor walk plus a redundant
		// InvalidateRect for every unchanged control, which dominates scrolling
		// and resize in large pages.
		// Only for non-degenerate rects: the initial 0,0,0,0 layout must still run
		// OnSize() once, because several controls size their internals there.
		if( (rc.right > rc.left || rc.bottom > rc.top) &&
			m_rcItem.left == rc.left && m_rcItem.top == rc.top &&
			m_rcItem.right == rc.right && m_rcItem.bottom == rc.bottom ) {
			if( m_pManager == nullptr ) return;
			m_bUpdateNeeded = false;
			return;
		}

		CDuiRect invalidateRc = m_rcItem;
		if( ::IsRectEmpty(&invalidateRc) ) invalidateRc = rc;

		m_rcItem = rc;
		if( m_pManager == nullptr ) return;

		if( !m_bSetPos ) {
			m_bSetPos = true;
			if( OnSize ) OnSize(this);
			m_bSetPos = false;
		}

		m_bUpdateNeeded = false;

		if( bNeedInvalidate && IsVisible() ) {
			invalidateRc.Join(m_rcItem);
			CControlUI* pParent = this;
			RECT rcTemp;
			RECT rcParent;
			while( (pParent = pParent->GetParent()) ) {
				if( !pParent->IsVisible() ) return;
				rcTemp = invalidateRc;
				rcParent = pParent->GetPos();
				if( !::IntersectRect(&invalidateRc, &rcTemp, &rcParent) ) return;
			}
			m_pManager->Invalidate(invalidateRc);
		}
	}

	void CControlUI::Move(SIZE szOffset, bool bNeedInvalidate)
	{
		m_cXY.cx += szOffset.cx;
		m_cXY.cy += szOffset.cy;
		NeedParentUpdate();
	}

	int CControlUI::GetWidth() const
	{
		return m_rcItem.right - m_rcItem.left;
	}

	int CControlUI::GetHeight() const
	{
		return m_rcItem.bottom - m_rcItem.top;
	}

	int CControlUI::GetX() const
	{
		return m_rcItem.left;
	}

	int CControlUI::GetY() const
	{
		return m_rcItem.top;
	}

	RECT CControlUI::GetPadding() const
	{
		RECT rcPadding = m_rcPadding;
		if(m_pManager != nullptr) m_pManager->GetDPIObj()->Scale(&rcPadding);
		return rcPadding;
	}

	void CControlUI::SetPadding(RECT rcPadding)
	{
		m_rcPadding = rcPadding;
		NeedParentUpdate();
	}

	SIZE CControlUI::GetFixedXY() const
	{
		SIZE cXY = m_cXY;
		if(m_pManager != nullptr) m_pManager->GetDPIObj()->Scale(&cXY);
		return cXY;
	}

	void CControlUI::SetFixedXY(SIZE szXY)
	{
		m_cXY.cx = szXY.cx;
		m_cXY.cy = szXY.cy;
		NeedParentUpdate();
	}

	SIZE CControlUI::GetFixedSize() const
	{
		SIZE cxyFixed = m_cxyFixed;
		if(m_pManager != nullptr) m_pManager->GetDPIObj()->Scale(&cxyFixed);
		return cxyFixed;
	}

	int CControlUI::GetFixedWidth() const
	{
		SIZE cxyFixed = m_cxyFixed;
		if(m_pManager != nullptr) m_pManager->GetDPIObj()->Scale(&cxyFixed);

		return cxyFixed.cx;
	}

	void CControlUI::SetFixedWidth(int cx)
	{
		if( cx < 0 ) return; 
		m_cxyFixed.cx = cx;
		NeedParentUpdate();
	}

	int CControlUI::GetFixedHeight() const
	{
		SIZE cxyFixed = m_cxyFixed;
		if(m_pManager != nullptr) m_pManager->GetDPIObj()->Scale(&cxyFixed);

		return cxyFixed.cy;
	}

	void CControlUI::SetFixedHeight(int cy)
	{
		if( cy < 0 ) return; 
		m_cxyFixed.cy = cy;
		NeedParentUpdate();
	}

	int CControlUI::GetMinWidth() const
	{
		SIZE cxyMin = m_cxyMin;
		if(m_pManager != nullptr) m_pManager->GetDPIObj()->Scale(&cxyMin);
		return cxyMin.cx;
	}

	void CControlUI::SetMinWidth(int cx)
	{
		if( m_cxyMin.cx == cx ) return;

		if( cx < 0 ) return; 
		m_cxyMin.cx = cx;
		NeedParentUpdate();
	}

	int CControlUI::GetMaxWidth() const
	{
		SIZE cxyMax = m_cxyMax;
		if(m_pManager != nullptr) m_pManager->GetDPIObj()->Scale(&cxyMax);
		return cxyMax.cx;
	}

	void CControlUI::SetMaxWidth(int cx)
	{
		if( m_cxyMax.cx == cx ) return;

		if( cx < 0 ) return; 
		m_cxyMax.cx = cx;
		NeedParentUpdate();
	}

	int CControlUI::GetMinHeight() const
	{
		SIZE cxyMin = m_cxyMin;
		if(m_pManager != nullptr) m_pManager->GetDPIObj()->Scale(&cxyMin);
		
		return cxyMin.cy;
	}

	void CControlUI::SetMinHeight(int cy)
	{
		if( m_cxyMin.cy == cy ) return;

		if( cy < 0 ) return; 
		m_cxyMin.cy = cy;
		NeedParentUpdate();
	}

	int CControlUI::GetMaxHeight() const
	{
		SIZE cxyMax = m_cxyMax;
		if(m_pManager != nullptr) m_pManager->GetDPIObj()->Scale(&cxyMax);
		return cxyMax.cy;
	}

	void CControlUI::SetMaxHeight(int cy)
	{
		if( m_cxyMax.cy == cy ) return;

		if( cy < 0 ) return; 
		m_cxyMax.cy = cy;
		NeedParentUpdate();
	}

	TPercentInfo CControlUI::GetFloatPercent() const
	{
		return m_piFloatPercent;
	}
	
	void CControlUI::SetFloatPercent(TPercentInfo piFloatPercent)
	{
		m_piFloatPercent = piFloatPercent;
		NeedParentUpdate();
	}

	void CControlUI::SetFloatAlign(UINT uAlign)
	{
		m_uFloatAlign = uAlign;
		NeedParentUpdate();
	}

	UINT CControlUI::GetFloatAlign() const
	{
		return m_uFloatAlign;
	}

	CDuiString CControlUI::GetToolTip() const
	{
		if (!IsResourceText()) return m_sToolTip;
		return CResourceManager::GetInstance()->GetText(m_sToolTip);
	}

	void CControlUI::SetToolTip(LPCTSTR pstrText)
	{
		CDuiString strTemp(pstrText);
		strTemp.Replace(_T("<n>"),_T("\r\n"));
		m_sToolTip = strTemp;
	}

	void CControlUI::SetToolTipWidth( int nWidth )
	{
		m_nTooltipWidth = nWidth;
	}

	int CControlUI::GetToolTipWidth( void )
	{
		if(m_pManager != nullptr) return m_pManager->GetDPIObj()->Scale(m_nTooltipWidth);
		return m_nTooltipWidth;
	}
	
	WORD CControlUI::GetCursor()
	{
		return m_wCursor;
	}

	void CControlUI::SetCursor(WORD wCursor)
	{
		m_wCursor = wCursor;
		Invalidate();
	}

	TCHAR CControlUI::GetShortcut() const
	{
		return m_chShortcut;
	}

	void CControlUI::SetShortcut(TCHAR ch)
	{
		m_chShortcut = ch;
	}

	bool CControlUI::IsContextMenuUsed() const
	{
		return m_bMenuUsed;
	}

	void CControlUI::SetContextMenuUsed(bool bMenuUsed)
	{
		m_bMenuUsed = bMenuUsed;
	}

	const CDuiString& CControlUI::GetUserData()
	{
		return m_sUserData;
	}

	void CControlUI::SetUserData(LPCTSTR pstrText)
	{
		m_sUserData = pstrText;
	}

	UINT_PTR CControlUI::GetTag() const
	{
		return m_pTag;
	}

	void CControlUI::SetTag(UINT_PTR pTag)
	{
		m_pTag = pTag;
	}

	bool CControlUI::IsVisible() const
	{

		return m_bVisible && m_bInternVisible;
	}

	void CControlUI::SetVisible(bool bVisible)
	{
		// [THREAD-1] UI-thread only; no-op in Release and when affinity
		// has not been claimed yet (controls built before AttachDialog).
		DUI_ASSERT_UI_THREAD_FOR(m_pManager);
		if( m_bVisible == bVisible ) return;

		const bool bWasVisible = IsVisible();
		m_bVisible = bVisible;
		if( !bVisible ) {
			if( m_bFocused ) m_bFocused = false;
			// Move focus to the next usable control instead of dropping it.
			// Dropping focus left the window with no focus owner, so Tab
			// restarted from the first control and keyboard users lost their place.
			if (m_pManager != nullptr && m_pManager->GetFocus() == this) {
				m_pManager->RelocateFocusFrom(this);
			}
		}
		if( IsVisible() != bWasVisible ) {
			NeedParentUpdate();
		}
	}

	void CControlUI::SetInternVisible(bool bVisible)
	{
		if( m_bInternVisible == bVisible ) return;
		m_bInternVisible = bVisible;
		if( !bVisible ) {
			if( m_bFocused ) m_bFocused = false;
			if (m_pManager != nullptr && m_pManager->GetFocus() == this) {
				m_pManager->RelocateFocusFrom(this);
			}
		}
	}

	bool CControlUI::IsEnabled() const
	{
		return m_bEnabled;
	}

	void CControlUI::SetEnabled(bool bEnabled)
	{
		// [THREAD-1] UI-thread only; no-op in Release and when affinity
		// has not been claimed yet (controls built before AttachDialog).
		DUI_ASSERT_UI_THREAD_FOR(m_pManager);
		if( m_bEnabled == bEnabled ) return;

		m_bEnabled = bEnabled;
		if( !bEnabled ) {
			// A disabled control cannot own keyboard focus: hand it on, otherwise
			// the focused-but-dead state swallowed arrow/space keys silently.
			if( m_bFocused ) m_bFocused = false;
			if (m_pManager != nullptr && m_pManager->GetFocus() == this) {
				m_pManager->RelocateFocusFrom(this);
			}
		}
		Invalidate();
	}

	bool CControlUI::IsMouseEnabled() const
	{
		return m_bMouseEnabled;
	}

	void CControlUI::SetMouseEnabled(bool bEnabled)
	{
		m_bMouseEnabled = bEnabled;
	}

	bool CControlUI::IsKeyboardEnabled() const
	{
		return m_bKeyboardEnabled ;
	}
	void CControlUI::SetKeyboardEnabled(bool bEnabled)
	{
		if (m_bKeyboardEnabled == bEnabled) return;
		m_bKeyboardEnabled = bEnabled;
		if (!bEnabled) {
			// Losing tab-stop status must also give up focus; otherwise the
			// control keeps receiving keys it is no longer allowed to handle.
			if (m_pManager != nullptr && m_pManager->GetFocus() == this) {
				m_pManager->RelocateFocusFrom(this);
			}
		}
	}

	bool CControlUI::IsFocused() const
	{
		return m_bFocused;
	}

	void CControlUI::SetFocus()
	{
		if( m_pManager != nullptr ) m_pManager->SetFocus(this);
	}

	bool CControlUI::IsFloat() const
	{
		return m_bFloat;
	}

	void CControlUI::SetFloat(bool bFloat)
	{
		if( m_bFloat == bFloat ) return;

		m_bFloat = bFloat;
		NeedParentUpdate();
	}

	CControlUI* CControlUI::FindControl(FINDCONTROLPROC Proc, LPVOID pData, UINT uFlags)
	{
		if( (uFlags & UIFIND_VISIBLE) != 0 && !IsVisible() ) return nullptr;
		if( (uFlags & UIFIND_ENABLED) != 0 && !IsEnabled() ) return nullptr;
		if( (uFlags & UIFIND_HITTEST) != 0 && (!m_bMouseEnabled || !::PtInRect(&m_rcItem, * static_cast<LPPOINT>(pData))) ) return nullptr;
		return Proc(this, pData);
	}

	void CControlUI::Invalidate()
	{
		if( !IsVisible() ) return;

		RECT invalidateRc = m_rcItem;

		CControlUI* pParent = this;
		RECT rcTemp;
		RECT rcParent;
		while( (pParent = pParent->GetParent()) )
		{
			rcTemp = invalidateRc;
			rcParent = pParent->GetPos();
			if( !::IntersectRect(&invalidateRc, &rcTemp, &rcParent) ) 
			{
				return;
			}
		}

		if( m_pManager != nullptr ) m_pManager->Invalidate(invalidateRc);
	}

	bool CControlUI::IsUpdateNeeded() const
	{
		return m_bUpdateNeeded;
	}

	void CControlUI::NeedUpdate()
	{
		if( !IsVisible() ) return;
		m_bUpdateNeeded = true;
		Invalidate();

		if( m_pManager != nullptr ) m_pManager->NeedUpdate();
	}

	void CControlUI::NeedParentUpdate()
	{
		if( GetParent() ) {
			GetParent()->NeedUpdate();
			GetParent()->Invalidate();
		}
		else {
			NeedUpdate();
		}

		if( m_pManager != nullptr ) m_pManager->NeedUpdate();
	}

	DWORD CControlUI::GetAdjustColor(DWORD dwColor)
	{
		if( !m_bColorHSL ) return dwColor;
		short H, S, L;
		CPaintManagerUI::GetHSL(&H, &S, &L);
		return CRenderEngine::AdjustColor(dwColor, H, S, L);
	}

	void CControlUI::Init()
	{
		DoInit();
		if( OnInit ) OnInit(this);
	}

	void CControlUI::DoInit()
	{

	}

	void CControlUI::Event(TEventUI& event)
	{
		if( OnEvent(&event) ) DoEvent(event);
	}

	void CControlUI::DoEvent(TEventUI& event)
	{
		if( event.Type == UIEVENT_SETCURSOR ) {
			if( GetCursor() ) {
				::SetCursor(::LoadCursor(nullptr, MAKEINTRESOURCE(GetCursor())));
			}
			else {
				::SetCursor(::LoadCursor(nullptr, MAKEINTRESOURCE(IDC_ARROW)));
			}
			return;
		}

		if( event.Type == UIEVENT_SETFOCUS ) 
		{
			m_bFocused = true;
			Invalidate();
			return;
		}
		if( event.Type == UIEVENT_KILLFOCUS ) 
		{
			m_bFocused = false;
			Invalidate();
			return;
		}
		if( event.Type == UIEVENT_TIMER )
		{
			// [SEC-37] Notify needs a manager to dispatch to. A detached control can
			// still receive a timer event (the message loop may deliver one queued
			// before SetManager(nullptr)), and this branch dereferenced the pointer:
			// the ASan probe reports TIMER crashes on 21 control classes.
			if( m_pManager != nullptr )
				m_pManager->SendNotify(this, DUI_MSGTYPE_TIMER, event.wParam, event.lParam);
			return;
		}
		if( event.Type == UIEVENT_CONTEXTMENU )
		{
			if( IsContextMenuUsed() && m_pManager != nullptr ) {
				m_pManager->SendNotify(this, DUI_MSGTYPE_MENU, event.wParam, event.lParam);
				return;
			}
		}

		if( m_pParent != nullptr ) m_pParent->DoEvent(event);
	}


	void CControlUI::SetVirtualWnd(LPCTSTR pstrValue)
	{
		m_sVirtualWnd = pstrValue;
		if (m_pManager != nullptr) m_pManager->UsedVirtualWnd(true);
	}

	CDuiString CControlUI::GetVirtualWnd() const
	{
		CDuiString str;
		if( !m_sVirtualWnd.IsEmpty() ){
			str = m_sVirtualWnd;
		}
		else{
			CControlUI* pParent = GetParent();
			if( pParent != nullptr){
				str = pParent->GetVirtualWnd();
			}
			else{
				str = _T("");
			}
		}
		return str;
	}

	void CControlUI::AddCustomAttribute(LPCTSTR pstrName, LPCTSTR pstrAttr)
	{
		if( pstrName == nullptr || pstrName[0] == _T('\0') || pstrAttr == nullptr || pstrAttr[0] == _T('\0') ) return;

		// ---- N1/T5.6 + T5.21: attribute-name runtime validation ----
		// Reaching this point means the control's S3 fast path and every
		// legacy branch left the attribute unhandled.
		// Three-tier classification (docs/c8-whitelist-design.md D4):
		//   1) S3 hit  -> framework property (handled in SetAttribute, not here)
		//   2) ext hit -> third-party extension property (whitelisted by design)
		//   3) both miss -> N1 diagnostic output, still stored for backward compat
		
			// [R43] Not Debug-only any more. Keeping this call site inside
			// #ifdef _DEBUG meant the Release binary -- the one users run -- had
			// the diagnostic compiled out entirely, so a bad attribute produced no
			// warning, no log and no visual symptom. Guarded at runtime instead by
			// IsAttributeValidationEnabled(), which defaults to false.

			if (m_pManager != nullptr && m_pManager->IsAttributeValidationEnabled())
			{
				// Tier 1: S3 compile-time registry (framework properties)
				if (DuiLib::s3::lookup_prop_t(pstrName) == DuiLib::s3::PropId::Unknown)
				{
					// Tier 2: ext runtime registry (third-party extensions)
					// DuiLib::ext is header-only + Meyers singleton; lookup is
					// a mutex-guarded hashmap probe — cheap enough for one-shot
					// use at skin parse time. When hit, we skip N1 diagnostics
					// because the name is explicitly whitelisted by design.
					//
					// NOTE: pstrName may be either TCHAR (Unicode wchar_t) or
					// char (MBCS). DuiLib::ext stores names as std::string
					// (narrow). We narrow Unicode -> MBCS before lookup so the
					// lookup is case-insensitive and matches Normalize()'s
					// lowercase+trim contract.
					const char* narrow = nullptr;
					#ifdef _UNICODE
						// Unicode build: convert wchar_t -> MBCS char (8-bit ANSI)
						wchar_t bufW[512];
						if (wcslen(pstrName) < 512)
						{
							wcscpy_s(bufW, 512, pstrName);
							char bufA[1024];
							int n = WideCharToMultiByte(CP_ACP, 0,
							                            bufW, -1, bufA, 1024, nullptr, nullptr);
							if (n > 0)
							{
								bufA[n > 0 && n < 1024 ? (n - 1) : 1023] = '\0';
								narrow = bufA;
							}
						}
					#else
						narrow = pstrName;
					#endif
					

					if (narrow == nullptr || DuiLib::ext::lookup_ext_prop(narrow) == DuiLib::ext::EXT_ID_NONE)
					{
						// Tier 3: neither S3 nor ext -> N1 diagnostic
						// One warning per distinct attribute name per process.
						if (CPaintManagerUI::ReportAttributeValidationUnknown(pstrName))
						{
							DUITRACE(_T("[N1] unknown attribute \"%s\" (ignored, not a framework property)\n"), pstrName);
						}
					}
				}
			}

		if (m_mCustomAttrHash.Find(pstrName) == nullptr) {
			CDuiString* pCostomAttr = new CDuiString(pstrAttr);
			if (pCostomAttr != nullptr) {
				m_mCustomAttrHash.Set(pstrName, reinterpret_cast<LPVOID>(pCostomAttr));
			}
		}
	}

	LPCTSTR CControlUI::GetCustomAttribute(LPCTSTR pstrName) const
	{
		if( pstrName == nullptr || pstrName[0] == _T('\0') ) return nullptr;
		CDuiString* pCostomAttr = static_cast<CDuiString*>(m_mCustomAttrHash.Find(pstrName));
		if( pCostomAttr ) return pCostomAttr->GetData();
		return nullptr;
	}

	bool CControlUI::RemoveCustomAttribute(LPCTSTR pstrName)
	{
		if( pstrName == nullptr || pstrName[0] == _T('\0') ) return false;
		CDuiString* pCostomAttr = static_cast<CDuiString*>(m_mCustomAttrHash.Find(pstrName));
		if( !pCostomAttr ) return false;

		delete pCostomAttr;
		return m_mCustomAttrHash.Remove(pstrName);
	}

	void CControlUI::RemoveAllCustomAttribute()
	{
		CDuiString* pCostomAttr;
		for( int i = 0; i< m_mCustomAttrHash.GetSize(); i++ ) {
			if(LPCTSTR key = m_mCustomAttrHash.GetAt(i)) {
				pCostomAttr = static_cast<CDuiString*>(m_mCustomAttrHash.Find(key));
				delete pCostomAttr;
			}
		}
		m_mCustomAttrHash.Resize();
	}

	// ---- [H5] C++17 separator-tolerant attribute parsing helpers ----
	// The old code called _tcstol(pstr + 1) unconditionally, which read one
	// character past the NUL terminator when the value was exhausted (e.g.
	// pos="100"). The helpers in DuiLib/Core/AttrParse.h return false as soon as
	// the string ends, so we never touch memory past the terminating NUL. Both
	// the S3 fast path and the legacy chain share these helpers, and derived
	// controls (阶段 2b) include the same header, so no variant can regress
	// independently.
	using DuiLib::attr::NextInt;
	using DuiLib::attr::NextDouble;
	using DuiLib::attr::ParseRect4;
	using DuiLib::attr::ParseSize2;

	inline bool ParseFloat4(LPCTSTR s, TPercentInfo& pi) {
		LPCTSTR p = s;
		return NextDouble(p, pi.left) && NextDouble(p, pi.top) &&
		       NextDouble(p, pi.right) && NextDouble(p, pi.bottom);
	}

	void CControlUI::SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue)
	{		// ---- S3/T3.3a: fast path via compile-time hash registry ----
		// O(log n) lookup instead of O(n) string compares.
		// Falls through to the legacy chain below when unmatched, so
		// third-party custom attributes keep working unchanged.
		switch( DuiLib::s3::lookup_prop_t(pstrName) ) {
		case DuiLib::s3::PropId::innerstyle: { ApplyAttributeList(pstrValue); return; }
		case DuiLib::s3::PropId::pos: { // [H5] C++17 helper: no read past NUL
			RECT rcPos = { 0 };
			if (!ParseRect4(pstrValue, rcPos)) {
				// [SEC-10] CDuiString is not trivially copyable: passing the object
				// through a C variadic (vsnprintf/%s) reads it as a raw char* and
				// corrupts every following argument (MSVC C4840). Pass its buffer.
				DUI_LOGW("CControlUI::SetAttribute bad pos='%s' (class=%s name=%s)", pstrValue, GetClass(), GetName().GetData());
				return;
			}
			SIZE szXY = {rcPos.left >= 0 ? rcPos.left : rcPos.right, rcPos.top >= 0 ? rcPos.top : rcPos.bottom};
			SetFixedXY(szXY); SetFixedWidth(abs(rcPos.right - rcPos.left)); SetFixedHeight(abs(rcPos.bottom - rcPos.top)); return; }
		case DuiLib::s3::PropId::attr_float: { // [H5] C++17 helper
			CDuiString nValue = pstrValue;
			if (nValue.Find(',') < 0) { SetFloat(_tcsicmp(pstrValue, _T("true")) == 0); return; }
			TPercentInfo piFloatPercent = { 0 };
			if (!ParseFloat4(pstrValue, piFloatPercent)) {
				DUI_LOGW("CControlUI::SetAttribute bad float='%s'", pstrValue);
				return;
			}
			SetFloatPercent(piFloatPercent); SetFloat(true); return; }
		case DuiLib::s3::PropId::floatalign: { UINT uAlign = GetFloatAlign(); while( *pstrValue != _T('\0') ) { 				CDuiString sValue; while( *pstrValue == _T(',') || *pstrValue == _T(' ') ) pstrValue = ::CharNext(pstrValue); while( *pstrValue != _T('\0') && *pstrValue != _T(',') && *pstrValue != _T(' ') ) { 					LPTSTR pstrTemp = ::CharNext(pstrValue); while( pstrValue < pstrTemp) { 						sValue += *pstrValue++; } 				} 				if(sValue.CompareNoCase(_T("null")) == 0) { 					uAlign = 0; } 				if( sValue.CompareNoCase(_T("left")) == 0 ) { 					uAlign &= ~(DT_CENTER | DT_RIGHT); uAlign |= DT_LEFT; } 				else if( sValue.CompareNoCase(_T("center")) == 0 ) { 					uAlign &= ~(DT_LEFT | DT_RIGHT); uAlign |= DT_CENTER; } 				else if( sValue.CompareNoCase(_T("right")) == 0 ) { 					uAlign &= ~(DT_LEFT | DT_CENTER); uAlign |= DT_RIGHT; } 				else if( sValue.CompareNoCase(_T("top")) == 0 ) { 					uAlign &= ~(DT_BOTTOM | DT_VCENTER); uAlign |= DT_TOP; } 				else if( sValue.CompareNoCase(_T("vcenter")) == 0 ) { 					uAlign &= ~(DT_TOP | DT_BOTTOM); uAlign |= DT_VCENTER; } 				else if( sValue.CompareNoCase(_T("bottom")) == 0 ) { 					uAlign &= ~(DT_TOP | DT_VCENTER); uAlign |= DT_BOTTOM; } 			} 			SetFloatAlign(uAlign); return; }
		case DuiLib::s3::PropId::padding: { // [H5] C++17 helper
			RECT rcPadding = { 0 };
			if (!ParseRect4(pstrValue, rcPadding)) {
				DUI_LOGW("CControlUI::SetAttribute bad padding='%s'", pstrValue);
				return;
			}
			SetPadding(rcPadding); return; }
		case DuiLib::s3::PropId::gradient: { SetGradient(pstrValue); return; }
		case DuiLib::s3::PropId::bkcolor3: { while( *pstrValue > _T('\0') && *pstrValue <= _T(' ') ) pstrValue = ::CharNext(pstrValue); if( *pstrValue == _T('#')) pstrValue = ::CharNext(pstrValue); LPTSTR pstr = nullptr; DWORD clrColor = _tcstoul(pstrValue, &pstr, 16); SetBkColor3(clrColor); return; }
		case DuiLib::s3::PropId::forecolor: { while( *pstrValue > _T('\0') && *pstrValue <= _T(' ') ) pstrValue = ::CharNext(pstrValue); if( *pstrValue == _T('#')) pstrValue = ::CharNext(pstrValue); LPTSTR pstr = nullptr; DWORD clrColor = _tcstoul(pstrValue, &pstr, 16); SetForeColor(clrColor); return; }
		case DuiLib::s3::PropId::bordercolor: { if( *pstrValue == _T('#')) pstrValue = ::CharNext(pstrValue); LPTSTR pstr = nullptr; DWORD clrColor = _tcstoul(pstrValue, &pstr, 16); SetBorderColor(clrColor); return; }
		case DuiLib::s3::PropId::focusbordercolor: { if( *pstrValue == _T('#')) pstrValue = ::CharNext(pstrValue); LPTSTR pstr = nullptr; DWORD clrColor = _tcstoul(pstrValue, &pstr, 16); SetFocusBorderColor(clrColor); return; }
		case DuiLib::s3::PropId::colorhsl: { SetColorHSL(_tcsicmp(pstrValue, _T("true")) == 0); return; }
		case DuiLib::s3::PropId::bordersize: { // [H5] C++17 helper
			CDuiString nValue = pstrValue;
			if (nValue.Find(',') < 0) { SetBorderSize(_ttoi(pstrValue)); return; }
			RECT rcBorder = { 0 };
			if (!ParseRect4(pstrValue, rcBorder)) {
				DUI_LOGW("CControlUI::SetAttribute bad bordersize='%s'", pstrValue);
				return;
			}
			SetBorderSize(rcBorder); return; }
		case DuiLib::s3::PropId::leftbordersize: { SetLeftBorderSize(_ttoi(pstrValue)); return; }
		case DuiLib::s3::PropId::topbordersize: { SetTopBorderSize(_ttoi(pstrValue)); return; }
		case DuiLib::s3::PropId::rightbordersize: { SetRightBorderSize(_ttoi(pstrValue)); return; }
		case DuiLib::s3::PropId::bottombordersize: { SetBottomBorderSize(_ttoi(pstrValue)); return; }
		case DuiLib::s3::PropId::borderstyle: { SetBorderStyle(_ttoi(pstrValue)); return; }
		case DuiLib::s3::PropId::borderround: { // [H5] C++17 helper
			SIZE cxyRound = { 0 };
			if (!ParseSize2(pstrValue, cxyRound)) {
				DUI_LOGW("CControlUI::SetAttribute bad borderround='%s'", pstrValue);
				return;
			}
			SetBorderRound(cxyRound); return; }
		case DuiLib::s3::PropId::bkimage: { SetBkImage(pstrValue); return; }
		case DuiLib::s3::PropId::foreimage: { SetForeImage(pstrValue); return; }
		case DuiLib::s3::PropId::width: { SetFixedWidth(_ttoi(pstrValue)); return; }
		case DuiLib::s3::PropId::height: { SetFixedHeight(_ttoi(pstrValue)); return; }
		case DuiLib::s3::PropId::minwidth: { SetMinWidth(_ttoi(pstrValue)); return; }
		case DuiLib::s3::PropId::minheight: { SetMinHeight(_ttoi(pstrValue)); return; }
		case DuiLib::s3::PropId::maxwidth: { SetMaxWidth(_ttoi(pstrValue)); return; }
		case DuiLib::s3::PropId::maxheight: { SetMaxHeight(_ttoi(pstrValue)); return; }
		case DuiLib::s3::PropId::name: { SetName(pstrValue); return; }
		case DuiLib::s3::PropId::drag: { SetDragEnable(_tcsicmp(pstrValue, _T("true")) == 0); return; }
		case DuiLib::s3::PropId::drop: { SetDropEnable(_tcsicmp(pstrValue, _T("true")) == 0); return; }
		case DuiLib::s3::PropId::resourcetext: { SetResourceText(_tcsicmp(pstrValue, _T("true")) == 0); return; }
		case DuiLib::s3::PropId::richevent: { SetRichEvent(_tcsicmp(pstrValue, _T("true")) == 0); return; }
		case DuiLib::s3::PropId::text: { SetText(pstrValue); return; }
		case DuiLib::s3::PropId::tooltip: { SetToolTip(pstrValue); return; }
		case DuiLib::s3::PropId::userdata: { SetUserData(pstrValue); return; }
		case DuiLib::s3::PropId::enabled: { SetEnabled(_tcsicmp(pstrValue, _T("true")) == 0); return; }
		case DuiLib::s3::PropId::mouse: { SetMouseEnabled(_tcsicmp(pstrValue, _T("true")) == 0); return; }
		case DuiLib::s3::PropId::keyboard: { SetKeyboardEnabled(_tcsicmp(pstrValue, _T("true")) == 0); return; }
		case DuiLib::s3::PropId::visible: { SetVisible(_tcsicmp(pstrValue, _T("true")) == 0); return; }
		case DuiLib::s3::PropId::shortcut: { SetShortcut(pstrValue[0]); return; }
		case DuiLib::s3::PropId::menu: { SetContextMenuUsed(_tcsicmp(pstrValue, _T("true")) == 0); return; }
		case DuiLib::s3::PropId::virtualwnd: { SetVirtualWnd(pstrValue); return; }
		default: break;
		}

		// ---- legacy fallback chain (kept verbatim for compatibility) ----

		// 样式表
		if(m_pManager != nullptr &&  _tcsicmp(pstrName, _T("style")) == 0) {
			LPCTSTR pStyle = m_pManager->GetStyle(pstrValue);
			if( pStyle != nullptr) {
				ApplyAttributeList(pStyle);
				return;
			}
		}
		// 属性
		if( _tcsicmp(pstrName, _T("innerstyle")) == 0 ) {
			ApplyAttributeList(pstrValue);
		}
		else if( _tcsicmp(pstrName, _T("pos")) == 0 ) { // [H5] C++17 helper: no read past NUL
			RECT rcPos = { 0 };
			if( !ParseRect4(pstrValue, rcPos) ) {
				// [SEC-10] see the note above: pass GetData(), not the CDuiString object.
				DUI_LOGW("CControlUI::SetAttribute bad pos='%s' (class=%s name=%s)", pstrValue, GetClass(), GetName().GetData());
			}
			else {
				SIZE szXY = {rcPos.left >= 0 ? rcPos.left : rcPos.right, rcPos.top >= 0 ? rcPos.top : rcPos.bottom};
				SetFixedXY(szXY);
				SetFixedWidth(abs(rcPos.right - rcPos.left));
				SetFixedHeight(abs(rcPos.bottom - rcPos.top));
			}
		}
		else if( _tcsicmp(pstrName, _T("float")) == 0 ) { // [H5] C++17 helper
			CDuiString nValue = pstrValue;
			if(nValue.Find(',') < 0) {
				SetFloat(_tcsicmp(pstrValue, _T("true")) == 0);
			}
			else {
				TPercentInfo piFloatPercent = { 0 };
				if( !ParseFloat4(pstrValue, piFloatPercent) ) {
					DUI_LOGW("CControlUI::SetAttribute bad float='%s'", pstrValue);
				}
				else {
					SetFloatPercent(piFloatPercent);
					SetFloat(true);
				}
			}
		}
		else if( _tcsicmp(pstrName, _T("floatalign")) == 0) {
			UINT uAlign = GetFloatAlign();
			// 解析文字属性
			while( *pstrValue != _T('\0') ) {
				CDuiString sValue;
				while( *pstrValue == _T(',') || *pstrValue == _T(' ') ) pstrValue = ::CharNext(pstrValue);

				while( *pstrValue != _T('\0') && *pstrValue != _T(',') && *pstrValue != _T(' ') ) {
					LPTSTR pstrTemp = ::CharNext(pstrValue);
					while( pstrValue < pstrTemp) {
						sValue += *pstrValue++;
					}
				}
				if(sValue.CompareNoCase(_T("null")) == 0) {
					uAlign = 0;
				}
				if( sValue.CompareNoCase(_T("left")) == 0 ) {
					uAlign &= ~(DT_CENTER | DT_RIGHT);
					uAlign |= DT_LEFT;
				}
				else if( sValue.CompareNoCase(_T("center")) == 0 ) {
					uAlign &= ~(DT_LEFT | DT_RIGHT);
					uAlign |= DT_CENTER;
				}
				else if( sValue.CompareNoCase(_T("right")) == 0 ) {
					uAlign &= ~(DT_LEFT | DT_CENTER);
					uAlign |= DT_RIGHT;
				}
				else if( sValue.CompareNoCase(_T("top")) == 0 ) {
					uAlign &= ~(DT_BOTTOM | DT_VCENTER);
					uAlign |= DT_TOP;
				}
				else if( sValue.CompareNoCase(_T("vcenter")) == 0 ) {
					uAlign &= ~(DT_TOP | DT_BOTTOM);
					uAlign |= DT_VCENTER;
				}
				else if( sValue.CompareNoCase(_T("bottom")) == 0 ) {
					uAlign &= ~(DT_TOP | DT_VCENTER);
					uAlign |= DT_BOTTOM;
				}
			}
			SetFloatAlign(uAlign);
		}
		else if( _tcsicmp(pstrName, _T("padding")) == 0 ) { // [H5] C++17 helper
			RECT rcPadding = { 0 };
			if( !ParseRect4(pstrValue, rcPadding) ) {
				DUI_LOGW("CControlUI::SetAttribute bad padding='%s'", pstrValue);
			}
			else SetPadding(rcPadding);
		}
		else if( _tcsicmp(pstrName, _T("gradient")) == 0 ) SetGradient(pstrValue);
		else if( _tcsicmp(pstrName, _T("bkcolor")) == 0 || _tcsicmp(pstrName, _T("bkcolor1")) == 0 ) {
			while( *pstrValue > _T('\0') && *pstrValue <= _T(' ') ) pstrValue = ::CharNext(pstrValue);
			if( *pstrValue == _T('#')) pstrValue = ::CharNext(pstrValue);
			LPTSTR pstr = nullptr;
			DWORD clrColor = _tcstoul(pstrValue, &pstr, 16);
			SetBkColor(clrColor);
		}
		else if( _tcsicmp(pstrName, _T("bkcolor2")) == 0 ) {
			while( *pstrValue > _T('\0') && *pstrValue <= _T(' ') ) pstrValue = ::CharNext(pstrValue);
			if( *pstrValue == _T('#')) pstrValue = ::CharNext(pstrValue);
			LPTSTR pstr = nullptr;
			DWORD clrColor = _tcstoul(pstrValue, &pstr, 16);
			SetBkColor2(clrColor);
		}
		else if( _tcsicmp(pstrName, _T("bkcolor3")) == 0 ) {
			while( *pstrValue > _T('\0') && *pstrValue <= _T(' ') ) pstrValue = ::CharNext(pstrValue);
			if( *pstrValue == _T('#')) pstrValue = ::CharNext(pstrValue);
			LPTSTR pstr = nullptr;
			DWORD clrColor = _tcstoul(pstrValue, &pstr, 16);
			SetBkColor3(clrColor);
		}
		else if( _tcsicmp(pstrName, _T("forecolor")) == 0 ) {
			while( *pstrValue > _T('\0') && *pstrValue <= _T(' ') ) pstrValue = ::CharNext(pstrValue);
			if( *pstrValue == _T('#')) pstrValue = ::CharNext(pstrValue);
			LPTSTR pstr = nullptr;
			DWORD clrColor = _tcstoul(pstrValue, &pstr, 16);
			SetForeColor(clrColor);
		}
		else if( _tcsicmp(pstrName, _T("bordercolor")) == 0 ) {
			if( *pstrValue == _T('#')) pstrValue = ::CharNext(pstrValue);
			LPTSTR pstr = nullptr;
			DWORD clrColor = _tcstoul(pstrValue, &pstr, 16);
			SetBorderColor(clrColor);
		}
		else if( _tcsicmp(pstrName, _T("focusbordercolor")) == 0 ) {
			if( *pstrValue == _T('#')) pstrValue = ::CharNext(pstrValue);
			LPTSTR pstr = nullptr;
			DWORD clrColor = _tcstoul(pstrValue, &pstr, 16);
			SetFocusBorderColor(clrColor);
		}
		else if( _tcsicmp(pstrName, _T("colorhsl")) == 0 ) SetColorHSL(_tcsicmp(pstrValue, _T("true")) == 0);
		else if( _tcsicmp(pstrName, _T("bordersize")) == 0 ) { // [H5] C++17 helper
			CDuiString nValue = pstrValue;
			if(nValue.Find(',') < 0) {
				SetBorderSize(_ttoi(pstrValue));
				RECT rcPadding = {0};
				SetBorderSize(rcPadding);
			}
			else {
				RECT rcPadding = { 0 };
				if( !ParseRect4(pstrValue, rcPadding) ) {
					DUI_LOGW("CControlUI::SetAttribute bad bordersize='%s'", pstrValue);
				}
				else SetBorderSize(rcPadding);
			}
		}
		else if( _tcsicmp(pstrName, _T("leftbordersize")) == 0 ) SetLeftBorderSize(_ttoi(pstrValue));
		else if( _tcsicmp(pstrName, _T("topbordersize")) == 0 ) SetTopBorderSize(_ttoi(pstrValue));
		else if( _tcsicmp(pstrName, _T("rightbordersize")) == 0 ) SetRightBorderSize(_ttoi(pstrValue));
		else if( _tcsicmp(pstrName, _T("bottombordersize")) == 0 ) SetBottomBorderSize(_ttoi(pstrValue));
		else if( _tcsicmp(pstrName, _T("borderstyle")) == 0 ) SetBorderStyle(_ttoi(pstrValue));
		else if( _tcsicmp(pstrName, _T("borderround")) == 0 ) { // [H5] C++17 helper
			SIZE cxyRound = { 0 };
			if( !ParseSize2(pstrValue, cxyRound) ) {
				DUI_LOGW("CControlUI::SetAttribute bad borderround='%s'", pstrValue);
			}
			else SetBorderRound(cxyRound);
		}
		else if( _tcsicmp(pstrName, _T("bkimage")) == 0 ) SetBkImage(pstrValue);
		else if( _tcsicmp(pstrName, _T("foreimage")) == 0 ) SetForeImage(pstrValue);
		else if( _tcsicmp(pstrName, _T("width")) == 0 ) SetFixedWidth(_ttoi(pstrValue));
		else if( _tcsicmp(pstrName, _T("height")) == 0 ) SetFixedHeight(_ttoi(pstrValue));
		else if( _tcsicmp(pstrName, _T("minwidth")) == 0 ) SetMinWidth(_ttoi(pstrValue));
		else if( _tcsicmp(pstrName, _T("minheight")) == 0 ) SetMinHeight(_ttoi(pstrValue));
		else if( _tcsicmp(pstrName, _T("maxwidth")) == 0 ) SetMaxWidth(_ttoi(pstrValue));
		else if( _tcsicmp(pstrName, _T("maxheight")) == 0 ) SetMaxHeight(_ttoi(pstrValue));
		else if( _tcsicmp(pstrName, _T("name")) == 0 ) SetName(pstrValue);
		else if( _tcsicmp(pstrName, _T("drag")) == 0 ) SetDragEnable(_tcsicmp(pstrValue, _T("true")) == 0);
		else if( _tcsicmp(pstrName, _T("drop")) == 0 ) SetDropEnable(_tcsicmp(pstrValue, _T("true")) == 0);
		else if( _tcsicmp(pstrName, _T("resourcetext")) == 0 ) SetResourceText(_tcsicmp(pstrValue, _T("true")) == 0);
		else if( _tcsicmp(pstrName, _T("richevent")) == 0 ) SetRichEvent(_tcsicmp(pstrValue, _T("true")) == 0);
		else if( _tcsicmp(pstrName, _T("text")) == 0 ) SetText(pstrValue);
		else if( _tcsicmp(pstrName, _T("tooltip")) == 0 ) SetToolTip(pstrValue);
		else if( _tcsicmp(pstrName, _T("userdata")) == 0 ) SetUserData(pstrValue);
		else if( _tcsicmp(pstrName, _T("enabled")) == 0 ) SetEnabled(_tcsicmp(pstrValue, _T("true")) == 0);
		else if( _tcsicmp(pstrName, _T("mouse")) == 0 ) SetMouseEnabled(_tcsicmp(pstrValue, _T("true")) == 0);
		else if( _tcsicmp(pstrName, _T("keyboard")) == 0 ) SetKeyboardEnabled(_tcsicmp(pstrValue, _T("true")) == 0);
		else if( _tcsicmp(pstrName, _T("visible")) == 0 ) SetVisible(_tcsicmp(pstrValue, _T("true")) == 0);
		else if( _tcsicmp(pstrName, _T("float")) == 0 ) SetFloat(_tcsicmp(pstrValue, _T("true")) == 0);
		else if( _tcsicmp(pstrName, _T("shortcut")) == 0 ) SetShortcut(pstrValue[0]);
		else if( _tcsicmp(pstrName, _T("menu")) == 0 ) SetContextMenuUsed(_tcsicmp(pstrValue, _T("true")) == 0);
		else if( _tcsicmp(pstrName, _T("cursor")) == 0 && pstrValue) {
			if( _tcsicmp(pstrValue, _T("arrow")) == 0 )			SetCursor(DUI_ARROW);
			else if( _tcsicmp(pstrValue, _T("ibeam")) == 0 )	SetCursor(DUI_IBEAM);
			else if( _tcsicmp(pstrValue, _T("wait")) == 0 )		SetCursor(DUI_WAIT);
			else if( _tcsicmp(pstrValue, _T("cross")) == 0 )	SetCursor(DUI_CROSS);
			else if( _tcsicmp(pstrValue, _T("uparrow")) == 0 )	SetCursor(DUI_UPARROW);
			else if( _tcsicmp(pstrValue, _T("size")) == 0 )		SetCursor(DUI_SIZE);
			else if( _tcsicmp(pstrValue, _T("icon")) == 0 )		SetCursor(DUI_ICON);
			else if( _tcsicmp(pstrValue, _T("sizenwse")) == 0 )	SetCursor(DUI_SIZENWSE);
			else if( _tcsicmp(pstrValue, _T("sizenesw")) == 0 )	SetCursor(DUI_SIZENESW);
			else if( _tcsicmp(pstrValue, _T("sizewe")) == 0 )	SetCursor(DUI_SIZEWE);
			else if( _tcsicmp(pstrValue, _T("sizens")) == 0 )	SetCursor(DUI_SIZENS);
			else if( _tcsicmp(pstrValue, _T("sizeall")) == 0 )	SetCursor(DUI_SIZEALL);
			else if( _tcsicmp(pstrValue, _T("no")) == 0 )		SetCursor(DUI_NO);
			else if( _tcsicmp(pstrValue, _T("hand")) == 0 )		SetCursor(DUI_HAND);
		}
		else if( _tcsicmp(pstrName, _T("virtualwnd")) == 0 ) SetVirtualWnd(pstrValue);
		else {
			AddCustomAttribute(pstrName, pstrValue);
		}
	}

	CControlUI* CControlUI::ApplyAttributeList(LPCTSTR pstrValue)
	{
		// 解析样式表
		if(m_pManager != nullptr) {
			LPCTSTR pStyle = m_pManager->GetStyle(pstrValue);
			if( pStyle != nullptr) {
				return ApplyAttributeList(pStyle);
			}
		}
		CDuiString sXmlData = pstrValue;
        sXmlData.Replace(_T("&quot;"), _T("\""));
        sXmlData.Replace(_T("\r"), _T(" "));
        sXmlData.Replace(_T("\n"), _T(" "));
        sXmlData.Replace(_T("\t"), _T(" "));
        sXmlData.Trim();

		LPCTSTR pstrList = sXmlData.GetData();
		// 解析样式属性
		CDuiString sItem;
		CDuiString sValue;
		while( *pstrList != _T('\0') ) {
			sItem.Empty();
			sValue.Empty();
            while (*pstrList != _T('\0')  && (*pstrList == _T(' ')) )
            {
				pstrList++;
            }
			while( *pstrList != _T('\0') && *pstrList != _T('=') ) {
				LPTSTR pstrTemp = ::CharNext(pstrList);
				while( pstrList < pstrTemp) {
					sItem += *pstrList++;
				}
			}			
			ASSERT( *pstrList == _T('=') );
			if( *pstrList++ != _T('=') ) return this;
			ASSERT( *pstrList == _T('\"') );
			if( *pstrList++ != _T('\"') ) return this;
			while( *pstrList != _T('\0') && *pstrList != _T('\"') ) {
				LPTSTR pstrTemp = ::CharNext(pstrList);
				while( pstrList < pstrTemp) {
					sValue += *pstrList++;
				}
			}
			ASSERT( *pstrList == _T('\"') );
			if( *pstrList++ != _T('\"') ) return this;
			SetAttribute(sItem, sValue);
			if( *pstrList != _T(' ') && *pstrList != _T(',') )
			{
				return this;
			}else
			{
                ++pstrList;
			}
		}
		return this;
	}

	SIZE CControlUI::EstimateSize(SIZE szAvailable)
	{
		if(m_pManager != nullptr)
			return m_pManager->GetDPIObj()->Scale(m_cxyFixed);
		return m_cxyFixed;
	}

	bool CControlUI::Paint(HDC hDC, const RECT& rcPaint, CControlUI* pStopControl)
	{
		// [SEC-30] Painting is meaningless without a paint manager: there is no
		// font, no DPI context and no paint DC to resolve resources against.
		// Every derived Paint/DoPaint implementation dereferences m_pManager
		// (GetPaintDC, GetFont, GetDefaultFontInfo...), and the ASan probe
		// tools/probes/paint_no_manager.cpp reports 31 crashes across 19 control
		// classes when Paint runs detached.
		// Guarding the single entry point covers all of them without touching any
		// control implementation.
		if (m_pManager == nullptr) return true;
		if (pStopControl == this) return false;
		if( !::IntersectRect(&m_rcPaint, &rcPaint, &m_rcItem) ) return true;
		if (!DoPaint(hDC, m_rcPaint, pStopControl)) return false;
		return true;
	}

	bool CControlUI::DoPaint(HDC hDC, const RECT& rcPaint, CControlUI* pStopControl)
	{
		// [SEC-30] see Paint(): DoPaint is virtual and can be invoked directly by
		// third-party code, so it needs the same guard as the Paint entry point.
		if (m_pManager == nullptr) return true;

		// 绘制循序：背景颜色->背景图->状态图->文本->边框
		SIZE cxyBorderRound = GetBorderRound();
		RECT rcBorderSize = GetBorderRectSize();

		if( cxyBorderRound.cx > 0 || cxyBorderRound.cy > 0 ) {
			CRenderClip roundClip;
			CRenderClip::GenerateRoundClip(hDC, m_rcPaint,  m_rcItem, cxyBorderRound.cx, cxyBorderRound.cy, roundClip);
			PaintBkColor(hDC);
			PaintBkImage(hDC);
			PaintStatusImage(hDC);
			PaintForeColor(hDC);
			PaintForeImage(hDC);
			PaintText(hDC);
			PaintBorder(hDC);
		}
		else {
			PaintBkColor(hDC);
			PaintBkImage(hDC);
			PaintStatusImage(hDC);
			PaintForeColor(hDC);
			PaintForeImage(hDC);
			PaintText(hDC);
			PaintBorder(hDC);
		}
		return true;
	}

	void CControlUI::PaintBkColor(HDC hDC)
	{
		if( m_dwBackColor != 0 ) {
			bool bVer = (m_sGradient.CompareNoCase(_T("hor")) != 0);
			if( m_dwBackColor2 != 0 ) {
				if( m_dwBackColor3 != 0 ) {
					RECT rc = m_rcItem;
					rc.bottom = (rc.bottom + rc.top) / 2;
					CRenderEngine::DrawGradient(hDC, rc, GetAdjustColor(m_dwBackColor), GetAdjustColor(m_dwBackColor2), bVer, 8);
					rc.top = rc.bottom;
					rc.bottom = m_rcItem.bottom;
					CRenderEngine::DrawGradient(hDC, rc, GetAdjustColor(m_dwBackColor2), GetAdjustColor(m_dwBackColor3), bVer, 8);
				}
				else {
					CRenderEngine::DrawGradient(hDC, m_rcItem, GetAdjustColor(m_dwBackColor), GetAdjustColor(m_dwBackColor2), bVer, 16);
				}
			}
			// [WIN11-ADAPT] 有 borderround 时改走圆角填充。
			// 原先纯色背景固定走 DrawColor（直角矩形），皮肤写 borderround 后
			// 只有 1px 描边是圆的、填充仍是直角，Win11 的圆角控件无法表达。
			else if( m_dwBackColor >= 0xFF000000 )
			{
				const int nRound = static_cast<int>(GetBorderRoundScaled().cx);
				if( nRound > 0 ) CRenderEngine::DrawRoundRectFilled(hDC, m_rcPaint, nRound, GetAdjustColor(m_dwBackColor));
				else CRenderEngine::DrawColor(hDC, m_rcPaint, GetAdjustColor(m_dwBackColor));
			}
			else
			{
				const int nRound = static_cast<int>(GetBorderRoundScaled().cx);
				if( nRound > 0 ) CRenderEngine::DrawRoundRectFilled(hDC, m_rcItem, nRound, GetAdjustColor(m_dwBackColor));
				else CRenderEngine::DrawColor(hDC, m_rcItem, GetAdjustColor(m_dwBackColor));
			}
		}
	}

	void CControlUI::PaintBkImage(HDC hDC)
	{
		if( m_sBkImage.IsEmpty() ) return;
		if( !DrawImage(hDC, (LPCTSTR)m_sBkImage) ) {}
	}

	void CControlUI::PaintStatusImage(HDC hDC)
	{
		return;
	}

	void CControlUI::PaintForeColor(HDC hDC)
	{
		CRenderEngine::DrawColor(hDC, m_rcItem, GetAdjustColor(m_dwForeColor));
	}
	
	void CControlUI::PaintForeImage(HDC hDC)
	{
		if( m_sForeImage.IsEmpty() ) return;
		DrawImage(hDC, (LPCTSTR)m_sForeImage);
	}

	void CControlUI::PaintText(HDC hDC)
	{
		return;
	}

	void CControlUI::PaintBorder(HDC hDC)
	{
		int nBorderSize = GetBorderSize();
		SIZE cxyBorderRound = GetBorderRound();
		RECT rcBorderSize = GetBorderRectSize();
		
		// [WIN11-ADAPT] 焦点态必须能画出描边，即使皮肤没设 bordersize。
		//
		// 缺陷：m_nBorderSize 默认是 0，而下面所有焦点分支都要求 nBorderSize > 0。
		// 于是"只设 focusbordercolor 而不设 bordersize"的皮肤（本项目所有按钮
		// 都是这样）在获得焦点时【一个像素都不画】—— Tab 切换看不出焦点在哪。
		// 这是"能力存在但触发条件是死的"，比完全不支持更难发现。
		//
		// 修法：仅在"有焦点且设了焦点色"时把绘制厚度兜底为 1，
		// 不影响非焦点态的正常边框逻辑。
		// [UI-5] A keyboard-focusable control must always show where focus is.
		// The colour used to default to 0, so a skin that never set
		// focusbordercolor produced a control that could be Tab-ed to but showed
		// no feedback at all. Fall back to the system highlight colour (which is
		// also the high-contrast colour) for tab-stop controls.
		DWORD dwFocusColor = m_dwFocusBorderColor;
		if (dwFocusColor == 0 && IsFocused() && (GetControlFlags() & UIFLAG_TABSTOP) != 0) {
			dwFocusColor = ::GetSysColor(COLOR_HIGHLIGHT);
			if (dwFocusColor == 0) dwFocusColor = 0xFF0078D7;
		}

		int nPaintBorderSize = nBorderSize;
		if (nPaintBorderSize <= 0 && IsFocused() && dwFocusColor != 0)
			nPaintBorderSize = 1;

		if(m_dwBorderColor != 0 || m_dwFocusBorderColor != 0 || (IsFocused() && dwFocusColor != 0)) {
			//画圆角边框
			if(nPaintBorderSize > 0 && ( cxyBorderRound.cx > 0 || cxyBorderRound.cy > 0 )) {
				if (IsFocused() && dwFocusColor != 0)
					CRenderEngine::DrawRoundRect(hDC, m_rcItem, nPaintBorderSize, cxyBorderRound.cx, cxyBorderRound.cy, GetAdjustColor(dwFocusColor), m_nBorderStyle);
				else
					CRenderEngine::DrawRoundRect(hDC, m_rcItem, nPaintBorderSize, cxyBorderRound.cx, cxyBorderRound.cy, GetAdjustColor(m_dwBorderColor), m_nBorderStyle);
			}
			else {
				if (IsFocused() && dwFocusColor != 0 && nPaintBorderSize > 0) { 
					CRenderEngine::DrawRect(hDC, m_rcItem, nPaintBorderSize, GetAdjustColor(dwFocusColor), m_nBorderStyle);
				}
				else if(rcBorderSize.left > 0 || rcBorderSize.top > 0 || rcBorderSize.right > 0 || rcBorderSize.bottom > 0) {
					RECT rcBorder;

					if(rcBorderSize.left > 0){
						rcBorder		= m_rcItem;
						rcBorder.right	= rcBorder.left;
						CRenderEngine::DrawLine(hDC,rcBorder,rcBorderSize.left,GetAdjustColor(m_dwBorderColor),m_nBorderStyle);
					}
					if(rcBorderSize.top > 0){
						rcBorder		= m_rcItem;
						rcBorder.bottom	= rcBorder.top;
						CRenderEngine::DrawLine(hDC,rcBorder,rcBorderSize.top,GetAdjustColor(m_dwBorderColor),m_nBorderStyle);
					}
					if(rcBorderSize.right > 0){
						rcBorder		= m_rcItem;
						rcBorder.right -= 1;
						rcBorder.left	= rcBorder.right;
						CRenderEngine::DrawLine(hDC,rcBorder,rcBorderSize.right,GetAdjustColor(m_dwBorderColor),m_nBorderStyle);
					}
					if(rcBorderSize.bottom > 0){
						rcBorder		= m_rcItem;
						rcBorder.bottom -= 1;
						rcBorder.top	= rcBorder.bottom;
						CRenderEngine::DrawLine(hDC,rcBorder,rcBorderSize.bottom,GetAdjustColor(m_dwBorderColor),m_nBorderStyle);
					}
				}
				else if(nBorderSize > 0) {
					CRenderEngine::DrawRect(hDC, m_rcItem, nPaintBorderSize, GetAdjustColor(m_dwBorderColor), m_nBorderStyle);
				}
			}
		}
	}

	void CControlUI::DoPostPaint(HDC hDC, const RECT& rcPaint)
	{
		return;
	}

	int CControlUI::GetLeftBorderSize() const
	{
		RECT rcBorderSize = m_rcBorderSize;
		if(m_pManager != nullptr) m_pManager->GetDPIObj()->Scale(&rcBorderSize);

		return rcBorderSize.left;
	}

	void CControlUI::SetLeftBorderSize( int nSize )
	{
		m_rcBorderSize.left = nSize;
		Invalidate();
	}

	int CControlUI::GetTopBorderSize() const
	{
		RECT rcBorderSize = m_rcBorderSize;
		if(m_pManager != nullptr) m_pManager->GetDPIObj()->Scale(&rcBorderSize);

		return rcBorderSize.top;
	}

	void CControlUI::SetTopBorderSize( int nSize )
	{
		m_rcBorderSize.top = nSize;
		Invalidate();
	}

	int CControlUI::GetRightBorderSize() const
	{
		RECT rcBorderSize = m_rcBorderSize;
		if(m_pManager != nullptr) m_pManager->GetDPIObj()->Scale(&rcBorderSize);

		return rcBorderSize.right;
	}

	void CControlUI::SetRightBorderSize( int nSize )
	{
		m_rcBorderSize.right = nSize;
		Invalidate();
	}

	int CControlUI::GetBottomBorderSize() const
	{
		RECT rcBorderSize = m_rcBorderSize;
		if(m_pManager != nullptr) m_pManager->GetDPIObj()->Scale(&rcBorderSize);

		return rcBorderSize.bottom;
	}

	void CControlUI::SetBottomBorderSize( int nSize )
	{
		m_rcBorderSize.bottom = nSize;
		Invalidate();
	}

	int CControlUI::GetBorderStyle() const
	{
		return m_nBorderStyle;
	}

	void CControlUI::SetBorderStyle( int nStyle )
	{
		m_nBorderStyle = nStyle;
		Invalidate();
	}

} // namespace DuiLib
