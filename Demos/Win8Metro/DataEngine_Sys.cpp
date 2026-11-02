#include "stdafx.h"
#include "DataEngine.h"


#include <shellapi.h>   // SHChangeNotify
#include <winsvc.h>     // [R9-03b] Service Start/Recovery 原语
#include <objbase.h>    // [R9-03b] 电源方案 IIDFromString/StringFromGUID2
#include "AppxRuntimeBridge.h"   // [P0-A1] SetAppxPackage 原生 WinRT 安装/卸载
#include "DriverService.h"       // [P1-A6] Rescan：cfgmgr32 原生重扫
#include "DismApiBridge.h"      // [P1-收尾] AddDriver/AddPackage 进程内 dismapi
// [C++17 阶段2] 局部 RAII 守卫：注册表键（见 WinHandles.h 使用规范）
struct DESRegGuard
{
    HKEY k = nullptr;
    ~DESRegGuard() { if (k) ::RegCloseKey(k); }
    operator HKEY() const { return k; }
};

// ============================================================
// DataEngine_Sys.cpp —— [R9-02] 系统/服务/账户执行器。
// SetServiceStart(含 SCConfig 别名)/ExplorerNotify/SetServiceRecovery/
// SetAppxPackage/SetPowerPlan/AddDriver/AddPackage/Rescan
// 动作处理器从 DataEngine.cpp ExecActivate if 链拆出，逻辑零改动。
// ============================================================

CDataEngine::EActResult CDataEngine::ActSetServiceStart(CMarkupNode node, TActCtx&)
{
    // [R-B40] SCConfig 别名：StartType 作为 Type 的 fallback，缺省 3（手动）。
    const wchar_t* pszSvcName = node.GetAttributeValue(L"Name");
    const wchar_t* pszType = node.GetAttributeValue(L"Type");
    if ((!pszType || !*pszType) && node.HasAttribute(L"StartType"))
        pszType = node.GetAttributeValue(L"StartType");
    int nStartType = (pszType && *pszType) ? _ttoi(pszType) : 3;
    return SetServiceStart(pszSvcName, nStartType) ? EActResult::ACT_OK : EActResult::ACT_FAIL;
}

CDataEngine::EActResult CDataEngine::ActExplorerNotify(CMarkupNode node, TActCtx&)
{
    const wchar_t* pszNType = node.GetAttributeValue(L"Type");

    if (lstrcmpiW(pszNType, L"AssocChanged") == 0)
    {
        // file association / icon change -> notify and refresh desktop icons
        ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        RefreshDesktopIcons();
        return EActResult::ACT_OK;
    }
    if (lstrcmpiW(pszNType, L"Custom") != 0)
    {
        // Type="Cmd" 为显式拒绝（见下）；其余未知 Type 中性忽略（旧版行为）。
        if (lstrcmpiW(pszNType, L"Cmd") == 0)
            return EActResult::ACT_FAIL;   // 零外部进程：绝不隐式拉起命令行，按失败记账
        return EActResult::ACT_SKIP;
    }

    const wchar_t* pszMsg = node.GetAttributeValue(L"msg");
    if (pszMsg && lstrcmpW(pszMsg, L"1A") == 0)
    {
        // taskbar / shell settings: broadcast WM_SETTINGCHANGE
        const wchar_t* pszL = node.GetAttributeValue(L"lParam");
        DWORD_PTR dwpRes = 0;
        ::SendMessageTimeout(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                             reinterpret_cast<LPARAM>(pszL), SMTO_ABORTIFHUNG, 1000, &dwpRes);
        // [智能重启] Policy="True"：本次变更含 HKLM\...\Policies 策略键
        // （如 24H2+ 搜索框 SearchOnTaskbarMode）。额外广播 lParam="Policy"，
        // 驱动会缓存策略的 shell 组件即时重读，best-effort，不阻塞、不报错。
        if (node.HasAttribute(L"Policy")
            && lstrcmpiW(node.GetAttributeValue(L"Policy"), L"True") == 0)
        {
            DWORD_PTR dwpPolicy = 0;
            ::SendMessageTimeout(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                                 reinterpret_cast<LPARAM>(L"Policy"), SMTO_ABORTIFHUNG,
                                 1000, &dwpPolicy);
        }
        // [Win11 兜底] TraySettings 广播在 Win11 上经常被 explorer 忽略，
        // 定向给常见宿主窗口再送一次，保证任务栏立即刷新（全部 Post 不阻塞）。
        struct { const wchar_t* cls; const wchar_t* cap; } targets[] = {
            { L"Shell_TrayWnd",   nullptr },
            { L"Button",          L"Start" },
            { L"WorkerW",         nullptr },
            { L"Shell_SecondaryTrayWnd", nullptr },  // Win11 多显示器
        };
        for (auto& t : targets)
        {
            HWND h = ::FindWindowW(t.cls, t.cap);
            if (h) ::PostMessageW(h, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(pszL));
        }
        return EActResult::ACT_OK;
    }
    if (pszMsg && lstrcmpW(pszMsg, L"111") == 0)
    {
        // custom taskbar message with hex wParam (e.g. "#A220" / "A220")
        const wchar_t* pszW = node.GetAttributeValue(L"wParam");
        if (pszW && *pszW == L'#') ++pszW;   // 去掉 Dism++ 的 0x 风格前缀
        WPARAM wp = static_cast<WPARAM>(wcstoul(pszW ? pszW : L"0", nullptr, 16));
        DWORD_PTR dwpRes2 = 0;
        ::SendMessageTimeout(HWND_BROADCAST, 0x111, wp, 0,
                             SMTO_ABORTIFHUNG, 1000, &dwpRes2);
        // Explorer="True"（Data.xml 全部 wParam=A220）：额外把 WM_COMMAND
        // 投给资源管理器/任务栏，任务栏才会即时刷新（PostMessage，best-effort）。
        if (node.HasAttribute(L"Explorer")
            && lstrcmpiW(node.GetAttributeValue(L"Explorer"), L"True") == 0)
        {
            HWND hTray = ::FindWindow(L"Shell_TrayWnd", nullptr);
            HWND hProg = ::FindWindow(L"Progman", nullptr);
            if (hTray) ::PostMessage(hTray, WM_COMMAND, wp, 0);
            if (hProg) ::PostMessage(hProg, WM_COMMAND, wp, 0);
        }
        return EActResult::ACT_OK;
    }
    return EActResult::ACT_SKIP;
}

CDataEngine::EActResult CDataEngine::ActSetServiceRecovery(CMarkupNode node, TActCtx&)
{
    // [R17 / P1-5] 设置服务失败后的恢复策略
    // Action=0/1/2/3；Delay 毫秒或秒（>60000 按毫秒换算）。
    const wchar_t* pszName = node.GetAttributeValue(L"Name");
    int nAction = _ttoi(node.GetAttributeValue(L"Action"));
    const wchar_t* pszDelay = node.GetAttributeValue(L"Delay");
    int nDelaySec = 0;
    if (pszDelay)
    {
        LONGLONG l = _ttoi64(pszDelay);   // [B6] _ttoi64 返回 __int64，存 LONG 触发 C4244
        nDelaySec = (l > 60000) ? static_cast<int>(l / 1000) : static_cast<int>(l);
    }
    return SetServiceRecovery(pszName, nAction, nDelaySec) ? EActResult::ACT_OK : EActResult::ACT_FAIL;
}

CDataEngine::EActResult CDataEngine::ActSetAppxPackage(CMarkupNode node, TActCtx&)
{
    // [R17 / P1-6] 安装/卸载 AppX 包（Install="True"=安装，极性已修正）
    const wchar_t* pszPath = node.GetAttributeValue(L"Path");
    bool bInstall = true;
    if (node.HasAttribute(L"Install"))
        bInstall = lstrcmpiW(node.GetAttributeValue(L"Install"), L"True") == 0;
    return DoSetAppxPackage(pszPath, bInstall) ? EActResult::ACT_OK : EActResult::ACT_FAIL;
}

CDataEngine::EActResult CDataEngine::ActSetPowerPlan(CMarkupNode node, TActCtx&)
{
    // [P0-A7] 电源计划激活（powrprof 原生）；Ultimate 首次自动复制隐藏模板。
    const wchar_t* pszGuid = node.GetAttributeValue(L"Guid");
    bool bUltimate = node.HasAttribute(L"Ultimate")
        && lstrcmpiW(node.GetAttributeValue(L"Ultimate"), L"True") == 0;
    return SetActivePowerPlan(pszGuid, bUltimate) ? EActResult::ACT_OK : EActResult::ACT_FAIL;
}

CDataEngine::EActResult CDataEngine::ActAddDriver(CMarkupNode node, TActCtx& ctx)
{
    // [R19/A3][P0] 通过 DISM 添加驱动；WinPE 离线会话走 /Image 目标根。
    const wchar_t* pszPath = node.GetAttributeValue(L"Path");
    if (!pszPath || !*pszPath) { ctx.Skip(); return EActResult::ACT_SKIP; }
    CDuiString sDrv = ResolveExeRelative(pszPath, L"\\Drivers\\");
    bool bForceUnsigned = node.HasAttribute(L"Force")
        && lstrcmpiW(node.GetAttributeValue(L"Force"), L"True") == 0;
    // [P1-收尾] 零 dism.exe：DismAddDriver 进程内直连（在线/离线会话）
    std::wstring sDismErr;
    DismApiBridge::Init();
    const wchar_t* pszImg = IsOfflineSession()
        ? GetOfflineImageRoot().GetData() : nullptr;
    bool bAct = DismApiBridge::AddDriver(sDrv.GetData(), &sDismErr,
                                         pszImg, bForceUnsigned);
    return bAct ? EActResult::ACT_OK : EActResult::ACT_FAIL;
}

CDataEngine::EActResult CDataEngine::ActAddPackage(CMarkupNode node, TActCtx& ctx)
{
    // [R-B74] 添加 cabinet/msu 包，等价 dism /Add-Package。
    const wchar_t* pszPath = node.HasAttribute(L"Path")
                    ? node.GetAttributeValue(L"Path")
                    : node.GetAttributeValue(L"PackagePath");
    if (!pszPath || !*pszPath) { ctx.Skip(); return EActResult::ACT_SKIP; }
    CDuiString sPkg = ResolveExeRelative(pszPath, L"\\Packages\\");
    // [P1-收尾] 零 dism.exe：DismAddPackage 进程内直连（在线/离线会话）
    std::wstring sPkgErr;
    DismApiBridge::Init();
    const wchar_t* pszPkgImg = IsOfflineSession()
        ? GetOfflineImageRoot().GetData() : nullptr;
    bool bAct = DismApiBridge::AddPackage(sPkg.GetData(), &sPkgErr, pszPkgImg);
    return bAct ? EActResult::ACT_OK : EActResult::ACT_FAIL;
}

CDataEngine::EActResult CDataEngine::ActRescan(CMarkupNode, TActCtx&)
{
    // [R-B74][P1-A6] cfgmgr32!CM_Reenumerate_DevNode 根节点同步重扫，零 pnputil。
    return (DriverService::ScanDevicesNative() == 0) ? EActResult::ACT_OK : EActResult::ACT_FAIL;
}

// ============================================================
// [R9-03b] SetServiceRecovery / DoSetAppxPackage（逐字搬迁自 DataEngine.cpp）
// ============================================================

bool CDataEngine::SetServiceRecovery(const wchar_t* pszServiceName,
                                     int nRecoveryAction, int nDelaySeconds)
{
    if (!pszServiceName || !*pszServiceName) return false;

    SC_HANDLE hSCM = ::OpenSCManager(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!hSCM) return false;
    SC_HANDLE hSvc = ::OpenService(hSCM, pszServiceName,
                                   SERVICE_CHANGE_CONFIG);
    if (!hSvc) { ::CloseServiceHandle(hSCM); return false; }

    // 映射到 SC_ACTION
    DWORD dwActionType = 0;   // SERVICE_NO_ACTION
    switch (nRecoveryAction)
    {
        case 0: dwActionType = 0;   // SERVICE_NO_ACTION
            break;
        case 1: dwActionType = 1;   // SERVICE_REBOOT
            break;
        case 2: dwActionType = 2;   // SERVICE_RESTART
            break;
        case 3: dwActionType = 0;
            break;
        default: dwActionType = 0;
            break;
    }

    // 手工定义 SC_ACTION 与 SC_RECOVERY（winsvc.h 版本差异兜底）
    using SC_ACTION_LOCAL = struct _SC_ACTION_LOCAL {
        DWORD  dwType;
        DWORD  dwDelay;
    };
    using SC_RECOVERY_LOCAL = struct _SC_RECOVERY_LOCAL {
        SC_ACTION_LOCAL* lpRecoveryActions;
        DWORD            dwResetPeriod;
        DWORD            dwRecoveryActions;
    };

    SC_ACTION_LOCAL act;
    act.dwType  = dwActionType;
    act.dwDelay = static_cast<DWORD>(nDelaySeconds) * 1000;
    SC_ACTION_LOCAL acts[3] = { act };

    SC_RECOVERY_LOCAL rec;
    rec.lpRecoveryActions = acts;
    rec.dwRecoveryActions = 1;
    rec.dwResetPeriod = 0;

    // ChangeServiceConfig2 在此 SDK 版本未导出，改用手写 LoadLibrary
    using PFN_ChangeServiceConfig2 = BOOL ( WINAPI*)(SC_HANDLE, DWORD, void*);
    static PFN_ChangeServiceConfig2 pfnChg = nullptr;
    if (!pfnChg)
    {
        HMODULE hSvc = ::GetModuleHandleW(L"advapi32.dll");
        if (!hSvc) hSvc = ::LoadLibraryW(L"advapi32.dll");
        if (hSvc)
            pfnChg = (PFN_ChangeServiceConfig2)::GetProcAddress(
                hSvc, "ChangeServiceConfig2W");
        if (!pfnChg)
            pfnChg = (PFN_ChangeServiceConfig2)::GetProcAddress(
                hSvc, "ChangeServiceConfig2A");
    }
    bool bOk = pfnChg && pfnChg(hSvc, 2 /* SERVICE_CONFIG_RECOVERY_INFO */, &rec) != FALSE;
    ::CloseServiceHandle(hSvc);
    ::CloseServiceHandle(hSCM);
    return bOk;
}

bool CDataEngine::DoSetAppxPackage(const wchar_t* pszPath, bool bInstall)
{
    if (!pszPath || !*pszPath) return false;

    CDuiString sPath(pszPath);

    // 相对路径 → 解析到 exe 同级 Packages\ 目录
    if (sPath.GetLength() > 0 && sPath[0] != L'\\'
        && (sPath.GetLength() < 2 || sPath[1] != L':'))
    {
        wchar_t szExe[MAX_PATH] = { 0 };
        ::GetModuleFileName(nullptr, szExe, MAX_PATH);
        CDuiString sDir(szExe);
        int p = sDir.ReverseFind(L'\\');
        if (p >= 0) sDir = sDir.Mid(0, p);
        sPath = sDir + L"\\Packages\\" + sPath;
    }

    // [P0-A1] 原生 WinRT PackageManager（旧 powershell.exe
    // Add-AppxPackage / Remove-AppxPackage -AllUsers 子进程已删除）
    std::wstring sErr;
    bool bOk = false;
    if (bInstall)
        bOk = AppxRuntimeBridge::AddPackage(sPath.GetData(), std::wstring(), &sErr);
    else
        bOk = AppxRuntimeBridge::RemovePackage(sPath.GetData(), &sErr);

    if (!bOk)
    {
        CDuiString sDbg;
        sDbg.Format(L"[SetAppxPackage] %s\n", sErr.c_str());
        ::OutputDebugStringW(sDbg.GetData());
    }
    return bOk;
}

// ============================================================
// [R9-03b] Service Start/Query + 电源方案原语（逐字搬迁自 DataEngine.cpp；
// g_bOffSess 文件静态量改用公开成员 IsOfflineSession()，语义等价）
// ============================================================

// ============================================================
// service helpers
// ============================================================
int CDataEngine::QueryServiceStart(const wchar_t* pszName)
{
    // [P0] 离线会话：SCM 只服务于运行中的 PE 内核，直接读挂载的 SYSTEM 蜂巢。
    // 规则层看到的 5=延迟自动：Start=2 且 DelayedAutostart=1。
    if (IsOfflineSession())
    {
        CDuiString sSvc;
        sSvc.Format(L"HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Services\\%s",
                    pszName ? pszName : L"");
        CDuiString sMap = RemapOfflinePath(sSvc, false);
        DESRegGuard hKey;
        if (!OpenRegKey(sMap, KEY_READ, hKey.k, false, false)) return -1;
        int nStart = -1;
        DWORD dwStart = 0, cb = sizeof(dwStart), dwType = 0;
        if (::RegQueryValueEx(hKey, L"Start", nullptr, &dwType,
                              (BYTE*)&dwStart, &cb) == ERROR_SUCCESS)
        {
            nStart = static_cast<int>(dwStart);
            if (nStart == 2)   // AUTO
            {
                DWORD dwDelay = 0; cb = sizeof(dwDelay);
                if (::RegQueryValueEx(hKey, L"DelayedAutostart", nullptr, &dwType,
                                      (BYTE*)&dwDelay, &cb) == ERROR_SUCCESS
                    && dwDelay == 1)
                    nStart = 5;
            }
        }
        return nStart;
    }

    SC_HANDLE hSCM = ::OpenSCManager(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hSCM) return -1;

    SC_HANDLE hSvc = ::OpenService(hSCM, pszName, SERVICE_QUERY_CONFIG);
    if (!hSvc) { ::CloseServiceHandle(hSCM); return -1; }

    DWORD dwNeed = 0;
    ::QueryServiceConfig(hSvc, nullptr, 0, &dwNeed);
    std::vector<BYTE> buf(dwNeed + 64, 0);
    QUERY_SERVICE_CONFIG* pCfg = (QUERY_SERVICE_CONFIG*)buf.data();
    int nStart = -1;
    if (::QueryServiceConfig(hSvc, pCfg, dwNeed + 64, &dwNeed))
        nStart = static_cast<int>(pCfg->dwStartType);

    ::CloseServiceHandle(hSvc);
    ::CloseServiceHandle(hSCM);
    return nStart;
}

bool CDataEngine::SetServiceStart(const wchar_t* pszName, int nStartType)
{
    // [P0] 离线会话：直写挂载 SYSTEM 蜂巢 Services\<name>，与 SCM 语义对齐
    // （5 延迟自动 => Start=2 + DelayedAutostart=1；恢复普通自动要清该标志）。
    if (IsOfflineSession())
    {
        if (!pszName || !*pszName) return false;
        CDuiString sSvc;
        sSvc.Format(L"HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Services\\%s", pszName);
        CDuiString sMap = RemapOfflinePath(sSvc, false);
        DESRegGuard hKey;
        if (!OpenRegKey(sMap, KEY_SET_VALUE, hKey.k, true, false)) return false;
        bool bDelayed = (nStartType == 5);
        DWORD dwStart = static_cast<DWORD>(nStartType);
        if (nStartType == 5) dwStart = 2;
        else if (nStartType != 0 && nStartType != 1 && nStartType != 2
            && nStartType != 3 && nStartType != 4)
            dwStart = 2;
        bool bOk = ::RegSetValueEx(hKey, L"Start", 0, REG_DWORD,
                                   (BYTE*)&dwStart, sizeof(dwStart)) == ERROR_SUCCESS;
        if (bOk)
        {
            DWORD dwDelay = bDelayed ? 1u : 0u;
            // [R128-A] 延迟启动标志与 Start 同属一次配置，写失败需计入整体成败
            if (::RegSetValueEx(hKey, L"DelayedAutostart", 0, REG_DWORD,
                            (BYTE*)&dwDelay, sizeof(dwDelay)) != ERROR_SUCCESS)
            {
                ::OutputDebugStringW(L"[R128][sys] DelayedAutostart 写入失败\r\n");
                bOk = false;
            }
        }
        return bOk;
    }

    SC_HANDLE hSCM = ::OpenSCManager(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hSCM) return false;

    SC_HANDLE hSvc = ::OpenService(hSCM, pszName, SERVICE_CHANGE_CONFIG);
    if (!hSvc) { ::CloseServiceHandle(hSCM); return false; }

    // 启动类型映射（KB 32 §3.3）：
    //   0 Boot / 1 System —— 驱动专用，SCM 的 ChangeServiceConfig 虽接受，
    //     但规则只用于服务，出现其它非法值时规范化为自动(2)。
    //   2 Automatic / 3 Manual / 4 Disabled
    //   5 Automatic (Delayed Start) —— SCM 没有直接的 dwStartType 取值，
    //     必须 ChangeServiceConfig(AUTO_START) 再用 ChangeServiceConfig2
    //     置 SERVICE_DELAYED_AUTO_START_INFO.fDelayed=TRUE。
    //   WSearch 恢复分支写 5，旧实现一律压成普通自动，丢失“延迟”语义。
    bool bDelayed = false;
    DWORD dwStartType = static_cast<DWORD>(nStartType);
    if (nStartType == 5)
    {
        dwStartType = SERVICE_AUTO_START;
        bDelayed = true;
    }
    else if (nStartType != SERVICE_AUTO_START
        && nStartType != SERVICE_DEMAND_START
        && nStartType != SERVICE_DISABLED
        && nStartType != SERVICE_BOOT_START
        && nStartType != SERVICE_SYSTEM_START)
    {
        dwStartType = SERVICE_AUTO_START;
    }

    BOOL b = ::ChangeServiceConfig(hSvc, SERVICE_NO_CHANGE, dwStartType,
                                   SERVICE_NO_CHANGE, nullptr, nullptr, nullptr,
                                   nullptr, nullptr, nullptr, nullptr);
    // 自动(2)与延迟自动(5)都要同步延迟标志，避免两者互相残留
    if (b && (dwStartType == SERVICE_AUTO_START))
    {
        SERVICE_DELAYED_AUTO_START_INFO d = { 0 };
        d.fDelayedAutostart = bDelayed ? TRUE : FALSE;
        ::ChangeServiceConfig2(hSvc, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &d);
    }
    ::CloseServiceHandle(hSvc);
    ::CloseServiceHandle(hSCM);
    return b != FALSE;
}

// ============================================================
// [P0-A7] 电源方案原语：powrprof.dll 动态绑定（替代 powercfg 进程）
// PowerGetActiveScheme 分配的 GUID 必须 LocalFree；
// PowerDuplicateScheme 允许调用方给定目标 GUID —— 卓越性能模板
// e9a42b02-... 首次使用时按固定 GUID 复制，复制后方案 GUID 不变，
// 与 Data.xml <PowerPlan Guid="e9a42b02-..."> 条件保持一致。
// ============================================================
namespace
{
    using PFN_PowerGetActiveScheme = DWORD ( WINAPI*)(HKEY hRoot, GUID** pGuid);
    using PFN_PowerSetActiveScheme = DWORD ( WINAPI*)(HKEY hRoot, const GUID* pGuid);
    using PFN_PowerDuplicateScheme = DWORD ( WINAPI*)(HKEY hRoot,
                                                     const GUID* pSrc,
                                                     GUID** pDst);

    struct PowrProfApi
    {
        HMODULE hMod;
        PFN_PowerGetActiveScheme pGetActive;
        PFN_PowerSetActiveScheme pSetActive;
        PFN_PowerDuplicateScheme pDuplicate;
        PowrProfApi()
            : hMod(nullptr), pGetActive(nullptr), pSetActive(nullptr), pDuplicate(nullptr)
        {
            hMod = ::LoadLibraryW(L"powrprof.dll");
            if (hMod)
            {
                pGetActive = reinterpret_cast<PFN_PowerGetActiveScheme>(
                    ::GetProcAddress(hMod, "PowerGetActiveScheme"));
                pSetActive = reinterpret_cast<PFN_PowerSetActiveScheme>(
                    ::GetProcAddress(hMod, "PowerSetActiveScheme"));
                pDuplicate = reinterpret_cast<PFN_PowerDuplicateScheme>(
                    ::GetProcAddress(hMod, "PowerDuplicateScheme"));
            }
        }
        ~PowrProfApi() { if (hMod) ::FreeLibrary(hMod); }
    };

    const wchar_t* const kUltimatePlanGuid =
        L"e9a42b02-d5df-448d-aa00-03f14749eb61";
}

bool CDataEngine::IsUltimatePowerPlan(const wchar_t* pszGuid)
{
    return pszGuid && lstrcmpiW(pszGuid, kUltimatePlanGuid) == 0;
}

bool CDataEngine::GetActivePowerPlan(CDuiString& strGuid)
{
    // [修复 P2-PWR] 离线会话从挂载 SYSTEM 蜂巢读取活动方案指针。
    // 实机权威存储为"无括号"REG_SZ（与 powercfg /getactivescheme 一致）。
    if (IsOfflineSession())
    {
        CDuiString sBase(
            L"HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\Power\\User\\PowerSchemes");
        CDuiString sMap = RemapOfflinePath(sBase, false);
        DESRegGuard hKey;
        if (!OpenRegKey(sMap, KEY_READ, hKey.k, false, false)) return false;
        WCHAR sz[64] = { 0 };
        DWORD dwType = 0, dwCb = sizeof(sz);
        LONG lRc = ::RegQueryValueExW(hKey, L"ActivePowerScheme", nullptr, &dwType,
                                      reinterpret_cast<BYTE*>(sz), &dwCb);
        if (lRc != ERROR_SUCCESS) return false;
        CDuiString s(sz);
        if (s.GetLength() >= 2 && s[0] == L'{')
            s = s.Mid(1, s.GetLength() - 2);
        s.MakeLower();
        strGuid = s;
        return true;
    }

    PowrProfApi api;
    if (!api.pGetActive) return false;

    // [阶段2 有意保留] pg 为 PowerGetActiveScheme 出参 GUID*（GUID** 出参直配，
    // 单出口直线块）；LocalFree 语义明确，不引入双层包装以免改变可读性。
    GUID* pg = nullptr;
    if (api.pGetActive(nullptr, &pg) != ERROR_SUCCESS || !pg) return false;

    WCHAR sz[64] = { 0 };
    ::StringFromGUID2(*pg, sz, _countof(sz));
    ::LocalFree(pg);

    // StringFromGUID2 输出 "{大写带括号}"，Data.xml 用无括号小写
    CDuiString s(sz);
    if (s.GetLength() >= 2 && s[0] == L'{')
        s = s.Mid(1, s.GetLength() - 2);
    s.MakeLower();
    strGuid = s;
    return true;
}

bool CDataEngine::SetActivePowerPlan(const wchar_t* pszGuid, bool bUltimate)
{
    if (!pszGuid || !*pszGuid) return false;

    CDuiString sBraced;
    sBraced.Format(L"{%s}", pszGuid);
    GUID g = { 0 };
    if (FAILED(::IIDFromString(sBraced, &g))) return false;

    // 离线会话：直接写挂载蜂巢的活动方案值（REG_SZ 含结尾 \0）
    if (IsOfflineSession())
    {
        CDuiString sKey(
            L"HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\Power\\User\\PowerSchemes");
        CDuiString sMap = RemapOfflinePath(sKey, false);

        // [修复 P2-PWR] 实机该值权威存储为"无括号"GUID（实测
        // 8c5e7fda-...，powercfg 同值）；旧代码写 "{guid}"，目标系统解析
        // 失败会静默回落平衡方案，离线切换 4 条规则全部不生效。
        CDuiString sBare(pszGuid);
        if (sBare.GetLength() >= 2 && sBare[0] == L'{')
            sBare = sBare.Mid(1, sBare.GetLength() - 2);

        // 方案必须已存在于目标蜂巢：内置三方案键始终存在；卓越性能为
        // 隐藏模板（在线靠 PowerDuplicateScheme 生成），离线蜂巢无源键
        // 可复制，写悬空指针会让目标系统回落默认方案——明确判失败。
        CDuiString sPlanKey = sMap + L"\\" + sBare;
        DESRegGuard hChk;
        if (!OpenRegKey(sPlanKey, KEY_READ, hChk.k, false, false)) return false;

        DESRegGuard hKey;
        if (!OpenRegKey(sMap, KEY_SET_VALUE, hKey.k, true, false)) return false;
        const DWORD cb = (DWORD)((sBare.GetLength() + 1) * sizeof(WCHAR));
        bool bOk = ::RegSetValueExW(hKey, L"ActivePowerScheme", 0, REG_SZ,
                                    reinterpret_cast<const BYTE*>(sBare.GetData()),
                                    cb) == ERROR_SUCCESS;
        return bOk;
    }

    PowrProfApi api;
    if (!api.pSetActive) return false;

    DWORD dw = api.pSetActive(nullptr, &g);
    if (dw == ERROR_SUCCESS) return true;

    // 方案不存在（典型：卓越性能隐藏模板尚未复制）——按固定 GUID 复制后激活。
    // 固定目标 GUID 使方案 GUID 与 Data.xml 条件一致。
    if (api.pDuplicate && (bUltimate || dw == ERROR_FILE_NOT_FOUND))
    {
        GUID gDup = g;
        GUID* pOut = &gDup;
        if (api.pDuplicate(nullptr, &g, &pOut) == ERROR_SUCCESS && pOut)
        {
            GUID gActivate = *pOut;
            if (pOut != &gDup) ::LocalFree(pOut);  // [阶段2 有意保留] pOut 可能指向栈上 gDup（别名指针），条件释放语义保留
            dw = api.pSetActive(nullptr, &gActivate);
            if (dw == ERROR_SUCCESS) return true;
        }
    }
    return false;
}

// [R9-03b] RefreshDesktopIcons（逐字搬迁自 DataEngine.cpp）

void CDataEngine::RefreshDesktopIcons()
{
    // The desktop folder view ("SHELLDLL_DefView") lives either under
    // Progman or under a WorkerW window. Hit every one with F5, which is
    // the same as manually refreshing the desktop.
    ::EnumWindows([](HWND hTop, LPARAM) -> BOOL {
        HWND hDef = ::FindWindowEx(hTop, nullptr,
                                   L"SHELLDLL_DefView", nullptr);
        if (hDef)
        {
            ::PostMessage(hDef, WM_KEYDOWN, VK_F5, 0x003E0001);
            ::PostMessage(hDef, WM_KEYUP,   VK_F5, 0xC03E0001);
        }
        return TRUE;
    }, 0);
}
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
