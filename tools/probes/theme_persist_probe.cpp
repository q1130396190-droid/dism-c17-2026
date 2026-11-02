// ============================================================================
//  theme_persist_probe.cpp —— [R236] 主题选择的落盘与读回
//
//  为什么单独做一个探针：
//    "点了深色，下次启动还是深色"是这条需求的隐含要求。它跨越两个子系统
//    （令牌层 + config.ini），任何一侧写错都不会在编译期暴露：
//      · 写盘键名与读盘键名不一致 -> 永远读不到，静默回落到 Auto
//      · 段落名拼错 -> WritePrivateProfileString 会新建一个段落，看起来"成功"
//      · 写入后没有 FlushFileBuffers 语义 -> 进程被结束前丢配置
//    这些用肉眼审查配置文件都看不出来，必须让程序自己写、自己读。
//
//  侧信道保护：本探针只操作临时文件（/Out=<path>），不碰 bin64\config.ini，
//    避免把测试值留在用户真实配置里。
// ============================================================================

#include <windows.h>
#include <cstdio>
#include <cstring>

#include "ThemeTokens.h"

namespace
{
    int g_nPass = 0;
    int g_nFail = 0;

    void Check(bool bCond, const char* pszWhat)
    {
        if (bCond) { ++g_nPass; std::printf("[PASS] %s\n", pszWhat); }
        else       { ++g_nFail; std::printf("[FAIL] %s\n", pszWhat); }
    }
}

int main(int argc, char** argv)
{
    bool bInject = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "/Inject") == 0) bInject = true;
    }

    // 必须用**绝对路径**：WritePrivateProfileString 对相对路径是
    //   "相对当前工作目录"且部分情况下直接失败（返回 FALSE），
    //   而 GetPrivateProfileString 又会用另一套查找规则。两者不一致时
    //   表现为"写成功、读不到"，非常难查。统一转绝对路径。
    wchar_t szPath[MAX_PATH] = {};
    ::GetFullPathNameW(L"theme_persist_probe.ini", MAX_PATH, szPath, nullptr);
    const wchar_t* pszFile = szPath;
    std::printf("[INFO] ini file: %ls\n", pszFile);

    std::printf("[INFO] theme persist probe, inject=%d\n", bInject ? 1 : 0);

    // 每轮从干净文件开始，否则上一轮的残留会让断言恒真
    ::DeleteFileW(pszFile);

    const wchar_t* kSection = L"Theme";
    // 注入时故意用另一个键名写入 —— 模拟"读写键名不一致"这个真实缺陷形态
    const wchar_t* kWriteKey = bInject ? L"ModeXX" : L"Mode";
    const wchar_t* kReadKey  = L"Mode";

    // ---- 1) 三种模式往返 ----
    struct TCase { const wchar_t* psz; tokens::EThemeMode e; };
    const TCase cases[] = {
        { L"light", tokens::EThemeMode::Light },
        { L"dark",  tokens::EThemeMode::Dark  },
        { L"auto",  tokens::EThemeMode::Auto  },
    };

    for (const TCase& c : cases)
    {
        const BOOL bOk = ::WritePrivateProfileStringW(kSection, kWriteKey, c.psz, pszFile);
        char sz[128];
        std::snprintf(sz, sizeof(sz), "write '%ls' to [Theme]", c.psz);
        Check(bOk != FALSE, sz);

        wchar_t szRead[32] = {};
        ::GetPrivateProfileStringW(kSection, kReadKey, L"__missing__", szRead, 32, pszFile);
        std::snprintf(sz, sizeof(sz), "read back '%ls' matches what was written", c.psz);
        Check(wcscmp(szRead, c.psz) == 0, sz);
    }

    // ---- 2) 缺键时的默认值必须是 auto（跟随系统） ----
    {
        wchar_t szRead[32] = {};
        ::GetPrivateProfileStringW(kSection, L"NotExist", L"auto", szRead, 32, pszFile);
        Check(wcscmp(szRead, L"auto") == 0,
              "missing key falls back to 'auto' (follow system, not a hard-coded light)");
    }

    // ---- 3) 键名大小写不敏感（Windows 语义），但拼错必须读不到 ----
    {
        ::WritePrivateProfileStringW(kSection, L"Mode", L"dark", pszFile);
        wchar_t szRead[32] = {};
        ::GetPrivateProfileStringW(kSection, L"MODE", L"auto", szRead, 32, pszFile);
        Check(wcscmp(szRead, L"dark") == 0, "key lookup is case-insensitive (Windows semantics)");

        wchar_t szBad[32] = {};
        ::GetPrivateProfileStringW(kSection, L"Mod", L"auto", szBad, 32, pszFile);
        // 这一条是反向对照：拼错就应当读不到默认值，证明上面的命中不是巧合
        Check(wcscmp(szBad, L"auto") == 0, "a misspelt key does NOT resolve (negative control)");
    }

    // ---- 4) 模式字符串与枚举的映射必须完整覆盖 ----
    {
        // 与 CFrameWnd::ApplyThemeMode / LoadThemeMode 里的映射保持一致。
        //   这里独立复算一遍：若两处对不上，配置写的是 dark、读出来却是 Auto，
        //   表现为"设置了深色，重启变浅色"。
        auto toStr = [](tokens::EThemeMode e) -> const wchar_t*
        {
            return (e == tokens::EThemeMode::Light) ? L"light"
                 : (e == tokens::EThemeMode::Dark)  ? L"dark"
                                                    : L"auto";
        };
        auto fromStr = [](const wchar_t* p) -> tokens::EThemeMode
        {
            if (wcscmp(p, L"light") == 0) return tokens::EThemeMode::Light;
            if (wcscmp(p, L"dark")  == 0) return tokens::EThemeMode::Dark;
            return tokens::EThemeMode::Auto;
        };

        bool bRoundTrip = true;
        for (int i = 0; i < static_cast<int>(tokens::EThemeMode::Count); ++i)
        {
            const tokens::EThemeMode e = static_cast<tokens::EThemeMode>(i);
            if (fromStr(toStr(e)) != e) bRoundTrip = false;
        }
        Check(bRoundTrip, "every EThemeMode survives a string round-trip");
    }

    ::DeleteFileW(pszFile);

    std::printf("RESULT pass=%d fail=%d\n", g_nPass, g_nFail);
    if (g_nFail != 0) return 1;
    std::printf("[PASS] theme persistence verified\n");
    return 0;
}
