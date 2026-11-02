#pragma once

// ============================================================
// IsoMount.h —— [R6-04] ISO 直连挂载/卸载/内部映像定位 原语（零 UI）。
//
// 知识库 41 §8：原版不走 IMAPI2，走挂卷内核路径；直连 virtdisk.dll
// AttachVirtualDisk（与原版零外部命令一致，不依赖 PowerShell，精简
// 系统/WinPE 同样可用）。全程 LoadLibrary 动态绑定，不新增链接依赖。
//
// 本 TU 仅依赖 Win32（virtdisk/文件枚举），std::wstring 边界，
// 不 include UIlib.h、不出现任何 C*UI/CDuiString（R7 契约）。
//
// 行为等价基线：ToolDialog.cpp(2026-10-13) 匿名命名空间
// TdLoadVirtdisk/TdCollectCdLetters/TdOpenIsoHandle/TdNativeMount/
// TdNativeDetach（5895-6053）与静态 IsoMountIsoFile/IsoDismountIsoFile/
// IsoResolveInnerImage（6058-6094），逐字搬迁仅换字符串类型。
// ============================================================

#include <string>

namespace toolsvc {

// 挂载 .iso 并返回盘符（形如 "X:\\"）；失败返回空串。
std::wstring IsoMount(const std::wstring& sIso);

// 卸载先前挂载的 .iso（virtdisk DetachVirtualDisk 直连，无外部命令）。
bool IsoDismount(const std::wstring& sIso);

// [R-B80] 在已挂载盘符（形如 "X:\\"）下定位内部安装映像。
// 候选集与 R-B70 双架构 ISO 8 路径一致；命中返回全路径，否则空串。
std::wstring IsoResolveInner(const std::wstring& sMount);

} // namespace toolsvc
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
