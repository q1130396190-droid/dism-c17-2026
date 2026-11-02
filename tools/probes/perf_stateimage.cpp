// PERF-03 收益实测：PaintStatusImage 的 TDrawInfo 解析缓存
// 直接对比「每帧 Parse」vs「缓存命中」的成本差。
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

int main()
{
    using namespace DuiLib;
    CPaintManagerUI pm;
    pm.Init(nullptr);
    pm.SetDefaultFont(_T("Segoe UI"), 16, false, false, false, false, false);

    // 典型按钮状态图描述串（与真实皮肤同构）
    LPCTSTR kState = _T("file='btn_states.png' source='0,0,120,40' dest='0,0,120,40' mask='#FF00FF' fade='255'");

    const int N = 20000;

    // A: 每次重新 Parse（改造前的行为）
    auto t0 = clk::now();
    for (int i = 0; i < N; ++i) {
        TDrawInfo info;
        info.Parse(kState, _T(""), &pm);
    }
    auto t1 = clk::now();
    double tParse = us(t0, t1) / N;
    printf("[probe] TDrawInfo::Parse each call : %.2f us/call\n", tParse);

    // B: 缓存命中（改造后：只做一次字符串比较）
    DuiLib::CButtonUI btn;
    btn.SetAttribute(_T("stateimage"), kState);
    // 先触发一次真实绘制以填充缓存
    HDC hScreen = ::GetDC(nullptr);
    HDC hDC = ::CreateCompatibleDC(hScreen);
    BYTE* pBits = nullptr;
    HBITMAP hBmp = CRenderEngine::CreateARGB32Bitmap(hDC, 200, 60, &pBits);
    HBITMAP hOld = (HBITMAP)::SelectObject(hDC, hBmp);
    btn.SetPos(DuiLib::CDuiRect(0, 0, 200, 60));
    btn.SetManager(&pm, nullptr, false);
    btn.SetVisible(true);
    btn.Paint(hDC, DuiLib::CDuiRect(0, 0, 200, 60), nullptr);

    t0 = clk::now();
    for (int i = 0; i < N; ++i) {
        btn.Paint(hDC, DuiLib::CDuiRect(0, 0, 200, 60), nullptr);
    }
    t1 = clk::now();
    double tPaint = us(t0, t1) / N;
    printf("[probe] full button Paint (cached) : %.2f us/call\n", tPaint);

    ::SelectObject(hDC, hOld);
    ::DeleteObject(hBmp);
    ::DeleteDC(hDC);
    ::ReleaseDC(nullptr, hScreen);

    printf("\n[PERF-03] parse=%.2f us  paint=%.2f us  avoided=%.2f us/repaint\n",
           tParse, tPaint, tParse);
    return 0;
}
