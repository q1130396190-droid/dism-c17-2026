// Dump the name and rectangle of every control in a live Win8Metro window.
//
// Why this exists
// ---------------
// The dark skin paints button fill and page background with the SAME colour
// (32,32,32), so locating buttons by pixel is unreliable: only the 1px border
// separates them, and a scan by "differs from background" finds almost nothing.
// Measuring the layout that way produced contradictory numbers twice.
//
// The program already knows every control's rectangle. UIA exposes it. Asking the
// program is both exact and independent of the theme, so a question like "did
// these four buttons disappear and is the remaining row still laid out sanely"
// gets an answer instead of an estimate.
//
// Usage:
//   ui_dump_rects.exe <hwnd-decimal> [substring]
// Prints:  name<TAB>left<TAB>top<TAB>width<TAB>height
// Exit:    0 dumped at least one element, 2 nothing matched / no window
#include <windows.h>
#include <objbase.h>
#include <UIAutomationCore.h>
#include <UIAutomationClient.h>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "uiautomationcore.lib")

struct Ctx
{
    const char* pszFilter;
    int         nFound;
};

static void Emit(IUIAutomationElement* pElem)
{
    BSTR bstrName = nullptr;
    if (FAILED(pElem->get_CurrentName(&bstrName)) || bstrName == nullptr) return;

    char szName[512] = {};
    ::WideCharToMultiByte(CP_UTF8, 0, bstrName, -1, szName, sizeof(szName) - 1,
                          nullptr, nullptr);
    ::SysFreeString(bstrName);
    if (szName[0] == 0) return;

    RECT rc = {};
    if (FAILED(pElem->get_CurrentBoundingRectangle(&rc))) return;

    ::printf("%s\t%ld\t%ld\t%ld\t%ld\n", szName, rc.left, rc.top,
             rc.right - rc.left, rc.bottom - rc.top);
}

static void Walk(IUIAutomationElement* pElem, IUIAutomationTreeWalker* pWalker,
                 Ctx& ctx, int nDepth, int nDepthLimit)
{
    if (pElem == nullptr || nDepth > nDepthLimit) return;

    bool bMatch = true;
    if (ctx.pszFilter && *ctx.pszFilter)
    {
        BSTR bstrName = nullptr;
        bMatch = false;
        if (SUCCEEDED(pElem->get_CurrentName(&bstrName)) && bstrName != nullptr)
        {
            char szName[512] = {};
            ::WideCharToMultiByte(CP_UTF8, 0, bstrName, -1, szName, sizeof(szName) - 1,
                                  nullptr, nullptr);
            bMatch = (::strstr(szName, ctx.pszFilter) != nullptr);
            ::SysFreeString(bstrName);
        }
    }
    if (bMatch && pElem != nullptr)
    {
        Emit(pElem);
        // Do not count the root pane as a hit; it has an empty name anyway.
        ++ctx.nFound;
    }

    IUIAutomationElement* pChild = nullptr;
    HRESULT hr = pWalker->GetFirstChildElement(pElem, &pChild);
    int nGuard = 0;
    while (SUCCEEDED(hr) && pChild != nullptr && nGuard++ < 5000)
    {
        Walk(pChild, pWalker, ctx, nDepth + 1, nDepthLimit);
        IUIAutomationElement* pNext = nullptr;
        hr = pWalker->GetNextSiblingElement(pChild, &pNext);
        pChild->Release();
        pChild = pNext;
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        ::printf("[FAIL] usage: ui_dump_rects.exe <hwnd-decimal> [substring]\n");
        return 2;
    }

    HWND hWnd = reinterpret_cast<HWND>(static_cast<INT_PTR>(_strtoi64(argv[1], nullptr, 10)));
    if (hWnd == nullptr || !::IsWindow(hWnd))
    {
        ::printf("[FAIL] not a window: %s\n", argv[1]);
        return 2;
    }
    const char* pszFilter = (argc >= 3) ? argv[2] : "";

    HRESULT hrCo = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hrCo))
    {
        ::printf("[FAIL] CoInitializeEx hr=0x%08lX\n", hrCo);
        return 2;
    }

    IUIAutomation* pUia = nullptr;
    if (FAILED(::CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&pUia))))
    {
        ::printf("[FAIL] CUIAutomation unavailable\n");
        return 2;
    }

    IUIAutomationElement* pRoot = nullptr;
    if (FAILED(pUia->ElementFromHandle(hWnd, &pRoot)) || pRoot == nullptr)
    {
        ::printf("[FAIL] ElementFromHandle returned nothing\n");
        pUia->Release();
        return 2;
    }

    IUIAutomationTreeWalker* pWalker = nullptr;
    if (FAILED(pUia->get_ControlViewWalker(&pWalker)) || pWalker == nullptr)
    {
        ::printf("[FAIL] no tree walker\n");
        pRoot->Release();
        pUia->Release();
        return 2;
    }

    Ctx ctx = { pszFilter, 0 };
    ::printf("=== name\tleft\ttop\twidth\theight\n");
    // Depth 30: the cleanup page nests page -> layout -> row -> button, and the
    // earlier probe showed a shallow limit silently under-reports the tree.
    Walk(pRoot, pWalker, ctx, 0, 30);

    pWalker->Release();
    pRoot->Release();
    pUia->Release();

    ::printf("=== matched=%d\n", ctx.nFound);
    return (ctx.nFound > 0) ? 0 : 2;
}
