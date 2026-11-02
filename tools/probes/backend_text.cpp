// R258 验证：DrawText 后端分发
//
// 只编译通过不算完成。验证：
//   1) 默认无后端 -> 走原文字路径，像素与改造前一致
//   2) 安装 GDI 后端 -> 输出与默认路径像素完全相同
//   3) 安装 Spy 后端 -> DrawText 被调用，实参原样透传
//   4) 卸载 -> 恢复默认路径
//   5) 防递归 -> 后端内部回调 CRenderEngine::DrawText 不会无限递归
#include "StdAfx.h"
#include "UIlib.h"
#include <cstdio>
#include "Core/UIRender.h"
#include "Core/UIManager.h"

using DuiLib::IRenderBackend;
using DuiLib::CGdiRenderBackend;
using DuiLib::CRenderEngine;
using DuiLib::CPaintManagerUI;

static int g_bad = 0;
#define CHECK(cond, msg)                                        \
    do {                                                        \
        if (cond) printf("  ok   %s\n", msg);                   \
        else { printf("  FAIL %s\n", msg); ++g_bad; }           \
        fflush(stdout);                                         \
    } while (0)

class SpyTextBackend : public IRenderBackend
{
public:
    int nText = 0;
    int lastFont = -1;
    UINT lastStyle = 0;
    DWORD lastColor = 0;
    const TCHAR* lastText = nullptr;

    const TCHAR* Name() const override { return _T("SpyText"); }
    void DrawColor(HDC, const RECT&, DWORD) override {}
    void DrawGradient(HDC, const RECT&, DWORD, DWORD, bool, int) override {}
    void DrawLine(HDC, const RECT&, int, DWORD, int) override {}
    void DrawRect(HDC, const RECT&, int, DWORD, int) override {}
    void DrawRoundRect(HDC, const RECT&, int, int, int, DWORD, int) override {}
    void DrawImage(HDC, HBITMAP, const RECT&, const RECT&, const RECT&,
                   const RECT&, bool, UINT, bool, bool, bool) override {}

    void DrawText(HDC, CPaintManagerUI*, RECT&, LPCTSTR pstrText,
                  DWORD dwTextColor, int iFont, UINT uStyle) override
    {
        ++nText;
        lastText = pstrText;
        lastColor = dwTextColor;
        lastFont = iFont;
        lastStyle = uStyle;
    }
};

class RecurseTextBackend : public IRenderBackend
{
public:
    int nCalls = 0;
    const TCHAR* Name() const override { return _T("RecurseText"); }
    void DrawColor(HDC, const RECT&, DWORD) override {}
    void DrawGradient(HDC, const RECT&, DWORD, DWORD, bool, int) override {}
    void DrawLine(HDC, const RECT&, int, DWORD, int) override {}
    void DrawRect(HDC, const RECT&, int, DWORD, int) override {}
    void DrawRoundRect(HDC, const RECT&, int, int, int, DWORD, int) override {}
    void DrawImage(HDC, HBITMAP, const RECT&, const RECT&, const RECT&,
                   const RECT&, bool, UINT, bool, bool, bool) override {}

    void DrawText(HDC hDC, CPaintManagerUI* pManager, RECT& rc, LPCTSTR pstrText,
                  DWORD dwTextColor, int iFont, UINT uStyle) override
    {
        ++nCalls;
        // 若防递归失效，这里会无限递归
        CRenderEngine::DrawText(hDC, pManager, rc, pstrText, dwTextColor, iFont, uStyle);
    }
};

static size_t HashBits(BYTE* p, int n)
{
    size_t h = 1469598103934665603ull;
    for (int i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

int main()
{
    const int W = 320, H = 60;

    // CPaintManagerUI needs a real window: Init(nullptr) leaves m_hDcPaint null and
    // GetDefaultFontInfo() then cannot measure the font, so DrawText silently draws
    // nothing (this made the first version of this probe "fail" for the wrong reason).
    HINSTANCE hInst = ::GetModuleHandle(nullptr);
    WNDCLASSEX wc = { 0 };
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ::DefWindowProc;
    wc.hInstance = hInst;
    wc.lpszClassName = _T("DuiLibBackendTextProbe");
    ::RegisterClassEx(&wc);
    HWND hWnd = ::CreateWindowEx(0, wc.lpszClassName, _T("probe"), WS_OVERLAPPEDWINDOW,
                                 0, 0, 320, 240, nullptr, nullptr, hInst, nullptr);
    if (hWnd == nullptr) { printf("[probe] CreateWindow failed\n"); return 2; }

    CPaintManagerUI pm;
    pm.Init(hWnd);
    pm.SetDefaultFont(_T("Segoe UI"), 16, false, false, false, false, false);

    HDC hScreen = ::GetDC(nullptr);
    HDC hDC = ::CreateCompatibleDC(hScreen);
    BYTE* pBits = nullptr;
    HBITMAP hBmp = CRenderEngine::CreateARGB32Bitmap(hDC, W, H, &pBits);
    HBITMAP hOld = (HBITMAP)::SelectObject(hDC, hBmp);
    ::SetBkMode(hDC, TRANSPARENT);

    RECT rc = { 0, 0, W, H };
    const TCHAR* kText = _T("backend text 0123");

    printf("[probe] === 1) default: no backend ===\n");
    CHECK(CRenderEngine::GetRenderBackend() == nullptr, "no backend by default");
    ::ZeroMemory(pBits, (size_t)W * H * 4);
    CRenderEngine::DrawText(hDC, &pm, rc, kText, 0xFF202020, -1, DT_LEFT | DT_SINGLELINE);
    GdiFlush();
    size_t hDefault = HashBits(pBits, W * H * 4);
    // 默认路径必须真的画了东西（防假绿）
    // GDI does not write the alpha channel when drawing text into a 32bpp DIB
    // (alpha stays 0), so counting alpha alone reports "nothing drawn". Count a
    // pixel as ink if any of RGB or A is non-zero.
    size_t ink = 0;
    for (int i = 0; i < W * H; ++i) {
        const BYTE* px = pBits + i * 4;
        if (px[0] | px[1] | px[2] | px[3]) ++ink;
    }
    printf("        default path ink pixels = %zu\n", ink);
    CHECK(ink > 50, "default text path actually drew something");

    printf("\n[probe] === 2) GDI backend: pixel-identical ===\n");
    CGdiRenderBackend gdi;
    CRenderEngine::SetRenderBackend(&gdi);
    ::ZeroMemory(pBits, (size_t)W * H * 4);
    CRenderEngine::DrawText(hDC, &pm, rc, kText, 0xFF202020, -1, DT_LEFT | DT_SINGLELINE);
    GdiFlush();
    size_t hGdi = HashBits(pBits, W * H * 4);
    printf("        hash default=%zx gdiBackend=%zx\n", hDefault, hGdi);
    CHECK(hDefault == hGdi, "GDI backend text output identical");

    printf("\n[probe] === 3) spy backend: dispatch ===\n");
    SpyTextBackend spy;
    CRenderEngine::SetRenderBackend(&spy);
    CRenderEngine::DrawText(hDC, &pm, rc, kText, 0xFF112233, 2, DT_CENTER);
    printf("        calls=%d font=%d style=0x%X color=0x%08X\n",
           spy.nText, spy.lastFont, spy.lastStyle, spy.lastColor);
    CHECK(spy.nText == 1, "DrawText dispatched");
    CHECK(spy.lastFont == 2, "font index passed through");
    CHECK(spy.lastStyle == DT_CENTER, "style passed through");
    CHECK(spy.lastColor == 0xFF112233, "colour passed through");
    CHECK(spy.lastText != nullptr && _tcscmp(spy.lastText, kText) == 0,
          "text pointer passed through");

    printf("\n[probe] === 4) uninstall ===\n");
    CRenderEngine::SetRenderBackend(nullptr);
    int before = spy.nText;
    ::ZeroMemory(pBits, (size_t)W * H * 4);
    CRenderEngine::DrawText(hDC, &pm, rc, kText, 0xFF202020, -1, DT_LEFT | DT_SINGLELINE);
    GdiFlush();
    CHECK(spy.nText == before, "spy not called after uninstall");
    CHECK(HashBits(pBits, W * H * 4) == hDefault, "output back to default");

    printf("\n[probe] === 5) re-entrancy guard ===\n");
    RecurseTextBackend rec;
    CRenderEngine::SetRenderBackend(&rec);
    ::ZeroMemory(pBits, (size_t)W * H * 4);
    CRenderEngine::DrawText(hDC, &pm, rc, kText, 0xFF202020, -1, DT_LEFT | DT_SINGLELINE);
    GdiFlush();
    printf("        recurse calls=%d (expect 1)\n", rec.nCalls);
    CHECK(rec.nCalls == 1, "no infinite recursion");
    CRenderEngine::SetRenderBackend(nullptr);

    ::SelectObject(hDC, hOld);
    ::DeleteObject(hBmp);
    ::DeleteDC(hDC);
    ::ReleaseDC(nullptr, hScreen);

    printf("\n[R258] %s (%d failures)\n", g_bad == 0 ? "ALL GREEN" : "FAIL", g_bad);
    return g_bad;
}
