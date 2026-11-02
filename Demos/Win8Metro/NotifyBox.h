#pragma once

// ============================================================
// NotifyBox.h —— 页面统一的提示 / 确认框入口（跨页共享，单一定义）。
//
// 为什么抽成独立中立头：
//   HostNotify / HostConfirm 原先定义在 Page_ToolsCommon.h 的 ToolSql
//   命名空间里。该头同时带着 PTFileGuard / PTRegGuard 等工具箱页专用
//   RAII 守卫，并额外引入 <optional> / <type_traits>。其它页面（Optim /
//   Appx）只是想弹个框，却要为此反向依赖工具箱页的内部头，层次颠倒。
//
//   本头只含这两个无状态 inline 函数，任何页面都可直接包含；命名空间
//   沿用 ToolSql，使既有调用点（Tools 页 / AppxDel 页）零改动。
//
// 语义要点：
//   · HostConfirm 强制附加 MB_DEFBUTTON2 —— 默认按钮固定为「否 / 取消」，
//     避免高危操作（禁用更新、删除规则、批量改写注册表）被回车误确认。
//     原先各页手写的确认框本就显式带 MB_DEFBUTTON2，行为完全等价。
//   · 两函数都返回 MessageBoxW 的按钮 ID（IDYES / IDNO / IDOK），
//     调用点原有的 == IDYES / != IDYES 比较可直接保留。
// ============================================================

#include <windows.h>
#include "CommandLineService.h"   // [R-AUTOTEST] /Unattend 无人值守自动应答

namespace ToolSql
{

// [R-AUTOTEST] 告知框在无人值守下直接返回（不弹窗、不阻塞工作流）
inline bool UnattendedSilent()
{
    return CommandLineService::IsUnattended();
}

// 纯告知型提示框（无分支决策）：一律走此入口。
//  hwndOwner 可为 nullptr（无窗口上下文时按系统默认）。
inline int HostNotify(HWND hwndOwner, const wchar_t* pszBody, const wchar_t* pszTitle,
                      UINT uStyle = MB_OK | MB_ICONINFORMATION)
{
    // [R-AUTOTEST] 告知框在无人值守下直接返回（不弹窗、不阻塞工作流）
    if (CommandLineService::IsUnattended()) return IDOK;
    return ::MessageBoxW(hwndOwner, pszBody ? pszBody : L"",
                         pszTitle ? pszTitle : L"", uStyle);
}

// 需要「是/否」或「确定/取消」的交互：统一走此入口。
//  默认按钮固定为「否/取消」（MB_DEFBUTTON2），避免误触高危操作。
[[nodiscard]] inline int HostConfirm(HWND hwndOwner, const wchar_t* pszBody, const wchar_t* pszTitle,
                                     UINT uButtons = MB_YESNO,
                                     UINT uIcon = MB_ICONWARNING)
{
    // [R-AUTOTEST] 无人值守：仅"继续"类按钮自动应答；MB_OKCANCEL/MB_CANCELTRYCONTINUE
    // 视为用户取消（-1 交回真实弹窗之外的保守值），避免测试开关变成"全部同意"
    // ——ResetBase/重启资源管理器等不可逆确认绝不允许被批量放行。
    int nAuto = -1;
    if (CommandLineService::IsUnattended())
    {
        switch (uButtons & 0x0F)
        {
        case MB_OK:            nAuto = IDOK;   break;
        case MB_YESNO:         nAuto = IDYES;  break;
        case MB_ABORTRETRYIGNORE:
        case MB_RETRYCANCEL:   nAuto = IDRETRY; break;
        case MB_OKCANCEL:
        case MB_YESNOCANCEL:   nAuto = IDCANCEL; break;
        case MB_CANCELTRYCONTINUE: nAuto = IDCANCEL; break;
        default:               nAuto = IDOK;   break;
        }
    }
    if (nAuto != -1) return nAuto;
    return ::MessageBoxW(hwndOwner, pszBody ? pszBody : L"",
                         pszTitle ? pszTitle : L"",
                         uButtons | uIcon | MB_DEFBUTTON2);
}

}   // namespace ToolSql
