#pragma once
// ============================================================
// [R16][T10] 工具箱弹窗渲染器拆分的内部共享设施（仅实现层，非公共契约）。
// 仅供 ToolDialogPage.cpp 与 ToolDlgBuild_*.cpp include；
// 禁止被 Page_Tools.cpp / FrameWnd 等宿主侧引用。
// 内容：后台作业统一结果信封/PostMessage Sink、模态进度窗、
//       链接标签控件、表格行单元格写值助手（自 ToolDialogPage.cpp 原样搬迁）。
// ============================================================
#include "ToolDialogPage.h"            // CDuiString / TDialogWimIndex / toolsvc 前向声明
#include "FrameWnd_Messages.h"         // WM_TDLG_* / WM_TP_* 消息号注册表
#include "ThemeTokens.h"             // tokens::Token / EToken (self-contained use below)
#include "BackgroundJobs.h"            // TJobDoneEnvelope（[R16][T9] 统一作业信封）
#include "WinHandles.h"                // [C++17 R11] win::GdiObject RAII for F11_raw_handle_close
#include "ToolService/IIsoImageService.h" // toolsvc::IToolJobSink（ToolTypes.h）
// ---- CTdlgCheck：弹窗复选控件（自 ToolDialogPage.cpp 原样搬迁，全局类型）----
class CTdlgCheck : public CTextUI
{
public:
    void Setup(const wchar_t* pszLabel, bool bChecked)
    {
        m_sLabel = pszLabel;
        m_bChecked = bChecked;
        SetFont(1);
        SetTextColor(tokens::Token(tokens::EToken::Ink));
        RECT rc = { 0, 0, 26, 0 };
        SetTextPadding(rc);
        // CTextUI 默认 DT_WORDBREAK：窄格会把复选框与文字折成两行
        SetAttribute(L"valign", L"vcenter");
        SetAttribute(L"endellipsis", L"true");
        Sync();
    }



    // [C++17 R3] [[nodiscard]] 返回勾选态，勿忽略
    [[nodiscard]] bool Checked() const { return m_bChecked; }



    virtual void DoEvent(TEventUI& event)
    {
        if (event.Type == UIEVENT_BUTTONUP)
        {
            m_bChecked = !m_bChecked;
            Sync();
        }
        CTextUI::DoEvent(event);
    }



private:
    void Sync()
    {
        CDuiString s;
        s.Format(L"%c %s",
                 m_bChecked ? (wchar_t)0x2611 : (wchar_t)0x2610,
                 m_sLabel.GetData());
        SetText(s);
    }



    bool       m_bChecked;
    CDuiString m_sLabel;
};
namespace tdlg {
// [C++17 R3] DPI 基准常量：TP_X 宏的 96 分基准，替代魔法数字
inline constexpr int kDpiBase = 96;
// [2026-10-12] 后台映像任务结果（工作线程 new，UI 线程 OnJobDone 处理后 delete）
// [R16-T9] 派生统一信封：nKind/bOk/sMsg（原 sErrMsg）由信封承载，
// 异构字段（脏位/关闭/引导链）留在本派生结构（最小公共集约定）。
struct TJobResult : TJobDoneEnvelope
{
    TJobResult()
        : bDirty(false), bCloseOnOk(false),
          bBootAttempted(false), bBootOk(false) {}
    bool        bDirty;         // ImageX 列表需重读
    bool        bCloseOnOk;     // 成功后关闭弹窗
    CDuiString  sTitle;
    CDuiString  sOkMsg;        // 成功弹窗文本（成功必弹）
    // 释放专用：wimgapi 成功但引导链失败时给修复指引
    bool        bBootAttempted;
    bool        bBootOk;
    CDuiString  sBootErr;
};
// 工作线程 → 宿主日志面板（AppendLogEx 非线程安全，必须回 UI 线程写）
static void TdJobLog(HWND hDlg, int nLevel, const CDuiString& sLine)
{
    if (!hDlg || !::IsWindow(hDlg)) return;
    auto p = std::make_unique<CDuiString>(sLine); // [C++17 R3] ownership to UI receiver on successful post
    if (::PostMessageW(hDlg, WM_TDLG_JOBLOG, static_cast<WPARAM>(nLevel), reinterpret_cast<LPARAM>(p.get())))
        p.release(); // [C++17 R3] receiver deletes; unique_ptr self-deletes when post fails
}
// 工作线程 → 进度窗切换阶段文字（如"正在注入驱动…"）
static void TdJobStage(HWND hProg, const CDuiString& sText)
{
    if (!hProg || !::IsWindow(hProg)) return;
    auto p = std::make_unique<CDuiString>(sText); // [C++17 R3] ownership to UI receiver on successful post
    if (::PostMessageW(hProg, WM_TP_STAGETEXT, 0, reinterpret_cast<LPARAM>(p.get())))
        p.release(); // [C++17 R3] receiver deletes; unique_ptr self-deletes when post fails
}
// [R16-T9] 统一 IToolJobSink→PostMessage 实现（原 TImgxSink/TApplySink 两套
// 逐字相同的本地定义收编为一）：hDlg=nullptr 即不记日志、hProg=nullptr 即无进度/
// 阶段；工作线程内只 PostMessage，绝不触碰 UI 控件（R7 契约）。
struct TPostJobSink : toolsvc::IToolJobSink
{
    HWND hDlg = nullptr, hProg = nullptr;
    void OnProgress(int nPercent, const std::wstring&) override
    {
        if (hProg && ::IsWindow(hProg))
            ::PostMessageW(hProg, WM_TP_PROGRESS, static_cast<WPARAM>(nPercent), static_cast<LPARAM>(0));
    }
    void OnLog(int nLevel, const std::wstring& sLine) override
    { TdJobLog(hDlg, nLevel, CDuiString(sLine.c_str())); }
    void OnStage(const std::wstring& sText) override
    { TdJobStage(hProg, CDuiString(sText.c_str())); }
};
// ============================================================
// [2026-10-12] 模态进度窗：禁用属主（真模态）+ 文字 + 0-100% 进度条
//   UI 线程创建；工作线程 PostMessage WM_TP_PROGRESS/WM_TP_STAGETEXT；
//   结束时 SendMessage(WM_CLOSE) 恢复并销毁属主。无 WS_SYSMENU，不可中途关闭。
// [B4] 高 DPI 修复：原窗口/控件全部固定物理像素，字体却按 DPI 放大，
//   125% 缩放下两行文字被裁成乱码横线、百分比贴边、进度条出界。
//   现统一按属主 DC 的 DPI 缩放全部坐标；客户区白底 + STATIC 透明背景，
//   与 DuiLib 工具箱对话框观感一致。
// ============================================================
class CTpProgressWnd
{
public:
    static HWND Create(HWND hOwner, const CDuiString& sText, bool bMarquee)
    {
        static const wchar_t* kCls = L"DismTool_TaskProgressWnd";
        static bool s_bReg = false;
        if (!s_bReg)
        {
            WNDCLASSW wc = { 0 };
            wc.lpfnWndProc   = &CTpProgressWnd::StaticProc;
            wc.hInstance     = ::GetModuleHandle(nullptr);
            wc.hCursor       = ::LoadCursor(nullptr, IDC_WAIT);
            wc.hbrBackground = (HBRUSH)::GetStockObject(WHITE_BRUSH);  // [B4] 白底
            wc.lpszClassName = kCls;
            ::RegisterClassW(&wc);
            s_bReg = true;
        }
        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS };
        InitCommonControlsEx(&icc);
        // [B4] 以属主 DC 实际 DPI 为缩放基准（96 下 MulDiv 为恒等）
        int nDpi = 96;
        HDC hdcOwn = ::GetDC(hOwner);
        if (hdcOwn)
        {
            nDpi = ::GetDeviceCaps(hdcOwn, LOGPIXELSY);
            ::ReleaseDC(hOwner, hdcOwn);
        }
        #define TP_X(v) ::MulDiv((v), nDpi, tdlg::kDpiBase)
        const int cxCfg = TP_X(440), cyCfg = TP_X(172);
        RECT rc = { 0 };
        ::GetWindowRect(hOwner, &rc);
        int nX = rc.left + ((rc.right - rc.left) - cxCfg) / 2;
        int nY = rc.top  + ((rc.bottom - rc.top) - cyCfg) / 2;
        HWND h = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kCls,
            L"任务进行中",
            WS_POPUP | WS_CAPTION,
            nX, nY, cxCfg, cyCfg, hOwner, nullptr,
            ::GetModuleHandle(nullptr), nullptr);
        if (!h) return nullptr;
        ::SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)hOwner);
        HFONT hFont = ::CreateFontW(
            -TP_X(15),
            0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        ::SetPropW(h, L"TpFont", reinterpret_cast<HANDLE>(hFont));
        // 两行阶段文案：高 48@96，DPI 缩放后完整容纳
        HWND hLbl = ::CreateWindowExW(0, L"STATIC", sText.GetData(),
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            TP_X(20), TP_X(16), TP_X(400), TP_X(48), h, (HMENU)1001,
            ::GetModuleHandle(nullptr), nullptr);
        ::SendMessageW(hLbl, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
        // 进度条与百分比同一行
        HWND hPct = ::CreateWindowExW(0, L"STATIC", L"0%",
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            TP_X(384), TP_X(99), TP_X(40), TP_X(18), h, (HMENU)1003,
            ::GetModuleHandle(nullptr), nullptr);
        ::SendMessageW(hPct, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
        DWORD dwBar = WS_CHILD | WS_VISIBLE | PBS_SMOOTH
                    | (bMarquee ? PBS_MARQUEE : 0);
        HWND hBar = ::CreateWindowExW(0, PROGRESS_CLASSW, nullptr, dwBar,
            TP_X(20), TP_X(102), TP_X(352), TP_X(12), h, (HMENU)1002,
            ::GetModuleHandle(nullptr), nullptr);
        if (!bMarquee)
        {
            ::SendMessageW(hBar, PBM_SETRANGE32, 0, 100);
            ::SendMessageW(hBar, PBM_SETPOS, 0, 0);
        }
        else
        {
            ::SendMessageW(hBar, PBM_SETMARQUEE, TRUE, 30);
        }
        #undef TP_X
        // 真模态：禁用属主窗口（不阻塞消息循环，进度条照常重绘）
        if (hOwner && ::IsWindow(hOwner))
            ::EnableWindow(hOwner, FALSE);
        ::ShowWindow(h, SW_SHOW);
        ::UpdateWindow(h);
        return h;
    }
private:
    static LRESULT CALLBACK StaticProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
    {
        switch (m)
        {
        case WM_TP_PROGRESS:
        {
            HWND hBar = ::GetDlgItem(h, 1002);
            HWND hPct = ::GetDlgItem(h, 1003);
            int n = static_cast<int>(wp);
            if (n < 0) n = 0;
            if (n > 100) n = 100;
            if (hBar) ::SendMessageW(hBar, PBM_SETPOS, n, 0);
            if (hPct)
            {
                std::wstring sPct = std::to_wstring(n) + L"%";
                ::SetWindowTextW(hPct, sPct.c_str());
            }
            return 0;
        }
        case WM_TP_STAGETEXT:
        {
            std::unique_ptr<CDuiString> guard(reinterpret_cast<CDuiString*>(lp)); // [C++17 R3] ownership from poster
            CDuiString* p = guard.get();
            if (p)
            {
                HWND hLbl = ::GetDlgItem(h, 1001);
                if (hLbl) ::SetWindowTextW(hLbl, p->GetData());
                // [C++17 R3] receiver-side delete happens at guard scope exit
            }
            return 0;
        }
        case WM_CTLCOLORSTATIC:
        {
            // [B4] STATIC 透明文字 + 白/透明背景，消除灰底方块
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
            {
                win::GdiObject hFont(reinterpret_cast<HGDIOBJ>(
                    reinterpret_cast<uintptr_t>(::RemovePropW(h, L"TpFont")))); // [C++17 R11] RAII DeleteObject
            } // [C++17 R11] DeleteObject runs before DestroyWindow (same order as before)
            ::DestroyWindow(h);
            return 0;
        }
        default:
            return ::DefWindowProcW(h, m, wp, lp);
        }
    }
};
    // 带下划线的链接文字（编辑映像文件底部 5 项）
    class CLinkLabelUI : public CTextUI
    {
    public:
        virtual void DoEvent(TEventUI& event)
        {
            if (event.Type == UIEVENT_BUTTONUP && m_pManager != nullptr)
                m_pManager->SendNotify(this, DUI_MSGTYPE_CLICK);
            CTextUI::DoEvent(event);
        }
    };
    inline void SetRowCell(CHorizontalLayoutUI* pRow, int c, const wchar_t* pszText)
    {
        if (!pRow || c < 0 || c >= static_cast<int>(pRow->GetCount())) return;
        CControlUI* p = pRow->GetItemAt(c);
        if (lstrcmpiW(p->GetClass(), L"TextUI") == 0)
            static_cast<CTextUI*>(p)->SetText(pszText);
    }
    // [R127-A] 重新读取 WIM/ESD 索引并刷新表格。
    //   EsdWizReload / MountReload 原是两份逐字相同的 16 行实现
    //   （仅成员名与文案键不同），提取于此。
    //   ImageXReload 不并入：它返回 bool 且在非弹窗路径写 HostLog，语义不同。
    //   返回是否成功读到了非空索引表。
    inline bool ReloadWimIndex(IToolHost* pHost, HWND hWnd, bool bAlert,
                               const CDuiString& sFile,
                               std::vector<TDialogWimIndex>& vIdx,
                               const wchar_t* pszKeyNoSrc,
                               const wchar_t* pszKeyNoIdx,
                               const wchar_t* pszKeyTitle)
    {
        vIdx.clear();
        CDuiString sErr;
        if (sFile.IsEmpty())
        {
            if (bAlert)
                ::MessageBoxW(hWnd, pszKeyNoSrc, pszKeyTitle, MB_OK | MB_ICONWARNING);
            return false;
        }
        if (!pHost->HostQueryWim(sFile, vIdx, &sErr)
            || vIdx.empty())
        {
            if (bAlert)
            {
                CDuiString sMsg = pszKeyNoIdx;
                if (!sErr.IsEmpty()) { sMsg += L"\r\n\r\n"; sMsg += sErr; }
                ::MessageBoxW(hWnd, sMsg.GetData(), pszKeyTitle, MB_OK | MB_ICONWARNING);
            }
            return false;
        }
        return true;
    }
    // [R127-A] WIM 索引表 → 网格行的逐格填充。
    //   EsdWizFillRows / MountFillRows 原是两份逐字相同的 17 行实现
    //   （仅索引数组成员不同），提取于此，两份改为转发。
    //   列约定：0=Index 1=Desc(空则 Name) 2=Version 3=Created；越界行清空。
    inline void FillWimIndexRows(const std::vector<CHorizontalLayoutUI*>& vRows,
                                 const std::vector<TDialogWimIndex>& vIdx)
    {
        for (int i = 0; i < static_cast<int>(vRows.size()); ++i)
        {
            if (i < static_cast<int>(vIdx.size()))
            {
                const TDialogWimIndex& ix = vIdx[i];
                CDuiString sIdx;
                sIdx.Format(L"%d", ix.nIndex);
                SetRowCell(vRows[i], 0, sIdx.GetData());
                SetRowCell(vRows[i], 1,
                           ix.sDesc.IsEmpty() ? ix.sName.GetData() : ix.sDesc.GetData());
                SetRowCell(vRows[i], 2, ix.sVersion);
                SetRowCell(vRows[i], 3, ix.sCreated);
            }
            else
            {
                for (int c = 0; c < 4; ++c) SetRowCell(vRows[i], c, L"");
            }
        }
    }
} // namespace tdlg
