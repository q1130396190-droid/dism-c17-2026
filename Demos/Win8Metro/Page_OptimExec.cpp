// ============================================================
// Page_OptimExec.cpp —— 优化页「批量应用」执行族（从 Page_Optim.cpp 拆出）。
//
// [R138] Page_Optim.cpp 两次拆分后仍 1897 行，规模分 4.9（全仓最低）。
// 本文件承接全局批量链路：统计可操作作用域 → 汇总去重 Warning → 二次确认
// → 逐项写入并节流刷新进度 → 收尾核实与失败/重启回弹，共 6 个成员函数。
//
// 闭包说明：全部是 CPageOptim 的成员（声明见 Page_Optim.h）。文件级依赖
// 只有四个过滤/收集判定（OptimItemShown / OptimGroupMatch /
// CollectWarningTexts / ContainsCI）与 FindOptimItem / ItemIsApplicable，
// 均已在 Page_OptimInternal.h 提升为 inline，两处共用一份实现。
//
// 搬迁为逐字搬运，零逻辑改动。
// ============================================================

#include "stdafx.h"

#include "Page_Optim.h"
#include "Page_OptimInternal.h"   // optim_detail::*（过滤判定 / Warning 收集）

#include "StringManager.h"        // StringMgr
#include "NotifyBox.h"          // [R143] ToolSql::HostNotify / HostConfirm（页面统一弹框入口）
#include "CustomControls.h"       // CSwitchUI（批量后同步开关视觉）
#include "WmMessages.h"           // WM_HOST_REQUEST_RESTART_EXPLORER
#include "ExternalToolService.h"  // [Privilege] IsElevated
#include "SafeThread.h"           // [G1] SafeDispatchMessage：批量期间泵消息
#include <chrono>                  // [G1] 分片计时

using i18n::CStringManager;

// [C++17 R6] 本 TU 特征常量：作用域开关目标态。
// [F-OPT-4] 原 kOptimExecProgressThrottleMs(40) 已删除：ThrottleProgress 实际
//   节流窗口为 100ms（Page_Optim.cpp），该常量从未被引用，留着会误导调参。
inline constexpr int      kOptimExecScopeOn = 1;

// 批量执行的安全约束：目标态只能是 0/1。
static_assert(kOptimExecScopeOn == 1, "bulk target state must be the enabled value");

namespace
{
using optim_detail::CollectWarningTexts;
using optim_detail::OptimGroupMatch;
using optim_detail::OptimItemShown;
}   // namespace

void CPageOptim::BulkApplyAllOptim()

{
    CMarkupNode root = m_ctx.DataEngine().GetOptimizationRoot();
    int nOn = 0, nTotal = 0;
    BaCountScopes(root, nOn, nTotal);
    if (nTotal == 0)
    {
        ToolSql::HostNotify(m_ctx.HostHwnd(), StringMgr.Get("optim.s20c4d3cd"), StringMgr.Get("misc.scd74eab4"), MB_OK | MB_ICONINFORMATION);
        return;
    }
    bool bTarget = (nOn == 0);   // 一个都没开 -> 全开；否则 -> 全关
    // [Fix #12] 批量必须与单项 OnSwitchToggle 走同一条安全链路：
    // 先汇总所有待变更开关的 Warning 文本（去重），并入确认框。
    std::vector<CDuiString> vBulkWarn;
    int nTodo = 0;   // [R2 P1] 实际待变更作用域数（进度条 max）
    int nPrivNeeded = 0;   // [Privilege] 其中需要 Admin 权限的作用域数
    BaCollectWarn(root, bTarget, vBulkWarn, nTodo, nPrivNeeded);
    if (!BaShowConfirm(bTarget, nTotal, vBulkWarn, nPrivNeeded)) return;
    // [CRASHFIX] 批量确认框（MessageBox 模态泵）返回后，队列里残留本次
    //   点击与弹层关闭的陈旧输入消息。BaExecuteBatch 的分片泵会把它们
    //   在点击栈内派发出去——先定向清理一次，后续泵只处理批量期间新到消息。
    optim_detail::SettleAfterDialog();
    int nChanged = 0;
    bool bAnyFailed = false;   // [Fix #12/#13] 部分动作失败要向用户回弹
    bool bAnyRestart = false;  // [Fix #12] 任一动作要求重启 Explorer，结束后统一询问
    BaExecuteBatch(root, bTarget, nTodo, nChanged, bAnyFailed, bAnyRestart);
    BaFinishReport(bTarget, nChanged, nTotal, bAnyFailed, bAnyRestart);
}
void CPageOptim::BaCountScopes(CMarkupNode root, int& nOn, int& nTotal)

{
    CMarkupNode gNode = root.GetChild(L"Group");
    int g = 0;
    while (gNode.IsValid())
    {
        const bool bGroupMatch = OptimGroupMatch(m_ctx, gNode, m_sOptimFindText);
        CMarkupNode item = gNode.GetChild(L"Item");
        int ii = 0;
        while (item.IsValid())
        {
            // [FILTER-SSOT] 用与界面同一谓词判定"是否在统计范围内"。
            // 原 pSw->IsVisible() 会把分组折叠也算作不可见，导致所有组
            // 折叠时批量按钮认为"没有可操作项"而静默不动，而分组复选框
            // 却能改同一批规则——同一份数据两个入口给出相反结论。
            const bool bShown = OptimItemShown(m_ctx, item, m_sOptimFindText,
                                               m_nOptimStateFilter, bGroupMatch);
            for (int sc : optim_scope::kColumns)
            {
                if (!item.GetChild(m_ctx.DataEngine().ScopeName(sc)).IsValid()) continue;
                if (!bShown) continue;
                ++nTotal;
                if (m_ctx.DataEngine().GetScopeState(item, sc) == 1) ++nOn;
            }
            ++ii;
            item = item.GetSibling();
        }
        ++g;
        gNode = gNode.GetSibling();
    }
}

// [R176] 从 BaCollectWarn（48 行 / CC13）抽出：单个作用域是否需要纳入批量变更。
// 三个跳过条件逐字保留：作用域节点不存在 / 条目被筛选隐藏 / 当前状态已等于目标。
// 第三个条件是"已有该状态就不必写"的去重逻辑，反转写会退化成"只改已对的项"。
[[nodiscard]] bool CPageOptim::BaScopeNeedsChange(CMarkupNode item, int sc,
                                                  bool bShown, bool bTarget)
{
    if (!item.GetChild(m_ctx.DataEngine().ScopeName(sc)).IsValid()) return false;
    if (!bShown) return false;
    return (m_ctx.DataEngine().GetScopeState(item, sc) == 1) != bTarget;
}

// [R176] 从 BaCollectWarn 抽出：收集单作用域警告文本并按内容去重追加。
// 去重是 O(n²) 线性扫描（警告文本通常 <20 条），保持原实现不改用 set：
// 顺序会影响提示弹窗里的阅读顺序，set 会打乱。
void CPageOptim::BaAppendWarnUnique(CMarkupNode item, int sc, bool bTarget,
                                    std::vector<CDuiString>& vBulkWarn)
{
    std::vector<CDuiString> vW;
    CollectWarningTexts(m_ctx.DataEngine(), item, sc, bTarget, -1, vW);
    for (size_t w = 0; w < vW.size(); ++w)
    {
        bool bDup = false;
        for (size_t q = 0; q < vBulkWarn.size(); ++q)
            if (vBulkWarn[q] == vW[w]) { bDup = true; break; }
        if (!bDup) vBulkWarn.push_back(vW[w]);
    }
}

// [R160] 从 BaCollectWarn 抽出：单条目 × 单作用域的统计单元。
//   原实现是 while/while/for/if 四层嵌套（实测控制流嵌套 5 层），
//   但"一次判定是否计入"只依赖 (item, sc, bShown)，与遍历结构无关。
//   抽成成员函数后：判定逻辑单点可测，遍历层数下降，语义逐字未改。
//   Admin 计数只认 Admin —— System 项即便提权也写不动（受 ACL 保护），
//   提示"请以管理员身份运行"反而是误导。
[[nodiscard]] bool CPageOptim::BaAccumOneScope(CMarkupNode item, int sc, bool bShown,
                                               bool bTarget, int& nPrivNeeded,
                                               std::vector<CDuiString>& vBulkWarn)
{
    // [R176] 单作用域是否待变更抽为 BaScopeNeedsChange，回归测试只需改一处
    if (!BaScopeNeedsChange(item, sc, bShown, bTarget)) return false;
    const wchar_t* pszPriv = item.GetAttributeValue(L"Privilege");
    if (pszPriv != nullptr && lstrcmpiW(pszPriv, L"Admin") == 0) ++nPrivNeeded;
    // [R176] 警告收集去重抽为 BaAppendWarnUnique
    BaAppendWarnUnique(item, sc, bTarget, vBulkWarn);
    return true;
}

void CPageOptim::BaCollectWarn(CMarkupNode root, bool bTarget,

                               std::vector<CDuiString>& vBulkWarn, int& nTodo,
                               int& nPrivNeeded)

{
    CMarkupNode gNode = root.GetChild(L"Group");
    int g = 0;
    while (gNode.IsValid())
    {
        const bool bGroupMatch = OptimGroupMatch(m_ctx, gNode, m_sOptimFindText);
        CMarkupNode item = gNode.GetChild(L"Item");
        int ii = 0;
        while (item.IsValid())
        {
            const bool bShown = OptimItemShown(m_ctx, item, m_sOptimFindText,
                                               m_nOptimStateFilter, bGroupMatch);
            for (int sc : optim_scope::kColumns)
            {
                // [R160] 改用返回值表达"已计入"，不再在四层里 continue 跳转
                if (BaAccumOneScope(item, sc, bShown, bTarget, nPrivNeeded, vBulkWarn))
                    ++nTodo;
            }
            ++ii;
            item = item.GetSibling();
        }
        ++g;
        gNode = gNode.GetSibling();
    }
}

bool CPageOptim::BaShowConfirm(bool bTarget, int nTotal, const std::vector<CDuiString>& vBulkWarn,
                               int nPrivNeeded)

{
    CDuiString sMsg;
    sMsg.Format((std::wstring(StringMgr.Get("optim.sfdfa8a3c")) + std::wstring(StringMgr.Get("optim.s8614d6f4"))).c_str(),
                bTarget ? StringMgr.Get("misc.scc014f97") : StringMgr.Get("misc.se1252210"), nTotal);
    if (!vBulkWarn.empty())
    {
        sMsg += StringMgr.Get("misc.s30f04db5");
        for (size_t w = 0; w < vBulkWarn.size(); ++w) { sMsg += vBulkWarn[w]; sMsg += L"\r\n"; }
    }
    // [Privilege] 未提权且存在 Admin 项时追加提示。批量走的是同一条安全链路，
    // 不与单项预警重复拦截——这里只提示，不改变"继续/取消"的既有语义。
    if (nPrivNeeded > 0 && !ExternalToolService::IsElevated())
    {
        CDuiString sPriv;
        sPriv.Format(StringMgr.Get("optim.admin_batch_note"), nPrivNeeded);
        sMsg += sPriv;
    }
    return ToolSql::HostConfirm(m_ctx.HostHwnd(), sMsg.GetData(), StringMgr.Get("misc.s345876e9"), MB_YESNO, MB_ICONWARNING) == IDYES;
}

// [G1 2026-10-25] 批量执行期间的消息泵。
//
// 原实现：BaExecuteBatch 在 UI 线程逐条同步写注册表，数百条会冻结数秒到数十秒，
// 进度条只更新数值但消息循环不转 —— 视觉上就是"卡死"，用户往往直接强杀进程。
//
// 为什么不用工作线程：本循环同时做两件非线程安全的事 ——
//   1) CMarkupNode（DuiLib XML DOM）的迭代；
//   2) HostFindControl + CSwitchUI::SetOn 直接操作控件。
// 两者都只在 UI 线程合法。硬搬到工作线程需要把整条链路改成"快照下发 + 消息回传"，
// 改动面远大于收益，且一旦漏改就是随机崩溃。
//
// 选择：留在 UI 线程，但每处理若干作用域就泵一次消息，并设时间片上限。
// 这样 UI 保持可拖动、可重绘、进度条真实推进；危险动作已在确认框里过了，
// 中途不再接受新的批量请求（m_bBatchRunning 闸门）。

void CPageOptim::BaExecuteBatch(CMarkupNode root, bool bTarget, int nTodo,

                                int& nChanged, bool& bAnyFailed, bool& bAnyRestart)

{
    // [G1] 重入闸门：批量期间用户仍可点击其它按钮，必须在入口拦住第二次执行。
    if (m_bBatchRunning)
    {
        AppendLog(L"[optim] batch already running, request ignored");
        return;
    }
    // [F-OPT-2] 闸门 RAII 化：原实现在循环尾部手工复位 m_bBatchRunning，
    //   中途任何异常（bad_alloc / DuiLib 内部抛出）都会让闸门卡死 true——
    //   批量按钮从此永久失效、进度条残留可见。与 VerifyOptimStates 的
    //   VerifyGuard 同一模式；析构顺带隐藏进度条，异常路径不留残余 UI。
    struct BatchGate
    {
        CPageOptim* m_pPage;
        explicit BatchGate(CPageOptim* p) : m_pPage(p) { }
        ~BatchGate() { m_pPage->NotifyBatchGateReleased(); }
    } batchGate(this);
    m_bBatchRunning = true;

    int nDone = 0;             // [R2 P1] 已处理作用域数（进度条 val）
    int nUndoPushed = 0;       // [G3] 实际入撤销栈的条数（供收尾日志核对）
    m_dwLastOptimMs = 0;
    SetOptimProgress(0, nTodo);
    CMarkupNode gNode = root.GetChild(L"Group");
    int g = 0;
    while (gNode.IsValid())
    {
        const bool bGroupMatch = OptimGroupMatch(m_ctx, gNode, m_sOptimFindText);
        CMarkupNode item = gNode.GetChild(L"Item");
        int ii = 0;
        while (item.IsValid())
        {
            const bool bShown = OptimItemShown(m_ctx, item, m_sOptimFindText,
                                               m_nOptimStateFilter, bGroupMatch);
            for (int sc : optim_scope::kColumns)
            {
                // [R160] 原实现把"读前态 → 写入 → 回读 → 入撤销栈 → 同步控件 → 计数"
                //   全内联在 while/while/for 三层里，控制流嵌套 5 层。
                //   抽为 BaExecOneScope 后遍历只剩两层，写入成败的记账集中一处。
                BaExecOneScope(item, g, ii, sc, bShown, bTarget,
                               nDone, nTodo, nUndoPushed,
                               nChanged, bAnyFailed, bAnyRestart);
            }
            ++ii;
            item = item.GetSibling();
        }
        SyncOptimGroupGlyph(g);
        ++g;
        gNode = gNode.GetSibling();

        // [G1] 每个分组结束时让出一次时间片：分组粒度天然合适，
        // 既不会太频繁（每组通常十余项），也不会让单组过久占住消息循环。
        optim_detail::PumpUiMessages(8);
    }
    SetOptimProgress(0, 0);   // [R2 P1] 批量结束，隐藏进度条
    m_bBatchRunning = false;  // [G1] 正常路径显式复位（异常路径由 BatchGate 兜底）

    // [G3] 把入栈条数写进日志：用户按 Ctrl+Z 能回退多少，取决于这里记了多少。
    // 栈深上限 32，超出部分会被挤出——用户看到数字才知道"撤销只能退 32 步"。
    if (nUndoPushed > 0)
    {
        CDuiString sLog;
        sLog.Format(L"[optim] batch pushed %d undo record(s); Ctrl+Z can revert up to 32",
                    nUndoPushed);
        AppendLog(sLog.GetData());
    }
}

// [R160] 从 BaExecuteBatch 抽出：单个 条目×作用域 的执行单元。
//   函数体与原内联块逐字等价，只把对外部计数器的写入改由引用参数承担：
//   nDone/nTodo 供进度节流，nUndoPushed/nChanged 供收尾统计，
//   bAnyFailed/bAnyRestart 由 DataEngine 回填并向外汇总。
//   提前返回的三条前置条件（列不存在 / 被筛选隐藏 / 已达目标态）保持原顺序，
//   因为 GetScopeState 对不存在的列也会返回 0，顺序颠倒会把"不可用"误判成"待改"。
void CPageOptim::BaExecOneScope(CMarkupNode item, int g, int ii, int sc, bool bShown,
                                bool bTarget, int& nDone, int nTodo, int& nUndoPushed,
                                int& nChanged, bool& bAnyFailed, bool& bAnyRestart)
{
    if (!item.GetChild(m_ctx.DataEngine().ScopeName(sc)).IsValid()) return;
    if (!bShown) return;
    CDuiString sSw; sSw.Format(L"sw_%d_%d_%d", g, ii, sc);
    CSwitchUI* pSw = static_cast<CSwitchUI*>(m_ctx.HostFindControl(sSw));
    int nBefore = m_ctx.DataEngine().GetScopeState(item, sc);
    if ((nBefore == 1) == bTarget) return;  // 已达目标态
    std::vector<CDuiString> vOut;
    int nUnsupported = 0;
    bool bRestart = false;
    bool bOk = m_ctx.DataEngine().SetScopeState(item, sc, bTarget, vOut,
                                          nUnsupported, &bRestart,
                                          &bAnyFailed);
    if (bRestart) bAnyRestart = true;
    int nAfter = m_ctx.DataEngine().GetScopeState(item, sc);
    // [G3 2026-10-25] 批量也要进撤销栈。
    //   原先只有单开关切换与右键"恢复默认"入栈，最危险的批量改动反而
    //   无法 Ctrl+Z 回退 —— 用户一次误点全开三百项，只能手动逐个关回去。
    //   只记录"真正改变成功"的项：写入失败者状态未变，入栈会让撤销
    //   把本来就对的项改错。nBefore 记写入前状态，供 UndoLastOptim 还原。
    if (bOk && nAfter != nBefore)
    {
        PushOptimUndo(g, ii, sc, nBefore == 1);
        ++nUndoPushed;
    }
    // 开关控件可能不存在（该列被 dash 占位），只在拿到控件时同步视觉，
    // 写入结果本身由 nAfter 回读保证，不依赖控件存在。
    // [FIX-STATE] nAfter==-1 表示该项在该作用域"未配置/不可判"，不是"关"。
    //   旧实现把 -1 当 0 强写控件视觉，与单项路径的 nAfter>=0 守卫
    //   （Page_OptimSwitch OcmFinishSwitch）不一致，批量期间会闪现错误态。
    if (pSw && nAfter >= 0) pSw->SetOn(nAfter == 1, false);
    if (bOk && (nAfter == 1) == bTarget) ++nChanged;
    ThrottleProgress(m_dwLastOptimMs, ++nDone, nTodo);  // [R2 P1]
}

void CPageOptim::BaFinishReport(bool bTarget, int nChanged, int nTotal,

                                bool bAnyFailed, bool bAnyRestart)

{
    CDuiString sLog;
    sLog.Format(L"[optim] global bulk -> %s, %d/%d scopes applied%s",
                bTarget ? L"ON" : L"OFF", nChanged, nTotal,
                bAnyFailed ? L", SOME ACTIONS FAILED" : L"");
    AppendLog(sLog.GetData());
    // [VERIFY-TTL] 批量刚写完，必须强制核实：VerifyOptimStates(false) 受 5 秒 TTL
    // 约束，而批量执行可能就在上一次核实的 5 秒窗口内（例如用户连点两次批量，
    // 或先点「提交规则」再点批量），此时本次核实被直接短路，界面上刚写入的
    // 真实状态不会回读，底部「已核实 X 项」也停留在旧值。
    InvalidateOptimVerifyCache();
    VerifyOptimStates(true);
    if (bAnyFailed)
    {
        ToolSql::HostNotify(m_ctx.HostHwnd(), (std::wstring(StringMgr.Get("error.sec28c80b")) + std::wstring(StringMgr.Get("system.sd4cb8615"))).c_str(), StringMgr.Get("misc.s345876e9"), MB_OK | MB_ICONWARNING);
    }
    if (bAnyRestart)
    {
        if (ToolSql::HostConfirm(m_ctx.HostHwnd(), StringMgr.Get("optim.se8d6c6df"), StringMgr.Get("toolbox.restart_expl"), MB_YESNO, MB_ICONQUESTION) == IDYES)
        {
            ::PostMessageW(m_ctx.HostHwnd(), WM_HOST_REQUEST_RESTART_EXPLORER, 0, 0);
        }
        // [CRASHFIX] 批量重启确认框返回后稳定队列：陈旧输入定向丢弃、
        //   只派发绘制消息；重启请求留在队列由主循环栈外处理（与单项同构）。
        optim_detail::SettleAfterDialog();
    }
}
