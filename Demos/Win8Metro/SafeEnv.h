#pragma once

// ============================================================================
// SafeEnv.h —— 安全环境变量读取封装（C++17 升级·B6 告警清零）
//
// 为什么需要：
//   `_wgetenv` 在 MSVC 已弃用（C4996），触发编译告警。本封装改用
//   GetEnvironmentVariableW 两段式读取，语义与原 _wgetenv 等价：
//     - 变量不存在 / 为空 → 返回空 std::wstring
//     - 变量存在           → 返回完整 UTF-16 值
//   与 FormatW.h / WinHandles.h 同属 C++17 升级允许的新增封装文件。
//
// 用法：
//   std::wstring sLocal = SafeGetEnv(L"LOCALAPPDATA");
//   sDir.Format(L"...%s...", SafeGetEnv(L"LOCALAPPDATA").c_str());
// ============================================================================

#include <windows.h>
#include <string>

namespace winenv
{

// 读取环境变量；不存在返回空串（与 _wgetenv 返回 nullptr 语义等价）。
inline std::wstring SafeGetEnv(const wchar_t* pszName)
{
    if (!pszName || !*pszName) return std::wstring();

    // 第一次调用取所需长度（含 null）
    DWORD nLen = ::GetEnvironmentVariableW(pszName, nullptr, 0);
    if (nLen == 0) return std::wstring();

    // 第二次调用实际读取
    std::wstring s(nLen, L'\0');
    DWORD nWritten = ::GetEnvironmentVariableW(pszName, &s[0], nLen);
    if (nWritten == 0 || nWritten > nLen) return std::wstring();

    s.resize(nWritten);
    return s;
}

} // namespace winenv
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
