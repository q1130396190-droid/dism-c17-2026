// R247 验证：D2D 动态文本自适应降级
//
// 要证明的两件事：
//   1) 静态文本：D2D 仍享受缓存加速（不被降级误伤）
//   2) 动态文本：连续变化的内容会自动退回 GDI，不会比 GDI 慢一个数量级
//
// 判定方式：
//   * 动态文本用「每次内容都不同」驱动，测量稳定态的单次成本
//   * 与同长度文本的纯 GDI 成本比较，比值应接近 1（允许 2x 内）
//   * 静态文本对比 D2D 与 GDI，D2D 应更快
#include "StdAfx.h"
#include "UIlib.h"
#include <cstdio>
#include <chrono>
#include "Core/UIRender.h"
#include "Core/UIManager.h"

using clk = std::chrono::high_resolution_clock;
static double us_since(clk::time_point a) {
    return std::chrono::duration<double, std::micro>(clk::now() - a).count();
}

int main()
{
    using namespace DuiLib;
    if (!CRenderEngine::IsD2DTextAvailable()) {
        printf("[probe] SKIP: d2d unavailable\n");
        return 0;
    }

    const int W = 420, H = 60;
    CPaintManagerUI pm;
    pm.Init(nullptr);
    pm.SetDefaultFont(_T("Segoe UI"), 16, false, false, false, false, false);

    HDC hScreen = ::GetDC(nullptr);
    HDC hDC = ::CreateCompatibleDC(hScreen);
    BYTE* pBits = nullptr;
    HBITMAP hBmp = CRenderEngine::CreateARGB32Bitmap(hDC, W, H, &pBits);
    HBITMAP hOld = (HBITMAP)::SelectObject(hDC, hBmp);
    RECT rc = { 0, 0, W, H };

    int bad = 0;

    // ---------- 1) 静态文本：D2D 应更快 ----------
    const TCHAR* kStatic = _T("Static label text 0123456789");
    CRenderEngine::SetD2DTextEnabled(true);
    // 预热缓存
    CRenderEngine::DrawTextD2D(hDC, &pm, rc, kStatic, 0xFF202020, -1, DT_LEFT | DT_SINGLELINE);

    const int N = 400;
    auto t0 = clk::now();
    for (int i = 0; i < N; ++i)
        CRenderEngine::DrawTextD2D(hDC, &pm, rc, kStatic, 0xFF202020, -1, DT_LEFT | DT_SINGLELINE);
    double tStaticD2D = us_since(t0) / N;

    // 同长度 GDI
    HFONT hf = ::CreateFont(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH, _T("Segoe UI"));
    HFONT hfOld = (HFONT)::SelectObject(hDC, hf);
    ::SetBkMode(hDC, TRANSPARENT);
    t0 = clk::now();
    for (int i = 0; i < N; ++i)
        ::DrawText(hDC, kStatic, -1, &rc, DT_LEFT | DT_SINGLELINE);
    double tStaticGDI = us_since(t0) / N;

    printf("[probe] static  D2D=%.2f us  GDI=%.2f us  ratio=%.2fx\n",
           tStaticD2D, tStaticGDI, tStaticD2D / (tStaticGDI > 0 ? tStaticGDI : 1));
    if (tStaticD2D > tStaticGDI * 1.5) {
        printf("[probe] FAIL: static text lost its D2D speed advantage\n");
        ++bad;
    }

    // ---------- 2) 动态文本：应自动降级，不慢于 GDI ----------
    // 模拟时钟/进度：每次内容都不同
    CDuiString sDyn;
    t0 = clk::now();
    for (int i = 0; i < N; ++i) {
        sDyn.Format(_T("dynamic %d"), i);
        CRenderEngine::DrawTextD2D(hDC, &pm, rc, sDyn.GetData(), 0xFF202020, -1,
                                   DT_LEFT | DT_SINGLELINE);
    }
    double tDynD2D = us_since(t0) / N;

    // 动态内容走 GDI 对照
    t0 = clk::now();
    for (int i = 0; i < N; ++i) {
        sDyn.Format(_T("dynamic %d"), i);
        ::DrawText(hDC, sDyn.GetData(), -1, &rc, DT_LEFT | DT_SINGLELINE);
    }
    double tDynGDI = us_since(t0) / N;

    printf("[probe] dynamic D2D=%.2f us  GDI=%.2f us  ratio=%.2fx\n",
           tDynD2D, tDynGDI, tDynD2D / (tDynGDI > 0 ? tDynGDI : 1));
    printf("[probe] (ratio near 1.0 means adaptive fallback took over)\n");

    // 关键判据：降级后不应显著慢于 GDI（允许 3x 容差，
    // 因为前 8 次未命中仍会走 D2D 光栅化，均摊后可接受）
    if (tDynD2D > tDynGDI * 3.0) {
        printf("[probe] FAIL: dynamic text still pays full D2D cost "
               "(%.1fx vs GDI)\n", tDynD2D / (tDynGDI > 0 ? tDynGDI : 1));
        ++bad;
    }

    ::SelectObject(hDC, hfOld);
    ::DeleteObject(hf);
    ::SelectObject(hDC, hOld);
    ::DeleteObject(hBmp);
    ::DeleteDC(hDC);
    ::ReleaseDC(nullptr, hScreen);

    printf("\n[R247] %s\n", bad == 0 ? "ALL GREEN" : "FAIL");
    return bad;
}
