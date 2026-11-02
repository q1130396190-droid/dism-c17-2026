// [R88] Can a control be created from OUTSIDE the DLL?
//
// Why this probe exists: round 87 found that CRingUI exported zero symbols while
// being registered with the factory (ControlFactory.cpp: INNER_REGISTER_DUICONTROL).
// It was invisible because every caller lived inside the DLL, where an unexported
// symbol resolves fine. Fixing it added member-level exports and CreateControl is
// now present in the export table -- but "the symbol is exported" and "a client can
// actually construct the control" are different claims, and only the second one
// matters to a user of the library.
//
// This links against DuiLib.lib (the import library, i.e. exactly what a third
// party would link) rather than including the control's header implementation, so a
// missing export fails at LINK time and a broken factory path fails at RUN time.
//
// It asserts three things:
//   1. the factory returns a non-null control for "RingUI";
//   2. the returned object reports the expected class name;
//   3. a name that was never registered returns null (so a "success" above is not
//      just the factory ignoring its argument).
#include <windows.h>
#include <cstdio>
#include "UIlib.h"

using namespace DuiLib;

namespace {
int g_nPass = 0;
int g_nFail = 0;

void Chk(const char* pszName, bool bOk, const char* pszDetail)
{
    ::printf("[%s] %s | %s\n", bOk ? "PASS" : "FAIL", pszName, pszDetail);
    if (bOk) ++g_nPass; else ++g_nFail;
}
} // namespace

int main(int argc, char** argv)
{
    bool bInject = false;
    for (int i = 1; i < argc; ++i)
        if (::lstrcmpiA(argv[i], "/Inject") == 0) bInject = true;

    CControlFactory* pFactory = CControlFactory::GetInstance();
    if (pFactory == nullptr)
    {
        ::printf("[FAIL] factory-instance | CControlFactory::GetInstance() returned null\n");
        ::printf("\nresult: 0 pass / 1 fail\n");
        return 1;
    }
    Chk("factory-instance", true, "CControlFactory::GetInstance() non-null");

    // 1) The control this round repaired.
    // INNER_REGISTER_DUICONTROL(CRingUI) expands to RegistControl(_T("CRingUI"), ...)
    // -- the registration name is the CLASS name, not a UI-suffixed type name.
    // Measured round 88: "RingUI" returned nullptr while "CRingUI" works.
    CControlUI* pRing = pFactory->CreateControl(CDuiString(_T("CRingUI")));
    {
        char sz[160] = { 0 };
        ::sprintf_s(sz, "CreateControl(\"CRingUI\") -> %s",
                    pRing != nullptr ? "object" : "nullptr");
        Chk("create-RingUI", pRing != nullptr, sz);
    }

    // 2) If we got an object, it must identify itself correctly. A factory that
    //    returned the wrong type would still pass test 1.
    if (pRing != nullptr)
    {
        const CDuiString sClass = pRing->GetClass();
        char sz[200] = { 0 };
        ::sprintf_s(sz, "GetClass()=\"%ls\"",
                    sClass.GetData() != nullptr ? sClass.GetData() : L"(null)");
        // Two different names on purpose, and it is worth pinning both:
        //   registration name = "CRingUI"  (the C++ class name, used by the factory)
        //   GetClass()        = "RingUI"   (the skin/XML tag name)
        // Round 88 asserted the registration name here and got a red for a passing
        // system; this assertion now checks the documented skin tag.
        Chk("RingUI-class-name",
            sClass.GetData() != nullptr && ::lstrcmpiW(sClass.GetData(), L"RingUI") == 0,
            sz);
        // The factory hands back a raw pointer and owns nothing; deleting it here is
        // what a client would do for a control it created without a parent.
        delete pRing;
    }
    else
    {
        Chk("RingUI-class-name", false, "skipped: no object to inspect");
    }

    // 3) Negative control: an unregistered name must yield null. Without this, a
    //    factory that ignored its argument entirely would pass everything above.
    {
        CControlUI* pBogus = pFactory->CreateControl(CDuiString(_T("NoSuchControlUI88")));
        char sz[160] = { 0 };
        ::sprintf_s(sz, "CreateControl(\"NoSuchControlUI88\") -> %s",
                    pBogus != nullptr ? "object (unexpected)" : "nullptr (expected)");
        Chk("unknown-name-yields-null", pBogus == nullptr, sz);
        if (pBogus != nullptr) delete pBogus;
    }

    if (bInject)
    {
        ::printf("[FAIL] injected-must-fail | self-test: forced failure\n");
        ++g_nFail;
    }

    ::printf("\nresult: %d pass / %d fail\n", g_nPass, g_nFail);
    return g_nFail == 0 ? 0 : 1;
}
