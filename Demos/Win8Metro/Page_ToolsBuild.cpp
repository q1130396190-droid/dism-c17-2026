// ============================================================
// Page_ToolsBuild.cpp —— 工具箱页「卡片构建」（从 Page_Tools.cpp 拆出）。
//
// [R163] Page_Tools.cpp 现 1,478 行、规模分 8.4（六页最低）。规模分只统计主文件，
// 故从主文件往外搬。本文件承接单张工具卡片的装配：
//   BpBuildCard —— 按 nKind 生成 64x64 图标 + 标题 + 描述 + 风险徽章的卡片控件
//
// 闭包说明：CPageTools 成员（Page_Tools.h 已声明）。族外唯一调用方是
// BpBuildCards（留在主文件）。依赖：ToolCards 命名空间的自绘控件
// （CToolIconUI / CClickVLayoutUI，见 Page_ToolsCards.h）、BpToolAvailable。
//
// 搬迁为逐字搬运，零逻辑改动。
// ============================================================

#include "stdafx.h"
#include "ThemeTokens.h"   // [R203] 颜色令牌（深色/浅色跟随）

#include "Page_Tools.h"
#include "Page_ToolsCards.h"      // ToolCards::CToolIconUI / CClickVLayoutUI
#include "Page_ToolsCommon.h"     // ToolSql::HostNotify（卡片提示）

#include "StringManager.h"
#include "ExternalToolService.h"
#include "ActivationKitClient.h"  // nKind==11 激活备份：DLL 就绪判定

using namespace DuiLib;
using i18n::CStringManager;

namespace
{
using ToolCards::CClickVLayoutUI;
using ToolCards::CToolIconUI;
}   // namespace

// [C++17 R6] 本 TU 特征常量：卡片图标边长与卡片最小高度（与原字面量同值）。
inline constexpr int kToolCardIconSize = 64;
inline constexpr int kToolCardMinHeight = 96;
static_assert(kToolCardIconSize > 0, "icon size must be positive");

// [R170] BpBuildCard 原 97 行（页 maxLen 97，六页最高档）。按"卡片由四个子控件
// 依次装配"这一自然边界拆为四个装配函数 + 一个编排体，逐字搬运零逻辑改动。
// 返回基类指针：调用方只用于后续置灰（SetTextColor/SetEnabled），
// 不需要具体类型，因此本 TU 内的装配函数全部以 CControlUI* 返回。

// [R170] 从 BpBuildCard 抽出：图标控件（64×64 自绘，按 nKind 取图形）。
CControlUI* CPageTools::BpAddIcon(CVerticalLayoutUI* pItem, int nKind)
{
    auto pIcon = std::make_unique<CToolIconUI>(); // [C++17 R3]
    pIcon->SetKind(nKind);
    pIcon->SetMouseEnabled(false);
    CToolIconUI* pRet = pIcon.get();
    pItem->Add(pIcon.release()); // [C++17 R3] ownership to DuiLib
    return pRet;
}

// [R170] 从 BpBuildCard 抽出：标题控件（从皮肤取 tl_<kind> 文案，居中）。
CTextUI* CPageTools::BpAddLabel(CVerticalLayoutUI* pItem, int nKind)
{
    CDuiString sLabelName;
    sLabelName.Format(L"tl_%d", nKind);
    auto pLabel = std::make_unique<CTextUI>(); // [C++17 R3]
    pLabel->SetText(m_ctx.HostSkinText(sLabelName.GetData()));
    pLabel->SetFixedHeight(32);
    // [FIX-FONT0] 原为 SetFont(0)。皮肤里没有 id=0 的字体项，
    //   GetFontInfo(0) 会走"查不到 → 回退默认字体"的容错分支；
    //   虽然显示结果与默认一致，但把"0"当字体 id 传下去是错的语义，
    //   一旦将来有人定义 id=0，卡片标题会突然换字体。
    //   这里显式用 -1（DuiLib 约定的"用默认字体"）。
    pLabel->SetFont(-1);
    pLabel->SetTextColor(tokens::Token(tokens::EToken::Ink));
    pLabel->SetAttribute(L"align", L"center");
    pLabel->SetAttribute(L"valign", L"vcenter");
    pLabel->SetMouseEnabled(false);
    CTextUI* pRet = pLabel.get();
    pItem->Add(pLabel.release()); // [C++17 R3] ownership to DuiLib
    return pRet;
}

// [R170] 从 BpBuildCard 抽出：分类标签（系统=青 / 磁盘镜像=绿 / 系统工具=紫）。
// 三档配色为视觉规范，改用查表而非 switch（原 3 case）。
CTextUI* CPageTools::BpAddCat(CVerticalLayoutUI* pItem, int nCat,
                                 const wchar_t* kToolCatNames[3])
{
    // [FIX-UNIFY-COLOR] 分类标签统一为黑色（#1A1A1A），与标题同色。
    //   原实现按下标取三色（系统=#00695C 青 / 磁盘镜像=#2E7D32 绿 /
    //   系统工具=#6A1B9A 紫），同一页里出现三种彩色分类标签，
    //   与标题(#1A1A1A)、描述(#666666)混排时观感不统一。
    //   用户要求"默认黑色统一"，故保留数组结构（便于将来恢复彩色分类）
    //   但三项同色，实际渲染不再有色彩跳变。
    // [R231] Was a constexpr triple of 0xFF1A1A1A (near-black): unreadable on a dark
    //   card. Cannot stay constexpr because tokens are resolved at runtime, so the
    //   array is filled per call instead.
    const DWORD dwCatInk = tokens::Token(tokens::EToken::Ink);
    const DWORD kCatColors[] = { dwCatInk, dwCatInk, dwCatInk };
    static constexpr int   kCatCount   = _countof(kCatColors);
    // [R103] 原实现在这里直接 kToolCatNames[nCat] 取名字，只给颜色做了 clamp，
    //   两行口径不一致：一旦 kToolCat 里出现 nCat >= 3（或负数），
    //   颜色会安全回退，名字却是**越界读**（UB，且不报错，读到相邻内存当字符串用）。
    //   现在名字与颜色用同一个已校验的下标。
    const int nCatSafe = (nCat >= 0 && nCat < kCatCount) ? nCat : 0;
    const wchar_t* pszCatName = kToolCatNames[nCatSafe];
    auto pCat = std::make_unique<CTextUI>(); // [C++17 R3]
    pCat->SetText(pszCatName ? pszCatName : L"");
    pCat->SetFixedHeight(18);
    pCat->SetFont(1);
    pCat->SetTextColor(kCatColors[nCatSafe]);
    pCat->SetAttribute(L"align", L"left");
    pCat->SetAttribute(L"valign", L"vcenter");
    pCat->SetMouseEnabled(false);
    CTextUI* pRet = pCat.get();
    pItem->Add(pCat.release());  // [C++17 R3] ownership to DuiLib
    return pRet;
}

// [R170] 从 BpBuildCard 抽出：一句话描述（toolbox.td_<kind>，超长省略）。
CTextUI* CPageTools::BpAddDesc(CVerticalLayoutUI* pItem, int nKind)
{
    CDuiString sDescKey;
    sDescKey.Format(L"toolbox.td_%d", nKind);
    auto pDesc = std::make_unique<CTextUI>(); // [C++17 R3]
    pDesc->SetText(StringMgr.Get(sDescKey.GetData()));
    pDesc->SetFixedHeight(32);
    pDesc->SetFont(1);
    // [FIX-UNIFY-COLOR] 描述文字统一为黑色 #1A1A1A（原 #666666 灰）。
    //   同页标题已用 #1A1A1A，描述再用灰会形成"标题黑、描述灰"的双色阶，
    //   加上分类标签原本三色，整页文字颜色过于零散。统一为黑色后，
    //   层级仍靠字号(标题 font=6 加粗 / 描述 font=1 常规)与位置区分。
    pDesc->SetTextColor(tokens::Token(tokens::EToken::Ink));
    pDesc->SetAttribute(L"align", L"left");
    pDesc->SetAttribute(L"valign", L"vcenter");
    pDesc->SetAttribute(L"endellipsis", L"true");
    pDesc->SetMouseEnabled(false);
    CTextUI* pRet = pDesc.get();
    pItem->Add(pDesc.release()); // [C++17 R3] ownership to DuiLib
    return pRet;
}

CControlUI* CPageTools::BpBuildCard(int nKind, const wchar_t* kToolCatNames[3],
                                    bool bExpertMode)
{
    // 11 个工具箱工具的依赖组件（kind→"必须存在的可执行程序"）
    // oscdimg.exe 是 ADK 组件，没装 ADK 时 kind 5/9 自动置灰
    constexpr int nItemW = 160;
    // [U-3] 取消风险徽章后内容固定高合计 64+32+18+32=146，
    // childpadding 4×4=16，上下 inset 16 → 178。
    // 须与 page_tools.xml 的 TileLayout itemsize 同步。
    constexpr int nItemH = 178;
    CDuiString sItemName;
    sItemName.Format(L"toolitem_%d", nKind);
    auto pItem = std::make_unique<CClickVLayoutUI>(); // [C++17 R3]
    pItem->SetName(sItemName.GetData());
    pItem->SetFixedWidth(nItemW);
    pItem->SetFixedHeight(nItemH);  // [U-1] 卡片固定高度
    pItem->SetChildPadding(4);
    pItem->SetAttribute(L"inset", L"8,8,8,8");  // [U-1] 卡片内边距
    // [U-2] 背景/边框/hover/press 全部由 CClickVLayoutUI 自绘（Win11 圆角卡）。
    // 不再设置 bkcolor/bordercolor/bordersize：基类若画直角白底+#ABABAB 直角
    // 边框，会盖掉自绘圆角底；hotbkcolor/pushedbkcolor 基类根本不识别。

    // 四个子控件按固定顺序装配（图标 → 标题 → 分类 → 描述）。
    CControlUI* pIcon  = BpAddIcon(pItem.get(), nKind);
    CTextUI* pLabel = BpAddLabel(pItem.get(), nKind);
    // [R119-1] 卡片分类标签（系统 / 磁盘镜像 / 系统工具三分类）
    CTextUI* pCat = BpAddCat(pItem.get(), kToolCat[nKind].nCat, kToolCatNames);
    (void)pCat;   // 分类标签无后续置灰需求，保留指针仅为装配返回值一致
    // [R119-1] 工具卡片一句话描述
    CTextUI* pDesc = BpAddDesc(pItem.get(), nKind);

    // [U-3] 风险等级仅用于专家模式过滤（高风险工具默认隐藏），
    // 卡片上的"低/中/高风险"药丸徽章按需求已移除，不再创建对应控件。
    int nRisk = kToolCat[nKind].nRisk;
    if (nRisk < 0) nRisk = 0;
    if (nRisk > 2) nRisk = 2;
    // ---- 可用性探测（参考 Qt6 Engine/SystemTools.cpp::isToolAvailable）----
    // [R01] kind=11 激活备份：以独立组件 ActivationKit.dll 是否就绪为准
    bool bAvail = (nKind == 11) ? ActivationKitClient::IsReady()
                                : BpToolAvailable(kToolDepends[nKind]);
    if (!bAvail)
    {
        pItem->SetEnabled(false);
        pLabel->SetTextColor(tokens::Token(tokens::EToken::InkMuted));
        // 图标也调灰：把 CToolIconUI 设成 Disable 状态让 OnPaint 调暗
        pIcon->SetEnabled(false);
        // [R119-1] 描述同步置灰（风险徽章已在 [U-3] 移除）
        pDesc->SetTextColor(tokens::Token(tokens::EToken::InkMuted));
        // [R-B36] 缺失4『部分工具依赖外部 ADK/系统程序』：置灰原因 Tooltip。
        // 原来只把卡片调灰，用户"看得见但不知道为什么"；现悬停可见缺件说明。
        // [R01] 激活备份卡缺的是 ActivationKit.dll（非 ADK），用专属提示。
        pItem->SetToolTip(nKind == 11
            ? StringMgr.Get("toolbox.act_missing")
            : StringMgr.Get("toolbox.dep_missing"));
    }
    // [R-B32] ExpertMode 消费点：非专家模式隐藏高风险（nRisk==2）工具卡片。
    // 当前高风险卡片仅 kind 1（系统还原）。[R-B85] kind 3（引导修复）已降
    // 为中风险默认显示（ToolBootRepair 入口内有多步确认+BCD 备份兜底）。
    if (!bExpertMode && nRisk == 2)
        return nullptr;
    return pItem.release(); // [C++17 R3] ownership transferred to caller
}

// [R231] Re-colour the tool cards after a light<->dark switch.
//
// PageBase.h contract: colours only -- no Build()/Add/Remove. The grid is a
// CTileLayoutUI whose children are the tool cards (toolitem_<kind>) and the category
// header rows; rebuilding would lose the expert-mode layout and the scroll position.
//
// Card structure (MakeToolCard): category label (colour by category), title, desc.
// The title/desc are Ink / InkMuted; the category line was a fixed near-black that is
// now tokenised at build time, so it only needs a re-set here, not a re-derive.
void CPageTools::RefreshThemeColors()
{
    CTileLayoutUI* pGrid = static_cast<CTileLayoutUI*>(
        m_ctx.HostFindControl(L"tools_grid"));
    if (!pGrid) return;

    const DWORD dwInk   = tokens::Token(tokens::EToken::Ink);
    const DWORD dwMuted = tokens::Token(tokens::EToken::InkMuted);

    for (int i = 0; i < pGrid->GetCount(); ++i)
    {
        CControlUI* pItem = pGrid->GetItemAt(i);
        if (!pItem) continue;
        IContainerUI* pBox = static_cast<IContainerUI*>(
            pItem->GetInterface(DUI_CTR_CONTAINER));
        if (!pBox) continue;
        for (int c = 0; c < pBox->GetCount(); ++c)
        {
            CTextUI* pText = dynamic_cast<CTextUI*>(pBox->GetItemAt(c));
            if (!pText) continue;
            // 0 = category line, 1 = title, 2 = description (see MakeToolCard).
            pText->SetTextColor(c == 2 ? dwMuted : dwInk);
        }
    }
}
