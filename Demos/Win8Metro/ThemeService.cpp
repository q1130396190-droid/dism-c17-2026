// ============================================================
// ThemeService.cpp - Host-side unified theme orchestrator
// ============================================================
#include "stdafx.h"
#include "ThemeService.h"
#include "ThemeManager.h"
#include "DwmTheme.h"   // AccentColorRef, IsSystemDarkMode, ApplyWindowTheme

#include <algorithm>

namespace ThemeService {

// ============================================================
// Internal state
// ============================================================
// [debug shim] TS_LOG —— 零开销空宏；如需开启调试日志，把宏体替换为:
//   do { FILE* fp=nullptr; if(fopen_s(&fp,"theme_debug.log","a")==0){fprintf(fp,"[TS] %s\n",msg);fclose(fp);} } while(0)
#define TS_LOG(msg) do { } while(0)

namespace {

std::mutex g_mx;
std::wstring g_exeDir;
HWND g_hWnd = nullptr;
bool g_bInitialized = false;

std::vector<ThemePreset> g_presets;
const ThemePreset* g_pCurrent = nullptr;   // pointer into g_presets
std::vector<IThemeObserver*> g_observers;

DuiLibCallbacks g_uiCb;

// ============================================================
// Preset catalog (built-ins + dynamic scan)
// ============================================================
void BuildBuiltinPresets()
{
    g_presets.clear();

    // Classic — deep-blue, independent of light/dark system
    {
        ThemePreset p;
        p.name = L"classic";
        p.displayName = L"经典蓝";
        p.skinDir = L"skin\\Classic";
        p.darkPolicy = EDarkPolicy::Never;
        p.accentSource = EAccentSource::Override;
        p.accentOverride = 0xFF007ACC;
        p.hasPalette = true;
        // [2026] 全部补 alpha=FF —— 之前是 0x00RRGGBB (透明!)，渲染会出问题
        // [2026-fix] 这里的每个值都是 docs/theme_design_spec.md §2.2 的 Classic Palette
        //   权威值，且与 bin64\skin\Classic 下 XML 里写死的颜色一一对应。
        //   token 层与皮肤层必须同源 —— 否则运行时动态生成的控件（走 token）
        //   和皮肤里的控件（走硬编码）会出现两套颜色。
        p.palette.accent = 0xFF007ACC;          // 经典亮蓝强调
        p.palette.accentHot = 0xFF1C97EA;       // hover 态
        p.palette.accentPushed = 0xFF005A9E;    // pressed 态
        p.palette.accentText = 0xFF007ACC;      // 蓝字（压在白底上 4.52:1）
        p.palette.accentSoft = 0xFFEEF4FB;      // 淡蓝按钮底（页内次级强调按钮）
        p.palette.accentBorder = 0xFFB8D0F0;    // 淡蓝按钮描边
        p.palette.onAccent = 0xFFFFFFFF;        // 蓝底上的白字
        p.palette.onAccentHot = 0xFFFFFFFF;
        p.palette.onAccentPushed = 0xFFFFFFFF;
        p.palette.surface = 0xFFFFFFFF;         // 白
        p.palette.surfaceAlt = 0xFFEFF6FC;       // 极浅蓝白（面板 / 表头带）
        p.palette.surfaceSunk = 0xFFE5E5E5;      // 凹陷灰（进度槽）
        p.palette.surfaceHover = 0xFFE5F1FB;     // 悬停浅蓝
        p.palette.border = 0xFFABABAB;           // 中灰
        p.palette.borderSoft = 0xFFE5E5E5;       // 淡分割线
        p.palette.ink = 0xFF1A1A1A;              // 近黑
        p.palette.inkSecondary = 0xFF333333;     // 深灰（表头文字）
        p.palette.inkTertiary = 0xFF666666;      // 中灰（次要文字）
        p.palette.inkMuted = 0xFF999999;         // 浅灰（占位 / 禁用）
        p.palette.danger = 0xFFE23A3E;           // 红
        p.palette.dangerHot = 0xFFE74C3C;        // 红 hover
        p.palette.dangerPushed = 0xFF96281B;     // 红 pressed
        p.palette.onDanger = 0xFFFFFFFF;         // 红底上的白字
        p.palette.success = 0xFF2E7D32;
        p.palette.warn = 0xFFF57F17;
        p.palette.warnBg = 0xFFFFF8E1;
        p.palette.headerBand = 0xFF1E5799;       // 经典深蓝标题栏（chrome 品牌色）
        p.palette.onHeaderBand = 0xFFFFFFFF;     // 标题栏上的白字
        p.palette.bandSurface = 0xFFEFF6FC;      // 内容区表头带 → EToken::HeaderBand
        p.palette.onBandSurface = 0xFF1A1A1A;    // 表头带文字 → EToken::OnHeaderBand
        g_presets.push_back(std::move(p));
    }

    // Auto — follow system, Win8Metro skin
    {
        ThemePreset p;
        p.name = L"auto";
        p.displayName = L"跟随系统";
        p.skinDir = L"skin\\Win8Metro";
        p.darkPolicy = EDarkPolicy::FollowSystem;
        p.accentSource = EAccentSource::System;
        p.hasPalette = false;
        g_presets.push_back(std::move(p));
    }

    // Light — force light, Win8Metro skin
    {
        ThemePreset p;
        p.name = L"light";
        p.displayName = L"浅色";
        p.skinDir = L"skin\\Win8Metro";
        p.darkPolicy = EDarkPolicy::AlwaysLight;
        p.accentSource = EAccentSource::System;
        p.hasPalette = false;
        g_presets.push_back(std::move(p));
    }

    // Dark — force dark, Win8Metro skin
    {
        ThemePreset p;
        p.name = L"dark";
        p.displayName = L"深色";
        p.skinDir = L"skin\\Win8Metro";
        p.darkPolicy = EDarkPolicy::Always;
        p.accentSource = EAccentSource::System;
        p.hasPalette = false;
        g_presets.push_back(std::move(p));
    }
}

// Scan skin\ directory for additional preset folders (skin\Neon\, skin\Ocean\, ...)
// Each scanned preset follows Win8Metro pattern (system accent, follow-system dark).
void ScanDynamicPresets()
{
    const std::wstring skinRoot = g_exeDir + L"skin";
    WIN32_FIND_DATAW fd = {};
    std::wstring find = skinRoot + L"\\*";
    HANDLE hFind = ::FindFirstFileW(find.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    // built-in dirs to skip
    static const wchar_t* kBuiltins[] = { L"Classic", L"Win8Metro", nullptr };

    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;

        const std::wstring dirName = fd.cFileName;

        bool bBuiltin = false;
        for (const wchar_t** pp = kBuiltins; *pp; ++pp)
            if (dirName == *pp) { bBuiltin = true; break; }
        if (bBuiltin) continue;

        // Found a new skin directory — make it a preset
        ThemePreset p;
        std::wstring lower = dirName;
        for (auto& ch : lower) ch = static_cast<wchar_t>(::towlower(ch));
        p.name = lower;
        p.displayName = dirName;
        p.skinDir = L"skin\\" + dirName;
        p.darkPolicy = EDarkPolicy::FollowSystem;
        p.accentSource = EAccentSource::System;
        p.hasPalette = false;
        g_presets.push_back(std::move(p));
    } while (::FindNextFileW(hFind, &fd));
    ::FindClose(hFind);
}

bool IsSystemDarkModeInternal()
{
    HKEY hKey = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &hKey) != ERROR_SUCCESS) return false;
    DWORD val = 0; DWORD sz = sizeof(val);
    ::RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, nullptr,
                       reinterpret_cast<BYTE*>(&val), &sz);
    ::RegCloseKey(hKey);
    return val == 0;
}

// Resolve effective dark flag from preset's policy
bool EffectiveIsDark(const ThemePreset& p)
{
    switch (p.darkPolicy)
    {
        case EDarkPolicy::Always:       return true;
        case EDarkPolicy::AlwaysLight:
        case EDarkPolicy::Never:        return false;
        case EDarkPolicy::FollowSystem: return IsSystemDarkModeInternal();
    }
    return false;
}

// Apply DWM dark-mode attribute for current preset
void ApplyDwmForCurrentPreset()
{
    if (!g_hWnd || !g_pCurrent) return;
    const bool bDark = EffectiveIsDark(*g_pCurrent);
    static HMODULE hMod = nullptr;
    if (!hMod) hMod = ::LoadLibraryW(L"dwmapi.dll");
    if (hMod)
    {
        using Fn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        auto fn = reinterpret_cast<Fn>(::GetProcAddress(hMod, "DwmSetWindowAttribute"));
        if (fn)
        {
            const DWORD ATTR = 20;
            BOOL v = bDark ? TRUE : FALSE;
            (void)fn(g_hWnd, ATTR, &v, sizeof(v));
        }
    }
}

}   // namespace

// ============================================================
// 公开辅助函数（匿名 namespace 中的 EffectiveIsDark 对外包装）
// ============================================================
bool EffectiveIsDarkFor(const ThemePreset& p)
{
    // 复用匿名 namespace 内的实现（只能在同一个 .cpp 中调用）
    switch (p.darkPolicy)
    {
        case EDarkPolicy::Always:       return true;
        case EDarkPolicy::AlwaysLight:
        case EDarkPolicy::Never:        return false;
        case EDarkPolicy::FollowSystem: return IsSystemDarkModeInternal();
    }
    return false;
}

// ============================================================
// Public API
// ============================================================
void Initialize(const wchar_t* pszExeDir, HWND hWnd)
{
    TS_LOG("Initialize entry");
    if (!pszExeDir) return;
    std::lock_guard<std::mutex> lk(g_mx);
    TS_LOG("Initialize locked");

    g_exeDir = pszExeDir;
    if (!g_exeDir.empty() && g_exeDir.back() != L'\\')
        g_exeDir += L"\\";
    g_hWnd = hWnd;
    TS_LOG("Initialize exeDir set");

    // Build preset catalog
    BuildBuiltinPresets();
    ScanDynamicPresets();
    TS_LOG("Initialize presets built");

    // Load saved preset name from config.ini (default Classic)
    wchar_t szName[64] = {};
    ::GetPrivateProfileStringW(L"theme", L"Mode", L"classic",
                               szName, 64, (g_exeDir + L"config.ini").c_str());
    TS_LOG("Initialize config read");

    // [WIN1607] Win10 全系强制 classic 经典蓝主题，忽略 config.ini 保存值。
    //   Win11 上用户可自由选择 auto/light/dark/classic 并存档。
    if (!DuiLib::IsWindows11OrGreater())
    {
        szName[0] = L'c'; szName[1] = L'l'; szName[2] = L'a';
        szName[3] = L's'; szName[4] = L's'; szName[5] = L'i';
        szName[6] = L'c'; szName[7] = L'\0';
        // 顺手把 config.ini 纠正掉，下次启动就干净了
        ::WritePrivateProfileStringW(L"theme", L"Mode", L"classic",
                                     (g_exeDir + L"config.ini").c_str());
    }

    // Resolve to preset pointer (case-insensitive)
    std::wstring saved = szName;
    for (auto& ch : saved) ch = static_cast<wchar_t>(::towlower(ch));
    g_pCurrent = nullptr;
    for (const auto& p : g_presets)
    {
        std::wstring cmp = p.name;
        for (auto& ch : cmp) ch = static_cast<wchar_t>(::towlower(ch));
        if (cmp == saved) { g_pCurrent = &p; break; }
    }
    if (!g_pCurrent) g_pCurrent = &g_presets[0];   // fallback: Classic
    TS_LOG("Initialize preset resolved");

    // PreInit ThemeManager DLL (瘦化版本: 只存 exeDir + 注册 fnSetSkinDir)
    //   新版 DLL 不再内部自动 SetResourcePath —— 我们在 FullInit 后由宿主自己算
    ThemeManager::InitParams p;
    p.pszExeDir = g_exeDir.c_str();
    p.hWnd = hWnd;   // nullptr OK, FullInit 会补
    p.fnSetSkinDir = [](const wchar_t* dir) {
        if (g_uiCb.fnSetSkinDir) g_uiCb.fnSetSkinDir(dir);
    };
    TS_LOG("Initialize before TM::Initialize");
    ThemeManager::Initialize(p);
    TS_LOG("Initialize after TM::Initialize");

    g_bInitialized = true;
    TS_LOG("Initialize DONE");
}

void FullInit(const DuiLibCallbacks& cb)
{
    TS_LOG("FullInit entry");
    // [2026] FullInit 里的 ApplyDwm + Observer 广播也必须在锁外，
    //   原因同 ApplyTheme：OnThemeChanged → tokens::Refresh → ActivePaletteForTokens
    //   会尝试再次 lock g_mx，形成重入死锁。
    std::vector<IThemeObserver*> observers;
    const ThemePreset* pCur = nullptr;
    HWND hWndLocal = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_uiCb = cb;
        if (cb.hWnd) g_hWnd = cb.hWnd;
        hWndLocal = g_hWnd;
        TS_LOG("FullInit locked + hWnd set");

        ThemeManager::InitParams p;
        p.hWnd = g_hWnd;
        p.pszExeDir = g_exeDir.c_str();
        p.fnSetSkinDir = [](const wchar_t* dir) {
            if (g_uiCb.fnSetSkinDir) g_uiCb.fnSetSkinDir(dir);
        };
        TS_LOG("FullInit before TM::Initialize");
        ThemeManager::Initialize(p);
        TS_LOG("FullInit after TM::Initialize");

        pCur = g_pCurrent;
        observers = g_observers;
    }
    TS_LOG("FullInit unlocked");
    if (pCur)
    {
        TS_LOG("FullInit before ApplyDwm");
        ApplyDwmForCurrentPreset();
        TS_LOG("FullInit before observer broadcast");
        for (auto* pObs : observers)
        {
            TS_LOG("FullInit before OnThemeChanged");
            if (pObs) pObs->OnThemeChanged(*pCur);
            TS_LOG("FullInit after OnThemeChanged");
        }
        TS_LOG("FullInit DONE");
    }
}

void AddObserver(IThemeObserver* p)
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (std::find(g_observers.begin(), g_observers.end(), p) == g_observers.end())
        g_observers.push_back(p);
}

void RemoveObserver(IThemeObserver* p)
{
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = std::find(g_observers.begin(), g_observers.end(), p);
    if (it != g_observers.end()) g_observers.erase(it);
}

const std::vector<ThemePreset>& GetPresets()
{
    std::lock_guard<std::mutex> lk(g_mx);
    return g_presets;
}

const ThemePreset* FindPreset(const std::wstring& name)
{
    std::lock_guard<std::mutex> lk(g_mx);
    std::wstring lower = name;
    for (auto& ch : lower) ch = static_cast<wchar_t>(::towlower(ch));
    for (const auto& p : g_presets)
    {
        std::wstring cmp = p.name;
        for (auto& ch : cmp) ch = static_cast<wchar_t>(::towlower(ch));
        if (cmp == lower) return &p;
    }
    return nullptr;
}

DWORD EffectiveAccent(const ThemePreset& p)
{
    if (p.accentSource == EAccentSource::Override)
        return p.accentOverride;
    return dwmtheme::AccentColorRef();
}

bool IsDark()
{
    std::lock_guard<std::mutex> lk(g_mx);
    return g_pCurrent ? EffectiveIsDark(*g_pCurrent) : false;
}

DWORD EffectiveAccent()
{
    std::lock_guard<std::mutex> lk(g_mx);
    return g_pCurrent ? EffectiveAccent(*g_pCurrent) : 0xFF007ACC;
}

bool ApplyTheme(const std::wstring& presetName, bool bPersist, bool bForce)
{
    TS_LOG("ApplyTheme entry");

    // [WIN1607] Win10 全系强制 classic 经典蓝主题，禁止切换到 auto/light/dark。
    //   任何 ApplyTheme 调用（含 config.ini 读入 + Observer 广播回调）
    //   都会在这里被拦截并收敛到 classic。
    if (!DuiLib::IsWindows11OrGreater())
    {
        // 如果已经是 classic，直接返回成功但不持久化
        if (_wcsicmp(presetName.c_str(), L"classic") != 0)
        {
            TS_LOG("ApplyTheme redirected to classic on Win10");
            return ApplyTheme(L"classic", false, bForce);
        }
        bPersist = false;   // Win10 上不持久化任何主题值（config.ini 里永远保持 classic）
    }

    // [2026] 锁只保护"状态切换"这一步 —— 查找 preset + 赋值 g_pCurrent。
    //   任何可能反调 ThemeService 的操作（ApplyMode 内部 fnSetAttr、
    //   Observer 广播里的 tokens::Refresh → ActivePaletteForTokens、
    //   RedrawWindow 触发 WM_PAINT → 控件读 tokens）**必须在锁外执行**，
    //   否则同一线程重入加 g_mx（非递归 mutex）→ UB → Release 下崩溃消失。
    std::vector<IThemeObserver*> observers;
    const ThemePreset* pNew = nullptr;
    HWND hWndLocal = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (!g_bInitialized) { TS_LOG("ApplyTheme NOT initialized"); return false; }

        std::wstring lower = presetName;
        for (auto& ch : lower) ch = static_cast<wchar_t>(::towlower(ch));
        for (const auto& p : g_presets)
        {
            std::wstring cmp = p.name;
            for (auto& ch : cmp) ch = static_cast<wchar_t>(::towlower(ch));
            if (cmp == lower) { pNew = &p; break; }
        }
        if (!pNew) { TS_LOG("ApplyTheme preset not found"); return false; }
        if (!bForce && g_pCurrent == pNew) { TS_LOG("ApplyTheme same preset, skip"); return false; }

        g_pCurrent = pNew;
        observers = g_observers;   // copy observer list —— 锁外再广播
        hWndLocal = g_hWnd;
        TS_LOG("ApplyTheme locked + preset changed");
    }
    // ⚠️ g_mx 已释放！以下所有操作都不会再触发重入加锁

    // [1] ThemeManager DLL: 瘦化后只做 SetResourcePath + DWM + persist
    //   新版 DLL 内部不再有 fnSetAttr 刷硬编码色 + fnOnChanged 回调
    //   —— 宿主 Observer 广播里会 RebuildMainTree，重建控件树本身就会展开
    //      当前皮肤的 {tok:*} 字面值，所以不需要 DLL 帮忙刷。
    bool bDark = EffectiveIsDark(*pNew);
    std::wstring skinDirFull = g_exeDir + pNew->skinDir;
    TS_LOG("ApplyTheme before TM::Apply");
    ThemeManager::Apply(skinDirFull, bDark, false, L"");
    TS_LOG("ApplyTheme after TM::Apply");

    // [2] Persist config.ini (ThemeService 是唯一 owner)
    if (bPersist)
        ::WritePrivateProfileStringW(L"theme", L"Mode", pNew->name.c_str(),
                                      (g_exeDir + L"config.ini").c_str());

    // [3] DWM 已经由 DLL 做了 —— 不再调 ApplyDwmForCurrentPreset

    // [4] Observer broadcast
    TS_LOG("ApplyTheme before observer broadcast");
    for (auto* pObs : observers)
    {
        TS_LOG("ApplyTheme before OnThemeChanged");
        if (pObs) pObs->OnThemeChanged(*pNew);
        TS_LOG("ApplyTheme after OnThemeChanged");
    }
    TS_LOG("ApplyTheme after observer broadcast");

    // [5] Repaint
    if (hWndLocal)
        ::RedrawWindow(hWndLocal, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);

    TS_LOG("ApplyTheme DONE");
    return true;
}

const ThemePreset& CurrentPreset()
{
    std::lock_guard<std::mutex> lk(g_mx);
    return g_pCurrent ? *g_pCurrent : g_presets[0];
}

const ThemePalette* ActivePaletteForTokens()
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_pCurrent || !g_pCurrent->hasPalette) return nullptr;
    return &g_pCurrent->palette;
}

void Shutdown()
{
    std::lock_guard<std::mutex> lk(g_mx);
    ThemeManager::Shutdown();
    g_observers.clear();
    g_presets.clear();
    g_pCurrent = nullptr;
    g_bInitialized = false;
}

}   // namespace ThemeService
