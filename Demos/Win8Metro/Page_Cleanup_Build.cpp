#include "stdafx.h"
#include "ThemeTokens.h"   // [R203] 颜色令牌（深色/浅色跟随）
// #include "OfflineSession.h" // [R24-F2] 删除假依赖：0 处使用  // [R10-02] 离线会话经独立组件访问，去 CDataEngine 静态耦合

#include "Page_Cleanup.h"

#include "StringManager.h"
#include "MsgParam.h"          // [R159] msgparam::FromParam<T>

#include "FrameWnd_Util.h"          // StyleCheckBox / RecalcBodyHeight / CopyTextToClipboard
#include <memory>                   // [C++17 R3] make_unique/unique_ptr
#include <map>                      // [G2] 刷新快照：规则名 → 勾选
#include <string>                   // [G2] std::wstring 作为 map 键
#include "FrameWnd_UiHelpers.h"  // P8：统一列表右键菜单 / EListCtx

#include "CustomControls.h"        // CClickRowUI

#include "ProcessRunner.h"  // RunHiddenExit / RunCapture

#include "DiagnosticsService.h"    // 进入页面的 3 个诊断快照

#include "WmMessages.h"            // [R13 P0收敛] [LAG-FIX] WM_DIAG_LOG 定义在 WmMessages.h；UI 专属常量零使用
#include "SafeThread.h"            // 碎片诊断后台线程异常边界

#include "ExternalToolService.h"  // IsElevated
// #include "DismApiBridge.h" // [R25-F1] 删除假依赖：0 处使用  // [R-B18] dismapi.dll 直连 DismRemovePackage
// #include "AppxRuntimeBridge.h" // [R25-F1] 删除假依赖：0 处使用  // [P0-A1] Appx 重注册/坏包清理 WinRT 原生

// #include "CleanExtend.h" // [R25-F1] 删除假依赖：0 处使用  // [R80] 深度扫描/残留/空文件夹/撤销引擎
// #include "VssNativeService.h" // [R25-F1] 删除假依赖：0 处使用  // [R-B31] VSS 原生 API 删除快照（vssapi.dll COM）

// #include "CbsHostClient.h" // [R23-F4] 删除假依赖：0 处使用
// #include "NCleanerClient.h" // [R25-F1] 删除假依赖：0 处使用  // [R-拆分] NCleaner 7 导出经 NCleaner.dll 动态调用
// #include "DataEngine.h" // [R23-F4] 删除假依赖：0 处使用
// #include "SystemInfoService.h" // [R23-F4] 删除假依赖：0 处使用

#include "CleanNativePrimitives.h"  // [R-B57] CreateHardLink 通用重复文件回收原语
// #include "WofCompact.h" // [R25-F1] 删除假依赖：0 处使用           // [P0-A2] CompactOS 原生 FSCTL（替代 compact.exe）
// #include "RegCleanEngine.h" // [R23-F4] 删除假依赖：0 处使用

#include <shlobj.h>                // SHGetFolderPath / SHChangeNotify
#include <winevt.h>                // [R-B17] EvtClearLog / EvtOpenChannelEnum

#pragma comment(lib, "wevtapi.lib")

#include <string>

#include <algorithm>              // [R126-1] std::stable_sort

using i18n::CStringManager;   // 便捷：直接写 CStringManager 不用前缀

// ============================================================
// Page_Cleanup_Build.cpp —— [R14-02] CPageCleanup 页面构建/过滤/展示切片
// 从 Page_Cleanup.cpp（原 4496 行）物理切出，满足 R14-02 验收“文件≤2.5k”：
//   Build 薄壳 + BdBuildGroup/BdBuildRuleRow/BdStyleRiskBadge/BdLogRiskOverview
//   ApplyCleanFilter/ToggleCleanGroup/RefreshGroupTotals/SetWorkerUiBusy/
//   ShowCleanDesc/OnCleanRowCheck/OnScanItem/SortCleanRows/OcRefreshDescPanel
// 物理搬迁护栏：函数体逐字迁移（Build 仅做等价分段提取），宿主引用仍走 m_ctx。
// ============================================================

// ============================================================
// [LAG-FIX] 磁盘碎片分析（FSCTL 全卷位图 + MFT 逐文件检索指针）最长
// 跑满 60s，旧实现在 UI 线程随页面构建同步执行，是"启动卡 60 秒/越界崩溃"
// 的根因。改为 fire-and-forget 后台线程，完成后 PostMessage(WM_DIAG_LOG)
// 回 UI 只追加一行摘要（明细已写入 fragmentation.log）。
// ============================================================
static volatile LONG g_defragDiagInFlight = 0;

// 线程体：C++ 异常在此捕获（保证在途标志复位、UI 不死等）。
// SEH 硬件异常（如旧版 FSCTL 卷位图越界读）由 SafeThreadTrampoline 外层兜底，
// 但此时标志也必须复位 —— 故复位放在外层过程的 finally 等价位置（见 DefragDiagSafeProc）。
static DWORD WINAPI DefragDiagThreadBody(void* lpParam)
{
    // [R159] void* → HWND 走 msgparam::FromVoidParam 单一收敛点
    HWND hWnd = msgparam::FromVoidParam<HWND>(lpParam);
    CDuiString sResult;
    try
    {
        sResult = DiagnosticsService::AnalyzeDiskFragmentationToLog();
    }
    catch (...)
    {
        sResult = L"";
    }
    if (sResult.IsEmpty() || !::IsWindow(hWnd)) return 0;
    // [C++17 R3] msg ownership transferred to UI on successful post; unique_ptr frees it if post fails
    auto pMsg = std::make_unique<CDuiString>(sResult);
    if (::PostMessageW(hWnd, WM_DIAG_LOG, 0, msgparam::ToParam(pMsg.get())))
        pMsg.release(); // [C++17 R3] ownership transferred to UI
    return 0;
}

// [R23-FIX] 线程外层过程：复位在途标志。
//   SafeThreadTrampoline 已有 SEH 边界 + SafeThreadDispatchCxx 有 C++ catch，
//   本函数的 __try/__except 永远捕获不到任何异常（冗余）。
//   这里直接调用并在函数尾部复位标志即可。
static DWORD WINAPI DefragDiagThreadProc(void* lpParam)
{
    SafeThreadDispatchCxx(DefragDiagThreadBody, lpParam);
    ::InterlockedExchange(&g_defragDiagInFlight, 0);
    return 0;
}

static void PostDefragDiagAsync(HWND hWnd)
{
    if (!hWnd) return;
    if (::InterlockedCompareExchange(&g_defragDiagInFlight, 1, 0) != 0) return;  // 在途则跳过
    // 经 SafeCreateThread：线程过程逃逸的异常不会再终止进程
    // （历史 c000041d 启动崩溃 / c0000409 BEX64 的直接根因）
    HANDLE hThread = SafeCreateThread(DefragDiagThreadProc, hWnd);
    if (hThread) ::CloseHandle(hThread);
    else ::InterlockedExchange(&g_defragDiagInFlight, 0);
}

// case-insensitive substring test（随 [R14-02] Build/ApplyCleanFilter 切片迁入本 TU；
// 原 Page_Cleanup.cpp 匿名命名空间副本，仅本 TU 过滤逻辑使用，口径不变）
[[nodiscard]] static bool ContainsCI(const wchar_t* pszHay, const wchar_t* pszNeedle)
{
    CDuiString sHay(pszHay); sHay.MakeLower();
    CDuiString sNeedle(pszNeedle); sNeedle.MakeLower();
    return sHay.Find(sNeedle.GetData()) >= 0;
}

// [修复 #23] UI 大小快照中“尚未扫到该项”的哨兵值（区别于扫到的真实 0 字节）
// [R14-02] 原 Page_Cleanup.cpp 匿名命名空间常量；切片后各 TU 各持 internal-linkage 副本。
// [C++17 R3] inline constexpr：编译期常量 + 内联，替代 static const（内部链接副本语义等价）
inline constexpr ULONGLONG CLEAN_SIZE_UNKNOWN = static_cast<ULONGLONG>(static_cast<LONGLONG>(-1));

// ============================================================
// [R14-02] Build 拆分为薄壳 + Bd* 辅助（原 367 行单函数）。
//   薄壳只做状态复位/展开策略/顺序编排；单组组装 BdBuildGroup、
//   单行组装 BdBuildRuleRow（风险徽章着色 BdStyleRiskBadge）、
//   风险总览日志 BdLogRiskOverview。纯 UI 组装迁移，零行为改动。
// ============================================================
void CPageCleanup::Build()
{
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"clean_list"));
    if (!pList) return;

    // [G2 2026-10-25] 刷新保留用户状态：勾选 + 滚动位置。
    //   原实现无条件 RemoveAll 后按默认策略重建，用户勾选 20 项再点刷新 = 全部重来，
    //   滚动也回顶部 —— 这直接破坏了"勾选→复核→清理"的核心流程。
    //   快照键用规则名而非索引：索引依赖 Data.xml 顺序，规则集变动时会错位到别的项；
    //   规则名是用户可见身份，重名极罕见，错位风险更低。
    std::map<std::wstring, bool> mapChecked;
    SIZE szScroll = { 0, 0 };
    bool bHadRows = false;
    for (int i = 0; i < m_ctx.CleanEngine().GetRuleCount(); ++i)
    {
        CDuiString sChk; sChk.Format(L"ckclean_%d", i);
        CCheckBoxUI* pChk = static_cast<CCheckBoxUI*>(m_ctx.HostFindControl(sChk.GetData()));
        if (!pChk) continue;
        mapChecked[m_ctx.CleanEngine().GetRule(i).sName.GetData()] = pChk->GetCheck() != FALSE;
        bHadRows = true;
    }
    if (bHadRows) szScroll = pList->GetScrollPos();

    pList->RemoveAll();
    int nGroups = m_ctx.CleanEngine().GetGroupCount();
    // fresh page: any previous scan result is discarded
    m_bCleanScanned = false;
    m_nCleanScannedCnt = 0;
    // C-8: 刷新后旧的清理报告失效
    m_bCleanHasReport = false;
    m_sCleanFullDetail.Empty();
    // [修复 #20/#23] UI 侧数据态复位：行过滤通过标志、大小快照（未知）
    m_vCleanRowPass.assign(m_ctx.CleanEngine().GetRuleCount(), 1);
    m_vUiSize.assign(m_ctx.CleanEngine().GetRuleCount(), CLEAN_SIZE_UNKNOWN);
    // [STAB] 展开态是用户状态：首次构建默认展开前两组（smart load），
    // 重入（F5 刷新 / 切页往返）时保留用户当前的展开/折叠选择。
    // 旧实现每次 Build 都重新 assign + 强制展开前两组，用户折叠过的组会被强行打开。
    if (static_cast<int>(m_vCleanExpanded.size()) != nGroups)
    {
        m_vCleanExpanded.assign(nGroups, false);
        for (int k = 0; k < 2 && k < nGroups; ++k) m_vCleanExpanded[k] = true;
    }
    for (int g = 0; g < nGroups; ++g)
        BdBuildGroup(pList, g);
    // style the filter checkbox with the shared glyph so it renders a box
    CCheckBoxUI* pShowZero = static_cast<CCheckBoxUI*>(
        m_ctx.HostFindControl(L"clean_showzero"));
    // [R58-A] 风险分级总览：清理项按等级统计，帮助用户识别整体风险分布
    BdLogRiskOverview();
    // [R58-C] 每次进入空间回收页 → 追加一条系统信息快照到持久化日志
    // 用于跨会话追踪 RAM / 磁盘使用趋势，用户可离线回看
    {
        CDuiString sSnap = DiagnosticsService::AppendSysInfoSnapshotToFile();
        if (!sSnap.IsEmpty()) AppendLog(sSnap.GetData());
    }
    // [R61-D] 启动时间完整追踪：反推启动时间戳 → boottime_history.log
    {
        CDuiString sBoot = DiagnosticsService::AppendBootTimeSnapshotToFile();
        if (!sBoot.IsEmpty()) AppendLog(sBoot.GetData());
    }
    // [R61-C] 磁盘碎片分析采样（不实际运行 defrag.exe，仅记录采样点）→ fragmentation.log
    // [LAG-FIX] 移至后台线程（最长 60s），完成后经 WM_DIAG_LOG 回 UI 追加摘要，
    // 不再阻塞页面构建/窗口创建。
    // [AB-TEST 结论] 启动崩溃与 defrag 线程无关（禁用后增量构建照样崩，
    //   全量 Rebuild 后 6/6 稳定）——真因是增量构建 vtable 错位，已恢复调用。
    PostDefragDiagAsync(m_ctx.HostHwnd());
    if (pShowZero)
    {
        // [复选框共性修复] 空框起手 + 按实际状态刷新字形
        StyleCheckBox(pShowZero, StringMgr.Get("misc.s420f06a2"), 26, m_ctx.HostGlyphBox(0));
        pShowZero->SetCheck(m_bCleanShowZero);
        m_ctx.HostRefreshCheckText(pShowZero);
    }
    // apply current keyword / zero-size recognition
    ApplyCleanFilter();

    // [G2] 恢复勾选 + 滚动。必须在 ApplyCleanFilter 之后：过滤会改可见性，
    // 先恢复滚动会因行高尚未生效而被夹回顶部。
    if (!mapChecked.empty())
    {
        int nRestored = 0;
        for (int i = 0; i < m_ctx.CleanEngine().GetRuleCount(); ++i)
        {
            CDuiString sChk; sChk.Format(L"ckclean_%d", i);
            CCheckBoxUI* pChk = static_cast<CCheckBoxUI*>(m_ctx.HostFindControl(sChk.GetData()));
            if (!pChk) continue;
            auto it = mapChecked.find(std::wstring(m_ctx.CleanEngine().GetRule(i).sName.GetData()));
            if (it == mapChecked.end()) continue;
            if ((pChk->GetCheck() != FALSE) != it->second)
            {
                pChk->SetCheck(it->second);
                m_ctx.HostRefreshCheckText(pChk);
            }
            ++nRestored;
        }
        if (nRestored > 0 && bHadRows)
            pList->SetScrollPos(szScroll, false);
        CDuiString sLog;
        sLog.Format(L"[cleanup] refresh restored %d/%d checked states, scroll.y=%d",
                    nRestored, static_cast<int>(mapChecked.size()), static_cast<int>(szScroll.cy));
        AppendLog(sLog.GetData());
    }
}

// [R14-02] 单个清理组：组头（箭头+组复选框）+ body（仅同组适用规则行）。
void CPageCleanup::BdBuildGroup(CVerticalLayoutUI* pList, int g)
{
    CDuiString strGroup;
    m_ctx.CleanEngine().GetGroup(g, strGroup);
    // ---- group header row (click toggles body) ----
    auto pHeader = std::make_unique<CClickRowUI>(); // [C++17 R3]
    pHeader->SetFixedHeight(34);
    RECT rcHeaderInset = { 4, 0, 10, 0 };
    pHeader->SetInset(rcHeaderInset);
    pHeader->SetChildPadding(6);
    CDuiString sHdrName;
    sHdrName.Format(L"clngrp_%d", g);
    pHeader->SetName(sHdrName.GetData());
    auto pArrow = std::make_unique<CTextUI>(); // [C++17 R3]
    CDuiString sArName;
    sArName.Format(L"clnarrow_%d", g);
    pArrow->SetName(sArName.GetData());
    pArrow->SetFixedWidth(14);
    pArrow->SetFont(9);
    pArrow->SetText(m_ctx.HostGlyphArrow(m_vCleanExpanded[g] ? 1 : 0));
    pArrow->SetTextColor(tokens::Token(tokens::EToken::InkMuted));
    pArrow->SetAttribute(L"align", L"left");
    pArrow->SetAttribute(L"valign", L"vcenter");
    pArrow->SetMouseEnabled(false);
    pHeader->Add(pArrow.release()); // [C++17 R3] ownership transferred to DuiLib
    auto pHk = std::make_unique<CBoxCheckUI>(); // [C++17 R3] [BOX-CHECK] 自绘方框
    CDuiString sHkName;
    sHkName.Format(L"grpck_%d", g);
    pHk->SetName(sHkName.GetData());
    // [复选框共性修复] 组头先以空框起手，建完行后由 SyncCleanGroupCheck
    // 统一设置全选/半选/空三态，避免中间态闪出错误的 ☑。
    StyleCheckBox(pHk.get(), strGroup.GetData(), 30, m_ctx.HostGlyphBox(0));
    // [E-5] 组勾选态在下方行创建完成后按 Level 默认策略统一三态同步
    pHk->SetCheck(false);
    pHeader->Add(pHk.release()); // [C++17 R3] ownership transferred to DuiLib
    // ---- body (rows for applicable rules only) ----
    auto pBody = std::make_unique<CVerticalLayoutUI>(); // [C++17 R3]
    CDuiString sBodyName;
    sBodyName.Format(L"clnbody_%d", g);
    pBody->SetName(sBodyName.GetData());
    pBody->SetChildPadding(0);
    for (int i = 0; i < m_ctx.CleanEngine().GetRuleCount(); ++i)
    {
        CleanRule& rule = m_ctx.CleanEngine().GetRule(i);
        if (rule.sGroup != strGroup) continue;
        // [OS-Filter] 不属于当前系统的条目（如仅 24H2 才有的清理项）
        // 直接不建行：旧版本上不该看到这些功能。判定已在 LoadRules 时
        // 按 XML <Applicable> 求值并缓存在 bOsApplicable。
        if (!rule.bOsApplicable) continue;
        pBody->Add(BdBuildRuleRow(rule, i));
    }
    // [E-5] 行默认勾选态建完后，组复选框按真实全选/半选三态同步
    SyncCleanGroupCheck(g);
    RecalcBodyHeight(pBody.get());
    pList->Add(pHeader.release()); // [C++17 R3] ownership transferred to DuiLib
    CVerticalLayoutUI* pBodyRaw = pBody.release(); // [C++17 R3] ownership transferred to DuiLib
    pList->Add(pBodyRaw);
    pBodyRaw->SetVisible(m_vCleanExpanded[g]);
}

// [R14-02] 单条规则行：勾选框 + 风险等级徽章 + 状态列。
CClickRowUI* CPageCleanup::BdBuildRuleRow(const CleanRule& rule, int i)
{
    auto pRow = std::make_unique<CClickRowUI>(); // [C++17 R3]
    pRow->SetFixedHeight(28);
    RECT rcRowInset = { 40, 0, 10, 0 };
    pRow->SetInset(rcRowInset);
    pRow->SetChildPadding(10);
    CDuiString sRowClickName;
    sRowClickName.Format(L"clnrow_%d", i);
    pRow->SetName(sRowClickName.GetData());
    auto pCheck = std::make_unique<CBoxCheckUI>(); // [C++17 R3] [BOX-CHECK] 自绘方框
    CDuiString sName;
    sName.Format(L"ckclean_%d", i);
    pCheck->SetName(sName.GetData());
    // [复选框共性修复] 初始字形用空框；SetCheck 后必须刷新文本，
    // 否则 Level 0/1 默认不勾的项仍显示 ☑（视觉状态与实际相反）。
    StyleCheckBox(pCheck.get(), rule.sName.GetData(), 26, m_ctx.HostGlyphBox(0));
    // [E-5 极性修复 2026-10-07] 默认勾选策略（对齐原版 Dism++ 与 Data.xml
    // 实际语义）：Level 0/1 是高风险项（Installer 目录/CompactOs/硬链接合并/
    // 过期驱动/Office 安装源等，共 20 项），默认不勾；Level 2/3 是常规/安全
    // 项（临时文件/回收站/事件日志等），默认勾选。旧代码 <3 极性写反。
    pCheck->SetCheck(rule.nLevel >= 2);
    m_ctx.HostRefreshCheckText(pCheck.get());
    pRow->Add(pCheck.release()); // [C++17 R3] ownership transferred to DuiLib
    // [R58-A][极性修复] 风险等级徽章：Data.xml 中 nLevel 0=高风险(红)、
    // 1=中等偏高(黄)、2/3=常规安全(绿)。旧代码按反向极性着色。
    auto pRiskBadge = std::make_unique<CTextUI>(); // [C++17 R3]
    CDuiString sRiskName;
    sRiskName.Format(L"crisk_%d", i);
    pRiskBadge->SetName(sRiskName.GetData());
    pRiskBadge->SetFixedWidth(58);   // [R217] 54 clipped 2-char CJK labels:
    // 2*16px glyphs + 2*12px padding + 2px border = 58.
    pRiskBadge->SetFixedHeight(20);
    pRiskBadge->SetFont(1);
    pRiskBadge->SetAttribute(L"align", L"center");
    pRiskBadge->SetAttribute(L"valign", L"vcenter");
    pRiskBadge->SetAttribute(L"padding", L"4,2,4,2");
    pRiskBadge->SetMouseEnabled(false);
    BdStyleRiskBadge(pRiskBadge.get(), rule.nLevel);
    pRow->Add(pRiskBadge.release()); // [C++17 R3] ownership transferred to DuiLib
    auto pStatus = std::make_unique<CTextUI>(); // [C++17 R3]
    CDuiString sStatusName;
    sStatusName.Format(L"csz_%d", i);
    pStatus->SetName(sStatusName.GetData());
    pStatus->SetText(StringMgr.Get("cleanup.s6695ef78"));
    pStatus->SetFixedWidth(160);
    pStatus->SetTextColor(tokens::Token(tokens::EToken::InkSecondary));
    pStatus->SetAttribute(L"align", L"left");
    pStatus->SetAttribute(L"valign", L"vcenter");
    pStatus->SetMouseEnabled(false);
    pRow->Add(pStatus.release()); // [C++17 R3] ownership transferred to DuiLib
    return pRow.release(); // [C++17 R3] ownership returned to caller (added to body layout)
}

// [R14-02] 风险徽章三态着色（文案/前景/背景口径与原内联分支完全一致）。
void CPageCleanup::BdStyleRiskBadge(CTextUI* pRiskBadge, int nLevel)
{
    if (nLevel == 0)
    {
        // 高风险：默认不勾选，红色徽标提醒
        pRiskBadge->SetText(StringMgr.Get("misc.h4433"));
        pRiskBadge->SetTextColor(tokens::Token(tokens::EToken::Surface));
        pRiskBadge->SetBkColor(tokens::Token(tokens::EToken::Danger));
    }
    else if (nLevel == 1)
    {
        // 中等偏高：默认不勾选，黄色徽标提醒留意
        pRiskBadge->SetText(StringMgr.Get("misc.headd"));
        pRiskBadge->SetTextColor(tokens::Token(tokens::EToken::Warn));
        pRiskBadge->SetBkColor(tokens::Token(tokens::EToken::WarnBg));
    }
    else
    {
        // 常规/安全（Level 2/3）：默认可直接清理
        pRiskBadge->SetText(StringMgr.Get("misc.hfdbc"));
        // [R209] 原为硬编码浅绿底(#C8E6C9)+深绿字(#1B5E20)，深色主题下是一块亮补丁。
        //   改用语义令牌：Success 作底、OnAccent 作字，随主题自动适配。
        pRiskBadge->SetTextColor(tokens::Token(tokens::EToken::OnAccent));
        pRiskBadge->SetBkColor(tokens::Token(tokens::EToken::Success));
    }
}

// [R58-A] 风险分级总览：清理项按等级统计，帮助用户识别整体风险分布
void CPageCleanup::BdLogRiskOverview()
{
    int nSafe = 0, nMid = 0, nHigh = 0;
    for (int i = 0; i < m_ctx.CleanEngine().GetRuleCount(); ++i)
    {
        CleanRule& rule = m_ctx.CleanEngine().GetRule(i);
        // [极性修复 2026-10-07] Level 0=高风险 1=中等 2/3=安全
        if (rule.nLevel == 0) ++nHigh;
        else if (rule.nLevel == 1) ++nMid;
        else ++nSafe;
    }
    if (nSafe + nMid + nHigh > 0)
    {
        CDuiString sRiskLog;
        sRiskLog.Format(StringMgr.Get("misc.h2d32"),
                        nSafe, nMid, nHigh);
        AppendLog(sRiskLog.GetData());
    }
}

// [R154] 单行的过滤可见性判定（从 ApplyCleanFilter 抽出，原 CC20）。
// 两条独立判据：关键字命中、零字节隐藏。
// [修复 #23] 只读 UI 线程快照 m_vUiSize（WM_SCAN_ITEM 投递），
// 不直接读扫描线程正在写的 rule.ullSize（x86 下可能读到撕裂值）。
bool CPageCleanup::ClnRowShouldShow(int i, const CleanRule& rule) const
{
    if (!m_sCleanFilter.IsEmpty()
        && !ContainsCI(rule.sName.GetData(), m_sCleanFilter.GetData()))
        return false;
    // Smart zero-size recognition: 已扫描且为空、又不是特殊项(Custom ProcName，
    // 如 DismComponentCleanup)时隐藏；特殊项即使 0 字节也始终保留。
    const bool bScanned = m_bCleanScanned || i < m_nCleanScannedCnt;
    if (bScanned && !m_bCleanShowZero
        && m_vUiSize[i] == 0 && rule.sSpecialProc.IsEmpty())
        return false;
    return true;
}

// [R154] 过滤后的组体收尾：重算高度 + 空组体隐藏 + 组框三态重算。
// [B89 修复] 可见但固定高度为 0 的 VerticalLayout 会被内核当作"自适应填充"子项，
// 反而吞掉列表剩余/滚动空间，形成组后大片空白。空组体必须整体隐藏，
// 组头仍保留；展开态记录在 m_vCleanExpanded，恢复过滤后自动重现。
void CPageCleanup::ClnRecalcGroupsAfterFilter(int nGroups)
{
    for (int g = 0; g < nGroups; ++g)
    {
        CDuiString sBody;
        sBody.Format(L"clnbody_%d", g);
        CVerticalLayoutUI* pBody = static_cast<CVerticalLayoutUI*>(
            m_ctx.HostFindControl(sBody.GetData()));
        RecalcBodyHeight(pBody);
        if (static_cast<int>(m_vCleanExpanded.size()) < nGroups)
            m_vCleanExpanded.resize(nGroups, false);
        const bool bBodyOn = (g < static_cast<int>(m_vCleanExpanded.size()) && m_vCleanExpanded[g])
                             && pBody->GetFixedHeight() > 0;
        if (pBody->IsVisible() != bBodyOn) pBody->SetVisible(bBodyOn);
        // [修复 #20/#25] 过滤后可见集合变化，组框三态必须按真实勾选重算
        // （此前扫描后组框恒为"全选"，与被隐藏/手动取消的子项不一致）
        SyncCleanGroupCheck(g);
    }
}

void CPageCleanup::ApplyCleanFilter()
{
    const int nGroups = m_ctx.CleanEngine().GetGroupCount();
    const int nRules  = m_ctx.CleanEngine().GetRuleCount();
    // [修复 #20/#23] 数据态数组兜底（页面尚未重建也可能被调用）
    if (static_cast<int>(m_vCleanRowPass.size()) != nRules) m_vCleanRowPass.assign(nRules, 1);
    if (static_cast<int>(m_vUiSize.size()) != nRules) m_vUiSize.assign(nRules, CLEAN_SIZE_UNKNOWN);
    // [G12 2026-10-25] 统计过滤命中数：原实现只切换可见性，用户无法知道
    //   "这个关键字到底命中多少条"，只能靠滚动目测。这里顺手累计并写日志。
    int nHit = 0;
    int nVisibleTotal = 0;
    for (int i = 0; i < nRules; ++i)
    {
        const CleanRule& rule = m_ctx.CleanEngine().GetRule(i);
        CDuiString sRow;
        sRow.Format(L"clnrow_%d", i);
        CControlUI* pRow = m_ctx.HostFindControl(sRow.GetData());
        if (!pRow) continue;
        const bool bShow = ClnRowShouldShow(i, rule);
        m_vCleanRowPass[i] = bShow ? 1 : 0;  // [修复 #20] 数据态：与折叠无关
        pRow->SetVisible(bShow);
        if (bShow) ++nHit;
        ++nVisibleTotal;
    }
    // 仅在过滤生效时输出计数（无关键字时 nHit==nVisibleTotal，没必要刷屏）
    if (m_sCleanFilter.GetLength() > 0 || m_bCleanShowZero)
    {
        CDuiString sLog;
        sLog.Format(L"[cleanup] filter \"%s\": %d/%d matched",
                    m_sCleanFilter.GetData(), nHit, nVisibleTotal);
        AppendLog(sLog.GetData());
    }
    // recompute body heights; group headers always remain visible so the
    // group structure can be restored with the zero-size toggle
    ClnRecalcGroupsAfterFilter(nGroups);
    // relayout the tree
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(
        m_ctx.HostFindControl(L"clean_list"));
    if (!pList) return;
    pList->SetPos(pList->GetPos());
    if (pList->GetParent())
        pList->GetParent()->SetPos(pList->GetParent()->GetPos());
}

void CPageCleanup::ToggleCleanGroup(int g)

{
    CDuiString sBodyName;
    sBodyName.Format(L"clnbody_%d", g);
    CVerticalLayoutUI* pBody = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(sBodyName.GetData()));
    if (!pBody) return;
    CDuiString sArrowName;
    sArrowName.Format(L"clnarrow_%d", g);
    CTextUI* pArrow = static_cast<CTextUI*>(m_ctx.HostFindControl(sArrowName.GetData()));
    bool bShow = !pBody->IsVisible();
    if (g >= 0 && g < static_cast<int>(m_vCleanExpanded.size())) m_vCleanExpanded[g] = bShow;
    // [B89 修复] 展开一个当前无可见行的组体时，其固定高度为 0，会被内核当作
    // 自适应填充项吞掉列表空间；保持隐藏，待过滤恢复有行时再由 ApplyCleanFilter 展开
    if (bShow && pBody->GetFixedHeight() <= 0) bShow = false;
    pBody->SetVisible(bShow);
    if (pArrow) pArrow->SetText(m_ctx.HostGlyphArrow(bShow ? 1 : 0));
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"clean_list"));
    if (pList)
    {
        pList->SetPos(pList->GetPos());
        if (pList->GetParent()) pList->GetParent()->SetPos(pList->GetParent()->GetPos());
    }
}

void CPageCleanup::RefreshGroupTotals()

{
    // [修复 #23] 汇总只累加 UI 线程快照（WM_SCAN_ITEM 投递），不再读
    // 扫描线程正在写的 CleanRule::ullSize（x86 下 64 位读写非原子）。
    int nRules = m_ctx.CleanEngine().GetRuleCount();
    if (static_cast<int>(m_vUiSize.size()) != nRules) m_vUiSize.assign(nRules, CLEAN_SIZE_UNKNOWN);
    ULONGLONG ullGrand = 0;
    for (int g = 0; g < m_ctx.CleanEngine().GetGroupCount(); ++g)
    {
        CDuiString strGroup;
        if (!m_ctx.CleanEngine().GetGroup(g, strGroup)) continue;
        for (int i = 0; i < nRules; ++i)
        {
            if (m_ctx.CleanEngine().GetRule(i).sGroup != strGroup) continue;
            if (m_vUiSize[i] != CLEAN_SIZE_UNKNOWN)
                ullGrand += m_vUiSize[i];
        }
    }
    CDuiString sGrand;
    CCleanEngine::FormatSize(ullGrand, sGrand);
    CDuiString s;
    s.Format(StringMgr.Get("misc.sc972e18f"), sGrand.GetData());
    m_ctx.HostSetCtrlText(L"sess_status", s.GetData());
}

// [R154] 批量设置一组控件的启用状态（从 SetWorkerUiBusy 抽出）。
void CPageCleanup::ClnEnableControls(const wchar_t* const* ppszNames, size_t nCount, bool bEnabled)
{
    for (size_t i = 0; i < nCount; ++i)
        if (CControlUI* p = m_ctx.HostFindControl(ppszNames[i]))
            p->SetEnabled(bEnabled);
}

// 扫描进行中的按钮态：btn_clean/btn_refresh 禁用，btn_scan 保留可用且文案改为
// "停止扫描"充当取消入口（R17：扫描可取消）。
void CPageCleanup::ClnSetScanningButtons()
{
    if (CControlUI* p = m_ctx.HostFindControl(L"btn_clean"))   p->SetEnabled(false);
    if (CControlUI* p = m_ctx.HostFindControl(L"btn_refresh")) p->SetEnabled(false);
    if (CButtonUI* p = static_cast<CButtonUI*>(m_ctx.HostFindControl(L"btn_scan")))
    {
        p->SetEnabled(true);
        p->SetText(StringMgr.Get("cleanup.s9f6ffc8e"));
    }
}

void CPageCleanup::SetWorkerUiBusy(bool bBusy, bool bScanMode)
{
    // [D1] 底部状态条同步忙碌文案
    if (CTextUI* pStatus = static_cast<CTextUI*>(m_ctx.HostFindControl(L"app_status")))
    {
        pStatus->SetText(!bBusy ? StringMgr.Get("status.ready")
            : bScanMode ? StringMgr.Get("cleanup.s6c8ae46e")
                        : StringMgr.Get("cleanup.sd235e043"));
    }
    // 扫描/清理期间统一锁住：左侧导航、顶部菜单、清理页底栏辅助按钮，
    // 防止任务中途切页或触发二次操作。
    static const wchar_t* s_chrome[] = {
        L"menu_file", L"menu_recover", L"menu_option", L"menu_help"
    };
    static const wchar_t* s_clnAux[] = {
        L"btn_cln_default",
        L"btn_cln_refresh", L"btn_cln_export"
    };
    for (int n = 0; n <= 10; ++n)
    {
        CDuiString sNav;
        sNav.Format(L"nav_%d", n);
        if (CControlUI* p = m_ctx.HostFindControl(sNav.GetData()))
            p->SetEnabled(!bBusy);
    }
    ClnEnableControls(s_chrome, _countof(s_chrome), !bBusy);
    ClnEnableControls(s_clnAux, _countof(s_clnAux), !bBusy);
    if (bBusy && bScanMode)
    {
        ClnSetScanningButtons();
        return;
    }
    static const wchar_t* s_ids[] = { L"btn_scan", L"btn_clean", L"btn_refresh" };
    ClnEnableControls(s_ids, _countof(s_ids), !bBusy);
    if (bBusy) return;
    if (CButtonUI* p = static_cast<CButtonUI*>(m_ctx.HostFindControl(L"btn_scan")))
        p->SetText(StringMgr.Get("cleanup.scan"));
}

void CPageCleanup::ShowCleanDesc(int iRule)

{
    if (iRule < 0 || iRule >= m_ctx.CleanEngine().GetRuleCount()) return;
    CleanRule& rule = m_ctx.CleanEngine().GetRule(iRule);
    // BUG-C1: clean_desc 在 XML 中为 RichEdit，必须用 CRichEditUI*，
    // 否则 CTextUI 与 CRichEditUI vtable 不同 → 通过错误类型调 SetText 是未定义行为
    CRichEditUI* pDesc = static_cast<CRichEditUI*>(m_ctx.HostFindControl(L"clean_desc"));
    if (pDesc)
    {
        // RichEdit 的 m_bWordWrap 构造默认 false，且 XML 不支持 wordwrap 属性，必须代码设置
        pDesc->SetWordWrap(true);
        CDuiString sText = rule.sName;
        sText += L"\r\n\r\n";
        sText += rule.sDesc.IsEmpty() ? StringMgr.Get("hint.s8bfb557d") : rule.sDesc;
        // [修复 R5 / N-3] Custom 动作未接通时，说明面板必须明示能力边界，
        // 避免用户以为勾选并清理后该项会真正生效。
        if (!rule.sSpecialProc.IsEmpty() && !rule.bProcSupported
            && !rule.sProcNote.IsEmpty())
        {
            sText += L"\r\n\r\n⚠ ";
            sText += rule.sProcNote;
        }
        // [E-4] 展示当前适用分支携带的 Warning 文案（原版数据中的风险提示
        // 此前在本页任何地方都不展示），只展示当前系统真正会走到的分支。
        std::vector<CDuiString> vWarn;
        m_ctx.CleanEngine().CollectRuleWarnings(iRule, vWarn);
        if (!vWarn.empty())
        {
            for (size_t wi = 0; wi < vWarn.size(); ++wi)
            {
                sText += L"\r\n\r\n⚠ ";
                sText += vWarn[wi];
            }
        }
        pDesc->SetText(sText.GetData());
    }
}

void CPageCleanup::OnCleanRowCheck(CCheckBoxUI* p, int a)

{
    m_ctx.HostRefreshCheckText(p);
    // sync the owning group checkbox with its visible items
    if (a >= 0 && a < m_ctx.CleanEngine().GetRuleCount())
    {
        // [修复 R5 / N-3] 勾选未接通的 Custom 项时即时提示
        const CleanRule& ruleChk = m_ctx.CleanEngine().GetRule(a);
        if (p && p->GetCheck()
            && !ruleChk.sSpecialProc.IsEmpty()
            && !ruleChk.bProcSupported
            && !ruleChk.sProcNote.IsEmpty())
        {
            CDuiString sNote;
            sNote.Format(L"[%s] %s",
                ruleChk.sName.GetData(), ruleChk.sProcNote.GetData());
            AppendLog(sNote.GetData());
        }
        CDuiString sRuleGroup = m_ctx.CleanEngine().GetRule(a).sGroup;
        for (int gg = 0; gg < m_ctx.CleanEngine().GetGroupCount(); ++gg)
        {
            CDuiString sg;
            if (m_ctx.CleanEngine().GetGroup(gg, sg) && sg == sRuleGroup)
            {
                SyncCleanGroupCheck(gg);
                break;
            }
        }
    }
}

void CPageCleanup::OnScanItem(ScanItemMsg* m)

{
    if (m)
    {
        // [C++17 R3] receiver takes ownership, freed at scope end (was: delete m at end)
        std::unique_ptr<ScanItemMsg> mGuard(m);
        // [修复 #23] 大小只经消息跨线程：存 UI 独占快照，后续过滤/汇总读它
        if (m->iIdx >= 0 && m->iIdx < static_cast<int>(m_vUiSize.size()))
            m_vUiSize[m->iIdx] = m->ullSize;
        CDuiString sName;
        sName.Format(L"csz_%d", m->iIdx);
        CDuiString sSize;
        CCleanEngine::FormatSize(m->ullSize, sSize);
        m_ctx.HostSetCtrlText(sName.GetData(), sSize.GetData());
        RefreshGroupTotals();
        // live zero-size recognition for the rules scanned so far
        m_nCleanScannedCnt = m->iIdx + 1;
        ApplyCleanFilter();
        // [A3] 累加错误码分类统计（成功项 nErrorCode=ScanRuleError::SCAN_RULE_OK=0 会被跳过）
        if (m->nErrorCode >= 1 && m->nErrorCode < static_cast<int>(ScanRuleError::SCAN_RULE_ERROR_COUNT))
            ++m_nScanErrStats[m->nErrorCode];
        // [R17] 规则级进度：状态栏显示 “正在扫描 (k/n) 规则名”
        // [E-6] 进度条数值按 Scan@Work 权重走（重规则占比大）；
        // 旧消息无权重信息时回退规则条数。
        if (CleanJobService::IsScanning())
        {
            // [AUDIT-F6] 分母改用「本系统适用」规则数：原为全部规则数，
            // 尾部含不适用规则时 k 永远到不了 n（如收尾仍显示 "(97/120)"）。
            int nTotal = 0;
            for (int nti = 0; nti < m_ctx.CleanEngine().GetRuleCount(); ++nti)
                if (m_ctx.CleanEngine().GetRule(nti).bOsApplicable) ++nTotal;
            if (nTotal <= 0) nTotal = m_ctx.CleanEngine().GetRuleCount();
            // k 用线程带来的适用序数（跳过不适用规则计数），
            // 与分母严格同口径；旧消息无序数时回退索引（行为不变）。
            const int nOrdinal = m->nRuleOrdinal > 0 ? m->nRuleOrdinal : m->iIdx + 1;
            CDuiString sStatus;
            sStatus.Format(StringMgr.Get("cleanup.s3bd78ee6"),
                           nOrdinal, nTotal,
                           m_ctx.CleanEngine().GetRule(m->iIdx).sName.GetData());
            // S-14: 状态条前缀加"[扫描中]"阶段标签
            CDuiString sPhase = StringMgr.Get("phase.scanning");
            CDuiString sStatusWithPhase;
            sStatusWithPhase.Format(L"[%s] %s", sPhase.GetData(), sStatus.GetData());
            m_ctx.HostSetCtrlText(L"sess_status", sStatusWithPhase.GetData());
            if (m->nTotalWork > 0)
                m_ctx.HostProgress(m->nDoneWork, m->nTotalWork);
            else
                m_ctx.HostProgress(m->iIdx + 1, nTotal);
        }
        // [C++17 R3] mGuard frees m at scope end
    }
}

// [R154] 收集本组内「已扫描且已知大小」的行索引，按大小降序（大在前）。
// 未知大小的行由调用方按索引序追加到尾部。
void CPageCleanup::ScGatherSizedRows(const CDuiString& strGroup, int nRules, std::vector<int>& vIdx)
{
    for (int i = 0; i < nRules; ++i)
    {
        const CleanRule& rule = m_ctx.CleanEngine().GetRule(i);
        if (rule.sGroup != strGroup) continue;
        // [AUDIT-F4] 与 BdBuildGroup 同口径：!bOsApplicable 的规则没有建行，
        // 混入 vIdx 会产生"幻影索引"——ScApplyRowOrder 按 pos 位移时遇到
        // 无控件项跳过但 pos 继续前进，后续真实行的 SetItemIndex 目标位
        // 超过容器子项数：DuiLib 先 Remove 再 InsertAt，越界插入失败后
        // 该行已从容器移除且不再插回 → 行从 UI 永久消失并泄漏。
        if (!rule.bOsApplicable) continue;
        if (m_vUiSize[i] == CLEAN_SIZE_UNKNOWN) continue;
        vIdx.push_back(i);
    }
    std::stable_sort(vIdx.begin(), vIdx.end(),
        [this](int a, int b) { return m_vUiSize[a] > m_vUiSize[b]; });
    // 未扫描/未知大小的行按索引序追加到尾部
    for (int i = 0; i < nRules; ++i)
    {
        const CleanRule& rule = m_ctx.CleanEngine().GetRule(i);
        if (rule.sGroup != strGroup) continue;
        if (!rule.bOsApplicable) continue;   // [AUDIT-F4] 同上：无行规则不入序
        if (m_vUiSize[i] == CLEAN_SIZE_UNKNOWN) vIdx.push_back(i);
    }
}

// 按 vIdx 顺序把行搬到目标位置（SetItemIndex 为容器内移除+插入）。
void CPageCleanup::ScApplyRowOrder(CVerticalLayoutUI* pBody, const std::vector<int>& vIdx)
{
    for (int pos = 0; pos < static_cast<int>(vIdx.size()); ++pos)
    {
        CDuiString sRow;
        sRow.Format(L"clnrow_%d", vIdx[pos]);
        CControlUI* pRow = m_ctx.HostFindControl(sRow.GetData());
        if (pRow && pBody->GetItemIndex(pRow) != pos)
            pBody->SetItemIndex(pRow, pos);
    }
}

// 排序后重算各组高度 + 同步空组体可见性 + 触发重排。
void CPageCleanup::ScRecalcAfterSort(int nGroups)
{
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(
        m_ctx.HostFindControl(L"clean_list"));
    if (static_cast<int>(m_vCleanExpanded.size()) < nGroups)
        m_vCleanExpanded.resize(nGroups, false);
    for (int g = 0; g < nGroups; ++g)
    {
        CDuiString sBody;
        sBody.Format(L"clnbody_%d", g);
        CVerticalLayoutUI* pBody = static_cast<CVerticalLayoutUI*>(
            m_ctx.HostFindControl(sBody.GetData()));
        if (!pBody) continue;
        RecalcBodyHeight(pBody);
        // [B89 修复] 排序与重算后同步空组体可见性（高度 0 的可见组体会被内核
        // 当作自适应填充项吞掉滚动空间，造成大块空白）
        const bool bBodyOn = (g < static_cast<int>(m_vCleanExpanded.size()) && m_vCleanExpanded[g])
                             && pBody->GetFixedHeight() > 0;
        if (pBody->IsVisible() != bBodyOn) pBody->SetVisible(bBodyOn);
    }
    if (!pList) return;
    pList->SetPos(pList->GetPos());
    if (pList->GetParent())
        pList->GetParent()->SetPos(pList->GetParent()->GetPos());
}

void CPageCleanup::SortCleanRows()
{
    const int nGroups = m_ctx.CleanEngine().GetGroupCount();
    const int nRules  = m_ctx.CleanEngine().GetRuleCount();
    if (static_cast<int>(m_vUiSize.size()) != nRules)
        m_vUiSize.assign(nRules, CLEAN_SIZE_UNKNOWN);
    for (int g = 0; g < nGroups; ++g)
    {
        CDuiString strGroup;
        if (!m_ctx.CleanEngine().GetGroup(g, strGroup)) continue;
        CDuiString sBody;
        sBody.Format(L"clnbody_%d", g);
        CVerticalLayoutUI* pBody = static_cast<CVerticalLayoutUI*>(
            m_ctx.HostFindControl(sBody.GetData()));
        if (!pBody) continue;
        std::vector<int> vIdx;
        ScGatherSizedRows(strGroup, nRules, vIdx);
        ScApplyRowOrder(pBody, vIdx);
    }
    ScRecalcAfterSort(nGroups);
}

void CPageCleanup::OcRefreshDescPanel(const CleanDoneMsg* m)

{
    // 本次真正参与删除的规则集合（未选中/不可见的一律不计入报告）
    const std::vector<int>& vSel = m_vLastCleanSel;
// ===== C-5 结果摘要 + C-3 明细面板回写 clean_desc =====
// BUG-C1: clean_desc 是 RichEdit，必须用 CRichEditUI*（CTextUI* 会触发未定义行为）
CRichEditUI* pDesc = static_cast<CRichEditUI*>(m_ctx.HostFindControl(L"clean_desc"));
if (pDesc && m)
{
    pDesc->SetWordWrap(true);   // RichEdit 默认不换行，且 XML 无法设置
    CDuiString sDetail;
    // C-5 摘要
    CDuiString sFreed;
    CCleanEngine::FormatSize(m->ullFreed, sFreed);
    int nTotalDel = 0;  // 本次删除涉及的路径总数（仅勾选规则）
    for (size_t si = 0; si < vSel.size(); ++si)
        nTotalDel += static_cast<int>(m_ctx.CleanEngine().GetRule(vSel[si]).targets.size());
    CDuiString sHead;
    sHead.Format(StringMgr.Get("cleanup.s239af6c3"),
                 m->nRules, nTotalDel,
                 nTotalDel - m->nFailed, m->nFailed,
                 sFreed.GetData());
    sDetail += sHead;
    sDetail += StringMgr.Get("cleanup.s4f10a505");
    // C-3 明细：面板仅显示前 200 条；C-8 全量另存到 m_sCleanFullDetail 供导出
    CDuiString sFull;
    sFull += sHead;
    sFull += StringMgr.Get("cleanup.sd5a2ec30");
    int nShown = 0;
    constexpr int MAX_SHOWN = 200;
    for (size_t si = 0; si < vSel.size(); ++si)
    {
        const CleanRule& r = m_ctx.CleanEngine().GetRule(vSel[si]);
        for (size_t t = 0; t < r.targets.size(); ++t)
        {
            const CleanTarget& tgt = r.targets[t];
            bool bGone = ::GetFileAttributes(tgt.sPath.GetData()) == INVALID_FILE_ATTRIBUTES;
            CDuiString sOne;
            sOne.Format(L"%c %s\n", bGone ? L'✓' : L'✗', tgt.sPath.GetData());
            sFull += sOne;
            if (nShown < MAX_SHOWN) sDetail += sOne;
            ++nShown;
        }
    }
    if (nShown > MAX_SHOWN)
    {
        CDuiString sMore;
        sMore.Format(StringMgr.Get("export.sda818aac"),
                     MAX_SHOWN, nShown);
        sDetail += sMore;
    }
    m_sCleanFullDetail = sFull;          // C-8 全量缓存
    m_bCleanHasReport = true;
    pDesc->SetText(sDetail.GetData());
}
}

// ============================================================
// [R220] 主题切换：按语义重设本页自建控件的颜色。
//
//   为什么必须写在本页、而不是壳里做一次通用树遍历：
//   BdBuildRuleRow / BdBuildGroup 里写的 SetTextColor(tokens::Token(X)) 存的是
//   **解析后的 ARGB**，切到深色主题后控件仍按旧 ARGB 绘制；而通用遍历只能看到
//   结果色，无法反推它来自哪个令牌。风险徽章尤其如此——它按 nLevel 语义取色
//   （Danger / Warn+WarnBg / Success+OnAccent），必须按等级重算。
//
//   契约（PageBase.h:84）：只改颜色属性，不 Build/Add/Remove。
//   本页没有 m_vCapRowPtr 那样的行指针缓存，但控件名确实存在
//   （clnrow_/csz_/crisk_/clnarrow_ 均由 Bd* 显式 SetName），故按名查找；
//   上界用 CleanEngine 的规则数/组数——不能用 m_vCleanExpanded，
//   它只在 Build 成功后才有内容。
// ============================================================
void CPageCleanup::RefreshThemeColors()
{
    const DWORD dwSub   = tokens::Token(tokens::EToken::InkSecondary);
    const DWORD dwMuted = tokens::Token(tokens::EToken::InkMuted);

    int nRows = 0, nBadges = 0, nArrows = 0;

    const int nRules = m_ctx.CleanEngine().GetRuleCount();
    for (int i = 0; i < nRules; ++i)
    {
        // 行是否存在 = 该规则是否建了行：OS-Filter 不适用（bOsApplicable==false）
        // 的规则根本不建行，按名查找会落空，这里以行命中与否作为判据。
        CDuiString sRowName;
        sRowName.Format(L"clnrow_%d", i);
        if (!m_ctx.HostFindControl(sRowName.GetData())) continue;
        ++nRows;

        // 状态列：InkSecondary
        CDuiString sSzName;
        sSzName.Format(L"csz_%d", i);
        if (CTextUI* pStatus = dynamic_cast<CTextUI*>(
                m_ctx.HostFindControl(sSzName.GetData())))
            pStatus->SetTextColor(dwSub);

        // 风险徽章：重调三态着色，语义取色（字/底/文案）整体重算，
        // 而不是「把当前颜色换成另一个颜色」。
        CDuiString sRiskName;
        sRiskName.Format(L"crisk_%d", i);
        if (CTextUI* pRisk = dynamic_cast<CTextUI*>(
                m_ctx.HostFindControl(sRiskName.GetData())))
        {
            BdStyleRiskBadge(pRisk, m_ctx.CleanEngine().GetRule(i).nLevel);
            ++nBadges;
        }
    }

    const int nGroups = m_ctx.CleanEngine().GetGroupCount();
    for (int g = 0; g < nGroups; ++g)
    {
        // 组头折叠箭头：InkMuted
        CDuiString sArName;
        sArName.Format(L"clnarrow_%d", g);
        if (CTextUI* pArrow = dynamic_cast<CTextUI*>(
                m_ctx.HostFindControl(sArName.GetData())))
        {
            pArrow->SetTextColor(dwMuted);
            ++nArrows;
        }
    }

    // 行底色来自 CClickRowUI 构造（Surface）；本页从不调 SetRowSelected，
    // 不会有 Accent 选中底的残留，故只重设 Surface 一路即可。
    // 复选框文字色由共享 StyleCheckBox（FrameWnd.cpp:697，Ink）负责，属壳级
    // 共享样式，壳会在主题切换时统一重刷，本页不重复设置。
    const DWORD dwSurface = tokens::Token(tokens::EToken::Surface);
    int nRowBk = 0;
    for (int i = 0; i < nRules; ++i)
    {
        CDuiString sRowName;
        sRowName.Format(L"clnrow_%d", i);
        if (CClickRowUI* pRow = dynamic_cast<CClickRowUI*>(
                m_ctx.HostFindControl(sRowName.GetData())))
        {
            pRow->SetBkColor(dwSurface);
            ++nRowBk;
        }
    }

    // [R323] 分组头 clngrp_* 也必须重刷。
    //   实测：切到浅色后，内容区 y=440 与 y=720 两条整宽带仍是 95.9% 的
    //   RGB(32,32,32)（= 深色 Surface）。原因就是这里只遍历了规则行
    //   clnrow_*，分组头 clngrp_* 从未被覆盖 —— 它的底色同样来自
    //   tokens::Surface，是代码在建行时取的旧值。
    //   分组头数量与规则行不同（按组数），必须单独循环。
    int nGrpBk = 0;
    const int nGrpTotal = m_ctx.CleanEngine().GetGroupCount();
    for (int g = 0; g < nGrpTotal; ++g)
    {
        CDuiString sGrpName;
        sGrpName.Format(L"clngrp_%d", g);
        if (CClickRowUI* pGrp = dynamic_cast<CClickRowUI*>(
                m_ctx.HostFindControl(sGrpName.GetData())))
        {
            pGrp->SetBkColor(dwSurface);
            ++nGrpBk;
        }
    }

    CDuiString sLog;
    sLog.Format(L"[cleanup][theme] recolored rows=%d badges=%d arrows=%d rowbk=%d grpbk=%d",
                nRows, nBadges, nArrows, nRowBk, nGrpBk);
    AppendLog(sLog.GetData());
}
