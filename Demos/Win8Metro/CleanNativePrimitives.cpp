#include "stdafx.h"
#include "CleanNativePrimitives.h"



#include <vector>
#include <map>
// [C++17 阶段2] 局部 RAII 守卫：文件/查找/流查找句柄（见 WinHandles.h 使用规范）
struct CNPHandleGuard
{
    HANDLE h;
    explicit CNPHandleGuard(HANDLE x) : h(x) {}
    ~CNPHandleGuard() { if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h); }
    CNPHandleGuard(const CNPHandleGuard&) = delete;
    CNPHandleGuard& operator=(const CNPHandleGuard&) = delete;
    operator HANDLE() const { return h; }
};

struct CNPFindGuard
{
    HANDLE h;
    explicit CNPFindGuard(HANDLE x) : h(x) {}
    ~CNPFindGuard() { if (h && h != INVALID_HANDLE_VALUE) ::FindClose(h); }
    CNPFindGuard(const CNPFindGuard&) = delete;
    CNPFindGuard& operator=(const CNPFindGuard&) = delete;
    operator HANDLE() const { return h; }
};

// ============================================================
// [R-B38] 清单维度 1 缺失 2「NCleaner 5 个底层清理原语」中
// 全树 0 命中的 3 个原生原语落地实现。
// ============================================================

// ------------------------------------------------------------
// 原语 1：NCDeleteFile（重启后删除）—— MoveFileExW 原语
// ------------------------------------------------------------
bool CleanNativePrimitives::NCPendingRebootDelete(const CDuiString& sPath)
{
    if (sPath.IsEmpty()) return false;

    if (::GetFileAttributes(sPath.GetData()) == INVALID_FILE_ATTRIBUTES)
        return true;   // 已不存在

    // 标准登记：源路径 + nullptr 目标 = Session Manager 删除
    BOOL ok = ::MoveFileExW(sPath.GetData(), nullptr,
                            MOVEFILE_DELAY_UNTIL_REBOOT | MOVEFILE_REPLACE_EXISTING);
    if (ok) return true;

    // 部分只读卷要求显式目标名；以自身名触发同样的登记语义
    ok = ::MoveFileExW(sPath.GetData(), sPath.GetData(),
                       MOVEFILE_DELAY_UNTIL_REBOOT | MOVEFILE_REPLACE_EXISTING);
    return ok != FALSE;
}

// ------------------------------------------------------------
// 原语 2：强制删除 —— SetFileInformationByHandle(FileDispositionInfoEx)
// ------------------------------------------------------------
bool CleanNativePrimitives::NCForceDeleteByDisposition(const CDuiString& sPath)
{
    if (sPath.IsEmpty()) return false;
    if (::GetFileAttributes(sPath.GetData()) == INVALID_FILE_ATTRIBUTES)
        return true;

    DWORD dwAttr = ::GetFileAttributes(sPath.GetData());
    if (dwAttr & FILE_ATTRIBUTE_DIRECTORY)
    {
        // 目录走 RemoveDirectoryW（本原语只覆盖文件级）
        return ::RemoveDirectoryW(sPath.GetData()) != FALSE;
    }

    // 清只读位，否则 FileDisposition 会因只读失败
    ::SetFileAttributesW(sPath.GetData(), dwAttr & ~FILE_ATTRIBUTE_READONLY);

    CNPHandleGuard h = CNPHandleGuard(::CreateFileW(sPath.GetData(),
                             DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS,
                             nullptr));
    if (h == INVALID_HANDLE_VALUE)
        return false;

    // FILE_DISPOSITION_INFO（BOOLEAN DeleteFile）在 Vista+ 全版本可用
    FILE_DISPOSITION_INFO disp;
    disp.DeleteFile = TRUE;
    BOOL ok = ::SetFileInformationByHandle(h, FileDispositionInfo, &disp, sizeof(disp));

    if (ok)
        return ::GetFileAttributes(sPath.GetData()) == INVALID_FILE_ATTRIBUTES;
    return false;
}

// ------------------------------------------------------------
// 原语 3：ADS 枚举 —— FindFirstStreamW / FindNextStreamW
// ------------------------------------------------------------
ULONGLONG CleanNativePrimitives::NCQueryAdsSize(const CDuiString& sPath)
{
    if (sPath.IsEmpty()) return 0;

    CDuiString sPattern(sPath);
    sPattern += L":*";

    ULONGLONG ullTotal = 0;
    WIN32_FIND_STREAM_DATA fsd;
    CNPFindGuard h(::FindFirstStreamW(sPattern.GetData(), FindStreamInfoStandard, &fsd, 0));
    if (h == INVALID_HANDLE_VALUE)
        return 0;

    do
    {
        if (fsd.cStreamName[0] != L':' || fsd.cStreamName[1] == L'\0')
            continue;
        if (lstrcmpiW(fsd.cStreamName, L"::$DATA") == 0)
            continue;

        LARGE_INTEGER li;
        li.HighPart = fsd.StreamSize.HighPart;
        li.LowPart  = fsd.StreamSize.LowPart;
        if (li.QuadPart > 0)
            ullTotal += static_cast<ULONGLONG>(li.QuadPart);
    } while (::FindNextStreamW(h, &fsd));

    return ullTotal;
}

bool CleanNativePrimitives::NCAnyAdsExist(const CDuiString& sPath)
{
    if (sPath.IsEmpty()) return false;
    CDuiString sPattern(sPath);
    sPattern += L":*";

    WIN32_FIND_STREAM_DATA fsd;
    CNPFindGuard h(::FindFirstStreamW(sPattern.GetData(), FindStreamInfoStandard, &fsd, 0));
    if (h == INVALID_HANDLE_VALUE)
        return false;

    bool bFound = false;
    do
    {
        if (fsd.cStreamName[0] != L':' || fsd.cStreamName[1] == L'\0')
            continue;
        if (lstrcmpiW(fsd.cStreamName, L"::$DATA") == 0)
            continue;
        bFound = true;
        break;
    } while (::FindNextStreamW(h, &fsd));

    return bFound;
}

// ------------------------------------------------------------
// [FIX-DEL-1] 原语 6：SCM 停/启服务（服务感知删除支撑）
// 精简移植自 Page_CleanupOps 的 ScmStopService/ScmStartService（TU 局部
// static 跨 TU 不可见）；DeleteRules 回退链对被服务独占的目标先停服务
// 再强删。返回 ERROR_SUCCESS 表示目标状态已达成。
// ------------------------------------------------------------
static constexpr DWORD kNcSvcPollCapMs = 1000;

DWORD CleanNativePrimitives::NCStopService(const wchar_t* pszName, DWORD dwTimeoutMs)
{
    if (!pszName || !pszName[0]) return ERROR_INVALID_PARAMETER;
    SC_HANDLE hScm = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hScm) return ::GetLastError();
    SC_HANDLE hSvc = ::OpenServiceW(hScm, pszName, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!hSvc) { DWORD e = ::GetLastError(); ::CloseServiceHandle(hScm); return e; }
    SERVICE_STATUS ss = {};
    DWORD dwErr = ERROR_SUCCESS;
    if (::ControlService(hSvc, SERVICE_CONTROL_STOP, &ss))
        dwErr = ERROR_SUCCESS;
    else
        dwErr = ::GetLastError();   // ERROR_SERVICE_NOT_ACTIVE 视为已停
    if (dwErr == ERROR_SUCCESS || dwErr == ERROR_SERVICE_NOT_ACTIVE)
    {
        const ULONGLONG ulEnd = ::GetTickCount64() + dwTimeoutMs;
        for (;;)
        {
            if (!::QueryServiceStatus(hSvc, &ss)) { dwErr = ::GetLastError(); break; }
            if (ss.dwCurrentState == SERVICE_STOPPED) { dwErr = ERROR_SUCCESS; break; }
            if (::GetTickCount64() >= ulEnd) { dwErr = ERROR_TIMEOUT; break; }
            DWORD dwWait = ss.dwWaitHint ? ss.dwWaitHint : 500;
            if (dwWait > kNcSvcPollCapMs) dwWait = kNcSvcPollCapMs;
            ::Sleep(dwWait);
        }
    }
    ::CloseServiceHandle(hSvc);
    ::CloseServiceHandle(hScm);
    return dwErr;
}

DWORD CleanNativePrimitives::NCStartService(const wchar_t* pszName, DWORD dwTimeoutMs)
{
    if (!pszName || !pszName[0]) return ERROR_INVALID_PARAMETER;
    SC_HANDLE hScm = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hScm) return ::GetLastError();
    SC_HANDLE hSvc = ::OpenServiceW(hScm, pszName, SERVICE_START | SERVICE_QUERY_STATUS);
    if (!hSvc) { DWORD e = ::GetLastError(); ::CloseServiceHandle(hScm); return e; }
    SERVICE_STATUS ss = {};
    DWORD dwErr = ERROR_SUCCESS;
    if (::QueryServiceStatus(hSvc, &ss) && ss.dwCurrentState == SERVICE_RUNNING)
        dwErr = ERROR_SUCCESS;
    else if (::StartServiceW(hSvc, 0, nullptr))
        dwErr = ERROR_SUCCESS;
    else
        dwErr = ::GetLastError();   // ERROR_SERVICE_ALREADY_RUNNING 视为已运行
    if (dwErr == ERROR_SUCCESS || dwErr == ERROR_SERVICE_ALREADY_RUNNING)
    {
        const ULONGLONG ulEnd = ::GetTickCount64() + dwTimeoutMs;
        for (;;)
        {
            if (!::QueryServiceStatus(hSvc, &ss)) { dwErr = ::GetLastError(); break; }
            if (ss.dwCurrentState == SERVICE_RUNNING) { dwErr = ERROR_SUCCESS; break; }
            if (::GetTickCount64() >= ulEnd) { dwErr = ERROR_TIMEOUT; break; }
            DWORD dwWait = ss.dwWaitHint ? ss.dwWaitHint : 500;
            if (dwWait > kNcSvcPollCapMs) dwWait = kNcSvcPollCapMs;
            ::Sleep(dwWait);
        }
    }
    ::CloseServiceHandle(hSvc);
    ::CloseServiceHandle(hScm);
    return dwErr;
}

// ------------------------------------------------------------
// 原语 5：CreateHardLink 通用重复文件回收 —— [R-B57]
// ------------------------------------------------------------
// 内部：单文件条目（完整路径 + 大小）
namespace
{
struct Rb57FileEntry
{
    CDuiString sPath;
    LONGLONG   nSize;
};
}

// 内部：递归收集 sDir 下普通文件（跳过目录、隐藏系统保护项、软链接、自身 ADS）
static void Rb57CollectFiles(const CDuiString& sDir, bool bRecursive,
                             std::vector<Rb57FileEntry>& vFiles)
{
    CDuiString sPat(sDir);
    if (!sPat.IsEmpty() && sPat[sPat.GetLength()-1] != L'\\')
        sPat += L'\\';
    sPat += L'*';

    WIN32_FIND_DATAW fd;
    CNPFindGuard h(::FindFirstFileW(sPat.GetData(), &fd));
    if (h == INVALID_HANDLE_VALUE)
        return;

    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (bRecursive)
            {
                if (fd.cFileName[0] == L'.' &&
                    (fd.cFileName[1] == L'\0' ||
                     (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0')))
                    continue;
                CDuiString sSub(sDir);
                if (!sSub.IsEmpty() && sSub[sSub.GetLength()-1] != L'\\')
                    sSub += L'\\';
                sSub += fd.cFileName;
                Rb57CollectFiles(sSub, bRecursive, vFiles);
            }
            continue;
        }

        // 只收普通文件；跳过系统/隐藏与重解析点（避免回收系统链、junction 目标）
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_HIDDEN |
                                   FILE_ATTRIBUTE_REPARSE_POINT))
            continue;

        Rb57FileEntry e;
        e.nSize = ((LONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        e.sPath = sDir;
        if (!e.sPath.IsEmpty() && e.sPath[e.sPath.GetLength()-1] != L'\\')
            e.sPath += L'\\';
        e.sPath += fd.cFileName;
        vFiles.push_back(e);
    } while (::FindNextFileW(h, &fd));

}

// 内部：Fnv1a64 内容哈希，用于判定内容等价（硬链接合并的安全前提）
static ULONGLONG Rb57HashFile(const CDuiString& sPath)
{
    CNPHandleGuard h = CNPHandleGuard(::CreateFileW(sPath.GetData(), FILE_READ_ATTRIBUTES | GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (h == INVALID_HANDLE_VALUE)
        return 0;

    ULONGLONG ullHash = 1469598103934665603ULL;   // FNV offset
    BYTE buf[65536];
    DWORD nRead = 0;
    // 全量读取保证等价（不按前缀采样，避免假阳性合并）
    while (::ReadFile(h, buf, sizeof(buf), &nRead, nullptr) && nRead > 0)
    {
        for (DWORD i = 0; i < nRead; ++i)
        {
            ullHash ^= buf[i];
            ullHash *= 1099511628211ULL;            // FNV prime
        }
    }
    return ullHash ? ullHash : 1;                   // 避免空值歧义
}

bool CleanNativePrimitives::NCHardLinkMerge(const CDuiString& sRootDir, bool bRecursive,
                                             int& nMerged, ULONGLONG& nBytesFreed, int& nFailed)
{
    nMerged = 0;
    nBytesFreed = 0;
    nFailed = 0;
    if (sRootDir.IsEmpty()) return false;

    // 前置：同卷约束（硬链接必须同卷）
    wchar_t szVol[4] = {0};
    ::GetVolumePathNameW(sRootDir.GetData(), szVol, _countof(szVol));
    if (szVol[0] == L'\0') return false;

    std::vector<Rb57FileEntry> vFiles;
    Rb57CollectFiles(sRootDir, bRecursive, vFiles);
    if (vFiles.size() < 2) return true;

    // 按 (大小, 哈希) 分组；分组内首文件为保留文件
    std::map<ULONGLONG, std::vector<Rb57FileEntry>> mGroups;
    for (size_t i = 0; i < vFiles.size(); ++i)
    {
        Rb57FileEntry& e = vFiles[i];
        if (e.nSize <= 0) continue;
        ULONGLONG ullHash = Rb57HashFile(e.sPath);
        if (ullHash == 0) continue;
        // 组键：size<<32 ^ hash（size 高 32 与 hash 低位拼接，冲突率可忽略）
        ULONGLONG ullKey = (static_cast<ULONGLONG>(e.nSize) << 32) ^ ullHash;
        mGroups[ullKey].push_back(e);
    }

    for (std::map<ULONGLONG, std::vector<Rb57FileEntry>>::iterator it = mGroups.begin();
         it != mGroups.end(); ++it)
    {
        std::vector<Rb57FileEntry>& vGrp = it->second;
        if (vGrp.size() < 2) continue;
        const CDuiString& sKeep = vGrp[0].sPath;
        const LONGLONG nKeepSize = vGrp[0].nSize;
        for (size_t k = 1; k < vGrp.size(); ++k)
        {
            // 双重防护：组键已经包含 hash，这里再校验大小完全一致，
            // 避免极端哈希碰撞下对不同大小文件误建硬链接。
            if (vGrp[k].nSize != nKeepSize)
            {
                continue;
            }
            const CDuiString& sDup = vGrp[k].sPath;
            // [修复 P1-HARDLINK] 旧实现先 DeleteFile(sDup) 再 CreateHardLink，
            // 一旦建链失败（跨卷、权限、文件被占用），原副本已被永久删除。
            // 改为：先在同目录建临时硬链接（不动 sDup），成功后再原子替换
            // sDup；任何一步失败都保留原始数据。
            CDuiString sTmp;
            sTmp.Format(L"%s.rblink%u.tmp",
                        sDup.GetData(), ::GetTickCount());
            if (!::CreateHardLinkW(sTmp.GetData(), sKeep.GetData(), nullptr))
            {
                ++nFailed;
                continue;
            }
            bool bReplaced = false;
            if (::GetFileAttributes(sDup.GetData()) != INVALID_FILE_ATTRIBUTES)
            {
                // ReplaceFile 原子地用 sTmp 替换现存的 sDup（保留 sDup 的
                // 目录项/属性语义），失败再退回 MoveFileEx 强替换。
                if (::ReplaceFileW(sDup.GetData(), sTmp.GetData(),
                                   nullptr, REPLACEFILE_WRITE_THROUGH,
                                   nullptr, nullptr))
                    bReplaced = true;
            }
            if (!bReplaced &&
                ::MoveFileExW(sTmp.GetData(), sDup.GetData(),
                              MOVEFILE_REPLACE_EXISTING))
                bReplaced = true;
            if (!bReplaced)
            {
                ::DeleteFileW(sTmp.GetData());
                ++nFailed;
                continue;
            }
            ++nMerged;
            nBytesFreed += static_cast<ULONGLONG>(vGrp[k].nSize);
        }
    }
    return true;
}
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
