// ============================================================
// dynamic_theme_probe.cpp —— 动态新增控件的主题覆盖（实测）
//
// 为什么必须单独验证：
//   第 12 轮在 EnsurePageBuilt() 里加了"页构建后重放主题"，但那只是
//   代码存在。本项目所有页面的行控件都是运行期 new 出来的
//   （clean_list 是 VerticalLayoutUI，行由 Page_Cleanup_Build 逐条创建），
//   /StartupCheck 与既有探针都只检查启动时已存在的控件树，
//   完全覆盖不到"后建的控件是否继承当前主题"。
//   这与 focusbordercolor 只设属性却不画像素是同一类风险：
//   "看着有兜底，实际没生效"。
//
// 判据（模拟真实时序）：
//   1) 先建空壳树 -> Apply(Dark) 建立基线
//   2) 之后再动态插入新控件（模拟页面懒构建）
//   3) 对新控件再 Apply(Dark)，断言其颜色与深色主题一致
//   4) 断言新控件颜色与"浅色原值"不同（否则说明根本没被改）
//
// 探针侧注意：SetDPI 必须在 AttachDialog 之后（第 10 轮踩过）。
// ============================================================

#include "UIlib.h"
#include "ThemeService.h"

#include <windows.h>
#include <gdiplus.h>
#include <cstdio>
#include <string>

#pragma comment(lib, "gdiplus.lib")

using namespace DuiLib;

namespace
{
int g_nPass = 0;
int g_nFail = 0;

void Mark(const char* s) { ::printf("[STEP] %s\n", s); ::fflush(stdout); }

void Chk(const char* pszName, bool bOk, const std::wstring& sDetail)
{
    char sz[512] = {};
    ::WideCharToMultiByte(CP_UTF8, 0, sDetail.c_str(),
                          static_cast<int>(sDetail.size()),
                          sz, sizeof(sz) - 1, nullptr, nullptr);
    if (bOk) { ++g_nPass; ::printf("[PASS] %s | %s\n", pszName, sz); }
    else     { ++g_nFail; ::printf("[FAIL] %s | %s\n", pszName, sz); }
    ::fflush(stdout);
}

std::wstring Hex(DWORD c)
{
    wchar_t b[16] = {};
    ::swprintf_s(b, L"#%08lX", static_cast<unsigned long>(c));
    return b;
}

// 深色主题下的期望值（与 ThemeService 的映射表保持一致）
constexpr DWORD kLightBk   = 0xFFFFFFFF;   // 皮肤常见底色
constexpr DWORD kLightText = 0xFF1B1B1B;   // 皮肤常见文字色
constexpr DWORD kDarkBk    = 0xFF202020;   // 对应深色
constexpr DWORD kDarkText  = 0xFFF2F2F2;   // 对应深色

} // namespace

int main(int argc, char** argv)
{
    // [GATE-SELFTEST] /Inject adds one deliberately failing assertion so this
    // probe can prove its verdict path still works; otherwise a check that
    // silently stopped asserting would look exactly like a passing run.
    bool bInject = false;
    for (int i = 1; i < argc; ++i)
        if (::lstrcmpiA(argv[i], "/Inject") == 0) bInject = true;

    Mark("1 begin");

    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gtok = 0;
    if (Gdiplus::GdiplusStartup(&gtok, &gsi, nullptr) != Gdiplus::Ok)
    {
        ::printf("[FATAL] GdiplusStartup\n");
        return 2;
    }

    HINSTANCE hInst = ::GetModuleHandleW(nullptr);
    CPaintManagerUI::SetInstance(hInst);
    Mark("2 gdiplus ok");

    const wchar_t* kCls = L"DynamicThemeProbeWnd";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = hInst;
    wc.lpszClassName = kCls;
    ::RegisterClassExW(&wc);

    HWND hWnd = ::CreateWindowExW(0, kCls, L"probe", WS_OVERLAPPEDWINDOW,
                                  0, 0, 600, 400, nullptr, nullptr, hInst, nullptr);
    if (hWnd == nullptr) { ::printf("[FATAL] CreateWindowEx\n"); return 2; }
    Mark("3 hwnd ok");

    CPaintManagerUI pm;
    pm.Init(hWnd, L"DynamicThemeProbe");
    Mark("4 init ok");

    // ---- 阶段 1：先建一个"启动时就存在"的容器并应用深色 ----
    auto* pRoot = new CVerticalLayoutUI();
    pRoot->SetName(L"probe_root");
    pRoot->SetBkColor(kLightBk);

    auto* pOld = new CTextUI();
    pOld->SetName(L"old_text");
    pOld->SetAttribute(L"textcolor", L"#FF1B1B1B");
    pOld->SetText(L"existing");
    pRoot->Add(pOld);

    pm.AttachDialog(pRoot);
    pm.SetDPI(96);   // 必须在 AttachDialog 之后
    Mark("5 attached");

    const int nFirst = theme::Apply(pRoot, theme::EMode::Dark);
    const DWORD dwOldAfter = pOld->GetTextColor();
    ::printf("[INFO] 首次 Apply(Dark) 改色控件数=%d, old_text=%ls\n",
             nFirst, Hex(dwOldAfter).c_str());
    ::fflush(stdout);

    Chk("启动期控件被改色", dwOldAfter == kDarkText,
        L"实际=" + Hex(dwOldAfter) + L" 期望=" + Hex(kDarkText));

    // ---- 阶段 2：模拟"页面懒构建"，之后才动态插入新控件 ----
    // 新控件带着皮肤的浅色值（就像 Page_Cleanup_Build 里 new CTextUI 那样）
    auto* pNew = new CTextUI();
    pNew->SetName(L"new_text");
    pNew->SetAttribute(L"textcolor", L"#FF1B1B1B");
    pNew->SetText(L"lazy-built");
    pRoot->Add(pNew);
    Mark("6 new control added after theme applied");

    const DWORD dwNewBefore = pNew->GetTextColor();
    ::printf("[INFO] 新控件刚插入时 textColor=%ls（仍是浅色值）\n",
             Hex(dwNewBefore).c_str());
    ::fflush(stdout);

    Chk("新控件初始为浅色（模拟懒构建）", dwNewBefore == kLightText,
        L"实际=" + Hex(dwNewBefore) + L" 期望=" + Hex(kLightText));

    // ---- 阶段 3：按 EnsurePageBuilt 的做法重放主题 ----
    theme::Apply(pRoot, theme::EMode::Dark);
    const DWORD dwNewAfter = pNew->GetTextColor();
    ::printf("[INFO] 重放主题后新控件 textColor=%ls\n",
             Hex(dwNewAfter).c_str());
    ::fflush(stdout);

    // 核心判据：动态新增的控件必须被重放覆盖到
    Chk("动态新增控件被主题覆盖", dwNewAfter == kDarkText,
        L"实际=" + Hex(dwNewAfter) + L" 期望=" + Hex(kDarkText) +
        L"（若仍是 " + Hex(kLightText) + L" 说明重放没覆盖到新控件）");

    // 已存在的控件不应被二次改变（幂等）
    Chk("重放对已有控件幂等", pOld->GetTextColor() == dwOldAfter,
        L"重放前=" + Hex(dwOldAfter) + L" 重放后=" + Hex(pOld->GetTextColor()));

    if (hWnd != nullptr) ::DestroyWindow(hWnd);
    ::UnregisterClassW(kCls, hInst);
    Gdiplus::GdiplusShutdown(gtok);

    ::printf("\n结果：%d 通过 / %d 失败\n", g_nPass, g_nFail);
    if (bInject)
    {
        Chk("injected-must-fail", false, L"self-test: forced failure");
    }
    return (g_nFail == 0) ? 0 : 1;
}
