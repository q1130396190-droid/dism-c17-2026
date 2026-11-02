// ============================================================
// Page_AppxCtx.cpp —— 应用页「行级右键菜单 + 依赖图」（从 Page_Appx.cpp 拆出）。
//
// [R179] Page_Appx.cpp 拆前 1,230 行、规模分 9.10（六页最低）。本文件承接
// 八个成员，合计 210 行，边界按「从行级右键菜单进入的两条支线」划分：
//   · ShowRowContextMenu   弹出行级菜单（5 项）并分派
//   · OpenInstallLocation  打开包的安装位置（Explorer 选中）
//   · CopyFullName         复制 PackageFullName
//   · CopyFamilyName       复制 PackageFamilyName
//   · ShowDepGraph         依赖关系图（弹窗式文本视图）
//   · OnDepData            依赖查询结果回填
//   · OnGlobalCopy         Ctrl+C 全局复制当前选中行
//   · OnGlobalSelectAll    Ctrl+A 全选
//
// 闭包说明：八个成员声明见 Page_Appx.h。跨 TU 依赖仅五项且都是头文件级：
//   · m_ctx（HostHwnd/HostLog/HostToast/HostFindControl/HostCopyText）
//   · m_vCache（按 Tab 的包缓存，头文件已声明）
//   · StringMgr（appx.s_ctx_* / appx.s_* 语言键，键名逐字保持，未新增）
//   · UiHelpers_ShowUnifiedListContextMenu（FrameWnd_UiHelpers.h）
//   · EListCtx::LCX_APPX（同上）
//
// 逐字搬运，零逻辑改动。
// ============================================================

#include "stdafx.h"

#include "Page_Appx.h"

#include "StringManager.h"
#include "FrameWnd_UiHelpers.h"   // UiHelpers_ShowUnifiedListContextMenu / EListCtx
#include "FrameWnd_Util.h"       // FrameWnd_Util_CopyTextToClipboard
#include "CustomControls.h"       // CVerticalLayoutUI
#include <shellapi.h>             // SHELLEXECUTEINFOW

using namespace DuiLib;
using i18n::CStringManager;

// [R179] 分页数（待安装 / 已安装）。原实现 6 处写死 `nTab >= 2`，
// 一旦新增 Tab 就会漏改其中几处，导致越界访问 m_vCache[nTab]。
inline constexpr int kAppxTabCount = 2;

// [R179] Tab 编号是否在有效范围内。原实现在 6 个函数里各写一遍
// `if (nTab < 0 || nTab >= 2) return;`（后 5 处已随 kAppxTabCount 具名化），
// 合并为单一判定：越界一律视为"不是本页数据"，调用方直接返回。
// [[nodiscard]] 是刻意的 —— 丢弃返回值就等于放过一次 m_vCache[nTab] 越界访问。
[[nodiscard]] static bool AppxTabValid(int nTab) noexcept
{
    return nTab >= 0 && nTab < kAppxTabCount;
}

// ============================================================
// [R123-1] 行级右键菜单增强：打开安装位置 / 复制包名 / 复制 FamilyName
// 由 OnContextMenu axrow_{tab}_{k} 命中进入，比统一 EListCtx::LCX_APPX 更精细。
// ============================================================
void CPageAppx::ShowRowContextMenu(int nTab, int k, POINT pt)
{
    if (!AppxTabValid(nTab)) return;
    if (k < 0 || k >= static_cast<int>(m_vCache[nTab].size())) return;
    ShowDetail(nTab, k);   // 同步高亮选中行
    HMENU hMenu = ::CreatePopupMenu();
    ::AppendMenu(hMenu, MF_STRING, 4401, StringMgr.Get("appx.s_ctx_openloc"));
    ::AppendMenu(hMenu, MF_STRING, 4402, StringMgr.Get("appx.s_ctx_copyfull"));
    ::AppendMenu(hMenu, MF_STRING, 4403, StringMgr.Get("appx.s_ctx_copyfamily"));
    ::AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenu(hMenu, MF_STRING, 4404, StringMgr.Get("appx.s_ctx_detail"));
    ::AppendMenu(hMenu, MF_STRING, 4405, StringMgr.Get("appx.s_ctx_depgraph"));
    int iCmd = ::TrackPopupMenu(hMenu,
        TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
        pt.x, pt.y, 0, m_ctx.HostHwnd(), nullptr);
    ::DestroyMenu(hMenu);
    if (iCmd == 4401)         OpenInstallLocation(nTab, k);
    else if (iCmd == 4402)    CopyFullName(nTab, k);
    else if (iCmd == 4403)    CopyFamilyName(nTab, k);
    else if (iCmd == 4404)    ShowDetail(nTab, k);
    else if (iCmd == 4405)    ShowDepGraph(nTab, k);
}

void CPageAppx::OpenInstallLocation(int nTab, int k)
{
    if (!AppxTabValid(nTab)) return;
    if (k < 0 || k >= static_cast<int>(m_vCache[nTab].size())) return;
    const CDuiString& sPath = m_vCache[nTab][k].sPath;
    if (sPath.IsEmpty())
    {
        m_ctx.HostToast(StringMgr.Get("appx.s_ctx_openloc_fail"),
                        StringMgr.Get("appx.s_ctx_openloc_fail_body"), 1);
        return;
    }
    // 目录已存在 → 打开目录；否则尝试定位文件
    DWORD dwAttr = ::GetFileAttributes(sPath.GetData());
    if (dwAttr != INVALID_FILE_ATTRIBUTES && (dwAttr & FILE_ATTRIBUTE_DIRECTORY))
    {
        ::ShellExecuteW(m_ctx.HostHwnd(), L"open", sPath.GetData(), nullptr, nullptr, SW_SHOWNORMAL);
        CDuiString sLog;
        sLog.Format(L"[appx] open install dir: %s", sPath.GetData());
        m_ctx.HostLog(sLog.GetData());
    }
    else if (UiHelpers_OpenExplorerSelect(m_ctx.HostHwnd(), sPath.GetData()))
    {
        CDuiString sLog;
        sLog.Format(L"[appx] explorer /select: %s", sPath.GetData());
        m_ctx.HostLog(sLog.GetData());
    }
    else
    {
        m_ctx.HostToast(StringMgr.Get("appx.s_ctx_openloc_fail"),
                        StringMgr.Get("appx.s_ctx_openloc_fail_body"), 1);
    }
}

void CPageAppx::CopyFullName(int nTab, int k)
{
    if (!AppxTabValid(nTab)) return;
    if (k < 0 || k >= static_cast<int>(m_vCache[nTab].size())) return;
    const CDuiString& sFull = m_vCache[nTab][k].sFull;
    if (sFull.IsEmpty()) return;
    FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), sFull.GetData());
    m_ctx.HostToast(StringMgr.Get("appx.s_ctx_copyfull_done"),
                    StringMgr.Get("appx.s_ctx_copyfull_done_body"), 0);
    CDuiString sLog;
    sLog.Format(L"[appx] copy full name: %s", sFull.GetData());
    m_ctx.HostLog(sLog.GetData());
}

void CPageAppx::CopyFamilyName(int nTab, int k)
{
    if (!AppxTabValid(nTab)) return;
    if (k < 0 || k >= static_cast<int>(m_vCache[nTab].size())) return;
    const CDuiString& sFam = m_vCache[nTab][k].sFamily;
    if (sFam.IsEmpty()) return;
    FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), sFam.GetData());
    m_ctx.HostToast(StringMgr.Get("appx.s_ctx_copyfamily_done"),
                    StringMgr.Get("appx.s_ctx_copyfamily_done_body"), 0);
    CDuiString sLog;
    sLog.Format(L"[appx] copy family name: %s", sFam.GetData());
    m_ctx.HostLog(sLog.GetData());
}
// ============================================================
// [R-B1] Ctrl+A Appx 页全选当前 Tab
// ============================================================

void CPageAppx::OnGlobalSelectAll()
{
    SelectAll();
    m_ctx.HostLog(L"[appx] Ctrl+A select all");
}

// ============================================================
// [R-B1] Ctrl+C / Ctrl+X 复制当前 Tab 首个条目 Full Name
// ============================================================

void CPageAppx::OnGlobalCopy([[maybe_unused]] bool bCut)
{
    CTabLayoutUI* pPages = static_cast<CTabLayoutUI*>(m_ctx.HostFindControl(L"appx_pages"));
    int nTab = pPages ? pPages->GetCurSel() : 0;
    if (nTab < 0) nTab = 0;
    if (!AppxTabValid(nTab)) return;
    if (m_vCache[nTab].empty())
    {
        m_ctx.HostToast(StringMgr.Get("appx.s_nocopy"),
                        StringMgr.Get("appx.s_nocopy_body"), 1);
        return;
    }
    CopyFullName(nTab, 0);
}

// ============================================================
// [R-B25] Appx 依赖 TSV 回读完成：按 FullName 匹配填回 sDependencies
// 由 FrameWnd WM_APPX_DEPDATA 分发调用（lParam = AppxDepsMsg*，本函数负责 delete）。
// ============================================================
// [R164] 从 OnDepData 抽出：把依赖查询结果按 sFull 回填到两个 tab 的缓存。
//   匹配键是 sFull（完整包名）而不是显示名 —— 显示名可能重复，
//   用显示名匹配会把 A 的依赖写到 B 上，且不会报错，只是列表看起来"依赖错了"。
//   sFull 为空的条目跳过：它们是尚未完成身份解析的占位项。
//
// 复杂度：先建 sFull → deps 索引再单趟回填，O(N+M)（原为 O(tab×N×M) 线性比对）。
//   重复 sFull 取第一条，与原实现的 break 语义一致。
void CPageAppx::FillDepsIntoCache(const std::vector<AppxDepEntry>& vDeps)
{
    // [R164] 先建索引再单趟回填：O(tab×N×M) → O(N+M)，嵌套 5→2。
    //   重复 sFull 取第一条 —— 与原实现的 break 语义严格一致。
    std::map<std::wstring, const CDuiString*> idx;
    for (size_t d = 0; d < vDeps.size(); ++d)
        idx.emplace(std::wstring(vDeps[d].sFull.GetData()), &vDeps[d].sDeps);

    for (int tab = 0; tab < 2; ++tab)
    {
        for (size_t k = 0; k < m_vCache[tab].size(); ++k)
        {
            CDuiString& sFull = m_vCache[tab][k].sFull;
            if (sFull.IsEmpty()) continue;
            auto it = idx.find(std::wstring(sFull.GetData()));
            if (it != idx.end()) m_vCache[tab][k].sDependencies = *it->second;
        }
    }
}

void CPageAppx::OnDepData(AppxDepsMsg* pMsg)
{
    if (!pMsg) return;
    // [C++17 R3] receiver takes ownership, freed at scope end (was: delete pMsg at end)
    std::unique_ptr<AppxDepsMsg> msgGuard(pMsg);
    // [R164] 回填抽为 FillDepsIntoCache，嵌套 5→2
    FillDepsIntoCache(pMsg->vDeps);
    if (pMsg->nErr != 0)
    {
        m_ctx.HostLog(L"[appx] dependency query failed");
    }
    else if (!pMsg->vDeps.empty())
    {
        m_ctx.HostLog(L"[appx] dependency TSV loaded");
    }
    // [C++17 R3] msgGuard frees pMsg at scope end
}
// ============================================================
// [R-B25] Appx 依赖图：打开 EToolDlgKind::TD_DEPGRAPH 弹窗
// 数据源：m_vCache[nTab][k].sDependencies（OnDepData 从 appx_deps.tsv 回读）
// ============================================================
void CPageAppx::ShowDepGraph(int nTab, int k)
{
    if (!AppxTabValid(nTab)) return;
    if (k < 0 || k >= static_cast<int>(m_vCache[nTab].size())) return;
    const AppxPkg& pkg = m_vCache[nTab][k];
    CDuiString sRoot = pkg.sFull;
    if (sRoot.IsEmpty())
    {
        sRoot = pkg.sFamily;
    }
    // 解析逗号分隔的依赖包列表
    std::vector<CDuiString> vDeps;
    CDuiString sWork = pkg.sDependencies;
    int nPos = 0;
    while (nPos < static_cast<int>(sWork.GetLength()))
    {
        int nComma = sWork.Find(L",", nPos);
        CDuiString sOne;
        if (nComma < 0)
        {
            sOne = sWork.Mid(nPos);
            nPos = static_cast<int>(sWork.GetLength());
        }
        else
        {
            sOne = sWork.Mid(nPos, nComma - nPos);
            nPos = nComma + 1;
        }
        sOne.Trim();
        if (!sOne.IsEmpty()) vDeps.push_back(sOne);
    }
    // [R127-A] 依赖图对话框组装与展示提取为共享 helper
    //   （原与 Page_Startup::ShowServiceDepGraph 逐字重复）
    UiHelpers_ShowDepGraphDialog(&m_ctx, sRoot.GetData(), vDeps);
    CDuiString sLog;
    sLog.Format(L"[appx] dep graph: tab=%d idx=%d deps=%d",
                nTab, k, static_cast<int>(vDeps.size()));
    m_ctx.HostLog(sLog.GetData());
}
