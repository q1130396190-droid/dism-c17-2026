// ============================================================
// FontSupport.cpp —— 字体可用性探测与回退
// 见 FontSupport.h 顶部说明（含实测证据）。
// ============================================================

#include "stdafx.h"

#include "FontSupport.h"

namespace fontsup
{

bool IsFontAvailable(const wchar_t* pszFaceName)
{
    if (!pszFaceName || !pszFaceName[0]) return false;

    // 用 CreateFontIndirect + GetTextFace 实测：GDI 在字体不存在时会
    // **静默替换**成别的字体，因此必须回读实际选中的字面名来判定。
    LOGFONTW lf = {};
    lf.lfHeight = -12;                     // 字号不影响字面名判定
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfWeight = FW_NORMAL;
    // LF_FACESIZE 是 SDK 常量（32），这里不另定义常量
    ::lstrcpynW(lf.lfFaceName, pszFaceName, LF_FACESIZE);

    HFONT hFont = ::CreateFontIndirectW(&lf);
    if (!hFont) return false;

    bool bOk = false;
    HDC hdc = ::CreateCompatibleDC(nullptr);
    if (hdc)
    {
        HGDIOBJ hOld = ::SelectObject(hdc, hFont);
        wchar_t szActual[LF_FACESIZE] = {};   // API_OUT：GetTextFaceW 需要可写缓冲
        const int n = ::GetTextFaceW(hdc, LF_FACESIZE, szActual);
        if (n > 0) bOk = (::lstrcmpiW(szActual, pszFaceName) == 0);
        ::SelectObject(hdc, hOld);
        ::DeleteDC(hdc);
    }
    ::DeleteObject(hFont);
    return bOk;
}

const wchar_t* ResolveFirstAvailable(const wchar_t* const* ppCandidates, int nCount)
{
    if (!ppCandidates) return L"";
    for (int i = 0; i < nCount; ++i)
    {
        if (IsFontAvailable(ppCandidates[i])) return ppCandidates[i];
    }
    return L"";
}

namespace
{

// [R201] 三组语义字体的候选表。
//   顺序依据：先"设计意图"（YaHei），再"同族变体"（YaHei UI 是 Win10/11
//   实际安装的那个），再"跨语言同族"（JhengHei 覆盖繁体），最后系统兜底。
const wchar_t* const kTextCandidates[] = {
    L"Microsoft YaHei",        // 设计稿指定
    L"Microsoft YaHei UI",     // Win10/11 实际安装（本机实测只有这个）
    L"Microsoft JhengHei UI",  // 繁体同名族
    L"SimSun",                 // 中易宋体：所有中文系统必有
};

const wchar_t* const kSymbolCandidates[] = {
    L"Segoe UI Symbol",
    L"Segoe UI",
    L"SimSun",
};

const wchar_t* const kMonoCandidates[] = {
    L"Consolas",
    L"Cascadia Mono",          // Win11 新版终端等宽字体
    L"Courier New",
};

const wchar_t* ResolveOnce(const wchar_t* const* ppCandidates, int nCount)
{
    const wchar_t* psz = ResolveFirstAvailable(ppCandidates, nCount);
    return (psz && psz[0]) ? psz : ppCandidates[0];   // 全不可用则保留首选名
}

}   // namespace

const wchar_t* UiTextFace()
{
    static const wchar_t* const s_psz = []()
    {
        const wchar_t* psz = ResolveOnce(kTextCandidates,
            static_cast<int>(sizeof(kTextCandidates) / sizeof(kTextCandidates[0])));
        // 一次性日志：把"设计名 → 实际名"的差异留痕，便于排查字体类显示问题。
        std::wstring s = L"[font] ui-text: ";
        s += kTextCandidates[0];
        s += L" -> ";
        s += psz;
        s += L"\r\n";
        ::OutputDebugStringW(s.c_str());
        return psz;
    }();
    return s_psz;
}

const wchar_t* UiSymbolFace()
{
    static const wchar_t* const s_psz = ResolveOnce(kSymbolCandidates,
        static_cast<int>(sizeof(kSymbolCandidates) / sizeof(kSymbolCandidates[0])));
    return s_psz;
}

const wchar_t* UiMonoFace()
{
    static const wchar_t* const s_psz = ResolveOnce(kMonoCandidates,
        static_cast<int>(sizeof(kMonoCandidates) / sizeof(kMonoCandidates[0])));
    return s_psz;
}

}   // namespace fontsup