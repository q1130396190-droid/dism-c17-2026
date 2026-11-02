// ============================================================
// caption_btn_probe.cpp —— 标题栏按钮字形可见性（像素级）
//
// 用户反馈："放大、缩小、退出按钮都不正常显示"。
//
// 为什么必须用像素判据：
//   原来的实现用皮肤 <Button text="&#xE921;" font="10">，依赖
//   Segoe Fluent Icons 字体 + GDI 绘制单字符。实测整窗渲染里
//   三个按钮合计只有约 100 个非背景像素——最小化那条横线仅 1px 高，
//   肉眼几乎看不见。而"字体缺失"时字形整块消失，且不报错。
//
// 现在的实现改成 GDI+ 自绘（CaptionBtn.cpp），线宽 1px、10x10 网格。
// 本探针直接实例化真实类、绘制到 DIB、读回像素，验证：
//   1) 每个按钮常态下有可见字形（非背景像素数达到合理量级）
//   2) 悬停态有整块底色（Win11 特征）
//   3) 关闭按钮悬停为红底白字形
// ============================================================

#include "UIlib.h"
#include "CaptionBtn.h"
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

    // 统计与哨兵底色不同的像素（= 被绘制到的像素）
    int CountDrawn(DWORD bg) const
    {
        if (!p) return -1;
        int n = 0;
        const DWORD b = bg & 0x00FFFFFF;
        for (int i = 0; i < w * h; ++i)
        {
            const DWORD c = p[i] & 0x00FFFFFF;
            if (c != b) ++n;
        }
        return n;
    }

    // 统计某颜色（容差）的像素数
    int CountColor(DWORD target, int tol = 30) const
    {
        if (!p) return -1;
        const int tr = (target >> 16) & 0xFF, tg = (target >> 8) & 0xFF, tb = target & 0xFF;
        int n = 0;
        for (int i = 0; i < w * h; ++i)
        {
            const DWORD c = p[i];
            const int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
            if (abs(r - tr) <= tol && abs(g - tg) <= tol && abs(b - tb) <= tol) ++n;
        }
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

// 哨兵底色：洋红，与黑/白/红都远离，便于统计
const DWORD kSentinel = 0xFFFF00FFu;
const int BW = 46, BH = 37;

// 构造一个按钮并绘制一次，返回绘制像素数
// kind: 0=min 1=max 2=restore 3=close
int RenderButton(CPaintManagerUI& pm, int kind, bool bHot, bool bPushed,
                 DWORD* pOutBgHit = nullptr, DWORD dwProbeColor = 0)
{
    CCaptionBtnUI btn;
    wchar_t szKind[8] = {};
    ::swprintf_s(szKind, L"%d", kind);
    btn.SetAttribute(L"capkind", szKind);
    btn.SetManager(&pm, nullptr, false);

    RECT rc = { 0, 0, BW, BH };
    btn.SetPos(rc, false);

    // 用公开接口驱动状态：DoEvent 是虚函数，可直接投递事件
    TEventUI ev;
    ::ZeroMemory(&ev, sizeof(ev));
    ev.pSender = &btn;
    ev.Type = UIEVENT_MOUSEENTER;
    if (bHot) btn.DoEvent(ev);
    ev.Type = UIEVENT_BUTTONDOWN;
    if (bPushed) btn.DoEvent(ev);

    DIB d;
    if (!d.Create(BW, BH, kSentinel)) return -1;
    btn.DoPaint(d.hDC, rc, nullptr);
    const int nDrawn = d.CountDrawn(kSentinel);
    if (pOutBgHit != nullptr && dwProbeColor != 0)
        *pOutBgHit = static_cast<DWORD>(d.CountColor(dwProbeColor));
    d.Destroy();
    return nDrawn;
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

    const wchar_t* kCls = L"CaptionBtnProbeWnd";
    WNDCLASSEXW wc; ::ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = hInst;
    wc.lpszClassName = kCls;
    ::RegisterClassExW(&wc);
    HWND hWnd = ::CreateWindowExW(0, kCls, L"probe", WS_OVERLAPPEDWINDOW,
                                  0, 0, 400, 300, nullptr, nullptr, hInst, nullptr);
    if (!hWnd) { ::printf("[FATAL] CreateWindowEx\n"); return 2; }

    CPaintManagerUI pm;
    pm.Init(hWnd, L"CaptionBtnProbe");
    // 不要在空控件树上调 SetDPI：它会走 DPI 资源重建路径并崩溃
    // （0xC0000005，第 10 轮已踩过同一坑）。DPI 默认即 96，本探针无需设置。
    Mark("2 pm ready");

    // ---- 1) 常态字形可见 ----
    // 阈值必须按字形几何算，不能一刀切：
    //   最小化 = 一条 10px 长、约 1.4px 粗的横线 → 物理上限约 20~28 像素
    //   最大化 = 10x10 方框（4 条边）→ 约 60~90
    //   还原   = 错位双框 → 约 80~110
    //   关闭   = 两条对角线 → 约 55~80
    // 早期统一用 >=30，对横线不成立（24 像素已是该形状的上限），
    // 属判据错误而非按钮缺陷 —— 用"是否达到该形状的合理下限"替代。
    struct { int kind; const char* name; int minPixels; } kCases[] = {
        { 0, "minbtn (minimize)", 14 },
        { 1, "maxbtn (maximize)", 50 },
        { 2, "restorebtn (restore)", 60 },
        { 3, "closebtn (close)", 45 },
    };

    for (const auto& c : kCases)
    {
        const int n = RenderButton(pm, c.kind, false, false);
        char szLabel[64] = {};
        ::sprintf_s(szLabel, "%s 常态字形", c.name);

        char szDetail[128] = {};
        ::sprintf_s(szDetail, "drawnPixels=%d (expect >=%d for this shape)",
                    n, c.minPixels);

        const bool bOk = (n >= c.minPixels);
        if (bOk) { ++g_nPass; ::printf("[PASS] %s | %s\n", szLabel, szDetail); }
        else     { ++g_nFail; ::printf("[FAIL] %s | %s\n", szLabel, szDetail); }
        ::fflush(stdout);
    }

    // ---- 2) 悬停态必须有整块底色（Win11 特征）----
    {
        const int nNormal = RenderButton(pm, 1, false, false);
        const int nHot    = RenderButton(pm, 1, true,  false);
        // 悬停时整块 46x37=1702 被浅灰底覆盖 → 绘制像素数应大幅上升
        wchar_t szW[200] = {};
        ::swprintf_s(szW, L"normal=%d hot=%d (hot should be much larger)",
                     nNormal, nHot);
        Chk("悬停态出现整块底色", nHot > nNormal * 3, szW);
    }

    // ---- 3) 关闭按钮悬停 = 红底 + 白字形 ----
    {
        DWORD nRed = 0;
        const DWORD kWin11Red = 0xFFE81123;
        const int n = RenderButton(pm, 3, true, false, &nRed, kWin11Red);
        // Chk 需要 std::wstring，这里用宽字符缓冲（曾误传 char[] 导致 C2664）
        wchar_t szW[200] = {};
        ::swprintf_s(szW, L"drawn=%d redPixels=%lu (Win11 #E81123)",
                     n, (unsigned long)nRed);
        Chk("关闭悬停为 Win11 红底", nRed > 1000, szW);
    }

    ::DestroyWindow(hWnd);
    ::UnregisterClassW(kCls, hInst);
    Gdiplus::GdiplusShutdown(gtok);

    ::printf("\n结果：%d 通过 / %d 失败\n", g_nPass, g_nFail);
    ::fflush(stdout);
    if (bInject)
    {
        Chk("injected-must-fail", false, L"self-test: forced failure");
    }
    return (g_nFail == 0) ? 0 : 1;
}
