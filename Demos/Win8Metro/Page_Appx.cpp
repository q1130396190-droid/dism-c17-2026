#include "stdafx.h"
#include "ThemeTokens.h"   // [R203] 颜色令牌（深色/浅色跟随）

// ============================================================
// Page_Appx.cpp —— v2 P4 用户/预装 Appx 页
// 8 个原 CFrameWnd 函数物理搬迁，函数体零逻辑改动，宿主引用
// 改 m_ctx.Xxx；后台注册表枚举在 AppxCapUpdService（S6）。
// ============================================================

#include "Page_Appx.h"
#include "CustomControls.h"
#include "StringManager.h"
#include "FrameWnd_Util.h"
#include "FrameWnd_UiHelpers.h" // P8：ParsePrefix2 / EListCtx::LCX_APPX / 统一菜单
#include "FrameWnd_Messages.h"
#include "ExternalToolService.h" // [R67] LaunchSystem/IsElevated
#include "AppxDeployService.h"   // [R92] 部署动作 Service（卸载/备份/恢复）
#include "AppxRuntimeBridge.h"   // [P0-A1] WinRT PackageManager 原生桥
#include "OsVersionInfo.h"       // [R200] 分级日志：Win11 判定唯一来源
#include "SafeThread.h"          // 后台依赖查询线程异常边界
#include <shellapi.h>            // [R66] SHELLEXECUTEINFOW
#include <memory>                // [C++17 R3] make_unique

using namespace DuiLib;
using i18n::CStringManager;

#pragma comment(lib, "shell32.lib")

// AppxDepsMsg / AppxDepEntry 定义在 AppxCapUpdService.h（R-B25）

// [R3] F5 刷新：触发 Appx 枚举（异步，结果通过 WM_APPX_DATA 回传）+ 刷新日志
void CPageAppx::OnRefreshPage()
{
    StartEnumeration();
    m_ctx.HostLog(StringMgr.Get("misc.h7bca"));
}

void CPageAppx::StartEnumeration()

{
    if (m_bLoading) return;
    m_bLoading = true;
    m_ctx.HostSetCtrlText(L"sess_status",
                m_ctx.HostSkinText(L"lbl_appx_loading"));
    // [D2] 两个列表显示旋转环加载占位（枚举完成后由 ApplyFilter 替换）
    for (int tab = 0; tab < 2; ++tab)
    {
        CDuiString sListName;
        sListName.Format(L"appx_list_%d", tab);
        CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(
            m_ctx.HostFindControl(sListName.GetData()));
        if (pList)
        {
            pList->RemoveAll();
            CDuiString sRing;
            sRing.Format(L"appx_ring_%d", tab);
            pList->Add(m_ctx.HostMakeLoadingHint(
                m_ctx.HostSkinText(L"lbl_appx_loading"), sRing.GetData()));
        }
    }
    ::SetTimer(m_ctx.HostHwnd(), APPX_RING_TIMER_ID, 90, nullptr);
    // [R200] 枚举发起前先建好分类下拉框与分级映射日志：冷启动首屏即可用
    //   分类筛选，不必等后台枚举回来。
    AxInitSmartCombo();
    AppxCapUpdService::StartAppxEnumThread(m_ctx.HostHwnd());
}

void CPageAppx::OnData(AppxDataMsg* pData)

{
    // [FIX-T3-1] 载荷接管：kMsgOwnPage 契约"由 OnData 释放"但函数全程
    // 无 delete/unique_ptr —— 每次枚举泄漏一个 AppxDataMsg（含全部包
    // 字符串）。unique_ptr 就地接管，异常路径也保证释放。
    std::unique_ptr<AppxDataMsg> guard(pData);
    m_bLoading = false;
    ::KillTimer(m_ctx.HostHwnd(), APPX_RING_TIMER_ID);
    if (!pData)
    {
        // 枚举线程异常：两列表落为空态，避免旋转环永久停留
        ApplyFilter();
        return;
    }
    // A-2: 缓存原始数据供过滤使用
    m_vCache[0] = pData->vUser;
    m_vCache[1] = pData->vPre;
    // [R66] 依赖关系富化（异步 PowerShell 查询）
    EnrichAppxDependencies(pData->vUser, pData->vPre);
    // 渲染（应用当前过滤条件）
    ApplyFilter();
    CDuiString sLog;
    sLog.Format(L"[appx] user=%d provisioned=%d repoErr=%d preErr=%d enumErr=%d",
                static_cast<int>(pData->vUser.size()), static_cast<int>(pData->vPre.size()),
                pData->nRepoErr, pData->nPreErr, pData->nEnumErr);
    m_ctx.HostLog(sLog.GetData());
    // [U-3] 页面收尾状态条：显示程序和功能页完成情况
    {
        CDuiString sPageStatus;
        sPageStatus.Format(StringMgr.Get("misc.he333"),
                           static_cast<int>(pData->vUser.size()), static_cast<int>(pData->vPre.size()));
        m_ctx.HostLog(sPageStatus.GetData());
    }
    m_ctx.HostSetCtrlText(L"sess_status", m_ctx.HostStatusReady().GetData());
}

void CPageAppx::BuildList(int nTab, const std::vector<AppxPkg>& v)

{
    // [R200] 分类筛选生效时同样走 ApplyFilter 的过滤视图：
    //   否则“选了运行时框架”却仍渲染全量，下拉框看起来毫无作用。
    if (AxCurrentCategory() != EAppxCategory::All)
    {
        ApplyFilter();
        return;
    }
    CDuiString sListName;
    sListName.Format(L"appx_list_%d", nTab);
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(sListName.GetData()));
    if (!pList) return;
    pList->RemoveAll();
    if (v.empty())
    {
        AxBuildEmptyList(nTab);
        return;
    }
    for (size_t k = 0; k < v.size(); ++k)
    {
        AxBuildRow(nTab, static_cast<int>(k), v[k], pList);
    }
    // [R48] 系统内置包统计 + [R50] 唯一 Publisher 统计 + [R54] Top-3 发布者
    AxLogStats(nTab, v);
    UpdateCount(nTab);
}
void CPageAppx::AxBuildEmptyList(int nTab)

{
    CDuiString sListName;
    sListName.Format(L"appx_list_%d", nTab);
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(sListName.GetData()));
    if (!pList) return;
    auto pEmpty = std::make_unique<CTextUI>(); // [C++17 R3]
    pEmpty->SetText(StringMgr.Get("hint.no_content"));
    pEmpty->SetFixedHeight(48);
    pEmpty->SetTextColor(tokens::Token(tokens::EToken::InkMuted));
    pEmpty->SetAttribute(L"align", L"center");
    pEmpty->SetMouseEnabled(false);
    pList->Add(pEmpty.release()); // [C++17 R3] ownership transferred to DuiLib
}

// ============================================================
// [R200] 智能筛选：分级中文映射 + 分类
//
// 三个约定（改动前先读）：
//   1) 分类下拉框的项在 Win10 上不含任何 Win11 独有项 —— 与映射表的
//      分级门控同源（见 AppxSmartFilter.cpp 的 AppxMapTierApplies）；
//   2) 关键词匹配面是「中文名 / 原始包名 / 分类名」，因此搜“运行时”能命中
//      运行时框架类，不必让用户记住包名；
//   3) 勾选状态按行名 axck_<tab>_<k> 保留 —— 过滤只隐藏行，
//      重新显示后勾选仍在（这是“筛选不该丢用户操作”的底线）。
// ============================================================
EAppxCategory CPageAppx::AxCurrentCategory() const
{
    CComboUI* pCombo = static_cast<CComboUI*>(m_ctx.HostFindControl(L"appx_cat_combo"));
    if (!pCombo || pCombo->GetCount() == 0) return EAppxCategory::All;
    const int nSel = pCombo->GetCurSel();
    if (nSel <= 0) return EAppxCategory::All;
    return static_cast<EAppxCategory>(nSel);
}

bool CPageAppx::AxRowPasses(const AppxPkg& pkg, const CDuiString& sKwLower) const
{
    const AppxSmartInfo info = AppxSmartDescribe(pkg.sFull, pkg.bIsResource);
    return AppxSmartMatches(pkg.sFull, info, sKwLower, AxCurrentCategory());
}

void CPageAppx::AxLogMapTier()
{
    // 分级生效条数：Win10 上应等于通用表条数，Win11 上更多。
    // 现场只有一行日志，却能把“为何某个包没中文名”从猜变成核对。
    CDuiString sLog;
    if (osver::IsWindows11OrLater())
    {
        // [FIX-I18N] 语言包译文可能用 {0} 风格占位（非 printf），
        //   与 CDuiString::Format 的 %d 不兼容 → 日志原样输出 "{0}"。
        //   格式化前把 {0} 规范化为 %d，两种译文风格都能出正确数字。
        std::wstring sTpl = StringMgr.Get("appx.smart.s_osgate_on");
        for (size_t p; (p = sTpl.find(L"{0}")) != std::wstring::npos; )
            sTpl.replace(p, 3, L"%d");
        sLog.Format(sTpl.c_str(), AppxSmartActiveEntryCount());
    }
    else
        sLog = StringMgr.Get("appx.smart.s_osgate_off");
    m_ctx.HostLog(sLog.GetData());
}

void CPageAppx::AxInitSmartCombo()
{
    CComboUI* pCombo = static_cast<CComboUI*>(m_ctx.HostFindControl(L"appx_cat_combo"));
    AppxSmartFillCategoryCombo(pCombo);
    AxLogMapTier();
}

// [R171] AxBuildRow 原 100 行（页 maxLen 100，六页次高）。按「一行由勾选框 +
// 信息列 + 尺寸列装配」这一自然边界拆为四个装配函数 + 两个文本拼接函数，
// 逐字搬运零逻辑改动。文本拼接单独成函数是因为它们含 3~4 层条件分支
// （版本/路径/系统标记/Publisher），是本函数里唯一有判定密度的部分。

// [R171] 从 AxBuildRow 抽出：二行文本「v<版本>   ·   <路径>」。
// 分支逐字保留：无版本时整体退化为路径（路径也空则 "-"）；
// 有版本但无路径时尾部补 "   ·   -"。
CDuiString CPageAppx::AxFormatRowSub(const AppxPkg& pkg)
{
    CDuiString sSub;
    // R32: 二行 = 版本 + 路径（若解析到版本）
    if (!pkg.sVersion.IsEmpty())
    {
        sSub = L"v";
        sSub += pkg.sVersion;
        if (!pkg.sPath.IsEmpty())
        {
            sSub += L"   ·   ";
            sSub += pkg.sPath;
        }
        else sSub += L"   ·   -";
    }
    else
    {
        sSub = pkg.sPath.IsEmpty() ? CDuiString(L"-") : pkg.sPath;
    }
    return sSub;
}

// [R171] 从 AxBuildRow 抽出：三行文本「[⭐ ]<Owner>   ·   <Publisher>」。
// [R48] 系统内置包前置星标 + "SYSTEM" 兜底；[R50] 追加 Publisher。
CDuiString CPageAppx::AxFormatRowOwner(const AppxPkg& pkg)
{
    CDuiString sOwnerText;
    if (pkg.bSystem)
    {
        sOwnerText = L"⭐ ";
        sOwnerText += pkg.sOwner.IsEmpty() ? CDuiString(L"SYSTEM") : pkg.sOwner;
        sOwnerText += StringMgr.Get("misc.h82a8");
    }
    else
    {
        sOwnerText = pkg.sOwner.IsEmpty() ? CDuiString(L"-") : pkg.sOwner;
    }
    if (!pkg.sPublisher.IsEmpty())
    {
        sOwnerText += L"   ·   ";
        sOwnerText += pkg.sPublisher;
    }
    return sOwnerText;
}

// [R171] 从 AxBuildRow 抽出：勾选框（30px 固定宽，字形取自宿主的 0 号框）。
void CPageAppx::AxAddRowCheck(CHorizontalLayoutUI* pRow, int nTab, int k)
{
    auto pCheck = std::make_unique<CBoxCheckUI>(); // [C++17 R3] [BOX-CHECK] 自绘方框
    CDuiString sCkName;
    sCkName.Format(L"axck_%d_%d", nTab, k);
    pCheck->SetName(sCkName.GetData());
    StyleCheckBox(pCheck.get(), L"", 60, m_ctx.HostGlyphBox(0));
    pCheck->SetFixedWidth(30);
    pRow->Add(pCheck.release()); // [C++17 R3] ownership transferred to DuiLib
}

// [R171] 从 AxBuildRow 抽出：信息列（三行文本：完整包名 / 版本+路径 / Owner）。
void CPageAppx::AxAddRowInfo(CHorizontalLayoutUI* pRow, const AppxPkg& pkg)
{
    auto pInfo = std::make_unique<CVerticalLayoutUI>(); // [C++17 R3]
    pInfo->SetFixedWidth(0);
    pInfo->SetChildPadding(0);

    auto pFull = std::make_unique<CTextUI>(); // [C++17 R3]
    // [I18N-APPX-NAME] 首行优先显示本地化应用名（appx.name.<PackageName> 语言键），
    //   查不到才回退到原始 PackageFullName —— 既让普通用户看到"天气"而不是
    //   Microsoft.BingWeather_4.54…，又保证任何未知包都不会出现空标题。
    //   注意：这里不写任何中文，名称全部来自语言包。
    {
        // [R200] 三级解析：语言包译文 -> 分级中文映射（Win11 独有条目在
        //   Win10 上不会被命中，因为索引里根本没有它们）-> 原始包名。
        CDuiString sTitle = AppxDisplayName(pkg.sFull);
        if (sTitle.IsEmpty())
        {
            const AppxSmartInfo info = AppxSmartDescribe(pkg.sFull, pkg.bIsResource);
            sTitle = info.sName;
        }
        pFull->SetText(sTitle.IsEmpty() ? pkg.sFull.GetData() : sTitle.GetData());
    }
    pFull->SetFixedHeight(22);
    pFull->SetFont(6);
    pFull->SetTextColor(tokens::Token(tokens::EToken::Ink));
    pFull->SetAttribute(L"valign", L"vcenter");
    pFull->SetAttribute(L"endellipsis", L"true");
    pFull->SetMouseEnabled(false);
    pInfo->Add(pFull.release()); // [C++17 R3] ownership transferred to DuiLib

    auto pPath = std::make_unique<CTextUI>(); // [C++17 R3]
    CDuiString sSub = AxFormatRowSub(pkg);
    pPath->SetText(sSub.GetData());
    pPath->SetFixedHeight(20);
    pPath->SetTextColor(tokens::Token(tokens::EToken::InkTertiary));
    pPath->SetAttribute(L"valign", L"vcenter");
    pPath->SetAttribute(L"endellipsis", L"true");
    pPath->SetMouseEnabled(false);
    pInfo->Add(pPath.release()); // [C++17 R3] ownership transferred to DuiLib

    auto pOwner = std::make_unique<CTextUI>(); // [C++17 R3]
    CDuiString sOwnerText = AxFormatRowOwner(pkg);
    pOwner->SetText(sOwnerText.GetData());
    pOwner->SetFixedHeight(20);
    pOwner->SetTextColor(tokens::Token(tokens::EToken::Ink));
    pOwner->SetAttribute(L"valign", L"vcenter");
    pOwner->SetMouseEnabled(false);
    pInfo->Add(pOwner.release()); // [C++17 R3] ownership transferred to DuiLib

    pRow->Add(pInfo.release()); // [C++17 R3] ownership transferred to DuiLib
}

// [R171] 从 AxBuildRow 抽出：尺寸列（150px 右对齐；尺寸为 0 且无路径时留空）。
void CPageAppx::AxAddRowSize(CHorizontalLayoutUI* pRow, const AppxPkg& pkg)
{
    CDuiString sSize;
    if (pkg.ullSize == 0 && pkg.sPath.IsEmpty()) sSize = L"";
    else CCleanEngine::FormatSize(pkg.ullSize, sSize);
    auto pSize = std::make_unique<CTextUI>(); // [C++17 R3]
    pSize->SetText(sSize.GetData());
    pSize->SetFixedWidth(150);
    pSize->SetTextColor(tokens::Token(tokens::EToken::Ink));
    pSize->SetAttribute(L"align", L"right");
    pSize->SetAttribute(L"valign", L"vcenter");
    pSize->SetTextPadding({ 0, 0, 8, 0 });
    pSize->SetMouseEnabled(false);
    pRow->Add(pSize.release()); // [C++17 R3] ownership transferred to DuiLib
}

void CPageAppx::AxBuildRow(int nTab, int k, const AppxPkg& pkg, CVerticalLayoutUI* pList)
{
    auto pRow = std::make_unique<CClickRowUI>(); // [C++17 R3]
    CDuiString sRowName;
    // R32 fix: filter 场景下 k 是原缓存的真实索引
    sRowName.Format(L"axrow_%d_%d", nTab, k);
    pRow->SetName(sRowName);
    pRow->SetFixedHeight(76);
    RECT rcInset = { 12, 8, 10, 6 };
    pRow->SetInset(rcInset);
    pRow->SetChildPadding(10);

    AxAddRowCheck(pRow.get(), nTab, k);
    AxAddRowInfo(pRow.get(), pkg);
    AxAddRowSize(pRow.get(), pkg);

    // [D3] 整行 tooltip：长包名/路径悬停可见
    CDuiString sTip = pkg.sFull;
    if (!pkg.sPath.IsEmpty()) { sTip += L"\n"; sTip += pkg.sPath; }
    pRow->SetToolTip(sTip.GetData());
    pList->Add(pRow.release()); // [C++17 R3] ownership transferred to DuiLib
}

// [R166] 过滤视图的单行构建（从 ApplyFilter 内联 78 行抽出）。
// 注意：这与 AxBuildRow **不等价**，不可互相替换 ——
//   · 本函数 Owner 行只显示 sOwner（无系统星标、不拼 Publisher）
//   · AxBuildRow 的 Owner 行含 [R48] 系统星标、[R50] Publisher 追加
// 两者是历史上分叉出来的两套渲染口径，本轮只做「原样提取」，不合并
// （合并会改变过滤视图的显示内容，属行为变更，需单独评估）。
// nTab/k 为原缓存真实索引，保证 ShowDetail 能直接查 m_vCache。
// [R191] 过滤视图信息列的单行文本装配（三行共用）。
// AxBuildFilteredRow 原把「SetText/FixedHeight/Font/TextColor/valign/endellipsis/
// MouseEnabled + Add」各写三遍（约 24 行），差异只有高度、颜色、是否设 6 号字体。
// nFont < 0 表示不设字体（沿用控件默认），与原"不调 SetFont"等价。
// **不得**用来替换 AxAddRowInfo —— 那版 Owner 行含系统星标与 Publisher，
// 与过滤视图口径不同（R66 已判定不等价，见 AxBuildFilteredRow 上方注释）。
[[nodiscard]] static CTextUI* AxMakeInfoText(const wchar_t* pszText, int nHeight,
                                            DWORD dwColor, int nFont)
{
    auto p = std::make_unique<CTextUI>(); // [C++17 R3]
    p->SetText(pszText);
    p->SetFixedHeight(nHeight);
    if (nFont >= 0) p->SetFont(nFont);
    p->SetTextColor(dwColor);
    p->SetAttribute(L"valign", L"vcenter");
    p->SetAttribute(L"endellipsis", L"true");
    p->SetMouseEnabled(false);
    return p.release();
}

void CPageAppx::AxBuildFilteredRow(int nTab, size_t k, const AppxPkg& pkg,
                                   CVerticalLayoutUI* pList)
{
    const int tab = nTab;
    auto pRow = std::make_unique<CClickRowUI>(); // [C++17 R3]
    CDuiString sRowName;
    sRowName.Format(L"axrow_%d_%d", tab, static_cast<int>(k));  // k = 原缓存真实索引
    pRow->SetName(sRowName);
    pRow->SetFixedHeight(76);
    RECT rcInset = { 12, 8, 10, 6 };
    pRow->SetInset(rcInset);
    pRow->SetChildPadding(10);
    auto pCheck = std::make_unique<CBoxCheckUI>(); // [C++17 R3] [BOX-CHECK] 自绘方框
    CDuiString sCkName;
    sCkName.Format(L"axck_%d_%d", tab, static_cast<int>(k));
    pCheck->SetName(sCkName.GetData());
    StyleCheckBox(pCheck.get(), L"", 60, m_ctx.HostGlyphBox(0));
    pCheck->SetFixedWidth(30);
    pRow->Add(pCheck.release()); // [C++17 R3] ownership transferred to DuiLib
    auto pInfo = std::make_unique<CVerticalLayoutUI>(); // [C++17 R3]
    pInfo->SetFixedWidth(0);
    pInfo->SetChildPadding(0);
    // [R191] 三行文本改用 AxMakeInfoText（差异点即三个实参：高度/颜色/字体）
    // [I18N-APPX-NAME] 与 AxAddRowInfo 同口径：优先本地化名，未命中回退原始包名。
    // [R200] 与 AxAddRowInfo 同口径的三级解析；过滤视图额外把分类挂在
    //   名称后面（见下），这样用户按分类筛完后，每行仍能看出它属于哪一类。
    AppxSmartInfo info = AppxSmartDescribe(pkg.sFull, pkg.bIsResource);
    {
        CDuiString sLocalized = AppxDisplayName(pkg.sFull);
        if (!sLocalized.IsEmpty()) info.sName = sLocalized;
    }
    {
        CDuiString sTitle = info.sName.IsEmpty() ? pkg.sFull : info.sName;
        if (!info.sName.IsEmpty() && info.nCat != EAppxCategory::All)
        {
            sTitle += L"   ·   ";
            sTitle += info.sCategory;
        }
        pInfo->Add(AxMakeInfoText(sTitle.GetData(), 22, tokens::Token(tokens::EToken::Ink), 6));
    }
    // R32: 二行显示版本（若解析到）+ 路径
    CDuiString sSub;
    if (!pkg.sVersion.IsEmpty())
    {
        sSub = L"v";
        sSub += pkg.sVersion;
        if (!pkg.sPath.IsEmpty())
        {
            sSub += L"   ·   ";
            sSub += pkg.sPath;
        }
        else sSub += L"   ·   -";
    }
    else
    {
        sSub = pkg.sPath.IsEmpty() ? CDuiString(L"-") : pkg.sPath;
    }
    pInfo->Add(AxMakeInfoText(sSub.GetData(), 20, tokens::Token(tokens::EToken::InkTertiary), -1));
    // 三行：Owner（用户/SYSTEM）
    pInfo->Add(AxMakeInfoText(pkg.sOwner.IsEmpty() ? L"-" : pkg.sOwner.GetData(),
                              20, tokens::Token(tokens::EToken::Ink), -1));
    pRow->Add(pInfo.release()); // [C++17 R3] ownership transferred to DuiLib
    CDuiString sSize;
    if (pkg.ullSize == 0 && pkg.sPath.IsEmpty()) sSize = L"";
    else CCleanEngine::FormatSize(pkg.ullSize, sSize);
    auto pSize = std::make_unique<CTextUI>(); // [C++17 R3]
    pSize->SetText(sSize.GetData());
    pSize->SetFixedWidth(150);
    pSize->SetTextColor(tokens::Token(tokens::EToken::Ink));
    pSize->SetAttribute(L"align", L"right");
    pSize->SetAttribute(L"valign", L"vcenter");
    pSize->SetTextPadding({ 0, 0, 8, 0 });
    pSize->SetMouseEnabled(false);
    pRow->Add(pSize.release()); // [C++17 R3] ownership transferred to DuiLib
    CDuiString sTip = pkg.sFull;
    if (!pkg.sPath.IsEmpty()) { sTip += L"\n"; sTip += pkg.sPath; }
    pRow->SetToolTip(sTip.GetData());
    pList->Add(pRow.release()); // [C++17 R3] ownership transferred to DuiLib
}

void CPageAppx::ApplyFilter()

{
    CDuiString sKeyword = m_ctx.HostReadEditValue(L"appx_filter");
    CDuiString sKwLower = sKeyword;
    sKwLower.MakeLower();
    // [R200] 分类是第二个筛选维度：选「全部」且关键词为空时才走全量渲染。
    const EAppxCategory nCat = AxCurrentCategory();
    for (int tab = 0; tab < 2; ++tab)
    {
        // 无过滤 → 全量；有过滤 → 只渲染匹配的项，且行名保留原缓存真实索引
        if (sKeyword.IsEmpty() && nCat == EAppxCategory::All)
        {
            BuildList(tab, m_vCache[tab]);
            continue;
        }
        // 手动渲染过滤视图：k = 原缓存真实索引，便于 ShowDetail 直接查缓存
        CDuiString sListName;
        sListName.Format(L"appx_list_%d", tab);
        CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(sListName.GetData()));
        if (!pList) continue;
        pList->RemoveAll();
        bool bAny = false;
        for (size_t k = 0; k < m_vCache[tab].size(); ++k)
        {
            const AppxPkg& pkg = m_vCache[tab][k];
            // [R200] 分类 + 关键词统一走 AxRowPasses，渲染与计数不可能分叉。
            if (!AxRowPasses(pkg, sKwLower)) continue;
            bAny = true;
            AxBuildFilteredRow(tab, k, pkg, pList);
        }
        if (!bAny)
        {
            auto pEmpty = std::make_unique<CTextUI>(); // [C++17 R3]
            CDuiString sEmpty;
            if (sKeyword.IsEmpty())
            {
                // [R200] 只选了分类、没有关键词：不能说“没有包含「」的项”，
                //   否则空串拼进引号里像渲染故障。
                sEmpty = StringMgr.Get("appx.smart.s_empty_cat");
            }
            else
            {
                sEmpty = StringMgr.Get("misc.hf4c8");
                sEmpty += sKeyword;
                sEmpty += L"）";
            }
            pEmpty->SetText(sEmpty.GetData());
            pEmpty->SetFixedHeight(48);
            pEmpty->SetTextColor(tokens::Token(tokens::EToken::InkMuted));
            pEmpty->SetAttribute(L"align", L"center");
            pEmpty->SetMouseEnabled(false);
            pList->Add(pEmpty.release()); // [C++17 R3] ownership transferred to DuiLib
        }
        UpdateCount(tab);
    }
}

// [R155] 版本行文本拼装（从 ShowDetail 抽出）。
// 原实现三层嵌套：版本非空 → 拼 "版本:" → 发布者非空再拼 "发布者:"。
// 提取后主函数只做「取值 → 交给本函数 → SetText」。
CDuiString CPageAppx::AxFormatVersionLine(const AppxPkg& pkg)
{
    if (pkg.sVersion.IsEmpty()) return CDuiString(L"-");
    CDuiString sVer = StringMgr.Get("misc.h2117");
    sVer += pkg.sVersion;
    if (!pkg.sPublisher.IsEmpty())
    {
        sVer += StringMgr.Get("misc.hda91");
        sVer += pkg.sPublisher;
    }
    return sVer;
}

void CPageAppx::ShowDetail(int nTab, int k)
{
    (void)nTab;
    (void)k;
}

void CPageAppx::SelectAll()

{
    CTabLayoutUI* pPages = static_cast<CTabLayoutUI*>(m_ctx.HostFindControl(L"appx_pages"));
    int nTab = pPages ? pPages->GetCurSel() : 0;
    if (nTab < 0) nTab = 0;
    CDuiString sListName;
    sListName.Format(L"appx_list_%d", nTab);
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(sListName.GetData()));
    if (!pList) return;
    bool bAny = false;
    for (int k = 0; k < pList->GetCount(); ++k)
    {
        CHorizontalLayoutUI* pRow = static_cast<CHorizontalLayoutUI*>(pList->GetItemAt(k));
        if (!pRow) continue;
        CCheckBoxUI* pCheck = static_cast<CCheckBoxUI*>(pRow->GetItemAt(0));
        if (pCheck && pCheck->GetCheck()) { bAny = true; break; }
    }
    for (int k = 0; k < pList->GetCount(); ++k)
    {
        CHorizontalLayoutUI* pRow = static_cast<CHorizontalLayoutUI*>(pList->GetItemAt(k));
        if (!pRow) continue;
        CCheckBoxUI* pCheck = static_cast<CCheckBoxUI*>(pRow->GetItemAt(0));
        if (pCheck)
        {
            pCheck->SetCheck(!bAny);
            m_ctx.HostRefreshCheckText(pCheck);
        }
    }
    UpdateCount(nTab);   // [D3]
}

void CPageAppx::Delete()
{
    // [AUDIT-P2] 原 202 行 / CC=30，拆为 4 段：
    //   AxCollectChecked  采集勾选包并累计大小（原 47 行循环体）
    //   AxConfirmDelete   空选择提示 + 带磁盘占用的二次确认
    //   AxDeployRemove    按 Tab 分派卸载 / 移除预置包
    //   AxReportDeleteResult 成功/失败分级上报
    // 确认文案、退出码语义、Toast 分级均逐字保留。
    CTabLayoutUI* pPages = static_cast<CTabLayoutUI*>(m_ctx.HostFindControl(L"appx_pages"));
    int nTab = pPages ? pPages->GetCurSel() : 0;
    if (nTab < 0) nTab = 0;
    CDuiString sListName;
    sListName.Format(L"appx_list_%d", nTab);
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(sListName.GetData()));
    if (!pList) return;
    std::vector<CDuiString> vSel;
    ULONGLONG ullTotalSize = 0;   // [R39] 累计选中包的磁盘占用
    AxCollectChecked(pList, nTab, vSel, ullTotalSize);
    if (!AxConfirmDelete(vSel, ullTotalSize)) return;
    CDuiString sErr;
    const EAppxDeployResult nDeploy = AxDeployRemove(nTab, vSel, sErr);
    AxReportDeleteResult(nDeploy, vSel, sErr);
}

// [AUDIT-P2] 从 Delete 提取：采集勾选包全名 + 按行名反查 m_vCache 累计大小

// ============================================================
// P8：第二级事件路由（原 CFrameWnd::Notify Appx 页分支逐字迁入）
// ============================================================

// [R155] OnEvent 拆为「click 按钮表驱动」+「行点击/过滤/切页分派」两段。
// 原实现 69 行 / CC21：6 段按钮 if-else 链 + 3 处重复的 axrow_ 解析
// + selectchanged 内嵌两级前缀判定，全部压在同一函数里。
namespace
{
// 解析 axrow_<tab>_<k>；成功返回 true 并回填 tab/k。
[[nodiscard]] bool AxParseRowName(const wchar_t* pszName, int& nTab, int& nK)
{
    if (wcsncmp(pszName, L"axrow_", 6) != 0) return false;
    nTab = _ttoi(pszName + 6);
    const wchar_t* p = wcsrchr(pszName + 6, L'_');
    if (!p) return false;
    nK = _ttoi(p + 1);
    return true;
}
}   // namespace

// click 类按钮分派：按钮名 → 无参处理函数。
// 表放在成员函数体内：成员函数指针取地址在类作用域外是非法访问。
bool CPageAppx::AxDispatchClickButton(const CDuiString& sName)
{
    using BtnHandler = void (CPageAppx::*)();
    struct BtnRoute { const wchar_t* pszName; BtnHandler pfn; };
    static constexpr BtnRoute kBtns[] = {
        { L"btn_appx_selall",  &CPageAppx::SelectAll },     // 全选
        { L"btn_appx_delete",  &CPageAppx::Delete },        // 删除
        { L"btn_appx_install", &CPageAppx::AppxInstall },   // [R67] Installer 安装通道
        { L"btn_appx_backup",  &CPageAppx::Backup },        // [R81] 导出选中包
        { L"btn_appx_restore", &CPageAppx::Restore },       // [R81] 从 .msixbundle 恢复
    };
    for (const BtnRoute& r : kBtns)
    {
        if (sName != r.pszName) continue;
        (this->*r.pfn)();
        return true;
    }
    // A-2: 清除过滤（原误挂在 selectchanged，按钮发的是 click，此前为死分支）
    if (sName != L"btn_appx_clear") return false;
    CEditUI* pEdit = static_cast<CEditUI*>(m_ctx.HostFindControl(L"appx_filter"));
    if (pEdit) pEdit->SetText(L"");
    // [R200] “清除”要同时清掉分类：只清关键词而保留分类，用户会以为
    //   按钮没生效（列表仍是筛选后的样子）。
    CComboUI* pCombo = static_cast<CComboUI*>(m_ctx.HostFindControl(L"appx_cat_combo"));
    if (pCombo && pCombo->GetCount() > 0) pCombo->SelectItem(0);
    ApplyFilter();
    return true;
}

// selectchanged 分支：子 Tab 切换 / 生成的复选框字形同步。
void CPageAppx::AxHandleSelectChanged(TNotifyUI& msg, const CDuiString& sName, const wchar_t* pszName)
{
    // ---- sub tabs ----
    if (wcsncmp(pszName, L"appxtab_", 8) == 0)
    {
        const int idx = _ttoi(pszName + 8);
        CTabLayoutUI* p = static_cast<CTabLayoutUI*>(m_ctx.HostFindControl(L"appx_pages"));
        if (p) p->SelectItem(idx);
        UpdateCount(idx);   // [D3] 切页同步已选计数
        return;
    }
    // ---- generated checkboxes: keep box glyph in sync ----
    int a = -1, b = -1;
    if (!ParsePrefix2(sName, L"axck_", a, b)) return;
    CCheckBoxUI* p = static_cast<CCheckBoxUI*>(msg.pSender);
    m_ctx.HostRefreshCheckText(p);
    UpdateCount(a);   // [D3]
}

void CPageAppx::OnEvent(TNotifyUI& msg)
{
    CDuiString sName = msg.pSender ? msg.pSender->GetName() : CDuiString(L"");
    const wchar_t* pszName = sName.GetData();
    if (msg.sType == L"click")
    {
        if (AxDispatchClickButton(sName)) return;
        // A-1: Appx 行单击 → 详情（行已改为 CClickRowUI，点击发 click）
        int nTab = -1, nK = -1;
        if (AxParseRowName(pszName, nTab, nK)) ShowDetail(nTab, nK);
        return;
    }
    if (msg.sType == L"dbclick")
    {
        int nTab = -1, nK = -1;
        if (AxParseRowName(pszName, nTab, nK)) ShowDetail(nTab, nK);
        return;
    }
    if (msg.sType == L"textchanged")
    {
        // A-2: Appx 实时过滤
        if (sName == L"appx_filter") ApplyFilter();
        return;
    }
    // [R200] 分类下拉：DuiLib 的组合框选中项变化两条都发（itemselect 给控件，
    //   selectchanged 给窗口），两处都接才能保证键盘与鼠标操作都生效。
    if (msg.sType == L"itemselect" || msg.sType == L"selectchanged")
    {
        if (sName == L"appx_cat_combo")
        {
            ApplyFilter();
            return;
        }
    }
    if (msg.sType == L"selectchanged") AxHandleSelectChanged(msg, sName, pszName);
}

// P8：原 CFrameWnd WM_RBUTTONUP 通用列表命中块（appx_list_0/1）
// [R175] 从 OnContextMenu（38 行 / CC13）抽出：在 hit 控件向上回溯的祖先链里
// 找名字为 axrow_<tab>_<k> 的行，命中则以行号弹行级菜单。
// 返回 true 表示已消费（调用方须立即返回）。
// 语义逐字保留：只认 axrow_ 前缀；tab 必须 0/1、k 必须 >=0 才弹，
// 否则继续向上回溯（不因解析失败就放弃整条链）。
bool CPageAppx::AxTryRowContextMenu(CControlUI* pHit, POINT pt)
{
    for (CControlUI* q = pHit; q; q = q->GetParent())
    {
        const wchar_t* pn = q->GetName();
        if (!pn) continue;
        if (wcsncmp(pn, L"axrow_", 6) != 0) continue;
        const int tab = _ttoi(pn + 6);
        const wchar_t* p = wcschr(pn + 6, L'_');
        const int k = p ? _ttoi(p + 1) : -1;
        if (tab < 0 || tab >= 2 || k < 0) continue;
        ShowRowContextMenu(tab, k, pt);
        return true;
    }
    return false;
}

// [R175] 从 OnContextMenu 抽出：祖先链里是否命中某个列表容器。
// 命中则弹统一列表右键菜单（EListCtx::LCX_APPX，无行号）。
// 两个列表名做成表：原实现是「遍历祖先 × 遍历 2 个名字」的双层循环。
[[nodiscard]] bool CPageAppx::AxTryListContextMenu(CControlUI* pHit, POINT pt)
{
    static const wchar_t* kListNames[] = { L"appx_list_0", L"appx_list_1" };
    for (CControlUI* q = pHit; q; q = q->GetParent())
    {
        const wchar_t* pn = q->GetName();
        if (!pn) continue;
        for (const wchar_t* pszList : kListNames)
        {
            if (lstrcmpW(pn, pszList) != 0) continue;
            UiHelpers_ShowUnifiedListContextMenu(&m_ctx, m_ctx.HostHwnd(),
                                               EListCtx::LCX_APPX, -1, pt);
            return true;
        }
    }
    return false;
}

bool CPageAppx::OnContextMenu(POINT pt, bool bFromContextMsg)
{
    if (bFromContextMsg) return false;
    CControlUI* pHit = m_ctx.HostFindControlAt(pt);
    // [R123-1] 行级右键菜单：axrow_{tab}_{k} 命中 → 直接弹出行级菜单
    if (AxTryRowContextMenu(pHit, pt)) return true;
    // 列表空白处 → 统一列表菜单
    if (AxTryListContextMenu(pHit, pt)) return true;
    return false;
}

// ============================================================
// [R66] Appx 依赖关系富化：批量查询每个包的 PackageFullNames 依赖
// 走 PowerShell Get-AppxPackage -AllUsers | Select PackageName, PackageFullName, Depends
// ============================================================
// [R-B25][R24-FIX] Appx 依赖回读后台线程：原生 WinRT 枚举依赖列表，
// PostMessage(WM_APPX_DEPDATA) 回 UI。线程参数（栈上，本函数负责 delete）。
struct AppxDepThreadCtx
{
    HWND hNotify;
};

// [P0-A1] 依赖枚举已改原生 WinRT（AppxRuntimeBridge::EnumeratePackages），
// 旧 PowerShell Get-AppxPackage -AllUsers → TSV 通道与 UTF-8 解析助手已删除。

DWORD WINAPI CPageAppx::AppxDepThreadProc(void* pv)
{
    AppxDepThreadCtx* pCtx = static_cast<AppxDepThreadCtx*>(pv);
    if (!pCtx) return 1;
    HWND hNotify = pCtx->hNotify;
    // [C++17 R3] thread ctx: receiver takes ownership, freed at scope end (was: delete here; pCtx unused after)
    std::unique_ptr<AppxDepThreadCtx> ctxGuard(pCtx);
    // 原生 WinRT 枚举：PackageFullName + 依赖 PackageFullName 列表
    // [C++17 R3] msg ownership transferred to UI on post; unpoted path freed by unique_ptr
    auto pMsg = std::make_unique<AppxDepsMsg>();
    pMsg->nErr = 0;
    std::vector<AppxRuntimeBridge::PkgInfo> vPkgs;
    std::wstring sErr;
    if (AppxRuntimeBridge::EnumeratePackages(vPkgs, &sErr))
    {
        for (size_t k = 0; k < vPkgs.size(); ++k)
        {
            AppxDepEntry e;
            e.sFull = vPkgs[k].sFullName.c_str();
            std::wstring sDeps;
            for (size_t d = 0; d < vPkgs[k].vDeps.size(); ++d)
            {
                if (d) sDeps += L",";
                sDeps += vPkgs[k].vDeps[d];
            }
            e.sDeps = sDeps.c_str();
            pMsg->vDeps.push_back(e);
        }
    }
    else
    {
        pMsg->nErr = 3;
    }
    // [FIX-P1D] Post 成功才移交所有权：旧写法先 release 再 Post，
    // 投递失败（队列满/窗口竞态销毁）时 AppxDepsMsg 无人释放 = 泄漏。
    if (hNotify && ::IsWindow(hNotify) &&
        ::PostMessageW(hNotify, WM_APPX_DEPDATA, 0, msgparam::ToParam(pMsg.get()))) // [C++17 R3] ownership transferred to UI
        pMsg.release();
    return 0;
}

void CPageAppx::EnrichAppxDependencies(std::vector<AppxPkg>& vUser, std::vector<AppxPkg>& vPre)
{
    // [P0-A1/R24-FIX] 已改原生 WinRT 枚举，旧 PowerShell TSV 路径已完全删除
    auto pCtx = std::make_unique<AppxDepThreadCtx>();
    pCtx->hNotify = m_ctx.HostHwnd();
    HANDLE hThread = SafeCreateThread(AppxDepThreadProc, pCtx.get());
    if (hThread)
    {
        pCtx.release(); // [C++17 R3] ownership transferred to thread proc
        // 不 join，线程自行 PostMessage 后退出
        ::CloseHandle(hThread);
        m_ctx.HostLog(StringMgr.Get("appx.log.depquery_started"));
    }
    // 同步判定：资源包标记（包名含 ~W~~~<lang> 段）
    for (size_t k = 0; k < vUser.size(); ++k)
    {
        const CDuiString& s = vUser[k].sFull;
        if (s.Find(L"~W~~~") >= 0)
            vUser[k].bIsResource = true;
    }
    for (size_t k = 0; k < vPre.size(); ++k)
    {
        const CDuiString& s = vPre[k].sFull;
        if (s.Find(L"~W~~~") >= 0)
            vPre[k].bIsResource = true;
    }
    int nRes = 0;
    for (size_t k = 0; k < vUser.size(); ++k) if (vUser[k].bIsResource) ++nRes;
    CDuiString sStat;
    sStat.Format(StringMgr.Get("misc.s86d5c25a"),
                static_cast<int>(vUser.size()) - nRes, nRes);
    m_ctx.HostLog(sStat.GetData());
}


// ============================================================
// [R121-1] Appx 部署失败分级 Toast
// 按 AppxDeployService::ClassifyDeployError 返回的 HRESULT 等级，
// 针对常见 Appx 部署错误码展示差异化提示（D4 失败分级）。
// ============================================================
// [R155] HRESULT → 分级文案（从 ShowDeployFailToast 的 default 分支抽出）。
// 原实现是「外层 4 case + default 内嵌 8 case」两层 switch，CC 堆到 15。
// 抽出内层后外层只剩 5 个分支点，且两类错误码的文案映射各自可独立核对。
void CPageAppx::AxClassifyHResultText(const CDuiString& sErr,
                                      const wchar_t*& pszTitle, const wchar_t*& pszBody)
{
    const int nHr = AppxDeployService::ClassifyDeployError(sErr);
    switch (nHr)
    {
    case EAppxHResultKind::APPX_HR_ACCESS_DENIED:
        pszTitle = StringMgr.Get("appx.s_err_access");
        pszBody  = StringMgr.Get("appx.s_err_access_detail");
        return;
    case EAppxHResultKind::APPX_HR_ALREADY_INSTALLED:
        pszTitle = StringMgr.Get("appx.s_err_exists");
        pszBody  = StringMgr.Get("appx.s_err_exists_detail");
        return;
    case EAppxHResultKind::APPX_HR_DEPLOY_BLOCKED:
        pszTitle = StringMgr.Get("appx.s_err_blocked");
        pszBody  = StringMgr.Get("appx.s_err_blocked_detail");
        return;
    case EAppxHResultKind::APPX_HR_DEP_LOST:
        pszTitle = StringMgr.Get("appx.s_err_deplost");
        pszBody  = StringMgr.Get("appx.s_err_deplost_detail");
        return;
    case EAppxHResultKind::APPX_HR_PKG_UPDATE_NOT_ALLOWED:
        pszTitle = StringMgr.Get("appx.s_err_updnotallowed");
        pszBody  = StringMgr.Get("appx.s_err_updnotallowed_detail");
        return;
    case EAppxHResultKind::APPX_HR_LICENSE_NOT_FOUND:
        pszTitle = StringMgr.Get("appx.s_err_license");
        pszBody  = StringMgr.Get("appx.s_err_license_detail");
        return;
    case EAppxHResultKind::APPX_HR_INSUFF_MEMORY:
        pszTitle = StringMgr.Get("appx.s_err_mem");
        pszBody  = StringMgr.Get("appx.s_err_mem_detail");
        return;
    case EAppxHResultKind::APPX_HR_INVALID_ARG:
        pszTitle = StringMgr.Get("appx.s_err_arg");
        pszBody  = StringMgr.Get("appx.s_err_arg_detail");
        return;
    default:
        // 未识别的 HRESULT / 其它失败：保留原始错误文本
        return;
    }
}

void CPageAppx::ShowDeployFailToast(const wchar_t* pszAction,
                                    EAppxDeployResult nDeployCode,
                                    const CDuiString& sErr)
{
    (void)pszAction;   // 动作名仅用于调用点可读性，文案已按错误码分级
    if (nDeployCode == EAppxDeployResult::APPX_DEPLOY_OK) return;   // 成功无需提示
    // 1) Service 层本身的分级错误码（非 HRESULT）
    const wchar_t* pszTitle = StringMgr.Get("appx.s_del_fail");
    const wchar_t* pszBody  = sErr.IsEmpty() ? L"Appx operation failed" : sErr.GetData();
    const int nIcon = 1;
    switch (nDeployCode)
    {
    case EAppxDeployResult::APPX_DEPLOY_ERR_NOT_ELEVATED:
        pszTitle = StringMgr.Get("appx.s_err_not_elevated");
        pszBody  = StringMgr.Get("appx.s_err_not_elevated_detail");
        break;
    case EAppxDeployResult::APPX_DEPLOY_ERR_EMPTY_SELECTION:
        pszTitle = StringMgr.Get("appx.s_err_empty");
        pszBody  = StringMgr.Get("appx.s_err_empty_detail");
        break;
    case EAppxDeployResult::APPX_DEPLOY_ERR_ARG:
        pszTitle = StringMgr.Get("appx.s_err_arg");
        pszBody  = StringMgr.Get("appx.s_err_arg_detail");
        break;
    case EAppxDeployResult::APPX_DEPLOY_ERR_RUN_FAIL:
        pszTitle = StringMgr.Get("appx.s_err_run");
        pszBody  = StringMgr.Get("appx.s_err_run_detail");
        break;
    default:
        // EAppxDeployResult::APPX_DEPLOY_ERR_EXIT_NONZERO：进一步细分 HRESULT
        AxClassifyHResultText(sErr, pszTitle, pszBody);
        break;
    }
    m_ctx.HostToast(pszTitle, pszBody, nIcon);
}


// ============================================================
// [R5] 后台枚举结果统一入口（原壳 OnPgAppxData / OnPgAppxDepData）
// ============================================================

bool CPageAppx::OnAsyncMessage(UINT uMsg, [[maybe_unused]] WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_APPX_DATA:
        // kMsgOwnPage：AppxDataMsg* 由 OnData 释放。
        OnData((AppxDataMsg*)lParam);
        return true;
    case WM_APPX_DEPDATA:
        // kMsgOwnPage：AppxDepsMsg* 由 OnDepData 释放。
        OnDepData((AppxDepsMsg*)lParam);
        return true;
    default:
        return false;
    }
}

// [R229] Re-colour the Appx rows after a light<->dark switch.
//
// PageBase.h contract: colour attributes only. Calling Build()/Add/Remove here would
// drop the lists and the scroll position and leave m_pFocusCtrl dangling.
//
// Two facts decided the shape of this function:
//   * The four text cells of a row have NO name, so columns are selected by POSITION.
//   * The row index `k` is the real cache index, so a filtered view has GAPS -- probing
//     axrow_<tab>_<k> until the first miss would silently skip rows. Walk the list
//     container instead and skip anything that is not a row (spinner / empty hint).
void CPageAppx::RefreshThemeColors()
{
    const DWORD dwInk = tokens::Token(tokens::EToken::Ink);
    const DWORD dwSub = tokens::Token(tokens::EToken::InkTertiary);
    const DWORD dwRow = tokens::Token(tokens::EToken::Surface);

    for (int nTab = 0; nTab < 2; ++nTab)
    {
        CDuiString sListName;
        sListName.Format(L"appx_list_%d", nTab);
        CControlUI* pListCtl = m_ctx.HostFindControl(sListName.GetData());
        if (!pListCtl) continue;
        IContainerUI* pList =
            static_cast<IContainerUI*>(pListCtl->GetInterface(DUI_CTR_CONTAINER));
        if (!pList) continue;

        for (int i = 0; i < pList->GetCount(); ++i)
        {
            CControlUI* pRowCtl = pList->GetItemAt(i);
            if (!dynamic_cast<CClickRowUI*>(pRowCtl)) continue;   // spinner / empty hint
            IContainerUI* pRow =
                static_cast<IContainerUI*>(pRowCtl->GetInterface(DUI_CTR_CONTAINER));
            if (!pRow) continue;

            pRowCtl->SetBkColor(dwRow);
            // Row layout: 0 = checkbox, 1 = info column (name / version+path / owner),
            //             2 = size.
            if (pRow->GetCount() > 1)
            {
                CControlUI* pInfoCtl = pRow->GetItemAt(1);
                if (pInfoCtl)
                {
                    IContainerUI* pInfo = static_cast<IContainerUI*>(
                        pInfoCtl->GetInterface(DUI_CTR_CONTAINER));
                    if (pInfo)
                    {
                        for (int c = 0; c < pInfo->GetCount(); ++c)
                        {
                            CTextUI* pText = dynamic_cast<CTextUI*>(pInfo->GetItemAt(c));
                            if (pText) pText->SetTextColor(c == 1 ? dwSub : dwInk);
                        }
                    }
                }
            }
            if (pRow->GetCount() > 2)
            {
                CTextUI* pSize = dynamic_cast<CTextUI*>(pRow->GetItemAt(2));
                if (pSize) pSize->SetTextColor(dwInk);
            }
        }
    }
}
