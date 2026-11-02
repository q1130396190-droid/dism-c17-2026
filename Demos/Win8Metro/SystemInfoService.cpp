// SystemInfoService.cpp —— S1（P1）
// 独立于预编译头：不 include stdafx.h / UIlib.h，仅系统头 + Utils。

#include <windows.h>
#include "FormatW.h"
#include "LogService.h"   // 统一日志目录（env.log）
#include <shlobj.h>
#include <objbase.h>
#include "OsVersionInfo.h"   // [OS-Filter] 版本识别（Win11 小版本）


#include "SystemInfoService.h"   // 内含 UILIB_API 防护 + Utils/Utils.h（CDuiString）
#include <stdarg.h>
#include <cstdio>   // [C17] _vsnwprintf_s（F18 tchar.h 清零）
#include <string>   // [R8] std::wstring（F13 纯内部缓冲提级）
// [C++17 阶段2] 局部 RAII 守卫：文件/令牌句柄、注册表键（见 WinHandles.h 使用规范）
struct SISFileGuard
{
    HANDLE h = nullptr;
    SISFileGuard() = default;
    explicit SISFileGuard(HANDLE x) : h(x) {}
    ~SISFileGuard() { if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h); }
    SISFileGuard(const SISFileGuard&) = delete;
    SISFileGuard& operator=(const SISFileGuard&) = delete;
    HANDLE* out() { return &h; }
    operator HANDLE() const { return h; }
};

struct SISRegGuard
{
    HKEY k = nullptr;
    ~SISRegGuard() { if (k) ::RegCloseKey(k); }
    HKEY* out() { return &k; }
    void reset(HKEY n = nullptr) { if (k) ::RegCloseKey(k); k = n; }
    operator HKEY() const { return k; }
};


using DuiLib::CDuiString;

// ============================================================
// 环境诊断日志：与 CleanJobService::LogScanLine 同构（独立模块，不依赖
// LogService 实例），追加到 %LOCALAPPDATA%\DismTool_Clone\logs\env.log
// ============================================================
namespace
{
    void LogEnvLine(const wchar_t* pszFmt, ...)
    {
        // [M-路径统一] env.log 并入统一日志目录（原 %LOCALAPPDATA%\DismTool_Clone\logs）
        const std::wstring sLogPath = LogService::UnifiedLogPath(L"env.log");
        if (sLogPath.empty()) return;
        const wchar_t* szLog = sLogPath.c_str();

        SISFileGuard h = SISFileGuard(::CreateFileW(szLog, FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr));
        if (h == INVALID_HANDLE_VALUE) return;

        std::wstring sLogLine;
        sLogLine.resize(512);
        va_list ap;
        va_start(ap, pszFmt);
        _vsnwprintf_s(&sLogLine[0], sLogLine.size(), _TRUNCATE, pszFmt, ap);
        va_end(ap);
        sLogLine.resize(wcslen(sLogLine.c_str()));

        ::WriteFile(h, sLogLine.c_str(), static_cast<DWORD>(sLogLine.size() * sizeof(wchar_t)), nullptr, nullptr);
    }
}

// "C:" where C:\Windows lives
CDuiString SystemInfoService::GetSystemDriveStr()
{
    wchar_t szWin[MAX_PATH] = { 0 };
    ::GetWindowsDirectory(szWin, MAX_PATH);

    CDuiString s;
    s.Format(L"%c:", szWin[0]);
    return s;
}

CDuiString SystemInfoService::GetDesktopDir()
{
    wchar_t sz[MAX_PATH] = { 0 };
    ::SHGetSpecialFolderPath(nullptr, sz, CSIDL_DESKTOPDIRECTORY, FALSE);
    return CDuiString(sz);
}

DWORD SystemInfoService::GetBuildNumber()
{
    SISRegGuard hKey;
    DWORD dwBuild = 0;

    if (::RegOpenKeyEx(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
            0, KEY_READ | KEY_WOW64_64KEY, hKey.out()) == ERROR_SUCCESS)
    {
        wchar_t szVal[16] = { 0 };
        DWORD cb = sizeof(szVal), dwType = 0;

        if (::RegQueryValueEx(hKey, L"CurrentBuildNumber", nullptr, &dwType,
                reinterpret_cast<BYTE*>(szVal), &cb) == ERROR_SUCCESS)
            dwBuild = static_cast<DWORD>(_wtoi(szVal));

    }

    return dwBuild;
}

CDuiString SystemInfoService::GetWindowsFamilyName()
{
    // [OS-Filter] 改用统一的版本识别：输出含 Win11 小版本（21H2/22H2/
    // 23H2/24H2），便于用户与日志判断规则为何被筛选掉。
    // 数据源为 RtlGetVersion（不受兼容性 manifest 影响），与
    // Data 规则里 <OSVersion> 的判定口径一致。
    return CDuiString(osver::DisplayName());
}

CDuiString SystemInfoService::MakeTimestamp()
{
    SYSTEMTIME st;
    ::GetLocalTime(&st);

    CDuiString s;
    s.Format(L"%04d%02d%02d_%02d%02d%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return s;
}

// create %TEMP%\<prefix>_<guid> and return its full path
CDuiString SystemInfoService::MakeTempWorkDir(const wchar_t* pszPrefix)
{
    wchar_t szTemp[MAX_PATH] = { 0 };
    ::GetTempPath(MAX_PATH, szTemp);

    GUID gid;
    constexpr int kGuidStrWchars = 64;   // 64 缓冲尺寸（语义命名，数值不变）
    wchar_t szGid[kGuidStrWchars] = { 0 };
    if (::CoCreateGuid(&gid) == S_OK)
        ::StringFromGUID2(gid, szGid, _countof(szGid)); // {XXXXXXXX....}

    CDuiString sGid(szGid);
    sGid.Replace(L"{", L"");
    sGid.Replace(L"}", L"");

    CDuiString sDir;
    sDir.Format(L"%s%s_%s", szTemp, pszPrefix, sGid.GetData());
    ::CreateDirectory(sDir, nullptr);
    return sDir;
}

// ============================================================
// [P0 2026-10-07] WinPE / 固件检测 + 特权提升
// ============================================================

// 三重判定（任一命中即按 WinPE 处理）：
//   1) HKLM\SYSTEM\CCS\Control\MiniNT 键存在（微软文档化的 WinPE 标志）
//   2) HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\WinPE 版本键
//   3) 系统目录位于 X:（WinPE 标准 RAM 盘）
bool SystemInfoService::IsWinPE()
{
    SISRegGuard hKey;
    bool bMini = false, bVer = false;

    if (::RegOpenKeyEx(HKEY_LOCAL_MACHINE,
            L"SYSTEM\\CurrentControlSet\\Control\\MiniNT",
            0, KEY_READ, hKey.out()) == ERROR_SUCCESS)
    {
        bMini = true;
    }

    hKey.reset();  // 守卫复位：复用同一守卫前先关旧键
    if (::RegOpenKeyEx(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\WinPE",
            0, KEY_READ | KEY_WOW64_64KEY, hKey.out()) == ERROR_SUCCESS)
    {
        bVer = true;
    }

    wchar_t szSys[MAX_PATH] = { 0 };
    bool bRam = false;
    if (::GetSystemDirectory(szSys, MAX_PATH) > 0)
        bRam = (szSys[0] == L'X' || szSys[0] == L'x');

    return bMini || bVer || bRam;
}

// 固件变量探测：对空变量名调用 GetFirmwareEnvironmentVariableW ——
//   ERROR_INVALID_FUNCTION(1)  = Legacy BIOS（固件不支持）
//   其它（5 拒绝访问 / 87 参数错误）= UEFI
// 这与 bcdboot / 微软文档建议的判定方式一致。
bool SystemInfoService::IsUEFIBoot()
{
    ::GetFirmwareEnvironmentVariableW(
        L"", L"{00000000-0000-0000-0000-000000000000}",
        nullptr, 0);
    return (::GetLastError() != ERROR_INVALID_FUNCTION);
}

static bool EnableOnePrivilege(HANDLE hToken, const wchar_t* pszPriv, const wchar_t* pszNameForLog = nullptr)

{
    LUID luid = {};

    if (!::LookupPrivilegeValue(nullptr, pszPriv, &luid))
    {
        if (pszNameForLog)
            LogEnvLine(L"[env] LookupPrivilegeValue(%s) failed: %lu\n",
                       pszNameForLog, (unsigned long)::GetLastError());
        return false;
    }

    TOKEN_PRIVILEGES tp = {};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    // [修复 PRIV-CHECK] 经典陷阱：AdjustTokenPrivileges 返回 TRUE 表示
    // 调用本身成功，但【不保证重置 GetLastError()】——成功路径上
    // GetLastError 可能残留此前任意调用留下的 stale 错误码，导致旧实现
    // `GetLastError()==ERROR_SUCCESS` 判假（每次启动误报
    // "required privileges not fully available"）。正确判据：
    //   返回 FALSE          → 硬失败（权限/句柄问题）
    //   ERROR_NOT_ALL_ASSIGNED → 令牌不含该特权（未提权或域控过滤）
    // 其余（含 stale 码）都视为成功。
    ::SetLastError(ERROR_SUCCESS);
    if (!::AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr))
    {
        if (pszNameForLog)
            LogEnvLine(L"[env] AdjustTokenPrivileges(%s) failed: %lu\n",
                       pszNameForLog, (unsigned long)::GetLastError());
        return false;
    }

    DWORD dwErr = ::GetLastError();
    if (dwErr == ERROR_NOT_ALL_ASSIGNED)
    {
        if (pszNameForLog)
            LogEnvLine(L"[env] token lacks privilege %s (not assigned)\n",
                       pszNameForLog);
        return false;
    }
    return true;
}

// manifest 已是 requireAdministrator，但管理员令牌默认不启用
// SeBackup/SeRestore/SeSecurity；VSS 备份、注册表蜂巢加载、服务 SCM
// 全权限操作都要求它们在令牌内显式启用。
bool SystemInfoService::EnableSystemPrivileges()
{
    SISFileGuard hToken;
    if (!::OpenProcessToken(::GetCurrentProcess(),
            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, hToken.out()))
        return false;

    bool bBak = EnableOnePrivilege(hToken, SE_BACKUP_NAME,    L"SeBackupPrivilege");
    bool bRes = EnableOnePrivilege(hToken, SE_RESTORE_NAME,   L"SeRestorePrivilege");
    bool bSec = EnableOnePrivilege(hToken, SE_SECURITY_NAME,  L"SeSecurityPrivilege");
    // 以下为尽力启用（失败不影响结论）
    EnableOnePrivilege(hToken, SE_LOAD_DRIVER_NAME,   L"SeLoadDriverPrivilege");
    EnableOnePrivilege(hToken, SE_SHUTDOWN_NAME,      L"SeShutdownPrivilege");
    EnableOnePrivilege(hToken, SE_TAKE_OWNERSHIP_NAME,L"SeTakeOwnershipPrivilege");
    EnableOnePrivilege(hToken, SE_DEBUG_NAME,         L"SeDebugPrivilege");

    LogEnvLine(L"[env] EnableSystemPrivileges result: bak=%d res=%d sec=%d\n",
               bBak ? 1 : 0, bRes ? 1 : 0, bSec ? 1 : 0);
    return bBak && bRes && bSec;
}

// ============================================================
// [P0 2026-10-07] WinPE 离线目标发现 / 全局目标根
// ============================================================
namespace {
    CDuiString g_sOfflineTarget;   // 例 "D:\" 或 "D:\Mount\Win"；空=在线模式

    bool IsWindowsInstallOn(const wchar_t* pszRoot)
    {
        // 一个"已安装 Windows"卷：Windows\System32\config\SYSTEM 蜂巢 +
        // Windows\explorer.exe（区分 WinSxS 残片/裸数据盘）。
        const wchar_t* kMustExist[] = {
            L"Windows\\System32\\config\\SYSTEM",
            L"Windows\\System32\\ntoskrnl.exe",
        };
        for (int i = 0; i < 2; ++i)
        {
            wchar_t szP[MAX_PATH] = { 0 };
            wfmt::FormatInto(szP, _countof(szP), L"%s%s", pszRoot, kMustExist[i]);
            if (::GetFileAttributes(szP) == INVALID_FILE_ATTRIBUTES)
                return false;
        }
        return true;
    }
}

CDuiString SystemInfoService::FindOfflineWindowsDir()
{
    // 非 PE：目标就是当前在线系统（返回系统盘根，调用方按在线路径处理，
    // 通常不使用本接口）。
    wchar_t szWin[MAX_PATH] = { 0 };
    ::GetWindowsDirectory(szWin, MAX_PATH);
    wchar_t cOnline = szWin[0];

    CDuiString sFirst;
    DWORD dwMask = ::GetLogicalDrives();
    for (int i = 0; i < 26; ++i)
    {
        if (!(dwMask & (1u << i))) continue;
        wchar_t cDrv = (wchar_t)(L'A' + i);
        if (cDrv == cOnline) continue;            // 当前运行系统卷（PE 下即 X:）
        std::wstring sRoot;
        sRoot += cDrv;
        sRoot += L":\\";
        if (::GetDriveType(sRoot.c_str()) != DRIVE_FIXED) continue;
        if (IsWindowsInstallOn(sRoot.c_str()))
        {
            // 固定按 C→Z 顺序取第一个；多系统并存罕见，日志中由调用方提示。
            sFirst = sRoot.c_str();
            break;
        }
    }
    return sFirst;
}

void SystemInfoService::SetOfflineTarget(const wchar_t* pszRoot)
{
    g_sOfflineTarget = pszRoot ? pszRoot : L"";
    // 统一去尾部反斜杠（保留 "D:" 形态根？规则：保留形如 "D:\" 的盘符根，
    // 深层挂载目录去尾斜杠），便于拼接 "\\Windows"。
    if (g_sOfflineTarget.GetLength() > 3
        && g_sOfflineTarget[g_sOfflineTarget.GetLength() - 1] == L'\\')
    {
        g_sOfflineTarget = g_sOfflineTarget.Mid(0, g_sOfflineTarget.GetLength() - 1);
    }
}

CDuiString SystemInfoService::GetOfflineTarget()
{
    return g_sOfflineTarget;
}

bool SystemInfoService::HasOfflineTarget()
{
    return !g_sOfflineTarget.IsEmpty();
}
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
