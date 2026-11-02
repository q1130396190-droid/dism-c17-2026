// Win32DialogService.cpp —— S2（P1）
// 独立于预编译头：不 include stdafx.h / UIlib.h，仅系统头 + Utils。

#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>

#include "Win32DialogService.h"
#include <string>       // [R8] std::wstring（F13 纯内部缓冲提级）

using DuiLib::CDuiString;

// OPENFILENAME.lpstrFilter 要求以真实 NUL 分隔、双 NUL 收尾的成对字符串：
//   L"说明\0掩码\0说明\0掩码\0\0"
// 语言包/字符串表中有一批条目把分隔符误存成了字面反斜杠序列加 '0'
// （源码 "\\0" 运行时为 '\' '0'；"\\\\0" 运行时为 '\\' '\\' '0'），
// 系统对话框会把全部格式拼成一行、无法下拉切换。
// 本函数在送入对话框前统一规范化，兼容两种来源：
//   1) 原生形态：真实 NUL 分隔、双 NUL 收尾（修正后的语言包/硬编码）→ 透传；
//   2) 病态形态：首个真实 NUL 前含字面 '\'+'0' 序列（旧语言包/外挂包）
//      → 把"一个或多个反斜杠 + 0"整体还原为 NUL，并补双 NUL 收尾。
// 判定依据：病态条目所有分隔符均为字面序列，其首个真实 NUL 必在全部字面
// 序列之后；过滤器文本只有"说明"与"*.ext"掩码，不存在合法反斜杠。
static int NormalizeFileFilter(const wchar_t* pszSrc, wchar_t* pszDst, int cchDst)
{
    if (pszDst == nullptr || cchDst <= 0) return 0;
    pszDst[0] = L'\0';
    if (pszSrc == nullptr || pszSrc[0] == L'\0') return 0;  // 空过滤器

    // ---- 形态探测（有界扫描）：
    //   bLiteral  —— 是否含字面 '\'+'0' 序列（病态条目标志）；
    //   bDoubleNul —— 有限范围内是否存在双 NUL（OPENFILENAME 合规收尾）。
    // 只有"无字面序列且双 NUL 收尾"才走原样透传；否则一律走按真实 NUL
    // 安全停止的重建分支，杜绝越过源串终止符读取。
    bool bLiteral = false;
    bool bDoubleNul = false;
    {
        const wchar_t* q = pszSrc;
        bool bPrevNul = false;
        for (int guard = 0; guard < 2048; ++guard)
        {
            if (q[0] == L'\0')
            {
                if (bPrevNul) { bDoubleNul = true; break; }  // 双 NUL
                // 已确认含字面序列时，首个真实 NUL 就是 C 串终止，不越界续读
                if (bLiteral) break;
                bPrevNul = true;  // 原生形态：可能是段间 NUL，继续探下一段
                ++q;
                continue;
            }
            if (!bLiteral && q[0] == L'\\')
            {
                const wchar_t* r = q;
                while (r[0] == L'\\') ++r;
                if (r[0] == L'0') bLiteral = true;
                q = r;
                bPrevNul = false;
                continue;
            }
            bPrevNul = false;
            ++q;
        }
    }
    const bool bPassThrough = !bLiteral && bDoubleNul;

    int i = 0;

    if (bPassThrough)
    {
        // 原生形态：逐字符透传，遇到双 NUL 结束
        const wchar_t* q = pszSrc;
        bool bPrevNul = false;
        while (i < cchDst - 1)
        {
            wchar_t ch = q[0];
            pszDst[i++] = ch;
            if (ch == L'\0' && bPrevNul) break;  // 双 NUL 收尾
            bPrevNul = (ch == L'\0');
            ++q;
        }
        return i;
    }

    // 病态形态：字面序列 → NUL，源串以单 NUL 终止，输出补成双 NUL
    const wchar_t* q = pszSrc;
    while (q[0] != L'\0' && i < cchDst - 2)
    {
        if (q[0] == L'\\')
        {
            const wchar_t* r = q;
            while (r[0] == L'\\') ++r;
            if (r[0] == L'0')
            {
                pszDst[i++] = L'\0';
                q = r + 1;  // 跳过整段反斜杠与 '0'
                continue;
            }
        }
        pszDst[i++] = q[0];
        ++q;
    }
    if (i < cchDst && pszDst[i - 1] != L'\0') pszDst[i++] = L'\0';
    if (i < cchDst) pszDst[i++] = L'\0';
    if (i < cchDst) pszDst[i] = L'\0';
    return i;
}

// 从"规范化后、双 NUL 分隔"的过滤器中，取第 nFilterIndex（1-based，
// 即 OPENFILENAME.nFilterIndex）所对掩码的首个扩展名（含 '.'，小写）。
// 掩码形如 "*.wim" 或 "*.wim;*.esd"；"所有文件"("*.*") 等无法确定
// 具体扩展名时返回 false，调用方据此保持用户原文件名不变。
static bool ExtFromFilterIndex(const wchar_t* pszFilter, int nFilterIndex,
                               wchar_t* pszExt, int cchExt)
{
    if (pszFilter == nullptr || nFilterIndex < 1 || pszExt == nullptr || cchExt < 4)
        return false;

    const wchar_t* p = pszFilter;
    int nPair = 0;
    while (p[0] != L'\0')
    {
        p += wcslen(p) + 1;                 // 描述段
        const wchar_t* pszMask = p;
        p += wcslen(p) + 1;                 // 掩码段
        ++nPair;
        if (nPair != nFilterIndex) continue;

        const wchar_t* pDot = wcschr(pszMask, L'.');
        if (pDot == nullptr) return false;
        const wchar_t* pSemi = wcschr(pDot, L';');
        int nLen = pSemi ? static_cast<int>(pSemi - pDot) : static_cast<int>(wcslen(pDot));
        if (nLen <= 1 || nLen >= 16) return false;   // "." 或异常长度

        std::wstring sTmp(pDot, static_cast<size_t>(nLen));
        _wcslwr_s(&sTmp[0], sTmp.size() + 1);
        if (lstrcmpW(sTmp.c_str(), L".*") == 0) return false;  // 所有文件

        wcsncpy_s(pszExt, cchExt, sTmp.c_str(), _TRUNCATE);
        return true;
    }
    return false;
}

// 把完整路径的文件名扩展名统一替换为 pszExt（自带 '.'）；
// 文件名本身无扩展名时追加。只动最后一个 '\\'/'/' 之后的文件名部分。
static void ApplyFileExtension(wchar_t* pszFile, int cchFile, const wchar_t* pszExt)
{
    if (pszFile == nullptr || pszExt == nullptr || pszExt[0] == L'\0') return;

    wchar_t* pName = pszFile;
    for (wchar_t* q = pszFile; q[0] != L'\0'; ++q)
        if (q[0] == L'\\' || q[0] == L'/') pName = q + 1;

    wchar_t* pDot = nullptr;
    for (wchar_t* q = pName; q[0] != L'\0'; ++q)
        if (q[0] == L'.') pDot = q;

    if (pDot) pDot[0] = L'\0';   // 截掉旧扩展名

    size_t nCur = wcslen(pszFile);
    if (static_cast<int>(nCur) < cchFile - 1)
        wcsncpy_s(pszFile + nCur, cchFile - static_cast<int>(nCur), pszExt, _TRUNCATE);
}

bool Win32DialogService::DlgOpenFile(CDuiString& sResult, const wchar_t* pszFilter, const wchar_t* pszTitle)
{
    wchar_t szFile[MAX_PATH * 4] = { 0 };
    constexpr int kFileFilterWchars = 512;   // 512 缓冲尺寸（语义命名，数值不变）
    wchar_t szFilter[kFileFilterWchars] = { 0 };
    NormalizeFileFilter(pszFilter, szFilter, _countof(szFilter));

    OPENFILENAME ofn; memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = ::GetForegroundWindow();
    ofn.lpstrFilter = szFilter;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = _countof(szFile);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrTitle = pszTitle;

    if (!::GetOpenFileName(&ofn)) return false;

    sResult = szFile;
    return true;
}

bool Win32DialogService::DlgSaveFile(CDuiString& sResult, const wchar_t* pszFilter,
                                     const wchar_t* pszTitle, const wchar_t* pszDefName)
{
    wchar_t szFile[MAX_PATH * 4] = { 0 };
    constexpr int kFileFilterWchars = 512;   // 512 缓冲尺寸（语义命名，数值不变）
    wchar_t szFilter[kFileFilterWchars] = { 0 };
    NormalizeFileFilter(pszFilter, szFilter, _countof(szFilter));

    if (pszDefName)
        wcsncpy_s(szFile, _countof(szFile), pszDefName, _TRUNCATE);

    OPENFILENAME ofn; memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = ::GetForegroundWindow();
    ofn.lpstrFilter = szFilter;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = _countof(szFile);
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY;
    ofn.lpstrTitle = pszTitle;

    if (!::GetSaveFileName(&ofn)) return false;

    // [U-4] GetSaveFileName 不会随"保存类型"下拉切换而修改文件名扩展名
    //   （默认 Backup.wim、切到 ESD 后仍返回 Backup.wim）。这里按用户实际
    //   选中的 nFilterIndex 主动同步：有扩展名则替换、无则追加；
    //   选"所有文件"时取不到具体扩展名，保持用户输入不变。
    wchar_t szExt[16] = { 0 };
    if (ExtFromFilterIndex(szFilter, ofn.nFilterIndex, szExt, _countof(szExt)))
        ApplyFileExtension(szFile, _countof(szFile), szExt);

    sResult = szFile;
    return true;
}

bool Win32DialogService::DlgBrowseFolder(CDuiString& sResult, const wchar_t* pszTitle)
{
    constexpr int kDlgTitleWchars = 256;   // 256 缓冲尺寸（语义命名，数值不变）
    wchar_t szTitle[kDlgTitleWchars] = { 0 };
    wcsncpy_s(szTitle, _countof(szTitle), pszTitle, _TRUNCATE);

    BROWSEINFO bi; memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = ::GetForegroundWindow();
    bi.lpszTitle = szTitle;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    LPITEMIDLIST pidl = ::SHBrowseForFolder(&bi);
    if (!pidl) return false;

    wchar_t szPath[MAX_PATH] = { 0 };
    bool bOk = !!::SHGetPathFromIDList(pidl, szPath);
    ::CoTaskMemFree(pidl);

    if (!bOk) return false;

    sResult = szPath;
    return true;
}
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
