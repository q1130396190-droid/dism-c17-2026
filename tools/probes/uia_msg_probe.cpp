// [A11Y-3 diag] Ask the window directly whether it answers UiaRootObjectId.
//
// Why: the UIA client path (ElementFromHandle) never reached the provider, and
// ruling out HWND identity and integrity level left one question open -- does
// the window answer WM_GETOBJECT at all? This sends the message by hand so the
// answer cannot be confused with UIA caching or proxy behaviour.
#include <windows.h>
#include <objbase.h>              /* COM base types first */
#include <UIAutomationCore.h>     /* PROPERTYID etc. consumed by CoreApi */
#include <UIAutomationCoreApi.h>
#include <cstdio>

#pragma comment(lib, "oleacc.lib")
#pragma comment(lib, "uiautomationcore.lib")

int main(int argc, char** argv)
{
    if (argc < 2) { ::printf("[FAIL] usage: probe <hwnd-decimal>"); ::putchar(10); return 2; }
    HWND h = reinterpret_cast<HWND>(static_cast<INT_PTR>(_strtoi64(argv[1], nullptr, 10)));
    if (h == nullptr) { ::printf("[FAIL] null hwnd"); ::putchar(10); return 2; }

    // 1) Does the window answer the MSAA client object?
    LRESULT lAcc = ::SendMessageW(h, WM_GETOBJECT, 0, OBJID_CLIENT);
    ::printf("WM_GETOBJECT(OBJID_CLIENT=0x%08lX) -> 0x%llX", (unsigned long)OBJID_CLIENT,
             (unsigned long long)lAcc);
    ::putchar(10);

    // 2) Does the window answer the UIA root object id?
    LRESULT lUia = ::SendMessageW(h, WM_GETOBJECT, 0, (LPARAM)UiaRootObjectId);
    ::printf("WM_GETOBJECT(UiaRootObjectId=%d) -> 0x%llX", (int)UiaRootObjectId,
             (unsigned long long)lUia);
    ::putchar(10);

    const bool bAcc = (lAcc != 0);
    const bool bUia = (lUia != 0);
    ::printf("verdict: msaa=%d uia=%d", bAcc ? 1 : 0, bUia ? 1 : 0);
    ::putchar(10);
    return (bAcc && bUia) ? 0 : 1;
}
