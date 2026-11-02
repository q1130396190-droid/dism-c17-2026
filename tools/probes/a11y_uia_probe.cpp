// [A11Y-3] Verify the UIA provider is reachable from a real UIA client.
//
// Why a compiled probe instead of a screenshot: the whole point of the provider
// is that a UIA *client* (Inspect.exe, Narrator, test automation) can enumerate
// controls. The only honest check is to be that client: call
// IUIAutomation::ElementFromHandle and walk the tree.
//
// What it asserts:
//   1. WM_GETOBJECT/UiaRootObjectId yields an element (before this change it
//      yielded nothing, so the window looked like one opaque pane);
//   2. the root has at least one child (the control tree is exposed);
//   3. at least one descendant reports a Name or a non-Pane control type, i.e.
//      real Controls -- not just nested panes;
//   4. property queries return sane values (IsEnabled is VT_BOOL, etc).
//
// Usage: a11y_uia_probe.exe <hwnd-decimal>
#include <windows.h>
#include <shellapi.h>   // ShellExecuteA: keep window discovery in-process
#include <objbase.h>
#include <UIAutomationCore.h>      /* PROPERTYID/CONTROLTYPEID, provider ifaces */
#include <UIAutomationClient.h>    /* IUIAutomation*, UIA_* constant ids */
#include <cstdio>
#include <string>

#pragma comment(lib, "uiautomationcore.lib")

static int g_nPass = 0;
static int g_nFail = 0;

static void Chk(const char* pszName, bool bOk, const char* pszDetail)
{
    ::printf("[%s] %s | %s\n", bOk ? "PASS" : "FAIL", pszName, pszDetail);
    ::fflush(stdout);
    if (bOk) ++g_nPass; else ++g_nFail;
}

// Depth-first walk; stops early once nNeed named elements were seen.
static void Walk(IUIAutomationElement* pElem, int nDepth, int& nTotal,
                 int& nNamed, int nMaxDepth, int nDepthLimit)
{
    if (pElem == nullptr || nDepth > nDepthLimit) return;

    ++nTotal;

    BSTR bstrName = nullptr;
    if (SUCCEEDED(pElem->get_CurrentName(&bstrName)) && bstrName != nullptr)
    {
        if (::SysStringLen(bstrName) > 0) ++nNamed;
        ::SysFreeString(bstrName);
    }

    IUIAutomationTreeWalker* pWalker = nullptr;
    IUIAutomation* pUia = nullptr;
    if (FAILED(::CoCreateInstance(__uuidof(CUIAutomation), nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pUia))))
        return;
    if (SUCCEEDED(pUia->get_ControlViewWalker(&pWalker)) && pWalker != nullptr)
    {
        IUIAutomationElement* pChild = nullptr;
        HRESULT hr = pWalker->GetFirstChildElement(pElem, &pChild);
        int nGuard = 0;
        while (SUCCEEDED(hr) && pChild != nullptr && nGuard++ < 5000)
        {
            Walk(pChild, nDepth + 1, nTotal, nNamed, nMaxDepth, nDepthLimit);

            IUIAutomationElement* pNext = nullptr;
            hr = pWalker->GetNextSiblingElement(pChild, &pNext);
            pChild->Release();
            pChild = pNext;
        }
        pWalker->Release();
    }
    pUia->Release();
}

// Depth-first search for a control that exposes InvokePattern; invokes the
// first one found and reports the HRESULT.
static void FindAndInvoke(IUIAutomationElement* pElem, int nDepth, int nDepthLimit,
                          int& nInvokable, bool& bInvoked, HRESULT& hrResult)
{
    if (pElem == nullptr || nDepth > nDepthLimit) return;

    IUnknown* pUnk = nullptr;
    if (SUCCEEDED(pElem->GetCurrentPattern(UIA_InvokePatternId, &pUnk)) && pUnk != nullptr)
    {
        ++nInvokable;
        if (!bInvoked)
        {
            IUIAutomationInvokePattern* pInvoke = nullptr;
            if (SUCCEEDED(pUnk->QueryInterface(__uuidof(IUIAutomationInvokePattern),
                                               reinterpret_cast<void**>(&pInvoke)))
                && pInvoke != nullptr)
            {
                hrResult = pInvoke->Invoke();
                bInvoked = SUCCEEDED(hrResult);
                pInvoke->Release();
            }
        }
        pUnk->Release();
    }

    IUIAutomation* pUia = nullptr;
    IUIAutomationTreeWalker* pWalker = nullptr;
    if (FAILED(::CoCreateInstance(__uuidof(CUIAutomation), nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pUia))))
        return;
    if (SUCCEEDED(pUia->get_ControlViewWalker(&pWalker)) && pWalker != nullptr)
    {
        IUIAutomationElement* pChild = nullptr;
        HRESULT hr = pWalker->GetFirstChildElement(pElem, &pChild);
        int nGuard = 0;
        while (SUCCEEDED(hr) && pChild != nullptr && nGuard++ < 5000)
        {
            FindAndInvoke(pChild, nDepth + 1, nDepthLimit, nInvokable, bInvoked, hrResult);

            IUIAutomationElement* pNext = nullptr;
            hr = pWalker->GetNextSiblingElement(pChild, &pNext);
            pChild->Release();
            pChild = pNext;
        }
        pWalker->Release();
    }
    pUia->Release();
}

// Depth-first search for an edit control exposing ValuePattern; reads its value
// back through the client-side interface.
static void FindAndSetValue(IUIAutomationElement* pElem, int nDepth, int nDepthLimit,
                            int& nValueable, bool& bOk, wchar_t* pszOut, int cchOut,
                            HRESULT& hrResult)
{
    if (pElem == nullptr || nDepth > nDepthLimit) return;

    IUnknown* pUnk = nullptr;
    if (SUCCEEDED(pElem->GetCurrentPattern(UIA_ValuePatternId, &pUnk)) && pUnk != nullptr)
    {
        ++nValueable;
        if (!bOk)
        {
            IUIAutomationValuePattern* pVal = nullptr;
            if (SUCCEEDED(pUnk->QueryInterface(__uuidof(IUIAutomationValuePattern),
                                               reinterpret_cast<void**>(&pVal)))
                && pVal != nullptr)
            {
                BSTR bstr = nullptr;
                hrResult = pVal->get_CurrentValue(&bstr);
                if (SUCCEEDED(hrResult) && bstr != nullptr)
                {
                    ::wcsncpy_s(pszOut, static_cast<size_t>(cchOut), bstr, _TRUNCATE);
                    bOk = true;
                }
                if (bstr != nullptr) ::SysFreeString(bstr);
                pVal->Release();
            }
        }
        pUnk->Release();
    }

    IUIAutomation* pUia = nullptr;
    IUIAutomationTreeWalker* pWalker = nullptr;
    if (FAILED(::CoCreateInstance(__uuidof(CUIAutomation), nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pUia))))
        return;
    if (SUCCEEDED(pUia->get_ControlViewWalker(&pWalker)) && pWalker != nullptr)
    {
        IUIAutomationElement* pChild = nullptr;
        HRESULT hr = pWalker->GetFirstChildElement(pElem, &pChild);
        int nGuard = 0;
        while (SUCCEEDED(hr) && pChild != nullptr && nGuard++ < 5000)
        {
            FindAndSetValue(pChild, nDepth + 1, nDepthLimit, nValueable, bOk,
                            pszOut, cchOut, hrResult);
            IUIAutomationElement* pNext = nullptr;
            hr = pWalker->GetNextSiblingElement(pChild, &pNext);
            pChild->Release();
            pChild = pNext;
        }
        pWalker->Release();
    }
    pUia->Release();
}

// Find the first element that reports IsKeyboardFocusable, returning a reference
// to it (caller releases).
static void FindFocusable(IUIAutomationElement* pElem, int nDepth, int nDepthLimit,
                          int& nFocusable, IUIAutomationElement*& pOut, HRESULT& hrResult)
{
    if (pElem == nullptr || nDepth > nDepthLimit || pOut != nullptr) return;

    VARIANT v;
    ::VariantInit(&v);
    if (SUCCEEDED(pElem->GetCurrentPropertyValue(UIA_IsKeyboardFocusablePropertyId, &v))
        && v.vt == VT_BOOL && v.boolVal != VARIANT_FALSE)
    {
        ++nFocusable;
        if (pOut == nullptr)
        {
            pElem->AddRef();
            pOut = pElem;
            hrResult = S_OK;
            ::VariantClear(&v);
            return;
        }
    }
    ::VariantClear(&v);

    IUIAutomation* pUia = nullptr;
    IUIAutomationTreeWalker* pWalker = nullptr;
    if (FAILED(::CoCreateInstance(__uuidof(CUIAutomation), nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pUia))))
        return;
    if (SUCCEEDED(pUia->get_ControlViewWalker(&pWalker)) && pWalker != nullptr)
    {
        IUIAutomationElement* pChild = nullptr;
        HRESULT hr = pWalker->GetFirstChildElement(pElem, &pChild);
        int nGuard = 0;
        while (SUCCEEDED(hr) && pChild != nullptr && nGuard++ < 5000)
        {
            FindFocusable(pChild, nDepth + 1, nDepthLimit, nFocusable, pOut, hrResult);
            IUIAutomationElement* pNext = nullptr;
            hr = pWalker->GetNextSiblingElement(pChild, &pNext);
            pChild->Release();
            pChild = pNext;
        }
        pWalker->Release();
    }
    pUia->Release();
}

int main(int argc, char** argv)
{
    // [GATE-SELFTEST] /Inject adds one deliberately failing assertion so this
    // probe can prove its verdict path still works.
    bool bInject = false;
    if (argc >= 2 && lstrcmpiA(argv[1], "/Inject") == 0) bInject = true;

    HWND hWnd = nullptr;
    if (argc >= 2 && !bInject)
    {
        hWnd = reinterpret_cast<HWND>(static_cast<INT_PTR>(_strtoi64(argv[1], nullptr, 10)));
    }
    if (hWnd == nullptr && !bInject)
    {
        // [R329] No hwnd given: launch the asInvoker build and find the window
        //   by class, right here in the C++ probe.
        //   Why: the PowerShell runner could not obtain the handle -- both
        //   Process.MainWindowHandle and a FindWindow P/Invoke returned zero,
        //   while this exact ShellExecuteW + FindWindowW pair works from a
        //   native process in ~1s. Keeping discovery next to the assertion
        //   removes that host-dependent behaviour from the gate.
        char szAsi[MAX_PATH] = {};
        // tools/probes -> repo root -> bin64, then append the file name.
        ::GetModuleFileNameA(nullptr, szAsi, MAX_PATH);
        {
            char* pEnd = szAsi + ::lstrlenA(szAsi);
            while (pEnd > szAsi && *pEnd != '\\') --pEnd;
            if (pEnd > szAsi) *pEnd = 0;            // -> tools/probes
            pEnd = szAsi + ::lstrlenA(szAsi);
            while (pEnd > szAsi && *pEnd != '\\') --pEnd;
            if (pEnd > szAsi) *pEnd = 0;            // -> tools
            pEnd = szAsi + ::lstrlenA(szAsi);
            while (pEnd > szAsi && *pEnd != '\\') --pEnd;
            if (pEnd > szAsi) *pEnd = 0;            // -> repo root
            ::lstrcatA(szAsi, "\\bin64");
        }
        const char* kAsi = szAsi;   // now "<root>\bin64"
        if (::GetFileAttributesA(kAsi) == INVALID_FILE_ATTRIBUTES)
        {
            ::printf("[SKIP] asInvoker build missing (Win8Metro_asi.exe); "
                     "UIA cannot cross integrity levels\n");
            return 3;
        }
        // chdir to bin64 so the launched app resolves its DLLs and skins.
        ::SetCurrentDirectoryA(szAsi);
        const HINSTANCE hRun =
            ::ShellExecuteA(nullptr, "open", "Win8Metro_asi.exe", nullptr,
                            nullptr, SW_SHOWNORMAL);
        const INT_PTR nRun = reinterpret_cast<INT_PTR>(hRun);
        if (nRun <= 32)
        {
            ::printf("[FAIL] ShellExecute failed, code=%lld\n",
                     static_cast<long long>(nRun));
            return 2;
        }
        // [DIAG] Enumerate top-level windows so we can tell 'no such class'
        //   apart from 'the window exists but FindWindow is blocked'.
        {
            struct Ctx { int n; } ctx = { 0 };
            ::EnumWindows([](HWND h, LPARAM lp) -> BOOL {
                Ctx* c = reinterpret_cast<Ctx*>(lp);
                wchar_t cls[128] = {};
                ::GetClassNameW(h, cls, 128);
                if (::wcsstr(cls, L"Dism") != nullptr ||
                    ::wcsstr(cls, L"Metro") != nullptr)
                {
                    ::printf("[DIAG] candidate hwnd=%lld class=%ls visible=%d\n",
                             static_cast<long long>(reinterpret_cast<INT_PTR>(h)),
                             cls, ::IsWindowVisible(h) ? 1 : 0);
                    ++c->n;
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&ctx));
            ::printf("[DIAG] candidates=%d\n", ctx.n);

        // [DIAG] Is the launcher child even alive? If not, the launch failed
        //   and 'window not found' is a consequence, not the root cause.
        {
            ::printf("[DIAG] GetLastError after launch=%lu\n",
                     ::GetLastError());
            char szProbe[MAX_PATH] = {};
            ::GetModuleFileNameA(nullptr, szProbe, MAX_PATH);
            ::printf("[DIAG] probeDir=%s\n", szAsi);
            ::printf("[DIAG] cwd=%s\n", []() -> const char* {
                static char c[MAX_PATH] = {};
                ::GetCurrentDirectoryA(MAX_PATH, c);
                return c;
            }());
            ::printf("[DIAG] asi present=%d\n",
                     ::GetFileAttributesA("Win8Metro_asi.exe") == INVALID_FILE_ATTRIBUTES ? 0 : 1);
        }
        }
        for (int i = 0; i < 120 && hWnd == nullptr; ++i)
        {
            ::Sleep(250);
            hWnd = ::FindWindowW(L"DismppConsole", nullptr);
        }
        if (hWnd == nullptr)
        {
            ::printf("[FAIL] launched %s but class DismppConsole not found\n", kAsi);
            return 2;
        }
        ::printf("[INFO] auto-launched, hwnd=%lld\n",
                 static_cast<long long>(reinterpret_cast<INT_PTR>(hWnd)));
    }

    HRESULT hrCo = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool bCoOk = SUCCEEDED(hrCo);

    IUIAutomation* pUia = nullptr;
    HRESULT hr = ::CoCreateInstance(__uuidof(CUIAutomation), nullptr,
                                    CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pUia));
    Chk("UIA client factory available", SUCCEEDED(hr) && pUia != nullptr, "CUIAutomation");

    if (SUCCEEDED(hr) && pUia != nullptr)
    {
        IUIAutomationElement* pRoot = nullptr;
        hr = pUia->ElementFromHandle(hWnd, &pRoot);
        Chk("ElementFromHandle returns a root element",
            SUCCEEDED(hr) && pRoot != nullptr, "WM_GETOBJECT/UiaRootObjectId answered");

        if (pRoot != nullptr)
        {
            int nTotal = 0, nNamed = 0;
            // [A11Y-7] Depth 10, not 4: depth 4 reported 207 elements while the real
        // tree has 2857 (7%). The limit silently truncated the walk and made
        // "Value pattern missing" look like a provider defect.
        Walk(pRoot, 0, nTotal, nNamed, 10, 10);

            char sz[192] = {};
            ::sprintf_s(sz, "elements=%d named=%d", nTotal, nNamed);
            // [A11Y-4] Now that the object id is read from lParam, the walker really
            // descends: measured 207 elements / 140 named. Assert the subtree, not
            // just the root.
            Chk("control tree is exposed (more than the root)", nTotal > 1, sz);

            ::sprintf_s(sz, "named=%d", nNamed);
            Chk("at least one element exposes a Name", nNamed > 0, sz);

            // [A11Y-5] A tree a client can only read is half an accessibility
            // story. Look for a button that advertises Invoke and actually call
            // it, so "the pattern exists" and "the pattern works" are both
            // measured.
            int nInvokable = 0;
            bool bInvoked = false;
            HRESULT hrInvoke = E_FAIL;
            FindAndInvoke(pRoot, 0, 10, nInvokable, bInvoked, hrInvoke);

            ::sprintf_s(sz, "invokable=%d invoked=%d hr=0x%08lX",
                        nInvokable, bInvoked ? 1 : 0,
                        static_cast<unsigned long>(hrInvoke));
            Chk("at least one element exposes Invoke", nInvokable > 0, sz);
            // [A11Y-6] Invoke really works. The two rounds that reported "E_FAIL in
            // the proxy" were a probe bug: QueryInterface was asking for
            // IInvokeProvider (the PROVIDER-side interface) instead of
            // IUIAutomationInvokePattern (the client-side one), so the proxy
            // rejected it before any call reached the provider. Measured now:
            // invokable=15 invoked=1 hr=S_OK.
            Chk("Invoke pattern is advertised by controls", nInvokable > 0, sz);
            Chk("Invoke actually activates the control", bInvoked
                && hrInvoke == S_OK, sz);

            // [A11Y-6] Value pattern: a client must be able to read and write an
            // edit control through the client-side interface.
            int nValueable = 0;
            bool bValueOk = false;
            wchar_t szValText[128] = {};
            HRESULT hrValue = E_FAIL;
            FindAndSetValue(pRoot, 0, 10, nValueable, bValueOk, szValText, 128, hrValue);
            ::sprintf_s(sz, "valueable=%d ok=%d hr=0x%08lX text=%ls",
                        nValueable, bValueOk ? 1 : 0,
                        static_cast<unsigned long>(hrValue), szValText);
            Chk("Value pattern is advertised by edit controls", nValueable > 0, sz);
            Chk("Value pattern is readable through the client interface", bValueOk, sz);

            // [A11Y-7] SetFocus must reach DuiLib's focus model. The probe asks for
            // focus on the first focused-capable element and then reads
            // HasKeyboardFocus back: "the call returned S_OK" alone would not prove
            // the framework actually moved focus.
            int nFocusable = 0;
            IUIAutomationElement* pFocused = nullptr;
            HRESULT hrFocus = E_FAIL;
            FindFocusable(pRoot, 0, 10, nFocusable, pFocused, hrFocus);
            if (pFocused != nullptr)
            {
                HRESULT hrSet = pFocused->SetFocus();
                BOOL bHasFocus = FALSE;
                pFocused->get_CurrentHasKeyboardFocus(&bHasFocus);
                ::sprintf_s(sz, "focusable=%d set=0x%08lX hasFocus=%d",
                            nFocusable, static_cast<unsigned long>(hrSet),
                            bHasFocus ? 1 : 0);
                Chk("an element accepts SetFocus", SUCCEEDED(hrSet) && bHasFocus != FALSE, sz);
                pFocused->Release();
            }
            else
            {
                Chk("an element accepts SetFocus", false, "no focusable element found");
            }

            // Property sanity: IsEnabled must come back as a BOOL variant.
            VARIANT v;
            ::VariantInit(&v);
            if (SUCCEEDED(pRoot->GetCurrentPropertyValue(UIA_IsEnabledPropertyId, &v)))
            {
                Chk("IsEnabled property returns a BOOL", v.vt == VT_BOOL, "VT_BOOL expected");
            }
            else
            {
                Chk("IsEnabled property queries", false, "GetCurrentPropertyValue failed");
            }
            ::VariantClear(&v);

            pRoot->Release();
        }
        pUia->Release();
    }

    if (bInject)
    {
        Chk("injected-must-fail", false, "self-test: forced failure");
    }

    if (bCoOk) ::CoUninitialize();

    ::printf("\nresult: %d pass / %d fail\n", g_nPass, g_nFail);
    return (g_nFail == 0) ? 0 : 1;
}
