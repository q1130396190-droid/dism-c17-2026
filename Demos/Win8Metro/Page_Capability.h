#pragma once

// ============================================================
// Page_Capability —— v2 P4 纵切页面（可选功能 FOD/Capabilities）
//
// 承接原 CFrameWnd 的 6 个函数：
//   BuildCapabilityPage / StartCapabilityEnumeration /
//   OnCapabilityData / BuildCapabilityList / OnCapUninstall
// 以及 CAP_REFRESH_TIMER_ID / CAP_RETRY_TIMER_ID 两个定时器分支
//（OnRefreshTimer / OnRetryTimer）和模块静态 s_sLastCapActionName/
// Type、m_bCapRetryPending、s_nCapAutoRefreshCount。
// 后台 DISM 枚举/解析归 AppxCapUpdService（S6），结果经 WM_CAP_DATA
// 由核心壳转发 OnData。
//
// 物理搬迁护栏：函数体零逻辑改动，宿主引用一律改 m_ctx.Xxx。
// ============================================================

#include "Utils/Utils.h"
#include "PageBase.h"
#include "AppxCapUpdService.h"   // CapEntry / CapDataMsg
#include "Page_Feature.h"       // [R5] Windows 功能子视图（代理其消息/定时/动作）

// ============================================================
// [R200] 可选功能智能筛选分类
//   All/Other 之外按 Identity 家族静态判定（不看描述，避免刷新后跳类）。
// ============================================================
enum class ECapCategory : int
{
    All      = 0,
    Fod      = 1,   // 功能包（打印/传真、盲文、无线显示…）
    LangPack = 2,   // 语言包（Language.Basic / Fonts / OCR / TTS…）
    Rsat     = 3,   // 远程服务器管理工具
    Tools    = 4,   // 开发工具（开发者模式 / SSH / WebDriver…）
    Other    = 5
};

class CPageCapability : public CPageBase
{
public:
    explicit CPageCapability(IPageContext& ctx)
        : CPageBase(ctx), m_bLoading(false),
          m_bRetryPending(false), m_nAutoRefreshCount(0),
          m_pFeature(nullptr),
          m_nRenderPos(0), m_dwEnumStartTick(0), m_dwRenderStartTick(0),
          // [A-4 2026-10-25] 首值 120s → 15s。120s 意味着前十来秒用户完全无反馈，
          //   这正是"加载超时"观感的来源；15s 足够覆盖正常枚举（本机 0.3~1.2s，
          //   慢机 1.5~6s），超时即提示"正在使用上次结果/可手动刷新"。
          //   后续仍按上次实测耗时×2 自适应（OnData 内），首值只影响第一次。
          m_nWatchdogMs(15000) {}

    void Build() override { BuildPage(); }

    // [R3] F5 刷新=重建功能列表（非整页 Build）
    void OnRefreshPage() override { BuildList(); }

    // [R5] CPageHost 装配后绑定 Windows 功能子视图（Feature 不占页序，
    // PAGE_FEATURE=7 的页表项即本页，下列入口代理到子视图）。
    void BindFeature(CPageFeature* pFeature) { m_pFeature = pFeature; }

    // [R5] 后台消息：WM_CAP_DATA 本页消费；WM_FEAT_DATA/WM_FEAT_APPLYDONE
    // 代理内部 Windows 功能子视图（FeatDataMsg/FeatApplyMsg 由子视图 delete）。
    bool OnAsyncMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) override;
    // [R5] CAP_* 定时器本页处理；FEAT_WATCHDOG 代理子视图。
    void OnHostTimer(UINT uId) override;
    // [R5] prog_pages Tab1 功能子视图动作代理
    void HostFeatureTabChanged(int idx) override;
    void HostFeatureLoad() override;
    void HostFeatureDeleteSelected() override;
    void HostFeatureApplyFilter(const CDuiString& sKeyword, bool bNoKw) override;
    // [修复] Tab1 功能行/按钮事件与右键菜单代理
    void HostFeatureEvent(TNotifyUI& msg) override;
    bool HostFeatureContextMenu(POINT pt, bool bFromContextMsg) override;

    // P8：第二级事件路由（原壳 Notify 本页分支）
    void OnEvent(TNotifyUI& msg) override;

    void BuildPage();                  // 原 BuildCapabilityPage
    void StartEnumeration();           // 原 StartCapabilityEnumeration
    void OnData(CapDataMsg* pData);    // 原 OnCapabilityData

    // [R162] 从 OnData 抽出：旧条目补齐新条目展示字段 + 统计状态变化数。
    //   6 条"新值为空则沿用旧值"是 [B2] 修复"越刷新越空"的核心契约，集中一处。
    [[nodiscard]] int MergeDisplayFields(std::vector<CapEntry>& vNew);

    // [R162] 从 OnData 抽出：枚举失败呈现（错误码映射 + 缓存兜底 + 错误行渲染）。
    //   返回 true 表示已用缓存兜底渲染，调用方直接返回。
    [[nodiscard]] bool RenderEnumError(int nExitCode);

    // [R165] 从 StartEnumeration 抽出：三级缓存读取 + 短路标志
    // [R165] 从 OnUninstall 抽出：卸载确认文案（[R45]/[R56] 依赖提示）
    [[nodiscard]] CDuiString CapBuildUninstallConfirm(const CapEntry& e);

    // [R169] 从 RenderBatch 抽出：渲染性能采样（ttfr / total）
    void CapLogRenderPerf(bool bFirstRow, int nTotal);

    // [R175] 从 BuildList 抽出：可见性×排序 → 渲染索引（三条路径含两条回退）
    void CapBuildRenderIndex(bool bShowAll);

    // [R175] 从 OnData 抽出：停定时器 + 看门狗阈值自适应（嵌套 4→3）
    void CapStopTimersAndAdaptWatchdog();

    // [R175] 从 CapBuildRenderIndex 抽出：纯 C++ 回退判定（语义须与 Rust 一致）
    void CapBuildRenderIndexCpp(bool bShowAll);

    // [R200] 智能筛选：分类下拉框内容与当前选中项。
    //   ECapCategory 已在本头文件 class 之前定义（类内可直接使用）。
    void AxInitSmartCombo();
    [[nodiscard]] ECapCategory AxCurrentCategory() const;
    // [R200] 分类 + 关键词联合匹配（渲染 / 增量追加 / 可见性切换三处共用）
    [[nodiscard]] bool AxCapPasses(const CapEntry& e, const DuiLib::CDuiString& sFilter) const;

    // [R175] 从 OnData 抽出：结果一致则保持列表（返回 true = 调用方直接返回）
    // [R175] 从 OnEvent 抽出：选择可选功能源目录并回填
    void CapPickSourceDir();

    // [R175] 从 CapSeedFromCache 抽出：[R85] 包内种子是否仍可信（嵌套 4→3）
    [[nodiscard]] bool CapSeedStillValid();

    // [R175] 从 OnContextMenu 抽出：命中控件 → 能力行号（-1 = 未命中/越界）
    [[nodiscard]] int CapRowFromHit(CControlUI* pHit);

    [[nodiscard]] bool CapKeepListIfIdentical(const std::vector<CapEntry>& vNew,
                                              int nStateChanged, int nExitCode);

    [[nodiscard]] bool CapSeedFromCache(std::vector<CapEntry>& vCache,
                                        bool& bSeedHit, bool& bStaleHit);

    // [R162] 从 OnData 抽出：枚举结果落盘（成功且非空才写，失败记日志不阻断）
    void SaveCacheLogged(const std::vector<CapEntry>& vCaps, int nExitCode);

    // [R162] 从 BlBuildRow 抽出：四条信息行 / 行尾动作按钮
    void BlBuildInfoLines(CVerticalLayoutUI* pInfo, const CapEntry& e,
                          bool bInstalled, const CDuiString& sDesc);
    void BlBuildActionButton(CHorizontalLayoutUI* pRow, size_t k,
                             bool bInstalled, const CDuiString& sUninstall);
    // [P1] 枚举分批回报：追加一批条目并增量渲染；末批做收尾（解除加载态、
    // 落缓存、更新概览）。消息对象在此释放。
    void OnCapPartial(CapPartialMsg* pMsg);
    void BuildList();                  // 原 BuildCapabilityList
    void OnUninstall(int n);           // 原 OnCapUninstall

    // [R124-1] 行级右键菜单增强：caprow_{k} 命中 → 弹出行级菜单
    bool OnContextMenu(POINT pt, bool bFromContextMsg) override;
    void ShowRowContextMenu(int k, POINT pt);
    void CopyIdentity(int k);          // 复制 Capability Identity
    void CopyDisplayName(int k);       // 复制显示名
    void CopyDescription(int k);       // 复制描述
    void ShowRowDetail(int k);         // 查看详细描述（Toast + 日志）

    // 刷新按钮：清缓存后重新枚举
    void Refresh() { m_vCaps.clear(); StartEnumeration(); }

    // 核心壳 WM_TIMER 按定时器 ID 转发
    void OnRefreshTimer();             // 原 CAP_REFRESH_TIMER_ID 分支
    void OnRetryTimer();               // 原 CAP_RETRY_TIMER_ID 分支
    // [rb23] 枚举超时兜底：线程被 SEH 吞掉/API 卡死时解除永久加载态
    void OnLoadWatchdog();

    const wchar_t* PageName() const override { return L"Capability"; }
    void    OnGlobalCopy(bool bCut) override;
private:
    // [R14-03] BuildList 拆分辅助（原 482 行单函数 → 薄壳+6 辅助，零行为改动）：
    //   BlBuildRow 单条能力行；BlBuildDepBadge 依赖徽章（无依赖返回 nullptr）；
    //   BlShowEmpty 空态文案；BlLogDepSummary 依赖总览日志；
    //   BlLogPageStatus 页面收尾状态条；BlUpdateVisualOverview FOD 顶部概览/进度条。
    void      BlBuildRow(CVerticalLayoutUI* pList, const CapEntry& e, size_t k,
                         bool bInstalled, const CDuiString& sUninstall);
    CTextUI*  BlBuildDepBadge(const CapEntry& e);
    void      BlShowEmpty(CVerticalLayoutUI* pList, const CDuiString& sFilter,
                          const CDuiString& sEmpty);
    void      BlLogDepSummary();
    void      BlLogPageStatus();
    void      BlUpdateVisualOverview();

    // [R16][O-2] 分批渲染：BuildList 快照过滤结果入 m_vRenderIdx，
    // RenderBatch 每批 ≤50 行，未完经 CAP_RENDER_BATCH_TIMER_ID 接力。
    void      RenderBatch();
    void      AppendRows(const std::vector<CapEntry>& vNew);
    void      FinalizeRender();
    std::vector<int> m_vRenderIdx;
    // [R12] 行缓存：caprow_{k} 指针与预计算小写搜索串（按 m_vCaps 下标存储）。
    // 过滤改为只切换可见性，不再 RemoveAll + 全量重建。
    std::vector<DuiLib::CControlUI*> m_vCapRowPtr;
    std::vector<DuiLib::CDuiString>  m_vCapRowSearch;
    void ApplyFilterVisibility();

    // [R222] Re-colour code-built controls after a light<->dark switch. Contract:
    //   colour attributes only -- never Build()/Add/Remove, or list contents,
    //   selection and scroll position are lost and m_pFocusCtrl dangles.
    void RefreshThemeColors() override;
    int              m_nRenderPos;
    // [A1 2026-10-25] 渲染起点 tick。与 m_dwEnumStartTick 区分：
    //   前者量"渲染花了多久 / 首行多久可见"，后者量"枚举花了多久"。
    //   两者混用会把排队时间算进渲染耗时，得出错误的优化结论。
    DWORD            m_dwRenderStartTick;
    CDuiString       m_sRenderFilter;   // 本批渲染快照的过滤词

    // [R16][O-1] 看门狗阈值自适应：记录枚举起始 tick，OnData 实测耗时后
    // 将阈值更新为耗时×2（下限 30s/上限 600s）；首次保持 120s。
    DWORD            m_dwEnumStartTick;
    UINT             m_nWatchdogMs;

    std::vector<CapEntry> m_vCaps;        // 原 m_vCaps
    bool                  m_bLoading;    // 原 m_bCapLoading
    // [P1] 分批回报是否已收尾：整表消息 OnData 到达或末批处理完毕后置位，
    // 用于丢弃迟到/过期的分批消息。
    bool                  m_bCapPartialDone = false;
    int                   m_nCapGen = 0;   // [STAB] 枚举 generation：每次 StartEnumeration 自增，用于过滤过期消息
    CDuiString            m_sLastActionName;  // 原 s_sLastCapActionName
    CDuiString            m_sLastActionType;  // 原 s_sLastCapActionType
    bool                  m_bRetryPending;    // 原 m_bCapRetryPending
    int                   m_nAutoRefreshCount; // 原函数静态 s_nCapAutoRefreshCount
    // [FIX-S1-4] 卸载/添加动作在途：DISM 结果未知，禁止用缓存短路枚举，
    // 20s/30s 自动刷新必须拿到真实 State —— 否则失败被乐观状态固化显示。
    bool                  m_bPendingRealRefresh = false;

    // [R5] 绑定的 Windows 功能子视图（CPageHost 装配后 BindFeature；
    // 所有权在 CPageHost，本页仅代理转发，生命周期与宿主一致）。
    CPageFeature*         m_pFeature;
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：

// [R77] 显示名/描述解析：实现于 Page_CapabilityResolve.cpp（原匿名 namespace，
// 因切片内外同时调用而改为外部链接；语义与迁出前逐字相同）。
// 调用方：BuildList / RenderBatch / BlBuildRow / BlBuildDepBadge / Copy* 系列。
[[nodiscard]] DuiLib::CDuiString ResolveCapDisplayName(const DuiLib::CDuiString& sIdentity);
[[nodiscard]] DuiLib::CDuiString ResolveCapDescription(const CapEntry& e);

[[nodiscard]] ECapCategory CapResolveCategory(const DuiLib::CDuiString& sIdentity);
[[nodiscard]] const wchar_t* CapCategoryText(ECapCategory nCat);
// 当前系统分级下实际生效的映射条数（Win10 不含 Win11 专属条目）。
[[nodiscard]] int CapMapActiveEntries();
// 分级情况写调试日志（Win10 上明确记录"已关闭 Win11 专属映射"）。
void CapLogMapTier(void* pUnused);

// ============================================================
// [R90] Capability 行高算式：**唯一来源**
//
// 为什么抽出来：R87/R88/R89 三轮在同一个函数上反复犯错，模式都是
//   "只验证一种内容组合，就把结果当成通用常量"。
//   根因是算式散落在两处：真实 BlBuildRow 一份、/BenchRender 基准一份，
//   改一处不改另一处，基准就测不到真实行为（R88 实际发生过）。
//   抽成内联函数后，两边调同一个，物理上不可能再脱节。
//
// 参数：bHasDesc = 该行是否有描述；bHasDeps = 是否有依赖徽章
// 返回：该行应设的固定高度（已含行 inset 上下各 4）
//
// 四种组合的预期值（自检穷举断言，勿改）：
//   无描述无依赖 50 / 无描述有依赖 66 / 有描述无依赖 70 / 有描述有依赖 86
//
// 注意：必须返回精确高度，不能只给下限 —— R88 实测，去掉固定高度后
//   CVerticalLayoutUI 会把无固定高度的子项当可伸缩项拉伸填满（8 行时每行 100px）。
[[nodiscard]] inline int CapRowHeight(bool bHasDesc, bool bHasDeps)
{
    int nContentH = 22;                     // 名称
    int nItems   = 1;
    if (bHasDesc) { nContentH += 18; ++nItems; }   // 描述
    nContentH += 18; ++nItems;                     // 状态（size 已合并进此行）
    if (bHasDeps) { nContentH += 14; ++nItems; }   // 依赖徽章
    nContentH += (nItems - 1) * 2;                 // pInfo childpadding
    return nContentH + 8;                          // + 行 inset 上下各 4
}
// [R101] 可选功能页跨 TU 的 DISM 异步起动入口（实现见 Page_Capability.cpp）。
void CapStartDismAsyncShared(HWND hWnd, int nOp, const std::wstring& sName,
                             const std::wstring& sSource, bool bLimitAccess);
