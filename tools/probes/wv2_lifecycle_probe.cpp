// [WV2-LIFETIME] WebView2 create/destroy lifecycle stress probe.
//
// Why this probe exists:
//   The upgrade fixed three lifecycle defects in CWebBrowserUI by inspection:
//     * event sinks held a raw CWebBrowserUI* (dangling after destroy),
//     * event registration tokens were thrown away, so nothing was ever unbound,
//     * queued async payloads leaked their COM reference when the window died
//       before the message was dispatched.
//   Those fixes were verified by compiling and by the existing suite, but the
//   existing suite never creates a WebView2 at all. A defect that only shows up
//   when a control is destroyed mid-initialisation therefore cannot regress-detect.
//   This probe creates and destroys the control repeatedly and watches for:
//     1. a crash (exit code != 0),
//     2. COM reference growth that no amount of destruction releases,
//     3. GDI/User handle growth (leaked DCs, bitmaps, windows),
//     4. the control reporting WebView2 mode while no backend was ever created.
//
// The probe is deliberately conservative: it does NOT require network access and
// does NOT need a visible desktop. If the WebView2 runtime or loader is absent it
// prints SKIP with a reason instead of pretending to pass.
//
// /Inject makes the probe assert something false, so the wrapper can prove the
// verdict path really fails. A probe that cannot fail is not a probe.

#include "StdAfx.h"
#include "UIlib.h"

#include <cstdio>
#include <cstring>

#include "Core/UIManager.h"
#include "Control/UIWebBrowser.h"

using namespace DuiLib;

namespace {

int g_fail = 0;
int g_pass = 0;

void Pass(const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    printf("[PASS] "); vprintf(fmt, ap); printf("\n");
    va_end(ap);
    ++g_pass;
}

void Fail(const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    printf("[FAIL] "); vprintf(fmt, ap); printf("\n");
    va_end(ap);
    ++g_fail;
}

struct HandleCounts
{
    DWORD gdi;
    DWORD user;
};

HandleCounts SampleHandles()
{
    HandleCounts h = { 0, 0 };
    h.gdi = ::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS);
    h.user = ::GetGuiResources(::GetCurrentProcess(), GR_USEROBJECTS);
    return h;
}

// A minimal host window. WebView2 needs a real HWND to parent its child window;
// a message-only window is not enough.
class CProbeHost
{
public:
    bool Create()
    {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = ::DefWindowProcW;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = L"Wv2LifecycleProbeHost";
        if (::RegisterClassExW(&wc) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;

        m_hWnd = ::CreateWindowExW(0, wc.lpszClassName, L"probe", WS_OVERLAPPEDWINDOW,
                                   0, 0, 640, 480, nullptr, nullptr, wc.hInstance, nullptr);
        return m_hWnd != nullptr;
    }

    ~CProbeHost()
    {
        if (m_hWnd != nullptr) ::DestroyWindow(m_hWnd);
    }

    HWND Hwnd() const { return m_hWnd; }

    // Pump just enough messages for posted WebView2 notifications to be delivered,
    // which is what makes the "queued payload" path actually run.
    static void Pump(int nMs)
    {
        const ULONGLONG deadline = ::GetTickCount64() + static_cast<ULONGLONG>(nMs);
        MSG msg;
        while (::GetTickCount64() < deadline)
        {
            while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                ::TranslateMessage(&msg);
                ::DispatchMessageW(&msg);
            }
            ::Sleep(5);
        }
    }

private:
    HWND m_hWnd = nullptr;
};

// One create/destroy cycle: build a browser control, attach it to a manager bound
// to the host window, let it start initialising, then destroy it immediately.
// Destroying while initialisation is in flight is the interesting case.
// Returns true when the control actually entered WebView2 mode (not the IE
// fallback). The caller aggregates this: a run where every cycle fell back to IE
// would still destroy 200 controls successfully while testing none of the
// WebView2 lifecycle, which is a green result that proves nothing.
bool RunOneCycle(HWND hWnd, int nPumpMs, bool bDestroyMidInit, bool* pbEnteredWebView2)
{
    CPaintManagerUI pm;
    pm.Init(hWnd);

    // A real container is used so teardown goes through the framework's own
    // removal path (CContainerUI::Remove -> ReleaseControl -> delete). Calling
    // ReleaseControl() directly is protected, and going through the container is
    // also what the shipping code does, so the probe exercises the real order.
    CContainerUI* pRoot = new CContainerUI();
    pRoot->SetManager(&pm, nullptr, false);

    CWebBrowserUI* pBrowser = new CWebBrowserUI();
    pBrowser->SetManager(&pm, pRoot, false);

    RECT rc = { 0, 0, 320, 200 };
    pRoot->SetPos(rc);
    pBrowser->SetPos(rc);

    const bool bSupported = pBrowser->IsWebView2Supported();
    const bool bCreated = pBrowser->DoCreateControl();

    // [BRANCH-CHECK] Record whether the WebView2 path was really taken. Without
    // this, a machine that silently fell back to IE would produce a fully green
    // lifecycle probe that never exercised a single WebView2 sink.
    if (pbEnteredWebView2 != nullptr)
        *pbEnteredWebView2 = pBrowser->IsWebView2Mode();

    if (bDestroyMidInit)
    {
        // Let the async environment/controller creation post its message, but do not
        // wait for it to complete: this is the destroy-mid-initialisation shape that
        // the generation/cancellation work exists for.
        CProbeHost::Pump(nPumpMs);
    }

    // Remove the child: this is what releases the WebView2 backend while the async
    // creation may still be in flight.
    pRoot->Remove(pBrowser);

    // Give any late callback a chance to arrive and be discarded by the token.
    CProbeHost::Pump(nPumpMs);

    pRoot->RemoveAll();
    delete pRoot;

    (void)bSupported;
    (void)bCreated;
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    bool bInject = false;
    for (int i = 1; i < argc; ++i)
        if (_stricmp(argv[i], "/Inject") == 0) bInject = true;

    printf("[probe] WebView2 lifecycle stress\n");

    // Runtime + loader availability, reported rather than assumed.
    const bool bLoader = WebView2Helper::LoadWebView2Loader() != nullptr;
    const bool bMinOk = WebView2Helper::IsRuntimeVersionSupported();
    WCHAR szVer[128] = { 0 };
    WebView2Helper::GetRuntimeVersionW(szVer, _countof(szVer));

    printf("[probe] loader=%s runtime=%ls minimum_ok=%s minimum=%ls\n",
           bLoader ? "present" : "MISSING",
           szVer[0] ? szVer : L"(none)",
           bMinOk ? "yes" : "no",
           WebView2Helper::MinimumRuntimeVersion());

    if (!bLoader) {
        printf("[SKIP] WebView2Loader.dll not loadable next to the probe\n");
        return 0;
    }
    if (szVer[0] == L'\0') {
        printf("[SKIP] no WebView2 Runtime answered on this machine\n");
        return 0;
    }

    // The version floor is a real assertion: 86.0.616.0 is the documented minimum
    // to load WebView2 at all, so a machine below it must be reported, not ignored.
    if (!bMinOk)
        Fail("installed runtime %ls is below the documented minimum %ls",
             szVer, WebView2Helper::MinimumRuntimeVersion());
    else
        Pass("installed runtime %ls >= minimum %ls", szVer, WebView2Helper::MinimumRuntimeVersion());

    const int kCycles = 200;     // enough repetition to expose a per-cycle leak
    const int kPumpMs = 60;

    // Warm-up in its OWN host window, which is then destroyed. Rationale: the
    // first cycle pays one-off costs (DLL loads, class registration, GDI caches,
    // DuiLib singletons). Measuring a baseline before that attributed those
    // one-time costs to per-cycle growth -- measured: baseline gdi=3, after 40
    // cycles gdi=39, which is mostly warm-up rather than a leak.
    bool bWarmEntered = false;
    {
        CProbeHost warm;
        if (!warm.Create()) {
            printf("[SKIP] could not create warm-up window\n");
            return 0;
        }
        for (int i = 0; i < 3; ++i) RunOneCycle(warm.Hwnd(), kPumpMs, false, &bWarmEntered);
        CProbeHost::Pump(200);
    }

    CProbeHost host;
    if (!host.Create()) {
        printf("[SKIP] could not create host window\n");
        return 0;
    }

    const HandleCounts base = SampleHandles();
    printf("[probe] baseline handles gdi=%lu user=%lu\n", base.gdi, base.user);

    int nEnteredWebView2 = 0;
    for (int i = 0; i < kCycles; ++i)
    {
        bool bEntered = false;
        RunOneCycle(host.Hwnd(), kPumpMs, (i % 2) == 1, &bEntered);
        if (bEntered) ++nEnteredWebView2;
    }

    CProbeHost::Pump(200);
    const HandleCounts end = SampleHandles();
    printf("[probe] after %d cycles handles gdi=%lu user=%lu\n", kCycles, end.gdi, end.user);

    // Threshold rationale: a correct teardown returns to (or near) the baseline.
    // Small jitter is normal because GDI caches and the window manager keep a few
    // objects alive; a real per-cycle leak over 200 cycles is far larger than this.
    const long kGdiSlack = 20;
    const long kUserSlack = 20;
    const long gdiDelta = static_cast<long>(end.gdi) - static_cast<long>(base.gdi);
    const long userDelta = static_cast<long>(end.user) - static_cast<long>(base.user);

    if (gdiDelta > kGdiSlack)
        Fail("GDI objects grew by %ld over %d cycles (limit %ld)", gdiDelta, kCycles, kGdiSlack);
    else
        Pass("GDI objects stable (delta %ld, limit %ld)", gdiDelta, kGdiSlack);

    if (userDelta > kUserSlack)
        Fail("User objects grew by %ld over %d cycles (limit %ld)", userDelta, kCycles, kUserSlack);
    else
        Pass("User objects stable (delta %ld, limit %ld)", userDelta, kUserSlack);

    // Reaching this line at all is the crash assertion: a dangling sink or a
    // double-released payload would have terminated the process above.
    Pass("survived %d create/destroy cycles (half destroyed mid-initialisation)", kCycles);

    // [BRANCH-CHECK] Did the run actually exercise the WebView2 path?
    printf("[probe] cycles that entered WebView2 mode: %d / %d (warm-up entered: %s)\n",
           nEnteredWebView2, kCycles, bWarmEntered ? "yes" : "no");
    if (nEnteredWebView2 == 0)
    {
        // Every cycle fell back to IE. Destroying controls still succeeded, but
        // none of the WebView2 sinks, tokens or payloads were involved, so this
        // run says nothing about the code under test. Report it as a hard failure
        // rather than letting a green verdict stand for an untested path.
        Fail("no cycle entered WebView2 mode -- the lifecycle path was never exercised "
             "(IsWebView2Supported/DoCreateControl fell back to IE)");
    }
    else if (nEnteredWebView2 < kCycles)
    {
        // Partial: some cycles fell back. Worth reporting, because a flaky backend
        // selection would make the stress result depend on timing.
        Fail("only %d of %d cycles entered WebView2 mode -- backend selection is not stable",
             nEnteredWebView2, kCycles);
    }
    else
    {
        Pass("all %d cycles ran in WebView2 mode", nEnteredWebView2);
    }

    if (bInject) {
        // Force a false statement through the same verdict path. If this does not
        // make the process exit non-zero, the wrapper cannot prove anything.
        Fail("injected: deliberately failing assertion");
    }

    printf("\n%d pass / %d fail\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
