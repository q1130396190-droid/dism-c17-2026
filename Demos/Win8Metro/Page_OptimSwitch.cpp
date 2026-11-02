// ============================================================
// Page_OptimSwitch.cpp —— 优化页「开关与下拉交互族」（从 Page_Optim.cpp 拆出）。
//
// [R178] Page_Optim.cpp 拆前 1,230 行、规模分 9.10（六页末位）。本文件承接
// 八个成员 + 一个文件级辅助，合计 268 行，边界按「单个开关/下拉的确认→应用→收尾」
// 划分：
//   · OcmConfirmSwitch     开关确认（Warning 与提权说明合并到一个框）
//   · OcmFinishSwitch      开关应用后的 UI 同步与失败回执
//   · OnSwitchToggle       开关点击编排（解析 → 确认 → 应用 → 收尾）
//   · AppendPrivilegeNote  提权说明追加到确认文案
//   · BuildApplyFailMessage 失败提示文案拼装
//   · OcmConfirmCombo      下拉确认
//   · OcmFinishCombo       下拉应用后的同步
//   · OnScopeCombo         作用域下拉点击编排
//   · OptimRollbackCombo   下拉回滚到原选中项（-1 = 未配置）
//
// 闭包说明：八个成员声明见 Page_Optim.h；OptimRollbackCombo 原为
// Page_Optim.cpp 匿名命名空间内的 static，调用点（1034/1054/1070）全部
// 在本段内，故随段迁入并保留在本 TU 的匿名命名空间里。
// 跨 TU 依赖仅四项且都是头文件级：
//   · m_ctx / m_setPrivWarned（成员）
//   · StringMgr（optim.* 语言键，键名逐字保持，未新增）
//   · ToolSql::HostConfirm / HostNotify（NotifyBox.h）
//   · CollectWarningTexts（optim_detail）、ParseSwName（FrameWnd_UiHelpers）、
//     AppendLog（成员）
//
// 逐字搬运，零逻辑改动。
// ============================================================

#include "stdafx.h"

#include "Page_Optim.h"
#include "Page_OptimInternal.h"   // optim_detail::FindOptimItem / CollectWarningTexts

#include "StringManager.h"
#include "NotifyBox.h"            // ToolSql::HostConfirm / HostNotify
#include "FrameWnd_UiHelpers.h"   // ParseSwName
#include "CustomControls.h"       // CSwitchUI / CComboUI
#include "ExternalToolService.h"  // IsElevated（提权前置判定）
#include "LogService.h"           // ELogLevel

using namespace DuiLib;
using i18n::CStringManager;

// 主文件经匿名命名空间的 using 声明引入；拆出后必须显式引入（R69/R178 同类）。
using optim_detail::FindOptimItem;
using optim_detail::CollectWarningTexts;

// [R178] 作用域开关的"已开启"取值。GetScopeState 返回 -1 表示该项在该作用域
// 未配置、0 关闭、1 开启；本段两处 `== 1` 都是这个语义，改用具名常量后
// 与 -1 的"未配置"分支形成对照，避免后人把 1 误当成某个计数。
inline constexpr int kOptimScopeSwitchOn = 1;

// [R178] CollectWarningTexts 的"不限动作序号"参数。该函数第 5 参是动作下标，
// 传 -1 表示收集该作用域下全部 Warning（本段两处调用都是全量收集）。
inline constexpr int kOptimAllActions = -1;

// [R152] OnSwitchToggle 拆为三段（原 87 行 / CC20）。
// 确认段与收尾段各自独立成函数，主函数只保留「解析 → 确认 → 应用 → 收尾」编排。
// 三段之间只通过显式参数传递状态，不依赖隐式顺序。
//
// 确认段：Warning 与提权说明合并到同一个确认框。
// [STAB] 旧实现先弹"需要管理员权限"、再弹 Warning 确认，用户为一次开关要连点
// 两次"是"；而且第一次选"是"后第二次选"否"时，提权提示已被记入 m_setPrivWarned
// （消耗掉），却不曾应用任何改动——状态与提示不一致。
// 返回 false 表示用户取消（调用方须把开关控件回滚到 bCurrent）。
bool CPageOptim::OcmConfirmSwitch(CMarkupNode node, int nScope, bool bOn)
{
    std::vector<CDuiString> warnings;
    CollectWarningTexts(m_ctx.DataEngine(), node, nScope, bOn, kOptimAllActions, warnings);
    // 无论有无 Warning，先把提权说明准备好（有则并入下方确认框）
    CDuiString sPrivNote;
    const bool bNeedPriv = AppendPrivilegeNote(node, sPrivNote);
    if (warnings.empty() && !bNeedPriv) return true;   // 无提示：直接放行
    CDuiString sMsg;
    for (size_t k = 0; k < warnings.size(); ++k)
    {
        sMsg += warnings[k];
        sMsg += L"\r\n";
    }
    sMsg += sPrivNote;   // [STAB] 提权说明（可能为空）
    sMsg += StringMgr.Get("confirm.s9a739f95");
    return ToolSql::HostConfirm(m_ctx.HostHwnd(), sMsg.GetData(),
                                StringMgr.Get("general.confirm"),
                                MB_YESNO, MB_ICONQUESTION) == IDYES;
}

// 收尾段：重启询问 + 回读核实 + 分组三态 + 让核实缓存失效 + 重放过滤。
void CPageOptim::OcmFinishSwitch(CMarkupNode node, int g, int nScope,
                                 const CDuiString& sItemName, CSwitchUI* pSw,
                                 bool bOn, int nUnsupported, bool bRestart)
{
    CDuiString sLog;
    sLog.Format(L"[optim] %s: [%s] %s%s",
                bOn ? L"ON" : L"OFF",
                m_ctx.DataEngine().ScopeName(nScope), sItemName.GetData(),
                nUnsupported > 0 ? L" (some actions unsupported)" : L"");
    AppendLog(sLog.GetData());
    // restart explorer prompt
    if (bRestart)
    {
        CDuiString sQ;
        sQ.Format(StringMgr.Get("appx.s036a390a"),
                  m_ctx.DataEngine().ScopeName(nScope), sItemName.GetData());
        if (ToolSql::HostConfirm(m_ctx.HostHwnd(), sQ.GetData(),
                                 StringMgr.Get("toolbox.restart_expl"),
                                 MB_YESNO, MB_ICONQUESTION) == IDYES)
        {
            ::PostMessageW(m_ctx.HostHwnd(), WM_HOST_REQUEST_RESTART_EXPLORER, 0, 0);
        }
    }
    // [CRASHFIX] 任何 MessageBox 返回后，队列里残留的是弹层关闭/陈旧输入
    //   消息；旧实现用全量 PumpUiMessages 在点击栈深处把它们派发出去，
    //   半销毁的 DuiLib 弹层收到陈旧消息 → UAF/CFG 崩溃（任务2实证路径）。
    //   改为 SettleAfterDialog：陈旧输入定向丢弃、只派发绘制/外观消息，
    //   重启请求等业务消息留在队列由主循环栈外处理。
    optim_detail::SettleAfterDialog();
    // re-verify the real state so UI can never disagree with the system
    const int nAfter = m_ctx.DataEngine().GetScopeState(node, nScope);
    if (nAfter >= 0 && pSw) pSw->SetOn(nAfter == kOptimScopeSwitchOn, false);
    // O-2: 开关变化后回算分组三态
    if (g >= 0) SyncOptimGroupGlyph(g);
    InvalidateOptimVerifyCache();   // [VERIFY-TTL] 写入后让 5 秒核实缓存失效，否则下次强制核实被直接跳过
    // [FILTER-STAY] 筛选/状态过滤生效时，本次开关可能已让该行不再满足条件
    // （如"仅未启用"下把项打开）。重放一次过滤让列表立即收敛，否则该行会
    // 停留在不符合当前筛选的位置，直到用户动一下搜索框才消失。
    if (m_bFiltering) ApplyFindFilter(m_sOptimFindText);
}

void CPageOptim::OnSwitchToggle(const wchar_t* pszName, bool bOn)
{
    int g = -1, i = -1, nScope = -1;
    if (!ParseSwName(pszName, L"sw_", g, i, nScope)) return;
    CMarkupNode root = m_ctx.DataEngine().GetOptimizationRoot();
    CMarkupNode node = FindOptimItem(root, g, i);
    if (!node.IsValid()) return;
    CDuiString sSwName;
    sSwName.Format(L"sw_%d_%d_%d", g, i, nScope);
    CSwitchUI* pSw = static_cast<CSwitchUI*>(m_ctx.HostFindControl(sSwName.GetData()));
    CDuiString sItemName = m_ctx.DataEngine().ResolveText(node.GetAttributeValue(L"Name"));
    // real state before the change (used to roll the UI back)
    const int nBefore = m_ctx.DataEngine().GetScopeState(node, nScope);
    const bool bCurrent = nBefore == kOptimScopeSwitchOn;
    if (bOn == bCurrent) return;
    if (!OcmConfirmSwitch(node, nScope, bOn))
    {
        // [CRASHFIX] 确认框返回后先稳定队列再回滚控件（同 OcmFinishSwitch）
        optim_detail::SettleAfterDialog();
        if (pSw) pSw->SetOn(bCurrent, false);
        return;
    }
    // apply for real
    std::vector<CDuiString> vOut;
    int nUnsupported = 0;
    bool bRestart = false;
    PushOptimUndo(g, i, nScope, bCurrent);   // [R19] Ctrl+Z 撤销栈
    const bool bOk = m_ctx.DataEngine().SetScopeState(node, nScope, bOn, vOut,
                                                      nUnsupported, &bRestart);
    if (!bOk)
    {
        // 应用失败：刚入栈的记录无实际变更，弹出避免撤销到无效步
        if (!m_vOptimUndo.empty()) m_vOptimUndo.pop_back();
        CDuiString sMsg;
        BuildApplyFailMessage(node, nScope, sItemName, sMsg);
        ToolSql::HostNotify(m_ctx.HostHwnd(), sMsg.GetData(),
                            StringMgr.Get("optim.settings"), MB_OK | MB_ICONWARNING);
        // [CRASHFIX] 失败提示框返回后先稳定队列再回滚控件
        optim_detail::SettleAfterDialog();
        if (pSw) pSw->SetOn(bCurrent, false);
        return;
    }
    OcmFinishSwitch(node, g, nScope, sItemName, pSw, bOn, nUnsupported, bRestart);
}

// [R146] 应用失败的提示文案装配（从 OnSwitchToggle 抽出）。
// 该段含 4 个嵌套判定（Privilege 有无 / PrivilegeNote 有无），抽出后
// OnSwitchToggle 主流程只保留"失败 → 弹框 → 回滚 UI"三步。
// [Privilege] 把 Data_Win11.xml 自带的 PrivilegeNote 拼进失败提示：
// 该属性在数据里出现 228 次（217 Admin / 11 System），每项都写清了
// "为什么写不动、要怎么办"；原实现完全不读，用户只看到笼统的
// "应用失败"，无从判断是权限问题还是规则不适用。
void CPageOptim::BuildApplyFailMessage(CMarkupNode node, int nScope,
                                       const CDuiString& sItemName, CDuiString& sMsg)
{
    sMsg.Format(StringMgr.Get("error.s337e4fd6"),
                m_ctx.DataEngine().ScopeName(nScope), sItemName.GetData());
    const wchar_t* pszPriv = node.GetAttributeValue(L"Privilege");
    if (pszPriv == nullptr || *pszPriv == L'\0') return;
    sMsg += L"\r\n\r\n";
    const wchar_t* pszNote = node.GetAttributeValue(L"PrivilegeNote");
    if (pszNote != nullptr && *pszNote != L'\0')
    {
        sMsg += m_ctx.DataEngine().ResolveText(pszNote);
        return;
    }
    // [R160] 原为拼接中文字面量，且用 += 直接塞入 pszPriv 指针；
    //   改为单条语言包键 + Format，%s 只吃"权限名"这一处变量。
    CDuiString sPrivMsg;
    sPrivMsg.Format(StringMgr.Get("optim.privilege_required"), pszPriv);
    sMsg += sPrivMsg;
}

// [STAB] 只查询不弹窗：把提权说明追加到调用方的 sMsg。
// 用途：该项同时带 Warning 时，把两条提示合并进同一个确认框，
// 避免用户为一次开关连续点两次"是"（旧行为还会出现"提权框选了是、
// 警告框选了否"导致提示已消耗但未应用的状态）。
[[nodiscard]] bool CPageOptim::AppendPrivilegeNote(CMarkupNode item, CDuiString& sMsg)
{
    const wchar_t* pszPriv = item.GetAttributeValue(L"Privilege");
    if (pszPriv == nullptr || *pszPriv == L'\0') return false;
    if (lstrcmpiW(pszPriv, L"Admin") != 0) return false;
    if (ExternalToolService::IsElevated()) return false;
    const wchar_t* pszName = item.GetAttributeValue(L"Name");
    std::wstring sKey = pszName ? pszName : L"";
    // [G7 2026-10-25] 原实现在同一会话内对同一条目只提示一次，第二次起静默直通。
    //   问题不是"提示太多"，而是"行为前后不一致"：用户第一次看到"可能写不进去"，
    //   第二次却什么都不说就执行了，失败时完全无从判断原因。
    //   改为每次都提示，但仍并入调用方的同一个确认框（AppendPrivilegeNote 只追加
    //   文本、不单独弹窗），所以不会出现"一次操作连点两次是"的老问题。
    //   m_setPrivWarned 保留写入，供后续统计/诊断使用，不再作为抑制条件。
    m_setPrivWarned.insert(sKey);
    sMsg += L"\r\n\r\n";
    sMsg += StringMgr.Get("optim.admin_item_note");
    const wchar_t* pszNote = item.GetAttributeValue(L"PrivilegeNote");
    if (pszNote != nullptr && *pszNote != L'\0')
    {
        sMsg += L"\r\n";
        sMsg += m_ctx.DataEngine().ResolveText(pszNote);
    }
    return true;
}

// [R152] OnScopeCombo 的回滚与收尾（原 83 行 / CC17）。
// 下拉有 3 处「回滚到原选中项」重复代码，抽成一个函数；
// 确认段与收尾段同 OnSwitchToggle 的思路独立成函数。
namespace
{
// 把下拉恢复到 nCurrentSel（-1 表示"未配置"）。
// [OPTIM-BOX] 两处关键：
//   ① -1 时**不能**用 SelectItem(-1) 清空：DuiLib 的 SelectItem 遇负数
//      会先把当前选中清成 -1 再返回 false（UICombo.cpp:352-359），
//      于是用户刚点选的项从框里消失、框变成空的带边框方框 ——
//      "改完之后框空了"就是这么来的。写入已经成功（bOk），系统侧只是回读不匹配
//      （可能需要重启、或该机是第三种状态），此时以用户选择为准。
//   ② CComboUI 画的是自己的 m_sText，SelectItem 不同步文字 —— 每次选中
//      都必须补一次 SetText，否则框里是空的。
void OptimRollbackCombo(IPageContext& ctx, CMarkupNode item, int nScope,
                         CComboUI* pCombo, int nCurrentSel)
{
    if (!pCombo) return;
    auto setText = [&](int nSel)
    {
        CDuiString sName;
        if (ctx.DataEngine().GetScopeDropdownName(item, nScope, nSel, sName))
            pCombo->SetText(sName.GetData());
    };
    if (nCurrentSel >= 0)
    {
        pCombo->SelectItem(nCurrentSel, false);
        setText(nCurrentSel);
        pCombo->Invalidate();
        return;
    }
    const int nKeep = pCombo->GetCurSel();
    if (nKeep >= 0) return;      // 保留用户刚选的项（文字已写好）
    if (pCombo->GetCount() > 0)  // 实在没有选中项：落到第一个候选项，别留空框
    {
        pCombo->SelectItem(0, false);
        setText(0);
        pCombo->Invalidate();
        return;
    }
    pCombo->Invalidate();
}
}   // namespace

// 确认段：Warning 汇总后并入同一个确认框；返回 false 表示用户取消。
bool CPageOptim::OcmConfirmCombo(CMarkupNode node, int nScope, int iSel)
{
    std::vector<CDuiString> warnings;
    CollectWarningTexts(m_ctx.DataEngine(), node, nScope, false, iSel, warnings);
    if (warnings.empty()) return true;
    CDuiString sMsg;
    for (size_t k = 0; k < warnings.size(); ++k)
    {
        sMsg += warnings[k];
        sMsg += L"\r\n";
    }
    sMsg += StringMgr.Get("confirm.s9a739f95");
    return ToolSql::HostConfirm(m_ctx.HostHwnd(), sMsg.GetData(),
                                StringMgr.Get("general.confirm"),
                                MB_YESNO, MB_ICONQUESTION) == IDYES;
}

// 收尾段：日志 + 重启询问 + 回读核实（-1 时清空下拉选中态并重绘）。
void CPageOptim::OcmFinishCombo(CMarkupNode node, int nScope, const CDuiString& sItemName,
                                CComboUI* pCombo, int iSel, int nUnsupported, bool bRestart)
{
    CDuiString sLog;
    sLog.Format(L"[optim] select %d: [%s] %s%s", iSel,
                m_ctx.DataEngine().ScopeName(nScope), sItemName.GetData(),
                nUnsupported > 0 ? L" (some actions unsupported)" : L"");
    AppendLog(sLog.GetData());
    if (bRestart)
    {
        CDuiString sQ;
        sQ.Format(StringMgr.Get("appx.s036a390a"),
                  m_ctx.DataEngine().ScopeName(nScope), sItemName.GetData());
        if (ToolSql::HostConfirm(m_ctx.HostHwnd(), sQ.GetData(),
                                 StringMgr.Get("toolbox.restart_expl"),
                                 MB_YESNO, MB_ICONQUESTION) == IDYES)
        {
            ::PostMessageW(m_ctx.HostHwnd(), WM_HOST_REQUEST_RESTART_EXPLORER, 0, 0);
        }
    }
    // [CRASHFIX] MessageBox 返回后稳定队列：陈旧输入丢弃、只派发绘制消息，
    //   与 OcmFinishSwitch 同构（旧全量泵会在点击栈内派发陈旧消息致弹层 UAF）。
    optim_detail::SettleAfterDialog();
    // re-verify the real selection
    const int nAfter = m_ctx.DataEngine().GetScopeDropdown(node, nScope);
    // [R-EXTASYNC] 回读为"未配置"（-1）：保留用户刚选的值（写入已成功，
    // 只是系统侧回读不匹配），不要把框清空。
    OptimRollbackCombo(m_ctx, node, nScope, pCombo, nAfter);
}

void CPageOptim::OnScopeCombo(const wchar_t* pszName, int iSel)
{
    int g = -1, i = -1, nScope = -1;
    if (!ParseSwName(pszName, L"cbu_", g, i, nScope)) return;
    CDuiString sComboName;
    sComboName.Format(L"cbu_%d_%d_%d", g, i, nScope);
    CComboUI* pCombo = static_cast<CComboUI*>(m_ctx.HostFindControl(sComboName.GetData()));
    if (!pCombo) return;
    CMarkupNode root = m_ctx.DataEngine().GetOptimizationRoot();
    CMarkupNode node = FindOptimItem(root, g, i);
    if (!node.IsValid()) return;
    CDuiString sItemName = m_ctx.DataEngine().ResolveText(node.GetAttributeValue(L"Name"));
    // real selection before the change
    const int nCurrentSel = m_ctx.DataEngine().GetScopeDropdown(node, nScope);
    if (iSel == nCurrentSel) return;
    // [OPTIM-BOX] 用户点选后框架只改选中态、**不改显示文字**
    //   （CComboUI::PaintText 画的是自己的 m_sText）—— 这里补上文字，
    //   否则框里永远是空的。
    {
        CDuiString sSel;
        if (m_ctx.DataEngine().GetScopeDropdownName(node, nScope, iSel, sSel))
            pCombo->SetText(sSel.GetData());
    }
    if (!OcmConfirmCombo(node, nScope, iSel))
    {
        // [CRASHFIX] 确认框返回后先稳定队列再回滚下拉（同 OcmFinishCombo）
        optim_detail::SettleAfterDialog();
        OptimRollbackCombo(m_ctx, node, nScope, pCombo, nCurrentSel);
        return;
    }
    // apply for real
    std::vector<CDuiString> vOut;
    int nUnsupported = 0;
    bool bRestart = false;
    const bool bOk = m_ctx.DataEngine().SelectScopeDropdown(node, nScope, iSel, vOut,
                                                            nUnsupported, &bRestart);
    if (!bOk)
    {
        CDuiString sMsg;
        sMsg.Format(StringMgr.Get("error.s337e4fd6"),
                    m_ctx.DataEngine().ScopeName(nScope), sItemName.GetData());
        ToolSql::HostNotify(m_ctx.HostHwnd(), sMsg.GetData(),
                            StringMgr.Get("optim.settings"), MB_OK | MB_ICONWARNING);
        // [CRASHFIX] 失败提示框返回后先稳定队列再回滚下拉
        optim_detail::SettleAfterDialog();
        OptimRollbackCombo(m_ctx, node, nScope, pCombo, nCurrentSel);
        return;
    }
    // [F-OPT-1] 下拉变更入撤销栈（写入成功后才入栈，免开关路径的失败弹出）：
    //   前态 = nCurrentSel（系统本有档位，>=0 才有可回写的目标），
    //   后态 = iSel（用户实际选中，Ctrl+Y 重做用）。
    //   原先下拉从不入栈，Ctrl+Z 会回退更早的开关操作而非用户最近的选择。
    if (nCurrentSel >= 0)
        PushOptimUndoCombo(g, i, nScope, nCurrentSel, iSel);
    OcmFinishCombo(node, nScope, sItemName, pCombo, iSel, nUnsupported, bRestart);
}

// [R14-03] ShowOptimContextMenu 拆分（原 569 行/cc154）：薄壳只负责弹出菜单与
// 命令分派；菜单装配 OcmBuildMenu、属性框 OcmShowProperties、详情复制
// OcmCopyDetails、JSON 复制 OcmCopyJson、4607-4610 增强命令 OcmHandleEnhancedCmds。
// 动作节点白名单三处遍历共用 OcmIsActionTag 谓词（原三处 13 联 || 逐字一致）。
// 零行为改动：4602 在现行分派下与 4601 同走属性框（历史死分支原样保留，待 R15 评估）。
