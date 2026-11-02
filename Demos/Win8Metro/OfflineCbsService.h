#pragma once

// ============================================================
// OfflineCbsService —— R-B13 离线 CBS 深链路
// ------------------------------------------------------------
// 基线要求（Dism复刻审计报告_知识库基线_20261003.md 六项）：
//   1. RegLoadKeyW 蜂巢挂载：SAM/SECURITY/SOFTWARE/COMPONENTS/
//      NTUSER.DAT/BCD/SCHEMA.DAT（离线映像 %MOUNT%\Windows\System32\config\*）
//   2. ntdll 直调：RtlGetVersion / NtQuerySystemInformation 类轻量原语，
//      避免对离线映像依赖已加载系统 ntdll 版本不一致
//   3. KTM 事务删除：对离线 Windows\Servicing\Packages 下 pending 项，
//      以事务方式删除（避免半删残留）
//   4. net stop TrustedInstaller：离线操作前停止 Windows Modules
//      Installer 服务，防止在线组件存储与离线挂载点交叉写入
//   5. OfflineCbs.log：在挂载点 \Windows\Logs\CBS\OfflineCbs.log
//      追加 CBS 离线会话日志（原版 Dism++ 离线链路的留痕行为）
//   6. pending.xml 校验：解析挂载点 \Windows\WinSxS\Temp\PendingDeletes.xml
//      与 \Windows\servicing\Packages 的 pending 状态，校验离线操作前置条件
//
// 本服务不引 UIlib.h，仅依赖 windows.h + advapi32/ntdll/kernel32，
// 可独立编译；所有函数均返回 bool，失败时提供 GetLastError 文本。
// ============================================================

#include <windows.h>
#include <winternl.h>      // SYSTEM_BASIC_INFORMATION / SystemBasicInformation
#include <string>
#include <vector>

namespace OfflineCbs
{

// [G3] 六个无内聚子系统升级为六个具名内部域（hives/ntdll/ktm/svc/log/
// pendingxml），域内符号 100% 落在具名 namespace；跨域编排者 CheckReadiness
// 与综合状态结构 OfflineReadiness 留外层。

// ---------- 1) 蜂巢挂载 ----------
namespace hives
{

// 离线映像挂载点根目录（如 Z:\ 或 C:\Mount\Win10）
// 蜂巢文件按基线规则定位：<mount>\Windows\System32\config\<hive>
// BCD 为 <mount>\Windows\System32\config\BCD-Template（或 Boot\BCD）
// SCHEMA.DAT 为 <mount>\Windows\System32\config\SCHEMA.DAT
struct HiveSpec
{
    const wchar_t* pszName;    // 逻辑名（SAM / SECURITY / SOFTWARE / ...）
    const wchar_t* pszFile;    // 相对 config\ 的文件名
    const wchar_t* pszKey;     // HKEY_LOCAL_MACHINE 下的目标键（如 SAM）
};

// 基线 7 个蜂巢（顺序固定）
const HiveSpec* GetBaselineHives(size_t* pnCount);

// 挂载一个蜂巢。成功返回 true（*pdwErr 记录结果码）。
// [BUG-12] 幂等：目标键已存在时返回 true 且置 ERROR_ALREADY_EXISTS，
// 不重复挂载、不登记到卸载表（该键非本次会话加载，UnmountAllHives 不卸载它）。
bool MountHive(const wchar_t* pszMountRoot, const wchar_t* pszHiveName,
               const wchar_t* pszTargetKey, DWORD* pdwErr = nullptr);

// 卸载一个蜂巢（失败返回 false）。
bool UnmountHive(const wchar_t* pszTargetKey, DWORD* pdwErr = nullptr);

// 一键挂载全部基线蜂巢；返回失败项列表。
// 已成功挂载的键保存在内部集合，UnmountAllHives 统一释放。
bool MountAllHives(const wchar_t* pszMountRoot,
                   std::vector<std::wstring>* pFailed = nullptr);
bool UnmountAllHives();

} // namespace hives

// ---------- 2) ntdll 直调 ----------
namespace ntdll
{

// RtlGetVersion（RtlGetVersion 比 GetVersionEx 更准，且不受 manifest 影响）
// 返回 OSVERSIONINFOEXW。
bool NtGetVersionEx(OSVERSIONINFOEXW* pVer);

// NtQuerySystemInformation（SystemBasicInformation=0 取处理器/页面大小）
// 返回物理内存页数（失败返回 0）。
bool NtQueryBasicInfo(SYSTEM_BASIC_INFORMATION* pInfo);

} // namespace ntdll

// ---------- 3) KTM 事务删除 ----------
namespace ktm
{

// 开启一个事务，删除指定文件，提交。
// 使用 CreateTransaction/CommitTransactionEx（未提交失败则 RollbackTransaction）。
// 删除离线映像内 pending 的 Servicing\Packages 项时，避免"删除一半"残留。
bool TxDeleteFile(const wchar_t* pszPath);

// 批量事务删除；任一失败全部回滚（原子性）。
bool TxDeleteFiles(const std::vector<std::wstring>& vPaths);

} // namespace ktm

// ---------- 4) net stop TrustedInstaller ----------
namespace svc
{

// 停止 Windows Modules Installer（TrustedInstaller）服务。
// 通过 OpenSCManager + OpenService + ControlService(SERVICE_CONTROL_STOP)
// 直调 SCM API（非 spawn net.exe），并等待服务进入 STOPPED。
// 返回是否成功停止；已停止（ERROR_SERVICE_NOT_ACTIVE）视为成功。
bool StopTrustedInstaller(DWORD dwTimeoutMs = 30000, DWORD* pdwErr = nullptr);

// 查询 TrustedInstaller 状态（running / stopped / other）。
enum class EServiceState : int { kSvcUnknown, kSvcRunning, kSvcStopped };
EServiceState QueryTrustedInstaller();

} // namespace svc

// ---------- 5) OfflineCbs.log ----------
namespace log
{

// 向 <mount>\Windows\Logs\CBS\OfflineCbs.log 追加一行。
// 原版行为：每次离线 CBS 会话在挂载点留痕，会话头/尾各写一行时间戳。
bool AppendOfflineCbsLog(const wchar_t* pszMountRoot, const wchar_t* pszLine,
                         DWORD* pdwErr = nullptr);

// 写入会话头（Start）与会话尾（End）。
bool BeginOfflineCbsSession(const wchar_t* pszMountRoot);
bool EndOfflineCbsSession(const wchar_t* pszMountRoot);

} // namespace log

// ---------- 6) pending.xml 校验 ----------
namespace pendingxml
{

// 解析挂载点 WinSxS\Temp\PendingDeletes.xml（离线 pending 项）。
// 输出每项路径与 Delete/Replace 标记；不存在的文件返回 false 但
// pNotFound 记录哪些路径缺失（用于前置条件告警）。
struct PendingDeleteItem
{
    std::wstring sSourcePath;
    std::wstring sDestPath;     // Replace 时的目标
    bool         bIsReplace;    // true=Replace，false=Delete
    DWORD        dwErrorCode;   // 0 = 未缺失
};

// 校验并返回全部 pending 项；缺失项写入 pMissing（路径 + 错误码）。
// 返回是否解析成功（文件存在且可读）；语义校验结果由 missing 体现。
bool CheckPendingDeletes(const wchar_t* pszMountRoot,
                         std::vector<PendingDeleteItem>* pItems,
                         std::vector<PendingDeleteItem>* pMissing,
                         DWORD* pdwErr = nullptr);

} // namespace pendingxml

// ---------- 综合前置校验（跨域编排，留外层）----------

// 综合前置校验：蜂巢可挂 + TI 已停止 + pending 无致命缺失。
struct OfflineReadiness
{
    bool         bHivesOk;          // 基线 7 蜂巢全部可挂
    bool         bTiStopped;        // TrustedInstaller 已停止
    bool         bPendingOk;        // pending.xml 无致命缺失
    bool         bLogDirWritable;   // OfflineCbs.log 目录可写
    std::wstring sMountRoot;
    std::wstring sDetail;           // 人类可读失败摘要
};

bool CheckReadiness(const wchar_t* pszMountRoot, OfflineReadiness* pOut);

} // namespace OfflineCbs
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
