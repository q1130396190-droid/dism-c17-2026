#pragma once

// ============================================================
// Page_ToolsJobs.h —— 工具箱页后台作业的进度窗与进度桥（跨编译单元共享）。
//
// [R130-1] 从 Page_Tools.cpp 抽出 169 行作业基础设施：
//   CJobProgressWnd  通用模态进度窗（SWM 合并 / WIM-ESD 互转 / ESD→ISO 共用）
//   JobProgPost      工作线程侧进度投递
//   TJobWimProgress  wimgapi 进度 → 总进度映射
//
// 闭包证明：m_ctx 0 处、CPageTools:: 0 处、HostNotify/HostConfirm 0 处，
// 仅依赖 Win32 / comctl32 / WIMBridge / StringMgr，为自包含单元。
//
// 段内 JP_X 宏成对 #define/#undef，作用域自闭合，不外泄到包含方。
// ============================================================

#include "stdafx.h"

#include <memory>
#include <string>

#include <commctrl.h>

#include "WmMessages.h"        // JOBWM_PROGRESS
#include "MsgParam.h"          // msgparam::ToParam / FromParam
#include "WIMHelperBridge.h"   // WIMBridge::IWimProgress
#include "StringManager.h"

namespace ToolJobs
{

// 进度窗控件 ID（模板与 WM_CTLCOLORSTATIC 共用）
inline constexpr int kJobProgLabelId = 1001;
inline constexpr int kJobProgBarId   = 1002;
inline constexpr int kJobProgPctId   = 1003;

class CJobProgressWnd
{
public:
    [[nodiscard]] static HWND Create(HWND hOwner, const std::wstring& sText)
    {
        static const wchar_t* kCls = L"DismTool_JobProgressWnd_PT";
        static bool s_bReg = false;
        if (!s_bReg)
        {
            WNDCLASSW wc = { 0 };
            wc.lpfnWndProc   = &CJobProgressWnd::StaticProc;
            wc.hInstance     = ::GetModuleHandle(nullptr);
            wc.hCursor       = ::LoadCursor(nullptr, IDC_WAIT);
            wc.hbrBackground = (HBRUSH)::GetStockObject(WHITE_BRUSH);
            wc.lpszClassName = kCls;
            ::RegisterClassW(&wc);
            s_bReg = true;
        }

        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS };
        InitCommonControlsEx(&icc);

        int nDpi = 96;
        HDC hdcOwn = ::GetDC(hOwner);
        if (hdcOwn)
        {
            nDpi = ::GetDeviceCaps(hdcOwn, LOGPIXELSY);
            ::ReleaseDC(hOwner, hdcOwn);
        }
        #define JP_X(v) ::MulDiv((v), nDpi, 96)

        RECT rc = { 0 };
        ::GetWindowRect(hOwner, &rc);
        const int cxCfg = JP_X(440), cyCfg = JP_X(172);
        int nX = rc.left + ((rc.right - rc.left) - cxCfg) / 2;
        int nY = rc.top  + ((rc.bottom - rc.top) - cyCfg) / 2;

        HWND h = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kCls,
            StringMgr.Get("toolbox.ui_task_running"),
            WS_POPUP | WS_CAPTION,
            nX, nY, cxCfg, cyCfg, hOwner, nullptr,
            ::GetModuleHandle(nullptr), nullptr);
        if (!h) return nullptr;

        ::SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)hOwner);

        HFONT hFont = ::CreateFontW(
            -JP_X(15),
            0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        ::SetPropW(h, L"JpFont", reinterpret_cast<HANDLE>(hFont));

        HWND hLbl = ::CreateWindowExW(0, L"STATIC", sText.c_str(),
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            JP_X(20), JP_X(16), JP_X(400), JP_X(48), h, (HMENU)1001,
            ::GetModuleHandle(nullptr), nullptr);
        ::SendMessageW(hLbl, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);

        HWND hPct = ::CreateWindowExW(0, L"STATIC", L"0%",
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            JP_X(384), JP_X(99), JP_X(40), JP_X(18), h, (HMENU)1003,
            ::GetModuleHandle(nullptr), nullptr);
        ::SendMessageW(hPct, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);

        HWND hBar = ::CreateWindowExW(0, PROGRESS_CLASSW, nullptr,
            WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
            JP_X(20), JP_X(102), JP_X(352), JP_X(12), h, (HMENU)1002,
            ::GetModuleHandle(nullptr), nullptr);
        ::SendMessageW(hBar, PBM_SETRANGE32, 0, 100);
        ::SendMessageW(hBar, PBM_SETPOS, 0, 0);

        // [R104] 取消按钮。原进度窗是 WS_POPUP|WS_CAPTION 且无关闭按钮，
        //   用户看着长任务跑却毫无办法 —— R103 审计把它记为"取消接口不统一"，
        //   实际上比那更糟：**根本没有取消入口**。
        //   按钮通过 JOBWM_CANCEL_REQ 把请求转达给作业（作业侧查 CancelToken）。
        HWND hCancel = ::CreateWindowExW(0, L"BUTTON",
            StringMgr.Get("toolbox.ui_btn_cancel"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            JP_X(352), JP_X(130), JP_X(72), JP_X(26), h, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCancel)),
            ::GetModuleHandle(nullptr), nullptr);
        if (hCancel)
            ::SendMessageW(hCancel, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
        #undef JP_X

        if (hOwner && ::IsWindow(hOwner))
            ::EnableWindow(hOwner, FALSE);

        ::ShowWindow(h, SW_SHOW);
        ::UpdateWindow(h);
        return h;
    }

private:
    // [R104] 控件 ID：1001 阶段文案 / 1002 进度条 / 1003 百分比 / 1004 取消
    static constexpr int kIdStage  = 1001;
    static constexpr int kIdBar    = 1002;
    static constexpr int kIdPct    = 1003;
    static constexpr int kIdCancel = 1004;

    static LRESULT CALLBACK StaticProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
    {
        switch (m)
        {
        // [R104] 取消按钮：把请求转达给 owner（页面），由页面置位 CancelToken。
        //   进度窗自己不持有令牌 —— 令牌属于作业，窗口只是 UI，不该成为状态源。
        case WM_COMMAND:
        {
            if (LOWORD(wp) == kIdCancel && HIWORD(wp) == BN_CLICKED)
            {
                HWND hOwnerWnd = reinterpret_cast<HWND>(
                    ::GetWindowLongPtrW(h, GWLP_USERDATA));
                if (hOwnerWnd && ::IsWindow(hOwnerWnd))
                    ::PostMessageW(hOwnerWnd, JOBWM_CANCEL_REQ, 0, 0);
                // 视觉反馈：禁用按钮并改文案，避免用户连点
                HWND hBtn = ::GetDlgItem(h, kIdCancel);
                if (hBtn)
                {
                    ::EnableWindow(hBtn, FALSE);
                    ::SetWindowTextW(hBtn, StringMgr.Get("general.cancelled"));
                }
                return 0;
            }
            // 非取消按钮的 WM_COMMAND（例如子控件自身的通知）本窗口不处理，
            // 交回默认处理——与下面 default 分支同一语义（DefWindowProcW 对
            // 未处理的 WM_COMMAND 返回 0），避免函数走到末尾无返回值。
            return ::DefWindowProcW(h, m, wp, lp);
        }
        case JOBWM_PROGRESS:
        {
            std::unique_ptr<std::wstring> spStage(
                msgparam::FromParam<std::wstring>(static_cast<LPARAM>(lp)));
            std::wstring* pStage = spStage.get();
            if (pStage)
            {
                HWND hLbl = ::GetDlgItem(h, 1001);
                if (hLbl) ::SetWindowTextW(hLbl, pStage->c_str());
            }
            int n = static_cast<int>(wp);
            if (n < 0) n = 0;
            if (n > 100) n = 100;
            HWND hBar = ::GetDlgItem(h, 1002);
            HWND hPct = ::GetDlgItem(h, 1003);
            if (hBar) ::SendMessageW(hBar, PBM_SETPOS, n, 0);
            if (hPct)
            {
                std::wstring sPct = std::to_wstring(n) + L"%";
                ::SetWindowTextW(hPct, sPct.c_str());
            }
            return 0;
        }
        case WM_CTLCOLORSTATIC:
        {
            HDC hdc = (HDC)wp;
            ::SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)::GetStockObject(WHITE_BRUSH);
        }
        case WM_CLOSE:
        {
            HWND hOwner = (HWND)::GetWindowLongPtrW(h, GWLP_USERDATA);
            if (hOwner && ::IsWindow(hOwner))
            {
                ::EnableWindow(hOwner, TRUE);
                ::SetForegroundWindow(hOwner);
            }
            HFONT hFont = (HFONT)::RemovePropW(h, L"JpFont");
            if (hFont) ::DeleteObject(hFont);
            ::DestroyWindow(h);
            return 0;
        }
        default:
            return ::DefWindowProcW(h, m, wp, lp);
        }
    }
};

// 工作线程侧：投递百分比/阶段文案（pStage 为空只更新百分比）。
static void JobProgPost(HWND hProg, DWORD dwPercent,
                        std::unique_ptr<std::wstring> spStage = nullptr)
{
    if (!hProg || !::IsWindow(hProg)) return;
    std::unique_ptr<std::wstring> p = spStage
        ? std::make_unique<std::wstring>(*spStage)
        : nullptr;
    if (::PostMessageW(hProg, JOBWM_PROGRESS, static_cast<WPARAM>(dwPercent),
                       msgparam::ToParam(p.get())))
        p.release();
}

// [rb28] wimgapi 进度接收器：把单次 0-100 映射到任务总进度 [dwLo,dwHi]。
struct TJobWimProgress : public WIMBridge::IWimProgress
{
    HWND  hWnd;
    DWORD dwLo;
    DWORD dwHi;
    DWORD dwLast;

    TJobWimProgress(HWND h, DWORD lo, DWORD hi)
        : hWnd(h), dwLo(lo), dwHi(hi), dwLast(0xFFFFFFFF) {}

    void OnWimProgress(DWORD dwPercent, DWORD /*dwMsgId*/) override
    {
        if (dwPercent > 100) dwPercent = 100;
        DWORD v = dwLo + (DWORD)((static_cast<ULONGLONG>(dwHi - dwLo) * dwPercent) / 100ULL);
        if (v > 100) v = 100;
        if (v == dwLast) return;
        dwLast = v;
        if (hWnd && ::IsWindow(hWnd))
            ::PostMessageW(hWnd, JOBWM_PROGRESS, static_cast<WPARAM>(v), static_cast<LPARAM>(0));
    }
};

} // namespace ToolJobs
