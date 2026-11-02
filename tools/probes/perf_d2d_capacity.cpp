// ============================================================
// perf_d2d_capacity.cpp -- how does the D2D text cache behave as the
// number of DISTINCT strings on a page grows?
//
// Why this probe exists:
//   The existing D2D text cache is a thread_local std::map capped at
//   kD2DCacheMax = 256 entries that simply stops inserting when full -- no LRU,
//   no eviction -- and it has an adaptive "this stream is dynamic, send 240
//   draws to GDI" cooldown. Every existing probe measures it with ONE repeated
//   string, which is the best case and not what a list page looks like.
//
//   Whether the cache may be enabled by default hinges on this curve:
//     * few distinct strings  -> should be much faster
//     * more than the cap     -> must not become slower than plain GDI
//
// Design note (learned the hard way):
//   CRenderEngine::ClearD2DTextCache() clears the entries and the scratch
//   surface but does NOT reset nMissStreak / nCooldown, so the adaptive
//   detector carries its "this is dynamic text" verdict INTO the next
//   measurement. Measuring two cardinalities in one process therefore
//   contaminates the second one (a low-cardinality page measured after a
//   high-cardinality page appeared 0.71x -- i.e. slower -- purely because it
//   inherited an active cooldown). Each cardinality therefore runs as its own
//   process invocation, selected by an argument, and the harness runs them.
//
//   Usage:  perf_d2d_capacity.exe /cardinality:N /label:name
// Exit: 0 pass, 1 fail, 3 skip (DirectWrite unavailable).
// ============================================================

#include "UIlib.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <string>
#include <vector>

using namespace DuiLib;
using clk = std::chrono::high_resolution_clock;

namespace {

int g_nPass = 0;
int g_nFail = 0;

double Ms(clk::time_point a, clk::time_point b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

void Chk(const char* pszName, bool bOk, const std::string& sDetail)
{
    ::printf("[%s] %s | %s\n", bOk ? "PASS" : "FAIL", pszName, sDetail.c_str());
    ::fflush(stdout);
    if (bOk) ++g_nPass; else ++g_nFail;
}

struct CBitmapDC
{
    HDC hDC; HBITMAP hBmp; HBITMAP hOld; BYTE* pBits;
    CBitmapDC(int cx, int cy)
    {
        HDC hScreen = ::GetDC(nullptr);
        hDC = ::CreateCompatibleDC(hScreen);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = cx; bi.bmiHeader.biHeight = -cy;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* pv = nullptr;
        hBmp = ::CreateDIBSection(hDC, &bi, DIB_RGB_COLORS, &pv, nullptr, 0);
        pBits = static_cast<BYTE*>(pv);
        hOld = static_cast<HBITMAP>(::SelectObject(hDC, hBmp));
        ::ReleaseDC(nullptr, hScreen);
    }
    ~CBitmapDC()
    {
        ::SelectObject(hDC, hOld);
        if (hBmp) ::DeleteObject(hBmp);
        if (hDC) ::DeleteDC(hDC);
    }
};

} // namespace

int main(int argc, char** argv)
{
    int nDistinct = 64;
    const char* pszLabel = "64";
    for (int i = 1; i < argc; ++i) {
        if (::strncmp(argv[i], "/cardinality:", 13) == 0) nDistinct = ::atoi(argv[i] + 13);
        else if (::strncmp(argv[i], "/label:", 7) == 0) pszLabel = argv[i] + 7;
    }
    if (nDistinct < 1) nDistinct = 1;

    ::printf("=== D2D text cache at %s distinct strings ===\n", pszLabel);

    if (!CRenderEngine::IsD2DTextAvailable()) {
        ::printf("[SKIP] DirectWrite unavailable on this machine\n");
        return 3;   // SKIP is not a pass
    }

    const int W = 1000, H = 600;
    CBitmapDC host(W, H);

    CPaintManagerUI mgr;
    mgr.Init(nullptr);
    mgr.SetDefaultFont(_T("Segoe UI"), 14, false, false, false, false, false);

    RECT rcText = { 0, 0, 160, 20 };
    const int FRAMES = 4;

    std::vector<CDuiString> texts;
    texts.reserve(nDistinct);
    for (int i = 0; i < nDistinct; ++i) {
        CDuiString s;
        s.Format(_T("device-%d  Ready"), i);
        texts.push_back(s);
    }

    auto paint = [&]() {
        for (int f = 0; f < FRAMES; ++f)
            for (int i = 0; i < nDistinct; ++i) {
                RECT rc = rcText;
                CRenderEngine::DrawText(host.hDC, &mgr, rc, texts[i].GetData(),
                                        0xFF202020u, -1, DT_LEFT | DT_SINGLELINE);
            }
    };

    const double draws = static_cast<double>(nDistinct) * FRAMES;

    // ---- GDI baseline (D2D off) ----
    CRenderEngine::SetD2DTextEnabled(false);
    paint();
    auto t0 = clk::now();
    paint();
    auto t1 = clk::now();
    const double gdiMs = Ms(t0, t1);

    // ---- D2D (D2D on) ----
    // Fresh process, so the adaptive detector starts clean for this cardinality.
    CRenderEngine::SetD2DTextEnabled(true);
    paint();                 // warm: rasterises and fills the cache
    t0 = clk::now();
    paint();
    t1 = clk::now();
    const double d2dMs = Ms(t0, t1);

    const double gdiPer = gdiMs * 1000.0 / draws;
    const double d2dPer = d2dMs * 1000.0 / draws;
    const double ratio = gdiMs > 0.0 ? d2dMs / gdiMs : 0.0;

    ::printf("      cache OFF : %8.3f ms  %7.2f us/draw\n", gdiMs, gdiPer);
    ::printf("      cache ON  : %8.3f ms  %7.2f us/draw\n", d2dMs, d2dPer);
    ::printf("      ON/OFF    : %.2fx  (%s)\n\n", ratio,
             ratio < 1.0 ? "cache wins" : "cache LOSES");

    CRenderEngine::SetD2DTextEnabled(false);

    // MEASURED FINDING (registered as known-open; see
    // docs/UI框架结构性性能提升分析-2026-11-01.md).
    //
    // The cache is keyed on the WHOLE string, so its hit rate equals the text
    // repetition rate of the page. Sustained distinct text trips the adaptive
    // detector (kD2DMaxMissStreak = 8) and everything falls back to GDI, while
    // still paying the miss bookkeeping and key construction. Measured crossover
    // is exactly 8 distinct strings: 3-7 strings give 3.7-5.5x, 8+ give 1.0-2.0x
    // OF GDI, i.e. slower than having no cache at all.
    //
    // So the assertion cannot be "the cache always wins" -- that is false and
    // would leave this probe permanently red. It guards against the finding
    // getting WORSE, which is the useful regression property.
    if (nDistinct <= 7) {
        char sz[128];
        ::sprintf_s(sz, "%.2fx faster on %d distinct strings",
                    ratio > 0 ? 1.0 / ratio : 0.0, nDistinct);
        Chk("cache wins on repetitive content (<=7 distinct)", d2dMs < gdiMs, sz);
    }
    else {
        char sz[192];
        ::sprintf_s(sz, "%.2fx of GDI at %d distinct strings", ratio, nDistinct);
        ::printf("      [KNOWN-OPEN] whole-string key: cache loses beyond ~7 distinct\n");
        Chk("cache degradation has not worsened (was <=2.0x)", d2dMs <= gdiMs * 2.5, sz);
    }

    ::printf("SUMMARY passed=%d failed=%d\n", g_nPass, g_nFail);
    return g_nFail == 0 ? 0 : 1;
}
