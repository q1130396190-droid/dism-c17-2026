// ============================================================
// focus_border_probe.cpp —— 焦点框是否真的画出来（像素级）
//
// 为什么必须做这一步：
//   只证明 focusbordercolor 被写进控件属性是不够的，"属性有值"不等于
//   "屏幕上有像素"。CControlUI::PaintBorder 的所有焦点分支都要求
//   nBorderSize > 0，而 m_nBorderSize 默认是 0，本项目按钮都没设
//   bordersize。于是属性设了、Tab 切换却一个像素都不画。
//
// 实现要点（前几版崩溃的原因已排除）：
//   * 派生类只暴露 PaintBorder，不走 DoPaint（后者会连带 PaintText，
//     未设字体时可能取到空字体）。
//   * 焦点用派生类直接置位 m_bFocused，绕开 PM 焦点机制
//     （CControlUI::SetFocus 会转调 m_pManager->SetFocus，未入树时崩）。
//   * 属性必须在入树前设（入树后 SetAttribute→Invalidate 走空父链）。
//   * 每步 printf 后 fflush，便于定位崩溃点。
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

class CProbeBtn : public CButtonUI
{
public:
    LPCTSTR GetClass() const override { return L"ProbeBtnUI"; }
    void LayoutFor(const RECT& rc) { m_rcItem = rc; m_rcPaint = rc; }
    void PaintBorderOnly(HDC hDC) { PaintBorder(hDC); }
    void SetFocusedForTest(bool b) { m_bFocused = b; }
};

struct DIB
{
    HDC     hDC;
    HBITMAP hBM;
    HBITMAP hOld;
    DWORD*  p;
    int     w;
    int     h;

    DIB() : hDC(nullptr), hBM(nullptr), hOld(nullptr), p(nullptr), w(0), h(0) {}

    bool Create(int cx, int cy, DWORD bg)
    {
        w = cx;
        h = cy;
        hDC = ::CreateCompatibleDC(nullptr);
        if (hDC == nullptr) return false;

        BITMAPINFO bi;
        ::ZeroMemory(&bi, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = cx;
        bi.bmiHeader.biHeight = -cy;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void* pv = nullptr;
        hBM = ::CreateDIBSection(hDC, &bi, DIB_RGB_COLORS, &pv, nullptr, 0);
        if (hBM == nullptr) return false;
        p = static_cast<DWORD*>(pv);
        hOld = static_cast<HBITMAP>(::SelectObject(hDC, hBM));
        for (int i = 0; i < cx * cy; ++i) p[i] = bg;
        return true;
    }

    int CountNonBg(DWORD bg) const
    {
        if (p == nullptr) return -1;
        int n = 0;
        for (int i = 0; i < w * h; ++i)
            if (p[i] != bg) ++n;
        return n;
    }

    void Destroy()
    {
        if (hDC != nullptr && hOld != nullptr) ::SelectObject(hDC, hOld);
        if (hBM != nullptr) ::DeleteObject(hBM);
        if (hDC != nullptr) ::DeleteDC(hDC);
        hDC = nullptr;
        hBM = nullptr;
        hOld = nullptr;
        p = nullptr;
    }
};

const DWORD kBg = 0xFF00FF00u;

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

    const wchar_t* kCls = L"FocusBorderProbeWnd";
    WNDCLASSEXW wc;
    ::ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = hInst;
    wc.lpszClassName = kCls;
    ::RegisterClassExW(&wc);

    HWND hWnd = ::CreateWindowExW(0, kCls, L"probe", WS_OVERLAPPEDWINDOW,
                                  0, 0, 400, 300, nullptr, nullptr, hInst, nullptr);
    if (hWnd == nullptr)
    {
        ::printf("[FATAL] CreateWindowEx\n");
        return 2;
    }
    Mark("3 hwnd ok");

    CPaintManagerUI pm;
    pm.Init(hWnd, L"FocusBorderProbe");
    // 注意：SetDPI 必须放在 AttachDialog 之后。
    // 在空控件树上调 SetDPI 会走 DPI 资源重建路径并崩溃 ——
    // 这是本探针前几版 0xC0000005 的真正根因，不是框架缺陷
    // （其他探针都在 AttachDialog 之后才 SetDPI）。
    Mark("4 pm init ok");

    const int W = 140;
    const int H = 44;
    RECT rc;
    rc.left = 0;
    rc.top = 0;
    rc.right = W;
    rc.bottom = H;

    CVerticalLayoutUI* pRoot = new CVerticalLayoutUI();
    pRoot->SetName(L"probe_root");
    pRoot->SetBkColor(0xFF202020u);

    CProbeBtn* pBtn = new CProbeBtn();
    pBtn->SetName(L"btn_probe");
    pBtn->SetAttribute(L"borderround", L"4,4");
    pBtn->SetAttribute(L"focusbordercolor", L"#FF1B1B1B");
    pBtn->SetAttribute(L"bkcolor", L"#FF3366CC");
    Mark("5 controls built");

    pRoot->Add(pBtn);
    Mark("6 added to root");

    pm.AttachDialog(pRoot);
    pm.SetDPI(96);   // 必须在 AttachDialog 之后
    Mark("7 attached");

    pBtn->LayoutFor(rc);
    Mark("8 laid out");

    ::printf("[INFO] focusColor=0x%08lX bordersize=%d borderround=%d\n",
             static_cast<unsigned long>(pBtn->GetFocusBorderColor()),
             pBtn->GetBorderSize(),
             static_cast<int>(pBtn->GetBorderRound().cx));
    ::fflush(stdout);

    int nOff = -1;
    {
        Mark("9 paint off start");
        pBtn->SetFocusedForTest(false);
        DIB d;
        if (d.Create(W, H, kBg))
        {
            pBtn->PaintBorderOnly(d.hDC);
            nOff = d.CountNonBg(kBg);
            d.Destroy();
        }
        Mark("10 paint off done");
    }

    int nOn = -1;
    {
        Mark("11 paint on start");
        pBtn->SetFocusedForTest(true);
        DIB d;
        if (d.Create(W, H, kBg))
        {
            pBtn->PaintBorderOnly(d.hDC);
            nOn = d.CountNonBg(kBg);
            d.Destroy();
        }
        Mark("12 paint on done");
    }

    ::printf("\n非焦点态描边像素 = %d (期望 0)\n", nOff);
    ::printf("焦点态描边像素   = %d (期望 >0)\n\n", nOn);
    ::fflush(stdout);

    Chk("非焦点态不画描边", nOff == 0, L"pixels=" + std::to_wstring(nOff));
    Chk("焦点态画出可见描边", nOn > 0,
        L"pixels=" + std::to_wstring(nOn) +
        L" 修复前恒为 0：bordersize 默认 0，焦点分支被整段跳过");

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
