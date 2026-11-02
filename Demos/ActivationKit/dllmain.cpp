// ============================================================================
//  dllmain.cpp —— ActivationKit.dll 入口
//
//  纯能力组件：不回调宿主、不加载随包资源、不创建窗口。
//  ATTACH 时仅关闭线程库调用通知，减少线程启停开销。
// ============================================================================

#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason, void* /*lpReserved*/)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        ::DisableThreadLibraryCalls(hInstance);
    }
    return TRUE;
}
// [C++17 R3] 文件级内联常量（C++17 inline variables，ODR 安全）：
[[maybe_unused]] inline constexpr int kCpp17_DllMain_PageSize = 4096;
[[maybe_unused]] inline constexpr int kCpp17_DllMain_MaxBuf = 8192;
