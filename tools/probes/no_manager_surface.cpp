// 无管理器场景崩溃面实测（v3）
//
// 前两版的教训：
//   v1 用 CControlFactory::CreateControl() 全返回 nullptr -> "exercised 0" 假绿
//   v2 在 main 里直接用 __try，与需要析构的局部对象冲突 -> C2712 编译失败，
//      而探针脚本仍跑旧 exe，输出看起来还是绿的
//
// v3 做法：把 __try 完全隔离进只含裸指针的小函数；main 里只做表驱动调用，
// 不出现任何需要析构的局部对象。
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
#include "Control/UIRichEdit.h"
#include "Control/UITreeView.h"
#include "Layout/UITabLayout.h"
#include "Layout/UIVerticalLayout.h"
#include "Layout/UIHorizontalLayout.h"
#include "Control/UIGroupBox.h"
#include "Control/UIRollText.h"
#include "Control/UIGifAnim.h"
#include "Control/UIHotKey.h"
#include "Control/UIDateTime.h"
#include "Control/UIIPAddress.h"
#include "Control/UIRing.h"
#include "Control/UIPageControl.h"

using DuiLib::CControlUI;

typedef CControlUI* (*MakeFn)();
typedef void (*StepFn)(CControlUI*);

static int g_crash = 0;
static int g_tested = 0;

// ---- SEH 隔离区：以下函数内不得出现需要析构的局部对象 ----
static int GuardedMake(MakeFn fn, CControlUI** out)
{
    __try {
        *out = fn();
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        *out = nullptr;
        return 1;
    }
}

static int GuardedCall(StepFn fn, CControlUI* p)
{
    __try {
        fn(p);
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

static int GuardedDelete(CControlUI* p)
{
    __try {
        delete p;
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

// ---- 步骤表 ----
static const struct { const char* name; StepFn fn; } kSteps[] = {
    { "SetText",      [](CControlUI* c) { c->SetText(_T("x")); } },
    { "GetText",      [](CControlUI* c) { (void)c->GetText(); } },
    { "SetName",      [](CControlUI* c) { c->SetName(_T("n")); } },
    { "GetName",      [](CControlUI* c) { (void)c->GetName(); } },
    { "SetPos",       [](CControlUI* c) { c->SetPos(DuiLib::CDuiRect(0, 0, 10, 10)); } },
    { "GetPos",       [](CControlUI* c) { (void)c->GetPos(); } },
    { "SetVisibleF",  [](CControlUI* c) { c->SetVisible(false); } },
    { "SetVisibleT",  [](CControlUI* c) { c->SetVisible(true); } },
    { "IsVisible",    [](CControlUI* c) { (void)c->IsVisible(); } },
    { "SetEnabledF",  [](CControlUI* c) { c->SetEnabled(false); } },
    { "SetEnabledT",  [](CControlUI* c) { c->SetEnabled(true); } },
    { "SetFixedXY",   [](CControlUI* c) { c->SetFixedXY(DuiLib::CDuiSize(20, 20)); } },
    { "Invalidate",   [](CControlUI* c) { c->Invalidate(); } },
    { "NeedUpdate",   [](CControlUI* c) { c->NeedUpdate(); } },
    { "SetFocus",     [](CControlUI* c) { c->SetFocus(); } },
    { "SetTimer",     [](CControlUI* c) { c->SetTimer(1, 100); } },
    { "KillTimer",    [](CControlUI* c) { c->KillTimer(1); } },
    { "GetInterface", [](CControlUI* c) { (void)c->GetInterface(DUI_CTR_CONTROL); } },
    { "SetAttrText",  [](CControlUI* c) { c->SetAttribute(_T("text"), _T("y")); } },
    { "SetAttrBk",    [](CControlUI* c) { c->SetAttribute(_T("bkcolor"), _T("#ff0000")); } },
    { "SetAttrPos",   [](CControlUI* c) { c->SetAttribute(_T("pos"), _T("0,0,10,10")); } },
    { "GetParent",    [](CControlUI* c) { (void)c->GetParent(); } },
    { "GetManager",   [](CControlUI* c) { (void)c->GetManager(); } },
    { "EstimateSize", [](CControlUI* c) { (void)c->EstimateSize(DuiLib::CDuiSize(100, 100)); } },
    // [R245] cy == 0 forces the "measure text with the default font" branch,
    // which dereferences m_pManager in several controls (UILabel/UIEdit/UICombo...).
    // Without this case the probe only exercises the cached-height fast path,
    // which is why earlier runs showed 0 crashes there.
    { "EstSizeZeroCY", [](CControlUI* c) {
          // Must use SetFixedHeight: SetFixedXY writes m_cXY, while the controls
          // test m_cxyFixed.cy. Using the wrong setter silently skipped the
          // branch and produced a false "no crash" result.
          c->SetFixedHeight(0);
          (void)c->EstimateSize(DuiLib::CDuiSize(100, 100)); } },
};

// ---- 构造器表 ----
#define MAKER(CLS) []() -> CControlUI* { return new DuiLib::CLS(); }

static const struct { const char* name; MakeFn make; } kClasses[] = {
    { "Button",           MAKER(CButtonUI) },
    { "Option",           MAKER(COptionUI) },
    { "Label",            MAKER(CLabelUI) },
    { "Edit",             MAKER(CEditUI) },
    { "List",             MAKER(CListUI) },
    { "ListEx",           MAKER(CListExUI) },
    { "Combo",            MAKER(CComboUI) },
    { "ComboBox",         MAKER(CComboBoxUI) },
    { "Slider",           MAKER(CSliderUI) },
    { "Progress",         MAKER(CProgressUI) },
    { "ScrollBar",        MAKER(CScrollBarUI) },
    { "Text",             MAKER(CTextUI) },
    { "RichEdit",         MAKER(CRichEditUI) },
    { "TreeView",         MAKER(CTreeViewUI) },
    { "TabLayout",        MAKER(CTabLayoutUI) },
    { "VerticalLayout",   MAKER(CVerticalLayoutUI) },
    { "HorizontalLayout", MAKER(CHorizontalLayoutUI) },
    { "GroupBox",         MAKER(CGroupBoxUI) },
    { "RollText",         MAKER(CRollTextUI) },
    { "GifAnim",          MAKER(CGifAnimUI) },
    { "HotKey",           MAKER(CHotKeyUI) },
    { "DateTime",         MAKER(CDateTimeUI) },
    { "IPAddress",        MAKER(CIPAddressUI) },
    { "Ring",             MAKER(CRingUI) },
    { "PageControl",      MAKER(CPageControlUI) },
};

// 对单个控件类跑完全部步骤；本函数内只有裸指针与常量表引用
static void RunOne(const char* cls, MakeFn make)
{
    CControlUI* p = nullptr;
    if (GuardedMake(make, &p) != 0) {
        printf("  CTOR-CRASH %s\n", cls);
        ++g_crash;
        return;
    }
    if (p == nullptr) {
        printf("  NULL-OBJ %s\n", cls);
        ++g_crash;
        return;
    }
    ++g_tested;

    const int n = (int)(sizeof(kSteps) / sizeof(kSteps[0]));
    for (int i = 0; i < n; ++i) {
        if (GuardedCall(kSteps[i].fn, p) != 0) {
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
    printf("[probe] controls with m_pManager == nullptr\n");
    fflush(stdout);

    const int m = (int)(sizeof(kClasses) / sizeof(kClasses[0]));
    for (int i = 0; i < m; ++i) {
        RunOne(kClasses[i].name, kClasses[i].make);
    }

    printf("[probe] objects constructed: %d\n", g_tested);
    printf("[probe] crash count: %d\n", g_crash);

    // 防假绿：必须真的构造到足够对象
    if (g_tested < 10) {
        printf("[NOMGR] INVALID: only %d constructed\n", g_tested);
        return 2;
    }
    printf("[NOMGR] %s\n", g_crash == 0 ? "ALL GREEN" : "FAIL");
    return g_crash;
}
