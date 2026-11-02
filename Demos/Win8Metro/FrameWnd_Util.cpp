#include "stdafx.h"
#include "FrameWnd_Util.h"
#include "StringManager.h"
// ============================================================
// FrameWnd_Util 实现 —— C1 拆分试点（R27）
// 从 FrameWnd.cpp 抽出的纯 Win32 UI 工具函数（不依赖 DuiLib 控件类）。
// 目标：为后续按耦合度分层拆分（FrameWnd_Scan / Optim / Clean 等）立模板。
// ============================================================
bool FrameWnd_Util_CopyTextToClipboard(HWND hwnd, const wchar_t* pszText)
{
    if (!pszText) return false;
    if (!::OpenClipboard(hwnd)) return false;
    ::EmptyClipboard();
    SIZE_T cb = (SIZE_T)(::lstrlen(pszText) + 1) * sizeof(wchar_t);
    HGLOBAL hMem = ::GlobalAlloc(GMEM_MOVEABLE, cb);
    if (!hMem) { ::CloseClipboard(); return false; }
    void* pMem = ::GlobalLock(hMem);
    if (!pMem)
    {
        ::GlobalFree(hMem);
        ::CloseClipboard();
        return false;
    }
    ::memcpy(pMem, pszText, cb);
    ::GlobalUnlock(hMem);
    HGLOBAL hSet = ::SetClipboardData(CF_UNICODETEXT, hMem);
    if (!hSet)
    {
        ::GlobalFree(hMem);   // SetClipboardData 失败，所有权未转移
    }
    ::CloseClipboard();
    return hSet != nullptr;
}
// ---- P-P1（新版移植）----
// 等价原 CFrameWnd::GetTempDir：GetTempPath 带缓冲 512，
// 有效长度 n∈(0,512) 时使用返回值，否则回退 L"%TEMP%"。
DuiLib::CDuiString FrameWnd_Util_GetTempDir()
{
    constexpr int kTempPathWchars = 512;   // 512 缓冲尺寸（语义命名，数值不变）
    wchar_t sz[kTempPathWchars] = { 0 };
    DWORD n = ::GetTempPath(512, sz);
    if (n > 0 && n < 512) return CDuiString(sz);
    return CDuiString(L"%TEMP%");
}
// ---- P4：Appx / 可选功能 / 更新页共享的纯字符串辅助 ----
// 实现原位于 FrameWnd.cpp，此处补充为共享实现；调用方只需 include FrameWnd_Util.h。
DuiLib::CDuiString TrimStr(const DuiLib::CDuiString& s)
{
    const wchar_t* p = s.GetData();
    int a = 0, b = s.GetLength() - 1;
    while (a <= b && (p[a] == L' ' || p[a] == L'\t')) ++a;
    while (b >= a && (p[b] == L' ' || p[b] == L'\t')) --b;
    if (a > b) return CDuiString();
    return s.Mid(a, b - a + 1);
}
DuiLib::CDuiString AppxFamily(const DuiLib::CDuiString& sFull)
{
    int p1 = sFull.Find(L'_');
    if (p1 <= 0) return sFull;
    const wchar_t* psz = sFull.GetData();
    int pLast = -1;
    for (int i = p1 + 1; i < static_cast<int>(sFull.GetLength()); ++i)
        if (psz[i] == L'_') pLast = i;
    if (pLast <= p1) return sFull.Mid(0, p1);
    return sFull.Mid(0, p1) + L"_" + sFull.Mid(pLast + 1);
}

// [I18N-APPX-NAME] Appx 包名 → 当前语言的显示名。
//   为什么需要：列表原来直接显示 PackageFullName（Microsoft.BingWeather_4.54…
//   _neutral_~_8wekyb3d8bbwe），普通用户读不出这是"天气"。这里按包名
//   （PackageFullName 第一个 '_' 之前的部分，即 PackageName）去语言包查
//   "appx.name.<PackageName>"，拿到当前语言的名称。
//   关键约束：本函数**不含任何语言的写死文案**，只拼 key 前缀；
//   中文/英文/其它语言的名称全部来自 lang/*_translations.json。
//   查不到时返回空串，由调用方决定回退到原始包名（保证永不出现空标题）。
DuiLib::CDuiString AppxDisplayName(const DuiLib::CDuiString& sFull)
{
    if (sFull.IsEmpty()) return DuiLib::CDuiString();

    // 取第一个 '_' 之前的包名；无 '_' 时整串即包名
    int nCut = sFull.Find(L'_');
    DuiLib::CDuiString sPkgName = (nCut > 0) ? sFull.Mid(0, nCut) : sFull;

    CDuiString sKey;
    sKey.Format(L"appx.name.%s", sPkgName.GetData());

    CDuiString sOut;
    if (i18n::CStringManager::GetInstance().TryGet(sKey.GetData(), sOut) && !sOut.IsEmpty())
        return sOut;
    return CDuiString();
}
// ---- P4：共享字符串辅助（原 FrameWnd.cpp）----
CDuiString PrettyCapName(const CDuiString& sIdentity)
{
    CDuiString s = sIdentity;
    int pTilde = s.Find(L'~');
    if (pTilde > 0) s = s.Mid(0, pTilde);


    CDuiString sOut;
    for (int i = 0; i < s.GetLength(); ++i)
    {
        wchar_t ch = s[i];
        if (ch == L'.') ch = L' ';
        sOut += ch;
    }
    sOut = TrimStr(sOut);


    // drop noisy well-known prefixes
    const wchar_t* pszSkip[] = {
        L"Microsoft Windows Wifi Client ",
        L"Microsoft ",
        L"Windows ",
        L"App "
    };
    for (int k = 0; k < 4; ++k)
    {
        size_t nLen = wcslen(pszSkip[k]);
        if (static_cast<size_t>(sOut.GetLength()) > nLen
            && _wcsnicmp(sOut.GetData(), pszSkip[k], nLen) == 0)
        {
            sOut = sOut.Mid(static_cast<int>(nLen));
            break;
        }
    }
    return TrimStr(sOut);
}
const wchar_t* WuaLocalizeErrorCode(int nErr)
{
    switch (nErr)
    {
    case 0x00000000: return StringMgr.Get("misc.h3303");
    case 0x80004005: return StringMgr.Get("misc.h9863");
    case 0x80070005: return StringMgr.Get("misc.h91f9");
    case 0x80070057: return StringMgr.Get("misc.he3c6");
    case 0x8007007E: return StringMgr.Get("misc.h56bf");
    case 0x80070490: return StringMgr.Get("misc.hfca1");
    case 0x800B0002: return StringMgr.Get("misc.h911b");
    case 0x80240001: return StringMgr.Get("misc.h4356");
    case 0x80240003: return StringMgr.Get("misc.hb814");
    case 0x80240012: return StringMgr.Get("misc.h6068");
    case 0x80240013: return StringMgr.Get("misc.heb55");
    case 0x80240015: return StringMgr.Get("misc.h53e1");
    case 0x80240017: return StringMgr.Get("misc.he12d");
    case 0x8024001D: return StringMgr.Get("misc.hc216");
    case 0x80240022: return StringMgr.Get("misc.h4c98");
    case 0x80240028: return StringMgr.Get("misc.h2ed1");
    case 0x8024002B: return StringMgr.Get("misc.h13f3");
    case 0x80240069: return StringMgr.Get("misc.h5044");
    case 0x80242001: return StringMgr.Get("misc.h785a");
    case 0x80242004: return StringMgr.Get("misc.h2a4e");
    case 0x80242005: return StringMgr.Get("misc.h244f");
    case 0x8024200B: return StringMgr.Get("misc.h1710");
    case 0x80244006: return StringMgr.Get("misc.h4540");
    case 0x80244007: return StringMgr.Get("misc.h5026");
    case 0x80244010: return StringMgr.Get("misc.h0795");
    case 0x80244011: return StringMgr.Get("misc.ha8ab");
    case 0x80244020: return StringMgr.Get("misc.h26ee");
    default: return StringMgr.Get("misc.h303a");
    }
}
int UpdateTypeOf(const CDuiString& sTitle)
{
    CDuiString s = sTitle;
    s.MakeUpper();
    if (s.Find(L"SECURITY") >= 0
     || s.Find(StringMgr.Get("misc.hfdbc")) >= 0)
        return 1;
    if (s.Find(L"DEFINITIONS") >= 0
     || s.Find(L"DEFINITION") >= 0
     || s.Find(StringMgr.Get("misc.h7ac3")) >= 0)
        return 3;
    if (s.Find(L"CUMULATIVE") >= 0
     || s.Find(StringMgr.Get("misc.hb2d2")) >= 0
     || s.Find(L"MINIUPDATE") >= 0)
        return 2;
    if (s.Find(L"FEATURE UPDATE") >= 0
     || s.Find(L"QUALITY UPDATE") >= 0
     || s.Find(StringMgr.Get("misc.he15e")) >= 0)
        return 4;
    if (s.Find(L"DRIVER") >= 0
     || s.Find(StringMgr.Get("misc.h0eb1")) >= 0)
        return 5;
    return 0;
}

// 逐级创建目录：CreateDirectory 不支持多级路径（%LOCALAPPDATA%\DismTool_Clone\logs
// 两级缺失时旧实现静默失败，日志从未落盘）。已存在按成功处理。
bool FrameWnd_Util_EnsureDirectoryDeep(const wchar_t* pszDir)
{
    if (!pszDir || !*pszDir) return false;
    std::wstring sCur;
    const wchar_t* p = pszDir;
    for (size_t i = 0; p[i]; ++i)
    {
        sCur.push_back(p[i]);
        if ((p[i] == L'\\' || p[i] == L'/') && sCur.size() >= 3 && sCur[1] == L':')
            ::CreateDirectory(sCur.c_str(), nullptr);   // 从 "X:\" 之后逐级建
    }
    ::CreateDirectory(sCur.c_str(), nullptr);
    DWORD attr = ::GetFileAttributes(pszDir);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
