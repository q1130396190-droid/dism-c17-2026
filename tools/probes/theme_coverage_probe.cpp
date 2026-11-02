// ============================================================
// theme_coverage_probe.cpp —— 证明"新增的状态色覆盖真的生效"
//
// 为什么需要这个探针：
//   ThemeService 的返回值统计的是【被改动的控件数】，不是属性数。
//   扩展了按钮 hover/pushed 色、RichEdit 文字色之后，控件数仍是 318 ——
//   这在数值上完全正常，但也意味着"318 不变"无法证明扩展有效。
//   若不做这一步，扩展代码即使一行都没跑到，外部观察也看不出来。
//
// 判据（直接读回属性值）：
//   1) CButtonUI 的 hotBkColor / pushedBkColor 在深色模式下必须变成
//      映射表里的深色值（而不是皮肤原始的 #F3F3F3 / #EDEDED）
//   2) 切回浅色必须还原为原始值（证明基线驱动可逆）
//   3) CButtonUI 的 hotTextColor 等若原始为 0（未设置）必须保持 0，
//      不能被"顺手改成某个深色值"
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

void Chk(const char* pszName, bool bOk, const std::wstring& sDetail)
{
    char sz[512] = {};
    ::WideCharToMultiByte(CP_UTF8, 0, sDetail.c_str(),
                          static_cast<int>(sDetail.size()),
                          sz, sizeof(sz) - 1, nullptr, nullptr);
    if (bOk) { ++g_nPass; ::printf("[PASS] %s | %s\n", pszName, sz); }
    else     { ++g_nFail; ::printf("[FAIL] %s | %s\n", pszName, sz); }
}

CControlUI* FindByName(CControlUI* pRoot, const wchar_t* pszName)
{
    if (pRoot == nullptr || pszName == nullptr) return nullptr;
    if (pRoot->GetName().Compare(pszName) == 0) return pRoot;

    CContainerUI* pContainer = dynamic_cast<CContainerUI*>(pRoot);
    if (pContainer == nullptr) return nullptr;

    for (int i = 0; i < pContainer->GetCount(); ++i)
    {
        CControlUI* pChild = pContainer->GetItemAt(i);
        if (pChild == nullptr) continue;
        if (CControlUI* pHit = FindByName(pChild, pszName)) return pHit;
    }
    return nullptr;
}

std::wstring Hex(DWORD c)
{
    wchar_t buf[16] = {};
    ::swprintf_s(buf, L"0x%08lX", static_cast<unsigned long>(c));
    return buf;
}

class CProbeNavOptionUI : public COptionUI
{
    DECLARE_DUICONTROL(CProbeNavOptionUI)
public:
    CProbeNavOptionUI() {}
    LPCTSTR GetClass() const override { return L"NavOptionUI"; }
};

IMPLEMENT_DUICONTROL(CProbeNavOptionUI)

} // namespace

int main(int argc, char** argv)
{
    // [GATE-SELFTEST] /Inject adds one deliberately failing assertion so this
    // probe can prove its verdict path still works; otherwise a check that
    // silently stopped asserting would look exactly like a passing run.
    bool bInject = false;
    for (int i = 1; i < argc; ++i)
        if (::lstrcmpiA(argv[i], "/Inject") == 0) bInject = true;

    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR tok = 0;
    if (Gdiplus::GdiplusStartup(&tok, &gsi, nullptr) != Gdiplus::Ok)
    {
        ::printf("[FATAL] GdiplusStartup\n");
        return 2;
    }

    wchar_t szExe[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, szExe, MAX_PATH);
    std::wstring sDir(szExe);
    const size_t nSlash = sDir.find_last_of(L"\\/");
    if (nSlash != std::wstring::npos) sDir.resize(nSlash);

    std::wstring sRoot = sDir;
    for (int i = 0; i < 2; ++i)
    {
        const size_t p = sRoot.find_last_of(L"\\/");
        if (p != std::wstring::npos) sRoot.resize(p);
    }
    const std::wstring sSkin = sRoot + L"\\bin64\\skin\\Win8Metro";

    ::printf("=== 主题覆盖范围验证（状态色 / 富文本）===\n\n");

    HINSTANCE hInst = ::GetModuleHandleW(nullptr);
    CPaintManagerUI::SetInstance(hInst);
    CPaintManagerUI::SetResourcePath(sSkin.c_str());

    const wchar_t* kCls = L"ThemeCoverageProbeWnd";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = hInst;
    wc.lpszClassName = kCls;
    ::RegisterClassExW(&wc);

    HWND hWnd = ::CreateWindowExW(0, kCls, L"probe", WS_OVERLAPPEDWINDOW,
                                  0, 0, 1280, 900, nullptr, nullptr, hInst, nullptr);
    if (hWnd == nullptr) { ::printf("[FATAL] CreateWindowEx\n"); return 2; }

    CPaintManagerUI pm;
    pm.Init(hWnd, L"ThemeCoverageProbe");

    CControlFactory::GetInstance()->RegistControl(
        L"CNavOptionUI", static_cast<CreateClass>(CProbeNavOptionUI::CreateControl));

    CDialogBuilder builder;
    CControlUI* pRoot = builder.Create(L"Main.xml", nullptr, nullptr, &pm);
    Chk("Main.xml 解析成功", pRoot != nullptr, L"");
    if (pRoot == nullptr) return 2;

    pm.AttachDialog(pRoot);

    // ---- 受测控件：必须有真实的三态底色 ----
    // 不用 menu_file：它的 bkcolor 是全透明 #00000000 且没有 pushedbkcolor，
    // 拿它断言"主底色/pushed 随主题切换"必然失败——那是断言选错了对象，
    // 不是主题服务有缺陷。btn_clean 有 bkcolor / hotbkcolor / pushedbkcolor
    // 三个真实值（#FF0067C0 / #FF0078D4 / #FF005A9E），才能验证本版新增覆盖。
    auto* pBtn = dynamic_cast<CButtonUI*>(FindByName(pRoot, L"btn_clean"));
    if (pBtn == nullptr)
    {
        Chk("找到受测按钮 btn_clean", false, L"未找到");
        return 2;
    }
    Chk("找到受测按钮 btn_clean", true, L"");
    pm.SetDPI(96);

    // 记录浅色原始值
    const DWORD bk0    = pBtn->GetBkColor();
    const DWORD hot0   = pBtn->GetHotBkColor();
    const DWORD push0  = pBtn->GetPushedBkColor();
    const DWORD text0  = pBtn->GetTextColor();
    const DWORD htext0 = pBtn->GetHotTextColor();

    ::printf("浅色原始值: bk=%ls hot=%ls pushed=%ls text=%ls hotText=%ls\n\n",
             Hex(bk0).c_str(), Hex(hot0).c_str(), Hex(push0).c_str(),
             Hex(text0).c_str(), Hex(htext0).c_str());

    // ---- 切深色 ----
    const int nDark = theme::Apply(pRoot, theme::EMode::Dark);

    const DWORD bkD    = pBtn->GetBkColor();
    const DWORD hotD   = pBtn->GetHotBkColor();
    const DWORD pushD  = pBtn->GetPushedBkColor();
    const DWORD textD  = pBtn->GetTextColor();
    const DWORD htextD = pBtn->GetHotTextColor();

    ::printf("深色改后  : bk=%ls hot=%ls pushed=%ls text=%ls hotText=%ls (改色控件数=%d)\n\n",
             Hex(bkD).c_str(), Hex(hotD).c_str(), Hex(pushD).c_str(),
             Hex(textD).c_str(), Hex(htextD).c_str(), nDark);

    // 判据 1：hot/pushed 底色必须变化（这正是本版新增的覆盖）
    Chk("hover 底色随主题切换", hotD != hot0,
        L"浅色=" + Hex(hot0) + L" 深色=" + Hex(hotD));

    Chk("pushed 底色随主题切换", pushD != push0,
        L"浅色=" + Hex(push0) + L" 深色=" + Hex(pushD));

    // 判据 2：主底色与文字色也应变化（首版已有能力，回归确认）
    Chk("主底色随主题切换", bkD != bk0,
        L"浅色=" + Hex(bk0) + L" 深色=" + Hex(bkD));

    Chk("文字色随主题切换", textD != text0,
        L"浅色=" + Hex(text0) + L" 深色=" + Hex(textD));

    // 判据 3：未设置的 hotText（值为 0）不得被无中生有地改成深色值
    Chk("未设置的 hotText 保持 0", htext0 == 0 && htextD == 0,
        L"浅色=" + Hex(htext0) + L" 深色=" + Hex(htextD));

    // ---- 切回浅色，必须完全还原 ----
    const int nLight = theme::Apply(pRoot, theme::EMode::Light);

    const DWORD bkL   = pBtn->GetBkColor();
    const DWORD hotL  = pBtn->GetHotBkColor();
    const DWORD pushL = pBtn->GetPushedBkColor();
    const DWORD textL = pBtn->GetTextColor();

    ::printf("浅色还原  : bk=%ls hot=%ls pushed=%ls text=%ls (改色控件数=%d)\n\n",
             Hex(bkL).c_str(), Hex(hotL).c_str(), Hex(pushL).c_str(),
             Hex(textL).c_str(), nLight);

    Chk("主底色可逆还原", bkL == bk0, L"还原=" + Hex(bkL) + L" 原始=" + Hex(bk0));
    Chk("hover 底色可逆还原", hotL == hot0, L"还原=" + Hex(hotL) + L" 原始=" + Hex(hot0));
    Chk("pushed 底色可逆还原", pushL == push0, L"还原=" + Hex(pushL) + L" 原始=" + Hex(push0));
    Chk("文字色可逆还原", textL == text0, L"还原=" + Hex(textL) + L" 原始=" + Hex(text0));

    // 还原过程本身会改动控件（318 个），这不叫空操作。
    // 空操作的判据是：已经在浅色时再应用一次浅色，必须一个都不改。
    const int nLightAgain = theme::Apply(pRoot, theme::EMode::Light);
    Chk("已在浅色时再应用浅色 = 空操作", nLightAgain == 0,
        L"改色控件数=" + std::to_wstring(nLightAgain) +
        L"（还原时改 " + std::to_wstring(nLight) + L" 个属正常）");

    ::DestroyWindow(hWnd);
    ::UnregisterClassW(kCls, hInst);
    Gdiplus::GdiplusShutdown(tok);

    ::printf("\n结果：%d 通过 / %d 失败\n", g_nPass, g_nFail);
    if (bInject)
    {
        Chk("injected-must-fail", false, L"self-test: forced failure");
    }
    return (g_nFail == 0) ? 0 : 1;
}
