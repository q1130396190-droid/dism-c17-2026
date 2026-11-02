// ============================================================
// ThemeManager.h - Thin Theme Manager DLL (C++17, Zero DuiLib Dep)
//
// [2026 重构] 纯引擎定位：
//   ThemeManager 是底层基础设施，**只负责 4 件事**：
//     ① 把"皮肤目录路径"注入给宿主 CPaintManagerUI（SetResourcePath）
//     ② DWM 标题栏深浅色属性（DwmSetWindowAttribute ATTR=20）
//     ③ config.ini 持久化（WritePrivateProfileString）
//     ④ IsDark 查询（注册表 AppsUseLightTheme + mode 推导）
//
//   ThemeManager 不管：
//     ✗ 控件树重建 / ReloadImages —— 宿主 OnThemeChanged 自己管
//     ✗ {tok:*} 令牌替换       —— 宿主 ExpandSkinFile 自己管
//     ✗ 硬编码颜色刷控件       —— 宿主 Classic 硬编码表自己管
//     ✗ Skin XML 预加载缓存    —— 删除！宿主 ReadSkinFromDll + ExpandSkinFile 自己管
//     ✗ 回调 fnSetAttr/fnOnChanged —— 删除！ThemeService Observer 模式已接管
//
//  为什么这样拆分？
//     之前的 fnSetAttr 在 DLL 持锁期间刷控件树，产生 2 个根因 bug：
//     P1: 刷的时机早于 ReloadImages，被后续宿主逻辑覆盖或残留
//     P2: PreloadAllSkins 硬编码缓存 Classic/Win8Metro 两套表，
//         但 Win8Metro 的 {tok:*} XML 展开后没有硬编码色 → 刷出来是 Classic 的残色
//     P3: fnOnChanged 回调进入宿主，可能反调 ThemeService（重入加锁）→ 崩溃
//
//   现在 DLL 零锁内回调，零 DuiLib 依赖，零皮肤内容耦合。
// ============================================================
#pragma once
#include <windows.h>
#include <string>
#include <mutex>

#ifdef THEMEMANAGER_EXPORTS
#  define TM_API __declspec(dllexport)
#else
#  define TM_API __declspec(dllimport)
#endif

namespace ThemeManager
{

// ALL fields are OPTIONAL (can be nullptr)
//   Initialize:  pszExeDir + hWnd + fnSetSkinDir 全部需要
//   SetSkinDir:  运行时主动改皮肤目录
//   ApplyDwmDark: 运行时主动切 DWM 标题栏深浅色
struct InitParams
{
    HWND hWnd = nullptr;
    const wchar_t* pszExeDir = nullptr;
    // fnSetSkinDir: 宿主 CPaintManagerUI::SetResourcePath
    //   DLL 不链接 DuiLib，通过回调注入。
    //   调用契约：路径是 "<exe>skin\Classic" 或 "<exe>skin\Win8Metro" 这样的完整路径。
    void (*fnSetSkinDir)(const wchar_t*) = nullptr;
};

// ============================================================
// C API (extern "C" for safe dynamic loading / 零 C++ ABI 风险)
// ============================================================
extern "C" {
    // 一次性初始化：注入 exeDir + hWnd + fnSetSkinDir
    //  读 config.ini 的 [theme] Mode 键（default "classic"）
    //  算皮肤目录 → fnSetSkinDir 注入宿主 → ApplyDwmDark
    //  幂等：已初始化时返回 S_OK，重新注入 hWnd/fn 是覆盖不是追加
    TM_API HRESULT TM_Init(const InitParams* p);

    TM_API HRESULT TM_Shutdown();

    // 纯引擎接口：给定皮肤目录路径 + DWM 深浅 + 持久化
    //  pszSkinDir: 完整路径，如 "D:\...\bin64\skin\Classic"
    //  bPersist:   是否写 config.ini（写固定字符串 pszIniKey）
    TM_API HRESULT TM_Apply(
        const wchar_t* pszSkinDir,
        bool bDark,
        bool bPersist,
        const wchar_t* pszIniKey   // 持久化 key, e.g. "classic"/"light"/"dark"/"auto"
    );

    TM_API bool TM_IsDarkForMode(bool bDarkCurrent);   // 纯注册表查询: AppsUseLightTheme
}

// ============================================================
// C++ wrappers
// ============================================================
TM_API HRESULT Initialize(const InitParams& params);
TM_API HRESULT Shutdown();
TM_API HRESULT Apply(
    const std::wstring& skinDir,
    bool bDark,
    bool bPersist,
    const std::wstring& iniKey
);
TM_API bool IsSystemDarkMode();   // 注册表查询

}   // namespace ThemeManager
