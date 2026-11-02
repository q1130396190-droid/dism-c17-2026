// ============================================================
// Page_CleanupSelect.cpp —— 清理页「选择族」（从 Page_Cleanup.cpp 拆出）。
//
// [R174] Page_Cleanup.cpp 拆前 1,283 行、规模分 8.96（六页最低）。本文件承接
// 五个选择相关成员 + 一个三态字形辅助，合计 168 行：
//   · ToggleSelectAll       全选/取消全选（逐行翻转复选框）
//   · CleanSelectNone       全不选
//   · CleanSelectDefault    恢复默认勾选（按规则默认值）
//   · OnGroupCheck          组复选框点击 → 组内逐项联动
//   · SyncCleanGroupCheck   组头三态徽标与复选框同步
//   · SetGroupGlyph         组三态字形（0 空 / 1 全选 / 2 半选）
//
// SetGroupGlyph 原为 Page_Cleanup.cpp 匿名命名空间内的副本（Page_Optim.cpp
// 另有一份独立副本，两者语义相同但各自封装，本轮不合并以免跨页耦合），
// 其唯一调用点 SyncCleanGroupCheck 随组迁入，故一并提升到本 TU 文件级。
//
// 闭包说明：五个函数均为 CPageCleanup 成员（声明见 Page_Cleanup.h），
// 依赖面仅两项且都是头文件级：
//   · m_ctx（HostFindControl / HostGlyphBox / CleanEngine）—— 页上下文
//   · m_vCleanRowPass（逐行是否命中的旁路标记，头文件已声明）
// 本段不读 StringMgr、不调 AppendLog、不碰 clean_detail，故无跨 TU 静态依赖。
//
// 逐字搬运，零逻辑改动。
// ============================================================

#include "stdafx.h"

#include "Page_Cleanup.h"

#include "CustomControls.h"        // CCheckBoxUI / CClickRowUI
#include "FrameWnd_Util.h"         // StyleCheckBox / RecalcBodyHeight

using namespace DuiLib;

// [R174] 三态字形的合法状态上界（0 空 / 1 全选 / 2 半选）。
// 原 SetGroupGlyph 内写死 nState >= 0 && nState <= 2，与 HostGlyphBox 的
// 0/1/2 语义靠约定同步；改用具名常量后两侧只有一处真值。
inline constexpr int kCleanGlyphStateMax = 2;

// [R174] 取第 i 行的复选框：行不可见或控件缺失一律返回 nullptr。
// ToggleSelectAll 原在两个循环里各写一遍「取 clnrow_i → 判可见 → 取 ckclean_i」，
// 两份逻辑必须完全一致（否则"是否已有勾选"与"翻转"会作用在不同行集合上）。
// [[nodiscard]] 是刻意的：调用方若丢弃返回值就退化成"什么都没做却继续"，无编译提示。
[[nodiscard]] static CCheckBoxUI* CleanRowCheckbox(IPageContext& ctx, int i)
{
    CDuiString sRow;
    sRow.Format(L"clnrow_%d", i);
    CControlUI* pRow = ctx.HostFindControl(sRow.GetData());
    if (!pRow || !pRow->IsVisible()) return nullptr;
    CDuiString sName;
    sName.Format(L"ckclean_%d", i);
    return static_cast<CCheckBoxUI*>(ctx.HostFindControl(sName.GetData()));
}

// 组复选框三态字形（原 CFrameWnd::SetGroupGlyph，纯控件操作副本，
// m_sGlyphBox 改经宿主访问）：0 空 / 1 全选 / 2 半选
void SetGroupGlyph(IPageContext& ctx, CCheckBoxUI* p, int nState)
{
    if (!p) return;
    int idx = (nState >= 0 && nState <= kCleanGlyphStateMax) ? nState : 0;
    // [BOX-CHECK] 自绘复选框：三态落到控件上（方框/填充/标记由控件自绘），
    //   不再改写文字里的字形前缀 —— 字形方案在选中态是实心块，边框会消失。
    if (auto pBox = dynamic_cast<CBoxCheckUI*>(p))
    {
        pBox->SetBoxState(idx);
        return;
    }
    // 剥掉旧的前导字形（1 字符）+ 双空格，保留组名
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

// C-8: 导出最近一次清理的全量明细（面板仅显示前 200 条，导出含全部）
void CPageCleanup::ToggleSelectAll()

{
    // inspect only rendered, visible rows
    bool bAny = false;
    for (int i = 0; i < m_ctx.CleanEngine().GetRuleCount(); ++i)
    {
        CCheckBoxUI* pCheck = CleanRowCheckbox(m_ctx, i);   // [R174] 查找抽为函数
        if (pCheck && pCheck->GetCheck()) { bAny = true; break; }
    }
    for (int i = 0; i < m_ctx.CleanEngine().GetRuleCount(); ++i)
    {
        CCheckBoxUI* pCheck = CleanRowCheckbox(m_ctx, i);   // [R174] 同一查找口径
        if (pCheck)
        {
            pCheck->SetCheck(!bAny);
            m_ctx.HostRefreshCheckText(pCheck);
        }
    }
    // keep every group checkbox coherent
    for (int g = 0; g < m_ctx.CleanEngine().GetGroupCount(); ++g)
        SyncCleanGroupCheck(g);
}

// [AUDIT-F3] 反选：对当前可见行勾选态取反，再全组三态同步。
// 原右键菜单"反选"项只写日志无动作，此为接入的真实实现；
// 复用 CleanRowCheckbox 的"可见行"口径，与 ToggleSelectAll 严格一致。
void CPageCleanup::CleanSelectInvert()
{
    for (int i = 0; i < m_ctx.CleanEngine().GetRuleCount(); ++i)
    {
        CCheckBoxUI* pCheck = CleanRowCheckbox(m_ctx, i);
        if (pCheck)
        {
            pCheck->SetCheck(!pCheck->GetCheck());
            m_ctx.HostRefreshCheckText(pCheck);
        }
    }
    for (int g = 0; g < m_ctx.CleanEngine().GetGroupCount(); ++g)
        SyncCleanGroupCheck(g);
}

// 全不选： clear every item and group checkbox
void CPageCleanup::CleanSelectNone()
{
    for (int i = 0; i < m_ctx.CleanEngine().GetRuleCount(); ++i)
    {
        CDuiString sName;
        sName.Format(L"ckclean_%d", i);
        CCheckBoxUI* pCheck = static_cast<CCheckBoxUI*>(m_ctx.HostFindControl(sName.GetData()));
        if (pCheck) { pCheck->SetCheck(false); m_ctx.HostRefreshCheckText(pCheck); }
    }
    for (int g = 0; g < m_ctx.CleanEngine().GetGroupCount(); ++g)
    {
        CDuiString sName;
        sName.Format(L"grpck_%d", g);
        CCheckBoxUI* pCheck = static_cast<CCheckBoxUI*>(m_ctx.HostFindControl(sName.GetData()));
        if (pCheck) { pCheck->SetCheck(false); m_ctx.HostRefreshCheckText(pCheck); }
    }
}

// restore the page's default selection: same state the page is built with
// [E-5 极性修复 2026-10-07] Level 0/1 高风险不勾，Level 2/3 常规安全勾选
// （Data.xml 实测 Level 0=Installer/CompactOs/硬链接等 17 项高风险）
void CPageCleanup::CleanSelectDefault()

{
    for (int i = 0; i < m_ctx.CleanEngine().GetRuleCount(); ++i)
    {
        CDuiString sName;
        sName.Format(L"ckclean_%d", i);
        CCheckBoxUI* pCheck = static_cast<CCheckBoxUI*>(m_ctx.HostFindControl(sName.GetData()));
        if (pCheck)
        {
            pCheck->SetCheck(m_ctx.CleanEngine().GetRule(i).nLevel >= 2);
            m_ctx.HostRefreshCheckText(pCheck);
        }
    }
    // 组复选框按各行状态做三态同步（含 L0/L1 项的组应为半选态）
    for (int g = 0; g < m_ctx.CleanEngine().GetGroupCount(); ++g)
        SyncCleanGroupCheck(g);
}

// [U-9 已移除] 定期清理计划任务（SaveCleanSchedule / DeleteCleanSchedule）—— 界面不再提供该入口

// show the selected rule's description in the right panel
void CPageCleanup::OnGroupCheck(int iGroup, bool bChecked)

{
    CDuiString strGroup;
    if (!m_ctx.CleanEngine().GetGroup(iGroup, strGroup)) return;
    // [修复 #20] 数据态数组兜底
    int nRules = m_ctx.CleanEngine().GetRuleCount();
    if (static_cast<int>(m_vCleanRowPass.size()) != nRules) m_vCleanRowPass.assign(nRules, 1);
    for (int i = 0; i < nRules; ++i)
    {
        CleanRule& rule = m_ctx.CleanEngine().GetRule(i);
        if (rule.sGroup != strGroup) continue;
        // [修复 #20] 按“过滤通过”数据态联动，而不是控件 IsVisible()：
        // 组处于折叠态时 body 内所有行 IsVisible()==false，旧逻辑导致
        // 折叠组的组复选框勾选/取消对子项完全不生效。被关键字/零字节
        // 过滤掉的行（m_vCleanRowPass==0）仍保持原状不动。
        if (!m_vCleanRowPass[i]) continue;
        CDuiString sName;
        sName.Format(L"ckclean_%d", i);
        CCheckBoxUI* pCheck = static_cast<CCheckBoxUI*>(
            m_ctx.HostFindControl(sName.GetData()));
        if (pCheck)
        {
            pCheck->SetCheck(bChecked);
            m_ctx.HostRefreshCheckText(pCheck);
        }
    }
}

// reflect the items' check state in the group checkbox:
// [修复 #20] 按“过滤通过”数据态统计（含折叠组内不可见的行），
// 折叠时组框也能显示真实的全选/半选/空三态。
void CPageCleanup::SyncCleanGroupCheck(int iGroup)

{
    CDuiString strGroup;
    if (!m_ctx.CleanEngine().GetGroup(iGroup, strGroup)) return;
    CDuiString sGrpName;
    sGrpName.Format(L"grpck_%d", iGroup);
    CCheckBoxUI* pGrp = static_cast<CCheckBoxUI*>(
        m_ctx.HostFindControl(sGrpName.GetData()));
    if (!pGrp) return;
    int nRules = m_ctx.CleanEngine().GetRuleCount();
    if (static_cast<int>(m_vCleanRowPass.size()) != nRules) m_vCleanRowPass.assign(nRules, 1);
    bool bAnyPass = false;
    bool bAllChecked = true;
    for (int i = 0; i < nRules; ++i)
    {
        CleanRule& rule = m_ctx.CleanEngine().GetRule(i);
        if (rule.sGroup != strGroup) continue;
        if (!m_vCleanRowPass[i]) continue;   // 被过滤掉的行不参与三态
        bAnyPass = true;
        CDuiString sName;
        sName.Format(L"ckclean_%d", i);
        CCheckBoxUI* pCheck = static_cast<CCheckBoxUI*>(
            m_ctx.HostFindControl(sName.GetData()));
        if (!pCheck || !pCheck->GetCheck()) bAllChecked = false;
    }
    // [修复 R5 / P1-6] 三元汇总改为三态字形：0 空 / 1 半选(▣) / 2 全选(☑)
    int nGlyphState = !bAnyPass ? 0 : (bAllChecked ? 2 : 1);
    pGrp->SetCheck(nGlyphState == 2);
    SetGroupGlyph(m_ctx, pGrp, nGlyphState);
}
