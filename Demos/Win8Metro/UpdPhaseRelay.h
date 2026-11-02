#pragma once

// ============================================================
// UpdPhaseRelay.h —— [R-B51] 升级链阶段回传通道
// ------------------------------------------------------------
// 清单维度 7 缺失 3：三阶段自调用拉起已通，但子进程→主进程的
// 进度/错误码回传通道未见。本模块补齐该通道：
//   * 子进程（被 /PreDownload|/Install|/Finalize 拉起）按阶段写
//     临时状态文件（UTF-8 单 BOM INI 文本）并在每次状态变化时
//     set 命名事件；
//   * 主进程 StartUpdatePhase 经 RunHiddenExit 等待子进程退出，
//     随后读状态文件驱动 9 态文本，失败时携带错误码/重试标志
//     供 UI 弹 EToolDlgKind::TD_LOGVIEW 定位；
//   * 双通道互补：事件仅表示"有更新"，状态文件承载结构化内容；
//   * 状态文件路径由主进程经 /path= 传入（同 UpdateInfo.zip
//     目录或 temp\updphase_xxx.ini），子进程无默认路径假设；
//   * 写/读失败均不阻塞：子进程照常退出，主进程按退出码兜底。
//
// 设计原则：纯系统层，不依赖 UI；与 CommandLineService /
// Page_Update 同风格，可独立编译测试（_cl_check_RB51.bat）。
// ============================================================

#include <windows.h>
#include "Utils/Utils.h"

#ifndef UILIB_API
#define UILIB_API __declspec(dllimport)
#endif

// 命名事件（主/子进程强约定，头文件内 internal linkage 副本等价）
static const wchar_t* const kUpdPhaseEventName = L"Local\\Win8Metro_UpdPhase_v1";

// 9 态映射：与 Page_Update::GetUpdStateText 的 0-8 保持一致
enum class EUpdState : int
{
    EUPD_NOTSTART   = 0,   // 未开始
    EUPD_PREPARING  = 1,   // 准备中
    EUPD_DOWNLOAD   = 2,   // 下载中
    EUPD_VERIFY     = 3,   // 校验中
    EUPD_INSTALL    = 4,   // 安装中
    EUPD_COMPLETING = 5,   // 完成中
    EUPD_DONE       = 6,   // 完成
    EUPD_FAILED     = 7,   // 失败
    EUPD_WAITING    = 8    // 等待中
};

class UpdPhaseRelay
{
public:
    // ---- 子进程侧 ----
    // 写阶段状态到 iniPath（UTF-8 单 BOM），然后 set 命名事件。
    // 返回 true 表示文件写成功（事件 set 始终尝试）。
    static bool WritePhase(const wchar_t* iniPath, int nPhase, int nState,
                           int nErrorCode, int bRetry, const wchar_t* szMsg);
    // 仅 set 命名事件（无状态文件时用于唤醒主进程）
    static bool SignalEvent();

    // ---- 主进程侧 ----
    // 等待命名事件（毫秒），返回 true 表示收到信号；false 超时
    static bool WaitSignal(DWORD dwTimeoutMs);
    // 读状态文件并解析出 nState/nErrorCode/nRetry/msg
    // 返回 true 表示文件存在且可解析
    static bool ReadPhase(const wchar_t* iniPath,
                          int& nState, int& nErrorCode,
                          int& bRetry, DuiLib::CDuiString& sMsg);
    // 生成一个阶段状态文件路径（temp\updphase_<phase>_<pid>.ini）
    static DuiLib::CDuiString MakePhaseFile(int nPhase);
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
