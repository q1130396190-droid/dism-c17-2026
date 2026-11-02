// ============================================================
// nav_diag_probe.cpp —— 诊断 nav_0 为何完全不绘制
//
// 起因：主窗口渲染图里「空间回收」这一项（nav_0，selected=true 的当前页）
// 所在槽位 100% 纯白、只有 1 种颜色，连文字都没画出来。而其余 nav_* 正常。
//
// 需要区分三种可能：
//   1) 控件没拿到布局矩形（GetPos 全 0）→ Paint 里 IntersectRect 直接失败
//   2) 控件不可见（IsVisible=false）
//   3) 可见且已布局，但绘制逻辑自身没画（颜色/状态判定问题）
// ============================================================

#include "UIlib.h"

#include <windows.h>
#include <gdiplus.h>
#include <cstdio>
#include <string>

#pragma comment(lib, "gdiplus.lib")

using namespace DuiLib;

namespace
{
void Mark(const char* s) { ::printf("[STEP] %s\n", s); ::fflush(stdout); }

CControlUI* FindByName(CControlUI* pRoot, const wchar_t* pszName)
{
    if (pRoot == nullptr || pszName == nullptr) return nullptr;
    if (pRoot->GetName().Compare(pszName) == 0) return pRoot;
    auto* pC = dynamic_cast<CContainerUI*>(pRoot);
    if (pC == nullptr) return nullptr;
    for (int i = 0; i < pC->GetCount(); ++i)
        if (CControlUI* h = FindByName(pC->GetItemAt(i), pszName)) return h;
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

struct DIB
{
    HDC hDC; HBITMAP hBM, hOld; DWORD* p; int w, h;
    DIB() : hDC(nullptr), hBM(nullptr), hOld(nullptr), p(nullptr), w(0), h(0) {}
    bool Create(int cx, int cy, DWORD bg)
    {
        w = cx; h = cy;
        hDC = ::CreateCompatibleDC(nullptr);
        if (!hDC) return false;
        BITMAPINFO bi; ::ZeroMemory(&bi, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = cx; bi.bmiHeader.biHeight = -cy;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* pv = nullptr;
        hBM = ::CreateDIBSection(hDC, &bi, DIB_RGB_COLORS, &pv, nullptr, 0);
        if (!hBM) return false;
        p = static_cast<DWORD*>(pv);
        hOld = static_cast<HBITMAP>(::SelectObject(hDC, hBM));
        for (int i = 0; i < cx * cy; ++i) p[i] = bg;
        return true;
    }
    int CountNonBg(DWORD bg) const
    {
        if (!p) return -1;
        int n = 0;
        for (int i = 0; i < w * h; ++i)
            if ((p[i] & 0x00FFFFFF) != (bg & 0x00FFFFFF)) ++n;
        return n;
    }
    void Destroy()
    {
        if (hDC && hOld) ::SelectObject(hDC, hOld);
        if (hBM) ::DeleteObject(hBM);
        if (hDC) ::DeleteDC(hDC);
        hDC = nullptr; hBM = nullptr; hOld = nullptr; p = nullptr;
    }
};

const DWORD kBg = 0xFF00FF00u;

} // namespace

int main()
{
    Mark("1 begin");
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gtok = 0;
    if (Gdiplus::GdiplusStartup(&gtok, &gsi, nullptr) != Gdiplus::Ok) return 2;

    wchar_t szExe[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, szExe, MAX_PATH);
    std::wstring sDir(szExe);
    size_t nSlash = sDir.find_last_of(L"\\/");
    if (nSlash != std::wstring::npos) sDir.resize(nSlash);
    std::wstring sRoot = sDir;
    for (int i = 0; i < 2; ++i)
    {
        size_t p = sRoot.find_last_of(L"\\/");
        if (p != std::wstring::npos) sRoot.resize(p);
    }

    HINSTANCE hInst = ::GetModuleHandleW(nullptr);
    CPaintManagerUI::SetInstance(hInst);
    CPaintManagerUI::SetResourcePath((sRoot + L"\\bin64\\skin\\Win8Metro").c_str());

    const wchar_t* kCls = L"NavDiagWnd";
    WNDCLASSEXW wc; ::ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = hInst; wc.lpszClassName = kCls;
    ::RegisterClassExW(&wc);
    HWND hWnd = ::CreateWindowExW(0, kCls, L"diag", WS_OVERLAPPEDWINDOW,
                                  0, 0, 1280, 900, nullptr, nullptr, hInst, nullptr);
    if (!hWnd) return 2;

    CPaintManagerUI pm;
    pm.Init(hWnd, L"NavDiag");
    CControlFactory::GetInstance()->RegistControl(
        L"CNavOptionUI", static_cast<CreateClass>(CProbeNavUI::CreateControl));

    CDialogBuilder builder;
    CControlUI* pRoot = builder.Create(L"Main.xml", nullptr, nullptr, &pm);
    if (!pRoot) { ::printf("[FATAL] parse failed\n"); return 2; }
    pm.AttachDialog(pRoot);
    pm.SetDPI(96);
    Mark("2 attached");

    RECT rcAll = { 0, 0, 1280, 900 };
    pRoot->SetPos(rcAll);
    Mark("3 laid out");

    // 逐个 nav 打印状态
    for (int i = 0; i <= 3; ++i)
    {
        wchar_t szName[32] = {};
        ::swprintf_s(szName, L"nav_%d", i);
        CControlUI* p = FindByName(pRoot, szName);
        if (!p) { ::printf("nav_%d: NOT FOUND\n", i); continue; }

        const RECT r = p->GetPos();
        auto* po = dynamic_cast<COptionUI*>(p);
        ::printf("nav_%d: pos=(%ld,%ld,%ld,%ld) w=%ld h=%ld visible=%d enabled=%d selected=%d bk=0x%08lX selBk=0x%08lX text=\"%ls\"\n",
                 i, r.left, r.top, r.right, r.bottom,
                 r.right - r.left, r.bottom - r.top,
                 p->IsVisible() ? 1 : 0, p->IsEnabled() ? 1 : 0,
                 (po && po->IsSelected()) ? 1 : 0,
                 (unsigned long)p->GetBkColor(),
                 (unsigned long)(po ? po->GetSelectBkColor() : 0),
                 p->GetText().GetData());
        ::fflush(stdout);

        // 单独绘制到自身尺寸画布
        const int cw = r.right - r.left, ch = r.bottom - r.top;
        if (cw > 0 && ch > 0)
        {
            DIB d;
            if (d.Create(cw, ch, kBg))
            {
                RECT rcLocal = { 0, 0, cw, ch };
                p->SetPos(rcLocal, false);
                p->Paint(d.hDC, rcLocal, nullptr);
                ::printf("        solo-paint pixels=%d\n", d.CountNonBg(kBg));
                ::fflush(stdout);
                d.Destroy();
            }
        }
        else
        {
            ::printf("        SKIP solo-paint: zero size\n");
            ::fflush(stdout);
        }
    }

    ::DestroyWindow(hWnd);
    ::UnregisterClassW(kCls, hInst);
    Gdiplus::GdiplusShutdown(gtok);
    return 0;
}
