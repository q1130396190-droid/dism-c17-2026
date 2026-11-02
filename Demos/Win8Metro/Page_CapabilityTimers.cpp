// ============================================================
// Page_CapabilityTimers.cpp —— 可选功能页「卸载动作 + 自动刷新/重试定时器」。
//
// [R101] 从 Page_Capability.cpp 拆出。选中这一段的三条理由：
//   1. 它不参与 R5（分批渲染）与 R6（缓存过期兜底）判据，
//      拆走不会让隔离守卫看错文件；
//   2. 它只依赖 m_vCaps / m_ctx / StringMgr，与列表构建、渲染无交叉；
//   3. 卸载动作与定时器回调本就是两类职责，
//      留在同一文件里只是为了历史方便。
//
// 拆分前后函数体逐字未改，仅搬迁位置。
// ============================================================

#include "stdafx.h"

#include "Page_Capability.h"

#include "StringManager.h"       // StringMgr 本地化宏

#include <algorithm>

using namespace DuiLib;
using i18n::CStringManager;

// [R165] 从 OnUninstall 抽出：已安装项的卸载确认文案。
//   [R45] 含必需依赖时追加"依赖风险"提示 —— DISM 会阻断，但要先给出可预期信息，
//   否则用户点了确认却看到失败，会以为程序坏了。
//   [R56] 依赖详情：完整展示首条必需/可选依赖 identity，不截断，方便用户
//   拿这个串去核对 DISM 输出（截断后用户无法搜索定位）。
//   拼接顺序与文案键逐字保留。
[[nodiscard]] CDuiString CPageCapability::CapBuildUninstallConfirm(const CapEntry& e)
{
    CDuiString sConfirm;
    sConfirm.Format(StringMgr.Get("capability.s926e07e0"), e.sIdentity.GetData());
    CDuiString sTemp;
    if (e.nRequiredDeps > 0)
    {
        sConfirm += L"\n\n";
        sTemp.Format(StringMgr.Get("misc.hd6f3"), e.nRequiredDeps);
        sConfirm += sTemp;
    }
    if (e.nOptionalDeps > 0)
    {
        sTemp.Format(StringMgr.Get("misc.hf958"), e.nOptionalDeps);
        sConfirm += sTemp;
    }
    if (e.sFirstRequiredDep.IsEmpty() && e.sFirstOptionalDep.IsEmpty())
        return sConfirm;

    sConfirm += StringMgr.Get("misc.hcba9");
    if (!e.sFirstRequiredDep.IsEmpty())
    {
        sTemp.Format(StringMgr.Get("misc.h7543"), e.sFirstRequiredDep.GetData());
        sConfirm += sTemp;
    }
    if (!e.sFirstOptionalDep.IsEmpty())
    {
        sTemp.Format(StringMgr.Get("misc.hb3c1"), e.sFirstOptionalDep.GetData());
        sConfirm += sTemp;
    }
    return sConfirm;
}

void CPageCapability::OnUninstall(int n)
{
    if (n < 0 || n >= static_cast<int>(m_vCaps.size())) return;



    const CapEntry& e = m_vCaps[n];
    CDuiString sIdentity = e.sIdentity;
    bool bInstalled = (lstrcmpiW(e.sState.GetData(), L"Installed") == 0);



    if (bInstalled)
    {
        // 已安装 → 卸载
        // [R165] 确认文案拼接抽为 CapBuildUninstallConfirm（原 5 层嵌套）
        const CDuiString sConfirm = CapBuildUninstallConfirm(e);
        if (!m_ctx.HostConfirmYesNo(StringMgr.Get("misc.se2cfe794"), sConfirm.GetData(), false)) return;



        // [R45] 日志留痕：卸载前记录依赖情况
        CDuiString sTemp;
        sTemp.Format(StringMgr.Get("misc.hd54b"),
                     sIdentity.GetData(), e.nRequiredDeps, e.nOptionalDeps);
        m_ctx.HostLog(sTemp);
        // [R-B21/P1-收尾] Capability 原生直连：DismApiBridge::RemoveCapability
        // （后台线程，在线/离线会话），零 dism.exe 外部命令。
        CapStartDismAsyncShared(m_ctx.HostHwnd(), 0, sIdentity.GetData(), L"", false);
        // [FIX-S1-4] 放弃乐观缓存：旧实现在 DISM 结果未知时就把缓存改成
        // "Not Present"，失败后 20s 自动刷新命中新鲜缓存即短路枚举 →
        // 失败被固化显示为"已卸载"且无任何报错。现只置"待真实刷新"标志
        //（StartEnumeration 强制绕过缓存短路），真实 State 由 DISM 完成
        // 后的刷新回报；DISM 失败由 CapDismThreadProc 经 WM_DIAG_LOG 可见。
        m_bPendingRealRefresh = true;
        // [R58-D] 记录本次卸载动作，供 CAP_REFRESH 触发自动重试判定
        m_sLastActionName = sIdentity;
        m_sLastActionType = L"remove";
        m_bRetryPending = FALSE;
        // [R50] 卸载后 20 秒自动刷新列表，反映最新 State
        ::SetTimer(m_ctx.HostHwnd(), CAP_REFRESH_TIMER_ID, 20000, nullptr);
    }
    else
    {
        // 未安装 → 添加（F-1 + F-1b + F-4 修复）
        CDuiString sConfirm;
        sConfirm.Format(StringMgr.Get("program.s5bf33604"),
                        sIdentity.GetData());
        if (!m_ctx.HostConfirmYesNo(StringMgr.Get("program.sbc8cd318"), sConfirm.GetData(), true)) return;



        // F-4 + [R30]: 优先读本页 cap_source_edit，兜底读程序页 feat_src_edit
        CDuiString sSource = m_ctx.HostReadEditValue(L"cap_source_edit");
        if (sSource.IsEmpty()) sSource = m_ctx.HostReadEditValue(L"feat_src_edit");
        // [R-B21/P1-收尾] Capability 原生直连：DismApiBridge::AddCapability
        // （后台线程，本地源 + 离线会话），零 dism.exe 外部命令。
        CapStartDismAsyncShared(m_ctx.HostHwnd(), 1, sIdentity.GetData(),
                          sSource.GetData(), !sSource.IsEmpty());
        // [FIX-S1-4] 同上：放弃乐观写缓存（"Installed"），改为待真实刷新。
        // 新增能力在缓存里原本不存在，真实枚举后自然出现或给出失败日志。
        m_bPendingRealRefresh = true;
        // [R58-D] 记录本次添加动作（添加通常更慢，允许自动重试兜底）
        m_sLastActionName = sIdentity;
        m_sLastActionType = L"add";
        m_bRetryPending = FALSE;
        // [R50] 添加后 30 秒自动刷新（添加通常比卸载耗时更长）
        ::SetTimer(m_ctx.HostHwnd(), CAP_REFRESH_TIMER_ID, 30000, nullptr);
    }
}

// ============================================================
// 以下两方法承接原 CFrameWnd WM_TIMER 的 CAP_REFRESH_TIMER_ID /
// CAP_RETRY_TIMER_ID 两个分支（函数体零逻辑改动），由核心壳按定时器
// ID 转发；模块静态 s_sLastCap* 与 m_bRetryPending 已落为本页成员。
// ============================================================
void CPageCapability::OnRefreshTimer()
{
    // [R50] 可选功能卸载/添加后自动刷新
    ::KillTimer(m_ctx.HostHwnd(), CAP_REFRESH_TIMER_ID);
    // [INTERACT-FIX R25] 页面隐藏时不做全量重扫：300+ 条 DISM 枚举 + 磁盘
    // 缓存写是重负载后台工作，用户在其它页面操作时照常触发会与切页构建/
    // 其它页枚举争抢 DISM 会话锁与 CPU/IO，是切页卡顿的来源之一。
    // 隐藏期间降频为 10s 轻量轮询（只查一次 GetCurSel），回到本页才真正刷新。
    {
        CTabLayoutUI* pTab = static_cast<CTabLayoutUI*>(
            m_ctx.HostFindControl(L"content_area"));
        if (pTab && pTab->GetCurSel() != static_cast<int>(EPageId::PAGE_FEATURE))
        {
            ::SetTimer(m_ctx.HostHwnd(), CAP_REFRESH_TIMER_ID, 10000, nullptr);
            return;
        }
    }
    // [R57] 自动刷新次数累计：让用户/日志侧了解 CAP 刷新链路触发频次
    // 用于诊断 DISM 枚举是否频繁或是否存在卸载后未生效的重试需求
    {
        ++m_nAutoRefreshCount;
        CDuiString sCnt;
        sCnt.Format(StringMgr.Get("misc.hb5da"),
                    m_nAutoRefreshCount);
        m_ctx.HostLog(sCnt.GetData());
    }
    // [R58-D] 自动重试：上次 CAP 动作后若 60s 内未完成，则静默重放一次
    // 使用独立 retry timer，避免与 refresh 冲突；最多 1 次以避免系统压力
    if (!m_bRetryPending && m_sLastActionName.GetLength() > 0 && m_sLastActionType.GetLength() > 0)
    {
        m_bRetryPending = TRUE;
        CDuiString sRetry;
        sRetry.Format(StringMgr.Get("misc.h5f8c"),
                      m_sLastActionType.GetData(), m_sLastActionName.GetData());
        m_ctx.HostLog(sRetry.GetData());
        ::SetTimer(m_ctx.HostHwnd(), CAP_RETRY_TIMER_ID, 60000, nullptr);
    }
    if (!m_bLoading)
    {
        m_ctx.HostLog(StringMgr.Get("misc.h3fa4"));
        m_vCaps.clear();
        StartEnumeration();
    }
    else
    {
        m_ctx.HostLog(StringMgr.Get("misc.h40b3"));
    }
}
void CPageCapability::OnRetryTimer()
{
    // [R58-D] 可选功能失败自动重试定时器
    ::KillTimer(m_ctx.HostHwnd(), CAP_RETRY_TIMER_ID);
    if (m_bRetryPending && m_sLastActionName.GetLength() > 0)
    {
        m_bRetryPending = FALSE;
        // 静默重放上次动作（原 dism 参数字符串仅作非空闸门，已随原生化移除）
        bool bHaveAction = (m_sLastActionType == L"remove")
                        || (m_sLastActionType == L"add");
        CDuiString sRetryLog;
        sRetryLog.Format(StringMgr.Get("misc.h3593"),
                         m_sLastActionType.GetData(), m_sLastActionName.GetData());
        m_ctx.HostLog(sRetryLog);
        if (bHaveAction)
        {
            // [R-B21] Capability 原生直连：按上次动作类型重放
            // remove → DismApiBridge::RemoveCapability；add → AddCapability
            int nOp = (m_sLastActionType == L"remove") ? 0 : 1;
            CDuiString sSrc = m_ctx.HostReadEditValue(L"cap_source_edit");
            if (sSrc.IsEmpty()) sSrc = m_ctx.HostReadEditValue(L"feat_src_edit");
            CapStartDismAsyncShared(m_ctx.HostHwnd(), nOp,
                              m_sLastActionName.GetData(),
                              sSrc.GetData(), !sSrc.IsEmpty());
            // 重试后再刷一次
            ::SetTimer(m_ctx.HostHwnd(), CAP_REFRESH_TIMER_ID, 30000, nullptr);
        }
    }
}
