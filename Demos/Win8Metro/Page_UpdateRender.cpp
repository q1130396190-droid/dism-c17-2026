// ============================================================
// Page_UpdateRender.cpp —— 更新页「列表渲染族」（从 Page_Update.cpp 拆出）。
//
// [R139] 承接更新页全部行渲染代码：容器解析、逐行构建（WUA / CBS 两套
// 行型）、空态占位、分批接力、Tab 命中缓存重渲染。
//
// 闭包说明：全部是 CPageUpdate 的成员（声明见 Page_Update.h）。本文件不
// 引入任何新的文件级辅助，所需外部实体均来自既有头：
//   · FrameWnd_Util.h      UpdateTypeOf（更新类型分类）
//   · FrameWnd_Messages.h  UPD_RENDER_BATCH_TIMER_ID（分批接力定时器）
//   · CustomControls.h     CClickRowUI / CTextUI
//
// 搬迁为逐字搬运，零逻辑改动。
// ============================================================

#include "stdafx.h"
#include "ThemeTokens.h"   // [R201] 强调色令牌（Win10/11 均跟随系统主题色）

#include "Page_Update.h"

#include "StringManager.h"       // StringMgr
#include "FrameWnd_Util.h"       // UpdateTypeOf
#include "FrameWnd_Messages.h"   // UPD_RENDER_BATCH_TIMER_ID
#include "CustomControls.h"      // CClickRowUI / CTextUI

using namespace DuiLib;
using i18n::CStringManager;

// [C++17 R6] 本 TU 特征常量：行高与空态占位高度（与原字面量同值）。
inline constexpr int kUpdRowFixedHeight = 34;
inline constexpr int kUpdEmptyStateHeight = 60;
static_assert(kUpdEmptyStateHeight > 0, "empty-state placeholder must be visible");

[[nodiscard]] CVerticalLayoutUI* CPageUpdate::UpdResolveList(UpdDataMsg* pData)
{
// U-1/U-2: 按 nTab 分派到不同列表容器

CDuiString sListName = (pData && pData->nTab == 0)

                     ? L"upd_pending_list" : L"upd_list";

CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(sListName));



// 隐藏 Tab0 的占位 Text（U-1）

if (pData && pData->nTab == 0)

{

    CTextUI* pHint = static_cast<CTextUI*>(m_ctx.HostFindControl(L"upd_wu_hint"));

    if (pHint) pHint->SetText(L"");

}
return pList;

}

void CPageUpdate::UpdRenderRows(CVerticalLayoutUI* pList, UpdDataMsg* pData)
{
    // [性能 B1] 全量逐行渲染改为分批接力（≤50 行/批，15ms 定时器续批），
    // 消除单次 UI 线程长时间阻塞；数据源取已写入的 m_vUpdList[nTab]。
    int nTab = (pData && pData->nTab == 0) ? 0 : 1;
    UpdStartBatchRender(pList, nTab, false);
}

[[nodiscard]] CControlUI* CPageUpdate::UpdRenderRow(CVerticalLayoutUI* pList, const UpdEntry& e, size_t k, bool bPending)
{
    CDuiString sState = bPending ? StringMgr.Get("program.s72482be2")
                                     : m_ctx.HostSkinText(L"lbl_installed");

    auto pRow = std::make_unique<CClickRowUI>(); // [C++17 R3]

    // [修复 R5 / P1-15a] 行命名规则统一
    CDuiString sRowName;
    sRowName.Format(L"updrow_%d", static_cast<int>(k));
    pRow->SetName(sRowName.GetData());
    pRow->SetFixedHeight(kUpdRowFixedHeight);
    pRow->SetBkColor(tokens::Token(tokens::EToken::Surface));
    RECT rcInset = { 10, 0, 0, 0 };
    pRow->SetInset(rcInset);
    pRow->SetToolTip(e.sTitle.GetData());   // [D3] 长更新标题悬停可见

    // [性能 B3] valign/endellipsis/align 属性字符串解析改为 SetTextStyle 直设
    auto pTitle = std::make_unique<CTextUI>(); // [C++17 R3]
    pTitle->SetText(e.sTitle);
    pTitle->SetFixedWidth(0);
    pTitle->SetTextColor(tokens::Token(tokens::EToken::Ink));
    pTitle->SetTextStyle(DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    pTitle->SetMouseEnabled(false);
    pRow->Add(pTitle.release());  // [C++17 R3] ownership to DuiLib

    auto pDate = std::make_unique<CTextUI>(); // [C++17 R3]
    pDate->SetText(bPending ? L"—" : e.sDate);
    pDate->SetFixedWidth(120);   // [R217] 240 was 3x what "2026-10-30" needs (~80px);
    // the surplus came out of the elastic title column, truncating KB captions early.
    pDate->SetTextColor(tokens::Token(tokens::EToken::Ink));
    pDate->SetTextStyle(DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    pDate->SetMouseEnabled(false);
    pRow->Add(pDate.release());  // [C++17 R3] ownership to DuiLib

    auto pState = std::make_unique<CTextUI>(); // [C++17 R3]
    // [R52] 类型标签：按标题关键字识别安全/累积/定义/功能/驱动
    int nType = UpdateTypeOf(e.sTitle);
    CDuiString sTypeTag = sState;
    COLORREF nStateColor = UpdStateColor(e.sTitle, bPending);
    CDuiString sPrefix;
    if (nType == 1)      { sPrefix = StringMgr.Get("misc.h3b58"); sTypeTag = sPrefix + sState; }
    else if (nType == 2) { sPrefix = StringMgr.Get("misc.h5fb8"); sTypeTag = sPrefix + sState; }
    else if (nType == 3) { sPrefix = StringMgr.Get("misc.h3bdc"); sTypeTag = sPrefix + sState; }
    else if (nType == 4) { sPrefix = StringMgr.Get("misc.h8836"); sTypeTag = sPrefix + sState; }
    else if (nType == 5) { sPrefix = StringMgr.Get("misc.h777c"); sTypeTag = sPrefix + sState; }
    pState->SetText(sTypeTag);
    pState->SetFixedWidth(180);
    pState->SetTextColor(nStateColor);
    pState->SetTextStyle(DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    pState->SetMouseEnabled(false);
    pRow->Add(pState.release());  // [C++17 R3] ownership to DuiLib

    pList->Add(pRow.get());  // [FIX-S1-5b] Add 接管所有权（原 release 后再 release 恒返回 nullptr）
    return pRow.release();   // 返回裸指针存入 m_vpRows 作非拥有引用（行高亮/全选底色/主题重着色依赖它） // [C++17 R3] ownership transferred to caller
}

void CPageUpdate::UpdRenderEmptyState(CVerticalLayoutUI* pList, UpdDataMsg* pData)
{


        // 空态：Tab0→已是最新, Tab1→无已安装更新
        // [FIX-S1-5] 枚举失败必须呈现错误态而非"已是最新"假成功：
        // nErr 非 0（WUA 服务停用/Search 失败等）时显示错误码，用户可据此处理。

        auto pEmpty = std::make_unique<CTextUI>(); // [C++17 R3]

        if (pData && pData->nErr != 0)
        {
            CDuiString sErr;
            sErr.Format(L"更新扫描失败（错误码 0x%08X）。请确认 Windows Update 服务（wuauserv）是否正在运行。",
                        static_cast<unsigned>(pData->nErr));
            pEmpty->SetText(sErr.GetData());
        }
        else
        {
            pEmpty->SetText(pData && pData->nTab == 0

                          ? StringMgr.Get("update.sac3b9fc9")

                          : StringMgr.Get("update.sc084171a"));
        }

        pEmpty->SetFixedHeight(kUpdEmptyStateHeight);

        pEmpty->SetMouseEnabled(false);

        pList->Add(pEmpty.release());  // [C++17 R3] ownership to DuiLib

}


// [性能 A2/B1] CBS 行渲染（cbsprow_ 前缀约定保留；不进入 m_vpRows 勾选索引）
void CPageUpdate::UpdRenderCbsRow(CVerticalLayoutUI* pList, const UpdEntry& e, size_t k)
{
    auto pRow = std::make_unique<CClickRowUI>(); // [C++17 R3]
    CDuiString sRowName;
    sRowName.Format(L"cbsprow_%d", static_cast<int>(k));
    pRow->SetName(sRowName.GetData());
    pRow->SetFixedHeight(kUpdRowFixedHeight);
    pRow->SetBkColor(tokens::Token(tokens::EToken::Surface));
    RECT rcInset = { 10, 0, 0, 0 };
    pRow->SetInset(rcInset);
    pRow->SetToolTip(e.sTitle.GetData());

    auto pTitle = std::make_unique<CTextUI>(); // [C++17 R3]
    pTitle->SetText(e.sTitle);
    pTitle->SetFixedWidth(0);
    pTitle->SetTextColor(tokens::Token(tokens::EToken::InkTertiary));
    pTitle->SetTextStyle(DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    pTitle->SetMouseEnabled(false);
    pRow->Add(pTitle.release());  // [C++17 R3] ownership to DuiLib

    auto pDate = std::make_unique<CTextUI>(); // [C++17 R3]
    pDate->SetText(e.sDate.IsEmpty() ? CDuiString(L"\u2014") : e.sDate);
    pDate->SetFixedWidth(120);   // [R217] 240 was 3x what "2026-10-30" needs (~80px);
    // the surplus came out of the elastic title column, truncating KB captions early.
    pDate->SetTextColor(tokens::Token(tokens::EToken::InkTertiary));
    pDate->SetTextStyle(DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    pDate->SetMouseEnabled(false);
    pRow->Add(pDate.release());  // [C++17 R3] ownership to DuiLib

    auto pState = std::make_unique<CTextUI>(); // [C++17 R3]
    pState->SetText(L"[CBS] DISM 包");
    pState->SetFixedWidth(180);
    pState->SetTextColor(tokens::Token(tokens::EToken::Success));
    pState->SetTextStyle(DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    pState->SetMouseEnabled(false);
    pRow->Add(pState.release());  // [C++17 R3] ownership to DuiLib

    pList->Add(pRow.release());  // [C++17 R3] ownership to DuiLib
}

// [性能 B1] 启动分批渲染（仿 Capability RenderBatch 先例：50 行/批，15ms 接力）
void CPageUpdate::UpdStartBatchRender(CVerticalLayoutUI* pList, int nTab, bool bCbs)
{
    m_pRenderList = pList;
    m_nRenderTab = nTab;
    m_bRenderCbs = bCbs;
    m_nRenderPos = 0;
    UpdRenderBatchStep();
}

// [R187] 从 UpdRenderBatchStep（48 行 / CC12）抽出：渲染单行并把行指针存入索引。
// 索引语义必须与 updrow_%d 的 k 严格对齐，因此分三种情况补齐：
//   · 现有 size < k   → 先 resize 到 k（中间补 nullptr 占位）
//   · 现有 size == k  → push_back（正常顺序追加）
//   · 现有 size > k   → 直接覆写第 k 位（重渲染同一 Tab 时）
// pRow 为空不入索引（UpdRenderRow 在专家模式隐藏等情况下会返回 nullptr）。
void CPageUpdate::UpdRenderOneRow(const UpdEntry& e, size_t k, bool bPending)
{
    CControlUI* pRow = UpdRenderRow(m_pRenderList, e, k, bPending);
    if (!pRow) return;
    std::vector<CControlUI*>& vRows = m_vpRows[m_nRenderTab];
    if (static_cast<int>(vRows.size()) < static_cast<int>(k)) vRows.resize(k, nullptr);
    if (static_cast<int>(vRows.size()) == static_cast<int>(k)) vRows.push_back(pRow);
    else                                                       vRows[k] = pRow;
}

void CPageUpdate::UpdRenderBatchStep()
{
    if (!m_pRenderList || m_nRenderTab < 0)
    {
        ::KillTimer(m_ctx.HostHwnd(), UPD_RENDER_BATCH_TIMER_ID);
        return;
    }

    const std::vector<UpdEntry>& vSrc = m_bRenderCbs ? m_vCbsDedup : m_vUpdList[m_nRenderTab];
    // [FULL-LOAD] 全量加载渲染：取消 50 行/批 + 15ms 接力，一次构建全部行。
    // vSrc 是引用而非快照，全量消费使其在同一帧内用完，不存在跨批次
    // 被替换导致的下标错位。
    bool bPending = (m_nRenderTab == 0);
    const size_t nEnd = vSrc.size();

    for (size_t k = m_nRenderPos; k < nEnd; ++k)
    {
        if (m_bRenderCbs) UpdRenderCbsRow(m_pRenderList, vSrc[k], k);
        else              UpdRenderOneRow(vSrc[k], k, bPending);   // [R187] 抽为函数
    }
    m_nRenderPos = nEnd;

    ::KillTimer(m_ctx.HostHwnd(), UPD_RENDER_BATCH_TIMER_ID);
    CVerticalLayoutUI* pDoneList = m_pRenderList;
    int nDoneTab = m_nRenderTab;
    bool bDoneCbs = m_bRenderCbs;
    m_pRenderList = nullptr;
    m_nRenderTab = -1;

    // [性能 A2] WUA tab1 渲染完成后并入 CBS（若在途则 CBS 回调延后）
    if (!bDoneCbs && nDoneTab == 1 && m_bCbsReady)
        UpdMergeCbs();
    (void)pDoneList;
}

// [性能 C3] Tab 切换命中缓存：仅从 m_vUpdList[nTab] 重渲染，不重查
void CPageUpdate::RenderTabFromCache(int nTab)
{
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(
        m_ctx.HostFindControl(nTab == 0 ? L"upd_pending_list" : L"upd_list"));
    if (!pList) return;
    pList->RemoveAll();
    m_vpRows[nTab].clear();
    if (!m_vUpdList[nTab].empty())
        UpdStartBatchRender(pList, nTab, false);
    else
    {
        // 空态占位（与 UpdRenderEmptyState 文案一致）
        auto pEmpty = std::make_unique<CTextUI>(); // [C++17 R3]
        pEmpty->SetText(nTab == 0 ? StringMgr.Get("update.sac3b9fc9")
                                  : StringMgr.Get("update.sc084171a"));
        pEmpty->SetFixedHeight(kUpdEmptyStateHeight);
        pEmpty->SetMouseEnabled(false);
        pList->Add(pEmpty.release());  // [C++17 R3] ownership to DuiLib
        if (nTab == 1 && m_bCbsReady) UpdMergeCbs();
    }
    // 过滤关键字仍在时按新 Tab 数据重新过滤
    CDuiString sKw = m_ctx.HostReadEditValue(L"upd_filter");
    if (!sKw.IsEmpty()) ApplyFilter();
}

// [R225] Status-column colour for one row. Extracted so the builder and the theme
//   refresher share ONE definition -- the project has repeatedly been bitten by
//   "build-time writes one colour, refresh writes another".
DWORD CPageUpdate::UpdStateColor(const CDuiString& sTitle, bool bPending) const
{
    // Pending rows show the accent colour; installed rows use the normal ink.
    if (bPending) return tokens::Token(tokens::EToken::Accent);
    const int nType = UpdateTypeOf(sTitle);
    switch (nType)
    {
    case 1:  return tokens::Token(tokens::EToken::Danger);    // security
    case 4:  return tokens::Token(tokens::EToken::WarnBg);    // definition update
    case 5:  return tokens::Token(tokens::EToken::Success);   // feature update
    default: return tokens::Token(tokens::EToken::Ink);       // cumulative / driver
    }
}

// [R225] Re-colour both tabs after a light<->dark switch.
//
// Contract (PageBase.h): colour attributes only. No Build()/Add/Remove -- that would
// drop the lists, the checked state and the scroll position, and leave m_pFocusCtrl
// dangling.
//
// The row cache m_vpRows[2] is index-aligned with m_vUpdList[2] and m_vChecked[2]
// (maintained by UpdRenderOneRow), so walking it covers both tabs without searching
// the tree. Row children are UNNAMED, so columns are selected by position:
// 0 = title, 1 = date, 2 = status.
void CPageUpdate::RefreshThemeColors()
{
    for (int nTab = 0; nTab < 2; ++nTab)
    {
        // tab 0 = pending, tab 1 = installed. The flag drives the status colour.
        const bool bPending = (nTab == 0);
        for (size_t k = 0; k < m_vpRows[nTab].size(); ++k)
        {
            CControlUI* pRow = m_vpRows[nTab][k];
            if (!pRow) continue;

            pRow->SetBkColor(tokens::Token(tokens::EToken::Surface));

            IContainerUI* pBox = static_cast<IContainerUI*>(
                pRow->GetInterface(DUI_CTR_CONTAINER));
            if (!pBox) continue;

            const CDuiString sTitle =
                (k < m_vUpdList[nTab].size()) ? m_vUpdList[nTab][k].sTitle : CDuiString();

            for (int c = 0; c < pBox->GetCount(); ++c)
            {
                CTextUI* pText = dynamic_cast<CTextUI*>(pBox->GetItemAt(c));
                if (!pText) continue;
                if (c == 2)
                    pText->SetTextColor(UpdStateColor(sTitle, bPending));
                else
                    pText->SetTextColor(tokens::Token(tokens::EToken::Ink));
            }
        }
    }
}
