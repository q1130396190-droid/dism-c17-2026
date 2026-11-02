// [C-09 修复] 本文件不再被 CBSHost 工程借编译（见 CBSHost.vcxproj），
// CBSHOST_DLL_BUILD 双身份开关随之删除；文件只归属 Win8Metro 工程。
#include "stdafx.h"

#include "OfflineCbsService.h"



#include <winreg.h>
#include <winternl.h>      // SYSTEM_BASIC_INFORMATION
#include <shlwapi.h>
#include <mutex>           // [BUG-18] OfflineCbs.log 写入互斥
#include <utility>     // [C++17 R-P11] std::move
// [C++17 阶段2] 局部 RAII 守卫：事务/文件句柄、注册表键（见 WinHandles.h 使用规范）
struct OBSFileGuard
{
    HANDLE h;
    explicit OBSFileGuard(HANDLE x) : h(x) {}
    ~OBSFileGuard() { if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h); }
    OBSFileGuard(const OBSFileGuard&) = delete;
    OBSFileGuard& operator=(const OBSFileGuard&) = delete;
    void reset(HANDLE x = nullptr) { if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h); h = x; }
    operator HANDLE() const { return h; }
};

struct OBSRegGuard
{
    HKEY k = nullptr;
    ~OBSRegGuard() { if (k) ::RegCloseKey(k); }
    HKEY* out() { return &k; }
    operator HKEY() const { return k; }
};

// ============================================================
// OfflineCbsService —— R-B13 离线 CBS 深链路实现
//  基线 6 项（[G3] 六域原位分区，不拆分文件）：
//    hives      蜂巢挂载
//    ntdll      ntdll 直调
//    ktm        KTM 事务删除
//    svc        TrustedInstaller 停止
//    log        OfflineCbs.log
//    pendingxml pending.xml 校验
//  CheckReadiness 为跨域编排者，留 OfflineCbs 外层。
// ============================================================

namespace OfflineCbs
{

namespace
{

// ---------- 跨域公共工具 ----------
void FormatErr(std::wstring* pOut, const wchar_t* pszPrefix, DWORD dwErr)
{
    if (!pOut) return;
    std::wstring sBuf;
    sBuf.resize(256);
    wfmt::FormatInto(sBuf.data(), sBuf.size() + 1, L"%s (WinErr=%lu)",
                 pszPrefix, dwErr);
    sBuf.resize(wcslen(sBuf.c_str()));
    *pOut = sBuf;
}

} // namespace

// ---------- 1) 蜂巢挂载 ----------
namespace hives
{

namespace
{

// 拼接 <mount>\Windows\System32\config\<file>
bool BuildConfigPath(const wchar_t* pszMount, const wchar_t* pszFile,
                     std::wstring& out)
{
    if (!pszMount || !pszFile) return false;
    out = pszMount;
    if (!out.empty() && out.back() != L'\\' && out.back() != L'/')
        out += L'\\';
    out += L"Windows\\System32\\config\\";
    out += pszFile;
    return true;
}

// 已挂载 hive 的登记（用于 UnmountAllHives 与幂等判断）
struct MountedHive
{
    std::wstring sTargetKey;
    std::wstring sSource;
};
std::vector<MountedHive>& Registry()
{
    static std::vector<MountedHive> s_v;
    return s_v;
}

} // namespace

const HiveSpec* GetBaselineHives(size_t* pnCount)
{
    static const HiveSpec kHives[] = {
        { L"SAM",            L"SAM",             L"HKLM\\SAM" },
        { L"SECURITY",       L"SECURITY",        L"HKLM\\SECURITY" },
        { L"SOFTWARE",       L"SOFTWARE",        L"HKLM\\SOFTWARE" },
        { L"SYSTEM",         L"SYSTEM",          L"HKLM\\SYSTEM" },
        { L"COMPONENTS",     L"COMPONENTS",      L"HKLM\\COMPONENTS" },
        { L"BCD",            L"BCD-Template",    L"HKLM\\BCD00000000" },
        { L"DEFAULT",        L"DEFAULT",         L"HKLM\\DEFAULT" },
        // [P0] SCHEMA.DAT：启动架构蜂巢（CBSHost 离线挂载七件套之一，
        // 头文件基线注释所列；缺失时按失败记录，不阻断其余蜂巢）。
        { L"SCHEMA",         L"SCHEMA.DAT",      L"HKLM\\SCHEMA_DPP" },
    };
    if (pnCount) *pnCount = _countof(kHives);
    return kHives;
}

bool MountHive(const wchar_t* pszMountRoot, const wchar_t* pszHiveName,
               const wchar_t* pszTargetKey, DWORD* pdwErr)
{
    if (!pszMountRoot || !pszHiveName || !pszTargetKey)
    {
        if (pdwErr) *pdwErr = ERROR_INVALID_PARAMETER;
        return false;
    }

    // [BUG-12] 幂等：目标键已存在则视为成功；非本次挂载，不登记到卸载表。
    OBSRegGuard hTest;
    LONG lr = ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, pszTargetKey, 0,
                              KEY_READ, hTest.out());
    if (lr == ERROR_SUCCESS)
    {
        if (pdwErr) *pdwErr = ERROR_ALREADY_EXISTS;
        return true;
    }

    // 定位 hive 文件：基线用 System32\config\<hiveName>；
    // BCD 用 BCD-Template；不存在时按 <mount>\Boot\BCD 回退
    std::wstring sFile;
    if (lstrcmpiW(pszHiveName, L"BCD") == 0)
    {
        sFile = pszMountRoot;
        if (!sFile.empty() && sFile.back() != L'\\') sFile += L'\\';
        sFile += L"Boot\\BCD";
        DWORD dwAttr = ::GetFileAttributesW(sFile.c_str());
        if (dwAttr == INVALID_FILE_ATTRIBUTES)
        {
            // 回退到 BCD-Template
            BuildConfigPath(pszMountRoot, L"BCD-Template", sFile);
        }
    }
    else
    {
        BuildConfigPath(pszMountRoot, pszHiveName, sFile);
    }

    if (::GetFileAttributesW(sFile.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        if (pdwErr) *pdwErr = ERROR_FILE_NOT_FOUND;
        return false;
    }

    // [BUG-12] RegLoadKeyW 不返回句柄；失败即失败，不再以相同参数重试
    // （原"降级只读探测"与首次调用参数完全相同，无任何降级语义）。
    LONG lLoad = ::RegLoadKeyW(HKEY_LOCAL_MACHINE, pszTargetKey, sFile.c_str());
    if (lLoad != ERROR_SUCCESS)
    {
        if (pdwErr) *pdwErr = static_cast<DWORD>(lLoad);
        return false;
    }

    // 登记
    MountedHive rec;
    rec.sTargetKey = pszTargetKey;
    rec.sSource = sFile;
    // [C++17 R-P11] rec 构造后即登记且不再引用，显式 move
    Registry().push_back(std::move(rec));

    if (pdwErr) *pdwErr = ERROR_SUCCESS;
    return true;
}

bool UnmountHive(const wchar_t* pszTargetKey, DWORD* pdwErr)
{
    if (!pszTargetKey)
    {
        if (pdwErr) *pdwErr = ERROR_INVALID_PARAMETER;
        return false;
    }
    LONG lr = ::RegUnLoadKeyW(HKEY_LOCAL_MACHINE, pszTargetKey);
    if (lr != ERROR_SUCCESS)
    {
        if (pdwErr) *pdwErr = static_cast<DWORD>(lr);
        return false;
    }
    return true;
}

bool MountAllHives(const wchar_t* pszMountRoot, std::vector<std::wstring>* pFailed)
{
    if (!pszMountRoot) return false;
    size_t n = 0;
    const HiveSpec* p = GetBaselineHives(&n);
    bool bAll = true;
    for (size_t i = 0; i < n; ++i)
    {
        DWORD dwErr = 0;
        bool bOk = MountHive(pszMountRoot, p[i].pszName, p[i].pszKey, &dwErr);
        if (!bOk)
        {
            bAll = false;
            if (pFailed)
            {
                std::wstring sErr;
                sErr = p[i].pszName;
                sErr += L": ";
                FormatErr(&sErr, L"load failed", dwErr);
                // [C++17 R-P11] sErr 拼装完成后即入队，显式 move
                pFailed->push_back(std::move(sErr));
            }
        }
    }
    return bAll;
}

bool UnmountAllHives()
{
    auto& v = Registry();
    bool bAll = true;
    for (auto it = v.rbegin(); it != v.rend(); ++it)
    {
        if (!UnmountHive(it->sTargetKey.c_str(), nullptr))
            bAll = false;
    }
    v.clear();
    return bAll;
}

} // namespace hives

// ---------- 2) ntdll 直调 ----------
namespace ntdll
{

namespace
{
using RtlGetVersionProc = LONG ( WINAPI*)(OSVERSIONINFOEXW*);
using NtQuerySystemInformationProc = NTSTATUS(NTAPI*)(unsigned long, void*, unsigned long, PULONG);
HMODULE s_hNtdll = nullptr;
bool s_bNtdllInit = false;

bool EnsureNtdll()
{
    if (s_bNtdllInit) return s_hNtdll != nullptr;
    s_bNtdllInit = true;
    s_hNtdll = ::GetModuleHandleW(L"ntdll.dll");
    if (!s_hNtdll) s_hNtdll = ::LoadLibraryW(L"ntdll.dll");
    return s_hNtdll != nullptr;
}
} // namespace

bool NtGetVersionEx(OSVERSIONINFOEXW* pVer)
{
    if (!pVer || !EnsureNtdll()) return false;
    RtlGetVersionProc pfn = (RtlGetVersionProc)
        ::GetProcAddress(s_hNtdll, "RtlGetVersion");
    if (!pfn) return false;
    *pVer = OSVERSIONINFOEXW{};   // [C++17] value-init replaces ZeroMemory
    pVer->dwOSVersionInfoSize = sizeof(*pVer);
    LONG lr = pfn(pVer);
    return lr == 0;
}

bool NtQueryBasicInfo(SYSTEM_BASIC_INFORMATION* pInfo)
{
    if (!pInfo || !EnsureNtdll()) return false;
    NtQuerySystemInformationProc pfn = (NtQuerySystemInformationProc)
        ::GetProcAddress(s_hNtdll, "NtQuerySystemInformation");
    if (!pfn) return false;
    *pInfo = SYSTEM_BASIC_INFORMATION{};   // [C++17] value-init replaces ZeroMemory
    unsigned long retLen = 0;
    NTSTATUS st = pfn(SystemBasicInformation, pInfo, sizeof(*pInfo), &retLen);
    return st == 0;
}

} // namespace ntdll

// ---------- 3) KTM 事务删除 ----------
namespace ktm
{

namespace
{
using CreateTransactionProc = HANDLE ( WINAPI*)(LPSECURITY_ATTRIBUTES, LPGUID, DWORD, DWORD, DWORD, DWORD, const wchar_t*);
using CommitTransactionProc = BOOL ( WINAPI*)(HANDLE);
using RollbackTransactionProc = BOOL ( WINAPI*)(HANDLE);
using DeleteFileTransactedProc = BOOL ( WINAPI*)(const wchar_t*, HANDLE);
HMODULE s_hKtm = nullptr;
bool s_bKtmInit = false;

bool EnsureKtm()
{
    if (s_bKtmInit) return s_hKtm != nullptr;
    s_bKtmInit = true;
    s_hKtm = ::LoadLibraryW(L"kernel32.dll");  // KTM 在 kernel32
    return s_hKtm != nullptr;
}
} // namespace

bool TxDeleteFile(const wchar_t* pszPath)
{
    if (!pszPath || !EnsureKtm()) return false;
    CreateTransactionProc pCreate = (CreateTransactionProc)
        ::GetProcAddress(s_hKtm, "CreateTransaction");
    CommitTransactionProc pCommit = (CommitTransactionProc)
        ::GetProcAddress(s_hKtm, "CommitTransaction");
    RollbackTransactionProc pRollback = (RollbackTransactionProc)
        ::GetProcAddress(s_hKtm, "RollbackTransaction");
    DeleteFileTransactedProc pDelete = (DeleteFileTransactedProc)
        ::GetProcAddress(s_hKtm, "DeleteFileTransactedW");
    if (!pCreate || !pCommit || !pRollback || !pDelete) return false;

    OBSFileGuard hTx(pCreate(nullptr, nullptr, 0, 0, 0, 0, L"OfflineCbs_TxDelete"));
    if (hTx == INVALID_HANDLE_VALUE) return false;

    BOOL bDel = pDelete(pszPath, hTx);
    BOOL bCommit = FALSE;
    if (bDel)
        bCommit = pCommit(hTx);
    else
        pRollback(hTx);
    return (bDel && bCommit) ? true : false;
}

bool TxDeleteFiles(const std::vector<std::wstring>& vPaths)
{
    if (vPaths.empty()) return true;
    if (!EnsureKtm()) return false;
    CreateTransactionProc pCreate = (CreateTransactionProc)
        ::GetProcAddress(s_hKtm, "CreateTransaction");
    CommitTransactionProc pCommit = (CommitTransactionProc)
        ::GetProcAddress(s_hKtm, "CommitTransaction");
    RollbackTransactionProc pRollback = (RollbackTransactionProc)
        ::GetProcAddress(s_hKtm, "RollbackTransaction");
    DeleteFileTransactedProc pDelete = (DeleteFileTransactedProc)
        ::GetProcAddress(s_hKtm, "DeleteFileTransactedW");
    if (!pCreate || !pCommit || !pRollback || !pDelete) return false;

    OBSFileGuard hTx(pCreate(nullptr, nullptr, 0, 0, 0, 0, L"OfflineCbs_TxDeleteAll"));
    if (hTx == INVALID_HANDLE_VALUE) return false;

    bool bAll = true;
    for (const auto& p : vPaths)
    {
        if (!pDelete(p.c_str(), hTx)) { bAll = false; break; }
    }
    if (bAll) pCommit(hTx);
    else pRollback(hTx);
    return bAll;
}

} // namespace ktm

// ---------- 4) net stop TrustedInstaller ----------
namespace svc
{

EServiceState QueryTrustedInstaller()
{
    SC_HANDLE hScm = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hScm) return EServiceState::kSvcUnknown;
    SC_HANDLE hSvc = ::OpenServiceW(hScm, L"TrustedInstaller", SERVICE_QUERY_STATUS);
    if (!hSvc) { ::CloseServiceHandle(hScm); return EServiceState::kSvcUnknown; }
    SERVICE_STATUS ss = {};
    EServiceState e = EServiceState::kSvcUnknown;
    if (::QueryServiceStatus(hSvc, &ss))
    {
        if (ss.dwCurrentState == SERVICE_STOPPED) e = EServiceState::kSvcStopped;
        else if (ss.dwCurrentState == SERVICE_RUNNING) e = EServiceState::kSvcRunning;
    }
    ::CloseServiceHandle(hSvc);
    ::CloseServiceHandle(hScm);
    return e;
}

bool StopTrustedInstaller(DWORD dwTimeoutMs, DWORD* pdwErr)
{
    SC_HANDLE hScm = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hScm)
    {
        if (pdwErr) *pdwErr = ::GetLastError();
        return false;
    }
    SC_HANDLE hSvc = ::OpenServiceW(hScm, L"TrustedInstaller",
                                     SERVICE_QUERY_STATUS | SERVICE_STOP);
    if (!hSvc)
    {
        if (pdwErr) *pdwErr = ::GetLastError();
        ::CloseServiceHandle(hScm);
        return false;
    }

    SERVICE_STATUS ss = {};
    if (!::QueryServiceStatus(hSvc, &ss))
    {
        DWORD e = ::GetLastError();
        if (pdwErr) *pdwErr = e;
        ::CloseServiceHandle(hSvc);
        ::CloseServiceHandle(hScm);
        return false;
    }

    // 已停止即视为成功
    if (ss.dwCurrentState == SERVICE_STOPPED)
    {
        ::CloseServiceHandle(hSvc);
        ::CloseServiceHandle(hScm);
        if (pdwErr) *pdwErr = ERROR_SUCCESS;
        return true;
    }

    // 请求停止
    if (!::ControlService(hSvc, SERVICE_CONTROL_STOP, &ss))
    {
        DWORD e = ::GetLastError();
        if (pdwErr) *pdwErr = e;
        ::CloseServiceHandle(hSvc);
        ::CloseServiceHandle(hScm);
        return false;
    }

    // 等待进入 STOPPED
    // [BUG-08 修复] 原循环在 dwCheckPoint==0 时即判成功，但 checkpoint 为 0
    // 只表示服务没有挂起的启停进度提示，绝不等于 STOPPED；唯一权威信号是
    // dwCurrentState == SERVICE_STOPPED。QueryServiceStatus 失败或超时也不
    // 能当作成功。
    DWORD dwStart = ::GetTickCount();
    bool bStopped = (ss.dwCurrentState == SERVICE_STOPPED);
    while (!bStopped)
    {
        DWORD dwWait = ss.dwWaitHint > 1000 ? ss.dwWaitHint : 1000;
        if (dwWait > 5000) dwWait = 5000;   // 单次等待封顶 5s，保证超时检查粒度
        ::Sleep(dwWait);
        if ((::GetTickCount() - dwStart) > dwTimeoutMs)
        {
            if (pdwErr) *pdwErr = ERROR_TIMEOUT;
            ::CloseServiceHandle(hSvc);
            ::CloseServiceHandle(hScm);
            return false;
        }
        if (!::QueryServiceStatus(hSvc, &ss))
        {
            if (pdwErr) *pdwErr = ::GetLastError();
            ::CloseServiceHandle(hSvc);
            ::CloseServiceHandle(hScm);
            return false;
        }
        bStopped = (ss.dwCurrentState == SERVICE_STOPPED);
    }

    ::CloseServiceHandle(hSvc);
    ::CloseServiceHandle(hScm);
    if (pdwErr) *pdwErr = ERROR_SUCCESS;
    return true;
}

} // namespace svc

// ---------- 5) OfflineCbs.log ----------
namespace log
{

bool AppendOfflineCbsLog(const wchar_t* pszMountRoot, const wchar_t* pszLine, DWORD* pdwErr)
{
    if (!pszMountRoot || !pszLine)
    {
        if (pdwErr) *pdwErr = ERROR_INVALID_PARAMETER;
        return false;
    }
    std::wstring sLog = pszMountRoot;
    if (!sLog.empty() && sLog.back() != L'\\') sLog += L'\\';
    sLog += L"Windows\\Logs\\CBS";

    // 确保目录存在
    if (!::CreateDirectoryW(sLog.c_str(), nullptr))
    {
        DWORD e = ::GetLastError();
        if (e != ERROR_ALREADY_EXISTS && e != ERROR_PATH_NOT_FOUND)
        {
            if (pdwErr) *pdwErr = e;
            return false;
        }
    }
    sLog += L"\\OfflineCbs.log";

    SYSTEMTIME st = {};
    ::GetLocalTime(&st);
    std::wstring sTs;
    sTs.resize(64);
    wfmt::FormatInto(sTs.data(), sTs.size() + 1,
                 L"%04u-%02u-%02u %02u:%02u:%02u",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    sTs.resize(wcslen(sTs.c_str()));

    // [BUG-18] 进程内互斥：多线程/多会话并发追加不再交错；新建文件写
    // UTF-16LE BOM，避免外部工具按 ANSI/UTF-8 打开时整体乱码。
    static std::mutex s_mtxOfflineLog;
    std::lock_guard<std::mutex> lk(s_mtxOfflineLog);

    // 追加写入（OPEN_ALWAYS 若不存在，FILE_APPEND_DATA 追加）
    OBSFileGuard hFile = OBSFileGuard(::CreateFileW(sLog.c_str(), FILE_APPEND_DATA,
                                 FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr));
    if (hFile == INVALID_HANDLE_VALUE)
    {
        if (pdwErr) *pdwErr = ::GetLastError();
        return false;
    }

    std::wstring sLine;
    sLine = sTs;
    sLine += L" | ";
    sLine += pszLine;
    sLine += L"\r\n";

    DWORD dwWritten = 0;
    BOOL bOk = TRUE;
    if (::GetFileSize(hFile, nullptr) == 0)
    {
        constexpr WORD wBom = 0xFEFF;
        ::WriteFile(hFile, &wBom, sizeof(wBom), &dwWritten, nullptr);
    }
    bOk = ::WriteFile(hFile, sLine.c_str(),
                      static_cast<DWORD>(sLine.size() * sizeof(wchar_t)),
                      &dwWritten, nullptr);
    if (!bOk && pdwErr) *pdwErr = ::GetLastError();
    return bOk ? true : false;
}

bool BeginOfflineCbsSession(const wchar_t* pszMountRoot)
{
    return AppendOfflineCbsLog(pszMountRoot, L"[CBS-OFFLINE] session START");
}

bool EndOfflineCbsSession(const wchar_t* pszMountRoot)
{
    return AppendOfflineCbsLog(pszMountRoot, L"[CBS-OFFLINE] session END");
}

} // namespace log

// ---------- 6) pending.xml 校验 ----------
namespace pendingxml
{

// [R124-1] 读取 PendingDeletes.xml 并按 BOM 正确转码为宽字符。
//  原实现把字节直接当宽字符，导致所有标签匹配失败、前置门恒通过（BUG-05）。
//  返回 true 表示读到了内容；false 表示无内容/不可读（调用方按空表继续）。
static bool ReadPendingXmlText(HANDLE hFile, std::wstring& sBuf)
{
    DWORD dwSizeHigh = 0;
    const DWORD dwSize = ::GetFileSize(hFile, &dwSizeHigh);
    constexpr DWORD kMaxBytes = 4u * 1024u * 1024u;
    if (dwSize == INVALID_FILE_SIZE || dwSizeHigh != 0 || dwSize == 0 || dwSize >= kMaxBytes)
        return false;

    std::vector<char> vRaw(dwSize);
    DWORD cbHave = 0;
    BOOL bRead = TRUE;
    while (cbHave < dwSize && bRead)
    {
        DWORD dwChunk = 0;
        bRead = ::ReadFile(hFile, &vRaw[cbHave], dwSize - cbHave, &dwChunk, nullptr);
        if (!bRead || dwChunk == 0) break;
        cbHave += dwChunk;
    }
    if (!bRead || cbHave == 0) return false;

    const char* p = vRaw.data();
    DWORD cb = cbHave;

    // UTF-16LE BOM：直接按宽字符解释（跳过 2 字节 BOM）
    if (cb >= 2 && static_cast<BYTE>(p[0]) == 0xFF && static_cast<BYTE>(p[1]) == 0xFE)
    {
        sBuf.assign(reinterpret_cast<const wchar_t*>(p + 2), (cb - 2) / sizeof(wchar_t));
        return !sBuf.empty();
    }

    // 其余按 UTF-8 解码；非法序列回退 ANSI（CP_ACP）
    if (cb >= 3 && static_cast<BYTE>(p[0]) == 0xEF && static_cast<BYTE>(p[1]) == 0xBB
        && static_cast<BYTE>(p[2]) == 0xBF)
    {
        p += 3;   // 去 UTF-8 BOM
        cb -= 3;
    }
    UINT uCP = CP_UTF8;
    int cch = ::MultiByteToWideChar(uCP, 0, p, static_cast<int>(cb), nullptr, 0);
    if (cch <= 0)
    {
        uCP = CP_ACP;
        cch = ::MultiByteToWideChar(uCP, 0, p, static_cast<int>(cb), nullptr, 0);
    }
    if (cch <= 0) return false;

    sBuf.resize(static_cast<size_t>(cch));
    ::MultiByteToWideChar(uCP, 0, p, static_cast<int>(cb), &sBuf[0], cch);
    return !sBuf.empty();
}

// [R124-2] PendingDeletes.xml 行解析：小型状态机，逐行推进 Type/SourcePath/
//  DestPath/Item 状态，遇到 </Item> 落一条记录并校验源与 Replace 目标是否存在。
//  提取自 CheckPendingDeletes，降低其圈复杂度；行为与原先逐行等价。
static void ParsePendingItems(const std::wstring& sBuf,
                              std::vector<PendingDeleteItem>* pItems,
                              std::vector<PendingDeleteItem>* pMissing)
{
std::wstring sCurType = L"Delete";
std::wstring sCurSource, sCurDest;
size_t pos = 0;
while (pos < sBuf.size())
{
    size_t eol = sBuf.find(L'\n', pos);
    if (eol == std::wstring::npos) eol = sBuf.size();
    std::wstring sLine = sBuf.substr(pos, eol - pos);
    pos = eol + 1;

    if (sLine.find(L"<Type") != std::wstring::npos)
    {
        if (sLine.find(L"Replace") != std::wstring::npos)
            sCurType = L"Replace";
        else if (sLine.find(L"Delete") != std::wstring::npos)
            sCurType = L"Delete";
    }
    else if (sLine.find(L"<SourcePath") != std::wstring::npos)
    {
        size_t a = sLine.find(L'>');
        size_t b = sLine.find(L"</SourcePath");
        if (a != std::wstring::npos && b != std::wstring::npos && b > a + 1)
            sCurSource = sLine.substr(a + 1, b - a - 1);
    }
    else if (sLine.find(L"<DestPath") != std::wstring::npos)
    {
        size_t a = sLine.find(L'>');
        size_t b = sLine.find(L"</DestPath");
        if (a != std::wstring::npos && b != std::wstring::npos && b > a + 1)
            sCurDest = sLine.substr(a + 1, b - a - 1);
    }
    else if (sLine.find(L"</PendingDeletes>") != std::wstring::npos)
    {
        break;
    }
    else if (sLine.find(L"</Item>") != std::wstring::npos)
    {
        if (!sCurSource.empty())
        {
            PendingDeleteItem item;
            item.sSourcePath = sCurSource;
            item.sDestPath = sCurDest;
            item.bIsReplace = (sCurType == L"Replace");
            item.dwErrorCode = 0;
            if (pItems) pItems->push_back(item);

            // 校验源存在性
            DWORD dwAttr = ::GetFileAttributesW(item.sSourcePath.c_str());
            if (dwAttr == INVALID_FILE_ATTRIBUTES)
            {
                item.dwErrorCode = ERROR_FILE_NOT_FOUND;
                if (pMissing) pMissing->push_back(item);
            }
            // Replace 目标校验
            if (item.bIsReplace && !item.sDestPath.empty())
            {
                dwAttr = ::GetFileAttributesW(item.sDestPath.c_str());
                if (dwAttr == INVALID_FILE_ATTRIBUTES)
                {
                    item.dwErrorCode = ERROR_FILE_NOT_FOUND;
                    if (pMissing) pMissing->push_back(item);
                }
            }
        }
        sCurSource.clear();
        sCurDest.clear();
        sCurType = L"Delete";
    }
}
}

bool CheckPendingDeletes(const wchar_t* pszMountRoot,
                         std::vector<PendingDeleteItem>* pItems,
                         std::vector<PendingDeleteItem>* pMissing,
                         DWORD* pdwErr)
{
    if (!pszMountRoot)
    {
        if (pdwErr) *pdwErr = ERROR_INVALID_PARAMETER;
        return false;
    }

    std::wstring sPending = pszMountRoot;
    if (!sPending.empty() && sPending.back() != L'\\') sPending += L'\\';
    sPending += L"Windows\\WinSxS\\Temp\\PendingDeletes.xml";

    // 简化解析：按 <SourcePath>、<DestPath>、<Type> 标签逐行扫描
    OBSFileGuard hFile = OBSFileGuard(::CreateFileW(sPending.c_str(), GENERIC_READ,
                                 FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr));
    if (hFile == INVALID_HANDLE_VALUE)
    {
        if (pdwErr) *pdwErr = ::GetLastError();
        return false;   // 无 pending 项即视为满足前置条件
    }

    // [BUG-05 修复] PendingDeletes.xml 是 UTF-8/ANSI 文本（可带 BOM），
    // 原实现把文件字节直接当宽字符（resize(dwSize/2)），所有标签均匹配
    // 失败，前置校验门恒为通过。这里按字节读入后正确转码，并补齐高 32 位
    // 文件大小校验与 4MB 上限。
    std::wstring sBuf;
    ReadPendingXmlText(hFile, sBuf);

    // 逐行解析：Type="Delete" | "Replace"
    ParsePendingItems(sBuf, pItems, pMissing);


    if (pdwErr) *pdwErr = ERROR_SUCCESS;
    return true;
}

} // namespace pendingxml

// ---------- 综合前置校验（跨域编排，留外层）----------

bool CheckReadiness(const wchar_t* pszMountRoot, OfflineReadiness* pOut)
{
    if (!pOut) return false;
    // [BUG-13 修复] 其余函数均显式拒绝 nullptr 映像根；此处原实现漏检，BCD
    // 分支 sPath.back() 与 BuildConfigPath 会解引用空指针。
    if (!pszMountRoot || *pszMountRoot == L'\0')
    {
        ::SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    OfflineReadiness r;
    r.sMountRoot = pszMountRoot ? pszMountRoot : L"";

    // 1) 蜂巢可挂（不真正挂载，只探测文件存在性 + 权限）
    size_t n = 0;
    const hives::HiveSpec* p = hives::GetBaselineHives(&n);
    bool bHives = true;
    std::wstring sHiveErr;
    for (size_t i = 0; i < n; ++i)
    {
        std::wstring sPath;
        if (lstrcmpiW(p[i].pszName, L"BCD") == 0)
        {
            // 与 hives::MountHive 同一定位规则：BCD-Template 优先，Boot\BCD 回退
            sPath = pszMountRoot;
            if (!sPath.empty() && sPath.back() != L'\\' && sPath.back() != L'/')
                sPath += L'\\';
            sPath += L"Windows\\System32\\config\\BCD-Template";
            if (::GetFileAttributesW(sPath.c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                sPath = pszMountRoot;
                if (sPath.back() != L'\\') sPath += L'\\';
                sPath += L"Boot\\BCD";
            }
        }
        else
        {
            sPath = pszMountRoot;
            if (!sPath.empty() && sPath.back() != L'\\' && sPath.back() != L'/')
                sPath += L'\\';
            sPath += L"Windows\\System32\\config\\";
            sPath += p[i].pszName;
        }
        if (::GetFileAttributesW(sPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            bHives = false;
            if (!sHiveErr.empty()) sHiveErr += L"; ";
            sHiveErr += p[i].pszName;
        }
    }
    r.bHivesOk = bHives;
    if (!bHives)
    {
        r.sDetail = L"蜂巢缺失: ";
        r.sDetail += sHiveErr;
    }

    // 2) TI 停止
    r.bTiStopped = (svc::QueryTrustedInstaller() == svc::EServiceState::kSvcStopped);

    // 3) pending 校验
    std::vector<pendingxml::PendingDeleteItem> vMissing;
    DWORD dwErr = 0;
    r.bPendingOk = pendingxml::CheckPendingDeletes(pszMountRoot, nullptr, &vMissing, &dwErr);
    if (r.bPendingOk && !vMissing.empty())
        r.bPendingOk = false;

    // 4) 日志目录可写
    std::wstring sLogDir = r.sMountRoot;
    if (!sLogDir.empty() && sLogDir.back() != L'\\') sLogDir += L'\\';
    sLogDir += L"Windows\\Logs\\CBS";
    DWORD dwAttr = ::GetFileAttributesW(sLogDir.c_str());
    r.bLogDirWritable = (dwAttr != INVALID_FILE_ATTRIBUTES);

    *pOut = r;
    return r.bHivesOk && r.bTiStopped && r.bPendingOk && r.bLogDirWritable;
}

} // namespace OfflineCbs
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
