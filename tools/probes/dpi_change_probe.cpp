// ============================================================
// dpi_change_probe.cpp -- verify WM_DPICHANGED is handled with the system's
// suggested rectangle, not by proportional scaling of the old rect.
//
// Why: the framework used to scale the window rect by newScale/oldScale. On a
// multi-monitor setup that drifts (the work areas differ) and it ignores the
// exact non-client metrics the system computes for the new DPI.
//
// The probe sends a real WM_DPICHANGED with a known suggested rect and checks:
//   D1  the scale actually changed (DPIObj reflects the new DPI)
//   D2  the window moved/resized to the suggested rect
//   D3  the root was marked for a layout update
//   D4  an unsupported DPI (0) is ignored instead of dividing by zero
// ============================================================

#include "UIlib.h"

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

const wchar_t* kCls = L"DpiChangeProbe";

LRESULT CALLBACK ProbeProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    CPaintManagerUI* pm = reinterpret_cast<CPaintManagerUI*>(::GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    if (pm != nullptr) {
        LRESULT lRes = 0;
        if (pm->MessageHandler(uMsg, wParam, lParam, lRes)) return lRes;
    }
    return ::DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

HWND MakeHost()
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ProbeProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = kCls;
    ::RegisterClassExW(&wc);
    return ::CreateWindowExW(0, kCls, L"dpi", WS_OVERLAPPEDWINDOW,
                             100, 100, 400, 300, nullptr, nullptr,
                             ::GetModuleHandleW(nullptr), nullptr);
}

} // namespace

int main(int argc, char** argv)
{
    // [GATE-SELFTEST] /Inject adds one deliberately failing assertion so this
    // probe can prove its verdict path still works; otherwise a check that
    // silently stopped asserting would look exactly like a passing run.
    bool bInject = false;
    for (int i = 1; i < argc; ++i)
        if (::lstrcmpiA(argv[i], "/Inject") == 0) bInject = true;

    ::SetProcessDPIAware();
    HWND hWnd = MakeHost();
    if (hWnd == nullptr) { ::puts("[FATAL] no host window"); return 2; }

    CPaintManagerUI pm;
    pm.Init(hWnd, L"DpiChangeProbe");
    ::SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&pm));

    CContainerUI* pRoot = new CContainerUI();
    pm.AttachDialog(pRoot);
    ::ShowWindow(hWnd, SW_SHOWNOACTIVATE);

    const int nScaleBefore = pm.GetDPIObj()->GetScale();

    // Suggest a deliberately distinctive rectangle: nothing here matches what
    // proportional scaling of the old rect would produce.
    RECT rcSuggested = { 220, 180, 220 + 917, 180 + 613 };
    ::SendMessageW(hWnd, WM_DPICHANGED, MAKEWPARAM(144, 144), reinterpret_cast<LPARAM>(&rcSuggested));

    const int nScaleAfter = pm.GetDPIObj()->GetScale();
    {
        // GetScale() is a PERCENT, not the raw DPI: SetScale(144) stores
        // MulDiv(144,100,96) == 150. Asserting 144 would fail against correct
        // behaviour.
        char sz[220];
        ::sprintf_s(sz, "scale %d -> %d (expected 150 from 144 dpi)", nScaleBefore, nScaleAfter);
        Chk("WM_DPICHANGED updates the DPI scale", nScaleAfter == 150, sz);
    }

    RECT rcNow = { 0 };
    ::GetWindowRect(hWnd, &rcNow);
    {
        char sz[260];
        ::sprintf_s(sz, "window=%ld,%ld %ldx%ld suggested=%ld,%ld %ldx%ld",
                    rcNow.left, rcNow.top, rcNow.right - rcNow.left, rcNow.bottom - rcNow.top,
                    rcSuggested.left, rcSuggested.top,
                    rcSuggested.right - rcSuggested.left,
                    rcSuggested.bottom - rcSuggested.top);
        // Exact placement can be adjusted by the window manager, so allow a small
        // tolerance while still rejecting proportional scaling (which produced a
        // ~1.5x larger rect at a different origin).
        const bool bNear = labs(rcNow.left - rcSuggested.left) <= 8 &&
                           labs(rcNow.top - rcSuggested.top) <= 8 &&
                           labs((rcNow.right - rcNow.left) - (rcSuggested.right - rcSuggested.left)) <= 8 &&
                           labs((rcNow.bottom - rcNow.top) - (rcSuggested.bottom - rcSuggested.top)) <= 8;
        Chk("window follows the system-suggested rect", bNear, sz);
    }

    Chk("root marked for relayout after DPI change",
        pRoot->IsUpdateNeeded() || pm.IsUpdateNeeded(),
        pRoot->IsUpdateNeeded() ? "root dirty" : "manager dirty");

    // D4: an invalid DPI must be ignored, not divided by.
    const int nScaleSane = pm.GetDPIObj()->GetScale();
    RECT rcBad = { 0, 0, 100, 100 };
    ::SendMessageW(hWnd, WM_DPICHANGED, MAKEWPARAM(0, 0), reinterpret_cast<LPARAM>(&rcBad));
    {
        char sz[160];
        ::sprintf_s(sz, "scale after invalid DPI=%d", pm.GetDPIObj()->GetScale());
        Chk("invalid DPI is ignored (no crash, scale unchanged)",
            pm.GetDPIObj()->GetScale() == nScaleSane, sz);
    }

    ::printf("\nresult: %d pass / %d fail\n", g_nPass, g_nFail);
    ::DestroyWindow(hWnd);
    if (bInject)
    {
        Chk("injected-must-fail", false, "self-test: forced failure");
    }
    return (g_nFail == 0) ? 0 : 1;
}
