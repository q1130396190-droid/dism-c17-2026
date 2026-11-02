// ============================================================
// ui_interaction_probe.cpp -- verify the interaction regressions that were
// fixed in this round, using the real framework entry points.
//
// Covered:
//   I1  Hiding the focused control moves focus to a usable control
//       (it used to be dropped, so Tab restarted from the first item)
//   I2  Disabling the focused control moves focus away
//   I3  Turning off keyboard-enable on the focused control moves focus away
//   I4  A hidden control can never receive focus
//   I5  Button Space: key-down shows pressed, key-up activates
//   I6  Button Space: losing focus cancels the pending press (no activation)
//   I7  Horizontal scroll keys are Left/Right (Up/Down must not scroll)
//
// Everything runs against a real CPaintManagerUI + window because the focus
// logic lives on the manager; a detached control is exactly the case that used
// to crash, so it cannot be used as the test bed.
// ============================================================

#include "UIlib.h"

#include <windows.h>
#include <cstdio>
#include <string>

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

const wchar_t* kCls = L"UiInteractionProbe";

HWND MakeHost()
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = kCls;
    ::RegisterClassExW(&wc);
    return ::CreateWindowExW(0, kCls, L"probe", WS_OVERLAPPEDWINDOW,
                             0, 0, 800, 600, nullptr, nullptr,
                             ::GetModuleHandleW(nullptr), nullptr);
}

// Counts Activate() calls without needing a notification sink.
class CProbeButton : public CButtonUI
{
public:
    int m_nActivated = 0;
    bool Activate() override { ++m_nActivated; return true; }
};

} // namespace

int main(int argc, char** argv)
{
    // [GATE-SELFTEST] /Inject adds one deliberately failing assertion so this
    // probe can prove its verdict path still works; otherwise a check that
    // silently stopped asserting would look exactly like a passing run.
    bool bInject = false;
    for (int i = 1; i < argc; ++i)
        if (::lstrcmpiA(argv[i], "/Inject") == 0) bInject = true;

    ::SetProcessDPIAware();
    HWND hWnd = MakeHost();
    if (hWnd == nullptr) { ::puts("[FATAL] no host window"); return 2; }

    CPaintManagerUI pm;
    pm.Init(hWnd, L"UiInteractionProbe");

    // Root -> three buttons in a row, plus one hidden button.
    CContainerUI* pRoot = new CContainerUI();
    CProbeButton* pB1 = new CProbeButton();
    CProbeButton* pB2 = new CProbeButton();
    CProbeButton* pB3 = new CProbeButton();
    CProbeButton* pHidden = new CProbeButton();
    pB1->SetName(L"b1"); pB1->SetText(L"one");
    pB2->SetName(L"b2"); pB2->SetText(L"two");
    pB3->SetName(L"b3"); pB3->SetText(L"three");
    pHidden->SetName(L"bh"); pHidden->SetText(L"hidden");
    for (auto* b : { pB1, pB2, pB3, pHidden })
    {
        b->SetFixedHeight(30);
        b->SetKeyboardEnabled(true);
        pRoot->Add(b);
    }
    pHidden->SetVisible(false);
    pm.AttachDialog(pRoot);
    pRoot->SetPos(RECT{ 0, 0, 400, 200 });
    pRoot->NeedUpdate();

    // I1: focus b2, hide it, focus must land on another usable control.
    pm.SetFocus(pB2);
    const bool bHadFocus = (pm.GetFocus() == pB2);
    pB2->SetVisible(false);
    CControlUI* pAfterHide = pm.GetFocus();
    {
        char sz[200];
        ::sprintf_s(sz, "had=%d after=%s", bHadFocus ? 1 : 0,
                    pAfterHide != nullptr ? "some" : "null");
        Chk("hiding the focused control relocates focus",
            bHadFocus && pAfterHide != nullptr && pAfterHide != pB2 &&
            pAfterHide->IsVisible() && pAfterHide->IsEnabled() &&
            pAfterHide->IsKeyboardEnabled(), sz);
    }

    // I2: focus b1, disable it, focus must move away.
    pB2->SetVisible(true);
    pm.SetFocus(pB1);
    pB1->SetEnabled(false);
    CControlUI* pAfterDisable = pm.GetFocus();
    Chk("disabling the focused control relocates focus",
        pAfterDisable != nullptr && pAfterDisable != pB1 &&
        pAfterDisable->IsEnabled() && pAfterDisable->IsVisible(),
        pAfterDisable != nullptr ? "moved" : "null");
    pB1->SetEnabled(true);

    // I3: focus b3, remove its keyboard capability, focus must move away.
    pm.SetFocus(pB3);
    pB3->SetKeyboardEnabled(false);
    CControlUI* pAfterKb = pm.GetFocus();
    Chk("disabling keyboard on the focused control relocates focus",
        pAfterKb != nullptr && pAfterKb != pB3,
        pAfterKb != nullptr ? "moved" : "null");
    pB3->SetKeyboardEnabled(true);

    // I4: a hidden control must never take focus.
    pm.SetFocus(pHidden);
    Chk("hidden control cannot receive focus", pm.GetFocus() != pHidden,
        pm.GetFocus() == nullptr ? "focus=null" : "focus=other");

    // I5: Space semantics. key-down must not activate; key-up must.
    pm.SetFocus(pB1);
    const int nBefore = pB1->m_nActivated;
    TEventUI ev = {};
    ev.Type = UIEVENT_KEYDOWN;
    ev.chKey = VK_SPACE;
    ev.pSender = pB1;
    pB1->Event(ev);
    const int nAfterDown = pB1->m_nActivated;
    ev.Type = UIEVENT_KEYUP;
    pB1->Event(ev);
    const int nAfterUp = pB1->m_nActivated;
    {
        char sz[200];
        ::sprintf_s(sz, "before=%d afterDown=%d afterUp=%d", nBefore, nAfterDown, nAfterUp);
        Chk("Space activates on key-up, not key-down",
            nAfterDown == nBefore && nAfterUp == nBefore + 1, sz);
    }

    // I6: focusing away during a pending Space press cancels the activation.
    pm.SetFocus(pB2);
    const int nBeforeCancel = pB2->m_nActivated;
    ev.Type = UIEVENT_KEYDOWN;
    ev.chKey = VK_SPACE;
    ev.pSender = pB2;
    pB2->Event(ev);
    ev.Type = UIEVENT_KILLFOCUS;
    pB2->Event(ev);
    ev.Type = UIEVENT_KEYUP;
    pB2->Event(ev);
    {
        char sz[200];
        ::sprintf_s(sz, "before=%d after=%d", nBeforeCancel, pB2->m_nActivated);
        Chk("losing focus cancels a pending Space press",
            pB2->m_nActivated == nBeforeCancel, sz);
    }

    // I7: horizontal scroll keys. Up/Down must NOT scroll a horizontal-only
    // container; Left/Right must.
    {
        CContainerUI* pH = new CContainerUI();
        pH->SetName(L"hscroll");
        pH->SetFixedHeight(60);
        // The keyboard branch is guarded by IsVisible() on the scrollbar, and the
        // default is "not shown until layout decides it is needed". Show it
        // explicitly, otherwise this test would silently measure nothing.
        pH->SetShowScrollbar(true);
        pH->EnableScrollBar(false, true);
        for (int i = 0; i < 40; ++i)
        {
            CControlUI* p = new CControlUI();
            p->SetFixedWidth(100);
            pH->Add(p);
        }
        pRoot->Add(pH);
        pRoot->SetPos(RECT{ 0, 0, 400, 400 });
        pH->SetPos(RECT{ 0, 200, 400, 260 });
        // Force a layout pass so the scroll range is computed.
        pH->NeedUpdate();
        pRoot->NeedUpdate();

        CScrollBarUI* pBar = pH->GetHorizontalScrollBar();
        if (pBar != nullptr) {
            pBar->SetVisible(true);
            pBar->SetShow(false);   // keep it laid out but always visible
            pBar->SetScrollRange(400);  // 40 * 100 px of content, 400 px viewport
            pBar->SetLineSize(40);
        }
        const int nStart = (pBar != nullptr) ? pBar->GetScrollPos() : -1;

        TEventUI kev = {};
        kev.Type = UIEVENT_KEYDOWN;
        kev.pSender = pH;
        kev.chKey = VK_DOWN;
        pH->DoEvent(kev);
        const int nAfterDown = (pBar != nullptr) ? pBar->GetScrollPos() : -1;

        kev.chKey = VK_RIGHT;
        pH->DoEvent(kev);
        const int nAfterRight = (pBar != nullptr) ? pBar->GetScrollPos() : -1;

        char sz[220];
        ::sprintf_s(sz, "start=%d afterDown=%d afterRight=%d", nStart, nAfterDown, nAfterRight);
        Chk("horizontal scroll ignores Up/Down, responds to Right",
            pBar != nullptr && nAfterDown == nStart && nAfterRight != nStart, sz);
    }

    ::printf("\nresult: %d pass / %d fail\n", g_nPass, g_nFail);
    ::DestroyWindow(hWnd);
    if (bInject)
    {
        Chk("injected-must-fail", false, "self-test: forced failure");
    }
    return (g_nFail == 0) ? 0 : 1;
}
