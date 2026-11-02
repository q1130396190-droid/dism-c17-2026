// ============================================================
// [R77] Capability 页“行构建组”独立编译单元
//
// 从 Page_Capability.cpp 切出的 6 个成员函数（函数体零逻辑改动）：
//   BlBuildRow            单条能力行（名称/描述/状态徽章/依赖徽章/卸载按钮）
//   BlBuildDepBadge       依赖徽章（无依赖返回 nullptr）
//   BlShowEmpty           空态（过滤无结果 / 默认空态）
//   BlLogDepSummary       依赖统计汇总日志
//   BlLogPageStatus       页面收尾状态条
//   BlUpdateVisualOverview FOD 状态可视化（概览文本 + 进度条）
//
// 切片外零调用（R14-03 拆解时即设计为 BuildList/RenderBatch 的内部构件），
// 依赖探查确认六个符号在切片外仅出现在注释与调用点，无其他编译单元引用。
// 显示名/描述解析已由 R77 迁至 Page_CapabilityResolve.cpp，此处经头文件声明调用。
// ============================================================

#include "stdafx.h"
#include "ThemeTokens.h"   // [R201] 强调色令牌（Win10/11 均跟随系统主题色）

#include "Page_Capability.h"
#include "AppxCapUpdService.h"   // CapEntry
#include "StringManager.h"       // StringMgr 本地化宏
#include "FrameWnd_Util.h"       // PrettyCapName

using namespace DuiLib;

// [R162] 从 BlBuildRow 抽出：四条信息行（名称 / 描述 / 状态徽章 / 依赖徽章）。
//   sDesc 由调用方传入，必须是"行高算式用的同一个解析结果" ——
//   R90 的教训：行高与"是否 Add 描述控件"若各解析一次，会出现
//   "算出 70px 却只渲染 2 个子项、行凭空多 18px 空白"。
void CPageCapability::BlBuildInfoLines(CVerticalLayoutUI* pInfo, const CapEntry& e,
                                       bool bInstalled, const CDuiString& sDesc)
{
    // 行 1：显示名（中文友好名优先，未命中再回退机械可读名）
    auto pName = std::make_unique<CTextUI>(); // [C++17 R3]
    pName->SetText(ResolveCapDisplayName(e.sIdentity));
    pName->SetFixedHeight(22);
    pName->SetFont(6);
    pName->SetTextColor(tokens::Token(tokens::EToken::Ink));
    pName->SetAttribute(L"valign", L"vcenter");
    pName->SetAttribute(L"endellipsis", L"true");
    pName->SetMouseEnabled(false);
    pInfo->Add(pName.release()); // [C++17 R3] ownership transferred to DuiLib

    // 行 2：描述（中文映射 / DISM Description；均无时不显示，不再暴露原始 Identity）
    if (!sDesc.IsEmpty())
    {
        auto pDesc = std::make_unique<CTextUI>(); // [C++17 R3]
        pDesc->SetText(sDesc);
        pDesc->SetFixedHeight(18);
        // [FONT-FIX 2026-10-25] 显式指定字号，不再依赖"继承默认值"。
        //   一旦某处父容器换了字体或在别的页面复用这段构建逻辑，字号就会漂。
        //   统一口径：名称 12 加粗（font 6），其余一律 12 常规（font 1）。
        pDesc->SetFont(1);
        pDesc->SetTextColor(tokens::Token(tokens::EToken::InkTertiary));
        pDesc->SetAttribute(L"valign", L"vcenter");
        pDesc->SetAttribute(L"endellipsis", L"true");
        pDesc->SetMouseEnabled(false);
        pInfo->Add(pDesc.release()); // [C++17 R3] ownership transferred to DuiLib
    }

    // 行 3：状态徽章（已安装 / 未安装）；DISM 若提供 Size 则合并到同一行，
    // 不再单独占一行显示恒为"未知"的无信息量内容。
    CDuiString sStateText = bInstalled ? StringMgr.Get("skin.lbl_installed")
                                       : StringMgr.Get("misc.hf45c");
    if (!e.sSize.IsEmpty())
    {
        sStateText += L" · ";
        sStateText += e.sSize;
    }

    auto pStateTag = std::make_unique<CTextUI>(); // [C++17 R3]
    pStateTag->SetText(sStateText);
    pStateTag->SetFixedHeight(18);
    pStateTag->SetFont(1);   // [FONT-FIX] 与名称同字号，仅颜色区分状态
    // [R222] Was hard-coded green/grey (0xFF2DA44E / 0xFF888888). Both are light-theme
    //   literals: on a dark surface the grey drops to about 2.4:1 and the state tag
    //   effectively disappears. Use the semantic tokens so the pair tracks the theme.
    pStateTag->SetTextColor(bInstalled ? tokens::Token(tokens::EToken::Success)
                                : tokens::Token(tokens::EToken::InkMuted));
    pStateTag->SetAttribute(L"valign", L"vcenter");
    pStateTag->SetAttribute(L"align", L"left");
    pStateTag->SetMouseEnabled(false);
    pInfo->Add(pStateTag.release()); // [C++17 R3] ownership transferred to DuiLib

    // [R45] 行 5：依赖徽章 —— 当 DISM 输出包含依赖项时展示数量与首条依赖名
    if (CTextUI* pDep = BlBuildDepBadge(e))
        pInfo->Add(pDep);
}

// [R162] 从 BlBuildRow 抽出：行尾的"卸载/安装"按钮。
//   [R87 显示修复] 尺寸 84×30：40px 高在 44~62px 的行里显得臃肿，
//   且"C 卸载"两字左右留白过多；84×30 与文字块高度接近，行看起来是一体的。
void CPageCapability::BlBuildActionButton(CHorizontalLayoutUI* pRow, size_t k,
                                          bool bInstalled, const CDuiString& sUninstall)
{
    auto pBtn = std::make_unique<CButtonUI>(); // [C++17 R3]
    pBtn->SetAttribute(L"borderround", L"4,4");   // Win11 Fluent control radius
    CDuiString sBtnName;
    sBtnName.Format(L"capun_%d", static_cast<int>(k));
    pBtn->SetName(sBtnName);

    if (bInstalled)
    {
        pBtn->SetText(sUninstall);
        pBtn->SetBkColor(tokens::Token(tokens::EToken::Accent));
        pBtn->SetHotBkColor(tokens::Token(tokens::EToken::AccentHot));
        pBtn->SetPushedBkColor(tokens::Token(tokens::EToken::AccentPushed));
    }
    else
    {
        pBtn->SetText(StringMgr.Get("edit.sb58c7549"));
        pBtn->SetBkColor(tokens::Token(tokens::EToken::Success));
        pBtn->SetHotBkColor(tokens::Token(tokens::EToken::Success));
        pBtn->SetPushedBkColor(tokens::Token(tokens::EToken::Success));
    }

    pBtn->SetFixedWidth(84);
    pBtn->SetFixedHeight(30);
    pBtn->SetTextColor(tokens::Token(tokens::EToken::Surface));
    pBtn->SetAttribute(L"align", L"center");
    pBtn->SetAttribute(L"valign", L"vcenter");
    pRow->Add(pBtn.release()); // [C++17 R3] ownership transferred to DuiLib
}

// [R14-03] 单条能力行：名称/描述/状态徽章/依赖徽章/卸载按钮。
void CPageCapability::BlBuildRow(CVerticalLayoutUI* pList, const CapEntry& e,
                                 size_t k, bool bInstalled,
                                 const CDuiString& sUninstall)
{
    auto pRow = std::make_unique<CHorizontalLayoutUI>(); // [C++17 R3]

    // [R87/R88 行高修复] 90 → 70。
    //   原值 90 的问题：内容只有 名称22 + 描述18 + 状态18 + childpadding 4
    //   ≈ 62px（无描述时约 44px），行被硬撑到 90，再叠 inset 上下各 6，
    //   每行多出 20~46px 空白 —— 截图里"行间大片留白、按钮浮在空白中间"就是这个。
    //   用户原怀疑"卸载按钮过大"，实测按钮 100×40 与 90 高的比例并不异常，
    //   根因是行高本身。
    // [R88 修正 R87] 不能用 SetMinHeight：去掉 FixedHeight 后行会被**拉伸**。
    //   依据：CVerticalLayoutUI::SetPos 里 `if (sz.cy == 0) nAdjustables++`，
    //   没有固定高度的子项被算作可伸缩项，按剩余空间均分 ——
    //   实测 8 行时每行被拉到 100px，比原来的 90 还大（改坏了）。
    //   所以必须给按内容算出的精确高度，不能靠 min 兜底。
    //   内容 = 名称22 + 描述18 + 状态18 + childpadding 2×2 = 62
    //        + inset 上下各 4 = 70
    //   （原值 90 比内容多 20~46px，是截图里"行间大片空白"的根因）
    //
    // [R89] 但 70 是"有描述、无依赖"这一种组合的高度。四种组合实际需要：
    //   无描述无依赖 50 / 无描述有依赖 66 / 有描述无依赖 70 / 有描述有依赖 86
    //   写死 70 会让"有描述+有依赖"的行把依赖徽章裁掉 16px。
    //   所以改为**按本行实际内容累加**，不再用单一常量。
    // [R90] 高度改由 CapRowHeight() 计算 —— 与 /BenchRender 基准共用同一份算式。
    //   原来这里和基准各写一份，R88 就出现过"基准没跟着改、量出来还是旧值"。
    //   描述是否存在要按**实际会渲染的文本**判断（sDescription 或本地化映射任一非空），
    //   与下面真正 Add 描述控件的条件保持一致，否则会算出 50 却渲染 3 个子项。
    //   [R90 修正] 判据必须与"真正 Add 描述控件的那个 if"字字相同：
    //   实际渲染只看 ResolveCapDescription 的返回值（见下方"行 2：描述"），
    //   若这里用 "sDescription 或 映射 任一非空"，会出现"算 70 却只渲染 2 项"，
    //   行凭空多 18px 空白 —— 与 R77 那次"两套实现必然漂移"是同一个错。
    const CDuiString sDescForH = ResolveCapDescription(e);
    const bool bHasDesc = !sDescForH.IsEmpty();
    const bool bHasDeps = (e.nRequiredDeps > 0 || e.nOptionalDeps > 0);
    pRow->SetFixedHeight(CapRowHeight(bHasDesc, bHasDeps));

    RECT rcInset = { 22, 4, 16, 4 };
    pRow->SetInset(rcInset);
    pRow->SetChildPadding(6);
    pRow->SetAttribute(L"childvalign", L"vcenter");

    // [R124-1] 行名：行级右键菜单命中测试用
    CDuiString sRowName;
    sRowName.Format(L"caprow_%d", static_cast<int>(k));
    pRow->SetName(sRowName.GetData());

    // [R162] 四条信息行抽为 BlBuildInfoLines（名称/描述/状态/依赖），
    //   边界沿用原注释里的"行 1 / 行 2 / 行 3 / 行 5"分段，不重新设计。
    //   sDescForH 仍由本函数算一次并传入 —— 绝不能在里面重新解析，
    //   否则会再次出现 R90 记录过的"行高算 70、实际只渲染 2 项"。
    auto pInfo = std::make_unique<CVerticalLayoutUI>(); // [C++17 R3]
    pInfo->SetFixedWidth(0);
    pInfo->SetChildPadding(2);
    BlBuildInfoLines(pInfo.get(), e, bInstalled, sDescForH);
    pRow->Add(pInfo.release()); // [C++17 R3] ownership transferred to DuiLib

    // [R162] 卸载/安装按钮抽为 BlBuildActionButton
    BlBuildActionButton(pRow.get(), k, bInstalled, sUninstall);

    pList->Add(pRow.release()); // [C++17 R3] ownership transferred to DuiLib

    // [R12] 写入行缓存：指针 + 预计算小写搜索串（Identity + 显示名）。
    if (k < m_vCapRowPtr.size())
    {
        m_vCapRowPtr[k] = pList->GetItemAt(pList->GetCount() - 1);
        CDuiString sSearch = e.sIdentity;
        sSearch += L' ';
        sSearch += ResolveCapDisplayName(e.sIdentity);
        sSearch.MakeLower();
        m_vCapRowSearch[k] = sSearch;
    }
}

// [R14-03] 依赖徽章构造；无任何依赖时返回 nullptr（调用方不加入行布局）。
CTextUI* CPageCapability::BlBuildDepBadge(const CapEntry& e)
{
    if (e.nRequiredDeps <= 0 && e.nOptionalDeps <= 0) return nullptr;

    CDuiString sDep = StringMgr.Get("misc.h61f9");
    CDuiString sNum;

    if (e.nRequiredDeps > 0)
    {
        sDep += StringMgr.Get("misc.h1a9c");
        sNum.Format(L"%d", e.nRequiredDeps);
        sDep += sNum;

        if (!e.sFirstRequiredDep.IsEmpty())
        {
            CDuiString sD = PrettyCapName(e.sFirstRequiredDep);
            if (sD.GetLength() > 32) sD = sD.Left(32);
            sDep += L" (";
            sDep += sD;
            sDep += L")";
        }
    }

    if (e.nOptionalDeps > 0)
    {
        if (e.nRequiredDeps > 0) sDep += L", ";
        sDep += StringMgr.Get("misc.h62f0");
        sNum.Format(L"%d", e.nOptionalDeps);
        sDep += sNum;

        if (!e.sFirstOptionalDep.IsEmpty())
        {
            CDuiString sD = PrettyCapName(e.sFirstOptionalDep);
            if (sD.GetLength() > 24) sD = sD.Left(24);
            sDep += L" (";
            sDep += sD;
            sDep += L")";
        }
    }

    auto pDep = std::make_unique<CTextUI>(); // [C++17 R3]
    pDep->SetText(sDep);
    pDep->SetFixedHeight(14);
    pDep->SetFont(1);   // [FONT-FIX] 依赖徽章同字号
    pDep->SetTextColor(tokens::Token(tokens::EToken::Warn));
    pDep->SetAttribute(L"valign", L"vcenter");
    pDep->SetAttribute(L"align", L"left");
    pDep->SetAttribute(L"endellipsis", L"true");
    pDep->SetMouseEnabled(false);
    return pDep.release(); // [C++17 R3] ownership returned to caller (BlBuildRow adds it to layout)
}

// [R14-03] 空态：过滤无结果显示带关键字的文案，否则显示默认空态。
void CPageCapability::BlShowEmpty(CVerticalLayoutUI* pList,
                                  const CDuiString& sFilter,
                                  const CDuiString& sEmpty)
{
    CDuiString sEmptyText;
    if (!sFilter.IsEmpty())
    {
        // 过滤后为空
        sEmptyText.Format(StringMgr.Get("misc.h3aef"), sFilter.GetData());
    }
    else
    {
        sEmptyText = sEmpty;
    }

    auto pEmpty = std::make_unique<CTextUI>(); // [C++17 R3]
    pEmpty->SetText(sEmptyText);
    pEmpty->SetFixedHeight(60);
    pEmpty->SetTextColor(tokens::Token(tokens::EToken::InkMuted));
    pEmpty->SetAttribute(L"align", L"center");
    pEmpty->SetAttribute(L"valign", L"vcenter");
    pEmpty->SetMouseEnabled(false);
    pList->Add(pEmpty.release()); // [C++17 R3] ownership transferred to DuiLib
}

// [R54] 依赖总览：统计全部能力的必需/可选依赖总数与"高依赖"能力数（必需依赖 >=2）
void CPageCapability::BlLogDepSummary()
{
    int nReq = 0, nOpt = 0, nHeavy = 0;
    for (size_t k = 0; k < m_vCaps.size(); ++k)
    {
        nReq += m_vCaps[k].nRequiredDeps;
        nOpt += m_vCaps[k].nOptionalDeps;
        if (m_vCaps[k].nRequiredDeps >= 2) ++nHeavy;
    }

    if (nReq > 0 || nOpt > 0 || nHeavy > 0)
    {
        CDuiString sDepLog;
        sDepLog.Format(StringMgr.Get("misc.h2b02"),
                       nReq, nOpt, nHeavy, static_cast<int>(m_vCaps.size()));
        m_ctx.HostLog(sDepLog.GetData());
    }
}

// [U-3] 页面收尾状态条：显示可选功能页完成情况
void CPageCapability::BlLogPageStatus()
{
    CDuiString sPageStatus;
    sPageStatus.Format(StringMgr.Get("misc.hcc20"), static_cast<int>(m_vCaps.size()));
    m_ctx.HostLog(sPageStatus.GetData());
}

// [R73] FOD 状态可视化：更新顶部概览文本 + 进度条
void CPageCapability::BlUpdateVisualOverview()
{
    int nTotal = static_cast<int>(m_vCaps.size());
    int nInstalled = 0, nReqDeps = 0, nOptDeps = 0;
    for (size_t k = 0; k < m_vCaps.size(); ++k)
    {
        if (lstrcmpiW(m_vCaps[k].sState.GetData(), L"Installed") == 0) ++nInstalled;
        nReqDeps += m_vCaps[k].nRequiredDeps;
        nOptDeps += m_vCaps[k].nOptionalDeps;
    }

    CDuiString sVisual;
    if (nTotal > 0)
    {
        sVisual.Format(StringMgr.Get("misc.s277edf57"),
                       nInstalled, nTotal, nReqDeps, nOptDeps);
    }
    else
    {
        sVisual = StringMgr.Get("misc.sf0890286");
    }

    CTextUI* pVis = static_cast<CTextUI*>(m_ctx.HostFindControl(L"lbl_cap_visual"));
    if (pVis) pVis->SetText(sVisual.GetData());

    CProgressUI* pBar = static_cast<CProgressUI*>(m_ctx.HostFindControl(L"cap_visual_progress"));
    if (pBar)
    {
        int nVal = (nTotal > 0) ? (int)((static_cast<double>(nInstalled) * 100.0) / static_cast<double>(nTotal)) : 0;
        pBar->SetValue(nVal);
    }
}

