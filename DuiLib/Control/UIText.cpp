#include "StdAfx.h"
#include "UIText.h"


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
	IMPLEMENT_DUICONTROL(CTextUI)

	CTextUI::CTextUI() : m_nLinks(0), m_nHoverLink(-1)
	{
		m_uTextStyle = DT_WORDBREAK;
		m_rcTextPadding.left = 2;
		m_rcTextPadding.right = 2;
		::ZeroMemory(m_rcLinks, sizeof(m_rcLinks));
	}

	CTextUI::~CTextUI()
	{
	}

	LPCTSTR CTextUI::GetClass() const
	{
		return _T("TextUI");
	}

	LPVOID CTextUI::GetInterface(LPCTSTR pstrName)
	{
		if( _tcsicmp(pstrName, DUI_CTR_TEXT) == 0 ) return static_cast<CTextUI*>(this);
		return CLabelUI::GetInterface(pstrName);
	}

	UINT CTextUI::GetControlFlags() const
	{
		if( IsEnabled() && m_nLinks > 0 ) return UIFLAG_SETCURSOR;
		else return 0;
	}

	CDuiString* CTextUI::GetLinkContent(int iIndex)
	{
		if( iIndex >= 0 && iIndex < m_nLinks ) return &m_sLinks[iIndex];
		return nullptr;
	}

	void CTextUI::DoEvent(TEventUI& event)
	{
		if( !IsMouseEnabled() && event.Type > UIEVENT__MOUSEBEGIN && event.Type < UIEVENT__MOUSEEND ) {
			if( m_pParent != nullptr ) m_pParent->DoEvent(event);
			else CLabelUI::DoEvent(event);
			return;
		}

		if( event.Type == UIEVENT_SETCURSOR ) {
			for( int i = 0; i < m_nLinks; i++ ) {
				if( ::PtInRect(&m_rcLinks[i], event.ptMouse) ) {
					::SetCursor(::LoadCursor(nullptr, MAKEINTRESOURCE(IDC_HAND)));
					return;
				}
			}
		}
		if( event.Type == UIEVENT_BUTTONDOWN || event.Type == UIEVENT_DBLCLICK && IsEnabled() ) {
			for( int i = 0; i < m_nLinks; i++ ) {
				if( ::PtInRect(&m_rcLinks[i], event.ptMouse) ) {
					Invalidate();
					return;
				}
			}
		}
		if( event.Type == UIEVENT_BUTTONUP && IsEnabled() ) {
			for( int i = 0; i < m_nLinks; i++ ) {
				if( ::PtInRect(&m_rcLinks[i], event.ptMouse) ) {
					m_pManager->SendNotify(this, DUI_MSGTYPE_LINK, i);
					return;
				}
			}
		}
		if( event.Type == UIEVENT_CONTEXTMENU )
		{
			return;
		}
		// When you move over a link
		if( m_nLinks > 0 && event.Type == UIEVENT_MOUSEMOVE && IsEnabled() ) {
			int nHoverLink = -1;
			for( int i = 0; i < m_nLinks; i++ ) {
				if( ::PtInRect(&m_rcLinks[i], event.ptMouse) ) {
					nHoverLink = i;
					break;
				}
			}

			if(m_nHoverLink != nHoverLink) {
				m_nHoverLink = nHoverLink;
				Invalidate();
				return;
			}      
		}
		if( event.Type == UIEVENT_MOUSELEAVE ) {
			if( m_nLinks > 0 && IsEnabled() ) {
				if(m_nHoverLink != -1) {
					m_nHoverLink = -1;
					Invalidate();
					return;
				}
			}
		}

		CLabelUI::DoEvent(event);
	}

	SIZE CTextUI::EstimateSize(SIZE szAvailable)
	{
		// [SEC-29] EstimateSize can run after the control was detached from its
		// paint manager (container destroyed first, layout pass on a discarded
		// control, skin load failure cleanup). GetManager() then returns nullptr
		// and this function dereferenced it three times; the ASan probe
		// (tools/probes/no_manager_surface.cpp) reports "CRASH Text::EstimateSize".
		// Without a manager there is no DC to measure text with and no DPI object,
		// so fall back to the base class behaviour, which sizes from fixed values.
		if( m_pManager == nullptr ) return CControlUI::EstimateSize(szAvailable);

		CDuiString sText = GetText();
		RECT m_rcTextPadding = GetTextPadding();

		RECT rcText = { 0, 0, m_bAutoCalcWidth ? 9999 : GetManager()->GetDPIObj()->Scale(m_cxyFixed.cx), 9999 };
		rcText.left += m_rcTextPadding.left;
		rcText.right -= m_rcTextPadding.right;

		if( m_bShowHtml ) {   
			int nLinks = 0;
			CRenderEngine::DrawHtmlText(m_pManager->GetPaintDC(), m_pManager, rcText, sText, m_dwTextColor, nullptr, nullptr, nLinks, m_iFont, DT_CALCRECT | m_uTextStyle);
		}
		else {
			CRenderEngine::DrawText(m_pManager->GetPaintDC(), m_pManager, rcText, sText, m_dwTextColor, m_iFont, DT_CALCRECT | m_uTextStyle);
		}
		SIZE cXY = {rcText.right - rcText.left + m_rcTextPadding.left + m_rcTextPadding.right,
			rcText.bottom - rcText.top + m_rcTextPadding.top + m_rcTextPadding.bottom};
		
		if (m_bAutoCalcWidth)
		{
			m_cxyFixed.cx = MulDiv(cXY.cx, 100.0, GetManager()->GetDPIObj()->GetScale());
		}

		return CControlUI::EstimateSize(szAvailable);
	}

	void CTextUI::PaintText(HDC hDC)
	{
		CDuiString sText = GetText();
		if( sText.IsEmpty() ) {
			m_nLinks = 0;
			return;
		}

		if( m_dwTextColor == 0 ) m_dwTextColor = m_pManager->GetDefaultFontColor();
		if( m_dwDisabledTextColor == 0 ) m_dwDisabledTextColor = m_pManager->GetDefaultDisabledColor();

		m_nLinks = lengthof(m_rcLinks);
		RECT rc = m_rcItem;
		rc.left += m_rcTextPadding.left;
		rc.right -= m_rcTextPadding.right;
		rc.top += m_rcTextPadding.top;
		rc.bottom -= m_rcTextPadding.bottom;
		if( IsEnabled() ) {
			if( m_bShowHtml )
				CRenderEngine::DrawHtmlText(hDC, m_pManager, rc, sText, m_dwTextColor, \
				m_rcLinks, m_sLinks, m_nLinks, m_iFont, m_uTextStyle);
			else
				CRenderEngine::DrawText(hDC, m_pManager, rc, sText, m_dwTextColor, \
				m_iFont, m_uTextStyle);
		}
		else {
			if( m_bShowHtml )
				CRenderEngine::DrawHtmlText(hDC, m_pManager, rc, sText, m_dwDisabledTextColor, \
				m_rcLinks, m_sLinks, m_nLinks, m_iFont, m_uTextStyle);
			else
				CRenderEngine::DrawText(hDC, m_pManager, rc, sText, m_dwDisabledTextColor, \
				m_iFont, m_uTextStyle);
		}
	}
}
