#pragma once

// ============================================================
// Page_Update —— v2 P4 纵切页面（Windows 更新 待安装/已安装）
//
// 承接原 CFrameWnd 的 6 个函数：
//   StartUpdateEnumeration / OnUpdateData / OnUpdateRowSelect /
//   StartUpdateUninstall / ApplyUpdateFilter / UpdateUpdateCount
// 及视图数据 m_vUpdList / m_nUpdSelected / m_nUpdWuaErr /
// m_dtUpdScanStart / m_bUpdLoading；并承接刷新按钮处理中的
// 扫描耗时统计/下载尝试计数/WUA 错误码日志三段逻辑。
// 后台 WUA 枚举/卸载归 AppxCapUpdService（S6），结果经
// WM_UPD_DATA 由核心壳转发 OnData。
//
// 物理搬迁护栏：函数体零逻辑改动，宿主引用一律改 m_ctx.Xxx。
// ============================================================

#include <vector>
#include "Utils/Utils.h"
#include "PageBase.h"
#include "AppxCapUpdService.h"   // UpdEntry / UpdDataMsg
#include "UpdateMetaService.h"  // [R-B78] Build 时崩溃过滤器/outbox 补发

class CPageUpdate : public CPageBase
{
public:
    explicit CPageUpdate(IPageContext& ctx)
        : CPageBase(ctx), m_nSelected(-1), m_nWuaErr(0),
          m_dwMetaMaxThread(10), m_bCbsLoading(false),
          m_nCurTab(-1), m_bCbsReady(false), m_bCbsExpanded(false),
          m_pRenderList(nullptr), m_nRenderTab(-1), m_bRenderCbs(false),
          m_nRenderPos(0)
    {
        m_dtScanStart.wYear = 0;
        m_bTabLoaded[0] = m_bTabLoaded[1] = false;
        m_bTabLoading[0] = m_bTabLoading[1] = false;
    }

    // [R209] 补 PageName：本页原先没有覆写，返回 nullptr，
    //   导致按页名查找控件（LogPageContent / ExportAllPageShots）拼出 "page_" 而失配
    //   —— 表现为该页渲染失败、统计名称为 "?"。与其它页保持同一契约。
    const wchar_t* PageName() const override { return L"Update"; }

    void Build() override
    {
        // [R-B78] 崩溃报告过滤器（仅在无既有过滤器时安装）；outbox 补发放到
        //         CBS 后台枚举线程里做，避免端点不可达时卡住界面。
        {
            wchar_t szExe[MAX_PATH] = {};
            ::GetModuleFileNameW(nullptr, szExe, MAX_PATH);
            wchar_t* pSlash = wcsrchr(szExe, L'\\');
            if (pSlash) *pSlash = 0;
            UpdateMetaService::InstallCrashReporter(szExe);
        }
        BuildMaxThreadCombo();
        RefreshPolicyButtons();   // [按钮整合] 三枚策略切换键按当前状态显示文案/配色
        UpdateBottomBar();        // [性能 P3] Tab 感知底栏显隐
        StartEnumeration(1);
    }

    // [R3] F5 刷新仅记录日志；懒构建=默认选中"已安装的更新"子页并枚举
    void OnRefreshPage() override;
    void OnDeferredBuild() override;

    // P8：第二级事件路由（原壳 Notify / WM_RBUTTONUP 本页分支）
    void OnEvent(TNotifyUI& msg) override;
    // [REFACTOR] 从 OnEvent 提取：13 个按钮的 click 分派。
    // 返回 true 表示本条事件已消费（原 return; 语义），调用点据此提前返回。
    [[nodiscard]] bool HandleButtonClick(const CDuiString& sName);
    [[nodiscard]] bool OnContextMenu(POINT pt, bool bFromContextMsg) override;

    // [R5] 后台消息收口：WM_UPD_DATA 本页释放；WM_UPD_PROGRESS 仅读取
    // 字段（kMsgOwnShell，壳 ReleaseMsgParam 统一释放，本页禁止 delete）。
    [[nodiscard]] bool OnAsyncMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) override;
    // [性能 C1/B1] 壳 kTimerTab 页属定时器分发：过滤防抖 / 分批渲染接力
    void OnHostTimer(UINT uId) override;
    // [R5] btn_upd_find 扫描起始时间戳/日志（EHostCmd::HCMD_UPD_SCAN_STARTED）
    bool OnHostCommand(int id) override;

    // [R-B76] 拖拽 .msu/.cab 到窗口 → 本地补丁安装（在线 dism /Add-Package；
    // 离线会话自动加 /Image:）。仅消费补丁包，其他文件交还壳通用拖拽逻辑。
    bool OnDropFiles(const std::vector<std::wstring>& vPaths) override;

    void StartEnumeration(int nTab = 1);        // 原 StartUpdateEnumeration
    void OnData(UpdDataMsg* pData);             // 原 OnUpdateData
    // [P1] 更新枚举分批回报：追加一批并增量渲染；末批收尾。消息对象在此释放。
    void OnPartial(UpdPartialMsg* pMsg);
    void OnRowSelect(int n);                    // 原 OnUpdateRowSelect（单击切换勾选）
    void OnSelectAll();                         // btn_upd_selall：全选/全不选切换
    void StartUninstall(int n);                 // 原 StartUpdateUninstall
    void ApplyFilter();                         // 原 ApplyUpdateFilter
    void UpdateCount(int nShown, int nTotal);   // 原 UpdateUpdateCount
    void StartUpdateInfoDownload();             // [R74] 下载更新信息
    void StartUpdatePhase(int nPhase);          // [R74] 执行更新阶段
    // [FIX-P2N] 阶段流在途标志：子进程等待已后台化，防 300s 内重复点击并发
    bool                  m_bPhaseRunning = false;
    void UpdateUpdatePhaseState(int nState);    // [R74] 更新阶段状态
    // [R153] StartUpdateInfoDownload 拆分（原 121 行 / CC23）
    bool UpdVerifyInfoZip();                                      // 校验 zip 大小，>1KB 视为有效
    void UpdResolveReportEndpoint(const wchar_t* pszUrls);         // 取 <URLS> 首地址作上报端点
    void UpdPostKeyReport(const UpdateMetaService::UpdateMeta& meta);  // 组包并投递 Key 回传
    void UpdParseInfoZip();                                       // 四字段解析 + Key 回传编排
    // [R153] StartUpdatePhase 拆分（原 113 行 / CC19）
    bool UpdPhasePreflight(int nPhase);                           // cab 就位 + CBS 独占倒计时
    bool UpdBuildPhaseCommand(int nPhase, const CDuiString& sPhaseFile, CDuiString& sCmd);
    void UpdReportPhaseResult(DWORD dwRet, const CDuiString& sPhaseFile);

    // ---- 核心壳通知区按钮处理迁入 ----
    // btn_upd_scan：记录扫描起始时间并日志
    void OnScanStarted();
    // btn_upd_refresh：输出扫描耗时、累计下载尝试并触发 StartDownload
    void OnRefreshElapsed();
    // btn_upd_refresh：输出最近 WUA 结果码本地化说明
    void LogLastWuaError();
    // btn_upd_delete：未选中提示，否则卸载选中行
    void OnUninstallButton();

    // [R80] WUA 正向安装/下载
    // btn_upd_install：未选中提示，否则收集所有未安装项 UpdateID 后启动安装线程
    void OnInstallButton();
    // WM_UPD_PROGRESS 回调：更新进度文本/进度条
    void OnInstallProgress(int nPercent, int nPhase, int nItemIndex, int nItemCount);
    // WM_UPD_DATA(nKind=2/3) 回调：安装完成/失败
    void OnInstallDone(int nKind, int nErr, int nResultCode, int nHr);

    // [R82] WUA 隐藏 / 取消隐藏（P0-5 HideUpdate / UnhideUpdate）
    // btn_upd_hide：收集所有未安装项 UpdateID 后启动隐藏线程
    void OnHideButton();
    // [R127-A] 收集当前 Tab 下勾选项的 UpdateID。
    //   原样搬迁自 OnInstallButton 与 OnHideButton 的两份逐字相同实现。
    //   勾选态与列表长度不一致时按"全选"补齐（沿用原语义）；
    //   结果为空时弹同一提示并返回 false，调用方据此直接 return。
    [[nodiscard]] bool CollectCheckedUpdateIds(std::vector<CDuiString>& vOut);

    // btn_upd_unhide：切换 Tab1（已安装）后对选中行取消隐藏
    void OnUnhideButton();
    // WM_UPD_DATA(nKind=4/5/3) 回调：隐藏/取消隐藏完成或失败
    void OnHideDone(int nKind, int nErr, int nResultCode, int nHr);

    // [R86 P1-18] WUA 策略设置
    // btn_upd_pause：暂停自动更新（写 HKCU WindowsUpdate 注册表）
    void OnPauseButton();
    // btn_upd_unpause：取消暂停自动更新
    void OnUnpauseButton();
    // btn_upd_skip：推迟功能更新（DeferFeatureUpdates + TargetReleaseVersion）
    void OnSkipButton();
    // btn_upd_unskip：恢复功能更新（清除 DeferFeatureUpdates）
    void OnUnskipButton();

    // [R-B49] DisableInstallUpdate：彻底禁用 / 恢复系统更新（清单维度 7 缺失 2）
    // btn_upd_disable：写 HKLM AU 策略 NoAutoUpdate=1 + AUOptions=1 并停止 wuauserv
    void OnDisableButton();
    // btn_upd_enable：删除 NoAutoUpdate、写 AUOptions=2 并启动 wuauserv
    void OnEnableButton();

    // [按钮整合] 按当前注册表策略刷新三个切换键的文案/配色：
    // btn_upd_pausetoggle（暂停更新↔恢复更新）、
    // btn_upd_skiptoggle（推迟功能更新↔恢复功能更新）、
    // btn_upd_disabletoggle（禁用系统更新↔恢复系统更新，危险方向红色）。
    // 在 Build 及每次策略操作后调用。
    void RefreshPolicyButtons();

    // [R-B50] 下载线程数上限选择（清单维度 7 缺失 1）
    // BuildMaxThreadCombo：填充 upd_maxthread_combo 候选并选中 config.ini 持久化值
    void BuildMaxThreadCombo();
    // OnMaxThreadChanged：combo 变化时写 config.ini [Update] MaxThread
    void OnMaxThreadChanged();
    // GetEffectiveMaxThread：读取 config.ini > 上次解析的 UpdateInfo 值 > 默认 10
    DWORD GetEffectiveMaxThread();

    // [R-B76] 本地补丁安装入口（右键菜单"安装本地补丁包"与拖拽共用）
    void InstallLocalPackages(const std::vector<std::wstring>& vPkgs);
    // [R-B76] CBS /Get-Packages 后台枚举并并入"已安装"列表
    void StartCbsEnumThread();
    void OnCbsPackages(UpdDataMsg* pData);
    // [R-B78] UpdateReport 回传（WUA 安装完成后，失败尽力而为不阻断）
    void PostInstallResultReport(int nKind, int nErr, int nResultCode, int nHr);

private:
    // [性能 C3] 两 Tab 各持独立数据缓存/勾选/行指针，切换免重查；
    // m_vUpdList[nTab] 索引与 updrow_%d 的 k 一一对应。
    std::vector<UpdEntry>   m_vUpdList[2];
    std::vector<char>       m_vChecked[2];
    std::vector<CControlUI*> m_vpRows[2];   // [性能 C2] updrow_ 行指针索引

    // [R225] Re-colour both tabs' rows after a light<->dark switch. Contract from
    //   PageBase.h: colour attributes only -- no Build()/Add/Remove, or the lists,
    //   selection and scroll position are lost and m_pFocusCtrl dangles.
    void RefreshThemeColors() override;
    // Status-column colour for one row, derived from the SAME rules the builder
    //   uses. Shared so build-time and switch-time cannot drift.
    DWORD UpdStateColor(const CDuiString& sTitle, bool bPending) const;
    bool                    m_bTabLoaded[2];   // 该 Tab 已有数据（缓存或枚举结果）
    bool                    m_bTabLoading[2];  // 该 Tab 后台查询在途
    // [R3-FIX] generation 守卫：每次 StartEnumeration 递增；UI 侧 OnData/OnPartial
    // 收到消息后比对 nGen，过期消息直接丢弃，防止上一次枚举的回传覆盖新一次结果
    int                     m_nUpdGen = 0;
    // [P1] 分批回报：累计已到达条目（按 Tab）+ 是否已收尾
    std::vector<UpdEntry>   m_vBatchUpd[2];
    bool                    m_bUpdPartialDone[2] = { false, false };
    int                     m_nCurTab;         // 当前选中 Tab（0=待安装 1=已安装）
    std::vector<UpdEntry>   m_vCbsPending;     // [性能 A2] CBS 原始结果（并行时序暂存）
    std::vector<UpdEntry>   m_vCbsDedup;       // [性能 A2] 与 WUA 按 KB 去重后的 CBS 行
    // [R162] 跨 TU 状态单一入口。m_bCbsReady / m_bCbsExpanded 是同一协议的两位：
    //   Ready 为真才允许合并 CBS 包，Expanded 表示已展开。原先 4 个 TU 各写一次，
    //   "扫描开始复位"在 Scan.cpp 里是手工两行 —— 新增第三条复位路径时极易只写一半，
    //   出现"Ready 已假、Expanded 仍真"的半复位（表现为切回在线后 CBS 列表不刷新）。
    void SetWuaErr(int n)                    { m_nWuaErr = n; }
    void MarkScanStart(const SYSTEMTIME& st) { m_dtScanStart = st; }
    void ResetCbsState()                     { m_bCbsReady = false; m_bCbsExpanded = false; }
    void MarkCbsReady()                      { m_bCbsReady = true; }
    void MarkCbsExpanded(bool bOn)           { m_bCbsExpanded = bOn; }

    // [R162] 从 OnContextMenu 抽出：更新列表右键菜单（含本地补丁包多选安装）
    void ShowUpdListMenu(POINT pt);

    // [R175] 从 StartEnumeration 抽出：命中缓存则先渲染（嵌套 4→2）
    void UpdRenderCacheFirst(int nTab);

    // [R162] 从 UpdLogStats 抽出：按更新标题分类计数
    void CountByUpdateType(const std::vector<UpdEntry>& vUpd,
                           int& nSec, int& nCum, int& nDef,
                           int& nFeat, int& nDrv, int& nOth);

    bool                    m_bCbsReady;       // CBS 结果已回传
    bool                    m_bCbsExpanded;    // 折叠组已展开
    // [性能 B1] 分批渲染接力状态（仿 Capability RenderBatch 先例）
    CVerticalLayoutUI*      m_pRenderList;
    int                     m_nRenderTab;
    bool                    m_bRenderCbs;
    size_t                  m_nRenderPos;
    int                     m_nSelected;  // 原 m_nUpdSelected（最后单击行，供单项操作）
    int                     m_nWuaErr;    // 原 m_nUpdWuaErr
    SYSTEMTIME              m_dtScanStart;  // 原 m_dtUpdScanStart
    CDuiString            m_sUpdateInfoPath;  // [R74] 更新信息文件路径
    CDuiString            m_sUpdatePhaseState;  // [R74] 当前阶段状态文本
    int                   m_nUpdatePhase;  // [R74] 当前更新阶段（0=PreDownload, 1=Install, 2=Finalize）
    DWORD                 m_dwMetaMaxThread;  // [R-B50] UpdateInfo.xml 解析的 MaxDownloadThread（缺省 10）
    bool                  m_bCbsLoading;      // [R-B76] CBS 并入枚举去重闸门
    CDuiString            m_sReportEndpoint;  // [R-B78] UpdateInfo <URLS> 首地址（回传端点）

    // [R14-03] OnData 拆解：卸载/安装/隐藏回调、状态准备、列表渲染、统计日志
    void UpdHandleUninstall(UpdDataMsg* pData);                  // nKind==1 卸载结果回传
    void UpdHandleInstall(UpdDataMsg* pData);                    // nKind==2/3 WUA 安装回调
    void UpdHandleHide(UpdDataMsg* pData);                       // nKind==4/5/6 隐藏/取消隐藏
    void UpdHandleCbs(UpdDataMsg* pData);                        // nKind==7 CBS 并入
    void UpdHandleLocalPatch(UpdDataMsg* pData);                 // nKind==8/9 本地补丁
    void UpdPrepareState(UpdDataMsg* pData);
    // [R175] 从 OnData 抽出：nKind 分派（9 种消息）/ 常规列表重建
    bool UpdRouteByKind(UpdDataMsg* pData);
    void UpdRenderDataList(UpdDataMsg* pData);
    // [R175] 从 OnEvent 抽出：click / dbclick 行点击路径（命中返回 true）
    [[nodiscard]] bool UpdHandleClickMsg(const CDuiString& sName);
    [[nodiscard]] bool UpdHandleDbClickMsg(const CDuiString& sName);
    // [R187] 从 OnEvent（CC12）抽出：过滤框防抖 / 下载线程数下拉
    [[nodiscard]] bool UpdHandleFilterMsg(const CDuiString& sName);
    [[nodiscard]] bool UpdHandleComboMsg(const wchar_t* pszName);
    // [R187] 从 OnEvent 抽出：指针类 / 值类消息聚合分派
    [[nodiscard]] bool UpdHandlePointerMsgs(const CDuiString& sType, const CDuiString& sName);
    [[nodiscard]] bool UpdHandleValueMsgs(const CDuiString& sType, const CDuiString& sName,
                                          const wchar_t* pszName);                     // 缓存列表/默认全选/重置状态
    [[nodiscard]] CVerticalLayoutUI* UpdResolveList(UpdDataMsg* pData);        // 按 nTab 选容器 + 隐藏占位
    void UpdRenderRows(CVerticalLayoutUI* pList, UpdDataMsg* pData);   // 遍历渲染行
    [[nodiscard]] CControlUI* UpdRenderRow(CVerticalLayoutUI* pList, const UpdEntry& e,
                             size_t k, bool bPending);            // 单行渲染（返回行控件指针供 C2 索引）
    void UpdRenderEmptyState(CVerticalLayoutUI* pList, UpdDataMsg* pData); // 空态渲染
    void UpdLogStats(UpdDataMsg* pData);

    // [R169] 从 HandleButtonClick 抽出：CBS 折叠组展开（先摘占位行，再分批渲染）
    void UpdExpandCbsGroup();                         // 统计日志 + 错误记录 + 状态条

    // ---- [性能改造] 新增辅助 ----
    // [STAB] m_nCurTab 初值为 -1（未选择任何 Tab）。访问器必须兜底，
    // 否则首次 Build 前若有人调用 CurList() 会越界访问 m_vUpdList[-1]。
    std::vector<UpdEntry>& CurList()
    {
        return m_vUpdList[m_nCurTab == 0 ? 0 : (m_nCurTab == 1 ? 1 : 0)];
    }
    std::vector<char>& CurChecked()
    {
        return m_vChecked[m_nCurTab == 0 ? 0 : (m_nCurTab == 1 ? 1 : 0)];
    }
    // [性能 C3] Tab 切换命中缓存：仅重渲染，不重查
    void RenderTabFromCache(int nTab);
    // [性能 B1] 分批渲染接力：≤50 行/批，未完成 SetTimer 接力下一批
    void UpdStartBatchRender(CVerticalLayoutUI* pList, int nTab, bool bCbs);
    void UpdRenderBatchStep();
    // [R187] 从 UpdRenderBatchStep（CC12）抽出：渲染单行并入行指针索引
    void UpdRenderOneRow(const UpdEntry& e, size_t k, bool bPending);
    // [R187] 从 OnSelectAll（CC12）抽出：全选态判定 / 批量置位与底色同步
    [[nodiscard]] bool UpdIsAllChecked(const std::vector<char>& vChecked);
    void UpdApplyCheckedAll(const std::vector<UpdEntry>& vList,
                            std::vector<char>& vChecked, char cSet);
    // [性能 A2] CBS 去重并入（WUA 渲染完成后调用；未渲染则等回调）
    void UpdMergeCbs();
    // [R168] 从 UpdMergeCbs 抽出：WUA/CBS 双方 KB 号集合去重（2 层嵌套循环）
    void UpdBuildCbsDedup();
    // [R168] 从 UpdMergeCbs 抽出：默认折叠时追加一行"显示系统组件包(N)"开关
    void UpdAppendCbsToggleRow(CVerticalLayoutUI* pList);
    // [R168] 从 OnEvent 抽出：selectchanged 的 updtab_ 分支；命中返回 true
    [[nodiscard]] bool UpdOnTabSwitch(const wchar_t* pszName);
    // [R168] 从 ApplyFilter 抽出：单容器内关键字匹配与行显隐；返回命中行数
    int UpdApplyFilterList(int nTab, const wchar_t* pszListName,
                           const CDuiString& sKw, const CDuiString& sKwLower,
                           int& nTotal);
    // [REFACTOR] 从 UpdMergeCbs 提取：从单个字符串收集 "KBxxxxxx" 形式的 KB 号。
    void CollectKbNumbers(const wchar_t* p, std::vector<CDuiString>& outKb);
    void UpdRenderCbsRow(CVerticalLayoutUI* pList, const UpdEntry& e, size_t k);
    // [性能 P3] Tab 感知底栏显隐 + 策略下拉菜单
    void UpdateBottomBar();
    void OnPolicyMenu();
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
