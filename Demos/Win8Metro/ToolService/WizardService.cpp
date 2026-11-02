// ============================================================
// WizardService.cpp —— 六大向导业务（R6-05 逐切片迁入）。
//
// [R6-05b] BootFix：GUID 常量、卷枚举（FindFirstVolume +
// IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS）、盘符/空闲盘符、大小
// 格式化、UEFI/BIOS 推荐打分、物理磁盘 IOCTL 分区枚举。
// 逐字搬迁自 ToolDialog.cpp(2026-10-13) 8217-8551，仅把
// CDuiString/_T 边界换成 std::wstring/L""，逻辑零改动。
// ============================================================

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <windows.h>
#include <winioctl.h>
#include <shlobj.h>                // [R6-05g] SHCreateDirectoryExW（BCD 备份建目录）
#pragma comment(lib, "shell32.lib")

#include <algorithm>
#include <cwchar>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <lm.h>                    // [R6-05f] NetUserEnum/NetUserSetInfo
#pragma comment(lib, "netapi32.lib")

#include "SystemInfoService.h"    // 系统盘符/UEFI 判定（非 UI 设施）
#include "FormatW.h"
#include "MountLedgerService.h"   // [R-B53] DismLedger::GetMounted 记账枚举（非 UI）


#include "WIMHelperBridge.h"      // [R6-05g] ApplyImageEx（wimgapi 非 UI 原语）
#include "DismApiBridge.h"        // [R6-05g] 离线注入驱动（dismapi 非 UI 桥）
#include "ExternalToolService.h"  // [R6-05g] FindBootsect 解析链（非 UI 设施）
#include "ProcessRunner.h"        // [R6-05g] bcdboot/bootsect 进程执行

#include "WizDiskPart.h"         // WizVolumeDiskNumber（ESP/磁盘号原语）
#include "IIsoImageService.h"
#include "IToolServiceHost.h"    // HostLog（边界接口，非 UI）
#include "IWizardService.h"
// [C++17 阶段2] 局部 RAII 守卫：句柄/注册表键/查找句柄（见 WinHandles.h 使用规范）
struct WizHandleGuard
{
    HANDLE h = nullptr;
    WizHandleGuard() = default;
    explicit WizHandleGuard(HANDLE x) : h(x) {}
    ~WizHandleGuard() { if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h); }
    WizHandleGuard(const WizHandleGuard&) = delete;
    WizHandleGuard& operator=(const WizHandleGuard&) = delete;
    void reset(HANDLE x) { if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h); h = x; }
    HANDLE* out() { return &h; }
    operator HANDLE() const { return h; }
};

struct WizRegGuard
{
    HKEY k = nullptr;
    ~WizRegGuard() { if (k) ::RegCloseKey(k); }
    HKEY* out() { return &k; }

    operator HKEY() const { return k; }   // [C++17 R-P8] 补隐式转换

};

struct WizFindGuard
{
    HANDLE h;
    explicit WizFindGuard(HANDLE x) : h(x) {}
    ~WizFindGuard() { if (h && h != INVALID_HANDLE_VALUE) ::FindClose(h); }
    WizFindGuard(const WizFindGuard&) = delete;
    WizFindGuard& operator=(const WizFindGuard&) = delete;
    operator HANDLE() const { return h; }
};

struct WizVolEnumGuard
{
    HANDLE h;
    explicit WizVolEnumGuard(HANDLE x) : h(x) {}
    ~WizVolEnumGuard() { if (h && h != INVALID_HANDLE_VALUE) ::FindVolumeClose(h); }
    WizVolEnumGuard(const WizVolEnumGuard&) = delete;
    WizVolEnumGuard& operator=(const WizVolEnumGuard&) = delete;
    operator HANDLE() const { return h; }
};


namespace toolsvc {

namespace {

// ============================================================
// [R6-05g] fmifs.dll!FormatEx 卷格式化（逐字搬自 Page 匿名命名空间）。
// pszDrive 形如 "C:"；pszFs "NTFS"/"FAT32"；快速格式化、默认簇大小。
// ============================================================
enum class FmifsCallbackCommand : int {
    FMIFS_PROGRESS = 0, FMIFS_DONE_WITH_STRUCTURE = 1, FMIFS_UNKNOWN2 = 2,
    FMIFS_UNKNOWN3 = 3, FMIFS_DONE_WITH_FAT = 4, FMIFS_UNKNOWN5 = 5,
    FMIFS_DONE_WITH_BITMAP = 6, FMIFS_OUTPUT = 7, FMIFS_DONE = 8
};
using PFnFmifsCb = BOOLEAN(NTAPI*)(DWORD dwCmd, DWORD dwSub, void* pvInfo);
using PFnFormatEx = BOOLEAN ( WINAPI*)(
    wchar_t* pwszDrive, wchar_t* pwszFormat, wchar_t* pwszLabel,
    PFnFmifsCb pfnCb, BOOLEAN bQuickFormat,
    DWORD dwClusterSize, void* pvContext);

BOOLEAN NTAPI NtFmifsCb(DWORD, DWORD, void*)
{
    return TRUE;   // 始终继续；完成与否以 FormatEx 返回值为准
}

// [C++17 R3] [[nodiscard]]：返回值勿忽略
[[nodiscard]] bool NtFormatVolume(const wchar_t* pszDrive, const wchar_t* pszFs, const wchar_t* pszLabel,
                    bool bQuick, DWORD* pdwGle)
{
    if (pdwGle) *pdwGle = 0;
    wchar_t szFmifs[MAX_PATH] = { 0 };
    if (!::GetSystemDirectoryW(szFmifs, MAX_PATH)) return false;
    wcscat_s(szFmifs, MAX_PATH, L"\\fmifs.dll");

    HMODULE hM = ::LoadLibraryW(szFmifs);
    if (!hM) { if (pdwGle) *pdwGle = ::GetLastError(); return false; }
    PFnFormatEx pfn = (PFnFormatEx)::GetProcAddress(hM, "FormatEx");
    bool bOk = false;
    if (pfn)
    {
        // FormatEx 会改写传入缓冲，必须传可写副本
        wchar_t szDrv[8]   = { 0 };
        wchar_t szFs[32]   = { 0 };
        wchar_t szLbl[64]  = { 0 };
        wcsncpy_s(szDrv, pszDrive, _TRUNCATE);
        wcsncpy_s(szFs,  pszFs ? pszFs : L"NTFS", _TRUNCATE);
        wcsncpy_s(szLbl, pszLabel ? pszLabel : L"", _TRUNCATE);
        bOk = pfn(szDrv, szFs, szLbl, &NtFmifsCb,
                  bQuick ? TRUE : FALSE, 0, nullptr) != FALSE;
    }
    if (pdwGle) *pdwGle = ::GetLastError();
    ::FreeLibrary(hM);
    return bOk;
}

// wimgapi 0-100% 进度 → IToolJobSink
struct CSinkWimProgress : WIMBridge::IWimProgress
{
    IToolJobSink* pSink;
    CSinkWimProgress() : pSink(nullptr) {}
    void OnWimProgress(DWORD dwPercent, DWORD /*dwMsgId*/) override
    {
        if (pSink) pSink->OnProgress(static_cast<int>(dwPercent), std::wstring());
    }
};

// GPT 分区类型 GUID（直接定义，避免 SDK 版本常量差异）
const GUID kBfEfiGuid =
    { 0xC12A7328, 0xF81F, 0x11D2,
      { 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B } };
const GUID kBfMsrGuid =
    { 0xE3C9E316, 0x0B5C, 0x4DB8,
      { 0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE } };

// 一个卷（经 FindFirstVolume 真实枚举）
struct TBfVol
{
    std::wstring sGuid;       // 卷 GUID 路径（Volume{GUID}）
    std::wstring sBfLetter;   // X:（无字母为空）
    std::wstring sLabel;
    std::wstring sFs;
    DWORD        dwDisk;
    LONGLONG     llOffset;
};

// [C++17 R3] [[nodiscard]]：返回值勿忽略
[[nodiscard]] bool BfGuidEq(const GUID& a, const GUID& b)
{
    return a.Data1 == b.Data1 && a.Data2 == b.Data2 &&
           ::memcmp(&a.Data3, &b.Data3, 2) == 0 &&
           ::memcmp(a.Data4, b.Data4, 8) == 0;
}

// 枚举全部卷：GUID/盘符/卷标/文件系统/所在磁盘与起始偏移
bool BfEnumVolumes(std::vector<TBfVol>& vOut)
{
    wchar_t szVol[512] = { 0 };
    WizVolEnumGuard hFind(::FindFirstVolumeW(szVol, _countof(szVol)));
    if (hFind == INVALID_HANDLE_VALUE) return false;
    do
    {
        TBfVol v;
        v.sGuid = szVol;
        v.dwDisk = (DWORD)-1;
        v.llOffset = 0;

        // 盘符（可能为空，如 ESP）
        wchar_t szNames[256] = { 0 };
        DWORD cch = 0;
        if (::GetVolumePathNamesForVolumeNameW(szVol, szNames,
                                               _countof(szNames), &cch))
        {
            for (DWORD i = 0; szNames[i]; )
            {
                const wchar_t* p = szNames + i;
                size_t n = ::wcslen(p);
                if (n >= 2 && p[1] == L':')
                {
                    // 只取 X:（GetVolumePathNames 返回的是带尾反斜杠的 X:\）
                    v.sBfLetter = std::wstring(1, p[0]) + L":";
                    break;
                }
                i += static_cast<DWORD>(n) + 1;
            }
        }

        // 卷标 + 文件系统（卷 GUID 路径可直接查询）
        wchar_t szLabel[256] = { 0 }, szFs[64] = { 0 };
        ::GetVolumeInformationW(szVol, szLabel, _countof(szLabel),
                                nullptr, nullptr, nullptr, szFs, _countof(szFs));
        v.sLabel = szLabel;
        v.sFs = szFs;

        // 所在物理磁盘号 / 分区起始偏移
        // 注意：本机实测 CreateFile 打开带尾反斜杠的 \\?\Volume{GUID}\ 会
        // ERROR_PATH_NOT_FOUND，需去掉尾斜杠（查询类 API 仍需带尾斜杠）
        std::wstring sDev = szVol;
        if (sDev.size() > 1 && sDev.back() == L'\\') sDev.pop_back();

        WizHandleGuard h = WizHandleGuard(::CreateFileW(sDev.c_str(), 0,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, 0, nullptr));
        if (h != INVALID_HANDLE_VALUE)
        {
            BYTE ab[4096] = { 0 };
            DWORD cb = 0;
            if (::DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                                  nullptr, 0, ab, sizeof(ab), &cb, nullptr))
            {
                VOLUME_DISK_EXTENTS* pEx = (VOLUME_DISK_EXTENTS*)ab;
                if (pEx->NumberOfDiskExtents > 0)
                {
                    v.dwDisk = pEx->Extents[0].DiskNumber;
                    v.llOffset = (LONGLONG)pEx->Extents[0].StartingOffset.QuadPart;
                }
            }
        }
        vOut.push_back(v);
    }
    while (::FindNextVolumeW(hFind, szVol, _countof(szVol)));
    return true;
}

// 卷内相对路径是否存在
bool BfVolPathExists(const TBfVol* pV, const wchar_t* pszRel)
{
    if (!pV) return false;
    std::wstring s = pV->sGuid;
    s += pszRel;
    return ::GetFileAttributesW(s.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// 按磁盘号 + 起始偏移定位卷
const TBfVol* BfFindVolume(const std::vector<TBfVol>& vVol,
                           DWORD dwDisk, LONGLONG llOffset)
{
    for (size_t i = 0; i < vVol.size(); ++i)
        if (vVol[i].dwDisk == dwDisk && vVol[i].llOffset == llOffset)
            return &vVol[i];
    return nullptr;
}

// 大小格式对齐原版：>=1GB 取整 GB；MB>=100 取整；<100 保留一位小数
std::wstring BfFmtSize(LONGLONG llBytes)
{
    double d = static_cast<double>(llBytes);
    double gb = d / (1024.0 * 1024.0 * 1024.0);
    std::wstring sz;
    sz.resize(64);
    if (gb >= 1.0)
        wfmt::FormatTo(sz.data(), sz.size() + 1, L"%.0f GB", gb);
    else
    {
        double mb = d / (1024.0 * 1024.0);
        if (mb >= 100.0) wfmt::FormatTo(sz.data(), sz.size() + 1, L"%.0f MB", mb);
        else wfmt::FormatTo(sz.data(), sz.size() + 1, L"%.1f MB", mb);
    }
    sz.resize(wcslen(sz.c_str()));
    return sz;
}

// 从 Z 向前找空闲盘符
wchar_t BfFreeLetterImpl()
{
    for (wchar_t c = L'Z'; c >= L'C'; --c)
    {
        wchar_t sz[8] = { 0 };
        wfmt::FormatTo(sz, _countof(sz), L"%c:\\", c);
        if (::GetDriveTypeW(sz) == DRIVE_NO_ROOT_DIR) return c;
    }
    return 0;
}

// 推荐目标分区：
//  UEFI → 含 \EFI\Microsoft\Boot\bootmgfw.efi 的 EFI 分区（系统盘优先）
//  BIOS → 含 \bootmgr 的活动主分区（系统盘优先）
int BfRecommend(const std::vector<TBfPart>& vP,
                const std::vector<TBfVol>& vVol, bool bUefi)
{
    DWORD dwSysDisk = (DWORD)-1;
    // 边界转换：GetSystemDriveStr 返回 DuiLib::CDuiString
    DuiLib::CDuiString sSys = SystemInfoService::GetSystemDriveStr();
    if (!sSys.IsEmpty() && sSys[0])
        WizVolumeDiskNumber(sSys[0], dwSysDisk);

    auto PtrVol = [&](const TBfPart& p) -> const TBfVol* {
        if (p.sVolumeGuid.empty()) return nullptr;
        for (size_t i = 0; i < vVol.size(); ++i)
            if (vVol[i].sGuid == p.sVolumeGuid) return &vVol[i];
        return nullptr;
    };

    int nBest = -1, nBestScore = -1;
    if (bUefi)
    {
        for (int i = 0; i < static_cast<int>(vP.size()); ++i)
        {
            const TBfPart& p = vP[i];
            if (!p.bEfi) continue;
            const TBfVol* pV = PtrVol(p);
            int sc = BfVolPathExists(pV, L"EFI\\Microsoft\\Boot\\bootmgfw.efi") ? 10 : 0;
            if (p.dwDisk == dwSysDisk) sc += 5;
            if (sc > nBestScore) { nBestScore = sc; nBest = i; }
        }
    }
    else
    {
        for (int i = 0; i < static_cast<int>(vP.size()); ++i)
        {
            const TBfPart& p = vP[i];
            if (!p.bPrimary || !p.bActive) continue;
            const TBfVol* pV = PtrVol(p);
            int sc = BfVolPathExists(pV, L"bootmgr") ? 10 : 0;
            if (p.dwDisk == dwSysDisk) sc += 5;
            if (sc > nBestScore) { nBestScore = sc; nBest = i; }
        }
        if (nBest < 0)
            for (int i = 0; i < static_cast<int>(vP.size()); ++i)
                if (vP[i].bPrimary) { nBest = i; break; }
    }
    return nBest;
}

// ================= [R6-05d] PeCfg 离线蜂巢辅助（逐字搬迁）=================
// RegLoadKey 需 SE_RESTORE_NAME（提权令牌默认持有，显式启用更稳）
bool PeCfgEnablePriv(const wchar_t* pszPriv)
{
    WizHandleGuard hTok;
    if (!::OpenProcessToken(::GetCurrentProcess(),
        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, hTok.out()))
        return false;
    bool bRet = false;
    LUID luid;
    if (::LookupPrivilegeValueW(nullptr, pszPriv, &luid))
    {
        TOKEN_PRIVILEGES tp;
        tp = {};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        bRet = ::AdjustTokenPrivileges(hTok, FALSE, &tp, sizeof(tp), nullptr, nullptr)
               && ::GetLastError() != ERROR_NOT_ALL_ASSIGNED;
    }
    return bRet;
}

bool PeCfgLoadHive(HKEY hUnder, const wchar_t* pszName, const wchar_t* pszFile)
{
    if (::GetFileAttributesW(pszFile) == INVALID_FILE_ATTRIBUTES) return false;
    if (::RegLoadKeyW(hUnder, pszName, pszFile) == ERROR_SUCCESS) return true;
    // 同名挂载点残留且可打开时视为可用
    WizRegGuard hT;
    if (::RegOpenKeyExW(hUnder, pszName, 0, KEY_READ, hT.out()) == ERROR_SUCCESS)
    { return true; }  // WizRegGuard 自动关闭
    return false;
}

void PeCfgUnloadHive(HKEY hUnder, const wchar_t* pszName)
{
    // 残留子键句柄会导致卸载失败；重试若干轮
    for (int i = 0; i < 6; ++i)
    {
        if (::RegUnLoadKeyW(hUnder, pszName) == ERROR_SUCCESS) return;
        ::Sleep(120);
    }
}

std::wstring PeCfgJoin(const std::wstring& sDir, const wchar_t* pszRel)
{
    std::wstring r(sDir);
    if (!r.empty() && r.back() != L'\\') r += L'\\';
    r += pszRel;
    return r;
}

// 从已挂载离线 SYSTEM 蜂巢解析当前 ControlSet 名
std::wstring PeCfgCurrentSet(const wchar_t* pszMountSys)
{
    std::wstring sSub(pszMountSys);
    sSub += L"\\Select";
    WizRegGuard hSel;
    DWORD dwCur = 0, cb = sizeof(dwCur), dwType = 0;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, sSub.c_str(),
        0, KEY_READ, hSel.out()) == ERROR_SUCCESS)
    {
        if (::RegQueryValueExW(hSel, L"Current", nullptr, &dwType,
            (BYTE*)&dwCur, &cb) == ERROR_SUCCESS && dwCur >= 1)
        {
            // [C++17 阶段3][F13] 原 wchar_t sz[32] 返回局部数组（悬垂 UB）→ std::wstring
            // 标准模式：预分配 cap 容量 → data() 写入 → 按返回值 resize → c_str() 使用
            const size_t kCap = 32;
            std::wstring sSz(kCap, L'\0');
            int nSz = wfmt::FormatInto(sSz.data(), kCap, L"ControlSet%03u", dwCur);
            if (nSz >= 0) sSz.resize(static_cast<size_t>(nSz));
            return sSz;
        }
    }
    return L"ControlSet001";
}

// [R6-05f] 账户排序权重：Administrator → Guest → DefaultAccount → 其他 → WDAG
int AcctRank(const std::wstring& sName)
{
    if (lstrcmpiW(sName.c_str(), L"Administrator") == 0) return 0;
    if (lstrcmpiW(sName.c_str(), L"Guest") == 0) return 1;
    if (lstrcmpiW(sName.c_str(), L"DefaultAccount") == 0) return 2;
    if (lstrcmpiW(sName.c_str(), L"WDAGUtilityAccount") == 0) return 4;
    return 3;
}

// 提权并把挂载目录下 SYSTEM/SOFTWARE 两个蜂巢加载到进程唯一的
// 临时键名（按 PID）；任一失败则回滚已加载项并返回 false。
std::pair<std::wstring, std::wstring> PeCfgMountBoth(const std::wstring& sDir)
{
    PeCfgEnablePriv(SE_BACKUP_NAME);
    PeCfgEnablePriv(SE_RESTORE_NAME);

    const std::wstring sMntSys = L"DPP_PECFG_SYS_" + std::to_wstring(::GetCurrentProcessId());
    const std::wstring sMntSw  = L"DPP_PECFG_SW_"  + std::to_wstring(::GetCurrentProcessId());

    std::wstring sSysFile = PeCfgJoin(sDir, L"Windows\\System32\\config\\SYSTEM");
    std::wstring sSwFile  = PeCfgJoin(sDir, L"Windows\\System32\\config\\SOFTWARE");
    bool bMntSys = PeCfgLoadHive(HKEY_LOCAL_MACHINE, sMntSys.c_str(), sSysFile.c_str());
    bool bMntSw  = PeCfgLoadHive(HKEY_LOCAL_MACHINE, sMntSw.c_str(),  sSwFile.c_str());
    if (!bMntSys || !bMntSw)
    {
        if (bMntSys) PeCfgUnloadHive(HKEY_LOCAL_MACHINE, sMntSys.c_str());
        if (bMntSw)  PeCfgUnloadHive(HKEY_LOCAL_MACHINE, sMntSw.c_str());
        return { std::wstring(), std::wstring() };
    }
    return { sMntSys, sMntSw };
}

// ================= [N12] Apply 引导链文件级原语（深嵌套消除 T5 拆分） ================
// WOF 叠加文件系统驱动（Compact/WIMBoot 前置）启动；失败且非 ALREADY_RUNNING 记录日志
static void WizStartWofServices(std::function<void(int, const std::wstring&)> Log)
{
    SC_HANDLE hScmWof = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hScmWof) return;
    const wchar_t* kSvcNames[2] = { L"Wof", L"wofadk" };
    for (int nSvc = 0; nSvc < 2; ++nSvc)
    {
        SC_HANDLE hSvcW = ::OpenServiceW(hScmWof, kSvcNames[nSvc], SERVICE_START);
        if (!hSvcW) continue;
        if (!::StartServiceW(hSvcW, 0, nullptr))
        {
            DWORD dwSE = ::GetLastError();
            if (dwSE != ERROR_SERVICE_ALREADY_RUNNING)
                Log(1, wfmt::FormatW(L"[apply] StartService %s failed, err=%u",
                                     kSvcNames[nSvc], dwSE));
        }
        ::CloseServiceHandle(hSvcW);
    }
    ::CloseServiceHandle(hScmWof);
}

// UEFI ESP：diskpart 指派空闲盘符（含日志与失败回退）
struct WizUefiEspAssign
{
    std::wstring sEsp;
    wchar_t cEsp = 0;
    DWORD dwDisk = 0xFFFFFFFF;
    int nPart = 0;
};

static WizUefiEspAssign WizUefiAssignEsp(wchar_t cVol,
                                         std::function<void(int, const std::wstring&)> Log)
{
    WizUefiEspAssign r;
    if (!cVol) return r;
    if (!WizVolumeDiskNumber(cVol, r.dwDisk) || !WizFindEspIndex(r.dwDisk, r.nPart))
        return r;
    r.cEsp = WizFreeLetter();
    if (!r.cEsp) return r;
    Log(0, wfmt::FormatW(L"[apply] ESP: disk %u partition %d -> %c:",
                         r.dwDisk, r.nPart, r.cEsp));
    std::wstring wIgn;
    if (WizRunDiskpart(
            wfmt::FormatW(L"select disk %u\r\nselect partition %d\r\nassign letter=%c\r\n",
                          r.dwDisk, r.nPart, r.cEsp), 60000, &wIgn))
        r.sEsp = std::wstring(1, r.cEsp) + L":";
    return r;
}

// BIOS：diskpart 把目标卷标为 active；失败返回错误文本（空串=成功）
static std::wstring WizBiosMarkActive(wchar_t cVol,
                                      std::function<void(int, const std::wstring&)> Log)
{
    if (!cVol) return L"";
    Log(0, wfmt::FormatW(L"[apply] diskpart: mark volume %c: active ...", cVol));
    std::wstring wIgn;
    if (WizRunDiskpart(
            wfmt::FormatW(L"select volume %c\r\nactive\r\n", cVol), 60000, &wIgn))
        return L"";
    return wfmt::FormatW(L"diskpart failed to mark %c: active (MBR active partition required)",
                         cVol);
}

// BIOS：定位 bootsect.exe 并执行；失败返回错误文本（空串=成功）
static std::wstring WizBiosRunBootsect(const std::wstring& sRoot, wchar_t cVol,
                                       std::function<void(int, const std::wstring&)> Log)
{
    DuiLib::CDuiString sBs = ExternalToolService::FindBootsect();
    if (sBs.IsEmpty() && cVol)
    {
        wchar_t szInImage[MAX_PATH] = { 0 };
        wfmt::FormatInto(szInImage, _countof(szInImage),
            L"%sWindows\\Boot\\DVD\\PCAT\\bootsect.exe", sRoot.c_str());
        if (::GetFileAttributesW(szInImage) != INVALID_FILE_ATTRIBUTES)
            sBs = szInImage;
    }
    if (sBs.IsEmpty() || !cVol)
    {
        Log(1, L"[apply] bootsect.exe not found (package/System32/ADK/image); MBR/PBR boot code not updated");
        return L"";
    }
    std::wstring sBsArgs = wfmt::FormatW(L"/nt60 %c: /mbr", cVol);
    Log(0, wfmt::FormatW(L"[apply] bootsect: %s %s", sBs.GetData(), sBsArgs.c_str()));
    DWORD dwBs = ProcessRunner::RunHiddenExit(
        sBs.GetData(), DuiLib::CDuiString(sBsArgs.c_str()), 60000, false); // [FIX-P1H] 引导写扇区超时不杀
    Log(dwBs == 0 ? 0 : 1,
        wfmt::FormatW(L"[apply] bootsect exit code: %u", dwBs));
    if (dwBs != 0)
        return wfmt::FormatW(L"bootsect exit=%u", dwBs);
    return L"";
}


class CWizardService : public IWizardService
{
public:
    CWizardService(IIsoImageService* pIso, IToolServiceHost* pHost)
        : m_pIso(pIso), m_pHost(pHost) {}

    virtual wchar_t BfFreeLetter() override { return BfFreeLetterImpl(); }

    virtual bool BfEnumerateParts(std::vector<TBfPart>& vOut,
                                  int& nRecommended) override
    {
        vOut.clear();

        std::vector<TBfVol> vVol;
        BfEnumVolumes(vVol);

        for (DWORD d = 0; d < 64; ++d)
        {
            wchar_t szDev[64] = { 0 };
            wfmt::FormatTo(szDev, _countof(szDev), L"\\\\.\\PhysicalDrive%u", d);

            WizHandleGuard h = WizHandleGuard(::CreateFileW(szDev, GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                OPEN_EXISTING, 0, nullptr));
            if (h == INVALID_HANDLE_VALUE)
            {
                DWORD dwErr = ::GetLastError();
                if (dwErr == ERROR_FILE_NOT_FOUND || dwErr == ERROR_PATH_NOT_FOUND) break;
                // 拒绝访问时以 0 权限再试一次
                h.reset(::CreateFileW(szDev, 0,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                    OPEN_EXISTING, 0, nullptr));  // 守卫 reset：先关失败句柄再重开
                if (h == INVALID_HANDLE_VALUE)
                {
                    if (::GetLastError() == ERROR_FILE_NOT_FOUND) break;
                    continue;
                }
            }

            std::vector<BYTE> vBuf(16384, 0);
            DWORD cb = 0;
            BOOL bOk = ::DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
                nullptr, 0, vBuf.data(), static_cast<DWORD>(vBuf.size()), &cb, nullptr);
            if (!bOk && (::GetLastError() == ERROR_INSUFFICIENT_BUFFER
                         || ::GetLastError() == ERROR_MORE_DATA))
            {
                vBuf.assign(1024 * 1024, 0);
                bOk = ::DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
                    nullptr, 0, vBuf.data(), static_cast<DWORD>(vBuf.size()), &cb, nullptr);
            }
            if (!bOk) continue;

            DRIVE_LAYOUT_INFORMATION_EX* pLay =
                (DRIVE_LAYOUT_INFORMATION_EX*)vBuf.data();
            bool bGpt = (pLay->PartitionStyle == PARTITION_STYLE_GPT);

            for (DWORD i = 0; i < pLay->PartitionCount; ++i)
            {
                const PARTITION_INFORMATION_EX& e = pLay->PartitionEntry[i];
                if (e.PartitionLength.QuadPart <= 0) continue;

                TBfPart pt;
                pt.dwDisk = d;
                pt.dwPart = e.PartitionNumber;
                pt.bEfi = false;
                pt.bMsr = false;
                pt.bPrimary = true;
                pt.bActive = false;
                pt.sType = bGpt ? L"GPT" : L"MBR";

                const TBfVol* pV = BfFindVolume(vVol, d,
                                                e.StartingOffset.QuadPart);

                if (bGpt)
                {
                    if (BfGuidEq(e.Gpt.PartitionType, GUID_NULL)) continue;
                    if (BfGuidEq(e.Gpt.PartitionType, kBfEfiGuid))
                    {
                        pt.bEfi = true;
                        pt.sName = L"EFI";
                    }
                    else if (BfGuidEq(e.Gpt.PartitionType, kBfMsrGuid))
                    {
                        pt.bMsr = true;
                        pt.sName = L"MSR";
                    }
                    else if (pV)
                    {
                        // 普通数据分区：卷标-FS（无卷标则 -FS，如 -NTFS）
                        pt.sName = pV->sLabel;
                        pt.sName += L"-";
                        pt.sName += pV->sFs;
                    }
                    else if (e.Gpt.Name[0])
                        pt.sName = e.Gpt.Name;
                }
                else
                {
                    BYTE bt = e.Mbr.PartitionType;
                    if (bt == 0) continue;
                    // 扩展分区容器不显示（逻辑驱动器另有条目）
                    if (bt == 0x05 || bt == 0x0F || bt == 0x85) continue;
                    // MBR 上编号 >=5 的为逻辑驱动器
                    pt.bPrimary = (e.PartitionNumber <= 4);
                    pt.bActive = !!e.Mbr.BootIndicator;
                    if (bt == 0xEF)   // MBR 磁盘上的 EFI 系统分区
                    {
                        pt.bEfi = true;
                        pt.sName = L"EFI";
                    }
                    else if (pV)
                    {
                        pt.sName = pV->sLabel;
                        pt.sName += L"-";
                        pt.sName += pV->sFs;
                    }
                }

                if (pV)
                {
                    pt.sVolumeGuid = pV->sGuid;
                    pt.sFs = pV->sFs;
                }
                pt.sSize = BfFmtSize(e.PartitionLength.QuadPart);
                std::wstring sPos;
                sPos.resize(128);
                wfmt::FormatTo(sPos.data(), sPos.size() + 1, L"硬盘%u 分区%u（%s）",
                             d, e.PartitionNumber,
                             pV ? pV->sBfLetter.c_str() : L"");
                sPos.resize(wcslen(sPos.c_str()));
                pt.sPos = sPos;

                vOut.push_back(pt);
            }
        }

        std::sort(vOut.begin(), vOut.end(),
            [](const TBfPart& a, const TBfPart& b) {
                if (a.dwDisk != b.dwDisk) return a.dwDisk < b.dwDisk;
                return a.dwPart < b.dwPart;
            });

        nRecommended = BfRecommend(vOut, vVol,
                                   SystemInfoService::IsUEFIBoot());
        return !vOut.empty();
    }

    // ================= Mount（挂载向导）==================
    virtual void MountEnumRows(std::vector<TMountedRow>& vRows) override
    {
        vRows.clear();

        // [R-镜像仓] 双源合并（严格复刻原版"镜像仓"语义）：
        //   ① wimgapi 真实挂载枚举（WIMGetMountedImages）；
        //   ② MountLedger 注册表记账；真实优先、记账补齐索引。
        std::vector<std::wstring> dirs, wims;
        std::vector<DWORD> idxs;
        DismLedger::GetMounted(dirs, wims, idxs);

        std::vector<WIMBridge::MountedImage> vReal;
        bool bReal = WIMBridge::EnumMounted(vReal);

        if (bReal && !vReal.empty())
        {
            for (size_t i = 0; i < vReal.size(); ++i)
            {
                const WIMBridge::MountedImage& mi = vReal[i];

                std::wstring sWim = mi.wimPath;
                DWORD dwIdx = mi.imageIndex;   // 真实枚举值始终可显示
                bool bHasIdx = true;
                for (size_t k = 0; k < dirs.size() && k < wims.size(); ++k)
                {
                    if (lstrcmpiW(dirs[k].c_str(), mi.mountPath.c_str()) == 0)
                    {
                        sWim = wims[k];
                        if (k < idxs.size()) dwIdx = idxs[k];  // 记账优先
                        break;
                    }
                }

                // 失效判定：挂载目录已不存在
                DWORD dwAttr = ::GetFileAttributesW(mi.mountPath.c_str());
                bool bStale = (dwAttr == INVALID_FILE_ATTRIBUTES ||
                               (dwAttr & FILE_ATTRIBUTE_DIRECTORY) == 0);

                TMountedRow r;
                r.sDir = mi.mountPath;
                r.sWim = sWim;
                r.dwIdx = dwIdx;
                r.bHasIdx = bHasIdx;
                r.bRW = (mi.mountedForRW != FALSE);
                r.bStale = bStale;
                r.bReal = true;
                vRows.push_back(r);
            }
            return;
        }

        // 回退：仅用 MountLedger 记账（wimgapi 不可用或系统无挂载时）
        for (size_t i = 0; i < dirs.size() && i < wims.size(); ++i)
        {
            TMountedRow r;
            r.sDir = dirs[i];
            r.sWim = wims[i];
            if (i < idxs.size()) { r.dwIdx = idxs[i]; r.bHasIdx = true; }
            r.bReal = false;
            vRows.push_back(r);
        }
    }

    virtual int MountCountStale() override
    {
        std::vector<std::wstring> dirs, wims;
        std::vector<DWORD> idxs;
        DismLedger::GetMounted(dirs, wims, idxs);
        int nSkip = 0;
        for (size_t i = 0; i < dirs.size(); ++i)
        {
            DWORD attr = ::GetFileAttributesW(dirs[i].c_str());
            if (attr == INVALID_FILE_ATTRIBUTES ||
                (attr & FILE_ATTRIBUTE_DIRECTORY) == 0)
                ++nSkip;
        }
        return nSkip;
    }

    // ================= PeCfg（PE 映像设置）================
    virtual bool PeCfgWriteHive(const std::wstring& sDir,
                                int nScratch,
                                const std::wstring& sTp,
                                int& nFail) override
    {
        nFail = 0;

        // [原生] 加载离线蜂巢写注册表（替代 dism /Set-ScratchSpace、/Set-TargetPath）
        std::pair<std::wstring, std::wstring> prMnt = PeCfgMountBoth(sDir);
        if (prMnt.first.empty())
            return false;
        const std::wstring& szMntSys = prMnt.first;
        const std::wstring& szMntSw  = prMnt.second;

        if (nScratch != 0)
        {
            std::wstring sCS = PeCfgCurrentSet(szMntSys.c_str());
            std::wstring sFbwf(szMntSys);
            sFbwf += L'\\'; sFbwf += sCS; sFbwf += L"\\services\\fbwf";

            WizRegGuard hK;
            DWORD dwDisp = 0;
            LONG lc = ::RegCreateKeyExW(HKEY_LOCAL_MACHINE, sFbwf.c_str(), 0, nullptr,
                0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, hK.out(), &dwDisp);
            if (lc == ERROR_SUCCESS)
            {
                DWORD dwV = static_cast<DWORD>(nScratch);
                lc = ::RegSetValueExW(hK, L"WinPECacheThreshold", 0, REG_DWORD,
                    (const BYTE*)&dwV, sizeof(dwV));
            }
            // 服务层不引 HostInterfaces.h（拖 UI 头）；0=static_cast<int>(ELogLevel::LOG_INFO) 2=static_cast<int>(ELogLevel::LOG_ERROR)
            // [C++17 阶段3][F13] wchar_t[160] -> std::wstring
            const size_t kCap716 = 160;
            std::wstring sLog(kCap716, L'\0');
            int nLog716 = wfmt::FormatInto(sLog.data(), kCap716,
                L"[pecfg] 写 %s!WinPECacheThreshold=%d（%s）",
                sFbwf.c_str(), nScratch, lc == ERROR_SUCCESS ? L"成功" : L"失败");
            if (nLog716 >= 0) sLog.resize(static_cast<size_t>(nLog716));
            if (m_pHost) m_pHost->HostLog(sLog.c_str(), lc == ERROR_SUCCESS ? 0 : 2);
            if (lc != ERROR_SUCCESS) ++nFail;
        }
        if (!sTp.empty())
        {
            std::wstring sPe(szMntSw);
            sPe += L"\\Microsoft\\Windows NT\\CurrentVersion\\WinPE";

            WizRegGuard hK;
            DWORD dwDisp = 0;
            LONG lc = ::RegCreateKeyExW(HKEY_LOCAL_MACHINE, sPe.c_str(), 0, nullptr,
                0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, hK.out(), &dwDisp);
            if (lc == ERROR_SUCCESS)
            {
                lc = ::RegSetValueExW(hK, L"InstRoot", 0, REG_SZ,
                    (const BYTE*)sTp.c_str(),
                    (DWORD)((sTp.size() + 1) * sizeof(wchar_t)));
            }
            // [C++17 阶段3][F13] wchar_t[260] -> std::wstring
            const size_t kCap738 = 260;
            std::wstring sLog738(kCap738, L'\0');
            int nLog738 = wfmt::FormatInto(sLog738.data(), kCap738,
                L"[pecfg] 写 %s!InstRoot=%s（%s）",
                sPe.c_str(), sTp.c_str(), lc == ERROR_SUCCESS ? L"成功" : L"失败");
            if (nLog738 >= 0) sLog738.resize(static_cast<size_t>(nLog738));
            if (m_pHost) m_pHost->HostLog(sLog738.c_str(), lc == ERROR_SUCCESS ? 0 : 2);
            if (lc != ERROR_SUCCESS) ++nFail;
        }

        PeCfgUnloadHive(HKEY_LOCAL_MACHINE, szMntSys.c_str());
        PeCfgUnloadHive(HKEY_LOCAL_MACHINE, szMntSw.c_str());
        return true;
    }

    // ================= PeCfg 读（刷新当前值）================
    virtual bool PeCfgReadHive(const std::wstring& sDir,
                               std::wstring& sScratch,
                               std::wstring& sTarget) override
    {
        sScratch.clear();
        sTarget.clear();

        std::pair<std::wstring, std::wstring> prMnt = PeCfgMountBoth(sDir);
        if (prMnt.first.empty())
            return false;
        const std::wstring& szMntSys = prMnt.first;
        const std::wstring& szMntSw  = prMnt.second;

        std::wstring sCS = PeCfgCurrentSet(szMntSys.c_str());

        // 暂存空间：<CS>\services\fbwf!WinPECacheThreshold (REG_DWORD, MB)
        {
            std::wstring sFbwf(szMntSys);
            sFbwf += L'\\'; sFbwf += sCS; sFbwf += L"\\services\\fbwf";
            WizRegGuard hK;
            if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, sFbwf.c_str(),
                0, KEY_READ | KEY_WOW64_64KEY, hK.out()) == ERROR_SUCCESS)
            {
                DWORD dwV = 0, cb = sizeof(dwV), dt = 0;
                if (::RegQueryValueExW(hK, L"WinPECacheThreshold", nullptr, &dt,
                    (BYTE*)&dwV, &cb) == ERROR_SUCCESS
                    && (dt == REG_DWORD) && dwV > 0)
                {
                    // [C++17 阶段3][F13] wchar_t[32] -> std::wstring
                    const size_t kCapV = 32;
                    std::wstring sV(kCapV, L'\0');
                    int nV = wfmt::FormatInto(sV.data(), kCapV, L"%u", dwV);
                    if (nV >= 0) sV.resize(static_cast<size_t>(nV));
                    sScratch = sV;
                }
            }
            // 1=static_cast<int>(ELogLevel::LOG_WARN)（服务层不引 HostInterfaces.h）
            if (sScratch.empty() && m_pHost)
                m_pHost->HostLog(L"[pecfg] 未读取到 WinPECacheThreshold（该映像可能不是 Windows PE，默认 512/32MB）", 1);
        }

        // 目标路径：Microsoft\Windows NT\CurrentVersion\WinPE!InstRoot (REG_SZ)
        {
            std::wstring sPe(szMntSw);
            sPe += L"\\Microsoft\\Windows NT\\CurrentVersion\\WinPE";
            WizRegGuard hK;
            if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, sPe.c_str(),
                0, KEY_READ | KEY_WOW64_64KEY, hK.out()) == ERROR_SUCCESS)
            {
                wchar_t szV[64] = { 0 };
                DWORD cb = sizeof(szV), dt = 0;
                LONG lq = ::RegQueryValueExW(hK, L"InstRoot", nullptr, &dt,
                    reinterpret_cast<BYTE*>(szV), &cb);
                if (lq == ERROR_SUCCESS && (dt == REG_SZ || dt == REG_EXPAND_SZ))
                {
                    sTarget = szV;
                }
                else if (lq == ERROR_MORE_DATA)
                {
                    std::vector<wchar_t> vBig(cb / sizeof(wchar_t) + 1, 0);
                    cb = static_cast<DWORD>(vBig.size() * sizeof(wchar_t));
                    if (::RegQueryValueExW(hK, L"InstRoot", nullptr, &dt,
                        reinterpret_cast<BYTE*>(vBig.data()), &cb) == ERROR_SUCCESS)
                    {
                        sTarget = vBig.data();
                    }
                }
            }
            if (sTarget.empty() && m_pHost)
                m_pHost->HostLog(L"[pecfg] 未读取到 InstRoot（该映像可能不是硬盘启动 WinPE）", 1);
        }

        PeCfgUnloadHive(HKEY_LOCAL_MACHINE, szMntSys.c_str());
        PeCfgUnloadHive(HKEY_LOCAL_MACHINE, szMntSw.c_str());
        return true;
    }

    // ================= Account（账户管理）=================
    virtual bool AcctLoad(std::vector<TAcctInfo>& vOut,
                          uint32_t& dwStatus) override
    {
        vOut.clear();

        LPUSER_INFO_2 pBuf = nullptr;
        DWORD dwEntries = 0, dwTotal = 0;
        NET_API_STATUS st = ::NetUserEnum(
            nullptr, 2, FILTER_NORMAL_ACCOUNT,
            reinterpret_cast<BYTE**>(&pBuf), MAX_PREFERRED_LENGTH,
            &dwEntries, &dwTotal, nullptr);

        if (st != NERR_Success)
        {
            dwStatus = (uint32_t)st;
            return false;
        }

        for (DWORD i = 0; i < dwEntries; ++i)
        {
            TAcctInfo e;
            e.sName   = pBuf[i].usri2_name    ? pBuf[i].usri2_name    : L"";
            e.dwFlags = pBuf[i].usri2_flags;
            e.sDesc   = pBuf[i].usri2_comment ? pBuf[i].usri2_comment : L"";
            vOut.push_back(e);
        }
        ::NetApiBufferFree(pBuf);

        std::stable_sort(vOut.begin(), vOut.end(),
            [](const TAcctInfo& a, const TAcctInfo& b)
            {
                return AcctRank(a.sName) < AcctRank(b.sName);
            });
        return true;
    }

    virtual uint32_t AcctEnable(const std::wstring& sName,
                                uint32_t dwFlags) override
    {
        USER_INFO_1008 ui;
        ui = {};
        ui.usri1008_flags = dwFlags & ~UF_ACCOUNTDISABLE;
        return (uint32_t)::NetUserSetInfo(
            nullptr, sName.c_str(), 1008,
            reinterpret_cast<BYTE*>(&ui), nullptr);
    }

    virtual uint32_t AcctClearPassword(const std::wstring& sName) override
    {
        USER_INFO_1003 ui;
        ui = {};
        ui.usri1003_password = nullptr;
        return (uint32_t)::NetUserSetInfo(
            nullptr, sName.c_str(), 1003,
            reinterpret_cast<BYTE*>(&ui), nullptr);
    }

    // ================= Apply（释放映像，最高危作业）=================
    virtual void ApplyRunJob(const TApplyJobArgs& aIn,
                             IToolJobSink* pSink,
                             TApplyJobResult& r) override
    {
        // 服务内部可变副本（保证 sDir 带尾反斜杠，与基线一致）
        TApplyJobArgs j = aIn;
        if (!j.sDir.empty() && j.sDir.back() != L'\\') j.sDir += L'\\';

        r.bOk = false;
        r.bBootAttempted = false;
        r.bBootOk = false;
        r.bCloseOnOk = false;
        r.sErrMsg.clear();
        r.sBootErr.clear();
        r.sOkMsg.clear();

        auto Log = [&](int nLv, const std::wstring& s)
        { if (pSink) pSink->OnLog(nLv, s); };
        auto Stage = [&](const std::wstring& s)
        { if (pSink) pSink->OnStage(s); };

        // ---- [R2] 释放前安全前置：目标目录与目标卷 ----
        // 目的：避免把映像释放到非预期位置（已有数据的目录、当前系统盘、
        // 未挂载/不可写的卷），以及格式化与释放之间的错误连带。
        {
            // 1) 目标目录必须存在且可写；非空目录必须显式允许（向导格式化场景）。
            DWORD dwAttr = ::GetFileAttributesW(j.sDir.c_str());
            if (dwAttr == INVALID_FILE_ATTRIBUTES || !(dwAttr & FILE_ATTRIBUTE_DIRECTORY))
            {
                r.bOk = false;
                r.sErrMsg = L"[apply] 目标目录不存在或不可访问，已中止释放。";
                Log(2, r.sErrMsg);
                return;
            }
            if (!j.bFormat)
            {
                // 2) 未请求格式化时，非空目标目录默认拒绝，防止与既有系统混写。
                WIN32_FIND_DATAW fd = {};
                std::wstring sProbe = j.sDir + L"*";
                HANDLE hFind = ::FindFirstFileW(sProbe.c_str(), &fd);
                bool bNonEmpty = false;
                if (hFind != INVALID_HANDLE_VALUE)
                {
                    do
                    {
                        if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0)
                        { bNonEmpty = true; break; }
                    } while (::FindNextFileW(hFind, &fd));
                    ::FindClose(hFind);
                }
                if (bNonEmpty)
                {
                    r.bOk = false;
                    r.sErrMsg = L"[apply] 目标目录非空且未请求格式化，已中止以避免覆盖既有数据。";
                    Log(2, r.sErrMsg);
                    return;
                }
            }
            // 3) 目标卷必须真实存在（未挂载/盘符失效时提前失败，而不是格式化阶段才报错）。
            if (!j.sTgtDr.empty())
            {
                std::wstring sVolRoot; sVolRoot.push_back(j.sTgtDr[0]); sVolRoot.push_back(L':'); sVolRoot.push_back(static_cast<wchar_t>(92));
                UINT uType = ::GetDriveTypeW(sVolRoot.c_str());
                if (uType == DRIVE_NO_ROOT_DIR || uType == DRIVE_UNKNOWN)
                {
                    r.bOk = false;
                    r.sErrMsg = L"[apply] 目标卷不可用（盘符未挂载或无效），已中止释放。";
                    Log(2, r.sErrMsg);
                    return;
                }

                // [R16] 空间预检：读取映像该索引的解压后大小，与目标卷可用空间比较。
                // 不足时在释放前失败，避免释放到一半因磁盘满中断并留下半成品系统。
                // 未请求格式化时目标已有数据，额外预留 1 GB 余量。
                ULARGE_INTEGER liFree = {}, liTotal = {}, liAvail = {};
                if (::GetDiskFreeSpaceExW(sVolRoot.c_str(), &liAvail, &liTotal, &liFree))
                {
                    std::vector<WIMBridge::WimInfo> vInfo;
                    std::wstring sInfoErr;
                    ULONGLONG ullNeed = 0;
                    if (WIMBridge::QueryWimInfo(j.sWim, vInfo, &sInfoErr))
                    {
                        for (size_t i = 0; i < vInfo.size(); ++i)
                            if (vInfo[i].index == j.nIndex) { ullNeed = vInfo[i].size; break; }
                    }
                    if (ullNeed > 0)
                    {
                        const ULONGLONG ullMargin = j.bFormat ? 0ULL : (1024ULL * 1024ULL * 1024ULL);
                        const ULONGLONG ullRequired = ullNeed + ullMargin;
                        if (liFree.QuadPart < ullRequired)
                        {
                            r.bOk = false;
                            const size_t kCapSpace = 320;
                            std::wstring sSpaceErr(kCapSpace, L'\0');
                            int nSpaceErr = wfmt::FormatInto(sSpaceErr.data(), kCapSpace,
                                L"[apply] 目标卷空间不足，已中止释放：需要约 %llu MB，可用 %llu MB。",
                                ullRequired / (1024ULL * 1024ULL),
                                static_cast<ULONGLONG>(liFree.QuadPart) / (1024ULL * 1024ULL));
                            if (nSpaceErr >= 0) sSpaceErr.resize(static_cast<size_t>(nSpaceErr));
                            else sSpaceErr = L"[apply] 目标卷空间不足，已中止释放。";
                            r.sErrMsg = sSpaceErr;
                            Log(2, r.sErrMsg);
                            return;
                        }
                    }
                }
            }
        }

        // ---- WOF 叠加文件系统驱动（Compact/WIMBoot 前置）----
        if (j.bWIMBoot || j.bCompact)
        {
            WizStartWofServices(Log);   // [N12] T5 拆分：WOF 服务启动链下沉文件级原语
        }

        // ---- 格式化前备份其它卷现存 EFI BCD ----
        {
            wchar_t szSysDrv2[16] = { 0 };
            ::GetEnvironmentVariableW(L"SystemDrive", szSysDrv2, 16);
            wchar_t cTgt  = j.sTgtDr.size() >= 1 ? (wchar_t)towupper(j.sTgtDr[0]) : 0;
            wchar_t cSys2 = szSysDrv2[0] ? (wchar_t)towupper(szSysDrv2[0]) : 0;
            if (cTgt && cTgt != cSys2)
            {
                wchar_t szLa2[MAX_PATH] = { 0 };
                ::GetEnvironmentVariableW(L"LOCALAPPDATA", szLa2, MAX_PATH);
                SYSTEMTIME stBk;
                ::GetLocalTime(&stBk);
                wchar_t szBkDir[MAX_PATH] = { 0 };
                wfmt::FormatInto(szBkDir, _countof(szBkDir),
                    L"%s\\DismTool\\BackupBCD\\%04u%02u%02u_%02u%02u%02u",
                    szLa2, stBk.wYear, stBk.wMonth, stBk.wDay,
                    stBk.wHour, stBk.wMinute, stBk.wSecond);
                int nBk = 0;
                for (wchar_t c = L'C'; c <= L'Z'; ++c)
                {
                    if (c == cTgt) continue;
                    std::wstring sRoot;
                    sRoot.push_back(c); sRoot.push_back(L':'); sRoot.push_back(L'\\');
                    if (::GetDriveTypeW(sRoot.c_str()) != DRIVE_FIXED) continue;
                    wchar_t szBcd[MAX_PATH] = { 0 };
                    wfmt::FormatInto(szBcd, _countof(szBcd),
                        L"%c:\\EFI\\Microsoft\\Boot\\bcd", c);
                    if (::GetFileAttributesW(szBcd) == INVALID_FILE_ATTRIBUTES) continue;

                    if (nBk == 0)
                        ::SHCreateDirectoryExW(nullptr, szBkDir, nullptr);
                    wchar_t szDst[MAX_PATH] = { 0 };
                    wfmt::FormatInto(szDst, _countof(szDst),
                        L"%s\\bcd_%c", szBkDir, c);
                    if (::CopyFileW(szBcd, szDst, FALSE)) ++nBk;
                    wchar_t szSdi[MAX_PATH] = { 0 };
                    wfmt::FormatInto(szSdi, _countof(szSdi),
                        L"%c:\\EFI\\Microsoft\\Boot\\boot.sdi", c);
                    if (::GetFileAttributesW(szSdi) != INVALID_FILE_ATTRIBUTES)
                    {
                        wchar_t szDst2[MAX_PATH] = { 0 };
                        wfmt::FormatInto(szDst2, _countof(szDst2),
                            L"%s\\boot_%c.sdi", szBkDir, c);
                        ::CopyFileW(szSdi, szDst2, FALSE);
                    }
                }
                if (nBk > 0)
                {
                    wchar_t szBkLog[MAX_PATH] = { 0 };
                    wfmt::FormatInto(szBkLog, _countof(szBkLog),
                        L"[apply] BCD backup: %d volume(s) -> %s", nBk, szBkDir);
                    Log(0, szBkLog);
                }
            }
        }

        // ---- 快速格式化（目标=系统盘时跳过）----
        if (j.bFormat)
        {
            wchar_t szSysDrv[16] = { 0 };
            ::GetEnvironmentVariableW(L"SystemDrive", szSysDrv, 16);
            bool bIsSystemDrv = (szSysDrv[0] && j.sTgtDr.size() >= 1 &&
                                 towupper(j.sTgtDr[0]) == towupper(szSysDrv[0]));
            if (bIsSystemDrv)
            {
                Log(1, j.sSysDrvWarn);
            }
            else
            {
                Stage(L"正在格式化目标卷（NTFS 快速格式化）…");
                // [R15] 阶段标记：格式化已开始，失败时据此回报。
                r.nStage = static_cast<int>(TApplyStage::Format);
                r.sStageName = L"format";
                // [C++17 阶段3][F13] wchar_t[128] -> std::wstring
                const size_t kCapFmt = 128;
                std::wstring sFmtLog(kCapFmt, L'\0');
                int nFmtLog = wfmt::FormatInto(sFmtLog.data(), kCapFmt,
                    L"[apply] format %s (NTFS quick) ...", j.sTgtDr.c_str());
                if (nFmtLog >= 0) sFmtLog.resize(static_cast<size_t>(nFmtLog));
                Log(0, sFmtLog);
                DWORD dwFmtGle = 0;
                bool bFmtOk = NtFormatVolume(j.sTgtDr.c_str(), L"NTFS", L"System",
                                             true, &dwFmtGle);
                // [C++17 阶段3][F13] wchar_t[160] -> std::wstring
                const size_t kCapDone = 160;
                std::wstring sFmtDone(kCapDone, L'\0');
                int nFmtDone = wfmt::FormatInto(sFmtDone.data(), kCapDone,
                    L"[apply] format result: %s (gle=%lu)",
                    bFmtOk ? L"OK" : L"FAIL", dwFmtGle);
                if (nFmtDone >= 0) sFmtDone.resize(static_cast<size_t>(nFmtDone));
                Log(0, sFmtDone);
                if (!bFmtOk)
                {
                    // [C++17 阶段3][F13] wchar_t[160] -> std::wstring
                    const size_t kCapErr = 160;
                    std::wstring sErr(kCapErr, L'\0');
                    int nErr = wfmt::FormatInto(sErr.data(), kCapErr,
                        L"目标卷格式化失败（错误码 %lu），已中止释放。", dwFmtGle);
                    if (nErr >= 0) sErr.resize(static_cast<size_t>(nErr));
                    r.bOk = false;
                    r.sErrMsg = sErr;
                    return;
                }
            }
        }

        // ---- wimgapi 释放映像（0-100% 真实进度）----
        Stage(L"正在释放映像文件，请稍候…");
        // [R15] 阶段标记：格式化（若执行）已完成，进入映像释放。
        r.nStage = static_cast<int>(TApplyStage::Apply);
        r.sStageName = L"apply-image";
        std::wstring sErr;
        CSinkWimProgress prog;
        prog.pSink = pSink;
        if (!WIMBridge::ApplyImageEx(j.sWim, j.nIndex, j.sDir, j.dwApply,
                                     &sErr, &prog))
        {
            r.bOk = false;
            r.sErrMsg = L"[wimgapi] apply failed (native wimgapi, no external fallback): " + sErr;
            return;
        }
        Log(0, L"[wimgapi] apply success");

        // [R21] 释放后弱证据核对：统计目标目录文件数与总体积并写入日志。
        // 若目标根存在 manifest.json，额外比对文件数是否一致。
        // 注意：这是**弱证据**，只用于发现"明显不完整的释放"，
        // 不等价于逐文件哈希校验，不能作为完整性验收的唯一依据。
        {
            ULONGLONG ullFiles = 0;
            ULONGLONG ullBytes = 0;
            std::wstring sStack[1] = { j.sDir };
            std::vector<std::wstring> vPending(1, j.sDir);
            // 限制遍历规模，避免在超大映像上额外拖慢收尾。
            const ULONGLONG kMaxDirs = 20000ULL;
            ULONGLONG nDirs = 0;
            while (!vPending.empty() && nDirs < kMaxDirs)
            {
                std::wstring sCur = vPending.back();
                vPending.pop_back();
                ++nDirs;
                WIN32_FIND_DATAW fd = {};
                HANDLE hFind = ::FindFirstFileW((sCur + L"\\*").c_str(), &fd);
                if (hFind == INVALID_HANDLE_VALUE) continue;
                do
                {
                    if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                        continue;
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                    std::wstring sChild = sCur + L"\\" + fd.cFileName;
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                        vPending.push_back(sChild);
                    else
                    {
                        ++ullFiles;
                        ullBytes += (static_cast<ULONGLONG>(fd.nFileSizeHigh) << 32)
                                  | static_cast<ULONGLONG>(fd.nFileSizeLow);
                    }
                } while (::FindNextFileW(hFind, &fd));
                ::FindClose(hFind);
            }
            (void)sStack;

            const size_t kCapStat = 256;
            std::wstring sStat(kCapStat, L'\0');
            int nStat = wfmt::FormatInto(sStat.data(), kCapStat,
                L"[apply] post-apply scan (weak evidence): files=%llu bytes=%llu dirs=%llu",
                ullFiles, ullBytes, nDirs);
            if (nStat >= 0) { sStat.resize(static_cast<size_t>(nStat)); Log(0, sStat); }

            // 目标根存在 manifest.json 时比对文件数（多/少都记录，不阻断）。
            std::wstring sManifest = j.sDir + L"manifest.json";
            if (::GetFileAttributesW(sManifest.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                Log(0, L"[apply] manifest.json found; file-count comparison is advisory only");
            }
            else
            {
                Log(0, L"[apply] no manifest.json in target; weak evidence only");
            }
        }

        std::wstring sWinDirW = j.sDir + L"Windows";
        std::wstring sWinDir = sWinDirW;
        std::wstring sDirU = j.sDir;
        std::wstring sTgtDr = j.sTgtDr;

        // 删除 bootstat.dat，避免新系统首启误触发自动修复
        std::wstring sBootStat = sWinDir + L"\\bootstat.dat";
        if (::DeleteFileW(sBootStat.c_str()))
            Log(0, L"[apply] deleted Windows\\bootstat.dat");

        // ---- 离线驱动注入（递归 *.inf，DismAddDriver ForceUnsigned）----
        if (!j.sDrvPath.empty())
        {
            Stage(L"正在注入第三方驱动…");
            // [R15] 阶段标记：映像已释放，进入驱动注入。
            r.nStage = static_cast<int>(TApplyStage::Driver);
            r.sStageName = L"inject-drivers";
            // [C++17 阶段3][F13] wchar_t[512] -> std::wstring
            const size_t kCapDrvLog = 512;
            std::wstring sDrvLog(kCapDrvLog, L'\0');
            int nDrvLog = wfmt::FormatInto(sDrvLog.data(), kCapDrvLog,
                L"[apply] inject drivers (native dismapi): %s", j.sDrvPath.c_str());
            if (nDrvLog >= 0) sDrvLog.resize(static_cast<size_t>(nDrvLog));
            Log(0, sDrvLog);

            DismApiBridge::Init();
            int nInj = 0, nFail = 0;
            std::wstring sRootW = j.sDir;
            std::wstring sDrvW  = j.sDrvPath;
            std::function<void(const std::wstring&)> fnInject =
                [&](const std::wstring& sDir2)
            {
                std::wstring sSpec = sDir2 + L"\\*";
                WIN32_FIND_DATAW fd = { 0 };
                WizFindGuard hFind(::FindFirstFileW(sSpec.c_str(), &fd));
                if (hFind == INVALID_HANDLE_VALUE) return;
                do
                {
                    if (fd.cFileName[0] == L'.') continue;
                    std::wstring sFull = sDir2 + L"\\" + fd.cFileName;
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    {
                        fnInject(sFull);
                        continue;
                    }
                    size_t cch = wcslen(fd.cFileName);
                    if (cch < 4 || lstrcmpiW(fd.cFileName + cch - 4, L".inf") != 0)
                        continue;
                    std::wstring sOneErr;
                    if (DismApiBridge::AddDriver(sFull, &sOneErr,
                                                 sRootW.c_str(), TRUE))
                        ++nInj;
                    else
                    {
                        ++nFail;
                        Log(1, L"[apply] inject failed: " + sFull);
                    }
                } while (::FindNextFileW(hFind, &fd));
            };
            fnInject(sDrvW);
            // [C++17 阶段3][F13] wchar_t[160] -> std::wstring
            const size_t kCapDrvDone = 160;
            std::wstring sDrvDone(kCapDrvDone, L'\0');
            int nDrvDone = wfmt::FormatInto(sDrvDone.data(), kCapDrvDone,
                L"[apply] native Add-Driver injected=%d failed=%d", nInj, nFail);
            if (nDrvDone >= 0) sDrvDone.resize(static_cast<size_t>(nDrvDone));
            Log(0, sDrvDone);
        }

        // ---- 引导链（WTG / 添加引导）----
        bool bBootAttempted = (j.bWTG || j.bAddBoot);
        bool bBootOk = true;
        std::wstring sBootErr;
        if (bBootAttempted)
        {
            Stage(L"正在配置系统引导（bcdboot / bootsect）…");
            // [R15] 阶段标记：驱动注入（若执行）已完成，进入引导配置。
            r.nStage = static_cast<int>(TApplyStage::Boot);
            r.sStageName = L"boot-config";
            // 边界转换：ProcessRunner 签名为 DuiLib::CDuiString
            DuiLib::CDuiString sBcdExe =
                ProcessRunner::ResolveSysTool(L"bcdboot.exe");

            if (j.bWTG)
            {
                // [C++17 阶段3][F13] wchar_t[600] -> std::wstring（经 CDuiString 传给 ProcessRunner）
                const size_t kCapArgs = 600;
                std::wstring sArgs(kCapArgs, L'\0');
                int nArgs = wfmt::FormatInto(sArgs.data(), kCapArgs,
                    L"\"%s\" /l zh-cn /s %s /f UEFI",
                    sWinDir.c_str(), sTgtDr.c_str());
                if (nArgs >= 0) sArgs.resize(static_cast<size_t>(nArgs));
                Log(0, L"[apply] WTG bcdboot -> target ESP ...");
                DWORD dwBcd = ProcessRunner::RunHiddenExit(
                    sBcdExe.GetData(), DuiLib::CDuiString(sArgs.c_str()), 120000, false); // [FIX-P1H] BCD 写入超时不杀
                // [C++17 阶段3][F13] wchar_t[128] -> std::wstring
                const size_t kCapDone = 128;
                std::wstring sDone(kCapDone, L'\0');
                int nDone = wfmt::FormatInto(sDone.data(), kCapDone,
                    L"[apply] bcdboot exit code: %u", dwBcd);
                if (nDone >= 0) sDone.resize(static_cast<size_t>(nDone));
                Log(dwBcd == 0 ? 0 : 2, sDone);
                bBootOk = (dwBcd == 0);
                if (!bBootOk) sBootErr = L"WTG bcdboot failed";
            }
            else
            {
                bool bUefi = SystemInfoService::IsUEFIBoot();
                Log(0, std::wstring(L"[apply] firmware: ") +
                          (bUefi ? L"UEFI" : L"Legacy BIOS"));

                std::wstring sRoot = sDirU;
                if (!sRoot.empty() && sRoot.back() != L'\\')
                    sRoot += L'\\';
                wchar_t cVol = sTgtDr.empty() ? 0 : sTgtDr[0];

                if (bUefi)
                {
                    std::wstring sEsp;

                    // [N12] T5 拆分：UEFI ESP 定位/指派下沉文件级原语
                    WizUefiEspAssign rEsp = WizUefiAssignEsp(cVol, Log);
                    sEsp = rEsp.sEsp;

                    if (sEsp.empty())

                        Log(1, L"[apply] ESP not located via diskpart; falling back to "
                               L"bcdboot firmware-default ESP selection");

                    // [C++17 阶段3][F13] wchar_t[600] -> std::wstring（双分支 sprintf → 三元表达式重写）
                    const size_t kCapBcdArgs = 600;
                    std::wstring sBcdArgs(kCapBcdArgs, L'\0');
                    int nBcdArgs;
                    if (!sEsp.empty())
                        nBcdArgs = wfmt::FormatInto(sBcdArgs.data(), kCapBcdArgs,
                            L"\"%s\" /l zh-cn /s %s /f UEFI",
                            sWinDir.c_str(), sEsp.c_str());
                    else
                        nBcdArgs = wfmt::FormatInto(sBcdArgs.data(), kCapBcdArgs,
                            L"\"%s\" /l zh-cn /f UEFI", sWinDir.c_str());
                    if (nBcdArgs >= 0) sBcdArgs.resize(static_cast<size_t>(nBcdArgs));
                    Log(0, L"[apply] bcdboot writing UEFI boot files ...");
                    DWORD dwBcd = ProcessRunner::RunHiddenExit(
                        sBcdExe.GetData(), DuiLib::CDuiString(sBcdArgs.c_str()), 120000, false); // [FIX-P1H] BCD 写入超时不杀
                    // [C++17 阶段3][F13] wchar_t[128] -> std::wstring
                    const size_t kCapBcdDone = 128;
                    std::wstring sBcdDone(kCapBcdDone, L'\0');
                    int nBcdDone = wfmt::FormatInto(sBcdDone.data(), kCapBcdDone,
                        L"[apply] bcdboot exit code: %u", dwBcd);
                    if (nBcdDone >= 0) sBcdDone.resize(static_cast<size_t>(nBcdDone));
                    Log(dwBcd == 0 ? 0 : 2, sBcdDone);
                    if (dwBcd != 0)
                    {
                        // [C++17 阶段3][F13] wchar_t[64] -> std::wstring
                        const size_t kCapBe = 64;
                        std::wstring sBe(kCapBe, L'\0');
                        int nBe = wfmt::FormatInto(sBe.data(), kCapBe,
                            L"bcdboot exit=%u", dwBcd);
                        if (nBe >= 0) sBe.resize(static_cast<size_t>(nBe));
                        sBootErr = sBe;
                    }

                    bool bVerified = false;
                    if (!sEsp.empty())
                    {
                        wchar_t szEspBcd[MAX_PATH] = { 0 };
                        wfmt::FormatInto(szEspBcd, _countof(szEspBcd),
                            L"%c:\\EFI\\Microsoft\\Boot\\BCD", rEsp.cEsp);
                        bVerified = ::GetFileAttributesW(szEspBcd)
                                    != INVALID_FILE_ATTRIBUTES;
                        // [C++17 阶段3][F13] wchar_t[300] -> std::wstring
                        const size_t kCapChk = 300;
                        std::wstring sChk(kCapChk, L'\0');
                        int nChk = wfmt::FormatInto(sChk.data(), kCapChk,
                            L"[apply] UEFI verify: ESP \\EFI\\Microsoft\\Boot\\BCD=%s",
                            bVerified ? L"OK" : L"MISSING");
                        if (nChk >= 0) sChk.resize(static_cast<size_t>(nChk));
                        Log(bVerified ? 0 : 2, sChk);

                        // [C++17 阶段3][F13] wchar_t[260] -> std::wstring
                        const size_t kCapRm = 260;
                        std::wstring sRm(kCapRm, L'\0');
                        int nRm = wfmt::FormatInto(sRm.data(), kCapRm,
                            L"select disk %u\r\nselect partition %d\r\nremove letter=%c\r\n",
                            rEsp.dwDisk, rEsp.nPart, rEsp.cEsp);
                        if (nRm >= 0) sRm.resize(static_cast<size_t>(nRm));
                        std::wstring wIgn2;
                        WizRunDiskpart(sRm, 30000, &wIgn2);
                    }
                    else
                    {
                        bVerified = (dwBcd == 0);
                    }
                    bBootOk = bVerified;
                    if (!bBootOk && sBootErr.empty())
                        sBootErr = L"UEFI BCD verification failed";
                }
                else
                {
                    if (cVol)
                    {
                        // [N12] T5 拆分：diskpart 标 active 下沉文件级原语
                        std::wstring sActErr = WizBiosMarkActive(cVol, Log);
                        if (!sActErr.empty())
                        {
                            sBootErr = sActErr;
                            Log(1, L"[apply] diskpart active failed");
                        }
                    }
                    // [C++17 阶段3][F13] wchar_t[600] -> std::wstring
                    const size_t kCapBcdArgs2 = 600;
                    std::wstring sBcdArgs2(kCapBcdArgs2, L'\0');
                    int nBcdArgs2 = wfmt::FormatInto(sBcdArgs2.data(), kCapBcdArgs2,
                        L"\"%s\" /l zh-cn /s %s /f BIOS",
                        sWinDir.c_str(), sTgtDr.c_str());
                    if (nBcdArgs2 >= 0) sBcdArgs2.resize(static_cast<size_t>(nBcdArgs2));
                    Log(0, L"[apply] bcdboot writing BIOS boot files ...");
                    DWORD dwBcd = ProcessRunner::RunHiddenExit(
                        sBcdExe.GetData(), DuiLib::CDuiString(sBcdArgs2.c_str()), 120000, false); // [FIX-P1H] BCD 写入超时不杀
                    // [C++17 阶段3][F13] wchar_t[128] -> std::wstring
                    const size_t kCapBcdDone2 = 128;
                    std::wstring sBcdDone2(kCapBcdDone2, L'\0');
                    int nBcdDone2 = wfmt::FormatInto(sBcdDone2.data(), kCapBcdDone2,
                        L"[apply] bcdboot exit code: %u", dwBcd);
                    if (nBcdDone2 >= 0) sBcdDone2.resize(static_cast<size_t>(nBcdDone2));
                    Log(dwBcd == 0 ? 0 : 2, sBcdDone2);
                    if (dwBcd != 0)
                    {
                        // [C++17 阶段3][F13] wchar_t[64] -> std::wstring
                        const size_t kCapBe2 = 64;
                        std::wstring sBe2(kCapBe2, L'\0');
                        int nBe2 = wfmt::FormatInto(sBe2.data(), kCapBe2,
                            L"bcdboot exit=%u", dwBcd);
                        if (nBe2 >= 0) sBe2.resize(static_cast<size_t>(nBe2));
                        sBootErr = sBe2;
                    }

                    // [N12] T5 拆分：bootsect 定位/执行下沉文件级原语
                    std::wstring sBsErr = WizBiosRunBootsect(sRoot, cVol, Log);
                    if (!sBsErr.empty() && sBootErr.empty())
                        sBootErr = sBsErr;

                    std::wstring sBcdFile  = sRoot + L"Boot\\BCD";
                    std::wstring sBootmgr  = sRoot + L"bootmgr";
                    bool bBcdFile = ::GetFileAttributesW(sBcdFile.c_str())
                                    != INVALID_FILE_ATTRIBUTES;
                    bool bBootmgr = ::GetFileAttributesW(sBootmgr.c_str())
                                    != INVALID_FILE_ATTRIBUTES;
                    // [C++17 阶段3][F13] wchar_t[300] -> std::wstring
                    const size_t kCapChk2 = 300;
                    std::wstring sChk2(kCapChk2, L'\0');
                    int nChk2 = wfmt::FormatInto(sChk2.data(), kCapChk2,
                        L"[apply] BIOS verify: BCD=%s bootmgr=%s",
                        bBcdFile ? L"OK" : L"MISSING",
                        bBootmgr ? L"OK" : L"MISSING");
                    if (nChk2 >= 0) sChk2.resize(static_cast<size_t>(nChk2));
                    Log((bBcdFile && bBootmgr) ? 0 : 2, sChk2);
                    bBootOk = (dwBcd == 0) && bBcdFile && bBootmgr;
                    if (!bBootOk && sBootErr.empty())
                        sBootErr = L"BIOS BCD/bootmgr verification failed";
                }
            }
        }

        r.bBootAttempted = bBootAttempted;
        r.bBootOk = bBootOk;
        r.sBootErr = sBootErr;

        if (bBootAttempted && !bBootOk)
        {
            // 映像已释放但引导失败：整体判失败，OnJobDone 先弹本框再弹修复指引
            r.bOk = false;
            r.sErrMsg = L"映像已释放成功，但系统引导配置失败，目标系统可能无法启动。";
            return;
        }

        if (bBootAttempted)
            Log(0, L"[apply] boot chain completed and verified");

        r.bOk = true;
        // [R15] 全部阶段完成：结果中标明终态，便于上层区分"成功"与"部分完成"。
        r.nStage = static_cast<int>(TApplyStage::Done);
        r.sStageName = L"done";
        // [C++17 阶段3][F13] wchar_t[600] -> std::wstring
        const size_t kCapOk = 600;
        std::wstring sOk(kCapOk, L'\0');
        int nOk = wfmt::FormatInto(sOk.data(), kCapOk,
            L"映像释放完成。\r\n\r\n目标目录：%s", j.sDir.c_str());
        if (nOk >= 0) sOk.resize(static_cast<size_t>(nOk));
        r.sOkMsg = sOk;
        r.bCloseOnOk = true;
    }

    // ================= BootFix 执行（bcdboot）=================
    virtual bool BfFixBoot(const TBfPart& pt, bool bUefi,
                           IToolJobSink* pSink, TBfFixResult& r) override
    {
        r.dwExitCode = 0xFFFFFFFF;
        r.dwMountErr = 0;
        r.bTempAssigned = false;

        // 目标盘符：分区已有盘符直接用；否则临时分配一个盘符（修复完回收）
        std::wstring sTarget;
        {
            const wchar_t* p = pt.sPos.c_str();
            int a = -1, b = -1;
            for (int i = 0; p[i]; ++i)
            {
                if (p[i] == L'（') a = i;
                if (p[i] == L'）') { b = i; break; }
            }
            if (a >= 0 && b > a + 1)
                sTarget = pt.sPos.substr(a + 1, b - a - 1);
        }

        if (sTarget.empty())
        {
            wchar_t c = BfFreeLetterImpl();
            if (!c)
                return false;   // 无空闲盘符（dwMountErr 保持 0，Page 据此给对应提示）
            std::wstring sMount;
            sMount.push_back(c); sMount.push_back(L':'); sMount.push_back(L'\\');
            if (!::SetVolumeMountPointW(sMount.c_str(), pt.sVolumeGuid.c_str()))
            {
                r.dwMountErr = ::GetLastError();
                return false;
            }
            sTarget = std::wstring(1, c) + L":";
            r.bTempAssigned = true;
            ::Sleep(300);
        }

        // bcdboot：随包优先（知识库 70：原版内置 Config\amd64\bcdboot.exe）。
        // 本工程随包件统一放主程序目录下 amd64\，兼容旧扁平布局。
        std::wstring sBcdboot;
        {
            wchar_t szExe[MAX_PATH] = { 0 };
            ::GetModuleFileNameW(nullptr, szExe, MAX_PATH);
            wchar_t* pSlash = ::wcsrchr(szExe, L'\\');
            if (pSlash)
            {
                *pSlash = 0;
                std::wstring sExeDir = szExe;
                const wchar_t* const kRel[] = { L"\\amd64\\bcdboot.exe", L"\\bcdboot.exe" };
                for (int i = 0; i < 2; ++i)
                {
                    std::wstring sCand = sExeDir + kRel[i];
                    if (::GetFileAttributesW(sCand.c_str()) != INVALID_FILE_ATTRIBUTES)
                    {
                        sBcdboot = sCand;
                        break;
                    }
                }
            }
            if (sBcdboot.empty())
                sBcdboot = L"bcdboot.exe";
        }

        // 系统映像必须在分区根目录：%SystemDrive%\Windows
        wchar_t szSys[8] = { 0 };
        ::GetEnvironmentVariableW(L"SystemDrive", szSys, 8);
        std::wstring sWin = std::wstring(szSys) + L"\\Windows";

        // [C++17 阶段3][F13] wchar_t[600] -> std::wstring
        const size_t kCapArgs2 = 600;
        std::wstring sArgs2(kCapArgs2, L'\0');
        int nArgs2 = wfmt::FormatInto(sArgs2.data(), kCapArgs2,
            L"\"%s\" /l zh-cn /s %s /f %s",
            sWin.c_str(), sTarget.c_str(), bUefi ? L"UEFI" : L"BIOS");
        if (nArgs2 >= 0) sArgs2.resize(static_cast<size_t>(nArgs2));
        if (pSink)
            pSink->OnLog(0, L"[bootfix] " + sBcdboot + L" " + sArgs2);
        // 边界转换：ProcessRunner 签名为 DuiLib::CDuiString
        DWORD dwBc = ProcessRunner::RunHiddenExit(
            DuiLib::CDuiString(sBcdboot.c_str()),
            DuiLib::CDuiString(sArgs2.c_str()), 120000, false); // [FIX-P1H] BCD 写入超时不杀

        // 完成后立即回收临时盘符（基线同：单次 DeleteVolumeMountPointW）
        if (r.bTempAssigned)
        {
            std::wstring sRm;
            sRm.push_back(sTarget[0]); sRm.push_back(L':'); sRm.push_back(L'\\');
            ::DeleteVolumeMountPointW(sRm.c_str());
        }

        r.dwExitCode = dwBc;
        if (pSink)
        {
            // [C++17 阶段3][F13] wchar_t[128] -> std::wstring
            const size_t kCapLog2 = 128;
            std::wstring sLog2(kCapLog2, L'\0');
            int nLog2 = wfmt::FormatInto(sLog2.data(), kCapLog2,
                L"[bootfix] bcdboot exit code: %u", dwBc);
            if (nLog2 >= 0) sLog2.resize(static_cast<size_t>(nLog2));
            pSink->OnLog(dwBc == 0 ? 0 : 2, sLog2);
        }
        return true;
    }

private:
    IIsoImageService*  m_pIso;    // 构造注入（非拥有）
    IToolServiceHost*  m_pHost;   // 构造注入（非拥有，日志）
};

} // anonymous namespace

std::unique_ptr<IWizardService> CreateWizardService(IIsoImageService* pIso,
                                                    IToolServiceHost* pHost)
{
    // [C++17 阶段3] 工厂所有权移交调用方（unique_ptr 管理，无裸 delete）
    return std::make_unique<CWizardService>(pIso, pHost);
}

} // namespace toolsvc
