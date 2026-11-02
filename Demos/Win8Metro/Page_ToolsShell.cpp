// ============================================================
// Page_ToolsShell.cpp —— 工具箱页「文件浏览器 + 激活备份」（从 Page_Tools.cpp 拆出）。
//
// [R181] Page_Tools.cpp 拆前 1,171 行、规模分 9.26（六页最低总分）。本文件承接
// 四个成员 + 两个小工具入口，合计 121 行，边界按「只是壳外调用、不做本页数据加工」
// 划分：
//   · ToolExplorer         文件浏览器（选目录 → explorer.exe 打开，失败回退 LaunchGuiEx）
//   · ToolActivationBackup 激活备份/还原编排（选方向 → 选目录 → 二次确认 → 执行）
//   · ActPrepareTarget     选目录 + 还原前置校验（无 Backup.ini 直接中止）
//   · ActRunAndReport      执行 Backup/Restore 并给出结果反馈
//
// 闭包说明：四个成员声明见 Page_Tools.h。跨 TU 依赖六项且都是头文件级：
//   · m_ctx（HostHwnd / HostLog / HostToast）
//   · StringMgr（toolbox.act_* / toolbox.s_* / misc.sbfc57f42，键名逐字保持）
//   · HostNotify / HostConfirm（NotifyBox.h，本页统一弹框入口）
//   · Win32DialogService::DlgBrowseFolder（Win32DialogService.h）
//   · ExternalToolService::ShellExecFailed / LaunchGuiEx（ExternalToolService.h）
//   · tool_dirs::ShowCommonDirMenu（Page_ToolsCommon.h）
//
// 逐字搬运，零逻辑改动。
// ============================================================

#include "stdafx.h"

#include "Page_Tools.h"
#include "Page_ToolsCommon.h"     // tool_dirs::ShowCommonDirMenu / ToolSql

#include "StringManager.h"
#include "NotifyBox.h"            // HostNotify / HostConfirm
#include "Win32DialogService.h"   // DlgBrowseFolder
#include "ExternalToolService.h"  // ShellExecFailed / LaunchGuiEx
#include "ActivationKitClient.h"  // 激活备份组件（IsReady / Backup / Restore）

using namespace DuiLib;
using i18n::CStringManager;

// 主文件经 using 声明引入；拆出后必须显式引入（R179/R180 同类）。
using ToolSql::HostNotify;
using ToolSql::HostConfirm;

// [R181] 激活备份/还原的结果提示框按钮组合。原在两处（成功、组件缺失）
// 各写一遍 MB_OK | MB_ICONINFORMATION，另有 MB_OK | MB_ICONERROR 与
// MB_ICONWARNING 分散在三处；具名后"同一流程用同一套按钮"成为可核对的事实，
// 不再依赖三处字面量恰好一致。
inline constexpr UINT kToolsActNotifyInfo = MB_OK | MB_ICONINFORMATION;
inline constexpr UINT kToolsActNotifyErr  = MB_OK | MB_ICONERROR;
inline constexpr UINT kToolsActNotifyWarn = MB_OK | MB_ICONWARNING;

// 8 — pick a folder and open it in Explorer

// ====================================================================


void CPageTools::ToolExplorer()

{
    CDuiString sDir;
    if (!Win32DialogService::DlgBrowseFolder(sDir, StringMgr.Get("misc.sbfc57f42")))
    {
        // P2-1: 用户取消选择，改为弹出常用目录菜单
        tool_dirs::ShowCommonDirMenu(m_ctx.HostHwnd());
        return;
    }
    // [R01] FrogPlugins 已剥离：文件浏览器统一走系统 explorer.exe
    // （失败再由 LaunchGuiEx CreateProcess 回退）。
    CDuiString sArgs;
    sArgs.Format(L"\"%s\"", sDir.GetData());
    HINSTANCE hInst = ::ShellExecute(m_ctx.HostHwnd(), L"open", L"explorer.exe",
                                     sArgs.GetData(), nullptr, SW_SHOWNORMAL);
    if (ExternalToolService::ShellExecFailed(hInst))
    {
        // [R129-1] D3/D4：改用 LaunchGuiEx 走 CreateProcessW 真实路径并回传错误码
        DWORD dwErr = 0;
        if (!ExternalToolService::LaunchGuiEx(L"explorer.exe", nullptr, false, 0, &dwErr))
        {
            CDuiString sFail;
            sFail.Format(StringMgr.Get("toolbox.s_done_cp"), dwErr);
            m_ctx.HostLog(sFail.GetData(), static_cast<int>(ELogLevel::LOG_ERROR));
            m_ctx.HostToast(StringMgr.Get("toolbox.s_done_title"), sFail.GetData(), 1);
        }
    }
}

// ============================================================================
// [R01] 11 — 激活备份 / 还原（独立组件 ActivationKit.dll）
//
// 能力来自 R01 重构对青蛙插件 slot[77]/[81] 的原生剥离：
//   备份 = DigitalProductId(4) 等写「激活备份\Backup.ini」；
//   还原 = 反向写回注册表。
// 交互：先选备份/还原 → 选目录 → 风险二次确认 → 执行 → 结果反馈。
// 组件缺失（DLL 未随包）时卡片已置灰；此处兜底再判一次。
// ============================================================================

// [R157] 激活备份/还原的「选目录 + 前置校验」段（从 ToolActivationBackup 抽出）。
// 返回 false 表示用户取消或前置条件不满足（调用方直接返回）。
// 还原路径要先确认所选目录确实有 Backup.ini —— 插件版曾在此误报"还原成功"。
bool CPageTools::ActPrepareTarget(bool bBackup, std::wstring& sBase)
{
    CDuiString sDir;
    if (!Win32DialogService::DlgBrowseFolder(
            sDir, StringMgr.Get(bBackup ? "toolbox.act_pick_b" : "toolbox.act_pick_r")))
        return false;   // 用户取消
    sBase.assign(sDir.GetData());
    if (sBase.empty()) return false;
    // 还原前预检：备份是否存在（不存在直接提示，不弹成功）
    if (bBackup || ActivationKitClient::BackupExists(sBase)) return true;
    HostNotify(m_ctx.HostHwnd(), StringMgr.Get("toolbox.act_no_backup"),
               StringMgr.Get("toolbox.act_title"), kToolsActNotifyWarn);
    m_ctx.HostLog(L"[tools] 激活还原：所选目录无 Backup.ini，已中止");
    return false;
}

// [R157] 执行备份或还原并给出结果反馈（从 ToolActivationBackup 抽出）。
// 备份走 Backup(base, 空, &err)，还原走 Restore(base, &err)。
void CPageTools::ActRunAndReport(bool bBackup, const std::wstring& sBase)
{
    std::wstring sErr;
    const long hr = bBackup ? ActivationKitClient::Backup(sBase, std::wstring(), &sErr)
                            : ActivationKitClient::Restore(sBase, &sErr);
    m_ctx.HostLog(bBackup
        ? (SUCCEEDED(hr) ? L"[tools] 激活备份：成功" : L"[tools] 激活备份：失败")
        : (SUCCEEDED(hr) ? L"[tools] 激活还原：成功" : L"[tools] 激活还原：失败"));
    if (SUCCEEDED(hr))
    {
        HostNotify(m_ctx.HostHwnd(),
                   StringMgr.Get(bBackup ? "toolbox.act_backup_ok" : "toolbox.act_restore_ok"),
                   StringMgr.Get("toolbox.act_title"), kToolsActNotifyInfo);
        return;
    }
    std::wstring sShow = sErr;
    if (sShow.empty())
        sShow = bBackup ? StringMgr.Get("toolbox.ui_act_backup_fail")
                        : StringMgr.Get("toolbox.ui_act_restore_fail");
    HostNotify(m_ctx.HostHwnd(), sShow.c_str(),
               StringMgr.Get("toolbox.act_title"), kToolsActNotifyErr);
}

void CPageTools::ToolActivationBackup()
{
    if (!ActivationKitClient::IsReady())
    {
        HostNotify(m_ctx.HostHwnd(), StringMgr.Get("toolbox.act_missing"),
                   StringMgr.Get("toolbox.act_title"), kToolsActNotifyInfo);
        return;
    }
    // 1) 选择操作：是=备份，否=还原，取消=退出
    const int nSel = HostConfirm(m_ctx.HostHwnd(),
                                 StringMgr.Get("toolbox.act_choose_body"),
                                 StringMgr.Get("toolbox.act_title"),
                                 MB_YESNOCANCEL, MB_ICONQUESTION);
    if (nSel != IDYES && nSel != IDNO) return;
    const bool bBackup = (nSel == IDYES);
    // 2)+3) 选目录 + 还原前置校验
    std::wstring sBase;
    if (!ActPrepareTarget(bBackup, sBase)) return;
    // 4) 风险二次确认（默认按钮停在"否"，防误触）
    const int nGo = HostConfirm(m_ctx.HostHwnd(),
                                StringMgr.Get(bBackup ? "toolbox.act_backup_warn"
                                                      : "toolbox.act_restore_warn"),
                                StringMgr.Get("toolbox.act_title"), MB_YESNO, MB_ICONWARNING);
    if (nGo != IDYES)
    {
        m_ctx.HostLog(bBackup ? L"[tools] 激活备份：用户取消"
                              : L"[tools] 激活还原：用户取消");
        return;
    }
    // 5)+6) 执行并反馈
    ActRunAndReport(bBackup, sBase);
}
