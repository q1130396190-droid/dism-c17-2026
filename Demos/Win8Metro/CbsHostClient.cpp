// ============================================================================
//  CbsHostClient.cpp
// ============================================================================

#include "CbsHostClient.h"
#include "FormatW.h"
#include "CBSHostApi.h"

#include <shlwapi.h>
#include <cstdarg>

#pragma comment(lib, "shlwapi.lib")

namespace CbsHostClient
{

namespace
{

// ---- 6 个导出的函数指针类型与全局表 ----

using PFN_CbsCreateTempDirectory2 = BOOL ( WINAPI*)(wchar_t*, DWORD, DWORD*);
using PFN_CreateCbsHostHelper = void* ( WINAPI*)(void*, unsigned char);
using PFN_DismGetScratchDir = BOOL ( WINAPI*)(wchar_t*, DWORD);
using PFN_DismWriteLog = void ( WINAPI*)(int, const wchar_t*, ...);
using PFN_GetConfig = BOOL ( WINAPI*)(const wchar_t*, CBS_CONFIG*);
using PFN_RunCbsHostW = int ( WINAPI*)(const wchar_t*, wchar_t*, DWORD);

struct ApiTable
{
    HMODULE hDll = nullptr;
    PFN_CbsCreateTempDirectory2 pfnCreateTempDirectory2 = nullptr;
    PFN_CreateCbsHostHelper     pfnCreateHelper = nullptr;
    PFN_DismGetScratchDir       pfnGetScratchDir = nullptr;
    PFN_DismWriteLog            pfnWriteLog = nullptr;
    PFN_GetConfig               pfnGetConfig = nullptr;
    PFN_RunCbsHostW             pfnRunCbsHostW = nullptr;
};

ApiTable g_api;

void SetErr(std::wstring* pErr, const wchar_t* pszMsg)
{
    if (pErr) *pErr = pszMsg ? pszMsg : L"";
}

} // namespace

// ============================================================================
// 生命周期
// ============================================================================

bool Init(std::wstring* pErr)
{
    if (g_api.hDll)
        return true;   // 已加载

    // 1) 优先按绝对路径加载：架构依赖目录 amd64\ 优先，兼容主程序同目录扁平布局
    //    （绿色部署，避免 PATH 污染）
    wchar_t szExe[MAX_PATH] = {};
    if (::GetModuleFileNameW(nullptr, szExe, _countof(szExe)) > 0)
    {
        ::PathRemoveFileSpecW(szExe);
        std::wstring sDir = szExe;
        if (!sDir.empty() && sDir.back() != L'\\') sDir += L'\\';
        // [MIG] Debug 卫星 DLL 名为 CBSHost_d.dll，见 Win8Metro.vcxproj 说明。
#ifdef _DEBUG
        const wchar_t* const kRel[] = { L"amd64\\CBSHost_d.dll", L"amd64\\CBSHost.dll",
                                        L"CBSHost_d.dll", L"CBSHost.dll" };
        const int kRelCount = _countof(kRel);
#else
        const wchar_t* const kRel[] = { L"amd64\\CBSHost.dll", L"CBSHost.dll" };
        const int kRelCount = _countof(kRel);
#endif
        for (int i = 0; i < kRelCount && !g_api.hDll; ++i)
        {
            std::wstring sLocal = sDir + kRel[i];
            if (::GetFileAttributesW(sLocal.c_str()) == INVALID_FILE_ATTRIBUTES)
                continue;
            g_api.hDll = ::LoadLibraryExW(sLocal.c_str(), nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        }
    }

    // 2) [BUG-02] 不做裸名 LoadLibrary 回退：裸名搜索顺序第一命中应用
    //    目录，是 DLL 植入口。绝对路径候选全部缺失即按缺失报错。
    if (!g_api.hDll)
    {
        DWORD dwErr = ::GetLastError();
        std::wstring sMsg;
        sMsg.resize(160);
        wfmt::FormatInto(sMsg.data(), sMsg.size() + 1,
                     L"无法加载 CBSHost.dll（错误码=%lu），请确认该文件与主程序在同一目录",
                     dwErr);
        sMsg.resize(wcslen(sMsg.c_str()));
        SetErr(pErr, sMsg.c_str());
        return false;
    }

    // 3) 解析全部 6 个导出；任一缺失即视为 DLL 不完整，整体卸载
    g_api.pfnCreateTempDirectory2 =
        (PFN_CbsCreateTempDirectory2)::GetProcAddress(g_api.hDll, "CbsCreateTempDirectory2");
    g_api.pfnCreateHelper =
        (PFN_CreateCbsHostHelper)::GetProcAddress(g_api.hDll, "CreateCbsHostHelper");
    g_api.pfnGetScratchDir =
        (PFN_DismGetScratchDir)::GetProcAddress(g_api.hDll, "DismGetScratchDir");
    g_api.pfnWriteLog =
        (PFN_DismWriteLog)::GetProcAddress(g_api.hDll, "DismWriteLog");
    g_api.pfnGetConfig =
        (PFN_GetConfig)::GetProcAddress(g_api.hDll, "GetConfig");
    g_api.pfnRunCbsHostW =
        (PFN_RunCbsHostW)::GetProcAddress(g_api.hDll, "RunCbsHostW");

    if (!g_api.pfnCreateTempDirectory2 || !g_api.pfnCreateHelper ||
        !g_api.pfnGetScratchDir || !g_api.pfnWriteLog ||
        !g_api.pfnGetConfig || !g_api.pfnRunCbsHostW)
    {
        ::FreeLibrary(g_api.hDll);
        g_api = ApiTable();
        SetErr(pErr, L"CBSHost.dll 导出不完整（6 个官方导出有缺失），请核对文件版本");
        return false;
    }

    return true;
}

void Shutdown()
{
    // [EXIT-FIX] 进程退出阶段不再 FreeLibrary：
    // 可能仍有 detached 后台工作线程正在 CBSHost.dll 内执行（CBS ALPC
    // 同步调用），此时卸载 DLL 会造成在飞代码页释放/加载器锁 DllMain
    // DETACH 死锁，是退出僵尸化的根因之一。DLL 与函数指针表保持原样，
    // 由进程终结时操作系统统一回收（WinMain 末尾有 TerminateProcess 兜底）。
    // 初始化失败路径的 FreeLibrary 仍保留（那时不存在在飞线程）。
}

bool IsReady()
{
    return (g_api.hDll != nullptr && g_api.pfnRunCbsHostW != nullptr);
}

// ============================================================================
// 业务接口
// ============================================================================

int DispatchCommand(ECmd cmd, const std::wstring& sTarget,
                    const std::wstring& sParam, DWORD dwFlags,
                    std::wstring* pErr)
{
    if (!IsReady())
    {
        SetErr(pErr, L"CBSHost.dll 未就绪（未加载或加载失败）");
        return -1;
    }

    // 通过官方导出 RunCbsHostW 以数字 ID 派发：
    //   "<id> \"<target>\" \"<param>\" <flags>"
    // target/param 即使为空也保留引号占位，保证 argc=4。
    std::wstring sCmdLine;
    std::wstring sNum;
    sNum.resize(32);

    wfmt::FormatInto(sNum.data(), sNum.size() + 1, L"%u ", static_cast<unsigned>(cmd));   // [C++17 R20] scoped enum -> explicit int conversion
    sNum.resize(wcslen(sNum.c_str()));
    sCmdLine = sNum;

    sCmdLine += L"\"";
    sCmdLine += sTarget;
    sCmdLine += L"\" \"";
    sCmdLine += sParam;
    sCmdLine += L"\" ";

    wfmt::FormatInto(sNum.data(), sNum.size() + 1, L"%lu", dwFlags);
    sNum.resize(wcslen(sNum.c_str()));
    sCmdLine += sNum;

    constexpr int kErrMsgWchars = 512;   // 512 缓冲尺寸（语义命名，数值不变）
    wchar_t szErr[kErrMsgWchars] = {};
    int nRet = g_api.pfnRunCbsHostW(sCmdLine.c_str(), szErr, _countof(szErr));
    if (pErr) *pErr = szErr;
    return nRet;
}

bool GetScratchDir(std::wstring* pOut)
{
    if (!IsReady()) return false;

    wchar_t szDir[MAX_PATH + 8] = {};
    if (!g_api.pfnGetScratchDir(szDir, _countof(szDir)))
        return false;

    if (pOut) *pOut = szDir;
    return true;
}

void WriteLog(int nLevel, const wchar_t* pszFormat, ...)
{
    if (!IsReady() || !pszFormat) return;

    // 变参无法直接跨边界转发，先在本侧格式化为完整消息，再以 %s 传入。
    std::wstring sMsg;
    sMsg.resize(1024);
    va_list ap;
    va_start(ap, pszFormat);
    _vsnwprintf_s(sMsg.data(), sMsg.size() + 1, _TRUNCATE, pszFormat, ap);
    va_end(ap);
    sMsg.resize(wcslen(sMsg.c_str()));

    g_api.pfnWriteLog(nLevel, L"%s", sMsg.c_str());
}

bool CreateTempDirectory(std::wstring* pOut, DWORD* pdwErr)
{
    if (!IsReady())
    {
        if (pdwErr) *pdwErr = ERROR_DLL_NOT_FOUND;
        return false;
    }

    wchar_t szDir[MAX_PATH + 8] = {};
    DWORD dwErr = 0;
    if (!g_api.pfnCreateTempDirectory2(szDir, _countof(szDir), &dwErr))
    {
        if (pdwErr) *pdwErr = dwErr;
        return false;
    }

    if (pOut) *pOut = szDir;
    return true;
}

bool ReadConfig(const std::wstring& sIni, Config* pCfg, std::wstring* pErr)
{
    if (!IsReady())
    {
        SetErr(pErr, L"CBSHost.dll 未就绪");
        return false;
    }
    if (!pCfg)
    {
        SetErr(pErr, L"ReadConfig: null output");
        return false;
    }

    CBS_CONFIG raw;
    raw = {};
    if (!g_api.pfnGetConfig(sIni.empty() ? nullptr : sIni.c_str(), &raw))
    {
        SetErr(pErr, L"GetConfig 失败");
        return false;
    }

    pCfg->sExeDir     = raw.sExeDir;
    pCfg->sScratchDir = raw.sScratchDir;
    pCfg->sLogDir     = raw.sLogDir;
    pCfg->sMountRoot  = raw.sMountRoot;
    pCfg->nLanguage   = raw.nLanguage;
    pCfg->bNoviceMode = (raw.bNoviceMode != FALSE);
    pCfg->bExpertMode = (raw.bExpertMode != FALSE);
    return true;
}

} // namespace CbsHostClient
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
