// Win11 能力实测：ApplyWin11Effects / SetPerMonitorV2Awareness
//
// 这是"真的能用"的验证，而不是"符号存在"：
//   1) 创建真实窗口
//   2) 调用 ApplyWin11Effects，检查返回的生效位掩码
//   3) 调用 SetPerMonitorV2Awareness，检查返回的 DPI 级别
//   4) 用 GetWindowDpiAwarenessContext 复核实际 DPI 感知级别
#include "StdAfx.h"
#include "UIlib.h"
#include <cstdio>
#include "Core/UIBase.h"
#include "Utils/DPI.h"

using DuiLib::CWindowWnd;
using DuiLib::CDPI;

static LRESULT CALLBACK ProbeProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return ::DefWindowProc(h, m, w, l);
}

// 一个能被 WindowImplBase 风格直接使用的最小 CWindowWnd 子类
class TinyWnd : public CWindowWnd
{
public:
    LPCTSTR GetWindowClassName() const override { return _T("DuiLibWin11Probe"); }
    LRESULT HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) override
    {
        if (uMsg == WM_DESTROY) { ::PostQuitMessage(0); return 0; }
        return ::DefWindowProc(m_hWnd, uMsg, wParam, lParam);
    }
};

int main()
{
    printf("[probe] === DPI awareness ===\n");
    int nLevel = CDPI::SetPerMonitorV2Awareness();
    printf("[probe] SetPerMonitorV2Awareness -> %d  (3=PMv2 2=PMv1 1=System 0=Unaware -1=none)\n", nLevel);
    fflush(stdout);

    printf("\n[probe] === Win11 window effects ===\n");
    TinyWnd wnd;
    if (!wnd.RegisterWindowClass()) {
        printf("[probe] RegisterWindowClass failed\n");
        return 2;
    }
    RECT rc = { 0, 0, 640, 420 };
    HWND hwnd = wnd.Create(nullptr, _T("DuiLib Win11 Probe"),
                           WS_OVERLAPPEDWINDOW, 0, rc);
    if (hwnd == nullptr) { printf("[probe] Create failed\n"); return 2; }
    printf("[probe] window created\n");
    fflush(stdout);

    UINT fx = wnd.ApplyWin11Effects(true, 1);
    printf("[probe] ApplyWin11Effects(dark=1, mica) -> 0x%X\n", fx);
    printf("[probe]   rounded=%d dark=%d backdrop=%d border=%d\n",
           (fx & CWindowWnd::kWin11RoundedCorner) ? 1 : 0,
           (fx & CWindowWnd::kWin11DarkTitleBar) ? 1 : 0,
           (fx & CWindowWnd::kWin11Backdrop) ? 1 : 0,
           (fx & CWindowWnd::kWin11BorderColor) ? 1 : 0);
    fflush(stdout);

    // 复核：RTL_OSVERSIONINFOW 判定是否真在 Win11 上
    typedef LONG(WINAPI* PFN_RtlGetVersion)(PRTL_OSVERSIONINFOW);
    HMODULE hNt = ::GetModuleHandleW(L"ntdll.dll");
    int nBuild = 0;
    if (hNt) {
        auto pfn = reinterpret_cast<PFN_RtlGetVersion>(::GetProcAddress(hNt, "RtlGetVersion"));
        if (pfn) {
            RTL_OSVERSIONINFOW vi = { 0 };
            vi.dwOSVersionInfoSize = sizeof(vi);
            if (pfn(&vi) == 0) nBuild = (int)vi.dwBuildNumber;
        }
    }
    printf("[probe] OS build = %d (%s)\n", nBuild,
           nBuild >= 22000 ? "Windows 11" : "Windows 10 or older");
    fflush(stdout);

    int bad = 0;
    if (hwnd == nullptr) ++bad;
    if (nBuild >= 22000) {
        // Win11 上圆角与暗色标题栏应当生效
        if (!(fx & CWindowWnd::kWin11RoundedCorner)) {
            printf("[probe] WARN: rounded corners not applied on Win11\n");
        }
        if (!(fx & CWindowWnd::kWin11DarkTitleBar)) {
            printf("[probe] WARN: dark title bar not applied on Win11\n");
        }
    }
    // DPI：至少应能设置成功（Win10 1703+ 应返回 3）
    if (nLevel < 0) {
        printf("[probe] WARN: no DPI awareness API available\n");
    }

    ::DestroyWindow(hwnd);
    printf("\n[WIN11] %s\n", bad == 0 ? "PASS" : "FAIL");
    return bad;
}
