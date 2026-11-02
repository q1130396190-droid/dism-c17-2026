#pragma once

// ============================================================
// WofUncompactJob —— [R16][T8] CompactOS 撤销后台作业（BackgroundJobs 体系）
// 原 FrameWnd_Menu.cpp 内联的线程体/进度回传（WofUncompactThreadProc/
// PostWofLog/上下文结构）整体迁入本模块；菜单层仅保留"状态检查 →
// 确认 → Start 启动"轻委托。
// ============================================================

#include <windows.h>

namespace WofUncompactJob
{
    // [P0-A2] CompactOS 状态原生查询（WofCompact，替代 compact.exe
    // /compactos:query 的本地化文本匹配）。返回：0=未压缩 1=部分 2=已优化 -1=失败
    int QueryState();

    // [CRASHFIX] 启动整卷 CompactOS 撤销后台作业：双层 SEH 边界、
    // 可被 WM_CLOSE 等待（BackgroundJobs 登记）。进度/完成经 WM_WOF_LOG 回传。
    // 返回 false = 线程创建失败（调用方记日志）。
    [[nodiscard]] bool Start(HWND hWnd);
}
