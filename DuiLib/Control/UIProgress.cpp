#include "StdAfx.h"
#include "S3AttrRegistry.h"
#include "UIProgress.h"


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
	IMPLEMENT_DUICONTROL(CProgressUI)

	CProgressUI::CProgressUI() : m_bShowText(false), m_bHorizontal(true), m_nMin(0), m_nMax(100), m_nValue(0), m_bStretchForeImage(true)
	{
		m_uTextStyle = DT_SINGLELINE | DT_CENTER;
		SetFixedHeight(12);
	}

	LPCTSTR CProgressUI::GetClass() const
	{
		return _T("ProgressUI");
	}

	LPVOID CProgressUI::GetInterface(LPCTSTR pstrName)
	{
		if( _tcsicmp(pstrName, DUI_CTR_PROGRESS) == 0 ) return static_cast<CProgressUI*>(this);
		return CLabelUI::GetInterface(pstrName);
	}
	
	bool CProgressUI::IsShowText()
	{
		return m_bShowText;
	}

	void CProgressUI::SetShowText(bool bShowText)
	{
		if( m_bShowText == bShowText ) return;
		m_bShowText = bShowText;
		if(!m_bShowText) SetText(_T(""));
	}

	bool CProgressUI::IsHorizontal()
	{
		return m_bHorizontal;
	}

	void CProgressUI::SetHorizontal(bool bHorizontal)
	{
		if( m_bHorizontal == bHorizontal ) return;

		m_bHorizontal = bHorizontal;
		Invalidate();
	}

	int CProgressUI::GetMinValue() const
	{
		return m_nMin;
	}

	void CProgressUI::SetMinValue(int nMin)
	{
		m_nMin = nMin;
		Invalidate();
	}

	int CProgressUI::GetMaxValue() const
	{
		return m_nMax;
	}

	void CProgressUI::SetMaxValue(int nMax)
	{
		m_nMax = nMax;
		Invalidate();
	}

	int CProgressUI::GetValue() const
	{
		return m_nValue;
	}

	void CProgressUI::SetValue(int nValue)
	{
		if(nValue == m_nValue || nValue<m_nMin || nValue > m_nMax) {
			return;
		}
		m_nValue = nValue;
		Invalidate();
		UpdateText();
	}

	void CProgressUI::SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue)
	{		// ---- S3/T3.3a: fast path via compile-time hash registry ----
		// O(log n) lookup instead of O(n) string compares.
		// Falls through to the legacy chain below when unmatched, so
		// third-party custom attributes keep working unchanged.
		switch( DuiLib::s3::lookup_prop_t(pstrName) ) {
		case DuiLib::s3::PropId::hor: { SetHorizontal(_tcsicmp(pstrValue, _T("true")) == 0); return; }
		case DuiLib::s3::PropId::min: { SetMinValue(_ttoi(pstrValue)); return; }
		case DuiLib::s3::PropId::max: { SetMaxValue(_ttoi(pstrValue)); return; }
		case DuiLib::s3::PropId::value: { SetValue(_ttoi(pstrValue)); return; }
		case DuiLib::s3::PropId::isstretchfore: { SetStretchForeImage(_tcsicmp(pstrValue, _T("true")) == 0? true : false); return; }
		case DuiLib::s3::PropId::showtext: { SetShowText(_tcsicmp(pstrValue, _T("true")) == 0? true : false); return; }
		default: break;
		}

		// ---- legacy fallback chain (kept verbatim for compatibility) ----

		if( _tcsicmp(pstrName, _T("hor")) == 0 ) SetHorizontal(_tcsicmp(pstrValue, _T("true")) == 0);
		else if( _tcsicmp(pstrName, _T("min")) == 0 ) SetMinValue(_ttoi(pstrValue));
		else if( _tcsicmp(pstrName, _T("max")) == 0 ) SetMaxValue(_ttoi(pstrValue));
		else if( _tcsicmp(pstrName, _T("value")) == 0 ) SetValue(_ttoi(pstrValue));
		else if( _tcsicmp(pstrName, _T("isstretchfore"))==0) SetStretchForeImage(_tcsicmp(pstrValue, _T("true")) == 0? true : false);
		else if( _tcsicmp(pstrName, _T("showtext"))==0) SetShowText(_tcsicmp(pstrValue, _T("true")) == 0? true : false);
		else CLabelUI::SetAttribute(pstrName, pstrValue);
	}

	void CProgressUI::PaintForeColor(HDC hDC)
	{
		if(m_dwForeColor == 0) return;

		if( m_nMax <= m_nMin ) m_nMax = m_nMin + 1;
		if( m_nValue > m_nMax ) m_nValue = m_nMax;
		if( m_nValue < m_nMin ) m_nValue = m_nMin;

		RECT rc = m_rcItem;
		if( m_bHorizontal ) {
			rc.right = m_rcItem.left + (m_nValue - m_nMin) * (m_rcItem.right - m_rcItem.left) / (m_nMax - m_nMin);
		}
		else {
			rc.bottom = m_rcItem.top + (m_rcItem.bottom - m_rcItem.top) * (m_nMax - m_nValue) / (m_nMax - m_nMin);
		
		}
		
		CRenderEngine::DrawColor(hDC, rc, GetAdjustColor(m_dwForeColor));
	}

	void CProgressUI::PaintForeImage(HDC hDC)
	{
		if( m_nMax <= m_nMin ) m_nMax = m_nMin + 1;
		if( m_nValue > m_nMax ) m_nValue = m_nMax;
		if( m_nValue < m_nMin ) m_nValue = m_nMin;

		RECT rc = {0};
		if( m_bHorizontal ) {
			rc.right = (m_nValue - m_nMin) * (m_rcItem.right - m_rcItem.left) * 1.0f / (m_nMax - m_nMin);
			rc.bottom = m_rcItem.bottom - m_rcItem.top;
		}
		else {
			rc.top = (m_rcItem.bottom - m_rcItem.top) * (m_nMax - m_nValue) * 1.0f / (m_nMax - m_nMin);
			rc.right = m_rcItem.right - m_rcItem.left;
			rc.bottom = m_rcItem.bottom - m_rcItem.top;
		}

		if( !m_sForeImage.IsEmpty() ) {
			m_sForeImageModify.Empty();
			int sw = MulDiv(rc.right - rc.left, 100, GetManager()->GetDPIObj()->GetScale());
			int sh = MulDiv(rc.bottom - rc.top, 100, GetManager()->GetDPIObj()->GetScale());
			rc.left = MulDiv(rc.left, 100, GetManager()->GetDPIObj()->GetScale());
			rc.top = MulDiv(rc.top, 100, GetManager()->GetDPIObj()->GetScale());
			rc.right = rc.left + sw;
			rc.bottom = rc.top + sh;
			if (m_bStretchForeImage) {
				m_sForeImageModify.SmallFormat(_T("dest='%d,%d,%d,%d'"), rc.left, rc.top, rc.right, rc.bottom);
			}
			else {
				m_sForeImageModify.SmallFormat(_T("dest='%d,%d,%d,%d' source='%d,%d,%d,%d'"), rc.left, rc.top, rc.right, rc.bottom, rc.left, rc.top, rc.right, rc.bottom);
			}

			if( !DrawImage(hDC, static_cast<LPCTSTR>(m_sForeImage), static_cast<LPCTSTR>(m_sForeImageModify)) ) {}
			else return;
		}
	}

	bool CProgressUI::IsStretchForeImage()
	{
		return m_bStretchForeImage;
	}

	void CProgressUI::SetStretchForeImage( bool bStretchForeImage /*= true*/ )
	{
		if (m_bStretchForeImage==bStretchForeImage)		return;
		m_bStretchForeImage=bStretchForeImage;
		Invalidate();
	}

	void CProgressUI::UpdateText()
	{
		if(m_bShowText) {
			CDuiString sText;
			sText.Format(_T("%.0f%%"), (m_nValue - m_nMin) * 100.0f / (m_nMax - m_nMin));
			SetText(sText);
		}
	}
}
