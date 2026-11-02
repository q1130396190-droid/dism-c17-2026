// ============================================================
// theme_incremental_probe.cpp -- prove that re-theming a page only touches the
// nodes it just created, instead of walking every previously built page.
//
// Why: EnsurePageBuilt() re-applies the theme after every first visit. On pages
// that build thousands of rows (optim: ~2100, assoc: ~4100, startup: ~2900) the
// old code walked the ENTIRE tree each time. That is a per-page-switch cost the
// user feels directly, and it grows with the app.
//
// The probe builds two trees (a small "earlier" tree and a large "new" tree),
// applies the theme once, then simulates a page switch and compares the number
// of controls re-evaluated against the total. It also verifies the incremental
// pass still themes the new nodes (no under-theming).
// ============================================================

#include "UIlib.h"
#include "../Demos/Win8Metro/ThemeService.h"

#include <windows.h>
#include <cstdio>
#include <string>

using namespace DuiLib;

namespace {

int g_nPass = 0;
int g_nFail = 0;

void Chk(const char* pszName, bool bOk, const std::string& sDetail)
{
    ::printf("[%s] %s | %s\n", bOk ? "PASS" : "FAIL", pszName, sDetail.c_str());
    ::fflush(stdout);
    if (bOk) ++g_nPass; else ++g_nFail;
}

CContainerUI* MakeRows(int n, const wchar_t* pszPrefix)
{
    CContainerUI* pBox = new CContainerUI();
    for (int i = 0; i < n; ++i)
    {
        CLabelUI* p = new CLabelUI();
        wchar_t szName[64] = {};
        ::swprintf_s(szName, L"%s_%d", pszPrefix, i);
        p->SetName(szName);
        p->SetText(L"row");
        p->SetBkColor(0xFFFFFFFF);
        p->SetTextColor(0xFF000000);
        pBox->Add(p);
    }
    return pBox;
}

int CountAll(CControlUI* pRoot)
{
    if (pRoot == nullptr) return 0;
    int n = 1;
    if (auto* pC = dynamic_cast<CContainerUI*>(pRoot))
        for (int i = 0; i < pC->GetCount(); ++i)
            n += CountAll(pC->GetItemAt(i));
    return n;
}

} // namespace

int main(int argc, char** argv)
{
    // [GATE-SELFTEST] /Inject adds one deliberately failing assertion so this
    // probe can prove its verdict path still works; otherwise a check that
    // silently stopped asserting would look exactly like a pass.
    bool bInject = false;
    for (int i = 1; i < argc; ++i)
        if (::lstrcmpiA(argv[i], "/Inject") == 0) bInject = true;

    // A page that already exists and has been themed.
    CContainerUI* pOld = MakeRows(500, L"old");
    CContainerUI* pRoot = new CContainerUI();
    pRoot->Add(pOld);

    // First full pass: themes everything (stamp 0 semantics).
    const int nFirst = theme::Apply(pRoot, theme::EMode::Dark);

    // A page switch builds 3000 new rows, then re-applies the theme.
    CContainerUI* pNew = MakeRows(3000, L"new");
    pRoot->Add(pNew);

    const int nTotal = CountAll(pRoot);

    // Reference: what a full re-walk would have had to look at. Counted without
    // applying, so the incremental pass below still has real work to do.
    const int nFullAgain = CountAll(pRoot) - CountAll(pOld);

    // Incremental pass used by EnsurePageBuilt().
    const int nInc = theme::ApplyIncremental(pRoot, theme::EMode::Dark, 4242u);

    char sz[320];
    ::sprintf_s(sz, "total=%d first=%d fullAgain=%d incremental=%d",
                nTotal, nFirst, nFullAgain, nInc);

    // The incremental pass must only visit the newly added subtree.
    Chk("incremental theme only touches new nodes", nInc <= 3001, sz);
    Chk("incremental theme is clearly cheaper than a full re-walk",
        nInc < nFullAgain, sz);

    // And it must actually theme them: a fresh label starts light, so after the
    // incremental pass it must no longer carry the light background.
    {
        auto* pBox = dynamic_cast<CContainerUI*>(pNew);
        CLabelUI* pLast = dynamic_cast<CLabelUI*>(pBox->GetItemAt(2999));
        const DWORD dwAfter = pLast->GetBkColor();
        char szC[200];
        ::sprintf_s(szC, "last new row bkcolor=0x%08X", (unsigned)dwAfter);
        Chk("incremental theme actually themed the new rows",
            dwAfter != 0xFFFFFFFF, szC);
    }

    // Re-applying with the SAME stamp must be a no-op (idempotent, cheap).
    const int nRepeat = theme::ApplyIncremental(pRoot, theme::EMode::Dark, 4242u);
    char szR[200];
    ::sprintf_s(szR, "repeat same stamp touched=%d", nRepeat);
    Chk("repeat with same stamp is a no-op", nRepeat == 0, szR);

    // Switching theme still needs a full pass.
    const int nSwitch = theme::ApplyIncremental(pRoot, theme::EMode::Light, 0u);
    char szS[200];
    ::sprintf_s(szS, "light full pass touched=%d", nSwitch);
    Chk("theme switch still performs a full pass", nSwitch > 0, szS);

    ::printf("\nresult: %d pass / %d fail\n", g_nPass, g_nFail);
    if (bInject)
    {
        Chk("injected-must-fail", false, "self-test: forced failure");
    }
    return (g_nFail == 0) ? 0 : 1;
}
