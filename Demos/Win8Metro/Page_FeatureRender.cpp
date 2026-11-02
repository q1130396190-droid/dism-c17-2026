#include "stdafx.h"
#include "ThemeTokens.h"   // [R201] 强调色令牌（Win10/11 均跟随系统主题色）
// ============================================================
// Page_FeatureRender.cpp —— [R91] Windows 功能页"列表渲染组"
//
// 从 Page_Feature.cpp 切出的 4 个成员函数（函数体零逻辑改动）：
//   RebuildList          建树 + 扁平化展开序列 + 启动分批接力
//   RenderFeatRow        单行渲染（箭头/三态勾选/名称）
//   RebuildListBatchStep 6ms 预算分批渲染一帧
//   FinishRebuildList    全部行挂载后的收尾（应用按钮可见性 + 详情同步）
//
// 为什么这组可以独立：
//   四者互相调用密集（RebuildList→BatchStep→RenderFeatRow→Finish），
//   但与文件其余部分只通过类成员交换数据，没有文件级 static 依赖。
//   切片外调用点：Load/Build/Empty 占位、ToggleCheck/ToggleExpand 重建、
//   OnHostTimer 接力、ApplyFilterKw，全部经类声明访问。
//
// 拆分动因：Page_Feature.cpp 拆出 Menu/Apply 两组后仍 1701 行，
//   继续超过 1500 行门槛；本组是剩余最大的内聚块（约 350 行）。
// ============================================================
#include "Page_Feature.h"
#include "CustomControls.h"      // CClickRowUI / CArrowUI（RenderFeatRow 用到）
#include "StringManager.h"
#include "DismApiBridge.h"       // PerfTraceEnabled（ttfr.feat 埋点）
#include <algorithm>            // [R91] std::sort（RebuildList 子树排序）
#include <functional>           // [R91] std::function（keyOf/flattenRow）
namespace {
// [R91] 从 Page_Feature.cpp 迁入：CArrowUI 只被 RenderFeatRow 使用，
//   随该函数一起搬，保持匿名 namespace（无外部链接需求）。
// tree expand arrow for feature rows: click handling independent of the
// CButtonUI push/capture state machine.
class CArrowUI : public CLabelUI
{
public:
    CArrowUI()
    {
        SetFixedWidth(20);
        SetFont(9);
        SetTextColor(tokens::Token(tokens::EToken::InkTertiary));
        SetAttribute(L"align", L"center");
        SetAttribute(L"valign", L"vcenter");
    }
    virtual void DoEvent(TEventUI& event)
    {
        if ((event.Type == UIEVENT_BUTTONDOWN || event.Type == UIEVENT_DBLCLICK)
            && ::PtInRect(&m_rcItem, event.ptMouse) && IsEnabled()
            && m_pManager != nullptr)
        {
            m_pManager->SendNotify(this, DUI_MSGTYPE_CLICK);
            // 不调基类：阻止事件向父行冒泡，否则行容器会把这次交互
            // 再当作行点击，展开的同时误切换勾选状态。
            return;
        }
        // 抬起事件同样吃掉：WM_LBUTTONUP 仍派发给 DOWN 时命中的箭头，
        // 默认会冒泡到父行触发一次 click。
        if (event.Type == UIEVENT_BUTTONUP)
            return;
        CLabelUI::DoEvent(event);
    }
};
} // namespace
using namespace DuiLib;
using i18n::CStringManager;
void CPageFeature::RebuildList()
{
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"prog_feat_list"));
    if (!pList) return;
    // [T8 P3] 重入保护：勾选/展开/刷新都会重进本函数。若上一轮分批接力尚未
    // 走完，必须先停掉在途定时器并复位状态，否则旧批次会把行追加到刚
    // RemoveAll 的新列表里，造成重复行与下标错位。
    ::KillTimer(m_ctx.HostHwnd(), FEAT_RENDER_BATCH_TIMER_ID);
    // [A1] 渲染起点：ttfr.feat 的计时基准（与枚举起点分开）。
    m_dwFeatRenderStartTick = ::GetTickCount();
    m_bBatchRendering = false;
    m_nRenderPos = 0;
    pList->RemoveAll();
    // [R11] 行缓存与功能下标严格对齐：重建先整体重置，避免旧指针悬挂。
    m_vFeatRowPtr.clear();
    m_vFeatRowSearch.clear();
    m_vFeatRowPtr.resize(m_vFeatures.size(), nullptr);
    m_vFeatRowSearch.resize(m_vFeatures.size());
    if (!m_bFeatLoaded)
    {
        auto pEmpty = std::make_unique<CTextUI>(); // [C++17 R3]
        pEmpty->SetText(StringMgr.Get("capability.s56824a9c"));
        pEmpty->SetFixedHeight(60);
        pEmpty->SetTextColor(tokens::Token(tokens::EToken::InkMuted));
        pEmpty->SetAttribute(L"align", L"center");
        pList->Add(pEmpty.release()); // [C++17 R3] ownership transferred to DuiLib
        ClearDetailPanel();   // [rb26] 列表不可用时复位右侧详情
        return;
    }
    // subtree sort key: smallest DISM position (virtual rows get a big token)
    std::vector<int> vKey(m_vFeatures.size(), 0);
    std::function<int(int)> keyOf = [&](int n) -> int
    {
        int k = m_vFeatures[n].sSys.IsEmpty() ? 100000 : n;
        for (size_t i = 0; i < m_vFeatures[n].vChild.size(); ++i)
        {
            int kc = keyOf(m_vFeatures[n].vChild[i]);
            if (kc < k) k = kc;
        }
        vKey[n] = k;
        return k;
    };
    std::vector<int> vRoots;
    for (size_t i = 0; i < m_vFeatures.size(); ++i)
    {
        if (m_vFeatures[i].nParent == -1)
        {
            vRoots.push_back(static_cast<int>(i));
            keyOf(static_cast<int>(i));
        }
    }
    std::sort(vRoots.begin(), vRoots.end(),
        [&](int a, int b) { return vKey[a] < vKey[b]; });
    // [T8 P3] 扁平化展开：把"当前可见行"（受 bExpanded 控制）压成
    // (功能下标, 缩进层级) 两个平行序列，供分批渲染逐段消费。
    // 原实现用 renderRow 递归一次性建完全部行，功能项多时单帧 >50ms
    // 造成可感知冻结；改为 50 行/批 + 15ms 接力（对齐 Upd 页先例）。
    m_vRenderRows.clear();
    m_vRenderDepths.clear();
    std::function<void(int,int)> flattenRow = [&](int n, int depth)
    {
        m_vRenderRows.push_back(n);
        m_vRenderDepths.push_back(depth);
        if (!m_vFeatures[n].bExpanded) return;
        for (size_t i = 0; i < m_vFeatures[n].vChild.size(); ++i)
            flattenRow(m_vFeatures[n].vChild[i], depth + 1);
    };
    for (size_t i = 0; i < vRoots.size(); ++i)
        flattenRow(vRoots[i], 0);
    // 首批同步渲染保证首屏不空白，其余交给 15ms 定时器接力
    m_nRenderPos = 0;
    m_bBatchRendering = true;
    RebuildListBatchStep();
}
// [T8 P3] 单行渲染：原 RebuildList 内 renderRow lambda 逐句搬迁，
// 仅去掉对 pList 的隐式捕获与对子行的递归（递归改由扁平序列驱动）。
void CPageFeature::RenderFeatRow(CVerticalLayoutUI* pList, int n, int depth)
{
    // [STAB] 分批渲染横跨多个定时器周期，期间 m_vFeatures 可能被
    // ApplyRawData（枚举结果到达 / 缓存刷新）整体重建为更短的列表，
    // 此时扁平序列里的旧下标会越界。这里必须防御，不能假定序列仍有效。
    if (n < 0 || n >= static_cast<int>(m_vFeatures.size())) return;
    FeatInfo& f = m_vFeatures[n];
        bool bSel = (n == m_nFeatSelected);   // [rb26] 选中行高亮，联动右侧详情面板
        auto pRow = std::make_unique<CClickRowUI>(); // [C++17 R3]
        pRow->SetFixedHeight(30);
        pRow->SetBkColor(bSel ? tokens::Token(tokens::EToken::Accent)
                             : tokens::Token(tokens::EToken::Surface));
        RECT rcInset = { 8 + depth * 18, 0, 8, 0 };
        pRow->SetInset(rcInset);
        pRow->SetChildPadding(0);
        CDuiString sRowName;
        sRowName.Format(L"feat_row_%d", n);
        pRow->SetName(sRowName.GetData());
        // expand arrow (separate hit area) or fixed spacer
        if (!f.vChild.empty())
        {
            auto pArr = std::make_unique<CArrowUI>(); // [C++17 R3]
            pArr->SetText(f.bExpanded ? m_ctx.HostGlyphArrow(1)
                                      : m_ctx.HostGlyphArrow(0));
            if (bSel) pArr->SetTextColor(tokens::Token(tokens::EToken::OnAccent));   // 蓝底上箭头转白
            CDuiString sArName;
            sArName.Format(L"feat_arrow_%d", n);
            pArr->SetName(sArName.GetData());
            pRow->Add(pArr.release()); // [C++17 R3] ownership transferred to DuiLib
        }
        else
        {
            auto pSp = std::make_unique<CControlUI>(); // [C++17 R3]
            pSp->SetFixedWidth(20);
            pRow->Add(pSp.release()); // [C++17 R3] ownership transferred to DuiLib
        }
        // tri-state checkbox glyph
        int nAgg = Agg(n);
        // [修复 R5 / P1-13] 新增第 4 态：3=永久移除（红色 ✕，与图例一致）
        const wchar_t* pszGlyph = (nAgg == 1) ? L"\u2611"
                         : (nAgg == 2) ? L"\u25A0"
                         : (nAgg == 3) ? L"\u2715"
                                       : L"\u2610";
        auto pChk = std::make_unique<CTextUI>(); // [C++17 R3]
        pChk->SetFixedWidth(24);
        pChk->SetFont(9);
        pChk->SetText(pszGlyph);
        // on / partial share the switch blue; remove uses warning red; off neutral dark.
        // [rb26] 选中行为蓝底，勾选符号统一转白，具体状态见右侧详情面板。
        DWORD dwChkColor = bSel ? tokens::Token(tokens::EToken::OnAccent)
                         : (nAgg == 3) ? tokens::Token(tokens::EToken::Danger)
                         : (nAgg == 0) ? tokens::Token(tokens::EToken::Ink)
                                       : tokens::Token(tokens::EToken::Accent);
        pChk->SetTextColor(dwChkColor);
        pChk->SetAttribute(L"valign", L"vcenter");
        pChk->SetMouseEnabled(false);
        pRow->Add(pChk.release()); // [C++17 R3] ownership transferred to DuiLib
        auto pName = std::make_unique<CTextUI>(); // [C++17 R3]
        pName->SetFixedWidth(0);
        pName->SetText(f.sDisp.GetData());
        pName->SetTextColor(bSel ? tokens::Token(tokens::EToken::OnAccent)
                                 : tokens::Token(tokens::EToken::Ink));   // 蓝底选中行文字转白
        pName->SetAttribute(L"valign", L"vcenter");
        pName->SetAttribute(L"endellipsis", L"true");
        pName->SetMouseEnabled(false);
        pRow->Add(pName.release()); // [C++17 R3] ownership transferred to DuiLib
    // [R220] Per-row uninstall button. The business path already exists
    //   (CPageFeature::DeleteSelected -> FeatApplyItem{nOp=2} -> /remove ->
    //   DismDisableFeature), but the only entries were btn_prog_delete and the
    //   row context menu, both acting on the *selected* row. The design reference
    //   shows an action button at the right edge of every row, so add one.
    //   Width 84x30 follows the sibling capun_* buttons (Page_CapabilityBuild.cpp);
    //   the 104 width belongs to page-level primary actions, not in-row buttons.
    {
        auto pUn = std::make_unique<CButtonUI>(); // [C++17 R3]
        CDuiString sUnName;
        sUnName.Format(L"featun_%d", n);
        pUn->SetName(sUnName.GetData());
        pUn->SetText(StringMgr.Get("feature.s_uninstall_btn"));
        pUn->SetFixedWidth(84);
        pUn->SetFixedHeight(30);
        pUn->SetFont(1);
        pUn->SetTextColor(bSel ? tokens::Token(tokens::EToken::OnAccent)
                                 : tokens::Token(tokens::EToken::Ink));
        pUn->SetBkColor(bSel ? tokens::Token(tokens::EToken::AccentHot)
                             : tokens::Token(tokens::EToken::Surface));
        // [R244] 这里原来写的是一个字符串字面量形式的令牌占位符。
        //   占位符只在皮肤 XML 展开阶段由 FrameWnd::ExpandSkinFile 替换，
        //   代码里 SetAttribute 传入的字符串不会经过那条路径 ——
        //   于是这个值被当成一个非法颜色串，边框取到默认色，
        //   深色模式下与背景几乎同色，按钮边界直接消失。
        pUn->SetAttribute(L"bordercolor", tokens::TokenHex(tokens::EToken::Border));
        pUn->SetAttribute(L"bordersize", L"1");
        pUn->SetAttribute(L"borderround", L"4,4");
        pUn->SetAttribute(L"align", L"center");
        pUn->SetAttribute(L"valign", L"vcenter");
        pRow->Add(pUn.release()); // [C++17 R3] ownership transferred to DuiLib
    }
        pList->Add(pRow.release()); // [C++17 R3] ownership transferred to DuiLib
        // [R11] 记录景点行指针与预计算搜索串（名称 + 系统名，均已小写）。
        // 过滤时直接遍历缓存，不再逐行 Format + HostFindControl + MakeLower。
        if (n >= 0 && static_cast<size_t>(n) < m_vFeatRowPtr.size())
        {
            m_vFeatRowPtr[static_cast<size_t>(n)] = pList->GetItemAt(pList->GetCount() - 1);
            CDuiString sSearch = f.sDisp;
            if (!f.sSys.IsEmpty())
            {
                sSearch += L' ';
                sSearch += f.sSys;
            }
            sSearch.MakeLower();
            m_vFeatRowSearch[static_cast<size_t>(n)] = sSearch;
        }
}
// [FIX-PERF] 时间预算分批渲染：每帧约 6ms，避免数百行一次性建控件。
// 快照 m_vRenderRows 在 RebuildList 时建立，批次间不再被 ApplyRawData 改写，
// 因此恢复分批不会重现旧的“快照失效、列表半截”问题。
// [R175] 从 RebuildListBatchStep 抽出：[A1] 首行 TTFR 采样（只在首帧打一次）。
//   口径 = 渲染起点 → 第一行控件创建完成，不含 DISM 枚举等待（那段记 acquire.*）。
//   bLogged 由调用方持有（thread_local static）：跨帧状态不能放本函数内 static，
//   否则每帧都从 false 开始，等于每帧都打一次。
//   抽出的目的：把 if(条件){ if(有起点) } 两层压成一行调用，
//   让 RebuildListBatchStep 的嵌套从 4 降到 3。
void CPageFeature::FeatLogTtfrOnce(bool& bLogged, int nTotal)
{
    if (bLogged) return;
    if (!DismApiBridge::PerfTraceEnabled()) return;
    bLogged = true;
    if (!m_dwFeatRenderStartTick) return;

    const DWORD dwTtfr = ::GetTickCount() - m_dwFeatRenderStartTick;
    CDuiString sMsg;
    sMsg.Format(L"[perf] ttfr.feat = %lu ms (rows=%d)", dwTtfr, nTotal);
    ::OutputDebugStringW(sMsg.GetData());
    ::OutputDebugStringW(L"\r\n");
}

void CPageFeature::RebuildListBatchStep()
{
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"prog_feat_list"));
    if (!pList || m_vRenderRows.size() != m_vRenderDepths.size())
    {
        ::KillTimer(m_ctx.HostHwnd(), FEAT_RENDER_BATCH_TIMER_ID);
        m_bBatchRendering = false;
        return;
    }
    const size_t nTotal = m_vRenderRows.size();
    const ULONGLONG uStart = ::GetTickCount64();
    const ULONGLONG uBudgetMs = 6;
    // [O7 2026-10-25] 视口优先首屏（Feature 页）。
    //   与 Capability 不同：本页是树形，一行的高度含缩进与父子关系，
    //   不能简单按"前 40 行"切。这里按"可视区能容纳的根级行数"取 20 行——
    //   树形下 20 行足以填满首屏（含展开的父行），继续切分只会把 TTFR 拖长。
    //   超出的行仍在屏幕外，晚几十毫秒建出来用户无感。
    constexpr size_t kFirstScreenRows = 20;
    const bool bFirstScreen = (m_nRenderPos == 0);
    // [A1] ttfr 只在首帧打一次，避免接力帧重复输出污染统计。
    static thread_local bool s_bTtfrLogged = false;
    if (bFirstScreen) s_bTtfrLogged = false;
    for (size_t k = m_nRenderPos; k < nTotal; ++k)
    {
        const int nIdx = m_vRenderRows[k];
        if (nIdx < 0 || nIdx >= static_cast<int>(m_vFeatures.size()))
        {
            ::KillTimer(m_ctx.HostHwnd(), FEAT_RENDER_BATCH_TIMER_ID);
            m_bBatchRendering = false;
            if (!m_vFeatures.empty()) RebuildList();
            return;
        }
        RenderFeatRow(pList, nIdx, m_vRenderDepths[k]);
        m_nRenderPos = k + 1;
        // [A1] 首行可见（TTFR）：抽为 FeatLogTtfrOnce，嵌套 4→3
        if (m_nRenderPos == 1) FeatLogTtfrOnce(s_bTtfrLogged, static_cast<int>(nTotal));
        // [O7] 首屏 20 行内不切分；超出后恢复 6ms 预算。
        const bool bInFirstScreen = bFirstScreen && (m_nRenderPos <= kFirstScreenRows);
        if (!bInFirstScreen
            && ::GetTickCount64() - uStart >= uBudgetMs && m_nRenderPos < nTotal)
        {
            ::SetTimer(m_ctx.HostHwnd(), FEAT_RENDER_BATCH_TIMER_ID, 1, nullptr);
            return;
        }
    }
    ::KillTimer(m_ctx.HostHwnd(), FEAT_RENDER_BATCH_TIMER_ID);
    m_bBatchRendering = false;
    FinishRebuildList();
}
// [T8 P3] 接力收尾：全部行挂载后再执行"整表相关"的副作用
// （应用按钮可见性 + 右侧详情面板同步）。原实现这两步在渲染循环之后，
// 分批后必须等最后一批落盘才能做，否则会按不完整列表计算。
void CPageFeature::FinishRebuildList()
{
    bool bAny = false;
    for (size_t i = 0; i < m_vFeatures.size(); ++i)
    {
        const FeatInfo& f = m_vFeatures[i];
        if (!f.sSys.IsEmpty() && f.nPending != -1
            && f.nPending != (f.bEnabled ? 1 : 0)) { bAny = true; break; }
    }
    CButtonUI* pApply = static_cast<CButtonUI*>(m_ctx.HostFindControl(L"btn_prog_apply"));
    if (pApply) pApply->SetVisible(bAny);
    // [rb26] 重建后同步右侧详情面板（勾选/展开都会 RebuildList，
    // m_nFeatSelected 不变时详情内容也随之刷新 pending 状态）
    if (m_nFeatSelected >= 0 && m_nFeatSelected < static_cast<int>(m_vFeatures.size()))
        ShowDetailInPanel(m_nFeatSelected);
    else
        ClearDetailPanel();
    // [FIX-UX] 补执行渲染接力期间被暂存的点击意图：旧实现直接丢弃，
    //   用户连点表现为"点了没反应"。此处按记录顺序重放（此时
    //   m_bBatchRendering 已复位，ToggleCheck/ToggleExpand 走正常路径，
    //   多个意图经 DeferRebuildList 去重后只重建一次）。
    if (!m_vPendingIntents.empty())
    {
        std::vector<std::pair<int, bool>> vReplay;
        vReplay.swap(m_vPendingIntents);
        for (const auto& it : vReplay)
        {
            if (it.second) ToggleExpand(it.first);
            else           ToggleCheck(it.first);
        }
    }
}

// [R233] Re-colour the feature rows after a light<->dark switch.
//
// PageBase.h contract: colours only -- no Build()/Add/Remove. A rebuild would re-run
// the batched renderer and lose the expansion and checked state.
//
// Row layout (RenderFeatRow): 0 = arrow or spacer, 1 = tri-state checkbox glyph,
// 2 = name (elastic), 3 = uninstall button.
// Selection is not stored on the row here; it lives in m_nFeatSelected, so the row
// index must be recovered from the cache position.
void CPageFeature::RefreshThemeColors()
{
    const DWORD dwInk   = tokens::Token(tokens::EToken::Ink);
    const DWORD dwMuted = tokens::Token(tokens::EToken::InkMuted);
    const DWORD dwBase  = tokens::Token(tokens::EToken::Surface);
    const DWORD dwSel   = tokens::Token(tokens::EToken::Accent);
    const DWORD dwOnSel = tokens::Token(tokens::EToken::OnAccent);

    for (size_t k = 0; k < m_vFeatRowPtr.size(); ++k)
    {
        CControlUI* pRowCtl = m_vFeatRowPtr[k];
        if (!pRowCtl) continue;
        const bool bSel = (static_cast<int>(k) == m_nFeatSelected);

        if (CClickRowUI* pRowRow = dynamic_cast<CClickRowUI*>(pRowCtl))
            pRowRow->SetRowSelected(bSel);
        pRowCtl->SetBkColor(bSel ? dwSel : dwBase);

        IContainerUI* pRow = static_cast<IContainerUI*>(
            pRowCtl->GetInterface(DUI_CTR_CONTAINER));
        if (!pRow) continue;
        for (int c = 0; c < pRow->GetCount(); ++c)
        {
            CControlUI* pCell = pRow->GetItemAt(c);
            if (!pCell) continue;

            if (CButtonUI* pBtn = dynamic_cast<CButtonUI*>(pCell))
            {
                pBtn->SetTextColor(bSel ? dwOnSel : dwInk);
                pBtn->SetBkColor(bSel ? tokens::Token(tokens::EToken::AccentHot) : dwBase);
                // [R244] 边框色同样要刷。Build 里设了，刷新漏了它；
                //   深色下边框与背景几乎同色，按钮边界会整体消失。
                pBtn->SetAttribute(L"bordercolor",
                                   tokens::TokenHex(tokens::EToken::Border));
                continue;
            }
            if (CTextUI* pText = dynamic_cast<CTextUI*>(pCell))
            {
                // 0 = arrow, 1 = tri-state glyph, 2 = name.
                if (c == 2)      pText->SetTextColor(bSel ? dwOnSel : dwInk);
                else if (c == 0) pText->SetTextColor(dwMuted);
                else             pText->SetTextColor(bSel ? dwOnSel : dwInk);
            }
        }
    }
}
