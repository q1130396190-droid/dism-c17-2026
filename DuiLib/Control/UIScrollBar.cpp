#include "StdAfx.h"
#include "S3AttrRegistry.h"
#include "UIScrollBar.h"


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


namespace DuiLib
{
	IMPLEMENT_DUICONTROL(CScrollBarUI)

	CScrollBarUI::CScrollBarUI() : m_bHorizontal(false), m_nRange(0), m_nScrollPos(0), m_nLineSize(8), 
		m_pOwner(nullptr), m_nLastScrollPos(0), m_nLastScrollOffset(0), m_nScrollRepeatDelay(0), m_uButton1State(0), \
		m_uButton2State(0), m_uThumbState(0), m_bShowButton1(true), m_bShowButton2(true), m_bShow(true),
		m_dwThumbBkColor(0), m_dwThumbHotBkColor(0), m_dwThumbPushedBkColor(0), m_dwRailBkColor(0)
	{
		m_cxyFixed.cx = DEFAULT_SCROLLBAR_SIZE;
		m_ptLastMouse.x = m_ptLastMouse.y = 0;
		::ZeroMemory(&m_rcThumb, sizeof(m_rcThumb));
		::ZeroMemory(&m_rcButton1, sizeof(m_rcButton1));
		::ZeroMemory(&m_rcButton2, sizeof(m_rcButton2));
	}

	// [WIN11-ADAPT] ����/���촿ɫ��0 = δ���ã�����ԭ��Ӳ������Ϊ��
	void CScrollBarUI::SetThumbBkColor(DWORD dwColor) { if(m_dwThumbBkColor != dwColor){ m_dwThumbBkColor = dwColor; Invalidate(); } }
	DWORD CScrollBarUI::GetThumbBkColor() const { return m_dwThumbBkColor; }
	void CScrollBarUI::SetThumbHotBkColor(DWORD dwColor) { if(m_dwThumbHotBkColor != dwColor){ m_dwThumbHotBkColor = dwColor; Invalidate(); } }
	DWORD CScrollBarUI::GetThumbHotBkColor() const { return m_dwThumbHotBkColor; }
	void CScrollBarUI::SetThumbPushedBkColor(DWORD dwColor) { if(m_dwThumbPushedBkColor != dwColor){ m_dwThumbPushedBkColor = dwColor; Invalidate(); } }
	DWORD CScrollBarUI::GetThumbPushedBkColor() const { return m_dwThumbPushedBkColor; }
	void CScrollBarUI::SetRailBkColor(DWORD dwColor) { if(m_dwRailBkColor != dwColor){ m_dwRailBkColor = dwColor; Invalidate(); } }
	DWORD CScrollBarUI::GetRailBkColor() const { return m_dwRailBkColor; }

	// ���� #AARRGGBB / #RRGGBB���޷��������� 0��= δ���ã�
	static DWORD ParseColor(LPCTSTR pszValue)
	{
		if( pszValue == nullptr ) return 0;
		LPCTSTR p = pszValue;
		if( *p == _T('#') ) ++p;
		const int nLen = static_cast<int>(_tcslen(p));
		if( nLen != 6 && nLen != 8 ) return 0;
		LPTSTR pEnd = nullptr;
		const DWORD dw = static_cast<DWORD>(_tcstoul(p, &pEnd, 16));
		if( pEnd == nullptr || pEnd[0] != 0 ) return 0;
		return (nLen == 6) ? (0xFF000000u | dw) : dw;
	}

	LPCTSTR CScrollBarUI::GetClass() const
	{
		return _T("ScrollBarUI");
	}

	LPVOID CScrollBarUI::GetInterface(LPCTSTR pstrName)
	{
		if( _tcsicmp(pstrName, DUI_CTR_SCROLLBAR) == 0 ) return static_cast<CScrollBarUI*>(this);
		return CControlUI::GetInterface(pstrName);
	}

	CContainerUI* CScrollBarUI::GetOwner() const
	{
		return m_pOwner;
	}

	void CScrollBarUI::SetOwner(CContainerUI* pOwner)
	{
		m_pOwner = pOwner;
	}

	void CScrollBarUI::SetVisible(bool bVisible)
	{
		DUI_ASSERT_UI_THREAD_FOR(m_pManager);
		if( m_bVisible == bVisible ) return;

		bool v = IsVisible();
		m_bVisible = bVisible;
		if( m_bFocused ) m_bFocused = false;

	}

	void CScrollBarUI::SetEnabled(bool bEnable)
	{
		CControlUI::SetEnabled(bEnable);
		if( !IsEnabled() ) {
			m_uButton1State = 0;
			m_uButton2State = 0;
			m_uThumbState = 0;
		}
	}

	void CScrollBarUI::SetFocus()
	{
		if( m_pOwner != nullptr ) m_pOwner->SetFocus();
		else CControlUI::SetFocus();
	}

	bool CScrollBarUI::IsHorizontal()
	{
		return m_bHorizontal;
	}

	void CScrollBarUI::SetHorizontal(bool bHorizontal)
	{
		if( m_bHorizontal == bHorizontal ) return;

		m_bHorizontal = bHorizontal;
		if( m_bHorizontal ) {
			if( m_cxyFixed.cy == 0 ) {
				m_cxyFixed.cx = 0;
				m_cxyFixed.cy = DEFAULT_SCROLLBAR_SIZE;
			}
		}
		else {
			if( m_cxyFixed.cx == 0 ) {
				m_cxyFixed.cx = DEFAULT_SCROLLBAR_SIZE;
				m_cxyFixed.cy = 0;
			}
		}

		if( m_pOwner != nullptr ) m_pOwner->NeedUpdate(); else NeedParentUpdate();
	}

	int CScrollBarUI::GetScrollRange() const
	{
		return m_nRange;
	}

	void CScrollBarUI::SetScrollRange(int nRange)
	{
		if( m_nRange == nRange ) return;

		m_nRange = nRange;
		if( m_nRange < 0 ) m_nRange = 0;
		if( m_nScrollPos > m_nRange ) m_nScrollPos = m_nRange;
		SetPos(m_rcItem);
	}

	int CScrollBarUI::GetScrollPos() const
	{
		return m_nScrollPos;
	}

	void CScrollBarUI::SetScrollPos(int nPos)
	{
		if( m_nScrollPos == nPos ) return;

		m_nScrollPos = nPos;
		if( m_nScrollPos < 0 ) m_nScrollPos = 0;
		if( m_nScrollPos > m_nRange ) m_nScrollPos = m_nRange;
		SetPos(m_rcItem);
	}

	int CScrollBarUI::GetLineSize() const
	{
		return m_nLineSize;
	}

	void CScrollBarUI::SetLineSize(int nSize)
	{
		m_nLineSize = nSize;
	}

	bool CScrollBarUI::GetShowButton1()
	{
		return m_bShowButton1;
	}

	void CScrollBarUI::SetShowButton1(bool bShow)
	{
		m_bShowButton1 = bShow;
		SetPos(m_rcItem);
	}

	LPCTSTR CScrollBarUI::GetButton1NormalImage()
	{
		return m_sButton1NormalImage;
	}

	void CScrollBarUI::SetButton1NormalImage(LPCTSTR pStrImage)
	{
		m_sButton1NormalImage = pStrImage;
		Invalidate();
	}

	LPCTSTR CScrollBarUI::GetButton1HotImage()
	{
		return m_sButton1HotImage;
	}

	void CScrollBarUI::SetButton1HotImage(LPCTSTR pStrImage)
	{
		m_sButton1HotImage = pStrImage;
		Invalidate();
	}

	LPCTSTR CScrollBarUI::GetButton1PushedImage()
	{
		return m_sButton1PushedImage;
	}

	void CScrollBarUI::SetButton1PushedImage(LPCTSTR pStrImage)
	{
		m_sButton1PushedImage = pStrImage;
		Invalidate();
	}

	LPCTSTR CScrollBarUI::GetButton1DisabledImage()
	{
		return m_sButton1DisabledImage;
	}

	void CScrollBarUI::SetButton1DisabledImage(LPCTSTR pStrImage)
	{
		m_sButton1DisabledImage = pStrImage;
		Invalidate();
	}

	bool CScrollBarUI::GetShowButton2()
	{
		return m_bShowButton2;
	}

	void CScrollBarUI::SetShowButton2(bool bShow)
	{
		m_bShowButton2 = bShow;
		SetPos(m_rcItem);
	}

	LPCTSTR CScrollBarUI::GetButton2NormalImage()
	{
		return m_sButton2NormalImage;
	}

	void CScrollBarUI::SetButton2NormalImage(LPCTSTR pStrImage)
	{
		m_sButton2NormalImage = pStrImage;
		Invalidate();
	}

	LPCTSTR CScrollBarUI::GetButton2HotImage()
	{
		return m_sButton2HotImage;
	}

	void CScrollBarUI::SetButton2HotImage(LPCTSTR pStrImage)
	{
		m_sButton2HotImage = pStrImage;
		Invalidate();
	}

	LPCTSTR CScrollBarUI::GetButton2PushedImage()
	{
		return m_sButton2PushedImage;
	}

	void CScrollBarUI::SetButton2PushedImage(LPCTSTR pStrImage)
	{
		m_sButton2PushedImage = pStrImage;
		Invalidate();
	}

	LPCTSTR CScrollBarUI::GetButton2DisabledImage()
	{
		return m_sButton2DisabledImage;
	}

	void CScrollBarUI::SetButton2DisabledImage(LPCTSTR pStrImage)
	{
		m_sButton2DisabledImage = pStrImage;
		Invalidate();
	}

	LPCTSTR CScrollBarUI::GetThumbNormalImage()
	{
		return m_sThumbNormalImage;
	}

	void CScrollBarUI::SetThumbNormalImage(LPCTSTR pStrImage)
	{
		m_sThumbNormalImage = pStrImage;
		Invalidate();
	}

	LPCTSTR CScrollBarUI::GetThumbHotImage()
	{
		return m_sThumbHotImage;
	}

	void CScrollBarUI::SetThumbHotImage(LPCTSTR pStrImage)
	{
		m_sThumbHotImage = pStrImage;
		Invalidate();
	}

	LPCTSTR CScrollBarUI::GetThumbPushedImage()
	{
		return m_sThumbPushedImage;
	}

	void CScrollBarUI::SetThumbPushedImage(LPCTSTR pStrImage)
	{
		m_sThumbPushedImage = pStrImage;
		Invalidate();
	}

	LPCTSTR CScrollBarUI::GetThumbDisabledImage()
	{
		return m_sThumbDisabledImage;
	}

	void CScrollBarUI::SetThumbDisabledImage(LPCTSTR pStrImage)
	{
		m_sThumbDisabledImage = pStrImage;
		Invalidate();
	}

	LPCTSTR CScrollBarUI::GetRailNormalImage()
	{
		return m_sRailNormalImage;
	}

	void CScrollBarUI::SetRailNormalImage(LPCTSTR pStrImage)
	{
		m_sRailNormalImage = pStrImage;
		Invalidate();
	}

	LPCTSTR CScrollBarUI::GetRailHotImage()
	{
		return m_sRailHotImage;
	}

	void CScrollBarUI::SetRailHotImage(LPCTSTR pStrImage)
	{
		m_sRailHotImage = pStrImage;
		Invalidate();
	}

	LPCTSTR CScrollBarUI::GetRailPushedImage()
	{
		return m_sRailPushedImage;
	}

	void CScrollBarUI::SetRailPushedImage(LPCTSTR pStrImage)
	{
		m_sRailPushedImage = pStrImage;
		Invalidate();
	}

	LPCTSTR CScrollBarUI::GetRailDisabledImage()
	{
		return m_sRailDisabledImage;
	}

	void CScrollBarUI::SetRailDisabledImage(LPCTSTR pStrImage)
	{
		m_sRailDisabledImage = pStrImage;
		Invalidate();
	}

	LPCTSTR CScrollBarUI::GetBkNormalImage()
	{
		return m_sBkNormalImage;
	}

	void CScrollBarUI::SetBkNormalImage(LPCTSTR pStrImage)
	{
		m_sBkNormalImage = pStrImage;
		Invalidate();
	}

	LPCTSTR CScrollBarUI::GetBkHotImage()
	{
		return m_sBkHotImage;
	}

	void CScrollBarUI::SetBkHotImage(LPCTSTR pStrImage)
	{
		m_sBkHotImage = pStrImage;
		Invalidate();
	}

	LPCTSTR CScrollBarUI::GetBkPushedImage()
	{
		return m_sBkPushedImage;
	}

	void CScrollBarUI::SetBkPushedImage(LPCTSTR pStrImage)
	{
		m_sBkPushedImage = pStrImage;
		Invalidate();
	}

	LPCTSTR CScrollBarUI::GetBkDisabledImage()
	{
		return m_sBkDisabledImage;
	}

	void CScrollBarUI::SetBkDisabledImage(LPCTSTR pStrImage)
	{
		m_sBkDisabledImage = pStrImage;
		Invalidate();
	}

	bool CScrollBarUI::GetShow()
	{
		return m_bShow;
	}

	void CScrollBarUI::SetShow(bool bShow)
	{
		m_bShow = bShow;
		Invalidate();
	}

	void CScrollBarUI::SetPos(RECT rc, bool bNeedInvalidate)
	{
		CControlUI::SetPos(rc, bNeedInvalidate);
		SIZE cxyFixed = m_cxyFixed;
		if (m_pManager != nullptr) {
			GetManager()->GetDPIObj()->Scale(&cxyFixed);
		}
		rc = m_rcItem;
		if( m_bHorizontal ) {
			int cx = rc.right - rc.left;
			if( m_bShowButton1 ) cx -= cxyFixed.cy;
			if( m_bShowButton2 ) cx -= cxyFixed.cy;
			if( cx > cxyFixed.cy ) {
				m_rcButton1.left = rc.left;
				m_rcButton1.top = rc.top;
				if( m_bShowButton1 ) {
					m_rcButton1.right = rc.left + cxyFixed.cy;
					m_rcButton1.bottom = rc.top + cxyFixed.cy;
				}
				else {
					m_rcButton1.right = m_rcButton1.left;
					m_rcButton1.bottom = m_rcButton1.top;
				}

				m_rcButton2.top = rc.top;
				m_rcButton2.right = rc.right;
				if( m_bShowButton2 ) {
					m_rcButton2.left = rc.right - cxyFixed.cy;
					m_rcButton2.bottom = rc.top + cxyFixed.cy;
				}
				else {
					m_rcButton2.left = m_rcButton2.right;
					m_rcButton2.bottom = m_rcButton2.top;
				}

				m_rcThumb.top = rc.top;
				m_rcThumb.bottom = rc.top + cxyFixed.cy;
				if( m_nRange > 0 ) {
					int cxThumb = cx * (rc.right - rc.left) / (m_nRange + rc.right - rc.left);
					if( cxThumb < cxyFixed.cy ) cxThumb = cxyFixed.cy;

					m_rcThumb.left = m_nScrollPos * (cx - cxThumb) / m_nRange + m_rcButton1.right;
					m_rcThumb.right = m_rcThumb.left + cxThumb;
					if( m_rcThumb.right > m_rcButton2.left ) {
						m_rcThumb.left = m_rcButton2.left - cxThumb;
						m_rcThumb.right = m_rcButton2.left;
					}
				}
				else {
					m_rcThumb.left = m_rcButton1.right;
					m_rcThumb.right = m_rcButton2.left;
				}
			}
			else {
				int cxButton = (rc.right - rc.left) / 2;
				if( cxButton > cxyFixed.cy ) cxButton = cxyFixed.cy;
				m_rcButton1.left = rc.left;
				m_rcButton1.top = rc.top;
				if( m_bShowButton1 ) {
					m_rcButton1.right = rc.left + cxButton;
					m_rcButton1.bottom = rc.top + cxyFixed.cy;
				}
				else {
					m_rcButton1.right = m_rcButton1.left;
					m_rcButton1.bottom = m_rcButton1.top;
				}

				m_rcButton2.top = rc.top;
				m_rcButton2.right = rc.right;
				if( m_bShowButton2 ) {
					m_rcButton2.left = rc.right - cxButton;
					m_rcButton2.bottom = rc.top + cxyFixed.cy;
				}
				else {
					m_rcButton2.left = m_rcButton2.right;
					m_rcButton2.bottom = m_rcButton2.top;
				}

				::ZeroMemory(&m_rcThumb, sizeof(m_rcThumb));
			}
		}
		else {
			int cy = rc.bottom - rc.top;
			if( m_bShowButton1 ) cy -= cxyFixed.cx;
			if( m_bShowButton2 ) cy -= cxyFixed.cx;
			if( cy > cxyFixed.cx ) {
				m_rcButton1.left = rc.left;
				m_rcButton1.top = rc.top;
				if( m_bShowButton1 ) {
					m_rcButton1.right = rc.left + cxyFixed.cx;
					m_rcButton1.bottom = rc.top + cxyFixed.cx;
				}
				else {
					m_rcButton1.right = m_rcButton1.left;
					m_rcButton1.bottom = m_rcButton1.top;
				}

				m_rcButton2.left = rc.left;
				m_rcButton2.bottom = rc.bottom;
				if( m_bShowButton2 ) {
					m_rcButton2.top = rc.bottom - cxyFixed.cx;
					m_rcButton2.right = rc.left + cxyFixed.cx;
				}
				else {
					m_rcButton2.top = m_rcButton2.bottom;
					m_rcButton2.right = m_rcButton2.left;
				}

				m_rcThumb.left = rc.left;
				m_rcThumb.right = rc.left + cxyFixed.cx;
				if( m_nRange > 0 ) {
					int cyThumb = cy * (rc.bottom - rc.top) / (m_nRange + rc.bottom - rc.top);
					if( cyThumb < cxyFixed.cx ) cyThumb = cxyFixed.cx;

					m_rcThumb.top = (m_nScrollPos * 1.0f / m_nRange) * (cy - cyThumb) + m_rcButton1.bottom;
					m_rcThumb.bottom = m_rcThumb.top + cyThumb;
					if( m_rcThumb.bottom > m_rcButton2.top ) {
						m_rcThumb.top = m_rcButton2.top - cyThumb;
						m_rcThumb.bottom = m_rcButton2.top;
					}
				}
				else {
					m_rcThumb.top = m_rcButton1.bottom;
					m_rcThumb.bottom = m_rcButton2.top;
				}
			}
			else {
				int cyButton = (rc.bottom - rc.top) / 2;
				if( cyButton > cxyFixed.cx ) cyButton = cxyFixed.cx;
				m_rcButton1.left = rc.left;
				m_rcButton1.top = rc.top;
				if( m_bShowButton1 ) {
					m_rcButton1.right = rc.left + cxyFixed.cx;
					m_rcButton1.bottom = rc.top + cyButton;
				}
				else {
					m_rcButton1.right = m_rcButton1.left;
					m_rcButton1.bottom = m_rcButton1.top;
				}

				m_rcButton2.left = rc.left;
				m_rcButton2.bottom = rc.bottom;
				if( m_bShowButton2 ) {
					m_rcButton2.top = rc.bottom - cyButton;
					m_rcButton2.right = rc.left + cxyFixed.cx;
				}
				else {
					m_rcButton2.top = m_rcButton2.bottom;
					m_rcButton2.right = m_rcButton2.left;
				}

				::ZeroMemory(&m_rcThumb, sizeof(m_rcThumb));
			}
		}
	}

	void CScrollBarUI::DoEvent(TEventUI& event)
	{
		if( !IsMouseEnabled() && event.Type > UIEVENT__MOUSEBEGIN && event.Type < UIEVENT__MOUSEEND ) {
			if( m_pOwner != nullptr ) m_pOwner->DoEvent(event);
			else CControlUI::DoEvent(event);
			return;
		}

		if( event.Type == UIEVENT_SETFOCUS ) 
		{
			return;
		}
		if( event.Type == UIEVENT_KILLFOCUS ) 
		{
			return;
		}
		if( event.Type == UIEVENT_BUTTONDOWN || event.Type == UIEVENT_DBLCLICK )
		{
			if( !IsEnabled() ) return;

			m_nLastScrollOffset = 0;
			m_nScrollRepeatDelay = 0;

			if( ::PtInRect(&m_rcButton1, event.ptMouse) ) {
				m_uButton1State |= UISTATE_PUSHED;
				if( !m_bHorizontal ) {
					if( m_pOwner != nullptr ) m_pOwner->LineUp(); 
					else SetScrollPos(m_nScrollPos - m_nLineSize);
				}
				else {
					if( m_pOwner != nullptr ) m_pOwner->LineLeft(); 
					else SetScrollPos(m_nScrollPos - m_nLineSize);
				}
			}
			else if( ::PtInRect(&m_rcButton2, event.ptMouse) ) {
				m_uButton2State |= UISTATE_PUSHED;
				if( !m_bHorizontal ) {
					if( m_pOwner != nullptr ) m_pOwner->LineDown(); 
					else SetScrollPos(m_nScrollPos + m_nLineSize);
				}
				else {
					if( m_pOwner != nullptr ) m_pOwner->LineRight(); 
					else SetScrollPos(m_nScrollPos + m_nLineSize);
				}
			}
			else if( ::PtInRect(&m_rcThumb, event.ptMouse) ) {
				m_uThumbState |= UISTATE_CAPTURED | UISTATE_PUSHED;
				m_ptLastMouse = event.ptMouse;
				m_nLastScrollPos = m_nScrollPos;
				
				m_pManager->SetTimer(this, DEFAULT_TIMERID, 50U);
			}
			else {
				if( !m_bHorizontal ) {
					if( event.ptMouse.y < m_rcThumb.top ) {
						if( m_pOwner != nullptr ) m_pOwner->PageUp(); 
						else SetScrollPos(m_nScrollPos + m_rcItem.top - m_rcItem.bottom);
					}
					else if ( event.ptMouse.y > m_rcThumb.bottom ){
						if( m_pOwner != nullptr ) m_pOwner->PageDown(); 
						else SetScrollPos(m_nScrollPos - m_rcItem.top + m_rcItem.bottom);                    
					}
				}
				else {
					if( event.ptMouse.x < m_rcThumb.left ) {
						if( m_pOwner != nullptr ) m_pOwner->PageLeft(); 
						else SetScrollPos(m_nScrollPos + m_rcItem.left - m_rcItem.right);
					}
					else if ( event.ptMouse.x > m_rcThumb.right ){
						if( m_pOwner != nullptr ) m_pOwner->PageRight(); 
						else SetScrollPos(m_nScrollPos - m_rcItem.left + m_rcItem.right);                    
					}
				}
			}
			if( m_pManager != nullptr) m_pManager->SendNotify(this, DUI_MSGTYPE_SCROLL);
			return;
		}
		if( event.Type == UIEVENT_BUTTONUP )
		{
			m_nScrollRepeatDelay = 0;
			m_nLastScrollOffset = 0;
			// [SEC-40] KillTimer needs a live manager; a detached scrollbar can
			// still receive BUTTONUP (ASan: CRASH ScrollBar::BUTTONUP).
			if( m_pManager != nullptr ) m_pManager->KillTimer(this, DEFAULT_TIMERID);

			if( (m_uThumbState & UISTATE_CAPTURED) != 0 ) {
				m_uThumbState &= ~( UISTATE_CAPTURED | UISTATE_PUSHED );
				Invalidate();
			}
			else if( (m_uButton1State & UISTATE_PUSHED) != 0 ) {
				m_uButton1State &= ~UISTATE_PUSHED;
				Invalidate();
			}
			else if( (m_uButton2State & UISTATE_PUSHED) != 0 ) {
				m_uButton2State &= ~UISTATE_PUSHED;
				Invalidate();
			}
			return;
		}
		if( event.Type == UIEVENT_MOUSEMOVE )
		{
			if( (m_uThumbState & UISTATE_CAPTURED) != 0 ) {
				// [PERF-1] Apply the drag immediately. The old code only stored an
				// offset and waited for the 50 ms repeat timer, which capped thumb
				// feedback at ~20 FPS and made fast drags feel sticky.
				int nDelta = 0;
				if( !m_bHorizontal ) {
					__int64 fMouseRange = ((__int64)event.ptMouse.y - m_ptLastMouse.y) * m_nRange;
					int nBtnSize = 0;
					if(GetShowButton1()) nBtnSize += m_cxyFixed.cx;
					if(GetShowButton2()) nBtnSize += m_cxyFixed.cx;
					int vRange = m_rcItem.bottom - m_rcItem.top - (m_rcThumb.bottom - m_rcThumb.top) - nBtnSize;
					if (vRange != 0){
						nDelta = (int)(fMouseRange / abs(vRange));
					}
				}
				else {
					__int64 fMouseRange = ((__int64)event.ptMouse.x - m_ptLastMouse.x) * m_nRange;
					int nBtnSize = 0;
					if(GetShowButton1()) nBtnSize += m_cxyFixed.cy;
					if(GetShowButton2()) nBtnSize += m_cxyFixed.cy;
					int hRange = m_rcItem.right - m_rcItem.left - m_rcThumb.right + m_rcThumb.left - nBtnSize;
					if (hRange != 0) nDelta = (int)(fMouseRange / abs(hRange));
				}
				if( nDelta != 0 ) {
					m_nLastScrollOffset += nDelta;
					m_ptLastMouse = event.ptMouse;
					if( m_pOwner != nullptr ) {
						if( !m_bHorizontal )
							m_pOwner->SetScrollPos(CDuiSize(m_pOwner->GetScrollPos().cx, m_nLastScrollPos + m_nLastScrollOffset));
						else
							m_pOwner->SetScrollPos(CDuiSize(m_nLastScrollPos + m_nLastScrollOffset, m_pOwner->GetScrollPos().cy));
					}
					else {
						SetScrollPos(m_nLastScrollPos + m_nLastScrollOffset);
					}
				}
			}
			else {
				if( !IsEnabled() ) return;
				// [UI-2] Hover only the part under the cursor. The previous code lit
				// both arrow buttons whenever the pointer entered the scrollbar.
				const bool bOverBtn1 = ::PtInRect(&m_rcButton1, event.ptMouse) != FALSE;
				const bool bOverBtn2 = ::PtInRect(&m_rcButton2, event.ptMouse) != FALSE;
				const bool bOverThumb = ::PtInRect(&m_rcThumb, event.ptMouse) != FALSE;
				const UINT uOld1 = m_uButton1State & UISTATE_HOT;
				const UINT uOld2 = m_uButton2State & UISTATE_HOT;
				const UINT uOldThumb = m_uThumbState & UISTATE_HOT;
				if( bOverBtn1 ) m_uButton1State |= UISTATE_HOT; else m_uButton1State &= ~UISTATE_HOT;
				if( bOverBtn2 ) m_uButton2State |= UISTATE_HOT; else m_uButton2State &= ~UISTATE_HOT;
				if( bOverThumb ) m_uThumbState |= UISTATE_HOT; else m_uThumbState &= ~UISTATE_HOT;
				if( uOld1 != (m_uButton1State & UISTATE_HOT) ||
					uOld2 != (m_uButton2State & UISTATE_HOT) ||
					uOldThumb != (m_uThumbState & UISTATE_HOT) ) {
					Invalidate();
				}
			}
			return;
		}
		if( event.Type == UIEVENT_CONTEXTMENU )
		{
			return;
		}
		if( event.Type == UIEVENT_TIMER && event.wParam == DEFAULT_TIMERID )
		{
			++m_nScrollRepeatDelay;
			if( (m_uThumbState & UISTATE_CAPTURED) != 0 ) {
				// Thumb drag is applied synchronously in MOUSEMOVE; the timer is
				// only needed for arrow/track auto-repeat.
			}
			else if( (m_uButton1State & UISTATE_PUSHED) != 0 ) {
				if( m_nScrollRepeatDelay <= 5 ) return;
				if( !m_bHorizontal ) {
					if( m_pOwner != nullptr ) m_pOwner->LineUp(); 
					else SetScrollPos(m_nScrollPos - m_nLineSize);
				}
				else {
					if( m_pOwner != nullptr ) m_pOwner->LineLeft(); 
					else SetScrollPos(m_nScrollPos - m_nLineSize);
				}
			}
			else if( (m_uButton2State & UISTATE_PUSHED) != 0 ) {
				if( m_nScrollRepeatDelay <= 5 ) return;
				if( !m_bHorizontal ) {
					if( m_pOwner != nullptr ) m_pOwner->LineDown(); 
					else SetScrollPos(m_nScrollPos + m_nLineSize);
				}
				else {
					if( m_pOwner != nullptr ) m_pOwner->LineRight(); 
					else SetScrollPos(m_nScrollPos + m_nLineSize);
				}
			}
			else {
				if( m_nScrollRepeatDelay <= 5 ) return;
				POINT pt = { 0 };
				::GetCursorPos(&pt);
				::ScreenToClient(m_pManager->GetPaintWindow(), &pt);
				if( !m_bHorizontal ) {
					if( pt.y < m_rcThumb.top ) {
						if( m_pOwner != nullptr ) m_pOwner->PageUp(); 
						else SetScrollPos(m_nScrollPos + m_rcItem.top - m_rcItem.bottom);
					}
					else if ( pt.y > m_rcThumb.bottom ){
						if( m_pOwner != nullptr ) m_pOwner->PageDown(); 
						else SetScrollPos(m_nScrollPos - m_rcItem.top + m_rcItem.bottom);                    
					}
				}
				else {
					if( pt.x < m_rcThumb.left ) {
						if( m_pOwner != nullptr ) m_pOwner->PageLeft(); 
						else SetScrollPos(m_nScrollPos + m_rcItem.left - m_rcItem.right);
					}
					else if ( pt.x > m_rcThumb.right ){
						if( m_pOwner != nullptr ) m_pOwner->PageRight(); 
						else SetScrollPos(m_nScrollPos - m_rcItem.left + m_rcItem.right);                    
					}
				}
			}
			if( m_pManager != nullptr ) m_pManager->SendNotify(this, DUI_MSGTYPE_SCROLL);
			return;
		}
		if( event.Type == UIEVENT_MOUSEENTER )
		{
			if( IsEnabled() ) {
				// Let MOUSEMOVE assign hover per part; do not light both arrows.
				if( ::PtInRect(&m_rcButton1, event.ptMouse) ) m_uButton1State |= UISTATE_HOT;
				if( ::PtInRect(&m_rcButton2, event.ptMouse) ) m_uButton2State |= UISTATE_HOT;
				if( ::PtInRect(&m_rcThumb, event.ptMouse) ) m_uThumbState |= UISTATE_HOT;
				Invalidate();
			}
			return;
		}
		if( event.Type == UIEVENT_MOUSELEAVE )
		{
			if( IsEnabled() ) {
				m_uButton1State &= ~UISTATE_HOT;
				m_uButton2State &= ~UISTATE_HOT;
				m_uThumbState &= ~UISTATE_HOT;
				Invalidate();
			}
			return;
		}

		if( m_pOwner != nullptr ) m_pOwner->DoEvent(event); else CControlUI::DoEvent(event);
	}

	void CScrollBarUI::SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue)
	{		// ---- S3/T3.3a: fast path via compile-time hash registry ----
		// O(log n) lookup instead of O(n) string compares.
		// Falls through to the legacy chain below when unmatched, so
		// third-party custom attributes keep working unchanged.
		switch( DuiLib::s3::lookup_prop_t(pstrName) ) {
		case DuiLib::s3::PropId::button1normalimage: SetButton1NormalImage(pstrValue); return;
		case DuiLib::s3::PropId::button1hotimage: SetButton1HotImage(pstrValue); return;
		case DuiLib::s3::PropId::button1pushedimage: SetButton1PushedImage(pstrValue); return;
		case DuiLib::s3::PropId::button1disabledimage: SetButton1DisabledImage(pstrValue); return;
		case DuiLib::s3::PropId::button2normalimage: SetButton2NormalImage(pstrValue); return;
		case DuiLib::s3::PropId::button2hotimage: SetButton2HotImage(pstrValue); return;
		case DuiLib::s3::PropId::button2pushedimage: SetButton2PushedImage(pstrValue); return;
		case DuiLib::s3::PropId::button2disabledimage: SetButton2DisabledImage(pstrValue); return;
		case DuiLib::s3::PropId::thumbnormalimage: SetThumbNormalImage(pstrValue); return;
		case DuiLib::s3::PropId::thumbhotimage: SetThumbHotImage(pstrValue); return;
		case DuiLib::s3::PropId::thumbpushedimage: SetThumbPushedImage(pstrValue); return;
		case DuiLib::s3::PropId::thumbdisabledimage: SetThumbDisabledImage(pstrValue); return;
		case DuiLib::s3::PropId::railnormalimage: SetRailNormalImage(pstrValue); return;
		case DuiLib::s3::PropId::railhotimage: SetRailHotImage(pstrValue); return;
		case DuiLib::s3::PropId::railpushedimage: SetRailPushedImage(pstrValue); return;
		case DuiLib::s3::PropId::raildisabledimage: SetRailDisabledImage(pstrValue); return;
		case DuiLib::s3::PropId::bknormalimage: SetBkNormalImage(pstrValue); return;
		case DuiLib::s3::PropId::bkhotimage: SetBkHotImage(pstrValue); return;
		case DuiLib::s3::PropId::bkpushedimage: SetBkPushedImage(pstrValue); return;
		case DuiLib::s3::PropId::bkdisabledimage: SetBkDisabledImage(pstrValue); return;
		case DuiLib::s3::PropId::hor: SetHorizontal(_tcsicmp(pstrValue, _T("true")) == 0); return;
		case DuiLib::s3::PropId::linesize: SetLineSize(_ttoi(pstrValue)); return;
		case DuiLib::s3::PropId::range: SetScrollRange(_ttoi(pstrValue)); return;
		case DuiLib::s3::PropId::value: SetScrollPos(_ttoi(pstrValue)); return;
		case DuiLib::s3::PropId::showbutton1: SetShowButton1(_tcsicmp(pstrValue, _T("true")) == 0); return;
		case DuiLib::s3::PropId::showbutton2: SetShowButton2(_tcsicmp(pstrValue, _T("true")) == 0); return;
		default: break;
		}

		// ---- legacy fallback chain (kept verbatim for compatibility) ----

		if( _tcsicmp(pstrName, _T("button1normalimage")) == 0 ) SetButton1NormalImage(pstrValue);
		else if( _tcsicmp(pstrName, _T("button1hotimage")) == 0 ) SetButton1HotImage(pstrValue);
		else if( _tcsicmp(pstrName, _T("button1pushedimage")) == 0 ) SetButton1PushedImage(pstrValue);
		else if( _tcsicmp(pstrName, _T("button1disabledimage")) == 0 ) SetButton1DisabledImage(pstrValue);
		else if( _tcsicmp(pstrName, _T("button2normalimage")) == 0 ) SetButton2NormalImage(pstrValue);
		else if( _tcsicmp(pstrName, _T("button2hotimage")) == 0 ) SetButton2HotImage(pstrValue);
		else if( _tcsicmp(pstrName, _T("button2pushedimage")) == 0 ) SetButton2PushedImage(pstrValue);
		else if( _tcsicmp(pstrName, _T("button2disabledimage")) == 0 ) SetButton2DisabledImage(pstrValue);
		else if( _tcsicmp(pstrName, _T("thumbnormalimage")) == 0 ) SetThumbNormalImage(pstrValue);
		else if( _tcsicmp(pstrName, _T("thumbhotimage")) == 0 ) SetThumbHotImage(pstrValue);
		else if( _tcsicmp(pstrName, _T("thumbpushedimage")) == 0 ) SetThumbPushedImage(pstrValue);
		else if( _tcsicmp(pstrName, _T("thumbdisabledimage")) == 0 ) SetThumbDisabledImage(pstrValue);
		else if( _tcsicmp(pstrName, _T("railnormalimage")) == 0 ) SetRailNormalImage(pstrValue);
		else if( _tcsicmp(pstrName, _T("railhotimage")) == 0 ) SetRailHotImage(pstrValue);
		else if( _tcsicmp(pstrName, _T("railpushedimage")) == 0 ) SetRailPushedImage(pstrValue);
		else if( _tcsicmp(pstrName, _T("raildisabledimage")) == 0 ) SetRailDisabledImage(pstrValue);
		else if( _tcsicmp(pstrName, _T("bknormalimage")) == 0 ) SetBkNormalImage(pstrValue);
		else if( _tcsicmp(pstrName, _T("bkhotimage")) == 0 ) SetBkHotImage(pstrValue);
		else if( _tcsicmp(pstrName, _T("bkpushedimage")) == 0 ) SetBkPushedImage(pstrValue);
		else if( _tcsicmp(pstrName, _T("bkdisabledimage")) == 0 ) SetBkDisabledImage(pstrValue);
		else if( _tcsicmp(pstrName, _T("hor")) == 0 ) SetHorizontal(_tcsicmp(pstrValue, _T("true")) == 0);
		else if( _tcsicmp(pstrName, _T("linesize")) == 0 ) SetLineSize(_ttoi(pstrValue));
		else if( _tcsicmp(pstrName, _T("range")) == 0 ) SetScrollRange(_ttoi(pstrValue));
		else if( _tcsicmp(pstrName, _T("value")) == 0 ) SetScrollPos(_ttoi(pstrValue));
		else if( _tcsicmp(pstrName, _T("showbutton1")) == 0 ) SetShowButton1(_tcsicmp(pstrValue, _T("true")) == 0);
		else if( _tcsicmp(pstrName, _T("showbutton2")) == 0 ) SetShowButton2(_tcsicmp(pstrValue, _T("true")) == 0);
		// [WIN11-ADAPT] Ƥ���ﳤ�ڴ��ڵ���δ����������ɫ����
		else if( _tcsicmp(pstrName, _T("thumbbkcolor")) == 0 ) SetThumbBkColor(ParseColor(pstrValue));
		else if( _tcsicmp(pstrName, _T("thumbhotbkcolor")) == 0 ) SetThumbHotBkColor(ParseColor(pstrValue));
		else if( _tcsicmp(pstrName, _T("thumbpushedbkcolor")) == 0 ) SetThumbPushedBkColor(ParseColor(pstrValue));
		else if( _tcsicmp(pstrName, _T("railbkcolor")) == 0 ) SetRailBkColor(ParseColor(pstrValue));
		else CControlUI::SetAttribute(pstrName, pstrValue);
	}

	bool CScrollBarUI::DoPaint(HDC hDC, const RECT& rcPaint, CControlUI* pStopControl)
	{
		// [SEC-31] This override bypasses CControlUI::DoPaint's guard, so it
		// needs its own: a detached control has no font/DC/DPI context.
		if (m_pManager == nullptr) return true;
		if(!GetShow()) return true;

		PaintBkColor(hDC);
		PaintBkImage(hDC);
		PaintBk(hDC);
		PaintButton1(hDC);
		PaintButton2(hDC);
		PaintThumb(hDC);
		PaintRail(hDC);
		PaintBorder(hDC);
		return true;
	}

	void CScrollBarUI::PaintBk(HDC hDC)
	{
		if( !IsEnabled() ) m_uThumbState |= UISTATE_DISABLED;
		else m_uThumbState &= ~ UISTATE_DISABLED;

		if( (m_uThumbState & UISTATE_DISABLED) != 0 ) {
			if( !m_sBkDisabledImage.IsEmpty() ) {
				if( !DrawImage(hDC, static_cast<LPCTSTR>(m_sBkDisabledImage)) ) {}
				else return;
			}
		}
		else if( (m_uThumbState & UISTATE_PUSHED) != 0 ) {
			if( !m_sBkPushedImage.IsEmpty() ) {
				if( !DrawImage(hDC, static_cast<LPCTSTR>(m_sBkPushedImage)) ) {}
				else return;
			}
		}
		else if( (m_uThumbState & UISTATE_HOT) != 0 ) {
			if( !m_sBkHotImage.IsEmpty() ) {
				if( !DrawImage(hDC, static_cast<LPCTSTR>(m_sBkHotImage)) ) {}
				else return;
			}
		}

		if( !m_sBkNormalImage.IsEmpty() ) {
			if( !DrawImage(hDC, static_cast<LPCTSTR>(m_sBkNormalImage)) ) {}
			else return;
		}
	}

	void CScrollBarUI::PaintButton1(HDC hDC)
	{
		if( !m_bShowButton1 ) return;

		if( !IsEnabled() ) m_uButton1State |= UISTATE_DISABLED;
		else m_uButton1State &= ~ UISTATE_DISABLED;

		int d1 = MulDiv(m_rcButton1.left - m_rcItem.left, 100, GetManager()->GetDPIObj()->GetScale());
		int d2 = MulDiv(m_rcButton1.top - m_rcItem.top, 100, GetManager()->GetDPIObj()->GetScale());
		int d3 = MulDiv(m_rcButton1.right - m_rcItem.left, 100, GetManager()->GetDPIObj()->GetScale());
		int d4 = MulDiv(m_rcButton1.bottom - m_rcItem.top, 100, GetManager()->GetDPIObj()->GetScale());
		m_sImageModify.Empty();
		m_sImageModify.SmallFormat(_T("dest='%d,%d,%d,%d'"), d1, d2, d3, d4);

		if( (m_uButton1State & UISTATE_DISABLED) != 0 ) {
			if( !m_sButton1DisabledImage.IsEmpty() ) {
				if( !DrawImage(hDC, static_cast<LPCTSTR>(m_sButton1DisabledImage), static_cast<LPCTSTR>(m_sImageModify)) ) {}
				else return;
			}
		}
		else if( (m_uButton1State & UISTATE_PUSHED) != 0 ) {
			if( !m_sButton1PushedImage.IsEmpty() ) {
				if( !DrawImage(hDC, static_cast<LPCTSTR>(m_sButton1PushedImage), static_cast<LPCTSTR>(m_sImageModify)) ) {}
				else return;
			}
		}
		else if( (m_uButton1State & UISTATE_HOT) != 0 ) {
			if( !m_sButton1HotImage.IsEmpty() ) {
				if( !DrawImage(hDC, static_cast<LPCTSTR>(m_sButton1HotImage), static_cast<LPCTSTR>(m_sImageModify)) ) {}
				else return;
			}
		}

		if( !m_sButton1NormalImage.IsEmpty() ) {
			if( !DrawImage(hDC, static_cast<LPCTSTR>(m_sButton1NormalImage), static_cast<LPCTSTR>(m_sImageModify)) ) {}
			else return;
		}

		DWORD dwBorderColor = 0xFF85E4FF;
		int nBorderSize = 2;
		CRenderEngine::DrawRect(hDC, m_rcButton1, nBorderSize, dwBorderColor);
	}

	void CScrollBarUI::PaintButton2(HDC hDC)
	{
		if( !m_bShowButton2 ) return;

		if( !IsEnabled() ) m_uButton2State |= UISTATE_DISABLED;
		else m_uButton2State &= ~ UISTATE_DISABLED;
		int d1 = MulDiv(m_rcButton2.left - m_rcItem.left, 100, GetManager()->GetDPIObj()->GetScale());
		int d2 = MulDiv(m_rcButton2.top - m_rcItem.top, 100, GetManager()->GetDPIObj()->GetScale());
		int d3 = MulDiv(m_rcButton2.right - m_rcItem.left, 100, GetManager()->GetDPIObj()->GetScale());
		int d4 = MulDiv(m_rcButton2.bottom - m_rcItem.top, 100, GetManager()->GetDPIObj()->GetScale());
		m_sImageModify.Empty();
		m_sImageModify.SmallFormat(_T("dest='%d,%d,%d,%d'"),d1 ,d2 ,d3 ,d4 );

		if( (m_uButton2State & UISTATE_DISABLED) != 0 ) {
			if( !m_sButton2DisabledImage.IsEmpty() ) {
				if( !DrawImage(hDC, static_cast<LPCTSTR>(m_sButton2DisabledImage), static_cast<LPCTSTR>(m_sImageModify)) ) {}
				else return;
			}
		}
		else if( (m_uButton2State & UISTATE_PUSHED) != 0 ) {
			if( !m_sButton2PushedImage.IsEmpty() ) {
				if( !DrawImage(hDC, static_cast<LPCTSTR>(m_sButton2PushedImage), static_cast<LPCTSTR>(m_sImageModify)) ) {}
				else return;
			}
		}
		else if( (m_uButton2State & UISTATE_HOT) != 0 ) {
			if( !m_sButton2HotImage.IsEmpty() ) {
				if( !DrawImage(hDC, static_cast<LPCTSTR>(m_sButton2HotImage), static_cast<LPCTSTR>(m_sImageModify)) ) {}
				else return;
			}
		}

		if( !m_sButton2NormalImage.IsEmpty() ) {
			if( !DrawImage(hDC, static_cast<LPCTSTR>(m_sButton2NormalImage), static_cast<LPCTSTR>(m_sImageModify)) ) {}
			else return;
		}

		DWORD dwBorderColor = 0xFF85E4FF;
		int nBorderSize = 2;
		CRenderEngine::DrawRect(hDC, m_rcButton2, nBorderSize, dwBorderColor);
	}

	void CScrollBarUI::PaintThumb(HDC hDC)
	{
		if( m_rcThumb.left == 0 && m_rcThumb.top == 0 && m_rcThumb.right == 0 && m_rcThumb.bottom == 0 ) return;
		if( !IsEnabled() ) m_uThumbState |= UISTATE_DISABLED;
		else m_uThumbState &= ~ UISTATE_DISABLED;
		int d1 = MulDiv(m_rcThumb.left - m_rcItem.left, 100, GetManager()->GetDPIObj()->GetScale());
		int d2 = MulDiv(m_rcThumb.top - m_rcItem.top, 100, GetManager()->GetDPIObj()->GetScale());
		int d3 = MulDiv(m_rcThumb.right - m_rcItem.left, 100, GetManager()->GetDPIObj()->GetScale());
		int d4 = MulDiv(m_rcThumb.bottom - m_rcItem.top, 100, GetManager()->GetDPIObj()->GetScale());
		m_sImageModify.Empty();
		m_sImageModify.SmallFormat(_T("dest='%d,%d,%d,%d'"), d1, d2, d3, d4);

		if( (m_uThumbState & UISTATE_DISABLED) != 0 ) {
			if( !m_sThumbDisabledImage.IsEmpty() ) {
				if( !DrawImage(hDC, static_cast<LPCTSTR>(m_sThumbDisabledImage), static_cast<LPCTSTR>(m_sImageModify)) ) {}
				else return;
			}
		}
		else if( (m_uThumbState & UISTATE_PUSHED) != 0 ) {
			if( !m_sThumbPushedImage.IsEmpty() ) {
				if( !DrawImage(hDC, static_cast<LPCTSTR>(m_sThumbPushedImage), static_cast<LPCTSTR>(m_sImageModify)) ) {}
				else return;
			}
		}
		else if( (m_uThumbState & UISTATE_HOT) != 0 ) {
			if( !m_sThumbHotImage.IsEmpty() ) {
				if( !DrawImage(hDC, static_cast<LPCTSTR>(m_sThumbHotImage), static_cast<LPCTSTR>(m_sImageModify)) ) {}
				else return;
			}
		}

		if( !m_sThumbNormalImage.IsEmpty() ) {
			if( !DrawImage(hDC, static_cast<LPCTSTR>(m_sThumbNormalImage), static_cast<LPCTSTR>(m_sImageModify)) ) {}
			else return;
		}

		// [WIN11-ADAPT] Ƥ���ṩ�˻��鴿ɫʱ��״̬��䣻
		// δ�ṩ��0���򱣳�ԭ��Ϊ����Ӱ�����Ƥ����
		DWORD dwThumbFill = 0;
		if( (m_uThumbState & UISTATE_PUSHED) != 0 && m_dwThumbPushedBkColor != 0 )
			dwThumbFill = m_dwThumbPushedBkColor;
		else if( (m_uThumbState & UISTATE_HOT) != 0 && m_dwThumbHotBkColor != 0 )
			dwThumbFill = m_dwThumbHotBkColor;
		else
			dwThumbFill = m_dwThumbBkColor;

		if( dwThumbFill != 0 ) {
			CRenderEngine::DrawColor(hDC, m_rcThumb, GetAdjustColor(dwThumbFill));
			return;
		}

		// ���ף�����ԭ��������ߣ�����ɫ��������ͼƬʱ��
		DWORD dwBorderColor = 0xFF85E4FF;
		int nBorderSize = 2;
		CRenderEngine::DrawRect(hDC, m_rcThumb, nBorderSize, dwBorderColor);
	}

	void CScrollBarUI::PaintRail(HDC hDC)
	{
		if( m_rcThumb.left == 0 && m_rcThumb.top == 0 && m_rcThumb.right == 0 && m_rcThumb.bottom == 0 ) return;
		if( !IsEnabled() ) m_uThumbState |= UISTATE_DISABLED;
		else m_uThumbState &= ~ UISTATE_DISABLED;

		m_sImageModify.Empty();
		if( !m_bHorizontal ) {
			int d1 = MulDiv(m_rcThumb.left - m_rcItem.left, 100, GetManager()->GetDPIObj()->GetScale());
			int d2 = MulDiv((m_rcThumb.top + m_rcThumb.bottom) / 2 - m_rcItem.top - m_cxyFixed.cx / 2, 100, GetManager()->GetDPIObj()->GetScale());
			int d3 = MulDiv(m_rcThumb.right - m_rcItem.left, 100, GetManager()->GetDPIObj()->GetScale());
			int d4 = MulDiv((m_rcThumb.top + m_rcThumb.bottom) / 2 - m_rcItem.top + m_cxyFixed.cx - m_cxyFixed.cx / 2, 100, GetManager()->GetDPIObj()->GetScale());
			m_sImageModify.SmallFormat(_T("dest='%d,%d,%d,%d'"), d1, d2, d3,d4);
		}
		else {
			int d1 = MulDiv((m_rcThumb.left + m_rcThumb.right) / 2 - m_rcItem.left - m_cxyFixed.cy / 2, 100, GetManager()->GetDPIObj()->GetScale());
			int d2 = MulDiv(m_rcThumb.top - m_rcItem.top, 100, GetManager()->GetDPIObj()->GetScale());
			int d3 = MulDiv((m_rcThumb.left + m_rcThumb.right) / 2 - m_rcItem.left + m_cxyFixed.cy - m_cxyFixed.cy / 2, 100, GetManager()->GetDPIObj()->GetScale());
			int d4 = MulDiv(m_rcThumb.bottom - m_rcItem.top, 100, GetManager()->GetDPIObj()->GetScale());
			m_sImageModify.SmallFormat(_T("dest='%d,%d,%d,%d'"), d1,d2, d3, d4);
		}

		if( (m_uThumbState & UISTATE_DISABLED) != 0 ) {
			if( !m_sRailDisabledImage.IsEmpty() ) {
				if( !DrawImage(hDC, static_cast<LPCTSTR>(m_sRailDisabledImage), static_cast<LPCTSTR>(m_sImageModify)) ) {}
				else return;
			}
		}
		else if( (m_uThumbState & UISTATE_PUSHED) != 0 ) {
			if( !m_sRailPushedImage.IsEmpty() ) {
				if( !DrawImage(hDC, static_cast<LPCTSTR>(m_sRailPushedImage), static_cast<LPCTSTR>(m_sImageModify)) ) {}
				else return;
			}
		}
		else if( (m_uThumbState & UISTATE_HOT) != 0 ) {
			if( !m_sRailHotImage.IsEmpty() ) {
				if( !DrawImage(hDC, static_cast<LPCTSTR>(m_sRailHotImage), static_cast<LPCTSTR>(m_sImageModify)) ) {}
				else return;
			}
		}

		if( !m_sRailNormalImage.IsEmpty() ) {
			if( !DrawImage(hDC, static_cast<LPCTSTR>(m_sRailNormalImage), static_cast<LPCTSTR>(m_sImageModify)) ) {}
			else return;
		}

		// [WIN11-ADAPT] ���촿ɫ��Ƥ���ṩʱ��Ч�����򱣳�͸��ԭ����
		if( m_dwRailBkColor != 0 ) {
			CRenderEngine::DrawColor(hDC, m_rcItem, GetAdjustColor(m_dwRailBkColor));
		}
	}
}
