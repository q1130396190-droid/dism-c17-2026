// ============================================================================
//  cbshost_local_process.h
//
//  独立 CBSHost.dll 的本地进程能力，替代对 ProcessRunner（依赖 DuiLib
//  CDuiString）和 ExternalCmdLedger 的引用。仅用原生 Win32 API。
// ============================================================================

#pragma once

#include <windows.h>
#include <string>

namespace CbsHostLocal
{

// 工具三级定位：
//   1) CBSHost.dll 同目录（绿色随包，如 bin64\dism.exe）
//   2) %SystemRoot%\System32
//   3) Windows ADK Deployment Tools（各固定盘）
// 找到返回 true 并写入 sOut 全路径；全部未命中返回 false。
bool ResolveTool(const wchar_t* pszExe, std::wstring& sOut);

// 隐藏运行指定全路径工具，等待结束并取真实退出码。
// CreateProcess 失败返回 false；超时返回 true 且 *pdwExit=STILL_ACTIVE(259)。
bool RunHiddenExit(const wchar_t* pszToolFull, const wchar_t* pszArgs,
                   DWORD dwWaitMs, DWORD* pdwExit);

} // namespace CbsHostLocal
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
inline constexpr int kCpp17H_cbshost_local_process_PageSize = 4096;
inline constexpr int kCpp17H_cbshost_local_process_MaxBuf = 8192;
