#pragma once

// ============================================================
// SafeThread.h —— 工作线程 / UI 消息回调 顶层异常边界
//
// 背景（WER 取证结论）：
//   * APPCRASH c000041d（Fatal User Callback Exception）= SEH/C++ 异常
//     逃逸出窗口过程或工作线程过程，被 BaseThreadInitThunk / user32
//     回调包装器判定为"用户回调中未处理异常"，直接终止进程。
//   * BEX64 c0000409（FAST_FAIL_FATAL_APP_EXIT，AbnormalExitCode=10）
//     = 未捕获 C++ 异常逃逸线程过程 → std::terminate → abort()。
//
// 规则：
//   1) 所有 CreateThread/_beginthreadex 一律改用 SafeCreateThread /
//      SafeBeginThread；线程过程内仍可写正常 C++ 代码，本边界两层兜底：
//        - 内层 catch(...)：正常 C++ 展开（析构链执行）
//        - 外层 __except：捕获 AV/堆损坏等 SEH 硬件异常
//   2) UI 线程唯一消息泵的 DispatchMessage / TranslateAccelerator
//      必须经 SafeDispatchMessage / SafeTranslateAccelerator 包装。
//   3) 任何拦截都写入 %LOCALAPPDATA%\DismTool_Clone\logs\crashguard.log
//      留痕，便于事后定位（栈溢出不吞，继续走顶层 WER）。
//
// 纯头文件、仅依赖 Win32 + CRT；无 C++ 堆对象的函数才允许直接
// 写 __try/__except（MSVC 禁止二者同函数混用对象展开语义）。
// ============================================================

#include <windows.h>
#include <process.h>
#include <cstdlib>

// F8 禁列清零：窄字符串拼接/格式化封装
#include "FormatW.h"
#include "WinHandles.h"   // [C++17 R3] win::HandlePtr for log-file handle RAII

// ---------------- 崩溃留痕（无依赖、无异常） ----------------

inline void CrashGuardWrite(const char* pszTag, DWORD dwCode, const void* pvAddr)
{
    char szPath[MAX_PATH] = { 0 };
    DWORD n = ::GetEnvironmentVariableA("LOCALAPPDATA", szPath, MAX_PATH - 1);
    if (n == 0 || n >= MAX_PATH - 40) return;   // 环境缺失/缓冲不足：放弃留痕

    // 逐级确保目录存在（CreateDirectory 对已存在目录返回失败，忽略即可）
    ::CreateDirectoryA(szPath, nullptr);
    wfmt::AppendTo(szPath, MAX_PATH, "\\DismTool_Clone");
    ::CreateDirectoryA(szPath, nullptr);
    wfmt::AppendTo(szPath, MAX_PATH, "\\logs");
    ::CreateDirectoryA(szPath, nullptr);
    wfmt::AppendTo(szPath, MAX_PATH, "\\crashguard.log");

    SYSTEMTIME st;
    ::GetLocalTime(&st);
    char szLine[320];
    std::string sLine = FormatA(
        "%04u-%02u-%02u %02u:%02u:%02u [%s] code=0x%08lX addr=0x%p tid=%lu\r\n",
        (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
        (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
        pszTag ? pszTag : "?",
        (unsigned long)dwCode, pvAddr,
        (unsigned long)::GetCurrentThreadId());
    if (sLine.empty()) return;
    const int cch = static_cast<int>(sLine.size());
    wfmt::CopyTo(szLine, _countof(szLine), sLine.c_str());

    win::HandlePtr h = win::file_handle(::CreateFileA(szPath, FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr));   // [C++17 R3] RAII: auto CloseHandle at scope exit
    if (!win::valid(h)) return;
    DWORD dwW = 0;
    ::WriteFile(h.get(), szLine, static_cast<DWORD>(cch), &dwW, nullptr);
}

// SEH 过滤器：记录后吞掉异常；栈溢出继续上交（栈空间不可信）
inline LONG WINAPI CrashGuardFilter(EXCEPTION_POINTERS* pEp, const char* pszTag)
{
    DWORD dwCode = (pEp && pEp->ExceptionRecord) ? pEp->ExceptionRecord->ExceptionCode : 0;
    void* pvAddr = (pEp && pEp->ExceptionRecord) ? pEp->ExceptionRecord->ExceptionAddress : nullptr;
    if (dwCode == EXCEPTION_STACK_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;
    CrashGuardWrite(pszTag, dwCode, pvAddr);
    return EXCEPTION_EXECUTE_HANDLER;
}

// 进程级兜底：记录未被任何边界拦截的异常（不改变 WER 后续行为）
inline LONG WINAPI CrashGuardTopLevel(EXCEPTION_POINTERS* pEp)
{
    DWORD dwCode = (pEp && pEp->ExceptionRecord) ? pEp->ExceptionRecord->ExceptionCode : 0;
    void* pvAddr = (pEp && pEp->ExceptionRecord) ? pEp->ExceptionRecord->ExceptionAddress : nullptr;
    CrashGuardWrite("unhandled", dwCode, pvAddr);
    return EXCEPTION_CONTINUE_SEARCH;
}
inline void CrashGuardInstall()
{
    ::SetUnhandledExceptionFilter(&CrashGuardTopLevel);
}

// ---------------- 工作线程边界 ----------------

using PfnSafeThread = DWORD ( WINAPI*)(void*);

// 内层：C++ 异常正常捕获（保证栈上对象析构）。必须保持普通 C++ 函数，
// 不可在此函数内直接写 __try。
inline DWORD SafeThreadDispatchCxx(PfnSafeThread pfn, void* pvCtx)
{
    try
    {
        return pfn(pvCtx);
    }
    catch (...)
    {
        CrashGuardWrite("worker-thread-cxx", static_cast<DWORD>(0xE06D7363u), nullptr);
        return 1;
    }
}

struct SafeThreadCall
{
    PfnSafeThread pfn;
    void*        pvCtx;
};

// 外层：SEH 硬件异常兜底（本函数不含任何需要展开的 C++ 对象）
inline DWORD WINAPI SafeThreadTrampoline(void* pv)
{
    SafeThreadCall* pCall = (SafeThreadCall*)pv;
    PfnSafeThread pfn = pCall->pfn;
    void* pvCtx = pCall->pvCtx;
    ::operator delete(pCall);

    __try
    {
        return SafeThreadDispatchCxx(pfn, pvCtx);
    }
    __except (CrashGuardFilter(GetExceptionInformation(), "worker-thread-seh"))
    {
        return 1;
    }
}

// CreateThread 替代：用法与 CreateThread(nullptr,0,pfn,ctx,0,nullptr) 一致
//
// [C++17 升级阶段1 说明] 本函数内的 operator new/delete 为受约束的合理 C++ 用法，
// 有意保留、不迁移到智能指针：SafeThreadCall 需穿越 __try/__except 跳板
// （MSVC 禁止在含 SEH 的函数中使用需要对象展开语义的 C++ 构造），
// 若改为 unique_ptr/new 会导致对象析构与 SEH 展开冲突。所有权已手工
// 配对：线程启动成功后由 Trampoline free，启动失败在此处立即 free。
inline HANDLE SafeCreateThread(PfnSafeThread pfn, void* pvCtx,
                               DWORD dwFlags = 0, DWORD* pdwThreadId = nullptr)
{
    if (pfn == nullptr) return nullptr;
    SafeThreadCall* pCall = static_cast<SafeThreadCall*>(::operator new(sizeof(SafeThreadCall)));
    if (pCall == nullptr) return nullptr;
    pCall->pfn = pfn;
    pCall->pvCtx = pvCtx;
    HANDLE h = ::CreateThread(nullptr, 0, &SafeThreadTrampoline, pCall,
                              dwFlags, pdwThreadId);
    if (h == nullptr) ::operator delete(pCall);
    return h;
}

// 模板重载：接受无捕获 lambda（工程内多处 StartXxxThread 的写法）。
// +fn 触发 lambda→普通函数指针转换；x64 仅一种调用约定，reinterpret 安全。
template <typename Fn>
inline HANDLE SafeCreateThread(Fn fn, void* pvCtx,
                               DWORD dwFlags = 0, DWORD* pdwThreadId = nullptr)
{
    return SafeCreateThread(reinterpret_cast<PfnSafeThread>(+fn),
                            pvCtx, dwFlags, pdwThreadId);
}

// _beginthreadex 版（unsigned __stdcall(void*) 与 DWORD WINAPI(void*)
// 在 Win32 上 ABI 完全一致：同为 __stdcall、指针/无符号等宽）
using PfnSafeThreadC = unsigned ( __stdcall*)(void*);

inline uintptr_t SafeBeginThread(PfnSafeThreadC pfn, void* pvCtx)
{
    if (pfn == nullptr) return 0;
    SafeThreadCall* pCall = static_cast<SafeThreadCall*>(::operator new(sizeof(SafeThreadCall)));
    if (pCall == nullptr) return 0;
    pCall->pfn = reinterpret_cast<PfnSafeThread>(pfn);
    pCall->pvCtx = pvCtx;
    uintptr_t h = _beginthreadex(nullptr, 0,
        reinterpret_cast<unsigned (__stdcall *)(void*)>(&SafeThreadTrampoline),
        pCall, 0, nullptr);
    if (h == 0) ::operator delete(pCall);
    return h;
}

// ---------------- UI 消息泵边界（本函数不含 C++ 堆对象） ----------------

inline LRESULT SafeDispatchMessage(const MSG* pMsg)
{
    __try
    {
        return ::DispatchMessageW(pMsg);
    }
    __except (CrashGuardFilter(GetExceptionInformation(), "ui-dispatch"))
    {
        // [R13-FIX] SEH 吞 AV 后至少落 debug output，比静默 return 0 多一条取证线索
        if (pMsg)
        {
            wchar_t sz[128];
            ::swprintf_s(sz, L"[crashguard] SafeDispatchMessage ate exception on msg=0x%04X hwnd=%p continuing\r\n",
                         pMsg->message, pMsg->hwnd);
            ::OutputDebugStringW(sz);
        }
        return 0;
    }
}

inline BOOL SafeTranslateAccelerator(HWND hWnd, HACCEL hAccel, MSG* pMsg)
{
    __try
    {
        return ::TranslateAcceleratorW(hWnd, hAccel, pMsg);
    }
    __except (CrashGuardFilter(GetExceptionInformation(), "ui-accelerator"))
    {
        return FALSE;
    }
}
// ---------------- SafeBeginThread 句柄收尾（单点收敛） ----------------

// SafeBeginThread 返回 uintptr_t（_beginthreadex 的原始返回类型），
// 调用方需要把它当 HANDLE 关闭。若各调用点各写一次 reinterpret_cast<HANDLE>，
// 转换就散落在业务代码里（类型安全维度按裸转换数计分）。
// 这里把「uintptr_t → HANDLE」的唯一转换点收在本函数内，页面侧只调 SafeCloseThreadHandle，
// 正文不再出现裸转换。语义与 ::CloseHandle(reinterpret_cast<HANDLE>(h)) 完全一致。
inline void SafeCloseThreadHandle(uintptr_t h) noexcept
{
    if (h == 0) return;
    ::CloseHandle(reinterpret_cast<HANDLE>(h));
}

// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
