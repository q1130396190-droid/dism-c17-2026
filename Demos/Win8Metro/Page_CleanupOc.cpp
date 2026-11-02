// ============================================================
// Page_CleanupOc.cpp —— 清理页「完成收口族」（从 Page_Cleanup.cpp 拆出）。
//
// [R182] Page_Cleanup.cpp 拆前 1,115 行、规模分 9.41（六页最低）。本文件承接
// 五个成员，合计 179 行，边界按「一次清理完成后的 UI 收口」划分：
//   · OnCleanDone             完成消息总入口（结果摘要 → 撤销栈 → 异步 DISM 尾巴）
//   · OcZeroUiSizes           本次参与删除的规则行尺寸清零
//   · OcPushUndoStack         走回收站的删除路径推入撤销栈
//   · OcDispatchCustomActions 12 路自定义动作派发（DISM/驱动/Compact/Appx…）
//   · OcRunDismComponent      WinSxS 组件清理（CBS 事务，可达数分钟）
//
// [AUDIT-P0] 后两者原在 WM_CLEAN_DONE 回调里同步执行，等于把 UI 线程钉死；
// 现由 OnCleanDone 交给 StartCleanDismTailAsync 后台化，完成后经
// WM_CLEAN_DISMTAIL 回 UI 收口。本文件只保留"发起"，不含后台实现。
//
// 闭包说明：五个成员声明见 Page_Cleanup.h。跨 TU 依赖五项，均为头文件级：
//   · m_ctx（HostHwnd / HostFindControl / HostLog）
//   · m_vLastCleanSel / m_ullLastCleanFreed（成员）
//   · AppendLog（成员）、StringMgr（clean.* 语言键，逐字保持）
//   · CleanJobService（CleanJobService.h）、DismApiBridge（DismApiBridge.h）
//   · ToolSql::HostConfirm（NotifyBox.h）
//
// 逐字搬运，零逻辑改动。
// ============================================================

#include "stdafx.h"

#include "Page_Cleanup.h"
#include "Page_CleanupInternal.h"   // cleanup_detail 共享

#include "StringManager.h"
#include "NotifyBox.h"              // ToolSql::HostConfirm
#include "CleanJobService.h"        // IsScanning / RequestCancel
#include "DismApiBridge.h"          // CleanComponentStore
#include "LogService.h"             // ELogLevel
#include "CustomControls.h"         // CSwitchUI / CControlUI

using namespace DuiLib;
using i18n::CStringManager;

// 主文件经 using 声明引入；拆出后必须显式引入（R179/R181 同类，这是第 4 次）。
using cleanup_detail::RunOnlineComponentCleanup;

// [R182] "继续清理吗"确认框的按钮组合。原只有一处调用（组件清理前的二次确认），
// 但该组合是**危险操作**的统一口径：默认按钮必须停在"否"（NotifyBox 内的
// HostConfirm 已强制 MB_DEFBUTTON2），图标必须是警告级而非问号。
// 具名后与同页其它确认框可对比，防止后续有人"顺手"改成 MB_ICONQUESTION。
inline constexpr UINT kCleanDismConfirmButtons = MB_YESNO | MB_ICONWARNING;

// [R160] 从 OcZeroUiSizes 抽出：规则的全部目标路径是否都已不存在。
//   注意判定语义：目标列表为空时返回 false（无目标≠已清理干净），
//   只有"至少一个目标且全部已消失"才算本条规则确实清完，
//   否则会把解析失败/规则被跳过的项也按 0 字节呈现。
[[nodiscard]] static bool OcAllTargetsGone(const CleanRule& r)
{
    if (r.targets.empty()) return false;
    for (size_t t = 0; t < r.targets.size(); ++t)
    {
        if (::GetFileAttributes(r.targets[t].sPath.GetData()) != INVALID_FILE_ATTRIBUTES)
            return false;
    }
    return true;
}

void CPageCleanup::OcZeroUiSizes(const CleanDoneMsg* m)

{
    // 本次真正参与删除的规则集合
    const std::vector<int>& vSel = m_vLastCleanSel;
    if (!m) return;
    for (size_t si = 0; si < vSel.size(); ++si)
    {
        int i = vSel[si];
        CleanRule& r = m_ctx.CleanEngine().GetRule(i);
        // [R160] 判定抽为 OcAllTargetsGone，遍历层数由 4 降到 3
        if (!OcAllTargetsGone(r)) continue;
        r.ullSize = 0;
        if (i < static_cast<int>(m_vUiSize.size())) m_vUiSize[i] = 0;  // [修复 #23]
        CDuiString sName;
        sName.Format(L"csz_%d", i);
        m_ctx.HostSetCtrlText(sName.GetData(), L"0.00 KB");
    }
    RefreshGroupTotals();
}

void CPageCleanup::OcPushUndoStack(const std::vector<CDuiString>& vDeleted, bool bToRecycleBin)

{
if (bToRecycleBin && !vDeleted.empty())
{
    std::vector<CleanExtendUndoEntry> vUndo;
    vUndo.reserve(vDeleted.size());
    for (size_t i = 0; i < vDeleted.size(); ++i)
    {
        CleanExtendUndoEntry e;
        e.sPath = vDeleted[i];
        DWORD dwAttr = ::GetFileAttributes(e.sPath.GetData());
        e.bDir = (dwAttr != INVALID_FILE_ATTRIBUTES
                  && (dwAttr & FILE_ATTRIBUTE_DIRECTORY));
        e.ullSize = 0;   // 主规则清理阶段未单独计量
        ::GetSystemTimeAsFileTime(&e.ftDeleted);
        vUndo.push_back(e);
    }
    m_ctx.CleanExtendEngine().PushUndo(vUndo, 20);
    CDuiString sUndoLog;
    sUndoLog.Format(L"[clean] %d paths sent to recycle bin, undo stack depth=%d",
                    static_cast<int>(vDeleted.size()),
                    m_ctx.CleanExtendEngine().UndoDepth());
    AppendLog(sUndoLog.GetData());
}
}

// [FIX-TAIL-PROG] 尾巴阶段分步进度：经 WM_CLEAN_PROGRESS(nPhase=2) 回投 UI
// 状态条（工作线程禁直写 UI）。sStage 为当前步骤描述。
static void OcTailProgress(HWND hNotify, int nDone, int nTotal, const wchar_t* pszStage)
{
    if (!hNotify || !::IsWindow(hNotify)) return;
    auto m = std::make_unique<CleanProgressMsg>();
    m->nDone = nDone;
    m->nTotal = nTotal > 0 ? nTotal : 1;
    m->nPhase = 2;
    if (pszStage) m->sStage = pszStage;
    if (::PostMessageW(hNotify, WM_CLEAN_PROGRESS, 0,
                       reinterpret_cast<LPARAM>(m.get())))
        m.release();   // 投递失败自释放（所有权契约同其它 WM_CLEAN_PROGRESS）
}

void CPageCleanup::OcDispatchCustomActions()

{
// queued Custom ProcName actions — dispatch one by one on main thread
// (dism.exe / powershell commands run synchronously via RunHiddenExit)
// Note: CustomProcIdx::CPI_DISMCOMP is intentionally skipped here — it runs separately
// below as the last cleanup step (WinSxS component cleanup).
// [FIX-TAIL-PROG] 逐路分步进度：每路动作执行前回投 "(i/n) 步骤名"，
// CBS/Appx 等长动作期间状态条持续可见推进，杜绝"卡死"错觉。
int nQueued = 0;
for (int kc = 0; kc < CUSTOM_PROC_COUNT; ++kc)
    if (CleanJobService::GetCustomProc(static_cast<CustomProcIdx>(kc))) ++nQueued;
OcTailProgress(m_ctx.HostHwnd(), 0, nQueued, L"开始执行清理收尾动作");

int nDoneStep = 0;
for (int ki = 0; ki < CUSTOM_PROC_COUNT; ++ki)
{
    const CustomProcIdx k = static_cast<CustomProcIdx>(ki);
    if (!CleanJobService::GetCustomProc(k)) continue;
    if (k == CustomProcIdx::CPI_DISMCOMP) continue;   // handled below
    CleanJobService::SetCustomProc(k, false);
    // [E-2] 派发前按当前实时状态重评来源规则 Applicable：
    // 扫描后已不适用的动作（如对应软件已卸载）本轮跳过。
    if (!CleanJobService::CustomActionStillApplicable(m_ctx.CleanEngine(), k))
    {
        const wchar_t* pszProc = CleanJobService::CpiToProcName(k);
        CDuiString sSkip;
        sSkip.Format(L"[clean] custom action '%s' skipped: source rule no longer applicable",
                     pszProc ? pszProc : L"?");
        AppendLog(sSkip.GetData());
        ++nDoneStep;
        continue;
    }
    ++nDoneStep;
    CDuiString sStep;
    sStep.Format(L"(%d/%d) 执行 %s", nDoneStep, nQueued,
                 CleanJobService::CpiToProcName(k));
    OcTailProgress(m_ctx.HostHwnd(), nDoneStep, nQueued, sStep.GetData());
    RunCustomProcAction(k);
}
}

// [R160] 从 OcRunDismComponent 抽出：ResetBase 的二次确认。
//   ResetBase 会永久移除被取代的更新组件，之后无法卸载已安装的累积更新，
//   属不可逆动作，必须在执行前单独确认。用户拒绝时降级为普通清理并留痕。
//   抽出目的：原实现把"确认 + 执行 + 结果判定"叠在 if/if/else 三层里，
//   控制流嵌套达 5 层；判定与执行分离后，两条路径各自可读可测。
[[nodiscard]] bool CPageCleanup::OcConfirmDismResetBase(bool bResetBase)
{
    if (!bResetBase) return false;
    // [FIX-TAIL-PROG] 弹框前先写日志：模态框若被主窗遮挡，用户可从日志区
    // 得知"正在等待确认"，避免误判冻结。
    AppendLog(L"[clean] waiting for user confirmation: ResetBase dialog (may be behind other windows)");
    CDuiString sMsg = CDuiString(StringMgr.Get("misc.h8d31")) + CDuiString(StringMgr.Get("misc.hd15a")) + CDuiString(StringMgr.Get("misc.hbff1")) + CDuiString(StringMgr.Get("misc.h1a2a"));
    int nYes = ToolSql::HostConfirm(m_ctx.HostHwnd(), sMsg.GetData(), StringMgr.Get("misc.hed51"), kCleanDismConfirmButtons);
    if (nYes == IDYES) return true;
    AppendLog(L"[clean] ResetBase declined by user, running normal StartComponentCleanup");
    return false;
}

// [R160] 从 OcRunDismComponent 抽出：组件清理的实际执行与结果判定。
//   [P1-收尾] 统一 DismApiBridge::CleanComponentStore 进程内原生（在线/离线同路径），
//   零 dism.exe 外部命令回退。
//   [R18-b] 接入取消状态：UI 置位的请求在 DISM 返回后如实反映到日志，
//   并阻止后续阶段继续执行（DISM 同步调用本身无法中断）。
void CPageCleanup::OcExecDismComponent(bool bReset)
{
    cleanup_detail::CleanProgressState cancelState;
    cancelState.bCancelRequested = m_bCleanDismCancel;
    CDuiString sCbsLog;
    int nCbs = cleanup_detail::RunOnlineComponentCleanupEx(bReset, sCbsLog, &cancelState);
    m_bCleanDismCancel = cancelState.bCancelRequested;
    AppendLog(sCbsLog.GetData());
    if (cleanup_detail::IsCleanCancelRequested(cancelState))
    {
        AppendLog(L"[!] [clean] cancel requested during component cleanup; "
                  L"the running DISM call could not be interrupted");
        m_bLastCleanPartial = true;
    }
    if (nCbs == 0)
    {
        AppendLog(bReset
            ? L"[clean] WinSxS component cleanup (ResetBase) ok"
            : L"[clean] WinSxS component cleanup ok");
        return;
    }
    AppendLog(bReset
        ? L"[clean] WinSxS component cleanup (ResetBase) failed (native dismapi path only; no external fallback)"
        : L"[clean] WinSxS component cleanup failed (native dismapi path only; no external fallback)");
}

void CPageCleanup::OcRunDismComponent(bool bPending, bool bResetBase)

{
// WinSxS DISM component cleanup — handled last (separate log line)
if (!bPending) return;
CleanJobService::SetCustomProc(CustomProcIdx::CPI_DISMCOMP, false);
// [E-2] 组件清理同样要过实时 Applicable 门控
if (!CleanJobService::CustomActionStillApplicable(m_ctx.CleanEngine(), CustomProcIdx::CPI_DISMCOMP))
{
    AppendLog(L"[clean] component cleanup skipped: source rule no longer applicable");
    return;
}
// [FIX-TAIL-PROG] CBS 事务两阶段进度（无内部百分比，用开始/完成两点）
OcTailProgress(m_ctx.HostHwnd(), 0, 1,
               L"WinSxS 组件存储清理中（CBS 事务，可能需要数分钟）");
// [R17] ResetBase 先过二次确认，拒绝则降级为普通清理
OcExecDismComponent(OcConfirmDismResetBase(bResetBase));
OcTailProgress(m_ctx.HostHwnd(), 1, 1, L"组件存储清理完成");
}
// [R14-02] OnCleanDone 拆解薄壳：DetachThread + 缓存 + 6 段子流程分发
[[nodiscard]] bool CPageCleanup::OnCleanDone(CleanDoneMsg* m)

{
    CleanJobService::DetachThread();
    // [C++17 R3] receiver takes ownership of m, freed at scope end (was: delete m mid-function)
    std::unique_ptr<CleanDoneMsg> mGuard(m);
    // m 在下方 delete，结果值提前缓存供结尾气泡使用
    const int nCleanFailed = m ? m->nFailed : 0;
    const ULONGLONG  ullCleanFreed = m ? m->ullFreed : 0;
    // P6b：4 个通知 bool 改由消息带出，delete m 前缓存
    const bool bDismComponentPending = m ? m->bDismComponentPending : false;
    const bool bDismResetBaseMsg     = m ? m->bDismResetBase : false;
    const bool bCleanAssocNotify     = m ? m->bCleanAssocNotify : false;
    const bool bCleanRestartExplorer = m ? m->bCleanRestartExplorer : false;
    m_nLastCleanFailed = nCleanFailed;
    m_ullLastCleanFreed = ullCleanFreed;
    CDuiString sLog;
    sLog.Format(L"[clean] done. rules=%d failed paths=%d",
             m ? m->nRules : 0, m ? m->nFailed : 0);
    AppendLog(sLog.GetData());
    // [R60-A] 清理完成：磁盘空间采样 + clean_history.log 追加 + 状态回写
    OcWriteCleanHistory(m);
    // C-5/C-3 结果摘要 + 明细面板回写
    OcRefreshDescPanel(m);
    // 清零勾选规则 UI 尺寸（仅本次勾选规则）
    OcZeroUiSizes(m);
    // 主规则清理走回收站时推入 CleanExtend 撤销栈（delete m 前缓存路径）
    const std::vector<CDuiString> vDeleted = m ? m->vDeletedPaths : std::vector<CDuiString>();
    const bool bToRecycleBin = m ? m->bToRecycleBin : false;
    OcPushUndoStack(vDeleted, bToRecycleBin);
    // [C++17 R3] mGuard frees m at scope end
    // [AUDIT-P0] Custom 派发 + WinSxS 组件清理移入工作线程。
    // 原实现在此同步调用 OcDispatchCustomActions() 与 OcRunDismComponent()：
    // 前者含 12 路自定义动作（DISM 组件清理/驱动/Compact/Appx...），后者
    // 走 DismApiBridge::CleanComponentStore（CBS 事务，实测可达数分钟）。
    // 二者都在 WM_CLEAN_DONE 回调里跑，等于把 UI 线程钉死，用户既看不到
    // 进度也无法取消，只能强杀进程——而此时 CBS 可能正在改写组件存储。
    // 改为后台执行，完成后经 WM_CLEAN_DISMTAIL 回到 UI 收口。
    StartCleanDismTailAsync(bDismComponentPending, bDismResetBaseMsg,
                            bCleanAssocNotify, bCleanRestartExplorer);
    // 返回 false：DISM 尾巴已异步化，重启询问与 Tail 解锁都在
    // OnCleanDismTail 中完成，壳侧不再需要（也不应）提前解锁。
    // 为兼容壳仍会无条件调用 HostOnCleanDoneTail()，OnCleanDoneTail 已加幂等保护：
    // 首次调用若早于异步收口，只会被标记；真正收尾以异步收口为准。
    return false;
}
