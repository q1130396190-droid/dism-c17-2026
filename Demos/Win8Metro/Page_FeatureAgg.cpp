// ============================================================
// Page_FeatureAgg.cpp —— 功能页「生效状态 / 聚合状态」纯计算层。
//
// [R103] 从 Page_Feature.cpp 拆出。这一段是整页里**唯一不碰 UI 的部分**：
//   输入是 m_vFeatures，输出是 int（0 关 / 1 开 / 2 混合 / 3 永久移除）。
//
// 拆它的理由：
//   1. 它被三条链路共同调用 —— 渲染链路（Page_FeatureRender.cpp 每行取状态）、
//      交互链路（Page_FeatureInteraction.cpp 勾选后失效缓存）、
//      数据链路（ApplyRawData 重建后失效），留在主文件会让主文件同时承担
//      「数据装载 + 状态计算 + 事件路由」，职责过宽；
//   2. 它不依赖任何匿名命名空间符号（不含 FeatDisplayName/FeatDescription），
//      可以安全独立成 TU —— 这一点拆之前逐个查过。
//
// 拆分前后函数体逐字未改，仅搬迁位置。
// ============================================================

#include "stdafx.h"

#include "Page_Feature.h"

#include "DismCore.h"            // Rust dismcore::FeatAggregate（聚合计算下沉 Rust）

using namespace DuiLib;

int CPageFeature::Eff(int n)
{
    if (n < 0 || n >= static_cast<int>(m_vFeatures.size())) return 0;

    const FeatInfo& f = m_vFeatures[n];

    if (f.nPending != -1) return f.nPending;

    return f.bEnabled ? 1 : 0;
}

int CPageFeature::Agg(int n)
{
    // [FIX-PERF] 一次性自底向上缓存聚合状态，渲染时 O(1) 读取。
    // 原实现对每一行都递归遍历整个子树，同一子树被重复访问。
    if (n < 0 || n >= static_cast<int>(m_vFeatures.size())) return 0;
    if (m_vAggCache.size() == m_vFeatures.size() && m_vAggValid)
        return m_vAggCache[static_cast<size_t>(n)];

    m_vAggCache.assign(m_vFeatures.size(), 0);

    // [R8 2026-10-25] 聚合下沉到 Rust 组件（dismcore_feat_agg）。
    //   原实现靠"子节点下标必大于父节点"的隐含约定逆序遍历，
    //   那只是 ApplyRawData 构造顺序的副产品。Rust 侧用显式后序遍历，
    //   只依赖 parent 关系本身，并且能报告成环。
    {
        std::vector<int32_t> vPending;
        std::vector<uint8_t> vEnabled;
        std::vector<int32_t> vParent;
        vPending.reserve(m_vFeatures.size());
        vEnabled.reserve(m_vFeatures.size());
        vParent.reserve(m_vFeatures.size());
        for (size_t i = 0; i < m_vFeatures.size(); ++i)
        {
            vPending.push_back(m_vFeatures[i].nPending);
            vEnabled.push_back(m_vFeatures[i].bEnabled ? 1u : 0u);
            vParent.push_back(m_vFeatures[i].nParent);
        }
        std::vector<uint8_t> vAgg;
        if (dismcore::FeatAggregate(vPending, vEnabled, vParent, vAgg)
            && vAgg.size() == m_vFeatures.size())
        {
            for (size_t i = 0; i < vAgg.size(); ++i)
                m_vAggCache[i] = static_cast<int>(vAgg[i]);
            MarkAggValid();
            return m_vAggCache[static_cast<size_t>(n)];
        }
        // [回退] Rust 失败（含成环）时走下方原逻辑。
    }

    // 由后向前遍历：ApplyRawData 按父先子后建立索引，子节点下标大于父节点，
    // 因此逆序即可保证子节点先算完（不依赖递归，避免深树栈溢出）。
    for (int i = static_cast<int>(m_vFeatures.size()) - 1; i >= 0; --i)
    {
        const FeatInfo& f = m_vFeatures[static_cast<size_t>(i)];
        if (f.vChild.empty())
        {
            const int nEff = Eff(i);
            m_vAggCache[static_cast<size_t>(i)] = (nEff == 2) ? 3 : nEff;
            continue;
        }
        int nTotal = static_cast<int>(f.vChild.size());
        int nOn = 0, nOff = 0, nRm = 0;
        for (size_t c = 0; c < f.vChild.size(); ++c)
        {
            const int s = m_vAggCache[static_cast<size_t>(f.vChild[c])];
            if (s == 1) ++nOn;
            else if (s == 0) ++nOff;
            else if (s == 3) ++nRm;
        }
        int nAgg = 2;
        if (nOn == nTotal) nAgg = 1;
        else if (nOff == nTotal) nAgg = 0;
        else if (nRm == nTotal) nAgg = 3;
        m_vAggCache[static_cast<size_t>(i)] = nAgg;
    }
    MarkAggValid();
    return m_vAggCache[static_cast<size_t>(n)];
}

// 状态变化后使聚合缓存失效（下一次 Agg 重建）。
void CPageFeature::InvalidateAggCache() { m_vAggValid = false; }
