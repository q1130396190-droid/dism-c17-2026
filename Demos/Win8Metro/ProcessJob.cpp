// ============================================================================
// ProcessJob.cpp - 见 ProcessJob.h 设计说明
// ============================================================================
#include "ProcessJob.h"
#include <utility>   // [C++17 R-P17] std::exchange
// [C++17 阶段2] 局部 RAII 守卫（见 WinHandles.h 使用规范）
struct PJJobGuard
{
    HANDLE h = nullptr;
    PJJobGuard() = default;
    explicit PJJobGuard(HANDLE x) : h(x) {}
    ~PJJobGuard() { if (h) ::CloseHandle(h); }
    PJJobGuard(const PJJobGuard&) = delete;
    PJJobGuard& operator=(const PJJobGuard&) = delete;
    operator HANDLE() const { return h; }
    bool valid() const { return h != nullptr; }
    HANDLE* out() { return &h; }
    // [C++17 R-P17] 放弃所有权且【不关闭】句柄，供“移交给进程级常驻变量”场景使用。
    //   此前用 reset() 移交会误关闭句柄，导致 g_hJob 悬垂（启动即退出的根因）。
    [[nodiscard]] HANDLE release() noexcept { return std::exchange(h, nullptr); }
    void reset() { if (h) ::CloseHandle(h); h = nullptr; }
};
struct PJProcGuard
{
    HANDLE h = nullptr;
    PJProcGuard() = default;
    explicit PJProcGuard(HANDLE x) : h(x) {}
    ~PJProcGuard() { if (h) ::CloseHandle(h); }
    PJProcGuard(const PJProcGuard&) = delete;
    PJProcGuard& operator=(const PJProcGuard&) = delete;
    operator HANDLE() const { return h; }
    bool valid() const { return h != nullptr; }
};


namespace ProcessJob {

namespace {

// 作业句柄：进程生命周期内常驻，刻意不关闭。
HANDLE g_hJob = nullptr;

// 单例化保证（EnsureCreated 幂等）。
volatile LONG g_lInitState = 0; // 0=未初始化 1=已尝试（成功或降级）

} // namespace

bool EnsureCreated()
{
    if (::InterlockedCompareExchange(&g_lInitState, 1, 0) != 0)
        return (g_hJob != nullptr);

    // 1) 创建匿名作业（不命名，防止其他进程 OpenJobObject 干扰）
    PJJobGuard hJob(::CreateJobObjectW(nullptr, nullptr));
    if (!hJob.valid())
        return false;

    // 2) 配置：
    //    KILL_ON_JOB_CLOSE - 作业句柄随进程终结关闭时，内核杀光作业内进程
    //    BREAKAWAY_OK      - 允许 CreateProcess(CREATE_BREAKAWAY_FROM_JOB)
    //                        让“崩溃重启接班进程”脱离本作业
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
    jeli.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
        JOB_OBJECT_LIMIT_BREAKAWAY_OK;

    if (!::SetInformationJobObject(hJob, JobObjectExtendedLimitInformation,
                                   &jeli, sizeof(jeli)))
    {
        // PJJobGuard 析构自动关闭
        return false;
    }

    // 3) 把主进程自身放入作业。
    //    Win8+ 支持嵌套作业，即使系统/宿主已将本进程放入其他作业也会成功；
    //    极少数受限环境失败时降级为无保护（不回归既有行为）。
    if (!::AssignProcessToJobObject(hJob, ::GetCurrentProcess()))
    {
        // PJJobGuard 析构自动关闭
        return false;
    }

    // [C++17 R-P17] 移交所有权：release() 放弃守卫所有权且不关闭句柄，g_hJob 常驻到进程结束
    g_hJob = hJob.release();
    return true;
}

HANDLE Handle()
{
    return g_hJob;
}

int KillChildren()
{
    HANDLE hJob = g_hJob;
    if (!hJob)
        return 0;

    const DWORD dwSelfPid = ::GetCurrentProcessId();
    int nKilledTotal = 0;

    // 多轮枚举：杀一批进程后其孙进程可能仍在派生/退出，PID 表会变化。
    for (int round = 0; round < 4; ++round)
    {
        // 缓冲约可容纳 1000 个 PID（结构含 2 个 DWORD 头 + ULONG_PTR 数组）
        constexpr size_t kJobPidListBufSize = 8192;
        BYTE abBuffer[kJobPidListBufSize] = {};
        JOBOBJECT_BASIC_PROCESS_ID_LIST* pPidList =
            reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(abBuffer);

        DWORD dwRet = 0;
        BOOL bOk = ::QueryInformationJobObject(
                       hJob, JobObjectBasicProcessIdList,
                       abBuffer, sizeof(abBuffer), &dwRet);
        if (!bOk)
            break;

        int nKilledRound = 0;
        const unsigned long nCount = pPidList->NumberOfProcessIdsInList;
        for (unsigned long i = 0; i < nCount; ++i)
        {
            DWORD dwPid = static_cast<DWORD>(static_cast<ULONG_PTR>(pPidList->ProcessIdList[i]));
            if (dwPid == 0 || dwPid == dwSelfPid)
                continue;

            PJProcGuard hProc(::OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE,
                                         FALSE, dwPid));
            if (hProc)
            {
                if (::TerminateProcess(hProc, 1))
                {
                    ++nKilledRound;
                    ++nKilledTotal;
                }
                // PJProcGuard 析构自动关闭
            }
        }

        if (nKilledRound == 0)
            break;                  // 作业内已无其他可杀进程

        ::Sleep(120);               // 等进程对象退出/PID 表刷新后再扫一轮
    }

    return nKilledTotal;
}

} // namespace ProcessJob
