#pragma once

// ============================================================
// CleanNativePrimitives —— [R-B38] 清单维度 1 缺失 2「NCleaner 5 个底层清理原语」
// 中「MoveFileEx / CreateHardLink / FindFirstStream / SetFileInformationByHandle」
// 三组全树 0 命中原语的真实落地。
//
// 原语覆盖矩阵（R-B17/R-B18 已闭合的 3 个不再重复）：
//   * EvtClearLog        -> Page_Cleanup.cpp CPI_NCLOG（wevtapi 直调）    [R-B17]
//   * DeleteCache        -> Page_Cleanup.cpp CPI_NCINSTALLER/NCDO/NCSPOOLER
//                           CleanDirRecursiveW（SHFileOperationW FO_DELETE）[R-B17]
//   * DismRemoveAppx     -> AppxDeployService（dismapi.dll 直连）         [R-B18]
//   * RegDeleteTreeW     -> DataEngine / Page_RegClean                    [R-B18]
//   * NCDeleteFile       -> 本模块 NCPendingRebootDelete（MoveFileExW
//                           MOVEFILE_DELAY_UNTIL_REBOOT 待重启删除）     [R-B38]
//   * 硬链接去重         -> CleanEngine::PathSize（GetFileInformationByHandleEx
//                           FileIdInfo，FILE_ID_INFO 集合去重，C-8）      [R-B38 复核]
//   * 强制删除           -> 本模块 NCForceDeleteByDisposition（
//                           SetFileInformationByHandle FileDispositionInfo）[R-B38]
//   * ADS 枚举           -> 本模块 NCQueryAdsSize / NCAnyAdsExist（
//                           FindFirstStreamW FindStreamInfoStandard）      [R-B38]
//
// 设计约束（与项目铁律一致）：
//   * 纯 Win32，不 include UIlib.h，不依赖 DuiLib / StringManager；
//   * 静态链接进主程序，不引入自制 DLL（外部命令路线 A）；
//   * 服务线程只计算/执行，不做 UI / 文案，错误返回布尔值。
//
// 目标编译环境：_WIN32_WINNT=0x0A00（Windows 10），
// FILE_DISPOSITION_INFO / WIN32_FIND_STREAM_DATA 均在 Windows SDK 内。
// ============================================================

#include <windows.h>

#include "Utils/Utils.h"   // DuiLib::CDuiString

using DuiLib::CDuiString;

class CleanNativePrimitives
{
public:
    // NCDeleteFile 语义：把无法立即删除的文件登记为"重启后删除"。
    // MoveFileExW 原语（MOVEFILE_DELAY_UNTIL_REBOOT | MOVEFILE_REPLACE_EXISTING）
    // 会把路径登记到 HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\PendingFileRenameOperations。
    // 返回 true 表示登记成功或文件已不存在；false 表示 API 失败（需回退其它删除路径）。
    static bool NCPendingRebootDelete(const CDuiString& sPath);

    // FILE_DISPOSITION 强制删除：以 DELETE 访问打开文件，置 FileDispositionInfo
    // 的 Delete=TRUE 后关闭句柄，NTFS 立即回收。用于绕过 SHFileOperation
    // 因文件锁/只读/无回收站语义而静默失败的场景。
    // 返回 true 表示删除已执行或路径已不存在；false 表示 API 失败。
    static bool NCForceDeleteByDisposition(const CDuiString& sPath);

    // FindFirstStreamW 枚举指定路径的备选数据流（ADS），返回全部流大小之和。
    // 不含主数据流本身。失败或非 NTFS 返回 0（不阻断）。
    static ULONGLONG NCQueryAdsSize(const CDuiString& sPath);

    // 是否存在任意备选数据流（如 Zone.Identifier / SummaryInformation）。
    // 用于扫描阶段快速判定"该文件附带 ADS"，避免对每个文件做完整枚举。
    static bool NCAnyAdsExist(const CDuiString& sPath);

    // [R-B57] 原语 5：CreateHardLink 通用重复文件回收。
    // 递归扫描 sRootDir 下普通文件，按 (大小 + 内容哈希) 分组，
    // 对每组中除保留文件外的冗余副本执行 ::CreateHardLinkW(副本路径, 保留路径)，
    // 成功后副本目录项与保留文件共享同一文件记录，物理空间被回收。
    // 参数：
    //   * sRootDir      待扫描目录（可含子目录）；
    //   * bRecursive    是否递归子目录；
    //   * nMerged       输出：成功合并的副本数；
    //   * nBytesFreed   输出：合并后释放的字节数（按副本大小累加）；
    //   * nFailed       输出：因权限/跨卷/非 NTFS 等失败而未合并的副本数。
    // 返回 false 仅表示 sRootDir 不可枚举；单文件失败不阻断（记入 nFailed）。
    // 说明：硬链接只能在同卷内创建，跨卷重复文件会失败并计入 nFailed；
    //      该原语不删除保留文件，仅合并目录项，风险远低于 FO_DELETE。
    static bool NCHardLinkMerge(const CDuiString& sRootDir, bool bRecursive,
                                int& nMerged, ULONGLONG& nBytesFreed, int& nFailed);

    // [FIX-DEL-1] 服务感知删除支撑：原生 SCM 停/启（精简自 Page_CleanupOps
    // ScmStopService/ScmStartService，该处为 TU 局部 static 跨 TU 不可见）。
    // DeleteRules 回退链对"被服务独占"的目标（如 wuauserv 持
    // SoftwareDistribution\DataStore）先停服务再强删、完成后恢复。
    // 返回 ERROR_SUCCESS(0) 表示目标状态已达成；其余为 Win32 错误码。
    static DWORD NCStopService(const wchar_t* pszName, DWORD dwTimeoutMs = 10000);
    static DWORD NCStartService(const wchar_t* pszName, DWORD dwTimeoutMs = 10000);
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
