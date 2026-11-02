import io

p = 'tools/probes/perf_struct_bench.cpp'
with io.open(p, 'r', encoding='utf-8', errors='replace') as f:
    text = f.read()

block = r'''
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
'''

anchor = '    // -------- C7: composite -- a dense list page --------'
text = text.replace(anchor, block.lstrip('\n') + chr(10) + anchor, 1)

with io.open(p, 'w', encoding='utf-8', newline='') as f:
    f.write(text)
print('inserted C11')
