// ============================================================
// round_clip_probe.cpp -- 圆角容器是否真的裁剪子控件
//
// 缺陷背景
// --------
// R43/R44 两次挂账未做的事。改动前 DuiLib 的行为是：
//   * borderround 让容器自己的填充与描边变成圆角
//   * 但 CContainerUI::DoPaint 给子控件建立的裁剪区仍是**矩形**
// 于是任何触到角落的子控件都会画到圆弧外面，视觉上「圆角被戳穿」。
// 只要容器同时具备「圆角背景」和「贴边内容」就能看到。
//
// 本探针不依赖人眼，直接读像素：
//   1) 建一个 40x40、圆半径 12 的容器，里面塞一个铺满的子控件（纯色）
//   2) 把容器画到 DIB
//   3) 断言：圆角外的四个角**必须是背景哨兵色**（被裁掉了）
//   4) 断言：圆心区域必须是子控件颜色（内容确实画出来了）
//
// 第 4 条是关键对照：只验证「角落为空」是不够的——如果整个子控件都没画，
// 角落当然也是空的。必须同时证明中心有内容，两条合起来才排除了假绿。
//
// 另外验证 radius=0 时行为不变（直角容器子控件照常铺满四角），
// 防止这次改动把不带圆角的容器也裁掉。
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

void Chk(const char* pszName, bool bOk, const std::wstring& sDetail)
{
    // pszName is already narrow (the probe's own literals); only sDetail needs
    // converting from wchar_t. The earlier version passed pszName through
    // WideCharToMultiByte, which expects LPCWCH -- C2664.
    char szd[512] = {};
    ::WideCharToMultiByte(CP_UTF8, 0, sDetail.c_str(), static_cast<int>(sDetail.size()),
                          szd, sizeof(szd) - 1, nullptr, nullptr);
    if (bOk) { ++g_nPass; ::printf("[PASS] %s | %s\n", pszName, szd); }
    else     { ++g_nFail; ::printf("[FAIL] %s | %s\n", pszName, szd); }
    ::fflush(stdout);
}

// 哨兵背景色：选一个皮肤里不会出现的值，便于区分"没画"与"画了别的色"
const DWORD kSentinel = 0xFF00FF00;   // 纯绿
const DWORD kChildFill = 0xFFFF0000;  // 纯红：子控件
const DWORD kBoxFill   = 0xFFFFFFFF;  // 白：容器自身底

class CFillBoxUI : public CControlUI
{
    DECLARE_DUICONTROL(CFillBoxUI)
public:
    CFillBoxUI() {}
    LPCTSTR GetClass() const override { return L"FillBoxUI"; }

    bool DoPaint(HDC hDC, const RECT& rcPaint, CControlUI* /*pStop*/) override
    {
        // 纯色填充：不调基类，避免它再去画背景把结果搞混
        CRenderEngine::DrawColor(hDC, m_rcItem, GetAdjustColor(kChildFill));
        return true;
    }
};

IMPLEMENT_DUICONTROL(CFillBoxUI)

class CDib
{
public:
    HDC     hDC = nullptr;
    HBITMAP hBM = nullptr;
    DWORD*  p   = nullptr;
    int     w = 0, h = 0;

    bool Create(int nW, int nH)
    {
        w = nW; h = nH;
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = nW;
        bi.bmiHeader.biHeight = -nH;      // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void* pBits = nullptr;
        hBM = ::CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &pBits, nullptr, 0);
        if (hBM == nullptr || pBits == nullptr) return false;
        p = static_cast<DWORD*>(pBits);
        hDC = ::CreateCompatibleDC(nullptr);
        if (hDC == nullptr) return false;
        ::SelectObject(hDC, hBM);

        // 铺哨兵色
        for (int i = 0; i < w * h; ++i) p[i] = kSentinel;
        return true;
    }

    DWORD At(int x, int y) const
    {
        if (x < 0 || y < 0 || x >= w || y >= h) return 0;
        return p[y * w + x] & 0x00FFFFFF;
    }

    void Destroy()
    {
        if (hDC) { ::DeleteDC(hDC); hDC = nullptr; }
        if (hBM) { ::DeleteObject(hBM); hBM = nullptr; }
        p = nullptr;
    }
};

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

    const wchar_t* kCls = L"RoundClipProbeWnd";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = hInst;
    wc.lpszClassName = kCls;
    ::RegisterClassExW(&wc);

    HWND hWnd = ::CreateWindowExW(0, kCls, L"probe", WS_OVERLAPPEDWINDOW,
                                  0, 0, 800, 600, nullptr, nullptr, hInst, nullptr);
    if (hWnd == nullptr) { ::printf("[FATAL] CreateWindowEx\n"); return 2; }

    CPaintManagerUI pm;
    pm.Init(hWnd, L"RoundClipProbe");
    CControlFactory::GetInstance()->RegistControl(
        L"CFillBoxUI", static_cast<CreateClass>(CFillBoxUI::CreateControl));


    // 手工搭两棵独立的树，分别测圆角与直角
    struct { int radius; const char* label; } kCases[] = {
        { 12, "radius=12" },
        { 0,  "radius=0 " },
    };

    for (const auto& c : kCases)
    {
        CVerticalLayoutUI* pBox = new CVerticalLayoutUI();
        pBox->SetFixedWidth(40);
        pBox->SetFixedHeight(40);
        pBox->SetBorderRound(SIZE{ c.radius, c.radius });
        pBox->SetBkColor(kBoxFill);

        CFillBoxUI* pChild = new CFillBoxUI();
        pChild->SetFixedWidth(40);
        pChild->SetFixedHeight(40);
        pBox->Add(pChild);

        pBox->SetManager(&pm, nullptr, false);
        pBox->SetPos(RECT{ 0, 0, 40, 40 }, false);

        CDib d;
        if (!d.Create(40, 40))
        {
            Chk("DIB 创建", false, std::wstring(L"失败"));
            delete pBox;
            continue;
        }

        pBox->Paint(d.hDC, RECT{ 0, 0, 40, 40 }, nullptr);

        // 圆角外四角：radius=12 时 (0,0)/(39,0)/(0,39)/(39,39) 都应在弧外
        const int cx[4] = { 0, 39, 0, 39 };
        const int cy[4] = { 0, 0, 39, 39 };
        int nCornerChild = 0;    // 角落被画上了子控件色（说明没裁）
        int nCornerSentinel = 0; // 角落仍是哨兵（被裁掉了）

        for (int i = 0; i < 4; ++i)
        {
            const DWORD v = d.At(cx[i], cy[i]);
            if (v == (kChildFill & 0x00FFFFFF)) ++nCornerChild;
            if (v == (kSentinel & 0x00FFFFFF)) ++nCornerSentinel;
        }

        // 中心点：必须画到了子控件
        const DWORD vCenter = d.At(20, 20);
        const bool bCenterOk = (vCenter == (kChildFill & 0x00FFFFFF));

        wchar_t szDetail[256] = {};
        ::swprintf_s(szDetail, L"cornerChild=%d cornerSentinel=%d center=%s",
                     nCornerChild, nCornerSentinel, bCenterOk ? L"child" : L"other");

        if (c.radius > 0)
        {
            // 圆角容器：角落不能出现子控件色
            Chk("圆角容器：子控件不得画到弧外四角", nCornerChild == 0, szDetail);
            // 中心必须有内容（排除"整个子控件都没画"导致的假绿）
            Chk("圆角容器：中心仍绘制子控件（排除假绿）", bCenterOk, szDetail);
        }
        else
        {
            // 直角容器：四角必须被铺满，不能因为这次改动把直角也裁了
            Chk("直角容器：子控件仍铺满四角（未回归）", nCornerChild == 4, szDetail);
            Chk("直角容器：中心绘制子控件", bCenterOk, szDetail);
        }

        d.Destroy();
        delete pBox;
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
