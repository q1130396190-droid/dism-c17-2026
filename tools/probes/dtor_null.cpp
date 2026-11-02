// 析构路径崩溃验证：控件在「无管理器」状态下被销毁
//
// 崩溃路径（修复前）：
//   CGifAnimUI::~CGifAnimUI   -> m_pManager->KillTimer  -> nullptr 解引用
//   CRollTextUI::~CRollTextUI -> m_pManager->KillTimer  -> nullptr 解引用
// 真实场景：工厂创建后丢弃、皮肤解析失败后的清理、控件先于管理器销毁。
#include "StdAfx.h"
#include "UIlib.h"
#include <cstdio>
#include "Control/UIGifAnim.h"
#include "Control/UIRollText.h"
#include "Control/UIRichEdit.h"

int main()
{
    using namespace DuiLib;

    printf("[probe] destroying CGifAnimUI with no manager ...\n");
    fflush(stdout);
    { CGifAnimUI* p = new CGifAnimUI(); delete p; }
    printf("[probe] CGifAnimUI destroyed ok\n");
    fflush(stdout);

    printf("[probe] destroying CRollTextUI with no manager ...\n");
    fflush(stdout);
    { CRollTextUI* p = new CRollTextUI(); delete p; }
    printf("[probe] CRollTextUI destroyed ok\n");
    fflush(stdout);

    printf("[probe] destroying CRichEditUI with no manager ...\n");
    fflush(stdout);
    { CRichEditUI* p = new CRichEditUI(); delete p; }
    printf("[probe] CRichEditUI destroyed ok\n");
    fflush(stdout);

    // 再验证一次：挂接后又脱离，再销毁（更接近真实销毁顺序）
    printf("[probe] detach-then-destroy path ...\n");
    fflush(stdout);
    {
        CPaintManagerUI pm;
        CGifAnimUI* p = new CGifAnimUI();
        p->SetManager(&pm, nullptr, false);
        p->SetManager(nullptr, nullptr, false);   // 模拟管理器先走
        delete p;
    }
    printf("[probe] detach-then-destroy ok\n");

    printf("[probe] OK: no nullptr dereference in destructors\n");
    return 0;
}
