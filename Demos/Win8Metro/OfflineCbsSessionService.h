#pragma once

// ============================================================
// OfflineCbsSessionService —— [R16][T8] 离线源目录 CBS 深链路会话服务
// ------------------------------------------------------------
// 原 FrameWnd_Menu.cpp::MenuCmdAddSourceDir 内联的约 80 行深链路
// （前置校验 → 停 TI → 挂蜂巢 → 会话头日志 → CbsHost.log 接线 →
// 结果文案组装）下沉至本服务；菜单层仅保留"选目录 → 委托 →
// HostAlert 结果提示"轻委托。纯 std::wstring 输出，无 DuiLib 依赖。
// ============================================================

#include <windows.h>
#include <string>

namespace OfflineCbsSession
{

// PrepareSourceDir 结果：用户可见文案 + 图标分级
// （语义与基线三段 MessageBox 分支逐字一致，2026-10-13）
struct TPrepareResult
{
    bool         bReady;      // true = 前置校验通过并已执行深链路
    bool         bWarning;    // true=MB_ICONWARNING，false=MB_ICONINFORMATION
    std::wstring sTitle;      // 固定："离线 CBS 深链路"
    std::wstring sMessage;    // 结果文案（与原弹窗内容逐字一致）
};

// 离线源目录深链路：CheckReadiness →（通过：StopTrustedInstaller →
// MountAllHives → BeginOfflineCbsSession → CbsHostClient 同源留痕）
// → 组装结果文案。不提交卸载，由退出路径统一释放（保持原行为）。
TPrepareResult PrepareSourceDir(const std::wstring& sMountRoot);

} // namespace OfflineCbsSession
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
