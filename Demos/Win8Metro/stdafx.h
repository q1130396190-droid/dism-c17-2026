#pragma once

#include "targetver.h"

// [NOMINMAX] 必须在 <windows.h> 之前定义：否则 windows.h 会定义 min/max
// 函数式宏，随后 UIlib.h 里的 std::min/std::max 会被展开成语法错误
// （实测 UIPageControl.h:79 报 C2589）。
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <cstdlib>
#include <cstdlib>
#include <memory.h>

// DuiLib 主头（所有类在 DuiLib 命名空间内）
#include "UIlib.h"

// 全项目唯一字符串格式化/拼接封装（C++17 升级落地，F8 禁列清零入口）
#include "FormatW.h"

// [AUDIT-P2] 消息参数与指针互转的唯一入口（含指针宽度 static_assert）。
// 放这里而非各页自行包含：页文件多含"文件|行号"级门禁锚点，逐页加行会让
// 锚点整体下移；stdafx.h 无行级锚点，更不会改变任何页面的行号。
#include "MsgParam.h"

// 引入所有 DuiLib 类型到全局命名空间，和 HiDPITest / duidemo 保持一致
using namespace DuiLib;
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
