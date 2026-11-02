// P6b：本编译单元不使用预编译头（四配置 NotUsing），仿 AppxCapUpdService：
// 不 include stdafx.h / UIlib.h / StringManager.h，不使用任何文案。


#include "LogService.h"   // [M-6] 统一日志目录
#include <stdarg.h>   // va_list（LogScanLine 可变参数）
#include "FormatW.h"
#include <cstdio>   // [C17] _vsnwprintf_s（F18 tchar.h 清零）
#include <vector>
#include <utility>
#include <functional>   // [R-CLEANPROG] DeleteRules 进度回调
#include <climits>      // [R-CLEANPROG] INT_MAX 钳制
#include <memory>   // [C++17 R3] make_unique / unique_ptr
#include <windows.h>
#ifndef UILIB_API
#define UILIB_API __declspec(dllimport)
#endif
#include "Utils/Utils.h"
#include "Core/UIMarkup.h"   // CMarkupNode（CleanEngine.h 依赖前置头）
// P6b：CleanEngine.h 内以裸名使用 CDuiString（PCH 场景由 FrameWnd.h 的
// using namespace DuiLib 兜底）；本 TU 不用 PCH，需在此显式引入。
using namespace DuiLib;
#include "CleanEngine.h"
#include "WmMessages.h"   // WM_SCAN_ITEM / WM_SCAN_DONE / WM_CLEAN_DONE / WM_DISM_LOG
#include "SafeThread.h"          // 工作线程顶层异常边界
#include "CleanJobService.h"
// [C++17 阶段2] 局部 RAII 守卫：日志文件句柄（见 WinHandles.h 使用规范）
struct CJSFileGuard
{
    HANDLE h = INVALID_HANDLE_VALUE;
    CJSFileGuard() = default;
    explicit CJSFileGuard(HANDLE x) : h(x) {}
    ~CJSFileGuard() { if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h); }
    CJSFileGuard(const CJSFileGuard&) = delete;
    CJSFileGuard& operator=(const CJSFileGuard&) = delete;
    operator HANDLE() const { return h; }
    [[nodiscard]] bool valid() const { return h && h != INVALID_HANDLE_VALUE; }
};


// ============================================================
// 文件内状态（原 CFrameWnd 值成员；单实例 UI 程序，文件 static 等价）
// ============================================================
namespace
{
    bool           s_bScanning = false;
    bool           s_bDeleting = false;
    volatile LONG  s_bScanCancel = 0;   // [R17] UI 置位，扫描线程每条规则前检查
    HANDLE         s_hWorkerThread = nullptr;   // 扫描/清理共用的唯一工作线程句柄
    bool           s_aCustomProc[CUSTOM_PROC_COUNT] = { 0 };
    std::vector<std::pair<CustomProcIdx,int> > s_vCustomJobs;

    // [FIX-P1B] 陈旧线程自愈（P1-10/P1-12）：上次完成消息（WM_SCAN_DONE/
    // WM_CLEAN_DONE）Post 失败丢失时，线程其实已结束，但句柄与忙碌标志
    // 残留 —— 旧实现下句柄被下次 Launch 覆盖泄漏、标志永久锁死按钮。
    // 线程已死（Wait 0 即返回）时回收句柄并复位标志是安全的。
    void SelfHealStaleWorkerThread()
    {
        HANDLE h = s_hWorkerThread;
        if (h && ::WaitForSingleObject(h, 0) == WAIT_OBJECT_0)
        {
            ::CloseHandle(h);
            s_hWorkerThread = nullptr;
            s_bScanning = false;
            s_bDeleting = false;
        }
    }
    // [FIX-P1B] 完成消息有限重试（P1-10）：Post 偶发失败（队列瞬时满）时
    // 退避重试；窗口确已销毁或重试耗尽返回 false，由调用方释放载荷并复位
    // 忙碌标志，杜绝"按钮永久锁死"。
    bool PostMessageWithRetry(HWND hNotify, UINT uMsg, WPARAM wp, LPARAM lp)
    {
        for (int nRetry = 0; nRetry < 10; ++nRetry)
        {
            if (!::IsWindow(hNotify)) return false;
            if (::PostMessageW(hNotify, uMsg, wp, lp)) return true;
            ::Sleep(100);
        }
        return false;
    }
}
// ============================================================
// 状态访问器
// ============================================================
bool CleanJobService::IsScanning() { return s_bScanning; }
bool CleanJobService::IsDeleting() { return s_bDeleting; }
bool CleanJobService::HasThread() { return s_hWorkerThread != nullptr; }
bool CleanJobService::IsBusy()
{
    return s_bScanning || s_bDeleting || s_hWorkerThread != nullptr;
}
void CleanJobService::BeginScan()
{
    s_bScanning = true;
    s_bScanCancel = 0;
}
void CleanJobService::EndScan()
{
    s_bScanning = false;
}
void CleanJobService::BeginClean()
{
    s_bDeleting = true;
}
void CleanJobService::EndClean()
{
    s_bDeleting = false;
}
void CleanJobService::DetachThread()
{
    if (s_hWorkerThread)
    {
        ::CloseHandle(s_hWorkerThread);
   // [C++17 阶段2] 类级工作线程句柄：跨函数分离，显式关闭
        s_hWorkerThread = nullptr;
    }
}
void CleanJobService::Join(DWORD dwTimeoutMs)
{
    if (s_hWorkerThread)
    {
        // [EXIT-FIX] 限时等待：WM_CLOSE 已杀光子进程，正常情况下线程
        // 会立刻从等待返回；超时也不再拖住整个进程退出。
        ::WaitForSingleObject(s_hWorkerThread, dwTimeoutMs);
        ::CloseHandle(s_hWorkerThread);
   // [C++17 阶段2] 类级工作线程句柄：跨函数分离，显式关闭
        s_hWorkerThread = nullptr;
    }
}
void CleanJobService::RequestCancel()
{
    ::InterlockedExchange(&s_bScanCancel, 1);
}
bool CleanJobService::CancelRequested()
{
    return s_bScanCancel != 0;
}
// ============================================================
// Custom ProcName 队列
// ============================================================
void CleanJobService::ResetCustomJobs()
{
    for (int k = 0; k < CUSTOM_PROC_COUNT; ++k) s_aCustomProc[k] = false;
    s_vCustomJobs.clear();
}
bool CleanJobService::GetCustomProc(CustomProcIdx i)
{
    return (static_cast<int>(i) >= 0 && static_cast<int>(i) < CUSTOM_PROC_COUNT) ? s_aCustomProc[static_cast<int>(i)] : false;
}
void CleanJobService::SetCustomProc(CustomProcIdx i, bool b)
{
    if (static_cast<int>(i) >= 0 && static_cast<int>(i) < CUSTOM_PROC_COUNT) s_aCustomProc[static_cast<int>(i)] = b;
}
int CleanJobService::CountCustomProc()
{
    int nQueued = 0;
    for (int k = 0; k < CUSTOM_PROC_COUNT; ++k) if (s_aCustomProc[k]) ++nQueued;
    return nQueued;
}
void CleanJobService::AddCustomJob(CustomProcIdx nCpi, int iRule)
{
    s_vCustomJobs.push_back(std::make_pair(nCpi, iRule));
}
void CleanJobService::ClearCustomJobs()
{
    s_vCustomJobs.clear();
}
// [E-2/E-3] ProcName → CustomProcIdx（与 StartClean 的分派表保持一致）
CustomProcIdx CleanJobService::ProcNameToCpi(const wchar_t* pszProc)
{
    if (!pszProc || !*pszProc) return CustomProcIdx::CPI_NONE;
    if (lstrcmpiW(pszProc, L"DismComponentCleanup")        == 0) return CustomProcIdx::CPI_DISMCOMP;
    if (lstrcmpiW(pszProc, L"DismAppxsCleanup")            == 0) return CustomProcIdx::CPI_DISMAPPXS;
    if (lstrcmpiW(pszProc, L"DismHardLinkMerge")           == 0) return CustomProcIdx::CPI_DISMHARD;
    if (lstrcmpiW(pszProc, L"DismDriverCleanup")           == 0) return CustomProcIdx::CPI_DISMDRV;
    if (lstrcmpiW(pszProc, L"DismCompactOs")               == 0) return CustomProcIdx::CPI_DISMCOMPACT;
    if (lstrcmpiW(pszProc, L"NCInstallerFolderCleanup")    == 0) return CustomProcIdx::CPI_NCINSTALLER;
    if (lstrcmpiW(pszProc, L"NCCorruptedAppXOnlineCleanup")== 0) return CustomProcIdx::CPI_NCCORRUPTAPPX;
    if (lstrcmpiW(pszProc, L"NCSRPointOnlineCleanup")      == 0) return CustomProcIdx::CPI_NCSR;
    if (lstrcmpiW(pszProc, L"NCDOCacheOnlineCleanup")      == 0) return CustomProcIdx::CPI_NCDO;
    if (lstrcmpiW(pszProc, L"NCPackageCacheFolderCleanup") == 0) return CustomProcIdx::CPI_NCPKG;
    if (lstrcmpiW(pszProc, L"NCVSPackageCacheCleanup")     == 0) return CustomProcIdx::CPI_NCVSPKG;
    if (lstrcmpiW(pszProc, L"NCWinEventLogOnlineCleanup")  == 0) return CustomProcIdx::CPI_NCLOG;
    if (lstrcmpiW(pszProc, L"NCSpoolerSafeCleanup")        == 0) return CustomProcIdx::CPI_NCSPOOLER;
    return CustomProcIdx::CPI_NONE;
}
// [E-2] CustomProcIdx → ProcName（派发前实时重评 Applicable 用）
const wchar_t* CleanJobService::CpiToProcName(CustomProcIdx nIdx)
{
    switch (nIdx)
    {
    case CustomProcIdx::CPI_DISMCOMP:     return L"DismComponentCleanup";
    case CustomProcIdx::CPI_DISMAPPXS:    return L"DismAppxsCleanup";
    case CustomProcIdx::CPI_DISMHARD:     return L"DismHardLinkMerge";
    case CustomProcIdx::CPI_DISMDRV:      return L"DismDriverCleanup";
    case CustomProcIdx::CPI_DISMCOMPACT:  return L"DismCompactOs";
    case CustomProcIdx::CPI_NCINSTALLER:  return L"NCInstallerFolderCleanup";
    case CustomProcIdx::CPI_NCCORRUPTAPPX:return L"NCCorruptedAppXOnlineCleanup";
    case CustomProcIdx::CPI_NCSR:         return L"NCSRPointOnlineCleanup";
    case CustomProcIdx::CPI_NCDO:         return L"NCDOCacheOnlineCleanup";
    case CustomProcIdx::CPI_NCPKG:        return L"NCPackageCacheFolderCleanup";
    case CustomProcIdx::CPI_NCVSPKG:      return L"NCVSPackageCacheCleanup";
    case CustomProcIdx::CPI_NCLOG:        return L"NCWinEventLogOnlineCleanup";
    case CustomProcIdx::CPI_NCSPOOLER:    return L"NCSpoolerSafeCleanup";
    }
    return nullptr;
}
// [E-2] 某 Custom 动作的任一来源规则当前是否仍适用（无来源记录时保守放行）
bool CleanJobService::CustomActionStillApplicable(CCleanEngine& eng, CustomProcIdx nCpi)
{
    const wchar_t* pszProc = CpiToProcName(nCpi);
    bool bFoundJob = false;
    for (size_t jj = 0; jj < s_vCustomJobs.size(); ++jj)
    {
        if (s_vCustomJobs[jj].first != nCpi) continue;
        bFoundJob = true;
        if (eng.IsRuleApplicableNow(s_vCustomJobs[jj].second, pszProc))
            return true;
    }
    return !bFoundJob;   // 防御：无映射信息时保持旧行为
}
// ============================================================
// 扫描线程（原 StartScan 内 lambda，零逻辑改动）
// ============================================================
namespace
{
    struct ScanCtx { HWND hNotify; CCleanEngine* pEng; int nTotalWork; };
    // ------------------------------------------------------------
    // [R-ScanResilience] 单规则 SEH 隔离 + 逐规则计时落盘诊断
    //
    // 事故复盘（crashguard 0xC0000005 worker-thread-seh）：
    //   工程以 /EHsc 编译，catch(...) 捕不到 SEH 硬件异常；ScanRule
    //   内任何一条规则触发 AV 都会穿透 ScanThreadProc 的 try/catch，
    //   被 SafeThreadTrampoline 外层 __except 整体吞掉 —— 扫描线程
    //   直接死亡，WM_SCAN_DONE 永不投递，UI 永久停在“扫描中”，
    //   用户看到的就是“扫描没有任何结果”。
    //
    // 对策：
    //   1) 每条规则在独立 __try 中执行（本函数无 C++ 堆对象，允许
    //      直接 SEH），单规则故障降级为规则级失败（-ScanRuleError::SCAN_RULE_UNKNOWN），
    //      扫描继续推进，DONE 必定到达。
    //   2) 每条规则调用前落盘 "start" 行（含序号/名称/时刻），
    //      调用后落盘 "done" 行（含返回码/大小/耗时）。线程若再
    //      出异常，crashguard 的 scan-rule-seh 与本日志中最后一条
    //      start 行即可唯一锁定肇事规则；若卡死在某条规则，最后
    //      一条 start 行同样指出挂起点。
    // ------------------------------------------------------------
    struct OneRuleCtx { CCleanEngine* pEng; int iIdx; int nRet; };
    DWORD WINAPI OneRuleProc(void* pv)
    {
        OneRuleCtx* c = (OneRuleCtx*)pv;
        c->nRet = c->pEng->ScanRule(c->iIdx);
        return 0;
    }
    // 无 C++ 堆对象函数：__try/__except 合规
    LONG ScanRuleSehFilter(EXCEPTION_POINTERS* pEp)
    {
        CrashGuardWrite("scan-rule-seh",
            (pEp && pEp->ExceptionRecord) ? pEp->ExceptionRecord->ExceptionCode : 0,
            (pEp && pEp->ExceptionRecord) ? pEp->ExceptionRecord->ExceptionAddress : nullptr);
        return EXCEPTION_EXECUTE_HANDLER;
    }
    // [C++17 R3] [[nodiscard]]：返回值勿忽略
    [[nodiscard]] int ScanOneRuleIsolated(CCleanEngine* pEng, int iIdx)
    {
        OneRuleCtx ctx;
        ctx.pEng = pEng; ctx.iIdx = iIdx; ctx.nRet = 0;
        __try
        {
            OneRuleProc(&ctx);
        }
        __except (ScanRuleSehFilter(GetExceptionInformation()))
        {
            ctx.nRet = -static_cast<int>(ScanRuleError::SCAN_RULE_UNKNOWN);   // 单规则异常 → 规则级失败
        }
        return ctx.nRet;
    }
    // 扫描日志追加一行（UTF-16，与原版诊断习惯一致；用户可直接 cat）
    void LogScanLine(const wchar_t* pszFmt, ...)
    {
        // [L-06][M-6] clean_scan.log 并入统一日志目录（EXE\Logs，不可写时自动回退）。
        //   原实现写死在 %LOCALAPPDATA%\DismTool_Clone\logs，与审计流分处两地，
        //   用户排查清理问题时要在两个目录之间来回找。
        const std::wstring sScanLog = LogService::UnifiedLogPath(L"clean_scan.log");
        if (sScanLog.empty()) return;
        const wchar_t* szLog = sScanLog.c_str();
        CJSFileGuard h(::CreateFileW(szLog, FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr));
        if (!h.valid()) return;
        std::wstring sLogLine;
        sLogLine.resize(512);
        va_list ap;
        va_start(ap, pszFmt);
        _vsnwprintf_s(&sLogLine[0], sLogLine.size(), _TRUNCATE, pszFmt, ap);
        va_end(ap);
        sLogLine.resize(wcslen(sLogLine.c_str()));
        ::WriteFile(h, sLogLine.c_str(), static_cast<DWORD>(sLogLine.size() * sizeof(wchar_t)), nullptr, nullptr);
        // CJSFileGuard 析构自动关闭
    }
    DWORD WINAPI ScanThreadProc(void* pv)
    {
        std::unique_ptr<ScanCtx> guard(static_cast<ScanCtx*>(pv));   // [C++17 R3] thread owns ctx, auto-freed at end
        HWND hNotify = guard->hNotify;
        CCleanEngine* pEng = guard->pEng;
        const int nTotalWork = guard->nTotalWork;


        int n = pEng->GetRuleCount();
        // [清理扫描诊断] 落盘规则总数到日志；如果 n=0 用户点扫描
        // 会完全不执行循环，现象就是"扫不到任何东西"。
        LogScanLine(L"[clean-scan] launch rule_count=%d total_work=%d\n", n, nTotalWork);
        int nDoneWork = 0;
        int nFailCount = 0;
        // [AUDIT-F6] 适用规则序数：跳过 !bOsApplicable 的规则不计数，
        // 与 UI 侧 "(k/n)" 的适用分母严格对应。
        int nApplicableDone = 0;
        const DWORD tScanBegin = ::GetTickCount();
        // 异常边界：单条规则抛异常不得吞掉 WM_SCAN_DONE，否则 UI 永久转圈
        try
        {
            for (int i = 0; i < n; ++i)
            {
                // [OS-Filter] 跳过不属于当前系统的规则：这类条目在 UI 上
                // 已被隐藏（Page_Cleanup_Build 建行时按 bOsApplicable 过滤），
                // 扫描它们既浪费 CBS/磁盘 IO，也会因找不到目标而刷失败日志，
                // 更严重的是会让进度分母与实际扫描量不符。豁免与 UI 同源，
                // 保证"看不到的就不会被扫"。
                if (!pEng->GetRule(i).bOsApplicable) continue;
                ++nApplicableDone;   // [AUDIT-F6] 适用规则已扫描序数
                // [R17] 取消令牌：规则粒度退出，已扫结果保留可见
                if (::InterlockedCompareExchange(&s_bScanCancel, 0, 0) != 0)
                    break;
                // [R-ScanResilience] 单规则 SEH 隔离：/EHsc 下 AV 会
                // 穿透 catch(...) 杀死整个扫描线程，导致 DONE 丢失、
                // UI 永久“扫描中”。此处单规则故障降级为规则级失败。
                const DWORD tRule = ::GetTickCount();
                int nRet = ScanOneRuleIsolated(pEng, i);
                const DWORD msRule = ::GetTickCount() - tRule;
                // 逐规则日志仅在失败时落盘（成功规则不刷日志，避免无限增长）
                if (nRet < 0)
                {
                    LogScanLine(L"[clean-scan] rule %d/%d FAIL ret=%d size=%I64u err=%d ms=%lu name='%s'\n",
                                i + 1, n, nRet,
                                pEng->GetRule(i).ullSize,
                                pEng->GetRule(i).nLastError,
                                (unsigned long)msRule,
                                pEng->GetRule(i).sName.GetData());
                }
                nDoneWork += pEng->GetRule(i).nWeight;
                auto msg = std::make_unique<ScanItemMsg>();   // [C++17 R3] transferred to UI on successful Post; auto-reclaimed on failure
                ScanItemMsg* m = msg.get();
                m->iIdx = i;
                m->ullSize = pEng->GetRule(i).ullSize;
                m->nDoneWork = nDoneWork;
                m->nTotalWork = nTotalWork;
                m->bRuleFailed = (nRet < 0);
                m->nErrorCode  = m->bRuleFailed ? CCleanEngine::ScanRuleErrorFromRet(nRet)
                                                : static_cast<int>(ScanRuleError::SCAN_RULE_OK);
                m->nRuleOrdinal = nApplicableDone;   // [AUDIT-F6] 适用序数随消息带给 UI
                if (m->bRuleFailed)
                {
                    ++nFailCount;
                }
                // 窗口可能已在扫描期间销毁：Post 失败必须自行释放，避免泄漏
                if (::PostMessageW(hNotify, WM_SCAN_ITEM, 0, reinterpret_cast<LPARAM>(msg.get())))
                    msg.release();   // [C++17 R3] ownership transferred to UI
            }
        }
        catch (...)
        {
            ++nFailCount;   // 异常中断按失败计入，保证 DONE 仍到达
        }
        LogScanLine(L"[clean-scan] done rules=%d failed=%d ms=%lu\n",
                    n, nFailCount,
                    (unsigned long)(::GetTickCount() - tScanBegin));
        // C-10: 通过 WM_SCAN_DONE 的 lParam 传递失败计数，
        // UI 端在结束处理时把它显示到状态条 / 日志。
        // [FIX-P1B] 有限重试：Post 失败（队列瞬时满）不再直接丢失 DONE，
        // 避免 s_bScanning 恒 true、按钮永久锁死（lParam 为值，无所有权问题）。
        if (::IsWindow(hNotify))
            PostMessageWithRetry(hNotify, WM_SCAN_DONE, 0, static_cast<LPARAM>(nFailCount));
        else
            s_bScanning = false;   // 窗口已亡：就地复位，服务态不残留
        return 0;
    }
}
bool CleanJobService::LaunchScan(HWND hNotify, CCleanEngine* pEng, int nTotalWork)
{
    // [FIX-P1B] 先自愈陈旧线程：上次 DONE 丢失时回收句柄/复位标志，
    // 避免下方 s_hWorkerThread 覆盖造成句柄泄漏（P1-12）。
    SelfHealStaleWorkerThread();
    auto ctxOwner = std::make_unique<ScanCtx>();   // [C++17 R3] transferred to worker thread on success; reclaimed here on creation failure
    ctxOwner->hNotify = hNotify;
    ctxOwner->pEng = pEng;
    ctxOwner->nTotalWork = nTotalWork;


    HANDLE h = SafeCreateThread(ScanThreadProc, ctxOwner.get());
    if (!h)
    {
        // CreateThread 失败：复位状态，否则按钮与标志永久锁死
        // ctxOwner reclaims ScanCtx at scope exit (was: delete ctx)
        s_bScanning = false;
        return false;
    }
    ctxOwner.release();   // [C++17 R3] ownership transferred to worker thread
    s_hWorkerThread = h;   // 句柄在 WM_SCAN_DONE 中关闭
    return true;
}
// ============================================================
// 删除线程（原 StartClean 内 lambda，零逻辑改动）
// ============================================================
namespace
{
    struct DelCtx
    {
        HWND hNotify;
        CCleanEngine* pEng;
        std::unique_ptr<std::vector<int>> sel;   // [C++17 R3] owned by ctx, freed with ctx
        bool bDismComponentPending;
        bool bDismResetBase;
        bool bCleanAssocNotify;
        bool bCleanRestartExplorer;
        bool bToRecycleBin;   // [R88] 主规则清理是否走回收站
        int  nFileWork;       // [R-CLEANPROG] 重评阶段进度分母（权重和）
    };


    DWORD WINAPI CleanThreadProc(void* pv)
    {
        std::unique_ptr<DelCtx> guard(static_cast<DelCtx*>(pv));   // [C++17 R3] thread owns ctx, auto-freed at end
        HWND hNotify = guard->hNotify;
        CCleanEngine* pEng = guard->pEng;

        // [R-CLEANPROG] 进度回投：窗口已销毁则静默放弃（堆对象自释放）。
        auto postProgress = [hNotify](int nDone, int nTotal, int nPhase)
        {
            if (nTotal <= 0 || !::IsWindow(hNotify)) return;
            auto m = std::make_unique<CleanProgressMsg>();
            m->nDone = nDone; m->nTotal = nTotal; m->nPhase = nPhase;
            if (::PostMessageW(hNotify, WM_CLEAN_PROGRESS, 0,
                               reinterpret_cast<LPARAM>(m.get())))
                m.release();
        };


        // [E-2] 删除前按当前实时状态重评每条勾选规则：重新跑 Applicable
        // 条件 + 重新枚举/计量目标。扫描结束后若发生状态漂移（软件卸载、
        // 盘符移除、服务状态变化等），已不适用或目标消失的规则本轮跳过，
        // 绝不沿用扫描期缓存的 targets 盲删。
        // 异常边界：任何阶段抛异常都必须回 WM_CLEAN_DONE，否则 UI 永久停在
        // “清理中”；上下文与消息对象由统一出口释放，杜绝泄漏。
        std::unique_ptr<CleanDoneMsg> msg;
        try
        {
            std::vector<int> vLive;
            // [R-CLEANPROG] 重评阶段进度（该阶段与扫描同量级耗时，此前无任何反馈）。
            // 分母用 UI 传来的权重和（与扫描页进度条同口径），<=0 退化为按条数。
            const int nRevalTotal = guard->nFileWork > 0
                ? guard->nFileWork : static_cast<int>(guard->sel->size());
            postProgress(0, nRevalTotal, 0);
            int nRevalDone = 0;
            for (size_t ri = 0; ri < guard->sel->size(); ++ri)
            {
                int iRule = (*(guard->sel))[ri];
                pEng->ScanRule(iRule);
                if (!pEng->GetRule(iRule).targets.empty())
                {
                    vLive.push_back(iRule);
                }
                else if (pEng->GetRule(iRule).sSpecialProc.IsEmpty())
                {
                    // [FIX-WINSXS] Custom 规则（如 DismComponentCleanup）不走
                    // 文件删除通道：targets 为空是预期形态，清理经 Custom 派发
                    // 独立承担 —— 不刷"skipped"误导日志。
                    auto pl = std::make_unique<CDuiString>();   // [C++17 R3] transferred to UI on successful Post; auto-reclaimed on failure
                    pl->Format(L"[clean] rule '%s' skipped at clean time: no longer applicable or no targets",
                               pEng->GetRule(iRule).sName.GetData());
                    if (::PostMessageW(hNotify, WM_DISM_LOG, 1, reinterpret_cast<LPARAM>(pl.get())))
                        pl.release();   // [C++17 R3] ownership transferred to UI
                }
                nRevalDone += (guard->nFileWork > 0)
                    ? (pEng->GetRule(iRule).nWeight > 0 ? pEng->GetRule(iRule).nWeight : 1)
                    : 1;
                postProgress(nRevalDone, nRevalTotal, 0);
            }


            int nFailed = 0;
            // [R88] 主规则清理走回收站（可撤销），并把实际尝试删除的路径回传给 UI
            std::vector<CDuiString> vDeletedPaths;
            // [R-CLEANPROG] 删除阶段：批次完成回调 → 回投 UI（size_t 转 int 防溢出钳制）
            std::function<void(size_t, size_t)> progCb =
                [&postProgress](size_t done, size_t total)
                {
                    const int nDone  = done   > INT_MAX ? INT_MAX : static_cast<int>(done);
                    const int nTotal = total  > INT_MAX ? INT_MAX : static_cast<int>(total);
                    postProgress(nDone, nTotal, 1);
                };
            ULONGLONG ullFreed = pEng->DeleteRules(vLive, nFailed, guard->bToRecycleBin,
                                                  &vDeletedPaths, &progCb);
            msg = std::make_unique<CleanDoneMsg>();   // [C++17 R3] transferred to UI on successful Post; auto-reclaimed on failure
            msg->nRules = static_cast<int>(vLive.size());
            msg->nFailed = nFailed;
            msg->ullFreed = ullFreed;
            msg->bToRecycleBin = guard->bToRecycleBin;
            msg->vDeletedPaths.swap(vDeletedPaths);
            // P6b：UI 收集阶段快照随消息带回（原壳成员）
            msg->bDismComponentPending = guard->bDismComponentPending;
            msg->bDismResetBase = guard->bDismResetBase;
            msg->bCleanAssocNotify = guard->bCleanAssocNotify;
            msg->bCleanRestartExplorer = guard->bCleanRestartExplorer;
        }
        catch (...)
        {
            // 清理流程异常中断：带回安全默认值，仍触发完成收尾与后续 DISM 步骤
            if (!msg)
            {
                msg = std::make_unique<CleanDoneMsg>();   // [C++17 R3] transferred to UI on successful Post; auto-reclaimed on failure
                msg->nRules = 0;
                msg->nFailed = 1;
                msg->ullFreed = 0;
                msg->bToRecycleBin = guard->bToRecycleBin;
                msg->bDismComponentPending = guard->bDismComponentPending;
                msg->bDismResetBase = guard->bDismResetBase;
                msg->bCleanAssocNotify = guard->bCleanAssocNotify;
                msg->bCleanRestartExplorer = guard->bCleanRestartExplorer;
            }
            auto pl = std::make_unique<CDuiString>(   // [C++17 R3] transferred to UI on successful Post; auto-reclaimed on failure
                L"[clean] 清理过程中发生异常，已安全中止");
            if (::PostMessageW(hNotify, WM_DISM_LOG, 1, reinterpret_cast<LPARAM>(pl.get())))
                pl.release();   // [C++17 R3] ownership transferred to UI
        }
        // guard reclaims DelCtx and sel at scope exit (was: delete c->sel; delete c;)
        // [FIX-P1B] 完成消息有限重试：Post 失败不再直接丢 WM_CLEAN_DONE；
        // 重试仍失败（窗口亡/队列持续满）时就地复位 s_bDeleting 并释放载荷，
        // 服务态不残留，避免"清理按钮永久锁死"。
        if (::IsWindow(hNotify))
        {
            if (PostMessageWithRetry(hNotify, WM_CLEAN_DONE, 0,
                                     reinterpret_cast<LPARAM>(msg.get())))
                msg.release();   // [C++17 R3] ownership transferred to UI; auto-reclaimed when window destroyed / Post failed
            else
                s_bDeleting = false;   // 重试耗尽：自愈复位，载荷由 unique_ptr 释放
        }
        else
        {
            s_bDeleting = false;   // 窗口已亡：就地复位，服务态不残留
        }
        return 0;
    }
}
bool CleanJobService::LaunchClean(HWND hNotify, CCleanEngine* pEng,
                                  const std::vector<int>& vSel,
                                  bool bDismComponentPending, bool bDismResetBase,
                                  bool bCleanAssocNotify, bool bCleanRestartExplorer,
                                  bool bToRecycleBin, int nFileWork)
{
    // [FIX-P1B] 先自愈陈旧线程（同 LaunchScan，P1-12）
    SelfHealStaleWorkerThread();
    auto ctxOwner = std::make_unique<DelCtx>();   // [C++17 R3] transferred to worker thread on success; reclaimed here on creation failure
    ctxOwner->hNotify = hNotify;
    ctxOwner->pEng = pEng;
    ctxOwner->sel = std::make_unique<std::vector<int>>(vSel);   // [C++17 R3] owned by ctx
    ctxOwner->bDismComponentPending = bDismComponentPending;
    ctxOwner->bDismResetBase = bDismResetBase;
    ctxOwner->bCleanAssocNotify = bCleanAssocNotify;
    ctxOwner->bCleanRestartExplorer = bCleanRestartExplorer;
    ctxOwner->bToRecycleBin = bToRecycleBin;
    ctxOwner->nFileWork = nFileWork;


    HANDLE h = SafeCreateThread(CleanThreadProc, ctxOwner.get());
    if (!h)
    {
        // ctxOwner reclaims DelCtx and sel at scope exit (was: delete ctx->sel; delete ctx;)
        s_bDeleting = false;
        return false;
    }
    ctxOwner.release();   // [C++17 R3] ownership transferred to worker thread
    s_hWorkerThread = h;   // 句柄在 WM_CLEAN_DONE 中关闭
    return true;
}
