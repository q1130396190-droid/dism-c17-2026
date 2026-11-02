#pragma once

// ============================================================
// Page_Optim —— P7「系统优化」页（重构最后一个页面切片）
//
// 承接原 CFrameWnd 的优化页全部 UI 函数：
//   BuildOptimTree(Build) / SyncOptimGroupGlyph / BulkApplyAllOptim /
//   ToggleGroup / ApplyFindFilter / VerifyOptimStates（5 秒 TTL）/
//   ExportOptimConfig / OnSwitchToggle / OnScopeCombo /
//   ShowOptimContextMenu / UndoLastOptim / PushOptimUndo（私有）/
//   SetOptimProgress / InitProgress / ThrottleProgress（私有）/
//   OnGroupCheckClick / OnStateFilterClicked / OnFindButtonClicked /
//   OnFindFilterChanged（原 Notify 内联块）
// 以及 SOptimUndo 撤销栈（上限 32）、状态过滤、节流四成员。
//
// 本切片不新增 Service：所有注册表/服务写入继续经 m_ctx.DataEngine()；
// 重启 explorer 经本切片唯一新增宿主接口 HostRestartExplorer()；
// 取色器 ?ColorDialog() 本就在 DataEngine，无需页内处理。
// 物理搬迁护栏：函数体零逻辑改动，宿主引用一律改 m_ctx.Xxx。
// ============================================================

#include <vector>
#include <set>        // [Privilege] m_setPrivWarned：已提示条目集合

#include "PageBase.h"
#include "CustomControls.h"   // CClickRowUI / CHoverRowUI / CSwitchUI (R14-03 Build 拆解)
#include "OptimScopeColumns.h"   // [SCOPE-2COL] optim_scope::kColumns 可见作用域列（唯一真源）

namespace DuiLib { class CProgressUI; }

// ============================================================
// [SCOPE-2COL] 系统优化页可见作用域列（唯一真源见 OptimScopeColumns.h）
//
// 顺序即界面列顺序，与 page_optim.xml 表头一一对应：
//   0 = 当前用户（Current，HKCU）
//   1 = 系统    （System，HKLM）
// 原第三列 2 = 新用户（Default，HKU\.DEFAULT）已按交互要求取消。
//
// 为什么必须集中成一张表：写路径（单项开关/批量/分组全选/导入/恢复默认）、
// 读路径（核实/三态统计/状态过滤/导出/属性框/JSON）此前各自硬编码
// `sc < 3`，取消一列却漏改任一处，就会出现"界面上看不见、注册表却被改"
// 或"三态统计把看不见的列算进来"这类不可见副作用。此后新增/删除一列
// 只需改 OptimScopeColumns.h。
// ============================================================
namespace optim_scope
{
// 可见作用域列定义在 OptimScopeColumns.h（供页面层与通用辅助层共用）。

// ============================================================
// [EXPORT-SYMMETRY] 导入时判定某条 <Optim> 记录是否应作用于该作用域。
//
// 导出侧逐列写 Scope 属性后，导入必须逐列回放；否则「导出→导入」会把
// 当前用户的值整批覆盖到系统列（旧实现正是如此：导出只写首个存在的列，
// 导入却写全部可见列）。pszScopeAttr 为空表示旧格式配置文件，按历史
// 语义作用于全部可见列，保证老文件仍可导入。
// ============================================================
[[nodiscard]] inline bool ImportTargetsScope(const wchar_t* pszScopeAttr, int nScope)
{
    if (pszScopeAttr == nullptr || *pszScopeAttr == L'\0') return true;
    const wchar_t* pszName = DismScopeName(nScope);
    return lstrcmpiW(pszScopeAttr, pszName) == 0;
}
}   // namespace optim_scope

class CPageOptim : public CPageBase

{

public:

    explicit CPageOptim(IPageContext& ctx);

    void Build() override;   // 原 BuildOptimTree

    // [R3] DPI 改变 / 最大化还原后整树重建
    void OnRebuildDynamicTree() override { Build(); }
    bool NeedsFullRebuildOnRelayout() const override { return true; }

    // [R4] 壳命令收口：Ctrl+Z 撤销最近一次优化设置
    bool OnHostCommand(int id) override;

    // P8：第二级事件路由（原壳 Notify 优化页分支）
    void OnEvent(TNotifyUI& msg) override;
    bool OnContextMenu(POINT pt, bool bFromContextMsg) override;

    // ---- 壳薄转发具名入口（签名与原 CFrameWnd 同名函数一致）----
    void SyncOptimGroupGlyph(int g);
    // [PERF] 重载：调用方已定位到该组的 XML 节点时直接传入，跳过"从根遍历到第 g 组"。
    // 批量刷新（VerifyOptimStates 等）逐组推进时用它把 O(n²) 降为 O(n)。
    void SyncOptimGroupGlyph(int g, CMarkupNode gNode);
    // [R169] 从 SyncOptimGroupGlyph 抽出：统计单个条目在各可见作用域上的开关数
    void SyncCountItemStates(int g, int ii, CMarkupNode item, bool bShown,
                             int& nOn, int& nTotal);

    void BulkApplyAllOptim();

    void ToggleGroup(int g);
    // [G9 2026-10-25] 一键展开/折叠全部组：原实现全默认折叠且无批量入口，
    //   浏览数百项需逐组点开。按目标态设置而非逐组取反，结果可预期。
    void SetAllGroupsExpanded(bool bExpand);

    void ApplyFindFilter(const CDuiString& strText);
    // [R152] ApplyFindFilter 拆分（原 81 行 / CC23）：复位分支与过滤分支各自成函数
    void OptimResetGroupVisibility(int g, CControlUI* pGroupRow,
                                   CVerticalLayoutUI* pBody, bool bGroupApplicable);
    void OptimShowFilteredGroup(int g, CControlUI* pGroupRow,
                                CVerticalLayoutUI* pBody, bool bVisible);

    void VerifyOptimStates(bool bForce = false);

    // [R213] 验收取证：把第 g 组前若干行的名称列/开关列真实矩形写进日志。
    //   用途：优化页开关列在导出位图里整列为空，靠像素统计无法区分
    //   "控件没有位置" 与 "位置正确但绘制时被裁掉"，必须读 m_rcItem 定案。
    void DumpOptimRowGeometry(int g, int nRows);

    void ExportOptimConfig();
    // [R193] 从 ExportOptimConfig（78 行 / CC9）抽出：UTF-8 BOM 写盘与失败回执
    // [R193] 从 OptimBuildComboCell（74 行）抽出：下拉项填充 / 空列占位
    void OptimFillComboOptions(CComboUI* pCombo, CMarkupNode item, int sc, int nItemH);
    void OptimAddDashCell(CHoverRowUI* pItemRow);
    [[nodiscard]] bool OptimWriteXmlFile(IPageContext& ctx, const CDuiString& sPath,
                                         const CDuiString& sXml);
    // [R156] OnEvent 按钮表驱动的零参薄壳：VerifyOptimStates 带默认参数 bForce=false，
    // 默认参数不参与函数类型，其地址类型是 void(bool)，无法入 void() 表（C2440）。
    void VerifyOptimStatesForce() { VerifyOptimStates(true); }
    // [R160] 核实缓存失效的单一入口：原先 4 个 TU 各自裸写 m_dwOptimVerifyTick = 0，
    //   契约只存在于注释里；集中一处后改 TTL 语义不必再找齐四份拷贝。
    void InvalidateOptimVerifyCache() { m_dwOptimVerifyTick = 0; }
    // [R160] 筛选三元组（关键字/状态过滤/过滤中标志）的单一写入口。
    //   三者的不变式：bFiltering == (关键字非空 || 状态过滤 != 0)，
    //   且 sKw 为空时一定是"未过滤"。原先由 ApplyFindFilter 与
    //   OptimBuildResetFilter 两处各自维护，新增清空路径极易漏改一项。
    void SetOptimFilterState(bool bFiltering, const CDuiString& sKw, int nStateFilter)
    {
        m_bFiltering         = bFiltering;
        m_sOptimFindText     = sKw;
        m_nOptimStateFilter  = nStateFilter;
    }
    // [R156] OnEvent 拆分（原 53 行 / CC18）
    bool OptimDispatchClickButton(const CDuiString& sName);
    // [R176] 从 OnEvent（CC13）抽出：click 三层判定 / itemselect 处理
    bool OpHandleClickMsg(const CDuiString& sName, const wchar_t* pszName);
    bool OpHandleItemSelectMsg(DuiLib::TNotifyUI& msg, const wchar_t* pszName);
    // [R176] 从 ApplyFindFilter（CC13）抽出：单个组内的逐项显隐
    // [R176] 从 Build（CC13）抽出：单个组内的条目建行
    void OptimBuildGroupItems(CMarkupNode group, CVerticalLayoutUI* pBody,
                              int g, bool bGroupApplicable, int& nApplicable);
    // [R176] 从 OcmCopyDetails（86 行 / CC13）抽出：动作计数 / 详情文本拼装
    [[nodiscard]] int OcmCountActions(CMarkupNode node);
    [[nodiscard]] CDuiString OcmBuildCopyDetails(CMarkupNode node, int nScope,
                                                 const CDuiString& sName,
                                                 const CDuiString& sDesc);
    void OptimApplyItemVisibility(CMarkupNode group, CVerticalLayoutUI* pBody,
                                  const CDuiString& strText, bool bReset,
                                  bool bHasKw, bool bGroupMatch,
                                  bool& bAnyItem, bool& bGroupApplicable);
    // [R176] 从 BaCollectWarn（CC13）抽出：单作用域待变更判定 / 警告去重追加
    [[nodiscard]] bool BaScopeNeedsChange(CMarkupNode item, int sc, bool bShown, bool bTarget);
    // [R160] 从 BaCollectWarn 抽出：单条目单作用域统计（待变更计数 + Admin 计数 + 警告去重）
    [[nodiscard]] bool BaAccumOneScope(CMarkupNode item, int sc, bool bShown, bool bTarget,
                                       int& nPrivNeeded, std::vector<CDuiString>& vBulkWarn);
    void BaAppendWarnUnique(CMarkupNode item, int sc, bool bTarget,
                            std::vector<CDuiString>& vBulkWarn);
    // [R156] OnContextMenu 拆分（原 52 行 / CC18）
    void OcmResolveRowFromHit(CControlUI* pHit, int& gCtx, int& iCtx);
    // [R186] 从 OcmResolveRowFromHit（CC12）抽出：在 body 下定位命中行（返回 -1 = 未定位）
    [[nodiscard]] int OcmResolveRowInBody(CControlUI* pHit, CControlUI* pBodyNode,
                                          CContainerUI* pBody);
    // [R186] 从 VerifyOptimStates（CC12）抽出：遍历统计可见条目并同步作用域开关
    void VerifyCountAndSync(CMarkupNode root, int& nVisible, int& nChecked);
    void OcmScanAncestors(CControlUI* pHit, int& gCtx, int& iCtx, int& scCtx, bool& bInOptim);
    // [G4-opt 2026-10-25] 键盘唤出右键菜单时需要锚点。
    //   优化页的行是 HoverRowUI（在 optbody_N 容器内按 index 定位），
    //   没有像清理页那样的全局行号，故缓存"最近一次点击命中的 (g,i,scope)"。
    void OcmSaveLastRow(CControlUI* pHit);
    [[nodiscard]] bool OcmReplayLastRow(int& gCtx, int& iCtx, int& scCtx);

    void OnSwitchToggle(const wchar_t* pszName, bool bOn);
    // [R152] OnSwitchToggle 拆分（原 87 行 / CC20）
    [[nodiscard]] bool OcmConfirmSwitch(CMarkupNode node, int nScope, bool bOn);
    void OcmFinishSwitch(CMarkupNode node, int g, int nScope, const CDuiString& sItemName,
                         CSwitchUI* pSw, bool bOn, int nUnsupported, bool bRestart);
    // [PERF-SW] OcmFinishSwitch 的"回读 → 分组三态 → 核实缓存 → 筛选收敛"段。
    //   独立成成员只为分段计时与单点维护；调用方仍只有 OcmFinishSwitch 一处。
    void OcmFinishSwitchUiSync(CMarkupNode node, int g, int nScope, CSwitchUI* pSw);

    void OnScopeCombo(const wchar_t* pszName, int iSel);
    // [R152] OnScopeCombo 拆分（原 83 行 / CC17）
    [[nodiscard]] bool OcmConfirmCombo(CMarkupNode node, int nScope, int iSel);
    void OcmFinishCombo(CMarkupNode node, int nScope, const CDuiString& sItemName,
                        CComboUI* pCombo, int iSel, int nUnsupported, bool bRestart);

    void ShowOptimContextMenu(int g, int i, int nScope, POINT pt);

    void UndoLastOptim();

    // 壳薄转发保留（Ctrl+Z 链路 OnAccelCommand 经 CFrameWnd::PushOptimUndo 入栈）
    void PushOptimUndo(int g, int i, int nScope, bool bBefore);
    // [F-OPT-1] 下拉变更入撤销栈：nBefore=变更前系统档位，nAfter=用户选中档位
    void PushOptimUndoCombo(int g, int i, int nScope, int nBefore, int nAfter);

    void SetOptimProgress(int nVal, int nMax);

    // 原 CFrameWnd::InitSessionProgress 中 optim_progress 抓取段（逐字搬迁）
    void InitProgress();

    // ---- 原 Notify 内联块（逐字抽出，壳单行路由）----
    // [组头复选框已移除] optgrp_ck_ 分组复选框的"批量全选/全不选整组"
    //   （含确认框/失败汇总/重启询问）随组头复选框一起删除：
    //   OnGroupCheckClick 与 Gck* 系列（定位/统计/汇总/执行）全部下线，
    //   组标题点击仍然只做折叠/展开。

    // btn_optim_state：循环切换 全部/仅已启用/仅未启用
    void OnStateFilterClicked();

    // btn_find：显示/隐藏 find_edit，隐藏时清空并复位过滤
    void OnFindButtonClicked();

    // find_edit textchanged：按关键字重新过滤
    void OnFindFilterChanged(const wchar_t* pszText);

    const wchar_t* PageName() const override { return L"Optim"; }
    // [R230] Re-colour code-built rows after a light<->dark switch. Colours only --
    //   no Build()/Add/Remove (would drop the tree, the collapsed state m_vExpanded
    //   and the scroll position).
    void RefreshThemeColors() override;
    void    OnGlobalSelectAll() override;
    void    OnGlobalSave() override;
    void    OnGlobalRedo() override;
private:

    // 等价原 CFrameWnd::AppendLog：级别仍走 LogService::DetectLevel 自动识别
    void AppendLog(const wchar_t* pszLine);

    // [R14-03] ShowOptimContextMenu 拆分辅助（原 569 行/cc154 → 薄壳分派+5 辅助）：
    //   OcmBuildMenu 菜单项装配；OcmShowProperties 4601/4602 属性框（现状两者共用）；
    //   OcmCopyDetails 4602 剪贴板分支（现行分派下不可达，保持结构等价，待 R15 评估）；
    //   OcmCopyJson 4606 JSON 复制；OcmHandleEnhancedCmds 4607-4610 打开位置/复制名称/
    //   复制命令/恢复默认。零行为改动（含历史死分支）。
    HMENU OcmBuildMenu(bool bHitRule, bool bCanRestore);
    void  OcmShowProperties(int g, int i, int nScope);
    void  OcmCopyDetails(int g, int i, int nScope);
    void  OcmCopyJson(int g, int i, int nScope);
    void  OcmHandleEnhancedCmds(int iCmd, int g, int i, int nScope);
    // [REFACTOR] 从 OcmHandleEnhancedCmds 提取：解析规则首个动作的 Key/Value/命令行。
    // 三个出参由调用方预置为空串，未解析到时保持为空。
    void  OcmResolveCommand(CMarkupNode node, int nScope,
                            CDuiString& sKey, CDuiString& sValue, CDuiString& sCmd);
    // [REFACTOR] 从 OcmHandleEnhancedCmds 按 iCmd 拆出的 4 个命令分支（逐字搬迁零改动）
    void  OcmCmdCopyName(const CDuiString& sName);                  // 4608 复制规则名
    void  OcmCmdOpenLocation(const CDuiString& sKey, const CDuiString& sName);   // 4607 打开位置
    void  OcmCmdCopyCommand(const CDuiString& sCmd, const CDuiString& sName);    // 4609 复制命令
    void  OcmCmdRestoreDefault(CMarkupNode node, int g, int i, int nScope,
                               const CDuiString& sName);            // 4610 恢复默认
    // [R169] 从 OcmCmdRestoreDefault 抽出：写注册表后若需重启 explorer 则询问并投递
    void  OcmPromptRestartIfNeeded(bool bRestart);
    // [R169] 从 OcmCmdRestoreDefault 抽出：按写入结果回写开关控件与组级徽标
    void  OcmSyncSwitchAfterChange(int g, int i, int nScope, int nAfter);
    // [R169] 从 ShowOptimContextMenu 抽出：右键菜单命令分派（4601/4602/4606/4607-4610）
    bool  OcmDispatchCommand(int iCmd, bool bHitRule, int g, int i, int nScope);
    // [R169] 从 OcmCopyJson 抽出：按可见作用域 × 动作标签白名单收集 JSON 条目
    void  OcmCollectActionEntries(CMarkupNode node, std::vector<CDuiString>& vActions);

    // 优化设置撤销栈（Ctrl+Z），上限 32
    // [F-OPT-1] 下拉(Combo)变更同样入栈：原结构只有开关的 bool 前态，
    //   下拉变更从不入栈 → Ctrl+Z 回退的是更早的开关操作而非用户最近
    //   的选择（栈序与用户操作错位）。补 combo 前态/后态两个字段。
    struct SOptimUndo
    {
        int g, i, nScope;
        bool bBefore;             // 开关撤销态（bIsCombo=false 时有效）
        bool bIsCombo    = false; // 下拉撤销记录
        int  nComboBefore = -1;   // 下拉撤销档位（入栈前系统实际档位，>=0）
        int  nComboAfter  = -1;   // 下拉重做档位（用户实际选中的档位）
    };

    // [R-B1] Redo 栈：PushOptimUndo 时把被覆盖的旧状态压入，Ctrl+Y 重做
    std::vector<SOptimUndo>         m_vOptimRedo;          // Redo 栈（上限 32）

    void ThrottleProgress(DWORD& dwLastMsRef, int nVal, int nMax);

    // [F-OPT-2] 批量闸门释放收口（BatchGate 析构调用）：复位 m_bBatchRunning
    //   并隐藏进度条。正常路径与其显式复位重复，幂等；异常路径由此兜底，
    //   避免"批量按钮永久失效 + 进度条残留"。
    void NotifyBatchGateReleased()
    {
        m_bBatchRunning = false;
        SetOptimProgress(0, 0);
    }

    int                             m_nOptimStateFilter;   // O-5: 0 全部 1 仅已启用 2 仅未启用
    DWORD                           m_dwOptimVerifyTick;   // 5 秒 TTL：上次全量状态核实时刻
    DuiLib::CProgressUI*            m_pOptimProgress;      // XML 内置 optim_progress（可能为 nullptr）
    DWORD                           m_dwLastOptimMs;       // 优化进度上次刷新时刻
    int                             m_nOptimPendingVal;    // 节流期间待提交值
    int                             m_nOptimPendingMax;    // 节流期间待提交 max
    // [G1 2026-10-25] 批量执行重入闸门。
    //   批量改为消息泵分片后，用户在批次间仍可点击；不拦截的话第二次点击会在
    //   第一轮 CMarkupNode 迭代中途再开一轮，同一规则被写两次、进度 max 被覆盖。
    bool                            m_bBatchRunning = false;
    // [STAB] VerifyOptimStates 重入闸门（与 m_bBatchRunning 同一模式）。
    //   VerifyCountAndSync 每组间泵消息（PumpUiMessages），泵内会派发
    //   WM_TIMER(PAGE_BUILD_TIMER_ID) → OnTimerPageBuild → VERIFY_SOFT →
    //   本函数在第一轮 CMarkupNode 迭代中途被再次进入 → 交错迭代/嵌套构建
    //   把控件 RemoveAll 掉 → 悬垂写 → 0xC0000374。G6 加泵时只给批量设了
    //   闸门，漏了这条切页路径——正是"切页随机崩溃"的根因。
    bool                            m_bVerifyingOptim = false;
    // [G4-opt] 最近一次命中行（-1 = 尚未交互过），键盘菜单锚点用
    int                             m_nOcmLastG = -1;
    int                             m_nOcmLastI = -1;
    int                             m_nOcmLastScope = -1;
    bool                            m_bFiltering;          // 关键字/状态过滤是否生效（原壳成员，仅本页写）
    // [FILTER-SSOT] 当前过滤关键字。批量/分组批量/状态统计此前靠 pSw->IsVisible()
    // 反推"用户此刻看得见哪些项"，但 IsVisible 同时受分组折叠影响：全部折叠时
    // 批量按钮判定为"没有可操作项"什么也不做，而分组复选框却能改——同一条规则
    // 两个入口给出相反结论。改为记录过滤关键字，用与界面完全相同的谓词求值。
    CDuiString                      m_sOptimFindText;
    std::vector<bool>               m_vExpanded;           // 优化分组折叠态
    std::vector<SOptimUndo>         m_vOptimUndo;          // 优化设置撤销栈（上限 32）

    // [R14-03] Build 拆解：复位过滤 / 统计组 / 组头行 / 开关行 / 下拉行 / 组收尾
    void OptimBuildResetFilter();                          // 复位状态快捷过滤按钮文本
    void OptimBuildCountGroups(CMarkupNode root);           // 统计 Group 子节点数，初始化 m_vExpanded
    CClickRowUI* OptimBuildGroupRow(const DuiLib::CDuiString& strGroupName, int g);
    void OptimBuildSwitchCell(CHoverRowUI* pItemRow, CMarkupNode item, int g, int i);
    void OptimBuildSwitchRow(CVerticalLayoutUI* pBody, CMarkupNode item,
                             const DuiLib::CDuiString& strDisplay,
                             int g, int i, bool bApplicable, bool bUnsupported);
    void OptimBuildComboCell(CHoverRowUI* pItemRow, CMarkupNode item, int g, int i);
    void OptimBuildComboRow(CVerticalLayoutUI* pBody, CMarkupNode item,
                            const DuiLib::CDuiString& strDisplay,
                            int g, int i, bool bApplicable, bool bUnsupported);
    void OptimBuildFinishGroup(CVerticalLayoutUI* pTree, CClickRowUI* pRow,
                               CVerticalLayoutUI* pBody, int nApplicable,
                               bool bExpanded);

    // [R14-04] BulkApplyAllOptim 拆解：统计 / 汇总警告 / 确认 / 批量执行 / 收尾
    void BaCountScopes(CMarkupNode root, int& nOn, int& nTotal);
    void BaCollectWarn(CMarkupNode root, bool bTarget,
                       std::vector<CDuiString>& vBulkWarn, int& nTodo, int& nPrivNeeded);
    bool BaShowConfirm(bool bTarget, int nTotal, const std::vector<CDuiString>& vBulkWarn,
                       int nPrivNeeded);
    // [R160] 从 BaExecuteBatch 抽出：单 条目×作用域 的执行单元（读态/写入/入栈/回读/计数）
    void BaExecOneScope(CMarkupNode item, int g, int ii, int sc, bool bShown, bool bTarget,
                        int& nDone, int nTodo, int& nUndoPushed,
                        int& nChanged, bool& bAnyFailed, bool& bAnyRestart);
    void BaExecuteBatch(CMarkupNode root, bool bTarget, int nTodo,
                        int& nChanged, bool& bAnyFailed, bool& bAnyRestart);
    void BaFinishReport(bool bTarget, int nChanged, int nTotal,
                        bool bAnyFailed, bool bAnyRestart);

    // [Privilege] 把提权说明追加到调用方的确认文案（只查询不弹窗）。
    // 返回 true 表示本次确实追加了（即该项需要 Admin 且当前未提权且本会话未提示过）。
    // [STAB] 取代早期的 ConfirmPrivilegeOnce：独立弹窗会与 Warning 确认框叠加，
    // 用户为一次开关要连点两次"是"，且中途取消会造成"提示已消耗但未应用"。
    // [R146] 应用失败提示装配（从 OnSwitchToggle 抽出，降 CC）
    void BuildApplyFailMessage(CMarkupNode node, int nScope,
                               const CDuiString& sItemName, CDuiString& sMsg);
    [[nodiscard]] bool AppendPrivilegeNote(CMarkupNode item, CDuiString& sMsg);

private:
    // [Privilege] 已提示过的条目键（g_i），避免反复弹窗
    std::set<std::wstring>          m_setPrivWarned;

};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
