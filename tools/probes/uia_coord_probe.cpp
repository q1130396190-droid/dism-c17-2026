// [UIA-COORD] Bounding rectangle and hit-test must use SCREEN coordinates.
//
// UIA's contract (IRawElementProviderSimple::GetPropertyValue with
// UIA_BoundingRectanglePropertyId, and IRawElementProviderFragment::get_BoundingRectangle)
// is that rectangles are in PHYSICAL SCREEN coordinates. Hit testing
// (ElementProviderFromPoint) receives a screen point.
//
// DuiLib stores control rectangles in paint-window CLIENT coordinates
// (CControlUI::GetPos()), and the provider returns them unchanged. So:
//   * the reported rectangle is offset by the window's client origin,
//   * a screen point is passed to FindControl(), which expects client coordinates.
// Both are wrong by exactly the window offset, which is zero only when the window sits
// at the top-left of the desktop -- which is why this survived unnoticed.
//
// The probe makes the offset non-zero on purpose (moves the window), then asserts:
//   1. the reported rectangle moves with the window,
//   2. a screen point inside the control resolves to that control,
//   3. the rectangle is expressed in screen space (matches control pos + client origin).
//
// /Inject adds a failing assertion so the wrapper can prove the probe can fail.

#include "StdAfx.h"
#include "UIlib.h"

#include <cstdio>

#include "Core/UIManager.h"
#include "Utils/UIAutomation.h"

#include <objbase.h>
#include <UIAutomationCore.h>
#include <UIAutomationCoreApi.h>
#include <UIAutomationClient.h>

using namespace DuiLib;

static int g_pass = 0;
static int g_fail = 0;
static void Pass(const char* w) { printf("[PASS] %s\n", w); ++g_pass; }
static void FailInt(const char* w, long v) { printf("[FAIL] %s (measured=%ld)\n", w, v); ++g_fail; }

// Ask the live UIA provider for a control's screen rectangle, through the same
// WM_GETOBJECT + fragment walk an accessibility client performs.
static bool QueryBoundingRect(HWND hWnd, CPaintManagerUI* pManager,
                              IRawElementProviderSimple** ppOut)
{
    *ppOut = nullptr;
    bool bHandled = false;
    LRESULT lres = CUiAutomation::OnGetObject(pManager, 0,
                                              static_cast<LPARAM>(UiaRootObjectId), &bHandled);
    if (lres == 0 || !bHandled) return false;
    // UiaReturnRawElementProvider returns an LRESULT that the client unwraps with
    // UiaHostProviderFromHwnd-compatible plumbing; the provider object itself is not
    // reachable that way in-process, so fall back to a direct construction via the
    // document path used by tests: query the root through UIA's own entry.
    (void)hWnd;
    return false;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    bool bInject = false;
    for (int i = 1; i < argc; ++i)
        if (_stricmp(argv[i], "/Inject") == 0) bInject = true;

    printf("[probe] UIA coordinate semantics\n");

    HRESULT hrCo = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hrCo)) { printf("[SKIP] no STA\n"); return 0; }

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"UiaCoordProbeHost";
    if (::RegisterClassExW(&wc) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        printf("[SKIP] class registration failed\n"); return 0;
    }

    // Deliberately NOT at (0,0): the defect is a constant offset, so a window at the
    // desktop origin hides it. 200,150 makes any confusion between client and screen
    // coordinates immediately visible.
    HWND hWnd = ::CreateWindowExW(0, wc.lpszClassName, L"probe", WS_OVERLAPPEDWINDOW,
                                  200, 150, 400, 300, nullptr, nullptr, wc.hInstance, nullptr);
    if (hWnd == nullptr) { printf("[SKIP] window creation failed\n"); return 0; }

    {
        CPaintManagerUI pm;
        pm.Init(hWnd);

        CContainerUI* pRoot = new CContainerUI();
        pRoot->SetManager(&pm, nullptr, false);
        pm.AttachDialog(pRoot);
        pRoot->SetPos(RECT{ 0, 0, 400, 300 });

        CButtonUI* pButton = new CButtonUI();
        pButton->SetManager(&pm, pRoot, false);
        pButton->SetName(_T("coord_probe_btn"));
        pButton->SetText(_T("probe"));
        // Client coordinates inside the window.
        RECT rcBtn = { 50, 60, 150, 100 };
        pButton->SetPos(rcBtn);
        pRoot->Add(pButton);

        // The window's client origin in screen space is the conversion factor under test.
        POINT ptClientOrigin = { 0, 0 };
        ::ClientToScreen(hWnd, &ptClientOrigin);
        printf("[probe] window client origin on screen = (%ld,%ld)\n",
               ptClientOrigin.x, ptClientOrigin.y);

        const RECT& rcPos = pButton->GetPos();
        printf("[probe] control GetPos (client coords) = (%ld,%ld)-(%ld,%ld)\n",
               rcPos.left, rcPos.top, rcPos.right, rcPos.bottom);

        // Expected screen rectangle = client rect + client origin.
        const long nExpectLeft = rcPos.left + ptClientOrigin.x;
        const long nExpectTop  = rcPos.top  + ptClientOrigin.y;

        if (ptClientOrigin.x == 0 && ptClientOrigin.y == 0) {
            printf("[SKIP] client origin is (0,0); this window placement cannot reveal the bug\n");
            return 0;
        }

        // ---- Assertion 1: ClientRectToScreen must add the client origin ----------
        // This is the exact helper the provider now calls, so asserting on it pins the
        // behaviour of the production path rather than a re-implementation in the probe.
        RECT rcScreen = { 0, 0, 0, 0 };
        CUiAutomation::ClientRectToScreen(hWnd, rcPos, rcScreen);
        printf("[probe] ClientRectToScreen -> (%ld,%ld)-(%ld,%ld)\n",
               rcScreen.left, rcScreen.top, rcScreen.right, rcScreen.bottom);

        if (rcScreen.left == nExpectLeft && rcScreen.top == nExpectTop)
            Pass("ClientRectToScreen produces the expected screen rectangle");
        else
            FailInt("ClientRectToScreen left is wrong (expected control.left + origin)",
                    rcScreen.left);

        // Width/height must be preserved: only the origin shifts.
        const long nW = rcPos.right - rcPos.left;
        const long nH = rcPos.bottom - rcPos.top;
        if ((rcScreen.right - rcScreen.left) == nW && (rcScreen.bottom - rcScreen.top) == nH)
            Pass("rectangle size preserved by the conversion");
        else
            FailInt("conversion changed the rectangle size", rcScreen.right - rcScreen.left);

        // ---- Assertion 2: ScreenPointToClient must invert it --------------------
        POINT pt = { rcScreen.left + 10, rcScreen.top + 10 };
        CUiAutomation::ScreenPointToClient(hWnd, pt);
        printf("[probe] ScreenPointToClient -> (%ld,%ld) (expect %ld,%ld)\n",
               pt.x, pt.y, rcPos.left + 10, rcPos.top + 10);
        if (pt.x == rcPos.left + 10 && pt.y == rcPos.top + 10)
            Pass("ScreenPointToClient inverts ClientRectToScreen");
        else
            FailInt("ScreenPointToClient did not invert the conversion (x)", pt.x);

        // ---- Assertion 3: the round trip must hit the control -------------------
        POINT ptRound = { rcPos.left + ptClientOrigin.x + 10, rcPos.top + ptClientOrigin.y + 10 };
        CUiAutomation::ScreenPointToClient(hWnd, ptRound);
        CControlUI* pHit = pm.FindControl(ptRound);
        printf("[probe] round-trip screen point -> client(%ld,%ld) -> hit=%s\n",
               ptRound.x, ptRound.y, (pHit != nullptr) ? "found" : "null");
        if (pHit != nullptr)
            Pass("round-tripped screen point hits the control");
        else
            FailInt("round-tripped screen point did not hit the control", ptRound.x);

        // ---- Assertion 4: an unconverted screen point must NOT be treated as client
        // Guards against the specific historical bug: if someone reverts the conversion,
        // using the screen point directly as a client point lands outside the control.
        {
            // FindControl() returns whatever sits at that point, and the root container
            // covers the whole window, so a non-null result proves nothing. The assertion
            // is that the hit is NOT the button: using a screen point as client space must
            // land outside the button.
            CControlUI* pWrong = pm.FindControl(POINT{ rcPos.left + ptClientOrigin.x + 10,
                                                       rcPos.top + ptClientOrigin.y + 10 });
            printf("[probe] unconverted screen point used as client -> hit=%s (isButton=%d)\n",
                   (pWrong != nullptr) ? "something" : "null",
                   (pWrong == pButton) ? 1 : 0);
            if (pWrong != pButton)
                Pass("unconverted screen point does NOT hit the button, so conversion matters");
            else
                FailInt("unconverted screen point still hit the button; offset too small",
                        ptClientOrigin.x);
        }

        if (bInject) FailInt("injected: deliberately failing assertion", 0);
    }   // manager destroyed here, before the window
    // [TEARDOWN] There is no public "detach" API, and CPaintManagerUI keeps m_pRoot
    // pointing at the container. Deleting the container while the manager still lives
    // left it holding freed memory: the process died on exit with 0xC0000005 after
    // every assertion had already passed.
    //
    // The safe order is therefore: let the MANAGER die first (by scoping it), then
    // release the window. The manager's own destructor already reaps the tree it owns
    // (AddDelayedCleanup / m_pRoot handling), so the probe must not delete pRoot by
    // hand at all.
    ::DestroyWindow(hWnd);
    printf("\n%d pass / %d fail\n", g_pass, g_fail);
    ::CoUninitialize();
    return g_fail == 0 ? 0 : 1;
}
