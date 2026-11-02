// ============================================================================
//  ncleaner_host.cpp —— 宿主回调解析层实现
//
//  还原原版 NCleaner.dll 的「反向导入 Dism++x64.exe」机制：
//  原版在导入表里静态引用宿主 exe 的 6 个导出；本工程改为运行期
//  从宿主模块导出表解析（GetProcAddress），语义等价且不产生链接期硬依赖。
// ============================================================================

#include "ncleaner_host.h"
#include "ncleaner_util.h"   // [C-05] 宿主层向 util 注册日志 sink（高层→低层）

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace NCleanerHost
{

namespace
{
    HMODULE g_hHost = nullptr;

    PFN_DismGetAllUsersAppx        g_pfnGetAllUsersAppx        = nullptr;
    PFN_DismRegOpenKeyEx           g_pfnRegOpenKeyEx           = nullptr;
    PFN_DismWriteLog               g_pfnWriteLog               = nullptr;
    PFN_DismGetSystemInfoBySession g_pfnGetSystemInfoBySession = nullptr;
    PFN_DismRemoveAppx             g_pfnRemoveAppx             = nullptr;
    PFN_DismFreeMemory             g_pfnFreeMemory             = nullptr;

    int  g_nResolved = 0;
    bool g_bInited   = false;

    // 宿主候选模块名（原版硬编码 "Dism++x64.exe"；本项目宿主为 Win8Metro.exe）。
    // [C-06 修复] 只允许在「已加载模块」中解析（GetModuleHandle），绝不
    // LoadLibrary 候选宿主——该解析可能发生在 DllMain 加载器锁内（历史路径），
    // 锁内加载其他模块是经典死锁源；卫星 DLL 本来就是被宿主加载的，进程
    // 本体句柄必然可用。
    const wchar_t* const kHostCandidates[] = {
        nullptr,                 // 当前进程本体（GetModuleHandleW(nullptr)）
        L"Win8Metro.exe",     // 本项目宿主
        L"Dism++x64.exe",     // 原版宿主（兼容直接改名部署）
    };

    template <typename T>
    T ResolveOne(const char* pszName)
    {
        if (g_hHost == nullptr) return nullptr;
        return reinterpret_cast<T>(::GetProcAddress(g_hHost, pszName));
    }

    void ResolveAll()
    {
        g_nResolved = 0;
        g_pfnGetAllUsersAppx        = ResolveOne<PFN_DismGetAllUsersAppx>("DismGetAllUsersAppx");
        g_pfnRegOpenKeyEx           = ResolveOne<PFN_DismRegOpenKeyEx>("DismRegOpenKeyEx");
        g_pfnWriteLog               = ResolveOne<PFN_DismWriteLog>("DismWriteLog");
        g_pfnGetSystemInfoBySession = ResolveOne<PFN_DismGetSystemInfoBySession>("DismGetSystemInfoBySession");
        g_pfnRemoveAppx             = ResolveOne<PFN_DismRemoveAppx>("DismRemoveAppx");
        g_pfnFreeMemory             = ResolveOne<PFN_DismFreeMemory>("DismFreeMemory");

        if (g_pfnGetAllUsersAppx)        ++g_nResolved;
        if (g_pfnRegOpenKeyEx)           ++g_nResolved;
        if (g_pfnWriteLog)               ++g_nResolved;
        if (g_pfnGetSystemInfoBySession) ++g_nResolved;
        if (g_pfnRemoveAppx)             ++g_nResolved;
        if (g_pfnFreeMemory)             ++g_nResolved;
    }
}  // namespace

int Init()
{
    // [C-06] 惰性初始化：不在 DllMain 加载器锁内执行；多工作线程并发首调
    // 时经互斥保证只解析一次。
    static std::mutex s_mtxInit;
    std::lock_guard<std::mutex> lk(s_mtxInit);
    if (g_bInited) return g_nResolved;
    g_bInited = true;

    // 逐个候选尝试：命中即停止（与原版"宿主优先"一致）。
    // 只查已加载模块，不 LoadLibrary（见 kHostCandidates 注释）。
    for (size_t i = 0; i < _countof(kHostCandidates); ++i) {
        HMODULE h = (kHostCandidates[i] == nullptr)
                        ? ::GetModuleHandleW(nullptr)
                        : ::GetModuleHandleW(kHostCandidates[i]);
        if (h == nullptr) continue;

        g_hHost = h;
        ResolveAll();
        if (g_nResolved > 0) break;      // 找到可用宿主
        g_hHost = nullptr;
    }

    // [C-05] 向 util 层注册日志通道（依赖方向：host → util）。
    NCleanerUtil::SetLogSink(&LogLevel);

    return g_nResolved;
}

void Shutdown()
{
    g_hHost = nullptr;
    g_pfnGetAllUsersAppx        = nullptr;
    g_pfnRegOpenKeyEx           = nullptr;
    g_pfnWriteLog               = nullptr;
    g_pfnGetSystemInfoBySession = nullptr;
    g_pfnRemoveAppx             = nullptr;
    g_pfnFreeMemory             = nullptr;
    g_nResolved = 0;
    g_bInited   = false;
}

bool IsReady(bool bRequireAll)
{
    if (bRequireAll) return g_nResolved == 6;
    return g_nResolved > 0;
}

PFN_DismGetAllUsersAppx        GetAllUsersAppx()        { return g_pfnGetAllUsersAppx; }
PFN_DismRegOpenKeyEx           RegOpenKeyEx()           { return g_pfnRegOpenKeyEx; }
PFN_DismWriteLog               WriteLog()               { return g_pfnWriteLog; }
PFN_DismGetSystemInfoBySession GetSystemInfoBySession() { return g_pfnGetSystemInfoBySession; }
PFN_DismRemoveAppx             RemoveAppx()             { return g_pfnRemoveAppx; }
PFN_DismFreeMemory             FreeMemory()             { return g_pfnFreeMemory; }

// ---- 便捷封装 --------------------------------------------------------------

void LogLevel(int nLevel, const wchar_t* pText)
{
    if (pText == nullptr) return;
    if (g_pfnWriteLog != nullptr) {
        g_pfnWriteLog(nLevel, pText);
        return;
    }
    // 宿主不可用：退化为调试输出，不静默丢弃
    ::OutputDebugStringW(pText);
    ::OutputDebugStringW(L"\n");
}

void Log(const wchar_t* pFormat, ...)
{
    if (pFormat == nullptr) return;

    std::wstring sLog;
    sLog.resize(1024);
    va_list ap;
    va_start(ap, pFormat);
    _vsnwprintf_s(&sLog[0], sLog.size(), _TRUNCATE, pFormat, ap);
    va_end(ap);
    sLog.resize(wcslen(sLog.c_str()));

    LogLevel(0, sLog.c_str());
}

long CallRemoveAppx(const wchar_t* pFullName)
{
    if (pFullName == nullptr) return E_INVALIDARG;
    if (g_pfnRemoveAppx == nullptr) {
        // 与原版模板一致：记录失败但不抛出
        Log(L"DismRemoveAppx(%s) ==> 0x%08lX", pFullName, (unsigned long)E_NOTIMPL);
        return E_NOTIMPL;
    }
    long hr = g_pfnRemoveAppx(pFullName);
    Log(L"DismRemoveAppx(%s) ==> 0x%08lX", pFullName, (unsigned long)hr);
    return hr;
}

}  // namespace NCleanerHost
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
inline constexpr int kCpp17_ncleaner_host_StepTimeoutMs = 120000;
inline constexpr int kCpp17_ncleaner_host_MaxRetry = 3;
