#include "stdafx.h"
#include "UILoading.h"
#include "S3AttrRegistry.h"
#include <algorithm> // std::min/std::max


#include <Gdiplus.h>


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


using namespace DuiLib;
using namespace Gdiplus;

namespace
{

void DrawLine(Graphics* _objGraphics, PointF _objPointOne, PointF _objPointTwo, Color _objColor, int _intLineThickness)
{
    SolidBrush brush(_objColor);
    Pen objPen(&brush, (Gdiplus::REAL)_intLineThickness);
    {
        objPen.SetStartCap(LineCap::LineCapRound);
        objPen.SetEndCap(LineCap::LineCapRound);
        _objGraphics->DrawLine(&objPen, _objPointOne, _objPointTwo);
    }
}

PointF GetCoordinate(PointF _objCircleCenter, int _intRadius, double _dblAngle)
{
    double dblAngle = 3.14 * _dblAngle / 180;
    PointF pf(_objCircleCenter.X + _intRadius * static_cast<float>(cos(dblAngle)), _objCircleCenter.Y + _intRadius * static_cast<float>(sin(dblAngle)));
    return pf;
}

Color Darken(Color _objColor, int _intPercent)
{
    int intRed = _objColor.GetR();
    int intGreen = _objColor.GetG();
    int intBlue = _objColor.GetB();
    Color color(_intPercent, std::min(intRed, 255), std::min(intGreen, 255), std::min(intBlue, 255));
    return color;
}

double* GetSpokesAngles(int _intNumberSpoke)
{
    double* Angles = new double[_intNumberSpoke];
    double dblAngle = static_cast<double>(360) / _intNumberSpoke;
    for (int shtCounter = 0; shtCounter < _intNumberSpoke; shtCounter++)
    {
        Angles[shtCounter] = (shtCounter == 0 ? dblAngle : Angles[shtCounter - 1] + dblAngle);
    }
    return Angles;
}
}

IMPLEMENT_DUICONTROL(CLoadingUI)

CLoadingUI::CLoadingUI()
	: m_bStop(true)
	, m_nTime(100)
	, m_nNumber(0)
	, m_Angles(nullptr)
	, m_Colors(nullptr)
	, m_NumberOfSpoke(10)
	, m_SpokeThickness(4)
	, m_OuterCircleRadius( 10)
	, m_InnerCircleRadius (8)
{

}

CLoadingUI::~CLoadingUI()
{
	Stop();

    if(m_Angles) delete m_Angles;
    if(m_Colors) delete m_Colors;
}

LPCTSTR CLoadingUI::GetClass() const
{
	return DUI_CTR_LOADINGCIRCLE;
}

LPVOID CLoadingUI::GetInterface(LPCTSTR pstrName)
{
	if (_tcscmp(pstrName, DUI_CTR_LOADINGCIRCLE) == 0) return static_cast<CLoadingUI*>(this);
	return CControlUI::GetInterface(pstrName);
}

void CLoadingUI::SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue)
{
        // ---- S3/T3.8 batch 2: fast path via compile-time hash registry ----
        // O(log n) lookup instead of O(n) string compares.
        // Falls through to the legacy chain below when unmatched, so
        // third-party custom attributes keep working unchanged (D3 open).
        switch( DuiLib::s3::lookup_prop_t(pstrName) ) {
        case DuiLib::s3::PropId::style: { LPTSTR pstr = nullptr; m_nNumber = _tcstol(pstrValue, &pstr, 10); return; }
        case DuiLib::s3::PropId::time: { LPTSTR pstr = nullptr; m_nTime = _tcstol(pstrValue, &pstr, 10); return; }
        case DuiLib::s3::PropId::spoke: { LPTSTR pstr = nullptr; m_NumberOfSpoke = _tcstol(pstrValue, &pstr, 0); return; }
        case DuiLib::s3::PropId::thickness: { LPTSTR pstr = nullptr; m_SpokeThickness = _tcstol(pstrValue, &pstr, 0); return; }
        case DuiLib::s3::PropId::outradius: { LPTSTR pstr = nullptr; m_OuterCircleRadius = _tcstol(pstrValue, &pstr, 0); return; }
        case DuiLib::s3::PropId::innerradius: { LPTSTR pstr = nullptr; m_InnerCircleRadius = _tcstol(pstrValue, &pstr, 0); return; }
        case DuiLib::s3::PropId::color: { while (*pstrValue > '\0' && *pstrValue <= _T(' ')) pstrValue = ::CharNext(pstrValue); if (*pstrValue == _T('#')) pstrValue = ::CharNext(pstrValue); LPTSTR pstr = nullptr; DWORD clrColor = _tcstoul(pstrValue, &pstr, 16); m_Color.SetValue(clrColor); return; }
        default: break;
        }

        // ---- legacy fallback chain (kept verbatim for compatibility) ----

	if (_tcsicmp(pstrName, _T("style") ) == 0)
	{
		LPTSTR pstr = nullptr;
		m_nNumber = _tcstol(pstrValue, &pstr, 10);
	}
	else if (_tcsicmp(pstrName, _T("time") ) == 0)
	{
		LPTSTR pstr = nullptr;
		m_nTime = _tcstol(pstrValue, &pstr, 10);
	}
	else if (_tcsicmp(pstrName, _T("spoke") ) == 0)
	{
		LPTSTR pstr = nullptr;
		m_NumberOfSpoke = _tcstol(pstrValue, &pstr, 0);
	}
	else if (_tcsicmp(pstrName, _T("thickness")) == 0)
	{
		LPTSTR pstr = nullptr;
		m_SpokeThickness = _tcstol(pstrValue, &pstr, 0);
	}
	else if (_tcsicmp(pstrName, _T("outradius")) == 0)
	{
		LPTSTR pstr = nullptr;
		m_OuterCircleRadius = _tcstol(pstrValue, &pstr, 0);
	}
	else if (_tcsicmp(pstrName, _T("innerradius")) == 0)
	{
		LPTSTR pstr = nullptr;
		m_InnerCircleRadius = _tcstol(pstrValue, &pstr, 0);
	}
	else if (_tcsicmp(pstrName, _T("color")) == 0)
	{
		while (*pstrValue > _T('\0') && *pstrValue <= _T(' ')) pstrValue = ::CharNext(pstrValue);
		if (*pstrValue == _T('#')) pstrValue = ::CharNext(pstrValue);
		LPTSTR pstr = nullptr;
		DWORD clrColor = _tcstoul(pstrValue, &pstr, 16);
		m_Color.SetValue(clrColor);
	}
	else
	{
		__super::SetAttribute(pstrName, pstrValue);
	}
}

void CLoadingUI::PaintBkImage(HDC hDC)
{
	m_CenterPoint.X = (Gdiplus::REAL)this->GetWidth() / 2;
	m_CenterPoint.Y = (Gdiplus::REAL)this->GetHeight() / 2;
	if (m_NumberOfSpoke > 0)
	{
		Image* img = new Bitmap(this->GetWidth(), this->GetHeight());
		Graphics g(img);
		g.SetSmoothingMode(SmoothingMode::SmoothingModeHighQuality);

		int intPosition = m_ProgressValue;
		for (int intCounter = 0; intCounter < m_NumberOfSpoke; intCounter++)
		{
			intPosition = intPosition % m_NumberOfSpoke;
			DrawLine(&g,
				GetCoordinate(m_CenterPoint, m_InnerCircleRadius, m_Angles[intPosition]),
				GetCoordinate(m_CenterPoint, m_OuterCircleRadius, m_Angles[intPosition]),
				m_Colors[intCounter], m_SpokeThickness);
			intPosition++;
		}
		// 获得窗口的Graphics对象
		Graphics gh(hDC);
		// 将描画好的CacheImage画到窗口上
		gh.DrawImage(img, Gdiplus::Rect(m_rcItem.left, m_rcItem.top, m_rcItem.right - m_rcItem.left, m_rcItem.bottom - m_rcItem.top));
		delete img;
	}
}

Color* CLoadingUI::GenerateColorsPallet(Color _objColor, bool _blnShadeColor, int _intNbSpoke)
{
	Color* objColors = new Color[m_NumberOfSpoke];
	::byte bytIncrement = (::byte)(255 / m_NumberOfSpoke);
	::byte PERCENTAGE_OF_DARKEN = 0;
	for (int intCursor = 0; intCursor < m_NumberOfSpoke; intCursor++)
	{
		if (_blnShadeColor)
		{
			if (intCursor == 0 || intCursor < m_NumberOfSpoke - _intNbSpoke)
			{
				objColors[intCursor] = _objColor;
			}
			else
			{
				PERCENTAGE_OF_DARKEN += bytIncrement;
				if (PERCENTAGE_OF_DARKEN > 255)
				{
					PERCENTAGE_OF_DARKEN = 255;
				}
				objColors[intCursor] = Darken(_objColor, PERCENTAGE_OF_DARKEN);
			}
		}
		else
		{
			objColors[intCursor] = _objColor;
		}
	}
	return objColors;
}

void CLoadingUI::Start()
{
	if (m_nTime > 0 && m_pManager && m_bStop == true)
	{
		m_pManager->SetTimer(this, static_cast<UINT>(TIMEID::kTimerLoadingId), m_nTime);
	}
	m_bStop = false;
}

void CLoadingUI::Stop()
{
	m_bStop = true;
	if (m_pManager)
	{
		m_pManager->KillTimer(this, static_cast<UINT>(TIMEID::kTimerLoadingId));
	}
}

void CLoadingUI::Init()
{
	CControlUI::Init();
	m_Angles = GetSpokesAngles(m_NumberOfSpoke);
	m_Colors = GenerateColorsPallet(m_Color, true, m_NumberOfSpoke);
	Start();
}

void DuiLib::CLoadingUI::DoEvent(TEventUI& event)
{
    if (event.Type == UIEVENT_TIMER && event.wParam == static_cast<WPARAM>(TIMEID::kTimerLoadingId))
    {
        m_ProgressValue = ++m_ProgressValue % m_NumberOfSpoke;
        Invalidate();
    }
    else
    {
        CControlUI::DoEvent(event);
    }
}

CControlUI* CreateLoadingControl(LPCTSTR pstrType)
{
	if (_tcsicmp(pstrType, DUI_CTR_LOADINGCIRCLE) == 0)
	{
		return new CLoadingUI();
	}

	return nullptr;
}

