#pragma once
// ============================================================
// CustomControls.h —— demo 自定义 DuiLib 控件（P2 抽出）
// 原定义位于 FrameWnd.cpp 内部；Page 层需要复用，故抽为共享头。
// 纯控件头：只依赖 UIlib.h，不含任何业务逻辑。
// ============================================================
#include "UIlib.h"
#include "WinHandles.h"
#include "ThemeTokens.h"   // [R209] 行底色跟随主题   // [C++17 R3] win::GdiObject for paint-time GDI objects
// horizontal row that emits a "click"/"dbclick" notify (plain containers do not).
// 选中态：满蓝 #007ACC 底（调用方同步把文字置白），未选中白底；无悬停特效。
class CClickRowUI : public DuiLib::CHorizontalLayoutUI
{
public:
    CClickRowUI() : m_bRowSel(false)
    {
        // [R209] 原构造写死纯白底，本控件被 10 个页面复用（Appx/清理/驱动/
        //   程序/优化/RegClean/欢迎…），深色主题下每张行都是白块。
        //   改为取 Surface 令牌：浅色下仍是 #FFFFFF（与原值一致），深色下跟随主题。
        SetBkColor(tokens::Token(tokens::EToken::Surface));
    }
    void SetRowSelected(bool bSel)
    {
        if (m_bRowSel == bSel) return;
        m_bRowSel = bSel;
        // [R209] 选中色与恢复色同样走令牌，否则深色下取消选中会变回白底
        SetBkColor(bSel ? tokens::Token(tokens::EToken::Accent)
                        : tokens::Token(tokens::EToken::Surface));
    }
    bool IsRowSelected() const { return m_bRowSel; }
    virtual void DoEvent(DuiLib::TEventUI& event)
    {
        // [CRASHFIX-UAF] 顺序修正：先让基类处理完本事件，通知最后发。
        // 旧实现 SendNotify 在前、基类 DoEvent 在后：页处理器若在
        // CLICK/DBCLICK 链里删除本控件（容器 RemoveAll），SendNotify
        // 返回后基类继续解引用已释放的 this → UAF（功能页崩溃实证）。
        // 重排后即使处理器把 this 删掉，SendNotify 返回即退出 DoEvent，
        // 不再触碰任何成员；管理器侧的悬垂回指由 ~CControlUI→ReapObjects
        // 统一清理。本控件被 10 个页面复用，这是通用防护。
        DuiLib::CHorizontalLayoutUI::DoEvent(event);
        if (m_pManager != nullptr)
        {
            if (event.Type == UIEVENT_BUTTONUP)
                m_pManager->SendNotify(this, DUI_MSGTYPE_CLICK);
            else if (event.Type == UIEVENT_DBLCLICK)
                m_pManager->SendNotify(this, DUI_MSGTYPE_DBCLICK);
        }
    }
protected:
    bool m_bRowSel;
};
// P7: self-drawn sliding switch (promoted from FrameWnd.cpp, verbatim
// body; only DuiLib:: qualifications added). Sole user is CPageOptim,
// kept here in case later pages need the same switch control.
class CSwitchUI : public DuiLib::CControlUI
{
public:
    // [R256] Win11 ToggleSwitch 度量。
    //   40x20 轨道 / 12 直径滑块 / 4 内边距，取自 WinUI 的 ToggleSwitch 默认值
    //   （TrackSize 40x20、ThumbSize 12）。旧实现是 46x20 + 16 直径，
    //   轨道偏长、滑块偏大，看起来更像 Android 的开关。
    enum { SW_W = 40, SW_H = 20, KNOB = 12, PAD = 4 };

    CSwitchUI() : m_bOn(false)
    {
        SetFixedWidth(SW_W);
        SetFixedHeight(SW_H);
    }

    bool GetOn() const { return m_bOn; }

    void SetOn(bool bOn, bool bMsg = true)
    {
        if (m_bOn == bOn) return;
        m_bOn = bOn;
        if (m_pManager != nullptr && bMsg)
            m_pManager->SendNotify(this, DUI_MSGTYPE_SELECTCHANGED, m_bOn ? 1 : 0, 0);
        Invalidate();
    }

    virtual void DoEvent(DuiLib::TEventUI& event)
    {
        // [FIX-UX] 松手生效 + 拖出取消（Win11 ToggleSwitch 语义）。
        //   旧实现按下即翻转并立即写注册表/弹确认框：按下后拖离松手，
        //   设置已经改掉、无法取消，与系统开关习惯相反。现 BUTTONDOWN
        //   只记按下意向，BUTTONUP 且松手点仍在开关内才生效
        //   （本框架无 BUTTONCANCEL 事件，拖出后松手由 PtInRect 判定取消）。
        if (event.Type == UIEVENT_BUTTONDOWN
            && ::PtInRect(&m_rcItem, event.ptMouse) && IsEnabled())
        {
            m_bDownPending = true;
            DuiLib::CControlUI::DoEvent(event);
            return;
        }
        if (event.Type == UIEVENT_BUTTONUP)
        {
            const bool bFire = m_bDownPending
                && ::PtInRect(&m_rcItem, event.ptMouse) && IsEnabled();
            m_bDownPending = false;
            DuiLib::CControlUI::DoEvent(event);
            if (bFire)
            {
                // [FIX-LAG] 视觉先行：先翻转状态并同步重绘，再派发
                //   SELECTCHANGED。通知触发的业务（注册表/命令写入、
                //   确认框、回读核实）同步跑在 UI 线程上可达数百毫秒，
                //   重绘排在业务前旋钮才能即时到位。用户取消或写入失败
                //   时 OnSwitchToggle 以 SetOn(..., false) 回滚并重绘，
                //   最终态不受影响。
                SetOn(!m_bOn, false);
                if (m_pManager != nullptr)
                {
                    HWND hPaint = m_pManager->GetPaintWindow();
                    if (hPaint != nullptr && ::IsWindow(hPaint))
                    {
                        ::InvalidateRect(hPaint, nullptr, FALSE);
                        ::UpdateWindow(hPaint);   // 同步绘制，旋钮立即到位
                    }
                    m_pManager->SendNotify(this, DUI_MSGTYPE_SELECTCHANGED,
                                           m_bOn ? 1 : 0, 0);
                }
            }
            return;
        }
        DuiLib::CControlUI::DoEvent(event);
    }


    virtual bool DoPaint(HDC hDC, const RECT& rcPaint, DuiLib::CControlUI* pStopControl)
    {
        const int x = m_rcItem.left;
        const int cy = (m_rcItem.top + m_rcItem.bottom) / 2;
        RECT rcTrack = { x, cy - SW_H / 2, x + SW_W, cy + SW_H / 2 };

        // [R257] Token() 返回的是 0xAARRGGBB（DuiLib 的 SetBkColor 口径），
        //   而 CreateSolidBrush / CreatePen 要的是 COLORREF = 0x00BBGGRR。
        //   直接把 0xFF1C1C1C 当 COLORREF 传进去，高字节 0xFF 会被当成红色分量，
        //   三个通道整个错位 —— 实测渲染出来是纯黑胶囊。
        auto toCref = [](DWORD cr) -> COLORREF
        {
            return RGB((cr >> 16) & 0xFF, (cr >> 8) & 0xFF, cr & 0xFF);
        };

        // ---- 轨道 ----
        // [R256] Win11 的两态是"结构不同"，不是"同一条轨道换个颜色"：
        //   关态 = 只描边、内部是底色（视觉上是空心的胶囊）
        //   开态 = 整体填充强调色
        // 旧实现两态都做实体填充，关态那条灰条在深色主题下很像一条分隔线，
        // 用户根本认不出是开关。
        const DWORD crAccent = tokens::Token(tokens::EToken::Accent);

        const DWORD crBorder = tokens::Token(tokens::EToken::Border);
        const DWORD crSunk   = tokens::Token(tokens::EToken::SurfaceSunk);
        if (m_bOn)
        {
            win::GdiObject hbrT(::CreateSolidBrush(toCref(crAccent)));
            win::GdiObject hpenT(::CreatePen(PS_SOLID, 1, toCref(crAccent)));
            HGDIOBJ o1 = ::SelectObject(hDC, hbrT.get());
            HGDIOBJ o2 = ::SelectObject(hDC, hpenT.get());
            ::RoundRect(hDC, rcTrack.left, rcTrack.top, rcTrack.right, rcTrack.bottom,
                        SW_H, SW_H);
            ::SelectObject(hDC, o1);
            ::SelectObject(hDC, o2);
        }
        else
        {
            // 空心：先用底色填出内部，再描边。顺序不能反 ——
            //   先描边再填充会把 1px 描边盖掉，边缘重新糊成一片（旧实现就是这样）。
            win::GdiObject hbrInside(::CreateSolidBrush(toCref(crSunk)));
            HGDIOBJ o1 = ::SelectObject(hDC, hbrInside.get());
            HGDIOBJ o2 = ::SelectObject(hDC, ::GetStockObject(NULL_PEN));
            ::RoundRect(hDC, rcTrack.left, rcTrack.top, rcTrack.right, rcTrack.bottom,
                        SW_H, SW_H);
            ::SelectObject(hDC, o1);
            ::SelectObject(hDC, o2);

            win::GdiObject hpenB(::CreatePen(PS_SOLID, 1, toCref(crBorder)));
            HGDIOBJ o3 = ::SelectObject(hDC, ::GetStockObject(NULL_BRUSH));
            HGDIOBJ o4 = ::SelectObject(hDC, hpenB.get());
            ::RoundRect(hDC, rcTrack.left, rcTrack.top, rcTrack.right, rcTrack.bottom,
                        SW_H, SW_H);
            ::SelectObject(hDC, o3);
            ::SelectObject(hDC, o4);
        }

        // ---- 滑块 ----
        // [R256] 前景色随轨道反选：
        //   开态滑块压在强调色上 -> OnAccent（深色主题下强调色是亮的，OnAccent 会是深色）
        //   关态滑块压在灰底上 -> Ink
        // 旧实现一律用 Ink，深色主题下"开"态的滑块与轨道对比不足。
        const DWORD crKnob = m_bOn ? tokens::Token(tokens::EToken::OnAccent)
                                   : tokens::Token(tokens::EToken::Ink);
        const int kx = m_bOn ? (rcTrack.right - PAD - KNOB) : (rcTrack.left + PAD);
        RECT rcKnob = { kx, cy - KNOB / 2, kx + KNOB, cy + KNOB / 2 };
        win::GdiObject hbrK(::CreateSolidBrush(toCref(crKnob)));
        win::GdiObject hpenK(::CreatePen(PS_SOLID, 1, toCref(crKnob)));
        HGDIOBJ o5 = ::SelectObject(hDC, hbrK.get());
        HGDIOBJ o6 = ::SelectObject(hDC, hpenK.get());
        ::Ellipse(hDC, rcKnob.left, rcKnob.top, rcKnob.right, rcKnob.bottom);
        ::SelectObject(hDC, o5);
        ::SelectObject(hDC, o6);
        return true;
    }

protected:
    bool m_bOn;
    // [FIX-UX] BUTTONDOWN 按下意向标志：松手在控件内才生效（见 DoEvent）。
    bool m_bDownPending = false;
};

// ============================================================
// 自绘复选框：三态方框，边框恒定可见，配色跟主题走
// --------------------------------------------------------
// 为什么不再用"☐ / ☑ / ▣"字形方案：
//   ① 选中态的字形 U+2611 在正文/符号字体里被画成**一块实心方块**，
//      空心方框的轮廓整个消失 —— 未选中与选中看着完全是两个东西，
//      这就是"选中之后边框消失"的直接来源；
//   ② 字形是文字，颜色只能跟 textcolor 走（Ink 近黑）：浅色主题下是突兀的
//      纯黑块，深色主题下几乎看不见，既不跟主题也不跟按钮/开关一致；
//   ③ 三态字形没有统一的框体，"半选"更是一个实心方块，看不出是复选框。
//
// 自绘后：三种状态都有明确的方框轮廓，内部用 Accent 填充表示选中、
// 勾用 OnAccent 画 —— 与 CSwitchUI、按钮同一套令牌，跨主题/跨页面一致。
// 文字由基类绘制（文字里不再塞字形前缀），textpadding.left 为方框让位。
// ============================================================
class CBoxCheckUI : public DuiLib::CCheckBoxUI
{
public:
    enum { BOX = 14 };          // 方框边长（逻辑像素，按 DPI 缩放）
    enum EBoxState
    {
        BoxEmpty   = 0,         // 未选中
        BoxPartial = 1,         // 半选（组内部分命中）
        BoxChecked = 2          // 全选
    };

    CBoxCheckUI() : m_nBoxState(BoxEmpty) {}

    // 供 DuiLib 控件工厂用（XML 标签 <BoxCheck> → "CBoxCheckUI"）。
    // 类内定义的静态成员函数隐式 inline，放头文件不会出现重复定义。
    static DuiLib::CControlUI* CreateControl() { return new CBoxCheckUI(); }

    int  GetBoxState() const { return m_nBoxState; }
    void SetBoxState(int nState)
    {
        if (m_nBoxState == nState) return;
        m_nBoxState = (nState >= BoxEmpty && nState <= BoxChecked) ? nState : BoxEmpty;
        Invalidate();
    }

    // 勾选态变化时同步方框；半选态由组三态维护，不被单次勾选覆盖。
    virtual void Selected(bool bSelected, bool bMsg = true)
    {
        DuiLib::CCheckBoxUI::Selected(bSelected, bMsg);
        if (m_nBoxState != BoxPartial)
            SetBoxState(bSelected ? BoxChecked : BoxEmpty);
    }

    virtual bool DoPaint(HDC hDC, const RECT& rcPaint, DuiLib::CControlUI* pStopControl)
    {
        // 基类负责背景与文字（文字已不含字形前缀），方框在其后叠加。
        const bool bRet = DuiLib::CCheckBoxUI::DoPaint(hDC, rcPaint, pStopControl);
        DrawBox(hDC);
        return bRet;
    }

private:
    void DrawBox(HDC hDC)
    {
        if (m_rcItem.right <= m_rcItem.left) return;
        const int nDpi = (m_pManager != nullptr && m_pManager->GetDPIObj() != nullptr)
            ? m_pManager->GetDPIObj()->Scale(BOX) : BOX;
        const int nPad = (m_pManager != nullptr && m_pManager->GetDPIObj() != nullptr)
            ? m_pManager->GetDPIObj()->Scale(6) : 6;

        RECT rc = m_rcItem;
        rc.left += nPad;
        rc.top = (m_rcItem.top + m_rcItem.bottom) / 2 - nDpi / 2;
        rc.right = rc.left + nDpi;
        rc.bottom = rc.top + nDpi;

        // [R257] CRenderEngine 收 AARRGGBB；裸 GDI 的 CreateSolidBrush 要 COLORREF。
        auto toCref = [](DWORD cr) -> COLORREF
        {
            return RGB((cr >> 16) & 0xFF, (cr >> 8) & 0xFF, cr & 0xFF);
        };

        const bool bOn = (m_nBoxState != BoxEmpty);
        const DWORD crFrame = bOn ? tokens::Token(tokens::EToken::Accent)
                                  : tokens::Token(tokens::EToken::Border);
        const DWORD crFill  = bOn ? tokens::Token(tokens::EToken::Accent)
                                  : tokens::Token(tokens::EToken::Surface);
        const DWORD crMark  = tokens::Token(tokens::EToken::OnAccent);

        // 底：填充色（选中=强调色，未选中=表面色）
        HBRUSH hbr = ::CreateSolidBrush(toCref(crFill));
        if (hbr != nullptr)
        {
            ::FillRect(hDC, &rc, hbr);
            ::DeleteObject(hbr);
        }
        // 框：三态都有，厚度 1px（选中时框与填充同色，轮廓仍然成立）
        CRenderEngine::DrawRect(hDC, rc, 1, crFrame, PS_SOLID);

        // 内标记：全选=勾，半选=横杠
        const int nIn = nDpi / 2;
        const int nPen = (nDpi / 8 > 0) ? nDpi / 8 : 1;
        const int nBar = (nIn / 2 > 0) ? nIn / 2 : 1;
        if (m_nBoxState == BoxChecked)
        {
            // 对勾 = 短撇（左上→右下）+ 长捺（左下→右上）。
            // DrawLine 画的是矩形对角线，方向由 left/top → right/bottom 决定，
            // 写反了会画成两条平行斜杠（看起来像「◣◢」）。
            RECT rcShort = { rc.left + nDpi * 26 / 100, rc.top + nDpi * 52 / 100,
                             rc.left + nDpi * 44 / 100, rc.top + nDpi * 72 / 100 };
            CRenderEngine::DrawLine(hDC, rcShort, nPen, crMark, PS_SOLID);
            RECT rcLong = { rc.left + nDpi * 44 / 100, rc.top + nDpi * 72 / 100,
                            rc.left + nDpi * 78 / 100, rc.top + nDpi * 28 / 100 };
            CRenderEngine::DrawLine(hDC, rcLong, nPen, crMark, PS_SOLID);
        }
        else if (m_nBoxState == BoxPartial)
        {
            RECT rcBar = { rc.left + nDpi / 4, (rc.top + rc.bottom) / 2 - nIn / 4,
                           rc.right - nDpi / 4, (rc.top + rc.bottom) / 2 + nIn / 4 };
            CRenderEngine::DrawRect(hDC, rcBar, nBar, crMark, PS_SOLID);
        }
    }

    int m_nBoxState;
};
// P7: hover-highlight row (promoted from FrameWnd.cpp, verbatim body;
// only DuiLib:: qualifications added). Sole user is CPageOptim::Build.
class CHoverRowUI : public DuiLib::CHorizontalLayoutUI
{
public:
    // [R213] 底色必须走令牌：原写死纯白 0xFFFFFFFF，深色主题下整行是一条白带。
    //   悬停色同理，由原来的浅蓝 #CCE8FB 换成 SurfaceHover。
    CHoverRowUI() { SetBkColor(tokens::Token(tokens::EToken::Surface)); }

    virtual void DoEvent(DuiLib::TEventUI& event)
    {
        if (event.Type == UIEVENT_MOUSEENTER)
            SetBkColor(tokens::Token(tokens::EToken::SurfaceHover));
        else if (event.Type == UIEVENT_MOUSELEAVE)
            SetBkColor(tokens::Token(tokens::EToken::Surface));
        DuiLib::CHorizontalLayoutUI::DoEvent(event);
    }
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
