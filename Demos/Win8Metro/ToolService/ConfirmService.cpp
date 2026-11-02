// ============================================================
// ConfirmService.cpp —— [R6-03] 详细设置 5 开关 + 4 系统副作用
// 行为等价基线：ToolDialog.cpp(2026-10-13) DetailsOk 6801-7074 与
// BuildDetails 6768-6772 的 ConfigReader 读取。逐字搬迁，仅把
// CDuiString/_T 边界换成 std::wstring/L""，日志告警改走 IToolServiceHost。
// ============================================================

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <windows.h>
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")

#include <string>
#include <vector>

#include "ConfigReader.h"     // [R-B10] 详细设置 [Advanced] 段读写
#include "ProcessRunner.h"    // [R-B33] reagentc 启用/禁用 WinRE

#include "IToolServiceHost.h" // HostLog/HostAlert 完整定义
#include "WinHandles.h"       // [C++17 R11] win::RegKey
#include "ConfirmService.h"

namespace toolsvc {

namespace {

// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
inline constexpr int kCpp17_ConfirmService_StepTimeoutMs = 120000;

// [Advanced] 5 键，顺序严格对应旧 DetailsOk kKeys / BuildDetails kOpts。
const wchar_t* const kAdvKeys[CConfirmService::kAdvancedCount] = {
    L"ExpertMode",
    L"LoadWofAdk",
    L"RightClickMenu",
    L"IntegrateRE",
    L"DeltaTransfer",
};

} // namespace

void CConfirmService::Open(const ToolOpenArgs& args)
{
    // [R16][T4] 删除清单与双路径/删除确认文案统一经 ToolOpenArgs 注入。
    m_vDel        = args.vDelItems;
    m_sDelTitle   = args.sDelTitle;
    m_sDualTitle  = args.sDualTitle;
    m_sDualLabel1 = args.sDualLabel1;
    m_sDualLabel2 = args.sDualLabel2;
}

void CConfirmService::LoadAdvancedFlags(bool vOut[kAdvancedCount]) const
{
    for (int i = 0; i < kAdvancedCount; ++i)
    {
        std::wstring sVal = i18n::CConfigReader::GetString(
            L"Advanced", kAdvKeys[i], L"0");
        vOut[i] = (sVal == L"1");
    }
}

void CConfirmService::ApplyAdvanced(const bool vIn[kAdvancedCount])
{
    for (int i = 0; i < kAdvancedCount; ++i)
    {
        i18n::CConfigReader::SetString(L"Advanced", kAdvKeys[i],
                                       vIn[i] ? L"1" : L"0");
    }

    // 顺序与旧 DetailsOk 完全一致：WofAdk -> 右键菜单 -> 整合RE -> 差异传输。
    ApplyLoadWofAdk();
    ApplyRightClickMenu();
    ApplyIntegrateRE();
    ApplyDeltaTransfer();

    m_pHost->HostLog(L"[details] advanced options saved", 0);
}

// ---- [R-B27] LoadWofAdk：把随包 wofadk.sys 注册/启动为内核驱动 ----
void CConfirmService::ApplyLoadWofAdk()
{
    i18n::CConfigReader::Load(nullptr);
    std::wstring wsLoad = i18n::CConfigReader::GetString(L"Advanced", L"LoadWofAdk", L"0");
    bool bLoadWofAdk = (wsLoad == L"1");

    // wofadk.sys 与 exe 同目录（双架构各自随包，bin 目录 i386 / bin64 目录 amd64）
    wchar_t szExeDir[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, szExeDir, _countof(szExeDir));
    ::PathRemoveFileSpecW(szExeDir);
    std::wstring wsDriver = szExeDir;
    if (!wsDriver.empty() && wsDriver.back() != L'\\') wsDriver += L'\\';
    wsDriver += L"wofadk.sys";

    SC_HANDLE hSCM = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (hSCM)
    {
        SC_HANDLE hSvc = ::OpenServiceW(hSCM, L"wofadk", SERVICE_ALL_ACCESS);
        if (bLoadWofAdk)
        {
            if (!hSvc && ::GetFileAttributesW(wsDriver.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                hSvc = ::CreateServiceW(hSCM, L"wofadk", L"WofAdk (Overlay File System)",
                                        SERVICE_ALL_ACCESS,
                                        SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START,
                                        SERVICE_ERROR_NORMAL, wsDriver.c_str(),
                                        nullptr, nullptr, nullptr, nullptr, nullptr);
            }
            if (hSvc)
            {
                SERVICE_STATUS ss = {};
                ::StartServiceW(hSvc, 0, nullptr);
                m_pHost->HostLog(L"[details] LoadWofAdk: wofadk.sys 已加载", 0);
                ::CloseServiceHandle(hSvc);
            }
            else
            {
                m_pHost->HostAlert(L"详细设置", L"LoadWofAdk: 注册 wofadk 服务失败", MB_ICONWARNING);
            }
        }
        else if (hSvc)
        {
            SERVICE_STATUS ss = {};
            ::ControlService(hSvc, SERVICE_CONTROL_STOP, &ss);
            ::DeleteService(hSvc);
            ::CloseServiceHandle(hSvc);
            m_pHost->HostLog(L"[details] LoadWofAdk: wofadk 服务已停止并删除", 0);
        }
        ::CloseServiceHandle(hSCM);
    }
}

// ---- [R-B28/R-B75] RightClickMenu：资源管理器外壳右键扩展 ----
void CConfirmService::ApplyRightClickMenu()
{
    i18n::CConfigReader::Load(nullptr);
    std::wstring wsCtx = i18n::CConfigReader::GetString(L"Advanced", L"RightClickMenu", L"0");
    bool bCtx = (wsCtx == L"1");

    wchar_t szExePath[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, szExePath, _countof(szExePath));

    // [R-B75] 知识库 v30 D30-1 / strings L832-857 权威形态：
    //   ① 8 动词注册到 HKCR\SOFTWARE\...\Explorer\CommandStore\shell\<Verb>
    //     （MUIVerb + command 子键）；
    //   ② *、Directory、Dism.esd 三处 shell\DismCompact 级联（SubCommands）；
    //   ③ .esd 默认关联 ProgID Dism.esd。
    std::wstring wsExe = L"\"" + std::wstring(szExePath) + L"\"";

    static const struct { const wchar_t* verb; const wchar_t* mui; const wchar_t* cmd; } kStore[] = {
        { L"UnCompact",    L"撤销 Compact",       L" /U \"%1\"" },
        { L"WofXpress4k",  L"Xpress4k（推荐）",    L" /C /exe xpress4k \"%1\"" },
        { L"WofXpress8k",  L"Xpress8k",            L" /C /exe Xpress8k \"%1\"" },
        { L"WofXpress16k", L"Xpress16k",           L" /C /exe Xpress16k \"%1\"" },
        { L"WofLZX",       L"LZX（体积最小）",     L" /C /exe LZX \"%1\"" },
        { L"DecryptESD",   L"Dism++ ESD 解密",     L" /ESDDECRYPT \"%1\"" },
        { L"ESD2ISO",      L"Dism++ ESD 转 ISO",   L" /ESD2ISO \"%1\"" },
        { L"ESD2WIM",      L"Dism++ ESD 转 WIM",   L" /ESD2WIM \"%1\"" },
    };
    static const wchar_t* kStoreRoot =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\CommandStore\\shell\\";

    static const struct { const wchar_t* key; const wchar_t* mui; const wchar_t* subs; } kCascade[] = {
        { L"*\\shell\\DismCompact", L"Dism++ 压缩",
          L"UnCompact;WofXpress4k;WofXpress8k;WofXpress16k;WofLZX" },
        { L"Directory\\shell\\DismCompact", L"Dism++ 压缩",
          L"UnCompact;WofXpress4k;WofXpress8k;WofXpress16k;WofLZX" },
        { L"Dism.esd\\shell\\DismCompact", L"Dism++",
          L"DecryptESD;ESD2ISO;ESD2WIM;UnCompact;WofXpress4k;WofXpress8k;WofXpress16k;WofLZX" },
    };

    // R-B28/R-B59 旧版注册根（切换/卸载时一并清理，避免菜单重复）
    static const wchar_t* kLegacy[] = {
        L"SystemFileAssociations\\.esd\\shell\\DismESDDecrypt",
        L"SystemFileAssociations\\.esd\\shell\\DismESD2ISO",
        L"SystemFileAssociations\\.esd\\shell\\DismESD2WIM",
        L"SystemFileAssociations\\.wim\\shell\\DismESDDecrypt",
        L"SystemFileAssociations\\.wim\\shell\\DismESD2ISO",
        L"SystemFileAssociations\\.wim\\shell\\DismESD2WIM",
        L"Directory\\shell\\DismUncompact",
    };

    auto BfRegSetSz = [](HKEY hRoot, const std::wstring& sSub,
                         const wchar_t* pszValue, const std::wstring& sData) -> bool
    {
        win::RegKey hk;   // [C++17 R11] local key, auto RegCloseKey
        if (::RegCreateKeyExW(hRoot, sSub.c_str(), 0, nullptr, 0,
                              KEY_SET_VALUE, nullptr, hk.out(), nullptr) != ERROR_SUCCESS)
            return false;
        bool bOk = ::RegSetValueExW(hk.get(), pszValue, 0, REG_SZ,
                      reinterpret_cast<const BYTE*>(sData.c_str()),
                      (DWORD)((sData.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
        return bOk;
    };

    for (size_t i = 0; i < _countof(kLegacy); ++i)
        ::SHDeleteKeyW(HKEY_CLASSES_ROOT, kLegacy[i]);

    if (bCtx)
    {
        for (size_t i = 0; i < _countof(kStore); ++i)
        {
            std::wstring sVerb = std::wstring(kStoreRoot) + kStore[i].verb;
            BfRegSetSz(HKEY_CLASSES_ROOT, sVerb, L"MUIVerb", kStore[i].mui);
            BfRegSetSz(HKEY_CLASSES_ROOT, sVerb + L"\\command", nullptr,
                       wsExe + kStore[i].cmd);
        }
        for (size_t i = 0; i < _countof(kCascade); ++i)
        {
            std::wstring sKey(kCascade[i].key);
            BfRegSetSz(HKEY_CLASSES_ROOT, sKey, L"MUIVerb", kCascade[i].mui);
            BfRegSetSz(HKEY_CLASSES_ROOT, sKey, L"SubCommands", kCascade[i].subs);
            BfRegSetSz(HKEY_CLASSES_ROOT, sKey, L"NeverDefault", L"");
        }
        BfRegSetSz(HKEY_CLASSES_ROOT, L".esd", nullptr, L"Dism.esd");
        m_pHost->HostLog(L"[details] RightClickMenu: CommandStore 8 verbs + */Directory/Dism.esd DismCompact cascade 已注册", 0);
    }
    else
    {
        for (size_t i = 0; i < _countof(kStore); ++i)
        {
            std::wstring sVerb = std::wstring(kStoreRoot) + kStore[i].verb;
            ::SHDeleteKeyW(HKEY_CLASSES_ROOT, (sVerb + L"\\command").c_str());
            ::SHDeleteKeyW(HKEY_CLASSES_ROOT, sVerb.c_str());
        }
        for (size_t i = 0; i < _countof(kCascade); ++i)
            ::SHDeleteKeyW(HKEY_CLASSES_ROOT, kCascade[i].key);
        ::SHDeleteKeyW(HKEY_CLASSES_ROOT, L"Dism.esd\\shell");
        m_pHost->HostLog(L"[details] RightClickMenu: CommandStore + DismCompact cascade 已卸载", 0);
    }
}

// ---- [R-B33] IntegrateRE：整合 WinRE（reagentc 启用/禁用）----
void CConfirmService::ApplyIntegrateRE()
{
    i18n::CConfigReader::Load(nullptr);
    std::wstring wsRE = i18n::CConfigReader::GetString(L"Advanced", L"IntegrateRE", L"0");
    bool bIntegrateRE = (wsRE == L"1");

    if (bIntegrateRE)
    {
        // 首选启用已配置的恢复映像
        DWORD dwEn = ProcessRunner::RunHiddenExit(L"reagentc.exe",
                            DuiLib::CDuiString(L"/enable"), kCpp17_ConfirmService_StepTimeoutMs);
        if (dwEn == 0)
        {
            m_pHost->HostLog(L"[details] IntegrateRE: reagentc /enable ok", 0);
        }
        else
        {
            // 未配置恢复映像：尝试从 Install.wim 的 WinRE 索引挂载
            wchar_t szWinDir[MAX_PATH] = {};
            ::GetWindowsDirectoryW(szWinDir, _countof(szWinDir));
            std::wstring wsInstall = std::wstring(szWinDir) + L"\\..\\sources\\install.wim";
            std::wstring sRe = L"/setreimage /path \"" + wsInstall + L"\" /index 1";
            DWORD dwSet = ProcessRunner::RunHiddenExit(L"reagentc.exe",
                                DuiLib::CDuiString(sRe.c_str()), 180000);
            if (dwSet == 0)
            {
                ProcessRunner::RunHiddenExit(L"reagentc.exe",
                            DuiLib::CDuiString(L"/enable"), kCpp17_ConfirmService_StepTimeoutMs);
                m_pHost->HostLog(L"[details] IntegrateRE: setreimage+enable ok", 0);
            }
            else
            {
                std::wstring sFail =
                    L"[details] IntegrateRE: reagentc failed (enable=" +
                    std::to_wstring(dwEn) + L", setreimage=" +
                    std::to_wstring(dwSet) + L")";
                m_pHost->HostLog(sFail, 0);
            }
        }
    }
    else
    {
        DWORD dwDis = ProcessRunner::RunHiddenExit(L"reagentc.exe",
                            DuiLib::CDuiString(L"/disable"), kCpp17_ConfirmService_StepTimeoutMs);
        std::wstring sDis = L"[details] IntegrateRE: reagentc /disable exit=" +
                            std::to_wstring(dwDis);
        m_pHost->HostLog(sDis, 0);
    }
}

// ---- [R-B34] DeltaTransfer：差异传输（系统级 Wof 开关）----
void CConfirmService::ApplyDeltaTransfer()
{
    i18n::CConfigReader::Load(nullptr);
    std::wstring wsDelta = i18n::CConfigReader::GetString(L"Advanced", L"DeltaTransfer", L"0");
    bool bDelta = (wsDelta == L"1");

    // 探测系统 wof.sys（%SystemRoot%\System32\drivers\wof.sys）
    wchar_t szSysDir[MAX_PATH] = {};
    ::GetSystemDirectoryW(szSysDir, _countof(szSysDir));
    std::wstring wsWof = std::wstring(szSysDir) + L"\\drivers\\wof.sys";
    bool bWofOk = ::GetFileAttributesW(wsWof.c_str()) != INVALID_FILE_ATTRIBUTES;

    if (!bWofOk)
    {
        std::wstring sWarn =
            L"[details] DeltaTransfer: wof.sys not found (" + wsWof +
            L"); Wof 差异压缩不可用";
        m_pHost->HostLog(sWarn, 0);
    }

    win::RegKey hKey;   // [C++17 R11] local key, auto RegCloseKey
    if (::RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                          L"SYSTEM\\CurrentControlSet\\Control\\Wof", 0, nullptr, 0,
                          KEY_SET_VALUE, nullptr, hKey.out(), nullptr) == ERROR_SUCCESS)
    {
        DWORD dwVal = bDelta ? 1 : 0;
        ::RegSetValueExW(hKey.get(), L"FileCompressionEnabled", 0, REG_DWORD,
                         reinterpret_cast<const BYTE*>(&dwVal), sizeof(dwVal));
        std::wstring sRes =
            L"[details] DeltaTransfer: Wof\\FileCompressionEnabled=" +
            std::to_wstring(dwVal) + L" (wof.sys=" +
            (bWofOk ? L"1" : L"0") + L")";
        m_pHost->HostLog(sRes, 0);
    }
    else
    {
        m_pHost->HostLog(L"[details] DeltaTransfer: RegCreateKeyExW Wof failed", 0);
    }
}

} // namespace toolsvc
