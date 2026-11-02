// R244: 无管理器时「绘制 / 事件」路径崩溃面实测（v2）
//
// v1 的两个问题：
//   * UIEVENT_* 定义在 UIManager.h（经 UIlib.h 引入），不是 UIDefine.h
//   * 把 captureless lambda 塞进 const 数组触发 C2737，改为普通静态函数
//
// 目的：静态扫描出 125 处未守卫 m_pManager 调用，但多数在 Paint*/DoEvent
// 内部。是否真的需要加守卫取决于可达性——本探针直接调用这些路径
// （m_pManager == nullptr），用 SEH 捕获崩溃，把「可疑」变成「事实」。
#include "StdAfx.h"
#include "UIlib.h"
#include <windows.h>
#include <cstdio>

#include "Control/UIButton.h"
#include "Control/UIOption.h"
#include "Control/UILabel.h"
#include "Control/UIEdit.h"
#include "Control/UIList.h"
#include "Control/UICombo.h"
#include "Control/UISlider.h"
#include "Control/UIProgress.h"
#include "Control/UIScrollBar.h"
#include "Control/UIText.h"
#include "Control/UIGifAnim.h"
#include "Control/UIRollText.h"
#include "Control/UIHotKey.h"
#include "Control/UIDateTime.h"
#include "Control/UIIPAddress.h"
#include "Control/UIRichEdit.h"
#include "Control/UITreeView.h"
#include "Layout/UITabLayout.h"
#include "Layout/UIVerticalLayout.h"

using DuiLib::CControlUI;

typedef CControlUI* (*MakeFn)();
typedef void (*StepFn)(CControlUI*, HDC);

static int g_crash = 0;
static int g_tested = 0;

// ---- SEH 隔离区（函数内无析构对象）----
static int GuardedMake(MakeFn fn, CControlUI** out)
{
    __try { *out = fn(); return 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { *out = nullptr; return 1; }
}

static int GuardedCall(StepFn fn, CControlUI* p, HDC hdc)
{
    __try { fn(p, hdc); return 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 1; }
}

static int GuardedDelete(CControlUI* p)
{
    __try { delete p; return 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 1; }
}

// ---- 步骤实现（普通静态函数，避免 lambda 的常量初始化问题）----
static void StepPaint(CControlUI* c, HDC dc)
{
    DuiLib::CDuiRect rc(0, 0, 100, 40);
    (void)c->Paint(dc, rc, nullptr);
}
static void StepDoPaint(CControlUI* c, HDC dc)
{
    DuiLib::CDuiRect rc(0, 0, 100, 40);
    (void)c->DoPaint(dc, rc, nullptr);
}
static void StepDoPostPaint(CControlUI* c, HDC dc)
{
    DuiLib::CDuiRect rc(0, 0, 100, 40);
    c->DoPostPaint(dc, rc);
}
static void StepDoEventHover(CControlUI* c, HDC)
{
    DuiLib::TEventUI ev = { 0 };
    ev.Type = DuiLib::UIEVENT_MOUSEENTER;
    c->DoEvent(ev);
}
static void StepDoEventDown(CControlUI* c, HDC)
{
    DuiLib::TEventUI ev = { 0 };
    ev.Type = DuiLib::UIEVENT_BUTTONDOWN;
    ev.ptMouse.x = 5;
    ev.ptMouse.y = 5;
    c->DoEvent(ev);
}
static void StepSetFocus(CControlUI* c, HDC) { c->SetFocus(); }
static void StepActivate(CControlUI* c, HDC) { (void)c->Activate(); }
static void StepGetText(CControlUI* c, HDC)  { (void)c->GetText(); }

struct StepDef { const char* name; StepFn fn; };

static const StepDef kSteps[] = {
    { "Paint",        StepPaint },
    { "DoPaint",      StepDoPaint },
    { "DoPostPaint",  StepDoPostPaint },
    { "DoEventHover", StepDoEventHover },
    { "DoEventDown",  StepDoEventDown },
    { "SetFocus",     StepSetFocus },
    { "Activate",     StepActivate },
    { "GetText",      StepGetText },
};

#define MAKER(CLS) []() -> CControlUI* { return new DuiLib::CLS(); }

struct ClassDef { const char* name; MakeFn make; };

static const ClassDef kClasses[] = {
    { "Button",         MAKER(CButtonUI) },
    { "Option",         MAKER(COptionUI) },
    { "Label",          MAKER(CLabelUI) },
    { "Edit",           MAKER(CEditUI) },
    { "List",           MAKER(CListUI) },
    { "Combo",          MAKER(CComboUI) },
    { "Slider",         MAKER(CSliderUI) },
    { "Progress",       MAKER(CProgressUI) },
    { "ScrollBar",      MAKER(CScrollBarUI) },
    { "Text",           MAKER(CTextUI) },
    { "GifAnim",        MAKER(CGifAnimUI) },
    { "RollText",       MAKER(CRollTextUI) },
    { "HotKey",         MAKER(CHotKeyUI) },
    { "DateTime",       MAKER(CDateTimeUI) },
    { "IPAddress",      MAKER(CIPAddressUI) },
    { "RichEdit",       MAKER(CRichEditUI) },
    { "TreeView",       MAKER(CTreeViewUI) },
    { "TabLayout",      MAKER(CTabLayoutUI) },
    { "VerticalLayout", MAKER(CVerticalLayoutUI) },
};

static void RunOne(const char* cls, MakeFn make, HDC hdc)
{
    CControlUI* p = nullptr;
    if (GuardedMake(make, &p) != 0 || p == nullptr) {
        printf("  CTOR-FAIL %s\n", cls);
        ++g_crash;
        return;
    }
    ++g_tested;

    p->SetPos(DuiLib::CDuiRect(0, 0, 100, 40));
    p->SetVisible(true);

    const int n = (int)(sizeof(kSteps) / sizeof(kSteps[0]));
    for (int i = 0; i < n; ++i) {
        if (GuardedCall(kSteps[i].fn, p, hdc) != 0) {
            printf("  CRASH %s::%s\n", cls, kSteps[i].name);
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
    HDC hScreen = ::GetDC(nullptr);
    HDC hDC = ::CreateCompatibleDC(hScreen);
    BYTE* pBits = nullptr;
    HBITMAP hBmp = DuiLib::CRenderEngine::CreateARGB32Bitmap(hDC, 128, 64, &pBits);
    HBITMAP hOld = (HBITMAP)::SelectObject(hDC, hBmp);
    ::SetBkMode(hDC, TRANSPARENT);

    printf("[probe] paint/event paths with m_pManager == nullptr\n");
    fflush(stdout);

    const int m = (int)(sizeof(kClasses) / sizeof(kClasses[0]));
    for (int i = 0; i < m; ++i)
        RunOne(kClasses[i].name, kClasses[i].make, hDC);

    ::SelectObject(hDC, hOld);
    ::DeleteObject(hBmp);
    ::DeleteDC(hDC);
    ::ReleaseDC(nullptr, hScreen);

    printf("[probe] objects constructed: %d\n", g_tested);
    printf("[probe] crash count: %d\n", g_crash);

    if (g_tested < 10) {
        printf("[PAINT-NOMGR] INVALID: only %d constructed\n", g_tested);
        return 2;
    }
    printf("[PAINT-NOMGR] %s\n", g_crash == 0 ? "ALL GREEN" : "FAIL");
    return g_crash;
}
