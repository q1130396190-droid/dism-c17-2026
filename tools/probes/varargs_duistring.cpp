// 探针：验证 CDuiString 作为可变参数传给 %s 的两种情形
//   (a) 堆分配字符串：对象首字段恰好是内部指针 -> 偶然"能跑"，但仍是 UB
//   (b) 短字符串（SSO/内联缓冲）：对象首 8 字节是字符数据而非指针
//       -> %s 把字符数据当指针解引用 -> 非法读取
#include "StdAfx.h"
#include <cstdio>
#include <string>
#include "Core/UIControl.h"
#include "Utils/UICrashGuard.h"

int main()
{
    DuiLib::UICrashGuard::Install();

    DuiLib::CControlUI ctl;

    // (a) 长名字 -> 走堆分配
    ctl.SetName(_T("a-very-long-control-name-exceeding-any-inline-buffer-1234567890"));
    printf("[probe] (a) long name: calling DUI_LOGW with CDuiString object ...\n");
    fflush(stdout);
    DUI_LOGW("long  '%s'", ctl.GetName());
    printf("[probe] (a) survived (object's first field happened to be the data pointer)\n");
    fflush(stdout);

    // (b) 极短名字 -> 很可能走内联缓冲；此时首字节是字符不是指针
    ctl.SetName(_T("ab"));
    printf("[probe] (b) short name: calling DUI_LOGW with CDuiString object ...\n");
    fflush(stdout);
    DUI_LOGW("short '%s'", ctl.GetName());
    printf("[probe] (b) survived\n");
    return 0;
}
