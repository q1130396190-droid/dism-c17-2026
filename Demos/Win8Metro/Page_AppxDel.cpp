#include "stdafx.h"
// ============================================================
// Page_AppxDel.cpp —— 应用商店页「删除/卸载」子系统。
//
// [R140-1] 从 Page_Appx.cpp（2122 行）拆出 169 行：
//   AxCollectChecked      采集勾选包全名 + 按行名反查缓存累计大小
//   AxConfirmDelete       删除前确认对话框
//   AxDeployRemove        执行移除（AppxDeployService）
//   AxReportDeleteResult  结果汇总与提示
//
// 闭包说明：4 个成员函数声明见 Page_Appx.h；其余依赖为
// StringMgr / ToolSql::HostConfirm / m_vCache（成员），均经下述 include 引入。
//
// 搬迁为逐字搬运，零逻辑改动。
// ============================================================
#include "Page_Appx.h"
#include "Page_ToolsCommon.h"     // ToolSql::HostConfirm
#include "StringManager.h"
#include "FrameWnd_Util.h"        // AppxFamily
#include "AppxDeployService.h"
#include <string>
#include <vector>
using namespace DuiLib;
using i18n::CStringManager;
// 删除阶段的状态码（与 AxReportDeleteResult 的汇总口径一致）
inline constexpr int kAppxDelOk      = 0;
inline constexpr int kAppxDelPartial = 1;
// [R166] 通过行名 axrow_{tab}_{origK} 反查缓存拿真实包大小。
// [R39] 从 AxCollectChecked 的嵌套块抽出（原含 2 层 if + 1 次 wcschr 解析）。
// 返回 0 表示无法解析或越界（调用方累加时不做判断）。
ULONGLONG CPageAppx::AxRowCacheSize(const CControlUI* pRow, int nTab) const
{
    const wchar_t* pszRowName = pRow ? pRow->GetName() : nullptr;
    if (!pszRowName || wcsncmp(pszRowName, L"axrow_", 6) != 0) return 0;
    // 解析最后一段（origK）：格式 axrow_{tab}_{origK}
    const wchar_t* pTabEnd = wcschr(pszRowName + 6, L'_');
    if (!pTabEnd) return 0;
    const int nOrig = _ttoi(pTabEnd + 1);
    if (nTab < 0 || nTab >= 2) return 0;
    if (nOrig < 0 || nOrig >= static_cast<int>(m_vCache[nTab].size())) return 0;
    return m_vCache[nTab][nOrig].ullSize;
}
// [FIX-S1-3] 行名 "axrow_<tab>_<orig>" → 缓存索引解析（AxCollectChecked /
// AxCollectCheckedFullNames 共用语义；两 TU 各持一份静态实现，注释互指）。
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

void CPageAppx::AxCollectChecked(CVerticalLayoutUI* pList, int nTab,
                                 std::vector<CDuiString>& vSel,
                                 ULONGLONG& ullTotalSize)
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
        // [FIX-S1-3] 卸载/备份 API 需要 PackageFullName（AppxDeployService 按
        // 全名操作），行首文本却是本地化显示名（AppxDisplayName）—— 直接采集
        // 显示名导致"有语言包名的包"操作必失败。改按行名反查缓存取 sFull，
        // 解析失败回退旧文本（防御）。
        CDuiString sPick;
        int nRowTab = nTab, nRowOrig = -1;
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
        ullTotalSize += AxRowCacheSize(pRow, nTab);
    }
}
// [AUDIT-P2] 从 Delete 提取：空选择提示 + 二次确认（含将释放空间）
// 返回 true 表示用户确认继续。
bool CPageAppx::AxConfirmDelete(const std::vector<CDuiString>& vSel,
                                ULONGLONG ullTotalSize)
{
if (vSel.empty())
{
    m_ctx.HostAlert(StringMgr.Get("appx.s9dd12873"),
        StringMgr.Get("appx.sebcd4943"), MB_ICONINFORMATION);
    return false;
}
    CDuiString sConfirm;
    // [R39] 二次确认框展示将释放的总磁盘空间（提升破坏性操作可逆感知）
    CDuiString sTotalSizeStr;
    CCleanEngine::FormatSize(ullTotalSize, sTotalSizeStr);
    sConfirm.Format(StringMgr.Get("misc.h166a"),
                    static_cast<int>(vSel.size()),
                    sTotalSizeStr.IsEmpty() ? StringMgr.Get("misc.h1622")
                                            : sTotalSizeStr.GetData());
    return m_ctx.HostConfirmYesNo(StringMgr.Get("general.confirm"),
                                  sConfirm.GetData(), false);
}
// [AUDIT-P2] 从 Delete 提取：按 Tab 分派（Tab0 卸载包 / Tab1 移除预置包）
EAppxDeployResult CPageAppx::AxDeployRemove(int nTab,
                                             const std::vector<CDuiString>& vSel,
                                             CDuiString& sErr)
{
// [R92] 部署动作提升到 AppxDeployService（同步执行 + 退出码回传）


EAppxDeployResult nDeploy = EAppxDeployResult::APPX_DEPLOY_ERR_ARG;
if (nTab == 0)
    nDeploy = static_cast<EAppxDeployResult>(AppxDeployService::AppxRemovePackage(vSel, sErr));
else
{
    // provisioned tab stores families; rebuild family from full name
    std::vector<CDuiString> vFam;
    for (size_t k = 0; k < vSel.size(); ++k)
    {
        CDuiString sF = AppxFamily(vSel[k]);
        bool bDup = false;
        for (size_t j = 0; j < vFam.size(); ++j)
            if (lstrcmpiW(vFam[j].GetData(), sF.GetData()) == 0) bDup = true;
        if (!bDup) vFam.push_back(sF);
    }
    nDeploy = static_cast<EAppxDeployResult>(AppxDeployService::AppxRemoveProvisioned(vFam, sErr));
}
    return nDeploy;
}
// [AUDIT-P2] 从 Delete 提取：成功写日志+Toast；失败走分级 Toast
void CPageAppx::AxReportDeleteResult(EAppxDeployResult nDeploy,
                                     const std::vector<CDuiString>& vSel,
                                     const CDuiString& sErr)
{
if (nDeploy == EAppxDeployResult::APPX_DEPLOY_OK)
{
    CDuiString sDone;
    sDone.Format(L"[appx-delete] removed %d package(s)", static_cast<int>(vSel.size()));
    m_ctx.HostLog(sDone.GetData());
    CDuiString sDone2;
    sDone2.Format(StringMgr.Get("appx.s_del_done"), static_cast<int>(vSel.size()));
    m_ctx.HostToast(L"Appx", sDone2.GetData(), 0);
}
else
{
    CDuiString sFail;
    sFail.Format(L"[appx-delete] failed code=%d err=%s", nDeploy,
                 sErr.IsEmpty() ? L"unknown" : sErr.GetData());
    m_ctx.HostLog(sFail.GetData());
    // [R121-1] 失败分级 Toast（按 HRESULT 细分）
    ShowDeployFailToast(L"delete", nDeploy, sErr);
}
}
