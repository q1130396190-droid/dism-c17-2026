// D2D vs GDI 文字质量与性能对照
// 质量：统计「抗锯齿过渡像素」占比 —— 高质量渲染应有更多中间灰阶而非硬边
// 性能：同等文本重复绘制，比较两条路径的单次耗时
#include "StdAfx.h"
#include "UIlib.h"
#include <cstdio>
#include <chrono>
#include "Core/UIRender.h"
#include "Core/UIManager.h"

using clk = std::chrono::high_resolution_clock;
static double us(clk::time_point a, clk::time_point b)
{
    return std::chrono::duration<double, std::micro>(b - a).count();
}

struct Stats { size_t ink; size_t mid; };   // mid = 中间灰阶（抗锯齿证据）

static Stats Analyze(BYTE* pBits, int w, int h)
{
    Stats s = { 0, 0 };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const BYTE* px = pBits + (y * w + x) * 4;
            if (px[0] | px[1] | px[2] | px[3]) {
                ++s.ink;
                int lum = (px[0] + px[1] + px[2]) / 3;
                if (lum > 20 && lum < 235) ++s.mid;   // 非纯黑非纯白
            }
        }
    return s;
}

int main()
{
    using namespace DuiLib;
    if (!CRenderEngine::IsD2DTextAvailable()) {
        printf("[probe] SKIP: d2d unavailable\n");
        return 0;
    }

    const int W = 480, H = 90;
    CPaintManagerUI pm;
    pm.Init(nullptr);
    pm.SetDefaultFont(_T("Segoe UI"), 16, false, false, false, false, false);

    HDC hScreen = ::GetDC(nullptr);
    HDC hDC = ::CreateCompatibleDC(hScreen);
    BYTE* pBits = nullptr;
    HBITMAP hBmp = CRenderEngine::CreateARGB32Bitmap(hDC, W, H, &pBits);
    HBITMAP hOld = (HBITMAP)::SelectObject(hDC, hBmp);
    RECT rc = { 0, 0, W, H };
    const TCHAR* kText = _T("DuiLib Win11 文字渲染 Quality Test 0123");

    // ---- 质量：D2D ----
    ::ZeroMemory(pBits, (size_t)W * H * 4);
    CRenderEngine::SetD2DTextEnabled(true);
    CRenderEngine::DrawTextD2D(hDC, &pm, rc, kText, 0xFF202020, -1, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    GdiFlush();
    Stats sd = Analyze(pBits, W, H);

    // ---- 质量：GDI ----
    ::ZeroMemory(pBits, (size_t)W * H * 4);
    {
        HFONT hf = ::CreateFont(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH, _T("Segoe UI"));
        HFONT hfOld = (HFONT)::SelectObject(hDC, hf);
        ::SetBkMode(hDC, TRANSPARENT);
        ::SetTextColor(hDC, RGB(0x20, 0x20, 0x20));
        ::DrawText(hDC, kText, -1, &rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        ::SelectObject(hDC, hfOld);
        ::DeleteObject(hf);
    }
    GdiFlush();
    Stats sg = Analyze(pBits, W, H);

    printf("[probe] D2D ink=%zu mid-grey=%zu (%.1f%% AA)\n",
           sd.ink, sd.mid, sd.ink ? 100.0 * sd.mid / sd.ink : 0.0);
    printf("[probe] GDI ink=%zu mid-grey=%zu (%.1f%% AA)\n",
           sg.ink, sg.mid, sg.ink ? 100.0 * sg.mid / sg.ink : 0.0);

    // ---- 性能 ----
    const int N = 300;
    auto t0 = clk::now();
    CRenderEngine::SetD2DTextEnabled(true);
    for (int i = 0; i < N; ++i)
        CRenderEngine::DrawTextD2D(hDC, &pm, rc, kText, 0xFF202020, -1, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    auto t1 = clk::now();
    double tD2D = us(t0, t1) / N;

    t0 = clk::now();
    {
        HFONT hf = ::CreateFont(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH, _T("Segoe UI"));
        HFONT hfOld = (HFONT)::SelectObject(hDC, hf);
        ::SetBkMode(hDC, TRANSPARENT);
        ::SetTextColor(hDC, RGB(0x20, 0x20, 0x20));
        for (int i = 0; i < N; ++i)
            ::DrawText(hDC, kText, -1, &rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        ::SelectObject(hDC, hfOld);
        ::DeleteObject(hf);
    }
    t1 = clk::now();
    double tGDI = us(t0, t1) / N;

    printf("[probe] perf D2D=%.1f us/call  GDI=%.1f us/call  ratio=%.2fx\n",
           tD2D, tGDI, tD2D / (tGDI > 0 ? tGDI : 1));

    ::SelectObject(hDC, hOld);
    ::DeleteObject(hBmp);
    ::DeleteDC(hDC);
    ::ReleaseDC(nullptr, hScreen);

    int bad = 0;
    if (sd.ink < 200) { printf("[probe] FAIL: D2D drew almost nothing\n"); ++bad; }
    printf("\n[D2D-QUALITY] %s\n", bad == 0 ? "PASS" : "FAIL");
    return bad;
}
