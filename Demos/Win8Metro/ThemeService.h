// ============================================================
// ThemeService.h - Host-side unified theme orchestrator (2026 mainstream)
//
// Architecture:
//   ┌─ ThemeService (singleton) ───────────────────────────┐
//   │  • Holds single ThemeState (name + dark + accentSrc) │
//   │  • ApplyTheme() is THE only entry point             │
//   │  • Broadcasts to IThemeObserver list                 │
//   │  • Delegates skin-dir switch to ThemeManager.dll     │
//   └─────────────────────────────────────────────────────┘
//       │                     │                    │
//       ▼                     ▼                    ▼
//  ThemeManager.dll      tokens::Token()     Observer list:
//  (skin cache,          (design tokens,     ├─ DwmBridge (title bar)
//   DWM, persist)         semantic only)     ├─ ControlTreeBridge (ReplayTokens + ScrollBar)
//                                            ├─ PageRefreshBridge (pages)
//                                            ├─ DefaultFontColorBridge
//                                            ├─ ThemeButtonBridge
//                                            └─ ReloadImagesBridge
//
// Theme Preset System (extensible, no hard-coded modes):
//   ┌─ PresetCatalog (skin\ scan + built-ins) ──────────┐
//   │  Classic : skin\Classic, dark=never,  accent=#FF1E5799  │
//   │  Auto    : skin\Win8Metro, dark=follow_system, accent=sys │
//   │  Light   : skin\Win8Metro, dark=off,   accent=system    │
//   │  Dark    : skin\Win8Metro, dark=on,    accent=system    │
//   │  (Dynamic: skin\Neon/, skin\Ocean/, ... scanned at startup) │
//   └───────────────────────────────────────────────────────┘
//
// Each preset: defines skinDir + darkMode policy + accent source.
// Tokens are computed from preset's token palette OR from system.
// Classic preset overrides token palette to deep-blue — tokens() on Classic
// returns the Classic palette, NOT system colors. This fixes the "two
// sources parallel" bug where Classic skin hardcodes #1E5799 but Token(Accent)
// still returned system blue.
//
// Why this is 2026-mainstream:
//   • Single source of truth (no more DLL-EMode vs tokens-EThemeMode sync dance)
//   • Open/Closed for new presets (drop skin\Foo\ folder, it appears)
//   • Observer pattern (subscribe once, all future changes fire — no fnOnChanged bloat)
//   • ThemeState is POD (easy to persist, debug, snapshot)
//   • Tokens consume ThemeState directly (no orphan token states possible)
// ============================================================
#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <mutex>

namespace ThemeService
{

// ============================================================
// Theme State (POD, single source of truth)
// ============================================================
enum class EDarkPolicy : int
{
    Never        = 0,   // Classic-like: always light
    Always       = 1,   // Force dark
    AlwaysLight  = 2,   // Force light (named for readability)
    FollowSystem = 3,   // Auto: follow OS
};

enum class EAccentSource : int
{
    System      = 0,   // Read from HKCU\...\DWM\AccentColor
    Override    = 1,   // Preset-provided fixed color (Classic=#FF1E5799)
};

// One preset = one skin directory + dark policy + accent source + optional palette
// Palette overrides ALL tokens (Surface/Ink/Border/etc.) for this preset.
// When palette is nullptr, tokens fall back to system/accent-derived computation.
struct ThemePalette
{
    // ---- Accent 家族（Classic: #FF007ACC / #FF1C97EA / #FF005A9E）----
    // [2026-fix] 这三个值原先是从 accent 用 Shift() 派生的：
    //   Shift(#007ACC,+12)=#1E89D2、Shift(#007ACC,-18)=#0065A8，
    //   与 docs/theme_design_spec.md §2.2 规定的 #1C97EA / #005A9E 不一致，
    //   也与 Classic 皮肤 XML 里写死的 hotbkcolor/pushedbkcolor 对不上。
    //   现在由 palette 显式承载 spec 值，token 与皮肤只有一份事实源。
    DWORD accent;
    DWORD accentHot;      // hover 态填充
    DWORD accentPushed;   // pressed 态填充
    DWORD accentText;     // 强调色当普通文字用（压在浅底上）
    DWORD accentSoft;     // 强调色淡底（次级强调按钮）
    DWORD accentBorder;   // 强调色描边（淡底按钮边框）
    DWORD onAccent;       // 压在 accent 填充上的文字
    DWORD onAccentHot;
    DWORD onAccentPushed;
    // ---- 中性色 ----
    DWORD surface;        // base background
    DWORD surfaceAlt;     // card / panel
    DWORD surfaceSunk;    // input / well
    DWORD surfaceHover;   // hover state
    DWORD border;
    DWORD borderSoft;
    DWORD ink;            // primary text
    DWORD inkSecondary;
    DWORD inkTertiary;
    DWORD inkMuted;
    // ---- 语义色 ----
    DWORD danger;
    DWORD dangerHot;
    DWORD dangerPushed;
    DWORD onDanger;
    DWORD success;
    DWORD warn;
    DWORD warnBg;
    // ---- 色带 ----
    // [2026-fix] headerBand 原来一词二义：既是 Classic 的"标题栏品牌深蓝"，
    //   又被塞进 EToken::HeaderBand（内容区表头带，绿语义）。
    //   这里拆开：headerBand 专指窗口 chrome 的标题栏品牌色；
    //   bandSurface 才是内容区表头带（Classic: SurfaceAlt #FFEFF6FC）。
    DWORD headerBand;     // 标题栏品牌色（Classic #FF1E5799）
    DWORD onHeaderBand;   // 标题栏上的文字色（Classic #FFFFFFFF）
    DWORD bandSurface;    // 内容区表头带底色 → EToken::HeaderBand
    DWORD onBandSurface;  // 内容区表头带文字色 → EToken::OnHeaderBand
};

struct ThemePreset
{
    std::wstring name;           // "classic", "auto", "light", "dark", "neon", ...
    std::wstring displayName;    // "经典蓝", "跟随系统", "浅色", "深色", ...
    std::wstring skinDir;        // "skin\\Classic", "skin\\Win8Metro", ...
    EDarkPolicy darkPolicy;
    EAccentSource accentSource;
    DWORD accentOverride;        // only valid when accentSource=Override
    bool hasPalette;             // true if tokens come from palette, false if system-derived
    ThemePalette palette;        // used when hasPalette=true
};

// ============================================================
// Observer (subscribe once, called on every ApplyTheme)
// ============================================================
class IThemeObserver
{
public:
    virtual ~IThemeObserver() = default;
    // Called AFTER ThemeState is set but BEFORE RedrawWindow.
    // Each observer owns its domain (DWM, control tree, pages, etc.)
    virtual void OnThemeChanged(const ThemePreset& preset) = 0;
};

// ============================================================
// Public API
// ============================================================

// Initialize: load preset catalog + read saved state + preload ThemeManager DLL
//   - pszExeDir: program root (must end with \ or not, handled internally)
//   - hWnd: main window HWND (can be nullptr at PreInit phase)
void Initialize(const wchar_t* pszExeDir, HWND hWnd);

// FullInit: inject DuiLib callbacks AFTER Create() (hWnd must be valid)
struct DuiLibCallbacks
{
    HWND hWnd = nullptr;                                       // real main window HWND (updates internal g_hWnd)
    std::function<void(const wchar_t*)> fnSetSkinDir;         // CPaintManagerUI::SetResourcePath
    std::function<bool(const wchar_t*, const wchar_t*, const wchar_t*)> fnSetAttr;
};
void FullInit(const DuiLibCallbacks& cb);

// Add an observer. Observers are called in registration order.
void AddObserver(IThemeObserver* p);
void RemoveObserver(IThemeObserver* p);

// Get all available presets (built-ins + scanned skin\ directory)
[[nodiscard]] const std::vector<ThemePreset>& GetPresets();

// Find preset by name (case-insensitive)
[[nodiscard]] const ThemePreset* FindPreset(const std::wstring& name);

// Apply a preset by name. bPersist=true writes config.ini.
//   This is THE single entry point for ALL theme changes (user pick, system setting change, startup).
//   Returns true if anything actually changed (preset differs from current).
bool ApplyTheme(const std::wstring& presetName, bool bPersist = true, bool bForce = false);

// Current preset
[[nodiscard]] const ThemePreset& CurrentPreset();

// Current effective dark flag (resolves Never/Always/FollowSystem)
[[nodiscard]] bool IsDark();

// Get effective accent color (resolves Override vs System)
[[nodiscard]] DWORD EffectiveAccent();
// [2026] 带参数版本：按指定 preset 解出强调色（供 ThemePickerWnd 等预览场景使用）
[[nodiscard]] DWORD EffectiveAccent(const ThemePreset& p);
// [2026] 按指定 preset 解出 EffectiveIsDark（供 ThemePickerWnd 预览使用）
[[nodiscard]] bool EffectiveIsDarkFor(const ThemePreset& p);

// Shutdown (cleanup observers, shutdown ThemeManager DLL)
void Shutdown();

// ============================================================
// Internal helpers for tokens layer (friend-like, public for tokens::BuildLocked)
// ============================================================
// Called by tokens::BuildLocked to get the active palette (or nullptr to fall back to system)
[[nodiscard]] const ThemePalette* ActivePaletteForTokens();

}   // namespace ThemeService
