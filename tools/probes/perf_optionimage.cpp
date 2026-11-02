// PERF-04 实测（v2）：COptionUI 状态图解析缓存的真实收益
//
// v1 的判定错误：拿 Paint 总成本与单独 Parse 比大小。
// Paint 还包含背景、边框、图片绘制，成本必然高于 Parse，
// 因此 "Paint > Parse" 不能证明缓存无效。
//
// v2 正确做法：同一控件、同一路径，比较
//   HIT  组：源码串不变，反复 Paint -> 缓存命中
//   MISS 组：每次改一个字符，强制重新 Parse -> 缓存未命中
// 两者差值即为「缓存省下的解析成本」。若差值接近 0，说明缓存无效。
#include "StdAfx.h"
#include "UIlib.h"
#include <cstdio>
#include <chrono>
#include "Core/UIRender.h"
#include "Core/UIManager.h"
#include "Control/UIOption.h"

using clk = std::chrono::high_resolution_clock;
static double us_since(clk::time_point a) {
    return std::chrono::duration<double, std::micro>(clk::now() - a).count();
}

int main()
{
    using namespace DuiLib;

    CPaintManagerUI pm;
    pm.Init(nullptr);
    pm.SetDefaultFont(_T("Segoe UI"), 16, false, false, false, false, false);

    HDC hScreen = ::GetDC(nullptr);
    HDC hDC = ::CreateCompatibleDC(hScreen);
    BYTE* pBits = nullptr;
    HBITMAP hBmp = CRenderEngine::CreateARGB32Bitmap(hDC, 220, 60, &pBits);
    HBITMAP hOld = (HBITMAP)::SelectObject(hDC, hBmp);

    const TCHAR* kBase =
        _T("file='opt_states.png' source='0,0,160,40' dest='0,0,160,40' mask='#FF00FF' fade='255'");

    COptionUI opt;
    opt.SetSelectedStateCount(2);
    opt.SetSelectedStateImage(kBase);
    opt.Selected(true, false);
    opt.SetPos(CDuiRect(0, 0, 220, 60));
    opt.SetManager(&pm, nullptr, false);
    opt.SetVisible(true);

    RECT rcDirty = { 0, 0, 220, 60 };
    const int N = 2000;

    // 预热
    opt.Paint(hDC, rcDirty, nullptr);

    // ---- HIT：字符串不变，缓存应命中 ----
    auto t0 = clk::now();
    for (int i = 0; i < N; ++i)
        opt.Paint(hDC, rcDirty, nullptr);
    double tHit = us_since(t0) / N;

    // ---- MISS：每次换一个尾随标记，强制重新 Parse ----
    CDuiString sBuf;
    t0 = clk::now();
    for (int i = 0; i < N; ++i) {
        sBuf.Format(_T("%s "), kBase);          // 每次内容不同 -> 缓存失效
        opt.SetSelectedStateImage(sBuf.GetData());
        opt.Paint(hDC, rcDirty, nullptr);
    }
    double tMiss = us_since(t0) / N;

    printf("[probe] Paint HIT  (cache reused) : %.2f us/call\n", tHit);
    printf("[probe] Paint MISS (re-parsed)    : %.2f us/call\n", tMiss);
    printf("[probe] saved per repaint         : %.2f us\n", tMiss - tHit);

    opt.SetManager(nullptr, nullptr, false);
    ::SelectObject(hDC, hOld);
    ::DeleteObject(hBmp);
    ::DeleteDC(hDC);
    ::ReleaseDC(nullptr, hScreen);

    // 判定：命中组必须显著快于未命中组
    int bad = 0;
    double saved = tMiss - tHit;
    if (tMiss <= 0 || tHit <= 0) {
        printf("[probe] FAIL: invalid timing\n"); ++bad;
    } else if (saved < tMiss * 0.10) {
        printf("[probe] FAIL: saving %.1f us is under 10%% of %.1f us -> cache ineffective\n",
               saved, tMiss);
        ++bad;
    } else {
        printf("[probe] PASS: cache saves %.1f%% of paint cost\n", 100.0 * saved / tMiss);
    }
    printf("\n[PERF-04] %s\n", bad == 0 ? "ALL GREEN" : "FAIL");
    return bad;
}
