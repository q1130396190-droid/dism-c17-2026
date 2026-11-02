// ============================================================
// Page_AppxLog.cpp —— 应用页「统计日志与计数徽标」（从 Page_Appx.cpp 拆出）。
//
// [R190] Page_Appx.cpp 拆前 1,020 行、规模分 9.67（六页最大 loc）。本文件承接
// 五个成员 + 两个阈值常量，合计 138 行，边界按「只读统计、只写日志/徽标」划分：
//   · AxLogStats              系统内置包数 + 唯一 Publisher 数统计日志
//   · AxCountPublishers       按发布者统计包数（外层遍历包 × 内层遍历已见发布者）
//   · AxSortPublishersByCount 按包数降序的插入排序（<=30 项，无需 std::sort）
//   · AxLogPublisherTop       Top-N 发布者日志 + 单厂商聚集告警
//   · UpdateCount             计数徽标文案（未选 / 已选 x/y）
//
// 闭包说明：五个成员声明（含 AxPubCount 结构）见 Page_Appx.h。依赖面仅三项：
//   · m_ctx（HostFindControl / HostLog）
//   · StringMgr（misc.hc97a / misc.h3237 / misc.s5d27f1ec / misc.s056993c2，逐字保持）
//   · kAppxTopPublisherCount / kAppxVendorClusterThreshold（随组迁入本 TU）
// 本段不读 m_vCache、不调 AppendLog，故无跨 TU 静态依赖。
//
// 逐字搬运，零逻辑改动。
// ============================================================

#include "stdafx.h"

#include "Page_Appx.h"

#include "StringManager.h"
#include "CustomControls.h"      // CTextUI

using namespace DuiLib;
using i18n::CStringManager;

void CPageAppx::AxLogStats(int nTab, const std::vector<AppxPkg>& v)

{
    // [R48] 系统内置包统计日志 + [R50] 唯一 Publisher 统计
    int nSys = 0;
    int nPub = 0;
    std::vector<CDuiString> vSeenPub;
    for (size_t k = 0; k < v.size(); ++k)
    {
        if (v[k].bSystem) ++nSys;
        if (!v[k].sPublisher.IsEmpty())
        {
            bool bDup = false;
            for (size_t j = 0; j < vSeenPub.size(); ++j)
                if (lstrcmpiW(vSeenPub[j].GetData(), v[k].sPublisher.GetData()) == 0) { bDup = true; break; }
            if (!bDup) { vSeenPub.push_back(v[k].sPublisher); ++nPub; }
        }
    }
    if (nSys > 0 || nPub > 0)
    {
        CDuiString sLog;
        sLog.Format(L"[appx] tab=%d total=%d system=%d user=%d publishers=%d",
                    nTab, static_cast<int>(v.size()), nSys, static_cast<int>(v.size()) - nSys, nPub);
        m_ctx.HostLog(sLog.GetData());
    }
    // [R54] 发布者分组视图：Top-3 发布者统计
    if (nPub >= 2)
    {
        AxLogPublisherTop(nTab, v, vSeenPub);
    }
}

// [R175] 从 AxLogPublisherTop（48 行 / CC13）抽出：按发布者统计包数。
// 原为「外层遍历包 × 内层遍历已见发布者」的嵌套循环，CC 记账 4 个判定点。
// 忽略空 Publisher；比较不区分大小写（原 lstrcmpiW 语义）。
void CPageAppx::AxCountPublishers(const std::vector<AppxPkg>& v,
                                  const std::vector<CDuiString>& vSeenPub,
                                  std::vector<AxPubCount>& vPubs)
{
    vPubs.clear();
    for (size_t j = 0; j < vSeenPub.size(); ++j)
    {
        AxPubCount pc; pc.sName = vSeenPub[j]; pc.n = 0;
        vPubs.push_back(pc);
    }
    for (size_t k = 0; k < v.size(); ++k)
    {
        if (v[k].sPublisher.IsEmpty()) continue;
        for (size_t j = 0; j < vPubs.size(); ++j)
        {
            if (lstrcmpiW(vPubs[j].sName.GetData(), v[k].sPublisher.GetData()) == 0)
            { ++vPubs[j].n; break; }
        }
    }
}

// [R175] 从 AxLogPublisherTop 抽出：按包数降序的插入排序。
// Publisher 数量通常 <= 30，插入排序比 std::sort 更省（无递归、无额外分配）。
void CPageAppx::AxSortPublishersByCount(std::vector<AxPubCount>& vPubs)
{
    for (size_t j = 1; j < vPubs.size(); ++j)
    {
        AxPubCount key = vPubs[j];
        size_t i = j;
        while (i > 0 && vPubs[i-1].n < key.n)
        { vPubs[i] = vPubs[i-1]; --i; }
        vPubs[i] = key;
    }
}

// [R175] 发布者日志的两个阈值。原在 AxLogPublisherTop 内写死 3（Top-N）
// 与 10（单厂商聚集告警），改用具名常量以免两处语义漂移。
inline constexpr int kAppxTopPublisherCount = 3;
inline constexpr int kAppxVendorClusterThreshold = 10;

void CPageAppx::AxLogPublisherTop(int nTab, const std::vector<AppxPkg>& v,
                                  const std::vector<CDuiString>& vSeenPub)
{
    // [R54] 发布者分组视图：统计 Top-3 发布者（按包含包数排序），日志输出
    std::vector<AxPubCount> vPubs;
    AxCountPublishers(v, vSeenPub, vPubs);   // [R175] 统计抽为函数
    AxSortPublishersByCount(vPubs);          // [R175] 排序抽为函数
    CDuiString sTop;
    sTop.Format(StringMgr.Get("misc.hc97a"), nTab);
    const int nShow = static_cast<int>(vPubs.size()) < kAppxTopPublisherCount
                    ? static_cast<int>(vPubs.size()) : kAppxTopPublisherCount;
    for (int i = 0; i < nShow; ++i)
    {
        CDuiString sItem;
        sItem.Format(L" %s=%d", vPubs[i].sName.GetData(), vPubs[i].n);
        sTop += sItem;
    }
    m_ctx.HostLog(sTop.GetData());
    // [R54] 单厂商标记：任一发布者包数 >=10 → 提示厂商聚集
    if (nShow > 0 && vPubs[0].n >= kAppxVendorClusterThreshold)
    {
        CDuiString sWarn;
        sWarn.Format(StringMgr.Get("misc.h3237"),
                    vPubs[0].sName.GetData(), vPubs[0].n);
        m_ctx.HostLog(sWarn.GetData(), 1);
    }
}

// [R164] 从 UpdateCount 抽出：统计列表里已勾选的行数，并把总行数带出。
//   [R32] 过滤视图下行名 k 是**原缓存真实索引**（不连续），
//   所以不能按行号反查缓存 —— 必须遍历子控件找 CCheckBoxUI。这条约定两处共用。
//   返回勾选数；nTotal 由引用带出（无列表时为 0）。
[[nodiscard]] int CPageAppx::CntCheckedInList(const wchar_t* pszListName, int& nTotal)
{
    nTotal = 0;
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(
        m_ctx.HostFindControl(pszListName));
    if (!pList) return 0;
    nTotal = pList->GetCount();
    int nChecked = 0;
    for (int i = 0; i < nTotal; ++i)
    {
        CControlUI* pChild = pList->GetItemAt(i);
        if (!pChild) continue;
        CContainerUI* pRow = dynamic_cast<CContainerUI*>(pChild);
        if (!pRow) continue;
        for (int j = 0; j < pRow->GetCount(); ++j)
        {
            CCheckBoxUI* pCk = dynamic_cast<CCheckBoxUI*>(pRow->GetItemAt(j));
            if (pCk && pCk->GetCheck()) ++nChecked;
        }
    }
    return nChecked;
}

void CPageAppx::UpdateCount(int nTab)

{
    CTextUI* pCount = static_cast<CTextUI*>(m_ctx.HostFindControl(L"lbl_appx_count"));
    if (!pCount) return;
    int nChecked = 0, nTotal = 0;
    CDuiString sListName;
    sListName.Format(L"appx_list_%d", nTab);
    // [R164] 双层遍历抽为 CntCheckedInList，嵌套 5→2
    nChecked = CntCheckedInList(sListName.GetData(), nTotal);
    CDuiString sText;
    if (nTotal == 0) sText = L"";
    else if (nChecked > 0) sText.Format(StringMgr.Get("misc.s5d27f1ec"), nTotal, nChecked);
    else sText.Format(StringMgr.Get("misc.s056993c2"), nTotal);
    pCount->SetText(sText.GetData());
}
