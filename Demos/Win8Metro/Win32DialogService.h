#pragma once

// ============================================================
// Win32DialogService —— S2（P1）
// 通用文件对话框（打开/保存/浏览目录），纯 Win32，无 UI 控件依赖。
// 实现文件不使用预编译头、不 include UIlib.h。
// ============================================================

#include <windows.h>

#ifndef UILIB_API
#define UILIB_API __declspec(dllimport)
#endif

#include "Utils/Utils.h"

class Win32DialogService
{
public:
    static bool DlgOpenFile(DuiLib::CDuiString& sResult, const wchar_t* pszFilter, const wchar_t* pszTitle);
    static bool DlgSaveFile(DuiLib::CDuiString& sResult, const wchar_t* pszFilter,
                            const wchar_t* pszTitle, const wchar_t* pszDefName = nullptr);
    static bool DlgBrowseFolder(DuiLib::CDuiString& sResult, const wchar_t* pszTitle);
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
