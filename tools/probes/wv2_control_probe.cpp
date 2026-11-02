// [WV2-CONTROL] Minimal WebView2 navigation test WITHOUT DuiLib.
//
// Purpose: separate "DuiLib's controller binding is wrong" from "this machine cannot
// run WebView2 at all".
//
// The DuiLib-based probe binds the core successfully but every Navigate() returns
// 0x8007139F (ERROR_INVALID_STATE), including about:blank. Two very different causes
// fit that symptom:
//   (a) DuiLib binds the controller incorrectly (wrong parent, wrong bounds timing),
//   (b) WebView2 cannot function in this environment regardless of the host code.
//
// This file calls the loader directly with no DuiLib involvement. It is the control
// in a controlled experiment:
//   control succeeds  -> the defect is in DuiLib's binding code  (case a)
//   control fails too -> the defect is environmental             (case b)
//
// Everything here mirrors the DuiLib probe's host conditions: STA, a real top-level
// window, a message pump, and an explicit user-data folder. The only variable is the
// absence of DuiLib.

#include <windows.h>
#include <objbase.h>
#include <cstdio>
#include <cstring>

// Loader signatures. WebView2 ships these as plain __stdcall exports, so the probe
// needs no SDK headers and no import library -- which is also how DuiLib consumes it.
typedef HRESULT(__stdcall* PFN_CreateEnv)(LPCWSTR, LPCWSTR, IUnknown*, IUnknown*);
typedef HRESULT(__stdcall* PFN_GetVersion)(LPCWSTR, LPWSTR*);

// Minimal COM plumbing: we only need to hand a handler to the loader and be called
// back. Full interface definitions would require the SDK headers; the vtable order
// for these two callbacks is stable and documented.
struct IEnvHandler : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE Invoke(HRESULT, IUnknown*) = 0;
};
struct ICtrlHandler : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE Invoke(HRESULT, IUnknown*) = 0;
};

static int g_fail = 0;
static int g_pass = 0;

static void Pass(const char* m) { printf("[PASS] %s\n", m); ++g_pass; }
static void Fail(const char* m) { printf("[FAIL] %s\n", m); ++g_fail; }

// We do not know the ICoreWebView2 vtable layout without the SDK header, but the
// only call needed is Navigate(). Its slot was verified against the shipped header
// (WebView2.h): ICoreWebView2 derives from IUnknown and Navigate is the 4th method
// after the three IUnknown entries.
typedef HRESULT(__stdcall* PFN_Navigate)(void* self, LPCWSTR uri);

struct EnvHandler : public IEnvHandler
{
    LONG ref = 1;
    HRESULT hrResult = E_PENDING;
    IUnknown* pEnv = nullptr;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG c = InterlockedDecrement(&ref);
        if (c == 0) delete this;
        return c;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, IUnknown* p) override
    {
        hrResult = hr;
        pEnv = p;
        if (pEnv != nullptr) pEnv->AddRef();
        return S_OK;
    }
};

struct CtrlHandler : public ICtrlHandler
{
    LONG ref = 1;
    HRESULT hrResult = E_PENDING;
    IUnknown* pCtrl = nullptr;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG c = InterlockedDecrement(&ref);
        if (c == 0) delete this;
        return c;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, IUnknown* p) override
    {
        hrResult = hr;
        pCtrl = p;
        if (pCtrl != nullptr) pCtrl->AddRef();
        return S_OK;
    }
};

static void Pump(int ms)
{
    ULONGLONG end = GetTickCount64() + (ULONGLONG)ms;
    MSG msg;
    while (GetTickCount64() < end)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    }
}

template <typename Pred>
static bool PumpUntil(Pred pred, int timeoutMs)
{
    ULONGLONG end = GetTickCount64() + (ULONGLONG)timeoutMs;
    MSG msg;
    while (GetTickCount64() < end)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (pred()) return true;
        Sleep(10);
    }
    return pred();
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    printf("[control] CoInitializeEx(STA) hr=0x%08lX\n", (unsigned long)hrCo);
    if (FAILED(hrCo)) { printf("[SKIP] no STA\n"); return 0; }

    HMODULE h = LoadLibraryW(L"WebView2Loader.dll");
    if (h == nullptr) { printf("[SKIP] loader not found\n"); CoUninitialize(); return 0; }

    PFN_CreateEnv pfnCreate = (PFN_CreateEnv)GetProcAddress(h, "CreateCoreWebView2EnvironmentWithOptions");
    if (pfnCreate == nullptr) { printf("[SKIP] CreateEnvironment export missing\n"); CoUninitialize(); return 0; }

    // A plain top-level window, exactly like the DuiLib probe's host.
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Wv2ControlHost";
    // DuiLib's paint window registers a real class with a background brush and uses
    // the same CREATE flags. Mirror that: a class without CS_* flags and a window
    // without WS_CLIPCHILDREN is a plausible difference for a child-window host.
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    HWND hWnd = CreateWindowExW(0, wc.lpszClassName, L"control",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                0, 0, 640, 480, nullptr, nullptr, wc.hInstance, nullptr);
    if (hWnd == nullptr) { printf("[SKIP] no window\n"); CoUninitialize(); return 0; }
    ShowWindow(hWnd, SW_SHOWNORMAL);
    UpdateWindow(hWnd);
    // Force the window to actually become foreground. WebView2's controller creation
    // waits on the host window being active in some configurations; fg=0 above means
    // the window is visible but not the foreground window, which is the one host-state
    // difference left versus a normal application.
    SetForegroundWindow(hWnd);
    SetActiveWindow(hWnd);
    SetFocus(hWnd);
    // A brief pump so the activation messages are processed before we measure.
    for (int i = 0; i < 20; ++i)
    {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        Sleep(10);
    }
    // Report the real visibility state: a host that is shown but not "visible" to
    // the window manager (minimized session, no interactive desktop) behaves
    // differently, and that must be visible in the log rather than assumed.
    printf("[control] host IsWindowVisible=%d IsIconic=%d fg=%d\n",
           IsWindowVisible(hWnd) ? 1 : 0, IsIconic(hWnd) ? 1 : 0,
           GetForegroundWindow() == hWnd ? 1 : 0);

    WCHAR szUdf[MAX_PATH] = { 0 };
    GetTempPathW(MAX_PATH, szUdf);
    wcscat_s(szUdf, L"wv2_control_udf");
    CreateDirectoryW(szUdf, nullptr);
    printf("[control] user-data folder: %ls\n", szUdf);

    // Match the DuiLib call exactly: it passes nullptr for the user-data folder and
    // succeeds. This experiment started by passing an explicit folder and hung at
    // controller creation, so the folder argument is itself a variable worth testing.
    EnvHandler* pEnvH = new EnvHandler();
    HRESULT hr = pfnCreate(nullptr, nullptr, nullptr, pEnvH);
    printf("[control] CreateEnvironment hr=0x%08lX\n", (unsigned long)hr);
    if (FAILED(hr)) { Fail("environment creation rejected"); }
    else
    {
        bool ok = PumpUntil([&]() { return pEnvH->hrResult != E_PENDING; }, 15000);
        printf("[control] env callback: arrived=%s hr=0x%08lX\n",
               ok ? "yes" : "no", (unsigned long)pEnvH->hrResult);
        if (!ok || FAILED(pEnvH->hrResult)) Fail("environment creation did not complete");
        else Pass("environment created (no DuiLib involved)");
    }

    if (SUCCEEDED(pEnvH->hrResult) && pEnvH->pEnv != nullptr)
    {
        // [ENV-READY] get_BrowserVersionString is the cheapest proof that the
        // environment finished initialising rather than merely being constructed.
        // ICoreWebView2EnvironmentVtbl[3] per DuiLib/WebView2.h.
        {
            typedef HRESULT(__stdcall* PFN_getVerStr)(void*, LPWSTR*);
            void** evt = *(void***)pEnvH->pEnv;
            PFN_getVerStr pfnVer = (PFN_getVerStr)evt[5];
            LPWSTR pszVer = nullptr;
            HRESULT hrVer = pfnVer(pEnvH->pEnv, &pszVer);
            printf("[control] get_BrowserVersionString hr=0x%08lX ver=%ls\n",
                   (unsigned long)hrVer, (pszVer != nullptr) ? pszVer : L"(null)");
            if (SUCCEEDED(hrVer) && pszVer != nullptr)
                Pass("environment reports a browser version -- environment is ready");
            else
                Fail("environment has no browser version -- it was constructed but not initialised");
            if (pszVer != nullptr) CoTaskMemFree(pszVer);
        }

        // Slot indices below were EXTRACTED from the shipped DuiLib/WebView2.h
        // (ICoreWebView2EnvironmentVtbl), not guessed. The first attempt guessed [3]
        // for CreateCoreWebView2Controller, which is actually get_BrowserVersionString:
        // it returned a BSTR reinterpreted as a pointer and the probe segfaulted.
        //
        //   ICoreWebView2EnvironmentVtbl: [0]QI [1]CreateCoreWebView2Controller ...
        //   ICoreWebView2ControllerVtbl:  [0]QI ... [23]get_CoreWebView2
        //   ICoreWebView2Vtbl:            [0]QI [1]get_Settings [2]get_Source [3]Navigate
        typedef HRESULT(__stdcall* PFN_CreateCtrl)(void* self, HWND, IUnknown*);
        void** vtbl = *(void***)pEnvH->pEnv;
        PFN_CreateCtrl pfnCreateCtrl = (PFN_CreateCtrl)vtbl[3];

        CtrlHandler* pCtrlH = new CtrlHandler();
        hr = pfnCreateCtrl(pEnvH->pEnv, hWnd, pCtrlH);
        printf("[control] CreateController hr=0x%08lX\n", (unsigned long)hr);
        // S_FALSE is a documented success: creation was deferred and the callback
        // will arrive later. Treating only S_OK as success made this look like a
        // failure and skipped the pump that the deferred case needs.
        if (hr == S_FALSE) printf("[control] creation deferred, pumping\n");
        if (FAILED(hr)) Fail("CreateController call failed outright");
        // Pump with progress logging: a silent 15s wait cannot distinguish "slow" from
        // "never". Print every 3s so the log shows whether anything at all happens.
        bool ok = false;
        for (int i = 1; i <= 15; ++i)
        {
            if (PumpUntil([&]() { return pCtrlH->hrResult != E_PENDING; }, 1000))
            {
                ok = true;
                printf("[control] controller callback after %d s\n", i);
                break;
            }
            if ((i % 3) == 0)
                printf("[control] still pending after %d s\n", i);
        }
        printf("[control] ctrl callback: arrived=%s hr=0x%08lX\n",
               ok ? "yes" : "no", (unsigned long)pCtrlH->hrResult);
        if (!ok || FAILED(pCtrlH->hrResult)) Fail("controller creation did not complete");
        else Pass("controller created (no DuiLib involved)");

        if (SUCCEEDED(pCtrlH->hrResult) && pCtrlH->pCtrl != nullptr)
        {
            // get_CoreWebView2 is slot 23 in ICoreWebView2ControllerVtbl.
            typedef HRESULT(__stdcall* PFN_getCore)(void*, void**);
            void** cvt = *(void***)pCtrlH->pCtrl;
            PFN_getCore pfnGetCore = (PFN_getCore)cvt[25];
            void* pCore = nullptr;
            hr = pfnGetCore(pCtrlH->pCtrl, &pCore);
            printf("[control] get_CoreWebView2 hr=0x%08lX ptr=%p\n", (unsigned long)hr, pCore);

            if (SUCCEEDED(hr) && pCore != nullptr && pCore != (void*)1)
            {
                // Mirror what a real host does before navigating. Skipping this is the
                // difference between "WebView2 rejects the call" and "WebView2 works",
                // so the control experiment must not skip it.
                typedef HRESULT(__stdcall* PFN_putVisible)(void*, BOOL);
                typedef HRESULT(__stdcall* PFN_putBounds)(void*, RECT);
                HRESULT hrVis = ((PFN_putVisible)cvt[4])(pCtrlH->pCtrl, TRUE);
                RECT rcWin = { 0, 0, 640, 480 };
                HRESULT hrBnd = ((PFN_putBounds)cvt[6])(pCtrlH->pCtrl, rcWin);
                printf("[control] put_IsVisible hr=0x%08lX, put_Bounds hr=0x%08lX\n",
                       (unsigned long)hrVis, (unsigned long)hrBnd);

                // ICoreWebView2::Navigate is slot 3 (IUnknown has 3 entries).
                PFN_Navigate pfnNav = (PFN_Navigate)(*(void***)pCore)[5];
                HRESULT hrNav = pfnNav(pCore, L"about:blank");
                printf("[control] Navigate(about:blank) hr=0x%08lX\n", (unsigned long)hrNav);
                if (SUCCEEDED(hrNav))
                    Pass("Navigate accepted about:blank without DuiLib -- defect is in DuiLib binding");
                else
                    Fail("Navigate rejected about:blank without DuiLib -- environment cannot run WebView2");
            }
        }
    }

    DestroyWindow(hWnd);
    printf("\n%d pass / %d fail\n", g_pass, g_fail);
    CoUninitialize();
    return g_fail == 0 ? 0 : 1;
}
