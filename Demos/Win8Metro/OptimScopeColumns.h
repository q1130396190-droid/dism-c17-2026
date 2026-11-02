// ============================================================
// OptimScopeColumns.h —— 系统优化页「可见作用域列」的唯一真源
//
// 为什么单独成一个头：
//   这张表同时被「页面层」（Page_Optim.cpp：建树/核实/批量/导出/三态）
//   与「通用辅助层」（FrameWnd_UiHelpers.cpp：ParseSwName 解析控件名）
//   使用。若把它放在 Page_Optim.h，底层辅助就要反向依赖页面头
//   （PageBase / CustomControls / CProgressUI 一整串），层次颠倒；
//   若各写一份，就又回到"多处硬编码、删一列漏改一处"的老问题。
//   故抽成这个不含任何 UI 依赖的轻量头。
//
// 列顺序即界面列顺序，与 page_optim.xml 表头逐列对应：
//   0 = 当前用户（Current，HKCU）
//   1 = 系统    （System，HKLM）
// 原第三列 2 = 新用户（Default，HKU\.DEFAULT）已按交互要求取消。
// ============================================================
#pragma once

#include <array>

namespace optim_scope
{
// 可见作用域列。顺序 = 界面列顺序。
inline constexpr std::array<int, 2> kColumns{ 0, 1 };

// 某作用域是否仍是界面可见列。
// 用途：校验来自控件名/配置文件的外部输入，拒绝"看不见的列"被写入。
[[nodiscard]] inline bool IsColumnVisible(int nScope)
{
    for (int c : kColumns) { if (c == nScope) return true; }
    return false;
}
}   // namespace optim_scope

// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
