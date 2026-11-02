// ============================================================
// Page_AssocRender.cpp —— 文件关联页「行渲染 + 分组统计」阶段。
//
// [R100] 从 Page_Assoc.cpp 拆出。这一段只关心「把已算好的分组数据
//   变成 assocrow_%d 控件行并把统计写进日志」，不读注册表、不分发消息。
//
// 拆分前后函数体逐字未改，仅搬迁位置。
// ============================================================

#include "stdafx.h"
#include "ThemeTokens.h"   // [R201] 强调色令牌（Win10/11 均跟随系统主题色）

#include "Page_Assoc.h"

#include "CustomControls.h"      // CClickRowUI（assocrow_%d 行）
#include "StringManager.h"       // StringMgr 本地化宏
#include "FrameWnd_Util.h"       // EnsureIconPngReady / Clipboard

#include <algorithm>
#include <memory>

using namespace DuiLib;
using i18n::CStringManager;

void CPageAssoc::AssocBuildRenderRows(
        const std::vector<DuiLib::CDuiString>& vGroupKey,
        const std::vector<DuiLib::CDuiString>& vProgId,
        const std::vector<DuiLib::CDuiString>& vAssocExe,
        CVerticalLayoutUI* pLive,
        int& nGroupCount, int& nExeResolved)
{
CDuiString sLastKey;


for (size_t k = 0; k < m_vAssocExt.size(); ++k)
{
    CDuiString sExt = m_vAssocExt[k];
    const CDuiString& sProgId = vProgId[k];
    const CDuiString& sKey = vGroupKey[k];
    const CDuiString& sExeName = vAssocExe[k];


    // [R41] 遇到新分组 → 插入分组标题行（不算作 assocrow_，保持索引对齐）
    if (sKey != sLastKey)
    {
        sLastKey = sKey;
        ++nGroupCount;
        auto pHead = std::make_unique<CClickRowUI>(); // [C++17 R3]
        CDuiString sHdrName;
        sHdrName.Format(L"aschdr_%d", nGroupCount);
        pHead->SetName(sHdrName.GetData());
        pHead->SetFixedHeight(24);
        RECT rcInsetH = { 10, 2, 10, 2 };
        pHead->SetInset(rcInsetH);
        pHead->SetBkColor(tokens::Token(tokens::EToken::SurfaceHover));
        pHead->SetMouseEnabled(false);
        auto pHeadTxt = std::make_unique<CTextUI>(); // [C++17 R3]
        CDuiString sHeadTitle = L"▸ ";
        sHeadTitle += sKey;
        sHeadTitle += StringMgr.Get("misc.h1295");
        pHeadTxt->SetText(sHeadTitle);
        pHeadTxt->SetFixedHeight(24);
        pHeadTxt->SetFixedWidth(0);
        pHeadTxt->SetFont(1);
        pHeadTxt->SetTextColor(tokens::Token(tokens::EToken::Accent));
        pHeadTxt->SetAttribute(L"valign", L"vcenter");
        pHeadTxt->SetAttribute(L"endellipsis", L"true");
        pHeadTxt->SetMouseEnabled(false);
        pHead->Add(pHeadTxt.release()); // [C++17 R3] ownership transferred to DuiLib
        pLive->Add(pHead.release()); // [C++17 R3] ownership transferred to DuiLib
    }


    auto pRow = std::make_unique<CClickRowUI>(); // [C++17 R3]
    CDuiString sRowName;
    sRowName.Format(L"assocrow_%d", static_cast<int>(k));
    pRow->SetName(sRowName.GetData());
    pRow->SetFixedHeight(28);
    RECT rcInset = { 10, 0, 10, 0 };
    pRow->SetInset(rcInset);
    pRow->SetChildPadding(10);


    auto pExt = std::make_unique<CTextUI>(); // [C++17 R3]
    pExt->SetText(sExt.GetData());
    pExt->SetFixedWidth(120);
    pExt->SetFont(6);
    pExt->SetTextColor(tokens::Token(tokens::EToken::Ink));
    pExt->SetAttribute(L"valign", L"vcenter");
    pRow->Add(pExt.release()); // [C++17 R3] ownership transferred to DuiLib


    auto pProg = std::make_unique<CTextUI>(); // [C++17 R3]
    // [R46] 若解析出关联的 exe 基名，优先展示"{ProgId} · {exe}"，让用户
    // 直观看到关联的目标程序；否则回退到"未关联"或纯 ProgId
    CDuiString sProgDisp;
    if (!sExeName.IsEmpty())
    {
        sProgDisp = sProgId.IsEmpty() ? sExeName : sProgId;
        if (!sProgId.IsEmpty())
        {
            sProgDisp += L" · ";
            sProgDisp += sExeName;
            ++nExeResolved;
        }
    }
    else
    {
        sProgDisp = sProgId.IsEmpty() ? CDuiString(StringMgr.Get("misc.s64ace489")) : sProgId;
    }
    pProg->SetText(sProgDisp.GetData());
    pProg->SetFixedWidth(0);
    pProg->SetTextColor(tokens::Token(tokens::EToken::InkSecondary));
    pProg->SetAttribute(L"valign", L"vcenter");
    pProg->SetAttribute(L"endellipsis", L"true");
    pRow->Add(pProg.release()); // [C++17 R3] ownership transferred to DuiLib


    pLive->Add(pRow.release()); // [C++17 R3] ownership transferred to DuiLib

}
}


void CPageAssoc::AssocBuildLogStats(
        const std::vector<DuiLib::CDuiString>& vAssocExe,
        int nGroupCount, int nExeResolved)
{
// [R41] 记录本次分组的类别数，供日志/统计使用
{
    CDuiString sAssocLog;
    sAssocLog.Format(L"[assoc] groups=%d exts=%d exe=%d",
                     nGroupCount, static_cast<int>(m_vAssocExt.size()), nExeResolved);
    m_ctx.HostLog(sAssocLog.GetData());
}


// [R56] 图标就绪统计：遍历所有已解析的 exe 名，查询图标缓存命中数
// 用户可以看到图标缓存的填充进度（首次访问=0，多次访问逐步累积）
{
    int nIconCacheReady = 0;
    int nIconTarget = 0;
    for (size_t k = 0; k < vAssocExe.size(); ++k)
    {
        if (vAssocExe[k].IsEmpty()) continue;
        ++nIconTarget;
        if (!EnsureIconPngReady(vAssocExe[k].GetData()).IsEmpty())
            ++nIconCacheReady;
    }
    if (nIconTarget > 0)
    {
        CDuiString sIconStat;
        sIconStat.Format(StringMgr.Get("misc.hb11b"),
                         nIconCacheReady, nIconTarget);
        m_ctx.HostLog(sIconStat.GetData());
    }


    // [R57] 按扩展名分组统计：前 8 个已解析 exe 名的 icon 缓存状态，
    // 让用户看到具体哪些扩展名关联的程序已具备图标缓存
    if (nIconTarget > 0)
    {
        CDuiString sBreak;
        sBreak = StringMgr.Get("misc.h2ae1");
        int nShown = 0;
        for (size_t k = 0; k < m_vAssocExt.size() && nShown < 8; ++k)
        {
            if (k >= vAssocExe.size() || vAssocExe[k].IsEmpty()) continue;
            CDuiString sStatus = EnsureIconPngReady(vAssocExe[k].GetData()).IsEmpty()
                                      ? L"-" : L"✓";
            sStatus += L" ";
            sStatus += m_vAssocExt[k];
            sStatus += L"=";
            sStatus += vAssocExe[k];
            sBreak += L" [";
            sBreak += sStatus;
            sBreak += L"]";
            ++nShown;
        }
        m_ctx.HostLog(sBreak.GetData());
    }
}


// [U-3] 页面收尾状态条：显示文件关联页完成情况
{
    CDuiString sPageStatus;
    sPageStatus.Format(StringMgr.Get("misc.h2492"), static_cast<int>(m_vAssocExt.size()));
    m_ctx.HostLog(sPageStatus.GetData());
}
}


// ============================================================
// [R230] 浅色 <-> 深色切换后重取令牌色。
//
// 契约（PageBase.h）：只改颜色属性。这里绝不 Build()/Add()/Remove() ——
// 那会清空两个列表、丢掉滚动位置，并让 m_pFocusCtrl 悬空。
//
// 两个决定性事实（见上面的 AssocBuildRenderRows）：
//   * 行内两个文本列（扩展名 / ProgId·exe）以及分组标题文本都**没有名字**，
//     只能用**位置**选列，按名字匹配会静默什么都不做。
//   * 行索引 k 是 m_vAssocExt 的真实下标，过滤后是**有空洞**的；因此不能
//     按 assocrow_<k> 逐个反查，而是遍历容器、跳过非行控件
//     （assoc_empty 空态提示 / 离线提示）。
// ============================================================
void CPageAssoc::RefreshThemeColors()
{
    const DWORD dwInk     = tokens::Token(tokens::EToken::Ink);
    const DWORD dwSub     = tokens::Token(tokens::EToken::InkSecondary);
    const DWORD dwMuted   = tokens::Token(tokens::EToken::InkMuted);
    const DWORD dwAccent  = tokens::Token(tokens::EToken::Accent);
    const DWORD dwOnAccent= tokens::Token(tokens::EToken::OnAccent);
    const DWORD dwSurface = tokens::Token(tokens::EToken::Surface);
    const DWORD dwHdrBk   = tokens::Token(tokens::EToken::SurfaceHover);

    const wchar_t* kListNames[] = { L"assoc_live_list", L"assoc_img_list" };
    for (int n = 0; n < 2; ++n)
    {
        CControlUI* pListCtl = m_ctx.HostFindControl(kListNames[n]);
        if (!pListCtl) continue;
        IContainerUI* pList =
            static_cast<IContainerUI*>(pListCtl->GetInterface(DUI_CTR_CONTAINER));
        if (!pList) continue;

        for (int i = 0; i < pList->GetCount(); ++i)
        {
            CControlUI* pKid = pList->GetItemAt(i);
            if (!pKid) continue;

            // 非行控件：assoc_empty 空态提示 / 离线提示（AssocBuildOfflineTip）。
            // 它们是直接挂在列表上的 CTextUI，无名或名 assoc_empty，统一走淡色。
            CClickRowUI* pRow = dynamic_cast<CClickRowUI*>(pKid);
            if (!pRow)
            {
                CTextUI* pLoose = dynamic_cast<CTextUI*>(pKid);
                if (pLoose) pLoose->SetTextColor(dwMuted);
                continue;
            }

            const wchar_t* pszName = pRow->GetName();
            const bool bHeader = (pszName && wcsncmp(pszName, L"aschdr_", 7) == 0);
            const bool bDataRow = (pszName && wcsncmp(pszName, L"assocrow_", 9) == 0);
            if (!bHeader && !bDataRow) continue;   // 未知行控件，不动

            // 选中态反解：CClickRowUI::SetRowSelected 在同值时提前返回，不会重上色，
            // 所以这里直接按 IsRowSelected() 选底色，避免抹掉高亮。
            const bool bSel = bDataRow && pRow->IsRowSelected();
            pRow->SetBkColor(bHeader ? dwHdrBk : (bSel ? dwAccent : dwSurface));

            IContainerUI* pInner =
                static_cast<IContainerUI*>(pRow->GetInterface(DUI_CTR_CONTAINER));
            if (!pInner) continue;

            // 列语义按位置（AssocBuildRenderRows）：
            //   aschdr_  → 0 = 分组标题（Accent）
            //   assocrow_→ 0 = 扩展名（Ink）、1 = ProgId·exe（InkSecondary）
            for (int c = 0; c < pInner->GetCount(); ++c)
            {
                CTextUI* pText = dynamic_cast<CTextUI*>(pInner->GetItemAt(c));
                if (!pText) continue;
                if (bHeader)
                    pText->SetTextColor(dwAccent);
                else if (bSel)
                    pText->SetTextColor(dwOnAccent);   // 压在强调色底上
                else
                    pText->SetTextColor(c == 0 ? dwInk : dwSub);
            }
        }
    }
}
