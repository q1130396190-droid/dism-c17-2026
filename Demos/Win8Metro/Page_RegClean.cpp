#include "stdafx.h"
#include "ThemeTokens.h"   // [R203] 颜色令牌（深色/浅色跟随）


// ============================================================
// Page_RegClean.cpp —— 注册表清理页（[R79] 四件套接线版）
//
// 承接原 CFrameWnd::BuildRegCleanPage：
//   Data.xml 中 Reg 类动作规则枚举（原版独立页还原）
//
// [R79] 接线：
//   BuildPage()   从 m_ctx.RegCleanEngine() 拉取真实规则，渲染复选框行
//   OnEvent()     响应 btn_regclean_scan / clean / backup / restore / selectall / selectnone
//   OnScan        调 ScanRules，Toast + 状态刷新
//   OnClean       先 HostConfirmWarn 确认 → BackupRules → CleanRules → 重建
//   OnBackup      单独备份
//   OnRestore     调 RestoreBackup 恢复上次备份
// ============================================================

#include "Page_RegClean.h"
#include "CustomControls.h"
#include "StringManager.h"
#include "DataEngine.h"
#include "RegCleanEngine.h"
#include "BackgroundJobs.h"   // [FIX-T2-4] 还原点/.reg 还原后台化
#include "WmMessages.h"            // [R13 P0收敛] 仅需 WM_* 宏；UI 专属常量零使用
#include "FrameWnd_UiHelpers.h"   // [R103] UiHelpers_ShowUnifiedListContextMenu / EListCtx::LCX_REGCLEAN
#include "FrameWnd_Util.h"        // [R103] FrameWnd_Util_CopyTextToClipboard
// [C++17 阶段2] 局部 RAII 守卫：FindFirstFile 句柄（见 WinHandles.h 使用规范）
struct PRCFindGuard
{
    HANDLE h;
    explicit PRCFindGuard(HANDLE x) : h(x) {}
    ~PRCFindGuard() { if (h && h != INVALID_HANDLE_VALUE) ::FindClose(h); }
    PRCFindGuard(const PRCFindGuard&) = delete;
    PRCFindGuard& operator=(const PRCFindGuard&) = delete;
    operator HANDLE() const { return h; }
    [[nodiscard]] bool valid() const { return h && h != INVALID_HANDLE_VALUE; }
};


using namespace DuiLib;
using i18n::CStringManager;

namespace
{
    // 行首复选框，宽 22 高 22，选中态由 SetCheck 驱动
    [[nodiscard]] CCheckBoxUI* MakeRegCheck(int nIdx, bool bChecked)
    {
        auto pCheck = std::make_unique<CBoxCheckUI>(); // [C++17 R3] [BOX-CHECK] 自绘方框
        CDuiString sName;
        sName.Format(L"regcheck_%d", nIdx);
        pCheck->SetName(sName.GetData());
        pCheck->SetFixedWidth(22);
        pCheck->SetFixedHeight(22);
        pCheck->SetAttribute(L"valign", L"vcenter");
        pCheck->SetAttribute(L"textpadding", L"0,0,0,0");
        pCheck->SetCheck(bChecked);
        return pCheck.release(); // [C++17 R3] ownership transferred to caller
    }
}

void CPageRegClean::BuildPage()
{
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"regclean_list"));
    if (!pList) return;
    pList->RemoveAll();

    m_vRegCleanItems.clear();
    m_nSelected = -1;

    CRegCleanEngine& eng = m_ctx.RegCleanEngine();
    int nCount = 0;

    // 未加载时先加载一次（幂等）
    if (eng.GetRuleCount() == 0)
        eng.LoadRules(&m_ctx.DataEngine());

    int nRules = eng.GetRuleCount();
    for (int i = 0; i < nRules; ++i)
    {
        RegCleanRule& rule = eng.GetRule(i);

        auto pRow = std::make_unique<CClickRowUI>(); // [C++17 R3]
        pRow->SetFixedHeight(30);
        pRow->SetBkColor(tokens::Token(tokens::EToken::Surface));
        RECT rcIn = { 10, 0, 10, 0 };
        pRow->SetInset(rcIn);

        CDuiString sRowName;
        sRowName.Format(L"regcleanrow_%d", nCount);
        pRow->SetName(sRowName.GetData());

        CCheckBoxUI* pCheck = MakeRegCheck(nCount, rule.bSelected);
        pCheck->SetMouseEnabled(true);
        pRow->Add(pCheck);

        auto pTx = std::make_unique<CTextUI>(); // [C++17 R3]
        pTx->SetText(DismResolveText(rule.sName.GetData()).GetData());
        pTx->SetTextColor(tokens::Token(tokens::EToken::Ink));
        pTx->SetAttribute(L"valign", L"vcenter");
        pTx->SetAttribute(L"endellipsis", L"true");
        pTx->SetMouseEnabled(false);
        pRow->Add(pTx.release());  // [C++17 R3] ownership to DuiLib

        // 状态列（扫描后显示 可清理/无需清理）
        auto pSt = std::make_unique<CTextUI>(); // [C++17 R3]
        pSt->SetFixedWidth(70);
        pSt->SetAttribute(L"align", L"center");
        pSt->SetAttribute(L"valign", L"vcenter");
        pSt->SetTextColor(tokens::Token(tokens::EToken::InkMuted));
        if (rule.nScanState == 1)
            pSt->SetText(StringMgr.Get("regclean.s_cleanable"));
        else if (rule.nScanState == 0)
            pSt->SetText(StringMgr.Get("regclean.s_none"));
        else
            pSt->SetText(L"--");
        pSt->SetMouseEnabled(false);
        pRow->Add(pSt.release());  // [C++17 R3] ownership to DuiLib

        pList->Add(pRow.release());  // [C++17 R3] ownership to DuiLib
        m_vRegCleanItems.push_back(i);
        ++nCount;
    }

    // 更新计数
    CTextUI* pCount = static_cast<CTextUI*>(m_ctx.HostFindControl(L"regclean_count"));
    if (pCount)
    {
        CDuiString sTxt;
        sTxt.Format(StringMgr.Get("regclean.s_count"), nCount);
        pCount->SetText(sTxt.GetData());
    }

    // [R229] Empty state. Before the first scan this list has no rows at all, and the
    //   page showed a blank white area -- indistinguishable from "still loading" or
    //   "broken". Add a centred hint so the user knows what to do next.
    if (nCount == 0 && pList->GetCount() == 0)
    {
        auto pHint = std::make_unique<CTextUI>(); // [C++17 R3]
        pHint->SetText(StringMgr.Get("regclean.s_empty_hint"));
        pHint->SetFixedHeight(170);
        pHint->SetTextColor(tokens::Token(tokens::EToken::InkTertiary));
        pHint->SetAttribute(L"align", L"center");
        pHint->SetAttribute(L"valign", L"vcenter");
        pHint->SetMouseEnabled(false);
        pList->Add(pHint.release()); // [C++17 R3] ownership transferred to DuiLib
    }

    CDuiString sLog;
    sLog.Format(L"[regclean] rules loaded: %d", nCount);
    m_ctx.HostLog(sLog.GetData());
}

int CPageRegClean::CurrentSelectedIndex() const
{
    if (m_nSelected < 0) return -1;
    if (static_cast<size_t>(m_nSelected) >= m_vRegCleanItems.size()) return -1;
    return m_nSelected;
}
// ---- [R-B1] Ctrl+i / Ctrl+X 复制当前选中规则名 ----
void CPageRegClean::OnGlobalCopy(bool /*bCut*/)
{
    int k = CurrentSelectedIndex();
    if (k < 0)
    {
        m_ctx.HostToast(StringMgr.Get("regclean.s_nocopy"),
                        StringMgr.Get("regclean.s_nocopy_body"), 1);
        return;
    }
    const RegCleanRule& r = m_ctx.RegCleanEngine().GetRule(k);
    if (r.sName.IsEmpty()) return;
    FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), r.sName.GetData());
    m_ctx.HostToast(StringMgr.Get("regclean.s_copydone"),
                    r.sName.GetData(), 0);
    CDuiString sLog;
    sLog.Format(L"[regclean] copy rule name: %s", r.sName.GetData());
    m_ctx.HostLog(sLog.GetData());
}

// ---- 扫描 ----
void CPageRegClean::SetRegCleanButtonsBusy(bool bBusy)
{
    // [R-EXTASYNC] 扫描/清理后台运行期间禁用所有动作按钮，防重入并发
    static const wchar_t* s_ids[] = {
        L"btn_regclean_scan", L"btn_regclean_clean",
        L"btn_regclean_backup", L"btn_regclean_restore",
        L"btn_regclean_restorepoint",
        L"btn_regclean_selectall", L"btn_regclean_selectnone"
    };
    for (size_t i = 0; i < _countof(s_ids); ++i)
    {
        CControlUI* pBtn = m_ctx.HostFindControl(s_ids[i]);
        if (pBtn) pBtn->SetEnabled(!bBusy);
    }
}

void CPageRegClean::OnScan()
{
    CRegCleanEngine& eng = m_ctx.RegCleanEngine();
    if (eng.GetRuleCount() == 0)
    {
        int n = eng.LoadRules(&m_ctx.DataEngine());
        if (n == 0)
        {
            m_ctx.HostToast(StringMgr.Get("regclean.s_norules"),
                            StringMgr.Get("misc.s2e69cd5a"), 1);
            return;
        }
    }

    // [R-EXTASYNC] 全量注册表枚举移到工作线程，避免 UI 未响应
    if (RegCleanJobService::IsRunning())
    {
        m_ctx.HostLog(L"[regclean] job already running, scan ignored", 2);
        return;
    }

    m_vScanResults.clear();
    m_scanSummary = RegCleanScanSummary();

    if (!RegCleanJobService::LaunchScan(m_ctx.HostHwnd(), &eng,
                                        &m_vScanResults, &m_scanSummary))
    {
        m_ctx.HostToast(StringMgr.Get("regclean.s_scantitle"),
                        L"启动扫描失败", 1);
        return;
    }
    SetRegCleanButtonsBusy(true);
    m_ctx.HostLog(L"[regclean] scan started (background)...", 0);
}

void CPageRegClean::OnScanDone(RegCleanScanDoneMsg* pMsg)
{
    RegCleanJobService::EndScan();
    RegCleanJobService::DetachThread();
    SetRegCleanButtonsBusy(false);

    if (!pMsg || !pMsg->bOk)
    {
        m_ctx.HostToast(StringMgr.Get("regclean.s_scantitle"),
                        L"扫描异常终止", 1);
        return;
    }

    CRegCleanEngine& eng = m_ctx.RegCleanEngine();

    // 同步勾选态到规则对象
    for (size_t i = 0; i < m_vScanResults.size() && i < static_cast<size_t>(eng.GetRuleCount()); ++i)
    {
        RegCleanRule& rule = eng.GetRule(static_cast<int>(i));
        if (rule.nScanState == 1) rule.bSelected = true;
    }

    CDuiString sToast;
    sToast.Format(StringMgr.Get("regclean.s_scandone"),
                  pMsg->nCleanable, pMsg->nCleanActions);
    m_ctx.HostToast(StringMgr.Get("regclean.s_scantitle"), sToast.GetData(), 0);

    BuildPage();
}

// ---- 备份 ----
void CPageRegClean::OnBackup()
{
    CRegCleanEngine& eng = m_ctx.RegCleanEngine();
    if (eng.GetRuleCount() == 0) { OnScan(); return; }

    // 收集勾选规则
    std::vector<int> vSel;
    for (int i = 0; i < eng.GetRuleCount(); ++i)
        if (eng.GetRule(i).bSelected) vSel.push_back(i);

    if (vSel.empty())
    {
        m_ctx.HostToast(StringMgr.Get("regclean.s_noselected"),
                        StringMgr.Get("misc.s2e69cd5a"), 1);
        return;
    }

    RegCleanBackup backup;
    if (eng.BackupRules(vSel, backup))
    {
        CDuiString sLog;
        sLog.Format(L"[regclean] backup created: %s (%d entries)",
                    backup.sFile.GetData(), static_cast<int>(backup.entries.size()));
        m_ctx.HostLog(sLog.GetData());

        CDuiString sToast;
        sToast.Format(StringMgr.Get("regclean.s_backupdone"), backup.sFile.GetData());
        m_ctx.HostToast(StringMgr.Get("regclean.s_backuptitle"), sToast.GetData(), 0);
    }
    else
    {
        m_ctx.HostToast(StringMgr.Get("regclean.s_backupfail"),
                        StringMgr.Get("misc.s2e69cd5a"), 1);
    }
}

// ---- 清理 ----
void CPageRegClean::OnClean()
{
    CRegCleanEngine& eng = m_ctx.RegCleanEngine();
    if (eng.GetRuleCount() == 0) { OnScan(); return; }

    std::vector<int> vSel;
    int nCleanable = 0;
    for (int i = 0; i < eng.GetRuleCount(); ++i)
    {
        RegCleanRule& rule = eng.GetRule(i);
        if (!rule.bSelected) continue;
        if (rule.nScanState != 1) continue;
        vSel.push_back(i);
        ++nCleanable;
    }

    if (vSel.empty())
    {
        m_ctx.HostToast(StringMgr.Get("regclean.s_noselected"),
                        StringMgr.Get("misc.s2e69cd5a"), 1);
        return;
    }

    CDuiString sConfirm;
    sConfirm.Format(StringMgr.Get("regclean.s_confirm"), nCleanable);
    if (!m_ctx.HostConfirmWarn(StringMgr.Get("regclean.s_cleantitle"), sConfirm.GetData()))
        return;

    // 先备份再清理
    RegCleanBackup backup;
    if (!eng.BackupRules(vSel, backup))
    {
        m_ctx.HostToast(StringMgr.Get("regclean.s_backupfail"),
                        StringMgr.Get("misc.s2e69cd5a"), 1);
        return;
    }

    // [R111] 清理下沉到后台线程：UI 不冻结，逐规则进度回传
    if (RegCleanJobService::IsRunning())
    {
        m_ctx.HostToast(StringMgr.Get("regclean.s_cleanpartial"),
                        L"worker busy", 1);
        return;
    }
    if (!RegCleanJobService::LaunchClean(m_ctx.HostHwnd(), &eng, vSel, backup))
    {
        m_ctx.HostToast(StringMgr.Get("regclean.s_cleanpartial"),
                        L"launch failed", 1);
        return;
    }

    m_ctx.HostProgress(0, static_cast<int>(vSel.size()));
    SetRegCleanButtonsBusy(true);
    CDuiString sStart;
    sStart.Format(L"[regclean] clean started: %d rules, backup=%s",
                  static_cast<int>(vSel.size()), backup.sFile.GetData());
    m_ctx.HostLog(sStart.GetData());
}

// [R111] 后台清理进度
void CPageRegClean::OnCleanProgress(int nDone, int nTotal)
{
    m_ctx.HostProgress(nDone, nTotal);
}

// [R111] 后台清理完成
// [R162] 从 OnCleanDone 抽出：把"首条失败规则"的证据追加到提示串并记日志。
//   原实现的全失败分支与部分失败分支各有一份逐字相同的拷贝，
//   只有日志级别不同（ERROR / WARN）。合成的风险很具体：
//   改提示格式时只改一处，用户会看到两种失败程度给出不同格式的定位信息。
//   nLogLevel 由调用方传，保持两条路径的日志分级语义不变。
void CPageRegClean::AppendFirstFailHint(const RegCleanDoneMsg* pMsg,
                                        CDuiString& sErr, int nLogLevel)
{
    if (pMsg == nullptr || pMsg->vResults.empty()) return;
    for (size_t r = 0; r < pMsg->vResults.size(); ++r)
    {
        const RegCleanExecResult& rr = pMsg->vResults[r];
        if (rr.nFailed <= 0) continue;
        CDuiString sFirst;
        sFirst.Format(StringMgr.Get("regclean.s_d3_firstfail"),
                      rr.sRuleName.GetData(), rr.sFirstError.GetData());
        sErr += L"\n";
        sErr += sFirst;
        CDuiString sLog2;
        sLog2.Format(L"[regclean] first failed rule: %s key=%s",
                    rr.sRuleName.GetData(), rr.sFirstError.GetData());
        m_ctx.HostLog(sLog2.GetData(), nLogLevel);
        return;
    }
}

void CPageRegClean::OnCleanDone(RegCleanDoneMsg* pMsg)
{
    RegCleanJobService::EndClean();
    RegCleanJobService::DetachThread();
    m_ctx.HostProgress(0, 0);
    SetRegCleanButtonsBusy(false);

    int nFailed = pMsg ? pMsg->nFailed : 0;
    int nSelected = pMsg ? pMsg->nSelected : 0;
    CDuiString sBackup = pMsg ? pMsg->sBackupFile : L"";

    CDuiString sLog;
    sLog.Format(L"[regclean] clean done: %d rules, %d ok, %d failed, backup=%s",
                nSelected,
                pMsg ? pMsg->nSucceeded : 0,
                nFailed, sBackup.GetData());
    // [R128-1] 失败分级日志：全失败升 ERROR、部分升 WARN、全成功 INFO
    int nLevel = (nFailed == 0) ? static_cast<int>(ELogLevel::LOG_INFO)
                : (nFailed >= nSelected && nSelected > 0) ? static_cast<int>(ELogLevel::LOG_ERROR)
                : static_cast<int>(ELogLevel::LOG_WARN);
    m_ctx.HostLog(sLog.GetData(), nLevel);

    // [R128-1] 三档 Toast：全成功 / 部分失败 / 全部失败
    if (nFailed == 0)
    {
        m_ctx.HostToast(StringMgr.Get("regclean.s_cleandone"),
                        sBackup.GetData(), 0);
    }
    else if (nFailed >= nSelected && nSelected > 0)
    {
        // 全部失败：提示检查权限 / 是否以管理员运行
        CDuiString sErr;
        sErr.Format(StringMgr.Get("regclean.s_cleanallfail"), nFailed);
        // [R131-1] i3 证据：追加首个失败规则名 + 目标键（引擎层回传 sRuleName/sFirstError）
        // [R162] 与部分失败分支共用 AppendFirstFailHint，仅日志级别不同
        AppendFirstFailHint(pMsg, sErr, static_cast<int>(ELogLevel::LOG_ERROR));
        m_ctx.HostToast(StringMgr.Get("regclean.s_cleantitle"), sErr.GetData(), 1);
    }
    else
    {
        CDuiString sErr;
        sErr.Format(StringMgr.Get("regclean.s_cleanpartial"), nFailed);
        // [R131-1] i3 证据：部分失败也定位首错规则
        AppendFirstFailHint(pMsg, sErr, static_cast<int>(ELogLevel::LOG_WARN));
        m_ctx.HostToast(StringMgr.Get("regclean.s_cleantitle"), sErr.GetData(), 1);
    }

    // 重建（把已清理的项标 0）
    OnScan();
}

// ---- 撤销 ----
void CPageRegClean::OnRestore()
{
    // 用最近一次备份恢复（原版语义：自动使用最近备份文件，无需用户挑选）
    // 备份文件名由 CRegCleanEngine::BackupPath 以时间戳生成
    // （RegCleanBackup_YYYYMMii_HHMMSS.reg），故按 ftLastWriteTime 取最新即"最近一次"。
    CRegCleanEngine& eng = m_ctx.RegCleanEngine();
    if (eng.GetRuleCount() == 0) { OnScan(); return; }

    // [R162] 原此处内联了一份"扫描 TEMP 取最新备份"，与 L787
    //   FindLatestRegCleanBackup 重复，且两份已漂移（本处跳过 _wow32，那份不跳）。
    //   改为复用同一实现，并把 _wow32 跳过补进共用版 —— 两个入口必须看到同一个文件。
    //   副作用：嵌套由 5 层降到 2 层。
    //   注：无备份时共用实现返回 TEMP 目录本身（不以 .reg 结尾），据此区分。
    const CDuiString sNewest = FindLatestRegCleanBackup();
    const bool bNoBackup = sNewest.IsEmpty()
        || sNewest.Right(4) != L".reg";
    if (bNoBackup)
    {
        m_ctx.HostToast(StringMgr.Get("regclean.s_nobackup"),
                        StringMgr.Get("misc.s2e69cd5a"), 1);
        return;
    }

    // 预置最近备份路径，用户可直接确认；亦可改选其他 .reg 文件
    wchar_t szFile[MAX_PATH] = { 0 };
    wfmt::FormatInto(szFile, _countof(szFile), L"%s", sNewest.GetData());

    OPENFILENAME ofn;
    ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"RegClean backup (*.reg)\0*.reg\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = _countof(szFile);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (!::GetOpenFileName(&ofn))
        return;   // 用户取消

    // [P1-收尾] 原生 .reg 解析器导入备份（零 reg.exe；备份为 UTF-16LE）
    // [B3] 主文件 64 位视图；同时间戳 *_wow32.reg 按 32 位视图一并还原
    // [FIX-T2-4b] 64MB 级 .reg 导入（UI 线程同步可冻结 2s+）移后台线程。
    // 完成弹窗经 WM_PAGE_ALERT；原收尾 OnScan() 为 UI 全量重扫，改由
    // 弹窗提示用户手动点"扫描"刷新（页面重扫线程安全边界更清晰）。
    struct ImpCtx
    {
        IPageContext* pCtx;
        std::wstring  sFile;
    };
    ImpCtx* pImp = new ImpCtx{ &m_ctx, std::wstring(szFile) };
    HANDLE hImp = BackgroundJobs::Start([](void* pv) -> DWORD {
        std::unique_ptr<ImpCtx> guard(static_cast<ImpCtx*>(pv));
        IPageContext* pCtx = guard->pCtx;
        CDuiString sImpErr;
        int nFail = CRegCleanEngine::ImportRegFile(guard->sFile.c_str(), false, &sImpErr);
        CDuiString sWow = guard->sFile.c_str();
        sWow.Replace(L".reg", L"_wow32.reg");
        if (nFail == 0 && !sWow.IsEmpty()
            && ::GetFileAttributes(sWow.GetData()) != INVALID_FILE_ATTRIBUTES)
        {
            CDuiString sWowErr;
            int nWowFail = CRegCleanEngine::ImportRegFile(sWow, true, &sWowErr);
            if (nWowFail != 0 && sImpErr.IsEmpty()) sImpErr = sWowErr;
            nFail += nWowFail;
        }
        CDuiString sBody;
        if (nFail == 0)
            sBody = StringMgr.Get("regclean.s_restoredone");
        else
        {
            sBody = StringMgr.Get("regclean.s_restorefail");
            if (!sImpErr.IsEmpty()) { sBody += L"\n"; sBody += sImpErr; }
        }
        sBody += L"\n（请点「扫描」刷新当前列表）";
        CDuiString sAlert;
        sAlert.Format(L"Registry Restore|%s|%d", sBody.GetData(),
                      nFail == 0 ? 0 : 1);
        BackgroundJobs::PostHeapOrDelete(pCtx->HostHwnd(), WM_PAGE_ALERT, 0,
            std::make_unique<CDuiString>(sAlert.GetData()).release());
        pCtx->HostLog(L"[regclean] restore import finished");
        return 0;
    }, pImp, "regclean-restore");
    if (hImp)
        m_ctx.HostLog(L"[regclean] restore import started in background");
    else
    {
        delete pImp;
        m_ctx.HostToast(StringMgr.Get("regclean.s_restorefail"),
                        StringMgr.Get("misc.s2e69cd5a"), 1);
    }
}

void CPageRegClean::SelectAll(bool bCheck)
{
    CRegCleanEngine& eng = m_ctx.RegCleanEngine();
    for (int i = 0; i < eng.GetRuleCount(); ++i)
        eng.GetRule(i).bSelected = bCheck;
    BuildPage();
}

// ---- [R118-1] 创建系统还原点 ----
void CPageRegClean::OnRestorePoint()
{
    // [FIX-T2-4] SRSetRestorePointW 同步数秒~数十秒移后台线程：原在 UI 线程
    // 直接执行且无忙碌提示，还原点创建期间整窗未响应。完成 Toast 改经
    // WM_PAGE_ALERT 弹窗收口（文案映射保持原 switch 语义）。
    struct RpCtx { IPageContext* pCtx; std::wstring sDesc; };
    RpCtx* pRp = new RpCtx{ &m_ctx, L"注册表清理前自动还原点" };
    HANDLE hRp = BackgroundJobs::Start([](void* pv) -> DWORD {
        std::unique_ptr<RpCtx> guard(static_cast<RpCtx*>(pv));
        IPageContext* pCtx = guard->pCtx;
        int rc = CRegCleanEngine::CreateRestorePoint(guard->sDesc.c_str());
        DWORD nStatus = CRegCleanEngine::GetLastRestoreStatus();
        CDuiString sLog;
        sLog.Format(L"[regclean] restorepoint rc=%d status=0x%08X", rc, nStatus);
        pCtx->HostLog(sLog.GetData());

        const wchar_t* pszTitle = StringMgr.Get("regclean.s_rp_done_title");
        const wchar_t* pszBody = StringMgr.Get("regclean.s_rp_done");
        int nIcon = 0;
        switch (rc)
        {
        case 0: break;
        case 1:
            pszTitle = StringMgr.Get("regclean.s_rp_fail_title");
            pszBody = StringMgr.Get("regclean.s_rp_disabled"); nIcon = 1; break;
        case 2:
            pszTitle = StringMgr.Get("regclean.s_rp_fail_title");
            pszBody = StringMgr.Get("regclean.s_rp_nopriv"); nIcon = 1; break;
        case 3:
            pszTitle = StringMgr.Get("regclean.s_rp_fail_title");
            pszBody = StringMgr.Get("regclean.s_rp_nolib"); nIcon = 1; break;
        case 5:
            pszTitle = StringMgr.Get("regclean.s_rp_fail_title");
            pszBody = StringMgr.Get("regclean.s_rp_badparam"); nIcon = 1; break;
        default:
            pszTitle = StringMgr.Get("regclean.s_rp_fail_title");
            pszBody = StringMgr.Get("regclean.s_rp_fail"); nIcon = 1; break;
        }
        CDuiString sAlert;
        sAlert.Format(L"%s|%s|%d", pszTitle, pszBody, nIcon);
        BackgroundJobs::PostHeapOrDelete(pCtx->HostHwnd(), WM_PAGE_ALERT, 0,
            std::make_unique<CDuiString>(sAlert.GetData()).release());
        return 0;
    }, pRp, "regclean-rp");
    if (hRp)
        m_ctx.HostLog(L"[regclean] restorepoint creation started in background");
    else
    {
        delete pRp;
        m_ctx.HostToast(StringMgr.Get("regclean.s_rp_fail_title"),
                        StringMgr.Get("regclean.s_rp_fail"), 1);
    }
}

// [R167] 从 OnEvent 抽出：把勾选框状态写回对应规则。
//   三道校验缺一不可，任一不过即静默返回（不算错误）：
//     · 行号在 m_vRegCleanItems 范围内 —— 视图可能刚被重建
//     · 规则号在引擎范围内 —— 扫描结果与规则表可能短暂不同步
//     · pSender 非空 —— 消息可能来自已销毁控件
//   静默返回是有意的：这是 UI 同步动作，用户没做错什么，不该弹提示。
void CPageRegClean::RcToggleRuleAt(int nRow, CControlUI* pSender)
{
    if (nRow < 0 || nRow >= static_cast<int>(m_vRegCleanItems.size())) return;
    const int iRule = m_vRegCleanItems[nRow];
    CRegCleanEngine& eng = m_ctx.RegCleanEngine();
    if (iRule < 0 || iRule >= eng.GetRuleCount()) return;
    if (!pSender) return;
    CCheckBoxUI* pCheck = static_cast<CCheckBoxUI*>(pSender);
    eng.GetRule(iRule).bSelected = pCheck->IsSelected();
}

// [R175] 从 OnEvent 抽出：点击行时记录选中索引。
//   越界静默忽略 —— 列表可能刚被重建，旧行名仍在消息队列里，
//   这不是错误，弹提示反而打扰用户。
void CPageRegClean::RcSelectRowAt(int nRow)
{
    if (nRow < 0 || nRow >= static_cast<int>(m_vRegCleanItems.size())) return;
    m_nSelected = nRow;
    CDuiString sLog;
    sLog.Format(L"[regclean] selected row %d", nRow);
    m_ctx.HostLog(sLog.GetData());
}

void CPageRegClean::OnEvent(TNotifyUI& msg)
{
    if (msg.sType == L"click")
    {
        const wchar_t* pszName = msg.pSender ? msg.pSender->GetName() : CDuiString(L"");
        if (!pszName) return;

        // 行点击：设置选中
        if (_wcsnicmp(pszName, L"regcleanrow_", 12) == 0)
        {
            // [R175] 抽为 RcSelectRowAt，嵌套 4→3
            RcSelectRowAt(_ttoi(pszName + 12));
            return;
        }

        // 勾选切换
        if (_wcsnicmp(pszName, L"regcheck_", 9) == 0)
        {
            // [R167] 三级校验 + 写回抽为 RcToggleRuleAt，嵌套 5→4
            RcToggleRuleAt(_ttoi(pszName + 9), msg.pSender);
            return;
        }

        // 按钮
        if (lstrcmpiW(pszName, L"btn_regclean_scan") == 0) { OnScan(); return; }
        if (lstrcmpiW(pszName, L"btn_regclean_clean") == 0) { OnClean(); return; }
        if (lstrcmpiW(pszName, L"btn_regclean_backup") == 0) { OnBackup(); return; }
        if (lstrcmpiW(pszName, L"btn_regclean_restore") == 0) { OnRestore(); return; }
        // [R118-1] 创建系统还原点
        if (lstrcmpiW(pszName, L"btn_regclean_restorepoint") == 0) { OnRestorePoint(); return; }
        if (lstrcmpiW(pszName, L"btn_regclean_selectall") == 0) { SelectAll(true); return; }
        if (lstrcmpiW(pszName, L"btn_regclean_selectnone") == 0) { SelectAll(false); return; }
    }
}

// ============================================================
// [R103] 注册表清理行级右键菜单（regcleanrow_%d 命中）
// 提供"复制规则名" + "查看规则详情"两项（无破坏性动作）。
// ============================================================
// [R162] 从 OnContextMenu 抽出：从命中控件向上找到 regcleanrow_N 行控件，
// 并校验行号与规则号都落在有效区间。三道校验保持原顺序：
//   名称前缀不匹配 → 继续向上找（不是失败）；
//   行号越界 / 规则号越界 → 立即返回 false（数据已不一致，不该继续弹菜单）。
[[nodiscard]] bool CPageRegClean::ResolveRowFromHit(CControlUI* pHit, int& nRow, int& iRule)
{
    nRow = -1;
    iRule = -1;
    for (CControlUI* p = pHit; p; p = p->GetParent())
    {
        const wchar_t* psz = p->GetName();
        if (!psz) continue;
        if (wcsncmp(psz, L"regcleanrow_", 12) != 0) continue;

        nRow = _ttoi(psz + 12);
        if (nRow < 0 || nRow >= static_cast<int>(m_vRegCleanItems.size())) return false;

        iRule = m_vRegCleanItems[nRow];
        CRegCleanEngine& eng = m_ctx.RegCleanEngine();
        if (iRule < 0 || iRule >= eng.GetRuleCount()) return false;
        return true;
    }
    return false;
}

// [R162] 从 OnContextMenu 抽出：复制类菜单动作的统一实现。
//   sValue 为空 → 提示"无可复制内容"（级别 1 警告）+ sName 指明是哪条规则；
//   否则复制到剪贴板并提示成功，pszLogTag 非空时额外记一条日志。
//   三个复制项必须走同一条路径：原先它们各自内联，已经漂移成
//   "只有命令分支记日志"，用户查日志无法确认复制过键或规则名。
void CPageRegClean::CopyOrWarn(const CDuiString& sValue, const CDuiString& sName,
                               const CDuiString& sOkName,
                               const char* pszOkKey, const char* pszEmptyKey,
                               const char* pszLogTag)
{
    if (sValue.IsEmpty())
    {
        m_ctx.HostToast(StringMgr.Get(pszEmptyKey), sName.GetData(), 1);
        return;
    }
    FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), sValue.GetData());
    // sOkName 与原实现逐项对齐：rule/key 传值本身，cmd 传规则名，
    // 不改任何一条提示的实际显示内容。
    m_ctx.HostToast(StringMgr.Get(pszOkKey), sOkName.GetData(), 0);
    if (pszLogTag == nullptr) return;
    CDuiString sLog;
    sLog.Format(L"[regclean] %s: %s", pszLogTag, sValue.GetData());
    m_ctx.HostLog(sLog.GetData());
}

// [R167] 从 OnContextMenu 抽出：输出规则详情（日志留痕 + 轻提示）。
//   详情只记日志不弹窗展示全文 —— 规则的动作列表可能很长，弹窗会盖住列表；
//   用户需要核对时看日志即可，Toast 只起"已输出"的确认作用。
void CPageRegClean::RcShowRuleDetail(int nRow, int iRule, const CDuiString& sRuleName,
                                     const CDuiString& sFirstKey)
{
    CRegCleanEngine& eng = m_ctx.RegCleanEngine();
    CDuiString sLog;
    sLog.Format(L"[regclean] row %d rule detail: name=%s group=%s key=%s actions=%d",
                nRow, sRuleName.GetData(), eng.GetRule(iRule).sGroup.GetData(),
                sFirstKey.GetData(), static_cast<int>(eng.GetRule(iRule).actions.size()));
    m_ctx.HostLog(sLog.GetData());
    m_ctx.HostToast(StringMgr.Get("regclean.s_menu_detail"), sRuleName.GetData(), 0);
}

// [R167] 从 OnContextMenu 抽出：在资源管理器中选中最近备份文件。
//   无备份时给提示而不是静默 —— 用户点了菜单却什么都没发生，会以为程序坏了。
[[nodiscard]] bool CPageRegClean::RcOpenBackupFolder(int nRow, const CDuiString& sLastBackup,
                                                     const CDuiString& sRuleName)
{
    if (sLastBackup.IsEmpty())
    {
        m_ctx.HostToast(StringMgr.Get("regclean.s_menu_nobackup"), sRuleName.GetData(), 1);
        return false;
    }
    UiHelpers_OpenExplorerSelect(m_ctx.HostHwnd(), sLastBackup.GetData());
    CDuiString sLog;
    sLog.Format(L"[regclean] row %d open backup: %s", nRow, sLastBackup.GetData());
    m_ctx.HostLog(sLog.GetData());
    return true;
}

bool CPageRegClean::OnContextMenu(POINT pt, bool bFromContextMsg)
{
    if (bFromContextMsg) return false;

    CControlUI* pHit = m_ctx.HostFindControlAt(pt);
    if (!pHit) return false;

    // [R162] 命中行定位抽为 ResolveRowFromHit：原先 4 层循环只为得到 (nRow, iRule)，
    //   与菜单组装无耦合；抽出后本函数遍历层数由 5 降到 3。
    int nRow = -1;
    int iRule = -1;
    if (!ResolveRowFromHit(pHit, nRow, iRule)) return false;
    {
        CRegCleanEngine& eng = m_ctx.RegCleanEngine();
        CDuiString sRuleName = DismResolveText(eng.GetRule(iRule).sName.GetData());

        // [R128-1] 组装右键菜单动作所需数据：目标键、清理命令、最近备份
        CDuiString sFirstKey;
        CDuiString sCleanCmd;
        ResolveRuleCommand(eng.GetRule(iRule), sFirstKey, sCleanCmd);

        const CDuiString sLastBackup = FindLatestRegCleanBackup();

        enum { CP_COPY_RULE = 6201, CP_DETAIL = 6202,
               CP_OPEN_BACKUP = 6203, ID_COPY_CMD = 6204, CP_COPY_KEY = 6205 }; // [R128-1]
        HMENU hMenu = ::CreatePopupMenu();
        if (!hMenu) return false;

        ::AppendMenu(hMenu, MF_STRING, CP_COPY_RULE, StringMgr.Get("regclean.s_menu_copy_rule"));
        ::AppendMenu(hMenu, MF_STRING, CP_COPY_KEY, StringMgr.Get("regclean.s_menu_copy_key"));
        ::AppendMenu(hMenu, MF_STRING, ID_COPY_CMD, StringMgr.Get("regclean.s_menu_copy_cmd"));
        ::AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenu(hMenu, MF_STRING, CP_DETAIL, StringMgr.Get("regclean.s_menu_detail"));
        ::AppendMenu(hMenu, MF_STRING, CP_OPEN_BACKUP, StringMgr.Get("regclean.s_menu_open_backup"));

        int iCmd = ::TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                    pt.x, pt.y, 0, m_ctx.HostHwnd(), nullptr);
        ::DestroyMenu(hMenu);
        if (iCmd == 0) return true;

        switch (iCmd)
        {
        // [R162] 三个复制分支同构（空则提示 / 非空则复制+提示），抽 CopyOrWarn。
        //   原先三份拷贝已出现不一致：只有"复制命令"分支额外记了日志，
        //   另两项没有 —— 排查"我到底复制过什么"时证据不全。
        case CP_COPY_RULE:
            // 成功提示第二实参沿用原实现：rule/key 用值本身，cmd 用规则名
            CopyOrWarn(sRuleName, sRuleName, sRuleName,
                       "regclean.s_menu_copied_rule", "regclean.s_menu_nokey", nullptr);
            break;
        case CP_COPY_KEY:
            CopyOrWarn(sFirstKey, sRuleName, sFirstKey,
                       "regclean.s_menu_copied_key", "regclean.s_menu_nokey", nullptr);
            break;
        case ID_COPY_CMD:
            CopyOrWarn(sCleanCmd, sRuleName, sRuleName,
                       "regclean.s_menu_copied_cmd", "regclean.s_menu_nocmd",
                       "cmd copied");
            break;
        // [R167] 两个分支体抽出，OnContextMenu 嵌套 5→3
        case CP_DETAIL:
            RcShowRuleDetail(nRow, iRule, sRuleName, sFirstKey);
            break;
        case CP_OPEN_BACKUP:
            // 返回值只表示"是否真的打开了资源管理器"。无备份的失败分支已在
            //   RcOpenBackupFolder 内部弹 Toast 告知用户，调用方没有别的补救
            //   动作可做；且菜单命令已消费，本函数无论如何返回 true。
            (void)RcOpenBackupFolder(nRow, sLastBackup, sRuleName);
            break;
        default: break;
        }
        return true;
    }
    return false;
}

// ============================================================
// [R5] 后台消息统一入口（原壳 OnPgRegCleanProgress/ione/ScanDone）
// 注意：三条消息全部 kMsgOwnShell，成对 int*/消息体的 delete 在壳
// ReleaseMsgParam 单出口，本页只读不删，保持原释放条件不变。
// ============================================================

bool CPageRegClean::OnAsyncMessage(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_REGCLEN_PROGRESS:
    {
        // wParam/lParam 为成对 new 的 int*（total/done），双非空才回调。
        int* pDone  = (int*)lParam;
        int* pTotal = (int*)wParam;
        if (pDone && pTotal)
            OnCleanProgress(*pDone, *pTotal);
        return true;
    }

    case WM_REGCLEN_DONE:
        if (RegCleanDoneMsg* pMsg = (RegCleanDoneMsg*)lParam)
            OnCleanDone(pMsg);
        return true;

    case WM_REGCLEN_SCANDONE:
        // 原壳对 OnScanDone 为无条件调用（含空指针），保持。
        OnScanDone((RegCleanScanDoneMsg*)lParam);
        return true;

    default:
        return false;
    }
}
#include <memory>   // [C++17 R3] make_unique（置于文件尾，避免影响 verify 行号）
// [REFACTOR] 从 OnContextMenu 提取：查找 TEMP 下最新的 RegCleanBackup_*.reg。
// 无备份时返回 TEMP 目录本身（调用方据此打开目录而非选中文件）。
// 逐字搬迁零逻辑改动。
// [R167] 从 FindLatestRegCleanBackup 抽出：按修改时间取最新的备份文件。
//   返回 true 时 sOut 为完整路径（目录 + 文件名）；
//   返回 false 表示没有符合条件文件，调用方自行决定回退值。
//
//   两条过滤规则：
//     · 跳过目录
//     · [B3] 跳过 32 位视图伴随文件（*_wow32.reg）—— 它由主文件触发导入，
//       单独选中会被当成"最近备份"，导致还原只还原一半。
//
//   【保留的历史差异，不当 bug 改】本函数拼出的路径保证以文件名结尾；
//   调用方在无命中时返回的是 sDir（不带尾反斜杠处理）。
//   即"有备份"与"无备份"两条路径返回的目录形式不同，调用方 `Right(4)==".reg"`
//   正是依赖这个差异来区分。若把两条路径统一，OnRestore 的判定会失效。
[[nodiscard]] bool CPageRegClean::RcNewestFileByTime(const wchar_t* pszPattern,
                                                     const CDuiString& sDir,
                                                     CDuiString& sOut)
{
    sOut.Empty();
    WIN32_FIND_DATA fd;
    PRCFindGuard hFind(::FindFirstFile(pszPattern, &fd));
    if (hFind == INVALID_HANDLE_VALUE) return false;

    ULARGE_INTEGER uiBest = { 0 };
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (wcsstr(fd.cFileName, L"_wow32") != nullptr) continue;

        ULARGE_INTEGER ui;
        ui.LowPart = fd.ftLastWriteTime.dwLowDateTime;
        ui.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        if (ui.QuadPart <= uiBest.QuadPart) continue;

        uiBest = ui;
        sOut = sDir;
        if (!sOut.IsEmpty() && sOut[sOut.GetLength() - 1] != L'\\')
            sOut += L"\\";
        sOut += fd.cFileName;
    } while (::FindNextFile(hFind, &fd));
    return !sOut.IsEmpty();
}

CDuiString CPageRegClean::FindLatestRegCleanBackup()
{
    wchar_t szTmp[MAX_PATH] = { 0 };
    ::GetTempPath(_countof(szTmp), szTmp);
    const CDuiString sDir(szTmp);

    CDuiString sPattern = sDir;
    if (!sPattern.IsEmpty() && sPattern[sPattern.GetLength() - 1] != L'\\')
        sPattern += L"\\";
    sPattern += L"RegCleanBackup_*.reg";

    // [R167] 遍历取值抽为 RcNewestFileByTime，嵌套 5→3
    CDuiString sLastBackup;
    if (RcNewestFileByTime(sPattern.GetData(), sDir, sLastBackup))
        return sLastBackup;

    // 无备份时返回 temp 目录本身（与有备份时的路径形式不同，见函数注释）
    return sDir;
}

// [R175] 从 ResolveRuleCommand 抽出：把规则动作拼成 reg.exe 命令行。
//   三种动作：DELETE（整键或单值）/ WRITE（写值）/ MOVE（改名）；
//   其它类型 continue 跳过而不是报错 —— 规则文件可能含新版工具才懂的动作，
//   旧版忽略它仍能执行剩余部分，比整条规则失败对用户更友好。
//   多条用 "  &&  " 串接：reg.exe 在 && 后失败会中断整串，
//   与"逐条执行并汇总失败"不同，这是既有语义，不改。
[[nodiscard]] CDuiString CPageRegClean::RcBuildCleanCommand(const RegCleanRule& r)
{
    CDuiString sCleanCmd;
    bool bFirst = true;
    for (size_t k = 0; k < r.actions.size(); ++k)
    {
        const RegCleanAction& a = r.actions[k];
        CDuiString sOne;
        if (a.eType == RegCleanActionType::REGCLEAN_ACTION_DELETE)
        {
            if (a.sValue.IsEmpty())
                sOne.Format(L"reg delete \"%s\" /f", a.sKey.GetData());
            else
                sOne.Format(L"reg delete \"%s\" /v \"%s\" /f",
                            a.sKey.GetData(), a.sValue.GetData());
        }
        else if (a.eType == RegCleanActionType::REGCLEAN_ACTION_WRITE)
        {
            sOne.Format(L"reg add \"%s\" /v \"%s\" /t %s /d \"%s\" /f",
                        a.sKey.GetData(), a.sValue.GetData(),
                        a.sType.IsEmpty() ? L"REG_SZ" : a.sType.GetData(),
                        a.sData.GetData());
        }
        else if (a.eType == RegCleanActionType::REGCLEAN_ACTION_MOVE)
        {
            sOne.Format(L"reg move \"%s\" \"%s\" /f",
                        a.sKey.GetData(), a.sDstKey.GetData());
        }
        else continue;

        if (sOne.IsEmpty()) continue;
        if (!bFirst) sCleanCmd += L"  &&  ";
        sCleanCmd += sOne;
        bFirst = false;
    }
    return sCleanCmd;
}

void CPageRegClean::ResolveRuleCommand(const RegCleanRule& r,
                                       CDuiString& sFirstKey, CDuiString& sCleanCmd)
{
    for (size_t k = 0; k < r.actions.size() && sFirstKey.IsEmpty(); ++k)
    {
        if (!r.actions[k].sKey.IsEmpty())
            sFirstKey = r.actions[k].sKey;
    }
    if (!sFirstKey.IsEmpty())
    {
        // [R175] 命令拼装抽为 RcBuildCleanCommand，嵌套 4→2
        sCleanCmd = RcBuildCleanCommand(r);
    }
}

// [R232] Re-colour the rule rows after a light<->dark switch.
//
// PageBase.h contract: colours only -- no Build()/Add/Remove (that would re-run the
// scan-driven build and lose the checked state).
//
// Row layout (BuildPage): 0 = checkbox, 1 = rule name (Ink), 2 = scan status (InkMuted).
//   NOTE: there is NO risk badge on this page -- BdStyleRiskBadge lives in
//   Page_Cleanup, not here. Guessing a 4-column layout would have left the status
//   column unrefreshed and painted a non-existent badge slot.
void CPageRegClean::RefreshThemeColors()
{
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(
        m_ctx.HostFindControl(L"regclean_list"));
    if (!pList) return;

    for (int i = 0; i < pList->GetCount(); ++i)
    {
        CControlUI* pRowCtl = pList->GetItemAt(i);
        if (!pRowCtl) continue;
        if (!dynamic_cast<CClickRowUI*>(pRowCtl)) continue;   // empty hint (27 轮补的)

        pRowCtl->SetBkColor(tokens::Token(tokens::EToken::Surface));

        IContainerUI* pRow = static_cast<IContainerUI*>(
            pRowCtl->GetInterface(DUI_CTR_CONTAINER));
        if (!pRow) continue;
        for (int c = 0; c < pRow->GetCount(); ++c)
        {
            CTextUI* pText = dynamic_cast<CTextUI*>(pRow->GetItemAt(c));
            if (!pText) continue;
            if (c == 1)      pText->SetTextColor(tokens::Token(tokens::EToken::Ink));
            else             pText->SetTextColor(tokens::Token(tokens::EToken::InkMuted));
        }
    }
}
