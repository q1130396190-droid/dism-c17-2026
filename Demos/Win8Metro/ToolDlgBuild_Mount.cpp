#include "stdafx.h"

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
void CToolDialogPage::ClickMount(const CDuiString& sName)
{
    if (sName == L"btn_mount_browse") MountBrowse();
    else if (sName == L"btn_mount_ok") MountOk();
    else if (sName == L"btn_mount_unmount") MountUnmount();
    else if (sName == L"btn_mount_pecfg") MountOpenPeCfg();   // [R-B79]
    else if (sName.Left(8) == L"gridrow_") OnGridRow(sName);
}

void CToolDialogPage::ClickPeCfg(const CDuiString& sName)
{
    if (sName == L"btn_pecfg_browse") PeCfgBrowse();
    else if (sName == L"btn_pecfg_refresh") PeCfgRefresh();
    else if (sName == L"btn_pecfg_apply") PeCfgApply();
}

void CToolDialogPage::BuildMount()
{
    // 顶部说明（基线 Step 1：挂载映像）
    CVerticalLayoutUI* pDesc = std::make_unique<CVerticalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pDesc->SetFixedHeight(28);
    pDesc->SetAttribute(L"inset", L"2,4,2,4");
    CTextUI* pDescTxt = MkText(StringMgr.Get("mount.desc"), 1, 0xFF606060);
    pDescTxt->SetTextPadding(RECT{ 0, 0, 0, 0 });
    pDesc->Add(pDescTxt);
    m_pBody->Add(pDesc);

    CControlUI* pGap0 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pGap0->SetFixedHeight(6);
    m_pBody->Add(pGap0);

    // 源 WIM/ESD 路径（Step 1：挂载映像）
    CTextUI* pLblSrc = MkText(StringMgr.Get("mount.src"), 1, 0xFF333333);
    pLblSrc->SetFixedHeight(24);
    m_pBody->Add(pLblSrc);

    CHorizontalLayoutUI* pSrcRow = std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pSrcRow->SetFixedHeight(38);
    CEditUI* pSrc = MkEdit(L"edt_mount_wim");
    pSrc->SetAttribute(L"tipvalue", StringMgr.Get("mount.hint"));
    pSrc->SetAttribute(L"tipvaluecolor", tokens::TokenHex(tokens::EToken::InkMuted));   // 原 #FF999999
    pSrcRow->Add(pSrc);
    CControlUI* pGW1 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pSrcRow->Add
    pGW1->SetFixedWidth(12);
    pSrcRow->Add(pGW1);
    CButtonUI* pBrowse = MkBtn(StringMgr.Get("general.browse"), L"btn_mount_browse", false);
    pBrowse->SetFixedWidth(104);
    pSrcRow->Add(pBrowse);
    m_pBody->Add(pSrcRow);

    CControlUI* pGap1 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pGap1->SetFixedHeight(8);
    m_pBody->Add(pGap1);

    // 索引表格（Step 2：挂载路径选映像）
    std::vector<TGridCol> cols;
    cols.push_back(TGridCol(StringMgr.Get("mount.idx"), 60));
    cols.push_back(TGridCol(StringMgr.Get("misc.sfdf6f7f6"), 220));
    cols.push_back(TGridCol(StringMgr.Get("misc.sc1835196"), 130));
    cols.push_back(TGridCol(StringMgr.Get("misc.s696f5a97"), -1));
    m_pBody->Add(MkGrid(cols, 12, 32, true));

    CControlUI* pGap2 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pGap2->SetFixedHeight(8);
    m_pBody->Add(pGap2);

    // 挂载路径（Step 2：挂载路径 + Step 3 占位符）
    CTextUI* pLblDir = MkText(StringMgr.Get("mount.mountdir"), 1, 0xFF333333);
    pLblDir->SetFixedHeight(24);
    m_pBody->Add(pLblDir);

    CHorizontalLayoutUI* pDirRow = std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pDirRow->SetFixedHeight(38);
    CEditUI* pDir = MkEdit(L"edt_mount_dir");
    pDir->SetText(L"D:\\1");   // 与原版占位符对齐
    pDirRow->Add(pDir);
    m_pBody->Add(pDirRow);

    // 只读模式（Step 4）
    CControlUI* pGap3 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pGap3->SetFixedHeight(10);
    m_pBody->Add(pGap3);

    CHorizontalLayoutUI* pChkRow = std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pChkRow->SetFixedHeight(30);
    CTdlgCheck* pRO = std::make_unique<CTdlgCheck>().release(); // [C++17 R3] framework-owned via pChkRow->Add
    pRO->Setup(StringMgr.Get("mount.readonly"), true);
    pRO->SetName(L"chk_mount_ro");
    pChkRow->Add(pRO);
    m_pBody->Add(pChkRow);

    // 已挂载列表 + 卸载（Step 7/8/9 记账回显）
    CControlUI* pGap4 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pGap4->SetFixedHeight(10);
    m_pBody->Add(pGap4);

    CHorizontalLayoutUI* pMntRow = std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pMntRow->SetFixedHeight(36);
    CTextUI* pLblMnt = MkText(StringMgr.Get("mount.mounted"), 1, 0xFF333333);
    pLblMnt->SetFixedWidth(140);
    pLblMnt->SetMouseEnabled(false);
    pMntRow->Add(pLblMnt);
    CComboUI* pMnt = MkCombo(L"cmb_mount_mounted", 200);
    m_mount.pDirCombo = pMnt;
    pMntRow->Add(pMnt);
    CControlUI* pGW3 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pMntRow->Add
    pGW3->SetFixedWidth(10);
    pMntRow->Add(pGW3);
    CButtonUI* pUnmount = MkBtn(StringMgr.Get("mount.unmount"), L"btn_mount_unmount", false);
    pUnmount->SetFixedWidth(96);
    pMntRow->Add(pUnmount);
    CControlUI* pGW4 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pMntRow->Add   // [R-B79] PE 映像设置入口（ScratchSpace/TargetPath）
    pGW4->SetFixedWidth(10);
    pMntRow->Add(pGW4);
    CButtonUI* pPeCfg = MkBtn(L"PE 设置…", L"btn_mount_pecfg", false);
    pPeCfg->SetFixedWidth(110);
    pMntRow->Add(pPeCfg);
    m_pBody->Add(pMntRow);

    // 底部：挂载映像 / 取消
    CControlUI* pGapL = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pGapL->SetMinHeight(14);
    m_pBody->Add(pGapL);

    AddBottomRow(MkBtn(StringMgr.Get("mount.btn_mount"), L"btn_mount_ok", true),
                 MkBtn(StringMgr.Get("general.cancel"), L"btn_cancel", false));

    // 已挂载下拉填充（MountLedger 枚举）
    RefreshMountedList();
}

void CToolDialogPage::MountBrowse()
{
    CDuiString sFile;
    if (!m_pHost->HostOpenFile(
            sFile,
            L"WIM/ESD 映像 (*.wim;*.esd)\0*.wim;*.esd\0所有文件 (*.*)\0*.*\0",
            StringMgr.Get("mount.src"))) return;

    CEditUI* pE = static_cast<CEditUI*>(m_pm.FindControl(L"edt_mount_wim"));
    if (pE) pE->SetText(sFile.GetData());

    m_mount.sFile = sFile;
    MountReload(true);
}

void CToolDialogPage::MountReload(bool bAlert)
{
    // [R127-A] 读取与提示提取为 tdlg::ReloadWimIndex
    //   （原与 EsdWizReload 逐字重复，仅成员名与文案键不同）
    if (!tdlg::ReloadWimIndex(m_pHost, m_hWnd, bAlert, m_mount.sFile, m_mount.vIdx,
                              StringMgr.Get("mount.nosrc"),
                              StringMgr.Get("mount.noidx"),
                              StringMgr.Get("mount.title"))) return;
    MountFillRows();
}

void CToolDialogPage::MountFillRows()
{
    // [R127-A] 逐格填充提取为 tdlg::FillWimIndexRows
    //   （原与 EsdWizFillRows 逐字重复，唯一差异是索引数组成员名）
    tdlg::FillWimIndexRows(m_vRows, m_mount.vIdx);
    SetGridSelection(0);
}

void CToolDialogPage::MountOk()
{
    if (m_mount.sFile.IsEmpty())
    {
        ::MessageBoxW(m_hWnd, StringMgr.Get("mount.nosrc"),
                      StringMgr.Get("mount.title"), MB_OK | MB_ICONWARNING);
        return;
    }

    if (m_nSel < 0 || m_nSel >= static_cast<int>(m_mount.vIdx.size()))
    {
        ::MessageBoxW(m_hWnd, StringMgr.Get("mount.noidx"),
                      StringMgr.Get("mount.title"), MB_OK | MB_ICONWARNING);
        return;
    }

    CEditUI* pDir = static_cast<CEditUI*>(m_pm.FindControl(L"edt_mount_dir"));
    CDuiString sDir = pDir ? pDir->GetText() : CDuiString(L"");
    sDir.Trim();
    if (sDir.IsEmpty())
    {
        ::MessageBoxW(m_hWnd, StringMgr.Get("mount.nodir"),
                      StringMgr.Get("mount.title"), MB_OK | MB_ICONWARNING);
        return;
    }

    CTdlgCheck* pRO = static_cast<CTdlgCheck*>(m_pm.FindControl(L"chk_mount_ro"));
    bool bReadOnly = (pRO && pRO->Checked());

    int nIndex = m_mount.vIdx[m_nSel].nIndex;

    // [FIX-T2-5] wimgapi 同步挂载（秒~十秒级）移后台线程：原在 UI 线程
    // 直接执行且无忙碌提示，整窗未响应。遮罩挡重入，完成后
    // WM_TDLG_TASKDONE(wParam=3 成功 / 4 失败) 收口解遮罩并刷新列表。
    if (m_bMountBusy) return;
    m_bMountBusy = true;
    BeginTaskShield(L"正在挂载映像（可能需要数十秒），请稍候……", true);
    struct MountCtx
    {
        CToolDialogPage* pDlg;
        CDuiString       sFile;
        int              nIndex;
        CDuiString       sDir;
        bool             bReadOnly;
    };
    MountCtx* pMCtx = new MountCtx{ this, m_mount.sFile, nIndex, sDir, bReadOnly };
    HANDLE hMk = BackgroundJobs::Start([](void* pv) -> DWORD {
        std::unique_ptr<MountCtx> guard(static_cast<MountCtx*>(pv));
        CToolDialogPage* pDlg = guard->pDlg;
        // [FIX-S0-1 同型] 持 shared_ptr 副本：对话框可能在线程期间被关闭
        auto spSvc = pDlg->ImgSvcForJob();
        bool bOk = spSvc && spSvc->MountImage(guard->sFile.GetData(),
                                              static_cast<DWORD>(guard->nIndex),
                                              guard->sDir.GetData(),
                                              guard->bReadOnly);
        if (!bOk)
        {
            // 失败：统计异常挂载点（下沉 IWizardService；基线"跳过 %d 个异常挂载点"）
            auto spWiz = pDlg->WizSvcForJob();
            int nSkip = spWiz ? spWiz->MountCountStale() : 0;
            if (nSkip > 0)
            {
                CDuiString sSkip;
                sSkip.Format(StringMgr.Get("mount.s_skip"), nSkip);
                pDlg->JobHostLog(sSkip);
            }
            CDuiString sErr;
            sErr.Format(L"[mount] bad mount: %s -> %s (skip=%d)",
                        guard->sFile.GetData(), guard->sDir.GetData(), nSkip);
            pDlg->JobHostLog(sErr);
        }
        else
        {
            CDuiString sLog;
            sLog.Format(StringMgr.Get("mount.s_ok"),
                        guard->sFile.GetData(), guard->sDir.GetData());
            pDlg->JobHostLog(sLog);
        }
        ::PostMessageW(pDlg->JobHwnd(), WM_TDLG_TASKDONE, bOk ? 3 : 4, 0);
        return 0;
    }, pMCtx, "wim-mount");
    if (!hMk)
    {
        m_bMountBusy = false;
        EndTaskShield();
        delete pMCtx;
        ::MessageBoxW(m_hWnd, L"后台挂载线程启动失败。", StringMgr.Get("mount.title"),
                      MB_OK | MB_ICONWARNING);
    }
}

void CToolDialogPage::MountUnmount()
{
    if (!m_mount.pDirCombo) return;
    int nSel = m_mount.pDirCombo->GetCurSel();
    if (nSel < 0 || nSel >= static_cast<int>(m_mount.vRows.size()))
    {
        ::MessageBoxW(m_hWnd, StringMgr.Get("mount.nomount"),
                      StringMgr.Get("mount.title"), MB_OK | MB_ICONWARNING);
        return;
    }

    // [R6-05c] 直接按"下拉选中的那一行"卸载（行含 wim/idx/dir）。
    // 修正基线：下拉按 wimgapi 真实枚举序填充、卸载却按 MountLedger
    // 记账序下标取值，两序不一致时会卸载错目标。
    // [FIX-T2-5] 卸载（wimgapi 同步）同样移后台线程，WM_TDLG_TASKDONE
    // (wParam=5) 收口解遮罩并刷新列表。
    if (m_bMountBusy) return;
    const toolsvc::TMountedRow r = m_mount.vRows[nSel];
    m_bMountBusy = true;
    BeginTaskShield(L"正在卸载映像，请稍候……", true);
    struct UnmCtx
    {
        CToolDialogPage* pDlg;
        std::wstring     sWim;
        DWORD            dwIdx;
        std::wstring     sDir;
    };
    UnmCtx* pUCtx = new UnmCtx{ this, r.sWim, r.dwIdx, r.sDir };
    HANDLE hUm = BackgroundJobs::Start([](void* pv) -> DWORD {
        std::unique_ptr<UnmCtx> guard(static_cast<UnmCtx*>(pv));
        CToolDialogPage* pDlg = guard->pDlg;
        auto spSvc = pDlg->ImgSvcForJob();
        bool bOk = spSvc && spSvc->UnmountImage(guard->sWim, guard->dwIdx,
                                                guard->sDir, false);
        CDuiString sLog;
        sLog.Format(StringMgr.Get("mount.s_unmounted"), guard->sDir.c_str());
        pDlg->JobHostLog(sLog);
        ::PostMessageW(pDlg->JobHwnd(), WM_TDLG_TASKDONE, bOk ? 5 : 4, 0);
        return 0;
    }, pUCtx, "wim-unmount");
    if (!hUm)
    {
        m_bMountBusy = false;
        EndTaskShield();
        delete pUCtx;
    }
}

void CToolDialogPage::RefreshMountedList()
{
    if (!m_mount.pDirCombo) return;
    m_mount.pDirCombo->RemoveAll();
    m_mount.vMountedDirs.clear();

    // [R6-05c] 双源合并/索引比对/失效判定下沉 IWizardService；
    // 这里仅按行渲染（文案/样式与基线逐字一致）。
    m_pWizSvc->MountEnumRows(m_mount.vRows);
    for (size_t i = 0; i < m_mount.vRows.size(); ++i)
    {
        const toolsvc::TMountedRow& r = m_mount.vRows[i];
        std::wstring sLabel = r.sDir + L"  <-  " + r.sWim;
        if (r.bReal)
        {
            // 显示映像索引（与 MountLedger 记账比对后的真实值）
            std::wstring sIdx(32, L'\0');
            wfmt::FormatInto(&sIdx[0], sIdx.size(), L"  [索引 %lu]",
                         (unsigned long)r.dwIdx);
            sIdx.resize(wcslen(sIdx.c_str()));
            sLabel += sIdx;
            if (r.bRW)    sLabel += L"  [可写]";
            if (r.bStale) sLabel += L"  [已失效]";
        }
        CListLabelElementUI* pItem = std::make_unique<CListLabelElementUI>().release(); // [C++17 R3] framework-owned via m_mount.pDirCombo->Add
        pItem->SetFixedHeight(28);
        pItem->SetText(sLabel.c_str());
        m_mount.pDirCombo->Add(pItem);
        m_mount.vMountedDirs.push_back(r.sDir);
    }
}

// ============================================================
// [R-B79] PE 映像设置（EToolDlgKind::TD_PECFG）
// ------------------------------------------------------------
// 知识库 06_镜像处理专题 §4.1：PEImageManager.xml 仅有两个配置项——
//   设置 Windows PE 映像的暂存空间(MB)  → 原 dism /Set-ScratchSpace
//   设置 Windows PE 映像的目标路径       → 原 dism /Set-TargetPath
// Main.xml 布局冻结，本能力走挂载向导"PE 设置…"动态弹窗，不新增 XML。
//
// [原生升级 2026-10-11] 不再调用 dism.exe，直接 RegLoadKey 离线蜂巢读写。
// 落点经原版 Dism++x64.exe（10.1.1002.1B）字符串池字节级核实：
//   暂存空间 = SYSTEM 蜂巢 <ControlSet>\services\fbwf!WinPECacheThreshold (REG_DWORD, MB)
//   目标路径 = SOFTWARE 蜂巢 Microsoft\Windows NT\CurrentVersion\WinPE!InstRoot (REG_SZ)
//   <ControlSet> 由 SYSTEM\Select!Current 解析（ControlSet00N）。
// ============================================================
// [R6-05d] PeCfg 蜂巢辅助（EnablePriv/LoadHive/UnloadHive/Join/CurrentSet）已下沉 ToolService/WizardService.cpp。

void CToolDialogPage::MountOpenPeCfg()
{
    CDuiString sDir;
    if (CEditUI* pDir = static_cast<CEditUI*>(m_pm.FindControl(L"edt_mount_dir")))
        sDir = pDir->GetText();
    sDir.Trim();
    if (sDir.IsEmpty() && m_mount.pDirCombo)
    {
        int nSel = m_mount.pDirCombo->GetCurSel();
        if (nSel >= 0 && nSel < static_cast<int>(m_mount.vMountedDirs.size()))
            sDir = m_mount.vMountedDirs[nSel].c_str();
    }

    CToolDialogPage dlg(m_pHost, EToolDlgKind::TD_PECFG);
    dlg.SetDialogDPI(m_nDPI);
    if (!sDir.IsEmpty())
    {
        // [R16][T4] 预填经统一 Open(ToolOpenArgs) 通路注入。
        toolsvc::ToolOpenArgs args;
        args.sInitialPeDir = std::wstring(sDir.GetData());
        args.bHasInitialPeDir = true;
        dlg.Open(args);
    }
    dlg.Create(m_hWnd, nullptr, UI_WNDSTYLE_DIALOG, 0);
    dlg.ShowModal();
}

void CToolDialogPage::BuildPeCfg()
{
    m_pe.pDir = m_pe.pScratch = m_pe.pTarget = nullptr;

    CTextUI* pDesc = MkText(
        L"对已挂载的 Windows PE 映像设置暂存空间与目标路径（dism /Set-ScratchSpace、/Set-TargetPath）。",
        1, 0xFF606060);
    pDesc->SetFixedHeight(28);
    m_pBody->Add(pDesc);

    CControlUI* pGap0 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pGap0->SetFixedHeight(8);
    m_pBody->Add(pGap0);

    CTextUI* pLblDir = MkText(L"挂载目录", 1, 0xFF333333);
    pLblDir->SetFixedHeight(24);
    m_pBody->Add(pLblDir);

    CHorizontalLayoutUI* pDirRow = std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pDirRow->SetFixedHeight(38);
    CEditUI* pDir = MkEdit(L"edt_pecfg_dir");
    pDir->SetAttribute(L"tipvalue", L"例如 D:\\Mount（已挂载的 PE 映像目录）");
    pDir->SetAttribute(L"tipvaluecolor", tokens::TokenHex(tokens::EToken::InkMuted));   // 原 #FF999999
    pDirRow->Add(pDir);
    m_pe.pDir = pDir;
    CControlUI* pGW1 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pDirRow->Add
    pGW1->SetFixedWidth(12);
    pDirRow->Add(pGW1);
    CButtonUI* pBrowse = MkBtn(L"浏览…", L"btn_pecfg_browse", false);
    pBrowse->SetFixedWidth(104);
    pDirRow->Add(pBrowse);
    m_pBody->Add(pDirRow);
    // [R16][T4] 挂载目录预填由 IIsoImageService 持有（经 Open 注入）。
    const std::wstring& sPeInit = m_pImgSvc->InitialPeDir();
    if (!sPeInit.empty()) pDir->SetText(CDuiString(sPeInit.c_str()));

    CControlUI* pGap1 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pGap1->SetFixedHeight(10);
    m_pBody->Add(pGap1);

    CTextUI* pLblSc = MkText(L"设置 Windows PE 映像的暂存空间(MB)（仅允许 128 / 256 / 512）",
                             1, 0xFF333333);
    pLblSc->SetFixedHeight(24);
    m_pBody->Add(pLblSc);
    CEditUI* pSc = MkEdit(L"edt_pecfg_scratch");
    pSc->SetAttribute(L"numberonly", L"true");
    pSc->SetFixedHeight(36);
    m_pBody->Add(pSc);
    m_pe.pScratch = pSc;

    CControlUI* pGap2 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pGap2->SetFixedHeight(10);
    m_pBody->Add(pGap2);

    CTextUI* pLblTp = MkText(L"设置 Windows PE 映像的目标路径（盘符根，如 X:\\）",
                             1, 0xFF333333);
    pLblTp->SetFixedHeight(24);
    m_pBody->Add(pLblTp);
    CEditUI* pTp = MkEdit(L"edt_pecfg_target");
    pTp->SetFixedHeight(36);
    m_pBody->Add(pTp);
    m_pe.pTarget = pTp;

    CControlUI* pGapF = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pGapF->SetMinHeight(10);
    m_pBody->Add(pGapF);

    // 底部：刷新 / 应用（左），关闭（右）
    CHorizontalLayoutUI* pBot = std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pBot->SetFixedHeight(74);
    pBot->SetAttribute(L"inset", L"0,18,0,18");
    CButtonUI* pRefresh = MkBtn(L"刷新", L"btn_pecfg_refresh", false);
    pRefresh->SetFixedWidth(110);
    pBot->Add(pRefresh);
    CControlUI* pGW2 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pBot->Add
    pGW2->SetFixedWidth(14);
    pBot->Add(pGW2);
    CButtonUI* pApply = MkBtn(L"应用", L"btn_pecfg_apply", true);
    pApply->SetFixedWidth(110);
    pBot->Add(pApply);
    CControlUI* pSp = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pBot->Add
    pBot->Add(pSp);
    CButtonUI* pClose = MkBtn(L"关闭", L"btn_cancel", false);
    pClose->SetFixedWidth(110);
    pBot->Add(pClose);
    m_pBody->Add(pBot);
}

void CToolDialogPage::PeCfgBrowse()
{
    CDuiString sDir;
    if (!m_pHost->HostBrowseFolder(sDir, L"选择已挂载的 PE 映像目录")) return;
    if (m_pe.pDir) m_pe.pDir->SetText(sDir.GetData());
}

void CToolDialogPage::PeCfgRefresh()
{
    if (!m_pe.pDir) return;
    CDuiString sDir = m_pe.pDir->GetText();
    sDir.Trim();
    if (sDir.IsEmpty() ||
        ::GetFileAttributesW(sDir.GetData()) == INVALID_FILE_ATTRIBUTES)
    {
        ::MessageBoxW(m_hWnd, L"请先填写有效的挂载目录。",
                      L"PE 映像设置", MB_OK | MB_ICONWARNING);
        return;
    }

    // [R6-05d] 提权/蜂巢加载/两项读取/卸载下沉 IWizardService
    std::wstring sScratchW, sTargetW;
    if (!m_pWizSvc->PeCfgReadHive(sDir.GetData(), sScratchW, sTargetW))
    {
        ::MessageBoxW(m_hWnd,
            L"无法加载该映像的离线注册表蜂巢（SYSTEM/SOFTWARE）。\n"
            L"请确认目录是已挂载、可写的 Windows PE 映像根目录。",
            L"PE 映像设置", MB_OK | MB_ICONWARNING);
        return;
    }

    CDuiString sScratch = sScratchW.c_str();
    CDuiString sTarget  = sTargetW.c_str();

    if (m_pe.pScratch) m_pe.pScratch->SetText(sScratch.GetData());
    if (m_pe.pTarget) m_pe.pTarget->SetText(sTarget.GetData());

    CDuiString sLog;
    sLog.Format(L"[pecfg] 刷新完成：%s（ScratchSpace=%s TargetPath=%s，原生蜂巢读取）",
                sDir.GetData(),
                sScratch.IsEmpty() ? L"<未设置>" : sScratch.GetData(),
                sTarget.IsEmpty() ? L"<未设置>" : sTarget.GetData());
    m_pHost->HostLog(sLog);
}

void CToolDialogPage::PeCfgApply()
{
    if (!m_pe.pDir) return;
    CDuiString sDir = m_pe.pDir->GetText();
    sDir.Trim();
    CDuiString sSc = m_pe.pScratch ? m_pe.pScratch->GetText() : CDuiString(L"");
    CDuiString sTp = m_pe.pTarget ? m_pe.pTarget->GetText() : CDuiString(L"");
    sSc.Trim();
    sTp.Trim();

    if (sDir.IsEmpty() ||
        ::GetFileAttributesW(sDir.GetData()) == INVALID_FILE_ATTRIBUTES)
    {
        ::MessageBoxW(m_hWnd, L"请先填写有效的挂载目录。",
                      L"PE 映像设置", MB_OK | MB_ICONWARNING);
        return;
    }
    if (sSc.IsEmpty() && sTp.IsEmpty())
    {
        ::MessageBoxW(m_hWnd, L"两项设置均为空，没有需要应用的内容。",
                      L"PE 映像设置", MB_OK | MB_ICONWARNING);
        return;
    }

    if (!sSc.IsEmpty())
    {
        int nSc = _ttoi(sSc.GetData());
        if (nSc != 128 && nSc != 256 && nSc != 512)
        {
            ::MessageBoxW(m_hWnd, L"暂存空间仅允许 128、256 或 512（MB）。",
                          L"PE 映像设置", MB_OK | MB_ICONWARNING);
            return;
        }
    }
    if (!sTp.IsEmpty())
    {
        // 形如 X:\（盘符根）
        bool bValid = sTp.GetLength() == 3
                      && iswalpha((wint_t)sTp[0])
                      && sTp[1] == L':'
                      && (sTp[2] == L'\\' || sTp[2] == L'/');
        if (!bValid)
        {
            ::MessageBoxW(m_hWnd, L"目标路径必须是盘符根目录，格式如 X:\\ 。",
                          L"PE 映像设置", MB_OK | MB_ICONWARNING);
            return;
        }
        std::wstring sTpAdj = sTp.GetData() ? sTp.GetData() : L"";
        if (sTpAdj.size() > 2) sTpAdj.resize(2);
        if (!sTpAdj.empty()) sTpAdj[0] = static_cast<wchar_t>(::towupper(sTpAdj[0]));
        sTpAdj += L"\\";
        sTp = sTpAdj.c_str();
    }

    // [R6-05d] 提权/蜂巢加载/两项写入/卸载全部下沉 IWizardService；
    // 过程日志经宿主桥输出，本页仅按结果弹框（0=static_cast<int>(ELogLevel::LOG_INFO) 2=static_cast<int>(ELogLevel::LOG_ERROR) 在服务侧）。
    int nFail = 0;
    if (!m_pWizSvc->PeCfgWriteHive(sDir.GetData(),
                                   sSc.IsEmpty() ? 0 : _ttoi(sSc.GetData()),
                                   sTp.GetData(), nFail))
    {
        ::MessageBoxW(m_hWnd,
            L"无法加载该映像的离线注册表蜂巢（SYSTEM/SOFTWARE）。\n"
            L"请确认目录是已挂载、可写的 Windows PE 映像根目录。",
            L"PE 映像设置", MB_OK | MB_ICONWARNING);
        return;
    }

    if (nFail == 0)
        ::MessageBoxW(m_hWnd, L"PE 映像设置已写入离线注册表蜂巢。",
                      L"PE 映像设置", MB_OK | MB_ICONINFORMATION);
    else
        ::MessageBoxW(m_hWnd, L"部分设置写入失败，请确认该映像是可写挂载的 Windows PE，详情见日志。",
                      L"PE 映像设置", MB_OK | MB_ICONWARNING);
}

// [R6-05b] BootFix 卷/分区枚举、GUID 常量、推荐打分（原 8215-8420
// 匿名命名空间 Bf* 系列）已下沉 ToolService/WizardService.cpp；
// EnumerateBfParts 仅做 wstring→CDuiString 呈现转换。
// [C++17 R3] 文件级内联常量（真实 C++17 inline variables）：
// 本页处理批次的超时/重试上限，供本文件内部使用；inline 保证多 TU 下 ODR 一致。
inline constexpr int kMntStepTimeoutMs = 120000;
inline constexpr int kMntMaxRetry = 3;
