// ============================================================
// DwmTheme.cpp —— Win11 视觉适配层实现
//
// 关键取舍：**不链接 dwmapi.lib**。
//   链接会让 EXE 的导入表里出现 DwmSetWindowAttribute，虽然 Win8 起
//   该导出就存在，但属性号（DWM_WINDOW_CORNER_PREFERENCE 等）是新加的；
//   更稳妥的做法是运行期解析 + 属性号试探，这样同一份二进制在
//   Win10/Win11 上行为各自最优，且不会因为一次 API 变化而启动失败。
// ============================================================

#include "stdafx.h"

#include "DwmTheme.h"

#include "WinHandles.h"   // win::RegKey（RAII）

#include <cmath>          // std::pow（对比度计算的 sRGB 幂）

namespace dwmtheme
{

namespace
{

// ---- DWM 私有属性号（未公开在旧 SDK 头里，按微软文档取值）----
//   DWMWA_USE_IMMERSIVE_DARK_MODE = 20（19 是 1809 预览版旧值，两者都试）
//   DWMWA_WINDOW_CORNER_PREFERENCE = 33（Win11 21H2+）
//   DWMWA_SYSTEMBACKDROP_TYPE = 38（Win11 22H2+，22621 起）
constexpr DWORD kDwmAttrDarkMode       = 20;
constexpr DWORD kDwmAttrDarkModeLegacy = 19;
constexpr DWORD kDwmAttrCorner         = 33;
constexpr DWORD kDwmAttrBackdrop       = 38;

// DWMWCP_ROUND = 2（圆角 8px）；DWMSBT_MAINWINDOW = 2（Mica）
constexpr int kCornerRound   = 2;
constexpr int kBackdropMica  = 2;

using PFN_DwmSetWindowAttribute = HRESULT (WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
using PFN_DwmGetColorizationColor = HRESULT (WINAPI*)(DWORD*, BOOL*);

// 运行期解析一次（magic static，线程安全）
[[nodiscard]] PFN_DwmSetWindowAttribute PfnSetAttr()
{
    static const PFN_DwmSetWindowAttribute s_pfn = []() -> PFN_DwmSetWindowAttribute
    {
        HMODULE hDwm = ::GetModuleHandleW(L"dwmapi.dll");
        if (!hDwm) hDwm = ::LoadLibraryW(L"dwmapi.dll");   // 极少数精简系统需显式加载
        if (!hDwm) return nullptr;
        return reinterpret_cast<PFN_DwmSetWindowAttribute>(
            ::GetProcAddress(hDwm, "DwmSetWindowAttribute"));
    }();
    return s_pfn;
}

[[nodiscard]] PFN_DwmGetColorizationColor PfnGetColorization()
{
    static const PFN_DwmGetColorizationColor s_pfn = []() -> PFN_DwmGetColorizationColor
    {
        HMODULE hDwm = ::GetModuleHandleW(L"dwmapi.dll");
        if (!hDwm) hDwm = ::LoadLibraryW(L"dwmapi.dll");
        if (!hDwm) return nullptr;
        return reinterpret_cast<PFN_DwmGetColorizationColor>(
            ::GetProcAddress(hDwm, "DwmGetColorizationColor"));
    }();
    return s_pfn;
}

// 读 HKCU 的 DWORD 值；不存在返回 false（不区分"读失败"与"值不存在"，
// 两者对调用方的含义相同：用下一个来源）。
[[nodiscard]] bool ReadHkcuDword(const wchar_t* pszSubKey, const wchar_t* pszName, DWORD& out)
{
    win::RegKey hKey;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, pszSubKey, 0, KEY_READ, hKey.out()) != ERROR_SUCCESS)
        return false;
    DWORD dwType = 0;
    DWORD cb = sizeof(DWORD);
    const LONG lRc = ::RegQueryValueExW(hKey.get(), pszName, nullptr, &dwType,
                                        reinterpret_cast<LPBYTE>(&out), &cb);
    return lRc == ERROR_SUCCESS && dwType == REG_DWORD;
}

// ---- sRGB 线性亮度（WCAG 2.1 定义），用于保证"强调色当文字"时的对比度 ----
[[nodiscard]] double LinearOf(unsigned int n8)
{
    const double c = static_cast<double>(n8) / 255.0;
    if (c <= 0.03928) return c / 12.92;
    // 指数 2.4：用 std::pow（cmath），不要自己近似 —— 近似会让对比度判定
    // 在临界值附近抖动，而 4.5:1 是硬指标，差一点就是不合格。
    return std::pow((c + 0.055) / 1.055, 2.4);
}

[[nodiscard]] double Luminance(DWORD cr)
{
    // [R205] AARRGGBB：R 在高 8 位
    const unsigned int r = (cr >> 16) & 0xFF;
    const unsigned int g = (cr >> 8) & 0xFF;
    const unsigned int b = cr & 0xFF;
    return 0.2126 * LinearOf(r) + 0.7152 * LinearOf(g) + 0.0722 * LinearOf(b);
}

[[nodiscard]] double ContrastVsWhite(DWORD cr)
{
    const double l = Luminance(cr);
    return 1.05 / (l + 0.05);
}

}   // namespace

DWORD AccentColorRef()
{
    // [R205] 通道换算（本轮"主题色不正常"的根因）
    //
    //   注册表 AccentColor 是 0xAABBGGRR（Win32 COLORREF 序），
    //   而 DuiLib / XML 用 0xAARRGGBB。之前直接拿注册表低 24 位当
    //   AARRGGBB 用，等于把 R 和 B 对调：
    //   实测本机 AccentColor=0xFFD47800 应为 #0078D4（Windows 默认蓝），
    //   却被显示成 #D47800（橙）—— 用户看到的就是"颜色不是我的主题色"。
    auto BgrToRgb = [](DWORD cr) -> DWORD
    {
        const DWORD nR = cr & 0xFF;
        const DWORD nG = (cr >> 8) & 0xFF;
        const DWORD nB = (cr >> 16) & 0xFF;
        return (nR << 16) | (nG << 8) | nB;
    };

    // 1) 用户在"设置 > 个性化 > 颜色"里选的强调色（Win10 1607+ 同键）
    DWORD dwAccent = 0;
    if (ReadHkcuDword(L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", dwAccent)
        && (dwAccent & 0x00FFFFFF) != 0)
        return BgrToRgb(dwAccent & 0x00FFFFFF);

    // 2) 老路径：DwmGetColorizationColor 按文档返回 0xAARRGGBB，
    //    与注册表口径不同，**不换通道**。两者混用会再错一次。
    if (const PFN_DwmGetColorizationColor pfn = PfnGetColorization())
    {
        DWORD dwArgb = 0;
        BOOL bOpaque = FALSE;
        if (SUCCEEDED(pfn(&dwArgb, &bOpaque)) && (dwArgb & 0x00FFFFFF) != 0)
            return dwArgb & 0x00FFFFFF;
    }

    // 3) 项目默认：AARRGGBB 的 #007ACC（与皮肤历史取色一致）
    return 0x00007ACC;
}


DWORD AccentTextColorRef()
{
    // 强调色作为文字压在白底上时，原色常常达不到 4.5:1
    // （#007ACC 只有约 4.0:1）。这里保持色相不变，只按比例压暗明度，
    // 直到与白底对比度 >= 4.5:1 —— 与项目"不为对比度改色相"的既有口径一致。
    DWORD cr = AccentColorRef();
    for (int i = 0; i < 12 && ContrastVsWhite(cr) < 4.5; ++i)
    {
        // [R205] AccentColorRef 返回 AARRGGBB，通道拆分必须与之匹配
        //   （R=高8 / G=中 / B=低8）。此前按 BGR 拆分会把颜色改错。
        const unsigned int nR = (((cr >> 16) & 0xFF) * 9) / 10;
        const unsigned int nG = (((cr >> 8) & 0xFF) * 9) / 10;
        const unsigned int nB = ((cr & 0xFF) * 9) / 10;
        cr = (nR << 16) | (nG << 8) | nB;
    }
    return cr;
}

bool IsSystemDarkMode()
{
    DWORD dwLight = 1;
    if (!ReadHkcuDword(L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                       L"AppsUseLightTheme", dwLight))
        return false;   // 读不到按浅色：保守默认，避免把浅色界面画成暗色
    return dwLight == 0;
}

bool IsHighContrastMode()
{
    // HIGHCONTRASTW 结构：cbSize + dwFlags + lpszDefaultScheme
    HIGHCONTRASTW hc = {};
    hc.cbSize = sizeof(hc);
    if (!::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0))
        return false;   // 查询失败按关闭处理：宁可少一次换色，也不能误把普通模式切到高对比度
    return (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

DWORD HighContrastWindowColor()
{
    // GetSysColor 返回 COLORREF（0x00BBGGRR），与 DuiLib 需要的排列一致，
    // 只需补不透明 alpha。
    return 0xFF000000u | static_cast<DWORD>(::GetSysColor(COLOR_WINDOW));
}

DWORD HighContrastTextColor()
{
    return 0xFF000000u | static_cast<DWORD>(::GetSysColor(COLOR_WINDOWTEXT));
}

unsigned int ApplyWindowTheme(HWND hWnd, bool bDarkCaption)
{
    unsigned int nAbility = 0;
    if (!hWnd) return nAbility;

    const PFN_DwmSetWindowAttribute pfn = PfnSetAttr();
    if (!pfn) return nAbility;   // 无 dwmapi（极简 PE/Server Core）：整体跳过

    // [WIN1607-FIX] 加版本门控：明确不支持的 OS Build 直接跳过，
    // 避免 1607/1703 等老版本 Win10 上反复触发 E_INVALIDARG。

    // ---- 1) Win11 圆角 ----
    //   仅 Win11 22000+ 支持。Win10 全系（含最新 22H2）不识别此属性号。
    if (DuiLib::IsWindows11OrGreater())
    {
        const int nPref = kCornerRound;
        if (SUCCEEDED(pfn(hWnd, kDwmAttrCorner, &nPref, sizeof(nPref))))
            nAbility |= static_cast<unsigned int>(EThemeAbility::RoundedCorner);
    }

    // ---- 2) Mica 背景（Win11 22H2+）----
    //
    // [R204] 默认**不启用** Mica。
    //   原因：Mica 要客户区不完全不透明才能透出桌面采样，而本项目根节点
    //   bkcolor 是实心令牌色。两者叠加的表现是分区不一致 —— 实测同一窗口
    //   内存在两层颜色，且 Mica 是否生效在不同启动/不同机器上不一致，
    //   用户看到的是"颜色时好时坏"。
    //   优先保证配色一致与可预期；若要做 Mica，必须同时把根节点改透明
    //   并在客户区重绘，属整体皮肤改造，不是加一个 DWM 属性就能得到。
    constexpr bool kEnableMica = false;
    if (kEnableMica)
    {
        const int nBackdrop = kBackdropMica;
        if (SUCCEEDED(pfn(hWnd, kDwmAttrBackdrop, &nBackdrop, sizeof(nBackdrop))))
            nAbility |= static_cast<unsigned int>(EThemeAbility::Mica);
    }

    // ---- 3) 标题栏深色（跟随系统）----
    //   19 与 20 两个属性号都试：20 是正式值（1803+），19 覆盖 1809 预览版。
    //   1607/1703 上两个都没有，跳过避免无效调用。
    if (DuiLib::IsWindows10DarkModeOrGreater() || DuiLib::IsWindows11OrGreater())
    {
        BOOL bDark = bDarkCaption ? TRUE : FALSE;
        const bool bOk20 = SUCCEEDED(pfn(hWnd, kDwmAttrDarkMode, &bDark, sizeof(bDark)));
        if (!bOk20 && SUCCEEDED(pfn(hWnd, kDwmAttrDarkModeLegacy, &bDark, sizeof(bDark))))
            nAbility |= static_cast<unsigned int>(EThemeAbility::DarkCaption);
        else if (bOk20)
            nAbility |= static_cast<unsigned int>(EThemeAbility::DarkCaption);
    }

    // ---- 4) 强调色读取（不依赖 DWM 是否支持上述属性）----
    if ((AccentColorRef() & 0x00FFFFFF) != 0)
        nAbility |= static_cast<unsigned int>(EThemeAbility::AccentApplied);

    return nAbility;
}

std::wstring AbilityToText(unsigned int nAbility)
{
    std::wstring s;
    auto add = [&s](const wchar_t* psz)
    {
        if (!s.empty()) s += L"+";
        s += psz;
    };
    if (nAbility & static_cast<unsigned int>(EThemeAbility::RoundedCorner)) add(L"corner");
    if (nAbility & static_cast<unsigned int>(EThemeAbility::Mica))          add(L"mica");
    if (nAbility & static_cast<unsigned int>(EThemeAbility::DarkCaption))   add(L"dark");
    if (nAbility & static_cast<unsigned int>(EThemeAbility::AccentApplied)) add(L"accent");
    if (s.empty()) s = L"none";
    return s;
}

}   // namespace dwmtheme