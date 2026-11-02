// [WV2-REFCOUNT] Does the loader's module refcount grow under concurrent calls?
//
// The previous probe looked for crashes and differing handles and found none. That does
// NOT prove the race is absent: LoadLibraryW takes an internal loader lock, so the
// redundancy window is tiny and the worst outcome is usually not a crash but a leaked
// module reference -- each extra LoadLibraryW bumps the refcount with no matching
// FreeLibrary, so the DLL can never unload.
//
// This probe measures that observable directly: it loads the module N times in a burst
// and reads the reference count. A correct implementation performs exactly ONE
// LoadLibraryW for the whole process, so the count must not depend on how many threads
// called it.
//
// Reference count is not directly queryable, so the effect is measured indirectly:
// LdrFindEntryForAddress-free approach -- count how many times a fresh FreeLibrary
// keeps the module mapped. With one load, a single FreeLibrary unloads it; with N
// loads it stays. The probe therefore runs in a helper process (this exe) whose only
// loader reference comes from the burst.
//
// /Inject adds a failing assertion.

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

static HANDLE g_hStart = nullptr;
static const int kThreads = 16;

static unsigned __stdcall Worker(void* p)
{
    ::WaitForSingleObject(g_hStart, INFINITE);
    // Each thread asks for the loader; a correct cache means only the first one ever
    // calls LoadLibraryW.
    (void)DuiLib::WebView2Helper::LoadWebView2Loader();
    return 0;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    bool bInject = false;
    for (int i = 1; i < argc; ++i)
        if (_stricmp(argv[i], "/Inject") == 0) bInject = true;

    printf("[probe] WebView2 loader reference-count growth\n");

    if (DuiLib::WebView2Helper::LoadWebView2Loader() == nullptr) {
        printf("[SKIP] loader not loadable\n");
        return 0;
    }

    // Burst of concurrent first calls: the exact shape the race needs.
    g_hStart = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE hThreads[kThreads] = {};
    for (int t = 0; t < kThreads; ++t) {
        unsigned id = 0;
        hThreads[t] = reinterpret_cast<HANDLE>(
            _beginthreadex(nullptr, 0, Worker, nullptr, 0, &id));
    }
    ::SetEvent(g_hStart);
    ::WaitForMultipleObjects(kThreads, hThreads, TRUE, 15000);
    for (int t = 0; t < kThreads; ++t)
        if (hThreads[t] != nullptr) ::CloseHandle(hThreads[t]);
    ::CloseHandle(g_hStart);

    // Count how many FreeLibrary calls are needed to actually unload. One load => one
    // call. Every redundant load adds one more.
    HMODULE h = ::GetModuleHandleW(L"WebView2Loader.dll");
    if (h == nullptr) {
        printf("[SKIP] loader is not mapped after the burst\n");
        return 0;
    }

    int nExtraRefs = 0;
    for (int i = 0; i < 64; ++i) {
        if (!::FreeLibrary(h)) break;
        ++nExtraRefs;
        if (::GetModuleHandleW(L"WebView2Loader.dll") == nullptr) break;
    }

    printf("[probe] extra loader references released before unload: %d\n", nExtraRefs);
    printf("[probe] threads in burst: %d\n", kThreads);

    // With a single correct LoadLibraryW the module goes away on the first
    // FreeLibrary. Extra references mean redundant loads happened.
    if (nExtraRefs <= 1)
        Pass("exactly one loader reference existed (no redundant load)");
    else
        FailInt("loader held redundant references from concurrent initialisation",
                nExtraRefs);

    if (bInject) FailInt("injected: deliberately failing assertion", 0);

    printf("\n%d pass / %d fail\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
