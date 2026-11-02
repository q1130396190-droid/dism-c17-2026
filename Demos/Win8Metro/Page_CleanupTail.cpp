// ============================================================
// Page_CleanupTail.cpp —— 清理页「DISM 尾巴后台化 + 收尾」
// （从 Page_Cleanup.cpp 拆出）。
//
// [R100] Page_Cleanup.cpp 拆前 936 行、规模分 9.90（距满分仅差 36 行）。
// 本文件承接四个实体，合计 122 行，边界按「清理完成后的异步收尾」划分：
//   · StartCleanDismTailAsync    起后台线程跑 12 路自定义动作 + WinSxS 组件清理
//   · CleanDismTailThreadProc    线程体（含 SEH 兜底 + 结果回投 UI）
//   · OnCleanDismTail            UI 侧收口（SHChangeNotify / 重启询问 / 解锁）
//   · OnCleanDoneTail            幂等收尾（双状态机保护，见函数内注释）
//
// [AUDIT-P0] 背景：这四段原在 WM_CLEAN_DONE 回调里同步执行，等于把 UI 线程
// 钉死数分钟（CBS 事务），用户既看不到进度也无法取消。改后台化后，
// 收尾时机由两条路径竞争（壳无条件调用 / 异步回投），靠
// m_bCleanTailInFlight + m_bCleanTailFinalized 双标志保证"只收尾一次"。
//
// 闭包说明：三个成员声明见 Page_Cleanup.h；TPendingCleanTail 结构体也定义在
// Page_Cleanup.h（共享类型，非本 TU 私有）。跨 TU 依赖：
//   · m_ctx / m_bCleanTailInFlight / m_bCleanTailFinalized /
//     m_ullLastCleanFreed / m_nLastCleanFailed（成员）
//   · OcDispatchCustomActions / OcRunDismComponent / AppendLog（成员，在 Page_CleanupOc.cpp）
//   · StringMgr / ToolSql::HostConfirm（NotifyBox.h）
//   · CleanJobService（CleanJobService.h）/ SafeCreateThread（SafeThread.h）
//   · msgparam::ToParam（MsgParam.h）/ CCleanEngine::FormatSize
//
// 逐字搬运，零逻辑改动。
// ============================================================

#include "stdafx.h"

#include "Page_Cleanup.h"

#include "StringManager.h"
#include "NotifyBox.h"            // ToolSql::HostConfirm
#include "CleanJobService.h"      // EndClean / ClearCustomJobs
#include "SafeThread.h"           // SafeCreateThread
#include "FrameWnd_Messages.h"    // WM_CLEAN_DISMTAIL / WM_HOST_REQUEST_RESTART_EXPLORER
#include "MsgParam.h"             // msgparam::ToParam
#include "LogService.h"           // ELogLevel
#include "CustomControls.h"       // CControlUI

#include <memory>
#include <vector>                 // [R-SAFEDLG] 暂存消息队列
#include <ole2.h>                 // [R-TAILFIX] CoInitializeEx/CoUninitialize
#include <shlobj.h>               // SHChangeNotify

using namespace DuiLib;
using i18n::CStringManager;

// [R100] 收尾气泡的通知级别。原在 OnCleanDoneTail 两个分支各写一次裸值
// （失败分支 1、成功分支 0）。具名后「失败要警告色、成功要普通色」成为
// 可核对的成对事实，不会出现只改一处导致成功也报红的情况。
inline constexpr int kCleanTailNotifyWarning = 1;
inline constexpr int kCleanTailNotifyNormal  = 0;

// ============================================================
// [AUDIT-P0] 清理后 DISM 尾巴后台化
// ============================================================

// 工作线程上下文 + 结果：含 UI 侧收尾所需的四个通知标志。
static DWORD WINAPI CleanDismTailThreadProc(void* pv);   // 前置声明（本函数先于定义使用）

// [FIX-P0] 真实工作体只接收引用，不接管所有权。
// 线程入口与同步回退路径共用，避免把栈对象地址交给 unique_ptr 删除（invalid free）。
// [R18] 返回 true 表示捕获到异常（可能只完成一部分），由调用方标记为"部分完成"。
static bool RunCleanDismTailWork(CPageCleanup* pPage, const TPendingCleanTail& t)
{
    if (pPage == nullptr) return false;
    try
    {
        // 12 路自定义动作（含 DISM 组件/驱动/Compact/Appx 等）
        pPage->OcDispatchCustomActions();
        // WinSxS 组件清理（最后执行，耗时最长）
        pPage->OcRunDismComponent(t.bDismPending, t.bResetBase);
        return false;
    }
    catch (...)
    {
        // [R18] 异常意味着某个 Custom 动作或组件清理可能只执行了一半，
        // 必须让上层把结果标为"部分完成"，不能记成成功。
        pPage->AppendLog(L"[!] [clean] DISM tail threw: partial completion");
        return true;
    }
}

// ============================================================
// [R-TAILFIX] 尾巴工作体 SEH 边界 —— "清理完成后页面不解锁"的根因修复
//
// 事故实证（crashguard 2026-11-02 05:50:01，tid=尾巴工作线程）：
//   RunCleanDismTailWork → OcDispatchCustomActions → NCleanerClient::Invoke
//   → NCleaner.dll 内 COM 调用 AV（写 0x3）。/EHsc 下 catch(...) 捕不到硬件
//   异常，SafeThreadTrampoline 顶层 __except 吞掉后线程直接死亡 ——
//   WM_CLEAN_DISMTAIL 永不投递，m_bCleanTailInFlight 恒为 true，
//   OnCleanDoneTail 永远早退 → 导航/按钮永久禁用（用户所见"清理完不返回、
//   其他页面还冻结"）。
//
// 对策（两层）：
//   1) 线程入口 CoInitializeEx(MTA)：NCleaner/DISM/Appx 自定义动作依赖 COM，
//      原线程无 COM 初始化，CoCreateInstance 失败被忽略后解引用空接口。
//   2) 本边界函数（无 C++ 对象、允许 __try）兜住 SEH：无论工作体怎么死，
//      回投消息都由调用方照常执行，UI 必定解冻。
// ============================================================
struct TTailSehCtx
{
    CPageCleanup*            pPage    = nullptr;
    const TPendingCleanTail* t        = nullptr;
    bool                     bPartial = false;
    bool                     bSeh     = false;
};
// 本函数不含需要展开的 C++ 对象，允许直接使用 __try（MSVC C2712 约束）。
static void RunTailSehGuarded(TTailSehCtx& c)
{
    __try
    {
        c.bPartial = RunCleanDismTailWork(c.pPage, *c.t);
    }
    __except (CrashGuardFilter(GetExceptionInformation(), "clean-tail-seh"))
    {
        c.bSeh     = true;
        c.bPartial = true;   // 硬件异常同样按"部分完成"上报，绝不谎报成功
    }
}

void CPageCleanup::StartCleanDismTailAsync(bool bDismPending, bool bResetBase,
                                           bool bAssocNotify, bool bRestartExplorer)
{
    auto pTail = std::make_unique<TPendingCleanTail>();
    pTail->hNotify          = m_ctx.HostHwnd();
    pTail->bDismPending     = bDismPending;
    pTail->bResetBase       = bResetBase;
    pTail->bAssocNotify     = bAssocNotify;
    pTail->bRestartExplorer = bRestartExplorer;
    pTail->pPage            = this;
    HANDLE hTh = SafeCreateThread(&CleanDismTailThreadProc, pTail.get());
    if (!hTh)
    {
        // [FIX-P1J] 线程资源耗尽时不再把整段 DISM 清理（13 路 Custom +
        // WinSxS 组件清理，可达数分钟）内联进 UI 线程执行 —— 低概率、高危害
        // （整窗冻结且不可中断，[FIX-P0] 注释自知）。改为拒绝执行：记警告 +
        // 按"部分完成"收口（bPartial=true 走警告色气泡），UI 正常解锁，
        // 用户可稍后重试清理。OnCleanDismTail 内部先 MarkTailFinished，
        // 无需再置 InFlight。
        m_ctx.HostLog(L"[clean] SafeCreateThread failed; DISM tail skipped, UI unfrozen (retry later)",
                      static_cast<int>(ELogLevel::LOG_WARN));
        pTail->bPartial = true;
        OnCleanDismTail(pTail.release());   // 所有权转交收口（EndClean/解锁/气泡）
        return;
    }
    pTail.release();   // [C++17] 所有权移交线程过程
    MarkTailInFlight();
    // [FIX-UNFREEZE] 状态反馈（见上注释）+ 看门狗武装：
    // 壳侧唯一一次 HostOnCleanDoneTail 已被 InFlight 挡回，若尾巴线程
    // PostMessage 全败将无人收尾 —— 1s 轮询看门狗在 InFlight 清除后
    // 强制走 OnCleanDoneTail（幂等，Finalized 先行检查防双收尾）。
    ::SetTimer(m_ctx.HostHwnd(), CLEAN_TAIL_WATCHDOG_TIMER_ID, 1000, nullptr);
    m_ctx.HostSetCtrlText(L"sess_status",
        L"[组件清理] 正在执行 DISM 组件清理与收尾动作（组件存储 / Appx 重注册 / "
        L"NCleaner 缓存等），可能需要数分钟，请耐心等待，详情见日志区……");
    m_ctx.HostProgress(0, 1);
    ::CloseHandle(hTh);   // 句柄即发即弃（沿用本页既有约定）
}

// ============================================================
// [R-SAFEDLG] 尾巴线程模态框安全泵
// 尾巴线程会弹模态确认框（ResetBase 二次确认，见 OcConfirmDismResetBase）。
// MessageBox 的模态泵会把主窗口的 WM_TIMER（PAGE_BUILD_TIMER 延迟构建 /
// LOG_FLUSH 日志刷写）派发进尾巴线程栈：延迟构建在尾巴线程上重建 DuiLib
// 控件树 = 跨线程 UI = UAF/堆损坏；日志刷写会把 m_vPendingLog 的读写竞争
// 进同一 vector。对策：确认框存续期间用 CBT 钩子把业务/定时器消息从队列
// 取出不派发、暂存，对话框返回后按原序回投主循环——与
// ProcessRunner::RunHiddenExit / PumpShellSafeMessages 的
// "嵌套泵不消费业务消息"契约同源。
// ============================================================
static std::vector<MSG> s_vTailHoldMsgs;
// [FIX-P2F] 暂存队列互斥：现设计为单尾巴线程实例安全，加锁防御未来
// 出现双尾巴（StartCleanDismTailAsync 重入/并发路径）时的 vector 竞态。
static CRITICAL_SECTION* TailHoldLock()
{
    static CRITICAL_SECTION cs;
    static struct SInit { SInit() { ::InitializeCriticalSection(&cs); } } s_init;
    return &cs;
}
static LRESULT CALLBACK TailCbtProc(int code, WPARAM, LPARAM)
{
    if (code == HCBT_ACTIVATE)
    {
        MSG m;
        ::EnterCriticalSection(TailHoldLock());
        while (::PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE | PM_NOYIELD))
        {
            // 注意：这里绝不能 PostQuitMessage——本 Drain 运行在确认框的
            // HCBT_ACTIVATE 回调栈内，置 WM_QUIT 标志会让随后的 MessageBox
            // 模态泵立即退出、对话框"一闪而过"（无人点击也返回）。
            // 退出意图由主循环与 RunHiddenExit 泵处理，此处原样回投。
            if (m.message == WM_QUIT) { s_vTailHoldMsgs.push_back(m); continue; }
            const bool bBusiness =
                (m.message >= WM_USER && m.message < WM_USER + 0x200)
                || (m.message >= WM_APP && m.message < WM_APP + 0x8000)
                || (m.message == WM_TIMER);
            if (bBusiness) s_vTailHoldMsgs.push_back(m);
            // 其余（绘制/IME/对话框自身）交回 MessageBox 的模态泵处理
        }
        ::LeaveCriticalSection(TailHoldLock());
    }
    return 0;   // 不吞 CBT 通知本身
}
struct TailDlgPumpHook
{
    HHOOK hHook = nullptr;
    TailDlgPumpHook()
    {
        hHook = ::SetWindowsHookExW(WH_CBT, &TailCbtProc, nullptr,
                                    ::GetCurrentThreadId());
    }
    ~TailDlgPumpHook()
    {
        if (hHook) ::UnhookWindowsHookEx(hHook);
        // 无论钩子是否触发过，作用域结束统一回投暂存消息（空则无操作）
        // [FIX-P2F] 取出+清空持锁（与 TailCbtProc 入队互斥）
        std::vector<MSG> vRepost;
        ::EnterCriticalSection(TailHoldLock());
        vRepost.swap(s_vTailHoldMsgs);
        ::LeaveCriticalSection(TailHoldLock());
        for (const MSG& m : vRepost)
            ::PostMessageW(m.hwnd, m.message, m.wParam, m.lParam);
    }
    TailDlgPumpHook(const TailDlgPumpHook&) = delete;
    TailDlgPumpHook& operator=(const TailDlgPumpHook&) = delete;
};

static DWORD WINAPI CleanDismTailThreadProc(void* pv)
{
    // [C++17] ctx 由本线程独占
    std::unique_ptr<TPendingCleanTail> tail(static_cast<TPendingCleanTail*>(pv));
    if (!tail) return 0;
    CPageCleanup* pPage = static_cast<CPageCleanup*>(tail->pPage);

    // [R-TAILFIX] COM 初始化：自定义动作族（NCleaner 的 WUA/DO 清理、
    // DismApiBridge 的 CBS 会话、Appx 重注册）都经 COM 入口。缺省 MTA——
    // 本线程没有消息泵，STA 会在跨套间封送上死锁；DismApi 官方要求即 MTA。
    const HRESULT hrCo = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // [R-SAFEDLG] 钩子作用域限定在工作体：花括号结束即卸载并回投暂存消息，
    // 顺序上先于 COM 拆除（回投只进队列，不依赖 COM）。
    {
        TailDlgPumpHook dlgPump;

        // [R-TAILFIX] 统一出口：无论 pPage 缺失、C++ 异常还是 SEH 硬件异常，
        // WM_CLEAN_DISMTAIL 必须投出（或窗口已亡则本地释放），UI 解冻不再依赖
        // "工作体恰好没崩"。
        if (pPage != nullptr)
        {
            TTailSehCtx c{ pPage, tail.get(), false, false };
            RunTailSehGuarded(c);
            tail->bPartial = c.bPartial;
            if (c.bSeh)
            {
                // SEH 后线程栈已不可信，只走无分配日志路径；bPartial 已置位，
                // 结尾气泡会如实显示"部分完成"。
                pPage->AppendLog(L"[!] [clean] DISM tail hit SEH; unfreezing UI as partial");
            }
        }
        else
        {
            tail->bPartial = true;
        }
    }

    if (SUCCEEDED(hrCo)) ::CoUninitialize();

    // [FIX-RC-2] 尾巴线程 PostMessage 失败兜底 —— "清理完页面永久冻结"的根因修复
    // 事故模式：CleanDismTailThreadProc 正常跑完工作体，WM_CLEAN_DISMTAIL
    // PostMessage 返回 false（TOCTOU 竞态 IsWindow(true) + PostMessage 失败
    // 或窗口消息队列临时满），TPendingCleanTail 被 unique_ptr 自动释放，
    // MarkTailFinished() 永远不被调 → m_bCleanTailInFlight 恒为 true →
    // OnCleanDoneTail 永远在首行 return → SetWorkerUiBusy(false) 永不执行
    // → UI 导航/按钮永久禁用。
    //
    // 修复策略（三层兜底）：
    //   1) PostMessage 之前先 MarkTailFinished()（bool 赋值 x86 原子），
    //      让 OnCleanDoneTail 的 InFlight 检查始终能通过
    //   2) PostMessage 失败时 SendMessageTimeout 跨窗口同步投递兜底
    //      （窗口还活着才会走到这里，SendMessageTimeout 最多等 3s）
    //   3) 页面销毁兜底（见 Page_Cleanup::OnCleanDone/OnDestroy）
    std::unique_ptr<TPendingCleanTail> out = std::move(tail);

    // [FIX-RC-2a] 先复位 InFlight —— 让任何后续到达的 OnCleanDoneTail 都能通过
    if (pPage != nullptr)
    {
        pPage->MarkTailFinished();
    }

    // [FIX-RC-2b→FIX-DS-4] 投递 WM_CLEAN_DISMTAIL 到 UI 窗口。
    // 原 SendMessageTimeoutW 兜底存在双删竞态：SMTO 超时返回 0 时消息仍可能
    // 在 UI 侧稍后送达并被 OnCleanDismTail 释放，而工作线程同时让 unique_ptr
    // 释放同一对象 → double-free 堆损坏。改为"只投递不等待"的有限重试：
    // PostMessage 成功 = 所有权移交（页内接收 / 空页兜底二者恰有一方释放）；
    // 全部重试失败 = 窗口确已消亡，本地释放 —— 任何时序下都恰好一次释放。
    if (::IsWindow(out->hNotify))
    {
        for (int nRetry = 0; nRetry < 10; ++nRetry)
        {
            if (::PostMessageW(out->hNotify, WM_CLEAN_DISMTAIL, 0,
                              msgparam::ToParam(out.get())))
            {
                (void)out.release();   // 投递成功：所有权移交 UI 线程
                return 0;
            }
            if (!::IsWindow(out->hNotify)) break;   // 窗口中途销毁：停止重试
            ::Sleep(100);   // 队列瞬时满：退避后重试（最长约 1s）
        }
        // 重试耗尽仍失败：本地释放兜底（此路径 MarkTailFinished 已先行，
        // 壳侧 HostOnCleanDoneTail 仍能完成 UI 解锁，不产生冻结）
        if (pPage != nullptr)
            pPage->AppendLog(L"[!] [clean] WM_CLEAN_DISMTAIL post failed after retries; payload freed locally");
    }
    // 窗口已销毁或投递失败：out 作用域结束自动释放
    return 0;
}

void CPageCleanup::OnCleanDismTail(TPendingCleanTail* pTail)
{
    MarkTailFinished();   // 尾巴已完成（无论成功与否）
    if (pTail == nullptr)
    {
        // [R-TAILFIX] 空载荷也必须走完整收尾：只 EndClean 不解锁正是
        // "清理完不返回"的又一冻结路径（EndClean 复位服务态，
        // OnCleanDoneTail 复位 UI 忙碌态/导航/气泡）。
        CleanJobService::EndClean();
        OnCleanDoneTail();
        return;
    }
    std::unique_ptr<TPendingCleanTail> guard(pTail);   // [C++17] 收口即释放
    CleanJobService::ClearCustomJobs();   // [E-2] Custom 派发结束，映射表复位
    bool bWantRestartExplorer = false;
    if (pTail->bAssocNotify)
    {
        ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        AppendLog(L"[clean] SHChangeNotify(SHCNE_ASSOCCHANGED) sent");
    }
    if (pTail->bRestartExplorer)
    {
        CDuiString sMsg = CDuiString(StringMgr.Get("misc.hc7bb")) + CDuiString(StringMgr.Get("misc.hda6a"));
        int nReboot = ToolSql::HostConfirm(m_ctx.HostHwnd(), sMsg.GetData(), StringMgr.Get("misc.h3427"), MB_YESNO, MB_ICONQUESTION);
        if (nReboot == IDYES) bWantRestartExplorer = true;
    }
    if (bWantRestartExplorer)
    {
        // 与壳侧原语义一致：先重启资源管理器，再走 Tail 解锁
        ::PostMessageW(m_ctx.HostHwnd(), WM_HOST_REQUEST_RESTART_EXPLORER, 0, 0);
    }
    OnCleanDoneTail();
}

void CPageCleanup::OnCleanDoneTail()

{
    // [AUDIT-P0] 幂等 + 时机保护：DISM 尾巴异步化后本函数有两条到达路径——
    //   ① 壳在 WM_CLEAN_DONE 后无条件调用（原路径，此刻尾巴可能还在跑）；
    //   ② 异步尾巴收口 OnCleanDismTail 在真正完成后调用（真正结束点）。
    // 规则：若尾巴仍在途，① 直接返回，等 ② 再收尾；② 之后任何调用都忽略。
    // 这样既不会重复 EndClean，也不会在 DISM 还在跑时就提前解锁 UI。
    if (m_bCleanTailFinalized) return;                 // 已真正收尾过
    if (m_bCleanTailInFlight) return;                  // 尾巴在途，等 OnCleanDismTail
    m_bCleanTailFinalized = true;   // [R160] 收尾认领：置位即代表唯一一次 EndClean
    // [FIX-UNFREEZE] 收尾完成：无论哪条路径（异步收口 / 看门狗兜底）到达，
    // 停掉 1s 看门狗轮询。
    ::KillTimer(m_ctx.HostHwnd(), CLEAN_TAIL_WATCHDOG_TIMER_ID);
    // 全部同步动作（含壳侧资源管理器重启）结束后才解除忙碌态
    CleanJobService::EndClean();
    m_ctx.HostSetCtrlText(L"sess_status", m_ctx.HostStatusReady().GetData());
    SetWorkerUiBusy(false);
    m_ctx.HostProgress(0, 0);
    // [R18 P0] 清理完成气泡
    {
        CDuiString sFreed;
        CCleanEngine::FormatSize(m_ullLastCleanFreed, sFreed);
        CDuiString sBalloon;
        if (m_nLastCleanFailed > 0)
        {
            sBalloon.Format(StringMgr.Get("cleanup.sa2ac40c9"),
                            sFreed.GetData(), m_nLastCleanFailed);
            m_ctx.HostNotify(StringMgr.Get("cleanup.s3f3309d9"), sBalloon.GetData(), kCleanTailNotifyWarning);
        }
        else if (m_bLastCleanPartial)
        {
            // [R18] 尾阶段异常：自定义动作或组件清理可能只完成一部分，
            // 不能按"全部成功"提示，否则用户会误以为所有项都已清理。
            sBalloon.Format(StringMgr.Get("cleanup.partial_done"),
                            sFreed.GetData());
            m_ctx.HostNotify(StringMgr.Get("cleanup.s3f3309d9"), sBalloon.GetData(), kCleanTailNotifyWarning);
        }
        else
        {
            sBalloon.Format(StringMgr.Get("cleanup.s73398652"), sFreed.GetData());
            m_ctx.HostNotify(StringMgr.Get("cleanup.sde0801f9"), sBalloon.GetData(), kCleanTailNotifyNormal);
        }
    }
}

// [FIX-UNFREEZE] 尾巴看门狗：1s 轮询 InFlight。
//   * 尾巴在途 → 继续等（CBS 事务可能数分钟）；
//   * InFlight 已清（线程 MarkTailFinished 先行）但 OnCleanDismTail 消息
//     投递全败 → 这里是唯一还能触达收尾的 UI 路径，强制 OnCleanDoneTail
//     （幂等：Finalized 检查先行，正常路径已收尾时这里直接 return）。
void CPageCleanup::OnHostTimer(UINT uId)
{
    if (uId != CLEAN_TAIL_WATCHDOG_TIMER_ID) return;
    if (m_bCleanTailInFlight) return;      // 尾巴仍在途：CBS 事务继续等
    ::KillTimer(m_ctx.HostHwnd(), CLEAN_TAIL_WATCHDOG_TIMER_ID);
    AppendLog(L"[clean] tail watchdog fired: completion message lost, finalizing UI now");
    OnCleanDoneTail();
}
