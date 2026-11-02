// [D2D-LEAK] Do D2D text resources survive ClearD2DTextCache()?
//
// The defect:
//   D2DTextState holds three kinds of resource:
//     * formats  : IDWriteTextFormat*   (COM, needs Release)
//     * cache    : per-entry HDC + HBITMAP + the previously selected HGDIOBJ
//     * scratch  : hScratchDC + hScratchBM + hScratchOld
//   ClearD2DTextCache() releases only `formats`. The bitmap cache and the scratch DIB
//   are never freed, and D2DTextState has no destructor at all, so a thread that
//   renders once and exits keeps its DCs and bitmaps for the life of the process.
//
// Why a probe instead of a code review:
//   GDI object counts are not monotonic in a busy process (other renders allocate and
//   free concurrently), so "it looks like a leak" needs a measured delta across a
//   counted number of repetitions, taken after a warm-up that absorbs one-off costs.
//
// The probe:
//   1. warms up so first-touch costs are not counted,
//   2. records the GDI object count,
//   3. renders N distinct strings (each misses the cache, so each allocates an entry),
//   4. calls ClearD2DTextCache(),
//   5. re-measures: a correct cache returns to its baseline.
//
// /Inject adds a failing assertion so the wrapper can prove the probe can fail.

#include "StdAfx.h"
#include "UIlib.h"

#include <cstdio>

#include "Core/UIRender.h"
#include "Core/UIManager.h"

using namespace DuiLib;

static int g_pass = 0;
static int g_fail = 0;
static void Pass(const char* w) { printf("[PASS] %s\n", w); ++g_pass; }
static void FailInt(const char* w, long v) { printf("[FAIL] %s (measured=%ld)\n", w, v); ++g_fail; }

static long GdiCount()
{
    return static_cast<long>(::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS));
}

// Render one string through the D2D path. Distinct text per call forces a cache miss,
// which is what allocates a cache entry (and therefore a DC + bitmap).
static void RenderOnce(HDC hDC, BYTE* pBits, CPaintManagerUI* pm, int W, int H, int nIndex)
{
    ::ZeroMemory(pBits, static_cast<size_t>(W) * H * 4);
    RECT rc = { 0, 0, W, H };
    TCHAR szText[64] = { 0 };
    _stprintf_s(szText, _countof(szText), _T("leak-probe-%d"), nIndex);
    CRenderEngine::DrawTextD2D(hDC, pm, rc, szText, 0xFF303030, -1, DT_LEFT | DT_SINGLELINE);
    ::GdiFlush();
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    bool bInject = false;
    for (int i = 1; i < argc; ++i)
        if (_stricmp(argv[i], "/Inject") == 0) bInject = true;

    printf("[probe] D2D text cache teardown\n");

    if (!CRenderEngine::IsD2DTextAvailable()) {
        printf("[SKIP] D2D text backend unavailable on this machine\n");
        return 0;
    }

    const int W = 240, H = 48;
    CPaintManagerUI pm;
    pm.Init(nullptr);
    pm.SetDefaultFont(_T("Segoe UI"), 16, false, false, false, false, false);

    HDC hScreen = ::GetDC(nullptr);
    HDC hDC = ::CreateCompatibleDC(hScreen);
    BYTE* pBits = nullptr;
    HBITMAP hBmp = CRenderEngine::CreateARGB32Bitmap(hDC, W, H, &pBits);
    if (hBmp == nullptr) {
        printf("[SKIP] could not create the ARGB bitmap\n");
        ::DeleteDC(hDC); ::ReleaseDC(nullptr, hScreen);
        return 0;
    }
    HBITMAP hOldBmp = static_cast<HBITMAP>(::SelectObject(hDC, hBmp));

    CRenderEngine::SetD2DTextEnabled(true);

    // Warm-up: first draw pays one-off costs (DWrite format creation, scratch DIB
    // growth). Counting those as a leak is the classic false positive here.
    for (int i = 0; i < 3; ++i) RenderOnce(hDC, pBits, &pm, W, H, 1000 + i);
    CRenderEngine::ClearD2DTextCache();

    const long baseGdi = GdiCount();
    printf("[probe] baseline GDI objects = %ld\n", baseGdi);

    const int kDraws = 48;   // more than the 256-entry cache would need; each allocates
    for (int i = 0; i < kDraws; ++i) RenderOnce(hDC, pBits, &pm, W, H, i);

    const long afterDraw = GdiCount();
    printf("[probe] after %d distinct draws = %ld (+%ld)\n",
           kDraws, afterDraw, afterDraw - baseGdi);

    CRenderEngine::ClearD2DTextCache();
    const long afterClear = GdiCount();
    printf("[probe] after ClearD2DTextCache() = %ld (+%ld vs baseline)\n",
           afterClear, afterClear - baseGdi);

    // A correct teardown releases every DC and bitmap it created, so the count returns
    // to (or very near) the baseline. GDI caches a few objects internally, so a small
    // slack is allowed; a per-entry leak over 48 draws is far larger than that.
    // Slack must be tiny: a correct teardown releases everything it created, and GDI's
    // internal caching accounts for at most one or two objects. An earlier slack of 8
    // silently absorbed a real +8 leak and made this probe report PASS on the defect it
    // was written to catch.
    const long kSlack = 2;
    if (afterClear - baseGdi <= kSlack)
        Pass("ClearD2DTextCache releases the bitmap cache and scratch DIB");
    else
        FailInt("GDI objects still held after ClearD2DTextCache", afterClear - baseGdi);

    // Second round: if clearing works, repeating the cycle must not accumulate.
    for (int i = 0; i < kDraws; ++i) RenderOnce(hDC, pBits, &pm, W, H, 2000 + i);
    CRenderEngine::ClearD2DTextCache();
    const long afterRound2 = GdiCount();
    printf("[probe] after a second cycle = %ld (+%ld vs baseline)\n",
           afterRound2, afterRound2 - baseGdi);
    if (afterRound2 - baseGdi <= kSlack)
        Pass("a repeated draw/clear cycle does not accumulate");
    else
        FailInt("GDI objects accumulate across cycles", afterRound2 - baseGdi);

    ::SelectObject(hDC, hOldBmp);
    ::DeleteObject(hBmp);
    ::DeleteDC(hDC);
    ::ReleaseDC(nullptr, hScreen);

    if (bInject) FailInt("injected: deliberately failing assertion", 0);

    printf("\n%d pass / %d fail\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
