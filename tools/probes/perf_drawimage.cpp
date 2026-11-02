// 优化效果对照：DrawImage 兼容 DC 复用 vs 每次创建
// 用同一份位图重复绘制，测量 CreateCompatibleDC/DeleteDC 的摊销成本。
#include "StdAfx.h"
#include "UIlib.h"
#include <cstdio>
#include <chrono>
#include "Core/UIRender.h"

using clk = std::chrono::high_resolution_clock;
static double ms(clk::time_point a, clk::time_point b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

int main()
{
    const int W = 320, H = 240;
    HDC hScreen = ::GetDC(nullptr);
    HDC hMem = ::CreateCompatibleDC(hScreen);
    BYTE* pBits = nullptr;
    HBITMAP hBmp = DuiLib::CRenderEngine::CreateARGB32Bitmap(hMem, W, H, &pBits);
    HBITMAP hOld = (HBITMAP)::SelectObject(hMem, hBmp);

    RECT rc = { 0, 0, W, H };
    RECT rcPaint = { 0, 0, W, H };
    RECT rcBmpPart = { 0, 0, W, H };
    RECT rcCorner = { 0, 0, 0, 0 };

    const int N = 5000;

    // 基线：手工 CreateCompatibleDC/DeleteDC 每次
    auto t0 = clk::now();
    for (int i = 0; i < N; ++i) {
        HDC dc = ::CreateCompatibleDC(hMem);
        HBITMAP old = (HBITMAP)::SelectObject(dc, hBmp);
        ::SelectObject(dc, old);
        ::DeleteDC(dc);
    }
    auto t1 = clk::now();
    double tCreate = ms(t0, t1);
    printf("[P1] Create/DeleteCompatibleDC x%d : %.3f ms total, %.4f us/call\n",
           N, tCreate, tCreate * 1000 / N);

    // 复用：同一 DC 反复使用
    HDC cached = ::CreateCompatibleDC(hMem);
    t0 = clk::now();
    for (int i = 0; i < N; ++i) {
        HBITMAP old = (HBITMAP)::SelectObject(cached, hBmp);
        ::SelectObject(cached, old);
    }
    t1 = clk::now();
    double tReuse = ms(t0, t1);
    ::DeleteDC(cached);
    printf("[P2] cached DC select/restore x%d : %.3f ms total, %.4f us/call\n",
           N, tReuse, tReuse * 1000 / N);
    printf("[P2] per-call saving: %.4f us (%.1fx)\n",
           (tCreate - tReuse) * 1000 / N,
           tCreate / (tReuse > 0 ? tReuse : 1));

    // 端到端：真实 DrawImage（现已复用 DC）
    t0 = clk::now();
    for (int i = 0; i < N; ++i)
        DuiLib::CRenderEngine::DrawImage(hMem, hBmp, rc, rcPaint, rcBmpPart, rcCorner,
                                         true, 255, false, false, false);
    t1 = clk::now();
    double tDraw = ms(t0, t1);
    printf("[P3] DrawImage (DC cached) x%d : %.3f ms total, %.4f us/call\n",
           N, tDraw, tDraw * 1000 / N);

    ::SelectObject(hMem, hOld);
    ::DeleteObject(hBmp);
    ::DeleteDC(hMem);
    ::ReleaseDC(nullptr, hScreen);

    printf("\n[PERF-RESULT] create=%.4f us/call reuse=%.4f us/call drawimage=%.4f us/call\n",
           tCreate * 1000 / N, tReuse * 1000 / N, tDraw * 1000 / N);
    return 0;
}
