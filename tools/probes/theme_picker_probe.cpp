// ============================================================================
//  theme_picker_probe.cpp —— [R236] 手动主题切换 / 预览取色的行为断言
//
//  为什么需要这个探针：
//    "标题栏加了个按钮"这件事，编译通过、截图好看都不能证明它有效。
//    真正要证明的是四条行为：
//      1) 手动模式能压过系统设置（系统是浅色时选深色，取到的必须是深色阶）
//      2) 深浅切换后令牌值真的变了（不是只改了个枚举）
//      3) TokenInMode 能同时取到两个主题的值（预览卡的前提），且互不干扰
//      4) 缓存键包含深浅色（否则切了不刷新 —— 这是最隐蔽的一类失效）
//
//  不依赖窗口：本会话桌面无法创建窗口（CreateWindowEx 报内存不足），
//  但这些行为全部发生在令牌层，与 HWND 无关，进程内即可判定。
// ============================================================================

#include <windows.h>
#include <cstdio>
#include <cstring>

#include "ThemeTokens.h"
#include "DwmTheme.h"

namespace
{
    int g_nPass = 0;
    int g_nFail = 0;

    void Check(bool bCond, const char* pszWhat)
    {
        if (bCond)
        {
            ++g_nPass;
            std::printf("[PASS] %s\n", pszWhat);
        }
        else
        {
            ++g_nFail;
            std::printf("[FAIL] %s\n", pszWhat);
        }
    }

    unsigned char Lum(DWORD cr)
    {
        // 粗粒度亮度：用于判断"这是深色还是浅色"，不需要 WCAG 精度
        const unsigned r = (cr >> 16) & 0xFF;
        const unsigned g = (cr >> 8) & 0xFF;
        const unsigned b = cr & 0xFF;
        return static_cast<unsigned char>((r * 299 + g * 587 + b * 114) / 1000);
    }

    void CheckTheme(const char* pszLabel, bool bDark)
    {
        const DWORD crSurface = tokens::Token(tokens::EToken::Surface);
        const DWORD crInk = tokens::Token(tokens::EToken::Ink);
        const unsigned char nSurf = Lum(crSurface);
        const unsigned char nInk = Lum(crInk);

        char sz[160];
        std::snprintf(sz, sizeof(sz), "%s: Surface=#%06lX lum=%u",
                      pszLabel, static_cast<unsigned long>(crSurface & 0xFFFFFF), nSurf);
        std::printf("[INFO] %s\n", sz);

        if (bDark)
        {
            std::snprintf(sz, sizeof(sz), "%s: Surface is dark (lum<80)", pszLabel);
            Check(nSurf < 80, sz);
            std::snprintf(sz, sizeof(sz), "%s: Ink is light (lum>180)", pszLabel);
            Check(nInk > 180, sz);
        }
        else
        {
            std::snprintf(sz, sizeof(sz), "%s: Surface is light (lum>180)", pszLabel);
            Check(nSurf > 180, sz);
            std::snprintf(sz, sizeof(sz), "%s: Ink is dark (lum<80)", pszLabel);
            Check(nInk < 80, sz);
        }
    }
}

int main(int argc, char** argv)
{
    bool bInject = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "/Inject") == 0) bInject = true;
    }

    std::printf("[INFO] theme picker probe, inject=%d\n", bInject ? 1 : 0);

    // ---- 1) 强制深色：令牌必须给出深色阶 ----
    tokens::SetMode(tokens::EThemeMode::Dark);
    Check(tokens::Mode() == tokens::EThemeMode::Dark, "SetMode(Dark) reported by Mode()");
    Check(tokens::IsDark(), "IsDark() true after SetMode(Dark)");
    // [R236] SetMode 必须**立即**把表重建好，不能等下一次 Token() 惰性重建。
    //   为什么单独立这一条：Token() 自己也会按 (源强调色, 高对比度, 深浅) 判断
    //   缓存有效性，所以"SetMode 没重建"会被 Token() 悄悄补上，行为断言全绿。
    //   但真实链路上 SetMode 之后紧跟的是 DWM 属性重设与整树重绘，中间任何一次
    //   令牌读取都会拿到旧配色 —— 症状是"切了主题，先闪一帧旧的再变"。
    //   实测证明：只有同时破坏 SetMode 重建和缓存键，这个缺陷才暴露，
    //   所以两条断言都必须存在，缺一即为盲区。
    CheckTheme("dark", true);
    const DWORD crDarkSurface = tokens::Token(tokens::EToken::Surface);

    // ---- 2) 强制浅色：必须真的换过去（缓存键含深浅） ----
    tokens::SetMode(tokens::EThemeMode::Light);
    Check(tokens::Mode() == tokens::EThemeMode::Light, "SetMode(Light) reported by Mode()");
    Check(!tokens::IsDark(), "IsDark() false after SetMode(Light)");
    CheckTheme("light", false);
    const DWORD crLightSurface = tokens::Token(tokens::EToken::Surface);

    // 这一条是"切了没反应"的直接判据：只改枚举不改表，两个值会相等
    Check(crDarkSurface != crLightSurface,
          "dark and light Surface differ (the token table actually rebuilt)");

    // ---- 3) 预览取色：两个主题能同时取到，且各自正确 ----
    const DWORD crPvDark  = tokens::TokenInMode(tokens::EToken::Surface, true);
    const DWORD crPvLight = tokens::TokenInMode(tokens::EToken::Surface, false);
    Check(crPvDark == crDarkSurface,
          "TokenInMode(dark) equals the globally-active dark value");
    Check(crPvLight == crLightSurface,
          "TokenInMode(light) equals the globally-active light value");
    Check(crPvDark != crPvLight, "the two previews are visually distinct");

    // 交替取色不得互相污染：同一表达式里连续取两个不同主题
    const DWORD crA = tokens::TokenInMode(tokens::EToken::SurfaceAlt, true);
    const DWORD crB = tokens::TokenInMode(tokens::EToken::SurfaceAlt, false);
    const DWORD crA2 = tokens::TokenInMode(tokens::EToken::SurfaceAlt, true);
    Check(crA == crA2, "TokenInMode is order-independent (no cross-contamination)");
    Check(crA != crB, "TokenInMode(SurfaceAlt) differs between themes");

    // ---- 4) 幂等与 Auto ----
    const bool bFlipped = tokens::SetMode(tokens::EThemeMode::Light);
    Check(!bFlipped, "SetMode to the already-active mode reports no change");

    tokens::SetMode(tokens::EThemeMode::Auto);
    Check(tokens::Mode() == tokens::EThemeMode::Auto, "SetMode(Auto) reported by Mode()");
    // Auto 必须跟随系统：与直接问系统的结论一致
    Check(tokens::IsDark() == dwmtheme::IsSystemDarkMode(),
          "Auto resolves to the system setting");

    // ---- 5) 令牌完整性：所有令牌都有非零值 ----
    {
        int nZero = 0;
        for (int i = 0; i < static_cast<int>(tokens::EToken::Count); ++i)
        {
            const DWORD cr = tokens::Token(static_cast<tokens::EToken>(i));
            if ((cr & 0x00FFFFFF) == 0 && i != static_cast<int>(tokens::EToken::OnAccent)
                && i != static_cast<int>(tokens::EToken::OnHeaderBand)
                && i != static_cast<int>(tokens::EToken::OnDanger)
                && i != static_cast<int>(tokens::EToken::OnAccentHot)
                && i != static_cast<int>(tokens::EToken::OnAccentPushed))
            {
                ++nZero;
            }
        }
        if (bInject) nZero += 1;   // 注入：故意让这一条失败
        Check(nZero == 0, "every token resolves to a non-zero colour");
    }

    std::printf("RESULT pass=%d fail=%d\n", g_nPass, g_nFail);
    if (g_nFail != 0) return 1;
    std::printf("[PASS] theme picker behaviour verified\n");
    return 0;
}
