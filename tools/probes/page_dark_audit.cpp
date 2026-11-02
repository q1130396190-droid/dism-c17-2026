// ============================================================
// page_dark_audit.cpp —— 逐页可读性巡检（单主题模式）
//
// 为什么需要：ThemeService 只报告"改了多少个控件"，这个总量无法回答
// 用户真正关心的问题 —— 每个页面在深色/浅色下是否还读得清。
//
// 用法：page_dark_audit.exe dark | light
//   每种主题各起一个进程，避免同一棵控件树往返切换带来的状态污染。
//   生产里用户看到的永远是"启动即为某主题"，这才是有意义的场景。
//
// 判据：对每个可见文字控件，取 textColor，向上找最近的**不透明**底色
//       （文字画在它所在的那一层面片上，含自身底色），算 WCAG 对比度。
//       正常文本需 >= 4.5:1，不达标即列出并可据此定位。
// ============================================================

#include "UIlib.h"
#include "ThemeService.h"

#include <windows.h>
#include <gdiplus.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

using namespace DuiLib;

namespace
{
int g_nPass = 0;
int g_nFail = 0;

double Lin(double c)
{
    c /= 255.0;
    return (c <= 0.04045) ? c / 12.92 : ::pow((c + 0.055) / 1.055, 2.4);
}

// DuiLib 色值是标准 #AARRGGBB：R=(c>>16), G=(c>>8), B=(c&0xFF)。
//
// 这里踩过一次严重误报：初版按 R=c&0xFF 取值（把蓝当成红），
// 于是 #FF0067C0（蓝底白字，真实 5.67:1）被算成 4.05:1，
// 凭空产出 32 个"不可读"假失败。
// 校验：DuiLib 的 DrawColor 内部用
//   Gdiplus::Color(a, GetBValue(c), GetGValue(c), GetRValue(c))
// 而 GetBValue 取高字节(0x00)、GetRValue 取低字节(0xC0)，
// 还原出的 RGB 正是 (0,103,192) 蓝色 —— 与标准 #AARRGGBB 一致。
double Luminance(DWORD c)
{
    const double r = Lin(static_cast<double>((c >> 16) & 0xFF));
    const double g = Lin(static_cast<double>((c >> 8) & 0xFF));
    const double b = Lin(static_cast<double>(c & 0xFF));
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

double Contrast(DWORD fg, DWORD bg)
{
    const double a = Luminance(fg);
    const double b = Luminance(bg);
    return (a > b) ? (a + 0.05) / (b + 0.05) : (b + 0.05) / (a + 0.05);
}

bool IsOpaque(DWORD c) { return ((c >> 24) & 0xFFu) != 0; }

std::wstring Hex(DWORD c)
{
    wchar_t b[16] = {};
    ::swprintf_s(b, L"#%08lX", static_cast<unsigned long>(c));
    return b;
}

DWORD ResolveBackground(CControlUI* pCtrl, DWORD dwFallback)
{
    for (CControlUI* q = pCtrl; q != nullptr; q = q->GetParent())
    {
        const DWORD c = q->GetBkColor();
        if (IsOpaque(c)) return c;
    }
    return dwFallback;
}

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

class CProbeNavOptionUI : public COptionUI
{
    DECLARE_DUICONTROL(CProbeNavOptionUI)
public:
    CProbeNavOptionUI() {}
    LPCTSTR GetClass() const override { return L"NavOptionUI"; }
};

IMPLEMENT_DUICONTROL(CProbeNavOptionUI)

struct Finding
{
    std::wstring name;
    std::wstring cls;
    DWORD fg;
    DWORD bg;
    double ratio;
};

void Audit(CControlUI* pRoot, DWORD dwFallbackBg, theme::EMode eMode,
           std::vector<Finding>& out)
{
    std::vector<CControlUI*> st;
    st.push_back(pRoot);

    while (!st.empty())
    {
        CControlUI* p = st.back();
        st.pop_back();
        if (p == nullptr) continue;

        DWORD fg = 0;
        bool hasText = false;

        if (auto* pl = dynamic_cast<CLabelUI*>(p))
        {
            fg = pl->GetTextColor();
            hasText = pl->GetText().GetLength() > 0;
        }
        else if (auto* pr = dynamic_cast<CRichEditUI*>(p))
        {
            fg = pr->GetTextColor();
            hasText = true;   // 富文本内容运行时填充，配色仍需可读
        }

        // [R29] 选中态必须按【真实渲染属性】衡量。
        //
        // COptionUI::PaintText 在 IsSelected() 时优先用 m_dwSelectedTextColor，
        // PaintBkColor 在 IsSelected() 时只用 m_dwSelectedBkColor 且不再回落到
        // bkcolor。审计若只读 textcolor / bkcolor，就会对选中项算出一个
        // 屏幕上根本不存在的组合 —— 实测正是如此：探针报 3.93 达标，
        // 而真实渲染的 #0067C0 压在 #4E5F12 上只有 1.25:1。
        DWORD selBkOverride = 0;
        if (auto* pOpt = dynamic_cast<COptionUI*>(p))
        {
            if (pOpt->IsSelected())
            {
                const DWORD selText = pOpt->GetSelectedTextColorRaw();
                if (selText != 0) fg = selText;
                selBkOverride = pOpt->GetSelectedBkColorRaw();
                // 侧栏导航是 GDI+ 自绘药丸（CNavOptionUI::PaintPillBg），
                // 不走 selectedbkcolor。本探针用普通 COptionUI 顶替注册，
                // 若不在这里补上药丸底色，就会按父容器白底去算，
                // 报出 fg=#FFFFFFFF bg=#FFFFFFFF ratio=1.00 这种
                // 屏幕上并不存在的组合。取色统一走 ThemeService 单源。
                if (selBkOverride == 0 &&
                    p->GetClass() != nullptr &&
                    ::lstrcmpiW(p->GetClass(), L"NavOptionUI") == 0)
                {
                    selBkOverride = theme::NavPillSelectedBk(eMode);
                }
                hasText = hasText || (pOpt->GetText().GetLength() > 0);
            }
        }

        if (hasText && IsOpaque(fg))
        {
            // 选中态若自带不透明底色，文字实际就压在这上面，必须优先采用；
            // 否则会拿父容器的底色去算，得到一个屏幕上不存在的组合。
            const DWORD bg = IsOpaque(selBkOverride)
                           ? selBkOverride
                           : ResolveBackground(p, dwFallbackBg);
            const double r = Contrast(fg, bg);
            if (r < 4.5)
            {
                Finding f;
                f.name = p->GetName().GetData();
                f.cls = p->GetClass();
                f.fg = fg;
                f.bg = bg;
                f.ratio = r;
                out.push_back(f);
            }
        }

        if (auto* pc = dynamic_cast<CContainerUI*>(p))
            for (int i = 0; i < pc->GetCount(); ++i)
                st.push_back(pc->GetItemAt(i));
    }
}

} // namespace

int main(int argc, char** argv)
{
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR tok = 0;
    if (Gdiplus::GdiplusStartup(&tok, &gsi, nullptr) != Gdiplus::Ok)
    {
        ::printf("[FATAL] GdiplusStartup\n");
        return 2;
    }

    const bool bDark = (argc >= 2 && argv[1] != nullptr && argv[1][0] == 'd');
    const char* pszTheme = bDark ? "Dark" : "Light";

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

    ::printf("=== 逐页可读性巡检（单主题）===\n\n");

    HINSTANCE hInst = ::GetModuleHandleW(nullptr);
    CPaintManagerUI::SetInstance(hInst);
    CPaintManagerUI::SetResourcePath(sSkin.c_str());

    const wchar_t* kCls = L"PageDarkAuditWnd";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = hInst;
    wc.lpszClassName = kCls;
    ::RegisterClassExW(&wc);

    HWND hWnd = ::CreateWindowExW(0, kCls, L"audit", WS_OVERLAPPEDWINDOW,
                                  0, 0, 1280, 900, nullptr, nullptr, hInst, nullptr);
    if (hWnd == nullptr) { ::printf("[FATAL] CreateWindowEx\n"); return 2; }

    CPaintManagerUI pm;
    pm.Init(hWnd, L"PageDarkAudit");

    CControlFactory::GetInstance()->RegistControl(
        L"CNavOptionUI", static_cast<CreateClass>(CProbeNavOptionUI::CreateControl));

    CDialogBuilder builder;
    CControlUI* pRoot = builder.Create(L"Main.xml", nullptr, nullptr, &pm);
    if (pRoot == nullptr) { ::printf("[FATAL] Main.xml parse failed\n"); return 2; }
    pm.AttachDialog(pRoot);
    pm.SetDPI(96);

    theme::Apply(pRoot, bDark ? theme::EMode::Dark : theme::EMode::Light);

    std::vector<Finding> bad;
    Audit(pRoot, bDark ? theme::DarkWindowBk() : 0xFFFFFFFF,
          bDark ? theme::EMode::Dark : theme::EMode::Light, bad);

    std::sort(bad.begin(), bad.end(),
              [](const Finding& a, const Finding& b) { return a.ratio < b.ratio; });

    ::printf("[%s] contrast < 4.5:1 text controls: %d\n",
             pszTheme, static_cast<int>(bad.size()));

    for (size_t i = 0; i < bad.size() && i < 40; ++i)
    {
        const Finding& f = bad[i];
        ::wprintf(L"   - %s (%s) fg=%s bg=%s ratio=%.2f\n",
                  f.name.c_str(), f.cls.c_str(),
                  Hex(f.fg).c_str(), Hex(f.bg).c_str(), f.ratio);
    }

    if (bad.empty()) { ++g_nPass; ::printf("[PASS] %s readable\n", pszTheme); }
    else             { ++g_nFail; ::printf("[FAIL] %s has unreadable text\n", pszTheme); }

    ::DestroyWindow(hWnd);
    ::UnregisterClassW(kCls, hInst);
    Gdiplus::GdiplusShutdown(tok);

    ::printf("\nresult: %d pass / %d fail\n", g_nPass, g_nFail);
    return (g_nFail == 0) ? 0 : 1;
}
