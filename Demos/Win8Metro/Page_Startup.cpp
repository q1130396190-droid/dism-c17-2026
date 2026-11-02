#include "stdafx.h"
#include "ThemeTokens.h"   // [R203] 颜色令牌（深色/浅色跟随）
#include "SafeEnv.h"   // [B6] 安全环境变量读取：替代弃用 _wgetenv

// ============================================================
// Page_Startup.cpp —— v2 P3b 启动项/服务页
// 12 个原 CFrameWnd 函数的物理搬迁目标，函数体零逻辑改动，
// 仅把宿主引用从隐式 CFrameWnd 成员改为 m_ctx.Xxx（
// FindControl/Hwnd/Log/Alert/Glyph/DPI/ReadEdit）。
// 注册表枚举、StartupApproved 启停、SCM 枚举/启停、增删启动项
// 全部在 StartupService（S3）；错误文案映射保留在本页（StringMgr）。
// ============================================================

#include "Page_Startup.h"
#include "LogService.h"   // [M-5] 统一日志目录（读侧）

#include "IPageHost.h"             // [R5] HostPageHost()->PageById（任务子控件跨页转发）
#include "CustomControls.h"        // CClickRowUI（stcat_/strow_/svcrow_ 行）
#include "StringManager.h"         // StringMgr 本地化宏
#include "FrameWnd_Util.h"         // StyleCheckBox/ParsePrefix1/CopyTextToClipboard
#include "FrameWnd_UiHelpers.h"  // [R127-A] 行内关键词匹配 helper
#include "Win32DialogService.h"    // DlgOpenFile（新增启动项选文件）
#include "DiagnosticsService.h"    // SnapshotServicesToLog/ParseBootEventsToLog
#include "WmMessages.h"            // [R13 P0收敛] [LAG-FIX] WM_DIAG_LOG 定义在 WmMessages.h；UI 专属常量零使用
#include "SafeThread.h"           // 启动项诊断后台线程异常边界

// [LAG-FIX] 启动事件日志解析 / 服务全量快照都是重枚举（事件日志 +
// EnumServicesStatusEx 全类型），旧实现在 UI 线程随首次切页同步执行，
// 是"切到启动项页未响应"的直接原因之一。改为 fire-and-forget 后台线程，
// 完成后 PostMessage(WM_DIAG_LOG) 回 UI 只追加一行摘要。
struct StartupDiagArg { HWND hWnd; int nKind; };   // 0=启动事件 1=服务快照
static volatile LONG g_diagInFlight[2] = { 0, 0 };  // 同类诊断在途标志

// 线程体：C++ 异常内层捕获
static DWORD WINAPI StartupDiagThreadBody(void* lpParam)
{
    std::unique_ptr<StartupDiagArg> spArg(static_cast<StartupDiagArg*>(lpParam));  // [C++17 R3] 线程 ctx：函数退出自动释放
    StartupDiagArg* pArg = spArg.get();
    HWND hWnd = pArg->hWnd;
    int nKind = pArg->nKind;

    CDuiString sResult;
    try
    {
        sResult = (nKind == 0)
            ? DiagnosticsService::ParseBootEventsToLog()
            : DiagnosticsService::SnapshotServicesToLog();
    }
    catch (...)
    {
        sResult = L"";
    }

    if (sResult.IsEmpty() || !::IsWindow(hWnd)) return 0;

    // [C++17 阶段3] Post 消息所有权移交 UI，Post 失败自删
    auto pMsg = std::make_unique<CDuiString>(sResult); // [C++17 R3]
    if (!::PostMessageW(hWnd, WM_DIAG_LOG, 0, msgparam::ToParam(pMsg.get())))
        return 0;   // [C++17 R3] unique_ptr auto-frees on scope exit
    pMsg.release(); // [C++17 R3] ownership moved to UI thread (WM_DIAG_LOG receiver deletes)
    return 0;
}

// [R23-FIX] 线程外层过程：复位在途标志。
//   SafeThreadTrampoline 已有 SEH 边界 + SafeThreadDispatchCxx 有 C++ catch，
//   本函数的 __try/__except 永远捕获不到任何异常（冗余）。
//   这里直接调用并在函数尾部复位标志即可。
static DWORD WINAPI StartupDiagThreadProc(void* lpParam)
{
    int nKind = ((StartupDiagArg*)lpParam)->nKind;   // 参数在线程体内 delete
    SafeThreadDispatchCxx(StartupDiagThreadBody, lpParam);
    if (nKind >= 0 && nKind <= 1)
        ::InterlockedExchange(&g_diagInFlight[nKind], 0);
    return 0;
}

// [R75] 改为外部链接：Page_StartupService.cpp 也要调用。
void PostStartupDiagAsync(HWND hWnd, int nKind)
{
    if (!hWnd || nKind < 0 || nKind > 1) return;
    if (::InterlockedCompareExchange(&g_diagInFlight[nKind], 1, 0) != 0) return;  // 同类在途则跳过

    // [C++17 阶段3] 线程 ctx：所有权移交 StartupDiagThreadBody，创建失败自删
    auto pArg = std::make_unique<StartupDiagArg>(); // [C++17 R3]
    pArg->hWnd = hWnd;
    pArg->nKind = nKind;
    HANDLE hThread = SafeCreateThread(StartupDiagThreadProc, pArg.get());
    if (hThread) { ::CloseHandle(hThread); pArg.release(); } // [C++17 R3] ownership to thread   // [C++17 阶段2] SafeCreateThread 分离线程句柄：显式关闭（沿用 ProcessRunner 约定）
    else { ::InterlockedExchange(&g_diagInFlight[nKind], 0); }   // [C++17 R3] unique_ptr auto-frees
}

// [C++17 阶段2] 局部 RAII 守卫：文件句柄（见 WinHandles.h 使用规范）
struct PSFileGuard
{
    HANDLE h = INVALID_HANDLE_VALUE;
    PSFileGuard() = default;
    explicit PSFileGuard(HANDLE x) : h(x) {}
    ~PSFileGuard() { if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h); }
    PSFileGuard(const PSFileGuard&) = delete;
    PSFileGuard& operator=(const PSFileGuard&) = delete;
    operator HANDLE() const { return h; }
    [[nodiscard]] bool valid() const { return h && h != INVALID_HANDLE_VALUE; }
};


using namespace DuiLib;
using i18n::CStringManager;        // 便捷：直接写 CStringManager 不用前缀

// [R126-2] 启动项固定分类数（BuildStartup 的 cats 表长度）。
// 原 5 类（系统/用户/启动文件夹/本机/新用户），R126-2 追加组策略 Run 后为 6。
// [C++17 R3] inline constexpr：编译期常量 + 内部链接（原 static constexpr 语义等价，消除 ODR 陷阱）
inline constexpr int kStartupCatCount = 6;



// [R162] 从 BuildStartup 抽出：单个分类头（箭头 + 名称 + 位置）。
//   [FIX-ALIGN] inset.left 必须与数据行同为 28：原分类头是 6+18=24、数据行 28，
//   差 4px 导致分类名与勾选框文字整体错位；统一为 28 后落在同一竖线。
void CPageStartup::PsBuildCatHeader(CVerticalLayoutUI* pList, const PsCatDef& cat, int nCatIdx)
{
    auto pHeader = std::make_unique<CClickRowUI>(); // [C++17 R3]
    pHeader->SetFixedHeight(30);
    RECT rcHeaderInset = { 28, 0, 10, 0 };
    pHeader->SetInset(rcHeaderInset);
    pHeader->SetChildPadding(4);
    CDuiString sHdrName;
    sHdrName.Format(L"stcat_%d", nCatIdx);
    pHeader->SetName(sHdrName.GetData());

    auto pArrow = std::make_unique<CTextUI>(); // [C++17 R3]
    CDuiString sArName;
    sArName.Format(L"starrow_%d", nCatIdx);
    pArrow->SetName(sArName.GetData());
    pArrow->SetFixedWidth(18);
    pArrow->SetFont(9);
    pArrow->SetText(m_ctx.HostGlyphArrow(0));   // 默认全部折叠
    pArrow->SetTextColor(tokens::Token(tokens::EToken::InkMuted));
    pArrow->SetAttribute(L"align", L"left");
    pArrow->SetAttribute(L"valign", L"vcenter");
    pArrow->SetMouseEnabled(false);
    pHeader->Add(pArrow.release());  // [C++17 R3] ownership to DuiLib

    auto pCatName = std::make_unique<CTextUI>(); // [C++17 R3]
    pCatName->SetText(cat.pszName);
    pCatName->SetFont(6);
    // [FIX-ALIGN] 分类名不设固定宽度：若强行给 240（=数据行勾选框宽），
    //   名称右侧会留一大段空白，把"位置"推到 x≈500，与条目命令列完全错开。
    pCatName->SetFixedWidth(0);
    pCatName->SetTextColor(tokens::Token(tokens::EToken::Ink));
    pCatName->SetAttribute(L"valign", L"vcenter");
    pCatName->SetAttribute(L"endellipsis", L"true");
    pCatName->SetMouseEnabled(false);
    pHeader->Add(pCatName.release());  // [C++17 R3] ownership to DuiLib

    auto pDash = std::make_unique<CTextUI>(); // [C++17 R3]
    pDash->SetText(m_ctx.HostGlyphDash());
    pDash->SetFixedWidth(16);
    pDash->SetTextColor(tokens::Token(tokens::EToken::InkMuted));
    pDash->SetAttribute(L"align", L"left");
    pDash->SetAttribute(L"valign", L"vcenter");
    pDash->SetMouseEnabled(false);
    pHeader->Add(pDash.release());  // [C++17 R3] ownership to DuiLib

    auto pLoc = std::make_unique<CTextUI>(); // [C++17 R3]
    pLoc->SetText(cat.pszLocation);
    pLoc->SetFixedWidth(0);
    pLoc->SetTextColor(tokens::Token(tokens::EToken::InkTertiary));
    pLoc->SetAttribute(L"valign", L"vcenter");
    pLoc->SetAttribute(L"endellipsis", L"true");
    pLoc->SetMouseEnabled(false);
    pHeader->Add(pLoc.release());  // [C++17 R3] ownership to DuiLib

    pList->Add(pHeader.release());  // [C++17 R3] ownership to DuiLib
}

// [R162] 从 BuildStartup 抽出：单个分类体（默认折叠，内含该来源的启动行）。
void CPageStartup::PsCollectBodyRows(CVerticalLayoutUI* pList, const PsCatDef& cat, int nCatIdx)
{
    auto pBody = std::make_unique<CVerticalLayoutUI>(); // [C++17 R3]
    CDuiString sBodyName;
    sBodyName.Format(L"stbody_%d", nCatIdx);
    pBody->SetName(sBodyName.GetData());
    pBody->SetChildPadding(0);
    pBody->SetVisible(false);   // 默认折叠

    for (size_t k = 0; k < m_vStartup.size(); ++k)
    {
        StartupEntry& e = m_vStartup[k];
        bool bBelong = false;
        for (int si = 0; cat.nSrcs[si] != -1; ++si)
            if (cat.nSrcs[si] == static_cast<int>(e.nSrcType)) { bBelong = true; break; }
        if (!bBelong) continue;
        PsBuildStartupRow(pBody.get(), e, k);
    }
    pList->Add(pBody.release());  // [C++17 R3] ownership to DuiLib
}

// [R162] 从 BuildStartup 抽出：单条启动行（勾选框 + 位宽徽章 + 命令）。
//   [R126-2] 位宽徽章按 PE 头 Machine 判定：x64 蓝 / x86 绿 / 未知灰。
//   BootExecute 来源不支持在此启停，勾选框置灰但仍显示真实状态。
void CPageStartup::PsBuildStartupRow(CVerticalLayoutUI* pBody, StartupEntry& e, size_t k)
{
            // [C3] 行整体可点击（点击命令区域等同切换勾选框）
            auto pRow = std::make_unique<CClickRowUI>(); // [C++17 R3]
            pRow->SetFixedHeight(26);
            RECT rcRowInset = { 28, 0, 10, 0 };
            pRow->SetInset(rcRowInset);
            pRow->SetChildPadding(8);
            CDuiString sRowName;
            sRowName.Format(L"strow_%d", static_cast<int>(k));
            pRow->SetName(sRowName.GetData());
            auto pCheck = std::make_unique<CBoxCheckUI>(); // [C++17 R3] [BOX-CHECK] 自绘方框
            CDuiString sCheckName;
            sCheckName.Format(L"stck_%d", static_cast<int>(k));
            pCheck->SetName(sCheckName.GetData());
            pCheck->SetFixedWidth(240);
            pCheck->SetCheck(e.bEnabled);
            if (e.nSrcType == StartupSrcType::SRC_BOOT_EXECUTE)
                pCheck->SetEnabled(false);   // BootExecute 不支持在此启停
            StyleCheckBox(pCheck.get(), e.sName.GetData(), 22, m_ctx.HostGlyphBox(0));
            m_ctx.HostRefreshCheckText(pCheck.get());
            pRow->Add(pCheck.release()); // [C++17 R3] ownership to DuiLib
            // [R126-2] 位宽徽章：PE 头 Machine 字段精确判定（IsWow64Process2 同源），
            // x64 蓝 / x86 绿 / 未知灰，展示启动命令的位宽。
            auto pArch = std::make_unique<CTextUI>(); // [C++17 R3]
            if (e.nArch == 64)
            {
                pArch->SetText(StringMgr.Get("startup.s_arch_x64"));
                pArch->SetTextColor(tokens::Token(tokens::EToken::AccentText));
            }
            else if (e.nArch == 32)
            {
                pArch->SetText(StringMgr.Get("startup.s_arch_x86"));
                pArch->SetTextColor(tokens::Token(tokens::EToken::Success));
            }
            else
            {
                pArch->SetText(StringMgr.Get("startup.s_arch_na"));
                pArch->SetTextColor(tokens::Token(tokens::EToken::InkMuted));
            }
            pArch->SetFixedWidth(36);
            pArch->SetAttribute(L"align", L"center");
            pArch->SetAttribute(L"valign", L"vcenter");
            pArch->SetMouseEnabled(false);
            pRow->Add(pArch.release());  // [C++17 R3] ownership to DuiLib
            auto pCmd = std::make_unique<CTextUI>(); // [C++17 R3]
            pCmd->SetText(e.sCommand.GetData());
            pCmd->SetFixedWidth(0);
            pCmd->SetTextColor(tokens::Token(tokens::EToken::InkTertiary));
            pCmd->SetAttribute(L"valign", L"vcenter");
            pCmd->SetAttribute(L"endellipsis", L"true");
            pCmd->SetMouseEnabled(false);
            pRow->Add(pCmd.release());  // [C++17 R3] ownership to DuiLib
            // 隐藏禁用项：折叠行高，展开过滤时仍生效
            if (m_bHideDisabled && !e.bEnabled)
                pRow->SetVisible(false);
            pBody->Add(pRow.release());  // [C++17 R3] ownership to DuiLib
}

void CPageStartup::BuildStartup()
{
    m_vStartup.clear();
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"startup_list"));
    if (!pList) return;
    pList->RemoveAll();
    // style the hide-disabled checkbox with the shared box glyph
    CCheckBoxUI* pHide = static_cast<CCheckBoxUI*>(
        m_ctx.HostFindControl(L"startup_hide_disabled"));
    if (pHide)
    {
        // [复选框共性修复] 初始字形必须用空框 glyph(0)，再按实际勾选态
        // 刷新文本；旧代码传 glyph(1)(☑) 且不刷新，未勾选时视觉恒显勾选。
        StyleCheckBox(pHide, StringMgr.Get("misc.sf8434f36"), 26, m_ctx.HostGlyphBox(0));
        pHide->SetCheck(m_bHideDisabled);
        m_ctx.HostRefreshCheckText(pHide);
    }
    // S3：枚举全部 11 个启动项来源（原 BuildStartupPage 枚举段，零逻辑改动）
    StartupService::EnumerateStartup(m_vStartup);
    // ---- 5 大分类定义（对齐原版：系统/用户/启动文件夹/本机/新用户）----
    const PsCatDef cats[] = {
        { StringMgr.Get("optim.scc872d6f"), L"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
          { static_cast<int>(StartupSrcType::SRC_HKLM_RUN), static_cast<int>(StartupSrcType::SRC_HKLM_WOW_RUN), static_cast<int>(StartupSrcType::SRC_HKLM_RUNONCE), static_cast<int>(StartupSrcType::SRC_HKLM_WOW_RUNONCE), -1 } },
        { StringMgr.Get("optim.sf101f973"), L"HKCU\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
          { static_cast<int>(StartupSrcType::SRC_HKCU_RUN), static_cast<int>(StartupSrcType::SRC_HKCU_RUNONCE), -1 } },
        { StringMgr.Get("misc.s2e69cd5a"), L"%UserProfile%\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Startup",
          { static_cast<int>(StartupSrcType::SRC_STARTUP_FOLDER_USER), static_cast<int>(StartupSrcType::SRC_STARTUP_FOLDER_COMMON), static_cast<int>(StartupSrcType::SRC_STARTUP_FOLDER_USER_EXE), -1 } },
        { StringMgr.Get("optim.s1fd1d679"), L"HKLM\\SYSTEM\\CurrentControlSet\\Control\\Session Manager",
          { static_cast<int>(StartupSrcType::SRC_BOOT_EXECUTE), -1 } },
        { StringMgr.Get("optim.sec26c842"), L"HKU\\DEFAULT\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
          { static_cast<int>(StartupSrcType::SRC_HKU_DEFAULT_RUN), static_cast<int>(StartupSrcType::SRC_HKU_DEFAULT_RUNONCE), -1 } },
        { StringMgr.Get("optim.sgpol_run"), L"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\\Run",
          { static_cast<int>(StartupSrcType::SRC_GPOLICY_RUN), -1 } }
    };
    for (size_t c = 0; c < _countof(cats); ++c)
    {
        const PsCatDef& cat = cats[c];
        // [R162] 分类头 / 分类体各抽一个成员函数（属性值逐字保留，
        //   尤其是 inset.left=28 与数据行同缩进 —— 这是 [FIX-ALIGN] 修过的 4px 错位）
        PsBuildCatHeader(pList, cat, static_cast<int>(c));
        PsCollectBodyRows(pList, cat, static_cast<int>(c));
    }
    RefreshInfoBar(0);
    // [R62-A] 启动事件解析：读取最近 System channel Event 6005/1074/12/6006 → bootevents.log
    // [LAG-FIX] 事件日志重枚举移至后台线程，完成后经 WM_DIAG_LOG 回 UI 记一行摘要
    PostStartupDiagAsync(m_ctx.HostHwnd(), 0);
}

void CPageStartup::ToggleCategory(int c)
{
    CDuiString sBodyName;
    sBodyName.Format(L"stbody_%d", c);
    CVerticalLayoutUI* pBody = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(sBodyName.GetData()));
    if (!pBody) return;
    CDuiString sArName;
    sArName.Format(L"starrow_%d", c);
    CTextUI* pArrow = static_cast<CTextUI*>(m_ctx.HostFindControl(sArName.GetData()));
    bool bShow = !pBody->IsVisible();
    pBody->SetVisible(bShow);
    if (pArrow) pArrow->SetText(m_ctx.HostGlyphArrow(bShow ? 1 : 0));
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"startup_list"));
    if (pList)
    {
        pList->SetPos(pList->GetPos());
        if (pList->GetParent()) pList->GetParent()->SetPos(pList->GetParent()->GetPos());
    }
}

// [R162] 从 ApplyFilter / TryHitStartupRow 抽出：在行容器子控件里找 stck_%d，
//   解析出该行对应的启动项索引；找不到返回 -1。
//   原实现逐字重复了两遍（过滤要知道索引、右键命中也要知道索引）。
//   行名格式 stck_ 只在这里维护 —— 将来若改命名，不会出现"过滤器认得、右键不认得"。
[[nodiscard]] int CPageStartup::PsRowIndexOf(CHorizontalLayoutUI* pRow)
{
    if (!pRow) return -1;
    for (int c = 0; c < pRow->GetCount(); ++c)
    {
        CControlUI* pCh = pRow->GetItemAt(c);
        if (!pCh) continue;
        const wchar_t* pszN = pCh->GetName();
        if (pszN && wcsncmp(pszN, L"stck_", 5) == 0) return _ttoi(pszN + 5);
    }
    return -1;
}

// [R175] 从 ApplyFilter 抽出：按关键字过滤启动项，返回可见条数。
//   R35 的三字段匹配：名称 / 命令 / 位置 —— 用户可能按任一项搜，
//   只搜名称会让人以为"这条不在列表里"。
//   隐藏禁用项与关键字命中的关系是「与」：勾了"隐藏已禁用"时，
//   禁用的项即便命中关键字也不显示（与原实现一致）。
// [R175] 从 PsFilterStartupRows 抽出：单行在"关键字 + 隐藏禁用"下是否可见。
//   R35 三字段匹配：名称 / 命令 / 位置 —— 用户可能按任一项搜，
//   只搜名称会让人以为"这条不在列表里"。
//   隐藏禁用项与关键字命中是「与」关系：勾了"隐藏已禁用"时，
//   禁用的项即便命中关键字也不显示（与原实现一致）。
//   k 非法（行名解析失败/越界）时：不参与关键字判断，仅受"隐藏禁用"影响 ——
//   这与原实现一致（原代码对 k 非法的情况 bKwHit 保持 true）。
[[nodiscard]] bool CPageStartup::PsRowVisibleByFilter(int k, const CDuiString& sKw,
                                                     const CDuiString& sKwLower) const
{
    const bool bValid = (k >= 0 && k < static_cast<int>(m_vStartup.size()));
    if (m_bHideDisabled && bValid && !m_vStartup[k].bEnabled) return false;
    if (sKw.IsEmpty() || !bValid) return true;

    const StartupEntry& e = m_vStartup[k];
    CDuiString sName = e.sName, sCmd = e.sCommand, sLoc = e.sLocation;
    sName.MakeLower(); sCmd.MakeLower(); sLoc.MakeLower();
    return sName.Find(sKwLower.GetData()) >= 0
        || sCmd.Find(sKwLower.GetData()) >= 0
        || sLoc.Find(sKwLower.GetData()) >= 0;
}

[[nodiscard]] int CPageStartup::PsFilterStartupRows(const CDuiString& sKw,
                                                    const CDuiString& sKwLower)
{
    int nShown = 0;
    for (int c = 0; c < kStartupCatCount; ++c)
    {
        CDuiString sBodyName;
        sBodyName.Format(L"stbody_%d", c);
        CVerticalLayoutUI* pBody = static_cast<CVerticalLayoutUI*>(
            m_ctx.HostFindControl(sBodyName.GetData()));
        if (!pBody) continue;

        for (int r = 0; r < pBody->GetCount(); ++r)
        {
            CHorizontalLayoutUI* pRow = static_cast<CHorizontalLayoutUI*>(pBody->GetItemAt(r));
            if (!pRow) continue;

            // [R175] 单行匹配抽为 PsRowMatchesKeyword，嵌套 4→3
            const int k = PsRowIndexOf(pRow);
            if (PsRowVisibleByFilter(k, sKw, sKwLower)) { pRow->SetVisible(true); ++nShown; }
            else pRow->SetVisible(false);
        }
    }
    return nShown;
}

// [R175] 从 ApplyFilter 抽出：统计各分类体的总条数（供底栏"共 N 项"）。
[[nodiscard]] int CPageStartup::PsCountBodyRows()
{
    int nTotal = 0;
    for (int c = 0; c < kStartupCatCount; ++c)
    {
        CDuiString sBodyName;
        sBodyName.Format(L"stbody_%d", c);
        CVerticalLayoutUI* pB = static_cast<CVerticalLayoutUI*>(
            m_ctx.HostFindControl(sBodyName.GetData()));
        if (pB) nTotal += pB->GetCount();
    }
    return nTotal;
}

void CPageStartup::ApplyFilter()
{
    // R35: 读取关键字过滤值（保留原有隐藏禁用项功能）
    CDuiString sKw = m_ctx.HostReadEditValue(L"startup_filter");
    CDuiString sKwLower = sKw;
    sKwLower.MakeLower();
    int nShown = 0;
    // ---- startup items ----
    // [R175] 过滤循环抽为 PsFilterStartupRows，嵌套 4→2
    nShown = PsFilterStartupRows(sKw, sKwLower);
    // ---- services（[R105] 统一走 ApplySvcFilter：svc_filter 关键字 + 隐藏禁用）----
    ApplySvcFilter();
    // R35: 更新底栏计数
    CTextUI* pCount = static_cast<CTextUI*>(m_ctx.HostFindControl(L"lbl_startup_count"));
    if (pCount)
    {
        int nTotal = 0;
        for (int c = 0; c < kStartupCatCount; ++c)
        {
            CDuiString sBodyName; sBodyName.Format(L"stbody_%d", c);
            if (CVerticalLayoutUI* pB = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(sBodyName.GetData())))
                nTotal += pB->GetCount();
        }
        CDuiString s;
        if (nTotal == 0) s = StringMgr.Get("misc.h6822");
        else if (sKw.IsEmpty()) s.Format(StringMgr.Get("misc.h5d85"), nTotal);
        else s.Format(StringMgr.Get("misc.h1173"), nTotal, nShown);
        pCount->SetText(s.GetData());
    }
}

void CPageStartup::RefreshInfoBar(int nTab)
{
    CTextUI* pBar = static_cast<CTextUI*>(m_ctx.HostFindControl(L"startup_infobar"));
    if (!pBar) return;
    CDuiString s;
    if (nTab == 1)
    {
        int nRun = 0;
        for (size_t i = 0; i < m_vServices.size(); ++i)
            if (m_vServices[i].bRunning) ++nRun;
        s.Format(StringMgr.Get("optim.s55ad4359"),
                 static_cast<int>(m_vServices.size()), nRun);
    }
    else
    {
        int nOff = 0;
        for (size_t i = 0; i < m_vStartup.size(); ++i)
            if (!m_vStartup[i].bEnabled) ++nOff;
        s.Format(StringMgr.Get("optim.s41c2a654"),
                 static_cast<int>(m_vStartup.size()), nOff);
    }

    // [R69] 追加启动时间估算（若 bootevents.log 中存在 6005/12 配对）
    // 源版 CFrameWnd::EstimateBootSeconds（FrameWnd.cpp 6088-6181）；GetLocalAppDataDir
    // 在架构版由 _wgetenv(L"LOCALAPPDATA") 替代（对齐 DiagnosticsService 风格）。
    {
        int nEst = EstimateBootSeconds();
        if (nEst >= 0)
        {
            CDuiString sBoot;
            sBoot.Format(StringMgr.Get("misc.sc9baee09"));
            s += sBoot;
        }
        else
        {
            s += StringMgr.Get("misc.s5a23c608");
        }
    }

    pBar->SetText(s);
}

// [R69] 启动事件配对检查：bootevents.log 中是否存在同时含 6005 与 12 的 stats 行
// 日志存的是事件命中计数而非时间戳，故本函数不计算时长，只判断"是否有配对"。
// 无日志或无配对返回 -1；存在配对返回 0。来源：源版 FrameWnd.cpp:6088。
int CPageStartup::EstimateBootSeconds()
{
    // [AUDIT-P2] 原 87 行 / CC=28，拆为 3 段：读日志解码 / 拆行 / 反向找配对行。
    //
    // 语义澄清（本轮核对后订正原注释）：bootevents.log 里 boot_start(6005) 与
    // boot_done(12) 存的是 DiagnosticsService 的事件**命中计数**
    // （DiagBootEventStats::nHit6005 / nHit12），不是时间戳，所以本函数
    // 无法算出真实启动秒数。原注释"解析 6005→12 时间差（秒）"是错的。
    // 真实语义 = 日志中是否存在同时含这两项的 stats 行；返回 0（存在）/
    // -1（不存在），调用方按 >=0 视为可用。本轮只做结构调整 + 注释订正，
    // 不改返回值语义。

    CDuiString sContent;
    if (!EstReadBootLog(sContent)) return -1;

    std::vector<CDuiString> vLines;
    EstSplitLines(sContent, vLines);

    return EstHasBootPair(vLines) ? 0 : -1;
}

// [AUDIT-P2] 从 EstimateBootSeconds 提取：读 bootevents.log 并 UTF-8 解码
bool CPageStartup::EstReadBootLog(CDuiString& sContent)
{
    // [M-5 读侧] bootevents.log 已由 DiagnosticsService 迁入统一日志目录，
    //   这里必须同批切换，否则启动耗时估算会永远读不到新写入的事件。
    CDuiString sPath = LogService::UnifiedLogPath(L"bootevents.log").c_str();

    PSFileGuard h(::CreateFile(sPath.GetData(), GENERIC_READ, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD nSize = ::GetFileSize(h, nullptr);
    if (nSize <= 0 || nSize == INVALID_FILE_SIZE) return false;   // PSFileGuard 析构自动关闭

    // [R162] 缓冲改 std::string：它本就只承载 UTF-8 字节流。
    //   原先 vector<BYTE> 导致 MultiByteToWideChar 前要 reinterpret_cast<const char*>，
    //   换容器后 ReadFile 与转换都直接吃 data()，两处别名转换从类型上消失。
    std::string vBuf(static_cast<size_t>(nSize), static_cast<char>(0));
    DWORD nRd = 0;
    ::ReadFile(h, &vBuf[0], nSize, &nRd, nullptr);
    // PSFileGuard 析构自动关闭
    if (nRd == 0) return false;

    // UTF-8 → 宽字符
    int nW = ::MultiByteToWideChar(CP_UTF8, 0, vBuf.data(), static_cast<int>(nRd), nullptr, 0);
    if (nW <= 0) return false;
    std::vector<wchar_t> wBuf(nW + 1);
    ::MultiByteToWideChar(CP_UTF8, 0, vBuf.data(), static_cast<int>(nRd), wBuf.data(), nW);
    wBuf[nW] = 0;
        sContent = CDuiString(wBuf.data(), nW);
        return true;
}

// [AUDIT-P2] 从 EstimateBootSeconds 提取：按换行拆行、去行尾 CR、跳过空行
void CPageStartup::EstSplitLines(const CDuiString& sContent,
                                 std::vector<CDuiString>& vLines)
{
    // 逐行扫描，寻找 boot_start(6005) 与 boot_done(12) 命中行
    int nStart = 0;
    int nLen = sContent.GetLength();
    for (int i = 0; i < nLen; ++i)
    {
        if (sContent[i] == L'\n')
        {
            CDuiString sLine = sContent.Mid(nStart, i - nStart);
            if (sLine.GetLength() > 0 && sLine[sLine.GetLength() - 1] == L'\r')
                sLine = sLine.Left(sLine.GetLength() - 1);
            if (!sLine.IsEmpty()) vLines.push_back(sLine);
            nStart = i + 1;
        }
    }
    if (nStart < nLen)
    {
        CDuiString sLast = sContent.Mid(nStart);
        if (sLast.GetLength() > 0 && sLast[sLast.GetLength() - 1] == L'\r')
            sLast = sLast.Left(sLast.GetLength() - 1);
        if (!sLast.IsEmpty()) vLines.push_back(sLast);
    }
}

// [AUDIT-P2] 从 EstimateBootSeconds 提取：反向找最近一次同时含 6005 与 12 的 stats 行
// [R175] 从 EstHasBootPair 抽出：从 key= 之后取整数，取不到返回 -1。
//   原实现两份逐字同构的解析（6005 与 12 各一段），合并后只维护一处。
//   只认紧跟 key 的连续数字：中间出现非数字即停止 —— 事件日志行格式是
//   "boot_start(6005)=1234 boot_done(12)=5678"，遇到空格就结束是对的。
[[nodiscard]] static int EstTakeNumberAfter(const CDuiString& s, const wchar_t* pszKey)
{
    int iStart = s.Find(pszKey);
    if (iStart < 0) return -1;
    iStart += static_cast<int>(wcslen(pszKey));
    int iEnd = iStart;
    while (iEnd < s.GetLength() && _istdigit(s[iEnd])) ++iEnd;
    if (iEnd <= iStart) return -1;
    return _ttoi(s.Mid(iStart, iEnd - iStart));
}

bool CPageStartup::EstHasBootPair(const std::vector<CDuiString>& vLines)
{
    // 反向找最近一次包含 6005 与 12 的 stats 行
    for (size_t k = vLines.size(); k-- > 0; )
    {
        const CDuiString& s = vLines[k];
        if (s.Find(L"boot_start(6005)") < 0) continue;
        if (s.Find(L"boot_done(12)") < 0) continue;

        // [R175] 两段同构解析合并为一行调用，嵌套 4→2
        const int n6005 = EstTakeNumberAfter(s, L"boot_start(6005)=");
        const int n12   = EstTakeNumberAfter(s, L"boot_done(12)=");
        if (n6005 > 0 && n12 > 0)
        {
    return true;
        }
        break;
    }
    return false;
}



// [R111] 复制启动命令到剪贴板（右键菜单 4105）
void CPageStartup::CopyStartupCommand(int k)
{
    if (k < 0 || k >= static_cast<int>(m_vStartup.size())) return;
    StartupEntry& e = m_vStartup[k];

    CDuiString sCopy = e.sCommand;
    if (sCopy.IsEmpty()) sCopy = e.sName;

    FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), sCopy.GetData());
    m_ctx.HostToast(StringMgr.Get("startup.s_menu_copied_cmd"),
                    sCopy.GetData(), 0);

    CDuiString sLog;
    sLog.Format(L"[startup] copied command: %s", sCopy.GetData());
    m_ctx.HostLog(sLog.GetData());
}

void CPageStartup::OpenLocation(int k)
{
    if (k < 0 || k >= static_cast<int>(m_vStartup.size())) return;
    StartupEntry& e = m_vStartup[k];
    // 1. 先尝试直接打开命令（explorer /select 或 ShellExecute）
    if (!e.sCommand.IsEmpty())
    {
        // 若是 .lnk 或 .exe：Explorer /select,"路径" 选中它
        CDuiString sPath = e.sCommand;
        // 取路径部分
        CDuiString sDir = sPath;
        int nSlash = sDir.ReverseFind(L'\\');
        if (nSlash > 0) sDir = sDir.Left(nSlash);
        if (!sDir.IsEmpty() && ::GetFileAttributes(sDir.GetData()) != INVALID_FILE_ATTRIBUTES)
        {
            CDuiString sArg;
            sArg.Format(L"/select,\"%s\"", sPath.GetData());
            ::ShellExecuteW(nullptr, L"open", L"explorer.exe",
                            sArg.GetData(), nullptr, SW_NORMAL);
            return;
        }
    }
    // 2. 否则打开来源注册表键（regedit 定位）
    HKEY hRoot = nullptr;
    CDuiString sPath;
    REGSAM samExtra = 0;
    if (StartupService::FindRunKey(e.nSrcType, hRoot, sPath, samExtra) || e.nSrcType == StartupSrcType::SRC_BOOT_EXECUTE)
    {
        CDuiString sFull;
        const wchar_t* pszRoot =
            hRoot == HKEY_CURRENT_USER ? L"HKEY_CURRENT_USER" :
            hRoot == HKEY_LOCAL_MACHINE ? L"HKEY_LOCAL_MACHINE" :
            hRoot == HKEY_USERS ? L"HKEY_USERS" : L"";
        if (pszRoot)
        {
            sFull = pszRoot;
            sFull += L"\\";
            sFull += e.nSrcType == StartupSrcType::SRC_BOOT_EXECUTE
                ? L"SYSTEM\\CurrentControlSet\\Control\\Session Manager"
                : sPath.GetData();
        }
        if (!sFull.IsEmpty())
        {
            // reg add HKCU\... /v xxx 不能直接 regedit 定位；用 /e 导出到 temp 再打开… 简单：启动 regedit 用户手动定位
            ::ShellExecuteW(nullptr, L"open", L"regedit.exe", nullptr, nullptr, SW_NORMAL);
            CDuiString sTip;
            sTip.Format(StringMgr.Get("optim.sae548cfc"),
                        sFull.GetData());
            m_ctx.HostAlert(StringMgr.Get("optim.sdbee5354"), sTip.GetData(), MB_ICONINFORMATION);
            return;
        }
    }
    m_ctx.HostAlert(StringMgr.Get("misc.s0fc60ddb"), StringMgr.Get("optim.s6f0ef4d0"), MB_ICONINFORMATION);
}

void CPageStartup::ShowStartupMenu(int k)
{
    if (k < 0 || k >= static_cast<int>(m_vStartup.size())) return;
    StartupEntry& e = m_vStartup[k];
    POINT pt; ::GetCursorPos(&pt);
    HMENU hMenu = ::CreatePopupMenu();
    UINT uChkOn  = e.bEnabled ? MF_CHECKED : 0;
    UINT uChkOff = e.bEnabled ? 0 : MF_CHECKED;
    bool bCanToggle = (e.nSrcType != StartupSrcType::SRC_BOOT_EXECUTE);
    ::AppendMenu(hMenu, MF_STRING | uChkOn, 4101, StringMgr.Get("misc.se5f9be69"));
    ::AppendMenu(hMenu, MF_STRING | uChkOff, 4102, StringMgr.Get("misc.s8ee5d2cf"));
    if (!bCanToggle)
    {
        ::EnableMenuItem(hMenu, 4101, MF_BYPOSITION | MF_GRAYED);
        ::EnableMenuItem(hMenu, 4102, MF_BYPOSITION | MF_GRAYED);
    }
    ::AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenu(hMenu, MF_STRING, 4103, StringMgr.Get("optim.sa8e16acb"));
    ::AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenu(hMenu, MF_STRING, 4104, StringMgr.Get("misc.sb45c9d17"));
    // [R111] 复制启动命令到剪贴板
    ::AppendMenu(hMenu, MF_STRING, 4105, StringMgr.Get("startup.s_menu_copy_cmd"));
    int iCmd = ::TrackPopupMenu(hMenu,
        TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
        pt.x, pt.y, 0, m_ctx.HostHwnd(), nullptr);
    ::DestroyMenu(hMenu);
    if (iCmd == 4101 && bCanToggle)
    {
        CDuiString sSt; sSt.Format(L"stck_%d", k);
        if (!OnToggle(sSt.GetData(), true))
            m_ctx.HostAlert(StringMgr.Get("optim.boot_items"), StringMgr.Get("optim.s4f53336d"), MB_ICONWARNING);
    }
    else if (iCmd == 4102 && bCanToggle)
    {
        CDuiString sSt; sSt.Format(L"stck_%d", k);
        if (!OnToggle(sSt.GetData(), false))
            m_ctx.HostAlert(StringMgr.Get("optim.boot_items"), StringMgr.Get("optim.sdabceb96"), MB_ICONWARNING);
    }
    else if (iCmd == 4103)               DeleteEntry(k);
    else if (iCmd == 4104)               OpenLocation(k);
    else if (iCmd == 4105)               CopyStartupCommand(k);
}

bool CPageStartup::OnToggle(const wchar_t* pszName, bool bEnabled)
{
    int k = -1;
    if (!ParsePrefix1(CDuiString(pszName), L"stck_", k)) return false;
    if (k < 0 || k >= static_cast<int>(m_vStartup.size())) return false;
    StartupEntry& e = m_vStartup[k];
    // BootExecute 不支持在此启停（勾选框已禁用，正常不会进入）
    if (e.nSrcType == StartupSrcType::SRC_BOOT_EXECUTE) return false;
    // S3：按来源写 StartupApproved 12 字节启停位
    int nRes = StartupService::SetStartupApproved(e, bEnabled);
    if (nRes == 1)
    {
        m_ctx.HostLog(StringMgr.Get("error.sfc21d459"));
        return false;
    }
    if (nRes != 0)
    {
        m_ctx.HostLog(StringMgr.Get("error.s36562464"));
        return false;
    }
    e.bEnabled = bEnabled;
    CDuiString sLog;
    sLog.Format(L"[startup] %s -> %s",
                 e.sName.GetData(), bEnabled ? CDuiString(L"enabled") : L"disabled");
    m_ctx.HostLog(sLog.GetData());
    if (CTabLayoutUI* pSt = static_cast<CTabLayoutUI*>(
            m_ctx.HostFindControl(L"startup_pages")))
    {
        if (pSt->GetCurSel() == 0) RefreshInfoBar(0);
    }
    return true;
}

// ============================================================
// [R105] 服务搜索过滤：按服务名 / 显示名 / 状态文本实时隐藏非命中行，
// 并并入 startup_hide_disabled 的"隐藏禁用项"逻辑（统一入口）。
// 采用"隐藏而非重建"策略，保证 svcrow_%d / svccombo_%d / svcstate_%d
// 行号与 m_vServices 序号严格对齐（与 ApplyFilter 同模式）。
// ============================================================
void CPageStartup::ApplySvcFilter()
{
    CEditUI* pEdit = static_cast<CEditUI*>(m_ctx.HostFindControl(L"svc_filter"));
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"svc_list"));
    if (!pList) return;

    CDuiString sKey = pEdit ? pEdit->GetText() : CDuiString();
    sKey.MakeLower();

    int nShown = 0;
    int nTotal = static_cast<int>(pList->GetCount());
    for (int i = 0; i < nTotal; ++i)
    {
        CControlUI* pRow = pList->GetItemAt(i);
        if (!pRow) continue;

        // 从行子控件中取服务名 / 显示名 / 状态文本（[R127-A] 判定提取为共享 helper）
        bool bKwHit = sKey.IsEmpty() || UiHelpers_RowTextHitsKey(pRow, sKey);

        bool bHideDis = m_bHideDisabled && i < static_cast<int>(m_vServices.size())
            && m_vServices[i].nStartType == SERVICE_DISABLED;

        pRow->SetVisible(bKwHit && !bHideDis);
        if (pRow->IsVisible()) ++nShown;
    }

    CDuiString sLog;
    sLog.Format(L"[svc] filter '%s' -> %d / %d visible (hideDisabled=%d)",
                pEdit ? pEdit->GetText().GetData() : L"",
                nShown, nTotal, m_bHideDisabled ? 1 : 0);
    m_ctx.HostLog(sLog.GetData());
}

// ============================================================
// [R117-1] 显示驱动服务复选框：切换后重建服务列表（驱动行显隐）
// ============================================================
void CPageStartup::OnShowDriversChanged(bool bShow)
{
    m_bShowDrivers = bShow;
    CDuiString sLog;
    sLog.Format(L"[svc] showDrivers=%d -> rebuild services list", bShow ? 1 : 0);
    m_ctx.HostLog(sLog.GetData());
    BuildServices();
}

// ============================================================
// P8：第二级事件路由（原 CFrameWnd::Notify 启动页分支逐字迁入；
// 跨页按钮 btn_startup_to_toolbox 走 m_ctx.SwitchToPage）
// ============================================================

void CPageStartup::OnEvent(TNotifyUI& msg)
{
    // [AUDIT-P2] 原 88 行 / CC=28，按事件类型拆为 5 个处理函数：
    //   StOnItemSelect / StOnClick / StOnDoubleClick / StOnTextChanged / StOnSelectChanged。
    // 路由语义逐字保留（含 Tab2 计划任务跨页转发、stcat_/sttab_/strow_ 前缀、
    // 服务类型组合框 svccombo_ 的 itemselect 分支）。
    CDuiString sName = msg.pSender ? msg.pSender->GetName() : CDuiString(L"");
    const wchar_t* pszName = sName.GetData();

    // 服务类型组合框以 svccombo_ 为前缀发 itemselect
    if (msg.sType == DUI_MSGTYPE_ITEMSELECT)
    {
        StOnItemSelect(msg, pszName);
        return;
    }

    if (msg.sType == L"click")
    {
        StOnClick(msg, sName, pszName);
        return;
    }

    // [R111-7] 服务行双击：运行中→停止，否则→启动（复用 ControlServiceItem 4401/4402）
    if (msg.sType == L"dbclick")
    {
        StOnDoubleClick(sName);
        return;
    }

    if (msg.sType == L"textchanged")
    {
        StOnTextChanged(sName);
        return;
    }

    if (msg.sType == L"selectchanged")
    {
        StOnSelectChanged(msg, sName, pszName);
        return;
    }
}

// [AUDIT-P2] 服务类型组合框（svccombo_ 前缀由本页处理，非跨页）
void CPageStartup::StOnItemSelect(TNotifyUI& msg, const wchar_t* pszName)
{
    if (wcsncmp(pszName, L"svccombo_", 9) == 0)
    {
        CComboUI* pCombo = static_cast<CComboUI*>(msg.pSender);
        if (pCombo) OnServiceStartType(_ttoi(pszName + 9), pCombo->GetCurSel());
    }
    return;
}

// [AUDIT-P2] click 分支：计划任务跨页转发 + 刷新/添加/工具箱 + 两个复选框 + stcat_ 折叠
void CPageStartup::StOnClick(TNotifyUI& msg, const CDuiString& sName, const wchar_t* pszName)
{
    // [R5] startup_pages Tab2 物理内嵌的计划任务控件（含 sttab_2 与
    // btn_task_*/tasktab_*/task_filter）跨页转发 PAGE_TASKS 处理；
    // 原壳 Notify 的 10 个具名分支整段下沉，壳不再直调 CPageTasks。
    if (ForwardTaskChildEvent(sName, msg)) return;

    if (sName == L"btn_startup_refresh") { BuildStartup(); BuildServices(); return; }
    // [U-2] 添加启动项
    else if (sName == L"btn_startup_add") { AddItem(); return; }
    // [R63] 工具箱入口跳转
    else if (sName == L"btn_startup_to_toolbox") { m_ctx.SwitchToPage(EPageId::PAGE_TOOLS); return; }
    // 注：sttab_2（计划任务子 Tab）[R5] 已在本函数开头跨页转发 PAGE_TASKS。

    else if (sName == L"startup_hide_disabled")
    {
        CCheckBoxUI* p = static_cast<CCheckBoxUI*>(msg.pSender);
        OnHideDisabledChanged(p && p->GetCheck());
        return;
    }
    // [R117-1] 显示驱动服务复选框
    else if (sName == L"svc_show_drivers")
    {
        CCheckBoxUI* p = static_cast<CCheckBoxUI*>(msg.pSender);
        OnShowDriversChanged(p && p->GetCheck());
        return;
    }

    if (wcsncmp(pszName, L"stcat_", 6) == 0)
    {
        ToggleCategory(_ttoi(pszName + 6));
        return;
    }
}

// [AUDIT-P2] 服务行双击
void CPageStartup::StOnDoubleClick(const CDuiString& sName)
{
    if (HandleServiceRowDbClick(sName)) return;
}

// [AUDIT-P2] 关键字过滤：启动项 / 服务各一路
void CPageStartup::StOnTextChanged(const CDuiString& sName)
{
    if (sName == L"startup_filter") ApplyFilter();
    // [R105] 服务搜索过滤：svc_filter 实时过滤服务行
    else if (sName == L"svc_filter") ApplySvcFilter();
}

// [AUDIT-P2] 勾选态变更：条目勾选 / 行非勾选区点击 / sttab_ 子 Tab 切换
void CPageStartup::StOnSelectChanged(TNotifyUI& msg, const CDuiString& sName, const wchar_t* pszName)
{
    int a = -1;
    if (HandleEntryCheckChanged(sName, pszName, msg)) return;
    // [C3] 点击启动项行的非勾选区域：等价切换勾选框（BootExecute 禁用项不响应）
    if (msg.sType == DUI_MSGTYPE_CLICK && ParsePrefix1(sName, L"strow_", a))
    {
        CDuiString sChk;
        sChk.Format(L"stck_%d", a);
        CCheckBoxUI* pChk = static_cast<CCheckBoxUI*>(m_ctx.HostFindControl(sChk.GetData()));
        if (pChk && pChk->IsEnabled())
            pChk->Selected(!pChk->GetCheck());
        return;
    }

    if (wcsncmp(pszName, L"sttab_", 6) == 0)
    {
        int idx = _ttoi(pszName + 6);
        CTabLayoutUI* p = static_cast<CTabLayoutUI*>(m_ctx.HostFindControl(L"startup_pages"));
        if (p) p->SelectItem(idx);
        RefreshInfoBar(idx);
        return;
    }
}


// P8：原 CFrameWnd WM_RBUTTONUP/WM_CONTEXTMENU 启动项/服务命中块。
// stck_ 首块对两种消息生效；svcrow_ 服务块仅 WM_RBUTTONUP。
// [R162] 从 OnContextMenu 抽出：启动项行命中 → 弹启动项菜单。
//   P8：原 CFrameWnd WM_RBUTTONUP/WM_CONTEXTMENU 启动项命中块，stck_ 对两种消息生效。
//   两种命中路径：
//     ① 先向上找到 startup_list 祖先，确认在启动列表内；
//     ② 再向上找 class 名为 HorizontalLayoutUI 的行容器，在子控件里找 stck_%d；
//        若某层本身就是 stck_%d（直接点中勾选框），也算命中。
//   避开 dynamic_cast 是有意为之（省 RTTI 依赖），改用 class 名判定 —— 逐字保留。
[[nodiscard]] bool CPageStartup::TryHitStartupRow(CControlUI* pHit)
{
    bool bInStartup = false;
    for (CControlUI* p = pHit; p; p = p->GetParent())
    {
        if (lstrcmpW(p->GetName(), L"startup_list") == 0) { bInStartup = true; break; }
    }
    if (!bInStartup) return false;

    for (CControlUI* p = pHit; p; p = p->GetParent())
    {
        const wchar_t* pszClass = p->GetClass();
        if (pszClass && lstrcmpiW(pszClass, L"HorizontalLayoutUI") == 0)
        {
            CHorizontalLayoutUI* pRow = static_cast<CHorizontalLayoutUI*>(p);
            // [R162] 与 ApplyFilter 共用 PsRowIndexOf
            const int kRow = PsRowIndexOf(pRow);
            if (kRow >= 0) { ShowStartupMenu(kRow); return true; }
        }
        // 直接点中了 stck_k 勾选框本体
        const wchar_t* pszMn = p->GetName();
        if (pszMn && wcsncmp(pszMn, L"stck_", 5) == 0)
        {
            ShowStartupMenu(_ttoi(pszMn + 5));
            return true;
        }
    }
    return false;
}

// [R162] 从 OnContextMenu 抽出：[C4] 服务行命中 → 弹启动/停止/重启菜单。
[[nodiscard]] bool CPageStartup::TryHitServiceRow(CControlUI* pHit)
{
    for (CControlUI* p = pHit; p; p = p->GetParent())
    {
        const wchar_t* psz = p->GetName();
        if (!psz) continue;
        if (wcsncmp(psz, L"svcrow_", 7) == 0)
        {
            ShowServiceMenu(_ttoi(psz + 7));
            return true;
        }
    }
    return false;
}

bool CPageStartup::OnContextMenu(POINT pt, bool bFromContextMsg)
{
    CControlUI* pHit = m_ctx.HostFindControlAt(pt);
    if (!pHit) return false;

    // [R162] 命中解析抽为 TryHitStartupRow / TryHitServiceRow，嵌套 6→3
    if (TryHitStartupRow(pHit)) return true;

    // [C4] 服务行右键：启动/停止/重启（原壳第二块，仅 WM_RBUTTONUP）
    if (bFromContextMsg) return false;
    return TryHitServiceRow(pHit);
}
// ============================================================
// [R-B1] Ctrl+C / Ctrl+X 复制首个启动项命令
// ============================================================

void CPageStartup::OnGlobalCopy(bool /*bCut*/)
{
    if (m_vStartup.empty())
    {
        m_ctx.HostToast(StringMgr.Get("startup.s_nocopy"),
                        StringMgr.Get("startup.s_nocopy_body"), 1);
        return;
    }
    CopyStartupCommand(0);
}
// ============================================================
// [R-B25] 服务依赖图：打开 EToolDlgKind::TD_DEPGRAPH 弹窗
// 正向依赖：QueryServiceConfig2(SERVICE_CONFIG_DEPENDENCIES)
// 反向依赖：EnumDependentServices（复用 StartupService::EnumerateDependentServices）
// ============================================================
void CPageStartup::ShowServiceDepGraph(int k)
{
    if (k < 0 || k >= static_cast<int>(m_vServices.size())) return;
    const ServiceEntry& se = m_vServices[k];

    // 正向依赖：该服务依赖哪些服务
    std::vector<CDuiString> vDep;
    DWORD dwErr = 0;
    DependencyGraphService::QueryServiceDependencies(se.sName.GetData(), vDep, dwErr);

    CDuiString sRoot;
    if (!se.sDisplayName.IsEmpty()) sRoot = se.sDisplayName;
    else sRoot = se.sName;
    sRoot += L" (";
    sRoot += se.sName;
    sRoot += L")";

    // 加反向依赖标记（子节点前缀）
    std::vector<CDuiString> vDepDisplay;
    std::vector<CDuiString> vDepNames;
    DWORD dwDepErr = 0;
    DependencyGraphService::EnumerateStartupDependents(
        se.sName.GetData(), vDepNames, vDepDisplay, dwDepErr);

    for (size_t i = 0; i < vDepNames.size(); ++i)
    {
        CDuiString sOne;
        if (!vDepDisplay[i].IsEmpty())
        {
            sOne = vDepDisplay[i];
            sOne += L" (";
            sOne += vDepNames[i];
            sOne += L")";
        }
        else
        {
            sOne = vDepNames[i];
        }
        vDep.push_back(sOne);
    }

    // [R127-A] 依赖图对话框组装与展示提取为共享 helper
    //   （原与 Page_AppxCtx::ShowDepGraph 逐字重复）
    UiHelpers_ShowDepGraphDialog(&m_ctx, sRoot.GetData(), vDep);

    CDuiString sLog;
    sLog.Format(L"[svc] dep graph: %s deps=%d dependents=%d",
                se.sName.GetData(), static_cast<int>(vDep.size()) - static_cast<int>(vDepDisplay.size()),
                static_cast<int>(vDepDisplay.size()));
    m_ctx.HostLog(sLog.GetData());
}

#include <memory>   // [C++17 R3] make_unique（置于文件尾，避免影响 verify 行号）

// [REFACTOR] 从 OnEvent 提取：startup_pages Tab2 物理内嵌的计划任务控件
// （sttab_2 与 btn_task_*/tasktab_*/task_filter）跨页转发给 PAGE_TASKS。
// 命中并转发后返回 true，调用方据此立即 return。逐字搬迁零逻辑改动。
bool CPageStartup::ForwardTaskChildEvent(const CDuiString& sName, TNotifyUI& msg)
{
    if (sName != L"sttab_2"
        && sName != L"btn_task_refresh"
        && sName != L"btn_task_enable"
        && sName != L"btn_task_disable"
        && sName != L"btn_task_run"
        && sName != L"btn_task_delete"
        && sName != L"task_filter"
        && sName != L"tasktab_0"
        && sName != L"tasktab_1"
        && sName != L"tasktab_2")
        return false;
    if (IPageHost* pHost = m_ctx.HostPageHost())
    {
        if (CPageBase* pTasks = pHost->PageById(EPageId::PAGE_TASKS))
            pTasks->OnEvent(msg);
    }
    return true;
}

// [REFACTOR] 从 OnEvent 提取：服务行双击——运行中则停止，否则启动
// （复用 ControlServiceItem 4402/4401）。命中 svcrow_ 并处理后返回 true。
// 逐字搬迁零逻辑改动。
bool CPageStartup::HandleServiceRowDbClick(const CDuiString& sName)
{
    int a = -1;
    if (!ParsePrefix1(sName, L"svcrow_", a)) return false;
    if (a >= 0 && a < static_cast<int>(m_vServices.size()))
    {
        ServiceEntry& se = m_vServices[a];
        OnServiceToggle(a, se.bRunning ? 4402 : 4401);
    }
    return true;
}

// [REFACTOR] 从 OnEvent 提取：启动项复选框勾选变更（stck_ 前缀）。
// 写入失败时回滚勾选状态（不触发通知，避免递归）并给出可见提示。
// 命中 stck_ 并处理后返回 true；pszName 为控件名原始指针。逐字搬迁。
bool CPageStartup::HandleEntryCheckChanged(const CDuiString& sName,
                                           const wchar_t* pszName, TNotifyUI& msg)
{
    int a = -1;
    if (!ParsePrefix1(sName, L"stck_", a)) return false;
    CCheckBoxUI* p = static_cast<CCheckBoxUI*>(msg.pSender);
    bool bWant = p && p->GetCheck();
    // [C3] 写入失败时回滚勾选状态（不触发通知，避免递归）并给出可见提示
    if (!OnToggle(pszName, bWant))
    {
        if (p) p->Selected(!bWant, false);
        ::MessageBoxW(m_ctx.HostHwnd(),
            StringMgr.Get("optim.s6209510a"),
            StringMgr.Get("optim.boot_items"), MB_OK | MB_ICONWARNING);
    }
    m_ctx.HostRefreshCheckText(p);
    return true;
}

// [R229] Re-colour the startup rows and the service sub-page after a theme switch.
//
// PageBase.h contract: colour attributes only -- no Build()/Add/Remove, or the lists,
// the checked state and the scroll position are lost and m_pFocusCtrl dangles.
//
// Two things this must get right:
//   * CClicKRowUI keeps its own selected flag, so the row background has to be derived
//     from IsRowSelected() -- painting Surface on every row would erase the selection.
//   * Most cell texts are UNNAMED; the row layout is positional.
void CPageStartup::RefreshThemeColors()
{
    if (CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(
            m_ctx.HostFindControl(L"startup_list")))
    {
        for (int i = 0; i < pList->GetCount(); ++i)
        {
            CControlUI* pRowCtl = pList->GetItemAt(i);
            CClickRowUI* pRowRow = dynamic_cast<CClickRowUI*>(pRowCtl);
            if (!pRowRow) continue;   // spinner / empty hint

            // Preserve the selected state instead of flattening it.
            pRowCtl->SetBkColor(pRowRow->IsRowSelected()
                                ? tokens::Token(tokens::EToken::Accent)
                                : tokens::Token(tokens::EToken::Surface));

            IContainerUI* pRow = static_cast<IContainerUI*>(
                pRowCtl->GetInterface(DUI_CTR_CONTAINER));
            if (!pRow) continue;
            for (int c = 0; c < pRow->GetCount(); ++c)
            {
                CTextUI* pText = dynamic_cast<CTextUI*>(pRow->GetItemAt(c));
                if (!pText) continue;
                pText->SetTextColor(pRowRow->IsRowSelected()
                                    ? tokens::Token(tokens::EToken::OnAccent)
                                    : tokens::Token(tokens::EToken::Ink));
            }
        }
    }

    // Service sub-page: same idea, plus the run-state column, which must be recomputed
    // from the live service state rather than from whatever colour is on the control.
    if (CVerticalLayoutUI* pSvc = static_cast<CVerticalLayoutUI*>(
            m_ctx.HostFindControl(L"svc_list")))
    {
        for (int i = 0; i < pSvc->GetCount(); ++i)
        {
            CControlUI* pRowCtl = pSvc->GetItemAt(i);
            if (!dynamic_cast<CClickRowUI*>(pRowCtl)) continue;
            IContainerUI* pRow = static_cast<IContainerUI*>(
                pRowCtl->GetInterface(DUI_CTR_CONTAINER));
            if (!pRow) continue;
            pRowCtl->SetBkColor(tokens::Token(tokens::EToken::Surface));
            for (int c = 0; c < pRow->GetCount(); ++c)
            {
                CTextUI* pText = dynamic_cast<CTextUI*>(pRow->GetItemAt(c));
                if (pText) pText->SetTextColor(tokens::Token(tokens::EToken::Ink));
            }
        }
    }
}
