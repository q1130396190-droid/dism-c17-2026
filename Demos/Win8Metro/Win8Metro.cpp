#include "stdafx.h"
#include "FrameWnd.h"
#include "GeometryDump.h"        // [UI-GEO] control tree geometry
#include "WinHandles.h"   // [H-51] win::HandlePtr RAII for F11_raw_handle_close
#include <optional>   // [C++17 R-P10]
#include <memory>     // [R121] std::unique_ptr 管理主窗口生命周期
#include "FrameWnd_Messages.h"  // kRestartEventName / EAccelCmd 单点定义
#include "DiagnosticsService.h" // P1/S12：诊断服务本地化回调绑定
#include "StringManager.h"
#include "ConfigReader.h"
#include "SeedCache.h"        // [R53] /DumpSeedFingerprint：种子指纹导出
#include "Bench.h"           // [R81] /BenchEnum /BenchRender 无界面基准
#include "CommandLineService.h" // R-B3：CLI 参数层解析
#include "AppxCapUpdService.h"  // [R-B51] 升级链阶段执行（RunUpdatePhase）
#include "UpdPhaseRelay.h"      // [R-B51] 阶段状态文件 + 命名事件回传
#include "DataEngine.h"      // [R-B39] 对拍单测
#include "SafeThread.h"      // 消息泵/工作线程顶层异常边界（防 c000041d/c0000409）
#include "ProcessJob.h"      // [EXIT-FIX] 子进程随主进程终结（Kill-On-Close 作业）
#include <shellapi.h>
#include <shlobj.h>
#include <dbghelp.h>
#include <ctime>
#include <psapi.h>
#include <wininet.h>   // B3: 崩溃上传 HTTP 客户端
#include <delayimp.h>  // DuiLib.dll 延迟加载定位 hook（amd64 子目录）
#include <string>




#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "wininet.lib")   // B3: 崩溃上传用 WinINet



// ============================================================
// 系统优化 R2 · 异常兜底（P0）。约束：回调内禁 MessageBox/StringMgr/
// new/malloc/printf/COM；默认 MiniDumpNormalDump（~5MB）；路径全 ASCII；
// 支持 %AppData%\DismPP\Config\crashdump.ini 开关 + MaxKeep。
// ============================================================
struct CrashDumpConfig { BOOL Enabled; int MaxKeep; MINIDUMP_TYPE DumpType; };



// B3: 崩溃上传配置（默认关闭）。写在 crashdump.ini [upload] 段。
struct CrashUploadConfig
{
    BOOL    bEnable;
    int     nTimeoutMs;
    WCHAR   szUrl[512];   // POST 目标 URL（HTTP/HTTPS）
    WCHAR   szApiKey[128];// 可选，作为 X-API-Key header
};



static void ReadCrashUploadConfig(CrashUploadConfig& cfg, const wchar_t* pszCfgPath)
{
    cfg.bEnable = FALSE;
    cfg.nTimeoutMs = 10000;
    cfg.szUrl[0] = L'\0';
    cfg.szApiKey[0] = L'\0';
    if (!pszCfgPath) return;
    cfg.bEnable = (::GetPrivateProfileIntW(L"Upload", L"Enable", 0, pszCfgPath) != 0);
    int t = ::GetPrivateProfileIntW(L"Upload", L"TimeoutMs", 10000, pszCfgPath);
    if (t > 0 && t <= 60000) cfg.nTimeoutMs = t;
    ::GetPrivateProfileStringW(L"Upload", L"Url", L"", cfg.szUrl, _countof(cfg.szUrl), pszCfgPath);
    ::GetPrivateProfileStringW(L"Upload", L"ApiKey", L"", cfg.szApiKey, _countof(cfg.szApiKey), pszCfgPath);
}



// B3: 从 URL "http(s)://host[:port]/path" 解析出 scheme/host/path/port。
// 用栈变量、无堆分配。
static bool ParseHttpUrlW(const wchar_t* pszUrl, WCHAR* pszScheme, int cchScheme,
                          WCHAR* pszHost, int cchHost,
                          WCHAR* pszPath, int cchPath, int* pnPort)
{
    if (!pszUrl || !pszUrl[0]) return false;
    const wchar_t* p = pszUrl;
    if (_wcsnicmp(p, L"http://", 7) == 0)      { wcscpy_s(pszScheme, cchScheme, L"http"); p += 7; }
    else if (_wcsnicmp(p, L"https://", 8) == 0) { wcscpy_s(pszScheme, cchScheme, L"https"); p += 8; }
    else return false;
    const wchar_t* pSlash = wcschr(p, L'/');
    size_t nHostLen = pSlash ? (size_t)(pSlash - p) : wcslen(p);
    if (nHostLen == 0 || nHostLen >= static_cast<size_t>(cchHost)) return false;
    wcsncpy_s(pszHost, cchHost, p, nHostLen);
    pszHost[nHostLen] = L'\0';
    // 端口（host:8080）
    *pnPort = 0;
    WCHAR* pColon = wcschr(pszHost, L':');
    if (pColon)
    {
        *pnPort = _wtol(pColon + 1);
        *pColon = L'\0';
    }
    if (*pnPort == 0)
        *pnPort = (lstrcmpW(pszScheme, L"https") == 0) ? 443 : 80;
    // 路径
    if (pSlash) wcscpy_s(pszPath, cchPath, pSlash);
    else wcscpy_s(pszPath, cchPath, L"/");
    return true;
}



// B3: 读取本地 dump 文件并通过 WinINet POST 到指定 URL。
// 无堆分配（栈缓冲），回调上下文安全。返回 HTTP 2xx 视为成功。
static bool UploadDumpViaWinInet(const wchar_t* pszDmpPath, const wchar_t* pszUrl,
                                 const wchar_t* pszApiKey, int nTimeoutMs)
{
    if (!pszDmpPath || !pszUrl || !pszUrl[0]) return false;
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (!::GetFileAttributesExW(pszDmpPath, GetFileExInfoStandard, &fad)) return false;
    ULONGLONG ullSize = (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | static_cast<ULONGLONG>(fad.nFileSizeLow);
    // 限制 16MB：避免把完整内存 dump 误传给第三方
    if (ullSize == 0 || ullSize > 16 * 1024 * 1024) return false;



    WCHAR szScheme[8] = {}, szHost[256] = {}, szPath[512] = {};
    int nPort = 0;
    if (!ParseHttpUrlW(pszUrl, szScheme, _countof(szScheme),
                       szHost, _countof(szHost),
                       szPath, _countof(szPath), &nPort))
        return false;



    HINTERNET hSession = ::InternetOpenW(L"Win8Metro-Crash/1.0",
                                         INTERNET_OPEN_TYPE_PRECONFIG,
                                         nullptr, nullptr, 0);
    if (!hSession) return false;



    bool bResult = false;
    __try
    {
        INTERNET_PORT port = static_cast<INTERNET_PORT>(nPort);   // [B6] InternetConnectW 第3参为 INTERNET_PORT，DWORD 触发 C4244
        DWORD flags = (lstrcmpW(szScheme, L"https") == 0)
            ? (INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD)
            : INTERNET_FLAG_RELOAD;
        HINTERNET hConnect = ::InternetConnectW(hSession, szHost, port,
                                                nullptr, nullptr,
                                                INTERNET_SERVICE_HTTP,
                                                flags, 0);
        if (!hConnect) return false;
        __try
        {
            HINTERNET hReq = ::HttpOpenRequestW(hConnect, L"POST", szPath,
                                                 L"Win8Metro/1.0",
                                                 nullptr, nullptr,
                                                 flags, 0);
            if (!hReq) return false;
            __try
            {
                ::HttpAddRequestHeadersW(hReq,
                    L"Content-Type: application/octet-stream\r\n"
                    L"X-Client: Win8Metro-Crash/1.0",
                    -1, HTTP_ADDREQ_FLAG_ADD | HTTP_ADDREQ_FLAG_REPLACE);
                if (pszApiKey && pszApiKey[0])
                {
                    // [F13 白名单] 本函数在 SEH __try 块内（Win8Metro.cpp:225 __try），
                    // MSVC C2712：要求对象展开的函数内禁止 __try，std::wstring 属 RAII
                    // 对象会破坏 SEH 展开。保留 WCHAR 栈缓冲（无堆分配，回调上下文安全）。
                    WCHAR szKeyHdr[200] = {};
                    wfmt::FormatInto(szKeyHdr, _countof(szKeyHdr),
                                 L"X-API-Key: %s", pszApiKey);
                    ::HttpAddRequestHeadersW(hReq, szKeyHdr, -1,
                                              HTTP_ADDREQ_FLAG_ADD);
                }
                // 简化：不设置细粒度超时（用默认）。
                // INTERNET_HEADERS_OPTION / INTERNET_OPTION_EXTENDED_TIMEOUT
                // 需要较新 SDK，此处用默认超时避免编译错误。



                HANDLE hFile = ::CreateFileW(pszDmpPath, GENERIC_READ,
                                             FILE_SHARE_READ, nullptr,
                                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (hFile == INVALID_HANDLE_VALUE) return false;
                __try
                {
                    // 用 HttpSendRequest 直接把整个文件作为 body 发送。
                    // 限制 16MB（前面已检查），一次读完。
                    void* pBody = nullptr;
                    DWORD dwBodyLen = 0;
                    constexpr int kStackCopyBufBytes = 4096;   // 4096 缓冲尺寸（语义命名，数值不变）
                    BYTE szStackBuf[kStackCopyBufBytes];
                    if (ullSize <= sizeof(szStackBuf))
                        pBody = szStackBuf;
                    else
                        pBody = ::LocalAlloc(LMEM_FIXED, ullSize);
                    if (!pBody)
                    {
                        // 无法分配：改用分块 SendRequest 不现实（回调上下文禁 heap）
                        // 直接放弃
                        return false;
                    }
                    DWORD nRead = 0;
                    if (!::ReadFile(hFile, pBody, static_cast<DWORD>(ullSize), &nRead, nullptr)
                        || nRead != static_cast<DWORD>(ullSize))
                    {
                        if (pBody != szStackBuf) ::LocalFree(pBody);
                        return false;
                    }
                    // SendRequest 一次性发送整个 body
                    BOOL bSent = ::HttpSendRequest(hReq, nullptr, 0,
                                                     pBody, static_cast<DWORD>(ullSize));
                    if (pBody != szStackBuf) ::LocalFree(pBody);
                    if (!bSent) return false;



                    DWORD dwStatus = 0, cbStatus = sizeof(dwStatus);
                    ::HttpQueryInfo(hReq, HTTP_QUERY_STATUS_CODE,
                                     (void*)&dwStatus, &cbStatus, nullptr);
                    // 读完 body（不管大小，避免连接挂起）
                    while (TRUE)
                    {
                        DWORD dwAvail = 0;
                        ::InternetQueryDataAvailable(hReq, &dwAvail, 0, 0);
                        if (dwAvail == 0) break;
                        constexpr int kPipeReadBufBytes = 4096;   // 4096 缓冲尺寸（语义命名，数值不变）
                        BYTE szRd[kPipeReadBufBytes];
                        DWORD nGot = 0;
                        if (!::InternetReadFile(hReq, szRd, _countof(szRd), &nGot)
                            || nGot == 0) break;
                    }
                    bResult = (dwStatus >= 200 && dwStatus < 300);
                }
                __finally
                {
                    if (hFile != INVALID_HANDLE_VALUE) ::CloseHandle(hFile);
                }
            }
            __finally
            {
                ::InternetCloseHandle(hReq);
            }
        }
        __finally
        {
            ::InternetCloseHandle(hConnect);
        }
    }
    __finally
    {
        ::InternetCloseHandle(hSession);
    }
    return bResult;
}



// B3: 上传任务与线程。使用 static 存储：崩溃处理回调里无法安全调 heap，
// 而且崩溃一次 = 进程一次死亡 = 只可能起一次上传线程，static 语义安全。
struct UploadTask
{
    WCHAR szDmp[MAX_PATH];
    WCHAR szUrl[512];
    WCHAR szApiKey[128];
    int   nTimeoutMs;
};
static UploadTask s_uploadTask;   // 崩溃后填充一次
// [FIX-P2L] 上传在途闸门：二次崩溃重写 s_uploadTask 时上传线程可能正读
// 同一结构（数据撕裂）。Interlocked 认领保证同一时刻只有一次填充+上传；
// 已在途时本次直接跳过（dump 文件保留，不影响下次会话上传）。
static volatile LONG s_bUploadInFlight = 0;
// [C++17 升级阶段2] 局部句柄 RAII 守卫：隐式转换为原句柄类型，调用点
// 零改动；只负责早退/异常路径自动释放（栈上构造，不堆分配，崩溃回调
// 上下文安全）。SEH __try/__finally 段（下载函数）、单实例互斥分支、
// detached 线程句柄 hUp 与 PROCESS_INFORMATION 按方案保持显式管理。
struct FileGuard
{
    FileGuard() = default;
    FileGuard(HANDLE h) : m_h(h) {}
    ~FileGuard() { if (m_h != nullptr && m_h != INVALID_HANDLE_VALUE) ::CloseHandle(m_h); }
    FileGuard(const FileGuard&) = delete;
    FileGuard& operator=(const FileGuard&) = delete;
    operator HANDLE() const { return m_h; }
private:
    HANDLE m_h = nullptr;
};
struct FindGuard
{
    FindGuard() = default;
    FindGuard(HANDLE h) : m_h(h) {}
    ~FindGuard() { if (m_h != nullptr && m_h != INVALID_HANDLE_VALUE) ::FindClose(m_h); }
    FindGuard(const FindGuard&) = delete;
    FindGuard& operator=(const FindGuard&) = delete;
    operator HANDLE() const { return m_h; }
private:
    HANDLE m_h = nullptr;
};
// [FIX-P2L] 工作体拆出（含 C++ 对象），线程入口层只留 SEH 壳：
// 裸 CreateThread 无顶层异常边界，工作体 AV 会直接杀进程（崩溃上下文
// 连 CrashGuard 都不会再触发）。C2712 约束：__try 函数内不得有需展开对象。
static bool UploadCrashBody()
{
    bool bOk = UploadDumpViaWinInet(s_uploadTask.szDmp, s_uploadTask.szUrl,
                                     s_uploadTask.szApiKey, s_uploadTask.nTimeoutMs);
    if (bOk)
    {
        // 写 sidecar .sent 标记，便于将来 PruneOldDumps 判断
        WCHAR szSent[MAX_PATH] = {};
        wfmt::FormatInto(szSent, _countof(szSent),
                     L"%s", s_uploadTask.szDmp);
        size_t n = wcslen(szSent);
        if (n > 4 && _wcsnicmp(szSent + n - 4, L".dmp", 4) == 0)
        {
            szSent[n - 4] = L'\0';
            wcscat_s(szSent, _countof(szSent), L".sent");
            FileGuard hSent = ::CreateFileW(szSent, GENERIC_WRITE, 0, nullptr,
                                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);


        }
    }
    return bOk;
}
static DWORD WINAPI UploadCrashThread(void* pv)
{
    (void)pv;
    __try
    {
        UploadCrashBody();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // 上传失败静默：崩溃上下文无 UI/日志可走，dump 文件仍在
    }
    return 0;
}



static void ReadCrashDumpConfig(CrashDumpConfig& cfg)
{
    cfg.Enabled = TRUE;
    cfg.MaxKeep = 10;
    cfg.DumpType = MiniDumpNormal;



    WCHAR szDir[MAX_PATH] = {}, szCfg[MAX_PATH] = {};
    if (!::SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, szDir))
    {
        wfmt::FormatInto(szCfg, _countof(szCfg),
                     L"%s\\DismPP\\Config\\crashdump.ini", szDir);
        cfg.Enabled = (::GetPrivateProfileIntW(L"Dump", L"Enabled", 1, szCfg) != 0);
        cfg.MaxKeep = ::GetPrivateProfileIntW(L"Dump", L"MaxKeep", 10, szCfg);
        if (cfg.MaxKeep <= 0)   cfg.MaxKeep = 1;
        if (cfg.MaxKeep > 100)  cfg.MaxKeep = 100;
        int t = ::GetPrivateProfileIntW(L"Dump", L"DumpType", 0, szCfg);
        // t==1 扩展 ThreadInfo+UnloadedModules；t==2 强制回退 NormalDump 防撑爆盘
        if (t == 1) cfg.DumpType = (MINIDUMP_TYPE)(MiniDumpNormal
                                   | MiniDumpWithThreadInfo
                                   | MiniDumpWithUnloadedModules);
    }
}



static void EnsureDirW(const wchar_t* pszPath)
{
    DWORD a = ::GetFileAttributesW(pszPath);
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return;
    ::SHCreateDirectoryExW(nullptr, pszPath, nullptr);   // 递归创建
}



// 异常码 → ASCII 字符串（回调内安全：仅查表，无堆分配）
static const wchar_t* ExcCode2Str(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:         return L"ACCESS_VIOLATION";
    case EXCEPTION_ILLEGAL_INSTRUCTION:      return L"ILLEGAL_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:       return L"INT_DIVIDE_BY_ZERO";
    case EXCEPTION_STACK_OVERFLOW:           return L"STACK_OVERFLOW";
    case EXCEPTION_PRIV_INSTRUCTION:         return L"PRIV_INSTRUCTION";
    case EXCEPTION_DATATYPE_MISALIGNMENT:    return L"DATATYPE_MISALIGNMENT";
    case EXCEPTION_IN_PAGE_ERROR:            return L"IN_PAGE_ERROR";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return L"ARRAY_BOUNDS";
    case EXCEPTION_FLT_DENORMAL_OPERAND:     return L"FLT_DENORMAL";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return L"FLT_DIV_ZERO";
    case EXCEPTION_FLT_INVALID_OPERATION:    return L"FLT_INVALID";
    case EXCEPTION_GUARD_PAGE:               return L"GUARD_PAGE";
    case EXCEPTION_INVALID_DISPOSITION:      return L"INVALID_DISPOSITION";
    case EXCEPTION_NONCONTINUABLE:           return L"NONCONTINUABLE";
    case EXCEPTION_SINGLE_STEP:              return L"SINGLE_STEP";
    case 0xC0000409 /*STACK_BUFFER_OVERRUN*/:return L"STACK_BUFFER_OVERRUN";
    case 0xC000013A /*CONTROL_C_EXIT*/:      return L"CTRL_C_EXIT";
    default:                                 return L"UNKNOWN";
    }
}



// 写 sidecar .log（回调安全：仅 Win32 文件 API + 栈变量；
// 用 EnumProcessModulesEx 线性扫描已加载模块，避免 Toolhelp 快照在回调内分配堆）
static void WriteCrashLog(const wchar_t* pszLog, const EXCEPTION_POINTERS* pEp, const wchar_t* pszDmp)
{
    if (!pEp || !pEp->ExceptionRecord) return;
    const EXCEPTION_RECORD* r = pEp->ExceptionRecord;
    DWORD code = r->ExceptionCode;
    DWORD64 addr = (DWORD64)r->ExceptionAddress;
    SYSTEMTIME st; ::GetLocalTime(&st);
    DWORD64 modBase = 0, offset = 0;



    HMODULE hMods[1024];
    DWORD cbNeeded = 0;
    HANDLE hProc = ::GetCurrentProcess();
    if (::EnumProcessModulesEx(hProc, hMods, sizeof(hMods), &cbNeeded, LIST_MODULES_ALL)
        && cbNeeded <= sizeof(hMods))
    {
        DWORD nMods = cbNeeded / sizeof(HMODULE);
        for (DWORD m = 0; m < nMods; ++m)
        {
            MODULEINFO mi = {};
            if (::GetModuleInformation(hProc, hMods[m], &mi, sizeof(mi)))
            {
                DWORD64 b = (DWORD64)mi.lpBaseOfDll;
                if (addr >= b && addr < b + (DWORD64)mi.SizeOfImage)
                { modBase = b; offset = addr - b; break; }
            }
        }
    }



    FileGuard h = ::CreateFileW(pszLog, GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    // [C++17 阶段3][F13] wchar_t[4096] -> std::wstring
    const size_t kCapCrashBuf = 4096;
    std::wstring sCrashBuf(kCapCrashBuf, L'\0');
    int w = wfmt::FormatInto(sCrashBuf.data(), kCapCrashBuf,
        L"Timestamp     : %04u-%02u-%02u %02u:%02u:%02u.%03u\n"
        L"ProcessId     : %u\nThreadId      : %u\n"
        L"ExceptionCode : %s (0x%08X)\nExceptionAddr : 0x%016llX\n"
        L"AccessType    : %s\nAccessAddr    : 0x%016llX\n"
        L"ModuleBase    : 0x%016llX\nOffset        : 0x%016llX\nDumpPath      : %s\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        (unsigned)::GetCurrentProcessId(), (unsigned)::GetCurrentThreadId(),
        ExcCode2Str(code), code, addr,
        (code == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 1)
            ? (r->ExceptionInformation[0] == 0 ? L"READ" : L"WRITE") : L"N/A",
        (code == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
            ? (DWORD64)r->ExceptionInformation[1] : 0,
        modBase, offset, pszDmp);
    if (w >= 0) sCrashBuf.resize(static_cast<size_t>(w));
    DWORD n = (w > 0) ? static_cast<DWORD>(w) : 0;
    DWORD wb = 0;
    ::WriteFile(h, sCrashBuf.data(), n * sizeof(wchar_t), &wb, nullptr);


}



// 保留最近 N 份（按时间戳字典序即时间序）
static void PruneOldDumps(const wchar_t* pszDir, int nKeep)
{
    if (nKeep <= 0) return;
    WCHAR pat[MAX_PATH] = {};
    wfmt::FormatInto(pat, _countof(pat), L"%s\\crash_*.dmp", pszDir);
    WIN32_FIND_DATAW fd; FindGuard h = ::FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    WCHAR names[100][64] = {}; int cnt = 0;
    do { if (cnt < 100) ::wcscpy_s(names[cnt++], fd.cFileName); }
    while (::FindNextFileW(h, &fd) && cnt < 100);


    if (cnt <= nKeep) return;
    for (int i = 0; i < cnt; ++i)
        for (int j = i + 1; j < cnt; ++j)
            if (::lstrcmpiW(names[i], names[j]) > 0)
            { WCHAR t[64]; ::wcscpy_s(t, names[j]); ::wcscpy_s(names[j], names[i]); ::wcscpy_s(names[i], t); }
    for (int i = 0; i < cnt - nKeep; ++i)
    {
        WCHAR p[MAX_PATH] = {};
        wfmt::FormatInto(p, _countof(p), L"%s\\%s", pszDir, names[i]); ::DeleteFileW(p);
        wfmt::FormatInto(p, _countof(p), L"%s\\%s.log", pszDir, names[i]); ::DeleteFileW(p);
    }
}



// [R-B61] CrashReport 网络上报确认（清单维度 10 缺失 5）
//
// 由 /Crash 接力分支调用：崩溃自重启后检测最新尚未上报的 .dmp，
// 读 crashdump.ini [Upload] 配置，弹 MessageBox 确认（默认「否」），
// 用户同意后复用 B3 的 UploadDumpViaWinInet POST 到配置端点并同步等待结果，
// 成功后写 .sent sidecar + 追加 crashreport.log，并按结果弹 sent/fail 提示。
//
// 此上下文不是崩溃回调，可安全使用堆/字符串 API 与 MessageBox；
// 且需同步拿到上传结果才能给出「已提交/失败」反馈，故不复用 detached 线程。
static void CrashReportPromptAndUpload(const wchar_t* pszDumpDir, const wchar_t* pszCfgPath)
{
    if (!pszDumpDir || !pszDumpDir[0] || !pszCfgPath || !pszCfgPath[0]) return;
    // 1) 定位最新尚未上报的 .dmp（按修改时间取最大；已有同名 .sent 视为已上报）
    WCHAR szLatest[MAX_PATH] = {};
    ULARGE_INTEGER uMax = {};
    uMax.QuadPart = 0;
    {
        WCHAR pat[MAX_PATH] = {};
        wfmt::FormatInto(pat, _countof(pat), L"%s\\crash_*.dmp", pszDumpDir);
        WIN32_FIND_DATAW fd = {};
        FindGuard hFind = ::FindFirstFileW(pat, &fd);
        if (hFind == INVALID_HANDLE_VALUE) return;   // 无 dump：不打扰
        do {
            WCHAR szSide[MAX_PATH] = {};
            wfmt::FormatInto(szSide, _countof(szSide), L"%s\\%s", pszDumpDir, fd.cFileName);
            size_t n = wcslen(szSide);
            if (n > 4 && _wcsnicmp(szSide + n - 4, L".dmp", 4) == 0)
            {
                szSide[n - 4] = L'\0';
                wcscat_s(szSide, _countof(szSide), L".sent");
            }
            if (::GetFileAttributesW(szSide) != INVALID_FILE_ATTRIBUTES) continue;
            ULARGE_INTEGER uFt = { fd.ftLastWriteTime.dwLowDateTime, fd.ftLastWriteTime.dwHighDateTime };
            if (uFt.QuadPart >= uMax.QuadPart)
            {
                uMax = uFt;
                wfmt::FormatInto(szLatest, _countof(szLatest), L"%s\\%s", pszDumpDir, fd.cFileName);
            }
        } while (::FindNextFileW(hFind, &fd));
    }
    if (!szLatest[0]) return;
    // 2) 读 [Upload] 配置（与崩溃回调同源，保持一次写入四处读取的一致语义）
    int nEnable = ::GetPrivateProfileIntW(L"Upload", L"Enable", 0, pszCfgPath);
    WCHAR szUrl[512] = {};
    WCHAR szApiKey[128] = {};
    ::GetPrivateProfileStringW(L"Upload", L"Url", L"", szUrl, _countof(szUrl), pszCfgPath);
    ::GetPrivateProfileStringW(L"Upload", L"ApiKey", L"", szApiKey, _countof(szApiKey), pszCfgPath);
    int nTimeout = ::GetPrivateProfileIntW(L"Upload", L"TimeoutMs", 10000, pszCfgPath);
    if (nTimeout <= 0 || nTimeout > 60000) nTimeout = 10000;
    bool bUrlOk = (szUrl[0] &&
        (_wcsnicmp(szUrl, L"http://", 7) == 0 || _wcsnicmp(szUrl, L"https://", 8) == 0));
    // 3) 上传开关关闭：静默跳过，不打扰用户
    if (nEnable == 0) return;
    // 4) 开关已开但端点无效：明确提示后保留本地
    if (!bUrlOk)
    {
        ::MessageBoxW(nullptr, StringMgr.Get("crash.report_nocfg"),
                      StringMgr.Get("crash.report_title"), MB_OK | MB_ICONINFORMATION);
        return;
    }
    // 5) 弹确认：默认「否」（MB_DEFBUTTON2），尊重用户选择
    CDuiString sAsk = StringMgr.Format(L"crash.report_ask", szLatest);
    if (::MessageBoxW(nullptr, sAsk, StringMgr.Get("crash.report_title"),
                      MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;
    // 6) 同步 POST（此上下文允许阻塞等待网络结果）
    bool bOk = UploadDumpViaWinInet(szLatest, szUrl, szApiKey, nTimeout);
    // 7) 成功：写 .sent sidecar，后续不再重复上报
    if (bOk)
    {
        WCHAR szSent[MAX_PATH] = {};
        wfmt::FormatInto(szSent, _countof(szSent), L"%s", szLatest);
        size_t n = wcslen(szSent);
        if (n > 4 && _wcsnicmp(szSent + n - 4, L".dmp", 4) == 0)
        {
            szSent[n - 4] = L'\0';
            wcscat_s(szSent, _countof(szSent), L".sent");
            FileGuard hSent = ::CreateFileW(szSent, GENERIC_WRITE, 0, nullptr,
                                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
    }
    // 8) 追加 crashreport.log（纯 Win32 + 栈缓冲，落盘审计）
    {
        SYSTEMTIME st0;
        ::GetLocalTime(&st0);
        // [C++17 阶段3][F13] WCHAR[512] -> std::wstring
        const size_t kCapLine = 512;
        std::wstring sLine(kCapLine, L'\0');
        int nLine = wfmt::FormatInto(sLine.data(), kCapLine,
                     L"%04u-%02u-%02u %02u:%02u:%02u  %s  %s\r\n",
                     st0.wYear, st0.wMonth, st0.wDay, st0.wHour, st0.wMinute, st0.wSecond,
                     szLatest, bOk ? L"sent" : L"failed");
        if (nLine >= 0) sLine.resize(static_cast<size_t>(nLine));
        // [C++17 阶段3][F13] WCHAR[MAX_PATH] -> std::wstring
        std::wstring sLogPath(MAX_PATH, L'\0');
        int nLogPath = wfmt::FormatInto(sLogPath.data(), MAX_PATH, L"%s\\crashreport.log", pszDumpDir);
        if (nLogPath >= 0) sLogPath.resize(static_cast<size_t>(nLogPath));
        FileGuard hLog = ::CreateFileW(sLogPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hLog != INVALID_HANDLE_VALUE)
        {
            DWORD wb = 0;
            ::WriteFile(hLog, sLine.data(), static_cast<DWORD>(sLine.size() * sizeof(WCHAR)), &wb, nullptr);
        }
    }
    // 9) 弹结果提示
    ::MessageBoxW(nullptr,
        StringMgr.Get(bOk ? "crash.report_sent" : "crash.report_fail"),
        StringMgr.Get("crash.report_title"),
        bOk ? (MB_OK | MB_ICONINFORMATION) : (MB_OK | MB_ICONWARNING));
}




static LONG WINAPI CrashDumpFilter(EXCEPTION_POINTERS* pEp)
{
    CrashDumpConfig cfg; ReadCrashDumpConfig(cfg);
    if (!cfg.Enabled) return EXCEPTION_EXECUTE_HANDLER;
    if (!pEp || !pEp->ExceptionRecord) return EXCEPTION_EXECUTE_HANDLER;
    DWORD code = pEp->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_BREAKPOINT || code == EXCEPTION_SINGLE_STEP
        || code == 0xC000013A /*CONTROL_C_EXIT*/)
        return EXCEPTION_EXECUTE_HANDLER;
    // ASCII 路径：%AppData%\DismPP\Dump\crash_yyyyMMdd_HHMMSS.###.dmp
    WCHAR szAppData[MAX_PATH] = {};
    if (::SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, szAppData) != S_OK)
        return EXCEPTION_EXECUTE_HANDLER;
    WCHAR szDumpDir[MAX_PATH] = {};
    wfmt::FormatInto(szDumpDir, _countof(szDumpDir),
                 L"%s\\DismPP\\Dump", szAppData);
    EnsureDirW(szDumpDir);
    SYSTEMTIME st; ::GetLocalTime(&st);
    WCHAR szTS[32] = {};
    wfmt::FormatInto(szTS, _countof(szTS),
                 L"crash_%04u%02u%02u_%02u%02u%02u.%03u",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    WCHAR szDmp[MAX_PATH] = {}, szLog[MAX_PATH] = {};
    wfmt::FormatInto(szDmp, _countof(szDmp), L"%s\\%s.dmp", szDumpDir, szTS);
    wfmt::FormatInto(szLog, _countof(szLog), L"%s\\%s.log", szDumpDir, szTS);
    WriteCrashLog(szLog, pEp, szDmp);   // 先写 log（即便 Dump 失败也能定位）



    FileGuard hFile = ::CreateFileW(szDmp, GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION mdei = {};
        mdei.ThreadId = ::GetCurrentThreadId();
        mdei.ExceptionPointers = pEp;
        mdei.ClientPointers = FALSE;
        (void)::MiniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(), hFile,
                                  cfg.DumpType, &mdei, nullptr, nullptr);


    }
    PruneOldDumps(szDumpDir, cfg.MaxKeep);



    // B3: 崩溃上传（默认关闭，由 crashdump.ini [upload] Enable=1 开启）
    // 用 detached thread 后台发送，避免阻塞主进程退出。
    // 用静态缓冲，因为回调上下文禁 new/malloc。
    WCHAR szCfg[MAX_PATH] = {};
    wfmt::FormatInto(szCfg, _countof(szCfg),
                 L"%s\\DismPP\\Config\\crashdump.ini", szAppData);
    if (::GetPrivateProfileIntW(L"Upload", L"Enable", 0, szCfg) != 0)
    {
        // [FIX-P2L] 上传在途闸门：二次崩溃时跳过重填（防 s_uploadTask 撕裂）
        if (::InterlockedCompareExchange(&s_bUploadInFlight, 1, 0) != 0)
            return EXCEPTION_EXECUTE_HANDLER;   // 已在传：按既有语义收尾，dump 已落盘
        s_uploadTask = {};
        wfmt::FormatInto(s_uploadTask.szDmp, _countof(s_uploadTask.szDmp), L"%s", szDmp);
        ::GetPrivateProfileStringW(L"Upload", L"Url", L"",
                                   s_uploadTask.szUrl,
                                   _countof(s_uploadTask.szUrl), szCfg);
        ::GetPrivateProfileStringW(L"Upload", L"ApiKey", L"",
                                   s_uploadTask.szApiKey,
                                   _countof(s_uploadTask.szApiKey), szCfg);
        int t = ::GetPrivateProfileIntW(L"Upload", L"TimeoutMs", 10000, szCfg);
        s_uploadTask.nTimeoutMs = (t > 0 && t <= 60000) ? t : 10000;



        // 仅在 URL 非空且 scheme 合法时才起线程，避免无意义尝试
        if (s_uploadTask.szUrl[0] &&
            (_wcsnicmp(s_uploadTask.szUrl, L"http://", 7) == 0 ||
             _wcsnicmp(s_uploadTask.szUrl, L"https://", 8) == 0))
        {
            HANDLE hUp = ::CreateThread(nullptr, 0, UploadCrashThread,
                                          nullptr, 0, nullptr);
            if (hUp) ::CloseHandle(hUp);   // detached，进程退出前不等待
        }
    }



    // [R-B4] 崩溃后自重启（原版 Dism++ 行为：`/Crash %p %d %p` 参数接力）
    // 仅当未以 /Crash 启动（避免无限循环）且已产生 dmp 时才重启。
    // 回调上下文禁 new/malloc，使用栈缓冲 + 局部字符串。
    {
        WCHAR szRestart[MAX_PATH + 64] = {};
        if (::GetModuleFileNameW(nullptr, szRestart, MAX_PATH) > 0)
        {
            // 检查当前进程是否以 /Crash 启动
            bool bAlreadyRestarted = false;
            wchar_t* szCmd = ::GetCommandLineW();
            if (szCmd && ::wcsstr(szCmd, L"/Crash") != nullptr)
                bAlreadyRestarted = true;
            if (!bAlreadyRestarted)
            {
                // [FIX-STORM] 崩溃重启风暴守卫：logs\crash_relaunch.mark 记录
                // 上一次崩溃时刻，120s 内二次崩溃则不再重启，避免
                // "消失→重现→再消失"的闪屏链。回调上下文禁堆分配：
                // 全部栈缓冲 + 文件时间 API。
                bool bStormSuppressed = false;
                {
                    WCHAR szMark[MAX_PATH + 48] = {};
                    WCHAR szDir[MAX_PATH] = {};
                    DWORD nDir = ::GetModuleFileNameW(nullptr, szDir, MAX_PATH);
                    if (nDir > 0 && nDir < MAX_PATH)
                    {
                        WCHAR* pBS = ::wcsrchr(szDir, L'\\');
                        if (pBS)
                        {
                            *pBS = L'\0';
                            wfmt::FormatInto(szMark, _countof(szMark),
                                             L"%s\\logs\\crash_relaunch.mark", szDir);
                        }
                    }
                    if (szMark[0])
                    {
                        FILETIME ftPrev = {};
                        HANDLE hR = ::CreateFileW(szMark, GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                        if (hR != INVALID_HANDLE_VALUE)
                        {
                            ::GetFileTime(hR, nullptr, nullptr, &ftPrev);
                            ::CloseHandle(hR);
                        }
                        FILETIME ftNow = {};
                        ::GetSystemTimeAsFileTime(&ftNow);
                        if (ftPrev.dwLowDateTime != 0 || ftPrev.dwHighDateTime != 0)
                        {
                            const ULONGLONG uPrev =
                                ((ULONGLONG)ftPrev.dwHighDateTime << 32) | ftPrev.dwLowDateTime;
                            const ULONGLONG uNow =
                                ((ULONGLONG)ftNow.dwHighDateTime << 32) | ftNow.dwLowDateTime;
                            if (uNow > uPrev && (uNow - uPrev) < 1200000000ULL) // 120s×10^7
                                bStormSuppressed = true;
                        }
                        HANDLE hW = ::CreateFileW(szMark, FILE_WRITE_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                        if (hW != INVALID_HANDLE_VALUE)
                        {
                            ::SetFileTime(hW, nullptr, nullptr, &ftNow);
                            ::CloseHandle(hW);
                        }
                    }
                }
                if (bStormSuppressed)
                {
                    ::OutputDebugStringW(L"[crash] restart suppressed (2nd crash within 120s)\r\n");
                }
                else
                {
                WCHAR szArgs[96] = {};
                wfmt::FormatInto(szArgs, _countof(szArgs),
                             L" /Crash %08X %u %08X",
                             (unsigned)(ULONG_PTR)pEp->ExceptionRecord->ExceptionAddress,
                             (unsigned)::GetCurrentProcessId(),
                             (unsigned)code);
                // 拼接完整命令行并启动新进程（不等待，分离执行）
                // 注意：回调上下文禁 new，这里用 CreateProcessW 栈变量
                STARTUPINFOW si = {}; si.cb = sizeof(si);
                PROCESS_INFORMATION pi = {};
                si.dwFlags = STARTF_USESHOWWINDOW;
                si.wShowWindow = SW_SHOWNORMAL;
                WCHAR szFull[MAX_PATH + 128] = {};
                wfmt::FormatInto(szFull, _countof(szFull),
                             L"\"%s\"%s", szRestart, szArgs);
                // 允许失败：仅记录，不影响崩溃处理流程
                // [EXIT-FIX] CREATE_BREAKAWAY_FROM_JOB：接班进程必须脱离
                // 本进程的 Kill-On-Close 作业，否则本进程崩溃终止、作业
                // 句柄关闭时会把刚拉起的新实例一并杀死（作业已设 BREAKAWAY_OK）。
                // [R128-B] 重启失败则用户停在原界面，必须留痕；结果在下方统一判定
                BOOL bRestarted = ::CreateProcessW(szRestart, szFull, nullptr, nullptr, FALSE,
                                 CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB,
                                 nullptr, nullptr, &si, &pi);
                // [R128-B] 拉起接班实例失败则用户只看到进程消失；此处已无可用日志设施，
                //   只能写调试输出（崩溃回调上下文禁用堆分配）。
                if (!bRestarted)
                    ::OutputDebugStringW(L"[R128][crash] 重启接班实例失败\r\n\r\n");
                win::HandlePtr hProc(pi.hProcess);   // [H-51] RAII: auto CloseHandle
                win::HandlePtr hThrd(pi.hThread);
                }   // else(!bStormSuppressed)
            }
        }
    }
    return EXCEPTION_EXECUTE_HANDLER;
}


static void InstallCrashHandler()
{
    static bool sInstalled = false;
    if (sInstalled) return;
    ::SetErrorMode(SEM_FAILCRITICALERRORS);
    ::SetUnhandledExceptionFilter(CrashDumpFilter);
    sInstalled = true;
}



// ============================================================
// [R18 P0] 全局加速键：Esc/F5/F9/F10/Ctrl+F/Ctrl+Z
// ============================================================
static HACCEL CreateAppAccelerators()
{
    ACCEL a[] = {
        { FVIRTKEY,                VK_ESCAPE, WORD(static_cast<UINT>(EAccelCmd::ID_ACCEL_CANCEL))  },
        { FVIRTKEY,                VK_F5,     WORD(static_cast<UINT>(EAccelCmd::ID_ACCEL_REFRESH)) },
        { FVIRTKEY,                VK_F9,     WORD(static_cast<UINT>(EAccelCmd::ID_ACCEL_SCAN))    },
        { FVIRTKEY,                VK_F10,    WORD(static_cast<UINT>(EAccelCmd::ID_ACCEL_CLEAN))   },
        { FVIRTKEY | FCONTROL,     'F',       WORD(static_cast<UINT>(EAccelCmd::ID_ACCEL_FIND))    },
        { FVIRTKEY | FCONTROL,     'Z',       WORD(static_cast<UINT>(EAccelCmd::ID_ACCEL_UNDO))    },
        // [R4] 快捷键扩展
        { FVIRTKEY | FCONTROL,     'N',       WORD(static_cast<UINT>(EAccelCmd::ID_ACCEL_NEW))       },
        { FVIRTKEY | FCONTROL,     'S',       WORD(static_cast<UINT>(EAccelCmd::ID_ACCEL_SAVE))      },
        { FVIRTKEY | FCONTROL,     'A',       WORD(static_cast<UINT>(EAccelCmd::ID_ACCEL_SELECT_ALL))},
        { FVIRTKEY | FCONTROL,     'X',       WORD(static_cast<UINT>(EAccelCmd::ID_ACCEL_CUT))       },
        { FVIRTKEY | FCONTROL,     'C',       WORD(static_cast<UINT>(EAccelCmd::ID_ACCEL_COPY))      },
        { FVIRTKEY | FCONTROL,     'V',       WORD(static_cast<UINT>(EAccelCmd::ID_ACCEL_PASTE))     },
        { FVIRTKEY | FCONTROL,     'Y',       WORD(static_cast<UINT>(EAccelCmd::ID_ACCEL_REDO))      },
        // [R-B1] 原版 7 条补齐：Ctrl+O / Ctrl+Shift+S / Ctrl+Shift+N / Ctrl+Break / Delete
        { FVIRTKEY | FCONTROL,     'O',       WORD(static_cast<UINT>(EAccelCmd::ID_ACCEL_OPEN))      },
        { FVIRTKEY | FCONTROL | FSHIFT, 'S',  WORD(static_cast<UINT>(EAccelCmd::ID_ACCEL_SAVEAS))    },
        { FVIRTKEY | FCONTROL | FSHIFT, 'N',  WORD(static_cast<UINT>(EAccelCmd::ID_ACCEL_NEWWINDOW)) },
        { FVIRTKEY | FCONTROL,     VK_CANCEL, WORD(static_cast<UINT>(EAccelCmd::ID_ACCEL_BREAK))     },
        { 0,                       VK_DELETE, WORD(static_cast<UINT>(EAccelCmd::ID_ACCEL_DELKEY))    },
    };
    return ::CreateAcceleratorTable(a, int(sizeof(a) / sizeof(a[0])));
}



// ============================================================
// 单实例互斥（参考 Qt6 Engine/SystemTools.cpp::hasSingleInstanceGuard）
//   CreateMutex + ERROR_ALREADY_EXISTS → 已有实例运行
//   FindWindow + SetForegroundWindow → 激活已有窗口
//   找不到窗口时弹提示让用户手动关闭
// ============================================================
static bool ActivateExistingWindow()
{
    // [R237] Find the existing window by CLASS name, not by title.
    //
    // Why this replaced the title matching: the titles were a hard-coded list
    // ("Win8 Metro UI" / "Dism++" / "Win8Metro") written before the product was
    // renamed. The window is now titled in a different language entirely, so both
    // FindWindowW(title) and the "contains Win8Metro/Dism" fallback matched nothing.
    // ActivateExistingWindow() then returned false and the second instance showed
    // "already running" and exited -- which the user experiences as the program
    // flashing open and closing. Measured: three instances alive at once, each with
    // its own main window, because none of them could find the others.
    //
    // The class name is registered once by CFrameWnd::GetWindowClassName() and does
    // not change when the display title is retranslated or rebranded, so it is the
    // stable key. Titles are incidentally supported too (for builds that only differ
    // in the caption), but the class is tried first and is authoritative.
    HWND hFound = ::FindWindowW(L"DismppConsole", nullptr);

    if (!hFound)
    {
        // Fallback: same class, any instance, including ones still initialising.
        //   FindWindowW already covers this, so fall back to enumerating top-level
        //   windows of that class in case the primary lookup raced with creation.
        hFound = ::FindWindowExW(HWND_DESKTOP, nullptr, L"DismppConsole", nullptr);
    }

    if (!hFound) return false;

    // Restore before activating: SetForegroundWindow on a minimized window only
    // flashes the taskbar button on some systems.
    if (::IsIconic(hFound)) ::ShowWindow(hFound, SW_RESTORE);

    // A process that is not in the foreground cannot normally steal it; this call
    // is permitted because the launcher currently owns the foreground.
    ::SetForegroundWindow(hFound);
    ::BringWindowToTop(hFound);
    return true;
}



// ============================================================
// [Phase 1 多语言] 解析命令行 /lang=XXX，
// 优先级：命令行 > config.ini > 系统 UI locale > 默认 zh-CN
// ============================================================
static void ResolveInitialLanguage(const wchar_t* pszCmdLine)
{
    // 1) 加载 config.ini（取 exe 目录）
    wchar_t szAppDir[MAX_PATH] = {};
    ::GetModuleFileName(nullptr, szAppDir, _countof(szAppDir));
    wchar_t* pSlash = const_cast<wchar_t*>(wcsrchr(szAppDir, L'\\'));
    if (pSlash) *pSlash = L'\0';
    i18n::CConfigReader::Load(szAppDir);



    // 2) 从命令行取 /lang=XXX（覆盖 config.ini）
    if (pszCmdLine && pszCmdLine[0])
    {
        const wchar_t* pLangFlag = wcsstr(pszCmdLine, L"/lang=");
        if (pLangFlag)
        {
            const wchar_t* pStart = pLangFlag + wcslen(L"/lang=");
            const wchar_t* pEnd = pStart;
            while (*pEnd && *pEnd != L' ' && *pEnd != L'\t'
                   && *pEnd != L'"' && *pEnd != L'\'')
                ++pEnd;
            if (pEnd > pStart)
            {
                std::wstring lang(pStart, pEnd - pStart);
                i18n::CConfigReader::SetLanguageCurrent(lang.c_str());
            }
        }
    }



    // 3) 初始化 CStringManager
    // [C++17 R-P10] std::optional 可选语义
    std::optional<std::wstring> optLang = i18n::CConfigReader::GetLanguageCurrentOpt();
    const wchar_t* pConfig = optLang ? optLang->c_str() : nullptr;
    i18n::CStringManager::GetInstance().Init(szAppDir, pConfig);
    // P1/S12：把诊断服务的本地化查询桥接到 StringMgr（服务单元不依赖 StringManager/UIlib）
    DiagnosticsService_BindLocalizer(
        [](const char* pszKeyUtf8) -> const wchar_t* {
            return i18n::CStringManager::GetInstance().Get(pszKeyUtf8);
        });
}



// ============================================================
// [R-B39] ?Get* 内嵌函数对拍单测（/FunSelfTest 触发）
// ------------------------------------------------------------
// [R31] DismCore FFI 自检入口的前置声明：定义在 DismCore_SelfTest.cpp，
// 未声明时此处调用会报 C3861（已在注册进 vcxproj 后实测确认）。
int RunDismCoreSelfTest();
// 用原版 Data.xml 规则包中真实出现的 ?Get* 表达式做注册表对拍：
//   1) 4 个已注册函数必须返回 true（QueryFunExprSupported 命中注册表）；
//   2) 未注册的 ?GetFoo() 必须返回 false（注册表不认识）；
//   3) 已注册但取值失败的情形仍返回 true 且 *pbResolved=false
//      （保守保留原文，不破坏表达式）。
// 结果写入 %TEMP%\dismpp_funselftest_<pid>.txt，便于无 GUI 环境核对。
// ============================================================
static int RunFunSelfTest()
{
    int nPass = 0, nFail = 0;
    struct Case { const wchar_t* pszExpr; bool bWantReg; bool bWantResolved; };
    // 表达式取自 bin64/DismData/Data.xml 真实出现的 ?Get* 调用。
    static const Case s_cases[] =
    {
        // ---- 已注册：必须命中注册表（取值是否成功取决于本机状态）----
        { L"?GetRegSz(HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\"
              "CurrentVersion\\App Paths\\chrome.exe,Path)", true, false },
        { L"?GetRegSz(HKEY_CURRENT_USER\\Software\\kugou,AppPath)", true, false },
        { L"?GetRegSz(HKEY_LOCAL_MACHINE\\SOFTWARE\\Alibaba\\WWLights\\"
              "EE76A38A6C9E01F42C551FE3BE585C3B,Path,True)", true, false },
        { L"?GetIniData(%Root%Aliim.ini,Common,Version)", true, false },
        { L"?GetIniData(%Root%version.dat,version,current)", true, false },
        { L"?GetFileVersion(%Root%360se.exe)", true, false },
        { L"?GetFileVersion(%Root%Chrome.exe)", true, false },
        { L"?GetLocaleName()", true, true },   // LOCALE_SNAME 必然可解析
        // ---- 未注册：必须返回 false ----
        { L"?GetFoo(Bar)", false, false },
        { L"?GetRegPath(C:\\a)", false, false },
        { L"plain text without func", false, false },
    };
    constexpr int nCases = static_cast<int>(sizeof(s_cases) / sizeof(s_cases[0]));
    for (int i = 0; i < nCases; ++i)
    {
        CDuiString sExpr = s_cases[i].pszExpr;
        bool bResolved = false;
        // [R10-02] 保留理由：自测对拍须直接核对引擎自身的内嵌函数注册表，
        // 走 m_ctx.DataEngine() 实例门面无意义（QueryFunExprSupported 是
        // 纯静态注册表查询，无实例状态；且自测在 GUI/上下文构建前执行）。
        bool bReg = CDataEngine::QueryFunExprSupported(sExpr, &bResolved);
        bool bOk = (bReg == s_cases[i].bWantReg);
        if (bOk && s_cases[i].bWantResolved)
            bOk = (bResolved == true);
        if (bOk) ++nPass; else ++nFail;
    }
    // 写结果文件（无 GUI 环境下核对用）
    // [C++17 阶段3][F13] WCHAR[MAX_PATH] -> std::wstring
    wchar_t szPath[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, szPath);
    std::wstring sFile(MAX_PATH, L'\0');
    int nFile = wfmt::FormatInto(sFile.data(), MAX_PATH,
                 L"%sdismpp_funselftest_%lu.txt", szPath,
                 (unsigned long)::GetCurrentProcessId());
    if (nFile >= 0) sFile.resize(static_cast<size_t>(nFile));
    FileGuard h = ::CreateFileW(sFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        // [C++17 阶段3][F13] char[256] -> std::string
        const size_t kCapOut = 256;
        std::string sOut(kCapOut, '\0');
        int nOut = sprintf_s(sOut.data(), kCapOut, "R-B39 funselftest pass=%d fail=%d total=%d\r\n",
                    nPass, nFail, nCases);
        if (nOut >= 0) sOut.resize(static_cast<size_t>(nOut));
        DWORD dw = 0;
        ::WriteFile(h, sOut.data(), static_cast<DWORD>(sOut.size()), &dw, nullptr);
    }
    // 同时把结果写入调试输出，便于 IDE/调试器观察
    {
        // [C++17 阶段3][F13] char[256] -> std::string
        const size_t kCapDbg = 256;
        std::string sDbg(kCapDbg, '\0');
        int nDbg = sprintf_s(sDbg.data(), kCapDbg, "[R-B39 funselftest] pass=%d fail=%d total=%d\n",
                    nPass, nFail, nCases);
        if (nDbg >= 0) sDbg.resize(static_cast<size_t>(nDbg));
        ::OutputDebugStringA(sDbg.c_str());
    }
    return nFail;
}


// ============================================================
// DuiLib.dll 延迟加载定位（依赖统一存放于 <exe>\amd64\）
//
// 背景：DuiLib.dll 已改为 /DELAYLOAD 延迟加载并随依赖件放入主程序
// 目录下的 amd64\ 子目录，而延迟加载默认 DLL 搜索路径不含该子目录；
// 且工程存在文件作用域非 POD 对象，其 CRT 动态初始化发生在 WinMain
// 之前（首次 DuiLib 调用早于任何显式预载时机）。
//
// 方案：注册 delayimp 通知 hook。__pfnDliNotifyHook2 为 loader 阶段
// 常量初始化（早于一切 CRT 动态初始化）；helper 在加载延迟 DLL 前
// 发出 dliNotePreLoadLibrary 通知，回调按绝对路径从 <exe>\amd64\
// （兼容旧扁平布局 <exe>\）加载并返回 HMODULE，任意时机的首次绑定
// 均可命中。回调返回 nullptr 时 helper 继续走系统默认搜索。
// ============================================================
static HMODULE LoadArchDependency(const char* pszDll)
{
    wchar_t szDll[MAX_PATH] = { 0 };
    if (::MultiByteToWideChar(CP_UTF8, 0, pszDll, -1, szDll, MAX_PATH) <= 0)
        return nullptr;
    wchar_t szExe[MAX_PATH] = { 0 };
    DWORD nExe = ::GetModuleFileNameW(nullptr, szExe, MAX_PATH);
    if (nExe == 0) return nullptr;
    wchar_t* pSlash = nullptr;
    for (DWORD i = 0; i < nExe; ++i)
        if (szExe[i] == L'\\') pSlash = &szExe[i];
    if (!pSlash) return nullptr;
    pSlash[1] = 0;
    const wchar_t* kRel[] = { L"amd64\\", L"" };
    for (int i = 0; i < _countof(kRel); ++i)
    {
        wchar_t szFull[MAX_PATH] = { 0 };
        wcsncpy_s(szFull, _countof(szFull), szExe, _TRUNCATE);
        wcscat_s(szFull, _countof(szFull), kRel[i]);
        wcscat_s(szFull, _countof(szFull), szDll);
        if (::GetFileAttributesW(szFull) == INVALID_FILE_ATTRIBUTES) continue;
        HMODULE h = ::LoadLibraryExW(szFull, nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (h) return h;
    }
    return nullptr;
}
static FARPROC WINAPI DuiDelayLoadHook(unsigned dliNotify, PDelayLoadInfo pdli)
{
    if (dliNotify != dliNotePreLoadLibrary) return nullptr;
    if (!pdli || !pdli->szDll) return nullptr;
    // [LAYOUT] duilibcalc.dll 同样统一存放于 <exe>\amd64\，据此改为延迟加载，
    // 避免运行时只能从 exe 根目录解析而被迫在根目录保留副本。
    if (_stricmp(pdli->szDll, "duilibcalc.dll") == 0)
        return (FARPROC)LoadArchDependency(pdli->szDll);
    if (_stricmp(pdli->szDll, "DuiLib.dll") != 0
#ifdef _DEBUG
        && _stricmp(pdli->szDll, "DuiLib_d.dll") != 0
#endif
        )
        return nullptr;
    return (FARPROC)LoadArchDependency(pdli->szDll);
}
// VS2015u3+ 头文件中该 hook 为 const 指针；常量初始化在 loader 阶段生效
extern "C" const PfnDliHook __pfnDliNotifyHook2 = DuiDelayLoadHook;


// [R125-2] 进程 DPI 感知初始化：优先 PerMonitorV2，其次 PerMonitor，最后 System。
//  返回 true 表示已由某种方式设置成功；三档全失败时调用方回退 SetProcessDPIAware()。
static bool InitProcessDpiAwareness()
{
bool bDpiSet = false;
HMODULE hUser32 = ::GetModuleHandle(L"user32.dll");
if (hUser32)
{
    using PFN_SetProcessDpiAwarenessContext = HANDLE ( WINAPI*)(HANDLE);
    PFN_SetProcessDpiAwarenessContext pfnCtx =
        (PFN_SetProcessDpiAwarenessContext)::GetProcAddress(
            hUser32, "SetProcessDpiAwarenessContext");
    // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
    if (pfnCtx && pfnCtx((HANDLE)-4))
        bDpiSet = true;
}
if (!bDpiSet)
{
    CDPI dpiHelper;
    if (dpiHelper.SetDPIAwareness(PROCESS_PER_MONITOR_DPI_AWARE))
        bDpiSet = true;
}
if (!bDpiSet)
    ::SetProcessDPIAware();
    return bDpiSet;
}
// [R125-2] /Crash 接力：崩溃自重启后写接力日志，再弹上报确认。
//  提取自 _tWinMain 崩溃分支（3 层嵌套），主函数只保留一句调用。
static void HandleCrashRelay()
{
    if (!CommandLineService::Has(L"crash"))
        return;
WCHAR szAppData[MAX_PATH] = {};
if (::SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, szAppData) == S_OK)
{
    WCHAR szDir[MAX_PATH] = {};
    wfmt::FormatInto(szDir, _countof(szDir),
                 L"%s\\DismPP\\Dump", szAppData);
    EnsureDirW(szDir);
    WCHAR szLog[MAX_PATH] = {};
    wfmt::FormatInto(szLog, _countof(szLog),
                 L"%s\\restart.txt", szDir);
    FileGuard h = ::CreateFileW(szLog, FILE_APPEND_DATA, FILE_SHARE_READ,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        WCHAR szBuf[256] = {};
        wfmt::FormatInto(szBuf, _countof(szBuf),
                     L"self-restart cmdline: %s\r\n", ::GetCommandLineW());
        DWORD wb = 0;
        ::WriteFile(h, szBuf, static_cast<DWORD>(wcslen(szBuf) * sizeof(WCHAR)), &wb, nullptr);
    }
    // ---- [R-B61] CrashReport 网络上报确认（清单维度 10 缺失 5）----
    // 崩溃自重启后弹「是否上报」确认，同意则复用 B3 WinINet POST 端点；
    // 放在 restart.txt 之后：接力日志必须先落盘，再弹用户可见的确认。
    WCHAR szCfg[MAX_PATH] = {};
    wfmt::FormatInto(szCfg, _countof(szCfg),
                 L"%s\\DismPP\\Config\\crashdump.ini", szAppData);
    CrashReportPromptAndUpload(szDir, szCfg);
}
}
static void DumpAttrCheckReport()
{
    // Written after the window exists (every skin file in use has been parsed by
    // then) and before the message loop, so the set is complete and the process
    // still exits on its own.
    const int nUnknown = DuiLib::CPaintManagerUI::GetAttributeValidationUnknownCount();
    std::string sOut = "ATTRCHECK total=" + std::to_string(nUnknown) + "\n";
    for (int i = 0; i < nUnknown; ++i)
    {
        LPCTSTR pszName = DuiLib::CPaintManagerUI::GetAttributeValidationUnknownName(i);
        if (pszName == nullptr) continue;
        // Narrow to UTF-8 so the report is byte-stable: it is diffed by tools,
        // and a UTF-16 file with a BOM trips naive line readers.
        const int cb = ::WideCharToMultiByte(CP_UTF8, 0, pszName, -1, nullptr, 0, nullptr, nullptr);
        if (cb <= 1) continue;
        std::string sName(static_cast<size_t>(cb - 1), static_cast<char>(0));
        ::WideCharToMultiByte(CP_UTF8, 0, pszName, -1, &sName[0], cb, nullptr, nullptr);
        sOut += "ATTRCHECK " + sName + "\n";
    }
    const HANDLE hOut = ::CreateFileW(L"attrcheck.txt", GENERIC_WRITE, 0, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hOut != INVALID_HANDLE_VALUE)
    {
        DWORD dwW = 0;
        ::WriteFile(hOut, sOut.data(), static_cast<DWORD>(sOut.size()), &dwW, nullptr);
        ::CloseHandle(hOut);
    }
}
int APIENTRY _tWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, wchar_t* lpCmdLine, int nCmdShow)
{
    // DuiLib.dll 的 amd64 定位由文件上部的 delayimp 通知 hook 自动完成
    // （loader 阶段生效，覆盖 WinMain 之前的全局初始化期），此处无需预载。


    // ---- 0) CLI 参数层解析（最早，供语言/动作/帮助共同使用）----
    CommandLineService::Parse(lpCmdLine);

    // ---- [MIG] /AttrCheck: skin attribute validation ----
    // After the migration an old skin may still write attribute names the new
    // framework does not know. The framework can record those names (tier-3
    // branch in UIControl.cpp) but defaults to off so normal runs pay nothing.
    // Must be on before any skin is parsed: CPaintManagerUI is created inside
    // WindowImplBase::Create, so enabling it after creation is too late -- hence
    // the process-wide default rather than a per-manager flag.
    const bool bAttrCheck = CommandLineService::Has(L"AttrCheck");
    if (bAttrCheck)
        DuiLib::CPaintManagerUI::SetAttributeValidationDefault(true);


    // ---- [R-B39] /FunSelfTest: 对拍单测（无 GUI 依赖）----
    if (CommandLineService::IsFunSelfTest())
    {
        RunFunSelfTest();
        return 0;   // 对拍完成即退出，不拉起 GUI
    }
    // ---- [R31] /DismCoreSelfTest: Rust dismcore FFI 自检（无 GUI 依赖）----
    // 该入口此前缺失，导致 DismCore_SelfTest.cpp 即使编译通过也永不执行。
    // 现在按自检结果给退出码：全部通过 0，存在失败非 0，便于 CI 判定。
    if (CommandLineService::IsDismCoreSelfTest())
    {
        const int nPass = RunDismCoreSelfTest();
        return (nPass > 0) ? 0 : 1;
    }
    // ---- [R81] /BenchEnum /BenchRender: 无界面性能基准（不建窗口、不进消息循环）----
    //   为什么必须在 GUI 之前拦：这两个开关要量的是"避开 UI 之后还剩多少成本"，
    //   一旦拉起窗口，测出来的数字就掺进了主界面初始化。
    if (CommandLineService::IsBenchEnum())
    {
        return bench::RunBenchEnum();
    }
    if (CommandLineService::IsSeedProbe())
    {
        return bench::RunSeedProbe();
    }
    if (CommandLineService::IsMakeSeed())
    {
        return bench::RunMakeSeed();
    }
    if (CommandLineService::IsBenchRender())
    {
        int nRows = CommandLineService::GetInt(L"Rows", 300);
        return bench::RunBenchRender(nRows);
    }
    // ---- [R53] /DumpSeedFingerprint: 输出种子指纹后退出（不拉 GUI）----
    //   种子包首行必须写 "SEED	<OSBUILD>|<SKU>|<LANG>"，
    //   而这个三元组只能从运行它的机器上取。
    //   原实现只在 SeedCache.cpp 内部计算、无对外出口，
    //   制作种子包的人只能猜指纹；
    //   猜错则加载端直接拒绝，表现为“放了种子包但页面照样转圈”。
    if (CommandLineService::IsDumpSeedFingerprint())
    {
        // [R61] 格式改由 seedcache::MakeSeedHeaderLine() 单一提供，
        //   自检与本入口走同一函数，避免两处格式分歧。
        const std::wstring sOut = seedcache::MakeSeedHeaderLine();
        const HANDLE hOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (hOut != INVALID_HANDLE_VALUE && hOut != nullptr)
        {
            // [R55] 统一走 UTF-8 + WriteFile，删掉 WriteConsoleW 分支。
            //   理由：R53 只实测了"重定向到文件"一条路径；控制台分支从未验证过，
            //   而"未被测试覆盖的分支等于不存在"。两条路径保留一条更安全。
            //   现代 Windows 控制台（conhost / Windows Terminal）能正确显示 UTF-8 字节，
            //   且若代码页不是 65001，用户可用 chcp 65001 或直接重定向取文件——
            //   这条路径已被实测证明可用（bytes=19, text=SEED	19045|0|2052）。
            DWORD dwW = 0;
            const int cb = ::WideCharToMultiByte(CP_UTF8, 0, sOut.c_str(),
                                static_cast<int>(sOut.size()), nullptr, 0, nullptr, nullptr);
            if (cb > 0)
            {
                std::string sUtf8(static_cast<size_t>(cb), '\0');
                ::WideCharToMultiByte(CP_UTF8, 0, sOut.c_str(),
                                static_cast<int>(sOut.size()), &sUtf8[0], cb, nullptr, nullptr);
                ::WriteFile(hOut, sUtf8.data(), static_cast<DWORD>(sUtf8.size()), &dwW, nullptr);
            }
        }
        return 0;
    }
    // ---- [R-B51] 三阶段自调用升级链：子进程侧阶段执行（清单维度 7 缺失 3）----
    // 主进程 StartUpdatePhase 以 /PreDownload|/Install|/Finalize /Update [/noreboot]
    // /path=<状态文件> 拉起本进程；此处不进入 GUI，直接执行阶段动作并退出，
    // 阶段内每次状态变化写 UpdPhaseRelay 状态文件 + set 命名事件，
    // 主进程在 RunHiddenExit 返回后读取驱动 9 态文本。
    {
        EClrAction ePhase = CommandLineService::GetPrimaryAction();
        if (ePhase == EClrAction::ECLR_PREDOWNLOAD || ePhase == EClrAction::ECLR_INSTALL
            || ePhase == EClrAction::ECLR_FINALIZE)
        {
            DuiLib::CDuiString sPhaseFile;
            if (!CommandLineService::GetUpdPhasePath(sPhaseFile))
                sPhaseFile = UpdPhaseRelay::MakePhaseFile(
                    (ePhase == EClrAction::ECLR_PREDOWNLOAD) ? 0
                    : (ePhase == EClrAction::ECLR_INSTALL) ? 1 : 2);
            int nPhase = (ePhase == EClrAction::ECLR_PREDOWNLOAD) ? 0
                       : (ePhase == EClrAction::ECLR_INSTALL) ? 1 : 2;
            bool bNoReboot = CommandLineService::IsUpdNoReboot();
            // 无 GUI：COM 初始化在 RunUpdatePhase 内部（MTA）
            DWORD dwExit = AppxCapUpdService::RunUpdatePhase(
                nPhase, sPhaseFile.GetData(), bNoReboot);
            return static_cast<int>(dwExit);
        }
    }




    // ---- 0) 崩溃转储兜底（尽早安装）----
    // [EXIT-FIX] 先于一切 GUI/业务初始化建立 Kill-On-Close 作业并把本进程
    // 纳入：此后本进程 CreateProcess 的所有工具子进程（dism/powershell/
    // sysprep/prowck/ESD 转换等）默认随本进程终结，正常退出、崩溃、被杀
    // 均不留孤儿；失败静默降级，不影响启动。
    ProcessJob::EnsureCreated();
    InstallCrashHandler();
    // ---- [R-B4] /Crash 接力：由崩溃自重启拉起的本进程写入接力日志 ----
    HandleCrashRelay();


    // ---- 0.5) [Phase 1] 多语言初始化：解析命令行 + config.ini + 加载 lang DLL
    ResolveInitialLanguage(lpCmdLine);



    // ---- 1) 单实例互斥 ----
    // [R-B71] 忠实原版（strings_utf16 L1809）：Global\Dism++-{固定GUID}，
    // 跨会话/终端服务全局唯一。
    static const wchar_t* kMutexName =
        L"Global\\Dism++-{B8BECCD6-BA05-443C-B649-012B3E5C21E6}";
    // 重启接力事件 kRestartEventName 统一定义于 FrameWnd_Messages.h（与 FrameWnd.cpp 同源）
    HANDLE hMutex = ::CreateMutexW(nullptr, FALSE, kMutexName);
    // [R243] 必须在 CreateMutexW 之后**紧邻一行**捕获，此后一律用 dwCrtErr。
    //
    // 原实现在这里隔了几条语句才读 ::GetLastError()，且后面又读了一次。
    // 中间任何 Win32 调用都会覆盖它 —— 实测 OpenEventW 打开一个不存在的事件时
    // 会把 183(ERROR_ALREADY_EXISTS) 改成 2(ERROR_FILE_NOT_FOUND)，
    // 于是"已有实例 -> 激活并退出"那一整块判定恒假，第二个实例直接建了第二个窗口。
    // 时序证据：连续启动时新进程 0.29 秒就出现；若真走了"等待旧实例退出"分支，
    // 至少要等 5 秒，不可能这么快。
    const DWORD dwCrtErr = ::GetLastError();

    {
        // 调试探针只允许打印已捕获的值，绝不能在这里再调 GetLastError。
        std::string szTmp = FormatA("P4 mutex=%p gle=%lu",
                                    static_cast<void*>(hMutex),
                                    static_cast<unsigned long>(dwCrtErr));
        (void)szTmp;
    }

    // 跨完整性级别语义：高权限实例持锁时，低权限实例 CreateMutexW 返回
    // NULL + ERROR_ACCESS_DENIED —— 这同样是"已有实例在运行"，
    // 不能当成创建失败弹错误框（原实现会弹，用户看到的是"启动失败"而非"已打开"）。
    const bool bAlreadyRunning =
        (hMutex != nullptr && dwCrtErr == ERROR_ALREADY_EXISTS) ||
        (hMutex == nullptr && dwCrtErr == ERROR_ACCESS_DENIED);

    if (!hMutex && dwCrtErr != ERROR_ACCESS_DENIED)
    {
        ::MessageBoxW(nullptr, StringMgr.Get("misc.h25e1"),
                      StringMgr.Get("error.title"), MB_OK | MB_ICONERROR);
        return 1;
    }

    if (bAlreadyRunning)
    {
        // 语言切换重启场景：旧实例退出前才 set 该事件（FrameWnd_Menu.cpp 的
        // RestartApplication 内部创建，函数返回即关闭），所以它并非常驻。
        HANDLE hRestart = ::OpenEventW(SYNCHRONIZE, FALSE, kRestartEventName);
        if (hRestart)
        {
            if (hMutex) { ::CloseHandle(hMutex); hMutex = nullptr; }
            ::WaitForSingleObject(hRestart, 5000);
            win::HandlePtr hRestartGuard(hRestart);   // [H-51] RAII: auto CloseHandle

            // 轮询等待旧进程释放互斥量（最多约 5 秒）
            // [FIX-P2I] 100ms×50 → 50ms×100：总上限不变，旧进程退出后
            // 平均接管延迟减半（启动期无窗口，Sleep 不影响 UI，仅拖自身）。
            // [FIX-EXIT] Abandoned 互斥接管：前主被强杀（如任务管理器结束
            // 卡死实例）后互斥对象仍以名字存在且 GetLastError 恒报
            // ALREADY_EXISTS —— 纯 CreateMutex 轮询永远无法接管，新实例
            // 静默退出。打开后 WaitForSingleObject 检测 WAIT_ABANDONED
            // （前主已死，所有权可获）→ 释放并重试 CreateMutex 即可正常
            // 创建全新互斥完成接管。
            DWORD dwErr = ERROR_ALREADY_EXISTS;
            for (int i = 0; i < 100; ++i)
            {
                hMutex = ::CreateMutexW(nullptr, FALSE, kMutexName);
                dwErr  = ::GetLastError();           // 紧邻捕获，不可前置任何调用
                if (hMutex && dwErr != ERROR_ALREADY_EXISTS)
                    break;
                if (hMutex)
                {
                    HANDLE hProbe = ::OpenMutexW(SYNCHRONIZE, FALSE, kMutexName);
                    if (hProbe)
                    {
                        const DWORD w = ::WaitForSingleObject(hProbe, 50);
                        ::ReleaseMutex(hProbe);      // 非持有者调用无害（返回 FALSE）
                        ::CloseHandle(hProbe);
                        if (w == WAIT_ABANDONED)
                        {
                            // 前主已死：所有权已被本进程获取，释放后名字随
                            // 最后句柄关闭而消失，下一轮 CreateMutex 全新创建
                            ::CloseHandle(hMutex);
                            hMutex = nullptr;
                            continue;
                        }
                    }
                    ::CloseHandle(hMutex);
                    hMutex = nullptr;
                }
                ::Sleep(50);
            }
            if (!hMutex)
            {
                hMutex = ::CreateMutexW(nullptr, FALSE, kMutexName);
                dwErr  = ::GetLastError();
            }
            if (!hMutex || dwErr == ERROR_ALREADY_EXISTS)
            {
                // 旧实例 5 秒内没退出：仍按"已有实例"处理，绝不再建一个窗口。
                if (!ActivateExistingWindow())
                {
                    ::MessageBoxW(nullptr, StringMgr.Get("misc.hdd9d"),
                                  L"Win8Metro", MB_OK | MB_ICONINFORMATION);
                }
                if (hMutex) ::CloseHandle(hMutex);
                return 0;
            }
            // 落到这里 = 旧实例已退出、互斥量已归本进程，继续正常启动
        }
        else
        {
            // [R243] 普通重复启动：没有重启事件。
            //   原实现缺这一整块 return，是双实例的**直接成因** ——
            //   条件为假后一路落到 CoInitialize / Create，建出第二个窗口。
            if (!ActivateExistingWindow())
            {
                ::MessageBoxW(nullptr, StringMgr.Get("misc.hdd9d"),
                              L"Win8Metro", MB_OK | MB_ICONINFORMATION);
            }
            if (hMutex) ::CloseHandle(hMutex);
            return 0;
        }
    }



    // ---- 2) 初始化 COM ----
    ::CoInitialize(nullptr);



    // ---- 3) DuiLib 全局设置 ----
    CPaintManagerUI::SetInstance(hInstance);
    // [CLASSIC-SKIN] 默认启用 Classic 风格皮肤（参考架构版 UI 设计规范）。
    //   皮肤路径名同时决定 SkinKeyFromPath 中的 kSkinRoot，二者必须一致。
    CPaintManagerUI::SetResourcePath(CPaintManagerUI::GetInstancePath() + L"skin\\Classic");



    // ---- 4) DPI 感知（必须在 CreateWindow 之前）----
    // [修复 R5 / F-2] 三级降级（仅 demo 层，不改 DuiLib 框架）：
    //   ① Win10 1703+ : Per-Monitor V2（SetProcessDpiAwarenessContext，动态加载）
    //   ② Win8.1+     : Per-Monitor（CDPI::SetDPIAwareness）
    //   ③ 更旧系统     : System DPI（SetProcessDPIAware）
    // 清单(app.manifest)已声明 PerMonitorV2，高版本系统以清单为准；
    // 此处运行时调用在清单缺失/被宿主忽略时兜底，二者不冲突。
    if (!InitProcessDpiAwareness())
        ::SetProcessDPIAware();



    // ---- 5) 创建主窗口 ----
    // [R138] 尺寸由 skin\Win8Metro\Main.xml 的 <Window size> 决定，当前为 1280x900。
    //   原注释写"720P 尺寸 (1280x720)"，与 XML 不符 —— R138 用 Win32 探针实测
    //   主窗口为 1280x900（与 XML 一致），据此更正注释。改尺寸请改 XML，不要改这里。
    // [U-3] 必须在 Main.xml 解析前注册自定义控件（NavOption 自绘导航图标）
    RegisterWin8MetroControls();
    // [R121] 原为裸 new + 不检查 Create 返回值，两个问题：
    //   1) Create 失败（XML 解析失败 / 窗口类注册失败）后仍继续 setDPI、
    //      CenterWindow、ShowWindow —— 对无效窗口操作，行为未定义；
    //   2) 若 setDPI 等中途抛异常，delete pFrame 不执行。
    //   改用 unique_ptr 管理：异常路径自动释放；同时检查 Create 结果，
    //   失败则打日志并提前退出，不再对空窗口做后续操作。
    //   注意：unique_ptr 的释放点仍在 TerminateProcess 之前（作用域结束），
    //   与原 delete 的时机等价。
    std::unique_ptr<CFrameWnd> uFrame(new CFrameWnd(L"Main.xml"));
    CFrameWnd* pFrame = uFrame.get();

    // [R253] 主题模式必须在 Create() 之前读回。
    //
    // 用户报"深色模式下启动是浅色，切一下页面才变深色"。
    // Create() 内部的 GetSkinFile()->ExpandSkinFile() 会把 {tok:Surface}
    // 这类占位符按**当时的**令牌值固化成具体颜色，16 页控件树随即带着
    // 这些颜色建好。LoadThemeMode() 原来排在 Create() 之后：
    //   Create()            <- 仍是 Auto，系统浅色 -> 全部浅色令牌
    //   LoadThemeMode()     <- 此时才改成 Dark，但树已建完
    //   ApplySystemTheme()  <- 只能改 DWM 标题栏与显式重刷的控件
    // 于是首屏浅色，切页时 EnsurePageBuilt 重新取色才变深色。
    // 前移到 Create 之前后，展开皮肤时令牌已是 Dark，首屏直接正确。
    pFrame->LoadThemeMode();


    // [R201] 窗口标题走语言包（原为写死的 "Win8 Metro UI - DuiLib"）。
    //   为什么改：① Win11 上标题栏写 "Win8" 与系统版本认知冲突；
    //   ② 标题是用户可见文案，按项目口径必须走 StringMgr，不能硬编码。
    //   内部控件类名（Win8Metro）与资源目录保持不变 —— 那些不是文案，
    //   改动会牵动皮肤路径与注册表键，收益为零。
    const wchar_t* pszWinTitle = StringMgr.Get("app.title");
    if (!pFrame->Create(nullptr, pszWinTitle,
            UI_WNDSTYLE_FRAME | WS_CLIPCHILDREN, WS_EX_WINDOWEDGE))
    {
        ::OutputDebugStringW(L"[App] 主窗口 Create 失败，无法进入消息循环\r\n");
        return -1;
    }



    // ---- 6) 初始 DPI = 主屏实际 DPI ----
    pFrame->setDPI(CDPI::GetMainMonitorDPI());

    // ---- 6b) [R322] Win11 窗口外观：必须在 ShowWindow 之后 ----
    //   [实测] 深色主题下，在 ShowWindow 之前对"尚未显示"的窗口设置
    //   DWMWA_USE_IMMERSIVE_DARK_MODE，进程会以 0xC0000005 崩溃在同一
    //   系统地址上；浅色主题不崩（该属性写的是 FALSE，不触发 DWM 的
    //   暗色标题栏重建路径）。
    //
    //   对照实验（每次只改一处，其余不动）：
    //     A) 原顺序 ApplySystemTheme -> ShowWindow     深色：崩溃，0xC0000005
    //     B) 注释掉 dwmtheme::ApplyWindowTheme 整段    深色：存活
    //     C) ApplySystemTheme 移到 ShowWindow 之后     深色：存活
    //   B 与 C 都消除崩溃，B 丢了圆角/标题栏跟随，故取 C。
    //
    //   为什么之前一直是"先应用再显示"：早期注释的理由是"圆角由 DWM 按
    //   窗口矩形合成，尺寸未定时观感不稳"。实测圆角在 ShowWindow 之后
    //   设置同样生效（几何已定，反而更稳），原理由不成立。
    //   顺序调整后：热键/开机自启路径上窗口先出现一瞬浅色标题栏，
    //   由紧随其后的 ApplySystemTheme 立刻纠正，肉眼不可见。
    pFrame->CenterWindow();
    pFrame->ShowWindow();
    pFrame->ApplySystemTheme();

    // [R240] 控件树建好之后必须显式重绘一次。
    //
    // 实测（DBWIN_BUFFER 抓 OutputDebugString）：整个进程生命周期里
    // CControlUI::Paint() 只被调用过 1 次、且对象是 root —— 子控件一个都没进去。
    // 也就是说皮肤加载 + 16 页装配完成之后，窗口再没收到过有效的 WM_PAINT，
    // 用户看到的就是"启动后一片空白/纯黑，没有任何显示"。
    //
    // 为什么不能指望系统自动重绘：ShowWindow 时控件树还没装配完，
    // 那一次 WM_PAINT 落在空树上（只有 root），之后没有任何 NeedUpdate()
    // 把根节点重新标脏，而 DuiLib 的 WM_PAINT 分支在
    //     if (!::GetUpdateRect(...)) return true;
    // 处会因为更新区为空直接返回，永远走不到 m_pRoot->Paint()。
    //
    // 这里主动标脏 + 同步 UpdateWindow，保证"树已就绪"这件事真正反映到屏幕。
    if (CControlUI* pRoot = pFrame->GetControlTreeRoot())
    {
        pRoot->NeedUpdate();
        ::InvalidateRect(pFrame->GetHWND(), nullptr, FALSE);
        ::UpdateWindow(pFrame->GetHWND());
    }

    // ---- [R209] 像素级验收：/SHOT=<dir> 离屏渲染各页并存 PNG ----
    //   为什么走程序内部渲染：本会话是非交互桌面，CopyFromScreen / PrintWindow
    //   都取不到像素，只有把控件树画到内存位图才能拿到真实渲染结果。
    //   渲染完立即退出，不进入消息循环，避免干扰正常使用。
    {
        const wchar_t* pszCmdShot = ::GetCommandLineW();
        // [R325] /TOKEND=<file>：按当前主题把全部令牌的 ARGB 值导出，供
        //   对比度分析使用。为什么需要：Accent / AccentSoft / AccentText /
        //   OnAccent 这些值来自系统强调色并在运行期派生（>Shift< 与 >pick<），
        //   静态解析 ThemeTokens.cpp 拿不到，只能让程序自己吐出来。
        {
            const wchar_t* pszAll2 = ::GetCommandLineW();
            const wchar_t* pszT =
                pszAll2 ? wcsstr(pszAll2, L"/TOKEND=") : nullptr;
            if (pszT)
            {
                pszT += 8;   // sizeof("/TOKEND=") - 1
                std::wstring sF(pszT);
                while (!sF.empty() && (sF.back() == L'"' || sF.back() == L' '))
                    sF.pop_back();
                while (!sF.empty() && sF.front() == L'"') sF.erase(sF.begin());
                HANDLE hF = ::CreateFileW(sF.c_str(), GENERIC_WRITE, 0, nullptr,
                                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (hF != INVALID_HANDLE_VALUE)
                {
                    char bF[256] = {};
                    int nF = 0;
                    nF += ::sprintf_s(bF + nF, sizeof(bF) - nF, "dark=%d",
                                      tokens::IsDark() ? 1 : 0);
                    bF[nF++] = static_cast<char>(10);
                    ::WriteFile(hF, bF, (DWORD)nF, nullptr, nullptr);
                    for (int ti = 0; ti < static_cast<int>(tokens::EToken::Count); ++ti)
                    {
                        const DWORD cv = tokens::Token(static_cast<tokens::EToken>(ti));
                        char bl[128] = {};
                        int nl2 = ::sprintf_s(bl, sizeof(bl), "%d=%08lX", ti,
                                              static_cast<unsigned long>(cv));
                        bl[nl2++] = static_cast<char>(10);
                        ::WriteFile(hF, bl, (DWORD)nl2, nullptr, nullptr);
                    }
                    ::CloseHandle(hF);
                }
                return 0;
            }
        }

        // [R324] /SHOTSWITCH=<dir>[,<finalMode>]：先按当前主题冷启动，再切到
        //   <finalMode> 后才逐页导出。
        //   为什么需要它：/SHOT 是冷启动渲染，每页首次构建时令牌已是最新值，
        //   因此**天然不会暴露"切主题时漏刷新"**。实测把某个页的
        //   RefreshThemeColors() 改成空实现后，/SHOT 仍然 16/16 全 OK ——
        //   门禁看着绿，实际对这类缺陷完全失明。只有先切换再渲染才走得到
        //   那条刷新路径。
        {
            const wchar_t* pszAll = ::GetCommandLineW();
            const wchar_t* pszSw =
                pszAll ? wcsstr(pszAll, L"/SHOTSWITCH=") : nullptr;
            if (pszSw)
            {
                pszSw += 12;   // sizeof("/SHOTSWITCH=") - 1 == 12（写成 13 会吃掉路径首字符）
                std::wstring sSpec(pszSw);
                while (!sSpec.empty() &&
                       (sSpec.back() == L'"' || sSpec.back() == L' '))
                    sSpec.pop_back();
                while (!sSpec.empty() && sSpec.front() == L'"')
                    sSpec.erase(sSpec.begin());
                std::wstring sDir = sSpec;
                std::wstring sMode = L"light";
                const size_t nComma = sSpec.find(L',');
                if (nComma != std::wstring::npos)
                {
                    sDir  = sSpec.substr(0, nComma);
                    sMode = sSpec.substr(nComma + 1);
                }
                tokens::EThemeMode eTarget = tokens::EThemeMode::Light;
                if (sMode == L"dark")  eTarget = tokens::EThemeMode::Dark;
                else if (sMode == L"auto") eTarget = tokens::EThemeMode::Auto;
                // [VERIFY] classic 也要能强切：验收要能在任意保存态下
                // 复现经典蓝首屏，否则"启动皮肤与令牌是否同源"没法验证。
                else if (sMode == L"classic") eTarget = tokens::EThemeMode::Classic;
                pFrame->ApplyThemeForShot(eTarget);   // 走真实切换路径
                pFrame->ExportAllPageShots(sDir.c_str());
                {
                    // [C4129-FIX] 原写法 L"\_geometry.txt" 中 \_ 是非法转义序列（C4129），
                    // 反斜杠被丢弃，路径退化为 sDir + "_geometry.txt"，缺少目录分隔符，
                    // 几何数据会落到错误位置。必须写成 \\ 才表示分隔符。
                    std::wstring sGeoPath = sDir + L"\\_geometry.txt";
                    pFrame->ExportControlTree(sGeoPath.c_str());
                }
                if (bAttrCheck) DumpAttrCheckReport();
                return 0;
            }
        }
        const wchar_t* pszShot = pszCmdShot ? wcsstr(pszCmdShot, L"/SHOT=") : nullptr;
        if (pszShot)
        {
            pszShot += 6;   // strlen("/SHOT=")
            std::wstring sDir(pszShot);
            // 去掉可能的引号与尾部空白
            while (!sDir.empty() && (sDir.back() == L'"' || sDir.back() == L' ')) sDir.pop_back();
            while (!sDir.empty() && (sDir.front() == L'"')) sDir.erase(sDir.begin());
            pFrame->ExportAllPageShots(sDir.c_str());

            // [UI-GEO] Also dump the laid-out control tree; see CFrameWnd::ExportControlTree.
            {
                std::wstring sGeoPath = sDir + L"\\_geometry.txt";
                pFrame->ExportControlTree(sGeoPath.c_str());
            }

            if (bAttrCheck) DumpAttrCheckReport();
            return 0;

        }
    }




    if (bAttrCheck) DumpAttrCheckReport();

    // ---- 7) 消息循环（先过加速键表）----
    HACCEL hAccel = CreateAppAccelerators();
    HWND hMain = pFrame->GetHWND();
    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0))
    {
        // 消息分发经异常边界：窗口过程/定时器/控件回调中逃逸的任何异常
        // 就地拦截留痕（crashguard.log），不再触发 c000041d 杀进程/自重启。
        if (hAccel && hMain && SafeTranslateAccelerator(hMain, hAccel, &msg))
            continue;
        ::TranslateMessage(&msg);
        SafeDispatchMessage(&msg);
        // [ZOMBIE-FIX] 主窗消失但无 WM_QUIT 即退出：正常关闭路径是
        // WM_CLOSE→DestroyWindow→WM_DESTROY→PostQuitMessage，GetMessage
        // 会自然收到 WM_QUIT。若派发栈内发生被 SEH 吞掉的致命 AV
        // （页面切换 UAF 实证），窗口被半途销毁而退出消息从未投递，
        // 进程便以"无窗口僵尸"形态永久存活（实测 PID 4976）。这里
        // 主动判定窗口句柄失效：留痕后跳出循环，走既有的
        // uFrame.reset + TerminateProcess 收尾，绝不再等下一条消息。
        if (hMain && !::IsWindow(hMain))
        {
            CrashGuardWrite("main-loop-window-gone", 0, nullptr);
            break;
        }
    }
    if (hAccel) ::DestroyAcceleratorTable(hAccel);



    // [R121] 与原 delete 同一位置：消息循环退出后再释放。
    //   unique_ptr 在作用域结束也会释放，这里显式 reset 是为了让释放点
    //   与原来的 delete 行**逐一对应**，便于对照行为。
    uFrame.reset();



    ::CoUninitialize();
    // [EXIT-FIX] 防僵尸最终兜底：跳过 CRT/ExitProcess 的 DLL_PROCESS_DETACH
    // 与“等待全部线程到达安全点”流程，立即终结本进程。历史上 detached
    // 工作线程卡在 dismapi/CBS ALPC 或子进程内核等待中，会令 ExitProcess
    // 在加载器锁阶段永久挂起，产生连 SYSTEM/taskkill /F 都无法清除、只能
    // 重启系统的僵尸进程。此处日志已落盘、窗口已销毁、离线蜂巢已卸载；
    // 作业句柄随进程关闭，内核自动杀光全部工具子进程；单实例互斥量也由
    // 内核即时释放，接班实例可立刻启动。
    ::TerminateProcess(::GetCurrentProcess(), 0);
    return 0;
}


// [C++17] HTTP 栈读取缓冲大小（原 L320 魔数常量化）
inline constexpr size_t kHttpStackBufSize = 4096;
