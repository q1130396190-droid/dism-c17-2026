// ============================================================
// Page_AppxDeploy.cpp —— 应用页「部署动作」（从 Page_Appx.cpp 拆出）。
//
// [R171] Page_Appx.cpp 拆前 1,376 行、规模分 8.70（六页最低）。本文件承接
// 四个部署相关函数（178 行），边界按「都会改动系统已安装包集合」划分：
//   · AppxInstall              安装 .appx/.msix（可选依赖目录）
//   · AxCollectCheckedFullNames 收集勾选项的 PackageFullName（备份/卸载共用）
//   · Backup                   备份勾选项到目录（WinRT 打包）
//   · Restore                  从备份恢复（可选依赖目录）
// 与 Page_AppxDel.cpp（卸载）同属部署族但动作方向相反，故分文件而非合并。
//
// 闭包说明：四个函数均为 CPageAppx 成员（声明见 Page_Appx.h），依赖面：
//   · m_ctx（HostOpenFile / HostBrowseFolder / HostConfirmYesNo / HostNotify /
//     HostToast / HostLog / HostFindControl）—— 头文件级上下文
//   · StringMgr（appx.s_* / misc.* 语言键，键名逐字保持，未新增）
//   · ExternalToolService::IsElevated（提权前置判定）
//   · AppxDeployService / EAppxDeployResult（部署动作与结果码）
//   · ShowDeployFailToast（同页成员，仍在 Page_Appx.cpp）
// 原文件不残留 file-scope 静态实体，跨 TU 无静态依赖。
//
// 逐字搬运，零逻辑改动。
// ============================================================

#include "stdafx.h"

#include "Page_Appx.h"

#include "StringManager.h"
#include "ExternalToolService.h" // IsElevated
#include "AppxDeployService.h"   // AppxRestore / EAppxDeployResult
#include "CustomControls.h"      // 列表控件（收集勾选）

using namespace DuiLib;
using i18n::CStringManager;

// [R171] HostNotify 的"警告级"取值。三处提权提示原各自硬编码 2，
// 与 HostToast 的 0/1/2 语义不同（HostNotify 是托盘气球），故单独具名。
inline constexpr int kAppxNotifyWarn = 2;

// [R171] 部署动作的三个前置条件在 AppxInstall / Backup / Restore 中各写了一遍
// （共 3 处 6 行完全相同的「未提权 → 托盘气球 → 返回」）。合并为单一函数。
// [[nodiscard]] 是刻意的：调用方若写成 `AxRequireElevated();` 而不判返回值，
// 就会在未提权时继续执行后续部署，且没有任何编译提示。
[[nodiscard]] static bool AxRequireElevated(IPageContext& ctx)
{
    if (ExternalToolService::IsElevated()) return true;
    ctx.HostNotify(StringMgr.Get("misc.s22dfbca7"), StringMgr.Get("misc.s22dfbca7"),
                   kAppxNotifyWarn);
    return false;
}


// ============================================================
// App Installer：原生 WinRT PackageManager.AddPackageAsync 安装通道
// [P0-A1] 旧 powershell.exe Add-AppxPackage 子进程已删除；
// 统一走 AppxDeployService::AppxRestore（支持依赖目录）。
// ============================================================
void CPageAppx::AppxInstall()
{
    CDuiString sFile;
    if (!m_ctx.HostOpenFile(sFile, StringMgr.Get("misc.s1207edb3"),
                            StringMgr.Get("misc.sc8059969")))
        return;
    if (!AxRequireElevated(m_ctx)) return;
    // 让用户选择依赖目录（可选）
    CDuiString sDepDir;
    if (m_ctx.HostConfirmYesNo(StringMgr.Get("misc.s8f3077b7"),
                               StringMgr.Get("misc.s587c8055")))
    {
        if (m_ctx.HostBrowseFolder(sDepDir, StringMgr.Get("misc.s5c45dab4")))
        {
            CDuiString sErr;
            EAppxDeployResult nDep = static_cast<EAppxDeployResult>(AppxDeployService::AppxRestore(sFile, sDepDir, sErr));
            if (nDep == EAppxDeployResult::APPX_DEPLOY_OK)
            {
                m_ctx.HostLog(L"[appx-install] AddPackageAsync OK (with dependencies)");
                m_ctx.HostToast(StringMgr.Get("appx.s_restore_title"),
                                StringMgr.Get("appx.s_restore_done2"), 0);
            }
            else
                ShowDeployFailToast(L"restore", nDep, sErr);
            return;
        }
    }
    // 无依赖目录：直接安装
    CDuiString sErr;
    EAppxDeployResult nMain = static_cast<EAppxDeployResult>(AppxDeployService::AppxRestore(sFile, L"", sErr));
    if (nMain == EAppxDeployResult::APPX_DEPLOY_OK)
    {
        CDuiString sLog;
        sLog.Format(L"[appx-install] AddPackageAsync OK: %s", sFile.GetData());
        m_ctx.HostLog(sLog.GetData());
        m_ctx.HostToast(StringMgr.Get("appx.s_restore_title"),
                        StringMgr.Get("appx.s_restore_done2"), 0);
    }
    else
    {
        CDuiString sLog;
        sLog.Format(L"[appx-install] failed code=%d err=%s", nMain,
                    sErr.IsEmpty() ? L"unknown" : sErr.GetData());
        m_ctx.HostLog(sLog.GetData());
        ShowDeployFailToast(L"restore", nMain, sErr);
    }
}
// ============================================================
// [R81] Appx 备份 / 恢复（评分报告 P2 缺口 #21/#22）
// 备份：Export-AppxPackage -Path <目标目录> 逐个导出选中包（.appx）
// 恢复：Add-AppxPackage -Path <选中 .appx> 安装回来
// 全部走 ExternalToolService::LaunchSystem（外部进程封装），页不直调系统 API。
// ============================================================

// [FIX-S1-3] 行名 "axrow_<tab>_<orig>" → 缓存索引解析（与 Page_AppxDel.cpp
// 同型静态实现，注释互指）。
static bool AxLocateCacheIndex(const wchar_t* pszRowName, int& nTab, int& nOrig)
{
    if (!pszRowName || _wcsnicmp(pszRowName, L"axrow_", 6) != 0) return false;
    const wchar_t* pTab = pszRowName + 6;
    const wchar_t* pUnd = wcschr(pTab, L'_');
    if (!pUnd) return false;
    nTab = _ttoi(pTab);
    nOrig = _ttoi(pUnd + 1);
    return true;
}

// [R155] 收集当前列表中勾选的包全名（从 Backup 抽出，原 12 行嵌套循环）。
// 行结构固定：pRow[0]=复选框，pRow[1]=信息列，info[0]=全名文本。
void CPageAppx::AxCollectCheckedFullNames(CVerticalLayoutUI* pList, std::vector<CDuiString>& vSel)
{
    for (int k = 0; k < pList->GetCount(); ++k)
    {
        CHorizontalLayoutUI* pRow = static_cast<CHorizontalLayoutUI*>(pList->GetItemAt(k));
        if (!pRow) continue;
        CCheckBoxUI* pCheck = static_cast<CCheckBoxUI*>(pRow->GetItemAt(0));
        if (!pCheck || !pCheck->GetCheck()) continue;
        CVerticalLayoutUI* pInfo = static_cast<CVerticalLayoutUI*>(pRow->GetItemAt(1));
        if (!pInfo) continue;
        CTextUI* pFull = static_cast<CTextUI*>(pInfo->GetItemAt(0));
        // [FIX-S1-3] 行首文本是本地化显示名而非 PackageFullName：按行名反查
        // 缓存取 sFull（备份按全名导出），解析失败回退旧文本。
        CDuiString sPick;
        int nRowTab = -1, nRowOrig = -1;
        if (AxLocateCacheIndex(pRow->GetName().GetData(), nRowTab, nRowOrig)
            && nRowTab >= 0 && nRowTab < 2
            && nRowOrig >= 0 && nRowOrig < static_cast<int>(m_vCache[nRowTab].size()))
        {
            sPick = m_vCache[nRowTab][nRowOrig].sFull;
        }
        else if (pFull && !pFull->GetText().IsEmpty())
        {
            sPick = pFull->GetText();
        }
        if (!sPick.IsEmpty()) vSel.push_back(sPick);
    }
}

void CPageAppx::Backup()
{
    CTabLayoutUI* pPages = static_cast<CTabLayoutUI*>(m_ctx.HostFindControl(L"appx_pages"));
    int nTab = pPages ? pPages->GetCurSel() : 0;
    if (nTab < 0) nTab = 0;
    CDuiString sListName;
    sListName.Format(L"appx_list_%d", nTab);
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(sListName.GetData()));
    if (!pList) return;
    std::vector<CDuiString> vSel;
    AxCollectCheckedFullNames(pList, vSel);
    if (vSel.empty())
    {
        m_ctx.HostAlert(StringMgr.Get("appx.s9dd12873"),
                        StringMgr.Get("appx.sebcd4943"), MB_ICONINFORMATION);
        return;
    }
    CDuiString sDir;
    if (!m_ctx.HostBrowseFolder(sDir, StringMgr.Get("appx.s_backup_browse_title")))
        return;   // 用户取消
    if (!AxRequireElevated(m_ctx)) return;
    CDuiString sLog;
    sLog.Format(L"[appx-backup] export %d packages to %s", static_cast<int>(vSel.size()), sDir.GetData());
    m_ctx.HostLog(sLog.GetData());
    m_ctx.HostSetCtrlText(L"sess_status", L"exporting appx...");
    // [R92] 备份动作提升到 AppxDeployService（同步执行 + 退出码回传）
    CDuiString sErr;
    EAppxDeployResult nDeploy = static_cast<EAppxDeployResult>(AppxDeployService::AppxBackup(vSel, sDir, sErr));
    if (nDeploy == EAppxDeployResult::APPX_DEPLOY_OK)
    {
        CDuiString sOkLog;
        sOkLog.Format(L"[appx-backup] exported %d package(s) to %s", static_cast<int>(vSel.size()), sDir.GetData());
        m_ctx.HostLog(sOkLog.GetData());
        CDuiString sBk;
        sBk.Format(StringMgr.Get("appx.s_backup_done2"), static_cast<int>(vSel.size()));
        m_ctx.HostToast(StringMgr.Get("appx.s_backup_title"), sBk.GetData(), 0);
        return;
    }
    CDuiString sFail;
    sFail.Format(L"[appx-backup] failed code=%d err=%s", nDeploy,
                 sErr.IsEmpty() ? L"unknown" : sErr.GetData());
    m_ctx.HostLog(sFail.GetData());
    // [R121-1] 失败分级 Toast（按 HRESULT 细分）
    ShowDeployFailToast(L"backup", nDeploy, sErr);
}

void CPageAppx::Restore()
{
    CDuiString sFile;
    if (!m_ctx.HostOpenFile(sFile, StringMgr.Get("appx.s_restore_filter"),
                            StringMgr.Get("appx.s_restore_title")))
        return;
    if (!AxRequireElevated(m_ctx)) return;
    CDuiString sErr;
    // 让用户选择依赖目录（可选，同 AppxInstall）
    CDuiString sDepDir;
    if (m_ctx.HostConfirmYesNo(StringMgr.Get("misc.s8f3077b7"),
                               StringMgr.Get("misc.s587c8055")))
    {
        if (m_ctx.HostBrowseFolder(sDepDir, StringMgr.Get("misc.s5c45dab4")))
        {
            EAppxDeployResult nDep = static_cast<EAppxDeployResult>(AppxDeployService::AppxRestore(sFile, sDepDir, sErr));
            if (nDep == EAppxDeployResult::APPX_DEPLOY_OK)
                m_ctx.HostToast(StringMgr.Get("appx.s_restore_title"),
                                StringMgr.Get("appx.s_restore_done2"), 0);
            else
                // [R121-1] 失败分级 Toast（按 HRESULT 细分）
                ShowDeployFailToast(L"restore", nDep, sErr);
            return;
        }
    }
    // 无依赖目录：直接安装
    EAppxDeployResult nMain = static_cast<EAppxDeployResult>(AppxDeployService::AppxRestore(sFile, L"", sErr));
    if (nMain == EAppxDeployResult::APPX_DEPLOY_OK)
    {
        CDuiString sLog;
        sLog.Format(L"[appx-restore] Add-AppxPackage OK: %s", sFile.GetData());
        m_ctx.HostLog(sLog.GetData());
        m_ctx.HostToast(StringMgr.Get("appx.s_restore_title"),
                        StringMgr.Get("appx.s_restore_done2"), 0);
    }
    else
    {
        CDuiString sLog;
        sLog.Format(L"[appx-restore] failed code=%d err=%s", nMain,
                     sErr.IsEmpty() ? L"unknown" : sErr.GetData());
        m_ctx.HostLog(sLog.GetData());
        // [R121-1] 失败分级 Toast（按 HRESULT 细分）
        ShowDeployFailToast(L"restore", nMain, sErr);
    }
}
