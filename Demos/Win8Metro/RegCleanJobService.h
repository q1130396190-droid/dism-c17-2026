#pragma once

// ============================================================
// RegCleanJobService —— [R111] 注册表清理页工作线程管线
//
// 目的：把 OnClean 中"备份 → CleanRules → 重建"整段同步执行改为
// 后台线程，UI 不冻结，逐规则进度回传，D4 异步错误处理补齐。
//
// 铁律（与 CleanJobService 一致）：
//   * 服务线程只允许 PostMessage，不做任何 UI / StringMgr 文案；
//   * 启动前把跨线程数据拷入 Ctx 快照，线程绝不回读 UI 状态；
//   * 本头不 include UIlib.h，仅用 CDuiString 轻量声明；CRegCleanEngine
//     前置声明，.cpp 内再 include RegCleanEngine.h。
// ============================================================

#include <windows.h>

#include <vector>

#ifndef UILIB_API
#define UILIB_API __declspec(dllimport)
#endif

#include "Utils/Utils.h"
#include "RegCleanEngine.h"   // RegCleanBackup / RegCleanExecResult

using DuiLib::CDuiString;

// WM_REGCLEN_PROGRESS 载荷：lParam = new int(已处理规则数)，wParam = 未用
// WM_REGCLEN_DONE 载荷：RegCleanDoneMsg*（UI 线程 delete）
struct RegCleanDoneMsg
{
    int                          nSelected;   // 本次清理的规则数
    int                          nSucceeded;  // 成功动作总数
    int                          nFailed;     // 失败动作总数
    CDuiString                   sBackupFile; // 备份文件路径（可能为空 = 未备份）
    std::vector<RegCleanExecResult> vResults; // 逐规则结果快照
};

// [R-EXTASYNC] WM_REGCLEN_SCANDONE 载荷（UI 线程 delete）。
// 结果向量由 UI 页持有（启动前清空，仅 worker 写入，DONE 后 UI 读）。
struct RegCleanScanDoneMsg
{
    bool bOk;             // false=扫描异常终止
    int  nRules;          // 参与扫描规则数
    int  nApplicable;     // 适用规则数
    int  nCleanable;      // 可清理规则数
    int  nCleanActions;   // 可清理动作总数
};

class RegCleanJobService
{
public:
    // ---- 忙碌状态 ----
    static bool IsRunning();
    static bool HasThread();

    // ---- 扫描（[R-EXTASYNC] 后台化，避免全量注册表枚举冻结 UI）----
    // pvResults/pSummary 为 UI 页持有存储：调用前须已 clear，
    // worker 运行期间 UI 不得读写；结束 Post WM_REGCLEN_SCANDONE。
    static bool LaunchScan(HWND hNotify, CRegCleanEngine* pEng,
                           std::vector<RegCleanScanResult>* pvResults,
                           RegCleanScanSummary* pSummary);
    static void EndScan();

    // ---- 清理 ----
    // 置 s_bRunning=true，拷贝 vSel/backup 快照，起 worker 线程；
    // CreateThread 失败时内部复位 s_bRunning 并返回 false。
    // worker 内部逐规则调用 CleanRules（引擎 API 不变），
    // 逐规则 PostMessage WM_REGCLEN_PROGRESS，结束 PostMessage
    // WM_REGCLEN_DONE（RegCleanDoneMsg*）。
    static bool LaunchClean(HWND hNotify, CRegCleanEngine* pEng,
                            const std::vector<int>& vSel,
                            const RegCleanBackup& backup);

    // WM_REGCLEN_DONE 收尾：s_bRunning=false，DetachThread 由 UI 调
    static void EndClean();

    // 句柄回收（UI 收到 DONE 后调用）
    static void DetachThread();

    // 析构/关窗限时等待（默认 2 秒）；超时放弃，避免退出路径僵尸化
    static void Join(DWORD dwTimeoutMs = 2000);
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
