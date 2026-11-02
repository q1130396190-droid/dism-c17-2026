// ============================================================================
//  FrameWnd_PageMsg.cpp —— [R1] 页完成消息注册表 / [R5] 表驱动到页虚入口
//
//  HandleCustomMessage 中 20 条"后台线程 → 内容页"消息以 kPageMsgTab
//  单表收口：表项 {uMsg, iPage, eOwn}，分发时经
//  PageById(iPage)->CPageBase::OnAsyncMessage() 虚入口到达各页；壳层不再
//  出现任何具体 CPageXxx 类型与具名方法（Windows 功能子视图的消息由
//  CPageCapability 代理转发）。
//
//  iPage<0 表示无目标页的壳级消息（WM_DIAG_LOG/WM_WOF_LOG：全局输出面板
//  日志），壳 AppendLog 后按 EPageMsgOwn::kMsgOwnShell 释放。
//
//  lParam/wParam 堆所有权契约（EPageMsgOwn，逐消息核对投递方后固化，
//  禁止凭直觉改动——双删/漏删都是崩溃或泄漏）：
//    EPageMsgOwn::kMsgOwnPage  堆对象由各页 OnAsyncMessage 链路内部 delete，壳禁止再碰；
//                 例外：[R16][T1] 空页（PageById 为 nullptr）时 lParam 无人
//                 接收，由 DispatchPageMessage 就地经 ReleaseMsgParam 兜底
//                 释放——页存在时绝不走该分支，无双删。
//    EPageMsgOwn::kMsgOwnShell 调页后由 ReleaseMsgParam() 单一出口释放；
//    EPageMsgOwn::kMsgValue    纯值传递（int/枚举/布尔），无堆对象；
//    EPageMsgOwn::kMsgNone     无参数。
//
//  刻意不入表：WM_CLEAN_DONE——OnCleanDone 返回值决定 RestartExplorer()
//  必须在 OnCleanDoneTail() 解锁 UI 之前执行的两段时序编排，保留在 HCM，
//  经 CPageBase::HostOnCleanDone/HostOnCleanDoneTail 专用虚入口收口。
//
//  行为等价基线：FrameWnd.cpp 5784-6196 原始 21 分支（2026-10-13 迁移）。
// ============================================================================

#include "stdafx.h"

#include "FrameWnd.h"
#include "WmMessages.h"
#include "Page_Program.h"   // ProgEnumCtx 释放    // [R56-D] 原 FrameWnd_Messages.h 假依赖：本文件仅使用 WM_* 常量（34 个），EAccelCmd/EHostCmd/定时器 ID 等独有符号 0 使用；改 include 纯值头避免 UI 层定义耦合
#include "Page_Cleanup.h"   // [R-TAILFIX] TPendingCleanTail 空页兜底释放
#include "CleanJobService.h" // [R-CLEANPROG] CleanProgressMsg 载荷类型

// ---------------- 注册表（消息 → 页序 + 堆所有权）----------------

const CFrameWnd::SPageMsgEntry CFrameWnd::kPageMsgTab[] =
{
    { WM_DISM_LOG,         EPageId::PAGE_TOOLS,    EPageMsgOwn::kMsgOwnPage  },
    { WM_DISM_DONE,        EPageId::PAGE_TOOLS,    EPageMsgOwn::kMsgNone     },
    { WM_DISM_PROGRESS,    EPageId::PAGE_TOOLS,    EPageMsgOwn::kMsgValue    },
    { WM_APPX_DATA,        EPageId::PAGE_APPX,     EPageMsgOwn::kMsgOwnPage  },
    { WM_DRV_PUBDATA,      EPageId::PAGE_DRIVER,   EPageMsgOwn::kMsgOwnPage  },
    { WM_DRV_DELDATA,      EPageId::PAGE_DRIVER,   EPageMsgOwn::kMsgOwnPage  },
    { WM_CLEAN_DISMTAIL,   EPageId::PAGE_CLEANUP,  EPageMsgOwn::kMsgOwnPage  },
    { WM_APPX_DEPDATA,     EPageId::PAGE_APPX,     EPageMsgOwn::kMsgOwnPage  },
    { WM_DIAG_LOG,         EPageId::PAGE_NONE, EPageMsgOwn::kMsgOwnShell },
    { WM_PAGE_ALERT,       EPageId::PAGE_NONE, EPageMsgOwn::kMsgOwnShell },  // [FIX-T2] 后台作业完成弹窗
    { WM_WOF_LOG,          EPageId::PAGE_NONE, EPageMsgOwn::kMsgOwnShell },
    { WM_PROG_DATA,        EPageId::PAGE_PROGRAM,  EPageMsgOwn::kMsgOwnPage  },
    // [INTERACT-FIX] 批量卸载后台完成 → Program 页收口
    { WM_PROG_BATCHDONE,   EPageId::PAGE_PROGRAM,  EPageMsgOwn::kMsgOwnPage  },
    { WM_CAP_DATA,         EPageId::PAGE_FEATURE,  EPageMsgOwn::kMsgOwnPage  },  // 7=Capability 本页
    // [FIX-PERF] partial 原先未登记路由：页面有 case 但主窗口不收，导致分批失效且载荷泄漏。
    { WM_CAP_PARTIAL,      EPageId::PAGE_FEATURE,  EPageMsgOwn::kMsgOwnPage  },
    { WM_FEAT_PARTIAL,     EPageId::PAGE_FEATURE,  EPageMsgOwn::kMsgOwnPage  },
    { WM_FEAT_DATA,        EPageId::PAGE_FEATURE,  EPageMsgOwn::kMsgOwnPage  },  // Capability 代理 Feature
    { WM_FEAT_APPLYDONE,   EPageId::PAGE_FEATURE,  EPageMsgOwn::kMsgOwnPage  },  // Capability 代理 Feature
    { WM_TASK_DATA,        EPageId::PAGE_TASKS,    EPageMsgOwn::kMsgOwnPage  },
    { WM_UPD_DATA,         EPageId::PAGE_UPDATE,   EPageMsgOwn::kMsgOwnPage  },
    // [FIX-DS-1] WM_UPD_PARTIAL 分批回报原先未注册路由：消息抵达壳后无人分发、
    // 载荷 UpdPartialMsg* 无人释放 —— 每满 100 条必泄漏一批、尾批 FlushUpdPartial
    // 必泄漏，分批渲染整条链路沦为死代码。补登记后与 WM_UPD_DATA 同走 PAGE_UPDATE。
    { WM_UPD_PARTIAL,      EPageId::PAGE_UPDATE,   EPageMsgOwn::kMsgOwnPage  },
    // [FIX-P2N] 更新阶段后台完成：后台线程 RunHiddenExit 结束后经此消息
    // 移交 UpdPhaseDoneMsg*，页面 UpdReportPhaseResult 在 UI 收口。
    { WM_UPD_PHASEDONE,    EPageId::PAGE_UPDATE,   EPageMsgOwn::kMsgOwnPage  },
    { WM_UPD_PROGRESS,     EPageId::PAGE_UPDATE,   EPageMsgOwn::kMsgOwnShell },
    { WM_REGCLEN_PROGRESS, EPageId::PAGE_REGCLEAN, EPageMsgOwn::kMsgOwnShell },
    { WM_REGCLEN_DONE,     EPageId::PAGE_REGCLEAN, EPageMsgOwn::kMsgOwnShell },
    { WM_REGCLEN_SCANDONE, EPageId::PAGE_REGCLEAN, EPageMsgOwn::kMsgOwnShell },
    { WM_SCAN_ITEM,        EPageId::PAGE_CLEANUP,  EPageMsgOwn::kMsgOwnPage  },
    { WM_SCAN_DONE,        EPageId::PAGE_CLEANUP,  EPageMsgOwn::kMsgValue    },
    // [R-CLEANPROG] 删除阶段两进度回报
    { WM_CLEAN_PROGRESS,   EPageId::PAGE_CLEANUP,  EPageMsgOwn::kMsgOwnPage  },
    // WM_CLEAN_DONE 故意缺席（见文件头说明）。
};

bool CFrameWnd::DispatchPageMessage(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    for (const SPageMsgEntry& ent : kPageMsgTab)
    {
        if (ent.uMsg != uMsg) continue;

        if (static_cast<int>(ent.iPage) >= 0)
        {
            // [R5] 经页表 + CPageBase::OnAsyncMessage 虚入口分发，
            // 壳不接触任何具体 CPageXxx 类型。
            // [FIX-P2G] 消费页内未接的消息：页存在但 OnAsyncMessage 返回
            // false（switch 未命中）且所有权为 kMsgOwnPage 时，载荷无人
            // 释放 —— 此前仅空页兜底，机制缺口补齐为"页缺失/页未消费"
            // 两条路径互斥释放（页已消费时绝不进入，无双删）。
            bool bConsumed = false;
            if (CPageBase* pPage = PageById(ent.iPage))
            {
                bConsumed = pPage->OnAsyncMessage(uMsg, wParam, lParam);
                if (!bConsumed && ent.eOwn == EPageMsgOwn::kMsgOwnPage)
                    ReleaseMsgParam(uMsg, wParam, lParam);
            }
            else if (ent.eOwn == EPageMsgOwn::kMsgOwnPage)
            {
                // [R16][T1] 空页兜底：页缺失时 EPageMsgOwn::kMsgOwnPage 的 lParam 堆对象
                // 无人接收，就地按消息类型释放（页存在时绝不走此分支，无双删）。
                ReleaseMsgParam(uMsg, wParam, lParam);
            }
        }
        else
        {
            // 无目标页的壳级消息：诊断/WOF 线程日志进全局输出面板。
            // [FIX-T2] WM_PAGE_ALERT：后台作业完成弹窗，载荷
            // "标题|文本|图标" —— 工作线程禁止直接 HostAlert（UI 弹窗），
            // 统一回投壳拆分执行。HostAlert 内部断言 UI 线程，此处安全。
            if (uMsg == WM_PAGE_ALERT)
            {
                CDuiString* pAlert = (CDuiString*)lParam;
                if (pAlert)
                {
                    CDuiString s = pAlert->GetData();
                    int p1 = s.Find(L'|');
                    int p2 = (p1 >= 0) ? s.Find(L'|', p1 + 1) : -1;
                    if (p1 >= 0 && p2 > p1)
                    {
                        CDuiString sTitle = s.Left(p1);
                        CDuiString sText  = s.Mid(p1 + 1, p2 - p1 - 1);
                        UINT uIcon = MB_ICONINFORMATION;
                        const int nIcon = _ttoi(s.Mid(p2 + 1).GetData());
                        if (nIcon == 2) uIcon = MB_ICONWARNING;
                        else if (nIcon == 3) uIcon = MB_ICONERROR;
                        HostAlert(sTitle.GetData(), sText.GetData(), uIcon);
                    }
                }
            }
            else
            {
                CDuiString* pLog = (CDuiString*)lParam;
                if (pLog)
                    AppendLog(pLog->GetData());
            }
        }

        // EPageMsgOwn::kMsgOwnShell：调页（或壳日志）后由本出口统一释放堆参数；
        // EPageMsgOwn::kMsgOwnPage（页已释放）/EPageMsgOwn::kMsgValue/EPageMsgOwn::kMsgNone 在此什么都不做。
        if (ent.eOwn == EPageMsgOwn::kMsgOwnShell)
            ReleaseMsgParam(uMsg, wParam, lParam);

        return true;
    }
    return false;
}

// ---------------- 壳侧堆参数唯一释放出口 ----------------
//
// 处理两类路径（页存在时的 EPageMsgOwn::kMsgOwnPage 释放点在页内，走到 default 必须
// 什么都不做，否则双重释放）：
//   * EPageMsgOwn::kMsgOwnShell：调页/壳日志后统一出口释放，各 case 释放条件与原 HCM
//     逐行等价；
//   * EPageMsgOwn::kMsgOwnPage 空页兜底（[R16][T1]）：仅当 PageById 为 nullptr 时被调用，
//     载荷类型逐条对照各页 OnAsyncMessage 所有权注释固化。
//   * [R16][T3] REGCLEN_PROGRESS 投递方成对 new 两个 int（Post 失败自删，
//     RegCleanJobService.cpp），接收方无条件成对 delete（delete nullptr 合法）。

void CFrameWnd::ReleaseMsgParam(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_DIAG_LOG:
    case WM_PAGE_ALERT:
    case WM_WOF_LOG:
        // [C++17 阶段3] 消息接收端 delete，所有权由投递方移交
        delete (CDuiString*)lParam;
        break;

    case WM_UPD_PROGRESS:
        // [C++17 阶段3] 消息接收端 delete，所有权由投递方移交
        delete (UpdProgressMsg*)lParam;
        break;

    case WM_REGCLEN_PROGRESS:
        // [R16][T3] 无条件成对释放（原"双非空才删"在单侧投递时泄漏）。
        // [C++17 阶段3] 消息接收端 delete，所有权由投递方移交
        delete (int*)wParam;
        delete (int*)lParam;
        break;

    case WM_REGCLEN_DONE:
        // [C++17 阶段3] 消息接收端 delete，所有权由投递方移交
        delete (RegCleanDoneMsg*)lParam;
        break;

    case WM_REGCLEN_SCANDONE:
        // [C++17 阶段3] 消息接收端 delete，所有权由投递方移交
        delete (RegCleanScanDoneMsg*)lParam;
        break;

    // ---- [R16][T1] EPageMsgOwn::kMsgOwnPage 空页兜底：仅页缺失路径可达，类型对照各页 ----
    case WM_DISM_LOG:
        // [C++17 阶段3] 消息接收端 delete，所有权由投递方移交
        delete (CDuiString*)lParam;
        break;
    case WM_APPX_DATA:
        // [C++17 阶段3] 消息接收端 delete，所有权由投递方移交
        delete (AppxDataMsg*)lParam;
        break;
    case WM_APPX_DEPDATA:
        // [C++17 阶段3] 消息接收端 delete，所有权由投递方移交
        delete (AppxDepsMsg*)lParam;
        break;
    case WM_DRV_PUBDATA:
        // [C++17 阶段3] 消息接收端 delete，所有权由投递方移交
        delete (TDrvPubMsg*)lParam;
        break;
    case WM_PROG_DATA:
        delete (ProgEnumCtx*)lParam;
        break;
    case WM_PROG_BATCHDONE:
        // [INTERACT-FIX] 空页兜底：批量卸载完成消息载荷
        delete (ProgBatchMsg*)lParam;
        break;
    case WM_CAP_DATA:
        // [C++17 阶段3] 消息接收端 delete，所有权由投递方移交
        delete (CapDataMsg*)lParam;
        break;
    case WM_CAP_PARTIAL:
        delete (CapPartialMsg*)lParam;
        break;
    case WM_FEAT_PARTIAL:
        delete (FeatPartialMsg*)lParam;
        break;
    case WM_FEAT_DATA:
        // [C++17 阶段3] 消息接收端 delete，所有权由投递方移交
        delete (FeatDataMsg*)lParam;
        break;
    case WM_FEAT_APPLYDONE:
        // [C++17 阶段3] 消息接收端 delete，所有权由投递方移交
        delete (FeatApplyMsg*)lParam;
        break;
    case WM_TASK_DATA:
        // [C++17 阶段3] 消息接收端 delete，所有权由投递方移交
        delete (CPageTasks::TaskQueryMsg*)lParam;
        break;
    case WM_UPD_DATA:
        // [C++17 阶段3] 消息接收端 delete，所有权由投递方移交
        delete (UpdDataMsg*)lParam;
        break;
    case WM_UPD_PARTIAL:
        // [FIX-DS-1] 空页兜底：分批回报载荷无人接收时释放（与 kPageMsgTab 登记配套）
        delete (UpdPartialMsg*)lParam;
        break;
    case WM_UPD_PHASEDONE:
        // [FIX-P2N] 空页兜底：阶段完成载荷无人接收时释放
        delete (UpdPhaseDoneMsg*)lParam;
        break;
    case WM_SCAN_ITEM:
        // [C++17 阶段3] 消息接收端 delete，所有权由投递方移交
        delete (ScanItemMsg*)lParam;
        break;
    case WM_CLEAN_PROGRESS:
        // [R-CLEANPROG] 空页兜底：进度载荷无人接收时释放
        delete (CleanProgressMsg*)lParam;
        break;
    case WM_CLEAN_DISMTAIL:
        // [AUDIT-P0][R-TAILFIX] 空页兜底：尾巴载荷无人接收时释放。
        // 漏收时还须复位清理态并解锁 UI，否则导航永久禁用（见 HCM 分支）。
        delete (TPendingCleanTail*)lParam;
        break;

    default:
        // EPageMsgOwn::kMsgOwnPage（页已释放）/ EPageMsgOwn::kMsgValue / EPageMsgOwn::kMsgNone：无壳侧释放动作。
        break;
    }
}
// [C++17 R3] 文件级内联常量（C++17 inline variables，ODR 安全）：
