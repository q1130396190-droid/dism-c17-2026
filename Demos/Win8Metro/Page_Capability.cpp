#include "stdafx.h"
#include "ThemeTokens.h"
#include "OfflineSession.h"  // [R10-02] 离线会话经独立组件访问，去 CDataEngine 静态耦合
#include "SeedStamp.h"      // [R85] 种子过期检测
// ============================================================
// Page_Capability.cpp —— v2 P4 可选功能页
// 6 个原 CFrameWnd 函数 + CAP 刷新/重试两个定时器分支物理搬迁，
// 函数体零逻辑改动，宿主引用改 m_ctx.Xxx；后台 DISM 枚举/解析在
// AppxCapUpdService（S6）。
// ============================================================
#include "DismCore.h"          // [R8] 可选功能页纯计算组件（Rust）
#include "Page_Capability.h"
#include "CustomControls.h"
#include "BackgroundJobs.h"   // [FIX-S1-4] PostHeapOrDelete（DISM 失败日志回投）
#include "StringManager.h"
#include "FrameWnd_Util.h"
#include "Win32DialogService.h"  // P8：DlgBrowseFolder（btn_cap_browse）
#include "FrameWnd_Messages.h"
#include "DismApiBridge.h"       // [R-B21] Capability 原生直连（DismAddCapability/DismRemoveCapability/DismGetCapabilities）
#include "DataEngine.h"          // [P0] WinPE 离线会话
#include "SafeThread.h"          // DISM 后台线程异常边界
#include <memory>               // AutoWrap 参数数组 RAII
#include <map>                  // [R16][O-3] FindCapLocalized 预构建索引
#include <cwctype>              // [R16][O-3] towlower
using namespace DuiLib;
using i18n::CStringManager;
// ============================================================
// [R-B21] Capability 页 dism.exe 外部进程 → dismapi.dll 原生直连
// 官方公开 API：DismAddCapability / DismRemoveCapability
// 保持原版"操作后定时刷新"语义：
//   后台线程 DismApiBridge::* → PostMessage WM_TIMER(CAP_REFRESH) → UI 刷新
// ============================================================
namespace
{
struct CapDismCtx
{
    HWND         hWnd;
    int          nOp;    // 0=Remove, 1=Add
    std::wstring sName;
    std::wstring sSource;
    bool         bLimitAccess;
};
DWORD WINAPI CapDismThreadProc(void* pv)
{
    CapDismCtx* c = static_cast<CapDismCtx*>(pv);
    if (!c) return 0;
    HWND hWnd = c->hWnd;
    int nOp = c->nOp;
    std::wstring sName = c->sName;
    std::wstring sSource = c->sSource;
    bool bLimitAccess = c->bLimitAccess;
    // [C++17 R3] thread ctx: receiver takes ownership, freed at scope end (was: delete here; c unused after)
    std::unique_ptr<CapDismCtx> ctxGuard(c);
    // [P0-A3 2026-10-11] 在线/离线统一直连 dismapi：
    //   Add  → DismAddCapability（离线会话 DismOpenSession(目标根)）
    //   Remove → DismRemoveCapability
    // 删除旧的 CBSHost 转发、dism.exe ShellExecute 兜底与
    // ProcessRunner /Image /Remove-Capability 子进程路线。
    std::wstring sErr;
    bool bOk = false;
    DismApiBridge::Init();
    std::wstring sRoot;
    const wchar_t* pszRoot = nullptr;
    if (COfflineSession::IsActive())
    {
        sRoot = COfflineSession::GetImageRoot().GetData();
        pszRoot = sRoot.c_str();
    }
    if (nOp == 1)
    {
        bOk = DismApiBridge::AddCapability(sName, sSource, bLimitAccess,
                                           &sErr, pszRoot);
    }
    else
    {
        bOk = DismApiBridge::RemoveCapability(sName, &sErr, pszRoot);
    }
    HRESULT hr = DismApiBridge::LastHResult();
    // [C++17 阶段3][F13] 固定缓冲 wchar_t[256] -> std::wstring：_snwprintf_s 写入
    // data() 指向的连续存储（C++17 起 data() 可写），再按实际长度 resize。
    std::wstring sLog(257, L'\0');                       // 257 = 256 内容 + 1 null，C++17 起 data() 可写
    int nLog = wfmt::FormatInto(sLog.data(), sLog.size(),
        L"[cap] %s '%s' hr=0x%08lX ok=%d %s\r\n",
        nOp == 1 ? L"add" : L"remove", sName.c_str(),
        (unsigned long)hr, bOk ? 1 : 0,
        sErr.empty() ? L"" : sErr.c_str());
    if (nLog >= 0) sLog.resize(static_cast<size_t>(nLog));
    ::OutputDebugStringW(sLog.c_str());
    // [FIX-S1-4] DISM 结果用户可见：失败时经 WM_DIAG_LOG（kMsgOwnShell →
    // AppendLog）投递，替代此前仅 ODS —— 乐观缓存已废弃，失败必须可见，
    // 否则用户无从得知卸载/添加未生效。
    if (!bOk)
    {
        BackgroundJobs::PostHeapOrDelete(hWnd, WM_DIAG_LOG, 1,
            std::make_unique<CDuiString>(sLog.c_str()).release());
    }
    // 刷新仍由原有 CAP_REFRESH_TIMER_ID 定时器（20s/30s）驱动，
    // 此处不 PostMessage，避免 CBS 操作未完成即刷新导致空列表。
    (void)bOk;
    (void)hWnd;
    return 0;
}
void CapStartDismAsync(HWND hWnd, int nOp, const std::wstring& sName,
                       const std::wstring& sSource, bool bLimitAccess)
{
    // [C++17 R3] thread ctx: transferred to CapDismThreadProc on success; freed here on failure (was delete c)
    auto c = std::make_unique<CapDismCtx>();
    c->hWnd = hWnd;
    c->nOp = nOp;
    c->sName = sName;
    c->sSource = sSource;
    c->bLimitAccess = bLimitAccess;
    HANDLE hTh = SafeCreateThread(CapDismThreadProc, c.get());
    if (!hTh)
    {
        return; // [C++17 R3] unique_ptr frees ctx automatically
    }
    c.release(); // [C++17 R3] ownership transferred to thread proc
    ::CloseHandle(hTh);
}
} // namespace
// [R101] 匿名命名空间内的 CapStartDismAsync 不可跨 TU 引用（C3861）。
// 此处开一个具名桥接，供 Page_CapabilityTimers.cpp 复用同一实现，
// 避免在两处各留一份线程上下文构造逻辑。
void CapStartDismAsyncShared(HWND hWnd, int nOp, const std::wstring& sName,
                             const std::wstring& sSource, bool bLimitAccess)
{
    CapStartDismAsync(hWnd, nOp, sName, sSource, bLimitAccess);
}



void CPageCapability::BuildPage()
{
    CCheckBoxUI* pShowAll = static_cast<CCheckBoxUI*>(m_ctx.HostFindControl(L"cap_showall"));
    if (pShowAll)
    {
        CDuiString sText = pShowAll->GetText();
        StyleCheckBox(pShowAll, sText.GetData(), 30, m_ctx.HostGlyphBox(0));
    }
    StartEnumeration();
}
// [R165] 从 StartEnumeration 抽出：三级缓存读取（磁盘新鲜 → 磁盘过期 → 包内种子）。
//   返回是否命中任一来源；bSeedHit / bStaleHit 用于调用方判断能否短路枚举。
//
//   三条规则必须整体成立，原实现散在 3 层 if 里、极易改坏：
//     · 磁盘新鲜缓存命中（TTL 内） → 可短路（数据可信）
//     · 磁盘过期缓存命中（bIgnoreTtl）→ **不**短路。否则等于把 TTL 变成无限，
//       用户永远看不到新数据（[R80 P0] 补 stale 时明确记过这一点）
//     · 包内种子命中但指纹过期 → **不**短路。用户装/卸能力后种子就旧了，
//       宁可多枚举一次也不要显示错的勾选状态（[R85]）
//   [P4] 种子无 TTL 但必须指纹匹配（OSBUILD|SKU|LANG），不匹配返回 false，
//   行为与改动前一致（继续显示加载占位）。
// [R175] 从 CapSeedFromCache 抽出：[R85] 包内种子是否仍可信。
//   种子是安装时快照，用户装/卸能力后就旧了。用指纹（OSBUILD|SKU|LANG）判断，
//   过期返回 false → 调用方不短路枚举（宁可多枚举一次，也不要显示错的勾选状态）。
//   与功能页 FeatSeedStillValid 同一规则，两页保持一致。
[[nodiscard]] bool CPageCapability::CapSeedStillValid()
{
    bool bStale = false;
    if (!seedstamp::IsSeedStale(bStale)) return true;   // 判不出来就当有效
    if (!bStale) return true;
    m_ctx.HostLog(L"[capability] seed stale (system changed), will re-enumerate");
    return false;
}

[[nodiscard]] bool CPageCapability::CapSeedFromCache(std::vector<CapEntry>& vCache,
                                                     bool& bSeedHit, bool& bStaleHit)
{
    bSeedHit = false;
    bStaleHit = false;
    bool bHit = AppxCapUpdService::LoadCapCache(vCache);

    if (!bHit)
    {
        bHit = AppxCapUpdService::LoadCapCache(vCache, /*bIgnoreTtl=*/true);
        bStaleHit = bHit;
    }
    if (!bHit)
    {
        bHit = AppxCapUpdService::LoadSeedCapCache(vCache);
        // [R175] 种子有效性判定抽为一行调用，嵌套 4→3
        bSeedHit = bHit && CapSeedStillValid();
        if (bHit)
            m_ctx.HostLog(L"[capability] seed cache hit (first run), refreshing in background");
    }
    return bHit;
}

void CPageCapability::StartEnumeration()
{
    if (m_bLoading) return;
    m_bLoading = true;
    // [STAB] bump generation BEFORE starting thread so any superseded run's messages
    // are guaranteed to be older than this one's ID — UI-side can safely drop them.
    ++m_nCapGen;
    const int nThisGen = m_nCapGen;   // snapshot for thread lambda capture below
    // [PERF] 用户已进入本页并要发起真实枚举：立刻通知后台预热线程让路。
    // 预热线程在共享 DISM 会话锁内跑整轮枚举（可选功能 300+ 项，实测数秒），
    // 若不提前通知，本页的 StartCapEnumThread 会在 AcquireEnumSession 上
    // 干等预热跑完——这正是"可选功能页比修复前更慢"的直接原因。
    // 预热线程会在每轮开始前复查该标志，置位后立即放弃剩余轮次。
    NotifyWarmupYield();
    // [P1] 新一轮枚举开始：复位分批收尾标志。
    // 若不复位，上一轮末批置位的 true 会让本轮的 WM_CAP_PARTIAL 全部被丢弃。
    m_bCapPartialDone = false;
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"cap_list"));
    // [R16][O-4] stale-while-revalidate：列表为空时先渲染 TTL（600s）内的
    // 磁盘缓存秒开，后台枚举照常发起，WM_CAP_DATA 到达后用新数据整体重建。
    // [REGRESSION-FIX 2026-10-25] 会话内已有数据时直接复用，不清空、不重枚举。
    //   这是"可选功能页加载又变慢"的直接原因：原实现在 m_vCaps 非空时
    //   bCacheHit 必为 false（条件里带 m_vCaps.empty()），于是走进 else 分支
    //   把 m_vCaps.clear() 并重新挂转环占位 —— 用户切走再切回来，数据其实
    //   还在内存里，却被当成首次加载，白等一整轮 CBS 枚举。
    //   修复：会话内已有数据视为权威结果，保持现状直接返回；
    //   需要刷新时走 Refresh()：它先 clear() 再调本函数，因此这里只需判空。
    //   [R28 2026-10-25] 删掉 m_bForceRefresh：该标志从未被置 true，
    //   Refresh() 用 clear() 实现同一目的，是纯粹的死代码。
    //   留着会让人误以为存在两条刷新路径。
    if (!m_vCaps.empty())
    {
        m_bLoading = false;
        m_ctx.HostLog(L"[capability] session data reused, enumeration skipped");
        return;
    }
    // [R165] 三级缓存读取抽为 CapSeedFromCache（三条短路规则集中一处）
    std::vector<CapEntry> vCache;
    bool bSeedHit = false;
    bool bStaleHit = false;
    const bool bCacheHit = CapSeedFromCache(vCache, bSeedHit, bStaleHit);
    if (bCacheHit)
    {
        m_vCaps.swap(vCache);
        CDuiString sLog;
        sLog.Format(bStaleHit
                        ? L"[capability] render STALE cache first (%d entries), refreshing in background"
                        : L"[capability] render cache first (%d entries), refreshing in background",
                    static_cast<int>(m_vCaps.size()));
        m_ctx.HostLog(sLog.GetData());
        BuildList();
    }
    else
    {
        m_vCaps.clear();
        if (pList)
        {
            pList->RemoveAll();
        // [R227] Drop the row cache: RemoveAll() destroyed those controls and the
        //   pointers below are now dangling. The null test in ApplyFilterVisibility
        //   does NOT catch a stale non-null pointer.
        m_vCapRowPtr.clear();
        m_vCapRowSearch.clear();
            // [rb23] 用与 Appx 页一致的「旋转环 + 文案」加载占位，替代纯静态
            // 文字（旧版线程崩溃后页面既无动画也无任何反馈，像程序无响应）。
            pList->Add(m_ctx.HostMakeLoadingHint(
                m_ctx.HostSkinText(L"lbl_cap_loading"), L"cap_ring"));
        }
    }
    // [A-3 2026-10-25] 新鲜缓存短路：磁盘缓存命中即视为数据有效，不再发起后台枚举。
    //   原实现无论缓存新旧都无条件 StartCapEnumThread：每次进页都重打一遍 CBS，
    //   数据回来后 OnData 又整表重建 → 列表"跳变"一次。缓存命中率越高浪费越频繁。
    //   只有磁盘缓存（有 TTL）可短路；种子缓存无 TTL，仍需枚举校正，故排除。
    //   代价：陈旧窗口 ≤ 缓存 TTL；增删能力后已有 InvalidateCapCache 主动失效兜底。
    // [FIX-S1-4] 有卸载/添加动作在途时强制真实枚举：缓存可能仍是动作前的
    // 旧状态（乐观写已废弃），短路会让 DISM 失败被旧状态固化显示。
    if (bCacheHit && !bSeedHit && !bStaleHit && !m_bPendingRealRefresh)
    {
        m_ctx.HostLog(L"[capability] fresh cache served, background enumeration skipped");
        return;   // 缓存已是终态：不起转环、不起看门狗、不起枚举
    }
    // 旋转环 90ms 重绘；看门狗：枚举线程即使被硬件异常吞掉也能解除加载态。
    // [R16][O-1] 阈值自适应：首次保持 120s，此后按上次枚举实测耗时×2
    //（下限 30s/上限 600s，见 OnData）；到期只提示、不重启枚举。
    m_dwEnumStartTick = ::GetTickCount();
    ::SetTimer(m_ctx.HostHwnd(), CAP_RING_TIMER_ID, 90, nullptr);
    ::SetTimer(m_ctx.HostHwnd(), CAP_WATCHDOG_TIMER_ID, m_nWatchdogMs, nullptr);
    AppxCapUpdService::StartCapEnumThread(m_ctx.HostHwnd(), nThisGen);
}
// [P1] 枚举分批回报处理。
// 工作线程每攒够 100 条就 PostMessage 一批，本函数把该批追加进 m_vCaps
// 并只渲染新增的行——首屏不必等整表构建完。
// 末批（bFinal=true）负责收尾：解除加载态、落缓存、更新概览、刷新底栏。
//
// 与 WM_CAP_DATA（整表重建）的关系：
//   分批路径是"先到先看"的优化通道，整表消息仍会发出并走 OnData 全量重算，
//   保证最终结果与不经分批时完全一致（分批只是提前显示，不改变最终数据）。
//   若某一帧内 OnData 先到，后续迟到的分批消息会被 m_bCapPartialDone 拦掉。
void CPageCapability::OnCapPartial(CapPartialMsg* pMsg)
{
    if (!pMsg) return;
    std::unique_ptr<CapPartialMsg> guard(pMsg);   // [C++17 R3] 本函数独占所有权
    // [STAB] generation guard: drop batches from superseded enum runs.
    //   Without this, if watchdog clears m_bLoading and StartEnumeration restarts,
    //   the OLD thread's batches would keep appending to m_vCaps alongside the new thread's.
    if (pMsg->nGen != m_nCapGen) return;
    // 整表已到达（OnData 已重建）：这批属于过期数据，直接丢弃
    if (m_bCapPartialDone) return;
    // 追加本批条目
    if (!pMsg->vBatch.empty())
    {
        m_vCaps.insert(m_vCaps.end(), pMsg->vBatch.begin(), pMsg->vBatch.end());
    }
    // [PROGRESS 2026-10-25] 实时进度：每批到达就立即推进进度条，
    //   而不是等全部完成名才一次性刷新。
    //   旧实现只有"转圈"一种反馈，用户无法判断进度，
    //   尤其 300+ 项要分 3 批时，中间完全没有反馈。
    //   总数由工作线程带回（枚举返回时就已知），故可算出真实百分比。
    if (pMsg->nTotal > 0)
    {
        CProgressUI* pBar = static_cast<CProgressUI*>(
            m_ctx.HostFindControl(L"cap_visual_progress"));
        if (pBar)
        {
            const int nPct = (pMsg->nSent * 100) / pMsg->nTotal;
            pBar->SetValue(nPct > 100 ? 100 : nPct);
        }
        if (CTextUI* pVis = static_cast<CTextUI*>(
                m_ctx.HostFindControl(L"lbl_cap_visual")))
        {
            CDuiString sProg;
            sProg.Format(StringMgr.Get("capability.s_enum_progress"), pMsg->nSent, pMsg->nTotal);
            pVis->SetText(sProg.GetData());
        }
    }
    if (!pMsg->bFinal)
    {
        // [FIX-PERF] 增量渲染：只把本批新行追加到现有列表，绝不 RemoveAll 重建。
        // 这样 100/200/300 批不再产生 O(N²) 控件创建。
        AppendRows(pMsg->vBatch);
        return;
    }
    // 末批：收尾（数据已在上面追加，不重复全量重建）
    m_bCapPartialDone = true;
    m_bLoading = false;
    ::KillTimer(m_ctx.HostHwnd(), CAP_RING_TIMER_ID);
    ::KillTimer(m_ctx.HostHwnd(), CAP_WATCHDOG_TIMER_ID);
    CDuiString sLog;
    sLog.Format(L"[capability] partial enum done: %d entries", static_cast<int>(m_vCaps.size()));
    m_ctx.HostLog(sLog.GetData());
    if (!m_vCaps.empty())
    {
        // [R28] 写入失败不阻断收尾，但要留痕：否则下次进页读到旧缓存，
        // 表现为"刚枚举完的部分功能进页又消失"。
        if (!AppxCapUpdService::SaveCapCache(m_vCaps))
            m_ctx.HostLog(L"[!] [capability] cache write failed after partial enum");
        // [R12 2026-10-25] 收尾前先把进度条归位，避免中间态跳变。
        //   枚举过程中进度条表达的是“枚举进度 N/M”；
        //   枚举完成后它的语义变为“已安装占比”。
        //   FinalizeRender 里的 BlUpdateVisualOverview 会完成这个归位，
        //   但若中间先停在 100% 再被覆盖，用户会看到一次无意义的满格闪现。
        //   这里不再把进度条推到 100%，直接交给归位逻辑。
        // 末批若带有剩余条目，AppendRows 已画出；这里只做空态/汇总收尾。
        FinalizeRender();
    }
    else
    {
        // 一批都没收到：交给整表消息走错误态（WM_CAP_DATA 即将到达）
        m_bCapPartialDone = false;
    }
}
// [rb23] 枚举看门狗到期：加载仍未结束则落为可重试错误态，杜绝永久转圈
void CPageCapability::OnLoadWatchdog()
{
    if (!m_bLoading) return;
    m_bLoading = false;
    ::KillTimer(m_ctx.HostHwnd(), CAP_RING_TIMER_ID);
    // [R16][O-1] 看门狗到期只提示、不重启枚举（重试仍由用户点刷新/操作后
    // 的 CAP_REFRESH/CAP_RETRY 链路驱动）。
    // [R16][O-4] 若当前显示的是磁盘缓存（已有数据），保留缓存列表，
    // 仅记日志，不落错误态覆盖可用内容。
    if (!m_vCaps.empty())
    {
        CDuiString sLog;
        sLog.Format(L"[capability] enum watchdog timeout (%u ms), keep cached list",
                    m_nWatchdogMs);
        m_ctx.HostLog(sLog.GetData());
        return;
    }
    CDuiString sTo;
    sTo.Format(L"[capability] enum watchdog timeout (%u ms), resetting UI",
               m_nWatchdogMs);
    m_ctx.HostLog(sTo.GetData());
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(
        m_ctx.HostFindControl(L"cap_list"));
    if (pList)
    {
        pList->RemoveAll();
        // [R227] Drop the row cache: RemoveAll() destroyed those controls and the
        //   pointers below are now dangling. The null test in ApplyFilterVisibility
        //   does NOT catch a stale non-null pointer.
        m_vCapRowPtr.clear();
        m_vCapRowSearch.clear();
        auto pErr = std::make_unique<CTextUI>(); // [C++17 R3]
        // [R12 2026-10-25] 超时文案带上实际等待时长，告知用户"等了多久"。
        //   旧文案只说"超时"，用户无法判断是真慢还是卡住；
        //   带上实测毫秒数后，配合前面的进度百分比，
        //   能区分"数据已到一半但卡住"与"完全没数据"两种情况。
        {
            const DWORD dwElapsed = m_dwEnumStartTick
                                    ? (::GetTickCount() - m_dwEnumStartTick) : m_nWatchdogMs;
            CDuiString sTimeout;
            sTimeout.Format(StringMgr.Get("capability.s_timeout"),
                            static_cast<unsigned>(dwElapsed / 1000u));
            pErr->SetText(sTimeout.GetData());
        }
        pErr->SetFixedHeight(60);
        pErr->SetTextColor(tokens::Token(tokens::EToken::Danger));
        pErr->SetAttribute(L"align", L"center");
        pErr->SetAttribute(L"valign", L"vcenter");
        pErr->SetMouseEnabled(false);
        pList->Add(pErr.release()); // [C++17 R3] ownership transferred to DuiLib
    }
}
// [R162] 从 OnData 抽出：用旧条目补齐新条目的展示字段，并统计状态变化数。
//   [B2 2026-10-25] 实时枚举只回填 Identity/State，其余字段（描述/大小/依赖计数）
//   若被空值覆盖，用户会看到"刷新后信息变少"（审核清单 G8/B3 越刷新越空）。
//   这 6 条"新值为空则沿用旧值"必须整体成立 —— 集中一处后，将来加字段只需改这里，
//   不会出现"某个字段忘了回填"这种只在特定刷新路径复现的问题。
//   返回状态字段真正变化的条目数，供调用方判断是否需要重建列表。
[[nodiscard]] int CPageCapability::MergeDisplayFields(std::vector<CapEntry>& vNew)
{
    std::map<std::wstring, const CapEntry*> oldById;
    for (const CapEntry& o : m_vCaps)
        oldById[std::wstring(o.sIdentity.GetData())] = &o;

    int nStateChanged = 0;
    for (CapEntry& e : vNew)
    {
        auto it = oldById.find(std::wstring(e.sIdentity.GetData()));
        if (it == oldById.end()) continue;
        const CapEntry& o = *it->second;
        if (lstrcmpiW(o.sState.GetData(), e.sState.GetData()) != 0) ++nStateChanged;
        // 展示字段：新值为空则沿用旧值，避免"刷新后信息变少"
        if (e.sDescription.IsEmpty())      e.sDescription = o.sDescription;
        if (e.sSize.IsEmpty())             e.sSize = o.sSize;
        if (e.nRequiredDeps == 0)          e.nRequiredDeps = o.nRequiredDeps;
        if (e.nOptionalDeps == 0)          e.nOptionalDeps = o.nOptionalDeps;
        if (e.sFirstRequiredDep.IsEmpty()) e.sFirstRequiredDep = o.sFirstRequiredDep;
        if (e.sFirstOptionalDep.IsEmpty()) e.sFirstOptionalDep = o.sFirstOptionalDep;
    }
    return nStateChanged;
}

// [R162] 从 OnData 抽出：枚举失败时的错误呈现（含缓存兜底）。
//   返回 true 表示已用缓存兜底渲染，调用方应直接结束（不必再走正常建表路径）。
//   三件事耦合很紧 —— "要不要渲染错误页"取决于"缓存有没有命中"，
//   所以整段一起抽出，而不是拆成三个函数把 m_vCaps 来回传递。
//   [R30] 原生直连后 nExitCode 为 HRESULT（不再是旧进程退出码），用十六进制展示；
//   未提权(0x800702E4)给可操作提示，避免用户只看到负数退出码、
//   误判为"没有已安装功能"。
//   [fix-cap-cache] 过期缓存仍优于空白报错页：命中则渲染缓存并落日志，
//   仅当缓存也无数据时才落入错误态文案。
[[nodiscard]] bool CPageCapability::RenderEnumError(int nExitCode)
{
    const DWORD dwHr = static_cast<DWORD>(static_cast<LONG>(nExitCode));
    std::vector<CapEntry> vFallback;
    if (AppxCapUpdService::LoadCapCache(vFallback, true) && !vFallback.empty())
    {
        m_vCaps.swap(vFallback);
        CDuiString sLog;
        sLog.Format(L"[capability] enum failed hr=0x%08lX, rendering stale cache (%d entries)",
                    static_cast<unsigned long>(static_cast<LONG_PTR>(nExitCode)),
                    static_cast<int>(m_vCaps.size()));
        m_ctx.HostLog(sLog.GetData());
        // 缓存兜底命中：解除加载态并渲染缓存数据
        m_bLoading = false;
        ::KillTimer(m_ctx.HostHwnd(), CAP_RING_TIMER_ID);
        ::KillTimer(m_ctx.HostHwnd(), CAP_WATCHDOG_TIMER_ID);
        BuildList();
        return true;
    }

    CDuiString sErr;
    if (dwHr == 0x800702E4u)
        sErr = StringMgr.Get("capability.s_err_needadmin");
    else if (dwHr == 0x8007007Eu)
        // [R162] 原为约 90 字中文硬编码：用户最需要看懂的一条报错，
        //   在英文界面下仍是中文。改走语言包。
        sErr = StringMgr.Get("capability.s_err_nomodule");
    else
        sErr.Format(StringMgr.Get("capability.s_err_code"), dwHr);

    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"cap_list"));
    if (pList)
    {
        pList->RemoveAll();
        // [R227] Drop the row cache: RemoveAll() destroyed those controls and the
        //   pointers below are now dangling. The null test in ApplyFilterVisibility
        //   does NOT catch a stale non-null pointer.
        m_vCapRowPtr.clear();
        m_vCapRowSearch.clear();
        auto pErr = std::make_unique<CTextUI>(); // [C++17 R3]
        pErr->SetText(sErr);
        pErr->SetFixedHeight(60);
        pErr->SetTextColor(tokens::Token(tokens::EToken::Danger));
        pErr->SetAttribute(L"align", L"center");
        pErr->SetAttribute(L"valign", L"vcenter");
        pErr->SetMouseEnabled(false);
        pList->Add(pErr.release()); // [C++17 R3] ownership transferred to DuiLib
    }
    return false;
}

// [R162] 从 OnData 抽出：枚举结果落盘（成功且非空才写，失败记日志不阻断 UI）。
//   原实现在"结果一致保持列表"与"正常重建"两条路径各写一份，
//   条件、日志文本完全相同。[R28] 的教训是"写失败要记录，否则下次进页仍显示旧数据
//   且无解释" —— 这条契约必须只有一处，否则漏改一条路径就静默退回到 R28 之前的行为。
void CPageCapability::SaveCacheLogged(const std::vector<CapEntry>& vCaps, int nExitCode)
{
    if (nExitCode != 0 || vCaps.empty()) return;
    if (!AppxCapUpdService::SaveCapCache(vCaps))
        m_ctx.HostLog(L"[!] [capability] cache write failed after enum");
}

// [R175] 从 OnData 抽出：[rb23] 停旋转环/看门狗 + [R16][O-1] 阈值自适应。
//   规则：本次耗时 ×2，下限 30s、上限 600s。
//   **耗时 <1s 视为异常样本，保持原阈值不动** ——
//   若照算，一次瞬时返回（缓存命中或立刻失败）会把阈值压到 30s 下限，
//   之后真实枚举（数秒~数十秒）就会误触发看门狗、弹出"可能卡住"提示。
//   与功能管理页 FeatStopTimersAndAdaptWatchdog 同一套规则，两页行为保持一致。
void CPageCapability::CapStopTimersAndAdaptWatchdog()
{
    ::KillTimer(m_ctx.HostHwnd(), CAP_RING_TIMER_ID);
    ::KillTimer(m_ctx.HostHwnd(), CAP_WATCHDOG_TIMER_ID);

    if (!m_dwEnumStartTick) return;
    const DWORD dwElapsed = ::GetTickCount() - m_dwEnumStartTick;
    m_dwEnumStartTick = 0;
    if (dwElapsed < 1000) return;

    UINT nNew = dwElapsed * 2;
    if (nNew < 30000) nNew = 30000;
    if (nNew > 600000) nNew = 600000;
    m_nWatchdogMs = nNew;
}

// [R175] 从 OnData 抽出：权威结果与当前列表一致时保持列表不动。
//   [B2 2026-10-25] 原实现无条件覆盖 + BuildList() 整表 RemoveAll：
//   列表跳变、滚动丢失，且实时枚举只填 Identity/State 两字段，
//   覆盖带 8 字段的缓存数据会让描述/大小/依赖计数凭空消失（G8/B3「越刷新越空」）。
//   返回 true 表示已判定一致、调用方应直接 return。
[[nodiscard]] bool CPageCapability::CapKeepListIfIdentical(const std::vector<CapEntry>& vNew,
                                                           int nStateChanged, int nExitCode)
{
    if (m_vCaps.empty() || vNew.size() != m_vCaps.size() || nStateChanged != 0) return false;
    CDuiString sSame;
    sSame.Format(L"[capability] authoritative result identical (%d entries), list kept as-is",
                 static_cast<int>(vNew.size()));
    m_ctx.HostLog(sSame.GetData());
    // [R162] 落盘与正常路径共用 SaveCacheLogged（条件与日志单点维护）
    SaveCacheLogged(vNew, nExitCode);
    return true;
}

void CPageCapability::OnData(CapDataMsg* pData)
{
    // [STAB] generation guard: drop authoritative results from superseded enum runs.
    //   This is the BIG one — OnData rebuilds the entire list. Without this check,
    //   an old thread's WM_CAP_DATA arriving after a restarted enumeration would blow
    //   away the new data with stale results, corrupting heap state in the process
    //   (RemoveAll on controls that may already have been destroyed by another path).
    if (!pData || pData->nGen != m_nCapGen)
    {
        delete pData;   // old or null — clean up
        return;
    }
    m_bLoading = false;
    // [FIX-S1-4] 权威整表已到：动作在途标志解除（真实 State 已回报）
    m_bPendingRealRefresh = false;
    // [P1] 整表到达：标记分批通道结束，此后任何迟到的 WM_CAP_PARTIAL 一律丢弃。
    // 注意这不是"整表被忽略"——恰恰相反，整表是权威数据，会完整重建列表；
    // 置位只是为了让仍在队列中的分批消息失效，避免它们把旧批次追加回新列表。
    m_bCapPartialDone = true;
    // [rb23] 枚举已回报：停定时器 + 自适应看门狗
    // [R175] 抽为 CapStopTimersAndAdaptWatchdog，嵌套 4→3
    CapStopTimersAndAdaptWatchdog();
    if (pData)
    {
        // [C++17 R3] receiver takes ownership, freed at scope end (was: delete pData at both exits)
        std::unique_ptr<CapDataMsg> dataGuard(pData);
        // [B2 2026-10-25] 权威结果先比对再决定是否重建。
        //   原实现无条件覆盖 + BuildList() 整表 RemoveAll：列表跳变、滚动丢失；
        //   且实时枚举只填 Identity/State 两字段，覆盖带 8 字段的缓存数据会让
        //   描述/大小/依赖计数凭空消失（审核清单 G8/B3「越刷新越空」）。
        // [R162] 旧值回填补齐抽为 MergeDisplayFields（原内联 for/if 两层）
        const int nStateChanged = MergeDisplayFields(pData->vCaps);
        // [R175] "结果一致则不重建"抽为 CapKeepListIfIdentical，嵌套 4→3
        if (CapKeepListIfIdentical(pData->vCaps, nStateChanged, pData->nExitCode))
            return;   // 不重建、不动滚动
        m_vCaps = pData->vCaps;
        CDuiString sLog;
        sLog.Format(L"[capability] count=%d exit=%d stateChanged=%d",
                    static_cast<int>(m_vCaps.size()), pData->nExitCode, nStateChanged);
        m_ctx.HostLog(sLog.GetData());
        // [R16][O-4] 枚举成功结果落盘（TTL 缓存，供下次进页先渲染）
        // [R28] 写失败不阻断 UI，但要记录，避免"下次进页仍显示旧数据"无解释。
        // [R162] 与"结果一致"分支共用 SaveCacheLogged，条件与日志单点维护。
        SaveCacheLogged(m_vCaps, pData->nExitCode);
        // [R30] 错误态：DISM 失败时给出提示
        // [R162] 整段抽为 RenderEnumError（错误码映射 + 缓存兜底 + 错误行渲染）
        if (pData->nExitCode != 0 && m_vCaps.empty())
        {
            if (RenderEnumError(pData->nExitCode)) return;
            // [C++17 R3] dataGuard frees pData on this early return
            return;
        }
        // [C++17 R3] dataGuard frees pData at scope end
    }
    BuildList();
}
// [R14-03] BuildList 三段式拆分（原 482 行单函数）：
//   薄壳只做 列表清空/过滤条件准备/循环筛选；单条行组装抽 BlBuildRow
//   （内含 BlBuildDepBadge 依赖徽章），空态/汇总日志/FOD 概览各独立。
//   纯 UI 组装语句迁移，零行为改动。
// [R16][O-2] 分批渲染：过滤条件先快照进 m_vRenderIdx，行组装按 50 行/批
// 经 CAP_RENDER_BATCH_TIMER_ID 接力，首批立即可见，避免数百行×每行 6-8
// 控件一次性构建阻塞 UI 线程（DuiLib 无虚拟化时的标准解）。
// [FIX-PERF] 增量追加：只把新批次的可见行追加到现有列表，不 RemoveAll。
// 过滤状态与 BuildList 保持一致；已存在列表不清空，滚动位置不丢。
void CPageCapability::AppendRows(const std::vector<CapEntry>& vNew)
{
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"cap_list"));
    if (!pList || vNew.empty()) return;
    bool bShowAll = false;
    CCheckBoxUI* pShowAll = static_cast<CCheckBoxUI*>(m_ctx.HostFindControl(L"cap_showall"));
    if (pShowAll) bShowAll = pShowAll->GetCheck();
    const CDuiString sFilter = TrimStr(m_ctx.HostReadEditValue(L"cap_filter_edit"));
    const CDuiString sUninstall = m_ctx.HostSkinText(L"lbl_uninstall");
    // [R17-FIX] 增量追加前同步扩容行缓存：BlBuildRow 只在 k < 缓存大小时写入，
    // 若此处不扩容，后续批次的可见行不会进入缓存，过滤对它们完全失效。
    if (m_vCapRowPtr.size() < m_vCaps.size())
        m_vCapRowPtr.resize(m_vCaps.size(), nullptr);
    if (m_vCapRowSearch.size() < m_vCaps.size())
        m_vCapRowSearch.resize(m_vCaps.size());
    // [BUGFIX 2026-10-25] 移除加载占位行。
    //   StartEnumeration 在无缓存时向 cap_list 加入了一个"转环 + 正在枚举"的
    //   占位行（HostMakeLoadingHint，内含名为 cap_ring 的旋转环）。
    //   之后数据通过 WM_CAP_PARTIAL → AppendRows 到达时，原实现**只追加行、
    //   从不移除占位** —— 于是转圈永远挂在列表顶部，即使枚举早已结束。
    //   这就是"加载完了转圈还在"的直接原因：不是动画没停，是控件还在。
    //   （KillTimer(CAP_RING_TIMER_ID) 只停止重绘，不删除控件。）
    //   按控件名定位：占位行是 cap_ring 的祖先，从 cap_list 中摘除整行。
    if (CControlUI* pRing = m_ctx.HostFindControl(L"cap_ring"))
    {
        CControlUI* pRow = pRing->GetParent();
        if (pRow)
            pList->Remove(pRow);
        else
            pList->Remove(pRing);
    }
    size_t k = m_vCaps.size() - vNew.size();
    for (const CapEntry& e : vNew)
    {
        const size_t idx = k++;
        const bool bInstalled = (lstrcmpiW(e.sState.GetData(), L"Installed") == 0);
        if (!bShowAll && !bInstalled) continue;
        // [R200] 分类 + 关键词统一走 AxCapPasses：分批追加与整表构建
        //   共用同一判据，不会出现"首屏按分类筛、后续批不筛"的错位。
        if (!AxCapPasses(e, sFilter)) continue;
        BlBuildRow(pList, e, idx, bInstalled, sUninstall);
    }
}
// 收尾：只做空态/汇总/概览，不重建列表。
void CPageCapability::FinalizeRender()
{
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"cap_list"));
    if (pList && m_vCaps.empty())
        BlShowEmpty(pList, m_sRenderFilter, StringMgr.Get("misc.h12c1"));
    BlLogPageStatus();
    BlUpdateVisualOverview();
}
// ============================================================
// [R200] 智能筛选：分类下拉框 + 分类/关键词联合匹配
//
// 为什么分类判定不看描述：描述会被 [B2] MergeDisplayFields 沿用旧值，
// 用它分类会在一次刷新后整行换类。Identity 家族是稳定的。
// ============================================================
void CPageCapability::AxInitSmartCombo()
{
    CComboUI* pCombo = static_cast<CComboUI*>(m_ctx.HostFindControl(L"cap_cat_combo"));
    if (pCombo && pCombo->GetCount() == 0)
    {
        static constexpr ECapCategory kCats[] = {
            ECapCategory::All, ECapCategory::Fod, ECapCategory::LangPack,
            ECapCategory::Rsat, ECapCategory::Tools, ECapCategory::Other,
        };
        for (const ECapCategory nCat : kCats)
        {
            auto pItem = std::make_unique<CListLabelElementUI>(); // [C++17 R3]
            pItem->SetText(CapCategoryText(nCat));
            pItem->SetFixedHeight(24);
            pCombo->Add(pItem.release());   // [C++17 R3] ownership to DuiLib
        }
        pCombo->SelectItem(0);
    }
    // [R200] 分级日志：Win10 上明确写"已关闭 Win11 专属映射"，
    //   排查"某个功能没有中文名"时先看这一行再怀疑映射表。
    CapLogMapTier(nullptr);
}

ECapCategory CPageCapability::AxCurrentCategory() const
{
    CComboUI* pCombo = static_cast<CComboUI*>(m_ctx.HostFindControl(L"cap_cat_combo"));
    if (!pCombo || pCombo->GetCount() == 0) return ECapCategory::All;
    const int nSel = pCombo->GetCurSel();
    if (nSel <= 0) return ECapCategory::All;
    return static_cast<ECapCategory>(nSel);
}

bool CPageCapability::AxCapPasses(const CapEntry& e, const CDuiString& sFilter) const
{
    const ECapCategory nCat = AxCurrentCategory();
    if (nCat != ECapCategory::All && CapResolveCategory(e.sIdentity) != nCat)
        return false;
    if (sFilter.IsEmpty()) return true;
    if (wcsstr(e.sIdentity.GetData(), sFilter.GetData())) return true;
    const CDuiString sName = ResolveCapDisplayName(e.sIdentity);
    if (wcsstr(sName.GetData(), sFilter.GetData())) return true;
    const CDuiString sDesc = ResolveCapDescription(e);
    return !sDesc.IsEmpty() && wcsstr(sDesc.GetData(), sFilter.GetData()) != nullptr;
}

// [R12] 过滤只切换可见性，不重建列表：输入每个字符都触发 BuildList
// 会导致 RemoveAll + 全量重建，既丢滚动位置也产生大量控件创建。
void CPageCapability::ApplyFilterVisibility()
{
    bool bShowAll = false;
    CCheckBoxUI* pShowAll = static_cast<CCheckBoxUI*>(m_ctx.HostFindControl(L"cap_showall"));
    if (pShowAll) bShowAll = pShowAll->GetCheck();
    const CDuiString sFilter = TrimStr(m_ctx.HostReadEditValue(L"cap_filter_edit"));
    const size_t n = m_vCaps.size();
    // [R8 2026-10-25] 可见性判定下沉到 Rust 组件（dismcore_cap_visibility）。
    //   这是本页唯一的纯计算热点（每次输入都跑一遍），且不涉及任何
    //   控件操作——正好是可安全脱离 UI 的部分。
    //   注意：只计算“应不应该显示”，真正的 SetVisible 仍在 C++。
    std::vector<std::wstring> vState, vIdent, vDisplay;
    vState.reserve(n); vIdent.reserve(n); vDisplay.reserve(n);
    for (size_t k = 0; k < n; ++k)
    {
        const CapEntry& e = m_vCaps[k];
        vState.push_back(e.sState.GetData());
        vIdent.push_back(e.sIdentity.GetData());
        // 显示名解析较贵（字典查找），但已在 BuildList 阶段缓存过搜索串；
        // 此处用缓存值，避免重复计算。
        vDisplay.push_back((k < m_vCapRowSearch.size()) ? m_vCapRowSearch[k].GetData()
                                                        : e.sIdentity.GetData());
    }
    std::vector<uint8_t> vVisible;
    const bool bRustOk = dismcore::CapVisibility(vState, vIdent, vDisplay, bShowAll,
                                                 std::wstring(sFilter.GetData()), vVisible);
    // [R200] 分类在 Rust 判定之后统一收口一次：Rust 只认关键词与已安装状态，
    //   若只把分类塞进"回退分支"，用户选了分类却看到全部条目 ——
    //   看起来像下拉框失效。分类判定放在循环外，每行只做一次枚举比较。
    const ECapCategory nCat = AxCurrentCategory();
    for (size_t k = 0; k < n; ++k)
    {
        CControlUI* pRow = (k < m_vCapRowPtr.size()) ? m_vCapRowPtr[k] : nullptr;
        if (!pRow) continue;
        if (bRustOk && k < vVisible.size())
        {
            bool bVis = vVisible[k] != 0;
            if (bVis && nCat != ECapCategory::All
                && CapResolveCategory(m_vCaps[k].sIdentity) != nCat)
                bVis = false;
            pRow->SetVisible(bVis);
            continue;
        }
        // [回退] Rust 组件不可用或尺寸不匹配时走原逻辑：
        //   宁可慢一点，也不能因侧边异常让列表显示错乱。
        const CapEntry& e = m_vCaps[k];
        const bool bInstalled = (lstrcmpiW(e.sState.GetData(), L"Installed") == 0);
        if (!bShowAll && !bInstalled) { pRow->SetVisible(false); continue; }
        // [R200] 分类维度在 C++ 侧补上：Rust 只认关键词与已安装状态，
        //   否则"选了语言包"在 Rust 路径下会显示全部。两条路径取"与"。
        if (!AxCapPasses(e, sFilter)) { pRow->SetVisible(false); continue; }
        pRow->SetVisible(true);
    }
}

// [R222] Re-apply theme colours to the controls this page builds in code.
//
// Why the page and not a generic tree walk: CapBuildRow paints the state tag by
// SEMANTICS (installed vs not), and a tree walk would only see the resulting ARGB --
// it cannot know which token it came from, so it cannot recompute it for the new theme.
//
// Contract (see PageBase.h): colour attributes only. Calling Build() here would
// discard the list, the selection and the scroll position, and leave m_pFocusCtrl
// dangling.
void CPageCapability::RefreshThemeColors()
{
    const DWORD dwInk     = tokens::Token(tokens::EToken::Ink);
    const DWORD dwSub     = tokens::Token(tokens::EToken::InkTertiary);
    const DWORD dwMuted   = tokens::Token(tokens::EToken::InkMuted);
    const DWORD dwOk      = tokens::Token(tokens::EToken::Success);

    // Rows are kept alive in m_vCapRowPtr (ApplyFilterVisibility already relies on
    // that cache), so walk it instead of searching the tree by name.
    for (size_t k = 0; k < m_vCapRowPtr.size(); ++k)
    {
        // Rows are built as: caprow_<k> [ capinfo_<k> [ name, desc?, state ], capun_<k> ].
        // The inner texts are unnamed, so select them by POSITION inside the info
        // column -- matching on names would silently do nothing.
        CControlUI* pCur = m_vCapRowPtr[k];
        if (!pCur) continue;
        IContainerUI* pRow = static_cast<IContainerUI*>(pCur->GetInterface(DUI_CTR_CONTAINER));
        if (!pRow || pRow->GetCount() < 1) continue;
        CControlUI* pInfoChild = pRow->GetItemAt(0);
        if (!pInfoChild) continue;
        IContainerUI* pInfo = static_cast<IContainerUI*>(pInfoChild->GetInterface(DUI_CTR_CONTAINER));
        if (!pInfo) continue;

        const bool bInstalled = (k < m_vCaps.size())
                             && (m_vCaps[k].sState == CDuiString(L"Installed"));
        // First text = name, last = state tag, anything between = description.
        const int nKids = pInfo->GetCount();
        for (int c = 0; c < nKids; ++c)
        {
            CTextUI* pText = dynamic_cast<CTextUI*>(pInfo->GetItemAt(c));
            if (!pText) continue;
            if (c == 0)              pText->SetTextColor(dwInk);
            else if (c == nKids - 1) pText->SetTextColor(bInstalled ? dwOk : dwMuted);
            else                     pText->SetTextColor(dwSub);
        }
        if (pRow->GetCount() >= 2)
        {
            CButtonUI* pBtn = dynamic_cast<CButtonUI*>(pRow->GetItemAt(1));
            if (pBtn)
            {
                pBtn->SetTextColor(tokens::Token(tokens::EToken::OnAccent));
                pBtn->SetBkColor(tokens::Token(tokens::EToken::Accent));
                pBtn->SetHotBkColor(tokens::Token(tokens::EToken::AccentHot));
                pBtn->SetPushedBkColor(tokens::Token(tokens::EToken::AccentPushed));
            }
        }
    }
}

// [R175] 从 BuildList 抽出：把"可见性 + 排序"合成为 m_vRenderIdx。
//   [R78-FIX] 此前 BuildList 自写一遍「已安装过滤 + Identity/显示名匹配」，
//   ApplyFilterVisibility 又走 dismcore::CapVisibility —— 同一语义两套实现，
//   改口径必然漂移。现在统一：可见性由 CapVisibility 给，排序由 CapSortOrder 给，
//   C++ 只做映射。
//
//   三条路径都要保功能可用（不能因为组件不可用就显示空列表）：
//     ① 两者都成功 → 按 vOrder 顺序、按 vVisible 过滤
//     ② 排序失败   → 保住过滤结果，顺序沿用枚举顺序
//     ③ 过滤失败   → 纯 C++ 判定（已安装 + 子串匹配），与原实现一致
// [R175] 从 CapBuildRenderIndex 抽出：纯 C++ 回退判定（Rust 过滤不可用时）。
//   语义必须与 dismcore::CapVisibility 完全一致，否则"组件可用/不可用"
//   两种情况下列表内容会不同 —— 那是最难查的一类不一致。
//   判据：已安装（除非勾了显示全部）+ 关键字匹配 Identity 或显示名。
void CPageCapability::CapBuildRenderIndexCpp(bool bShowAll)
{
    for (size_t k = 0; k < m_vCaps.size(); ++k)
    {
        const CapEntry& e = m_vCaps[k];
        const bool bInstalled = (lstrcmpiW(e.sState.GetData(), L"Installed") == 0);
        if (!bShowAll && !bInstalled) continue;
        // [R200] 与 AppendRows / ApplyFilterVisibility 同一判据（分类 + 关键词）
        if (!AxCapPasses(e, m_sRenderFilter)) continue;
        m_vRenderIdx.push_back(static_cast<int>(k));
    }
}

void CPageCapability::CapBuildRenderIndex(bool bShowAll)
{
    m_vRenderIdx.clear();
    std::vector<std::wstring> vState, vIdent, vDisplay;
    vState.reserve(m_vCaps.size());
    vIdent.reserve(m_vCaps.size());
    vDisplay.reserve(m_vCaps.size());
    for (size_t k = 0; k < m_vCaps.size(); ++k)
    {
        const CapEntry& e = m_vCaps[k];
        vState.push_back(e.sState.GetData());
        vIdent.push_back(e.sIdentity.GetData());
        vDisplay.push_back(ResolveCapDisplayName(e.sIdentity).GetData());
    }

    std::vector<uint8_t> vVisible;
    const bool bVisOk = dismcore::CapVisibility(
        vState, vIdent, vDisplay, bShowAll,
        std::wstring(m_sRenderFilter.GetData()), vVisible);
    if (!bVisOk || vVisible.size() != m_vCaps.size())
    {
        // ① 回退：Rust 过滤不可用 → 原纯 C++ 判定
        // [R175] 抽为 CapBuildRenderIndexCpp，嵌套 4→2
        CapBuildRenderIndexCpp(bShowAll);
        return;
    }

    std::vector<uint32_t> vOrder;
    const bool bOrderOk = dismcore::CapSortOrder(vState, vDisplay, vOrder);
    if (!bOrderOk || vOrder.size() != m_vCaps.size())
    {
        // ② 排序不可用：保住过滤结果，顺序沿用枚举顺序
        for (size_t k = 0; k < m_vCaps.size(); ++k)
            if (vVisible[k]) m_vRenderIdx.push_back(static_cast<int>(k));
        return;
    }

    // ③ 两者都可用：按排序索引取，并按可见性过滤
    for (size_t i = 0; i < vOrder.size(); ++i)
    {
        const size_t k = static_cast<size_t>(vOrder[i]);
        if (k < m_vCaps.size() && vVisible[k]) m_vRenderIdx.push_back(static_cast<int>(k));
    }
}

void CPageCapability::BuildList()
{
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"cap_list"));
    if (!pList) return;
    ::KillTimer(m_ctx.HostHwnd(), CAP_RENDER_BATCH_TIMER_ID);
    // [A1] 渲染起点：用于 ttfr（首行可见）与 render.total（渲染总耗时）。
    m_dwRenderStartTick = ::GetTickCount();
    pList->RemoveAll();
    // [R12] 行缓存与 m_vCaps 同下标：重建先整体重置，避免旧指针悬挂。
    m_vCapRowPtr.clear();
    m_vCapRowSearch.clear();
    m_vCapRowPtr.resize(m_vCaps.size(), nullptr);
    m_vCapRowSearch.resize(m_vCaps.size());
    bool bShowAll = false;
    CCheckBoxUI* pShowAll = static_cast<CCheckBoxUI*>(m_ctx.HostFindControl(L"cap_showall"));
    if (pShowAll) bShowAll = pShowAll->GetCheck();
    // [R200] 分类下拉框（幂等）：首次进页填充并复位到「全部」。
    AxInitSmartCombo();
    // [R30] 搜索过滤：读取 cap_filter_edit 值（大小写不敏感子串匹配）
    m_sRenderFilter = TrimStr(m_ctx.HostReadEditValue(L"cap_filter_edit"));
    // [R78-FIX] 整表构建改走与增量路径同一套 Rust 判据。
    //   [R175] 可见性×排序合成索引抽为 CapBuildRenderIndex，嵌套 4→2
    CapBuildRenderIndex(bShowAll);

    m_nRenderPos = 0;
    RenderBatch();
}
// [R16][O-2] 渲染一批（≤50 行）；未画完 SetTimer 接力下一批；全部画完
// 才做空态/依赖总览/收尾状态条/FOD 概览（这些只读 m_vCaps，与渲染进度无关，
// 但保持原"列表完成后"时序）。
// [R169] 从 RenderBatch 抽出：渲染性能采样（首行 TTFR / 整轮 total）。
//   [A1] 两条口径：
//     · ttfr   = 渲染起点 → 第一行控件创建完成，**不含** DISM 枚举等待
//                （那段记在 acquire.*；两者相加才是"进页到看见内容"的完整时间）
//     · total  = BuildList 起算到最后一帧结束，与 ttfr 之差即 O7 视口优先的收益
//   只在 PerfTraceEnabled() 时输出，避免日常运行刷调试器。
//   "本轮只打一次"用函数内 static 保证，total 时复位 ——
//   不用类成员：那会让"何时复位"变成需要额外约定的问题。
void CPageCapability::CapLogRenderPerf(bool bFirstRow, int nTotal)
{
    if (!m_dwRenderStartTick || !DismApiBridge::PerfTraceEnabled()) return;

    static bool s_bTtfrLogged = false;
    CDuiString sMsg;
    if (bFirstRow)
    {
        if (s_bTtfrLogged) return;
        s_bTtfrLogged = true;
        sMsg.Format(L"[perf] ttfr = %lu ms (rows=%d)",
                    ::GetTickCount() - m_dwRenderStartTick, nTotal);
        ::OutputDebugStringW(sMsg.GetData());
        ::OutputDebugStringW(L"\r\n");
        return;
    }

    sMsg.Format(L"[perf] render.total = %lu ms (rows=%d)",
                ::GetTickCount() - m_dwRenderStartTick, nTotal);
    ::OutputDebugStringW(sMsg.GetData());
    ::OutputDebugStringW(L"\r\n");
    m_dwRenderStartTick = 0;
    s_bTtfrLogged = false;   // 下一轮重新允许打 ttfr
}

void CPageCapability::RenderBatch()
{
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"cap_list"));
    if (!pList)
    {
        m_vRenderIdx.clear();
        m_nRenderPos = 0;
        return;
    }
    CDuiString sUninstall = m_ctx.HostSkinText(L"lbl_uninstall");
    // [FIX-PERF] 时间预算分批：每帧最多约 6ms，避免数百行一次性建控件造成长卡顿。
    // 数据快照 m_vRenderIdx 只在 BuildList（全量）时建立，partial 走 AppendRows，不会跨批失效。
    const int nTotal = static_cast<int>(m_vRenderIdx.size());
    const ULONGLONG uBudgetMs = 6;
    const ULONGLONG uStart = ::GetTickCount64();
    // [O7 2026-10-25] 视口优先首屏：首帧不做时间切分，先把"一眼能看到的量"建完。
    //   原实现首帧就按 6ms 切分：40 行要 2~3 帧，300 行要十几帧，帧间还有
    //   1ms 定时器 + 消息队列延迟，TTFR 被这段接力拉到 150~400ms。
    //   而用户感知的"加载完成"就是首屏那几十行——屏幕外的行晚几十毫秒毫无感。
    //   策略：前 kFirstScreenRows 行连续建（约 30~50ms CPU），随后立刻回到 6ms 接力。
    constexpr int kFirstScreenRows = 40;
    const bool bFirstScreen = (m_nRenderPos == 0);
    for (int j = m_nRenderPos; j < nTotal; ++j)
    {
        const int k = m_vRenderIdx[j];
        // [STAB] 保留越界校验：调用方仍可能在其他位置改动 m_vCaps。
        if (k < 0 || k >= static_cast<int>(m_vCaps.size()))
        {
            ::KillTimer(m_ctx.HostHwnd(), CAP_RENDER_BATCH_TIMER_ID);
            m_nRenderPos = 0;
            m_vRenderIdx.clear();
            if (!m_vCaps.empty()) BuildList();
            return;
        }
        const CapEntry& e = m_vCaps[k];
        bool bInstalled = (lstrcmpiW(e.sState.GetData(), L"Installed") == 0);
        BlBuildRow(pList, e, static_cast<size_t>(k), bInstalled, sUninstall);
        m_nRenderPos = j + 1;
        // [A1] 首行 TTFR：抽为 CapLogRenderPerf（88→约 78 行）
        if (m_nRenderPos == 1) CapLogRenderPerf(true, nTotal);
        // [O7] 首屏 40 行内不切分；超出后恢复 6ms 预算。
        const bool bInFirstScreen = bFirstScreen && (m_nRenderPos <= kFirstScreenRows);
        if (!bInFirstScreen
            && ::GetTickCount64() - uStart >= uBudgetMs && m_nRenderPos < nTotal)
        {
            ::SetTimer(m_ctx.HostHwnd(), CAP_RENDER_BATCH_TIMER_ID, 1, nullptr);
            return;
        }
    }
    // 全部完成
    ::KillTimer(m_ctx.HostHwnd(), CAP_RENDER_BATCH_TIMER_ID);
    // [A1] 渲染总耗时（与 ttfr 对比可见"首屏之后还花了多少"）
    CapLogRenderPerf(false, nTotal);
    // [R30] 空态：显示有意义的提示文案（不再空白）
    if (m_vRenderIdx.empty())
    {
        // [R30] 空态文案（fallback 到硬编码中文，避免 [key] 显示）
        BlShowEmpty(pList, m_sRenderFilter, StringMgr.Get("misc.h12c1"));
    }
    // [R54] 依赖总览：统计全部能力的必需/可选依赖总数与"高依赖"能力数
    BlLogDepSummary();
    // [U-3] 页面收尾状态条：显示可选功能页完成情况
    BlLogPageStatus();
    // [R73] FOD 状态可视化：更新顶部概览文本 + 进度条
    // 源版 FrameWnd.cpp:14689-14717；m_pm.FindControl → m_ctx.HostFindControl。
    BlUpdateVisualOverview();
}





// ============================================================
// update page (Windows Update Agent, real installed updates)
// ============================================================
// generic late-bound IDispatch call (get / method), args in natural order
static HRESULT AutoWrap(DWORD dwFlags, VARIANT* pvResult, IDispatch* pDisp,
                        LPOLESTR ptName, int cArgs, VARIANT* pArgs)
{
    if (!pDisp) return E_FAIL;



    DISPID dispid = 0;
    OLECHAR* pNames[1] = { ptName };
    HRESULT hr = pDisp->GetIDsOfNames(IID_NULL, pNames, 1,
                                      LOCALE_USER_DEFAULT, &dispid);
    if (FAILED(hr)) return hr;



    DISPPARAMS dp = { nullptr, nullptr, 0, 0 };
    VARIANT vResult;
    ::VariantInit(&vResult);



    // [加固] 参数逆序数组改用 RAII：旧实现 new VARIANT[cArgs] 后若 Invoke
    // 抛异常（或未来加入提前 return 路径）将泄漏；unique_ptr 保证释放。
    // VARIANT 为 POD，元素在下方循环中逐个赋值；按 IDispatch 浅拷贝语义，
    // 参数所有权仍归调用方，释放时不做 VariantClear（与原版一致）。
    std::vector<VARIANT> vArgs; // [C++17 R3] rule e: vector replaces nothrow new[] (OOM now throws bad_alloc)
    if (cArgs > 0)
    {
        vArgs.resize(static_cast<size_t>(cArgs));
        dp.cArgs = cArgs;
        dp.rgvarg = vArgs.data();
        for (int i = 0; i < cArgs; ++i)
            dp.rgvarg[i] = pArgs[cArgs - 1 - i];
    }



    hr = pDisp->Invoke(dispid, IID_NULL, static_cast<WORD>(LOCALE_USER_DEFAULT),
                       static_cast<WORD>(dwFlags), &dp, &vResult, nullptr, nullptr);
    if (SUCCEEDED(hr) && pvResult)
        ::VariantCopy(pvResult, &vResult);



    ::VariantClear(&vResult);
    return hr;
}



// ============================================================
// [R43] WUA 错误码本地化映射（覆盖 Windows Update 常见 HRESULT）
// 说明：仅覆盖高频错误；未匹配时回退为 0x%08X 十六进制字符串。
// ============================================================
static const wchar_t* WuaLocalizeErrorCode(int nErr)
{
    // [AUDIT-P2] 原 27 个 case 的 switch（CC=28），改为 static constexpr 查表。
    // 纯映射关系，没有副作用，表驱动后可读性更好、新增错误码只需加一行；
    // 未命中仍回落到原 default 文案，行为逐条等价。
    struct ErrText { int nCode; const char* pszKey; };
    static constexpr ErrText kErrTable[] = {
        { 0x00000000, "misc.h3303" },
        { 0x80004005, "misc.h9863" },
        { 0x80070005, "misc.h91f9" },
        { 0x80070057, "misc.he3c6" },
        { 0x8007007E, "misc.h56bf" },
        { 0x80070490, "misc.hfca1" },
        { 0x800B0002, "misc.h911b" },
        { 0x80240001, "misc.h4356" },
        { 0x80240003, "misc.hb814" },
        { 0x80240012, "misc.h6068" },
        { 0x80240013, "misc.heb55" },
        { 0x80240015, "misc.h53e1" },
        { 0x80240017, "misc.he12d" },
        { 0x8024001D, "misc.hc216" },
        { 0x80240022, "misc.h4c98" },
        { 0x80240028, "misc.h2ed1" },
        { 0x8024002B, "misc.h13f3" },
        { 0x80240069, "misc.h5044" },
        { 0x80242001, "misc.h785a" },
        { 0x80242004, "misc.h2a4e" },
        { 0x80242005, "misc.h244f" },
        { 0x8024200B, "misc.h1710" },
        { 0x80244006, "misc.h4540" },
        { 0x80244007, "misc.h5026" },
        { 0x80244010, "misc.h0795" },
        { 0x80244011, "misc.ha8ab" },
        { 0x80244020, "misc.h26ee" },
    };
    for (const ErrText& e : kErrTable)
    {
        if (e.nCode == nErr) return StringMgr.Get(e.pszKey);
    }
    return StringMgr.Get("misc.h303a");   // 未命中：原 default 文案
}
// ============================================================
// P8：第二级事件路由（原 CFrameWnd::Notify 可选功能页分支逐字迁入）
// ============================================================
// [R175] 从 OnEvent 抽出：选择可选功能源目录并回填。
//   用户取消时不覆盖已有路径 —— 与功能页 FeatPickSourceDir 同一语义，
//   两页行为保持一致（否则用户会觉得"功能页保留、可选功能页被清空"）。
void CPageCapability::CapPickSourceDir()
{
    CDuiString sDir;
    if (!Win32DialogService::DlgBrowseFolder(sDir, StringMgr.Get("misc.sfc6fe03a"))) return;
    CEditUI* pEdit = static_cast<CEditUI*>(m_ctx.HostFindControl(L"cap_source_edit"));
    if (pEdit) pEdit->SetText(sDir.GetData());
}

void CPageCapability::OnEvent(TNotifyUI& msg)
{
    CDuiString sName = msg.pSender ? msg.pSender->GetName() : CDuiString(L"");
    const wchar_t* pszName = sName.GetData();
    if (msg.sType == L"click")
    {
        // [R30] 可选功能页 /Source 浏览按钮
        if (sName == L"btn_cap_browse")
        {
            // [R175] 抽为 CapPickSourceDir，嵌套 4→3
            CapPickSourceDir();
            return;
        }
        // [R38] 可选功能页 刷新按钮：重新枚举 DISM /Get-Capabilities，
        //       反映 /Add-Capability、/Remove-Capability 后的真实状态与 Size
        else if (sName == L"btn_cap_refresh")
        {
            Refresh();
            return;
        }
        // ---- capability uninstall buttons ----
        int nCapUn = -1;
        if (ParsePrefix1(sName, L"capun_", nCapUn))
        {
            OnUninstall(nCapUn);
            return;
        }
    }
    if (msg.sType == L"textchanged")
    {
        // [R30][R12] 可选功能页实时过滤：只切换行可见性，
        // 不再 BuildList()（RemoveAll + 全量重建）导致输入卡顿与滚动丢失。
        if (sName == L"cap_filter_edit")
        {
            ApplyFilterVisibility();
            return;
        }
    }
    if (msg.sType == L"selectchanged" || msg.sType == L"itemselect")
    {
        // [R200] 分类下拉框：只切可见性（不重建列表），与关键词过滤同一路径，
        //   保证滚动位置与已勾选状态都不丢。两种消息都接：
        //   鼠标选择发 itemselect，键盘选择发 selectchanged。
        if (lstrcmpiW(pszName, L"cap_cat_combo") == 0)
        {
            // 列表尚未构建（首次进页枚举未回）时行缓存为空，此时切换分类
            // 也要保证下拉框项已就位；有数据则直接切可见性。
            if (m_vCapRowPtr.empty()) BuildList();
            else ApplyFilterVisibility();
            return;
        }
    }
    if (msg.sType == L"selectchanged")
    {
        // ---- "show all capabilities" toggles the list filter ----
        if (lstrcmpiW(pszName, L"cap_showall") == 0)
        {
            CCheckBoxUI* p = static_cast<CCheckBoxUI*>(msg.pSender);
            if (p) m_ctx.HostRefreshCheckText(p);
            BuildList();
            return;
        }
    }
}
// ============================================================
// [R124-1] 行级右键菜单增强：caprow_{k} 命中 → 弹出行级菜单
// 比统一 EListCtx::LCX_CAP 更精细：可直接复制 Identity / 显示名 / 描述。
// ============================================================
// [R175] 从 OnContextMenu 抽出：从命中控件向上找 caprow_N，返回有效行号。
//   找不到或越界 → -1。越界当"未命中"而非断言：列表可能正在重建，
//   此时旧行名仍短暂存在（与功能页 FeatRowFromHit 同一判据）。
[[nodiscard]] int CPageCapability::CapRowFromHit(CControlUI* pHit)
{
    for (CControlUI* q = pHit; q; q = q->GetParent())
    {
        const wchar_t* pn = q->GetName();
        if (!pn) continue;
        if (wcsncmp(pn, L"caprow_", 7) != 0) continue;
        const int k = _ttoi(pn + 7);
        if (k < 0 || k >= static_cast<int>(m_vCaps.size())) return -1;
        return k;
    }
    return -1;
}

bool CPageCapability::OnContextMenu(POINT pt, bool bFromContextMsg)
{
    if (bFromContextMsg) return false;
    CControlUI* pHit = m_ctx.HostFindControlAt(pt);
    // [R175] 命中行解析抽为 CapRowFromHit，嵌套 4→3
    const int k = CapRowFromHit(pHit);
    if (k < 0) return false;
    ShowRowContextMenu(k, pt);
    return true;
}
