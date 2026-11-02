#include "stdafx.h"
#include "ThemeTokens.h"   // [R201] 强调色令牌（Win10/11 均跟随系统主题色）

// ============================================================
// [R16][T10] 工具箱弹窗跨 kind 后台作业执行设施（自 ToolDialogPage.cpp
// 公共壳机械迁移，零行为改动）：TImgxJobHolder/统一线程过程、
// TaskShield 模态进度三件套、RunImageXJob/OnJobDone 作业收口，
// 以及旧 diskpart 薄委托（死代码原样保留）。
// ============================================================
#include "SafeThread.h"      // ESD 解密等工作线程异常边界
#include "BackgroundJobs.h"  // [CRASHFIX] 后台作业统一收口（SEH 边界+退出等待）

#include "ToolDialogPage.h"
#include "ToolDlgInternal.h" // tdlg::TJobResult/TPostJobSink/CTpProgressWnd

#include "ToolService/IToolServiceHost.h"
#include "ToolService/WizDiskPart.h"  // 旧 diskpart 薄委托原语
#include "StringManager.h"
#include "HostInterfaces.h"  // static_cast<int>(ELogLevel::LOG_INFO)/static_cast<int>(ELogLevel::LOG_WARN)/static_cast<int>(ELogLevel::LOG_ERROR)

#include <cwctype>   // towlower（OnJobDone 错误关键词分级）
#include <string>

using i18n::CStringManager;
// ============================================================
// [R6-05] 磁盘/diskpart 原语已下沉 ToolService/WizDiskPart.cpp；
// 这里保留 CDuiString 薄委托使 Apply/BootFix 向导调用点零改动。
// ============================================================
[[nodiscard]] static bool TdVolumeDiskNumber(wchar_t cLetter, DWORD& pdwDisk)
{
    return toolsvc::WizVolumeDiskNumber(cLetter, pdwDisk);
}

[[nodiscard]] static bool TdRunDiskpart(const CDuiString& sBody, DWORD dwTimeout, CDuiString* psOut)
{
    std::wstring wOut;
    bool b = toolsvc::WizRunDiskpart(sBody.GetData(), dwTimeout, psOut ? &wOut : nullptr);
    if (b && psOut) *psOut = wOut.c_str();
    return b;
}

static wchar_t TdFreeLetter()
{
    return toolsvc::WizFreeLetter();
}

static bool TdFindEspIndex(DWORD dwDisk, int& pnPart)
{
    return toolsvc::WizFindEspIndex(dwDisk, pnPart);
}

// [CRASHFIX] RunImageXJob 的作业持有者（原函数内局部结构，提升为文件级
// 以便统一线程过程使用；std::function 不可跨裸指针传递，必须由堆持有者承载）
struct TImgxJobHolder
{
    HWND       hDlg;
    HWND       hProg;
    // [R7] 作业闭包来自 IIsoImageService；进度经 toolsvc::IToolJobSink 回传
    toolsvc::WimJobFn fn;
    CDuiString sTitle;
    CDuiString sOkMsg;
    bool       bDirty;
    bool       bClose;
    int        nKind;
};

// [CRASHFIX] 通用映像作业线程：替代裸 std::thread::detach，
// SafeCreateThread 外层 SEH 兜底 + 本过程 C++ catch 保析构。
// [R-TAILFIX 同型] 作业体 SEH 边界：pH->fn 内含 wimgapi/DISM 原生调用，
// /EHsc 下 catch(...) 捕不到 AV；线程一死 WM_TDLG_JOBDONE 永不投递，
// 弹窗遮罩（m_bTaskRunning + 禁用 body）永久锁死。本函数无 C++ 对象，
// 允许 __try；异常降级为失败结果，DONE 照常回投。
static void ImgxJobSehBody(TImgxJobHolder* pH, toolsvc::IToolJobSink* pSink,
                           bool& bOk, std::wstring& sErr)
{
    __try
    {
        bOk = pH->fn(pSink, sErr);
    }
    __except (CrashGuardFilter(GetExceptionInformation(), "imgx-job-seh"))
    {
        bOk = false;
        if (sErr.empty()) sErr = L"映像操作发生硬件异常（已兜底收口）";
    }
}

static DWORD WINAPI ImgxJobThreadProc(void* pv)
{
    std::unique_ptr<TImgxJobHolder> guard(reinterpret_cast<TImgxJobHolder*>(pv)); // [C++17 R3] ownership from Start caller
    TImgxJobHolder* pH = guard.get();
    HWND hDlg = pH->hDlg;
    HWND hProg = pH->hProg;

    // [C++17 R3] helper PostHeapOrDelete always takes ownership (posts or deletes in place)
    tdlg::TJobResult* r = std::make_unique<tdlg::TJobResult>().release();
    r->nKind = pH->nKind;
    r->sTitle = pH->sTitle;
    r->sOkMsg = pH->sOkMsg;
    r->bDirty = pH->bDirty;
    r->bCloseOnOk = pH->bClose;

    // [R7] 作业回调在工作线程触发，只做 PostMessage，绝不触碰 UI 控件
    // [R16-T9] 本地 TImgxSink 收编为文件级统一 tdlg::TPostJobSink
    tdlg::TPostJobSink sink;
    sink.hDlg = hDlg;
    sink.hProg = hProg;

    std::wstring sErr;
    bool bOk = false;
    ImgxJobSehBody(pH, &sink, bOk, sErr);
    r->bOk = bOk;
    if (!bOk)
        r->sMsg = sErr.empty() ? L"映像操作失败" : sErr;

    // [GC-07] 统一收尾：投递失败由助手就地回收，不再各写一套
    BackgroundJobs::PostHeapOrDelete(hDlg, WM_TDLG_JOBDONE, 0, r);
    // [C++17 R3] thread ctx deleted by guard scope exit (ownership from Start caller)
    return 0;
}

// ============================================================
// [B4] 内嵌任务进度面板（原版 Dism++ 路线：进度在界面内呈现，
// 不再弹原生 Win32 小窗——后者在高 DPI 下窗口尺寸不缩放、字体按
// DPI 放大，出现第二行文字被裁成乱码横线、0% 贴边、进度条出界）。
// 面板为 dlg_root 上的 float 覆盖层（标题栏之下），所有控件走 DuiLib
// 字体/缩放体系；内容区置灰禁用防重入，面板自身吞掉鼠标点击。
// ============================================================
void CToolDialogPage::BuildTaskPanel()
{
    if (!m_pRoot) return;

    // 覆盖层：铺满标题栏以下整个客户区，默认隐藏
    CVerticalLayoutUI* pMask = std::make_unique<CVerticalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pRoot->Add
    pMask->SetFloat(true);
    pMask->SetAttribute(L"pos", L"0,50,0,0");
    pMask->SetBkColor(0xF2000000 | (tokens::Token(tokens::EToken::SurfaceAlt) & 0x00FFFFFF));   // [R220] keep 0xF2 alpha, RGB from token
    pMask->SetVisible(false);

    // 上弹性 → 卡片垂直居中
    CControlUI* pTop = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pMask->Add
    pMask->Add(pTop);

    CHorizontalLayoutUI* pMid = std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via pMask->Add
    pMid->SetFixedHeight(132);
    CControlUI* pLeft = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pMid->Add
    pMid->Add(pLeft);

    CVerticalLayoutUI* pCard = std::make_unique<CVerticalLayoutUI>().release(); // [C++17 R3] framework-owned via pMid->Add
    pCard->SetFixedWidth(520);
    pCard->SetBkColor(tokens::Token(tokens::EToken::Surface));
    pCard->SetAttribute(L"bordercolor", tokens::TokenHex(tokens::EToken::Border));
    pCard->SetAttribute(L"bordersize", L"1");
    pCard->SetAttribute(L"inset", L"24,16,24,16");

    CTextUI* pCap = MkText(L"任务进行中", 2, 0xFF666666);
    pCap->SetFixedHeight(22);
    pCap->SetMouseEnabled(false);
    pCard->Add(pCap);

    CControlUI* pGap1 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pCard->Add
    pGap1->SetFixedHeight(8);
    pCard->Add(pGap1);

    CTextUI* pStage = std::make_unique<CTextUI>().release(); // [C++17 R3] framework-owned via pCard->Add
    pStage->SetFont(1);
    pStage->SetTextColor(tokens::Token(tokens::EToken::Ink));
    pStage->SetFixedHeight(44);
    pStage->SetAttribute(L"align", L"left");
    pStage->SetAttribute(L"valign", L"top");
    pStage->SetMouseEnabled(false);
    pCard->Add(pStage);

    CControlUI* pGap2 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pCard->Add
    pGap2->SetFixedHeight(12);
    pCard->Add(pGap2);

    CHorizontalLayoutUI* pBarRow = std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via pCard->Add
    pBarRow->SetFixedHeight(20);

    CProgressUI* pBar = std::make_unique<CProgressUI>().release(); // [C++17 R3] framework-owned via pBarRow->Add
    pBar->SetHorizontal(true);
    pBar->SetShowText(false);
    pBar->SetMinValue(0);
    pBar->SetMaxValue(100);
    pBar->SetValue(0);
    pBar->SetFixedHeight(8);
    pBar->SetAttribute(L"valign", L"vcenter");
    pBar->SetAttribute(L"bkcolor", tokens::TokenHex(tokens::EToken::SurfaceSunk));   // [R220] progress trough
    pBar->SetAttribute(L"forecolor", tokens::TokenHex(tokens::EToken::Accent));   // [R210] 改令牌
    pBar->SetMouseEnabled(false);
    pBarRow->Add(pBar);

    CControlUI* pGap3 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pBarRow->Add
    pGap3->SetFixedWidth(14);
    pBarRow->Add(pGap3);

    CTextUI* pPct = std::make_unique<CTextUI>().release(); // [C++17 R3] framework-owned via pBarRow->Add
    pPct->SetFont(5);
    pPct->SetText(L"0%");
    pPct->SetTextColor(tokens::Token(tokens::EToken::Accent));
    pPct->SetFixedWidth(56);
    pPct->SetAttribute(L"align", L"right");
    pPct->SetAttribute(L"valign", L"vcenter");
    pPct->SetMouseEnabled(false);
    pBarRow->Add(pPct);

    pCard->Add(pBarRow);
    pMid->Add(pCard);

    CControlUI* pRight = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pMid->Add
    pMid->Add(pRight);
    pMask->Add(pMid);

    // 下弹性
    CControlUI* pBottom = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pMask->Add
    pMask->Add(pBottom);

    m_pRoot->Add(pMask);

    m_pTaskMask  = pMask;
    m_pTaskStage = pStage;
    m_pTaskBar   = pBar;
    m_pTaskPct   = pPct;
}

void CToolDialogPage::SetTaskStage(const wchar_t* pszText)
{
    if (m_pTaskStage) m_pTaskStage->SetText(pszText ? pszText : L"");
}

void CToolDialogPage::SetTaskProgress(int nPercent)
{
    if (nPercent < 0) nPercent = 0;
    if (nPercent > 100) nPercent = 100;
    if (m_pTaskBar) m_pTaskBar->SetValue(nPercent);
    if (m_pTaskPct)
    {
        CDuiString s;
        s.Format(L"%d%%", nPercent);
        m_pTaskPct->SetText(s.GetData());
    }
}

void CToolDialogPage::ShowTaskPanel(const wchar_t* pszText, bool bMarquee)
{
    m_bTaskMarquee = bMarquee;
    m_nMarqueePos  = 0;
    m_bMarqueeDir  = true;

    SetTaskStage(pszText);
    if (m_pTaskBar) m_pTaskBar->SetValue(0);
    if (m_pTaskPct) m_pTaskPct->SetText(bMarquee ? L"" : L"0%");

    // 内容区置灰禁用（确定/取消/浏览/输入全部防重入）
    if (m_pBody) m_pBody->SetEnabled(false);
    if (m_pTaskMask) m_pTaskMask->SetVisible(true);

    if (bMarquee && m_hWnd)
        ::SetTimer(m_hWnd, TASK_ANIM_TIMER_ID, 30, nullptr);
}

void CToolDialogPage::HideTaskPanel()
{
    if (m_hWnd) ::KillTimer(m_hWnd, TASK_ANIM_TIMER_ID);
    if (m_pTaskMask) m_pTaskMask->SetVisible(false);
    if (m_pBody) m_pBody->SetEnabled(true);
    m_bTaskMarquee = false;
}

void CToolDialogPage::BeginTaskShield(const wchar_t* pszText, bool bMarquee)

{

    if (m_bTaskRunning) return;

    m_bTaskRunning = true;

    // [B4] 原版路线：内嵌 DuiLib 进度面板（不再创建禁用属主的原生进度窗）
    CDuiString sText = pszText ? pszText : StringMgr.Get("misc.s35aa1472");
    ShowTaskPanel(sText.GetData(), bMarquee);

}



void CToolDialogPage::UpdateTaskShield(int nPercent, const wchar_t* pszText)

{

    // 工作线程契约不变：只允许 PostMessage；面板消息由 HandleMessage 收口
    if (pszText)
    {
        // [C++17 阶段3] Post 消息所有权移交 UI，Post 失败自删
        auto p = std::make_unique<CDuiString>(pszText); // [C++17 R3] ownership to UI receiver on successful post
        if (::PostMessageW(m_hWnd, WM_TP_STAGETEXT, 0, reinterpret_cast<LPARAM>(p.get())))
            p.release(); // [C++17 R3] receiver deletes; unique_ptr self-deletes when post fails
    }
    if (nPercent >= 0)
        ::PostMessageW(m_hWnd, WM_TP_PROGRESS, static_cast<WPARAM>(nPercent), 0);

}



void CToolDialogPage::EndTaskShield()

{

    m_bTaskRunning = false;

    HideTaskPanel();

    // 原生操作结束后统一投递给处理器：ImageX 变更成功时重读列表（m_imgx.bDirty），
    // 其它弹窗类型在处理器内为空操作。处理器不再回调本函数，故无递归。
    if (::IsWindow(m_hWnd))
        ::PostMessageW(m_hWnd, WM_TDLG_TASKDONE, 0, 0);

}

// ============================================================
// [2026-10-12] 后台映像任务统一框架
//   std::thread + detach；工作线程经 IWimProgress 直接收 wimgapi
//   WIM_MSG_PROGRESS(0-100)，PostMessage 到模态进度窗；
//   结束后 WM_TDLG_JOBDONE 回 UI 线程：关闭进度窗、成功/失败必弹窗、
//   按需重读 ImageX、按需关闭对话框。
// ============================================================
// [R7] CScaledWimProgress 多索引进度适配器已随 wimgapi 作业下沉
// ToolService/IsoImageService.cpp。

void CToolDialogPage::RunImageXJob(CDuiString sText, CDuiString sTitle,
                               CDuiString sOkMsg, bool bDirtyOnOk,
                               bool bCloseOnOk, TImgxJobFn fn, int nKind)
{
    if (m_bTaskRunning) return;
    BeginTaskShield(sText.GetData(), false);

    // [B4] 进度/阶段消息改投内嵌面板（由本对话框 HandleMessage 收口）；
    // hDlg 与 hProg 同为 m_hWnd，消息号不同互不冲突。
    HWND hDlg = m_hWnd, hProg = m_hWnd;

    // [CRASHFIX] std::function 装入堆持有者，经 BackgroundJobs 走
    // SafeCreateThread 双层异常边界（原裸 std::thread::detach 已移除）
    // [C++17 阶段3] 线程 ctx：所有权移交 ImgxJobThreadProc，Start 失败自删
    auto ctx = std::make_unique<TImgxJobHolder>(); // [C++17 R3] ownership to thread proc on successful start
    ctx->hDlg = hDlg;
    ctx->hProg = hProg;
    ctx->fn = std::move(fn);
    ctx->sTitle = sTitle;
    ctx->sOkMsg = sOkMsg;
    ctx->bDirty = bDirtyOnOk;
    ctx->bClose = bCloseOnOk;
    ctx->nKind = nKind;

    if (BackgroundJobs::Start(&ImgxJobThreadProc, ctx.get(), "imgx-job"))
        ctx.release(); // [C++17 R3] thread proc deletes via its unique_ptr guard
    else
    {
        EndTaskShield();
        ::MessageBoxW(m_hWnd, L"后台作业启动失败（线程资源不足）。",
                      sTitle.GetData(), MB_OK | MB_ICONERROR);
        ctx.reset(); // [C++17 R3] delete holder at the original failure-cleanup site
    }
}

void CToolDialogPage::OnJobDone(LPARAM lParam)
{
    std::unique_ptr<tdlg::TJobResult> guard(reinterpret_cast<tdlg::TJobResult*>(lParam)); // [C++17 R3] ownership from worker post
    tdlg::TJobResult* r = guard.get();
    if (!r) return;

    // 先置脏位再结束遮罩：EndTaskShield 会 Post WM_TDLG_TASKDONE 触发重读
    if (r->bDirty)
        m_imgx.bDirty = true;

    EndTaskShield();

    if (r->bOk)
    {
        // 成功路径必须弹窗（不再只写日志）
        m_pHost->HostLog(r->sOkMsg, static_cast<int>(ELogLevel::LOG_INFO));
        ::MessageBoxW(m_hWnd, r->sOkMsg.GetData(),
                      r->sTitle.GetData(), MB_OK | MB_ICONINFORMATION);
        if (r->bCloseOnOk)
            Close(IDOK);
    }
    else
    {
        CDuiString sErr(r->sMsg.c_str());
        if (r->nKind == 1)
        {
            // 释放映像：按错误关键词分级（保留原中文映射）
            std::wstring sLow = r->sMsg;
            for (auto& c : sLow) c = (wchar_t)towlower(c);
            int nLv = static_cast<int>(ELogLevel::LOG_ERROR);
            CDuiString sHuman;
            if (sLow.find(L"permission") != std::wstring::npos
                || sLow.find(L"access denied") != std::wstring::npos
                || sLow.find(L"denied") != std::wstring::npos)
            {
                sHuman = StringMgr.Get("toolbox.s_wim_noperm");
            }
            else if (sLow.find(L"disk full") != std::wstring::npos
                     || sLow.find(L"insufficient") != std::wstring::npos)
            {
                sHuman = StringMgr.Get("toolbox.s_wim_diskfull");
            }
            else if (sLow.find(L"in use") != std::wstring::npos
                     || sLow.find(L"locked") != std::wstring::npos
                     || sLow.find(L"volume") != std::wstring::npos)
            {
                sHuman = StringMgr.Get("toolbox.s_wim_locked");
                nLv = static_cast<int>(ELogLevel::LOG_WARN);
            }
            if (!sHuman.IsEmpty())
                m_pHost->HostLog(sHuman, nLv);
        }
        m_pHost->HostLog(sErr, static_cast<int>(ELogLevel::LOG_ERROR));
        ::MessageBoxW(m_hWnd, sErr.GetData(),
                      r->sTitle.GetData(), MB_OK | MB_ICONERROR);

        // 释放成功但引导链失败：给出 WinPE 手动修复指引（原成功路径内嵌弹窗）
        if (r->nKind == 1 && r->bBootAttempted && !r->bBootOk)
        {
            CDuiString sMsg;
            sMsg.Format(
                L"Image applied, but boot configuration FAILED.\r\n\r\n"
                   "Detail: %s\r\n\r\n"
                   "The system may not boot. You can manually repair in WinPE:\r\n"
                   "  UEFI : bcdboot X:\\Windows /s S: /f UEFI\r\n"
                   "  BIOS : bcdboot X:\\Windows /s C: /f BIOS  +  bootsect /nt60 C: /mbr",
                r->sBootErr.GetData());
            ::MessageBoxW(m_hWnd, sMsg.GetData(),
                          L"Dism++ Apply - boot repair failed",
                          MB_OK | MB_ICONERROR);
        }
    }

    // [C++17 R3] receiver-side delete happens at guard scope exit
}
