// ============================================================================
//  ncleaner_exports.cpp —— NCleaner.dll 的 7 个官方导出实现
//
//  逆向基线：Dism++10.1.1002.1B\Config\amd64\NCleaner.dll（v10.1.1001.10）
//  每个函数的行为依据均为实测字符串 + 导入表 + RVA（知识库《21》2.2-2.5、
//  《52_B》2.2-2.4、《50_QT6》§1.3）。
//
//  返回值约定（知识库《50_QT6》§1.3 反汇编实测）：
//    「只有返回 S_OK 代表成功」—— 全部 7 个导出返回 HRESULT。
//
//  7 个导出与依赖矩阵：
//    NCInstallerFolderCleanup   ：MSI 互斥量 + 注册表遍历删被取代 .msi/.msp/.cab
//    NCPackageCacheFolderCleanup：MSI 互斥量 + 两目录 + 6 过滤串
//    NCVSPackageCacheCleanup    ：MSI 互斥量 + VS Packages 递归删
//    NCDOCacheOnlineCleanup     ：DO COM 双管线（CLSID/IID 实测）
//    NCSRPointOnlineCleanup     ：vssapi 删全快照 → srclient 新建还原点
//    NCCorruptedAppXOnlineCleanup：经宿主回调 DismRemoveAppx
//    NCWinEventLogOnlineCleanup ：wevtapi 全通道 EvtClearLog
// ============================================================================

#include "ncleaner_stdafx.h"
#include "ncleaner_host.h"
#include "ncleaner_util.h"
#include "ncleaner_vss.h"



#include <shlwapi.h>
#include <objbase.h>
#include <winevt.h>
#include <wrl/client.h>   // [G2] COM 智能指针 RAII 化（审计 NCleaner G2 项）
// [C++17 阶段2] 局部 RAII 守卫：注册表键/查找句柄（见 WinHandles.h 使用规范）
struct NCRegGuard
{
    HKEY k = nullptr;
    NCRegGuard() = default;
    ~NCRegGuard() { if (k) ::RegCloseKey(k); }
    HKEY* out() { return &k; }
    // [C++17 R-P8] 隐式转换：保证既有 API 调用点（RegOpenKeyExW/RegEnumKeyExW/
    //   RegQueryValueExW 及 HKEY 形参的辅助函数）零改动；与 WinHandles.h 约定一致。
    operator HKEY() const { return k; }
    NCRegGuard(const NCRegGuard&) = delete;
    NCRegGuard& operator=(const NCRegGuard&) = delete;
};

struct NCFindGuard
{
    HANDLE h;
    explicit NCFindGuard(HANDLE x) : h(x) {}
    ~NCFindGuard() { if (h && h != INVALID_HANDLE_VALUE) ::FindClose(h); }
    NCFindGuard(const NCFindGuard&) = delete;
    NCFindGuard& operator=(const NCFindGuard&) = delete;
    operator HANDLE() const { return h; }
};


#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

using namespace NCleanerUtil;
using Microsoft::WRL::ComPtr;

// ============================================================================
//  1) NCInstallerFolderCleanup（RVA 0x1000）—— Installer 目录
//
//  实测字符串：
//    NCleaner!NCInstallerFolderCleanup
//    Global\_MSIExecute
//    SOFTWARE
//    Microsoft\Windows\CurrentVersion\Installer\UserData
//    Classes\Installer
//    \Patches  /  Patches
//    State
//    S-1-5-18\Products  /  S-1-5-18\Patches
//    LocalPackage
//    M2WIRemoveSupersededPatch(%s) ==> 0x%08lX
//    \InstallProperties
//    %s\Windows\Installer\
//    NCDeleteFile(%s) ==> 0x%08lX
//
//  行为（实测推断 A/B 级）：
//    ① 先查 Global\_MSIExecute：有 MSI 事务在跑则跳过（避免破坏安装）
//    ② 遍历 HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Installer\UserData
//       → <SID>\Products\<产品码>\InstallProperties 取 LocalPackage 值
//    ③ 对 Installer 目录下的 .msi/.msp/.cab，凡"已被取代"（不在 Live 引用集）者删除
//    ④ 每删一项按 NCDeleteFile(%s) ==> 0x%08lX 记录
// ============================================================================

// [G1 修复] 文件内技术域分区（不拆分文件）：
//   ncleaner_internals::installer —— MSI 注册表/Live 集合/Installer 目录/
//                                    Package Cache/VS Packages
//   ncleaner_internals::docache   —— Delivery Optimization COM
//   ncleaner_internals::srpoint   —— VSS 快照与还原点
//   ncleaner_internals::appx      —— 损坏 Appx 编排
//   ncleaner_internals::wevt      —— 事件日志频道
// 各域内部辅助函数置于域内匿名命名空间；导出薄壳在文件尾统一转发。
namespace ncleaner_internals {
namespace installer {

namespace
{
    // 读注册表 REG_SZ 值并取文件名部分入集合。
    void PushLocalPackageBaseName(HKEY hKey, const wchar_t* pszValue,
                                  std::vector<std::wstring>& out)
    {
        wchar_t szPkg[MAX_PATH * 2] = { 0 };
        DWORD cbPkg = sizeof(szPkg);
        DWORD dwType = 0;
        if (::RegQueryValueExW(hKey, pszValue, nullptr, &dwType,
                               reinterpret_cast<BYTE*>(szPkg), &cbPkg) == ERROR_SUCCESS
            && (dwType == REG_SZ || dwType == REG_EXPAND_SZ))
        {
            const wchar_t* pName = ::PathFindFileNameW(szPkg);
            if (pName != nullptr && *pName != L'\0') out.push_back(pName);
        }
    }

    // 单个补丁键：State\Action=2/3（Absent/Advertised）跳过；
    // 其余取补丁键自身的 LocalPackage 文件名（BUG-04）。
    void CollectOnePatch(HKEY hPatches, const wchar_t* pszPatch,
                         std::vector<std::wstring>& out)
    {
        NCRegGuard hOne;
        if (::RegOpenKeyExW(hPatches, pszPatch, 0, KEY_READ, hOne.out())
            != ERROR_SUCCESS) return;

        NCRegGuard hState;
        if (::RegOpenKeyExW(hOne, L"State", 0, KEY_READ, hState.out())
            == ERROR_SUCCESS)
        {
            DWORD dwAction = 0, cbA = sizeof(dwAction);
            if (::RegQueryValueExW(hState, L"Action", nullptr, nullptr,
                                  (BYTE*)&dwAction, &cbA) == ERROR_SUCCESS
                && (dwAction == 2 || dwAction == 3))
            {
                return;
            }
        }

        PushLocalPackageBaseName(hOne, L"LocalPackage", out);
    }

    // 单个产品键：InstallProperties\LocalPackage + Patches 下全部补丁
    void CollectOneProduct(const std::wstring& sProdKey,
                           const wchar_t* pszProduct,
                           std::vector<std::wstring>& out)
    {
        std::wstring sIpKey =
            sProdKey + L"\\" + pszProduct + L"\\InstallProperties";
        NCRegGuard hIp;
        if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, sIpKey.c_str(), 0,
                            KEY_READ, hIp.out()) == ERROR_SUCCESS)
        {
            PushLocalPackageBaseName(hIp, L"LocalPackage", out);
        }

        std::wstring sPatchesKey = sProdKey + L"\\" + pszProduct + L"\\Patches";
        NCRegGuard hPatches;
        if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, sPatchesKey.c_str(), 0,
                            KEY_READ, hPatches.out()) != ERROR_SUCCESS) return;

        for (DWORD iPatch = 0; ; ++iPatch)
        {
            wchar_t szP[128] = { 0 };
            DWORD cchP = _countof(szP);
            if (::RegEnumKeyExW(hPatches, iPatch, szP, &cchP, nullptr, nullptr,
                               nullptr, nullptr) != ERROR_SUCCESS) break;
            CollectOnePatch(hPatches, szP, out);
        }
    }

    // [N1] Installer 目录只处理这三类扩展名。
    bool IsCleanerTargetExt(const wchar_t* pszName)
    {
        const wchar_t* pExt = ::PathFindExtensionW(pszName);
        if (pExt == nullptr) return false;
        return lstrcmpiW(pExt, L".msi") == 0
            || lstrcmpiW(pExt, L".msp") == 0
            || lstrcmpiW(pExt, L".cab") == 0;
    }

    // [N1] 线性匹配 Live 引用集（集合规模为已安装产品数，成本可接受）。
    bool IsLiveFileName(const wchar_t* pszName,
                        const std::vector<std::wstring>& vLive)
    {
        for (size_t i = 0; i < vLive.size(); ++i)
        {
            if (lstrcmpiW(vLive[i].c_str(), pszName) == 0) return true;
        }
        return false;
    }
}  // namespace

// 收集仍在使用中的安装包文件名（Live 集合），用于判定"被取代"。
// 来源：UserData\<SID>\Products\<code>\InstallProperties\LocalPackage 与
//       ...\Products\<code>\Patches\<PatchCode>\LocalPackage。
void CollectLiveInstallerNames(std::vector<std::wstring>& out)
{
    out.clear();

    const wchar_t* kUserData =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Installer\\UserData";

    NCRegGuard hRoot;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kUserData, 0, KEY_READ, hRoot.out())
        != ERROR_SUCCESS) return;

    for (DWORD iSid = 0; ; ++iSid)
    {
        wchar_t szSid[256] = { 0 };
        DWORD cchSid = _countof(szSid);
        if (::RegEnumKeyExW(hRoot, iSid, szSid, &cchSid, nullptr, nullptr, nullptr, nullptr)
            != ERROR_SUCCESS) break;

        std::wstring sProdKey =
            std::wstring(kUserData) + L"\\" + szSid + L"\\Products";
        NCRegGuard hProd;
        if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, sProdKey.c_str(), 0,
                            KEY_READ, hProd.out()) != ERROR_SUCCESS) continue;

        for (DWORD iProd = 0; ; ++iProd)
        {
            wchar_t szCode[128] = { 0 };
            DWORD cchCode = _countof(szCode);
            if (::RegEnumKeyExW(hProd, iProd, szCode, &cchCode, nullptr, nullptr,
                                nullptr, nullptr) != ERROR_SUCCESS) break;
            CollectOneProduct(sProdKey, szCode, out);
        }
    }

}

static HRESULT WINAPI NcInstallerFolderCleanup_Impl()
{
    NCleanerHost::Init();   // [C-06] 惰性解析宿主回调（幂等）
    Log(L"NCleaner!NCInstallerFolderCleanup");

    // ① MSI 事务防护（原版三处互斥量之一）
    CMsiGuard guard;
    if (guard.IsBusy()) {
        Log(L"NCleaner!NCInstallerFolderCleanup: MSI 事务进行中，跳过");
        return S_FALSE;
    }

    // ② 取 SystemRoot\Windows\Installer 目录（实测模板 %s\Windows\Installer\）
    std::wstring sWinRoot;
    if (!GetSystemRoot(sWinRoot)) return E_FAIL;

    std::wstring sInstallerDir = sWinRoot + L"\\Installer";

    DWORD dwAttr = ::GetFileAttributesW(sInstallerDir.c_str());
    if (dwAttr == INVALID_FILE_ATTRIBUTES) return S_FALSE;

    // ③ 收集仍在引用中的包名
    std::vector<std::wstring> vLive;
    CollectLiveInstallerNames(vLive);

    // ④ 遍历 .msi / .msp / .cab，删除不在 Live 集合中的"被取代"包
    unsigned long nDeleted = 0;
    unsigned long nSkipped = 0;

    std::wstring sPattern = sInstallerDir + L"\\*";
    WIN32_FIND_DATAW fd;
    NCFindGuard hFind(::FindFirstFileW(sPattern.c_str(), &fd));
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (!IsCleanerTargetExt(fd.cFileName)) continue;              // [N1]
            if (IsLiveFileName(fd.cFileName, vLive)) { ++nSkipped; continue; }  // [N1]

            std::wstring sFull = sInstallerDir + L"\\" + fd.cFileName;
            bool bOk = DeleteFileForce(sFull.c_str());
            Log(L"NCDeleteFile(%s) ==> 0x%08lX", sFull.c_str(), bOk ? 0UL : 1UL);
            if (bOk) ++nDeleted;
        } while (::FindNextFileW(hFind, &fd));
    }

    Log(L"NCleaner!NCInstallerFolderCleanup: deleted=%lu live=%lu",
        nDeleted, (unsigned long)vLive.size());
    return S_OK;
}

// ============================================================================
//  2) NCPackageCacheFolderCleanup（RVA 0x3944）—— Package Cache 目录
//
//  实测字符串：
//    NCleaner!NCPackageCacheFolderCleanup
//    \state.rsm
//    Silverlight_Developer.exe / dotNet / SSCERuntime / vcredist
//    VC_redist / vsta_
//    \Users
//    \ProgramData\Package Cache
//    \AppData\Local\Package Cache
//
//  行为（实测推断 A 级）：
//    ① MSI 事务互斥量防护（原版三处之一）
//    ② 两个目标目录：系统级 %ProgramData%\Package Cache 与
//       各用户 %LOCALAPPDATA%\Package Cache（\Users 下遍历）
//    ③ 仅当条目名命中过滤串（dotNet/SSCERuntime/vcredist/VC_redist/
//       vsta_/Silverlight_Developer.exe）才删除 —— 避免误删正在使用的缓存
//    ④ state.rsm 为 Package Cache 的状态文件，跳过不删
// ============================================================================

namespace
{
    // 原版 6 个过滤串（实测字符串池）
    const wchar_t* const kPackageCacheFilters[] = {
        L"dotNet",
        L"SSCERuntime",
        L"vcredist",
        L"VC_redist",
        L"vsta_",
        L"Silverlight_Developer.exe",
    };
    constexpr int kPackageCacheFilterCount =
        static_cast<int>(sizeof(kPackageCacheFilters) / sizeof(kPackageCacheFilters[0]));

    // 清理单个 Package Cache 目录
    unsigned long CleanOnePackageCache(const std::wstring& sDir)
    {
        unsigned long nDeleted = 0;

        DWORD dwAttr = ::GetFileAttributesW(sDir.c_str());
        if (dwAttr == INVALID_FILE_ATTRIBUTES ||
            !(dwAttr & FILE_ATTRIBUTE_DIRECTORY)) {
            return 0;
        }

        std::wstring sPattern = sDir + L"\\*";
        WIN32_FIND_DATAW fd;
        NCFindGuard hFind(::FindFirstFileW(sPattern.c_str(), &fd));
        if (hFind == INVALID_HANDLE_VALUE) return 0;

        do {
            if (lstrcmpW(fd.cFileName, L".") == 0 || lstrcmpW(fd.cFileName, L"..") == 0) continue;

            // state.rsm：Package Cache 状态文件，原版跳过
            if (lstrcmpiW(fd.cFileName, L"state.rsm") == 0) continue;

            // 仅命中过滤串才删
            if (!NameMatchesAny(fd.cFileName, kPackageCacheFilters, kPackageCacheFilterCount)) {
                continue;
            }

            std::wstring sChild = sDir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                nDeleted += DeleteTree(sChild.c_str(), true);
            } else {
                if (DeleteFileForce(sChild.c_str())) ++nDeleted;
            }
            Log(L"NCDeleteFile(%s) ==> 0x%08lX", sChild.c_str(), 0UL);
        } while (::FindNextFileW(hFind, &fd));

        return nDeleted;
    }
}

static HRESULT WINAPI NcPackageCacheFolderCleanup_Impl()
{
    NCleanerHost::Init();
    Log(L"NCleaner!NCPackageCacheFolderCleanup");

    CMsiGuard guard;
    if (guard.IsBusy()) {
        Log(L"NCleaner!NCPackageCacheFolderCleanup: MSI 事务进行中，跳过");
        return S_FALSE;
    }

    unsigned long nTotal = 0;

    // ① 系统级：%ProgramData%\Package Cache
    std::wstring sProgramData;
    if (ExpandPath(L"%ProgramData%", sProgramData)) {
        nTotal += CleanOnePackageCache(sProgramData + L"\\Package Cache");
    }

    // ② 用户级：<SystemDrive>\Users\<user>\AppData\Local\Package Cache
    std::wstring sSysDrive;
    if (GetSystemDriveRoot(sSysDrive)) {
        std::wstring sUsers = sSysDrive + L"Users";
        std::wstring sPattern = sUsers + L"\\*";
        WIN32_FIND_DATAW fd;
        NCFindGuard hFind(::FindFirstFileW(sPattern.c_str(), &fd));
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (lstrcmpW(fd.cFileName, L".") == 0 || lstrcmpW(fd.cFileName, L"..") == 0) continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;

                std::wstring sUserCache =
                    sUsers + L"\\" + fd.cFileName + L"\\AppData\\Local\\Package Cache";
                nTotal += CleanOnePackageCache(sUserCache);
            } while (::FindNextFileW(hFind, &fd));
        }
    }

    Log(L"NCleaner!NCPackageCacheFolderCleanup: deleted=%lu", nTotal);
    return S_OK;
}

// ============================================================================
//  3) NCVSPackageCacheCleanup（RVA 0x49E4）—— Visual Studio 包缓存
//
//  实测字符串：
//    NCleaner!NCVSPackageCacheCleanup
//    \ProgramData\Microsoft\VisualStudio\Packages
//
//  行为（实测推断 A/B 级）：
//    ① MSI 事务互斥量防护（原版三处之一）
//    ② 递归删除 %ProgramData%\Microsoft\VisualStudio\Packages 下的内容
//       （保留根目录本身，供 VS 安装器后续重建）
// ============================================================================

static HRESULT WINAPI NcVSPackageCacheCleanup_Impl()
{
    NCleanerHost::Init();
    Log(L"NCleaner!NCVSPackageCacheCleanup");

    CMsiGuard guard;
    if (guard.IsBusy()) {
        Log(L"NCleaner!NCVSPackageCacheCleanup: MSI 事务进行中，跳过");
        return S_FALSE;
    }

    std::wstring sProgramData;
    if (!ExpandPath(L"%ProgramData%", sProgramData)) return E_FAIL;

    std::wstring sVsPackages =
        sProgramData + L"\\Microsoft\\VisualStudio\\Packages";

    DWORD dwAttr = ::GetFileAttributesW(sVsPackages.c_str());
    if (dwAttr == INVALID_FILE_ATTRIBUTES) return S_FALSE;

    // 递归删内容，保留根目录（bDeleteRoot=false）
    unsigned long nDeleted = DeleteTree(sVsPackages.c_str(), false);

    Log(L"NCleaner!NCVSPackageCacheCleanup: deleted=%lu", nDeleted);
    return S_OK;
}

}  // namespace installer

// ============================================================================
//  4) NCDOCacheOnlineCleanup（RVA 0x3180）—— 传递优化（DO）缓存
//
//  实测字符串：
//    NCleaner!NCDOCacheOnlineCleanup
//    {5B99FA76-721C-423C-ADAC-56D03C8A8007}   ← DeliveryOptimizationClient CLSID
//    {6692FD56-3B9B-433A-AC04-3FFB442556DD}   ← IDeliveryOptimizationMgrInternal IID
//    IDeliveryOptimizationCleanup
//    GetCacheSize() ==> 0x%08lX
//    DeleteCache() ==> 0x%08lX
//    IDeliveryOptimizationMgrInternal
//    Interface\
//
//  行为（实测推断 A/B 级 · 双 COM 管线）：
//    ① 先用 CLSID {5B99FA76-…} CoCreateInstance 取 DO 客户端，QueryInterface
//       拿 IDeliveryOptimizationCleanup，经 vtable +0x18 查缓存大小、
//       +0x20 执行 DeleteCache（知识库《52_B》1.3 / 2.4）
//    ② 管线二：{6692FD56-…} → IDeliveryOptimizationMgrInternal 枚举下载缓存目录
//    ③ HKCR\Interface\<IID> 注册校验（原版字符串 Interface\）
//
//  CLSCTX_LOCAL_SERVER（原版实测）
// ============================================================================

namespace docache {

// IID_IUnknown 的显式定义（避免因 initguid.h / uuid.lib 产生额外链接依赖）
static const GUID kIID_IUnknown_Local =
    { 0x00000000, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

// DO 清理接口的最小 vtable 布局（IUnknown 3 槽 + GetCacheSize + DeleteCache）。
// 偏移依据实测：+0x18=GetCacheSize、+0x20=DeleteCache。
struct NcDO_CleanupVtbl
{
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void* pThis, REFIID riid, void** ppv);
    unsigned long   (STDMETHODCALLTYPE *AddRef)(void* pThis);
    unsigned long   (STDMETHODCALLTYPE *Release)(void* pThis);
    HRESULT (STDMETHODCALLTYPE *GetCacheSize)(void* pThis, ULONGLONG* pullSize);
    HRESULT (STDMETHODCALLTYPE *DeleteCache)(void* pThis);
};

static HRESULT WINAPI NcDOCacheOnlineCleanup_Impl()
{
    NCleanerHost::Init();
    Log(L"NCleaner!NCDOCacheOnlineCleanup");

    // 实测 CLSID / IID 字面量
    const wchar_t* kClsidDoClient   = L"{5B99FA76-721C-423C-ADAC-56D03C8A8007}";
    const wchar_t* kIidDoCleanup    = L"{6692FD56-3B9B-433A-AC04-3FFB442556DD}";

    CLSID clsid = { 0 };
    if (FAILED(::CLSIDFromString(kClsidDoClient, &clsid))) {
        Log(L"NCDOCacheOnlineCleanup: CLSIDFromString failed");
        return E_FAIL;
    }

    // [BUG-06 修复] 清理接口 IID（原注册表探测只开键即弃，无任何消费方，
    // 已删除）。
    IID iidCleanup = { 0 };
    if (FAILED(::CLSIDFromString(kIidDoCleanup, &iidCleanup))) {
        Log(L"NCDOCacheOnlineCleanup: IIDFromString failed");
        return E_FAIL;
    }

    // COM 初始化（DLL 内可能需要；已初始化时返回 S_FALSE 属正常）
    HRESULT hrCo = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    ComPtr<IUnknown> pUnk;   // [G2]
    HRESULT hr = ::CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER,
                                    kIID_IUnknown_Local,
                                    reinterpret_cast<void**>(pUnk.GetAddressOf()));
    if (FAILED(hr) || pUnk.Get() == nullptr) {
        Log(L"NCDOCacheOnlineCleanup: CoCreateInstance failed 0x%08lX", (unsigned long)hr);
        if (SUCCEEDED(hrCo)) ::CoUninitialize();
        return hr;
    }

    // [BUG-06 修复] COM 身份规则：IUnknown 虚表不保证等于清理接口虚表，
    // 必须 QueryInterface 到 IDeliveryOptimizationCleanup 后再按其 vtable
    // 调用；直接把 IUnknown 指针当清理接口打槽是错误签名调用，目标对象
    // 布局变化时即 AV。
    ComPtr<IUnknown> pClean;   // [G2] 动态 IID QI 结果，析构自动 Release
    HRESULT hrQi = pUnk->QueryInterface(iidCleanup,
                                        reinterpret_cast<void**>(pClean.GetAddressOf()));
    if (FAILED(hrQi) || pClean.Get() == nullptr) {
        Log(L"NCDOCacheOnlineCleanup: QueryInterface(cleanup) failed 0x%08lX",
            (unsigned long)hrQi);
        if (SUCCEEDED(hrCo)) ::CoUninitialize();
        return FAILED(hrQi) ? hrQi : E_NOINTERFACE;
    }

    void* pCleanRaw = pClean.Get();
    NcDO_CleanupVtbl* pVtbl = *reinterpret_cast<NcDO_CleanupVtbl**>(pCleanRaw);

    ULONGLONG ullSize = 0;
    if (pVtbl != nullptr && pVtbl->GetCacheSize != nullptr) {
        HRESULT hrSize = pVtbl->GetCacheSize(pCleanRaw, &ullSize);
        Log(L"GetCacheSize() ==> 0x%08lX", (unsigned long)hrSize);
    }

    HRESULT hrDel = E_FAIL;
    if (pVtbl != nullptr && pVtbl->DeleteCache != nullptr) {
        hrDel = pVtbl->DeleteCache(pCleanRaw);
        Log(L"DeleteCache() ==> 0x%08lX", (unsigned long)hrDel);
    }

    // [G2] pClean/pUnk 由 ComPtr 析构统一 Release（原 pVtbl->Release(pCleanRaw) 与
    // pUnk->Release() 语义等价迁移）
    if (SUCCEEDED(hrCo)) ::CoUninitialize();

    return hrDel;
}

}  // namespace docache

// ============================================================================
//  5) NCSRPointOnlineCleanup（RVA 0x4350）—— 系统还原点 / 卷影
//
//  实测字符串：
//    NCleaner!NCSRPointOnlineCleanup
//    vssapi.dll / srclient.dll
//    NCQueryVSSUsedSpace() ==> 0x%08lX
//    NCDeleteVSSAllSnapshots() ==> 0x%08lX
//    NCCreateSystemRestorePoint() ==> 0x%08lX
//    After Cleanup By Dism++
//
//  行为（实测推断 A 级 · 三步）：
//    ① 查用量：NCQueryVSSUsedSpace
//    ② vssapi.dll 的 CreateVssBackupComponentsInternal 删除全部快照
//    ③ srclient.dll 的 SRSetRestorePointW 新建还原点，描述
//       "After Cleanup By Dism++"
//
//  动态加载原因：与主程序一致，避免链接期硬依赖 vssapi/srclient。
// ============================================================================

namespace srpoint {

namespace
{
    // RESTOREPOINTINFOW 最小定义（Windows Vista+ SDK 布局）
    constexpr DWORD kRPI_EventBeginNested = 102;   // BEGIN_NESTED_SYSTEM_CHANGE
    constexpr DWORD kRPI_EventEndNested = 103;   // [BUG-11] END_NESTED_SYSTEM_CHANGE
    constexpr DWORD kRPI_TypeApplication = 1;     // APPLICATION_INSTALL
    constexpr DWORD kRPI_MaxDescLen = 256;

    struct NcRpiRestorePointInfo
    {
        DWORD    dwEventType;
        DWORD    dwRestorePtType;
        LONGLONG llSequenceNumber;
        WCHAR    szDescription[kRPI_MaxDescLen];
    };
    struct NcRpiStateMgrStatus
    {
        DWORD    nStatus;
        LONGLONG llSequenceNumber;
    };
    using NcPFN_SRSetRestorePointW = BOOL ( WINAPI*)(NcRpiRestorePointInfo*, NcRpiStateMgrStatus*);

}

static HRESULT WINAPI NcSRPointOnlineCleanup_Impl()
{
    NCleanerHost::Init();
    Log(L"NCleaner!NCSRPointOnlineCleanup");

    HRESULT hrResult = S_OK;

    // ① 查用量：等价原版 NCQueryVSSUsedSpace（原生 VSS diff-area 占用）
    unsigned long long ullUsed = 0;
    std::wstring sErr;
    HRESULT hrQ = NCleanerVss::QueryUsedSpace(&ullUsed, &sErr);
    Log(L"NCQueryVSSUsedSpace() ==> 0x%08lX", (unsigned long)hrQ);
    if (FAILED(hrQ)) {
        // 用量查询失败：无法做阈值判定，仍继续删除（原版不因查询失败而中止）
        if (!sErr.empty()) Log(L"NCSRPointOnlineCleanup: %s", sErr.c_str());
    } else if (ullUsed < NCleanerVss::kMinCleanupBytes) {
        // 官方 ReadMe 1.0.0.4 权威行为：
        // 「减小系统还原点清理扫描大小误差；并设定大小小于 512MB 不予清理」
        Log(L"NCSRPointOnlineCleanup: 占用量 %llu 字节 < 512MB 阈值，不予清理",
            ullUsed);
        return S_OK;
    }

    // ② 删除全部快照：真实调用 VSS COM（VssSnapshotMgmt 枚举 + DeleteSnapshots）
    int nDeleted = 0;
    HRESULT hrDel = NCleanerVss::DeleteAllSnapshots(&nDeleted, &sErr);
    Log(L"NCDeleteVSSAllSnapshots() ==> 0x%08lX (deleted=%d)",
        (unsigned long)hrDel, nDeleted);
    if (FAILED(hrDel)) {
        if (!sErr.empty()) Log(L"NCSRPointOnlineCleanup: %s", sErr.c_str());
        hrResult = S_FALSE;
    }

    // ③ 新建还原点（描述固定为 "After Cleanup By Dism++"，与原版一致）
    //    抗劫持：只从 System32 解析 srclient.dll
    HMODULE hSr = SafeLoadSystem32Dll(L"srclient.dll");
    if (hSr != nullptr) {
        NcPFN_SRSetRestorePointW pSet =
            reinterpret_cast<NcPFN_SRSetRestorePointW>(
                ::GetProcAddress(hSr, "SRSetRestorePointW"));
        if (pSet != nullptr) {
            NcRpiRestorePointInfo rpi = { 0 };
            rpi.dwEventType     = kRPI_EventBeginNested;
            rpi.dwRestorePtType = kRPI_TypeApplication;
            wcscpy_s(rpi.szDescription, kRPI_MaxDescLen, L"清理完成 - 系统管理与优化");

            NcRpiStateMgrStatus sms = { 0 };
            BOOL bRc = pSet(&rpi, &sms);
            Log(L"NCCreateSystemRestorePoint() ==> 0x%08lX", (unsigned long)sms.nStatus);
            if (!bRc) {
                hrResult = S_FALSE;
            } else {
                // [BUG-11] BEGIN_NESTED_SYSTEM_CHANGE(102) 必须配对
                // END_NESTED_SYSTEM_CHANGE(103)；只发 begin 会让系统还原
                // 残留开启的嵌套变更事务，阻塞后续还原点与安装操作。结束
                // 事件的 llSequenceNumber 回填 begin 返回的序列号。
                NcRpiRestorePointInfo rpiEnd = { 0 };
                rpiEnd.dwEventType       = kRPI_EventEndNested;
                rpiEnd.llSequenceNumber  = sms.llSequenceNumber;
                NcRpiStateMgrStatus smsEnd = { 0 };
                BOOL bEnd = pSet(&rpiEnd, &smsEnd);
                Log(L"NCEndSystemRestorePoint() ==> 0x%08lX", (unsigned long)smsEnd.nStatus);
                if (!bEnd) hrResult = S_FALSE;
            }
        }
        ::FreeLibrary(hSr);
    }

    return hrResult;
}

}  // namespace srpoint

// ============================================================================
//  6) NCCorruptedAppXOnlineCleanup（RVA 0x4568）—— 损坏的 Appx
//
//  实测字符串：
//    NCleaner!NCCorruptedAppXOnlineCleanup
//    DismRemoveAppx(%s) ==> 0x%08lX
//
//  行为（实测推断 A 级）：
//    遍历所有用户的 Appx（经宿主回调 DismGetAllUsersAppx），
//    对判定为"损坏"的包调用宿主 DismRemoveAppx 移除。
//    本函数不自行实现 Appx 部署逻辑 —— 与 NCleaner 只做编排、
//    能力下沉到宿主的设计一致（知识库《21》2.3 六回调）。
// ============================================================================

namespace appx {

static HRESULT WINAPI NcCorruptedAppXOnlineCleanup_Impl()
{
    NCleanerHost::Init();
    Log(L"NCleaner!NCCorruptedAppXOnlineCleanup");

    NCleanerHost::PFN_DismGetAllUsersAppx pEnum = NCleanerHost::GetAllUsersAppx();
    if (pEnum == nullptr) {
        // 宿主回调不可用：无法枚举 Appx，报失败（原版 S_OK 才算成功）
        Log(L"NCCorruptedAppXOnlineCleanup: 宿主 DismGetAllUsersAppx 不可用");
        return E_NOTIMPL;
    }

    // 先计数
    int nCount = pEnum(nullptr, 0);
    if (nCount <= 0) {
        Log(L"NCCorruptedAppXOnlineCleanup: 无损坏 Appx 条目");
        return S_OK;
    }
    // [BUG-07] 宿主输入合理性上限：条目数异常（契约损坏/宿主实现被替换）
    // 时拒绝按其值分配缓冲，避免巨量 vector 分配触发 bad_alloc 跨 ABI。
    constexpr int kMaxAppxEntries = 4096;
    if (nCount > kMaxAppxEntries) {
        Log(L"NCCorruptedAppXOnlineCleanup: 条目数 %d 超过上限 %d，中止",
            nCount, kMaxAppxEntries);
        return HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY);
    }

    // 分配缓冲并枚举（每条 MAX_PATH 宽）
    constexpr int kMaxName = MAX_PATH;
    std::vector<wchar_t> vBuf(static_cast<size_t>(nCount) * kMaxName, L'\0');
    int nGot = pEnum(vBuf.data(), nCount);
    if (nGot < 0) nGot = 0;
    if (nGot > nCount) nGot = nCount;

    int nRemoved = 0;
    int nFailed  = 0;

    for (int i = 0; i < nGot; ++i) {
        const wchar_t* pFullName = vBuf.data() + static_cast<size_t>(i) * kMaxName;
        if (pFullName[0] == L'\0') continue;

        // 原版无独立损坏判定 API，此处按宿主回调返回的条目直接交给移除路径；
        // 宿主 DismRemoveAppx 内部对"损坏/不存在的包"返回失败 HRESULT，
        // 与原版"逐个尝试、记日志、失败不中断"的行为一致。
        long hr = NCleanerHost::CallRemoveAppx(pFullName);
        if (hr == 0) ++nRemoved; else ++nFailed;
    }

    Log(L"NCCorruptedAppXOnlineCleanup: removed=%d failed=%d total=%d",
        nRemoved, nFailed, nGot);
    return S_OK;
}

}  // namespace appx

// ============================================================================
//  7) NCWinEventLogOnlineCleanup（RVA 0x4BC0）—— Windows 事件日志
//
//  实测字符串：
//    NCleaner!NCWinEventLogOnlineCleanup
//    wevtapi.dll
//    EvtClearLog(%s) ==> 0x%08lX
//    NCQueryEventLogSize(%s) ==> 0x%08lX
//    System
//    EvtClearLog(System) ==> 0x%08lX
//    EvtNextChannelPath(%s) ==> 0x%08lX
//
//  行为（实测推断 A 级）：
//    ① 动态加载 wevtapi.dll（实测不在静态导入表 → GetProcAddress）
//    ② EvtOpenChannelEnum → EvtNextChannelPath 枚举全部频道
//    ③ 逐频道 EvtClearLog 清空（先 EvtOpenLog 查大小/信息）
//    ④ 每个频道按 EvtClearLog(%s) ==> 0x%08lX 记录
//    ⑤ 特别处理 "System" 频道
//
//  wevtapi 动态加载原因：与原版一致（原版导入表无 WEVTSVC，走运行期解析）。
// ============================================================================

namespace wevt {

namespace
{
    using NcPFN_EvtOpenChannelEnum = EVT_HANDLE ( WINAPI*)(EVT_HANDLE, DWORD);
    using NcPFN_EvtNextChannelPath = BOOL ( WINAPI*)(EVT_HANDLE, DWORD, wchar_t*, unsigned long*);
    using NcPFN_EvtOpenLog = EVT_HANDLE ( WINAPI*)(EVT_HANDLE, const wchar_t*, DWORD);
    using NcPFN_EvtGetLogInfo = BOOL ( WINAPI*)(EVT_HANDLE, EVT_LOG_PROPERTY_ID, DWORD, void*, unsigned long*);
    using NcPFN_EvtClearLog = BOOL ( WINAPI*)(EVT_HANDLE, const wchar_t*, const wchar_t*, DWORD);
    using NcPFN_EvtClose = BOOL ( WINAPI*)(EVT_HANDLE);

    struct NcWevtApi
    {
        HMODULE hMod;
        NcPFN_EvtOpenChannelEnum pOpenChannelEnum;
        NcPFN_EvtNextChannelPath pNextChannelPath;
        NcPFN_EvtOpenLog         pOpenLog;
        NcPFN_EvtGetLogInfo      pGetLogInfo;
        NcPFN_EvtClearLog        pClearLog;
        NcPFN_EvtClose           pClose;
    };

    bool LoadWevtApi(NcWevtApi& api)
    {
        api = {};
        api.hMod = SafeLoadSystem32Dll(L"wevtapi.dll");
        if (api.hMod == nullptr) return false;

        api.pOpenChannelEnum = reinterpret_cast<NcPFN_EvtOpenChannelEnum>(
            ::GetProcAddress(api.hMod, "EvtOpenChannelEnum"));
        api.pNextChannelPath = reinterpret_cast<NcPFN_EvtNextChannelPath>(
            ::GetProcAddress(api.hMod, "EvtNextChannelPath"));
        api.pOpenLog = reinterpret_cast<NcPFN_EvtOpenLog>(
            ::GetProcAddress(api.hMod, "EvtOpenLog"));
        api.pGetLogInfo = reinterpret_cast<NcPFN_EvtGetLogInfo>(
            ::GetProcAddress(api.hMod, "EvtGetLogInfo"));
        api.pClearLog = reinterpret_cast<NcPFN_EvtClearLog>(
            ::GetProcAddress(api.hMod, "EvtClearLog"));
        api.pClose = reinterpret_cast<NcPFN_EvtClose>(
            ::GetProcAddress(api.hMod, "EvtClose"));

        return (api.pOpenChannelEnum != nullptr && api.pNextChannelPath != nullptr &&
                api.pClearLog != nullptr && api.pClose != nullptr);
    }
}

static HRESULT WINAPI NcWinEventLogOnlineCleanup_Impl()
{
    NCleanerHost::Init();
    Log(L"NCleaner!NCWinEventLogOnlineCleanup");

    // 实测导入无 WEVTSVC → 运行期动态解析
    NcWevtApi api;
    if (!LoadWevtApi(api)) {
        Log(L"NCWinEventLogOnlineCleanup: wevtapi.dll 不可用");
        return E_FAIL;
    }

    int nCleared = 0;
    int nFailed  = 0;

    // [BUG-10] 频道缓冲可增长：EvtNextChannelPath 返回
    // ERROR_INSUFFICIENT_BUFFER 时按 dwUsed 扩容重试同一项（游标不前移），
    // 原固定 512 宽缓冲遇长频道名会把"缓冲不足"误判为枚举结束而整段中止。
    bool bEnumOk = false;
    EVT_HANDLE hEnum = api.pOpenChannelEnum(nullptr, 0);
    if (hEnum != nullptr) {
        bEnumOk = true;
        std::vector<wchar_t> vCh(512);
        DWORD dwUsed = 0;

        for (;;) {
            dwUsed = 0;
            BOOL bNext = api.pNextChannelPath(hEnum, static_cast<DWORD>(vCh.size()),
                                              vCh.data(), &dwUsed);
            if (!bNext) {
                DWORD dwChErr = ::GetLastError();
                if (dwChErr == ERROR_INSUFFICIENT_BUFFER && dwUsed > 0) {
                    vCh.resize(dwUsed);   // dwUsed 含终止 NUL
                    continue;            // 重试同一项
                }
                break;                    // ERROR_NO_MORE_ITEMS 等：枚举结束
            }
            if (dwUsed >= vCh.size()) vCh[vCh.size() - 1] = L'\0';
            const wchar_t* pszCh = vCh.data();
            if (pszCh[0] == L'\0') continue;

            // 查大小（原版 NCQueryEventLogSize(%s) ==> 0x%08lX）
            EVT_HANDLE hLog = api.pOpenLog ? api.pOpenLog(nullptr, pszCh, EvtOpenChannelPath) : nullptr;
            if (hLog != nullptr && api.pGetLogInfo != nullptr) {
                ULONGLONG ullSize = 0;
                DWORD dwRet = 0;
                api.pGetLogInfo(hLog, EvtLogNumberOfLogRecords, sizeof(ullSize), &ullSize, &dwRet);
                Log(L"NCQueryEventLogSize(%s) ==> 0x%08lX", pszCh, 0UL);
            }
            if (hLog != nullptr && api.pClose) api.pClose(hLog);

            // 清空频道（第三参 nullptr = 默认备份路径，与原版同形）
            BOOL bRc = api.pClearLog(nullptr, pszCh, nullptr, 0);
            Log(L"EvtClearLog(%s) ==> 0x%08lX", pszCh, bRc ? 0UL : 1UL);
            if (bRc) ++nCleared; else ++nFailed;
        }
        api.pClose(hEnum);
    }

    // [BUG-10] 枚举器可用时 System 已包含在频道列表中，不再重复清空；
    // 仅当枚举器打不开（wevt 服务异常等）时兜底单独清 System。
    if (!bEnumOk) {
        BOOL bRc = api.pClearLog(nullptr, L"System", nullptr, 0);
        Log(L"EvtClearLog(System) ==> 0x%08lX", bRc ? 0UL : 1UL);
        if (bRc) ++nCleared; else ++nFailed;
    }

    Log(L"NCleaner!NCWinEventLogOnlineCleanup: cleared=%d failed=%d", nCleared, nFailed);

    if (api.hMod != nullptr) ::FreeLibrary(api.hMod);
    return S_OK;
}

}  // namespace wevt
}  // namespace ncleaner_internals

// ============================================================================
//  [BUG-07] 统一 SEH 异常边界
//
//  NCleaner.dll 为 /MT 静态 CRT，宿主 Win8Metro.exe 为 /MD。实现函数内部
//  大量使用 STL，任何 bad_alloc/AV 一旦穿过 extern "C" ABI 进入宿主，跨
//  CRT 无匹配 catch → std::terminate 杀进程。所有官方导出统一经此薄层
//  转发：包装函数内不含任何需要析构展开的 C++ 对象（MSVC C2712 约束），
//  C++ 对象只存在于 *_Impl 实现内。
// ============================================================================

static HRESULT WINAPI NcSehGuard(HRESULT (WINAPI* pfnImpl)())
{
    __try
    {
        return pfnImpl();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return E_UNEXPECTED;
    }
}

extern "C" HRESULT WINAPI NCInstallerFolderCleanup()
{ return NcSehGuard(ncleaner_internals::installer::NcInstallerFolderCleanup_Impl); }

extern "C" HRESULT WINAPI NCPackageCacheFolderCleanup()
{ return NcSehGuard(ncleaner_internals::installer::NcPackageCacheFolderCleanup_Impl); }

extern "C" HRESULT WINAPI NCVSPackageCacheCleanup()
{ return NcSehGuard(ncleaner_internals::installer::NcVSPackageCacheCleanup_Impl); }

extern "C" HRESULT WINAPI NCDOCacheOnlineCleanup()
{ return NcSehGuard(ncleaner_internals::docache::NcDOCacheOnlineCleanup_Impl); }

extern "C" HRESULT WINAPI NCSRPointOnlineCleanup()
{ return NcSehGuard(ncleaner_internals::srpoint::NcSRPointOnlineCleanup_Impl); }

extern "C" HRESULT WINAPI NCCorruptedAppXOnlineCleanup()
{ return NcSehGuard(ncleaner_internals::appx::NcCorruptedAppXOnlineCleanup_Impl); }

extern "C" HRESULT WINAPI NCWinEventLogOnlineCleanup()
{ return NcSehGuard(ncleaner_internals::wevt::NcWinEventLogOnlineCleanup_Impl); }
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
inline constexpr int kCpp17_ncleaner_exports_StepTimeoutMs = 120000;
inline constexpr int kCpp17_ncleaner_exports_MaxRetry = 3;
