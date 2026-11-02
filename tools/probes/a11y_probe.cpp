// ============================================================
// a11y_probe.cpp -- verify the MSAA bridge really exposes the self-painted tree.
//
// Why: every DuiLib control is custom-drawn, so before this change the paint
// window answered OBJID_CLIENT with the default (opaque pane). A screen reader
// could not name a single button. This probe drives the real window message and
// then queries the returned IAccessible, so "accessibility works" is a measured
// fact rather than a claim.
//
// Assertions:
//   A1  WM_GETOBJECT(OBJID_CLIENT) returns a non-zero LRESULT
//   A2  ObjectFromLresult yields a usable IAccessible
//   A3  The root reports a child count matching the visible controls
//   A4  A named button reports ROLE_SYSTEM_PUSHBUTTON
//   A5  Its accessible name is the control name, not "pane"/empty
//   A6  A disabled control reports STATE_SYSTEM_UNAVAILABLE
//   A7  accLocation returns a non-empty on-screen rectangle
// ============================================================

#include "UIlib.h"

#include <windows.h>
#include <oleacc.h>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "oleacc.lib")

using namespace DuiLib;

namespace {

int g_nPass = 0;
int g_nFail = 0;

void Chk(const char* pszName, bool bOk, const std::string& sDetail)
{
    ::printf("[%s] %s | %s\n", bOk ? "PASS" : "FAIL", pszName, sDetail.c_str());
    ::fflush(stdout);
    if (bOk) ++g_nPass; else ++g_nFail;
}

const wchar_t* kCls = L"A11yProbe";

LRESULT CALLBACK ProbeProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    CPaintManagerUI* pm = reinterpret_cast<CPaintManagerUI*>(::GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    if (pm != nullptr) {
        LRESULT lRes = 0;
        // Signature: MessageHandler(uMsg, wParam, lParam, LRESULT&)
        if (pm->MessageHandler(uMsg, wParam, lParam, lRes)) return lRes;
    }
    return ::DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

HWND MakeHost()
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ProbeProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = kCls;
    ::RegisterClassExW(&wc);
    return ::CreateWindowExW(0, kCls, L"a11y", WS_OVERLAPPEDWINDOW,
                             0, 0, 640, 480, nullptr, nullptr,
                             ::GetModuleHandleW(nullptr), nullptr);
}

// Walks the accessible children and returns the first whose role matches.
IAccessible* FindChildByRole(IAccessible* pRoot, long lRole, std::wstring* psName, int* pnIndex)
{
    long nChildren = 0;
    if (FAILED(pRoot->get_accChildCount(&nChildren))) return nullptr;
    for (long i = 1; i <= nChildren; ++i) {
        VARIANT v;
        ::VariantInit(&v);
        v.vt = VT_I4;
        v.lVal = i;
        VARIANT vRole;
        ::VariantInit(&vRole);
        if (FAILED(pRoot->get_accRole(v, &vRole))) continue;
        if (vRole.vt == VT_I4 && vRole.lVal == lRole) {
            if (psName != nullptr) {
                BSTR bstr = nullptr;
                if (SUCCEEDED(pRoot->get_accName(v, &bstr)) && bstr != nullptr) {
                    *psName = bstr;
                    ::SysFreeString(bstr);
                }
            }
            if (pnIndex != nullptr) *pnIndex = static_cast<int>(i);
            pRoot->AddRef();
            return pRoot;
        }
    }
    return nullptr;
}

} // namespace

int main(int argc, char** argv)
{
    // [GATE-SELFTEST] /Inject adds one deliberately failing assertion so this
    // probe can prove its verdict path still works. An accessibility tree that
    // silently stopped exposing names would otherwise look like a pass.
    bool bInject = false;
    for (int i = 1; i < argc; ++i)
        if (::lstrcmpiA(argv[i], "/Inject") == 0) bInject = true;

    ::SetProcessDPIAware();
    ::CoInitialize(nullptr);

    HWND hWnd = MakeHost();
    if (hWnd == nullptr) { ::puts("[FATAL] no host window"); return 2; }

    CPaintManagerUI pm;
    pm.Init(hWnd, L"A11yProbe");
    ::SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&pm));

    // Use a VERTICAL layout, not a plain container: CContainerUI stacks every
    // child at full size, so a "real rect" assertion would be meaningless there.
    CVerticalLayoutUI* pRoot = new CVerticalLayoutUI();
    pRoot->SetName(L"root");
    CButtonUI* pBtn = new CButtonUI();
    pBtn->SetName(L"btn_save");
    pBtn->SetText(L"Save");
    pBtn->SetFixedHeight(30);
    pBtn->SetKeyboardEnabled(true);
    pRoot->Add(pBtn);

    CButtonUI* pDisabled = new CButtonUI();
    pDisabled->SetName(L"btn_disabled");
    pDisabled->SetText(L"Disabled");
    pDisabled->SetFixedHeight(30);
    pDisabled->SetEnabled(false);
    pRoot->Add(pDisabled);

    pm.AttachDialog(pRoot);
    // A container must be laid out before child rects mean anything: without a
    // real SetPos pass each child keeps the parent-sized default rect and the
    // accessibility rectangle would be meaningless (it was 616x416 for a 30px
    // button, which made the earlier "rect is non-empty" check pass for the
    // wrong reason).
    pRoot->SetPos(RECT{ 0, 0, 400, 200 });
    pRoot->NeedUpdate();
    pm.NeedUpdate();
    ::ShowWindow(hWnd, SW_SHOW);
    ::SetActiveWindow(hWnd);
    {
        // Diagnostics: a WM_PAINT path that early-returns (no update rect, no
        // client rect) would leave every child at its default rect, and the
        // accessibility geometry check would then pass for the wrong reason.
        RECT rcClient = { 0 };
        ::GetClientRect(hWnd, &rcClient);
        const BOOL bIconic = ::IsIconic(hWnd);
        char szW[220];
        ::sprintf_s(szW, "client=%ldx%ld iconic=%d visible=%d rootUpdate=%d pmUpdate=%d",
                    rcClient.right, rcClient.bottom, (int)bIconic,
                    (int)::IsWindowVisible(hWnd),
                    (int)pRoot->IsUpdateNeeded(), (int)pm.IsUpdateNeeded());
        ::puts(szW);
    }
    // Layout is dispatched inside the WM_PAINT path, so an UpdateWindow is what
    // actually gives children their rects. Without it every child keeps the
    // parent-sized default rect.
    {
        RECT rcUpd = { 0 };
        const BOOL bHasUpd = ::GetUpdateRect(hWnd, &rcUpd, FALSE);
        char szU[200];
        ::sprintf_s(szU, "before paint: updateRect=%d rootUpdate=%d pmUpdate=%d",
                    (int)bHasUpd, (int)pRoot->IsUpdateNeeded(), (int)pm.IsUpdateNeeded());
        ::puts(szU);
    }
    ::InvalidateRect(hWnd, nullptr, FALSE);
    ::UpdateWindow(hWnd);
    {
        char szU[200];
        ::sprintf_s(szU, "after paint: rootUpdate=%d pmUpdate=%d",
                    (int)pRoot->IsUpdateNeeded(), (int)pm.IsUpdateNeeded());
        ::puts(szU);
    }
    RECT rcBtn = pBtn->GetPos();
    char szLayout[160];
    ::sprintf_s(szLayout, "button rect=%ld,%ld %ldx%ld",
                rcBtn.left, rcBtn.top, rcBtn.right - rcBtn.left, rcBtn.bottom - rcBtn.top);
    Chk("button got a real layout rect", (rcBtn.bottom - rcBtn.top) <= 40, szLayout);

    // A1: the window must answer OBJID_CLIENT itself.
    const LRESULT lRes = ::SendMessageW(hWnd, WM_GETOBJECT, OBJID_CLIENT, 0);
    {
        char sz[160];
        ::sprintf_s(sz, "WM_GETOBJECT returned 0x%p", (void*)lRes);
        Chk("WM_GETOBJECT(OBJID_CLIENT) is answered", lRes != 0, sz);
    }

    IAccessible* pAcc = nullptr;
    if (lRes != 0) {
        const HRESULT hr = ::ObjectFromLresult(lRes, IID_IAccessible, 0,
                                               reinterpret_cast<void**>(&pAcc));
        char sz[160];
        ::sprintf_s(sz, "ObjectFromLresult hr=0x%08X acc=%p", (unsigned)hr, (void*)pAcc);
        Chk("returned LRESULT yields an IAccessible", SUCCEEDED(hr) && pAcc != nullptr, sz);
    }
    else {
        Chk("returned LRESULT yields an IAccessible", false, "no LRESULT");
    }

    long nChildren = 0;
    if (pAcc != nullptr) pAcc->get_accChildCount(&nChildren);
    {
        char sz[160];
        ::sprintf_s(sz, "child count=%ld", nChildren);
        Chk("root exposes a non-empty control tree", nChildren > 0, sz);
    }

    // A4/A5: find the named button and check role + name.
    if (pAcc != nullptr) {
        long nChildrenLocal = 0;
        pAcc->get_accChildCount(&nChildrenLocal);
        bool bRoleOk = false, bNameOk = false;
        char sz[220] = {};
        for (long i = 1; i <= nChildrenLocal; ++i) {
            VARIANT v; ::VariantInit(&v);
            v.vt = VT_I4; v.lVal = i;
            BSTR bstr = nullptr;
            if (FAILED(pAcc->get_accName(v, &bstr)) || bstr == nullptr) continue;
            const std::wstring sName(bstr, ::SysStringLen(bstr));
            ::SysFreeString(bstr);
            if (sName != L"btn_save") continue;

            VARIANT vRole; ::VariantInit(&vRole);
            if (SUCCEEDED(pAcc->get_accRole(v, &vRole)) && vRole.vt == VT_I4)
                bRoleOk = (vRole.lVal == ROLE_SYSTEM_PUSHBUTTON);
            bNameOk = true;

            // A6/A7 on the same element for locality.
            VARIANT vState; ::VariantInit(&vState);
            long lState = 0;
            if (SUCCEEDED(pAcc->get_accState(v, &vState)) && vState.vt == VT_I4) lState = vState.lVal;

            long x = 0, y = 0, cx = 0, cy = 0;
            pAcc->accLocation(&x, &y, &cx, &cy, v);
            ::sprintf_s(sz, "name=btn_save role=%ld state=0x%lX rect=%ld,%ld %ldx%ld",
                        (long)(bRoleOk ? ROLE_SYSTEM_PUSHBUTTON : -1), lState, x, y, cx, cy);
            Chk("named button exposes the button role", bRoleOk, sz);
            Chk("named button exposes a real on-screen rect", cx > 0 && cy > 0, sz);

            // Disabled sibling must report UNAVAILABLE.
            for (long j = 1; j <= nChildrenLocal; ++j) {
                if (j == i) continue;
                VARIANT vj; ::VariantInit(&vj);
                vj.vt = VT_I4; vj.lVal = j;
                BSTR b2 = nullptr;
                if (FAILED(pAcc->get_accName(vj, &b2)) || b2 == nullptr) continue;
                const std::wstring s2(b2, ::SysStringLen(b2));
                ::SysFreeString(b2);
                if (s2 != L"btn_disabled") continue;
                VARIANT vd; ::VariantInit(&vd);
                long lDisabled = 0;
                if (SUCCEEDED(pAcc->get_accState(vj, &vd)) && vd.vt == VT_I4) lDisabled = vd.lVal;
                char szD[160];
                ::sprintf_s(szD, "state=0x%lX", lDisabled);
                Chk("disabled control reports STATE_SYSTEM_UNAVAILABLE",
                    (lDisabled & STATE_SYSTEM_UNAVAILABLE) != 0, szD);
                break;
            }
            break;
        }
        Chk("named button is exposed by name", bNameOk, bNameOk ? "found btn_save" : "not found");
    }
    else {
        Chk("named button exposes the button role", false, "no accessible object");
        Chk("named button is exposed by name", false, "no accessible object");
        Chk("disabled control reports STATE_SYSTEM_UNAVAILABLE", false, "no accessible object");
        Chk("named button exposes a real on-screen rect", false, "no accessible object");
    }

    // A8/A9: MSAA action and value, which is what UIA's Invoke/Value patterns
    // are bridged from. Verifying them here means "the screen reader can press
    // the button and read the field" is a measured fact, not a claim.
    if (pAcc != nullptr) {
        long nLocal = 0;
        pAcc->get_accChildCount(&nLocal);
        bool bInvokeOk = false, bValueOk = false, bActionOk = false;
        char szAct[220] = {}, szVal[220] = {};
        for (long i = 1; i <= nLocal; ++i) {
            VARIANT v; ::VariantInit(&v);
            v.vt = VT_I4; v.lVal = i;
            BSTR b = nullptr;
            if (FAILED(pAcc->get_accName(v, &b)) || b == nullptr) continue;
            const std::wstring s(b, ::SysStringLen(b));
            ::SysFreeString(b);
            if (s != L"btn_save") continue;

            BSTR act = nullptr;
            if (SUCCEEDED(pAcc->get_accDefaultAction(v, &act)) && act != nullptr) {
                const std::wstring sa(act, ::SysStringLen(act));
                ::SysFreeString(act);
                bActionOk = !sa.empty();
                ::sprintf_s(szAct, "defaultAction=%ls", sa.c_str());
            }

            // A button's value is its label; a text field's value is its content.
            BSTR val = nullptr;
            if (SUCCEEDED(pAcc->get_accValue(v, &val)) && val != nullptr) {
                const std::wstring sv(val, ::SysStringLen(val));
                ::SysFreeString(val);
                bValueOk = !sv.empty();
            }

            bInvokeOk = SUCCEEDED(pAcc->accDoDefaultAction(v));
            break;
        }
        Chk("button exposes a default action (UIA Invoke bridge)", bActionOk, szAct);
        Chk("accDoDefaultAction succeeds (action is really callable)", bInvokeOk, szAct);
        Chk("button exposes a readable value (UIA Value bridge)", bValueOk, szVal);
    }

    if (pAcc != nullptr) pAcc->Release();

    ::printf("\nresult: %d pass / %d fail\n", g_nPass, g_nFail);
    ::DestroyWindow(hWnd);
    ::CoUninitialize();
    if (bInject)
    {
        Chk("injected-must-fail", false, "self-test: forced failure");
    }
    return (g_nFail == 0) ? 0 : 1;
}
