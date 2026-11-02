#include "StdAfx.h"
#include "UIShadow.h"
#include "math.h"
#include "crtdbg.h"
#include <algorithm> // std::min/std::max


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

const TCHAR *strWndClassName = _T("PerryShadowWnd");
bool CShadowUI::s_bHasInit = FALSE;

CShadowUI::CShadowUI(void)
: m_hWnd(nullptr)
, m_OriParentProc(0)
, m_Status(0)
, m_nDarkness(150)
, m_nSharpness(5)
, m_nSize(0)
, m_nxOffset(0)
, m_nyOffset(0)
, m_Color(RGB(0, 0, 0))
, m_WndSize(0)
, m_bUpdate(false)
, m_bIsImageMode(false)
, m_bIsShowShadow(false)
, m_bIsDisableShadow(false)
{
	::ZeroMemory(&m_rcShadowCorner, sizeof(RECT));
}

CShadowUI::~CShadowUI(void)
{
}

bool CShadowUI::Initialize(HINSTANCE hInstance)
{
	if (s_bHasInit)
		return false;

	// Register window class for shadow window
	WNDCLASSEX wcex;

	memset(&wcex, 0, sizeof(wcex));

	wcex.cbSize = sizeof(WNDCLASSEX); 
	wcex.style			= CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc	= DefWindowProc;
	wcex.cbClsExtra		= 0;
	wcex.cbWndExtra		= 0;
	wcex.hInstance		= hInstance;
	wcex.hIcon			= nullptr;
	wcex.hCursor		= LoadCursor(nullptr, IDC_ARROW);
	wcex.hbrBackground	= reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
	wcex.lpszMenuName	= nullptr;
	wcex.lpszClassName	= strWndClassName;
	wcex.hIconSm		= nullptr;

	RegisterClassEx(&wcex);

	s_bHasInit = true;
	return true;
}

void CShadowUI::Create(CPaintManagerUI* pPaintManager)
{
	if(!m_bIsShowShadow)
		return;

	// Already initialized
	_ASSERT(CPaintManagerUI::GetInstance() != INVALID_HANDLE_VALUE);
	_ASSERT(pPaintManager != nullptr);
	m_pManager = pPaintManager;
	HWND hParentWnd = m_pManager->GetPaintWindow();
	// Add parent window - shadow pair to the map
	_ASSERT(GetShadowMap().find(hParentWnd) == GetShadowMap().end());	// Only one shadow for each window
	GetShadowMap()[hParentWnd] = this;

	// Determine the initial show state of shadow according to parent window's state
	LONG_PTR lParentStyle = GetWindowLongPtr(hParentWnd, GWL_STYLE);

	// Create the shadow window
	LONG styleValue = static_cast<LONG>(lParentStyle & WS_CAPTION);
	m_hWnd = CreateWindowEx(WS_EX_LAYERED | WS_EX_TRANSPARENT, strWndClassName, nullptr,
		/*WS_VISIBLE | */styleValue | WS_POPUPWINDOW,
		CW_USEDEFAULT, 0, 0, 0, hParentWnd, nullptr, CPaintManagerUI::GetInstance(), nullptr);

	if(!(WS_VISIBLE & lParentStyle))	// Parent invisible
		m_Status = static_cast<BYTE>(ShadowStatus::kEnabled);
	else if((WS_MAXIMIZE | WS_MINIMIZE) & lParentStyle)	// Parent visible but does not need shadow
		m_Status = static_cast<BYTE>(SsBits(ShadowStatus::kEnabled, ShadowStatus::kParentVisible));
	else	// Show the shadow
	{
		m_Status = static_cast<BYTE>(SsBits(ShadowStatus::kEnabled, ShadowStatus::kVisible, ShadowStatus::kParentVisible));
		::ShowWindow(m_hWnd, SW_SHOWNOACTIVATE);
		Update(hParentWnd);
	}

	// Replace the original WndProc of parent window to steal messages
	m_OriParentProc = GetWindowLongPtr(hParentWnd, GWLP_WNDPROC);

#pragma warning(disable: 4311)	// temporrarily disable the type_cast warning in Win32
	SetWindowLongPtr(hParentWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ParentProc));
#pragma warning(default: 4311)

}

std::map<HWND, CShadowUI *>& CShadowUI::GetShadowMap()
{
	static std::map<HWND, CShadowUI *> s_Shadowmap;
	return s_Shadowmap;
}

LRESULT CALLBACK CShadowUI::ParentProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	// [SEC-24] map::operator[] default-inserts nullptr when hwnd is missing, and
	// the very next line dereferences it. A window can legitimately receive
	// messages after its shadow entry was removed (destroy order, subclass
	// lingering), so this must be a guarded lookup, not an assertion.
	auto it = GetShadowMap().find(hwnd);
	if (it == GetShadowMap().end() || it->second == nullptr) {
		return ::DefWindowProc(hwnd, uMsg, wParam, lParam);
	}
	CShadowUI *pThis = it->second;
	if (pThis->IsDisableShadow()) {
		return CallWindowProc((WNDPROC)pThis->m_OriParentProc, hwnd, uMsg, wParam, lParam);
	}

	switch(uMsg)
	{
	case WM_ACTIVATEAPP:
		{
			if(!IsWindowEnabled(hwnd)) break;
			::SetWindowPos(pThis->m_hWnd, hwnd, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
			break;
		}
	case WM_NCACTIVATE:
		{
			if(!IsWindowEnabled(hwnd)) break;
			::SetWindowPos(pThis->m_hWnd, hwnd, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
			break;
		}
	case WM_WINDOWPOSCHANGED:
		{
			RECT WndRect;
			LPWINDOWPOS pWndPos;
			pWndPos = reinterpret_cast<LPWINDOWPOS>(lParam);
			GetWindowRect(hwnd, &WndRect);
			if (pThis->m_bIsImageMode) {
				SetWindowPos(pThis->m_hWnd, hwnd, WndRect.left - pThis->m_nSize, WndRect.top - pThis->m_nSize, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
			}
			else {
				SetWindowPos(pThis->m_hWnd, hwnd, WndRect.left + pThis->m_nxOffset - pThis->m_nSize, WndRect.top + pThis->m_nyOffset - pThis->m_nSize, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
			}

			if (pWndPos->flags & SWP_SHOWWINDOW) {
				if ((pThis->m_Status & static_cast<BYTE>(ShadowStatus::kEnabled)) && !(pThis->m_Status & static_cast<BYTE>(ShadowStatus::kParentVisible)))
				{
					pThis->m_bUpdate = true;
					::ShowWindow(pThis->m_hWnd, SW_SHOWNOACTIVATE);
					pThis->m_Status |= static_cast<BYTE>(SsBits(ShadowStatus::kVisible, ShadowStatus::kParentVisible));
				}
			}
			else if (pWndPos->flags & SWP_HIDEWINDOW) {
				if (pThis->m_Status & static_cast<BYTE>(ShadowStatus::kEnabled))
				{
					::ShowWindow(pThis->m_hWnd, SW_HIDE);
					pThis->m_Status &= ~static_cast<BYTE>(SsBits(ShadowStatus::kVisible, ShadowStatus::kParentVisible));
				}
			}
			break;
		}
		
	case WM_MOVE:
		if(pThis->m_Status & static_cast<BYTE>(ShadowStatus::kVisible)) {
			RECT WndRect;
			GetWindowRect(hwnd, &WndRect);
			if (pThis->m_bIsImageMode) {
				SetWindowPos(pThis->m_hWnd, hwnd, WndRect.left - pThis->m_nSize, WndRect.top - pThis->m_nSize, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
			}
			else {
				SetWindowPos(pThis->m_hWnd, hwnd, WndRect.left + pThis->m_nxOffset - pThis->m_nSize, WndRect.top + pThis->m_nyOffset - pThis->m_nSize, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
			}
		}
		break;

	case WM_SIZE:
		if(pThis->m_Status & static_cast<BYTE>(ShadowStatus::kEnabled)) {
			if(SIZE_MAXIMIZED == wParam || SIZE_MINIMIZED == wParam) {
				::ShowWindow(pThis->m_hWnd, SW_HIDE);
				pThis->m_Status &= ~static_cast<BYTE>(ShadowStatus::kVisible);
			}
			// Parent maybe resized even if invisible
			else if(pThis->m_Status & static_cast<BYTE>(ShadowStatus::kParentVisible)) {
				// Awful! It seems that if the window size was not decreased
				// the window region would never be updated until WM_PAINT was sent.
				// So do not Update() until next WM_PAINT is received in this case
				if(LOWORD(lParam) > LOWORD(pThis->m_WndSize) || HIWORD(lParam) > HIWORD(pThis->m_WndSize))
					pThis->m_bUpdate = true;
				else
					pThis->Update(hwnd);

				if(!(pThis->m_Status & static_cast<BYTE>(ShadowStatus::kVisible))) {
					::ShowWindow(pThis->m_hWnd, SW_SHOWNOACTIVATE);
					pThis->m_Status |= static_cast<BYTE>(ShadowStatus::kVisible);
				}
			}
			pThis->m_WndSize = lParam;
		}
		break;

	case WM_PAINT:
		{
			if(pThis->m_bUpdate) {
				pThis->Update(hwnd);
				pThis->m_bUpdate = false;
			}
			break;
		}

		// In some cases of sizing, the up-right corner of the parent window region would not be properly updated
		// Update() again when sizing is finished
	case WM_EXITSIZEMOVE:
		if(pThis->m_Status & static_cast<BYTE>(ShadowStatus::kVisible)) {
			pThis->Update(hwnd);
		}
		break;

	case WM_SHOWWINDOW:
		if(pThis->m_Status & static_cast<BYTE>(ShadowStatus::kEnabled)) {
			// the window is being hidden
			if(!wParam) {
				::ShowWindow(pThis->m_hWnd, SW_HIDE);
				pThis->m_Status &= ~static_cast<BYTE>(SsBits(ShadowStatus::kVisible, ShadowStatus::kParentVisible));
			}
			else if(!(pThis->m_Status & static_cast<BYTE>(ShadowStatus::kParentVisible))) {
				//pThis->Update(hwnd);
				pThis->m_bUpdate = true;
				::ShowWindow(pThis->m_hWnd, SW_SHOWNOACTIVATE);
				pThis->m_Status |= static_cast<BYTE>(SsBits(ShadowStatus::kVisible, ShadowStatus::kParentVisible));
			}
		}
		break;

	case WM_DESTROY:
		DestroyWindow(pThis->m_hWnd);	// Destroy the shadow
		break;
	case WM_NCDESTROY:
		GetShadowMap().erase(hwnd);	// Remove this window and shadow from the map
		break;
	}

	return CallWindowProc((WNDPROC)pThis->m_OriParentProc, hwnd, uMsg, wParam, lParam);
}

//Formats GetLastError() value.
void GetLastErrorMessage() 
{          
	LPVOID lpMsgBuf;
	FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
		nullptr, GetLastError(),
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), // Default language
		reinterpret_cast<LPTSTR>(&lpMsgBuf), 0, nullptr
	);

	// Display the string.
	//MessageBox(nullptr, (const wchar_t*)lpMsgBuf, L"GetLastError", MB_OK | MB_ICONINFORMATION);

	// Free the buffer.
	LocalFree(lpMsgBuf);
}

void CShadowUI::Update(HWND hParent)
{
	if(!m_bIsShowShadow || !(m_Status & static_cast<BYTE>(ShadowStatus::kVisible))) return;
	RECT WndRect = {0, 0, 0, 0};
	if (!::GetWindowRect(hParent, &WndRect)) return;
	if (m_bIsImageMode && m_sShadowImage.IsEmpty()) return;
	if (!m_bIsImageMode && m_nSize == 0) return;

	// Reject degenerate or absurd geometry before it reaches the pixel loops.
	// A non-positive width used to yield a negative DIB size, and MakeShadow
	// then dereferenced a null pvBits.
	const long long lW = (long long)WndRect.right - WndRect.left;
	const long long lH = (long long)WndRect.bottom - WndRect.top;
	const long long lSize2 = (long long)m_nSize * 2;
	if (lW <= 0 || lH <= 0) return;
	const long long lShadW = lW + lSize2;
	const long long lShadH = lH + lSize2;
	const long long kMaxShadowDim = 16384; // keeps the DIB allocation sane
	if (lShadW <= 0 || lShadH <= 0 || lShadW > kMaxShadowDim || lShadH > kMaxShadowDim) return;
	const int nShadWndWid = (int)lShadW;
	const int nShadWndHei = (int)lShadH;

	// Create the alpha blending bitmap
	BITMAPINFO bmi;        // bitmap header
	ZeroMemory(&bmi, sizeof(BITMAPINFO));
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = nShadWndWid;
	bmi.bmiHeader.biHeight = nShadWndHei;
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;         // four 8-bit components
	bmi.bmiHeader.biCompression = BI_RGB;
	bmi.bmiHeader.biSizeImage = nShadWndWid * nShadWndHei * 4;
	BYTE *pvBits;          // pointer to DIB section
	HBITMAP hbitmap = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, (void **)&pvBits, nullptr, 0);
	if (hbitmap == nullptr || pvBits == nullptr) {
		if (hbitmap != nullptr) DeleteObject(hbitmap);
		GetLastErrorMessage();
		return;
	}

	HDC hMemDC = CreateCompatibleDC(nullptr);
	if (hMemDC == nullptr) {
		DeleteObject(hbitmap);
		GetLastErrorMessage();
		return;
	}
	HBITMAP hOriBmp = reinterpret_cast<HBITMAP>(SelectObject(hMemDC, hbitmap));
	if (hOriBmp == nullptr) {
		DeleteDC(hMemDC);
		DeleteObject(hbitmap);
		return;
	}
	if (m_bIsImageMode) {
		RECT rcPaint = {0, 0, nShadWndWid, nShadWndHei};
		const TDrawInfo* pDrawInfo = (m_pManager != nullptr) ? m_pManager->GetDrawInfo(m_sShadowImage, _T("")) : nullptr;
		if(pDrawInfo != nullptr) {
			const TImageInfo* data = (m_pManager != nullptr) ? m_pManager->GetImageEx(pDrawInfo->sImageName, pDrawInfo->sResType, pDrawInfo->dwMask, pDrawInfo->bHSL, pDrawInfo->bGdiplus) : nullptr;
			if( !data ) { SelectObject(hMemDC, hOriBmp); DeleteObject(hbitmap); DeleteDC(hMemDC); return; }
			RECT rcBmpPart = {0};
			rcBmpPart.right = data->nX;
			rcBmpPart.bottom = data->nY;
			RECT corner = m_rcShadowCorner;
			CRenderEngine::DrawImage(hMemDC, data->hBitmap, rcPaint, rcPaint, rcBmpPart, corner, data->bAlpha, 0xFF, true, false, false);
		}
	}
	else {
		ZeroMemory(pvBits, bmi.bmiHeader.biSizeImage);
		MakeShadow((UINT32 *)pvBits, hParent, &WndRect);
	}

	POINT ptDst;
	if (m_bIsImageMode) {
		ptDst.x = WndRect.left - m_nSize;
		ptDst.y = WndRect.top - m_nSize;
	}
	else {
		ptDst.x = WndRect.left + m_nxOffset - m_nSize;
		ptDst.y = WndRect.top + m_nyOffset - m_nSize;
	}

	POINT ptSrc = {0, 0};
	SIZE WndSize = {nShadWndWid, nShadWndHei};
	BLENDFUNCTION blendPixelFunction= { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
	MoveWindow(m_hWnd, ptDst.x, ptDst.y, nShadWndWid, nShadWndHei, FALSE);
	BOOL bRet= ::UpdateLayeredWindow(m_hWnd, nullptr, &ptDst, &WndSize, hMemDC, &ptSrc, 0, &blendPixelFunction, ULW_ALPHA);
	_ASSERT(bRet); // something was wrong....
	// Delete used resources
	SelectObject(hMemDC, hOriBmp);
	DeleteObject(hbitmap);
	DeleteDC(hMemDC);
}

void CShadowUI::MakeShadow(UINT32 *pShadBits, HWND hParent, RECT *rcParent)
{
	// The shadow algorithm:
	// Get the region of parent window,
	// Apply morphologic erosion to shrink it into the size (ShadowWndSize - Sharpness)
	// Apply modified (with blur effect) morphologic dilation to make the blurred border
	// The algorithm is optimized by assuming parent window is just "one piece" and without "wholes" on it

	if (pShadBits == nullptr || rcParent == nullptr || hParent == nullptr) return;

	// Determine the start/end point of each horizontal scan line. All of these
	// values feed pixel-pointer arithmetic, so validate them before allocating:
	// a non-positive extent or an overflowing kernel size would otherwise index
	// outside the DIB.
	const long long lW = (long long)rcParent->right - rcParent->left;
	const long long lH = (long long)rcParent->bottom - rcParent->top;
	const long long lSize = m_nSize;
	if (lW <= 0 || lH <= 0) return;
	const long long lShadW = lW + 2 * lSize;
	const long long lShadH = lH + 2 * lSize;
	const long long kMaxDim = 16384;
	if (lShadW <= 0 || lShadH <= 0 || lShadW > kMaxDim || lShadH > kMaxDim) return;
	const long long lKernel = std::max((long long)m_nSize, (long long)m_nSharpness);
	if (lKernel < 0 || (2 * lKernel + 1) * (2 * lKernel + 1) > 16 * 1024 * 1024) return;

	// Get the region of parent window,
	HRGN hParentRgn = CreateRectRgn(0, 0, (int)lW, (int)lH);
	if (hParentRgn == nullptr) return;
	if (GetWindowRgn(hParent, hParentRgn) == ERROR) {
		// Keep going: the initial rect region is a valid fallback shape.
	}

	SIZE szParent = {(int)lW, (int)lH};
	SIZE szShadow = {(int)lShadW, (int)lShadH};
	// Extra 2 lines (set to be empty) in ptAnchors are used in dilation
	int nAnchors = std::max(szParent.cy, szShadow.cy);	// # of anchor point pairs
	int (*ptAnchors)[2] = new (std::nothrow) int[nAnchors + 2][2];
	int (*ptAnchorsOri)[2] = new (std::nothrow) int[szParent.cy][2];	// anchor points, will not modify during erosion
	if (ptAnchors == nullptr || ptAnchorsOri == nullptr) {
		delete[] ptAnchors;
		delete[] ptAnchorsOri;
		DeleteObject(hParentRgn);
		return;
	}
	ptAnchors[0][0] = szParent.cx;
	ptAnchors[0][1] = 0;
	ptAnchors[nAnchors + 1][0] = szParent.cx;
	ptAnchors[nAnchors + 1][1] = 0;
	if(m_nSize > 0) {
		// Put the parent window anchors at the center
		for(int i = 0; i < m_nSize; i++) {
			ptAnchors[i + 1][0] = szParent.cx;
			ptAnchors[i + 1][1] = 0;
			ptAnchors[szShadow.cy - i][0] = szParent.cx;
			ptAnchors[szShadow.cy - i][1] = 0;
		}
		ptAnchors += m_nSize;
	}
	for(int i = 0; i < szParent.cy; i++) {
		// find start point
		int j = 0;
		for(j = 0; j < szParent.cx; j++) {
			if(PtInRegion(hParentRgn, j, i)) {
				ptAnchors[i + 1][0] = j + m_nSize;
				ptAnchorsOri[i][0] = j;
				break;
			}
		}
		// Start point not found
		if(j >= szParent.cx) {
			// Keep an empty interval in both coordinate spaces. Leaving either
			// endpoint uninitialized lets the blur loops address outside pShadBits.
			ptAnchors[i + 1][0] = szParent.cx;
			ptAnchors[i + 1][1] = 0;
			ptAnchorsOri[i][0] = szParent.cx;
			ptAnchorsOri[i][1] = 0;
		}
		else {
			// find end point
			for(j = szParent.cx - 1; j >= ptAnchors[i + 1][0]; j--) {
				if(PtInRegion(hParentRgn, j, i)) {
					ptAnchors[i + 1][1] = j + 1 + m_nSize;
					ptAnchorsOri[i][1] = j + 1;
					break;
				}
			}
		}
	}

	if(m_nSize > 0)
		ptAnchors -= m_nSize;	// Restore pos of ptAnchors for erosion
	int (*ptAnchorsTmp)[2] = new (std::nothrow) int[nAnchors + 2][2];	// Store the result of erosion
	if (ptAnchorsTmp == nullptr) {
		delete[] (ptAnchors - (m_nSize < 0 ? -m_nSize : 0));
		delete[] ptAnchorsOri;
		DeleteObject(hParentRgn);
		return;
	}
	// First and last line should be empty
	ptAnchorsTmp[0][0] = szParent.cx;
	ptAnchorsTmp[0][1] = 0;
	ptAnchorsTmp[nAnchors + 1][0] = szParent.cx;
	ptAnchorsTmp[nAnchors + 1][1] = 0;
	int nEroTimes = 0;
	// morphologic erosion
	for(int i = 0; i < m_nSharpness - m_nSize; i++) {
		nEroTimes++;
		//ptAnchorsTmp[1][0] = szParent.cx;
		//ptAnchorsTmp[1][1] = 0;
		//ptAnchorsTmp[szParent.cy + 1][0] = szParent.cx;
		//ptAnchorsTmp[szParent.cy + 1][1] = 0;
		for(int j = 1; j < nAnchors + 1; j++) {
			ptAnchorsTmp[j][0] = std::max(ptAnchors[j - 1][0], std::max(ptAnchors[j][0], ptAnchors[j + 1][0])) + 1;
			ptAnchorsTmp[j][1] = std::min(ptAnchors[j - 1][1], std::min(ptAnchors[j][1], ptAnchors[j + 1][1])) - 1;
		}
		// Exchange ptAnchors and ptAnchorsTmp;
		int (*ptAnchorsXange)[2] = ptAnchorsTmp;
		ptAnchorsTmp = ptAnchors;
		ptAnchors = ptAnchorsXange;
	}

	// morphologic dilation
	ptAnchors += (m_nSize < 0 ? -m_nSize : 0) + 1;	// now coordinates in ptAnchors are same as in shadow window
	// Generate the kernel
	int nKernelSize = m_nSize > m_nSharpness ? m_nSize : m_nSharpness;
	int nCenterSize = m_nSize > m_nSharpness ? (m_nSize - m_nSharpness) : 0;
	const size_t nKernelCells = (size_t)(2 * nKernelSize + 1) * (size_t)(2 * nKernelSize + 1);
	UINT32 *pKernel = new (std::nothrow) UINT32[nKernelCells];
	if (pKernel == nullptr) {
		delete[] (ptAnchorsTmp);
		delete[] (ptAnchors - (m_nSize < 0 ? -m_nSize : 0));
		delete[] ptAnchorsOri;
		DeleteObject(hParentRgn);
		return;
	}
	UINT32 *pKernelIter = pKernel;
	for(int i = 0; i <= 2 * nKernelSize; i++) {
		for(int j = 0; j <= 2 * nKernelSize; j++) {
			double dLength = sqrt((i - nKernelSize) * (i - nKernelSize) + (j - nKernelSize) * static_cast<double>(j - nKernelSize));
			if(dLength < nCenterSize)
				*pKernelIter = m_nDarkness << 24 | PreMultiply(m_Color, m_nDarkness);
			else if(dLength <= nKernelSize) {
				const double dSharp = (m_nSharpness > 0) ? (double)m_nSharpness : 1.0;
				double dFactor = (1.0 - (dLength - nCenterSize) / dSharp) * (double)m_nDarkness;
				if (dFactor < 0.0) dFactor = 0.0;
				if (dFactor > 255.0) dFactor = 255.0;
				UINT32 nFactor = (UINT32)dFactor;
				*pKernelIter = nFactor << 24 | PreMultiply(m_Color, nFactor);
			}
			else
				*pKernelIter = 0;
			//TRACE("%d ", *pKernelIter >> 24);
			pKernelIter ++;
		}
		//TRACE("\n");
	}
	// Generate blurred border
	for(int i = nKernelSize; i < szShadow.cy - nKernelSize; i++)
	{
		int j = 0;
		if(ptAnchors[i][0] < ptAnchors[i][1])
		{
			// Start of line
			for(j = ptAnchors[i][0]; j < std::min(std::max(ptAnchors[i - 1][0], ptAnchors[i + 1][0]) + 1, ptAnchors[i][1]); j++)
			{
				for(int k = 0; k <= 2 * nKernelSize; k++)
				{
					UINT32 *pPixel = pShadBits +
						(szShadow.cy - i - 1 + nKernelSize - k) * szShadow.cx + j - nKernelSize;
					UINT32 *pKernelPixel = pKernel + k * (2 * nKernelSize + 1);
					for(int l = 0; l <= 2 * nKernelSize; l++)
					{
						if(*pPixel < *pKernelPixel)
							*pPixel = *pKernelPixel;
						pPixel++;
						pKernelPixel++;
					}
				}
			}	// for() start of line

			// End of line
			for(j = std::max(j, std::min(ptAnchors[i - 1][1], ptAnchors[i + 1][1]) - 1); j < ptAnchors[i][1]; j++)
			{
				for(int k = 0; k <= 2 * nKernelSize; k++)
				{
					UINT32 *pPixel = pShadBits +
						(szShadow.cy - i - 1 + nKernelSize - k) * szShadow.cx + j - nKernelSize;
					UINT32 *pKernelPixel = pKernel + k * (2 * nKernelSize + 1);
					for(int l = 0; l <= 2 * nKernelSize; l++)
					{
						if(*pPixel < *pKernelPixel)
							*pPixel = *pKernelPixel;
						pPixel++;
						pKernelPixel++;
					}
				}
			}	// for() end of line

		}
	}	// for() Generate blurred border

	// Erase unwanted parts and complement missing
	UINT32 clCenter = m_nDarkness << 24 | PreMultiply(m_Color, m_nDarkness);
	for(int i = std::min(nKernelSize, std::max(m_nSize - m_nyOffset, 0)); i < std::max(szShadow.cy - nKernelSize, std::min(szParent.cy + m_nSize - m_nyOffset, szParent.cy + 2 * m_nSize)); i++) {
		UINT32 *pLine = pShadBits + (szShadow.cy - i - 1) * szShadow.cx;
		// Line is not covered by parent window 
		if(i - m_nSize + m_nyOffset < 0 || i - m_nSize + m_nyOffset >= szParent.cy)	{
			for(int j = ptAnchors[i][0]; j < ptAnchors[i][1]; j++) {
				*(pLine + j) = clCenter;
			}
		}
		else {
			for(int j = ptAnchors[i][0]; j < std::min(ptAnchorsOri[i - m_nSize + m_nyOffset][0] + m_nSize - m_nxOffset, ptAnchors[i][1]); j++)
				*(pLine + j) = clCenter;
			for(int j = std::max(ptAnchorsOri[i - m_nSize + m_nyOffset][0] + m_nSize - m_nxOffset, 0); j < std::min<LONG>(ptAnchorsOri[i - m_nSize + m_nyOffset][1] + m_nSize - m_nxOffset, szShadow.cx); j++)
				*(pLine + j) = 0;
			for(int j = std::max(ptAnchorsOri[i - m_nSize + m_nyOffset][1] + m_nSize - m_nxOffset, ptAnchors[i][0]); j < ptAnchors[i][1]; j++)
				*(pLine + j) = clCenter;
		}
	}

	// Delete used resources
	delete[] (ptAnchors - (m_nSize < 0 ? -m_nSize : 0) - 1);
	delete[] ptAnchorsTmp;
	delete[] ptAnchorsOri;
	delete[] pKernel;
	DeleteObject(hParentRgn);
}

void CShadowUI::ShowShadow(bool bShow)
{
	m_bIsShowShadow = bShow;
}

bool CShadowUI::IsShowShadow() const
{
	return m_bIsShowShadow;
}


void CShadowUI::DisableShadow(bool bDisable)
{
	m_bIsDisableShadow = bDisable;
	if (m_hWnd != nullptr) {
		if (m_bIsDisableShadow) {
			::ShowWindow(m_hWnd, SW_HIDE);
		}
		else {
			// Determine the initial show state of shadow according to parent window's state
			LONG_PTR lParentStyle = GetWindowLongPtr(GetParent(m_hWnd), GWL_STYLE);

			if (!(WS_VISIBLE & lParentStyle))	// Parent invisible
				m_Status = static_cast<BYTE>(ShadowStatus::kEnabled);
			else if ((WS_MAXIMIZE | WS_MINIMIZE) & lParentStyle)	// Parent visible but does not need shadow
				m_Status = static_cast<BYTE>(SsBits(ShadowStatus::kEnabled, ShadowStatus::kParentVisible));
			else {
				m_Status = static_cast<BYTE>(SsBits(ShadowStatus::kEnabled, ShadowStatus::kVisible, ShadowStatus::kParentVisible));
			}

			if ((WS_VISIBLE & lParentStyle) && !((WS_MAXIMIZE | WS_MINIMIZE) & lParentStyle)) {
				::ShowWindow(m_hWnd, SW_SHOWNOACTIVATE);
				Update(GetParent(m_hWnd));
			}
		}
	}
}

////TODO shadow disnable fix////
bool CShadowUI::IsDisableShadow() const 
{
	return m_bIsDisableShadow;
}

bool CShadowUI::SetSize(int NewSize)
{
	if(NewSize > 35 || NewSize < -35)
		return false;

	m_nSize = (signed char)NewSize;
	if(m_hWnd != nullptr && (static_cast<int>(ShadowStatus::kVisible) & m_Status))
		Update(GetParent(m_hWnd));
	return true;
}

bool CShadowUI::SetSharpness(unsigned int NewSharpness)
{
	if(NewSharpness > 35)
		return false;

	m_nSharpness = (unsigned char)NewSharpness;
	if(m_hWnd != nullptr && (static_cast<int>(ShadowStatus::kVisible) & m_Status))
		Update(GetParent(m_hWnd));
	return true;
}

bool CShadowUI::SetDarkness(unsigned int NewDarkness)
{
	if(NewDarkness > 255)
		return false;

	m_nDarkness = (unsigned char)NewDarkness;
	if(m_hWnd != nullptr && (static_cast<int>(ShadowStatus::kVisible) & m_Status))
		Update(GetParent(m_hWnd));
	return true;
}

bool CShadowUI::SetPosition(int NewXOffset, int NewYOffset)
{
	if(NewXOffset > 35 || NewXOffset < -35 ||
		NewYOffset > 35 || NewYOffset < -35)
		return false;
	
	m_nxOffset = (signed char)NewXOffset;
	m_nyOffset = (signed char)NewYOffset;
	if(m_hWnd != nullptr && (static_cast<int>(ShadowStatus::kVisible) & m_Status))
		Update(GetParent(m_hWnd));

	return true;
}

bool CShadowUI::SetColor(COLORREF NewColor)
{
	m_Color = NewColor;
	if(m_hWnd != nullptr && (static_cast<int>(ShadowStatus::kVisible) & m_Status))
		Update(GetParent(m_hWnd));

	return true;
}

bool CShadowUI::SetImage(LPCTSTR szImage)
{
	if (szImage == nullptr)
		return false;

	m_bIsImageMode = true;
	m_sShadowImage = szImage;
	if(m_hWnd != nullptr && (static_cast<int>(ShadowStatus::kVisible) & m_Status))
		Update(GetParent(m_hWnd));

	return true;
}

bool CShadowUI::SetShadowCorner(RECT rcCorner)
{
	if (rcCorner.left < 0 || rcCorner.top < 0 || rcCorner.right < 0 || rcCorner.bottom < 0) return false;

	m_rcShadowCorner = rcCorner;
	if(m_hWnd != nullptr && (static_cast<int>(ShadowStatus::kVisible) & m_Status)) {
		Update(GetParent(m_hWnd));
	}

	return true;
}

bool CShadowUI::CopyShadow(CShadowUI* pShadow)
{
	if (m_bIsImageMode) {
		pShadow->SetImage(m_sShadowImage);
		pShadow->SetShadowCorner(m_rcShadowCorner);
		pShadow->SetSize(static_cast<int>(m_nSize));
	}
	else {
		pShadow->SetSize(static_cast<int>(m_nSize));
		pShadow->SetSharpness(static_cast<unsigned int>(m_nSharpness));
		pShadow->SetDarkness(static_cast<unsigned int>(m_nDarkness));
		pShadow->SetColor(m_Color);
		pShadow->SetPosition(static_cast<int>(m_nxOffset), static_cast<int>(m_nyOffset));
	}

	pShadow->ShowShadow(m_bIsShowShadow);
	return true;
}
} //namespace DuiLib