#include "StdAfx.h"
#include "Utils/UICrashGuard.h"
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <atomic>
#include <filesystem>
#include <memory>
#include <io.h>     // _open_osfhandle / _close (OpenSharedAppend)
#include <fcntl.h>  // _O_APPEND / _O_TEXT (OpenSharedAppend)
#include <tlhelp32.h>   // [DIAG] LogCrashSite: module snapshot at crash time

// Windows headers come in via StdAfx.h -> UIlib.h (windows.h, windowsx.h).

namespace DuiLib {
namespace {

// ---- state (C++17) ----
std::once_flag        g_once;
std::atomic<bool>     g_installed{false};
std::mutex            g_logMtx;
FILE*                 g_logFile   = nullptr;
wchar_t               g_logPath[MAX_PATH] = {0};
std::filesystem::path g_logDir;

// ---- minimal dbghelp surface, loaded at runtime (no new link dependency) ----
struct MiniDumpExceptionInformation {
    DWORD ThreadId;
    PEXCEPTION_POINTERS ExceptionPointers;
    BOOL  ClientPointers;
};
constexpr DWORD kMiniDumpWithIndirectlyReferencedMemory = 0x00000040;
using MiniDumpWriteDumpFn = BOOL (WINAPI*)(HANDLE, DWORD, HANDLE, DWORD,
    MiniDumpExceptionInformation*, void*, void*);
HMODULE               g_dbgHelp = nullptr;
MiniDumpWriteDumpFn   g_miniDumpWriteDump = nullptr;

// ---- fatal first-chance classes worth logging ----
constexpr DWORD kFatalCodes[] = {
    EXCEPTION_ACCESS_VIOLATION, EXCEPTION_IN_PAGE_ERROR,
    EXCEPTION_PRIV_INSTRUCTION, EXCEPTION_ILLEGAL_INSTRUCTION,
    EXCEPTION_INT_DIVIDE_BY_ZERO, EXCEPTION_STACK_OVERFLOW,
    EXCEPTION_DATATYPE_MISALIGNMENT,
};

void Stamp(char* out, size_t cap)
{
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    ::snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

// [FIX-DIAG] Open the append log with FILE_SHARE_READ. The old _wfopen_s(L"at")
// grabbed the file exclusively, so no external tool (Get-Content, editors)
// could tail the ACTIVE log -- exactly what crash forensics needs most.
FILE* OpenSharedAppend(const wchar_t* path)
{
    HANDLE h = ::CreateFileW(path, GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    ::SetFilePointer(h, 0, nullptr, FILE_END);
    int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_APPEND | _O_TEXT);
    if (fd < 0) { ::CloseHandle(h); return nullptr; }
    FILE* f = _fdopen(fd, "a");
    if (!f) { _close(fd); ::CloseHandle(h); return nullptr; }
    return f;
}

const char* LevelTag(UICrashGuard::Level lv)
{
    static const char* kTags[] = { "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL" };
    return kTags[lv >= UICrashGuard::kDebug && lv <= UICrashGuard::kFatal ? lv : UICrashGuard::kError];
}

void OpenLogFile()
{
    // [R55] Honor DUILIB_TRACE_FILE first, so both trees in this project
    // collect logs the same way.
    //
    // Background: the architecture tree had no UICrashGuard at all, so R53
    // gave it a minimal sink driven by DUILIB_TRACE_FILE. That left the two
    // trees with different log locations and different enabling steps, which
    // means a field report needs two sets of instructions depending on which
    // binary the user happens to run. Supporting the same variable here costs
    // one GetEnvironmentVariable call at startup and removes that ambiguity.
    //
    // Contract kept identical to the architecture tree:
    //   * unset  -> previous behaviour (logs/ subdir next to the exe, timestamped)
    //   * set    -> append to exactly that path, no directory creation
    //               (the caller named the file, so the caller owns the folder)
    {
        wchar_t szExplicit[MAX_PATH] = {0};
        if (::GetEnvironmentVariableW(L"DUILIB_TRACE_FILE", szExplicit, MAX_PATH) > 0)
        {
            g_logFile = OpenSharedAppend(szExplicit);
            if (g_logFile) {
                ::setvbuf(g_logFile, nullptr, _IONBF, 0);
                ::wcsncpy_s(g_logPath, _countof(g_logPath), szExplicit, _TRUNCATE);
                g_logDir = std::filesystem::path(szExplicit).parent_path();
                return;
            }
            // fall through to the default location if the explicit path is unusable
        }
    }

    wchar_t exePath[MAX_PATH] = {0};
    if (::GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return;
    std::error_code ec;
    std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
    g_logDir = exeDir / L"logs";
    std::filesystem::create_directories(g_logDir, ec); // ec ignored: fall back to no-file logging

    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    // [SEC-11] g_logPath is wchar_t[MAX_PATH]. The previous code formatted with
    // snprintf into reinterpret_cast<char*>(g_logPath), storing narrow bytes in a
    // wide buffer: the resulting "wide string" is garbage (observed as a mangled
    // log filename) and can be read past the terminator. Use the wide formatter.
    ::swprintf_s(g_logPath, _countof(g_logPath),
        L"DuiLib_%04d%02d%02d_%02d%02d%02d_%lu.log",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
        static_cast<unsigned long>(::GetCurrentProcessId()));
    // UTF-8 path via std::filesystem (handles non-ASCII exe dirs)
    auto full = (g_logDir / std::filesystem::path(g_logPath)).native();
    g_logFile = OpenSharedAppend(full.c_str());
    if (g_logFile) { ::setvbuf(g_logFile, nullptr, _IONBF, 0); }
}

void WriteMiniDump(PEXCEPTION_POINTERS ep, DWORD code)
{
    if (g_miniDumpWriteDump == nullptr || g_logDir.empty()) return;
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    wchar_t name[96];
    // [SEC-11 same bug] The old code formatted narrow bytes into a wchar_t
    // buffer via snprintf: the "wide" filename was garbage, the extension got
    // truncated, and the dump landed in a mangled 0-byte file (OpenLogFile was
    // fixed the same way; this site was missed). Use the wide formatter.
    ::swprintf_s(name, _countof(name),
        L"crash_%04d%02d%02d_%02d%02d%02d_%lu_%08X.dmp",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
        static_cast<unsigned long>(::GetCurrentProcessId()), static_cast<unsigned>(code));
    auto full = (g_logDir / std::filesystem::path(name)).native();
    HANDLE hFile = ::CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return;
    MiniDumpExceptionInformation info{};
    info.ThreadId = ::GetCurrentThreadId();
    info.ExceptionPointers = ep;
    info.ClientPointers = FALSE;
    g_miniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(), hFile,
        kMiniDumpWithIndirectlyReferencedMemory, &info, nullptr, nullptr);
    ::CloseHandle(hFile);
}

// [DIAG] Crash-site reporter: logs the faulting address and up to 16 backtrace
// frames as MODULE+0xOFFSET (enough to pin the owner without a symbol server).
// Needed because MiniDumpWriteDump yields 0-byte files in this crash path.
// Crash-context rules: no heap, no new, no exceptions, static buffers only.
void LogCrashSite(PEXCEPTION_POINTERS ep)
{
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, ::GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    struct Row { DWORD64 base; DWORD size; char name[48]; };
    static Row rows[160];
    int n = 0;
    MODULEENTRY32W me{}; me.dwSize = sizeof(me);
    if (::Module32FirstW(snap, &me)) {
        do {
            if (n >= 160) break;
            rows[n].base = reinterpret_cast<DWORD64>(me.modBaseAddr);
            rows[n].size = me.modBaseSize;
            if (::WideCharToMultiByte(CP_ACP, 0, me.szModule, -1,
                    rows[n].name, 47, nullptr, nullptr) <= 0)
            { rows[n].name[0] = '?'; rows[n].name[1] = 0; }
            ++n;
        } while (::Module32NextW(snap, &me));
    }
    ::CloseHandle(snap);
    auto locate = [n](DWORD64 a) -> int {
        for (int i = 0; i < n; ++i)
            if (a >= rows[i].base && a < rows[i].base + rows[i].size) return i;
        return -1;
    };
    const DWORD64 exa = reinterpret_cast<DWORD64>(ep->ExceptionRecord->ExceptionAddress);
    // [DIAG] For AV: [0]=rw op (0 read /1 write /2 exec), [1]=faulting target.
    if (ep->ExceptionRecord->NumberParameters >= 2)
        DUI_LOGF("AVINFO op=%llu target=0x%llX",
            static_cast<unsigned long long>(ep->ExceptionRecord->ExceptionInformation[0]),
            static_cast<unsigned long long>(ep->ExceptionRecord->ExceptionInformation[1]));
    const int mi = locate(exa);
    if (mi >= 0) DUI_LOGF("CRASHSITE %s+0x%llX", rows[mi].name,
        static_cast<unsigned long long>(exa - rows[mi].base));
    else DUI_LOGF("CRASHSITE 0x%llX (no module)", static_cast<unsigned long long>(exa));
    void* frames[16] = {};
    const USHORT nf = ::RtlCaptureStackBackTrace(0, 16, frames, nullptr);
    for (USHORT f = 0; f < nf; ++f) {
        const DWORD64 a = reinterpret_cast<DWORD64>(frames[f]);
        const int k = locate(a);
        if (k >= 0) DUI_LOGF("FRAME %u %s+0x%llX", static_cast<unsigned>(f),
            rows[k].name, static_cast<unsigned long long>(a - rows[k].base));
        else DUI_LOGF("FRAME %u 0x%llX", static_cast<unsigned>(f),
            static_cast<unsigned long long>(a));
    }
}

LONG WINAPI UnhandledFilter(PEXCEPTION_POINTERS ep)
{
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    DUI_LOGF("UNHANDLED 0x%08X at 0x%p (tid=%lu)", code,
        ep->ExceptionRecord->ExceptionAddress,
        static_cast<unsigned long>(::GetCurrentThreadId()));
    LogCrashSite(ep);
    WriteMiniDump(ep, code);
    return EXCEPTION_EXECUTE_HANDLER; // quiet exit; WER dialog already suppressed
}

thread_local bool t_inVeh = false;
// [DIAG 2026-11-01] SEH filters (SafeDispatchMessage et al.) swallow fatal
// AVs before the unhandled filter ever runs: field evidence was "window
// gone, zombie process, log has only the bare first-chance line". Dump the
// crash site on first-chance too, capped per process (module snapshot +
// backtrace is expensive) and serialized so concurrent faults cannot race
// the static module table inside LogCrashSite.
std::atomic<int>  g_siteBudget{8};
std::atomic<bool> g_siteBusy{false};
LONG WINAPI FirstChanceVeh(PEXCEPTION_POINTERS ep)
{
    if (t_inVeh) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    bool fatal = false;
    for (DWORD c : kFatalCodes) { if (c == code) { fatal = true; break; } }
    if (fatal) {
        t_inVeh = true;
        DUI_LOGE("first-chance 0x%08X at 0x%p (tid=%lu)", code,
            ep->ExceptionRecord->ExceptionAddress,
            static_cast<unsigned long>(::GetCurrentThreadId()));
        if (g_siteBudget.fetch_sub(1) > 0) {
            bool expect = false;
            if (g_siteBusy.compare_exchange_strong(expect, true)) {
                LogCrashSite(ep);
                g_siteBusy.store(false);
            }
        }
        t_inVeh = false;
    }
    return EXCEPTION_CONTINUE_SEARCH; // observe only, never swallow
}

void InvalidParamHandler(const wchar_t* expr, const wchar_t* func,
    const wchar_t* file, unsigned line, uintptr_t)
{
    DUI_LOGE("invalid parameter: expr=%S func=%S file=%S line=%u",
        expr ? expr : L"?", func ? func : L"?", file ? file : L"?", line);
    // return instead of default fastfail: caller reports error gracefully
}

[[noreturn]] void PureCallHandler()
{
    DUI_LOGF("pure virtual call");
    std::fflush(nullptr);
    ::_exit(0x5055); // 'PU'
}

std::terminate_handler g_prevTerminate = nullptr;
void TerminateHandler()
{
    if (std::exception_ptr p = std::current_exception()) {
        try { std::rethrow_exception(p); }
        catch (const std::exception& e) { DUI_LOGF("std::terminate: %s", e.what()); }
        catch (...) { DUI_LOGF("std::terminate: unknown exception"); }
    } else {
        DUI_LOGF("std::terminate (no active exception)");
    }
    std::fflush(nullptr);
    if (g_prevTerminate) g_prevTerminate();
    ::_exit(0x7A72);
}

void DoInstall()
{
    if (::GetEnvironmentVariableW(L"DUILIB_CRASHGUARD_DISABLE", nullptr, 0) != 0) {
        g_installed.store(false);
        return; // kill-switch present
    }
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    OpenLogFile();
    g_dbgHelp = ::LoadLibraryW(L"dbghelp.dll");
    if (g_dbgHelp) {
        g_miniDumpWriteDump = reinterpret_cast<MiniDumpWriteDumpFn>(
            ::GetProcAddress(g_dbgHelp, "MiniDumpWriteDump"));
    }
    ::AddVectoredExceptionHandler(1, FirstChanceVeh);
    ::SetUnhandledExceptionFilter(UnhandledFilter);
    ::_set_invalid_parameter_handler(InvalidParamHandler);
    ::_set_purecall_handler(PureCallHandler);
    g_prevTerminate = std::set_terminate(TerminateHandler);
    g_installed.store(true);
    DUI_LOGI("UICrashGuard installed (pid=%lu, dump=%s, log=%ls)",
        static_cast<unsigned long>(::GetCurrentProcessId()),
        g_miniDumpWriteDump ? "on" : "off(dbghelp)", g_logPath);
}

} // namespace

void UICrashGuard::Install() { std::call_once(g_once, DoInstall); }
bool UICrashGuard::IsInstalled() { return g_installed.load(); }
const wchar_t* UICrashGuard::LogPath() { return g_logPath; }

void UICrashGuard::WriteLog(Level lv, const char* fmt, ...)
{
    if (g_logFile == nullptr) return;
    char msg[2048];
    va_list args;
    va_start(args, fmt);
    ::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    char ts[32];
    Stamp(ts, sizeof(ts));
    std::scoped_lock lk(g_logMtx);
    ::fprintf(g_logFile, "[%s] [pid %lu tid %lu] [%s] %s\n", ts,
        static_cast<unsigned long>(::GetCurrentProcessId()),
        static_cast<unsigned long>(::GetCurrentThreadId()), LevelTag(lv), msg);
}

} // namespace DuiLib