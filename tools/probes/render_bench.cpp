// 渲染三角测量：GDI 逐像素 vs AlphaBlend vs D2D 语义下的吞吐量
// 目的不是跑分，而是量化"当前架构在 Win11 4K/HDR 下差在哪一层"。
#include "StdAfx.h"
#include "UIlib.h"
#include <cstdio>
#include <chrono>
#include <vector>
#include "Core/UIRender.h"

using clk = std::chrono::high_resolution_clock;
static double ms(clk::time_point a, clk::time_point b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

int main()
{
    const int W = 1920, H = 1080;
    HDC hScreen = ::GetDC(nullptr);
    HDC hMem = ::CreateCompatibleDC(hScreen);

    // --- 基准 1：整屏 ARGB32 DIB 创建 ---
    BYTE* pBits = nullptr;
    auto t0 = clk::now();
    HBITMAP hBmp = DuiLib::CRenderEngine::CreateARGB32Bitmap(hMem, W, H, &pBits);
    auto t1 = clk::now();
    printf("[B1] CreateARGB32Bitmap %dx%d : %.3f ms  bits=%p\n", W, H, ms(t0, t1), (void*)pBits);
    if (!hBmp || !pBits) { printf("FATAL: no bitmap\n"); return 2; }
    HBITMAP hOld = (HBITMAP)::SelectObject(hMem, hBmp);

    // --- 基准 2：逐像素清零（m_bLayered 路径当前写法）---
    const size_t nPix = (size_t)W * H;
    t0 = clk::now();
    for (size_t i = 0; i < nPix; ++i)
        *reinterpret_cast<DWORD*>(pBits + i * 4) = 0;
    t1 = clk::now();
    double tPixelClear = ms(t0, t1);
    printf("[B2] per-pixel clear %zu px : %.3f ms\n", nPix, tPixelClear);

    // --- 基准 3：ZeroMemory 整块清零（同一语义的替代实现）---
    t0 = clk::now();
    ::ZeroMemory(pBits, nPix * 4);
    t1 = clk::now();
    double tZero = ms(t0, t1);
    printf("[B3] ZeroMemory clear         : %.3f ms  (%.1fx faster)\n",
           tZero, tPixelClear / (tZero > 0 ? tZero : 1));

    // --- 基准 4：GDI AlphaBlend 一次整屏 ---
    HDC hSrc = ::CreateCompatibleDC(hScreen);
    BYTE* pSrcBits = nullptr;
    HBITMAP hSrcBmp = DuiLib::CRenderEngine::CreateARGB32Bitmap(hSrc, W, H, &pSrcBits);
    ::SelectObject(hSrc, hSrcBmp);
    if (pSrcBits) ::ZeroMemory(pSrcBits, nPix * 4);

    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    t0 = clk::now();
    for (int i = 0; i < 10; ++i)
        ::AlphaBlend(hMem, 0, 0, W, H, hSrc, 0, 0, W, H, bf);
    t1 = clk::now();
    printf("[B4] AlphaBlend full screen x10 : %.3f ms total, %.3f ms/frame\n",
           ms(t0, t1), ms(t0, t1) / 10);

    // --- 基准 5：DrawImage 每次调用都 CreateCompatibleDC 的开销 ---
    RECT rc = { 0, 0, W, H };
    RECT rcPaint = { 0, 0, 64, 64 };      // 只刷一小块，模拟局部失效
    RECT rcBmpPart = { 0, 0, W, H };
    RECT rcCorner = { 0, 0, 0, 0 };
    t0 = clk::now();
    const int N = 200;
    for (int i = 0; i < N; ++i)
        DuiLib::CRenderEngine::DrawImage(hMem, hBmp, rc, rcPaint, rcBmpPart, rcCorner,
                                 true, 255, false, false, false);
    t1 = clk::now();
    double tDraw = ms(t0, t1);
    printf("[B5] DuiLib::CRenderEngine::DrawImage x%d : %.3f ms total, %.4f ms/call\n",
           N, tDraw, tDraw / N);

    // --- 基准 6：GDI+ 对象创建开销（用于说明为何不选 GDI+ 做主路径）---
    t0 = clk::now();
    for (int i = 0; i < 100; ++i) {
        Gdiplus::Graphics g(hMem);
        Gdiplus::SolidBrush br(Gdiplus::Color(128, 0, 0, 0));
    }
    t1 = clk::now();
    printf("[B6] GDI+ Graphics+Brush x100 : %.3f ms, %.4f ms/obj-pair\n",
           ms(t0, t1), ms(t0, t1) / 100);

    ::SelectObject(hMem, hOld);
    ::DeleteObject(hBmp);
    ::DeleteObject(hSrcBmp);
    ::DeleteDC(hMem);
    ::DeleteDC(hSrc);
    ::ReleaseDC(nullptr, hScreen);

    printf("\n[RENDER-BENCH] done (layered-clear=%.3fms, zeromem=%.3fms, drawimage=%.4fms/call)\n",
           tPixelClear, tZero, tDraw / N);
    return 0;
}
