// ============================================================================
//  NCleanerClient.cpp —— NCleaner.dll 客户端适配层实现
//
//  还原原版主程序对 NCleaner.dll 的调用模型：
//    LoadLibrary("NCleaner.dll") → 按选中项调用对应 NC*Cleanup。
//  （知识库《08_多模块协作与交互专题》3.3：NCleaner 无工厂函数，直接导出）
// ============================================================================

#include "NCleanerClient.h"
#include "FormatW.h"
#include "SafeThread.h"   // [R-NCSEH] CrashGuardFilter（NCleaner SEH 兜底）

#include <shlwapi.h>
#include <cstdio>

#pragma comment(lib, "shlwapi.lib")

namespace NCleanerClient
{

namespace
{
    // NCleaner 导出原型：HRESULT WINAPI NC*Cleanup(void)
    // （知识库《50_QT6》§1.3：只有返回 S_OK 代表成功）
    using PFN_NCCleanup = HRESULT ( WINAPI*)();

    // 7 个导出的名称表（顺序与 ECleanup 一致，与 NCleaner.def 一致）
    const wchar_t* const kNames[] = {
        L"NCInstallerFolderCleanup",        // ECleanup::kCleanInstaller
        L"NCCorruptedAppXOnlineCleanup",    // ECleanup::kCleanCorruptedAppx
        L"NCSRPointOnlineCleanup",          // ECleanup::kCleanSRPoint
        L"NCDOCacheOnlineCleanup",          // ECleanup::kCleanDOCache
        L"NCPackageCacheFolderCleanup",     // ECleanup::kCleanPackageCache
        L"NCVSPackageCacheCleanup",         // ECleanup::kCleanVSPackage
        L"NCWinEventLogOnlineCleanup",      // ECleanup::kCleanEventLog
    };

    HMODULE       g_hDll     = nullptr;
    PFN_NCCleanup g_pfn[7]   = { nullptr };
    int           g_nResolved = 0;
    std::wstring  g_sLastErr;

    // [R-NCSEH] SEH 兜底转发：NCleaner.dll 是二进制卫星（无源码），其内部
    // COM/原生路径任何硬件异常若直接穿透，会杀死调用它的工作线程 ——
    // 清理尾巴线程一死，WM_CLEAN_DISMTAIL 永不投递，UI 永久冻结
    // （crashguard 2026-11-02 05:50:01 实证：NCPackageCache 内 COM AV 写 0x3）。
    // 本函数无 C++ 对象，允许 __try；异常降级为 E_FAIL，由调用方回退原生实现。
    struct NCInvokeCtx { PFN_NCCleanup pfn; HRESULT hr; };
    static void InvokeSeh(NCInvokeCtx& c)
    {
        __try
        {
            c.hr = c.pfn();
        }
        __except (CrashGuardFilter(GetExceptionInformation(), "ncleaner-seh"))
        {
            c.hr = E_FAIL;
        }
    }
}  // namespace

const wchar_t* NameOf(ECleanup e)
{
    int i = static_cast<int>(e);
    if (i < 0 || i >= static_cast<int>(ECleanup::kCleanCount)) return L"";
    return kNames[i];
}

bool Init(std::wstring* pErr)
{
    if (g_hDll != nullptr) return g_nResolved == static_cast<int>(ECleanup::kCleanCount);

    g_sLastErr.clear();

    // 1) 优先按绝对路径加载：架构依赖目录 amd64\ 优先，兼容主程序同目录扁平布局
    //    （绿色部署，避免 PATH 污染）
    wchar_t szExe[MAX_PATH] = { 0 };
    if (::GetModuleFileNameW(nullptr, szExe, _countof(szExe)) > 0)
    {
        ::PathRemoveFileSpecW(szExe);
        std::wstring sDir = szExe;
        if (!sDir.empty() && sDir.back() != L'\\') sDir += L'\\';
        // [MIG] Debug 卫星 DLL 名为 NCleaner_d.dll，见 Win8Metro.vcxproj 说明。
#ifdef _DEBUG
        const wchar_t* const kRel[] = { L"amd64\\NCleaner_d.dll", L"amd64\\NCleaner.dll",
                                        L"NCleaner_d.dll", L"NCleaner.dll" };
        const int kRelCount = _countof(kRel);
#else
        const wchar_t* const kRel[] = { L"amd64\\NCleaner.dll", L"NCleaner.dll" };
        const int kRelCount = _countof(kRel);
#endif
        for (int i = 0; i < kRelCount && g_hDll == nullptr; ++i)
        {
            std::wstring sLocal = sDir + kRel[i];
            if (::GetFileAttributesW(sLocal.c_str()) == INVALID_FILE_ATTRIBUTES)
                continue;
            g_hDll = ::LoadLibraryExW(sLocal.c_str(), nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        }
    }

    // 2) [BUG-02] 不做裸名 LoadLibrary 回退：裸名搜索顺序第一命中应用
    //    目录，是 DLL 植入口。绝对路径候选全部缺失即按缺失报错。
    if (g_hDll == nullptr)
    {
        DWORD dwErr = ::GetLastError();
        wchar_t szMsg[192] = { 0 };
        wfmt::FormatInto(szMsg, _countof(szMsg),
                     L"无法加载 NCleaner.dll（错误码=%lu），"
                     L"请确认该文件与主程序在同一目录", dwErr);
        g_sLastErr = szMsg;
        if (pErr) *pErr = g_sLastErr;
        return false;
    }

    // 3) 解析 7 个导出（GetProcAddress 只接受 ANSI 符号名，故用窄字符名称）
    static const char* const kNamesA[] = {
        "NCInstallerFolderCleanup",
        "NCCorruptedAppXOnlineCleanup",
        "NCSRPointOnlineCleanup",
        "NCDOCacheOnlineCleanup",
        "NCPackageCacheFolderCleanup",
        "NCVSPackageCacheCleanup",
        "NCWinEventLogOnlineCleanup",
    };

    g_nResolved = 0;
    for (int i = 0; i < static_cast<int>(ECleanup::kCleanCount); ++i)
    {
        g_pfn[i] = reinterpret_cast<PFN_NCCleanup>(
            ::GetProcAddress(g_hDll, kNamesA[i]));
        if (g_pfn[i] != nullptr) ++g_nResolved;
    }

    if (g_nResolved != static_cast<int>(ECleanup::kCleanCount))
    {
        wchar_t szMsg[192] = { 0 };
        wfmt::FormatInto(szMsg, _countof(szMsg),
                     L"NCleaner.dll 导出不完整（7 个官方导出中仅解析到 %d 个）",
                     g_nResolved);
        g_sLastErr = szMsg;
        if (pErr) *pErr = g_sLastErr;
        return false;
    }

    return true;
}

void Shutdown()
{
    // [EXIT-FIX] 进程退出阶段不再 FreeLibrary：detached 清理工作线程可能
    // 正在 NCleaner.dll 内执行，卸载会造成在飞代码释放/加载器锁死锁，
    // 导致退出僵尸化。模块与函数指针保持原样，由进程终结统一回收。
}


bool IsLoaded()  { return g_hDll != nullptr; }
bool IsReady()   { return g_hDll != nullptr && g_nResolved == static_cast<int>(ECleanup::kCleanCount); }

const std::wstring& LastError() { return g_sLastErr; }

long Invoke(ECleanup e, std::wstring* pErr)
{
    int i = static_cast<int>(e);
    if (i < 0 || i >= static_cast<int>(ECleanup::kCleanCount)) return E_INVALIDARG;

    if (!IsReady())
    {
        if (pErr) *pErr = L"NCleaner.dll 未就绪";
        return E_NOTIMPL;
    }

    // [R-NCSEH] 经 SEH 兜底转发调用；异常 → E_FAIL → 调用方回退原生实现
    NCInvokeCtx c{ g_pfn[i], E_FAIL };
    InvokeSeh(c);
    HRESULT hr = c.hr;
    if (FAILED(hr) && pErr != nullptr)
    {
        constexpr int kErrMsgWchars = 160;   // 160 缓冲尺寸（语义命名，数值不变）
        wchar_t szMsg[kErrMsgWchars] = { 0 };
        wfmt::FormatInto(szMsg, _countof(szMsg),
                     L"%s 返回 0x%08lX", NameOf(e), (unsigned long)hr);
        *pErr = szMsg;
    }
    return hr;
}

}  // namespace NCleanerClient
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
