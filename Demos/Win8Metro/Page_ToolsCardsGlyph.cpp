// ============================================================
// Page_ToolsCardsGlyph.cpp —— 工具箱卡片 13 个图标的手绘实现。
//
// [R109] 从 Page_ToolsCards.h 拆出（原为类内定义的 static 成员）。
//   拆分依据：13 个函数占头文件 38% 行数（253/669），彼此独立、
//   只共享 GDI+ 的 Pen/Brush 初始化风格，混在类定义里让
//   "卡片交互态 / 背景绘制"这类真正的类职责淹没在图形代码中。
//
// 为什么保留 static 成员而非改自由函数：
//   调用点是 CToolIconUI::PaintGlyph 内的分派表 `&PaintGlyph_Xxx`，
//   改自由函数要改指针类型与全部 13 个表项；保留成员身份零改动。
//
// 代价（如实记录）：定义移出类外后隐式 inline 变外部链接，
//   13 个符号改为链接期解析。单次 GDI+ 绘制含多次 Path/Transform，
//   这点调用开销相对可忽略 —— 但这是取舍，不是零成本。
// ============================================================

#include "stdafx.h"

#include <gdiplus.h>

#include "Page_ToolsCards.h"

namespace ToolCards
{

void CToolIconUI::PaintGlyph_SystemBackup(Gdiplus::Graphics& g, Gdiplus::Pen& pen,
                             Gdiplus::Pen& penThin, Gdiplus::SolidBrush& br,
                             Gdiplus::Color c, Gdiplus::Color ac)
{
    using namespace Gdiplus;
    (void)penThin; (void)br; (void)ac;   // 各分支使用的资源不同，统一以 void 抑制未用告警
GraphicsPath drv;
MakeRoundRect(drv, 12.0f, 26.0f, 40.0f, 22.0f, 5.0f);
g.DrawPath(&pen, &drv);
g.FillEllipse(&br, 15.8f, 34.8f, 4.4f, 4.4f);
g.DrawLine(&pen, 32.0f, 25.0f, 32.0f, 13.0f);
g.DrawLine(&pen, 25.0f, 20.0f, 32.0f, 13.0f);
g.DrawLine(&pen, 32.0f, 13.0f, 39.0f, 20.0f);
}
// [AUDIT-REFACTOR] 原 PaintGlyph 内联 case，原样搬出（零改动）。
void CToolIconUI::PaintGlyph_SystemRestore(Gdiplus::Graphics& g, Gdiplus::Pen& pen,
                             Gdiplus::Pen& penThin, Gdiplus::SolidBrush& br,
                             Gdiplus::Color c, Gdiplus::Color ac)
{
    using namespace Gdiplus;
    (void)penThin; (void)br; (void)ac;   // 各分支使用的资源不同，统一以 void 抑制未用告警
// 表盘（居中于 64 网格）
g.DrawEllipse(&pen, 19.0f, 20.0f, 26.0f, 26.0f);
g.DrawLine(&pen, 32.0f, 33.0f, 32.0f, 26.0f);
g.DrawLine(&pen, 32.0f, 33.0f, 38.5f, 36.5f);
// 同心环绕圆弧（r=17），顺时针越过表盘顶部
g.DrawArc(&pen, 15.0f, 12.0f, 34.0f, 34.0f, 205.0f, 125.0f);
// 弧终点（约 330°，右上）的箭头
g.DrawLine(&pen, 46.7f, 20.5f, 42.6f, 20.0f);
g.DrawLine(&pen, 46.7f, 20.5f, 45.8f, 24.6f);
}
// [AUDIT-REFACTOR] 原 PaintGlyph 内联 case，原样搬出（零改动）。
void CToolIconUI::PaintGlyph_AccountManage(Gdiplus::Graphics& g, Gdiplus::Pen& pen,
                             Gdiplus::Pen& penThin, Gdiplus::SolidBrush& br,
                             Gdiplus::Color c, Gdiplus::Color ac)
{
    using namespace Gdiplus;
    (void)penThin; (void)br; (void)ac;   // 各分支使用的资源不同，统一以 void 抑制未用告警
Color back(c.GetA() * 150 / 255, 255, 255, 255);
SolidBrush brBack(back);
g.FillEllipse(&brBack, 19.5f, 17.0f, 9.0f, 9.0f);
GraphicsPath bodyBack;
bodyBack.AddArc(14.0f, 27.0f, 20.0f, 18.0f, 180.0f, 180.0f);
bodyBack.AddLine(34.0f, 36.0f, 34.0f, 48.0f);
bodyBack.AddLine(34.0f, 48.0f, 14.0f, 48.0f);
bodyBack.CloseFigure();
g.FillPath(&brBack, &bodyBack);



g.FillEllipse(&br, 29.0f, 18.0f, 11.0f, 11.0f);
GraphicsPath bodyFront;
bodyFront.AddArc(24.0f, 30.0f, 24.0f, 20.0f, 180.0f, 180.0f);
bodyFront.AddLine(48.0f, 40.0f, 48.0f, 50.0f);
bodyFront.AddLine(48.0f, 50.0f, 24.0f, 50.0f);
bodyFront.CloseFigure();
g.FillPath(&br, &bodyFront);
}
// [AUDIT-REFACTOR] 原 PaintGlyph 内联 case，原样搬出（零改动）。
void CToolIconUI::PaintGlyph_BootFix(Gdiplus::Graphics& g, Gdiplus::Pen& pen,
                             Gdiplus::Pen& penThin, Gdiplus::SolidBrush& br,
                             Gdiplus::Color c, Gdiplus::Color ac)
{
    using namespace Gdiplus;
    (void)penThin; (void)br; (void)ac;   // 各分支使用的资源不同，统一以 void 抑制未用告警
GraphicsPath shield;
shield.AddLine(13.0f, 15.0f, 51.0f, 15.0f);
shield.AddLine(51.0f, 15.0f, 51.0f, 32.0f);
shield.AddLine(51.0f, 32.0f, 32.0f, 52.0f);
shield.AddLine(32.0f, 52.0f, 13.0f, 32.0f);
shield.CloseFigure();
g.FillPath(&br, &shield);
Pen penAc(ac, 3.6f);
penAc.SetStartCap(LineCapRound);
penAc.SetEndCap(LineCapRound);
penAc.SetLineJoin(LineJoinRound);
g.DrawLine(&penAc, 22.0f, 33.0f, 30.0f, 40.0f);
g.DrawLine(&penAc, 30.0f, 40.0f, 44.0f, 25.0f);
}
// [AUDIT-REFACTOR] 原 PaintGlyph 内联 case，原样搬出（零改动）。
void CToolIconUI::PaintGlyph_ChunkHelper(Gdiplus::Graphics& g, Gdiplus::Pen& pen,
                             Gdiplus::Pen& penThin, Gdiplus::SolidBrush& br,
                             Gdiplus::Color c, Gdiplus::Color ac)
{
    using namespace Gdiplus;
    (void)penThin; (void)br; (void)ac;   // 各分支使用的资源不同，统一以 void 抑制未用告警
Pen penWand(c, 4.0f);
penWand.SetStartCap(LineCapRound);
penWand.SetEndCap(LineCapRound);
g.DrawLine(&penWand, 19.0f, 45.0f, 40.0f, 24.0f);
PointF star1[4] = {
    PointF(45.0f, 9.0f), PointF(48.5f, 17.0f),
    PointF(45.0f, 25.0f), PointF(41.5f, 17.0f)
};
g.FillPolygon(&br, star1, 4);
PointF star2[4] = {
    PointF(37.0f, 17.0f), PointF(45.0f, 20.5f),
    PointF(53.0f, 17.0f), PointF(45.0f, 13.5f)
};
g.FillPolygon(&br, star2, 4);
PointF star3[4] = {
    PointF(18.0f, 15.0f), PointF(20.0f, 19.0f),
    PointF(18.0f, 23.0f), PointF(16.0f, 19.0f)
};
g.FillPolygon(&br, star3, 4);
}
// [AUDIT-REFACTOR] 原 PaintGlyph 内联 case，原样搬出（零改动）。
void CToolIconUI::PaintGlyph_EsdToIso(Gdiplus::Graphics& g, Gdiplus::Pen& pen,
                             Gdiplus::Pen& penThin, Gdiplus::SolidBrush& br,
                             Gdiplus::Color c, Gdiplus::Color ac)
{
    using namespace Gdiplus;
    (void)penThin; (void)br; (void)ac;   // 各分支使用的资源不同，统一以 void 抑制未用告警
// 光盘（上移，为下方导出箭头让位，整体居中）
g.DrawEllipse(&pen, 18.0f, 11.0f, 28.0f, 28.0f);
g.DrawEllipse(&penThin, 27.5f, 20.5f, 9.0f, 9.0f);
g.FillEllipse(&br, 30.6f, 23.6f, 2.8f, 2.8f);
// 导出箭头从盘体下沿伸出，与盘同心，无独立徽章
g.DrawLine(&pen, 32.0f, 36.0f, 32.0f, 47.0f);
g.DrawLine(&pen, 25.5f, 40.5f, 32.0f, 47.0f);
g.DrawLine(&pen, 32.0f, 47.0f, 38.5f, 40.5f);
}
// [AUDIT-REFACTOR] 原 PaintGlyph 内联 case，原样搬出（零改动）。
void CToolIconUI::PaintGlyph_WimToEsd(Gdiplus::Graphics& g, Gdiplus::Pen& pen,
                             Gdiplus::Pen& penThin, Gdiplus::SolidBrush& br,
                             Gdiplus::Color c, Gdiplus::Color ac)
{
    using namespace Gdiplus;
    (void)penThin; (void)br; (void)ac;   // 各分支使用的资源不同，统一以 void 抑制未用告警
GraphicsPath docA, docB;
MakeRoundRect(docA, 9.0f, 17.0f, 22.0f, 30.0f, 3.0f);
MakeRoundRect(docB, 33.0f, 17.0f, 22.0f, 30.0f, 3.0f);
g.DrawPath(&pen, &docA);
g.DrawPath(&pen, &docB);
g.DrawLine(&penThin, 27.0f, 27.0f, 37.0f, 27.0f);
g.DrawLine(&penThin, 34.0f, 24.0f, 37.0f, 27.0f);
g.DrawLine(&penThin, 37.0f, 27.0f, 34.0f, 30.0f);
g.DrawLine(&penThin, 37.0f, 39.0f, 27.0f, 39.0f);
g.DrawLine(&penThin, 30.0f, 36.0f, 27.0f, 39.0f);
g.DrawLine(&penThin, 27.0f, 39.0f, 30.0f, 42.0f);
}
// [AUDIT-REFACTOR] 原 PaintGlyph 内联 case，原样搬出（零改动）。
void CToolIconUI::PaintGlyph_ImageX(Gdiplus::Graphics& g, Gdiplus::Pen& pen,
                             Gdiplus::Pen& penThin, Gdiplus::SolidBrush& br,
                             Gdiplus::Color c, Gdiplus::Color ac)
{
    using namespace Gdiplus;
    (void)penThin; (void)br; (void)ac;   // 各分支使用的资源不同，统一以 void 抑制未用告警
// 三层菱形叠片，由后向前透明度递增，表"多镜像分层"
SolidBrush brL3(Color(c.GetA() * 75 / 255, 255, 255, 255));
SolidBrush brL2(Color(c.GetA() * 150 / 255, 255, 255, 255));
PointF L3[4] = { PointF(32.0f, 36.0f), PointF(50.0f, 45.0f),
                 PointF(32.0f, 54.0f), PointF(14.0f, 45.0f) };
PointF L2[4] = { PointF(32.0f, 24.0f), PointF(50.0f, 33.0f),
                 PointF(32.0f, 42.0f), PointF(14.0f, 33.0f) };
PointF L1[4] = { PointF(32.0f, 12.0f), PointF(50.0f, 21.0f),
                 PointF(32.0f, 30.0f), PointF(14.0f, 21.0f) };
g.FillPolygon(&brL3, L3, 4);
g.FillPolygon(&brL2, L2, 4);
g.FillPolygon(&br,   L1, 4);
}
// [AUDIT-REFACTOR] 原 PaintGlyph 内联 case，原样搬出（零改动）。
void CToolIconUI::PaintGlyph_FileBrowser(Gdiplus::Graphics& g, Gdiplus::Pen& pen,
                             Gdiplus::Pen& penThin, Gdiplus::SolidBrush& br,
                             Gdiplus::Color c, Gdiplus::Color ac)
{
    using namespace Gdiplus;
    (void)penThin; (void)br; (void)ac;   // 各分支使用的资源不同，统一以 void 抑制未用告警
g.FillRectangle(&br, 14.0f, 17.0f, 11.0f, 5.0f);
GraphicsPath folder;
MakeRoundRect(folder, 10.0f, 21.0f, 42.0f, 25.0f, 4.0f);
g.FillPath(&br, &folder);
g.DrawEllipse(&pen, 35.5f, 31.5f, 15.0f, 15.0f);
g.DrawLine(&pen, 46.3f, 42.3f, 52.0f, 48.0f);
}
// [AUDIT-REFACTOR] 原 PaintGlyph 内联 case，原样搬出（零改动）。
void CToolIconUI::PaintGlyph_IsoGen(Gdiplus::Graphics& g, Gdiplus::Pen& pen,
                             Gdiplus::Pen& penThin, Gdiplus::SolidBrush& br,
                             Gdiplus::Color c, Gdiplus::Color ac)
{
    using namespace Gdiplus;
    (void)penThin; (void)br; (void)ac;   // 各分支使用的资源不同，统一以 void 抑制未用告警
// 光盘（整体略向左上，为右下加号留位，视觉仍居中）
g.DrawEllipse(&pen, 13.0f, 13.0f, 30.0f, 30.0f);
g.DrawEllipse(&penThin, 23.5f, 23.5f, 9.0f, 9.0f);
g.FillEllipse(&br, 26.7f, 26.7f, 2.6f, 2.6f);
// 加号叠在光盘右下角，同色一体（Win11"新建/生成"语义）
Pen penPlus(c, 3.4f);
penPlus.SetStartCap(LineCapRound);
penPlus.SetEndCap(LineCapRound);
g.DrawLine(&penPlus, 42.0f, 38.0f, 42.0f, 47.0f);
g.DrawLine(&penPlus, 37.5f, 42.5f, 46.5f, 42.5f);
}
// [AUDIT-REFACTOR] 原 PaintGlyph 内联 case，原样搬出（零改动）。
void CToolIconUI::PaintGlyph_HostsEditor(Gdiplus::Graphics& g, Gdiplus::Pen& pen,
                             Gdiplus::Pen& penThin, Gdiplus::SolidBrush& br,
                             Gdiplus::Color c, Gdiplus::Color ac)
{
    using namespace Gdiplus;
    (void)penThin; (void)br; (void)ac;   // 各分支使用的资源不同，统一以 void 抑制未用告警
// 文档（略向左上收窄，给斜搭铅笔留位）
GraphicsPath doc;
MakeRoundRect(doc, 14.0f, 8.0f, 30.0f, 44.0f, 3.0f);
g.DrawPath(&pen, &doc);
PointF fold[3] = {
    PointF(36.0f, 8.0f), PointF(36.0f, 16.0f),
    PointF(44.0f, 16.0f)
};
Color foldc(c.GetA() * 160 / 255, 255, 255, 255);
SolidBrush brFold(foldc);
g.FillPolygon(&brFold, fold, 3);
g.DrawLine(&penThin, 20.0f, 24.0f, 32.0f, 24.0f);
g.DrawLine(&penThin, 20.0f, 31.0f, 38.0f, 31.0f);
g.DrawLine(&penThin, 20.0f, 38.0f, 32.0f, 38.0f);
// 铅笔斜搭文档右下角，白色一体，无白底徽章
g.DrawLine(&pen, 38.0f, 52.0f, 49.5f, 40.5f);
g.FillEllipse(&br, 36.6f, 50.6f, 3.4f, 3.4f);
}
// [AUDIT-REFACTOR] 原 PaintGlyph 内联 case，原样搬出（零改动）。
void CToolIconUI::PaintGlyph_Activation(Gdiplus::Graphics& g, Gdiplus::Pen& pen,
                             Gdiplus::Pen& penThin, Gdiplus::SolidBrush& br,
                             Gdiplus::Color c, Gdiplus::Color ac)
{
    using namespace Gdiplus;
    (void)penThin; (void)br; (void)ac;   // 各分支使用的资源不同，统一以 void 抑制未用告警
// [U-3] 钥匙整体垂直居中（y=32）；环心由强调色改白，统一单色语言
g.DrawEllipse(&pen, 12.0f, 22.0f, 20.0f, 20.0f);
g.FillEllipse(&br, 18.6f, 28.6f, 6.8f, 6.8f);
// 钥匙杆（向右）+ 两个齿（向下）
Pen penShaft(c, 4.4f);
penShaft.SetStartCap(LineCapRound);
penShaft.SetEndCap(LineCapRound);
g.DrawLine(&penShaft, 31.0f, 32.0f, 50.0f, 32.0f);
g.DrawLine(&penShaft, 42.0f, 32.0f, 42.0f, 39.0f);
g.DrawLine(&penShaft, 50.0f, 32.0f, 50.0f, 37.5f);
}
// 12：Windows 11 镜像预处理（盾牌 + 齿轮，示意镜像定制）
void CToolIconUI::PaintGlyph_Win11Prep(Gdiplus::Graphics& g, Gdiplus::Pen& pen,
                                 Gdiplus::Pen& penThin, Gdiplus::SolidBrush& br,
                                 Gdiplus::Color c, Gdiplus::Color ac)
{
    using namespace Gdiplus;
    (void)penThin; (void)ac;
    GraphicsPath shield;
    shield.AddLine(32.0f, 12.0f, 50.0f, 19.0f);
    shield.AddLine(50.0f, 19.0f, 50.0f, 33.0f);
    shield.AddBezier(50.0f, 33.0f, 50.0f, 43.0f, 42.0f, 50.0f, 32.0f, 54.0f);
    shield.AddBezier(32.0f, 54.0f, 22.0f, 50.0f, 14.0f, 43.0f, 14.0f, 33.0f);
    shield.AddLine(14.0f, 33.0f, 14.0f, 19.0f);
    shield.CloseFigure();
    g.DrawPath(&pen, &shield);
    g.FillEllipse(&br, 25.5f, 25.0f, 13.0f, 13.0f);
}
} // namespace ToolCards
