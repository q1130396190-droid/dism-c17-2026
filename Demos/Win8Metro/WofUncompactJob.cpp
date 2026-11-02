// ============================================================================
//  WofUncompactJob.cpp —— [R16][T8] CompactOS 撤销作业迁入 BackgroundJobs 体系
//  线程体/进度回传/上下文结构自 FrameWnd_Menu.cpp 逐字搬迁
// （行为等价基线 2026-10-13）。
// ============================================================================

#include "stdafx.h"
#include "WofUncompactJob.h"
#include "WmMessages.h"   // WM_WOF_LOG
#include "WofCompact.h"
#include "BackgroundJobs.h"
#include <memory>                // [C++17 R3] std::unique_ptr / std::make_unique

namespace
{
    // [P0-A2] CompactOS 撤销进度回传上下文
    struct MenuWofCtx
    {
        HWND hWnd = nullptr;
    };

    void PostWofLog(HWND hWnd, const wchar_t* pszMsg)
    {
        if (!hWnd || !::IsWindow(hWnd)) return;
        // [C++17 R3] ownership transferred to UI on successful post; guard frees otherwise
        std::unique_ptr<CDuiString> pMsg = std::make_unique<CDuiString>(pszMsg);
        if (!::PostMessageW(hWnd, WM_WOF_LOG, 0, reinterpret_cast<LPARAM>(pMsg.get())))
            return;   // [C++17 R3] post failed, ownership not transferred; guard frees
        pMsg.release();
    }

    // [CRASHFIX] CompactOS 整卷撤销后台作业上下文
    struct TWofUncompactJob
    {
        HWND hWnd;
    };

    // [CRASHFIX] CompactOS 撤销线程：双层 SEH 边界，替代裸 detach
    DWORD WINAPI WofUncompactThreadProc(void* pv)
    {
        std::unique_ptr<TWofUncompactJob> guardJob(reinterpret_cast<TWofUncompactJob*>(pv));   // [C++17 R3] ctx ownership from BackgroundJobs::Start
        HWND hWnd = guardJob->hWnd;
        try
        {
            MenuWofCtx ctx;
            ctx.hWnd = hWnd;
            auto pfn = [](const WofCompact::Progress& st, void* p) -> bool
            {
                MenuWofCtx* c = (MenuWofCtx*)p;
                CDuiString s;
                s.Format(L"[clean] uncompact progress: scanned=%llu changed=%llu skipped=%llu failed=%llu",
                         st.nScanned, st.nChanged, st.nSkipped, st.nFailed);
                PostWofLog(c->hWnd, s.GetData());
                return true;
            };
            bool bOk = WofCompact::UncompactCompactOs(pfn, &ctx);
            PostWofLog(hWnd, bOk
                ? L"[clean] CompactOS uncompact done (native WOF)"
                : L"[clean] CompactOS uncompact failed or unsupported");
        }
        catch (...)
        {
            PostWofLog(hWnd, L"[clean] CompactOS 撤销任务异常中止");
        }
        // [C++17 R3] guardJob frees ctx at scope exit (was explicit delete here)
        return 0;
    }
}

namespace WofUncompactJob
{
    // [C++17 R3] [[nodiscard]]：返回值勿忽略
[[nodiscard]] int QueryState()
    {
        return WofCompact::QueryCompactOsState();
    }

    bool Start(HWND hWnd)
    {
        // [C++17 R3] thread ctx: ownership transferred to WofUncompactThreadProc on success; guard frees on failure
        std::unique_ptr<TWofUncompactJob> pJob = std::make_unique<TWofUncompactJob>();
        pJob->hWnd = hWnd;
        if (!BackgroundJobs::Start(&WofUncompactThreadProc, pJob.get(), "wof-uncompact"))
            return false;   // [C++17 R3] thread start failed, ctx not transferred; guard frees
        pJob.release();
        return true;
    }
}
