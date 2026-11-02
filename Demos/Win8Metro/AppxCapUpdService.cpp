#include <cstdio>   // [C15] std::snprintf（F8_C_format_str 清零）
#include "FormatW.h"
#include <string>   // [C14] std::stoi/std::stol（F21_C_parse 清零）
#include <vector>
#include <memory>   // [C++17 阶段3] make_unique
#include <algorithm>
#include <map>      // [性能 A4] DISPID 缓存
#include <mutex>    // [性能 A4] DISPID 缓存跨线程保护
#include <ctime>    // [R16][O-4] time() 缓存 TTL
#include <cstdlib>  // [R16][O-4] atoi/_strtoui64
#include <objbase.h>
#include <oleauto.h>
#include "AppxCapUpdService.h"
#include "SafeEnv.h"
#include "SeedCache.h"     // [P4] 种子缓存指纹与目录     // [B6] winenv::SafeGetEnv：替代弃用 _wgetenv（C4996 清零）
#include "Registry.h"
#include "ProcessRunner.h"
#include "WmMessages.h"
#include "FrameWnd_Util.h"
#include "UIlib.h"      // CMarkupNode（DataEngine.h 依赖；本文件不用 PCH）
using namespace DuiLib; // DataEngine.h 的裸类型名依赖（等价 stdafx.h 末尾 using）
#include "DataEngine.h" // [P0] WinPE 离线会话：Capability 枚举/管理定向目标
#include "OfflineSession.h" // [R10-02]
#include "DismApiBridge.h" // [P0-A3] Capability 枚举/增删直连 dismapi.dll
#include "DismCore.h"      // [R-P23 · R3] Rust dismcore：UTF-8↔UTF-16 纯计算
#include "SafeThread.h"  // 工作线程顶层异常边界
namespace {
// [C++17 升级阶段2] 局部句柄 RAII 守卫：隐式转换为原句柄类型，调用点零改动；
// 只负责早退/异常路径自动释放，不改变调用顺序与业务分支。
// 线程句柄 h（SafeCreateThread 返回后单点关闭）与 SC_HANDLE 按方案显式管理。
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


// [R16-FIX] 递归加深度上限 32：防止极端深目录树耗尽线程栈。
// 同时改为显式栈迭代，避免递归调用开销。
ULONGLONG FsDirSize(const CDuiString& strPath)
{
    constexpr int kMaxDepth = 32;

    ULONGLONG ullTotal = 0;
    struct StackFrame { CDuiString path; int depth; };
    std::vector<StackFrame> stack;
    stack.push_back({ strPath, 0 });

    while (!stack.empty())
    {
        StackFrame frame = stack.back();
        stack.pop_back();

        CDuiString sFind = frame.path;
        sFind += L"\\*";
        WIN32_FIND_DATA fd = { 0 };
        FindGuard hFind = ::FindFirstFile(sFind.GetData(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) continue;
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                if (lstrcmpW(fd.cFileName, L".") == 0
                    || lstrcmpW(fd.cFileName, L"..") == 0) continue;
                if (frame.depth + 1 > kMaxDepth) continue;   // 超深度跳过
                CDuiString sSub = frame.path;
                sSub += L"\\";
                sSub += fd.cFileName;
                stack.push_back({ sSub, frame.depth + 1 });
            }
            else
            {
                LARGE_INTEGER li;
                li.HighPart = fd.nFileSizeHigh;
                li.LowPart  = fd.nFileSizeLow;
                ullTotal += static_cast<ULONGLONG>(li.QuadPart);
            }
        }
        while (::FindNextFile(hFind, &fd));
    }
    return ullTotal;
}
// [P0-A3] dism 文本解析链已被 DismApiBridge 结构化枚举取代；下列 helper 暂无
// 调用点，以 [[maybe_unused]] 保留（/W3 不报 C4505，高警告级别亦干净）。
[[maybe_unused]] std::vector<wchar_t> DecodeBytes(const std::vector<char>& v)
{
    std::vector<wchar_t> out;
    size_t n = v.size();
    if (n < 2) return out;
    const unsigned char* pb = (const unsigned char*)v.data();
    bool b16 = false;
    size_t off = 0;
    if (pb[0] == 0xFF && pb[1] == 0xFE) { b16 = true; off = 2; }
    else
    {
        int z = 0, t = 0;
        for (size_t i = 1; i < n && i < 200; i += 2)
        {
            ++t;
            if (pb[i] == 0) ++z;
        }
        if (t > 0 && z * 100 / t > 80) b16 = true;
    }
    if (b16)
    {
        size_t cnt = (n - off) / 2;
        out.resize(cnt);
        ::memcpy(out.data(), pb + off, cnt * 2);
        return out;
    }
    UINT cp = CP_UTF8;
    DWORD dwFlags = MB_ERR_INVALID_CHARS;
    int wl = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                   v.data(), static_cast<int>(n), nullptr, 0);
    if (wl <= 0)
    {
        cp = CP_ACP;
        dwFlags = 0;
        wl = ::MultiByteToWideChar(CP_ACP, 0, v.data(), static_cast<int>(n), nullptr, 0);
    }
    if (wl <= 0) return out;
    out.resize(wl);
    ::MultiByteToWideChar(cp, dwFlags, v.data(), static_cast<int>(n), out.data(), wl);
    return out;
}
[[maybe_unused]] std::vector<CDuiString> SplitLines(const std::vector<wchar_t>& s)
{
    std::vector<CDuiString> v;
    const wchar_t* p = s.data();
    size_t n = s.size();
    size_t start = 0;
    for (size_t i = 0; i <= n; ++i)
    {
        if (i == n || p[i] == L'\n' || p[i] == L'\r')
        {
            if (i > start)
            {
                CDuiString str;
                str.Assign(p + start, static_cast<int>(i - start));
                v.push_back(str);
            }
            start = i + 1;
            if (i < n && p[i] == L'\r' && start < n && p[start] == L'\n')
            { ++start; ++i; }
        }
    }
    return v;
}
[[maybe_unused]] void ParseCapabilities(const std::vector<CDuiString>& lines,
                                 std::vector<CapEntry>& out)
{
    CapEntry cur;
    bool bInDeps = false;       // [R45] inside a "Dependencies :" block
    bool bDepsAreReq = false;   // [R45] current dep block is Required (else Optional)
    for (size_t i = 0; i < lines.size(); ++i)
    {
        const CDuiString& s = lines[i];
        int pColon = s.Find(L':');
        if (pColon <= 0) continue;
        CDuiString sLabel = TrimStr(s.Mid(0, pColon));
        CDuiString sValue = TrimStr(s.Mid(pColon + 1));
        if (lstrcmpiW(sLabel, L"Capability Identity") == 0)
        {
            cur.sIdentity = sValue;
        }
        else if (lstrcmpiW(sLabel, L"Description") == 0)
        {
            // [R30] capture Description (DISM /Get-Capabilities /English)
            cur.sDescription = sValue;
        }
        else if (lstrcmpiW(sLabel, L"Size") == 0)
        {
            // [R38] capture Size (DISM /Get-Capabilities Size, e.g. "879.82 KB")
            cur.sSize = sValue;
        }
        else if (lstrcmpiW(sLabel, L"State") == 0)
        {
            cur.sState = sValue;
        }
        else if (lstrcmpiW(sLabel, L"Required Dependencies") == 0
              || lstrcmpiW(sLabel, L"Optional Dependencies") == 0
              || (lstrcmpiW(sLabel, L"Dependencies") == 0 && cur.sIdentity != L""))
        {
            // [R45] DISM /Get-Capabilities English output exposes dependency
            // blocks after "State :". Track which block we're inside so that
            // subsequent unstructured lines can be classified as required or
            // optional dependency entries.
            cur.nRequiredDeps = 0;
            cur.nOptionalDeps = 0;
            cur.sFirstRequiredDep.Empty();
            cur.sFirstOptionalDep.Empty();
            CDuiString sSubKind = sValue; // e.g. "Required" / "Optional" / ""
            // Also accept label "Dependencies : Optional" / "Dependencies : Required"
            int pDep = sSubKind.Find(L"Required");
            int pOpt = sSubKind.Find(L"Optional");
            bool bIsReq;
            if (pDep >= 0 && (pOpt < 0 || pDep < pOpt)) bIsReq = true;
            else if (pOpt >= 0) bIsReq = false;
            else if (lstrcmpiW(sLabel, L"Required Dependencies") == 0) bIsReq = true;
            else if (lstrcmpiW(sLabel, L"Optional Dependencies") == 0) bIsReq = false;
            else bIsReq = true; // bare "Dependencies :" defaults to required
            // [R45] scan the next line to decide: does another dep block or
            // the next capability identity follow? If the next line is
            // "Capabilities : " or "Capability Identity : ", treat the current
            // dep section as empty and reset state.
            if (i + 1 < lines.size())
            {
                const CDuiString& sNext = lines[i + 1];
                if (sNext.Find(L"Capabilities") >= 0
                    || sNext.Find(L"Capability Identity") >= 0
                    || sNext.Find(L"Feature of Record") >= 0)
                {
                    // No dependencies here 鈥?leave counts at 0 and reset.
                    bInDeps = false;
                    bDepsAreReq = false;
                }
                else
                {
                    bInDeps = true;
                    bDepsAreReq = bIsReq;
                }
            }
        }
        else if (bInDeps)
        {
            // [R45] Inside a dependency section. Each dependency line is an
            // indented identity like "      X~~~~0.0.1.0". Count it, and keep
            // the first name of each category for the UI badge.
            const wchar_t* pRaw = s.GetData();
            // Skip lines with ':' 鈥?those belong to a different section.
            int pColon2 = s.Find(L':');
            if (pColon2 > 0 && s.Mid(0, pColon2).TrimLeft().TrimRight().Find(L'~') < 0)
            {
                // Structured line like "Feature of Record : X" or "Capabilities : "
                bInDeps = false;
                bDepsAreReq = false;
            }
            else if (s.Find(L'~') >= 0)
            {
                if (bDepsAreReq)
                {
                    ++cur.nRequiredDeps;
                    if (cur.sFirstRequiredDep.IsEmpty()) cur.sFirstRequiredDep = TrimStr(s);
                }
                else
                {
                    ++cur.nOptionalDeps;
                    if (cur.sFirstOptionalDep.IsEmpty()) cur.sFirstOptionalDep = TrimStr(s);
                }
            }
            else
            {
                // Not an identity line 鈥?end dep block.
                bInDeps = false;
                bDepsAreReq = false;
            }
        }
        if (bInDeps) continue;
        // A new capability starts here 鈥?flush the current one.
        if (cur.sIdentity != L"" && cur.sState != L"")
        {
            out.push_back(cur);
            cur = CapEntry();
        }
    }
}
// [性能 A4] DISPID 缓存：以 (COM 对象 vtable 指针, 成员名) 为键。同一接口类型
// 的所有实例共享同一 vtable，GetIDsOfNames 的字符串查找每类型只执行一次，
// 后续调用直接命中缓存。vtable 指针在进程生命周期内稳定（WUA 服务不卸载
// 自身类型库），跨枚举/卸载/安装/隐藏线程共享，用 mutex 保护。
// 注：KBArticleIDs 仍保留枚举期拉取（sKB 有单击日志消费点，懒取会改变日志
// 内容，属行为变更，故此处仅做 DISPID 缓存，见方案报告偏差说明）。
HRESULT AutoWrap(DWORD dwFlags, VARIANT* pvResult, IDispatch* pDisp,
                        LPOLESTR ptName, int cArgs, VARIANT* pArgs)
{
    if (!pDisp) return E_FAIL;
    HRESULT hr = E_FAIL;
    DISPID dispid = 0;
    void* pVTable = *reinterpret_cast<void**>(pDisp);
    std::wstring sName(ptName ? ptName : L"");
    using TKey = std::pair<void*, std::wstring>;
    static std::mutex s_mtx;
    static std::map<TKey, DISPID> s_mapDispIds;
    bool bCached = false;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        std::map<TKey, DISPID>::iterator it = s_mapDispIds.find(TKey(pVTable, sName));
        if (it != s_mapDispIds.end()) { dispid = it->second; bCached = true; }
    }
    if (!bCached)
    {
        OLECHAR* pNames[1] = { ptName };
        hr = pDisp->GetIDsOfNames(IID_NULL, pNames, 1,
                                  LOCALE_USER_DEFAULT, &dispid);
        if (FAILED(hr)) return hr;
        std::lock_guard<std::mutex> lk(s_mtx);
        s_mapDispIds[TKey(pVTable, sName)] = dispid;
    }
    DISPPARAMS dp = { nullptr, nullptr, 0, 0 };
    VARIANT vResult;
    ::VariantInit(&vResult);
    std::unique_ptr<VARIANT[]> rgvarg;   // [C++17 阶段3] 纯局部配对 → make_unique，函数结束自动释放
    if (cArgs > 0)
    {
        dp.cArgs = cArgs;
        rgvarg = std::make_unique<VARIANT[]>(cArgs);
        dp.rgvarg = rgvarg.get();
        for (int i = 0; i < cArgs; ++i)
            dp.rgvarg[i] = pArgs[cArgs - 1 - i];
    }
    hr = pDisp->Invoke(dispid, IID_NULL, static_cast<WORD>(LOCALE_USER_DEFAULT),
                       static_cast<WORD>(dwFlags), &dp, &vResult, nullptr, nullptr);
    if (SUCCEEDED(hr) && pvResult)
        ::VariantCopy(pvResult, &vResult);
    ::VariantClear(&vResult);
    return hr;
}
} // namespace

// [REFACTOR] 前置声明：ParseAppxFullName 定义见文件末尾
static void ParseAppxFullName(const wchar_t* szKey, AppxPkg& pkg);
// ============================================================
// [REFACTOR 记录] StartAppxEnumThread 的可读性说明
//
// 独立线程运行，异常必须捕获，否则 WM_APPX_DATA 丢失、页面永久停在加载态。
// 本函数负责枚举已安装 Appx 包，流程为：
//   1) 打开 HKCU\Software\Classes\Local Settings\Software\Microsoft\
//      Windows\CurrentVersion\AppModel\Repository\Packages
//   2) 逐个读取子键（包全名 Family_Version_Arch_PublisherID）
//   3) 由全名解析出版本号与发布者（已提取为 ParseAppxFullName）
//   4) 读 PackageRootFolder 判断是否系统内置
//   5) 从 State 子键或 AppxManifest 取启用状态
//   6) 汇总到 AppxDataMsg 并经 WM_APPX_DATA 回 UI 线程
//
// 为什么拆出 ParseAppxFullName：
//   原实现在主循环内联了一段 26 行的字符串拆分逻辑（含 for + if/else +
//   多分支），使该函数最大嵌套达到 7 层。提取后主循环嵌套降至 4 层，
//   且拆分逻辑本身可独立测试。搬迁过程零逻辑改动，仅调整缩进。
//
// 注意：本函数的剩余嵌套来自 Windows 注册表 API 的固有层级
//   注册表键 → 子键遍历 → 属性读取 → 错误分支，进一步拆分会把
//   一个连贯的枚举流程切碎，可读性反而下降，故保留现状。
// ============================================================
void AppxCapUpdService::StartAppxEnumThread(HWND hNotify)
{
    struct Ctx { HWND hNotify; };
    Ctx* ctx = std::make_unique<Ctx>(Ctx{ hNotify }).release();   // [C++17 R3] ownership transferred to worker thread
    HANDLE h = SafeCreateThread(
        [](void* pv) -> DWORD {
            std::unique_ptr<Ctx> guard(static_cast<Ctx*>(pv));   // [C++17 R3] thread owns ctx, auto-freed at end
            HWND s = guard->hNotify;
            constexpr int kUserNameWchars = 256;   // 256 缓冲尺寸（语义命名，数值不变）
            wchar_t szUser[kUserNameWchars] = { 0 };
            DWORD nUserLen = kUserNameWchars;
            ::GetUserName(szUser, &nUserLen);
            auto msg = std::make_unique<AppxDataMsg>();   // [C++17 R3] ownership transferred to UI on successful Post; auto-reclaimed on failure
            AppxDataMsg* pData = msg.get();
            const wchar_t* pszRepo =
                L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Packages";
            RegKeyGuard hRepo;
            LONG lRepo = ::RegOpenKeyEx(HKEY_CURRENT_USER, pszRepo, 0,
                    KEY_READ, hRepo.out());
            pData->nRepoErr = static_cast<int>(lRepo);
            if (lRepo == ERROR_SUCCESS)
            {
                DWORD dwIdx = 0;
                for (;;)
                {
                    constexpr int kRegKeyNameWchars = 512;   // 512 缓冲尺寸（语义命名，数值不变）
                    wchar_t szKey[kRegKeyNameWchars] = { 0 };
                    DWORD nLen = kRegKeyNameWchars;
                    LONG lE = ::RegEnumKeyEx(hRepo, dwIdx, szKey, &nLen,
                                             nullptr, nullptr, nullptr, nullptr);
                    if (lE != ERROR_SUCCESS)
                    {
                        if (dwIdx == 0) pData->nEnumErr = static_cast<int>(lE);
                        break;
                    }
                    ++dwIdx;
                    AppxPkg pkg;
                    pkg.sFull = szKey;
                    pkg.sFamily = AppxFamily(pkg.sFull);
                    pkg.ullSize = 0;
                    // [R126 重建] R32: 从全名解析出 Version / Publisher（以下划线分隔）；
                    // [R126 重建] 格式：Family_Version_Arch_PublisherID
                    ParseAppxFullName(szKey, pkg);
                    CDuiString sSub = pszRepo;
                    sSub += L"\\";
                    sSub += szKey;
                    pkg.sPath = RegGetString(HKEY_CURRENT_USER, sSub.GetData(), L"PackageRootFolder", KEY_READ);
                    // [R126 重建] [R48] 系统内置判定：PackageRootFolder 以 SystemRoot 环境变量或 Windows 目录开头；
                    // [R126 重建] （用户级安装通常在 LocalAppData\Packages\）。
                    if (!pkg.sPath.IsEmpty())
                    {
                        CDuiString sP = pkg.sPath;
                        // [R126 重建] 支持 %SystemRoot% 和直接 C:\Windows 形式
                        if (_wcsnicmp(sP.GetData(), L"%SystemRoot%", 12) == 0
                            || _wcsnicmp(sP.GetData(), L"\\Windows", 8) == 0
                            || wcsstr(sP.GetData(), L":\\Windows\\") != nullptr)
                            pkg.bSystem = true;
                    }
                    if (!pkg.sPath.IsEmpty())
                    {
                        pkg.ullSize = FsDirSize(pkg.sPath);
                        if (wcsstr(pkg.sPath.GetData(), L"SystemApps") != nullptr)
                            pkg.sOwner = L"SYSTEM";
                        else
                            pkg.sOwner = szUser[0] ? szUser : L"Administrator";
                    }
                    pData->vUser.push_back(pkg);
                }
            }
            // provisioned families -> resolve via collected packages
            const wchar_t* pszPre =
                L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Appx\\AppxAllUserStore\\Applications";
            RegKeyGuard hPre;
            LONG lPre = ::RegOpenKeyEx(HKEY_LOCAL_MACHINE, pszPre, 0,
                    KEY_READ | KEY_WOW64_64KEY, hPre.out());
            pData->nPreErr = static_cast<int>(lPre);
            if (lPre == ERROR_SUCCESS)
            {
                DWORD dwIdx = 0;
                for (;;)
                {
                    constexpr int kRegKeyNameWchars = 256;   // 256 缓冲尺寸（语义命名，数值不变）
                    wchar_t szFam[kRegKeyNameWchars] = { 0 };
                    DWORD nFamLen = kRegKeyNameWchars;
                    if (::RegEnumKeyEx(hPre, dwIdx++, szFam, &nFamLen,
                            nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                    // provisioned keys store '~' placeholders for unknown
                    // fields: collapse "_~_" segments into the real name
                    CDuiString sNorm = szFam;
                    while (sNorm.Replace(L"_~_", L"_") > 0) {}
                    CDuiString sKeyFamily = AppxFamily(sNorm);
                    bool bFound = false;
                    for (size_t k = 0; k < pData->vUser.size(); ++k)
                    {
                        if (lstrcmpiW(pData->vUser[k].sFamily.GetData(),
                                     sKeyFamily.GetData()) == 0)
                        {
                            pData->vPre.push_back(pData->vUser[k]);
                            bFound = true;
                            break;
                        }
                    }
                    if (!bFound)
                    {
                        AppxPkg pkg;
                        pkg.sFamily = sKeyFamily;
                        pkg.sFull   = sNorm;
                        pData->vPre.push_back(pkg);
                    }
                }
            }
            auto cmp = [](const AppxPkg& a, const AppxPkg& b) {
                return lstrcmpiW(a.sFull.GetData(), b.sFull.GetData()) < 0;
            };
            std::sort(pData->vUser.begin(), pData->vUser.end(), cmp);
            std::sort(pData->vPre.begin(), pData->vPre.end(), cmp);
            if (::IsWindow(s) && ::PostMessageW(s, WM_APPX_DATA, 0, reinterpret_cast<LPARAM>(msg.get())))   // [加固] 失败时回收，杜绝泄漏
                msg.release();   // [C++17 R3] ownership transferred to UI; otherwise auto-reclaimed at scope exit
            return 0;
        },
        ctx);
    if (h) ::CloseHandle(h);   // [C++17 阶段2] SafeCreateThread 分离线程句柄：单点关闭（文件头约定）
    else
    {
        // [R126] 线程创建失败：ctx 由 make_unique(...).release() 交出，此处必须回收，
        // 否则每次启动失败都泄漏一份 Ctx。同时回传失败消息解除页面加载态。
        if (::IsWindow(hNotify))
        {
            auto failMsg = std::make_unique<AppxDataMsg>();   // [C++17 R3] auto-reclaimed on Post failure
            failMsg->nRepoErr = static_cast<int>(E_OUTOFMEMORY);
            if (::PostMessageW(hNotify, WM_APPX_DATA, 0, reinterpret_cast<LPARAM>(failMsg.get())))
                failMsg.release();   // [C++17 R3] ownership transferred to UI
        }
        std::unique_ptr<Ctx> reclaim(ctx);   // [C++17 R3] thread creation failed, ctx not transferred
    }
}
static void PostCapPartials(HWND s, const std::vector<DismApiBridge::CapabilityInfo>& vApi, CapDataMsg& data, int nGen);

void AppxCapUpdService::StartCapEnumThread(HWND hNotify, int nGen)
{
    struct Ctx { HWND hNotify; int nGen; };
    Ctx* ctx = std::make_unique<Ctx>(Ctx{ hNotify, nGen }).release();   // [C++17 R3] ownership transferred to worker thread
    HANDLE h = SafeCreateThread(
        [](void* pv) -> DWORD {
            std::unique_ptr<Ctx> guard(static_cast<Ctx*>(pv));   // [C++17 R3] thread owns ctx, auto-freed at end
            HWND s = guard->hNotify;
            const int nGen = guard->nGen;
            // [P0-A3 2026-10-11] 原生直连 DismGetCapabilities，
            // 删除 dism.exe /Get-Capabilities 进程捕获与 ANSI/文本解析。
            auto msg = std::make_unique<CapDataMsg>();   // [C++17 R3] ownership transferred to UI on successful Post; auto-reclaimed on failure
            CapDataMsg* pData = msg.get();
            pData->nGen = nGen;   // [STAB] stamp all messages from this run with the caller's generation
            try
            {
                DismApiBridge::Init();
                std::wstring sRoot;
                const wchar_t* pszRoot = nullptr;
                if (COfflineSession::IsActive())
                {
                    sRoot = COfflineSession::GetImageRoot().GetData();
                    pszRoot = sRoot.c_str();
                }
                std::vector<DismApiBridge::CapabilityInfo> vApi;
                std::wstring sErr;
                if (DismApiBridge::GetCapabilities(vApi, &sErr, pszRoot))
                {
                    // [P1] 转换后分批 Post 给 UI（实现见文件尾 PostCapPartials）。
                    // DismGetCapabilities 一次性返回全部数组、无逐项回调，
                    // 因此"流式"只能在数据到手后做：每 100 条发一条
                    // WM_CAP_PARTIAL，页面收到即追加渲染，首屏不必等整表。
                    // 末批 bFinal=true 携带 nExitCode 通知页面收尾并落缓存。
                    PostCapPartials(s, vApi, *pData, nGen);
                    pData->nExitCode = 0;
                }
                else
                {
                    // 与旧进程路线一致：失败时空列表 + 非 0 退出码，UI 走重试/空态
                    pData->nExitCode = static_cast<int>(DismApiBridge::LastHResult());
                    CDuiString sLog;
                    sLog.Format(L"[cap] DismGetCapabilities failed hr=0x%08lX %s",
                                (unsigned long)(LONG_PTR)pData->nExitCode,
                                sErr.empty() ? L""
                                             : CDuiString(sErr.c_str()).GetData());
                    ::OutputDebugStringW(sLog.GetData());
                }
            }
            catch (...)
            {
                // 枚举异常不得吞掉 WM_CAP_DATA，否则页面永久停在“加载中”
                pData->vCaps.clear();
                pData->nExitCode = static_cast<int>(E_FAIL);
                ::OutputDebugStringW(L"[cap] DismGetCapabilities thread exception\r\n");
            }
            // 窗口已销毁 / Post 失败时必须自行释放消息对象
            if (::IsWindow(s) &&
                ::PostMessageW(s, WM_CAP_DATA, 0, reinterpret_cast<LPARAM>(msg.get())))
            {
                msg.release();   // [C++17 R3] ownership transferred to UI; auto-reclaimed on failure
            }
            return 0;
        },
        ctx);
    if (h)
    {
        ::CloseHandle(h);   // [C++17 阶段2] SafeCreateThread 分离线程句柄：单点关闭（文件头约定）
    }
    else
    {
        // 线程创建失败：UI 收不到 WM_CAP_DATA，回送一条失败消息解除加载态
        if (::IsWindow(hNotify))
        {
            auto failMsg = std::make_unique<CapDataMsg>();   // [C++17 R3] auto-reclaimed on Post failure
            failMsg->nExitCode = static_cast<int>(E_OUTOFMEMORY);
            failMsg->nGen = nGen;   // [STAB] same generation so UI drops stale failure messages too
            if (::PostMessageW(hNotify, WM_CAP_DATA, 0, reinterpret_cast<LPARAM>(failMsg.get())))
                failMsg.release();   // [C++17 R3] ownership transferred to UI
        }
        std::unique_ptr<Ctx> reclaim(ctx);   // [C++17 R3] thread creation failed, ctx not transferred, creator reclaims
    }
}

// ============================================================
// [R16][O-4] Capability 枚举结果 TTL 磁盘缓存（stale-while-revalidate）
// 文件：%LOCALAPPDATA%\DismTool_Clone\cap_enum_cache.dat（UTF-8 TSV）
// 首行 "CAPCACHE\t<epoch秒>"，其后每行 8 字段（制表/换行符转义为空格）。
// TTL 600s；LOCALAPPDATA 缺失或读写失败一律返回 false（无缓存=原行为）。
// ============================================================
namespace { inline constexpr time_t kCapCacheTtlSec = 3600;   // [T2 P0] TTL 600s→3600s：正确性由写后主动失效保证，避免"10 分钟后缓存过期回到转圈"

bool CapCachePath(std::wstring& sPath)
{
    // [B6] winenv::SafeGetEnv 替代弃用 _wgetenv（C4996 清零）；返回临时 std::wstring，不可绑定裸指针。
    std::wstring sDir = winenv::SafeGetEnv(L"LOCALAPPDATA");
    if (sDir.empty()) return false;
    sDir += L"\\DismTool_Clone";
    ::CreateDirectoryW(sDir.c_str(), nullptr);   // 已存在时失败可忽略
    sPath = sDir + L"\\cap_enum_cache.dat";
    return true;
}

void CapCacheEscape(const CDuiString& sIn, std::string& sOut)
{
    std::wstring w = sIn.GetData() ? sIn.GetData() : std::wstring();
    for (size_t i = 0; i < w.size(); ++i)
    {
        wchar_t ch = w[i];
        if (ch == L'\t' || ch == L'\r' || ch == L'\n') ch = L' ';
        w[i] = ch;
    }
    // [R-P23 · R3] 由 Rust dismcore::Utf16ToUtf8 替代 WideCharToMultiByte 两段式，
    // 消除"查长度→分配→再转换"的缓冲区边界风险
    const auto bytes = dismcore::Utf16ToUtf8(w.data(), w.size());
    sOut.assign(bytes.begin(), bytes.end());
}

CDuiString CapCacheUnescape(const std::string& s)
{
    // [R-P23 · R3] 由 Rust dismcore::Utf8ToUtf16 替代 MultiByteToWideChar 两段式
    const auto units = dismcore::Utf8ToUtf16(s.data(), s.size());
    if (units.empty()) return CDuiString();
    std::wstring w(units.begin(), units.end());
    return CDuiString(w.c_str());
}
} // namespace

bool AppxCapUpdService::SaveCapCache(const std::vector<CapEntry>& vCaps)
{
    if (vCaps.empty()) return false;
    std::wstring sPath;
    if (!CapCachePath(sPath)) return false;

    std::string sBody;
    {
        std::string sHead;
        sHead.resize(64);
        std::snprintf(sHead.data(), sHead.size() + 1, "CAPCACHE\t%llu\r\n",
                    (unsigned long long)::time(nullptr));
        sHead.resize(strlen(sHead.c_str()));
        sBody += sHead;
    }
    for (size_t i = 0; i < vCaps.size(); ++i)
    {
        const CapEntry& e = vCaps[i];
        std::string a, b, c, d, g, h;
        CapCacheEscape(e.sIdentity, a);
        CapCacheEscape(e.sState, b);
        CapCacheEscape(e.sDescription, c);
        CapCacheEscape(e.sSize, d);
        CapCacheEscape(e.sFirstRequiredDep, g);
        CapCacheEscape(e.sFirstOptionalDep, h);
        sBody += a + '\t' + b + '\t' + c + '\t' + d;
        std::string sNum;
        sNum.resize(32);
        std::snprintf(sNum.data(), sNum.size() + 1, "\t%d\t%d", e.nRequiredDeps, e.nOptionalDeps);
        sNum.resize(strlen(sNum.c_str()));
        sBody += sNum;
        sBody += '\t'; sBody += g; sBody += '\t'; sBody += h; sBody += "\r\n";
    }

    const std::wstring sTmp = sPath + L".tmp";   // [STAB] 原子写，防并发读到半写文件
    // [R215] The write handle must be CLOSED before the rename. FileGuard is an RAII
    //   guard whose destructor runs AFTER the return expression is evaluated, so the
    //   previous one-liner `return ::MoveFileExW(...)` renamed the file while this
    //   handle (share mode 0) still held it open. MoveFileExW then failed with
    //   ERROR_SHARING_VIOLATION and SaveCapCache returned false on every call --
    //   the capability cache never persisted, so a page reload always re-enumerated
    //   and the TTL tests below could never find a header to age.
    {
        FileGuard hFile = ::CreateFileW(sTmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return false;
        DWORD dwWritten = 0;
        const BOOL bOk = ::WriteFile(hFile, sBody.data(), static_cast<DWORD>(sBody.size()), &dwWritten, nullptr);
        if (!bOk || dwWritten != static_cast<DWORD>(sBody.size()))
        {
            ::DeleteFileW(sTmp.c_str());   // do not leave a partial file behind
            return false;
        }
    }   // hFile is closed here, before the rename
    return ::MoveFileExW(sTmp.c_str(), sPath.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

bool AppxCapUpdService::LoadCapCache(std::vector<CapEntry>& vOut, bool bIgnoreTtl)
{
    vOut.clear();
    std::wstring sPath;
    if (!CapCachePath(sPath)) return false; ::DeleteFileW((sPath + L".tmp").c_str());   // [STAB] 清 .tmp

    FileGuard hFile = ::CreateFileW(sPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD dwSize = ::GetFileSize(hFile, nullptr);
    if (dwSize == 0 || dwSize == INVALID_FILE_SIZE || dwSize > 4u * 1024u * 1024u)
    {
        return false;
    }
    std::string sBody(dwSize, '\0');
    DWORD dwRead = 0;
    BOOL bOk = ::ReadFile(hFile, &sBody[0], dwSize, &dwRead, nullptr);
    if (!bOk || dwRead != dwSize) return false;

    size_t pos = 0;
    auto nextLine = [&sBody, &pos](void) -> std::string
    {
        size_t e = sBody.find("\r\n", pos);
        std::string s;
        if (e == std::string::npos) { s = sBody.substr(pos); pos = sBody.size(); }
        else { s = sBody.substr(pos, e - pos); pos = e + 2; }
        return s;
    };

    std::string sHead = nextLine();
    if (sHead.compare(0, 9, "CAPCACHE\t") != 0) return false;
    unsigned long long ullSaved = _strtoui64(sHead.c_str() + 9, nullptr, 10);
    time_t nNow = ::time(nullptr);
    // [fix-cap-cache] bIgnoreTtl=true（枚举失败兜底渲染）仍保留时钟回拨守卫，
    // 仅放宽 TTL 时限；默认路径行为不变。
    if ((unsigned long long)nNow < ullSaved)
        return false;   // 时钟回拨异常按无缓存处理
    if (!bIgnoreTtl && (time_t)(nNow - (time_t)ullSaved) > kCapCacheTtlSec)
        return false;   // TTL 过期按无缓存处理

    while (pos < sBody.size())
    {
        std::string sLine = nextLine();
        if (sLine.empty()) continue;
        std::string f[8];
        size_t p = 0;
        int n = 0;
        for (; n < 7; ++n)
        {
            size_t t = sLine.find('\t', p);
            if (t == std::string::npos) break;
            f[n] = sLine.substr(p, t - p);
            p = t + 1;
        }
        if (n < 7) continue;                 // 字段不足：跳过该行（格式容错）
        f[7] = sLine.substr(p);

        CapEntry e;
        e.sIdentity = CapCacheUnescape(f[0]);
        if (e.sIdentity.IsEmpty()) continue;
        e.sState = CapCacheUnescape(f[1]);
        e.sDescription = CapCacheUnescape(f[2]);
        e.sSize = CapCacheUnescape(f[3]);
        e.nRequiredDeps = static_cast<int>(std::stoi(f[4]));
        e.nOptionalDeps = static_cast<int>(std::stoi(f[5]));
        e.sFirstRequiredDep = CapCacheUnescape(f[6]);
        e.sFirstOptionalDep = CapCacheUnescape(f[7]);
        vOut.push_back(e);
    }
    return !vOut.empty();
}


// ============================================================
// [性能 A1] 已安装/待安装更新列表 TTL 磁盘缓存（仿 Cap 缓存先例）
// 文件：upd_enum_cache_t0.dat / upd_enum_cache_t1.dat（按 Tab 分文件，
// 杜绝离线/在线串数据——离线会话由页面侧判定后直接跳过缓存读写）。
// ============================================================
namespace {
inline constexpr time_t kUpdCacheTtlSec = 3600;   // [T2 P0] TTL 600s→3600s：与 kCapCacheTtlSec 同理由（写后主动失效保证正确性）

bool UpdCachePath(int nTab, std::wstring& sPath)
{
    // [B6] winenv::SafeGetEnv 替代弃用 _wgetenv（C4996 清零）；返回临时 std::wstring，不可绑定裸指针。
    std::wstring sDir = winenv::SafeGetEnv(L"LOCALAPPDATA");
    if (sDir.empty()) return false;
    sDir += L"\\DismTool_Clone";
    ::CreateDirectoryW(sDir.c_str(), nullptr);   // 已存在时失败可忽略
    sPath = sDir + (nTab == 0 ? L"\\upd_enum_cache_t0.dat"
                              : L"\\upd_enum_cache_t1.dat");
    return true;
}
} // namespace

bool AppxCapUpdService::SaveUpdCache(int nTab, const std::vector<UpdEntry>& vUpd)
{
    if (vUpd.empty()) return false;
    std::wstring sPath;
    if (!UpdCachePath(nTab, sPath)) return false;

    std::string sBody;
    {
        std::string sHead;
        sHead.resize(64);
        std::snprintf(sHead.data(), sHead.size() + 1, "UPDCACHE\t%llu\r\n",
                    (unsigned long long)::time(nullptr));
        sHead.resize(strlen(sHead.c_str()));
        sBody += sHead;
    }
    for (size_t i = 0; i < vUpd.size(); ++i)
    {
        const UpdEntry& e = vUpd[i];
        std::string a, b, c, d;
        CapCacheEscape(e.sTitle, a);
        CapCacheEscape(e.sDate, b);
        CapCacheEscape(e.sUpdateID, c);
        CapCacheEscape(e.sKB, d);
        sBody += a + '\t' + b + '\t' + c + '\t' + d + "\r\n";
    }

    FileGuard hFile = ::CreateFileW(sPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD dwWritten = 0;
    BOOL bOk = ::WriteFile(hFile, sBody.data(), static_cast<DWORD>(sBody.size()), &dwWritten, nullptr);
    return bOk != FALSE && dwWritten == static_cast<DWORD>(sBody.size());
}

bool AppxCapUpdService::LoadUpdCache(int nTab, std::vector<UpdEntry>& vOut)
{
    vOut.clear();
    std::wstring sPath;
    if (!UpdCachePath(nTab, sPath)) return false;

    FileGuard hFile = ::CreateFileW(sPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD dwSize = ::GetFileSize(hFile, nullptr);
    if (dwSize == 0 || dwSize == INVALID_FILE_SIZE || dwSize > 4u * 1024u * 1024u)
    {
        return false;
    }
    std::string sBody(dwSize, '\0');
    DWORD dwRead = 0;
    BOOL bOk = ::ReadFile(hFile, &sBody[0], dwSize, &dwRead, nullptr);
    if (!bOk || dwRead != dwSize) return false;

    size_t pos = 0;
    auto nextLine = [&sBody, &pos](void) -> std::string
    {
        size_t e = sBody.find("\r\n", pos);
        std::string s;
        if (e == std::string::npos) { s = sBody.substr(pos); pos = sBody.size(); }
        else { s = sBody.substr(pos, e - pos); pos = e + 2; }
        return s;
    };

    std::string sHead = nextLine();
    if (sHead.compare(0, 9, "UPDCACHE\t") != 0) return false;
    unsigned long long ullSaved = _strtoui64(sHead.c_str() + 9, nullptr, 10);
    time_t nNow = ::time(nullptr);
    if ((unsigned long long)nNow < ullSaved || (time_t)(nNow - (time_t)ullSaved) > kUpdCacheTtlSec)
        return false;   // TTL 过期（或时钟回拨异常）按无缓存处理

    while (pos < sBody.size())
    {
        std::string sLine = nextLine();
        if (sLine.empty()) continue;
        std::string f[4];
        size_t p = 0;
        int n = 0;
        for (; n < 3; ++n)
        {
            size_t t = sLine.find('\t', p);
            if (t == std::string::npos) break;
            f[n] = sLine.substr(p, t - p);
            p = t + 1;
        }
        if (n < 3) continue;                 // 字段不足：跳过该行（格式容错）
        f[3] = sLine.substr(p);

        UpdEntry e;
        e.sTitle = CapCacheUnescape(f[0]);
        if (e.sTitle.IsEmpty()) continue;
        e.sDate = CapCacheUnescape(f[1]);
        e.sUpdateID = CapCacheUnescape(f[2]);
        e.sKB = CapCacheUnescape(f[3]);
        vOut.push_back(e);
    }
    return !vOut.empty();
}

void AppxCapUpdService::InvalidateUpdCache()
{
    std::wstring sPath;
    for (int nTab = 0; nTab < 2; ++nTab)
    {
        // [R119] 删除的是 TTL 缓存文件：删失败只会让下次多读一次旧缓存，
        //   不影响正确性（读时仍会按 TTL 判断）。故刻意不检查返回值。
        if (UpdCachePath(nTab, sPath))
            ::DeleteFileW(sPath.c_str());
    }
}

// [T6] 扫描服务持久复用（实现见文件末，避开 L1180/L1371 白名单）
static std::wstring SvCabStamp(const wchar_t*); static bool SvLoadRecord(std::wstring&, std::wstring&, bool&);
static void SvSaveRecord(const std::wstring&, const std::wstring&); static void SvClearRecord();
static bool SvCachedServiceAlive(IDispatch*, const std::wstring&); static bool UeTryReuseScanService(IDispatch*, const wchar_t*, IDispatch**, BSTR*); static void UeKillScanService(IDispatch*, BSTR); static void SvKillPersistedService();
// [FIX-P2B] 复用标志由裸 bool 改 Interlocked LONG：WUA 枚举线程读写 ×
// 复用/注销路径写，两次枚举并发（generation 守卫只防 UI 覆盖不防线程并存）
// 时裸 bool 是数据竞争。
static volatile LONG g_bScanSidReused = 0;
[[nodiscard]] static bool ScanSidReused() { return ::InterlockedCompareExchange(&g_bScanSidReused, 0, 0) != 0; }
static void SetScanSidReused(bool b) { ::InterlockedExchange(&g_bScanSidReused, b ? 1 : 0); }

// [U-Offline] 配置离线扫描源：ssOthers + AddScanPackageService 注册 cab
static void UeConfigureOfflineSource(IDispatch* pSearcher, IDispatch** ppSvcMgr,
                                     BSTR* ppScanSid)
{
    VARIANT vSS; SetScanSidReused(false);
    ::VariantInit(&vSS);
    vSS.vt = VT_I4;
    vSS.lVal = 3;   // ssOthers = 离线目录
    HRESULT hrSS = AutoWrap(DISPATCH_PROPERTYPUT,
            &vSS, pSearcher, L"ServerSelection", 1, &vSS);
    if (FAILED(hrSS)) { ::VariantClear(&vSS); return; }
    ::VariantClear(&vSS);

    // [P0] 离线扫描权威链：用随包 wsusscn3.cab（回退 wsusscn2.cab）注册扫描包服务
    wchar_t szExe[MAX_PATH] = { 0 };
    ::GetModuleFileNameW(nullptr, szExe, MAX_PATH);
    CDuiString sExeDir = szExe;
    int nSlashExe = sExeDir.ReverseFind(L'\\');
    if (nSlashExe >= 0) sExeDir = sExeDir.Left(nSlashExe);
    CDuiString sCab3 = sExeDir + L"\\wsusscn3.cab";
    CDuiString sCab2 = sExeDir + L"\\wsusscn2.cab";
    CDuiString sCab;
    if (::GetFileAttributesW(sCab3.GetData()) != INVALID_FILE_ATTRIBUTES) sCab = sCab3;
    else if (::GetFileAttributesW(sCab2.GetData()) != INVALID_FILE_ATTRIBUTES) sCab = sCab2;
    if (sCab.IsEmpty()) return;
    // [T6] 缓存命中（cab 未变 + 服务仍活）直接复用，跳过 100MB+ cab 重新解析
    if (UeTryReuseScanService(pSearcher, sCab.GetData(), ppSvcMgr, ppScanSid)) return;

    IDispatch* pSvcMgr = nullptr;
    CLSID clsidSvc = { 0 };
    if (SUCCEEDED(::CLSIDFromProgID(
            L"Microsoft.Update.ServiceManager", &clsidSvc)))
    {
        HRESULT hrSM = ::CoCreateInstance(clsidSvc, nullptr,
            CLSCTX_ALL, IID_IDispatch, (void**)&pSvcMgr);
        if (SUCCEEDED(hrSM) && pSvcMgr)
        {
            VARIANT vRet; VariantInit(&vRet);
            VARIANT vName; VariantInit(&vName);
            vName.vt = VT_BSTR; vName.bstrVal = SysAllocString(L"");
            VARIANT vCab; VariantInit(&vCab);
            vCab.vt = VT_BSTR; vCab.bstrVal = SysAllocString(sCab.GetData());
            VARIANT vFlags; VariantInit(&vFlags);
            vFlags.vt = VT_I4; vFlags.lVal = 0;
            VARIANT vArgs[3];
            vArgs[0] = vName;   // serviceName（基线传空串）
            vArgs[1] = vCab;    // authorizationCabPath = wsusscn cab
            vArgs[2] = vFlags;  // flags = 0
            HRESULT hrAdd = AutoWrap(DISPATCH_METHOD, &vRet,
                pSvcMgr, L"AddScanPackageService",
                3, vArgs);
            if (SUCCEEDED(hrAdd) && vRet.vt == VT_BSTR && vRet.bstrVal)
            {
                *ppScanSid = vRet.bstrVal;   // 转移所有权，不 Clear
                SvSaveRecord(std::wstring(*ppScanSid), SvCabStamp(sCab.GetData()));   // [T6] 落记录
            }
            else
            {
                VariantClear(&vRet);
            }
            VariantClear(&vName);
            VariantClear(&vCab);
            VariantClear(&vFlags);
        }
    }
    *ppSvcMgr = pSvcMgr;

    // 设置 ServiceID
    VARIANT vSid;
    ::VariantInit(&vSid);
    vSid.vt = VT_BSTR;
    if (*ppScanSid)
    {
        vSid.bstrVal = ::SysAllocString(*ppScanSid);
    }
    else
    {
        vSid.bstrVal = ::SysAllocString(
            L"7971f918-a847-4430-9279-4a52d1efe18d");
    }
    AutoWrap(DISPATCH_PROPERTYPUT,
            &vSid, pSearcher, L"ServiceID", 1, &vSid);
    ::VariantClear(&vSid);
}

// [U-Entry] KBArticleIDs 数组遍历（BSTR 数组 → ";" 分隔，压平嵌套）
static void UeAppendKbArray(VARIANT& vKB, UpdEntry& e)
{
    LONG lLB = 0, lUB = 0;
    if (::SafeArrayGetLBound(vKB.parray, 1, &lLB) == S_OK
        && ::SafeArrayGetUBound(vKB.parray, 1, &lUB) == S_OK)
    {
        for (LONG j = lLB; j <= lUB; ++j)
        {
            BSTR bstrKB = nullptr;
            if (::SafeArrayGetElement(vKB.parray, &j, &bstrKB) == S_OK
                && bstrKB)
            {
                if (!e.sKB.IsEmpty()) e.sKB += L"; ";
                e.sKB += bstrKB;
                ::SysFreeString(bstrKB);
            }
        }
    }
}

// [U-Entry] KBArticleIDs（BSTR 数组）→ ";" 分隔（提取为独立函数，压平嵌套）
static void UeParseKbArticleIds(IDispatch* pUp, UpdEntry& e)
{
    VARIANT vKB;
    ::VariantInit(&vKB);
    HRESULT hrKB = AutoWrap(DISPATCH_PROPERTYGET,
            &vKB, pUp, L"KBArticleIDs", 0, nullptr);
    if (SUCCEEDED(hrKB))
    {
        if (vKB.vt == (VT_ARRAY | VT_BSTR) && vKB.parray)
            UeAppendKbArray(vKB, e);
        else if (vKB.vt == VT_BSTR && vKB.bstrVal)
            e.sKB = vKB.bstrVal;
    }
    ::VariantClear(&vKB);
}

// [U-Entry] 诊断留痕（原为 lambda 局部，提取为文件级以便 UeEnumUpdates 复用）
static void UeDiag(const char* tag, HRESULT hr, int n)
{
    std::string sBuf;
    sBuf.resize(128);
    std::snprintf(sBuf.data(), sBuf.size() + 1, "%s hr=0x%08lX n=%d\n",
                tag, (unsigned long)hr, n);
    sBuf.resize(strlen(sBuf.c_str()));
    ::OutputDebugStringA(sBuf.c_str());
}

// [U-Entry] 前置声明：UeFillUpdEntry 定义在 UeEnumUpdates 之后
static void UeFillUpdEntry(IDispatch* pUp, UpdEntry& e);

void PostOneUpd(HWND, int, int, const UpdEntry&); void FlushUpdPartial(HWND, int, int);   // [P1] 定义在文件尾；下行枚举 IUpdateCollection [R3-FIX] 加 nGen
static void UeEnumUpdates(IDispatch* pUps, UpdDataMsg* pData, HWND hNotify, int nTab, int nGen)
{
    VARIANT vCnt;
    ::VariantInit(&vCnt);
    int nCnt = 0;
    HRESULT hrCnt = AutoWrap(DISPATCH_PROPERTYGET, &vCnt,
            pUps, L"Count", 0, nullptr);
    if (SUCCEEDED(hrCnt)) nCnt = vCnt.lVal;
    UeDiag("Count", hrCnt, nCnt);
    for (int i = 0; i < nCnt; ++i)
    {
        VARIANT vi;
        ::VariantInit(&vi);
        vi.vt = VT_I4;
        vi.lVal = i;
        VARIANT vItem;
        ::VariantInit(&vItem);
        HRESULT hrItem = AutoWrap(DISPATCH_PROPERTYGET, &vItem,
                pUps, L"Item", 1, &vi);
        UeDiag("Item", hrItem, i);
        if (FAILED(hrItem)) continue;
        IDispatch* pUp = vItem.pdispVal;
        UpdEntry e;
        UeFillUpdEntry(pUp, e);
        // [P1] 攒批：每满 100 条即 Post 一批给 UI（实现见文件尾 PostOneUpd）
        if (!e.sTitle.IsEmpty()) { pData->vUpd.push_back(e); PostOneUpd(hNotify, nTab, nGen, e); }
        ::VariantClear(&vItem);
    }
    ::VariantClear(&vCnt); FlushUpdPartial(hNotify, nTab, nGen);   // [P1] 尾批 [R3-FIX]
}

// [U-Entry] 单个 IUpdate 条目填充 Title/Date/UpdateID/KB
static void UeFillUpdEntry(IDispatch* pUp, UpdEntry& e)
{
    VARIANT vTitle;
    ::VariantInit(&vTitle);
    HRESULT hrT = AutoWrap(DISPATCH_PROPERTYGET,
            &vTitle, pUp, L"Title", 0, nullptr);
    if (SUCCEEDED(hrT) && vTitle.bstrVal)
        e.sTitle = vTitle.bstrVal;
    ::VariantClear(&vTitle);

    VARIANT vDate;
    ::VariantInit(&vDate);
    HRESULT hrD = AutoWrap(DISPATCH_PROPERTYGET,
            &vDate, pUp,
            L"LastDeploymentChangeTime", 0, nullptr);
    if (SUCCEEDED(hrD) && vDate.vt == VT_DATE)
    {
        SYSTEMTIME st = { 0 };
        if (::VariantTimeToSystemTime(vDate.date, &st))
        {
            std::wstring sDate;
            sDate.resize(32);
            wfmt::FormatInto(&sDate[0], sDate.size(),
                L"%04d/%02d/%02d",
                st.wYear, st.wMonth, st.wDay);
            sDate.resize(wcslen(sDate.c_str()));
            e.sDate = sDate.c_str();
        }
    }
    ::VariantClear(&vDate);

    // [P1-15b] 取 IUpdateIdentity.UpdateID，供卸载定位使用
    {
        VARIANT vId;
        ::VariantInit(&vId);
        HRESULT hrId = AutoWrap(DISPATCH_PROPERTYGET,
                &vId, pUp, L"Identity", 0, nullptr);
        if (SUCCEEDED(hrId) && vId.pdispVal)
        {
            VARIANT vUid;
            ::VariantInit(&vUid);
            HRESULT hrU = AutoWrap(DISPATCH_PROPERTYGET,
                    &vUid, vId.pdispVal, L"UpdateID", 0, nullptr);
            if (SUCCEEDED(hrU) && vUid.bstrVal)
                e.sUpdateID = vUid.bstrVal;
            ::VariantClear(&vUid);
            ::VariantClear(&vId);
        }
    }

    // [R86 P1-17] KBArticleIDs（BSTR 数组）→ ";" 分隔（压平到独立函数）
    UeParseKbArticleIds(pUp, e);
}

void AppxCapUpdService::StartUpdateEnumThread(HWND hNotify, int nTab, const CDuiString& sCrit, int nGen)
{
    // [R3-FIX] nGen generation 守卫：由页面每次 StartEnumeration 递增，
    // 工作线程透传到所有回传消息，UI 侧 OnData/OnPartial 比对当前 generation，
    // 过期消息直接 delete 丢弃，防止"上一次枚举回传覆盖新一次结果"的竞态。
    struct Ctx { HWND hNotify; int nTab; CDuiString sCrit; int nGen; };
    Ctx* ctx = std::make_unique<Ctx>(Ctx{ hNotify, nTab, sCrit, nGen }).release();   // [C++17 R3] ownership transferred to worker thread
    HANDLE h = SafeCreateThread(
        [](void* pv) -> DWORD {
            std::unique_ptr<Ctx> guard(static_cast<Ctx*>(pv));   // [C++17 R3] thread owns ctx, auto-freed at end
            HWND s = guard->hNotify;
            int nTabLocal = guard->nTab;
            CDuiString sCritLocal = guard->sCrit;
            const int nGenLocal = guard->nGen;
            auto msg = std::make_unique<UpdDataMsg>();   // [C++17 R3] ownership transferred to UI on successful Post; auto-reclaimed on failure
            UpdDataMsg* pData = msg.get();
            pData->nTab = nTabLocal;
            pData->nGen = nGenLocal;
            HRESULT hrCo = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            UeDiag("CoInit", hrCo, 0);
            bool bComInit = SUCCEEDED(hrCo);
            IDispatch* pSession = nullptr;
            IDispatch* pSvcMgr = nullptr;
            BSTR bstrScanSid = nullptr;
            CLSID clsid = { 0 };
            HRESULT hrCls = ::CLSIDFromProgID(L"Microsoft.Update.Session", &clsid);
            UeDiag("CLSID", hrCls, 0);
            if (SUCCEEDED(hrCls))
            {
                HRESULT hr = ::CoCreateInstance(clsid, nullptr, CLSCTX_ALL,
                            IID_IDispatch, (void**)&pSession);
                UeDiag("Create", hr, 0);
                if (FAILED(hr)) pData->nErr = static_cast<int>(hr);
            }
            if (pSession)
            {
                VARIANT vSearcher;
                ::VariantInit(&vSearcher);
                HRESULT hrS = AutoWrap(DISPATCH_METHOD | DISPATCH_PROPERTYGET,
                        &vSearcher, pSession, L"CreateUpdateSearcher", 0, nullptr);
                UeDiag("CreateSearcher", hrS, 0);
                if (SUCCEEDED(hrS))
                {                    IDispatch* pSearcher = vSearcher.pdispVal;
                    // [R86 P1-17] 离线源配置 + 扫描包服务注册
                    if (IsOfflineSourceEnabled())
                        UeConfigureOfflineSource(pSearcher, &pSvcMgr, &bstrScanSid);

                    VARIANT vCrit;
                    ::VariantInit(&vCrit);
                    vCrit.vt = VT_BSTR;
                    vCrit.bstrVal = ::SysAllocString(sCritLocal.GetData());
                    VARIANT vResult;
                    ::VariantInit(&vResult);
                    HRESULT hrSearch = AutoWrap(DISPATCH_METHOD, &vResult,
                            pSearcher, L"Search", 1, &vCrit);
                    UeDiag("Search", hrSearch, 0);
                    if (SUCCEEDED(hrSearch))
                    {
                        IDispatch* pSr = vResult.pdispVal;
                        VARIANT vUpdates;
                        ::VariantInit(&vUpdates);
                        HRESULT hrUps = AutoWrap(DISPATCH_PROPERTYGET, &vUpdates,
                                pSr, L"Updates", 0, nullptr);
                        UeDiag("Updates", hrUps, 0);
                        if (SUCCEEDED(hrUps))
                        {
                            IDispatch* pUps = vUpdates.pdispVal;
                            UeEnumUpdates(pUps, pData, s, pData->nTab, nGenLocal);
                        }
                        else pData->nErr = static_cast<int>(hrUps);   // [FIX-S1-5] Updates 取值失败不再是"无更新"
                        ::VariantClear(&vUpdates);
                    }
                    else pData->nErr = static_cast<int>(hrSearch);   // [FIX-S1-5] Search 失败（如 wuauserv 停用）不再假成功"已是最新"
                    ::VariantClear(&vResult);
                    ::VariantClear(&vCrit);
                }
                else pData->nErr = static_cast<int>(hrS);   // [FIX-S1-5] Searcher 创建失败不再假成功
                ::VariantClear(&vSearcher);
            }
            // [T6] 扫描服务生命周期改由 scan_svc.dat 托管：注册成功后不再每次
            // 枚举尾注销，改由切回在线源（自愈②）/ cab 变更（自愈③）时注销；
            // 若落记录失败（LOCALAPPDATA 不可写等），退回原行为立即注销，
            // 避免留下孤儿扫描服务。
            if (pSvcMgr && bstrScanSid && !ScanSidReused())
            {
                std::wstring sSidRec, sCabRec;
                bool bRec = false;
                const bool bPersisted = SvLoadRecord(sSidRec, sCabRec, bRec) && bRec;
                if (!bPersisted)
                {
                    // 无记录即本次注册未落盘，必须立即注销，否则成孤儿
                    UeKillScanService(pSvcMgr, bstrScanSid);
                }
                // BSTR 无论走哪条路径都要释放（服务本身已由记录托管）
                ::SysFreeString(bstrScanSid);
                bstrScanSid = nullptr;
            }
            if (pSvcMgr) { pSvcMgr->Release(); pSvcMgr = nullptr; if (bstrScanSid && ScanSidReused()) { ::SysFreeString(bstrScanSid); bstrScanSid = nullptr; } }   // [T6] 复用命中不注销，仅释放 BSTR
            if (pSession) pSession->Release();
            if (bComInit) ::CoUninitialize();
            if (::IsWindow(s) && ::PostMessageW(s, WM_UPD_DATA, 0, reinterpret_cast<LPARAM>(msg.get())))   // [加固] 失败时回收
                msg.release();   // [C++17 R3] ownership transferred to UI; otherwise auto-reclaimed at scope exit
            return 0;
        },
        ctx);
    if (h) ::CloseHandle(h);   // [C++17 阶段2] SafeCreateThread 分离线程句柄：单点关闭（文件头约定）
    else
    {
        // [R126] 同上：线程未接管，回收 ctx 并回传失败消息解除加载态
        if (::IsWindow(hNotify))
        {
            auto failMsg = std::make_unique<UpdDataMsg>();   // [C++17 R3] auto-reclaimed on Post failure
            failMsg->nKind = 3;   // 3 = 失败
            failMsg->nErr = static_cast<int>(E_OUTOFMEMORY);
            failMsg->nTab = nTab;
            failMsg->nGen = nGen;   // [R3-FIX] 失败消息也要带 generation
            if (::PostMessageW(hNotify, WM_UPD_DATA, 0, reinterpret_cast<LPARAM>(failMsg.get())))
                failMsg.release();   // [C++17 R3] ownership transferred to UI
        }
        std::unique_ptr<Ctx> reclaim(ctx);   // [C++17 R3] thread creation failed, ctx not transferred
    }
}

// [R11-02] 在已安装更新集合中按 UpdateID 匹配目标 IUpdate（StartUpdateUninstallThread 内层提取）
// [R13-01] UaFindTargetUpdate 深度拆解辅助（d6->d<=5）
// 仅提取单个 IUpdate 是否匹配 sId 的判定，逐字搬迁原逻辑，零行为改动。

// [UF-Item] 单条 IUpdate 的 UpdateID 是否等于 sId；命中时返回 AddRef 后的 pItem
static IDispatch* UfMatchItem(IDispatch* pItem, const CDuiString& sId)
{
    VARIANT vId; ::VariantInit(&vId);
    if (SUCCEEDED(AutoWrap(DISPATCH_PROPERTYGET, &vId,
            pItem, L"Identity", 0, nullptr)) && vId.pdispVal)
    {
        VARIANT vUid; ::VariantInit(&vUid);
        IDispatch* pRet = nullptr;
        if (SUCCEEDED(AutoWrap(DISPATCH_PROPERTYGET,
                &vUid, vId.pdispVal, L"UpdateID", 0, nullptr))
            && vUid.bstrVal)
        {
            if (sId.Compare(vUid.bstrVal) == 0)
            {
                pRet = pItem;
                pRet->AddRef();
            }
        }
        ::VariantClear(&vUid);
        ::VariantClear(&vId);
        return pRet;
    }
    ::VariantClear(&vId);
    return nullptr;
}

static IDispatch* UaFindTargetUpdate(IDispatch* pSession, const CDuiString& sId)
{
    IDispatch* pTarget = nullptr;
    VARIANT vSearcher;
    ::VariantInit(&vSearcher);
    if (SUCCEEDED(AutoWrap(DISPATCH_METHOD | DISPATCH_PROPERTYGET,
            &vSearcher, pSession, L"CreateUpdateSearcher", 0, nullptr)))
    {
        VARIANT vCrit;
        ::VariantInit(&vCrit);
        vCrit.vt = VT_BSTR;
        vCrit.bstrVal = ::SysAllocString(L"IsInstalled=1");
        VARIANT vRes;
        ::VariantInit(&vRes);
        if (SUCCEEDED(AutoWrap(DISPATCH_METHOD, &vRes,
                vSearcher.pdispVal, L"Search", 1, &vCrit)))
        {
            VARIANT vUps;
            ::VariantInit(&vUps);
            if (SUCCEEDED(AutoWrap(DISPATCH_PROPERTYGET, &vUps,
                    vRes.pdispVal, L"Updates", 0, nullptr)))
            {
                VARIANT vCnt;
                ::VariantInit(&vCnt);
                int nCnt = 0;
                if (SUCCEEDED(AutoWrap(DISPATCH_PROPERTYGET, &vCnt,
                        vUps.pdispVal, L"Count", 0, nullptr)))
                    nCnt = vCnt.lVal;
                for (int i = 0; i < nCnt && !pTarget; ++i)
                {
                    VARIANT vi; ::VariantInit(&vi);
                    vi.vt = VT_I4; vi.lVal = i;
                    VARIANT vItem; ::VariantInit(&vItem);
                    if (FAILED(AutoWrap(DISPATCH_PROPERTYGET, &vItem,
                            vUps.pdispVal, L"Item", 1, &vi)))
                        continue;
                    pTarget = UfMatchItem(vItem.pdispVal, sId);
                    ::VariantClear(&vItem);
                }
                ::VariantClear(&vCnt);
                ::VariantClear(&vUps);
            }
            ::VariantClear(&vRes);
        }
        ::VariantClear(&vCrit);
        ::VariantClear(&vSearcher);
    }
    return pTarget;
}

// [R11-02] 对目标 IUpdate 执行 WUA 卸载（StartUpdateUninstallThread 内层提取）
static void UaRunUpdateUninstall(IDispatch* pSession, IDispatch* pTarget, UpdDataMsg* pData)
{
    if (pTarget)
    {
        CLSID clsidIns = { 0 };
        IDispatch* pInstaller = nullptr;
        if (SUCCEEDED(::CLSIDFromProgID(L"Microsoft.Update.Installer", &clsidIns)))
            ::CoCreateInstance(clsidIns, nullptr, CLSCTX_ALL, IID_IDispatch,
                               (void**)&pInstaller);
        if (pInstaller)
        {
            // [R126 重建] Silent = TRUE，避免 WUA 自带的交互 UI
            VARIANT vSilent; ::VariantInit(&vSilent);
            vSilent.vt = VT_BOOL; vSilent.boolVal = VARIANT_TRUE;
            AutoWrap(DISPATCH_PROPERTYPUT, nullptr, pInstaller,
                     L"Silent", 1, &vSilent);
            // [R126 重建] 复用 IUpdateCollection 承载单个 IUpdate
            VARIANT vColl; ::VariantInit(&vColl);
            if (SUCCEEDED(AutoWrap(DISPATCH_METHOD | DISPATCH_PROPERTYGET,
                    &vColl, pSession, L"CreateUpdateCollection", 0, nullptr))
                && vColl.pdispVal)
            {
                VARIANT vIdx; ::VariantInit(&vIdx);
                vIdx.vt = VT_I4; vIdx.lVal = 0;
                VARIANT vUpd; ::VariantInit(&vUpd);
                vUpd.vt = VT_DISPATCH; vUpd.pdispVal = pTarget;
                AutoWrap(DISPATCH_METHOD, nullptr, vColl.pdispVal,
                         L"Add", 1, &vUpd);
                VARIANT vPut; ::VariantInit(&vPut);
                vPut.vt = VT_DISPATCH; vPut.pdispVal = vColl.pdispVal;
                AutoWrap(DISPATCH_PROPERTYPUT, nullptr, pInstaller,
                         L"Updates", 1, &vPut);
                VARIANT vRule; ::VariantInit(&vRule);
                if (SUCCEEDED(AutoWrap(DISPATCH_METHOD, &vRule,
                        pInstaller, L"Uninstall", 0, nullptr))
                    && vRule.pdispVal)
                {
                    // IInstallationResult: ResultCode / HResult
                    VARIANT vRc; ::VariantInit(&vRc);
                    if (SUCCEEDED(AutoWrap(DISPATCH_PROPERTYGET, &vRc,
                            vRule.pdispVal, L"ResultCode", 0, nullptr)))
                        pData->nResultCode = vRc.lVal;
                    VARIANT vHr; ::VariantInit(&vHr);
                    if (SUCCEEDED(AutoWrap(DISPATCH_PROPERTYGET, &vHr,
                            vRule.pdispVal, L"HResult", 0, nullptr)))
                        pData->nUnHr = vHr.lVal;
                    ::VariantClear(&vRc);
                    ::VariantClear(&vHr);
                    ::VariantClear(&vRule);
                }
                ::VariantClear(&vPut);
                ::VariantClear(&vIdx);
                ::VariantClear(&vColl);
            }
            pInstaller->Release();
        }
        else
        {
            pData->nErr = E_NOINTERFACE;
        }
        pTarget->Release();
    }
}
void AppxCapUpdService::StartUpdateUninstallThread(HWND hNotify, const CDuiString& sId, int nGen)
{
    struct UnCtx { HWND hNotify; CDuiString sId; int nGen; };
    UnCtx* ctx = std::make_unique<UnCtx>(UnCtx{ hNotify, sId, nGen }).release();   // [C++17 R3] ownership transferred to worker thread
    HANDLE h = SafeCreateThread(
        [](void* pv) -> DWORD {
            std::unique_ptr<UnCtx> guard(static_cast<UnCtx*>(pv));   // [C++17 R3] thread owns ctx, auto-freed at end
            HWND s = guard->hNotify;
            CDuiString sId = guard->sId;
            const int nGenLocal = guard->nGen;
            auto msg = std::make_unique<UpdDataMsg>();   // [C++17 R3] ownership transferred to UI on successful Post; auto-reclaimed on failure
            UpdDataMsg* pData = msg.get();
            pData->nKind = 1;   // [R126 重建] 1 = 卸载结果
            pData->nGen = nGenLocal;   // [R3-FIX] generation 守卫
            HRESULT hrCo = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            bool bComInit = SUCCEEDED(hrCo);
            IDispatch* pSession = nullptr;
            CLSID clsid = { 0 };
            if (SUCCEEDED(::CLSIDFromProgID(L"Microsoft.Update.Session", &clsid)))
                ::CoCreateInstance(clsid, nullptr, CLSCTX_ALL, IID_IDispatch,
                                   (void**)&pSession);
            if (pSession)
            {
                // 1) Re-search installed updates, match target IUpdate by UpdateID
                IDispatch* pTarget = UaFindTargetUpdate(pSession, sId);
                // [R126 重建] 2) 建 Installer，装入目标，更新后 Uninstall
                if (pTarget)
                    UaRunUpdateUninstall(pSession, pTarget, pData);
                else
                {
                    pData->nErr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
                }
                pSession->Release();
            }
            else
            {
                pData->nErr = E_FAIL;
            }
            if (bComInit) ::CoUninitialize();
            // [加固] 窗口失效/投递失败时回收，杜绝 UpdDataMsg 泄漏
            if (::IsWindow(s) && ::PostMessageW(s, WM_UPD_DATA, 0, reinterpret_cast<LPARAM>(msg.get())))
                msg.release();   // [C++17 R3] ownership transferred to UI; otherwise auto-reclaimed at scope exit
            return 0;
        },
        ctx);
    if (h) ::CloseHandle(h);   // [C++17 阶段2] SafeCreateThread 分离线程句柄：单点关闭（文件头约定）
    else
    {
        // [R126] 同上：线程未接管，回收 ctx 并回传卸载失败结果
        if (::IsWindow(hNotify))
        {
            auto failMsg = std::make_unique<UpdDataMsg>();   // [C++17 R3] auto-reclaimed on Post failure
            failMsg->nKind = 1;   // 1 = 卸载结果
            failMsg->nErr = static_cast<int>(E_OUTOFMEMORY);
            failMsg->nGen = nGen;   // [R3-FIX] generation 守卫
            if (::PostMessageW(hNotify, WM_UPD_DATA, 0, reinterpret_cast<LPARAM>(failMsg.get())))
                failMsg.release();   // [C++17 R3] ownership transferred to UI
        }
        std::unique_ptr<UnCtx> reclaim(ctx);   // [C++17 R3] thread creation failed, ctx not transferred
    }
}
// ============================================================
// [R86 P1-18] WUA 策略设置：暂停/推迟自动更新
// 写入 HKCU\Software\Microsoft\Windows\CurrentVersion\WindowsUpdate
// 键名与 Windows 更新策略组一致（PauseUpdatesExpiryTime /
// DeferFeatureUpdates / TargetReleaseVersion）。
// 不依赖 UI；失败返回非 0。
// ============================================================
namespace {
// WUA 策略根路径
const wchar_t* kWuaPolicyPath =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate";
// 把 SYSTEMTIME 转成 WUA 需要的 ISO8601 UTC 字符串（yyyy-MM-ddTHH:mm:ssZ）
bool SystemTimeToIsoUtc(const SYSTEMTIME& st, CDuiString& out)
{
    SYSTEMTIME stUtc;
    ::SystemTimeToTzSpecificLocalTime(nullptr, (const SYSTEMTIME*)&st, &stUtc);
    out.Format(L"%04d-%02d-%02dT%02d:%02d:%02dZ",
               stUtc.wYear, stUtc.wMonth, stUtc.wDay,
               stUtc.wHour, stUtc.wMinute, stUtc.wSecond);
    return true;
}
// 写 DWORD 值
LONG WriteRegDword(const wchar_t* path, const wchar_t* name, DWORD value)
{
    RegKeyGuard hKey;
    LONG l = ::RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, nullptr,
                                REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
                                nullptr, hKey.out(), nullptr);
    if (l != ERROR_SUCCESS) return l;
    l = ::RegSetValueExW(hKey, name, 0, REG_DWORD,
                         (const BYTE*)&value, sizeof(value));
    return l;
}
// 写字符串值
LONG WriteRegStr(const wchar_t* path, const wchar_t* name, const wchar_t* value)
{
    RegKeyGuard hKey;
    LONG l = ::RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, nullptr,
                                REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
                                nullptr, hKey.out(), nullptr);
    if (l != ERROR_SUCCESS) return l;
    l = ::RegSetValueExW(hKey, name, 0, REG_SZ,
                         (const BYTE*)value,
                         (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
    return l;
}
// 读 DWORD
DWORD ReadRegDwordDefault(const wchar_t* path, const wchar_t* name, DWORD def)
{
    RegKeyGuard hKey;
    LONG l = ::RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, hKey.out());
    if (l != ERROR_SUCCESS) return def;
    DWORD value = def, cb = sizeof(value);
    l = ::RegQueryValueExW(hKey, name, nullptr, nullptr, (BYTE*)&value, &cb);
    return l == ERROR_SUCCESS ? value : def;
}
// 读字符串
bool ReadRegStr(const wchar_t* path, const wchar_t* name, CDuiString& out)
{
    RegKeyGuard hKey;
    LONG l = ::RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, hKey.out());
    if (l != ERROR_SUCCESS) return false;
    DWORD cb = 0;
    l = ::RegQueryValueExW(hKey, name, nullptr, nullptr, nullptr, &cb);
    std::vector<wchar_t> buf(cb / sizeof(wchar_t) + 1, 0);
    l = ::RegQueryValueExW(hKey, name, nullptr, nullptr, reinterpret_cast<BYTE*>(buf.data()), &cb);
    if (l != ERROR_SUCCESS) return false;
    out = buf.data();
    return !out.IsEmpty();
}
} // namespace
DWORD AppxCapUpdService::SetPauseUpdatePolicy(bool bPause, int nDays)
{
    if (!bPause)
    {
        // 取消暂停：删除 PauseUpdatesExpiryTime / PauseFeatureUpdatesStartTime
        RegKeyGuard hKey;
        LONG l = ::RegCreateKeyExW(HKEY_CURRENT_USER, kWuaPolicyPath, 0, nullptr,
                                    REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
                                    nullptr, hKey.out(), nullptr);
        if (l != ERROR_SUCCESS) return static_cast<DWORD>(l);
        // [R128-A] 删除失败则策略残留，会让"取消暂停"形同无效；本函数以
        //   DWORD 表达成败，故把首个失败码回传（ERROR_FILE_NOT_FOUND 视为已无该值）。
        LONG lD1 = ::RegDeleteValueW(hKey, L"PauseUpdatesExpiryTime");
        LONG lD2 = ::RegDeleteValueW(hKey, L"PauseFeatureUpdatesStartTime");
        if (lD1 != ERROR_SUCCESS && lD1 != ERROR_FILE_NOT_FOUND)
            return static_cast<DWORD>(lD1);
        if (lD2 != ERROR_SUCCESS && lD2 != ERROR_FILE_NOT_FOUND)
            return static_cast<DWORD>(lD2);
        return 0;
    }
    // 暂停：写 PauseUpdatesExpiryTime = 未来 nDays 天（UTC ISO）
    SYSTEMTIME st;
    ::GetSystemTime(&st);
    FILETIME ft;
    ::SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    // + nDays 天（24h）
    uli.QuadPart += static_cast<ULONGLONG>(nDays) * 24ULL * 3600ULL * 10000000ULL;
    ft.dwLowDateTime = uli.LowPart;
    ft.dwHighDateTime = uli.HighPart;
    SYSTEMTIME stOut;
    ::FileTimeToSystemTime(&ft, &stOut);
    CDuiString sExpiry;
    sExpiry.Format(L"%04d-%02d-%02dT%02d:%02d:%02dZ",
                   stOut.wYear, stOut.wMonth, stOut.wDay,
                   stOut.wHour, stOut.wMinute, stOut.wSecond);
    LONG l1 = WriteRegStr(kWuaPolicyPath, L"PauseUpdatesExpiryTime", sExpiry.GetData());
    LONG l2 = WriteRegDword(kWuaPolicyPath, L"PauseFeatureUpdatesStartTime", 1);
    return static_cast<DWORD>(l1 != ERROR_SUCCESS ? l1 : l2);
}
DWORD AppxCapUpdService::SetSkipUpdatePolicy(bool bSkip, const CDuiString& sVersion)
{
    if (!bSkip)
    {
        // 取消推迟
        LONG l1 = WriteRegDword(kWuaPolicyPath, L"DeferFeatureUpdates", 0);
        return static_cast<DWORD>(l1 != ERROR_SUCCESS ? l1 : 0);
    }
    // 推迟功能更新：DeferFeatureUpdates=1 + TargetReleaseVersion=指定版本
    LONG l1 = WriteRegDword(kWuaPolicyPath, L"DeferFeatureUpdates", 1);
    LONG l2 = ERROR_SUCCESS;
    if (!sVersion.IsEmpty())
        l2 = WriteRegStr(kWuaPolicyPath, L"TargetReleaseVersion", sVersion.GetData());
    else
        l2 = WriteRegStr(kWuaPolicyPath, L"TargetReleaseVersion", L"");
    return static_cast<DWORD>(l1 != ERROR_SUCCESS ? l1 : l2);
}
// ============================================================
// [R-B49] DisableInstallUpdate：彻底禁用 / 恢复系统更新
// 基线：原版可彻底关 WU 策略（清单维度 7 缺失 2「禁用系统更新」开关）。
// 实现：写 HKLM\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate\AU
//       NoAutoUpdate=1 + AUOptions=1（禁用自动更新），并停止 wuauserv；
//       恢复则删除 NoAutoUpdate、写 AUOptions=2（通知下载）、启动 wuauserv。
// 需要管理员权限；无权限时返回 ACCESS_DENIED 并留痕。
// ============================================================
namespace {
// HKLM 组策略写 DWORD（AU 策略在 HKLM 下，需 KEY_SET_VALUE）
LONG WriteHklmDword(const wchar_t* path, const wchar_t* name, DWORD value)
{
    RegKeyGuard hKey;
    LONG l = ::RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, nullptr,
                                REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
                                nullptr, hKey.out(), nullptr);
    if (l != ERROR_SUCCESS) return l;
    l = ::RegSetValueExW(hKey, name, 0, REG_DWORD,
                         (const BYTE*)&value, sizeof(value));
    return l;
}
LONG DeleteHklmValue(const wchar_t* path, const wchar_t* name)
{
    RegKeyGuard hKey;
    LONG l = ::RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, nullptr,
                                REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
                                nullptr, hKey.out(), nullptr);
    if (l != ERROR_SUCCESS) return l;
    l = ::RegDeleteValueW(hKey, name);
    // ERROR_FILE_NOT_FOUND 视为成功（幂等）
    if (l == ERROR_FILE_NOT_FOUND) l = ERROR_SUCCESS;
    return l;
}
// 停止/启动 wuauserv（SCM 原生 API，不调 sc.exe）
DWORD ControlWuauserv(bool bStart, DWORD dwTimeoutMs)
{
    SC_HANDLE hSCM = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hSCM) return ::GetLastError();
    SC_HANDLE hSvc = ::OpenServiceW(hSCM, L"wuauserv",
                                     SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!hSvc) { DWORD e = ::GetLastError(); ::CloseServiceHandle(hSCM); return e; }
    DWORD dwErr = ERROR_SUCCESS;
    if (bStart)
    {
        if (!::StartServiceW(hSvc, 0, nullptr))
        {
            dwErr = ::GetLastError();
            if (dwErr == ERROR_SERVICE_ALREADY_RUNNING) dwErr = ERROR_SUCCESS;
        }
    }
    else
    {
        SERVICE_STATUS ss = {};
        if (!::ControlService(hSvc, SERVICE_CONTROL_STOP, &ss))
        {
            dwErr = ::GetLastError();
            // 服务已停止或未运行视为成功
            if (dwErr == ERROR_SERVICE_NOT_ACTIVE) dwErr = ERROR_SUCCESS;
        }
        else if (dwTimeoutMs > 0)
        {
            // 等待 SERVICE_STOPPED
            DWORD dwStart = ::GetTickCount();
            while (::GetTickCount() - dwStart < dwTimeoutMs)
            {
                SERVICE_STATUS s2 = {};
                DWORD cb = sizeof(s2);
                if (!::QueryServiceStatus(hSvc, &s2)) { dwErr = ::GetLastError(); break; }
                if (s2.dwCurrentState == SERVICE_STOPPED) { dwErr = ERROR_SUCCESS; break; }
                ::Sleep(200);
            }
        }
    }
    ::CloseServiceHandle(hSvc);
    ::CloseServiceHandle(hSCM);
    return dwErr;
}
} // namespace
DWORD AppxCapUpdService::SetDisableUpdatePolicy(bool bDisable)
{
    // 组策略根
    const wchar_t* kAuPolicy = L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate\\AU";
    if (bDisable)
    {
        // 1) AU 策略：NoAutoUpdate=1 + AUOptions=1（永不检查）
        LONG l1 = WriteHklmDword(kAuPolicy, L"NoAutoUpdate", 1);
        LONG l2 = WriteHklmDword(kAuPolicy, L"AUOptions", 1);
        // 2) 停止 wuauserv
        DWORD dwSvc = ControlWuauserv(false, 3000);
        // 3) 返回：注册表优先，其次服务
        return static_cast<DWORD>((l1 != ERROR_SUCCESS) ? l1 : (l2 != ERROR_SUCCESS ? l2 : dwSvc));
    }
    // 恢复
    LONG l1 = DeleteHklmValue(kAuPolicy, L"NoAutoUpdate");
    LONG l2 = WriteHklmDword(kAuPolicy, L"AUOptions", 2);
    DWORD dwSvc = ControlWuauserv(true, 0);
    return static_cast<DWORD>((l1 != ERROR_SUCCESS) ? l1 : (l2 != ERROR_SUCCESS ? l2 : dwSvc));
}
bool AppxCapUpdService::IsUpdatePaused()
{
    CDuiString sExpiry;
    if (!ReadRegStr(kWuaPolicyPath, L"PauseUpdatesExpiryTime", sExpiry))
        return false;
    return !sExpiry.IsEmpty();
}
bool AppxCapUpdService::IsUpdateSkipped(CDuiString& sVersion)
{
    DWORD dwDefer = ReadRegDwordDefault(kWuaPolicyPath, L"DeferFeatureUpdates", 0);
    if (dwDefer == 0) { sVersion.Empty(); return false; }
    ReadRegStr(kWuaPolicyPath, L"TargetReleaseVersion", sVersion);
    return true;
}
// [按钮整合] 读取 HKLM AU 策略：NoAutoUpdate==1 视为已彻底禁用系统更新
bool AppxCapUpdService::IsUpdateDisabled()
{
    const wchar_t* kAuPolicy = L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate\\AU";
    RegKeyGuard hKey;
    LONG l = ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kAuPolicy, 0,
                             KEY_QUERY_VALUE | KEY_WOW64_64KEY, hKey.out());
    if (l != ERROR_SUCCESS) return false;
    DWORD dwValue = 0, cb = sizeof(dwValue), dwType = 0;
    l = ::RegQueryValueExW(hKey, L"NoAutoUpdate", nullptr, &dwType,
                           (BYTE*)&dwValue, &cb);
    return (l == ERROR_SUCCESS && dwType == REG_DWORD && dwValue == 1);
}
// ============================================================
// [R86 P1-17] wsusscn2 离线源开关
// ============================================================
namespace {
volatile LONG g_nOfflineSource = 0;   // 0=在线（默认），1=离线
}
void AppxCapUpdService::SetOfflineUpdateSource(bool bEnable)
{
    ::InterlockedExchange(&g_nOfflineSource, bEnable ? 1 : 0);

    // [T6] 自愈点②：切回在线源时注销持久化的扫描服务并清记录。
    // 这是 T6「不再每次枚举尾注销」的配套清理——否则扫描服务会作为
    // 孤儿永久留在系统 WUA 服务列表里。
    if (!bEnable) SvKillPersistedService();
}
bool AppxCapUpdService::IsOfflineSourceEnabled()
{
    return ::InterlockedCompareExchange(&g_nOfflineSource, 0, 0) != 0;
}

// [T2 P0] 能力变更后立即删除 Cap 缓存（对齐 InvalidateUpdCache 语义：
// TTL 延长只是兜底，正确性由写后主动失效保证）
void AppxCapUpdService::InvalidateCapCache()
{
    std::wstring sPath;
    // [R119] 同上：缓存失效动作，删不掉不影响正确性 —— 读路径带 TTL 校验兜底。
    if (CapCachePath(sPath))
        ::DeleteFileW(sPath.c_str());
}


// ============================================================
// [R79 P1] 写后精准失效：按 Identity 单行更新 Capability 缓存状态
//
// 问题（R78 遗留）：卸载/添加成功后直接 InvalidateCapCache() 删掉整个缓存文件，
// 下次进页必然全量重扫。而用户实际只改了 1 项。
//
// 做法：读现有缓存 → 只把命中 sIdentity 的行的 state 字段改掉 → 刷新首行时间戳
// → 原子写回。文件格式完全不变（仍 CAPCACHE\t<epoch> + 每行 8 字段），
// 因此不需要版本迁移，LoadCapCache 与种子缓存都不用改。
//
// 返回 false 的两种情况（调用方据此回退到 InvalidateCapCache）：
//   · 缓存不存在/格式坏 —— 没有可更新的基底
//   · 一个目标 Identity 都没命中 —— 缓存已不能反映真实状态
// ============================================================
bool AppxCapUpdService::UpdateCapCacheStates(const std::vector<CapEntry>& vChanged,
                                              const std::wstring& sPathOverride)
{
    if (vChanged.empty()) return false;

    std::wstring sPath = sPathOverride;
    if (sPath.empty() && !CapCachePath(sPath)) return false;
    ::DeleteFileW((sPath + L".tmp").c_str());

    std::string sBody;
    {
        FileGuard hFile = ::CreateFileW(sPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return false;
        DWORD dwSize = ::GetFileSize(hFile, nullptr);
        if (dwSize == 0 || dwSize == INVALID_FILE_SIZE || dwSize > 4u * 1024u * 1024u)
            return false;
        sBody.resize(dwSize);
        DWORD dwRead = 0;
        if (!::ReadFile(hFile, &sBody[0], dwSize, &dwRead, nullptr) || dwRead != dwSize)
            return false;
    }

    // 「Identity(转义后) → 新 state(转义后)」索引
    std::map<std::string, std::string> mapWant;
    for (size_t i = 0; i < vChanged.size(); ++i)
    {
        if (vChanged[i].sIdentity.IsEmpty()) continue;
        std::string a, b;
        CapCacheEscape(vChanged[i].sIdentity, a);
        CapCacheEscape(vChanged[i].sState, b);
        if (!a.empty()) mapWant[a] = b;
    }
    if (mapWant.empty()) return false;

    const size_t nHeadEnd = sBody.find("\r\n");
    if (nHeadEnd == std::string::npos) return false;
    if (sBody.compare(0, 9, "CAPCACHE\t") != 0) return false;

    std::string sOut;
    sOut.reserve(sBody.size() + 32);
    {
        char szEpoch[48] = { 0 };
        std::snprintf(szEpoch, sizeof(szEpoch), "CAPCACHE\t%llu\r\n",
                      (unsigned long long)::time(nullptr));
        sOut += szEpoch;
    }

    size_t nMatched = 0;
    size_t nPos = nHeadEnd + 2;
    while (nPos < sBody.size())
    {
        size_t nEol = sBody.find("\r\n", nPos);
        if (nEol == std::string::npos) nEol = sBody.size();
        const std::string sLine = sBody.substr(nPos, nEol - nPos);

        // 字段：identity \t state \t desc \t size \t reqDeps \t optDeps \t reqDep \t optDep
        const size_t t1 = sLine.find('\t');
        const size_t t2 = (t1 == std::string::npos) ? std::string::npos
                                                   : sLine.find('\t', t1 + 1);
        if (t1 != std::string::npos && t1 > 0 && t2 != std::string::npos)
        {
            const std::string sKey = sLine.substr(0, t1);
            std::map<std::string, std::string>::const_iterator it = mapWant.find(sKey);
            if (it != mapWant.end())
            {
                // 命中：只替换第 2 字段（state），其余字段原样拼接
                sOut += sKey;
                sOut += '\t';
                sOut += it->second;
                sOut += sLine.substr(t2);   // 从 t2 起（含分隔符）原样保留
                sOut += "\r\n";
                ++nMatched;
                nPos = (nEol >= sBody.size()) ? sBody.size() : nEol + 2;
                continue;
            }
        }
        sOut.append(sLine);
        sOut += "\r\n";
        nPos = (nEol >= sBody.size()) ? sBody.size() : nEol + 2;
    }

    if (nMatched == 0) return false;

    const std::wstring sTmp = sPath + L".tmp";
    {
        FileGuard hTmp = ::CreateFileW(sTmp.c_str(), GENERIC_WRITE, 0, nullptr,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hTmp == INVALID_HANDLE_VALUE) return false;
        DWORD dwWritten = 0;
        if (!::WriteFile(hTmp, sOut.data(), static_cast<DWORD>(sOut.size()), &dwWritten, nullptr)
            || dwWritten != static_cast<DWORD>(sOut.size()))
            return false;
    }
    return ::MoveFileExW(sTmp.c_str(), sPath.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

// ============================================================
// [T6] 离线扫描服务持久复用（消除根因 R6）
//
// 问题：每次枚举都 AddScanPackageService(wsusscn3.cab) 注册、枚举尾 RemoveService
// 注销。100MB+ 的 cab 每次重新解析，单项可达数十秒。
//
// 方案：注册成功后把 ServiceID + cab 指纹写入 scan_svc.dat；下次枚举先校验
// （cab 未变 + 服务仍存活）则直接复用，跳过注册。注销时机改为：
//   * 切回在线源（SetOfflineUpdateSource(false)）
//   * cab 指纹变更（先注销旧服务再注册新的）
//   * 记录里的服务已失效（下次注册前清理记录）
//
// 三重自愈缺一不可——只做持久化不做清理会在系统 WUA 服务列表留孤儿扫描源。
// ============================================================

namespace {

// 记录文件路径：%LOCALAPPDATA%\DismTool_Clone\scan_svc.dat
bool SvRecordPath(std::wstring& sPath)
{
    std::wstring sDir = winenv::SafeGetEnv(L"LOCALAPPDATA");
    if (sDir.empty()) return false;
    sDir += L"\\DismTool_Clone";
    ::CreateDirectoryW(sDir.c_str(), nullptr);   // 已存在时失败可忽略
    sPath = sDir + L"\\scan_svc.dat";
    return true;
}

// 把制表/换行替换为空格，避免破坏行结构
std::wstring SvFlatten(const std::wstring& sIn)
{
    std::wstring s = sIn;
    for (wchar_t& ch : s)
        if (ch == L'\t' || ch == L'\r' || ch == L'\n') ch = L' ';
    return s;
}

} // namespace

// cab 指纹：路径 + 文件大小 + 最后写入时间。任一变化即视为换了版本，
// 旧服务必须注销后重注册（否则新 cab 的扫描语义不会生效）。
static std::wstring SvCabStamp(const wchar_t* pszCab)
{
    if (pszCab == nullptr || *pszCab == L'\0') return std::wstring();

    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (!::GetFileAttributesExW(pszCab, GetFileExInfoStandard, &fad))
        return std::wstring();

    const unsigned long long ullSize =
        (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    const unsigned long long ullTime =
        (static_cast<unsigned long long>(fad.ftLastWriteTime.dwHighDateTime) << 32)
        | fad.ftLastWriteTime.dwLowDateTime;

    std::wstring sOut(pszCab);
    sOut += L'\t';
    sOut += std::to_wstring(ullSize);
    sOut += L'\t';
    sOut += std::to_wstring(ullTime);
    return SvFlatten(sOut);
}

// 读取记录。bValid 表示记录完整可用；返回 false 表示文件不存在/损坏。
static bool SvLoadRecord(std::wstring& sSid, std::wstring& sCab, bool& bValid)
{
    sSid.clear();
    sCab.clear();
    bValid = false;

    std::wstring sPath;
    if (!SvRecordPath(sPath)) return false;

    FileGuard hFile = ::CreateFileW(sPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    DWORD dwSize = ::GetFileSize(hFile, nullptr);
    if (dwSize == 0 || dwSize == INVALID_FILE_SIZE || dwSize > 64u * 1024u) return false;

    std::string sRaw(dwSize, '\0');
    DWORD dwRead = 0;
    if (!::ReadFile(hFile, &sRaw[0], dwSize, &dwRead, nullptr) || dwRead != dwSize)
        return false;

    // UTF-8 → UTF-16（复用 Rust dismcore 纯计算模块）
    const auto units = dismcore::Utf8ToUtf16(sRaw.data(), sRaw.size());
    std::wstring sBody(units.begin(), units.end());

    // 逐行切分：line0=magic, line1=ServiceID, line2=cabStamp
    std::vector<std::wstring> vLine;
    size_t nPos = 0;
    while (vLine.size() < 3)
    {
        size_t nEol = sBody.find(L"\r\n", nPos);
        if (nEol == std::wstring::npos)
        {
            vLine.push_back(sBody.substr(nPos));
            break;
        }
        vLine.push_back(sBody.substr(nPos, nEol - nPos));
        nPos = nEol + 2;
    }
    if (vLine.size() < 3) return false;
    if (vLine[0] != L"SCANSVC\t1") return false;
    if (vLine[1].empty() || vLine[2].empty()) return false;

    sSid = vLine[1];
    sCab = vLine[2];
    bValid = true;
    return true;
}

// 原子写入：先写 .tmp 再替换，避免半写坏档导致下次启动读到不完整记录。
static void SvSaveRecord(const std::wstring& sSid, const std::wstring& sCab)
{
    std::wstring sPath;
    if (!SvRecordPath(sPath)) return;
    if (sSid.empty() || sCab.empty()) return;

    std::wstring sBody = L"SCANSVC\t1\r\n";
    sBody += SvFlatten(sSid);
    sBody += L"\r\n";
    sBody += SvFlatten(sCab);
    sBody += L"\r\n";

    const auto bytes = dismcore::Utf16ToUtf8(sBody.data(), sBody.size());
    std::string sRaw(bytes.begin(), bytes.end());

    const std::wstring sTmp = sPath + L".tmp";
    {
        FileGuard hFile = ::CreateFileW(sTmp.c_str(), GENERIC_WRITE, 0, nullptr,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return;
        DWORD dwWritten = 0;
        const BOOL bOk = ::WriteFile(hFile, sRaw.data(), static_cast<DWORD>(sRaw.size()),
                                     &dwWritten, nullptr);
        if (!bOk || dwWritten != static_cast<DWORD>(sRaw.size())) return;
    }
    // [R128-B] 记录是一次性快照，替换失败则旧记录残留，留痕
    if (!::MoveFileExW(sTmp.c_str(), sPath.c_str(), MOVEFILE_REPLACE_EXISTING))
        ::OutputDebugStringW(L"[R128][appx] 服务记录替换失败\r\n");
}

static void SvClearRecord()
{
    std::wstring sPath;
    if (!SvRecordPath(sPath)) return;
    // [R119] 删除的是服务状态记录文件，删失败等同于"保留上次状态"，无害。
    ::DeleteFileW(sPath.c_str());
}

// 校验记录里的 ServiceID 是否仍存在于本机 WUA 服务列表。
// IUpdateServiceManager::QueryService(serviceID) 成功即视为存活。
static bool SvCachedServiceAlive(IDispatch* pSvcMgr, const std::wstring& sSid)
{
    if (pSvcMgr == nullptr || sSid.empty()) return false;

    VARIANT vSid;
    ::VariantInit(&vSid);
    vSid.vt = VT_BSTR;
    vSid.bstrVal = ::SysAllocString(sSid.c_str());
    if (vSid.bstrVal == nullptr) return false;

    VARIANT vRet;
    ::VariantInit(&vRet);
    const HRESULT hr = AutoWrap(DISPATCH_METHOD, &vRet, pSvcMgr, L"QueryService", 1, &vSid);
    bool bAlive = SUCCEEDED(hr) && vRet.pdispVal != nullptr;

    if (vRet.pdispVal != nullptr) vRet.pdispVal->Release();
    ::VariantClear(&vRet);
    ::VariantClear(&vSid);
    return bAlive;
}

// 注销指定扫描包服务（切回在线源 / cab 变更 / 记录失效时调用）
static void UeKillScanService(IDispatch* pSvcMgr, BSTR bstrSid)
{
    if (pSvcMgr == nullptr || bstrSid == nullptr) return;

    VARIANT vRmv[2];
    VariantInit(&vRmv[0]);
    vRmv[0].vt = VT_BSTR; vRmv[0].bstrVal = bstrSid;
    VariantInit(&vRmv[1]);
    vRmv[1].vt = VT_I4; vRmv[1].lVal = 1;
    VARIANT vR; VariantInit(&vR);
    const HRESULT hrRmv = AutoWrap(DISPATCH_METHOD, &vR, pSvcMgr, L"RemoveService", 2, vRmv);
    UeDiag("RemoveScanService", hrRmv, 0);
    VariantClear(&vR);
    VariantClear(&vRmv[0]);
    VariantClear(&vRmv[1]);
}

// 复用入口：记录有效 + cab 指纹一致 + 服务仍存活 时直接接管，跳过 cab 注册。
// 返回 true 表示已复用（调用方应立即 return，不再走注册流程）。
// 任一条件不满足即返回 false，并顺手清理失效记录（自愈点①）。
static bool UeTryReuseScanService(IDispatch* pSearcher, const wchar_t* pszCab,
                                  IDispatch** ppSvcMgr, BSTR* ppScanSid)
{
    std::wstring sSid, sCab;
    bool bValid = false;
    if (!SvLoadRecord(sSid, sCab, bValid) || !bValid) return false;

    // 建 ServiceManager（cab 变更与存活校验都要用它）
    CLSID clsidSvc = { 0 };
    if (FAILED(::CLSIDFromProgID(L"Microsoft.Update.ServiceManager", &clsidSvc)))
        return false;

    IDispatch* pSvcMgr = nullptr;
    if (FAILED(::CoCreateInstance(clsidSvc, nullptr, CLSCTX_ALL,
                                  IID_IDispatch, (void**)&pSvcMgr)) || pSvcMgr == nullptr)
        return false;

    // 自愈点③：cab 指纹不一致说明换了版本文件。必须先注销旧服务再清记录，
    // 否则旧扫描服务会作为孤儿留在系统 WUA 服务列表里。
    const std::wstring sNow = SvCabStamp(pszCab);
    if (sNow.empty() || sNow != sCab)
    {
        BSTR bstrOld = ::SysAllocString(sSid.c_str());
        if (bstrOld != nullptr)
        {
            UeKillScanService(pSvcMgr, bstrOld);
            ::SysFreeString(bstrOld);
        }
        pSvcMgr->Release();
        SvClearRecord();
        return false;
    }

    if (!SvCachedServiceAlive(pSvcMgr, sSid))
    {
        // 自愈点①：记录里的服务已不存在（可能被系统清理或用户手动移除），
        // 清掉记录让本次走完整注册。此时服务已不在，无需注销。
        pSvcMgr->Release();
        SvClearRecord();
        return false;
    }

    // 复用：把 ServiceID 交回调用方（BSTR 所有权转移，由调用方 SysFreeString）
    *ppScanSid = ::SysAllocString(sSid.c_str());
    if (*ppScanSid == nullptr)
    {
        pSvcMgr->Release();
        return false;
    }
    *ppSvcMgr = pSvcMgr;

    // 设置 searcher 的 ServiceID（与注册路径同一收尾动作）
    VARIANT vSid;
    ::VariantInit(&vSid);
    vSid.vt = VT_BSTR;
    vSid.bstrVal = ::SysAllocString(*ppScanSid);
    if (vSid.bstrVal != nullptr)
        AutoWrap(DISPATCH_PROPERTYPUT, &vSid, pSearcher, L"ServiceID", 1, &vSid);
    ::VariantClear(&vSid);

    SetScanSidReused(true);
    UeDiag("ReuseScanService", S_OK, 1);
    return true;
}

// [T6] 自愈点②：注销持久化的扫描服务并清记录。
// 触发时机：用户切回在线源（SetOfflineUpdateSource(false)）。
// 只读记录、不依赖枚举上下文，因此可安全在任何线程调用。
static void SvKillPersistedService()
{
    std::wstring sSid, sCab;
    bool bValid = false;
    if (!SvLoadRecord(sSid, sCab, bValid) || !bValid)
    {
        // 无记录仍清一次，防止遗留半写文件
        SvClearRecord();
        return;
    }

    CLSID clsidSvc = { 0 };
    if (SUCCEEDED(::CLSIDFromProgID(L"Microsoft.Update.ServiceManager", &clsidSvc)))
    {
        IDispatch* pSvcMgr = nullptr;
        if (SUCCEEDED(::CoCreateInstance(clsidSvc, nullptr, CLSCTX_ALL,
                                          IID_IDispatch, (void**)&pSvcMgr)) && pSvcMgr != nullptr)
        {
            BSTR bstrSid = ::SysAllocString(sSid.c_str());
            if (bstrSid != nullptr)
            {
                UeKillScanService(pSvcMgr, bstrSid);
                ::SysFreeString(bstrSid);
            }
            pSvcMgr->Release();
        }
    }

    SvClearRecord();
    SetScanSidReused(false);
}

// [REFACTOR] 从 StartAppxEnumThread 提取：解析包全名 Family_Version_Arch_PublisherID。
// 逐字搬迁零逻辑改动，仅把深层嵌套块提出为独立函数（消除 3 层嵌套）。
// 原注释含编码损坏，一并修正为可读中文。
static void ParseAppxFullName(const wchar_t* szKey, AppxPkg& pkg)
{
    CDuiString sVersion, sPublisher;
    CDuiString sCur;
    int nPart = 0;
    for (int i = 0; szKey[i] != 0; i++)
    {
        wchar_t c = szKey[i];
        if (c == L'_')
        {
            if (nPart == 1) sVersion = sCur;
            if (nPart == 3) sPublisher = sCur;
            sCur.Empty();
            nPart++;
            if (nPart > 4) break;
        }
        else
        {
            sCur += c;
        }
    }
    // 处理最后一段（若末尾无 _）
    if (nPart == 1 && !sCur.IsEmpty()) sVersion = sCur;
    else if (nPart == 3 && !sCur.IsEmpty()) sPublisher = sCur;
    if (!sVersion.IsEmpty())   pkg.sVersion = sVersion;
    if (!sPublisher.IsEmpty()) pkg.sPublisher = sPublisher;
}

// [P4] 种子缓存加载：exe\seed\seed_cap.dat
//  首行 "SEED\t<OSBUILD>|<SKU>|<LANG>"，指纹不匹配直接返回 false。
//  其后与磁盘缓存同构：首行 "CAPCACHE\t<epoch>"，再逐行 8 字段 TSV。
//  无 TTL 校验 —— 出厂数据不随时间失效，但受指纹约束。
bool AppxCapUpdService::LoadSeedCapCache(std::vector<CapEntry>& vOut,
                                           const std::wstring& sPathOverride)
{
    vOut.clear();

    // [R60] sPathOverride 非空时直接用它，不再探 SeedDir()。
    //   目的：让自检能构造坏种子文件验证拒绝路径；
    //   业务调用不传参数，行为不变。
    std::wstring sPath;
    if (!sPathOverride.empty())
    {
        sPath = sPathOverride;
    }
    else
    {
        const std::wstring sDir = seedcache::SeedDir();
        if (sDir.empty()) return false;
        sPath = sDir + L"\\seed_cap.dat";
    }

    FileGuard hFile = ::CreateFileW(sPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    const DWORD dwSize = ::GetFileSize(hFile, nullptr);
    // 种子最坏 ~40KB，4MB 上限纯属防御损坏文件
    if (dwSize == 0 || dwSize == INVALID_FILE_SIZE || dwSize > 4u * 1024u * 1024u)
        return false;

    std::string sBody(dwSize, '\0');
    DWORD dwRead = 0;
    if (!::ReadFile(hFile, &sBody[0], dwSize, &dwRead, nullptr) || dwRead != dwSize)
        return false;

    size_t pos = 0;
    auto nextLine = [&sBody, &pos](void) -> std::string
    {
        const size_t e = sBody.find("\r\n", pos);
        std::string s;
        if (e == std::string::npos) { s = sBody.substr(pos); pos = sBody.size(); }
        else { s = sBody.substr(pos, e - pos); pos = e + 2; }
        return s;
    };

    // ---- 第 1 行：指纹 ----
    const std::string sSeedHead = nextLine();
    if (sSeedHead.size() <= 5 || sSeedHead.compare(0, 5, "SEED\t") != 0) return false;
    if (!seedcache::MatchesFingerprint(sSeedHead.substr(5)))
    {
        // 指纹不符：种子属于别的 OS build / SKU / 语言，禁止渲染
        ::OutputDebugStringW(L"[seed] cap seed fingerprint mismatch, ignored\r\n");
        return false;
    }

    // ---- 第 2 行：与磁盘缓存同构的数据头（仅校验存在，不做 TTL） ----
    const std::string sDataHead = nextLine();
    if (sDataHead.compare(0, 9, "CAPCACHE\t") != 0) return false;

    // ---- 其余行：8 字段条目 ----
    while (pos < sBody.size())
    {
        const std::string sLine = nextLine();
        if (sLine.empty()) continue;
        std::string f[8];
        size_t p = 0;
        int n = 0;
        for (; n < 7; ++n)
        {
            const size_t t = sLine.find('\t', p);
            if (t == std::string::npos) break;
            f[n] = sLine.substr(p, t - p);
            p = t + 1;
        }
        if (n < 7) continue;                 // 字段不足：跳过（格式容错）
        f[7] = sLine.substr(p);

        CapEntry e;
        e.sIdentity = CapCacheUnescape(f[0]);
        if (e.sIdentity.IsEmpty()) continue;
        e.sState = CapCacheUnescape(f[1]);
        e.sDescription = CapCacheUnescape(f[2]);
        e.sSize = CapCacheUnescape(f[3]);
        // 数值字段：损坏数据不应抛异常中断整个列表（std::stoi 会 throw）
        try
        {
            e.nRequiredDeps = static_cast<int>(std::stoi(f[4]));
            e.nOptionalDeps = static_cast<int>(std::stoi(f[5]));
        }
        catch (...) { continue; }
        e.sFirstRequiredDep = CapCacheUnescape(f[6]);
        e.sFirstOptionalDep = CapCacheUnescape(f[7]);
        vOut.push_back(e);
    }

    if (!vOut.empty())
        ::OutputDebugStringW(L"[seed] cap seed loaded (fingerprint matched)\r\n");
    return !vOut.empty();
}

// [P1] 把枚举结果分批 Post 给 UI，让页面逐批追加渲染，首屏不必等整表。
// DismGetCapabilities 是一次性返回全部数组的同步 API（无逐项回调），
// 因此"流式"只能在数据到手后做：每 kPartial 条发一条 WM_CAP_PARTIAL，
// 末批 bFinal=true 携带 nExitCode 通知页面收尾。
// 同时把全部条目写入 pData->vCaps（完整列表，供缓存落盘与整表重建）。
// 放在文件尾而非 StartCapEnumThread 内，避免推后该函数之后的行级白名单锚点。
static void PostCapPartials(HWND s, const std::vector<DismApiBridge::CapabilityInfo>& vApi,
                            CapDataMsg& data, int nGen)
{
    constexpr size_t kPartial = 100;
    std::vector<CapEntry> vBatch;
    vBatch.reserve(kPartial);

    // [PROGRESS 2026-10-25] 带上总数与累计已发送数，页面据此显示真实百分比。
    //   总条数在枚举返回时就已确定（vApi.size()），分批只是发送节奏；
    //   旧实现把这个已知量丢掉了，页面只能显示"无限转圈"，无法表达"到哪了"。
    const int nGrandTotal = static_cast<int>(vApi.size());
    int nSentSoFar = 0;

    auto flush = [&s, &vBatch, kPartial, nGrandTotal, &nSentSoFar, nGen](bool bFinal)
    {
        auto part = std::make_unique<CapPartialMsg>();
        nSentSoFar += static_cast<int>(vBatch.size());
        part->nTotal = nGrandTotal;
        part->nSent = nSentSoFar;
        part->vBatch.swap(vBatch);
        part->bFinal = bFinal;
        part->nGen = nGen;   // [STAB] stamp each partial with the enumeration generation
        if (::IsWindow(s) && ::PostMessageW(s, WM_CAP_PARTIAL, 0,
                reinterpret_cast<LPARAM>(part.get())))
            part.release();
        vBatch.reserve(kPartial);
    };

    for (size_t i = 0; i < vApi.size(); ++i)
    {
        CapEntry e;
        e.sIdentity = vApi[i].sName.c_str();
        // Page_Capability 以 sState == "Installed" 判定已安装
        e.sState = vApi[i].bInstalled ? L"Installed" : L"Not Present";
        data.vCaps.push_back(e);
        vBatch.push_back(e);
        if (vBatch.size() >= kPartial) flush(false);
    }
    flush(true);   // 尾批（可能为空，但必须发，用于通知"数据结束"）
}

// ---------------------------------------------------------------------------
// [P1] 更新枚举分批上报（实现放文件尾，避免推后行级白名单锚点）
//
// WUA 本身按 Item 逐条取，天然具备流式特性：每攒够 kUpdPartial 条就 Post
// 一批给 UI，页面可立即显示已到部分，不必等整轮 IUpdateCollection 遍历完成。
// 缓冲按 Tab 分开（0=待安装 / 1=已安装），因为两个 Tab 可能并发枚举。
// ---------------------------------------------------------------------------
inline constexpr size_t kUpdPartial = 100;

struct UpdBatchBuf
{
    std::vector<UpdEntry> v[2];
};

UpdBatchBuf& CurUpdBatch()
{
    // 工作线程单一线程内使用，UI 线程不触碰，故无需加锁；
    // 两个 Tab 的枚举线程各自持有自己的元素。
    static thread_local UpdBatchBuf s_buf;
    return s_buf;
}

void PostOneUpd(HWND hNotify, int nTab, int nGen, const UpdEntry& e)
{
    if (nTab != 0 && nTab != 1) nTab = 1;
    std::vector<UpdEntry>& v = CurUpdBatch().v[nTab];
    v.push_back(e);
    if (v.size() < kUpdPartial) return;

    auto part = std::make_unique<UpdPartialMsg>();
    part->vBatch.swap(v);
    part->bFinal = false;
    part->nTab = nTab;
    part->nGen = nGen;   // [R3-FIX] generation 守卫
    if (::IsWindow(hNotify) &&
        ::PostMessageW(hNotify, WM_UPD_PARTIAL, 0, reinterpret_cast<LPARAM>(part.get())))
        part.release();
    v.reserve(kUpdPartial);
}

void FlushUpdPartial(HWND hNotify, int nTab, int nGen)
{
    if (nTab != 0 && nTab != 1) nTab = 1;
    auto part = std::make_unique<UpdPartialMsg>();
    part->vBatch.swap(CurUpdBatch().v[nTab]);
    part->bFinal = true;
    part->nTab = nTab;
    part->nGen = nGen;   // [R3-FIX] generation 守卫
    if (::IsWindow(hNotify) &&
        ::PostMessageW(hNotify, WM_UPD_PARTIAL, 0, reinterpret_cast<LPARAM>(part.get())))
        part.release();
}
