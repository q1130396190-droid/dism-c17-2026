#pragma once

// ============================================================
// Page_CleanupInternal.h —— 清理页跨翻译单元共享的内部辅助。
//
// [R140] Page_Cleanup.cpp 原 2242 行（空行规范化后 1663），规模分 6.35
// 为全仓最低。把 ScRun* 自定义清理族（12 路进程派发 + Package Cache
// 安全剪枝）迁入 Page_CleanupOps.cpp 后，"在线组件清理"这一原语被
// 两个 TU 同时使用：
//   · Page_CleanupOps.cpp  —— ScRunDismHardLinkMerge 内调用
//   · Page_Cleanup.cpp     —— OcRunDismComponent 内调用
// 故提升为本头中的 inline 实现，避免两处各留一份 static。
//
// 仅为同一页面内部的实现细节，不属于公共接口。
// ============================================================

#include "stdafx.h"

#include "DismApiBridge.h"     // DismApiBridge::Init / CleanComponentStore
#include "OfflineSession.h"    // COfflineSession::IsActive / GetImageRoot
#include "WinHandles.h"        // win::HandlePtr（互斥量句柄 RAII）

namespace cleanup_detail
{

// [R7] 组件清理进度采样：DISM 回调可能来自工作线程，只做原子计数，
// 不触碰 UI 与 DuiLib，避免跨线程访问控件。
struct CleanProgressState
{
    volatile LONG nCurrent = 0;
    volatile LONG nTotal = 0;
    // [R18-b] 请求取消标志：由 UI 线程置 1，进度回调只读取并记录。
    // 注意：DismCleanImage 是同步阻塞调用，本身不可中断；
    // 这里只能做到"记录请求 + 不再启动后续阶段"，不能宣称立即中止当前调用。
    volatile LONG bCancelRequested = 0;
};

inline void CleanProgressSink(unsigned int nCurrent, unsigned int nTotal, void* pUser)
{
    CleanProgressState* p = static_cast<CleanProgressState*>(pUser);
    if (!p) return;
    ::InterlockedExchange(&p->nTotal, static_cast<LONG>(nTotal));
    ::InterlockedExchange(&p->nCurrent, static_cast<LONG>(nCurrent));
    // 回调里不做事：取消由调用方在 DISM 返回后判断，避免在 DISM 线程内做重活。
}

// [R18-b] 请求取消：线程安全置位。返回是否已接受（重复请求返回 false）。
inline bool RequestCleanCancel(CleanProgressState& st)
{
    return ::InterlockedExchange(&st.bCancelRequested, 1) == 0;
}

inline bool IsCleanCancelRequested(const CleanProgressState& st)
{
    return ::InterlockedCompareExchange(
        const_cast<volatile LONG*>(&st.bCancelRequested), 0, 0) != 0;
}

// [R-B52] 在线/离线组件清理（WinSxS）：DismApiBridge 进程内 dismapi 直连，
// 零 dism.exe 子进程、零 CbsHost 回退。
// 返回 0 表示成功；非 0 表示失败（dismapi 不可用或调用失败）。
// [R7] 重载：附带 DISM 原生进度，最终进度写入日志，便于确认清理确实推进。
// [R18-b] 额外接收取消状态：若期间收到取消请求，结果标为"已请求取消"，
// 由调用方决定是否跳过后续阶段。
[[nodiscard]] inline int RunOnlineComponentCleanupEx(bool bResetBase, CDuiString& sLog,
                                                     CleanProgressState* pCancelOut = nullptr)
{
    std::wstring sErr;
    DismApiBridge::Init();
    const wchar_t* pszImg = COfflineSession::IsActive()
        ? COfflineSession::GetImageRoot().GetData() : nullptr;

    CleanProgressState prog;
    if (pCancelOut) prog.bCancelRequested = pCancelOut->bCancelRequested;
    bool bOk = DismApiBridge::CleanComponentStoreEx(
        bResetBase, &sErr, &CleanProgressSink, &prog, pszImg);

    const LONG nCur = ::InterlockedCompareExchange(&prog.nCurrent, 0, 0);
    const LONG nTot = ::InterlockedCompareExchange(&prog.nTotal, 0, 0);
    const bool bCancelled = IsCleanCancelRequested(prog);
    if (pCancelOut) pCancelOut->bCancelRequested = prog.bCancelRequested;

    CDuiString sErrStr(sErr.empty() ? L"" : sErr.c_str());
    if (bOk)
    {
        sLog.Format(L"[clean] DismApiBridge CleanComponentStoreEx ok (%s%s) progress=%ld/%ld%s",
                    pszImg ? COfflineSession::GetImageRoot().GetData() : L"Online",
                    bResetBase ? L", ResetBase" : L"", nCur, nTot,
                    bCancelled ? L" [cancel requested]" : L"");
        return 0;
    }
    sLog.Format(L"[clean] DismApiBridge CleanComponentStoreEx failed err=%s progress=%ld/%ld%s",
                sErrStr.GetData(), nCur, nTot,
                bCancelled ? L" [cancel requested]" : L"");
    return 1;
}

[[nodiscard]] inline int RunOnlineComponentCleanup(bool bResetBase, CDuiString& sLog)
{
    return RunOnlineComponentCleanupEx(bResetBase, sLog);
}


// [E-3] KB §8.2：MSI/VS 安装事务持有 Global\_MSIExecute 互斥量。
// 两 TU 共用：Page_CleanupOps.cpp 的包缓存/安装器清理前先判忙；
// Page_Cleanup.cpp 侧的同名文件级实现已随之删除。
// 用 WinHandles 的 win::HandlePtr 自动关闭，不手写 CloseHandle。
[[nodiscard]] inline bool IsMsiExecuteBusy()
{
    win::HandlePtr h(::OpenMutexW(SYNCHRONIZE, FALSE, L"Global\\_MSIExecute"));
    return h != nullptr;
}

} // namespace cleanup_detail
