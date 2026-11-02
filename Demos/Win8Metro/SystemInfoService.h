#pragma once

// ============================================================
// SystemInfoService —— S1（P1）
// 纯系统信息查询，无 UI 依赖：头文件仅依赖 DuiLib Utils（CDuiString）。
// 实现文件不使用预编译头、不 include UIlib.h / 任何控件头。
// Utils.h 的类带 UILIB_API 标记（UIlib.h 按 UILIB_EXPORTS 二选一）；
// 服务编译单元不引入 UIlib.h，这里按 demo 侧（导入 DuiLib.dll）补齐宏。
// ============================================================

#include <windows.h>

#ifndef UILIB_API
#define UILIB_API __declspec(dllimport)
#endif

#include "Utils/Utils.h"

class SystemInfoService
{
public:
    // "C:" where C:\Windows lives
    static DuiLib::CDuiString GetSystemDriveStr();
    static DuiLib::CDuiString GetDesktopDir();
    static DWORD               GetBuildNumber();
    static DuiLib::CDuiString GetWindowsFamilyName();   // >=22000 → Windows 11
    static DuiLib::CDuiString MakeTimestamp();          // yyyyMMdd_HHmmss
    // create %TEMP%\<prefix>_<guid> and return its full path
    static DuiLib::CDuiString MakeTempWorkDir(const wchar_t* pszPrefix);

    // [P0 2026-10-07] 运行环境检测（知识库 v33 WinPE 报告）
    static bool IsWinPE();        // MiniNT 键 / WinPE 版本键 / X: RAM 盘三重判定
    static bool IsUEFIBoot();     // 固件变量探测（ERROR_INVALID_FUNCTION=BIOS，其余=UEFI）
    // 启用 SeBackup/SeRestore/SeSecurity（兼带加载驱动/关机/夺取所有权）特权
    static bool EnableSystemPrivileges();

    // [P0 2026-10-07] WinPE 离线目标（待服务的已安装 Windows 卷根，如 "D:\" 或
    // "D:\Mount\Win"）。PE 下本进程运行在 X: RAM 盘，优化/清理/驱动必须定向到
    // 该目标；FindOfflineWindowsDir 自动枚举固定盘上的 Windows 安装（跳过 X:
    // 与当前系统卷），未发现返回空串。SetOfflineTarget 同时供各模块统一查询。
    static DuiLib::CDuiString FindOfflineWindowsDir();
    static void SetOfflineTarget(const wchar_t* pszRoot);
    static DuiLib::CDuiString GetOfflineTarget();
    static bool HasOfflineTarget();
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
