// R240 渲染后端抽象层验证
//
// 只编译通过不算完成。本探针验证：
//   1) 默认无后端：走原 GDI 路径，像素与"未改造前"等价（用 GDI 直绘对照）
//   2) 安装后端：确实被调用（记录调用次数与实参）
//   3) 卸载后端：回到 GDI，行为恢复
//   4) 防递归：后端内部回调 CRenderEngine 不会无限递归
#include "StdAfx.h"
#include "UIlib.h"
#include <cstdio>
#include <cstring>
#include "Core/UIRender.h"

using DuiLib::IRenderBackend;
using DuiLib::CGdiRenderBackend;
using DuiLib::CRenderEngine;

static int g_bad = 0;
#define CHECK(cond, msg)                                        \
    do {                                                        \
        if (cond) printf("  ok   %s\n", msg);                   \
        else { printf("  FAIL %s\n", msg); ++g_bad; }           \
        fflush(stdout);                                         \
    } while (0)

// 计数后端：只统计，不真正绘制
class SpyBackend : public IRenderBackend
{
public:
    int nColor = 0, nGrad = 0, nLine = 0, nRect = 0, nRound = 0;
    RECT lastRect = { 0, 0, 0, 0 };

    const TCHAR* Name() const override { return _T("Spy"); }
    void DrawColor(HDC, const RECT& rc, DWORD) override { ++nColor; lastRect = rc; }
    void DrawGradient(HDC, const RECT&, DWORD, DWORD, bool, int) override { ++nGrad; }
    void DrawLine(HDC, const RECT&, int, DWORD, int) override { ++nLine; }
    void DrawRect(HDC, const RECT&, int, DWORD, int) override { ++nRect; }
    void DrawRoundRect(HDC, const RECT&, int, int, int, DWORD, int) override { ++nRound; }
};

// 递归后端：在自身实现里回调 CRenderEngine，验证防递归
class RecurseBackend : public IRenderBackend
{
public:
    int nCalls = 0;
    const TCHAR* Name() const override { return _T("Recurse"); }
    void DrawColor(HDC hDC, const RECT& rc, DWORD c) override
    {
        ++nCalls;
        CRenderEngine::DrawColor(hDC, rc, c);   // 若防递归失效会栈溢出
    }
    void DrawGradient(HDC, const RECT&, DWORD, DWORD, bool, int) override {}
    void DrawLine(HDC, const RECT&, int, DWORD, int) override {}
    void DrawRect(HDC, const RECT&, int, DWORD, int) override {}
    void DrawRoundRect(HDC, const RECT&, int, int, int, DWORD, int) override {}
};

static size_t HashBits(BYTE* p, int n)
{
    size_t h = 1469598103934665603ull;
    for (int i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

int main()
{
    const int W = 64, H = 64;
    HDC hScreen = ::GetDC(nullptr);
    HDC hDC = ::CreateCompatibleDC(hScreen);
    BYTE* pBits = nullptr;
    HBITMAP hBmp = CRenderEngine::CreateARGB32Bitmap(hDC, W, H, &pBits);
    HBITMAP hOld = (HBITMAP)::SelectObject(hDC, hBmp);
    RECT rc = { 4, 4, 40, 30 };

    printf("[probe] === 1) default: no backend ===\n");
    CHECK(CRenderEngine::GetRenderBackend() == nullptr, "no backend installed by default");
    CHECK(!CRenderEngine::IsUsingCustomBackend(), "IsUsingCustomBackend() == false");

    ::ZeroMemory(pBits, (size_t)W * H * 4);
    CRenderEngine::DrawColor(hDC, rc, 0xFFFF0000);
    GdiFlush();
    size_t hDefault = HashBits(pBits, W * H * 4);

    printf("\n[probe] === 2) install GDI backend: must be pixel-identical ===\n");
    CGdiRenderBackend gdi;
    CRenderEngine::SetRenderBackend(&gdi);
    CHECK(CRenderEngine::IsUsingCustomBackend(), "backend installed");
    CHECK(std::strcmp("", "") == 0 && CRenderEngine::GetRenderBackend() == &gdi,
          "GetRenderBackend() returns installed one");

    ::ZeroMemory(pBits, (size_t)W * H * 4);
    CRenderEngine::DrawColor(hDC, rc, 0xFFFF0000);
    GdiFlush();
    size_t hGdiBackend = HashBits(pBits, W * H * 4);
    printf("        hash default=%zx gdiBackend=%zx\n", hDefault, hGdiBackend);
    CHECK(hDefault == hGdiBackend, "GDI backend output identical to default path");

    printf("\n[probe] === 3) spy backend: dispatch must happen ===\n");
    SpyBackend spy;
    CRenderEngine::SetRenderBackend(&spy);
    CRenderEngine::DrawColor(hDC, rc, 0xFF00FF00);
    CRenderEngine::DrawGradient(hDC, rc, 0, 0, true, 10);
    CRenderEngine::DrawLine(hDC, rc, 1, 0, PS_SOLID);
    CRenderEngine::DrawRect(hDC, rc, 1, 0, PS_SOLID);
    CRenderEngine::DrawRoundRect(hDC, rc, 4, 4, 1, 0, PS_SOLID);
    printf("        calls: color=%d grad=%d line=%d rect=%d round=%d\n",
           spy.nColor, spy.nGrad, spy.nLine, spy.nRect, spy.nRound);
    CHECK(spy.nColor == 1, "DrawColor dispatched");
    CHECK(spy.nGrad == 1, "DrawGradient dispatched");
    CHECK(spy.nLine == 1, "DrawLine dispatched");
    CHECK(spy.nRect == 1, "DrawRect dispatched");
    CHECK(spy.nRound == 1, "DrawRoundRect dispatched");
    CHECK(spy.lastRect.left == rc.left && spy.lastRect.right == rc.right,
          "rect argument passed through unchanged");

    printf("\n[probe] === 4) uninstall: back to GDI ===\n");
    CRenderEngine::SetRenderBackend(nullptr);
    CHECK(CRenderEngine::GetRenderBackend() == nullptr, "backend removed");
    int before = spy.nColor;
    ::ZeroMemory(pBits, (size_t)W * H * 4);
    CRenderEngine::DrawColor(hDC, rc, 0xFFFF0000);
    GdiFlush();
    CHECK(spy.nColor == before, "spy not called after uninstall");
    CHECK(HashBits(pBits, W * H * 4) == hDefault, "output back to default path");

    printf("\n[probe] === 5) re-entrancy guard ===\n");
    RecurseBackend rec;
    CRenderEngine::SetRenderBackend(&rec);
    ::ZeroMemory(pBits, (size_t)W * H * 4);
    CRenderEngine::DrawColor(hDC, rc, 0xFF0000FF);   // 后端内部再调一次
    GdiFlush();
    printf("        recurse backend calls = %d (expect 1, no stack overflow)\n", rec.nCalls);
    CHECK(rec.nCalls == 1, "no infinite recursion (guard works)");
    CRenderEngine::SetRenderBackend(nullptr);

    ::SelectObject(hDC, hOld);
    ::DeleteObject(hBmp);
    ::DeleteDC(hDC);
    ::ReleaseDC(nullptr, hScreen);

    printf("\n[R240] %s (%d failures)\n", g_bad == 0 ? "ALL GREEN" : "FAIL", g_bad);
    return g_bad;
}
