#include "stdafx.h"
#include "OfflineSession.h"  // [R10-02] 离线会话经独立组件访问，去 CDataEngine 静态耦合

#include "Page_Cleanup.h"
#include "Page_CleanupInternal.h"   // cleanup_detail::RunOnlineComponentCleanup（跨 TU 共用）

#include "StringManager.h"
#include "NotifyBox.h"           // [R144] ToolSql::HostNotify / HostConfirm（页面统一弹框入口）

#include "FrameWnd_Util.h"          // StyleCheckBox / RecalcBodyHeight / CopyTextToClipboard
#include "FrameWnd_UiHelpers.h"    // P8：统一列表右键菜单 / EListCtx

#include "CustomControls.h"        // CClickRowUI

#include "ProcessRunner.h"  // RunHiddenExit / RunCapture

#include "FrameWnd_Messages.h"     // [LAG-FIX] WM_DIAG_LOG
#include "SafeThread.h"            // 碎片诊断后台线程异常边界

#include "ExternalToolService.h"   // IsElevated
#include "DismApiBridge.h"         // [R-B18] dismapi.dll 直连 DismRemovePackage
#include "AppxRuntimeBridge.h"     // [P0-A1] Appx 重注册/坏包清理 WinRT 原生

#include "CleanExtend.h"          // [R80] 深度扫描/残留/空文件夹/撤销引擎
// #include "VssNativeService.h" // [R25-F1] 删除假依赖：0 处使用  // [R-B31] VSS 原生 API 删除快照（vssapi.dll COM）

#include "LogService.h"            // DetectLevel（与原 AppendLog 自动分级一致）
// #include "CbsHostClient.h" // [R23-F3] 删除假依赖：0 处使用（L819-830 走 DismApiBridge+COfflineSession）
#include "NCleanerClient.h"       // [R-拆分] NCleaner 7 导出经 NCleaner.dll 动态调用
// #include "DataEngine.h" // [R23-F3] 删除假依赖：0 处使用（L827-828 用 COfflineSession）
// #include "SystemInfoService.h" // [R23-F3] 删除假依赖：0 处使用

#include "CleanNativePrimitives.h" // [R-B57] CreateHardLink 通用重复文件回收原语
// #include "WofCompact.h" // [R25-F1] 删除假依赖：0 处使用           // [P0-A2] CompactOS 原生 FSCTL（替代 compact.exe）
// #include "RegCleanEngine.h" // [R23-F3] 删除假依赖：0 处使用（注释 [P1-A10] 声称 SRSetRestorePointW；L819-830 实际走 DismApiBridge，未用还原点）
#include <memory>                 // [C++17 R3] make_unique/unique_ptr

#include <shlobj.h>                // SHGetFolderPath / SHChangeNotify
#include <winevt.h>                // [R-B17] EvtClearLog / EvtOpenChannelEnum

#pragma comment(lib, "wevtapi.lib")

#include <string>

#include <algorithm>              // [R126-1] std::stable_sort
// [C++17 阶段2] 局部 RAII 守卫：查找/互斥量句柄（见 WinHandles.h 使用规范）
struct PCFindGuard
{
    HANDLE h;
    explicit PCFindGuard(HANDLE x) : h(x) {}
    ~PCFindGuard() { if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h); }
    PCFindGuard(const PCFindGuard&) = delete;
    PCFindGuard& operator=(const PCFindGuard&) = delete;
    explicit operator bool() const { return h && h != INVALID_HANDLE_VALUE; }
    operator HANDLE() const { return h; }
};

struct PCMutexGuard
{
    HANDLE h;
    explicit PCMutexGuard(HANDLE x) : h(x) {}
    ~PCMutexGuard() { if (h) ::CloseHandle(h); }
    PCMutexGuard(const PCMutexGuard&) = delete;
    PCMutexGuard& operator=(const PCMutexGuard&) = delete;
    explicit operator bool() const { return h != nullptr; }
};

using i18n::CStringManager;   // 便捷：直接写 CStringManager 不用前缀
using cleanup_detail::RunOnlineComponentCleanup;

// ============================================================
// 文件内辅助（随清理页函数一并从 FrameWnd.cpp 机械搬迁；除 SetGroupGlyph
// 增加 ctx 参数外零逻辑改动）
// ============================================================

namespace

{
    // [修复 #23] UI 大小快照中“尚未扫到该项”的哨兵值（区别于扫到的真实 0 字节）
    const ULONGLONG CLEAN_SIZE_UNKNOWN = static_cast<ULONGLONG>(static_cast<LONGLONG>(-1));
    // ---- C-1/C-2: Package Cache 安全剪枝辅助 ----
    struct PkgCacheResult { int nFiles; int nDirs; ULONGLONG ullBytes; };
    bool IsJunkExt(const wchar_t* pszName)
    {
        static const wchar_t* szJunk[] = {
            L".tmp", L".temp", L".log", L".old", L".bak", L".etl"
        };
        for (size_t k = 0; k < sizeof(szJunk)/sizeof(szJunk[0]); ++k)
        {
            size_t nNeed = wcslen(szJunk[k]);
            size_t nName = wcslen(pszName);
            if (nName >= nNeed &&
                lstrcmpiW(pszName + nName - nNeed, szJunk[k]) == 0)
                return true;
        }
        return false;
    }
    void PurgeStaleJunk(const CDuiString& sDir, ULONGLONG ullCutoff100ns,
                        PkgCacheResult& r)
    {
        CDuiString sFind = sDir + L"\\*";
        WIN32_FIND_DATA fd;
        PCFindGuard h(::FindFirstFile(sFind.GetData(), &fd));
        if (h == INVALID_HANDLE_VALUE) return;
        do
        {
            if (lstrcmpW(fd.cFileName, L".") == 0 ||
                lstrcmpW(fd.cFileName, L"..") == 0) continue;
            CDuiString sChild = sDir + L"\\" + fd.cFileName;
            // 永不穿越 reparse point，避免误伤挂载点/junction
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                PurgeStaleJunk(sChild, ullCutoff100ns, r);
                // 递归后若目录已空则删除（RemoveDirectory 仅在空时成功，天然安全）
                if (::RemoveDirectory(sChild.GetData())) ++r.nDirs;
            }
            else if (IsJunkExt(fd.cFileName))
            {
                ULARGE_INTEGER uw;
                uw.LowPart  = fd.ftLastWriteTime.dwLowDateTime;
                uw.HighPart = fd.ftLastWriteTime.dwHighDateTime;
                if (uw.QuadPart < ullCutoff100ns)  // 早于截止时间 → 旧残留（ULONGLONG 比较）
                {
                    ULARGE_INTEGER us;
                    us.LowPart  = fd.nFileSizeLow;
                    us.HighPart = fd.nFileSizeHigh;
                    if (::DeleteFile(sChild.GetData()))
                    {
                        ++r.nFiles;
                        r.ullBytes += us.QuadPart;
                    }
                }
            }
        } while (::FindNextFile(h, &fd));
    }
    // 取 C:\ProgramData（系统盘非 C: 也正确），失败返回 false
    bool GetProgramDataPath(CDuiString& sOut)
    {
        wchar_t sz[MAX_PATH] = { 0 };
        if (::SHGetFolderPath(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, sz) == S_OK
            && sz[0] != 0)
        {
            sOut = sz;
            return true;
        }
        return false;
    }
    // bVsOnly=true 时仅处理 Package Cache 顶层中属于 Visual Studio 的子目录
    void PurgePackageCache(const CDuiString& sRoot, bool bVsOnly,
                           ULONGLONG ullCutoff100ns, PkgCacheResult& r)
    {
        if (!bVsOnly)
        {
            PurgeStaleJunk(sRoot, ullCutoff100ns, r);
            return;
        }
        CDuiString sFind = sRoot + L"\\*";
        WIN32_FIND_DATA fd;
        PCFindGuard h(::FindFirstFile(sFind.GetData(), &fd));
        if (h == INVALID_HANDLE_VALUE) return;
        do
        {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (lstrcmpW(fd.cFileName, L".") == 0 ||
                lstrcmpW(fd.cFileName, L"..") == 0) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            CDuiString sName = fd.cFileName;
            sName.MakeLower();
            if (sName.Find(L"visualstudio") < 0
                && sName.Find(L"visual studio") < 0) continue;
            CDuiString sSub = sRoot + L"\\" + fd.cFileName;
            PurgeStaleJunk(sSub, ullCutoff100ns, r);
        } while (::FindNextFile(h, &fd));
    }
    // 原 CFrameWnd::RequireAdmin 的页内等价（HostInterfaces 无此能力，
    // 不新增宿主接口）：文案/图标/警告级日志逐字一致
    bool RequireCleanAdmin(IPageContext& ctx, const wchar_t* pszAction)
    {
        if (ExternalToolService::IsElevated()) return true;
        CDuiString sMsg;
        sMsg.Format(StringMgr.Get("program.sd90addbf"),
                    pszAction ? pszAction : StringMgr.Get("misc.sb548362e"));
        ToolSql::HostNotify(ctx.HostHwnd(), sMsg.GetData(), StringMgr.Get("misc.safa4b723"), MB_OK | MB_ICONWARNING);
        ctx.HostLog(sMsg.GetData(), static_cast<int>(ELogLevel::LOG_WARN));
        return false;
    }
}

// ============================================================

CPageCleanup::CPageCleanup(IPageContext& ctx)

    : CPageBase(ctx)

    , m_bCleanScanned(false)

    , m_nCleanScannedCnt(0)

    , m_bCleanShowZero(false)

    , m_bCleanHasReport(false)

    , m_ullCleanBeforeBytes(0)

    , m_nLastCleanFailed(0)

    , m_bLastCleanPartial(false)

    , m_ullLastCleanFreed(0)

    {
    for (int ei = 0; ei < 6; ++ei) m_nScanErrStats[ei] = 0;
}

void CPageCleanup::AppendLog(const wchar_t* pszLine)

{
    m_ctx.HostLog(pszLine, LogService::DetectLevel(pszLine));
}

// ============================================================
// build cleanup page (dynamic from Data.xml, light theme)
// ============================================================

// [R4] 加速键/CLI 命令收口（原壳 OnAccelCommand 中 4 处清理页直调）
bool CPageCleanup::OnHostCommand(int id)
{
    switch (id)
    {
    case EAccelCmd::ID_ACCEL_SCAN:   StartScan();         return true;   // F9
    case EAccelCmd::ID_ACCEL_CLEAN:  StartClean();        return true;   // F10
    case EAccelCmd::ID_ACCEL_CANCEL:                       // Esc
    case EAccelCmd::ID_ACCEL_BREAK:  RequestCancelScan(); return true;   // Ctrl+Break
    // [R5] CLI /C 未带 /All 时按默认选择规则
    case EHostCmd::HCMD_CLEAN_SELECT_DEFAULT: CleanSelectDefault(); return true;
    default: return false;
    }
}

// ============================================================
// [R5] 扫描线程消息 + WM_CLEAN_DONE 两段时序统一入口
// （原壳 OnPgScanItem/OnPgScanDone/OnPgExtendScanDone 与
// HCM 内 WM_CLEAN_DONE 分支）。CleanDoneMsg 所有权同 kMsgOwnPage：
// 投递方 PostMessage 失败才自行 delete，成功后由 OnCleanDone 内 delete；
// 该消息不入 OnAsyncMessage，仅因壳需在两段之间插 RestartExplorer。
// ============================================================

bool CPageCleanup::OnAsyncMessage(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_SCAN_ITEM:
        // kMsgOwnPage：ScanItemMsg* 由 OnScanItem 释放；ullSize 值随结构
        // 传递，保 x86 64 位防撕裂（注释见原 HCM）。
        OnScanItem((ScanItemMsg*)lParam);
        return true;
    case WM_SCAN_DONE:
        // kMsgValue：lParam = 失败规则计数。
        OnScanDone(static_cast<int>(lParam));
        return true;
    case WM_CLEAN_DISMTAIL:
        // [AUDIT-P0] kMsgOwnPage：TPendingCleanTail* 所有权移交 OnCleanDismTail。
        OnCleanDismTail(msgparam::FromParam<TPendingCleanTail>(lParam));
        return true;
    case WM_CLEAN_PROGRESS:
        // [R-CLEANPROG] kMsgOwnPage：CleanProgressMsg* 由 OnCleanProgress 释放。
        OnCleanProgress((CleanProgressMsg*)lParam);
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool CPageCleanup::HostOnCleanDone(LPARAM lParam)
{
    return OnCleanDone((CleanDoneMsg*)lParam);
}

// [R-CLEANPROG] 删除阶段进度落地（UI 线程）：会话条 + 任务栏 + 状态条百分比。
// 所有权：kMsgOwnPage —— 本函数 delete 载荷。
void CPageCleanup::OnCleanProgress(CleanProgressMsg* pMsg)
{
    std::unique_ptr<CleanProgressMsg> guard(pMsg);
    if (!pMsg) return;
    // [FIX-TAIL-PROG] 尾巴阶段（nPhase=2）：步骤描述直显状态条。
    // CBS 事务无内部百分比，用"步骤推进"表达进度（进度条按 done/total 走格）。
    if (pMsg->nPhase == 2)
    {
        CDuiString s;
        if (!pMsg->sStage.empty())
            s.Format(L"[组件清理] %s", pMsg->sStage.c_str());
        else
            s = L"[组件清理] 正在执行收尾动作……";
        m_ctx.HostSetCtrlText(L"sess_status", s.GetData());
        m_ctx.HostProgress(pMsg->nDone > 0 ? pMsg->nDone : 0,
                           pMsg->nTotal > 0 ? pMsg->nTotal : 1);
        return;
    }
    if (pMsg->nTotal <= 0) return;
    if (pMsg->nDone < 0) return;
    const int nPct = static_cast<int>(
        (static_cast<long long>(pMsg->nDone) * 100) / pMsg->nTotal);
    m_ctx.HostProgress(pMsg->nDone, pMsg->nTotal);
    const wchar_t* pszPhase = pMsg->nPhase == 0
        ? StringMgr.Get("phase.revalidating")
        : StringMgr.Get("phase.deleting");
    CDuiString s;
    s.Format(L"[%s] %s %d%% (%d/%d)", pszPhase,
             StringMgr.Get("cleanup.s89e99d91"),
             nPct, pMsg->nDone, pMsg->nTotal);
    m_ctx.HostSetCtrlText(L"sess_status", s.GetData());
}

void CPageCleanup::HostOnCleanDoneTail()
{
    OnCleanDoneTail();
}

// ============================================================
// scan / clean busy UI
// ============================================================

// ============================================================
// scan actions（UI 段留页；线程段入 CleanJobService）
// ============================================================

void CPageCleanup::StartScan()

{
    if (CleanJobService::IsScanning()) { AppendLog(L"scan already running"); return; }
    if (CleanJobService::IsDeleting() || CleanJobService::HasThread())
    {
        AppendLog(L"[clean] cannot scan while cleanup is running");
        return;
    }
    CleanJobService::BeginScan();   // 原 m_bScanning = true; m_bScanCancel = 0;
    m_nCleanScannedCnt = 0;
    m_bCleanScanned = false;   // 重新扫描期间以 i<m_nCleanScannedCnt 为准
    // [A3] 清零上一轮的错误分类统计
    for (int ei = 0; ei < static_cast<int>(ScanRuleError::SCAN_RULE_ERROR_COUNT); ++ei) m_nScanErrStats[ei] = 0;
    // [修复 #23] 重新扫描：UI 快照全部复位为未知（等 WM_SCAN_ITEM 重新投递）
    m_vUiSize.assign(m_ctx.CleanEngine().GetRuleCount(), CLEAN_SIZE_UNKNOWN);
    SetWorkerUiBusy(true, true);
    // S-14: 状态条前缀加"[扫描中]"阶段标签
    CDuiString sScanPhase;
    sScanPhase.Format(L"[%s] %s", StringMgr.Get("phase.scanning"),
                      StringMgr.Get("cleanup.s0e48e426"));
    m_ctx.HostSetCtrlText(L"sess_status", sScanPhase.GetData());
    // [E-6] 进度总量按 Scan@Work 权重求和；[OS-Filter] 只累计 bOsApplicable
    // 规则——被筛掉的条目不进扫描循环，计入分母会让进度条到不了 100%。
    int nTotalWork = 0; int nScanRules = 0;
    for (int wi = 0; wi < m_ctx.CleanEngine().GetRuleCount(); ++wi)
    {
        if (!m_ctx.CleanEngine().GetRule(wi).bOsApplicable) continue;
        nTotalWork += m_ctx.CleanEngine().GetRule(wi).nWeight; ++nScanRules;
    }
    if (nTotalWork <= 0) nTotalWork = nScanRules > 0 ? nScanRules : 1;
    m_ctx.HostProgress(0, nTotalWork);
    AppendLog(L"[clean] scan started");
    if (!CleanJobService::LaunchScan(m_ctx.HostHwnd(), &m_ctx.CleanEngine(), nTotalWork))
    {
        // CreateThread 失败：复位状态，否则按钮与标志永久锁死（服务内已复位扫描态）
        SetWorkerUiBusy(false);
        m_ctx.HostSetCtrlText(L"sess_status", m_ctx.HostStatusReady().GetData());
        m_ctx.HostProgress(0, 0);
        AppendLog(L"[clean] scan failed: CreateThread returned nullptr");
        return;
    }
}

// [AUDIT-F2] 形参 vSel = 本轮真正勾选的规则（调用点已移到收集之后）。
// [R7] 多卷采样：C: 用于兼容既有日志；同时按本次勾选规则的实际目标
// 收集所有涉及的卷，清理后逐卷对比，避免"只统计系统盘"的失真。
void CPageCleanup::ScSampleDiskBefore(const std::vector<int>& vSel)
{
    using PfnGDSE = BOOL ( WINAPI*)(wchar_t*, PULARGE_INTEGER, PULARGE_INTEGER, PULARGE_INTEGER, void*);
    // [FIX-DS-3] GetProcAddress(nullptr,...) 恒返回 NULL：磁盘采样恒为 0、
    // 清理前后空间统计失效。先取 kernel32 模块句柄再查过程地址。
    PfnGDSE pfnG = (PfnGDSE)::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "GetDiskFreeSpaceExW");
    auto FreeOf = [&](const wchar_t* pszRoot) -> ULONGLONG {
        if (!pfnG || !pszRoot) return 0;
        ULARGE_INTEGER liT = { 0 }, liF = { 0 }, liA = { 0 };
        pfnG(const_cast<wchar_t*>(pszRoot), &liA, &liT, &liF, nullptr);
        return static_cast<ULONGLONG>(liF.QuadPart);
    };

    m_ullCleanBeforeBytes = FreeOf(L"C:\\");
    {
        CDuiString sBefore;
        sBefore.Format(StringMgr.Get("misc.h8b23"), m_ullCleanBeforeBytes / (1024*1024));
        AppendLog(sBefore.GetData());
    }

    m_vVolBefore.clear();
    std::vector<CDuiString> vRoots;
    for (size_t si = 0; si < vSel.size(); ++si)
    {
        const CleanRule& r = m_ctx.CleanEngine().GetRule(vSel[si]);
        for (size_t ti = 0; ti < r.targets.size(); ++ti)
        {
            const CDuiString& sPath = r.targets[ti].sPath;
            if (sPath.GetLength() < 2 || sPath[1] != L':') continue;
            CDuiString sRoot;
            sRoot.Format(L"%c:\\", sPath[0]);
            bool bSeen = false;
            for (size_t vi = 0; vi < vRoots.size(); ++vi)
                if (lstrcmpiW(vRoots[vi].GetData(), sRoot.GetData()) == 0) { bSeen = true; break; }
            if (!bSeen) vRoots.push_back(sRoot);
        }
    }
    for (size_t vi = 0; vi < vRoots.size(); ++vi)
    {
        VolumeBefore vb;
        vb.sRoot = vRoots[vi];
        vb.ullFree = FreeOf(vRoots[vi].GetData());
        m_vVolBefore.push_back(vb);
    }
}

// [R14-02] StartClean 规则收集：遍历勾选规则 → sel / CustomProc / 通知 bool
// [R14-02] 空选择检查：bHasProc 传入保持原 3 分支语义；返回 false = 用户取消
[[nodiscard]] bool CPageCleanup::ScCheckSelection(const SCCollect& r)
{
    bool bHasProc = (CleanJobService::CountCustomProc() > 0);
    if (r.bFileChecked && r.sel.empty() && !bHasProc)
    {
        ToolSql::HostNotify(m_ctx.HostHwnd(), StringMgr.Get("cleanup.sbe1d6bfc"), StringMgr.Get("cleanup.system"), MB_OK | MB_ICONINFORMATION);
        return false;
    }
    if (r.sel.empty() && !bHasProc)
    {
        ToolSql::HostNotify(m_ctx.HostHwnd(), StringMgr.Get("cleanup.s8cc4be5a"), StringMgr.Get("cleanup.system"), MB_OK | MB_ICONINFORMATION);
        return false;
    }
    if (r.bFileChecked && r.sel.empty() && bHasProc)
    {
        // 文件规则尚未扫描（无目标）时仅执行已勾选的 Custom 动作，
        // 不再整个 return 把 Custom 一并挡掉。
        AppendLog(L"[clean] file rules not scanned yet; only custom action(s) will run");
    }
    if (r.nHiddenSkipped > 0)
    {
        CDuiString sWarn;
        sWarn.Format(StringMgr.Get("cleanup.s5ac66468"),
                     r.nHiddenSkipped);
        AppendLog(sWarn.GetData());
    }
    return true;
}

// [R14-02] 清理前确认：汇总本次会执行规则的 Warning 文案一次性确认；
// 无 Warning 时对 Level 0 兜底通用风险确认。返回 false = 用户取消。
[[nodiscard]] bool CPageCleanup::ScConfirmWarnings(SCCollect& r)
{
    std::vector<CDuiString> vWarnings;
    for (size_t wj = 0; wj < r.vWarnRules.size(); ++wj)
        m_ctx.CleanEngine().CollectRuleWarnings(r.vWarnRules[wj], vWarnings);
    if (!vWarnings.empty())
    {
        CDuiString sMsg(StringMgr.Get("misc.hf513"));
        constexpr int MAX_WARN_LINES = 15;
        int nShown = 0;
        for (size_t wi = 0; wi < vWarnings.size() && nShown < MAX_WARN_LINES; ++wi)
        {
            sMsg += L"• ";
            sMsg += vWarnings[wi];
            sMsg += L"\n";
            ++nShown;
        }
        if (static_cast<int>(vWarnings.size()) > MAX_WARN_LINES)
        {
            CDuiString sMore;
            sMore.Format(StringMgr.Get("misc.hbc9a"),
                         static_cast<int>(vWarnings.size()) - MAX_WARN_LINES);
            sMsg += sMore;
        }
        sMsg += StringMgr.Get("misc.hfa25");
        int nBtn = ToolSql::HostConfirm(m_ctx.HostHwnd(), sMsg.GetData(), StringMgr.Get("general.confirm"), MB_YESNO, MB_ICONWARNING);
        if (nBtn != IDYES) return false;
    }
    else
    {
        // 无 Warning 文案时的兜底：勾选了 Level 0（重型常驻项）仍弹通用风险确认
        bool bRisk = false;
        for (size_t k = 0; k < r.sel.size(); ++k)
            if (m_ctx.CleanEngine().GetRule(r.sel[k]).nLevel == 0) bRisk = true;
        if (bRisk)
        {
            int nBtn = ToolSql::HostConfirm(m_ctx.HostHwnd(), StringMgr.Get("confirm.s617ed6aa"), StringMgr.Get("general.confirm"), MB_YESNO, MB_ICONWARNING);
            if (nBtn != IDYES) return false;
        }
    }
    return true;
}

// [R14-02] 启动清理线程：BeginClean + LaunchClean + 状态条 + 失败恢复 + 队列提示
void CPageCleanup::ScLaunchClean(SCCollect& r)
{
    CleanJobService::BeginClean();   // 原 m_bDeleting = true
    m_vLastCleanSel = r.sel;   // 报告仅统计本次真正勾选且可见的规则
    SetWorkerUiBusy(true);
    // [R-CLEANPROG] 初始化删除阶段进度条：分母=重评权重和（线程首个
    // WM_CLEAN_PROGRESS 到达后即被替换为各阶段真实分母）。
    m_ctx.HostProgress(0, r.nFileWork > 0 ? r.nFileWork
                       : (r.sel.empty() ? 1 : static_cast<int>(r.sel.size())));
    // S-14: 状态条前缀加"[删除中]"阶段标签
    CDuiString sDelPhase;
    sDelPhase.Format(L"[%s] %s", StringMgr.Get("phase.deleting"),
                     StringMgr.Get("cleanup.s89e99d91"));
    m_ctx.HostSetCtrlText(L"sess_status", sDelPhase.GetData());
    AppendLog(L"[clean] deleting targets permanently...");
    // 4 个通知 bool 在 UI 收集阶段已算好，作为快照入线程 Ctx，
    // 线程结束写入 CleanDoneMsg（不再作壳/页成员）。
    // [R88] 主规则清理默认走回收站（FOF_ALLOWUNDO），支持一键撤销
    // [R-CLEANPROG] r.nFileWork：重评阶段进度分母（权重和，与扫描同口径）
    if (!CleanJobService::LaunchClean(m_ctx.HostHwnd(), &m_ctx.CleanEngine(), r.sel,
            CleanJobService::GetCustomProc(CustomProcIdx::CPI_DISMCOMP), r.bDismResetBase,
            r.bCleanAssocNotify, r.bCleanRestartExplorer, true, r.nFileWork))
    {
        // CreateThread 失败（服务内已删除 Ctx 并复位清理态）：恢复 UI
        SetWorkerUiBusy(false);
        m_ctx.HostSetCtrlText(L"sess_status", m_ctx.HostStatusReady().GetData());
        AppendLog(L"[clean] failed: CreateThread returned nullptr");
        return;
    }
    // Custom ProcName queue — dispatches in WM_CLEAN_DONE after file deletion finishes
    int nQueued = CleanJobService::CountCustomProc();
    if (nQueued > 0)
    {
        CDuiString sNote; sNote.Format(L"[clean] %d custom action(s) queued after file deletion", nQueued);
        AppendLog(sNote.GetData());
    }
}

// [R14-02] StartClean 拆解后的薄壳：前置守卫 + 4 段子流程
void CPageCleanup::StartClean()
{
    if (!RequireCleanAdmin(m_ctx, StringMgr.Get("cleanup.sc00fc14b"))) return;
    if (CleanJobService::IsDeleting()) { AppendLog(L"clean already running"); return; }
    if (CleanJobService::IsScanning() || CleanJobService::HasThread())
    {
        AppendLog(L"[clean] cannot clean while scan is running");
        return;
    }
    // [R17] 新一轮清理开始：复位收尾标志。
    // 若不复位，首次清理把 m_bCleanTailFinalized 置 true 后，后续每次清理
    // OnCleanDoneTail 都会在首行直接 return，导致 UI 不解锁、不显示结果气泡。
    // [R160] 双标志复位走单入口（原先两行裸写与 Tail 侧的 4 处写点各管一段协议）
    ResetTailGuards();
    CleanJobService::ResetCustomJobs();   // 原 for(k)m_aCustomProc[k]=false + m_vCustomJobs.clear()
    SCCollect r;
    ScCollectCheckedRules(r);
    if (!ScCheckSelection(r)) return;
    if (!ScConfirmWarnings(r)) return;
    // [AUDIT-F2] 磁盘"清理前"采样移到本轮勾选收集之后，并显式传入本轮
    // 勾选规则：原实现在收集前调用，多卷清单读的是上一次清理遗留的
    // m_vLastCleanSel（首轮为空 → 恒只采样 C:），逐卷释放量统计失真。
    // 采样仍发生在 LaunchClean 之前，"清理前"语义不变。
    ScSampleDiskBefore(r.sel);
    ScLaunchClean(r);
}

// ============================================================
// [R-B52] CbsHostBridge 接线辅助：在线组件清理
// ------------------------------------------------------------
// 清单维度 3 缺失 1『CbsHostBridge 零外部调用』：6 导出语义层
// 已编译但无人上桥。此处把 Page_Cleanup 的 DISM 家族 Custom Proc
// （DismHardLinkMerge / WinSxS 组件清理）改调 CbsHost::DispatchCommand
// 的 0x60001 Cleanup 子命令，从而让语义层真实可达：
//   * 内部优先 DismApiBridge::CleanComponentStore（dismapi.dll 原生）；
//   * 失败回退 LaunchDism 外部命令（路线 A 允许的等价降级）。
// 返回 0 表示成功；非 0 表示失败（含 dismapi 不可用且外部命令回退也失败）。
// ============================================================

// C-1/C-2: Package Cache 安全剪枝实现
void CPageCleanup::CleanPackageCacheSafe(bool bVsOnly)

{
    CDuiString sPD;
    if (!GetProgramDataPath(sPD))
    {
        AppendLog(StringMgr.Get("error.s715b8161"));
        return;
    }
    CDuiString sRoot = sPD + L"\\Package Cache";
    if (::GetFileAttributes(sRoot.GetData()) == INVALID_FILE_ATTRIBUTES)
    {
        CDuiString sLog;
        sLog.Format(StringMgr.Get("cleanup.scf35d57e"), sRoot.GetData());
        AppendLog(sLog.GetData());
        return;
    }
    CDuiString sAsk;
    sAsk.Format(
        CDuiString(StringMgr.Get("cleanup.sd4a99bba")) + StringMgr.Get("cleanup.sf27d1d66") + StringMgr.Get("edit.s2b4d6815"),
        StringMgr.Get("confirm.continue"),
        sRoot.GetData(), bVsOnly ? StringMgr.Get("hint.sa7d8d89f") : L"");
    if (ToolSql::HostConfirm(m_ctx.HostHwnd(), sAsk.GetData(), StringMgr.Get("cleanup.s2bcd693e"), MB_YESNO, MB_ICONINFORMATION) != IDYES)
    {
        AppendLog(L"[clean] Package Cache cleanup skipped by user");
        return;
    }
    FILETIME ftNow;
    ::GetSystemTimeAsFileTime(&ftNow);
    ULARGE_INTEGER uwNow;
    uwNow.LowPart  = ftNow.dwLowDateTime;
    uwNow.HighPart = ftNow.dwHighDateTime;
    const ULONGLONG DAY100NS = 86400ULL * 10000000ULL;
    ULONGLONG ullCutoff = uwNow.QuadPart - 30ULL * DAY100NS;  // 30 天前
    AppendLog(bVsOnly
        ? L"[clean] pruning Visual Studio Package Cache stale residues..."
        : L"[clean] pruning ProgramData Package Cache stale residues...");
    PkgCacheResult r; r.nFiles = 0; r.nDirs = 0; r.ullBytes = 0;
    PurgePackageCache(sRoot, bVsOnly, ullCutoff, r);
    CDuiString sSize;
    CCleanEngine::FormatSize(r.ullBytes, sSize);
    CDuiString sDone;
    sDone.Format(L"[clean] Package Cache safe cleanup done: %d files, %d empty dirs, freed %s",
                 r.nFiles, r.nDirs, sSize.GetData());
    AppendLog(sDone.GetData());
}


// ============================================================
// Notify / 加速键入口
// ============================================================

void CPageCleanup::OnScanClicked()

{
    // [R17] 扫描进行中该按钮充当“停止扫描”
    if (CleanJobService::IsScanning())
    {
        CleanJobService::RequestCancel();
        if (CButtonUI* p = static_cast<CButtonUI*>(m_ctx.HostFindControl(L"btn_scan")))
            p->SetText(StringMgr.Get("status.stopping"));
        m_ctx.HostSetCtrlText(L"sess_status", StringMgr.Get("cleanup.s432ac3b6"));
        AppendLog(L"[clean] scan cancel requested");
    }
    else
    {
        StartScan();
    }
}

void CPageCleanup::RequestCancelScan()

{
    if (CleanJobService::IsScanning() && !CleanJobService::CancelRequested())
    {
        CleanJobService::RequestCancel();
        m_ctx.HostSetCtrlText(L"sess_status", StringMgr.Get("cleanup.s432ac3b6"));
        AppendLog(L"[clean] scan cancel requested (Esc)");
    }
}

void CPageCleanup::OnFilterChanged(const wchar_t* pszText)

{
    m_sCleanFilter = pszText;
    ApplyCleanFilter();
}

void CPageCleanup::OnShowZeroChanged(bool bShow)

{
    m_bCleanShowZero = bShow;
    ApplyCleanFilter();
}

// ============================================================
// 工作线程消息处理（壳 HandleCustomMessage 转发；结构本页 delete）
// ============================================================

// ============================================================
// [R126-1] 扫描完成后组内按大小降序重排
// 仅调整 clnbody_%d 内 clnrow_%d 的显示次序，不改控件名，因此
// clnrow_/ckclean_/crisk_/csz_ 与 CleanRule 索引的映射保持不变，
// 后续 ApplyCleanFilter / SyncCleanGroupCheck / OnCleanRowCheck
// 仍按 i 索引定位控件，无需改动。
// ============================================================
void CPageCleanup::OnScanDone(int nRuleFailed)

{
    CleanJobService::DetachThread();
    bool bCancelled = CleanJobService::CancelRequested();
    CleanJobService::EndScan();
    int nScanRules = 0;
    ScdUpdateScanState(nRuleFailed, bCancelled, nScanRules);
    if (bCancelled)
        ScdCancelReport(nScanRules, nRuleFailed);
    else
    {
        if (nRuleFailed > 0)
            ScdFailureReport(nScanRules, nRuleFailed);
        else
            ScdSuccessReport();
        // [R18 P0] 扫描完成气泡（汇总可回收空间）
        // [R126-1] 扫描完成后组内按大小降序重排（显示次序，不改索引）
        SortCleanRows();
        ScdScanBalloon(nScanRules, nRuleFailed);
    }
    ScdFinishUi();
}



// ============================================================
// P8：第二级事件路由（原 CFrameWnd::Notify 清理页分支逐字迁入；
// 仅把宿主控件查找改为 m_ctx.HostFindControl）
// ============================================================

void CPageCleanup::OnEvent(TNotifyUI& msg)
{
    // [AUDIT-P2] 原 71 行 / CC=27，按事件类型拆为 4 个处理函数：
    //   ClnOnClick / ClnOnDoubleClick / ClnOnFilterChanged / ClnOnSelectChanged。
    // 路由语义逐字保留（按钮表、clngrp_/clnrow_ 前缀、组勾选三态联动）。
    CDuiString sName = msg.pSender ? msg.pSender->GetName() : CDuiString(L"");
    if (msg.sType == L"click")
    {
        ClnOnClick(msg, sName);
        return;
    }
    if (msg.sType == L"dbclick")
    {
        ClnOnDoubleClick(sName);
        return;
    }
    if (msg.sType == L"textchanged")
    {
        ClnOnFilterChanged(msg, sName);
        return;
    }
    if (msg.sType == L"selectchanged")
    {
        ClnOnSelectChanged(msg, sName);
        return;
    }
}

// [AUDIT-P2] click 分支：按钮表 + 分组折叠 + 行说明
// [R165] 按钮分派表（从 ClnOnClick 抽出，原 11 段 if-else 链，CC16）。
// 表放在成员函数体内：成员函数指针取地址在类作用域外是非法访问。
// 所有处理函数均为 void() 无参——若有带默认参数的成员，须加零参薄壳
// （第 51/56 轮的 C2440 教训）。
bool CPageCleanup::ClnDispatchButton(const CDuiString& sName)
{
    using BtnHandler = void (CPageCleanup::*)();
    struct BtnRoute { const wchar_t* pszName; BtnHandler pfn; };
    static constexpr BtnRoute kBtns[] = {
        { L"btn_scan",              &CPageCleanup::OnScanClicked },          // 扫描
        { L"btn_clean",             &CPageCleanup::StartClean },             // 开始清理
        { L"btn_cln_default",       &CPageCleanup::CleanSelectDefault },     // 恢复默认勾选
        { L"btn_cln_refresh",       &CPageCleanup::Build },                  // 重建列表
        { L"btn_cln_export",        &CPageCleanup::ExportCleanReport },      // 导出报告
        { L"btn_cln_orphan",        &CPageCleanup::OnExtendOrphanScan },     // 孤立项扫描
        { L"btn_cln_undo",          &CPageCleanup::OnExtendUndoLast },       // 撤销上次清理
    };
    for (const BtnRoute& r : kBtns)
    {
        if (sName != r.pszName) continue;
        (this->*r.pfn)();
        return true;
    }
    return false;
}

void CPageCleanup::ClnOnClick(TNotifyUI& msg, const CDuiString& sName)
{
    // 1) 固定按钮名 → 表驱动
    if (ClnDispatchButton(sName)) return;
    // 2) "显示零字节项"复选框：需要 pSender 的勾选态，不能入无参表
    // [U-9 已移除] 定期清理计划任务（btn_clean_sched_save / btn_clean_sched_del / clean_sched_enable）
    if (sName == L"clean_showzero")
    {
        CCheckBoxUI* p = static_cast<CCheckBoxUI*>(msg.pSender);
        OnShowZeroChanged(p && p->GetCheck());
        return;
    }
    // 3) 前缀命中：组标题 → 切换折叠；行 → 显示描述
    const wchar_t* pszName = sName.GetData();
    if (wcsncmp(pszName, L"clngrp_", 7) == 0)
    {
        ToggleCleanGroup(_ttoi(pszName + 7));
        return;
    }
    if (wcsncmp(pszName, L"clnrow_", 7) == 0)
    {
        const int nRow = _ttoi(pszName + 7);
        m_nClnLastRow = nRow;   // [G4] 供键盘唤出右键菜单时定位
        ShowCleanDesc(nRow);
    }
}

// [AUDIT-P2] 双击行 → 该页主操作（清理页：复制项说明到日志）
void CPageCleanup::ClnOnDoubleClick(const CDuiString& sName)
{
    int a = -1;
    if (ParsePrefix1(sName, L"clnrow_", a)) { ShowCleanDesc(a); return; }
}

// [AUDIT-P2] 关键字过滤框变更
void CPageCleanup::ClnOnFilterChanged(TNotifyUI& msg, const CDuiString& sName)
{
    if (sName == L"clean_filter")
    {
        CEditUI* pEdit = static_cast<CEditUI*>(msg.pSender);
        if (pEdit) OnFilterChanged(pEdit->GetText());
    }
}

// [AUDIT-P2] 勾选态变更：组复选框（三态同步）/ 行复选框
void CPageCleanup::ClnOnSelectChanged(TNotifyUI& msg, const CDuiString& sName)
{
    int a = -1, b = -1;
    if (ParsePrefix1(sName, L"grpck_", a))
    {
        CCheckBoxUI* p = static_cast<CCheckBoxUI*>(msg.pSender);
        OnGroupCheck(a, p && p->GetCheck());
        return;
    }
    if (ParsePrefix1(sName, L"ckclean_", a))
    {
        // P6b：勾选文本刷新 + 未接通 Custom 提示 + 组三态同步全在页内
        CCheckBoxUI* p = static_cast<CCheckBoxUI*>(msg.pSender);
        OnCleanRowCheck(p, a);
        return;
    }
}

// P8：原 CFrameWnd WM_RBUTTONUP 通用列表命中块（clean_list 分支；
// 仅 WM_RBUTTONUP 生效，WM_CONTEXTMENU 键盘路径原壳不放行）
// [R165] 右键命中行号解析（从 OnContextMenu 抽出）。
// [R85 P1-13] 命中控件可能是 clnrow_ / ckclean_ / csz_ / crisk_ 任一前缀，
// 沿父链向上找第一个能解析出序号的控件；都不命中返回 -1。
int CPageCleanup::ClnResolveRowFromHit(CControlUI* pHit)
{
    int nRow = -1;
    for (CControlUI* q = pHit; q; q = q->GetParent())
    {
        const wchar_t* pn = q->GetName();
        if (!pn) continue;
        if (ParsePrefix1(pn, L"clnrow_", nRow)) return nRow;
        if (ParsePrefix1(pn, L"ckclean_", nRow)) return nRow;
        if (ParsePrefix1(pn, L"csz_", nRow)) return nRow;
        if (ParsePrefix1(pn, L"crisk_", nRow)) return nRow;
    }
    return -1;
}

bool CPageCleanup::OnContextMenu(POINT pt, bool bFromContextMsg)
{
    // [AUDIT-F1] 忙碌期禁弹右键菜单：扫描线程与清理重评线程都会对
    // rule.targets 做清空重建（ScanRule 首行 targets.clear() 后重扫），
    // 本函数尾部会读 rulePath.targets 取"打开所在位置"的路径，与工作
    // 线程构成无锁数据竞争——vector 扩容期读取旧指针属堆读越界（AV）。
    // SetWorkerUiBusy 只禁按钮/导航，不禁列表右键，必须在此显式拦截。
    if (CleanJobService::IsBusy()) return false;
    // [G4 2026-10-25] 键盘路径（Shift+F10 / 菜单键）原直接 return false，
    //   导致属性、复制、打开所在位置对键盘用户整片不可达。
    //   键盘触发时 pt 为 (-1,-1)，FindControl 必然落空，故用「最近交互行」
    //   合成锚点；没有最近行（还没点过任何行）才真正放弃。
    if (bFromContextMsg)
    {
        if (m_nClnLastRow < 0 || m_nClnLastRow >= m_ctx.CleanEngine().GetRuleCount())
            return false;
        CDuiString sRowName;
        sRowName.Format(L"clnrow_%d", m_nClnLastRow);
        CControlUI* pRow = m_ctx.HostFindControl(sRowName.GetData());
        if (!pRow) return false;          // 行被过滤隐藏：不弹菜单
        const RECT& rcRow = pRow->GetPos();
        POINT ptAnchor = { rcRow.left + 24, rcRow.top + (rcRow.bottom - rcRow.top) / 2 };
        ::ClientToScreen(m_ctx.HostHwnd(), &ptAnchor);
        pt = ptAnchor;
    }
    CControlUI* pHit = m_ctx.HostFindControlAt(pt);
    // 先确认命中的祖先链里有 clean_list（否则不是本页列表的右键）
    bool bInList = false;
    for (CControlUI* q = pHit; q; q = q->GetParent())
    {
        const wchar_t* pn = q->GetName();
        if (pn && lstrcmpW(pn, L"clean_list") == 0) { bInList = true; break; }
    }
    if (!bInList) return false;
    const int nRow = ClnResolveRowFromHit(pHit);
    // 取该行首个目标路径，供右键菜单"打开所在位置"定位
    CDuiString sPath;
    if (nRow >= 0 && nRow < m_ctx.CleanEngine().GetRuleCount())
    {
        CleanRule& rulePath = m_ctx.CleanEngine().GetRule(nRow);
        if (!rulePath.targets.empty())
            sPath = rulePath.targets[0].sPath;
    }
    UiHelpers_ShowUnifiedListContextMenuPath(&m_ctx, m_ctx.HostHwnd(),
                                       EListCtx::LCX_CLEAN, nRow, pt,
                                       sPath.IsEmpty() ? nullptr : sPath.GetData(),
                                       // [AUDIT-F3] 全选/反选接入真实动作：
                                       // 原实现这两个菜单项只写一行日志，
                                       // 用户点击后无任何效果（死菜单）。
                                       [this]() { ToggleSelectAll(); },
                                       [this]() { CleanSelectInvert(); },
                                       // [AUDIT-G2] 详情接真：行命中时传
                                       // ShowCleanDesc，空白处(nRow<0)不传
                                       // →「详情」项自动不装配（假动作屏蔽）。
                                       nRow >= 0
                                           ? std::function<void()>([this, nRow]() { ShowCleanDesc(nRow); })
                                           : std::function<void()>());
    return true;
}

// ============================================================
// [R-B1] Ctrl+A 清理页全选/全不选
// ============================================================

void CPageCleanup::OnGlobalSelectAll()
{
    ToggleSelectAll();
    AppendLog(L"[cleanup] Ctrl+A toggle all");
}

// ============================================================
// [R-B1] Ctrl+S 清理页导出报告
// ============================================================

void CPageCleanup::OnGlobalSave()
{
    ExportCleanReport();
    AppendLog(L"[cleanup] Ctrl+S export report");
}
