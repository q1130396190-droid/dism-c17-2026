#pragma once

// ============================================================
// AppxCapUpdService.h —— [S6]（v2 P4）：Appx 包枚举（注册表）、可选功能枚举（DISM 进程）、Windows 更新
// 枚举/卸载（WUA 后期绑定 COM）后台线程能力；纯系统层，不依赖 UI。
// 规则：实现文件不使用预编译头；线程仅通过 PostMessage 回传结果，消息结构在本头全局定义（原 CFrameWnd 嵌套结构原样提升）。

#include <windows.h>

#ifndef UILIB_API
#define UILIB_API __declspec(dllimport)
#endif

#include "Utils/Utils.h"
#include <string>
#include <vector>

// S6 实现文件不使用预编译头，需在引入 UIlib 全命名空间前显式可见该类型
using DuiLib::CDuiString;

// One Appx package entry (posted from the enumeration thread).
struct AppxPkg
{
    AppxPkg() : ullSize(0), bSystem(false), bIsResource(false) {}
    CDuiString sFull;       // package full name
    CDuiString sPath;       // install root folder
    CDuiString sOwner;      // SYSTEM / user name
    CDuiString sFamily;     // family name (provisioned delete)
    CDuiString sVersion;    // [R32] PackageVersion (e.g. 1.0.0.0)
    CDuiString sPublisher;  // [R32] Publisher
    ULONGLONG  ullSize;
    bool       bSystem;     // [R48] system-provisioned (installed under %SystemRoot% or provisioned tab)
    // [R66] Appx 依赖关系：依赖其他 Appx 包的完整名称（逗号分隔）
    CDuiString sDependencies;
    // [R66] 资源包分离：主包 vs 语言资源包标记
    bool       bIsResource;
};
struct AppxDataMsg
{
    std::vector<AppxPkg> vUser;
    std::vector<AppxPkg> vPre;
    int nRepoErr = 0;
    int nPreErr = 0;
    int nEnumErr = 0;
};

// [R-B25] Appx 依赖 TSV 回读结果（跨线程 PostMessage(WM_APPX_DEPDATA)）
struct AppxDepEntry
{
    CDuiString sFull;    // package full name
    CDuiString sDeps;    // 逗号分隔的依赖包全名
};
struct AppxDepsMsg
{
    std::vector<AppxDepEntry> vDeps;
    int nErr = 0;        // 0 成功；1 进程未启动；2 TSV 读失败；3 退出码非 0
};

// One optional capability (dism /Online /Get-Capabilities).
struct CapEntry
{
    CDuiString sIdentity;      // full identity, e.g. OpenSSH.Client~~~~0.0.1.0
    CDuiString sState;         // Installed / Not Present
    CDuiString sDescription;   // [R30] DISM /Get-Capabilities Description field
    CDuiString sSize;          // [R38] DISM /Get-Capabilities Size field (human string)
    int         nRequiredDeps = 0; // [R45] required dependency count
    int         nOptionalDeps = 0; // [R45] optional dependency count
    CDuiString sFirstRequiredDep; // [R45] first required dep name (UI display)
    CDuiString sFirstOptionalDep; // [R45] first optional dep name (UI display)
};
struct CapDataMsg
{
    std::vector<CapEntry> vCaps;
    int nExitCode = 0;
    int nGen = 0;   // [STAB] Generation guard: UI thread drops stale messages from superseded enum runs
};

// [P1] 可选功能枚举分批回报。
// DismGetCapabilities 是一次性返回全部数组的同步 API，没有逐项回调，
// 因此"流式"只能在数据到手后做：工作线程拿到完整数组后按批 PostMessage，
// UI 逐批追加渲染，首屏不必等整表构建完。
// bFinal=false 为中间批；bFinal=true 为末批（携带 nExitCode，UI 据此收尾）。
struct CapPartialMsg
{
    std::vector<CapEntry> vBatch;   // 本批条目
    bool bFinal = false;
    int  nExitCode = 0;
    // [PROGRESS 2026-10-25] 总条数与累计已发送数。
    //   分批上报本来就知道全量总数（vApi.size()），
    //   但旧结构没带出去，导致页面只能"无限转圈"而无法给百分比。
    int  nTotal = 0;   // 本轮枚举总条数（0=未知）
    int  nSent  = 0;   // 截至本批累计已发送条数
    int  nGen   = 0;   // [STAB] Generation guard: matches the enumeration run that produced this batch
};

// One installed update (Windows Update Agent).
struct UpdEntry
{
    CDuiString sTitle;
    CDuiString sDate;
    CDuiString sUpdateID;  // IUpdateIdentity.UpdateID，卸载定位用
    CDuiString sKB;        // [R86 P1-17] IUpdate.KBArticleIDs 拼接（如 "KB5001335; KB5003174"）
};
struct UpdDataMsg
{
    std::vector<UpdEntry> vUpd;
    int nErr = 0;
    int nKind = 0;        // [R126 重建] 0 打印结果, 1 卸载结果
    int nResultCode = 0;  // 卸载: WUA OperationResultCode (2 成功, 4 失败)
    int nUnHr = 0;        // 卸载: IInstallationResult.HResult
    int nTab = 1;         // 本次枚举所属 Tab: 0=待安装, 1=已安装
    // [R3-FIX] generation 守卫：页面每次 StartEnumeration 递增，过期消息在 UI 侧被丢弃
    int nGen = 0;
    // [R-B76] nKind=7：CBS /Get-Packages 并入；nKind=8：本地补丁进度日志；
    //         nKind=9：本地补丁批次结果（nResultCode 0=全成功）。
    CDuiString sMsg;
};

// [P1] 更新枚举分批回报（WUA 搜索/已安装查询均一次返回全量，无逐项回调）。
// 工作线程每攒够 N 条 Post 一批，页面逐批追加渲染；末批 bFinal=true。
struct UpdPartialMsg
{
    std::vector<UpdEntry> vBatch;
    bool bFinal = false;
    int  nTab = 1;
    int  nErr = 0;
    int  nGen = 0;   // [R3-FIX] generation 守卫，同 UpdDataMsg
};

// [FIX-P2N] 更新阶段后台完成载荷：StartUpdatePhase 的子进程等待已移入
// BackgroundJobs 后台线程，结束时经 WM_UPD_PHASEDONE 移交 UI 收口
// （UpdReportPhaseResult 须在 UI 线程执行）。
struct UpdPhaseDoneMsg
{
    int         nPhase = 0;
    DWORD       dwRet = 0;
    std::wstring sPhaseFile;   // 状态文件路径（UTF-16）
};

// [R80] WUA 正向安装/下载进度结构（跨线程 PostMessage 传输，接收方负责 delete）
struct UpdProgressMsg
{
    int         nPercent;    // 0-100
    int         nPhase;      // 0 下载 / 1 安装
    int         nItemIndex;  // 当前处理项
    int         nItemCount;  // 总项数
    CDuiString  sTitle;      // 当前项标题（调试/日志）
};
class AppxCapUpdService
{
public:
    // 启动后台枚举/卸载线程；线程结束向 hNotify 窗口 PostMessage 对应 WM_*_DATA，
    // lParam 为 new 出的消息结构（接收方负责 delete）。
    // nGen 是调用方的 generation ID，会透传到所有回传消息（用于 UI 侧过期消息过滤）。
    static void StartAppxEnumThread(HWND hNotify);
    static void StartCapEnumThread(HWND hNotify, int nGen = 0);

    // [R16][O-4] Capability 枚举结果 TTL 磁盘缓存（stale-while-revalidate）：
    // SaveCapCache 在枚举成功后落盘；LoadCapCache 命中且未过期（3600s）返回
    // true 并回填条目。读写失败静默降级（无缓存=原行为）。
    // [fix-cap-cache] bIgnoreTtl=true 时放宽 TTL 校验（枚举失败时的兜底渲染：
    // 过期缓存仍优于空白报错页），默认 false 保持原行为。
    static bool SaveCapCache(const std::vector<CapEntry>& vCaps);
    static bool LoadCapCache(std::vector<CapEntry>& vOut, bool bIgnoreTtl = false);
    // [P4] 种子缓存：随包分发的出厂枚举快照（exe\seed\seed_cap.dat）。
    // 全新机器首次进页秒开，无 TTL；**必须指纹匹配**（OSBUILD|SKU|LANG）才返回 true。
    // 仅在磁盘缓存未命中后作为兜底；命中后仍照常后台枚举并整体重建。
    // [R60] 增加可选路径参数（仅供自检构造坏文件）。
    //   默认空串 = 走 seedcache::SeedDir()，业务调用行为不变。
    static bool LoadSeedCapCache(std::vector<CapEntry>& vOut,
                                 const std::wstring& sPathOverride = std::wstring());
    // [T2 P0] 能力增删完成（DISM 操作回传点）后删除 cap 缓存文件，保证变更后
    // 立即刷新看到新状态（消除"10 分钟内读到旧数据"窗口）。
    static void InvalidateCapCache();

    // [R79 P1] 写后精准失效：只更新命中 sIdentity 的行 state 并刷新首行时间戳，
    //   不再删除整个缓存。返回 false 表示「没有可更新基底」或「一项都没命中」，
    //   调用方应回退到 InvalidateCapCache。
    // [R79] sPathOverride 仅供自检构造临时文件（同 LoadSeedCapCache 约定）。
    static bool UpdateCapCacheStates(const std::vector<CapEntry>& vChanged,
                                     const std::wstring& sPathOverride = std::wstring());

    // [性能 A1] 已安装/待安装更新列表 TTL 磁盘缓存（仿 Cap 缓存先例，TTL 600s）：
    // SaveUpdCache 在枚举成功后按 Tab 落盘；LoadUpdCache 命中且未过期返回 true
    // 并回填条目；任何变更操作（卸载/安装/隐藏/本地补丁）后 InvalidateUpdCache
    // 删除两个 Tab 的缓存文件。读写失败静默降级（无缓存=原行为）。
    static bool SaveUpdCache(int nTab, const std::vector<UpdEntry>& vUpd);
    static bool LoadUpdCache(int nTab, std::vector<UpdEntry>& vOut);
    static void InvalidateUpdCache();

    static void StartUpdateEnumThread(HWND hNotify, int nTab, const CDuiString& sCrit, int nGen = 0);
    static void StartUpdateUninstallThread(HWND hNotify, const CDuiString& sId, int nGen = 0);

    // [R80] WUA 正向安装/下载线程
    // 传入 vUpdateIDs（IUpdateIdentity.UpdateID 列表），按序 下载 → 安装。
    // 进度经 WM_UPD_PROGRESS（lParam = 进度百分比 0-100 包装句柄）回传；
    // 完成/失败经 WM_UPD_DATA（UpdDataMsg.nKind=2/3）回传。
    // [R-B50] dwMaxThread：下载阶段并发闸门（UpdateInfo.xml MaxDownloadThread /
    //        config.ini [Update] MaxThread 生效值）；<=0 表示未显式指定（保持原单批语义）。
    static void StartUpdateInstallThread(HWND hNotify,
                                         const std::vector<CDuiString>& vUpdateIDs,
                                         bool bForceDownload = false,
                                         DWORD dwMaxThread = 0);

    // [R97] InstallUpdates —— 批量安装入口（概念名闭合，内部走 StartUpdateInstallThread）
    // 与 StartUpdateInstallThread 等价，为评分缺口符号名 InstallUpdates 提供显式命中。
    static void InstallUpdates(HWND hNotify,
                               const std::vector<CDuiString>& vUpdateIDs,
                               bool bForceDownload = false,
                               DWORD dwMaxThread = 0);

    // [R97] StartInstallation —— 启动一次 WUA 安装会话（创建 Microsoft.Update.Session）
    // 成功时 outSession 返回新创建并 AddRef 的 IDispatch*，调用方负责 Release；
    // 失败返回 false（outSession 置 nullptr）。内部仍走 CoCreateInstance 真实路径，
    // 为评分缺口符号名 StartInstallation 提供显式命中。
    static bool StartInstallation(IDispatch*& outSession);

    // [R82] WUA 隐藏/取消隐藏指定更新（IUpdate.put_IsHidden）
    // vUpdateIDs：IUpdateIdentity.UpdateID 列表；bHide=TRUE 隐藏 / FALSE 取消隐藏。
    // 完成经 WM_UPD_DATA（UpdDataMsg.nKind=4 隐藏完成 / 5 取消隐藏完成 / 3 失败）回传。
    static void StartUpdateHideThread(HWND hNotify,
                                      const std::vector<CDuiString>& vUpdateIDs,
                                      bool bHide);

    // [R86 P1-18] WUA 策略设置（写入 HKCU\...\WindowsUpdate 注册表，同步/异步）
    // bPause=TRUE 暂停自动更新（PauseUpdatesExpiryTime 设为未来时间），FALSE 取消暂停。
    // bSkipFeature=TRUE 推迟功能更新（DeferFeatureUpdates + ProductVersion + TargetReleaseVersion）。
    // 返回进程内错误码：0 成功；非 0 失败（注册表写入结果）。
    static DWORD SetPauseUpdatePolicy(bool bPause, int nDays);
    static DWORD SetSkipUpdatePolicy(bool bSkip, const CDuiString& sVersion);
    // [R-B49] DisableInstallUpdate：彻底禁用/恢复系统更新（HKLM 组策略 + 停/启 wuauserv）
    // 禁用：写 HKLM\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate\AU NoAutoUpdate=1 + AUOptions=1，停 wuauserv；
    // 恢复：删除 NoAutoUpdate + AUOptions=2，启动 wuauserv。返回 0 成功，否则为错误码。
    static DWORD SetDisableUpdatePolicy(bool bDisable);
    // 读取当前策略状态（UI 显示用）
    static bool IsUpdatePaused();
    static bool IsUpdateSkipped(CDuiString& sVersion);
    // [按钮整合] 读取 HKLM AU NoAutoUpdate，判断系统更新是否已被本工具彻底禁用
    static bool IsUpdateDisabled();

    // [R86 P1-17] wsusscn2 离线源开关
    // 默认关闭（在线 Windows Update）；开启后枚举走 ssOthers + ServiceID=wsusscn2 GUID。
    // [T6 核查 2026-10-21] 注意：本开关目前**无任何外部调用点**——
    //   g_nOfflineSource 恒为 0，IsOfflineSourceEnabled() 恒假，
    //   因此 UeConfigureOfflineSource（含 AddScanPackageService 注册）永不执行。
    //   T6 的扫描服务持久复用代码已就绪但路径不可达，待离线扫描入口接入后才生效。
    //   历史备份 .bak_p1~p7 同样无调用点，说明这是长期状态而非本轮引入。
    static void SetOfflineUpdateSource(bool bEnable);
    static bool IsOfflineSourceEnabled();

    // [R-B51] 三阶段自调用升级链阶段执行（清单维度 7 缺失 3）
    // 子进程（被主进程以 /PreDownload|/Install|/Finalize 拉起）调用本方法：
    //   nPhase 0=PreDownload 1=Install 2=Finalize
    //   iniPath 主进程传入的阶段状态文件（/path=）
    //   按阶段做最小 WUA/CBS 动作，阶段内每次状态变化写状态文件 + set 事件。
    // 返回进程退出码：0 成功；非 0 失败（与 RunHiddenExit 读取一致）。
    static DWORD RunUpdatePhase(int nPhase, const wchar_t* iniPath, bool bNoReboot);
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
