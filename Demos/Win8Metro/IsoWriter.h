// ============================================================================
//  IsoWriter.h
//
//  [2026-10-08 复刻修正] 知识库 40 §2.1 行6/行10、§2.3：
//  原版不调用 oscdimg.exe，ISO 组装在主程序内完成。本模块为内置
//  ISO9660:1999 + Joliet + UDF 2.01 + El Torito（BIOS/UEFI 双启动）
//  映像写入器，纯 Win32 API + 自研文件系统结构，零外部命令依赖。
//
//  特性：
//    * ISO9660:1999（长名 207、深度不限）+ Joliet Level 3（Unicode）
//    * UDF 2.01 桥（支持单文件 > 4GB、Unicode 原名）
//    * El Torito：BIOS（etfsboot.com，platform 0）、UEFI（efisys.bin，
//      platform 0xEF），两者可共存（双启动 ISO，等价 oscdimg -bootdata:2）
//    * 文件数据在 ISO/UDF 间共享同一份 extent，不重复占位
//
//  线程模型：CreateImage 为同步阻塞调用，调用方自行决定线程。
// ============================================================================

#ifndef ISO_WRITER_H
#define ISO_WRITER_H

#include <windows.h>
#include <string>
#include <functional>

namespace IsoWriter
{

struct Options
{
    std::wstring sVolumeLabel;    // 卷标（最长 32；非法字符自动替换）
    std::wstring sBiosBootFile;   // BIOS 引导映像（etfsboot.com）路径，空=无
    std::wstring sUefiBootFile;   // UEFI 引导映像（efisys.bin）路径，空=无
    bool         bJoliet;         // 是否写 Joliet SVD（默认 true）
    bool         bUdf;            // 是否写 UDF 2.01 桥（默认 true）

    Options() : bJoliet(true), bUdf(true) {}
};

// 进度回调：bytesDone / bytesTotal（仅文件数据量；可在任意线程调用，
// 回调内不要做重活）。
using ProgressFn = std::function<void(ULONGLONG bytesDone, ULONGLONG bytesTotal)>;

// [R104] 取消查询回调：返回 true 表示"请求停止"，CreateImage 会立刻收尾并返回 false。
//   为什么不改 ProgressFn 的返回类型：
//     ProgressFn 在 ToolService/IsoImageService.cpp 与 Page_ToolsE2I.h 两处 lambdas 里
//     被用作 std::function<void(...)>，改返回类型要同时改两个调用点与各自的进度窗代码。
//     新增一个可选的独立查询参数，调用方不传就完全保持原行为（默认 nullptr）。
//   为什么检查放在进度回调处：
//     文件数据写入是唯一的逐块长循环（while(remain>0)），进度回调本来就在每块之后，
//     是检查点的天然位置；检查成本是一次函数调用，远小于一次 64KB 写盘。
using CancelQueryFn = std::function<bool()>;

// 以 sSourceRoot 目录树组装 ISO 并写入 sIsoPath。
// 返回 true 成功；失败时 *pErr 为可展示的中文说明。
bool CreateImage(const std::wstring& sSourceRoot,
                 const std::wstring& sIsoPath,
                 const Options& opts,
                 std::wstring* pErr,
                 const ProgressFn& fnProgress = nullptr,
                 const CancelQueryFn& fnCancel = nullptr);

} // namespace IsoWriter

// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：

#endif // ISO_WRITER_H
