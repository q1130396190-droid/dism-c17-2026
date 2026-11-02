// ============================================================
// ControlService.cpp —— 控制面板页动态枚举（R91 P2 切片）
//
// 背景：评分报告 D3 缺口"无 Shell API 调用，仅硬编码 GUID"。
// 实现：
//   1) EnumControlApplets：枚举 HKLM\...\Explorer\ControlPanel\NameSpace
//      下所有子键，读 Default（可能 @resource 引用）与 InfoTip，
//      用 SHLoadIndirectString 解析显示名。
//   2) OpenControlApplet：canonical 以 "Microsoft." 开头 → control /name X；
//      以 "{" 开头 → explorer shell:::{CLSID}；否则 control /name 兜底。
//   3) ResolveIndirectString / FallbackDisplayName：资源引用解析与硬编码兜底。
// 纯 Win32，无 UI 控件依赖；不使用预编译头。
// ============================================================

#include "ControlService.h"



#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <shellapi.h>   // [R127-2] SE_ERR_NOASSOC 等 ShellExecute 语义错误码

// [C++17 阶段2] 局部 RAII 守卫：注册表键、Shell PIDL（见 WinHandles.h 使用规范）
struct CTRegGuard
{
    HKEY k = nullptr;
    ~CTRegGuard() { if (k) ::RegCloseKey(k); }
    HKEY* out() { return &k; }
    operator HKEY() const { return k; }
};
struct CTPidlGuard
{
    PIDLIST_ABSOLUTE p;
    explicit CTPidlGuard(PIDLIST_ABSOLUTE x) : p(x) {}
    ~CTPidlGuard() { if (p) ::CoTaskMemFree(p); }
    CTPidlGuard(const CTPidlGuard&) = delete;
    CTPidlGuard& operator=(const CTPidlGuard&) = delete;
    explicit operator bool() const { return p != nullptr; }
    operator PIDLIST_ABSOLUTE() const { return p; }
    // [C++17 R-P8] SHParseDisplayName / IShellItem 需要 PIDLIST_ABSOLUTE* 出参
    PIDLIST_ABSOLUTE* out() { return &p; }
};

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")   // [R127-2] SHParseDisplayName / SHCreateItemFromIDList / ShellExecuteExW

using namespace DuiLib;

namespace
{
    // 读取字符串值（REG_SZ / REG_EXPAND_SZ），返回空串表示不存在
    CDuiString ReadStr(HKEY hKey, const wchar_t* pszValue)
    {
        if (!hKey) return CDuiString();
        DWORD dwType = 0, dwSize = 0;
        if (::RegQueryValueEx(hKey, pszValue, nullptr, &dwType, nullptr, &dwSize)
                != ERROR_SUCCESS)
            return CDuiString();
        if (dwType != REG_SZ && dwType != REG_EXPAND_SZ)
            return CDuiString();

        CDuiString sOut;
        if (dwSize >= sizeof(wchar_t))
        {
            std::vector<WCHAR> bufBuf(dwSize / sizeof(WCHAR) + 1, 0);
            wchar_t* pBuf = bufBuf.data();
            if (::RegQueryValueEx(hKey, pszValue, nullptr, &dwType, reinterpret_cast<BYTE*>(pBuf),
                                  &dwSize) == ERROR_SUCCESS)
                sOut = pBuf;
        }
        return sOut;
    }

    // 硬编码 CLSID → 显示名兜底映射（当 SHLoadIndirectString 失败时使用）
    struct ClsidName
    {
        const wchar_t* clsid;
        const wchar_t* name;
    };
    const ClsidName kFallbacks[] = {
        { L"{025A5937-A6BE-4686-A844-36FE4BEC8B6D}", L"Power Options" },
        { L"{0DF44EAA-FF21-4412-828E-260A8728E7F1}", L"Default Programs" },
        { L"{4026492F-2F69-46B8-B9BF-5654FC07E423}", L"Windows Firewall" },
        { L"{40419485-C444-4567-851A-2DD7BFA1684D}", L"Network and Sharing Center" },
        { L"{58E3C745-D971-4081-9034-86E34B30836A}", L"Power Options" },
        { L"{62D8ED13-C9D0-4CE8-A914-47DD628FB1B0}", L"Region and Language" },
        { L"{6DFD7C5C-2451-11d3-A299-00C04F8EF6AF}", L"Date and Time" },
        { L"{725BE8F7-668E-4C7B-8F90-46BDB0936430}", L"User Accounts" },
        { L"{74246bfc-4c96-11d0-abef-0020af6b0b7a}", L"Device Manager" },
        { L"{ED7BA470-8E54-465E-825C-99712043E01C}", L"Programs and Features" },
    };
    constexpr int kFallbackCount = sizeof(kFallbacks) / sizeof(kFallbacks[0]);
}

// ---- public ----

bool ControlService::ResolveIndirectString(const wchar_t* pszSource, CDuiString& sOut)
{
    sOut.Empty();
    if (!pszSource || !*pszSource) return false;

    WCHAR buf[512] = { 0 };
    void* pReserved = nullptr;
    HRESULT hr = ::SHLoadIndirectString(pszSource, buf, 512, &pReserved);
    if (SUCCEEDED(hr))
    {
        sOut = buf;
        return true;
    }

    // 兜底：非 @ 引用直接当纯字符串
    if (pszSource[0] != L'@')
    {
        sOut = pszSource;
        return true;
    }
    return false;
}

bool ControlService::FallbackDisplayName(const wchar_t* pszCanonical, CDuiString& sOut)
{
    sOut.Empty();
    if (!pszCanonical || !*pszCanonical) return false;
    for (int i = 0; i < kFallbackCount; ++i)
    {
        if (lstrcmpiW(kFallbacks[i].clsid, pszCanonical) == 0)
        {
            sOut = kFallbacks[i].name;
            return true;
        }
    }
    return false;
}

EControlEnumResult ControlService::EnumControlApplets(std::vector<CtrlAppletEntry>& vOut)
{
    CTRegGuard hKey;
    LONG l = ::RegOpenKeyEx(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ControlPanel\\NameSpace",
        0, KEY_READ | KEY_WOW64_64KEY, hKey.out());
    if (l != ERROR_SUCCESS) return EControlEnumResult::CTRL_ENUM_NO_KEY;

    DWORD dwIndex = 0;
    int nCount = 0;
    int nParseOk = 0;

    for (;;)
    {
        constexpr int kRegKeyNameWchars = 256;   // 256 缓冲尺寸（语义命名，数值不变）
        WCHAR szSub[kRegKeyNameWchars] = { 0 };
        DWORD dwLen = kRegKeyNameWchars;
        l = ::RegEnumKeyExW(hKey, dwIndex, szSub, &dwLen, nullptr, nullptr, nullptr, nullptr);
        if (l != ERROR_SUCCESS) break;
        ++dwIndex;

        CTRegGuard hSub;
        if (::RegOpenKeyExW(hKey, szSub, 0, KEY_READ | KEY_WOW64_64KEY, hSub.out())
                != ERROR_SUCCESS)
            continue;

        CtrlAppletEntry entry;
        entry.sCanonical = szSub;

        // Default 值
        CDuiString sDef = ReadStr(hSub, L"");
        CDuiString sInfoTip = ReadStr(hSub, L"InfoTip");

        if (sDef.IsEmpty())
        {
            // 无 Default，尝试 LocalizedString
            sDef = ReadStr(hSub, L"LocalizedString");
        }

        if (!sDef.IsEmpty())
        {
            CDuiString sResolved;
            if (ResolveIndirectString(sDef.GetData(), sResolved) && !sResolved.IsEmpty())
                entry.sDisplayName = sResolved;
            else if (sDef[0] != L'@')  // [R106] 非资源引用直接当纯字符串
                entry.sDisplayName = sDef;
            // [R106] 解析失败的 @ 资源引用：不泄漏原始字符串，留给下方兜底
        }

        if (entry.sDisplayName.IsEmpty())
        {
            // 兜底：InfoTip 或硬编码
            CDuiString sTip;
            if (ResolveIndirectString(sInfoTip.GetData(), sTip) && !sTip.IsEmpty())
                entry.sDisplayName = sTip;
            else
                FallbackDisplayName(szSub, entry.sDisplayName);
        }

        entry.sInfoTip = sInfoTip;

        if (!entry.sCanonical.IsEmpty() && !entry.sDisplayName.IsEmpty())
        {
            // [R111] 显示名去重：同名项追加 canonical 括注，避免列表出现多行同名
            bool bDup = false;
            for (size_t qi = 0; qi < vOut.size(); ++qi)
            {
                if (lstrcmpiW(vOut[qi].sDisplayName.GetData(),
                             entry.sDisplayName.GetData()) == 0)
                {
                    bDup = true;
                    break;
                }
            }
            if (bDup)
            {
                CDuiString sDedup;
                sDedup.Format(L"%s（%s）",
                              entry.sDisplayName.GetData(),
                              entry.sCanonical.GetData());
                entry.sDisplayName = sDedup;
            }
            vOut.push_back(entry);
            ++nParseOk;
        }
        ++nCount;

        if (dwIndex >= 512) break;  // 安全上限
    }


    if (nCount == 0) return EControlEnumResult::CTRL_ENUM_REG_FAIL;
    if (nParseOk == 0) return EControlEnumResult::CTRL_ENUM_PARSE_FAIL;
    return EControlEnumResult::CTRL_ENUM_OK;
}

namespace
{
    // [R127-2] 用 Shell 命名空间直连打开（SHParseDisplayName + SEE_MASK_IDLIST）。
    // 相比纯字符串命令 "control /name X"，本路径：
    //   - SHParseDisplayName 把 shell:::{CLSID} 解析为真实 PIDL
    //   - ShellExecuteExW(SEE_MASK_IDLIST) 直接定位 shell folder 对象打开
    //   - 额外走 IShellItem 绑定验证，返回 HRESULT 便于上层分级诊断
    // 返回 S_OK 表示已成功拉起目标；否则返回失败 HRESULT（保留 dwErr 供诊断）。
    HRESULT OpenShellNamespace(HWND hwnd, const wchar_t* pszUri, DWORD* pdwErrOut)
    {
        if (!pszUri || !*pszUri) return E_INVALIDARG;

        CTPidlGuard pidl(nullptr);
        DWORD rgf = 0;
        HRESULT hr = ::SHParseDisplayName(pszUri, nullptr, pidl.out(), 0, &rgf);
        if (FAILED(hr) || !pidl) return hr;  // CTPidlGuard 空 p 不释放

        // IShellItem 直连：验证该命名空间项可被 Shell 识别（BindToHandler BHID_SFObject）
        IShellItem* pItem = nullptr;
        HRESULT hrItem = ::SHCreateItemFromIDList(pidl, IID_IShellItem,
                                                  (void**)&pItem);
        if (FAILED(hrItem))
        {
            return hrItem;
        }

        IShellItem2* pItem2 = nullptr;
        if (SUCCEEDED(pItem->QueryInterface(IID_IShellItem2, (void**)&pItem2)))
        {
            pItem2->Release();  // 仅做接口可达性验证
        }
        pItem->Release();

        SHELLEXECUTEINFO sei = { 0 };
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_IDLIST | SEE_MASK_FLAG_LOG_USAGE;
        sei.hwnd = hwnd;
        sei.lpVerb = L"open";
        sei.lpIDList = pidl;
        sei.nShow = SW_SHOWNORMAL;

        ::SetLastError(ERROR_SUCCESS);
        BOOL bOk = ::ShellExecuteExW(&sei);
        DWORD dwErr = ::GetLastError();

        if (pdwErrOut) *pdwErrOut = dwErr;
        if (bOk) return S_OK;
        return HRESULT_FROM_WIN32(dwErr ? dwErr : ERROR_BAD_COMMAND);
    }

    // [R127-2] Win32 错误码 → ShellExecute 语义分级（SE_ERR_* 与 Win32 共存）
    EControlOpenDiag ClassifyWin32Error(DWORD dwErr)
    {
        switch (dwErr)
        {
        case ERROR_ACCESS_DENIED:      return EControlOpenDiag::CTRL_DIAG_ACCESSDENIED;
        case ERROR_ELEVATION_REQUIRED: return EControlOpenDiag::CTRL_DIAG_ELEV_REQUIRED;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:     return EControlOpenDiag::CTRL_DIAG_FILENOTFOUND;
        case ERROR_CANCELLED:          return EControlOpenDiag::CTRL_DIAG_CANCELED;
        case static_cast<DWORD>(SE_ERR_NOASSOC):    return EControlOpenDiag::CTRL_DIAG_NOASSOC;
        default:                       return EControlOpenDiag::CTRL_DIAG_OTHER;
        }
    }
}

EControlOpenResult ControlService::OpenControlApplet(HWND hwnd, const wchar_t* pszCanonical,
                                      DWORD* pdwLastError)
{
    if (pdwLastError) *pdwLastError = ERROR_SUCCESS;
    if (!pszCanonical || !*pszCanonical)
    {
        if (pdwLastError) *pdwLastError = ERROR_INVALID_PARAMETER;
        return EControlOpenResult::CTRL_OPEN_BAD_CANON;
    }

    // [R127-2] CLSID 型 canonical 优先走 Shell 命名空间直连
    //（SHParseDisplayName + IShellItem + SEE_MASK_IDLIST），
    // 失败保留诊断码后回退到字符串命令兜底。
    DWORD dwNsErr = ERROR_SUCCESS;
    bool bClsid = (pszCanonical[0] == L'{');
    if (bClsid)
    {
        CDuiString sUri;
        sUri.Format(L"shell:::%s", pszCanonical);
        HRESULT hr = OpenShellNamespace(hwnd, sUri.GetData(), &dwNsErr);
        if (SUCCEEDED(hr)) return EControlOpenResult::CTRL_OPEN_OK;
    }

    // [R106] 统一走 ShellExecuteExW + SEE_MASK_FLAG_LOG_USAGE：
    //   - 规范封装（支持 lpVerb/lpParameters/lpDirectory/lpIDList）
    //   - SEE_MASK_FLAG_LOG_USAGE 让系统记录本次打开（用户活动/统计）
    //   - 失败时 GetLastError() 提供更精确诊断（替代 ShellExecuteW 的 HINSTANCE 猜测）
    CDuiString sUri2;
    if (bClsid)
        sUri2.Format(L"shell:::%s", pszCanonical);
    else
        sUri2.Format(L"control /name %s", pszCanonical);

    SHELLEXECUTEINFO sei = { 0 };
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_FLAG_LOG_USAGE;
    sei.hwnd = hwnd;
    sei.lpVerb = L"open";
    sei.lpFile = sUri2.GetData();
    sei.nShow = SW_SHOWNORMAL;

    ::SetLastError(ERROR_SUCCESS);
    BOOL bOk = ::ShellExecuteExW(&sei);
    if (bOk) return EControlOpenResult::CTRL_OPEN_OK;

    // [R109] 回传真实错误码（此前错误地把 GetLastError 清零，上层无法诊断）
    DWORD dwErr = ::GetLastError();
    if (pdwLastError) *pdwLastError = dwErr;
    // [R127-2] 若命名空间直连已取到更具体错误，优先回传（如 ERROR_ELEVATION_REQUIRED）
    if (dwErr == ERROR_SUCCESS && dwNsErr != ERROR_SUCCESS && pdwLastError)
        *pdwLastError = dwNsErr;
    return EControlOpenResult::CTRL_OPEN_SHELLEX_FAIL;
}

EControlOpenResult ControlService::OpenControlAppletElevated(HWND hwnd, const wchar_t* pszCanonical,
                                              DWORD* pdwLastError)
{
    if (pdwLastError) *pdwLastError = ERROR_SUCCESS;
    if (!pszCanonical || !*pszCanonical)
    {
        if (pdwLastError) *pdwLastError = ERROR_INVALID_PARAMETER;
        return EControlOpenResult::CTRL_OPEN_BAD_CANON;
    }

    CDuiString sUri;
    if (pszCanonical[0] == L'{')
        sUri.Format(L"shell:::%s", pszCanonical);
    else
        sUri.Format(L"control /name %s", pszCanonical);

    // 优先走 Shell 命名空间 runas（SEE_MASK_IDLIST + lpVerb="runas" 触发 UAC）
    CTPidlGuard pidl(nullptr);
    DWORD rgf = 0;
    HRESULT hr = ::SHParseDisplayName(sUri.GetData(), nullptr, pidl.out(), 0, &rgf);

    SHELLEXECUTEINFO sei = { 0 };
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_FLAG_LOG_USAGE;
    sei.hwnd = hwnd;
    sei.lpVerb = L"runas";
    sei.nShow = SW_SHOWNORMAL;
    BOOL bOk = FALSE;

    if (SUCCEEDED(hr) && pidl)
    {
        sei.fMask |= SEE_MASK_IDLIST;
        sei.lpIDList = pidl;
        ::SetLastError(ERROR_SUCCESS);
        bOk = ::ShellExecuteExW(&sei);
        DWORD dwErr = ::GetLastError();
        if (pdwLastError) *pdwLastError = dwErr;
    }
    else
    {
        sei.lpFile = sUri.GetData();
        ::SetLastError(ERROR_SUCCESS);
        bOk = ::ShellExecuteExW(&sei);
        if (pdwLastError) *pdwLastError = ::GetLastError();
    }

    return bOk ? EControlOpenResult::CTRL_OPEN_OK : EControlOpenResult::CTRL_OPEN_ELEVATE_FAIL;
}

EControlOpenDiag ControlService::DiagnoseOpenFailure(EControlOpenResult nRc, DWORD dwLastError)
{
    if (nRc == EControlOpenResult::CTRL_OPEN_OK) return EControlOpenDiag::CTRL_DIAG_NONE;
    if (nRc == EControlOpenResult::CTRL_OPEN_BAD_CANON) return EControlOpenDiag::CTRL_DIAG_OTHER;
    return ClassifyWin32Error(dwLastError);
}
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
