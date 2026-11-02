#pragma once
// ============================================================
// CustomControls.h —— demo 自定义 DuiLib 控件（P2 抽出）
// 原定义位于 FrameWnd.cpp 内部；Page 层需要复用，故抽为共享头。
// 纯控件头：只依赖 UIlib.h，不含任何业务逻辑。
// ============================================================
// [PERF-SW] 标准头显式引入：GDI 缓存依赖 unordered_map/vector/memory，
//   不依赖 UIlib.h 或其它头文件传递性包含（等级头顺序一变就断编译）。
#include <unordered_map>
#include <vector>
#include <memory>

#include "UIlib.h"
#include "WinHandles.h"
#include "ThemeTokens.h"   // [R209] 行底色跟随主题   // [C++17 R3] win::GdiObject for paint-time GDI objects

// ============================================================
// [PERF-SW] 绘制期 GDI 对象缓存（文件内 static，进程单例）
// ------------------------------------------------------------
// 为什么：CPaintManagerUI 对父容器相交的每个可见子控件都调 DoPaint，
//   一次 hover 重绘就会让整页几十个开关/复选框各自 Create/Delete
//   2~6 个画笔画刷 —— 一次 GDI 对象创建是内核句柄表操作，成本远高于
//   一次 40x20 的 RoundRect。令牌色在主题切换前是有限的几个值，
//   按 COLORREF 缓存后稳态下每帧 0 次 Create/Delete。
// 约束：
//   ① 仅 UI 线程使用（thread_local 实例，跨线程看不到彼此的句柄）；
//   ② 缓存对象在整个进程生命周期内一直"存活"并被不同 DC 反复 Select，
//      因此绝不能用本控件/本窗口"最后引用即删"的 win::GdiObject 管理；
//   ③ 容量上限 128：理论上令牌色不会超，上限只防御失控输入（如逐帧
//      渐变色）；触顶后停止缓存、返回临时对象（由调用方按 GdiObject
//      语义释放），宁可慢一点也不无限增长句柄表。
// ============================================================
namespace customcontrols_gdi_cache
{
    class CGdiCache
    {
    public:
        enum { MAX_ENTRIES = 128 };

        ~CGdiCache()
        {
            // 进程/线程退出时统一删除：缓存对象长期"存活"，这里必须补删。
            for (auto& kv : m_pens)   if (kv.second != nullptr) ::DeleteObject(kv.second);
            for (auto& kv : m_brushes) if (kv.second != nullptr) ::DeleteObject(kv.second);
            for (HGDIOBJ h : m_volatile) if (h != nullptr) ::DeleteObject(h);
        }

        // 返回 SOLID 1px 画笔；返回 nullptr 表示创建失败（调用方应跳过描边）。
        HPEN GetPen(COLORREF cr)
        {
            auto it = m_pens.find(cr);
            if (it != m_pens.end()) return it->second;
            HPEN h = ::CreatePen(PS_SOLID, 1, cr);
            if (h == nullptr) return nullptr;
            if (m_pens.size() < MAX_ENTRIES) { m_pens.emplace(cr, h); return h; }
            // 触顶：不缓存，交给本帧结束时的批量清理（临时对象必须活到绘制完成）。
            m_volatile.push_back(h);
            return h;
        }

        // 返回实心画刷；语义同 GetPen。
        HBRUSH GetBrush(COLORREF cr)
        {
            auto it = m_brushes.find(cr);
            if (it != m_brushes.end()) return it->second;
            HBRUSH h = ::CreateSolidBrush(cr);
            if (h == nullptr) return nullptr;
            if (m_brushes.size() < MAX_ENTRIES) { m_brushes.emplace(cr, h); return h; }
            m_volatile.push_back(h);
            return h;
        }

        // 控件绘制结束（DoPaint 返回前）调用：释放触顶后产生的临时对象。
        //   DoPaint 已把 DC 里的旧对象 SelectObject 回去，此时删除句柄安全。
        void FlushVolatile()
        {
            for (HGDIOBJ h : m_volatile) if (h != nullptr) ::DeleteObject(h);
            m_volatile.clear();
        }

    private:
        std::unordered_map<COLORREF, HPEN>   m_pens;
        std::unordered_map<COLORREF, HBRUSH> m_brushes;
        std::vector<HGDIOBJ> m_volatile;
    };

    inline CGdiCache& Instance()
    {
        // [PERF-SW] thread_local：UI 线程独占（DuiLib 绘制线程约束），
        //   同时免去静态初始化顺序与销毁顺序问题。
        thread_local CGdiCache s_cache;
        return s_cache;
    }

    // [PERF-SW] 句柄所有权归缓存（或由 FlushVolatile 批量回收），
    //   SelectObject 之后必须换回原本的对象，不能 DeleteObject。
    inline HPEN GetCachedPen(COLORREF cr)    { return Instance().GetPen(cr); }
    inline HBRUSH GetCachedBrush(COLORREF cr){ return Instance().GetBrush(cr); }
    inline void  FlushCachedVolatile()       { Instance().FlushVolatile(); }
}
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
        // [PERF-SW] 内部同步/回滚路径立即出图：Invalidate() 只是把区域挂进
        //   WM_PAINT，而下拉框弹层未关闭/焦点未回收时主窗口收不到 WM_PAINT，
        //   表现为"点选后开关很久才变"。bMsg==true（通知已发出）时不做同步
        //   绘制：回调里可能再 SetOn / 弹模态框，同步重绘会嵌进通知链造成重入。
        if (!bMsg) SyncRepaint();
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
                // [PERF-SW] 同步重绘已抽进 SetOn(bOn,false)->SyncRepaint()，
                //   这里不再重复 InvalidateRect + UpdateWindow。
                if (m_pManager != nullptr)
                {
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
        // [PERF-SW] 脏矩形不相交直接返回：DuiLib 只按父容器判断是否进子控件
        //   Paint，整页几十个开关即使没变化也会进这里。原来不管相不相交都
        //   照画（还顺带建/删 GDI 对象），这是 hover 时整屏卡顿的主因之一。
        //   绘制区域以 m_rcItem 与 rcPaint 的交集为准，连碰都不碰才算省。
        RECT rcClip = { 0, 0, 0, 0 };
        if (!::IntersectRect(&rcClip, &m_rcItem, &rcPaint) && !::IsRectEmpty(&rcPaint))
            return true;

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
        // [PERF-SW] 填充/描边改为取缓存对象：稳态下每帧 0 次 Create/Delete；
        //   几何与绘制顺序保持原样（先填后描，1px 描边才不会被填充盖掉）。
        const DWORD crAccent = tokens::Token(tokens::EToken::Accent);

        const DWORD crBorder = tokens::Token(tokens::EToken::Border);
        const DWORD crSunk   = tokens::Token(tokens::EToken::SurfaceSunk);
        if (m_bOn)
        {
            HBRUSH hbrT = customcontrols_gdi_cache::GetCachedBrush(toCref(crAccent));
            HPEN   hpenT = customcontrols_gdi_cache::GetCachedPen(toCref(crAccent));
            HGDIOBJ o1 = (hbrT != nullptr) ? ::SelectObject(hDC, hbrT) : nullptr;
            HGDIOBJ o2 = (hpenT != nullptr) ? ::SelectObject(hDC, hpenT) : nullptr;
            ::RoundRect(hDC, rcTrack.left, rcTrack.top, rcTrack.right, rcTrack.bottom,
                        SW_H, SW_H);
            if (o1 != nullptr) ::SelectObject(hDC, o1);
            if (o2 != nullptr) ::SelectObject(hDC, o2);
        }
        else
        {
            // 空心：先用底色填出内部，再描边。顺序不能反 ——
            //   先描边再填充会把 1px 描边盖掉，边缘重新糊成一片（旧实现就是这样）。
            HBRUSH hbrInside = customcontrols_gdi_cache::GetCachedBrush(toCref(crSunk));
            HGDIOBJ o1 = (hbrInside != nullptr) ? ::SelectObject(hDC, hbrInside) : nullptr;
            HGDIOBJ o2 = ::SelectObject(hDC, ::GetStockObject(NULL_PEN));
            ::RoundRect(hDC, rcTrack.left, rcTrack.top, rcTrack.right, rcTrack.bottom,
                        SW_H, SW_H);
            if (o1 != nullptr) ::SelectObject(hDC, o1);
            if (o2 != nullptr) ::SelectObject(hDC, o2);

            HPEN hpenB = customcontrols_gdi_cache::GetCachedPen(toCref(crBorder));
            HGDIOBJ o3 = ::SelectObject(hDC, ::GetStockObject(NULL_BRUSH));
            HGDIOBJ o4 = (hpenB != nullptr) ? ::SelectObject(hDC, hpenB) : nullptr;
            ::RoundRect(hDC, rcTrack.left, rcTrack.top, rcTrack.right, rcTrack.bottom,
                        SW_H, SW_H);
            if (o3 != nullptr) ::SelectObject(hDC, o3);
            if (o4 != nullptr) ::SelectObject(hDC, o4);
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
        HBRUSH hbrK = customcontrols_gdi_cache::GetCachedBrush(toCref(crKnob));
        HPEN   hpenK = customcontrols_gdi_cache::GetCachedPen(toCref(crKnob));
        HGDIOBJ o5 = (hbrK != nullptr) ? ::SelectObject(hDC, hbrK) : nullptr;
        HGDIOBJ o6 = (hpenK != nullptr) ? ::SelectObject(hDC, hpenK) : nullptr;
        ::Ellipse(hDC, rcKnob.left, rcKnob.top, rcKnob.right, rcKnob.bottom);
        if (o5 != nullptr) ::SelectObject(hDC, o5);
        if (o6 != nullptr) ::SelectObject(hDC, o6);

        // [PERF-SW] 缓存触顶时产生的临时对象在绘制完成后统一回收
        //   （此时 DC 已换回旧对象，句柄无人引用）。
        customcontrols_gdi_cache::FlushCachedVolatile();
        return true;
    }

protected:
    // [PERF-SW] 本控件矩形同步重绘：只失效自身，避免整窗 InvalidateRange
    //   连带重画同层其它控件；UpdateWindow 立刻走 WM_PAINT，不等消息队列。
    //   只在 manager 与宿主窗口都有效时执行（销毁/换窗期间用的是陈旧的
    //   HWND，IsWindow 挡掉悬垂窗口句柄）。
    void SyncRepaint()
    {
        if (m_pManager == nullptr) return;
        HWND hPaint = m_pManager->GetPaintWindow();
        if (hPaint == nullptr || !::IsWindow(hPaint)) return;
        ::InvalidateRect(hPaint, &m_rcItem, FALSE);
        ::UpdateWindow(hPaint);
    }

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
        // [PERF-SW] 方框与脏矩形不相交时跳过：复选行整页几十个，滚动/悬停
        //   都会进本函数，原本每个都 FillRect + 描边 + 勾。
        RECT rcClip = { 0, 0, 0, 0 };
        if (::IntersectRect(&rcClip, &m_rcItem, &rcPaint) || ::IsRectEmpty(&rcPaint))
            DrawBox(hDC);
        return bRet;
    }

private:
    // [PERF-SW] DPI 度量缓存：原实现每次绘制向 manager 取两次 GetDPIObj 并
    //   Scale()（DPI 对象访问 + 取整），实际只取决于 m_rcItem 尺寸与 DPI 比例。
    //   这里按 (尺寸, 比例) 比较，命中即复用；尺寸或比例一变（布局/换显示器/
    //   缩放变更）就重算，保证与原来逐帧计算的结果一致。
    void UpdateBoxMetrics(int nScale)
    {
        if (m_bBoxMetricsValid
            && m_rcBoxCache.left   == m_rcItem.left
            && m_rcBoxCache.top    == m_rcItem.top
            && m_rcBoxCache.right  == m_rcItem.right
            && m_rcBoxCache.bottom == m_rcItem.bottom
            && m_nBoxScaleCache == nScale)
            return;
        const int nBox = nScale > 0 ? MulDiv(BOX, nScale, 100) : BOX;
        const int nPad = nScale > 0 ? MulDiv(6, nScale, 100) : 6;
        m_nBoxDpi = nBox;
        m_nBoxPad = nPad;
        m_rcBoxCache = m_rcItem;
        m_nBoxScaleCache = nScale;
        m_bBoxMetricsValid = true;
    }

    void DrawBox(HDC hDC)
    {
        if (m_rcItem.right <= m_rcItem.left) return;
        // 主题令牌不变，比例也基本不变：取一次比例即可（GetDPIObj 只在有
        //   manager 时可用，缺失时按 0 处理 = 不缩放，与原 nDpi=BOX 语义一致）。
        const int nScale = (m_pManager != nullptr && m_pManager->GetDPIObj() != nullptr)
            ? static_cast<int>(m_pManager->GetDPIObj()->GetScale()) : 0;
        UpdateBoxMetrics(nScale);
        const int nDpi = m_nBoxDpi;
        const int nPad = m_nBoxPad;

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
        // [PERF-SW] 画刷取缓存，不再每次 Create/Delete；FillRect 不改 DC 选择，
        //   画刷可长期存于缓存（仅 UI 线程）。
        HBRUSH hbr = customcontrols_gdi_cache::GetCachedBrush(toCref(crFill));
        if (hbr != nullptr)
            ::FillRect(hDC, &rc, hbr);
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
        // [PERF-SW] 缓存触顶时的临时对象在绘制结束后回收。
        customcontrols_gdi_cache::FlushCachedVolatile();
    }

    int m_nBoxState;
    // [PERF-SW] 方框 DPI 度量缓存（见 UpdateBoxMetrics）：首次绘制或
    //   m_rcItem 尺寸/DPI 比例变化时失效重算。
    bool m_bBoxMetricsValid = false;
    RECT m_rcBoxCache = { 0, 0, 0, 0 };
    int  m_nBoxScaleCache = -1;
    int  m_nBoxDpi = BOX;
    int  m_nBoxPad = 6;
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
