// ============================================================
// WelcomeService.cpp —— [R95] Welcome 首页系统状态采集实现
// 纯 Win32：RtlGetVersion + GetDiskFreeSpaceExW + GlobalMemoryStatusEx
//           + SHQueryUserNotificationState
// 实现文件不使用预编译头、不 include UIlib.h / StringManager.h。
// 行尾 CRLF。
// ============================================================

#include "WelcomeService.h"
#include "SystemInfoService.h"
#include "WinHandles.h"   // [H-51] win::RegKey RAII for F11_raw_handle_close

#include <shellapi.h>   // SHQueryUserNotificationState / QUERY_USER_NOTIFICATION_STATE

#include <windowsx.h>   // GET_X_LPARAM（若 UI 侧使用）
#include <synchapi.h>   // SRWLOCK（CPU 差分采集线程安全）
#include <mutex>

// CPU 差分采集：WelcomeService 单例静态状态（进程内，非线程安全问题）
namespace WelcomeCpuMeter
{
    // 上次采样基准
    ULONGLONG g_lastIdle = 0, g_lastKernel = 0, g_lastUser = 0;
    DWORD     g_lastCpu = 0;
    std::mutex g_mtx;

    // 把 FILETIME 转成 100ns 计数
    static ULONGLONG FileTimeToQpc(const FILETIME& ft)
    {
        ULARGE_INTEGER ul; ul.LowPart = ft.dwLowDateTime; ul.HighPart = ft.dwHighDateTime;
        return ul.QuadPart;
    }

    // 执行一次双采样差分（首次间隔 250ms），返回 0-100 整数 CPU 占用率
    DWORD SampleCpuPercent()
    {
        FILETIME ftIdle, ftKernel, ftUser;
        ULONGLONG idle = 0, kern = 0, user = 0;
        bool bFirst = false;

        {
            std::lock_guard<std::mutex> lk(g_mtx);
            bFirst = (g_lastIdle == 0);
        }

        if (!::GetSystemTimes(&ftIdle, &ftKernel, &ftUser))
            return 0;
        idle = FileTimeToQpc(ftIdle);
        kern = FileTimeToQpc(ftKernel);
        user = FileTimeToQpc(ftUser);

        if (bFirst)
        {
            ::Sleep(250);
            if (!::GetSystemTimes(&ftIdle, &ftKernel, &ftUser))
                return 0;
            idle = FileTimeToQpc(ftIdle);
            kern = FileTimeToQpc(ftKernel);
            user = FileTimeToQpc(ftUser);
        }

        ULONGLONG dIdle = 0, dKern = 0, dUser = 0, busy = 0, total = 0;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            dIdle = (idle > g_lastIdle)   ? (idle - g_lastIdle)   : 0;
            dKern = (kern > g_lastKernel) ? (kern - g_lastKernel) : 0;
            dUser = (user > g_lastUser)   ? (user - g_lastUser)   : 0;
            g_lastIdle = idle; g_lastKernel = kern; g_lastUser = user;

            total = dKern + dUser;         // kernel 已含 idle
            busy  = (total > dIdle) ? (total - dIdle) : 0;
            DWORD cpu = (total > 0) ? static_cast<DWORD>((busy * 100ULL) / total) : 0;
            if (cpu > 100) cpu = 100;
            g_lastCpu = cpu;
            return cpu;
        }
    }
}

// ============================================================
// 系统概览
// ============================================================
bool WelcomeService::GetSystemOverview(WelcomeOverview& out, DWORD& dwErr)
{
    dwErr = 0;
    out = WelcomeOverview();

    // [R124-3] 分级失败收集：把各 API 的失败单独记录，UI 侧可精确提示
    auto addFail = [&out](const wchar_t* pszApi, DWORD e)
    {
        if (!pszApi) return;
        DuiLib::CDuiString sItem;
        sItem.Format(L"%s(err=0x%08X)", pszApi, (unsigned)e);
        if (out.sFailList.IsEmpty())
            out.sFailList = sItem;
        else
        {
            out.sFailList += L"; ";
            out.sFailList += sItem;
        }
        ++out.nFailCount;
    };

    // OS 名称与构建号：RtlGetVersion 动态加载避免 manifest 依赖
    HMODULE hNtdll = ::GetModuleHandleW(L"ntdll.dll");
    using RtlGetVersionFn = LONG ( WINAPI*)(PRTL_OSVERSIONINFOW);
    RtlGetVersionFn pRtlGetVersion =
        hNtdll ? (RtlGetVersionFn)::GetProcAddress(hNtdll, "RtlGetVersion") : nullptr;
    if (pRtlGetVersion)
    {
        RTL_OSVERSIONINFOW osi = { sizeof(osi) };
        LONG lOs = pRtlGetVersion(&osi);
        if (lOs == 0)
        {
            out.dwBuild = osi.dwBuildNumber;
            const wchar_t* pszName = nullptr;
            if (osi.dwMajorVersion >= 10)
                pszName = (osi.dwBuildNumber >= 22000) ? L"Windows 11" : L"Windows 10";
            else if (osi.dwMajorVersion == 6 && osi.dwMinorVersion == 3)
                pszName = L"Windows 8.1";
            else if (osi.dwMajorVersion == 6 && osi.dwMinorVersion == 2)
                pszName = L"Windows 8";
            else if (osi.dwMajorVersion == 6 && osi.dwMinorVersion == 1)
                pszName = L"Windows 7";
            else
                pszName = L"Windows";

            DuiLib::CDuiString sEdition;
            win::RegKey hKey;
            if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                    0, KEY_READ | KEY_WOW64_64KEY, hKey.out()) == ERROR_SUCCESS)
            {
                WCHAR szBuf[128] = { 0 };
                DWORD dwSize = sizeof(szBuf);
                DWORD dwType = 0;
                if (::RegQueryValueExW(hKey.get(), L"ProductName", nullptr, &dwType,
                        reinterpret_cast<BYTE*>(szBuf), &dwSize) == ERROR_SUCCESS && dwType == REG_SZ)
                {
                    sEdition = szBuf;
                }
                else
                {
                    addFail(L"RegQueryValueExW(ProductName)", ::GetLastError());
                }

            }
            else
            {
                addFail(L"RegOpenKeyExW(ProductName)", ::GetLastError());
            }
            if (sEdition.IsEmpty()) sEdition = pszName;

            DuiLib::CDuiString sBld;
            sBld.Format(L"%s (Build %lu)", sEdition.GetData(),
                        (unsigned long)osi.dwBuildNumber);
            out.sOsName = sBld;
        }
        else
        {
            addFail(L"RtlGetVersion", static_cast<DWORD>(lOs));
        }
    }
    else
    {
        addFail(L"GetProcAddress(RtlGetVersion)", ERROR_PROC_NOT_FOUND);
    }

    // 系统盘
    out.sSystemDrive = SystemInfoService::GetSystemDriveStr();
    if (!out.sSystemDrive.IsEmpty())
    {
        DuiLib::CDuiString sRoot = out.sSystemDrive;
        if (sRoot[sRoot.GetLength() - 1] != L'\\') sRoot += L"\\";

        ULARGE_INTEGER ulFree = { 0 }, ulTotal = { 0 }, ulAvail = { 0 };
        if (::GetDiskFreeSpaceExW(sRoot.GetData(), &ulAvail, &ulTotal, &ulFree))
        {
            out.ullFreeMB = ulFree.QuadPart / (1024 * 1024);
            out.ullTotalMB = ulTotal.QuadPart / (1024 * 1024);
        }
        else
        {
            addFail(L"GetDiskFreeSpaceExW", ::GetLastError());
        }
    }
    else
    {
        addFail(L"SystemInfoService::GetSystemDriveStr", ERROR_NOT_FOUND);
    }

    // 内存
    MEMORYSTATUSEX msx = { sizeof(msx) };
    if (::GlobalMemoryStatusEx(&msx))
    {
        out.ullMemTotalMB = msx.ullTotalPhys / (1024 * 1024);
        out.ullMemFreeMB = msx.ullAvailPhys / (1024 * 1024);
    }
    else
    {
        addFail(L"GlobalMemoryStatusEx", ::GetLastError());
    }

    // [R100] 系统运行时长（GetTickCount64）—— 该 API 永不失败，仅记录 0 值
    ULONGLONG ullTicks = ::GetTickCount64();
    out.ullUptimeSec = ullTicks / 1000ULL;
    if (ullTicks == 0)
        addFail(L"GetTickCount64", ERROR_GEN_FAILURE);

    // [R100] 启动时间（本地时间，Unix epoch 秒 = 现在 - 已运行时长）
    {
        FILETIME ftNow, ftLocal;
        ::GetSystemTimeAsFileTime(&ftNow);
        ::FileTimeToLocalFileTime(&ftNow, &ftLocal);
        ULARGE_INTEGER ulNow;
        ulNow.LowPart = ftLocal.dwLowDateTime;
        ulNow.HighPart = ftLocal.dwHighDateTime;
        // 1601-01-01 → 1970-01-01 = 11644473600 秒
        ULONGLONG ullNowSec = ulNow.QuadPart / 10000000ULL;
        if (ullNowSec > 11644473600ULL)
            ullNowSec -= 11644473600ULL;
        out.ullBootTime = (ullNowSec > out.ullUptimeSec)
                          ? (ullNowSec - out.ullUptimeSec) : ullNowSec;
    }

    // [R100] CPU 占用率（GetSystemTimes 差分）
    out.dwCpuPercent = WelcomeCpuMeter::SampleCpuPercent();

    // [R116-1] 磁盘分区详情：GetLogicalDriveStringsW + GetVolumeInformationW
    out.nDriveCount = 0;
    {
        WCHAR szDrives[512] = { 0 };
        UINT uLen = ::GetLogicalDriveStringsW(512, szDrives);
        if (uLen > 0 && uLen < 512)
        {
            wchar_t* p = szDrives;
            while (*p)
            {
                UINT uType = ::GetDriveTypeW(p);
                if (uType == DRIVE_FIXED || uType == DRIVE_REMOVABLE)
                {
                    if (out.nDriveCount < 16)
                    {
                        if (uType == DRIVE_FIXED) out.dwDriveTypes |= (1u << out.nDriveCount);
                        out.sDriveList += p;
                        out.sDriveList += L"|";
                        ++out.nDriveCount;
                    }
                }
                p += wcslen(p) + 1;
            }
        }
        else
        {
            addFail(L"GetLogicalDriveStringsW", ::GetLastError());
        }
    }

    // [R116-1] 启动模式：GetSystemFirmwareTable('RSMB') 探测 UEFI
    out.sBootMode = L"";
    {
        DWORD dwSize = ::GetSystemFirmwareTable('RSMB', 0, nullptr, 0);
        out.sBootMode = (dwSize > 0) ? L"UEFI" : L"Legacy BIOS";
    }

    // [R116-1] 架构：GetNativeSystemInfo（无失败返回，但 wProcessorArchitecture 可能为未知）
    out.sArchitecture = L"";
    {
        SYSTEM_INFO si;
        ::GetNativeSystemInfo(&si);
        switch (si.wProcessorArchitecture)
        {
        case PROCESSOR_ARCHITECTURE_AMD64:  out.sArchitecture = L"x64"; break;
        case PROCESSOR_ARCHITECTURE_INTEL:  out.sArchitecture = L"x86"; break;
        case PROCESSOR_ARCHITECTURE_ARM64:  out.sArchitecture = L"ARM64"; break;
        case PROCESSOR_ARCHITECTURE_ARM:    out.sArchitecture = L"ARM"; break;
        case PROCESSOR_ARCHITECTURE_UNKNOWN:
            addFail(L"GetNativeSystemInfo(ArchUnknown)", ERROR_UNSUPPORTED_TYPE);
            out.sArchitecture = L"未知";
            break;
        default:
            out.sArchitecture = L"未知";
            break;
        }
    }

    // [R116-1] 系统版本完整字段（RtlGetVersion 扩展）
    out.dwMajorVer = 0;
    out.dwMinorVer = 0;
    {
        HMODULE hN2 = ::GetModuleHandleW(L"ntdll.dll");
        using RtlGetVersionFn = LONG ( WINAPI*)(PRTL_OSVERSIONINFOW);
        RtlGetVersionFn pV = hN2 ? (RtlGetVersionFn)::GetProcAddress(hN2, "RtlGetVersion") : nullptr;
        if (pV)
        {
            RTL_OSVERSIONINFOW osi2 = { sizeof(osi2) };
            LONG lV = pV(&osi2);
            if (lV == 0)
            {
                out.dwMajorVer = osi2.dwMajorVersion;
                out.dwMinorVer = osi2.dwMinorVersion;
            }
            else
            {
                addFail(L"RtlGetVersion(ver)", static_cast<DWORD>(lV));
            }
        }
        else
        {
            addFail(L"GetProcAddress(RtlGetVersion ver)", ERROR_PROC_NOT_FOUND);
        }
    }

    // [R124-3] 任一 API 失败时把首错码回传 dwErr（供上层快速判断）
    if (out.nFailCount > 0)
    {
        // 从 sFailList 首段解析 err= 十六进制
        int i = out.sFailList.Find(L"(err=0x");
        if (i >= 0)
        {
            const wchar_t* pszHex = out.sFailList.GetData() + i + 7;
            DWORD e = static_cast<DWORD>(wcstoul(pszHex, nullptr, 16));
            if (e != 0) dwErr = e;
            else dwErr = ERROR_GEN_FAILURE;
        }
        else
        {
            dwErr = ERROR_GEN_FAILURE;
        }
    }

    return true;
}

// ============================================================
// SHQueryUserNotificationState
// ============================================================
bool WelcomeService::GetUserNotificationState(int& nState, DWORD& dwErr)
{
    dwErr = 0;
    nState = -1;
    QUERY_USER_NOTIFICATION_STATE quns = QUNS_NOT_PRESENT;
    HRESULT hr = ::SHQueryUserNotificationState(&quns);
    if (hr == S_OK)
    {
        nState = static_cast<int>(quns);
        return true;
    }
    // [R124-3] SHQueryUserNotificationState 返回 HRESULT（32 位），
    // 失败时回传完整 HRESULT 供 UI 精确提示
    dwErr = static_cast<DWORD>(static_cast<ULONG_PTR>(hr));
    return false;
}

// ============================================================
// 状态字符串映射
// ============================================================
DuiLib::CDuiString WelcomeService::UserStateToString(int nState)
{
    DuiLib::CDuiString s;
    switch (nState)
    {
    case QUNS_NOT_PRESENT:             s = L"用户不在场"; break;
    case QUNS_BUSY:                    s = L"用户忙碌"; break;
    case QUNS_RUNNING_D3D_FULL_SCREEN: s = L"全屏应用运行中"; break;
    case QUNS_PRESENTATION_MODE:       s = L"演示模式"; break;
    case QUNS_ACCEPTS_NOTIFICATIONS:   s = L"可接收通知"; break;
    case QUNS_QUIET_TIME:              s = L"安静时段"; break;
    case QUNS_APP:                     s = L"应用模式"; break;
    default:                           s.Format(L"未知(%d)", nState); break;
    }
    return s;
}
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：

