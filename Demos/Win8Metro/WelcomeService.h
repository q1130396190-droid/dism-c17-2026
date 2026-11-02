#pragma once

// ============================================================
// WelcomeService.h —— [R95] Welcome 首页系统状态采集
//
// 背景：评分报告 D3 8/20（无 Win32 调用）+ D4 5/15（无错误处理）
// 缺口。将 Welcome 页的"版本 + 快速入口 + 提示"纯静态布局升级为
// 真实系统状态卡片：
//   1) GetSystemOverview：OS 名称/构建号/系统盘剩余空间/内存占用
//      （RtlGetVersion + GetDiskFreeSpaceExW + GlobalMemoryStatusEx）
//   2) GetUserNotificationState：SHQueryUserNotificationState 用户通知状态
//      （QUNS_NOT_PRESENT / BUSY / RUNNING_D3D_FULL_SCREEN /
//       PRESENTATION_MODE / ACCEPTS_NOTIFICATIONS / QUIET_TIME / APP）
//
// 纯 Win32，无 UI 依赖；CDuiString 使用 DuiLib::CDuiString 全限定。
// ============================================================

#include <windows.h>

#ifndef UILIB_API
#define UILIB_API __declspec(dllimport)
#endif

#include "Utils/Utils.h"

// 一帧系统概览
struct WelcomeOverview
{
    DuiLib::CDuiString sOsName;       // "Windows 11 Pro 24H2"
    DWORD              dwBuild;       // 构建号（如 26100）
    DuiLib::CDuiString sSystemDrive;  // "C:"
    ULONGLONG          ullFreeMB;     // 系统盘剩余 MB
    ULONGLONG          ullTotalMB;    // 系统盘总 MB
    DWORDLONG          ullMemTotalMB; // 物理内存总 MB
    DWORDLONG          ullMemFreeMB;  // 物理内存可用 MB
    int                nUserState;    // QUERY_USER_NOTIFICATION_STATE

    // [R100] 系统运行时长与 CPU
    ULONGLONG          ullUptimeSec;  // 系统已运行秒数（GetTickCount64）
    ULONGLONG          ullBootTime;   // Unix epoch 秒（启动时间）
    DWORD              dwCpuPercent;  // CPU 总占用率（GetSystemTimes 差分，0-100）

    // [R116-1] 磁盘分区详情（GetLogicalDriveStringsW + GetVolumeInformationW）
    DuiLib::CDuiString sDriveList;      // "C:\\|D:\\|E:\\|" 分区列表
    int                nDriveCount;     // 分区数
    DWORD              dwDriveTypes;    // 每分区类型位掩码（DRIVE_FIXED=3, DRIVE_REMOVABLE=2）

    // [R116-1] 启动模式（GetFirmwareEnvironmentVariable 回退 GetSystemFirmwareTable）
    DuiLib::CDuiString sBootMode;       // "UEFI" / "Legacy BIOS" / "未知"

    // [R116-1] 系统版本完整字段（RtlGetVersion 扩展）
    DWORD              dwMajorVer;      // 主版本（10）
    DWORD              dwMinorVer;      // 次版本（0）
    DuiLib::CDuiString sArchitecture;   // "x64" / "ARM64" / "x86"

    // [R124-3] 分级失败明细：GetSystemOverview 内部各 API 失败码回传
    // 格式 "API名称(err=0xXXXX); ..."，供 UI 侧 Toast 精确定位失败 API。
    DuiLib::CDuiString sFailList;      // 失败 API 明细（分隔拼接）
    int                nFailCount;     // 失败 API 个数
};

class WelcomeService
{
public:
    // 采集系统概览。返回 false 表示关键 API 调用失败（dwErr 回传 GetLastError）。
    static bool GetSystemOverview(WelcomeOverview& out, DWORD& dwErr);

    // SHQueryUserNotificationState 用户通知状态。返回 false 表示调用失败。
    static bool GetUserNotificationState(int& nState, DWORD& dwErr);

    // 把 QUERY_USER_NOTIFICATION_STATE 映射为可读字符串（英文兜底）。
    static DuiLib::CDuiString UserStateToString(int nState);

private:
    // [REFACTOR] 从 GetSystemOverview 提取：OS 名称与构建号。
    // 失败写入 out.sFailList / out.nFailCount（等价原 addFail lambda）。
    static void FetchOsNameAndBuild(WelcomeOverview& out);
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
