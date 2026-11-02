import io

p = 'tools/probes/perf_struct_bench.cpp'
with io.open(p, 'r', encoding='utf-8', errors='replace') as f:
    text = f.read()

block = r'''
    // -------- C12: does a scrolled list actually cull off-screen rows? --------
    // The largest theoretical win in a retained-mode UI is not a faster pixel,
    // it is not painting the pixels at all. CContainerUI::DoPaint does test
    // each child for intersection with rcPaint before calling Paint(), so the
    // culling should already work -- but "should" is exactly the kind of claim
    // this project has been burned by. This counts how many children actually
    // receive a Paint() call when a 2000-row list is scrolled to the middle.
    {
        // Counts how many times the framework reached this control's paint.
        class CCountLabel : public CLabelUI
        {
        public:
            static int s_nPainted;
            bool DoPaint(HDC hDC, const RECT& rcPaint, CControlUI* pStop) override
            {
                ++s_nPainted;
                return CLabelUI::DoPaint(hDC, rcPaint, pStop);
            }
        };

        const int ROWS = 2000;
        const int VISIBLE_ROWS = 20;

        CBitmapDC host(800, 600);
        CPaintManagerUI mgr;
        mgr.Init(nullptr);
        mgr.SetDefaultFont(_T("Segoe UI"), 14, false, false, false, false, false);

        CCountLabel::s_nPainted = 0;

        CVerticalLayoutUI* pRoot = new CVerticalLayoutUI();
        for (int i = 0; i < ROWS; ++i) {
            CCountLabel* p = new CCountLabel();
            p->SetText(_T("row"));
            p->SetTextColor(0xFF202020u);
            p->SetFixedHeight(20);
            pRoot->Add(p);
        }
        mgr.AttachDialog(pRoot);
        RECT rcAll = { 0, 0, 800, 600 };
        pRoot->SetPos(rcAll, true);

        // Paint the first screen, then scroll far down and paint again.
        RECT rcPaint = rcAll;
        pRoot->Paint(host.hDC, rcPaint, nullptr);
        const int nFirst = CCountLabel::s_nPainted;

        CCountLabel::s_nPainted = 0;
        RECT rcMid = { 0, 0, 800, 600 };
        // Scroll by moving the root up: this is what a real scroll does.
        RECT rcScrolled = { 0, -18000, 800, -17400 };
        pRoot->SetPos(rcScrolled, true);
        pRoot->Paint(host.hDC, rcMid, nullptr);
        const int nScrolled = CCountLabel::s_nPainted;

        ::printf("      %-42s %6d of %d rows\n",
                 "C12 children painted, top of list", nFirst, ROWS);
        ::printf("      %-42s %6d of %d rows\n",
                 "C12 children painted, scrolled to middle", nScrolled, ROWS);

        const int budget = VISIBLE_ROWS * 3;
        ::printf("      expected visible rows ~%d, culling budget %d\n\n",
                 VISIBLE_ROWS, budget);

        char sz[192];
        ::sprintf_s(sz, "%d of %d rows reached Paint() after scrolling", nScrolled, ROWS);
        Chk("C12 scrolled list culls off-screen rows", nScrolled <= budget, sz);
        Chk("C12 first screen paints only what is visible", nFirst <= budget,
            "top screen within culling budget");
    }
'''

anchor = '    // -------- C7: composite -- a dense list page --------'
text = text.replace(anchor, block.lstrip('\n') + chr(10) + anchor, 1)
text = text.replace('using namespace DuiLib;', 'using namespace DuiLib;\n\nint g_nPaintedDummy = 0;', 1)

with io.open(p, 'w', encoding='utf-8', newline='') as f:
    f.write(text)
print('inserted C12')
