// ============================================================
// list_theme_probe.cpp —— 列表项颜色是否随主题切换（实测）
//
// 为什么需要：CListLabelElementUI::DrawItemText 的文字色来自
// m_pOwner->GetListInfo()->dwTextColor，不是控件自己的 textcolor。
// 这意味着"按控件属性遍历"的 ThemeService 结构上覆盖不到列表行 ——
// 不改这里，深色模式下所有列表行文字仍是浅色主题的深灰，压在深底上看不清。
//
// 判据：加载皮肤 -> 找一个真实 CListUI -> 深色 Apply -> 读回
//   itemTextColor / selectedItemTextColor / itemBkColor
// 必须与原值不同；切回浅色必须还原。
//
// 注意（探针侧，前几轮踩过）：
//   * SetDPI 必须在 AttachDialog 之后调用，空树上调会崩溃。
//   * 属性/主题操作都在入树之后做，避免 Invalidate 走空父链。
// ============================================================

#include "UIlib.h"
#include "ThemeService.h"

#include <windows.h>
#include <gdiplus.h>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

using namespace DuiLib;

namespace
{
int g_nPass = 0;
int g_nFail = 0;

void Mark(const char* pszStep)
{
    ::printf("[STEP] %s\n", pszStep);
    ::fflush(stdout);
}

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

// 找第一个 CListUI（递归）
CListUI* FindFirstList(CControlUI* pRoot)
{
    if (pRoot == nullptr) return nullptr;
    if (auto* pl = dynamic_cast<CListUI*>(pRoot)) return pl;

    auto* pC = dynamic_cast<CContainerUI*>(pRoot);
    if (pC == nullptr) return nullptr;
    for (int i = 0; i < pC->GetCount(); ++i)
        if (CListUI* h = FindFirstList(pC->GetItemAt(i))) return h;
    return nullptr;
}

class CProbeNavUI : public COptionUI
{
    DECLARE_DUICONTROL(CProbeNavUI)
public:
    CProbeNavUI() {}
    LPCTSTR GetClass() const override { return L"NavOptionUI"; }
};

IMPLEMENT_DUICONTROL(CProbeNavUI)

} // namespace

int main()
{
    Mark("1 begin");

    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gtok = 0;
    if (Gdiplus::GdiplusStartup(&gtok, &gsi, nullptr) != Gdiplus::Ok)
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

    HINSTANCE hInst = ::GetModuleHandleW(nullptr);
    CPaintManagerUI::SetInstance(hInst);
    CPaintManagerUI::SetResourcePath((sRoot + L"\\bin64\\skin\\Win8Metro").c_str());

    const wchar_t* kCls = L"ListThemeProbeWnd";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = hInst;
    wc.lpszClassName = kCls;
    ::RegisterClassExW(&wc);

    HWND hWnd = ::CreateWindowExW(0, kCls, L"probe", WS_OVERLAPPEDWINDOW,
                                  0, 0, 1280, 900, nullptr, nullptr, hInst, nullptr);
    if (hWnd == nullptr) { ::printf("[FATAL] CreateWindowEx\n"); return 2; }
    Mark("2 hwnd ok");

    CPaintManagerUI pm;
    pm.Init(hWnd, L"ListThemeProbe");
    Mark("3 init ok");

    CControlFactory::GetInstance()->RegistControl(
        L"CNavOptionUI", static_cast<CreateClass>(CProbeNavUI::CreateControl));

    CDialogBuilder builder;
    CControlUI* pRoot = builder.Create(L"Main.xml", nullptr, nullptr, &pm);
    if (pRoot == nullptr) { ::printf("[FATAL] Main.xml parse failed\n"); return 2; }
    pm.AttachDialog(pRoot);
    pm.SetDPI(96);   // 必须在 AttachDialog 之后
    Mark("4 attached");

    CListUI* pList = FindFirstList(pRoot);
    if (pList == nullptr)
    {
        ::printf("[FATAL] no CListUI found in skin\n");
        return 2;
    }
    Mark("5 list found");

    const DWORD t0 = pList->GetItemTextColor();
    const DWORD s0 = pList->GetSelectedItemTextColor();
    const DWORD b0 = pList->GetItemBkColor();

    ::printf("[INFO] 浅色原值 itemText=%ls selText=%ls itemBk=%ls\n",
             Hex(t0).c_str(), Hex(s0).c_str(), Hex(b0).c_str());
    ::fflush(stdout);

    // ---- 深色 ----
    const int nDark = theme::Apply(pRoot, theme::EMode::Dark);
    const DWORD tD = pList->GetItemTextColor();
    const DWORD sD = pList->GetSelectedItemTextColor();
    const DWORD bD = pList->GetItemBkColor();
    ::printf("[INFO] 深色后 itemText=%ls selText=%ls itemBk=%ls (changed=%d)\n",
             Hex(tD).c_str(), Hex(sD).c_str(), Hex(bD).c_str(), nDark);
    ::fflush(stdout);

    Chk("列表项文字色随主题切换", tD != t0,
        L"浅=" + Hex(t0) + L" 深=" + Hex(tD));
    Chk("列表选中项文字色随主题切换", sD != s0,
        L"浅=" + Hex(s0) + L" 深=" + Hex(sD));

    // ---- 切回浅色，必须还原 ----
    theme::Apply(pRoot, theme::EMode::Light);
    const DWORD tL = pList->GetItemTextColor();
    const DWORD sL = pList->GetSelectedItemTextColor();
    ::printf("[INFO] 还原后 itemText=%ls selText=%ls\n",
             Hex(tL).c_str(), Hex(sL).c_str());
    ::fflush(stdout);

    Chk("列表项文字色可逆还原", tL == t0, L"还原=" + Hex(tL) + L" 原始=" + Hex(t0));
    Chk("列表选中项文字色可逆还原", sL == s0, L"还原=" + Hex(sL) + L" 原始=" + Hex(s0));

    if (hWnd != nullptr) ::DestroyWindow(hWnd);
    ::UnregisterClassW(kCls, hInst);
    Gdiplus::GdiplusShutdown(gtok);

    ::printf("\n结果：%d 通过 / %d 失败\n", g_nPass, g_nFail);
    return (g_nFail == 0) ? 0 : 1;
}
