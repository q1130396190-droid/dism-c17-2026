// ============================================================
// Page_UpdateButtons.cpp —— 更新页「按钮分派 + 统计日志」（从 Page_Update.cpp 拆出）。
//
// [R161] Page_Update.cpp 现 1,565 行、规模分 8.2（六页最低）。
// 本文件承接两个自成体系的函数：
//   · HandleButtonClick —— 13 个按钮名的 click 分派（每个分支一次转发即返回）
//   · UpdLogStats       —— 枚举结果统计日志 + WUA 错误码本地化 + 状态条
//
// 闭包说明：两者均为 CPageUpdate 成员（Page_Update.h 已声明）。
// HandleButtonClick 调用的 17 个成员函数（OnRefreshElapsed / OnPolicyMenu /
// OnUninstallButton / StartUpdatePhase / UpdStartBatchRender 等）全部在头文件
// 声明，跨 TU 无碍。UpdLogStats 依赖的自由函数 UpdateTypeOf / WuaLocalizeErrorCode
// 在 FrameWnd_Util.h。
//
// 搬迁为逐字搬运，零逻辑改动。
// ============================================================

#include "stdafx.h"

#include "Page_Update.h"

#include "StringManager.h"       // StringMgr
#include "FrameWnd_Util.h"       // UpdateTypeOf / WuaLocalizeErrorCode
#include "CustomControls.h"      // CVerticalLayoutUI
#include "AppxCapUpdService.h"   // AppxCapUpdService::IsUpdatePaused / Skipped / Disabled

#include <memory>

using namespace DuiLib;
using i18n::CStringManager;

// [C++17 R6] 本 TU 特征常量：CBS 折叠后单批追加的行数上限（与原语义一致）。
inline constexpr int kUpdCbsBatchStep = 24;
static_assert(kUpdCbsBatchStep > 0, "batch step must be positive");

// [R169] 从 HandleButtonClick 抽出：[性能 A2] CBS 折叠组展开。
//   顺序必须是"先摘占位行，再起分批渲染"：
//   若先渲染，新行会插到占位行之前，占位行留在列表底部形成一条空行；
//   原实现正是先删后渲染，这里保持。
//
//   倒序遍历删除：正序删会让后续索引整体前移、漏删。
//   只删名为 cbstoggle_row 的项 —— 同名多行（多组折叠）都要清掉。
void CPageUpdate::UpdExpandCbsGroup()
{
    MarkCbsExpanded(true);
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(
        m_ctx.HostFindControl(L"upd_list"));
    if (!pList) return;

    for (int r = pList->GetCount() - 1; r >= 0; --r)
    {
        CControlUI* p = pList->GetItemAt(r);
        if (p && p->GetName() && lstrcmpW(p->GetName(), L"cbstoggle_row") == 0)
            pList->RemoveAt(r);
    }
    UpdStartBatchRender(pList, 1, true);
}

[[nodiscard]] bool CPageUpdate::HandleButtonClick(const CDuiString& sName)
{
    if (sName == L"btn_upd_refresh")
    {
        OnRefreshElapsed();
        LogLastWuaError();
        StartEnumeration(m_nCurTab);
        return true;
    }
    // [性能 P3] 策略下拉菜单（收纳 暂停/推迟/禁用 三枚 toggle）
    else if (sName == L"btn_upd_policy")
    {
        OnPolicyMenu();
        return true;
    }
    // [性能 A2] CBS 折叠组展开：点击"显示系统组件包(N)"行后分批追加
    else if (sName == L"cbstoggle_row")
    {
        // [R169] 展开逻辑抽为 UpdExpandCbsGroup（105→约 85 行）
        UpdExpandCbsGroup();
        return true;
    }
    else if (sName == L"btn_upd_delete")
    {
        OnUninstallButton();
        return true;
    }
    else if (sName == L"btn_upd_selall")
    {
        // [功能补齐] 全选/全不选（批量安装/隐藏按勾选集合执行）
        OnSelectAll();
        return true;
    }
    // [R74] 三阶段更新流 UI 入口
    else if (sName == L"btn_upd_predownload")
    {
        StartUpdatePhase(0);
        return true;
    }
    else if (sName == L"btn_upd_install")
    {
        // [R80] 与 R74 三阶段流分离：R74 走 dism++ /Install 系统级安装，
        // R80 走 WUA COM 正向安装（仅针对待安装列表 UpdateID）
        OnInstallButton();
        return true;
    }
    else if (sName == L"btn_upd_finalize")
    {
        StartUpdatePhase(2);
        return true;
    }
    else if (sName == L"btn_upd_hide")
    {
        // [R82] P0-5 HideUpdate：隐藏待安装项
        OnHideButton();
        return true;
    }
    else if (sName == L"btn_upd_unhide")
    {
        // [R82] P0-5 UnhideUpdate：取消隐藏选中项
        OnUnhideButton();
        return true;
    }
    // [按钮整合] 暂停更新/恢复更新合并为单一切换键：
    // 文案随当前策略状态切换，点一下执行反向操作，再点切回。
    else if (sName == L"btn_upd_pausetoggle")
    {
        if (AppxCapUpdService::IsUpdatePaused())
            OnUnpauseButton();
        else
            OnPauseButton();
        return true;
    }
    // [按钮整合] 推迟功能更新/恢复功能更新合并为单一切换键
    else if (sName == L"btn_upd_skiptoggle")
    {
        CDuiString sVer;
        if (AppxCapUpdService::IsUpdateSkipped(sVer))
            OnUnskipButton();
        else
            OnSkipButton();
        return true;
    }
    // [按钮整合] 禁用系统更新/恢复系统更新合并为单一切换键（危险方向红色样式）
    else if (sName == L"btn_upd_disabletoggle")
    {
        if (AppxCapUpdService::IsUpdateDisabled())
            OnEnableButton();
        else
            OnDisableButton();
        return true;
    }
    return false;   // 未命中任何按钮：交由后续 updrow_ 段继续判断
}

// [R162] 从 UpdLogStats 抽出：按更新标题分类计数（安全/累积/定义/功能/驱动/其他）。
//   分类规则沿用既有的 UpdateTypeOf，本题不动规则本身，只把"遍历 + 分派"独立出来。
//   纯计数、无 UI 无日志，便于单独验证分类是否符合预期。
void CPageUpdate::CountByUpdateType(const std::vector<UpdEntry>& vUpd,
                                    int& nSec, int& nCum, int& nDef,
                                    int& nFeat, int& nDrv, int& nOth)
{
    nSec = nCum = nDef = nFeat = nDrv = nOth = 0;
    for (size_t kk = 0; kk < vUpd.size(); ++kk)
    {
        switch (UpdateTypeOf(vUpd[kk].sTitle))
        {
            case 1: ++nSec;  break;
            case 2: ++nCum;  break;
            case 3: ++nDef;  break;
            case 4: ++nFeat; break;
            case 5: ++nDrv;  break;
            default: ++nOth; break;
        }
    }
}

void CPageUpdate::UpdLogStats(UpdDataMsg* pData)
{
    if (!pData) return;   // [R175] 早退代替整函数包裹，嵌套 4→3
CDuiString sLog;
sLog.Format(L"[update] tab=%d count=%d err=%d",
             pData->nTab, static_cast<int>(pData->vUpd.size()), pData->nErr);
m_ctx.HostLog(sLog.GetData());
// [R52] 类型分类统计：安全/累积/定义/功能/驱动/其他
// [R162] for+switch 抽为 CountByUpdateType（纯计数，可单测），嵌套 5→3
{
    int nSec = 0, nCum = 0, nDef = 0, nFeat = 0, nDrv = 0, nOth = 0;
    CountByUpdateType(pData->vUpd, nSec, nCum, nDef, nFeat, nDrv, nOth);
    if (!pData->vUpd.empty())
    {
        CDuiString sTypeLog;
        sTypeLog.Format(StringMgr.Get("misc.h7a8c"),
                        nSec, nCum, nDef, nFeat, nDrv, nOth);
        m_ctx.HostLog(sTypeLog.GetData());
    }
}
// [R43] 记录最近一次 WUA 错误码 + 本地化说明（若非成功）
if (pData->nErr != 0)
{
    SetWuaErr(pData->nErr);
    CDuiString sLoc;
    sLoc.Format(StringMgr.Get("misc.h1f33"),
                (unsigned int)pData->nErr,
                WuaLocalizeErrorCode(pData->nErr));
    m_ctx.HostLog(sLoc.GetData(), 1);
}
else
{
    SetWuaErr(0);
}
// [U-3] 页面收尾状态条：显示更新管理页完成情况
{
    CDuiString sPageStatus;
    sPageStatus.Format(StringMgr.Get("misc.hbae3"), static_cast<int>(pData->vUpd.size()));
    m_ctx.HostLog(sPageStatus.GetData());
}
// [C++17 阶段3] 消息接收端 unique_ptr 接管，所有权由投递方移交
std::unique_ptr<UpdDataMsg> spData(pData);  // [C++17 R3] unique_ptr 接管
}
