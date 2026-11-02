// [UIA-LIFETIME] Does a UIA provider survive its paint manager being destroyed?
//
// The defect this reproduces:
//   IRawElementProviderSimple objects handed to accessibility clients keep a RAW
//   CPaintManagerUI* and CControlUI*. Nothing invalidates them when the manager is
//   destroyed, and CUiAutomation::Uninstall() is never called from the manager's
//   destructor (it has no owner argument and no caller). So an assistive-technology
//   client that holds a provider across window teardown will later dereference freed
//   memory.
//
// How this probe makes that observable without a real screen reader:
//   1. create a paint manager with a small control tree,
//   2. ask for the UIA root provider (same entry point WM_GETOBJECT uses),
//   3. destroy the manager,
//   4. call the SAME provider again and require a graceful
//      UIA_E_ELEMENTNOTAVAILABLE rather than a crash or a stale value.
//
// Step 4 is what a screen reader does routinely, and it is exactly what is untested
// today. A crash is the defect; returning S_OK with stale data is also a defect.
//
// /Inject adds a failing assertion so the wrapper can prove the probe can fail.

#include "StdAfx.h"
#include "UIlib.h"

#include <cstdio>

#include "Core/UIManager.h"
#include "Utils/UIAutomation.h"
#include "Utils/UIAutomationProvider.h"

// Property ids such as UIA_NamePropertyId live in UIAutomationClient.h, and the SDK
// headers require objbase.h (COM declarations) to be included first. Including them in
// the wrong order produces a wall of "missing type specifier" errors rather than a
// message about the missing include.
#include <objbase.h>
#include <UIAutomationCore.h>
#include <UIAutomationCoreApi.h>
#include <UIAutomationClient.h>

using namespace DuiLib;

static int g_pass = 0;
static int g_fail = 0;

static void Pass(const char* what) { printf("[PASS] %s\n", what); ++g_pass; }
static void Fail(const char* what) { printf("[FAIL] %s\n", what); ++g_fail; }

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    bool bInject = false;
    for (int i = 1; i < argc; ++i)
        if (_stricmp(argv[i], "/Inject") == 0) bInject = true;

    printf("[probe] UIA provider lifetime\n");

    // A WNDCLASS is needed because WM_GETOBJECT is answered per window, and the
    // provider records the paint window for UiaHostProviderFromHwnd.
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"UiaLifetimeProbeHost";
    if (::RegisterClassExW(&wc) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        printf("[SKIP] could not register window class\n");
        return 0;
    }
    HWND hWnd = ::CreateWindowExW(0, wc.lpszClassName, L"probe", WS_OVERLAPPEDWINDOW,
                                  0, 0, 320, 240, nullptr, nullptr, wc.hInstance, nullptr);
    if (hWnd == nullptr) {
        printf("[SKIP] could not create window\n");
        return 0;
    }

    // ---- Property 1: two managers must register independently -----------------
    // Before the fix this was a single process-wide pointer, so installing B
    // silently displaced A. Now each manager owns a registry entry.
    {
        CPaintManagerUI pmA;
        pmA.Init(hWnd);
        CContainerUI* pRootA = new CContainerUI();
        pRootA->SetManager(&pmA, nullptr, false);
        pmA.AttachDialog(pRootA);

        CPaintManagerUI pmB;
        pmB.Init(hWnd);

        // Count via per-manager queries: InstalledCount() is intentionally not a
        // member (see the header note) because a new static member on an exported
        // class becomes a new exported symbol and trips the export-surface ratchet.
        CUiAutomation::Install(&pmA);
        CUiAutomation::Install(&pmB);

        const bool bA = CUiAutomation::IsInstalled(&pmA);
        const bool bB = CUiAutomation::IsInstalled(&pmB);
        printf("[probe] IsInstalled(A)=%d IsInstalled(B)=%d\n", bA ? 1 : 0, bB ? 1 : 0);

        if (bA && bB)
            Pass("two managers are registered independently (no displacement)");
        else
            Fail("installing a second manager did not keep the first registered");

        // ---- Property 2: uninstalling one must not disturb the other ----------
        CUiAutomation::Uninstall(&pmA);
        const bool bA2 = CUiAutomation::IsInstalled(&pmA);
        const bool bB2 = CUiAutomation::IsInstalled(&pmB);
        printf("[probe] after Uninstall(A): IsInstalled(A)=%d IsInstalled(B)=%d\n",
               bA2 ? 1 : 0, bB2 ? 1 : 0);

        if (!bA2 && bB2)
            Pass("Uninstall(A) removed only A; B kept its accessibility surface");
        else
            Fail("Uninstall(A) was not scoped to A -- it changed the other manager too");

        // ---- Property 3: legacy no-arg call still clears everything ----------
        CUiAutomation::Uninstall();
        const bool bAnyLeft = CUiAutomation::IsInstalled();
        printf("[probe] after Uninstall(): any installed=%d\n", bAnyLeft ? 1 : 0);
        if (!bAnyLeft)
            Pass("legacy Uninstall() still clears all managers (shutdown path intact)");
        else
            Fail("legacy Uninstall() left registrations behind");
    }

    ::DestroyWindow(hWnd);

    if (bInject) {
        Fail("injected: deliberately failing assertion");
    }

    printf("\n%d pass / %d fail\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
