#include "stdafx.h"

// ============================================================
// Page_CapabilityActions.cpp —— [R91] 可选功能页"行操作与代理组"
//
// 从 Page_Capability.cpp 切出的 12 个成员函数（函数体零逻辑改动）：
//   ShowRowContextMenu        行级右键菜单（4601~4604 复制/详情）
//   ShowRowDetail             详情 Toast
//   OnGlobalCopy              Ctrl+C 复制首项
//   OnAsyncMessage            异步消息分发（WM_CAP_DATA / PARTIAL / FEAT_*）
//   OnHostTimer               页属定时器分发
//   HostFeatureTabChanged     ┐
//   HostFeatureLoad           │ 向 Windows 功能子视图的代理转发
//   HostFeatureDeleteSelected │（prog_pages Tab1 寄宿于程序页，
//   HostFeatureApplyFilter    │  壳把事件投给程序页，程序页再转给子视图）
//   HostFeatureEvent          │
//   HostFeatureContextMenu    ┘
//
// 为什么这组可以独立：
//   全部是成员函数，跨编译单元调用无链接性问题；
//   依赖的 CopyIdentity/CopyDisplayName/CopyDescription 已在
//   Page_CapabilityCopy.cpp（R77 拆出），同样经头文件声明访问；
//   不引用本文件任何 static / 匿名 namespace 符号。
//
// 拆分动因：Page_Capability.cpp 1545 行，超过 1500 行门槛。
// ============================================================

#include "Page_Capability.h"
#include "AppxCapUpdService.h"   // CapEntry / CapDataMsg / CapPartialMsg
#include "Page_Feature.h"        // m_pFeature 代理目标
#include "StringManager.h"
#include "FrameWnd_Util.h"       // PrettyCapName
#include "FrameWnd_Messages.h"   // WM_CAP_DATA / WM_FEAT_* 等
#include "WmMessages.h"

using namespace DuiLib;
using i18n::CStringManager;

void CPageCapability::ShowRowContextMenu(int k, POINT pt)
{
    if (k < 0 || k >= static_cast<int>(m_vCaps.size())) return;

    HMENU hMenu = ::CreatePopupMenu();
    ::AppendMenu(hMenu, MF_STRING, 4601, StringMgr.Get("cap.s_ctx_copyid"));
    ::AppendMenu(hMenu, MF_STRING, 4602, StringMgr.Get("cap.s_ctx_copyname"));
    ::AppendMenu(hMenu, MF_STRING, 4603, StringMgr.Get("cap.s_ctx_copydesc"));
    ::AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenu(hMenu, MF_STRING, 4604, StringMgr.Get("cap.s_ctx_detail"));

    int iCmd = ::TrackPopupMenu(hMenu,
        TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
        pt.x, pt.y, 0, m_ctx.HostHwnd(), nullptr);
    ::DestroyMenu(hMenu);

    if (iCmd == 4601)        CopyIdentity(k);
    else if (iCmd == 4602)   CopyDisplayName(k);
    else if (iCmd == 4603)   CopyDescription(k);
    else if (iCmd == 4604)   ShowRowDetail(k);
}


void CPageCapability::ShowRowDetail(int k)
{
    if (k < 0 || k >= static_cast<int>(m_vCaps.size())) return;
    const CapEntry& e = m_vCaps[k];

    CDuiString sBody;
    sBody.Format(L"[%s]\r\n%s\r\n\r\nSize: %s\r\nState: %s",
                 PrettyCapName(e.sIdentity).GetData(),
                 e.sDescription.IsEmpty() ? e.sIdentity.GetData() : e.sDescription.GetData(),
                 e.sSize.IsEmpty() ? L"-" : e.sSize.GetData(),
                 e.sState.IsEmpty() ? L"-" : e.sState.GetData());
    m_ctx.HostToast(StringMgr.Get("cap.s_ctx_detail"), sBody.GetData(), 1);

    CDuiString sLog;
    sLog.Format(L"[capability] detail: %s | %s", e.sIdentity.GetData(), e.sState.GetData());
    m_ctx.HostLog(sLog.GetData());
}
// ============================================================
// [R-B1] Ctrl+C / Ctrl+X 复制首个能力项 Identity
// ============================================================

void CPageCapability::OnGlobalCopy(bool /*bCut*/)
{
    if (m_vCaps.empty())
    {
        m_ctx.HostToast(StringMgr.Get("cap.s_nocopy"),
                        StringMgr.Get("cap.s_nocopy_body"), 1);
        return;
    }
    CopyIdentity(0);
}

// ============================================================
// [R5] 异步消息 / 定时器 / Windows 功能子视图代理
// ============================================================

bool CPageCapability::OnAsyncMessage(UINT uMsg, WPARAM /*wParam*/, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_CAP_DATA:
        // kMsgOwnPage：CapDataMsg* 由本页 OnData 释放。
        OnData((CapDataMsg*)lParam);
        return true;

    case WM_CAP_PARTIAL:
        // [P1] 枚举分批回报：UI 逐批追加渲染，首屏不等整表。
        // 消息对象由 OnCapPartial 释放（与 WM_CAP_DATA 同一所有权约定）。
        OnCapPartial((CapPartialMsg*)lParam);
        return true;

    case WM_FEAT_DATA:
        // kMsgOwnPage：FeatDataMsg* 由功能子视图 OnData 释放。
        // [FIX-P1E] m_pFeature 未绑定时也必须释放载荷（unique_ptr 就地回收），
        // 否则守卫早退 = 泄漏。以下 WM_FEAT_PARTIAL / WM_FEAT_APPLYDONE 同型。
        if (m_pFeature) m_pFeature->OnData((FeatDataMsg*)lParam);
        else delete (FeatDataMsg*)lParam;
        return true;

    case WM_FEAT_PARTIAL:
        // [P1] 功能枚举分批回报，由功能子视图消费并释放消息对象。
        if (m_pFeature) m_pFeature->OnPartial((FeatPartialMsg*)lParam);
        else delete (FeatPartialMsg*)lParam;
        return true;

    case WM_FEAT_APPLYDONE:
        // kMsgOwnPage：FeatApplyMsg* 由功能子视图 OnApplyDone 释放。
        if (m_pFeature) m_pFeature->OnApplyDone((FeatApplyMsg*)lParam);
        else delete (FeatApplyMsg*)lParam;
        return true;

    default:
        return false;
    }
}

void CPageCapability::OnHostTimer(UINT uId)
{
    switch (uId)
    {
    case CAP_WATCHDOG_TIMER_ID:
        // 原壳 OnTimerCapWatchdog：先 Kill 一次性看门狗，再解除永久加载态。
        ::KillTimer(m_ctx.HostHwnd(), uId);
        OnLoadWatchdog();
        break;

    case FEAT_WATCHDOG_TIMER_ID:
        // 原壳 OnTimerFeatWatchdog：功能子视图枚举看门狗。
        ::KillTimer(m_ctx.HostHwnd(), uId);
        if (m_pFeature) m_pFeature->OnLoadWatchdog();
        break;

    case CAP_REFRESH_TIMER_ID:
        OnRefreshTimer();
        break;

    case CAP_RETRY_TIMER_ID:
        OnRetryTimer();
        break;

    case CAP_RENDER_BATCH_TIMER_ID:
        // [R16][O-2] 分批渲染接力（一次性：先 Kill 再续批）
        ::KillTimer(m_ctx.HostHwnd(), uId);
        RenderBatch();
        break;

    case FEAT_RENDER_BATCH_TIMER_ID:
        // [STAB] T8 功能页分批渲染接力的分发终点。
        // 关键：PageById(PAGE_FEATURE) 返回的是本类（m_pgCapability），
        // 而 CPageFeature 只是其内部子视图、不占页序（见 PageHost.cpp 装配注释）。
        // 因此壳把 FEAT_RENDER_BATCH_TIMER_ID 分发到本页，必须由本页转发给
        // m_pFeature，否则事件落进 default 分支、功能列表永远停在第一批 50 行。
        // 注意：不在此 KillTimer —— 一次性定时器由接收方 CPageFeature 自行 Kill，
        // 两边都 Kill 虽幂等但会让"谁负责停表"的意图模糊。
        if (m_pFeature) m_pFeature->OnHostTimer(uId);
        break;

    default:
        break;
    }
}

void CPageCapability::HostFeatureTabChanged(int idx)
{
    if (m_pFeature) m_pFeature->OnTabChanged(idx);
}

void CPageCapability::HostFeatureLoad()
{
    // [FIX-T3-4] 刷新入口（Tab1 btn_prog_refresh）：置刷新请求标志，
    // 使 Load() 绕过会话复用早退——原实现有数据后刷新完全无效。
    if (m_pFeature)
    {
        m_pFeature->m_bFeatRefreshRequested = true;
        m_pFeature->Load();
    }
}

void CPageCapability::HostFeatureDeleteSelected()
{
    if (m_pFeature) m_pFeature->DeleteSelected();
}

void CPageCapability::HostFeatureApplyFilter(const CDuiString& sKeyword, bool bNoKw)
{
    if (m_pFeature) m_pFeature->ApplyFilterKw(sKeyword, bNoKw);
}

// [修复] prog_pages Tab1 寄宿于程序页，壳只把事件投递给程序页；
// 程序页把功能行/按钮的 click/dbclick 经本代理转入子视图。
void CPageCapability::HostFeatureEvent(TNotifyUI& msg)
{
    if (m_pFeature) m_pFeature->OnEvent(msg);
}

bool CPageCapability::HostFeatureContextMenu(POINT pt, bool bFromContextMsg)
{
    return m_pFeature ? m_pFeature->OnContextMenu(pt, bFromContextMsg) : false;
}
