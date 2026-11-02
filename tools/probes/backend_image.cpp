// R251 验证：DrawImage 后端分发
//
// 只编译通过不算完成。本探针验证：
//   1) 默认无后端 -> 走原 GDI 路径，像素与"未改造前"一致
//   2) 安装 GDI 后端 -> 输出与默认路径像素完全相同
//   3) 安装 Spy 后端 -> DrawImage 确实被调用，实参原样透传
//   4) 卸载 -> 恢复默认路径
//   5) 防递归 -> 后端内部回调 CRenderEngine::DrawImage 不会无限递归
#include "StdAfx.h"
#include "UIlib.h"
#include <cstdio>
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

class SpyBackend : public IRenderBackend
{
public:
    int nImage = 0, nColor = 0;
    RECT lastRc = { 0, 0, 0, 0 };
    RECT lastPaint = { 0, 0, 0, 0 };
    bool lastAlpha = false;
    UINT lastFade = 0;

    const TCHAR* Name() const override { return _T("Spy"); }
    void DrawColor(HDC, const RECT&, DWORD) override { ++nColor; }
    void DrawGradient(HDC, const RECT&, DWORD, DWORD, bool, int) override {}
    void DrawLine(HDC, const RECT&, int, DWORD, int) override {}
    void DrawRect(HDC, const RECT&, int, DWORD, int) override {}
    void DrawRoundRect(HDC, const RECT&, int, int, int, DWORD, int) override {}

    void DrawImage(HDC, HBITMAP, const RECT& rc, const RECT& rcPaint,
                   const RECT&, const RECT&, bool bAlpha, UINT uFade,
                   bool, bool, bool) override
    {
        ++nImage;
        lastRc = rc;
        lastPaint = rcPaint;
        lastAlpha = bAlpha;
        lastFade = uFade;
    }
};

class RecurseBackend : public IRenderBackend
{
public:
    int nCalls = 0;
    HBITMAP hbm = nullptr;
    const TCHAR* Name() const override { return _T("Recurse"); }
    void DrawColor(HDC, const RECT&, DWORD) override {}
    void DrawGradient(HDC, const RECT&, DWORD, DWORD, bool, int) override {}
    void DrawLine(HDC, const RECT&, int, DWORD, int) override {}
    void DrawRect(HDC, const RECT&, int, DWORD, int) override {}
    void DrawRoundRect(HDC, const RECT&, int, int, int, DWORD, int) override {}
    void DrawImage(HDC hDC, HBITMAP hBitmap, const RECT& rc, const RECT& rcPaint,
                   const RECT& rcBmpPart, const RECT& rcCorners, bool bAlpha,
                   UINT uFade, bool hole, bool xtiled, bool ytiled) override
    {
        ++nCalls;
        // 若防递归失效，这里会无限递归直到栈溢出
        CRenderEngine::DrawImage(hDC, hBitmap, rc, rcPaint, rcBmpPart, rcCorners,
                                 bAlpha, uFade, hole, xtiled, ytiled);
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
    const int W = 96, H = 96;
    HDC hScreen = ::GetDC(nullptr);

    // 源图：一个不透明红块
    HDC hSrc = ::CreateCompatibleDC(hScreen);
    BYTE* pSrcBits = nullptr;
    HBITMAP hSrcBmp = CRenderEngine::CreateARGB32Bitmap(hSrc, 32, 32, &pSrcBits);
    ::SelectObject(hSrc, hSrcBmp);
    for (int i = 0; i < 32 * 32; ++i) {
        pSrcBits[i * 4 + 0] = 0x00;
        pSrcBits[i * 4 + 1] = 0x00;
        pSrcBits[i * 4 + 2] = 0xFF;   // BGR: 红
        pSrcBits[i * 4 + 3] = 0xFF;
    }

    // 目标
    HDC hDC = ::CreateCompatibleDC(hScreen);
    BYTE* pBits = nullptr;
    HBITMAP hBmp = CRenderEngine::CreateARGB32Bitmap(hDC, W, H, &pBits);
    HBITMAP hOld = (HBITMAP)::SelectObject(hDC, hBmp);

    RECT rc = { 8, 8, 72, 72 };
    RECT rcPaint = { 0, 0, W, H };
    RECT rcSrc = { 0, 0, 32, 32 };
    RECT rcCorner = { 0, 0, 0, 0 };

    printf("[probe] === 1) default: no backend ===\n");
    CHECK(CRenderEngine::GetRenderBackend() == nullptr, "no backend by default");
    ::ZeroMemory(pBits, (size_t)W * H * 4);
    CRenderEngine::DrawImage(hDC, hSrcBmp, rc, rcPaint, rcSrc, rcCorner,
                             true, 255, false, false, false);
    GdiFlush();
    size_t hDefault = HashBits(pBits, W * H * 4);

    printf("\n[probe] === 2) GDI backend: pixel-identical ===\n");
    CGdiRenderBackend gdi;
    CRenderEngine::SetRenderBackend(&gdi);
    ::ZeroMemory(pBits, (size_t)W * H * 4);
    CRenderEngine::DrawImage(hDC, hSrcBmp, rc, rcPaint, rcSrc, rcCorner,
                             true, 255, false, false, false);
    GdiFlush();
    size_t hGdi = HashBits(pBits, W * H * 4);
    printf("        hash default=%zx gdiBackend=%zx\n", hDefault, hGdi);
    CHECK(hDefault == hGdi, "GDI backend output identical");

    printf("\n[probe] === 3) spy backend: dispatch ===\n");
    SpyBackend spy;
    CRenderEngine::SetRenderBackend(&spy);
    CRenderEngine::DrawImage(hDC, hSrcBmp, rc, rcPaint, rcSrc, rcCorner,
                             true, 200, false, false, false);
    printf("        DrawImage calls=%d alpha=%d fade=%u\n",
           spy.nImage, (int)spy.lastAlpha, spy.lastFade);
    CHECK(spy.nImage == 1, "DrawImage dispatched");
    CHECK(spy.lastRc.left == rc.left && spy.lastRc.right == rc.right,
          "dest rect passed through");
    CHECK(spy.lastPaint.right == rcPaint.right, "paint rect passed through");
    CHECK(spy.lastAlpha && spy.lastFade == 200, "alpha/fade passed through");

    printf("\n[probe] === 4) uninstall ===\n");
    CRenderEngine::SetRenderBackend(nullptr);
    int before = spy.nImage;
    ::ZeroMemory(pBits, (size_t)W * H * 4);
    CRenderEngine::DrawImage(hDC, hSrcBmp, rc, rcPaint, rcSrc, rcCorner,
                             true, 255, false, false, false);
    GdiFlush();
    CHECK(spy.nImage == before, "spy not called after uninstall");
    CHECK(HashBits(pBits, W * H * 4) == hDefault, "output back to default");

    printf("\n[probe] === 5) re-entrancy guard ===\n");
    RecurseBackend rec;
    CRenderEngine::SetRenderBackend(&rec);
    ::ZeroMemory(pBits, (size_t)W * H * 4);
    CRenderEngine::DrawImage(hDC, hSrcBmp, rc, rcPaint, rcSrc, rcCorner,
                             true, 255, false, false, false);
    GdiFlush();
    printf("        recurse calls=%d (expect 1)\n", rec.nCalls);
    CHECK(rec.nCalls == 1, "no infinite recursion");
    CRenderEngine::SetRenderBackend(nullptr);

    ::SelectObject(hDC, hOld);
    ::DeleteObject(hBmp);
    ::DeleteObject(hSrcBmp);
    ::DeleteDC(hDC);
    ::DeleteDC(hSrc);
    ::ReleaseDC(nullptr, hScreen);

    printf("\n[R251] %s (%d failures)\n", g_bad == 0 ? "ALL GREEN" : "FAIL", g_bad);
    return g_bad;
}
