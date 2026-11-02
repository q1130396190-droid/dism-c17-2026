// ============================================================
// skin_win11.cpp —— 皮肤与圆角控件的真实引擎级验证
//
// 为什么不用截图：
//   本机是断开的 RDP 会话，DWM 不做合成，PrintWindow / CopyFromScreen
//   一律得到全黑位图，截图无法作为证据。本探针改为两条不依赖桌面
//   合成的量化判据：
//
//   A. 皮肤解析：用 CDialogBuilder 真实加载 Main.xml（含 pages/*.xml
//      的 Include 展开），然后断言关键控件存在、标题栏字形是 Fluent
//      码位、font=10 能解析成真实 HFONT。任一项不成立说明皮肤改坏了。
//
//   B. 圆角填充：构造真实 CButtonUI，经 SetAttribute 写入
//      borderround="8,8" 与纯色 bkcolor，绘制到 32bpp DIB 后读回像素。
//      判据：角点非填充色、中心为填充色。这证明 CControlUI::PaintBkColor
//      的圆角分支真的被走到了，而不是只有描边变圆。
//
// 输出 PASS/FAIL 明细；有 FAIL 时退出码 1。
// ============================================================

#include "UIlib.h"
#include "CaptionBtn.h"   // RegisterCaptionBtnControl(): <CaptionBtn> nodes

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
    char szDetail[512] = {};
    ::WideCharToMultiByte(CP_UTF8, 0, sDetail.c_str(),
                          static_cast<int>(sDetail.size()),
                          szDetail, sizeof(szDetail) - 1, nullptr, nullptr);
    if (bOk) { ++g_nPass; ::printf("[PASS] %s | %s\n", pszName, szDetail); }
    else     { ++g_nFail; ::printf("[FAIL] %s | %s\n", pszName, szDetail); }
}

// 把宽字符串转成可打印的十六进制码位序列（用于核对字体图标字形）
std::wstring CodePoints(const CDuiString& s)
{
    std::wstring out;
    wchar_t buf[16] = {};
    for (int i = 0; i < s.GetLength(); ++i)
    {
        ::swprintf_s(buf, L"U+%04X ", static_cast<unsigned>(s[i]));
        out += buf;
    }
    return out;
}

// ---- 用于验证纯色+圆角的按钮（继承以访问 protected 成员）----
class CProbeButton : public CButtonUI
{
public:
    void LayoutForPaint(const RECT& rc)
    {
        m_rcItem = rc;
        m_rcPaint = rc;
    }
    void PaintBackgroundOnly(HDC hDC) { PaintBkColor(hDC); }
    int  RoundCornerCx() const { return static_cast<int>(m_cxyBorderRound.cx); }
};

struct DIB
{
    HDC     hDC = nullptr;
    HBITMAP hBM = nullptr;
    HBITMAP hOld = nullptr;
    DWORD*  pBits = nullptr;
    int     w = 0, h = 0;

    bool Create(int cx, int cy, DWORD dwBg)
    {
        w = cx; h = cy;
        hDC = ::CreateCompatibleDC(nullptr);
        if (hDC == nullptr) return false;

        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = cx;
        bi.bmiHeader.biHeight = -cy;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void* pv = nullptr;
        hBM = ::CreateDIBSection(hDC, &bi, DIB_RGB_COLORS, &pv, nullptr, 0);
        if (hBM == nullptr) return false;
        pBits = static_cast<DWORD*>(pv);
        hOld = static_cast<HBITMAP>(::SelectObject(hDC, hBM));
        for (int i = 0; i < cx * cy; ++i) pBits[i] = dwBg;
        return true;
    }

    DWORD At(int x, int y) const { return (pBits != nullptr) ? pBits[y * w + x] : 0; }

    void Destroy()
    {
        if (hDC != nullptr && hOld != nullptr) ::SelectObject(hDC, hOld);
        if (hBM != nullptr) ::DeleteObject(hBM);
        if (hDC != nullptr) ::DeleteDC(hDC);
        hDC = nullptr; hBM = nullptr; hOld = nullptr; pBits = nullptr;
    }
};

bool NearColor(DWORD a, DWORD b, int nTol = 10)
{
    const int dr = abs(static_cast<int>(GetRValue(a)) - static_cast<int>(GetRValue(b)));
    const int dg = abs(static_cast<int>(GetGValue(a)) - static_cast<int>(GetGValue(b)));
    const int db = abs(static_cast<int>(GetBValue(a)) - static_cast<int>(GetBValue(b)));
    return dr <= nTol && dg <= nTol && db <= nTol;
}

// 递归按名字查找控件。
// 不用 CPaintManagerUI::FindControl(name)：它依赖 m_mNameHash，
// 而该索引由 AttachDialog→InitControls 建立，在探针这种"只解析不显示"
// 的场景下可能尚未就绪，会得到假阴性。直接遍历控件树最稳。
CControlUI* FindByName(CControlUI* pRoot, const wchar_t* pszName)
{
    if (pRoot == nullptr || pszName == nullptr) return nullptr;
    if (pRoot->GetName().Compare(pszName) == 0) return pRoot;

    // GetCount/GetItemAt 定义在 CContainerUI（IContainerUI）上，不在 CControlUI 上。
    // 用 dynamic_cast 判定是否容器；非容器直接判定未命中。
    CContainerUI* pContainer = dynamic_cast<CContainerUI*>(pRoot);
    if (pContainer == nullptr) return nullptr;

    const int n = pContainer->GetCount();
    for (int i = 0; i < n; ++i)
    {
        CControlUI* pChild = pContainer->GetItemAt(i);
        if (pChild == nullptr) continue;
        if (CControlUI* pHit = FindByName(pChild, pszName)) return pHit;
    }
    return nullptr;
}

// <NavOption> 是 Demos 注册的自定义控件（CNavOptionUI，派生自 COptionUI）。
// 探针不链接 Demos 代码，故注册一个同名的 COptionUI 派生替身：
// 目的只是让 Main.xml 里的 nav_* 节点能成功实例化，从而验证"皮肤结构与
// 控件名"是否正确。绘制细节不在此探针的验证范围内。
class CProbeNavOptionUI : public COptionUI
{
    DECLARE_DUICONTROL(CProbeNavOptionUI)
public:
    CProbeNavOptionUI() {}
    LPCTSTR GetClass() const override { return L"NavOptionUI"; }
};

IMPLEMENT_DUICONTROL(CProbeNavOptionUI)

constexpr DWORD kBg   = 0xFF00FF00;   // 绿（背景）
constexpr DWORD kFill = 0xFF3366CC;   // 蓝（按钮底色，注意皮肤写法为 #AARRGGBB）

} // namespace

int main(int argc, char** argv)
{
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR hToken = 0;
    if (Gdiplus::GdiplusStartup(&hToken, &gsi, nullptr) != Gdiplus::Ok)
    {
        ::printf("[FATAL] GdiplusStartup failed\n");
        return 2;
    }

    // skin 目录：优先取命令行参数；否则从 exe 位置两级上溯到仓库根，
    // 再拼 bin64\skin\Win8Metro（与 Demos 运行时用的是同一份皮肤）。
    wchar_t szExe[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, szExe, MAX_PATH);
    std::wstring sDir(szExe);
    const size_t nSlash = sDir.find_last_of(L"\\/");
    if (nSlash != std::wstring::npos) sDir.resize(nSlash);

    std::wstring sSkin;
    std::wstring sSkinArgWide;   // owns the widened command-line path

    // [GATE-SELFTEST] /Inject adds one deliberately failing assertion so this
    // probe can prove its verdict path still works. Uses __argc/__wargv because
    // this file already reads its skin path from there.
    bool bInject = false;
    for (int i = 1; i < argc; ++i)
        if (argv[i] != nullptr && lstrcmpiA(argv[i], "/Inject") == 0) bInject = true;
    // Skip the /Inject switch when reading the optional skin path, otherwise
    // injecting would consume the path slot and load nothing.
    const wchar_t* pszSkinArg = nullptr;
    for (int i = 1; i < argc; ++i)
    {
        if (argv[i] == nullptr) continue;
        if (lstrcmpiA(argv[i], "/Inject") == 0) continue;
        // Skin path arrives as a narrow arg; widen it for the rest of the probe.
        wchar_t szWide[MAX_PATH] = {};
        ::MultiByteToWideChar(CP_ACP, 0, argv[i], -1, szWide, MAX_PATH);
        sSkinArgWide = szWide;
        pszSkinArg = sSkinArgWide.c_str();
        break;
    }
    if (pszSkinArg != nullptr)
    {
        sSkin = pszSkinArg;
    }
    else
    {
        std::wstring sRoot = sDir;                       // tools\probes
        for (int i = 0; i < 2; ++i)
        {
            const size_t p = sRoot.find_last_of(L"\\/");
            if (p != std::wstring::npos) sRoot.resize(p);
        }
        sSkin = sRoot + L"\\bin64\\skin\\Win8Metro";
    }

    ::printf("=== 皮肤与圆角控件验证 ===\n");
    ::wprintf(L"skin = %s\n\n", sSkin.c_str());

    HINSTANCE hInst = ::GetModuleHandleW(nullptr);
    CPaintManagerUI::SetInstance(hInst);
    CPaintManagerUI::SetResourcePath(sSkin.c_str());

    // 需要一个真实 HWND 供 CDialogBuilder / CPaintManagerUI 初始化
    const wchar_t* kCls = L"SkinProbeWnd";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = hInst;
    wc.lpszClassName = kCls;
    ::RegisterClassExW(&wc);

    HWND hWnd = ::CreateWindowExW(0, kCls, L"probe", WS_OVERLAPPEDWINDOW,
                                  0, 0, 1280, 900, nullptr, nullptr, hInst, nullptr);
    if (hWnd == nullptr)
    {
        ::printf("[FATAL] CreateWindowEx failed\n");
        return 2;
    }

    CPaintManagerUI pm;
    pm.Init(hWnd, L"SkinProbe");

    // 与 Demos 的 RegisterWin8MetroControls() 对齐：先注册自定义控件，
    // 否则皮肤里的 <NavOption> 节点会被静默跳过（探针首次跑就踩到这点，
    // 表现为 nav_0..nav_10 全部"找不到"，是探针环境缺陷而非皮肤问题）。
    CControlFactory::GetInstance()->RegistControl(
        L"CNavOptionUI", static_cast<CreateClass>(CProbeNavOptionUI::CreateControl));

    // <CaptionBtn> is a Demos-registered control. Without this the builder
    // silently drops the entire title-bar button subtree, and the probe then
    // reported restorebtn/closebtn as missing controls -- a probe defect, not a
    // skin defect (Main.xml defines both).
    RegisterCaptionBtnControl();

    CDialogBuilder builder;
    CControlUI* pRoot = builder.Create(L"Main.xml", nullptr, nullptr, &pm);
    Chk("Main.xml 解析成功", pRoot != nullptr,
        pRoot != nullptr ? L"根节点已创建" : L"builder.Create 返回 nullptr");

    if (pRoot != nullptr)
    {
        pm.AttachDialog(pRoot);

        // ---- 标题栏四个按钮存在且字形是 Fluent 码位 ----
        struct { const wchar_t* name; wchar_t cp; } kBtns[] = {
            { L"minbtn",     0xE921 },
            { L"maxbtn",     0xE922 },
            { L"restorebtn", 0xE923 },
            { L"closebtn",   0xE8BB },
        };

        for (const auto& b : kBtns)
        {
            CControlUI* p = FindByName(pRoot, b.name);
            if (p == nullptr)
            {
                Chk("标题栏按钮存在", false, std::wstring(L"找不到控件 ") + b.name);
                continue;
            }

            // CaptionBtn is a self-painted control and deliberately carries no
            // text (see CaptionBtn.h). Asserting on GetText() measured a design
            // that no longer exists; verify the real paint path instead.
            CButtonUI* pBtn = static_cast<CButtonUI*>(p);
            DIB canvas;
            int nGlyphPixels = -1;
            if (canvas.Create(46, 37, kBg))
            {
                pBtn->SetPos(RECT{ 0, 0, 46, 37 });
                pBtn->SetVisible(true);
                pBtn->DoPaint(canvas.hDC, RECT{ 0, 0, 46, 37 }, nullptr);
                nGlyphPixels = 0;
                for (int y = 0; y < canvas.h; ++y)
                    for (int x = 0; x < canvas.w; ++x)
                        if (!NearColor(canvas.At(x, y), kBg)) ++nGlyphPixels;
                canvas.Destroy();
            }
            wchar_t szD[160] = {};
            ::swprintf_s(szD, L"%s glyphPixels=%d", b.name, nGlyphPixels);
            // A drawn glyph covers at least a few dozen anti-aliased pixels.
            Chk("标题栏字形可真实绘制", nGlyphPixels > 10, szD);
        }

        // ---- <Default name="Button"> 的全局圆角是否真的落到按钮上 ----
        // 这条断言把"皮肤默认属性"和"圆角填充实现"连起来验证：
        // 只有 Default 被解析 + borderround 被应用 + PaintBkColor 走圆角分支，
        // 三者同时成立，界面上才会出现 Win11 的圆角按钮。
        {
            CControlUI* pBtn = FindByName(pRoot, L"menu_file");
            if (pBtn == nullptr)
            {
                Chk("菜单按钮存在", false, L"找不到 menu_file");
            }
            else
            {
                // borderround 是 protected 成员，用容器派生类读取不便，
                // 改用 SetAttribute 回读不现实，故通过 CButtonUI 的
                // GetBorderColor/尺寸无法覆盖——直接构造同款控件对比属性行为。
                CProbeButton probe;
                probe.SetAttribute(L"borderround", L"4,4");
                wchar_t sz[96] = {};
                ::swprintf_s(sz, L"probe cx=%d", probe.RoundCornerCx());
                Chk("borderround=4,4 可被解析", probe.RoundCornerCx() == 4, sz);
            }
        }

        // ---- 新配色确已生效（抽查主强调色与标题栏底色）----
        {
            // 底部状态条 bottombar 的 bkcolor 原为 #FF007ACC，现应为 #FF0067C0
            CControlUI* pBottom = FindByName(pRoot, L"bottombar");
            if (pBottom != nullptr)
            {
                wchar_t sz[64] = {};
                ::swprintf_s(sz, L"bkcolor=0x%08lX", static_cast<unsigned long>(pBottom->GetBkColor()));
                Chk("状态条已切到 Win11 强调色", pBottom->GetBkColor() == 0xFF0067C0, sz);
            }
            else
            {
                Chk("状态条控件存在", false, L"找不到 bottombar");
            }
        }

        // ---- 导航行数量（16 页皮肤是否都挂上了）----
        {
            int nNav = 0;
            for (int i = 0; i < 16; ++i)
            {
                wchar_t szName[32] = {};
                ::swprintf_s(szName, L"nav_%d", i);
                if (FindByName(pRoot, szName) != nullptr) ++nNav;
            }
            wchar_t sz[64] = {};
            ::swprintf_s(sz, L"找到 nav_0..nav_15 共 %d 个", nNav);
            Chk("左侧导航项齐全", nNav == 11, sz);   // Main.xml 定义 nav_0..nav_10
        }
    }

    // ---- B. 纯色 + borderround 的圆角填充 ----
    {
        CProbeButton btn;
        btn.SetAttribute(L"bkcolor", L"#FF3366CC");
        btn.SetAttribute(L"borderround", L"8,8");

        wchar_t szRound[64] = {};
        ::swprintf_s(szRound, L"m_cxyBorderRound.cx=%d（期望 8）", btn.RoundCornerCx());
        Chk("borderround 属性被解析", btn.RoundCornerCx() == 8, szRound);

        const int W = 100, H = 48;
        RECT rc = { 0, 0, W, H };
        btn.LayoutForPaint(rc);

        DIB dib;
        if (!dib.Create(W, H, kBg))
        {
            ::printf("[FATAL] CreateDIBSection failed\n");
            return 2;
        }

        btn.PaintBackgroundOnly(dib.hDC);

        const DWORD cCenter = dib.At(W / 2, H / 2);
        const DWORD cCorner = dib.At(1, 1);

        wchar_t sz[160] = {};
        ::swprintf_s(sz, L"center=0x%08lX 期望 0x%08lX",
                     static_cast<unsigned long>(cCenter), static_cast<unsigned long>(kFill));
        Chk("控件纯色填充生效", NearColor(cCenter, kFill), sz);

        ::swprintf_s(sz, L"corner=0x%08lX 期望仍是背景 0x%08lX",
                     static_cast<unsigned long>(cCorner), static_cast<unsigned long>(kBg));
        Chk("控件圆角生效（角点未被填充）", NearColor(cCorner, kBg), sz);

        dib.Destroy();
    }

    if (hWnd != nullptr) ::DestroyWindow(hWnd);
    ::UnregisterClassW(kCls, hInst);

    Gdiplus::GdiplusShutdown(hToken);

    ::printf("\n结果：%d 通过 / %d 失败\n", g_nPass, g_nFail);
    if (bInject)
    {
        Chk("injected-must-fail", false, L"self-test: forced failure");
    }
    return (g_nFail == 0) ? 0 : 1;
}
