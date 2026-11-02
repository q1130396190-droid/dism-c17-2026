#pragma once

// ============================================================
// ProcessRunner —— S9 进程能力基础层（v2 P2b 首次从 FrameWnd.cpp
// 抽取；S11 ScheduleTaskService 是第一个复用者，后续 S9 切片再承接
// LaunchSystemEx/StartTool/StartDism 等更高层封装）。
//
// 规则：纯 Win32 进程调用，无 UI 控件依赖；实现文件不使用预编译头、
// 不 include UIlib.h。函数签名/行为与原 FrameWnd.cpp 内 static 版本
// 完全一致（物理搬迁，零逻辑改动）。
// ============================================================

#include <windows.h>

#ifndef UILIB_API
#define UILIB_API __declspec(dllimport)
#endif

#include "Utils/Utils.h"
#include <vector>

class ProcessRunner
{
public:
    // 解析工具全路径：优先 exe 同目录下同名工具（绿色部署），回退 %System32%
    static DuiLib::CDuiString ResolveSysTool(const wchar_t* pszExe);

    // 隐藏运行 %System32%\<exe>，不捕获输出，固定等待 dwWaitMs
    static void RunHidden(const wchar_t* pszExe, const DuiLib::CDuiString& sArgs,
                          DWORD dwWaitMs = 20000);

    // 隐藏运行并带消息泵等待结束，返回退出码；
    // CreateProcess 失败返回 (DWORD)-1；超时返回时进程仍在运行（可能为 259）
    // [FIX-P1H] bKillOnTimeout=false 时超时不终止子进程（bcdboot 等关键
    // 引导操作中途强杀可能留下半写 BCD/半完成 CBS 事务），仅告警留痕。
    static DWORD RunHiddenExit(const wchar_t* pszExe, const DuiLib::CDuiString& sArgs,
                               DWORD dwWaitMs = 180000, bool bKillOnTimeout = true);

    // [FIX-P1H] UI 线程白名单泵等待：等待单个内核对象（可为 nullptr=纯定时
    // 泵），期间按与 RunHiddenExit 同源的契约派发窗口消息 —— 绘制/激活立即
    // 派发；业务消息(WM_USER/WM_APP 族)与 WM_TIMER 暂存回投；输入/命令丢弃；
    // WM_QUIT 转投退出。供 UI 线程上的轮询场景使用（服务状态轮询/虚拟光驱
    // 盘符探测等），替代裸 Sleep 防止窗口 5s 判"未响应"。
    // 返回值同 WaitForSingleObject：WAIT_OBJECT_0 / WAIT_TIMEOUT。
    static DWORD WaitHandleWithUiPump(HANDLE hObject, DWORD dwTimeoutMs);

    // 隐藏运行并捕获合并 stdout/stderr（ANSI → wchar_t）；
    // 管道异步读避免填满死锁，超时 TerminateProcess；pdwExit 可为 nullptr
    static bool RunCapture(const wchar_t* pszExe, const DuiLib::CDuiString& sArgs,
                           DuiLib::CDuiString& sOut, DWORD dwWaitMs = 60000,
                           DWORD* pdwExit = nullptr);

    // 简化版隐藏捕获（原仅用于 schtasks.exe 查询），成功返回 0，失败 -1
    static int RunHiddenCapture(const wchar_t* pszExe, const DuiLib::CDuiString& sArgs,
                                DuiLib::CDuiString& sOutput);

    // P4：原始字节捕获（逐字摘自原 FrameWnd.cpp static RunCapture）。
    // DISM 输出可能是 UTF-16LE，调用方自行用 DecodeBytes 解码；
    // 超时 TerminateProcess，*pdwExit 语义同原函数（出参 dwExit）。
    static bool RunCaptureBytes(const wchar_t* pszExe, const DuiLib::CDuiString& sArgs,
                                std::vector<char>& vBytes, DWORD& dwExit);
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
