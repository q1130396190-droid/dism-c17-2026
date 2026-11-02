// ============================================================================
//  VssNativeService.h
//
//  R-B31 —— VSS 原生 API 实现（清单维度 3『后端引擎』缺失 2：VSS 仍非原生 API）
//
//  背景：
//    基线要求后端引擎维度补全 VSS 原生 API 路径。此前 Page_Cleanup.cpp
//    CPI_NCSR 段只用 `vssadmin.exe delete shadows /all /quiet` 调用外部进程，
//    属“外部命令路线 A”，清单判定为非原生 API。
//
//  本模块：
//    通过 vssapi.dll 公开的 COM 接口直接调用 VSS 服务：
//      CreateVssBackupComponents → InitializeForBackup → SetContext(VSS_CTX_ALL)
//      → GatherWriterMetadata → AbortBackup
//    + CLSID_VssSnapshotMgmt → IVssSnapshotMgmt::QueryVolumesSupportedForSnapshots
//      → 逐卷 IVssSnapshotMgmt::QuerySnapshotsByVolume → 枚举 VSS_OBJECT_SNAPSHOT
//      → IVssBackupComponents::DeleteSnapshots(SnapshotId, VSS_OBJECT_SNAPSHOT,
//        TRUE /*force*/, &nDeleted, nullptr)
//
//  回退策略（对外保证）：
//    VSS COM 路径任一步失败（服务未启动 / 未安装 / RPC 失败）时返回 HRESULT
//    失败码；调用方 Page_Cleanup.cpp 会据此回退 vssadmin.exe，保证清理流程
//    不因 VSS 模块失败而中断。
//
//  链接：
//    vssapi.lib（x86 / x64），COM 基础设施由 ole32.lib 提供。
//  线程模型：
//    纯同步；调用方（清理工作线程）自行决定线程，内部自管 CoInitializeEx /
//    CoUninitialize 生命周期，不要求外部已初始化 COM。
// ============================================================================

#ifndef VSS_NATIVE_SERVICE_H
#define VSS_NATIVE_SERVICE_H

#include <windows.h>
#include <string>
#include <vector>

namespace VssNative
{

// ---------------- 返回结构 ----------------

// 单条快照信息（供日志/审计使用）
struct SnapshotInfo
{
    std::wstring sSnapshotId;        // VSS_ID 格式化后（{XXXXXXXX-...}）
    std::wstring sOriginalVolume;    // 原始卷（如 \\?\Volume{GUID}\）
    std::wstring sDeviceObject;      // 快照设备对象
    std::wstring sCreationTime;      // 快照创建时间（本地时间字符串）
    __int64      nDeleted;           // 是否被本次删除（-1=未知）
};

// 一次删除会话的结果
struct DeleteResult
{
    int               nTotalSnapshots;   // 枚举到的快照总数
    int               nDeleted;          // 实际删除数量
    std::vector<SnapshotInfo> vLog;      // 删除日志（供 UI/审计展示）
};

// ---------------- 主入口 ----------------

// 枚举所有系统保护还原点（VSS 快照）并逐个强制删除。
// 完全走 VSS 原生 COM API，不调用任何外部命令。
//
// 返回：
//   S_OK              —— 枚举+删除全部成功（即使 0 个快照也算成功）
//   E_ACCESSDENIED    —— 未以管理员身份运行（VSS 需要管理员）
//   E_OUTOFMEMORY     —— 内存不足
//   其它 HRESULT      —— VSS 服务/RPC 不可用（调用方应回退 vssadmin.exe）
HRESULT DeleteAllShadows(std::wstring* pErrDetail = nullptr);

// 仅枚举（不删除）所有系统保护还原点，用于“查看”场景。
[[nodiscard]] HRESULT EnumShadows(std::vector<SnapshotInfo>* pOut, std::wstring* pErrDetail = nullptr);

// ---------------- 内部工具（供日志使用） ----------------

// 把 GUID 格式化为 {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX} 大写字符串。
std::wstring FormatGuid(const GUID& g);

// ---------------- 状态标志 ----------------

// 返回本模块是否编译期启用了 VSS 原生路径（恒为 true）。
[[nodiscard]] bool IsNativeEnabled();

} // namespace VssNative

#endif // VSS_NATIVE_SERVICE_H