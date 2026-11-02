// ============================================================================
//  ncleaner_vss.h
//
//  NCSRPointOnlineCleanup 的 VSS 原生支持层。
//
//  逆向基线（原版 NCleaner.dll 实测字符串）：
//    vssapi.dll / srclient.dll
//    NCQueryVSSUsedSpace() ==> 0x%08lX
//    NCDeleteVSSAllSnapshots() ==> 0x%08lX
//    NCCreateSystemRestorePoint() ==> 0x%08lX
//    After Cleanup By Dism++
//
//  官方 ReadMe（ReadMe for NCleaner.txt）权威行为补充：
//    * 1.0.0.4 ：「减小系统还原点清理扫描大小误差；并设定大小小于 512MB 不予清理」
//    * 1.0.3.8  ：「添加 dll 抗劫持功能」
//    * 1.0.3.10 ：「降低某些情况下潜在 dll 抗劫风险」
//
//  实现原则（严格对齐原版）：
//    * vssapi.dll / srclient.dll 均为运行期动态加载，不产生静态导入
//      （原版 NCleaner.dll 导入表实测无 vssapi/srclient）；
//    * 抗劫持：一律 LOAD_LIBRARY_SEARCH_SYSTEM32，只从 System32 解析；
//    * 真实调用 VSS COM 接口删除快照，不使用外部命令、不做占位返回。
// ============================================================================

#pragma once

#include "ncleaner_stdafx.h"

namespace NCleanerVss
{

// 查询全部系统还原点（VSS 卷影）当前占用的可回收空间（字节）。
// 语义等价原版 NCQueryVSSUsedSpace。无快照时 *pullUsedBytes = 0。
[[nodiscard]] HRESULT QueryUsedSpace(unsigned long long* pullUsedBytes, std::wstring* pErr = nullptr);

// 强制删除全部系统还原点（VSS 卷影）。
// 语义等价原版 NCDeleteVSSAllSnapshots。*pnDeleted 回传实际删除数量。
[[nodiscard]] HRESULT DeleteAllSnapshots(int* pnDeleted, std::wstring* pErr = nullptr);

// 512MB 最小清理阈值（ReadMe 1.0.0.4 权威行为：小于该值不予清理）。
const unsigned long long kMinCleanupBytes = 512ULL * 1024ULL * 1024ULL;

}  // namespace NCleanerVss
