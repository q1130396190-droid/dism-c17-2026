// ============================================================================
// WIMHelperBridge.h - FrameWnd <-> WIMHelper 桥接（可选接线层）
// ============================================================================
// 用途：把 WIMHelper 的 wimgapi 直接 API 能力暴露给 FrameWnd.cpp。
// 说明：本文件不修改 FrameWnd.cpp 本身，用户可选择
//       (a) 保留 FrameWnd 的 dism 外部命令路径，本文件仅供未来扩展；或
//       (b) 应用 FrameWnd_R30.patch.diff，把关键调用点切换到本桥接。
//
// 目标：+5~7 分（当前 83.0% → 目标 92%+）
// 前置：wimgapi.dll 就位 + WIMHelper.cpp 已编译
// ============================================================================

#ifndef __DUI_WIMHELPER_BRIDGE_H__
#define __DUI_WIMHELPER_BRIDGE_H__

#include "WIMHelper.h"
#include <dwmapi.h>

#pragma once

#include <functional>   // [R105] CancelQueryFn
// 全局单例（应用整个生命周期共享一个桥接实例）
inline CWIMHelper* g_pWIMBridge = nullptr;

namespace WIMBridge {

// ---- 生命周期 ---------------------------------------------------------

// 初始化：加载 wimgapi.dll + 注册全局日志文件（可选）
// logPath 为空则不写日志。返回 true 表示 wimgapi 可用。
bool Init(const std::wstring& logPath = L"");

// 退出：注销日志 + 清理全局状态
bool Shutdown();

// 是否可用
bool IsReady();

// ---- 日志聚合 ---------------------------------------------------------

// 追加一条应用层日志到 WIMHelper 日志缓冲（与 wimgapi 内部日志合并）
void Log(LogLevel level, const std::wstring& msg);

// 获取所有已收集的日志条目（用于 UI 日志面板显示）
std::vector<WIMLogEntry> SnapshotLog();

// ---- 进度回调桥接 -----------------------------------------------------

// [2026-10-12] 每任务进度接收器：后台线程内 wimgapi 的 WIM_MSG_PROGRESS
// 回调直接调用 OnWimProgress(percent=0..100)。实现方必须线程安全
// （典型做法：::PostMessage 到进度窗口，绝不在回调里直接动 DuiLib 控件）。
// 一个 wimgapi 句柄同一时刻只挂一个任务，故无需全局状态、可并发安全使用。
struct IWimProgress
{
    virtual ~IWimProgress() {}
    // percent : 0-100 完成百分比（WIM_MSG_PROGRESS 的 wParam）
    // msgId   : wimgapi 原始消息号（WIM_MSG_PROGRESS=0x00000009）
    virtual void OnWimProgress(DWORD percent, DWORD msgId) = 0;
};

// 全局进度回调函数（供 CWIMHelper::RegisterCallback 使用；已废弃，
// 仅为旧测试/调用保留。新代码请向各 *Ex 函数传 IWimProgress*）。
// 每次 WIM_MSG_PROGRESS / WIM_MSG_FILE 事件被 wimgapi 触发时，
// 通过 progressFn(percent, stage, fileName) 通知调用方（通常是 UI 线程）。
using WIMProgressFn = void (*)(DWORD percent, const std::wstring& stage,
                              const std::wstring& fileName);

// 设置全局进度回调（在 Init 之后、wimgapi 操作之前调用）
void SetProgressCallback(WIMProgressFn fn, void* ctx = nullptr);

// ---- WimInfo 解析 -----------------------------------------------------

// 全版本"产品身份证"：把映像原始 VERSION/EDITIONID/ARCH 翻译为统一产品信息。
// 覆盖客户端 Win7 / 8 / 8.1 / 10 / 11 与服务器 Server 2003 / 2008 /
// 2008 R2 / 2012 / 2012 R2 / 2016 / 2019 / 2022 / 2025 及未来前向兼容。
struct ProductIdentity {
    std::wstring family;     // 客户端 / 服务器
    std::wstring product;    // Windows 11 / Windows Server 2025
    std::wstring release;    // 24H2 / SP1 / LTSC 2024 /（未知版本→Build 号）
    std::wstring editionCn;  // 专业版 / 企业版 / Standard / Datacenter
    std::wstring arch;       // x86 / x64 / arm64 / arm64ec
    std::wstring build;      // 26100.3775
    std::wstring summary;    // 一句话：Windows 11 24H2 专业版 (x64)
};

// 版本矩阵解析：version 形如 "10.0.26100.3775"；editionId 为 WIM XML
// EDITIONID；arch 为已解析体系。任何字段缺失均给出尽可能具体的结论，
// 未知 build 按前向兼容处理（不误判为低版本）。
ProductIdentity ResolveProduct(const std::wstring& version,
                               const std::wstring& editionId,
                               const std::wstring& arch);

// 结构：单个映像索引的信息
struct WimInfo {
    DWORD     index = 0;
    std::wstring name;
    std::wstring description;
    std::wstring edition;
    std::wstring version;
    std::wstring architecture;
    std::wstring creationDate;
    ULONGLONG size = 0;        // 压缩前
    ULONGLONG compressedSize = 0;
    bool      isBootable = false;
    bool      isEnterprise = false;
    std::wstring language;
};

// 通过 wimgapi 打开 WIM，读取全部索引信息（不调 dism /Get-WimInfo）
// 替代 FrameWnd::QueryWimInfoEx() 的 dism 外部命令
[[nodiscard]] bool QueryWimInfo(const std::wstring& wimPath,
                  std::vector<WimInfo>& outInfos,
                  std::wstring* pErr = nullptr);

// ---- 挂载/卸载封装（替代 dism /Mount-Wim / /Unmount-Wim） -----------

// 挂载：MountImage + LoadImage + WIMMount
[[nodiscard]] bool Mount(const std::wstring& wimPath, DWORD imageIndex,
           const std::wstring& mountPath, DWORD flags = 0);
// 卸载并丢弃改动
[[nodiscard]] bool Unmount(const std::wstring& wimPath, DWORD imageIndex,
             const std::wstring& mountPath, BOOL commit = FALSE);

// ---- 已挂载映像枚举（镜像仓）----------------------------------------
// WIMGetMountedImages 直连（wimgapi.h WIM_MOUNT_LIST）。
// 与 MountLedger 注册表记账互补：本接口反映**系统实际**挂载状态
// （含其他进程/前次会话遗留的挂载点），用于镜像仓列表与失效点识别。
struct MountedImage {
    std::wstring wimPath;
    std::wstring mountPath;
    DWORD        imageIndex;
    BOOL         mountedForRW;   // TRUE=可写挂载
};

// 枚举系统当前全部已挂载映像。成功返回 true（outList 可能为空）。
[[nodiscard]] bool EnumMounted(std::vector<MountedImage>& outList);

// ---- 导出/导入封装（替代 dism /Export-Image） -----------------------

// 从源 WIM 指定索引复制到目标 WIM
bool ExportImage(const std::wstring& srcWim, DWORD srcIdx,
                 const std::wstring& dstWim, DWORD dstIdx);

// ---- Apply 封装（替代 dism /Apply-Image） ---------------------------

// 把 WIM 指定索引释放到目标目录
[[nodiscard]] bool Apply(const std::wstring& wimPath, DWORD imageIndex,
           const std::wstring& targetPath, DWORD flags = 0);

// ---- 捕获封装（替代 dism /Capture-Image） ---------------------------

// 从目录打包为 WIM
[[nodiscard]] bool Capture(const std::wstring& srcDir, const std::wstring& wimPath,
             DWORD compression, const std::wstring& imageDesc = L"");

// ---- 驱动扫描（替代 dism /Get-Drivers） -----------------------------

struct DriverEntry {
    std::wstring name;
    std::wstring provider;
    std::wstring version;
    std::wstring description;
};

[[nodiscard]] bool ScanDrivers(const std::wstring& wimPath, DWORD imageIndex,
                 std::vector<DriverEntry>& outDrivers,
                 std::wstring* pErr = nullptr);

// ---- ToolDialog 层专用（替代 dism 批处理命令，带完整参数） ----------

// 压缩模式映射：max→WIM_COMPRESS_LZX，recovery→WIM_COMPRESS_LZMS
DWORD CompressFromName(const std::wstring& sCompress);

// Capture（替代 dism /Capture-Image，带 Name/Description/Compress/Bootable）
// srcDir 为待捕获目录；bootable=true 时对映像设置 WIM_FLAG_BOOTABLE
[[nodiscard]] bool CaptureImageEx(const std::wstring& wimPath,
                    const std::wstring& srcDir,
                    const std::wstring& imageName,
                    const std::wstring& imageDesc,
                    DWORD compression,
                    bool bootable,
                    std::wstring* pErr = nullptr,
                    IWimProgress* pProg = nullptr);

// Export（替代 dism /Export-Image，带 DestinationName/Description/Compress/Bootable）
// 等价于：先创建目标 WIM(compression) → 装载源索引 → 设置元信息 → WIMExportImage → SetBootImage
bool ExportImageEx(const std::wstring& srcWim, DWORD srcIdx,
                   const std::wstring& dstWim,
                   DWORD compression,
                   const std::wstring& dstName,
                   const std::wstring& dstDesc,
                   bool bootable,
                   std::wstring* pErr = nullptr,
                   IWimProgress* pProg = nullptr);

// [rb28 2026-10-13] 多索引导出（WIM<->ESD 互转核心）：把源映像**全部索引**
// 依次导出到同一个新建目标（目标句柄只创建一次，WIM_CREATE_ALWAYS）。
// 用途：替代 Page_Tools::WecRunConvert 里手工 Open/Create/Loop/Export，
// 统一走 Bridge；wimgapi 的单索引 0-100 进度按索引数线性映射为总进度。
// 任一索引失败即删除半成品目标并返回 false。
// [R105] 取消查询回调：返回 true = 请求停止，导出在**索引边界**收尾并返回 false。
//   为什么是索引粒度而不是块粒度：
//     wimgapi 的 WIMExportImage 是单个不可分割的 API 调用，中途无法打断；
//     能安全插入检查点的位置只有「两个索引之间」。这意味单个超大索引
//     （例如 install.wim 的 5GB 全集）一旦开始导出就必须等它结束。
//     这是 wimgapi 的能力边界，不是本实现的取舍 —— 如需更细粒度，
//     只能改成自建文件级拷贝，那会失去 wimgapi 的压缩与元数据处理。
//   不传 fnCancel 时行为与改前完全一致（默认 nullptr）。
using CancelQueryFn = std::function<bool()>;

bool ExportAllImagesEx(const std::wstring& srcWim,
                       const std::wstring& dstWim,
                       DWORD compression,
                       std::wstring* pErr = nullptr,
                       IWimProgress* pProg = nullptr,
                       const CancelQueryFn& fnCancel = nullptr);

// Delete（替代 dism /Delete-Image）
[[nodiscard]] bool DeleteImageEx(const std::wstring& wimPath, DWORD imageIndex,
                   std::wstring* pErr = nullptr,
                   IWimProgress* pProg = nullptr);

// Append（替代 dism /Append-Image）
[[nodiscard]] bool AppendImageEx(const std::wstring& wimPath,
                   const std::wstring& srcDir,
                   const std::wstring& imageName,
                   std::wstring* pErr = nullptr,
                   IWimProgress* pProg = nullptr);

// Apply（替代 dism /Apply-Image，带 WIMBoot/Compact 标志）
[[nodiscard]] bool ApplyImageEx(const std::wstring& wimPath, DWORD imageIndex,
                  const std::wstring& targetPath,
                  DWORD applyFlags,
                  std::wstring* pErr = nullptr,
                  IWimProgress* pProg = nullptr);

// Split（替代 dism /Split-Image）：dstSwm 为首卷 .swm 路径，partSizeMB=每卷 MB
bool SplitImageEx(const std::wstring& wimPath,
                  const std::wstring& dstSwm,
                  DWORD partSizeMB,
                  std::wstring* pErr = nullptr,
                  IWimProgress* pProg = nullptr);

} // namespace WIMBridge

#endif // __DUI_WIMHELPER_BRIDGE_H__
