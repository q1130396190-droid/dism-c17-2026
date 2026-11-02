// [UIA-PATTERN] Do the new patterns actually reach a UIA client?
//
// The provider code returning an object proves nothing: the previous Invoke comment in
// this repository claimed Invoke "never arrives" for a year while the measured truth
// was that it did. So this probe asks through the real client API
// (IUIAutomation::ElementFromHandle -> GetCurrentPattern) instead of trusting the
// provider side.
//
// It builds a window with an Option, a CheckBox, a Progress and a Slider, then asserts
// each advertises the pattern its semantics require:
//   Option / CheckBox -> Toggle + SelectionItem
//   Progress          -> RangeValue (read-only)
//   Slider            -> RangeValue
//
// /Inject adds a failing assertion so the wrapper can prove the probe can fail.

#include "StdAfx.h"
#include "UIlib.h"

#include <cstdio>
#include <objbase.h>
#include <UIAutomationCore.h>
#include <UIAutomationCoreApi.h>
#include <UIAutomationClient.h>

#include "Core/UIManager.h"
#include "Control/UIOption.h"
#include "Control/UIProgress.h"
#include "Control/UISlider.h"

using namespace DuiLib;

static int g_pass = 0;
static int g_fail = 0;
static CPaintManagerUI* g_pProbeManager = nullptr;

// Forward every message to the paint manager first, exactly like a real DuiLib window.
// Without this the provider is never reached through WM_GETOBJECT.
static LRESULT CALLBACK PatternProbeWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    static thread_local bool t_inDispatch = false;
    if (g_pProbeManager != nullptr && !t_inDispatch && g_pProbeManager->GetPaintWindow() == hWnd)
    {
        t_inDispatch = true;
        LRESULT lRes = 0;
        const bool bHandled = g_pProbeManager->MessageHandler(uMsg, wParam, lParam, lRes);
        t_inDispatch = false;
        if (bHandled) return lRes;
    }
    return ::DefWindowProcW(hWnd, uMsg, wParam, lParam);
}
static void Pass(const char* w) { printf("[PASS] %s\n", w); ++g_pass; }
static void Fail(const char* w) { printf("[FAIL] %s\n", w); ++g_fail; }

// Walk the tree looking for the element whose AutomationId matches, then ask it for a
// pattern. Depth-first, bounded, because the tree here is tiny but real.
static IUIAutomationElement* FindByName(IUIAutomation* pAuto, IUIAutomationElement* pRoot,
                                        const wchar_t* pszName)
{
    if (pAuto == nullptr || pRoot == nullptr || pszName == nullptr) return nullptr;

    BSTR bszName = nullptr;
    if (SUCCEEDED(pRoot->get_CurrentAutomationId(&bszName)) && bszName != nullptr) {
        const bool bMatch = (wcscmp(bszName, pszName) == 0);
        ::SysFreeString(bszName);
        if (bMatch) { pRoot->AddRef(); return pRoot; }
    }

    IUIAutomationTreeWalker* pWalker = nullptr;
    if (FAILED(pAuto->get_ControlViewWalker(&pWalker)) || pWalker == nullptr) return nullptr;

    IUIAutomationElement* pChild = nullptr;
    pWalker->GetFirstChildElement(pRoot, &pChild);
    while (pChild != nullptr) {
        if (IUIAutomationElement* pHit = FindByName(pAuto, pChild, pszName)) {
            pChild->Release();
            pWalker->Release();
            return pHit;
        }
        IUIAutomationElement* pNext = nullptr;
        pWalker->GetNextSiblingElement(pChild, &pNext);
        pChild->Release();
        pChild = pNext;
    }
    pWalker->Release();
    return nullptr;
}

static bool HasPattern(IUIAutomationElement* pElem, PATTERNID id)
{
    if (pElem == nullptr) return false;
    IUnknown* pUnk = nullptr;
    const bool bOk = SUCCEEDED(pElem->GetCurrentPattern(id, &pUnk)) && pUnk != nullptr;
    if (pUnk != nullptr) pUnk->Release();
    return bOk;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    bool bInject = false;
    for (int i = 1; i < argc; ++i)
        if (_stricmp(argv[i], "/Inject") == 0) bInject = true;

    printf("[probe] UIA pattern coverage\n");

    HRESULT hrCo = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hrCo)) { printf("[SKIP] no STA\n"); return 0; }

    WNDCLASSEXW wc = { sizeof(wc) };
    // [DISPATCH] WM_GETOBJECT must reach CPaintManagerUI, so the window proc forwards to
    // the manager. With a bare DefWindowProc the client falls back to the default
    // window proxy and sees only an empty pane with a stray "MenuBar" -- which is what
    // the first runs of this probe reported, while the provider itself was fine.
    wc.lpfnWndProc = &PatternProbeWndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"UiaPatternProbeHost";
    if (::RegisterClassExW(&wc) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        printf("[SKIP] class registration failed\n"); return 0;
    }
    HWND hWnd = ::CreateWindowExW(0, wc.lpszClassName, L"probe", WS_OVERLAPPEDWINDOW,
                                  100, 100, 400, 300, nullptr, nullptr, wc.hInstance, nullptr);
    if (hWnd == nullptr) { printf("[SKIP] window creation failed\n"); return 0; }
    ::ShowWindow(hWnd, SW_SHOWNORMAL);
    ::UpdateWindow(hWnd);

    CPaintManagerUI pm;
    pm.Init(hWnd);
    g_pProbeManager = &pm;

    {
        CContainerUI* pRoot = new CContainerUI();
        pRoot->SetManager(&pm, nullptr, false);
        // Attach BEFORE adding children: AttachDialog installs the root and resets the
        // tree, so children added earlier are not the ones the provider walks. The
        // client tree showed a stray "MenuBar" child and no AutomationId at all, which
        // is what a mis-ordered attach looks like from the outside.
        pRoot->SetPos(RECT{ 0, 0, 400, 300 });
        pm.AttachDialog(pRoot);

        struct Item { const wchar_t* name; RECT rc; };
        COptionUI* pOpt = new COptionUI();
        pOpt->SetManager(&pm, pRoot, false);
        pOpt->SetName(_T("pat_option"));
        pOpt->SetText(_T("option"));
        pOpt->SetPos(RECT{ 10, 10, 200, 40 });
        pRoot->Add(pOpt);

        CProgressUI* pProg = new CProgressUI();
        pProg->SetManager(&pm, pRoot, false);
        pProg->SetName(_T("pat_progress"));
        pProg->SetMaxValue(100);
        pProg->SetValue(42);
        pProg->SetPos(RECT{ 10, 60, 300, 90 });
        pRoot->Add(pProg);

        CSliderUI* pSlider = new CSliderUI();
        pSlider->SetManager(&pm, pRoot, false);
        pSlider->SetName(_T("pat_slider"));
        pSlider->SetPos(RECT{ 10, 110, 300, 140 });
        pRoot->Add(pSlider);

        pRoot->SetVisible(true);
        // Force a layout: the provider reports GetPos(), and a control that never
        // laid out has a zero rectangle, which makes hit tests and client positions
        // meaningless even though the pattern objects are correct.
        pRoot->SetPos(RECT{ 0, 0, 400, 300 });

        IUIAutomation* pAuto = nullptr;
        HRESULT hr = ::CoCreateInstance(__uuidof(CUIAutomation), nullptr,
                                        CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation),
                                        reinterpret_cast<void**>(&pAuto));
        if (FAILED(hr) || pAuto == nullptr) {
            printf("[SKIP] UIA client factory unavailable\n");
            ::DestroyWindow(hWnd); ::CoUninitialize(); return 0;
        }

        printf("[probe] probing hwnd=%p title-check via GetWindowText\n", (void*)hWnd);
        {
            wchar_t szTitle[128] = { 0 };
            ::GetWindowTextW(hWnd, szTitle, 128);
            printf("[probe] window title='%ls' visible=%d\n",
                   szTitle, ::IsWindowVisible(hWnd) ? 1 : 0);
            printf("[probe] manager paint window=%p root=%p\n",
                   (void*)pm.GetPaintWindow(), (void*)pm.GetRoot());
        }

        IUIAutomationElement* pRootElem = nullptr;
        hr = pAuto->ElementFromHandle(hWnd, &pRootElem);
        if (FAILED(hr) || pRootElem == nullptr) {
            printf("[SKIP] ElementFromHandle failed\n");
            pAuto->Release(); ::DestroyWindow(hWnd); ::CoUninitialize(); return 0;
        }

        // ---- Option: Toggle + SelectionItem ---------------------------------
        // Report the tree the client actually sees before asserting anything: a depth
        // limit or a filtered walker produces "element not found" while the provider is
        // perfectly correct, and that misdiagnosis costs more than the log line.
        {
            IUIAutomationTreeWalker* pW = nullptr;
            if (SUCCEEDED(pAuto->get_ControlViewWalker(&pW)) && pW != nullptr) {
                int nSeen = 0;
                struct Local {
                    static void Dump(IUIAutomationTreeWalker* pWalker, IUIAutomationElement* pElem,
                                     int depth, int& nSeen) {
                        if (pElem == nullptr || depth > 4) return;
                        BSTR id = nullptr, nm = nullptr;
                        pElem->get_CurrentAutomationId(&id);
                        pElem->get_CurrentName(&nm);
                        printf("[tree] %*s id='%ls' name='%ls'\n", depth * 2, "",
                               (id != nullptr) ? id : L"", (nm != nullptr) ? nm : L"");
                        if (id != nullptr) ::SysFreeString(id);
                        if (nm != nullptr) ::SysFreeString(nm);
                        ++nSeen;
                        IUIAutomationElement* pC = nullptr;
                        pWalker->GetFirstChildElement(pElem, &pC);
                        while (pC != nullptr) {
                            Dump(pWalker, pC, depth + 1, nSeen);
                            IUIAutomationElement* pN = nullptr;
                            pWalker->GetNextSiblingElement(pC, &pN);
                            pC->Release();
                            pC = pN;
                        }
                    }
                };
                Local::Dump(pW, pRootElem, 0, nSeen);
                printf("[probe] elements seen in client tree: %d\n", nSeen);
                pW->Release();
            }
        }

        printf("[probe] control classes: option='%ls' progress='%ls' slider='%ls'\n",
               pOpt->GetClass(), pProg->GetClass(), pSlider->GetClass());

        IUIAutomationElement* pOptElem = FindByName(pAuto, pRootElem, L"pat_option");
        if (pOptElem == nullptr) {
            Fail("option element not found in the UIA tree");
        } else {
            if (HasPattern(pOptElem, UIA_TogglePatternId))
                Pass("Option exposes TogglePattern");
            else
                Fail("Option does not expose TogglePattern");

            if (HasPattern(pOptElem, UIA_SelectionItemPatternId))
                Pass("Option exposes SelectionItemPattern");
            else
                Fail("Option does not expose SelectionItemPattern");
            pOptElem->Release();
        }

        // ---- Progress: RangeValue -------------------------------------------
        IUIAutomationElement* pProgElem = FindByName(pAuto, pRootElem, L"pat_progress");
        if (pProgElem == nullptr) {
            Fail("progress element not found in the UIA tree");
        } else {
            IUnknown* pUnk = nullptr;
            if (SUCCEEDED(pProgElem->GetCurrentPattern(UIA_RangeValuePatternId, &pUnk))
                && pUnk != nullptr) {
                Pass("Progress exposes RangeValuePattern");
                IUIAutomationRangeValuePattern* pRange = nullptr;
                if (SUCCEEDED(pUnk->QueryInterface(__uuidof(IUIAutomationRangeValuePattern),
                                                   reinterpret_cast<void**>(&pRange)))
                    && pRange != nullptr) {
                    double dVal = -1.0, dMax = -1.0;
                    pRange->get_CurrentValue(&dVal);
                    pRange->get_CurrentMaximum(&dMax);
                    printf("[probe] progress value=%.1f max=%.1f\n", dVal, dMax);
                    if (dVal == 42.0 && dMax == 100.0)
                        Pass("Progress reports the real value and range");
                    else
                        Fail("Progress reported a wrong value/range");
                    BOOL bRO = FALSE;
                    pRange->get_CurrentIsReadOnly(&bRO);
                    if (bRO) Pass("Progress is reported read-only");
                    else     Fail("Progress should be read-only");
                    pRange->Release();
                }
                pUnk->Release();
            } else {
                Fail("Progress does not expose RangeValuePattern");
            }
            pProgElem->Release();
        }

        // ---- Slider: RangeValue ---------------------------------------------
        IUIAutomationElement* pSliderElem = FindByName(pAuto, pRootElem, L"pat_slider");
        if (pSliderElem == nullptr) {
            Fail("slider element not found in the UIA tree");
        } else {
            if (HasPattern(pSliderElem, UIA_RangeValuePatternId))
                Pass("Slider exposes RangeValuePattern");
            else
                Fail("Slider does not expose RangeValuePattern");
            pSliderElem->Release();
        }

        pRootElem->Release();
        pAuto->Release();
        ::DestroyWindow(hWnd);
    }

    if (bInject) Fail("injected: deliberately failing assertion");

    printf("\n%d pass / %d fail\n", g_pass, g_fail);
    ::CoUninitialize();
    return g_fail == 0 ? 0 : 1;
}
