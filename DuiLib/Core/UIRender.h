#ifndef __UIRENDER_H__
#define __UIRENDER_H__

#pragma once

#ifdef USE_XIMAGE_EFFECT
class CxImage;
#endif

namespace DuiLib {
	/////////////////////////////////////////////////////////////////////////////////////
	//

	class UILIB_API CRenderClip
	{
	public:
		// [SAFE-2] Owns two GDI regions (hRgn / hOldRgn) that the destructor deletes
		// (UIRender.cpp:99-100). Every use site is a stack temporary:
		//     CRenderClip clip;
		//     CRenderClip::GenerateClip(hDC, rc, clip);
		// so a copy would delete the same regions twice. Deleting the copy operations
		// costs nothing at those sites and turns a future by-value pass into a compile
		// error instead of a heap double-free.
		CRenderClip() = default;
		CRenderClip(const CRenderClip&) = delete;
		CRenderClip& operator=(const CRenderClip&) = delete;
		~CRenderClip();
		RECT rcItem;
		HDC hDC;
		HRGN hRgn;
		HRGN hOldRgn;

		static void GenerateClip(HDC hDC, RECT rc, CRenderClip& clip);
		static void GenerateRoundClip(HDC hDC, RECT rc, RECT rcItem, int width, int height, CRenderClip& clip);
		static void UseOldClipBegin(HDC hDC, CRenderClip& clip);
		static void UseOldClipEnd(HDC hDC, CRenderClip& clip);
	};

	/////////////////////////////////////////////////////////////////////////////////////
	//


	/////////////////////////////////////////////////////////////////////////////////////
	// [R240] Render backend abstraction.
	//
	// DuiLib historically called GDI primitives directly from every control, so
	// swapping the rasteriser (Direct2D, Skia, a test spy) meant editing all of
	// them. This interface puts a seam at the *primitive* level instead: the
	// public CRenderEngine entry points dispatch to the installed backend, and
	// with no backend installed they run the original GDI code unchanged.
	//
	// Contract: an implementation must be safe to call from the UI thread only,
	// and must not assume any particular DC state (the caller may have set a
	// clip or a transform).
	class UILIB_API IRenderBackend
	{
	public:
		virtual ~IRenderBackend() {}

		// Human-readable identifier, used by diagnostics and probes.
		virtual const TCHAR* Name() const = 0;

		// Filled rectangle (alpha ignored, matching the GDI path).
		virtual void DrawColor(HDC hDC, const RECT& rc, DWORD dwColor) = 0;

		// Vertical/horizontal two-stop gradient.
		virtual void DrawGradient(HDC hDC, const RECT& rc, DWORD dwFirst,
								  DWORD dwSecond, bool bVertical, int nSteps) = 0;

		virtual void DrawLine(HDC hDC, const RECT& rc, int nSize,
							  DWORD dwPenColor, int nStyle) = 0;

		virtual void DrawRect(HDC hDC, const RECT& rc, int nSize,
							  DWORD dwPenColor, int nStyle) = 0;

		virtual void DrawRoundRect(HDC hDC, const RECT& rc, int width, int height,
								   int nSize, DWORD dwPenColor, int nStyle) = 0;

		// [R251] Image blit. This is the other hot primitive: a list or table
		// repaints hundreds of images per frame. Giving it the same seam as the
		// geometry primitives is what makes the backend useful for real UIs.
		// hBitmap is the DIB to blit; the corner/part rectangles describe the
		// nine-slice and source sub-rectangle exactly as the GDI path does.
		virtual void DrawImage(HDC hDC, HBITMAP hBitmap, const RECT& rc,
							   const RECT& rcPaint, const RECT& rcBmpPart,
							   const RECT& rcCorners, bool bAlpha, UINT uFade,
							   bool hole, bool xtiled, bool ytiled) = 0;

		// [R258] Text. The last drawing dimension to get the seam: with this the
		// whole per-frame surface (geometry, images, text) can be redirected at
		// one install point. A backend that returns without drawing leaves the
		// repaint incomplete, so implementations must draw or the host must not
		// install them.
		virtual void DrawText(HDC hDC, CPaintManagerUI* pManager, RECT& rc,
							  LPCTSTR pstrText, DWORD dwTextColor, int iFont, UINT uStyle) = 0;
	};

	// Default backend: forwards every primitive back to CRenderEngine's own GDI
	// implementations, so installing it changes nothing except taking the
	// (short) virtual-call path.
	class UILIB_API CGdiRenderBackend : public IRenderBackend
	{
	public:
		const TCHAR* Name() const override;
		void DrawColor(HDC hDC, const RECT& rc, DWORD dwColor) override;
		void DrawGradient(HDC hDC, const RECT& rc, DWORD dwFirst,
						  DWORD dwSecond, bool bVertical, int nSteps) override;
		void DrawLine(HDC hDC, const RECT& rc, int nSize,
					  DWORD dwPenColor, int nStyle) override;
		void DrawRect(HDC hDC, const RECT& rc, int nSize,
					  DWORD dwPenColor, int nStyle) override;
		void DrawRoundRect(HDC hDC, const RECT& rc, int width, int height,
						   int nSize, DWORD dwPenColor, int nStyle) override;
		void DrawImage(HDC hDC, HBITMAP hBitmap, const RECT& rc,
					   const RECT& rcPaint, const RECT& rcBmpPart,
					   const RECT& rcCorners, bool bAlpha, UINT uFade,
					   bool hole, bool xtiled, bool ytiled) override;
		void DrawText(HDC hDC, CPaintManagerUI* pManager, RECT& rc,
					  LPCTSTR pstrText, DWORD dwTextColor, int iFont, UINT uStyle) override;
	};


	/////////////////////////////////////////////////////////////////////////////////////
	// [R240] Render backend abstraction.
	//
	// DuiLib historically called GDI primitives directly from every control, so
	// swapping the rasteriser (Direct2D, Skia, a test spy) meant editing all of
	// them. This interface puts a seam at the *primitive* level instead: the
	// public CRenderEngine entry points dispatch to the installed backend, and
	// with no backend installed they run the original GDI code unchanged.
	//
	// Contract: an implementation must be safe to call from the UI thread only,
	// and must not assume any particular DC state (the caller may have set a
	// clip or a transform).

	class UILIB_API CRenderEngine
	{
	public:
		// 图片加载
#ifdef USE_XIMAGE_EFFECT
		static CxImage *LoadGifImageX(STRINGorID bitmap, LPCTSTR type = nullptr, DWORD mask = 0);
#endif
		static TImageInfo* LoadImage(STRINGorID bitmap, LPCTSTR type = nullptr, DWORD mask = 0, HINSTANCE instance = nullptr);
		static void FreeImage(TImageInfo* pImageInfo, bool bDelete = true);
		static TImageInfo* LoadImage(LPCTSTR pStrImage, LPCTSTR type = nullptr, DWORD mask = 0, HINSTANCE instance = nullptr);
		static TImageInfo* LoadImage(UINT nID, LPCTSTR type = nullptr, DWORD mask = 0, HINSTANCE instance = nullptr);

		static void DrawImage(HDC hDC, HBITMAP hBitmap, const RECT& rc, const RECT& rcPaint, const RECT& rcBmpPart, const RECT& rcCorners, bool bAlpha, UINT uFade = 255, bool hole = false, bool xtiled = false, bool ytiled = false);
		static bool DrawImageInfo(HDC hDC, CPaintManagerUI* pManager, const RECT& rcItem, const RECT& rcPaint, const TDrawInfo* pDrawInfo, HINSTANCE instance = nullptr);
		static bool DrawImageString(HDC hDC, CPaintManagerUI* pManager, const RECT& rcItem, const RECT& rcPaint, LPCTSTR pStrImage, LPCTSTR pStrModify = nullptr, HINSTANCE instance = nullptr);

		// Gdiplus绘制
		static TImageInfo* GdiplusLoadImage(STRINGorID bitmap, LPCTSTR type = nullptr, DWORD mask = 0, HINSTANCE instance = nullptr);
		static void GdiplusDrawImage(HDC hDC, Gdiplus::Image* image, const RECT& rc, const RECT& rcPaint, const RECT& rcBmpPart, bool bAlpha, UINT uFade = 255, UINT uRotate = 0);

	public:
		// [WIN11-03] Opt-in Direct2D/DirectWrite text backend.
		//
		// DirectWrite gives correct CJK layout and sub-pixel positioning that GDI
		// DrawText cannot express. Any failure returns false and the caller keeps
		// using the GDI path, so enabling this can only affect text quality, never
		// correctness. Off by default: the compatibility line stays byte-identical.
		static bool DrawTextD2D(HDC hDC, CPaintManagerUI* pManager, RECT& rc,
								LPCTSTR pstrText, DWORD dwTextColor, int iFont, UINT uStyle);
		static bool IsD2DTextAvailable();
		// [PERF-20] Build the Direct2D device on a background thread. Creating
		// the DCRenderTarget brings up the WARP/DXGI device and costs ~100ms
		// on this hardware (invariant across thread/factory modes), which used
		// to sit directly on the startup critical path. Call this as early as
		// possible; IsD2DTextAvailable() still builds synchronously if the
		// warm-up did not run or has not finished.
		static void WarmUpD2DTextAsync();
		// Block until the async warm-up finishes (diagnostics / tests only).
		static bool WaitD2DTextWarmUp(unsigned nTimeoutMs);
		static void SetD2DTextEnabled(bool bEnable);
		static bool IsD2DTextEnabled();
		static void ClearD2DTextCache();
		static void GdiplusDrawText(HDC hDC, CPaintManagerUI* pManager, RECT& rc, LPCTSTR pstrText, DWORD dwTextColor, int iFont, UINT uStyle);

		// 以下函数中的颜色参数alpha值无效
		// 图元绘制
		static void DrawColor(HDC hDC, const RECT& rc, DWORD color);
		static void DrawGradient(HDC hDC, const RECT& rc, DWORD dwFirst, DWORD dwSecond, bool bVertical, int nSteps);
		static void DrawLine(HDC hDC, const RECT& rc, int nSize, DWORD dwPenColor,int nStyle = PS_SOLID);
		static void DrawRect(HDC hDC, const RECT& rc, int nSize, DWORD dwPenColor,int nStyle = PS_SOLID);
		static void DrawRoundRect(HDC hDC, const RECT& rc, int width, int height, int nSize, DWORD dwPenColor,int nStyle = PS_SOLID);

		// [WIN11-ADAPT] 圆角实心填充。
		//
		// 背景：本框架原先只有 DrawRoundRect（描边），纯色背景一律走
		// DrawColor（直角矩形）。结果是皮肤里写 borderround 后，只有 1px
		// 描边是圆的，填充底色仍是直角，Win11 的圆角按钮无法表达。
		// 本函数补齐"圆角填充"这一半能力，供 CControlUI::PaintBkColor 在有
		// borderround 时使用；cxy<=0 时调用方应继续走 DrawColor。
		static void DrawRoundRectFilled(HDC hDC, const RECT& rc, int cxyRound, DWORD dwColor);

		// 字体绘制
		static void DrawText(HDC hDC, CPaintManagerUI* pManager, RECT& rc, LPCTSTR pstrText,DWORD dwTextColor, int iFont, UINT uStyle, DWORD dwTextBKColor);
		static void DrawText(HDC hDC, CPaintManagerUI* pManager, RECT& rc, LPCTSTR pstrText, DWORD dwTextColor, int iFont, UINT uStyle);
		static void DrawHtmlText(HDC hDC, CPaintManagerUI* pManager, RECT& rc, LPCTSTR pstrText, DWORD dwTextColor, RECT* pLinks, CDuiString* sLinks, int& nLinkRects, int iFont, UINT uStyle);

		// 辅助函数
		static void CheckAlphaColor(DWORD& dwColor);
		static DWORD AdjustColor(DWORD dwColor, short H, short S, short L);


		// [R240] Render backend plug-in point.
		//
		// Install nullptr to return to the built-in GDI path (default).
		// The caller keeps ownership of the backend object; uninstall it before
		// the object dies. Passing the same pointer again is a no-op.
		static void SetRenderBackend(IRenderBackend* pBackend);
		static IRenderBackend* GetRenderBackend();
		static bool IsUsingCustomBackend();

		static HBITMAP CreateARGB32Bitmap(HDC hDC, int cx, int cy, BYTE** pBits);
		static void AdjustImage(bool bUseHSL, TImageInfo* imageInfo, short H, short S, short L);

		static HBITMAP GenerateBitmap(CPaintManagerUI* pManager, RECT rc, CControlUI* pStopControl = nullptr, DWORD dwFilterColor = 0);
		static HBITMAP GenerateBitmap(CPaintManagerUI* pManager, CControlUI* pControl, RECT rc, DWORD dwFilterColor = 0);
		static SIZE GetTextSize(HDC hDC, CPaintManagerUI* pManager , LPCTSTR pstrText, int iFont, UINT uStyle);

	};

} // namespace DuiLib

#endif // __UIRENDER_H__
