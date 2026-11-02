// 验证 SEC-10 修复：走真实的 CControlUI::SetAttribute 失败路径（bad pos），
// 检查日志中 class/name 是否被正确记录（修复前会错位/乱码）。
#include "StdAfx.h"
#include <cstdio>
#include "Core/UIControl.h"
#include "Utils/UICrashGuard.h"

int main()
{
    DuiLib::UICrashGuard::Install();
    printf("[probe] LogPath=%ls\n", DuiLib::UICrashGuard::LogPath());
    fflush(stdout);

    DuiLib::CControlUI ctl;
    ctl.SetName(_T("probe-name-1234"));
    printf("[probe] class=%ls name=%ls\n", ctl.GetClass(), ctl.GetName().GetData());
    fflush(stdout);

    // 触发 UIControl.cpp 中 bad pos 的日志分支
    ctl.SetAttribute(_T("pos"), _T("not-a-rect"));
    printf("[probe] SetAttribute(bad pos) returned\n");
    return 0;
}
