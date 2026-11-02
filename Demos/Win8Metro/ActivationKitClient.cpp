// ============================================================================
//  ActivationKitClient.cpp —— ActivationKit.dll 客户端适配层实现
//
//  装载策略与 NCleanerClient 完全一致：
//    1) amd64\ 子目录绝对路径优先（绿色部署、防 PATH 污染与劫持）；
//    2) 主程序同目录扁平布局回退；
//    3) LOAD_LIBRARY_SEARCH_* 安全标志；
//    4) 3 导出齐全才 Ready，缺失不阻断主程序启动。
// ============================================================================

#include "ActivationKitClient.h"

#include <shlwapi.h>
#include <cstdio>

#pragma comment(lib, "shlwapi.lib")

namespace ActivationKitClient
{

namespace
{
    // ---- 与 Demos\ActivationKit\ActivationKit.h 保持二进制一致的函数指针 ----
    using PFN_Backup = HRESULT ( __cdecl*)(const wchar_t*, const wchar_t*, wchar_t*, DWORD);
    using PFN_Restore = HRESULT ( __cdecl*)(const wchar_t*, wchar_t*, DWORD);
    using PFN_Exists = BOOL ( __cdecl*)(const wchar_t*);

    HMODULE      g_hDll       = nullptr;
    PFN_Backup   g_pfnBackup  = nullptr;
    PFN_Restore  g_pfnRestore = nullptr;
    PFN_Exists   g_pfnExists  = nullptr;
    int          g_nResolved  = 0;
    std::wstring g_sLastErr;
}  // namespace

bool Init(std::wstring* pErr)
{
    if (g_hDll != nullptr) return g_nResolved == 3;

    g_sLastErr.clear();

    // 1) 绝对路径装载：amd64\ 优先，exe 同目录回退
    wchar_t szExe[MAX_PATH] = { 0 };
    if (::GetModuleFileNameW(nullptr, szExe, _countof(szExe)) > 0)
    {
        ::PathRemoveFileSpecW(szExe);
        std::wstring sDir = szExe;
        if (!sDir.empty() && sDir.back() != L'\\') sDir += L'\\';
        // [MIG] Debug 构建的卫星 DLL 名为 *_d.dll（避免覆盖 Release 产物），
        // 因此按当前配置优先查找对应命名，另一命名作为兼容回退。
#ifdef _DEBUG
        const wchar_t* const kRel[] = { L"amd64\\ActivationKit_d.dll",
                                        L"amd64\\ActivationKit.dll",
                                        L"ActivationKit_d.dll",
                                        L"ActivationKit.dll" };
        const int kRelCount = _countof(kRel);
#else
        const wchar_t* const kRel[] = { L"amd64\\ActivationKit.dll",
                                        L"ActivationKit.dll" };
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
        std::wstring sMsg = L"无法加载 ActivationKit.dll（错误码=" + std::to_wstring(dwErr) + L"），激活备份功能不可用";
        g_sLastErr = sMsg;
        if (pErr) *pErr = g_sLastErr;
        return false;
    }

    // 3) 解析 3 个导出（缺一即不就绪，但不卸载——允许热修复后重新初始化场景）
    g_pfnBackup  = reinterpret_cast<PFN_Backup>(
        ::GetProcAddress(g_hDll, "AK_BackupActivation"));
    g_pfnRestore = reinterpret_cast<PFN_Restore>(
        ::GetProcAddress(g_hDll, "AK_RestoreActivation"));
    g_pfnExists  = reinterpret_cast<PFN_Exists>(
        ::GetProcAddress(g_hDll, "AK_ActivationBackupExists"));

    g_nResolved = (g_pfnBackup ? 1 : 0) + (g_pfnRestore ? 1 : 0)
                + (g_pfnExists ? 1 : 0);

    if (g_nResolved != 3)
    {
        std::wstring sMsg = L"ActivationKit.dll 导出不完整（3 个导出中仅解析到 "
            + std::to_wstring(g_nResolved) + L" 个）";
        g_sLastErr = sMsg;
        if (pErr) *pErr = g_sLastErr;
        return false;
    }

    return true;
}

void Shutdown()
{
    // 与其他卫星客户端一致：退出阶段不强制 FreeLibrary（组件内可能仍有
    // 在飞调用），由进程终结统一回收。函数指针仅置空防退出期误用。
    g_pfnBackup = nullptr;
    g_pfnRestore = nullptr;
    g_pfnExists = nullptr;
    g_nResolved = 0;
}

bool IsReady()
{
    return g_hDll != nullptr && g_nResolved == 3;
}

const std::wstring& LastError()
{
    return g_sLastErr;
}

long Backup(const std::wstring& sOutDir, const std::wstring& sNote,
            std::wstring* pErr)
{
    if (!IsReady())
    {
        if (pErr) *pErr = L"ActivationKit.dll 未就绪，激活备份功能不可用。";
        return E_NOTIMPL;
    }

    constexpr int kErrMsgWchars = 512;   // 512 缓冲尺寸（语义命名，数值不变）
    wchar_t szErr[kErrMsgWchars] = { 0 };
    HRESULT hr = g_pfnBackup(sOutDir.c_str(),
                             sNote.empty() ? nullptr : sNote.c_str(),
                             szErr, _countof(szErr));
    if (FAILED(hr) && pErr)
        *pErr = szErr[0] ? szErr : L"激活信息备份失败。";
    return hr;
}

long Restore(const std::wstring& sBaseDir, std::wstring* pErr)
{
    if (!IsReady())
    {
        if (pErr) *pErr = L"ActivationKit.dll 未就绪，激活还原功能不可用。";
        return E_NOTIMPL;
    }

    constexpr int kErrMsgWchars = 512;   // 512 缓冲尺寸（语义命名，数值不变）
    wchar_t szErr[kErrMsgWchars] = { 0 };
    HRESULT hr = g_pfnRestore(sBaseDir.c_str(), szErr, _countof(szErr));
    if (FAILED(hr) && pErr)
        *pErr = szErr[0] ? szErr : L"激活信息还原失败。";
    return hr;
}

bool BackupExists(const std::wstring& sBaseDir)
{
    if (!IsReady()) return false;
    return g_pfnExists(sBaseDir.c_str()) != FALSE;
}

}  // namespace ActivationKitClient
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
