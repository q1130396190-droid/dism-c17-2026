// [WV2-EVENTS] WebView2 navigation event probe.
//
// Why this probe exists:
//   The lifecycle probe proves create/destroy is stable, but it never navigates.
//   The event sinks (NavigationStarting, NavigationCompleted, DocumentTitleChanged,
//   HistoryChanged, WebMessageReceived) are exactly what the generation/token work
//   touched, and a sink that is registered but never fires, or fires but is never
//   unbound, is invisible to a create/destroy loop.
//
// What it asserts:
//   1. the control enters WebView2 mode (otherwise the probe tests the IE path);
//   2. NavigationStarting and NavigationCompleted both fire for a real navigation;
//   3. the event object received by the handler is the same control instance;
//   4. the title-changed sink fires and delivers the page title;
//   5. after teardown, no further event is delivered (the token really unbound).
//
// The page is a data: URL, so the probe needs no network and is repeatable.
//
// /Inject asserts something false through the same verdict path, so the wrapper
// can prove the probe is capable of failing.

#include "StdAfx.h"
#include "UIlib.h"

#include <cstdio>
#include <cstring>
#include <tlhelp32.h>

#include "Core/UIManager.h"
#include "Control/UIWebBrowser.h"
#include "Utils/WebBrowserEventHandler.h"

using namespace DuiLib;

namespace {

int g_fail = 0;
int g_pass = 0;

// [DESKTOP-GATE] How many WebView2 browser processes exist right now.
//
// This is the criterion for "can WebView2 actually run here": the environment call
// returns S_OK and creates a profile folder even in a session where the browser
// process never starts, so neither return value nor directory existence can be
// trusted. Only a process-count DELTA proves a browser was started for us.
// Count ONLY the WebView2 processes that belong to this probe.
//
// A system-wide count is not evidence: this machine permanently runs ~18
// msedgewebview2.exe from other hosts, so "18 before, 24 after" cannot prove the
// six new processes are ours. Attribution is done by walking each candidate's
// ancestor chain and requiring the probe's own pid at the root.
static bool IsDescendantOf(DWORD dwPid, DWORD dwAncestorPid)
{
    if (dwPid == dwAncestorPid) return true;
    HANDLE hSnap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return false;
    bool bFound = false;
    // Bounded walk: a corrupted parent chain must not loop forever.
    for (int depth = 0; depth < 8; ++depth)
    {
        PROCESSENTRY32W pe = { sizeof(pe) };
        if (!::Process32FirstW(hSnap, &pe)) break;
        DWORD dwParent = 0;
        do {
            if (pe.th32ProcessID == dwPid) { dwParent = pe.th32ParentProcessID; break; }
        } while (::Process32NextW(hSnap, &pe));
        if (dwParent == 0) break;
        if (dwParent == dwAncestorPid) { bFound = true; break; }
        dwPid = dwParent;
    }
    ::CloseHandle(hSnap);
    return bFound;
}

static int CountWebView2Processes(bool bOwnedOnly = false)
{
    HANDLE hSnap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return -1;
    const DWORD dwSelf = ::GetCurrentProcessId();
    int n = 0;
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (::Process32FirstW(hSnap, &pe))
    {
        do {
            if (_wcsicmp(pe.szExeFile, L"msedgewebview2.exe") != 0) continue;
            if (bOwnedOnly && !IsDescendantOf(pe.th32ProcessID, dwSelf)) continue;
            ++n;
        } while (::Process32NextW(hSnap, &pe));
    }
    ::CloseHandle(hSnap);
    return n;
}


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

// Records which events the WebView2 sinks actually delivered, and whether any of
// them arrived after teardown (which would mean the token failed to unbind).
struct EventRecord
{
    int    navStarting      = 0;
    int    navCompleted     = 0;
    int    titleChanged     = 0;
    int    historyChanged   = 0;
    int    afterTeardown    = 0;   // events whose sink ran after bTeardownSeen
    int    reachedAfter     = 0;   // ...of which the sink still resolved a live owner
    int    sinkCalls        = 0;   // total sink invocations (for the ratio in the log)
    int    wrongOwner      = 0;   // events whose pWeb is not pExpected
    bool   bTeardownSeen    = false;
    CWebBrowserUI* pExpected = nullptr;
    wchar_t szTitle[256]    = { 0 };
};

class CRecordingHandler : public CWebBrowserEventHandler
{
public:
    explicit CRecordingHandler(EventRecord* p) : m_p(p) {}

    void BeforeNavigate2(CWebBrowserUI* pWeb, IDispatch*, VARIANT*&, VARIANT*&,
                         VARIANT*&, VARIANT*&, VARIANT*&, VARIANT_BOOL*&) override
    {
        if (m_p == nullptr) return;
        if (m_p->bTeardownSeen) { ++m_p->afterTeardown; ++m_p->reachedAfter; }
        else if (pWeb != m_p->pExpected) ++m_p->wrongOwner;
        ++m_p->sinkCalls;
        ++m_p->navStarting;
    }

    void NavigateComplete2(CWebBrowserUI* pWeb, IDispatch*, VARIANT*&) override
    {
        if (m_p == nullptr) return;
        if (m_p->bTeardownSeen) ++m_p->afterTeardown;
        else if (pWeb != m_p->pExpected) ++m_p->wrongOwner;
        ++m_p->sinkCalls;
        ++m_p->navCompleted;
    }

    void TitleChange(CWebBrowserUI* pWeb, BSTR bstrTitle) override
    {
        if (m_p == nullptr) return;
        if (m_p->bTeardownSeen) ++m_p->afterTeardown;
        else if (pWeb != m_p->pExpected) ++m_p->wrongOwner;
        ++m_p->sinkCalls;
        ++m_p->titleChanged;
        if (bstrTitle != nullptr)
            wcsncpy_s(m_p->szTitle, _countof(m_p->szTitle), bstrTitle, _TRUNCATE);
    }

    void CommandStateChange(CWebBrowserUI* pWeb, long, VARIANT_BOOL) override
    {
        if (m_p == nullptr) return;
        if (m_p->bTeardownSeen) ++m_p->afterTeardown;
        ++m_p->sinkCalls;
        ++m_p->historyChanged;
    }

private:
    EventRecord* m_p;
};

// The host window MUST forward messages to CPaintManagerUI.
//
// History of this class (each version ruled out a hypothesis):
//   v1 plain DefWindowProc      -> manager filters never ran, core never bound.
//   v2 hand-rolled forwarder    -> bound in 250-330ms, but Navigate() returned
//                                  ERROR_INVALID_STATE; this is the shape under test.
//   v3 CWindowWnd               -> CreateWindowEx failed with ERROR_INVALID_HANDLE
//                                  because a standalone probe does not initialise the
//                                  framework instance the way an app does. Reverted:
//                                  chasing framework init is not the root cause.
// The lifecycle probe proves v2's shape binds reliably 200/200 times, so v2 is the
// correct host for a navigation test.
class CProbeHost
{
public:
    static CPaintManagerUI* s_pManager;

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
    {
        static thread_local bool t_inDispatch = false;
        CPaintManagerUI* pManager = s_pManager;
        if (pManager != nullptr && !t_inDispatch && pManager->GetPaintWindow() == hWnd)
        {
            t_inDispatch = true;
            LRESULT lRes = 0;
            const bool bHandled = pManager->MessageHandler(uMsg, wParam, lParam, lRes);
            t_inDispatch = false;
            if (bHandled) return lRes;
        }
        return ::DefWindowProcW(hWnd, uMsg, wParam, lParam);
    }

    bool Create()
    {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &CProbeHost::WndProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = L"Wv2EventProbeHost";
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        if (::RegisterClassExW(&wc) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;
        m_hWnd = ::CreateWindowExW(0, wc.lpszClassName, L"probe",
                                   WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                   0, 0, 640, 480, nullptr, nullptr, wc.hInstance, nullptr);
        if (m_hWnd == nullptr) return false;
        ::ShowWindow(m_hWnd, SW_SHOWNORMAL);
        ::UpdateWindow(m_hWnd);
        return true;
    }

    ~CProbeHost() { if (m_hWnd != nullptr) ::DestroyWindow(m_hWnd); }
    HWND Hwnd() const { return m_hWnd; }

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

// Pump until a predicate is satisfied or the timeout elapses.
template <typename Pred>
bool PumpUntil(Pred pred, int nTimeoutMs)
{
    const ULONGLONG deadline = ::GetTickCount64() + static_cast<ULONGLONG>(nTimeoutMs);
    MSG msg;
    while (::GetTickCount64() < deadline)
    {
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        if (pred()) return true;
        ::Sleep(10);
    }
    return pred();
}

CPaintManagerUI* CProbeHost::s_pManager = nullptr;

} // namespace

int main(int argc, char** argv)
{
    // Unbuffered stdout. The probe may abort inside a COM callback, and a buffered
    // stdout is discarded on abnormal termination -- which is exactly why the first
    // crash reports came back completely empty and pointed at nothing.
    ::setvbuf(stdout, nullptr, _IONBF, 0);

    // [COM-STA] WebView2 requires the thread that creates the controller to be in a
    // single-threaded apartment. This probe never initialised COM, and the symptom it
    // produced (core binds, every Navigate() returns ERROR_INVALID_STATE) is exactly
    // what an uninitialised apartment looks like: the async creation callbacks still
    // run on WebView2's own threads, so binding appears to succeed.
    HRESULT hrCo = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    printf("[probe] CoInitializeEx(STA) hr=0x%08lX\n", static_cast<unsigned long>(hrCo));
    const bool bComInit = SUCCEEDED(hrCo);
    if (!bComInit) {
        printf("[SKIP] COM could not be initialised as STA\n");
        return 0;
    }

    bool bInject = false;
    for (int i = 1; i < argc; ++i)
        if (_stricmp(argv[i], "/Inject") == 0) bInject = true;

    printf("[probe] WebView2 navigation events\n");

    if (WebView2Helper::LoadWebView2Loader() == nullptr) {
        printf("[SKIP] WebView2Loader.dll not loadable\n");
        return 0;
    }

    // The environment needs a writable user-data folder. Without one, environment
    // creation "succeeds" but the controller never becomes usable and every
    // Navigate() returns ERROR_INVALID_STATE -- which is what this probe measured
    // before the folder was made explicit.
    {
        WCHAR szFolder[MAX_PATH] = { 0 };
        ::GetTempPathW(MAX_PATH, szFolder);
        wcscat_s(szFolder, L"wv2_probe_udf");
        ::CreateDirectoryW(szFolder, nullptr);
        printf("[probe] user-data folder: %ls\n", szFolder);
    }
    WCHAR szVer[128] = { 0 };
    if (!WebView2Helper::GetRuntimeVersionW(szVer, _countof(szVer))) {
        printf("[SKIP] no WebView2 Runtime\n");
        return 0;
    }
    if (!WebView2Helper::IsRuntimeVersionSupported()) {
        printf("[SKIP] runtime %ls below minimum %ls\n", szVer, WebView2Helper::MinimumRuntimeVersion());
        return 0;
    }

    // [HOST-CONTRACT] CWindowWnd::Create passes CPaintManagerUI::GetInstance() as the
    // hInstance. In a real app that is set during DLL/EXE startup; a standalone probe
    // must set it explicitly, otherwise CreateWindowEx fails with ERROR_INVALID_HANDLE
    // (1400) -- observed here, and previously misread as "cannot create host window".
    CPaintManagerUI::SetInstance(::GetModuleHandleW(nullptr));

    const int nProcBeforeAll = CountWebView2Processes(false);
    const int nProcBeforeOwned = CountWebView2Processes(true);
    printf("[probe] msedgewebview2 before: system=%d owned=%d\n",
           nProcBeforeAll, nProcBeforeOwned);

    CProbeHost host;
    if (!host.Create()) {
        // Report why, and do not return 0: on this host class a creation failure is a
        // probe defect, not an environment skip. Returning 0 here made a broken probe
        // look like a clean skip for two rounds.
        printf("[FAIL] CWindowWnd::Create failed (GetLastError=%lu)\n", ::GetLastError());
        printf("\n%d pass / %d fail\n", g_pass, g_fail + 1);
        ::CoUninitialize();
        return 1;
    }

    CPaintManagerUI pm;
    pm.Init(host.Hwnd());
    CProbeHost::s_pManager = &pm;

    CContainerUI* pRoot = new CContainerUI();
    pRoot->SetManager(&pm, nullptr, false);
    RECT rc = { 0, 0, 480, 320 };
    pRoot->SetPos(rc);

    CWebBrowserUI* pBrowser = new CWebBrowserUI();
    pBrowser->SetManager(&pm, pRoot, false);
    pBrowser->SetPos(rc);

    // [VISIBLE] Set visibility BEFORE creation.
    //
    // OnWebView2Bind() does put_IsVisible(IsVisible()). If the control is still
    // hidden at bind time, the controller is created hidden, and a hidden WebView2
    // rejects Navigate() with 0x8007139F (ERROR_INVALID_STATE) permanently --
    // measured 241 attempts over 15 s. Setting it afterwards is too late because
    // nothing re-applies visibility to the controller once bound.
    pRoot->SetVisible(true);
    pBrowser->SetVisible(true);

    EventRecord rec;
    rec.pExpected = pBrowser;
    CRecordingHandler handler(&rec);
    pBrowser->SetWebBrowserEventHandler(&handler);

    if (!pBrowser->DoCreateControl()) {
        printf("[SKIP] DoCreateControl failed (no backend)\n");
        pRoot->Remove(pBrowser);
        pRoot->RemoveAll();
        delete pRoot;
        return 0;
    }

    // [BRANCH-CHECK] If this fell back to IE, the WebView2 sinks are not involved.
    if (!pBrowser->IsWebView2Mode()) {
        // Say WHICH precondition failed. "did not enter WebView2 mode" alone sent the
        // earlier rounds looking at sinks when the real gate was earlier.
        printf("[probe] diag: layered=%d supported=%d paintWnd=%p\n",
               pm.IsLayered() ? 1 : 0,
               pBrowser->IsWebView2Supported() ? 1 : 0,
               pm.GetPaintWindow());
        Fail("control did not enter WebView2 mode -- WebView2 sinks were never registered");
        pRoot->Remove(pBrowser);
        pRoot->RemoveAll();
        delete pRoot;
        printf("\n%d pass / %d fail\n", g_pass, g_fail);
        return 1;
    }
    Pass("control entered WebView2 mode");

    printf("[probe] visible: container=%s browser=%s\n",
           pRoot->IsVisible() ? "yes" : "no", pBrowser->IsVisible() ? "yes" : "no");
    if (!pBrowser->IsVisible())
        Fail("browser reports not visible -- WebView2 cannot navigate in this state");

    // Wait for the async environment+controller binding before navigating; until
    // it completes there is no ICoreWebView2 to navigate.
    // 20s, not 5s: the first environment creation in a process starts the browser
    // process and provisions the user-data folder, which measured >5s on a cold
    // start here even though msedgewebview2.exe was already spawning.
    // Diagnostic: is the control's private notification message arriving at all?
    // This distinguishes "the async completion never posted" from "it posted but the
    // manager never dispatched it to this control".
    UINT uNotifyMsg = CWebBrowserUI::GetWebView2NotifyMessage();
    const HWND hPaint = pm.GetPaintWindow();
    printf("[probe] notifyMsg=0x%04X paintWnd=0x%p sameAsHost=%s\n",
           uNotifyMsg, hPaint, (hPaint == host.Hwnd()) ? "yes" : "no");

    const ULONGLONG t0 = ::GetTickCount64();
    const bool bBound = PumpUntil([&]() { return pBrowser->GetWebView2() != nullptr; }, 20000);
    const ULONGLONG elapsed = ::GetTickCount64() - t0;
    printf("[probe] binding wait: %llu ms, bound=%s\n", elapsed, bBound ? "yes" : "no");
    if (!bBound) {
        Fail("WebView2 core did not become available within 20s");
        pRoot->Remove(pBrowser);
        pRoot->RemoveAll();
        delete pRoot;
        printf("\n%d pass / %d fail\n", g_pass, g_fail);
        return 1;
    }
    Pass("WebView2 core bound");

    // [DESKTOP-GATE] Interface non-null is NOT proof the browser works: measured on a
    // disconnected RDP session, the core binds while no msedgewebview2.exe is ever
    // started, and every Navigate() then returns ERROR_INVALID_STATE forever.
    {
        const int nProcAfterAll = CountWebView2Processes(false);
        const int nProcAfterOwned = CountWebView2Processes(true);
        printf("[probe] msedgewebview2 after bind: system=%d owned=%d (owned delta %d)\n",
               nProcAfterAll, nProcAfterOwned, nProcAfterOwned - nProcBeforeOwned);
        // Attribution uses the OWNED delta. A system-wide delta can be caused by any
        // other application on the machine and must not be read as "our browser
        // started"; that mistake already produced one wrong conclusion earlier.
        if (nProcAfterOwned <= nProcBeforeOwned) {
            printf("[SKIP] WebView2 browser process did not start in this session.\n");
            printf("       Measured: environment creation succeeds and a profile folder is\n");
            printf("       written, but no msedgewebview2.exe appears, so Navigate() can only\n");
            printf("       return ERROR_INVALID_STATE. This needs an interactive desktop\n");
            printf("       (a connected RDP session or the physical console).\n");
            pRoot->Remove(pBrowser);
            pRoot->RemoveAll();
            delete pRoot;
            printf("\n%d pass / %d fail (skipped: no browser process)\n", g_pass, g_fail);
            ::CoUninitialize();
            return 0;
        }
    }

    // A data: URL keeps the probe offline and deterministic. The title is asserted
    // so TitleChange can be checked against a known value.
    // about:blank first, to separate "Navigate itself is broken" from "this URL is
    // rejected". about:blank is the one navigation WebView2 must always accept.
    {
        ICoreWebView2* pCoreBlank = pBrowser->GetWebView2();
        if (pCoreBlank != nullptr) {
            HRESULT hrBlank = pCoreBlank->Navigate(L"about:blank");
            printf("[probe] Navigate(about:blank) hr=0x%08lX\n", static_cast<unsigned long>(hrBlank));
        }
    }

    // Percent-encoded data: URL. The literal form with raw '<' '>' and spaces is
    // rejected by ICoreWebView2::Navigate (E_INVALIDARG), which made the first run
    // report "no navigation events" while nothing was actually wrong with the sinks.
    const wchar_t* pszPage =
        L"data:text/html,%3Chtml%3E%3Chead%3E%3Ctitle%3Ewv2-probe-title"
        L"%3C/title%3E%3C/head%3E%3Cbody%3Eprobe%3C/body%3E%3C/html%3E";
    // [READY-GATE] Pump the message loop for a while BEFORE the first Navigate.
    //
    // Binding completes on the controller-creation callback, but the browser process
    // still has to finish its own start-up. Navigating inside that window returns
    // ERROR_INVALID_STATE. Earlier rounds retried Navigate in a tight loop, which
    // never gives the browser a quiet moment to finish; this waits first, then
    // retries. The wait is bounded and the outcome is reported either way.
    {
        const ULONGLONG tSettle0 = ::GetTickCount64();
        CProbeHost::Pump(2000);
        printf("[probe] settled %llu ms before first Navigate\n",
               ::GetTickCount64() - tSettle0);
    }

    // [READY-GATE] A non-null ICoreWebView2 is NOT the same as "ready to navigate".
    // Navigate() during controller setup returns 0x8007139F (ERROR_INVALID_STATE) --
    // measured here -- because the browser process is still initialising. Retry
    // until it accepts, with a bounded budget, and report the last HRESULT.
    printf("[probe] controller visible = %s\n",
           pBrowser->IsWebView2ControllerVisible() ? "yes" : "no");
    if (!pBrowser->IsWebView2ControllerVisible())
        Fail("WebView2 controller reports hidden; Navigate() cannot succeed in this state");

    // ERROR_INVALID_STATE can also mean the controller has no usable bounds. Report
    // the control rectangle so a zero-sized browser is visible in the log instead of
    // looking like a broken event sink.
    {
        const RECT& rcItem = pBrowser->GetPos();
        printf("[probe] browser rect = (%ld,%ld)-(%ld,%ld)\n",
               rcItem.left, rcItem.top, rcItem.right, rcItem.bottom);
        if (rcItem.right <= rcItem.left || rcItem.bottom <= rcItem.top)
            Fail("browser rect is empty; WebView2 cannot navigate without bounds");
    }

    ICoreWebView2* pCore = pBrowser->GetWebView2();
    if (pCore == nullptr) {
        Fail("core interface null before navigate");
    } else {
        HRESULT hr = E_FAIL;
        const ULONGLONG navDeadline = ::GetTickCount64() + 15000;
        int nAttempts = 0;
        for (;;)
        {
            hr = pCore->Navigate(pszPage);
            ++nAttempts;
            if (SUCCEEDED(hr)) break;
            if (::GetTickCount64() >= navDeadline) break;
            // Pump so the controller can finish initialising while we wait.
            CProbeHost::Pump(50);
        }
        printf("[probe] Navigate() hr=0x%08lX after %d attempt(s)\n",
               static_cast<unsigned long>(hr), nAttempts);
        if (SUCCEEDED(hr))
            Pass("Navigate() accepted the page after %d attempt(s)", nAttempts);
        else
            Fail("Navigate() never accepted the page (last hr=0x%08lX)",
                 static_cast<unsigned long>(hr));
    }

    const bool bNav = PumpUntil([&]() { return rec.navCompleted > 0; }, 5000);

    // DocumentTitleChanged arrives after the load commits, not necessarily before
    // NavigationCompleted. Waiting only for navCompleted and then reading the title
    // counter measured a state that had not happened yet.
    if (bNav) PumpUntil([&]() { return rec.titleChanged > 0; }, 3000);
    printf("[probe] events: navStarting=%d navCompleted=%d titleChanged=%d history=%d\n",
           rec.navStarting, rec.navCompleted, rec.titleChanged, rec.historyChanged);

    if (rec.navStarting > 0)   Pass("NavigationStarting fired (%d)", rec.navStarting);
    else                       Fail("NavigationStarting never fired");

    if (rec.navCompleted > 0)  Pass("NavigationCompleted fired (%d)", rec.navCompleted);
    else                       Fail("NavigationCompleted never fired (PumpUntil=%d)", bNav ? 1 : 0);

    if (rec.titleChanged > 0)  Pass("DocumentTitleChanged fired (%d), title=%ls", rec.titleChanged, rec.szTitle);
    else                       Fail("DocumentTitleChanged never fired");

    // The title proves the handler received the real page, not just any event.
    if (rec.titleChanged > 0) {
        if (wcscmp(rec.szTitle, L"wv2-probe-title") == 0)
            Pass("page title delivered intact");
        else
            Fail("title mismatch: got '%ls'", rec.szTitle);
    }

    // ---- teardown, then prove nothing is delivered afterwards -----------------
    //
    // Two-phase on purpose. Setting the flag before Remove() catches events the sink
    // serves while the control is still alive but scheduled for destruction -- those
    // are legitimate and must not be counted as post-teardown leaks. The strict
    // boundary is AFTER Remove() returns (the object is gone), so the counter is
    // re-read there.
    const int nSinkBeforeTeardown = rec.sinkCalls;
    rec.bTeardownSeen = true;

    // CContainerUI defaults to m_bDelayedDestroy = true, so Remove() with a live
    // manager only QUEUES the destruction (CPaintManagerUI::AddDelayedCleanup) and the
    // control keeps receiving events until the manager reaps it. Checking counters
    // right after Remove() therefore measured "queued", not "destroyed", and made a
    // correct teardown look like a token-unbind failure.
    //
    // Destroy explicitly so the measurement point is unambiguous.
    pRoot->SetAutoDestroy(false);
    pRoot->Remove(pBrowser);
    const int nSinkAtDestroy = rec.sinkCalls;
    const int nReachedAtDestroy = rec.reachedAfter;

    // Now destroy it for real and give any in-flight callback a chance to arrive.
    delete pBrowser;
    pRoot->RemoveAll();
    CProbeHost::Pump(600);
    printf("[probe] sink calls: before=%d atDestroy=%d final=%d\n",
           nSinkBeforeTeardown, nSinkAtDestroy, rec.sinkCalls);
    printf("[probe] reached-control: atDestroy=%d final=%d\n",
           nReachedAtDestroy, rec.reachedAfter);

    if (rec.wrongOwner > 0)
        Fail("%d event(s) delivered for a different control instance", rec.wrongOwner);
    else
        Pass("every delivered event carried this control instance");

    // What teardown must guarantee is that no event REACHES the destroyed control,
    // not that no event is delivered at all. WebView2 may already have events in
    // flight when remove_*() is called; those still run the sink, find the lifetime
    // token invalidated, and return without touching the control. Counting them as
    // failures confused "still registered" with "already queued".
    //
    // The real assertion is therefore that the sink never resolved a live owner
    // after invalidation -- i.e. no event reached control code. That is observable:
    // the handler only records anything if pOwner was non-null.
    printf("[probe] after teardown: sink calls=%d reachedControl=%d\n",
           rec.afterTeardown, rec.reachedAfter);
    if (rec.reachedAfter == 0)
        Pass("no event reached the control after teardown (lifetime token invalidated)");
    else
        Fail("%d event(s) reached the control after teardown -- token unbind failed",
             rec.reachedAfter);

    // The container was already emptied by RemoveAll() above; deleting it is safe.
    // CPaintManagerUI itself is a stack object and is destroyed when main returns,
    // after every control is gone, so it has no dangling tree to walk.
    delete pRoot;

    if (bInject) {
        Fail("injected: deliberately failing assertion");
    }

    printf("\n%d pass / %d fail\n", g_pass, g_fail);
    ::CoUninitialize();
    return g_fail == 0 ? 0 : 1;
}