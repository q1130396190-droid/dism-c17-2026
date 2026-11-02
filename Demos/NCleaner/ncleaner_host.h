// ============================================================================
//  ncleaner_host.h
//
//  宿主回调解析层 —— 等价原版 NCleaner.dll 对 Dism++x64.exe 的「反向导入」：
//  NCleaner 在 DLL_PROCESS_ATTACH 后，从宿主模块解析 6 个回调函数指针，
//  形成进程内微服务（知识库《52_B》2.2 / 《21》2.3）。
//
//  6 个宿主回调（顺序与逆向导入表一致）：
//    DismGetAllUsersAppx        枚举所有用户的 Appx
//    DismRegOpenKeyEx           打开注册表键
//    DismWriteLog               写日志（复用宿主日志通道）
//    DismGetSystemInfoBySession 按会话取系统信息
//    DismRemoveAppx             移除 Appx 包
//    DismFreeMemory             释放内存
//
//  解析顺序（与原版一致：宿主优先，独立运行兜底）：
//    1) GetModuleHandleW(nullptr) —— 宿主进程本体；
//    2) 若当前不是宿主进程（如被测试宿主加载），回退到显式
//       LoadLibraryExW 已加载的模块名。
//  解析失败不阻断 DllMain：各调用点自行判空并按原版语义返回错误码。
// ============================================================================

#pragma once

#include "ncleaner_stdafx.h"

namespace NCleanerHost
{

// ---- 宿主回调原型 ----------------------------------------------------------
// 采用与原版一致的 C 调用约定 + wchar_t* 文件名模型（跨模块不跨边界分配）。

// 枚举所有用户的 Appx。返回 Appx 条目数；pNames 可空（仅计数）。
// pNames 为调用方提供的缓冲区，元素为 [MAX_PATH] 宽的包全名，nCap 为容量。
using PFN_DismGetAllUsersAppx = int ( __cdecl*)(wchar_t* pNames, int nCap);

// 打开注册表键。成功返回 0，否则返回 Win32 错误码；句柄经 phKey 回传。
using PFN_DismRegOpenKeyEx = int ( __cdecl*)(HKEY hRoot, const wchar_t* pSubKey,
                                            DWORD dwSam, HKEY* phKey);

// 写日志。level 0..4（与原版 DismWriteLog 分级一致）。
using PFN_DismWriteLog = void ( __cdecl*)(int nLevel, const wchar_t* pText);

// 按会话取系统信息。返回 1 成功 / 0 失败，文本写入 pBuf（容量 cch）。
using PFN_DismGetSystemInfoBySession = int ( __cdecl*)(DWORD dwSession,
                                                      wchar_t* pBuf, DWORD cch);

// 移除 Appx 包。返回 HRESULT。
using PFN_DismRemoveAppx = long ( __cdecl*)(const wchar_t* pFullName);

// 释放宿主内部分配的内存。
using PFN_DismFreeMemory = void ( __cdecl*)(void* p);

// ---- 解析与访问 ------------------------------------------------------------

// 在 DllMain(PROCESS_ATTACH) 后调用一次；幂等。
// 返回成功解析到的回调个数（0..6）。不抛出、不阻断。
int  Init();

// 释放（DllMain PROCESS_DETACH 时调用）。
void Shutdown();

// 是否已就绪。bRequireAll=true 时要求 6 个回调全部解析成功。
bool IsReady(bool bRequireAll = false);

// 取回可空的函数指针（未解析返回 nullptr）。
PFN_DismGetAllUsersAppx        GetAllUsersAppx();
PFN_DismRegOpenKeyEx           RegOpenKeyEx();
PFN_DismWriteLog               WriteLog();
PFN_DismGetSystemInfoBySession GetSystemInfoBySession();
PFN_DismRemoveAppx             RemoveAppx();
PFN_DismFreeMemory             FreeMemory();

// ---- 便捷封装（内部按原版日志模板输出） ------------------------------------

// 等价原版 `DismRemoveAppx(%s) ==> 0x%08lX`
long CallRemoveAppx(const wchar_t* pFullName);

// 等价原版 `NCQueryEventLogSize(%s) ==> 0x%08lX` 等模板的通用日志。
void Log(const wchar_t* pFormat, ...);

// 写日志（带级别）。宿主不可用时退化为 OutputDebugStringW。
void LogLevel(int nLevel, const wchar_t* pText);

}  // namespace NCleanerHost
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
inline constexpr int kCpp17H_ncleaner_host_PageSize = 4096;
inline constexpr int kCpp17H_ncleaner_host_MaxBuf = 8192;
