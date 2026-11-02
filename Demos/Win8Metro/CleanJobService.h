#pragma once
// ============================================================
// CleanJobService —— P6b「系统清理」页工作线程管线（方案 S10）
//
// 纯机械下沉自原 CFrameWnd：
//   * 扫描线程（原 StartScan 内 lambda）/ 删除线程（原 StartClean 内 lambda）
//   * 取消令牌 volatile LONG s_bScanCancel（Interlocked 操作原样）
//   * 唯一工作线程句柄 s_hWorkerThread
//   * Custom ProcName 队列：s_aCustomProc[12] / s_vCustomJobs
//   * CustomProcIdx 枚举（值序不变）、ProcNameToCpi / CpiToProcName /
//     CustomActionStillApplicable
//   * 线程消息结构 ScanItemMsg / CleanDoneMsg（CleanDoneMsg 扩 4 个
//     通知快照字段，WM_SCAN_ITEM/WM_SCAN_DONE/WM_CLEAN_DONE 编号与
//     wParam/lParam 时序一字不改）
//
// 铁律：
//   * 服务线程只允许 PostMessage，不做任何 UI / StringMgr 文案（错误走
//     计数与消息结构）；
//   * UI 侧在启动前把所有跨线程数据拷入 Ctx 快照，线程绝不回读 UI 状态；
//   * 本头不 include UIlib.h，仅用 CDuiString 轻量声明；CCleanEngine
//     前置声明，.cpp 内再 include CleanEngine.h。
// ============================================================
#include <windows.h>
#include <vector>
#include <utility>
#ifndef UILIB_API
#define UILIB_API __declspec(dllimport)
#endif
#include "Utils/Utils.h"
using DuiLib::CDuiString;
class CCleanEngine;
// Custom ProcName dispatch queue 索引（原 CFrameWnd::CustomProcIdx，
// 值序必须与 StartClean 收集分派表保持一致）
// 0 = DismComponentCleanup (WinSxS); 1..4 = DISM family; 5..11 = NCleaner family
// CPI_NONE=-1：ProcNameToCpi 失败哨兵（不可用裸 -1，enum class 已限定作用域）
enum class CustomProcIdx : int { CPI_NONE = -1,
                     CPI_DISMCOMP = 0, CPI_DISMAPPXS, CPI_DISMHARD, CPI_DISMDRV, CPI_DISMCOMPACT,
                     CPI_NCINSTALLER, CPI_NCCORRUPTAPPX, CPI_NCSR, CPI_NCDO, CPI_NCPKG, CPI_NCVSPKG, CPI_NCLOG,
                     CPI_NCSPOOLER };
constexpr int CUSTOM_PROC_COUNT = 13;
// WM_SCAN_ITEM 载荷（原 FrameWnd.cpp 文件内结构，逐字搬迁）
struct ScanItemMsg
{
    int        iIdx;
    ULONGLONG  ullSize;
    // [E-6] 按 Scan@Work 权重的累计进度（已完成权重/总权重）
    int        nDoneWork;
    int        nTotalWork;
    // S-13: 单条规则扫描是否失败（返回值 <0 表示内部规则执行错误）。
    // UI 线程据此累加失败计数，在 WM_SCAN_DONE 阶段统一反馈。
    bool       bRuleFailed;
    // [A3] 失败时的错误类别（SCAN_RULE_*）；成功时为 ScanRuleError::SCAN_RULE_OK。
    // 用于 WM_SCAN_DONE 阶段按类别汇总统计，气泡/日志文案更精准。
    int        nErrorCode;
    // [AUDIT-F6] 本轮已扫描的「本系统适用」规则序数（1 起，跳过 !bOsApplicable）。
    // 状态条 "(k/n)" 的 k 原用 iIdx+1：iIdx 含不适用规则的索引空洞，尾部
    // 有不适用规则时 k 永远到不了 n；改用本序数与适用分母严格对应。
    int        nRuleOrdinal = 0;
};
// WM_CLEAN_DONE 载荷（原结构 + P6b 扩 4 个通知快照字段）
struct CleanDoneMsg
{
    int        nRules;
    int        nFailed;
    ULONGLONG  ullFreed;
    // P6b：原壳成员 m_bDismComponentPending / m_bDismResetBase /
    // m_bCleanAssocNotify / m_bCleanRestartExplorer 改为 UI 侧启动前
    // 算好放入线程 Ctx、线程结束写入本消息，OnCleanDone 从消息读。
    bool       bDismComponentPending;
    bool       bDismResetBase;
    bool       bCleanAssocNotify;
    bool       bCleanRestartExplorer;
    // [R88] 本次主规则清理是否走回收站（FOF_ALLOWUNDO）
    bool       bToRecycleBin;
    // [R88] 本次实际尝试删除的路径（供 UI 推入撤销栈）
    std::vector<CDuiString> vDeletedPaths;
};
// [R-CLEANPROG] WM_CLEAN_PROGRESS 载荷：删除线程两阶段进度回报。
//   nPhase=0：删除前逐规则重评（nTotal=勾选规则数）；
//   nPhase=1：SHFileOperation 批次删除（nTotal=去重后路径总数，nDone=已完成批次的路径数）。
//   nPhase=2：[FIX-TAIL-PROG] DISM 尾巴阶段（12 路自定义动作 + WinSxS 组件
//             清理），sStage 携带当前步骤描述（UI 状态条直接展示）。
// 所有权：投递方 new，UI 线程 OnCleanProgress delete（kMsgOwnPage 契约）。
struct CleanProgressMsg
{
    int nDone;
    int nTotal;
    int nPhase;
    std::wstring sStage;   // [FIX-TAIL-PROG] nPhase=2 时的步骤描述（可空）
};
class CleanJobService
{
public:
    // ---- 忙碌状态（原 m_bScanning / m_bDeleting / m_hWorkerThread 判断）----
    static bool IsScanning();
    static bool IsDeleting();
    static bool HasThread();
    // 原 RefreshAll：m_bScanning || m_bDeleting || m_hWorkerThread
    static bool IsBusy();

    // ---- 扫描（原 StartScan 的状态/线程段；UI 准备段留页）----
    // 置 s_bScanning=true、取消令牌清零（原 StartScan 首段两行）
    static void BeginScan();
    // 建 Ctx 起扫描线程并保存句柄；CreateThread 失败时内部复位
    // s_bScanning 并返回 false（UI 侧自行恢复按钮/状态条）
    static bool LaunchScan(HWND hNotify, CCleanEngine* pEng, int nTotalWork);
    // WM_SCAN_DONE：s_bScanning=false（原 handler 第三行）
    static void EndScan();

    // ---- 清理（原 StartClean 的 m_bDeleting/线程段）----
    // 置 s_bDeleting=true（原确认弹窗后的 m_bDeleting = true）
    static void BeginClean();
    // sel 拷贝入线程 Ctx；4 个通知 bool 为 UI 收集阶段快照，线程结束
    // 写入 CleanDoneMsg。CreateThread 失败内部复位 s_bDeleting 并返回 false
    // [R88] 新增 bToRecycleBin：主规则清理走 FOF_ALLOWUNDO（进回收站可撤销）
    // [R-CLEANPROG] nFileWork：勾选文件规则的权重和（重评阶段进度分母，
    // 与扫描页进度条同一口径；<=0 时线程退化为按规则条数计）
    static bool LaunchClean(HWND hNotify, CCleanEngine* pEng,
                            const std::vector<int>& vSel,
                            bool bDismComponentPending, bool bDismResetBase,
                            bool bCleanAssocNotify, bool bCleanRestartExplorer,
                            bool bToRecycleBin = true,
                            int nFileWork = 0);
    // WM_CLEAN_DONE 全部同步派发结束后：s_bDeleting=false
    static void EndClean();

    // ---- 句柄 / 取消 / 关窗 ----
    // 原 CloseWorkerThread：仅 CloseHandle（DONE handler 首行调用，
    // 不 wait；Join 仅析构用）
    static void DetachThread();
    // 析构等待：限时等待工作线程退出（默认 2 秒）。
    // 超时不再永久阻塞退出（历史缺陷：线程卡在子进程等待上会导致
    // ExitProcess 僵尸化）；超时后释放句柄放弃等待，残留线程随进程
    // 终结（Kill-On-Close 作业/TerminateProcess 兜底）一并清除。
    static void Join(DWORD dwTimeoutMs = 2000);
    // 原 InterlockedExchange(&m_bScanCancel, 1)
    static void RequestCancel();
    // 原 (m_bScanCancel != 0)
    static bool CancelRequested();

    // ---- Custom ProcName 队列（原 m_aCustomProc[12] / m_vCustomJobs）----
    // 生产在 UI（页 StartClean 收集段），消费在 UI（页 OnCleanDone）
    static void ResetCustomJobs();   // 原 for(12)=false + jobs.clear()
    static bool GetCustomProc(CustomProcIdx i);
    static void SetCustomProc(CustomProcIdx i, bool b);
    static int  CountCustomProc();   // 原 nQueued 统计
    static void AddCustomJob(CustomProcIdx nCpi, int iRule);
    static void ClearCustomJobs();   // 本轮派发结束映射表复位
    // 派发前按实时 Applicable 重评（无来源记录时保守放行）
    static bool CustomActionStillApplicable(CCleanEngine& eng, CustomProcIdx nCpi);
    static const wchar_t* CpiToProcName(CustomProcIdx nIdx);
    static CustomProcIdx ProcNameToCpi(const wchar_t* pszProc);
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
