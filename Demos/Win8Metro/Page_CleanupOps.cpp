// ============================================================
// Page_CleanupOps.cpp —— 清理页「自定义清理动作族」
// （从 Page_Cleanup.cpp 拆出）。
//
// [R140] Page_Cleanup.cpp 原 2242 行（空行规范化后 1663），规模分 6.35
// 为全仓最低。本文件承接 12 路自定义进程派发与其处理器：
//   · RunCustomProcAction —— 派发薄壳（NCleaner 优先 + 12 个 case）
//   · ScRunNcleanerFirst  —— NCleaner.dll 优先路径
//   · ScRun* 十个处理器   —— DISM / NCleaner / 硬链接合并 / 包缓存 / 打印队列
//   · CleanPackageCacheSafe —— Package Cache 安全剪枝（含 VSS 快照取舍）
//
// 随段迁出的三个文件级原生辅助（调用点全部在本文件内，已 grep 确认）：
//   · CleanDirRecursiveW —— SHFileOperationW 递归删除
//   · ScmStopService / ScmStartService —— 原生 SCM 停/启，替代 sc.exe
//
// 跨 TU 共用的 RunOnlineComponentCleanup 已提升到 Page_CleanupInternal.h
// 的 cleanup_detail 命名空间（Page_Cleanup.cpp 的 OcRunDismComponent 也用）。
//
// 搬迁为逐字搬运，零逻辑改动。
// ============================================================

#include "stdafx.h"

#include "Page_Cleanup.h"
#include "Page_CleanupInternal.h"   // cleanup_detail::RunOnlineComponentCleanup

#include "StringManager.h"          // StringMgr
#include "DismApiBridge.h"          // DismApiBridge::AnalyzeComponentStore / StartComponentCleanup
#include "NCleanerClient.h"         // NCleaner 7 导出经 NCleaner.dll 动态调用
#include "CleanNativePrimitives.h"  // [R-B57] CreateHardLink 重复文件回收原语
#include "AppxRuntimeBridge.h"      // [P0-A1] Appx 重注册/坏包清理 WinRT 原生
#include "ExternalToolService.h"    // IsElevated
#include "OfflineSession.h"         // COfflineSession::IsActive / GetImageRoot

#include <shlobj.h>                 // SHGetFolderPath / SHGFP_TYPE_CURRENT
#include <string>
#include <iterator>                  // [R172] std::size（C++17，动作分派表长度）

using i18n::CStringManager;
using cleanup_detail::IsMsiExecuteBusy;
using cleanup_detail::RunOnlineComponentCleanup;

// [C++17 R6] 本 TU 特征常量：SCM 状态轮询的等待上限与单次轮询封顶。
inline constexpr DWORD kCleanSvcDefaultTimeoutMs = 30000;
inline constexpr DWORD kCleanSvcPollCapMs = 1000;
static_assert(kCleanSvcPollCapMs > 0, "poll interval must be positive");

static long CleanDirRecursiveW(const wchar_t* pszDir)
{
    if (!pszDir || !*pszDir) return 0;
    std::wstring sPath(pszDir);
    if (sPath.size() < 2 || sPath[sPath.size() - 1] != L'\\')
        sPath += L'\\';
    std::wstring sBuf = sPath;
    sBuf += L'\0';
    sBuf += L'\0';
    SHFILEOPSTRUCTW so;
    so = {};
    so.wFunc = FO_DELETE;
    so.pFrom = sBuf.c_str();
    so.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    int r = ::SHFileOperationW(&so);
    if (r != 0) return -1;
    long n = (long)so.fAnyOperationsAborted ? 0 : 1;
    return n;
}

// ============================================================
// [P0-A7] 原生 SCM 停/启服务，替代 sc.exe stop/start（零外部进程）
// 返回 ERROR_SUCCESS(0) 表示目标状态已达成；其余为 Win32 错误码。
// 内部按 SERVICE_STATUS.dwWaitHint 轮询到状态翻转或超时。
// ============================================================
[[nodiscard]] static DWORD ScmStopService(const wchar_t* pszName, DWORD dwTimeoutMs = kCleanSvcDefaultTimeoutMs)
{
    SC_HANDLE hScm = ::OpenSCManager(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hScm) return ::GetLastError();
    SC_HANDLE hSvc = ::OpenService(hScm, pszName,
                                   SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!hSvc) { DWORD e = ::GetLastError(); ::CloseServiceHandle(hScm); return e; }
    SERVICE_STATUS ss = { 0 };
    DWORD dwErr = ERROR_SUCCESS;
    if (::ControlService(hSvc, SERVICE_CONTROL_STOP, &ss))
        dwErr = ERROR_SUCCESS;
    else
        dwErr = ::GetLastError();   // ERROR_SERVICE_NOT_ACTIVE 视为已停
    if (dwErr == ERROR_SUCCESS || dwErr == ERROR_SERVICE_NOT_ACTIVE)
    {
        const ULONGLONG ulEnd = ::GetTickCount64() + dwTimeoutMs;
        for (;;)
        {
            if (!::QueryServiceStatus(hSvc, &ss)) { dwErr = ::GetLastError(); break; }
            if (ss.dwCurrentState == SERVICE_STOPPED) { dwErr = ERROR_SUCCESS; break; }
            if (::GetTickCount64() >= ulEnd) { dwErr = ERROR_TIMEOUT; break; }
            DWORD dwWait = ss.dwWaitHint ? ss.dwWaitHint : 500;
            if (dwWait > kCleanSvcPollCapMs) dwWait = kCleanSvcPollCapMs;
            ::Sleep(dwWait);
        }
    }
    ::CloseServiceHandle(hSvc);
    ::CloseServiceHandle(hScm);
    return dwErr;
}

[[nodiscard]] static DWORD ScmStartService(const wchar_t* pszName, DWORD dwTimeoutMs = kCleanSvcDefaultTimeoutMs)
{
    SC_HANDLE hScm = ::OpenSCManager(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hScm) return ::GetLastError();
    SC_HANDLE hSvc = ::OpenService(hScm, pszName,
                                   SERVICE_START | SERVICE_QUERY_STATUS);
    if (!hSvc) { DWORD e = ::GetLastError(); ::CloseServiceHandle(hScm); return e; }
    SERVICE_STATUS ss = { 0 };
    DWORD dwErr = ERROR_SUCCESS;
    if (::QueryServiceStatus(hSvc, &ss) && ss.dwCurrentState == SERVICE_RUNNING)
        dwErr = ERROR_SUCCESS;
    else if (::StartService(hSvc, 0, nullptr))
        dwErr = ERROR_SUCCESS;
    else
        dwErr = ::GetLastError();   // ERROR_SERVICE_ALREADY_RUNNING 视为已运行
    if (dwErr == ERROR_SUCCESS || dwErr == ERROR_SERVICE_ALREADY_RUNNING)
    {
        const ULONGLONG ulEnd = ::GetTickCount64() + dwTimeoutMs;
        for (;;)
        {
            if (!::QueryServiceStatus(hSvc, &ss)) { dwErr = ::GetLastError(); break; }
            if (ss.dwCurrentState == SERVICE_RUNNING) { dwErr = ERROR_SUCCESS; break; }
            if (::GetTickCount64() >= ulEnd) { dwErr = ERROR_TIMEOUT; break; }
            DWORD dwWait = ss.dwWaitHint ? ss.dwWaitHint : 500;
            if (dwWait > kCleanSvcPollCapMs) dwWait = kCleanSvcPollCapMs;
            ::Sleep(dwWait);
        }
    }
    ::CloseServiceHandle(hSvc);
    ::CloseServiceHandle(hScm);
    return dwErr;
}

// ============================================================
// ---- Custom ProcName dispatch ------------------------------------------------
// 12 路自定义进程派发（进程调用仍走 ProcessRunner；UI 日志/MessageBox 留页）
// ============================================================

// [R14-02] RunCustomProcAction 拆解薄壳：NCleaner 优先 + 12 个 case 处理器分发
void CPageCleanup::RunCustomProcAction(CustomProcIdx nIdx)

{
    if (ScRunNcleanerFirst(nIdx))
        return;
    // [R172] 原为 12 个 case 的 switch（CC14）。改为按 CustomProcIdx 枚举值直接
    // 下标取表：枚举是 0..12 的连续整数（CPI_DISMCOMP=0 … CPI_NCSPOOLER=12），
    // 因此下标即枚举值，无需查找。
    // 注意 CPI_DISMCOMP（0）在原 switch 里没有分支，落到 default: break ——
    // 表内该位必须是 nullptr 并据此提前返回，否则会出现"原本什么都不做、
    // 改完却执行了某个清理"的行为变更。
    using Action = void (CPageCleanup::*)();
    static constexpr Action kActions[] = {
        nullptr,                              //  0 CPI_DISMCOMP（原无分支，保持空操作）
        &CPageCleanup::ScRunDismAppxs,        //  1 CPI_DISMAPPXS
        &CPageCleanup::ScRunDismHardLinkMerge,//  2 CPI_DISMHARD
        &CPageCleanup::ScRunDismDriver,       //  3 CPI_DISMDRV
        &CPageCleanup::ScRunDismCompact,      //  4 CPI_DISMCOMPACT
        &CPageCleanup::ScRunNcInstaller,      //  5 CPI_NCINSTALLER
        &CPageCleanup::ScRunNcCorruptAppx,    //  6 CPI_NCCORRUPTAPPX
        &CPageCleanup::ScRunNcSrPoint,        //  7 CPI_NCSR
        &CPageCleanup::ScRunNcDoCache,        //  8 CPI_NCDO
        &CPageCleanup::ScRunNcPkgCache,       //  9 CPI_NCPKG
        &CPageCleanup::ScRunNcVsPkgCache,     // 10 CPI_NCVSPKG
        &CPageCleanup::ScRunNcEventLog,       // 11 CPI_NCLOG
        &CPageCleanup::ScRunNcSpooler,        // 12 CPI_NCSPOOLER
    };
    static_assert(std::size(kActions) == 13, "CustomProcIdx 枚举值与表长必须一一对应");
    const int nSlot = static_cast<int>(nIdx);
    if (nSlot < 0 || nSlot >= static_cast<int>(std::size(kActions))) return;
    const Action fn = kActions[nSlot];
    if (!fn) return;   // 空位 = 原 default: break
    (this->*fn)();
}

// ============================================================
// [FIX-HANG-2] NCleaner 调用超时包装：黑盒 COM 同步调用无法中断，
// 一旦内部挂起（NCVSPackageCacheCleanup 实测事故）即拖死整个收尾。
// 包装：子线程执行 Invoke，调用方限时等待 ——
//   * 完成   → 正常返回 hr；
//   * 超时   → 返回 TIMEOUT 标记（该路按"跳过"处理，不回退原生——
//              挂死的 NCleaner 可能稍后苏醒并发删同一目标，回退有并发
//              风险；泄漏一个挂死工作线程 + 其 COM 会话，进程生命周期
//              内可容忍，远优于整体冻结）。
// 依赖：仅尾巴线程调用（IsBusy 串行），同刻至多一个包装在等待。
// ============================================================
namespace {

constexpr DWORD kNcInvokeTimeoutMs = 120000;   // 单路上限 2 分钟

struct NcAsyncCtx
{
    NCleanerClient::ECleanup e;
    long                     hr;
    std::wstring*            pErr;
    HANDLE                   hDone;
};

DWORD WINAPI NcInvokeThreadProc(void* pv)
{
    // [FIX-HANG-2] 子线程 COM 初始化：原 MTA 守卫在调用线程，Invoke 移入
    // 本子线程后必须自带初始化（NCleaner 内部 CoCreateInstance 依赖）。
    const HRESULT hrCo = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    NcAsyncCtx* c = static_cast<NcAsyncCtx*>(pv);
    c->hr = NCleanerClient::Invoke(c->e, c->pErr);
    if (hrCo == S_OK) ::CoUninitialize();
    ::SetEvent(c->hDone);
    return 0;
}

long NcInvokeWithTimeout(NCleanerClient::ECleanup e, std::wstring* pErr, bool* bTimedOut)
{
    *bTimedOut = false;
    NcAsyncCtx ctx = { e, (long)0x8000FFFF /*E_UNEXPECTED 哨兵*/, pErr, nullptr };
    ctx.hDone = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ctx.hDone)
    {
        // 事件创建失败：退化为直接同步调用（历史行为）
        return NCleanerClient::Invoke(e, pErr);
    }
    // [FIX-HANG-2] 工作线程用裸 CreateThread：本包装自身就是异常边界外的
    // "弃子"模式 —— 超时后工作线程继续跑直到 NCleaner 内部自行返回/挂死
    // 至进程退出，SafeThread 的 SEH 兜底对已挂死场景无意义；且 SafeThread.h
    // 未被本 TU 引入，避免新增依赖。
    HANDLE hWorker = ::CreateThread(nullptr, 0, NcInvokeThreadProc, &ctx, 0, nullptr);
    if (!hWorker)
    {
        ::CloseHandle(ctx.hDone);
        return NCleanerClient::Invoke(e, pErr);
    }
    const DWORD dwWait = ::WaitForSingleObject(ctx.hDone, kNcInvokeTimeoutMs);
    ::CloseHandle(hWorker);      // 句柄即发即弃（超时则工作线程继续跑，泄漏可容忍）
    ::CloseHandle(ctx.hDone);
    if (dwWait == WAIT_OBJECT_0)
        return ctx.hr;
    *bTimedOut = true;
    return (long)0x8000FFFF;
}

} // namespace

// [R14-02] RunCustomProcAction 拆解：NCleaner 优先路径 + 12 个 case 处理器
bool CPageCleanup::ScRunNcleanerFirst(CustomProcIdx nIdx)

{
    if (!NCleanerClient::IsReady())
        return false;
    // [R183] 原 7 个 case 的 switch（CC12）。改为「枚举值 → ECleanup」对照表 + 一次遍历：
    // 表内顺序与原 case 顺序一致，未命中返回 false（与原 default: break + nc<0 等价）。
    // 表放函数内：NCleanerClient::ECleanup 是对外部枚举，取常量值无需类作用域。
    // [FIX-HANG-1] CPI_NCVSPKG 已从表内移除：NCleaner!NCVSPackageCacheCleanup
    // 在实机上内部永久挂起（action_log 09:38:34 止于该函数、数分钟无返回），
    // 黑盒 COM 同步调用既无超时也无法中断 —— 挂死即拖死整个收尾。
    // 该路强制走原生 ScRunNcVsPkgCache（CleanPackageCacheSafe，可控且带
    // MSI 忙预检）。其余 6 路实测毫秒级完成，保留并加 InvokeWithTimeout 防护。
    struct NcRoute { CustomProcIdx idx; NCleanerClient::ECleanup e; };
    static constexpr NcRoute kRoutes[] = {
        { CustomProcIdx::CPI_NCINSTALLER,   NCleanerClient::ECleanup::kCleanInstaller     },
        { CustomProcIdx::CPI_NCCORRUPTAPPX, NCleanerClient::ECleanup::kCleanCorruptedAppx },
        { CustomProcIdx::CPI_NCSR,          NCleanerClient::ECleanup::kCleanSRPoint       },
        { CustomProcIdx::CPI_NCDO,          NCleanerClient::ECleanup::kCleanDOCache       },
        { CustomProcIdx::CPI_NCPKG,         NCleanerClient::ECleanup::kCleanPackageCache  },
        { CustomProcIdx::CPI_NCLOG,         NCleanerClient::ECleanup::kCleanEventLog      },
    };
    int nc = -1;
    for (const NcRoute& r : kRoutes)
    {
        if (r.idx != nIdx) continue;
        nc = static_cast<int>(r.e);
        break;
    }
    if (nc < 0)
        return false;
    // [R-NCSEH] COM 初始化守卫：NCleaner 经 UI 线程路径调用时宿主已初始化
    // COM；若未来新增工作线程调用点而忘记初始化，CoCreateInstance 会失败、
    // 卫星内部解引用空接口 → AV 杀线程（尾巴线程冻结事故同型）。
    // 本守卫在 UI(STA) 线程返回 RPC_E_CHANGED_MODE（已初始化，不释放）；
    // 在 MTA 线程返回 S_FALSE（已初始化，不释放）；仅未初始化线程返回
    // S_OK，此时补初始化并配对释放。
    const HRESULT hrCoSelf = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool bCoSelf = (hrCoSelf == S_OK);
    std::wstring sNcErr;
    bool bTimedOut = false;
    long hr = NcInvokeWithTimeout((NCleanerClient::ECleanup)nc, &sNcErr, &bTimedOut);
    if (bCoSelf) ::CoUninitialize();
    if (bTimedOut)
    {
        // [FIX-HANG-2] 单路超时：按"已处理-跳过"返回（true），不回退原生
        //（挂死的 NCleaner 可能稍后苏醒并发删同一目标）——收尾继续推进，
        // 该路留待下次清理再试。
        CDuiString sTo;
        sTo.Format(L"[clean] NCleaner.dll %s TIMED OUT after %u ms, skipping this path "
                   L"(worker leaked; retry on next cleanup)",
                   NCleanerClient::NameOf((NCleanerClient::ECleanup)nc),
                   (unsigned)kNcInvokeTimeoutMs);
        AppendLog(sTo.GetData());
        return true;
    }
    CDuiString sNc;
    sNc.Format(L"[clean] NCleaner.dll %s => 0x%08lX",
               NCleanerClient::NameOf((NCleanerClient::ECleanup)nc),
               (unsigned long)hr);
    AppendLog(sNc.GetData());
    if (hr == S_OK) return true;   // 与原版语义一致：S_OK 即成功
    // [BUG-03] S_FALSE means the DLL performed a SAFE SKIP because a risky
    // precondition exists (e.g. an MSI install transaction is running). It is
    // not a failure: falling back to the native path (which lacks the same
    // mutex guard) would bypass the protection. Treat it as success.
    if (hr == S_FALSE)
    {
        AppendLog(L"[clean] NCleaner.dll safe-skipped this cleanup (precondition not met, no native fallback)");
        return true;
    }
    AppendLog(L"[clean] NCleaner.dll 返回非 S_OK，回退原生实现");
    return false;
}

void CPageCleanup::ScRunDismAppxs()

{
    // [P0] 离线会话：Get-AppxPackage/Add-AppxPackage 只作用于运行中的
    // PE，重注册目标系统内置应用需在其首次启动时由系统自身完成
    // （ provisioned 包在新用户/首次登录自动注册），故跳过并如实记录，
    // 绝不在 PE 上空跑。
    if (COfflineSession::IsActive())
    {
        AppendLog(L"[clean] Appx re-register skipped in offline WinPE session "
                     "(provisioned apps self-register on first boot of target OS)");
        return;
    }
    AppendLog(L"[clean] starting Appx re-register via WinRT RegisterPackageAsync (this may take several minutes)...");
    int nRegOk = 0, nRegTotal = 0;
    std::wstring sRegErr;
    bool bRegOk = AppxRuntimeBridge::ReRegisterAllPackages(&nRegOk, &nRegTotal, &sRegErr);
    CDuiString sRes;
    if (bRegOk)
        sRes.Format(L"[clean] Appx re-register ok=%d/%d", nRegOk, nRegTotal);
    else
        sRes.Format(L"[clean] Appx re-register failed: %s",
                    sRegErr.empty() ? L"unknown"
                                    : CDuiString(sRegErr.c_str()).GetData());
    AppendLog(sRes.GetData());
}

void CPageCleanup::ScRunDismHardLinkMerge()

{
    // [R17 S-04 修正] 原版此处跑 /StartComponentCleanup /ResetBase 是错配：
    // ResetBase 语义属于 DismComponentCleanup 的 Flags="#1"（永久移除被取代
    // 组件），与“硬链接合并”无关；Windows 也没有等价的公开 API 可直接对
    // WinSxS 做 NTFS 硬链接合并。StartComponentCleanup 内部会重组组件存储
    // 并回收/合并可共享的硬链接，是公开渠道最接近的等效操作，故此处只做
    // 普通组件清理（绝不带 /ResetBase），日志如实说明。
    AppendLog(L"[clean] starting component store cleanup for hard-link consolidation (no ResetBase)...");
    // [P1-收尾] DismApiBridge 原生组件清理（在线/离线同路径），零 dism.exe 回退。
    CDuiString sCbsLog;
    int nCbs = RunOnlineComponentCleanup(false, sCbsLog);
    AppendLog(sCbsLog.GetData());
    if (nCbs != 0)
        AppendLog(L"[clean] component store cleanup failed (native dismapi path only; no external fallback)");
    // [R160] 用户目录硬链接合并抽为 ScMergeProfileHardLinks，本函数只做编排
    ScMergeProfileHardLinks();
}

// [R160] 从 ScRunDismHardLinkMerge 抽出：对当前用户配置目录做硬链接合并。
// [R-B57] 补齐第 5 原语：CreateHardLink 通用重复文件回收。
//   WinSxS 组件库合并不适用于普通用户目录；DismHardLinkMerge 的真正通用路径是
//   NCHardLinkMerge —— 扫描用户目录，把同卷同内容副本合并为硬链接，物理空间被
//   回收（副本目录项与保留文件共享同一文件记录）。这是 R-B38 缺失的
//   CreateHardLink 原语接线点。
//   取不到配置目录（服务账户/受限令牌）时静默跳过：这是"没有可合并范围"，
//   不是失败，不该在用户日志里记一条错误。
void CPageCleanup::ScMergeProfileHardLinks()
{
    wchar_t szProfile[MAX_PATH] = {0};
    if (::SHGetFolderPath(nullptr, CSIDL_PROFILE, nullptr, SHGFP_TYPE_CURRENT,
                          szProfile) != S_OK || !szProfile[0])
        return;
    CDuiString sRoot(szProfile);
    int nMerged = 0, nFailed = 0;
    ULONGLONG ullFreed = 0;
    const bool bOk = CleanNativePrimitives::NCHardLinkMerge(sRoot, true,
                                                           nMerged, ullFreed, nFailed);
    CDuiString sHl;
    if (bOk)
    {
        sHl.Format(L"[clean] HardLinkMerge(user profile) ok merged=%d freed=%I64u bytes failed=%d",
                   nMerged, static_cast<ULONGLONG>(ullFreed), nFailed);
    }
    else
    {
        sHl.Format(L"[clean] HardLinkMerge(user profile) skipped (enumerate failed)");
    }
    AppendLog(sHl.GetData());
}

void CPageCleanup::ScRunNcInstaller()

{
    wchar_t szSys[MAX_PATH] = { 0 };
    ::GetSystemDirectory(szSys, MAX_PATH);
    // [E-3] MSI 安装事务进行中（Global\_MSIExecute 被持有）则跳过，
    // 避免删除 $PatchCache$ 破坏正在进行的安装/回滚。
    if (IsMsiExecuteBusy())
    {
        AppendLog(L"[clean] Installer cleanup skipped: Windows Installer is busy (Global\\_MSIExecute held)");
        return;
    }
    AppendLog(L"[clean] cleaning Windows Installer $PatchCache$...");
    CDuiString sPath; sPath.Format(L"%s\\Installer\\$PatchCache$", szSys);
    // C-6: 仅用动态 szSys（来自 GetSystemDirectory），不再硬编码 C:\Windows
    // [R-B17] 原生 WinAPI 递归删除（SHFileOperationW FO_DELETE），替代 cmd /c rd /s /q
    CleanDirRecursiveW(sPath.GetData());
    AppendLog(L"[clean] Installer PatchCache cleanup done (if any)");
}

void CPageCleanup::ScRunNcCorruptAppx()

{
    // [P0] 离线会话：损坏 AppX 清理依赖在线 SCM/PS 应用模型，离线跳过
    // （目标系统离线映像的包失效状态由其首次启动的 Ti 自检修复）。
    if (COfflineSession::IsActive())
    {
        AppendLog(L"[clean] corrupted AppX cleanup skipped in offline WinPE session");
        return;
    }
    AppendLog(L"[clean] removing corrupted AppX packages (WinRT PackageStatus.VerifyIsOK)...");
    // [P0-A1] 原生 WinRT：枚举当前用户包，IPackage3.Status.VerifyIsOK
    // 返回 FALSE 的包调 RemovePackageAsync 移除。
    // 旧 PowerShell Get-AppxPackage|?Status -ne OK|Remove-AppxPackage
    // 与误用的 DismRemovePackage（CBS 包原语）通道均已删除。
    int nBadTotal = 0;
    int nBadRemoved = 0;
    std::wstring sBadErr;
    bool bBadOk = AppxRuntimeBridge::RemoveBadPackages(&nBadRemoved,
                                                        &nBadTotal, &sBadErr);
    CDuiString sBadRes;
    if (bBadOk)
    {
        sBadRes.Format(L"[clean] corrupted Appx native-removed=%d / bad=%d",
                       nBadRemoved, nBadTotal);
    }
    else
    {
        sBadRes.Format(L"[clean] corrupted Appx cleanup failed: %s",
                       sBadErr.empty()
                           ? L"unknown"
                           : CDuiString(sBadErr.c_str()).GetData());
    }
    AppendLog(sBadRes.GetData());
}

void CPageCleanup::ScRunNcDoCache()

{
    AppendLog(L"[clean] cleaning Delivery Optimization cache...");
    // [修复 #14] 此前执行的是 /Online /Cleanup-Image /AnalyzeComponentStore
    // —— 只"分析"组件存储、不删除任何内容，日志却报告 DOCache 完成（谎报）。
    // 改为真正清理 Delivery Optimization 缓存：直接清空
    // %SystemRoot%\SoftwareDistribution\DeliveryOptimization
    // （DoSvc 占用中的文件删除会自动跳过，不影响服务运行）。
    // [P1-收尾] 删除 PowerShell Delete-DeliveryOptimizationCache
    // 子进程路径；目录原生删除即等价清理，且离线/PE 同样适用。
    CDuiString sDir;
    if (COfflineSession::IsActive())
    {
        sDir.Format(L"%s\\Windows\\SoftwareDistribution\\DeliveryOptimization",
                    COfflineSession::GetImageRoot().GetData());
    }
    else
    {
        wchar_t szWin[MAX_PATH] = { 0 };
        ::GetWindowsDirectory(szWin, MAX_PATH);
        sDir.Format(L"%s\\SoftwareDistribution\\DeliveryOptimization", szWin);
    }
    // [R-B17] 原生 WinAPI 递归删除（SHFileOperationW FO_DELETE），替代 cmd /c rd /s /q
    long nDel = CleanDirRecursiveW(sDir.GetData());
    CDuiString sRes;
    sRes.Format(L"[clean] DOCache cleanup done (cache-dir deleted=%ld)", nDel);
    AppendLog(sRes.GetData());
}

void CPageCleanup::ScRunNcPkgCache()

{
    // [E-3] MSI/安装器事务进行中跳过 Package Cache 剪枝
    if (IsMsiExecuteBusy())
    {
        AppendLog(L"[clean] Package Cache cleanup skipped: Windows Installer is busy (Global\\_MSIExecute held)");
        return;
    }
    // C-1/C-2: 真实执行安全剪枝（动态 ProgramData，非硬编码 C:\ProgramData）
    CleanPackageCacheSafe(false);
}

void CPageCleanup::ScRunNcVsPkgCache()

{
    // [E-3] VS 安装/更新进行中（同样持有 _MSIExecute）跳过
    if (IsMsiExecuteBusy())
    {
        AppendLog(L"[clean] VS Package Cache cleanup skipped: Windows Installer is busy (Global\\_MSIExecute held)");
        return;
    }
    // C-1: 仅处理 Package Cache 中属于 Visual Studio 的子目录
    CleanPackageCacheSafe(true);
}

void CPageCleanup::ScRunNcSpooler()

{
    // [R126-1] 打印队列安全清理：先暂停 Spooler 服务（避免待打印
    // 文件被占用删不掉），清空 spool\PRINTERS，再恢复服务。
    // [P0-A7] 原 sc.exe stop/start 换原生 SCM（ControlService/StartService）。
    AppendLog(L"[clean] safe-cleaning printer spooler queue...");
    DWORD dwStop = ScmStopService(L"Spooler", kCleanSvcDefaultTimeoutMs);
    wchar_t szWin[MAX_PATH] = { 0 };
    ::GetWindowsDirectory(szWin, MAX_PATH);
    CDuiString sDir;
    sDir.Format(L"%s\\System32\\spool\\PRINTERS", szWin);
    // [R-B17] 原生 WinAPI 递归删除（SHFileOperationW FO_DELETE），替代 cmd /c rd /s /q
    long nDel = CleanDirRecursiveW(sDir.GetData());
    DWORD dwRd = (nDel < 0) ? (DWORD)-1 : (nDel > 0 ? 0 : 1);
    DWORD dwStart = ScmStartService(L"Spooler", kCleanSvcDefaultTimeoutMs);
    CDuiString sRes;
    sRes.Format(L"[clean] spooler queue cleanup done (stop=%lu rd=%lu start=%lu)",
                dwStop, dwRd, dwStart);
    AppendLog(sRes.GetData());
}
