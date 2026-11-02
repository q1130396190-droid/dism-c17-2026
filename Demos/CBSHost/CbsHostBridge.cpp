// ============================================================================
//  CbsHostBridge.cpp —— CBSHost.dll 6 导出背后的进程内原生语义实现
//
//  所有 CBS 组件栈操作经 DismApiBridge 直连 dismapi.dll（官方公开 API），
//  不 fork dism.exe，不依赖工作进程。离线深链路能力见 OfflineCbsService。
// ============================================================================

#include "cbshost_stdafx.h"

#include "CbsHostBridge.h"
#include "CBSHostApi.h"          // CBS_CMD_* 数字 ID
#include "DismApiBridge.h"
#include "../Win8Metro/FormatW.h"   // [C++17] wfmt::FormatInto for F26
#include "../Win8Metro/WinHandles.h"   // [C++17 R3] win::HandlePtr RAII for F11_raw_handle_close

#include <shellapi.h>           // CommandLineToArgvW
#include <objbase.h>            // CoCreateGuid / CoInitializeEx
#include <shlwapi.h>
#include <cstdio>
#include <cstdarg>
#include <mutex>
#include <new>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")

// dllmain.cpp 保存的本 DLL 模块句柄
extern HMODULE g_hCbsHostModule;

namespace CbsHost
{

// [G4 修复] 文件内设施子域：会话状态/目录服务/配置/日志/初始化集中于此具名
// namespace，命令派发位于 cbs_dispatch；不再靠一个匿名命名空间混杂全部职责。
namespace cbs_facility
{

// ---- 进程内会话状态 ----
// [C-07 修复] 散放的跨调用可变全局收进单一状态结构，所有读写经同一临界
// 区串行化（导出可能被多个工作线程并发调用）；[BUG-17] bDismInited 只在
// 初始化成功后置位。
struct CbsSessionState
{
    std::wstring sScratchDir;     // 当前暂存目录（空 = 惰性默认）
    std::wstring sLogDir;         // 日志目录（空 = <模块目录>\Log）
    bool         bDismInited = false;
    std::mutex   mtx;
};

CbsSessionState& State()
{
    static CbsSessionState s_state;
    return s_state;
}

// [BUG-18] 日志文件写入串行化（多线程输出不再交错）
std::mutex& LogFileMutex()
{
    static std::mutex s_mtxLog;
    return s_mtxLog;
}

// 取本 DLL 所在目录（无尾部分隔符）。
std::wstring ModuleDir()
{
    wchar_t szPath[MAX_PATH] = {};
    DWORD cch = ::GetModuleFileNameW(g_hCbsHostModule, szPath, _countof(szPath));
    if (cch == 0 || cch >= _countof(szPath))
    {
        // 极端情况下回退到主程序目录
        cch = ::GetModuleFileNameW(nullptr, szPath, _countof(szPath));
        if (cch == 0 || cch >= _countof(szPath)) return L".";
    }
    std::wstring sDir = szPath;
    size_t pos = sDir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) sDir.resize(pos);
    return sDir;
}

// 确保目录存在（含多级）。
bool EnsureDir(const std::wstring& sPath)
{
    if (sPath.empty()) return false;
    DWORD attr = ::GetFileAttributesW(sPath.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES)
        return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return ::CreateDirectoryW(sPath.c_str(), nullptr) != FALSE ||
           ::GetLastError() == ERROR_ALREADY_EXISTS;
}

// 调用方必须已持有 State().mtx
std::wstring LogDirLocked()
{
    if (!State().sLogDir.empty()) return State().sLogDir;
    std::wstring sDir = ModuleDir();
    sDir += L"\\Log";
    EnsureDir(sDir);
    return sDir;
}

std::wstring LogDir()
{
    std::lock_guard<std::mutex> lk(State().mtx);
    return LogDirLocked();
}

std::wstring DefaultScratchDir()
{
    wchar_t szTemp[MAX_PATH] = {};
    DWORD cch = ::GetTempPathW(_countof(szTemp), szTemp);
    std::wstring sBase;
    if (cch > 0 && cch < _countof(szTemp))
    {
        sBase = szTemp;
        if (!sBase.empty() && sBase.back() != L'\\') sBase += L'\\';
    }
    else
    {
        sBase = ModuleDir() + L"\\";
    }
    sBase += L"Dism++Host";
    EnsureDir(sBase);
    return sBase;
}

// 生成 GUID 字符串（无花括号）。
std::wstring MakeGuidName(const wchar_t* pszPrefix)
{
    GUID guid = {};
    wchar_t szGuid[64] = {};
    if (SUCCEEDED(::CoCreateGuid(&guid)) &&
        ::StringFromGUID2(guid, szGuid, _countof(szGuid)) > 0)
    {
        // 去掉花括号
        std::wstring s = szGuid;
        if (!s.empty() && s.front() == L'{') s.erase(s.begin());
        if (!s.empty() && s.back() == L'}')  s.pop_back();
        std::wstring sName = pszPrefix ? pszPrefix : L"";
        sName += s;
        return sName;
    }
    std::wstring sFallback;
    sFallback.resize(64);
    wfmt::FormatInto(&sFallback[0], sFallback.size(),
                 L"%s%08x%08x", pszPrefix ? pszPrefix : L"",
                 (unsigned)::GetCurrentProcessId(),
                 (unsigned)::GetTickCount64());
    sFallback.resize(wcslen(sFallback.c_str()));
    return sFallback;
}

// 判断 s 是否以指定扩展名（如 L".cab"）结尾，不区分大小写。
bool EndsWithExt(const std::wstring& s, const wchar_t* pszExt)
{
    size_t dot = s.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    return ::lstrcmpiW(s.c_str() + dot, pszExt) == 0;
}

// 惰性初始化 DismApiBridge（幂等）。
bool EnsureDism(std::wstring* psErr)
{
    std::lock_guard<std::mutex> lk(State().mtx);
    if (State().bDismInited) return DismApiBridge::IsReady();

    std::wstring sLog = LogDirLocked() + L"\\DismHost.log";
    // [BUG-17] 成功后才置位：原实现在 Init 之前置 true，并发首调的第二线程
    // 看到 true 但 IsReady()=false，会误报初始化失败。
    if (!DismApiBridge::Init(sLog.c_str()))
    {
        if (psErr) *psErr = L"dismapi.dll 初始化失败（系统缺少 DISM API 或核心导出不可用）";
        return false;
    }
    State().bDismInited = true;
    return true;
}

} // namespace cbs_facility

using namespace cbs_facility;

// ============================================================================
// 1) CbsCreateTempDirectory
// ============================================================================

bool CbsCreateTempDirectory(std::wstring* psOut, DWORD* pdwErr)
{
    if (pdwErr) *pdwErr = 0;

    std::wstring sBase = DefaultScratchDir();   // 已确保存在
    std::wstring sDir;
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        sDir = sBase + L"\\" + MakeGuidName(L"CBS_");
        if (::CreateDirectoryW(sDir.c_str(), nullptr))
        {
            {
                std::lock_guard<std::mutex> lk(State().mtx);  // [C-07]
                State().sScratchDir = sDir;
            }
            if (psOut) *psOut = sDir;
            return true;
        }
        DWORD dwErr = ::GetLastError();
        if (dwErr != ERROR_ALREADY_EXISTS)
        {
            if (pdwErr) *pdwErr = dwErr;
            return false;
        }
    }

    if (pdwErr) *pdwErr = ERROR_ALREADY_EXISTS;
    return false;
}

// ============================================================================
// 2) CbsHostCreateHelper
// ============================================================================

namespace cbs_helper   // [G4] 工厂上下文子域
{
struct HelperCtx
{
    Config sCfg;
    void*  pOuter;
    int    nMode;
};
}

void* CbsHostCreateHelper(const Config* pCfg, void* pOuter, int nMode)
{
    // [C-08 修复] 原实现每次 new 一个 HelperCtx，且全 DLL 没有对应释放导出
    // （调用一次即确定性泄漏）。改为进程内静态单例：工厂语义保留（返回稳定
    // 不透明指针），无配对释放要求。
    static cbs_helper::HelperCtx s_ctx;
    std::lock_guard<std::mutex> lk(State().mtx);
    if (pCfg) s_ctx.sCfg = *pCfg;
    s_ctx.pOuter = pOuter;
    s_ctx.nMode  = nMode;
    return &s_ctx;
}

// ============================================================================
// 3) DismGetScratchDir
// ============================================================================

bool DismGetScratchDir(std::wstring* psOut)
{
    std::lock_guard<std::mutex> lk(State().mtx);   // [C-07]
    if (State().sScratchDir.empty())
        State().sScratchDir = DefaultScratchDir();
    if (!EnsureDir(State().sScratchDir)) return false;
    if (psOut) *psOut = State().sScratchDir;
    return true;
}

// ============================================================================
// 4) DismWriteLogV
// ============================================================================

void DismWriteLogV(int nLevel, const wchar_t* pszFormat, va_list ap)
{
    if (!pszFormat) return;

    std::wstring sMsg;
    sMsg.resize(1024);
    ::_vsnwprintf_s(&sMsg[0], sMsg.size(), _TRUNCATE, pszFormat, ap);
    sMsg.resize(wcslen(sMsg.c_str()));

    static const wchar_t* const kLevelTag[5] =
        { L"ERROR", L"WARN", L"INFO", L"VERBOSE", L"DEBUG" };
    const wchar_t* pszTag =
        (nLevel >= 0 && nLevel <= 4) ? kLevelTag[nLevel] : L"LOG";

    SYSTEMTIME st = {};
    ::GetLocalTime(&st);
    std::wstring sLine;
    sLine.resize(1200);
    wfmt::FormatInto(&sLine[0], sLine.size(),
                 L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] [%s] %s\r\n",
                 st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                 pszTag, sMsg.c_str());
    sLine.resize(wcslen(sLine.c_str()));

    std::wstring sLog = LogDir() + L"\\CbsHost.log";
    // [BUG-18] 进程内互斥：多工作线程并发写日志不再交错；新建文件写
    // UTF-16LE BOM（原实现无 BOM，外部按 ANSI/UTF-8 打开的工具会乱码）。
    std::lock_guard<std::mutex> lkLog(LogFileMutex());
    // [C++17 R3] F11: RAII handle, auto CloseHandle on every exit path
    win::HandlePtr h(win::file_handle(::CreateFileW(sLog.c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr)));
    if (!h) return;

    DWORD cbWritten = 0;
    if (::GetFileSize(h.get(), nullptr) == 0)
    {
        constexpr WORD wBom = 0xFEFF;
        ::WriteFile(h.get(), &wBom, sizeof(wBom), &cbWritten, nullptr);
    }

    DWORD cbBytes = static_cast<DWORD>(::wcslen(sLine.c_str()) * sizeof(wchar_t));
    ::WriteFile(h.get(), sLine.c_str(), cbBytes, &cbWritten, nullptr);
}

// ============================================================================
// 5) GetConfigFromIni
// ============================================================================

bool GetConfigFromIni(const std::wstring& sIni, Config* pCfg,
                      std::wstring* psErr)
{
    if (!pCfg)
    {
        if (psErr) *psErr = L"GetConfig: 输出参数为空";
        ::SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }

    Config cfg;
    cfg.sExeDir     = ModuleDir();
    cfg.sScratchDir = DefaultScratchDir();
    cfg.sLogDir     = LogDir();
    cfg.sMountRoot.clear();
    cfg.nLanguage   = 0;
    cfg.bNoviceMode = true;
    cfg.bExpertMode = false;

    // 解析 ini 路径：显式 > 模块\Config\Config.ini > 模块\Config.ini
    std::wstring sPath = sIni;
    if (sPath.empty())
    {
        std::wstring sA = cfg.sExeDir + L"\\Config\\Config.ini";
        std::wstring sB = cfg.sExeDir + L"\\Config.ini";
        if (::GetFileAttributesW(sA.c_str()) != INVALID_FILE_ATTRIBUTES)
            sPath = sA;
        else
            sPath = sB;   // 不存在也无妨：读取时回填默认值
    }

    const wchar_t* kSec = L"CBSHost";
    wchar_t szBuf[MAX_PATH] = {};

    ::GetPrivateProfileStringW(kSec, L"ExeDir", cfg.sExeDir.c_str(),
                               szBuf, _countof(szBuf), sPath.c_str());
    cfg.sExeDir = szBuf;

    ::GetPrivateProfileStringW(kSec, L"ScratchDir", cfg.sScratchDir.c_str(),
                               szBuf, _countof(szBuf), sPath.c_str());
    cfg.sScratchDir = szBuf;

    ::GetPrivateProfileStringW(kSec, L"LogDir", cfg.sLogDir.c_str(),
                               szBuf, _countof(szBuf), sPath.c_str());
    cfg.sLogDir = szBuf;

    ::GetPrivateProfileStringW(kSec, L"MountRoot", L"",
                               szBuf, _countof(szBuf), sPath.c_str());
    cfg.sMountRoot = szBuf;

    cfg.nLanguage = ::GetPrivateProfileIntW(kSec, L"Language", 0, sPath.c_str());
    cfg.bNoviceMode = ::GetPrivateProfileIntW(kSec, L"NoviceMode", 1, sPath.c_str()) != 0;
    cfg.bExpertMode = ::GetPrivateProfileIntW(kSec, L"ExpertMode", 0, sPath.c_str()) != 0;

    {
        std::lock_guard<std::mutex> lk(State().mtx);   // [C-07]
        State().sLogDir = cfg.sLogDir;
    }

    *pCfg = cfg;
    return true;
}

// ============================================================================
// 6) RunCbsHostW —— 数字 ID 命令派发到原生 dismapi
//
// [G4/N5 修复] 派发改为描述符表（id → 处理函数），新增命令只动表与处理
// 函数，不再增长 switch；原 goto 清理路径随 RAII 式早返回消失，最大嵌套
// 降到 2。
// ============================================================================

namespace cbs_dispatch
{

// 单个命令处理函数：flags=argv[3]，sTarget=映像根（可空在线），sParam=参数。
using PfnDispatch = bool (*)(unsigned long flags,
                            const std::wstring& sTarget,
                            const std::wstring& sParam,
                            DWORD* pdwStatus, std::wstring* psErr);

const wchar_t* ImageRoot(const std::wstring& sTarget)
{
    return sTarget.empty() ? nullptr : sTarget.c_str();
}

bool CmdStart(unsigned long, const std::wstring&, const std::wstring&,
              DWORD*, std::wstring* psErr)
{
    // 会话打开由各操作内部 OpenSession 完成；此处仅确认 API 就绪。
    if (DismApiBridge::IsReady()) return true;
    if (psErr) *psErr = L"DISM API 未就绪";
    return false;
}

bool CmdCleanup(unsigned long flags, const std::wstring& sTarget,
                const std::wstring&, DWORD*, std::wstring* psErr)
{
    // bit0=ResetBase；在线/离线均由 target 是否为空决定。
    return DismApiBridge::CleanComponentStore(
               (flags & 0x1UL) != 0, psErr, ImageRoot(sTarget));
}

bool CmdInstall(unsigned long flags, const std::wstring& sTarget,
                const std::wstring& sParam, DWORD*, std::wstring* psErr)
{
    // .cab/.msu → AddPackage；含 "~~~" → AddCapability；否则 EnableFeature
    if (EndsWithExt(sParam, L".cab") || EndsWithExt(sParam, L".msu"))
        return DismApiBridge::AddPackage(sParam, psErr, ImageRoot(sTarget));
    if (sParam.find(L"~~~") != std::wstring::npos)
        return DismApiBridge::AddCapability(
                   sParam, L"", (flags & 0x2UL) != 0, psErr, ImageRoot(sTarget));
    return DismApiBridge::EnableFeature(
               sParam, L"", (flags & 0x2UL) != 0, psErr, ImageRoot(sTarget));
}

bool CmdCheckHealth(unsigned long, const std::wstring& sTarget,
                    const std::wstring&, DWORD* pdwStatus, std::wstring* psErr)
{
    // [BUG-09] target=映像根时透传
    return DismApiBridge::CheckImageHealth(
               false, pdwStatus, psErr, ImageRoot(sTarget));
}

bool CmdScanHealth(unsigned long, const std::wstring& sTarget,
                   const std::wstring&, DWORD* pdwStatus, std::wstring* psErr)
{
    return DismApiBridge::CheckImageHealth(
               true, pdwStatus, psErr, ImageRoot(sTarget));
}

bool CmdRestoreHealth(unsigned long flags, const std::wstring& sTarget,
                      const std::wstring& sParam, DWORD*, std::wstring* psErr)
{
    // param=修复源（可空），bit1=LimitAccess，target=映像根
    return DismApiBridge::RestoreImageHealth(
               sParam, (flags & 0x2UL) != 0, psErr, ImageRoot(sTarget));
}

bool CmdQueryPackages(unsigned long, const std::wstring& sTarget,
                      const std::wstring&, DWORD*, std::wstring* psErr)
{
    std::vector<DismApiBridge::PkgInfo> vPkgs;
    return DismApiBridge::GetPackages(vPkgs, psErr, ImageRoot(sTarget));
}

struct CmdEntry
{
    unsigned long id;
    PfnDispatch   pfn;
};

// 覆盖全部 CBS_CMD_*；ID 为官方固定数字，顺序不影响查找。
const CmdEntry kCmdTable[] =
{
    { CBS_CMD_START,         &CmdStart },
    { CBS_CMD_CLEANUP,       &CmdCleanup },
    { CBS_CMD_CLEANUP_IMAGE, &CmdCleanup },
    { CBS_CMD_INSTALL,       &CmdInstall },
    { CBS_CMD_CHECK_HEALTH,  &CmdCheckHealth },
    { CBS_CMD_SCAN_HEALTH,   &CmdScanHealth },
    { CBS_CMD_RESTORE_HEALTH,&CmdRestoreHealth },
    { CBS_CMD_QUERY_PACKAGES,&CmdQueryPackages },
};

const PfnDispatch Lookup(unsigned long id)
{
    for (size_t i = 0; i < _countof(kCmdTable); ++i)
        if (kCmdTable[i].id == id) return kCmdTable[i].pfn;
    return nullptr;
}

} // namespace cbs_dispatch

int RunCbsHostW(const std::wstring& sCmdLine, std::wstring* psErr)
{
    if (psErr) psErr->clear();

    // DISM API 需要 COM；按线程幂等初始化（S_OK/S_FALSE 都配对 CoUninitialize）。
    HRESULT hrCo = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool bCoMine = SUCCEEDED(hrCo);

    int nExit = 0;

    int argc = 0;
    wchar_t** argv = ::CommandLineToArgvW(sCmdLine.c_str(), &argc);
    if (!argv || argc < 1)
    {
        if (psErr) *psErr = L"RunCbsHostW: 命令行为空";
        if (argv) ::LocalFree(argv);
        if (bCoMine) ::CoUninitialize();
        return ERROR_INVALID_PARAMETER;
    }

    unsigned long idCmd  = wcstoul(argv[0], nullptr, 0);
    std::wstring sTarget = (argc >= 2) ? argv[1] : L"";
    std::wstring sParam  = (argc >= 3) ? argv[2] : L"";
    unsigned long flags  = (argc >= 4) ? wcstoul(argv[3], nullptr, 0) : 0;
    ::LocalFree(argv);

    if (!EnsureDism(psErr))
    {
        if (bCoMine) ::CoUninitialize();
        return 1;
    }

    cbs_dispatch::PfnDispatch pfn = cbs_dispatch::Lookup(idCmd);
    if (pfn == nullptr)
    {
        if (psErr)
        {
            std::wstring sErrMsg;
            sErrMsg.resize(96);
            wfmt::FormatInto(&sErrMsg[0], sErrMsg.size(),
                         L"RunCbsHostW: 未知命令 ID=0x%lx", idCmd);
            sErrMsg.resize(wcslen(sErrMsg.c_str()));
            *psErr = sErrMsg;
        }
        if (bCoMine) ::CoUninitialize();
        return ERROR_INVALID_FUNCTION;
    }

    DWORD dwStatus = 0;
    std::wstring sErr;
    bool ok = pfn(flags, sTarget, sParam, &dwStatus, &sErr);
    if (!ok)
    {
        if (psErr)
            *psErr = sErr.empty() ? DismApiBridge::GetLastErrorText() : sErr;
        nExit = 1;
    }

    if (bCoMine) ::CoUninitialize();
    return nExit;
}

} // namespace CbsHost
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
inline constexpr int kCpp17_CbsHostBridge_StepTimeoutMs = 120000;
inline constexpr int kCpp17_CbsHostBridge_MaxRetry = 3;
