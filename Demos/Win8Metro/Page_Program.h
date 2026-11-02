#pragma once

// ============================================================
// Page_Program —— v2 P3a 纵切页面（程序和功能：已安装程序）
//
// 承接原 CFrameWnd 的 5 个程序页函数（BuildProgramPage /
// OnProgramRowSelect / ShowProgramContextMenu / ModifyProgram /
// UninstallProgram）及视图数据 m_vPrograms 与选中态 m_nProgSelected
//（prow_%d 行序号严格一一对应）。
// 三源 Uninstall 注册表枚举归 ProgramService（S4）。
//
// 物理搬迁护栏：函数体零逻辑改动，仅把宿主引用从 CFrameWnd
// 隐式成员改为 m_ctx.Xxx（FindControl/Hwnd/Log/Confirm）。
// ============================================================

#include <vector>
#include <utility>   // [INTERACT-FIX] std::pair（ProgBatchMsg.vFailures）
#include "Utils/Utils.h"
#include "PageBase.h"
#include "ProgramService.h"   // ProgEntry
#include "CustomControls.h"   // CClickRowUI (R14-03 ProgBuildRow/NameText/MetaText)

struct ProgEntry;
struct ProgEnumCtx
{
    HWND hWnd = nullptr;
    std::vector<ProgEntry> v;
    int  nGen = 0;   // [R3-FIX] generation 守卫，同 Driver/Capability/Update
};

// [INTERACT-FIX] 批量卸载后台完成回传（WM_PROG_BATCHDONE，new，UI 线程 delete）。
// 卸载执行体在工作线程逐条 CreateProcess+等待（单条最长 120s），
// 全部结束后一次性回传计数与失败名单，UI 侧收口日志/Toast/列表刷新。
struct ProgBatchMsg
{
    int nOk = 0;
    int nFail = 0;
    int nTotal = 0;
    // 失败条目（名称 + 线程内捕获的 GetLastError，UI 侧再取错误码已失真）
    std::vector<std::pair<DuiLib::CDuiString, unsigned long>> vFailures;
};

class CPageProgram : public CPageBase
{
public:
    explicit CPageProgram(IPageContext& ctx) : CPageBase(ctx), m_nSel(-1) {}

    void Build() override;   // 原 BuildProgramPage
    bool OnAsyncMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) override;
    void StartProgramRefresh();
    void RenderProgramList(const std::vector<ProgEntry>& v);
    void OnHostTimer(UINT uId) override;
    void RenderProgramStep();

    // P8：第二级事件路由（原壳 Notify / WM_RBUTTONUP / VK_DELETE 本页分支）
    void OnEvent(TNotifyUI& msg) override;
    bool OnContextMenu(POINT pt, bool bFromContextMsg) override;
    [[nodiscard]] bool OnDeleteKey() override;

    // [AUDIT-P2] OnEvent 原 146 行 / CC=37，按事件类型拆为 4 个处理函数：
    //   PrgOnClick        click  分支（批量卸载/刷新/删除/行选中）
    //   PrgOnTabChanged   progtab_ selectchanged 分支
    //   PrgApplyFilter    prog_filter textchanged 分支
    //   PrgOnDoubleClick  dbclick 分支（卸载）
    // 拆前拆后路由语义逐字等价（含 Tab1 向 Feature 代理的兜底转发）。
    // [BUGFIX-20261025] click/dbclick 处理器返回"是否消费"：false 时由
    // OnEvent 兜底转发 ForwardFeatureEventInTab1，修复功能页勾选/展开失效。
    bool PrgOnClick(const CDuiString& sName);
    void PrgOnTabChanged(const CDuiString& sName);
    void PrgApplyFilter();
    bool PrgOnDoubleClick(const CDuiString& sName);

    void OnRowSelect(int n); // 原 OnProgramRowSelect
    void ShowContextMenu(int n);  // 原 ShowProgramContextMenu
    void Modify(int n);      // 原 ModifyProgram

    // [R162] 从 Modify 抽出：命令行构造 / fix.log 追加
    // [R167] 从 ProgShowFixLog 抽出：读 fix.log 并输出末段（嵌套 5→2）
    [[nodiscard]] bool ProgReadFixLogAndShow();

    // [R170] 从 BatchUninstallPrograms 抽出：收集勾选。
    // [INTERACT-FIX] 单条执行（PrgRunUninstallOne）已迁为工作线程静态函数，
    // 不再作为成员（成员版依赖 m_ctx.HostLog，禁止在 UI 线程外调用）。
    [[nodiscard]] std::vector<int> PrgCollectCheckedRows(CVerticalLayoutUI* pList);

    // [R175] 从 OnContextMenu 抽出：行命中 / Tab1 转发给 Feature（嵌套 4→2）
    [[nodiscard]] int PrgRowFromHit(CControlUI* pHit);
    [[nodiscard]] bool PrgForwardToFeatureMenu(POINT pt, bool bFromContextMsg);

    // [R175] 从 PrgApplyFilter 抽出：Tab0 关键字过滤（嵌套 4→2）
    void PrgApplyFilterTab0(const CDuiString& sKeyword, bool bNoKw);

    // [R175] 从 Uninstall 抽出：启动卸载器（含长路径引号重试，不等待）
    [[nodiscard]] bool PrgLaunchUninstaller(const CDuiString& sUninstall);

    [[nodiscard]] bool BuildModifyCommand(const ProgEntry& e,
                                          CDuiString& sCmd, CDuiString& sProduct);
    void AppendFixLog(const CDuiString& sName, const CDuiString& sProduct,
                      const CDuiString& sCmd);
    void Uninstall(int n);   // 原 UninstallProgram
    void BatchUninstallPrograms();  // [R70] 批量卸载（P-P2 新增）

    // 供核心壳底部"卸载"按钮/刷新等路由读取当前选中行
    int  Selected() const { return m_nSel; }

    const wchar_t* PageName() const override { return L"Program"; }
    // [R210] 主题切换（壳 ApplySystemTheme 逐页回调）：按令牌重算行底色与各列文字色。
    //   PageBase.h 契约：只改颜色属性，不得 Build/Add/Remove（会丢列表与选中态，
    //   且 m_pFocusCtrl 悬挂）。
    void    RefreshThemeColors() override;
    // 行内文字色的唯一计算点：Build / OnRowSelect / RefreshThemeColors 三处共用，
    //   避免"构建时算一套、选中时写死另一套"的漂移。
    void    OnGlobalCopy(bool bCut) override;
private:
    std::vector<ProgEntry> m_vPrograms;   // 与 prow_ 行序号一一对应
    int                    m_nSel;        // 原 m_nProgSelected，-1 无选中
    int                    m_nProgGen = 0;   // [R3-FIX] generation 守卫，同 Update/Driver
    bool                   m_bBatchUninstalling = false;  // [INTERACT-FIX] 批量卸载在途闸门
    size_t                 m_nRenderPos = 0;   // [FIX-PERF] 首屏分帧进度
    // [R8] 行控件缓存：过滤/选中不再逐行 HostFindControl（名称查找）。
    // 与 m_vPrograms 下标一一对应；列表重建时同步重置。
    std::vector<DuiLib::CControlUI*> m_vRowPtr;
    std::vector<DuiLib::CDuiString>  m_vRowSearch;   // 预计算小写搜索串

    // [R14-03] Build 拆解：行构建三段 + fix.log 预览 + 图标统计
    void ProgBuildRow(const ProgEntry& e, size_t k);   // 内部自建 pRow 并 Add 到 prog_apps_list
    void ProgBuildNameText(CClickRowUI* pRow, const ProgEntry& e);
    void ProgBuildMetaText(CClickRowUI* pRow, const ProgEntry& e);
    void ProgShowFixLog();      // [R51] 读取 fix.log 最近 10 条写入主日志区
    void ProgLogIconStat();     // [R56] 图标就绪统计

    // [REFACTOR] 从 OnEvent 提取：Tab1（Windows 功能）未被共享分支消费的
    // click/dbclick 经 PAGE_FEATURE 代理转发给功能子视图。
    void ForwardFeatureEventInTab1(TNotifyUI& msg);
};

// [R76] fix.log 预览纯函数：拆分/取尾，实现于 Page_ProgramBuild.cpp。
// 二者仅被 CPageProgram::ProgShowFixLog 使用，声明在类外以保持头文件依赖最小。
std::vector<DuiLib::CDuiString> SplitFixLogLines(const DuiLib::CDuiString& sContent);
void LogFixLogTail(IPageContext& ctx, const std::vector<DuiLib::CDuiString>& vLines);
