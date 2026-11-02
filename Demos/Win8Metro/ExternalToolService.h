#pragma once

// ============================================================
// ExternalToolService —— S9（P6a）
// 工具箱外部进程能力：DISM/通用控制台工具异步管道会话、GUI 进程
// 启动、ShellExecuteEx 提权启动、受守护 bat 执行器（失败兜底 +
// UAC + 日志归档）、ADK/引导扇区探测与工具可用性探测。
//
// 纯机械物理搬迁（原 CFrameWnd 文件 static / 成员函数），零行为改动：
//   * 实现文件不使用预编译头、不 include UIlib.h / StringManager / stdafx；
//   * 工作线程仅 PostMessage（WM_DISM_LOG/WM_DISM_PROGRESS/WM_DISM_DONE），
//     不触碰任何 UI；本地化文案一律留在 UI 层（FrameWnd / CPageTools）；
//   * 会话句柄为文件 static，全局同时只允许一路控制台捕获。
// ============================================================

#include <windows.h>

#ifndef UILIB_API
#define UILIB_API __declspec(dllimport)
#endif

#include "Utils/Utils.h"

// S9 实现文件不使用预编译头，需在引入 UIlib 全命名空间前显式可见该类型
using DuiLib::CDuiString;

class ExternalToolService
{
public:
    // ---- DISM / 通用控制台工具异步会话 ----
    // 当前是否已有一路控制台工具在运行（原 StartTool 开头的 m_hDismProcess 互斥判断）。
    static bool IsRunning();

    // 承接原 CFrameWnd::StartTool 的管道/进程/读取线程主体（UI 日志与
    // “starting...” 状态仍由 UI 调用侧负责）。
    // 返回 true=进程与读取线程已启动；false=启动失败，此时：
    //   *pnFail = 1 stdout 管道创建失败；2 stderr 管道创建失败；3 CreateProcess 失败
    //   *pdwLastError 仅在失败 3 时有效（GetLastError 原值）。
    // 读取线程只向 hNotify PostMessage WM_DISM_LOG / WM_DISM_PROGRESS /
    // WM_DISM_DONE（WM_DISM_LOG 的 lParam 为 new CDuiString，接收方 delete）。
    static bool StartToolAsync(HWND hNotify, const wchar_t* pszExeName,
                               const CDuiString& sArgs,
                               int* pnFail = nullptr, DWORD* pdwLastError = nullptr);

    // [R14-04] 原 StartToolAsync 内嵌读取线程主体抽出：轮询子进程退出码、
    // 读取 stdout/stderr 管道、PostMessage WM_DISM_LOG/PROGRESS/DONE 到 hNotify。
    // pv = ToolReadCtx*（匿名命名空间内定义，本 TU 内可见）。
    static DWORD WINAPI ToolReadThreadProc(void* pv);

    // 原 AbortDism 的句柄段：终止进程并关闭三句柄。
    static void Abort();

    // 原 CFrameWnd::OnDismDone 前半段：进程正常结束后关闭三句柄。
    // WM_DISM_DONE 的 UI 收尾在 CPageTools::OnDismDone。
    static void OnDone();

    // ---- 外部 GUI 进程（原 LaunchGUI；%System32%\<exe>，不等待）----
    static void LaunchGui(const wchar_t* pszExeName, const wchar_t* pszArgs);

    // [R129-1] 带错误回传与真实等待的 GUI 启动（D3/D4 证据路径）：
    //   * 返回 false 表示 CreateProcessW 直接失败，此时 *pdwErr 收 GetLastError 原值；
    //   * 启动成功后由调用方决定是否等待：bWait=true 时使用
    //     WaitForSingleObject + GetExitCodeProcess 真实读取退出码，超时 dWaitMs
    //     后返回 false（调用方据此提示"仍在运行"）；bWait=false 与旧 LaunchGui 等价。
    //   * 工具执行统一走 CreateProcessW 真实路径，不再走 ShellExecuteW 拉起后无跟踪。
    static bool LaunchGuiEx(const wchar_t* pszExeName, const wchar_t* pszArgs,
                            bool bWait, DWORD dWaitMs,
                            DWORD* pdwErr = nullptr, DWORD* pdwExitCode = nullptr);

    // 统一启动系统工具（原文件 static LaunchSystemEx，去 StringMgr 化）：
    //   bElevated=true → runas（UAC）；false → open。
    // 返回 true=已成功启动；false=失败（*pdwErr 收 GetLastError 原值，
    // ec==5 即用户在 UAC 点了“否”，本地化判定在 UI 调用侧）。
    // phProcess 非空时接管进程句柄；否则服务内部关闭。
    static bool LaunchSystem(HWND hWnd, const wchar_t* pszProgram, const wchar_t* pszArgs,
                             const wchar_t* pszWorkDir, bool bElevated,
                             DWORD* pdwErr = nullptr, HANDLE* phProcess = nullptr);

    // 当前进程是否持有完整提权令牌（原文件 static IsElevated）。
    static bool IsElevated();

    // 在资源管理器中打开目录（原文件 static OpenExplorerAtDir）：
    // 目录不存在先创建；explorer 启动失败回退为直接打开目录。
    static void ExploreFolder(HWND hWnd, const CDuiString& sDir);

    // ---- 受守护 bat 执行器（原 LaunchBatGuarded）----
    // 写入 GBK/ANSI bat（可选追加失败兜底 + rc 回写），经 UAC 启动 cmd /k，
    // 归档 %LOCALAPPDATA%\DismTool\Logs。返回 true=启动成功。
    static bool RunBatGuarded(HWND hwnd, const wchar_t* pszBatName, const CDuiString& sBody,
                              const CDuiString& sWorkDir, bool bElevated,
                              bool bAppendFailGuard, HANDLE* phProcess = nullptr);

    // ---- 纯文件 / 路径探测 ----
    // 以进程 ANSI 代码页写 .bat（原文件 static WriteBatFile）。
    static bool WriteBatFile(const CDuiString& sPath, const CDuiString& sContent);
    // BIOS 引导扇区 etfsboot.com（原 FindEtfsboot）。
    static CDuiString FindEtfsboot();
    // UEFI 引导扇区 efisys.bin（原 FindEfisys）。
    static CDuiString FindEfisys();
    // [P0 2026-10-07] bootsect.exe（BIOS 引导扇区写入）：
    // 随包(bootsect.exe/bin/bin64/Boot) → PE/系统 System32 → ADK Deployment
    // Tools 的 BCDBoot 目录 → 固定盘 Program Files\Windows Kits 扫描兜底。
    static CDuiString FindBootsect();
    // 工具可用性三阶段探测（AppPaths/System32/PATH；原 IsToolAvailable）。
    static bool IsToolAvailable(const wchar_t* pszTool);

    // ---- [R107] 工具箱统一进度：读取 %LOCALAPPDATA%\DismTool\Logs\progress.txt ----
    // ArchiveToolRun 在每次工具启动/结束时写入 tool/timestamp/status 三字段
    // （[R109] 再补 pct 字段：启动写 0、完成写 100）。
    // 返回 true=文件可读且字段完整；false=无记录（未启动过或已清理）。
    static bool ReadToolProgress(CDuiString& sTool, CDuiString& sTimestamp,
                                 CDuiString& sStatus, int& nPct);

    // ---- ShellExecuteW 返回值判错（HINSTANCE → INT_PTR 的单一转换点） ----
    // ShellExecuteW 的"成功"是 > 32 的伪句柄，<= 32 为错误码。
    // 每处调用点各写一次 reinterpret_cast<INT_PTR> 会把类型转换散落到业务代码；
    // 判定与转换都收在本函数内，业务侧只写 ShellExecFailed(hInst)。
    [[nodiscard]] static bool ShellExecFailed(HINSTANCE hInst) noexcept
    {
        return reinterpret_cast<INT_PTR>(hInst) <= 32;
    }
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
