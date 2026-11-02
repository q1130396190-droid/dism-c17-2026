#pragma once
// ============================================================
// Page_ToolsCards.h —— 工具箱页卡片/图标自绘层（跨编译单元共享）。
//
// [R133-1] 从 Page_Tools.cpp 抽出 837 行纯 UI 绘制代码：
//   IToolCardState   卡片交互态桥（hover/press 视觉）
//   CToolIconUI      64x64 Fluent 风格工具图标（11 种 kind）
//   CClickVLayoutUI  可点击卡片容器（实现 IToolCardState）
//   另含 MakeRoundRect / LerpColor / BuildRoundRect 三个绘制辅助
//
// 闭包证明：m_ctx 0 处、CPageTools:: 0 处、StringMgr 0 处、
// 所有 Tool* 命名空间 0 处；绘制辅助均在本段内定义。纯 GDI+/DuiLib，
// 为自包含单元。
// ============================================================
#include "stdafx.h"
#include <algorithm>
#include <cmath>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
#include "CustomControls.h"
#include "HostInterfaces.h"
#include "ThemeTokens.h"   // [R209] 卡片底色随主题，不再写死纯白
namespace ToolCards
{
// 图标绘制基准尺寸（设计稿 64x64，按 DPI 缩放）
inline constexpr int kIconDesignSize = 64;
// 图标 kind 总数（0..10 有效，11 为越界兜底齿轮）
inline constexpr int kIconKindCount = 11;
class IToolCardState
{
public:
    virtual ~IToolCardState() {}
    virtual bool  IsCardHot() const = 0;
    virtual bool  IsCardPushed() const = 0;
    virtual float CardHoverAnim() const = 0;   // 0..1，Win11 hover 过渡进度
};
class CToolIconUI : public CControlUI
{
public:
    CToolIconUI() : m_nKind(0)
    {
        // [U-2] 仅固定高度；宽度交给父布局拉伸（EstimateSize cx=0），
        // DoPaint 内按实际宽度水平居中。避免"固定 64 宽 + 容器 DT_LEFT
        // 默认对齐"把图标钉在卡片左侧、与居中标题错位。
        SetFixedHeight(64);
    }
    // [U-2] 查询父卡片交互态（图标自身不接收鼠标事件）
    const IToolCardState* CardState() const
    {
        CControlUI* pParent = GetParent();
        if (pParent == nullptr) return nullptr;
        // GetInterface 返回 LPVOID（void*），指向派生控件内部的接口实现体。
        // 以 void* -> const void* -> const IToolCardState* 两步 static_cast 表达，
        // 语义比 reinterpret_cast 更严格（第一步走标准指针转换，第二步走对象指针下行转换）。
        const void* pIface = pParent->GetInterface(L"ToolCardState");
        return static_cast<const IToolCardState*>(pIface);
    }
    // SetKind: clamp 0..12 safely; callers (BpBuildCard) only send 0..12
    // but defensive coding prevents out-of-bounds read on s_top/s_bot.
    void SetKind(int nKind)
    {
        if (nKind < 0)  nKind = 0;
        if (nKind >= 13) nKind = 12;
        if (nKind != m_nKind) {
            m_nKind = nKind;
            Invalidate();
        }
    }
    // [U-2] DPI 变化无需钩子：本库 CControlUI 无 SetDpi 虚函数，且 DoPaint
    // 全程矢量绘制、尺寸经 DPIObj()->Scale 换算，DPI 改变后框架重排+重绘
    // 自然清晰，旧的 SetDpi override 无法编译，已移除。
    virtual bool DoPaint(HDC hDC, const RECT& rcPaint, CControlUI* pStopControl)
    {
        using namespace Gdiplus;
        // [U-3] Win11 Fluent 应用图标配色：同色相低饱和竖向渐变，
        // 顶亮底深；去掉旧版 iOS 化的高饱和/撞色（如靛蓝、亮橙）。
        static const Color s_top[13] = {
            Color(0xFF,0x3A,0x96,0xDD),  // 0 系统备份   Communication Blue
            Color(0xFF,0x28,0xB4,0xC4),  // 1 系统还原   Teal
            Color(0xFF,0x4B,0xA9,0x4F),  // 2 账户管理   Excel/Green
            Color(0xFF,0xF7,0xA5,0x41),  // 3 引导修复   Orange
            Color(0xFF,0x9B,0x7F,0xE8),  // 4 春哥附体   Purple
            Color(0xFF,0x2E,0xC5,0xD4),  // 5 ESD转ISO   Cyan
            Color(0xFF,0x1F,0xC8,0xA9),  // 6 WIM/ESD互转 Mint
            Color(0xFF,0x64,0x79,0x8E),  // 7 ImageX     Graphite
            Color(0xFF,0xE8,0x5D,0x52),  // 8 文件浏览器 Red
            Color(0xFF,0xFF,0xC4,0x2E),  // 9 ISO生成器  Amber
            Color(0xFF,0x6E,0x84,0x99),  //10 hosts编辑器 Slate
            Color(0xFF,0x14,0xB8,0xA6),  //11 激活备份   Deep Teal
            Color(0xFF,0x5B,0x8D,0xEF)   //12 Win11预处理 Azure
        };
        static const Color s_bot[13] = {
            Color(0xFF,0x0F,0x6C,0xBD),  Color(0xFF,0x0E,0x7C,0x8E),
            Color(0xFF,0x10,0x7C,0x10),  Color(0xFF,0xCA,0x50,0x10),
            Color(0xFF,0x6B,0x53,0xD6),  Color(0xFF,0x00,0x99,0xA4),
            Color(0xFF,0x00,0x85,0x75),  Color(0xFF,0x3B,0x4E,0x5E),
            Color(0xFF,0xB0,0x22,0x26),  Color(0xFF,0xE0,0x86,0x00),
            Color(0xFF,0x41,0x56,0x6B),  Color(0xFF,0x0A,0x6E,0x72),
            Color(0xFF,0x2B,0x4C,0xA8)
        };
        const bool bEnabled = IsEnabled();
        // [U-2] 交互态改从父卡片读取（本控件 MouseEnabled=false）
        const IToolCardState* pCard = CardState();
        const bool bPush = (bEnabled && pCard != nullptr) ? pCard->IsCardPushed() : false;
        float fHover = (bEnabled && pCard != nullptr) ? pCard->CardHoverAnim() : 0.0f;
        if (fHover < 0.0f) fHover = 0.0f;
        else if (fHover > 1.0f) fHover = 1.0f;
        int w = m_rcItem.right - m_rcItem.left;
        int h = m_rcItem.bottom - m_rcItem.top;
        if (w <= 0 || h <= 0) return true;   // clip guard
        Graphics g(hDC);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        // [U-2] 全程矢量绘制（无 DrawImage），HighQualityBicubic 毫无收益；
        // Half 像素偏移让 0.75px 描边在 125%/150% 等非整数倍 DPI 下均匀不发虚。
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        // 以高度为基准等比映射到 64x64 设计空间（矢量在任意 DPI 下重算，保持
        // 锐利）；水平方向按控件实际宽度居中，修复图标左偏。
        const REAL kScale = (REAL)h / 64.0f;
        const REAL kOffsetX = ((REAL)w / kScale - 64.0f) * 0.5f;
        g.TranslateTransform((REAL)m_rcItem.left, (REAL)m_rcItem.top);
        g.ScaleTransform(kScale, kScale);
        g.TranslateTransform(kOffsetX, 0.0f);
        // Win11 微交互：hover 1.00→1.03 轻抬升，press 收到 0.97。
        const REAL kPress = bPush ? 0.97f : (1.0f + 0.03f * fHover);
        g.TranslateTransform(32.0f, 32.0f);
        g.ScaleTransform(kPress, kPress);
        g.TranslateTransform(-32.0f, -32.0f);
        const Color cTop = s_top[m_nKind];
        const Color cBot = s_bot[m_nKind];
        // ---- Tile (Win11 Fluent: r=12 rounded square) ------------------
        GraphicsPath tile;
        MakeRoundRect(tile, 2.0f, 2.0f, 60.0f, 60.0f, 12.0f);
        LinearGradientBrush brTile(Point(0, 2), Point(0, 62), cTop, cBot);
        g.FillPath(&brTile, &tile);
        // ---- Border (Win11: 0.75px 18-alpha — softer than Aero 1.0px) -
        Pen penEdge(Color(18, 0, 0, 0), 0.75f);
        g.DrawPath(&penEdge, &tile);
        // ---- State overlays ---------------------------------------------
        // Disabled: desaturate with a neutral gray wash (65% opacity).
        // Pressed: darken with 11% black.
        // Hover: white overlay fades 0→12% with the card transition.
        if (!bEnabled) {
            SolidBrush brDim(Color(166, 128, 128, 128));
            g.FillPath(&brDim, &tile);
        } else {
            if (bPush) {
                SolidBrush brPush(Color(28, 0, 0, 0));
                g.FillPath(&brPush, &tile);
            }
            int nHoverAlpha = static_cast<int>(31.0f * fHover + 0.5f);
            if (nHoverAlpha > 0) {
                SolidBrush brHot(Color(static_cast<BYTE>(nHoverAlpha), 255, 255, 255));
                g.FillPath(&brHot, &tile);
            }
        }
        // ---- Glyph -------------------------------------------------------
        // [U-3] Win11 Fluent：单层扁平白色 glyph（取消旧的 1px 黑色投影，
        // 投影在小尺寸上只会显脏）。统一裁剪到圆角板内，贴边元素不溢出。
        GraphicsState gsClip = g.Save();
        g.SetClip(&tile);
        Color cGlyph    = bEnabled ? Color(242, 255, 255, 255)  : Color(102, 220, 220, 220);
        // 个别 glyph 用瓦片底色做反白细节（如白盾内的对勾）；禁用态转中性灰
        Color cAccent   = bEnabled ? cBot                       : Color(255, 120, 120, 120);
        PaintGlyph(g, m_nKind, cGlyph, cAccent);
        g.Restore(gsClip);
        return true;
    }



protected:
    int  m_nKind;



private:
    static void MakeRoundRect(Gdiplus::GraphicsPath& p,
                              float x, float y, float w, float h, float r)
    {
        using namespace Gdiplus;
        p.Reset();
        p.AddArc(x, y, 2*r, 2*r, 180.0f, 90.0f);
        p.AddArc(x + w - 2*r, y, 2*r, 2*r, 270.0f, 90.0f);
        p.AddArc(x + w - 2*r, y + h - 2*r, 2*r, 2*r, 0.0f, 90.0f);
        p.AddArc(x, y + h - 2*r, 2*r, 2*r, 90.0f, 90.0f);
        p.CloseFigure();
    }


    // [AUDIT-REFACTOR] 原 PaintGlyph 内联 case，原样搬出（零改动）。
    // [R109] 13 个图标绘制函数改为**声明**，定义移入 Page_ToolsCardsGlyph.cpp。
    //   为什么这样拆：它们原先是类内定义的 static 成员（隐式 inline），
    //   占了本文件 38% 的行数（257/669），而每个函数的函数体互相独立——
    //   只是共享 GDI+ 的 Pen/Brush 建法。把定义挪出类外不改变调用方式
    //   （仍是 CToolIconUI::PaintGlyph_Xxx），也不引入虚函数开销。
    //   代价：13 个符号变成跨 TU 外部链接，链接期解析而非编译期内联。
    //   这是有意的取舍——绘制单次开销远大于一次普通调用，实测见报告。
    static void PaintGlyph_SystemBackup(Gdiplus::Graphics&, Gdiplus::Pen&, Gdiplus::Pen&, Gdiplus::SolidBrush&, Gdiplus::Color, Gdiplus::Color);
    static void PaintGlyph_SystemRestore(Gdiplus::Graphics&, Gdiplus::Pen&, Gdiplus::Pen&, Gdiplus::SolidBrush&, Gdiplus::Color, Gdiplus::Color);
    static void PaintGlyph_AccountManage(Gdiplus::Graphics&, Gdiplus::Pen&, Gdiplus::Pen&, Gdiplus::SolidBrush&, Gdiplus::Color, Gdiplus::Color);
    static void PaintGlyph_BootFix(Gdiplus::Graphics&, Gdiplus::Pen&, Gdiplus::Pen&, Gdiplus::SolidBrush&, Gdiplus::Color, Gdiplus::Color);
    static void PaintGlyph_ChunkHelper(Gdiplus::Graphics&, Gdiplus::Pen&, Gdiplus::Pen&, Gdiplus::SolidBrush&, Gdiplus::Color, Gdiplus::Color);
    static void PaintGlyph_EsdToIso(Gdiplus::Graphics&, Gdiplus::Pen&, Gdiplus::Pen&, Gdiplus::SolidBrush&, Gdiplus::Color, Gdiplus::Color);
    static void PaintGlyph_WimToEsd(Gdiplus::Graphics&, Gdiplus::Pen&, Gdiplus::Pen&, Gdiplus::SolidBrush&, Gdiplus::Color, Gdiplus::Color);
    static void PaintGlyph_ImageX(Gdiplus::Graphics&, Gdiplus::Pen&, Gdiplus::Pen&, Gdiplus::SolidBrush&, Gdiplus::Color, Gdiplus::Color);
    static void PaintGlyph_FileBrowser(Gdiplus::Graphics&, Gdiplus::Pen&, Gdiplus::Pen&, Gdiplus::SolidBrush&, Gdiplus::Color, Gdiplus::Color);
    static void PaintGlyph_IsoGen(Gdiplus::Graphics&, Gdiplus::Pen&, Gdiplus::Pen&, Gdiplus::SolidBrush&, Gdiplus::Color, Gdiplus::Color);
    static void PaintGlyph_HostsEditor(Gdiplus::Graphics&, Gdiplus::Pen&, Gdiplus::Pen&, Gdiplus::SolidBrush&, Gdiplus::Color, Gdiplus::Color);
    static void PaintGlyph_Activation(Gdiplus::Graphics&, Gdiplus::Pen&, Gdiplus::Pen&, Gdiplus::SolidBrush&, Gdiplus::Color, Gdiplus::Color);
    static void PaintGlyph_Win11Prep(Gdiplus::Graphics&, Gdiplus::Pen&, Gdiplus::Pen&, Gdiplus::SolidBrush&, Gdiplus::Color, Gdiplus::Color);
    // [AUDIT-REFACTOR] 原 422 行 / 12 个内联 case 的巨型 switch，改为：
    //   12 个独立绘制函数（PaintGlyph_*）+ 函数指针表分派。
    // 收益：单函数从 422 行降到 20 行，新增图标只需加一个函数 + 一行表项，
    // 且每个图标的绘制逻辑可单独阅读/比对，不再在 400 行 switch 里翻找。
    static void PaintGlyph(Gdiplus::Graphics& g, int kind,
                           Gdiplus::Color c, Gdiplus::Color ac)
    {
        using namespace Gdiplus;
        SolidBrush br(c);
        Pen pen(c, 3.2f);
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        pen.SetLineJoin(LineJoinRound);
        Pen penThin(c, 2.4f);
        penThin.SetStartCap(LineCapRound);
        penThin.SetEndCap(LineCapRound);
        penThin.SetLineJoin(LineJoinRound);
        // 分派表：索引即 kind。越界/未注册一律落到安全带（齿轮轮廓）。
        using GlyphFn = void(*)(Graphics&, Pen&, Pen&, SolidBrush&, Color, Color);
        static constexpr GlyphFn kTable[] = {
            &PaintGlyph_SystemBackup,   // 0
            &PaintGlyph_SystemRestore,   // 1
            &PaintGlyph_AccountManage,   // 2
            &PaintGlyph_BootFix,   // 3
            &PaintGlyph_ChunkHelper,   // 4
            &PaintGlyph_EsdToIso,   // 5
            &PaintGlyph_WimToEsd,   // 6
            &PaintGlyph_ImageX,   // 7
            &PaintGlyph_FileBrowser,   // 8
            &PaintGlyph_IsoGen,   // 9
            &PaintGlyph_HostsEditor,   // 10
            &PaintGlyph_Activation,   // 11
            &PaintGlyph_Win11Prep,   // 12
        };
        constexpr int kCount = static_cast<int>(sizeof(kTable) / sizeof(kTable[0]));
        if (kind >= 0 && kind < kCount)
        {
            kTable[kind](g, pen, penThin, br, c, ac);
            return;
        }
        // ---- safety net: gear outline for any out-of-range kind ----------
        {
            Pen penDef(c, 2.4f);
            penDef.SetStartCap(LineCapRound);
            penDef.SetEndCap(LineCapRound);
            Pen penThinDef(c, 1.8f);
            penThinDef.SetStartCap(LineCapRound);
            penThinDef.SetEndCap(LineCapRound);
            g.DrawEllipse(&penDef, 24.0f, 24.0f, 16.0f, 16.0f);
            for (int i = 0; i < 8; ++i) {
                float a = static_cast<float>(i) * 45.0f * 3.14159265f / 180.0f;
                g.DrawLine(&penThinDef,
                    32.0f + 9.0f * cosf(a), 32.0f + 9.0f * sinf(a),
                    32.0f + 12.0f * cosf(a), 32.0f + 12.0f * sinf(a));
            }
        }
    }
};



// [U-2] Win11 Fluent 工具卡片：圆角底 + 1px 细边 + hover/press 100ms 过渡
// + 底部内阴影。基类 CControlUI 不识别 hotbkcolor/pushedbkcolor（SetAttribute
// 直接忽略），旧卡片完全无状态反馈，故背景与边框全部自绘；子控件仍走基类布局。
class CClickVLayoutUI : public CVerticalLayoutUI, public IToolCardState
{
public:
    CClickVLayoutUI()
        : m_bHot(false), m_bPushed(false)
        , m_fHover(0.0f), m_fHoverTarget(0.0f) {}
    virtual ~CClickVLayoutUI()
    {
        // 控件销毁时撤掉动画定时器，防止 UIManager 悬挂回调
        if (m_pManager != nullptr) m_pManager->KillTimer(this, kAnimTimerId);
    }
    void* GetInterface(const wchar_t* pstrName)
    {
        if (lstrcmpiW(pstrName, L"ToolCardState") == 0)
            return static_cast<IToolCardState*>(this);
        return CVerticalLayoutUI::GetInterface(pstrName);
    }
    // ---- IToolCardState（供内部 CToolIconUI 读取交互态）----
    virtual bool  IsCardHot() const    { return m_bHot; }
    virtual bool  IsCardPushed() const { return m_bPushed; }
    virtual float CardHoverAnim() const { return m_fHover; }
    virtual void DoEvent(TEventUI& event)
    {
        if (event.Type == UIEVENT_MOUSEENTER) {
            if (IsEnabled() && !m_bHot) { m_bHot = true; StartAnim(1.0f); }
        }
        else if (event.Type == UIEVENT_MOUSELEAVE) {
            m_bHot = false; m_bPushed = false; StartAnim(0.0f);
        }
        else if (event.Type == UIEVENT_BUTTONDOWN) {
            if (IsEnabled()) { m_bPushed = true; Invalidate(); }
        }
        else if (event.Type == UIEVENT_BUTTONUP) {
            bool bFired = m_bPushed;
            m_bPushed = false;
            Invalidate();
            // [U-2] 禁用卡（依赖缺失）不发 click；且仅在卡内松开才触发
            if (bFired && IsEnabled() && ::PtInRect(&m_rcItem, event.ptMouse)
                && m_pManager != nullptr)
            {
                m_pManager->SendNotify(this, DUI_MSGTYPE_CLICK);
            }
        }
        else if (event.Type == UIEVENT_TIMER && event.wParam == kAnimTimerId) {
            TickAnim();
        }
        CVerticalLayoutUI::DoEvent(event);
    }
    // 先画卡片底板，再走基类流程绘制全部子控件
    virtual bool DoPaint(HDC hDC, const RECT& rcPaint, CControlUI* pStopControl)
    {
        PaintCardBg(hDC);
        return CVerticalLayoutUI::DoPaint(hDC, rcPaint, pStopControl);
    }
private:
    enum { kAnimTimerId = 0x7431 };   // 'TC1'
    bool  m_bHot;
    bool  m_bPushed;
    float m_fHover;         // 当前动画进度 0..1
    float m_fHoverTarget;   // 动画目标
    void StartAnim(float fTarget)
    {
        if (m_fHover == fTarget) return;
        m_fHoverTarget = fTarget;
        // [FIX-P2S] 16ms→32ms 合帧：多卡片快速扫过时 hover 动画叠加，
        // 每帧 Gdiplus PaintCardBg 全卡重绘负载减半；步长同比加倍，
        // 100ms 总过渡时长不变，视觉平滑度差异不可感知。
        if (m_pManager != nullptr) m_pManager->SetTimer(this, kAnimTimerId, 32);
        TickAnim();   // 立即推进一帧，消除首帧迟滞
    }
    void TickAnim()
    {
        // 32ms/帧 × 0.32 ≈ 100ms 到位（Win11 标准 control 过渡时长）
        constexpr float kStep = 0.32f;
        if (m_fHover < m_fHoverTarget)
            m_fHover = (m_fHover + kStep >= m_fHoverTarget) ? m_fHoverTarget
                                                             : m_fHover + kStep;
        else if (m_fHover > m_fHoverTarget)
            m_fHover = (m_fHover - kStep <= m_fHoverTarget) ? m_fHoverTarget
                                                             : m_fHover - kStep;
        if (m_fHover == m_fHoverTarget && m_pManager != nullptr)
            m_pManager->KillTimer(this, kAnimTimerId);
        Invalidate();
    }
    static Gdiplus::Color LerpColor(const Gdiplus::Color& c1,
                                    const Gdiplus::Color& c2, float t)
    {
        using namespace Gdiplus;
        if (t <= 0.0f) return c1;
        if (t >= 1.0f) return c2;
        return Color(
            static_cast<BYTE>(c1.GetA() + static_cast<INT>(static_cast<INT>(c2.GetA()) - c1.GetA()) * t),
            static_cast<BYTE>(c1.GetR() + static_cast<INT>(static_cast<INT>(c2.GetR()) - c1.GetR()) * t),
            static_cast<BYTE>(c1.GetG() + static_cast<INT>(static_cast<INT>(c2.GetG()) - c1.GetG()) * t),
            static_cast<BYTE>(c1.GetB() + static_cast<INT>(static_cast<INT>(c2.GetB()) - c1.GetB()) * t));
    }
    static void BuildRoundRect(Gdiplus::GraphicsPath& path,
                               const Gdiplus::RectF& rc, Gdiplus::REAL r)
    {
        using namespace Gdiplus;
        const REAL d = r * 2.0f;
        path.Reset();
        path.AddArc(rc.X, rc.Y, d, d, 180.0f, 90.0f);
        path.AddArc(rc.GetRight() - d, rc.Y, d, d, 270.0f, 90.0f);
        path.AddArc(rc.GetRight() - d, rc.GetBottom() - d, d, d, 0.0f, 90.0f);
        path.AddArc(rc.X, rc.GetBottom() - d, d, d, 90.0f, 90.0f);
        path.CloseFigure();
    }
    void PaintCardBg(HDC hDC)
    {
        using namespace Gdiplus;
        // 圆角 8dp，随 DPI 缩放（与卡片内边距 8dp 一致，内容不触及圆角）
        int nRadius = (m_pManager != nullptr) ? m_pManager->GetDPIObj()->Scale(8) : 8;
        if (nRadius < 2) nRadius = 2;
        // [R209] 原实现把卡片底色写死成纯白(255,255,255)、边框写死成 #E3E3E3，
        //   而这里是 GDI+ 自绘，绕过了控件属性，深色主题下就成了满屏白块
        //   （像素验收实测 Tools 页 content=#FFFFFF、亮块占比 28%）。
        //   改为从令牌取色：浅色下与原值一致，深色下自动跟随主题。
        auto tok = [](DWORD cr) -> Color
        {
            return Color(255, (cr >> 16) & 0xFF, (cr >> 8) & 0xFF, cr & 0xFF);
        };
        const Color cBgNormal = tok(tokens::Token(tokens::EToken::Surface));
        const Color cBgHover  = tok(tokens::Token(tokens::EToken::SurfaceHover));
        const Color cBgPress  = tok(tokens::Token(tokens::EToken::SurfaceSunk));
        const Color cBdNormal = tok(tokens::Token(tokens::EToken::BorderSoft));
        const Color cBdHover  = tok(tokens::Token(tokens::EToken::Border));
        const Color cBdPress  = tok(tokens::Token(tokens::EToken::Border));
        const Color cBgDisabled = tok(tokens::Token(tokens::EToken::SurfaceAlt));
        const Color cBdDisabled = tok(tokens::Token(tokens::EToken::BorderSoft));
        Color cBg, cBd;
        if (!IsEnabled()) {
            cBg = cBgDisabled;
            cBd = cBdDisabled;
        } else {
            cBg = LerpColor(cBgNormal, cBgHover, m_fHover);
            cBd = LerpColor(cBdNormal, cBdHover, m_fHover);
            if (m_bPushed) {
                cBg = LerpColor(cBg, cBgPress, 0.85f);
                cBd = LerpColor(cBd, cBdPress, 0.85f);
            }
        }
        // 0.5px 内缩，避免右/下边框被物理像素边界吞掉半像素
        RectF rc((REAL)m_rcItem.left + 0.5f, (REAL)m_rcItem.top + 0.5f,
                 (REAL)(m_rcItem.right - m_rcItem.left) - 1.0f,
                 (REAL)(m_rcItem.bottom - m_rcItem.top) - 1.0f);
        Graphics g(hDC);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        GraphicsPath path;
        BuildRoundRect(path, rc, (REAL)nRadius);
        SolidBrush brBg(cBg);
        g.FillPath(&brBg, &path);
        // 底部内阴影（3dp，全部在自身圆角区域内，无越界重绘/残留）：
        // hover 时阴影加深，强化卡片"浮起"感
        int nShadow = (m_pManager != nullptr) ? m_pManager->GetDPIObj()->Scale(3) : 3;
        if (nShadow < 2) nShadow = 2;
        BYTE aShadow = static_cast<BYTE>(10 + (int)(12.0f * m_fHover + 0.5f));
        GraphicsState gs = g.Save();
        g.SetClip(&path);
        RectF rcShadow(rc.X, rc.GetBottom() - (REAL)nShadow,
                       rc.Width, (REAL)nShadow);
        LinearGradientBrush brShadow(
            Point(0, static_cast<INT>(rcShadow.GetTop())),
            Point(0, static_cast<INT>(rcShadow.GetBottom())),
            Color(0, 0, 0, 0), Color(aShadow, 0, 0, 0));
        g.FillRectangle(&brShadow, rcShadow);
        g.Restore(gs);
        Pen penBorder(cBd, 1.0f);
        g.DrawPath(&penBorder, &path);
    }
};
} // namespace ToolCards
