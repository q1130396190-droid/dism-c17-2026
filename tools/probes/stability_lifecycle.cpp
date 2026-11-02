// 稳定性探针 #2：控件生命周期 / 管理器析构顺序
//   case uaf_manager_reaped : 管理器先析构，随后控件析构 -> 悬空 m_pManager
//   case attach_dialog      : AttachDialog 后正常析构
//   case control_no_manager : 无管理器的控件做全套 API 调用
#include "StdAfx.h"
#include <cstdio>
#include <cstring>
#include "Core/UIManager.h"
#include "Core/UIControl.h"
#include "Core/UIContainer.h"
#include "Control/UIButton.h"
#include "Control/UICombo.h"
#include "Control/UIPageControl.h"
#include "Control/UIList.h"
#include "Utils/Utils.h"

static void show(const char* t) { printf("  %s\n", t); fflush(stdout); }

int main(int argc, char** argv)
{
    const char* c = (argc > 1) ? argv[1] : "";

    if (::strcmp(c, "uaf_manager_reaped") == 0) {
        show("new manager + root button, AttachDialog");
        DuiLib::CPaintManagerUI* pm = new DuiLib::CPaintManagerUI();
        DuiLib::CButtonUI* btn = new DuiLib::CButtonUI();
        btn->SetText(_T("hello"));
        bool ok = pm->AttachDialog(btn);
        printf("  AttachDialog=%d\n", ok); fflush(stdout);
        show("delete manager (owns root)");
        delete pm;
        show("now touch the control that the manager just deleted? (skip: it is gone)");
        // 真正的 UAF 形态：控件由外部持有、管理器先走
        DuiLib::CPaintManagerUI* pm2 = new DuiLib::CPaintManagerUI();
        DuiLib::CButtonUI* ext = new DuiLib::CButtonUI();
        ext->SetManager(pm2, nullptr, false);
        show("delete manager while control still holds m_pManager");
        delete pm2;
        show("control destructor now calls m_pManager->ReapObjects (dangling)");
        delete ext;
        show("survived");
        return 0;
    }

    if (::strcmp(c, "control_no_manager") == 0) {
        show("control without manager: full API surface");
        DuiLib::CButtonUI b;
        b.SetText(_T("x"));
        printf("  text=%ls\n", b.GetText().GetData()); fflush(stdout);
        b.SetPos(DuiLib::CDuiRect(0,0,10,10));
        b.SetVisible(true); b.SetEnabled(true);
        b.SetAttribute(_T("text"), _T("y"));
        b.SetAttribute(_T("bkcolor"), _T("#ff0000"));
        b.SetAttribute(_T("bogus_prop"), _T("v"));
        b.SetTimer(1, 100);
        b.KillTimer(1);
        b.SetFocus();
        b.Invalidate();
        b.NeedUpdate();
        b.GetInterface(_T("Button"));
        printf("  size=%d,%d\n", b.GetWidth(), b.GetHeight()); fflush(stdout);
        show("survived");
        return 0;
    }

    if (::strcmp(c, "container_ops") == 0) {
        show("container add/remove/index churn");
        DuiLib::CVerticalLayoutUI root;
        for (int i = 0; i < 200; ++i) {
            auto* b = new DuiLib::CButtonUI();
            TCHAR t[32]; _stprintf_s(t, _countof(t), _T("b%d"), i);
            b->SetText(t);
            root.Add(b);
        }
        printf("  count=%d\n", root.GetCount()); fflush(stdout);
        for (int i = 0; i < 100; ++i) root.Remove(root.GetItemAt(0));
        printf("  after remove count=%d\n", root.GetCount()); fflush(stdout);
        // 越界与空指针
        printf("  GetItemAt(999)=%p GetItemAt(-1)=%p\n",
               (void*)root.GetItemAt(999), (void*)root.GetItemAt(-1)); fflush(stdout);
        root.Remove(nullptr);
        root.SetItemIndex(nullptr, 1);
        printf("  index of first=%d\n", root.GetItemIndex(root.GetItemAt(0))); fflush(stdout);
        show("survived");
        return 0;
    }

    if (::strcmp(c, "pagecontrol_ops") == 0) {
        show("CPageControlUI default ctor + page ops");
        DuiLib::CPageControlUI pc;
        printf("  ctor ok\n"); fflush(stdout);
        pc.SetMaxPages(1);
        pc.SetMaxPages(0);
        pc.SetMaxPages(20, 6);
        pc.SetMaxPages(-5, -5);
        pc.SetFont(0);
        pc.SetAttribute(_T("maxpage"), _T("30,7"));
        pc.GotoPage(5);
        pc.GotoPage(-1);
        pc.GotoPage(9999);
        printf("  page ops done\n"); fflush(stdout);
        show("survived");
        return 0;
    }

    printf("usage: %s <case>\n"
           "  uaf_manager_reaped | control_no_manager | container_ops | pagecontrol_ops\n", argv[0]);
    return 2;
}
