// ============================================================
// perf_fill_equiv.cpp -- is a GDI FillRect byte-identical to
// CRenderEngine::DrawColor on the surfaces DuiLib actually paints on?
//
// Why this probe exists:
//   DrawColor builds a GDI+ Graphics + SolidBrush on every call (measured
//   10.8us vs 3.4us for FillRect). Replacing the opaque case looks like a
//   free 3x. It is only free if the resulting bytes are the same, INCLUDING
//   the alpha byte, because the layered-window path feeds this buffer
//   straight to UpdateLayeredWindow with ULW_ALPHA and reads the alpha.
//
//   A 32bpp DIB filled by GDI FillRect leaves the alpha byte at whatever the
//   brush/ROP produces - typically 0. That would turn every opaque fill
//   transparent in the layered path: a catastrophic visual regression that no
//   functional test would catch. This probe checks the bytes on two surfaces:
//
//     S1 32bpp DIB (what the layered path paints into)
//     S2   compatible bitmap (what the plain BitBlt path paints into)
//
// Exit code 0 only when the recommendation is actually safe.
// ============================================================

#include "UIlib.h"

#include <windows.h>
#include <gdiplus.h>
#include <cstdio>
#include <cstring>

using namespace DuiLib;

namespace {

int g_nPass = 0;
int g_nFail = 0;

void Chk(const char* pszName, bool bOk, const char* pszDetail)
{
    ::printf("[%s] %s | %s\n", bOk ? "PASS" : "FAIL", pszName, pszDetail);
    ::fflush(stdout);
    if (bOk) ++g_nPass; else ++g_nFail;
}

struct CSurface
{
    HDC     hDC;
    HBITMAP hBmp;
    HBITMAP hOld;
    BYTE*   pBits;   // null for a compatible bitmap (not directly addressable)
    int     cx, cy;
};

CSurface MakeDib(int cx, int cy)
{
    CSurface s = {};
    s.cx = cx; s.cy = cy;
    HDC hScreen = ::GetDC(nullptr);
    s.hDC = ::CreateCompatibleDC(hScreen);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = cx;
    bi.bmiHeader.biHeight = -cy;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* pv = nullptr;
    s.hBmp = ::CreateDIBSection(s.hDC, &bi, DIB_RGB_COLORS, &pv, nullptr, 0);
    s.pBits = static_cast<BYTE*>(pv);
    s.hOld = static_cast<HBITMAP>(::SelectObject(s.hDC, s.hBmp));
    ::ReleaseDC(nullptr, hScreen);
    return s;
}

void FreeSurface(CSurface& s)
{
    if (s.hDC) { ::SelectObject(s.hDC, s.hOld); }
    if (s.hBmp) ::DeleteObject(s.hBmp);
    if (s.hDC) ::DeleteDC(s.hDC);
}

// Count pixels whose alpha is NOT 0xFF after filling with an opaque colour.
// This is the single number that decides whether FillRect may be used.
int CountNonOpaque(BYTE* p, int nPix)
{
    int n = 0;
    for (int i = 0; i < nPix; ++i)
        if (p[i * 4 + 3] != 0xFF) ++n;
    return n;
}

} // namespace

int main()
{
    Gdiplus::GdiplusStartupInput in;
    ULONG_PTR tok = 0;
    Gdiplus::GdiplusStartup(&tok, &in, nullptr);

    const int W = 200, H = 100;
    const int nPix = W * H;
    RECT rc = { 0, 0, W, H };
    const DWORD kOpaque = 0xFF2D6CB5u;   // skin-style opaque colour

    // ---------- S1: 32bpp DIB (layered path) ----------
    {
        CSurface a = MakeDib(W, H);
        CSurface b = MakeDib(W, H);

        // (a) current implementation
        ::ZeroMemory(a.pBits, (size_t)nPix * 4);
        CRenderEngine::DrawColor(a.hDC, rc, kOpaque);
        GdiFlush();

        // (b) proposed replacement
        ::ZeroMemory(b.pBits, (size_t)nPix * 4);
        HBRUSH hBr = ::CreateSolidBrush(RGB(0x2D, 0x6C, 0xB5));
        ::FillRect(b.hDC, &rc, hBr);
        ::DeleteObject(hBr);
        GdiFlush();

        const bool sameRgb   = ::memcmp(a.pBits, b.pBits, (size_t)nPix * 4) == 0;
        const int  alphaCur  = CountNonOpaque(a.pBits, nPix);
        const int  alphaNew  = CountNonOpaque(b.pBits, nPix);

        char sz[256];
        ::sprintf_s(sz, "GDI+ alpha!=FF:%d  FillRect alpha!=FF:%d", alphaCur, alphaNew);
        Chk("S1 GDI+ DrawColor keeps alpha opaque on a 32bpp DIB", alphaCur == 0, sz);

        ::sprintf_s(sz, "alpha!=FF:%d of %d (all 4 bytes %s)",
                    alphaNew, nPix, sameRgb ? "identical" : "DIFFER");
        // This is the decisive check, and it is phrased as a GUARD rather than as
        // a hoped-for result.
        //
        // The question this probe answers is "may DrawColor be replaced by FillRect
        // unconditionally?". Measured answer: NO -- FillRect clears the alpha byte
        // on every pixel of a 32bpp DIB, which is exactly the surface the layered
        // window hands to UpdateLayeredWindow(ULW_ALPHA). Asserting "they are
        // identical" would make this probe fail forever and the suite red, which
        // teaches reviewers to ignore it.
        //
        // So the assertion encodes the measured fact that makes the optimisation
        // conditional. It goes RED if the fact ever changes (e.g. a Windows update
        // starts preserving alpha), which is precisely when the unconditional fast
        // path WOULD become safe and this decision should be revisited.
        Chk("S1 unconditional FillRect is unsafe on the layered surface (alpha lost)",
            alphaNew == nPix,
            sz);

        FreeSurface(a);
        FreeSurface(b);
    }

    // ---------- S2: compatible bitmap (plain BitBlt path) ----------
    {
        HDC hScreen = ::GetDC(nullptr);
        HDC hA = ::CreateCompatibleDC(hScreen);
        HBITMAP bA = ::CreateCompatibleBitmap(hScreen, W, H);
        HBITMAP oA = static_cast<HBITMAP>(::SelectObject(hA, bA));
        HDC hB = ::CreateCompatibleDC(hScreen);
        HBITMAP bB = ::CreateCompatibleBitmap(hScreen, W, H);
        HBITMAP oB = static_cast<HBITMAP>(::SelectObject(hB, bB));

        ::PatBlt(hA, 0, 0, W, H, WHITENESS);
        ::PatBlt(hB, 0, 0, W, H, WHITENESS);

        CRenderEngine::DrawColor(hA, rc, kOpaque);
        HBRUSH hBr = ::CreateSolidBrush(RGB(0x2D, 0x6C, 0xB5));
        ::FillRect(hB, &rc, hBr);
        ::DeleteObject(hBr);
        GdiFlush();

        // Compare through a DIB so the bytes can be inspected.
        auto grab = [&](HDC src) {
            CSurface d = MakeDib(W, H);
            ::BitBlt(d.hDC, 0, 0, W, H, src, 0, 0, SRCCOPY);
            GdiFlush();
            return d;
        };
        CSurface ga = grab(hA);
        CSurface gb = grab(hB);
        const bool same = ::memcmp(ga.pBits, gb.pBits, (size_t)nPix * 4) == 0;

        Chk("S2 non-layered surface: FillRect is byte-identical (conditional path OK)",
            same, same ? "identical bytes" : "DIFFER");

        FreeSurface(ga);
        FreeSurface(gb);
        ::SelectObject(hA, oA); ::DeleteObject(bA); ::DeleteDC(hA);
        ::SelectObject(hB, oB); ::DeleteObject(bB); ::DeleteDC(hB);
        ::ReleaseDC(nullptr, hScreen);
    }

    ::printf("SUMMARY passed=%d failed=%d\n", g_nPass, g_nFail);
    Gdiplus::GdiplusShutdown(tok);
    return g_nFail == 0 ? 0 : 1;
}
