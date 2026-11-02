#pragma once

// ============================================================
// Page_Tasks —— v2 P2b 纵切页面（content_area 页索引 11：计划任务）
//
// 承接原 CFrameWnd 的 9 个任务页函数（BuildScheduledTasksPage /
// UpdateTaskCount / TaskButton / ShowTaskContextMenu /
// OnTaskToggle / OnTaskRun / OnTaskDelete / OnTaskTabChanged /
// ApplyTaskFilter）及 TaskEntry 结构与 m_vTasks 视图数据。
// schtasks 进程调用归 ScheduleTaskService（S11）。
//
// 物理搬迁护栏：函数体零逻辑改动，仅把宿主引用从 CFrameWnd
// 隐式成员改为 m_ctx.Xxx（FindControl/日志/Toast）。
// ============================================================

#include <vector>
#include "Utils/Utils.h"
#include "PageBase.h"
#include "ScheduleTaskService.h"   // [P1-C3] TaskInfo 结构化枚举结果

namespace DuiLib { class CVerticalLayoutUI; }

class CPageTasks : public CPageBase
{
public:
    explicit CPageTasks(IPageContext& ctx)
        : CPageBase(ctx), m_nSelected(-1), m_bQuerying(false) {}

    // [R-B84] 改为异步：仅清表+起后台枚举线程，结果经 WM_TASK_DATA
    // 由 OnTaskData 在 UI 线程落地，避免枚举冻结界面。
    void Build() override;   // 原 BuildScheduledTasksPage（现：发起异步枚举）

    // [R3] F5 刷新无动作（保持原 switch 缺省行为，枚举仅 Build/按钮触发）
    void OnRefreshPage() override {}

    // [R-B84/P1-C3] 后台枚举结果（工作线程 new、PostMessage 回 UI、UI 线程 delete）
    struct TaskQueryMsg
    {
        int      nRet;       // 0 成功
        DWORD    dwErr;      // HRESULT 低 32 位（nRet != 0 时有效）
        std::vector<ScheduleTaskService::TaskInfo> vTasks;
        TaskQueryMsg() : nRet(-1), dwErr(0) {}
    };

    // [R-B84] WM_TASK_DATA 落地函数（pMsg 由本函数负责 delete）
    void OnTaskData(TaskQueryMsg* pMsg);

    // [R168] 从 OnTaskData 抽出：构建任务行并同步缓存（列宽与表头对齐）
    bool TaskAppendRow(CVerticalLayoutUI* pList,
                       const ScheduleTaskService::TaskInfo& ti, int nRowIndex);
    // [R5] 后台枚举消息统一入口（WM_TASK_DATA）
    bool OnAsyncMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) override;

    // P8：第二级事件路由（原壳 Notify 计划任务页分支）
    void OnEvent(TNotifyUI& msg) override;

    // [R102] 行级右键菜单：taskrow_%d 命中后调 ShowContextMenu / UiHelpers
    bool OnContextMenu(POINT pt, bool bFromContextMsg) override;

    void UpdateCount(int nCount);                          // 原 UpdateTaskCount
    void ShowContextMenu(int nTaskIndex, POINT pt);        // 原 ShowTaskContextMenu
    void OnToggle(bool bEnable);                           // 原 OnTaskToggle
    void OnRun();                                          // 原 OnTaskRun
    void OnDelete();                                       // 原 OnTaskDelete
    void OnTabChanged(int idx);                            // 原 OnTaskTabChanged
    void ApplyFilter();                                    // 原 ApplyTaskFilter

    // [R127] 行级右键菜单命令分发（iCmd 见 ShowContextMenu）
    void OnTaskMenuCmd(int k, int iCmd);
    // [R127] 导出任务 XML 到 .xml 文件（SaveFileDialog + ExportTaskXml）
    void OnExportTaskXml(int k);
    // [R127] 打开"任务计划程序"控制台（taskschd.msc）
    void OnOpenTaskScheduler(int k);
    // [R127] 复制任务完整路径 / 任务名 到剪贴板
    void OnCopyTaskPath(int k);
    void OnCopyTaskName(int k);

    // One scheduled task entry (from schtasks.exe /query /fo csv /v)
    struct TaskEntry
    {
        TaskEntry() : bEnabled(true) {}
        DuiLib::CDuiString sName;        // task name (with folder path)
        DuiLib::CDuiString sNextRun;     // next run time (or "已禁用")
        DuiLib::CDuiString sMode;        // task mode (ready/run/disabled)
        DuiLib::CDuiString sStatus;      // status
        DuiLib::CDuiString sUser;        // run as user
        bool       bEnabled;
    };

    const wchar_t* PageName() const override { return L"Tasks"; }
    // [R232] Colours only -- rows live in inc_task_panel.xml (Startup sub-tab).
    void RefreshThemeColors() override;
    void    OnGlobalCopy(bool bCut) override;
    void    OnGlobalSave() override;
private:
    std::vector<TaskEntry> m_vTasks;

    // [R78] 当前选中任务行索引（-1 表示无选中），OnToggle/OnRun/OnDelete 使用
    int m_nSelected;

    // [R-B84] 是否已有后台枚举线程在途（防止快速切 Tab/刷新导致并发查询）
    bool m_bQuerying;

    // [R78] 取当前选中索引；越界或无选中返回 -1
    int CurrentSelectedIndex() const;
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
