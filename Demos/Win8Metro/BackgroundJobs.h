#pragma once

// ============================================================
// BackgroundJobs.h —— 进程内后台作业统一收口
//
// 解决两类已取证问题（见《13_上帝文件源代码审计》G-CRS-01/02）：
//   1) 9 个裸 std::thread(...).detach() 只有 C++ catch，/EHsc 接不住
//      SEH 硬件异常（AV/堆损坏），异常逃逸直接终止进程。
//      BackgroundJobs::Start 一律走 SafeCreateThread 双层边界
//      （内层 C++ catch 保析构，外层 __except 兜底 SEH，留痕 crashguard.log）。
//   2) WM_CLOSE 不跟踪 in-process 作业：Apply 作业"已格式化卷、映像未释放完"
//      时退出会留下不可启动的目标卷。本设施登记全部作业句柄，
//      WM_CLOSE 经 WaitAll 有限时等待，仍未结束的危险作业需用户二次确认。
//
// 纯头文件、Win32 + CRT、无外部依赖。
// 约定：
//   - 上下文一律堆分配（new TJobCtx），线程过程正常/异常路径自行 delete；
//     线程过程签名为 DWORD WINAPI XxxThreadProc(void* pv)。
//   - 完成通知仍走各模块既有的 PostMessage 私有消息，本设施不改变消息契约。
//   - 句柄在作业结束后由 PurgeFinished 回收；Start 时顺带回收，长期运行不累积。
//   - [R104] 长任务应持有一个 CancelTokenPtr，并在进度回调里查 IsRequested()；
//     作业收到取消后须自行清理已写产物（半截 ISO 必须删掉），再以 bOk=false
//     回投完成信封。禁止用 TerminateThread 实现取消（见 CancelToken 注释）。
//   - [R16-T9] 作业完成 sink 归一：统一信封 TJobDoneEnvelope + 单一
//     CJobDoneSink（自 Page_Tools.cpp 迁入，CWecDoneSink/CIsoJobSink 已删除，
//     三个完成消息号收敛为 JOB_WM_DONE）。lParam 所有权随 PostMessage 移交
//     接收方，接收方在唯一出口 delete。
// ============================================================

#include "SafeThread.h"
#include "FrameWnd_Messages.h"   // [R16-T9] JOB_WM_DONE（CJobDoneSink 完成消息）
#include "WinHandles.h"          // [C++17 R3] win::HandlePtr for job-handle registry
#include <string>
#include <vector>
#include <memory>                // [C++17 R3] std::unique_ptr / std::make_unique
#include <atomic>                // [R104] CancelToken 的原子标志
// [R236] ShellExecuteW is used directly (see the job-result "open folder" path).
//   It was resolved only because some other translation unit happened to include
//   shellapi.h first; a header must be self-contained (include-what-you-use), and
//   scan_header_selfcontained.ps1 correctly flagged it.
#include <shellapi.h>

namespace BackgroundJobs {

// [C++17 R3] 分批等待上限（WaitForMultipleObjects 单批句柄数受 SDK 上限约束）
inline constexpr DWORD kMaxWaitBatch = MAXIMUM_WAIT_OBJECTS;

namespace Detail {

struct Entry
{
    win::HandlePtr hThread;   // [C++17 R3] RAII: handle auto-CloseHandle on entry erase
    const char* pszTag;    // 仅用于诊断/退出提示，不释放
};

struct Global
{
    CRITICAL_SECTION cs;
    std::vector<Entry> v;
    Global()  { ::InitializeCriticalSection(&cs); }
    ~Global() { ::DeleteCriticalSection(&cs); }
};

inline Global& G()
{
    static Global s_g;
    return s_g;
}

} // namespace Detail

// ============================================================
// [R104] 取消令牌：给长任务一个"能真的停下来"的入口。
//
// 背景（R103 审计发现）：工具箱原本**没有任何运行期中断机制**。
//   各处出现的"取消"全部是对话框的"取消按钮"（选文件时放弃），
//   一旦作业跑起来就只能等它结束 —— 用户点关闭，进度窗消失，
//   后台仍在往磁盘写 ISO/WIM。这不是"取消接口不统一"，是"根本没有取消"。
//
// 设计取舍：
//   · 用原子标志 + 协作式检查，不用 TerminateThread。
//     TerminateThread 会在任意指令处杀死线程：已分配的 WIMGAPI 句柄、
//     FILE 缓冲、临界区全部泄漏，甚至把进程堆留在锁内 —— 比不取消更糟。
//   · 令牌是 shared_ptr，作业上下文与 UI 各持一份；UI 释放不影响作业，
//     作业释放不影响 UI 判断状态，天然避免悬空。
//   · 作业在**进度回调里**检查（那里本来就是长循环的唯一出口），
//     检查成本是一次 relaxed load，可忽略。
// ============================================================
class CancelToken
{
public:
    CancelToken() : m_flag(0) {}

    // UI 线程调用：请求停止。可重复调用，幂等。
    void Request() { m_flag.store(1, std::memory_order_relaxed); }

    // 作业线程调用：是否已被请求停止。
    [[nodiscard]] bool IsRequested() const
    {
        return m_flag.load(std::memory_order_relaxed) != 0;
    }

private:
    std::atomic<int> m_flag;
};

using CancelTokenPtr = std::shared_ptr<CancelToken>;

inline CancelTokenPtr MakeCancelToken()
{
    return std::make_shared<CancelToken>();
}

// 回收已结束作业的句柄（WaitForSingleObject 零超时，不阻塞调用线程）。
// 每次启动新作业时自动调用一次；长期运行的进程不会累积僵尸句柄。
inline void PurgeFinished()
{
    Detail::Global& g = Detail::G();
    ::EnterCriticalSection(&g.cs);
    for (size_t i = 0; i < g.v.size(); )
    {
        if (::WaitForSingleObject(g.v[i].hThread.get(), 0) == WAIT_OBJECT_0)
        {
            g.v.erase(g.v.begin() + i);   // [C++17 R3] entry dtor closes handle (was explicit CloseHandle)
        }
        else
        {
            ++i;
        }
    }
    ::LeaveCriticalSection(&g.cs);
}

// 作业线程统一收尾：把 new 出来的结果对象 Post 给 UI 接收窗；
// 窗口已失效或投递失败时在工作线程就地 delete，杜绝泄漏/悬空。
// [GC-07] 收编原先 5 套作业模板各自手写的 IsWindow+Post+delete 重复尾巴，
// 全库作业只允许经本助手移交堆对象所有权。
template <typename T>
inline void PostHeapOrDelete(HWND hWnd, UINT uMsg, WPARAM wParam, T* pHeap)
{
    std::unique_ptr<T> guard(pHeap);   // [C++17 R3] take ownership first; release on successful post
    if (hWnd && ::IsWindow(hWnd)
        && ::PostMessageW(hWnd, uMsg, wParam, reinterpret_cast<LPARAM>(pHeap)))
    {
        guard.release();   // 所有权已移交 UI 线程，由接收方 delete
        return;
    }
    // [C++17 R3] post failed: guard frees (was explicit delete)
}

// 启动一个受崩溃边界保护的后台作业并登记句柄。
// 成功返回线程句柄（由本设施管理，调用方不要 CloseHandle）；失败返回 nullptr。
[[nodiscard]] inline HANDLE Start(PfnSafeThread pfn, void* pvCtx, const char* pszTag = "job")   // [C++17 R3] nodiscard：nullptr=启动失败
{
    PurgeFinished();
    HANDLE h = SafeCreateThread(pfn, pvCtx);
    if (h == nullptr)
        return nullptr;

    Detail::Global& g = Detail::G();
    ::EnterCriticalSection(&g.cs);
    g.v.push_back(Detail::Entry{ win::HandlePtr(h), pszTag });   // [C++17 R3] registry owns handle via RAII
    ::LeaveCriticalSection(&g.cs);
    return h;
}

// 当前仍在运行的作业数（先回收已结束句柄）。
[[nodiscard]] inline size_t ActiveCount()   // [C++17 R3] 返回值表示在途作业数，勿忽略
{
    PurgeFinished();
    Detail::Global& g = Detail::G();
    ::EnterCriticalSection(&g.cs);
    size_t n = g.v.size();
    ::LeaveCriticalSection(&g.cs);
    return n;
}

// [EXIT-DIAG] 当前仍在运行作业的标签快照（先回收已结束句柄）。
//   退出确认弹窗据此列出具体作业名（warmup/wof-tree/wim-apply…），
//   用户与排查者能直接看出是谁在拦退出，而不是一句无法核实的"N 个作业"。
//   pszTag 指向静态字面量（Start 调用点传入），进程生命周期内有效，可安全持有。
inline void GetActiveTags(std::vector<const char*>& vOut)
{
    PurgeFinished();
    Detail::Global& g = Detail::G();
    ::EnterCriticalSection(&g.cs);
    vOut.reserve(g.v.size());
    for (size_t i = 0; i < g.v.size(); ++i)
        vOut.push_back(g.v[i].pszTag);
    ::LeaveCriticalSection(&g.cs);
}

// 等待全部活动作业结束，最长 dwTimeoutMs 毫秒（分批，每批 <=64 句柄）。
// 返回 true = 全部结束；false = 超时仍有作业。返回 true 时句柄已全部回收。
inline bool WaitAll(DWORD dwTimeoutMs)
{
    PurgeFinished();

    Detail::Global& g = Detail::G();
    ::EnterCriticalSection(&g.cs);
    std::vector<HANDLE> vh;
    vh.reserve(g.v.size());
    for (size_t i = 0; i < g.v.size(); ++i)
        vh.push_back(g.v[i].hThread.get());
    ::LeaveCriticalSection(&g.cs);

    ULONGLONG ullDeadline = ::GetTickCount64() + dwTimeoutMs;
    bool bAllDone = true;

    for (size_t base = 0; base < vh.size(); base += kMaxWaitBatch)
    {
        DWORD n = (DWORD)((std::min)(static_cast<size_t>(kMaxWaitBatch), vh.size() - base));
        ULONGLONG ullNow = ::GetTickCount64();
        DWORD dwWait = (ullNow >= ullDeadline) ? 0 : static_cast<DWORD>(ullDeadline - ullNow);
        DWORD wr = ::WaitForMultipleObjects(
            n, &vh[base], TRUE, dwWait);
        if (wr == WAIT_TIMEOUT)
        {
            bAllDone = false;
            break;
        }
    }

    if (bAllDone)
        PurgeFinished();   // 全部 signaled：统一回收
    return bAllDone;
}

} // namespace BackgroundJobs

// ============================================================
// [R16-T9] 作业完成统一信封（最小公共集）
//   约定：作业完成消息的 lParam 一律为 new 的 TJobDoneEnvelope（或派生
//   结构体）；所有权随 PostMessage 移交接收方，接收方在唯一出口 delete。
//   异构业务字段一律留在派生结构（禁止塞进信封造成巨型结构）。
// ============================================================
struct TJobDoneEnvelope
{
    int          nKind;   // 作业类别（语义由各模块定义）
    bool         bOk;     // 主操作成功与否
    std::wstring sMsg;    // 结果文案（失败=错误信息；成功=可选覆盖，空=用默认）
    TJobDoneEnvelope() : nKind(0), bOk(false) {}
};

// ============================================================
// [R16-T9] 单一完成消息接收窗（自 Page_Tools.cpp CJobDoneSink 迁入）
//   原 CWecDoneSink / CIsoJobSink 已删除，其文案/打开目录行为全部由
//   Cfg 表达；完成消息统一 JOB_WM_DONE，lParam=TJobDoneEnvelope*。
//   释放点收敛：本 StaticProc 即 JOB_WM_DONE 的唯一 delete 出口。
// ============================================================
class CJobDoneSink
{
public:
    struct Cfg
    {
        std::wstring sOkTitle;
        std::wstring sOkMsg;
        std::wstring sFailTitle;
        std::wstring sFailMsg;   // [R16-T9] 失败兜底文案（空=沿用 sFailTitle）
        bool bAskOpen;           // 成功后 YESNO 询问打开目录
        Cfg() : bAskOpen(false) {}
    };

    static HWND Create(HWND hMain, const std::wstring& wOpenPath, const Cfg& cfg)
    {
        static const wchar_t* kCls = L"DismTool_JobDoneSink";
        WNDCLASSW wc = { 0 };
        wc.lpfnWndProc   = &CJobDoneSink::StaticProc;
        wc.hInstance     = ::GetModuleHandle(nullptr);
        wc.lpszClassName = kCls;
        ::RegisterClassW(&wc);
        HWND h = ::CreateWindowExW(0, kCls, L"", 0, 0, 0, 0, 0,
                                   HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        if (h)
        {
            ::SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)hMain);
            // [C++17 R3] ownership transferred to window properties; freed at JOB_WM_DONE exit below
            ::SetPropW(h, L"JobOpen", reinterpret_cast<HANDLE>(std::make_unique<std::wstring>(wOpenPath).release()));
            ::SetPropW(h, L"JobCfg",  reinterpret_cast<HANDLE>(std::make_unique<Cfg>(cfg).release()));
        }
        return h;
    }

private:
    static LRESULT CALLBACK StaticProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
    {
        if (m == JOB_WM_DONE)
        {
            HWND hMain = (HWND)::GetWindowLongPtrW(h, GWLP_USERDATA);
            std::wstring* pOpen = (std::wstring*)::GetPropW(h, L"JobOpen");
            Cfg* pCfg = (Cfg*)::GetPropW(h, L"JobCfg");
            // [C++17 R3] envelope ownership taken from PostMessage lParam; freed at scope exit
            std::unique_ptr<TJobDoneEnvelope> guardEnv(reinterpret_cast<TJobDoneEnvelope*>(lp));
            TJobDoneEnvelope* pEnv = guardEnv.get();

            if (pEnv && pEnv->bOk)
            {
                std::wstring sMsg = (pCfg ? pCfg->sOkMsg : L"");
                if (pEnv && !pEnv->sMsg.empty()) sMsg = pEnv->sMsg;
                std::wstring sTitle = pCfg ? pCfg->sOkTitle : L"";
                if (pCfg && pCfg->bAskOpen)
                {
                    if (::MessageBoxW(hMain, sMsg.c_str(), sTitle.c_str(),
                                      MB_YESNO | MB_ICONINFORMATION) == IDYES
                        && pOpen && !pOpen->empty())
                    {
                        std::wstring sDir = pOpen->substr(0,
                            pOpen->find_last_of(L"\\/") + 1);
                        ::ShellExecuteW(hMain, L"explore", sDir.c_str(),
                                        nullptr, nullptr, SW_SHOWNORMAL);
                    }
                }
                else
                    ::MessageBoxW(hMain, sMsg.c_str(), sTitle.c_str(),
                                  MB_OK | MB_ICONINFORMATION);
            }
            else
            {
                std::wstring sMsg;
                if (pEnv)
                    sMsg = pEnv->sMsg;
                else if (pCfg && !pCfg->sFailMsg.empty())
                    sMsg = pCfg->sFailMsg;
                else
                    sMsg = pCfg ? pCfg->sFailTitle : L"失败";
                std::wstring sTitle = pCfg ? pCfg->sFailTitle : L"";
                ::MessageBoxW(hMain, sMsg.c_str(), sTitle.c_str(),
                              MB_OK | MB_ICONERROR);
            }

            // [C++17 R3] guardEnv frees the envelope at scope exit (was the single delete here)
            if (pOpen) { ::RemovePropW(h, L"JobOpen"); std::unique_ptr<std::wstring> guard(pOpen); }   // [C++17 R3] RAII free
            if (pCfg)  { ::RemovePropW(h, L"JobCfg");  std::unique_ptr<Cfg> guard(pCfg); }              // [C++17 R3] RAII free
            ::DestroyWindow(h);
            return 0;
        }
        return ::DefWindowProcW(h, m, wp, lp);
    }
};
