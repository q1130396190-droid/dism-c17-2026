#include "stdafx.h"
// #include "OfflineSession.h" // [R24-F2] 删除假依赖：0 处使用  // [R10-02] 离线会话经独立组件访问，去 CDataEngine 静态耦合

#include "Page_Cleanup.h"

#include "StringManager.h"
#include "NotifyBox.h"           // [R144] ToolSql::HostNotify / HostConfirm

#include "ProcessRunner.h"  // RunHiddenExit / RunCapture

#include "SafeThread.h"            // 碎片诊断后台线程异常边界

#include "ExternalToolService.h"  // IsElevated
#include "DismApiBridge.h"         // [R-B18] dismapi.dll 直连 DismRemovePackage
// #include "AppxRuntimeBridge.h" // [R25-F1] 删除假依赖：0 处使用  // [P0-A1] Appx 重注册/坏包清理 WinRT 原生

// #include "CleanExtend.h" // [R25-F1] 删除假依赖：0 处使用  // [R80] 深度扫描/残留/空文件夹/撤销引擎
#include "VssNativeService.h"     // [R-B31] VSS 原生 API 删除快照（vssapi.dll COM）

// [R23-F2] 删除假依赖：全文件 0 处使用 CbsHostClient API（注释 [R-拆分] 与实际使用不符；Page_Cleanup.cpp:878 实际是 NCleanerClient::IsReady）
// #include "CbsHostClient.h"
// [R23-F2] 删除假依赖：全文件 0 处使用 DataEngine API（注释 [P0] 声称 IsOfflineSession/GetOfflineImageRoot，实际全库零调用；离线会话已由 OfflineSession.h 提供）
// #include "DataEngine.h"
// [R23-F2] 删除假依赖：全文件 0 处使用 SystemInfoService API（注释 [P0] 声称 WinPE 离线目标，实际全库零调用）
// #include "SystemInfoService.h"

#include "WofCompact.h"           // [P0-A2] CompactOS 原生 FSCTL（替代 compact.exe）
#include "RegCleanEngine.h"       // [P1-A10] SRSetRestorePointW 原生还原点（替代 PS/wmic）

#include <shlobj.h>                // SHGetFolderPath / SHChangeNotify
#include <winevt.h>                // [R-B17] EvtClearLog / EvtOpenChannelEnum

#pragma comment(lib, "wevtapi.lib")

#include <string>

#include <algorithm>              // [R126-1] std::stable_sort

using i18n::CStringManager;   // 便捷：直接写 CStringManager 不用前缀

// ============================================================
// Page_Cleanup_Diag.cpp —— [R14-02] CPageCleanup 清理执行器切片
//   ScCollectCheckedRules/ScRunDismDriver/ScRunDismCompact/
//   ScRunNcSrPoint/ScRunNcEventLog（DISM/原生 API 清理动作）
// 物理搬迁护栏：函数体逐字迁移，宿主引用仍走 m_ctx；类声明在 Page_Cleanup.h。
// ============================================================

// CompactOS 状态查询（[P0-A2] 原生 WOF 抽样，替代 compact.exe /compactos:query
// 本地化文本匹配；返回 0/1/2/-1 口径不变）。
// [R14-02] 随 ScRunDismCompact 迁入本 TU（原 Page_Cleanup.cpp 匿名命名空间）。
static int QueryCompactOSState()
{
    return WofCompact::QueryCompactOsState();
}

// [R154] 单条规则的「文件规则」收集（原内联在 ScCollectCheckedRules 的循环体里）。
// 被关键字过滤/折叠隐藏的行虽然仍处于勾选态，但用户当前看不见，
// 不允许"看不见也照删"，跳过并计数提示。
void CPageCleanup::ScCollectFileRule(int i, CCheckBoxUI* pCheck,
                                     const CleanRule& rule, SCCollect& out)
{
    out.bFileChecked = true;
    if (!pCheck->IsVisible())
    {
        ++out.nHiddenSkipped;
        return;
    }
    if (rule.targets.empty()) return;
    out.sel.push_back(i);
    // [R-CLEANPROG] 记录文件规则的扫描权重，删除线程按权重回报进度，
    // 与扫描页进度条同一分母口径（大目录不再与小目录等宽）。
    if (rule.nWeight > 0) out.nFileWork += rule.nWeight;
    out.vWarnRules.push_back(i);
    if (rule.bAssocNotify) out.bCleanAssocNotify = true;
    if (rule.bRestartExplorer) out.bCleanRestartExplorer = true;
}

// [R154] 单条规则的「自定义动作」分派（原内联在 ScCollectCheckedRules 的循环体里）。
// [AUDIT-P2] 原 30+ 分支 if-else 长链（CC 贡献约 26）已改为单一真源查表：
// CleanJobService::ProcNameToCpi 维护同一份 13 项 ProcName → Cpi 映射。
// 保留在此的只有"DismComponentCleanup 需要读 Flags 判定 ResetBase"这一条
// 语义分支——它是链里唯一有额外副作用的项。
void CPageCleanup::ScCollectCustomProc(int i, const CleanRule& rule, SCCollect& out)
{
    const wchar_t* pp = rule.sSpecialProc.GetData();
    if (!pp || !*pp) return;
    const CustomProcIdx nCpiSel = CleanJobService::ProcNameToCpi(pp);
    if (nCpiSel == CustomProcIdx::CPI_NONE) return;
    CleanJobService::SetCustomProc(nCpiSel, true);
    if (nCpiSel == CustomProcIdx::CPI_DISMCOMP)
    {
        // [R17] Flags="#1" = ResetBase 模式（KB：清理被取代组件后不可再卸载更新）。
        // 无 Flags 的离线分支只做普通组件清理。
        if (rule.sSpecialFlags.Find(L'1') >= 0)
            out.bDismResetBase = true;
    }
    // [E-2] 记录动作→来源规则映射，派发前按实时 Applicable 重评
    // [AUDIT-P2] 复用上面的 nCpiSel，不再重复查表。
    CleanJobService::AddCustomJob(nCpiSel, i);
    for (size_t wj = 0; wj < out.vWarnRules.size(); ++wj)
        if (out.vWarnRules[wj] == i) return;   // 已在文件规则分支加入过
    out.vWarnRules.push_back(i);
}

void CPageCleanup::ScCollectCheckedRules(SCCollect& out)
{
    for (int i = 0; i < m_ctx.CleanEngine().GetRuleCount(); ++i)
    {
        CDuiString sName;
        sName.Format(L"ckclean_%d", i);
        CCheckBoxUI* pCheck = static_cast<CCheckBoxUI*>(m_ctx.HostFindControl(sName.GetData()));
        if (!pCheck || !pCheck->GetCheck()) continue;
        const CleanRule& rule = m_ctx.CleanEngine().GetRule(i);
        // [R17 S-22] 勾选规则带 Activate Restart="Explorer" / AssocChanged 通知时，
        // 清理完成后统一发关联刷新并询问重启资源管理器。Custom 规则勾选即执行，
        // 这里直接收集；文件规则的置位在 ScCollectFileRule 内（可见性判定之后）。
        if (rule.sSpecialProc.GetData() && *rule.sSpecialProc.GetData())
        {
            if (rule.bAssocNotify) out.bCleanAssocNotify = true;
            if (rule.bRestartExplorer) out.bCleanRestartExplorer = true;
        }
        if (rule.bFileRule) ScCollectFileRule(i, pCheck, rule, out);
        ScCollectCustomProc(i, rule, out);
    }
}

void CPageCleanup::ScRunDismDriver()

{
    // NCleaner.dll 原版是"枚举设备存在性 → 只删 orphaned oem*.inf"。
    // 安全过滤：只删【非 Microsoft 发布者】且类为 MEDIA/DISPLAY/NET 的
    // 第三方驱动包；不卸载在用设备（DismRemoveDriver 等价无 /force）。
    // [P1-A6] 原生 DismGetDrivers+DismRemoveDriver，替代 PowerShell
    // 管道 pnputil 文本解析。
    AppendLog(L"[clean] DismDriverCleanup (filtered safe mode, non-Microsoft only)...");
    {
        std::vector<DismApiBridge::DriverInfo> vDrv;
        std::wstring sDrvErr;
        int nDel = 0, nFail = 0;
        if (DismApiBridge::GetDrivers(vDrv, &sDrvErr, nullptr, false))
        {
            for (const auto& d : vDrv)
            {
                // 发布者含 Microsoft → 跳过（大小写不敏感子串匹配）
                bool bMs = false;
                for (size_t k2 = 0; k2 + 9 <= d.sProvider.size(); ++k2)
                {
                    if (_wcsnicmp(d.sProvider.c_str() + k2, L"Microsoft", 9) == 0)
                    { bMs = true; break; }
                }
                if (bMs) continue;
                // 类限定 MEDIA / DISPLAY / NET（不区分大小写）
                const wchar_t* pszCls = d.sClass.c_str();
                if (lstrcmpiW(pszCls, L"MEDIA") != 0
                    && lstrcmpiW(pszCls, L"DISPLAY") != 0
                    && lstrcmpiW(pszCls, L"NET") != 0)
                    continue;
                std::wstring sOneErr;
                bool bOk = DismApiBridge::RemoveDriver(d.sPublished, &sOneErr);
                CDuiString sLog;
                sLog.Format(L"[clean] %s %s (%s) provider=%s",
                            bOk ? L"delete" : L"delete FAILED",
                            CDuiString(d.sPublished.c_str()).GetData(),
                            CDuiString(d.sClass.c_str()).GetData(),
                            CDuiString(d.sProvider.c_str()).GetData());
                AppendLog(sLog.GetData());
                bOk ? ++nDel : ++nFail;
            }
            CDuiString sDone;
            sDone.Format(L"[clean] driver cleanup done: deleted=%d failed=%d",
                         nDel, nFail);
            AppendLog(sDone.GetData());
        }
        else
        {
            CDuiString sRes;
            sRes.Format(L"[clean] driver cleanup enumerate failed: %s",
                        CDuiString(sDrvErr.c_str()).GetData());
            AppendLog(sRes.GetData());
        }
    }
}

// [R195] 从 ScRunDismCompact（76 行 / CC8）抽出：原生 WOF 压缩 + 事后复核。
// 进度回调每约 5000 个文件回报一次（避免日志洪流），阈值与格式逐字保留；
// 压缩后复核 state 必须是 2（optimized），否则记 warning 提示可回滚——
// 这一步是「压缩是否真的生效」的唯一验证点，不可省。
void CPageCleanup::ScRunWofCompact()
{
    // [P0-A2] 业务闭环第 3 步：原生 WOF 逐文件压缩（本函数已在后台
    // 清理线程，直接递归 %SystemRoot%；等价 compact /compactos:optimize）。
    AppendLog(L"[clean] starting CompactOS optimize (native WOF)...");
    {
        struct Ctx { CPageCleanup* pPage; unsigned long long nLast; };
        Ctx ctxProg = { this, 0 };
        auto pfnProgress = +[](const WofCompact::Progress& st, void* p) -> bool
        {
            Ctx* c = (Ctx*)p;
            // 每 ~5000 个文件回报一次，避免日志洪流
            if (st.nScanned - c->nLast >= 5000)
            {
                c->nLast = st.nScanned;
                CDuiString sP;
                sP.Format(L"[clean] CompactOS progress: scanned=%llu compacted=%llu skipped=%llu failed=%llu",
                          st.nScanned, st.nChanged, st.nSkipped, st.nFailed);
                c->pPage->AppendLog(sP.GetData());
            }
            return true;
        };
        bool bOk = WofCompact::OptimizeCompactOs(pfnProgress, &ctxProg);
        CDuiString sRes;
        sRes.Format(L"[clean] CompactOS native optimize result=%d", bOk ? 1 : 0);
        AppendLog(sRes.GetData());
        // 业务闭环第 4 步：压缩后复核状态
        if (bOk)
        {
            int nAfter = QueryCompactOSState();
            CDuiString sCheck;
            sCheck.Format(L"[clean] CompactOS post-run state=%d (target=2=optimized)",
                          nAfter);
            AppendLog(sCheck.GetData());
            if (nAfter != 2)
                AppendLog(L"[clean] warning: CompactOS state did not become Optimized; consider rollback (uncompact)");
        }
    }
}


void CPageCleanup::ScRunDismCompact()

{
    // [P0-A2] 前置条件：WOF 外部支持可用（Win10+ 内核自带；
    // Win7/8.x 尝试加载随包 Config\wofadk.sys）。
    if (!WofCompact::IsSupported())
    {
        AppendLog(L"[clean] CompactOS skipped: WOF external backing not supported");
        return;
    }
    // [R18/S-03] 业务闭环第 1 步：查询当前状态（幂等保护 + 回滚基准）
    int nState = QueryCompactOSState();
    {
        CDuiString sLog;
        sLog.Format(L"[clean] CompactOS current state=%d (0=never,1=mixed,2=optimized,-1=unknown)",
                    nState);
        AppendLog(sLog.GetData());
    }
    if (nState == 2)
    {
        // 已全部压缩：跳过重复执行，避免用户白等 5-10 分钟
        AppendLog(L"[clean] CompactOS already optimized, skipping");
        return;
    }
    if (nState == 1)
    {
        AppendLog(L"[clean] CompactOS partially compressed, running optimize to complete...");
    }
    // [R18/S-03] 业务闭环第 2 步：压缩前创建还原点（回滚能力）
    // [P1-A10] 原生 srclient!SRSetRestorePointW（事件 102/类型 1，
    // 与 NCleaner "After Cleanup By Dism++" 同构），替代 PowerShell
    // Checkpoint-Computer。返回码 1=系统保护未启用，仅日志不阻断。
    {
        int nRp = CRegCleanEngine::CreateRestorePoint(StringMgr.Get("cleanup.restore_point_compactos"));
        CDuiString sRPLog;
        sRPLog.Format(L"[clean] pre-CompactOS restore point result=%d (sr status=%lu)",
                      nRp, CRegCleanEngine::GetLastRestoreStatus());
        AppendLog(sRPLog.GetData());
    }
    // [R195] 压缩 + 复核抽为 ScRunWofCompact（原内联 36 行）
    ScRunWofCompact();
}

void CPageCleanup::ScRunNcSrPoint()

{
    CDuiString sAsk;
    sAsk.Format(StringMgr.Get("toolbox.sff14d10e"));
    if (ToolSql::HostConfirm(m_ctx.HostHwnd(), sAsk.GetData(), StringMgr.Get("misc.s21602703"), MB_YESNO, MB_ICONWARNING) != IDYES)
    {
        AppendLog(L"[clean] NCSRPointOnlineCleanup skipped by user");
        return;
    }
    AppendLog(L"[clean] deleting all system restore points...");
    // [R-B31/P1-收尾] VSS 唯一路径：DeleteAllShadows 走 vssapi.dll COM
    // 通道（IVssBackupComponents::DeleteSnapshots，*，所有卷）。
    // 不再保留 vssadmin.exe 外部命令兜底（零外部进程原则）。
    bool bVssNativeDone = false;
    {
        ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::wstring sVssErr;
        HRESULT hrVss = VssNative::DeleteAllShadows(&sVssErr);
        ::CoUninitialize();
        if (SUCCEEDED(hrVss))
        {
            bVssNativeDone = true;
            CDuiString sVssRes;
            sVssRes.Format(L"[clean] VSS native DeleteAllShadows hr=0x%08X (all shadows removed)", (unsigned)hrVss);
            AppendLog(sVssRes.GetData());
        }
        else
        {
            CDuiString sVssWarn;
            sVssWarn.Format(L"[clean] VSS native DeleteAllShadows hr=0x%08X failed: %s",
                            (unsigned)hrVss, sVssErr.c_str());
            AppendLog(sVssWarn.GetData());
        }
    }
    // [R17 S-05] 删后立即补建一个“清理后”还原点，避免用户在清理后无还原点可用。
    // [P1-A10] 原生 srclient!SRSetRestorePointW（事件 102/类型 1），替代
    // wmic SystemRestore + PowerShell Checkpoint-Computer 双路拐杖。
    // 失败通常是该卷未启用系统保护（返回码 1），仅日志提示不阻断。
    if (bVssNativeDone)
    {
        AppendLog(L"[clean] creating post-cleanup restore point...");
        int nRp = CRegCleanEngine::CreateRestorePoint(
            StringMgr.Get("cleanup.s23dc9b92"));
        CDuiString sLog;
        if (nRp == 0)
        {
            AppendLog(L"[clean] post-cleanup restore point created");
        }
        else
        {
            sLog.Format(L"[clean] restore point failed result=%d sr_status=%lu "
                           "(1=System Protection disabled, 2=access denied)",
                        nRp, CRegCleanEngine::GetLastRestoreStatus());
            AppendLog(sLog.GetData());
        }
    }
}

// [R183] 从 ScRunNcEventLog（67 行 / CC12）抽出：取下一个频道路径。
// 返回：1 取到（sCh 非空）、0 已到末尾、-1 出错（调用方记日志并中止）。
// EvtNextChannelPath 的两段式调用（先问长度再取内容）与错误码判别逐字保留：
// ERROR_INSUFFICIENT_BUFFER 表示"长度不够、继续"，ERROR_NO_MORE_ITEMS 才是结束。
int CPageCleanup::DiagNextChannelPath(EVT_HANDLE hEnum, std::wstring& sCh)
{
    DWORD dwNeeded = 0;
    if (::EvtNextChannelPath(hEnum, 0, nullptr, &dwNeeded))
        return 0;   // 不应发生：0 长度还能成功，视为无更多
    const DWORD e = ::GetLastError();
    if (e != ERROR_INSUFFICIENT_BUFFER || dwNeeded == 0)
        return (e == ERROR_NO_MORE_ITEMS) ? 0 : -1;
    sCh.resize((dwNeeded / sizeof(wchar_t)) + 1);
    DWORD dwGot = 0;
    if (!::EvtNextChannelPath(hEnum, static_cast<DWORD>(sCh.size()), &sCh[0], &dwGot))
    {
        const DWORD e2 = ::GetLastError();
        if (e2 == ERROR_NO_MORE_ITEMS) return 0;
        AppendLog(L"[clean] EvtNextChannelPath failed");
        return -1;
    }
    sCh.resize(wcslen(sCh.c_str()));
    return sCh.empty() ? 0 : 1;
}

// [R183] 从 ScRunNcEventLog 抽出：清空单个频道，成功返回 true。
// ERROR_INVALID_PARAMETER 视为"该频道不支持清空"而**不记日志**：Windows 有若干
// 只读/虚拟频道，逐个报错会把日志刷满，掩盖真正失败的频道。
bool CPageCleanup::DiagClearOneChannel(const std::wstring& sCh)
{
    if (::EvtClearLog(nullptr, sCh.c_str(), nullptr, 0)) return true;
    const DWORD e = ::GetLastError();
    if (e == ERROR_INVALID_PARAMETER) return false;
    CDuiString sW;
    sW.Format(L"[clean] EvtClearLog %s failed, err=%lu", sCh.c_str(), e);
    AppendLog(sW.GetData());
    return false;
}

void CPageCleanup::ScRunNcEventLog()
{
    CDuiString sAsk;
    sAsk.Format(StringMgr.Get("program.se3bff24f"));
    if (ToolSql::HostConfirm(m_ctx.HostHwnd(), sAsk.GetData(), StringMgr.Get("misc.s21602703"), MB_YESNO, MB_ICONWARNING) != IDYES)
    {
        AppendLog(L"[clean] NCWinEventLogOnlineCleanup skipped by user");
        return;
    }
    AppendLog(L"[clean] clearing all event logs...");
    // [R-B17] 原生 wevtapi 直调：EvtOpenChannelEnum 枚举频道 + EvtClearLog 逐个清空，
    // 与 NCleaner.dll EvtClearLog 原语等价；不再走外部命令。
    EVT_HANDLE hEnum = ::EvtOpenChannelEnum(nullptr, 0);
    if (!hEnum)
    {
        CDuiString sRes0;
        sRes0.Format(L"[clean] EvtOpenChannelEnum failed, err=%lu", ::GetLastError());
        AppendLog(sRes0.GetData());
        return;
    }
    DWORD dwCount = 0;
    for (;;)
    {
        std::wstring sCh;
        const int nRc = DiagNextChannelPath(hEnum, sCh);   // [R183] 取路径抽为函数
        if (nRc < 0) { ::EvtClose(hEnum); return; }        // 出错：先关句柄再退出
        if (nRc == 0) break;                               // 已到末尾
        if (DiagClearOneChannel(sCh)) ++dwCount;            // [R183] 清空抽为函数
    }
    ::EvtClose(hEnum);
    CDuiString sRes;
    sRes.Format(L"[clean] event log clear done, channels=%lu", dwCount);
    AppendLog(sRes.GetData());
}
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
