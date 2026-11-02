#pragma once

// ============================================================
// Page_Feature —— 程序和功能 → Windows 功能 Tab
//
// [P0-A3 2026-10-11] 数据源原生直连：
//   Load() 启动 FeatureService::StartEnumThread（dismapi DismGetFeatures），
//   结果经 WM_FEAT_DATA 回 UI 线程 OnData；应用经 WM_FEAT_APPLYDONE 回报。
//   旧的 _getfeat.bat/runas/featraw.sig 轮询与 FEAT_TIMER_ID 已删除。
// ============================================================

#include <vector>
#include <unordered_map>   // [T8 P3] sSys 索引，FindBySys 由线性扫描改哈希查表
#include "Utils/Utils.h"
#include "PageBase.h"
#include "FeatureService.h"   // FeatInfo / FeatRawEntry / FeatDataMsg / FeatApplyMsg

class CPageFeature : public CPageBase
{
public:
    explicit CPageFeature(IPageContext& ctx)
        : CPageBase(ctx), m_bFeatLoaded(false), m_bFeatLoading(false),
          m_bApplying(false), m_nFeatSelected(-1),
          m_dwFeatEnumStartTick(0), m_nFeatWatchdogMs(120000) {}

    // 功能列表是程序页的第二个 Tab：首次切到该 Tab 时自动 Load()（与原版
    // 一致），btn_prog_refresh 可手动强制重载；核心壳 EnsurePageBuilt 不调用。
    void Build() override {}

    // P8：第二级事件路由（原壳 Notify 功能页分支）
    void OnEvent(TNotifyUI& msg) override;

    int  FindBySys(const CDuiString& sSys);
    void BuildSysIndex();                     // [T8 P3] 重建 sSys → 下标哈希索引
    void Load();                              // 启动后台原生枚举
    // [FIX-T3-4] 刷新请求标志：Load() 会话复用早退的例外通道——刷新入口
    // 置位后 Load 强制丢弃会话数据真实枚举（原刷新在有数据时完全无效）。
    bool m_bFeatRefreshRequested = false;
    void OnData(FeatDataMsg* pData);

    // [R162] 从 OnData 抽出（原 178 行 / 嵌套 5）：失败兜底 / 差异比对
    [[nodiscard]] bool FeatRenderEnumError(const CDuiString& sErr);
    [[nodiscard]] bool FeatIsSameAsCurrent(const std::vector<FeatRawEntry>& vRaw);

    // [R162] 从 Load 抽出：三级缓存兜底（新鲜 / 过期 / 包内种子）+ 短路判定
    [[nodiscard]] bool FeatSeedFromCache(std::vector<FeatRawEntry>& vCache, bool& bHitFresh);

    // [R175] 从 FeatSeedFromCache 抽出：[R85] 包内种子是否仍可信（嵌套 4→3）
    [[nodiscard]] bool FeatSeedStillValid();

    // [R175] 从 RebuildListBatchStep 抽出：首行 TTFR 采样（嵌套 4→3）
    void FeatLogTtfrOnce(bool& bLogged, int nTotal);

    // [R175] 从 OnApplyDone 抽出：[R79] 写后精准失效（嵌套 4→3）
    void FeatWriteBackChangedRows();

    // [R175] 从 OnTabChanged 抽出：是否存在待应用变更（嵌套 4→3）
    [[nodiscard]] bool FeatHasPendingChange() const;

    // [R175] 从 OnContextMenu 抽出：命中控件 → 功能行号（-1 = 未命中/越界）
    [[nodiscard]] int FeatRowFromHit(CControlUI* pHit);

    // [R175] 从 OnEvent 抽出：选择功能源目录并回填（嵌套 4→3）
    void FeatPickSourceDir();          // WM_FEAT_DATA（delete 由本方法负责）
    // [P1] 枚举分批回报：追加一批并增量渲染；末批收尾。消息对象在此释放。
    void OnPartial(FeatPartialMsg* pMsg);
    // [PROGRESS 2026-10-25] 把已发送数/总数写进加载占位行的文案。
    //   本页无独立进度条控件，故就地改文案；
    //   不改 XML 布局，也不会在列表里插入额外行。
    void UpdateFeatLoadingProgress(int nSent, int nTotal);
    // [T1 P0] OnData 成功分支抽出的公共入口：vRaw → m_vFeatures 建树 + 渲染。
    // 缓存命中（LoadFeatCache）时也走此路径实现 stale-while-revalidate 秒开。
    void ApplyRawData(const std::vector<FeatRawEntry>& vRaw);
    // [rb23] 枚举超时兜底：线程被 SEH 吞掉/API 卡死时解除永久加载态
    void OnLoadWatchdog();
    int  Eff(int n);
    int  Agg(int n);
    void InvalidateAggCache();
    void RebuildList();
    // [T8 P3] 分批渲染接力：50 行/批 + 15ms WM_TIMER，对照 Upd 页 UpdStartBatchRender。
    // 首批同步渲染，保证首屏不空白；接力期间禁行点击，避免命中未挂载行。
    void RebuildListBatchStep();
    // [T8 P3] 单行渲染（原 renderRow lambda 逐句搬迁）与接力收尾
    void RenderFeatRow(CVerticalLayoutUI* pList, int n, int depth);
    void FinishRebuildList();
    void OnHostTimer(UINT uId) override;
    void ToggleCheck(int n);
    void ToggleExpand(int n);
    // [CRASHFIX-UAF] 勾选/展开后的整表重建延迟入口（见 Page_FeatureInteraction.cpp）
    void DeferRebuildList();
    void Apply();
    void OnApplyDone(FeatApplyMsg* pMsg);     // WM_FEAT_APPLYDONE（delete 由本方法负责）

    // ---- 核心壳路由迁入 ----
    void OnTabChanged(int idx);
    void DeleteSelected();                    // btn_prog_delete Tab=1 分支（原生线程）
    void ApplyFilterKw(const CDuiString& sKeyword, bool bNoKw);

    // [R124-2] 行级右键菜单增强：feat_row_{n} 命中 → 弹出行级菜单
    bool OnContextMenu(POINT pt, bool bFromContextMsg) override;
    void ShowFeatContextMenu(int n, POINT pt);
    void CopyFeatSysName(int n);       // 复制 DISM 功能名 sSys
    void CopyFeatDispName(int n);      // 复制显示名 sDisp
    void CopyFeatState(int n);         // 复制状态描述
    void ShowFeatDetail(int n);        // 查看详细状态（Toast + 日志）

    const wchar_t* PageName() const override { return L"Feature"; }
    // [R233] Colours only -- a rebuild would re-run the batched renderer.
    void RefreshThemeColors() override;
    void    OnGlobalCopy(bool bCut) override;
private:
    // [rb26] 右侧「功能详情」面板：选中功能行后显示中文名/状态/说明/内部名
    void ShowDetailInPanel(int n);
    void ClearDetailPanel();
    // [R124-2] 根据当前 pending/commit 派生状态文本
    CDuiString FeatStateText(int n);
    // 收集 pending 变更；bFromDelete=true 时为右键永久移除单项路径
    void LaunchApply(const std::vector<FeatApplyItem>& vItems);

private:
    std::vector<FeatInfo> m_vFeatures;
    // [T8 P3] sSys → m_vFeatures 下标索引。FindBySys 在 RebuildList 建树时
    // 每行都要调一次（kFeatMeta × 组），线性扫描是 O(n²)；索引化后 O(n)。
    // 键为小写化后的 sSys，值为首次出现的下标（与原线性扫描"首个匹配"语义一致）。
    std::unordered_map<std::wstring, int> m_mapSysIndex;
    bool                  m_bFeatLoaded;
    bool                  m_bFeatLoading; // 后台枚举进行中，防切 Tab 重复发起
    // [P1] 分批回报状态：累计已到达的原始条目 + 是否已收尾（整表到达或末批）
    std::vector<FeatRawEntry> m_vBatchRaw;
    bool                  m_bFeatPartialDone = false;
    bool                  m_bApplying;   // [P0-A3] 应用进行中，屏蔽重复提交
    // [R79 P1] 本次应用的条目快照：OnApplyDone 成功时据此只更新缓存里对应行，
    // 而不是删掉整个缓存文件（删了下次进页必然全量重扫）。
    std::vector<FeatApplyItem> m_vLastApply;
    int                   m_nFeatSelected;

    // [R16][O-1] 看门狗阈值自适应：记录枚举起始 tick，OnData 实测耗时后
    // 将阈值更新为耗时×2（下限 30s/上限 600s）；首次保持 120s，
    // 到期只提示、不重启枚举。
    DWORD                 m_dwFeatEnumStartTick;
    UINT                  m_nFeatWatchdogMs;

    // [T8 P3] 分批渲染接力状态（对齐 CPageUpdate::m_pRenderList/m_nRenderPos）：
    // 行按下标惰性展开成扁平序列，每批 50 行；接力期间 m_bBatchRendering=true，
    // 行点击/勾选/展开一律忽略，避免命中尚未挂载的行。
    std::vector<int>      m_vRenderRows;      // 扁平化后的行下标（含 depth 编码）
    std::vector<int>      m_vRenderDepths;    // 与 m_vRenderRows 一一对应的缩进层级
    std::vector<int>      m_vAggCache;        // 聚合状态缓存（Agg 一次算全表）
    // [R162] 跨 TU 状态单一入口。选中索引 5 处直写、聚合缓存"置真" 2 处直写，
    //   而失效只有一个 InvalidateAggCache —— 新增"改状态"路径时漏调失效，
    //   就会出现"分组计数显示旧值"，且只在特定操作顺序下复现，最难排查。
    void SelectFeat(int n)  { m_nFeatSelected = n; }
    void MarkAggValid()     { m_vAggValid = true; }

    bool                  m_vAggValid = false;
    // [R10] 行缓存：feat_row_{n} 的控件指针与预计算小写搜索串（按功能下标存储）。
    // 过滤时直接按缓存迭代，不再逐行 Format + HostFindControl + 两次 MakeLower。
    std::vector<DuiLib::CControlUI*> m_vFeatRowPtr;
    std::vector<DuiLib::CDuiString>  m_vFeatRowSearch;
    size_t                m_nRenderPos = 0;   // 已渲染到的位置
    // [A1 2026-10-25] 渲染起点 tick。与 m_dwFeatEnumStartTick（枚举起点）分开：
    //   混用会把排队时间算进渲染耗时，得出"渲染慢"的错误结论。
    DWORD                 m_dwFeatRenderStartTick = 0;
    bool                  m_bBatchRendering = false;
    // [CRASHFIX-UAF] HostDefer 去重标志：true=已排入一次延迟重建，
    // 回调执行时复位。连点多次只重建一次，最终态按 m_vFeatures 现值渲染。
    bool                  m_bPendingRebuild = false;
    // [FIX-UX] 渲染接力期间的点击意图队列：旧实现只弹 Toast 并丢弃点击，
    //   用户感知为"点了没反应"。现记录意图（同一下标同类操作去重），
    //   FinishRebuildList 收尾时统一补执行，最终态与用户点选一致。
    std::vector<std::pair<int, bool>> m_vPendingIntents;   // (功能下标, true=展开/false=勾选)
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
