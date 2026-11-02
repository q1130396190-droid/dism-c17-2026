// ============================================================
// ThemeManager.cpp - Thin Pure-Engine Theme Manager DLL
//
// [2026 重构] 删除: fnSetAttr / PreloadAllSkins / fnOnChanged / EMode enum
//   DLL 不再知道 Classic/Win8Metro 这些 preset 名 —— 宿主传完整皮肤目录路径来
//
//   DLL 只做 4 件事:
//     ① 调 fnSetSkinDir 把皮肤目录注入宿主 CPaintManagerUI
//     ② 读注册表 AppsUseLightTheme
//     ③ DwmSetWindowAttribute DWMWA_USE_IMMERSIVE_DARK_MODE
//     ④ WritePrivateProfileString 持久化
//
//   DLL 不再:
//     ✗ 缓存皮肤 XML (PreloadAllSkins 删)
//     ✗ 刷硬编码颜色到控件树 (fnSetAttr 删)
//     ✗ 通知宿主刷新 (fnOnChanged 删 —— ThemeService Observer 已接管)
//     ✗ 重建控件树 (宿主 OnThemeChanged 里 RebuildMainTree 做)
//     ✗ RedrawWindow (宿主自己做)
//
//  为什么这样改:
//     删除前 DLL 持锁期间做 fnSetAttr 刷硬编码色 + fnOnChanged 回调宿主 —— 宿主
//     OnThemeChanged 里会调 tokens::Refresh → ThemeService, ThemeService 自己
//     也持锁 → 重入加锁 → Release 下进程直接崩。
//     而且 PreloadAllSkins 缓存的硬编码颜色表有 Cross-Contamination 问题:
//     Win8Metro 的 XML 里 titlebar 走 {tok:} 字面值, PreloadAllSkins 解析时
//     就找不到 bkcolor, 缓存表里没有, 刷控件树时 titlebar 残留 Classic 的
//     #FF1E5799 深蓝 —— 切到 Win8Metro 后标题栏颜色不对。
//
//  现在 DLL 零锁内回调, 零 DuiLib 依赖, 零皮肤内容耦合。
// ============================================================
#include "ThemeManager.h"
#include <mutex>
#include <string>

namespace ThemeManager {
namespace detail {

std::mutex        g_mx;
HWND              g_hWnd   = nullptr;
std::wstring      g_exeDir;
std::wstring      g_iniPath;
InitParams        g_params;
bool              g_bInit  = false;

// 上次皮肤目录 —— 用来判断是否真的切了 (避免重复 SetResourcePath)
std::wstring      g_lastSkinDir;

// DWM 模块加载一次
static HMODULE    g_hDwmApi = nullptr;

static HMODULE GetDwmApi()
{
    if (!g_hDwmApi) g_hDwmApi = ::LoadLibraryW(L"dwmapi.dll");
    return g_hDwmApi;
}

// ============================================================
// DWM 标题栏深浅色
// ============================================================
static HRESULT ApplyDwmDark(HWND hWnd, bool bDark)
{
    if (!hWnd) return S_FALSE;
    HMODULE hMod = GetDwmApi();
    if (!hMod) return E_FAIL;
    using Fn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    auto fn = reinterpret_cast<Fn>(::GetProcAddress(hMod, "DwmSetWindowAttribute"));
    if (!fn) return E_FAIL;
    const DWORD ATTR = 20;   // DWMWA_USE_IMMERSIVE_DARK_MODE
    BOOL v = bDark ? TRUE : FALSE;
    return fn(hWnd, ATTR, &v, sizeof(v));
}

// ============================================================
// 系统注册表: AppsUseLightTheme
// ============================================================
static bool ReadAppsUseLightTheme()
{
    HKEY hKey = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &hKey) != ERROR_SUCCESS) return true;
    DWORD val = 1; DWORD sz = sizeof(val);
    ::RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, nullptr,
                       reinterpret_cast<BYTE*>(&val), &sz);
    ::RegCloseKey(hKey);
    return val != 0;
}

}   // namespace detail


// ============================================================
// Exported API
// ============================================================
extern "C" TM_API HRESULT TM_Init(const InitParams* p)
{
    if (!p || !p->pszExeDir) return E_POINTER;

    std::lock_guard<std::mutex> lk(detail::g_mx);

    detail::g_exeDir = p->pszExeDir;
    if (!detail::g_exeDir.empty() && detail::g_exeDir.back() != L'\\')
        detail::g_exeDir += L"\\";
    detail::g_iniPath = detail::g_exeDir + L"config.ini";

    // 覆盖注入 —— Init 可以被 PreInit 和 FullInit 各调一次
    // 第一次传 exeDir + fnSetSkinDir, 第二次补 hWnd
    if (p->hWnd) detail::g_hWnd = p->hWnd;
    if (p->fnSetSkinDir) detail::g_params.fnSetSkinDir = p->fnSetSkinDir;

    detail::g_bInit = true;
    return S_OK;
}

extern "C" TM_API HRESULT TM_Shutdown()
{
    std::lock_guard<std::mutex> lk(detail::g_mx);
    detail::g_hWnd  = nullptr;
    detail::g_params = {};
    detail::g_lastSkinDir.clear();
    detail::g_bInit = false;
    return S_OK;
}

// [2026] 纯 Apply: 给定完整路径 + DWM 深浅 + 持久化 key
//   pszSkinDir 形如 "D:\...\bin64\skin\Classic"
//   bDark:      DWM 标题栏是否深色
//   bPersist:   是否写 config.ini
//   pszIniKey:  写 config.ini 用的字符串 (不写 key 则 bPersist 忽略)
extern "C" TM_API HRESULT TM_Apply(
    const wchar_t* pszSkinDir,
    bool bDark,
    bool bPersist,
    const wchar_t* pszIniKey)
{
    std::lock_guard<std::mutex> lk(detail::g_mx);
    if (!detail::g_bInit) return E_UNEXPECTED;
    if (!pszSkinDir) return E_POINTER;

    // [1] 皮肤目录 —— 只有真的不同才调 SetResourcePath
    std::wstring sDir = pszSkinDir;
    if (detail::g_params.fnSetSkinDir && detail::g_lastSkinDir != sDir)
    {
        detail::g_params.fnSetSkinDir(sDir.c_str());
        detail::g_lastSkinDir = sDir;
    }

    // [2] DWM 标题栏深浅色
    if (detail::g_hWnd)
        detail::ApplyDwmDark(detail::g_hWnd, bDark);

    // [3] 持久化
    if (bPersist && pszIniKey)
        ::WritePrivateProfileStringW(L"theme", L"Mode",
                                      pszIniKey,
                                      detail::g_iniPath.c_str());

    // [2026 删] 不再调 fnSetAttr 刷硬编码色 —— 宿主重建控件树时自己做
    // [2026 删] 不再调 fnOnChanged —— ThemeService Observer 已接管
    // [2026 删] 不再 RedrawWindow —— 宿主 Observer 广播之后统一做

    return S_OK;
}

extern "C" TM_API bool TM_IsDarkForMode(bool /*bDarkCurrent*/)
{
    return !detail::ReadAppsUseLightTheme();
}

// ============================================================
// C++ wrappers (返回 HRESULT 而不是 bool/EMode, 更易写错误处理)
// ============================================================
TM_API HRESULT Initialize(const InitParams& p)          { return TM_Init(&p); }
TM_API HRESULT Shutdown()                               { return TM_Shutdown(); }
TM_API HRESULT Apply(const std::wstring& sDir, bool bDark,
                      bool bPersist, const std::wstring& iniKey)
{
    return TM_Apply(sDir.c_str(), bDark, bPersist, iniKey.c_str());
}
TM_API bool IsSystemDarkMode()                          { return !detail::ReadAppsUseLightTheme(); }

}   // namespace ThemeManager
