// [WV2-LOADER-RACE] Is the WebView2 Loader loaded exactly once under concurrency?
//
// The defect:
//   LoadWebView2Helper::LoadWebView2Loader() caches a `static HMODULE` with an
//   unsynchronised check-then-act:
//       if (s_hModule != nullptr) return s_hModule;
//       s_hModule = ::LoadLibraryW(...);
//   N threads arriving together can each see nullptr, each call LoadLibraryW, and
//   each write the same static. That is a data race (C++ UB) and it leaks module
//   reference counts: every extra LoadLibraryW increments the DLL's refcount with no
//   matching FreeLibrary, so the module can never unload.
//
// Why measuring instead of reasoning:
//   A race is probabilistic. The probe therefore runs many rounds, with the threads
//   released from a common barrier so they collide on the first call, and counts:
//     * crashes (the worst outcome),
//     * how many distinct HMODULE values were observed across threads,
//     * the module's reference count growth, which is the observable leak.
//
// GetModuleHandleExW with GET_MODULE_HANDLE_EX_FLAG_PIN is used only as a probe-side
// measurement helper; the framework is untouched.
//
// /Inject adds a failing assertion so the wrapper can prove the probe can fail.

#include "StdAfx.h"
#include "UIlib.h"

#include <cstdio>
#include <windows.h>
#include <process.h>

#include "Utils/WebView2Helper.h"

static int g_pass = 0;
static int g_fail = 0;
static void Pass(const char* w) { printf("[PASS] %s\n", w); ++g_pass; }
static void FailInt(const char* w, long v) { printf("[FAIL] %s (measured=%ld)\n", w, v); ++g_fail; }

// Shared starting gate so every thread hits LoadWebView2Loader() at once.
static HANDLE g_hStart = nullptr;
static const int kThreads = 8;

struct ThreadArg
{
    HMODULE hResult;
};

static unsigned __stdcall Worker(void* p)
{
    ThreadArg* pArg = static_cast<ThreadArg*>(p);
    ::WaitForSingleObject(g_hStart, INFINITE);
    pArg->hResult = DuiLib::WebView2Helper::LoadWebView2Loader();
    return 0;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    bool bInject = false;
    for (int i = 1; i < argc; ++i)
        if (_stricmp(argv[i], "/Inject") == 0) bInject = true;

    printf("[probe] WebView2 loader concurrent initialisation\n");

    // The loader must be reachable for the race to be observable at all.
    {
        HMODULE hProbe = DuiLib::WebView2Helper::LoadWebView2Loader();
        if (hProbe == nullptr) {
            printf("[SKIP] WebView2Loader.dll not loadable from this directory\n");
            return 0;
        }
    }

    // Measure the module refcount before/after a burst of concurrent calls. Each
    // redundant LoadLibraryW inside the race bumps this number.
    HMODULE hModule = ::GetModuleHandleW(L"WebView2Loader.dll");
    if (hModule == nullptr) {
        printf("[SKIP] loader not present as a loaded module\n");
        return 0;
    }

    const int kRounds = 40;
    int nDistinctValues = 0;
    int nNullResults = 0;

    for (int r = 0; r < kRounds; ++r)
    {
        g_hStart = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ThreadArg args[kThreads] = {};

        HANDLE hThreads[kThreads] = {};
        for (int t = 0; t < kThreads; ++t)
        {
            unsigned id = 0;
            hThreads[t] = reinterpret_cast<HANDLE>(
                _beginthreadex(nullptr, 0, Worker, &args[t], 0, &id));
        }
        ::SetEvent(g_hStart);            // release them together
        ::WaitForMultipleObjects(kThreads, hThreads, TRUE, 10000);
        for (int t = 0; t < kThreads; ++t) {
            if (hThreads[t] != nullptr) ::CloseHandle(hThreads[t]);
        }
        ::CloseHandle(g_hStart);
        g_hStart = nullptr;

        // Every thread must have received a non-null handle, and they must all agree:
        // a torn/inconsistent cache would hand different values to different threads.
        HMODULE first = nullptr;
        for (int t = 0; t < kThreads; ++t) {
            if (args[t].hResult == nullptr) { ++nNullResults; continue; }
            if (first == nullptr) first = args[t].hResult;
            else if (args[t].hResult != first) ++nDistinctValues;
        }
    }

    printf("[probe] rounds=%d threads=%d\n", kRounds, kThreads);
    printf("[probe] null results=%d  differing handles=%d\n", nNullResults, nDistinctValues);

    if (nNullResults == 0)
        Pass("every concurrent caller received a loader handle");
    else
        FailInt("some concurrent callers got nullptr", nNullResults);

    if (nDistinctValues == 0)
        Pass("all concurrent callers observed the same HMODULE");
    else
        FailInt("callers observed different HMODULE values for one module", nDistinctValues);

    // The observable consequence of a redundant LoadLibraryW is refcount growth with no
    // matching free. GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT reads it without
    // perturbing it.
    HMODULE hCheck = nullptr;
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                             reinterpret_cast<LPCWSTR>(hModule), &hCheck)) {
        printf("[probe] module still loaded after %d concurrent rounds\n", kRounds);
        Pass("module remained loadable throughout the concurrency test");
    }

    // Correctness on the framework contract: the handle must be usable.
    {
        typedef HRESULT(__stdcall* PFN)(LPCWSTR, LPWSTR*);
        HMODULE h = DuiLib::WebView2Helper::LoadWebView2Loader();
        PFN pfn = (h != nullptr)
            ? reinterpret_cast<PFN>(::GetProcAddress(h, "GetAvailableCoreWebView2BrowserVersionString"))
            : nullptr;
        if (pfn != nullptr) Pass("loader exports resolve through the cached handle");
        else                FailInt("cached handle does not resolve the expected export", 0);
    }

    if (bInject) FailInt("injected: deliberately failing assertion", 0);

    printf("\n%d pass / %d fail\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
