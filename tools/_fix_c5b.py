import io

p = 'tools/probes/perf_struct_bench.cpp'
with io.open(p, 'r', encoding='utf-8', errors='replace') as f:
    text = f.read()

start = text.index('    // -------- C5b: split the text cost')
end = text.index('    // -------- C8: the D2D text cache')

new = r'''    // -------- C5b: how much of the text cost belongs to DuiLib? --------
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

'''

text = text[:start] + new + text[end:]

with io.open(p, 'w', encoding='utf-8', newline='') as f:
    f.write(text)
print('C5b rewritten with interleaved batches')
