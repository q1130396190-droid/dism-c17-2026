// [A11Y-3 diag] Enumerate every top-level window of a PID with its class name.
//
// Why: SendMessage(WM_GETOBJECT) to MainWindowHandle returned 0 for BOTH the
// MSAA and UIA object ids, which cannot happen if that handle is the DuiLib
// paint window (the message handler is wired in WindowImplBase). So the handle
// is probably not the one DuiLib owns. This lists them all so the probe can
// target the real one instead of guessing.
#include <windows.h>
#include <cstdio>
#include <cstdlib>   /* strtoul */

struct CEnumCtx
{
    DWORD dwPid;
    int   nFound;
};

BOOL CALLBACK EnumProc(HWND hWnd, LPARAM lParam)
{
    CEnumCtx* pCtx = reinterpret_cast<CEnumCtx*>(lParam);
    DWORD dwPid = 0;
    ::GetWindowThreadProcessId(hWnd, &dwPid);
    if (dwPid != pCtx->dwPid) return TRUE;

    wchar_t szClass[256] = {};
    ::GetClassNameW(hWnd, szClass, 256);
    wchar_t szTitle[256] = {};
    ::GetWindowTextW(hWnd, szTitle, 256);

    const bool bVisible = ::IsWindowVisible(hWnd) != FALSE;
    char szClassA[256] = {};
    ::WideCharToMultiByte(CP_UTF8, 0, szClass, -1, szClassA, 255, nullptr, nullptr);
    char szOut[768] = {};
    ::sprintf_s(szOut, "hwnd=%lld visible=%d class=%s",
                (long long)(INT_PTR)hWnd, bVisible ? 1 : 0, szClassA);
    ::printf("%s", szOut);
    ::putchar(10);
    ++pCtx->nFound;
    return TRUE;
}

int main(int argc, char** argv)
{
    if (argc < 2) { ::printf("[FAIL] usage: winenum <pid>"); ::putchar(10); return 2; }
    CEnumCtx ctx = {};
    ctx.dwPid = static_cast<DWORD>(static_cast<DWORD>(std::strtoul(argv[1], nullptr, 10)));
    ::EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
    ::printf("count=%d", ctx.nFound);
    ::putchar(10);
    return ctx.nFound > 0 ? 0 : 1;
}
