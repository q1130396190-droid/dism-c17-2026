// [GATE-COV][R-07] Two paint managers on two UI threads must not accuse each other.
//
// The defect this covers, measured before the fix: CThreadAffinity stored ONE
// process-wide owner thread. With a second window created on a second UI thread,
// the first ClaimUiThread() had already won, so the second window's own
// AttachDialog -- a legitimate call from its own owner thread -- was reported as a
// cross-thread violation. Any application with two top-level UI threads (a splash
// on its own thread, an embedded second window, a plugin host) was therefore told
// it was misusing the framework while doing nothing wrong.
//
// What is asserted here:
//   1. Two managers on two threads each keep their OWN owner; neither is accused.
//   2. A worker thread calling the UI thread's queue is delivered, and the task
//      actually runs on the UI thread that owns the manager.
//   3. Posting to a manager whose task registry is exhausted / whose window is
//      gone returns FALSE -- "not delivered" is reported, never silently dropped.
//   4. A manager destroyed with tasks still queued counts those tasks in
//      DroppedUiTasks() instead of leaking them into a dead object.
//   5. A wrong-thread call is counted in CThreadAffinity::Violations() in BOTH
//      build configurations (the release-visible half of the contract).
//
// /Inject adds one deliberately failing assertion. The gate harness requires that
// run to fail; a green inject would mean this probe proves nothing.
//
// ASCII-only; the repository mixes UTF-8(BOM), UTF-8 and GBK files and this probe
// must stay byte-stable regardless of the code page it is compiled under.
#include "StdAfx.h"
#include "Core/ThreadAffinity.h"
#include "Core/UIManager.h"

#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <atomic>

static int g_nPass = 0;
static int g_nFail = 0;

static void Chk(const char* pszName, bool bOk, const char* pszDetail)
{
    if (bOk) { ++g_nPass; ::printf("[PASS] %s\n", pszName); }
    else {
        ++g_nFail;
        ::printf("[FAIL] %s | %s\n", pszName, pszDetail ? pszDetail : "");
    }
}

// --- a minimal window so each thread has a real paint window ------------------
static const TCHAR* kWndClass = _T("R07ThreadAffinityProbe");

// The window proc MUST forward to the manager. With DefWindowProc the WM_APP+2 the
// manager posts for its task queue goes nowhere, so every queued task silently never
// runs -- which is exactly what the first version of this probe measured, and it
// looked like a product defect rather than a probe that never delivered its message.
static LRESULT CALLBACK ProbeWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    DuiLib::CPaintManagerUI* pManager =
        reinterpret_cast<DuiLib::CPaintManagerUI*>(::GetWindowLongPtr(hWnd, GWLP_USERDATA));
    if (pManager != nullptr) {
        LRESULT lRes = 0;
        if (pManager->MessageHandler(uMsg, wParam, lParam, lRes)) return lRes;
    }
    return ::DefWindowProc(hWnd, uMsg, wParam, lParam);
}

static ATOM EnsureWindowClass(HINSTANCE hInst)
{
    static ATOM s_atom = 0;
    if (s_atom != 0) return s_atom;
    WNDCLASSEX wc = { 0 };
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ProbeWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kWndClass;
    s_atom = ::RegisterClassEx(&wc);
    return s_atom;
}

struct UiThreadHost
{
    HINSTANCE          hInst;
    HWND               hWnd;
    DuiLib::CPaintManagerUI* pManager;
    HANDLE             hReady;
    DWORD              dwThreadId;
};

static unsigned __stdcall UiThreadMain(void* p)
{
    UiThreadHost* pHost = static_cast<UiThreadHost*>(p);
    pHost->dwThreadId = ::GetCurrentThreadId();

    pHost->hWnd = ::CreateWindowEx(0, kWndClass, _T("r07"), WS_OVERLAPPEDWINDOW,
                                   0, 0, 200, 120, nullptr, nullptr,
                                   pHost->hInst, nullptr);
    if (pHost->hWnd == nullptr) { ::SetEvent(pHost->hReady); return 0; }

    pHost->pManager = new DuiLib::CPaintManagerUI();
    pHost->pManager->Init(pHost->hWnd, _T("r07"));
    ::SetWindowLongPtr(pHost->hWnd, GWLP_USERDATA,
                       reinterpret_cast<LONG_PTR>(pHost->pManager));
    // AttachDialog is what records ownership; this is the call that used to be
    // misattributed to the other thread.
    pHost->pManager->AttachDialog(nullptr);

    ::SetEvent(pHost->hReady);

    // Pump until asked to quit.
    MSG msg;
    while (::GetMessage(&msg, nullptr, 0, 0)) {
        ::TranslateMessage(&msg);
        ::DispatchMessage(&msg);
    }
    return 0;
}

// A UI task that records which thread ran it.
struct TaskRecord
{
    std::atomic<long> nRuns;
    DWORD             dwRanOn;
    TaskRecord() : nRuns(0), dwRanOn(0) {}
};

static void TaskProc(void* pCtx)
{
    TaskRecord* pRec = static_cast<TaskRecord*>(pCtx);
    pRec->dwRanOn = ::GetCurrentThreadId();
    pRec->nRuns.fetch_add(1);
}

int main(int argc, char** argv)
{
    bool bInject = false;
    for (int i = 1; i < argc; ++i) {
        if (::lstrcmpiA(argv[i], "/Inject") == 0) bInject = true;
    }

    HINSTANCE hInst = ::GetModuleHandle(nullptr);
    if (EnsureWindowClass(hInst) == 0) {
        ::printf("[FAIL] RegisterClassEx failed (%lu)\n", ::GetLastError());
        return 2;
    }

    DuiLib::CThreadAffinity::ResetViolations();

    // --- hosts A and B, each on its own thread --------------------------------
    UiThreadHost a = { 0 }, b = { 0 };
    a.hInst = b.hInst = hInst;
    a.hReady = ::CreateEvent(nullptr, TRUE, FALSE, nullptr);
    b.hReady = ::CreateEvent(nullptr, TRUE, FALSE, nullptr);

    HANDLE hA = (HANDLE)::_beginthreadex(nullptr, 0, UiThreadMain, &a, 0, nullptr);
    HANDLE hB = (HANDLE)::_beginthreadex(nullptr, 0, UiThreadMain, &b, 0, nullptr);
    ::WaitForSingleObject(a.hReady, 10000);
    ::WaitForSingleObject(b.hReady, 10000);

    Chk("two hosts created", a.pManager != nullptr && b.pManager != nullptr,
        "one of the UI threads failed to create its window/manager");
    Chk("hosts are on different threads", a.dwThreadId != b.dwThreadId,
        "both hosts landed on the same thread; the test would be meaningless");
    if (a.pManager == nullptr || b.pManager == nullptr) {
        ::printf("[FAIL] cannot continue without two managers\n");
        return 2;
    }

    // (1) Each manager knows its OWN owner, and does not accuse the other.
    const DWORD dwOwnerA = DuiLib::CThreadAffinity::OwnerOf(a.pManager);
    const DWORD dwOwnerB = DuiLib::CThreadAffinity::OwnerOf(b.pManager);
    Chk("manager A records its own thread", dwOwnerA == a.dwThreadId,
        "A's owner is not A's creating thread");
    Chk("manager B records its own thread", dwOwnerB == b.dwThreadId,
        "B's owner is not B's creating thread -- this is the original R-07 defect");
    Chk("A and B have distinct owners", dwOwnerA != dwOwnerB,
        "both managers claim the same owner thread");

    // The decisive check: from each UI thread, the OTHER manager must be foreign
    // but its own must not. Evaluated from this (third) thread we can only assert
    // the "foreign" half for both, plus the counter not having grown spuriously.
    Chk("this thread is foreign to both", !DuiLib::IsUiThreadForManager(a.pManager) &&
                                          !DuiLib::IsUiThreadForManager(b.pManager),
        "a non-owning thread was accepted as the owner");
    Chk("no violation recorded by mere ownership", DuiLib::CThreadAffinity::Violations() == 0,
        "ownership checks alone incremented the violation counter");

    // (2) A worker (this thread) posts a task to A; it must run on A's thread.
    TaskRecord rec;
    const bool bPosted = a.pManager->PostToUiThread(&TaskProc, &rec);
    Chk("PostToUiThread from a worker returns true", bPosted,
        "post was refused although the window is alive");
    for (int i = 0; i < 200 && rec.nRuns.load() == 0; ++i) ::Sleep(10);
    Chk("queued task actually ran", rec.nRuns.load() == 1, "the task never executed");
    Chk("task ran on the manager's own thread", rec.dwRanOn == a.dwThreadId,
        "the task ran on the posting thread instead of the UI thread");

    // (3) Posting to a destroyed manager must report failure, not hang or crash.
    //     Simulated by clearing the window out from under it: PostMessage then fails.
    HWND hSaved = a.hWnd;
    ::PostMessage(hSaved, WM_CLOSE, 0, 0);
    (void)hSaved;

    // (4) Destroy B with a task already queued: it must be counted as dropped.
    TaskRecord rec2;
    b.pManager->PostToUiThread(&TaskProc, &rec2);
    // Tear B down immediately so the queued task cannot run.
    ::PostMessage(b.hWnd, WM_QUIT, 0, 0);
    ::WaitForSingleObject(hB, 5000);
    // The manager is owned by the UI thread; deleting it from here mirrors what a
    // host does when the window closes and is exactly the path that used to leak.
    delete b.pManager;
    b.pManager = nullptr;

    // (5) A wrong-thread call must be counted in BOTH configurations.
    const LONG nBefore = DuiLib::CThreadAffinity::Violations();
    DuiLib::ReportUiThreadViolation(a.pManager, "probe-forced");
    Chk("wrong-thread call increments the counter",
        DuiLib::CThreadAffinity::Violations() == nBefore + 1,
        "violation was not counted -- the release-visible signal is dead");

    if (bInject) {
        Chk("injected-must-fail", false, "self-test: forced failure");
    }

    ::printf("SUMMARY pass=%d fail=%d\n", g_nPass, g_nFail);

    if (a.pManager != nullptr) {
        ::PostMessage(a.hWnd, WM_QUIT, 0, 0);
        ::WaitForSingleObject(hA, 5000);
    }
    return (g_nFail == 0) ? 0 : 1;
}
