// ============================================================================
//  Page_DriverProbe.cpp —— 驱动页头文件链自检（R69 建立）
//
//  由来：R66/R68 两次拆分 Page_Driver.cpp 均报 218 个错误，
//        排查后确认根因是**新建 .cpp 缺少 UTF-8 BOM**——
//        中文注释在无 BOM 时被按 GBK 解析，吞掉换行，
//        编译器于是认为函数头缺失，连锁报出
//        "RECT 重定义 / 缺少函数标题"等 218 条错误。
//
//  本文件保留为**回归探针**，不是临时文件：
//    它是"新源文件的 PCH + 头文件链是否装配正确"的最小可编译样例。
//    今后再拆任何页面文件时，先复制本文件的头部（BOM + stdafx.h + 目标页头）
//    再写函数体，可避免重复踩同一坑。
//
//  约定（必须遵守）：
//    · 文件必须以 UTF-8 BOM 开头（本文件已带）；
//    · 第一行必须是 #include "stdafx.h"；
//    · 之后按 **被拆源文件的原始 include 顺序** 逐字复制，不要自拟精简版。
//
//  当前状态：编译通过、链接通过（0 error / 0 warning）。
// ============================================================================

#include "stdafx.h"

#include "Page_Driver.h"

// 编译期自检：证明 Page_Driver.h → DriverService.h 的类型链可见。
// 不引入运行期行为，仅供本 TU 编译时验证头文件装配。
namespace
{
struct DrvProbeCompileCheck
{
    // DriverRowInfo 来自 DriverService.h（经 Page_Driver.h 传递）
    DriverRowInfo row{};
    // CPageDriver 类声明可见（不实例化，避免构造依赖）
    CPageDriver* page = nullptr;
};
}   // namespace
