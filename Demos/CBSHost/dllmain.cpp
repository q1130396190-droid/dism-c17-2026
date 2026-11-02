// ============================================================================
//  dllmain.cpp —— CBSHost.dll 入口
// ============================================================================

#include "cbshost_stdafx.h"

// CBSHost.dll 自身模块句柄，供 CbsHostLocal 定位随包工具
HMODULE g_hCbsHostModule = nullptr;

BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason, void* /*lpReserved*/)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        g_hCbsHostModule = (HMODULE)hInstance;
        ::DisableThreadLibraryCalls(hInstance);
    }
    else if (dwReason == DLL_PROCESS_DETACH)
    {
        g_hCbsHostModule = nullptr;
    }
    return TRUE;
}
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
inline constexpr int kCpp17_dllmain_StepTimeoutMs = 120000;
inline constexpr int kCpp17_dllmain_MaxRetry = 3;
