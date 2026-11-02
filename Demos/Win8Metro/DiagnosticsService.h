#pragma once

// ============================================================
// DiagnosticsService —— S12（P1）
// 8 个诊断快照（sysinfo / Top20 进程 / 服务 / 启动事件 /
// 驱动签名 / 回滚候选 / 碎片 / 启动时间），纯 Win32 + STL，
// 输出 CDuiString 文本并落盘 %LOCALAPPDATA%\DismTool_Clone\logs。
//
// 实现文件不使用预编译头、不 include UIlib.h / StringManager.h。
// 本地化文案经 Localizer 回调由核心壳注入（转发到 StringMgr.Get）。
// ============================================================

#include <windows.h>

#ifndef UILIB_API
#define UILIB_API __declspec(dllimport)
#endif

#include "Utils/Utils.h"

// 本地化查询：参数为 UTF-8 key（与 CStringManager::Get(const char*) 同签名），
// 核心壳在 StringMgr.Init 之后调用 DiagnosticsService_BindLocalizer 绑定。
// 用默认 __cdecl 以便无捕获 lambda 直接转换（x86 下不可用 CALLBACK/__stdcall）。
using DiagLocalizeFn = const wchar_t* (*)(const char* pszKeyUtf8);
void DiagnosticsService_BindLocalizer(DiagLocalizeFn fn);

class DiagnosticsService
{
public:
    static DuiLib::CDuiString AppendSysInfoSnapshotToFile();      // sysinfo_history.log
    static DuiLib::CDuiString Top20ProcessesToLog();              // proctop20.log
    static DuiLib::CDuiString SnapshotServicesToLog();            // services_snapshot.log
    static DuiLib::CDuiString ParseBootEventsToLog();             // bootevents.log
    static DuiLib::CDuiString VerifyDriverSignaturesToLog();      // driversignature.log
    static DuiLib::CDuiString EnumerateRollbackCandidatesToLog(); // rollback_candidates.log
    static DuiLib::CDuiString AnalyzeDiskFragmentationToLog();    // fragmentation.log
    static DuiLib::CDuiString AppendBootTimeSnapshotToFile();     // boottime_history.log
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
