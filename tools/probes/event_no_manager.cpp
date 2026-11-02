// R250: DoEvent 全事件类型崩溃面实测
//
// 背景：静态扫描报 DoEvent 族有 41 处未守卫 m_pManager 解引用。
// 但静态计数不等于可达崩溃——事件类型不同，走的代码路径完全不同。
// 只有实际派发每种事件，才知道哪些真的会崩。
//
// 覆盖：UIEVENT_KEYDOWN / KEYUP / CHAR / SYSKEY / MOUSEMOVE / MOUSELEAVE /
//       MOUSEENTER / MOUSEHOVER / BUTTONDOWN / BUTTONUP / RBUTTONDOWN /
//       RBUTTONUP / MBUTTONDOWN / MBUTTONUP / DBLCLICK / CONTEXTMENU /
//       SCROLLWHEEL / KILLFOCUS / SETFOCUS / WINDOWSIZE / SETCURSOR / TIMER
#include "StdAfx.h"
#include "UIlib.h"
#include <windows.h>
#include <cstdio>

#include "Control/UIButton.h"
#include "Control/UIOption.h"
#include "Control/UILabel.h"
#include "Control/UIEdit.h"
#include "Control/UIList.h"
#include "Control/UIListEx.h"
#include "Control/UICombo.h"
#include "Control/UIComboBox.h"
#include "Control/UISlider.h"
#include "Control/UIProgress.h"
#include "Control/UIScrollBar.h"
#include "Control/UIText.h"
#include "Control/UIHotKey.h"
#include "Control/UIDateTime.h"
#include "Control/UIIPAddress.h"
#include "Control/UIRichEdit.h"
#include "Control/UITreeView.h"
#include "Control/UIRollText.h"
#include "Control/UIGifAnim.h"
#include "Control/UIRing.h"
#include "Control/UIPageControl.h"
#include "Layout/UITabLayout.h"
#include "Layout/UIVerticalLayout.h"

using DuiLib::CControlUI;

typedef CControlUI* (*MakeFn)();
typedef void (*EvFn)(CControlUI*);

static int g_crash = 0;
static int g_tested = 0;
static int g_events = 0;

static int GuardedMake(MakeFn fn, CControlUI** out)
{
    __try { *out = fn(); return 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { *out = nullptr; return 1; }
}

// 单事件派发：SEH 隔离，无析构对象
static int GuardedEvent(CControlUI* p, int type)
{
    __try {
        DuiLib::TEventUI ev = { 0 };
        ev.Type = type;
        ev.ptMouse.x = 7;
        ev.ptMouse.y = 9;
        ev.chKey = _T('A');
        ev.wKeyState = 0;
        ev.dwTimestamp = 0;
        ev.pSender = p;
        p->DoEvent(ev);
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

static int GuardedDelete(CControlUI* p)
{
    __try { delete p; return 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 1; }
}

struct EvDef { const char* name; int type; };

static const EvDef kEvents[] = {
    { "KEYDOWN",      DuiLib::UIEVENT_KEYDOWN },
    { "KEYUP",        DuiLib::UIEVENT_KEYUP },
    { "CHAR",         DuiLib::UIEVENT_CHAR },
    { "SYSKEY",       DuiLib::UIEVENT_SYSKEY },
    { "MOUSEMOVE",    DuiLib::UIEVENT_MOUSEMOVE },
    { "MOUSELEAVE",   DuiLib::UIEVENT_MOUSELEAVE },
    { "MOUSEENTER",   DuiLib::UIEVENT_MOUSEENTER },
    { "MOUSEHOVER",   DuiLib::UIEVENT_MOUSEHOVER },
    { "BUTTONDOWN",   DuiLib::UIEVENT_BUTTONDOWN },
    { "BUTTONUP",     DuiLib::UIEVENT_BUTTONUP },
    { "RBUTTONDOWN",  DuiLib::UIEVENT_RBUTTONDOWN },
    { "RBUTTONUP",    DuiLib::UIEVENT_RBUTTONUP },
    { "MBUTTONDOWN",  DuiLib::UIEVENT_MBUTTONDOWN },
    { "MBUTTONUP",    DuiLib::UIEVENT_MBUTTONUP },
    { "DBLCLICK",     DuiLib::UIEVENT_DBLCLICK },
    { "CONTEXTMENU",  DuiLib::UIEVENT_CONTEXTMENU },
    { "SCROLLWHEEL",  DuiLib::UIEVENT_SCROLLWHEEL },
    { "KILLFOCUS",    DuiLib::UIEVENT_KILLFOCUS },
    { "SETFOCUS",     DuiLib::UIEVENT_SETFOCUS },
    { "WINDOWSIZE",   DuiLib::UIEVENT_WINDOWSIZE },
    { "SETCURSOR",    DuiLib::UIEVENT_SETCURSOR },
    { "TIMER",        DuiLib::UIEVENT_TIMER },
};

#define MAKER(CLS) []() -> CControlUI* { return new DuiLib::CLS(); }

struct ClassDef { const char* name; MakeFn make; };

static const ClassDef kClasses[] = {
    { "Button",         MAKER(CButtonUI) },
    { "Option",         MAKER(COptionUI) },
    { "Label",          MAKER(CLabelUI) },
    { "Edit",           MAKER(CEditUI) },
    { "List",           MAKER(CListUI) },
    { "ListEx",         MAKER(CListExUI) },
    { "Combo",          MAKER(CComboUI) },
    { "ComboBox",       MAKER(CComboBoxUI) },
    { "Slider",         MAKER(CSliderUI) },
    { "Progress",       MAKER(CProgressUI) },
    { "ScrollBar",      MAKER(CScrollBarUI) },
    { "Text",           MAKER(CTextUI) },
    { "HotKey",         MAKER(CHotKeyUI) },
    { "DateTime",       MAKER(CDateTimeUI) },
    { "IPAddress",      MAKER(CIPAddressUI) },
    { "RichEdit",       MAKER(CRichEditUI) },
    { "TreeView",       MAKER(CTreeViewUI) },
    { "RollText",       MAKER(CRollTextUI) },
    { "GifAnim",        MAKER(CGifAnimUI) },
    { "Ring",           MAKER(CRingUI) },
    { "PageControl",    MAKER(CPageControlUI) },
    { "TabLayout",      MAKER(CTabLayoutUI) },
    { "VerticalLayout", MAKER(CVerticalLayoutUI) },
};

static void RunOne(const char* cls, MakeFn make)
{
    CControlUI* p = nullptr;
    if (GuardedMake(make, &p) != 0 || p == nullptr) {
        printf("  CTOR-FAIL %s\n", cls);
        ++g_crash;
        return;
    }
    ++g_tested;

    p->SetPos(DuiLib::CDuiRect(0, 0, 120, 40));
    p->SetVisible(true);
    p->SetFixedHeight(0);   // 让 EstimateSize 走 cy==0 分支

    const int n = (int)(sizeof(kEvents) / sizeof(kEvents[0]));
    for (int i = 0; i < n; ++i) {
        ++g_events;
        if (GuardedEvent(p, kEvents[i].type) != 0) {
            printf("  CRASH %s::%s\n", cls, kEvents[i].name);
            ++g_crash;
        }
    }

    if (GuardedDelete(p) != 0) {
        printf("  DTOR-CRASH %s\n", cls);
        ++g_crash;
    }
}

int main()
{
    printf("[probe] dispatching every UIEVENT_* with m_pManager == nullptr\n");
    fflush(stdout);

    const int m = (int)(sizeof(kClasses) / sizeof(kClasses[0]));
    for (int i = 0; i < m; ++i)
        RunOne(kClasses[i].name, kClasses[i].make);

    printf("\n[probe] controls: %d  events dispatched: %d  crashes: %d\n",
           g_tested, g_events, g_crash);

    // 双保险防假绿：既要有对象，也要有事件
    if (g_tested < 10 || g_events < 100) {
        printf("[EVENT-NOMGR] INVALID: %d objects / %d events\n", g_tested, g_events);
        return 2;
    }
    printf("[EVENT-NOMGR] %s\n", g_crash == 0 ? "ALL GREEN" : "FAIL");
    return g_crash;
}
