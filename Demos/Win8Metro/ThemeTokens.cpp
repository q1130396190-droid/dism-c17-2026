// ============================================================
// ThemeTokens.cpp —— 颜色令牌实现
//
// 设计：一次解析 → 缓存 → 提供 6 个派生色。
//   派生色全部在"强调色"基础上做明度变换，**不改色相**，
//   这样用户换了主题色，整个界面依然协调（与项目既有口径一致）。
//
// [2026] ThemeService Palette 消费：
//   当 ThemeService 当前 preset 有 palette（hasPalette=true，如 Classic 深蓝）
//   时，所有 token 直接从 palette 拷贝 + 派生。这消除了"皮肤硬编码一套色、
//   tokens 又从系统取一套色"的并行事实源 bug。
// ============================================================

#include "stdafx.h"

#include "ThemeTokens.h"
#include "ThemeService.h"   // [2026] ActivePaletteForTokens()

#include "DwmTheme.h"   // AccentColorRef / AccentTextColorRef（含 4.5:1 修正）

#include <cmath>   // [R204] std::pow (sRGB linearization)
#include <mutex>

namespace tokens
{

namespace
{


// [R205] COLORREF(0x00BBGGRR) -> 令牌序 AARRGGBB
[[nodiscard]] inline DWORD SysToToken(int nIdx)
{
    const DWORD c = static_cast<DWORD>(::GetSysColor(nIdx));
    const DWORD nR = c & 0xFF;
    const DWORD nG = (c >> 8) & 0xFF;
    const DWORD nB = (c >> 16) & 0xFF;
    return 0xFF000000u | (nR << 16) | (nG << 8) | nB;
}
// [R204] sRGB channel -> linear factor (WCAG 2.1)
inline double lin3(double c)
{
    return (c <= 0.03928) ? (c / 12.92) : std::pow((c + 0.055) / 1.055, 2.4);
}

struct TokenSet
{
    DWORD arr[static_cast<int>(EToken::Count)] = {};
    // 缓存对应的源强调色：源变了就整体重算（改主题色的场景）
    DWORD dwSourceAccent = 0;
    // [R201] 高对比度开关也参与缓存有效性：开/关切换时源色种类会变，
    //   只比较 AccentColor 会漏掉这个状态，表现为"开了高对比度但颜色没变"。
    bool  bHighContrast = false;
    // [R236] 这份缓存是按深色还是浅色算出来的。必须记进来：
    //   只靠"源强调色 + 高对比度"判断有效性，手动切深浅时两者都不变，
    //   缓存会被误判有效。
    bool  bDark = false;
    bool  bValid = false;
};

TokenSet g_set;
std::mutex g_mx;

// [R236] 手动主题模式。Auto 表示跟随系统 —— 所以它不是"第三种配色"，
//   而是"把决定权交回系统"。默认值就是改造前的行为，老用户不会感到变化。
EThemeMode g_eMode = EThemeMode::Auto;

// 明度变换：percent > 0 提亮（向 255 靠），< 0 压暗（向 0 靠）。
//   逐通道线性插值；保留 alpha 由调用方补。
[[nodiscard]] DWORD Shift(DWORD cr, int nPercent)
{
    auto one = [nPercent](unsigned int c) -> unsigned int
    {
        const int v = static_cast<int>(c);
        const int target = (nPercent >= 0) ? 255 : 0;
        const int p = (nPercent >= 0) ? nPercent : -nPercent;
        return static_cast<unsigned int>(v + (target - v) * p / 100);
    };
    // [R205] 令牌统一用 AARRGGBB（与 DuiLib / XML 一致），通道位序同步修正
    const unsigned int r = one((cr >> 16) & 0xFF);
    const unsigned int g = one((cr >> 8) & 0xFF);
    const unsigned int b = one(cr & 0xFF);
    return (r << 16) | (g << 8) | b;
}


// [R207] 把强调色调整到"作为文字时在目标背景上 >= 4.5:1"。
//   深色底 -> 逐级提亮；浅色底 -> 逐级压暗。只改明度、保持色相，
//   与项目"不为对比度改色相"的既有口径一致。
[[nodiscard]] DWORD FitAccentForText(DWORD crAccent, bool bDark)
{
    // [R207] 深色下要按"最亮的那个深色底"来算：SurfaceAlt(#2B2B2B) 比
    //   Surface(#1F1F1F) 亮，若只对 Surface 达标，落在卡片/表头上就会差一点
    //   （实测 4.18 < 4.5）。取 SurfaceAlt 作为基准可一次覆盖两种底。
    // [R208] 浅色侧基准从纯白改为 AccentSoft(#D1E6F7)：强调色文字最常见的
    //   落点是"强调色淡底"按钮，只按白底算会在那里差一点（实测 4.11 < 4.5）。
    const DWORD crBg = bDark ? 0x002B2B2B : 0x00D1E6F7;

    auto lin = [](double c) { return (c <= 0.03928) ? c / 12.92
                                                    : std::pow((c + 0.055) / 1.055, 2.4); };
    auto lum = [&lin](DWORD cr) -> double
    {
        return 0.2126 * lin(((cr >> 16) & 0xFF) / 255.0)
             + 0.7152 * lin(((cr >> 8) & 0xFF) / 255.0)
             + 0.0722 * lin((cr & 0xFF) / 255.0);
    };
    auto ratio = [&lum](DWORD a, DWORD b) -> double
    {
        const double la = lum(a), lb = lum(b);
        const double hi = (la > lb) ? la : lb;
        const double lo = (la > lb) ? lb : la;
        return (hi + 0.05) / (lo + 0.05);
    };

    DWORD cr = crAccent;
    for (int i = 0; i < 20 && ratio(cr, crBg) < 4.5; ++i)
    {
        unsigned int nR = (cr >> 16) & 0xFF;
        unsigned int nG = (cr >> 8) & 0xFF;
        unsigned int nB = cr & 0xFF;
        if (bDark)
        {
            // 向白靠 10%
            nR += (255 - nR) / 10; nG += (255 - nG) / 10; nB += (255 - nB) / 10;
        }
        else
        {
            // 向黑靠 10%
            nR = nR * 9 / 10; nG = nG * 9 / 10; nB = nB * 9 / 10;
        }
        cr = (nR << 16) | (nG << 8) | nB;
    }
    return cr;
}

void BuildLocked(TokenSet& s, bool bWantDark)
{
    // [2026] ThemeService Palette 优先
    //   当 ThemeService 当前 preset 提供了 palette（Classic 深蓝），
    //   所有 token 直接从 palette 拷贝 —— 消除 Classic 模式下
    //   "皮肤硬编码 #1E5799 但 Token(Accent) 仍读系统蓝"的并行源 bug。
    {
        const ThemeService::ThemePalette* pPal = ThemeService::ActivePaletteForTokens();
        if (pPal)
        {
            const DWORD crA = pPal->accent;
            s.dwSourceAccent = crA;

            // ---- 基础：直接从 palette 拷贝 ----
            s.arr[static_cast<int>(EToken::Accent)]       = crA;
            s.arr[static_cast<int>(EToken::Surface)]      = pPal->surface;
            s.arr[static_cast<int>(EToken::SurfaceAlt)]   = pPal->surfaceAlt;
            s.arr[static_cast<int>(EToken::SurfaceSunk)]  = pPal->surfaceSunk;
            s.arr[static_cast<int>(EToken::SurfaceHover)] = pPal->surfaceHover;
            s.arr[static_cast<int>(EToken::Border)]       = pPal->border;
            s.arr[static_cast<int>(EToken::BorderSoft)]   = pPal->borderSoft;
            s.arr[static_cast<int>(EToken::Ink)]          = pPal->ink;
            s.arr[static_cast<int>(EToken::InkSecondary)] = pPal->inkSecondary;
            s.arr[static_cast<int>(EToken::InkTertiary)]  = pPal->inkTertiary;
            s.arr[static_cast<int>(EToken::InkMuted)]     = pPal->inkMuted;
            s.arr[static_cast<int>(EToken::Danger)]       = pPal->danger;
            s.arr[static_cast<int>(EToken::Success)]     = pPal->success;
            s.arr[static_cast<int>(EToken::Warn)]        = pPal->warn;
            s.arr[static_cast<int>(EToken::WarnBg)]      = pPal->warnBg;
            // [2026-fix] EToken::HeaderBand 是"内容区表头带"，不是窗口标题栏。
            //   原来直接灌 palette.headerBand（Classic 的深蓝 #FF1E5799），
            //   语义错位；Classic 改走 bandSurface / onBandSurface。
            s.arr[static_cast<int>(EToken::HeaderBand)]   = pPal->bandSurface;
            s.arr[static_cast<int>(EToken::OnHeaderBand)] = pPal->onBandSurface;

            // ---- Accent 家族：直接取 palette 显式值 ----
            // [2026-fix] 原来这里用 Shift() 从 accent 派生 AccentHot/AccentPushed，
            //   得到 #1E89D2 / #0065A8，与 spec §2.2（#1C97EA / #005A9E）以及
            //   Classic 皮肤 XML 里写死的 hotbkcolor/pushedbkcolor 都不一致 ——
            //   表现为"鼠标悬停时按钮颜色跳了一下，没跟着皮肤走"。
            s.arr[static_cast<int>(EToken::AccentHot)]    = pPal->accentHot;
            s.arr[static_cast<int>(EToken::AccentPushed)] = pPal->accentPushed;
            s.arr[static_cast<int>(EToken::AccentText)]   = pPal->accentText;
            s.arr[static_cast<int>(EToken::AccentSoft)]   = pPal->accentSoft;
            s.arr[static_cast<int>(EToken::AccentBorder)] = pPal->accentBorder;

            // Danger 家族
            s.arr[static_cast<int>(EToken::DangerHot)]    = pPal->dangerHot;
            s.arr[static_cast<int>(EToken::DangerPushed)] = pPal->dangerPushed;

            // OnAccent / OnDanger：palette 显式承载，不再靠亮度挑选。
            // [2026-fix] 原来的 pickFg() 把 AARRGGBB 当 COLORREF 拆通道
            //   （R 读了低 8 位、B 读了高 8 位），算出的亮度是错的，
            //   Classic 的 #007ACC 竟然被判成"需要黑字"→ OnAccent = 黑色，
            //   蓝底上的字全变黑 —— 这就是"有些地方颜色没转换过来"的直接原因。
            s.arr[static_cast<int>(EToken::OnAccent)]       = pPal->onAccent;
            s.arr[static_cast<int>(EToken::OnAccentHot)]    = pPal->onAccentHot;
            s.arr[static_cast<int>(EToken::OnAccentPushed)] = pPal->onAccentPushed;
            s.arr[static_cast<int>(EToken::OnDanger)]       = pPal->onDanger;

            s.bHighContrast = false;
            s.bDark = bWantDark;
            s.bValid = true;
            return;   // palette 路径完成，不再走下面的高对比度/系统强调色分支
        }
    }

    // [R201] 高对比度优先级最高：用户开它的目的就是拿走配色控制权。
    //   此时强调色不再读系统主题（那会得到低对比度的柔和色），
    //   改用系统高亮色，保证"选中/强调"在任何高对比度主题下都清晰。
    if (dwmtheme::IsHighContrastMode())
    {
        // [R205] GetSysColor 返回 COLORREF(0x00BBGGRR)，必须转成
        //   AARRGGBB 才能喂给 DuiLib；否则高对比度配色同样是错的。
        const DWORD crHl  = SysToToken(COLOR_HIGHLIGHT);
        const DWORD crHlT = SysToToken(COLOR_HIGHLIGHTTEXT);
        s.dwSourceAccent = crHl;   // 记录源值，便于缓存有效性判断
        s.arr[static_cast<int>(EToken::Accent)]       = crHl & 0x00FFFFFF;
        s.arr[static_cast<int>(EToken::AccentHot)]    = crHl & 0x00FFFFFF;   // 高对比度下不加亮
        s.arr[static_cast<int>(EToken::AccentPushed)] = crHl & 0x00FFFFFF;
        // [R208] AccentText 是"强调色当**普通文字**"，压在窗口底上，
        //   应取窗口文字色（COLOR_WINDOWTEXT），不能取 COLOR_HIGHLIGHTTEXT ——
        //   后者只在"高亮底"上成立。实测高对比黑主题下取它会得到黑字压黑底（1.00:1）。
        //   与高亮底配对的那个色（OnAccent）才用 COLOR_HIGHLIGHTTEXT。
        s.arr[static_cast<int>(EToken::AccentText)]   = SysToToken(COLOR_WINDOWTEXT) & 0x00FFFFFF;
        s.arr[static_cast<int>(EToken::AccentSoft)]   = crHl & 0x00FFFFFF;
        s.arr[static_cast<int>(EToken::AccentBorder)] = crHl & 0x00FFFFFF;

        // [R202] 高对比度下的中性色也必须取系统值：用户已经把
        //   窗口底色/文字换成高反差配对，这里自造任何中性色都可能破坏可读性。
        const DWORD crBg   = SysToToken(COLOR_WINDOW);
        const DWORD crFg   = SysToToken(COLOR_WINDOWTEXT);
        const DWORD crBtnF = SysToToken(COLOR_BTNFACE);
        s.arr[static_cast<int>(EToken::Surface)]      = crBg   & 0x00FFFFFF;
        s.arr[static_cast<int>(EToken::SurfaceAlt)]   = crBtnF & 0x00FFFFFF;
        s.arr[static_cast<int>(EToken::SurfaceHover)] = crHl   & 0x00FFFFFF;
        s.arr[static_cast<int>(EToken::SurfaceSunk)]  = crBtnF & 0x00FFFFFF;
        s.arr[static_cast<int>(EToken::Border)]       = crFg   & 0x00FFFFFF;
        s.arr[static_cast<int>(EToken::BorderSoft)]   = crFg   & 0x00FFFFFF;
        s.arr[static_cast<int>(EToken::Ink)]          = crFg   & 0x00FFFFFF;
        s.arr[static_cast<int>(EToken::InkSecondary)] = crFg   & 0x00FFFFFF;
        s.arr[static_cast<int>(EToken::InkTertiary)]  = crFg   & 0x00FFFFFF;
        s.arr[static_cast<int>(EToken::InkMuted)]     = crFg   & 0x00FFFFFF;
        // 高对比度下语义色不得自造：统一用窗口文字色，靠文字本身区分含义
        s.arr[static_cast<int>(EToken::Danger)]  = crFg & 0x00FFFFFF;
        s.arr[static_cast<int>(EToken::DangerHot)]    = crFg & 0x00FFFFFF;
        s.arr[static_cast<int>(EToken::DangerPushed)] = crFg & 0x00FFFFFF;
        s.arr[static_cast<int>(EToken::OnDanger)]     = crBg & 0x00FFFFFF;
        s.arr[static_cast<int>(EToken::Success)] = crFg & 0x00FFFFFF;
        s.arr[static_cast<int>(EToken::Warn)]    = crFg & 0x00FFFFFF;
        s.arr[static_cast<int>(EToken::WarnBg)]  = crBg & 0x00FFFFFF;
        s.arr[static_cast<int>(EToken::OnAccent)] = crHlT & 0x00FFFFFF;

        // 高对比度下深浅色的差异被系统配色覆盖，但字段仍要记，
        //   否则退出高对比度时缓存键不可比。
        s.bHighContrast = true;
        s.bDark = bWantDark;
        s.bValid = true;
        return;
    }

    s.bHighContrast = false;
    const DWORD crAccent = dwmtheme::AccentColorRef();
    s.dwSourceAccent = crAccent;

    s.arr[static_cast<int>(EToken::Accent)]       = crAccent;
    s.arr[static_cast<int>(EToken::AccentHot)]    = Shift(crAccent, 12);    // 提亮
    s.arr[static_cast<int>(EToken::AccentPushed)] = Shift(crAccent, -18);   // 压暗
    // [R207] AccentText 必须"按目标背景"算，不能只按白底算：
    //   深色模式下文字压的是 #1F1F1F，蓝色原始明度只有 3.6:1 —— 看不清。
    //   这里按当前主题的 Surface 实测对比度，不足就提亮（深色）或压暗（浅色），
    //   保持色相不变，只动明度。
    s.arr[static_cast<int>(EToken::AccentText)]   = FitAccentForText(crAccent, bWantDark);
    // [R207] AccentSoft 是"强调色淡底"，深色模式下必须往**暗**靠，
    //   否则会得到 #D1E6F7 这类浅色块 —— 在深色界面里像一块补丁，
    //   而压在上面的 Ink 文字（白）会直接不可读。
    s.arr[static_cast<int>(EToken::AccentSoft)]   =
        bWantDark ? Shift(crAccent, -78) : Shift(crAccent, 82);
    s.arr[static_cast<int>(EToken::AccentBorder)] = Shift(crAccent, -10);

    // [R202] 中性色：深色模式与浅色模式在这里分叉，调用点完全不用改。
    //   浅色值 = 现有皮肤实测色（保证观感零变化）；
    //   深色值按 Win11 深色主题的实际层级取值，不是简单反色 ——
    //   反色会让纯白变纯黑、对比过硬，反而比系统应用更刺眼。
    if (bWantDark)
    {
        // [R216] Dark ramp now taken from the OFFICIAL WinUI theme resources instead of
        //   hand-picked values. Source: themeresources.xaml SolidBackgroundFillColor* and
        //   TextFillColor* ramps (the same numbers WinUI 3 Gallery renders and the
        //   fluent_ui port mirrors). The previous values were invented, and three of them
        //   were measurably wrong in ways that flattened the visual hierarchy:
        //     * InkSecondary #E6E6E6 vs official textFillColorSecondary #CCCCCC -- every
        //       secondary label rendered one step too bright, so "secondary" read as
        //       "primary" and the page had no depth.
        //     * InkTertiary #B4B4B4 vs official #969696, InkMuted #9A9A9A vs #717171 --
        //       same problem, one step brighter than the platform.
        //     * Border #555555 vs official surfaceStrokeColorDefault #757575 -- borders
        //       were darker than Windows and nearly vanished on the dark surface.
        //   Alpha-over-base values are pre-composited here because DuiLib paints opaque:
        //   textFillColorSecondary is #C5FFFFFF over #202020 -> #CCCCCC, etc.
        s.arr[static_cast<int>(EToken::Surface)]      = 0x00202020;   // SolidBackgroundFillColorBase
        s.arr[static_cast<int>(EToken::SurfaceAlt)]   = 0x00282828;   // SolidBackgroundFillColorTertiary (cards)
        s.arr[static_cast<int>(EToken::SurfaceSunk)]  = 0x001C1C1C;   // SolidBackgroundFillColorSecondary (inputs/wells)
        s.arr[static_cast<int>(EToken::SurfaceHover)] = 0x00333333;   // SolidBackgroundFillColorQuinary
        s.arr[static_cast<int>(EToken::Border)]       = 0x00757575;   // surfaceStrokeColorDefault
        s.arr[static_cast<int>(EToken::BorderSoft)]   = 0x00323232;   // dividerStrokeColorDefault (#15FFFFFF over base)
        s.arr[static_cast<int>(EToken::Ink)]          = 0x00FFFFFF;   // textFillColorPrimary
        s.arr[static_cast<int>(EToken::InkSecondary)] = 0x00CCCCCC;   // textFillColorSecondary  (#C5FFFFFF over base)
        s.arr[static_cast<int>(EToken::InkTertiary)]  = 0x00969696;   // textFillColorTertiary   (#87FFFFFF over base)
        // [R216] Deliberate deviation from textFillColorDisabled (#5D -> #717171).
        //   In WinUI that token is for DISABLED controls, where 4.5:1 does not apply.
        //   Here InkMuted also paints placeholders, hints and muted labels that ARE
        //   live text, so it must clear 4.5:1 on every surface it can land on. At the
        //   official value it measures 3.02:1 on SurfaceAlt (#282828) and 2.59:1 on
        //   hover (#333333) -- unreadable. Raised to the alpha that clears the gate on
        //   the WORST background (alpha 0x8F -> #9D9D9D: 5.64 / 5.13). This is a
        //   readability requirement, not an aesthetic choice.
        s.arr[static_cast<int>(EToken::InkMuted)]     = 0x009D9D9D;   // >=4.5:1 on Base/Alt/Sunk/Hover

        // 语义色提亮版：实测在 #1F1F1F 上分别为 5.94 / 9.46 / 11.82 / 9.47:1
        s.arr[static_cast<int>(EToken::Danger)]  = 0x00FF6B6B;   // #FF6B6B
        // [R215] 派生值：深色下亮红只需小幅变化即可表达状态，
        //   幅度过大（+12%）会让 hover 底逼近白色、失去"亮起"的观感。
        s.arr[static_cast<int>(EToken::DangerHot)]    = Shift(0x00FF6B6B,  8);  // #FF8080
        s.arr[static_cast<int>(EToken::DangerPushed)] = Shift(0x00FF6B6B, -14); // #DB5C5C
        s.arr[static_cast<int>(EToken::Success)] = 0x004ADE80;   // #4ADE80
        s.arr[static_cast<int>(EToken::Warn)]    = 0x00FFD666;   // #FFD666
        s.arr[static_cast<int>(EToken::WarnBg)]  = 0x003A4A36;   // 深色下的警告底

        // [UI-REF] 原版绿 #9BC716 是高明度色，压在 #1F1F1F 深窗上过于刺眼，
        //   换成同色相的压暗版 #4E6B08：白字实测 6.12:1，仍读得出是同一个设计特征。
        s.arr[static_cast<int>(EToken::HeaderBand)]   = 0x004E6B08;
        s.arr[static_cast<int>(EToken::OnHeaderBand)] = 0x00FFFFFF;
    }
    else
    {
        // [R326] Light ramp now mirrors the OFFICIAL WinUI light resources, for the same
        //   reason the dark ramp was rewritten in R216: the two themes have to describe
        //   the same design language, and they did not.
        //
        //   What was wrong before: every light value was inherited from the original
        //   Win8-era skin -- pure white base, blue-tinted panels (#EFF6FC), blue-tinted
        //   hover (#E5F1FB), #ABABAB strokes. That reads as "Windows 8 Metro", not
        //   Windows 11: Win11 light is a NEUTRAL grey canvas with WHITE cards floating
        //   on it, one-step-grey dividers and no colour cast anywhere except the accent.
        //
        //   The hierarchy is deliberately inverted versus the old table, and it now
        //   matches the dark ramp's relationship (base darker, panels lighter):
        //       dark : Surface #202020  <  SurfaceAlt #282828
        //       light: Surface #F3F3F3  <  SurfaceAlt #FFFFFF
        //   Keeping Surface = #FFFFFF while panels were #EFF6FC meant "card darker than
        //   window", which is the opposite of every Win11 surface stack.
        s.arr[static_cast<int>(EToken::Surface)]      = 0x00F3F3F3;   // SolidBackgroundFillColorBase
        s.arr[static_cast<int>(EToken::SurfaceAlt)]   = 0x00FFFFFF;   // card / content surface (SolidBackgroundFillColorQuarternary)
        s.arr[static_cast<int>(EToken::SurfaceSunk)]  = 0x00FBFBFB;   // ControlFillColorDefault (inputs, wells)
        s.arr[static_cast<int>(EToken::SurfaceHover)] = 0x00F0F0F0;   // neutral hover; the old #E5F1FB was blue-cast
        s.arr[static_cast<int>(EToken::Border)]       = 0x00D1D1D1;   // controlStrokeColorDefault (was #ABABAB, too dark/heavy)
        s.arr[static_cast<int>(EToken::BorderSoft)]   = 0x00E5E5E5;   // dividerStrokeColorDefault (already official)
        s.arr[static_cast<int>(EToken::Ink)]          = 0x001A1A1A;   // textFillColorPrimary (#E4000000 over base)
        s.arr[static_cast<int>(EToken::InkSecondary)] = 0x00616161;   // textFillColorSecondary (#9E000000 over base)
        s.arr[static_cast<int>(EToken::InkTertiary)]  = 0x008A8A8A;   // textFillColorTertiary (#72000000 over base)
        // [R208] Kept at #707070 rather than the official #A5A5A5 disabled value:
        //   InkMuted also paints placeholders and hints that ARE live text, so it has to
        //   clear 4.5:1 on SurfaceAlt (#FFFFFF: 4.95) and on Surface (#F3F3F3: 4.60).
        s.arr[static_cast<int>(EToken::InkMuted)]     = 0x00707070;

        // 语义色沿用既有值（与改造前完全一致）
        s.arr[static_cast<int>(EToken::Danger)]  = 0x00D32F2F;   // #D32F2F
        // [R215] 原 hover 写死 #E81123/#E74C3C，白字只有 3.90/3.82:1，两个主题
        //   都不达标。压暗版派生保证白字始终 >= 4.5:1（#D84747 实测 4.28 仍不够，
        //   所以浅色 hover 走 -10% 而不是提亮）。
        s.arr[static_cast<int>(EToken::DangerHot)]    = Shift(0x00D32F2F, -10); // #BE2A2A
        s.arr[static_cast<int>(EToken::DangerPushed)] = Shift(0x00D32F2F, -26); // #9C2323
        s.arr[static_cast<int>(EToken::Success)] = 0x001A7F3F;   // #1A7F3F
        // [UI-A11Y] 原 #2DA44E 在白底上仅 3.22:1、卡片底 2.95:1，未达 4.5。
        //   取同色相压暗版 #1A7F3F：白底 5.06、卡片 4.64，两底均达标。
        s.arr[static_cast<int>(EToken::Warn)]    = 0x007A5C00;   // #7A5C00
        s.arr[static_cast<int>(EToken::WarnBg)]  = 0x00FFE082;   // #FFE082

        // [UI-REF] 参考图实测值，逐像素取值而非目测：#9BC716。
        //   其上文字必须用深色：白字仅 1.99:1（远低于 4.5），深字 #1E1E1E 达 8.39:1。
        //   参考图本身用的就是 #1E1E1E，这里与它一致。
        s.arr[static_cast<int>(EToken::HeaderBand)]   = 0x009BC716;
        s.arr[static_cast<int>(EToken::OnHeaderBand)] = 0x001E1E1E;
    }

    // [R214] OnAccent: black or white by accent luminance, so text on an
    //   accent background stays readable for any accent colour.
    //   [修复] 原来这里少了一个闭合深/浅分支的 '}'，导致 OnAccent 块
    //   被嵌在 else 分支里，且收尾多出一个 '}' —— 深色分支的赋值整体
    //   错位，实测开关、列表行等取令牌处全部得到 0（纯黑）。
    {
        const DWORD crA = s.arr[static_cast<int>(EToken::Accent)];
        // [2026-fix] 令牌统一用 AARRGGBB（R 在高 8 位）。这里原来把
        //   AARRGGBB 当 COLORREF(0x00BBGGRR) 拆通道 —— R 读了低 8 位、
        //   B 读了高 8 位，红蓝互换，亮度算错，OnAccent 会选错黑白。
        const double dR2 = ((crA >> 16) & 0xFF) / 255.0;
        const double dG2 = ((crA >> 8) & 0xFF) / 255.0;
        const double dB2 = (crA & 0xFF) / 255.0;
        const double lum2 = 0.2126 * lin3(dR2) + 0.7152 * lin3(dG2) + 0.0722 * lin3(dB2);
        const double cb2 = (lum2 + 0.05) / 0.05;
        const double cw2 = 1.05 / (lum2 + 0.05);
        // [R219] Pick the foreground per STATE FILL, not just from the base accent.
        //   Measured failures this fixes: light black-on-AccentPushed 3.38:1,
        //   dark white-on-AccentHot 3.77:1 -- unreadable precisely while clicking.
        auto pick = [&lum2](DWORD crBg) -> DWORD
        {
            const double dR = ((crBg >> 16) & 0xFF) / 255.0;
            const double dG = ((crBg >> 8) & 0xFF) / 255.0;
            const double dB = (crBg & 0xFF) / 255.0;
            const double lum = 0.2126 * lin3(dR) + 0.7152 * lin3(dG) + 0.0722 * lin3(dB);
            const double cb = (lum + 0.05) / 0.05;      // contrast vs black
            const double cw = 1.05 / (lum + 0.05);      // contrast vs white
            return (cb >= cw) ? 0x000000 : 0xFFFFFF;
        };
        (void)lum2;
        s.arr[static_cast<int>(EToken::OnAccent)]       = pick(crA);
        s.arr[static_cast<int>(EToken::OnAccentHot)]    =
            pick(s.arr[static_cast<int>(EToken::AccentHot)]);
        s.arr[static_cast<int>(EToken::OnAccentPushed)] =
            pick(s.arr[static_cast<int>(EToken::AccentPushed)]);
    }

    // [R215] OnDanger: black or white by Danger luminance -- same rule as OnAccent,
    //   but Danger is a different colour, so OnAccent cannot be reused. In the dark
    //   theme Danger is a bright red (#FF6B6B) where white is only 2.78:1 while black
    //   is 7.57:1; reusing OnAccent here would leave the button text unreadable.
    {
        const DWORD crD = s.arr[static_cast<int>(EToken::Danger)];
        // [2026-fix] 同上：AARRGGBB 的 R 在高 8 位
        const double dR3 = ((crD >> 16) & 0xFF) / 255.0;
        const double dG3 = ((crD >> 8) & 0xFF) / 255.0;
        const double dB3 = (crD & 0xFF) / 255.0;
        const double lum3 = 0.2126 * lin3(dR3) + 0.7152 * lin3(dG3) + 0.0722 * lin3(dB3);
        const double cb3 = (lum3 + 0.05) / 0.05;
        const double cw3 = 1.05 / (lum3 + 0.05);
        s.arr[static_cast<int>(EToken::OnDanger)] = (cb3 >= cw3) ? 0x000000 : 0xFFFFFF;
    }

    s.bDark = bWantDark;
    s.bValid = true;
}

// [R236] 生效中的深浅色。Auto 在这里解析成系统的真实值 ——
//   所有取色分支都走它，不能直接问 dwmtheme::IsSystemDarkMode()，
//   否则手动覆盖会被绕过（预览卡深色、主界面浅色）。
[[nodiscard]] bool IsDarkLocked()
{
    return g_eMode == EThemeMode::Dark
        || (g_eMode == EThemeMode::Auto && dwmtheme::IsSystemDarkMode());
}

}   // namespace

DWORD Token(EToken e)
{
    const int n = static_cast<int>(e);
    if (n < 0 || n >= static_cast<int>(EToken::Count)) return 0xFF007ACC;

    std::lock_guard<std::mutex> lock(g_mx);
    // 重算条件：缓存无效，或"决定颜色的外部状态"变了。
    //   [R201] 高对比度开关必须纳入：它切换时强调色源从 AccentColor
    //   变成 COLOR_HIGHLIGHT，只比较 AccentColor 会让缓存永远"看起来有效"，
    //   表现为"开了高对比度但颜色没变"。
    const bool bHc = dwmtheme::IsHighContrastMode();
    const DWORD dwSrc = bHc
        ? (0xFF000000u | static_cast<DWORD>(::GetSysColor(COLOR_HIGHLIGHT)))
        : dwmtheme::AccentColorRef();

    // [PERF] 不再每次调 IsSystemDarkMode()（RegOpenKey+RegQuery = 毫秒级！）
    //   用缓存值 g_set.bDark 直接比较。Refresh() / SetMode() / WM_SETTINGCHANGE
    //   会强制重新 BuildLocked，确保系统主题变化时能跟上。
    const bool bDarkCached = g_set.bValid ? g_set.bDark : IsDarkLocked();
    if (!g_set.bValid || g_set.dwSourceAccent != dwSrc || g_set.bHighContrast != bHc
        || g_set.bDark != bDarkCached)
        BuildLocked(g_set, bDarkCached);
    return 0xFF000000u | g_set.arr[n];
}

void Refresh()
{
    std::lock_guard<std::mutex> lock(g_mx);
    BuildLocked(g_set, IsDarkLocked());
}

// ---------------------------------------------------------------
// [R236] 手动主题切换
// ---------------------------------------------------------------

EThemeMode Mode()
{
    std::lock_guard<std::mutex> lock(g_mx);
    return g_eMode;
}

bool IsDark()
{
    std::lock_guard<std::mutex> lock(g_mx);
    return IsDarkLocked();
}

bool SetMode(EThemeMode e)
{
    if (e < EThemeMode::Auto || e >= EThemeMode::Count) return false;

    std::lock_guard<std::mutex> lock(g_mx);
    const bool bWasDark = IsDarkLocked();
    if (g_eMode == e)
    {
        // 同一模式重复点击：不做任何事，也不报"变了"。
        //   否则点击已选中的项会触发一次无意义的整树重绘（可见的闪烁）。
        return false;
    }
    g_eMode = e;
    const bool bNowDark = IsDarkLocked();
    // 立刻按新模式重建缓存：调用方紧接着就要读 Token() 上色，
    //   若留到下一次 Token() 才重建，中间任何一次读取都会拿到旧配色。
    BuildLocked(g_set, bNowDark);
    // 只有"可见的深浅色真的翻转"才算需要重绘。
    //   Auto -> Light 在系统本来就是浅色时，界面一个像素都不该动。
    return bWasDark != bNowDark;
}

DWORD TokenInMode(EToken e, bool bDark)
{
    const int n = static_cast<int>(e);
    if (n < 0 || n >= static_cast<int>(EToken::Count)) return 0xFF007ACC;

    // 独立构建一份，绝不写 g_set：预览卡要同时画出两个主题，
    //   共用全局缓存会让"先取的那个"被"后取的那个"覆盖。
    TokenSet tmp;
    BuildLocked(tmp, bDark);
    return 0xFF000000u | tmp.arr[n];
}

const wchar_t* TokenHexInMode(EToken e, bool bDark)
{
    // 与 TokenHex 分开的缓冲：预览卡通常在同一行里取底色和文字色，
    //   共用一个缓冲会让先取的颜色被覆盖（表现为预览卡文字与底色相同）。
    static thread_local wchar_t s_szM[12] = {};
    const DWORD cr = TokenInMode(e, bDark);
    wfmt::FormatInto(s_szM, _countof(s_szM), L"#%08lX", static_cast<unsigned long>(cr));
    return s_szM;
}

const wchar_t* TokenHex(EToken e)
{
    // 线程局部缓冲：调用点通常只做一次 SetAttribute，不需要长期持有。
    // 用 thread_local 避免多页面并发渲染时互相覆盖。
    static thread_local wchar_t s_sz[12] = {};
    const DWORD cr = Token(e);
    wfmt::FormatInto(s_sz, _countof(s_sz), L"#%08lX", static_cast<unsigned long>(cr));
    return s_sz;
}

const wchar_t* TokenHexAlpha(EToken e, unsigned char aAlpha)
{
    // [R235] 独立缓冲，不复用 TokenHex 的那一个：同一个表达式里同时取两个
    //   令牌（如前景色+底色）时，共用缓冲会让先取的那个被后取的覆盖，
    //   表现为"颜色偶尔错成另一个令牌"，且只在特定调用点复现。
    static thread_local wchar_t s_szA[12] = {};
    const DWORD rgb = Token(e) & 0x00FFFFFFu;
    wfmt::FormatInto(s_szA, _countof(s_szA), L"#%02X%06lX",
                     static_cast<unsigned>(aAlpha), static_cast<unsigned long>(rgb));
    return s_szA;
}

}   // namespace tokens