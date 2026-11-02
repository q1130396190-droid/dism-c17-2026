import io

p = 'tools/probes/perf_struct_bench.cpp'
with io.open(p, 'r', encoding='utf-8', errors='replace') as f:
    text = f.read()

block = r'''
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
            CDuiString sDyn;
            for (int w = 0; w < 64; ++w) {
                sDyn.Format(_T("counter %d"), w);
                RECT rc = rcText;
                CRenderEngine::DrawText(host.hDC, &mgr, rc, sDyn.GetData(), 0xFF202020u, -1,
                                        DT_LEFT | DT_SINGLELINE);
            }
            t0 = clk::now();
            for (int i = 0; i < 2000; ++i) {
                sDyn.Format(_T("counter %d"), i);
                RECT rc = rcText;
                CRenderEngine::DrawText(host.hDC, &mgr, rc, sDyn.GetData(), 0xFF202020u, -1,
                                        DT_LEFT | DT_SINGLELINE);
            }
            t1 = clk::now();
            const double dynMs = Ms(t0, t1);
            Report("C8 D2D      DrawText dynamic (all miss)", dynMs, 2000);
            Chk("C8 dynamic text not penalised", (dynMs / 2000.0) < (gdiMs / N) * 3.0,
                "dynamic stream stays within 3x of GDI");
            CRenderEngine::SetD2DTextEnabled(false);
        }
        else {
            Chk("C8 d2d text availability", false, "DirectWrite unavailable on this machine");
        }
    }
'''

anchor = '    // -------- C7: composite -- a dense list page --------'
text = text.replace(anchor, block.lstrip('\n') + chr(10) + anchor, 1)

with io.open(p, 'w', encoding='utf-8', newline='') as f:
    f.write(text)
print('inserted C8')
