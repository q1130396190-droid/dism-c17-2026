#include "StdAfx.h"
#include "S3AttrRegistry.h"
#include "AttrParse.h"
#include "UISlider.h"


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



// [T5.36] W4 batch-4: suppress variable-hiding warnings (C4456/C4457/C4458).
// DuiLib conventionally uses local variables that shadow outer-scope / member
// names (e.g. m_rcTextPadding shadow, LPTSTR pstr = nullptr redeclarations in
// switch cases). All shadowing variables are properly initialized at declaration
// and used within their own scope. Not UB; documented in
// docs/t535-w4-b4-uninit-audit.json. Modernization path: rename to m_local_*
// prefix or restructure scopes.
#pragma warning(disable: 4456 4457 4458)
// [T5.36] end


namespace DuiLib
{
	IMPLEMENT_DUICONTROL(CSliderUI)
		CSliderUI::CSliderUI() : m_uButtonState(0), m_nStep(1),m_bSendMove(false)
	{
		m_uTextStyle = DT_SINGLELINE | DT_CENTER;
		m_szThumb.cx = m_szThumb.cy = 10;
	}

	LPCTSTR CSliderUI::GetClass() const
	{
		return _T("SliderUI");
	}

	UINT CSliderUI::GetControlFlags() const
	{
		if( IsEnabled() ) return UIFLAG_SETCURSOR;
		else return 0;
	}

	LPVOID CSliderUI::GetInterface(LPCTSTR pstrName)
	{
		if( _tcsicmp(pstrName, DUI_CTR_SLIDER) == 0 ) return static_cast<CSliderUI*>(this);
		return CProgressUI::GetInterface(pstrName);
	}

	void CSliderUI::SetEnabled(bool bEnable)
	{
		CControlUI::SetEnabled(bEnable);
		if( !IsEnabled() ) {
			m_uButtonState = 0;
		}
	}

	int CSliderUI::GetChangeStep()
	{
		return m_nStep;
	}

	void CSliderUI::SetChangeStep(int step)
	{
		m_nStep = step;
	}

	void CSliderUI::SetThumbSize(SIZE szXY)
	{
		m_szThumb = szXY;
	}

	RECT CSliderUI::GetThumbRect() const
	{
		RECT rcThumb = {0};
		SIZE m_szThumb = CSliderUI::m_szThumb;
		if (GetManager() != nullptr) {
			GetManager()->GetDPIObj()->Scale(&m_szThumb);
		}
		if( m_bHorizontal ) {
			int left = m_rcItem.left + (m_rcItem.right - m_rcItem.left - m_szThumb.cx) * (m_nValue - m_nMin) / (m_nMax - m_nMin);
			int top = (m_rcItem.bottom + m_rcItem.top - m_szThumb.cy) / 2;
			rcThumb = CDuiRect(left, top, left + m_szThumb.cx, top + m_szThumb.cy); 
		}
		else {
			int left = (m_rcItem.right + m_rcItem.left - m_szThumb.cx) / 2;
			int top = m_rcItem.bottom - m_szThumb.cy - (m_rcItem.bottom - m_rcItem.top - m_szThumb.cy) * (m_nValue - m_nMin) / (m_nMax - m_nMin);
			rcThumb = CDuiRect(left, top, left + m_szThumb.cx, top + m_szThumb.cy); 
		}
		if(m_pManager != nullptr) {
			//m_pManager->GetDPIObj()->Scale(&rcThumb);
		}
		return rcThumb;
	}

	LPCTSTR CSliderUI::GetThumbImage() const
	{
		return m_sThumbImage;
	}

	void CSliderUI::SetThumbImage(LPCTSTR pStrImage)
	{
		m_sThumbImage = pStrImage;
		Invalidate();
	}

	LPCTSTR CSliderUI::GetThumbHotImage() const
	{
		return m_sThumbHotImage;
	}

	void CSliderUI::SetThumbHotImage(LPCTSTR pStrImage)
	{
		m_sThumbHotImage = pStrImage;
		Invalidate();
	}

	LPCTSTR CSliderUI::GetThumbPushedImage() const
	{
		return m_sThumbPushedImage;
	}

	void CSliderUI::SetThumbPushedImage(LPCTSTR pStrImage)
	{
		m_sThumbPushedImage = pStrImage;
		Invalidate();
	}

	void CSliderUI::SetValue(int nValue)
	{
		if( (m_uButtonState & UISTATE_CAPTURED) != 0 ) return;
		CProgressUI::SetValue(nValue);
	}

	void CSliderUI::DoEvent(TEventUI& event)
	{
		if( !IsMouseEnabled() && event.Type > UIEVENT__MOUSEBEGIN && event.Type < UIEVENT__MOUSEEND ) {
			if( m_pParent != nullptr ) m_pParent->DoEvent(event);
			else CProgressUI::DoEvent(event);
			return;
		}

		if( event.Type == UIEVENT_BUTTONDOWN || event.Type == UIEVENT_DBLCLICK ) {
			if( IsEnabled() ) {
				m_uButtonState |= UISTATE_CAPTURED;

				int nValue = m_nMin;
				if( m_bHorizontal ) {
					if( event.ptMouse.x >= m_rcItem.right - m_szThumb.cx / 2 ) nValue = m_nMax;
					else if( event.ptMouse.x <= m_rcItem.left + m_szThumb.cx / 2 ) nValue = m_nMin;
					else nValue = m_nMin + 1.0f * (m_nMax - m_nMin) * (event.ptMouse.x - m_rcItem.left - m_szThumb.cx / 2 ) / (m_rcItem.right - m_rcItem.left - m_szThumb.cx);
				}
				else {
					if( event.ptMouse.y >= m_rcItem.bottom - m_szThumb.cy / 2 ) nValue = m_nMin;
					else if( event.ptMouse.y <= m_rcItem.top + m_szThumb.cy / 2  ) nValue = m_nMax;
					else nValue = m_nMin + 1.0f * (m_nMax - m_nMin) * (m_rcItem.bottom - event.ptMouse.y - m_szThumb.cy / 2 ) / (m_rcItem.bottom - m_rcItem.top - m_szThumb.cy);
				}
				if(m_nValue != nValue && nValue >= m_nMin && nValue <= m_nMax) {
					m_nValue = nValue;
					Invalidate();
				}
				UpdateText();
			}
			return;
		}

		if( event.Type == UIEVENT_BUTTONUP || event.Type == UIEVENT_RBUTTONUP) {
			if( IsEnabled() ) {
				int nValue = 0;
				if( (m_uButtonState & UISTATE_CAPTURED) != 0 ) {
					m_uButtonState &= ~UISTATE_CAPTURED;
				}
				if( m_bHorizontal ) {
					if( event.ptMouse.x >= m_rcItem.right - m_szThumb.cx / 2 ) nValue = m_nMax;
					else if( event.ptMouse.x <= m_rcItem.left + m_szThumb.cx / 2 ) nValue = m_nMin;
					else nValue = m_nMin + (m_nMax - m_nMin) * (event.ptMouse.x - m_rcItem.left - m_szThumb.cx / 2 ) / (m_rcItem.right - m_rcItem.left - m_szThumb.cx);
				}
				else {
					if( event.ptMouse.y >= m_rcItem.bottom - m_szThumb.cy / 2 ) nValue = m_nMin;
					else if( event.ptMouse.y <= m_rcItem.top + m_szThumb.cy / 2  ) nValue = m_nMax;
					else nValue = m_nMin + (m_nMax - m_nMin) * (m_rcItem.bottom - event.ptMouse.y - m_szThumb.cy / 2 ) / (m_rcItem.bottom - m_rcItem.top - m_szThumb.cy);
				}
				if(nValue >= m_nMin && nValue <= m_nMax) {
					m_nValue =nValue;
					if( m_pManager != nullptr ) m_pManager->SendNotify(this, DUI_MSGTYPE_VALUECHANGED);
					Invalidate();
				}
				UpdateText();
				return;
			}
		}
		if( event.Type == UIEVENT_CONTEXTMENU )
		{
			return;
		}
		if( event.Type == UIEVENT_SCROLLWHEEL ) 
		{
			if( IsEnabled() ) {
				switch( LOWORD(event.wParam) ) {
				case SB_LINEUP:
					SetValue(GetValue() + GetChangeStep());
					if( m_pManager != nullptr ) m_pManager->SendNotify(this, DUI_MSGTYPE_VALUECHANGED);
					return;
				case SB_LINEDOWN:
					SetValue(GetValue() - GetChangeStep());
					if( m_pManager != nullptr ) m_pManager->SendNotify(this, DUI_MSGTYPE_VALUECHANGED);
					return;
				}
			}
		}
		if( event.Type == UIEVENT_MOUSEMOVE ) {
			if( (m_uButtonState & UISTATE_CAPTURED) != 0 ) {
				if( m_bHorizontal ) {
					if( event.ptMouse.x >= m_rcItem.right - m_szThumb.cx / 2 ) m_nValue = m_nMax;
					else if( event.ptMouse.x <= m_rcItem.left + m_szThumb.cx / 2 ) m_nValue = m_nMin;
					else m_nValue = m_nMin + 1.0f * (m_nMax - m_nMin) * (event.ptMouse.x - m_rcItem.left - m_szThumb.cx / 2 ) / (m_rcItem.right - m_rcItem.left - m_szThumb.cx);
				}
				else {
					if( event.ptMouse.y >= m_rcItem.bottom - m_szThumb.cy / 2 ) m_nValue = m_nMin;
					else if( event.ptMouse.y <= m_rcItem.top + m_szThumb.cy / 2  ) m_nValue = m_nMax;
					else m_nValue = m_nMin + 1.0f * (m_nMax - m_nMin) * (m_rcItem.bottom - event.ptMouse.y - m_szThumb.cy / 2 ) / (m_rcItem.bottom - m_rcItem.top - m_szThumb.cy);
				}
				if (m_bSendMove) {
					UpdateText();
					if( m_pManager != nullptr ) m_pManager->SendNotify(this, DUI_MSGTYPE_VALUECHANGED_MOVE);
				}
				Invalidate();
			}

			POINT pt = event.ptMouse;
			RECT rcThumb = GetThumbRect();
			if( IsEnabled() && ::PtInRect(&rcThumb, event.ptMouse) ) {
				m_uButtonState |= UISTATE_HOT;
				Invalidate();
			}
			else {
				m_uButtonState &= ~UISTATE_HOT;
				Invalidate();
			}
			return;
		}
		if( event.Type == UIEVENT_SETCURSOR )
		{
			RECT rcThumb = GetThumbRect();
			if( IsEnabled()) {
				::SetCursor(::LoadCursor(nullptr, MAKEINTRESOURCE(IDC_HAND)));
				return;
			}
		}
		if( event.Type == UIEVENT_MOUSELEAVE )
		{
			if( IsEnabled() ) {
				m_uButtonState &= ~UISTATE_HOT;
				Invalidate();
			}
			return;
		}
		CControlUI::DoEvent(event);
	}

	void CSliderUI::SetCanSendMove(bool bCanSend)
	{
		m_bSendMove = bCanSend;
	}
	bool CSliderUI::GetCanSendMove() const
	{
		return m_bSendMove;
	}

	void CSliderUI::SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue)
	{		// ---- S3/T3.3a: fast path via compile-time hash registry ----
		// O(log n) lookup instead of O(n) string compares.
		// Falls through to the legacy chain below when unmatched, so
		// third-party custom attributes keep working unchanged.
		switch( DuiLib::s3::lookup_prop_t(pstrName) ) {
		case DuiLib::s3::PropId::thumbimage: { SetThumbImage(pstrValue); return; }
		case DuiLib::s3::PropId::thumbhotimage: { SetThumbHotImage(pstrValue); return; }
		case DuiLib::s3::PropId::thumbpushedimage: { SetThumbPushedImage(pstrValue); return; }
		case DuiLib::s3::PropId::thumbsize: { SIZE szXY = {0}; LPTSTR pstr = nullptr; if( !DuiLib::attr::ParseSize2(pstrValue, szXY) ) { DUI_LOGW("attr 'thumbsize' bad value='%s'", pstrValue); return; } SetThumbSize(szXY); return; }
		case DuiLib::s3::PropId::step: { SetChangeStep(_ttoi(pstrValue)); return; }
		case DuiLib::s3::PropId::sendmove: { SetCanSendMove(_tcsicmp(pstrValue, _T("true")) == 0); return; }
		default: break;
		}

		// ---- legacy fallback chain (kept verbatim for compatibility) ----

		if( _tcsicmp(pstrName, _T("thumbimage")) == 0 ) SetThumbImage(pstrValue);
		else if( _tcsicmp(pstrName, _T("thumbhotimage")) == 0 ) SetThumbHotImage(pstrValue);
		else if( _tcsicmp(pstrName, _T("thumbpushedimage")) == 0 ) SetThumbPushedImage(pstrValue);
		else if( _tcsicmp(pstrName, _T("thumbsize")) == 0 ) {
			SIZE szXY = { 0 };			if( !DuiLib::attr::ParseSize2(pstrValue, szXY) ) { DUI_LOGW("attr 'thumbsize' bad value='%s'", pstrValue); }			else { SetThumbSize(szXY); }
		}
		else if( _tcsicmp(pstrName, _T("step")) == 0 ) {
			SetChangeStep(_ttoi(pstrValue));
		}
		else if( _tcsicmp(pstrName, _T("sendmove")) == 0 ) {
			SetCanSendMove(_tcsicmp(pstrValue, _T("true")) == 0);
		}
		else CProgressUI::SetAttribute(pstrName, pstrValue);
	}

	void CSliderUI::PaintForeImage(HDC hDC)
	{
		CProgressUI::PaintForeImage(hDC);

		RECT rcThumb = GetThumbRect();
		rcThumb.left -= m_rcItem.left;
		rcThumb.top -= m_rcItem.top;
		rcThumb.right -= m_rcItem.left;
		rcThumb.bottom -= m_rcItem.top;

		GetManager()->GetDPIObj()->ScaleBack(&rcThumb);

		if( (m_uButtonState & UISTATE_CAPTURED) != 0 ) {
			if( !m_sThumbPushedImage.IsEmpty() ) {
				m_sImageModify.Empty();
				m_sImageModify.SmallFormat(_T("dest='%d,%d,%d,%d'"), rcThumb.left, rcThumb.top, rcThumb.right, rcThumb.bottom);
				if( !DrawImage(hDC, static_cast<LPCTSTR>(m_sThumbPushedImage), static_cast<LPCTSTR>(m_sImageModify)) ) {}
				else return;
			}
		}
		else if( (m_uButtonState & UISTATE_HOT) != 0 ) {
			if( !m_sThumbHotImage.IsEmpty() ) {
				m_sImageModify.Empty();
				m_sImageModify.SmallFormat(_T("dest='%d,%d,%d,%d'"), rcThumb.left, rcThumb.top, rcThumb.right, rcThumb.bottom);
				if( !DrawImage(hDC, static_cast<LPCTSTR>(m_sThumbHotImage), static_cast<LPCTSTR>(m_sImageModify)) ) {}
				else return;
			}
		}

		if( !m_sThumbImage.IsEmpty() ) {
			m_sImageModify.Empty();
			m_sImageModify.SmallFormat(_T("dest='%d,%d,%d,%d'"), rcThumb.left, rcThumb.top, rcThumb.right, rcThumb.bottom);
			if( !DrawImage(hDC, static_cast<LPCTSTR>(m_sThumbImage), static_cast<LPCTSTR>(m_sImageModify)) ) {}
			else return;
		}
	}
}
