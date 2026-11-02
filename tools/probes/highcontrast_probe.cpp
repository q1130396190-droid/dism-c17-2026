// ============================================================
// highcontrast_probe.cpp —— 证明高对比度支持真的生效
//
// 为什么需要这个探针：
//   评估报告指出"高对比度模式不生效"（全量检索 HIGHCONTRAST 命中 0）。
//   加了 IsHighContrast() / ApplyHighContrast() 之后，必须证明三件事：
//     1) 开关能正确读出系统状态（不能在未开启时误判为开启）
//     2) 开启时整棵树被刷成系统前景/背景，且对比度达到 WCAG AA
//     3) 强调色用于焦点，保证焦点指示可见（非文本 UI 组件要求 >= 3:1）
//
// 判据：
//   - 未开启高对比度时 ApplyHighContrast 返回 0（不改任何控件）
//   - 系统前景/背景色的对比度 >= 4.5（WCAG AA 正文）
//   - 焦点色相对背景的对比度 >= 3.0（WCAG AA 非文本）
// ============================================================

#include "UIlib.h"
#include "ThemeService.h"
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

using namespace DuiLib;

namespace
{
int g_nPass = 0;
int g_nFail = 0;

void Chk(const char* pszName, bool bOk, const std::wstring& sDetail)
{
    char sz[512] = {};
    ::WideCharToMultiByte(CP_UTF8, 0, sDetail.c_str(),
                          static_cast<int>(sDetail.size()),
                          sz, sizeof(sz) - 1, nullptr, nullptr);
    if (bOk) { ++g_nPass; ::printf("[PASS] %s | %s\n", pszName, sz); }
    else     { ++g_nFail; ::printf("[FAIL] %s | %s\n", pszName, sz); }
}

std::wstring Hex(DWORD c)
{
    wchar_t buf[16] = {};
    ::swprintf_s(buf, L"0x%08lX", static_cast<unsigned long>(c));
    return buf;
}

// WCAG 2.1 相对亮度
double Luminance(DWORD c)
{
    auto ch = [](int v) {
        const double s = v / 255.0;
        return (s <= 0.03928) ? s / 12.92 : ::pow((s + 0.055) / 1.055, 2.4);
    };
    const int r = (c >> 16) & 0xFF;
    const int g = (c >> 8) & 0xFF;
    const int b = c & 0xFF;
    return 0.2126 * ch(r) + 0.7152 * ch(g) + 0.0722 * ch(b);
}

double Contrast(DWORD a, DWORD b)
{
    const double la = Luminance(a);
    const double lb = Luminance(b);
    const double hi = (la > lb) ? la : lb;
    const double lo = (la > lb) ? lb : la;
    return (hi + 0.05) / (lo + 0.05);
}

} // namespace

int main(int argc, char** argv)
{
    // [GATE-SELFTEST] /Inject adds one deliberately failing assertion so this
    // probe can prove its verdict path still works; otherwise a check that
    // silently stopped asserting would look exactly like a passing run.
    bool bInject = false;
    for (int i = 1; i < argc; ++i)
        if (::lstrcmpiA(argv[i], "/Inject") == 0) bInject = true;

    ::printf("=== 高对比度支持验证 ===\n\n");

    // ---- 1) 系统状态读取 ----
    const bool bHc = theme::IsHighContrast();
    {
        wchar_t buf[128] = {};
        ::swprintf_s(buf, L"IsHighContrast()=%d（当前系统会话）", bHc ? 1 : 0);
        // 这里不判对错：探针要能在两种会话下都跑通。
        // 关键是它与系统 SPI_GETHIGHCONTRAST 的读取结果一致。
        HIGHCONTRASTW hc = {};
        hc.cbSize = sizeof(hc);
        const bool bDirect =
            ::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) &&
            (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
        Chk("读取结果与 SPI_GETHIGHCONTRAST 一致", bHc == bDirect, buf);
    }

    // ---- 2) 系统色对 ----
    const DWORD dwText = theme::HighContrastText();
    const DWORD dwBk = theme::HighContrastBk();
    const DWORD dwAccent = theme::HighContrastAccent();

    {
        wchar_t buf[160] = {};
        const double r = Contrast(dwText, dwBk);
        ::swprintf_s(buf, L"text=%s bk=%s ratio=%.2f",
                     Hex(dwText).c_str(), Hex(dwBk).c_str(), r);
        Chk("系统前景/背景对比度 >= 4.5", r >= 4.5, buf);
    }
    {
        wchar_t buf[160] = {};
        const double r = Contrast(dwAccent, dwBk);
        ::swprintf_s(buf, L"accent=%s bk=%s ratio=%.2f",
                     Hex(dwAccent).c_str(), Hex(dwBk).c_str(), r);
        // 非文本 UI 组件的 WCAG 要求是 3:1，不是 4.5:1
        Chk("强调色/背景对比度 >= 3.0（非文本 UI）", r >= 3.0, buf);
    }

    // ---- 3) 未开启时不应改任何控件 ----
    if (!bHc)
    {
        Chk("未开启高对比度时 ApplyHighContrast(nullptr) = 0",
            theme::ApplyHighContrast(nullptr) == 0, L"null root 安全返回 0");
    }
    else
    {
        ::printf("[SKIP] 当前会话已开启高对比度，跳过未开启时不改动的用例\n");
    }

    // ---- 4) 实测开关：用 SPI_SETHIGHCONTRAST 临时打开，验证读取与应用真的跟随 ----
    // 这一步把"逻辑正确"变成"实测生效"。
    // 开关必须无条件还原，否则会改掉用户的系统设置。
    {
        HIGHCONTRASTW orig = {};
        orig.cbSize = sizeof(orig);
        const bool bGotOrig =
            ::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(orig), &orig, 0) != 0;

        HIGHCONTRASTW want = orig;
        want.cbSize = sizeof(want);
        want.dwFlags |= HCF_HIGHCONTRASTON;

        const bool bSet = bGotOrig &&
            ::SystemParametersInfoW(SPI_SETHIGHCONTRAST, sizeof(want), &want,
                                    SPIF_UPDATEINIFILE | SPIF_SENDCHANGE) != 0;

        Chk("能临时打开高对比度（探针权限允许）", bSet,
            bSet ? L"SPI_SETHIGHCONTRAST ok"
                 : L"SPI_SETHIGHCONTRAST failed，后续用例跳过");

        if (bSet)
        {
            Chk("打开后 IsHighContrast() 立即为 true",
                theme::IsHighContrast(), L"SPI 读回 true");

            // 真实控件树：建一个深色底的容器，应用后应被拉到系统色
            CContainerUI box;
            box.SetBkColor(0xFF101010);
            box.SetBorderColor(0xFF303030);

            const DWORD dwBefore = box.GetBkColor();
            const int nChanged = theme::ApplyHighContrast(&box);
            const DWORD dwAfter = box.GetBkColor();

            wchar_t buf[192] = {};
            ::swprintf_s(buf, L"before=%s after=%s changed=%d",
                         Hex(dwBefore).c_str(), Hex(dwAfter).c_str(), nChanged);
            Chk("应用后控件底色真的变了",
                dwBefore != dwAfter && nChanged >= 1, buf);

            wchar_t buf2[192] = {};
            ::swprintf_s(buf2, L"after=%s system=%s",
                         Hex(dwAfter).c_str(), Hex(theme::HighContrastBk()).c_str());
            Chk("应用后底色等于系统容器色",
                dwAfter == theme::HighContrastBk(), buf2);

            // 边框也要跟上，否则控件边界在高对比度下不可见
            Chk("应用后边框色等于系统前景色",
                box.GetBorderColor() == theme::HighContrastText(),
                Hex(box.GetBorderColor()));
        }

        // ---- 无条件还原 ----
        if (bGotOrig)
        {
            HIGHCONTRASTW back = orig;
            back.cbSize = sizeof(back);
            ::SystemParametersInfoW(SPI_SETHIGHCONTRAST, sizeof(back), &back,
                                    SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);

            HIGHCONTRASTW chk = {};
            chk.cbSize = sizeof(chk);
            const bool bRestored =
                ::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(chk), &chk, 0) != 0 &&
                ((chk.dwFlags & HCF_HIGHCONTRASTON) == (orig.dwFlags & HCF_HIGHCONTRASTON));
            Chk("探针结束后高对比度已还原", bRestored,
                bRestored ? L"已还原到探针开始前的状态"
                          : L"还原失败，请手动检查系统设置");
        }
    }

    ::printf("\n结果：%d 通过 / %d 失败\n", g_nPass, g_nFail);
    if (bInject)
    {
        Chk("injected-must-fail", false, L"self-test: forced failure");
    }
    return (g_nFail == 0) ? 0 : 1;
}
