#pragma once

// ============================================================
// ScheduleTaskService —— S11（v2 P2b）/ [P1-C3] 全 COM 化
// 计划任务枚举、启停、运行、删除、XML 导入导出、定期清理计划注册。
// 全部经 Task Scheduler COM（ITaskService / ITaskFolder /
// IRegisteredTask / ITaskDefinition），零 schtasks.exe 进程。
//
// 纯 Win32，无 UI 控件依赖；实现文件不使用预编译头、不 include
// UIlib.h / StringManager.h。所有 COM 函数内部自管 CoInitializeEx，
// 可在任意线程直接调用。
// ============================================================

#include <vector>

#include <windows.h>

#ifndef UILIB_API
#define UILIB_API __declspec(dllimport)
#endif

#include "Utils/Utils.h"

class ScheduleTaskService
{
public:
    // [P1-C3] 枚举结果项（字段对应任务列表 5 列）
    struct TaskInfo
    {
        DuiLib::CDuiString sName;        // 完整路径（"\Microsoft\Windows\..."）
        DuiLib::CDuiString sNextRun;     // 下次运行时间（无可触发点时为空）
        DuiLib::CDuiString sStatus;      // Ready / Running / Disabled / Queued / Unknown
        DuiLib::CDuiString sMode;        // 登录模式（Interactive/S4U/Password/...）
        DuiLib::CDuiString sUser;        // Run As User
        bool               bEnabled;     // !Disabled
        bool               bRunning;     // State == TASK_STATE_RUNNING
    };

    // 定期清理计划任务名（原两处局部 sTaskName 常量）
    static const DuiLib::CDuiString& CleanTaskName();

    // [P1-C3] COM 注册每周一 03:00 静默清理任务（TASK_CREATE_OR_UPDATE）。
    // 返回 0 = 成功；非 0 = HRESULT 低 32 位。
    static DWORD CreateWeeklyCleanSilentTask(const DuiLib::CDuiString& sExePath);

    // [P1-C3] COM 删除定期清理任务；不存在视为成功（0）。
    static DWORD DeleteCleanTask();

    // [P1-C3] ITaskService 递归枚举全部文件夹下全部任务。
    // 返回 0 = 成功（可能为空列表）；非 0 = HRESULT 低 32 位。
    static int EnumTasks(std::vector<TaskInfo>& vOut, DWORD& dwErrOut);

    // [R78/R86] 启停任务（IRegisteredTask::put_Enabled）。
    // sTaskPath 为任务完整路径（含开头 "\"）。
    // 返回值 0 = 成功；非 0 = 失败（HRESULT 低 32 位）。
    static DWORD SetTaskEnabledCOM(const DuiLib::CDuiString& sTaskPath, bool bEnable);

    // [R78/R86] 立即运行任务（IRegisteredTask::Run）。
    static DWORD RunTaskCOM(const DuiLib::CDuiString& sTaskPath);

    // [R78/R86] 删除任务（ITaskFolder::DeleteTask）。
    static DWORD DeleteTaskCOM(const DuiLib::CDuiString& sTaskPath);

    // [R86] 检测 ITaskService 是否可用（COM 接口可加载），用于 UI 侧提示
    static bool IsTaskServiceAvailable();

    // [R127] 导出任务 XML（ITaskDefinition::get_XmlText 直连）。
    // 返回：0 成功；1 无 TaskService；2 任务未找到；3 get_XmlText 失败
    //      （dwErr 回传 HRESULT 低 32 位）。
    static int ExportTaskXml(const DuiLib::CDuiString& sTaskPath,
                             DuiLib::CDuiString& sXmlOut, DWORD& dwErrOut);

    // [R127] 导入任务 XML（ITaskFolder::RegisterTaskDefinition 直连）。
    // 返回：0 成功；1 无 TaskService；2 打开根目录失败；3 创建 ITaskDefinition 失败；
    //      4 LoadXml 失败；5 RegisterTaskDefinition 失败
    //      （dwErr 回传 HRESULT 低 32 位）。
    static int ImportTaskXml(const DuiLib::CDuiString& sTaskPath,
                             const DuiLib::CDuiString& sXml, DWORD& dwErrOut);
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
