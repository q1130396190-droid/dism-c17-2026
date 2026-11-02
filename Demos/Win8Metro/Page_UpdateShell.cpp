// ============================================================
// Page_UpdateShell.cpp —— 更新页「壳消息入口 + 底栏/策略菜单」
// （从 Page_Update.cpp 拆出）。
//
// [R188] Page_Update.cpp 拆前 1,118 行、规模分 9.41（六页唯一 <9.5）。本文件
// 承接四个成员，合计 114 行，边界按「壳层调用进来的入口 + Tab 感知底栏」划分：
//   · OnHostCommand  壳命令入口（仅 HCMD_UPD_SCAN_STARTED 一条）
//   · OnHostTimer    页属定时器分发（过滤防抖 / 分批渲染接力）
//   · UpdateBottomBar Tab 感知底栏显隐（Tab0 更新流按钮 / Tab1 删除）
//   · OnPolicyMenu   三枚策略切换键收纳成的"策略▾"菜单
//
// 闭包说明：四个成员声明见 Page_Update.h。跨 TU 依赖仅五项且都是头文件级：
//   · m_ctx（HostHwnd / HostFindControl）
//   · m_nCurTab（当前 Tab 事实源）
//   · EHostCmd::HCMD_UPD_SCAN_STARTED（FrameWnd_Messages.h）
//   · UPD_FILTER_TIMER_ID / UPD_RENDER_BATCH_TIMER_ID（本页定时器 ID）
//   · ApplyFilter / UpdRenderBatchStep（成员，仍在 Page_Update.cpp）
//
// 逐字搬运，零逻辑改动。
// ============================================================

#include "stdafx.h"

#include "Page_Update.h"

#include "FrameWnd_Messages.h"   // EHostCmd
#include "CustomControls.h"      // CButtonUI / CTextUI

using namespace DuiLib;

// [R188] 一次性定时器命中判定：id 命中则**先 Kill 自己再返回 true**。
// 原 OnHostTimer 两个分支各写一遍 `::KillTimer(hwnd, ID); Action();`，
// 两处必须完全一致 —— 漏 Kill 会让定时器持续重入（过滤框每次重设、
// 渲染接力反复触发同一批）。合并后「命中即自毁」成为单一事实。
// [[nodiscard]] 是刻意的：丢弃返回值等于既不 Kill 也不执行，静默失效。
[[nodiscard]] static bool UpdTakeOneShotTimer(IPageContext& ctx, UINT uId, UINT uTargetId)
{
    if (uId != uTargetId) return false;
    ::KillTimer(ctx.HostHwnd(), uTargetId);
    return true;
}

bool CPageUpdate::OnHostCommand(int id)
{
    if (id == static_cast<int>(EHostCmd::HCMD_UPD_SCAN_STARTED))
    {
        OnScanStarted();
        return true;
    }
    return false;
}

// ============================================================
// [性能改造] 分批渲染接力 / Tab 缓存重渲染 / 过滤防抖 / Tab 感知底栏
// ============================================================

// [性能 C1/B1] 壳 kTimerTab 页属定时器分发
void CPageUpdate::OnHostTimer(UINT uId)
{
    // [R188] 「命中即 Kill 再执行」抽为 UpdTakeOneShotTimer，两分支语义合一
    if (UpdTakeOneShotTimer(m_ctx, uId, UPD_FILTER_TIMER_ID))
    {
        ApplyFilter();
        return;
    }
    if (UpdTakeOneShotTimer(m_ctx, uId, UPD_RENDER_BATCH_TIMER_ID))
        UpdRenderBatchStep();
}

// [性能 P3] Tab 感知底栏：Tab0（待安装）显示更新流按钮，
// Tab1（已安装）显示删除；三枚策略 toggle 恒隐藏（收纳进"策略▾"菜单），
// 控件名与处理函数全部保留，仅切换可见性。
void CPageUpdate::UpdateBottomBar()
{
    const bool bTab0 = (m_nCurTab == 0);
    struct { const wchar_t* name; int bTab0 : 1; int bTab1 : 1; } kBtns[] = {
        { L"btn_upd_find",         1, 1 },
        { L"btn_upd_refresh",      1, 1 },
        { L"btn_upd_selall",       1, 1 },
        { L"btn_upd_delete",       0, 1 },
        { L"btn_upd_predownload",  1, 0 },
        { L"btn_upd_install",      1, 0 },
        { L"btn_upd_finalize",     1, 0 },
        { L"btn_upd_hide",         1, 0 },
        { L"btn_upd_unhide",       1, 0 },
        { L"btn_upd_policy",       1, 1 },
        { L"btn_upd_pausetoggle",  0, 0 },
        { L"btn_upd_skiptoggle",   0, 0 },
        { L"btn_upd_disabletoggle",0, 0 },
    };
    for (int i = 0; i < static_cast<int>(sizeof(kBtns) / sizeof(kBtns[0])); ++i)
    {
        CControlUI* pBtn = m_ctx.HostFindControl(kBtns[i].name);
        if (!pBtn) continue;
        bool bShow = bTab0 ? (kBtns[i].bTab0 != 0) : (kBtns[i].bTab1 != 0);
        pBtn->SetVisible(bShow);
    }
    // 重排底栏，避免隐藏按钮后留白
    CHorizontalLayoutUI* pBar = nullptr;
    CControlUI* pPolicy = m_ctx.HostFindControl(L"btn_upd_policy");
    if (pPolicy && pPolicy->GetParent())
        pBar = static_cast<CHorizontalLayoutUI*>(pPolicy->GetParent());
    if (pBar) pBar->SetPos(pBar->GetPos());
}

// [性能 P3] 策略下拉菜单：收纳 暂停/推迟/禁用 三枚 toggle 的六个动作，
// 菜单项前显示状态点，行为与原 toggle 一致（沿用其确认/日志/刷新逻辑）。
void CPageUpdate::OnPolicyMenu()
{
    enum { IDC_UPD_POLICY_PAUSE = 6221, IDC_UPD_POLICY_SKIP = 6222,
           IDC_UPD_POLICY_DISABLE = 6223 };
    const bool bPaused   = AppxCapUpdService::IsUpdatePaused();
    CDuiString sVer;
    const bool bSkipped  = AppxCapUpdService::IsUpdateSkipped(sVer);
    const bool bDisabled = AppxCapUpdService::IsUpdateDisabled();
    HMENU hMenu = ::CreatePopupMenu();
    ::AppendMenuW(hMenu, MF_STRING, IDC_UPD_POLICY_PAUSE,
                  bPaused   ? L"● 恢复自动更新"  : L"○ 暂停更新（7 天）");
    ::AppendMenuW(hMenu, MF_STRING, IDC_UPD_POLICY_SKIP,
                  bSkipped  ? L"● 恢复功能更新"  : L"○ 推迟功能更新");
    ::AppendMenuW(hMenu, MF_STRING, IDC_UPD_POLICY_DISABLE,
                  bDisabled ? L"● 恢复系统更新"  : L"○ 禁用系统更新");
    POINT pt = { 0, 0 };
    CControlUI* pBtn = m_ctx.HostFindControl(L"btn_upd_policy");
    if (pBtn)
    {
        RECT rc = pBtn->GetPos();
        pt.x = rc.left;
        pt.y = rc.top;
        ::ClientToScreen(m_ctx.HostHwnd(), &pt);
    }
    else
    {
        ::GetCursorPos(&pt);
    }
    int iCmd = ::TrackPopupMenu(hMenu,
                                TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                pt.x, pt.y, 0, m_ctx.HostHwnd(), nullptr);
    ::DestroyMenu(hMenu);
    switch (iCmd)
    {
    case IDC_UPD_POLICY_PAUSE:
        if (bPaused)   OnUnpauseButton(); else OnPauseButton();
        break;
    case IDC_UPD_POLICY_SKIP:
        if (bSkipped)  OnUnskipButton();  else OnSkipButton();
        break;
    case IDC_UPD_POLICY_DISABLE:
        if (bDisabled) OnEnableButton();  else OnDisableButton();
        break;
    default:
        break;
    }
}
