#include "stdafx.h"

#include "FrameWnd_UiHelpers.h"

#include "HostInterfaces.h"      // IHostUi / ELogLevel
#include "StringManager.h"
#include "FrameWnd_Util.h"       // FrameWnd_Util_CopyTextToClipboard
#include "OptimScopeColumns.h"   // [SCOPE-2COL] optim_scope::IsColumnVisible（可见列唯一真源）

#include <shellapi.h>

// P8：原 FrameWnd.cpp static ParsePrefix2，多页共享前缀解析。
bool ParsePrefix2(const CDuiString& sName, const wchar_t* pszPrefix, int& a, int& b)
{
    size_t n = wcslen(pszPrefix);
    if (wcsncmp(sName.GetData(), pszPrefix, n) != 0) return false;

    CDuiString rest = sName.Mid(static_cast<int>(n));
    int p = rest.Find(L'_');
    if (p <= 0) return false;

    a = _ttoi(rest.Mid(0, p).GetData());
    b = _ttoi(rest.Mid(p + 1).GetData());
    return true;
}

// P8：原 FrameWnd.cpp static ParseSwName（优化页右键菜单解析）。
bool ParseSwName(const wchar_t* pszName, const wchar_t* pszPrefix,
                 int& g, int& i, int& nScope)
{
    size_t nPre = wcslen(pszPrefix);
    if (wcsncmp(pszName, pszPrefix, nPre) != 0) return false;

    CDuiString rest = pszName + nPre;
    int p1 = rest.Find(L'_');
    if (p1 <= 0) return false;

    CDuiString tail = rest.Mid(p1 + 1);
    int p2 = tail.Find(L'_');
    if (p2 <= 0) return false;

    g = _ttoi(rest.Mid(0, p1).GetData());
    i = _ttoi(tail.Mid(0, p2).GetData());
    nScope = _ttoi(tail.Mid(p2 + 1).GetData());

    // [SCOPE-2COL] 作用域合法性只认页面真实存在的列（当前用户/系统）。
    // 原实现放开 scope == 2（新用户/Default），而该列已按交互要求取消，
    // 页面不再创建任何 sw_g_i_2 控件。保留 2 等于给"看不见的列被写入"
    // 留一道缺口：一旦名字来源被污染（控件被复用/名字拼错），
    // 写入会静默落到 HKU\.DEFAULT 而界面上毫无反馈。
    // 与 optim_scope::kColumns 同源，增删列时自动跟随。
    return optim_scope::IsColumnVisible(nScope);
}

// P8：原 CFrameWnd::ShowUnifiedListContextMenu，逐字搬迁，仅把
// m_hWnd/AppendLog 改为参数 host 能力（菜单本身无破坏性动作）。
// [R85 P1-13] 内部实现：与 UiHelpers_ShowUnifiedListContextMenu 相同逻辑，
// 仅多出 pszPath 参数，非空时追加"打开所在位置"菜单项。
static void ShowUnifiedListContextMenuEx(IHostUi* host, HWND hwnd,
                                         EListCtx nKind, int nIdx, POINT pt,
                                         const wchar_t* pszPath,
                                         const std::function<void()>& fnSelectAll = std::function<void()>(),
                                         const std::function<void()>& fnInvert = std::function<void()>(),
                                         const std::function<void()>& fnDetail = std::function<void()>())
{
    HMENU hMenu = ::CreatePopupMenu();
    if (!hMenu) return;

    // 菜单 ID 分配：5xxx 段，避免与已有 4xxx 段冲突
    enum {
        IDC_DETAIL     = 5002,  // 显示详情（AppendLog）
        IDC_JUMP       = 5003,  // 跳转商店/设置
        IDC_DELETE     = 5004,  // 删除/卸载
        IDC_REPAIR     = 5005,  // 修复/重装
        IDC_RESET      = 5006,  // 重置
        IDC_SELECT_ALL = 5007,  // 全选
        IDC_INVERT     = 5008,  // 反选
        IDC_OPEN_EXPLORER = 5009,  // [R85] 打开所在位置
        // [AUDIT-G2] 原 IDC_COPY_SEL(5001) 已删除：从未装配进菜单、
        //   处理分支为空 break——纯死代码（G10 死菜单治理同类）。
    };

    bool bHasOpenExplorer = (pszPath != nullptr && pszPath[0] != L'\0');

    // [R125-3] 可见项由种类位掩码决定，取代原有 6 分支 switch（CC 主要来源）。
    enum : unsigned
    {
        kF_Detail = 1u << 0, kF_Jump = 1u << 1, kF_Delete = 1u << 2,
        kF_Repair = 1u << 3, kF_Reset = 1u << 4,
        kF_All = 1u << 5,    kF_Invert = 1u << 6,
    };
    struct CtxMask { EListCtx kind; unsigned flags; };
    static constexpr CtxMask kMask[] = {
        { EListCtx::LCX_CLEAN,      kF_Detail | kF_All | kF_Invert },
        { EListCtx::LCX_UPDATE,     kF_Detail | kF_Jump },
        { EListCtx::LCX_APPX,       kF_Detail | kF_Delete | kF_Jump },
        { EListCtx::LCX_ASSOC_IMG,  kF_Detail | kF_Delete | kF_Reset },
        { EListCtx::LCX_ASSOC_LIVE, kF_Detail | kF_Delete | kF_Reset },
        { EListCtx::LCX_PROGRAM,    kF_Detail | kF_Delete | kF_Repair },
    };
    unsigned uFlags = kF_Detail;   // 默认只出现「详情」
    for (const auto& m : kMask)
        if (m.kind == nKind) { uFlags = m.flags; break; }

    // [AUDIT-G1] 破坏性动作（删除/修复/重置）只在命中具体行时提供。
    //   列表空白处右键 nIdx = -1 没有目标行，历史上这三个项点击只写一行
    //   日志（假动作承诺：Appx 列表空白处显示「删除」、Assoc 显示「删除/
    //   重置」，点了毫无反应）。真实链路在行级专属右键菜单（Appx 4401-4405 /
    //   Assoc 6 动作）与「勾选 + 按钮」（AxConfirmDelete / AssocButton），
    //   统一菜单只在带行号时才应透传破坏性项。
    if (nIdx < 0)
        uFlags &= ~(kF_Delete | kF_Repair | kF_Reset);
    // [AUDIT-G2] 「详情」同理：未接入真实详情回调（fnDetail）时不提供，
    //   否则点击只写一行日志（假动作）。清理页传入 ShowCleanDesc 接真。
    if (!fnDetail)
        uFlags &= ~kF_Detail;

    const bool bHasDetail    = (uFlags & kF_Detail) != 0;
    const bool bHasJump      = (uFlags & kF_Jump)   != 0;
    const bool bHasDelete    = (uFlags & kF_Delete) != 0;
    const bool bHasRepair    = (uFlags & kF_Repair) != 0;
    const bool bHasReset     = (uFlags & kF_Reset)  != 0;
    const bool bHasSelectAll = (uFlags & kF_All)    != 0;
    const bool bHasInvert    = (uFlags & kF_Invert) != 0;

    // [AUDIT-G1] 屏蔽后没有任何可用项（如 Assoc 空白处只剩破坏性位）：
    //   不弹空菜单，直接放行（DefWindowProc 继续处理 WM_CONTEXTMENU）。
    if (!bHasDetail && !bHasJump && !bHasDelete && !bHasRepair
        && !bHasReset && !bHasSelectAll && !bHasInvert && !bHasOpenExplorer)
    {
        ::DestroyMenu(hMenu);
        return;
    }

    // 组装菜单（按固定顺序，未启用的项不添加，避免灰项）
    int nId = 0;
    if (bHasSelectAll) { nId = IDC_SELECT_ALL; ::AppendMenu(hMenu, MF_STRING, nId, StringMgr.Get("misc.hd935")); }
    if (bHasInvert)    { nId = IDC_INVERT;     ::AppendMenu(hMenu, MF_STRING, nId, StringMgr.Get("misc.haf22")); }
    if (bHasSelectAll || bHasInvert) ::AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);

    if (bHasDetail)    { nId = IDC_DETAIL;     ::AppendMenu(hMenu, MF_STRING, nId, StringMgr.Get("misc.h727c")); }
    if (bHasJump)      { nId = IDC_JUMP;       ::AppendMenu(hMenu, MF_STRING, nId, StringMgr.Get("misc.hdd16")); }
    if (bHasDelete)    { nId = IDC_DELETE;     ::AppendMenu(hMenu, MF_STRING, nId, StringMgr.Get("misc.h951a")); }
    if (bHasRepair)    { nId = IDC_REPAIR;     ::AppendMenu(hMenu, MF_STRING, nId, StringMgr.Get("misc.h3085")); }
    if (bHasReset)     { nId = IDC_RESET;      ::AppendMenu(hMenu, MF_STRING, nId, StringMgr.Get("misc.h931c")); }
    if (bHasOpenExplorer) { nId = IDC_OPEN_EXPLORER; ::AppendMenu(hMenu, MF_STRING, nId, StringMgr.Get("misc.h5f3a")); }

    int iCmd = ::TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                pt.x, pt.y, 0, hwnd, nullptr);
    ::DestroyMenu(hMenu);

    if (iCmd == 0) return;

    // 处理菜单选择
    switch (iCmd)
    {
    case IDC_SELECT_ALL:
        host->HostLog(StringMgr.Get("misc.he7d2"), static_cast<int>(ELogLevel::LOG_INFO));
        if (fnSelectAll) fnSelectAll();   // [AUDIT-F3] 原只写日志：菜单项接入真实动作
        break;

    case IDC_INVERT:
        host->HostLog(StringMgr.Get("misc.h8786"), static_cast<int>(ELogLevel::LOG_INFO));
        if (fnInvert) fnInvert();         // [AUDIT-F3] 同上
        break;

    case IDC_DETAIL:
        if (fnDetail)
        {
            fnDetail();   // [AUDIT-G2] 真实详情（如清理页 ShowCleanDesc）
            break;
        }
        // 无回调时不可达（菜单装配已屏蔽详情项）；保留日志兜底
        {
            CDuiString sLog;
            sLog.Format(StringMgr.Get("misc.h718d"), static_cast<int>(nKind), nIdx);
            host->HostLog(sLog.GetData(), static_cast<int>(ELogLevel::LOG_INFO));
        }
        break;

    case IDC_JUMP:
    {
        if (nKind == EListCtx::LCX_UPDATE)
        {
            // 打开 Windows 更新设置（ms-settings:windowsupdate）
            ::ShellExecuteW(hwnd, L"open", L"ms-settings:windowsupdate", nullptr, nullptr, SW_SHOWNORMAL);
            host->HostLog(StringMgr.Get("misc.h8a51"), static_cast<int>(ELogLevel::LOG_INFO));
        }
        else if (nKind == EListCtx::LCX_APPX)
        {
            ::ShellExecuteW(hwnd, L"open", L"ms-windows-store:", nullptr, nullptr, SW_SHOWNORMAL);
            host->HostLog(StringMgr.Get("misc.h4b6f"), static_cast<int>(ELogLevel::LOG_INFO));
        }
        else
        {
            host->HostLog(StringMgr.Get("misc.hd42c"), static_cast<int>(ELogLevel::LOG_INFO));
        }
        break;
    }

    case IDC_DELETE:
    case IDC_REPAIR:
    case IDC_RESET:
    {
        // [AUDIT-G1] 这些分支仅在调用方带行号（nIdx >= 0）时可达——当前
        // 三个调用方都只在空白处弹统一菜单（-1），破坏性项已被装配层屏蔽。
        // 若未来接入带行号的调用方，必须同时接通真实动作，不得保留假动作。
        CDuiString sInfo;
        sInfo.Format(StringMgr.Get("misc.h30dd"),
                    static_cast<int>(nKind), nIdx);
        host->HostLog(sInfo.GetData(), static_cast<int>(ELogLevel::LOG_INFO));

        // 复制提示到剪贴板，便于用户操作
        FrameWnd_Util_CopyTextToClipboard(hwnd, sInfo.GetData());
        break;
    }

    case IDC_OPEN_EXPLORER:
    {
        // [R85] 打开所在位置：GetFullPathNameW + explorer /select
        CDuiString sFull;
        wchar_t wszFull[MAX_PATH] = { 0 };
        DWORD dwLen = ::GetFullPathNameW(pszPath, MAX_PATH, wszFull, nullptr);
        if (dwLen > 0 && dwLen < MAX_PATH)
        {
            if (UiHelpers_OpenExplorerSelect(hwnd, wszFull))
            {
                CDuiString sLog;
                sLog.Format(StringMgr.Get("misc.h5a7e"), wszFull);
                host->HostLog(sLog.GetData(), static_cast<int>(ELogLevel::LOG_INFO));
            }
            else
            {
                CDuiString sLog;
                sLog.Format(StringMgr.Get("misc.h5b2f"), wszFull);
                host->HostLog(sLog.GetData(), static_cast<int>(ELogLevel::LOG_INFO));
            }
        }
        else
        {
            host->HostLog(StringMgr.Get("misc.h5c8d"), static_cast<int>(ELogLevel::LOG_INFO));
        }
        break;
    }
    }   // switch (iCmd)
}

// P8：原 CFrameWnd::ShowUnifiedListContextMenu，逐字搬迁，仅把
// m_hWnd/AppendLog 改为参数 host 能力（菜单本身无破坏性动作）。
void UiHelpers_ShowUnifiedListContextMenu(IHostUi* host, HWND hwnd,
                                         EListCtx nKind, int nIdx, POINT pt)
{
    ShowUnifiedListContextMenuEx(host, hwnd, nKind, nIdx, pt, nullptr);
}

// [R85 P1-13] 带路径参数版本：pszPath 非空时追加"打开所在位置"。
// [AUDIT-F3] 全选/反选回调透传（缺省空 → 其余调用点行为不变）。
// [AUDIT-G2] fnDetail：真实详情回调，未传时菜单不装配「详情」项。
void UiHelpers_ShowUnifiedListContextMenuPath(IHostUi* host, HWND hwnd,
                                              EListCtx nKind, int nIdx, POINT pt,
                                              const wchar_t* pszPath,
                                              const std::function<void()>& fnSelectAll,
                                              const std::function<void()>& fnInvert,
                                              const std::function<void()>& fnDetail)
{
    ShowUnifiedListContextMenuEx(host, hwnd, nKind, nIdx, pt, pszPath,
                                 fnSelectAll, fnInvert, fnDetail);
}

// [R85 P1-13] 打开"所在位置"：GetFullPathNameW 规范化 → ShellExecuteW explorer /select
bool UiHelpers_OpenExplorerSelect(HWND hwnd, const wchar_t* pszPath)
{
    if (!pszPath || pszPath[0] == L'\0') return false;

    // GetFullPathNameW 规范化相对/未展开路径；输出缓冲含 0 结尾
    wchar_t wszFull[MAX_PATH] = { 0 };
    DWORD dwLen = ::GetFullPathNameW(pszPath, MAX_PATH, wszFull, nullptr);
    if (dwLen == 0 || dwLen >= MAX_PATH) return false;

    // explorer /select,"path" —— 只定位，不打开；失败时退回仅打开父目录
    CDuiString sArgs;
    sArgs.Format(L"/select,\"%s\"", wszFull);

    HINSTANCE h = ::ShellExecuteW(hwnd, L"open", L"explorer.exe",
                                  sArgs.GetData(), nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(h) <= 32)
    {
        // 回退：只打开所在目录（文件可能刚被删除，仅剩父目录可定位）
        wchar_t wszDir[MAX_PATH] = { 0 };
        if (::GetFullPathNameW(wszFull, MAX_PATH, wszDir, nullptr) > 0)
        {
            wchar_t* pSep = wcsrchr(wszDir, L'\\');
            if (pSep) { *pSep = L'\0'; }
            ::ShellExecuteW(hwnd, L"open", wszDir, nullptr, nullptr, SW_SHOWNORMAL);
        }
        return false;
    }
    return true;
}
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：

// [R127-A] 行内子控件关键词命中判定（实现自两页逐字重复代码提取）。
bool UiHelpers_RowTextHitsKey(CControlUI* pRow, const CDuiString& sKey)
{
    if (!pRow) return false;
    // 静态转换取 ContainerUI，避免依赖 RTTI
    CContainerUI* pC = pRow->GetInterface(L"ContainerUI") ?
        static_cast<CContainerUI*>(pRow) : nullptr;
    if (!pC) return false;
    for (int c = 0; c < pC->GetCount(); ++c)
    {
        CControlUI* pSub = pC->GetItemAt(c);
        if (!pSub) continue;
        CDuiString sTxt = pSub->GetText();
        if (sTxt.IsEmpty()) continue;
        sTxt.MakeLower();
        if (sTxt.Find(sKey.GetData()) >= 0) return true;
    }
    return false;
}

// [R127-A] 依赖图对话框组装与展示（实现自两页逐字重复代码提取）。
void UiHelpers_ShowDepGraphDialog(IHostUi* host, const wchar_t* pszRoot,
                                  const std::vector<CDuiString>& vDeps)
{
    if (!host) return;

    const CDuiString sRoot(pszRoot ? pszRoot : L"");

    DepGraphData g;
    DependencyGraphService::Compose(sRoot, vDeps, g);

    CToolDialogPage dlg(host, EToolDlgKind::TD_DEPGRAPH);
    dlg.SetDialogDPI(host->HostDPI());
    // [R16][T4] 依赖图节点转服务层 DTO，经统一 Open(ToolOpenArgs) 通路注入。
    toolsvc::ToolOpenArgs args;
    args.vDepNodes.reserve(g.vNodes.size());
    for (size_t i = 0; i < g.vNodes.size(); ++i)
    {
        const DepGraphNode& q = g.vNodes[i];
        toolsvc::TDepNode z;
        z.sLabel  = q.sLabel.GetData();
        z.nParent = q.nParent;
        z.nLevel  = q.nLevel;
        args.vDepNodes.push_back(std::move(z));
    }
    dlg.Open(args);
    dlg.Create(host->HostHwnd(), nullptr, UI_WNDSTYLE_DIALOG, 0);
    dlg.ShowModal();
}
