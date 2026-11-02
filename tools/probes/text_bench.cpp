// 文字渲染与控件树遍历基准
// 说明 Win11 下 GDI DrawText(单次/多字符串) 的实际吞吐，以及控件树遍历开销。
#include "StdAfx.h"
#include "UIlib.h"
#include <cstdio>
#include <chrono>
#include <vector>
#include <string>
#include "Core/UIControl.h"
#include "Core/UIContainer.h"
#include "Control/UIButton.h"

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
    BYTE* pBits = nullptr;
    HBITMAP hBmp = DuiLib::CRenderEngine::CreateARGB32Bitmap(hMem, W, H, &pBits);
    HBITMAP hOld = (HBITMAP)::SelectObject(hMem, hBmp);
    ::SetBkMode(hMem, TRANSPARENT);

    HFONT hFont = ::CreateFont(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH, _T("Segoe UI"));
    HFONT hOldFont = (HFONT)::SelectObject(hMem, hFont);

    // --- B7: DrawText 单行 x 1000 ---
    const int N = 1000;
    RECT rc = { 10, 10, 400, 40 };
    auto t0 = clk::now();
    for (int i = 0; i < N; ++i) {
        ::DrawText(hMem, _T("DuiLib Win11 text rendering probe"), -1, &rc,
                   DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }
    auto t1 = clk::now();
    double tDrawText = ms(t0, t1);
    printf("[B7] DrawText single line x%d : %.3f ms total, %.4f ms/call\n",
           N, tDrawText, tDrawText / N);

    // --- B8: DrawTextEx ---
    t0 = clk::now();
    for (int i = 0; i < N; ++i) {
        ::DrawTextEx(hMem, const_cast<LPTSTR>(_T("DuiLib Win11 text rendering probe")), -1,
                     &rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER, nullptr);
    }
    t1 = clk::now();
    double tDrawTextEx = ms(t0, t1);
    printf("[B8] DrawTextEx  single line x%d : %.3f ms total, %.4f ms/call\n",
           N, tDrawTextEx, tDrawTextEx / N);

    // --- B9: 每帧 100 个文本控件（模拟列表/表格）---
    t0 = clk::now();
    for (int f = 0; f < 10; ++f) {
        for (int i = 0; i < 100; ++i) {
            RECT r = { 10, 10 + i * 20, 600, 30 + i * 20 };
            ::DrawText(hMem, _T("row item"), -1, &r, DT_LEFT | DT_SINGLELINE);
        }
    }
    t1 = clk::now();
    printf("[B9] 100 texts x10 frames       : %.3f ms total, %.3f ms/frame\n",
           ms(t0, t1), ms(t0, t1) / 10);

    // --- B10: 控件树构建/遍历（纯内存侧）---
    t0 = clk::now();
    DuiLib::CVerticalLayoutUI root;
    for (int i = 0; i < 2000; ++i) {
        auto* b = new DuiLib::CButtonUI();
        b->SetText(_T("btn"));
        b->SetName(_T("b"));
        root.Add(b);
    }
    t1 = clk::now();
    printf("[B10] build 2000 controls      : %.3f ms\n", ms(t0, t1));

    t0 = clk::now();
    for (int pass = 0; pass < 100; ++pass) {
        for (int i = 0; i < root.GetCount(); ++i) {
            DuiLib::CControlUI* c = root.GetItemAt(i);
            (void)c->GetName();
            (void)c->IsVisible();
        }
    }
    t1 = clk::now();
    printf("[B11] iterate 2000 controls x100 : %.3f ms total, %.4f ms/pass\n",
           ms(t0, t1), ms(t0, t1) / 100);

    // --- B12: 属性查找（S3 哈希快速路径 vs 名字比较）---
    DuiLib::CButtonUI probe;
    t0 = clk::now();
    for (int i = 0; i < 20000; ++i) {
        probe.SetAttribute(_T("text"), _T("x"));
    }
    t1 = clk::now();
    printf("[B12] SetAttribute(\"text\") x20000 : %.3f ms total, %.4f us/call\n",
           ms(t0, t1), ms(t0, t1) * 1000 / 20000);

    ::SelectObject(hMem, hOldFont);
    ::DeleteObject(hFont);
    ::SelectObject(hMem, hOld);
    ::DeleteObject(hBmp);
    ::DeleteDC(hMem);
    ::ReleaseDC(nullptr, hScreen);

    printf("\n[TEXT-BENCH] done (drawtext=%.4f ms/call, 100text/frame=%.3f ms)\n",
           tDrawText / N, 0.0);
    return 0;
}
