#include "stdafx.h"
// ============================================================
// Page_FeatureApply.cpp —— [R91] Windows 功能页"应用/变更组"
//
// 从 Page_Feature.cpp 切出的 5 个成员函数（函数体零逻辑改动）：
//   OnTabChanged   切到"Windows 功能"Tab 时的按钮态与自动加载
//   Apply          收集 pending → 二次确认 → 组包
//   LaunchApply    提交到 DismApiBridge 后台线程
//   OnApplyDone    应用结果回调：写后精准失效（R79）+ 重新枚举
//   DeleteSelected /remove 永久删除所选功能
//
// 为什么这组可以独立：
//   全部是成员函数，跨 TU 调用无链接性问题；
//   依赖 m_vFeatures / m_vLastApply / m_bApplying 等类成员，均在头文件声明；
//   切片外调用点：OnEvent（btn_prog_apply）、OnApplyDone（消息分发）、
//   HostFeatureDeleteSelected，全部经类声明访问。
//
// 拆分动因：Page_Feature.cpp 拆出 Menu 组后仍 1956 行，超过 1500 行门槛。
// ============================================================
#include "Page_Feature.h"
#include "FeatureService.h"      // FeatApplyItem / StartApplyThread / UpdateFeatCacheStates
#include "StringManager.h"
using namespace DuiLib;
using i18n::CStringManager;
// [R175] 从 OnTabChanged 抽出：是否存在"待应用"的功能变更。
//   判据三条件同时成立：
//     · sSys 非空（虚拟分组没有系统名，不参与）
//     · nPending != -1（该项被用户操作过）
//     · nPending 与当前实际状态不同（真的还没生效）
//   第三条是关键：用户改了又改回去时 nPending 等于当前值，
//   这时"应用"按钮不该亮 —— 否则用户点了会看到"没有需要应用的更改"。
[[nodiscard]] bool CPageFeature::FeatHasPendingChange() const
{
    for (size_t i = 0; i < m_vFeatures.size(); ++i)
    {
        const FeatInfo& f = m_vFeatures[i];
        if (f.sSys.IsEmpty()) continue;
        if (f.nPending == -1) continue;
        if (f.nPending != (f.bEnabled ? 1 : 0)) return true;
    }
    return false;
}

void CPageFeature::OnTabChanged(int idx)
{
    CControlUI* pLegend = m_ctx.HostFindControl(L"prog_legend");
    if (pLegend) pLegend->SetVisible(idx == 1);
    CButtonUI* pApply = static_cast<CButtonUI*>(m_ctx.HostFindControl(L"btn_prog_apply"));
    if (pApply)
    {
        // [R175] 待变更判定抽为 FeatHasPendingChange，嵌套 4→3
        pApply->SetVisible(idx == 1 && FeatHasPendingChange());
    }
    if (idx == 1)
    {
        CButtonUI* pDel = static_cast<CButtonUI*>(m_ctx.HostFindControl(L"btn_prog_delete"));
        if (pDel) pDel->SetVisible(false);
        // [修复] 原版行为：切到"Windows 功能"Tab 即自动后台枚举功能列表，
        // 旧实现只显示"请点击刷新读取"占位，用户必须手动点刷新（功能像坏了）。
        // 首次切入或上次枚举失败时自动加载；加载中/已加载则不重复发起。
        if (!m_bFeatLoaded && !m_bFeatLoading && !m_bApplying)
            Load();
    }
}
void CPageFeature::Apply()
{
    std::vector<int> vEnable;
    std::vector<int> vDisable;
    std::vector<int> vRemove;   // [修复 R5 / P1-13] /remove 永久删除负载
    for (size_t i = 0; i < m_vFeatures.size(); ++i)
    {
        const FeatInfo& f = m_vFeatures[i];
        if (f.sSys.IsEmpty() || f.nPending == -1) continue;
        int nWant = f.nPending;
        int nWas  = f.bEnabled ? 1 : 0;
        if (nWant == nWas) continue;
        if (nWant == 1)      vEnable.push_back(static_cast<int>(i));
        else if (nWant == 2) vRemove.push_back(static_cast<int>(i));
        else                 vDisable.push_back(static_cast<int>(i));
    }
    if (vEnable.empty() && vDisable.empty() && vRemove.empty())
    {
        m_ctx.HostAlert(StringMgr.Get("capability.s154094f3"),
                        StringMgr.Get("appx.s2a0b5038"), MB_ICONINFORMATION);
        return;
    }
    CDuiString sMsg;
    sMsg.Format(CDuiString(StringMgr.Get("appx.s1feb5581")) + StringMgr.Get("capability.sc2bb5ccd") + StringMgr.Get("update.s218be32b"),
                static_cast<int>(vEnable.size() + vDisable.size() + vRemove.size()),
                static_cast<int>(vEnable.size()), static_cast<int>(vDisable.size()), static_cast<int>(vRemove.size()));
    if (!m_ctx.HostConfirmWarn(StringMgr.Get("appx.s3d227c4d"), sMsg.GetData()))
        return;
    CEditUI* pSrc = static_cast<CEditUI*>(m_ctx.HostFindControl(L"feat_src_edit"));
    CDuiString sSource = pSrc ? pSrc->GetText() : CDuiString(L"");
    std::vector<FeatApplyItem> vItems;
    for (size_t i = 0; i < vEnable.size(); ++i)
    {
        FeatApplyItem it;
        it.sName = m_vFeatures[vEnable[i]].sSys;
        it.nOp = 1;
        vItems.push_back(it);
    }
    for (size_t i = 0; i < vDisable.size(); ++i)
    {
        FeatApplyItem it;
        it.sName = m_vFeatures[vDisable[i]].sSys;
        it.nOp = 0;
        vItems.push_back(it);
    }
    // /remove 永久删除负载 → DismDisableFeature(bRemovePayload=true)
    for (size_t i = 0; i < vRemove.size(); ++i)
    {
        FeatApplyItem it;
        it.sName = m_vFeatures[vRemove[i]].sSys;
        it.nOp = 2;
        vItems.push_back(it);
    }
    // 源路径/ LimitAccess 由 LaunchApply 从同一编辑框读取
    (void)sSource;
    LaunchApply(vItems);
}
void CPageFeature::LaunchApply(const std::vector<FeatApplyItem>& vItems)
{
    if (vItems.empty()) return;
    if (m_bApplying)
    {
        m_ctx.HostLog(L"[feat] apply already in progress");
        return;
    }
    CEditUI* pSrc = static_cast<CEditUI*>(m_ctx.HostFindControl(L"feat_src_edit"));
    CDuiString sSource = pSrc ? pSrc->GetText() : CDuiString(L"");
    m_bApplying = true;
    m_vLastApply = vItems;   // [R79 P1] 记录本次改动项，成功后据此精准更新缓存
    // [CRASHFIX-STUCK] 线程创建失败必须就地复位在途标志：WM_FEAT_APPLYDONE
    // 永远不会到达，旧实现让 m_bApplying 卡死 true，之后所有提交被
    // "apply already in progress" 静默拦截，只能重启程序。
    if (!FeatureService::StartApplyThread(m_ctx.HostHwnd(), vItems, sSource, !sSource.IsEmpty()))
    {
        m_bApplying = false;
        m_vLastApply.clear();
        m_ctx.HostLog(L"[feat] apply thread creation FAILED",
                      static_cast<int>(ELogLevel::LOG_ERROR));
        m_ctx.HostToast(StringMgr.Get("appx.s3d227c4d"),
                        StringMgr.Get("feat.s_apply_done_fail"), 2);
        return;
    }
    m_ctx.HostLog(L"[feat] apply launched via DismApiBridge");
}
// [R175] 从 OnApplyDone 抽出：[R79 P1] 写后精准失效。
//   把"本次改动项"的状态写回缓存，而不是删整个文件 ——
//   原实现删文件导致下次进页必然全量重扫（500~15000ms），而用户只改了 N 项。
//   命中失败（缓存不存在 / 格式坏 / 一项都没命中）才回退删除：
//   宁可重扫，也不能显示与实际不符的旧状态。
//
//   nOp 语义：0=禁用 1=启用 2=永久移除。永久移除后系统里已不存在，
//   写 enabled=0 与真实状态一致（后续枚举会把它彻底抹掉）。
void CPageFeature::FeatWriteBackChangedRows()
{
    std::vector<FeatRawEntry> vChanged;
    vChanged.reserve(m_vLastApply.size());
    for (size_t i = 0; i < m_vLastApply.size(); ++i)
    {
        FeatRawEntry e;
        e.sSys = m_vLastApply[i].sName;
        e.bEnabled = (m_vLastApply[i].nOp == 1);
        if (!e.sSys.IsEmpty()) vChanged.push_back(e);
    }
    if (!FeatureService::UpdateFeatCacheStates(vChanged))
        FeatureService::InvalidateFeatCache();
    else
        m_ctx.HostLog(L"[feat] cache updated in place (changed rows only)");
    m_vLastApply.clear();
}

void CPageFeature::OnApplyDone(FeatApplyMsg* pMsg)
{
    if (!pMsg) return;
    // [C++17 R3] receiver takes ownership, freed at scope end (was: delete pMsg hereafter)
    std::unique_ptr<FeatApplyMsg> msgGuard(pMsg);
    m_bApplying = false;
    if (!pMsg->sDetail.IsEmpty())
        m_ctx.HostLog(pMsg->sDetail.GetData());
    CDuiString sSummary;
    if (pMsg->nFail == 0)
    {
        sSummary.Format(StringMgr.Get("feat.s_apply_done"),
                        pMsg->nOk,
                        pMsg->bRebootHint ? L"（需要重启生效）" : L"");
        m_ctx.HostToast(StringMgr.Get("appx.s3d227c4d"), sSummary.GetData(), 1);
        // 清掉 pending 并立即重新枚举，反映新状态
        for (size_t i = 0; i < m_vFeatures.size(); ++i)
            m_vFeatures[i].nPending = -1;
        InvalidateAggCache();
        // [R79 P1] 写后精准失效：只把本次改动项的状态写回缓存，不删整个文件。
        //   原实现删文件 → 下次进页必然全量重扫（500~15000ms），而用户只改了 N 项。
        //   命中失败（缓存不存在/格式坏/一项都没命中）才回退删除，避免读到与
        //   实际不符的旧状态 —— 宁可重扫，也不能显示错的。
        // [R175] 写回抽为 FeatWriteBackChangedRows，嵌套 4→3
        FeatWriteBackChangedRows();
        Load();
    }
    else
    {
        sSummary.Format(StringMgr.Get("feat.s_apply_done_fail"),
                        pMsg->nOk, pMsg->nFail);
        m_ctx.HostAlert(StringMgr.Get("appx.s3d227c4d"),
                        sSummary.GetData(), MB_ICONWARNING);
        RebuildList();
    }
}
void CPageFeature::DeleteSelected()
{
    // Tab=1 Windows功能 → DismDisableFeature(/Remove) 永久删除（原生后台线程）
    if (m_nFeatSelected < 0 || m_nFeatSelected >= static_cast<int>(m_vFeatures.size()))
    {
        m_ctx.HostAlert(StringMgr.Get("capability.s64596f9a"),
                        StringMgr.Get("capability.sb43b4036"), MB_ICONINFORMATION);
        return;
    }
    const FeatInfo& feat = m_vFeatures[m_nFeatSelected];
    if (feat.sSys.IsEmpty()) return;   // 虚拟显示分组，不可操作
    CDuiString sConfirm;
    sConfirm.Format(StringMgr.Get("toolbox.s506e3e64"),
                    feat.sDisp.GetData(), feat.sSys.GetData());
    if (!m_ctx.HostConfirmYesNo(StringMgr.Get("capability.s6d0be396"),
                                sConfirm.GetData(), false)) return;
    FeatApplyItem it;
    it.sName = feat.sSys;
    it.nOp = 2;
    std::vector<FeatApplyItem> vItems(1, it);
    LaunchApply(vItems);
}
