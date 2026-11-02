// ============================================================
// Page_ToolsDism.cpp —— 工具箱页「DISM 收尾 + 工具进度轮询」
// （从 Page_Tools.cpp 拆出）。
//
// [R189] Page_Tools.cpp 拆前 1,070 行、规模分 9.54（六页最低总分页 9.77）。
// 本文件承接六个成员，合计 139 行，边界按「后台工具运行期的 UI 回执」划分：
//   · OnDismLog            WM_DISM_LOG   行级日志（warning 前缀 [!]）
//   · OnDismDone           WM_DISM_DONE  退出码分级日志 + 状态条
//   · OnDismProgress       WM_DISM_PROGRESS 百分比 → 进度条
//   · StartToolProgressPoll 开启 800ms 轮询
//   · StopToolProgressPoll  停止轮询 + 隐藏进度条 + 清空标签
//   · OnToolProgressTimer   轮询一次：读 progress.txt，按 status/pct 更新
//
// 闭包说明：六个成员声明见 Page_Tools.h。跨 TU 依赖六项且都是头文件级：
//   · m_ctx（HostLog / HostSetCtrlText / HostFindControl / HostProgress / HostHwnd）
//   · m_bToolProgressOn（轮询在途标志，头文件已声明）
//   · StringMgr（toolbox.s_progress_* 语言键，键名逐字保持，未新增）
//   · ExternalToolService（ReadToolProgress / OnDone）
//   · msgparam::FromParam<CDuiString>（MsgParam.h，消息参数所有权移交）
//   · TOOL_PROGRESS_TIMER_ID（本页定时器 ID）
//
// 逐字搬运，零逻辑改动。
// ============================================================

#include "stdafx.h"

#include "Page_Tools.h"
#include "Page_ToolsCommon.h"

#include "StringManager.h"
#include "ExternalToolService.h"   // ReadToolProgress / OnDone
#include "FrameWnd_Messages.h"     // WM_DISM_LOG / WM_DISM_DONE / WM_DISM_PROGRESS
#include "MsgParam.h"              // msgparam::FromParam
#include "LogService.h"            // ELogLevel
#include "CustomControls.h"        // CProgressUI / CControlUI

#include <memory>

using namespace DuiLib;
using i18n::CStringManager;

// [R189] 工具进度轮询的两个常量。原在多处硬编码：
//   · 800 是轮询间隔（progress.txt 由 bat 写，秒级更新，800ms 足够且不烧 CPU）
//   · 100 既是百分比上限，也是 HostProgress 的满值分母
// 具名后「间隔」与「满刻度」两个语义不再与裸数字混在一起。
inline constexpr UINT kToolsProgressPollMs = 800;
inline constexpr int  kToolsProgressFullPct = 100;

// ============================================================
// WM_DISM_* UI 收尾（原 HandleCustomMessage 分支 + CFrameWnd::OnDismDone 后半段）
// ============================================================

void CPageTools::OnDismLog(WPARAM wParam, LPARAM lParam)

{
    std::unique_ptr<CDuiString> spLine(msgparam::FromParam<CDuiString>(lParam));  // [C++17 R3] 接收端接管
    CDuiString* pLine = spLine.get();
    if (pLine)
    {
        // [L-04] 级别在消息源头（wParam）本来就有，此前却被丢掉、只靠拼 "[!] "
        //   + DetectLevel 反猜文本标签，属于"级别靠字符串猜"的架构缺陷。
        //   现在直接把 wParam 映射成级别传给 HostLog：
        //     wParam==1（DISM/外部工具告警）→ WARN
        //     其余                            → INFO
        //   "[!] " 前缀保留：日志正文里带这个符号便于人工扫读，且
        //   DetectLevel 仍认它（兼容其它未改造的旧路径），两者不冲突。
        if (wParam == 1)
        {
            CDuiString sWarn = L"[!] ";
            sWarn += *pLine;
            m_ctx.HostLog(sWarn.GetData(), static_cast<int>(ELogLevel::LOG_WARN));
        }
        else
        {
            m_ctx.HostLog(pLine->GetData(), static_cast<int>(ELogLevel::LOG_INFO));
        }
        // [C++17 R3] spLine unique_ptr 自动释放，所有权由投递方移交
    }
}

void CPageTools::OnDismDone(int nExitCode)

{
    // [R111] 失败分级：退出码 0=成功、-1=未等待、非 0=失败（static_cast<int>(ELogLevel::LOG_ERROR) + Toast）
    CDuiString s;
    s.Format(L"tool exit code: %d", nExitCode);
    m_ctx.HostLog(s.GetData());
    if (nExitCode == 0)
    {
        m_ctx.HostToast(StringMgr.Get("toolbox.s_done_title"),
                        StringMgr.Get("toolbox.s_done_ok"), 0);
    }
    else if (nExitCode == -1)
    {
        m_ctx.HostLog(StringMgr.Get("toolbox.s_done_nowait"), static_cast<int>(ELogLevel::LOG_WARN));
        m_ctx.HostToast(StringMgr.Get("toolbox.s_done_title"),
                        StringMgr.Get("toolbox.s_done_nowait"), 1);
    }
    else
    {
        CDuiString sFail;
        sFail.Format(StringMgr.Get("toolbox.s_done_fail"), nExitCode);
        m_ctx.HostLog(sFail.GetData(), static_cast<int>(ELogLevel::LOG_ERROR));
        m_ctx.HostToast(StringMgr.Get("toolbox.s_done_title"),
                        sFail.GetData(), 1);
        m_ctx.HostLog(StringMgr.Get("toolbox.sa2efd66f"), static_cast<int>(ELogLevel::LOG_WARN));
    }
    m_ctx.HostSetCtrlText(L"sess_status", m_ctx.HostStatusReady().GetData());
    // HostProgress 等价原 SetSessionProgress（壳内实现即转发）
    m_ctx.HostProgress(0, 0);
}

void CPageTools::OnDismProgress(int nPct)

{
    CDuiString s;
    s.Format(StringMgr.Get("status.sddbfe464"), nPct);
    m_ctx.HostSetCtrlText(L"sess_status", s.GetData());
    m_ctx.HostProgress(nPct, kToolsProgressFullPct);
}

// ============================================================
// [R107] 工具箱统一进度条 UI
//
// 背景：ExternalToolService::ArchiveToolRun 在每次工具 bat 启动/结束时
// 写 %LOCALAPPDATA%\DismTool\Logs\progress.txt（tool / timestamp / status
// 三字段），但 UI 从未订阅。D2 缺口"统一进度条 UI"。
//
// 实现：Toolbox 主页顶部新增 tools_progress 条 + tools_progress_label
// 文本；工具启动（RunBatFileUi 成功后）开启 800ms 定时器轮询，
// 读到 status=started 时显示进度条，status=done 时停止轮询并保留
// 最后状态 3 秒（由调用方 Toast/日志提示）。纯文件轮询，无阻塞。
// ============================================================

void CPageTools::StartToolProgressPoll()
{
    if (m_bToolProgressOn) return;
    ::SetTimer(m_ctx.HostHwnd(), TOOL_PROGRESS_TIMER_ID, kToolsProgressPollMs, nullptr);
    m_bToolProgressOn = true;
}

void CPageTools::StopToolProgressPoll()
{
    if (!m_bToolProgressOn) return;
    ::KillTimer(m_ctx.HostHwnd(), TOOL_PROGRESS_TIMER_ID);
    m_bToolProgressOn = false;
    // 隐藏进度条并清理状态文本
    if (CControlUI* p = m_ctx.HostFindControl(L"tools_progress"))
        p->SetVisible(false);
    m_ctx.HostSetCtrlText(L"tools_progress_label", L"");
}

void CPageTools::OnToolProgressTimer()
{
    CDuiString sTool, sTs, sStatus;
    int nPct = 0;
    if (!ExternalToolService::ReadToolProgress(sTool, sTs, sStatus, nPct))
    {
        // progress.txt 不存在或字段不全：视为空闲，停止轮询
        StopToolProgressPoll();
        return;
    }
    bool bDone = (sStatus.CompareNoCase(L"done") == 0);
    // [R109] 完成时强制 100（bat 若未写 pct，则按 status=done 归一）
    if (bDone) nPct = kToolsProgressFullPct;
    if (nPct < 0) nPct = 0;
    if (nPct > kToolsProgressFullPct) nPct = kToolsProgressFullPct;
    // 显示进度条
    if (CControlUI* p = m_ctx.HostFindControl(L"tools_progress"))
    {
        p->SetVisible(true);
        // [R109] 真实百分比：SetValue 已内部 Invalidate
        CProgressUI* pProg = static_cast<CProgressUI*>(p);
        pProg->SetValue(nPct);
    }
    CDuiString sLabel;
    if (bDone)
    {
        sLabel.Format(StringMgr.Get("toolbox.s_progress_done"), sTool.GetData());
        m_ctx.HostSetCtrlText(L"tools_progress_label", sLabel.GetData());
        StopToolProgressPoll();
    }
    else
    {
        sLabel.Format(StringMgr.Get("toolbox.s_progress_running_pct"),
                      sTool.GetData(), nPct);
        m_ctx.HostSetCtrlText(L"tools_progress_label", sLabel.GetData());
    }
    // 统一日志（供诊断）
    CDuiString sLog;
    sLog.Format(L"[tools] progress: tool=%s status=%s pct=%d ts=%s",
                sTool.GetData(), sStatus.GetData(), nPct, sTs.GetData());
    m_ctx.HostLog(sLog.GetData(), static_cast<int>(ELogLevel::LOG_INFO));
}
