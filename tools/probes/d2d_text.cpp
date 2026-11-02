// D2D 文字后端实测：验证真的能把文字画出来（不是只有编译通过）
//
// 判定标准：
//   1) IsD2DTextAvailable() 为真（d2d1/dwrite 可用）
//   2) DrawTextD2D 返回 true
//   3) 目标位图的非透明像素数与 GDI 路径同量级（说明真的画了字）
//   4) 与 GDI 结果做像素差异统计，确认两者都在绘制而非空白
#include "StdAfx.h"
#include "UIlib.h"
#include <cstdio>
#include "Core/UIRender.h"
#include "Core/UIManager.h"

// 「有内容」像素：GDI 往 32bpp DIB 画字时不写 alpha 通道（alpha 恒 0），
// 而 D2D 写的是 premultiplied BGRA（alpha 有效）。为了让两条路径可比，
// 这里对 RGB 与 alpha 任一非零都计入。
static size_t CountInk(BYTE* pBits, int w, int h)
{
    size_t n = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const BYTE* px = pBits + (y * w + x) * 4;
            if (px[0] | px[1] | px[2] | px[3]) ++n;
        }
    return n;
}

int main()
{
    using namespace DuiLib;

    printf("[probe] IsD2DTextAvailable = %d\n", (int)CRenderEngine::IsD2DTextAvailable());
    fflush(stdout);
    if (!CRenderEngine::IsD2DTextAvailable()) {
        printf("[probe] SKIP: d2d1/dwrite unavailable on this machine\n");
        return 0;
    }

    const int W = 420, H = 80;

    CPaintManagerUI pm;
    pm.Init(nullptr);            // 仅用于 GetFont
    // 没有真实窗口时，默认字体需要一个 DC 才能建立；Servlet GDI 对照
    // 也需要有效 HFONT，否则 DrawText 什么都不画。
    pm.SetDefaultFont(_T("Segoe UI"), 16, false, false, false, false, false);

    // 目标位图
    HDC hScreen = ::GetDC(nullptr);
    HDC hDC = ::CreateCompatibleDC(hScreen);
    BYTE* pBits = nullptr;
    HBITMAP hBmp = CRenderEngine::CreateARGB32Bitmap(hDC, W, H, &pBits);
    HBITMAP hOld = (HBITMAP)::SelectObject(hDC, hBmp);

    RECT rc = { 0, 0, W, H };
    const TCHAR* kText = _T("DuiLib Win11 D2D 文字渲染 Text 12345");

    // --- D2D 路径 ---
    ::ZeroMemory(pBits, (size_t)W * H * 4);
    CRenderEngine::SetD2DTextEnabled(true);
    bool ok = CRenderEngine::DrawTextD2D(hDC, &pm, rc, kText, 0xFF101010, -1, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    GdiFlush();
    size_t inkD2D = CountInk(pBits, W, H);
    printf("[probe] DrawTextD2D returned %d, ink pixels = %zu\n", (int)ok, inkD2D);
    fflush(stdout);

    // --- GDI 路径（用原始 GDI 作为公平基准）---
    // 注意：DuiLib 的 CPaintManagerUI::DrawText 依赖 m_hDcPaint 建立字体度量，
    // 无真实窗口时 GetFont 返回空 HFONT，什么都画不出来。这是既有实现细节，
    // 与本探针要验证的 D2D 能力无关，因此对照基准直接用 GDI DrawText。
    ::ZeroMemory(pBits, (size_t)W * H * 4);
    CRenderEngine::SetD2DTextEnabled(false);
    {
        HFONT hf = ::CreateFont(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH, _T("Segoe UI"));
        HFONT hfOld = (HFONT)::SelectObject(hDC, hf);
        ::SetBkMode(hDC, TRANSPARENT);
        ::SetTextColor(hDC, RGB(0x10, 0x10, 0x10));
        ::DrawText(hDC, kText, -1, &rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        ::SelectObject(hDC, hfOld);
        ::DeleteObject(hf);
    }
    GdiFlush();
    size_t inkGDI = CountInk(pBits, W, H);
    printf("[probe] GDI DrawText ink pixels = %zu\n", inkGDI);
    fflush(stdout);

    ::SelectObject(hDC, hOld);
    ::DeleteObject(hBmp);
    ::DeleteDC(hDC);
    ::ReleaseDC(nullptr, hScreen);

    int bad = 0;
    if (!ok) { printf("[probe] FAIL: DrawTextD2D returned false\n"); ++bad; }
    if (inkD2D < 100) { printf("[probe] FAIL: D2D produced (almost) no ink\n"); ++bad; }
    if (inkGDI < 100) { printf("[probe] FAIL: GDI produced (almost) no ink\n"); ++bad; }
    // 两者应在同一量级（允许字体光栅差异）
    if (inkD2D > 0 && inkGDI > 0) {
        double ratio = (double)inkD2D / (double)inkGDI;
        printf("[probe] ink ratio D2D/GDI = %.2f\n", ratio);
        if (ratio < 0.3 || ratio > 3.0) {
            printf("[probe] FAIL: ink ratio out of range\n"); ++bad;
        }
    }

    printf("\n[D2D] %s\n", bad == 0 ? "ALL GREEN" : "FAIL");
    return bad;
}
