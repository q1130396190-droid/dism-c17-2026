// ============================================================
// Page_FeatureInteraction.cpp —— 功能页「勾选 / 展开 / 详情面板 / 关键字过滤」。
//
// [R102] 从 Page_Feature.cpp 拆出。选中这一段的原因：
//   1. 它不参与 R5（RebuildListBatchStep 分批渲染）与 R6（进页过期缓存兜底）
//      判据，拆走不会让隔离守卫看错文件；
//   2. 它处理的是「用户操作后的状态变更」，与加载链路（Load/OnPartial/OnData）
//      和渲染链路（Page_FeatureRender.cpp）是三条独立的线；
//   3. 聚合计算的缓存失效点（InvalidateAggCache）不在本段，避免与 Agg 耦合。
//
// 拆分前后函数体逐字未改，仅搬迁位置。
// ============================================================

#include "stdafx.h"
#include "ThemeTokens.h"   // [R201] 强调色令牌（Win10/11 均跟随系统主题色）

#include "Page_Feature.h"

#include "StringManager.h"       // StringMgr 本地化宏
#include "FrameWnd_Util.h"       // 详情面板重排/滚动定位

#include <algorithm>

using namespace DuiLib;
using i18n::CStringManager;

// [CRASHFIX-UAF] 勾选/展开的整表重建统一延迟入口。
// 调用点都在控件事件派发栈内（CClickRowUI::DoEvent → SendNotify → OnEvent
// → ToggleCheck/ToggleExpand），此时 RebuildList 的 pList->RemoveAll() 会
// delete 正在派发的控件，栈回退后基类继续用 this → UAF（崩溃日志实证
// RIP 野跳转）。经 HostDefer 排到 WM_UI_DEFERRED_EXEC 执行，事件栈已空。
// 去重：多次连点只排一次重建；重建按 m_vFeatures 当前值渲染，语义等价
// "每次都重建"的最终态。
void CPageFeature::DeferRebuildList()
{
    if (m_bPendingRebuild) return;
    m_bPendingRebuild = true;
    m_ctx.HostDefer([this]
    {
        m_bPendingRebuild = false;
        RebuildList();
    });
}

// [FIX-UX] 渲染接力期间的点击意图入队（同下标同类操作去重，保留最后一次）。
namespace
{
void QueuePendingIntent(std::vector<std::pair<int, bool>>& v, int n, bool bExpand)
{
    for (auto& it : v)
    {
        if (it.first == n && it.second == bExpand) return;   // 重复意图，忽略
    }
    v.push_back({ n, bExpand });
}
} // namespace

void CPageFeature::ToggleCheck(int n)
{
    if (!m_bFeatLoaded || n < 0 || n >= static_cast<int>(m_vFeatures.size())) return;

    // [T8 P3] 接力期间拒绝交互：行尚未全部挂载，点击沿用的下标可能与
    // 可见行不一致（尤其展开态变化后），先让本批渲染完再操作。
    // [FIX-UX] 但不再丢弃意图：入队，接力收尾时补执行。
    if (m_bBatchRendering)
    {
        QueuePendingIntent(m_vPendingIntents, n, false);
        return;
    }

    SelectFeat(n);  // P-1: 记录当前选中功能索引

    // [修复 R5 / P1-13] 行循环：全关/半选 → 启用；全开 → 永久移除；
    // 全部已标记移除 → 恢复为普通关闭。
    // pending 含义：-1 不变 / 0 关 / 1 启 / 2 永久移除（/remove）

    int nAgg = Agg(n);

    int nTarget;

    if (nAgg == 1)      nTarget = 2;

    else if (nAgg == 3) nTarget = 0;

    else                nTarget = 1;

    std::function<void(int)> setSub = [&](int x)

    {
        m_vFeatures[x].nPending = nTarget;

        for (size_t i = 0; i < m_vFeatures[x].vChild.size(); ++i)

            setSub(m_vFeatures[x].vChild[i]);

    };

    setSub(n);

    InvalidateAggCache();
    // [CRASHFIX-UAF] 不能在此直接 RebuildList：本函数由 CClickRowUI::DoEvent
    // 的 BUTTONUP → SendNotify(CLICK) 派发栈调起，RebuildList 的 RemoveAll
    // 会删除正在派发的行控件，DoEvent 返回后基类继续解引用 this → UAF。
    // 改经 HostDefer 排到事件栈退清后执行；m_bPendingRebuild 去重，
    // 连续多次点击只排一次重建，最终状态由回调按 m_vFeatures 现值渲染。
    DeferRebuildList();
}

void CPageFeature::ToggleExpand(int n)
{
    if (!m_bFeatLoaded || n < 0 || n >= static_cast<int>(m_vFeatures.size())) return;

    // [T8 P3] 接力期间拒绝交互：展开会触发整表重建，与在途接力冲突会
    // 造成两套行序列交错挂载，先等本批渲染完。
    // [FIX-UX] 同 ToggleCheck：入队保留意图，接力收尾时补执行，不再丢弃。
    if (m_bBatchRendering)
    {
        QueuePendingIntent(m_vPendingIntents, n, true);
        return;
    }

    if (m_vFeatures[n].vChild.empty()) return;

    m_vFeatures[n].bExpanded = !m_vFeatures[n].bExpanded;

    // [CRASHFIX-UAF] 同 ToggleCheck：展开箭头也在 BUTTONUP 派发栈内调起，
    // 直接 RebuildList 会删除在途派发控件。延迟到栈退清后执行。
    DeferRebuildList();
}

// [rb26] 右侧详情面板：未选中时的引导态
void CPageFeature::ClearDetailPanel()
{
    CTextUI* pName  = static_cast<CTextUI*>(m_ctx.HostFindControl(L"feat_detail_name"));
    CTextUI* pState = static_cast<CTextUI*>(m_ctx.HostFindControl(L"feat_detail_state"));
    CTextUI* pDesc  = static_cast<CTextUI*>(m_ctx.HostFindControl(L"feat_detail_desc"));
    CTextUI* pSys   = static_cast<CTextUI*>(m_ctx.HostFindControl(L"feat_detail_sys"));
    if (!pName || !pState || !pDesc || !pSys) return;

    pName->SetText(StringMgr.Get("feat.s_detailtitle"));
    pName->SetTextColor(tokens::Token(tokens::EToken::Ink));
    pState->SetText(L"");
    pDesc->SetText(StringMgr.Get("feat.s_detailhint"));
    pDesc->SetTextColor(tokens::Token(tokens::EToken::InkMuted));
    pSys->SetText(L"");
}

// [rb26] 右侧详情面板：显示指定功能行的中文名/状态/说明/内部名
void CPageFeature::ShowDetailInPanel(int n)
{
    CTextUI* pName  = static_cast<CTextUI*>(m_ctx.HostFindControl(L"feat_detail_name"));
    CTextUI* pState = static_cast<CTextUI*>(m_ctx.HostFindControl(L"feat_detail_state"));
    CTextUI* pDesc  = static_cast<CTextUI*>(m_ctx.HostFindControl(L"feat_detail_desc"));
    CTextUI* pSys   = static_cast<CTextUI*>(m_ctx.HostFindControl(L"feat_detail_sys"));
    if (!pName || !pState || !pDesc || !pSys) return;

    if (n < 0 || n >= static_cast<int>(m_vFeatures.size())) { ClearDetailPanel(); return; }

    const FeatInfo& f = m_vFeatures[n];
    bool bVirtual = f.sSys.IsEmpty();

    pName->SetText(f.sDisp.IsEmpty() ? f.sSys.GetData() : f.sDisp.GetData());
    pName->SetTextColor(tokens::Token(tokens::EToken::Ink));

    int nAgg = Agg(n);   // 0 关 / 1 开 / 2 混合 / 3 永久移除
    DWORD dwStateClr = (nAgg == 1) ? tokens::Token(tokens::EToken::Success)
                     : (nAgg == 3) ? tokens::Token(tokens::EToken::Danger)
                     : (nAgg == 2) ? tokens::Token(tokens::EToken::Accent)
                                   : tokens::Token(tokens::EToken::InkTertiary); // [R237] 原 0xFF666666

    CDuiString sState = FeatStateText(n);

    // 虚拟分组按子项聚合，没有独立的待应用状态
    if (!bVirtual && f.nPending != -1 && f.nPending != (f.bEnabled ? 1 : 0))
    {
        const wchar_t* pszAct = (f.nPending == 1) ? StringMgr.Get("feat.s_pending_on")
                       : (f.nPending == 2) ? StringMgr.Get("feat.s_pending_remove")
                                           : StringMgr.Get("feat.s_pending_off");
        sState += L"（";
        sState += pszAct;
        sState += L"）";
    }
    pState->SetText(sState.GetData());
    pState->SetTextColor(dwStateClr);

    if (f.sDesc.IsEmpty())
    {
        pDesc->SetText(StringMgr.Get("feat.s_nodesc"));
        pDesc->SetTextColor(tokens::Token(tokens::EToken::InkMuted));
    }
    else
    {
        pDesc->SetText(f.sDesc.GetData());
        pDesc->SetTextColor(tokens::Token(tokens::EToken::InkTertiary));
    }

    if (bVirtual)
        pSys->SetText(StringMgr.Get("feat.s_groupnote"));
    else
    {
        CDuiString sSysLine;
        sSysLine.Format(StringMgr.Get("feat.s_internal_name"), f.sSys.GetData());
        pSys->SetText(sSysLine.GetData());
    }
    pSys->SetTextColor(tokens::Token(tokens::EToken::InkMuted));
}


void CPageFeature::ApplyFilterKw(const CDuiString& sKeyword, bool bNoKw)
{
    // [R11] 用缓存行指针 + 预计算小写搜索串过滤：
    // 不再逐行 Format("feat_row_%d") + HostFindControl + 两次 MakeLower。
    const size_t n = m_vFeatures.size();
    for (size_t k = 0; k < n; ++k)
    {
        CControlUI* pRow = (k < m_vFeatRowPtr.size()) ? m_vFeatRowPtr[k] : nullptr;
        if (!pRow) continue;
        if (bNoKw)
        {
            pRow->SetVisible(true);
            continue;
        }
        const CDuiString& sSearch = (k < m_vFeatRowSearch.size()) ? m_vFeatRowSearch[k] : CDuiString();
        pRow->SetVisible(sSearch.Find(sKeyword.GetData()) >= 0);
    }
}
