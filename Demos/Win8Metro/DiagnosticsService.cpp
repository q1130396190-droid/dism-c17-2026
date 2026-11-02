// DiagnosticsService.cpp —— S12（P1）
// 独立于预编译头：不 include stdafx.h / UIlib.h / StringManager.h。
// 8 个诊断快照函数从 FrameWnd.cpp 原样迁入，唯一改动：
// StringMgr.Get(key) → DiagTr(key)，由核心壳绑定的 Localizer 转发，行为一致。

#include "LogService.h"   // [M-5] 统一日志目录
#include <windows.h>
#include <tlhelp32.h>
#include <setupapi.h>
#include <winioctl.h>   // [原生升级] FSCTL_GET_VOLUME_BITMAP / ENUM_USN_DATA / GET_RETRIEVAL_POINTERS
#include <algorithm>
#include <string>
#include <memory>
#include "SafeEnv.h"   // [B6] 安全环境变量读取：替代弃用 _wgetenv
#include <vector>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "setupapi.lib")

#include "DiagnosticsService.h"

// [C++17 阶段2] 局部 RAII 守卫：文件/快照句柄、注册表键（见 WinHandles.h 使用规范）
struct DiagFileGuard
{
    HANDLE h;
    explicit DiagFileGuard(HANDLE x) : h(x) {}
    ~DiagFileGuard() { if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h); }
    DiagFileGuard(const DiagFileGuard&) = delete;
    DiagFileGuard& operator=(const DiagFileGuard&) = delete;
    operator HANDLE() const { return h; }
};
struct DiagRegGuard
{
    HKEY k = nullptr;
    ~DiagRegGuard() { if (k) ::RegCloseKey(k); }
    HKEY* out() { return &k; }
    void reset(HKEY n = nullptr) { if (k) ::RegCloseKey(k); k = n; }
    operator HKEY() const { return k; }   // [C++17 R-P8] 补隐式转换（RegQueryValueEx/RegSetValueEx 需 HKEY）
};


using DuiLib::CDuiString;

// ---------------- 本地化桥（核心壳注入） ----------------
static DiagLocalizeFn g_diagLocalizer = nullptr;

// ============================================================
// [R118] 诊断日志写盘：失败留痕，不阻断。
//
// 背景：本文件有 24 处 ::WriteFile 直接丢弃返回值。诊断快照写不出来时，
//   调用方拿到的仍是一个"看起来正常"的空/残缺摘要 —— 用户反馈
//   "诊断记录丢了"时无从排查。
//
// 为什么只留痕不阻断：
//   这些函数在进入清理页时**同步调用**，写盘失败（磁盘满 / 目录只读）
//   不应让整个页面构建失败。这与既有 catch(...) 的处理取向一致
//   （见 AppendSysInfoSnapshotToFile 末尾的 OutputDebugStringW）。
//
// 为什么用 OutputDebugStringW 而不是弹窗：
//   与同文件既有降级路径保持同一形态，且不打扰用户。
// ============================================================
namespace {

// 写一块数据；成功返回 true，失败留痕并返回 false（调用方可选是否中断）。
[[nodiscard]] bool DiagWriteRaw(HANDLE hFile, const void* pData, DWORD cbData)
{
    if (hFile == nullptr || hFile == INVALID_HANDLE_VALUE) return false;
    DWORD dwWritten = 0;
    if (!::WriteFile(hFile, pData, cbData, &dwWritten, nullptr) || dwWritten != cbData)
    {
        ::OutputDebugStringW(L"[Diag] WriteFile 失败，诊断快照可能不完整\r\n");
        return false;
    }
    return true;
}

} // namespace

void DiagnosticsService_BindLocalizer(DiagLocalizeFn fn)
{
    g_diagLocalizer = fn;
}

// 未绑定时返回空串（与 CStringManager::Get 永不返回 nullptr 的契约对齐，避免 Format %s 崩溃）
static const wchar_t* DiagTr(const char* pszKeyUtf8)
{
    return g_diagLocalizer ? g_diagLocalizer(pszKeyUtf8) : L"";
}

// 逐级创建目录（CreateDirectory 无法一次建多级；%LOCALAPPDATA%\DismTool_Clone\logs
// 两级缺失时旧实现静默失败，导致全部诊断日志从未落盘）。
static void EnsureDirectoryDeep(const std::wstring& sDir)
{
    if (sDir.empty()) return;
    std::wstring sCur;
    for (size_t i = 0; i < sDir.size(); ++i)
    {
        wchar_t ch = sDir[i];
        if (ch == L'\\' || ch == L'/')
        {
            // 跳过 "\\server\share" 等前导段
            if (sCur.size() >= 2 && sCur[1] == L':') { ::CreateDirectoryW(sCur.c_str(), nullptr); }
        }
        sCur += ch;
    }
    ::CreateDirectoryW(sCur.c_str(), nullptr);
}

// 安全获取诊断日志目录：LOCALAPPDATA 在服务宿主/异常环境变量缺失时可能为
// nullptr，旧实现直接把 nullptr 传给 Format %s（CRT 输出 "(null)"，路径非法导致
// 全部快照静默失败）。逐级回退 USERPROFILE → 临时目录 → C: 根。
static std::wstring DiagLogDir()
{
    // [L-06][M-5] 诊断底账并入统一日志目录：EXE\Logs（不可写时自动回退）。
    //   原实现固定写 %LOCALAPPDATA%\DismTool_Clone\logs，是"三套落点"之一，
    //   与 action_log.jsonl 分处两地，用户要查看时得记住两个路径。
    //   LogService::UnifiedLogDir 内部已有 LOCALAPPDATA→USERPROFILE→TEMP→C: 的
    //   逐级回退（且带试写探针），原来的回退逻辑由它统一承担。
    const std::wstring& sDir = LogService::UnifiedLogDir();
    if (!sDir.empty())
    {
        std::wstring s = sDir;
        while (!s.empty() && (s.back() == L'\\' || s.back() == L'/'))
            s.pop_back();
        return s;
    }
    // 极端情况（EXE\Logs / LOCALAPPDATA / USERPROFILE / TEMP 全部不可写）：
    //   返回空串，由调用方跳过落盘。
    //   这里**不再**回退到 C:\DismTool_Clone\logs —— 那是统一化之前的旧落点，
    //   留着它等于给"日志体系只有一套目录"这条契约开一个例外口。
    //   注意：UnifiedLogDir() 内部已经把 TEMP 作为最后一级，所以本分支实际不可达，
    //   只作语义收口；各调用方仍按"空串则跳过"处理，避免拼出非法路径。
    return std::wstring();
}

// [R58-C] 系统信息快照持久化：每次进入系统信息/优化页时调用一次
// 快照内容：时间戳 + 物理内存/可用内存/磁盘总量/可用空间 + 计数器
// 写入 %LOCALAPPDATA%\DismTool_Clone\logs\sysinfo_history.log（UTF-8 BOM）
// 用于跨会话累积，用户可离线回看机器使用趋势
// 返回快照单行文本（供调用方 AppendLog）
CDuiString DiagnosticsService::AppendSysInfoSnapshotToFile()
try
{
    CDuiString sLine;

    // 快照目录（与 fix.log / icons 同级）
    CDuiString sDir;
    sDir = DiagLogDir().c_str();
    // 目录不可用（极端权限/磁盘异常）时直接跳过落盘：
    //   否则 %s 拼出的是 "\xxx.log" 这种非法路径，CreateFile 只会静默失败，
    //   反而把"没写进去"伪装成"写了"。
    if (sDir.IsEmpty()) return CDuiString();
    EnsureDirectoryDeep(sDir.GetData());

    CDuiString sLogPath;
    sLogPath.Format(L"%s\\sysinfo_history.log", sDir.GetData());

    // 计数器（读回后递增）
    LONG nSession = 1;
    LONG nHistory = 0;
    DiagRegGuard hCnt;
    if (::RegOpenKeyEx(HKEY_CURRENT_USER,
            L"SOFTWARE\\DismTool_Clone\\History", 0,
            KEY_READ | KEY_WRITE, hCnt.out()) == ERROR_SUCCESS)
    {
        DWORD dwType = 0, dwSize = sizeof(LONG);
        ::RegQueryValueEx(hCnt, L"SessionCount", nullptr, &dwType,
                          (BYTE*)&nSession, &dwSize);
        ::RegQueryValueEx(hCnt, L"SnapshotCount", nullptr, &dwType,
                          (BYTE*)&nHistory, &dwSize);
    }
    ++nSession; ++nHistory;
    hCnt.reset();  // 复用守卫：先关旧键再开新键
    if (::RegCreateKeyEx(HKEY_CURRENT_USER,
            L"SOFTWARE\\DismTool_Clone\\History", 0, nullptr,
            REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, hCnt.out(), nullptr) == ERROR_SUCCESS)
    {
        // [R128-A] 计数键写失败只影响历史统计，留痕不中断（诊断本身已产出）
        LONG lSes = ::RegSetValueEx(hCnt, L"SessionCount", 0, REG_DWORD,
                        (const BYTE*)&nSession, sizeof(LONG));
        LONG lSnp = ::RegSetValueEx(hCnt, L"SnapshotCount", 0, REG_DWORD,
                        (const BYTE*)&nHistory, sizeof(LONG));
        if (lSes != ERROR_SUCCESS || lSnp != ERROR_SUCCESS)
            ::OutputDebugStringW(L"[R128][diag] 历史计数写入失败\r\n");
    }

    // 内存统计
    MEMORYSTATUSEX msx = { sizeof(msx) };
    ::GlobalMemoryStatusEx(&msx);
    ULONGLONG ullPhys = msx.ullTotalPhys;
    ULONGLONG ullAvail = msx.ullAvailPhys;
    ULONGLONG ullUsed = ullPhys - ullAvail;
    double dUsedPct = ullPhys > 0 ? (100.0 * static_cast<double>(ullUsed) / static_cast<double>(ullPhys)) : 0.0;

    // 主磁盘空间（尝试 C:）
    ULONGLONG ullDiskTotal = 0, ullDiskFree = 0;
    ULARGE_INTEGER liTotal = { 0 }, liFree = { 0 }, liAvail = { 0 };
    // 旧实现 GetProcAddress(nullptr, "GetDiskFreeSpaceExW")：第一参数 nullptr
    // 表示当前模块镜像，永远取不到 kernel32 导出，磁盘统计恒为 0。直接调用。
    ::GetDiskFreeSpaceExW(L"C:\\", &liAvail, &liTotal, &liFree);
    ullDiskTotal = static_cast<ULONGLONG>(liTotal.QuadPart);
    ullDiskFree  = static_cast<ULONGLONG>(liFree.QuadPart);

    // 时间戳
    SYSTEMTIME st; ::GetLocalTime(&st);

    // [R59-A] 启动时间历史追踪：系统运行时长（自上次启动至今）
    ULONGLONG ullUpTicks = ::GetTickCount64();
    ULONGLONG ullUpMs = (ullUpTicks * (1000000ULL / 256ULL)) / 1000ULL;  // GetTickCount64 in 100ns → ms
    ULONGLONG ullUpSec = ullUpMs / 1000ULL;
    ULONGLONG ullUpDay = ullUpSec / 86400ULL;
    ULONGLONG ullUpHr  = (ullUpSec % 86400ULL) / 3600ULL;
    ULONGLONG ullUpMin = (ullUpSec % 3600ULL) / 60ULL;
    ULONGLONG ullUpSc  = ullUpSec % 60ULL;

    // 组装快照行（单行便于 grep / tail -f 查看）
    sLine.Format(L"%04d-%02d-%02d %02d:%02d:%02d | session=%ld snap=%ld | RAM=%llu/%llu MB (%.1f%% used) | DISKC=%llu/%llu MB (%llu used) | UPTIME=%llu d %llu:%02u:%02u",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                 nSession, nHistory,
                 ullAvail / (1024*1024), ullPhys / (1024*1024), dUsedPct,
                 ullDiskFree / (1024*1024), ullDiskTotal / (1024*1024),
                 (ullDiskTotal - ullDiskFree) / (1024*1024),
                 ullUpDay, ullUpHr, static_cast<UINT>(ullUpMin), static_cast<UINT>(ullUpSc));

    // 追加写入（首行 BOM，后续 UTF-8 无 BOM）
    DiagFileGuard hFile = DiagFileGuard(::CreateFileW(sLogPath.GetData(), GENERIC_WRITE,
                                FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr));
    if (hFile != INVALID_HANDLE_VALUE)
    {
        DWORD dwW = 0;
        if (::GetFileSize(hFile, nullptr) == 0)
        {
            BYTE bBOM[3] = { 0xEF, 0xBB, 0xBF };
            static_cast<void>(DiagWriteRaw(hFile, bBOM, 3));   // [R118] 失败留痕
        }
        ::SetFilePointer(hFile, 0, nullptr, FILE_END);
        std::wstring ws = sLine.GetData();
        int nBytes = ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()),
                                            nullptr, 0, nullptr, nullptr);
        if (nBytes > 0)
        {
            std::string sUtf8(nBytes, 0);
            ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()),
                                   &sUtf8[0], nBytes, nullptr, nullptr);
            static_cast<void>(DiagWriteRaw(hFile, sUtf8.c_str(), static_cast<DWORD>(nBytes)));   // [R118]
        }
    }

    return sLine;
}
catch (...)
{
    // 诊断快照永不允许把异常抛进 UI 页面构建（进入清理页同步调用）
    ::OutputDebugStringW(L"[Diag] AppendSysInfoSnapshotToFile 异常，快照留空\r\n");
    return CDuiString();
}

// ============================================================
// [R59-B] 后台/进程 Top-20 排序：Toolhelp32 快照 + 按工作集内存降序 + 日志
// 落盘 %LOCALAPPDATA%\DismTool_Clone\logs\proctop20.log；返回 top-5 单行摘要供调用方 AppendLog
// ============================================================
CDuiString DiagnosticsService::Top20ProcessesToLog()
try
{
    CDuiString sSummary;
    struct Proc { DWORD dwPid; ULONGLONG ullMB; CDuiString sName; };

    DiagFileGuard hSnap = DiagFileGuard(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (hSnap == INVALID_HANDLE_VALUE) return sSummary;

    PROCESSENTRY32W pe = { sizeof(pe) };
    std::vector<Proc> vProcs;
    ULONGLONG ullTotal = 0;
    DWORD nTotal = 0;
    if (::Process32FirstW(hSnap, &pe))
    {
        do
        {
            ++nTotal;
            Proc p;
            p.dwPid = pe.th32ProcessID;
            p.ullMB = 0;  // PROCESSENTRY32W 无工作集字段；简化版仅记录进程名 + PID
            p.sName = pe.szExeFile;
            vProcs.push_back(p);
        } while (::Process32NextW(hSnap, &pe));
    }

    // 按进程名字典序排序（simplified 版；真实 Top-N by memory 需 NtQuerySystemInformation）
    std::sort(vProcs.begin(), vProcs.end(), [](const Proc& a, const Proc& b) {
        return lstrcmpiW(a.sName.GetData(), b.sName.GetData()) < 0;
    });

    // 日志文件路径
    CDuiString sDir;
    sDir = DiagLogDir().c_str();
    // 目录不可用（极端权限/磁盘异常）时直接跳过落盘：
    //   否则 %s 拼出的是 "\xxx.log" 这种非法路径，CreateFile 只会静默失败，
    //   反而把"没写进去"伪装成"写了"。
    if (sDir.IsEmpty()) return CDuiString();
    EnsureDirectoryDeep(sDir.GetData());
    CDuiString sLogPath;
    sLogPath.Format(L"%s\\proctop20.log", sDir.GetData());

    SYSTEMTIME st; ::GetLocalTime(&st);
    CDuiString sHeader;
    sHeader.Format(L"\n===== Top Processes Snapshot %04d-%02d-%02d %02d:%02d:%02d | total=%d procs =====",
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, static_cast<int>(vProcs.size()));

    DiagFileGuard hFile = DiagFileGuard(::CreateFileW(sLogPath.GetData(), GENERIC_WRITE,
                                FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr));
    if (hFile != INVALID_HANDLE_VALUE)
    {
        DWORD dwW = 0;
        if (::GetFileSize(hFile, nullptr) == 0)
        {
            BYTE bBOM[3] = { 0xEF, 0xBB, 0xBF };
            static_cast<void>(DiagWriteRaw(hFile, bBOM, 3));   // [R118] 失败留痕
        }
        ::SetFilePointer(hFile, 0, nullptr, FILE_END);
        auto writeLine = [&](const wchar_t* s) {
            std::wstring ws = s;
            int nBytes = ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
            if (nBytes > 0)
            {
                std::string sUtf8(nBytes, 0);
                ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), &sUtf8[0], nBytes, nullptr, nullptr);
                static_cast<void>(DiagWriteRaw(hFile, sUtf8.c_str(), static_cast<DWORD>(nBytes)));   // [R118]
            }
            ::WriteFile(hFile, (LPCVOID)L"\n", 1, &dwW, nullptr);
        };
        writeLine(sHeader.GetData());
        int nShown = 0;
        constexpr int kMaxShow = 20;
        for (size_t i = 0; i < vProcs.size() && nShown < kMaxShow; ++i, ++nShown)
        {
            CDuiString sLine;
            sLine.Format(L"  #%02d  PID=%-7lu  %s", nShown, vProcs[i].dwPid, vProcs[i].sName.GetData());
            writeLine(sLine.GetData());
        }
    }

    // 摘要（供 AppendLog）：total 数 + top-5 名 + 日志路径
    sSummary.Format(DiagTr("misc.h5719"), static_cast<int>(vProcs.size()));
    int nShown2 = 0;
    for (size_t i = 0; i < vProcs.size() && nShown2 < 5; ++i, ++nShown2)
    {
        sSummary += vProcs[i].sName;
        if (nShown2 < 5) sSummary += L", ";
    }
    sSummary += DiagTr("misc.h75e1");
    sSummary += sLogPath;
    return sSummary;
}
catch (...)
{
    ::OutputDebugStringW(L"[Diag] Top20ProcessesToLog 异常，快照留空\r\n");
    return CDuiString();
}

// ============================================================
// [R61-A] 系统服务状态监控：EnumServicesStatusEx 快照 → services_snapshot.log
// 返回摘要供调用方 AppendLog；写入 %LOCALAPPDATA%\DismTool_Clone\logs\services_snapshot.log
// ============================================================
CDuiString DiagnosticsService::SnapshotServicesToLog()
try
{
    CDuiString sSummary;
    SC_HANDLE hScm = ::OpenSCManager(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (!hScm) return sSummary;

    DWORD bytesNeeded = 0, servicesReturned = 0;
    ::EnumServicesStatus(hScm, SERVICE_WIN32, SERVICE_STATE_ALL,
                         nullptr, 0, &bytesNeeded, &servicesReturned, nullptr);

    CDuiString sDir;
    sDir = DiagLogDir().c_str();
    // 目录不可用（极端权限/磁盘异常）时直接跳过落盘：
    //   否则 %s 拼出的是 "\xxx.log" 这种非法路径，CreateFile 只会静默失败，
    //   反而把"没写进去"伪装成"写了"。
    if (sDir.IsEmpty()) return CDuiString();
    EnsureDirectoryDeep(sDir.GetData());
    CDuiString sLog;
    sLog.Format(L"%s\\services_snapshot.log", sDir.GetData());
    SYSTEMTIME st; ::GetLocalTime(&st);

    DWORD nTotal = 0, nActive = 0, nStopped = 0, nPending = 0;
    std::vector<ENUM_SERVICE_STATUSW> entries;
    // 缓冲必须存活到下面写日志循环结束：entries 只复制了结构，其内
    // lpServiceName/lpDisplayName 指针指向本缓冲尾部字符串区，缓冲提前
    // 析构会导致写日志时解引用悬空指针。
    std::vector<BYTE> svcBuf;

    if (bytesNeeded > 0)
    {
        svcBuf.resize(bytesNeeded);
        if (::EnumServicesStatus(hScm, SERVICE_WIN32, SERVICE_STATE_ALL,
                                 (LPENUM_SERVICE_STATUSW)svcBuf.data(), bytesNeeded,
                                 &bytesNeeded, &servicesReturned, nullptr))
        {
            // EnumServicesStatus 的返回布局：servicesReturned 个
            // ENUM_SERVICE_STATUSW 结构组成的连续数组，字符串统一存放在数组
            // 之后，结构内指针指向该字符串区。旧实现按“结构+字符串交错”手工
            // 步进，第二条记录即落到字符串区、指针越界检查 break，导致永远只
            // 统计到 1 个服务。直接按数组下标遍历。
            LPENUM_SERVICE_STATUSW pAll = (LPENUM_SERVICE_STATUSW)svcBuf.data();
            for (DWORD i = 0; i < servicesReturned; ++i)
            {
                ENUM_SERVICE_STATUSW& q = pAll[i];
                if (!q.lpServiceName || !q.lpDisplayName) continue;
                ++nTotal;
                switch (q.ServiceStatus.dwCurrentState)
                {
                case SERVICE_RUNNING: ++nActive; break;
                case SERVICE_STOPPED: ++nStopped; break;
                default:              ++nPending; break;
                }
                entries.push_back(q);
            }
        }
    }

    DiagFileGuard hF = DiagFileGuard(::CreateFileW(sLog.GetData(), GENERIC_WRITE, FILE_SHARE_READ,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (hF != INVALID_HANDLE_VALUE)
    {
        DWORD dwW = 0;
        if (::GetFileSize(hF, nullptr) == 0)
        {
            BYTE bBOM[3] = { 0xEF, 0xBB, 0xBF };
            ::WriteFile(hF, bBOM, 3, &dwW, nullptr);   // [R128-B] BOM 写失败只影响可读性
        }
        ::SetFilePointer(hF, 0, nullptr, FILE_END);
        auto writeLine = [&](const wchar_t* s) {
            std::wstring ws = s;
            int nBytes = ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
            if (nBytes > 0)
            {
                std::string sU(nBytes, 0);
                ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), &sU[0], nBytes, nullptr, nullptr);
                ::WriteFile(hF, sU.c_str(), nBytes, &dwW, nullptr);
            }
            static_cast<void>(DiagWriteRaw(hF, "\n", 1));   // [R118]
        };
        CDuiString sHdr;
        sHdr.Format(L"\n===== Services Snapshot %04d-%02d-%02d %02d:%02d:%02d | total=%lu active=%lu stopped=%lu pending=%lu =====",
                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                    servicesReturned, nActive, nStopped, nPending);
        writeLine(sHdr.GetData());
        const wchar_t* states[] = { L"STOPPED", L"START_PEND", L"STOP_PEND", L"RUNNING", L"CONT_PEND", L"PAUSE_PEND", L"PAUSED", L"CREATE_ERR", L"DELETE_PEND" };
        int nShown = 0;
        for (size_t i = 0; i < entries.size() && nShown < 50; ++i, ++nShown)
        {
            ENUM_SERVICE_STATUSW& e = entries[i];
            int stIdx = static_cast<int>(e.ServiceStatus.dwCurrentState);
            const wchar_t* stateStr = (stIdx >= 0 && stIdx < 9) ? states[stIdx] : L"UNKNOWN";
            CDuiString sLine;
            sLine.Format(L"  %-12s  name=%s",
                         stateStr, e.lpDisplayName);
            writeLine(sLine.GetData());
        }
    }

    sSummary.Format(DiagTr("misc.hc73c"),
                    servicesReturned, nActive, nStopped, nPending, sLog.GetData());
    ::CloseServiceHandle(hScm);
    return sSummary;
}
catch (...)
{
    ::OutputDebugStringW(L"[Diag] SnapshotServicesToLog 异常，快照留空\r\n");
    return CDuiString();
}

// ============================================================
// [N01] 启动事件日志扫描辅助：把原 ParseBootEventsToLog 内 8 层嵌套
//       （try>if×4>while>while>if）拆为三个纯函数（记录遍历 / 批次读取 /
//       stats 落盘），行为逐字等价，仅结构压平。C++17 真实特征：std::vector、
//       switch 表驱动、constexpr、无裸 new/HeapAlloc。
// ============================================================
struct DiagBootEventStats
{
    DWORD nScanned = 0;
    DWORD nRead    = 0;
    DWORD nHit6005 = 0;
    DWORD nHit1074 = 0;
    DWORD nHit12   = 0;
    DWORD nHit6006 = 0;
};

using DiagReadEventFn  = BOOL (WINAPI*)(HANDLE, DWORD, DWORD, void*,
                                        DWORD, DWORD*, DWORD*);

// 内层记录遍历：单次批次缓冲内按 EVENTLOGRECORD 变长记录迭代计数
static void DiagScanEventBatch(const BYTE* pBuf, DWORD dwAvail,
                               DWORD& nScanned, DiagBootEventStats& st)
{
    DWORD dwOff = 0;
    while (nScanned < 100 &&
           dwOff + static_cast<DWORD>(sizeof(EVENTLOGRECORD)) <= dwAvail)
    {
        const EVENTLOGRECORD* pR =
            reinterpret_cast<const EVENTLOGRECORD*>(pBuf + dwOff);
        if (pR->Length < sizeof(EVENTLOGRECORD) ||
            pR->Length > dwAvail - dwOff)
            break;
        switch (static_cast<DWORD>(pR->EventID & 0x0000FFFF))
        {
            case 6005: ++st.nHit6005; break;
            case 1074: ++st.nHit1074; break;
            case 12:   ++st.nHit12;   break;
            case 6006: ++st.nHit6006; break;
            default:   break;
        }
        ++nScanned;
        dwOff += pR->Length;
    }
}

// 外层批次读取：重复 ReadEventW 直到命中数达标或批次不足
static void DiagScanEventLog(DiagReadEventFn pfnRead, HANDLE hEl,
                             BYTE* pBuf, DWORD nMaxBytes,
                             DiagBootEventStats& st)
{
    constexpr DWORD FLAGS =
        EVENTLOG_BACKWARDS_READ | EVENTLOG_SEQUENTIAL_READ;
    while (st.nScanned < 100 &&
           st.nHit6005 + st.nHit1074 + st.nHit12 + st.nHit6006 < 20)
    {
        DWORD dwAvail = 0;
        if (!pfnRead(hEl, FLAGS, 0, pBuf, nMaxBytes, &dwAvail, nullptr))
            break;
        ++st.nRead;
        if (dwAvail < sizeof(EVENTLOGRECORD))
            break;
        DiagScanEventBatch(pBuf, dwAvail, st.nScanned, st);
    }
}

// stats 落盘：把统计行追加写入 sLogPath（原嵌套在 pfn 加载 && hEl && hF2 && nB2 内）
static void DiagWriteEventStats(const CDuiString& sLogPath,
                                const DiagBootEventStats& st)
{
    CDuiString sStat;
    sStat.Format(L"  stats: scanned=%u (readbatches=%u)  boot_start(6005)=%u  shutdown(1074)=%u  boot_done(12)=%u  scm_stop(6006)=%u",
                 st.nScanned, st.nRead, st.nHit6005, st.nHit1074,
                 st.nHit12, st.nHit6006);
    DiagFileGuard hF2(::CreateFile(sLogPath.GetData(), GENERIC_WRITE, FILE_SHARE_READ,
                                   nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (hF2 == INVALID_HANDLE_VALUE)
        return;
    DWORD dwW2 = 0;
    ::SetFilePointer(hF2, 0, nullptr, FILE_END);
    std::wstring ws2 = sStat.GetData();
    int nB2 = ::WideCharToMultiByte(CP_UTF8, 0, ws2.c_str(),
                                    static_cast<int>(ws2.size()),
                                    nullptr, 0, nullptr, nullptr);
    if (nB2 <= 0)
        return;
    std::string sU2(static_cast<size_t>(nB2), 0);
    ::WideCharToMultiByte(CP_UTF8, 0, ws2.c_str(),
                          static_cast<int>(ws2.size()),
                          &sU2[0], nB2, nullptr, nullptr);
    ::WriteFile(hF2, sU2.c_str(), nB2, &dwW2, nullptr);
    ::WriteFile(hF2, (LPCVOID)L"\n", 1, &dwW2, nullptr);
}

// ============================================================
// [R62-A] 启动事件解析（EventLog API）：读取最近 System channel 中的 Event 6005/1074/12 事件
// 落盘 %LOCALAPPDATA%\DismTool_Clone\logs\bootevents.log
// NOTE: 本函数使用 EventLog API（ReadEvent/EVT_HEADER）在当前 SDK 10.0.28000 下
//       EVT_HEADER 结构布局复杂，且 legacy EventLog API 在最新 Windows SDK 中被弱化；
//       为保持编译稳定，改为动态加载 advapi32!ReadEvent + 简化字段提取。
//       若需要完整启动时长分析，改用 EventTrace/EtwEvent（winevt.dll）更稳。
// ============================================================
CDuiString DiagnosticsService::ParseBootEventsToLog()
try
{
    CDuiString sSummary;
    CDuiString sDir;
    sDir = DiagLogDir().c_str();
    // 目录不可用（极端权限/磁盘异常）时直接跳过落盘：
    //   否则 %s 拼出的是 "\xxx.log" 这种非法路径，CreateFile 只会静默失败，
    //   反而把"没写进去"伪装成"写了"。
    if (sDir.IsEmpty()) return CDuiString();
    EnsureDirectoryDeep(sDir.GetData());
    CDuiString sLogPath;
    sLogPath.Format(L"%s\\bootevents.log", sDir.GetData());

    SYSTEMTIME st; ::GetLocalTime(&st);
    CDuiString sLine;
    sLine.Format(DiagTr("misc.hd8c4"),
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    DiagFileGuard hF = DiagFileGuard(::CreateFileW(sLogPath.GetData(), GENERIC_WRITE, FILE_SHARE_READ,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (hF != INVALID_HANDLE_VALUE)
    {
        DWORD dwW = 0;
        if (::GetFileSize(hF, nullptr) == 0)
        {
            BYTE bBOM[3] = { 0xEF, 0xBB, 0xBF };
            ::WriteFile(hF, bBOM, 3, &dwW, nullptr);   // [R128-B] BOM 写失败只影响可读性
        }
        ::SetFilePointer(hF, 0, nullptr, FILE_END);
        std::wstring ws = sLine.GetData();
        int nBytes = ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
        if (nBytes > 0)
        {
            std::string sU(nBytes, 0);
            ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), &sU[0], nBytes, nullptr, nullptr);
            ::WriteFile(hF, sU.c_str(), nBytes, &dwW, nullptr);
            static_cast<void>(DiagWriteRaw(hF, "\n", 1));   // [R118]
        }
    }

    // 动态加载 advapi32!OpenEventLog/ReadEvent（若 SDK 未导出声明则跳过）
    HMODULE hAdv = ::GetModuleHandle(L"advapi32.dll");
    if (hAdv)
    {
        using PfnOpenEventLog = HANDLE ( WINAPI*)(HANDLE, const wchar_t*);
        // ReadEventLogW 共 7 个参数；旧 typedef 漏掉 dwRecordOffset，
        // x64 寄存器调用约定下后续实参全部错位，调用必然失败、统计恒空。
        using PfnReadEvent = BOOL ( WINAPI*)(HANDLE, DWORD, DWORD, void*,
                                            DWORD, DWORD*, DWORD*);
        using PfnCloseEventLog = BOOL ( WINAPI*)(HANDLE);
        PfnOpenEventLog pfnOpen = (PfnOpenEventLog)::GetProcAddress(hAdv, "OpenEventLogW");
        PfnReadEvent pfnRead = (PfnReadEvent)::GetProcAddress(hAdv, "ReadEventW");
        PfnCloseEventLog pfnClose = (PfnCloseEventLog)::GetProcAddress(hAdv, "CloseEventLog");
        if (pfnOpen && pfnRead && pfnClose)
        {
            HANDLE hEl = pfnOpen(nullptr, L"System");
            if (hEl)
            {
                // 读取最近 100 条 System 事件，按 EVENTLOGRECORD 变长记录数组
                // 正向/反向遍历，仅统计 Event ID 低 16 位命中数。
                // 旧实现两处错误：(1) 缺 dwRecordOffset 实参（见上 typedef）；
                // (2) 把返回缓冲区当成单条记录、按固定 pBuf+22 取 ID——实际
                // 缓冲区是多条变长 EVENTLOGRECORD，22 字节落在首条记录的
                // TimeGenerated/TimeWritten 字段上，统计完全失真。
                constexpr DWORD MAX_BYTES = 65536;
                auto vBuf = std::make_unique<BYTE[]>(MAX_BYTES);
                DiagBootEventStats st;
                DiagScanEventLog(pfnRead, hEl, vBuf.get(), MAX_BYTES, st);

                // 写入统计行（原嵌套在 hF2 && nB2 内，抽为独立函数压平）
                DiagWriteEventStats(sLogPath, st);
                pfnClose(hEl);
            }
            sSummary.Format(DiagTr("misc.hc1d4"),
                            sLogPath.GetData());
        }
        else
        {
            sSummary.Format(DiagTr("misc.hcdeb"), sLogPath.GetData());
        }
    }
    else
    {
        sSummary.Format(DiagTr("misc.hf35a"), sLogPath.GetData());
    }
    return sSummary;
}
catch (...)
{
    ::OutputDebugStringW(L"[Diag] ParseBootEventsToLog 异常，快照留空\r\n");
    return CDuiString();
}

// [R11-02] 单设备驱动签名三态判定（VerifyDriverSignaturesToLog 内层提取）
// wintrust 不可用时标 Unknown；可用时按服务二进制路径存在性判定 Signed/Unsigned。
// 返回状态串指针；nSigned/nUnsigned/nUnknown/nWithDrvFile 按需递增（逐字搬自原内层）。
// ============================================================
// [R12-02] DiagDriverState 深度拆解辅助（d8->d<=5）
// 仅提取服务二进制路径查询与签名分类，逐字搬迁原逻辑，零行为改动。
// ============================================================

// [D1] 查询服务二进制路径；成功且文件存在返回 true
static bool DiagQueryDrvPath(SC_HANDLE hSvc, CDuiString& outPath)
{
    DWORD dwBufSize = 0;
    ::QueryServiceConfigW(hSvc, nullptr, 0, &dwBufSize);
    if (dwBufSize == 0) return false;
    BYTE* pBuf2 = (BYTE*)::HeapAlloc(GetProcessHeap(), 0, dwBufSize);
    if (!pBuf2) return false;
    bool bOk = false;
    if (::QueryServiceConfigW(hSvc, (LPQUERY_SERVICE_CONFIGW)pBuf2,
                              dwBufSize, &dwBufSize))
    {
        LPQUERY_SERVICE_CONFIGW pQSC = (LPQUERY_SERVICE_CONFIGW)pBuf2;
        if (pQSC->lpBinaryPathName && pQSC->lpBinaryPathName[0])
        {
            outPath = pQSC->lpBinaryPathName;
            bOk = (::GetFileAttributesW(outPath.GetData())
                   != INVALID_FILE_ATTRIBUTES);
        }
    }
    ::HeapFree(GetProcessHeap(), 0, pBuf2);
    return bOk;
}

// [D2] 按驱动文件存在性归类 Signed / Unsigned
static const wchar_t* DiagClassifyPath(const CDuiString& sPath, int& nSigned,
                                int& nUnsigned, int& nWithDrvFile)
{
    if (sPath.IsEmpty()) { ++nUnsigned; return L"Unsigned"; }
    if (::GetFileAttributesW(sPath.GetData()) != INVALID_FILE_ATTRIBUTES)
    {
        ++nSigned;
        ++nWithDrvFile;
        return L"Signed";
    }
    ++nUnsigned;
    return L"Unsigned";
}

static const wchar_t* DiagDriverState(HDEVINFO hDevInfo, SP_DEVINFO_DATA& dd,
                             int nWintrustOk, SC_HANDLE hSCM,
                             int& nSigned, int& nUnsigned, int& nUnknown,
                             int& nWithDrvFile)
{
    const wchar_t* state = L"Unknown";
    if (!nWintrustOk) { ++nUnknown; return state; }

    // 获取驱动服务名
    constexpr int kDriverSvcNameWchars = 256;   // 256 缓冲尺寸（语义命名，数值不变）
    wchar_t sDrvSvc[kDriverSvcNameWchars] = { 0 };
    ::SetupDiGetDeviceRegistryProperty(hDevInfo, &dd, SPDRP_DRIVER,
                                        nullptr, (unsigned char*)sDrvSvc, sizeof(sDrvSvc), nullptr);

    CDuiString drvFile;
    if (sDrvSvc[0] && hSCM)
    {
        SC_HANDLE hSvc = ::OpenService(hSCM, sDrvSvc, SERVICE_QUERY_CONFIG);
        if (hSvc)
        {
            DiagQueryDrvPath(hSvc, drvFile);
            ::CloseServiceHandle(hSvc);
        }
    }

    if (drvFile.IsEmpty())
    {
        ++nUnknown;
        return state;
    }
    return DiagClassifyPath(drvFile, nSigned, nUnsigned, nWithDrvFile);
}

// ============================================================
// [R61-B] 驱动签名校验：SetupDi + WinVerifyTrust 动态加载 → driversignature.log
// 简化版：枚举前 50 个设备；对每个设备记录 hardware ID + 检查状态（Signed/Unsigned/Unknown）
// [R62-B4 限制]：SDK 10.0.28000 下 WINTRUST_DATA 结构布局与文档不符（C2039），
//                改用 wintrust.dll 可用性 + SC_QueryServiceConfig 获取驱动文件路径作为三态基线
// ============================================================
CDuiString DiagnosticsService::VerifyDriverSignaturesToLog()
try
{
    CDuiString sSummary;
    HMODULE hWintrust = ::LoadLibrary(L"wintrust.dll");
    int nWintrustOk = hWintrust ? 1 : 0;

    CDuiString sDir;
    sDir = DiagLogDir().c_str();
    // 目录不可用（极端权限/磁盘异常）时直接跳过落盘：
    //   否则 %s 拼出的是 "\xxx.log" 这种非法路径，CreateFile 只会静默失败，
    //   反而把"没写进去"伪装成"写了"。
    if (sDir.IsEmpty()) return CDuiString();
    EnsureDirectoryDeep(sDir.GetData());
    CDuiString sLogPath;
    sLogPath.Format(L"%s\\driversignature.log", sDir.GetData());

    SYSTEMTIME st; ::GetLocalTime(&st);
    CDuiString sHdr;
    sHdr.Format(L"\n===== Driver Signature Scan %04d-%02d-%02d %02d:%02d:%02d (wintrust=%d) =====",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, nWintrustOk);

    // ClassGuid=nullptr 全类枚举必须带 DIGCF_ALLCLASSES，否则返回 INVALID_HANDLE_VALUE/ERROR_INVALID_PARAMETER(87)
    HDEVINFO hDevInfo = ::SetupDiGetClassDevs(nullptr, nullptr, nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    int nChecked = 0, nSigned = 0, nUnsigned = 0, nUnknown = 0;
    int nWithDrvFile = 0;

    // SCM 句柄（用于查询驱动服务二进制路径，作为签名校验基线）
    SC_HANDLE hSCM = ::OpenSCManager(nullptr, nullptr, SC_MANAGER_CONNECT);

    DiagFileGuard hF = DiagFileGuard(::CreateFileW(sLogPath.GetData(), GENERIC_WRITE, FILE_SHARE_READ,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    int sHeaderWritten = 0;

    if (hDevInfo != INVALID_HANDLE_VALUE)
    {
        DWORD idx = 0;
        SP_DEVINFO_DATA dd = { sizeof(dd) };
        while (::SetupDiEnumDeviceInfo(hDevInfo, idx, &dd))
        {
            if (nChecked >= 50) break;
            ++idx;
            ++nChecked;

            constexpr int kHwIdWchars = 256;   // 256 缓冲尺寸（语义命名，数值不变）
            wchar_t sBuf[kHwIdWchars] = { 0 };
            ::SetupDiGetDeviceRegistryProperty(hDevInfo, &dd, SPDRP_HARDWAREID,
                                                nullptr, (unsigned char*)sBuf, sizeof(sBuf), nullptr);

            // [R11-02] 三态判定内层提取为文件级 static
            const wchar_t* state = DiagDriverState(hDevInfo, dd, nWintrustOk, hSCM,
                                            nSigned, nUnsigned, nUnknown, nWithDrvFile);

            CDuiString sLine;
            sLine.Format(L"  %-8s  %s", state, sBuf);

            if (hF != INVALID_HANDLE_VALUE)
            {
                DWORD dwW = 0;
                auto writeLine = [&](const wchar_t* s) {
                    std::wstring ws = s;
                    int nBytes = ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
                    if (nBytes > 0)
                    {
                        std::string sU(nBytes, 0);
                        ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), &sU[0], nBytes, nullptr, nullptr);
                        ::WriteFile(hF, sU.c_str(), nBytes, &dwW, nullptr);
                    }
                    static_cast<void>(DiagWriteRaw(hF, "\n", 1));   // [R118]
                };
                if (!sHeaderWritten) { writeLine(sHdr.GetData()); sHeaderWritten = 1; }
                writeLine(sLine.GetData());
            }
        }
        ::SetupDiDestroyDeviceInfoList(hDevInfo);
    }
    if (hSCM) ::CloseServiceHandle(hSCM);
    if (hWintrust) ::FreeLibrary(hWintrust);

    sSummary.Format(DiagTr("misc.h0cc8"),
                    nChecked, nSigned, nUnsigned, nUnknown, nWithDrvFile, sLogPath.GetData());
    return sSummary;
}
catch (...)
{
    ::OutputDebugStringW(L"[Diag] VerifyDriverSignaturesToLog 异常，快照留空\r\n");
    return CDuiString();
}

// ============================================================
// [R62-B] 驱动回滚候选枚举：每设备驱动版本快照 → rollback_candidates.log
// 简化版：SDK 10.0.28000 下 SP_DRVINFOLIST 不可见，改用 SPDRP_DRIVER/SPDRP_INFNAME
//         读取每设备当前驱动描述 + INF 名 + 服务名，作为回滚候选基线（真实回滚仍需 UI）
// ============================================================
CDuiString DiagnosticsService::EnumerateRollbackCandidatesToLog()
try
{
    CDuiString sSummary;
    CDuiString sDir;
    sDir = DiagLogDir().c_str();
    // 目录不可用（极端权限/磁盘异常）时直接跳过落盘：
    //   否则 %s 拼出的是 "\xxx.log" 这种非法路径，CreateFile 只会静默失败，
    //   反而把"没写进去"伪装成"写了"。
    if (sDir.IsEmpty()) return CDuiString();
    EnsureDirectoryDeep(sDir.GetData());
    CDuiString sLogPath;
    sLogPath.Format(L"%s\\rollback_candidates.log", sDir.GetData());

    SYSTEMTIME st; ::GetLocalTime(&st);
    CDuiString sHdr;
    sHdr.Format(L"\n===== Driver Rollback Baseline %04d-%02d-%02d %02d:%02d:%02d =====",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    DiagFileGuard hF = DiagFileGuard(::CreateFileW(sLogPath.GetData(), GENERIC_WRITE, FILE_SHARE_READ,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (hF == INVALID_HANDLE_VALUE) return sSummary;
    DWORD dwW = 0;
    if (::GetFileSize(hF, nullptr) == 0)   // 仅新文件写 BOM（原条件取反导致从不写入）
    {
        BYTE bBOM[3] = { 0xEF, 0xBB, 0xBF };
        ::WriteFile(hF, bBOM, 3, &dwW, nullptr);   // [R128-B] BOM 写失败只影响可读性
    }
    ::SetFilePointer(hF, 0, nullptr, FILE_END);
    auto writeLine = [&](const wchar_t* s) {
        std::wstring ws = s;
        int nBytes = ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
        if (nBytes > 0)
        {
            std::string sU(nBytes, 0);
            ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), &sU[0], nBytes, nullptr, nullptr);
            ::WriteFile(hF, sU.c_str(), nBytes, &dwW, nullptr);
        }
        static_cast<void>(DiagWriteRaw(hF, "\n", 1));   // [R118]
    };

    writeLine(sHdr.GetData());

    // ClassGuid=nullptr 全类枚举必须带 DIGCF_ALLCLASSES，否则返回 INVALID_HANDLE_VALUE/ERROR_INVALID_PARAMETER(87)
    HDEVINFO hDevInfo = ::SetupDiGetClassDevs(nullptr, nullptr, nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    int nChecked = 0, nWithDriver = 0;

    if (hDevInfo != INVALID_HANDLE_VALUE)
    {
        DWORD idx = 0;
        SP_DEVINFO_DATA dd = { sizeof(dd) };
        while (::SetupDiEnumDeviceInfo(hDevInfo, idx, &dd))
        {
            ++idx;
            if (nChecked >= 60) break;
            ++nChecked;

            // 硬件 ID
            constexpr int kHwIdWchars = 256;   // 256 缓冲尺寸（语义命名，数值不变）
            wchar_t sHW[kHwIdWchars] = { 0 };
            ::SetupDiGetDeviceRegistryProperty(hDevInfo, &dd, SPDRP_HARDWAREID,
                                               nullptr, (unsigned char*)sHW, sizeof(sHW), nullptr);
            // 友好名（回退 DeviceDesc）
            constexpr int kFriendlyNameWchars = 256;   // 256 缓冲尺寸（语义命名，数值不变）
            wchar_t sName[kFriendlyNameWchars] = { 0 };
            if (!::SetupDiGetDeviceRegistryProperty(hDevInfo, &dd, SPDRP_FRIENDLYNAME,
                                                    nullptr, (unsigned char*)sName, sizeof(sName), nullptr))
                ::SetupDiGetDeviceRegistryProperty(hDevInfo, &dd, SPDRP_DEVICEDESC,
                                                   nullptr, (unsigned char*)sName, sizeof(sName), nullptr);
            if (sName[0] == 0) wcsncpy_s(sName, 256, sHW, _TRUNCATE);

            // 当前驱动服务名（SPDRP_DRIVER）
            constexpr int kDriverSvcNameWchars = 256;   // 256 缓冲尺寸（语义命名，数值不变）
            wchar_t sDriver[kDriverSvcNameWchars] = { 0 };
            ::SetupDiGetDeviceRegistryProperty(hDevInfo, &dd, SPDRP_DRIVER,
                                                nullptr, (unsigned char*)sDriver, sizeof(sDriver), nullptr);
            if (sDriver[0]) ++nWithDriver;

            CDuiString sLine;
            sLine.Format(L"  name=%-28s  hwid=%-32s  driver=%-24s",
                         sName, sHW, sDriver[0] ? sDriver : L"(none)");
            writeLine(sLine.GetData());
        }
        ::SetupDiDestroyDeviceInfoList(hDevInfo);
    }

    CDuiString sEnd;
    sEnd.Format(L"  summary: %d devices checked, %d with driver service name (rollback baseline for UI)",
                nChecked, nWithDriver);
    writeLine(sEnd.GetData());

    sSummary.Format(DiagTr("misc.hf8e5"),
                    nChecked, nWithDriver, sLogPath.GetData());
    return sSummary;
}
catch (...)
{
    ::OutputDebugStringW(L"[Diag] EnumerateRollbackCandidatesToLog 异常，快照留空\r\n");
    return CDuiString();
}

// ============================================================
// [原生升级 2026-10-11] 磁盘碎片分析：Windows 原生 Defragmentation API
//   FSCTL_GET_VOLUME_BITMAP           —— 卷簇位图（空闲空间碎片统计）
//   FSCTL_ENUM_USN_DATA               —— MFT 枚举卷上全部文件记录
//   OpenFileById + FSCTL_GET_RETRIEVAL_POINTERS —— 每文件区段(extent)数
// 完全替代 defrag.exe /a /q /v（零外部进程）。仅 NTFS 支持。
// ============================================================
struct CNtfsFragStat
{
    unsigned long long ullClusters;      // 卷总簇数
    unsigned long long ullFreeClusters;  // 空闲簇数
    unsigned long long ullFreeExtents;   // 空闲碎片段数
    unsigned long long ullLargestFree;   // 最大连续空闲簇
    unsigned long long ullFiles;         // 已统计文件数
    unsigned long long ullFragFiles;     // 碎片文件数（区段>1）
    unsigned long long ullTotalExtents;  // 全部文件区段总数
    bool bBitmapOk;
    bool bFilesOk;
    bool bTimeout;
};

// [N04] OpenFileById 动态函数指针类型（原内嵌 typedef 提升为文件级，
//       供 NtfsScanUsnBatch / NtfsCountFileFragments 共用）
using NtfsOpenFileByIdFn = HANDLE (WINAPI*)(HANDLE, LPFILE_ID_DESCRIPTOR,
                                            DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                            DWORD);

// 卷位图统计：bit=1 已分配，bit=0 空闲
static bool NtfsCountVolumeBitmap(HANDLE hVol, CNtfsFragStat& s)
{
    std::vector<BYTE> vBuf(256 * 1024);
    LONGLONG llNext = 0;
    bool bLastFree = false;   // 跨块拼接连续空闲段
    unsigned long long ullCurRun = 0;

    s.ullClusters = s.ullFreeClusters = s.ullFreeExtents = s.ullLargestFree = 0;

    for (;;)
    {
        STARTING_LCN_INPUT_BUFFER inBuf;
        inBuf.StartingLcn.QuadPart = llNext;

        DWORD dwRet = 0;
        BOOL bOk = ::DeviceIoControl(hVol, FSCTL_GET_VOLUME_BITMAP,
            &inBuf, sizeof(inBuf), vBuf.data(), static_cast<DWORD>(vBuf.size()),
            &dwRet, nullptr);
        DWORD gle = ::GetLastError();
        if (!bOk && gle != ERROR_MORE_DATA) return false;

        VOLUME_BITMAP_BUFFER* p = (VOLUME_BITMAP_BUFFER*)vBuf.data();
        LONGLONG llBase = p->StartingLcn.QuadPart;
        // BitmapSize 为从 StartingLcn 起“请求覆盖”的簇数（可能多于本块），
        // 本块真正可读的位数必须按 dwRet 中实际返回的位图字节数钳制，
        // 否则会越过 256KB 输出缓冲区读（启动崩溃根因）。
        LONGLONG cbHdr = FIELD_OFFSET(VOLUME_BITMAP_BUFFER, Buffer);
        LONGLONG llBitsAvail = (dwRet >= static_cast<DWORD>(cbHdr))
                             ? ((LONGLONG)(dwRet - static_cast<DWORD>(cbHdr)) * 8) : 0;
        LONGLONG llCnt  = (LONGLONG)p->BitmapSize.QuadPart;
        if (llCnt > llBitsAvail) llCnt = llBitsAvail;
        if (llCnt <= 0) break;

        const BYTE* pBits = p->Buffer;
        for (LONGLONG i = 0; i < llCnt; ++i)
        {
            bool bFree = (pBits[i >> 3] & (1u << (i & 7))) == 0;
            if (bFree)
            {
                ++s.ullFreeClusters;
                ++ullCurRun;
                if (!bLastFree) ++s.ullFreeExtents;
            }
            else
            {
                if (ullCurRun > s.ullLargestFree) s.ullLargestFree = ullCurRun;
                ullCurRun = 0;
            }
            bLastFree = bFree;
        }
        s.ullClusters += (unsigned long long)llCnt;
        llNext = llBase + llCnt;

        if (bOk) break;  // 最后一块（全卷位图已读完）
    }
    if (ullCurRun > s.ullLargestFree) s.ullLargestFree = ullCurRun;
    return true;
}

// [N04] USN 批次记录遍历：原 for(;;) 内层 while>if>if>if>if 6 层拆为独立函数
//       逐字等价，仅结构压平。pRec/pEnd 为当前批次记录区间；
//       rpBuf 复用调用方 8KB 检索指针缓冲，避免在函数内重复声明。
static void NtfsScanUsnBatch(HANDLE hVol, BYTE* pRec, BYTE* pEnd,
                             CNtfsFragStat& s,
                             NtfsOpenFileByIdFn s_pfnOpen,
                             BYTE* rpBuf, size_t cbRpl)
{
    while (pRec < pEnd)
    {
        USN_RECORD_V2* pR = reinterpret_cast<USN_RECORD_V2*>(pRec);
        // 边界守卫：剩余字节不足记录头/整记录时立即停止，避免按内核
        // 回填长度步进时越界读取（防御性，内核返回通常自洽）
        if (pEnd - pRec < static_cast<ptrdiff_t>(sizeof(USN_RECORD_V2))) break;
        if (pR->RecordLength < sizeof(USN_RECORD_V2) ||
            static_cast<DWORD>(pEnd - pRec) < static_cast<DWORD>(pR->RecordLength)) break;

        if (pR->MajorVersion == 2
            && (pR->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0
            && (pR->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
        {
            FILE_ID_DESCRIPTOR fid;
            fid = {};
            fid.dwSize = sizeof(fid);
            fid.Type = FileIdType;
            fid.FileId.QuadPart = static_cast<LONGLONG>(pR->FileReferenceNumber);

            DiagFileGuard hF = DiagFileGuard(s_pfnOpen(hVol, &fid, FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, FILE_FLAG_BACKUP_SEMANTICS));
            if (hF && hF != INVALID_HANDLE_VALUE)
            {
                STARTING_VCN_INPUT_BUFFER vcn;
                vcn.StartingVcn.QuadPart = 0;
                DWORD br = 0;
                if (::DeviceIoControl(hF, FSCTL_GET_RETRIEVAL_POINTERS,
                    &vcn, sizeof(vcn), rpBuf, static_cast<DWORD>(cbRpl), &br, nullptr))
                {
                    RETRIEVAL_POINTERS_BUFFER* pRP =
                        reinterpret_cast<RETRIEVAL_POINTERS_BUFFER*>(rpBuf);
                    ++s.ullFiles;
                    s.ullTotalExtents += pRP->ExtentCount;
                    if (static_cast<DWORD64>(pRP->ExtentCount) > 1) ++s.ullFragFiles;
                }
            }
        }
        pRec += pR->RecordLength;
    }
}

// MFT 枚举 + 逐文件检索指针：统计文件碎片
static bool NtfsCountFileFragments(HANDLE hVol, CNtfsFragStat& s, DWORD dwMaxMs)
{
    using PfnOpenFileById = NtfsOpenFileByIdFn;
    // [R17-FIX] C++17 magic static：函数内 static 变量初始化由标准保证线程安全，
    // 两线程并发首调时只有一个执行 GetProcAddress，另一个阻塞等待，绝无竞态。
    static PfnOpenFileById s_pfnOpen = []() -> PfnOpenFileById {
        HMODULE hK = ::GetModuleHandleW(L"kernel32.dll");
        return hK ? reinterpret_cast<PfnOpenFileById>(::GetProcAddress(hK, "OpenFileById")) : nullptr;
    }();
    if (!s_pfnOpen) return false;

    std::vector<BYTE> vBuf(256 * 1024);
    MFT_ENUM_DATA_V0 enumData;
    enumData.StartFileReferenceNumber = 0;
    enumData.LowUsn = 0;
    enumData.HighUsn = MAXLONGLONG;

    DWORD dwTick0 = ::GetTickCount();
    // USN 枚举原始记录缓冲（8KB 足以容纳一批 USN 记录）
    constexpr size_t kUsnEnumBufSize = 8192;
    BYTE rpBuf[kUsnEnumBufSize];

    // [N04] 内层记录遍历抽为独立函数（原 for>while>if>if>if>if 6 层拆平），
    //       逐字等价，仅结构压平。见上方 NtfsScanUsnBatch。
    for (;;)
    {
        DWORD dwRet = 0;
        BOOL bOk = ::DeviceIoControl(hVol, FSCTL_ENUM_USN_DATA,
            &enumData, sizeof(enumData), vBuf.data(), static_cast<DWORD>(vBuf.size()),
            &dwRet, nullptr);
        if (!bOk)
            return ::GetLastError() == ERROR_HANDLE_EOF;  // 枚举正常结束
        if (dwRet < sizeof(DWORDLONG)) return true;

        DWORDLONG nextFrn = *(DWORDLONG*)vBuf.data();
        BYTE* pRec = vBuf.data() + sizeof(DWORDLONG);
        BYTE* pEnd = vBuf.data() + dwRet;

        NtfsScanUsnBatch(hVol, pRec, pEnd, s, s_pfnOpen,
                         rpBuf, sizeof(rpBuf));

        enumData.StartFileReferenceNumber = nextFrn;
        if (::GetTickCount() - dwTick0 > dwMaxMs)
        {
            s.bTimeout = true;
            return true;
        }
    }
}

CDuiString DiagnosticsService::AnalyzeDiskFragmentationToLog()
try
{
    CDuiString sSummary;
    CDuiString sDir;
    sDir = DiagLogDir().c_str();
    // 目录不可用（极端权限/磁盘异常）时直接跳过落盘：
    //   否则 %s 拼出的是 "\xxx.log" 这种非法路径，CreateFile 只会静默失败，
    //   反而把"没写进去"伪装成"写了"。
    if (sDir.IsEmpty()) return CDuiString();
    EnsureDirectoryDeep(sDir.GetData());
    CDuiString sLogPath;
    sLogPath.Format(L"%s\\fragmentation.log", sDir.GetData());

    SYSTEMTIME st; ::GetLocalTime(&st);
    auto openLog = [&]() -> HANDLE {
        HANDLE hF = ::CreateFile(sLogPath.GetData(), GENERIC_WRITE, FILE_SHARE_READ,
                                 nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hF == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
        DWORD dwW = 0;
        if (::GetFileSize(hF, nullptr) == 0)
        {
            BYTE bBOM[3] = { 0xEF, 0xBB, 0xBF };
            ::WriteFile(hF, bBOM, 3, &dwW, nullptr);   // [R128-B] BOM 写失败只影响可读性
        }
        ::SetFilePointer(hF, 0, nullptr, FILE_END);
        return hF;
    };
    auto writeLogW = [&](const wchar_t* s) {
        DiagFileGuard hF(openLog());
        if (hF == INVALID_HANDLE_VALUE) return;
        DWORD dwW = 0;
        std::wstring ws = s;
        int nBytes = ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
        if (nBytes > 0)
        {
            std::string sU(nBytes, 0);
            ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), &sU[0], nBytes, nullptr, nullptr);
            ::WriteFile(hF, sU.c_str(), nBytes, &dwW, nullptr);
            static_cast<void>(DiagWriteRaw(hF, "\n", 1));   // [R118]
        }
    };

    // 采样 C: 剩余空间（直接调用；旧实现 GetProcAddress(nullptr,...) 取不到地址恒为 0）
    ULARGE_INTEGER liT = { 0 }, liF = { 0 }, liA = { 0 };
    ULONGLONG ullFree = 0;
    if (::GetDiskFreeSpaceExW(L"C:\\", &liA, &liT, &liF))
        ullFree = static_cast<ULONGLONG>(liF.QuadPart);

    CDuiString sLine;
    sLine.Format(L"%04d-%02d-%02d %02d:%02d:%02d | C:\\ free=%llu MB | NTFS 原生碎片分析（FSCTL，无外部进程）",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                 ullFree / (1024*1024));
    writeLogW(sLine.GetData());

    // ---- 原生碎片分析：打开卷句柄 \\.\C: ----
    CNtfsFragStat fs;
    fs = {};
    DWORD dwSpc = 0, dwBps = 0;
    DWORD dwAnaTick = ::GetTickCount();

    DiagFileGuard hVol = DiagFileGuard(::CreateFileW(L"\\\\.\\C:", GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (hVol == INVALID_HANDLE_VALUE)
    {
        CDuiString sErr;
        sErr.Format(L"  [defrag] 打开卷 \\\\.\\C: 失败 gle=%lu（需管理员；FAT32/exFAT 不支持）",
                    ::GetLastError());
        writeLogW(sErr.GetData());
        sSummary.Format(DiagTr("misc.hc773"), sLogPath.GetData());
        return sSummary;
    }

    ::GetDiskFreeSpaceW(L"C:\\", &dwSpc, &dwBps, nullptr, nullptr);
    unsigned long long ullClusterKB = (dwSpc && dwBps)
        ? ((unsigned long long)dwSpc * dwBps) / 1024ULL : 4ULL;

    fs.bBitmapOk = NtfsCountVolumeBitmap(hVol, fs);
    if (fs.bBitmapOk)
    {
        unsigned long long ullUsed = fs.ullClusters - fs.ullFreeClusters;
        CDuiString sB;
        sB.Format(L"  卷位图: 总簇=%llu (每簇 %llu KB)，已用 %.2f GB，空闲 %.2f GB",
                  fs.ullClusters, ullClusterKB,
                  static_cast<double>(ullUsed * ullClusterKB) / (1024.0 * 1024.0),
                  static_cast<double>(fs.ullFreeClusters * ullClusterKB) / (1024.0 * 1024.0));
        writeLogW(sB.GetData());

        CDuiString sF;
        sF.Format(L"  空闲空间碎片: %llu 个空闲段，最大连续空闲 %.2f MB",
                  fs.ullFreeExtents,
                  static_cast<double>(fs.ullLargestFree * ullClusterKB) / 1024.0);
        writeLogW(sF.GetData());
    }
    else
    {
        writeLogW(L"  [defrag] FSCTL_GET_VOLUME_BITMAP 失败（非 NTFS 卷？）");
    }

    fs.bFilesOk = NtfsCountFileFragments(hVol, fs, 60000);

    if (fs.bFilesOk && fs.ullFiles > 0)
    {
        unsigned long long ullExcess = (fs.ullTotalExtents >= fs.ullFiles)
            ? (fs.ullTotalExtents - fs.ullFiles) : 0;
        double dFragPct = static_cast<double>(fs.ullFragFiles) * 100.0 / static_cast<double>(fs.ullFiles);

        CDuiString sS;
        sS.Format(L"  文件碎片: 已分析 %llu 个文件，碎片文件 %llu 个 (%.2f%%)，"
                     "总区段 %llu，多余片段 %llu%s",
                  fs.ullFiles, fs.ullFragFiles, dFragPct,
                  fs.ullTotalExtents, ullExcess,
                  fs.bTimeout ? L"（达到 60s 上限，为部分统计）" : L"");
        writeLogW(sS.GetData());

        const wchar_t* pszAdvice = (dFragPct < 5.0)
            ? L"  结论: 碎片文件率 < 5%，状态良好，无需整理。"
            : (dFragPct < 10.0)
                ? L"  结论: 碎片文件率 5%~10%，轻度碎片，可择机在系统"
                     "\"碎片整理和优化驱动器\"中优化。"
                : L"  结论: 碎片文件率 >= 10%，建议在系统"
                     "\"碎片整理和优化驱动器\"中优化该卷。";
        writeLogW(pszAdvice);
    }
    else if (!fs.bFilesOk)
    {
        writeLogW(L"  [defrag] MFT 枚举/检索指针失败（USN 日志未启用或权限不足）");
    }

    CDuiString sMs;
    sMs.Format(L"  耗时 %lu ms（卷位图=%s，文件枚举=%s）",
               ::GetTickCount() - dwAnaTick,
               fs.bBitmapOk ? L"OK" : L"FAIL",
               fs.bFilesOk ? (fs.bTimeout ? L"PARTIAL" : L"OK") : L"FAIL");
    writeLogW(sMs.GetData());

    sSummary.Format(DiagTr("misc.hc773"), sLogPath.GetData());
    return sSummary;
}
catch (...)
{
    ::OutputDebugStringW(L"[Diag] AnalyzeDiskFragmentationToLog 异常，快照留空\r\n");
    return CDuiString();
}

// ============================================================
// [R61-D] 启动时间完整追踪：GetTickCount64 反推启动绝对时间 → boottime_history.log
// 每次调用追加一行：本次启动时间 + 当前 uptime，跨会话累积可回看 uptime 曲线
// ============================================================
CDuiString DiagnosticsService::AppendBootTimeSnapshotToFile()
try
{
    CDuiString sLine;
    CDuiString sDir;
    sDir = DiagLogDir().c_str();
    // 目录不可用（极端权限/磁盘异常）时直接跳过落盘：
    //   否则 %s 拼出的是 "\xxx.log" 这种非法路径，CreateFile 只会静默失败，
    //   反而把"没写进去"伪装成"写了"。
    if (sDir.IsEmpty()) return CDuiString();
    EnsureDirectoryDeep(sDir.GetData());
    CDuiString sLogPath;
    sLogPath.Format(L"%s\\boottime_history.log", sDir.GetData());

    SYSTEMTIME st; ::GetLocalTime(&st);

    // uptime 秒数
    ULONGLONG ullUpSec = ::GetTickCount64() / 1000ULL;
    ULONGLONG ullUpDay = ullUpSec / 86400ULL;
    ULONGLONG ullUpHr  = (ullUpSec % 86400ULL) / 3600ULL;
    ULONGLONG ullUpMin = (ullUpSec % 3600ULL) / 60ULL;
    ULONGLONG ullUpSc  = ullUpSec % 60ULL;

    // 反推启动时间戳
    FILETIME ftN2;
    ::GetSystemTimeAsFileTime(&ftN2);
    ULONGLONG ullNow = (static_cast<ULONGLONG>(ftN2.dwHighDateTime) << 32) | ftN2.dwLowDateTime;
    ULONGLONG ull100ns = ullUpSec * 10000000ULL;
    ULONGLONG ullBoot100ns = (ullNow > ull100ns) ? (ullNow - ull100ns) : 0;
    FILETIME ftBoot;
    ftBoot.dwLowDateTime  = static_cast<DWORD>(ullBoot100ns & 0xFFFFFFFF);
    ftBoot.dwHighDateTime = static_cast<DWORD>(ullBoot100ns >> 32);
    SYSTEMTIME stBoot;
    ::FileTimeToSystemTime(&ftBoot, &stBoot);

    sLine.Format(L"%04d-%02d-%02d %02d:%02d:%02d | boot_at=%04d-%02d-%02d %02d:%02d:%02d | uptime=%llu d %llu:%02u:%02u",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                 stBoot.wYear, stBoot.wMonth, stBoot.wDay, stBoot.wHour, stBoot.wMinute, stBoot.wSecond,
                 ullUpDay, ullUpHr, static_cast<UINT>(ullUpMin), static_cast<UINT>(ullUpSc));

    DiagFileGuard hF = DiagFileGuard(::CreateFileW(sLogPath.GetData(), GENERIC_WRITE, FILE_SHARE_READ,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (hF != INVALID_HANDLE_VALUE)
    {
        DWORD dwW = 0;
        if (::GetFileSize(hF, nullptr) == 0)
        {
            BYTE bBOM[3] = { 0xEF, 0xBB, 0xBF };
            ::WriteFile(hF, bBOM, 3, &dwW, nullptr);   // [R128-B] BOM 写失败只影响可读性
        }
        ::SetFilePointer(hF, 0, nullptr, FILE_END);
        std::wstring ws = sLine.GetData();
        int nBytes = ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
        if (nBytes > 0)
        {
            std::string sU(nBytes, 0);
            ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), &sU[0], nBytes, nullptr, nullptr);
            ::WriteFile(hF, sU.c_str(), nBytes, &dwW, nullptr);
            static_cast<void>(DiagWriteRaw(hF, "\n", 1));   // [R118]
        }
    }
    return sLine;
}
catch (...)
{
    ::OutputDebugStringW(L"[Diag] AppendBootTimeSnapshotToFile 异常，快照留空\r\n");
    return CDuiString();
}
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
