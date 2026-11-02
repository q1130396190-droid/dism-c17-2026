#pragma once

// ============================================================
// ProgramService —— S4（v2 P3a）
// 已安装程序枚举：HKLM 64 位 / WOW6432Node / HKCU 三个 Uninstall
// 注册表源，过滤 SystemComponent/ParentKeyName/重名，按名称排序。
// 纯 Win32 注册表读取，无 UI 控件依赖；
// 实现文件不使用预编译头、不 include UIlib.h / StringManager.h。
//
// ProgEntry 与枚举循环体逐字摘自原 CFrameWnd 嵌套结构与
// CFrameWnd::BuildProgramPage 的枚举段（物理搬迁，零逻辑改动）。
// ============================================================

#include <windows.h>

#ifndef UILIB_API
#define UILIB_API __declspec(dllimport)
#endif

#include "Utils/Utils.h"
#include <vector>

// One installed program read from the Uninstall registry keys.
struct ProgEntry
{
    ProgEntry() : nRoot(0), bSigned(false), bMsft(false), bIsMsi(false) {}
    int        nRoot;        // 0 HKLM 64bit, 1 HKLM WOW6432Node, 2 HKCU
    DuiLib::CDuiString sSubKey;
    DuiLib::CDuiString sName;
    DuiLib::CDuiString sDate;
    DuiLib::CDuiString sSize;
    DuiLib::CDuiString sUninstall;   // command line used to remove the program
    // R34: 发布者 / 签名 / 安装位置 / MSI 标记
    DuiLib::CDuiString sPublisher;   // Publisher value
    DuiLib::CDuiString sInstallLoc;  // InstallLocation value (may be empty)
    DuiLib::CDuiString sIconBase;    // [R47] basename of DisplayIcon (install icon hint)
    bool       bSigned;      // MsftSig DWORD != 0 OR 有 Signer 子键
    bool       bMsft;        // MsftSig DWORD == 1 (Microsoft signed)
    bool       bIsMsi;       // true when UninstallString uses MsiExec / 或子键是 {GUID}
};

class ProgramService
{
public:
    // 枚举三个 Uninstall 源的有效程序项，按显示名不区分大小写排序。
    // 结果追加进 vOut（调用方通常传入空向量）。
    static void EnumeratePrograms(std::vector<ProgEntry>& vOut);
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
