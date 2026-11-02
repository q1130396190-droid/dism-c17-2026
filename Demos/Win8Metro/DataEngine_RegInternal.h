#pragma once
// ============================================================
// DataEngine_RegInternal.h —— [R9-02] 注册表执行器内部共享件。
//
// 原 DataEngine.cpp 顶部文件局部 Wow64EnableRegistryKey 动态加载 +
// Wow64Scope RAII；DoReg* 原语迁入 DataEngine_Reg.cpp 后两 TU 共用。
// 仅 DataEngine.cpp / DataEngine_Reg.cpp 可包含，非公开头。
// ============================================================
#include <windows.h>

using PFN_Wow64EnableRegistryKey = BOOL ( WINAPI*)(HKEY hKey, BOOL bEnableRedirection);

// RAII：作用域内关闭某 HKEY 的重定向，析构时恢复。
// 仅在进程为 64 位且 Wow64EnableRegistryKey 可用时生效；其它情况为 no-op。
class Wow64Scope
{
public:
    explicit Wow64Scope(HKEY hKey) : m_hKey(hKey), m_bEntered(false)
    {
        // 函数局部静态：首次构造时动态解析 kernel32!Wow64EnableRegistryKey，
        // 线程安全初始化（32 位 kernel32 无此导出时保持 nullptr=no-op）。
        static PFN_Wow64EnableRegistryKey s_pfn = []() -> PFN_Wow64EnableRegistryKey
        {
            HMODULE hK = ::GetModuleHandleW(L"kernel32.dll");
            return hK ? reinterpret_cast<PFN_Wow64EnableRegistryKey>(
                ::GetProcAddress(hK, "Wow64EnableRegistryKey")) : nullptr;
        }();
        if (s_pfn && m_hKey)
        {
            if (s_pfn(m_hKey, FALSE) == TRUE)
                m_bEntered = true;
        }
    }
    ~Wow64Scope()
    {
        static PFN_Wow64EnableRegistryKey s_pfn = []() -> PFN_Wow64EnableRegistryKey
        {
            HMODULE hK = ::GetModuleHandleW(L"kernel32.dll");
            return hK ? reinterpret_cast<PFN_Wow64EnableRegistryKey>(
                ::GetProcAddress(hK, "Wow64EnableRegistryKey")) : nullptr;
        }();
        if (m_bEntered && s_pfn)
            s_pfn(m_hKey, TRUE);
    }
    Wow64Scope(const Wow64Scope&) = delete;
    Wow64Scope& operator=(const Wow64Scope&) = delete;
private:
    HKEY m_hKey;
    BOOL m_bEntered;
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
