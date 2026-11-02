// ============================================================================
//  ThemePickerWnd.cpp —— 标题栏主题下拉（自绘，带皮肤预览）
//
//  绘制用原生 GDI 而不是 CRenderEngine::DrawText：后者要 CPaintManagerUI*
//  才能解析字体索引，而这个浮层不挂 PaintManager（它不是 DuiLib 控件树的一
//  部分，挂进去反而要处理布局/焦点/页面栈）。用 GDI + 自己 CreateFont 更直接，
//  也让这个文件不依赖主窗口的字体表。
// ============================================================================

#include "stdafx.h"
#include "ThemePickerWnd.h"
#include "DwmTheme.h"   // [R236] dwmtheme::IsSystemDarkMode（Auto 项预览用）

namespace
{
    constexpr wchar_t kThemePickerClass[] = L"DismThemePickerWnd";

    // 内存 DC 绘制，避免闪烁：先画到兼容位图，再一次性 BitBlt。
    //   嵌在标题栏下的 200x150 面板，直接画会出现明显的块状重绘。
    struct TMemDC
    {
        HDC     hDC = nullptr;
        HBITMAP hBmp = nullptr;
        HGDIOBJ hOld = nullptr;
        HDC     hRef = nullptr;

        TMemDC(HDC hRefDC, int cx, int cy)
        {
            hRef = hRefDC;
            hDC = ::CreateCompatibleDC(hRefDC);
            if (hDC && cx > 0 && cy > 0)
            {
                hBmp = ::CreateCompatibleBitmap(hRefDC, cx, cy);
                if (hBmp) hOld = ::SelectObject(hDC, hBmp);
            }
        }
        ~TMemDC()
        {
            if (hDC)
            {
                if (hOld) ::SelectObject(hDC, hOld);
                if (hBmp) ::DeleteObject(hBmp);
                ::DeleteDC(hDC);
            }
        }
        TMemDC(const TMemDC&) = delete;
        TMemDC& operator=(const TMemDC&) = delete;

        void Blit(int cx, int cy) const
        {
            if (hDC && hRef) ::BitBlt(hRef, 0, 0, cx, cy, hDC, 0, 0, SRCCOPY);
        }
    };

    // 字体守卫：CreateFont 失败时 GetStockObject 兜底，绝不把空 HFONT 选进 DC
    //   （空 HFONT 会让 SelectObject 失败，之后的文字全部用系统默认字体，
    //   表现为"高分屏下字号突然变回 9pt"，很难联想到是字体创建失败）。
    struct TFontGuard
    {
        HFONT h = nullptr;
        bool  bOwned = false;   // [R325] 只有自建的字体才能删，stock 对象不能
        explicit TFontGuard(int nHeight, int nWeight)
        {
            h = ::CreateFontW(-nHeight, 0, 0, 0, nWeight, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                              L"Microsoft YaHei UI");
            bOwned = (h != nullptr);
            if (!h) h = static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
        }
        ~TFontGuard() { if (bOwned && h) ::DeleteObject(h); }
        TFontGuard(const TFontGuard&) = delete;
        TFontGuard& operator=(const TFontGuard&) = delete;
    };

    // 0xAARRGGBB -> COLORREF（GDI 要 0x00BBGGRR）
    COLORREF ToCOLORREF(DWORD cr)
    {
        return RGB((cr >> 16) & 0xFF, (cr >> 8) & 0xFF, cr & 0xFF);
    }

    // [R325] 当前打开的浮层（同一时刻最多一个）。由 Create 登记、OnFinalMessage 注销。
    CThemePickerWnd* g_pOpenPicker = nullptr;
}

CThemePickerWnd::CThemePickerWnd() = default;
CThemePickerWnd::~CThemePickerWnd() = default;

bool CThemePickerWnd::IsOpen()
{
    return g_pOpenPicker != nullptr;
}

void CThemePickerWnd::CloseOpen()
{
    if (g_pOpenPicker && g_pOpenPicker->m_hWnd)
        g_pOpenPicker->Close(0);
}

LPCTSTR CThemePickerWnd::GetWindowClassName() const
{
    return kThemePickerClass;
}

void CThemePickerWnd::BuildItems()
{
    m_vItems.clear();

    // [2026] 从 ThemeService 动态读取所有预设（4 built-ins + 扫描 skin\ 目录）
    const auto& presets = ThemeService::GetPresets();
    const bool bSysDark = dwmtheme::IsSystemDarkMode();

    for (const auto& p : presets)
    {
        TItem it;
        it.name = p.name;
        it.displayName = p.displayName;
        it.bHasPalette = p.hasPalette;
        if (p.hasPalette) it.palette = p.palette;
        it.crAccentOverride = ThemeService::EffectiveAccent(p);

        // 副标题按 preset 属性自动生成（无需硬编码）
        switch (p.darkPolicy)
        {
            case ThemeService::EDarkPolicy::Never:
                it.subtitle = L"固定浅色，独立皮肤";
                it.bDarkPreview = false;
                break;
            case ThemeService::EDarkPolicy::Always:
                it.subtitle = L"始终使用深色外观";
                it.bDarkPreview = true;
                break;
            case ThemeService::EDarkPolicy::AlwaysLight:
                it.subtitle = L"始终使用浅色外观";
                it.bDarkPreview = false;
                break;
            case ThemeService::EDarkPolicy::FollowSystem:
            default:
                it.subtitle = L"随 Windows 主题自动切换";
                it.bDarkPreview = bSysDark;   // 预览块按当前系统状态渲染
                break;
        }
        m_vItems.push_back(std::move(it));
    }

    // 高亮当前生效 preset
    const std::wstring curName = ThemeService::CurrentPreset().name;
    m_nCur = -1;
    for (int i = 0; i < static_cast<int>(m_vItems.size()); ++i)
    {
        if (_wcsicmp(m_vItems[i].name.c_str(), curName.c_str()) == 0)
        {
            m_nCur = i;
            break;
        }
    }
}

int CThemePickerWnd::ItemHeight() const
{
    return Dp(48);
}

RECT CThemePickerWnd::ItemRect(int i) const
{
    RECT rc = { Dp(6), m_nPadY + i * ItemHeight(), 0, 0 };
    rc.right = m_nWidth - Dp(6);
    rc.bottom = rc.top + ItemHeight();
    return rc;
}

int CThemePickerWnd::ItemAtY(int y) const
{
    if (y < m_nPadY) return -1;
    const int i = (y - m_nPadY) / ItemHeight();
    return (i >= 0 && i < static_cast<int>(m_vItems.size())) ? i : -1;
}

void CThemePickerWnd::Layout()
{
    // 宽度由"标题 + 副标题 + 预览块 + 勾选位"共同决定，取固定值以保证
    //   三个选项对齐（按文字实测宽度会得到三个不同宽度，看起来像没对齐）。
    m_nWidth = Dp(300);   // [FIX-UX] 268 实测会把副标题尾部裁掉（"随 Windows 主题自动切换"显示不全）
    const int cy = m_nPadY * 2 + ItemHeight() * static_cast<int>(m_vItems.size());
    ::SetWindowPos(m_hWnd, HWND_TOPMOST, 0, 0, m_nWidth, cy,
                   SWP_NOMOVE | SWP_NOACTIVATE);
}

bool CThemePickerWnd::Create(HWND anchorHwnd, const RECT& rcAnchor, int nDpi, FnOnPick fnOnPick)
{
    m_hAnchor = anchorHwnd;
    m_nDpi = (nDpi > 0) ? nDpi : 96;
    m_fnOnPick = std::move(fnOnPick);
    BuildItems();

    if (!RegisterWindowClass()) return false;

    // 先算尺寸再 Create：一次创建就是最终大小，避免"先小后大"的闪烁。
    m_nWidth = Dp(300);   // [FIX-UX] 268 实测会把副标题尾部裁掉（"随 Windows 主题自动切换"显示不全）
    const int cy = m_nPadY * 2 + ItemHeight() * static_cast<int>(m_vItems.size());

    // 定位：贴住按钮左边缘，向下展开；超出屏幕右/下边界时向内收。
    int x = rcAnchor.left;
    int y = rcAnchor.bottom + Dp(2);
    if (MONITORINFO mi = {}; ::GetMonitorInfoW(::MonitorFromRect(&rcAnchor, MONITOR_DEFAULTTONEAREST), &mi))
    {
        if (x + m_nWidth > mi.rcWork.right)  x = mi.rcWork.right - m_nWidth;
        if (x < mi.rcWork.left)              x = mi.rcWork.left;
        if (y + cy > mi.rcWork.bottom)       y = rcAnchor.top - cy - Dp(2);   // 往上翻
        if (y < mi.rcWork.top)               y = mi.rcWork.top;
    }

    // WS_EX_TOOLWINDOW：不占任务栏、不出现在 Alt+Tab 里。
    //   WS_EX_TOPMOST：标题栏菜单必须压在窗口内容之上。
    //   WS_POPUP + WS_BORDER：无标题栏，但保留 1px 边框以和背景分离。
    if (!CWindowWnd::Create(anchorHwnd, L"", WS_POPUP | WS_BORDER,
                            WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                            x, y, m_nWidth, cy, nullptr))
        return false;

    ::ShowWindow(m_hWnd, SW_SHOWNOACTIVATE);
    ::SetFocus(m_hWnd);   // 拿焦点才能收到失焦关闭
    ::InvalidateRect(m_hWnd, nullptr, FALSE);
    g_pOpenPicker = this;   // [R325] 登记为当前唯一打开的浮层
    return true;
}

void CThemePickerWnd::OnFinalMessage(HWND hWnd)
{
    // [2026] 回调传 preset name
    const bool bPicked = m_bPicked;
    const std::wstring sPicked = m_sPicked;
    FnOnPick fn = m_fnOnPick;
    m_bPicked = false;

    if (g_pOpenPicker == this) g_pOpenPicker = nullptr;

    if (bPicked && fn) fn(sPicked);

    __super::OnFinalMessage(hWnd);
    delete this;
}

void CThemePickerWnd::OnMouseMove(POINT pt)
{
    const int nHot = ItemAtY(pt.y);
    if (nHot != m_nHot)
    {
        m_nHot = nHot;
        ::InvalidateRect(m_hWnd, nullptr, FALSE);
    }
    // 手型光标：让"可点"这件事在视觉上先成立
    TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, m_hWnd, 0 };
    ::TrackMouseEvent(&tme);
}

void CThemePickerWnd::OnLButtonUp(POINT pt)
{
    const int i = ItemAtY(pt.y);
    if (i < 0 || i >= static_cast<int>(m_vItems.size()))
    {
        Close(0);
        return;
    }

    // [2026] 只登记选择（preset name），回调推迟到 OnFinalMessage
    m_sPicked = m_vItems[i].name;
    m_bPicked = true;
    Close(0);
}

void CThemePickerWnd::OnPaint(HDC hRefDC)
{
    RECT rcClient = {};
    ::GetClientRect(m_hWnd, &rcClient);
    const int cx = rcClient.right;
    const int cy = rcClient.bottom;

    TMemDC mem(hRefDC, cx, cy);
    if (!mem.hDC) return;
    HDC dc = mem.hDC;

    // 面板底色/边框取**当前生效**主题：下拉伸出来的那一刻，
    //   用户看的还是当前界面，面板理应和它同色。
    const DWORD crPanel = tokens::Token(tokens::EToken::SurfaceAlt);
    const DWORD crBorder = tokens::Token(tokens::EToken::Border);
    const DWORD crInk = tokens::Token(tokens::EToken::Ink);
    const DWORD crSub = tokens::Token(tokens::EToken::InkTertiary);
    const DWORD crHover = tokens::Token(tokens::EToken::SurfaceHover);
    const DWORD crAccent = tokens::Token(tokens::EToken::Accent);

    // [R325] 画刷必须自己释放。原来把 CreateSolidBrush 的返回值直接内联进
    //   FillRect，句柄当场丢失 —— 每次 WM_PAINT 泄漏 1 个 HBRUSH，
    //   而悬停换项（OnMouseMove）每次都 InvalidateRect 触发重绘。
    //   反复展开下拉会让进程 GDI 句柄单调增长，撞到 10000 上限后
    //   CreateSolidBrush 返回 NULL，面板底色/边框直接不画，
    //   并拖累同进程其它 GDI 绘制。
    HBRUSH hbPanel = ::CreateSolidBrush(ToCOLORREF(crPanel));
    ::FillRect(dc, &rcClient, hbPanel);
    ::DeleteObject(hbPanel);

    TFontGuard fTitle(Dp(13), FW_NORMAL);
    TFontGuard fSub(Dp(11), FW_NORMAL);

    ::SetBkMode(dc, TRANSPARENT);

    for (int i = 0; i < static_cast<int>(m_vItems.size()); ++i)
    {
        const TItem& it = m_vItems[i];
        RECT rc = ItemRect(i);

        // 悬停底
        if (i == m_nHot)
        {
            HBRUSH hb = ::CreateSolidBrush(ToCOLORREF(crHover));
            ::FillRect(dc, &rc, hb);
            ::DeleteObject(hb);
        }

        // ---- 预览块：一块"缩微界面" ----
        //   bHasPalette（如 Classic）：直接用 palette 固定颜色
        //   其余项：按目标深浅模式取令牌 —— 所以"还没选中"的主题也能画出正确预览
        RECT rcPrev = { rc.left + Dp(10), rc.top + Dp(9),
                        rc.left + Dp(10) + Dp(52), rc.top + Dp(9) + Dp(30) };
        DWORD crPvBg, crPvAlt, crPvAc, crPvBd;
        if (it.bHasPalette)
        {
            // 预设自带 palette（如 Classic 深蓝）— 直接用 palette 固定值
            crPvBg  = it.palette.surface;       // 页面底（Classic: 白）
            crPvAlt = it.palette.headerBand;    // 标题栏色带（Classic: 深蓝 #1E5799）
            crPvAc  = it.palette.accent;        // 强调色（Classic: #007ACC）
            crPvBd  = it.palette.border;        // 边框色
        }
        else
        {
            const bool bDk = it.bDarkPreview;
            crPvBg  = tokens::TokenInMode(tokens::EToken::Surface,   bDk);
            crPvAlt = tokens::TokenInMode(tokens::EToken::SurfaceAlt, bDk);
            crPvAc  = it.crAccentOverride;   // 用 EffectiveAccent（已解出 Override vs System）
            crPvBd  = tokens::TokenInMode(tokens::EToken::Border,   bDk);
        }

        // 底色（圆角填充，Win11 观感）
        CRenderEngine::DrawRoundRectFilled(dc, rcPrev, Dp(4), 0xFF000000u | crPvBg);
        // 顶部一条"标题栏"色带
        RECT rcBar = { rcPrev.left + 1, rcPrev.top + 1, rcPrev.right - 1, rcPrev.top + Dp(9) };
        CRenderEngine::DrawRoundRectFilled(dc, rcBar, Dp(3), 0xFF000000u | crPvAlt);
        // 强调色小条（模拟"选中/主按钮"）
        RECT rcAcc = { rcPrev.left + Dp(5), rcPrev.top + Dp(13),
                       rcPrev.right - Dp(5), rcPrev.top + Dp(18) };
        CRenderEngine::DrawRoundRectFilled(dc, rcAcc, Dp(2), 0xFF000000u | crPvAc);
        // 描边
        CRenderEngine::DrawRoundRect(dc, rcPrev, 0, 0, 1, 0xFF000000u | crPvBd);

        // ---- 文字 ----
        const int nTextLeft = rcPrev.right + Dp(10);
        RECT rcT = { nTextLeft, rc.top + Dp(8), rc.right - Dp(26), rc.top + Dp(8) + Dp(20) };
        HGDIOBJ hOld = ::SelectObject(dc, fTitle.h);
        ::SetTextColor(dc, ToCOLORREF(crInk));
        ::DrawTextW(dc, it.displayName.c_str(), -1, &rcT, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

        RECT rcS = { nTextLeft, rcT.bottom - Dp(2), rc.right - Dp(26), rcT.bottom - Dp(2) + Dp(18) };
        ::SelectObject(dc, fSub.h);
        ::SetTextColor(dc, ToCOLORREF(crSub));
        ::DrawTextW(dc, it.subtitle.c_str(), -1, &rcS, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        ::SelectObject(dc, hOld);

        // ---- 当前选中：右侧勾 ----
        //   用文字画的勾而不是位图：这套代码库没有勾选图标资源，
        //   为一个下拉新增 PNG 会引入额外的资源部署依赖。
        if (i == m_nCur)
        {
            RECT rcCk = { rc.right - Dp(26), rc.top, rc.right - Dp(8), rc.bottom };
            ::SelectObject(dc, fTitle.h);
            ::SetTextColor(dc, ToCOLORREF(crAccent));
            ::DrawTextW(dc, L"\u2713", -1, &rcCk,
                        DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        }
    }

    // 外框最后画，压在内容之上
    HBRUSH hFrame = ::CreateSolidBrush(ToCOLORREF(crBorder));
    ::FrameRect(dc, &rcClient, hFrame);
    ::DeleteObject(hFrame);

    mem.Blit(cx, cy);
}

LRESULT CThemePickerWnd::HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_CREATE:
        return 0;

    case WM_PAINT:
        {
            PAINTSTRUCT ps = {};
            HDC hDC = ::BeginPaint(m_hWnd, &ps);
            OnPaint(hDC);
            ::EndPaint(m_hWnd, &ps);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;   // 全部在 WM_PAINT 的内存 DC 里画完，禁止系统刷底色（防闪）

    case WM_MOUSEMOVE:
        {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            OnMouseMove(pt);
        }
        return 0;

    case WM_LBUTTONUP:
        {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            OnLButtonUp(pt);
        }
        return 0;

    case WM_MOUSELEAVE:
        if (m_nHot != -1)
        {
            m_nHot = -1;
            ::InvalidateRect(m_hWnd, nullptr, FALSE);
        }
        return 0;

    case WM_KEYDOWN:
        // Esc 关闭、上下键移动、回车确认 —— 键盘用户不能只能用鼠标
        if (wParam == VK_ESCAPE) { Close(0); return 0; }
        if (wParam == VK_UP || wParam == VK_DOWN)
        {
            if (m_vItems.empty()) return 0;
            int n = (m_nHot < 0) ? 0 : m_nHot + ((wParam == VK_DOWN) ? 1 : -1);
            if (n < 0) n = static_cast<int>(m_vItems.size()) - 1;
            if (n >= static_cast<int>(m_vItems.size())) n = 0;
            m_nHot = n;
            ::InvalidateRect(m_hWnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_RETURN && m_nHot >= 0)
        {
            m_sPicked = m_vItems[m_nHot].name;
            m_bPicked = true;
            Close(0);
            return 0;
        }
        return 0;

    case WM_KILLFOCUS:
        // 点到别处就收起：这是所有下拉/菜单的既定行为。
        Close(0);
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) { Close(0); return 0; }
        return 0;

    default:
        break;
    }
    return CWindowWnd::HandleMessage(uMsg, wParam, lParam);
}
