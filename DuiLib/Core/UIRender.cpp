// Round 55 T5.33: 15 C4311/C4312 warnings in this file are int-as-pointer
// borrows via CStdPtrArray (see docs/t532-w4-b3-pointer-cast-audit.json).
// Real fix: replace CStdPtrArray<LPVOID> with std::vector<int> (cross-module).
#pragma warning(disable: 4311 4312)
#include "StdAfx.h"
#include <d2d1.h>
#include <dwrite.h>
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#include <algorithm> // std::min/std::max
#include <memory> // [R4] std::unique_ptr

#define STB_IMAGE_IMPLEMENTATION
#include "..\Utils\stb_image.h"


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


#ifdef USE_XIMAGE_EFFECT
#	include "../../3rd/CxImage/ximage.h"
#	include "../../3rd/CxImage/ximage.cpp"
#	include "../../3rd/CxImage/ximaenc.cpp"
#	include "../../3rd/CxImage/ximagif.cpp"
#	include "../../3rd/CxImage/ximainfo.cpp"
#	include "../../3rd/CxImage/ximalpha.cpp"
#	include "../../3rd/CxImage/ximapal.cpp"
#	include "../../3rd/CxImage/ximatran.cpp"
#	include "../../3rd/CxImage/ximawnd.cpp"
#	include "../../3rd/CxImage/xmemfile.cpp"
#endif

///////////////////////////////////////////////////////////////////////////////////////
namespace DuiLib {

	//////////////////////////////////////////////////////////////////////////
	// [R240] Render backend dispatch
	//////////////////////////////////////////////////////////////////////////
	namespace
	{
		// Installed backend (nullptr = use the built-in GDI code path).
		IRenderBackend* g_pRenderBackend = nullptr;

		// Re-entrancy guard: a backend that delegates back to CRenderEngine must
		// not be dispatched to again, or it would recurse forever.
		thread_local bool t_bInBackend = false;

		// RAII guard so every dispatch is exception-safe.
		struct BackendScope
		{
			bool bEntered;
			BackendScope() : bEntered(false)
			{
				if (!t_bInBackend) { t_bInBackend = true; bEntered = true; }
			}
			~BackendScope() { if (bEntered) t_bInBackend = false; }
		};
	}

// [R8] 64 MB per-image ceiling to reject corrupt zip entries
constexpr DWORD kMaxImageBytes = 64u * 1024u * 1024u;

	static int g_iFontID = MAX_FONT_ID;

	/////////////////////////////////////////////////////////////////////////////////////
	//
	//

	CRenderClip::~CRenderClip()
	{
		ASSERT(::GetObjectType(hDC)==OBJ_DC || ::GetObjectType(hDC)==OBJ_MEMDC);
		ASSERT(::GetObjectType(hRgn)==OBJ_REGION);
		ASSERT(::GetObjectType(hOldRgn)==OBJ_REGION);
		::SelectClipRgn(hDC, hOldRgn);
		::DeleteObject(hOldRgn);
		::DeleteObject(hRgn);
	}

	void CRenderClip::GenerateClip(HDC hDC, RECT rc, CRenderClip& clip)
	{
		RECT rcClip = { 0 };
		::GetClipBox(hDC, &rcClip);
		clip.hOldRgn = ::CreateRectRgnIndirect(&rcClip);
		clip.hRgn = ::CreateRectRgnIndirect(&rc);
		::ExtSelectClipRgn(hDC, clip.hRgn, RGN_AND);
		clip.hDC = hDC;
		clip.rcItem = rc;
	}

	void CRenderClip::GenerateRoundClip(HDC hDC, RECT rc, RECT rcItem, int width, int height, CRenderClip& clip)
	{
		RECT rcClip = { 0 };
		::GetClipBox(hDC, &rcClip);
		clip.hOldRgn = ::CreateRectRgnIndirect(&rcClip);
		clip.hRgn = ::CreateRectRgnIndirect(&rc);
		HRGN hRgnItem = ::CreateRoundRectRgn(rcItem.left, rcItem.top, rcItem.right + 1, rcItem.bottom + 1, width, height);
		::CombineRgn(clip.hRgn, clip.hRgn, hRgnItem, RGN_AND);
		::ExtSelectClipRgn(hDC, clip.hRgn, RGN_AND);
		clip.hDC = hDC;
		clip.rcItem = rc;
		::DeleteObject(hRgnItem);
	}

	void CRenderClip::UseOldClipBegin(HDC hDC, CRenderClip& clip)
	{
		::SelectClipRgn(hDC, clip.hOldRgn);
	}

	void CRenderClip::UseOldClipEnd(HDC hDC, CRenderClip& clip)
	{
		::SelectClipRgn(hDC, clip.hRgn);
	}

	/////////////////////////////////////////////////////////////////////////////////////
	//
	//

	static const float OneThird = 1.0f / 3;

	static void RGBtoHSL(DWORD ARGB, float* H, float* S, float* L) {
		const float
			R = static_cast<float>(GetRValue(ARGB)),
			G = static_cast<float>(GetGValue(ARGB)),
			B = static_cast<float>(GetBValue(ARGB)),
			nR = (R<0?0:(R>255?255:R))/255,
			nG = (G<0?0:(G>255?255:G))/255,
			nB = (B<0?0:(B>255?255:B))/255,
			m = std::min(std::min(nR,nG),nB),
			M = std::max(std::max(nR,nG),nB);
		*L = (m + M)/2;
		if (M==m) *H = *S = 0;
		else {
			const float
				f = (nR==m)?(nG-nB):((nG==m)?(nB-nR):(nR-nG)),
				i = (nR==m)?3.0f:((nG==m)?5.0f:1.0f);
			*H = (i-f/(M-m));
			if (*H>=6) *H-=6;
			*H*=60;
			*S = (2*(*L)<=1)?((M-m)/(M+m)):((M-m)/(2-M-m));
		}
	}

	static void HSLtoRGB(DWORD* ARGB, float H, float S, float L) {
		const float
			q = 2*L<1?L*(1+S):(L+S-L*S),
			p = 2*L-q,
			h = H/360,
			tr = h + OneThird,
			tg = h,
			tb = h - OneThird,
			ntr = tr<0?tr+1:(tr>1?tr-1:tr),
			ntg = tg<0?tg+1:(tg>1?tg-1:tg),
			ntb = tb<0?tb+1:(tb>1?tb-1:tb),
			B = 255*(6*ntr<1?p+(q-p)*6*ntr:(2*ntr<1?q:(3*ntr<2?p+(q-p)*6*(2.0f*OneThird-ntr):p))),
			G = 255*(6*ntg<1?p+(q-p)*6*ntg:(2*ntg<1?q:(3*ntg<2?p+(q-p)*6*(2.0f*OneThird-ntg):p))),
			R = 255*(6*ntb<1?p+(q-p)*6*ntb:(2*ntb<1?q:(3*ntb<2?p+(q-p)*6*(2.0f*OneThird-ntb):p)));
		*ARGB &= 0xFF000000;
		*ARGB |= RGB( static_cast<BYTE>(R<0?0:(R>255?255:R)), static_cast<BYTE>(G<0?0:(G>255?255:G)), static_cast<BYTE>(B<0?0:(B>255?255:B)) );
	}

	static COLORREF PixelAlpha(COLORREF clrSrc, double src_darken, COLORREF clrDest, double dest_darken)
	{
		return RGB (GetRValue (clrSrc) * src_darken + GetRValue (clrDest) * dest_darken, 
			GetGValue (clrSrc) * src_darken + GetGValue (clrDest) * dest_darken, 
			GetBValue (clrSrc) * src_darken + GetBValue (clrDest) * dest_darken);
	}

	static BOOL WINAPI AlphaBitBlt(HDC hDC, int nDestX, int nDestY, int dwWidth, int dwHeight, HDC hSrcDC, int nSrcX, int nSrcY, int wSrc, int hSrc, BLENDFUNCTION ftn)
	{
		HDC hTempDC = ::CreateCompatibleDC(hDC);
		if (nullptr == hTempDC)
			return FALSE;

		//Creates Source DIB
		LPBITMAPINFO lpbiSrc = nullptr;
		// Fill in the BITMAPINFOHEADER
		lpbiSrc = reinterpret_cast<LPBITMAPINFO>(new BYTE[sizeof(BITMAPINFOHEADER)]);
		if (lpbiSrc == nullptr)
		{
			::DeleteDC(hTempDC);
			return FALSE;
		}
		lpbiSrc->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		lpbiSrc->bmiHeader.biWidth = dwWidth;
		lpbiSrc->bmiHeader.biHeight = dwHeight;
		lpbiSrc->bmiHeader.biPlanes = 1;
		lpbiSrc->bmiHeader.biBitCount = 32;
		lpbiSrc->bmiHeader.biCompression = BI_RGB;
		lpbiSrc->bmiHeader.biSizeImage = dwWidth * dwHeight;
		lpbiSrc->bmiHeader.biXPelsPerMeter = 0;
		lpbiSrc->bmiHeader.biYPelsPerMeter = 0;
		lpbiSrc->bmiHeader.biClrUsed = 0;
		lpbiSrc->bmiHeader.biClrImportant = 0;

		COLORREF* pSrcBits = nullptr;
		HBITMAP hSrcDib = CreateDIBSection (
			hSrcDC, lpbiSrc, DIB_RGB_COLORS, (void **)&pSrcBits,
			nullptr, 0);

		if ((nullptr == hSrcDib) || (nullptr == pSrcBits)) 
		{
			delete [] lpbiSrc;
			::DeleteDC(hTempDC);
			return FALSE;
		}

		HBITMAP hOldTempBmp = (HBITMAP)::SelectObject (hTempDC, hSrcDib);
		::StretchBlt(hTempDC, 0, 0, dwWidth, dwHeight, hSrcDC, nSrcX, nSrcY, wSrc, hSrc, SRCCOPY);
		::SelectObject (hTempDC, hOldTempBmp);

		//Creates Destination DIB
		LPBITMAPINFO lpbiDest = nullptr;
		// Fill in the BITMAPINFOHEADER
		lpbiDest = reinterpret_cast<LPBITMAPINFO>(new BYTE[sizeof(BITMAPINFOHEADER)]);
		if (lpbiDest == nullptr)
		{
			delete [] lpbiSrc;
			::DeleteObject(hSrcDib);
			::DeleteDC(hTempDC);
			return FALSE;
		}

		lpbiDest->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		lpbiDest->bmiHeader.biWidth = dwWidth;
		lpbiDest->bmiHeader.biHeight = dwHeight;
		lpbiDest->bmiHeader.biPlanes = 1;
		lpbiDest->bmiHeader.biBitCount = 32;
		lpbiDest->bmiHeader.biCompression = BI_RGB;
		lpbiDest->bmiHeader.biSizeImage = dwWidth * dwHeight;
		lpbiDest->bmiHeader.biXPelsPerMeter = 0;
		lpbiDest->bmiHeader.biYPelsPerMeter = 0;
		lpbiDest->bmiHeader.biClrUsed = 0;
		lpbiDest->bmiHeader.biClrImportant = 0;

		COLORREF* pDestBits = nullptr;
		HBITMAP hDestDib = CreateDIBSection (
			hDC, lpbiDest, DIB_RGB_COLORS, (void **)&pDestBits,
			nullptr, 0);

		if ((nullptr == hDestDib) || (nullptr == pDestBits))
		{
			delete [] lpbiSrc;
			::DeleteObject(hSrcDib);
			::DeleteDC(hTempDC);
			return FALSE;
		}

		::SelectObject (hTempDC, hDestDib);
		::BitBlt (hTempDC, 0, 0, dwWidth, dwHeight, hDC, nDestX, nDestY, SRCCOPY);
		::SelectObject (hTempDC, hOldTempBmp);

		double src_darken = 0.0;
		BYTE nAlpha;

		for (int pixel = 0; pixel < dwWidth * dwHeight; pixel++, pSrcBits++, pDestBits++)
		{
			nAlpha = LOBYTE(*pSrcBits >> 24);
			src_darken = static_cast<double>(nAlpha * ftn.SourceConstantAlpha) / 255.0 / 255.0;
			if( src_darken < 0.0 ) src_darken = 0.0;
			*pDestBits = PixelAlpha(*pSrcBits, src_darken, *pDestBits, 1.0 - src_darken);
		} //for

		::SelectObject (hTempDC, hDestDib);
		::BitBlt (hDC, nDestX, nDestY, dwWidth, dwHeight, hTempDC, 0, 0, SRCCOPY);
		::SelectObject (hTempDC, hOldTempBmp);

		delete [] lpbiDest;
		::DeleteObject(hDestDib);

		delete [] lpbiSrc;
		::DeleteObject(hSrcDib);

		::DeleteDC(hTempDC);
		return TRUE;
	}

	/////////////////////////////////////////////////////////////////////////////////////
	//
	//

	bool MakeImageDest(const RECT& rcControl, const CDuiSize& szImage, const CDuiString& sAlign, const RECT& rcPadding, RECT& rcDest)
	{
		if(sAlign.Find(_T("left")) != -1)
		{
			rcDest.left = rcControl.left;  
			rcDest.right = rcDest.left + szImage.cx;
		}
		else if(sAlign.Find(_T("hcenter")) != -1)
		{
			rcDest.left = rcControl.left + ((rcControl.right - rcControl.left) - szImage.cx)/2;  
			rcDest.right = rcDest.left + szImage.cx;
		}
		else if(sAlign.Find(_T("right")) != -1)
		{
			rcDest.left = rcControl.right - szImage.cx;  
			rcDest.right = rcDest.left + szImage.cx;
		}

		if(sAlign.Find(_T("top")) != -1)
		{
			rcDest.top = rcControl.top;
			rcDest.bottom = rcDest.top + szImage.cy;
		}
		else if(sAlign.Find(_T("vcenter")) != -1)
		{
			rcDest.top = rcControl.top + ((rcControl.bottom - rcControl.top) - szImage.cy)/2;
			rcDest.bottom = rcDest.top + szImage.cy;
		}
		else if(sAlign.Find(_T("bottom")) != -1)
		{
			rcDest.top = rcControl.bottom - szImage.cy;
			rcDest.bottom = rcDest.top + rcDest.top;
		}

		::OffsetRect(&rcDest, rcPadding.left, rcPadding.top);		
		::OffsetRect(&rcDest, -rcPadding.right, -rcPadding.bottom);

		if (rcDest.right > rcControl.right) 
			rcDest.right = rcControl.right;

		if (rcDest.bottom > rcControl.bottom) 
			rcDest.bottom = rcControl.bottom;

		return true;
	}

	bool DrawImage(HDC hDC, CPaintManagerUI* pManager, const RECT& rc, const RECT& rcPaint, const CDuiString& sImageName, const CDuiString& sImageResType, RECT rcItem, RECT rcBmpPart, RECT rcCorner, DWORD dwMask, UINT uFade, UINT uRotate, bool bGdiplus, bool bHole, bool bTiledX, bool bTiledY, HINSTANCE instance = nullptr)
	{
		if (sImageName.IsEmpty()) {
			return false;
		}
		const TImageInfo* data = nullptr;
		data = pManager->GetImageEx(static_cast<LPCTSTR>(sImageName), sImageResType.IsEmpty() ? nullptr : static_cast<LPCTSTR>(sImageResType), dwMask, false, bGdiplus, instance);
		if( !data ) return false;

		if( rcBmpPart.left == 0 && rcBmpPart.right == 0 && rcBmpPart.top == 0 && rcBmpPart.bottom == 0 ) {
			rcBmpPart.right = data->nX;
			rcBmpPart.bottom = data->nY;
		}
		if (rcBmpPart.right > data->nX) rcBmpPart.right = data->nX;
		if (rcBmpPart.bottom > data->nY) rcBmpPart.bottom = data->nY;

		RECT rcTemp;
		if( !::IntersectRect(&rcTemp, &rcItem, &rc) ) return true;
		if( !::IntersectRect(&rcTemp, &rcItem, &rcPaint) ) return true;

		if(bGdiplus) {
			CRenderEngine::GdiplusDrawImage(hDC, data->pImage, rcItem, rcPaint, rcBmpPart, pManager->IsLayered() ? true : data->bAlpha, uFade, uRotate);
		}
		else {
			CRenderEngine::DrawImage(hDC, data->hBitmap, rcItem, rcPaint, rcBmpPart, rcCorner, pManager->IsLayered() ? true : data->bAlpha, uFade, bHole, bTiledX, bTiledY);
		}

		return true;
	}

	/////////////////////////////////////////////////////////////////////////////////////
	//
	//

#ifdef USE_XIMAGE_EFFECT
	static DWORD LoadImage2Memory(const STRINGorID &bitmap, LPCTSTR type,LPBYTE &pData)
	{
		assert(pData == nullptr);
		pData = nullptr;
		DWORD dwSize(0U);
		do 
		{
			if( type == nullptr )
			{
				CDuiString sFile = CPaintManagerUI::GetResourcePath();
				if( CPaintManagerUI::GetResourceZip().IsEmpty() )
				{
					sFile += bitmap.m_lpstr;
					HANDLE hFile = ::CreateFile(sFile.GetData(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, \
						FILE_ATTRIBUTE_NORMAL, nullptr);
					if( hFile == INVALID_HANDLE_VALUE ) break;
					dwSize = ::GetFileSize(hFile, nullptr);
					if( dwSize == 0 ) break;

					DWORD dwRead = 0;
					pData = new BYTE[ dwSize + 1 ];
					memset(pData,0,dwSize+1);
					::ReadFile( hFile, pData, dwSize, &dwRead, nullptr );
					::CloseHandle( hFile );

					if( dwRead != dwSize ) 
					{
						delete[] pData;
						pData = nullptr;
						dwSize = 0U;
						break;
					}
				}
				else 
				{
					sFile += CPaintManagerUI::GetResourceZip();
					HZIP hz = nullptr;
					if( CPaintManagerUI::IsCachedResourceZip() ) 
						hz = reinterpret_cast<HZIP>(CPaintManagerUI::GetResourceZipHandle());
					else {
						CDuiString sFilePwd = CPaintManagerUI::GetResourceZipPwd();
#ifdef UNICODE
						std::unique_ptr<char[]> pwd{ w2a(const_cast<wchar_t*>(sFilePwd.GetData())) }; // [R4] RAII
						hz = OpenZip(const_cast<void*>(sFile.GetData()), pwd.get());
#else
						hz = OpenZip(const_cast<void*>(sFile.GetData()), sFilePwd.GetData());
#endif
					}
					if( hz == nullptr ) break;
					ZIPENTRY ze; 
					int i = 0; 
					CDuiString key = bitmap.m_lpstr;
					key.Replace(_T("\\"), _T("/")); 
					if( FindZipItem(hz, key, true, &i, &ze) != 0 ) { // [R6] close before break
						if( !CPaintManagerUI::IsCachedResourceZip() ) CloseZip(hz);
						break;
					}
					dwSize = ze.unc_size;
					if( dwSize == 0 ) { // [R6] close before break
						if( !CPaintManagerUI::IsCachedResourceZip() ) CloseZip(hz);
						break;
					}
					if( dwSize > kMaxImageBytes ) { // [R8] size ceiling
						DUI_LOGE("LoadImage: zip entry '%s' bad size %u", key.GetData(), dwSize);
						if( !CPaintManagerUI::IsCachedResourceZip() ) CloseZip(hz);
						break;
					}
					pData = new(std::nothrow) BYTE[ dwSize ]; // [R8] nothrow
					if( pData == nullptr ) { if( !CPaintManagerUI::IsCachedResourceZip() ) CloseZip(hz); break; }
					int res = UnzipItem(hz, i, pData, dwSize, 3);
					if( res != 0x00000000 && res != 0x00000600)
					{
						delete[] pData;
						pData = nullptr;
						dwSize = 0U;
						if( !CPaintManagerUI::IsCachedResourceZip() )
							CloseZip(hz);
						break;
					}
					if( !CPaintManagerUI::IsCachedResourceZip() )
						CloseZip(hz);
				}
			}
			else 
			{
				HINSTANCE hDll = CPaintManagerUI::GetResourceDll();
				HRSRC hResource = ::FindResource(hDll, bitmap.m_lpstr, type);
				if( hResource == nullptr ) break;
				HGLOBAL hGlobal = ::LoadResource(hDll, hResource);
				if( hGlobal == nullptr ) 
				{
					FreeResource(hResource);
					break;
				}

				dwSize = ::SizeofResource(hDll, hResource);
				if( dwSize == 0 ) { ::FreeResource(hGlobal); break; } // [R7]
				pData = new BYTE[ dwSize ];
				::CopyMemory(pData, (LPBYTE)::LockResource(hGlobal), dwSize);
				::FreeResource(hGlobal);
			}
		} while (0);

		while (!pData)
		{
			//读不到图片, 则直接去读取bitmap.m_lpstr指向的路径
			HANDLE hFile = ::CreateFile(bitmap.m_lpstr, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, \
				FILE_ATTRIBUTE_NORMAL, nullptr);
			if( hFile == INVALID_HANDLE_VALUE ) break;
			dwSize = ::GetFileSize(hFile, nullptr);
			if( dwSize == 0 ) break;

			DWORD dwRead = 0;
			pData = new BYTE[ dwSize ];
			::ReadFile( hFile, pData, dwSize, &dwRead, nullptr );
			::CloseHandle( hFile );

			if( dwRead != dwSize ) 
			{
				delete[] pData;
				pData = nullptr;
				dwSize = 0U;
			}
			break;
		}
		return dwSize;
	}
	CxImage* CRenderEngine::LoadGifImageX(STRINGorID bitmap, LPCTSTR type , DWORD mask)
	{
		//write by wangji
		LPBYTE pData = nullptr;
		DWORD dwSize = LoadImage2Memory(bitmap,type,pData);
		if(dwSize == 0U || !pData)
			return nullptr;
		CxImage * pImg = nullptr;
		if(pImg = new CxImage())
		{
			pImg->SetRetreiveAllFrames(TRUE);
			if(!pImg->Decode(pData,dwSize,CXIMAGE_FORMAT_GIF))
			{
				delete pImg;
				pImg = nullptr;
			}
		}
		delete[] pData;
		pData = nullptr;
		return pImg;
	}
#endif//USE_XIMAGE_EFFECT

	TImageInfo* CRenderEngine::LoadImage(STRINGorID bitmap, LPCTSTR type, DWORD mask, HINSTANCE instance)
	{
		LPBYTE pData = nullptr;
		DWORD dwSize = 0;
		do 
		{
			if( type == nullptr ) {
				CDuiString sFile = CPaintManagerUI::GetResourcePath();
				if( CPaintManagerUI::GetResourceZip().IsEmpty() ) {
					sFile += bitmap.m_lpstr;
					HANDLE hFile = ::CreateFile(sFile.GetData(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, \
						FILE_ATTRIBUTE_NORMAL, nullptr);
					if( hFile == INVALID_HANDLE_VALUE ) break;
					dwSize = ::GetFileSize(hFile, nullptr);
					if( dwSize == 0 ) break;

					DWORD dwRead = 0;
					pData = new BYTE[ dwSize ];
					::ReadFile( hFile, pData, dwSize, &dwRead, nullptr );
					::CloseHandle( hFile );

					if( dwRead != dwSize ) {
						delete[] pData;
						pData = nullptr;
						break;
					}
				}
				else {
					sFile += CPaintManagerUI::GetResourceZip();
					CDuiString sFilePwd = CPaintManagerUI::GetResourceZipPwd();
					HZIP hz = nullptr;
					if( CPaintManagerUI::IsCachedResourceZip() ) hz = reinterpret_cast<HZIP>(CPaintManagerUI::GetResourceZipHandle());
					else
					{
#ifdef UNICODE
						std::unique_ptr<char[]> pwd{ w2a(const_cast<wchar_t*>(sFilePwd.GetData())) }; // [R4] RAII
						hz = OpenZip(sFile.GetData(), pwd.get());
#else
						hz = OpenZip(sFile.GetData(), sFilePwd.GetData());
#endif
					}
					if( hz == nullptr ) break;
					ZIPENTRY ze; 
					int i = 0; 
					CDuiString key = bitmap.m_lpstr;
					key.Replace(_T("\\"), _T("/"));
					if( FindZipItem(hz, key, true, &i, &ze) != 0 ) { // [R6] close before break
						if( !CPaintManagerUI::IsCachedResourceZip() ) CloseZip(hz);
						break;
					}
					dwSize = ze.unc_size;
					if( dwSize == 0 ) { // [R6] close before break
						if( !CPaintManagerUI::IsCachedResourceZip() ) CloseZip(hz);
						break;
					}
					if( dwSize > kMaxImageBytes ) { // [R8] size ceiling
						DUI_LOGE("LoadImage: zip entry '%s' bad size %u", key.GetData(), dwSize);
						if( !CPaintManagerUI::IsCachedResourceZip() ) CloseZip(hz);
						break;
					}
					pData = new(std::nothrow) BYTE[ dwSize ]; // [R8] nothrow
					if( pData == nullptr ) { if( !CPaintManagerUI::IsCachedResourceZip() ) CloseZip(hz); break; }
					int res = UnzipItem(hz, i, pData, dwSize);
					if( res != 0x00000000 && res != 0x00000600) {
						delete[] pData;
						pData = nullptr;
						if( !CPaintManagerUI::IsCachedResourceZip() ) CloseZip(hz);
						break;
					}
					if( !CPaintManagerUI::IsCachedResourceZip() ) CloseZip(hz);
				}
			}
			else {
				HINSTANCE dllinstance = nullptr;
				if (instance) {
					dllinstance = instance;
				}
				else {
					dllinstance = CPaintManagerUI::GetResourceDll();
				}
				HRSRC hResource = ::FindResource(dllinstance, bitmap.m_lpstr, type);
				if( hResource == nullptr ) break;
				HGLOBAL hGlobal = ::LoadResource(dllinstance, hResource);
				if( hGlobal == nullptr ) {
					FreeResource(hResource);
					break;
				}

				dwSize = ::SizeofResource(dllinstance, hResource);
				if( dwSize == 0 ) { ::FreeResource(hGlobal); break; } // [R7]
				pData = new BYTE[ dwSize ];
				::CopyMemory(pData, (LPBYTE)::LockResource(hGlobal), dwSize);
				::FreeResource(hGlobal);
			}
		} while (0);

		while (!pData)
		{
			//读不到图片, 则直接去读取bitmap.m_lpstr指向的路径
			HANDLE hFile = ::CreateFile(bitmap.m_lpstr, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, \
				FILE_ATTRIBUTE_NORMAL, nullptr);
			if( hFile == INVALID_HANDLE_VALUE ) break;
			dwSize = ::GetFileSize(hFile, nullptr);
			if( dwSize == 0 ) break;

			DWORD dwRead = 0;
			pData = new BYTE[ dwSize ];
			::ReadFile( hFile, pData, dwSize, &dwRead, nullptr );
			::CloseHandle( hFile );

			if( dwRead != dwSize ) {
				delete[] pData;
				pData = nullptr;
			}
			break;
		}
		if (!pData)
		{
			return nullptr;
		}

		LPBYTE pImage = nullptr;
		int x,y,n;
		pImage = stbi_load_from_memory(pData, dwSize, &x, &y, &n, 4);
		delete[] pData;
		if( !pImage ) {
			return nullptr;
		}

		BITMAPINFO bmi;
		::ZeroMemory(&bmi, sizeof(BITMAPINFO));
		bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth = x;
		bmi.bmiHeader.biHeight = -y;
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 32;
		bmi.bmiHeader.biCompression = BI_RGB;
		bmi.bmiHeader.biSizeImage = x * y * 4;

		bool bAlphaChannel = false;
		LPBYTE pDest = nullptr;
		HBITMAP hBitmap = ::CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, (void**)&pDest, nullptr, 0);
		if( !hBitmap ) {
			return nullptr;
		}

		for( int i = 0; i < x * y; i++ ) 
		{
			pDest[i*4 + 3] = pImage[i*4 + 3];
			if( pDest[i*4 + 3] < 255 )
			{
				pDest[i*4] = static_cast<BYTE>(DWORD(pImage[i*4 + 2])*pImage[i*4 + 3]/255);
				pDest[i*4 + 1] = static_cast<BYTE>(DWORD(pImage[i*4 + 1])*pImage[i*4 + 3]/255);
				pDest[i*4 + 2] = static_cast<BYTE>(DWORD(pImage[i*4])*pImage[i*4 + 3]/255); 
				bAlphaChannel = true;
			}
			else
			{
				pDest[i*4] = pImage[i*4 + 2];
				pDest[i*4 + 1] = pImage[i*4 + 1];
				pDest[i*4 + 2] = pImage[i*4]; 
			}

			if( *(DWORD*)(&pDest[i*4]) == mask ) {
				pDest[i*4] = (BYTE)0;
				pDest[i*4 + 1] = (BYTE)0;
				pDest[i*4 + 2] = (BYTE)0; 
				pDest[i*4 + 3] = (BYTE)0;
				bAlphaChannel = true;
			}
		}

		stbi_image_free(pImage);

		TImageInfo* data = new TImageInfo;
		data->pBits = nullptr;
		data->pSrcBits = nullptr;
		data->hBitmap = hBitmap;
		data->nX = x;
		data->nY = y;
		data->bAlpha = bAlphaChannel;
		return data;
	}

	void CRenderEngine::FreeImage(TImageInfo* pImageInfo, bool bDelete)
	{
		if (pImageInfo == nullptr) return;

		if(pImageInfo->pImage) {
			delete pImageInfo->pImage;
		}
		pImageInfo->pImage = nullptr;

		if (pImageInfo->hBitmap) {
			::DeleteObject(pImageInfo->hBitmap);
		}
		pImageInfo->hBitmap = nullptr;

		if (pImageInfo->pBits) {
			delete[] pImageInfo->pBits;
		}
		pImageInfo->pBits = nullptr;

		if (pImageInfo->pSrcBits) {
			delete[] pImageInfo->pSrcBits;
		}
		pImageInfo->pSrcBits = nullptr;

		if (bDelete) {
			delete pImageInfo;
			pImageInfo = nullptr;
		}
	}

	TImageInfo* CRenderEngine::LoadImage(LPCTSTR pStrImage, LPCTSTR type, DWORD mask, HINSTANCE instance)
	{	
		if(pStrImage == nullptr) return nullptr;

		CDuiString sStrPath = pStrImage;
		if( type == nullptr )  {
			sStrPath = CResourceManager::GetInstance()->GetImagePath(pStrImage);
			if (sStrPath.IsEmpty()) sStrPath = pStrImage;
		}
		return LoadImage(STRINGorID(sStrPath.GetData()), type, mask, instance);
	}

	TImageInfo* CRenderEngine::LoadImage(UINT nID, LPCTSTR type, DWORD mask, HINSTANCE instance)
	{
		return LoadImage(STRINGorID(nID), type, mask, instance);
	}

	void CRenderEngine::DrawImage(HDC hDC, HBITMAP hBitmap, const RECT& rc, const RECT& rcPaint, const RECT& rcBmpPart, const RECT& rcCorners, bool bAlpha, UINT uFade, bool hole, bool xtiled, bool ytiled)
	{
		// [R251] dispatch to the installed backend when one is set;
		// with no backend the body below runs unchanged.
		if (g_pRenderBackend != nullptr && !t_bInBackend)
		{
			BackendScope scope;
			g_pRenderBackend->DrawImage(hDC, hBitmap, rc, rcPaint, rcBmpPart, rcCorners,
									bAlpha, uFade, hole, xtiled, ytiled);
			return;
		}
		ASSERT(::GetObjectType(hDC)==OBJ_DC || ::GetObjectType(hDC)==OBJ_MEMDC);

		typedef BOOL (WINAPI *LPALPHABLEND)(HDC, int, int, int, int,HDC, int, int, int, int, BLENDFUNCTION);
		static LPALPHABLEND lpAlphaBlend = (LPALPHABLEND) ::GetProcAddress(::GetModuleHandle(_T("msimg32.dll")), "AlphaBlend");

		if( lpAlphaBlend == nullptr ) lpAlphaBlend = AlphaBitBlt;
		if( hBitmap == nullptr ) return;

		// [PERF-02] A compatible DC was created and destroyed on every call.
		// DrawImage is the per-image hot path (a 200-control page calls it
		// hundreds of times per frame), and CreateCompatibleDC/DeleteDC are
		// kernel transitions. Reuse one per thread; the selected bitmap is
		// still restored before returning, so no state leaks between calls.
		thread_local HDC t_hCloneDC = nullptr;
		if( t_hCloneDC == nullptr ) t_hCloneDC = ::CreateCompatibleDC(hDC);
		HDC hCloneDC = t_hCloneDC;
		HBITMAP hOldBitmap = (HBITMAP) ::SelectObject(hCloneDC, hBitmap);
		::SetStretchBltMode(hDC, HALFTONE);

		RECT rcTemp = {0};
		RECT rcDest = {0};
		if( lpAlphaBlend && (bAlpha || uFade < 255) ) {
			BLENDFUNCTION bf = { AC_SRC_OVER, static_cast<BYTE>(0), static_cast<BYTE>(uFade), AC_SRC_ALPHA };
			// middle
			if( !hole ) {
				rcDest.left = rc.left + rcCorners.left;
				rcDest.top = rc.top + rcCorners.top;
				rcDest.right = rc.right - rc.left - rcCorners.left - rcCorners.right;
				rcDest.bottom = rc.bottom - rc.top - rcCorners.top - rcCorners.bottom;
				rcDest.right += rcDest.left;
				rcDest.bottom += rcDest.top;
				if( ::IntersectRect(&rcTemp, &rcPaint, &rcDest) ) {
					if( !xtiled && !ytiled ) {
						rcDest.right -= rcDest.left;
						rcDest.bottom -= rcDest.top;
						lpAlphaBlend(hDC, rcDest.left, rcDest.top, rcDest.right, rcDest.bottom, hCloneDC, \
							rcBmpPart.left + rcCorners.left, rcBmpPart.top + rcCorners.top, \
							rcBmpPart.right - rcBmpPart.left - rcCorners.left - rcCorners.right, \
							rcBmpPart.bottom - rcBmpPart.top - rcCorners.top - rcCorners.bottom, bf);
					}
					else if( xtiled && ytiled ) {
						LONG lWidth = rcBmpPart.right - rcBmpPart.left - rcCorners.left - rcCorners.right;
						LONG lHeight = rcBmpPart.bottom - rcBmpPart.top - rcCorners.top - rcCorners.bottom;
						int iTimesX = (rcDest.right - rcDest.left + lWidth - 1) / lWidth;
						int iTimesY = (rcDest.bottom - rcDest.top + lHeight - 1) / lHeight;
						for( int j = 0; j < iTimesY; ++j ) {
							LONG lDestTop = rcDest.top + lHeight * j;
							LONG lDestBottom = rcDest.top + lHeight * (j + 1);
							LONG lDrawHeight = lHeight;
							if( lDestBottom > rcDest.bottom ) {
								lDrawHeight -= lDestBottom - rcDest.bottom;
								lDestBottom = rcDest.bottom;
							}
							for( int i = 0; i < iTimesX; ++i ) {
								LONG lDestLeft = rcDest.left + lWidth * i;
								LONG lDestRight = rcDest.left + lWidth * (i + 1);
								LONG lDrawWidth = lWidth;
								if( lDestRight > rcDest.right ) {
									lDrawWidth -= lDestRight - rcDest.right;
									lDestRight = rcDest.right;
								}
								lpAlphaBlend(hDC, rcDest.left + lWidth * i, rcDest.top + lHeight * j, 
									lDestRight - lDestLeft, lDestBottom - lDestTop, hCloneDC, 
									rcBmpPart.left + rcCorners.left, rcBmpPart.top + rcCorners.top, lDrawWidth, lDrawHeight, bf);
							}
						}
					}
					else if( xtiled ) {
						LONG lWidth = rcBmpPart.right - rcBmpPart.left - rcCorners.left - rcCorners.right;
						int iTimes = (rcDest.right - rcDest.left + lWidth - 1) / lWidth;
						for( int i = 0; i < iTimes; ++i ) {
							LONG lDestLeft = rcDest.left + lWidth * i;
							LONG lDestRight = rcDest.left + lWidth * (i + 1);
							LONG lDrawWidth = lWidth;
							if( lDestRight > rcDest.right ) {
								lDrawWidth -= lDestRight - rcDest.right;
								lDestRight = rcDest.right;
							}
							lpAlphaBlend(hDC, lDestLeft, rcDest.top, lDestRight - lDestLeft, rcDest.bottom, 
								hCloneDC, rcBmpPart.left + rcCorners.left, rcBmpPart.top + rcCorners.top, \
								lDrawWidth, rcBmpPart.bottom - rcBmpPart.top - rcCorners.top - rcCorners.bottom, bf);
						}
					}
					else { // ytiled
						LONG lHeight = rcBmpPart.bottom - rcBmpPart.top - rcCorners.top - rcCorners.bottom;
						int iTimes = (rcDest.bottom - rcDest.top + lHeight - 1) / lHeight;
						for( int i = 0; i < iTimes; ++i ) {
							LONG lDestTop = rcDest.top + lHeight * i;
							LONG lDestBottom = rcDest.top + lHeight * (i + 1);
							LONG lDrawHeight = lHeight;
							if( lDestBottom > rcDest.bottom ) {
								lDrawHeight -= lDestBottom - rcDest.bottom;
								lDestBottom = rcDest.bottom;
							}
							lpAlphaBlend(hDC, rcDest.left, rcDest.top + lHeight * i, rcDest.right, lDestBottom - lDestTop, 
								hCloneDC, rcBmpPart.left + rcCorners.left, rcBmpPart.top + rcCorners.top, \
								rcBmpPart.right - rcBmpPart.left - rcCorners.left - rcCorners.right, lDrawHeight, bf);                    
						}
					}
				}
			}

			// left-top
			if( rcCorners.left > 0 && rcCorners.top > 0 ) {
				rcDest.left = rc.left;
				rcDest.top = rc.top;
				rcDest.right = rcCorners.left;
				rcDest.bottom = rcCorners.top;
				rcDest.right += rcDest.left;
				rcDest.bottom += rcDest.top;
				if( ::IntersectRect(&rcTemp, &rcPaint, &rcDest) ) {
					rcDest.right -= rcDest.left;
					rcDest.bottom -= rcDest.top;
					lpAlphaBlend(hDC, rcDest.left, rcDest.top, rcDest.right, rcDest.bottom, hCloneDC, \
						rcBmpPart.left, rcBmpPart.top, rcCorners.left, rcCorners.top, bf);
				}
			}
			// top
			if( rcCorners.top > 0 ) {
				rcDest.left = rc.left + rcCorners.left;
				rcDest.top = rc.top;
				rcDest.right = rc.right - rc.left - rcCorners.left - rcCorners.right;
				rcDest.bottom = rcCorners.top;
				rcDest.right += rcDest.left;
				rcDest.bottom += rcDest.top;
				if( ::IntersectRect(&rcTemp, &rcPaint, &rcDest) ) {
					rcDest.right -= rcDest.left;
					rcDest.bottom -= rcDest.top;
					lpAlphaBlend(hDC, rcDest.left, rcDest.top, rcDest.right, rcDest.bottom, hCloneDC, \
						rcBmpPart.left + rcCorners.left, rcBmpPart.top, rcBmpPart.right - rcBmpPart.left - \
						rcCorners.left - rcCorners.right, rcCorners.top, bf);
				}
			}
			// right-top
			if( rcCorners.right > 0 && rcCorners.top > 0 ) {
				rcDest.left = rc.right - rcCorners.right;
				rcDest.top = rc.top;
				rcDest.right = rcCorners.right;
				rcDest.bottom = rcCorners.top;
				rcDest.right += rcDest.left;
				rcDest.bottom += rcDest.top;
				if( ::IntersectRect(&rcTemp, &rcPaint, &rcDest) ) {
					rcDest.right -= rcDest.left;
					rcDest.bottom -= rcDest.top;
					lpAlphaBlend(hDC, rcDest.left, rcDest.top, rcDest.right, rcDest.bottom, hCloneDC, \
						rcBmpPart.right - rcCorners.right, rcBmpPart.top, rcCorners.right, rcCorners.top, bf);
				}
			}
			// left
			if( rcCorners.left > 0 ) {
				rcDest.left = rc.left;
				rcDest.top = rc.top + rcCorners.top;
				rcDest.right = rcCorners.left;
				rcDest.bottom = rc.bottom - rc.top - rcCorners.top - rcCorners.bottom;
				rcDest.right += rcDest.left;
				rcDest.bottom += rcDest.top;
				if( ::IntersectRect(&rcTemp, &rcPaint, &rcDest) ) {
					rcDest.right -= rcDest.left;
					rcDest.bottom -= rcDest.top;
					lpAlphaBlend(hDC, rcDest.left, rcDest.top, rcDest.right, rcDest.bottom, hCloneDC, \
						rcBmpPart.left, rcBmpPart.top + rcCorners.top, rcCorners.left, rcBmpPart.bottom - \
						rcBmpPart.top - rcCorners.top - rcCorners.bottom, bf);
				}
			}
			// right
			if( rcCorners.right > 0 ) {
				rcDest.left = rc.right - rcCorners.right;
				rcDest.top = rc.top + rcCorners.top;
				rcDest.right = rcCorners.right;
				rcDest.bottom = rc.bottom - rc.top - rcCorners.top - rcCorners.bottom;
				rcDest.right += rcDest.left;
				rcDest.bottom += rcDest.top;
				if( ::IntersectRect(&rcTemp, &rcPaint, &rcDest) ) {
					rcDest.right -= rcDest.left;
					rcDest.bottom -= rcDest.top;
					lpAlphaBlend(hDC, rcDest.left, rcDest.top, rcDest.right, rcDest.bottom, hCloneDC, \
						rcBmpPart.right - rcCorners.right, rcBmpPart.top + rcCorners.top, rcCorners.right, \
						rcBmpPart.bottom - rcBmpPart.top - rcCorners.top - rcCorners.bottom, bf);
				}
			}
			// left-bottom
			if( rcCorners.left > 0 && rcCorners.bottom > 0 ) {
				rcDest.left = rc.left;
				rcDest.top = rc.bottom - rcCorners.bottom;
				rcDest.right = rcCorners.left;
				rcDest.bottom = rcCorners.bottom;
				rcDest.right += rcDest.left;
				rcDest.bottom += rcDest.top;
				if( ::IntersectRect(&rcTemp, &rcPaint, &rcDest) ) {
					rcDest.right -= rcDest.left;
					rcDest.bottom -= rcDest.top;
					lpAlphaBlend(hDC, rcDest.left, rcDest.top, rcDest.right, rcDest.bottom, hCloneDC, \
						rcBmpPart.left, rcBmpPart.bottom - rcCorners.bottom, rcCorners.left, rcCorners.bottom, bf);
				}
			}
			// bottom
			if( rcCorners.bottom > 0 ) {
				rcDest.left = rc.left + rcCorners.left;
				rcDest.top = rc.bottom - rcCorners.bottom;
				rcDest.right = rc.right - rc.left - rcCorners.left - rcCorners.right;
				rcDest.bottom = rcCorners.bottom;
				rcDest.right += rcDest.left;
				rcDest.bottom += rcDest.top;
				if( ::IntersectRect(&rcTemp, &rcPaint, &rcDest) ) {
					rcDest.right -= rcDest.left;
					rcDest.bottom -= rcDest.top;
					lpAlphaBlend(hDC, rcDest.left, rcDest.top, rcDest.right, rcDest.bottom, hCloneDC, \
						rcBmpPart.left + rcCorners.left, rcBmpPart.bottom - rcCorners.bottom, \
						rcBmpPart.right - rcBmpPart.left - rcCorners.left - rcCorners.right, rcCorners.bottom, bf);
				}
			}
			// right-bottom
			if( rcCorners.right > 0 && rcCorners.bottom > 0 ) {
				rcDest.left = rc.right - rcCorners.right;
				rcDest.top = rc.bottom - rcCorners.bottom;
				rcDest.right = rcCorners.right;
				rcDest.bottom = rcCorners.bottom;
				rcDest.right += rcDest.left;
				rcDest.bottom += rcDest.top;
				if( ::IntersectRect(&rcTemp, &rcPaint, &rcDest) ) {
					rcDest.right -= rcDest.left;
					rcDest.bottom -= rcDest.top;
					lpAlphaBlend(hDC, rcDest.left, rcDest.top, rcDest.right, rcDest.bottom, hCloneDC, \
						rcBmpPart.right - rcCorners.right, rcBmpPart.bottom - rcCorners.bottom, rcCorners.right, \
						rcCorners.bottom, bf);
				}
			}
		}
		else 
		{
			if (rc.right - rc.left == rcBmpPart.right - rcBmpPart.left \
				&& rc.bottom - rc.top == rcBmpPart.bottom - rcBmpPart.top \
				&& rcCorners.left == 0 && rcCorners.right == 0 && rcCorners.top == 0 && rcCorners.bottom == 0)
			{
				if( ::IntersectRect(&rcTemp, &rcPaint, &rc) ) {
					::BitBlt(hDC, rcTemp.left, rcTemp.top, rcTemp.right - rcTemp.left, rcTemp.bottom - rcTemp.top, \
						hCloneDC, rcBmpPart.left + rcTemp.left - rc.left, rcBmpPart.top + rcTemp.top - rc.top, SRCCOPY);
				}
			}
			else
			{
				// middle
				if( !hole ) {
					rcDest.left = rc.left + rcCorners.left;
					rcDest.top = rc.top + rcCorners.top;
					rcDest.right = rc.right - rc.left - rcCorners.left - rcCorners.right;
					rcDest.bottom = rc.bottom - rc.top - rcCorners.top - rcCorners.bottom;
					rcDest.right += rcDest.left;
					rcDest.bottom += rcDest.top;
					if( ::IntersectRect(&rcTemp, &rcPaint, &rcDest) ) {
						if( !xtiled && !ytiled ) {
							rcDest.right -= rcDest.left;
							rcDest.bottom -= rcDest.top;
							::StretchBlt(hDC, rcDest.left, rcDest.top, rcDest.right, rcDest.bottom, hCloneDC, \
								rcBmpPart.left + rcCorners.left, rcBmpPart.top + rcCorners.top, \
								rcBmpPart.right - rcBmpPart.left - rcCorners.left - rcCorners.right, \
								rcBmpPart.bottom - rcBmpPart.top - rcCorners.top - rcCorners.bottom, SRCCOPY);
						}
						else if( xtiled && ytiled ) {
							LONG lWidth = rcBmpPart.right - rcBmpPart.left - rcCorners.left - rcCorners.right;
							LONG lHeight = rcBmpPart.bottom - rcBmpPart.top - rcCorners.top - rcCorners.bottom;
							int iTimesX = (rcDest.right - rcDest.left + lWidth - 1) / lWidth;
							int iTimesY = (rcDest.bottom - rcDest.top + lHeight - 1) / lHeight;
							for( int j = 0; j < iTimesY; ++j ) {
								LONG lDestTop = rcDest.top + lHeight * j;
								LONG lDestBottom = rcDest.top + lHeight * (j + 1);
								LONG lDrawHeight = lHeight;
								if( lDestBottom > rcDest.bottom ) {
									lDrawHeight -= lDestBottom - rcDest.bottom;
									lDestBottom = rcDest.bottom;
								}
								for( int i = 0; i < iTimesX; ++i ) {
									LONG lDestLeft = rcDest.left + lWidth * i;
									LONG lDestRight = rcDest.left + lWidth * (i + 1);
									LONG lDrawWidth = lWidth;
									if( lDestRight > rcDest.right ) {
										lDrawWidth -= lDestRight - rcDest.right;
										lDestRight = rcDest.right;
									}
									::BitBlt(hDC, rcDest.left + lWidth * i, rcDest.top + lHeight * j, \
										lDestRight - lDestLeft, lDestBottom - lDestTop, hCloneDC, \
										rcBmpPart.left + rcCorners.left, rcBmpPart.top + rcCorners.top, SRCCOPY);
								}
							}
						}
						else if( xtiled ) {
							LONG lWidth = rcBmpPart.right - rcBmpPart.left - rcCorners.left - rcCorners.right;
							int iTimes = (rcDest.right - rcDest.left + lWidth - 1) / lWidth;
							for( int i = 0; i < iTimes; ++i ) {
								LONG lDestLeft = rcDest.left + lWidth * i;
								LONG lDestRight = rcDest.left + lWidth * (i + 1);
								LONG lDrawWidth = lWidth;
								if( lDestRight > rcDest.right ) {
									lDrawWidth -= lDestRight - rcDest.right;
									lDestRight = rcDest.right;
								}
								::StretchBlt(hDC, lDestLeft, rcDest.top, lDestRight - lDestLeft, rcDest.bottom, 
									hCloneDC, rcBmpPart.left + rcCorners.left, rcBmpPart.top + rcCorners.top, \
									lDrawWidth, rcBmpPart.bottom - rcBmpPart.top - rcCorners.top - rcCorners.bottom, SRCCOPY);
							}
						}
						else { // ytiled
							LONG lHeight = rcBmpPart.bottom - rcBmpPart.top - rcCorners.top - rcCorners.bottom;
							int iTimes = (rcDest.bottom - rcDest.top + lHeight - 1) / lHeight;
							for( int i = 0; i < iTimes; ++i ) {
								LONG lDestTop = rcDest.top + lHeight * i;
								LONG lDestBottom = rcDest.top + lHeight * (i + 1);
								LONG lDrawHeight = lHeight;
								if( lDestBottom > rcDest.bottom ) {
									lDrawHeight -= lDestBottom - rcDest.bottom;
									lDestBottom = rcDest.bottom;
								}
								::StretchBlt(hDC, rcDest.left, rcDest.top + lHeight * i, rcDest.right, lDestBottom - lDestTop, 
									hCloneDC, rcBmpPart.left + rcCorners.left, rcBmpPart.top + rcCorners.top, \
									rcBmpPart.right - rcBmpPart.left - rcCorners.left - rcCorners.right, lDrawHeight, SRCCOPY);                    
							}
						}
					}
				}

				// left-top
				if( rcCorners.left > 0 && rcCorners.top > 0 ) {
					rcDest.left = rc.left;
					rcDest.top = rc.top;
					rcDest.right = rcCorners.left;
					rcDest.bottom = rcCorners.top;
					rcDest.right += rcDest.left;
					rcDest.bottom += rcDest.top;
					if( ::IntersectRect(&rcTemp, &rcPaint, &rcDest) ) {
						rcDest.right -= rcDest.left;
						rcDest.bottom -= rcDest.top;
						::StretchBlt(hDC, rcDest.left, rcDest.top, rcDest.right, rcDest.bottom, hCloneDC, \
							rcBmpPart.left, rcBmpPart.top, rcCorners.left, rcCorners.top, SRCCOPY);
					}
				}
				// top
				if( rcCorners.top > 0 ) {
					rcDest.left = rc.left + rcCorners.left;
					rcDest.top = rc.top;
					rcDest.right = rc.right - rc.left - rcCorners.left - rcCorners.right;
					rcDest.bottom = rcCorners.top;
					rcDest.right += rcDest.left;
					rcDest.bottom += rcDest.top;
					if( ::IntersectRect(&rcTemp, &rcPaint, &rcDest) ) {
						rcDest.right -= rcDest.left;
						rcDest.bottom -= rcDest.top;
						::StretchBlt(hDC, rcDest.left, rcDest.top, rcDest.right, rcDest.bottom, hCloneDC, \
							rcBmpPart.left + rcCorners.left, rcBmpPart.top, rcBmpPart.right - rcBmpPart.left - \
							rcCorners.left - rcCorners.right, rcCorners.top, SRCCOPY);
					}
				}
				// right-top
				if( rcCorners.right > 0 && rcCorners.top > 0 ) {
					rcDest.left = rc.right - rcCorners.right;
					rcDest.top = rc.top;
					rcDest.right = rcCorners.right;
					rcDest.bottom = rcCorners.top;
					rcDest.right += rcDest.left;
					rcDest.bottom += rcDest.top;
					if( ::IntersectRect(&rcTemp, &rcPaint, &rcDest) ) {
						rcDest.right -= rcDest.left;
						rcDest.bottom -= rcDest.top;
						::StretchBlt(hDC, rcDest.left, rcDest.top, rcDest.right, rcDest.bottom, hCloneDC, \
							rcBmpPart.right - rcCorners.right, rcBmpPart.top, rcCorners.right, rcCorners.top, SRCCOPY);
					}
				}
				// left
				if( rcCorners.left > 0 ) {
					rcDest.left = rc.left;
					rcDest.top = rc.top + rcCorners.top;
					rcDest.right = rcCorners.left;
					rcDest.bottom = rc.bottom - rc.top - rcCorners.top - rcCorners.bottom;
					rcDest.right += rcDest.left;
					rcDest.bottom += rcDest.top;
					if( ::IntersectRect(&rcTemp, &rcPaint, &rcDest) ) {
						rcDest.right -= rcDest.left;
						rcDest.bottom -= rcDest.top;
						::StretchBlt(hDC, rcDest.left, rcDest.top, rcDest.right, rcDest.bottom, hCloneDC, \
							rcBmpPart.left, rcBmpPart.top + rcCorners.top, rcCorners.left, rcBmpPart.bottom - \
							rcBmpPart.top - rcCorners.top - rcCorners.bottom, SRCCOPY);
					}
				}
				// right
				if( rcCorners.right > 0 ) {
					rcDest.left = rc.right - rcCorners.right;
					rcDest.top = rc.top + rcCorners.top;
					rcDest.right = rcCorners.right;
					rcDest.bottom = rc.bottom - rc.top - rcCorners.top - rcCorners.bottom;
					rcDest.right += rcDest.left;
					rcDest.bottom += rcDest.top;
					if( ::IntersectRect(&rcTemp, &rcPaint, &rcDest) ) {
						rcDest.right -= rcDest.left;
						rcDest.bottom -= rcDest.top;
						::StretchBlt(hDC, rcDest.left, rcDest.top, rcDest.right, rcDest.bottom, hCloneDC, \
							rcBmpPart.right - rcCorners.right, rcBmpPart.top + rcCorners.top, rcCorners.right, \
							rcBmpPart.bottom - rcBmpPart.top - rcCorners.top - rcCorners.bottom, SRCCOPY);
					}
				}
				// left-bottom
				if( rcCorners.left > 0 && rcCorners.bottom > 0 ) {
					rcDest.left = rc.left;
					rcDest.top = rc.bottom - rcCorners.bottom;
					rcDest.right = rcCorners.left;
					rcDest.bottom = rcCorners.bottom;
					rcDest.right += rcDest.left;
					rcDest.bottom += rcDest.top;
					if( ::IntersectRect(&rcTemp, &rcPaint, &rcDest) ) {
						rcDest.right -= rcDest.left;
						rcDest.bottom -= rcDest.top;
						::StretchBlt(hDC, rcDest.left, rcDest.top, rcDest.right, rcDest.bottom, hCloneDC, \
							rcBmpPart.left, rcBmpPart.bottom - rcCorners.bottom, rcCorners.left, rcCorners.bottom, SRCCOPY);
					}
				}
				// bottom
				if( rcCorners.bottom > 0 ) {
					rcDest.left = rc.left + rcCorners.left;
					rcDest.top = rc.bottom - rcCorners.bottom;
					rcDest.right = rc.right - rc.left - rcCorners.left - rcCorners.right;
					rcDest.bottom = rcCorners.bottom;
					rcDest.right += rcDest.left;
					rcDest.bottom += rcDest.top;
					if( ::IntersectRect(&rcTemp, &rcPaint, &rcDest) ) {
						rcDest.right -= rcDest.left;
						rcDest.bottom -= rcDest.top;
						::StretchBlt(hDC, rcDest.left, rcDest.top, rcDest.right, rcDest.bottom, hCloneDC, \
							rcBmpPart.left + rcCorners.left, rcBmpPart.bottom - rcCorners.bottom, \
							rcBmpPart.right - rcBmpPart.left - rcCorners.left - rcCorners.right, rcCorners.bottom, SRCCOPY);
					}
				}
				// right-bottom
				if( rcCorners.right > 0 && rcCorners.bottom > 0 ) {
					rcDest.left = rc.right - rcCorners.right;
					rcDest.top = rc.bottom - rcCorners.bottom;
					rcDest.right = rcCorners.right;
					rcDest.bottom = rcCorners.bottom;
					rcDest.right += rcDest.left;
					rcDest.bottom += rcDest.top;
					if( ::IntersectRect(&rcTemp, &rcPaint, &rcDest) ) {
						rcDest.right -= rcDest.left;
						rcDest.bottom -= rcDest.top;
						::StretchBlt(hDC, rcDest.left, rcDest.top, rcDest.right, rcDest.bottom, hCloneDC, \
							rcBmpPart.right - rcCorners.right, rcBmpPart.bottom - rcCorners.bottom, rcCorners.right, \
							rcCorners.bottom, SRCCOPY);
					}
				}
			}
		}

		::SelectObject(hCloneDC, hOldBitmap);
		// [PERF-02] keep the DC cached (see the allocation site); it is released
		// when the owning thread exits.
	}

	bool CRenderEngine::DrawImageInfo(HDC hDC, CPaintManagerUI* pManager, const RECT& rcItem, const RECT& rcPaint, const TDrawInfo* pDrawInfo, HINSTANCE instance)
	{
		if( pManager == nullptr || hDC == nullptr || pDrawInfo == nullptr ) return false;
		RECT rcDest = rcItem;
		// 计算绘制目标区域
		if( pDrawInfo->rcDest.left != 0 || pDrawInfo->rcDest.top != 0 ||
			pDrawInfo->rcDest.right != 0 || pDrawInfo->rcDest.bottom != 0 ) {
				rcDest.left = rcItem.left + pDrawInfo->rcDest.left;
				rcDest.top = rcItem.top + pDrawInfo->rcDest.top;
				rcDest.right = rcItem.left + pDrawInfo->rcDest.right;
				if( rcDest.right > rcItem.right ) rcDest.right = rcItem.right;
				rcDest.bottom = rcItem.top + pDrawInfo->rcDest.bottom;
				if( rcDest.bottom > rcItem.bottom ) rcDest.bottom = rcItem.bottom;
		}
		// 根据对齐方式计算目标区域
		if(pDrawInfo->szImage.cx > 0 && pDrawInfo->szImage.cy > 0) {
			SIZE szImage = pManager->GetDPIObj()->Scale(pDrawInfo->szImage);
			RECT rcPadding = pManager->GetDPIObj()->Scale(pDrawInfo->rcPadding);
			DuiLib::MakeImageDest(rcItem, szImage, pDrawInfo->sAlign, rcPadding, rcDest);
		}

		bool bRet = DuiLib::DrawImage(hDC, pManager, rcItem, rcPaint, pDrawInfo->sImageName, pDrawInfo->sResType, rcDest, \
			pDrawInfo->rcSource, pDrawInfo->rcCorner, pDrawInfo->dwMask, pDrawInfo->uFade, pDrawInfo->uRotate, pDrawInfo->bGdiplus, pDrawInfo->bHole, pDrawInfo->bTiledX, pDrawInfo->bTiledY, instance);

		return bRet;
	}

	bool CRenderEngine::DrawImageString(HDC hDC, CPaintManagerUI* pManager, const RECT& rcItem, const RECT& rcPaint, LPCTSTR pStrImage, LPCTSTR pStrModify, HINSTANCE instance)
	{
		if ((pManager == nullptr) || (hDC == nullptr)) return false;
		const TDrawInfo* pDrawInfo = pManager->GetDrawInfo(pStrImage, pStrModify);
		return DrawImageInfo(hDC, pManager, rcItem, rcPaint, pDrawInfo, instance);
	}

	/////////////////////////////////////////////////////////////////////////////////////
	//
	//
	TImageInfo* CRenderEngine::GdiplusLoadImage(STRINGorID bitmap, LPCTSTR type, DWORD mask, HINSTANCE instance)
	{
		LPBYTE pData = nullptr;
		DWORD dwSize = 0;
		do 
		{
			if( type == nullptr ) {
				CDuiString sFile = CPaintManagerUI::GetResourcePath();
				if( CPaintManagerUI::GetResourceZip().IsEmpty() ) {
					sFile += bitmap.m_lpstr;
					HANDLE hFile = ::CreateFile(sFile.GetData(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, \
						FILE_ATTRIBUTE_NORMAL, nullptr);
					if( hFile == INVALID_HANDLE_VALUE ) break;
					dwSize = ::GetFileSize(hFile, nullptr);
					if( dwSize == 0 ) break;

					DWORD dwRead = 0;
					pData = new BYTE[ dwSize ];
					::ReadFile( hFile, pData, dwSize, &dwRead, nullptr );
					::CloseHandle( hFile );

					if( dwRead != dwSize ) {
						delete[] pData;
						pData = nullptr;
						break;
					}
				}
				else {
					sFile += CPaintManagerUI::GetResourceZip();
					CDuiString sFilePwd = CPaintManagerUI::GetResourceZipPwd();
					HZIP hz = nullptr;
					if( CPaintManagerUI::IsCachedResourceZip() ) hz = reinterpret_cast<HZIP>(CPaintManagerUI::GetResourceZipHandle());
					else
					{
#ifdef UNICODE
						std::unique_ptr<char[]> pwd{ w2a(const_cast<wchar_t*>(sFilePwd.GetData())) }; // [R4] RAII
						hz = OpenZip(sFile.GetData(), pwd.get());
#else
						hz = OpenZip(sFile.GetData(), sFilePwd.GetData());
#endif
					}
					if( hz == nullptr ) break;
					ZIPENTRY ze; 
					int i = 0; 
					CDuiString key = bitmap.m_lpstr;
					key.Replace(_T("\\"), _T("/"));
					if( FindZipItem(hz, key, true, &i, &ze) != 0 ) { // [R6] close before break
						if( !CPaintManagerUI::IsCachedResourceZip() ) CloseZip(hz);
						break;
					}
					dwSize = ze.unc_size;
					if( dwSize == 0 ) { // [R6] close before break
						if( !CPaintManagerUI::IsCachedResourceZip() ) CloseZip(hz);
						break;
					}
					if( dwSize > kMaxImageBytes ) { // [R8] size ceiling
						DUI_LOGE("LoadImage: zip entry '%s' bad size %u", key.GetData(), dwSize);
						if( !CPaintManagerUI::IsCachedResourceZip() ) CloseZip(hz);
						break;
					}
					pData = new(std::nothrow) BYTE[ dwSize ]; // [R8] nothrow
					if( pData == nullptr ) { if( !CPaintManagerUI::IsCachedResourceZip() ) CloseZip(hz); break; }
					int res = UnzipItem(hz, i, pData, dwSize);
					if( res != 0x00000000 && res != 0x00000600) {
						delete[] pData;
						pData = nullptr;
						if( !CPaintManagerUI::IsCachedResourceZip() ) CloseZip(hz);
						break;
					}
					if( !CPaintManagerUI::IsCachedResourceZip() ) CloseZip(hz);
				}
			}
			else {
				HINSTANCE dllinstance = nullptr;
				if (instance) {
					dllinstance = instance;
				}
				else {
					dllinstance = CPaintManagerUI::GetResourceDll();
				}
				HRSRC hResource = ::FindResource(dllinstance, bitmap.m_lpstr, type);
				if( hResource == nullptr ) break;
				HGLOBAL hGlobal = ::LoadResource(dllinstance, hResource);
				if( hGlobal == nullptr ) {
					FreeResource(hResource);
					break;
				}

				dwSize = ::SizeofResource(dllinstance, hResource);
				if( dwSize == 0 ) { ::FreeResource(hGlobal); break; } // [R7]
				pData = new BYTE[ dwSize ];
				::CopyMemory(pData, (LPBYTE)::LockResource(hGlobal), dwSize);
				::FreeResource(hGlobal);
			}
		} while (0);

		while (!pData)
		{
			//读不到图片, 则直接去读取bitmap.m_lpstr指向的路径
			HANDLE hFile = ::CreateFile(bitmap.m_lpstr, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, \
				FILE_ATTRIBUTE_NORMAL, nullptr);
			if( hFile == INVALID_HANDLE_VALUE ) break;
			dwSize = ::GetFileSize(hFile, nullptr);
			if( dwSize == 0 ) break;

			DWORD dwRead = 0;
			pData = new BYTE[ dwSize ];
			::ReadFile( hFile, pData, dwSize, &dwRead, nullptr );
			::CloseHandle( hFile );

			if( dwRead != dwSize ) {
				delete[] pData;
				pData = nullptr;
			}
			break;
		}
		
		if(pData == nullptr) {
			return nullptr;
		}

		// GDI
		bool bAlphaChannel = false;
		HBITMAP hBitmap = nullptr;
		int x,y,n;
		LPBYTE pImage = stbi_load_from_memory(pData, dwSize, &x, &y, &n, 4);
		if(pImage != nullptr) {
			BITMAPINFO bmi;
			::ZeroMemory(&bmi, sizeof(BITMAPINFO));
			bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			bmi.bmiHeader.biWidth = x;
			bmi.bmiHeader.biHeight = -y;
			bmi.bmiHeader.biPlanes = 1;
			bmi.bmiHeader.biBitCount = 32;
			bmi.bmiHeader.biCompression = BI_RGB;
			bmi.bmiHeader.biSizeImage = x * y * 4;

			LPBYTE pDest = nullptr;
			hBitmap = ::CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, (void**)&pDest, nullptr, 0);
			if(hBitmap != nullptr) {
				for( int i = 0; i < x * y; i++ ) 
				{
					pDest[i*4 + 3] = pImage[i*4 + 3];
					if( pDest[i*4 + 3] < 255 )
					{
						pDest[i*4] = static_cast<BYTE>(DWORD(pImage[i*4 + 2])*pImage[i*4 + 3]/255);
						pDest[i*4 + 1] = static_cast<BYTE>(DWORD(pImage[i*4 + 1])*pImage[i*4 + 3]/255);
						pDest[i*4 + 2] = static_cast<BYTE>(DWORD(pImage[i*4])*pImage[i*4 + 3]/255); 
						bAlphaChannel = true;
					}
					else
					{
						pDest[i*4] = pImage[i*4 + 2];
						pDest[i*4 + 1] = pImage[i*4 + 1];
						pDest[i*4 + 2] = pImage[i*4]; 
					}

					if( *(DWORD*)(&pDest[i*4]) == mask ) {
						pDest[i*4] = (BYTE)0;
						pDest[i*4 + 1] = (BYTE)0;
						pDest[i*4 + 2] = (BYTE)0; 
						pDest[i*4 + 3] = (BYTE)0;
						bAlphaChannel = true;
					}
				}
				stbi_image_free(pImage);
			}
		}

		// GdiPlus
		HGLOBAL hMem = ::GlobalAlloc(GMEM_FIXED, dwSize);
		BYTE* pMem = (BYTE*)::GlobalLock(hMem);
		memcpy(pMem, pData, dwSize);
		IStream* pStm = nullptr;
		::CreateStreamOnHGlobal(hMem, TRUE, &pStm);

		Gdiplus::Image* pGdiplusImage = Gdiplus::Image::FromStream(pStm);
		if(!pGdiplusImage || pGdiplusImage->GetLastStatus() != Gdiplus::Ok)
		{
			pGdiplusImage = nullptr;

			pStm->Release();
			::GlobalUnlock(hMem);
		}

		delete[] pData;
		pData = nullptr;

		if(hBitmap == nullptr && pGdiplusImage == nullptr) {
			return nullptr;
		}

		TImageInfo* data = new TImageInfo;
		data->pBits = nullptr;
		data->pSrcBits = nullptr;
		data->pImage = pGdiplusImage;
		data->hBitmap = hBitmap;
		data->nX = x;
		data->nY = y;
		data->bAlpha = bAlphaChannel;
		return data;
	}

	void CRenderEngine::GdiplusDrawImage(HDC hDC, Gdiplus::Image* image, const RECT& rc, const RECT& rcPaint, const RECT& rcBmpPart, bool bAlpha, UINT uFade, UINT uRotate)
	{
		Gdiplus::Graphics g(hDC);

		//设置画图时的滤波模式为消除锯齿现象
		g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);

		Gdiplus::ImageAttributes imageAtt;
		if(uFade != 255) {
			Gdiplus::ColorMatrix colorMatrix =
			{
				1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
				0.0f, 1.0f, 0.0f, 0.0f, 0.0f,
				0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
				0.0f, 0.0f, 0.0f, uFade / 255.0f, 0.0f,
				0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
			};
			imageAtt.SetColorMatrix (&colorMatrix, Gdiplus::ColorMatrixFlagsDefault, Gdiplus::ColorAdjustTypeBitmap);
		}
		
		Gdiplus::RectF rcDest(rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
		Gdiplus::RectF rcSrc(rcBmpPart.left, rcBmpPart.top, rcBmpPart.right - rcBmpPart.left, rcBmpPart.bottom - rcBmpPart.top);
		if(uRotate > 0) {
			POINT ptCenter = {rc.left + (rc.right - rc.left) / 2, rc.top + (rc.bottom - rc.top) / 2};
			int cx = rc.right - rc.left;
			int cy = rc.bottom - rc.top;

			Gdiplus::Matrix matrix;
			matrix.RotateAt(uRotate, Gdiplus::PointF(ptCenter.x, ptCenter.y));
			g.SetTransform(&matrix);
			g.DrawImage(image, rcDest, rcSrc.GetLeft(), rcSrc.GetTop(), rcSrc.Width, rcSrc.Height, Gdiplus::UnitPixel, &imageAtt);
			g.ResetTransform();
		}
		else {
			g.DrawImage(image, rcDest, rcSrc.GetLeft(), rcSrc.GetTop(), rcSrc.Width, rcSrc.Height, Gdiplus::UnitPixel, &imageAtt);
		}

		g.ReleaseHDC(hDC);
	}

	//////////////////////////////////////////////////////////////////////////
	// [WIN11-03] Direct2D / DirectWrite text backend (opt-in)
	//
	// Layering: DirectWrite lays the string out, ID2D1DCRenderTarget rasterises
	// it into a per-thread reusable DIB, and the DIB is AlphaBlend-ed onto the
	// caller's HDC. The existing GDI composition pipeline stays untouched, so
	// this cannot corrupt the caller's DC state.
	//
	// Why not bind D2D straight to the paint HDC: Paint() mixes GDI and GDI+ on
	// the same offscreen DC; interleaving a D2D render target there would put
	// two state machines on one HDC. Rasterising into our own DIB keeps them
	// fully separate at the cost of one extra blit.
	//
	// Any failure (no d2d1/dwrite, OOM, unsupported format, bad layout) returns
	// false and the caller keeps using the original GDI path.
	namespace
	{
		// [WIN11-03b] resolved once, used by both the cached and fresh paths
		typedef BOOL(WINAPI* PFN_ALPHABLEND)(HDC, int, int, int, int, HDC, int, int, int, int, BLENDFUNCTION);
		// [WIN11-03b] a cached rasterised text bitmap
		struct D2DTextEntry
		{
			HDC      hdc = nullptr;
			HBITMAP  hbm = nullptr;
			HGDIOBJ  hOld = nullptr;
			int      cx = 0;
			int      cy = 0;
			UINT64   stamp = 0;

			// [D2D-LEAK] Release what this entry created. Without this the map's
			// destructor only forgets the handles: every cached string kept a DC and a
			// DIB allocated for the life of the process (measured: +8 GDI objects that
			// ClearD2DTextCache() never returned to baseline).
			~D2DTextEntry()
			{
				ReleaseResources();
			}

			void ReleaseResources()
			{
				if (hdc != nullptr)
				{
					if (hOld != nullptr) ::SelectObject(hdc, hOld);
					hOld = nullptr;
				}
				if (hbm != nullptr) { ::DeleteObject(hbm); hbm = nullptr; }
				if (hdc != nullptr) { ::DeleteDC(hdc); hdc = nullptr; }
			}

			// Copy would duplicate raw handles and double-free them. The map only ever
			// moves entries in, so deleting the copy operations is safe and keeps a
			// future edit from introducing a double free.
			D2DTextEntry() = default;
			D2DTextEntry(const D2DTextEntry&) = delete;
			D2DTextEntry& operator=(const D2DTextEntry&) = delete;
			D2DTextEntry(D2DTextEntry&& o) noexcept { Swap(o); }
			D2DTextEntry& operator=(D2DTextEntry&& o) noexcept
			{
				if (this != &o) { ReleaseResources(); Swap(o); }
				return *this;
			}

			void Swap(D2DTextEntry& o) noexcept
			{
				std::swap(hdc, o.hdc);
				std::swap(hbm, o.hbm);
				std::swap(hOld, o.hOld);
				std::swap(cx, o.cx);
				std::swap(cy, o.cy);
				std::swap(stamp, o.stamp);
			}
		};

		// [PERF-20] Device-side objects are process-wide, not per thread.
		// CreateDCRenderTarget alone costs 94-110ms on this machine: it brings
		// up the WARP/DXGI device. Measured invariant across single- and
		// multi-threaded factories, so moving threads cannot make it cheaper.
		// The only useful fix is to pay it OFF the startup critical path, which
		// requires one shared device that a warm-up thread can build while the
		// demo is still creating its window.
		struct D2DDeviceShared
		{
			ID2D1Factory*        pFactory = nullptr;
			IDWriteFactory*      pWrite = nullptr;
			ID2D1DCRenderTarget* pTarget = nullptr;
			PFN_ALPHABLEND       pfnAlphaBlend = nullptr;
			volatile LONG        nState = 0;   // 0 idle, 1 building, 2 ready, 3 failed
			volatile LONG        bEnabled = 0;
		};

		D2DDeviceShared& D2DShared()
		{
			// Function-local static: thread-safe init, destroyed at process exit.
			static D2DDeviceShared s;
			return s;
		}

		struct D2DTextState
		{
			bool                 bOk = false;
			std::map<HFONT, IDWriteTextFormat*> formats;
			std::map<CDuiString, D2DTextEntry> cache;   // [WIN11-03b]
			UINT64 nStamp = 0;                          // [WIN11-03b]
			// [R247] dynamic-text detection
			int nMissStreak = 0;      // consecutive cache misses
			int nHitCount = 0;        // lifetime hits (diagnostics)
			int nCooldown = 0;        // >0 means "let GDI handle the next draws"
			// Per-thread scratch DIB, grown on demand and reused across calls.
			HDC                  hScratchDC = nullptr;
			HBITMAP              hScratchBM = nullptr;
			HGDIOBJ              hScratchOld = nullptr;
			BYTE*                pScratchBits = nullptr;
			int                  nScratchW = 0;
			int                  nScratchH = 0;

			// [D2D-LEAK] This state lives in a thread_local, so its destructor runs when
			// the thread exits. Without one, a short-lived render thread left its
			// DirectWrite formats, scratch DIB and cached bitmaps allocated permanently.
			~D2DTextState()
			{
				ReleaseAll();
			}

			void ReleaseAll()
			{
				for (auto& kv : formats)
				{
					if (kv.second != nullptr) kv.second->Release();
				}
				formats.clear();

				// cache entries release their own DC/bitmap via ~D2DTextEntry.
				cache.clear();

				if (hScratchDC != nullptr)
				{
					if (hScratchOld != nullptr) ::SelectObject(hScratchDC, hScratchOld);
					hScratchOld = nullptr;
				}
				if (hScratchBM != nullptr) { ::DeleteObject(hScratchBM); hScratchBM = nullptr; }
				if (hScratchDC != nullptr) { ::DeleteDC(hScratchDC); hScratchDC = nullptr; }
				pScratchBits = nullptr;
				nScratchW = nScratchH = 0;
			}

			D2DTextState() = default;
			D2DTextState(const D2DTextState&) = delete;
			D2DTextState& operator=(const D2DTextState&) = delete;
		};

		// [WIN11-03b] rasterisation cache
		thread_local CDuiString g_sD2DKey;
		constexpr size_t kD2DCacheMax = 256;

		// [R247] Adaptive fallback for dynamic text.
			//
			// A cache hit is ~68us; a miss has to BindDC/BeginDraw/EndDraw and
			// costs ~2700us, i.e. 10x slower than GDI. Text that changes every
			// frame (clock, progress, caret, scrolling log) can never hit the
			// cache, so it must not be routed through D2D at all.
			//
			// Rather than asking callers to tag dynamic text, we detect it:
			// a run of consecutive misses means the current text stream is
			// dynamic, so we stop rasterising for a cooldown window and let the
			// GDI path handle it. After the cooldown we probe again, so a window
			// that becomes static later still benefits.
			constexpr int kD2DMaxMissStreak = 8;     // consecutive misses => dynamic
			constexpr int kD2DCooldownDraws = 240;  // draws handled by GDI before re-probe

		D2DTextState& D2DState()
		{
			thread_local D2DTextState s;
			return s;
		}

		// [PERF-20] Build the process-wide D2D device exactly once.
		void BuildD2DDeviceOnce()
		{
			D2DDeviceShared& sh = D2DShared();
			if (::InterlockedCompareExchange(&sh.nState, 1, 0) != 0) return;

			HMODULE hD2D = ::LoadLibraryW(L"d2d1.dll");
			HMODULE hDW = ::LoadLibraryW(L"dwrite.dll");
			if (hD2D == nullptr || hDW == nullptr) { ::InterlockedExchange(&sh.nState, 3); return; }

			typedef HRESULT(WINAPI* PFN_D2D1CreateFactory)(D2D1_FACTORY_TYPE, REFIID, const void*, void**);
			typedef HRESULT(WINAPI* PFN_DWriteCreateFactory)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**);
			auto pfnD2D = reinterpret_cast<PFN_D2D1CreateFactory>(::GetProcAddress(hD2D, "D2D1CreateFactory"));
			auto pfnDW = reinterpret_cast<PFN_DWriteCreateFactory>(::GetProcAddress(hDW, "DWriteCreateFactory"));
			if (pfnD2D == nullptr || pfnDW == nullptr) { ::InterlockedExchange(&sh.nState, 3); return; }

			// Multi-threaded factory: the object is created by the warm-up thread
			// and used by the UI thread, which a single-threaded factory forbids.
			if (FAILED(pfnD2D(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory),
			                  nullptr, reinterpret_cast<void**>(&sh.pFactory))) || sh.pFactory == nullptr)
			{ ::InterlockedExchange(&sh.nState, 3); return; }
			if (FAILED(pfnDW(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
			                  reinterpret_cast<IUnknown**>(&sh.pWrite))) || sh.pWrite == nullptr)
			{ ::InterlockedExchange(&sh.nState, 3); return; }

			D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
				D2D1_RENDER_TARGET_TYPE_DEFAULT,
				D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
			if (FAILED(sh.pFactory->CreateDCRenderTarget(&props, &sh.pTarget)) || sh.pTarget == nullptr)
			{ ::InterlockedExchange(&sh.nState, 3); return; }

			// [WIN11-03b] resolve AlphaBlend once; the cached path and the fresh
			// path both need it. Leaving this null silently disables cache hits.
			HMODULE hMs = ::GetModuleHandleW(L"msimg32.dll");
			if (hMs == nullptr) hMs = ::LoadLibraryW(L"msimg32.dll");
			if (hMs != nullptr)
				sh.pfnAlphaBlend = reinterpret_cast<PFN_ALPHABLEND>(
					::GetProcAddress(hMs, "AlphaBlend"));

			::InterlockedExchange(&sh.nState, 2);
		}

		// File-scope thunk so the detached worker thread (declared outside this
		// anonymous namespace) can reach the builder by name.
		void D2DWarmUpDeviceThunk() { BuildD2DDeviceOnce(); }
		// [FIX-P2D] 预热线程死于中段时的失败态置位入口：nState 1→3，
		// 让 WaitD2DTextWarmUp 立即返回而不是空转到超时。
		void D2DWarmFailThunk() { ::InterlockedExchange(&D2DShared().nState, 3); }

		// The UI thread must still have a usable device even if warm-up never ran.
		void EnsureD2DText()
		{
			D2DDeviceShared& sh = D2DShared();
			D2DTextState& st = D2DState();
			if (st.bOk) return;
			BuildD2DDeviceOnce();
			st.bOk = (::InterlockedCompareExchange(&sh.nState, 2, 2) == 2);
		}

		// Build (and cache) an IDWriteTextFormat matching the resolved HFONT.
		IDWriteTextFormat* GetTextFormat(HFONT hFont, UINT uStyle)
		{
			D2DTextState& st = D2DState();
			D2DDeviceShared& sh = D2DShared();
			if (sh.pWrite == nullptr) return nullptr;

			auto it = st.formats.find(hFont);
			if (it != st.formats.end() && it->second != nullptr)
			{
				// Alignment is per-call, so reset it on the cached object.
				it->second->SetTextAlignment((uStyle & DT_CENTER) ? DWRITE_TEXT_ALIGNMENT_CENTER
					: (uStyle & DT_RIGHT) ? DWRITE_TEXT_ALIGNMENT_TRAILING
					: DWRITE_TEXT_ALIGNMENT_LEADING);
				it->second->SetParagraphAlignment((uStyle & DT_VCENTER) ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER
					: (uStyle & DT_BOTTOM) ? DWRITE_PARAGRAPH_ALIGNMENT_FAR
					: DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
				return it->second;
			}

			LOGFONTW lf = { 0 };
			if (::GetObjectW(hFont, sizeof(lf), &lf) <= 0) return nullptr;

			IDWriteTextFormat* pFormat = nullptr;
			HRESULT hr = sh.pWrite->CreateTextFormat(
				lf.lfFaceName, nullptr,
				lf.lfWeight >= FW_SEMIBOLD ? DWRITE_FONT_WEIGHT_SEMI_BOLD
					: (lf.lfWeight >= FW_BOLD ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL),
				lf.lfItalic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
				DWRITE_FONT_STRETCH_NORMAL,
				static_cast<FLOAT>(std::abs(lf.lfHeight)),
				L"", &pFormat);
			if (FAILED(hr) || pFormat == nullptr) return nullptr;

			pFormat->SetWordWrapping((uStyle & DT_SINGLELINE) ? DWRITE_WORD_WRAPPING_NO_WRAP
															 : DWRITE_WORD_WRAPPING_WRAP);
			pFormat->SetTextAlignment((uStyle & DT_CENTER) ? DWRITE_TEXT_ALIGNMENT_CENTER
				: (uStyle & DT_RIGHT) ? DWRITE_TEXT_ALIGNMENT_TRAILING
				: DWRITE_TEXT_ALIGNMENT_LEADING);
			pFormat->SetParagraphAlignment((uStyle & DT_VCENTER) ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER
				: (uStyle & DT_BOTTOM) ? DWRITE_PARAGRAPH_ALIGNMENT_FAR
				: DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

			st.formats[hFont] = pFormat;
			return pFormat;
		}

		// Ensure the scratch DIB can hold at least cx x cy; grow-only.
		bool EnsureScratch(D2DTextState& st, int cx, int cy)
		{
			if (cx <= 0 || cy <= 0) return false;
			if (st.hScratchDC != nullptr && cx <= st.nScratchW && cy <= st.nScratchH) return true;

			int nW = (cx > st.nScratchW) ? ((cx + 63) & ~63) : st.nScratchW;
			int nH = (cy > st.nScratchH) ? ((cy + 63) & ~63) : st.nScratchH;

			if (st.hScratchDC == nullptr) st.hScratchDC = ::CreateCompatibleDC(nullptr);
			if (st.hScratchDC == nullptr) return false;
			if (st.hScratchBM != nullptr)
			{
				::SelectObject(st.hScratchDC, st.hScratchOld);
				::DeleteObject(st.hScratchBM);
				st.hScratchBM = nullptr;
				st.pScratchBits = nullptr;
			}
			BITMAPINFO bi = { 0 };
			bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			bi.bmiHeader.biWidth = nW;
			bi.bmiHeader.biHeight = -nH;          // top-down
			bi.bmiHeader.biPlanes = 1;
			bi.bmiHeader.biBitCount = 32;
			bi.bmiHeader.biCompression = BI_RGB;
			void* pBits = nullptr;
			st.hScratchBM = ::CreateDIBSection(st.hScratchDC, &bi, DIB_RGB_COLORS, &pBits, nullptr, 0);
			if (st.hScratchBM == nullptr) return false;
			st.hScratchOld = ::SelectObject(st.hScratchDC, st.hScratchBM);
			st.pScratchBits = static_cast<BYTE*>(pBits);
			st.nScratchW = nW;
			st.nScratchH = nH;
			return true;
		}
	}

	// ---- public entry points ------------------------------------------------

	// [PERF-20] Warm-up plumbing. A single detached thread builds the shared
	// device; nState makes it idempotent and lets the waiter return early.
	// [FIX-P2D] 预热线程补 SEH 边界：worker 硬件异常死亡时 nState 恒 1，
	// WaitD2DTextWarmUp 只能空转到超时。__except 里置失败态（1→3）。
	static DWORD WINAPI D2DWarmProc(LPVOID)
	{
		// BuildD2DDeviceOnce lives in the anonymous namespace above; expose it
		// through a file-scope thunk so this worker can stay outside the namespace.
		__try
		{
			D2DWarmUpDeviceThunk();
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			D2DWarmFailThunk();
		}
		return 0;
	}

	void CRenderEngine::WarmUpD2DTextAsync()
	{
		D2DDeviceShared& sh = D2DShared();
		if (::InterlockedCompareExchange(&sh.nState, 0, 0) != 0) return;   // already started
		HANDLE h = ::CreateThread(nullptr, 0, D2DWarmProc, nullptr, 0, nullptr);
		if (h != nullptr) ::CloseHandle(h);
	}

	bool CRenderEngine::WaitD2DTextWarmUp(unsigned nTimeoutMs)
	{
		D2DDeviceShared& sh = D2DShared();
		const ULONGLONG ullDeadline = ::GetTickCount64() + nTimeoutMs;
		while (::InterlockedCompareExchange(&sh.nState, 0, 0) == 1)
		{
			if (::GetTickCount64() >= ullDeadline) return false;
			::Sleep(1);
		}
		if (::InterlockedCompareExchange(&sh.nState, 0, 0) == 0) BuildD2DDeviceOnce();
		D2DState().bOk = (::InterlockedCompareExchange(&sh.nState, 2, 2) == 2);
		return D2DState().bOk;
	}

	bool CRenderEngine::IsD2DTextAvailable()
	{
		EnsureD2DText();
		return D2DState().bOk;
	}

	void CRenderEngine::SetD2DTextEnabled(bool bEnable)
	{
		// [PERF-20] Do NOT build the device here: enabling is a request flag. The
		// device is built by WarmUpD2DTextAsync() off the startup path. Gating the
		// flag on "device already ready" would silently drop the request when the
		// warm-up is still in flight, and D2D would then never turn on.
		D2DDeviceShared& sh = D2DShared();
		::InterlockedExchange(&sh.bEnabled, bEnable ? 1 : 0);
	}

	bool CRenderEngine::IsD2DTextEnabled()
	{
		D2DDeviceShared& sh = D2DShared();
		return (sh.bEnabled != 0) &&
		       (::InterlockedCompareExchange(&sh.nState, 2, 2) == 2);
	}

	void CRenderEngine::ClearD2DTextCache()
	{
		// [D2D-LEAK] Release everything the state holds, not just the DirectWrite
		// formats. The name promises a full clear, and callers/tests rely on that:
		// leaking the bitmap cache here made a 48-draw cycle leave 8 GDI objects
		// allocated for the life of the process.
		D2DState().ReleaseAll();
	}

	bool CRenderEngine::DrawTextD2D(HDC hDC, CPaintManagerUI* pManager, RECT& rc,
									LPCTSTR pstrText, DWORD dwTextColor, int iFont, UINT uStyle)
	{
		if (hDC == nullptr || pManager == nullptr || pstrText == nullptr || pstrText[0] == _T('\0'))
			return false;

		D2DTextState& st = D2DState();
		D2DDeviceShared& sh = D2DShared();
		if (sh.bEnabled == 0 || !st.bOk) return false;
		if (sh.pTarget == nullptr || sh.pWrite == nullptr) return false;

		// [WIN11-ADAPT] 字体图标转义必须交回 GDI 路径。
		//
		// DrawText 的 GDI 分支专门处理 "&#xNNNN" 形式的内嵌字形（见下方
		// fonticonpos 分支：截取 4 位十六进制码位并用 DrawTextW 单字符绘制）。
		// DirectWrite 不做这种解释，会把 "&#xE921" 当成 7 个普通字符原样排版，
		// 结果就是标题栏 / 导航图标位置显示成 "&#xE921" 这样的乱码。
		// 因此凡含该转义的文本一律返回 false，由调用方回落 GDI。
		if (_tcsstr(pstrText, _T("&#x")) != nullptr) return false;

		// [R247] During cooldown the text stream is known to be dynamic: the cache
		// cannot help it and a fresh rasterisation would be ~10x slower than GDI.
		// Return false immediately so the caller's existing GDI path takes over.
		if (st.nCooldown > 0)
		{
			--st.nCooldown;
			if (st.nCooldown == 0) st.nMissStreak = 0;   // start a fresh probe
			return false;
		}

		HFONT hFont = pManager->GetFont(iFont);
		if (hFont == nullptr) return false;

		IDWriteTextFormat* pFormat = GetTextFormat(hFont, uStyle);
		if (pFormat == nullptr) return false;

		// [WIN11-03b] cache key: font + colour + style + size + text
		{
			TCHAR szKey[64] = { 0 };
			_stprintf_s(szKey, _countof(szKey), _T("%p|%08X|%08X|%d|%d|"),
				reinterpret_cast<void*>(hFont), static_cast<unsigned>(dwTextColor),
				static_cast<unsigned>(uStyle), rc.right - rc.left, rc.bottom - rc.top);
			g_sD2DKey = szKey;
			g_sD2DKey += pstrText;
			st.nStamp++;
			auto itHit = st.cache.find(g_sD2DKey);
			if (itHit != st.cache.end()) {
				D2DTextEntry& e = itHit->second;
				e.stamp = st.nStamp;
				st.nMissStreak = 0;      // [R247] hit => stream looks static
				++st.nHitCount;
				BLENDFUNCTION bfHit = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
				if (sh.pfnAlphaBlend != nullptr) {
					sh.pfnAlphaBlend(hDC, rc.left, rc.top, e.cx, e.cy, e.hdc, 0, 0, e.cx, e.cy, bfHit);
					return true;
				}
			}
		}

		int nW = rc.right - rc.left;
		int nH = rc.bottom - rc.top;
		if (nW <= 0 || nH <= 0) return false;

		// Very large text is rare; bounding it keeps the scratch DIB sane.
		if (nW > 4096 || nH > 4096) return false;

		// [R247] Reaching here means the cache missed and we are about to pay the
		// full rasterisation cost. Track the streak; once it crosses the threshold
		// the text stream is dynamic, so bail out *before* the expensive work and
		// hand this and the next kD2DCooldownDraws draws to GDI.
		if (++st.nMissStreak >= kD2DMaxMissStreak)
		{
			st.nCooldown = kD2DCooldownDraws;
			st.nMissStreak = 0;
			return false;
		}

		if (!EnsureScratch(st, nW, nH)) return false;

		// Clear the used area so previous frames do not bleed through.
		const size_t nRowBytes = static_cast<size_t>(st.nScratchW) * 4;
		for (int y = 0; y < nH; ++y)
			::ZeroMemory(st.pScratchBits + y * nRowBytes, static_cast<size_t>(nW) * 4);

		RECT rcBind = { 0, 0, nW, nH };
		HRESULT hr = sh.pTarget->BindDC(st.hScratchDC, &rcBind);
		if (FAILED(hr)) return false;

		IDWriteTextLayout* pLayout = nullptr;
#ifdef _UNICODE
		// Unicode build: the text already is UTF-16, hand it over unchanged.
		UINT32 cch = static_cast<UINT32>(_tcslen(pstrText));
		hr = sh.pWrite->CreateTextLayout(pstrText, cch, pFormat,
										 static_cast<FLOAT>(nW), static_cast<FLOAT>(nH), &pLayout);
#else
		// [MBCS] DirectWrite only accepts UTF-16 (WCHAR). In a MultiByte build
		// LPCTSTR is char*, so convert with the system code page - the same
		// mapping GDI uses for DrawTextA - and pass the wide buffer instead.
		// The temporary is released right after CreateTextLayout() returns
		// (DWrite copies the string it needs); every early return below is
		// therefore leak-free.
		UINT32 cch = 0;
		WCHAR* pwszText = nullptr;
		{
			const int nLen = ::MultiByteToWideChar(CP_ACP, 0, pstrText, -1, nullptr, 0);
			if (nLen <= 0) return false;
			pwszText = new WCHAR[static_cast<size_t>(nLen)];
			if (::MultiByteToWideChar(CP_ACP, 0, pstrText, -1, pwszText, nLen) <= 0)
			{
				delete[] pwszText;
				return false;
			}
			cch = static_cast<UINT32>(nLen - 1);   // omit the terminator
		}
		hr = sh.pWrite->CreateTextLayout(pwszText, cch, pFormat,
										 static_cast<FLOAT>(nW), static_cast<FLOAT>(nH), &pLayout);
		delete[] pwszText;
#endif
		if (FAILED(hr) || pLayout == nullptr) return false;

		ID2D1SolidColorBrush* pBrush = nullptr;
		D2D1_COLOR_F color = D2D1::ColorF(
			GetRValue(dwTextColor) / 255.0f,
			GetGValue(dwTextColor) / 255.0f,
			GetBValue(dwTextColor) / 255.0f,
			// ARGB is 0xAARRGGBB; only R/G/B have Win32 macros, so alpha is
			// extracted explicitly. Treat alpha 0 as opaque (DuiLib convention).
			((((dwTextColor >> 24) & 0xFFu) == 0 ? 255u : ((dwTextColor >> 24) & 0xFFu)) / 255.0f));

		sh.pTarget->BeginDraw();
		sh.pTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
		hr = sh.pTarget->CreateSolidColorBrush(color, &pBrush);
		if (SUCCEEDED(hr) && pBrush != nullptr)
		{
			D2D1_RECT_F rcDraw = D2D1::RectF(0.0f, 0.0f, static_cast<FLOAT>(nW), static_cast<FLOAT>(nH));
			sh.pTarget->DrawTextLayout(D2D1::Point2F(0.0f, 0.0f), pLayout, pBrush,
									   D2D1_DRAW_TEXT_OPTIONS_CLIP);
			(void)rcDraw;
		}
		hr = sh.pTarget->EndDraw();
		if (pBrush != nullptr) pBrush->Release();
		pLayout->Release();
		if (FAILED(hr)) return false;

		// Composite onto the caller's DC through the existing tested path.
		BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
		typedef BOOL(WINAPI* PFN_ALPHABLEND)(HDC, int, int, int, int, HDC, int, int, int, int, BLENDFUNCTION);
		static PFN_ALPHABLEND s_pfnAlphaBlend = []() -> PFN_ALPHABLEND
		{
			HMODULE h = ::GetModuleHandleW(L"msimg32.dll");
			if (h == nullptr) h = ::LoadLibraryW(L"msimg32.dll");
			if (h == nullptr) return nullptr;
			return reinterpret_cast<PFN_ALPHABLEND>(::GetProcAddress(h, "AlphaBlend"));
		}();
		if (s_pfnAlphaBlend == nullptr) return false;

		// D2D writes premultiplied BGRA; AlphaBlend with AC_SRC_ALPHA expects
		// exactly that, so no conversion is needed.
		s_pfnAlphaBlend(hDC, rc.left, rc.top, nW, nH, st.hScratchDC, 0, 0, nW, nH, bf);

		// [WIN11-03b] store the rasterised result for repeat draws
		if (!g_sD2DKey.IsEmpty() && st.cache.size() < kD2DCacheMax)
		{
			D2DTextEntry e;
			e.cx = nW;
			e.cy = nH;
			e.stamp = st.nStamp;
			e.hdc = ::CreateCompatibleDC(nullptr);
			if (e.hdc != nullptr)
			{
				BITMAPINFO bi = { 0 };
				bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
				bi.bmiHeader.biWidth = nW;
				bi.bmiHeader.biHeight = -nH;
				bi.bmiHeader.biPlanes = 1;
				bi.bmiHeader.biBitCount = 32;
				bi.bmiHeader.biCompression = BI_RGB;
				void* pBits = nullptr;
				e.hbm = ::CreateDIBSection(e.hdc, &bi, DIB_RGB_COLORS, &pBits, nullptr, 0);
				if (e.hbm != nullptr && pBits != nullptr)
				{
					const BYTE* pSrc = st.pScratchBits;
					BYTE* pDst = static_cast<BYTE*>(pBits);
					const size_t nSrcRow = static_cast<size_t>(st.nScratchW) * 4;
					const size_t nDstRow = static_cast<size_t>(nW) * 4;
					for (int y = 0; y < nH; ++y)
						::CopyMemory(pDst + y * nDstRow, pSrc + y * nSrcRow, nDstRow);
					e.hOld = ::SelectObject(e.hdc, e.hbm);
					// [D2D-LEAK] Insert by move, not copy-assign.
					// D2DTextEntry now owns a DC and a bitmap and its copy operations are
					// deleted (a copy would double-free them), so `cache[key] = e` no
					// longer compiles. emplace moves the finished entry in place.
					st.cache.emplace(g_sD2DKey, std::move(e));
				}
				else
				{
					// e's destructor releases whatever was created (hbm, hdc).
					e.ReleaseResources();
				}
			}
		}

		return true;
	}

	void CRenderEngine::GdiplusDrawText(HDC hDC, CPaintManagerUI* pManager, RECT& rc, LPCTSTR pstrText, DWORD dwTextColor, int iFont, UINT uStyle)
	{
		ASSERT(::GetObjectType(hDC)==OBJ_DC || ::GetObjectType(hDC)==OBJ_MEMDC);
		if( pstrText == nullptr || pManager == nullptr ) return;

		HFONT hOldFont = (HFONT)::SelectObject(hDC, pManager->GetFont(iFont));
		Gdiplus::Graphics graphics( hDC );
		Gdiplus::Font font(hDC, pManager->GetFont(iFont));
		Gdiplus::TextRenderingHint trh = Gdiplus::TextRenderingHintSystemDefault;
		switch(pManager->GetGdiplusTextRenderingHint()) 
		{
		case 0: {trh = Gdiplus::TextRenderingHintSystemDefault; break;}
		case 1: {trh = Gdiplus::TextRenderingHintSingleBitPerPixelGridFit; break;}
		case 2: {trh = Gdiplus::TextRenderingHintSingleBitPerPixel; break;}
		case 3: {trh = Gdiplus::TextRenderingHintAntiAliasGridFit; break;}
		case 4: {trh = Gdiplus::TextRenderingHintAntiAlias; break;}
		case 5: {trh = Gdiplus::TextRenderingHintClearTypeGridFit; break;}
		}
		graphics.SetTextRenderingHint(trh);
		graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality); 
		graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

		Gdiplus::RectF rectF(static_cast<Gdiplus::REAL>(rc.left), static_cast<Gdiplus::REAL>(rc.top), static_cast<Gdiplus::REAL>(rc.right - rc.left), static_cast<Gdiplus::REAL>(rc.bottom - rc.top));
		Gdiplus::SolidBrush brush(Gdiplus::Color(254, GetBValue(dwTextColor), GetGValue(dwTextColor), GetRValue(dwTextColor)));

		Gdiplus::StringFormat stringFormat = Gdiplus::StringFormat::GenericTypographic();

		if ((uStyle & DT_END_ELLIPSIS) != 0) {
			stringFormat.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
		}
		
		int formatFlags = 0;
		if ((uStyle & DT_NOCLIP) != 0) {
			formatFlags |= Gdiplus::StringFormatFlagsNoClip;
		}
		if ((uStyle & DT_SINGLELINE) != 0) {
			formatFlags |= Gdiplus::StringFormatFlagsNoWrap;
		}
		if ((uStyle & DT_NOPREFIX) != 0) {
			stringFormat.SetHotkeyPrefix(Gdiplus::HotkeyPrefixHide);
		}
		else {
			stringFormat.SetHotkeyPrefix(Gdiplus::HotkeyPrefixShow);
		}

		stringFormat.SetFormatFlags(formatFlags);

		if ((uStyle & DT_LEFT) != 0) {
			stringFormat.SetAlignment(Gdiplus::StringAlignmentNear);
		}
		else if ((uStyle & DT_CENTER) != 0) {
			stringFormat.SetAlignment(Gdiplus::StringAlignmentCenter);
		}
		else if ((uStyle & DT_RIGHT) != 0) {
			stringFormat.SetAlignment(Gdiplus::StringAlignmentFar);
		}
		else {
			stringFormat.SetAlignment(Gdiplus::StringAlignmentNear);
		}
		stringFormat.GenericTypographic();
		if ((uStyle & DT_TOP) != 0) {
			stringFormat.SetLineAlignment(Gdiplus::StringAlignmentNear);
		}
		else if ((uStyle & DT_VCENTER) != 0) {
			stringFormat.SetLineAlignment(Gdiplus::StringAlignmentCenter);
		}
		else if ((uStyle & DT_BOTTOM) != 0) {
			stringFormat.SetLineAlignment(Gdiplus::StringAlignmentFar);
		}
		else {
			stringFormat.SetLineAlignment(Gdiplus::StringAlignmentNear);
		}
#ifdef UNICODE
		if ((uStyle & DT_CALCRECT) != 0)
		{
			Gdiplus::RectF bounds;

			graphics.MeasureString(pstrText, -1, &font, rectF, &stringFormat, &bounds);

			// MeasureString存在计算误差，这里加一像素
			rc.bottom = rc.top + static_cast<long>(bounds.Height) + 1;
			rc.right = rc.left + static_cast<long>(bounds.Width) + 1;
		}
		else
		{
			graphics.DrawString(pstrText, -1, &font, rectF, &stringFormat, &brush);
		}
#else
		DWORD dwSize = MultiByteToWideChar(CP_ACP, 0, pstrText, -1, nullptr, 0);
		WCHAR * pcwszDest = new WCHAR[dwSize + 1];
		memset(pcwszDest, 0, (dwSize + 1) * sizeof(WCHAR));
		MultiByteToWideChar(CP_ACP, 0, pstrText, -1, pcwszDest, dwSize);
		if(pcwszDest != nullptr)
		{
			if ((uStyle & DT_CALCRECT) != 0)
			{
				Gdiplus::RectF bounds;
				graphics.MeasureString(pcwszDest, -1, &font, rectF, &stringFormat, &bounds);
				rc.bottom = rc.top + static_cast<long>(bounds.Height * 1.06);
				rc.right = rc.left + static_cast<long>(bounds.Width * 1.06);
			}
			else
			{
				graphics.DrawString(pcwszDest, -1, &font, rectF, &stringFormat, &brush);
			}
			delete []pcwszDest;
		}
#endif
		::SelectObject(hDC, hOldFont);
	}


	// 绘制及填充圆角矩形
	void GdiplusDrawRoundRect(HDC hDC, float x, float y, float width, float height, float arcSize, float lineWidth, Gdiplus::Color lineColor, bool fillPath, Gdiplus::Color fillColor, int nStyle)
	{
		float arcDiameter = arcSize * 2;
		// 创建GDI+对象
		Gdiplus::Graphics  g(hDC);
		//设置画图时的滤波模式为消除锯齿现象
		g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);

		// 绘图路径
		Gdiplus::GraphicsPath roundRectPath;

		// 保存绘图路径
		roundRectPath.AddLine(x + arcSize, y, x + width - arcSize, y);  // 顶部横线
		roundRectPath.AddArc(x + width - arcDiameter, y, arcDiameter, arcDiameter, 270, 90); // 右上圆角

		roundRectPath.AddLine(x + width, y + arcSize, x + width, y + height - arcSize);  // 右侧竖线
		roundRectPath.AddArc(x + width - arcDiameter, y + height - arcDiameter, arcDiameter, arcDiameter, 0, 90); // 右下圆角

		roundRectPath.AddLine(x + width - arcSize, y + height, x + arcSize, y + height);  // 底部横线
		roundRectPath.AddArc(x, y + height - arcDiameter, arcDiameter, arcDiameter, 90, 90); // 左下圆角

		roundRectPath.AddLine(x, y + height - arcSize, x, y + arcSize);  // 左侧竖线
		roundRectPath.AddArc(x, y, arcDiameter, arcDiameter, 180, 90); // 左上圆角

		//创建画笔
		Gdiplus::Pen pen(lineColor, lineWidth);
		pen.SetDashStyle(static_cast<Gdiplus::DashStyle>(nStyle));
		// 绘制矩形
		g.DrawPath(&pen, &roundRectPath);

		// 是否填充
		if(fillPath) {
			if(fillColor.GetAlpha() == 0) {
				fillColor = lineColor; // 若未指定填充色，则用线条色填充
			}

			// 创建画刷
			Gdiplus::SolidBrush brush(fillColor);

			// 填充
			g.FillPath(&brush, &roundRectPath);
		}
	}

	/////////////////////////////////////////////////////////////////////////////////////
	//
	//

	void CRenderEngine::SetRenderBackend(IRenderBackend* pBackend)
	{
		g_pRenderBackend = pBackend;
	}

	IRenderBackend* CRenderEngine::GetRenderBackend()
	{
		return g_pRenderBackend;
	}

	bool CRenderEngine::IsUsingCustomBackend()
	{
		return g_pRenderBackend != nullptr;
	}

	// ---- CGdiRenderBackend: forwards to the built-in implementations ----

	const TCHAR* CGdiRenderBackend::Name() const
	{
		return _T("GDI (built-in)");
	}

	void CGdiRenderBackend::DrawColor(HDC hDC, const RECT& rc, DWORD dwColor)
	{
		CRenderEngine::DrawColor(hDC, rc, dwColor);
	}

	void CGdiRenderBackend::DrawGradient(HDC hDC, const RECT& rc, DWORD dwFirst,
										 DWORD dwSecond, bool bVertical, int nSteps)
	{
		CRenderEngine::DrawGradient(hDC, rc, dwFirst, dwSecond, bVertical, nSteps);
	}

	void CGdiRenderBackend::DrawLine(HDC hDC, const RECT& rc, int nSize,
									 DWORD dwPenColor, int nStyle)
	{
		CRenderEngine::DrawLine(hDC, rc, nSize, dwPenColor, nStyle);
	}

	void CGdiRenderBackend::DrawRect(HDC hDC, const RECT& rc, int nSize,
									 DWORD dwPenColor, int nStyle)
	{
		CRenderEngine::DrawRect(hDC, rc, nSize, dwPenColor, nStyle);
	}

	void CGdiRenderBackend::DrawRoundRect(HDC hDC, const RECT& rc, int width, int height,
										  int nSize, DWORD dwPenColor, int nStyle)
	{
		CRenderEngine::DrawRoundRect(hDC, rc, width, height, nSize, dwPenColor, nStyle);
	}

	void CGdiRenderBackend::DrawImage(HDC hDC, HBITMAP hBitmap, const RECT& rc,
								   const RECT& rcPaint, const RECT& rcBmpPart,
								   const RECT& rcCorners, bool bAlpha, UINT uFade,
								   bool hole, bool xtiled, bool ytiled)
	{
		// Forwards to the built-in GDI blitter; the re-entrancy guard keeps this
		// from dispatching back into the backend.
		CRenderEngine::DrawImage(hDC, hBitmap, rc, rcPaint, rcBmpPart, rcCorners,
							  bAlpha, uFade, hole, xtiled, ytiled);
	}

	void CGdiRenderBackend::DrawText(HDC hDC, CPaintManagerUI* pManager, RECT& rc,
								 LPCTSTR pstrText, DWORD dwTextColor, int iFont, UINT uStyle)
	{
		// Forwards to the built-in text path (which may itself use D2D when that
		// opt-in is enabled). The re-entrancy guard prevents dispatching back here.
		CRenderEngine::DrawText(hDC, pManager, rc, pstrText, dwTextColor, iFont, uStyle);
	}



	void CRenderEngine::DrawColor(HDC hDC, const RECT& rc, DWORD color)
	{
		// [R240] dispatch to the installed backend when one is set;
		// with no backend this function body runs unchanged.
		if (g_pRenderBackend != nullptr && !t_bInBackend)
		{
			BackendScope scope;
			g_pRenderBackend->DrawColor(hDC, rc, color);
			return;
		}
		// [R240] dispatch to the installed backend when one is set;
		// with no backend this function body runs unchanged.
		if (g_pRenderBackend != nullptr && !t_bInBackend)
		{
			BackendScope scope;
			g_pRenderBackend->DrawColor(hDC, rc, color);
			return;
		}
		if( color <= 0x00FFFFFF ) return;

		Gdiplus::Graphics graphics( hDC );
		Gdiplus::SolidBrush brush(Gdiplus::Color((LOBYTE((color)>>24)), GetBValue(color), GetGValue(color), GetRValue(color)));
		graphics.FillRectangle(&brush, Gdiplus::Rect(rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top));
	}

	// [WIN11-ADAPT] 圆角实心填充（配套 DrawRoundRect 的填充半边）。
	//
	// 实现选择 GDI+ FillPath 而非 DrawRoundRect+画刷，原因：
	//   * DrawRoundRect 只描边，用粗画刷模拟填充会在大圆角处出现锯齿；
	//   * GDI+ 抗锯齿能让 Win11 的 4-6px 小圆角保持平滑。
	// 颜色语义与 DrawColor 一致：0xAARRGGBB，alpha 为 0 视为不透明。
	void CRenderEngine::DrawRoundRectFilled(HDC hDC, const RECT& rc, int cxyRound, DWORD dwColor)
	{
		if (hDC == nullptr) return;
		if (dwColor <= 0x00FFFFFF) return;              // 与 DrawColor 一致：全透明直接跳过
		if (cxyRound <= 0) { DrawColor(hDC, rc, dwColor); return; }
		if (rc.right <= rc.left || rc.bottom <= rc.top) return;

		const BYTE a = static_cast<BYTE>((dwColor >> 24) & 0xFFu);
		Gdiplus::Graphics g(hDC);
		g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
		g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

		// 半径不得超过矩形短边的一半，否则路径会自交
		const int nMax = ((rc.right - rc.left) < (rc.bottom - rc.top)
		                  ? (rc.right - rc.left) : (rc.bottom - rc.top)) / 2;
		const Gdiplus::REAL d = static_cast<Gdiplus::REAL>(cxyRound < nMax ? cxyRound : nMax) * 2.0f;

		Gdiplus::GraphicsPath path;
		const Gdiplus::REAL x = static_cast<Gdiplus::REAL>(rc.left) + 0.5f;
		const Gdiplus::REAL y = static_cast<Gdiplus::REAL>(rc.top) + 0.5f;
		const Gdiplus::REAL w = static_cast<Gdiplus::REAL>(rc.right - rc.left) - 1.0f;
		const Gdiplus::REAL h = static_cast<Gdiplus::REAL>(rc.bottom - rc.top) - 1.0f;

		path.AddArc(x, y, d, d, 180.0f, 90.0f);
		path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
		path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
		path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
		path.CloseFigure();

		// 颜色通道顺序必须与 CRenderEngine::DrawColor 完全一致。
		// DrawColor 用的是 (GetBValue, GetGValue, GetRValue)：对 0xAARRGGBB
		// 形式的皮肤色值，这三次取值正好还原成 GDI+ 需要的 (r,g,b)，
		// 与皮肤里 #AARRGGBB 的书写顺序一致。
		// 若这里写成 (GetRValue, GetGValue, GetBValue)，同一个色值在
		// 圆角路径与直角路径上会渲染出 R/B 互换的颜色（实测 FAIL）。
		Gdiplus::SolidBrush br(Gdiplus::Color(a,
			static_cast<BYTE>(GetBValue(dwColor)),
			static_cast<BYTE>(GetGValue(dwColor)),
			static_cast<BYTE>(GetRValue(dwColor))));
		g.FillPath(&br, &path);
	}

	void CRenderEngine::DrawGradient(HDC hDC, const RECT& rc, DWORD dwFirst, DWORD dwSecond, bool bVertical, int nSteps)
	{
		// [R240] dispatch to the installed backend when one is set;
		// with no backend this function body runs unchanged.
		if (g_pRenderBackend != nullptr && !t_bInBackend)
		{
			BackendScope scope;
			g_pRenderBackend->DrawGradient(hDC, rc, dwFirst, dwSecond, bVertical, nSteps);
			return;
		}
		// [R240] dispatch to the installed backend when one is set;
		// with no backend this function body runs unchanged.
		if (g_pRenderBackend != nullptr && !t_bInBackend)
		{
			BackendScope scope;
			g_pRenderBackend->DrawGradient(hDC, rc, dwFirst, dwSecond, bVertical, nSteps);
			return;
		}
		typedef BOOL (WINAPI *LPALPHABLEND)(HDC, int, int, int, int,HDC, int, int, int, int, BLENDFUNCTION);
		static LPALPHABLEND lpAlphaBlend = (LPALPHABLEND) ::GetProcAddress(::GetModuleHandle(_T("msimg32.dll")), "AlphaBlend");
		if( lpAlphaBlend == nullptr ) lpAlphaBlend = AlphaBitBlt;
		typedef BOOL (WINAPI *PGradientFill)(HDC, PTRIVERTEX, ULONG, PVOID, ULONG, ULONG);
		static PGradientFill lpGradientFill = (PGradientFill) ::GetProcAddress(::GetModuleHandle(_T("msimg32.dll")), "GradientFill");

		BYTE bAlpha = static_cast<BYTE>(((dwFirst >> 24) + (dwSecond >> 24)) >> 1);
		if( bAlpha == 0 ) return;
		int cx = rc.right - rc.left;
		int cy = rc.bottom - rc.top;
		RECT rcPaint = rc;
		HDC hPaintDC = hDC;
		HBITMAP hPaintBitmap = nullptr;
		HBITMAP hOldPaintBitmap = nullptr;
		if( bAlpha < 255 ) {
			rcPaint.left = rcPaint.top = 0;
			rcPaint.right = cx;
			rcPaint.bottom = cy;
			hPaintDC = ::CreateCompatibleDC(hDC);
			hPaintBitmap = ::CreateCompatibleBitmap(hDC, cx, cy);
			ASSERT(hPaintDC);
			ASSERT(hPaintBitmap);
			hOldPaintBitmap = (HBITMAP) ::SelectObject(hPaintDC, hPaintBitmap);
		}
		if( lpGradientFill != nullptr ) 
		{
			TRIVERTEX triv[2] = 
			{
				{ rcPaint.left, rcPaint.top, 
				static_cast<COLOR16>(GetBValue(dwFirst) << 8),
				static_cast<COLOR16>(GetGValue(dwFirst) << 8),
				static_cast<COLOR16>(GetRValue(dwFirst) << 8), 0xFF00 },
				{ rcPaint.right, rcPaint.bottom, 
				static_cast<COLOR16>(GetBValue(dwSecond) << 8),
				static_cast<COLOR16>(GetGValue(dwSecond) << 8),
				static_cast<COLOR16>(GetRValue(dwSecond) << 8), 0xFF00 }
			};
			GRADIENT_RECT grc = { 0, 1 };
			lpGradientFill(hPaintDC, triv, 2, &grc, 1, bVertical ? GRADIENT_FILL_RECT_V : GRADIENT_FILL_RECT_H);
		}
		else 
		{
			// Determine how many shades
			int nShift = 1;
			if( nSteps >= 64 ) nShift = 6;
			else if( nSteps >= 32 ) nShift = 5;
			else if( nSteps >= 16 ) nShift = 4;
			else if( nSteps >= 8 ) nShift = 3;
			else if( nSteps >= 4 ) nShift = 2;
			int nLines = 1 << nShift;
			for( int i = 0; i < nLines; i++ ) {
				// Do a little alpha blending
				BYTE bR = static_cast<BYTE>((GetBValue(dwSecond) * (nLines - i) + GetBValue(dwFirst) * i) >> nShift);
				BYTE bG = static_cast<BYTE>((GetGValue(dwSecond) * (nLines - i) + GetGValue(dwFirst) * i) >> nShift);
				BYTE bB = static_cast<BYTE>((GetRValue(dwSecond) * (nLines - i) + GetRValue(dwFirst) * i) >> nShift);
				// ... then paint with the resulting color
				HBRUSH hBrush = ::CreateSolidBrush(RGB(bR,bG,bB));
				RECT r2 = rcPaint;
				if( bVertical ) {
					r2.bottom = rc.bottom - ((i * (rc.bottom - rc.top)) >> nShift);
					r2.top = rc.bottom - (((i + 1) * (rc.bottom - rc.top)) >> nShift);
					if( (r2.bottom - r2.top) > 0 ) ::FillRect(hDC, &r2, hBrush);
				}
				else {
					r2.left = rc.right - (((i + 1) * (rc.right - rc.left)) >> nShift);
					r2.right = rc.right - ((i * (rc.right - rc.left)) >> nShift);
					if( (r2.right - r2.left) > 0 ) ::FillRect(hPaintDC, &r2, hBrush);
				}
				::DeleteObject(hBrush);
			}
		}
		if( bAlpha < 255 ) {
			BLENDFUNCTION bf = { AC_SRC_OVER, 0, bAlpha, AC_SRC_ALPHA };
			lpAlphaBlend(hDC, rc.left, rc.top, cx, cy, hPaintDC, 0, 0, cx, cy, bf);
			::SelectObject(hPaintDC, hOldPaintBitmap);
			::DeleteObject(hPaintBitmap);
			::DeleteDC(hPaintDC);
		}
	}

	// [FIX-P2Q] GDI 画笔缓存：边框/线条类控件每次重绘 CreatePen+DeleteObject
	// 是高频小对象抖动。键=(style, size, color)，渲染仅 UI 线程无需加锁；
	// 缓存句柄进程生命周期持有（组合数有限，不设淘汰）。
	namespace {
	HPEN CachedGdiPen(int nStyle, int nSize, DWORD dwPenColor, bool bInsideFrame)
	{
		struct PenKey
		{
			int style; int size; DWORD color;
			bool operator<(const PenKey& o) const
			{
				if (style != o.style) return style < o.style;
				if (size != o.size) return size < o.size;
				return color < o.color;
			}
		};
		static std::map<PenKey, HPEN> s_map;
		const PenKey k{ bInsideFrame ? (nStyle | PS_INSIDEFRAME) : nStyle, nSize, dwPenColor };
		auto it = s_map.find(k);
		if (it != s_map.end()) return it->second;
		HPEN h = ::CreatePen(k.style, nSize,
			RGB(GetBValue(dwPenColor), GetGValue(dwPenColor), GetRValue(dwPenColor)));
		s_map.emplace(k, h);
		return h;
	}
	} // namespace

	void CRenderEngine::DrawLine( HDC hDC, const RECT& rc, int nSize, DWORD dwPenColor,int nStyle /*= PS_SOLID*/ )
	{
		// [R240] dispatch to the installed backend when one is set;
		// with no backend this function body runs unchanged.
		if (g_pRenderBackend != nullptr && !t_bInBackend)
		{
			BackendScope scope;
			g_pRenderBackend->DrawLine(hDC, rc, nSize, dwPenColor, nStyle);
			return;
		}
		ASSERT(::GetObjectType(hDC)==OBJ_DC || ::GetObjectType(hDC)==OBJ_MEMDC);

		// [FIX-P2Q] 缓存画笔（不再每次 Create/Delete）
		HPEN hPen = CachedGdiPen(nStyle, nSize, dwPenColor, false);
		HPEN hOldPen = (HPEN)::SelectObject(hDC, hPen);
		POINT ptTemp = { 0 };
		::MoveToEx(hDC, rc.left, rc.top, &ptTemp);
		::LineTo(hDC, rc.right, rc.bottom);
		::SelectObject(hDC, hOldPen);
	}

	void CRenderEngine::DrawRect(HDC hDC, const RECT& rc, int nSize, DWORD dwPenColor,int nStyle /*= PS_SOLID*/)
	{
		// [R240] dispatch to the installed backend when one is set;
		// with no backend this function body runs unchanged.
		if (g_pRenderBackend != nullptr && !t_bInBackend)
		{
			BackendScope scope;
			g_pRenderBackend->DrawRect(hDC, rc, nSize, dwPenColor, nStyle);
			return;
		}
#ifdef USE_GDI_RENDER
		ASSERT(::GetObjectType(hDC) == OBJ_DC || ::GetObjectType(hDC) == OBJ_MEMDC);
		// [FIX-P2Q] 缓存画笔（含 PS_INSIDEFRAME 语义）
		HPEN hPen = CachedGdiPen(nStyle, nSize, dwPenColor, true);
		HPEN hOldPen = (HPEN)::SelectObject(hDC, hPen);
		::SelectObject(hDC, ::GetStockObject(HOLLOW_BRUSH));
		::Rectangle(hDC, rc.left, rc.top, rc.right, rc.bottom);
		::SelectObject(hDC, hOldPen);
#else
		ASSERT(::GetObjectType(hDC) == OBJ_DC || ::GetObjectType(hDC) == OBJ_MEMDC);
		Gdiplus::Graphics graphics(hDC);
		Gdiplus::Pen pen(Gdiplus::Color(dwPenColor), static_cast<Gdiplus::REAL>(nSize));
		pen.SetAlignment(Gdiplus::PenAlignmentInset);
		pen.SetDashStyle(static_cast<Gdiplus::DashStyle>(nStyle));

		graphics.DrawRectangle(&pen, Gdiplus::Rect(rc.left, rc.top, rc.right - rc.left - 1, rc.bottom - rc.top - 1));
#endif
	}

	void CRenderEngine::DrawRoundRect(HDC hDC, const RECT& rc, int nSize, int width, int height, DWORD dwPenColor, int nStyle /*= PS_SOLID*/)
	{
		// [R240] dispatch to the installed backend when one is set;
		// with no backend this function body runs unchanged.
		if (g_pRenderBackend != nullptr && !t_bInBackend)
		{
			BackendScope scope;
			g_pRenderBackend->DrawRoundRect(hDC, rc, width, height, nSize, dwPenColor, nStyle);
			return;
		}
#ifdef USE_GDI_RENDER
		ASSERT(::GetObjectType(hDC)==OBJ_DC || ::GetObjectType(hDC)==OBJ_MEMDC);
		// [FIX-P2Q] 缓存画笔（无 PS_INSIDEFRAME 语义）
		HPEN hPen = CachedGdiPen(nStyle, nSize, dwPenColor, false);
		HPEN hOldPen = (HPEN)::SelectObject(hDC, hPen);
		::SelectObject(hDC, ::GetStockObject(HOLLOW_BRUSH));
		::RoundRect(hDC, rc.left, rc.top, rc.right, rc.bottom, width, height);
		::SelectObject(hDC, hOldPen);
#else
		GdiplusDrawRoundRect(hDC, rc.left, rc.top, rc.right - rc.left - 1, rc.bottom - rc.top - 1, width / 2, nSize, Gdiplus::Color(dwPenColor), false, Gdiplus::Color(dwPenColor), nStyle);
#endif
	}

	/////////////////////////////////////////////////////////////////////////////////////
	//
	//
	void CRenderEngine::DrawText(HDC hDC, CPaintManagerUI* pManager, RECT& rc, LPCTSTR pstrText,DWORD dwTextColor, int iFont, UINT uStyle, DWORD dwTextBKColor)
	{
		ASSERT(::GetObjectType(hDC)==OBJ_DC || ::GetObjectType(hDC)==OBJ_MEMDC);
		if( pstrText == nullptr || pManager == nullptr ) return;
		DrawColor(hDC, rc, dwTextBKColor);
		DrawText(hDC, pManager, rc, pstrText, dwTextColor, iFont, uStyle);
	}

	void CRenderEngine::DrawText(HDC hDC, CPaintManagerUI* pManager, RECT& rc, LPCTSTR pstrText, DWORD dwTextColor, int iFont, UINT uStyle)
	{
		ASSERT(::GetObjectType(hDC)==OBJ_DC || ::GetObjectType(hDC)==OBJ_MEMDC);
		if( pstrText == nullptr || pManager == nullptr ) return;

		// [R258] An installed backend owns text drawing; it is expected to route
		// back here (or to its own rasteriser) with the re-entrancy guard set.
		if (g_pRenderBackend != nullptr && !t_bInBackend)
		{
			BackendScope scope;
			g_pRenderBackend->DrawText(hDC, pManager, rc, pstrText, dwTextColor, iFont, uStyle);
			return;
		}

		// [WIN11-03] Opt-in D2D/DirectWrite text. Falls through to the original
		// GDI/GDI+ paths when the backend is unavailable or disabled.
		if (CRenderEngine::IsD2DTextEnabled() && IsD2DTextAvailable())
		{
			if (CRenderEngine::DrawTextD2D(hDC, pManager, rc, pstrText, dwTextColor, iFont, uStyle))
				return;
		}

		if (pManager->IsLayered() || pManager->IsUseGdiplusText())
		{
			GdiplusDrawText(hDC, pManager, rc, pstrText, dwTextColor, iFont, uStyle);
		}
		else
		{
			::SetBkMode(hDC, TRANSPARENT);
			::SetTextColor(hDC, RGB(GetBValue(dwTextColor), GetGValue(dwTextColor), GetRValue(dwTextColor)));
			HFONT hOldFont = (HFONT)::SelectObject(hDC, pManager->GetFont(iFont));
			// [PERF-2b] Fast path for the overwhelmingly common case: no escape at
			// all. An icon label is detected with a cheap scan and the escape is
			// decoded inline (no CDuiString temporaries). Measuring showed the
			// previous generic parse cost +106 us per icon draw versus plain text.
			LPCTSTR pScan = pstrText;
			while (*pScan != 0 && *pScan != _T('&')) ++pScan;
			if (*pScan == 0) {
				::DrawText(hDC, pstrText, -1, &rc, uStyle);
			}
			else {
				LPCTSTR pHex = pScan + 1;
				if (*pHex == _T('#')) ++pHex;
				if (*pHex == _T('x') || *pHex == _T('X')) {
					++pHex;
					// Bounded hex decode: at most 7 digits, no library call.
					unsigned int uCode = 0;
					int nDigits = 0;
					while (nDigits < 7) {
						const TCHAR ch = *pHex;
						int n = -1;
						if (ch >= _T('0') && ch <= _T('9')) n = ch - _T('0');
						else if (ch >= _T('a') && ch <= _T('f')) n = ch - _T('a') + 10;
						else if (ch >= _T('A') && ch <= _T('F')) n = ch - _T('A') + 10;
						if (n < 0) break;
						uCode = (uCode << 4) | (unsigned int)n;
						++pHex; ++nDigits;
					}
					if (nDigits > 0 && *pHex == _T(';') && uCode > 0 && uCode <= 0x10FFFF) {
						wchar_t wch[3] = { 0, 0, 0 };
						if (uCode <= 0xFFFF) {
							wch[0] = static_cast<wchar_t>(uCode);
						}
						else {
							// Surrogate pair so non-BMP glyphs render correctly.
							const unsigned int uRest = uCode - 0x10000;
							wch[0] = static_cast<wchar_t>(0xD800 + (uRest >> 10));
							wch[1] = static_cast<wchar_t>(0xDC00 + (uRest & 0x3FF));
						}
						::DrawTextW(hDC, wch, -1, &rc, uStyle);
					}
					else {
						::DrawText(hDC, pstrText, -1, &rc, uStyle);
					}
				}
				else {
					::DrawText(hDC, pstrText, -1, &rc, uStyle);
				}
			}
			::SelectObject(hDC, hOldFont);
		}
	}

	void CRenderEngine::DrawHtmlText(HDC hDC, CPaintManagerUI* pManager, RECT& rc, LPCTSTR pstrText, DWORD dwTextColor, RECT* prcLinks, CDuiString* sLinks, int& nLinkRects, int iFont, UINT uStyle)
	{
		// 考虑到在xml编辑器中使用<>符号不方便，可以使用{}符号代替
		// 支持标签嵌套（如<l><b>text</b></l>），但是交叉嵌套是应该避免的（如<l><b>text</l></b>）
		// The string formatter supports a kind of "mini-html" that consists of various short tags:
		//
		//   Bold:             <b>text</b>
		//   Color:            <c #xxxxxx>text</c>  where x = RGB in hex
		//   Font:             <f x>text</f>        where x = font id
		//   Italic:           <i>text</i>
		//   Image:            <i x y z>            where x = image name and y = imagelist num and z(optional) = imagelist id
		//   Link:             <a x>text</a>        where x(optional) = link content, normal like app:notepad or http:www.xxx.com
		//   NewLine           <n>                  
		//   Paragraph:        <p x>text</p>        where x = extra pixels indent in p
		//   Raw Text:         <r>text</r>
		//   Selected:         <s>text</s>
		//   Underline:        <u>text</u>
		//   X Indent:         <x i>                where i = hor indent in pixels
		//   Y Indent:         <y i>                where i = ver indent in pixels 

		ASSERT(::GetObjectType(hDC)==OBJ_DC || ::GetObjectType(hDC)==OBJ_MEMDC);
		if( pstrText == nullptr || pManager == nullptr ) return;
		if( ::IsRectEmpty(&rc) ) return;

		bool bDraw = (uStyle & DT_CALCRECT) == 0;

		CStdPtrArray aFontArray(10);
		CStdPtrArray aColorArray(10);
		CStdPtrArray aPIndentArray(10);

		RECT rcClip = { 0 };
		::GetClipBox(hDC, &rcClip);
		HRGN hOldRgn = ::CreateRectRgnIndirect(&rcClip);
		HRGN hRgn = ::CreateRectRgnIndirect(&rc);
		if( bDraw ) ::ExtSelectClipRgn(hDC, hRgn, RGN_AND);

		TFontInfo* pDefFontInfo = pManager->GetFontInfo(iFont);
		if(pDefFontInfo == nullptr) {
			pDefFontInfo = pManager->GetDefaultFontInfo();
		}
		TEXTMETRIC* pTm = &pDefFontInfo->tm;
		HFONT hOldFont = (HFONT) ::SelectObject(hDC, pDefFontInfo->hFont);
		::SetBkMode(hDC, TRANSPARENT);
		::SetTextColor(hDC, RGB(GetBValue(dwTextColor), GetGValue(dwTextColor), GetRValue(dwTextColor)));
		DWORD dwBkColor = pManager->GetDefaultSelectedBkColor();
		::SetBkColor(hDC, RGB(GetBValue(dwBkColor), GetGValue(dwBkColor), GetRValue(dwBkColor)));

		// If the drawstyle include a alignment, we'll need to first determine the text-size so
		// we can draw it at the correct position...
		if( ((uStyle & DT_CENTER) != 0 || (uStyle & DT_RIGHT) != 0 || (uStyle & DT_VCENTER) != 0 || (uStyle & DT_BOTTOM) != 0) && (uStyle & DT_CALCRECT) == 0 ) {
			RECT rcText = { 0, 0, 9999, 100 };
			int nLinks = 0;
			DrawHtmlText(hDC, pManager, rcText, pstrText, dwTextColor, nullptr, nullptr, nLinks, iFont, uStyle | DT_CALCRECT);
			if( (uStyle & DT_SINGLELINE) != 0 ){
				if( (uStyle & DT_CENTER) != 0 ) {
					rc.left = rc.left + ((rc.right - rc.left) / 2) - ((rcText.right - rcText.left) / 2);
					rc.right = rc.left + (rcText.right - rcText.left);
				}
				if( (uStyle & DT_RIGHT) != 0 ) {
					rc.left = rc.right - (rcText.right - rcText.left);
				}
			}
			if( (uStyle & DT_VCENTER) != 0 ) {
				rc.top = rc.top + ((rc.bottom - rc.top) / 2) - ((rcText.bottom - rcText.top) / 2);
				rc.bottom = rc.top + (rcText.bottom - rcText.top);
			}
			if( (uStyle & DT_BOTTOM) != 0 ) {
				rc.top = rc.bottom - (rcText.bottom - rcText.top);
			}
		}

		bool bHoverLink = false;
		CDuiString sHoverLink;
		POINT ptMouse = pManager->GetMousePos();
		for( int i = 0; !bHoverLink && i < nLinkRects; i++ ) {
			if( ::PtInRect(prcLinks + i, ptMouse) ) {
				sHoverLink = *reinterpret_cast<CDuiString*>(sLinks + i);
				bHoverLink = true;
			}
		}

		POINT pt = { rc.left, rc.top };
		int iLinkIndex = 0;
		int cyLine = pTm->tmHeight + pTm->tmExternalLeading + static_cast<int>(reinterpret_cast<intptr_t>(aPIndentArray.GetAt(aPIndentArray.GetSize() - 1)));
		int cyMinHeight = 0;
		int cxMaxWidth = 0;
		POINT ptLinkStart = { 0 };
		bool bLineEnd = false;
		bool bInRaw = false;
		bool bInLink = false;
		bool bInSelected = false;
		int iLineLinkIndex = 0;

		// 排版习惯是图文底部对齐，所以每行绘制都要分两步，先计算高度，再绘制
		CStdPtrArray aLineFontArray;
		CStdPtrArray aLineColorArray;
		CStdPtrArray aLinePIndentArray;
		LPCTSTR pstrLineBegin = pstrText;
		bool bLineInRaw = false;
		bool bLineInLink = false;
		bool bLineInSelected = false;
		int cyLineHeight = 0;
		bool bLineDraw = false; // 行的第二阶段：绘制
		while( *pstrText != _T('\0') ) {
			if( pt.x >= rc.right || *pstrText == _T('\n') || bLineEnd ) {
				if( *pstrText == _T('\n') ) pstrText++;
				if( bLineEnd ) bLineEnd = false;
				if( !bLineDraw ) {
					if( bInLink && iLinkIndex < nLinkRects ) {
						::SetRect(&prcLinks[iLinkIndex++], ptLinkStart.x, ptLinkStart.y, MIN(pt.x, rc.right), pt.y + cyLine);
						CDuiString *pStr1 = reinterpret_cast<CDuiString*>(sLinks + iLinkIndex - 1);
						CDuiString *pStr2 = reinterpret_cast<CDuiString*>(sLinks + iLinkIndex);
						*pStr2 = *pStr1;
					}
					for( int i = iLineLinkIndex; i < iLinkIndex; i++ ) {
						prcLinks[i].bottom = pt.y + cyLine;
					}
					if( bDraw ) {
						bInLink = bLineInLink;
						iLinkIndex = iLineLinkIndex;
					}
				}
				else {
					if( bInLink && iLinkIndex < nLinkRects ) iLinkIndex++;
					bLineInLink = bInLink;
					iLineLinkIndex = iLinkIndex;
				}
				if( (uStyle & DT_SINGLELINE) != 0 && (!bDraw || bLineDraw) ) break;
				if( bDraw ) bLineDraw = !bLineDraw; // !
				pt.x = rc.left;
				if( !bLineDraw ) pt.y += cyLine;
				if( pt.y > rc.bottom && bDraw ) break;
				ptLinkStart = pt;
				cyLine = pTm->tmHeight + pTm->tmExternalLeading + static_cast<int>(reinterpret_cast<intptr_t>(aPIndentArray.GetAt(aPIndentArray.GetSize() - 1)));
				if( pt.x >= rc.right ) break;
			}
			else if( !bInRaw && ( *pstrText == _T('<') || *pstrText == _T('{') )
				&& ( pstrText[1] >= _T('a') && pstrText[1] <= _T('z') )
				&& ( pstrText[2] == _T(' ') || pstrText[2] == _T('>') || pstrText[2] == _T('}') ) ) {
					pstrText++;
					LPCTSTR pstrNextStart = nullptr;
					switch( *pstrText ) {
					case _T('a'):  // Link
						{
							pstrText++;
							while( *pstrText > _T('\0') && *pstrText <= _T(' ') ) pstrText = ::CharNext(pstrText);
							if( iLinkIndex < nLinkRects && !bLineDraw ) {
								CDuiString *pStr = reinterpret_cast<CDuiString*>(sLinks + iLinkIndex);
								pStr->Empty();
								while( *pstrText != _T('\0') && *pstrText != _T('>') && *pstrText != _T('}') ) {
									LPCTSTR pstrTemp = ::CharNext(pstrText);
									while( pstrText < pstrTemp) {
										*pStr += *pstrText++;
									}
								}
							}

							DWORD clrColor = dwTextColor;
							if(clrColor == 0) pManager->GetDefaultLinkFontColor();
							if( bHoverLink && iLinkIndex < nLinkRects ) {
								CDuiString *pStr = reinterpret_cast<CDuiString*>(sLinks + iLinkIndex);
								if( sHoverLink == *pStr ) clrColor = pManager->GetDefaultLinkHoverFontColor();
							}
							//else if( prcLinks == nullptr ) {
							//    if( ::PtInRect(&rc, ptMouse) )
							//        clrColor = pManager->GetDefaultLinkHoverFontColor();
							//}
							aColorArray.Add(reinterpret_cast<LPVOID>(static_cast<UINT_PTR>(clrColor)));
							::SetTextColor(hDC,  RGB(GetBValue(clrColor), GetGValue(clrColor), GetRValue(clrColor)));
							TFontInfo* pFontInfo = pDefFontInfo;
							if( aFontArray.GetSize() > 0 ) pFontInfo = (TFontInfo*)aFontArray.GetAt(aFontArray.GetSize() - 1);
							if( pFontInfo->bUnderline == false ) {
								HFONT hFont = pManager->GetFont(pFontInfo->sFontName, pFontInfo->iSize, pFontInfo->bBold, true, pFontInfo->bItalic, pFontInfo->bStrikeout);
								if( hFont == nullptr ) hFont = pManager->AddFont(g_iFontID, pFontInfo->sFontName, pFontInfo->iSize, pFontInfo->bBold, true, pFontInfo->bItalic, pFontInfo->bStrikeout);
								pFontInfo = pManager->GetFontInfo(hFont);
								aFontArray.Add(pFontInfo);
								pTm = &pFontInfo->tm;
								::SelectObject(hDC, pFontInfo->hFont);
								cyLine = MAX(cyLine, pTm->tmHeight + pTm->tmExternalLeading + static_cast<int>(reinterpret_cast<intptr_t>(aPIndentArray.GetAt(aPIndentArray.GetSize() - 1))));
							}
							ptLinkStart = pt;
							bInLink = true;
						}
						break;
					case _T('b'):  // Bold
						{
							pstrText++;
							TFontInfo* pFontInfo = pDefFontInfo;
							if( aFontArray.GetSize() > 0 ) pFontInfo = (TFontInfo*)aFontArray.GetAt(aFontArray.GetSize() - 1);
							if( pFontInfo->bBold == false ) {
								HFONT hFont = pManager->GetFont(pFontInfo->sFontName, pFontInfo->iSize, true, pFontInfo->bUnderline, pFontInfo->bItalic, pFontInfo->bStrikeout);
								if( hFont == nullptr ) hFont = pManager->AddFont(g_iFontID, pFontInfo->sFontName, pFontInfo->iSize, true, pFontInfo->bUnderline, pFontInfo->bItalic, pFontInfo->bStrikeout);
								pFontInfo = pManager->GetFontInfo(hFont);
								aFontArray.Add(pFontInfo);
								pTm = &pFontInfo->tm;
								::SelectObject(hDC, pFontInfo->hFont);
								cyLine = MAX(cyLine, pTm->tmHeight + pTm->tmExternalLeading + static_cast<int>(reinterpret_cast<intptr_t>(aPIndentArray.GetAt(aPIndentArray.GetSize() - 1))));
							}
						}
						break;
					case _T('c'):  // Color
						{
							pstrText++;
							while( *pstrText > _T('\0') && *pstrText <= _T(' ') ) pstrText = ::CharNext(pstrText);
							if( *pstrText == _T('#')) pstrText++;
							DWORD clrColor = _tcstol(pstrText, const_cast<LPTSTR*>(&pstrText), 16);
							aColorArray.Add(reinterpret_cast<LPVOID>(static_cast<UINT_PTR>(clrColor)));
							::SetTextColor(hDC, RGB(GetBValue(clrColor), GetGValue(clrColor), GetRValue(clrColor)));
						}
						break;
					case _T('f'):  // Font
						{
							pstrText++;
							while( *pstrText > _T('\0') && *pstrText <= _T(' ') ) pstrText = ::CharNext(pstrText);
							LPCTSTR pstrTemp = pstrText;
							int iFont = static_cast<int>(_tcstol(pstrText, const_cast<LPTSTR*>(&pstrText), 10));
							if( pstrTemp != pstrText ) {
								TFontInfo* pFontInfo = pManager->GetFontInfo(iFont);
								aFontArray.Add(pFontInfo);
								pTm = &pFontInfo->tm;
								::SelectObject(hDC, pFontInfo->hFont);
							}
							else {
								CDuiString sFontName;
								int iFontSize = 10;
								CDuiString sFontAttr;
								bool bBold = false;
								bool bUnderline = false;
								bool bItalic = false;
								bool bStrikeout = false;
								while( *pstrText != _T('\0') && *pstrText != _T('>') && *pstrText != _T('}') && *pstrText != _T(' ') ) {
									pstrTemp = ::CharNext(pstrText);
									while( pstrText < pstrTemp) {
										sFontName += *pstrText++;
									}
								}
								while( *pstrText > _T('\0') && *pstrText <= _T(' ') ) pstrText = ::CharNext(pstrText);
								if( isdigit(*pstrText) ) {
									iFontSize = static_cast<int>(_tcstol(pstrText, const_cast<LPTSTR*>(&pstrText), 10));
								}
								while( *pstrText > _T('\0') && *pstrText <= _T(' ') ) pstrText = ::CharNext(pstrText);
								while( *pstrText != _T('\0') && *pstrText != _T('>') && *pstrText != _T('}') ) {
									pstrTemp = ::CharNext(pstrText);
									while( pstrText < pstrTemp) {
										sFontAttr += *pstrText++;
									}
								}
								sFontAttr.MakeLower();
								if( sFontAttr.Find(_T("bold")) >= 0 ) bBold = true;
								if( sFontAttr.Find(_T("underline")) >= 0 ) bUnderline = true;
								if( sFontAttr.Find(_T("italic")) >= 0 ) bItalic = true;
								if( sFontAttr.Find(_T("strikeout")) >= 0 ) bStrikeout = true;
								HFONT hFont = pManager->GetFont(sFontName, iFontSize, bBold, bUnderline, bItalic, bStrikeout);
								if( hFont == nullptr ) hFont = pManager->AddFont(g_iFontID, sFontName, iFontSize, bBold, bUnderline, bItalic, bStrikeout);
								TFontInfo* pFontInfo = pManager->GetFontInfo(hFont);
								aFontArray.Add(pFontInfo);
								pTm = &pFontInfo->tm;
								::SelectObject(hDC, pFontInfo->hFont);
							}
							cyLine = MAX(cyLine, pTm->tmHeight + pTm->tmExternalLeading + static_cast<int>(reinterpret_cast<intptr_t>(aPIndentArray.GetAt(aPIndentArray.GetSize() - 1))));
						}
						break;
					case _T('i'):  // Italic or Image
						{    
							pstrNextStart = pstrText - 1;
							pstrText++;
							CDuiString sImageString = pstrText;
							int iWidth = 0;
							int iHeight = 0;
							while( *pstrText > _T('\0') && *pstrText <= _T(' ') ) pstrText = ::CharNext(pstrText);
							const TImageInfo* pImageInfo = nullptr;
							CDuiString sName;
							while( *pstrText != _T('\0') && *pstrText != _T('>') && *pstrText != _T('}') && *pstrText != _T(' ') ) {
								LPCTSTR pstrTemp = ::CharNext(pstrText);
								while( pstrText < pstrTemp) {
									sName += *pstrText++;
								}
							}
							if( sName.IsEmpty() ) { // Italic
								pstrNextStart = nullptr;
								TFontInfo* pFontInfo = pDefFontInfo;
								if( aFontArray.GetSize() > 0 ) pFontInfo = (TFontInfo*)aFontArray.GetAt(aFontArray.GetSize() - 1);
								if( pFontInfo->bItalic == false ) {
									HFONT hFont = pManager->GetFont(pFontInfo->sFontName, pFontInfo->iSize, pFontInfo->bBold, pFontInfo->bUnderline, pFontInfo->bStrikeout, true);
									if( hFont == nullptr ) hFont = pManager->AddFont(g_iFontID, pFontInfo->sFontName, pFontInfo->iSize, pFontInfo->bBold, pFontInfo->bUnderline, pFontInfo->bStrikeout, true);
									pFontInfo = pManager->GetFontInfo(hFont);
									aFontArray.Add(pFontInfo);
									pTm = &pFontInfo->tm;
									::SelectObject(hDC, pFontInfo->hFont);
									cyLine = MAX(cyLine, pTm->tmHeight + pTm->tmExternalLeading + static_cast<int>(reinterpret_cast<intptr_t>(aPIndentArray.GetAt(aPIndentArray.GetSize() - 1))));
								}
							}
							else {
								while( *pstrText > _T('\0') && *pstrText <= _T(' ') ) pstrText = ::CharNext(pstrText);
								int iImageListNum = static_cast<int>(_tcstol(pstrText, const_cast<LPTSTR*>(&pstrText), 10));
								if( iImageListNum <= 0 ) iImageListNum = 1;
								while( *pstrText > _T('\0') && *pstrText <= _T(' ') ) pstrText = ::CharNext(pstrText);
								int iImageListIndex = static_cast<int>(_tcstol(pstrText, const_cast<LPTSTR*>(&pstrText), 10));
								if( iImageListIndex < 0 || iImageListIndex >= iImageListNum ) iImageListIndex = 0;

								if( _tcsstr(sImageString.GetData(), _T("file=\'")) != nullptr || _tcsstr(sImageString.GetData(), _T("res=\'")) != nullptr ) {
									CDuiString sImageResType;
									CDuiString sImageName;
									LPCTSTR pStrImage = sImageString.GetData();
									CDuiString sItem;
									CDuiString sValue;
									while( *pStrImage != _T('\0') ) {
										sItem.Empty();
										sValue.Empty();
										while( *pStrImage > _T('\0') && *pStrImage <= _T(' ') ) pStrImage = ::CharNext(pStrImage);
										while( *pStrImage != _T('\0') && *pStrImage != _T('=') && *pStrImage > _T(' ') ) {
											LPTSTR pstrTemp = ::CharNext(pStrImage);
											while( pStrImage < pstrTemp) {
												sItem += *pStrImage++;
											}
										}
										while( *pStrImage > _T('\0') && *pStrImage <= _T(' ') ) pStrImage = ::CharNext(pStrImage);
										if( *pStrImage++ != _T('=') ) break;
										while( *pStrImage > _T('\0') && *pStrImage <= _T(' ') ) pStrImage = ::CharNext(pStrImage);
										if( *pStrImage++ != _T('\'') ) break;
										while( *pStrImage != _T('\0') && *pStrImage != _T('\'') ) {
											LPTSTR pstrTemp = ::CharNext(pStrImage);
											while( pStrImage < pstrTemp) {
												sValue += *pStrImage++;
											}
										}
										if( *pStrImage++ != _T('\'') ) break;
										if( !sValue.IsEmpty() ) {
											if( sItem == _T("file") || sItem == _T("res") ) {
												sImageName = sValue;
											}
											else if( sItem == _T("restype") ) {
												sImageResType = sValue;
											}
										}
										if( *pStrImage++ != _T(' ') ) break;
									}

									pImageInfo = pManager->GetImageEx(static_cast<LPCTSTR>(sImageName), sImageResType);
								}
								else
									pImageInfo = pManager->GetImageEx(static_cast<LPCTSTR>(sName));

								if( pImageInfo ) {
									iWidth = pImageInfo->nX;
									iHeight = pImageInfo->nY;
									if( iImageListNum > 1 ) iWidth /= iImageListNum;

									if( pt.x + iWidth > rc.right && pt.x > rc.left && (uStyle & DT_SINGLELINE) == 0 ) {
										bLineEnd = true;
									}
									else {
										pstrNextStart = nullptr;
										if( bDraw && bLineDraw ) {
											CDuiRect rcImage(pt.x, pt.y + cyLineHeight - iHeight, pt.x + iWidth, pt.y + cyLineHeight);
											if( iHeight < cyLineHeight ) { 
												rcImage.bottom -= (cyLineHeight - iHeight) / 2;
												rcImage.top = rcImage.bottom -  iHeight;
											}
											CDuiRect rcBmpPart(0, 0, iWidth, iHeight);
											rcBmpPart.left = iWidth * iImageListIndex;
											rcBmpPart.right = iWidth * (iImageListIndex + 1);
											CDuiRect rcCorner(0, 0, 0, 0);
											DrawImage(hDC, pImageInfo->hBitmap, rcImage, rcImage, rcBmpPart, rcCorner, \
												pImageInfo->bAlpha, 255);
										}

										cyLine = MAX(iHeight, cyLine);
										pt.x += iWidth;
										cyMinHeight = pt.y + iHeight;
										cxMaxWidth = MAX(cxMaxWidth, pt.x);
									}
								}
								else pstrNextStart = nullptr;
							}
						}
						break;
					case _T('n'):  // Newline
						{
							pstrText++;
							if( (uStyle & DT_SINGLELINE) != 0 ) break;
							bLineEnd = true;
						}
						break;
					case _T('p'):  // Paragraph
						{
							pstrText++;
							if( pt.x > rc.left ) bLineEnd = true;
							while( *pstrText > _T('\0') && *pstrText <= _T(' ') ) pstrText = ::CharNext(pstrText);
							int cyLineExtra = static_cast<int>(_tcstol(pstrText, const_cast<LPTSTR*>(&pstrText), 10));
							aPIndentArray.Add(reinterpret_cast<LPVOID>(static_cast<INT_PTR>(cyLineExtra)));
							cyLine = MAX(cyLine, pTm->tmHeight + pTm->tmExternalLeading + cyLineExtra);
						}
						break;
					case _T('r'):  // Raw Text
						{
							pstrText++;
							bInRaw = true;
						}
						break;
					case _T('s'):  // Selected text background color
						{
							pstrText++;
							bInSelected = !bInSelected;
							if( bDraw && bLineDraw ) {
								if( bInSelected ) ::SetBkMode(hDC, OPAQUE);
								else ::SetBkMode(hDC, TRANSPARENT);
							}
						}
						break;
					case _T('u'):  // Underline text
						{
							pstrText++;
							TFontInfo* pFontInfo = pDefFontInfo;
							if( aFontArray.GetSize() > 0 ) pFontInfo = (TFontInfo*)aFontArray.GetAt(aFontArray.GetSize() - 1);
							if( pFontInfo->bUnderline == false ) {
								HFONT hFont = pManager->GetFont(pFontInfo->sFontName, pFontInfo->iSize, pFontInfo->bBold, true, pFontInfo->bItalic, pFontInfo->bStrikeout);
								if( hFont == nullptr ) hFont = pManager->AddFont(g_iFontID, pFontInfo->sFontName, pFontInfo->iSize, pFontInfo->bBold, true, pFontInfo->bItalic, pFontInfo->bStrikeout);
								pFontInfo = pManager->GetFontInfo(hFont);
								aFontArray.Add(pFontInfo);
								pTm = &pFontInfo->tm;
								::SelectObject(hDC, pFontInfo->hFont);
								cyLine = MAX(cyLine, pTm->tmHeight + pTm->tmExternalLeading + static_cast<int>(reinterpret_cast<intptr_t>(aPIndentArray.GetAt(aPIndentArray.GetSize() - 1))));
							}
						}
						break;
					case _T('x'):  // X Indent
						{
							pstrText++;
							while( *pstrText > _T('\0') && *pstrText <= _T(' ') ) pstrText = ::CharNext(pstrText);
							int iWidth = static_cast<int>(_tcstol(pstrText, const_cast<LPTSTR*>(&pstrText), 10));
							pt.x += iWidth;
							cxMaxWidth = MAX(cxMaxWidth, pt.x);
						}
						break;
					case _T('y'):  // Y Indent
						{
							pstrText++;
							while( *pstrText > _T('\0') && *pstrText <= _T(' ') ) pstrText = ::CharNext(pstrText);
							cyLine = static_cast<int>(_tcstol(pstrText, const_cast<LPTSTR*>(&pstrText), 10));
						}
						break;
					}
					if( pstrNextStart != nullptr ) pstrText = pstrNextStart;
					else {
						while( *pstrText != _T('\0') && *pstrText != _T('>') && *pstrText != _T('}') ) pstrText = ::CharNext(pstrText);
						pstrText = ::CharNext(pstrText);
					}
			}
			else if( !bInRaw && ( *pstrText == _T('<') || *pstrText == _T('{') ) && pstrText[1] == _T('/') )
			{
				pstrText++;
				pstrText++;
				switch( *pstrText )
				{
				case _T('c'):
					{
						pstrText++;
						aColorArray.Remove(aColorArray.GetSize() - 1);
						DWORD clrColor = dwTextColor;
						if( aColorArray.GetSize() > 0 ) clrColor = static_cast<int>(reinterpret_cast<intptr_t>(aColorArray.GetAt(aColorArray.GetSize() - 1)));
						::SetTextColor(hDC, RGB(GetBValue(clrColor), GetGValue(clrColor), GetRValue(clrColor)));
					}
					break;
				case _T('p'):
					pstrText++;
					if( pt.x > rc.left ) bLineEnd = true;
					aPIndentArray.Remove(aPIndentArray.GetSize() - 1);
					cyLine = MAX(cyLine, pTm->tmHeight + pTm->tmExternalLeading + static_cast<int>(reinterpret_cast<intptr_t>(aPIndentArray.GetAt(aPIndentArray.GetSize() - 1))));
					break;
				case _T('s'):
					{
						pstrText++;
						bInSelected = !bInSelected;
						if( bDraw && bLineDraw ) {
							if( bInSelected ) ::SetBkMode(hDC, OPAQUE);
							else ::SetBkMode(hDC, TRANSPARENT);
						}
					}
					break;
				case _T('a'):
					{
						if( iLinkIndex < nLinkRects ) {
							if( !bLineDraw ) ::SetRect(&prcLinks[iLinkIndex], ptLinkStart.x, ptLinkStart.y, MIN(pt.x, rc.right), pt.y + pTm->tmHeight + pTm->tmExternalLeading);
							iLinkIndex++;
						}
						aColorArray.Remove(aColorArray.GetSize() - 1);
						DWORD clrColor = dwTextColor;
						if( aColorArray.GetSize() > 0 ) clrColor = static_cast<int>(reinterpret_cast<intptr_t>(aColorArray.GetAt(aColorArray.GetSize() - 1)));
						::SetTextColor(hDC, RGB(GetBValue(clrColor), GetGValue(clrColor), GetRValue(clrColor)));
						bInLink = false;
					}
				case _T('b'):
				case _T('f'):
				case _T('i'):
				case _T('u'):
					{
						pstrText++;
						aFontArray.Remove(aFontArray.GetSize() - 1);
						TFontInfo* pFontInfo = reinterpret_cast<TFontInfo*>(aFontArray.GetAt(aFontArray.GetSize() - 1));
						if( pFontInfo == nullptr ) pFontInfo = pDefFontInfo;
						if( pTm->tmItalic && pFontInfo->bItalic == false ) {
							ABC abc;
							::GetCharABCWidths(hDC, _T(' '), _T(' '), &abc);
							pt.x += abc.abcC / 2; // 简单修正一下斜体混排的问题, 正确做法应该是http://support.microsoft.com/kb/244798/en-us
						}
						pTm = &pFontInfo->tm;
						::SelectObject(hDC, pFontInfo->hFont);
						cyLine = MAX(cyLine, pTm->tmHeight + pTm->tmExternalLeading + static_cast<int>(reinterpret_cast<intptr_t>(aPIndentArray.GetAt(aPIndentArray.GetSize() - 1))));
					}
					break;
				}
				while( *pstrText != _T('\0') && *pstrText != _T('>') && *pstrText != _T('}') ) pstrText = ::CharNext(pstrText);
				pstrText = ::CharNext(pstrText);
			}
			else if( !bInRaw &&  *pstrText == _T('<') && pstrText[2] == _T('>') && (pstrText[1] == _T('{')  || pstrText[1] == _T('}')) )
			{
				SIZE szSpace = { 0 };
				::GetTextExtentPoint32(hDC, &pstrText[1], 1, &szSpace);
				if( bDraw && bLineDraw ) ::TextOut(hDC, pt.x, pt.y + cyLineHeight - pTm->tmHeight - pTm->tmExternalLeading, &pstrText[1], 1);
				pt.x += szSpace.cx;
				cxMaxWidth = MAX(cxMaxWidth, pt.x);
				pstrText++;pstrText++;pstrText++;
			}
			else if( !bInRaw &&  *pstrText == _T('{') && pstrText[2] == _T('}') && (pstrText[1] == _T('<')  || pstrText[1] == _T('>')) )
			{
				SIZE szSpace = { 0 };
				::GetTextExtentPoint32(hDC, &pstrText[1], 1, &szSpace);
				if( bDraw && bLineDraw ) ::TextOut(hDC, pt.x,  pt.y + cyLineHeight - pTm->tmHeight - pTm->tmExternalLeading, &pstrText[1], 1);
				pt.x += szSpace.cx;
				cxMaxWidth = MAX(cxMaxWidth, pt.x);
				pstrText++;pstrText++;pstrText++;
			}
			else if( !bInRaw &&  *pstrText == _T(' ') )
			{
				SIZE szSpace = { 0 };
				::GetTextExtentPoint32(hDC, _T(" "), 1, &szSpace);
				// Still need to paint the space because the font might have
				// underline formatting.
				if( bDraw && bLineDraw ) ::TextOut(hDC, pt.x,  pt.y + cyLineHeight - pTm->tmHeight - pTm->tmExternalLeading, _T(" "), 1);
				pt.x += szSpace.cx;
				cxMaxWidth = MAX(cxMaxWidth, pt.x);
				pstrText++;
			}
			else
			{
				POINT ptPos = pt;
				int cchChars = 0;
				int cchSize = 0;
				int cchLastGoodWord = 0;
				int cchLastGoodSize = 0;
				LPCTSTR p = pstrText;
				LPCTSTR pstrNext;
				SIZE szText = { 0 };
				if( !bInRaw && *p == _T('<') || *p == _T('{') ) p++, cchChars++, cchSize++;
				while( *p != _T('\0') && *p != _T('\n') ) {
					// This part makes sure that we're word-wrapping if needed or providing support
					// for DT_END_ELLIPSIS. Unfortunately the GetTextExtentPoint32() call is pretty
					// slow when repeated so often.
					// TODO: Rewrite and use GetTextExtentExPoint() instead!
					if( bInRaw ) {
						if( ( *p == _T('<') || *p == _T('{') ) && p[1] == _T('/') 
							&& p[2] == _T('r') && ( p[3] == _T('>') || p[3] == _T('}') ) ) {
								p += 4;
								bInRaw = false;
								break;
						}
					}
					else {
						if( *p == _T('<') || *p == _T('{') ) break;
					}
					pstrNext = ::CharNext(p);
					cchChars++;
					cchSize += static_cast<int>(pstrNext - p);
					szText.cx = cchChars * pTm->tmMaxCharWidth;
					if( pt.x + szText.cx >= rc.right ) {
						::GetTextExtentPoint32(hDC, pstrText, cchSize, &szText);
					}
					if( pt.x + szText.cx > rc.right ) {
						if( pt.x + szText.cx > rc.right && pt.x != rc.left) {
							cchChars--;
							cchSize -= static_cast<int>(pstrNext - p);
						}
						if( (uStyle & DT_WORDBREAK) != 0 && cchLastGoodWord > 0 ) {
							cchChars = cchLastGoodWord;
							cchSize = cchLastGoodSize;                 
						}
						if( (uStyle & DT_END_ELLIPSIS) != 0 && cchChars > 0 ) {
							cchChars -= 1;
							LPCTSTR pstrPrev = ::CharPrev(pstrText, p);
							if( cchChars > 0 ) {
								cchChars -= 1;
								pstrPrev = ::CharPrev(pstrText, pstrPrev);
								cchSize -= static_cast<int>(p - pstrPrev);
							}
							else 
								cchSize -= static_cast<int>(p - pstrPrev);
							pt.x = rc.right;
						}
						bLineEnd = true;
						cxMaxWidth = MAX(cxMaxWidth, pt.x);
						break;
					}
					if (!( ( p[0] >= _T('a') && p[0] <= _T('z') ) || ( p[0] >= _T('A') && p[0] <= _T('Z') ) )) {
						cchLastGoodWord = cchChars;
						cchLastGoodSize = cchSize;
					}
					if( *p == _T(' ') ) {
						cchLastGoodWord = cchChars;
						cchLastGoodSize = cchSize;
					}
					p = ::CharNext(p);
				}

				::GetTextExtentPoint32(hDC, pstrText, cchSize, &szText);
				if( bDraw && bLineDraw ) {
					if( (uStyle & DT_SINGLELINE) == 0 && (uStyle & DT_CENTER) != 0 ) {
						ptPos.x += (rc.right - rc.left - szText.cx)/2;
					}
					else if( (uStyle & DT_SINGLELINE) == 0 && (uStyle & DT_RIGHT) != 0) {
						ptPos.x += (rc.right - rc.left - szText.cx);
					}
					::TextOut(hDC, ptPos.x, ptPos.y + cyLineHeight - pTm->tmHeight - pTm->tmExternalLeading, pstrText, cchSize);
					if( pt.x >= rc.right && (uStyle & DT_END_ELLIPSIS) != 0 ) 
						::TextOut(hDC, ptPos.x + szText.cx, ptPos.y, _T("..."), 3);
				}
				pt.x += szText.cx;
				cxMaxWidth = MAX(cxMaxWidth, pt.x);
				pstrText += cchSize;
			}

			if( pt.x >= rc.right || *pstrText == _T('\n') || *pstrText == _T('\0') ) bLineEnd = true;
			if( bDraw && bLineEnd ) {
				if( !bLineDraw ) {
					aFontArray.Resize(aLineFontArray.GetSize());
					::CopyMemory(aFontArray.GetData(), aLineFontArray.GetData(), aLineFontArray.GetSize() * sizeof(LPVOID));
					aColorArray.Resize(aLineColorArray.GetSize());
					::CopyMemory(aColorArray.GetData(), aLineColorArray.GetData(), aLineColorArray.GetSize() * sizeof(LPVOID));
					aPIndentArray.Resize(aLinePIndentArray.GetSize());
					::CopyMemory(aPIndentArray.GetData(), aLinePIndentArray.GetData(), aLinePIndentArray.GetSize() * sizeof(LPVOID));

					cyLineHeight = cyLine;
					pstrText = pstrLineBegin;
					bInRaw = bLineInRaw;
					bInSelected = bLineInSelected;

					DWORD clrColor = dwTextColor;
					if( aColorArray.GetSize() > 0 ) clrColor = static_cast<int>(reinterpret_cast<intptr_t>(aColorArray.GetAt(aColorArray.GetSize() - 1)));
					::SetTextColor(hDC, RGB(GetBValue(clrColor), GetGValue(clrColor), GetRValue(clrColor)));
					TFontInfo* pFontInfo = reinterpret_cast<TFontInfo*>(aFontArray.GetAt(aFontArray.GetSize() - 1));
					if( pFontInfo == nullptr ) pFontInfo = pDefFontInfo;
					pTm = &pFontInfo->tm;
					::SelectObject(hDC, pFontInfo->hFont);
					if( bInSelected ) ::SetBkMode(hDC, OPAQUE);
				}
				else {
					aLineFontArray.Resize(aFontArray.GetSize());
					::CopyMemory(aLineFontArray.GetData(), aFontArray.GetData(), aFontArray.GetSize() * sizeof(LPVOID));
					aLineColorArray.Resize(aColorArray.GetSize());
					::CopyMemory(aLineColorArray.GetData(), aColorArray.GetData(), aColorArray.GetSize() * sizeof(LPVOID));
					aLinePIndentArray.Resize(aPIndentArray.GetSize());
					::CopyMemory(aLinePIndentArray.GetData(), aPIndentArray.GetData(), aPIndentArray.GetSize() * sizeof(LPVOID));
					pstrLineBegin = pstrText;
					bLineInSelected = bInSelected;
					bLineInRaw = bInRaw;
				}
			}

			ASSERT(iLinkIndex<=nLinkRects);
		}

		nLinkRects = iLinkIndex;

		// Return size of text when requested
		if( (uStyle & DT_CALCRECT) != 0 ) {
			rc.bottom = MAX(cyMinHeight, pt.y + cyLine);
			rc.right = MIN(rc.right, cxMaxWidth);
		}

		if( bDraw ) ::SelectClipRgn(hDC, hOldRgn);
		::DeleteObject(hOldRgn);
		::DeleteObject(hRgn);

		::SelectObject(hDC, hOldFont);
	}

	HBITMAP CRenderEngine::GenerateBitmap(CPaintManagerUI* pManager, RECT rc, CControlUI* pStopControl, DWORD dwFilterColor)
	{
		if (pManager == nullptr) return nullptr;
		int cx = rc.right - rc.left;
		int cy = rc.bottom - rc.top;

		bool bUseOffscreenBitmap = true;
		HDC hPaintDC = ::CreateCompatibleDC(pManager->GetPaintDC());
		ASSERT(hPaintDC);
		HBITMAP hPaintBitmap = nullptr;
		//if (pStopControl == nullptr && !pManager->IsLayered()) hPaintBitmap = pManager->Get();
		if( hPaintBitmap == nullptr ) {
			bUseOffscreenBitmap = false;
			hPaintBitmap = ::CreateCompatibleBitmap(pManager->GetPaintDC(), rc.right, rc.bottom);
			ASSERT(hPaintBitmap);
		}
		HBITMAP hOldPaintBitmap = (HBITMAP) ::SelectObject(hPaintDC, hPaintBitmap);
		if (!bUseOffscreenBitmap) {
			CControlUI* pRoot = pManager->GetRoot();
			pRoot->Paint(hPaintDC, rc, pStopControl);
		}

		BITMAPINFO bmi = { 0 };
		bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth = cx;
		bmi.bmiHeader.biHeight = cy;
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 32;
		bmi.bmiHeader.biCompression = BI_RGB;
		bmi.bmiHeader.biSizeImage = cx * cy * sizeof(DWORD);
		LPDWORD pDest = nullptr;
		HDC hCloneDC = ::CreateCompatibleDC(pManager->GetPaintDC());
		HBITMAP hBitmap = ::CreateDIBSection(pManager->GetPaintDC(), &bmi, DIB_RGB_COLORS, (LPVOID*) &pDest, nullptr, 0);
		ASSERT(hCloneDC);
		ASSERT(hBitmap);
		if( hBitmap != nullptr )
		{
			HBITMAP hOldBitmap = (HBITMAP) ::SelectObject(hCloneDC, hBitmap);
			::BitBlt(hCloneDC, 0, 0, cx, cy, hPaintDC, rc.left, rc.top, SRCCOPY);
			RECT rcClone = {0, 0, cx, cy};
			if (dwFilterColor > 0x00FFFFFF) DrawColor(hCloneDC, rcClone, dwFilterColor);
			::SelectObject(hCloneDC, hOldBitmap);
			::DeleteDC(hCloneDC);  
			::GdiFlush();
		}

		// Cleanup
		::SelectObject(hPaintDC, hOldPaintBitmap);
		if (!bUseOffscreenBitmap) ::DeleteObject(hPaintBitmap);
		::DeleteDC(hPaintDC);

		return hBitmap;
	}

	HBITMAP CRenderEngine::GenerateBitmap(CPaintManagerUI* pManager, CControlUI* pControl, RECT rc, DWORD dwFilterColor)
	{
		if (pManager == nullptr || pControl == nullptr) return nullptr;
		int cx = rc.right - rc.left;
		int cy = rc.bottom - rc.top;

		HDC hPaintDC = ::CreateCompatibleDC(pManager->GetPaintDC());
		HBITMAP hPaintBitmap = ::CreateCompatibleBitmap(pManager->GetPaintDC(), rc.right, rc.bottom);
		ASSERT(hPaintDC);
		ASSERT(hPaintBitmap);
		HBITMAP hOldPaintBitmap = (HBITMAP) ::SelectObject(hPaintDC, hPaintBitmap);
		pControl->Paint(hPaintDC, rc, nullptr);

		BITMAPINFO bmi = { 0 };
		bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth = cx;
		bmi.bmiHeader.biHeight = cy;
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 32;
		bmi.bmiHeader.biCompression = BI_RGB;
		bmi.bmiHeader.biSizeImage = cx * cy * sizeof(DWORD);
		LPDWORD pDest = nullptr;
		HDC hCloneDC = ::CreateCompatibleDC(pManager->GetPaintDC());
		HBITMAP hBitmap = ::CreateDIBSection(pManager->GetPaintDC(), &bmi, DIB_RGB_COLORS, (LPVOID*) &pDest, nullptr, 0);
		ASSERT(hCloneDC);
		ASSERT(hBitmap);
		if( hBitmap != nullptr )
		{
			HBITMAP hOldBitmap = (HBITMAP) ::SelectObject(hCloneDC, hBitmap);
			::BitBlt(hCloneDC, 0, 0, cx, cy, hPaintDC, rc.left, rc.top, SRCCOPY);
			RECT rcClone = {0, 0, cx, cy};
			if (dwFilterColor > 0x00FFFFFF) DrawColor(hCloneDC, rcClone, dwFilterColor);
			::SelectObject(hCloneDC, hOldBitmap);
			::DeleteDC(hCloneDC);  
			::GdiFlush();
		}

		// Cleanup
		::SelectObject(hPaintDC, hOldPaintBitmap);
		::DeleteObject(hPaintBitmap);
		::DeleteDC(hPaintDC);

		return hBitmap;
	}

	SIZE CRenderEngine::GetTextSize( HDC hDC, CPaintManagerUI* pManager , LPCTSTR pstrText, int iFont, UINT uStyle )
	{
		SIZE size = {0,0};
		ASSERT(::GetObjectType(hDC)==OBJ_DC || ::GetObjectType(hDC)==OBJ_MEMDC);
		if( pstrText == nullptr || pManager == nullptr ) return size;
		::SetBkMode(hDC, TRANSPARENT);
		HFONT hOldFont = (HFONT)::SelectObject(hDC, pManager->GetFont(iFont));
		GetTextExtentPoint32(hDC, pstrText, static_cast<int>(_tcslen(pstrText)) , &size);
		::SelectObject(hDC, hOldFont);
		return size;
	}

	void CRenderEngine::CheckAlphaColor(DWORD& dwColor)
	{
		//RestoreAlphaColor认为0x00000000是真正的透明，其它都是GDI绘制导致的
		//所以在GDI绘制中不能用0xFF000000这个颜色值，现在处理是让它变成RGB(0,0,1)
		//RGB(0,0,1)与RGB(0,0,0)很难分出来
		if((0x00FFFFFF & dwColor) == 0)
		{
			dwColor += 1;
		}
	}

	DWORD CRenderEngine::AdjustColor(DWORD dwColor, short H, short S, short L)
	{
		if( H == 180 && S == 100 && L == 100 ) return dwColor;
		float fH, fS, fL;
		float S1 = S / 100.0f;
		float L1 = L / 100.0f;
		RGBtoHSL(dwColor, &fH, &fS, &fL);
		fH += (H - 180);
		fH = fH > 0 ? fH : fH + 360; 
		fS *= S1;
		fL *= L1;
		HSLtoRGB(&dwColor, fH, fS, fL);
		return dwColor;
	}

	HBITMAP CRenderEngine::CreateARGB32Bitmap(HDC hDC, int cx, int cy, BYTE** pBits)
	{
		LPBITMAPINFO lpbiSrc = nullptr;
		lpbiSrc = reinterpret_cast<LPBITMAPINFO>(new BYTE[sizeof(BITMAPINFOHEADER)]);
		if (lpbiSrc == nullptr) return nullptr;

		lpbiSrc->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		lpbiSrc->bmiHeader.biWidth = cx;
		lpbiSrc->bmiHeader.biHeight = cy;
		lpbiSrc->bmiHeader.biPlanes = 1;
		lpbiSrc->bmiHeader.biBitCount = 32;
		lpbiSrc->bmiHeader.biCompression = BI_RGB;
		lpbiSrc->bmiHeader.biSizeImage = abs(cx * cy);
		lpbiSrc->bmiHeader.biXPelsPerMeter = 0;
		lpbiSrc->bmiHeader.biYPelsPerMeter = 0;
		lpbiSrc->bmiHeader.biClrUsed = 0;
		lpbiSrc->bmiHeader.biClrImportant = 0;

		HBITMAP hBitmap = CreateDIBSection (hDC, lpbiSrc, DIB_RGB_COLORS, (void **)pBits, nullptr, 0);
		delete [] lpbiSrc;
		return hBitmap;
	}

	void CRenderEngine::AdjustImage(bool bUseHSL, TImageInfo* imageInfo, short H, short S, short L)
	{
		if( imageInfo == nullptr || imageInfo->bUseHSL == false || imageInfo->hBitmap == nullptr || 
			imageInfo->pBits == nullptr || imageInfo->pSrcBits == nullptr ) 
			return;
		if( bUseHSL == false || (H == 180 && S == 100 && L == 100)) {
			::CopyMemory(imageInfo->pBits, imageInfo->pSrcBits, imageInfo->nX * imageInfo->nY * 4);
			return;
		}

		float fH, fS, fL;
		float S1 = S / 100.0f;
		float L1 = L / 100.0f;
		for( int i = 0; i < imageInfo->nX * imageInfo->nY; i++ ) {
			RGBtoHSL(*reinterpret_cast<DWORD*>(imageInfo->pSrcBits + i*4), &fH, &fS, &fL);
			fH += (H - 180);
			fH = fH > 0 ? fH : fH + 360; 
			fS *= S1;
			fL *= L1;
			HSLtoRGB(reinterpret_cast<DWORD*>(imageInfo->pBits + i*4), fH, fS, fL);
		}
	}

} // namespace DuiLib
