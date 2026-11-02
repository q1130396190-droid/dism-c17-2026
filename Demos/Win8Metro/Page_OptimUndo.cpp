// ============================================================
// Page_OptimUndo.cpp —— 优化页「撤销 / 重做 / 全局快捷键」（从 Page_Optim.cpp 拆出）。
//
// [R169] Page_Optim.cpp 拆前 1,312 行、规模分 8.85；本文件承接五个函数：
//   · PushOptimUndo      —— 压栈（上限 32）+ 清空重做栈
//   · UndoLastOptim      —— Ctrl+Z：失败回滚记录、成功转入重做栈
//   · OnGlobalRedo       —— Ctrl+Y：把最近一次撤销重新应用
//   · OnGlobalSelectAll  —— Ctrl+A：全量应用
//   · OnGlobalSave       —— Ctrl+S：导出配置
//
// 闭包说明：五个函数均为 CPageOptim 成员（声明见 Page_Optim.h），
// 依赖面全部为头文件级接口：
//   · m_ctx / m_vOptimUndo / m_vOptimRedo / m_dwOptimVerifyTick /
//     m_bFiltering / m_sOptimFindText（成员，头文件已声明）
//   · SOptimUndo（Page_Optim.h 内的成员结构体）
//   · AppendLog / SyncOptimGroupGlyph / ApplyFindFilter / BulkApplyAllOptim /
//     ExportOptimConfig（成员函数，头文件已声明）
//   · optim_detail::FindOptimItem（Page_OptimInternal.h）
//   · ToolSql::HostNotify（NotifyBox.h）、StringMgr、ELogLevel（LogService.h）
// 原文件不残留 file-scope 静态实体，跨 TU 无静态依赖。
// ============================================================

#include "stdafx.h"

#include "Page_Optim.h"
#include "Page_OptimInternal.h"   // optim_detail::FindOptimItem

#include "StringManager.h"
#include "NotifyBox.h"            // ToolSql::HostNotify
#include "LogService.h"           // ELogLevel

using i18n::CStringManager;

// 主文件经匿名命名空间的 using 声明引入；拆出后必须显式引入，否则 C3861。
using optim_detail::FindOptimItem;

// [R169] 撤销栈深度上限。原为 PushOptimUndo 与 OnGlobalRedo 各写一处裸字面量
// 32（两处必须同步，否则两条路径的栈容量会不一致）。改用具名常量后只有一处真值。
inline constexpr size_t kOptimUndoDepth = 32;

void CPageOptim::PushOptimUndo(int g, int i, int nScope, bool bBefore)

{
    SOptimUndo u;
    u.g = g; u.i = i; u.nScope = nScope; u.bBefore = bBefore;
    m_vOptimUndo.push_back(u);
    if (m_vOptimUndo.size() > kOptimUndoDepth) m_vOptimUndo.erase(m_vOptimUndo.begin());
    m_vOptimRedo.clear();   // [R-B1] 新动作后清空 Redo 栈
}

// [F-OPT-1] 下拉变更入撤销栈：与开关同栈同上限，Ctrl+Z/Ctrl+Y 按同一条
// 时间线回退/重做，不再出现"刚改了下拉、Ctrl+Z 却回退到更早的开关"的错位。
// 只在 nBefore >= 0（系统本有配置档位）时由调用方入栈——"未配置"无档位可回写。
void CPageOptim::PushOptimUndoCombo(int g, int i, int nScope, int nBefore, int nAfter)
{
    SOptimUndo u;
    u.g = g; u.i = i; u.nScope = nScope;
    u.bBefore = false;
    u.bIsCombo = true;
    u.nComboBefore = nBefore;
    u.nComboAfter = nAfter;
    m_vOptimUndo.push_back(u);
    if (m_vOptimUndo.size() > kOptimUndoDepth) m_vOptimUndo.erase(m_vOptimUndo.begin());
    m_vOptimRedo.clear();   // 新动作后清空 Redo 栈（与 PushOptimUndo 同协议）
}

void CPageOptim::UndoLastOptim()

{
    if (m_vOptimUndo.empty())
    {
        ToolSql::HostNotify(m_ctx.HostHwnd(), StringMgr.Get("optim.sb8e87cdb"), StringMgr.Get("misc.sbd9fcf46"), MB_OK | MB_ICONINFORMATION);
        return;
    }
    // [FIX-UNDO] 先 peek 校验再 pop：旧实现 pop 后才发现节点失效直接 return，
    //   记录被静默消耗、也没进重做栈，Ctrl+Z 链出现"空洞"——再按一次会跳过
    //   它去撤销更早的操作，栈与真实状态从此错位（数据 XML 热替换后必然命中）。
    //   节点失效时保留记录并提示，重载数据后仍可继续撤销。
    const SOptimUndo u = m_vOptimUndo.back();
    CMarkupNode root = m_ctx.DataEngine().GetOptimizationRoot();
    CMarkupNode node = FindOptimItem(root, u.g, u.i);
    if (!node.IsValid())
    {
        m_ctx.HostLog(L"[optim] undo skipped: rule node not found (undo record kept)",
                      static_cast<int>(ELogLevel::LOG_WARN));
        return;
    }
    m_vOptimUndo.pop_back();
    std::vector<CDuiString> vOut;
    int nUnsupported = 0;
    bool bRestart = false;
    // [F-OPT-1] 下拉记录走 SelectScopeDropdown 回写撤销档位；开关仍走 SetScopeState
    bool bOk = false;
    if (u.bIsCombo)
        bOk = m_ctx.DataEngine().SelectScopeDropdown(node, u.nScope, u.nComboBefore,
                                                     vOut, nUnsupported, &bRestart);
    else
        bOk = m_ctx.DataEngine().SetScopeState(node, u.nScope, u.bBefore,
                                               vOut, nUnsupported, &bRestart);
    if (!bOk)
    {
        // [UNDO-STACK] 撤销失败时把记录放回栈顶：原实现直接 return 丢掉了
        // 这条记录，用户再按一次 Ctrl+Z 会跳过它去撤销更早的操作，栈与
        // 真实状态从此错位。放回后重试或换个时机再撤销仍然有效。
        m_vOptimUndo.push_back(u);
        m_ctx.HostLog(L"[optim] undo failed", static_cast<int>(ELogLevel::LOG_WARN));
        ToolSql::HostNotify(m_ctx.HostHwnd(), StringMgr.Get("error.sed23e2e1"), StringMgr.Get("misc.sbd9fcf46"), MB_OK | MB_ICONWARNING);
        return;
    }
    // [UNDO-STACK] 撤销成功 → 进入重做栈。原实现从不写 m_vOptimRedo，
    // Ctrl+Y 因此恒为空栈（界面上有重做菜单项却永远不可用）。
    m_vOptimRedo.push_back(u);
    // [F-OPT-1] 回写撤销后的控件视觉：下拉补 SelectItem+SetText（CComboUI
    // 画自己的 m_sText），开关补 SetOn；随后共用组三态/缓存/过滤/重启链。
    if (u.bIsCombo)
    {
        CDuiString sCbuName;
        sCbuName.Format(L"cbu_%d_%d_%d", u.g, u.i, u.nScope);
        CComboUI* pCombo = static_cast<CComboUI*>(m_ctx.HostFindControl(sCbuName.GetData()));
        if (pCombo)
        {
            pCombo->SelectItem(u.nComboBefore, false);
            CDuiString sDropName;
            if (m_ctx.DataEngine().GetScopeDropdownName(node, u.nScope, u.nComboBefore, sDropName))
                pCombo->SetText(sDropName.GetData());
            pCombo->Invalidate();
        }
    }
    else
    {
        CDuiString sSwName;
        sSwName.Format(L"sw_%d_%d_%d", u.g, u.i, u.nScope);
        CSwitchUI* pSw = static_cast<CSwitchUI*>(m_ctx.HostFindControl(sSwName.GetData()));
        int nAfter = m_ctx.DataEngine().GetScopeState(node, u.nScope);
        if (nAfter >= 0 && pSw) pSw->SetOn(nAfter == 1, false);
    }
    if (u.g >= 0) SyncOptimGroupGlyph(u.g);
    InvalidateOptimVerifyCache();   // [VERIFY-TTL] 写入后让 5 秒核实缓存失效，否则下次强制核实被直接跳过
    // [FILTER-STAY] 撤销同样会改变状态过滤下的可见集合
    if (m_bFiltering) ApplyFindFilter(m_sOptimFindText);
    // [G8 2026-10-25] 消费 bRestart：原先取到后直接丢弃，导致"撤销了但界面没变"
    //   （该项要重启 explorer 才生效），用户会以为撤销失败而反复按 Ctrl+Z，
    //   把栈一路退穿。这里与"恢复默认"路径走同一个询问。
    OcmPromptRestartIfNeeded(bRestart);
    // [F-OPT-1] 撤销日志区分开关/下拉
    if (u.bIsCombo)
    {
        CDuiString sLog;
        sLog.Format(L"[optim] undo cbu_%d_%d_%d -> combo:%d",
                    u.g, u.i, u.nScope, u.nComboBefore);
        AppendLog(sLog.GetData());
    }
    else
    {
        CDuiString sLog;
        sLog.Format(L"[optim] undo sw_%d_%d_%d -> %s",
                    u.g, u.i, u.nScope, u.bBefore ? L"ON" : L"OFF");
        AppendLog(sLog.GetData());
    }
}
// ============================================================

// [R-B1] Ctrl+Y Redo：把最近一次撤销的操作重新应用

// ============================================================

void CPageOptim::OnGlobalRedo()

{
    if (m_vOptimRedo.empty())
    {
        AppendLog(L"[optim] redo stack empty");
        m_ctx.HostToast(StringMgr.Get("optim.s_redo_empty"),
                        StringMgr.Get("optim.s_redo_empty_body"), 1);
        return;
    }
    // [FIX-UNDO] 同 Undo：先 peek 校验再 pop，节点失效时保留重做记录。
    const SOptimUndo u = m_vOptimRedo.back();
    CMarkupNode root = m_ctx.DataEngine().GetOptimizationRoot();
    CMarkupNode node = FindOptimItem(root, u.g, u.i);
    if (!node.IsValid())
    {
        m_ctx.HostLog(L"[optim] redo skipped: rule node not found (redo record kept)",
                      static_cast<int>(ELogLevel::LOG_WARN));
        return;
    }
    m_vOptimRedo.pop_back();
    // redo = 把状态设回撤销前（!bBefore）
    // [F-OPT-1] 下拉记录重做档位 = 用户当时实际选中的 nComboAfter
    bool bNewOn = !u.bBefore;
    std::vector<CDuiString> vOut;
    int nUnsupported = 0;
    bool bRestart = false;
    bool bOk = false;
    if (u.bIsCombo)
        bOk = m_ctx.DataEngine().SelectScopeDropdown(node, u.nScope, u.nComboAfter,
                                                     vOut, nUnsupported, &bRestart);
    else
        bOk = m_ctx.DataEngine().SetScopeState(node, u.nScope, bNewOn,
                                               vOut, nUnsupported, &bRestart);
    if (!bOk)
    {
        // [UNDO-STACK] 重做失败同样放回栈顶，避免"操作被吞掉"。
        m_vOptimRedo.push_back(u);
        m_ctx.HostLog(L"[optim] redo failed", static_cast<int>(ELogLevel::LOG_WARN));
        ToolSql::HostNotify(m_ctx.HostHwnd(), StringMgr.Get("error.sed23e2e1"), StringMgr.Get("misc.sbd9fcf46"), MB_OK | MB_ICONWARNING);
        return;
    }
    // [UNDO-STACK] 重做成功 → 回填撤销栈，使 Ctrl+Z / Ctrl+Y 可反复往返。
    // 直接 push_back 而不走 PushOptimUndo：后者会清空重做栈。
    m_vOptimUndo.push_back(u);
    if (m_vOptimUndo.size() > kOptimUndoDepth) m_vOptimUndo.erase(m_vOptimUndo.begin());
    // [F-OPT-1] 下拉/开关分别回写控件视觉（同 UndoLastOptim）
    if (u.bIsCombo)
    {
        CDuiString sCbuName;
        sCbuName.Format(L"cbu_%d_%d_%d", u.g, u.i, u.nScope);
        CComboUI* pCombo = static_cast<CComboUI*>(m_ctx.HostFindControl(sCbuName.GetData()));
        if (pCombo)
        {
            pCombo->SelectItem(u.nComboAfter, false);
            CDuiString sDropName;
            if (m_ctx.DataEngine().GetScopeDropdownName(node, u.nScope, u.nComboAfter, sDropName))
                pCombo->SetText(sDropName.GetData());
            pCombo->Invalidate();
        }
    }
    else
    {
        CDuiString sSwName;
        sSwName.Format(L"sw_%d_%d_%d", u.g, u.i, u.nScope);
        CSwitchUI* pSw = static_cast<CSwitchUI*>(m_ctx.HostFindControl(sSwName.GetData()));
        int nAfter = m_ctx.DataEngine().GetScopeState(node, u.nScope);
        if (nAfter >= 0 && pSw) pSw->SetOn(nAfter == 1, false);
    }
    if (u.g >= 0) SyncOptimGroupGlyph(u.g);
    InvalidateOptimVerifyCache();   // [VERIFY-TTL] 写入后让 5 秒核实缓存失效，否则下次强制核实被直接跳过
    if (m_bFiltering) ApplyFindFilter(m_sOptimFindText);
    // [G8] 重做同样要消费 bRestart，理由同 Undo。
    OcmPromptRestartIfNeeded(bRestart);
    // [F-OPT-1] 重做日志区分开关/下拉
    if (u.bIsCombo)
    {
        CDuiString sLog;
        sLog.Format(L"[optim] redo cbu_%d_%d_%d -> combo:%d",
                    u.g, u.i, u.nScope, u.nComboAfter);
        AppendLog(sLog.GetData());
    }
    else
    {
        CDuiString sLog;
        sLog.Format(L"[optim] redo sw_%d_%d_%d -> %s",
                    u.g, u.i, u.nScope, bNewOn ? L"ON" : L"OFF");
        AppendLog(sLog.GetData());
    }
}

// ============================================================

// [R-B1] Ctrl+A：全选优化页所有已启用项

// ============================================================

void CPageOptim::OnGlobalSelectAll()

{
    BulkApplyAllOptim();
    AppendLog(L"[optim] Ctrl+A bulk apply all");
}

// ============================================================

// [R-B1] Ctrl+S：导出当前优化配置

// ============================================================

void CPageOptim::OnGlobalSave()

{
    ExportOptimConfig();
    AppendLog(L"[optim] Ctrl+S export config");
}
