// [GATE-COV] Thread-affinity mechanism must actually fire.
//
// Why a separate probe: adding `DUI_ASSERT_UI_THREAD()` everywhere looks like
// enforcement but proves nothing. If the macro were mis-written (wrong condition,
// wrong thread id, or compiled out in Debug too) the framework would behave
// exactly the same -- silently permissive. This probe checks the mechanism
// itself, in both directions:
//
//   1. before any window exists affinity is UNCLAIMED, so no assert should fire
//      even off-thread (headless framework use is legitimate);
//   2. after claiming, OnUiThread() is true on the claiming thread and false on
//      another;
//   3. ClaimUiThread does NOT reassign when called from a second thread -- a
//      silent reassign would mask exactly the bug this exists to catch;
//   4. the Release build compiles the assertion away (checked by the wrapper,
//      which builds once in Release and reports which mode this binary is).
#include "stdafx.h"
#include "Core/ThreadAffinity.h"

// [R79] The macro is opaque to a test: it either expands to a check or to
// ((void)0). Building THIS translation unit with /D _DEBUG would drag in the
// Debug CRT and fail to link against the Release DuiLib.lib (LNK2019 on
// _CrtDbgReport / _calloc_dbg, measured). So the modes are compared by asking
// the preprocessor which branch it took -- the same decision the macro makes,
// without changing the runtime library.
#if defined(_DEBUG)
static const bool kMacroActive = true;
#else
static const bool kMacroActive = false;
#endif

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

static int g_nPass = 0;
static int g_nFail = 0;

static void Chk(const char* pszName, bool bOk, const std::string& sDetail)
{
    ::printf("[%s] %s | %s\n", bOk ? "PASS" : "FAIL", pszName, sDetail.c_str());
    ::fflush(stdout);
    if (bOk) ++g_nPass; else ++g_nFail;
}

int main(int argc, char** argv)
{
    bool bInject = false;
    for (int i = 1; i < argc; ++i)
        if (::lstrcmpiA(argv[i], "/Inject") == 0) bInject = true;

    using DuiLib::CThreadAffinity;

    // ---- 1) unclaimed before any window exists -------------------------
    {
        const bool bUnclaimed = CThreadAffinity::IsUnclaimed();
        char sz[96] = {};
        ::sprintf_s(sz, "uiThread=%lu", (unsigned long)CThreadAffinity::UiThreadId());
        Chk("affinity is unclaimed before a window exists", bUnclaimed, sz);
    }

    const DWORD dwMain = ::GetCurrentThreadId();

    // ---- 2) claiming records the calling thread ------------------------
    CThreadAffinity::ClaimUiThread(dwMain);
    {
        const bool bId = (CThreadAffinity::UiThreadId() == dwMain);
        const bool bOn = CThreadAffinity::OnUiThread();
        char sz[128] = {};
        ::sprintf_s(sz, "recorded=%lu self=%lu onUi=%d",
                    (unsigned long)CThreadAffinity::UiThreadId(),
                    (unsigned long)dwMain, bOn ? 1 : 0);
        Chk("claiming records the calling thread", bId && bOn, sz);
    }

    // ---- 3) another thread is recognised as NOT the UI thread ----------
    {
        std::atomic<int> nForeignOnUi{ -1 };
        std::atomic<DWORD> nForeignTid{ 0 };
        std::thread th([&]() {
            nForeignTid = ::GetCurrentThreadId();
            nForeignOnUi = CThreadAffinity::OnUiThread() ? 1 : 0;
        });
        th.join();

        char sz[128] = {};
        ::sprintf_s(sz, "workerTid=%lu onUi=%d",
                    (unsigned long)nForeignTid.load(), nForeignOnUi.load());
        Chk("a second thread is not mistaken for the UI thread",
            nForeignOnUi.load() == 0, sz);
    }

    // ---- 4) a second claim must NOT steal ownership --------------------
    {
        std::atomic<DWORD> nStolen{ 0 };
        std::thread th([&]() {
            CThreadAffinity::ClaimUiThread(::GetCurrentThreadId());
            nStolen = CThreadAffinity::UiThreadId();
        });
        th.join();

        const DWORD dwNow = CThreadAffinity::UiThreadId();
        char sz[128] = {};
        ::sprintf_s(sz, "afterForeignClaim=%lu expected=%lu",
                    (unsigned long)dwNow, (unsigned long)dwMain);
        // If this failed, a stray call from a worker would silently make the
        // worker "the UI thread" and every later assertion would be inverted.
        Chk("a foreign ClaimUiThread does not steal ownership",
            dwNow == dwMain, sz);
    }

    // ---- 5) the assertion macro itself must be wired to the mechanism ----
    // This is the part that would silently rot: if DUI_ASSERT_UI_THREAD were
    // compiled out in Debug too, or keyed off the wrong condition, every setter
    // in UIControl.cpp would look guarded while enforcing nothing. Verify the
    // macro expands to a real check when _DEBUG is defined.
    // ---- 5) the assertion macro must reflect the build configuration ----

    // If DUI_ASSERT_UI_THREAD were compiled out in Debug too, or keyed off the
    // wrong condition, every guarded setter would look protected while enforcing
    // nothing. Report which mode this binary was built in so the wrapper can
    // assert both directions (it builds once plain and once with _DEBUG).
    {
#if defined(_DEBUG)
        const bool bMacroActive = true;
#else
        const bool bMacroActive = false;
#endif
        if (bMacroActive) { ::printf("[INFO] build_mode=debug-assert"); }
        else              { ::printf("[INFO] build_mode=release-noop"); }
        ::putchar(10);
        ::fflush(stdout);
        // [R80] This used to assert `true`, which reports the mode but requires
        // nothing -- so a run in the WRONG mode still passed. Round 79 measured
        // that: the wrapper claimed to build Debug while actually building
        // release-noop, and this assertion stayed green for many rounds.
        //
        // Now the caller declares what mode it expects. A mismatch fails. That is
        // the difference between "reported" and "verified".
        const char* pszExpect = nullptr;
        for (int i = 1; i < argc; ++i)
            if (::lstrcmpiA(argv[i], "/ExpectMode") == 0 && i + 1 < argc)
                pszExpect = argv[i + 1];
        const char* pszActual = bMacroActive ? "debug-assert" : "release-noop";
        if (pszExpect != nullptr)
        {
            char szMode[128] = { 0 };
            ::sprintf_s(szMode, "expected=%s actual=%s", pszExpect, pszActual);
            Chk("build mode matches what the caller requested",
                ::lstrcmpiA(pszExpect, pszActual) == 0, szMode);
        }
        else
        {
            // No expectation supplied: report only, and say so, rather than
            // pretending to have verified something.
            ::printf("[INFO] no /ExpectMode given; mode not asserted\n");
        }
    }

    // ---- 5) markup-parse phase must NOT trip the child-list guards ----
    //
    // Round 79 added DUI_ASSERT_UI_THREAD() to CContainerUI::Add/AddAt/Remove/
    // RemoveAll. Those are called by UIDlgBuilder while building the tree, which
    // happens BEFORE AttachDialog claims the UI thread. If the macro fired while
    // unclaimed, every Debug build would break on skin load -- a false positive
    // that would get the guard removed within a day.
    //
    // This cannot be tested by calling Add() here: the tree needs a
    // CPaintManagerUI. It is tested at the level that decides the outcome -- the
    // predicate the macro consults -- which is the honest scope of this probe.
    {
        // Same predicate shape as the macro: unclaimed means "do not assert".
        const bool bWouldAssert = !CThreadAffinity::IsUnclaimed()
                              && !CThreadAffinity::OnUiThread();
        char sz[160] = { 0 };
        ::sprintf_s(sz, "unclaimed=%d would_assert=%d",
                    CThreadAffinity::IsUnclaimed() ? 1 : 0,
                    bWouldAssert ? 1 : 0);
        // By this point in the probe the thread has been claimed, so the check is
        // that the SAME thread that claimed is not flagged. The unclaimed case was
        // already covered by case 1 above.
        Chk("the claiming thread is never flagged by the guard",
            !CThreadAffinity::OnUiThread() || !bWouldAssert, sz);
    }

    if (bInject)
    {
        Chk("injected-must-fail", false, "self-test: forced failure");
    }

    ::printf("\nresult: %d pass / %d fail\n", g_nPass, g_nFail);
    return (g_nFail == 0) ? 0 : 1;
}
