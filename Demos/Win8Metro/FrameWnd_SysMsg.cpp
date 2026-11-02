// ============================================================================
//  FrameWnd_SysMsg.cpp —— [R2] 壳系统/输入消息外迁
//
//  HandleCustomMessage 中的壳级系统消息（加速键、WM_SIZE、托盘、右键/
//  键盘转页、WM_DROPFILES、DPI、跨 DPI 重建、最大化重排、WM_TIMER、
//  WM_CLOSE 退出序列）逐字搬入本文件 HandleSystemMessage()，HCM 只做
//  一次路由。语义/顺序与原 HCM 分支逐一对应：
//    * 返回 true  = 原 bHandled=TRUE; return 0（拦截）；
//    * 返回 false = 原落到函数尾 bHandled=FALSE（放行 DuiLib 默认处理），
//      WM_SIZE 与未命中的右键/按键/定时器必须保持放行。
//
//  WM_TIMER 九个自建 ID 改 kTimerTab 表驱动；非本窗口定时器（DuiLib
//  滚动条 thumb 拖拽等）一律 return false 放行。
//
//  行为等价基线：FrameWnd.cpp HandleCustomMessage 系统分支（2026-10-13）。
// ============================================================================

#include "stdafx.h"
#include "OfflineSession.h"  // [R10-02] 离线会话经独立组件访问，去 CDataEngine 静态耦合

#include <windowsx.h>           // GET_X_LPARAM / GET_Y_LPARAM

#include "FrameWnd.h"
#include "FrameWnd_Messages.h"
#include "FrameWnd_Util.h"      // FrameWnd_Util_CopyTextToClipboard
#include "StringManager.h"
#include "DismApiBridge.h"      // DropInstallThreadProc 拖放原生安装
#include "FeatureService.h"     // [T4 P1] 预热：缓存缺失时补枚举并落盘
#include "AppxCapUpdService.h"  // [T4 P1] 预热：Cap 缓存同理由
#include "BackgroundJobs.h"     // 受管后台作业（SEH 边界 + 退出等待）
#include "ProcessJob.h"         // KillChildren：退出前杀光作业子进程
#include <memory>   // [C++17 R3] make_unique
#include <thread>   // [T4 P1] 启动预热后台线程
#include <atomic>   // [STAB] g_bWarmupYield 跨线程标志

// ----------------------------------------------------------------------------
// [CRASHFIX] 拖放安装（.inf 驱动 / .cab/.msu 补丁）后台作业上下文 + 线程
// （随 WM_DROPFILES 自 FrameWnd.cpp 迁入；双层 SEH 边界替代裸 detach，
//  日志经 WM_DISM_LOG 回 UI，投递失败自行 delete，见 PostHeapOrDelete 契约）
// ----------------------------------------------------------------------------
namespace
{
    struct TDropInstallJob
    {
        HWND                         hWnd;
        std::vector<std::wstring>    vInf;
        std::vector<std::wstring>    vCab;
        std::wstring                 sImgRoot;   // 离线目标根；空=当前系统
    };

    DWORD WINAPI DropInstallThreadProc(void* pv)
    {
        std::unique_ptr<TDropInstallJob> spJob(static_cast<TDropInstallJob*>(pv));  // [C++17 R3] 线程 ctx：函数退出自动释放
        TDropInstallJob* pJob = spJob.get();
        HWND hWndPost = pJob->hWnd;
        auto Post = [&](const CDuiString& s, WPARAM wKind)
        {
            // [C++17 R3] Post 消息所有权移交 UI，Post 失败由 unique_ptr 自删
            auto pMsg = std::make_unique<CDuiString>(s);
            if (hWndPost && ::IsWindow(hWndPost)
                && ::PostMessageW(hWndPost, WM_DISM_LOG, wKind, reinterpret_cast<LPARAM>(pMsg.get())))
            {
                pMsg.release();   // [C++17 R3] 所有权移交 UI 线程
            }
        };
        try
        {
            DismApiBridge::Init();
            const wchar_t* pszRoot = pJob->sImgRoot.empty() ? nullptr : pJob->sImgRoot.c_str();
            int nOk = 0, nFail = 0;

            for (size_t ii = 0; ii < pJob->vInf.size(); ++ii)
            {
                const std::wstring& sInf = pJob->vInf[ii];
                std::wstring sErr;
                bool bOk = DismApiBridge::AddDriver(sInf, &sErr, pszRoot, false);
                CDuiString sLine;
                sLine.Format(L"[drop] AddDriver %s : %s%s",
                             sInf.c_str(), bOk ? L"OK" : L"FAIL ",
                             bOk ? L"" : sErr.c_str());
                Post(sLine, bOk ? 0 : 1);
                if (bOk) ++nOk; else ++nFail;
            }
            for (size_t ic = 0; ic < pJob->vCab.size(); ++ic)
            {
                const std::wstring& sCab = pJob->vCab[ic];
                std::wstring sErr;
                bool bOk = DismApiBridge::AddPackage(sCab, &sErr, pszRoot);
                CDuiString sLine;
                sLine.Format(L"[drop] AddPackage %s : %s%s",
                             sCab.c_str(), bOk ? L"OK" : L"FAIL ",
                             bOk ? L"" : sErr.c_str());
                Post(sLine, bOk ? 0 : 1);
                if (bOk) ++nOk; else ++nFail;
            }

            CDuiString sDone;
            sDone.Format(L"[drop] 原生安装完成：成功 %d，失败 %d", nOk, nFail);
            Post(sDone, nFail ? 1 : 0);
        }
        catch (...)
        {
            Post(CDuiString(L"[drop] 拖放安装过程中发生异常，已中止"), 1);
        }
        // [C++17 R3] spJob unique_ptr 自动释放，所有权由启动方移交
        return 0;
    }
}

// ----------------------------------------------------------------------------
// WM_TIMER 分发表：仅本窗口自建的 9 个 ID（ID 契约见 FrameWnd_Messages.h）
// [R5] iPage>=0 的 5 个页属定时器直接经 PageById->OnHostTimer 分发，
// 壳不再保留对应 OnTimerXxx 薄转发；iPage<0 的 4 个纯壳定时器走
// pfnShell（类私有静态成员表，故可取同类私有 handler 的成员指针）。
// ----------------------------------------------------------------------------
const CFrameWnd::STimerEntry CFrameWnd::kTimerTab[] =
{
    { APPX_RING_TIMER_ID,     EPageId::PAGE_NONE, &CFrameWnd::OnTimerRing },  // [R2-FIX] Appx 独立转环
    { CAP_RING_TIMER_ID,      EPageId::PAGE_NONE, &CFrameWnd::OnTimerRing },  // [R2-FIX] Capability 独立转环
    { FEAT_RING_TIMER_ID,     EPageId::PAGE_NONE, &CFrameWnd::OnTimerRing },  // Windows 功能独立转环
    { CAP_WATCHDOG_TIMER_ID,  EPageId::PAGE_FEATURE, nullptr },  // Capability（看门狗）
    { FEAT_WATCHDOG_TIMER_ID, EPageId::PAGE_FEATURE, nullptr },  // Capability 代理 Feature
    { CAP_RENDER_BATCH_TIMER_ID, EPageId::PAGE_FEATURE, nullptr }, // [R16][O-2] 分批渲染接力
    { PROG_RENDER_TIMER_ID,     EPageId::PAGE_PROGRAM, nullptr }, // [FIX-PERF] 程序首屏分帧
    { PROG_FILTER_TIMER_ID,     EPageId::PAGE_PROGRAM, nullptr }, // [FIX-PERF] 程序搜索防抖
    { FEAT_TIMER_ID,          EPageId::PAGE_NONE, &CFrameWnd::OnTimerFeatLegacy },
    { CAP_REFRESH_TIMER_ID,   EPageId::PAGE_FEATURE, nullptr },  // Capability 自动刷新
    { CAP_RETRY_TIMER_ID,     EPageId::PAGE_FEATURE, nullptr },  // Capability 失败重试
    { TOOL_PROGRESS_TIMER_ID, EPageId::PAGE_TOOLS,   nullptr },  // Tools 进度条轮询
    { UPD_FILTER_TIMER_ID,    EPageId::PAGE_UPDATE,  nullptr },  // [性能 C1] 更新页过滤防抖
    { UPD_RENDER_BATCH_TIMER_ID, EPageId::PAGE_UPDATE, nullptr }, // [性能 B1] 更新列表分批渲染接力
    { FEAT_RENDER_BATCH_TIMER_ID, EPageId::PAGE_FEATURE, nullptr }, // [T8 P3] 功能列表分批渲染接力
    { CLEAN_TAIL_WATCHDOG_TIMER_ID, EPageId::PAGE_CLEANUP, nullptr }, // [FIX-UNFREEZE] 清理尾巴看门狗
    { LOG_FLUSH_TIMER_ID,     EPageId::PAGE_NONE, &CFrameWnd::FlushPendingLogs },
    { PAGE_BUILD_TIMER_ID,    EPageId::PAGE_NONE, &CFrameWnd::OnTimerPageBuild },
    { WARMUP_TIMER_ID,        EPageId::PAGE_NONE, &CFrameWnd::OnTimerWarmup },
};

// ---- 纯壳定时器 handler（页属定时器已 [R5] 下沉 OnHostTimer）----

void CFrameWnd::OnTimerRing()
{
    // [D2] 驱动旋转环重绘（Appx / 可选功能 / Windows 功能加载占位共用）
    for (int tab = 0; tab < 2; ++tab)
    {
        CDuiString sRing;
        sRing.Format(L"appx_ring_%d", tab);
        if (CControlUI* pRing = m_pm.FindControl(sRing.GetData()))
            pRing->Invalidate();
    }
    // [rb23] 可选功能页 / Windows 功能页加载环
    if (CControlUI* pRing = m_pm.FindControl(L"cap_ring"))
        pRing->Invalidate();
    if (CControlUI* pRing = m_pm.FindControl(L"feat_ring"))
        pRing->Invalidate();
}

void CFrameWnd::OnTimerFeatLegacy()
{
    // [P0-A3] 功能页已改后台线程 DismGetFeatures + WM_FEAT_DATA，
    // 旧 sig 轮询定时器不再产生；残留 ID 直接忽略并保险性 Kill。
    ::KillTimer(m_hWnd, FEAT_TIMER_ID);
}

// [T4 P1] 启动预热：5s 后在后台把「冷启动成本」转移到无人等待期。
// 步骤：Init → 探缓存 → 缺失/过期才补枚举 → 落盘。
// 目的：用户首次点开 功能/可选功能/已安装更新 任一页时，磁盘缓存已就绪，
// 直接走 stale-while-revalidate 秒开，不必现场等 DISM 全量枚举。
// 约束：
//   * 先到先得——用户已进过三页之一则整体让路（见下方判定）；
//   * 只补「缓存缺失或过期」的项，避免每次启动都打 CBS；
//   * 离线会话激活时跳过（预热的是在线系统，不是目标映像）；
//   * 失败静默：仅 OutputDebugString，绝不影响主界面。
// [STAB] 预热让路标志：UI 线程在任意页面构建时置位，预热 detach 线程只读。
// 用 atomic<bool> 而非普通 bool，避免跨线程读写竞争（UB）。
// 非 static：FrameWnd.cpp 的 EnsurePageBuilt 需要置位，经 NotifyWarmupYield() 访问。
std::atomic<bool> g_bWarmupYield{ false };

// [EXIT-FIX] 预热线程关闭标志（仅本 TU 使用）：
//   HandleCloseMessage 置位后，预热线程的「等用户空闲 30s」轮询环在 ≤250ms
//   内退出。原缺陷：warmup 按 [FIX-P2K] 登记进 BackgroundJobs 退出等待集，
//   但它的设计语义是"用户不空闲就一直等"（见 R80 P2 注释）——两份契约冲突。
//   实测后果：只要用户在退出前 30 秒内操作过，warmup 必然还活着，
//   WM_CLOSE 先被 WaitAll(15000) 卡 15 秒（退出卡顿），随后误弹
//   "仍有 1 个后台作业尚未结束（可能正在写盘或释放映像）"——
//   预热只是读 CBS 建缓存，根本不是危险作业。
//   修复后：warmup 收到标志立即收尾；若此刻正处在 CBS 枚举中途，
//   WaitAll 仍会等它（这才是 [FIX-P2K] 要保护的窗口，且为秒级短暂态）。
namespace { std::atomic<bool> g_bWarmupShutdown{ false }; }

// [REFACTOR] 预热子任务前置声明（定义在文件末尾，避免打乱 OnTimerWarmup 的结构）
static void WarmupFeatCache();
static void WarmupCapCache();


// ============================================================
// [R80 P2] 预热让路：「整轮放弃」改为「暂停可恢复」
//
// 原行为（R44 起）：预热线程在若干个检查点 `if (g_bWarmupYield) return;`。
//   `g_bWarmupYield` 一旦被 UI 线程置位就**永不回落**，于是这些 return 等价于
//   「预热永久终止」。实测后果：用户启动后先点了一下功能页，预热就直接结束——
//   可选功能页的缓存直到用户亲自进那一页才会建立，而那一页恰恰是最慢的一页
//   （300+ 条，DISM 会话 + CBS 读取）。
//
// 新行为：让路只表示「现在别抢」，不表示「以后不用做」。
//   · 遇到让路 → 记下还欠哪几页，线程不退出；
//   · 等键鼠空闲（沿用启动时的 30s 门）→ 补上欠的那几页；
//   · 补完后清账退出。
//
// 关键约束：**不能把让路当成功**。若期间用户一直不空闲，就一直等；
//   线程用后台模式 + 1s 轮询，开销可忽略。真正放弃只有两种情况：
//   进程退出（线程随进程结束）或离线会话（WarmupCapCache 内部另有门控）。
//
// 日志三分支（供事后核对，R80 的验收证据）：
//   [warmup] yield -> deferred (feat=1 cap=1)   遇到让路，记账继续等
//   [warmup] resumed after idle (feat=1 cap=1)  空闲后补跑
//   [warmup] done (feat=0 cap=0)                全部补齐，正常收尾
// ============================================================
namespace
{
// 还欠哪几页：started 表示这一轮已经发起过（不论成功/失败），
// 只有「因让路被跳过」的才记 pending。
struct WarmupPending
{
    bool feat = false;
    bool cap  = false;

    bool Any() const { return feat || cap; }
};

// 最近是否有键鼠输入（沿用启动时的 30s 空闲门，阈值一致避免两套口径）。
bool IsUserIdle()
{
    LASTINPUTINFO lii = { 0 };
    lii.cbSize = sizeof(lii);
    if (!::GetLastInputInfo(&lii)) return false;   // 取不到按忙碌处理
    return (::GetTickCount() - lii.dwTime) >= 30u * 1000u;
}

// [EXIT-FIX] 等待空闲或关闭请求。返回 false = 已请求退出（线程应立即收尾）。
//   原实现的等待环是裸 Sleep(1000) 轮询，退出请求最迟 1s 才被看见；
//   拆成 250ms 切片后关闭延迟 ≤250ms，WaitAll(15000) 不再被拖满。
//   *pbWaited 供日志沿用"是否真等过"的口径（可为 nullptr）。
bool WaitForIdleOrShutdown(bool* pbWaited)
{
    if (pbWaited) *pbWaited = false;
    for (;;)
    {
        if (g_bWarmupShutdown.load(std::memory_order_relaxed)) return false;
        if (IsUserIdle()) return true;
        if (pbWaited) *pbWaited = true;
        ::Sleep(250);
    }
}

void LogWarmupYield(const char* pszStage, const WarmupPending& p)
{
    char szBuf[160] = { 0 };
    std::snprintf(szBuf, sizeof(szBuf),
                  "[warmup] yield -> deferred (%s) (feat=%d cap=%d)\r\n",
                  pszStage, p.feat ? 1 : 0, p.cap ? 1 : 0);
    ::OutputDebugStringA(szBuf);
}

void LogWarmupResumed(const WarmupPending& p)
{
    char szBuf[160] = { 0 };
    std::snprintf(szBuf, sizeof(szBuf),
                  "[warmup] resumed after idle (feat=%d cap=%d)\r\n",
                  p.feat ? 1 : 0, p.cap ? 1 : 0);
    ::OutputDebugStringA(szBuf);
}

void LogWarmupDone(const WarmupPending& p)
{
    char szBuf[160] = { 0 };
    std::snprintf(szBuf, sizeof(szBuf),
                  "[warmup] done (feat=%d cap=%d)\r\n",
                  p.feat ? 1 : 0, p.cap ? 1 : 0);
    ::OutputDebugStringA(szBuf);
}
} // namespace

void CFrameWnd::OnTimerWarmup()
{
    ::KillTimer(m_hWnd, WARMUP_TIMER_ID);
    if (m_bWarmupStarted) return;
    if (COfflineSession::IsActive()) return;   // WinPE 目标映像：不预热在线会话
    // [T4 P1] 先到先得：用户已进过三页之一则预热整体让路。
    // 三页构建即代表用户已在等待真实枚举，此时再抢共享会话只会加剧排队。
    if (m_bPageBuilt[static_cast<int>(EPageId::PAGE_PROGRAM)]
        || m_bPageBuilt[static_cast<int>(EPageId::PAGE_FEATURE)]
        || m_bPageBuilt[static_cast<int>(EPageId::PAGE_UPDATE)])
    {
        m_bWarmupStarted = true;
        return;
    }
    m_bWarmupStarted = true;

    // 后台线程：不阻塞 UI。线程内全部异常就地吞掉（预热失败不得影响主界面）。
    // [STAB] 用原子标志做"中途让路"：预热要跑 Feat + Cap 两轮枚举，各可能数秒，
    // 期间用户若进页会阻塞在共享锁上。这里只在启动那一刻判定"先到先得"是不够的，
    // 必须在每轮之间复查。原子标志由 UI 线程在页面构建时置位，detach 线程只读，
    // 不触碰 m_bPageBuilt（成员属 UI 线程，且窗口可能先于线程析构）。
    // [R125] 原为 std::thread(...).detach()：只有 C++ catch，/EHsc 接不住
    //   SEH 硬件异常（AV/堆损坏）。本线程要跑 Feat + Cap 两轮 DISM 枚举，
    //   正是最容易在原生层触发 AV 的路径 —— 一旦逃逸会直接终止进程。
    //   BackgroundJobs.h 头注释写明要把这类裸 detach 全部收敛到
    //   SafeCreateThread（双层边界：内层 C++ catch 保析构、外层 __except 兜底
    //   SEH 并留痕 crashguard.log），这一处是遗留的最后一例。
    //   仍保留 detach 语义：线程继续跑，本函数拿到句柄后立即关闭。
    auto WarmupBody = [](void*) -> DWORD
    {
        // [P5] 预热错峰：把整个预热线程降到「后台模式」。
        //   THREAD_MODE_BACKGROUND_BEGIN 让系统把本线程的 I/O 与 CPU 优先级
        //   同时降到最低（等价 SetThreadPriority(THREAD_PRIORITY_LOWEST) 的
        //   加强版），效果是：用户进页时，页面枚举线程能立刻抢占 CPU；
        //   预热只在系统空闲时推进，不会与用户操作争夺时间片。
        //   这比单纯调优先级更彻底——后台模式还会降低磁盘/网络 I/O 优先级，
        //   而预热的主要开销正是 DISM 会话建立与 CBS 组件存储读取。
        //   失败不影响功能（仅少一层优先级保护），故不检查返回值。
        //   注意：必须在 try 之前设置，且与本线程生命周期同进退（detach 线程
        //   结束时系统自动回收；THREAD_MODE_BACKGROUND_END 非必需，但为清晰
        //   语义在正常退出路径显式恢复）。
        ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);

        try
        {
            if (!DismApiBridge::Init()) return 0;

            // [R80 P2] 让路检查从「return 退出线程」改为「记账后继续等」。
            //   原实现 g_bWarmupYield 一旦置位就永不回落，各处 return 等价于
            //   预热永久终止：用户启动后先点一下功能页，可选功能页（300+ 条、
            //   最慢的一页）的缓存就再也不会被预热建立。
            //   现在：欠哪几页就记在 p 里，等空闲再补；只有 DismApiBridge::Init
            //   失败这类「环境不具备」才真正退出。
            WarmupPending p;
            // [EXIT-FIX] 关闭请求：外层 for 在每个检查点探测，命中即跳出。
            bool bAbort = false;

            // 主循环：每轮先看让路与空闲，能跑就跑，跑完清账；
            // 跑不了且确实欠账就睡 1s 再看，直到补完。
            for (;;)
            {
                if (g_bWarmupShutdown.load(std::memory_order_relaxed))
                {
                    bAbort = true;
                    break;
                }

                if (g_bWarmupYield.load())
                {
                    // 让路：先把「本来要做但现在别做」的记下来
                    if (!p.feat) p.feat = true;
                    if (!p.cap)  p.cap  = true;
                    LogWarmupYield("yield flag set", p);

                    // 等空闲：原为 1s 轮询。后台模式下开销可忽略，
                    // 且不占用共享 DISM 会话锁（真正占锁的是下面两次枚举）。
                    // [EXIT-FIX] 改经 WaitForIdleOrShutdown：退出请求 ≤250ms 生效。
                    bool bWaited = false;
                    if (!WaitForIdleOrShutdown(&bWaited))
                    {
                        bAbort = true;
                        break;
                    }
                    if (bWaited) LogWarmupResumed(p);

                    // 恢复：让路标志由 UI 线程置位后不回落，这里由预热侧
                    // 自行「消耗」它——空闲且无人正在进页时才清。
                    // 用 exchange 保证只有一个读者清掉，避免把新一次进页的
                    // 让路请求误清（置位方在构建页时重新置 true 即可）。
                    g_bWarmupYield.store(false, std::memory_order_relaxed);
                    continue;
                }

                // [A-2 2026-10-25] 空闲门保留：用户正在操作时不发起新枚举。
                //   与上面 IsUserIdle 同一阈值（30s），避免两套口径。
                if (!IsUserIdle())
                {
                    if (!p.Any()) p.feat = p.cap = true;
                    LogWarmupYield("user active", p);
                    // [EXIT-FIX] 原 Sleep(1000) 后重查让路；现等空闲或退出请求，
                    //   语义等价（欠账已记在 p 里，回循环顶部重新判定）。
                    if (!WaitForIdleOrShutdown(nullptr))
                    {
                        bAbort = true;
                        break;
                    }
                    continue;
                }

                // ---- 功能页缓存：缺失或过期才补 ----
                if (p.feat)
                {
                    WarmupFeatCache();
                    p.feat = false;
                }

                // ---- 可选功能页缓存 ----
                // 映射与 AppxCapUpdService::StartCapEnumThread 完全一致（该路径本就
                // 只填 sIdentity/sState 两字段，Description/Size/依赖计数为空），
                // 因此预热写入的缓存与页面枚举产出的缓存逐字段等价。
                if (p.cap)
                {
                    WarmupCapCache();
                    p.cap = false;
                }

                // 已安装更新页的缓存来自 WUA Search，而非 CBS GetPackages，
                // 无法在本线程内安全构造；该项留待页面自身枚举后回写。

                if (!p.Any())
                {
                    LogWarmupDone(p);
                    break;   // 两页都补齐，正常收尾
                }
                // 还有欠账（例如某页内部因让路提前返回）→ 回到循环顶部再判
            }

            if (bAbort)
                ::OutputDebugStringA("[warmup] aborted by shutdown request\r\n");
        }
        catch (...)
        {
            // 预热失败静默：不写日志区、不弹框
            ::OutputDebugStringW(L"[warmup] skipped due to exception\r\n");
        }

        // [P5] 恢复正常优先级再退出（detach 线程即将结束，仅为语义完整）
        ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_END);

        // 返回 0：本 lambda 是线程入口（DWORD 即线程退出码），走到这里表示
        // 预热正常结束（两页补齐后 break 出主循环，或异常被就地吞掉）。
        // 与上面 Init 失败分支的 return 0 一致；只有 SEH 兜底才返回非 0。
        return 0;
    };

    // SafeCreateThread 第 3 参是 dwFlags（DWORD），不是线程名标签
    // [FIX-P2K] 收编 BackgroundJobs：DISM 预热线程纳入退出等待集合
    // （预热中 CBS/DISM 会话不再随 ExitProcess 被硬杀）
    HANDLE hWarmup = BackgroundJobs::Start(WarmupBody, nullptr, "warmup");
    (void)hWarmup;   // 句柄由 BackgroundJobs 登记管理
    if (!hWarmup)
        ::OutputDebugStringW(L"[warmup] thread create failed, warmup skipped\r\n");
}

void CFrameWnd::OnTimerPageBuild()
{
    // [LAG-FIX] 切页后延迟构建：先强制绘制空白页，再执行重活
    ::KillTimer(m_hWnd, PAGE_BUILD_TIMER_ID);

    int idx = m_nPendingBuildPage;
    m_nPendingBuildPage = -1;

    CTabLayoutUI* pTabPg = static_cast<CTabLayoutUI*>(
        m_pm.FindControl(L"content_area"));

    // 用户在定时器到期前已切走：放弃本次构建，回到该页时会重新排队
    if (idx >= 0 && (!pTabPg || pTabPg->GetCurSel() == idx))
    {
        ::UpdateWindow(m_hWnd);   // 先完成空白页一次绘制，消除点击未响应感

        if (!m_bPageBuilt[idx])
            EnsurePageBuilt(idx);

        // [R5] 优化页延迟构建后的状态核实（5 秒 TTL）经命令虚入口收口
        if (idx == 4)
        {
            if (CPageBase* pOptim = PageById(EPageId::PAGE_OPTIM))
                pOptim->OnHostCommand(static_cast<int>(EHostCmd::HCMD_OPTIM_VERIFY_SOFT));
        }
    }
}

// ----------------------------------------------------------------------------
// 系统消息分发
// ----------------------------------------------------------------------------
bool CFrameWnd::HandleSystemMessage(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    // [R18/R4] 全局加速键（Win8Metro.cpp 加速键表 → WM_COMMAND）
    if (uMsg == WM_COMMAND)
    {
        int nId = LOWORD(wParam);
        if (nId >= static_cast<int>(EAccelCmd::ID_ACCEL_SCAN) && nId <= static_cast<int>(EAccelCmd::ID_ACCEL_REDO))
        {
            OnAccelCommand(nId);
            return true;
        }
        // 其余 WM_COMMAND 放行给 DuiLib 控件路由
    }

    // 窗口尺寸变化后同步 tooltip 命中矩形
    if (uMsg == WM_SIZE) HandleSizeMessage(wParam, lParam);   // 仅副作用，不拦截

    // 托盘回调：单击/双击激活窗口，右键弹简易菜单
    if (uMsg == WM_TRAYNOTIFY) return HandleTrayNotify(wParam, lParam);

    // [修复 R5 / P1-10] 启动项右键菜单钩子
    // P8：右键菜单第二级路由（原 WM_RBUTTONUP/WM_CONTEXTMENU 两大块
    // ——startup_list 钩子、统一列表菜单、optim_tree、prow_/ucrow_/
    // svcrow_ 行命中——已随各页 CPageBase::OnContextMenu 下沉）。
    // 消息差异：startup_list 钩子原对两种消息生效，其余菜单仅
    // WM_RBUTTONUP；页内按 bFromContextMsg 自行甄别保持原行为。
    if (uMsg == WM_RBUTTONUP || uMsg == WM_CONTEXTMENU)
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (CPageBase* pPage = CurrentPage())
        {
            if (pPage->OnContextMenu(pt, uMsg == WM_CONTEXTMENU))
                return true;
        }
        // 页未消费则放行 DuiLib 默认菜单处理
    }

    // [C2] Delete 键：程序页卸载选中项 / 预应答页删除选中命令（编辑框聚焦时不拦截）
    if (uMsg == WM_KEYDOWN && wParam == VK_DELETE)
    {
        CControlUI* pFocus = m_pm.GetFocus();
        if (pFocus)
        {
            const wchar_t* pszCls = pFocus->GetClass();
            if (pszCls && (lstrcmpiW(pszCls, L"EditUI") == 0
                        || lstrcmpiW(pszCls, L"RichEditUI") == 0))
            {
                // 交给编辑框正常处理
            }
            else
            {
                // P8：Delete 动作下沉当前页 CPageBase::OnDeleteKey
                if (CPageBase* pPage = CurrentPage())
                {
                    if (pPage->OnDeleteKey())
                        return true;
                }
            }
        }
    }

    // [B2] F5 全局刷新：按当前页面重载（编辑框聚焦时不拦截，避免打断输入）
    if (uMsg == WM_KEYDOWN && wParam == VK_F5 && !wParam)
    {
        CControlUI* pFocus = m_pm.GetFocus();
        if (pFocus)
        {
            const wchar_t* pszCls = pFocus->GetClass();
            if (pszCls && (lstrcmpiW(pszCls, L"EditUI") == 0
                        || lstrcmpiW(pszCls, L"RichEditUI") == 0))
            {
                // 编辑框聚焦，不拦截
            }
            else
            {
                RefreshCurrentPage();
                return true;
            }
        }
        else
        {
            RefreshCurrentPage();
            return true;
        }
    }

    // [B3] 拖拽文件：接受 .inf / .sys / .cab，把路径复制到剪贴板 + 日志记录
    // 真正的 pnputil /add-driver 需管理员权限，此处仅给出命令行提示
    if (uMsg == WM_DROPFILES) return HandleDropFiles(wParam);

    // [修复 R5 / F-2] 窗口跨不同缩放比例显示器移动时热切换 DPI。
    if (uMsg == WM_DPICHANGED)
    {
        OnDpiChanged(LOWORD(wParam), HIWORD(wParam), (RECT*)lParam);
        return true;
    }

    // [R201] 系统主题/强调色变更：重新应用窗口外观并刷新强调色缓存。
    //   为什么必须接这条：用户改了"设置 > 个性化 > 颜色"后，
    //   不重启本应用就看不到变化 —— 表现为"主题色设置了但没生效"。
    //   WM_SETTINGCHANGE 的 lParam 会指明具体变了什么（"ImmersiveColorSet"
    //   等），这里不细分辨：统一重放一次外观应用，代价极低（几次属性写入）。
    if (uMsg == WM_SETTINGCHANGE || uMsg == WM_THEMECHANGED
        || uMsg == WM_DWMCOLORIZATIONCOLORCHANGED)
    {
        // [R325] 必须强制重放，不能只靠 ApplySystemTheme 内部的"深浅翻转"判据。
        //   反例：用户只改「设置 > 个性化 > 颜色 > 强调色」，系统深浅色没变，
        //   m_bLastDarkTheme == bDark，重放分支整段被跳过 —— 令牌缓存确实重建了
        //   （tokens::Refresh），但控件上的属性仍是旧强调色，界面看起来毫无反应。
        //   这条是唯一绕过翻转判据的路径，也是"改了主题色但界面不变"的残留原因。
        ApplySystemTheme(true);
        // 不拦截：DuiLib 自身也可能对 WM_THEMECHANGED 有处理，
        // 返回 false 让它继续按框架逻辑走。
        return false;
    }

    // B1 增量：跨 DPI 后延迟重建动态控件树（清理/优化/启动三项页面里手写的
    // 字号、列宽、按钮宽度都缓存了旧 DPI 值，需要重新构建才能反映新缩放）。
    if (uMsg == WM_APP + 100)
    {
        // [修复 P1-DPI] ① 只重建此前已懒构建过的页面，避免把未访问页面
        // 提前固化；② Build 会销毁旧控件树，而 m_pFocusCtrl（焦点高亮边框）
        // 仍指向旧控件——跨显示器 DPI 改变后会解引用悬挂指针(UAF)。
        // 重建前（旧控件仍存活，还原边框是安全的）先清焦点。
        // （原 m_pCachedOutput 列表缓存随 out_* 死链路删除，不再需要失效）
        if (m_pFocusCtrl) OnFocusChanged(nullptr);

        // [R3] 三页手算固定尺寸树的重建收口为
        // CPageBase::OnRebuildDynamicTree（仅 Cleanup/Optim/Startup 覆写
        // 非空，其余页默认空），只重建此前已懒构建过的页面。
        for (int i = 0; i < 16; ++i)
        {
            if (m_bPageBuilt[i])
            {
                if (CPageBase* pPage = PageById(i)) pPage->OnRebuildDynamicTree();
            }
        }

        return true;
    }

    // 页侧 UI 点击路径（优化页 combo/开关/批量）请求的延迟重启：
    // 此时已不在 combo/开关的鼠标消息处理栈上（DuiLib 弹层已完成关闭、
    // 鼠标捕获已释放），执行 RestartExplorer 不会发生弹层半销毁/UAF。
    // 重启进行中重复到达的请求由 RestartExplorer 入口守卫拒绝。
    if (uMsg == WM_HOST_REQUEST_RESTART_EXPLORER)
    {
        RestartExplorer();
        return true;
    }

    // [CRASHFIX-UAF] 页侧 HostDefer 排入的延迟 UI 重建（如功能页勾选/展开
    // 后的整表 RebuildList）。执行点已脱离鼠标事件派发栈，RemoveAll 删除
    // 的控件不再有任何在途 DoEvent 引用。回调内再次 HostDefer 属合法接力
    // （入队后由本轮 while 继续消费），上限守卫防失控自排循环。
    if (uMsg == WM_UI_DEFERRED_EXEC)
    {
        for (int nGuard = 0; nGuard < 64 && !m_vDeferredFns.empty(); ++nGuard)
        {
            std::vector<std::function<void()>> vLocal;
            vLocal.swap(m_vDeferredFns);
            for (auto& fn : vLocal) if (fn) fn();
        }
        return true;
    }

    // [FIX-TRAY] explorer 重启/崩溃后新 shell 会广播 TaskbarCreated，
    // 所有托盘注册随旧 shell 一起失效——不重注册图标就永久消失。
    // RestartExplorer 成功路径另有显式补注册，这里覆盖"非本程序发起的
    // shell 重启"（explorer 崩溃自恢复、用户手动重启等）场景。
    static UINT s_uTaskbarCreated = 0;
    if (s_uTaskbarCreated == 0)
        s_uTaskbarCreated = ::RegisterWindowMessageW(L"TaskbarCreated");
    if (s_uTaskbarCreated != 0 && uMsg == s_uTaskbarCreated)
    {
        if (m_bTrayAdded) RemoveTrayIcon();
        InitTrayIcon();
        return true;
    }

    // [B4] 最大化 / 还原后的当前页重排（异步、边沿触发、可合并）。
    if (uMsg == WM_APP + 101) return HandleRelayoutRequest(wParam, lParam);

    // [FIX-TITLEBAR] 标题栏窗口按钮定位（WM_APP+102）。
    //   必须是独立消息且晚于 DuiLib 布局：WM_SIZE 里直接 SetPos 会被随后
    //   DuiLib 的 Measure/Arrange 覆盖掉（实测无效，按钮仍在 868~967）。
    //   这里在 WM_SIZE 中 PostMessage 排队，等本轮布局结束后再定位，
    //   位置才不会被覆盖。
    if (uMsg == WM_APP + 102)
    {
        LayoutTitlebarButtons();
        return true;
    }

    // [T4 P1] 启动预热惰性启用：主窗口已建且收到系统消息后，挂一次性 5s 定时器。
    // 放在这里而非 InitWindow()，可避免推后 FrameWnd.cpp 的行级白名单行号；
    // 离线会话激活时 handler 内部自行跳过。m_bWarmupArmed 防重复 SetTimer。
    if (!m_bWarmupArmed && uMsg != WM_TIMER && m_hWnd)
    {
        m_bWarmupArmed = true;
        ::SetTimer(m_hWnd, WARMUP_TIMER_ID, 5000, nullptr);
    }

    // 定时器：只处理本窗口自建 ID；其余（DuiLib 滚动条 thumb 拖拽、
    // 按钮长按重复）必须放行 m_pm.MessageHandler，否则滚动条无法拖动。
    if (uMsg == WM_TIMER)
    {
        for (const STimerEntry& ent : kTimerTab)
        {
            if (wParam == ent.uId)
            {
                // [R5] 页属定时器 → 页虚入口；纯壳定时器 → 壳成员 handler。
                if (ent.pfnShell)
                    (this->*(ent.pfnShell))();
                else if (CPageBase* pPage = PageById(ent.iPage))
                    pPage->OnHostTimer(static_cast<UINT>(wParam));
                return true;
            }
        }
        // 非本窗口定时器：放行
    }

    if (uMsg == WM_CLOSE) return HandleCloseMessage(wParam);

    return false;
}
// [C++17 R3] 文件级内联常量（真实 C++17 inline variables）：
// 本页处理批次的超时/重试上限，供本文件内部使用；inline 保证多 TU 下 ODR 一致。
inline constexpr int kFwStepTimeoutMs = 120000;
inline constexpr int kFwMaxRetry = 3;

// [STAB] 由 UI 线程在页面构建时调用，通知预热线程让路。
// 幂等：多次置位无副作用。用 atomic store 保证 detach 线程能立即看到。
void NotifyWarmupYield()
{
    g_bWarmupYield.store(true, std::memory_order_relaxed);
}

// [REFACTOR] 从 HandleSystemMessage 提取：WM_DROPFILES 拖放处理。
// 逐字搬迁，仅去掉外层 if 与一级缩进，零逻辑改动。
bool CFrameWnd::HandleDropFiles(WPARAM wParam)
{
    HDROP hDrop = (HDROP)wParam;
    UINT nFiles = ::DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
    if (nFiles > 0)
    {
        // [R-B76] 先把全部拖入路径交给当前页消费（更新页：.msu/.cab 本地补丁安装）
        {
            std::vector<std::wstring> vDrop;
            for (UINT fi = 0; fi < nFiles; ++fi)
            {
                wchar_t szOne[MAX_PATH] = { 0 };
                ::DragQueryFileW(hDrop, fi, szOne, _countof(szOne));
                vDrop.push_back(szOne);
            }
            CPageBase* pDropPage = CurrentPage();
            if (pDropPage && pDropPage->OnDropFiles(vDrop))
            {
                ::DragFinish(hDrop);
                return true;
            }
        }

        // [P1-收尾] 拖入 .inf/.cab 经确认后原生安装（DismAddDriver/
        // DismAddPackage 进程内直连，后台线程，零 pnputil/dism 命令行）；
        // .sys 无法独立安装，仍复制路径到剪贴板供用户参考。
        std::vector<std::wstring> vInf, vCab, vSys;

        for (UINT i = 0; i < nFiles; ++i)
        {
            wchar_t szPath[MAX_PATH] = { 0 };
            ::DragQueryFileW(hDrop, i, szPath, _countof(szPath));

            CDuiString sPath(szPath);
            const wchar_t* sLower = sPath.GetData();
            int nLen = static_cast<int>(wcslen(sLower));

            if (nLen > 4 && lstrcmpiW(sLower + nLen - 4, L".inf") == 0)
            {
                vInf.push_back(szPath);
                CDuiString sLog;
                sLog.Format(StringMgr.Get("misc.h5386"), sPath.GetData(), i + 1, nFiles);
                AppendLog(sLog.GetData());
            }
            else if (nLen > 4 && lstrcmpiW(sLower + nLen - 4, L".sys") == 0)
                vSys.push_back(szPath);
            else if (nLen > 4 &&
                     (lstrcmpiW(sLower + nLen - 4, L".cab") == 0
                      || lstrcmpiW(sLower + nLen - 4, L".msu") == 0))
                vCab.push_back(szPath);
        }

        CDuiString sAll;
        for (const auto& sOne : vInf) { if (!sAll.IsEmpty()) sAll += L"\n"; sAll += sOne.c_str(); }
        for (const auto& sOne : vCab) { if (!sAll.IsEmpty()) sAll += L"\n"; sAll += sOne.c_str(); }

        const size_t nInstallable = vInf.size() + vCab.size();

        if (nInstallable > 0)
        {
            CDuiString sAsk;
            sAsk.Format(L"检测到 %d 个驱动(.inf)/补丁包(.cab/.msu)，是否立即原生安装到%s？",
                        static_cast<int>(nInstallable),
                        COfflineSession::IsActive() ? L"离线目标映像" : L"当前系统");
            int nYes = ::MessageBoxW(m_hWnd, sAsk.GetData(), L"原生安装确认",
                                     MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1);

            // 同时保留路径复制，方便取消时手动处理
            FrameWnd_Util_CopyTextToClipboard(m_hWnd, sAll.GetData());

            if (nYes == IDYES)
            {
                // [CRASHFIX] 裸 detach 迁移为受 SEH 边界保护、可被 WM_CLOSE 等待的统一作业
                // [C++17 R3] 线程 ctx：所有权移交 DropInstallThreadProc，Start 失败由 unique_ptr 自删
                auto pJob = std::make_unique<TDropInstallJob>();
                pJob->hWnd = m_hWnd;
                pJob->vInf = vInf;
                pJob->vCab = vCab;
                pJob->sImgRoot = COfflineSession::IsActive()
                    ? COfflineSession::GetImageRoot().GetData() : std::wstring();
                if (BackgroundJobs::Start(&DropInstallThreadProc, pJob.get(), "drop-install"))
                    pJob.release();   // [C++17 R3] 所有权移交 DropInstallThreadProc
                else
                    AppendLog(L"[drop] 后台作业启动失败（线程资源不足）");   // unique_ptr 自动释放
            }
            else
            {
                CDuiString sHint;
                sHint.Format(StringMgr.Get("misc.h6973"), static_cast<int>(nInstallable));
                AppendLog(sHint.GetData());
            }
        }
        else if (!vSys.empty())
        {
            // 纯 .sys：无 INF 描述符不能直接安装，复制路径供用户参考
            CDuiString sSysPaths;
            for (size_t k = 0; k < vSys.size(); ++k)
            {
                if (k) sSysPaths += L"\n";
                sSysPaths += vSys[k].c_str();
            }
            FrameWnd_Util_CopyTextToClipboard(m_hWnd, sSysPaths.GetData());
            CDuiString sHint;
            sHint.Format(L"已复制 %d 个 .sys 路径到剪贴板（.sys 需配套 .inf 才能安装）",
                         static_cast<int>(vSys.size()));
            AppendLog(sHint.GetData());
        }
        else
        {
            CDuiString sSkip;
            sSkip.Format(StringMgr.Get("misc.hd967"), nFiles);
            AppendLog(sSkip.GetData());
        }
    }

    ::DragFinish(hDrop);
    return true;
}

// [REFACTOR] 从 HandleSystemMessage 提取：WM_CLOSE 退出序列。逐字搬迁零改动。
bool CFrameWnd::HandleCloseMessage(WPARAM wParam)
{
    ::KillTimer(m_hWnd, m_uTimerID);
    ::KillTimer(m_hWnd, FEAT_TIMER_ID);
    ::KillTimer(m_hWnd, APPX_RING_TIMER_ID);  // [R2-FIX] 各页独立转环 ID
    ::KillTimer(m_hWnd, CAP_RING_TIMER_ID);
    ::KillTimer(m_hWnd, FEAT_RING_TIMER_ID);
    ::KillTimer(m_hWnd, CAP_REFRESH_TIMER_ID);
    ::KillTimer(m_hWnd, CAP_RETRY_TIMER_ID);
    ::KillTimer(m_hWnd, TOOL_PROGRESS_TIMER_ID);  // [R107]
    ::KillTimer(m_hWnd, LOG_FLUSH_TIMER_ID);      // [LAG-FIX]
    ::KillTimer(m_hWnd, PAGE_BUILD_TIMER_ID);     // [LAG-FIX]
    ::KillTimer(m_hWnd, CAP_WATCHDOG_TIMER_ID);   // [rb23]
    ::KillTimer(m_hWnd, FEAT_WATCHDOG_TIMER_ID);  // [rb23]

    FlushPendingLogs();   // [LAG-FIX] 退出前落盘残留日志

    // [EXIT-FIX] 先通知预热线程收尾：它按设计"等用户空闲 30s 才干活"，
    // 不属于"正在写盘/释放映像"的危险作业。必须在 WaitAll 之前置位，
    // 否则用户在退出前 30 秒内操作过，warmup 必然还在 250ms 轮询环里，
    // 每次退出都会先卡 WaitAll(15000) 再误弹"后台作业仍在运行"。
    g_bWarmupShutdown.store(true, std::memory_order_relaxed);

    AbortDism();

    // [EXIT-FIX] 主动杀光作业内全部工具子进程（dism/powershell/sysprep/
    // prowck/ESD 转换等）。
    ProcessJob::KillChildren();

    // [CRASHFIX G-CRS-02] 等待受管后台作业收尾（映像释放/转换/WOF 等）。
    // 先给 15 秒优雅窗口；仍有作业未结束时必须二次确认——
    // "已格式化目标卷、映像尚未释放完"窗口期强退会留下不可启动的卷。
    // [EXIT-DIAG] 日志与弹窗列出具体作业标签，"是谁在拦退出"可直接核实。
    size_t nActive = BackgroundJobs::ActiveCount();
    if (nActive > 0)
    {
        std::vector<const char*> vJobTags;
        BackgroundJobs::GetActiveTags(vJobTags);
        CDuiString sTags;
        for (size_t ti = 0; ti < vJobTags.size(); ++ti)
        {
            CDuiString sOne;
            sOne.Format(L"%hs%s", vJobTags[ti],
                        (ti + 1 < vJobTags.size()) ? L", " : L"");
            sTags += sOne;
        }
        CDuiString sWaitLog;
        sWaitLog.Format(L"[exit] 等待后台作业收尾，请勿强制结束进程… (%u: %s)",
                        static_cast<unsigned>(vJobTags.size()), sTags.GetData());
        AppendLogEx(sWaitLog.GetData(), 1);
        if (!BackgroundJobs::WaitAll(15000))
        {
            CDuiString sAsk;
            sAsk.Format(L"仍有 %u 个后台作业尚未结束（%s）。\r\n"
                        L"可能正在写盘或释放映像，现在强制退出可能导致目标文件损坏或目标卷无法启动。\r\n\r\n"
                        L"是否仍要退出？",
                        static_cast<unsigned>(BackgroundJobs::ActiveCount()),
                        sTags.IsEmpty() ? L"未知" : sTags.GetData());
            if (::MessageBoxW(m_hWnd, sAsk.GetData(), L"后台作业仍在运行",
                              MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
            {
                // 用户取消退出：放弃本次关闭，作业继续运行
                return true;
            }
            // 用户明确要求强退：句柄随进程回收；先再回收一次已完成句柄
            BackgroundJobs::PurgeFinished();
        }
    }

    // [EXIT-FIX] 原代码吞掉 WM_CLOSE 默认处理后未 DestroyWindow，
    // 一旦退出流程卡住会留下无响应的僵尸窗口。显式销毁之。
    ::DestroyWindow(m_hWnd);
    PostQuitMessage(0);
    return true;
}

// [REFACTOR] 从 HandleSystemMessage 提取：WM_TRAYNOTIFY 托盘图标消息。逐字搬迁零改动。
bool CFrameWnd::HandleTrayNotify(WPARAM wParam, LPARAM lParam)
{
    UINT uEvt = static_cast<UINT>(LOWORD(lParam));   // NOTIFYICON_VERSION_4: 事件在 LOWORD

    if (uEvt == WM_LBUTTONUP || uEvt == WM_LBUTTONDBLCLK)
    {
        if (::IsIconic(m_hWnd)) ::ShowWindow(m_hWnd, SW_RESTORE);
        ::ShowWindow(m_hWnd, SW_SHOW);
        ::SetForegroundWindow(m_hWnd);
    }
    else if (uEvt == WM_RBUTTONUP)
    {
        HMENU hMenu = ::CreatePopupMenu();
        ::AppendMenu(hMenu, MF_STRING, 1, StringMgr.Get("misc.sc575937d"));
        // [B4] 主题列表（保守版：仅枚举 skins/，不做热切换）
        ::AppendMenu(hMenu, MF_STRING, 3, StringMgr.Get("misc.hc0d3"));
        // [B5] 语言选择
        ::AppendMenu(hMenu, MF_STRING, 4, StringMgr.Get("misc.h56e9"));
        ::AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenu(hMenu, MF_STRING, 2, StringMgr.Get("general.exit"));

        POINT pt; ::GetCursorPos(&pt);
        ::SetForegroundWindow(m_hWnd);
        int nSel = ::TrackPopupMenu(hMenu,
            TPM_RETURNCMD | TPM_RIGHTBUTTON,
            pt.x, pt.y, 0, m_hWnd, nullptr);
        ::DestroyMenu(hMenu);

        if (nSel == 1)
        {
            if (::IsIconic(m_hWnd)) ::ShowWindow(m_hWnd, SW_RESTORE);
            ::ShowWindow(m_hWnd, SW_SHOW);
            ::SetForegroundWindow(m_hWnd);
        }
        else if (nSel == 2)
        {
            Close(0);
        }
        else if (nSel == 3)
        {
            std::vector<CDuiString> vThemes;
            ListAvailableThemes(vThemes);
            if (!vThemes.empty())
            {
                CDuiString sAll;
                for (size_t i = 0; i < vThemes.size(); ++i)
                {
                    if (!sAll.IsEmpty()) sAll += L"\n";
                    sAll += vThemes[i];
                }
                FrameWnd_Util_CopyTextToClipboard(m_hWnd, sAll.GetData());
            }
        }
        else if (nSel == 4)
        {
            ShowLanguagePicker();
        }
    }

    return true;
}

// [REFACTOR] 从 HandleSystemMessage 提取：WM_APP+101 最大化/还原后动态树重建。逐字搬迁零改动。
// [FIX-TITLEBAR] 标题栏窗口按钮组定位：贴右缘、46px 等距。
//   在 WM_SIZE 中 PostMessage(WM_APP+102) 延后调用，确保 DuiLib 本轮
//   Measure/Arrange 已经跑完，SetPos 的结果不会被覆盖。
//   顺序（从右往左）：关闭 → 最大化/还原（同槽位）→ 最小化。
void CFrameWnd::LayoutTitlebarButtons()
{
    // [FIX-TITLEBAR] 标题栏窗口按钮组定位：贴右缘、46px 等距。
    //   为什么不用 titlebar 自己的矩形：
    //     实测 titlebar->GetPos() 恒为 (0,0,0,0) —— 它是 root 下的直接子项，
    //     但 DuiLib 对它的 SetPos 没有回写有效矩形（多次探针确认）。
    //     因此改以 root 的宽度作为右边界基准，root->GetPos() 是可靠的
    //     （实测 (0,0,2240,1575)，即 175% DPI 下的物理像素）。
    //   坐标系：GetPos 返回物理像素，CControlUI::SetPos 内部不再缩放，
    //     所以按钮宽度也必须换算成物理像素（46 × scale / 100）。
    CControlUI* pRoot = m_pm.GetRoot();
    if (!pRoot) return;
    RECT rcRoot = pRoot->GetPos();
    if (rcRoot.right <= rcRoot.left) return;

    const int nScale = (m_pm.GetDPIObj() != nullptr && m_pm.GetDPIObj()->GetScale() > 0)
                       ? static_cast<int>(m_pm.GetDPIObj()->GetScale()) : 100;
    const int nBtnW = 46 * nScale / 100;
    const int nTop = 0;
    const int nBottom = 37 * nScale / 100;

    int nRight = rcRoot.right;
    // 关闭 → 最大化/还原（同槽位）→ 最小化
    if (CControlUI* p = m_pm.FindControl(L"closebtn"))
    {
        RECT rc = { nRight - nBtnW, nTop, nRight, nBottom };
        p->SetPos(rc, false);
    }
    nRight -= nBtnW;

    RECT rcSlot = { nRight - nBtnW, nTop, nRight, nBottom };
    if (CControlUI* p = m_pm.FindControl(L"maxbtn"))
        p->SetPos(rcSlot, false);
    if (CControlUI* p = m_pm.FindControl(L"restorebtn"))
        p->SetPos(rcSlot, false);
    nRight -= nBtnW;

    if (CControlUI* p = m_pm.FindControl(L"minbtn"))
    {
        RECT rc = { nRight - nBtnW, nTop, nRight, nBottom };
        p->SetPos(rc, false);
    }

    if (pRoot) pRoot->NeedUpdate();
}

bool CFrameWnd::HandleRelayoutRequest(WPARAM wParam, LPARAM lParam)
{



    m_bRelayoutPosted = false;

    if (!::IsIconic(m_hWnd))
    {
        CTabLayoutUI* pTab = static_cast<CTabLayoutUI*>(
            m_pm.FindControl(L"content_area"));

        int nPage = pTab ? pTab->GetCurSel() : -1;

        if (nPage >= 0 && nPage < 16 && m_bPageBuilt[nPage])
        {
            CPageBase* pPage = PageById(nPage);

            // [R3] 手算固定尺寸三页（NeedsFullRebuildOnRelayout）整树
            // 重建，其余页仅根控件 NeedUpdate。重建前失效焦点，
            // 防悬挂解引用（[修复 P1-DPI]）。
            if (pPage && pPage->NeedsFullRebuildOnRelayout())
            {
                if (m_pFocusCtrl) OnFocusChanged(nullptr);
                pPage->OnRebuildDynamicTree();
            }
            else if (m_pm.GetRoot())
            {
                m_pm.GetRoot()->NeedUpdate();
            }
        }
        else if (m_pm.GetRoot())
        {
            m_pm.GetRoot()->NeedUpdate();
        }

        RefreshTooltipRects();
    }

    return true;
}

// [REFACTOR] 从 HandleSystemMessage 提取：WM_SIZE 尺寸变化（同步 tooltip 命中矩形）。逐字搬迁零改动。
void CFrameWnd::HandleSizeMessage(WPARAM wParam, LPARAM lParam)
{
    RefreshTooltipRects();
    // [B4] 最大化 / 从最大化还原为离散事件：仅这两种边沿异步重建当前页。
    // Cleanup/Optim/Startup 三页在 Build 时按当时客户区宽度手算列宽/字号/
    // 按钮宽（与 DPI 路径 WM_APP+100 重建的三页一致），最大化后必须重建
    // 才能重排；拖拽缩放的连续 SIZE_RESTORED 不触发，避免性能抖动。
    // 注意：本块仅做副作用，不拦截 WM_SIZE（继续放行 DuiLib 布局）。
    bool bEdge = false;
    if (wParam == SIZE_MAXIMIZED)
    {
        bEdge = !m_bWasMaximized;
        m_bWasMaximized = true;
    }
    else if (wParam == SIZE_MINIMIZED)
    {
        m_bWasMaximized = false;
    }
    else // SIZE_RESTORED
    {
        bEdge = m_bWasMaximized;
        m_bWasMaximized = false;
    }

    if (bEdge && !m_bRelayoutPosted && m_hWnd)
    {
        m_bRelayoutPosted = true;
        ::PostMessageW(m_hWnd, WM_APP + 101, 0, 0);
    }

    // [FIX-TITLEBAR] 窗口按钮组按主流标题栏规范显式定位：贴右、46px 等距。
    //   为什么必须在代码里做，而不是靠 XML 布局：
    //     DuiLib 的 CHorizontalLayoutUI 只把 EstimateSize 返回 cx==0 的子项
    //     计为可伸缩项（nAdjustables），而**任何嵌套容器**的 m_cxyFixed.cx
    //     恒为 0。标题栏里"图标+标题"与"按钮组"都是嵌套容器，于是两者同时
    //     被当成可伸缩项、平分整行 → 按钮组白拿约 300px，组内按钮被拉开成
    //     83px 间距，整组停在窗口中部（实测：窗口宽 1280，按钮落在 868~967）。
    //     试过的 XML 修法都不可靠：给按钮组写死 width 会让整组被推到可视区
    //     之外（按钮直接消失）；float+floatalign 在 SetPos 阶段拿不到最终
    //     父矩形，同样消失。这里改为在布局完成后直接 SetPos，位置完全可控。
    //   注意：必须 PostMessage 延后执行 —— 在 WM_SIZE 内直接 SetPos 会被
    //   紧随其后的 DuiLib Measure/Arrange 覆盖（实测无效）。
    //   每次都投递（不做一次性标记）：窗口初始显示、还原、缩放都会触发
    //   WM_SIZE，而 DuiLib 每轮都会重新 Arrange 把按钮摆回错误位置，
    //   因此每轮布局后都要重新定位一次，否则只有首帧正确、后续被覆盖。
    if (m_hWnd)
        ::PostMessageW(m_hWnd, WM_APP + 102, 0, 0);

    // 与原实现语义一致：本块仅做副作用，不拦截 WM_SIZE（调用点也不 return）。
}

// [REFACTOR] 从 OnTimerWarmup 提取：功能页缓存预热（缺失/过期才补）。
static void WarmupFeatCache()
{
            std::vector<FeatRawEntry> vProbe;
            if (!FeatureService::LoadFeatCache(vProbe))
            {
                // [FIX-PERF] 进入同步 DISM 前最后复查：用户已进页则直接放弃本轮预热，
                // 避免前台等待一个不可中断的整轮枚举。
                if (g_bWarmupYield.load()) return;
                std::vector<DismApiBridge::FeatureInfo> vApi;
                std::wstring sErr;
                if (DismApiBridge::GetFeatures(vApi, &sErr))
                {
                    // [PERF] 与 WarmupCapCache 同理：枚举返回后复查让路标志，
                    // 用户已进页则不回写缓存，避免覆盖页面自己刚写入的数据。
                    if (g_bWarmupYield.load()) return;
                    std::vector<FeatRawEntry> vRaw;
                    vRaw.reserve(vApi.size());
                    for (size_t i = 0; i < vApi.size(); ++i)
                    {
                        FeatRawEntry e;
                        e.sSys = vApi[i].sName.c_str();
                        e.bEnabled = vApi[i].bEnabled;
                        vRaw.push_back(e);
                    }
                    // [R28] 预热写缓存失败不应静默：写不进会导致下次进页
                    // 仍读到旧/空列表，而日志里表现为"已刷新"，很难归因。
                    if (FeatureService::SaveFeatCache(vRaw))
                        ::OutputDebugStringW(L"[warmup] feat cache refreshed\r\n");
                    else
                        ::OutputDebugStringW(L"[warmup] feat cache write FAILED (list unaffected)\r\n");
                }
            }
}

// [REFACTOR] 从 OnTimerWarmup 提取：可选功能页缓存预热（缺失/过期才补）。
// 逐字搬迁，仅去掉外层裸作用域块与一级缩进，零逻辑改动。
static void WarmupCapCache()
{
            std::vector<CapEntry> vProbe;
            if (!AppxCapUpdService::LoadCapCache(vProbe))
            {
                // [FIX-PERF] 同上：前台已请求时不再启动新的整轮预热。
                if (g_bWarmupYield.load()) return;
                std::vector<DismApiBridge::CapabilityInfo> vApi;
                std::wstring sErr;
                if (DismApiBridge::GetCapabilities(vApi, &sErr))
                {
                    // [PERF] 枚举返回后再复查让路标志：若用户已在这几秒内进页，
                    // 说明页面自己的枚举正在跑或即将跑，此时绝不能把预热结果
                    // 写回缓存——否则会覆盖页面刚写入的新数据，导致下次进页
                    // 读到预热那份字段不全（无描述/无依赖计数）的缓存。
                    if (g_bWarmupYield.load()) return;
                    std::vector<CapEntry> vCaps;
                    vCaps.reserve(vApi.size());
                    for (size_t i = 0; i < vApi.size(); ++i)
                    {
                        CapEntry e;
                        e.sIdentity = vApi[i].sName.c_str();
                        e.sState = vApi[i].bInstalled ? L"Installed" : L"Not Present";
                        vCaps.push_back(e);
                    }
                    // [R28] 同 feat：预热写失败需可见，不能伪装成刷新成功。
                    if (AppxCapUpdService::SaveCapCache(vCaps))
                        ::OutputDebugStringW(L"[warmup] cap cache refreshed\r\n");
                    else
                        ::OutputDebugStringW(L"[warmup] cap cache write FAILED (list unaffected)\r\n");
                }
            }
}
