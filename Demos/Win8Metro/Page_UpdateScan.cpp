// ============================================================
// Page_UpdateScan.cpp —— 更新页「枚举启动 + 扫描计时」（从 Page_Update.cpp 拆出）。
//
// [R197] Page_Update.cpp 拆前 1,004 行、规模分 9.72（六页最低）。本文件承接
// 四个成员，合计 112 行，边界按「一次枚举的启动与耗时统计」划分：
//   · StartEnumeration   启动枚举（离线/缓存先渲染 → 并行 CBS → WUA 线程）
//   · OnScanStarted      记录扫描起始时间戳（供耗时统计）
//   · OnRefreshElapsed   算耗时 + 触发 usoclient StartDownload + 下载尝试计数
//   · LogLastWuaError    输出 WUA 结果码本地化说明
//
// 闭包说明：四个成员声明见 Page_Update.h。跨 TU 依赖六项且都是头文件级：
//   · m_ctx（HostFindControl / HostLog / HostSetCtrlText / HostSkinText / HostStartTool）
//   · m_vUpdList / m_vChecked / m_vpRows / m_bTabLoading / m_bTabLoaded /
//     m_bCbsReady / m_bCbsExpanded / m_vCbsPending / m_vCbsDedup /
//     m_dtScanStart / m_nWuaErr（成员，头文件已声明）
//   · StringMgr（misc.* 语言键，逐字保持，未新增）
//   · COfflineSession::IsActive（OfflineSession.h）
//   · AppxCapUpdService::LoadUpdCache / StartUpdateEnumThread（AppxCapUpdService.h）
//   · WuaLocalizeErrorCode（FrameWnd_Util.h）
//
// 逐字搬运，零逻辑改动。
// ============================================================

#include "stdafx.h"

#include "OfflineSession.h"      // [R10-02] COfflineSession::IsActive
#include "Page_Update.h"

#include "StringManager.h"
#include "AppxCapUpdService.h"   // LoadUpdCache / StartUpdateEnumThread
#include "FrameWnd_Util.h"       // WuaLocalizeErrorCode
#include "CustomControls.h"      // CVerticalLayoutUI

using namespace DuiLib;
using i18n::CStringManager;

// [R197] 扫耗时的粗算单位。OnRefreshElapsed 把 SYSTEMTIME 压成一个可相减的秒数，
// 用的是「年→372 天」的近似（不为闰年做分支，只求量级正确）。原为 6 处裸数字
// 散落在两个 6 项加式里，具名后两式结构一眼可比，也避免只改一处导致量纲不一致。
inline constexpr int kSecPerMinute = 60;
inline constexpr int kSecPerHour   = 3600;
inline constexpr int kDaysPerYear  = 372;   // 近似值（含闰年余量）
inline constexpr int kHoursPerDay  = 24;

// [R175] 从 StartEnumeration 抽出：[性能 A1] 命中 TTL 磁盘缓存则先渲染。
//   stale-while-revalidate：先画缓存让用户立刻看到内容，后台照常静默刷新，
//   数据回来后由上层决定是否重建。
//   命中后置 m_bTabLoaded=true，避免下次进页重复读同一个缓存。
//   离线会话不进本函数（调用方判断）：离线枚举的是目标映像，
//   读在线缓存会把宿主系统的更新列表显示成目标映像的，属串数据。
void CPageUpdate::UpdRenderCacheFirst(int nTab)
{
    std::vector<UpdEntry> vCached;
    if (!AppxCapUpdService::LoadUpdCache(nTab, vCached)) return;

    m_vUpdList[nTab] = vCached;
    m_vChecked[nTab].assign(vCached.size(), 1);
    m_bTabLoaded[nTab] = true;

    CVerticalLayoutUI* pCacheList = static_cast<CVerticalLayoutUI*>(
        m_ctx.HostFindControl(nTab == 0 ? L"upd_pending_list" : L"upd_list"));
    if (!pCacheList) return;
    pCacheList->RemoveAll();
    m_vpRows[nTab].clear();
    UpdStartBatchRender(pCacheList, nTab, false);
}

void CPageUpdate::StartEnumeration(int nTab)
{
    if (m_bTabLoading[nTab]) return;
    // [R3-FIX] generation 守卫：每次 StartEnumeration 递增一次，
    // 透传到工作线程 → 所有回传消息；UI 侧 OnData/OnPartial 比对当前 generation，
    // 过期消息（上一次枚举）直接丢弃，防止"快速切 Tab 枚举回传错乱"。
    ++m_nUpdGen;
    const int nGen = m_nUpdGen;
    // [性能 A1] 在线态且该 Tab 尚无数据：命中 TTL 磁盘缓存则先渲染缓存，
    // 后台继续静默刷新（stale-while-revalidate）；离线会话不读写缓存，
    // 杜绝离线/在线串数据。
    // [R175] 缓存先渲染抽为 UpdRenderCacheFirst，嵌套 4→2
    if (!COfflineSession::IsActive() && !m_bTabLoaded[nTab])
        UpdRenderCacheFirst(nTab);
    m_bTabLoading[nTab] = true;
    // U-2 检索条件按 Tab 分派：Tab0=待安装, Tab1=已安装
    CDuiString sCrit;
    if (nTab == 0)
        sCrit = L"IsInstalled=0 and IsHidden=0";
    else
        sCrit = L"IsInstalled=1 and IsHidden=0";
    m_ctx.HostSetCtrlText(L"upd_wu_hint",
                nTab == 0 ? StringMgr.Get("cleanup.s02c692c1")
                          : m_ctx.HostSkinText(L"lbl_wu_hint"));
    // [性能 A2] CBS 枚举与 WUA 并行启动（原为 WUA 渲染完成后串行触发），
    // 结果先暂存，待 WUA 渲染完成后去重并入。
    if (nTab == 1)
    {
        ResetCbsState();
        m_vCbsPending.clear();
        m_vCbsDedup.clear();
        StartCbsEnumThread();
    }
    AppxCapUpdService::StartUpdateEnumThread(m_ctx.HostHwnd(), nTab, sCrit, nGen);
}

void CPageUpdate::OnScanStarted()
{
    // [R43] 记录扫描起始时间戳，便于事后计算耗时
    SYSTEMTIME st; ::GetLocalTime(&st);
    MarkScanStart(st);   // [R44] 保存扫描起始时间用于耗时统计
    CDuiString sScan;
    sScan.Format(StringMgr.Get("misc.h68aa"),
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    m_ctx.HostLog(sScan.GetData());
}

void CPageUpdate::OnRefreshElapsed()
{
    // [R44] 计算扫描耗时（若已启动扫描），日志输出
    SYSTEMTIME stNow; ::GetLocalTime(&stNow);
    if (m_dtScanStart.wYear > 0)
    {
        ULONGLONG lStart = static_cast<ULONGLONG>(m_dtScanStart.wYear) * kDaysPerYear * kHoursPerDay * kSecPerHour
                          + m_dtScanStart.wMonth * kHoursPerDay * kSecPerHour
                          + m_dtScanStart.wDay * kSecPerHour
                          + m_dtScanStart.wHour * kSecPerHour
                          + m_dtScanStart.wMinute * kSecPerMinute
                          + m_dtScanStart.wSecond;
        ULONGLONG lNow   = static_cast<ULONGLONG>(stNow.wYear) * kDaysPerYear * kHoursPerDay * kSecPerHour
                          + stNow.wMonth * kHoursPerDay * kSecPerHour
                          + stNow.wDay * kSecPerHour
                          + stNow.wHour * kSecPerHour
                          + stNow.wMinute * kSecPerMinute
                          + stNow.wSecond;
        ULONGLONG nElapsed = (lNow > lStart) ? (lNow - lStart) : 0;
        if (nElapsed > 0)
        {
            CDuiString sElapsed;
            sElapsed.Format(StringMgr.Get("misc.he5c7"),
                            nElapsed, nElapsed / 60);
            m_ctx.HostLog(sElapsed.GetData());
        }
        // [R53] 扫描启动后刷新按钮 → 主动触发 usoclient StartDownload（下载链）
        // 用户显式按下 refresh 即视为对上一次扫描结果的下载确认
        m_ctx.HostLog(StringMgr.Get("misc.hf156"));
        // [R56] 下载尝试次数递增（供用户/日志侧了解已尝试多少次下载）
        {
            static int s_nDownloadAttempt = 0;
            ++s_nDownloadAttempt;
            CDuiString sAtt;
            sAtt.Format(StringMgr.Get("misc.h4d06"),
                        s_nDownloadAttempt);
            m_ctx.HostLog(sAtt.GetData());
        }
        m_ctx.HostStartTool(L"usoclient.exe", L"StartDownload");
        m_dtScanStart.wYear = 0;   // 重置
    }
}

void CPageUpdate::LogLastWuaError()
{
    // [R43] 刷新时同步输出 WUA 结果码本地化说明（若有）
    if (m_nWuaErr != 0)
    {
        CDuiString sErr;
        sErr.Format(StringMgr.Get("misc.h2ea4"),
                    (unsigned int)m_nWuaErr,
                    WuaLocalizeErrorCode(m_nWuaErr));
        m_ctx.HostLog(sErr.GetData());
    }
}
