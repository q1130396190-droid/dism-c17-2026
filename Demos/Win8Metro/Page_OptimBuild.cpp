// ============================================================
// Page_OptimBuild.cpp —— 优化页「树构建族」（从 Page_Optim.cpp 拆出）。
//
// [R162] Page_Optim.cpp 现 1,545 行、规模分 8.2（六页最低）。规模分只统计
// 主文件行数，故必须从主文件往外搬，而不是动 Page_OptimOcm.cpp。
// 本文件承接 8 个 Build 族成员（共 244 行）：
//   OptimBuildResetFilter / OptimBuildCountGroups / OptimBuildGroupRow /
//   OptimBuildSwitchCell / OptimBuildSwitchRow / OptimBuildComboCell /
//   OptimBuildComboRow / OptimBuildFinishGroup
//
// 闭包说明：全部是 CPageOptim 成员（Page_Optim.h 已声明）。
// 族外唯一调用方是 Build()（在 Split 后仍留在主文件），
// 其余族内互调均在同一 TU 内。
// 依赖：FrameWnd_Util.h（StyleCheckBox / RecalcBodyHeight）、
//       CustomControls.h（CClickRowUI / CHoverRowUI 等）、OptimScopeColumns.h。
//
// 搬迁为逐字搬运，零逻辑改动。
// ============================================================

#include "stdafx.h"
#include "ThemeTokens.h"   // [R203] 颜色令牌（深色/浅色跟随）

#include "Page_Optim.h"

#include "StringManager.h"
#include "FrameWnd_Util.h"          // StyleCheckBox / RecalcBodyHeight
#include "CustomControls.h"         // CClickRowUI / CHoverRowUI / CSwitchUI / CComboUI
#include "OptimScopeColumns.h"      // optim_scope::kColumns

using namespace DuiLib;
using i18n::CStringManager;

// [C++17 R6] 本 TU 特征常量：树构建时组内条目缩进与固定行高（与原字面量同值）。
// 行内子控件间距：原散落 3 处 SetChildPadding(kOptimBuildCellPadding)，统一为具名常量。
inline constexpr int kOptimBuildCellPadding = 8;
// 下拉框固定高度：原裸字面量 28，统一为具名常量。
inline constexpr int kOptimBuildComboHeight = 28;
static_assert(kOptimBuildComboHeight > 0, "combo height must be positive");
static_assert(kOptimBuildCellPadding >= 0, "cell padding must not be negative");

void CPageOptim::OptimBuildResetFilter()
{
// O-5: 重建树后复位状态快捷过滤
// [R160] 三元组统一经 SetOptimFilterState 写入（原来此处在 Build 侧裸写三行，
//   与 ApplyFindFilter 的条件赋值构成两条并行维护的状态路径）
SetOptimFilterState(false, CDuiString(), 0);
CButtonUI* pBtnStateReset = static_cast<CButtonUI*>(m_ctx.HostFindControl(L"btn_optim_state"));
if (pBtnStateReset) pBtnStateReset->SetText(StringMgr.Get("misc.sc9646bfe"));
// [FILTER-SSOT] 状态过滤复位的同时必须清掉关键字与过滤标志，否则：
//   树是全新的（所有行按适用性重建为可见），但 m_sOptimFindText 仍留着
//   上一次的关键字 → 之后点「全选/全不选」只改到那个早已不显示的旧
//   关键字命中的少数几项。同时把搜索框收起并清空，与视觉状态一致。
CEditUI* pEditReset = static_cast<CEditUI*>(m_ctx.HostFindControl(L"find_edit"));
if (pEditReset)
{
    if (!pEditReset->GetText().IsEmpty()) pEditReset->SetText(L"");
    pEditReset->SetVisible(false);
}
}

void CPageOptim::OptimBuildCountGroups(CMarkupNode root)
{
    int nGroups = 0;
    for (CMarkupNode t = root.GetChild(L"Group"); t.IsValid(); t = t.GetSibling())
        ++nGroups;
    // [STAB] 只在首次构建（或组数变化）时初始化折叠态，重入时保留用户已展开的组。
    // 旧实现无条件 assign(nGroups, false)，导致 F5 刷新 / 切页往返 / DPI 重建后
    // 用户展开的分组全部塌回去——与预应答器页曾出现的"定位丢失"是同一类问题。
    // [FIX-UX] 首次构建默认展开第一组：全部折叠的空白页让新用户以为"没有内容"，
    //   与空间回收页"默认展开前两组"的先例对齐。
    if (static_cast<int>(m_vExpanded.size()) != nGroups)
    {
        m_vExpanded.assign(nGroups, false);
        if (nGroups > 0) m_vExpanded[0] = true;
    }
}

CClickRowUI* CPageOptim::OptimBuildGroupRow(const DuiLib::CDuiString& strGroupName, int g)
{
    // ---- group row ----
    auto pRow = std::make_unique<CClickRowUI>(); // [C++17 R3]
    pRow->SetFixedHeight(40);
    RECT rcRowInset = { 6, 0, 10, 0 };
    pRow->SetInset(rcRowInset);
    pRow->SetChildPadding(kOptimBuildCellPadding);
    CDuiString sRowName;
    sRowName.Format(L"optgrp_%d", g);
    pRow->SetName(sRowName.GetData());
    auto pArrow = std::make_unique<CTextUI>(); // [C++17 R3]
    CDuiString sArrowName;
    sArrowName.Format(L"optarrow_%d", g);
    pArrow->SetName(sArrowName.GetData());
    pArrow->SetFixedWidth(16);
    pArrow->SetFont(9);
    pArrow->SetText(m_ctx.HostGlyphArrow(0));
    pArrow->SetTextColor(tokens::Token(tokens::EToken::InkMuted));
    pArrow->SetAttribute(L"align", L"left");
    pArrow->SetAttribute(L"valign", L"vcenter");
    pArrow->SetMouseEnabled(false);
    pRow->Add(pArrow.release()); // [C++17 R3] ownership transferred to DuiLib
    // [组头复选框已移除] 原来组名是由组复选框 optgrp_ck_%d 承载的
    // （文本即组名，宽度 0 占满弹性列）。按需求"大类上不要有复选框"，
    // 这里改回一个纯文本承载组名：组头只剩"箭头 + 组名"，
    // 组内条目（开关 / 取值框）完全不受影响。
    auto pName = std::make_unique<CTextUI>(); // [C++17 R3]
    CDuiString sGrpName;
    sGrpName.Format(L"optgrp_name_%d", g);
    pName->SetName(sGrpName.GetData());
    pName->SetFixedWidth(0);
    pName->SetText(strGroupName.GetData());
    pName->SetTextColor(tokens::Token(tokens::EToken::Ink));
    pName->SetAttribute(L"valign", L"vcenter");
    pName->SetAttribute(L"endellipsis", L"true");
    pName->SetMouseEnabled(false);
    pRow->Add(pName.release()); // [C++17 R3] ownership transferred to DuiLib
return pRow.release(); // [C++17 R3] ownership returned to caller (added to tree in OptimBuildFinishGroup)
}

void CPageOptim::OptimBuildSwitchCell(CHoverRowUI* pItemRow, CMarkupNode item, int g, int i)
{
    // [SCOPE-2COL] 列定义唯一真源：optim_scope::kColumns（当前用户 → 系统）。
    // 每列固定 140 宽，与 page_optim.xml 表头逐列对齐；该作用域盒子不存在
    // （条目本身不覆盖此作用域）时渲染灰色 dash 占位，保持行内控件数与
    // 表头列数恒定——否则后续列的开关会整体左移，落到别的表头下面。
    for (int sc : optim_scope::kColumns)
    {
        CMarkupNode box = item.GetChild(m_ctx.DataEngine().ScopeName(sc));
        if (box.IsValid())
        {
            auto pCell = std::make_unique<CHorizontalLayoutUI>(); // [C++17 R3]
            pCell->SetFixedWidth(140);
            // [R213] 开关在列内居中：这里不再用 float，也不用 width=0 的垫片。
            //   两版失败原因（都已实测确认）：
            //     1) 垫片版：CHorizontalLayoutUI 把 cx==0 的子项都计入
            //        nAdjustables，三个弹性项平分 140，开关被压成 0 像素；
            //     2) float 版：SetFloatPos 只在 uAlign!=0 时生效，而容器在
            //        SetPos 里对 float 子项走 continue 分支，列体高度未被计入，
            //        实测开关列整列无像素。
            //   改为「列内容居中对齐 + 开关固定宽高」：列内只有开关一个普通
            //   子项，由水平布局按 childalign=center 直接摆到中间，不参与抢宽。
            pCell->SetAttribute(L"childalign", L"center");
            pCell->SetAttribute(L"childvalign", L"vcenter");
            pCell->SetChildPadding(0);
            auto pSw = std::make_unique<CSwitchUI>(); // [C++17 R3]
            CDuiString sSwName;
            sSwName.Format(L"sw_%d_%d_%d", g, i, sc);
            pSw->SetName(sSwName.GetData());
            // state is verified on demand when the page is shown
            pCell->Add(pSw.release()); // [C++17 R3] ownership transferred to DuiLib
            pItemRow->Add(pCell.release()); // [C++17 R3] ownership transferred to DuiLib
        }
        else
        {
            // [R193] 空列占位抽为 OptimAddDashCell（原内联 8 行）
            OptimAddDashCell(pItemRow);
        }
    }
}

void CPageOptim::OptimBuildSwitchRow(CVerticalLayoutUI* pBody, CMarkupNode item, const DuiLib::CDuiString& strDisplay, int g, int i, bool bApplicable, bool bUnsupported)
{
        {
            auto pItemRow = std::make_unique<CHoverRowUI>(); // [C++17 R3]
            pItemRow->SetFixedHeight(32);
            RECT rcItemInset = { 30, 0, 10, 0 };
            pItemRow->SetInset(rcItemInset);
            pItemRow->SetChildPadding(kOptimBuildCellPadding);
            pItemRow->SetVisible(!!bApplicable);
            auto pLbl = std::make_unique<CTextUI>(); // [C++17 R3]
            pLbl->SetText(strDisplay.GetData());
            // [R213] 命名：验收时要能按名字回读该列的真实几何（见 DumpOptimRowGeometry）
            CDuiString sLblName;
            sLblName.Format(L"optname_%d_%d", g, i);
            pLbl->SetName(sLblName.GetData());
            // [R213] 名称列必须吃掉行内剩余宽度。
            // [R320] 但**不能**用 SetAutoCalcWidth(true)。
            //   实测（shottheme/_geometry.txt）：开启之后 CLabelUI::EstimateSize
            //   走"按文字量宽"分支，返回一个随文字长度变化的非零 cx，
            //   CHorizontalLayoutUI 就不再把它计入 nAdjustables —— 于是每一行
            //   的宽度都变成"文字宽 + 两个 140 列"，行与行右边缘对不齐，
            //   开关列跟着文字长度左右漂移：
            //       optname_0_0  右 862 -> 开关 x 963
            //       optname_0_2  右 715 -> 开关 x 816
            //       optname_0_6  右 673 -> 开关 x 774
            //       optname_0_11 右 787 -> 开关 x 888
            //   开关位置 = 名称列右边界 + 101，完全是逐行自适应，不是列对齐。
            //
            //   正确做法：只保留 width=0。EstimateSize 会返回 cx==0，
            //   被计入 nAdjustables，按可用宽度平分 —— 每一行的行宽、
            //   两个开关列的 x 位置因此严格一致，这才是表格式排版。
            //   endellipsis 已在下面开启，长名称会被省略号截断而不是撑宽。
            pLbl->SetAttribute(L"width", L"0");
            pLbl->SetTextColor(tokens::Token(tokens::EToken::Ink));   // [FIX 2026-10-25] 统一黑色：原"不支持"条目置橙 0xFFC85A00，与整体不协调
            pLbl->SetAttribute(L"valign", L"vcenter");
            pLbl->SetAttribute(L"endellipsis", L"true");
            pLbl->SetMouseEnabled(false);
            pItemRow->Add(pLbl.release()); // [C++17 R3] ownership transferred to DuiLib
            // [SCOPE-2COL] 列装配下沉到 OptimBuildSwitchCell，行构建只负责行外壳：
            // 列定义（含 140 宽、dash 占位、控件命名）不再在此重复一份。
            OptimBuildSwitchCell(pItemRow.get(), item, g, i);
pBody->Add(pItemRow.release()); // [C++17 R3] ownership transferred to DuiLib
        }
}

// [R193] 从 OptimBuildComboCell（74 行 / CC4）抽出：把该作用域的候选项灌进下拉框。
// 每个候选项一个 CListLabelElementUI，高度用已按 DPI 缩放的 nItemH 传入 ——
// 弹层管理器不做 DPI 缩放（固定 100%），所以高度必须由调用方预先缩放。
void CPageOptim::OptimFillComboOptions(CComboUI* pCombo, CMarkupNode item, int sc, int nItemH)
{
    const int nDrop = m_ctx.DataEngine().GetScopeDropdownCount(item, sc);
    for (int dd = 0; dd < nDrop; ++dd)
    {
        CDuiString sDrop;
        m_ctx.DataEngine().GetScopeDropdownName(item, sc, dd, sDrop);
        auto pDropItem = std::make_unique<CListLabelElementUI>(); // [C++17 R3]
        pDropItem->SetFixedHeight(nItemH);
        pDropItem->SetText(sDrop.GetData());
        pCombo->Add(pDropItem.release()); // [C++17 R3] ownership transferred to DuiLib
    }
}

// [R193] 从 OptimBuildComboCell 抽出：该作用域不存在时补一个破折号占位。
// 占位保持列宽 140 恒定，避免缺列导致后续列的横向位置漂移。
void CPageOptim::OptimAddDashCell(CHoverRowUI* pItemRow)
{
    auto pDash = std::make_unique<CTextUI>(); // [C++17 R3]
    pDash->SetFixedWidth(140);
    pDash->SetText(m_ctx.HostGlyphDash());
    pDash->SetTextColor(tokens::Token(tokens::EToken::InkMuted));
    pDash->SetAttribute(L"align", L"center");
    pDash->SetAttribute(L"valign", L"vcenter");
    pDash->SetMouseEnabled(false);
    pItemRow->Add(pDash.release()); // [C++17 R3] ownership transferred to DuiLib
}

void CPageOptim::OptimBuildComboCell(CHoverRowUI* pItemRow, CMarkupNode item, int g, int i)
{
    // [SCOPE-2COL] 同 OptimBuildSwitchCell：列定义唯一真源，dash 占位保持列数恒定。
    for (int sc : optim_scope::kColumns)
    {
        CMarkupNode box = item.GetChild(m_ctx.DataEngine().ScopeName(sc));
        if (box.IsValid())
        {
            auto pCell = std::make_unique<CHorizontalLayoutUI>(); // [C++17 R3]
            pCell->SetFixedWidth(140);
            auto pSp1 = std::make_unique<CControlUI>(); pSp1->SetFixedWidth(0); // [C++17 R3]
            auto pCombo = std::make_unique<CComboUI>(); // [C++17 R3]
            CDuiString sComboName;
            sComboName.Format(L"cbu_%d_%d_%d", g, i, sc);
            pCombo->SetName(sComboName.GetData());
            pCombo->SetFixedWidth(124);
            pCombo->SetFixedHeight(kOptimBuildComboHeight);
            // The popup manager is not DPI-scaled (100%), so pre-scale
            // item heights / drop height ourselves using the main manager.
            int nDpiScale = m_ctx.HostDPIScale();
            int nItemH = MulDiv(26, nDpiScale, 100);
            int nDropH = MulDiv(220, nDpiScale, 100);
            pCombo->SetFont(1);
            pCombo->SetTextColor(tokens::Token(tokens::EToken::Ink));
            RECT rcComboTextPad = { 8, 4, 20, 4 };
            pCombo->SetTextPadding(rcComboTextPad);
            // cx = 0: popup keeps the combo width in physical coords
            SIZE szDropBox = { 0, nDropH };
            pCombo->SetDropBoxSize(szDropBox);
            pCombo->SetBkColor(tokens::Token(tokens::EToken::Surface));
            pCombo->SetAttribute(L"bordercolor", tokens::TokenHex(tokens::EToken::Border));
            pCombo->SetAttribute(L"bordersize", L"1");
            // [OPTIM-BOX] 焦点框：代码创建的控件拿不到皮肤 <Default name="Combo">
            //   （Default 只作用于 XML 解析出来的控件），焦点框会回退到系统高亮色，
            //   与页面里 XML 组合框的主题焦点框不一致。显式给主题强调色。
            pCombo->SetAttribute(L"focusbordercolor", tokens::TokenHex(tokens::EToken::Accent));
            pCombo->SetAttribute(L"itemtextcolor", tokens::TokenHex(tokens::EToken::Ink));
            // [FIX 深色] itembkcolor 原写死 #FFFFFFFF：本控件 itemtextcolor 取 Ink，
            //   深色模式下 Ink=#FFFFFF，而写死的白底 → 白字压白底，下拉列表项完全
            //   看不见。必须跟随 Surface（与 RefreshThemeColors 的同名赋值一致）。
            pCombo->SetAttribute(L"itembkcolor", tokens::TokenHex(tokens::EToken::Surface));
            // 选中项文字压在 Accent 底上：OnAccent 正是为此对比设计的令牌，
            //   深色/浅色都会自动选对（原写死白色，浅色强调底色下对比度不足）。
            pCombo->SetAttribute(L"itemselectedtextcolor", tokens::TokenHex(tokens::EToken::OnAccent));
            pCombo->SetAttribute(L"itemselectedbkcolor", tokens::TokenHex(tokens::EToken::Accent));   // [R210] 改令牌
            pCombo->SetAttribute(L"itemhotbkcolor", tokens::TokenHex(tokens::EToken::SurfaceHover));
            // single-line, left aligned, ellipsized; otherwise GDI+ wraps
            // the long option texts in the narrow popup and overflows rows
            pCombo->SetAttribute(L"itemfont", L"1");
            pCombo->SetAttribute(L"itemalign", L"left");
            pCombo->SetAttribute(L"itemvalign", L"vcenter");
            pCombo->SetAttribute(L"itemendellipsis", L"true");
            pCombo->SetAttribute(L"itemtextpadding", L"8,3,8,3");
            // [R193] 下拉项填充抽为 OptimFillComboOptions（原内联 10 行）
            OptimFillComboOptions(pCombo.get(), item, sc, nItemH);
            // [OPTIM-BOX] 建树时就把**显示文字**写好。
            //   ① CComboUI 画的是自己的 m_sText，SelectItem 不同步文字；
            //   ② 状态核实（VerifyOptimStates）带 5 秒 TTL，且"切页延迟构建"
            //      常常落在启动那次核实的窗口内 → 软核实被短路，
            //      组合框既没有选中项也没有文字 = 界面上一排空的带边框方框。
            //   所以这里在建树当场按系统真实状态取一项（匹配不到就取第一项），
            //   框里一定有内容，且不依赖核实是否跑过。
            {
                const int nCount = m_ctx.DataEngine().GetScopeDropdownCount(item, sc);
                int nCur = m_ctx.DataEngine().GetScopeDropdown(item, sc);
                if (nCur < 0 && nCount > 0) nCur = 0;
                if (nCur >= 0)
                {
                    pCombo->SelectItem(nCur, false);
                    CDuiString sName;
                    if (m_ctx.DataEngine().GetScopeDropdownName(item, sc, nCur, sName))
                        pCombo->SetText(sName.GetData());
                }
            }
            // selected item is verified on demand when the page is shown
            auto pSp2 = std::make_unique<CControlUI>(); pSp2->SetFixedWidth(0); // [C++17 R3]
            pCell->Add(pSp1.release()); // [C++17 R3] ownership transferred to DuiLib
            pCell->Add(pCombo.release()); // [C++17 R3] ownership transferred to DuiLib
            pCell->Add(pSp2.release()); // [C++17 R3] ownership transferred to DuiLib
            pItemRow->Add(pCell.release()); // [C++17 R3] ownership transferred to DuiLib
        }
        else
        {
            auto pDash = std::make_unique<CTextUI>(); // [C++17 R3]
            pDash->SetFixedWidth(140);
            pDash->SetText(m_ctx.HostGlyphDash());
            pDash->SetTextColor(tokens::Token(tokens::EToken::InkMuted));
            pDash->SetAttribute(L"align", L"center");
            pDash->SetAttribute(L"valign", L"vcenter");
            pDash->SetMouseEnabled(false);
            pItemRow->Add(pDash.release()); // [C++17 R3] ownership transferred to DuiLib
        }
    }
}

void CPageOptim::OptimBuildComboRow(CVerticalLayoutUI* pBody, CMarkupNode item, const DuiLib::CDuiString& strDisplay, int g, int i, bool bApplicable, bool bUnsupported)
{
        {
            auto pItemRow = std::make_unique<CHoverRowUI>(); // [C++17 R3]
            pItemRow->SetFixedHeight(32);
            RECT rcItemInset = { 30, 0, 10, 0 };
            pItemRow->SetInset(rcItemInset);
            pItemRow->SetChildPadding(kOptimBuildCellPadding);
            pItemRow->SetVisible(!!bApplicable);
            auto pLabel = std::make_unique<CTextUI>(); // [C++17 R3]
            pLabel->SetText(strDisplay.GetData());
            // [R320] 与开关行同因同解：关掉 AutoCalcWidth 才能让 width=0 的标签
            //   被计入 nAdjustables 并平分剩余宽度。开着它则每行按文字撑宽，
            //   下拉列与开关列的 x 位置逐行漂移（见 OptimBuildSwitchRow 的实测数据）。
            pLabel->SetAttribute(L"width", L"0");
            pLabel->SetTextColor(tokens::Token(tokens::EToken::Ink));   // [FIX 2026-10-25] 统一黑色：原"不支持"条目置橙 0xFFC85A00，与整体不协调
            pLabel->SetAttribute(L"valign", L"vcenter");
            pLabel->SetAttribute(L"endellipsis", L"true");
            pLabel->SetMouseEnabled(false);
            pItemRow->Add(pLabel.release()); // [C++17 R3] ownership transferred to DuiLib
            // [SCOPE-2COL] 列装配下沉到 OptimBuildComboCell（同 switch 行）。
            OptimBuildComboCell(pItemRow.get(), item, g, i);
pBody->Add(pItemRow.release()); // [C++17 R3] ownership transferred to DuiLib
        }
}

void CPageOptim::OptimBuildFinishGroup(CVerticalLayoutUI* pTree, CClickRowUI* pRow, CVerticalLayoutUI* pBody, int nApplicable, bool bExpanded)
{
    RecalcBodyHeight(pBody);
    pTree->Add(pRow);
    pTree->Add(pBody);
    // now hide the body; SetVisible propagates intern visibility
    // to rows, and later SetVisible(true) restores the chain
    // [FIX-UX] bExpanded：首组默认展开（与 m_vExpanded 初始化一致），
    //   箭头字形同步，否则"展开态 + 折叠箭头"自相矛盾。
    pBody->SetVisible(bExpanded);
    if (bExpanded && pRow)
    {
        // body 名形如 optbody_<g>：取 '_' 之后的组号拼出箭头名 optarrow_<g>
        const CDuiString sBody(pBody->GetName());
        const int us = sBody.Find(L"_");
        if (us >= 0)
        {
            CDuiString sArrowName;
            sArrowName.Format(L"optarrow_%s", static_cast<LPCTSTR>(sBody) + us + 1);
            if (CTextUI* pArrow = static_cast<CTextUI*>(
                    pRow->FindSubControl(sArrowName.GetData())))
                pArrow->SetText(m_ctx.HostGlyphArrow(1));
        }
    }
    // [B90 智能加载] 整组没有任何适用于当前系统的项时（如 Win10 下的
    // “Windows 11 专属优化”），连组头一起隐藏：只藏行会留下一个展开后
    // 为空的组标题。组索引仍与 Data.xml 保持一致，切到 Win11 或将来数据
    // 里加入反向（仅 Win10）门控时，构建时自动恢复显示。
    if (nApplicable == 0) pRow->SetVisible(false);
}

// [R230] Re-colour the optimisation tree after a light<->dark switch.
//
// PageBase.h contract: colour attributes only. Calling Build() here would
// pTree->RemoveAll() and lose the collapse state (m_vExpanded) and scroll position.
//
// Structure of optim_tree (verified in OptimBuildFinishGroup):
//   [2g]   = group header row  (CClickRowUI, name optgrp_<g>)
//   [2g+1] = group body        (CVerticalLayoutUI, name optbody_<g>)
// Groups that do not apply to this OS are not added at all, so the index is NOT a
// reliable group number -- walk the tree by position and filter by type instead of
// probing optgrp_<g> names.
//
// Item rows are CHoverRowUI, not CClickRowUI: they paint SurfaceHover on mouse-enter
// and Surface on leave (CustomControls.h:150-153), so setting Surface here matches the
// resting state and does not fight the hover paint. CSwitchUI draws itself from tokens
// on every paint, so it needs no refresh.
void CPageOptim::RefreshThemeColors()
{
    CVerticalLayoutUI* pTree = static_cast<CVerticalLayoutUI*>(
        m_ctx.HostFindControl(L"optim_tree"));
    if (!pTree) return;

    const DWORD dwInk   = tokens::Token(tokens::EToken::Ink);
    const DWORD dwMuted = tokens::Token(tokens::EToken::InkMuted);
    const DWORD dwBase  = tokens::Token(tokens::EToken::Surface);
    const DWORD dwSel   = tokens::Token(tokens::EToken::Accent);
    const DWORD dwOnSel = tokens::Token(tokens::EToken::OnAccent);

    for (int i = 0; i < pTree->GetCount(); ++i)
    {
        CControlUI* pNode = pTree->GetItemAt(i);
        if (!pNode) continue;

        if (CClickRowUI* pGrp = dynamic_cast<CClickRowUI*>(pNode))
        {
            const bool bSel = pGrp->IsRowSelected();
            pGrp->SetBkColor(bSel ? dwSel : dwBase);
            IContainerUI* pHdr = static_cast<IContainerUI*>(
                pNode->GetInterface(DUI_CTR_CONTAINER));
            if (!pHdr) continue;
            for (int c = 0; c < pHdr->GetCount(); ++c)
            {
                CTextUI* pText = dynamic_cast<CTextUI*>(pHdr->GetItemAt(c));
                if (!pText) continue;
                // [F-OPT-5] 折叠箭头（optarrow_g）构建态是 InkMuted，
                //   主题刷新不得把它刷成正文色——否则切一次主题，
                //   箭头会从淡灰突然变成正文黑/白，视觉层级错乱。
                const wchar_t* pszNode = pText->GetName();
                if (pszNode && wcsncmp(pszNode, L"optarrow_", 9) == 0)
                    pText->SetTextColor(dwMuted);
                else
                    pText->SetTextColor(bSel ? dwOnSel : dwInk);
            }
            continue;
        }

        // Group body: walk its item rows.
        IContainerUI* pBody = static_cast<IContainerUI*>(
            pNode->GetInterface(DUI_CTR_CONTAINER));
        if (!pBody) continue;
        for (int r = 0; r < pBody->GetCount(); ++r)
        {
            CControlUI* pRowCtl = pBody->GetItemAt(r);
            if (!pRowCtl) continue;
            if (!dynamic_cast<CHoverRowUI*>(pRowCtl)) continue;   // 只处理条目行
            pRowCtl->SetBkColor(dwBase);

            IContainerUI* pRow = static_cast<IContainerUI*>(
                pRowCtl->GetInterface(DUI_CTR_CONTAINER));
            if (!pRow) continue;
            for (int c = 0; c < pRow->GetCount(); ++c)
            {
                CControlUI* pCell = pRow->GetItemAt(c);
                if (!pCell) continue;
                // The name column is a TextUI; the right-hand cells may hold a dash
                // placeholder (TextUI), a spinner shim, a switch (self-drawn) or a combo.
                if (CComboUI* pCombo = dynamic_cast<CComboUI*>(pCell))
                {
                    pCombo->SetTextColor(dwInk);
                    pCombo->SetBkColor(dwBase);
                    pCombo->SetAttribute(L"bordercolor",
                                         tokens::TokenHex(tokens::EToken::Border));
                    pCombo->SetAttribute(L"itembkcolor",
                                         tokens::TokenHex(tokens::EToken::Surface));
                    pCombo->SetAttribute(L"itemselectedtextcolor",
                                         tokens::TokenHex(tokens::EToken::OnAccent));
                    pCombo->SetAttribute(L"itemhotbkcolor",
                                         tokens::TokenHex(tokens::EToken::SurfaceHover));
                    // [R244] Build 里设了这两个，刷新清单漏了它们 ——
                    //   切换深浅色后下拉的普通项文字与选中项底色仍是旧主题值。
                    //   刷新清单是手写枚举，漏项只能靠"Build 与 Refresh 逐属性对照"发现。
                    pCombo->SetAttribute(L"itemtextcolor",
                                         tokens::TokenHex(tokens::EToken::Ink));
                    pCombo->SetAttribute(L"itemselectedbkcolor",
                                         tokens::TokenHex(tokens::EToken::Accent));
                    continue;
                }
                if (CTextUI* pText = dynamic_cast<CTextUI*>(pCell))
                    pText->SetTextColor(c == 0 ? dwInk : dwMuted);
            }
        }
    }
}
