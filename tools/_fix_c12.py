import io

p = 'tools/probes/perf_struct_bench.cpp'
with io.open(p, 'r', encoding='utf-8', errors='replace') as f:
    text = f.read()

start = text.index('    // -------- C12: does a scrolled list actually cull off-screen rows? --------')
end = text.index('    // -------- C7: composite -- a dense list page --------')

new = r'''    // -------- C12: does a scrolled list actually cull off-screen rows? --------
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

'''

text = text[:start] + new + text[end:]

with io.open(p, 'w', encoding='utf-8', newline='') as f:
    f.write(text)
print('C12 rewritten with a real scroll container')
