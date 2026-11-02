// ============================================================
// IsoMount.cpp —— [R6-04] ISO 直连挂载原语（virtdisk.dll）。
// 逐字搬迁自 ToolDialog.cpp(2026-10-13) 5895-6094，仅把
// CDuiString/_T 边界换成 std::wstring/L""，逻辑零改动。
// ============================================================

#include <windows.h>

#include <string>

#include "IsoMount.h"
#include "ProcessRunner.h"   // [FIX-P1I] WaitHandleWithUiPump：盘符探测轮询泵化
// [C++17 阶段2] 局部 RAII 守卫：虚拟磁盘/查找句柄（见 WinHandles.h 使用规范）
struct IMDiskGuard
{
    HANDLE h = INVALID_HANDLE_VALUE;
    IMDiskGuard() = default;
    ~IMDiskGuard() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    IMDiskGuard(const IMDiskGuard&) = delete;
    IMDiskGuard& operator=(const IMDiskGuard&) = delete;
    HANDLE* out() { return &h; }
    void reset() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); h = INVALID_HANDLE_VALUE; }
    operator HANDLE() const { return h; }
    bool valid() const { return h && h != INVALID_HANDLE_VALUE; }
};
struct IMFindGuard
{
    HANDLE h;
    explicit IMFindGuard(HANDLE x) : h(x) {}
    ~IMFindGuard() { if (h && h != INVALID_HANDLE_VALUE) ::FindClose(h); }
    IMFindGuard(const IMFindGuard&) = delete;
    IMFindGuard& operator=(const IMFindGuard&) = delete;
    operator HANDLE() const { return h; }
};


namespace toolsvc {

namespace {

struct TdVirtualStorageType {
    unsigned long DeviceId;
    GUID  VendorId;
};

using TdPfnOpenVirtualDisk = DWORD ( WINAPI*)(
    const TdVirtualStorageType*, const wchar_t*, DWORD /*AccessMask*/,
    DWORD /*Flags*/, const void* /*Parameters*/, HANDLE*);
using TdPfnAttachVirtualDisk = DWORD ( WINAPI*)(
    HANDLE, PSECURITY_DESCRIPTOR, DWORD /*Flags*/, DWORD /*ProviderFlags*/,
    const void* /*Parameters*/, LPOVERLAPPED);
using TdPfnDetachVirtualDisk = DWORD ( WINAPI*)(
    HANDLE, DWORD /*Flags*/, DWORD /*ProviderFlags*/);

constexpr DWORD kTdVdAccessAttachRo = 0x00010000;
constexpr DWORD kTdVdAccessDetach = 0x00040000;
constexpr DWORD kTdAttachFlagReadOnly = 0x00000001;
// [R-B90 修复] 必须同时给 PERMANENT_LIFETIME(0x4)：默认"临时生命周期"下，
// Attach 后关闭唯一持有的句柄系统会立即自动卸载磁盘 → 永远等不到盘符，
// 只能被迫降级 PowerShell，精简系统/WinPE 无 PowerShell 时即彻底失败。
constexpr DWORD kTdAttachFlagPermanentLifetime = 0x00000004;

// {EC984AEC-A0F9-47e9-901F-71415A66345B}
const GUID kTdVendorMicrosoft =
    { 0xec984aec, 0xa0f9, 0x47e9,
      { 0x90, 0x1f, 0x71, 0x41, 0x5a, 0x66, 0x34, 0x5b } };

struct TdVirtdiskApi {
    HMODULE                hMod;
    TdPfnOpenVirtualDisk   Open;
    TdPfnAttachVirtualDisk Attach;
    TdPfnDetachVirtualDisk Detach;
};

bool TdLoadVirtdisk(TdVirtdiskApi& api)
{
    api = {};
    HMODULE h = LoadLibraryW(L"virtdisk.dll");
    if (!h) return false;
    api.hMod   = h;
    api.Open   = (TdPfnOpenVirtualDisk)GetProcAddress(h, "OpenVirtualDisk");
    api.Attach = (TdPfnAttachVirtualDisk)GetProcAddress(h, "AttachVirtualDisk");
    api.Detach = (TdPfnDetachVirtualDisk)GetProcAddress(h, "DetachVirtualDisk");
    return api.Open && api.Attach && api.Detach;
}

void TdCollectCdLetters(bool letters[26])
{
    WCHAR szVol[MAX_PATH] = { 0 };
    HANDLE hFind = FindFirstVolumeW(szVol, MAX_PATH);
    if (hFind == INVALID_HANDLE_VALUE) return;
    do {
        WCHAR szNames[256] = { 0 };
        DWORD cch = 256;
        if (GetVolumePathNamesForVolumeNameW(szVol, szNames, cch, &cch)) {
            for (wchar_t* p = szNames; *p; ) {
                WCHAR c = p[0];
                if ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z'))
                    letters[towupper(c) - L'A'] = true;
                p += wcslen(p) + 1;
            }
        }
    } while (FindNextVolumeW(hFind, szVol, MAX_PATH));
    FindVolumeClose(hFind);
}

bool TdOpenIsoHandle(const TdVirtdiskApi& api, const wchar_t* pszIso, DWORD access,
                     HANDLE& phDisk)
{
    TdVirtualStorageType st;
    st.DeviceId = 1;                    // VIRTUAL_STORAGE_TYPE_DEVICE_ISO
    st.VendorId = kTdVendorMicrosoft;

    // OPEN_VIRTUAL_DISK_PARAMETERS Version1 { BOOL RWDepth }
    struct {
        DWORD Version;
        BOOL  RWDepth;
    } params;
    params.Version = 1;                // OPEN_VIRTUAL_DISK_VERSION_1
    params.RWDepth = FALSE;

    IMDiskGuard hDisk;
    DWORD ec = api.Open(&st, pszIso, access, 0, &params, hDisk.out());
    if (ec != ERROR_SUCCESS || hDisk == INVALID_HANDLE_VALUE)
        return false;
    phDisk = hDisk;
    return true;
}

// 直连挂载成功返回盘符（"X:\\"），失败返回空串
std::wstring TdNativeMount(const std::wstring& sIso)
{
    TdVirtdiskApi api;
    if (!TdLoadVirtdisk(api)) return L"";

    bool before[26] = { 0 };
    TdCollectCdLetters(before);

    IMDiskGuard hDisk;
    if (!TdOpenIsoHandle(api, sIso.c_str(), kTdVdAccessAttachRo, hDisk.h))
        return L"";

    // ATTACH_VIRTUAL_DISK_PARAMETERS Version1（仅版本号）
    DWORD attachParams = 1;
    // [R-B90] READ_ONLY + PERMANENT_LIFETIME：关闭句柄后挂载仍保持、盘符正常分配
    DWORD ec = api.Attach(hDisk, nullptr,
                         kTdAttachFlagReadOnly | kTdAttachFlagPermanentLifetime, 0,
                         &attachParams, nullptr);
    if (ec != ERROR_SUCCESS) {
        return L"";  // IMDiskGuard 自动关闭
    }
    hDisk.reset();  // 挂载状态由系统保持；守卫 reset 显式关闭（不改变时序）

    // 等待新 CD-ROM 盘符出现（最多约 6 秒）
    for (int tries = 0; tries < 30; ++tries) {
        bool after[26] = { 0 };
        TdCollectCdLetters(after);
        for (int i = 0; i < 26; ++i) {
            if (after[i] && !before[i]) {
                wchar_t szDrive[8] = { 0 };
                szDrive[0] = (wchar_t)(L'A' + i);
                szDrive[1] = L':';
                szDrive[2] = L'\\';
                szDrive[3] = L'\0';
                std::wstring sDrive = szDrive;
                // ISO 挂接为 CD-ROM 设备，校验类型与根目录可访问
                if (GetDriveTypeW(sDrive.c_str()) == DRIVE_CDROM)
                {
                    // [R-B90] 设备先到、文件系统(UDF/ISO9660)可能略晚就绪，
                    // 必须确认卷根可访问再返回，否则随后定位内部映像会抢跑失败。
                    std::wstring sProbe = sDrive + L"*";
                    WIN32_FIND_DATAW fdProbe;
                    IMFindGuard hProbe(::FindFirstFileW(sProbe.c_str(), &fdProbe));
                    if (hProbe != INVALID_HANDLE_VALUE)
                    {
                        return sDrive;
                    }
                }
            }
        }
        // [FIX-P1I] 裸 Sleep(200)×30 在 UI 线程最长 6s 不泵消息（超 Windows
        // 5s "未响应"阈值）。改白名单泵轮询：绘制保持、输入丢弃。
        ProcessRunner::WaitHandleWithUiPump(nullptr, 200);
    }
    return L"";
}

// 直连卸载；返回是否成功
bool TdNativeDetach(const std::wstring& sIso)
{
    TdVirtdiskApi api;
    if (!TdLoadVirtdisk(api)) return false;
    IMDiskGuard hDisk;
    if (!TdOpenIsoHandle(api, sIso.c_str(), kTdVdAccessDetach, hDisk.h))
        return false;
    bool ok = (api.Detach(hDisk, 0, 0) == ERROR_SUCCESS);
    return ok;  // IMDiskGuard 自动关闭
}

} // anonymous namespace

std::wstring IsoMount(const std::wstring& sIso)
{
    if (sIso.empty()) return L"";
    return TdNativeMount(sIso);
}

bool IsoDismount(const std::wstring& sIso)
{
    if (sIso.empty()) return false;
    return TdNativeDetach(sIso);
}

std::wstring IsoResolveInner(const std::wstring& sMount)
{
    if (sMount.empty()) return L"";
    std::wstring sBase = sMount;
    if (sBase.empty() || sBase[sBase.size() - 1] != L'\\') sBase += L'\\';
    static const wchar_t* const kRel[] = {
        L"sources\\install.wim",
        L"sources\\install.esd",
        L"x64\\sources\\install.wim",
        L"x64\\sources\\install.esd",
        L"x86\\sources\\install.wim",
        L"x86\\sources\\install.esd",
        L"install.wim",
        L"install.esd",
    };
    for (int i = 0; i < static_cast<int>(sizeof(kRel) / sizeof(kRel[0])); ++i)
    {
        std::wstring sCand = sBase + kRel[i];
        if (::GetFileAttributesW(sCand.c_str()) != INVALID_FILE_ATTRIBUTES)
            return sCand;
    }
    return L"";
}

} // namespace toolsvc
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
