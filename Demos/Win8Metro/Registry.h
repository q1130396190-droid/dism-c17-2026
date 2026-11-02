#pragma once

// ============================================================
// Registry —— S8 切片按需下沉的注册表只读基础层（v2 P2c）。
// 原 FrameWnd.cpp 内 static RegGetString，被 S8 AssocService 复用，
// 去 static 提为全局函数；FrameWnd.cpp 内既有调用点零改动。
// 纯 Win32，无 UI 控件依赖；实现不使用预编译头。
// ============================================================

#include <windows.h>

#ifndef UILIB_API
#define UILIB_API __declspec(dllimport)
#endif

#include "Utils/Utils.h"

// 读取字符串值（REG_SZ / REG_EXPAND_SZ）；不存在或失败返回空串。
DuiLib::CDuiString RegGetString(HKEY hRoot, const wchar_t* pszPath, const wchar_t* pszValue,
                                REGSAM samDesired = KEY_READ | KEY_WOW64_64KEY);
// REG_SZ / REG_EXPAND_SZ 写入时的数据指针：wchar_t* → const BYTE*。
// RegSetValueExW 的数据参数是 const BYTE*，每个调用点各写一次 reinterpret_cast
// 会把转换散落到业务代码。统一收在本函数内，业务侧只写 AsRegSzBytes(psz)。
[[nodiscard]] inline const BYTE* AsRegSzBytes(const wchar_t* pszData) noexcept
{
    return reinterpret_cast<const BYTE*>(pszData);
}

// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
