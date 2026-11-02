// ============================================================
// font_fallback_probe.cpp —— 字体回退守卫的可证伪验证
//
// 为什么需要这个探针：
//   本机是 Win11，Segoe Fluent Icons 存在，因此壳层的回退分支
//   "不会被执行"——分支里的代码等于从未跑过。这类"永远为真/永远为假"
//   的条件是典型的假绿来源：看上去有兜底，实际可能永远不触发，或者
//   永远触发（把 Win11 的正常路径也改坏）。
//
//   本探针直接验证判据函数本身：
//     1) 真实存在的字体 → 必须返回 true
//     2) 不存在的字体   → 必须返回 false（这才让回退分支有机会执行）
//     3) 空串 / nullptr → 必须返回 false（不能崩）
//   只要 2) 成立，就证明"Win10 上会走回退"这一推论是可成立的；
//   反之若它恒返回 true，回退分支就是死代码。
// ============================================================

#include "StartupCheck.h"

#include <windows.h>
#include <cstdio>
#include <string>

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

void Expect(const char* pszLabel, const wchar_t* pszFont, bool bWant)
{
    const bool bGot = startupcheck::IsFontFamilyAvailable(pszFont);
    std::wstring sDetail = pszFont ? (std::wstring(L"\"") + pszFont + L"\"") : L"(null)";
    sDetail += bWant ? L" 期望存在" : L" 期望不存在";
    sDetail += bGot ? L" → 实际存在" : L" → 实际不存在";
    Chk(pszLabel, bGot == bWant, sDetail);
}

} // namespace

int main()
{
    ::printf("=== 字体回退守卫验证（IsFontFamilyAvailable）===\n\n");

    // 1) Win11 自带图标字体：本机必须存在
    Expect("Win11 图标字体可用", L"Segoe Fluent Icons", true);

    // 2) Win10 兜底字体：码位与 Fluent Icons 对齐
    Expect("Win10 兜底字体可用", L"Segoe MDL2 Assets", true);

    // 3) 界面正文字体
    Expect("界面字体 Microsoft YaHei 可用", L"Microsoft YaHei", true);

    // 4) 关键：不存在的字体必须返回 false。
    //    这一条决定回退分支是不是活代码。
    Expect("虚构字体必须判定为不存在", L"__NoSuchFontFamily_9F3A__", false);
    Expect("空串必须判定为不存在", L"", false);

    // 5) nullptr 不能崩
    Expect("nullptr 必须安全返回 false", nullptr, false);

    ::printf("\n结果：%d 通过 / %d 失败\n", g_nPass, g_nFail);
    return (g_nFail == 0) ? 0 : 1;
}
