// 验证 SetStretchBltMode 是否为主要开销
// DrawImage 每次调用都会 SetStretchBltMode(hDC, HALFTONE)，
// 在 GDI 下 HALFTONE 会重置缩放上下文，代价可能很高。
#include "StdAfx.h"
#include "UIlib.h"
#include <cstdio>
#include <chrono>

using clk = std::chrono::high_resolution_clock;
static double us(clk::time_point a, clk::time_point b)
{
    return std::chrono::duration<double, std::micro>(b - a).count();
}

int main()
{
    const int W = 1920, H = 1080;
    HDC hScreen = ::GetDC(nullptr);
    HDC hMem = ::CreateCompatibleDC(hScreen);
    BYTE* pBits = nullptr;
    HBITMAP hBmp = DuiLib::CRenderEngine::CreateARGB32Bitmap(hMem, W, H, &pBits);
    HBITMAP hOld = (HBITMAP)::SelectObject(hMem, hBmp);

    const int N = 200;

    // A: 每次 SetStretchBltMode(HALFTONE)
    auto t0 = clk::now();
    for (int i = 0; i < N; ++i)
        ::SetStretchBltMode(hMem, HALFTONE);
    auto t1 = clk::now();
    printf("[H1] SetStretchBltMode(HALFTONE) x%d : %.1f us/call\n", N, us(t0, t1) / N);

    // B: 每次 SetStretchBltMode(COLORONCOLOR)
    t0 = clk::now();
    for (int i = 0; i < N; ++i)
        ::SetStretchBltMode(hMem, COLORONCOLOR);
    t1 = clk::now();
    printf("[H2] SetStretchBltMode(COLORONCOLOR) x%d : %.1f us/call\n", N, us(t0, t1) / N);

    // C: 一次性设置，之后只做 AlphaBlend（同尺寸，1:1）
    ::SetStretchBltMode(hMem, COLORONCOLOR);
    HDC hSrc = ::CreateCompatibleDC(hScreen);
    BYTE* pSrc = nullptr;
    HBITMAP hSrcBmp = DuiLib::CRenderEngine::CreateARGB32Bitmap(hSrc, W, H, &pSrc);
    ::SelectObject(hSrc, hSrcBmp);
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };

    t0 = clk::now();
    for (int i = 0; i < N; ++i)
        ::AlphaBlend(hMem, 0, 0, W, H, hSrc, 0, 0, W, H, bf);
    t1 = clk::now();
    printf("[H3] AlphaBlend 1:1 full screen x%d : %.1f us/call\n", N, us(t0, t1) / N);

    ::SelectObject(hMem, hOld);
    ::DeleteObject(hBmp);
    ::DeleteObject(hSrcBmp);
    ::DeleteDC(hMem);
    ::DeleteDC(hSrc);
    ::ReleaseDC(nullptr, hScreen);
    printf("\n[HALFTONE-BENCH] done\n");
    return 0;
}
