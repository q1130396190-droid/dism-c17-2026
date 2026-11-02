#pragma once

// ============================================================
// UiSnapshot.h —— 界面离屏渲染（像素级验收用）
//
// 为什么需要它：
//   本会话是非交互桌面，系统级截屏（CopyFromScreen / PrintWindow）都取不到像素，
//   而"像素级视觉验收"必须有真实渲染结果。DuiLib 的控件树可以画到任意 HDC，
//   所以从程序内部把 root 渲染到 32 位 DIB，再存成 PNG —— 这条路径不依赖桌面合成，
//   在任何会话下都能出图。
//
// 与真实窗口的差异（验收结论里必须说明）：
//   · 不含 DWM 合成层：窗口圆角、标题栏、阴影不会有；
//   · 不含 DuiLib 的滚动条滑轨动画；
//   客户区内容（背景、控件、文字）与真实绘制一致，因为走的是同一个 DoPaint。
// ============================================================

#include <windows.h>

#include <string>

namespace uisnap
{

// 把 root 控件树渲染到 width x height 的位图并写 PNG。
//   pRoot     : 控件树根（通常是 m_pm.GetRoot()）
//   pszPath   : 输出 PNG 路径（UTF-16）
//   width/height : 客户区尺寸
// 返回 true 表示渲染并写盘成功。
//   实现要点：CreateDIBSection 拿到可写像素 + 用 DoPaint 绘制，
//   再交给 GDI+ 编码为 PNG（避免自己写 PNG 编码器）。
[[nodiscard]] bool RenderRootToPng(DuiLib::CControlUI* pRoot, const wchar_t* pszPath,
                                   int width, int height);

// 最近一次渲染的像素统计（作为日志输出用）。
//   为什么用全局而不是返回值：调用点只关心"成功/失败"，
//   统计信息属于诊断数据，没必要改所有调用点的签名。
[[nodiscard]] const wchar_t* LastStatsText();

}   // namespace uisnap
