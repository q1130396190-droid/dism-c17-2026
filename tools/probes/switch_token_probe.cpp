// ============================================================================
//  switch_token_probe.cpp -- 验证 CSwitchUI 取到的令牌值是否真的是黑色
//
//  现象：优化页的开关渲染成纯黑胶囊，无滑块、无描边。
//  CSwitchUI 用的是 tokens::Token(SurfaceSunk/Border/Ink/Accent)。
//  若这些令牌返回 0x00000000 或 0xFF000000，toCref 之后就是纯黑 ——
//  轨道与滑块同色，视觉上融成一个黑胶囊，正好对上截图。
//
//  这个探针把四个令牌的真实值打出来，区分两种可能：
//    · 令牌表没建（arr 全是 0）        -> 返回 0xFF000000
//    · 令牌正常但 CSwitchUI 取错了索引 -> 返回正常色
// ============================================================================

#include <windows.h>
#include <cstdio>

#include "ThemeTokens.h"

int main()
{
    struct TItem { const char* pszName; tokens::EToken e; };
    const TItem items[] = {
        { "SurfaceSunk", tokens::EToken::SurfaceSunk },
        { "Border",      tokens::EToken::Border },
        { "Ink",         tokens::EToken::Ink },
        { "Accent",      tokens::EToken::Accent },
        { "OnAccent",    tokens::EToken::OnAccent },
        { "Surface",     tokens::EToken::Surface },
    };

    std::printf("[INFO] isDark=%d\n", tokens::IsDark() ? 1 : 0);

    // 两种模式各打一次：应用实际跑在 dark，而探针默认 Auto(跟随系统=浅色)，
    //   只看 Auto 会漏掉深色分支里才有的问题。
    for (int nMode = 0; nMode < 2; ++nMode)
    {
        tokens::SetMode(nMode == 0 ? tokens::EThemeMode::Light
                                   : tokens::EThemeMode::Dark);
        std::printf("[INFO] mode=%s  Sunk=0x%08lX Border=0x%08lX Ink=0x%08lX "
                    "Accent=0x%08lX OnAccent=0x%08lX Surface=0x%08lX\n",
            nMode == 0 ? "light" : "dark ",
            (unsigned long)tokens::Token(tokens::EToken::SurfaceSunk),
            (unsigned long)tokens::Token(tokens::EToken::Border),
            (unsigned long)tokens::Token(tokens::EToken::Ink),
            (unsigned long)tokens::Token(tokens::EToken::Accent),
            (unsigned long)tokens::Token(tokens::EToken::OnAccent),
            (unsigned long)tokens::Token(tokens::EToken::Surface));
    }
    std::printf("[INFO] isDark=%d\n", tokens::IsDark() ? 1 : 0);

    int nBlack = 0;
    for (const TItem& it : items)
    {
        const DWORD cr = tokens::Token(it.e);
        std::printf("[INFO] %-12s = 0x%08lX\n", it.pszName,
                    static_cast<unsigned long>(cr));
        if ((cr & 0x00FFFFFF) == 0)
        {
            ++nBlack;
            std::printf("[FAIL] %-12s resolves to pure black\n", it.pszName);
        }
    }

    // 深色主题下 Surface 应该是 #202020 一类，不可能是纯黑。
    // 如果这里就是 0，说明令牌表根本没建起来。
    if (nBlack > 0)
    {
        std::printf("[FAIL] %d token(s) are pure black -- the table is not built\n", nBlack);
        return 1;
    }
    std::printf("[PASS] every token resolves to a non-black colour\n");
    return 0;
}
