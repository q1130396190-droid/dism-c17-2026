#include "StdAfx.h"
#include "UIRing.h"


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
	IMPLEMENT_DUICONTROL(CRingUI)

		CRingUI::CRingUI() : m_fCurAngle(0.0f), m_pBkimage(nullptr)
	{
	}

	CRingUI::~CRingUI()
	{
		if(m_pManager) m_pManager->KillTimer(this, RING_TIMERID);

		DeleteImage();
	}

	LPCTSTR CRingUI::GetClass() const
	{
		return _T("RingUI");
	}

	LPVOID CRingUI::GetInterface( LPCTSTR pstrName )
	{
		if( _tcscmp(pstrName, _T("Ring")) == 0 ) return static_cast<CRingUI*>(this);
		return CLabelUI::GetInterface(pstrName);
	}

	void CRingUI::SetAttribute( LPCTSTR pstrName, LPCTSTR pstrValue )
	{
		if( _tcscmp(pstrName, _T("bkimage")) == 0 ) SetBkImage(pstrValue);
		else CLabelUI::SetAttribute(pstrName, pstrValue);
	}

	void CRingUI::SetBkImage( LPCTSTR pStrImage )
	{
		if (m_sBkImage == pStrImage) return;
		m_sBkImage = pStrImage;
		DeleteImage();
		Invalidate();
	}

	void CRingUI::PaintBkImage( HDC hDC )
	{
		if(m_pBkimage == nullptr) {
			InitImage();
		}

		if(m_pBkimage != nullptr) {
			RECT rcItem = m_rcItem;
			int iWidth = rcItem.right - rcItem.left;
			int iHeight = rcItem.bottom - rcItem.top;
			Gdiplus::PointF centerPos(rcItem.left + iWidth/2, rcItem.top + iHeight/2);

			// 解决偶数时抖动问题
			if ((iWidth % 2) == 0) centerPos.X -= 0.5;
			if ((iHeight % 2) == 0) centerPos.Y -= 0.5;

			Gdiplus::Graphics graphics(hDC);
			graphics.TranslateTransform(centerPos.X,centerPos.Y);
			graphics.RotateTransform(m_fCurAngle);
			graphics.TranslateTransform(-centerPos.X, -centerPos.Y);//还原源点
			graphics.DrawImage(m_pBkimage,rcItem.left,rcItem.top,iWidth,iHeight);
		}
	}

	void CRingUI::DoEvent( TEventUI& event )
	{
		if( event.Type == UIEVENT_TIMER && event.wParam == RING_TIMERID ) {
			if(m_fCurAngle > 359) {
				m_fCurAngle = 0;
			}
			m_fCurAngle += 36.0;
			//Invalidate();
			NeedParentUpdate();
		}
		else {
			CLabelUI::DoEvent(event);
		}
	}

	void CRingUI::InitImage()
	{
		TImageInfo* pImageInfo = CRenderEngine::GdiplusLoadImage(GetBkImage());
		if(pImageInfo != nullptr) {
			m_pBkimage = pImageInfo->pImage;

			delete pImageInfo;
			pImageInfo = nullptr;

			if(m_pManager != nullptr && m_pBkimage != nullptr) {
				m_pManager->SetTimer(this, RING_TIMERID, 100);
			}
		}

	}

	void CRingUI::DeleteImage()
	{
		if ( m_pBkimage != nullptr )
		{
			delete m_pBkimage;
			m_pBkimage = nullptr;
		}
	}
}
