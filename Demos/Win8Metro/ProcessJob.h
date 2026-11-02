#pragma once
// ============================================================================
// ProcessJob.h - 子进程生命周期随主进程终结（Kill-On-Close Job Object）
// ----------------------------------------------------------------------------
// 背景缺陷：本程序通过 CreateProcess 拉起大量工具子进程（dism/powershell/
//   sfc/sysprep/prowck/wimg 回调链/ESD 转换等），此前没有任何作业对象管理：
//   主程序正常退出、崩溃或被结束时，这些子进程全部沦为孤儿继续运行；
//   同时后台工作线程阻塞在 WaitForSingleObject(子进程) 上，叠加析构期
//   FreeLibrary 竞态，导致 ExitProcess 在加载器锁阶段永久挂起，进程变成
//   连 SYSTEM/taskkill /F 都无法终结的僵尸（只能重启清除）。
//
// 修复机制：
//   1) WinMain 早期调用 EnsureCreated()：创建匿名作业，设置
//      JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE（作业最后一个句柄关闭时，内核
//      强制终止作业内全部进程）并把主进程自身放入作业。
//   2) Windows 8+ 嵌套作业语义下，主进程之后 CreateProcess 创建的所有
//      子进程默认自动加入同一作业（零侵入，无需修改任何调用点）；
//      Win7 单作业时代此默认行为同样成立。
//   3) 主进程无论正常退出、TerminateProcess 兜底强退还是崩溃，作业句柄
//      由内核关闭 -> 全部子进程被内核连带终止，杜绝孤儿。
//   4) 崩溃自重启(CrashDumpFilter)/语言切换接力等“接班进程”必须以
//      CREATE_BREAKAWAY_FROM_JOB 标志启动（作业设置了 BREAKAWAY_OK），
//      避免新实例被旧实例的作业连带杀死。
//   5) WM_CLOSE 时调用 KillChildren() 主动提前杀光子进程，使阻塞在
//      子进程句柄上的后台工作线程及时解除等待，保证退出路径不挂死。
//
// 注意：作业句柄刻意永不 CloseHandle（其关闭即终止信号，依赖进程终结）。
// ============================================================================
#include <windows.h>

namespace ProcessJob {

// 幂等创建作业并把当前进程加入。失败时静默降级（返回 false），
// 不影响程序任何既有功能（仅失去子进程连带终结保护）。
bool EnsureCreated();

// 终止作业内除当前进程外的全部进程（多轮枚举，覆盖杀进程期间新派生者）。
// 返回累计终止的进程数；作业不可用时返回 0。
int  KillChildren();

// 作业句柄（仅用于诊断/判断；调用方不得 CloseHandle）。
HANDLE Handle();

} // namespace ProcessJob
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
