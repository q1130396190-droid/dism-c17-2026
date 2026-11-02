// R242 性能门禁探针
//
// 输出格式固定为 [METRIC] name=value，供 tools/perf_regression.py 解析。
// 采集的是「已优化过」的关键路径，用于防止后续改动静默把性能退回去。
#include "StdAfx.h"
#include "UIlib.h"
#include <cstdio>
#include <chrono>
#include "Core/UIRender.h"
#include "Core/UIManager.h"
#include "Core/UIControl.h"
#include "Control/UIButton.h"

using clk = std::chrono::high_resolution_clock;
static double ms_since(clk::time_point a) {
    return std::chrono::duration<double, std::milli>(clk::now() - a).count();
}

int main()
{
    using namespace DuiLib;
    const int W = 1920, H = 1080;
    const size_t nPix = (size_t)W * H;

    HDC hScreen = ::GetDC(nullptr);
    HDC hMem = ::CreateCompatibleDC(hScreen);
    BYTE* pBits = nullptr;
    HBITMAP hBmp = CRenderEngine::CreateARGB32Bitmap(hMem, W, H, &pBits);
    if (!hBmp || !pBits) { printf("[FATAL] no dib\n"); return 2; }
    HBITMAP hOld = (HBITMAP)::SelectObject(hMem, hBmp);

    // --- M1: 清屏路径（ZeroMemory 整行，对应 PERF-01）---
    {
        auto t0 = clk::now();
        const int N = 20;
        for (int i = 0; i < N; ++i)
            ::ZeroMemory(pBits, nPix * 4);
        printf("[METRIC] clear_fullscreen_ms=%.4f\n", ms_since(t0) / N);
    }

    // --- M2: AlphaBlend 整屏合成（架构上限）---
    {
        HDC hSrc = ::CreateCompatibleDC(hScreen);
        BYTE* pSrc = nullptr;
        HBITMAP hSrcBmp = CRenderEngine::CreateARGB32Bitmap(hSrc, W, H, &pSrc);
        ::SelectObject(hSrc, hSrcBmp);
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        auto t0 = clk::now();
        const int N = 10;
        for (int i = 0; i < N; ++i)
            ::AlphaBlend(hMem, 0, 0, W, H, hSrc, 0, 0, W, H, bf);
        printf("[METRIC] alphablend_fullscreen_ms=%.4f\n", ms_since(t0) / N);
        ::DeleteObject(hSrcBmp);
        ::DeleteDC(hSrc);
    }

    // --- M3: DrawImage 每次调用（PERF-02：DC 已复用）---
    {
        RECT rc = { 0, 0, 320, 240 };
        RECT rcPaint = { 0, 0, 320, 240 };
        RECT rcBmp = { 0, 0, 320, 240 };
        RECT rcCorner = { 0, 0, 0, 0 };
        auto t0 = clk::now();
        const int N = 500;
        for (int i = 0; i < N; ++i)
            CRenderEngine::DrawImage(hMem, hBmp, rc, rcPaint, rcBmp, rcCorner,
                                     true, 255, false, false, false);
        printf("[METRIC] drawimage_us_per_call=%.4f\n", ms_since(t0) * 1000.0 / N);
    }

    // --- M4: 文字（GDI vs D2D 缓存命中）---
    CPaintManagerUI pm;
    pm.Init(nullptr);
    pm.SetDefaultFont(_T("Segoe UI"), 16, false, false, false, false, false);

    {
        HDC hTxt = ::CreateCompatibleDC(hScreen);
        BYTE* pTxt = nullptr;
        HBITMAP hTxtBmp = CRenderEngine::CreateARGB32Bitmap(hTxt, 420, 60, &pTxt);
        HBITMAP hTxtOld = (HBITMAP)::SelectObject(hTxt, hTxtBmp);
        RECT rc = { 0, 0, 420, 60 };
        const TCHAR* kText = _T("DuiLib perf gate text sample 0123");

        // GDI
        {
            HFONT hf = ::CreateFont(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, DEFAULT_PITCH, _T("Segoe UI"));
            HFONT hfOld = (HFONT)::SelectObject(hTxt, hf);
            ::SetBkMode(hTxt, TRANSPARENT);
            auto t0 = clk::now();
            const int N = 200;
            for (int i = 0; i < N; ++i)
                ::DrawText(hTxt, kText, -1, &rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            printf("[METRIC] gdi_text_us_per_call=%.4f\n", ms_since(t0) * 1000.0 / N);
            ::SelectObject(hTxt, hfOld);
            ::DeleteObject(hf);
        }

        // D2D（缓存命中路径）
        if (CRenderEngine::IsD2DTextAvailable()) {
            CRenderEngine::SetD2DTextEnabled(true);
            // 预热缓存
            CRenderEngine::DrawTextD2D(hTxt, &pm, rc, kText, 0xFF202020, -1,
                                       DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            auto t0 = clk::now();
            const int N = 200;
            for (int i = 0; i < N; ++i)
                CRenderEngine::DrawTextD2D(hTxt, &pm, rc, kText, 0xFF202020, -1,
                                           DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            printf("[METRIC] d2d_text_cached_us_per_call=%.4f\n", ms_since(t0) * 1000.0 / N);

            // [R249] Dynamic-text metric.
            //
            // R247 added adaptive fallback: a run of cache misses marks the text
            // stream as dynamic and hands it back to GDI. Without a gate on this,
            // a later change could silently disable the fallback and make clocks /
            // progress bars ~10x slower than before, while every functional test
            // still passes. This metric is the regression tripwire for that.
            {
                CDuiString sDyn;
                // Warm-up so the miss-streak detector has already tripped and the
                // cooldown window is active before we start timing.
                for (int w = 0; w < 32; ++w) {
                    sDyn.Format(_T("dyn %d"), w);
                    CRenderEngine::DrawTextD2D(hTxt, &pm, rc, sDyn.GetData(), 0xFF202020,
                                               -1, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
                }
                auto td = clk::now();
                const int ND = 200;
                for (int i = 0; i < ND; ++i) {
                    sDyn.Format(_T("dyn %d"), i);
                    CRenderEngine::DrawTextD2D(hTxt, &pm, rc, sDyn.GetData(), 0xFF202020,
                                               -1, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
                }
                printf("[METRIC] d2d_text_dynamic_us_per_call=%.4f\n",
                       ms_since(td) * 1000.0 / ND);
            }

            CRenderEngine::SetD2DTextEnabled(false);
        } else {
            printf("[METRIC] d2d_text_cached_us_per_call=-1\n");
            printf("[METRIC] d2d_text_dynamic_us_per_call=-1\n");
        }

        ::SelectObject(hTxt, hTxtOld);
        ::DeleteObject(hTxtBmp);
        ::DeleteDC(hTxt);
    }

    // --- M5: PaintStatusImage 解析缓存（PERF-03）---
    {
        DuiLib::CButtonUI btn;
        btn.SetAttribute(_T("stateimage"),
            _T("file='btn_states.png' source='0,0,120,40' dest='0,0,120,40' mask='#FF00FF' fade='255'"));
        btn.SetPos(DuiLib::CDuiRect(0, 0, 200, 60));
        btn.SetManager(&pm, nullptr, false);
        btn.SetVisible(true);
        // 预热
        btn.Paint(hMem, DuiLib::CDuiRect(0, 0, 200, 60), nullptr);
        auto t0 = clk::now();
        const int N = 500;
        for (int i = 0; i < N; ++i)
            btn.Paint(hMem, DuiLib::CDuiRect(0, 0, 200, 60), nullptr);
        printf("[METRIC] button_paint_us_per_call=%.4f\n", ms_since(t0) * 1000.0 / N);
        btn.SetManager(nullptr, nullptr, false);
    }

    // --- M6: 控件树遍历 ---
    {
        DuiLib::CVerticalLayoutUI root;
        for (int i = 0; i < 2000; ++i) {
            auto* b = new DuiLib::CButtonUI();
            b->SetText(_T("b"));
            root.Add(b);
        }
        auto t0 = clk::now();
        for (int pass = 0; pass < 20; ++pass) {
            for (int i = 0; i < root.GetCount(); ++i) {
                DuiLib::CControlUI* c = root.GetItemAt(i);
                (void)c->GetName();
                (void)c->IsVisible();
            }
        }
        printf("[METRIC] iterate_2000_ms_per_pass=%.4f\n", ms_since(t0) / 20);
    }

    ::SelectObject(hMem, hOld);
    ::DeleteObject(hBmp);
    ::DeleteDC(hMem);
    ::ReleaseDC(nullptr, hScreen);
    printf("[METRIC] done=1\n");
    return 0;
}
