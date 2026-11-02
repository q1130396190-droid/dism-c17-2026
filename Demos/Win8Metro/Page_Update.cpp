#include "stdafx.h"

#include "OfflineSession.h"  // [R10-02] 离线会话经独立组件访问，去 CDataEngine 静态耦合

// ============================================================
// Page_Update.cpp —— v2 P4 Windows 更新页
// 6 个原 CFrameWnd 函数物理搬迁，函数体零逻辑改动，宿主引用
// 改 m_ctx.Xxx；后台 WUA 枚举/卸载在 AppxCapUpdService（S6）。
// ============================================================

#include "Page_Update.h"
#include "CustomControls.h"
#include "StringManager.h"
#include "FrameWnd_Util.h"
#include "FrameWnd_UiHelpers.h"  // P8：EListCtx::LCX_UPDATE / 统一列表右键菜单
#include "FrameWnd_Messages.h"
#include "ProcessRunner.h"       // [R74] RunHiddenExit
#include "AppxCapUpdService.h"  // [R80] StartUpdateInstallThread
#include "UpdateMetaService.h"  // [R-B8] UpdateInfo 四字段 / CBS 独占对话框 / wsusscn 随包
#include "ConfigReader.h"       // [R-B50] config.ini [Update] MaxThread 读写
#include "UpdPhaseRelay.h"      // [R-B51] 阶段状态文件 + 命名事件回传
#include "ToolDialogPage.h"         // [R-B51] EToolDlgKind::TD_LOGVIEW 失败定位
#include "DataEngine.h"         // [R-B76] IsOfflineSession/GetOfflineImageRoot
#include "FrameWnd_Messages.h"  // [R-B76] WM_UPD_DATA
#include <commdlg.h>            // [R-B76] GetOpenFileNameW
#include <process.h>             // [R-B76] _beginthreadex
#include "SafeThread.h"          // CPU 密集工作线程异常边界

using namespace DuiLib;
using i18n::CStringManager;

// [C++17 R6] 过滤框防抖窗口：连续输入只在停手后过滤一次，
// 避免每敲一个字符就重建数百行列表。
inline constexpr UINT kUpdFilterDebounceMs = 250;

// [R168] 从 ApplyFilter 抽出：单个列表容器内的关键字匹配与行显隐。
// 返回本容器内命中的 updrow_ 行数，nTotal 累加实际数据行数（不含空态/折叠行）。
// 判定与逐字迁移前一致：非 updrow_ 前缀行按"有关键字即隐藏"处理，
// updrow_ 行解析序号 k 后用本 Tab 的 m_vUpdList[nTab][k].sTitle 做包含匹配。
int CPageUpdate::UpdApplyFilterList(int nTab, const wchar_t* pszListName,
                                   const CDuiString& sKw, const CDuiString& sKwLower,
                                   int& nTotal)
{
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(pszListName));
    if (!pList) return 0;
    int nHit = 0;
    for (int r = 0; r < pList->GetCount(); ++r)
    {
        CControlUI* pRow = pList->GetItemAt(r);
        if (!pRow) continue;
        const wchar_t* pszN = pRow->GetName();
        if (!pszN || wcsncmp(pszN, L"updrow_", 7) != 0)
        {
            // 空态 Text 行 / CBS 行 / 折叠开关行等：有关键字时隐藏
            pRow->SetVisible(sKw.IsEmpty());
            if (pRow->IsVisible()) ++nHit;
            continue;
        }
        ++nTotal;
        if (sKw.IsEmpty())
        {
            pRow->SetVisible(true);
            ++nHit;
            continue;
        }
        // 解析 updrow_%d 中的 k，用本 Tab 的 m_vUpdList[nTab][k] 匹配 sTitle
        int k = _ttoi(pszN + 7);
        bool bHit = false;
        if (k >= 0 && k < static_cast<int>(m_vUpdList[nTab].size()))
        {
            CDuiString sTitle = m_vUpdList[nTab][k].sTitle;
            sTitle.MakeLower();
            bHit = (sTitle.Find(sKwLower.GetData()) >= 0);
        }
        pRow->SetVisible(bHit);
        if (bHit) ++nHit;
    }
    return nHit;
}

void CPageUpdate::ApplyFilter()
{
    CDuiString sKw = m_ctx.HostReadEditValue(L"upd_filter");
    CDuiString sKwLower = sKw;
    sKwLower.MakeLower();
    int nShown = 0, nTotal = 0;
    // [性能 C3] 遍历两个 Tab 的列表容器，匹配各自独立的 m_vUpdList[n]
    //（修复原共享缓存导致的跨 Tab 过滤错位）
    static const wchar_t* kLists[] = { L"upd_pending_list", L"upd_list" };
    for (int n = 0; n < 2; ++n)
        nShown += UpdApplyFilterList(n, kLists[n], sKw, sKwLower, nTotal);
    // 强制重排：隐藏行后列表容器需重新计算位置
    for (int n = 0; n < 2; ++n)
    {
        CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(kLists[n]));
        if (pList) pList->SetPos(pList->GetPos());
    }
    UpdateCount(nShown, nTotal);
}

void CPageUpdate::UpdateCount(int nShown, int nTotal)

{
    CTextUI* pCount = static_cast<CTextUI*>(m_ctx.HostFindControl(L"lbl_upd_count"));
    if (!pCount) return;
    if (nTotal < 0) nTotal = nShown;
    CDuiString s;
    if (nTotal == 0) s = StringMgr.Get("misc.hfc81");
    else if (nShown == nTotal) s.Format(StringMgr.Get("misc.hadf7"), nTotal);
    else s.Format(StringMgr.Get("misc.h5a5c"), nTotal, nShown);
    pCount->SetText(s.GetData());
}

// [R3] F5 刷新：更新页仅记录日志（保持原壳 switch 行为）
void CPageUpdate::OnRefreshPage()
{
    m_ctx.HostLog(StringMgr.Get("misc.h1f56"));
}

// [R3] 懒构建：默认选中“已安装的更新”子页后发起枚举
// （原壳 EnsurePageBuilt PAGE_UPDATE 分支，逐字保留语义，区别于 Build()
//  的崩溃过滤器安装/线程数列构建/策略按钮刷新序列）。
void CPageUpdate::OnDeferredBuild()
{
    CTabLayoutUI* pUpdPages = static_cast<CTabLayoutUI*>(m_ctx.HostFindControl(L"upd_pages"));
    const int nTab = (m_nCurTab < 0) ? 1 : m_nCurTab;   // [STAB] Tab 是用户状态，仅首次默认 1
    if (pUpdPages) pUpdPages->SelectItem(nTab);
    m_nCurTab = nTab;
    UpdateBottomBar();
    StartEnumeration(nTab);
}


// [R175] 从 OnData（51 行 / CC13）抽出：按 nKind 分派到 9 种数据消息处理器。
// 返回 true 表示本消息已被某个处理器消费（调用方须立即返回，不再走常规渲染）。
// 分组与原表一一对应：1 卸载 / 2、3 安装 / 4、5、6 隐藏 / 7 CBS / 8、9 本地补丁。
// 表放成员函数内：取私有成员函数地址在类作用域外非法。
bool CPageUpdate::UpdRouteByKind(UpdDataMsg* pData)
{
    using UpdKindHandler = void (CPageUpdate::*)(UpdDataMsg*);
    struct UpdKindRoute { int nKind; UpdKindHandler pfn; };
    static constexpr UpdKindRoute kRoutes[] = {
        { 1, &CPageUpdate::UpdHandleUninstall },   // [修复 R5 / P1-15] 卸载结果回传，不重建列表
        { 2, &CPageUpdate::UpdHandleInstall },     // [R80] WUA 正向安装完成
        { 3, &CPageUpdate::UpdHandleInstall },     // [R80] WUA 正向安装失败
        { 4, &CPageUpdate::UpdHandleHide },        // [R82] WUA 隐藏/取消隐藏完成
        { 5, &CPageUpdate::UpdHandleHide },
        { 6, &CPageUpdate::UpdHandleHide },
        { 7, &CPageUpdate::UpdHandleCbs },         // [R-B76] CBS /Get-Packages 并入已安装列表
        { 8, &CPageUpdate::UpdHandleLocalPatch },  // [R-B76] 本地补丁进度日志
        { 9, &CPageUpdate::UpdHandleLocalPatch },  // [R-B76] 批次结果弹窗
    };
    if (!pData) return false;
    for (const UpdKindRoute& r : kRoutes)
    {
        if (r.nKind != pData->nKind) continue;
        (this->*r.pfn)(pData);
        return true;
    }
    return false;
}

// [R175] 从 OnData 抽出：常规枚举结果的列表重建（清空 → 渲染行或空态 → CBS 并入机会）。
void CPageUpdate::UpdRenderDataList(UpdDataMsg* pData)
{
    CVerticalLayoutUI* pList = UpdResolveList(pData);
    if (!pList) return;
    const int nTab = (pData && pData->nTab == 0) ? 0 : 1;
    pList->RemoveAll();
    m_vpRows[nTab].clear();
    if (pData && !pData->vUpd.empty())
    {
        UpdRenderRows(pList, pData);   // [性能 B1] 内部转分批渲染接力
        return;
    }
    UpdRenderEmptyState(pList, pData);
    // [性能 A2] 空列表也要给 CBS 并入机会
    if (nTab == 1 && m_bCbsReady) UpdMergeCbs();
}

void CPageUpdate::OnData(UpdDataMsg* pData)
{
    // [R3-FIX] generation 守卫：过期消息（上一次枚举）直接丢弃
    if (pData && pData->nGen != 0 && pData->nGen != m_nUpdGen)
    {
        std::unique_ptr<UpdDataMsg> guard(pData);
        m_ctx.HostLog(L"[update] stale UpdDataMsg dropped (gen mismatch)");
        return;
    }
    // [性能 C3] 在途标记按消息所属 Tab 清除；[P1] 整表到达即结束分批通道（迟到的分批消息将被丢弃）
    if (pData) { const int nT = (pData->nTab == 0) ? 0 : 1; m_bTabLoading[nT] = false; m_bUpdPartialDone[nT] = true; }
    // [R146] nKind→处理器分派表（[R175] 已抽为 UpdRouteByKind）
    if (UpdRouteByKind(pData)) return;
    // 常规枚举结果：缓存列表、默认全选、重置状态
    UpdPrepareState(pData);
    // 按 nTab 解析列表容器并隐藏占位
    UpdRenderDataList(pData);
    // 统计日志 + 错误记录 + 页面状态条
    UpdLogStats(pData);
}

void CPageUpdate::UpdHandleUninstall(UpdDataMsg* pData)
{
{
    CDuiString sLog;
    if (pData->nResultCode == 2 /* orcSucceeded */ && pData->nUnHr == 0)
        sLog = StringMgr.Get("update.s66bac223");
    else
        sLog.Format(StringMgr.Get("error.s2ca410e5"),
                    pData->nResultCode, (unsigned long)pData->nUnHr);
    m_ctx.HostLog(sLog.GetData());
    m_ctx.HostAlert(StringMgr.Get("update.sc47cec27"), sLog.GetData(),
                  pData->nResultCode == 2 ? MB_ICONINFORMATION
                                          : MB_ICONERROR);
    // [性能 A1] 卸载后失效磁盘缓存，重新枚举让列表反映真实状态
    if (pData->nResultCode == 2 && pData->nUnHr == 0)
        AppxCapUpdService::InvalidateUpdCache();
    m_bTabLoaded[1] = false;
    StartEnumeration(1);
    // [C++17 R3] 消息接收端 unique_ptr 接管，所有权由投递方移交
    std::unique_ptr<UpdDataMsg> spData(pData);
    return;
}
}

void CPageUpdate::UpdHandleInstall(UpdDataMsg* pData)
{
{
    OnInstallDone(pData->nKind, pData->nErr,
                  pData->nResultCode, pData->nUnHr);
    // [C++17 R3] 消息接收端 unique_ptr 接管，所有权由投递方移交
    std::unique_ptr<UpdDataMsg> spData(pData);
    return;
}
}

void CPageUpdate::UpdHandleHide(UpdDataMsg* pData)
{
{
    OnHideDone(pData->nKind, pData->nErr,
               pData->nResultCode, pData->nUnHr);
    // [C++17 R3] 消息接收端 unique_ptr 接管，所有权由投递方移交
    std::unique_ptr<UpdDataMsg> spData(pData);
    return;
}
}

void CPageUpdate::UpdHandleCbs(UpdDataMsg* pData)
{
{
    OnCbsPackages(pData);
    // [C++17 阶段3] 消息接收端 unique_ptr 接管，所有权由投递方移交
    std::unique_ptr<UpdDataMsg> spData(pData);  // [C++17 R3] unique_ptr 接管
    return;
}
}

void CPageUpdate::UpdHandleLocalPatch(UpdDataMsg* pData)
{
{
    if (!pData->sMsg.IsEmpty()) m_ctx.HostLog(pData->sMsg);
    if (pData->nKind == 9)
    {
        m_ctx.HostAlert(L"本地补丁安装", pData->sMsg.GetData(),
                        pData->nResultCode == 0 ? MB_ICONINFORMATION
                                                : MB_ICONWARNING);
        // [性能 A1] 本地补丁改变已安装状态：失效缓存后刷新"已安装"列表
        AppxCapUpdService::InvalidateUpdCache();
        m_bTabLoaded[1] = false;
        StartEnumeration(1);
    }
    // [C++17 阶段3] 消息接收端 unique_ptr 接管，所有权由投递方移交
    std::unique_ptr<UpdDataMsg> spData(pData);  // [C++17 R3] unique_ptr 接管
    return;
}
}

void CPageUpdate::UpdPrepareState(UpdDataMsg* pData)
{
    if (pData)
    {
        // [性能 C3] 结果写入所属 Tab 的独立缓存
        int nTab = (pData->nTab == 0) ? 0 : 1;
        m_vUpdList[nTab] = pData->vUpd;
        m_bTabLoaded[nTab] = true;
        // [功能补齐] 列表重建后默认全部勾选（与"全选"按钮语义一致）
        m_vChecked[nTab].assign(m_vUpdList[nTab].size(), 1);
        // [性能 A1] 在线态枚举成功后落盘（离线会话不写，杜绝串数据）
        // [R30] 与 R27/R28 同一原则：写缓存失败不阻断 UI，但必须留痕，
        // 否则下次进页读到旧更新列表，表现为"刚扫到的更新又没了"。
        if (pData->nKind == 0 && !COfflineSession::IsActive())
        {
            if (!AppxCapUpdService::SaveUpdCache(nTab, m_vUpdList[nTab]))
                m_ctx.HostLog(L"[!] [update] cache write failed after enum");
        }
        // [性能 A2] CBS 已改与 WUA 并行启动（见 StartEnumeration），
        // 此处不再串行触发；若 CBS 结果先回传，由分批渲染收尾统一并入。
    }
    m_nSelected = -1;
    SetWuaErr(0);
    MarkScanStart(SYSTEMTIME{});
}


void CPageUpdate::OnRowSelect(int n)
{
    std::vector<UpdEntry>& vList = CurList();
    std::vector<char>& vChecked = CurChecked();
    if (n < 0 || n >= static_cast<int>(vList.size())) return;
    // [功能补齐] 单击行切换该行勾选；m_nSelected 记录最后单击行（单项操作用）
    if (static_cast<int>(vChecked.size()) != static_cast<int>(vList.size()))
        vChecked.assign(vList.size(), 1);
    vChecked[n] = vChecked[n] ? 0 : 1;
    m_nSelected = n;
    // [性能 C2] 高亮：行指针直接索引，仅重绘被点击行（原全量 HostFindControl
    // 名称查找 O(N²) → O(1)；其余行颜色仅随自身点击变化，无需全刷）
    if (n < static_cast<int>(m_vpRows[m_nCurTab].size()) && m_vpRows[m_nCurTab][n])
        m_vpRows[m_nCurTab][n]->SetBkColor(vChecked[n] ? tokens::Token(tokens::EToken::AccentSoft)
                                                   : tokens::Token(tokens::EToken::Surface));
    {
        CDuiString sLog;
        sLog.Format(L"[update] %s #%d \"%s\" id=%s kb=%s",
                    vChecked[n] ? L"checked" : L"unchecked",
                    n,
                    vList[n].sTitle.GetData(),
                    vList[n].sUpdateID.GetData(),
                    vList[n].sKB.GetData());
        m_ctx.HostLog(sLog.GetData());
    }
}

// [功能补齐] btn_upd_selall：全部已勾选则全不选，否则全部勾选
// [R187] 从 OnSelectAll（27 行 / CC12）抽出：判断当前是否「全部已勾选」。
// 空表返回 true（视为全选态）—— 与调用方「空表已在前面 return」的实际路径不冲突，
// 但保留该值语义可让本函数独立可测。
[[nodiscard]] bool CPageUpdate::UpdIsAllChecked(const std::vector<char>& vChecked)
{
    for (size_t i = 0; i < vChecked.size(); ++i)
        if (!vChecked[i]) return false;
    return true;
}

// [R187] 从 OnSelectAll 抽出：批量置勾选态 + 同步可见行的底色。
// 勾选数组先按列表长度对齐（不足则全置 1），避免两数组长度错位导致批量安装/隐藏
// 作用到错误的条目。行底色只是视觉反馈：0=未选（白）/1=已选（浅蓝）。
void CPageUpdate::UpdApplyCheckedAll(const std::vector<UpdEntry>& vList,
                                     std::vector<char>& vChecked, char cSet)
{
    if (static_cast<int>(vChecked.size()) != static_cast<int>(vList.size()))
        vChecked.assign(vList.size(), 1);
    for (size_t i = 0; i < vChecked.size(); ++i) vChecked[i] = cSet;
    // [性能 C2] 行指针直接索引，替代逐行 HostFindControl 名称查找
    const size_t nRows = m_vpRows[m_nCurTab].size();
    for (size_t k = 0; k < vList.size() && k < nRows; ++k)
    {
        CControlUI* pRow = m_vpRows[m_nCurTab][k];
        if (pRow) pRow->SetBkColor(cSet ? tokens::Token(tokens::EToken::AccentSoft)
                                    : tokens::Token(tokens::EToken::Surface));
    }
}

void CPageUpdate::OnSelectAll()
{
    std::vector<UpdEntry>& vList = CurList();
    std::vector<char>& vChecked = CurChecked();
    if (vList.empty())
    {
        m_ctx.HostToast(StringMgr.Get("update.sc47cec27"),
                        StringMgr.Get("update.s962c764b"), 1);
        return;
    }
    // [R187] 「是否已全选」与「批量置位 + 行底色调」各抽为函数（原内联，CC 记账 6 点）
    const bool bAllOn = UpdIsAllChecked(vChecked);
    const char cSet  = bAllOn ? 0 : 1;
    UpdApplyCheckedAll(vList, vChecked, cSet);
    m_ctx.HostLog(bAllOn ? L"[update] select-all: unchecked all"
                         : L"[update] select-all: checked all");
}

void CPageUpdate::StartUninstall(int n)

{
    if (n < 0 || n >= static_cast<int>(CurList().size())) return;
    const CDuiString sTargetId = CurList()[n].sUpdateID;
    if (sTargetId.IsEmpty())
    {
        m_ctx.HostAlert(StringMgr.Get("update.sc47cec27"),
            StringMgr.Get("error.s858fdeb4"), MB_ICONWARNING);
        return;
    }
    CDuiString sAsk;
    sAsk.Format(StringMgr.Get("update.s966de14f"),
                CurList()[n].sTitle.GetData());
    if (!m_ctx.HostConfirmWarn(StringMgr.Get("update.s8d0c4800"), sAsk.GetData()))
        return;
    AppxCapUpdService::StartUpdateUninstallThread(m_ctx.HostHwnd(), sTargetId);
}

// ============================================================
// 以下四方法承接原 CFrameWnd 通知区 btn_upd_scan / btn_upd_refresh /
// btn_upd_delete 处理中与本页状态耦合的片段（扫描时间戳、下载尝试
// 计数、WUA 错误码、选中行守卫），函数体零逻辑改动。
// ============================================================


void CPageUpdate::OnUninstallButton()
{
    // [修复 R5 / P1-15c] ① 未选中任何更新行时给出明确提示；
    //           ② 选中后交由 StartUninstall() 走 WUA 安装器卸载。
    if (m_nSelected < 0 || m_nSelected >= static_cast<int>(CurList().size()))
    {
        m_ctx.HostAlert(StringMgr.Get("update.sc47cec27"),
                        StringMgr.Get("update.s962c764b"), MB_ICONINFORMATION);
        return;
    }
    StartUninstall(m_nSelected);
}

// ============================================================
// [R80] WUA 正向安装/下载：btn_upd_install → 后台线程 → 进度/结果回传
// ============================================================

void CPageUpdate::OnInstallButton()
{
    // [功能补齐] 仅收集勾选的未安装项（Tab0 待安装列表）UpdateID
    // [R127-A] 收集与空态提示提取为 CollectCheckedUpdateIds
    std::vector<CDuiString> vIDs;
    if (!CollectCheckedUpdateIds(vIDs)) return;
    // 确认弹窗
    CDuiString sAsk;
    sAsk.Format(StringMgr.Get("update.s_install_confirm"), static_cast<int>(vIDs.size()));
    if (!m_ctx.HostConfirmWarn(StringMgr.Get("update.s_install_title"), sAsk.GetData()))
        return;
    m_ctx.HostSetCtrlText(L"upd_install_state",
                          StringMgr.Get("update.s_install_preparing"));
    m_ctx.HostLog(StringMgr.Get("update.s_install_start"));
    // [R97] 安装入口统一走 InstallUpdates（内部 = StartUpdateInstallThread 语义）
    // [R-B50] 传入当前生效的下载线程上限（config.ini > UpdateInfo > 默认 10）
    AppxCapUpdService::InstallUpdates(m_ctx.HostHwnd(), vIDs, true,
                                      GetEffectiveMaxThread());
}

void CPageUpdate::OnInstallProgress(int nPercent, int nPhase,
                                    int nItemIndex, int nItemCount)
{
    CDuiString sPhase = (nPhase == 0)
        ? StringMgr.Get("update.s_install_downloading")
        : StringMgr.Get("update.s_install_installing");
    CDuiString sState;
    sState.Format(StringMgr.Get("update.s_install_progress"),
                  sPhase.GetData(), nItemIndex, nItemCount, nPercent);
    m_ctx.HostSetCtrlText(L"upd_install_state", sState.GetData());
}

void CPageUpdate::OnInstallDone(int nKind, int nErr, int nResultCode, int nHr)
{
    CDuiString sLog;
    if (nKind == 2)
    {
        sLog.Format(StringMgr.Get("update.s_install_done"), nResultCode, nHr);
        m_ctx.HostLog(sLog.GetData());
        m_ctx.HostToast(StringMgr.Get("update.s_install_title"),
                        StringMgr.Get("update.s_install_done_short"), 0);
        m_ctx.HostSetCtrlText(L"upd_install_state",
                              StringMgr.Get("update.s_install_done_short"));
        // [性能 A1] 安装改变更新状态：失效磁盘缓存后重新枚举，反映真实状态
        AppxCapUpdService::InvalidateUpdCache();
        m_bTabLoaded[0] = false;
        m_bTabLoaded[1] = false;
        StartEnumeration(0);
    }
    else
    {
        sLog.Format(StringMgr.Get("update.s_install_fail"), nErr, nResultCode, nHr);
        m_ctx.HostLog(sLog.GetData());
        m_ctx.HostToast(StringMgr.Get("update.s_install_title"),
                        StringMgr.Get("update.s_install_fail_short"), 1);
        m_ctx.HostSetCtrlText(L"upd_install_state",
                              StringMgr.Get("update.s_install_fail_short"));
    }
    // [R-B78] UpdateReport 回传（尽力而为，失败排队 outbox，不影响交互）
    PostInstallResultReport(nKind, nErr, nResultCode, nHr);
}


void CPageUpdate::OnHideDone(int nKind, int nErr, int nResultCode, int nHr)
{
    CDuiString sLog;
    if (nKind == 4 || nKind == 5)
    {
        // nResultCode = 成功处理条数, nHr = 目标条数
        bool bHide = (nKind == 4);
        CDuiString sTmpl = bHide
            ? StringMgr.Get("update.s_hide_done")
            : StringMgr.Get("update.s_unhide_done");
        sLog.Format(sTmpl, nResultCode, nHr);
        m_ctx.HostLog(sLog.GetData());
        m_ctx.HostToast(StringMgr.Get("update.s_hide_title"),
                        bHide ? StringMgr.Get("update.s_hide_done_short")
                              : StringMgr.Get("update.s_unhide_done_short"), 0);
        m_ctx.HostSetCtrlText(L"upd_install_state",
                              StringMgr.Get("update.s_hide_done_short"));
        // [性能 A1] 隐藏/取消隐藏改变状态：失效磁盘缓存后重新枚举待安装列表
        AppxCapUpdService::InvalidateUpdCache();
        m_bTabLoaded[0] = false;
        m_bTabLoaded[1] = false;
        StartEnumeration(0);
    }
    else
    {
        sLog.Format(StringMgr.Get("update.s_hide_fail"), nErr, nResultCode, nHr);
        m_ctx.HostLog(sLog.GetData());
        m_ctx.HostToast(StringMgr.Get("update.s_hide_title"),
                        StringMgr.Get("update.s_hide_fail_short"), 1);
        m_ctx.HostSetCtrlText(L"upd_install_state",
                              StringMgr.Get("update.s_hide_fail_short"));
    }
}

// ============================================================
// 更新页事件路由说明（配套下方 CPageUpdate::OnEvent）
// ------------------------------------------------------------
// 本页事件分五类消息处理，顺序与优先级如下：
//
//   1) click —— 按钮与列表行点击
//      · 13 个按钮（刷新/策略/删除/全选/预下载/安装/收尾/隐藏/
//        取消隐藏/暂停切换/推迟切换/禁用切换/CBS 折叠展开）
//        及其处理已提为 HandleButtonClick，本函数只留一行分派。
//      · updrow_ 行点击留在本函数尾部：它需要 ParsePrefix1 的序号
//        结果，与按钮名判定共享 sName，按数据流边界不并入按钮分派。
//
//   2) dbclick —— 更新行双击
//      命中 updrow_ 即触发卸载流程（StartUninstall），不弹确认。
//
//   3) textchanged —— 过滤框输入
//      不立即过滤，挂 250ms 一次性定时器做防抖；连续输入只在停手后
//      过滤一次，避免每敲一个字符就重建数百行列表。
//
//   4) selectchanged —— Tab 切换（updtab_ 前缀）
//      切页后先更新底栏显隐，再按「该 Tab 是否已有数据缓存」二选一：
//      有缓存则只重渲染（免一次 3~10 秒的 WUA/CBS 查询），
//      无缓存才真正发起枚举。这是更新页最关键的体验优化点。
//
//   5) itemselect —— 下拉框选中
//      仅 upd_maxthread_combo（下载线程数上限）。注意 CComboUI 发的是
//      DUI_MSGTYPE_ITEMSELECT（"itemselect"），不是 Option 用的
//      "selectchanged"；旧代码挂错消息导致该项切换永远不生效。
//
// ------------------------------------------------------------
// 关于 btn_upd_find 的例外：
//   该按钮的注册表待重启检测属「系统调用纪律」，保留在壳层
//   CFrameWnd::Notify 内直接处理，不进本函数。壳在检测完成后回调
//   本页 OnScanStarted() 承接状态。这是刻意保留的跨层入口，
//   不是遗漏——移植时不要把它"补"进本页。
//
// 关于 Tab 索引与 m_nCurTab：
//   m_nCurTab 是本页的当前 Tab 事实源，切页时立即写入。
//   刷新按钮（btn_upd_refresh）用的是 m_nCurTab 而非界面当前值，
//   这样即使界面上 Tab 控件状态与成员不同步，刷新仍作用于
//   用户看到的那个 Tab。
//
// 关于 CBS 折叠组（cbstoggle_row）：
//   点击后先把该行自身从列表移除，再以 bCbs=true 发起分批渲染。
//   移除必须从后往前遍历（RemoveAt 会使后续索引前移），
//   这一点在提取到 HandleButtonClick 时逐字保留。
//
// ------------------------------------------------------------
// 维护提示：
//   · 新增按钮时，优先在 HandleButtonClick 内加分支，保持本函数精简。
//   · 若新按钮的处理需要行序号（updrow_ 语义），则改在本函数尾部处理。
//   · 任何分支一旦消费事件必须立即返回，不可落到后续消息类型判定，
//     否则 click 会再次被当成 selectchanged 等重复处理。
// ============================================================

// ============================================================
// ------------------------------------------------------------
// 更新页数据流与线程模型（理解本页 OnEvent 的前置知识）
// ------------------------------------------------------------
// 本页三条枚举来源并行，各自独立回传，互不阻塞：
//
//   A) WUA（Windows Update Agent）—— Tab0 待安装 / Tab1 已安装
//      经 COM 查询，耗时数秒，结果写 m_vUpdList[0] / m_vUpdList[1]。
//   B) CBS（DISM /Get-Packages）—— 系统组件包
//      结果先落 m_vCbsPending（时序暂存），待 WUA 渲染完成后再调用
//      UpdMergeCbs 按 KB 号去重并入，避免两套列表同屏重复。
//   C) 磁盘缓存 —— 上次成功枚举的落盘结果
//      进页时先渲染缓存秒开，后台照常发起真实枚举，到达后整体重建。
//
// 三者的到达顺序不确定，因此本页所有入口都必须能处理「数据未就绪」：
//   · m_bTabLoaded[x] 为 false 时不得假设 m_vUpdList[x] 可用；
//   · m_bCbsReady 为 false 时 UpdMergeCbs 直接返回（等回调）；
//   · 分批渲染期间若底层容器被替换，需按序号校验而非假定序列有效。
//
// ------------------------------------------------------------
// 分批渲染接力（与可选功能页同一套范式）
// ------------------------------------------------------------
// 更新列表行数可达数百（Tab0 待安装 + Tab1 已安装 + CBS 组件包），
// 每行含名称/大小/日期/勾选框/状态徽章等多个控件，一次性构建会阻塞
// UI 线程数百毫秒，表现为点 Tab 后界面卡住不动。
//
// 做法：把待渲染行的下标压成快照（m_pRenderList / m_nRenderTab /
// m_nRenderCbs），每批 ≤50 行同步渲染，未完成则挂 15ms 一次性定时器
// 接力下一批。首批同步执行保证首屏不空白。
//
// 关键约束：接力跨多个定时器周期，期间底层数据可能被新的枚举结果
// 整体替换为更短的列表，此时快照里的旧下标会越界。因此每批渲染前
// 必须逐项校验下标上界，越界即判定快照失效并重建，不可直接下标访问。
//
// ------------------------------------------------------------
// 勾选状态与批量操作
// ------------------------------------------------------------
// m_vChecked[2] 与 m_vUpdList[2] 平行存放（同为 char 数组 / 条目数组），
// 长度必须始终一致。凡是重建 m_vUpdList 的路径，都必须同步重建
// m_vChecked，否则批量安装/隐藏会按错位的勾选集合执行。
//
// 批量操作（全选/隐藏/安装）一律先收集目标集合到局部 vector，
// 再统一执行；不可在遍历 m_vUpdList 的同时改动它的长度。
// ============================================================

// ------------------------------------------------------------
// 日志与输出区的约定
// ------------------------------------------------------------
// 更新页的所有操作结果都写两条通道：
//   · HostLog  —— 主日志区，带级别（INFO/WARN/ERROR），供事后追溯；
//   · HostToast / HostAlert —— 即时反馈，成功用 Toast，失败用 Alert。
// 二者不可互相替代：只写日志用户看不到，只弹框无法事后追溯。
// ============================================================
// P8：第二级事件路由（原 CFrameWnd::Notify 更新页分支逐字迁入。
// 例外：btn_upd_find 含注册表待重启检测（系统调用纪律），保留壳分派，
// 壳在检测后调本页 OnScanStarted()）
// ============================================================

// [R168] 从 OnEvent 抽出：selectchanged 的 updtab_ 分支。
// 语义与逐字迁移前完全一致：先同步 CTabLayoutUI 选中项，再写 m_nCurTab
// 事实源、刷底栏显隐，最后按"该 Tab 是否已有缓存"二选一。
// 返回 true 表示本事件已被消费（调用方须立即 return，不可继续后续消息判定）。
bool CPageUpdate::UpdOnTabSwitch(const wchar_t* pszName)
{
    if (!pszName || wcsncmp(pszName, L"updtab_", 7) != 0) return false;
    int idx = _ttoi(pszName + 7);
    CTabLayoutUI* p = static_cast<CTabLayoutUI*>(m_ctx.HostFindControl(L"upd_pages"));
    if (p) p->SelectItem(idx);
    // [性能 C3] Tab 切换：已有该 Tab 数据缓存时仅重渲染（免 3-13s 重查），
    // 仅首次进入才发查询；"刷新"按钮仍保留全量重查入口。
    m_nCurTab = idx;
    UpdateBottomBar();   // [性能 P3] 底栏随 Tab 感知显隐
    if (m_bTabLoaded[idx])
        RenderTabFromCache(idx);
    else
        StartEnumeration(idx);
    return true;
}

// [R175] 从 OnEvent（46 行 / CC13）抽出：click / dbclick 两条行点击路径。
// 返回 true 表示已消费（调用方须立即返回）。
// 语义逐字保留：click 先试按钮分派，未命中再试 updrow_ 行号；dbclick 只认 updrow_。
bool CPageUpdate::UpdHandleClickMsg(const CDuiString& sName)
{
    if (HandleButtonClick(sName)) return true;
    // [修复 R5 / P1-15a] 更新列表行点击 → 记录选中项
    int nUpdRow = -1;
    if (!ParsePrefix1(sName, L"updrow_", nUpdRow)) return false;
    OnRowSelect(nUpdRow);
    return true;
}

bool CPageUpdate::UpdHandleDbClickMsg(const CDuiString& sName)
{
    int a = -1;
    if (!ParsePrefix1(sName, L"updrow_", a)) return false;
    StartUninstall(a);
    return true;
}

// [R187] 从 OnEvent 抽出：指针类消息（click / dbclick）。
// 返回 true 表示已消费。两条消息都只是把名字交给行级处理器，
// 处理器内部再决定「按钮 / 行 / 未命中」，故此处只做消息类型分流。
bool CPageUpdate::UpdHandlePointerMsgs(const CDuiString& sType, const CDuiString& sName)
{
    if (sType == L"click")   return UpdHandleClickMsg(sName);
    if (sType == L"dbclick") return UpdHandleDbClickMsg(sName);
    return false;
}

// [R187] 从 OnEvent 抽出：非指针类消息（textchanged / selectchanged / itemselect）。
// 返回 true 表示已消费。pszName 供前缀判定使用（与 sName 同源，避免重复 GetData）。
bool CPageUpdate::UpdHandleValueMsgs(const CDuiString& sType, const CDuiString& sName,
                                     const wchar_t* pszName)
{
    if (sType == L"textchanged")   return UpdHandleFilterMsg(sName);
    if (sType == L"selectchanged") return UpdOnTabSwitch(pszName);
    if (sType == DUI_MSGTYPE_ITEMSELECT) return UpdHandleComboMsg(pszName);
    return false;
}

void CPageUpdate::OnEvent(TNotifyUI& msg)
{
    CDuiString sName = msg.pSender ? msg.pSender->GetName() : CDuiString(L"");
    const wchar_t* pszName = sName.GetData();
    // 两类分派：指针类（click/dbclick 走行级处理器）与值类（过滤/Tab/下拉）。
    // 顺序与原实现一致：指针类先判，值类后判，命中即返回。
    if (UpdHandlePointerMsgs(msg.sType, sName)) return;
    (void)UpdHandleValueMsgs(msg.sType, sName, pszName);   // 末位分派，返回值无需消费
}

// [R187] 从 OnEvent（38 行 / CC12）抽出：过滤框输入（带 250ms 防抖）。
// 不立即过滤：连续输入只在停手后触发一次，避免每敲一个字符重建数百行列表。
// 只有 upd_filter 一个控件发此消息，其它控件返回 false 让调用方继续后续判定。
bool CPageUpdate::UpdHandleFilterMsg(const CDuiString& sName)
{
    if (sName != L"upd_filter") return false;
    // [性能 C1] 250ms 防抖：连续输入只在停手后过滤一次
    ::SetTimer(m_ctx.HostHwnd(), UPD_FILTER_TIMER_ID, kUpdFilterDebounceMs, nullptr);
    return true;
}

// [R187] 从 OnEvent 抽出：下载线程数上限下拉（清单维度 7）。
// [修复] CComboUI 选中项后发的是 DUI_MSGTYPE_ITEMSELECT（"itemselect"），
// 不是 Option 用的 "selectchanged"。旧代码挂错消息，该项切换永远不生效
// （config.ini 不更新）。此处保留 lstrcmpiW 不区分大小写比较，与原实现一致。
bool CPageUpdate::UpdHandleComboMsg(const wchar_t* pszName)
{
    if (lstrcmpiW(pszName, L"upd_maxthread_combo") != 0) return false;
    OnMaxThreadChanged();
    return true;
}

// P8：原 CFrameWnd WM_RBUTTONUP 通用列表命中块（upd_pending_list/upd_list）
// [N14] T2 抽纯函数：解析 OFN_ALLOWMULTISELECT 多选路径缓冲
// 单选时首串为完整路径；多选时首串为目录、后续为文件名
static std::vector<std::wstring> ParseMultiSelPaths(const wchar_t* pBuf)
{
    std::vector<std::wstring> vOut;
    if (!pBuf || !*pBuf) return vOut;
    std::wstring sDir = pBuf;
    pBuf += sDir.size() + 1;
    if (*pBuf == 0)
    {
        vOut.push_back(sDir);            // 单选：已是完整路径
        return vOut;
    }
    while (*pBuf)
    {
        std::wstring sName = pBuf;
        pBuf += sName.size() + 1;
        vOut.push_back(sDir + L"\\" + sName);
    }
    return vOut;
}

// [R162] 从 OnContextMenu 抽出：更新列表（待安装/已安装）的右键菜单。
//   [R-B76] 自有菜单：本地补丁安装入口 + 原统一菜单两项（更新设置/详情）。
//   【修掉一处真 bug】原实现在选了本地补丁包之后调用了两次 InstallLocalPackages：
//        InstallLocalPackages(vPick);
//        InstallLocalPackages(vPick);
//   等于用户选一次、安装跑两遍。第二次通常报"已存在"或全部失败，
//   成功数被重复累加，用户无法判断到底装成功没有。已删去重复调用。
void CPageUpdate::ShowUpdListMenu(POINT pt)
{
    enum { IDC_UPD_ADDLOCAL = 6201, IDC_UPD_WUSETTINGS = 6202, IDC_UPD_DETAIL = 6203 };
    HMENU hUpdMenu = ::CreatePopupMenu();
    if (!hUpdMenu) return;
    ::AppendMenuW(hUpdMenu, MF_STRING, IDC_UPD_ADDLOCAL,
                  StringMgr.Get("update.menu_addlocal"));
    ::AppendMenuW(hUpdMenu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(hUpdMenu, MF_STRING, IDC_UPD_WUSETTINGS,
                  StringMgr.Get("misc.hdd16"));   // 打开 Windows 更新设置
    ::AppendMenuW(hUpdMenu, MF_STRING, IDC_UPD_DETAIL,
                  StringMgr.Get("misc.h727c"));   // 详情
    const int iCmd = ::TrackPopupMenu(hUpdMenu,
                        TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                        pt.x, pt.y, 0, m_ctx.HostHwnd(), nullptr);
    ::DestroyMenu(hUpdMenu);

    if (iCmd == IDC_UPD_ADDLOCAL)
    {
        // 更新包多选对话框路径缓冲（单路径最长约 4K wchar）
        constexpr size_t kOpenFileBufWchars = 8192;
        std::vector<wchar_t> vBuf(kOpenFileBufWchars, 0);
        OPENFILENAMEW ofn;
        ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = m_ctx.HostHwnd();
        ofn.lpstrFilter = StringMgr.Get("update.pkg_filter");
        ofn.lpstrFile = vBuf.data();
        ofn.nMaxFile = static_cast<DWORD>(vBuf.size());
        ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER |
                    OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (!::GetOpenFileNameW(&ofn)) return;
        // [N14] 多选路径解析下沉文件级纯函数
        const std::vector<std::wstring> vPick = ParseMultiSelPaths(vBuf.data());
        InstallLocalPackages(vPick);   // [R162] 只用调一次
        return;
    }
    if (iCmd == IDC_UPD_WUSETTINGS)
    {
        ::ShellExecuteW(m_ctx.HostHwnd(), L"open",
                        L"ms-settings:windowsupdate",
                        nullptr, nullptr, SW_SHOWNORMAL);
        m_ctx.HostLog(StringMgr.Get("misc.h8a51"));
        return;
    }
    if (iCmd == IDC_UPD_DETAIL)
    {
        CDuiString sD;
        sD.Format(StringMgr.Get("misc.h718d"), static_cast<int>(EListCtx::LCX_UPDATE), -1);
        m_ctx.HostLog(sD.GetData());
    }
}

[[nodiscard]] bool CPageUpdate::OnContextMenu(POINT pt, bool bFromContextMsg)
{
    if (bFromContextMsg) return false;
    CControlUI* pHit = m_ctx.HostFindControlAt(pt);
    // [R162] 命中列表即交给 ShowUpdListMenu 处理，本函数只做命中判定
    //   （原为 for/for/if/if 四层，嵌套 6）。
    const wchar_t* kListNames[] = { L"upd_pending_list", L"upd_list" };
    for (CControlUI* q = pHit; q; q = q->GetParent())
    {
        const wchar_t* pn = q ? q->GetName() : nullptr;
        if (!pn) continue;
        for (const wchar_t* pszList : kListNames)
        {
            if (lstrcmpW(pn, pszList) == 0) { ShowUpdListMenu(pt); return true; }
        }
    }
    return false;
}


// ============================================================
// [R5] 后台消息 / 壳命令统一入口（原壳 OnPgUpdData / OnPgUpdProgress、
// btn_upd_find 中的 OnScanStarted 直调）
// ============================================================

[[nodiscard]] bool CPageUpdate::OnAsyncMessage(UINT uMsg, WPARAM /*wParam*/, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_UPD_DATA:
        // kMsgOwnPage：UpdDataMsg* 由 OnData 释放。
        OnData((UpdDataMsg*)lParam);
        return true;
    case WM_UPD_PARTIAL:
        // [P1] 更新枚举分批回报，由 OnPartial 消费并释放消息对象。
        OnPartial((UpdPartialMsg*)lParam);
        return true;
    case WM_UPD_PHASEDONE:
        // [FIX-P2N] 更新阶段后台完成：UI 收口 UpdReportPhaseResult
        // （后台线程已结束，m_bPhaseRunning 在此复位）。
        if (UpdPhaseDoneMsg* pPhase = (UpdPhaseDoneMsg*)lParam)
        {
            std::unique_ptr<UpdPhaseDoneMsg> guard(pPhase);
            m_bPhaseRunning = false;
            UpdReportPhaseResult(pPhase->dwRet,
                                 CDuiString(pPhase->sPhaseFile.c_str()));
        }
        else
        {
            m_bPhaseRunning = false;   // null 载荷也必须解在途标志
        }
        return true;
    case WM_UPD_PROGRESS:
        // kMsgOwnShell：UpdProgressMsg* 仅读字段，壳负责释放。
        if (UpdProgressMsg* pProg = (UpdProgressMsg*)lParam)
        {
            OnInstallProgress(pProg->nPercent, pProg->nPhase,
                              pProg->nItemIndex, pProg->nItemCount);
        }
        return true;
    default:
        return false;
    }
}


#include <memory>   // [C++17 R3] make_unique（置于文件尾，避免影响 verify 行号）

// [REFACTOR] 从 OnEvent 提取：更新页 13 个按钮的 click 分派。
// 每个分支只做一次转发调用即返回；原 return; 语义为"本条已消费"，
// 提取后改为返回 true，调用点据此外提前返回。逐字搬迁零逻辑改动。

// [P1] 更新枚举分批回报：把该批追加进对应 Tab 的累积表并增量渲染。
// 仅在"该 Tab 尚无完整数据"时接受分批（已有缓存/已完成枚举时丢弃，
// 避免与整表内容重复）。末批（bFinal）清在途标记并收尾。
void CPageUpdate::OnPartial(UpdPartialMsg* pMsg)
{
    if (!pMsg) return;
    // [R3-FIX] generation 守卫：过期分批消息（上一次枚举）直接丢弃
    if (pMsg->nGen != 0 && pMsg->nGen != m_nUpdGen)
    {
        std::unique_ptr<UpdPartialMsg> guard(pMsg);
        m_ctx.HostLog(L"[update] stale UpdPartialMsg dropped (gen mismatch)");
        return;
    }
    std::unique_ptr<UpdPartialMsg> guard(pMsg);   // [C++17 R3] 本函数独占
    const int nTab = (pMsg->nTab == 0) ? 0 : 1;
    // 整表已到：迟到的分批一律丢弃
    if (m_bUpdPartialDone[nTab]) return;
    // 该 Tab 已有完整数据（缓存命中）：分批无意义
    if (!m_vUpdList[nTab].empty()) return;
    if (!pMsg->vBatch.empty())
    {
        std::vector<UpdEntry>& vAcc = m_vBatchUpd[nTab];
        vAcc.insert(vAcc.end(), pMsg->vBatch.begin(), pMsg->vBatch.end());
        // 用累积数据重建列表（数据量有限，重建比维护增量行映射更可靠）
        m_vUpdList[nTab] = vAcc;
        m_bTabLoaded[nTab] = true;
        m_nCurTab = nTab;
        UpdStartBatchRender(
            static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(
                nTab == 0 ? L"upd_pending_list" : L"upd_list")), nTab, false);
    }
    if (!pMsg->bFinal) return;
    m_bUpdPartialDone[nTab] = true;
    m_bTabLoading[nTab] = false;
    m_vBatchUpd[nTab].clear();
    CDuiString sLog;
    sLog.Format(L"[update] partial enum done: tab=%d %d entries",
                nTab, static_cast<int>(m_vUpdList[nTab].size()));
    m_ctx.HostLog(sLog.GetData());
}

// [R127-A] 收集当前 Tab 勾选项的 UpdateID（实现自两处逐字重复代码提取）。
bool CPageUpdate::CollectCheckedUpdateIds(std::vector<CDuiString>& vOut)
{
    vOut.clear();
    // 勾选态与列表长度不一致时按"全选"补齐（沿用原语义）
    if (static_cast<int>(CurChecked().size()) != static_cast<int>(CurList().size()))
        CurChecked().assign(CurList().size(), 1);
    for (size_t i = 0; i < CurList().size(); ++i)
    {
        if (!CurChecked()[i]) continue;
        const CDuiString& sId = CurList()[i].sUpdateID;
        if (!sId.IsEmpty())
            vOut.push_back(sId);
    }
    if (vOut.empty())
    {
        m_ctx.HostToast(StringMgr.Get("update.s_install_noselected"),
                        StringMgr.Get("update.s_install_noselected"), 1);
        return false;
    }
    return true;
}
