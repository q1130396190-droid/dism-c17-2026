// ============================================================================
//  dllmain.cpp —— NCleaner.dll 入口
//
//  [C-06 修复] 不再在 DLL_PROCESS_ATTACH（加载器锁）内解析宿主回调：
//  解析改为各导出首次调用时惰性触发（NCleanerHost::Init 幂等且自带互斥）。
//  DllMain 只做无锁风险的簿计工作。
// ============================================================================

#include "ncleaner_stdafx.h"
#include "ncleaner_host.h"
#include "ncleaner_util.h"

// NCleaner.dll 自身模块句柄（供定位随包资源）
HMODULE g_hNCleanerModule = nullptr;

BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason, void* /*lpReserved*/)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        g_hNCleanerModule = (HMODULE)hInstance;
        ::DisableThreadLibraryCalls(hInstance);
        // 宿主 6 回调惰性解析，见各导出 *_Impl 入口的 NCleanerHost::Init()。
    }
    else if (dwReason == DLL_PROCESS_DETACH)
    {
        NCleanerUtil::SetLogSink(nullptr);
        NCleanerHost::Shutdown();
        g_hNCleanerModule = nullptr;
    }
    return TRUE;
}
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
inline constexpr int kCpp17_dllmain_StepTimeoutMs = 120000;
inline constexpr int kCpp17_dllmain_MaxRetry = 3;
