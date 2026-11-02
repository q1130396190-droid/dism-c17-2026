// WofCompact.cpp
// ------------------------------------------------------------------
// [P0-A2] WOF/CompactOS 原生实现，替代 compact.exe 与 PowerShell。
// 协议与原版 Dism++ 字节级同构（Ghidra 证据见头文件注释）：
//   SET 0x9030C / GET 0x90310 / DELETE 0x90314，20 字节输入缓冲。
// ------------------------------------------------------------------
#include "stdafx.h"
#include "WofCompact.h"

#include <winnt.h>
#include <shlwapi.h>
#include <vector>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "version.lib")

// ---- SDK 常量兜底（旧 SDK 无定义时补齐；新 SDK 走 #ifndef） ----
#ifndef WOF_PROVIDER_FILE
#define WOF_PROVIDER_FILE 2ul
#endif

#ifndef FILE_PROVIDER_COMPRESSION_XPRESS4K
#define FILE_PROVIDER_COMPRESSION_XPRESS4K  0ul
#define FILE_PROVIDER_COMPRESSION_XPRESS8K  1ul
#define FILE_PROVIDER_COMPRESSION_XPRESS16K 2ul
#define FILE_PROVIDER_COMPRESSION_LZX       3ul
#endif

#ifndef FSCTL_SET_EXTERNAL_BACKING
// FILE_DEVICE_FILE_SYSTEM=9, func 195/196/197, METHOD_BUFFERED, FILE_ANY_ACCESS
#define FSCTL_SET_EXTERNAL_BACKING    CTL_CODE(FILE_DEVICE_FILE_SYSTEM,195,METHOD_BUFFERED,FILE_ANY_ACCESS)
#define FSCTL_GET_EXTERNAL_BACKING    CTL_CODE(FILE_DEVICE_FILE_SYSTEM,196,METHOD_BUFFERED,FILE_ANY_ACCESS)
#define FSCTL_DELETE_EXTERNAL_BACKING CTL_CODE(FILE_DEVICE_FILE_SYSTEM,197,METHOD_BUFFERED,FILE_ANY_ACCESS)
#endif

struct WOF_EXTERNAL_INFO_DEF
{
    DWORD Version;
    DWORD Provider;
};

struct FILE_PROVIDER_EXTERNAL_INFO_V1_DEF
{
    DWORD Version;
    DWORD Algorithm;
    DWORD Flags;
};

namespace
{

// ---- 版本：RtlGetVersion（不受兼容性 manifest 影响） ----
bool RtlVersion(OSVERSIONINFOEXW& ovi)
{
    ovi = {};
    ovi.dwOSVersionInfoSize = sizeof(ovi);
    HMODULE hNtdll = ::GetModuleHandleW(L"ntdll.dll");
    if (!hNtdll) return false;
    using PFN_RtlGetVersion = LONG ( WINAPI*)(OSVERSIONINFOEXW*);
    PFN_RtlGetVersion pfn = reinterpret_cast<PFN_RtlGetVersion>(
        ::GetProcAddress(hNtdll, "RtlGetVersion"));
    return pfn && pfn(&ovi) == 0;
}

// Win10(build 10000) 及以上：系统 wof.sys 原生支持文件提供程序压缩，
// 直接发 FSCTL 即可。Win8.1(6.3) 虽自带 wof.sys，但只支持 WIM 启动
// 场景，FSCTL_SET_EXTERNAL_BACKING 的 FILE provider 是 Win10 新增，
// 故 6.3 也必须走 wofadk（与原版 build<0xA0000001 门槛一致）。
bool IsWindows10OrGreaterBuild()
{
    OSVERSIONINFOEXW ovi;
    if (!RtlVersion(ovi)) return false;
    return (ovi.dwMajorVersion > 10)
        || (ovi.dwMajorVersion == 10 && ovi.dwBuildNumber >= 10000);
}

// 以备份语义打开（目录/文件均可），不跟随 reparse 点
HANDLE OpenForWof(const wchar_t* pszPath, DWORD dwAccess)
{
    return ::CreateFileW(pszPath, dwAccess,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                         nullptr);
}

bool g_bTriedAdk = false;
bool g_bAdkReady  = false;

// [C++17 升级阶段2] 句柄 RAII 守卫：隐式转换为原句柄类型，调用点零改动；
// 只负责早退/异常路径自动释放，不改变任何调用顺序与业务分支。
// hFlt（fltlib.dll 模块句柄，FreeLibrary 保留）为模块生命周期，保持显式。
struct RegKeyGuard
{
    RegKeyGuard() = default;
    ~RegKeyGuard() { if (m_h) ::RegCloseKey(m_h); }
    RegKeyGuard(const RegKeyGuard&) = delete;
    RegKeyGuard& operator=(const RegKeyGuard&) = delete;
    operator HKEY() const { return m_h; }
    HKEY* out() { return &m_h; }
private:
    HKEY m_h = nullptr;
};

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

// Win7/8.x 在线安装/加载随包 wofadk.sys（原版 FUN_1400207bc/140007b0c 同构）。
// 仅在 IsSupported() 探测到旧系统时执行一次。
// [R4] 旧系统兼容路径的失败回滚上下文。
// 该路径会复制内核驱动、创建 Boot 启动服务并禁用系统 Wof；
// 任一步失败都必须恢复到调用前状态，否则可能留下无法启动的过滤器配置。
struct WofAdkRollback
{
    std::wstring sDriverDst;
    bool         bDriverExisted = false;
    bool         bServiceExisted = false;
    bool         bWofExisted = false;
    DWORD        dwWofStart = 0;
    bool         bWofStartSaved = false;

    void Restore() const
    {
        if (!bServiceExisted)
        {
            // 本次新建的服务：整棵子树删除，恢复系统原状。
            // 使用服务控制管理器删除，避免直接改注册表留下悬挂实例。
            SC_HANDLE hScm = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
            if (hScm)
            {
                SC_HANDLE hSvc = ::OpenServiceW(hScm, L"WofAdk", DELETE | SERVICE_STOP);
                if (hSvc)
                {
                    SERVICE_STATUS st = { 0 };
                    // [R128-A] 停止失败仍继续删除/压缩会操作使用中的 Wof 驱动；
                    //   这里只留痕不中断（此处属清理链，中断会留下更糟的残留），
                    //   但失败必须可见。
                    if (!::ControlService(hSvc, SERVICE_CONTROL_STOP, &st))
                        ::OutputDebugStringW(L"[R128][wof] WofAdk 停止失败\r\n");
                    if (!::DeleteService(hSvc))
                        ::OutputDebugStringW(L"[R128][wof] WofAdk 删除失败\r\n");
                    ::CloseServiceHandle(hSvc);
                }
                ::CloseServiceHandle(hScm);
            }
        }
        // [R120] 回滚路径：删掉本次新装的驱动文件。
        //   **刻意不检查返回值也不留痕** —— 这是回滚链条中的一步，
        //   在其前后还有删服务、恢复注册表等动作；若为此处加分支，
        //   会打断回滚流程，后果比残留一个驱动文件严重得多。
        if (!bDriverExisted && !sDriverDst.empty())
            ::DeleteFileW(sDriverDst.c_str());

        // 恢复系统自带 Wof 的原始启动类型
        if (bWofExisted && bWofStartSaved)
        {
            RegKeyGuard hWof;
            if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                    L"SYSTEM\\CurrentControlSet\\Services\\Wof",
                    0, KEY_SET_VALUE, hWof.out()) == ERROR_SUCCESS)
            {
                DWORD dw = dwWofStart;
                // [R118] 这是**回滚路径**里的恢复写入：即使失败也不能中断回滚，
                //   否则后面的清理步骤（删驱动、删服务）都不会执行，留下更糟的残留。
                //   故此处刻意不检查返回值 —— 与正向写键的处理不同，理由如上。
                ::RegSetValueExW(hWof, L"Start", 0, REG_DWORD,
                                 reinterpret_cast<BYTE*>(&dw), sizeof(dw));
            }
        }
    }
};

bool EnsureWofAdkOnline()
{
    if (g_bTriedAdk) return g_bAdkReady;
    g_bTriedAdk = true;

    // [R4] 失败回滚上下文：记录调用前的驱动/服务/Wof 状态。
    WofAdkRollback rb;

    OSVERSIONINFOEXW ovi;
    if (!RtlVersion(ovi)) return false;
    // Win10 之前（含 Win8.1 6.3）均需 wofadk；ntdll < 6.1（Vista 及以下）
    // 原版直接放弃（FUN_1400207bc 版本门槛 0x60001）。
    if (ovi.dwMajorVersion != 6 || ovi.dwMinorVersion < 1)
        return false;

    wchar_t szExe[MAX_PATH] = { 0 }, szWin[MAX_PATH] = { 0 };
    ::GetModuleFileNameW(nullptr, szExe, _countof(szExe));
    ::GetWindowsDirectoryW(szWin, _countof(szWin));
    std::wstring sExeDir(szExe);
    size_t p = sExeDir.find_last_of(L"\\/");
    if (p == std::wstring::npos) return false;
    sExeDir.resize(p);
    // 随包 wofadk.sys 探测：新布局 amd64\ 优先（架构专属依赖目录），
    // 再回退原版 Config\ 与旧扁平布局。
    static const wchar_t* const kWofRel[] = {
        L"\\amd64\\wofadk.sys", L"\\Config\\wofadk.sys", L"\\wofadk.sys" };
    std::wstring sSrc;
    for (int i = 0; i < 3; ++i)
    {
        std::wstring sCand = sExeDir + kWofRel[i];
        if (::GetFileAttributesW(sCand.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            sSrc = sCand;
            break;
        }
    }
    if (sSrc.empty()) return false;
    std::wstring sDst = std::wstring(szWin) + L"\\System32\\drivers\\wofadk.sys";
    // [R4] 记录原始状态：复制或后续任一步失败时恢复。
    rb.sDriverDst = sDst;
    rb.bDriverExisted = (::GetFileAttributesW(sDst.c_str()) != INVALID_FILE_ATTRIBUTES);
    if (!::CopyFileW(sSrc.c_str(), sDst.c_str(), FALSE))
    {
        rb.Restore();
        return false;
    }

    // 服务键 Services\\WofAdk（Start=0 Boot / Type=2 / FSFilter Compression 组）
    // [R4] 判断服务是否本次新建，决定失败时是否删除。
    {
        HKEY hProbe = nullptr;
        rb.bServiceExisted =
            (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                L"SYSTEM\\CurrentControlSet\\Services\\WofAdk",
                0, KEY_READ, &hProbe) == ERROR_SUCCESS);
        if (hProbe) ::RegCloseKey(hProbe);
    }
    RegKeyGuard hSvc;
    if (::RegCreateKeyExW(HKEY_LOCAL_MACHINE,
            L"SYSTEM\\CurrentControlSet\\Services\\WofAdk",
            0, nullptr, 0, KEY_WRITE, nullptr, hSvc.out(), nullptr) != ERROR_SUCCESS)
    {
        rb.Restore();
        return false;
    }
    // [R118] 原实现在这里连写 12 个服务键，全部丢弃 RegSetValueExW 的返回值，
    //   然后无条件走到 FilterLoad。若某个键写失败（权限/磁盘/键被占用），
    //   会留下一个**配置不完整的 WofAdk 服务**：能加载但行为异常，
    //   或者下次开机时服务起不来 —— 而调用方拿到的仍是"成功"。
    //   改法：累积首个失败码，写完/加载后统一判断，失败即走既有 rb.Restore() 回滚。
    DWORD dw;
    LSTATUS lsSvc = ERROR_SUCCESS;   // 首个写键失败码，ERROR_SUCCESS = 全部成功
    // 宏：写一个值并记录首个失败，不改变原有写入顺序与参数
#define WOFC_SET(h, name, type, data, cb)                                   \
    do {                                                                    \
        LSTATUS _ls = ::RegSetValueExW((h), (name), 0, (type), (data), (cb)); \
        if (_ls != ERROR_SUCCESS && lsSvc == ERROR_SUCCESS) lsSvc = _ls;     \
    } while (0)

    dw = 0; WOFC_SET(hSvc, L"Start", REG_DWORD, reinterpret_cast<BYTE*>(&dw), 4);
    dw = 2; WOFC_SET(hSvc, L"Type", REG_DWORD, reinterpret_cast<BYTE*>(&dw), 4);
    dw = 1; WOFC_SET(hSvc, L"ErrorControl", REG_DWORD, reinterpret_cast<BYTE*>(&dw), 4);
    WOFC_SET(hSvc, L"Group", REG_SZ,
        const_cast<BYTE*>(reinterpret_cast<const BYTE*>(L"FSFilter Compression")),
        (DWORD)((wcslen(L"FSFilter Compression") + 1) * sizeof(WCHAR)));
    WOFC_SET(hSvc, L"ImagePath", REG_EXPAND_SZ,
        const_cast<BYTE*>(reinterpret_cast<const BYTE*>(L"\\SystemRoot\\System32\\drivers\\wofadk.sys")),
        (DWORD)((wcslen(L"\\SystemRoot\\System32\\drivers\\wofadk.sys") + 1) * sizeof(WCHAR)));
    static const WCHAR kDep[] = L"FltMgr\0";   // MULTI_SZ：串 + 双 \0
    WOFC_SET(hSvc, L"DependOnService", REG_MULTI_SZ,
        const_cast<BYTE*>(reinterpret_cast<const BYTE*>(kDep)), sizeof(kDep));
    WOFC_SET(hSvc, L"Description", REG_SZ,
        const_cast<BYTE*>(reinterpret_cast<const BYTE*>(L"WofAdk Mini-Filter Driver")),
        (DWORD)((wcslen(L"WofAdk Mini-Filter Driver") + 1) * sizeof(WCHAR)));
    WOFC_SET(hSvc, L"DisplayName", REG_SZ,
        const_cast<BYTE*>(reinterpret_cast<const BYTE*>(L"WofAdk")), (DWORD)((wcslen(L"WofAdk") + 1) * sizeof(WCHAR)));
    dw = 3; WOFC_SET(hSvc, L"SupportedFeatures", REG_DWORD, reinterpret_cast<BYTE*>(&dw), 4);

    RegKeyGuard hInst;
    if (::RegCreateKeyExW(hSvc, L"Instances", 0, nullptr, 0, KEY_WRITE, nullptr, hInst.out(), nullptr)
        == ERROR_SUCCESS)
    {
        WOFC_SET(hInst, L"DefaultInstance", REG_SZ,
            const_cast<BYTE*>(reinterpret_cast<const BYTE*>(L"WofAdk Instance")),
            (DWORD)((wcslen(L"WofAdk Instance") + 1) * sizeof(WCHAR)));
        RegKeyGuard hOne;
        if (::RegCreateKeyExW(hInst, L"WofAdk Instance", 0, nullptr, 0,
                              KEY_WRITE, nullptr, hOne.out(), nullptr) == ERROR_SUCCESS)
        {
            WOFC_SET(hOne, L"Altitude", REG_SZ,
                const_cast<BYTE*>(reinterpret_cast<const BYTE*>(L"40730")), (DWORD)((wcslen(L"40730") + 1) * sizeof(WCHAR)));
            dw = 0; WOFC_SET(hOne, L"Flags", REG_DWORD, reinterpret_cast<BYTE*>(&dw), 4);
        }
    }

    // 系统自带 Wof 在旧系统上让位（Start=4 disabled）
    RegKeyGuard hWof;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SYSTEM\\CurrentControlSet\\Services\\Wof",
            0, KEY_READ | KEY_SET_VALUE, hWof.out()) == ERROR_SUCCESS)
    {
        // [R4] 先保存原 Start，失败回滚时恢复，避免残留“Wof 被禁用”状态。
        DWORD dwOld = 0, cbOld = sizeof(dwOld), dwType = 0;
        if (::RegQueryValueExW(hWof, L"Start", nullptr, &dwType,
                               reinterpret_cast<BYTE*>(&dwOld), &cbOld) == ERROR_SUCCESS)
        {
            rb.bWofExisted = true;
            rb.dwWofStart = dwOld;
            rb.bWofStartSaved = true;
        }
        dw = 4; WOFC_SET(hWof, L"Start", REG_DWORD, reinterpret_cast<BYTE*>(&dw), 4);
    }

    // [R118] 写键失败则不再往下走：带着不完整配置去 FilterLoad 只会得到
    //   更难诊断的失败（或"看似成功但行为异常"）。这里直接回滚并返回失败。
    if (lsSvc != ERROR_SUCCESS)
    {
        rb.Restore();
        return false;
    }

    // fltlib!FilterLoad(L"WofAdk")
    HMODULE hFlt = ::LoadLibraryW(L"fltlib.dll");
    if (!hFlt) { rb.Restore(); return false; }
    using PFN_FilterLoad = HRESULT ( WINAPI*)(const wchar_t*);
    PFN_FilterLoad pLoad = reinterpret_cast<PFN_FilterLoad>(
        ::GetProcAddress(hFlt, "FilterLoad"));
    if (!pLoad) { rb.Restore(); return false; }
    HRESULT hr = pLoad(L"WofAdk");
    // 服务型 minifilter 已运行时返回 FLT_INSTANCE_ALTITUDE_COLLISION 等，按成功宽松处理
    g_bAdkReady = SUCCEEDED(hr)
        || hr == static_cast<HRESULT>(0x801F0013) /*FLT_INSTANCE_ALTITUDE_COLLISION*/
        || hr == HRESULT_FROM_WIN32(ERROR_SERVICE_ALREADY_RUNNING);
    // [R4] 过滤器加载失败：撤销驱动/服务/Wof 改动，避免残留不可用过滤器配置。
    if (!g_bAdkReady)
        rb.Restore();
#undef WOFC_SET
    return g_bAdkReady;
}

// 黑名单（原版 FUN_14001fee8 文件名表，hvloader 等引导关键件不可压）
bool IsBlacklisted(const wchar_t* pszName)
{
    static const wchar_t* kList[] = {
        L"hvloader.exe", L"hvix64.exe", L"hvax64.exe",
        L"ntldr", L"bootmgr",
    };
    for (const wchar_t* p : kList)
        if (lstrcmpiW(pszName, p) == 0) return true;
    return false;
}

bool IsPeMzFile(const std::wstring& sPath, DWORD dwAttr)
{
    if (dwAttr & FILE_ATTRIBUTE_DIRECTORY) return false;
    FileGuard h = ::CreateFileW(sPath.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                             nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    BYTE b[2] = { 0 };
    DWORD dwRead = 0;
    BOOL bOk = ::ReadFile(h, b, 2, &dwRead, nullptr) && dwRead == 2 && b[0] == 'M' && b[1] == 'Z';
    return bOk != FALSE;
}

// 压缩/解压树递归核心
struct WalkCtx
{
    int  nAlgorithm;
    bool bUncompact;
    bool bCompactOsRules;     // CompactOS 7 天/系统文件/黑名单/PE 过滤
    WofCompact::PFN_PROGRESS pfn;
    void* pUser;
    WofCompact::Progress st;
    unsigned long long ulCutoff;   // 7 天前（100ns）
};

void WalkRecursive(const std::wstring& sDir, WalkCtx& c, unsigned nDepth)
{
    if (nDepth > 64) return;
    std::wstring sSpec = sDir + L"\\*";
    WIN32_FIND_DATAW fd = { 0 };
    FindGuard hFind = ::FindFirstFileExW(sSpec.c_str(), FindExInfoBasic, &fd,
                                      FindExSearchNameMatch, nullptr,
                                      FIND_FIRST_EX_LARGE_FETCH);
    if (hFind == INVALID_HANDLE_VALUE) return;
    do
    {
        if (fd.cFileName[0] == L'.'
            && (fd.cFileName[1] == L'\0'
                || (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0')))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            continue;   // reparse/junction/symlink 全部跳过

        std::wstring sFull = sDir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            WalkRecursive(sFull, c, nDepth + 1);
            continue;
        }

        ++c.st.nScanned;

        if (c.bCompactOsRules)
        {
            // 系统属性文件不压；黑名单不压
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) { ++c.st.nSkipped; continue; }
            if (IsBlacklisted(fd.cFileName))                 { ++c.st.nSkipped; continue; }
            // 仅压缩 7 天未改动的文件
            ULARGE_INTEGER ft;
            ft.LowPart  = fd.ftLastWriteTime.dwLowDateTime;
            ft.HighPart = fd.ftLastWriteTime.dwHighDateTime;
            if (ft.QuadPart > c.ulCutoff)                   { ++c.st.nSkipped; continue; }
            // mode0：.dll 必须是真 PE（MZ）才动
            size_t cchName = wcslen(fd.cFileName);
            if (cchName >= 4 && lstrcmpiW(fd.cFileName + cchName - 4, L".dll") == 0)
            {
                if (!IsPeMzFile(sFull, fd.dwFileAttributes)) { ++c.st.nSkipped; continue; }
            }
        }

        bool bBacked = WofCompact::IsFileBacked(sFull.c_str());
        bool bOk = false;
        if (c.bUncompact)
        {
            if (!bBacked) { ++c.st.nSkipped; continue; }
            bOk = WofCompact::UncompactFile(sFull.c_str());
        }
        else
        {
            if (bBacked)  { ++c.st.nSkipped; continue; }
            bOk = WofCompact::CompactFile(sFull.c_str(), c.nAlgorithm);
        }
        if (bOk) ++c.st.nChanged; else ++c.st.nFailed;

        if (c.pfn && (c.st.nScanned & 0x3F) == 0)
        {
            c.st.sCurrent = sFull;
            if (!c.pfn(c.st, c.pUser)) return;   // FindGuard 自动关闭
        }
    } while (::FindNextFileW(hFind, &fd));
}

void WriteCompactMarker(DWORD dwValue)
{
    RegKeyGuard h;
    if (::RegCreateKeyExW(HKEY_LOCAL_MACHINE,
            L"SYSTEM\\CurrentControlSet\\Setup",
            0, nullptr, 0, KEY_SET_VALUE, nullptr, h.out(), nullptr) == ERROR_SUCCESS)
    {
        // [R128-A] 标记写失败则下次启动无法判断压缩状态，留痕
        if (::RegSetValueExW(h, L"Compact", 0, REG_DWORD,
                         reinterpret_cast<BYTE*>(&dwValue), 4) != ERROR_SUCCESS)
            ::OutputDebugStringW(L"[R128][wof] Compact 标记写入失败\r\n");
    }
}

} // namespace

namespace WofCompact
{

int AlgorithmFromName(const wchar_t* pszName)
{
    if (!pszName) return -1;
    if (lstrcmpiW(pszName, L"xpress4k") == 0)  return static_cast<int>(Algorithm::AlgXpress4k);
    if (lstrcmpiW(pszName, L"xpress8k") == 0)  return static_cast<int>(Algorithm::AlgXpress8k);
    if (lstrcmpiW(pszName, L"xpress16k") == 0) return static_cast<int>(Algorithm::AlgXpress16k);
    if (lstrcmpiW(pszName, L"lzx") == 0)       return static_cast<int>(Algorithm::AlgLzx);
    return -1;
}

bool IsSupported()
{
    if (IsWindows10OrGreaterBuild()) return true;
    return EnsureWofAdkOnline();
}

bool IsFileBacked(const wchar_t* pszPath)
{
    FileGuard h = OpenForWof(pszPath, GENERIC_READ);
    if (h == INVALID_HANDLE_VALUE) return false;
    BYTE buf[0x30] = { 0 };
    DWORD cb = 0;
    BOOL b = ::DeviceIoControl(h, FSCTL_GET_EXTERNAL_BACKING,
                               nullptr, 0, buf, sizeof(buf), &cb, nullptr);
    return b != FALSE;
}

bool CompactFile(const wchar_t* pszPath, int nAlgorithm, DWORD* pdwLastError)
{
    BYTE buf[sizeof(WOF_EXTERNAL_INFO_DEF)
           + sizeof(FILE_PROVIDER_EXTERNAL_INFO_V1_DEF)] = { 0 };
    WOF_EXTERNAL_INFO_DEF* pWof = reinterpret_cast<WOF_EXTERNAL_INFO_DEF*>(buf);
    FILE_PROVIDER_EXTERNAL_INFO_V1_DEF* pFp =
        reinterpret_cast<FILE_PROVIDER_EXTERNAL_INFO_V1_DEF*>(pWof + 1);
    pWof->Version   = 1;
    pWof->Provider  = WOF_PROVIDER_FILE;
    pFp->Version    = 1;
    pFp->Algorithm  = static_cast<DWORD>(nAlgorithm);
    pFp->Flags      = 0;

    FileGuard h = OpenForWof(pszPath, GENERIC_READ | GENERIC_WRITE);
    if (h == INVALID_HANDLE_VALUE)
    {
        if (pdwLastError) *pdwLastError = ::GetLastError();
        return false;
    }
    DWORD cb = 0;
    BOOL b = ::DeviceIoControl(h, FSCTL_SET_EXTERNAL_BACKING,
                               buf, static_cast<DWORD>(sizeof(buf)), nullptr, 0, &cb, nullptr);
    if (!b && pdwLastError) *pdwLastError = ::GetLastError();
    return b != FALSE;
}

bool UncompactFile(const wchar_t* pszPath, DWORD* pdwLastError)
{
    FileGuard h = OpenForWof(pszPath, GENERIC_READ | GENERIC_WRITE);
    if (h == INVALID_HANDLE_VALUE)
    {
        if (pdwLastError) *pdwLastError = ::GetLastError();
        return false;
    }
    DWORD cb = 0;
    BOOL b = ::DeviceIoControl(h, FSCTL_DELETE_EXTERNAL_BACKING,
                               nullptr, 0, nullptr, 0, &cb, nullptr);
    if (!b && pdwLastError) *pdwLastError = ::GetLastError();
    return b != FALSE;
}

void CompactTree(const wchar_t* pszPath, int nAlgorithm, bool bUncompact,
                 PFN_PROGRESS pfnProgress, void* pCtx)
{
    if (!pszPath || !*pszPath) return;
    DWORD dwAttr = ::GetFileAttributesW(pszPath);
    if (dwAttr == INVALID_FILE_ATTRIBUTES) return;

    WalkCtx c = { 0 };
    c.nAlgorithm       = nAlgorithm;
    c.bUncompact       = bUncompact;
    c.bCompactOsRules  = false;   // 右键动作：无 7 天/系统文件过滤
    c.pfn              = pfnProgress;
    c.pUser            = pCtx;
    c.ulCutoff         = 0;

    if (dwAttr & FILE_ATTRIBUTE_DIRECTORY)
    {
        WalkRecursive(pszPath, c, 0);
    }
    else
    {
        bool bBacked = IsFileBacked(pszPath);
        bool bOk = false;
        if (bUncompact) { if (bBacked) bOk = UncompactFile(pszPath); }
        else            { if (!bBacked) bOk = CompactFile(pszPath, nAlgorithm); }
        c.st.nScanned = 1;
        if (bOk) c.st.nChanged = 1; else if (bBacked != !bUncompact) c.st.nSkipped = 1;
        else c.st.nFailed = 1;
    }
    if (pfnProgress)
    {
        c.st.sCurrent = pszPath;
        pfnProgress(c.st, pCtx);
    }
}

int QueryCompactOsState()
{
    if (!IsSupported()) return -1;

    // 原生判定：抽样 %SystemRoot%\\System32 顶层 OS 二进制的外部支持比例。
    // compact.exe 内部同样按系统文件是否压缩计数（ON/OFF/MIXED）。
    wchar_t szWin[MAX_PATH] = { 0 };
    ::GetWindowsDirectoryW(szWin, _countof(szWin));
    std::wstring sSpec = std::wstring(szWin) + L"\\System32\\*";

    unsigned nTotal = 0, nBacked = 0;
    const unsigned kMaxSample = 512;
    WIN32_FIND_DATAW fd = { 0 };
    FindGuard hFind = ::FindFirstFileExW(sSpec.c_str(), FindExInfoBasic, &fd,
                                      FindExSearchNameMatch, nullptr,
                                      FIND_FIRST_EX_LARGE_FETCH);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
                continue;
            size_t cch = wcslen(fd.cFileName);
            if (cch < 4) continue;
            if (lstrcmpiW(fd.cFileName + cch - 4, L".dll") != 0
                && lstrcmpiW(fd.cFileName + cch - 4, L".exe") != 0)
                continue;
            std::wstring sFull = std::wstring(szWin) + L"\\System32\\" + fd.cFileName;
            ++nTotal;
            if (IsFileBacked(sFull.c_str())) ++nBacked;
        } while (nTotal < kMaxSample && ::FindNextFileW(hFind, &fd));
    }
    if (nTotal == 0) return -1;
    if (nBacked == 0) return 0;
    // 90% 以上已压缩视为 Optimized，与 compact.exe 的 ON 口径接近
    return (nBacked * 10 >= nTotal * 9) ? 2 : 1;
}

int GetForceAlgorithm()
{
    RegKeyGuard h;
    DWORD dw = 0, cb = sizeof(dw), dwType = 0;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\WOF",
            0, KEY_READ, h.out()) == ERROR_SUCCESS)
    {
        if (::RegQueryValueExW(h, L"ForceAlgorithm", nullptr, &dwType,
                               reinterpret_cast<BYTE*>(&dw), &cb) == ERROR_SUCCESS
            && dwType == REG_DWORD && dw <= 3)
        {
            return static_cast<int>(dw);
        }
    }
    return 0;
}

bool OptimizeCompactOs(PFN_PROGRESS pfnProgress, void* pCtx)
{
    if (!IsSupported()) return false;

    wchar_t szWin[MAX_PATH] = { 0 };
    ::GetWindowsDirectoryW(szWin, _countof(szWin));

    // 原版 FUN_140020df0：ForceAlgorithm（>3 视为 0）
    int nAlg = GetForceAlgorithm();
    if (nAlg == 0) nAlg = static_cast<int>(Algorithm::AlgXpress4k);
    else nAlg -= 1;   // 注册表 1/2/3 -> FILE_PROVIDER_COMPRESSION 0/1/2

    WalkCtx c = { 0 };
    c.nAlgorithm      = nAlg;
    c.bUncompact      = false;
    c.bCompactOsRules = true;
    c.pfn             = pfnProgress;
    c.pUser           = pCtx;
    FILETIME ft;
    ::GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER ulNow;
    ulNow.LowPart  = ft.dwLowDateTime;
    ulNow.HighPart = ft.dwHighDateTime;
    c.ulCutoff = ulNow.QuadPart - (7ull * 86400ull * 10000000ull); // 7 天

    WalkRecursive(szWin, c, 0);

    // 原版：压缩完成写目标蜂巢 Setup!Compact = 1
    WriteCompactMarker(1);

    if (pfnProgress) { c.st.sCurrent = szWin; pfnProgress(c.st, pCtx); }
    return true;
}

bool UncompactCompactOs(PFN_PROGRESS pfnProgress, void* pCtx)
{
    if (!IsSupported()) return false;

    wchar_t szWin[MAX_PATH] = { 0 };
    ::GetWindowsDirectoryW(szWin, _countof(szWin));

    WalkCtx c = { 0 };
    c.nAlgorithm      = 0;
    c.bUncompact      = true;
    c.bCompactOsRules = false;   // 撤销：凡已压缩一律解
    c.pfn             = pfnProgress;
    c.pUser           = pCtx;

    WalkRecursive(szWin, c, 0);
    WriteCompactMarker(0);

    if (pfnProgress) { c.st.sCurrent = szWin; pfnProgress(c.st, pCtx); }
    return true;
}

} // namespace WofCompact
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
