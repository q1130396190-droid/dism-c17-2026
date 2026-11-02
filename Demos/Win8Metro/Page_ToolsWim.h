#pragma once

// ============================================================
// Page_ToolsWim.h —— WIM/ESD 互转与 SWM 合并的线程层（跨编译单元共享）。
//
// [R132-1] 从 Page_Tools.cpp 抽出 89 行线程基础设施：
//   TSwmMergeJob / SwmMergeThreadProc   SWM 分卷合并到 WIM
//   TWecJob      / WecJobThreadProc     WIM/ESD 互转（全部索引）
//
// 闭包证明：m_ctx 0 处、CPageTools:: 0 处、HostNotify/HostConfirm 0 处。
// 依赖：ToolJobs 作业层（Page_ToolsJobs.h）、ToolJobs::WecRunConvert
//      （Page_ToolsE2I.h 内声明）、BackgroundJobs、WIMBridge、StringMgr。
//
// 成员函数 CPageTools::ToolWimEsdConvert / WecChooseCompress /
// WecCheckTargetSpace / WecLaunchConvert / ToolSwmMerge 依赖 m_ctx，
// 仍留在主文件，通过本头创建线程上下文后交给以下线程过程。
// ============================================================

#include "stdafx.h"

#include <memory>
#include <string>

#include "WmMessages.h"
#include "MsgParam.h"
#include "WIMHelperBridge.h"
#include "StringManager.h"
#include "BackgroundJobs.h"

#include "Page_ToolsJobs.h"   // TJobWimProgress / CJobProgressWnd
#include "Page_ToolsE2I.h"    // ToolJobs::WecRunConvert 声明

namespace ToolWim
{

// 线程层复用 ToolJobs 作业层（声明见 Page_ToolsJobs.h）
using ToolJobs::TJobWimProgress;

// 互转作业的完成信封类型（BackgroundJobs.h 定义）
inline constexpr int kWecJobDoneKind   = 2;   // pEnv->nKind
inline constexpr int kSwmJobDoneKind   = 1;

struct TSwmMergeJob
{
    std::wstring wSwm;
    std::wstring wWim;
    HWND         hWait;
    HWND         hSink;
};

static DWORD WINAPI SwmMergeThreadProc(void* pv)
{
    std::unique_ptr<TSwmMergeJob> spJob(static_cast<TSwmMergeJob*>(pv));  // [C++17 R3] 线程 ctx：函数退出自动释放
    TSwmMergeJob* j = spJob.get();
    HWND hWait = j->hWait;
    HWND hSink = j->hSink;

    std::wstring sErr;
    bool bOk = false;
    try
    {
        TJobWimProgress sp(hWait, 0, 100);   // [rb28] 单索引导出：0-100 直传
        bOk = WIMBridge::ExportImageEx(j->wSwm, 1, j->wWim,
                                       WIM_COMPRESS_LZX,
                                       L"", L"", false, &sErr, &sp);
    }
    catch (...)
    {
        bOk = false;
        if (sErr.empty()) sErr = StringMgr.Get("toolbox.ui_ex_swm");
    }
    // [R16-T9] 统一信封投递（nKind=1：SWM 合并）
    // [C++17 阶段3] Post 消息所有权移交 UI，Post 失败自删（PostHeapOrDelete）
    auto pEnv = std::make_unique<TJobDoneEnvelope>(); // [C++17 R3]
    pEnv->nKind = 1;
    pEnv->bOk   = bOk;
    if (!bOk) pEnv->sMsg = sErr;

    if (hWait && ::IsWindow(hWait)) ::PostMessageW(hWait, WM_CLOSE, 0, 0);
    // [GC-07] 统一收尾：投递失败由助手就地回收，不再各写一套
    BackgroundJobs::PostHeapOrDelete(hSink, JOB_WM_DONE, 0, pEnv.release()); // [C++17 R3] ownership to helper

    // [C++17 R3] spJob unique_ptr 自动释放，所有权由启动方移交
    return 0;
}

struct TWecJob
{
    std::wstring wSrc;
    std::wstring wDst;
    DWORD        dwCompress;
    HWND         hWait;
    HWND         hSink;
    // [R105] 取消令牌：UI 点取消后置位，Bridge 在索引边界查到并删半成品
    BackgroundJobs::CancelTokenPtr pCancel;
};

static DWORD WINAPI WecJobThreadProc(void* pv)
{
    std::unique_ptr<TWecJob> spJob(static_cast<TWecJob*>(pv));  // [C++17 R3] 线程 ctx：函数退出自动释放
    TWecJob* j = spJob.get();
    HWND hWait = j->hWait;
    HWND hSink = j->hSink;

    DWORD dwErr = 0;
    bool bOk = false;
    try
    {
        TJobWimProgress sp(hWait, 0, 100);   // [rb28] Bridge 已做多索引折算
        WIMBridge::CancelQueryFn fnCancel = nullptr;
        if (j->pCancel)
        {
            BackgroundJobs::CancelTokenPtr pTok = j->pCancel;
            fnCancel = [pTok]() { return pTok->IsRequested(); };
        }
        bOk = WecRunConvert(j->wSrc, j->wDst, j->dwCompress, dwErr, &sp, fnCancel);
    }
    catch (...)
    {
        bOk = false;
        if (dwErr == 0) dwErr = ERROR_GEN_FAILURE;
    }

    // [R16-T9] 统一信封投递（nKind=2：WIM/ESD 互转）
    // [C++17 阶段3] Post 消息所有权移交 UI，Post 失败自删（PostHeapOrDelete）
    auto pEnv = std::make_unique<TJobDoneEnvelope>(); // [C++17 R3]
    pEnv->nKind = 2;
    pEnv->bOk   = bOk;
    if (!bOk)
    {
        CDuiString sMsg;
        const std::wstring sErrNo = std::to_wstring(dwErr);
        sMsg.Format(StringMgr.Get("toolbox.ui_convert_fail_fmt"), sErrNo.c_str());
        pEnv->sMsg = sMsg.GetData();
    }
    if (hWait && ::IsWindow(hWait)) ::PostMessageW(hWait, WM_CLOSE, 0, 0);
    // [GC-07] 统一收尾：投递失败由助手就地回收，不再各写一套
    BackgroundJobs::PostHeapOrDelete(hSink, JOB_WM_DONE, 0, pEnv.release()); // [C++17 R3] ownership to helper

    // [C++17 R3] spJob unique_ptr 自动释放，所有权由启动方移交
    return 0;
}

} // namespace ToolWim
