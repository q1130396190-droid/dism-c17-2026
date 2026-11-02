// ============================================================
// perf_struct_bench.cpp -- quantify the STRUCTURAL costs in DuiLib's
// paint path, as opposed to the per-frame algorithmic costs that
// perf_ui_path.cpp already covers.
//
// Why this probe exists:
//   The audit identified that several primitives construct and destroy a
//   GDI+/GDI object on EVERY call, and that every container regenerates a
//   clip region on EVERY paint. Those are "constant overhead per draw call"
//   problems: they scale with control count, not with area, so they are
//   invisible in a full-screen blit benchmark and brutal on dense pages
//   (a 2000-row list repaints 2000 cell fills).
//
//   The question this probe answers is not "how fast is GDI" but "how much
//   of the current per-call cost is object churn we could remove without
//   changing any visual result". Every case therefore measures the CURRENT
//   implementation against a drop-in equivalent that produces the same
//   pixels on an opaque background.
//
// Cases:
//   C1  DrawColor (GDI+ Graphics+SolidBrush+FillRectangle) vs GDI FillRect
//   C2  DrawRoundRectFilled (GDI+ path+brush, anti-aliased) -- per call cost
//   C3  DrawGradient  opaque, GradientFill path               -- per call cost
//   C4  CRenderClip::GenerateClip (2x CreateRectRgn + ExtSelect + restore)
//   C5  DrawText plain short string (GDI DrawText)            -- per call cost
//   C6  DrawText with a font-icon escape                      -- per call cost
//   C7  composite: 2000 fills, current primitives vs GDI-core equivalents
//
// Exit code is 0 unless a hard ceiling is exceeded, so a regression is
// visible to the suite rather than only in the printed numbers.
// ============================================================

#include "UIlib.h"

#include <windows.h>
#include <gdiplus.h>
#include <immintrin.h>
#include <cstdio>
#include <chrono>
#include <string>
#include <vector>

using namespace DuiLib;

int g_nPaintedDummy = 0;
using clk = std::chrono::high_resolution_clock;

namespace {

int g_nPass = 0;
int g_nFail = 0;
bool g_bInject = false;

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

// Scoreboard: prints ms and us/call so a budget can be written in either unit.
void Report(const char* pszCase, double msTotal, int nIters)
{
    const double us = (nIters > 0) ? (msTotal * 1000.0 / nIters) : 0.0;
    ::printf("      %-42s %8.3f ms  %8.3f us/call  (n=%d)\n",
             pszCase, msTotal, us, nIters);
    ::fflush(stdout);
}

struct CBitmapDC
{
    HDC      hDC;
    HBITMAP  hBmp;
    HBITMAP  hOld;
    BYTE*    pBits;

    CBitmapDC(int cx, int cy)
    {
        HDC hScreen = ::GetDC(nullptr);
        hDC = ::CreateCompatibleDC(hScreen);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = cx;
        bi.bmiHeader.biHeight = -cy;      // top-down, same as DuiLib's ARGB bitmap
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* pv = nullptr;
        hBmp = ::CreateDIBSection(hDC, &bi, DIB_RGB_COLORS, &pv, nullptr, 0);
        pBits = static_cast<BYTE*>(pv);
        hOld = static_cast<HBITMAP>(::SelectObject(hDC, hBmp));
    }
    ~CBitmapDC()
    {
        ::SelectObject(hDC, hOld);
        if (hBmp) ::DeleteObject(hBmp);
        if (hDC) ::DeleteDC(hDC);
    }
};

// Wraps the GDI+ token once: constructing it per measurement would dominate
// the very number we are trying to isolate.
ULONG_PTR StartGdiplus()
{
    Gdiplus::GdiplusStartupInput in;
    ULONG_PTR tok = 0;
    Gdiplus::GdiplusStartup(&tok, &in, nullptr);
    return tok;
}

} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        if (::strcmp(argv[i], "/Inject") == 0) g_bInject = true;
    }

    ::printf("=== DuiLib structural paint cost bench ===\n");
    ::printf("(per-call overhead of the primitives every control uses)\n\n");

    ULONG_PTR gdipToken = StartGdiplus();

    const int W = 1600, H = 900;
    CBitmapDC dc(W, H);
    if (!dc.hDC || !dc.hBmp) {
        ::printf("FATAL: could not create offscreen DC\n");
        return 2;
    }

    RECT rcFill = { 100, 100, 300, 200 };   // 200x100 control, a realistic cell

    // -------- C1: DrawColor, current GDI+ path vs GDI FillRect --------
    {
        const int N = 4000;
        auto t0 = clk::now();
        for (int i = 0; i < N; ++i)
            CRenderEngine::DrawColor(dc.hDC, rcFill, 0xFF2D6CB5u);
        auto t1 = clk::now();
        const double cur = Ms(t0, t1);

        HBRUSH hBr = ::CreateSolidBrush(RGB(0x2D, 0x6C, 0xB5));
        t0 = clk::now();
        for (int i = 0; i < N; ++i)
            ::FillRect(dc.hDC, &rcFill, hBr);
        t1 = clk::now();
        const double alt = Ms(t0, t1);
        ::DeleteObject(hBr);

        Report("C1 current  CRenderEngine::DrawColor", cur, N);
        Report("C1 alt      GDI FillRect (opaque)", alt, N);
        ::printf("      speedup if opaque fills used FillRect: %.1fx\n\n",
                 alt > 0.0 ? cur / alt : 0.0);

        // Opaque fills are the overwhelming majority; the GDI equivalent is
        // pixel-identical for an opaque colour on an opaque surface.
        Chk("C1 DrawColor opaque ceiling", (cur / N) < 60.0,
            "current per-call us within budget");
    }

    // -------- C2: DrawRoundRectFilled per call --------
    {
        const int N = 2000;
        auto t0 = clk::now();
        for (int i = 0; i < N; ++i)
            CRenderEngine::DrawRoundRectFilled(dc.hDC, rcFill, 6, 0xFF2D6CB5u);
        auto t1 = clk::now();
        Report("C2 current  DrawRoundRectFilled", Ms(t0, t1), N);
        ::printf("\n");
        Chk("C2 round fill ceiling", (Ms(t0, t1) / N) < 80.0,
            "per-call us within budget");
    }

    // -------- C3: DrawGradient opaque --------
    {
        const int N = 2000;
        auto t0 = clk::now();
        for (int i = 0; i < N; ++i)
            CRenderEngine::DrawGradient(dc.hDC, rcFill, 0xFF2D6CB5u, 0xFF1B4B82u, true, 16);
        auto t1 = clk::now();
        Report("C3 current  DrawGradient (opaque)", Ms(t0, t1), N);
        ::printf("\n");
        Chk("C3 gradient ceiling", (Ms(t0, t1) / N) < 80.0,
            "per-call us within budget");
    }

    // -------- C4: clip region churn per container paint --------
    {
        const int N = 20000;
        RECT rcClip = { 0, 0, W, H };
        auto t0 = clk::now();
        for (int i = 0; i < N; ++i) {
            CRenderClip clip;
            CRenderClip::GenerateClip(dc.hDC, rcClip, clip);
        }
        auto t1 = clk::now();
        const double cur = Ms(t0, t1);
        Report("C4 current  GenerateClip (2 regions)", cur, N);

        // Equivalent with one region: save/restore via SaveDC + IntersectClipRect.
        t0 = clk::now();
        for (int i = 0; i < N; ++i) {
            int saved = ::SaveDC(dc.hDC);
            ::IntersectClipRect(dc.hDC, rcClip.left, rcClip.top, rcClip.right, rcClip.bottom);
            ::RestoreDC(dc.hDC, saved);
        }
        t1 = clk::now();
        const double alt = Ms(t0, t1);
        Report("C4 alt      SaveDC+IntersectClipRect", alt, N);
        ::printf("      speedup if rect clip used IntersectClipRect: %.1fx\n\n",
                 alt > 0.0 ? cur / alt : 0.0);
        Chk("C4 clip churn ceiling", (cur / N) < 20.0,
            "per-clip us within budget");
    }

    // -------- C5 / C6: text --------
    {
        CBitmapDC host(W, H);
        // A manager is needed for font resolution; only the font cache is used.
        // Init(nullptr) is the documented detached-manager path (same as the
        // other text probes in this directory): it gives the manager a real DC
        // to measure the default font against, without a window.
        CPaintManagerUI mgr;
        mgr.Init(nullptr);
        mgr.SetDefaultFont(_T("Segoe UI"), 16, false, false, false, false, false);

        RECT rcText = { 100, 100, 400, 132 };
        const TCHAR* pszPlain = _T("Firmware update 12.4.1 ready");
        const TCHAR* pszIcon  = _T("&#xE921; Settings");

        const int N = 20000;
        auto t0 = clk::now();
        for (int i = 0; i < N; ++i) {
            RECT rc = rcText;
            CRenderEngine::DrawText(host.hDC, &mgr, rc, pszPlain, 0xFF202020u, 0, DT_LEFT | DT_SINGLELINE);
        }
        auto t1 = clk::now();
        const double plainMs = Ms(t0, t1);
        Report("C5 current  DrawText plain", plainMs, N);

        t0 = clk::now();
        for (int i = 0; i < N; ++i) {
            RECT rc = rcText;
            CRenderEngine::DrawText(host.hDC, &mgr, rc, pszIcon, 0xFF202020u, 0, DT_LEFT | DT_SINGLELINE);
        }
        t1 = clk::now();
        const double iconMs = Ms(t0, t1);
        Report("C6 current  DrawText icon escape", iconMs, N);
        ::printf("      icon-escape vs plain: %+.1f%%\n\n",
                 plainMs > 0.0 ? (iconMs - plainMs) / plainMs * 100.0 : 0.0);
        // An absolute us/call ceiling here would measure GDI, not DuiLib: C5b
        // shows raw GDI DrawText alone costs the same ~54us. What belongs to the
        // framework is the delta on top, which C5b asserts. Record, do not gate.
    }

    // -------- C5b: how much of the text cost belongs to DuiLib? --------
    // A sequential "time raw GDI, then time the framework" comparison produced a
    // NEGATIVE overhead (-1.7us), because the two runs land in different phases of
    // the machine's turbo/thermal state and the run-to-run drift is bigger than
    // the difference being measured. Interleaving the two implementations inside
    // one batch puts them under the same conditions, which is the only way this
    // comparison means anything.
    {
        CBitmapDC host(W, H);
        CPaintManagerUI mgr;
        mgr.Init(nullptr);
        mgr.SetDefaultFont(_T("Segoe UI"), 16, false, false, false, false, false);

        RECT rcText = { 100, 100, 400, 132 };
        const TCHAR* pszPlain = _T("Firmware update 12.4.1 ready");
        const int BATCH = 500;
        const int BATCHES = 20;

        HFONT hFont = mgr.GetFont(-1);
        ::SetBkMode(host.hDC, TRANSPARENT);
        ::SetTextColor(host.hDC, RGB(0x20, 0x20, 0x20));

        double raw = 0.0, full = 0.0, font = 0.0;
        for (int b = 0; b < BATCHES; ++b) {
            HFONT hOld = static_cast<HFONT>(::SelectObject(host.hDC, hFont));
            auto t0 = clk::now();
            for (int i = 0; i < BATCH; ++i) {
                RECT rc = rcText;
                ::DrawText(host.hDC, pszPlain, -1, &rc, DT_LEFT | DT_SINGLELINE);
            }
            auto t1 = clk::now();
            ::SelectObject(host.hDC, hOld);
            raw += Ms(t0, t1);

            volatile HFONT hSink = nullptr;
            t0 = clk::now();
            for (int i = 0; i < BATCH; ++i) hSink = static_cast<HFONT>(mgr.GetFont(-1));
            t1 = clk::now();
            font += Ms(t0, t1);

            t0 = clk::now();
            for (int i = 0; i < BATCH; ++i) {
                RECT rc = rcText;
                CRenderEngine::DrawText(host.hDC, &mgr, rc, pszPlain, 0xFF202020u, -1,
                                        DT_LEFT | DT_SINGLELINE);
            }
            t1 = clk::now();
            full += Ms(t0, t1);
        }

        const int N = BATCH * BATCHES;
        Report("C5b raw      GDI DrawText (font preselected)", raw, N);
        Report("C5b cost     GetFont(-1) resolution", font, N);
        Report("C5b full     CRenderEngine::DrawText", full, N);

        const double overheadMs = full - raw;
        ::printf("      DuiLib overhead per call: %+.3f us (%+.1f%% of full path)\n",
                 (overheadMs / N) * 1000.0, full > 0 ? overheadMs / full * 100.0 : 0.0);
        ::printf("      of which GetFont resolution: %.3f us\n\n", (font / N) * 1000.0);

        // The only defensible assertion: the framework must not add meaningful
        // cost on top of GDI. A generous 30% band absorbs interleaved sampling
        // noise while still catching a real regression (an extra lock, a string
        // format or a map lookup per draw would show up here).
        Chk("C5b framework adds no material text overhead",
            overheadMs < raw * 0.30,
            "DuiLib wrapper cost within 30% of raw GDI text draw");
    }

    // -------- C8: the D2D text cache, measured through the NORMAL entry --------
    // UIRender already contains a DirectWrite path with a rasterised-bitmap cache,
    // but D2DDeviceShared::bEnabled defaults to 0, so nothing enables it and every
    // text draw takes the GDI path measured in C5. The existing d2d probes time
    // DrawTextD2D directly, which answers "is the cache fast" but not "what would
    // the framework gain if it were on", because the real cost includes the
    // dispatch, the availability check and the fallbacks. This case measures the
    // public entry point both ways on the same text stream.
    {
        CBitmapDC host(W, H);
        CPaintManagerUI mgr;
        mgr.Init(nullptr);
        mgr.SetDefaultFont(_T("Segoe UI"), 16, false, false, false, false, false);

        RECT rcText = { 100, 100, 400, 132 };
        const TCHAR* pszPlain = _T("Firmware update 12.4.1 ready");
        const int N = 20000;

        CRenderEngine::SetD2DTextEnabled(false);
        auto t0 = clk::now();
        for (int i = 0; i < N; ++i) {
            RECT rc = rcText;
            CRenderEngine::DrawText(host.hDC, &mgr, rc, pszPlain, 0xFF202020u, -1, DT_LEFT | DT_SINGLELINE);
        }
        auto t1 = clk::now();
        const double gdiMs = Ms(t0, t1);
        Report("C8 GDI      DrawText via entry", gdiMs, N);

        if (CRenderEngine::IsD2DTextAvailable()) {
            CRenderEngine::SetD2DTextEnabled(true);
            // one warm draw so the glyphs are rasterised before timing
            { RECT rc = rcText; CRenderEngine::DrawText(host.hDC, &mgr, rc, pszPlain, 0xFF202020u, -1,
                                                        DT_LEFT | DT_SINGLELINE); }
            t0 = clk::now();
            for (int i = 0; i < N; ++i) {
                RECT rc = rcText;
                CRenderEngine::DrawText(host.hDC, &mgr, rc, pszPlain, 0xFF202020u, -1, DT_LEFT | DT_SINGLELINE);
            }
            t1 = clk::now();
            const double d2dMs = Ms(t0, t1);
            Report("C8 D2D      DrawText via entry", d2dMs, N);
            ::printf("      static-text speedup from enabling the existing cache: %.2fx\n\n",
                     d2dMs > 0.0 ? gdiMs / d2dMs : 0.0);

            // Dynamic stream: every string differs, so the cache can never hit.
            // The adaptive cooldown must hand these back to GDI, otherwise a page
            // of clocks counters would be far slower than before.
            // Build every distinct string BEFORE timing. Format() inside the loop
            // made this case measure string formatting rather than text drawing
            // (81us/call, which is higher than plain GDI and looked like the cache
            // penalising dynamic text). That was a probe defect, not a finding.
            const int DYN = 2000;
            std::vector<CDuiString> dyn;
            dyn.reserve(DYN);
            for (int i = 0; i < DYN; ++i) {
                CDuiString s;
                s.Format(_T("counter %d"), i);
                dyn.push_back(s);
            }
            for (int w = 0; w < 64; ++w) {
                RECT rc = rcText;
                CRenderEngine::DrawText(host.hDC, &mgr, rc, dyn[w].GetData(), 0xFF202020u, -1,
                                        DT_LEFT | DT_SINGLELINE);
            }
            t0 = clk::now();
            for (int i = 0; i < DYN; ++i) {
                RECT rc = rcText;
                CRenderEngine::DrawText(host.hDC, &mgr, rc, dyn[i].GetData(), 0xFF202020u, -1,
                                        DT_LEFT | DT_SINGLELINE);
            }
            t1 = clk::now();
            const double dynMs = Ms(t0, t1);
            Report("C8 D2D      DrawText dynamic (all miss)", dynMs, DYN);
            ::printf("      dynamic vs GDI: %.2fx\n\n",
                     (gdiMs / N) > 0.0 ? (dynMs / DYN) / (gdiMs / N) : 0.0);
            Chk("C8 dynamic text not penalised", (dynMs / DYN) < (gdiMs / N) * 2.0,
                "dynamic stream stays within 2x of GDI");
            CRenderEngine::SetD2DTextEnabled(false);
        }
        else {
            Chk("C8 d2d text availability", false, "DirectWrite unavailable on this machine");
        }
    }

    // -------- C9: layered-window per-pixel composite --------
    // The layered path (transparent/rounded window, the Win11 default look) walks
    // the dirty rectangle and touches every pixel in scalar code after the whole
    // frame has been painted. This is the one place where the per-pixel work is
    // both unavoidable and trivially vectorisable, so its share of a frame decides
    // whether a SIMD change is worth anything at all.
    {
        const int W2 = 1920, H2 = 1080;
        CBitmapDC src(W2, H2);
        CBitmapDC dst(W2, H2);
        if (src.pBits && dst.pBits) {
            ::ZeroMemory(src.pBits, (size_t)W2 * H2 * 4);
            // Paint something so the loop does real work (opaque glyph-ish pixels
            // scattered, like text and icons on a background).
            for (int y = 0; y < H2; y += 3) {
                for (int x = 0; x < W2; x += 17) {
                    src.pBits[((size_t)y * W2 + x) * 4 + 0] = 0x30;
                    src.pBits[((size_t)y * W2 + x) * 4 + 1] = 0x30;
                    src.pBits[((size_t)y * W2 + x) * 4 + 2] = 0x30;
                    src.pBits[((size_t)y * W2 + x) * 4 + 3] = 0xFF;
                }
            }

            const int ROUNDS = 10;
            auto t0 = clk::now();
            for (int r = 0; r < ROUNDS; ++r) {
                // Exactly the loop from CPaintManagerUI::WM_PAINT's layered branch
                // (the "else" form, without a layered draw-string): force alpha on
                // pixels that are non-zero but have no alpha channel.
                DWORD* pBits = reinterpret_cast<DWORD*>(src.pBits);
                const int nPix = W2 * H2;
                for (int i = 0; i < nPix; ++i) {
                    DWORD v = pBits[i];
                    if ((v & 0xFF000000u) == 0 && (v & 0x00FFFFFFu) != 0)
                        pBits[i] = v | 0xFF000000u;
                }
            }
            auto t1 = clk::now();
            const double curMs = Ms(t0, t1) / ROUNDS;
            ::printf("      %-42s %8.3f ms/frame  (%dx%d)\n",
                     "C9 current   layered alpha fixup (scalar)", curMs, W2, H2);

            Chk("C9 layered fixup share", curMs < 12.0,
                "full-HD fixup under 12ms per frame");
        }
    }

    // -------- C10: is SIMD actually worth it on the layered fixup? --------
    // C9 says the scalar per-pixel pass costs ~4ms/frame at full HD. Haswell and
    // everything after (4th gen Core, 2013+) has AVX2, so a 32-byte kernel could
    // in principle cut that. This measures the real ceiling before recommending
    // anything: if the memory traffic dominates, vectorising buys little and the
    // recommendation would be wrong.
    {
        const int W2 = 1920, H2 = 1080;
        CBitmapDC buf(W2, H2);
        if (buf.pBits) {
            const int nPix = W2 * H2;
            const int ROUNDS = 20;

            // reset helper: mostly-opaque pixels with a few zero-alpha ones, which
            // is exactly the shape the framework loop has to fix up.
            auto reset = [&]() {
                DWORD* p = reinterpret_cast<DWORD*>(buf.pBits);
                for (int i = 0; i < nPix; ++i)
                    p[i] = ((i & 0xFF) == 0) ? (0x00123456u) : 0xFF203040u;
            };

            // scalar, exactly as the framework writes it
            reset();
            auto t0 = clk::now();
            for (int r = 0; r < ROUNDS; ++r) {
                DWORD* p = reinterpret_cast<DWORD*>(buf.pBits);
                for (int i = 0; i < nPix; ++i) {
                    DWORD v = p[i];
                    if ((v & 0xFF000000u) == 0 && (v & 0x00FFFFFFu) != 0)
                        p[i] = v | 0xFF000000u;
                }
            }
            auto t1 = clk::now();
            const double scalarMs = Ms(t0, t1) / ROUNDS;

            // SSE2 floor: 4 pixels, branch-free, the operation is an OR when the
            // pixel is non-zero, which is what the branch computes anyway.
            reset();
            t0 = clk::now();
            for (int r = 0; r < ROUNDS; ++r) {
                DWORD* p = reinterpret_cast<DWORD*>(buf.pBits);
                int i = 0;
                for (; i + 4 <= nPix; i += 4) {
                    __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i));
                    __m128i rgb = _mm_and_si128(v, _mm_set1_epi32(0x00FFFFFF));
                    __m128i nonZero = _mm_cmpeq_epi32(rgb, _mm_setzero_si128());
                    // nonZero lanes are "all ones" where rgb == 0 => keep alpha as is
                    __m128i withAlpha = _mm_or_si128(v, _mm_set1_epi32(static_cast<int>(0xFF000000u)));
                    __m128i out = _mm_blendv_epi8(withAlpha, v, nonZero);
                    _mm_storeu_si128(reinterpret_cast<__m128i*>(p + i), out);
                }
                for (; i < nPix; ++i) {
                    DWORD v = p[i];
                    if ((v & 0xFF000000u) == 0 && (v & 0x00FFFFFFu) != 0)
                        p[i] = v | 0xFF000000u;
                }
            }
            t1 = clk::now();
            const double sseMs = Ms(t0, t1) / ROUNDS;

#if defined(__AVX2__)
            // AVX2 kernel: only compiled when the build actually targets AVX2, so
            // this number is reported honestly rather than assumed.
            reset();
            t0 = clk::now();
            for (int r = 0; r < ROUNDS; ++r) {
                DWORD* p = reinterpret_cast<DWORD*>(buf.pBits);
                int i = 0;
                const __m256i vAlpha = _mm256_set1_epi32(static_cast<int>(0xFF000000u));
                const __m256i vRgb   = _mm256_set1_epi32(0x00FFFFFF);
                for (; i + 8 <= nPix; i += 8) {
                    __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + i));
                    __m256i rgb = _mm256_and_si256(v, vRgb);
                    __m256i isZero = _mm256_cmpeq_epi32(rgb, _mm256_setzero_si256());
                    __m256i withAlpha = _mm256_or_si256(v, vAlpha);
                    __m256i out = _mm256_blendv_epi8(withAlpha, v, isZero);
                    _mm256_storeu_si256(reinterpret_cast<__m256i*>(p + i), out);
                }
                for (; i < nPix; ++i) {
                    DWORD v = p[i];
                    if ((v & 0xFF000000u) == 0 && (v & 0x00FFFFFFu) != 0)
                        p[i] = v | 0xFF000000u;
                }
            }
            t1 = clk::now();
            const double avxMs = Ms(t0, t1) / ROUNDS;
            ::printf("      %-42s %8.3f ms/frame\n", "C10 avx2   verify-fixup", avxMs);
            ::printf("      speedup vs scalar: %.2fx (SSE2: %.2fx)\n\n",
                     avxMs > 0 ? scalarMs / avxMs : 0.0, sseMs > 0 ? scalarMs / sseMs : 0.0);
#else
            ::printf("      %-42s %8.3f ms/frame\n", "C10 sse2   verify-fixup", sseMs);
            ::printf("      speedup vs scalar: %.2fx  (AVX2 not enabled in this build)\n\n",
                     sseMs > 0 ? scalarMs / sseMs : 0.0);
#endif

            ::printf("      %-42s %8.3f ms/frame\n", "C10 scalar verify-fixup", scalarMs);
            // Measured finding: vectorising this loop does NOT help. Almost every
            // pixel is already opaque, so the scalar branch is predicted ~100% of
            // the time and the loop is bound by memory traffic, not instruction
            // count. Recorded rather than asserted positive, because proposing a
            // SIMD rewrite here would be a change with no measured benefit.
            Chk("C10 simd does not regress the fixup", sseMs < scalarMs * 1.5,
                "vectorised fixup within 1.5x of scalar (SIMD not beneficial here)");
        }
    }

    // -------- C11: whole-frame paint of a real control tree, D2D off vs on --------
    // Every previous case measured a primitive in isolation. The decision this
    // probe exists to support ("which single change buys the most for the least
    // risk") needs the composite: paint an attached control tree through
    // CControlUI::Paint exactly as WM_PAINT does, once with the existing D2D text
    // cache enabled and once without.
    {
        // A label control always draws text; that is the only thing under test.
        const int ROWS = 120;
        const int COLS = 6;

        auto runFrame = [&](bool bD2D) -> double {
            CBitmapDC host(1200, 800);
            CPaintManagerUI mgr;
            mgr.Init(nullptr);
            mgr.SetDefaultFont(_T("Segoe UI"), 14, false, false, false, false, false);

            CVerticalLayoutUI* pRoot = new CVerticalLayoutUI();
            for (int r = 0; r < ROWS; ++r) {
                CHorizontalLayoutUI* pRow = new CHorizontalLayoutUI();
                pRow->SetFixedHeight(20);
                for (int c = 0; c < COLS; ++c) {
                    CLabelUI* pCell = new CLabelUI();
                    pCell->SetText(_T("Ready"));
                    pCell->SetTextColor(0xFF202020u);
                    pCell->SetFixedWidth(120);
                    pRow->Add(pCell);
                }
                pRoot->Add(pRow);
            }
            // AttachDialog makes the manager the owner; do not delete by hand.
            mgr.AttachDialog(pRoot);
            RECT rcAll = { 0, 0, 1200, 800 };
            pRoot->SetPos(rcAll, true);

            CRenderEngine::SetD2DTextEnabled(bD2D);
            RECT rcPaint = rcAll;
            // warm
            pRoot->Paint(host.hDC, rcPaint, nullptr);
            auto t0 = clk::now();
            const int FRAMES = 20;
            for (int f = 0; f < FRAMES; ++f)
                pRoot->Paint(host.hDC, rcPaint, nullptr);
            auto t1 = clk::now();
            CRenderEngine::SetD2DTextEnabled(false);
            return Ms(t0, t1) / FRAMES;
        };

        const double offMs = runFrame(false);
        const double onMs  = runFrame(true);
        ::printf("      %-42s %8.3f ms/frame  (%d labels)\n",
                 "C11 tree paint, D2D text OFF", offMs, ROWS * COLS);
        ::printf("      %-42s %8.3f ms/frame\n", "C11 tree paint, D2D text ON ", onMs);
        ::printf("      whole-frame gain: %.2fx  (saves %.2f ms/frame)\n\n",
                 onMs > 0.0 ? offMs / onMs : 0.0, offMs - onMs);
        Chk("C11 enabling the cache helps a real frame", onMs <= offMs,
            "tree paint not slower with the existing cache");
    }

    // -------- C12: does a scrolled list actually cull off-screen rows? --------
    // The largest theoretical win in a retained-mode UI is not a faster pixel,
    // it is not painting the pixels at all. CContainerUI::DoPaint does test each
    // child against rcPaint before calling Paint(), so culling should already
    // work -- but this project has been burned by "should" before. This counts
    // how many children actually receive Paint() on a 2000-row scrolled list.
    //
    // Probe note: the first version of this case moved the ROOT node up by
    // -18000px, which put the root entirely outside rcPaint, so the container
    // early-returned and 0 children painted. That measured the root falling off
    // screen, not scrolling. A real scroll keeps the container in place and
    // repositions the children, which is what SetScrollPos does below.
    {
        class CCountLabel : public CLabelUI
        {
        public:
            bool DoPaint(HDC hDC, const RECT& rcPaint, CControlUI* pStop) override
            {
                ++g_nPaintedDummy;
                return CLabelUI::DoPaint(hDC, rcPaint, pStop);
            }
        };

        const int ROWS = 2000;
        const int VIEW_H = 600;
        const int ROW_H = 20;

        CBitmapDC host(800, VIEW_H);
        CPaintManagerUI mgr;
        mgr.Init(nullptr);
        mgr.SetDefaultFont(_T("Segoe UI"), 14, false, false, false, false, false);

        CVerticalLayoutUI* pRoot = new CVerticalLayoutUI();
        CVerticalLayoutUI* pView = new CVerticalLayoutUI();
        pView->SetFixedWidth(800);
        pView->SetFixedHeight(VIEW_H);
        pRoot->Add(pView);
        mgr.AttachDialog(pRoot);
        RECT rcAll = { 0, 0, 800, VIEW_H };
        pRoot->SetPos(rcAll, true);

        for (int i = 0; i < ROWS; ++i) {
            CCountLabel* p = new CCountLabel();
            p->SetText(_T("row"));
            p->SetTextColor(0xFF202020u);
            p->SetFixedHeight(ROW_H);
            pView->Add(p);
        }
        pView->EnableScrollBar(true, false);
        pView->SetScrollStepSize(ROW_H);
        pView->SetPos(RECT{ 0, 0, 800, VIEW_H }, true);

        RECT rcPaint = rcAll;
        g_nPaintedDummy = 0;
        pRoot->Paint(host.hDC, rcPaint, nullptr);
        const int nTop = g_nPaintedDummy;

        // Scroll to the middle: children keep their identity, only their rects move.
        pView->SetScrollPos(CDuiSize(0, (ROWS / 2) * ROW_H));
        g_nPaintedDummy = 0;
        pRoot->Paint(host.hDC, rcPaint, nullptr);
        const int nMid = g_nPaintedDummy;

        const int nVisible = VIEW_H / ROW_H;            // 30 rows fit
        const int nBudget = nVisible * 2;               // allow one screen of slack

        ::printf("      %-42s %6d of %d rows\n", "C12 painted at top", nTop, ROWS);
        ::printf("      %-42s %6d of %d rows\n", "C12 painted scrolled to middle", nMid, ROWS);
        ::printf("      visible rows = %d, culling budget = %d\n\n", nVisible, nBudget);

        char sz[192];
        ::sprintf_s(sz, "%d of %d rows reached Paint() at the top", nTop, ROWS);
        Chk("C12 top of list paints only visible rows", nTop > 0 && nTop <= nBudget, sz);

        ::sprintf_s(sz, "%d of %d rows reached Paint() after scrolling", nMid, ROWS);
        // This is the assertion that matters: a list that paints all 2000 rows on
        // every scroll step cannot hold 60 FPS no matter how fast drawing is.
        Chk("C12 scrolled list culls off-screen rows", nMid > 0 && nMid <= nBudget, sz);
    }

    // -------- C7: composite -- a dense list page --------
    {
        // 2000 cells, each = one opaque fill + one 1px border + one short text.
        const int ROWS = 2000;
        CBitmapDC host(W, H);
        CPaintManagerUI mgr;
        mgr.Init(nullptr);
        mgr.SetDefaultFont(_T("Segoe UI"), 16, false, false, false, false, false);

        auto t0 = clk::now();
        for (int i = 0; i < ROWS; ++i) {
            RECT rc = { 0, (i % 20) * 40, 200, (i % 20) * 40 + 38 };
            CRenderEngine::DrawColor(host.hDC, rc, (i & 1) ? 0xFFFFFFFFu : 0xFFF5F5F5u);
            CRenderEngine::DrawLine(host.hDC, rc, 1, 0xFFE0E0E0u);
            RECT rt = rc;
            rt.left += 8;
            CRenderEngine::DrawText(host.hDC, &mgr, rt, _T("row"), 0xFF202020u, 0, DT_LEFT | DT_SINGLELINE);
        }
        auto t1 = clk::now();
        const double cur = Ms(t0, t1);
        Report("C7 current  2000 rows (fill+line+text)", cur, ROWS);

        HBRUSH hBr = ::CreateSolidBrush(RGB(0xF5, 0xF5, 0xF5));
        HPEN hPen = ::CreatePen(PS_SOLID, 1, RGB(0xE0, 0xE0, 0xE0));
        t0 = clk::now();
        for (int i = 0; i < ROWS; ++i) {
            RECT rc = { 0, (i % 20) * 40, 200, (i % 20) * 40 + 38 };
            ::FillRect(host.hDC, &rc, hBr);
            HPEN hOld = static_cast<HPEN>(::SelectObject(host.hDC, hPen));
            ::MoveToEx(host.hDC, rc.left, rc.bottom - 1, nullptr);
            ::LineTo(host.hDC, rc.right, rc.bottom - 1);
            ::SelectObject(host.hDC, hOld);
            RECT rt = rc;
            rt.left += 8;
            CRenderEngine::DrawText(host.hDC, &mgr, rt, _T("row"), 0xFF202020u, 0, DT_LEFT | DT_SINGLELINE);
        }
        t1 = clk::now();
        const double alt = Ms(t0, t1);
        ::DeleteObject(hBr);
        ::DeleteObject(hPen);
        Report("C7 alt      same rows, GDI fill+line", alt, ROWS);
        ::printf("      speedup if fills/lines used core GDI: %.1fx\n\n",
                 alt > 0.0 ? cur / alt : 0.0);

        Chk("C7 dense page ceiling", cur < 2000.0, "2000 rows within 2s budget");
    }

    if (g_bInject) {
        // Deliberately impossible budget: proves the ceiling checks can fail.
        Chk("INJECT forced failure", false, "inject mode always fails");
    }

    ::printf("SUMMARY passed=%d failed=%d\n", g_nPass, g_nFail);
    Gdiplus::GdiplusShutdown(gdipToken);
    return g_nFail == 0 ? 0 : 1;
}
