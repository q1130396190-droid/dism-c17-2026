// ============================================================
// Page_AssocActions.cpp —— 文件关联页「6 按钮隐藏执行 dism」动作族。
//
// [R100] 从 Page_Assoc.cpp 拆出。原有 192 行 / CC=29 的巨型函数在早前
//   轮次已按职责拆成三段，本轮把这三段整体搬到独立 TU：
//     AswResolveAction  按钮 → (bImage, nAction, sXml, sMount)
//     AswRunAction      执行原生关联维护，返回退出码
//     AswReportResult   拼结果文案 + 失败复制剪贴板 + 告警 + 成功后刷新
//
// 拆分前后函数体逐字未改，仅搬迁位置。
// ============================================================

#include "stdafx.h"

#include "Page_Assoc.h"

#include "StringManager.h"       // StringMgr 本地化宏
#include "FrameWnd_Util.h"       // FrameWnd_Util_CopyTextToClipboard
#include "FrameWnd_ErrorMap.h"   // ErrorMap::LogError/GetFriendlyMessage
#include "Win32DialogService.h"  // DlgOpenFile/DlgSaveFile/DlgBrowseFolder
#include "AssocService.h"        // RunDismAssocArgs

#include <algorithm>
#include <memory>

using namespace DuiLib;
using i18n::CStringManager;

// ============================================================
// 文件关联 6 按钮：隐藏执行 dism 并等待退出码
// ============================================================
void CPageAssoc::AssocButton(const wchar_t* pszBtn)
{
    // [AUDIT-P2] 原 192 行 / CC=29，按职责拆为 3 段：
    //   AswResolveAction  按钮 → (bImage, nAction, sXml, sMount)，false=用户取消/未知按钮
    //   AswRunAction      执行原生关联维护，返回退出码
    //   AswReportResult   拼结果文案 + 失败复制剪贴板 + 告警 + 成功后刷新
    // 弹窗文案、错误码映射、日志文本逐字保留。

    bool bImage = false;      // true=离线映像 / false=联机系统
    int  nAction = 0;         // 0 导入 / 1 导出 / 2 删除(重置)
    CDuiString sXml;          // 关联 XML 路径
    CDuiString sMount;        // 离线映像挂载目录

    if (!AswResolveAction(pszBtn, bImage, nAction, sXml, sMount)) return;

    CDuiString sA9Err;
    const DWORD dwExit = AswRunAction(nAction, bImage, sMount, sXml, sA9Err);

    AswReportResult(dwExit == 0, nAction, dwExit, sA9Err);
}

// [AUDIT-P2] 从 AssocButton 提取：按钮 → 动作/作用域/路径 解析
// 返回 false 表示用户取消（对话框取消/确认框选否）或按钮未知，调用方直接返回。
bool CPageAssoc::AswResolveAction(const wchar_t* pszBtn, bool& bImage, int& nAction,
                                  CDuiString& sXml, CDuiString& sMount)
{
    CDuiString sBtn(pszBtn);

    // 关联 XML 的文件对话框过滤器（联机/离线共用）
    const wchar_t* pszFilter = StringMgr.Get("misc.s405d4072");
if (sBtn == L"btn_assoc_live_export")
{
    bImage = false; nAction = 1;
    if (!Win32DialogService::DlgSaveFile(sXml, pszFilter, StringMgr.Get("appx.se5e4532f"),
                     L"DefaultAppAssociations.xml")) return false;
}
else if (sBtn == L"btn_assoc_live_import")
{
    bImage = false; nAction = 0;
    if (!Win32DialogService::DlgOpenFile(sXml, pszFilter, StringMgr.Get("misc.s81c11e73"))) return false;
}
else if (sBtn == L"btn_assoc_live_reset")
{
    bImage = false; nAction = 2;
    if (!m_ctx.HostConfirmWarn(StringMgr.Get("misc.s80cfc33c"),
                                   StringMgr.Get("appx.s7b028431")))
    return false;
}
else if (sBtn == L"btn_assoc_img_import"
      || sBtn == L"btn_assoc_img_export"
      || sBtn == L"btn_assoc_img_delete")
{
    bImage = true;
    nAction = sBtn == L"btn_assoc_img_import" ? 0
            : sBtn == L"btn_assoc_img_export" ? 1 : 2;


    // 先选挂载目录

    if (!Win32DialogService::DlgBrowseFolder(sMount, StringMgr.Get("toolbox.s115afc1c"))) return false;



    if (nAction == 0)
    {
        if (!Win32DialogService::DlgOpenFile(sXml, pszFilter, StringMgr.Get("misc.s81c11e73"))) return false;
    }
    else if (nAction == 1)
    {
        if (!Win32DialogService::DlgSaveFile(sXml, pszFilter, StringMgr.Get("toolbox.s30460dc7"),
                         L"DefaultAppAssociations.xml")) return false;
    }
    else
    {
        if (!m_ctx.HostConfirmWarn(StringMgr.Get("edit.s631cd220"),
                                       StringMgr.Get("toolbox.scd6af3fd")))
    return false;
    }
}
    // [FIX-S1-1] 致命笔误：末尾误写的 return false; 使真正的 return true;
    // 不可达 —— 联机/离线 导入/导出/重置 6 个按钮走完文件框/确认后静默
    // 无执行无反馈（功能假死）。删除后正常返回 true。
    return true;
}

// [AUDIT-P2] 从 AssocButton 提取：执行原生默认应用关联维护
// 返回 0 表示成功，其余为原生错误码。
DWORD CPageAssoc::AswRunAction(int nAction, bool bImage, const CDuiString& sMount,
                               const CDuiString& sXml, CDuiString& sA9Err)
{
CDuiString sOffline = bImage ? sMount : CDuiString();
DWORD dwExit = 0;
if (nAction == 0)
{
    CDuiString sLog;
    sLog.Format(L"[assoc] native Import-DefaultAppAssociations: %s (root=%s)",
                sXml.GetData(), bImage ? sMount.GetData() : L"online");
    m_ctx.HostLog(sLog.GetData());
    dwExit = AssocService::ImportDefaultAssociations(sXml, sOffline, sA9Err);
}
else if (nAction == 1)
{
    CDuiString sLog;
    sLog.Format(L"[assoc] native Export-DefaultAppAssociations -> %s (root=%s)",
                sXml.GetData(), bImage ? sMount.GetData() : L"online");
    m_ctx.HostLog(sLog.GetData());
    dwExit = AssocService::ExportDefaultAssociations(sXml, sOffline, sA9Err);
}
else
{
    m_ctx.HostLog(L"[assoc] native Remove-DefaultAppAssociations");
    dwExit = AssocService::RemoveDefaultAssociations(sOffline, sA9Err);
}
    return dwExit;
}

// [AUDIT-P2] 从 AssocButton 提取：结果文案 + 告警 + 成功后刷新
void CPageAssoc::AswReportResult(bool bOk, int nAction, DWORD dwExit,
                                 const CDuiString& sA9Err)
{
    CDuiString sMsg;
if (bOk)
{
    sMsg = StringMgr.Get("misc.s685415b1");
    if (nAction == 0)
        sMsg += StringMgr.Get("misc.se06521d4");
}
else
{
    // 原生错误码 + 可读原因；失败详情复制到剪贴板
    CDuiString sFriendly;
    sFriendly.Format(StringMgr.Get("misc.hbc04"),
                     sA9Err.IsEmpty() ? ErrorMap::GetFriendlyMessage(dwExit)
                                      : sA9Err,
                     dwExit);
    sMsg = sFriendly;
    ErrorMap::LogError(dwExit, L"native-assoc", L"association-fix");

}
// UI 全局错误提示统一：失败时复制详情到剪贴板

if (!bOk) FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), sMsg.GetData());



m_ctx.HostAlert(StringMgr.Get("appx.s5e8159ab"), sMsg.GetData(),
               bOk ? MB_ICONINFORMATION : MB_ICONERROR);



// 成功后刷新扩展名列表

if (bOk) Build();
}
