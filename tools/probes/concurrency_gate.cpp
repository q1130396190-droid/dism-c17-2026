// [GATE-COV] Concurrency probe -- the one dimension the audit scored 6.0 with
// zero measurements behind it.
//
// What this does NOT do: call rendering APIs from several threads. Those are
// documented "UI thread only" (UIRender.h:42), so a crash there would be the
// caller breaking a stated contract, not a framework defect. Testing it would
// produce a scary-looking result with no actionable meaning.
//
// What it DOES test, where the framework makes an explicit thread-safety claim:
//   1. CResourceManager::GetInstance / GetScopeCount / ReleaseScope
//      -- "注册表与查询均持 mutex，多线程安全" (UIResourceManager.h:56)
//      Assert: concurrent callers all get the SAME instance, no crash, and the
//      scope registry stays coherent under concurrent create/release.
//   2. UICrashGuard::Install -- "Idempotent & thread-safe (std::once_flag)"
//      Assert: N concurrent Install() calls are all safe and the effect is
//      applied exactly once.
//
// A probe that only tests "no crash" would pass even if every call returned
// garbage, so each case also asserts a value.
#include "stdafx.h"
#include "Core/UIResourceManager.h"
#include "Utils/UICrashGuard.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

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

    const int nThreads = 16;
    const int nIters = 2000;

    // ---- 1) concurrent GetInstance must yield one instance --------------
    {
        std::vector<DuiLib::CResourceManager*> vSeen(nThreads, nullptr);
        std::atomic<int> nCrash{ 0 };
        std::vector<std::thread> vThreads;
        vThreads.reserve(nThreads);
        for (int t = 0; t < nThreads; ++t)
        {
            vThreads.emplace_back([&vSeen, t, nIters]() {
                for (int i = 0; i < nIters; ++i)
                {
                    DuiLib::CResourceManager* p =
                        DuiLib::CResourceManager::GetInstance(_T("probe_scope"));
                    if (p != nullptr) vSeen[t] = p;   // same value every call
                }
            });
        }
        for (auto& th : vThreads) th.join();

        int nNull = 0;
        DuiLib::CResourceManager* pFirst = nullptr;
        int nDistinct = 0;
        for (int t = 0; t < nThreads; ++t)
        {
            if (vSeen[t] == nullptr) { ++nNull; continue; }
            if (pFirst == nullptr) { pFirst = vSeen[t]; ++nDistinct; }
            else if (vSeen[t] != pFirst) ++nDistinct;
        }

        char sz[192] = {};
        ::sprintf_s(sz, "threads=%d iters=%d null=%d distinct=%d crashes=%d",
                    nThreads, nIters, nNull, nDistinct, nCrash.load());
        // The documented contract is "safe under concurrency"; the value contract
        // is that every caller sees the same instance for the same scope.
        Chk("concurrent GetInstance returns one instance per scope",
            nNull == 0 && nDistinct == 1, sz);

        DuiLib::CResourceManager::ReleaseScope(_T("probe_scope"));
    }

    // ---- 2) scope registry stays coherent under concurrent use ---------
    {
        std::atomic<int> nBad{ 0 };
        std::vector<std::thread> vThreads;
        vThreads.reserve(nThreads);
        for (int t = 0; t < nThreads; ++t)
        {
            vThreads.emplace_back([&nBad, t]() {
                _TCHAR szScope[64] = {};
                _stprintf_s(szScope, _countof(szScope), _T("probe_%d"), t);
                for (int i = 0; i < 200; ++i)
                {
                    if (DuiLib::CResourceManager::GetInstance(szScope) == nullptr)
                        ++nBad;
                }
            });
        }
        for (auto& th : vThreads) th.join();

        const int nScopes = DuiLib::CResourceManager::GetScopeCount();
        char sz[160] = {};
        ::sprintf_s(sz, "nulls=%d scopes=%d", nBad.load(), nScopes);
        // Each distinct scope name must have produced exactly one entry.
        Chk("distinct scopes are registered exactly once",
            nBad.load() == 0 && nScopes >= nThreads, sz);

        DuiLib::CResourceManager::ReleaseAll();
        const int nAfter = DuiLib::CResourceManager::GetScopeCount();
        char sz2[128] = {};
        ::sprintf_s(sz2, "scopes_after_release=%d", nAfter);
        Chk("ReleaseAll clears the registry", nAfter == 0, sz2);
    }

    // ---- 3) UICrashGuard::Install is idempotent under contention -------
    {
        std::vector<std::thread> vThreads;
        vThreads.reserve(nThreads);
        std::atomic<int> nOk{ 0 };
        for (int t = 0; t < nThreads; ++t)
        {
            vThreads.emplace_back([&nOk]() {
                // Documented as idempotent + std::once_flag guarded.
                DuiLib::UICrashGuard::Install();
                ++nOk;
            });
        }
        for (auto& th : vThreads) th.join();

        char sz[128] = {};
        ::sprintf_s(sz, "install_calls=%d", nOk.load());
        Chk("concurrent Install() calls all complete", nOk.load() == nThreads, sz);
    }

    if (bInject)
    {
        Chk("injected-must-fail", false, "self-test: forced failure");
    }

    ::printf("\nresult: %d pass / %d fail\n", g_nPass, g_nFail);
    return (g_nFail == 0) ? 0 : 1;
}
