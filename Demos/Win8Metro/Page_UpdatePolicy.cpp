// ============================================================
// Page_UpdatePolicy.cpp —— 更新页「三组策略开关」（从 Page_Update.cpp 拆出）。
//
// [R168] Page_Update.cpp 拆前 1,429 行、规模分 8.59；本文件承接八枚策略按钮
// 与一枚状态同步函数，合计 228 行，边界自成体系：
//   · 隐藏 / 取消隐藏   —— WUA HideUpdate，经 AppxCapUpdService 隐线程执行
//   · 暂停 / 恢复更新   —— HKCU PauseUpdatesExpiryTime
//   · 推迟 / 恢复功能更新 —— HKCU DeferFeatureUpdates + TargetReleaseVersion
//   · 禁用 / 恢复系统更新 —— HKLM AU NoAutoUpdate（危险方向，二次确认）
//   · RefreshPolicyButtons —— 三枚切换键的文案与配色同步
//
// 闭包说明：九个函数均为 CPageUpdate 成员（声明见 Page_Update.h），
// 依赖面仅四项且全部为头文件级接口：
//   · m_ctx（HostToast / HostLog / HostConfirmWarn / HostSetCtrlText / HostFindControl）
//   · StringMgr（update.s_* 语言键，键名逐字保持，未新增）
//   · AppxCapUpdService（StartUpdateHideThread / Set*UpdatePolicy / IsUpdate*）
//   · CurList / CurChecked / m_nSelected（页内状态，头文件已声明）
// 因此跨 TU 无静态实体依赖，原文件不残留 file-scope 符号。
// ============================================================

#include "stdafx.h"
#include "ThemeTokens.h"   // [R203] 颜色令牌（深色/浅色跟随）

#include "Page_Update.h"

#include "StringManager.h"       // StringMgr（update.s_* 语言键）
#include "CustomControls.h"      // CButtonUI / CControlUI
#include "AppxCapUpdService.h"   // StartUpdateHideThread / Set*UpdatePolicy

using namespace DuiLib;
using i18n::CStringManager;

// [R168] 暂停更新的天数。原为 OnPauseButton 内两处裸字面量 7
//（Format 的参数与 SetPauseUpdatePolicy 的第二参），改用具名常量，
// 避免两处漂移成不同的暂停期。
inline constexpr int kUpdPauseDays = 7;

// [R168] 策略写接口的统一成功判定：AppxCapUpdService 的 Set*UpdatePolicy
// 一律返回 Win32 错误码，0 表示注册表写入成功。六处 if (dw == 0) 此前
// 各自硬编码，现统一此处；[[nodiscard]] 防止调用方丢弃判定结果而
// 误用成功文案（历史上正是这类漏判把失败报成成功）。
[[nodiscard]] static bool UpdPolicySucceeded(DWORD dw) noexcept { return dw == 0; }

// ============================================================
// [R82] WUA 隐藏 / 取消隐藏更新（P0-5 HideUpdate / UnhideUpdate）
// ============================================================

void CPageUpdate::OnHideButton()
{
    // [功能补齐] 仅收集勾选的未安装项（Tab0 待安装列表）UpdateID
    // [R127-A] 收集与空态提示提取为 CollectCheckedUpdateIds
    std::vector<CDuiString> vIDs;
    if (!CollectCheckedUpdateIds(vIDs)) return;
    // 确认弹窗：隐藏后这些更新在常规搜索中不再显示
    CDuiString sAsk;
    sAsk.Format(StringMgr.Get("update.s_hide_confirm"), static_cast<int>(vIDs.size()));
    if (!m_ctx.HostConfirmWarn(StringMgr.Get("update.s_hide_title"), sAsk.GetData()))
        return;
    m_ctx.HostSetCtrlText(L"upd_install_state",
                          StringMgr.Get("update.s_hide_preparing"));
    m_ctx.HostLog(StringMgr.Get("update.s_hide_start"));
    AppxCapUpdService::StartUpdateHideThread(m_ctx.HostHwnd(), vIDs, true);
}

void CPageUpdate::OnUnhideButton()
{
    // 已安装 Tab 选中行才可取消隐藏；无选中则提示
    if (m_nSelected < 0 || m_nSelected >= static_cast<int>(CurList().size()))
    {
        m_ctx.HostToast(StringMgr.Get("update.s_unhide_noselected"),
                        StringMgr.Get("update.s_unhide_noselected"), 1);
        return;
    }
    const CDuiString& sId = CurList()[m_nSelected].sUpdateID;
    if (sId.IsEmpty())
    {
        m_ctx.HostToast(StringMgr.Get("update.s_unhide_noselected"),
                        StringMgr.Get("update.s_unhide_noselected"), 1);
        return;
    }
    m_ctx.HostSetCtrlText(L"upd_install_state",
                          StringMgr.Get("update.s_unhide_preparing"));
    m_ctx.HostLog(StringMgr.Get("update.s_unhide_start"));
    std::vector<CDuiString> vIDs;
    vIDs.push_back(sId);
    AppxCapUpdService::StartUpdateHideThread(m_ctx.HostHwnd(), vIDs, false);
}

void CPageUpdate::OnPauseButton()
{
    // [R86 P1-18] 暂停自动更新：写 WUA 策略注册表（PauseUpdatesExpiryTime）
    CDuiString sAsk;
    sAsk.Format(StringMgr.Get("update.s_pause_confirm"), kUpdPauseDays);
    if (!m_ctx.HostConfirmWarn(StringMgr.Get("update.s_pause_title"), sAsk.GetData()))
        return;
    DWORD dw = AppxCapUpdService::SetPauseUpdatePolicy(true, kUpdPauseDays);
    CDuiString sLog;
    sLog.Format(StringMgr.Get("update.s_pause_done"), static_cast<int>(dw));
    m_ctx.HostLog(sLog.GetData());
    if (UpdPolicySucceeded(dw))
        m_ctx.HostToast(StringMgr.Get("update.s_pause_title"),
                        StringMgr.Get("update.s_pause_done_short"), 0);
    else
        m_ctx.HostToast(StringMgr.Get("update.s_pause_title"),
                        StringMgr.Get("update.s_pause_fail_short"), 1);
    RefreshPolicyButtons();   // [按钮整合] 切换键文案翻为"恢复更新"
}

void CPageUpdate::OnUnpauseButton()
{
    // [R86 P1-18] 取消暂停自动更新：删除 PauseUpdatesExpiryTime
    CDuiString sAsk;
    sAsk.Format(StringMgr.Get("update.s_unpause_confirm"));
    if (!m_ctx.HostConfirmWarn(StringMgr.Get("update.s_unpause_title"), sAsk.GetData()))
        return;
    DWORD dw = AppxCapUpdService::SetPauseUpdatePolicy(false, 0);
    CDuiString sLog;
    sLog.Format(StringMgr.Get("update.s_unpause_done"), static_cast<int>(dw));
    m_ctx.HostLog(sLog.GetData());
    if (UpdPolicySucceeded(dw))
        m_ctx.HostToast(StringMgr.Get("update.s_unpause_title"),
                        StringMgr.Get("update.s_unpause_done_short"), 0);
    else
        m_ctx.HostToast(StringMgr.Get("update.s_unpause_title"),
                        StringMgr.Get("update.s_unpause_fail_short"), 1);
    RefreshPolicyButtons();   // [按钮整合] 切换键文案翻为"暂停更新"
}

void CPageUpdate::OnSkipButton()
{
    // [R86 P1-18] 推迟功能更新：DeferFeatureUpdates=1 + TargetReleaseVersion
    CDuiString sVersion = L"";
    CDuiString sAsk;
    sAsk.Format(StringMgr.Get("update.s_skip_confirm"));
    if (!m_ctx.HostConfirmWarn(StringMgr.Get("update.s_skip_title"), sAsk.GetData()))
        return;
    DWORD dw = AppxCapUpdService::SetSkipUpdatePolicy(true, sVersion);
    CDuiString sLog;
    sLog.Format(StringMgr.Get("update.s_skip_done"), static_cast<int>(dw));
    m_ctx.HostLog(sLog.GetData());
    if (UpdPolicySucceeded(dw))
        m_ctx.HostToast(StringMgr.Get("update.s_skip_title"),
                        StringMgr.Get("update.s_skip_done_short"), 0);
    else
        m_ctx.HostToast(StringMgr.Get("update.s_skip_title"),
                        StringMgr.Get("update.s_skip_fail_short"), 1);
    RefreshPolicyButtons();   // [按钮整合] 切换键文案翻为"恢复功能更新"
}

void CPageUpdate::OnUnskipButton()
{
    // [R86 P1-18] 恢复功能更新：清除 DeferFeatureUpdates
    CDuiString sAsk;
    sAsk.Format(StringMgr.Get("update.s_unskip_confirm"));
    if (!m_ctx.HostConfirmWarn(StringMgr.Get("update.s_unskip_title"), sAsk.GetData()))
        return;
    DWORD dw = AppxCapUpdService::SetSkipUpdatePolicy(false, CDuiString());
    CDuiString sLog;
    sLog.Format(StringMgr.Get("update.s_unskip_done"), static_cast<int>(dw));
    m_ctx.HostLog(sLog.GetData());
    if (UpdPolicySucceeded(dw))
        m_ctx.HostToast(StringMgr.Get("update.s_unskip_title"),
                        StringMgr.Get("update.s_unskip_done_short"), 0);
    else
        m_ctx.HostToast(StringMgr.Get("update.s_unskip_title"),
                        StringMgr.Get("update.s_unskip_fail_short"), 1);
    RefreshPolicyButtons();   // [按钮整合] 切换键文案翻为"推迟功能更新"
}

// ============================================================
// [R-B49] DisableInstallUpdate：彻底禁用 / 恢复系统更新
// 清单维度 7 缺失 2（原版 DisableInstallUpdate 策略）。
// 按钮文本在 page_update.xml 中静态（btn_upd_disable / btn_upd_enable），
// 语言键沿用 update.s_* 既有体系；本轮为控制面闭环，新增键见下。
// ============================================================

void CPageUpdate::OnDisableButton()
{
    // 危险操作：禁用系统更新会影响长期安全性，二次确认必须醒目
    CDuiString sAsk;
    sAsk.Format(StringMgr.Get("update.s_disable_confirm"));
    if (!m_ctx.HostConfirmWarn(StringMgr.Get("update.s_disable_title"), sAsk.GetData()))
        return;
    DWORD dw = AppxCapUpdService::SetDisableUpdatePolicy(true);
    CDuiString sLog;
    sLog.Format(StringMgr.Get("update.s_disable_done"), static_cast<int>(dw));
    m_ctx.HostLog(sLog.GetData());
    if (UpdPolicySucceeded(dw))
        m_ctx.HostToast(StringMgr.Get("update.s_disable_title"),
                        StringMgr.Get("update.s_disable_done_short"), 0);
    else
        m_ctx.HostToast(StringMgr.Get("update.s_disable_title"),
                        StringMgr.Get("update.s_disable_fail_short"), 1);
    RefreshPolicyButtons();   // [按钮整合] 切换键翻为"恢复系统更新"+中性配色
}

void CPageUpdate::OnEnableButton()
{
    CDuiString sAsk;
    sAsk.Format(StringMgr.Get("update.s_enable_confirm"));
    if (!m_ctx.HostConfirmWarn(StringMgr.Get("update.s_enable_title"), sAsk.GetData()))
        return;
    DWORD dw = AppxCapUpdService::SetDisableUpdatePolicy(false);
    CDuiString sLog;
    sLog.Format(StringMgr.Get("update.s_enable_done"), static_cast<int>(dw));
    m_ctx.HostLog(sLog.GetData());
    if (UpdPolicySucceeded(dw))
        m_ctx.HostToast(StringMgr.Get("update.s_enable_title"),
                        StringMgr.Get("update.s_enable_done_short"), 0);
    else
        m_ctx.HostToast(StringMgr.Get("update.s_enable_title"),
                        StringMgr.Get("update.s_enable_fail_short"), 1);
    RefreshPolicyButtons();   // [按钮整合] 切换键翻回红色"禁用系统更新"
}

// ============================================================
// [按钮整合] 三枚策略切换键的文案/配色同步
// ============================================================
void CPageUpdate::RefreshPolicyButtons()
{
    // 暂停更新 ↔ 恢复更新（HKCU PauseUpdatesExpiryTime）
    CControlUI* pPause = m_ctx.HostFindControl(L"btn_upd_pausetoggle");
    if (pPause)
        pPause->SetText(AppxCapUpdService::IsUpdatePaused()
                            ? L"恢复更新" : L"暂停更新");
    // 推迟功能更新 ↔ 恢复功能更新（HKCU DeferFeatureUpdates）
    CDuiString sVer;
    const bool bSkipped = AppxCapUpdService::IsUpdateSkipped(sVer);
    CControlUI* pSkip = m_ctx.HostFindControl(L"btn_upd_skiptoggle");
    if (pSkip)
        pSkip->SetText(bSkipped ? L"恢复功能更新" : L"推迟功能更新");
    // 禁用系统更新 ↔ 恢复系统更新（HKLM AU NoAutoUpdate）
    // 危险方向（即将执行"禁用"）保留红色实心样式；恢复方向用中性白底描边。
    const bool bDisabled = AppxCapUpdService::IsUpdateDisabled();
    CButtonUI* pDis = static_cast<CButtonUI*>(
        m_ctx.HostFindControl(L"btn_upd_disabletoggle"));
    if (pDis)
    {
        pDis->SetText(bDisabled ? L"恢复系统更新" : L"禁用系统更新");
        if (bDisabled)
        {
            pDis->SetTextColor(tokens::Token(tokens::EToken::Ink));
            pDis->SetBkColor(tokens::Token(tokens::EToken::Surface));
            pDis->SetHotBkColor(tokens::Token(tokens::EToken::SurfaceHover));
            pDis->SetPushedBkColor(tokens::Token(tokens::EToken::AccentSoft));
            pDis->SetBorderColor(tokens::Token(tokens::EToken::Border));
        }
        else
        {
            pDis->SetTextColor(tokens::Token(tokens::EToken::OnDanger));
            pDis->SetBkColor(tokens::Token(tokens::EToken::Danger));
            pDis->SetHotBkColor(tokens::Token(tokens::EToken::DangerHot));
            pDis->SetPushedBkColor(tokens::Token(tokens::EToken::DangerPushed));
            pDis->SetBorderColor(tokens::Token(tokens::EToken::Danger));
        }
        pDis->Invalidate();
    }
}
