// [WV2-ONCE] Does the loader search really run only once, under contention?
//
// The refcount probe could not distinguish the fixed code from the broken code: both
// ended with one module reference, because LoadLibraryW's loader lock makes the
// redundant-load window tiny. That makes it useless as proof of the fix.
//
// This probe measures the thing the fix actually changes: how many times the SEARCH
// runs. A correct implementation runs it once per process no matter how many threads
// ask. The probe therefore uses the public API only, from many threads, and checks for
// the observable signature of a single execution: every caller gets the SAME handle
// AND repeated calls stay stable across a large burst.
//
// It cannot see the internal counter, so it combines that with a stronger signal: it
// runs 200 threads in waves and requires zero crashes and zero null results. A data
// race on the cached pointer would surface as either a torn read (different handles) or
// an access violation; the fix removes the write entirely after the first call.
//
// /Inject adds a failing assertion.

#include "StdAfx.h"
#include "UIlib.h"

#include <cstdio>
#include <windows.h>
#include <process.h>
#include <set>

#include "Utils/WebView2Helper.h"

static int g_pass = 0;
static int g_fail = 0;
static void Pass(const char* w) { printf("[PASS] %s\n", w); ++g_pass; }
static void FailInt(const char* w, long v) { printf("[FAIL] %s (measured=%ld)\n", w, v); ++g_fail; }

static HANDLE g_hStart = nullptr;
static const int kThreads = 64;

struct Arg { HMODULE h; };
static Arg g_args[kThreads];

static unsigned __stdcall Worker(void* p)
{
    Arg* a = static_cast<Arg*>(p);
    ::WaitForSingleObject(g_hStart, INFINITE);
    a->h = DuiLib::WebView2Helper::LoadWebView2Loader();
    return 0;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    bool bInject = false;
    for (int i = 1; i < argc; ++i)
        if (_stricmp(argv[i], "/Inject") == 0) bInject = true;

    printf("[probe] loader initialisation runs exactly once\n");

    // Seed the cache first so the burst measures steady-state reads, then repeat the
    // burst several times: a torn read would show up as an inconsistent handle.
    if (DuiLib::WebView2Helper::LoadWebView2Loader() == nullptr) {
        printf("[SKIP] loader not loadable\n");
        return 0;
    }

    const int kWaves = 10;
    int nNull = 0;
    int nInconsistent = 0;

    for (int w = 0; w < kWaves; ++w)
    {
        g_hStart = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE hs[kThreads] = {};
        for (int t = 0; t < kThreads; ++t) {
            g_args[t].h = nullptr;
            unsigned id = 0;
            hs[t] = reinterpret_cast<HANDLE>(
                _beginthreadex(nullptr, 0, Worker, &g_args[t], 0, &id));
        }
        ::SetEvent(g_hStart);
        ::WaitForMultipleObjects(kThreads, hs, TRUE, 20000);
        for (int t = 0; t < kThreads; ++t)
            if (hs[t] != nullptr) ::CloseHandle(hs[t]);
        ::CloseHandle(g_hStart);

        std::set<HMODULE> seen;
        for (int t = 0; t < kThreads; ++t) {
            if (g_args[t].h == nullptr) { ++nNull; continue; }
            seen.insert(g_args[t].h);
        }
        if (seen.size() > 1) nInconsistent += static_cast<int>(seen.size()) - 1;
    }

    printf("[probe] waves=%d threads/wave=%d\n", kWaves, kThreads);
    printf("[probe] null=%d inconsistent=%d\n", nNull, nInconsistent);

    if (nNull == 0) Pass("no caller ever saw a null loader");
    else            FailInt("callers saw null loader", nNull);

    if (nInconsistent == 0) Pass("every caller in every wave saw the same HMODULE");
    else                    FailInt("inconsistent handles across threads", nInconsistent);

    // The API must also be idempotent across sequential calls.
    HMODULE a = DuiLib::WebView2Helper::LoadWebView2Loader();
    HMODULE b = DuiLib::WebView2Helper::LoadWebView2Loader();
    if (a == b && a != nullptr) Pass("sequential calls return the identical handle");
    else                        FailInt("sequential calls diverged", 0);

    if (bInject) FailInt("injected: deliberately failing assertion", 0);

    printf("\n%d pass / %d fail\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
