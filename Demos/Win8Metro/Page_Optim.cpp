#include "stdafx.h"

#include "Page_Optim.h"
#include "Page_OptimInternal.h"   // [R134-1] FindOptimItem（跨 TU 共享）

#include "StringManager.h"
#include "NotifyBox.h"          // [R143] ToolSql::HostNotify / HostConfirm（页面统一弹框入口）

#include "FrameWnd_Util.h"          // StyleCheckBox / RecalcBodyHeight / CopyTextToClipboard
#include "FrameWnd_UiHelpers.h"    // P8：ParseSwName

#include "CustomControls.h"        // CClickRowUI / CHoverRowUI

#include "Win32DialogService.h"  // DlgSaveFile

#include "LogService.h"            // DetectLevel (same auto-leveling as old AppendLog)

#include <string>

#include <vector>
#include "WinHandles.h"          // [C++17 R3] win::HandlePtr RAII file handle (also brings <memory>)
#include "ExternalToolService.h"  // [Privilege] IsElevated：判断是否已提权

using i18n::CStringManager;   // convenience: write StringMgr without prefix

// ============================================================
// File-local helpers (mechanically moved from FrameWnd.cpp in P7;
// zero logic changes). FindOptimItem/CollectWarningTexts/ParseSwName
// still exist as shell file-statics because remaining shell code uses
// them (ImportConfig / Notify group-bulk / WM_CONTEXTMENU); the page
// keeps its own copies per the P5 Page_Driver/ParsePrefix2 precedent.
// ============================================================
namespace
{
using optim_detail::FindOptimItem;
using optim_detail::ItemIsApplicable;
using optim_detail::ContainsCI;
using optim_detail::CollectWarningTexts;
using optim_detail::OptimItemShown;
using optim_detail::OptimGroupMatch;
// P8：ParseSwName 已上移 FrameWnd_UiHelpers（多页共享），本页经头文件使用。
// Tri-state group checkbox glyph (copy of former CFrameWnd::SetGroupGlyph,
    // migrated with the optimize page in P7; glyphs now read via host).
    // Page_Cleanup.cpp holds its own independent copy: 0 empty / 1 half / 2 all.
    void SetGroupGlyph(IPageContext& ctx, CCheckBoxUI* p, int nState)
    {
        if (!p) return;
        int idx = (nState >= 0 && nState <= 2) ? nState : 0;
        // [BOX-CHECK] 自绘复选框：三态落到控件上（方框/填充/标记由控件自绘），
        //   不再改写文字里的字形前缀 —— 字形方案在选中态是实心块，边框会消失。
        if (auto pBox = dynamic_cast<CBoxCheckUI*>(p))
        {
            pBox->SetBoxState(idx);
            return;
        }
        // strip the old leading glyph (1 char) + double space, keep group name
        CDuiString strText = p->GetText();
        if (strText.GetLength() >= 3)
        {
            wchar_t cFirst = strText[0];
            if (cFirst == ctx.HostGlyphBox(0)[0]
                || cFirst == ctx.HostGlyphBox(1)[0]
                || (ctx.HostGlyphBox(2)[0] != 0 && cFirst == ctx.HostGlyphBox(2)[0]))
                strText = strText.Mid(3);
        }
        CDuiString strShow = ctx.HostGlyphBox(idx);
        strShow += L"  ";
        strShow += strText;
        p->SetText(strShow.GetData());
    }
// [A-2] 未指定具体列时的作用域描述。
//
// 旧实现固定返回 misc.h8049（"全部作用域"）。取消「新用户」列后页面只剩
// 「当前用户 / 系统」两列，用户点整行（而非某个开关）看属性时，"全部作用域"
// 会让人以为还存在看不见的第三列——而那一列已不再写入。
//
// 不新增语言键：复用 11 个语言包都齐全的表头键（skin.hdr_op_cur / sys），
// 按 optim_scope::kColumns 顺序拼接。将来增删列时文案自动跟随。
}   // namespace

// ============================================================

CPageOptim::CPageOptim(IPageContext& ctx)

    : CPageBase(ctx)

    , m_nOptimStateFilter(0)

    , m_dwOptimVerifyTick(0)

    , m_pOptimProgress(nullptr)

    , m_dwLastOptimMs(0)

    , m_nOptimPendingVal(0)

    , m_nOptimPendingMax(0)

    , m_bFiltering(false)

{
}

void CPageOptim::AppendLog(const wchar_t* pszLine)

{
    m_ctx.HostLog(pszLine, LogService::DetectLevel(pszLine));
}

// Grab the XML-built optim_progress once (verbatim body of the former
// CFrameWnd::InitSessionProgress block, routed here in P7); a missing
// control makes SetOptimProgress fall back to HostProgress.
void CPageOptim::InitProgress()

{
    m_pOptimProgress = static_cast<CProgressUI*>(m_ctx.HostFindControl(L"optim_progress"));
    if (m_pOptimProgress)
    {
        m_pOptimProgress->SetHorizontal(true);
        m_pOptimProgress->SetShowText(false);
        m_pOptimProgress->SetVisible(false);
    }
}

// [R2 P1] 优化规则执行进度。XML 有 optim_progress 就用它（页面原位进度），
// 否则回退到会话条动态进度条 + 任务栏进度。
void CPageOptim::SetOptimProgress(int nVal, int nMax)
{
    if (m_pOptimProgress)
    {
        if (nMax <= 0)
        {
            m_pOptimProgress->SetVisible(false);
        }
        else
        {
            if (nVal < 0) nVal = 0;
            if (nVal > nMax) nVal = nMax;
            m_pOptimProgress->SetMaxValue(nMax);
            m_pOptimProgress->SetValue(nVal);
            m_pOptimProgress->SetVisible(true);
        }
    }
    else
    {
        m_ctx.HostProgress(nVal, nMax);
    }
}

// 100ms 节流：批量执行规则时避免每条规则都触发 UI 重绘；
// nMax<=0（隐藏）与终态（nVal==nMax）立即刷新不等待。
void CPageOptim::ThrottleProgress(DWORD& dwLastMsRef, int nVal, int nMax)
{
    m_nOptimPendingVal = nVal;
    m_nOptimPendingMax = nMax;
    DWORD dwNow = ::GetTickCount();
    if (nMax <= 0 || nVal == nMax || (dwNow - dwLastMsRef) >= 100)
    {
        SetOptimProgress(nVal, nMax);
        dwLastMsRef = dwNow;
    }
}

// [R4] 加速键命令收口（原壳 OnAccelCommand 中 Ctrl+Z 优化页直调）
bool CPageOptim::OnHostCommand(int id)
{
    switch (id)
    {
    case EAccelCmd::ID_ACCEL_UNDO:
        UndoLastOptim();
        return true;
    // [R5] 壳内三处 VerifyOptimStates 编排 + InitSessionProgress 收口
    case EHostCmd::HCMD_OPTIM_VERIFY_FORCE:
        VerifyOptimStates(true);    // InitWindow/ImportConfig/RefreshAll
        return true;
    case EHostCmd::HCMD_OPTIM_VERIFY_SOFT:
        VerifyOptimStates(false);   // 切页延迟构建后（受 5 秒 TTL 约束）
        return true;
    case EHostCmd::HCMD_OPTIM_INIT_PROGRESS:
        InitProgress();
        return true;
    default:
        return false;
    }
}

// [R176] 从 Build（71 行 / CC13）抽出：单个组内的条目建行。
// bGroupApplicable 为假时整个循环不执行（组不适用 → 一条都不建，避免残留空行）；
// nApplicable 回传给调用方做组头计数。
// O-1：含无法执行动作的条目在显示名后追加标记，同时以 bUnsupported 传给建行函数置橙。
void CPageOptim::OptimBuildGroupItems(CMarkupNode group, CVerticalLayoutUI* pBody,
                                      int g, bool bGroupApplicable, int& nApplicable)
{
    int i = 0;
    CMarkupNode item = group.GetChild(L"Item");
    // [OS-Filter] 组不适用时跳过整个条目循环：既省去无谓的
    // ResolveText/建行开销，也避免创建一批立即被丢弃的隐藏控件。
    while (bGroupApplicable && item.IsValid())
    {
        CDuiString strItemName = m_ctx.DataEngine().ResolveText(item.GetAttributeValue(L"Name"));
        const bool bCombo = lstrcmpiW(item.GetAttributeValue(L"Type"), L"Combo") == 0;
        // [OS-Filter] 组不适用时整组跳过，条目一律不建（避免残留空行）
        const bool bApplicable = bGroupApplicable
            && ItemIsApplicable(m_ctx.DataEngine(), item);
        if (bApplicable) ++nApplicable;
        // O-1: 含无法执行动作的条目。
        // [FIX 2026-10-25] 原实现用 `名称 + StringMgr.Get("error.s7ee035a6")` 拼接标注，
        //   但该键在 StringManager 与 lang/ 中都不存在，Get 返回空串 ——
        //   屏上留下的是一对孤立括号（如"某规则（）"），比不标注更糟。
        //   按用户要求：括弧及其内容一并去掉，仅保留 bUnsupported 供行样式使用。
        const bool bUnsupported = m_ctx.DataEngine().ItemHasUnsupportedAction(item);
        const CDuiString strDisplay = strItemName;
        if (!bCombo)
            OptimBuildSwitchRow(pBody, item, strDisplay, g, i, bApplicable, bUnsupported);
        else
            OptimBuildComboRow(pBody, item, strDisplay, g, i, bApplicable, bUnsupported);
        ++i;
        item = item.GetSibling();
    }
}

void CPageOptim::Build()
{
    CMarkupNode root = m_ctx.DataEngine().GetOptimizationRoot();
    if (!root.IsValid()) return;
    CVerticalLayoutUI* pTree = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"optim_tree"));
    if (!pTree) return;
    pTree->RemoveAll();
    // O-5: 重建树后复位状态快捷过滤
    OptimBuildResetFilter();
    // 统计 Group 子节点数，初始化折叠态
    OptimBuildCountGroups(root);
    int g = 0;
    CMarkupNode group = root.GetChild(L"Group");
    while (group.IsValid())
    {
        // [OS-Filter] 组级适用性：XML 中 7 个分组带 <Group><Applicable>，
        // 用于整组只在特定系统出现（如 #Windows 7体验 仅 Win7、
        // Internet Explorer 仅旧版、#Microsoft Edge 依赖 FileExist）。
        // 组不适用则整组不建树——这是"旧版本看不到新功能"的必要条件，
        // 仅靠条目级判定会留下空组壳。
        CMarkupNode grpApp = group.GetChild(L"Applicable");
        const bool bGroupApplicable = !grpApp.IsValid()
            || m_ctx.DataEngine().EvalConditions(grpApp);
        CDuiString strGroupName = m_ctx.DataEngine().ResolveText(group.GetAttributeValue(L"Name"));
        // ---- group row ----
        CClickRowUI* pRow = bGroupApplicable ? OptimBuildGroupRow(strGroupName, g) : nullptr;
        // ---- body (item rows) ----
        auto pBody = std::make_unique<CVerticalLayoutUI>(); // [C++17 R3]
        CDuiString sBodyName;
        sBodyName.Format(L"optbody_%d", g);
        pBody->SetName(sBodyName.GetData());
        pBody->SetChildPadding(0);
        int nApplicable = 0;
        // [R176] 组内条目建行抽为 OptimBuildGroupItems（原 while 内联，CC 记账 6 个判定点）
        OptimBuildGroupItems(group, pBody.get(), g, bGroupApplicable, nApplicable);
        // [OS-Filter] 组不适用：整组（标题行 + body）都不加入界面。
        // 仍递增 g 以保持 optbody_%d / optgrp_%d 的编号与 XML 中的
        // Group 序号一一对应——否则后续组的控件名会与索引错位，
        // 导致 GetOptimizationRoot 的按序查找取错分组。
        if (!bGroupApplicable)
        {
            ++g;
            group = group.GetSibling();
            continue;
        }
        OptimBuildFinishGroup(pTree, pRow, pBody.release(), nApplicable,
                              g < static_cast<int>(m_vExpanded.size()) && m_vExpanded[g]); // [FIX-UX] 首组默认展开
        ++g;
        group = group.GetSibling();
    }

    // [OPTIM-BOX] 树是刚重建出来的：新的组合框/开关都没有任何状态。
    //   必须让紧随其后的那次核实真的跑一次 —— 核实有 5 秒 TTL，
    //   而"切页延迟构建"通常就落在启动那次核实之后的 5 秒窗口内，
    //   于是软核实被 TTL 直接短路：组合框永远没有选中项、也没有文字，
    //   界面上就是一排空的带边框方框。这里把 TTL 缓存作废即可。
    m_dwOptimVerifyTick = 0;
}


void CPageOptim::SyncOptimGroupGlyph(int g)
{
    // 定位第 g 组后委托给重载实现
    CMarkupNode root = m_ctx.DataEngine().GetOptimizationRoot();
    CMarkupNode gNode = root.GetChild(L"Group");
    for (int gi = 0; gNode.IsValid() && gi < g; ++gi)
        gNode = gNode.GetSibling();
    SyncOptimGroupGlyph(g, gNode);
}

// [PERF] 批量刷新用：调用方已持有该组节点，避免每组都从根重扫。
// 原批量路径（VerifyOptimStates）对 25 个组各调一次单参版本，
// 合计约 25×12.5 ≈ 312 次兄弟节点遍历；改为单趟推进后降为 25 次。
// [R169] 从 SyncOptimGroupGlyph 抽出：统计单个条目在各可见作用域上的开关数。
// 计数口径逐字迁移：
//  · 以数据为准，不依赖 IsVisible()（折叠会让子控件 m_bInternVisible=false，
//    使折叠组三态恒为空框 —— 修复 TRISTATE-2）；
//  · 但必须复用 OptimItemShown 的 bShown：分组三态本质是"点这个组复选框会发生
//    什么"的预览，与组三态的统计范围必须一致（FILTER-SSOT）——
//    否则筛选状态下三态显示全选、点下去却只改筛选内的部分项；
//  · 控件不存在或该作用域节点无效时都不计数（两个 continue 保持原顺序）。
void CPageOptim::SyncCountItemStates(int g, int ii, CMarkupNode item, bool bShown,
                                     int& nOn, int& nTotal)
{
    for (int sc : optim_scope::kColumns)
    {
        CMarkupNode box = item.GetChild(m_ctx.DataEngine().ScopeName(sc));
        if (!box.IsValid()) continue;
        CDuiString sSw;
        sSw.Format(L"sw_%d_%d_%d", g, ii, sc);
        CSwitchUI* pSw = static_cast<CSwitchUI*>(m_ctx.HostFindControl(sSw));
        if (!pSw) continue;
        if (!bShown) continue;
        ++nTotal;
        if (m_ctx.DataEngine().GetScopeState(item, sc) == 1) ++nOn;
    }
}

void CPageOptim::SyncOptimGroupGlyph(int g, CMarkupNode gNode)
{
    // [组头复选框已移除] 三态字形是给组复选框（optgrp_ck_%d）用的。
    //   组头不再有复选框 → 控件不存在时直接返回，顺带省掉一次全组开关
    //   状态统计（每项都要读注册表）。
    CDuiString sCkName;
    sCkName.Format(L"optgrp_ck_%d", g);
    CCheckBoxUI* pCk = static_cast<CCheckBoxUI*>(m_ctx.HostFindControl(sCkName));
    if (!pCk) return;

    // 统计本组所有可见项的开关状态（界面可见列）—— 直接用 XML 节点遍历，避免 DuiLib 容器 API 差异
    int nOn = 0, nTotal = 0;
    if (!gNode.IsValid()) return;
    const bool bGroupMatch = OptimGroupMatch(m_ctx, gNode, m_sOptimFindText);
    CMarkupNode item = gNode.GetChild(L"Item");
    int ii = 0;
    while (item.IsValid())
    {
        const bool bShown = OptimItemShown(m_ctx, item, m_sOptimFindText,
                                           m_nOptimStateFilter, bGroupMatch);
        // [R169] 单项计数抽为 SyncCountItemStates（原内层 for + 3 个 continue 判定）
        SyncCountItemStates(g, ii, item, bShown, nOn, nTotal);
        ++ii;
        item = item.GetSibling();
    }
    // 计算三态
    int nState = 0; // 空（nTotal==0 时必须保持空框：组内无任何可操作项）
    if (nTotal > 0 && nOn == nTotal) nState = 2; // 全选
    else if (nOn > 0) nState = 1; // 半选
    {
        // [修复 #8] 三态字形之外还要同步内部勾选态：半选/空态必须 false，
        // 否则用户点击时 Activate 基于残留 checked 翻转，目标态会算反。
        pCk->SetCheck(nState == 2);
        // [修复 TRISTATE-1] 全局字形表(m_sGlyphBox)语义为 0空/1全选(☑)/2半选(▣)，
        // 与本页 nState 的 0空/1半选/2全选 在 1/2 上恰好相反。直接传 nState 会把
        // 半选渲染成 ☑（用户看到的“多选复显错误”），全选渲染成 ▣。此处做映射。
        int nGlyphIdx = (nState == 1) ? 2 : (nState == 2 ? 1 : 0);
        SetGroupGlyph(m_ctx, pCk, nGlyphIdx);
    }
}

// [G9 2026-10-25] 展开/折叠全部组。
//   原实现所有组默认折叠且没有一次性展开入口：浏览 333 项要逐组点开，
//   想整体扫一遍几乎不可行。这里复用 ToggleGroup 的可见性语义，
//   但直接按目标态设置而非逐组取反（避免部分展开时结果不确定）。
void CPageOptim::SetAllGroupsExpanded(bool bExpand)
{
    const int nGroups = static_cast<int>(m_vExpanded.size());
    if (nGroups <= 0) return;
    CVerticalLayoutUI* pTree = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"optim_tree"));
    for (int g = 0; g < nGroups; ++g)
    {
        CDuiString sBodyName; sBodyName.Format(L"optbody_%d", g);
        CVerticalLayoutUI* pBody = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(sBodyName.GetData()));
        if (!pBody) continue;
        bool bShow = bExpand;
        // 与 ToggleGroup 同一条约束：高度为 0 的组体不允许展开
        if (bShow && pBody->GetFixedHeight() <= 0) bShow = false;
        pBody->SetVisible(bShow);
        m_vExpanded[g] = bShow;
        CDuiString sArrowName; sArrowName.Format(L"optarrow_%d", g);
        if (CTextUI* pArrow = static_cast<CTextUI*>(m_ctx.HostFindControl(sArrowName.GetData())))
            pArrow->SetText(m_ctx.HostGlyphArrow(bShow ? 1 : 0));
    }
    if (pTree) { pTree->SetPos(pTree->GetPos()); pTree->NeedUpdate(); }
    CDuiString sLog;
    sLog.Format(L"[optim] %s all %d group(s)",
                bExpand ? L"expand" : L"collapse", nGroups);
    AppendLog(sLog.GetData());
}

void CPageOptim::ToggleGroup(int g)

{
    CDuiString sBodyName;
    sBodyName.Format(L"optbody_%d", g);
    CVerticalLayoutUI* pBody = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(sBodyName.GetData()));
    CDuiString sArrowName;
    sArrowName.Format(L"optarrow_%d", g);
    CTextUI* pArrow = static_cast<CTextUI*>(m_ctx.HostFindControl(sArrowName.GetData()));
    if (!pBody) return;
    bool bShow = !pBody->IsVisible();
    // [B90 智能加载] 组体当前高度为 0（所有项被 OS 门控/过滤隐藏）时不允许展开：
    // 可见但高度 0 的 VerticalLayout 会被内核当作自适应填充项吞掉列表空间
    if (bShow && pBody->GetFixedHeight() <= 0) bShow = false;
    pBody->SetVisible(bShow);
    if (g >= 0 && g < static_cast<int>(m_vExpanded.size())) m_vExpanded[g] = bShow;
    if (pArrow) pArrow->SetText(m_ctx.HostGlyphArrow(bShow ? 1 : 0));
    // explicit relayout: visibility toggle must reflow the tree immediately
    CVerticalLayoutUI* pTree = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"optim_tree"));
    if (pTree)
    {
        pTree->SetPos(pTree->GetPos());
        if (pTree->GetParent()) pTree->GetParent()->SetPos(pTree->GetParent()->GetPos());
    }
    // O-2: 展开/折叠后同步分组三态（可见项变化可能影响统计）
    SyncOptimGroupGlyph(g);
}

// ============================================================
// [R12-01] ApplyFindFilter 深度拆解辅助（d7->d<=5）
// 仅提取状态过滤判定，逐字搬迁原逻辑，零行为改动。
// ============================================================

// [FILTER-SSOT] 单条目可见性：直接委托 OptimItemShown（界面与写入共用同一判定）。
// 返回 bShow；副作用：聚合 bAnyItem / bGroupApplicable（供组级显隐判定）。
static bool OptimItemVisible(IPageContext& ctx, CMarkupNode item, int nStateFilter,
                             const CDuiString& strText, bool bReset, bool bHasKw,
                             bool bGroupMatch, bool& bAnyItem, bool& bGroupApplicable)
{
    bool bApplicable = ItemIsApplicable(ctx.DataEngine(), item);
    if (bApplicable) bGroupApplicable = true;
    if (bReset) return !!bApplicable;
    bool bShow = OptimItemShown(ctx, item, bHasKw ? strText : CDuiString(), nStateFilter,
                                bGroupMatch);
    if (bShow) bAnyItem = true;
    return bShow;
}

// [R152] 组头/组体的可见性与箭头字形设置（从 ApplyFindFilter 抽出）。
// 原实现把「复位分支」与「过滤分支」各写成 20 行同构代码，两分支都要
// 查 optarrow_N 并设字形，判定点密集导致 CC=23。
// 抽成两个具名函数后，主循环只表达「算状态 → 交给对应分支」。
namespace
{
// 取 optarrow_<g> 控件并按 nGlyphIdx（0=收起 1=展开）设置箭头字形。
void OptimSetGroupArrow(IPageContext& ctx, int g, int nGlyphIdx)
{
    CDuiString sArrowName;
    sArrowName.Format(L"optarrow_%d", g);
    CTextUI* pArrow = static_cast<CTextUI*>(ctx.HostFindControl(sArrowName.GetData()));
    if (pArrow) pArrow->SetText(ctx.HostGlyphArrow(nGlyphIdx));
}
}   // namespace

// 复位分支：组头按 OS 适用性、组体按用户折叠态还原。
// 不能无条件展开，否则每次清空搜索框都会把用户折叠好的组全部弹开。
void CPageOptim::OptimResetGroupVisibility(int g, CControlUI* pGroupRow,
                                           CVerticalLayoutUI* pBody, bool bGroupApplicable)
{
    const bool bExpanded = bGroupApplicable
        && g < static_cast<int>(m_vExpanded.size()) && m_vExpanded[g];
    if (pGroupRow) pGroupRow->SetVisible(bGroupApplicable);
    if (pBody) pBody->SetVisible(bExpanded);
    // [A-1] 复位分支必须重设箭头字形：过滤分支会把箭头强制设为"已展开"，
    // 若复位时不还原，就会出现"箭头朝下表示展开、组体却已按折叠态隐藏"的
    // 错位，用户点箭头反而先收起再展开。
    OptimSetGroupArrow(m_ctx, g, bExpanded ? 1 : 0);
}

// 过滤生效分支：命中的组必须整组可见（否则组体仍按折叠态隐藏，
// 用户看到"搜到了组名但下面一片空白"）。
void CPageOptim::OptimShowFilteredGroup(int g, CControlUI* pGroupRow,
                                        CVerticalLayoutUI* pBody, bool bVisible)
{
    if (pGroupRow) pGroupRow->SetVisible(bVisible);
    if (pBody) pBody->SetVisible(bVisible);
    // 组体被强制展开时箭头必须同步为"已展开"：否则折叠箭头朝右、内容却在下面
    // 显示，用户点箭头反而先把组收起来再展开。折叠态本身不动（m_vExpanded 未改），
    // 清空搜索后自动还原。
    OptimSetGroupArrow(m_ctx, g, bVisible ? 1 : 0);
}

// [R176] 从 ApplyFindFilter（51 行 / CC13）抽出：单个组内的逐项显隐。
// bAnyItem / bGroupApplicable 由 OptimItemVisible 累加，回传给调用方决定组头显隐。
// pBody 可能为空（组体尚未建好）：此时仍要推进 i 与 item，保证 bAnyItem 统计口径
// 与"UI 是否存在"无关 —— 否则未建体的组会被误判为空组而整组隐藏。
void CPageOptim::OptimApplyItemVisibility(CMarkupNode group, CVerticalLayoutUI* pBody,
                                          const CDuiString& strText, bool bReset,
                                          bool bHasKw, bool bGroupMatch,
                                          bool& bAnyItem, bool& bGroupApplicable)
{
    int i = 0;
    CMarkupNode item = group.GetChild(L"Item");
    while (item.IsValid())
    {
        CControlUI* pItemRow = pBody ? pBody->GetItemAt(i) : nullptr;
        if (pItemRow)
        {
            const bool bShow = OptimItemVisible(m_ctx, item, m_nOptimStateFilter, strText,
                                                bReset, bHasKw, bGroupMatch,
                                                bAnyItem, bGroupApplicable);
            pItemRow->SetVisible(bShow);
        }
        ++i;
        item = item.GetSibling();
    }
}

void CPageOptim::ApplyFindFilter(const CDuiString& strText)
{
    CMarkupNode root = m_ctx.DataEngine().GetOptimizationRoot();
    if (!root.IsValid()) return;
    CVerticalLayoutUI* pTree = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"optim_tree"));
    if (!pTree) return;
    const bool bHasKw = !strText.IsEmpty();
    const bool bHasState = (m_nOptimStateFilter != 0);
    const bool bReset = !bHasKw && !bHasState;
    // [FILTER-SSOT] 供写入链路复用；[R160] 三元组统一经 SetOptimFilterState 写入
    SetOptimFilterState(!bReset, bHasKw ? strText : CDuiString(), m_nOptimStateFilter);
    int g = 0;
    CMarkupNode group = root.GetChild(L"Group");
    while (group.IsValid())
    {
        CDuiString strGroupName = m_ctx.DataEngine().ResolveText(group.GetAttributeValue(L"Name"));
        CDuiString sRowName;
        sRowName.Format(L"optgrp_%d", g);
        CControlUI* pGroupRow = m_ctx.HostFindControl(sRowName.GetData());
        CDuiString sBodyName;
        sBodyName.Format(L"optbody_%d", g);
        CVerticalLayoutUI* pBody = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(sBodyName.GetData()));
        // [FILTER-SSOT] 组名命中关键字也算组内条目可见，与 OptimGroupMatch 同义。
        const bool bGroupMatch = bHasKw && ContainsCI(strGroupName.GetData(), strText.GetData());
        bool bAnyItem = false;
        bool bGroupApplicable = false;
        // [R176] 组内逐项显隐抽为 OptimApplyItemVisibility（原 while 内联，CC 记账 4 个判定点）
        OptimApplyItemVisibility(group, pBody, strText, bReset, bHasKw, bGroupMatch,
                                 bAnyItem, bGroupApplicable);
        RecalcBodyHeight(pBody);
        if (bReset)
            OptimResetGroupVisibility(g, pGroupRow, pBody, bGroupApplicable);
        else
            OptimShowFilteredGroup(g, pGroupRow, pBody,
                                   bAnyItem || (bGroupMatch && bGroupApplicable));
        ++g;
        group = group.GetSibling();
    }
}

// [OPTIM-BOX] DuiLib 的 CComboUI 画的是**自己的 m_sText**（UICombo.cpp:1282
//   PaintText 里 DrawText(GetText())），而 SelectItem 只改选中态、**不同步文字**。
//   所以程序选中之后必须补一次 SetText，否则框里永远是空白 ——
//   界面上就是一个"空的带边框方框"，与同列另一列的"—"占位完全不对。
static void OptimSelectCombo(IPageContext& ctx, CMarkupNode item, int sc,
                             CComboUI* pCombo, int nSel)
{
    if (!pCombo || nSel < 0) return;
    pCombo->SelectItem(nSel, false);
    CDuiString sName;
    if (ctx.DataEngine().GetScopeDropdownName(item, sc, nSel, sName))
        pCombo->SetText(sName.GetData());
    pCombo->Invalidate();
}

// [N15] T2/T5 拆分：VerifyOptimStates 单作用域状态同步（原 for 内 if(pSw)/if(pCombo) 内联）
static void OptimSyncItemScope(IPageContext& ctx, CMarkupNode item,
                               int g, int i, int sc, int& nChecked)
{
    if (!item.GetChild(ctx.DataEngine().ScopeName(sc)).IsValid()) return;
    CDuiString sSwName;
    sSwName.Format(L"sw_%d_%d_%d", g, i, sc);
    CSwitchUI* pSw = static_cast<CSwitchUI*>(ctx.HostFindControl(sSwName.GetData()));
    if (pSw)
    {
        int nSt = ctx.DataEngine().GetScopeState(item, sc);
        bool bOn = (nSt == 1);
        if (bOn) ++nChecked;
        pSw->SetOn(bOn, false);
    }
    CDuiString sComboName;
    sComboName.Format(L"cbu_%d_%d_%d", g, i, sc);
    CComboUI* pCombo = static_cast<CComboUI*>(ctx.HostFindControl(sComboName.GetData()));
    if (pCombo)
    {
        const int nReal = ctx.DataEngine().GetScopeDropdown(item, sc);
        // [OPTIM-BOX] nReal<0 = 系统当前状态与任何候选项都不匹配。
        //   原实现直接 SelectItem(nReal)：DuiLib 的 SelectItem 拒绝负数
        //   （UICombo.cpp:359），组合框因此**没有选中项** → 渲染成一个
        //   空的带边框方框（同列另一列是"—"占位），用户看到的就是"这个框空的"。
        //   统一规则：方框永远要有内容 —— 落到第一个候选项。
        const int nItems = pCombo->GetCount();
        int nSel = nReal;
        if (nSel < 0 && nItems > 0) nSel = 0;
        // 文字与选中态必须一起更新：光 SelectItem 框里仍然是空的。
        if (pCombo->GetCurSel() != nSel || pCombo->GetText().IsEmpty())
            OptimSelectCombo(ctx, item, sc, pCombo, nSel);
    }
}

// [R186] 从 VerifyOptimStates（51 行 / CC12）抽出：遍历全部组与条目，
// 统计「可见条目数」，并对每个可见条目同步其各作用域开关状态。
// 只有 ItemIsApplicable 为真的条目才计入 nVisible 并同步 —— 不适用的条目
// 本页不展示，同步它反而会把状态写进用户看不到的控件。
// nChecked 由 OptimSyncItemScope 累加（引用传出）。
void CPageOptim::VerifyCountAndSync(CMarkupNode root, int& nVisible, int& nChecked)
{
    int g = 0;
    CMarkupNode group = root.GetChild(L"Group");
    while (group.IsValid())
    {
        int i = 0;
        CMarkupNode item = group.GetChild(L"Item");
        while (item.IsValid())
        {
            if (ItemIsApplicable(m_ctx.DataEngine(), item))
            {
                ++nVisible;
                for (int sc : optim_scope::kColumns)
                    OptimSyncItemScope(m_ctx, item, g, i, sc, nChecked);
            }
            ++i;
            item = item.GetSibling();
        }
        ++g;
        group = group.GetSibling();

        // [G6 2026-10-25] 每处理完一组让出一次时间片。
        //   原实现切页时在 UI 线程一次性同步全量刷新（333 项 × 多作用域），
        //   注册表慢时表现为"切页卡顿"。这里按组分片泵消息，与 G1 批量执行
        //   同一套做法；泵的预算设小（4ms），因为校验不该抢占用户交互。
        optim_detail::PumpUiMessages(4);
    }
}

// [R213] 验收取证：把优化页第 g 组前 nRows 行的名称列 / 开关列真实矩形写进日志。
//   为什么需要：导出位图里开关列整列为空，像素统计无法区分
//   "控件根本没有位置" 与 "位置正确但绘制被裁掉"，只能读 m_rcItem 定案。
//   只在导出脚径调用，不进入交互路径。
void CPageOptim::DumpOptimRowGeometry(int g, int nRows)
{
    for (int i = 0; i < nRows; ++i)
    {
        CDuiString sName; sName.Format(L"optname_%d_%d", g, i);
        CControlUI* pName = m_ctx.HostFindControl(sName.GetData());
        if (!pName) continue;
        const RECT rcN = pName->GetPos();

        CDuiString sLine;
        sLine.Format(L"[geo] optname_%d_%d vis=%d rc=(%d,%d,%d,%d)",
                     g, i, pName->IsVisible() ? 1 : 0,
                     static_cast<int>(rcN.left), static_cast<int>(rcN.top),
                     static_cast<int>(rcN.right), static_cast<int>(rcN.bottom));
        AppendLog(sLine.GetData());

        // 两个作用域的开关：位置最可疑的是它们
        for (int sc : optim_scope::kColumns)
        {
            CDuiString sSw; sSw.Format(L"sw_%d_%d_%d", g, i, sc);
            CControlUI* pSw = m_ctx.HostFindControl(sSw.GetData());
            if (!pSw) { AppendLog(L"[geo] switch MISSING"); continue; }
            const RECT rcS = pSw->GetPos();
            CDuiString sL;
            sL.Format(L"[geo] sw_%d_%d work=%d vis=%d rc=(%d,%d,%d,%d) w=%d h=%d",
                      g, i, sc, pSw->IsVisible() ? 1 : 0,
                      static_cast<int>(rcS.left), static_cast<int>(rcS.top),
                      static_cast<int>(rcS.right), static_cast<int>(rcS.bottom),
                      static_cast<int>(rcS.right - rcS.left),
                      static_cast<int>(rcS.bottom - rcS.top));
            AppendLog(sL.GetData());
        }
        if (i >= 1) break;   // 取前 2 行足够说明问题
    }
}

void CPageOptim::VerifyOptimStates(bool bForce)

{
    if (!m_ctx.DataEngine().IsValid()) return;
    // [STAB] 重入闸门：VerifyCountAndSync 每组间泵消息，泵内派发的
    //   WM_TIMER(PAGE_BUILD) 会经 OnTimerPageBuild→VERIFY_SOFT 再次进入本函数，
    //   在第一轮 CMarkupNode 迭代中途开第二轮（与 G1 批量 m_bBatchRunning 同一
    //   风险模式，G6 加泵时漏设这条）。嵌套遍历 + 嵌套构建 RemoveAll 控件 →
    //   悬垂写 → 0xC0000374 随机崩溃。
    if (m_bVerifyingOptim) return;
    struct VerifyGuard { bool& m_f; explicit VerifyGuard(bool& f) : m_f(f) { m_f = true; }
                        ~VerifyGuard() { m_f = false; } } verifyGuard(m_bVerifyingOptim);
    // 5 second session cache (like the renderer scan TTL)
    DWORD dwNow = ::GetTickCount();
    if (!bForce && m_dwOptimVerifyTick != 0 &&
        (dwNow - m_dwOptimVerifyTick) < 5000)
    {
        return;
    }
    CMarkupNode root = m_ctx.DataEngine().GetOptimizationRoot();
    if (!root.IsValid()) return;
    // [STAB] TTL 时间戳提前到入口写：原在函数末尾写，泵窗口内时间戳未更新，
    //   5 秒内的重入只靠闸门拦；现在闸门+TTL 两层防线对齐。
    m_dwOptimVerifyTick = dwNow;
    // [R186] 双层遍历统计抽为 VerifyCountAndSync（原内联，CC 记账 5 点）
    int nChecked = 0, nVisible = 0;
    VerifyCountAndSync(root, nVisible, nChecked);
    CDuiString sLog;
    sLog.Format(L"[optim] verified: %d visible items, %d scope-switches on",
                nVisible, nChecked);
    AppendLog(sLog.GetData());
    // O-6 状态反馈：已核实 X/N 项
    CDuiString sStatus;
    sStatus.Format(StringMgr.Get("misc.se3c70f77"), nVisible, nChecked);
    CTextUI* pSt = static_cast<CTextUI*>(m_ctx.HostFindControl(L"lbl_optim_status"));
    if (pSt) pSt->SetText(sStatus);
    // O-2: 状态回读后同步所有分组三态
    CMarkupNode grpRoot = m_ctx.DataEngine().GetOptimizationRoot();
    CMarkupNode gNode = grpRoot.GetChild(L"Group");
    int gi = 0;
    // [PERF] 单趟推进：直接把手上的 gNode 传进去，免去每组从根重扫
    for (; gNode.IsValid(); gNode = gNode.GetSibling()) { SyncOptimGroupGlyph(gi, gNode); ++gi; }
    // [STAB] m_dwOptimVerifyTick 已在入口写（见上），此处不再重复。
}


void CPageOptim::OnStateFilterClicked()

{
            // O-5: 循环切换 全部 / 仅已启用 / 仅未启用
            m_nOptimStateFilter = (m_nOptimStateFilter + 1) % 3;
            const wchar_t* pszCap[3] = { StringMgr.Get("misc.sc9646bfe"), StringMgr.Get("misc.s6353c21c"), StringMgr.Get("misc.sb1fd8d26") };
            CButtonUI* pBtnState = static_cast<CButtonUI*>(
                m_ctx.HostFindControl(L"btn_optim_state"));
            if (pBtnState) pBtnState->SetText(pszCap[m_nOptimStateFilter]);
            CEditUI* pEdit = static_cast<CEditUI*>(m_ctx.HostFindControl(L"find_edit"));
            CDuiString sKw = pEdit ? pEdit->GetText() : CDuiString();
            ApplyFindFilter(sKw);
            return;
}

void CPageOptim::OnFindButtonClicked()

{
            CEditUI* pEdit = static_cast<CEditUI*>(m_ctx.HostFindControl(L"find_edit"));
            if (pEdit)
            {
                if (!pEdit->IsVisible())
                {
                    pEdit->SetVisible(true);
                    pEdit->SetFocus();
                }
                else
                {
                    pEdit->SetVisible(false);
                    pEdit->SetText(L"");
                    ApplyFindFilter(L"");
                }
            }
            return;
}

// find_edit textchanged (verbatim routing of the former Notify block)
void CPageOptim::OnFindFilterChanged(const wchar_t* pszText)

{
    ApplyFindFilter(pszText);
}



// ============================================================
// P8：第二级事件路由（原 CFrameWnd::Notify 优化页分支逐字迁入）
// ============================================================

// [R156] click 按钮分派：按钮名 → 无参处理函数（从 OnEvent 抽出，原 CC18）。
// 表放在成员函数体内：成员函数指针取地址在类作用域外是非法访问。
bool CPageOptim::OptimDispatchClickButton(const CDuiString& sName)
{
    using BtnHandler = void (CPageOptim::*)();
    struct BtnRoute { const wchar_t* pszName; BtnHandler pfn; };
    static constexpr BtnRoute kBtns[] = {
        { L"btn_find",        &CPageOptim::OnFindButtonClicked },  // 查找
        { L"btn_optim_bulk",  &CPageOptim::BulkApplyAllOptim },    // 批量应用
        { L"btn_optim_state", &CPageOptim::OnStateFilterClicked },  // 状态过滤
        { L"btn_apply_rules", &CPageOptim::VerifyOptimStatesForce }, // 强制核实（原 VerifyOptimStates(true)）
        { L"btn_export_cfg",  &CPageOptim::ExportOptimConfig },    // 导出配置
    };
    for (const BtnRoute& r : kBtns)
    {
        if (sName != r.pszName) continue;
        (this->*r.pfn)();
        return true;
    }
    return false;
}

// [R176] 从 OnEvent（43 行 / CC13）抽出：click 事件的层级判定。
// 返回 true 表示已消费（调用方须立即 return）。
// [组头复选框已移除] 原来这里还有一条 optgrp_ck_ 分支（组复选框 → 批量
//   全选/全不选整组）。组头不再有复选框，该分支连同它的实现
//   （OnGroupCheckClick / Gck* 系列）一并删除；组标题点击仍是折叠/展开。
// [R199] 组控件名前缀的长度（含下划线），必须与 wcsncmp 的字符串字面量
// 长度严格一致，写错会导致 _ttoi 从错误偏移取号。具名后与字面量成对可核。
inline constexpr int kOptimGroupPrefixLen = 7;    // L"optgrp_"

bool CPageOptim::OpHandleClickMsg(const CDuiString& sName, const wchar_t* pszName)
{
    // [G4-opt] 任意点击都刷新"最近交互行"：键盘菜单需要它做锚点。
    //   这里只读控件名/祖先链，代价是几次字符串比较，可忽略。
    (void)sName;
    if (wcsncmp(pszName, L"optgrp_", kOptimGroupPrefixLen) == 0)
    {
        ToggleGroup(_ttoi(pszName + kOptimGroupPrefixLen));
        return true;
    }
    OptimDispatchClickButton(sName);
    return true;
}

// [R176] 从 OnEvent 抽出：itemselect（下拉框）事件。
// 只认 cbu_ 前缀；仅靠前缀不能证明发送者类型，故用 RTTI 校验，
// 杜绝同名/重名控件被误当 CComboUI 解引用而崩溃。
bool CPageOptim::OpHandleItemSelectMsg(TNotifyUI& msg, const wchar_t* pszName)
{
    if (wcsncmp(pszName, L"cbu_", 4) != 0) return false;
    CComboUI* pCombo = dynamic_cast<CComboUI*>(msg.pSender);
    if (pCombo) OnScopeCombo(pszName, pCombo->GetCurSel());
    return true;
}

void CPageOptim::OnEvent(TNotifyUI& msg)
{
    CDuiString sName = msg.pSender ? msg.pSender->GetName() : CDuiString(L"");
    const wchar_t* pszName = sName.GetData();
    // A combo fires "itemselect" with the CComboUI itself as sender (the
    // later "click" is sent by the popup's ListLabelElement, which has no
    // name and must not be cast to CComboUI).
    if (msg.sType == DUI_MSGTYPE_ITEMSELECT)
    {
        OpHandleItemSelectMsg(msg, pszName);   // [R176] 抽为函数
        return;
    }
    if (msg.sType == L"click")
    {
        // 组标题点击 = 折叠/展开（组复选框已移除，不再有优先级分支）
        OpHandleClickMsg(sName, pszName);      // [R176] 抽为函数
        return;
    }
    if (msg.sType == L"textchanged")
    {
        if (sName != L"find_edit") return;
        CEditUI* pEdit = static_cast<CEditUI*>(m_ctx.HostFindControl(L"find_edit"));
        if (pEdit) OnFindFilterChanged(pEdit->GetText());
        return;
    }
    if (msg.sType != L"selectchanged") return;
    if (wcsncmp(pszName, L"sw_", 3) == 0)
        OnSwitchToggle(pszName, msg.wParam == 1);
}

// P8：原 CFrameWnd WM_RBUTTONUP optim_tree 命中块（仅鼠标消息）
// [R156] 从命中控件向上找「它所属的 optbody_<g> 行」，回填 g/i。
// 旧实现只在命中 sw_g_i_s 开关时解析 g/i；用户右键点条目的文字区域
// （最常见的操作）拿不到 g/i，bHitRule 为假 → 整个菜单除分隔符外全部置灰，
// "属性/复制/打开位置"在行上点等于没有。
// 行容器的名字是 optbody_<g>，行本身在其子项中的序号即条目序号 i。
// [R186] 从 OcmResolveRowFromHit（26 行 / CC12）抽出：在指定的 body 下定位命中行。
// 自命中控件向上走到 body 的直接子项（即该行），再向 body 取行号。
// 返回 -1 表示没走到（控件树异常），调用方与原实现一样直接 return 不写上下文。
[[nodiscard]] int CPageOptim::OcmResolveRowInBody(CControlUI* pHit, CControlUI* pBodyNode,
                                                 CContainerUI* pBody)
{
    CControlUI* pRow = pHit;
    while (pRow != nullptr && pRow->GetParent() != pBodyNode)
        pRow = pRow->GetParent();
    if (pRow == nullptr) return -1;
    return pBody->GetItemIndex(pRow);
}

void CPageOptim::OcmResolveRowFromHit(CControlUI* pHit, int& gCtx, int& iCtx)
{
    if (pHit == nullptr) return;
    const wchar_t* pnHit = pHit->GetName();
    // 命中开关时不走这条路（由调用方的祖先链扫描解析 sw_g_i_s）
    if (pnHit != nullptr && wcsncmp(pnHit, L"sw_", 3) == 0) return;
    for (CControlUI* q = pHit; q != nullptr; q = q->GetParent())
    {
        const wchar_t* pn = q->GetName();
        if (pn == nullptr) continue;
        if (wcsncmp(pn, L"optbody_", 8) != 0) continue;
        CContainerUI* pBody = dynamic_cast<CContainerUI*>(q);
        if (pBody == nullptr) return;
        // [R186] 找行 + 取行号的 5 步抽为 OcmResolveRowInBody（原内联，CC 记账 5 点）
        const int iRow = OcmResolveRowInBody(pHit, q, pBody);
        if (iRow >= 0)
        {
            gCtx = _ttoi(pn + 8);
            iCtx = iRow;
        }
        return;
    }
}

// [R156] 祖先链扫描：命中 sw_g_i_s 时解析出 (g,i,scope)，同时判定是否落在 optim_tree 内。
void CPageOptim::OcmScanAncestors(CControlUI* pHit, int& gCtx, int& iCtx, int& scCtx, bool& bInOptim)
{
    for (CControlUI* q = pHit; q; q = q->GetParent())
    {
        const wchar_t* pn = q->GetName();
        if (!pn) continue;
        if (wcsncmp(pn, L"sw_", 3) == 0)
            ParseSwName(pn, L"sw_", gCtx, iCtx, scCtx);
        if (lstrcmpW(pn, L"optim_tree") == 0) bInOptim = true;
    }
}

// [G4-opt 2026-10-25] 记录最近命中行，供键盘唤出右键菜单时复用。
//   优化页的行是 HoverRowUI，位于 optbody_N 容器内、按 index 定位，
//   没有全局行号可用；键盘触发时 pt=(-1,-1)，FindControl 必然落空。
//   因此改为把"用户最后一次点到的 (g,i,scope)"缓存下来。
void CPageOptim::OcmSaveLastRow(CControlUI* pHit)
{
    if (pHit == nullptr) return;
    int g = -1, i = -1, sc = -1;
    bool bIn = false;
    OcmResolveRowFromHit(pHit, g, i);
    OcmScanAncestors(pHit, g, i, sc, bIn);
    if (!bIn || g < 0 || i < 0) return;   // 不在优化树内：不覆盖上次有效值
    m_nOcmLastG = g;
    m_nOcmLastI = i;
    m_nOcmLastScope = sc;
}

// [G4-opt] 用缓存的最近行还原上下文并算出锚点。
//   返回 false 表示没有可用锚点（用户尚未点过任何行），调用方应放弃弹菜单。
[[nodiscard]] bool CPageOptim::OcmReplayLastRow(int& gCtx, int& iCtx, int& scCtx)
{
    if (m_nOcmLastG < 0 || m_nOcmLastI < 0) return false;
    gCtx = m_nOcmLastG;
    iCtx = m_nOcmLastI;
    scCtx = m_nOcmLastScope;
    return true;
}

bool CPageOptim::OnContextMenu(POINT pt, bool bFromContextMsg)
{
    int gCtx = -1, iCtx = -1, scCtx = -1;
    if (bFromContextMsg)
    {
        // [G4-opt] 键盘路径：用缓存的最近行合成菜单，锚在 optim_tree 左上角内侧。
        //   没有缓存行时放弃（弹一个无上下文的菜单没有意义）。
        if (!OcmReplayLastRow(gCtx, iCtx, scCtx)) return false;
        CControlUI* pTree = m_ctx.HostFindControl(L"optim_tree");
        if (!pTree) return false;
        const RECT& rcTree = pTree->GetPos();
        POINT ptAnchor = { rcTree.left + 40, rcTree.top + 20 };
        ::ClientToScreen(m_ctx.HostHwnd(), &ptAnchor);
        ShowOptimContextMenu(gCtx, iCtx, scCtx, ptAnchor);
        return true;
    }
    bool bInOptim = false;
    CControlUI* pHit = m_ctx.HostFindControlAt(pt);
    // [CTX-ROW] 右键落在"行"上也要能定位到规则
    OcmResolveRowFromHit(pHit, gCtx, iCtx);
    // 继承旧行为：祖先链扫描会覆盖上面解析出的 g/i（开关命中时以 sw_ 名为准）
    OcmScanAncestors(pHit, gCtx, iCtx, scCtx, bInOptim);
    if (!bInOptim) return false;
    // [G4-opt] 鼠标右键同样是一次有效交互：记录下来，供之后键盘唤出菜单使用。
    if (gCtx >= 0 && iCtx >= 0)
    {
        m_nOcmLastG = gCtx; m_nOcmLastI = iCtx; m_nOcmLastScope = scCtx;
    }
    ShowOptimContextMenu(gCtx, iCtx, scCtx, pt);
    return true;
}
