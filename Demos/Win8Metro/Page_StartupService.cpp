#include "stdafx.h"
#include "ThemeTokens.h"   // [R203] 颜色令牌（深色/浅色跟随）
#include "SafeEnv.h"   // [B6] 安全环境变量读取：替代弃用 _wgetenv

// ============================================================
// ============================================================
// Page_StartupService.cpp —— 启动项页「服务管理」段（R75 拆出）
// 承接服务枚举与操作 11 个函数：
//   BuildServices / StAddServiceEmptyHint / StFilterDriverServices / StBuildServiceRow /
//   AddItem / OnServiceStartType / ShowServiceMenu / ShowServiceCtlError / OnServiceToggle /
//   DeleteService / DeleteEntry
// 物理搬迁：函数体零逻辑改动；已核实不依赖本文件匿名 namespace 或 PSFileGuard。
// ============================================================
#include "Page_Startup.h"

#include "IPageHost.h"             // [R5] HostPageHost()->PageById（任务子控件跨页转发）
#include "CustomControls.h"        // CClickRowUI（stcat_/strow_/svcrow_ 行）
#include "StringManager.h"         // StringMgr 本地化宏
#include "FrameWnd_Util.h"         // StyleCheckBox/ParsePrefix1/CopyTextToClipboard
#include "Win32DialogService.h"    // DlgOpenFile（新增启动项选文件）
#include "DiagnosticsService.h"    // SnapshotServicesToLog/ParseBootEventsToLog
#include "WmMessages.h"            // [R13 P0收敛] [LAG-FIX] WM_DIAG_LOG 定义在 WmMessages.h；UI 专属常量零使用
#include "SafeThread.h"           // 启动项诊断后台线程异常边界


void CPageStartup::BuildServices()
{
    // [AUDIT-P2] 原 185 行 / CC=30，按职责拆为 3 段：
    //   StAddServiceEmptyHint  枚举失败的空提示
    //   StFilterDriverServices 非 Win32 服务过滤（m_bShowDrivers 开关）
    //   StBuildServiceRow      单条服务行装配（原 140 行循环体）
    // 本函数保留枚举/日志/收尾编排，渲染顺序与属性逐字不变。

    m_vServices.clear();

    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"svc_list"));
    if (!pList) return;

    pList->RemoveAll();

    // S3：SCM 枚举（原 BuildServicesPage 枚举段；1=SCM 打开失败需显示空提示）
    int nEnum = StartupService::EnumerateServices(m_vServices);

    if (nEnum == 1)
    {
        StAddServiceEmptyHint(pList);
        return;
    }
    if (nEnum != 0)
        return;

    // [R117-1] 枚举成功分级日志（部分类型失败不阻断）
    if (nEnum == 0)
    {
        CDuiString sSvcLog;
        sSvcLog.Format(L"[svc] enum types=win32+kernel+fs+recognizer, total=%d (showDrivers=%d)",
                       static_cast<int>(m_vServices.size()), m_bShowDrivers ? 1 : 0);
        m_ctx.HostLog(sSvcLog.GetData());
    }

    // [R117-1] 默认隐藏驱动服务：非 Win32 类型行按开关过滤
    StFilterDriverServices();

    const int nDpiScale = m_ctx.HostDPIScale();
    const int nItemH = MulDiv(24, nDpiScale, 100);
    const int nDropH = MulDiv(180, nDpiScale, 100);

    for (size_t k = 0; k < m_vServices.size(); ++k)
        StBuildServiceRow(pList, k, nItemH, nDropH);

    RefreshInfoBar(1);

    // [R61-A] 服务状态后台快照：EnumServicesStatusEx → services_snapshot.log
    // [LAG-FIX] 二次全量枚举移至后台线程，避免首次切页时 UI 线程长时间阻塞
    PostStartupDiagAsync(m_ctx.HostHwnd(), 1);
}

// [AUDIT-P2] 从 BuildServices 提取：SCM 枚举失败时的空提示行
void CPageStartup::StAddServiceEmptyHint(CVerticalLayoutUI* pList)
{
    auto pEmpty = std::make_unique<CTextUI>(); // [C++17 R3]
    pEmpty->SetText(StringMgr.Get("optim.s41b67ced"));
    pEmpty->SetTextColor(tokens::Token(tokens::EToken::InkMuted));
    pEmpty->SetFixedHeight(28);
    pEmpty->SetAttribute(L"align", L"center");
    pList->Add(pEmpty.release());  // [C++17 R3] ownership to DuiLib
    return;
}

// [AUDIT-P2] 从 BuildServices 提取：非 Win32 服务过滤（m_bShowDrivers 关闭时）
void CPageStartup::StFilterDriverServices()
{
// [R117-1] 默认隐藏驱动服务：非 Win32 类型行按开关过滤
int nTotal = static_cast<int>(m_vServices.size());
if (!m_bShowDrivers)
{
    int nNew = 0;
    for (int i = 0; i < nTotal; ++i)
        if (m_vServices[i].nSvcType == SERVICE_WIN32)
            m_vServices[nNew++] = m_vServices[i];
    m_vServices.resize(nNew);
}
}

// [AUDIT-P2] 从 BuildServices 提取：单条服务行装配（原循环体 137 行）
// [R162] 从 StBuildServiceRow 抽出：SCM 服务状态 → 显示文本。
//   [R131-2] 覆盖 RUNNING / PAUSED / START_PENDING / STOP_PENDING / PAUSE_* 细分状态，
//   并在 QueryServiceConfig2 拿到恢复策略时叠加"延迟"副文本。
//   未识别状态回退到"未知"键，不回退到"手动"之类的误导性文案。
[[nodiscard]] CDuiString CPageStartup::StServiceStateText(const ServiceEntry& se)
{
    const wchar_t* pszState = StringMgr.Get("misc.s82977854");
    switch (se.nCurrentState)
    {
    case SERVICE_RUNNING:         pszState = StringMgr.Get("misc.sd679aea3");          break;
    case SERVICE_PAUSED:          pszState = StringMgr.Get("svc.s_state_paused");      break;
    case SERVICE_START_PENDING:   pszState = StringMgr.Get("svc.s_state_starting");    break;
    case SERVICE_STOP_PENDING:    pszState = StringMgr.Get("svc.s_state_stopping");    break;
    case SERVICE_PAUSE_PENDING:
    case SERVICE_CONTINUE_PENDING: pszState = StringMgr.Get("svc.s_state_pausing");    break;
    default:                      pszState = StringMgr.Get("misc.s82977854");          break;
    }
    if (se.nRestartDelay < 0) return CDuiString(pszState);
    CDuiString sState;
    sState.Format(StringMgr.Get("svc.s_restart_badge"), pszState, se.nRestartDelay);
    return sState;
}

// [R162] 从 StBuildServiceRow 抽出：[R117-1] 服务类型 → 显示文本。
//   非 Win32 的三类驱动用棕色显示（调用方判定颜色），此处只负责文案。
[[nodiscard]] const wchar_t* CPageStartup::StServiceKindText(DWORD dwSvcType)
{
    switch (dwSvcType)
    {
    case SERVICE_WIN32:               return StringMgr.Get("svc.kind_win32");
    case SERVICE_KERNEL_DRIVER:       return StringMgr.Get("svc.kind_kernel");
    case SERVICE_FILE_SYSTEM_DRIVER:  return StringMgr.Get("svc.kind_fs");
    case SERVICE_RECOGNIZER_DRIVER:   return StringMgr.Get("svc.kind_recognizer");
    default:                          return L"--";
    }
}

// [R169] 从 StBuildServiceRow 抽出：启动类型下拉（自动/手动/禁用）。
//   返回 118px 的外层容器（内含 106px 下拉），调用方直接 Add 进行。
//
//   [C4] 查询失败（nStartType == -1）时显示"未知"而不是默认"手动" ——
//   这是关键：把"查不到"显示成"手动"会让用户以为服务确实是手动启动，
//   而实际上可能是权限不足没读到配置。误导性比留空更严重。
//
//   外观属性（字号/边框/项高亮）属本页统一视觉规格，集中在此；
//   若将来别处也要画同样的下拉，直接复用，不会各自微调出不同观感。
[[nodiscard]] std::unique_ptr<CHorizontalLayoutUI>
CPageStartup::StBuildStartTypeCombo(size_t k, const ServiceEntry& se, int nItemH, int nDropH)
{
    auto pCell = std::make_unique<CHorizontalLayoutUI>(); // [C++17 R3]
    pCell->SetFixedWidth(118);

    auto pCombo = std::make_unique<CComboUI>(); // [C++17 R3]
    CDuiString sComboName;
    sComboName.Format(L"svccombo_%d", static_cast<int>(k));
    pCombo->SetName(sComboName.GetData());
    pCombo->SetFixedWidth(106);
    pCombo->SetFixedHeight(26);
    pCombo->SetFont(1);
    pCombo->SetTextColor(tokens::Token(tokens::EToken::Ink));
    RECT rcComboPad = { 8, 4, 20, 4 };
    pCombo->SetTextPadding(rcComboPad);
    SIZE szDrop = { 0, nDropH };
    pCombo->SetDropBoxSize(szDrop);
    pCombo->SetBkColor(tokens::Token(tokens::EToken::Surface));
    pCombo->SetAttribute(L"bordercolor", tokens::TokenHex(tokens::EToken::Border));
    pCombo->SetAttribute(L"bordersize", L"1");
    pCombo->SetAttribute(L"itemtextcolor", tokens::TokenHex(tokens::EToken::Ink));
    pCombo->SetAttribute(L"itembkcolor", tokens::TokenHex(tokens::EToken::Surface));
    pCombo->SetAttribute(L"itemselectedtextcolor", tokens::TokenHex(tokens::EToken::OnAccent));
    pCombo->SetAttribute(L"itemselectedbkcolor", tokens::TokenHex(tokens::EToken::Accent));   // [R210] 改令牌
    pCombo->SetAttribute(L"itemhotbkcolor", tokens::TokenHex(tokens::EToken::SurfaceHover));
    pCombo->SetAttribute(L"itemfont", L"1");
    pCombo->SetAttribute(L"itemalign", L"left");
    pCombo->SetAttribute(L"itemvalign", L"vcenter");
    pCombo->SetAttribute(L"itemendellipsis", L"true");
    pCombo->SetAttribute(L"itemtextpadding", L"8,3,8,3");

    const wchar_t* pszTypes[] = {
        StringMgr.Get("misc.s3aed2c11"),   // 自动
        StringMgr.Get("misc.s2a3e7f5c"),   // 手动
        StringMgr.Get("misc.s710ad08b"),   // 禁用
    };
    for (int t = 0; t < 3; ++t)
    {
        auto pItem = std::make_unique<CListLabelElementUI>(); // [C++17 R3]
        pItem->SetFixedHeight(nItemH);
        pItem->SetText(pszTypes[t]);
        pCombo->Add(pItem.release());  // [C++17 R3] ownership to DuiLib
    }

    // [C4] 查询失败时显示"未知"，绝不用"手动"兜底（会误导用户）
    if (se.nStartType < 0)
    {
        pCombo->SetText(StringMgr.Get("misc.s1622dc9b"));
    }
    else
    {
        const int nSel = se.nStartType == SERVICE_AUTO_START ? 0
                       : se.nStartType == SERVICE_DISABLED   ? 2 : 1;
        pCombo->SelectItem(nSel);
    }

    pCell->Add(pCombo.release());  // [C++17 R3] ownership to DuiLib
    return pCell;
}

void CPageStartup::StBuildServiceRow(CVerticalLayoutUI* pList, size_t k,
                                     int nItemH, int nDropH)
{
    ServiceEntry& se = m_vServices[k];
    // [C4] 服务行可点击（统一 hover），右键弹启动/停止/重启菜单
    auto pRow = std::make_unique<CClickRowUI>(); // [C++17 R3]
    pRow->SetFixedHeight(28);
    RECT rcRowInset = { 0, 0, 0, 0 };
    pRow->SetInset(rcRowInset);
    pRow->SetChildPadding(0);
    CDuiString sSvcRowName;
    sSvcRowName.Format(L"svcrow_%d", static_cast<int>(k));
    pRow->SetName(sSvcRowName.GetData());
    // [D3] 悬停看全名（显示名 + 服务键名）
    CDuiString sSvcTip = se.sDisplayName;
    if (!se.sDisplayName.IsEmpty()) sSvcTip += L"\n";
    sSvcTip += se.sName;
    pRow->SetToolTip(sSvcTip.GetData());
    auto pName = std::make_unique<CTextUI>(); // [C++17 R3]
    pName->SetText(se.sName.GetData());
    pName->SetFixedWidth(240);
    pName->SetFont(6);
    pName->SetTextColor(tokens::Token(tokens::EToken::Ink));
    pName->SetAttribute(L"valign", L"vcenter");
    pName->SetAttribute(L"textpadding", L"10,3,6,3");
    pName->SetAttribute(L"endellipsis", L"true");
    pName->SetMouseEnabled(false);
    pRow->Add(pName.release());  // [C++17 R3] ownership to DuiLib
    auto pDisp = std::make_unique<CTextUI>(); // [C++17 R3]
    pDisp->SetText(se.sDisplayName.GetData());
    pDisp->SetFixedWidth(0);
    pDisp->SetTextColor(tokens::Token(tokens::EToken::InkSecondary));
    pDisp->SetAttribute(L"valign", L"vcenter");
    pDisp->SetAttribute(L"textpadding", L"6,3,6,3");
    pDisp->SetAttribute(L"endellipsis", L"true");
    pDisp->SetMouseEnabled(false);
    pRow->Add(pDisp.release());  // [C++17 R3] ownership to DuiLib
    auto pState = std::make_unique<CTextUI>(); // [C++17 R3]
    CDuiString sStateName;
    sStateName.Format(L"svcstate_%d", static_cast<int>(k));
    pState->SetName(sStateName.GetData());
    // [R131-2] D3：SCM 细分状态文本 + 恢复策略副文本
    // [R162] 映射抽为 StServiceStateText（纯查表，可单独验证每个状态码）
    pState->SetText(StServiceStateText(se).GetData());
    pState->SetFixedWidth(90);
    pState->SetTextColor(se.bRunning ? tokens::Token(tokens::EToken::Success)
                                 : tokens::Token(tokens::EToken::InkMuted));
    pState->SetAttribute(L"align", L"center");
    pState->SetAttribute(L"valign", L"vcenter");
    pState->SetMouseEnabled(false);
    pRow->Add(pState.release());  // [C++17 R3] ownership to DuiLib
    // [R117-1] 服务类型列（Win32 服务 / 内核驱动 / 文件系统驱动 / 识别器驱动）
    auto pKind = std::make_unique<CTextUI>(); // [C++17 R3]
    // [R162] 类型映射抽为 StServiceKindText
    pKind->SetText(StServiceKindText(se.nSvcType));
    pKind->SetFixedWidth(88);
    pKind->SetTextColor(se.nSvcType == SERVICE_WIN32 ? tokens::Token(tokens::EToken::InkSecondary)
                                         : tokens::Token(tokens::EToken::Warn));
    pKind->SetAttribute(L"align", L"center");
    pKind->SetAttribute(L"valign", L"vcenter");
    pKind->SetMouseEnabled(false);
    pRow->Add(pKind.release());  // [C++17 R3] ownership to DuiLib
    // [R169] 启动类型下拉抽为 StBuildStartTypeCombo（110→约 70 行）
    pRow->Add(StBuildStartTypeCombo(k, se, nItemH, nDropH).release());

    if (m_bHideDisabled && se.nStartType == SERVICE_DISABLED)
        pRow->SetVisible(false);
    pList->Add(pRow.release());  // [C++17 R3] ownership to DuiLib
}


void CPageStartup::AddItem()
{
    CDuiString sFile;
    const wchar_t* pszFilter = StringMgr.Get("misc.h3b97");
    if (!Win32DialogService::DlgOpenFile(sFile, pszFilter, StringMgr.Get("misc.hf771")))
        return;
    // S3：生成值名并写入 HKCU\...\Run
    CDuiString sName;
    int nRes = StartupService::AddCurrentUserRunItem(sFile.GetData(), sName);
    if (nRes == 1)
    {
        m_ctx.HostAlert(StringMgr.Get("error.title"), StringMgr.Get("misc.h3b7b"), MB_ICONERROR);
        return;
    }
    if (nRes == 2)
    {
        m_ctx.HostAlert(StringMgr.Get("error.title"), StringMgr.Get("misc.hb2cb"), MB_ICONERROR);
        m_ctx.HostLog(StringMgr.Get("misc.h5c60"));
        return;
    }
    m_ctx.HostLog(StringMgr.Get("misc.h4112"));
    m_ctx.HostLog(sFile.GetData());
    // 刷新启动项列表
    BuildStartup();
    // 显示成功 Toast 提示（应用内）
    CDuiString sToastTitle = StringMgr.Get("misc.h4b1e");
    CDuiString sToastText;
    sToastText.Format(StringMgr.Get("misc.h41df"), sName.GetData());
    m_ctx.HostToast(sToastTitle.GetData(), sToastText.GetData(), 0);
}

// [R175] 从 OnServiceStartType 抽出：设置启动类型失败后把下拉退回真实值。
//   关键分支：se.nStartType < 0 表示当初就没查到配置 ——
//   此时回滚成"未知"文本，而不是选中某一项。若硬选"手动"，
//   用户会以为该项本来就是手动启动，而真实原因是权限不足没读到。
void CPageStartup::StRollbackStartTypeCombo(CComboUI* pCombo, const ServiceEntry& se)
{
    if (!pCombo) return;
    if (se.nStartType < 0)
    {
        pCombo->SetText(StringMgr.Get("misc.s1622dc9b"));
        return;
    }
    const int nBack = se.nStartType == SERVICE_AUTO_START ? 0
                    : se.nStartType == SERVICE_DISABLED   ? 2 : 1;
    pCombo->SelectItem(nBack);
}

void CPageStartup::OnServiceStartType(int k, int iSel)
{
    if (k < 0 || k >= static_cast<int>(m_vServices.size())) return;
    DWORD dwStart = iSel == 0 ? SERVICE_AUTO_START
                  : iSel == 2 ? SERVICE_DISABLED : SERVICE_DEMAND_START;
    ServiceEntry& se = m_vServices[k];
    if (se.nStartType == static_cast<int>(dwStart)) return;
    // S3：ChangeServiceConfig
    DWORD dwErr = 0;
    int nRes = StartupService::SetServiceStartType(se.sName.GetData(), dwStart, dwErr);
    CDuiString sComboName;
    sComboName.Format(L"svccombo_%d", k);
    CComboUI* pCombo = static_cast<CComboUI*>(m_ctx.HostFindControl(sComboName.GetData()));
    if (nRes == 1)
    {
        m_ctx.HostLog(StringMgr.Get("optim.s4dca6d75"));
        return;
    }
    if (nRes != 0)
    {
        CDuiString sLog;
        sLog.Format(StringMgr.Get("optim.s47095854"), se.sName.GetData(), dwErr);
        m_ctx.HostLog(sLog.GetData());
        // revert combo selection
        // [R175] 回滚抽为 StRollbackStartTypeCombo，嵌套 4→3
        StRollbackStartTypeCombo(pCombo, se);
        return;
    }
    se.nStartType = dwStart;
    CDuiString sLog;
    const wchar_t* pszType = iSel == 0 ? StringMgr.Get("misc.s3aed2c11") : iSel == 2 ? StringMgr.Get("misc.s710ad08b") : StringMgr.Get("misc.s2a3e7f5c");
    sLog.Format(StringMgr.Get("optim.s4761decf"), se.sName.GetData(), pszType);
    m_ctx.HostLog(sLog.GetData());
    ApplyFilter();
}

void CPageStartup::ShowServiceMenu(int k)
{
    if (k < 0 || k >= static_cast<int>(m_vServices.size())) return;
    ServiceEntry& se = m_vServices[k];
    HMENU hMenu = ::CreatePopupMenu();
    ::AppendMenu(hMenu, MF_STRING | (se.bRunning ? MF_GRAYED : 0), 4401, StringMgr.Get("misc.s21647f47"));
    ::AppendMenu(hMenu, MF_STRING | (se.bRunning ? 0 : MF_GRAYED), 4402, StringMgr.Get("misc.s2df4645c"));
    ::AppendMenu(hMenu, MF_STRING | (se.bRunning ? 0 : MF_GRAYED), 4403, StringMgr.Get("misc.s4c517e1f"));
    // [R80] 暂停 / 继续：仅处于运行中的服务可暂停，暂停中可继续
    ::AppendMenu(hMenu, MF_STRING | (se.bRunning ? 0 : MF_GRAYED), 4404, StringMgr.Get("svc.s1f1a1b2"));
    ::AppendMenu(hMenu, MF_STRING | (se.bRunning ? MF_GRAYED : 0), 4405, StringMgr.Get("svc.s2f2a2b3"));
    ::AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenu(hMenu, MF_STRING, 4406, StringMgr.Get("svc.s3f3a4b5"));
    // [R111] 复制服务名到剪贴板
    ::AppendMenu(hMenu, MF_STRING, 4407, StringMgr.Get("svc.s_menu_copy_name"));
    ::AppendMenu(hMenu, MF_STRING, 4408, StringMgr.Get("svc.s_ctx_depgraph"));
    POINT pt; ::GetCursorPos(&pt);
    int iCmd = ::TrackPopupMenu(hMenu,
        TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
        pt.x, pt.y, 0, m_ctx.HostHwnd(), nullptr);
    ::DestroyMenu(hMenu);
    if (!iCmd) return;
    if (iCmd == 4406)
    {
        // [R80] 删除服务：先确认，再停止服务，然后 DeleteService
        DeleteService(k);
        return;
    }
    if (iCmd == 4407)
    {
        // [R111] 复制服务名
        FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), se.sName.GetData());
        m_ctx.HostToast(StringMgr.Get("svc.s_menu_copied_name"),
                        se.sName.GetData(), 0);
        return;
    }

    if (iCmd == 4408)
    {
        // [R-B25] 查看服务依赖图
        ShowServiceDepGraph(k);
        return;
    }
    // S3：OpenSCManager/OpenService/Start/Control/WaitSvcState 整段
    StartupService::SvcCtlResult r =
        StartupService::ControlServiceItem(se.sName.GetData(), iCmd);
    if (r.code != 0)
    {
        // [R125-2] 错误码 1-11 统一分级文案（ERROR_SERVICE_* 完整映射）
        ShowServiceCtlError(r.code, r.dwErr, se.sName.GetData());
        return;
    }
    // 刷新状态列与缓存
    se.bRunning = r.bWantRun;
    CDuiString sStateName;
    sStateName.Format(L"svcstate_%d", k);
    if (CTextUI* pState = static_cast<CTextUI*>(m_ctx.HostFindControl(sStateName.GetData())))
    {
        pState->SetText(r.bWantRun ? StringMgr.Get("misc.sd679aea3") : StringMgr.Get("misc.s82977854"));
        pState->SetTextColor(r.bWantRun ? tokens::Token(tokens::EToken::Success)
                                  : tokens::Token(tokens::EToken::InkMuted));
    }
    CDuiString sLog;
    sLog.Format(StringMgr.Get("optim.s2b91d321"),
                se.sName.GetData(),
                iCmd == 4401 ? StringMgr.Get("misc.s8e54ddfe")
               : iCmd == 4402 ? StringMgr.Get("misc.s095e938e")
               : iCmd == 4403 ? StringMgr.Get("misc.s01b4e06f")
               : iCmd == 4404 ? StringMgr.Get("svc.s7e7f8a9")
               : StringMgr.Get("svc.s8f8a9b1"));
    m_ctx.HostLog(sLog.GetData());
    if (CTabLayoutUI* pSt = static_cast<CTabLayoutUI*>(
            m_ctx.HostFindControl(L"startup_pages")))
    {
        if (pSt->GetCurSel() == 1) RefreshInfoBar(1);
    }
}

// [R125-2] 服务控制失败统一分级：ControlServiceItem 返回 code 1-11，
// 全部映射到 svc.s_err_* 专用文案，HostAlert 展示 + HostLog 记录。
void CPageStartup::ShowServiceCtlError(int code, DWORD dwErr, const wchar_t* pszName)
{
    CDuiString sErr;
    switch (code)
    {
    case 1:  sErr = StringMgr.Get("svc.s_err_scm"); break;
    case 2:  sErr.Format(StringMgr.Get("svc.s_err_start"), dwErr); break;
    case 3:  sErr.Format(StringMgr.Get("svc.s_err_stop"), dwErr); break;
    case 4:  sErr = StringMgr.Get("svc.s_err_stop_timeout"); break;
    case 5:  sErr.Format(StringMgr.Get("svc.s_err_restart"), dwErr); break;
    case 6:  sErr = StringMgr.Get("svc.s_err_state_timeout"); break;
    case 7:  sErr.Format(StringMgr.Get("svc.s_err_open"), pszName, dwErr); break;
    case 8:  sErr.Format(StringMgr.Get("svc.sf3a3b4c"), dwErr); break;
    case 9:  sErr.Format(StringMgr.Get("svc.sf4b4c5d"), dwErr); break;
    case 10: sErr = StringMgr.Get("svc.s5c5d6e7"); break;
    case 11: sErr = StringMgr.Get("svc.s6d6e7f8"); break;
    default: sErr.Format(StringMgr.Get("svc.s_err_unknown"), code, dwErr); break;
    }
    m_ctx.HostAlert(StringMgr.Get("optim.services"), sErr.GetData(), MB_ICONWARNING);
    CDuiString sLog;
    sLog.Format(L"[svc] ctl failed: %s code=%d err=%lu",
                pszName, code, dwErr);
    m_ctx.HostLog(sLog.GetData(), static_cast<int>(ELogLevel::LOG_ERROR));
}

// [R80] 删除服务：二次确认 → 若运行中先停止 → DeleteService。
// [R111-7] 服务行双击复用入口：执行 4401 启动 / 4402 停止，
// 失败分级 Toast + 日志，成功后刷新状态列与 InfoBar。
void CPageStartup::OnServiceToggle(int k, int iCmd)
{
    if (k < 0 || k >= static_cast<int>(m_vServices.size())) return;
    ServiceEntry& se = m_vServices[k];

    StartupService::SvcCtlResult r =
        StartupService::ControlServiceItem(se.sName.GetData(), iCmd);
    if (r.code != 0)
    {
        // [R125-2] 复用统一分级（补齐 8/9 暂停/继续失败）
        ShowServiceCtlError(r.code, r.dwErr, se.sName.GetData());
        return;
    }

    se.bRunning = r.bWantRun;
    CDuiString sStateName;
    sStateName.Format(L"svcstate_%d", k);
    if (CTextUI* pState = static_cast<CTextUI*>(m_ctx.HostFindControl(sStateName.GetData())))
    {
        pState->SetText(r.bWantRun ? StringMgr.Get("misc.sd679aea3")
                                   : StringMgr.Get("misc.s82977854"));
        pState->SetTextColor(r.bWantRun ? tokens::Token(tokens::EToken::Success)
                                  : tokens::Token(tokens::EToken::InkMuted));
    }

    CDuiString sLog;
    sLog.Format(StringMgr.Get("optim.s2b91d321"),
                se.sName.GetData(),
                iCmd == 4401 ? StringMgr.Get("misc.s8e54ddfe")
                             : StringMgr.Get("misc.s095e938e"));
    m_ctx.HostLog(sLog.GetData());

    if (CTabLayoutUI* pSt = static_cast<CTabLayoutUI*>(
            m_ctx.HostFindControl(L"startup_pages")))
    {
        if (pSt->GetCurSel() == 1) RefreshInfoBar(1);
    }
}

// [R168] 从 DeleteService 抽出：拼接依赖服务名列表（最多 5 条 + "以及另外 N 个"）。
//   为什么只列 5 条：某些系统服务（如 RPC 相关）有上百个依赖，
//   全列出来会把确认框撑成不可读的一屏，用户反而看不清重点。
//   显示名优先、回退服务名 —— 用户认"Print Spooler"不认"Spooler"。
[[nodiscard]] CDuiString CPageStartup::StBuildDepList(const std::vector<DepServiceEntry>& vDep)
{
    CDuiString sDepList;
    int nShown = 0;
    for (size_t di = 0; di < vDep.size() && nShown < 5; ++di, ++nShown)
    {
        if (di) sDepList += L"\r\n    - ";
        sDepList += vDep[di].sDisplayName.IsEmpty() ? vDep[di].sName : vDep[di].sDisplayName;
    }
    if (static_cast<int>(vDep.size()) > nShown)
    {
        CDuiString sMore;
        sMore.Format(StringMgr.Get("svc.dep_more"),
                     static_cast<int>(vDep.size() - nShown));
        sDepList += sMore;
    }
    return sDepList;
}

void CPageStartup::DeleteService(int k)
{
    if (k < 0 || k >= static_cast<int>(m_vServices.size())) return;
    ServiceEntry& se = m_vServices[k];
    CDuiString sAsk;
    sAsk.Format(StringMgr.Get("svc.s4f4b5c6d"), se.sName.GetData());
    // [R93] 删除服务前枚举依赖服务并追加提示
    std::vector<DepServiceEntry> vDep;
    DWORD dwDepErr = 0;
    StartupService::EnumerateDependentServices(se.sName.GetData(), vDep, dwDepErr);
    if (!vDep.empty())
    {
        // [R168] 依赖列表拼接抽为 StBuildDepList，文案迁语言包
        CDuiString sDeps;
        sDeps.Format(StringMgr.Get("svc.dep_confirm"), sAsk.GetData(),
                     static_cast<int>(vDep.size()), StBuildDepList(vDep).GetData());
        if (!m_ctx.HostConfirmWarn(StringMgr.Get("optim.sb94c25f3"), sDeps.GetData())) return;
    }
    else if (!m_ctx.HostConfirmWarn(StringMgr.Get("optim.sb94c25f3"), sAsk.GetData()))
    {
        return;
    }
    if (se.bRunning)
    {
        StartupService::SvcCtlResult rStop =
            StartupService::ControlServiceItem(se.sName.GetData(), 4402);
        if (rStop.code != 0)
        {
            CDuiString sErr;
            sErr.Format(StringMgr.Get("svc.sa5a5c6d7"), rStop.code, rStop.dwErr);
            m_ctx.HostAlert(StringMgr.Get("optim.services"), sErr.GetData(), MB_ICONERROR);
            // [R113] 停止服务失败分级 Toast + static_cast<int>(ELogLevel::LOG_ERROR)
            CDuiString sStopLog;
            sStopLog.Format(L"[svc] stop failed before delete: %s code=%d err=%lu",
                            se.sName.GetData(), rStop.code, rStop.dwErr);
            m_ctx.HostLog(sStopLog.GetData(), static_cast<int>(ELogLevel::LOG_ERROR));
            CDuiString sStopToast;
            sStopToast.Format(L"%s | code=%d err=0x%08X",
                              sErr.GetData(), rStop.code, (unsigned)rStop.dwErr);
            m_ctx.HostToast(StringMgr.Get("svc.s_stop_fail_title"), sStopToast.GetData(), 2);
            return;
        }
    }
    DWORD dwErr = 0;
    int nRes = StartupService::DeleteServiceItem(se.sName.GetData(), dwErr);
    if (nRes != 0)
    {
        CDuiString sErr;
        sErr.Format(StringMgr.Get("svc.sb6b6d7e8"), nRes, dwErr);
        FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), sErr.GetData());
        m_ctx.HostAlert(StringMgr.Get("optim.services"), sErr.GetData(), MB_ICONERROR);
        // [R113] 删除服务失败分级 Toast + static_cast<int>(ELogLevel::LOG_ERROR)（错误码回传）
        CDuiString sLogErr;
        sLogErr.Format(L"[svc] delete failed: %s code=%d err=%lu",
                       se.sName.GetData(), nRes, dwErr);
        m_ctx.HostLog(sLogErr.GetData(), static_cast<int>(ELogLevel::LOG_ERROR));
        CDuiString sToastErr;
        sToastErr.Format(L"%s | code=%d err=0x%08X",
                         sErr.GetData(), nRes, (unsigned)dwErr);
        m_ctx.HostToast(StringMgr.Get("svc.s_delete_fail_title"), sToastErr.GetData(), 2);
        return;
    }
    CDuiString sLog;
    sLog.Format(L"[service] deleted: %s", se.sName.GetData());
    m_ctx.HostLog(sLog.GetData());
    BuildServices();
}

void CPageStartup::DeleteEntry(int k)
{
    if (k < 0 || k >= static_cast<int>(m_vStartup.size())) return;
    StartupEntry& e = m_vStartup[k];
    CDuiString sAsk;
    sAsk.Format(StringMgr.Get("optim.s84d50305"),
                e.sName.GetData());
    if (!m_ctx.HostConfirmWarn(StringMgr.Get("optim.sb94c25f3"), sAsk.GetData())) return;
    // S3：启动文件夹删文件 / BootExecute 重建 MULTI_SZ / Run 删值
    int nRes = StartupService::DeleteStartupEntry(e);
    if (nRes == 0)
    {
        CDuiString sLog;
        sLog.Format(L"[startup] deleted: %s", e.sName.GetData());
        m_ctx.HostLog(sLog.GetData());
        // 成功反馈以列表刷新为准，不再弹多余的“正在刷新”消息框
        BuildStartup();
    }
    else
    {
        const wchar_t* pszErr = StringMgr.Get("misc.s7e0f1854");
        if (nRes == 2)      pszErr = StringMgr.Get("error.sd9281916");
        else if (nRes == 3) pszErr = StringMgr.Get("optim.s087901eb");
        else if (nRes == 4) pszErr = StringMgr.Get("error.safaa9866");
        else if (nRes == 5) pszErr = StringMgr.Get("optim.se11e7c76");
        else if (nRes == 6) pszErr = StringMgr.Get("optim.s2bb980c8");
        // UI 全局错误提示统一：复制错误详情到剪贴板
        FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), pszErr);
        m_ctx.HostAlert(StringMgr.Get("optim.sb94c25f3"), pszErr, MB_ICONERROR);
    // [R113] 启动项删除失败分级 Toast + static_cast<int>(ELogLevel::LOG_ERROR)（错误码回传，替代单一信息条）
    CDuiString sLogErr;
    sLogErr.Format(L"[startup] delete failed: %s code=%d", e.sName.GetData(), nRes);
    m_ctx.HostLog(sLogErr.GetData(), static_cast<int>(ELogLevel::LOG_ERROR));
    CDuiString sToast;
    sToast.Format(L"%s | err=%d", pszErr, nRes);
    m_ctx.HostToast(StringMgr.Get("startup.s_delete_fail_title"), sToast.GetData(), 2);
    }
}
