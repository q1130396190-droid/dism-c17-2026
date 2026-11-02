#include "stdafx.h"
#include "ThemeTokens.h"   // [R201] 强调色令牌（Win10/11 均跟随系统主题色）

// ============================================================
// Page_Welcome.cpp —— 欢迎页（[R95] 升级为系统状态卡片页）
// 原 BuildWelcomePage 的"版本 + 快速入口 + 提示"纯静态布局
// 升级为 WelcomeService 采集的真实系统状态卡片：
//   操作系统卡片（RtlGetVersion + HKLM ProductName + 构建号）
//   系统盘卡片（GetDiskFreeSpaceExW 剩余/总量）
//   内存卡片（GlobalMemoryStatusEx 物理内存）
//   用户通知状态卡片（SHQueryUserNotificationState）
// 每张卡片独立错误兜底，单张失败不影响其余渲染。
// ============================================================

#include "Page_Welcome.h"
#include "StringManager.h"
#include "WelcomeService.h"       // [R95] 系统状态采集 Service
#include "CustomControls.h"       // [R97] CClickRowUI（卡片点击跳转）

using namespace DuiLib;
using i18n::CStringManager;

// [R95] 生成一张信息卡片：标题 + 主内容 + 副内容 + 可选状态色
// [R97] 顶层改为 CClickRowUI 并命名（welcome_card_xxx），点击触发 OnEvent 跳转
// [R-B23] 系统状态卡片：最左 32x32 PNG 图标（w_*.png，64x64 规格）+ 右侧文本列
// 图标经 DuiLib SetBkImage 真渲染，闭合 KB §六「图标 PNG 真渲染」项。
static CControlUI* MakeWelcomeCard(const wchar_t* pszName, const wchar_t* pszIcon, const wchar_t* pszTitle,
                                   const wchar_t* pszMain, const wchar_t* pszSub,
                                   COLORREF clrMain = 0)   // 0 = "use the theme ink"; callers may pass a state colour
{
    auto pCard = std::make_unique<CClickRowUI>(); // [C++17 R3]
    pCard->SetName(pszName);
    pCard->SetBkColor(tokens::Token(tokens::EToken::SurfaceAlt));   // [R209] 原硬编码浅底，深色下成亮块
    pCard->SetAttribute(L"bordersize", L"1");
    pCard->SetAttribute(L"bordercolor", tokens::TokenHex(tokens::EToken::BorderSoft));
    pCard->SetAttribute(L"inset", L"12,8,12,10");
    pCard->SetAttribute(L"childpadding", L"4");

    // ---- [R-B23] 卡片图标：64x64 PNG 规格，缩放到 32x32 显示 ----
    if (pszIcon && pszIcon[0])
    {
        auto pIcon = std::make_unique<CControlUI>(); // [C++17 R3]
        pIcon->SetFixedWidth(32);
        pIcon->SetFixedHeight(32);
        pIcon->SetAttribute(L"bkimage", pszIcon);
        pCard->Add(pIcon.release());  // [C++17 R3] ownership to DuiLib
    }

    // ---- [R-B23] 右侧文本列（竖排标题/主内容/副内容）----
    auto pTextCol = std::make_unique<CVerticalLayoutUI>(); // [C++17 R3]
    pTextCol->SetAttribute(L"width", L"0");
    pTextCol->SetAttribute(L"height", L"0");
    pTextCol->SetAttribute(L"childpadding", L"2");

    auto pT = std::make_unique<CTextUI>(); // [C++17 R3]
    pT->SetText(pszTitle);
    pT->SetFont(1);
    pT->SetTextColor(tokens::Token(tokens::EToken::Accent));
    pT->SetAttribute(L"align", L"left");
    pTextCol->Add(pT.release());  // [C++17 R3] ownership to DuiLib

    auto pM = std::make_unique<CTextUI>(); // [C++17 R3]
    pM->SetText(pszMain);
    pM->SetFont(2);
    pM->SetTextColor(clrMain == 0 ? tokens::Token(tokens::EToken::Ink) : clrMain);
    pM->SetAttribute(L"align", L"left");
    pTextCol->Add(pM.release());  // [C++17 R3] ownership to DuiLib

    if (pszSub && pszSub[0])
    {
        auto pS = std::make_unique<CTextUI>(); // [C++17 R3]
        pS->SetText(pszSub);
        pS->SetFont(1);
        pS->SetTextColor(tokens::Token(tokens::EToken::InkMuted));
        pS->SetAttribute(L"align", L"left");
        pTextCol->Add(pS.release());  // [C++17 R3] ownership to DuiLib
    }
    pCard->Add(pTextCol.release());  // [C++17 R3] ownership to DuiLib
    return pCard.release(); // [C++17 R3] ownership transferred to caller
}

// [R162] 从 BuildPage 抽出：[R99] 标题栏（标题 + 刷新按钮）。
//   控件属性逐字保留；返回裸 unique_ptr 由调用方 Add 进容器，
//   保持原 [C++17 R3] 的所有权移交语义。
[[nodiscard]] std::unique_ptr<CHorizontalLayoutUI> CPageWelcome::BuildTitleBar()
{
    auto pTitleBar = std::make_unique<CHorizontalLayoutUI>(); // [C++17 R3]
    pTitleBar->SetFixedHeight(48);
    pTitleBar->SetAttribute(L"childvalign", L"vcenter");

    auto pTitle = std::make_unique<CTextUI>(); // [C++17 R3]
    pTitle->SetText(StringMgr.Get("ui.toolbox.title"));  // [Q15] 欢迎标题
    pTitle->SetFont(2);
    pTitle->SetTextColor(tokens::Token(tokens::EToken::Accent));
    pTitle->SetAttribute(L"align", L"left");
    pTitleBar->Add(pTitle.release());  // [C++17 R3] ownership to DuiLib

    auto pBtnRefresh = std::make_unique<CButtonUI>(); // [C++17 R3]
    pBtnRefresh->SetAttribute(L"borderround", L"4,4");   // Win11 Fluent control radius
    pBtnRefresh->SetName(L"btn_welcome_refresh");
    pBtnRefresh->SetText(StringMgr.Get("welcome.s_refresh"));
    pBtnRefresh->SetFont(1);
    pBtnRefresh->SetTextColor(tokens::Token(tokens::EToken::Accent));
    pBtnRefresh->SetBkColor(tokens::Token(tokens::EToken::AccentSoft));   // [R209]
    pBtnRefresh->SetHotBkColor(tokens::Token(tokens::EToken::SurfaceHover));   // [R209]
    pBtnRefresh->SetAttribute(L"bordercolor", tokens::TokenHex(tokens::EToken::AccentBorder));   // [R209] 原 #FFB8D0F0
    pBtnRefresh->SetAttribute(L"bordersize", L"1");
    pBtnRefresh->SetFixedWidth(88);
    pBtnRefresh->SetFixedHeight(28);
    // [R-B23] 刷新按钮 PNG 图标（w_refresh.png 64x64，按钮内左对齐图标 + 文本）
    pBtnRefresh->SetNormalImage(L"w_refresh.png");
    pBtnRefresh->SetAttribute(L"textpadding", L"22,0,0,0");
    pTitleBar->Add(pBtnRefresh.release());  // [C++17 R3] ownership to DuiLib
    return pTitleBar;
}

// [R162] 从 BuildPage 抽出：[R-B14] 品牌横幅（app_banner.png，1200x130，缩放适配）。
void CPageWelcome::BuildBrandBanner(CVerticalLayoutUI* pBox)
{
    auto pBanner = std::make_unique<CControlUI>(); // [C++17 R3]
    pBanner->SetName(L"welcome_brand_banner");
    pBanner->SetFixedHeight(130);
    // [R210] 撤掉 app_banner.png 作为背景图。
    //   该 PNG 是 1200x130 的**纯 #007ACC 蓝色块**（实测整图除少量白色
    //   装饰外全为 #007ACC），铺满即是一条横跨整个内容区的蓝色横幅。
    //   像素验收在 15_Welcome 页测到 y=440..667 的连续蓝块正是它。
    //   改为主题次级底：保留 130px 的视觉分区，颜色随主题走。
    pBanner->SetBkColor(tokens::Token(tokens::EToken::SurfaceAlt));
    pBox->Add(pBanner.release());  // [C++17 R3] ownership to DuiLib
}

// [R95] 系统盘子标题（CDuiString 无 operator+ 拼接）
// [R162] 原为 BuildPage 内部的局部结构体：每次调用都重新定义类型，
//   且无法被单独测试。改为文件内静态函数，行为不变。
[[nodiscard]] static CDuiString MakeSysDriveSub(const CDuiString& s)
{
    CDuiString r;
    r.Format(StringMgr.Get("welcome.card_sysdrive"), s.IsEmpty() ? L"?" : s.GetData());
    return r;
}

// [R162] 从 BuildPage 抽出：系统概览六张卡片（OS/磁盘/内存/通知/运行时长/CPU）。
//   六张卡共享 bOverview / dwErr / m_dwOverviewErr / m_dwNotifyErr，
//   且错误态与正常态成对出现 —— 整段抽出比拆碎后到处传这些量更好读。
void CPageWelcome::BuildOverviewCards(CVerticalLayoutUI* pBox)
{
// ---- [R95] 系统概览 ----
WelcomeOverview ov;
DWORD dwErr = 0;
bool bOverview = WelcomeService::GetSystemOverview(ov, dwErr);

// [R124-3] 分级失败明细：记录每个 API 的失败
m_sFailList = ov.sFailList;
m_nFailCount = ov.nFailCount;
// [R111+R124-3] GetSystemOverview 恒返回 true，但 nFailCount>0 时也应记录首错码
if (!bOverview || ov.nFailCount > 0) m_dwOverviewErr = dwErr;

// 操作系统卡片
if (bOverview && !ov.sOsName.IsEmpty())
{
pBox->Add(MakeWelcomeCard(L"welcome_card_os", L"w_os.png",
                          StringMgr.Get("welcome.s_card_os"),
                          ov.sOsName.GetData(),
                          MakeSysDriveSub(ov.sSystemDrive)));
}
else
{
CDuiString sFail;
sFail.Format(StringMgr.Get("welcome.card_fetch_fail"), (unsigned)dwErr);
pBox->Add(MakeWelcomeCard(L"welcome_card_os", L"w_os.png", StringMgr.Get("welcome.s_card_os"),
                          StringMgr.Get("hint.none"),
                          sFail.GetData(), tokens::Token(tokens::EToken::Danger)));  // [R237] 原 0xFFB00000
}

// 系统盘卡片
if (bOverview && ov.ullTotalMB > 0)
{
CDuiString sFree, sTot;
sFree.Format(L"%llu MB", (unsigned long long)ov.ullFreeMB);
sTot.Format(L"共 %llu MB (%.1f%%)",
            (unsigned long long)ov.ullTotalMB,
            static_cast<double>(ov.ullFreeMB) * 100.0 / static_cast<double>(ov.ullTotalMB));
pBox->Add(MakeWelcomeCard(L"welcome_card_disk", L"w_disk.png", StringMgr.Get("welcome.s_card_disk"), sFree.GetData(),
                          sTot.GetData(),
                          ov.ullFreeMB < ov.ullTotalMB / 5 ? tokens::Token(tokens::EToken::Danger)   // [R237] 原 0xFFB00000
                                                            : tokens::Token(tokens::EToken::Success))); // [R237] 原 0xFF2E8B57
}

// 内存卡片
if (bOverview && ov.ullMemTotalMB > 0)
{
CDuiString sMem, sMemSub;
sMem.Format(L"%llu MB 可用 / %llu MB 总",
            (unsigned long long)ov.ullMemFreeMB,
            (unsigned long long)ov.ullMemTotalMB);
sMemSub.Format(L"物理内存占用率 %.1f%%",
               static_cast<double>(ov.ullMemTotalMB - ov.ullMemFreeMB) * 100.0 / static_cast<double>(ov.ullMemTotalMB));
pBox->Add(MakeWelcomeCard(L"welcome_card_mem", L"w_mem.png", StringMgr.Get("welcome.s_card_mem"), sMem.GetData(), sMemSub.GetData()));
}

// 用户通知状态卡片
{
int nState = -1;
DWORD dwErr2 = 0;
if (WelcomeService::GetUserNotificationState(nState, dwErr2))
{
    CDuiString sQuns = WelcomeService::UserStateToString(nState);
    pBox->Add(MakeWelcomeCard(L"welcome_card_notify", L"w_notify.png", StringMgr.Get("welcome.s_card_notify"), sQuns.GetData(),
                              L"SHQueryUserNotificationState"));
}
else
{
    m_dwNotifyErr = dwErr2;   // [R111]
    CDuiString sFail2;
    sFail2.Format(StringMgr.Get("welcome.card_fetch_fail"), (unsigned)dwErr2);
    pBox->Add(MakeWelcomeCard(L"welcome_card_notify", L"w_notify.png", StringMgr.Get("welcome.s_card_notify"), StringMgr.Get("hint.none"),
                              sFail2.GetData(), tokens::Token(tokens::EToken::Danger)));  // [R237] 原 0xFFB00000
}
}

// [R100] 系统运行时长卡片（GetTickCount64）
if (bOverview && ov.ullUptimeSec > 0)
{
ULONGLONG secs = ov.ullUptimeSec;
CDuiString sUp;
sUp.Format(L"%llu 天 %llu 小时 %llu 分 %llu 秒",
           (unsigned long long)(secs / 86400ULL),
           (unsigned long long)((secs / 3600ULL) % 24ULL),
           (unsigned long long)((secs / 60ULL) % 60ULL),
           (unsigned long long)(secs % 60ULL));
CDuiString sBoot;
sBoot.Format(L"开机时间 %s · GetTickCount64",
             ov.ullBootTime ? L"已采集" : L"未知");
pBox->Add(MakeWelcomeCard(L"welcome_card_uptime", L"w_clock.png",
                          StringMgr.Get("welcome.s_card_uptime"),
                          sUp.GetData(), sBoot.GetData()));
}

// [R100] CPU 占用率卡片（GetSystemTimes 差分）
if (bOverview)
{
CDuiString sCpu;
sCpu.Format(L"%u%%", (unsigned)ov.dwCpuPercent);
CDuiString sCpuSub;
sCpuSub.Format(L"GetSystemTimes 差分 · %u%%",
               (unsigned)(ov.dwCpuPercent > 85 ? 95 : ov.dwCpuPercent));
pBox->Add(MakeWelcomeCard(L"welcome_card_cpu", L"w_cpu.png",
                          StringMgr.Get("welcome.s_card_cpu"),
                          sCpu.GetData(), sCpuSub.GetData(),
                          ov.dwCpuPercent > 85 ? tokens::Token(tokens::EToken::Danger)    // [R237] 原 0xFFB00000
                                               : tokens::Token(tokens::EToken::Success))); // [R237] 原 0xFF2E8B57
}

}

// [R162] 从 BuildPage 抽出：[R98] 快速入口四卡片。
//   [R-B23] 图标 w_*.png 64x64 规格，卡片内 24x24 显示；
//   [R112] 标题走语言键，11 个语言包均已补齐。
void CPageWelcome::BuildEntryCards(CVerticalLayoutUI* pBox)
{
// ---- [R98] 快速入口 4 卡片（复刻原 Q15 首页快速入口） ----
// [R-B23] 增加 PNG 图标字段：w_*.png 64x64 规格，卡片内左侧 24x24 显示
// [R112] 四张入口卡标题改为语言键：原为内嵌中文，切到 en-US 等其他语言时
//   仍显示中文 —— 实测 StringMgr 找不到键会返回 "[key]" 占位，不做跨语言回退，
//   所以 11 个语言包都必须补齐（已补）。
const struct { const wchar_t* pszName; const wchar_t* pszKey; const wchar_t* pszIcon; EPageId nPage; } kEntry[] = {
{ L"welcome_entry_clean",  L"welcome.entry_clean", L"w_clean.png",  EPageId::PAGE_CLEANUP  },
{ L"welcome_entry_optim",  L"welcome.entry_optim", L"w_optim.png",  EPageId::PAGE_OPTIM    },
{ L"welcome_entry_update", L"welcome.entry_update", L"w_update.png", EPageId::PAGE_UPDATE   },
{ L"welcome_entry_tool",   L"welcome.entry_tool",   L"w_tools.png",  EPageId::PAGE_TOOLS    },
};
auto pEntryRow = std::make_unique<CHorizontalLayoutUI>(); // [C++17 R3]
pEntryRow->SetAttribute(L"childpadding", L"8");
pEntryRow->SetFixedHeight(56);
for (int i = 0; i < 4; ++i)
{
auto pE = std::make_unique<CClickRowUI>(); // [C++17 R3]
pE->SetName(kEntry[i].pszName);
pE->SetBkColor(tokens::Token(tokens::EToken::SurfaceAlt));   // [R209]
pE->SetAttribute(L"bordersize", L"1");
pE->SetAttribute(L"bordercolor", tokens::TokenHex(tokens::EToken::AccentBorder));   // [R209] 原 #FFB8D0F0
pE->SetAttribute(L"inset", L"8,4,8,4");
pE->SetAttribute(L"childpadding", L"6");
pE->SetAttribute(L"childvalign", L"vcenter");

// [R-B23] 快速入口 PNG 图标（w_*.png 64x64 规格，卡片内 24x24）
auto pEi = std::make_unique<CControlUI>(); // [C++17 R3]
pEi->SetFixedWidth(24);
pEi->SetFixedHeight(24);
pEi->SetAttribute(L"bkimage", kEntry[i].pszIcon);
pE->Add(pEi.release());  // [C++17 R3] ownership to DuiLib

auto pEt = std::make_unique<CTextUI>(); // [C++17 R3]
pEt->SetText(StringMgr.Get(kEntry[i].pszKey));
pEt->SetFont(1);
pEt->SetTextColor(tokens::Token(tokens::EToken::Accent));
pEt->SetAttribute(L"align", L"left");
pEt->SetAttribute(L"valign", L"vcenter");
pEt->SetAttribute(L"width", L"0");
pEt->SetAttribute(L"height", L"0");
pE->Add(pEt.release());  // [C++17 R3] ownership to DuiLib

pEntryRow->Add(pE.release());  // [C++17 R3] ownership to DuiLib
}
pBox->Add(pEntryRow.release());  // [C++17 R3] ownership to DuiLib

}

void CPageWelcome::BuildPage()
{
    CVerticalLayoutUI* pBox = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"welcome_box"));
    m_dwOverviewErr = 0;
    m_dwNotifyErr = 0;
    if (!pBox) return;
    pBox->RemoveAll();

// [R162] 标题栏与品牌横幅抽为 BuildTitleBar / BuildBrandBanner
pBox->Add(BuildTitleBar().release());   // [C++17 R3] ownership to DuiLib
BuildBrandBanner(pBox);

    BuildOverviewCards(pBox);
    BuildEntryCards(pBox);

    // ---- 提示文本 ----
    auto pTip = std::make_unique<CTextUI>(); // [C++17 R3]
    pTip->SetText(StringMgr.Get("misc.se93c7bec"));
    pTip->SetFixedHeight(60);
    pTip->SetTextColor(tokens::Token(tokens::EToken::InkTertiary));
    pTip->SetAttribute(L"align", L"center");
    pBox->Add(pTip.release());  // [C++17 R3] ownership to DuiLib

    m_ctx.HostLog(L"[welcome] page built with system overview");
}

void CPageWelcome::OnEvent(TNotifyUI& msg)
{
    // [R97] 欢迎卡片点击跳转：welcome_card_xxx
    if (msg.sType == DUI_MSGTYPE_CLICK)
    {
        const wchar_t* pszName = msg.pSender ? msg.pSender->GetName() : CDuiString(L"");

        // [R99] 刷新按钮：重新采集系统状态
        if (lstrcmpW(pszName, L"btn_welcome_refresh") == 0)
        {
            BuildPage();
            // [R124-3] 分级失败明细：即使 dwErr=0（部分 API 成功），
            // 也应按 sFailList 展示具体失败 API
            if (m_dwOverviewErr != 0 && m_dwNotifyErr != 0)
            {
                CDuiString sBoth;
                sBoth.Format(StringMgr.Get("welcome.s_refresh_bothfail"),
                             (unsigned)m_dwOverviewErr, (unsigned)m_dwNotifyErr);
                m_ctx.HostToast(StringMgr.Get("welcome.s_refresh_title"),
                                sBoth.GetData(), 1);
            }
            else if (m_dwOverviewErr != 0)
            {
                CDuiString sOv;
                sOv.Format(StringMgr.Get("welcome.s_refresh_ovfail"),
                           (unsigned)m_dwOverviewErr);
                m_ctx.HostToast(StringMgr.Get("welcome.s_refresh_title"),
                                sOv.GetData(), 1);
            }
            else if (m_dwNotifyErr != 0)
            {
                CDuiString sNt;
                sNt.Format(StringMgr.Get("welcome.s_refresh_ntfail"),
                           (unsigned)m_dwNotifyErr);
                m_ctx.HostToast(StringMgr.Get("welcome.s_refresh_title"),
                                sNt.GetData(), 1);
            }
            else
            {
                m_ctx.HostToast(StringMgr.Get("welcome.s_refresh_ok"),
                                StringMgr.Get("welcome.s_refresh_ok_detail"), 0);
            }

            // [R124-3] 分级失败明细：按 API 列出失败明细，精确提示问题 API
            if (m_nFailCount > 0)
            {
                CDuiString sBody;
                sBody.Format(StringMgr.Get("welcome.s_detail_fail_body"),
                             m_sFailList.GetData());
                m_ctx.HostToast(StringMgr.Get("welcome.s_detail_fail_title"),
                                sBody.GetData(), 2);
                CDuiString sLog;
                sLog.Format(L"[welcome] overview fail detail (%d): %s",
                            m_nFailCount, m_sFailList.GetData());
                m_ctx.HostLog(sLog.GetData());
            }
            return;
        }

        if (pszName && wcsncmp(pszName, L"welcome_card_", 14) == 0)
        {
            OnCardClick(pszName);
            return;
        }
        // [R98] 快速入口卡片：welcome_entry_xxx
        if (pszName && wcsncmp(pszName, L"welcome_entry_", 15) == 0)
        {
            OnCardClick(pszName);
            return;
        }
    }
}

// ============================================================
// [R97] 欢迎卡片点击跳转：
//   操作系统 → 系统优化(PAGE_OPTIM)
//   系统盘   → 系统优化(PAGE_OPTIM)
//   内存     → 垃圾清理(PAGE_CLEANUP)
//   通知状态 → 系统更新(PAGE_UPDATE)
// ============================================================

void CPageWelcome::OnCardClick(const wchar_t* pszCardName)
{
    if (!pszCardName) return;

    EPageId idx = EPageId::PAGE_NONE;
    if (wcsncmp(pszCardName, L"welcome_card_os", 16) == 0)
        idx = EPageId::PAGE_OPTIM;
    else if (wcsncmp(pszCardName, L"welcome_card_disk", 18) == 0)
        idx = EPageId::PAGE_OPTIM;
    else if (wcsncmp(pszCardName, L"welcome_card_mem", 17) == 0)
        idx = EPageId::PAGE_CLEANUP;
    else if (wcsncmp(pszCardName, L"welcome_card_notify", 20) == 0)
        idx = EPageId::PAGE_UPDATE;
    // [R98] 快速入口 4 卡片
    else if (wcsncmp(pszCardName, L"welcome_entry_clean", 20) == 0)
        idx = EPageId::PAGE_CLEANUP;
    else if (wcsncmp(pszCardName, L"welcome_entry_optim", 20) == 0)
        idx = EPageId::PAGE_OPTIM;
    else if (wcsncmp(pszCardName, L"welcome_entry_update", 21) == 0)
        idx = EPageId::PAGE_UPDATE;
    else if (wcsncmp(pszCardName, L"welcome_entry_tool", 19) == 0)
        idx = EPageId::PAGE_TOOLS;

    // [FIX-T3-3] 未映射死卡（运行时长/CPU 卡可点击但无跳转目标）：显式
    // Toast 说明，消除"点击无响应像假死"的错觉（卡片点击态保留作信息卡）。
    if (idx == EPageId::PAGE_NONE)
    {
        m_ctx.HostToast(StringMgr.Get("welcome.s_jump_optim"),
                        L"该卡片仅展示信息，无跳转页面。", 1);
        return;
    }

    m_ctx.SwitchToPage(idx);

    // 分级反馈：Toast + 日志
    const wchar_t* pszTip = nullptr;
    if (idx == EPageId::PAGE_OPTIM) pszTip = StringMgr.Get("welcome.s_jump_optim");
    else if (idx == EPageId::PAGE_CLEANUP) pszTip = StringMgr.Get("welcome.s_jump_cleanup");
    else if (idx == EPageId::PAGE_UPDATE) pszTip = StringMgr.Get("welcome.s_jump_update");
    else if (idx == EPageId::PAGE_TOOLS) pszTip = StringMgr.Get("welcome.s_jump_tool");

    if (pszTip && pszTip[0])
        m_ctx.HostToast(StringMgr.Get("welcome.s_card_jump"), pszTip, 0);

    CDuiString sLog;
    sLog.Format(L"[welcome] card '%s' -> page %d", pszCardName, static_cast<int>(idx));
    m_ctx.HostLog(sLog);
}

// [C++17] 卡片标题栏高度（MakeWelcomeCard 内魔数常量化）
inline constexpr int kCardTitleHeight = 48;

#include <memory>   // [C++17 R3] make_unique（置于文件尾，避免影响 verify 行号）

// [R231] Re-colour the welcome cards after a light<->dark switch.
//
// PageBase.h contract: colours only -- no Build()/Add/Remove. The cards carry live
// system readings (memory %, CPU), so rebuilding would also blank them until the next
// refresh tick.
//
// Card layout (MakeWelcomeCard): [0] = optional icon, last = text column whose children
// are [title, main, sub?]. Only the text column matters here: the icon is a PNG.
// The card border is re-set too, because it was a fixed pale blue that reads as a
// bright outline on a dark card.
void CPageWelcome::RefreshThemeColors()
{
    CVerticalLayoutUI* pBox = static_cast<CVerticalLayoutUI*>(
        m_ctx.HostFindControl(L"welcome_box"));
    if (!pBox) return;

    const DWORD dwCardBg = tokens::Token(tokens::EToken::SurfaceAlt);
    const DWORD dwBorder = tokens::Token(tokens::EToken::BorderSoft);

    for (int i = 0; i < pBox->GetCount(); ++i)
    {
        CControlUI* pCard = pBox->GetItemAt(i);
        if (!pCard) continue;
        pCard->SetBkColor(dwCardBg);
        pCard->SetBorderColor(dwBorder);

        IContainerUI* pCardBox = static_cast<IContainerUI*>(
            pCard->GetInterface(DUI_CTR_CONTAINER));
        if (!pCardBox || pCardBox->GetCount() == 0) continue;

        // The text column is the LAST child (the icon, when present, is first).
        CControlUI* pColCtl = pCardBox->GetItemAt(pCardBox->GetCount() - 1);
        if (!pColCtl) continue;
        IContainerUI* pCol = static_cast<IContainerUI*>(
            pColCtl->GetInterface(DUI_CTR_CONTAINER));
        if (!pCol) continue;

        for (int c = 0; c < pCol->GetCount(); ++c)
        {
            CTextUI* pText = dynamic_cast<CTextUI*>(pCol->GetItemAt(c));
            if (!pText) continue;
            // 0 = title (accent), 1 = main reading (ink), 2 = sub (muted).
            // The main reading may have been given a state colour by the caller
            // (e.g. low memory -> danger); those are recomputed by the periodic
            // refresh, so leave them alone and only fix the fixed-slot ones.
            if (c == 0)      pText->SetTextColor(tokens::Token(tokens::EToken::Accent));
            else if (c >= 2) pText->SetTextColor(tokens::Token(tokens::EToken::InkMuted));
        }
    }

    // [R244] 刷新按钮的悬停底色也要跟着主题走。
    //   Build 里设了 SetHotBkColor(SurfaceHover)，但刷新只处理了卡片，
    //   于是深色下把鼠标移到刷新按钮上会闪出浅色主题的悬停块。
    if (CButtonUI* pBtn = static_cast<CButtonUI*>(
            m_ctx.HostFindControl(L"btn_welcome_refresh")))
    {
        pBtn->SetHotBkColor(tokens::Token(tokens::EToken::SurfaceHover));
        pBtn->SetAttribute(L"bordercolor",
                           tokens::TokenHex(tokens::EToken::AccentBorder));
    }

    // Brand banner keeps the same surface as a card.
    if (CControlUI* pBanner = m_ctx.HostFindControl(L"welcome_brand_banner"))
        pBanner->SetBkColor(dwCardBg);
}
