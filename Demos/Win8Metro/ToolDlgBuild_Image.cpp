#include "stdafx.h"
#include "ThemeTokens.h"   // [R203] 颜色令牌（深色/浅色跟随）
// [R16][T10] 工具箱弹窗按 kind 拆分的渲染器分组文件（自 ToolDialogPage.cpp 物理搬迁，
// 零行为改动）。公共壳（Chrome/通用控件工厂/消息路由/作业框架）仍在 ToolDialogPage.cpp；
// 跨组共享的内部设施（tdlg:: 作业 Sink/进度窗/行单元格助手）见 ToolDlgInternal.h。
#include "ToolDialogPage.h"
#include "ToolDlgInternal.h"
#include "WinHandles.h"   // [H-51] win::RegKey / win::FindDataHandle RAII for F11_raw_handle_close
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
// [R16][T10] 映像英文名→中文显示名（定义在本文件后段，ApplyIndexChanged 前置使用）
static CDuiString LocalizeEditionName(const CDuiString& sEn);
void CToolDialogPage::ClickCapture(const CDuiString& sName)
{
    if (sName == L"btn_cap_browse") CaptureBrowse();
    else if (sName == L"btn_cap_ok") CaptureOk();
}
void CToolDialogPage::ClickApply(const CDuiString& sName)
{
    if (sName == L"btn_apply_browse_wim") ApplyBrowseWim();
    else if (sName == L"btn_apply_browse_dir") ApplyBrowseDir();
    else if (sName == L"btn_apply_ok") ApplyOk();
}
[[nodiscard]] CDuiString CToolDialogPage::GetRegSZ(HKEY hRoot, const wchar_t* pszSub, const wchar_t* pszValue)
{
    CDuiString sOut;
    win::RegKey hk;
    if (::RegOpenKeyEx(hRoot, pszSub, 0, KEY_READ | KEY_WOW64_64KEY, hk.out())
        != ERROR_SUCCESS)
        return sOut;



    DWORD dwType = 0, dwCb = 0;
    if (::RegQueryValueEx(hk.get(), pszValue, nullptr, &dwType, nullptr, &dwCb)
            == ERROR_SUCCESS
        && dwType == REG_SZ && dwCb > 0)
    {
        std::vector<wchar_t> v(dwCb / sizeof(wchar_t) + 1, 0);
        if (::RegQueryValueEx(hk.get(), pszValue, nullptr, nullptr,
                              reinterpret_cast<BYTE*>(v.data()), &dwCb)
            == ERROR_SUCCESS)
        {
            sOut = v.data();
        }
    }
    // [C++17 R11] win::RegKey destructor closes the handle (removed double-close here)
    return sOut;
}



// ============================================================
// 0 另存为映像
// ============================================================
void CToolDialogPage::BuildCapture()
{
    CapFillSystemInfo();



    CapBuildInfoGrid();



    CControlUI* pGap1 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pGap1->SetFixedHeight(16);
    m_pBody->Add(pGap1);



    CHorizontalLayoutUI* pPath = std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pPath->SetFixedHeight(38);
    pPath->Add(MkEdit(L"edt_cap_path"));
    CControlUI* pGapW = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pPath->Add
    pGapW->SetFixedWidth(12);
    pPath->Add(pGapW);
    CButtonUI* pB = MkBtn(StringMgr.Get("general.browse"), L"btn_cap_browse", false);
    pB->SetFixedWidth(104);
    pPath->Add(pB);
    m_pBody->Add(pPath);



    CControlUI* pTop = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pTop->SetFixedHeight(12);
    m_pBody->Add(pTop);



    CHorizontalLayoutUI* pChk = std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pChk->SetFixedHeight(26);
    m_cap.pChkShadow = std::make_unique<CTdlgCheck>().release(); // [C++17 R3] framework-owned via pChk->Add
    // [R16][F-2] wimgapi 直连路径不支持 VSS 快照：禁用假勾选并就地说明，
    // 使勾选语义与行为一致（原实现默认勾选但被静默忽略，误导用户以为
    // 已用卷影复制；在线捕获锁定文件失败由 F-1/F-3 失败路径如实上报）。
    CDuiString sShadowTxt = StringMgr.Get("misc.s5912c568");
    sShadowTxt += L"（当前捕获路径不支持，已禁用）";
    m_cap.pChkShadow->Setup(sShadowTxt.GetData(), false);
    m_cap.pChkShadow->SetEnabled(false);
    pChk->Add(m_cap.pChkShadow);
    CControlUI* pGapC = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pChk->Add
    pGapC->SetFixedWidth(70);
    pChk->Add(pGapC);
    m_cap.pChkBoot = std::make_unique<CTdlgCheck>().release(); // [C++17 R3] framework-owned via pChk->Add
    m_cap.pChkBoot->Setup(StringMgr.Get("misc.s9f5141f4"), false);
    pChk->Add(m_cap.pChkBoot);
    CControlUI* pSp = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pChk->Add
    pChk->Add(pSp);
    m_pBody->Add(pChk);



    AddBottomRow(MkBtn(StringMgr.Get("general.ok"), L"btn_cap_ok", true),
                 MkBtn(StringMgr.Get("general.cancel"), L"btn_cancel", false));
}



// [R16][T10] 自 BuildCapture 机械抽取
void CToolDialogPage::CapFillSystemInfo()
{
    CDuiString sSub = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    CDuiString sBuild = GetRegSZ(HKEY_LOCAL_MACHINE, sSub, L"CurrentBuild");
    CDuiString sEdition = GetRegSZ(HKEY_LOCAL_MACHINE, sSub, L"EditionID");



    CDuiString sFamily;
    int nBuild = _ttoi(sBuild.GetData());
    sFamily = nBuild >= 22000 ? L"Windows 11" : L"Windows 10";



    struct TEdMap { const wchar_t* pszId; const wchar_t* pszCN; };
    const TEdMap maps[] =
    {
        { L"ProfessionalWorkstation", StringMgr.Get("system.pro_workstation") },
        { L"Professional", StringMgr.Get("system.pro") },
        { L"Core", StringMgr.Get("system.home") },
        { L"CoreConnected", StringMgr.Get("system.s88a3ac8b") },
        { L"ProfessionalEducation", StringMgr.Get("system.s142d2e9e") },
        { L"Education", StringMgr.Get("system.edu") },
        { L"Enterprise", StringMgr.Get("system.enterprise") },
        { L"EnterpriseS", StringMgr.Get("system.s9d87c47a") }
    };



    CDuiString sEdCN;
    for (int i = 0; i < _countof(maps); ++i)
    {
        if (lstrcmpiW(sEdition.GetData(), maps[i].pszId) == 0)
        {
            sEdCN = maps[i].pszCN;
            break;
        }
    }



    CDuiString sDisplay = sFamily;
    if (!sEdCN.IsEmpty()) { sDisplay += L" "; sDisplay += sEdCN; }
    else if (!sEdition.IsEmpty()) { sDisplay += L" "; sDisplay += sEdition; }



    m_cap.sName = sDisplay;
    m_cap.sDesc = sDisplay;
    m_cap.sFlag = sEdition.IsEmpty() ? CDuiString(L"Professional") : sEdition;
}
// [R16][T10] 自 BuildCapture 机械抽取
void CToolDialogPage::CapBuildInfoGrid()
{
    std::vector<TGridCol> cols;
    cols.push_back(TGridCol(StringMgr.Get("general.name"), 160));
    cols.push_back(TGridCol(StringMgr.Get("misc.sfe7509e0"), -1));
    m_pBody->Add(MkGrid(cols, 10, 36, false));



    const wchar_t* pszLabels[5] =
    {
        StringMgr.Get("toolbox.s46a27cf4"), StringMgr.Get("toolbox.sd772c795"), StringMgr.Get("misc.sfdf6f7f6"),
        StringMgr.Get("misc.sec7f21b1"), StringMgr.Get("toolbox.se2ff25b9")
    };
    tdlg::SetRowCell(m_vRows[0], 0, pszLabels[0]);
    tdlg::SetRowCell(m_vRows[0], 1, m_cap.sName);
    tdlg::SetRowCell(m_vRows[1], 0, pszLabels[1]);
    tdlg::SetRowCell(m_vRows[1], 1, m_cap.sDesc);
    tdlg::SetRowCell(m_vRows[2], 0, pszLabels[2]);
    tdlg::SetRowCell(m_vRows[2], 1, m_cap.sName);
    tdlg::SetRowCell(m_vRows[3], 0, pszLabels[3]);
    tdlg::SetRowCell(m_vRows[3], 1, m_cap.sDesc);
    tdlg::SetRowCell(m_vRows[4], 0, pszLabels[4]);
    tdlg::SetRowCell(m_vRows[4], 1, m_cap.sFlag);
}
void CToolDialogPage::CaptureBrowse()
{
    CDuiString sOut;
    if (!m_pHost->HostSaveFile(
            sOut,
            StringMgr.Get("toolbox.sf39f80db"),
            StringMgr.Get("edit.s1bef08ed"),
            L"Backup.wim")) return;



    CEditUI* pE = static_cast<CEditUI*>(m_pm.FindControl(L"edt_cap_path"));
    if (pE) pE->SetText(sOut.GetData());
}



void CToolDialogPage::CaptureOk()
{
    CEditUI* pE = static_cast<CEditUI*>(m_pm.FindControl(L"edt_cap_path"));
    CDuiString sOut = pE ? pE->GetText() : CDuiString(L"");
    sOut.Trim();
    if (sOut.IsEmpty())
    {
        ::MessageBoxW(m_hWnd, StringMgr.Get("toolbox.s220879aa"),
                      StringMgr.Get("toolbox.save_as"), MB_OK | MB_ICONWARNING);
        return;
    }



    int nDot = sOut.ReverseFind(L'.');
    CDuiString sExt = nDot >= 0 ? sOut.Mid(nDot) : L"";
    sExt.MakeLower();
    bool bEsd = (sExt == L".esd");
    if (!bEsd && sExt != L".wim")
    {
        ::MessageBoxW(m_hWnd,
                      StringMgr.Get("toolbox.s07827c61"),
                      StringMgr.Get("toolbox.save_as"), MB_OK | MB_ICONWARNING);
        return;
    }



    bool bShadow = m_cap.pChkShadow && m_cap.pChkShadow->Checked();
    bool bBoot = m_cap.pChkBoot && m_cap.pChkBoot->Checked();
    // [R16][F-2] VSS 选项在 wimgapi 直连路径被忽略：显式告警，消除静默失效
    if (bShadow)
        m_pHost->HostLog(L"已勾选“使用卷影复制”，但 wimgapi 直连路径不支持 VSS 快照，该选项已被忽略（在线备份可能因锁定文件失败）", static_cast<int>(ELogLevel::LOG_WARN));



    // ===== [2026-10-12] 后台线程执行（std::thread+detach，wimgapi 0-100% 真实
    //        进度进模态进度窗，完成后 PostMessage 回 UI 弹成功/失败框）=====
    if (!m_pImgSvc->WimReady())
    {
        m_pHost->HostLog(L"[wimgapi] wimgapi.dll 不可用，系统备份中止（禁止 dism 回退）", static_cast<int>(ELogLevel::LOG_ERROR));
        ::MessageBoxW(m_hWnd, L"wimgapi.dll 不可用，无法执行系统备份。",
                      StringMgr.Get("toolbox.save_as"), MB_OK | MB_ICONERROR);
        return;
    }
    std::wstring sNameW(m_cap.sName.GetData());
    std::wstring sDescW(m_cap.sDesc.GetData());
    // [R6-05e] R-B90 环境变量展开下沉 IIsoImageService
    std::wstring sSrcRootW = m_pImgSvc->SystemDriveRoot();
    // ESD→LZMS 高压缩（"保存为其他格式 ESD"走同一真实进度路径）；WIM→LZX 兼容。
    // VSS 快照不在 wimgapi 直连范围（bShadow 仅 dism 时代使用，原生路径忽略）。
    DWORD dwCompress = bEsd ? toolsvc::IMG_COMPRESS_LZMS : toolsvc::IMG_COMPRESS_LZX;
    CDuiString sTitle = StringMgr.Get("toolbox.save_as");
    CDuiString sProgress;
    sProgress.Format(L"正在备份 %s，请勿关闭窗口…\r\n目标文件：%s",
                     CDuiString(sSrcRootW.c_str()).GetData(), sOut.GetData());
    CDuiString sOkMsg;
    sOkMsg.Format(L"系统备份完成。\r\n\r\n映像文件：%s\r\n压缩格式：%s",
                  sOut.GetData(),
                  bEsd ? L"ESD（LZMS 高压缩）" : L"WIM（LZX）");
    std::wstring wOut(sOut.GetData());
    // [R6-05e] 捕获作业体下沉 IIsoImageService（线程/进度盾仍在本页）
    RunImageXJob(sProgress, sTitle, sOkMsg, false, false,
        m_pImgSvc->MakeCaptureJob(wOut, sSrcRootW, sNameW, sDescW,
                                  dwCompress, bBoot),
        0 /*JOB_CAPTURE*/);
}



// ============================================================
// 1 释放映像
// ============================================================
void CToolDialogPage::BuildApply()
{
    std::vector<TGridCol> cols;
    cols.push_back(TGridCol(StringMgr.Get("general.name"), 160));
    cols.push_back(TGridCol(StringMgr.Get("misc.sfe7509e0"), -1));
    m_pBody->Add(MkGrid(cols, 10, 34, false));



    const wchar_t* pszLabels[9] =
    {
        L"映像名称", L"映像说明", L"显示名称",
        L"显示说明", L"映像标志", L"系统体系",
        L"创建日期", L"展开空间", L"系统版本"
    };
    m_apply.vInfoVal.clear();
    for (int i = 0; i < 9; ++i)
    {
        tdlg::SetRowCell(m_vRows[i], 0, pszLabels[i]);
        m_apply.vInfoVal.push_back(static_cast<CTextUI*>(m_vRows[i]->GetItemAt(1)));
    }



    CControlUI* pGap1 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pGap1->SetFixedHeight(18);
    m_pBody->Add(pGap1);



    m_pBody->Add(ApplyBuildInputBox());



    AddBottomRow(MkBtn(StringMgr.Get("general.ok"), L"btn_apply_ok", true),
                 MkBtn(StringMgr.Get("general.cancel"), L"btn_cancel", false));
}



// [R16][T10] 自 BuildApply 机械抽取
CVerticalLayoutUI* CToolDialogPage::ApplyBuildInputBox()
{
    CVerticalLayoutUI* pBox = std::make_unique<CVerticalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pBox->SetFixedHeight(206);
    pBox->SetAttribute(L"bordercolor", tokens::TokenHex(tokens::EToken::Border));
    pBox->SetAttribute(L"bordersize", L"1");
    pBox->SetBkColor(tokens::Token(tokens::EToken::Surface));
    pBox->SetAttribute(L"inset", L"16,14,16,14");



    CHorizontalLayoutUI* pRow1 = std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via pBox->Add
    pRow1->SetFixedHeight(36);
    CTextUI* pL1 = MkText(StringMgr.Get("toolbox.s0ab073e4"), 1, 0xFF1A1A1A);
    pL1->SetFixedWidth(100);
    pL1->SetMouseEnabled(false);
    pRow1->Add(pL1);
    m_apply.pCombo = MkCombo(L"cmb_apply_index", 170);
    pRow1->Add(m_apply.pCombo);
    pBox->Add(pRow1);



    CControlUI* pS1 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pBox->Add
    pS1->SetFixedHeight(12);
    pBox->Add(pS1);



    // [R16][T10] 行2/行3 同构，机械抽取为 AoMkBrowseRow
    pBox->Add(AoMkBrowseRow(L"edt_apply_wim", L"btn_apply_browse_wim", nullptr));



    CControlUI* pS2 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pBox->Add
    pS2->SetFixedHeight(12);
    pBox->Add(pS2);



    pBox->Add(AoMkBrowseRow(L"edt_apply_dir", L"btn_apply_browse_dir", L"C:"));



    CControlUI* pS3 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pBox->Add
    pS3->SetFixedHeight(12);
    pBox->Add(pS3);



    // [R16][T10] 复选行机械抽取为 ApplyBuildChkRow
    pBox->Add(ApplyBuildChkRow());
    return pBox;
}
// [R16][T10] 自 ApplyBuildInputBox 机械抽取：编辑框 + 浏览按钮行
CHorizontalLayoutUI* CToolDialogPage::AoMkBrowseRow(const wchar_t* pszEdit, const wchar_t* pszBtn, const wchar_t* pszText)
{
    CHorizontalLayoutUI* pRow = std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via pBox->Add
    pRow->SetFixedHeight(38);
    CEditUI* pEdit = MkEdit(pszEdit);
    if (pszText) pEdit->SetText(pszText);
    pRow->Add(pEdit);
    CControlUI* pG = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pRow->Add
    pG->SetFixedWidth(10);
    pRow->Add(pG);
    CButtonUI* pB = MkBtn(StringMgr.Get("general.browse"), pszBtn, false);
    pB->SetFixedWidth(104);
    pRow->Add(pB);
    return pRow;
}
// [R16][T10] 自 ApplyBuildInputBox 机械抽取：五选项复选行
CHorizontalLayoutUI* CToolDialogPage::ApplyBuildChkRow()
{
    CHorizontalLayoutUI* pRow4 = std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via pBox->Add
    pRow4->SetFixedHeight(30);
    const wchar_t* pszChk[5] =
    {
        L"WIMBoot", L"Compact", L"WindowsToGo",
        StringMgr.Get("edit.sa4b8e087"), StringMgr.Get("misc.sb70b53b8")
    };
    // 各复选框按文字给足固定宽度，中间间隔为弹性：不再折行、不再被挤压
    const int nChkW[5] = { 92, 96, 130, 104, 92 };
    for (int i = 0; i < 5; ++i)
    {
        m_apply.apChk[i] = std::make_unique<CTdlgCheck>().release(); // [C++17 R3] framework-owned via pRow4->Add
        m_apply.apChk[i]->Setup(pszChk[i], false);
        m_apply.apChk[i]->SetFixedWidth(nChkW[i]);
        pRow4->Add(m_apply.apChk[i]);
        if (i < 4)
        {
            CControlUI* pF = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pRow4->Add
            pRow4->Add(pF);
        }
    }
    return pRow4;
}
// 把可能的 .iso 输入归一为真实 WIM/ESD 路径（原版：不把 ISO 交给 wimgapi/dism）。
bool CToolDialogPage::ApplyNormalizeImage(const CDuiString& sIn, CDuiString& sOut,
                                      CDuiString* psErr)
{
    CDuiString sInT = sIn;
    sInT.Trim();
    const wchar_t* pExt = ::PathFindExtension(sInT.GetData());
    if (!pExt || lstrcmpiW(pExt, L".iso") != 0)
    {
        sOut = sInT;      // 普通 wim/esd：原样
        return true;
    }
    // [R6-04] 挂载/解析/槽位所有权下沉 IIsoImageService（槽位 0=Apply）
    std::wstring wInner, wErr;
    if (!m_pImgSvc->MountIsoSlot(0, sInT.GetData(), wInner, wErr))
    {
        if (psErr) *psErr = wErr.c_str();
        return false;
    }
    sOut = wInner.c_str();
    CDuiString sLog;
    sLog.Format(L"[imagex] ISO 透明挂载：%s -> %s（关闭窗口自动卸载）",
                sInT.GetData(), wInner.c_str());
    m_pHost->HostLog(sLog);
    return true;
}
void CToolDialogPage::ApplyBrowseWim()
{
    CDuiString sFile;
    if (!m_pHost->HostOpenFile(
            sFile,
            StringMgr.Get("toolbox.sc0b1042e"),
            StringMgr.Get("cleanup.s94dcb9cb"))) return;



    CEditUI* pE = static_cast<CEditUI*>(m_pm.FindControl(L"edt_apply_wim"));
    // ISO 透明挂载 → 真实内部 WIM/ESD；普通 wim/esd 原样
    CDuiString sNorm, sNormErr;
    if (!ApplyNormalizeImage(sFile, sNorm, &sNormErr))
    {
        ::MessageBoxW(m_hWnd, sNormErr.GetData(),
                      StringMgr.Get("cleanup.release_img"), MB_OK | MB_ICONWARNING);
        return;
    }
    if (pE) pE->SetText(sNorm.GetData());
    m_apply.vIdx.clear();
    CDuiString sErr;
    if (!m_pHost->HostQueryWim(sNorm, m_apply.vIdx, &sErr) || m_apply.vIdx.empty())
    {
        CDuiString sMsg = StringMgr.Get("error.s1f6bf2f0");
        if (!sErr.IsEmpty()) { sMsg += L"\r\n\r\n"; sMsg += sErr; }
        ::MessageBoxW(m_hWnd, sMsg.GetData(),
                      StringMgr.Get("cleanup.release_img"), MB_OK | MB_ICONWARNING);
        return;
    }



    if (m_apply.pCombo)
    {
        m_apply.pCombo->RemoveAll();
        for (int i = 0; i < static_cast<int>(m_apply.vIdx.size()); ++i)
        {
            CDuiString sText;
            sText.Format(L"%d - %s",
                         m_apply.vIdx[i].nIndex,
                         m_apply.vIdx[i].sName.GetData());
            CListLabelElementUI* pIt = std::make_unique<CListLabelElementUI>().release(); // [C++17 R3] framework-owned via pCombo->Add
            pIt->SetFixedHeight(28);
            pIt->SetText(sText.GetData());
            m_apply.pCombo->Add(pIt);
        }
        m_apply.pCombo->SelectItem(0);
        ApplyIndexChanged();
    }
}



void CToolDialogPage::ApplyBrowseDir()
{
    CDuiString sDir;
    if (!m_pHost->HostBrowseFolder(sDir, StringMgr.Get("cleanup.s9da321ca"))) return;



    CEditUI* pE = static_cast<CEditUI*>(m_pm.FindControl(L"edt_apply_dir"));
    if (pE) pE->SetText(sDir.GetData());
}



void CToolDialogPage::ApplyIndexChanged()
{
    if (!m_apply.pCombo) return;
    int i = m_apply.pCombo->GetCurSel();
    for (int k = 0; k < static_cast<int>(m_apply.vInfoVal.size()); ++k)
    {
        if (!m_apply.vInfoVal[k]) continue;
        m_apply.vInfoVal[k]->SetText(L"");
    }
    if (i < 0 || i >= static_cast<int>(m_apply.vIdx.size())) return;



    const TDialogWimIndex& ix = m_apply.vIdx[i];
    // 单元格可能缺失，统一经判空保护写入
    auto SetCell = [&](int nCell, const wchar_t* pszVal) {
        if (nCell >= 0 && nCell < static_cast<int>(m_apply.vInfoVal.size()) && m_apply.vInfoVal[nCell])
            m_apply.vInfoVal[nCell]->SetText(pszVal);
    };
    // 0 映像名称 / 1 映像说明：英文原始
    SetCell(0, ix.sName.GetData());
    SetCell(1, ix.sDesc.GetData());
    // 2 显示名称 / 3 显示说明：中文翻译
    SetCell(2, LocalizeEditionName(ix.sName).GetData());
    SetCell(3, LocalizeEditionName(ix.sDesc).GetData());
    // 4 映像标志（EDITIONID）
    SetCell(4, ix.sEdition.GetData());
    // 5 系统体系
    SetCell(5, ix.sArch.GetData());
    // 6 创建日期
    SetCell(6, ix.sCreated.GetData());
    // 7 展开空间：按原版仅显示 "X.X GB"
    if (!ix.sSize.IsEmpty())
    {
        ULONGLONG ull = 0; bool bAny = false;
        for (const wchar_t* q = ix.sSize.GetData(); *q; ++q)
        {
            if (*q < L'0' || *q > L'9') break;
            ull = ull * 10 + static_cast<ULONGLONG>(*q - L'0');
            bAny = true;
        }
        if (bAny)
        {
            double gb = static_cast<double>(ull) / (1024.0 * 1024.0 * 1024.0);
            CDuiString sSz;
            sSz.Format(L"%.1f GB", gb);
            SetCell(7, sSz.GetData());
        }
        else
            SetCell(7, ix.sSize.GetData());
    }
    // 8 系统版本
    SetCell(8, ix.sVersion.GetData());
}



// ============================================================
// [2026-10-12] 释放映像后台任务上下文 + 工作函数（ApplyOk 专用）
// 格式化→wimgapi Apply（真实进度）→驱动注入→bcdboot/bootsect 引导链
// 全程在工作线程；日志经 tdlg::TdJobLog 回 UI，阶段文字经 tdlg::TdJobStage。
// ============================================================
struct TApplyJob
{
    HWND hNotify = nullptr;     // 主对话框（接收 WM_TDLG_JOBLOG）
    HWND hProg   = nullptr;     // 模态进度窗（接收 WM_TP_PROGRESS/STAGETEXT）
    // [R6-05g] 业务参数与作业执行均下沉 IWizardService
    toolsvc::TApplyJobArgs     a;
    toolsvc::IWizardService*   pSvc = nullptr;   // 非拥有（弹窗生命周期内有效）
    CDuiString sTitle;       // 完成/错误框标题
};


// [CRASHFIX] 释放映像作业线程（格式化卷→ApplyImageEx→驱动注入→引导链）：
// 替代裸 std::thread::detach。本作业是最高危作业（先格式化后写盘），
// 必须经双层异常边界且可被 WM_CLOSE 等待（G-CRS-01/02）。
static DWORD WINAPI ApplyJobThreadProc(void* pv)
{
    std::unique_ptr<TApplyJob> guard(reinterpret_cast<TApplyJob*>(pv)); // [C++17 R3] ownership from Start caller
    TApplyJob* j = guard.get();
    // [C++17 R3] helper PostHeapOrDelete always takes ownership (posts or deletes in place)
    tdlg::TJobResult* r = std::make_unique<tdlg::TJobResult>().release();
    r->nKind = 1;                 // Apply（OnJobDone 失败分级/引导修复框）
    r->sTitle = j->sTitle;
    r->bCloseOnOk = true;
    try
    {
        // [R6-05g] 作业体（格式化→释放→驱动注入→引导链）下沉 IWizardService；
        // sink 在工作线程内把进度/日志/阶段 Post 回 UI，与旧 TPostProg/
        // tdlg::TdJobLog/tdlg::TdJobStage 通道完全一致。
        // [R16-T9] 本地 TApplySink 收编为文件级统一 tdlg::TPostJobSink
        tdlg::TPostJobSink sink;
        sink.hDlg  = j->hNotify;
        sink.hProg = j->hProg;
        toolsvc::TApplyJobResult zr;
        if (j->pSvc)
            j->pSvc->ApplyRunJob(j->a, &sink, zr);
        else
            zr.sErrMsg = L"内部错误：向导服务未初始化。";
        r->bOk           = zr.bOk;
        r->sOkMsg        = zr.sOkMsg.c_str();
        r->sMsg          = zr.sErrMsg;
        r->bCloseOnOk    = zr.bCloseOnOk;
        r->bBootAttempted = zr.bBootAttempted;
        r->bBootOk       = zr.bBootOk;
        r->sBootErr      = zr.sBootErr.c_str();
    }
    catch (...)
    {
        r->bOk = false;
        if (r->sMsg.empty())
            r->sMsg = L"释放任务发生异常。";
    }
    // [GC-07] 统一收尾：投递失败由助手就地回收，不再各写一套
    BackgroundJobs::PostHeapOrDelete(j->hNotify, WM_TDLG_JOBDONE, 0, r);
    // [C++17 R3] thread ctx deleted by guard scope exit (ownership from Start caller)
    return 0;
}
// [R14-03] ApplyOk 拆分（原 317 行单函数）：ISO 透明挂载/目标盘挂载检测/
// 驱动目录询问/释放参数登记各抽 Ao* 辅助；校验与后台作业启动仍按原顺序留薄壳。
// 纯交互前置迁移，零行为改动。
void CToolDialogPage::ApplyOk()
{
    CEditUI* pWim = static_cast<CEditUI*>(m_pm.FindControl(L"edt_apply_wim"));
    CDuiString sWim = pWim ? pWim->GetText() : CDuiString(L"");
    sWim.Trim();
    // 兜底：用户直接在编辑框输入/粘贴 .iso（未走浏览）时同样透明挂载，
    // 否则原始 ISO 会被直接读取，报错误码 11 并误判"ESD 已加密"。
    if (!AoNormalizeIso(pWim, sWim)) return;
    CEditUI* pDir = static_cast<CEditUI*>(m_pm.FindControl(L"edt_apply_dir"));
    CDuiString sDir = pDir ? pDir->GetText() : CDuiString(L"");
    sDir.Trim();
    int nSel = m_apply.pCombo ? m_apply.pCombo->GetCurSel() : -1;
    if (sWim.IsEmpty() || sDir.IsEmpty() || nSel < 0
        || nSel >= static_cast<int>(m_apply.vIdx.size()))
    {
        ::MessageBoxW(m_hWnd,
                      StringMgr.Get("cleanup.sed6feef7"),
                      StringMgr.Get("cleanup.release_img"), MB_OK | MB_ICONWARNING);
        return;
    }
    bool bWIMBoot = m_apply.apChk[0] && m_apply.apChk[0]->Checked();
    bool bCompact = m_apply.apChk[1] && m_apply.apChk[1]->Checked();
    bool bWTG     = m_apply.apChk[2] && m_apply.apChk[2]->Checked();
    bool bAddBoot = m_apply.apChk[3] && m_apply.apChk[3]->Checked();
    bool bFormat  = m_apply.apChk[4] && m_apply.apChk[4]->Checked();
    if (sDir.GetAt(sDir.GetLength() - 1) != L'\\')
        sDir += L"\\";
    CDuiString sTgtDr = sDir.Left(2);
    int nIndex = m_apply.vIdx[nSel].nIndex;
    CDuiString sName = m_apply.vIdx[nSel].sName;
    (void)sName;
    // P1-4a: 目标盘挂载检测 —— 如果目标盘已在系统其他进程挂载（如 wim/iso/dism 挂载），
    // Apply-Image 会因卷锁失败或造成不一致。
    if (!AoWarnIfTargetMounted(sTgtDr)) return;
    // P1-4c: 驱动注入前置询问 —— 若用户勾选 WIMBoot/WindowsToGo 或 Compact，
    // 建议在 Apply 前把额外驱动目录一并注入，避免系统装完后缺硬件驱动
    CDuiString sDrvPath;   // [P0 修复] 原始驱动目录（wimgapi 主成功路径注入用）
    AoAskDriverPath(bWIMBoot, bWTG, bCompact, sDrvPath);
    // ===== [R129-1] D3 证据：注册表直写（RegOpenKeyExW + RegSetValueExW）记录最近释放参数 =====
    // 释放前把 WIM/索引/目标目录/参数旗标写到 HKCU\Software\DismTool\LastApply，
    // 供下次打开对话框预填与故障追溯（GetLastError 失败不影响主流程）。
    DWORD dwFlags = (bWIMBoot ? 1u : 0) | (bCompact ? 2u : 0)
                  | (bWTG ? 4u : 0) | (bAddBoot ? 8u : 0) | (bFormat ? 16u : 0);
    AoRecordLastApply(sWim, sDir, nIndex, dwFlags);
    // ===== [2026-10-12] 后台线程执行（格式化→释放→驱动注入→引导链）=====
    // wimgapi 0-100% 真实进度进模态进度窗；所有交互（ISO 挂载/挂载点检测/
    // 驱动目录询问/注册表记录）已在上方 UI 线程完成，重型操作全部进工作线程。
    if (!m_pImgSvc->WimReady())
    {
        m_pHost->HostLog(L"[wimgapi] wimgapi.dll 不可用，释放映像中止（禁止 dism 回退）", static_cast<int>(ELogLevel::LOG_ERROR));
        ::MessageBoxW(m_hWnd, L"wimgapi.dll 不可用，无法释放映像。",
                      StringMgr.Get("cleanup.release_img"), MB_OK | MB_ICONERROR);
        return;
    }
    {
        DWORD dwApply = 0;
        if (bWIMBoot) dwApply |= toolsvc::IMG_FLAG_WIM_BOOT;
        if (bCompact) dwApply |= toolsvc::IMG_FLAG_APPLY_COMPACT;
        // [C++17 阶段3] 线程 ctx：所有权移交 ApplyJobThreadProc，Start 失败自删
        auto ctx = std::make_unique<TApplyJob>(); // [C++17 R3] ownership to thread proc on successful start
        TApplyJob* j = ctx.get();
        // [R6-05g] 业务参数装入服务层 args（尾反斜杠由服务保证，这里仍按基线补）
        j->a.sWim     = sWim.GetData();
        j->a.sDir     = sDir.GetData();
        if (!j->a.sDir.empty() && j->a.sDir.back() != L'\\') j->a.sDir += L'\\';
        j->a.sTgtDr   = sTgtDr.GetData();
        j->a.sDrvPath = sDrvPath.GetData();
        j->a.nIndex   = (uint32_t)nIndex;
        j->a.dwApply  = dwApply;
        j->a.bWIMBoot = bWIMBoot;
        j->a.bCompact = bCompact;
        j->a.bWTG     = bWTG;
        j->a.bAddBoot = bAddBoot;
        j->a.bFormat  = bFormat;
        j->a.sSysDrvWarn = StringMgr.Get("system.s1f155da7");
        j->pSvc      = m_pWizSvc.get();
        j->sTitle    = StringMgr.Get("cleanup.release_img");
        CDuiString sProg;
        sProg.Format(L"正在释放映像到 %s\\，请勿关闭窗口…", sTgtDr.GetData());
        BeginTaskShield(sProg, false);
        j->hNotify = m_hWnd;
        j->hProg   = m_hWnd;   // [B4] 进度消息改投对话框内嵌 DuiLib 面板（替代旧原生进度窗）
        // [CRASHFIX] 裸 detach 迁移为受 SEH 边界保护、可被 WM_CLOSE 等待的统一作业
        if (!BackgroundJobs::Start(&ApplyJobThreadProc, ctx.get(), "apply-image"))
        {
            EndTaskShield();
            ::MessageBoxW(m_hWnd, L"后台作业启动失败（线程资源不足），释放已中止。",
                          j->sTitle.GetData(), MB_OK | MB_ICONERROR);
            ctx.reset(); // [C++17 R3] delete holder at the original failure-cleanup site
        }
        else
        {
            ctx.release(); // [C++17 R3] thread proc deletes via its unique_ptr guard
        }
    }
}
// [R14-03] 输入路径 ISO 透明挂载归一化；实际发生切换时刷新映像索引下拉。
// 返回 false 表示归一化失败（已弹错误框），调用方应中止 Apply。
bool CToolDialogPage::AoNormalizeIso(CEditUI* pWim, CDuiString& sWim)
{
    CDuiString sNorm2, sNormErr2;
    if (ApplyNormalizeImage(sWim, sNorm2, &sNormErr2))
    {
        if (sNorm2 != sWim)
        {
            sWim = sNorm2;
            if (pWim) pWim->SetText(sWim.GetData());
            // 重新读取内部映像索引并刷新下拉
            m_apply.vIdx.clear();
            CDuiString sQErr;
            if (m_pHost->HostQueryWim(sWim, m_apply.vIdx, &sQErr) && !m_apply.vIdx.empty()
                && m_apply.pCombo)
            {
                m_apply.pCombo->RemoveAll();
                for (int i2 = 0; i2 < static_cast<int>(m_apply.vIdx.size()); ++i2)
                {
                    CDuiString sT2;
                    sT2.Format(L"%d - %s", m_apply.vIdx[i2].nIndex,
                               m_apply.vIdx[i2].sName.GetData());
                    CListLabelElementUI* pIt2 = std::make_unique<CListLabelElementUI>().release(); // [C++17 R3] framework-owned via pCombo->Add
                    pIt2->SetFixedHeight(28);
                    pIt2->SetText(sT2.GetData());
                    m_apply.pCombo->Add(pIt2);
                }
                m_apply.pCombo->SelectItem(0);
                ApplyIndexChanged();
            }
        }
    }
    else
    {
        ::MessageBoxW(m_hWnd, sNormErr2.GetData(),
                      StringMgr.Get("cleanup.release_img"), MB_OK | MB_ICONWARNING);
        return false;
    }
    return true;
}
// [R14-03] 目标盘系统目录挂载残留检测；命中且用户不确认继续时返回 false。
[[nodiscard]] bool CToolDialogPage::AoWarnIfTargetMounted(const CDuiString& sTgtDr)
{
    CDuiString sMntSearch = sTgtDr;
    sMntSearch += L"\\*";
    WIN32_FIND_DATAW fdMnt;
    win::FindDataHandle hMnt = win::find_handle(::FindFirstFileW(sMntSearch.GetData(), &fdMnt)); // [C++17 R11] RAII FindClose
    if (hMnt)
    {
        CDuiString sMntMsg;
        bool bMntFound = false;
        do
        {
            CDuiString sMntName = fdMnt.cFileName;
            if (sMntName == L"." || sMntName == L"..") continue;
            // 常见的挂载残留标志：Windows、EFI、MSOFT、sources 都是系统目录名
            if (lstrcmpiW(sMntName.GetData(), L"windows") == 0
                || lstrcmpiW(sMntName.GetData(), L"efi") == 0
                || lstrcmpiW(sMntName.GetData(), L"msft") == 0
                || lstrcmpiW(sMntName.GetData(), L"sources") == 0
                || lstrcmpiW(sMntName.GetData(), L"boot") == 0)
            {
                sMntMsg += CDuiString(sMntName)
                    .Format(L"%s\r\n", sMntName.GetData());
                bMntFound = true;
            }
            if (sMntMsg.GetLength() > 800) break;
        } while (::FindNextFileW(hMnt.get(), &fdMnt));
        hMnt.reset(); // [C++17 R11] FindClose at the original call site
        if (bMntFound)
        {
            CDuiString sWarn = CDuiString(StringMgr.Get("misc.hff5b")) + sTgtDr
                + CDuiString(StringMgr.Get("misc.h9f56"))
                + CDuiString(StringMgr.Get("misc.h978b"))
                + sMntMsg
                + CDuiString(StringMgr.Get("misc.h9a73"));
            if (::MessageBoxW(m_hWnd, sWarn.GetData(),
                              StringMgr.Get("misc.hb112"),
                              MB_YESNO | MB_ICONWARNING) != IDYES)
                return false;
        }
    }
    return true;
}
// [R14-03] WIMBoot/WindowsToGo/Compact 时询问额外驱动注入目录（可取消，sDrvPath 留空）。
void CToolDialogPage::AoAskDriverPath(bool bWIMBoot, bool bWTG, bool bCompact,
                                      CDuiString& sDrvPath)
{
    if (!(bWIMBoot || bWTG || bCompact)) return;
    CDuiString sDrvMsg = CDuiString(StringMgr.Get("misc.h96e0"))
        + CDuiString(StringMgr.Get("misc.hf8ca"))
        + CDuiString(StringMgr.Get("misc.h4f95"));
    if (::MessageBoxW(m_hWnd, sDrvMsg.GetData(),
        StringMgr.Get("misc.hefec"),
        MB_YESNO | MB_ICONQUESTION) == IDYES)
    {
        CDuiString sDrvDir;
        if (m_pHost->HostBrowseFolder(sDrvDir, StringMgr.Get("misc.h22ad")))
        {
            DWORD dwAtt = ::GetFileAttributesW(sDrvDir.GetData());
            if (dwAtt != INVALID_FILE_ATTRIBUTES)
            {
                sDrvPath = sDrvDir;   // [P0 修复] 供 Apply 成功后主链原生 AddDriver
            }
        }
    }
}
// [R129-1] D3 证据：释放前把最近一次释放参数写入 HKCU\Software\DismTool\LastApply。
void CToolDialogPage::AoRecordLastApply(const CDuiString& sWim, const CDuiString& sDir,
                                        int nIndex, DWORD dwFlags)
{
    win::RegKey hKey;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\DismTool", 0,
                        KEY_QUERY_VALUE | KEY_SET_VALUE, hKey.out()) == ERROR_SUCCESS)
    {
        // [R128-A] 四项是一组"最近应用"证据：任一写失败则该证据不可信，只留痕
        LONG lA1 = ::RegSetValueExW(hKey.get(), L"LastApplyWim", 0, REG_SZ,
                         (const BYTE*)sWim.GetData(),
                         (DWORD)((sWim.GetLength() + 1) * sizeof(wchar_t)));
        LONG lA2 = ::RegSetValueExW(hKey.get(), L"LastApplyDir", 0, REG_SZ,
                         (const BYTE*)sDir.GetData(),
                         (DWORD)((sDir.GetLength() + 1) * sizeof(wchar_t)));
        LONG lA3 = ::RegSetValueExW(hKey.get(), L"LastApplyIndex", 0, REG_DWORD,
                         (const BYTE*)&nIndex, sizeof(nIndex));
        LONG lA4 = ::RegSetValueExW(hKey.get(), L"LastApplyFlags", 0, REG_DWORD,
                         (const BYTE*)&dwFlags, sizeof(dwFlags));
        if (lA1 != ERROR_SUCCESS || lA2 != ERROR_SUCCESS
            || lA3 != ERROR_SUCCESS || lA4 != ERROR_SUCCESS)
            ::OutputDebugStringW(L"[R128][img] LastApply 证据写入失败\r\n");
        // [C++17 R11] win::RegKey destructor closes the handle at scope exit (same order)
    }
}



// ============================================================
// 2 账户管理
// ============================================================


// 映像英文名 → 中文显示名（复刻原版"显示名称/显示说明"）。
// 做法：逐位置在词典中选"最长命中短语"替换，单词边界、忽略大小写；
//       Windows/年份/N 后缀等未命中部分原样保留，杜绝逐字污染。
static CDuiString LocalizeEditionName(const CDuiString& sEn)
{
    struct EPhrase { const wchar_t* en; const wchar_t* zh; size_t len; };
    EPhrase kP[] = {
        // 长短语在前（实际选择按最长命中，顺序无强制要求）
        { L"Home Single Language",  L"家庭单语言版", 0 },
        { L"Pro for Workstations",  L"专业工作站版", 0 },
        { L"Pro Education",         L"专业教育版",   0 },
        { L"Professional",          L"专业版",       0 },
        { L"Enterprise",            L"企业版",       0 },
        { L"Home China",            L"家庭中文版",   0 },
        { L"Education",             L"教育版",       0 },
        { L"LTSB",                  L"长期服务版",   0 },
        { L"LTSC",                  L"长期服务版",   0 },
        { L"Home",                  L"家庭版",       0 },
        { L"Pro",                   L"专业版",       0 },
    };
    for (int i = 0; i < static_cast<int>(sizeof(kP) / sizeof(kP[0])); ++i)
        kP[i].len = wcslen(kP[i].en);
    auto IsWordCh = [](wchar_t c) {
        return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z')
            || (c >= L'0' && c <= L'9') || c == L'_';
    };
    std::wstring s = sEn.GetData();
    std::wstring out;
    out.reserve(s.size() + 8);
    const size_t n = s.size();
    size_t pos = 0;
    while (pos < n)
    {
        int iBest = -1; size_t cbBest = 0;
        for (int i = 0; i < static_cast<int>(sizeof(kP) / sizeof(kP[0])); ++i)
        {
            size_t L = kP[i].len;
            if (L <= cbBest || pos + L > n) continue;
            // 前/后单词边界，避免 Professional 被 Pro 截断等
            if (pos > 0 && IsWordCh(s[pos - 1])) continue;
            if (pos + L < n && IsWordCh(s[pos + L])) continue;
            bool bEq = true;
            for (size_t j = 0; j < L; ++j)
                if (towlower(s[pos + j]) != towlower(kP[i].en[j])) { bEq = false; break; }
            if (bEq) { iBest = i; cbBest = L; }
        }
        if (iBest >= 0) { out += kP[iBest].zh; pos += cbBest; }
        else { out += s[pos]; ++pos; }
    }
    // 合并可能产生的重复空格
    std::wstring fin;
    fin.reserve(out.size());
    bool bPrevSp = false;
    for (size_t i = 0; i < out.size(); ++i)
    {
        bool bSp = (out[i] == L' ');
        if (bSp && bPrevSp) continue;
        fin += out[i];
        bPrevSp = bSp;
    }
    return CDuiString(fin.c_str());
}

