#pragma once

// ============================================================
// ThemeTokens.h —— 颜色令牌（唯一取色入口）
//
// 为什么需要：
//   报告实测：全项目硬编码色值 1124 处（XML 817 + 代码 307），去重后仅 90 种。
//   其中强调色 #007ACC 出现 74 处 —— 用户改了 Windows 主题色，界面不变，
//   这就是"观感陈旧"的直接来源。
//
// 本模块只做一件事：**把"强调色"从常量变成运行时查表**。
//   不追求一次性替换 1124 处（那是 P2 的全量令牌化），而是先让
//   "用户可见的蓝色"全部可跟随系统 —— 收益最大、风险最小。
//
// 与 P2 的关系：
//   P2 会把下面这些令牌扩展到背景/文字/边框，并加浅色/深色两套值；
//   届时调用方不需要改 —— 仍然只调 Token(EAccent)，只是内部多了主题分支。
//
// Win10 兼容：
//   强调色读的是 HKCU\Software\Microsoft\Windows\DWM\AccentColor，
//   Win10 1607+ 与 Win11 同样存在该键，无需版本分支。
// ============================================================

#include <windows.h>

namespace tokens
{

// [R201] 当前使用中的令牌种类。
//   命名按"用途"而非"颜色"：以后换成深色主题时，调用点语义不变。
enum class EToken : int
{
    Accent        = 0,   // 强调色：按钮底、选中行底、进度条前景
    AccentHot     = 1,   // 悬停：强调色提亮约 12%
    AccentPushed  = 2,   // 按下：强调色压暗约 18%
    AccentText    = 3,   // 强调色作为**文字**压在白底上（保证 4.5:1）
    AccentSoft    = 4,   // 强调色淡底：选中行浅背景、信息条
    AccentBorder  = 5,   // 强调色描边

    // [R202] 背景/文字/边框三组中性色：深色模式的全部差异都收敛在这里。
    //   浅色值取自现有皮肤的实测色值（#FFFFFFFF / #FF1A1A1A / #FFABABAB），
    //   因此浅色模式下的观感与改造前**完全一致** —— 这是敢全量替换的前提。
    Surface       = 6,   // 窗口/列表底色（原 #FFFFFFFF）
    SurfaceAlt    = 7,   // 次级底色：表头、状态条（原 #FFEFF6FC / #FFFAFAFA）
    SurfaceHover  = 8,   // 悬停底色（原 #FFE5F1FB）
    SurfaceSunk   = 9,   // 凹陷区：输入框、进度槽（原 #FFE8E8E8）
    Border        = 10,  // 常规边框（原 #FFABABAB）
    BorderSoft    = 11,  // 淡分割线（原 #FFE5E5E5）
    Ink           = 12,  // 主文字（原 #FF1A1A1A）
    InkSecondary  = 13,  // 次文字（原 #FF333333）
    InkTertiary   = 14,  // 三文字/提示（原 #FF666666）
    InkMuted      = 15,  // 占位/禁用（原 #FF999999）

    // [R203] 语义色。浅色沿用既有值（观感不变），深色换成提亮版 ——
    //   原因是深底上原色偏暗：实测 #7A5C00 在 #1F1F1F 上只有 2.64:1。
    Danger        = 16,  // 风险/错误（原 #FFD32F2F / #FFCC0000）
    Success       = 17,  // 成功/已启用（原 #FF2DA44E）
    Warn          = 18,  // 警告文字（原 #FF7A5C00）
    WarnBg        = 19,  // 警告底色（原 #FFFFE082）

    // [R204] 压在强调色上的文字色。原来皮肤里用 {tok:Surface} 充当，
    //   浅色下恰好是白色所以"看起来对"，深色下 Surface=#1F1F1F 就成了
    //   深字压深底 —— 选中项文字直接消失。必须单独成令牌。
    OnAccent      = 20,

    // [UI-REF] 表头/页签带底色。#9BC716 取自界面参考图的实测像素值
    //   （4 个页面出现：Appx 页签带、系统优化列头、程序功能列头）。
    //   这是原版的**局部控件特征**，不是全局主题色，所以单独成令牌而不是改 SurfaceAlt。
    //   深色主题下原色在深窗上过于刺眼，换成同色相的压暗版。
    HeaderBand    = 21,
    // 压在 HeaderBand 上的文字。浅色下绿底必须用深字（白字仅 1.99:1，不可读）。
    OnHeaderBand  = 22,

    // [R215] Danger 的悬停/按下/前景三件套。原来危险按钮在皮肤里写死了
    //   #E81123/#F03A3E/#C50F1F 与 #C0392B/#E74C3C/#96281B 两组红，
    //   实测白字压 hover 底只有 3.90:1 与 3.82:1 —— 两个主题都不达标，
    //   等于"鼠标一移上去文字就变淡"。且深色主题下这组亮红完全没有收敛，
    //   与检查器/更新页的其它元素割裂。按 Accent 的既有模式补三件套：
    //   底色由 Danger 派生，前景由底色亮度二选一（深色下亮红必须配黑字，
    //   白字在 #FF6B6B 上仅 2.78:1）。
    DangerHot     = 23,
    DangerPushed  = 24,
    OnDanger      = 25,

    // [R219] OnAccent is picked from the BASE accent colour, but a button's fill
    //   changes on hover (light1) and press (dark1). Measured: black on AccentPushed
    //   was 3.38:1 in light, and white on AccentHot was 3.77:1 in dark -- unreadable
    //   exactly while the user is clicking. The foreground must follow the state fill.
    OnAccentHot    = 26,
    OnAccentPushed = 27,

    Count         = 28
};

// [R236] 主题模式：浅色 / 深色 / 跟随系统。
//   为什么要"手动"这一层：在这之前深浅色只由 dwmtheme::IsSystemDarkMode()
//   决定，用户想单独把这个程序切成深色（系统保持浅色）没有任何入口，
//   标题栏也就没有可切换的东西可放。三档而不是两档，是因为"跟随系统"
//   是绝大多数人想要的默认行为，一旦把它换成纯手动，系统换主题时程序
//   会静止不动 —— 那是比不能切更糟的退化。
enum class EThemeMode : int
{
    Auto    = 0,   // 跟随系统（Win8Metro 皮肤，令牌自动解析）
    Light   = 1,   // Win8Metro 皮肤，强制浅色
    Dark    = 2,   // Win8Metro 皮肤，强制深色
    Classic = 3,   // [CLASSIC-SKIN] 独立经典蓝皮肤（skin\Classic），硬编码颜色，不参与深浅色切换
    Count   = 4
};

// 当前生效的模式（Auto 表示"跟着系统走"，不是"当前是浅色"）。
[[nodiscard]] EThemeMode Mode();
// 设置模式。会丢弃缓存，下一次 Token() 即按新模式取色。
//   返回 true 表示"生效的深浅色真的变了"，调用方据此决定要不要全量重绘 ——
//   Auto -> Light 在系统本来就是浅色时不应触发一次无意义的整树重绘。
[[nodiscard]] bool SetMode(EThemeMode e);

// 生效中的深浅色判定（已把 Auto 解析成系统的实际值）。
//   所有内部取色分支都必须用它，不能用 dwmtheme::IsSystemDarkMode()：
//   后者无视手动覆盖，会出现"预览卡是深色、主界面还是浅色"。
[[nodiscard]] bool IsDark();

// 取色（0xAARRGGBB，可直接喂给 DuiLib 的 SetBkColor / SetTextColor）。
//   首次调用时按当前系统强调色解析一次并缓存；系统主题变更时需调
//   Refresh() 让它重算（CFrameWnd::ApplySystemTheme 已负责）。
[[nodiscard]] DWORD Token(EToken e);

// [R236] 按**指定**深浅模式取同一个令牌 —— 只给主题预览卡用。
//   预览必须画出"还没选中的那个主题"长什么样，所以不能读全局状态。
//   只读，不改全局缓存；同一个表达式里可以安全连续取多个值。
[[nodiscard]] DWORD TokenInMode(EToken e, bool bDark);
[[nodiscard]] const wchar_t* TokenHexInMode(EToken e, bool bDark);

// 丢弃缓存并按当前系统设置重算。幂等，可重复调用。
void Refresh();

// 把令牌转成 "#AARRGGBB" 字符串（供 XML 动态属性或日志使用）。
[[nodiscard]] const wchar_t* TokenHex(EToken e);

// [R235] 同上，但 alpha 由调用方指定 —— 半透明叠加层专用（会话进度浮层、遮罩）。
//   这些位置原来直接写 "#66xxxxxx"/"0xF2xxxxxx"：RGB 必须跟随主题，
//   但 alpha 是设计意图（要能透出底下内容），不能一律拍成 FF。
//   用独立函数而不是给 TokenHex 加默认参数：默认参数会让
//   "#66005A9E" 这类历史写法在审查时看不出 alpha 是否被有意保留。
[[nodiscard]] const wchar_t* TokenHexAlpha(EToken e, unsigned char aAlpha);
// [WIN11-FLUENT] Control corner radius, in DIPs, applied through the same attribute the
// skin XML uses. Centralised here because buttons built in code were the last place
// with no radius at all: measured 125 square buttons against 70 rounded ones, and the
// square set was exactly the ones created by Page_*.cpp instead of loaded from XML.
// Small controls take 4 (WinUI 3 default); cards/large surfaces take 8.
inline constexpr int kRadiusControl = 4;
inline constexpr int kRadiusSurface = 8;




}   // namespace tokens