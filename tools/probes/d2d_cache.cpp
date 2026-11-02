// D2D 缓存正确性验证：不同文本/颜色/尺寸不能互相串图
//
// 缓存以 (字体,颜色,样式,宽,高,文本) 为键。本探针逐项改变一个维度，
// 确认输出像素与「首次无缓存渲染」一致，防止键设计缺陷导致串图。
#include "StdAfx.h"
#include "UIlib.h"
#include <cstdio>
#include <vector>
#include "Core/UIRender.h"
#include "Core/UIManager.h"

using DuiLib::CPaintManagerUI;
using DuiLib::CRenderEngine;

static size_t HashBits(BYTE* p, int n)
{
    size_t h = 1469598103934665603ull;
    for (int i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

struct Shot { size_t hash; size_t ink; };

static Shot Render(HDC hDC, BYTE* pBits, CPaintManagerUI* pm, int W, int H,
                   const TCHAR* text, DWORD color, int font, UINT style)
{
    ::ZeroMemory(pBits, (size_t)W * H * 4);
    RECT rc = { 0, 0, W, H };
    CRenderEngine::DrawTextD2D(hDC, pm, rc, text, color, font, style);
    GdiFlush();
    size_t ink = 0;
    for (int i = 0; i < W * H; ++i) if (pBits[i * 4 + 3]) ++ink;
    return { HashBits(pBits, W * H * 4), ink };
}

int main()
{
    if (!CRenderEngine::IsD2DTextAvailable()) { printf("[probe] SKIP\n"); return 0; }

    const int W = 360, H = 70;
    CPaintManagerUI pm;
    pm.Init(nullptr);
    pm.SetDefaultFont(_T("Segoe UI"), 16, false, false, false, false, false);

    HDC hScreen = ::GetDC(nullptr);
    HDC hDC = ::CreateCompatibleDC(hScreen);
    BYTE* pBits = nullptr;
    HBITMAP hBmp = CRenderEngine::CreateARGB32Bitmap(hDC, W, H, &pBits);
    HBITMAP hOld = (HBITMAP)::SelectObject(hDC, hBmp);

    CRenderEngine::SetD2DTextEnabled(true);
    const DWORD kCol = 0xFF303030;

    int bad = 0;
    // 基线（无缓存）
    CRenderEngine::ClearD2DTextCache();
    Shot a = Render(hDC, pBits, &pm, W, H, _T("Alpha"), kCol, -1, DT_LEFT | DT_SINGLELINE);
    printf("[probe] baseline 'Alpha'      ink=%zu hash=%zx\n", a.ink, a.hash);

    // 1) 同参数二次渲染 -> 必须与基线完全相同（缓存命中路径）
    Shot a2 = Render(hDC, pBits, &pm, W, H, _T("Alpha"), kCol, -1, DT_LEFT | DT_SINGLELINE);
    printf("[probe] repeat  'Alpha'      ink=%zu hash=%zx  %s\n", a2.ink, a2.hash,
           a2.hash == a.hash ? "SAME" : "DIFF");
    if (a2.hash != a.hash) { printf("  FAIL: cache hit changed pixels\n"); ++bad; }

    // 2) 不同文本 -> 必须不同，且不等于基线
    Shot b = Render(hDC, pBits, &pm, W, H, _T("Bravo"), kCol, -1, DT_LEFT | DT_SINGLELINE);
    printf("[probe] different text          ink=%zu hash=%zx  %s\n", b.ink, b.hash,
           b.hash != a.hash ? "distinct" : "COLLIDED");
    if (b.hash == a.hash) { printf("  FAIL: different text returned cached image\n"); ++bad; }

    // 3) 不同颜色 -> 必须不同
    Shot c = Render(hDC, pBits, &pm, W, H, _T("Alpha"), 0xFFD02020, -1, DT_LEFT | DT_SINGLELINE);
    printf("[probe] different colour        ink=%zu hash=%zx  %s\n", c.ink, c.hash,
           c.hash != a.hash ? "distinct" : "COLLIDED");
    if (c.hash == a.hash) { printf("  FAIL: colour not part of key\n"); ++bad; }

    // 4) 不同对齐 -> 必须不同
    Shot d = Render(hDC, pBits, &pm, W, H, _T("Alpha"), kCol, -1, DT_RIGHT | DT_SINGLELINE);
    printf("[probe] different alignment     ink=%zu hash=%zx  %s\n", d.ink, d.hash,
           d.hash != a.hash ? "distinct" : "COLLIDED");
    if (d.hash == a.hash) { printf("  FAIL: style not part of key\n"); ++bad; }

    // 5) 回到基线参数 -> 必须与最初一致（缓存未被污染）
    Shot e = Render(hDC, pBits, &pm, W, H, _T("Alpha"), kCol, -1, DT_LEFT | DT_SINGLELINE);
    printf("[probe] back to baseline        ink=%zu hash=%zx  %s\n", e.ink, e.hash,
           e.hash == a.hash ? "SAME" : "DIFF");
    if (e.hash != a.hash) { printf("  FAIL: cache corrupted across entries\n"); ++bad; }

    ::SelectObject(hDC, hOld);
    ::DeleteObject(hBmp);
    ::DeleteDC(hDC);
    ::ReleaseDC(nullptr, hScreen);

    printf("\n[D2D-CACHE] %s (%d failures)\n", bad == 0 ? "ALL GREEN" : "FAIL", bad);
    return bad;
}
