#include "stdafx.h"
#include "ThemeTokens.h"   // [R201] 强调色令牌（Win10/11 均跟随系统主题色）

// [R16][T10] 工具箱弹窗按 kind 拆分的渲染器分组文件（自 ToolDialogPage.cpp 物理搬迁，
// 零行为改动）。公共壳（Chrome/通用控件工厂/消息路由/作业框架）仍在 ToolDialogPage.cpp；
// 跨组共享的内部设施（tdlg:: 作业 Sink/进度窗/行单元格助手）见 ToolDlgInternal.h。

#include "ToolDialogPage.h"
#include "ToolDlgInternal.h"
#include "WinHandles.h"   // [H-51] win::GdiObject RAII for F11_raw_handle_close

#include "SafeThread.h"      // 工作线程异常边界
#include "BackgroundJobs.h"  // 后台作业统一收口（SEH 边界+退出等待）
#include "WmMessages.h"      // [FIX-P2M] WM_TDLG_TASKDONE（后台应用完成收口）

// [R6] Service 层（零 UI 业务）
#include "ToolService/IToolServiceHost.h"
#include "ToolService/ToolServiceFactory.h"
#include "ToolService/LogDepGraphService.h"
#include "ToolService/ListService.h"
#include "ToolService/ConfirmService.h"
#include "ToolService/IsoMount.h"          // ISO 直连挂载原语（virtdisk）
#include "ToolService/IIsoImageService.h"  // wimgapi 作业工厂
#include "ToolService/WizDiskPart.h"       // diskpart/ESP 原语
#include "ToolService/IWizardService.h"    // 向导业务（BootFix…）

#include "DependencyGraphService.h"
#include "StringManager.h"
#include "LogService.h"        // 清空日志时同步内存审计环
#include "HostInterfaces.h"   // static_cast<int>(ELogLevel::LOG_ERROR)/static_cast<int>(ELogLevel::LOG_WARN) 分级常量
#include "SystemInfoService.h"

#include <algorithm>
#include <set>
#include <cwchar>
#include <cwctype>
#include <lm.h>
#include <cstdio>
#include <string>
#include <shlwapi.h>
#include <thread>
#include <winioctl.h>

using i18n::CStringManager;

#pragma comment(lib, "netapi32.lib")
void CToolDialogPage::ClickDetails(const CDuiString& sName)
{
    if (sName == L"btn_det_ok") DetailsOk();
}

void CToolDialogPage::ClickLogView(const CDuiString& sName)
{
    if (sName == L"btn_logview_refresh") LogViewRefresh();
    else if (sName == L"btn_logview_open") LogViewOpenFolder();
    else if (sName == L"btn_logview_clear") LogViewClear();
    else if (sName == L"btn_logview_copy") LogViewCopySelection();
    else if (sName == L"btn_logview_all")  LogViewSelectAll();
    // [L-10] 过滤 / 搜索 / 跟随 / 导出
    else if (sName == L"btn_logview_follow") LogViewToggleFollow();
    else if (sName == L"btn_logview_export") LogViewExport();
}

void CToolDialogPage::ClickDepGraph(const CDuiString& sName)
{
    if (sName == L"btn_depgraph_refresh") DepGraphRefresh();
    else if (sName == L"btn_depgraph_copy") DepGraphCopyAll();
}

void CToolDialogPage::ClickLinkList(const CDuiString& sName)
{
    if (sName.Left(8) == L"link_") LinkListOpen(sName);
}

void CToolDialogPage::BuildDetails()
{
    struct TOpt { const wchar_t* key; const wchar_t* label; };
    static const TOpt kOpts[] = {
        { L"ExpertMode",     StringMgr.Get("misc.detail_expert") },
        { L"LoadWofAdk",     StringMgr.Get("misc.detail_wofadk") },
        { L"RightClickMenu", StringMgr.Get("misc.detail_ctxmenu") },
        { L"IntegrateRE",    StringMgr.Get("misc.detail_integre") },
        { L"DeltaTransfer",  StringMgr.Get("misc.detail_delta") },
    };
    constexpr int nCount = static_cast<int>(sizeof(kOpts) / sizeof(kOpts[0]));

    CVerticalLayoutUI* pDesc = std::make_unique<CVerticalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pDesc->SetFixedHeight(78);
    pDesc->SetAttribute(L"inset", L"2,4,2,4");
    CTextUI* pNote = MkText(StringMgr.Get("misc.detail_hint"), 1, 0xFF606060);
    pNote->SetTextPadding(RECT{ 0, 0, 0, 0 });
    pDesc->Add(pNote);
    m_pBody->Add(pDesc);

    CControlUI* pGap0 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pGap0->SetFixedHeight(8);
    m_pBody->Add(pGap0);

    CVerticalLayoutUI* pBox = std::make_unique<CVerticalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pBox->SetAttribute(L"bordercolor", tokens::TokenHex(tokens::EToken::Border));
    pBox->SetAttribute(L"bordersize", L"1");
    pBox->SetBkColor(tokens::Token(tokens::EToken::Surface));
    pBox->SetAttribute(L"inset", L"20,16,20,16");

    // [R6-03] 5 开关当前值由服务从 config.ini[Advanced] 读取。
    bool vFlags[toolsvc::CConfirmService::kAdvancedCount] = {};
    SvcConfirm()->LoadAdvancedFlags(vFlags);

    for (int i = 0; i < nCount; ++i)
    {
        bool bOn = vFlags[i];

        CTdlgCheck* pChk = std::make_unique<CTdlgCheck>().release(); // [C++17 R3] framework-owned via pBox->Add
        pChk->Setup(kOpts[i].label, bOn);
        CDuiString sName;
        sName.Format(L"chk_detail_%d", i);
        pChk->SetName(sName);
        pChk->SetFixedHeight(26);
        pChk->SetAttribute(L"endellipsis", L"false");
        pBox->Add(pChk);

        if (i < nCount - 1)
        {
            CControlUI* pGap = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pBox->Add
            pGap->SetFixedHeight(6);
            pBox->Add(pGap);
        }
    }

    m_pBody->Add(pBox);

    CControlUI* pGapL = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pGapL->SetMinHeight(16);
    m_pBody->Add(pGapL);

    AddBottomRow(MkBtn(StringMgr.Get("general.ok"), L"btn_det_ok", true),
                 MkBtn(StringMgr.Get("general.cancel"), L"btn_cancel", false));
}

void CToolDialogPage::DetailsOk()
{
    // [FIX-P2M] 在途防重入：后台应用期间 OK/遮罩重复触发直接忽略
    if (m_bAdvancedApplying) return;
    // [R6-03] 仅收集 5 个勾选态；config.ini 写入与 LoadWofAdk/右键菜单/
    // 整合RE/差异传输 4 个系统副作用全部下沉 CConfirmService::ApplyAdvanced。
    // [FIX-P2M] ApplyAdvanced 含整合 RE 三条 reagentc（最坏 ~7 分钟），
    // 不再在 UI 线程同步执行：遮罩(marquee) + BackgroundJobs 后台应用，
    // 完成后 WM_TDLG_TASKDONE(wParam=2) 解遮罩并 Close(IDOK)。
    // 线程期间对话框存活（遮罩挡输入），SvcConfirm()/m_pHost 访问安全。
    bool vIn[toolsvc::CConfirmService::kAdvancedCount] = {};
    for (int i = 0; i < toolsvc::CConfirmService::kAdvancedCount; ++i)
    {
        CDuiString sName;
        sName.Format(L"chk_detail_%d", i);
        CTdlgCheck* pChk =
            static_cast<CTdlgCheck*>(m_pm.FindControl(sName.GetData()));
        vIn[i] = (pChk && pChk->Checked());
    }
    m_bAdvancedApplying = true;
    BeginTaskShield(L"正在应用高级设置（整合 WinRE 可能需要数分钟），请稍候……",
                    true);
    struct DetOkCtx
    {
        CToolDialogPage* pDlg;
        bool vIn[toolsvc::CConfirmService::kAdvancedCount];
    };
    DetOkCtx* pCtx = new DetOkCtx{ this, {} };
    for (int i = 0; i < toolsvc::CConfirmService::kAdvancedCount; ++i)
        pCtx->vIn[i] = vIn[i];
    HANDLE hTh = BackgroundJobs::Start([](void* pv) -> DWORD {
        std::unique_ptr<DetOkCtx> guard(static_cast<DetOkCtx*>(pv));
        guard->pDlg->SvcConfirm()->ApplyAdvanced(guard->vIn);
        // wParam=2：DetailsOk 后台应用完成 → 解遮罩并关闭对话框
        ::PostMessageW(guard->pDlg->m_hWnd, WM_TDLG_TASKDONE, 2, 0);
        return 0;
    }, pCtx, "details-apply");
    if (!hTh)
    {
        m_bAdvancedApplying = false;
        EndTaskShield();
        delete pCtx;
        ::MessageBoxW(m_hWnd, L"启动应用任务失败。", StringMgr.Get("esdwiz.title"),
                      MB_OK | MB_ICONERROR);
    }
}

// [R6-03] Old DetailsOk config writes + 4 system side effects moved to ToolService/ConfirmService.cpp.

// ====================================================================
// R-B16: 查看日志（原版 DLGTEMPLATEEX ID134）
// 读取 EXE 同目录 action_log.jsonl，按行解析 JSON 展示、刷新、
// 打开所在目录、清空；级别着色（error/warn/info）。
// ====================================================================

// [R6-02] LogUtf8ToW 已随 JSON 解析迁入 ToolService/LogDepGraphService.cpp；
// 下方两个 GDI 文本测量函数属 UI 呈现（行宽/折行高度），保留在 Page 侧。

// [R-B81] 以下两个测量函数统一按 96 逻辑像素计算：动态对话框的固定尺寸先以
// 96 DPI 落地，InitWindow 末尾再由 CPaintManagerUI::SetDPI 整体等比放大，
// 因此这里算出的逻辑宽/高交给 SetFixedWidth/Height 后在任何宿主 DPI 下都正确。
// 字体与 ToolDialog.xml 中 id=1 的 Microsoft YaHei 12 保持一致。
[[nodiscard]] static int LogViewMeasureWidth96(const std::wstring& sText)
{
    if (sText.empty()) return 0;
    HDC hdc = ::GetDC(nullptr);
    int nDcX = ::GetDeviceCaps(hdc, LOGPIXELSX);
    int nDcY = ::GetDeviceCaps(hdc, LOGPIXELSY);
    win::GdiObject hf(::CreateFontW(-::MulDiv(12, nDcY, 72), 0, 0, 0, FW_NORMAL,
                            FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                            L"Microsoft YaHei")); // [C++17 R11] RAII DeleteObject
    HGDIOBJ hOld = ::SelectObject(hdc, hf.get());
    RECT rc = { 0, 0, 0, 0 };
    ::DrawTextW(hdc, sText.c_str(), -1, &rc,
                DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    ::SelectObject(hdc, hOld);
    hf.reset(); // [C++17 R11] DeleteObject at the original call site
    ::ReleaseDC(nullptr, hdc);
    return ::MulDiv(rc.right - rc.left, 96, nDcX);
}

// 按 nWidth96（96 逻辑像素下的文本宽度）用 DT_WORDBREAK 折行测量所需高度
[[nodiscard]] static int LogViewMeasureHeight96(const std::wstring& sText, int nWidth96)
{
    if (sText.empty() || nWidth96 <= 0) return 20;
    HDC hdc = ::GetDC(nullptr);
    int nDcX = ::GetDeviceCaps(hdc, LOGPIXELSX);
    int nDcY = ::GetDeviceCaps(hdc, LOGPIXELSY);
    win::GdiObject hf(::CreateFontW(-::MulDiv(12, nDcY, 72), 0, 0, 0, FW_NORMAL,
                            FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                            L"Microsoft YaHei")); // [C++17 R11] RAII DeleteObject
    HGDIOBJ hOld = ::SelectObject(hdc, hf.get());
    RECT rc = { 0, 0, ::MulDiv(nWidth96, nDcX, 96), 0 };
    ::DrawTextW(hdc, sText.c_str(), -1, &rc,
                DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    ::SelectObject(hdc, hOld);
    hf.reset(); // [C++17 R11] DeleteObject at the original call site
    ::ReleaseDC(nullptr, hdc);
    return ::MulDiv(rc.bottom - rc.top, 96, nDcY);
}

// [R6-02] LogJsonField / LogUnescape 已迁入
// ToolService/LogDepGraphService.cpp（ReadLogLines 内部）。

CDuiString CToolDialogPage::LogViewPath()
{
    // [R6-02] 路径计算在服务；本包装仅供打开目录等 UI 动作使用。
    return CDuiString(SvcLogDep()->LogPath().c_str());
}

void CToolDialogPage::BuildLogView()
{
    // 顶部：记录计数（动态更新）+ 操作提示（告知可拖选/右键复制，避免用户以为只读不可选）
    CVerticalLayoutUI* pDesc = std::make_unique<CVerticalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pDesc->SetFixedHeight(42);
    pDesc->SetAttribute(L"inset", L"2,1,2,1");
    CTextUI* pCnt = MkText(L"", 1, 0xFF1A1A1A);
    pCnt->SetName(L"logview_count");
    pCnt->SetFixedHeight(20);
    pCnt->SetTextPadding(RECT{ 0, 0, 0, 0 });
    pCnt->SetMouseEnabled(false);
    CTextUI* pCntFirst = pCnt;   // 构建末尾写占位文字用
    pDesc->Add(pCnt);
    CTextUI* pHint = MkText(StringMgr.Get("misc.logview_hint"), 1, 0xFF7A7A7A);
    pHint->SetFixedHeight(20);
    pHint->SetTextPadding(RECT{ 0, 0, 0, 0 });
    pHint->SetMouseEnabled(false);
    pDesc->Add(pHint);
    m_pBody->Add(pDesc);

    // [L-10] 工具条：关键字搜索 + 级别过滤 + 自动跟随
    //   这三项是 2026 主流日志查看器的基线能力，此前只有一个"刷新"按钮，
    //   几千行日志里找一条错误只能靠人眼翻。三个控件都走本页自绘/自管，
    //   不引入新皮肤文件，避免与多语言皮肤同步问题。
    CHorizontalLayoutUI* pTools = std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pTools->SetFixedHeight(30);
    pTools->SetAttribute(L"inset", L"2,2,2,2");

    CEditUI* pSearch = MkEdit(L"edt_logview_search");
    pSearch->SetFixedWidth(240);
    pSearch->SetAttribute(L"text", L"");
    pSearch->SetAttribute(L"tipvalue", StringMgr.Get("misc.logview_search_tip"));
    pSearch->SetAttribute(L"valign", L"vcenter");
    pTools->Add(pSearch);

    CControlUI* pGapT1 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pTools->Add
    pGapT1->SetFixedWidth(8);
    pTools->Add(pGapT1);

    // 级别过滤：全部 / 警告及以上 / 仅错误
    CComboUI* pFilter = MkCombo(L"cmb_logview_level", 90);
    pFilter->SetFixedWidth(150);
    pFilter->SetAttribute(L"valign", L"vcenter");
    {
        // 复用一个方向：索引 0/1/2 与服务层过滤条件一一对应
        CListLabelElementUI* p0 = new CListLabelElementUI();
        p0->SetText(StringMgr.Get("misc.logview_filter_all"));
        pFilter->Add(p0);
        CListLabelElementUI* p1 = new CListLabelElementUI();
        p1->SetText(StringMgr.Get("misc.logview_filter_warn"));
        pFilter->Add(p1);
        CListLabelElementUI* p2 = new CListLabelElementUI();
        p2->SetText(StringMgr.Get("misc.logview_filter_error"));
        pFilter->Add(p2);
    }
    pTools->Add(pFilter);

    CControlUI* pGapT2 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pTools->Add
    pGapT2->SetFixedWidth(8);
    pTools->Add(pGapT2);

    CButtonUI* pFollow = MkBtn(StringMgr.Get("misc.logview_follow"),
                              L"btn_logview_follow", false);
    pFollow->SetFixedWidth(
        LogViewMeasureWidth96(StringMgr.Get("misc.logview_follow")) + 56);
    pTools->Add(pFollow);

    CControlUI* pGapT3 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pTools->Add
    pGapT3->SetFixedWidth(8);
    pTools->Add(pGapT3);

    CButtonUI* pExport = MkBtn(StringMgr.Get("misc.logview_export"),
                              L"btn_logview_export", false);
    pExport->SetFixedWidth(
        LogViewMeasureWidth96(StringMgr.Get("misc.logview_export")) + 56);
    pTools->Add(pExport);

    // 右侧弹性：把上面几个控件压在左侧
    CControlUI* pSpT = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pTools->Add
    pTools->Add(pSpT);

    m_pBody->Add(pTools);

    CControlUI* pGap0 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pGap0->SetFixedHeight(4);
    m_pBody->Add(pGap0);

    // 中部：日志正文。用 CRichEditUI 而非逐行 CTextUI：
    //   逐行 CTextUI + SetMouseEnabled(false) 导致日志既不能选中也不能复制，
    //   RichEdit 原生支持拖选、Ctrl+A/Ctrl+C、右键菜单与纵向滚动。
    //   只读（readonly）保留，但只读不影响选中与复制。
    CRichEditUI* pView = std::make_unique<CRichEditUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pView->SetName(L"logview_text");
    pView->SetFont(1);
    pView->SetTextColor(tokens::Token(tokens::EToken::Ink));
    pView->SetBkColor(tokens::Token(tokens::EToken::Surface));
    pView->SetReadOnly(true);
    // 构造默认 m_bWordWrap=false 且 XML/属性不回传该位，必须代码显式打开，
    // 否则长路径行会横向溢出、看不到行尾。
    pView->SetWordWrap(true);
    pView->SetMultiLine(true);
    pView->SetWantReturn(true);
    // 允许 Ctrl+C / Ctrl+A 走控件自身；不拦截回车（只读文本回车无副作用）
    pView->SetAttribute(L"bordercolor", tokens::TokenHex(tokens::EToken::Border));
    pView->SetAttribute(L"bordersize", L"1");
    pView->SetAttribute(L"multiline", L"true");
    // 右键菜单交给本页自绘（复用已有多语言文案键），因此关掉框架内置菜单：
    // 内置菜单是硬编码 GBK 中文，与外层多语言不一致，且其 WM_COMMAND 分支
    // 要求控件已获焦，右键未聚焦时点菜单项会静默无效。
    pView->SetContextMenuUsed(false);
    pView->SetTextPadding(RECT{ 10, 8, 10, 8 });
    // [R-LOGV-1] 纵向滚动条：此前正文控件从未打开 vscrollbar，滑块对象
    //   CScrollBarUI 压根没被创建（m_pVerticalScrollBar == NULL），于是
    //   SetPos 里"有滑块才让出宽度、才显示滑块"的分支全被跳过 ——
    //   表现就是日志超出可视区后既没有滑块，也无法滚动。
    //   两条语句都必须有，各自解决一半问题：
    //   · Add 之前设：只写 m_lTwhStyle，让 DoInit 创建的 CREATESTRUCT
    //     带上 WS_VSCROLL（TextServices 据此按自动滚动布局）；
    //   · Add 之后再设：此时 m_pTwh 已存在，SetAttribute 会调
    //     TxEnableScrollBar → CContainerUI::EnableScrollBar 真正 new 出
    //     CScrollBarUI。注意 CRichEditUI::SetAttribute 对 vscrollbar
    //     是"拦截"而非"透传"，所以不能指望它自己走到基类的建对象逻辑。
    pView->SetAttribute(L"vscrollbar", L"true");
    m_pBody->Add(pView);
    pView->SetAttribute(L"vscrollbar", L"true");   // m_pTwh 已就绪：建出滑块对象
    // [R-LOGV-1] 上限从默认 32K 字符抬到 512K：
    //   cInitTextMax 是 RichEdit 1.0 的向后兼容残留值（32K-1），
    //   日志正文超过该长度会被静默截断（EM_EXLIMITTEXT 之后的内容丢失），
    //   这正是"日志明明有几千行、界面上却只到某一行为止"的直接原因。
    pView->SetLimitText(512 * 1024);

    CControlUI* pGapL = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    // 必须用固定高度：SetMinHeight 时 EstimateSize 仍返回 cy=0，会被
    // CVerticalLayoutUI 计入 nAdjustables，与日志正文平分剩余高度，
    // 表现就是正文下方多出一大块空白（该空白区域同样没有内容）。
    pGapL->SetFixedHeight(12);
    m_pBody->Add(pGapL);

    // 底部：左「复制选中 / 全选」，右「刷新 / 打开日志目录 / 清空」
    CHorizontalLayoutUI* pRow = std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pRow->SetFixedHeight(56);
    pRow->SetAttribute(L"inset", L"0,9,0,9");

    CButtonUI* pCopy = MkBtn(StringMgr.Get("misc.logview_copy"), L"btn_logview_copy", true);
    pCopy->SetFixedWidth(LogViewMeasureWidth96(StringMgr.Get("misc.logview_copy")) + 56);
    pRow->Add(pCopy);

    CControlUI* pGapC = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pRow->Add
    pGapC->SetFixedWidth(10);
    pRow->Add(pGapC);

    CButtonUI* pAll = MkBtn(StringMgr.Get("misc.hd935"), L"btn_logview_all", false);
    pAll->SetFixedWidth(LogViewMeasureWidth96(StringMgr.Get("misc.hd935")) + 56);
    pRow->Add(pAll);

    CControlUI* pSp = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pRow->Add
    pRow->Add(pSp);

    CButtonUI* pRefresh = MkBtn(
        StringMgr.Get("misc.logview_refresh"),
        L"btn_logview_refresh", false);
    // [R-B81] 宽度按各语言文案实测（MkBtn 左右文本内边距各 28，另留 12 余量），
    // 不再统一 130——此前"打开日志目录(O)"等较长文案右侧被裁。
    pRefresh->SetFixedWidth(
        LogViewMeasureWidth96(StringMgr.Get("misc.logview_refresh")) + 56);
    pRow->Add(pRefresh);

    CControlUI* pGap = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pRow->Add
    pGap->SetFixedWidth(10);
    pRow->Add(pGap);

    CButtonUI* pOpen = MkBtn(
        StringMgr.Get("misc.logview_open_folder"),
        L"btn_logview_open", false);
    pOpen->SetFixedWidth(
        LogViewMeasureWidth96(StringMgr.Get("misc.logview_open_folder")) + 56);
    pRow->Add(pOpen);

    CControlUI* pGap2 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pRow->Add
    pGap2->SetFixedWidth(10);
    pRow->Add(pGap2);

    CButtonUI* pClear = MkBtn(
        StringMgr.Get("misc.logview_clear"),
        L"btn_logview_clear", false);
    pClear->SetFixedWidth(
        LogViewMeasureWidth96(StringMgr.Get("misc.logview_clear")) + 56);
    pRow->Add(pClear);

    m_pBody->Add(pRow);

    // 不在构建期同步读日志：日志可能上千行、数百 KB，同步解析 + RichEdit
    // 排版会把模态窗口的创建整个卡住（实测 >40s 窗口都不出现）。
    // 这里只写占位文字，等窗口真正显示后再由 WM_TIMER 一次性加载。
    CRichEditUI* pFirst = static_cast<CRichEditUI*>(m_pm.FindControl(L"logview_text"));
    if (pFirst)
    {
        // [R-LOGV-3] 加载态直接写进正文控件：窗口一显示就能看到"正在加载"，
        //   而不是一片空白让人以为日志空了。定时器随后用真实内容整体替换。
        pFirst->SetText(StringMgr.Get("misc.logview_loading"));
    }
    if (pCntFirst)
        pCntFirst->SetText(StringMgr.Get("misc.logview_loading"));
    m_bLogViewLoaded = false;
}

// [L-10] 读盘 + 解析（只做一次），结果留在 m_vLogAll 供过滤/搜索复用。
//   过滤、搜索、跟随都只重走 LogViewFillView，不重复读文件。
void CToolDialogPage::LogViewRefresh()
{
    LARGE_INTEGER _t0, _t1, _t2, _fq;
    ::QueryPerformanceFrequency(&_fq);
    ::QueryPerformanceCounter(&_t0);
    CRichEditUI* pView = static_cast<CRichEditUI*>(
        m_pm.FindControl(L"logview_text"));
    CTextUI* pCnt = static_cast<CTextUI*>(
        m_pm.FindControl(L"logview_count"));
    if (!pView) return;

    // [R6-02] 文件读取/JSON 解析/行拼接全部下沉服务；本函数只负责渲染。
    std::vector<toolsvc::TLogLine> vLines;
    // [L-26][L-28] 三源合并：审计流 + 诊断底账 + 工具归档
    if (!SvcLogDep()->ReadMergedLines(vLines))
    {
        m_vLogAll.clear();
        pView->SetText(L"");
        if (pCnt)
            pCnt->SetText(StringMgr.Get("misc.logview_empty"));
        return;
    }
    ::QueryPerformanceCounter(&_t1);
    const double _msRead = 1000.0 * (_t1.QuadPart - _t0.QuadPart) / _fq.QuadPart;

    m_vLogAll.swap(vLines);      // 全量留底，下面按过滤条件填充

    ::QueryPerformanceCounter(&_t2);
    { wchar_t _b[256]={0}; swprintf_s(_b,L"[logview-perf] read=%.1fms lines=%d\n",
        _msRead, (int)m_vLogAll.size());
      ::OutputDebugStringW(_b); }

    // 读取完成 → 交给填充（过滤/搜索/上色/滚动都在那一步）
    LogViewFillView();
}

// [L-10] 按当前过滤/搜索条件填充正文。
//   与 LogViewRefresh 分离的原因：刷新要读盘（几十毫秒级），而改过滤条件、
//   输入关键字、自动跟随都是高频操作，重读文件纯属浪费；这里只做内存筛选。
void CToolDialogPage::LogViewFillView()
{
    CRichEditUI* pView = static_cast<CRichEditUI*>(
        m_pm.FindControl(L"logview_text"));
    CTextUI* pCnt = static_cast<CTextUI*>(
        m_pm.FindControl(L"logview_count"));
    if (!pView) return;

    // 1) 过滤：级别门槛 + 关键字（大小写不敏感）
    const int nMinLevel = (m_nLogFilter == 2) ? static_cast<int>(toolsvc::ELogLevel::kLogError)
                        : (m_nLogFilter == 1) ? static_cast<int>(toolsvc::ELogLevel::kLogWarn)
                        :                        -1;
    std::vector<const toolsvc::TLogLine*> vKeep;
    vKeep.reserve(m_vLogAll.size());
    for (size_t i = 0; i < m_vLogAll.size(); ++i)
    {
        const toolsvc::TLogLine& L = m_vLogAll[i];
        if (nMinLevel >= 0 && L.nLevel < nMinLevel) continue;
        if (!m_sLogSearch.IsEmpty())
        {
            // 简单子串匹配：日志定位够用，且不必引入正则与转义坑。
            // CompareNoCase 由 CDuiString 提供，避免自己写 towlower 循环。
            const CDuiString sLine = L.sText.c_str();
            if (sLine.Find(m_sLogSearch) < 0) continue;
        }
        vKeep.push_back(&L);
    }

    // 2) 只渲染尾部：把"全量历史"换成"最近 N 行"
    //   模型是 cchTextMost 级的天花板 + TextServices 对整段文本做逐字断行，
    //   行数一多就是 O(行数 × 字符数) 的重排版。之前的做法是"全量拼串 +
    //   全量 SetText + 逐行 SetSel/SetSelectionCharFormat 上色"，每次 SetSel
    //   都会让 TextServices 重算行布局，几千行时这就是几十秒级卡顿的主因，
    //   而且超过上限还会被静默截断（尾部丢失，正是最需要看的部分）。
    //   这里自尾向前累计到预算为止，保证"最新的一定看得到"。
    const size_t kMaxChars = 28 * 1024;
    const size_t kMaxLines = 2000;
    size_t iFirst = vKeep.size();
    size_t nFit = 0;
    while (iFirst > 0 && vKeep.size() - iFirst < kMaxLines)
    {
        const size_t nNext = vKeep[iFirst - 1]->sText.size() + 2;   // +CRLF
        if (nFit + nNext > kMaxChars) break;
        nFit += nNext;
        --iFirst;
    }
    // 一行就超预算时至少保留最后一行，否则会出现"空正文 + 有记录数"的怪状态
    if (iFirst == vKeep.size() && !vKeep.empty()) iFirst = vKeep.size() - 1;
    const size_t nShown = vKeep.size() - iFirst;
    const bool bTruncated = (iFirst > 0);

    // 3) 拼串：日志 msg 内已规范为 '\n'，RichEdit 需 CRLF 才正确换行；
    //    一次性 SetText 而不是逐次 AppendText（后者每行触发一次重排版）。
    //    同时记录"每行写入后的实际字符数"：上色需要 RichEdit 里的真实偏移，
    //    而 sText.size() 在遇到内嵌 '\n' 扩成 CRLF 时比实际短 1，逐行累加
    //    会让误差不断放大，导致后面所有行整体串色。
    std::wstring sAll;
    sAll.reserve(nFit + 4);
    std::vector<long> vOutLen;
    vOutLen.reserve(nShown);
    // [R-LOGV-5] 倒序渲染：最新一条在最上面。
    //   原先按文件顺序正着排，用户想看的"刚刚发生了什么"永远在窗口最底部，
    //   每次都得先滚到底；改成倒序后，打开即见最新，符合日志查看器的通用习惯。
    //   实现上从末尾往前遍历，仍然只取预算内的最后 N 条（"最近"语义不变），
    //   只是输出顺序反过来。
    for (size_t i = vKeep.size(); i > iFirst; --i)
    {
        const size_t nIdx = i - 1;
        if (i != vKeep.size()) sAll += L"\r\n";
        const size_t nBefore = sAll.size();
        const std::wstring& sDisp = vKeep[nIdx]->sText;
        for (size_t k = 0; k < sDisp.size(); ++k)
        {
            const wchar_t c = sDisp[k];
            if (c == L'\r')
            {
                sAll += L"\r\n";
                if (k + 1 < sDisp.size() && sDisp[k + 1] == L'\n') ++k;
            }
            else if (c == L'\n') sAll += L"\r\n";
            else sAll += c;
        }
        vOutLen.push_back(static_cast<long>(sAll.size() - nBefore));
    }

    pView->SetText(sAll.c_str());

    // 4) 上色：只对 warn/error 行发一次 EM_SETCHARFORMAT，偏移用 vOutLen 累计。
    //    旧实现对每行先 SetSel 再 SetSelectionCharFormat（两次 TxSendMessage，
    //    每次都会触发布局校验），行数一多既慢又会串色。
    long nWrote = pView->GetTextLength();
    long nLineStart = 0;
    size_t nPainted = 0;
    for (size_t j = 0; j < vOutLen.size() && nLineStart < nWrote; ++j)
    {
        // 倒序渲染后，第 j 个显示行对应 vKeep[size-1-j]
        const toolsvc::TLogLine& L = *vKeep[vKeep.size() - 1 - j];
        COLORREF cr = 0;   // 0 = 保持默认色
        bool bBold = false;
        if (L.nLevel == toolsvc::CLogDepGraphService::kSectionHeader) { cr = RGB(0x60, 0x60, 0x60); bBold = true; }
        else if (L.nLevel == static_cast<int>(toolsvc::ELogLevel::kLogWarn))      cr = RGB(0xE6, 0x7E, 0x22);
        else if (L.nLevel == static_cast<int>(toolsvc::ELogLevel::kLogError)) cr = RGB(0xC0, 0x39, 0x2B);
        if (cr != 0)
        {
            long nEnd = nLineStart + vOutLen[j];
            if (nEnd > nWrote) nEnd = nWrote;
            if (nEnd > nLineStart)
            {
                CHARFORMAT2 cf = { 0 };
                cf.cbSize = sizeof(cf);
                cf.dwMask = CFM_COLOR | (bBold ? CFM_BOLD : 0);
                cf.crTextColor = cr;
                if (bBold) cf.dwEffects = CFE_BOLD;
                pView->SetSel(nLineStart, nEnd);
                pView->SetSelectionCharFormat(cf);
                ++nPainted;
            }
        }
        nLineStart += vOutLen[j] + 2;   // +CRLF
    }

    // 5) 定位到顶部。
    //    [R-LOGV-5] 正文已改为倒序（最新在最上），因此打开后应停在文档开头，
    //    而不是像正序那样滚到底部。
    //    [R-LOGV-4] 顺带说明：插入点必须显式放在 0 且保持一致——RichEdit 在
    //    UIEVENT_SETFOCUS 时会 OnTxInPlaceActivate 并按插入点重设视图，
    //    这正是此前"点一下正文就跳回最早那条"的根因；现在倒序 + 插入点在 0，
    //    聚焦前后视图位置一致，不会再出现跳动。
    //    注意不能写 SetSel(-1,-1)：RichEdit 里 -1 表示"全选"，会整段高亮。
    if (nWrote > 0)
    {
        pView->SetSel(0, 0);
    }

    // 6) 计数：区分"过滤后可见条数"与"文件总条数"，否则用户会以为日志丢了
    if (pCnt)
    {
        if (m_vLogAll.empty())
            pCnt->SetText(StringMgr.Get("misc.logview_empty"));
        else
        {
            CDuiString s;
            const bool bFiltered = (m_nLogFilter != 0) || !m_sLogSearch.IsEmpty();
            if (bFiltered)
                s.Format(StringMgr.Get("misc.logview_count_filtered"),
                         static_cast<int>(vKeep.size()),
                         static_cast<int>(m_vLogAll.size()),
                         static_cast<int>(nShown));
            else if (bTruncated)
                s.Format(StringMgr.Get("misc.logview_count_tail"),
                         static_cast<int>(m_vLogAll.size()),
                         static_cast<int>(nShown));
            else
                s.Format(StringMgr.Get("misc.logview_count"),
                         static_cast<int>(m_vLogAll.size()));
            pCnt->SetText(s);
        }
    }

    { wchar_t _b[256]={0}; swprintf_s(_b,
        L"[logview-fill] total=%d kept=%d shown=%d chars=%d painted=%d\n",
        (int)m_vLogAll.size(), (int)vKeep.size(), (int)nShown,
        (int)sAll.size(), (int)nPainted);
      ::OutputDebugStringW(_b); }
}

// [L-10] 导出当前（已过滤）内容到文件：先存到统一日志目录，再打开所在文件夹。
//   此前只能靠剪贴板转存，遇到几千行时用户体验很差。
void CToolDialogPage::LogViewExport()
{
    // 用"审计日志同目录 + 时间戳"生成文件名，避免落到不可写位置
    SYSTEMTIME st = { 0 };
    ::GetLocalTime(&st);
    CDuiString sName;
    sName.Format(L"log_export_%04d%02d%02d_%02d%02d%02d.txt",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    const std::wstring sPath = LogService::UnifiedLogPath(sName.GetData());
    if (sPath.empty())
    {
        m_pHost->HostAlert(StringMgr.Get("misc.logview_title"),
                           StringMgr.Get("misc.logview_export_fail"),
                           MB_ICONWARNING);
        return;
    }

    // 导出内容 = 当前正文（也就是用户眼前看到的、已按条件过滤的结果），
    //   但不受"尾部 N 行"预算限制：导出应当完整，否则等于把过滤结果又裁一次。
    FILE* fp = nullptr;
    if (::_wfopen_s(&fp, sPath.c_str(), L"wb") != 0 || !fp)
    {
        m_pHost->HostAlert(StringMgr.Get("misc.logview_title"),
                           StringMgr.Get("misc.logview_export_fail"),
                           MB_ICONWARNING);
        return;
    }
    // 写 UTF-8 BOM：让记事本/Excel 正确识别中文，避免导出后乱码
    const unsigned char kBom[3] = { 0xEF, 0xBB, 0xBF };
    std::fwrite(kBom, 1, sizeof(kBom), fp);

    const int nMinLevel = (m_nLogFilter == 2) ? static_cast<int>(toolsvc::ELogLevel::kLogError)
                        : (m_nLogFilter == 1) ? static_cast<int>(toolsvc::ELogLevel::kLogWarn)
                        :                        -1;
    size_t nWritten = 0;
    for (size_t i = 0; i < m_vLogAll.size(); ++i)
    {
        const toolsvc::TLogLine& L = m_vLogAll[i];
        if (nMinLevel >= 0 && L.nLevel < nMinLevel) continue;
        if (!m_sLogSearch.IsEmpty())
        {
            const CDuiString sLine = L.sText.c_str();
            if (sLine.Find(m_sLogSearch) < 0) continue;
        }
        // 宽 → UTF-8
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, L.sText.c_str(),
                                            static_cast<int>(L.sText.size()),
                                            nullptr, 0, nullptr, nullptr);
        if (n > 0)
        {
            std::string sU8(static_cast<size_t>(n), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, L.sText.c_str(),
                                  static_cast<int>(L.sText.size()),
                                  &sU8[0], n, nullptr, nullptr);
            std::fwrite(sU8.data(), 1, sU8.size(), fp);
        }
        std::fwrite("\r\n", 1, 2, fp);
        ++nWritten;
    }
    std::fclose(fp);

    // 导出结果告知 + 直接打开所在目录（用户下一步通常就是要去拿文件）
    CDuiString sMsg;
    sMsg.Format(StringMgr.Get("misc.logview_export_done"),
                static_cast<int>(nWritten), sPath.c_str());
    m_pHost->HostNotify(StringMgr.Get("misc.logview_title"), sMsg.GetData(), 0);

    CDuiString sDir = sPath.c_str();
    const int nSep = sDir.ReverseFind(L'\\');
    if (nSep >= 0) sDir = sDir.Left(nSep);
    ::ShellExecuteW(m_hWnd, L"open", sDir.GetData(), nullptr, nullptr, SW_SHOWNORMAL);
}

// [L-10] 切换自动跟随：开启后由定时器按固定间隔增量刷新（只看新增内容）。
void CToolDialogPage::LogViewToggleFollow()
{
    m_bLogFollow = !m_bLogFollow;
    if (m_bLogFollow)
    {
        // 跟随的语义是"盯着最新"，因此先把过滤放开到全部、并滚到底部，
        // 否则开了跟随却因为过滤看不到新行，会让人以为跟随失效。
        if (m_hWnd) ::SetTimer(m_hWnd, LOGVIEW_FOLLOW_TIMER_ID, 700, nullptr);
        LogViewRefresh();
    }
    else
    {
        if (m_hWnd) ::KillTimer(m_hWnd, LOGVIEW_FOLLOW_TIMER_ID);
    }

    // 按钮态反馈：跟随中用主按钮样式，关闭用次按钮样式
    if (CButtonUI* pBtn = static_cast<CButtonUI*>(m_pm.FindControl(L"btn_logview_follow")))
    {
        pBtn->SetText(StringMgr.Get(m_bLogFollow ? "misc.logview_follow_on"
                                                 : "misc.logview_follow"));
    }
}

void CToolDialogPage::LogViewCopySelection()
{
    CRichEditUI* pView = static_cast<CRichEditUI*>(
        m_pm.FindControl(L"logview_text"));
    if (!pView) return;

    if (pView->GetSelectionType() == SEL_EMPTY)
    {
        m_pHost->HostAlert(StringMgr.Get("misc.logview_title"),
                           StringMgr.Get("misc.logview_copy_empty"),
                           MB_ICONINFORMATION);
        return;
    }
    pView->Copy();
}

// [R-LOGV-4] 聚焦时兜底：仅当插入点还在 0、且当前没有选区时才把光标推到末尾。
//   目的是挡住"点一下正文就跳回最早那条"——RichEdit 在 UIEVENT_SETFOCUS
//   里会 OnTxInPlaceActivate 并按插入点重设视图，插入点为 0 就等于滚动到顶。
//   一旦有选区（用户正在拖选/已选中）或插入点已在别处，就完全不动，
//   避免把用户刚建立的选区冲掉。
void CToolDialogPage::LogViewKeepTail()
{
    CRichEditUI* pView = static_cast<CRichEditUI*>(
        m_pm.FindControl(L"logview_text"));
    if (!pView) return;

    long nStart = 0, nEnd = 0;
    pView->GetSel(nStart, nEnd);
    if (nStart != nEnd) return;          // 有选区：尊重用户当前选择
    if (nStart > 0) return;              // 插入点不在开头：已经在正常滚动位置

    const long nLen = pView->GetTextLength();
    if (nLen <= 0) return;
    pView->SetSel(nLen, nLen);
    pView->ScrollCaret();
}

// 全选日志正文，便于一次性复制整段。
void CToolDialogPage::LogViewSelectAll()
{
    CRichEditUI* pView = static_cast<CRichEditUI*>(
        m_pm.FindControl(L"logview_text"));
    if (!pView) return;
    ::SetFocus(m_hWnd);          // RichEdit 的复制依赖宿主焦点
    pView->SetSelAll();
}

// 日志正文右键菜单：复用既有文案键，避免新增翻译条目。
void CToolDialogPage::LogViewContextMenu(POINT ptScreen)
{
    CRichEditUI* pView = static_cast<CRichEditUI*>(
        m_pm.FindControl(L"logview_text"));
    if (!pView) return;

    // 与框架内置菜单不同，这里先保证控件已聚焦：后续 Copy() 走 RichEdit
    // 自身命令通道，未聚焦时部分宿主上会静默失败。
    ::SetFocus(m_hWnd);
    pView->SetFocus();

    const bool bHasSel = (pView->GetSelectionType() != SEL_EMPTY);

    HMENU hMenu = ::CreatePopupMenu();
    if (!hMenu) return;
    ::AppendMenuW(hMenu, MF_STRING | (bHasSel ? MF_ENABLED : MF_GRAYED),
                  1, StringMgr.Get("misc.logview_copy"));
    ::AppendMenuW(hMenu, MF_STRING, 2, StringMgr.Get("misc.hd935"));
    ::AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(hMenu, MF_STRING, 3, StringMgr.Get("misc.logview_refresh"));

    const int nCmd = ::TrackPopupMenu(
        hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN,
        ptScreen.x, ptScreen.y, 0, m_hWnd, nullptr);
    ::DestroyMenu(hMenu);

    if (nCmd == 1) LogViewCopySelection();
    else if (nCmd == 2) LogViewSelectAll();
    else if (nCmd == 3) LogViewRefresh();
}

void CToolDialogPage::LogViewOpenFolder()
{
    const CDuiString sPath = LogViewPath();
    CDuiString sDir = sPath;
    int nSep = sDir.ReverseFind(L'\\');
    if (nSep >= 0) sDir = sDir.Left(nSep);
    ::ShellExecuteW(m_hWnd, L"open", sDir.GetData(), nullptr, nullptr,
                    SW_SHOWNORMAL);
}

void CToolDialogPage::LogViewClear()
{
    // [L-10-6] 清空审计日志前必须二次确认：日志是事后定位问题的唯一依据，
    //   一键蒸发后无法恢复，误触成本远高于多一次确认。
    if (!m_pHost->HostConfirm(StringMgr.Get("misc.logview_title"),
                              StringMgr.Get("misc.logview_clear_confirm")))
        return;

    // [L-10-6] 清空前自动留档一份，满足"审计不可篡改"的最低要求：
    //   清空动作本身也不再是"删除即蒸发"，而是"移到归档"。
    const CDuiString sOld = LogViewPath();
    if (!sOld.IsEmpty())
    {
        CDuiString sBak = sOld;
        sBak += L".cleared";
        ::DeleteFileW(sBak.GetData());          // 只保留最近一次清空前快照
        ::MoveFileExW(sOld.GetData(), sBak.GetData(), MOVEFILE_REPLACE_EXISTING);
    }

    // [R6-02] 文件截断下沉服务（旧实现同样忽略打开失败）。
    SvcLogDep()->ClearLogFile();

    // [L-17] 清空后同步内存环：否则"清空完刷新又把旧行放回来"。
    LogService::RingClear();

    // 审计日志本身记录清空动作（清空后的第一条）
    m_pHost->HostLog(CDuiString(L"[logview] log file cleared (archived as .cleared)"));

    CRichEditUI* pView = static_cast<CRichEditUI*>(
        m_pm.FindControl(L"logview_text"));
    if (pView) pView->SetText(L"");

    CTextUI* pCnt = static_cast<CTextUI*>(
        m_pm.FindControl(L"logview_count"));
    if (pCnt) pCnt->SetText(StringMgr.Get("misc.logview_cleared"));

    m_pHost->HostAlert(StringMgr.Get("misc.logview_title"),
                       StringMgr.Get("misc.logview_cleared"),
                       MB_ICONINFORMATION);
}

// ============================================================
// [R-B25] 依赖图可视化弹窗（EToolDlgKind::TD_DEPGRAPH）
// 数据由宿主经 Open(ToolOpenArgs.vDepNodes) 注入服务；本函数只负责渲染缩进树。
// 节点结构：ToolOpenArgs.vDepNodes（扁平数组，nLevel 决定缩进层级）。
// ============================================================
void CToolDialogPage::BuildDepGraph()
{
    CVerticalLayoutUI* pDesc = std::make_unique<CVerticalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pDesc->SetFixedHeight(28);
    pDesc->SetAttribute(L"inset", L"2,4,2,4");
    CTextUI* pCnt = MkText(L"", 1, 0xFF606060);
    pCnt->SetName(L"depgraph_count");
    pCnt->SetTextPadding(RECT{ 0, 0, 0, 0 });
    pCnt->SetMouseEnabled(false);
    pDesc->Add(pCnt);
    m_pBody->Add(pDesc);

    CControlUI* pGap0 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pGap0->SetFixedHeight(8);
    m_pBody->Add(pGap0);

    CVerticalLayoutUI* pList = std::make_unique<CVerticalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pList->SetName(L"depgraph_list");
    pList->SetBkColor(tokens::Token(tokens::EToken::Surface));
    pList->SetAttribute(L"bordercolor", tokens::TokenHex(tokens::EToken::Border));
    pList->SetAttribute(L"bordersize", L"1");
    pList->SetAttribute(L"inset", L"14,10,14,10");
    pList->SetAttribute(L"childpadding", L"0");
    pList->SetAttribute(L"vscrollbar", L"true");
    m_pBody->Add(pList);

    CControlUI* pGapL = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pGapL->SetMinHeight(16);
    m_pBody->Add(pGapL);

    CHorizontalLayoutUI* pRow = std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pRow->SetFixedHeight(74);
    pRow->SetAttribute(L"inset", L"0,18,0,18");

    CControlUI* pSp = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pRow->Add
    pRow->Add(pSp);

    CButtonUI* pRefresh = MkBtn(
        StringMgr.Get("misc.depgraph_refresh"),
        L"btn_depgraph_refresh", false);
    pRefresh->SetFixedWidth(130);
    pRow->Add(pRefresh);

    CControlUI* pGap = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pRow->Add
    pGap->SetFixedWidth(14);
    pRow->Add(pGap);

    CButtonUI* pCopy = MkBtn(
        StringMgr.Get("misc.depgraph_copy"),
        L"btn_depgraph_copy", false);
    pCopy->SetFixedWidth(130);
    pRow->Add(pCopy);

    m_pBody->Add(pRow);

    DepGraphRefresh();
}

void CToolDialogPage::DepGraphRefresh()
{
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(
        m_pm.FindControl(L"depgraph_list"));
    CTextUI* pCnt = static_cast<CTextUI*>(
        m_pm.FindControl(L"depgraph_count"));
    if (!pList) return;

    pList->RemoveAll();

    // [R6-02] 节点->缩进视图行的构建在服务；本函数只负责控件着色/字体。
    toolsvc::TDepView view = SvcLogDep()->BuildDepView();

    if (!view.bHasNodes)
    {
        CTextUI* pEmpty = MkText(StringMgr.Get("misc.depgraph_empty"), 1, 0xFF999999);
        pEmpty->SetTextPadding(RECT{ 4, 0, 4, 0 });
        pList->Add(pEmpty);
        if (pCnt) pCnt->SetText(L"");
        return;
    }

    for (size_t i = 0; i < view.rows.size(); ++i)
    {
        const toolsvc::TDepRow& r = view.rows[i];

        CTextUI* pRow = std::make_unique<CTextUI>().release(); // [C++17 R3] framework-owned via pList->Add
        pRow->SetFont(1);
        pRow->SetText(r.sText.c_str());
        if (r.bRoot)
        {
            pRow->SetTextColor(tokens::Token(tokens::EToken::Ink));
            pRow->SetFont(2);
        }
        else
        {
            pRow->SetTextColor(tokens::Token(tokens::EToken::InkTertiary));
        }
        pRow->SetTextPadding(RECT{ 4, 0, 4, 0 });
        pRow->SetFixedHeight(24);
        pRow->SetToolTip(r.sLabel.c_str());
        pList->Add(pRow);
    }

    if (pCnt)
    {
        CDuiString sCnt;
        sCnt.Format(StringMgr.Get("misc.depgraph_count"),
                    view.nRoots + view.nDeps);
        pCnt->SetText(sCnt.GetData());
    }
}

void CToolDialogPage::DepGraphCopyAll()
{
    // [R6-02] 纯文本拼装在服务；剪贴板/弹窗留页。
    const std::wstring sCopy = SvcLogDep()->BuildDepCopyText();
    CDuiString sAll = sCopy.c_str();

    if (sAll.IsEmpty())
    {
        m_pHost->HostAlert(StringMgr.Get("misc.depgraph_title"),
                           StringMgr.Get("misc.depgraph_empty"),
                           MB_ICONINFORMATION);
        return;
    }

    if (::OpenClipboard(m_hWnd))
    {
        ::EmptyClipboard();
        size_t nBytes = (sAll.GetLength() + 1) * sizeof(wchar_t);
        HGLOBAL hMem = ::GlobalAlloc(GMEM_MOVEABLE, nBytes);
        if (hMem)
        {
            void* p = ::GlobalLock(hMem);
            if (p)
            {
                memcpy(p, sAll.GetData(), nBytes);
                ::GlobalUnlock(hMem);
                ::SetClipboardData(CF_UNICODETEXT, hMem);
            }
            else
            {
                ::GlobalFree(hMem);
            }
        }
        ::CloseClipboard();
    }
}
// ============================================================
// [R-B35] 独立 ESD 解密向导（EToolDlgKind::TD_ESDWIZ）
// 流程：选源 ESD → 显示索引 → 选密钥（自动匹配 / 手动输入）→ 导出 WIM
// 数据源：Data\DismData\Data.xml 的 <ESDDecryptKey> 节点（6 条 base64 RSA 私钥），
//        由 Page 经 CDataEngine::GetEsdKeys() 读取后经 Open(ToolOpenArgs.vEsdKeys) 注入。
// 说明：[2026-10-08] 已按知识库 41 §26 实现进程内 CryptoAPI 自主解密
//        （RSA 私钥解包 AES 会话密钥 + AES-CBC RANGE 循环），不再依赖
//        dism.exe /Export-Image，实现见 EsdDecryptor.h/.cpp。
// ============================================================
CTextUI* CToolDialogPage::MkSysLink(const wchar_t* pszText, const wchar_t* pszName)
{
    CTextUI* p = std::make_unique<CTextUI>().release(); // [C++17 R3] framework-owned via caller pList->Add
    p->SetName(pszName);
    p->SetText(pszText);
    p->SetFont(1);
    p->SetTextColor(tokens::Token(tokens::EToken::Accent));          // 超链接蓝
    p->SetAttribute(L"hottextcolor", tokens::TokenHex(tokens::EToken::AccentText));
    p->SetAttribute(L"underline", L"true");
    p->SetAttribute(L"valign", L"vcenter");
    p->SetFixedHeight(24);
    p->SetMouseEnabled(true);
    p->SetBkColor(tokens::Token(tokens::EToken::Surface));
    return p;
}

// [R16][T4] 旧 SetLinkItems 已删除：条目/标题统一经 Open(ToolOpenArgs) 注入 CListService。
void CToolDialogPage::BuildLinkList()
{
    // 顶部：标题 + 说明
    CVerticalLayoutUI* pDesc = std::make_unique<CVerticalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pDesc->SetFixedHeight(28);
    pDesc->SetAttribute(L"inset", L"2,4,2,4");
    const std::wstring& sLinkTitle = SvcList()->Title();
    CTextUI* pNote = MkText(
        sLinkTitle.empty() ? StringMgr.Get("linklist.desc") : CDuiString(sLinkTitle.c_str()),
        1, 0xFF606060);
    pNote->SetTextPadding(RECT{ 0, 0, 0, 0 });
    pDesc->Add(pNote);
    m_pBody->Add(pDesc);

    CControlUI* pGap0 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pGap0->SetFixedHeight(8);
    m_pBody->Add(pGap0);

    // 中部：条目列表（可点击，点击打开 target）
    CVerticalLayoutUI* pList = std::make_unique<CVerticalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pList->SetName(L"linklist_box");
    pList->SetBkColor(tokens::Token(tokens::EToken::Surface));
    pList->SetAttribute(L"bordercolor", tokens::TokenHex(tokens::EToken::Border));
    pList->SetAttribute(L"bordersize", L"1");
    pList->SetAttribute(L"inset", L"14,10,14,10");
    pList->SetAttribute(L"childpadding", L"0");
    pList->SetAttribute(L"vscrollbar", L"true");
    m_pBody->Add(pList);

    // [R6-02] 条目数据/越界由 CListService 持有；这里只负责渲染。
    const size_t nItems = SvcList()->Count();
    for (size_t i = 0; i < nItems; ++i)
    {
        std::wstring sLabel;
        SvcList()->LabelAt(static_cast<int>(i), sLabel);
        CDuiString sName;
        sName.Format(L"link_%d", static_cast<int>(i));
        CTextUI* pL = MkSysLink(CDuiString(sLabel.c_str()), sName);
        pL->SetTextPadding(RECT{ 4, 0, 4, 0 });
        pList->Add(pL);

        if (i + 1 < nItems)
        {
            CControlUI* pGap = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pList->Add
            pGap->SetFixedHeight(4);
            pList->Add(pGap);
        }
    }

    // 底部：关闭
    CControlUI* pGapL = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pGapL->SetMinHeight(16);
    m_pBody->Add(pGapL);

    AddBottomRow(MkBtn(StringMgr.Get("general.ok"), L"btn_cancel", false),
                 MkBtn(StringMgr.Get("general.cancel"), L"btn_cancel", false));
}

void CToolDialogPage::LinkListOpen(const CDuiString& sName)
{
    int nIdx = _ttoi(sName.Mid(5).GetData());
    // [R6-02] 越界/空目标由服务拒绝；打开动作（UI/ShellExecute）留页。
    std::wstring sTarget;
    if (!SvcList()->TargetAt(nIdx, sTarget)) return;
    // http(s) 与本地路径均经 ShellExecute "open"（与旧实现一致）
    ::ShellExecuteW(m_hWnd, L"open", sTarget.c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL);
}

