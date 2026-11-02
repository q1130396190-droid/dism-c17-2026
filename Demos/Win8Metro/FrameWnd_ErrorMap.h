// FrameWnd_ErrorMap.h
// 错误码 → 中文友好提示映射表（后台模块，不涉及 UI）
// 覆盖 dism.exe / CBSHost.dll / compact.exe / sfc.exe 常见 exit code
#pragma once

#include "WinBase.h"

namespace ErrorMap {

    // 返回 exit code 对应的中文友好错误提示。
    // 未识别的 code 会返回通用提示 + exit code 数值（用于人工排查）。
    const wchar_t* GetFriendlyMessage(DWORD dwExit);

    // 返回 exit code 的一句话说明（用于日志与审计），比 GetFriendlyMessage 更技术化。
    const wchar_t* GetTechnicalMessage(DWORD dwExit);

    // 将 (exit code, source, target) 追加写入 clean_errors.log（UTF-8，时间戳格式）。
    // 失败返回 FALSE（磁盘/权限问题不会阻塞主流程）。
    BOOL LogError(DWORD dwExit, const wchar_t* szSource, const wchar_t* szTarget);

    // 供外部一次性读取最近错误条数（内存计数器，仅用于统计）。
    [[nodiscard]] DWORD GetErrorCount();
}
