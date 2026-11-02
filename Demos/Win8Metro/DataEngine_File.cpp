#include "stdafx.h"
#include "DataEngine.h"
#include <shellapi.h>   // SHCreateDirectoryExW
#include "unzip.h"      // [R9-03b] UnzipEntryToPath：仓库内 minizip
#include "WinHandles.h" // [C++17 R3] win::RegKey RAII for F11_raw_handle_close
#include <vector>

// ============================================================
// DataEngine_File.cpp —— [R9-02] 文件系统执行器。
// FileCreateByZIP / FileMove / SetDesktop / NtSetSystemInformation
// 动作处理器从 DataEngine.cpp ExecActivate if 链拆出，逻辑零改动。
// ============================================================

CDataEngine::EActResult CDataEngine::ActFileCreateByZip(CMarkupNode node, TActCtx& ctx)
{
    const wchar_t* pszPath = node.GetAttributeValue(L"Path");
    if (!pszPath || !*pszPath) { ctx.Skip(); return EActResult::ACT_SKIP; }

    const wchar_t* pszZipFile = node.HasAttribute(L"ZIPFile")
        ? node.GetAttributeValue(L"ZIPFile")
        : node.GetAttributeValue(L"ZipFile");
    const wchar_t* pszKey  = node.GetAttributeValue(L"Key");
    const wchar_t* pszName = node.GetAttributeValue(L"Name");
    bool bOverwrite = node.HasAttribute(L"Overwrite")
        && lstrcmpiW(node.GetAttributeValue(L"Overwrite"), L"False") != 0;

    CDuiString sZip, sEntry;
    if (pszZipFile && *pszZipFile)
    {
        CDuiString sComb(pszZipFile);
        int nSlash = sComb.Find(L'/');
        if (nSlash <= 0) { ctx.Skip(); return EActResult::ACT_SKIP; }
        sZip = sComb.Mid(0, nSlash);
        sEntry = sComb.Mid(nSlash + 1);
    }
    else if (pszKey && *pszKey)
    {
        sZip = pszKey;
        sEntry = (pszName && *pszName) ? pszName : pszKey;
    }
    else { ctx.Skip(); return EActResult::ACT_SKIP; }

    bool bLegacy = (pszKey && *pszKey) && !(pszZipFile && *pszZipFile);
    if (!sZip.IsEmpty() && sZip[0] != L'\\'
        && (sZip.GetLength() < 2 || sZip[1] != L':'))
    {
        sZip = ResolveExeRelative(sZip, bLegacy ? L"\\Resources\\" : L"\\");
    }

    bool bDstExists = false;
    wchar_t szT[MAX_PATH];
    ::ExpandEnvironmentStrings(pszPath, szT, _countof(szT));
    bDstExists = ::GetFileAttributes(szT) != INVALID_FILE_ATTRIBUTES;

    bool bAct = UnzipEntryToPath(sZip.GetData(), sEntry.GetData(), pszPath, 15000);
    if (!bAct && !bOverwrite && bDstExists) return EActResult::ACT_OK;  // 已存在按跳过=成功
    return bAct ? EActResult::ACT_OK : EActResult::ACT_FAIL;
}

CDataEngine::EActResult CDataEngine::ActSetDesktop(CMarkupNode node, TActCtx&)
{
    bool bAct = DoSetDesktop(node.GetAttributeValue(L"WallPaper"),
                             node.GetAttributeValue(L"Color"));
    return bAct ? EActResult::ACT_OK : EActResult::ACT_FAIL;
}

CDataEngine::EActResult CDataEngine::ActNtSetSystemInformation(CMarkupNode node, TActCtx&)
{
    const wchar_t* pszCls = node.GetAttributeValue(L"Class");
    DWORD dwClass = 0;
    if (!ParseInformationClass(pszCls, &dwClass))
        return EActResult::ACT_SKIP;   // 旧版此处不计数 unsupported，仅跳过
    const wchar_t* pszBuf = node.GetAttributeValue(L"Buffer");
    // Base64/hex 解码输出缓冲（单条 Buffer 属性通常远小于 4KB）
    constexpr size_t kAttrBufSize = 4096;
    BYTE abBuf[kAttrBufSize] = { 0 };
    DWORD dwLen = 0;
    // [R5 / P1] '#' 前缀走 Base64Decode，兼容 R3 现有 hex/dec 分号语法
    if (pszBuf && pszBuf[0] == L'#')
    {
        std::vector<BYTE> vBin;
        if (Base64Decode(pszBuf + 1, vBin)
            && !vBin.empty() && vBin.size() <= sizeof(abBuf))
        {
            dwLen = static_cast<DWORD>(vBin.size());
            ::memcpy(abBuf, vBin.data(), dwLen);
        }
    }
    else
    {
        dwLen = ParseNtBuffer(pszBuf, abBuf, sizeof(abBuf));
    }
    LONG lStatus = 0;
    return DoNtSetSystemInformation(dwClass, abBuf, dwLen, &lStatus)
           ? EActResult::ACT_OK : EActResult::ACT_FAIL;
}

CDataEngine::EActResult CDataEngine::ActFileMove(CMarkupNode node, TActCtx& ctx)
{
    const wchar_t* pszSrc = node.GetAttributeValue(L"Src");
    const wchar_t* pszDst = node.GetAttributeValue(L"Dst");
    if (!pszSrc || !*pszSrc || !pszDst || !*pszDst)
    { ctx.Skip(); return EActResult::ACT_SKIP; }

    wchar_t szSrc[MAX_PATH * 2] = { 0 };
    wchar_t szDst[MAX_PATH * 2] = { 0 };
    DWORD c1 = ::ExpandEnvironmentStrings(pszSrc, szSrc, _countof(szSrc));
    DWORD c2 = ::ExpandEnvironmentStrings(pszDst, szDst, _countof(szDst));
    bool bSkip = node.HasAttribute(L"SkipError");
    if (c1 == 0 || c2 == 0)
        return bSkip ? EActResult::ACT_SKIP : EActResult::ACT_FAIL;

    // 源不存在按幂等成功处理（规则可能已执行过）
    if (::GetFileAttributesW(szSrc) == INVALID_FILE_ATTRIBUTES)
        return EActResult::ACT_OK;

    // 自动创建目标父目录
    wchar_t szParent[MAX_PATH * 2] = { 0 };
    wcsncpy_s(szParent, szDst, _TRUNCATE);
    wchar_t* pSlash = wcsrchr(szParent, L'\\');
    if (pSlash && pSlash != szParent + 2)   // 保留 "X:\" 根
    {
        *pSlash = 0;
        ::SHCreateDirectoryExW(nullptr, szParent, nullptr);
    }
    bool bMv = ::MoveFileExW(szSrc, szDst,
                             MOVEFILE_REPLACE_EXISTING
                             | MOVEFILE_COPY_ALLOWED
                             | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (bMv) return EActResult::ACT_OK;
    return bSkip ? EActResult::ACT_SKIP : EActResult::ACT_FAIL;
}

// ============================================================
// [R9-03b] NtSetSystemInformation / DoSetDesktop（逐字搬迁自 DataEngine.cpp）
// ============================================================

// ============================================================
// R16 · NtSetSystemInformation helpers
// ============================================================
using PFN_NtSetSystemInformation = NTSTATUS(NTAPI*)(unsigned long, void*, unsigned long);

static PFN_NtSetSystemInformation GetNtSetSystemInformation()
{
    static PFN_NtSetSystemInformation pfn = nullptr;
    if (!pfn)
    {
        HMODULE hNt = ::GetModuleHandleW(L"ntdll.dll");
        if (hNt)
            pfn = (PFN_NtSetSystemInformation)::GetProcAddress(hNt,
                "NtSetSystemInformation");
    }
    return pfn;
}

// InformationClass 名称 → 数值
bool CDataEngine::ParseInformationClass(const wchar_t* pszName, DWORD* pdwOut)
{
    if (!pszName || !pdwOut) return false;

    // 支持两种写法：直接写数值（如 "61"），或写符号名（如 "SystemEulaAccepted"）
    bool bAllDec = true;
    for (const wchar_t* p = pszName; *p; ++p)
        if (*p < L'0' || *p > L'9') { bAllDec = false; break; }
    if (bAllDec)
    {
        *pdwOut = static_cast<DWORD>(wcstoul(pszName, nullptr, 10));
        return true;
    }

    struct Ent { const wchar_t* psz; DWORD dw; };
    static const Ent s_tab[] = {
        { L"SystemPolicyInformation",                 1 },
        { L"SystemTimeInformation",                   2 },
        { L"SystemPowerInformation",                  3 },
        { L"SystemProcessInformation",                5 },
        { L"SystemProcessorPerformanceInformation",   6 },
        { L"SystemInterruptInformation",              7 },
        { L"SystemTimezoneInformation",               8 },
        { L"SystemPerformanceInformation",            10 },
        { L"SystemResumeInformation",                 13 },
        { L"SystemResumePerformanceInformation",      14 },
        { L"SystemProcessInformation2",               15 },
        { L"SystemTimeZoneInformation",               18 },
        { L"SystemEulaAccepted",                      29 },
        { L"SystemSecureBootInformation",             57 },
        { L"SystemHypervisorInformation",             66 },
        { L"SystemHypervisorBootInformation",         67 },
        { L"SystemBootProperties",                    68 },
        { L"SystemDmaProtectionPolicyInformation",    69 },
        { L"SystemPowerRequestLimits",                70 },
        { L"SystemAppContainerElevationInformation",  71 },
        { L"SystemConnectedStandbyPolicy",            80 },
        { L"SystemConnectedStandbyPolicyEx",          81 },
        { L"SystemNetworkThrottling",                 96 },
        { L"SystemNetworkThrottlingLimits",           97 },
        { L"SystemSystemCpuPolicy",                   98 },
        { L"SystemSystemCpuPolicyEx",                 99 },
        { L"SystemSystemCpuPolicyEx2",               100 },
        { L"SystemSystemCpuPolicyEx3",               101 },
        { L"SystemSystemCpuPolicyEx4",               102 },
        { L"SystemSystemCpuPolicyEx5",               103 },
        { L"SystemSystemCpuPolicyEx6",               104 },
        { L"SystemSystemCpuPolicyEx7",               105 },
    };
    for (size_t i = 0; i < _countof(s_tab); ++i)
        if (lstrcmpiW(pszName, s_tab[i].psz) == 0)
        { *pdwOut = s_tab[i].dw; return true; }
    return false;
}

// 解析 Buffer 属性（hex 字符串，可含空格）到字节数组；返回字节数
DWORD CDataEngine::ParseNtBuffer(const wchar_t* pszBuf, BYTE* pOut, DWORD dwOutMax)
{
    if (!pszBuf || !pOut || dwOutMax == 0) return 0;
    DWORD n = 0;
    while (*pszBuf && n + 1 < dwOutMax)
    {
        if (*pszBuf == L' ' || *pszBuf == L'\t') { ++pszBuf; continue; }
        if (pszBuf[0] && pszBuf[1])
        {
            const std::wstring sHex(pszBuf, 2);
            pOut[n++] = static_cast<BYTE>(wcstoul(sHex.c_str(), nullptr, 16));
            pszBuf += 2;
        }
        else break;
    }
    return n;
}

bool CDataEngine::DoNtSetSystemInformation(
    DWORD dwInformationClass, const void* pBuffer, DWORD dwBufferLength,
    LONG* pErrStatus)
{
    if (pErrStatus) *pErrStatus = 0;
    PFN_NtSetSystemInformation pfn = GetNtSetSystemInformation();
    if (!pfn) return false;
    NTSTATUS st = pfn(dwInformationClass,
                       const_cast<void*>(pBuffer), dwBufferLength);
    if (pErrStatus) *pErrStatus = static_cast<LONG>(st);
    return (st == 0 /* STATUS_SUCCESS */ || st == (NTSTATUS)0xC0000004L /* STATUS_INFO_LENGTH_MISMATCH */);
}

// ============================================================
// R17 · SetDesktop / SetServiceRecovery / SetAppxPackage
// ============================================================

bool CDataEngine::DoSetDesktop(const wchar_t* pszWallPaper, const wchar_t* pszColor)
{
    // ---- 颜色（AccentColor / ColorizationColor）----
    if (pszColor && *pszColor)
    {
        // 输入 RRGGBB（无 0x 前缀），Windows 期望 COLORREF (0x00BBGGRR)
        DWORD dw = ParseDwordData(pszColor);
        // 写入 HKCU\Software\Microsoft\Windows\DWM (AccentColor/ColorizationColor)
        // [P0] 走 OpenRegKey 统一入口，PE 离线会话自动定向到挂载 NTUSER。
        win::RegKey key;   // [C++17 R3] F11: RAII auto RegCloseKey
        if (OpenRegKey(L"HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\DWM",
                KEY_SET_VALUE, *key.out(), true, false))
        {
            // [R128-A] 写失败即强调色未生效，但继续写下一项（两个键独立）
            LONG lA = ::RegSetValueEx(key.get(), L"AccentColor", 0, REG_DWORD,
                            (BYTE*)&dw, sizeof(dw));
            LONG lC = ::RegSetValueEx(key.get(), L"ColorizationColor", 0, REG_DWORD,
                            (BYTE*)&dw, sizeof(dw));
            if (lA != ERROR_SUCCESS || lC != ERROR_SUCCESS)
                ::OutputDebugStringW(L"[R128][visual] DWM 强调色写入失败\r\n");
        }
    }

    // ---- 壁纸 ----
    if (pszWallPaper && *pszWallPaper)
    {
        CDuiString sWall(pszWallPaper);

        // 相对路径：解析到 exe 同级 Wallpapers\ 目录
        if (wcslen(sWall) > 0 && sWall[0] != L'\\'
            && (sWall.GetLength() < 2 || sWall[1] != L':'))
        {
            wchar_t szExe[MAX_PATH] = { 0 };
            ::GetModuleFileName(nullptr, szExe, MAX_PATH);
            CDuiString sDir(szExe);
            int p = sDir.ReverseFind(L'\\');
            if (p >= 0) sDir = sDir.Mid(0, p);
            sWall = sDir + L"\\Wallpapers\\" + sWall;
        }

        // 写入当前用户壁纸（[P0] 统一入口，离线会话写入挂载 NTUSER）
        win::RegKey key;   // [C++17 R3] F11: RAII auto RegCloseKey
        if (OpenRegKey(L"HKEY_CURRENT_USER\\Control Panel\\Desktop",
                KEY_SET_VALUE, *key.out(), true, false))
        {
            // [R128-A] 三个键独立写入，逐个判定后汇总留痕（不提前 return）
            LONG lW  = ::RegSetValueEx(key.get(), L"Wallpaper", 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(sWall.GetData()),
                            (DWORD)((wcslen(sWall) + 1) * sizeof(wchar_t)));
            LONG lWS = ::RegSetValueEx(key.get(), L"WallpaperStyle", 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(L"2"), sizeof(L"2"));
            LONG lTW = ::RegSetValueEx(key.get(), L"TileWallpaper", 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(L"0"), sizeof(L"0"));
            if (lW != ERROR_SUCCESS || lWS != ERROR_SUCCESS || lTW != ERROR_SUCCESS)
                ::OutputDebugStringW(L"[R128][visual] 壁纸键写入失败\r\n");
        }

        // 通过 SystemParametersInfo 让壁纸立即生效
        ::SystemParametersInfo(SPI_SETDESKWALLPAPER, 0,
                               const_cast<void*>(static_cast<const void*>(sWall.GetData())), 0);
    }

    // 通知资源管理器刷新
    ::SendMessageTimeout(HWND_BROADCAST, WM_WININICHANGE, 0,
                         reinterpret_cast<LPARAM>(L"Control Panel\\Desktop"),
                         SMTO_ABORTIFHUNG, 2000, nullptr);
    RefreshDesktopIcons();
    return true;
}

// ============================================================
// [R9-03b] UnzipEntryToPath（逐字搬迁自 DataEngine.cpp）
// ============================================================

// ============================================================
// [R18 / S-09] FileCreateByZIP 执行器
// --------------------------------------------------
// [P0-A7] 直接用仓库自带 minizip（DuiLib\Utils\unzip.cpp）从 zip 中
// 提取单条目到目标路径：零进程、零外部依赖，WinPE/精简系统可用。
// ============================================================
bool CDataEngine::UnzipEntryToPath(const wchar_t* pszZipPath, const wchar_t* pszEntry,
                                    const wchar_t* pszDst, DWORD dwTimeoutMs)
{
    (void)dwTimeoutMs;   // 进程内 inflate，无需进程超时
    if (!pszZipPath || !*pszZipPath || !pszEntry || !*pszEntry ||
        !pszDst || !*pszDst)
        return false;

    // 展开环境变量并确保父目录链存在
    wchar_t szDst[MAX_PATH] = { 0 };
    ::ExpandEnvironmentStrings(pszDst, szDst, _countof(szDst));
    wchar_t szParent[MAX_PATH] = { 0 };
    wfmt::FormatInto(szParent, _countof(szParent), L"%s", szDst);
    wchar_t* pSlash = wcsrchr(szParent, L'\\');
    if (pSlash && pSlash != szParent)
    {
        *pSlash = L'\0';
        // 逐级创建（中间层可能不存在）
        for (wchar_t* p = szParent + 3; *p; ++p)   // 跳过 "\\?\" 不必，从盘符后开始
        {
            if (*p == L'\\')
            {
                *p = L'\0';
                ::CreateDirectory(szParent, nullptr);
                *p = L'\\';
            }
        }
        ::CreateDirectory(szParent, nullptr);
    }

    HZIP hz = OpenZip(pszZipPath, nullptr);
    if (!hz) return false;

    bool bOk = false;
    int nIndex = -1;
    ZIPENTRY ze = { 0 };
    // zip 条目名大小写不敏感匹配；Data.xml 中的条目名即包内相对路径
    if (FindZipItem(hz, pszEntry, true, &nIndex, &ze) == ZR_OK
        && nIndex >= 0
        && !(ze.attr & FILE_ATTRIBUTE_DIRECTORY))
    {
        bOk = (UnzipItem(hz, nIndex, szDst) == ZR_OK);
    }
    CloseZip(hz);

    // 兜底：验证目标文件确实被创建
    if (bOk)
    {
        DWORD dwAttr = ::GetFileAttributes(szDst);
        if (dwAttr == INVALID_FILE_ATTRIBUTES || (dwAttr & FILE_ATTRIBUTE_DIRECTORY))
            bOk = false;
    }
    return bOk;
}
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
