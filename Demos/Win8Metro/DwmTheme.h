#pragma once

// ============================================================
// DwmTheme.h —— Win11 视觉适配层（圆角 / Mica / 强调色）
//
// 设计原则（升级 Win11 同时兼容 Win10 的唯一可行做法）：
//   1) 所有 DWM 入口一律 GetModuleHandle + GetProcAddress 动态解析，
//      **不链接 dwmapi.lib**。Win10 上属性号不被识别时函数返回
//      E_INVALIDARG，调用方忽略即可 —— 不崩溃、不缺导入。
//   2) 是否启用只看"运行期探测结果"，不看版本号硬分支：
//      DwmApplyWindowTheme 内部逐项试探，成功即生效，失败即跳过。
//   3) 强调色读 HKCU（Win10 1607+ 与 Win11 同样存在这两个键），
//      不做版本判断，所以两代系统都能跟随用户主题色。
//
// 为什么单独成模块而不是散在 FrameWnd：
//   这三个能力都属于"窗口级外观"，会在 WM_CREATE、WM_SETTINGCHANGE、
//   WM_THEMECHANGED 三个时机重复应用；集中一处才能保证三个时机的
//   行为完全一致（否则改一处漏一处，表现为"切主题后圆角没了"）。
// ============================================================

#include <windows.h>

#include <string>

namespace dwmtheme
{

// [R201] 强调色（0x00BBGGRR，与 COLORREF 同序）。
//   读取顺序：HKCU\...\DWM\AccentColor（用户在设置里选的）
//            -> DwmGetColorizationColor（Aero 时代的老回退）
//            -> 项目默认（#007ACC，Win8 时代蓝）
//   返回的 DWORD 可直接用于 CControlUI::SetBkColor（需自行补 0xFF alpha）。
[[nodiscard]] DWORD AccentColorRef();

// 是否需要"强调色作为文字前景"时使用的加深版本（保证 4.5:1 对比度）。
//   规则：把强调色按 0.75 亮度压暗后，若与白底对比度仍不足 4.5:1 则继续压。
[[nodiscard]] DWORD AccentTextColorRef();

// 一次性应用窗口视觉：Win11 圆角 + Mica（若可用）+ 暗色标题栏（若系统为暗色）。
//   任何一项不受支持都静默跳过，返回实际生效的能力位集合（供日志自证）。
//   bDarkMode 由调用方按系统主题判定（本项目当前只做标题栏跟随）。
enum class EThemeAbility : unsigned int
{
    None        = 0,
    RoundedCorner = 1u << 0,   // DWMWA_WINDOW_CORNER_PREFERENCE 生效
    Mica        = 1u << 1,     // DWMWA_SYSTEMBACKDROP_TYPE 生效（Win11 22H2+）
    DarkCaption = 1u << 2,     // DWMWA_USE_IMMERSIVE_DARK_MODE 生效
    AccentApplied = 1u << 3,   // 强调色读取成功
};

[[nodiscard]] unsigned int ApplyWindowTheme(HWND hWnd, bool bDarkCaption);

// 系统是否处于深色应用模式（HKCU\...\Themes\Personalize\AppsUseLightTheme == 0）。
//   读不到按浅色处理（保守默认，避免把浅色界面画成暗色而不可读）。
[[nodiscard]] bool IsSystemDarkMode();

// [R201] 系统是否处于高对比度模式（无障碍设置）。
//   高对比度下**必须优先于**自定义配色：用户开它的目的就是拿走配色控制权。
//   Win10/Win11 同一接口（SPI_GETHIGHCONTRAST），无需版本判断。
[[nodiscard]] bool IsHighContrastMode();

// 高对比度模式下的两个基础色：系统窗口背景 / 系统窗口文字。
//   直接取 GetSysColor(COLOR_WINDOW) / COLOR_WINDOWTEXT，
//   这样在任意高对比度主题（含第三方）下都能得到用户实际看到的配色。
[[nodiscard]] DWORD HighContrastWindowColor();
[[nodiscard]] DWORD HighContrastTextColor();

// 把生效能力位转成紧凑日志串（如 "corner+mica+accent"），供 HostLog 使用。
[[nodiscard]] std::wstring AbilityToText(unsigned int nAbility);

}   // namespace dwmtheme