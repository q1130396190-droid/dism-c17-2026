// ============================================================
// RegCleanJobService.cpp —— [R111] 注册表清理页工作线程管线实现
//
// 线程契约：
//   * 启动前把 vSel / backup 完整拷贝进 Ctx，线程不再回读引擎外部状态；
//   * worker 逐规则调用 CRegCleanEngine::CleanRules（单规则切片，
//     引擎内部对未选中规则无副作用），完成后 PostMessage
//     WM_REGCLEN_DONE 并附 RegCleanDoneMsg*（UI 线程负责 delete）；
//   * 进度逐规则 PostMessage WM_REGCLEN_PROGRESS（lParam=int*）。
//
// 本文件不 include stdafx.h / UIlib.h / StringManager.h，不引用文案。
// ============================================================

#include <vector>
#include <memory>
#include <windows.h>

#ifndef UILIB_API
#define UILIB_API __declspec(dllimport)
#endif

#include "Utils/Utils.h"
#include "RegCleanJobService.h"
#include "RegCleanEngine.h"
#include "WmMessages.h"   // WM_REGCLEN_PROGRESS / WM_REGCLEN_DONE
#include "SafeThread.h"          // 工作线程顶层异常边界

namespace
{
    // ---- 全局状态（单实例进程内唯一 worker）----
    volatile LONG s_bRunning  = 0;
    HANDLE        s_hThread   = nullptr;

    // [FIX-P1C] 陈旧线程自愈（P1-11）：s_bRunning 单标志承担扫描/清理双
    // 语义，完成消息（SCANDONE/DONE）Post 丢失时标志永久残留，扫描/清理
    // 锁死到进程退出。线程已死（Wait 0 即返回）而标志未清 = 消息丢失，
    // 此时回收句柄并复位标志是安全的（上次作业已实际结束）。
    void SelfHealStaleThread()
    {
        HANDLE h = s_hThread;
        if (h && ::WaitForSingleObject(h, 0) == WAIT_OBJECT_0)
        {
            ::CloseHandle(h);
            s_hThread = nullptr;
            ::InterlockedExchange(&s_bRunning, 0);
        }
    }
    // [FIX-P1C] 完成消息有限重试：Post 失败（队列瞬时满）退避重试；
    // 重试仍失败返回 false，由调用方复位 s_bRunning（载荷 unique_ptr 自释放）。
    bool PostDoneWithRetry(HWND hNotify, UINT uMsg, void* pPayload)
    {
        for (int nRetry = 0; nRetry < 10; ++nRetry)
        {
            if (!::IsWindow(hNotify)) return false;
            if (::PostMessageW(hNotify, uMsg, 0, reinterpret_cast<LPARAM>(pPayload)))
                return true;
            ::Sleep(100);
        }
        return false;
    }

    struct CleanCtx
    {
        HWND             hNotify;
        CRegCleanEngine* pEng;                 // 引擎指针（线程读引擎内 m_rules）
        std::vector<int> vSel;                 // 规则索引快照
        RegCleanBackup   backup;               // 备份快照
    };

    // [R-TAILFIX 同型] 清理体两层拆分：Body 保留原 try/catch（含 C++ 对象），
    // Seh 层兜住引擎硬件异常 —— 否则 /EHsc 下 AV 穿透 catch(...)，线程死亡，
    // WM_REGCLEN_DONE 永不投递，UI 永久停在进度条（原注释设想的
    // "SafeThreadTrampoline 兜底"恰恰是不回传 DONE 的元凶）。
    struct RegCleanRunState
    {
        int nProcessed = 0;
        int nSucTotal  = 0;
        int nFailTotal = 0;
    };
    static void RegCleanCleanBody(CRegCleanEngine* pEng, const std::vector<int>& vSel,
                                  RegCleanBackup& backup, RegCleanDoneMsg* done,
                                  RegCleanRunState& st, HWND hNotify)
    {
        try
        {
            for (size_t si = 0; si < vSel.size(); ++si)
            {
                // 逐规则切片清理：只处理当前一条规则，失败计数累计
                std::vector<int> vOne(1, vSel[si]);
                std::vector<RegCleanExecResult> vOneRes;
                int nFailed = pEng->CleanRules(vOne, backup, vOneRes);

                // 合并结果
                for (size_t k = 0; k < vOneRes.size(); ++k)
                {
                    done->vResults.push_back(vOneRes[k]);
                    st.nSucTotal += vOneRes[k].nSucceeded;
                    st.nFailTotal += vOneRes[k].nFailed;
                }
                (void)nFailed;

                ++st.nProcessed;
                auto pP = std::make_unique<int>(st.nProcessed);   // [C++17 R3b] post payload, ownership to UI (lParam); self-free on Post failure
                auto pN = std::make_unique<int>(static_cast<int>(vSel.size()));   // [C++17 R3b] post payload, ownership to UI (wParam); self-free on Post failure
                // 进度回传：UI 侧 delete 两个 int
                if (::PostMessageW(hNotify, WM_REGCLEN_PROGRESS, reinterpret_cast<WPARAM>(pN.get()), reinterpret_cast<LPARAM>(pP.get())))
                {
                    pP.release(); pN.release();   // [C++17 R3b] ownership transferred to UI thread
                }
                // Post failed: unique_ptr frees both (ownership not transferred)
            }
        }
        catch (...)
        {
            // 异常中断：未处理条目计入失败，保证计数闭环
            st.nFailTotal += (static_cast<int>(vSel.size()) - st.nProcessed);
        }
    }
    // 本函数不含需要展开的 C++ 对象，允许直接使用 __try（MSVC C2712 约束）。
    static void RegCleanCleanSeh(CRegCleanEngine* pEng, const std::vector<int>& vSel,
                                 RegCleanBackup& backup, RegCleanDoneMsg* done,
                                 RegCleanRunState& st, HWND hNotify)
    {
        __try
        {
            RegCleanCleanBody(pEng, vSel, backup, done, st, hNotify);
        }
        __except (CrashGuardFilter(GetExceptionInformation(), "regclean-seh"))
        {
            st.nFailTotal += (static_cast<int>(vSel.size()) - st.nProcessed);
        }
    }

    DWORD WINAPI CleanThreadProc(void* pv)
    {
        std::unique_ptr<CleanCtx> c(static_cast<CleanCtx*>(pv));   // [C++17 R3b] ownership taken from creator
        if (!c) return 1;

        HWND hNotify = c->hNotify;
        CRegCleanEngine* pEng = c->pEng;
        std::vector<int> vSel = c->vSel;
        RegCleanBackup backup = c->backup;
        c.reset();   // [C++17 R3b] release ctx at original point (was: delete c)

        auto done = std::make_unique<RegCleanDoneMsg>();   // [C++17 R3b] ownership transferred to UI via Post; self-free on Post failure
        done->nSelected = static_cast<int>(vSel.size());
        done->sBackupFile = backup.sFile;

        // [R-TAILFIX 同型] 引擎缺失同样要回传 DONE（bOk=false 收口），
        // 原 return 2 会让 UI 永久停在"清理中"。
        if (pEng)
        {
            RegCleanRunState st;
            RegCleanCleanSeh(pEng, vSel, backup, done.get(), st, hNotify);
            done->nSucceeded = st.nSucTotal;
            done->nFailed = st.nFailTotal;
        }
        else
        {
            done->nFailed = static_cast<int>(vSel.size());
        }

        // [FIX-P1C] 完成消息有限重试：重试仍失败（窗口亡/队列持续满）时
        // 复位 s_bRunning，服务态不残留 —— 否则 7 个按钮锁死到进程退出。
        if (::IsWindow(hNotify))
        {
            if (PostDoneWithRetry(hNotify, WM_REGCLEN_DONE, done.get()))
                done.release();   // [C++17 R3b] ownership transferred to UI thread
            else
                ::InterlockedExchange(&s_bRunning, 0);
        }
        else
        {
            ::InterlockedExchange(&s_bRunning, 0);   // 窗口已亡：就地复位
        }

        return 0;
    }
}

[[nodiscard]] bool RegCleanJobService::IsRunning()
{
    return ::InterlockedCompareExchange(&s_bRunning, 0, 0) != 0;
}

[[nodiscard]] bool RegCleanJobService::HasThread()
{
    return s_hThread != nullptr;
}

// ============================================================
// [R-EXTASYNC] 注册表扫描工作线程
// ============================================================
namespace
{
    struct ScanCtx
    {
        HWND                               hNotify;
        CRegCleanEngine*                   pEng;
        std::vector<RegCleanScanResult>*   pvResults;
        RegCleanScanSummary*               pSummary;
    };

    // [R-TAILFIX 同型] 扫描体两层拆分：SEH 兜底后 WM_REGCLEN_SCANDONE
    // 必回投，UI 不再因引擎硬件异常永久停在"扫描中"。
    static void RegCleanScanBody(CRegCleanEngine* pEng,
                                 std::vector<RegCleanScanResult>* pvResults,
                                 RegCleanScanSummary* pSummary,
                                 RegCleanScanDoneMsg* pMsg)
    {
        try
        {
            if (pEng && pvResults && pSummary)
            {
                pEng->ScanRules(*pvResults, *pSummary);
                pMsg->bOk = true;
                pMsg->nRules = pSummary->nRules;
                pMsg->nApplicable = pSummary->nApplicable;
                pMsg->nCleanable = pSummary->nCleanable;
                pMsg->nCleanActions = pSummary->nCleanActions;
            }
        }
        catch (...)
        {
            // 与清理线程一致：异常不允许逃逸线程过程
            pMsg->bOk = false;
        }
    }
    // 本函数不含需要展开的 C++ 对象，允许直接使用 __try（MSVC C2712 约束）。
    static void RegCleanScanSeh(CRegCleanEngine* pEng,
                                std::vector<RegCleanScanResult>* pvResults,
                                RegCleanScanSummary* pSummary,
                                RegCleanScanDoneMsg* pMsg)
    {
        __try
        {
            RegCleanScanBody(pEng, pvResults, pSummary, pMsg);
        }
        __except (CrashGuardFilter(GetExceptionInformation(), "regclean-scan-seh"))
        {
            pMsg->bOk = false;
        }
    }

    DWORD WINAPI ScanThreadProc(void* pv)
    {
        std::unique_ptr<ScanCtx> c(static_cast<ScanCtx*>(pv));   // [C++17 R3b] ownership taken from creator
        if (!c) return 1;

        HWND hNotify = c->hNotify;
        CRegCleanEngine* pEng = c->pEng;
        std::vector<RegCleanScanResult>* pvResults = c->pvResults;
        RegCleanScanSummary* pSummary = c->pSummary;
        c.reset();   // [C++17 R3b] release ctx at original point (was: delete c)

        auto pMsg = std::make_unique<RegCleanScanDoneMsg>();   // [C++17 R3b] ownership transferred to UI via Post; self-free on Post failure
        pMsg->bOk = false;

        RegCleanScanSeh(pEng, pvResults, pSummary, pMsg.get());

        // [FIX-P1C] 完成消息有限重试：重试仍失败时复位 s_bRunning，
        // 扫描态不残留（否则重新扫描/清理被永久拒绝）。
        if (::IsWindow(hNotify))
        {
            if (PostDoneWithRetry(hNotify, WM_REGCLEN_SCANDONE, pMsg.get()))
                pMsg.release();   // [C++17 R3b] ownership transferred to UI thread
            else
                ::InterlockedExchange(&s_bRunning, 0);
        }
        else
        {
            ::InterlockedExchange(&s_bRunning, 0);   // 窗口已亡：就地复位
        }
        return 0;
    }
}

bool RegCleanJobService::LaunchScan(HWND hNotify, CRegCleanEngine* pEng,
                                    std::vector<RegCleanScanResult>* pvResults,
                                    RegCleanScanSummary* pSummary)
{
    if (!pEng || !pvResults || !pSummary) return false;

    // [FIX-P1C] 先自愈陈旧线程：上次完成消息丢失时回收句柄/复位标志，
    // 避免下方 s_hThread 覆盖泄漏句柄、s_bRunning 永久锁死。
    SelfHealStaleThread();

    auto ctx = std::make_unique<ScanCtx>();   // [C++17 R3b] thread ctx: passed across thread, taken by thread proc; reclaimed here on launch failure
    ctx->hNotify = hNotify;
    ctx->pEng = pEng;
    ctx->pvResults = pvResults;
    ctx->pSummary = pSummary;

    ::InterlockedExchange(&s_bRunning, 1);

    HANDLE h = SafeCreateThread(ScanThreadProc, ctx.get());
    if (!h)
    {
        // [C++17 R3b] launch failed, ctx not transferred; unique_ptr reclaims
        ::InterlockedExchange(&s_bRunning, 0);
        return false;
    }
    ctx.release();   // [C++17 R3b] ownership transferred to thread proc
    s_hThread = h;
    return true;
}

void RegCleanJobService::EndScan()
{
    ::InterlockedExchange(&s_bRunning, 0);
}

bool RegCleanJobService::LaunchClean(HWND hNotify, CRegCleanEngine* pEng,
                                     const std::vector<int>& vSel,
                                     const RegCleanBackup& backup)
{
    // [FIX-P1C] 先自愈陈旧线程（同 LaunchScan）
    SelfHealStaleThread();

    auto ctx = std::make_unique<CleanCtx>();   // [C++17 R3b] thread ctx: passed across thread, taken by thread proc; reclaimed here on launch failure
    ctx->hNotify = hNotify;
    ctx->pEng = pEng;
    ctx->vSel = vSel;
    ctx->backup = backup;

    ::InterlockedExchange(&s_bRunning, 1);

    HANDLE h = SafeCreateThread(CleanThreadProc, ctx.get());
    if (!h)
    {
        // [C++17 R3b] launch failed, ctx not transferred; unique_ptr reclaims
        ::InterlockedExchange(&s_bRunning, 0);
        return false;
    }
    ctx.release();   // [C++17 R3b] ownership transferred to thread proc
    s_hThread = h;
    return true;
}

void RegCleanJobService::EndClean()
{
    ::InterlockedExchange(&s_bRunning, 0);
}

void RegCleanJobService::DetachThread()
{
    if (s_hThread)
    {
        ::CloseHandle(s_hThread);
   // [C++17 阶段2] 类级工作线程句柄：跨函数分离，显式关闭
        s_hThread = nullptr;
    }
}

void RegCleanJobService::Join(DWORD dwTimeoutMs)
{
    if (s_hThread)
    {
        // [EXIT-FIX] 限时等待，超时放弃，杜绝 INFINITE 等待导致的退出僵尸
        ::WaitForSingleObject(s_hThread, dwTimeoutMs);
        ::CloseHandle(s_hThread);
   // [C++17 阶段2] 类级工作线程句柄：跨函数分离，显式关闭
        s_hThread = nullptr;
    }
    ::InterlockedExchange(&s_bRunning, 0);
}