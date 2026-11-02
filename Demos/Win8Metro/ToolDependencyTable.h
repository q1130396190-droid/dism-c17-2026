#pragma once

// [R110] 依赖表已合并：本头改为转发到 Page_ToolsCommon.h 的唯一真源。
#include "Page_ToolsCommon.h"

// ============================================================
// ToolDependencyTable.h —— [R-B25] 工具箱依赖表（单一真相源）
//
// kToolDepends 原先是 Page_Tools.cpp 匿名 namespace 内的静态数组，
// R-B24 右键菜单复用后，依赖图服务（DependencyGraphService）也需要
// 同一份数据。为避免跨文件重复定义，把数组提升到本头（inline 变量，
// C++17），Page_Tools 与 DependencyGraphService 共同 include。
//
// 语义：kind → "该工具正常运行所必须存在的可执行程序"。
//   oscdimg.exe 是 ADK 组件，没装 ADK 时 kind 5/9 自动置灰。
// ============================================================

// [R103] 原写死 12，但 kToolCat 已是 13 项（kind 12 = Windows 11 镜像处理）。
//   两套常量各说各话，ToolDependsCount() 也随之报 12 —— 漏掉 kind 12。
//   改为从本表自身的 switch 覆盖范围推导，避免再手工维护副本。
// [R110] 由 kToolCatCount 推导，不再手写常量（避免再次漂移）
static constexpr int kToolKindCount = kToolCatCount;

// ============================================================
// [R110] 依赖表合并：本函数不再自持一份数据，改为转发到唯一真源。
//
// 合并前状态（R109 实测）：两张表 **13 项里有 10 项不一致**，
//   不是格式差异而是内容分歧：
//     kind 0 系统备份    kToolDepends=""(wimgapi 内置)  ToolDependsForKind="dism.exe"
//     kind 5 ESD转ISO    ""(内置 IsoWriter)            "oscdimg.exe"
//     kind 10 hosts编辑  ""(进程内对话框)               "notepad.exe"
//   ...
//
// 哪张是对的：kToolDepends（Page_ToolsCommon.h）。
//   依据是**实现事实**，不是偏好：
//     · 系统备份/还原走 wimgapi.dll（EWIMHelper），不调 dism.exe
//     · ESD转ISO 走内置 IsoWriter 组装，不用 oscdimg.exe（ADK 组件）
//     · hosts 编辑器是进程内对话框，不起 notepad.exe
//   这些在 Page_ToolsCommon.h:146-160 的注释里逐条写明，
//   而 ToolDependencyTable 的旧数据是更早期的假设，未随实现演进。
//
// 危害（合并前）：本函数被 DependencyGraphService::ToolDependencyAvailable 调用。
//   若有人接上依赖图 UI，kind 5/9 会因为"找不到 oscdimg.exe"被判不可用，
//   而它们实际是内置实现、永远可用 —— 用户会看到功能被无故置灰。
//
// 为什么保留本函数而不是删掉：DependencyGraphService 依赖这个符号。
//   保留转发既满足调用方，又保证只有一处数据，后续改依赖只需改 kToolDepends。
// ============================================================

inline const wchar_t* ToolDependsForKind(int nKind)
{
    if (nKind < 0 || nKind >= kToolCatCount) return nullptr;
    const wchar_t* p = kToolDepends[nKind];
    return p ? p : L"";
}

// [R103] 与 Page_ToolsCommon.h 的 kToolCatCount 必须同值（当前均为 13）。
//   历史上这里写的是 12，而 kToolCat 是 13，两者长期不一致；
//   现在两处都显式写 13，并由下方 static_assert 在 Page_ToolsCommon.h 侧校验长度。
inline int ToolDependsCount() { return kToolKindCount; }
// [R103] 防复发：本表的 kToolKindCount 与 kToolCat 的 kToolCatCount
//   曾经一个写 12、一个写 13，导致 kind 12 在依赖查询里返回 nullptr。
//   这里把两表长度关系钉成编译期断言 —— 任一侧增删工具而忘了同步，
//   编译就会失败，而不是运行时静默返回"不可用"。
//   依赖方向：ToolDependencyTable.h 不 include Page_ToolsCommon.h（后者会引入
//   DuiLib 重依赖），故断言放在 Page_ToolsCommon.h 侧（见该文件末尾）。
//
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
