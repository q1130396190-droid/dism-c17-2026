#pragma once

// ============================================================
// Page_Cleanup —— P6b「系统清理」页（最高风险切片，方案 S10）
//
// 承接原 CFrameWnd 的清理页全部 UI 函数：
//   BuildCleanupPage / ApplyCleanFilter / ToggleCleanGroup /
//   RefreshGroupTotals / StartScan(UI 段) / StartClean(UI 收集段) /
//   RunCustomProcAction / CleanPackageCacheSafe / ExportCleanReport /
//   ToggleSelectAll / CleanSelectNone / CleanSelectDefault /
//   ShowCleanDesc /
//   OnGroupCheck / SyncCleanGroupCheck / SetWorkerUiBusy
// 以及 WM_SCAN_ITEM / WM_SCAN_DONE / WM_CLEAN_DONE 三个消息处理。
//
// 扫描/删除工作线程、取消令牌、CustomJobs 队列归 CleanJobService。
// 物理搬迁护栏：函数体零逻辑改动，宿主引用一律改 m_ctx.Xxx，
// 壳成员改本页成员或 CleanJobService 静态访问器。
// ============================================================

#include <vector>
#include <atomic>   // [FIX-P2A] 尾巴双标志跨线程原子化

#include "PageBase.h"

#include "CleanJobService.h"   // ScanItemMsg / CleanDoneMsg / CustomProcIdx
#include "CleanExtend.h"       // CleanExtendBigFile / CleanExtendEmptyDir

// [R14-02] Build 拆分辅助的指针/引用形参（完整定义在各自 .cpp 引入）
class CClickRowUI;
struct CleanRule;

// [AUDIT-P0] 清理后 DISM 尾巴的工作线程上下文与结果。
// 由 UI 线程 new 并移交线程过程；线程完成后经 WM_CLEAN_DISMTAIL 回投，
// UI 线程在 OnCleanDismTail 内 delete（kMsgOwnPage 约定）。
struct TPendingCleanTail
{
    HWND          hNotify          = nullptr;
    void*         pPage            = nullptr;   // CPageCleanup*（避免头文件前置声明顺序问题）
    bool          bDismPending    = false;     // WinSxS 组件清理挂起标志
    bool          bResetBase       = false;     // 用户确认了 ResetBase
    bool          bAssocNotify     = false;     // 需发 SHChangeNotify
    bool          bRestartExplorer = false;     // 需询问重启资源管理器
    // [R18] 部分完成标记：尾阶段抛出异常时置位，UI 据此区分
    // "全部完成" 与 "部分完成"，避免把只执行了一半的自定义动作报成成功。
    bool          bPartial         = false;
};

class CPageCleanup : public CPageBase

{

public:

    explicit CPageCleanup(IPageContext& ctx);

    void Build() override;   // 原 BuildCleanupPage

    // [R220] 主题切换后按语义重设本页代码创建控件的颜色。
    //   契约（PageBase.h:84）：只许重设颜色属性，禁止 Build()/Add()/Remove()——
    //   否则丢列表内容、勾选态、滚动位置，并让 m_pFocusCtrl 悬挂。
    void RefreshThemeColors() override;

    // [R3] 清理页不在首次切页时懒构建；DPI/最大化后需整树重建
    void OnDeferredBuild() override {}
    void OnRebuildDynamicTree() override { Build(); }
    bool NeedsFullRebuildOnRelayout() const override { return true; }

    // [R4] 壳命令收口：F9 扫描 / F10 清理 / Esc、Ctrl+Break 取消
    bool OnHostCommand(int id) override;

    // [R5] 扫描线程消息统一入口（WM_SCAN_ITEM 本页 delete；WM_SCAN_DONE 为值参）。
    //   WM_CLEAN_DONE 含壳 RestartExplorer 两段时序，走下方
    //   HostOnCleanDone/HostOnCleanDoneTail。
    bool OnAsyncMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) override;

    // [AUDIT-P0] 清理完成后的 DISM 尾巴（Custom 派发 + WinSxS 组件清理）
    // 原在 WM_CLEAN_DONE 回调里同步执行：CleanComponentStore 走 CBS 事务，
    // 实测可达数分钟，期间 UI 完全冻结且无法取消。现改为工作线程执行 +
    // WM_CLEAN_DISMTAIL 回投，UI 只做结果收口。
    void StartCleanDismTailAsync(bool bDismPending, bool bResetBase,
                                 bool bAssocNotify, bool bRestartExplorer);
    void OnCleanDismTail(struct TPendingCleanTail* pTail);
    // [R5] WM_CLEAN_DONE 两段时序（lParam=CleanDoneMsg*，本页不 delete）
    [[nodiscard]] bool HostOnCleanDone(LPARAM lParam) override;
    void HostOnCleanDoneTail() override;
    // [FIX-UNFREEZE] 尾巴看门狗：Post 全败时兜底收尾（1s 轮询 InFlight）
    void OnHostTimer(UINT uId) override;

    // [R-CLEANPROG] 删除阶段进度（WM_CLEAN_PROGRESS，lParam=CleanProgressMsg*，
    // 本页 delete）：驱动会话条进度 + 状态条百分比文案。
    void OnCleanProgress(struct CleanProgressMsg* pMsg);

    // P8：第二级事件路由（原壳 Notify / WM_RBUTTONUP 本页分支）
    void OnEvent(TNotifyUI& msg) override;

    // [AUDIT-P2] OnEvent 原 71 行 / CC=27，按事件类型拆为 4 个处理函数。
    // 路由语义逐字保留（按钮表、clngrp_/clnrow_ 前缀、组勾选三态联动）。
    void ClnOnClick(TNotifyUI& msg, const CDuiString& sName);
    // [R165] ClnOnClick 拆分（原 31 行 / CC16）
    bool ClnDispatchButton(const CDuiString& sName);
    // [R165] OnContextMenu 拆分（原 36 行 / CC15）
    [[nodiscard]] int ClnResolveRowFromHit(CControlUI* pHit);
    void ClnOnDoubleClick(const CDuiString& sName);
    void ClnOnFilterChanged(TNotifyUI& msg, const CDuiString& sName);
    void ClnOnSelectChanged(TNotifyUI& msg, const CDuiString& sName);
    bool OnContextMenu(POINT pt, bool bFromContextMsg) override;

    // ---- 壳薄转发具名入口（签名与原 CFrameWnd 同名函数一致）----
    void ApplyCleanFilter();
    // [R154] ApplyCleanFilter 拆分（原 66 行 / CC20）
    [[nodiscard]] bool ClnRowShouldShow(int i, const CleanRule& rule) const;
    void ClnRecalcGroupsAfterFilter(int nGroups);

    void ToggleCleanGroup(int g);

    void RefreshGroupTotals();

    void StartScan();

    void StartClean();

    // [R14-02] StartClean 规则收集上下文（UI 收集阶段快照）
    struct SCCollect
    {
        std::vector<int> sel;
        std::vector<int> vWarnRules;
        bool             bFileChecked = false;
        int              nHiddenSkipped = 0;
        bool             bDismResetBase = false;
        bool             bCleanAssocNotify = false;
        bool             bCleanRestartExplorer = false;
        // [R-CLEANPROG] 勾选文件规则的权重和（删除线程进度分母）
        int              nFileWork = 0;
    };

    // [R14-02] StartClean 拆解：磁盘采样 / 规则收集 / 空选择检查 / 清理前确认 / 启动收尾
    // [AUDIT-F2] 磁盘采样接收本轮真实勾选规则（多卷清单不再读上次遗留状态）
    void ScSampleDiskBefore(const std::vector<int>& vSel);
    void ScCollectCheckedRules(SCCollect& out);
    // [R154] ScCollectCheckedRules 拆分（原 70 行 / CC22）
    void ScCollectFileRule(int i, CCheckBoxUI* pCheck, const CleanRule& rule, SCCollect& out);
    void ScCollectCustomProc(int i, const CleanRule& rule, SCCollect& out);
    [[nodiscard]] bool ScCheckSelection(const SCCollect& r);  // 返回 false = 用户取消
    [[nodiscard]] bool ScConfirmWarnings(SCCollect& r);   // 返回 false = 用户取消
    void ScLaunchClean(SCCollect& r);

    // [R14-02] RunCustomProcAction 拆解
    bool ScRunNcleanerFirst(CustomProcIdx nIdx);      // NCleaner.dll 优先路径，返回 true 已消费
    void ScRunDismAppxs();
    void ScRunDismHardLinkMerge();
    // [R160] 从 ScRunDismHardLinkMerge 抽出：用户配置目录硬链接合并
    void ScMergeProfileHardLinks();
    void ScRunDismDriver();
    void ScRunDismCompact();
    // [R195] 从 ScRunDismCompact（76 行）抽出：WOF 压缩 + 事后复核
    void ScRunWofCompact();
    void ScRunNcInstaller();
    void ScRunNcCorruptAppx();
    void ScRunNcSrPoint();
    void ScRunNcDoCache();
    void ScRunNcPkgCache();
    void ScRunNcVsPkgCache();
    void ScRunNcEventLog();
    // [R183] 从 ScRunNcEventLog（67 行 / CC12）抽出：读下一频道 / 清空单频道。
    // 形参用 void* 而非 EVT_HANDLE —— 后者定义在 wevtapi.h，本头不引入该依赖；
    // EVT_HANDLE 本身就是 PVOID，类型等价，实现处仍写 EVT_HANDLE 便于阅读。
    [[nodiscard]] int  DiagNextChannelPath(void* hEnum, std::wstring& sCh);
    bool DiagClearOneChannel(const std::wstring& sCh);
    void ScRunNcSpooler();

    // [R14-02] OnCleanDone 拆解
    void OcWriteCleanHistory(const CleanDoneMsg* m);   // 内部采样磁盘空间 + 追加 clean_history.log
    // [R147] 从 OcWriteCleanHistory 拆出（降 CC / 行数）
    void OcCollectHistoryStats(CDuiString& sGroups, CDuiString& sRules,
                               CDuiString& sFailedPaths);   // 规则名/分组分布/失败路径收集
    bool OcAppendUtf8Line(const wchar_t* pszPath, const CDuiString& sLine);   // UTF-8 追加 + 首次写 BOM
    void OcRefreshDescPanel(const CleanDoneMsg* m);
    void OcZeroUiSizes(const CleanDoneMsg* m);
    void OcPushUndoStack(const std::vector<CDuiString>& vDeleted,
                         bool bToRecycleBin);   // delete m 前调用，避免悬垂指针
    void OcDispatchCustomActions();            // 派发 queued Custom 动作（跳过 CPI_DISMCOMP）
    void OcRunDismComponent(bool bPending, bool bResetBase);  // WinSxS 组件清理（[AUDIT-P0] 线程过程调用，故置 public）
    // [R160] 从 OcRunDismComponent 抽出：ResetBase 二次确认 / 组件清理执行与结果判定
    [[nodiscard]] bool OcConfirmDismResetBase(bool bResetBase);
    void OcExecDismComponent(bool bReset);

    void RunCustomProcAction(CustomProcIdx nIdx);

    void CleanPackageCacheSafe(bool bVsOnly);

    void ExportCleanReport();

    void ToggleSelectAll();

    // [AUDIT-F3] 右键菜单"反选"的真实动作（原菜单项只写日志无效果）
    void CleanSelectInvert();

    void CleanSelectNone();

    void CleanSelectDefault();

    void ShowCleanDesc(int iRule);

    void OnGroupCheck(int iGroup, bool bChecked);

    void SyncCleanGroupCheck(int iGroup);

    void SetWorkerUiBusy(bool bBusy, bool bScanMode = false);
    // [R154] SetWorkerUiBusy 拆分（原 57 行 / CC19）
    void ClnEnableControls(const wchar_t* const* ppszNames, size_t nCount, bool bEnabled);
    void ClnSetScanningButtons();

    // ---- Notify / 加速键内联块入口 ----
    // btn_scan：扫描中充当“停止扫描”（含按钮/状态条 UI），否则启动扫描
    void OnScanClicked();

    // Esc：扫描中请求取消（含状态条 UI）
    void RequestCancelScan();

    // clean_filter textchanged
    void OnFilterChanged(const wchar_t* pszText);

    // clean_showzero
    void OnShowZeroChanged(bool bShow);

    // ckclean_ selectchanged（未接通 Custom 项提示 + 组三态同步）
    void OnCleanRowCheck(DuiLib::CCheckBoxUI* pCheck, int a);

    // ---- 工作线程消息（壳 HandleCustomMessage 转发；消息结构本页 delete）----
    void OnScanItem(ScanItemMsg* pMsg);

    void OnScanDone(int nFailCount);

    // [R14-04] OnScanDone 拆解：状态收拢 / 取消报告 / 失败报告 / 成功报告 / 气泡 / 收尾
    void ScdUpdateScanState(int nRuleFailed, bool& bCancelled, int& nScanRules);
    void ScdCancelReport(int nScanRules, int nRuleFailed);
    // [R196] 从 ScdFailureReport（74 行 / CC9）抽出：日志明细 / RichEdit 追加
    void ScdLogFailureDetails(int nScanRules);
    void ScdAppendFailureToDesc(int nScanRules, int nRuleFailed);
    void ScdFailureReport(int nScanRules, int nRuleFailed);
    void ScdSuccessReport();
    void ScdScanBalloon(int nScanRules, int nRuleFailed);
    void ScdFinishUi();

    // 返回 true 表示用户已同意重启资源管理器，由壳在其后调用
    // RestartExplorer()（该能力为优化页/清理页共用，留壳且禁止新增宿主
    // 接口）；SHChangeNotify/询问弹窗本身仍在本页。
    [[nodiscard]] bool OnCleanDone(CleanDoneMsg* pMsg);

    // 壳执行完 RestartExplorer 后回调：解除忙碌态 + 完成气泡。
    // 拆分为两段是为保持原 handler“先重启 explorer、后解锁 UI/气泡”的
    // 严格时序。
    void OnCleanDoneTail();

    // [R80] Cleanup 增强：卸载残留扫描 / 撤销清理
    void OnExtendOrphanScan();
    void OnExtendUndoLast();

    // [R126-1] 扫描完成后按大小降序重排组内行（不改行名，仅调显示次序）
    void SortCleanRows();
    // [R154] SortCleanRows 拆分（原 69 行 / CC22）
    void ScGatherSizedRows(const CDuiString& strGroup, int nRules, std::vector<int>& vIdx);
    void ScApplyRowOrder(CVerticalLayoutUI* pBody, const std::vector<int>& vIdx);
    void ScRecalcAfterSort(int nGroups);

    const wchar_t* PageName() const override { return L"Cleanup"; }
    void    OnGlobalSelectAll() override;
    void    OnGlobalSave() override;

    // [AUDIT-P0] OnCleanDoneTail 幂等标志：壳与异步收口两条路径都会调用，
    // 必须保证真正收尾只执行一次。
    // [FIX-P2A] 尾巴双标志由裸 bool 改 std::atomic<bool>：跨 UI/尾巴线程
    // 读写消除标准层面的数据竞争 UB（x86 上行为等价，纯类型层修复）。
    std::atomic<bool> m_bCleanTailFinalized{ false };
    std::atomic<bool> m_bCleanTailInFlight{ false };   // DISM 尾巴工作线程在途（此期间不得收尾）
    // [R160] 收尾双标志的状态迁移集中入口。两者是同一协议的两位状态，
    //   原先 6 个写点分散在 Page_Cleanup.cpp 与 Page_CleanupTail.cpp 两个 TU，
    //   "复位时机"只写在注释里；新增任何复位路径都容易只改一半。
    void ResetTailGuards() { m_bCleanTailFinalized.store(false, std::memory_order_relaxed); m_bCleanTailInFlight.store(false, std::memory_order_release); }
    void MarkTailInFlight() { m_bCleanTailInFlight.store(true, std::memory_order_release); }
    void MarkTailFinished() { m_bCleanTailInFlight.store(false, std::memory_order_release); }
    // [FIX-P1K] 退出序列只读查询：DISM 尾巴（CBS 事务）在途时 ~CFrameWnd
    // 需有限等待，避免强杀损坏组件存储。
    bool IsCleanTailInFlight() const { return m_bCleanTailInFlight.load(std::memory_order_acquire); }
    // [R18-b] DISM 组件清理的"请求取消"标志：UI 线程置 1，清理线程只读取。
    // 因 DismCleanImage 是同步阻塞调用，此标志不能中断正在执行的调用，
    // 只能用于记录请求并阻止后续阶段启动，日志与结果中如实标注。
    volatile LONG m_bCleanDismCancel = 0;

    // [AUDIT-P0] 工作线程过程与 UI 收口都要写日志，故与线程入口一并置 public
    // 等价原 CFrameWnd::AppendLog：级别仍走 LogService::DetectLevel 自动识别
    void AppendLog(const wchar_t* pszLine);
private:

    // [R14-02] Build 拆分辅助（原 367 行单函数 → 薄壳+4 辅助，零行为改动）：
    //   BdBuildGroup 单组（组头+body+适用规则行）；BdBuildRuleRow 单条规则行；
    //   BdStyleRiskBadge 风险等级徽章着色；BdLogRiskOverview 风险分级总览日志。
    void BdBuildGroup(DuiLib::CVerticalLayoutUI* pList, int g);
    CClickRowUI* BdBuildRuleRow(const CleanRule& rule, int i);
    void BdStyleRiskBadge(DuiLib::CTextUI* pRiskBadge, int nLevel);
    void BdLogRiskOverview();

    // [修复 #20] 行是否通过关键字/零字节过滤（数据态）
    std::vector<char>      m_vCleanRowPass;

    // [修复 #23] UI 线程独占的每项大小快照（WM_SCAN_ITEM 值传递写入）
    std::vector<ULONGLONG> m_vUiSize;

    bool m_bCleanScanned;      // a full scan has finished

    int  m_nCleanScannedCnt;   // number of rules scanned so far (live zero-hide)
    // [G4 2026-10-25] 最近一次交互行：键盘唤出右键菜单时用来合成菜单锚点。
    //   WM_CONTEXTMENU 的键盘路径坐标是 (-1,-1)，FindControl 必然落空；
    //   原实现直接 return false，导致属性/复制/打开位置对键盘用户整片不可达。
    int  m_nClnLastRow = -1;

    // [A3] 上一次扫描的错误码分类统计（索引 = ScanRuleError::SCAN_RULE_ERROR_COUNT）
    int  m_nScanErrStats[6];

    CDuiString m_sCleanFilter;     // clean keyword filter

    bool       m_bCleanShowZero;   // keep showing zero-size / non-special items

    std::vector<bool> m_vCleanExpanded;

    CDuiString m_sCleanFullDetail; // C-8: 最近一次清理的全量明细（未截断），供导出

    bool       m_bCleanHasReport;  // C-8: 是否存在可导出的清理报告

    ULONGLONG  m_ullCleanBeforeBytes;  // [R60-A] 本次清理开始时的 C: 剩余空间
    // [R7] 多卷释放量统计：按规则实际涉及的卷分别采样前后可用空间。
    struct VolumeBefore { CDuiString sRoot; ULONGLONG ullFree = 0; };
    std::vector<VolumeBefore> m_vVolBefore;

    std::vector<int> m_vLastCleanSel;  // 最近一次清理实际勾选的规则（报告仅统计这些）

    // OnCleanDone → OnCleanDoneTail 之间缓存的完成结果（供结尾气泡）
    int        m_nLastCleanFailed;

    // [R18] 尾阶段是否部分完成：自定义动作/组件清理抛异常时置位，
    // 让结尾气泡区分"全部完成"与"部分完成"，避免误报成功。
    bool       m_bLastCleanPartial;

    ULONGLONG  m_ullLastCleanFreed;

    // [R-EXTASYNC] 增强扫描期间禁用入口按钮（现仅残留扫描一个入口）。
    void SetExtendButtonsBusy(bool bBusy);

};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
