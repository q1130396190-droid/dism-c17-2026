#include "stdafx.h"
#include "ThemeTokens.h"   // [R203] 颜色令牌（深色/浅色跟随）
// [R16][T10] 工具箱弹窗按 kind 拆分的渲染器分组文件（自 ToolDialogPage.cpp 物理搬迁，
// 零行为改动）。公共壳（Chrome/通用控件工厂/消息路由/作业框架）仍在 ToolDialogPage.cpp；
// 跨组共享的内部设施（tdlg:: 作业 Sink/进度窗/行单元格助手）见 ToolDlgInternal.h。
#include "ToolDialogPage.h"
#include "ToolDlgInternal.h"
#include "SafeThread.h"      // 工作线程异常边界
#include "BackgroundJobs.h"  // 后台作业统一收口（SEH 边界+退出等待）
// [R6] Service 层（零 UI 业务）
#include "ToolService/IToolServiceHost.h"
#include "ToolService/ToolServiceFactory.h"
#include "ToolService/LogDepGraphService.h"
#include "ToolService/ListService.h"
#include "ToolService/ConfirmService.h"
#include "ToolService/IsoMount.h"          // ISO 直连挂载原语（virtdisk）
#include "ToolService/IIsoImageService.h"  // wimgapi 作业工厂
#include "ToolService/WizDiskPart.h"       // diskpart/ESP 原语
#include "ToolService/IWizardService.h"    // 向导业务（BootFix…）
#include "DependencyGraphService.h"
#include "StringManager.h"
#include "HostInterfaces.h"   // static_cast<int>(ELogLevel::LOG_ERROR)/static_cast<int>(ELogLevel::LOG_WARN) 分级常量
#include "SystemInfoService.h"
#include <algorithm>
#include <set>
#include <cwchar>
#include <cwctype>
#include <lm.h>
#include <cstdio>
#include <string>
#include <shlwapi.h>
#include <thread>
#include <winioctl.h>
using i18n::CStringManager;
#pragma comment(lib, "netapi32.lib")
void CToolDialogPage::ClickAccount(const CDuiString& sName)
{
    if (sName.Left(8) == L"gridrow_") OnGridRow(sName);
    else if (sName == L"btn_acc_enable") AccountEnable();
    else if (sName == L"btn_acc_clear") AccountClearPwd();
}
void CToolDialogPage::ClickDualPath(const CDuiString& sName)
{
    if (sName == L"btn_dual_browse1") DualBrowse(0);
    else if (sName == L"btn_dual_browse2") DualBrowse(1);
    else if (sName == L"btn_dual_ok") DualOk();
}
void CToolDialogPage::ClickDelConfirm(const CDuiString& sName)
{
    if (sName == L"btn_del_ok") DelConfirmOk();
}
bool CToolDialogPage::LoadAccounts()
{
    // [R6-05f] NetUserEnum+排序下沉 IWizardService；Page 仅转呈现类型
    std::vector<toolsvc::IWizardService::TAcctInfo> vZ;
    uint32_t dwStatus = 0;
    if (!m_pWizSvc->AcctLoad(vZ, dwStatus))
    {
        CDuiString sLog;
        sLog.Format(StringMgr.Get("error.sb0679613"), static_cast<DWORD>(dwStatus));
        m_pHost->HostLog(sLog);
        return false;
    }
    m_acct.vList.clear();
    for (size_t i = 0; i < vZ.size(); ++i)
    {
        TAcctEntry e;
        e.sName   = vZ[i].sName.c_str();
        e.dwFlags = static_cast<DWORD>(vZ[i].dwFlags);
        e.sDesc   = vZ[i].sDesc.c_str();
        m_acct.vList.push_back(e);
    }
    return true;
}



void CToolDialogPage::BuildAccount()
{
    m_imgx.bMeta = false;
    if (!LoadAccounts())
    {
        m_pHost->HostLog(StringMgr.Get("error.s0b2c7fe7"));
    }



    std::vector<TGridCol> cols;
    cols.push_back(TGridCol(StringMgr.Get("misc.s819767ad"), 220));
    cols.push_back(TGridCol(StringMgr.Get("misc.s3fea7ca7"), 90));
    cols.push_back(TGridCol(StringMgr.Get("misc.s3bdd08ad"), -1));
    int nRows = static_cast<int>(m_acct.vList.size());
    if (nRows < 10) nRows = 10;
    m_pBody->Add(MkGrid(cols, nRows, 36, true));



    for (int i = 0; i < static_cast<int>(m_acct.vList.size()); ++i)
    {
        tdlg::SetRowCell(m_vRows[i], 0, m_acct.vList[i].sName);
        bool bDisabled = (m_acct.vList[i].dwFlags & UF_ACCOUNTDISABLE) != 0;
        tdlg::SetRowCell(m_vRows[i], 1, bDisabled ? StringMgr.Get("misc.s710ad08b") : StringMgr.Get("misc.s7854b52a"));
        tdlg::SetRowCell(m_vRows[i], 2, m_acct.vList[i].sDesc);
    }



    CHorizontalLayoutUI* pRow = std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pRow->SetFixedHeight(74);
    pRow->SetAttribute(L"inset", L"0,18,0,18");
    CControlUI* pSp = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pRow->Add
    pRow->Add(pSp);



    m_acct.pEnable = MkBtn(StringMgr.Get("misc.sa2d02a5e"), L"btn_acc_enable", false);
    m_acct.pEnable->SetFixedWidth(180);
    m_acct.pEnable->SetEnabled(false);
    pRow->Add(m_acct.pEnable);



    CControlUI* pGap = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pRow->Add
    pGap->SetFixedWidth(14);
    pRow->Add(pGap);



    m_acct.pClear = MkBtn(StringMgr.Get("misc.scc193ee9"), L"btn_acc_clear", false);
    m_acct.pClear->SetFixedWidth(180);
    m_acct.pClear->SetEnabled(false);
    pRow->Add(m_acct.pClear);



    m_pBody->Add(pRow);
}



void CToolDialogPage::AccountEnable()
{
    if (m_nSel < 0 || m_nSel >= static_cast<int>(m_acct.vList.size())) return;



    TAcctEntry& e = m_acct.vList[m_nSel];
    if ((e.dwFlags & UF_ACCOUNTDISABLE) == 0)
    {
        ::MessageBoxW(m_hWnd, StringMgr.Get("misc.s502052ae"),
                      StringMgr.Get("toolbox.account"), MB_OK | MB_ICONINFORMATION);
        return;
    }



    // [R6-05f] NetUserSetInfo 下沉 IWizardService
    NET_API_STATUS st = (NET_API_STATUS)m_pWizSvc->AcctEnable(
        e.sName.GetData(), (uint32_t)e.dwFlags);
    if (st != NERR_Success)
    {
        CDuiString sLog;
        sLog.Format(StringMgr.Get("error.se3ba7b86"),
                    static_cast<DWORD>(st),
                    st == ERROR_ACCESS_DENIED ? StringMgr.Get("hint.need_admin") : L"");
        m_pHost->HostLog(sLog);
        ::MessageBoxW(m_hWnd, sLog.GetData(),
                      StringMgr.Get("toolbox.account"), MB_OK | MB_ICONERROR);
        return;
    }



    m_pBody->RemoveAll();
    BuildAccount();
}



void CToolDialogPage::AccountClearPwd()
{
    if (m_nSel < 0 || m_nSel >= static_cast<int>(m_acct.vList.size())) return;



    CDuiString sName = m_acct.vList[m_nSel].sName;
    CDuiString sAsk;
    sAsk.Format(StringMgr.Get("confirm.s5a4243eb"), sName.GetData());
    if (::MessageBoxW(m_hWnd, sAsk.GetData(), StringMgr.Get("toolbox.account"),
                      MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;



    // [R6-05f] NetUserSetInfo 清空密码下沉 IWizardService
    NET_API_STATUS st = (NET_API_STATUS)m_pWizSvc->AcctClearPassword(
        sName.GetData());
    if (st != NERR_Success)
    {
        CDuiString sLog;
        sLog.Format(StringMgr.Get("error.s2f7024e5"),
                    static_cast<DWORD>(st),
                    st == ERROR_ACCESS_DENIED ? StringMgr.Get("hint.need_admin") : L"");
        m_pHost->HostLog(sLog);
        ::MessageBoxW(m_hWnd, sLog.GetData(),
                      StringMgr.Get("toolbox.account"), MB_OK | MB_ICONERROR);
        return;
    }



    m_pHost->HostLog(CDuiString(StringMgr.Get("misc.s54a04a7d")) + sName);
    m_pBody->RemoveAll();
    BuildAccount();
}



// ============================================================
// 7 编辑映像文件
// ============================================================
void CToolDialogPage::BuildDualPath()
{
    // 顶部描述
    CVerticalLayoutUI* pDesc = std::make_unique<CVerticalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pDesc->SetFixedHeight(28);
    pDesc->SetAttribute(L"inset", L"2,4,2,4");
    // [R16][T4] 标题/标签由 CConfirmService 持有（经 Open 注入），空=默认文案。
    const std::wstring& sDualTitle = SvcConfirm()->DualTitle();
    CTextUI* pNote = MkText(
        sDualTitle.empty() ? StringMgr.Get("dualpath.desc") : CDuiString(sDualTitle.c_str()),
        1, 0xFF606060);
    pNote->SetTextPadding(RECT{ 0, 0, 0, 0 });
    pDesc->Add(pNote);
    m_pBody->Add(pDesc);
    CControlUI* pGap0 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pGap0->SetFixedHeight(8);
    m_pBody->Add(pGap0);
    // 路径 1
    const std::wstring& sDualL1 = SvcConfirm()->DualLabel1();
    CTextUI* pL1 = MkText(
        sDualL1.empty() ? StringMgr.Get("dualpath.l1") : CDuiString(sDualL1.c_str()),
        1, 0xFF333333);
    pL1->SetFixedHeight(24);
    m_pBody->Add(pL1);
    CHorizontalLayoutUI* pRow1 = std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pRow1->SetFixedHeight(38);
    pRow1->Add(MkEdit(L"edt_dual_1"));
    CControlUI* pG1 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pRow1->Add
    pG1->SetFixedWidth(12);
    pRow1->Add(pG1);
    CButtonUI* pB1 = MkBtn(StringMgr.Get("general.browse"),
                           L"btn_dual_browse1", false);
    pB1->SetFixedWidth(104);
    pRow1->Add(pB1);
    m_pBody->Add(pRow1);
    CControlUI* pGap1 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pGap1->SetFixedHeight(8);
    m_pBody->Add(pGap1);
    // 路径 2
    const std::wstring& sDualL2 = SvcConfirm()->DualLabel2();
    CTextUI* pL2 = MkText(
        sDualL2.empty() ? StringMgr.Get("dualpath.l2") : CDuiString(sDualL2.c_str()),
        1, 0xFF333333);
    pL2->SetFixedHeight(24);
    m_pBody->Add(pL2);
    CHorizontalLayoutUI* pRow2 = std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pRow2->SetFixedHeight(38);
    pRow2->Add(MkEdit(L"edt_dual_2"));
    CControlUI* pG2 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pRow2->Add
    pG2->SetFixedWidth(12);
    pRow2->Add(pG2);
    CButtonUI* pB2 = MkBtn(StringMgr.Get("general.browse"),
                           L"btn_dual_browse2", false);
    pB2->SetFixedWidth(104);
    pRow2->Add(pB2);
    m_pBody->Add(pRow2);
    // 底部：确定/取消
    CControlUI* pGapL = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pGapL->SetMinHeight(16);
    m_pBody->Add(pGapL);
    AddBottomRow(MkBtn(StringMgr.Get("general.ok"), L"btn_dual_ok", true),
                 MkBtn(StringMgr.Get("general.cancel"), L"btn_cancel", false));
}
void CToolDialogPage::DualBrowse(int nIdx)
{
    CDuiString sResult;
    if (!m_pHost->HostBrowseFolder(sResult, StringMgr.Get("dualpath.browse")))
        return;
    CDuiString sName;
    sName.Format(L"edt_dual_%d", nIdx + 1);
    CEditUI* pE = static_cast<CEditUI*>(m_pm.FindControl(sName));
    if (pE) pE->SetText(sResult.GetData());
}
// [R6-03] 路径由 CConfirmService 持有；错误文案/弹窗留 Page。
[[nodiscard]] CDuiString CToolDialogPage::DualPath1() const
{
    return CDuiString(SvcConfirm()->Dual1().c_str());
}
[[nodiscard]] CDuiString CToolDialogPage::DualPath2() const
{
    return CDuiString(SvcConfirm()->Dual2().c_str());
}
void CToolDialogPage::DualOk()
{
    CEditUI* p1 = static_cast<CEditUI*>(m_pm.FindControl(L"edt_dual_1"));
    CEditUI* p2 = static_cast<CEditUI*>(m_pm.FindControl(L"edt_dual_2"));
    std::wstring s1 = p1 ? std::wstring(p1->GetText().GetData()) : std::wstring();
    std::wstring s2 = p2 ? std::wstring(p2->GetText().GetData()) : std::wstring();
    SvcConfirm()->SetDualPaths(s1, s2);
    if (!SvcConfirm()->DualValid())
    {
        ::MessageBoxW(m_hWnd, StringMgr.Get("dualpath.err"),
                      StringMgr.Get("dualpath.title"), MB_OK | MB_ICONWARNING);
        return;
    }
    Close(1);
}
void CToolDialogPage::BuildDelConfirm()
{
    // 顶部：警告标题
    CVerticalLayoutUI* pDesc = std::make_unique<CVerticalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pDesc->SetFixedHeight(28);
    pDesc->SetAttribute(L"inset", L"2,4,2,4");
    // [R16][T4] 标题由 CConfirmService 持有（经 Open 注入），空=默认文案。
    const std::wstring& sDelTitle = SvcConfirm()->DelTitle();
    CTextUI* pNote = MkText(
        sDelTitle.empty() ? StringMgr.Get("delconfirm.desc") : CDuiString(sDelTitle.c_str()),
        1, 0xFF606060);
    pNote->SetTextPadding(RECT{ 0, 0, 0, 0 });
    pDesc->Add(pNote);
    m_pBody->Add(pDesc);
    CControlUI* pGap0 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pGap0->SetFixedHeight(8);
    m_pBody->Add(pGap0);
    // 中部：危险项清单（可勾选/取消）
    CVerticalLayoutUI* pList = std::make_unique<CVerticalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pList->SetName(L"delconfirm_list");
    pList->SetBkColor(tokens::Token(tokens::EToken::Surface));
    pList->SetAttribute(L"bordercolor", tokens::TokenHex(tokens::EToken::Border));
    pList->SetAttribute(L"bordersize", L"1");
    pList->SetAttribute(L"inset", L"14,10,14,10");
    pList->SetAttribute(L"vscrollbar", L"true");
    m_pBody->Add(pList);
    // [R6-03] 危险项清单元数据由 CConfirmService 持有。
    const size_t nDel = SvcConfirm()->DelCount();
    for (size_t i = 0; i < nDel; ++i)
    {
        CTdlgCheck* pChk = std::make_unique<CTdlgCheck>().release(); // [C++17 R3] framework-owned via pList->Add
        pChk->Setup(CDuiString(SvcConfirm()->DelAt(i).c_str()), false);
        CDuiString sName;
        sName.Format(L"chk_del_%d", static_cast<int>(i));
        pChk->SetName(sName);
        pChk->SetFixedHeight(28);
        pChk->SetAttribute(L"endellipsis", L"false");
        pList->Add(pChk);
        if (i + 1 < nDel)
        {
            CControlUI* pGap = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pList->Add
            pGap->SetFixedHeight(4);
            pList->Add(pGap);
        }
    }
    // 底部：删除 / 取消（默认焦点在取消）
    CControlUI* pGapL = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pGapL->SetMinHeight(16);
    m_pBody->Add(pGapL);
    AddBottomRow(MkBtn(StringMgr.Get("delconfirm.ok"), L"btn_del_ok", true),
                 MkBtn(StringMgr.Get("general.cancel"), L"btn_cancel", false));
}
// [R16][T4] 旧 SetDelConfirmItems 已删除：清单/标题统一经 Open(ToolOpenArgs) 注入服务。
[[nodiscard]] bool CToolDialogPage::DelConfirmed() const
{
    return SvcConfirm()->Confirmed();
}
void CToolDialogPage::DelConfirmOk()
{
    SvcConfirm()->MarkConfirmed();
    Close(1);
}
// ============================================================
// [R-B53] 挂载 12 步向导（EToolDlgKind::TD_MOUNT，清单维度 4 缺失 2 0.3）
// 知识库基线（Dism知识库 17_镜像挂载UI）：挂载映像 / 挂载路径 /
// 只读模式 / 释放映像 / ImageFiles·MountPaths·ApplyPaths 记账 /
// MountFlags 日志 / PEImageManager.xml 引用 12 步字符串全集。
// 实现：wimgapi 直连 WIMBridge::Mount（替代 dism /Mount-Wim），
//       挂载失败时枚举 MountLedger 并跳过异常挂载点。
// ============================================================

