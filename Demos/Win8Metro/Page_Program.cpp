#include "stdafx.h"
#include "OfflineSession.h"  // [R10-02] 离线会话经独立组件访问，去 CDataEngine 静态耦合
// ============================================================
// Page_Program.cpp —— v2 P3a 程序和功能页（已安装程序）
// 5 个原 CFrameWnd 程序页函数的物理搬迁目标，函数体零逻辑改动，
// 仅把宿主引用从隐式 CFrameWnd 成员改为 m_ctx.Xxx（
// FindControl/Hwnd/Log/HostConfirmWarn）。
// 三源 Uninstall 枚举在 ProgramService（S4），注册表读取在 Registry。
// ============================================================
#include "Page_Program.h"
#include "ProgramCacheService.h"
#include "SafeThread.h"
#include "WmMessages.h"
#include "NotifyBox.h"


#include "IPageHost.h"           // [R5] HostPageHost()->PageById（prog_pages Tab1 代理）
#include "CustomControls.h"      // CClickRowUI（prow_%d 行）
#include "StringManager.h"       // StringMgr 本地化宏
#include "LogService.h"         // [M-7] 统一日志目录（fix.log 读写两侧）
#include "FrameWnd_Util.h"       // EnsureIconCacheDirectory/EnsureIconPngReady
#include "Registry.h"            // RegGetString（ChangeCommandLine 读取）
#include "DataEngine.h"          // [P0] WinPE 离线会话门控
#include <memory>                // [C++17 R3] make_unique
// [C++17 阶段2] 局部 RAII 守卫：文件句柄（见 WinHandles.h 使用规范）
struct PPFileGuard
{
    HANDLE h;
    explicit PPFileGuard(HANDLE x) : h(x) {}
    ~PPFileGuard() { if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h); }
    PPFileGuard(const PPFileGuard&) = delete;
    PPFileGuard& operator=(const PPFileGuard&) = delete;
    operator HANDLE() const { return h; }
};


using namespace DuiLib;
using i18n::CStringManager;      // 便捷：直接写 CStringManager 不用前缀





// [R167] 从 ProgShowFixLog 抽出：读 fix.log 并输出最后若干条。
//   返回 false 表示没读到可展示内容（文件不存在 / 为空 / 读失败 / 转码失败）。
//   五层"如果一切正常就继续"串成一条线性流程 —— 外层不需要区分失败原因，
//   但**空文件与其它失败要分开处理**：空文件是正常状态（还没修复过），
//   给用户看"日志为空"比什么都不说更清楚；读取失败才记 WARN。
bool CPageProgram::ProgReadFixLogAndShow()
{
    const CDuiString sLogPath = LogService::UnifiedLogPath(L"fix.log").c_str();

    PPFileGuard h(::CreateFile(sLogPath.GetData(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (h == INVALID_HANDLE_VALUE) return false;

    const DWORD nSize = ::GetFileSize(h, nullptr);
    if (nSize == 0 || nSize == INVALID_FILE_SIZE)
    {
        m_ctx.HostLog(StringMgr.Get("misc.he6de"));
        return false;
    }

    // [R162] 缓冲为 std::string：它只承载 UTF-8 字节流，
    // ReadFile 与 MultiByteToWideChar 都能直接吃 data()，不需要 BYTE*→char* 转换。
    std::string vBuf(static_cast<size_t>(nSize), static_cast<char>(0));
    DWORD nRd = 0;
    ::ReadFile(h, &vBuf[0], nSize, &nRd, nullptr);

    const char* pszUtf8 = vBuf.data();
    const int nBytes = static_cast<int>(nRd);
    const int nW = ::MultiByteToWideChar(CP_UTF8, 0, pszUtf8, nBytes, nullptr, 0);
    if (nW <= 0) return false;

    std::vector<wchar_t> wBuf(static_cast<size_t>(nW) + 1);
    ::MultiByteToWideChar(CP_UTF8, 0, pszUtf8, nBytes, wBuf.data(), nW);
    wBuf[static_cast<size_t>(nW)] = 0;
    const CDuiString sContent(wBuf.data(), nW);

    // 按行拆分，取最后 10 条（[N11] 拆行与末段输出已下沉为文件级纯函数）
    const std::vector<CDuiString> vLines = SplitFixLogLines(sContent);
    LogFixLogTail(m_ctx, vLines);
    return true;
}

void CPageProgram::ProgShowFixLog()
{
    // [L-24][M-7] fix.log 读侧并入统一日志目录。
    //   必须与写侧同批切换：只改一边会出现"记录写进去了、页面却读不到"。
    //   [R167] 读取与输出整段抽为 ProgReadFixLogAndShow，嵌套 5→2
    (void)ProgReadFixLogAndShow();
}


void CPageProgram::ProgLogIconStat()
{
    // [FIX-PERF] 原实现遍历全部程序并调用 EnsureIconPngReady（会触发图标提取与落盘），
    // 在列表构建尾部同步执行，等同于把"为所有程序生成图标"塞进首屏路径。
    // 现在只统计"有 DisplayIcon 的程序数"，不再触发生成；图标按行/按需生成。
    int nWithIcon = 0;
    for (size_t k = 0; k < m_vPrograms.size(); ++k)
        if (!m_vPrograms[k].sIconBase.IsEmpty()) ++nWithIcon;
    if (nWithIcon > 0)
    {
        CDuiString sIconStat;
        sIconStat.Format(StringMgr.Get("misc.h5d1e"),
                         nWithIcon, static_cast<int>(m_vPrograms.size()), 0);
        m_ctx.HostLog(sIconStat.GetData());
    }
}
void CPageProgram::Build()
{
    // [R55] 程序和功能页首建图标缓存目录（%LOCALAPPDATA%\DismTool_Clone\icons）
    EnsureIconCacheDirectory();
    // [FIX-PERF] 首屏优先：先用上次缓存立即渲染，注册表全量枚举移至后台。
    std::vector<ProgEntry> vCache;
    if (ProgramCacheService::Load(vCache) && !vCache.empty())
    {
        RenderProgramList(vCache);
        m_ctx.HostLog(L"[program] cached list rendered, refreshing in background");
        StartProgramRefresh();
        return;
    }
    // 无缓存：显示加载态后后台枚举，不阻塞 UI 线程。
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"prog_apps_list"));
    if (pList)
    {
        pList->RemoveAll();
        // [R228] RemoveAll() destroyed the rows; m_vRowPtr still holds those raw
        //   pointers. OnRowSelect / RefreshThemeColors walk the cache and only test for
        //   null, so a stale non-null pointer is dereferenced -> use-after-free.
        //   Same defect class as Page_Capability / Page_Feature, found on the
        //   "no cache yet, show loading hint" path.
        m_vRowPtr.clear();
        pList->Add(m_ctx.HostMakeLoadingHint(
            m_ctx.HostSkinText(L"lbl_loading"), L"cap_ring"));
    }
    StartProgramRefresh();
}
// [FIX-PERF] 渲染已枚举的程序列表（UI 线程）
void CPageProgram::RenderProgramList(const std::vector<ProgEntry>& v)
{
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"prog_apps_list"));
    if (!pList) return;
    // [R23-FIX] 重建前必须停掉在途的分帧定时器：
    // 原实现未 KillTimer，旧接力会继续按旧 m_nRenderPos 向刚 RemoveAll 的新列表
    // 追加行，导致重复行与下标错位（与 Feature 页踩过的同类问题一致）。
    ::KillTimer(m_ctx.HostHwnd(), PROG_RENDER_TIMER_ID);
    // 搜索防抖若有在途请求，也用新列表重新计算，避免旧关键字套到新数据上。
    ::KillTimer(m_ctx.HostHwnd(), PROG_FILTER_TIMER_ID);
    pList->RemoveAll();
    m_nSel = -1;
    m_vPrograms = v;
    // [R9] 行缓存与数据严格同下标：重建先整体重置，避免旧指针悬挂。
    m_vRowPtr.clear();
    m_vRowSearch.clear();
    m_vRowPtr.resize(v.size(), nullptr);
    m_vRowSearch.resize(v.size());
    CButtonUI* pBtnDel = static_cast<CButtonUI*>(m_ctx.HostFindControl(L"btn_prog_delete"));
    if (pBtnDel) pBtnDel->SetVisible(false);
    // [FIX-PERF] 首屏分帧：先画可视区约 40 行，其余按 6ms 预算接力，
    // 避免数百行 × 多控件在单帧内一次性创建造成切页卡顿。
    m_nRenderPos = 0;
    const size_t nFirst = v.size() < 40 ? v.size() : 40;
    for (size_t k = 0; k < nFirst; ++k)
        ProgBuildRow(v[k], k);
    m_nRenderPos = nFirst;
    if (m_nRenderPos < v.size())
        ::SetTimer(m_ctx.HostHwnd(), PROG_RENDER_TIMER_ID, 1, nullptr);
    else
    {
        ProgShowFixLog();
        ProgLogIconStat();
    }
    // [R23-FIX] 新列表落地后重新应用当前搜索关键字，
    // 避免"先输入关键字、后刷新列表"时过滤被清空。
    if (!m_ctx.HostReadEditValue(L"prog_filter").IsEmpty())
        PrgApplyFilter();
}
namespace
{
DWORD WINAPI ProgEnumThreadProc(void* pv)
{
    std::unique_ptr<ProgEnumCtx> ctx(static_cast<ProgEnumCtx*>(pv));
    if (!ctx) return 0;
    ProgramService::EnumeratePrograms(ctx->v);
    // [R27] 缓存写入失败不应阻断枚举结果回投，但要留下痕迹：
    // 否则下次进页会读到旧列表，表现为"卸载了程序但列表没变"。
    if (!ProgramCacheService::Save(ctx->v))
        ::OutputDebugStringW(L"[program] cache save failed (list still shown)\r\n");
    // [R17-FIX-P0] 必须按"投递是否成功"决定所有权，不能用 v.empty() 判断。
    // 原实现在空的枚举结果下会先 PostMessage 移交指针、随后因 v.empty() 提前
    // return，unique_ptr 析构把已投递给 UI 的对象释放掉，UI 侧收到悬垂指针。
    bool bPosted = false;
    if (::IsWindow(ctx->hWnd))
        bPosted = (::PostMessageW(ctx->hWnd, WM_PROG_DATA, 0,
                                  msgparam::ToParam(ctx.get())) != FALSE);
    if (bPosted)
    {
        // 所有权移交 UI 线程（含空列表：UI 需要用它把加载态清成"无程序"）。
        (void)ctx.release();
    }
    else
    {
        // 窗口已销毁或投递失败：本线程负责释放，并清掉可能残留的加载态。
        if (::IsWindow(ctx->hWnd))
            ::PostMessageW(ctx->hWnd, WM_PROG_DATA, 0, 0);
    }
    return 0;
}
} // namespace
// [FIX-PERF] 后台枚举：避免 UI 线程同步扫三个注册表源 + 逐项查询签名。
// [R3-FIX] generation 守卫：递增 m_nProgGen，透传到线程上下文；UI 侧收到后校验，
// 过期消息（上一次枚举）直接丢弃，防止"快速刷新回传错乱"。
void CPageProgram::StartProgramRefresh()
{
    ++m_nProgGen;
    const int nGen = m_nProgGen;
    auto ctx = std::make_unique<ProgEnumCtx>();
    ctx->hWnd = m_ctx.HostHwnd();
    ctx->nGen = nGen;
    HANDLE h = SafeCreateThread(&ProgEnumThreadProc, ctx.get());
    if (!h)
    {
        // 线程创建失败：退回同步，保证功能可用
        ProgramService::EnumeratePrograms(ctx->v);
        if (!ProgramCacheService::Save(ctx->v))
            ::OutputDebugStringW(L"[program] cache save failed (inline path)\r\n");
        RenderProgramList(ctx->v);
        return;
    }
    ctx.release();
    ::CloseHandle(h);
}
// [FIX-PERF] 后台枚举完成：替换列表（保留搜索框关键字）
// [INTERACT-FIX] 新增 WM_PROG_BATCHDONE：批量卸载后台完成收口
// （日志汇总/Toast/失败分级/列表刷新——原在 UI 线程同步循环内联执行）。
bool CPageProgram::OnAsyncMessage(UINT uMsg, WPARAM, LPARAM lParam)
{
    // [R162] 整数还原指针：先转 uintptr_t 再转指针，比 reinterpret_cast 直转
    //   更明确表达意图（LPARAM 本就是把指针当整数传的载体），也不依赖别名规则。
    if (uMsg == WM_PROG_BATCHDONE)
    {
        std::unique_ptr<ProgBatchMsg> msg(
            reinterpret_cast<ProgBatchMsg*>(static_cast<std::uintptr_t>(lParam)));
        if (!msg) return true;
        m_bBatchUninstalling = false;
        for (const auto& kv : msg->vFailures)
        {
            CDuiString sLog;
            sLog.Format(StringMgr.Get("misc.sf4f17c78"), kv.first.GetData(), kv.second);
            m_ctx.HostLog(sLog.GetData(), static_cast<int>(ELogLevel::LOG_ERROR));   // [R113] 单项失败升 ERROR
        }
        CDuiString sSummary;
        sSummary.Format(StringMgr.Get("misc.s4a787ee6"),
                        msg->nOk, msg->nFail, msg->nTotal);
        m_ctx.HostLog(sSummary.GetData());
        CDuiString sToast;
        sToast.Format(StringMgr.Get("misc.sbaa793d4"), msg->nOk, msg->nFail);
        m_ctx.HostNotify(StringMgr.Get("misc.s6da661b2"), sToast.GetData(), 0);
        // [R113] 批量卸载失败分级 Toast：全部失败 / 部分失败 → 错误图标 + LOG_WARN
        if (msg->nFail > 0)
        {
            CDuiString sWarn;
            sWarn.Format(StringMgr.Get("program.s_batch_fail"),
                         msg->nOk, msg->nFail, msg->nTotal);
            m_ctx.HostLog(sWarn.GetData(), static_cast<int>(ELogLevel::LOG_WARN));
            m_ctx.HostToast(StringMgr.Get("program.s_batch_fail_title"),
                            sWarn.GetData(), 2);
        }
        // 卸载完成后刷新列表
        Build();
        return true;
    }
    if (uMsg != WM_PROG_DATA) return false;
    std::unique_ptr<ProgEnumCtx> ctx(
        reinterpret_cast<ProgEnumCtx*>(static_cast<std::uintptr_t>(lParam)));
    if (!ctx) return true;
    // [R3-FIX] generation 守卫：过期消息直接丢弃，不覆盖当前结果
    if (ctx->nGen != 0 && ctx->nGen != m_nProgGen)
    {
        m_ctx.HostLog(L"[program] stale ProgEnumCtx dropped (gen mismatch)");
        return true;
    }
    RenderProgramList(ctx->v);
    PrgApplyFilter();
    return true;
}




// [FIX-PERF] 首屏分帧接力：每帧约 6ms，避免一次性创建数百行。
void CPageProgram::RenderProgramStep()
{
    const ULONGLONG uStart = ::GetTickCount64();
    while (m_nRenderPos < m_vPrograms.size())
    {
        ProgBuildRow(m_vPrograms[m_nRenderPos], m_nRenderPos);
        ++m_nRenderPos;
        if (::GetTickCount64() - uStart >= 6) break;
    }
    if (m_nRenderPos < m_vPrograms.size())
    {
        ::SetTimer(m_ctx.HostHwnd(), PROG_RENDER_TIMER_ID, 1, nullptr);
        return;
    }
    ::KillTimer(m_ctx.HostHwnd(), PROG_RENDER_TIMER_ID);
    ProgShowFixLog();
    ProgLogIconStat();
}
void CPageProgram::OnHostTimer(UINT uId)
{
    if (uId == PROG_RENDER_TIMER_ID) { RenderProgramStep(); return; }
    if (uId == PROG_FILTER_TIMER_ID)
    {
        ::KillTimer(m_ctx.HostHwnd(), PROG_FILTER_TIMER_ID);
        PrgApplyFilter();
    }
}
// [R223] Re-apply theme colours after a light<->dark switch.
//
// PageBase.h contract: colours only. No Build()/Add/Remove here -- doing so would drop
// the list contents, the selection and the scroll position, and leave m_pFocusCtrl
// dangling.
//
// It reuses OnRowSelect's own loop on purpose: selection colouring has exactly one
// definition, so "build-time colours", "select-time colours" and "switch-theme colours"
// cannot drift apart.
void CPageProgram::RefreshThemeColors()
{
    const size_t nRows = m_vRowPtr.size();
    for (size_t k = 0; k < nRows; ++k)
    {
        CClickRowUI* pRow = dynamic_cast<CClickRowUI*>(m_vRowPtr[k]);
        if (!pRow) continue;
        const bool bSel = (static_cast<int>(k) == m_nSel);
        pRow->SetRowSelected(bSel);   // handles the row background for both states
        for (int c = 0; c < pRow->GetCount(); ++c)
        {
            CTextUI* pText = dynamic_cast<CTextUI*>(pRow->GetItemAt(c));
            if (!pText) continue;
            if (bSel)
            {
                pText->SetTextColor(tokens::Token(tokens::EToken::OnAccent));   // on the accent fill
                continue;
            }
            // Column semantics are positional (see ProgBuildRow): 1=name, 2=root,
            // 3=arch, 4=publisher, 5=signature, 6=date, 7=size. Index 0 is the checkbox.
            switch (c)
            {
            case 1: pText->SetTextColor(tokens::Token(tokens::EToken::Ink)); break;
            case 2:
            case 3:
            {
                // root/arch use per-state colours; recompute from the row's own data so
                // they match what ProgBuildRow would paint today.
                const ProgEntry& e = m_vPrograms[k];
                if (c == 2)
                    pText->SetTextColor(e.nRoot == 2 ? tokens::Token(tokens::EToken::Warn)
                                                     : tokens::Token(tokens::EToken::Accent));
                else
                    pText->SetTextColor(e.nRoot == 0 ? tokens::Token(tokens::EToken::Accent)
                                    : (e.nRoot == 1 ? tokens::Token(tokens::EToken::Success)
                                                    : tokens::Token(tokens::EToken::Warn)));
                break;
            }
            case 5:
            {
                const ProgEntry& e = m_vPrograms[k];
                pText->SetTextColor(e.bMsft   ? tokens::Token(tokens::EToken::Accent)
                                  : e.bSigned ? tokens::Token(tokens::EToken::Success)
                                              : tokens::Token(tokens::EToken::Danger));
                break;
            }
            default:
                pText->SetTextColor(tokens::Token(tokens::EToken::InkSecondary));
                break;
            }
        }
    }
}

void CPageProgram::OnRowSelect(int n)
{
    m_nSel = n;
    // [R9] 直接用缓存行指针更新选中态，不再逐行 Format("prow_%d") 后查找。
    const size_t nRows = m_vRowPtr.size();
    for (size_t k = 0; k < nRows; ++k)
    {
        CClickRowUI* pRow = static_cast<CClickRowUI*>(m_vRowPtr[k]);
        if (!pRow) continue;
        const bool bSel = (static_cast<int>(k) == n);
        // [R223] The non-selected colour was the literal 0xFF1A1A1A -- near-black, which
        //   is invisible on a dark row. Use the theme's Ink instead. The cast is also
        //   checked now: row children include a checkbox, and a blind static_cast to
        //   CTextUI on it would be undefined behaviour.
        pRow->SetRowSelected(bSel);
        for (int c = 0; c < pRow->GetCount(); ++c)
        {
            CTextUI* pText = dynamic_cast<CTextUI*>(pRow->GetItemAt(c));
            if (pText)
                pText->SetTextColor(bSel ? tokens::Token(tokens::EToken::OnAccent) : tokens::Token(tokens::EToken::Ink));
        }
    }
    // reveal the delete button only when the selected row can be removed
    CButtonUI* pBtnDel = static_cast<CButtonUI*>(m_ctx.HostFindControl(L"btn_prog_delete"));
    if (pBtnDel) {
        bool bCan = (n >= 0 && n < static_cast<int>(m_vPrograms.size())
                     && !m_vPrograms[n].sUninstall.IsEmpty());
        pBtnDel->SetVisible(bCan);
    }
}



void CPageProgram::ShowContextMenu(int n)
{
    if (n < 0 || n >= static_cast<int>(m_vPrograms.size())) return;



    OnRowSelect(n);



    HMENU hMenu = ::CreatePopupMenu();
    bool bCan = !m_vPrograms[n].sUninstall.IsEmpty();
    ::AppendMenu(hMenu, MF_STRING | (bCan ? 0 : MF_GRAYED), 4201, StringMgr.Get("program.sbc78d8a8"));
    // R34: 修改/修复（仅 MSI 程序，ProductCode 从子键 {GUID} 提取或从 ChangeCommandLine 值读）
    bool bCanModify = m_vPrograms[n].bIsMsi;
    ::AppendMenu(hMenu, MF_STRING | (bCanModify ? 0 : MF_GRAYED), 4202, StringMgr.Get("misc.h0ce6"));



    POINT pt; ::GetCursorPos(&pt);
    int iCmd = ::TrackPopupMenu(hMenu,
        TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
        pt.x, pt.y, 0, m_ctx.HostHwnd(), nullptr);
    ::DestroyMenu(hMenu);



    if (iCmd == 4201 && bCan) Uninstall(n);
    else if (iCmd == 4202 && bCanModify) Modify(n);
}



// [R162] 从 Modify 抽出：构造"修改/修复"命令行。
//   优先级与原实现一致：ChangeCommandLine（MSI 官方推荐）>
//   ProductCode 拼 MsiExec.exe /fa > 都没有则放弃（记 misc.h1da1 并返回 false）。
//   ProductCode 从注册表子键名里截取第一个 {GUID}；越界用 ptrdiff_t→int 显式转换。
[[nodiscard]] bool CPageProgram::BuildModifyCommand(const ProgEntry& e,
                                                    CDuiString& sCmd, CDuiString& sProduct)
{
    // 优先读 ChangeCommandLine（MSI 官方推荐）
    CDuiString sChange = RegGetString(
        e.nRoot == 2 ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE,
        e.sSubKey.GetData(), L"ChangeCommandLine");

    // 提取 ProductCode {GUID}
    const wchar_t* p = e.sSubKey.GetData();
    const wchar_t* pBrace = wcschr(p, L'{');
    if (pBrace)
    {
        const wchar_t* pClose = wcschr(pBrace, L'}');
        if (pClose) sProduct = CDuiString(pBrace, static_cast<int>(pClose - pBrace + 1));   // [B6] ptrdiff_t->int cast
    }

    if (!sChange.IsEmpty())
    {
        sCmd = sChange;
        return true;
    }
    if (!sProduct.IsEmpty())
    {
        sCmd = L"MsiExec.exe /fa ";
        sCmd += sProduct;
        return true;
    }
    m_ctx.HostLog(StringMgr.Get("misc.h1da1"));
    return false;
}

// [R162] 从 Modify 抽出：[R42] 修复日志持久化（追加 JSONL 到统一日志目录 fix.log）。
//   [L-24][M-7] 写侧并入统一日志目录（LogService::UnifiedLogPath），
//   该目录已含"逐级建目录 + 可写性探针 + 回退链"，不必手工 EnsureDirectory。
//   [L-24] 格式改 JSONL：字段化后读侧按字段取值，不再靠 " | " 切分 ——
//   程序名或命令行里一旦出现竖线，旧的定长分隔就会错列。
//   写盘失败只告警不阻断：修复业务已经成功，日志写不进去不该报成修复失败。
void CPageProgram::AppendFixLog(const CDuiString& sName, const CDuiString& sProduct,
                                const CDuiString& sCmd)
{
    CDuiString sLogPath = LogService::UnifiedLogPath(L"fix.log").c_str();

    SYSTEMTIME st; ::GetLocalTime(&st);
    CDuiString sLine;
    sLine.Format(L"{\"ts\":\"%04d-%02d-%02dT%02d:%02d:%02d.%03d\","
                 L"\"name\":\"%s\",\"product\":\"%s\",\"cmd\":\"%s\"}",
                 st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                 sName.GetData(),
                 sProduct.IsEmpty() ? L"(no GUID)" : sProduct.GetData(),
                 sCmd.GetData());
    if (sLine.GetLength() > 0 && sLine[sLine.GetLength() - 1] != L'\n')
        sLine += L"\r\n";

    // [R162] 缓冲改 std::string：UTF-8 字节本就以 char 承载，
    //   原先的 BYTE* → char* 两处 reinterpret_cast 从类型上消失
    //   （WriteFile 形参是 LPCVOID，data() 直接可用）。
    const int nUtf8 = ::WideCharToMultiByte(CP_UTF8, 0, sLine.GetData(), -1, nullptr, 0, nullptr, nullptr);
    if (nUtf8 <= 0) return;
    std::string buf(static_cast<size_t>(nUtf8) + 3, '\0');

    PPFileGuard hL(::CreateFile(sLogPath.GetData(), GENERIC_WRITE, 0, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (hL == INVALID_HANDLE_VALUE)
    {
        m_ctx.HostLog(StringMgr.Get("misc.h0182"), static_cast<int>(ELogLevel::LOG_WARN));
        return;
    }

    ::SetFilePointer(hL, 0, nullptr, FILE_END);
    DWORD nWritten = 0;
    if (::GetFileSize(hL, nullptr) == 0)
    {
        // 空文件才写 BOM（首行 BOM）；写失败仅告警
        buf[0] = static_cast<char>(0xEF);
        buf[1] = static_cast<char>(0xBB);
        buf[2] = static_cast<char>(0xBF);
        if (!::WriteFile(hL, buf.data(), 3, &nWritten, nullptr) || nWritten != 3)
        {
            m_ctx.HostLog(StringMgr.Get("misc.h0182"), static_cast<int>(ELogLevel::LOG_WARN));
            return;
        }
        ::WideCharToMultiByte(CP_UTF8, 0, sLine.GetData(), -1, &buf[3], nUtf8, nullptr, nullptr);
        if (!::WriteFile(hL, buf.data() + 3, static_cast<DWORD>(nUtf8), &nWritten, nullptr)
            || nWritten != static_cast<DWORD>(nUtf8))
            m_ctx.HostLog(StringMgr.Get("misc.h0182"), static_cast<int>(ELogLevel::LOG_WARN));
    }
    else
    {
        ::WideCharToMultiByte(CP_UTF8, 0, sLine.GetData(), -1, &buf[0], nUtf8, nullptr, nullptr);
        if (!::WriteFile(hL, buf.data(), static_cast<DWORD>(nUtf8), &nWritten, nullptr)
            || nWritten != static_cast<DWORD>(nUtf8))
            m_ctx.HostLog(StringMgr.Get("misc.h0182"), static_cast<int>(ELogLevel::LOG_WARN));
    }

    CDuiString sLogWrite;
    sLogWrite.Format(StringMgr.Get("misc.h6435"), sLogPath.GetData());
    m_ctx.HostLog(sLogWrite.GetData());
}

void CPageProgram::Modify(int n)
{
    if (n < 0 || n >= static_cast<int>(m_vPrograms.size())) return;
    const ProgEntry& e = m_vPrograms[n];
    if (!e.bIsMsi)
    {
        m_ctx.HostLog(StringMgr.Get("misc.h9828"));
        return;
    }



    // [R53] 展示 DisplayIcon 基名（辅助诊断，不阻断）
    if (!e.sIconBase.IsEmpty())
    {
        CDuiString sIconLog;
        sIconLog.Format(StringMgr.Get("misc.hf31c"),
                        e.sIconBase.GetData());
        m_ctx.HostLog(sIconLog.GetData());
    }



    // [R162] 命令行构造抽为 BuildModifyCommand（ChangeCommandLine 优先，
    //   缺失则用 ProductCode 兜底，两者都无则放弃）。
    CDuiString sCmd;
    CDuiString sProduct;
    if (!BuildModifyCommand(e, sCmd, sProduct)) return;



    // 二次确认
    CDuiString sMsg;
    sMsg.Format(StringMgr.Get("misc.hb229"),
                e.sName.GetData(), sCmd.GetData());
    if (!m_ctx.HostConfirmWarn(StringMgr.Get("misc.h17a6"), sMsg.GetData()))
        return;
    std::wstring sCmdBuf = sCmd.GetData() ? sCmd.GetData() : L"";
    sCmdBuf.reserve(2048);   // 保留可写缓冲（CreateProcessW 形参为可写 LPWSTR）
    // ---- 单程序卸载：启动 UninstallString 并同步等待 ----
    // CreateProcessW 的 lpCommandLine 形参类型是 LPWSTR（可写），
    // 而 GetData() 返回 const wchar_t*，故先拷进 std::wstring 再取
    // &sCmdBuf[0]。reserve(2048) 保证这块内存在调用期间不会被重新
    // 分配，否则 &sCmdBuf[0] 会在 CreateProcessW 读取过程中失效。
    //
    // 成功分支内必须成对关闭 hThread 与 hProcess：
    //   hThread —— 本函数不等待该线程，仅用于确认进程已创建，可直接关；
    //   hProcess —— 卸载程序是交互式向导，等待其退出后再关，
    //               避免"卸载完成"提示与真实进度脱节。
    // 失败分支不产生有效句柄，无需关闭。
    STARTUPINFO si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    BOOL ok = ::CreateProcessW(nullptr, &sCmdBuf[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
    if (ok) {
        ::CloseHandle(pi.hThread);  // [阶段2 有意保留] PROCESS_INFORMATION 成功路径单出口显式关闭（沿用 ExternalToolService 约定）
        ::CloseHandle(pi.hProcess);
        CDuiString sLog;
        sLog.Format(StringMgr.Get("misc.h1bd8"), e.sName.GetData());
        m_ctx.HostLog(sLog.GetData());



        // [R162] fix.log 写入整段抽为 AppendFixLog（含 BOM 首行判定与 JSONL 拼装）
        AppendFixLog(e.sName, sProduct, sCmd);
    }
    else {
        CDuiString sLog;
        sLog.Format(StringMgr.Get("misc.h25ce"), (int)::GetLastError());
        m_ctx.HostLog(sLog.GetData());
    }
}



// [R175] 从 Uninstall 抽出：启动厂商卸载器，首次失败时尝试给 .exe 前加引号重试。
//   为什么要重试：UninstallString 常形如
//     C:\Program Files\Foo\uninst.exe /S
//   路径含空格且未加引号时，CreateProcessW 会把 C:\Program 当成程序名而失败。
//   重试只在首个 .exe 处加引号，其余参数原样保留 —— 这是 Windows 自身的解析规则。
//
//   与批量卸载的 PrgRunUninstallOne 不同：这里**不等待**进程结束。
//   单程序卸载打开的是交互式向导，等它会把 UI 卡住直到用户关掉向导。
[[nodiscard]] bool CPageProgram::PrgLaunchUninstaller(const CDuiString& sUninstall)
{
    auto runCmd = [](const wchar_t* pszCmd) -> bool {
        std::wstring sCmdBuf = pszCmd ? pszCmd : L"";
        sCmdBuf.reserve(2048);   // 可写缓冲：CreateProcessW 的 lpCommandLine 是 LPWSTR
        STARTUPINFO si = { sizeof(si) };
        PROCESS_INFORMATION pi = { 0 };
        const BOOL ok = ::CreateProcessW(nullptr, &sCmdBuf[0], nullptr, nullptr, FALSE,
                                         0, nullptr, nullptr, &si, &pi);
        if (!ok) return false;
        ::CloseHandle(pi.hThread);   // 不等待该线程，仅确认进程已创建
        ::CloseHandle(pi.hProcess);
        return true;
    };

    if (runCmd(sUninstall.GetData())) return true;

    // 失败后重试：给首个 .exe 加引号（处理含空格的长路径）
    CDuiString sCmd = sUninstall;
    const wchar_t* psz = sCmd.GetData();
    if (!psz || psz[0] == L'"') return false;

    int nCut = -1;
    for (int i = 0; i + 4 <= static_cast<int>(sCmd.GetLength()); ++i)
    {
        if (_wcsnicmp(psz + i, L".exe", 4) == 0) { nCut = i + 4; break; }
    }
    if (nCut <= 0) return false;

    CDuiString sFixed = L"\"";
    sFixed += sCmd.Mid(0, nCut);
    sFixed += L"\"";
    sFixed += sCmd.Mid(nCut);
    return runCmd(sFixed.GetData());
}

void CPageProgram::Uninstall(int n)
{
    if (n < 0 || n >= static_cast<int>(m_vPrograms.size())) return;
    // [P0] WinPE 离线会话：卸载器必须在运行的目标系统内执行，离线无法卸载
    if (COfflineSession::IsActive())
    {
        m_ctx.HostNotify(L"WinPE offline mode",
                         L"Programs cannot be uninstalled from an offline image. "
                            "Boot the target system and uninstall there.", 2);
        return;
    }
    const ProgEntry& e = m_vPrograms[n];
    if (e.sUninstall.IsEmpty()) {
        m_ctx.HostLog(StringMgr.Get("program.s0a575ef0"));
        return;
    }



    // confirmation: launches the vendor's own uninstaller
    CDuiString sMsg;
    sMsg.Format(StringMgr.Get("update.s00f8bac3"),
                e.sName.GetData());
    if (!m_ctx.HostConfirmWarn(StringMgr.Get("program.s8a090d1e"), sMsg.GetData()))
        return;



    // [R175] 启动卸载器（含长路径引号重试）抽为 PrgLaunchUninstaller，嵌套 4→2
    const bool bOk = PrgLaunchUninstaller(e.sUninstall);



    CDuiString sLog;
    if (bOk) {
        sLog.Format(StringMgr.Get("program.s678c146e"), e.sName.GetData());
        m_ctx.HostLog(sLog.GetData());
        // [FIX] 卸载器已启动：缓存立即失效，避免下次进入仍展示已卸载程序。
        ProgramCacheService::Invalidate();
    } else {
        sLog.Format(StringMgr.Get("error.s0a5d5ed1"),
                    e.sName.GetData(), (int)::GetLastError());
        m_ctx.HostLog(sLog.GetData(), static_cast<int>(ELogLevel::LOG_ERROR));
        // [R113] 单条卸载失败分级 Toast + 错误码回传
        CDuiString sToastErr;
        sToastErr.Format(L"%s | err=0x%08X",
                         e.sName.GetData(), (unsigned)::GetLastError());
        m_ctx.HostToast(StringMgr.Get("program.s_uninst_fail_title"),
                        sToastErr.GetData(), 2);
    }
}
// ============================================================
// P8：第二级事件路由（原 CFrameWnd::Notify / WM_RBUTTONUP /
// VK_DELETE 程序页分支逐字迁入）
// 例外（跨 Program/Feature 共享容器，保留壳分派）：
//   btn_prog_refresh / btn_prog_delete / progtab_ / prog_filter。
// ============================================================
void CPageProgram::OnEvent(TNotifyUI& msg)
{
    // [AUDIT-P2] 原 146 行 / CC=37 的单事件函数，按事件类型拆为 4 个
    // 处理函数：click / Tab 切换 / 关键字过滤 / 双击。
    // 路由语义逐字保留（含 Tab1 向 Feature 代理的兜底转发）。
    CDuiString sName = msg.pSender ? msg.pSender->GetName() : CDuiString(L"");
    if (msg.sType == L"click")
    {
        // [BUGFIX-20261025] AUDIT-P2 拆分时遗留的回归：原实现先 return，
        // 末尾的 ForwardFeatureEventInTab1 永远不可达，Tab1 的 feat_row_/
        // feat_arrow_ 点击全部被吞 → 勾选/展开/选中失效。改为按"是否消费"
        // 决定兜底转发（逐字等价原壳 Notify 未命中即落功能页分支的语义）。
        if (!PrgOnClick(sName))
            ForwardFeatureEventInTab1(msg);
        return;
    }
    if (msg.sType == L"selectchanged"
        && wcsncmp(sName.GetData(), L"progtab_", 8) == 0)
    {
        PrgOnTabChanged(sName);
        return;
    }
    if (msg.sType == L"textchanged" && sName == L"prog_filter")
    {
        // [FIX-PERF] 输入防抖：连续键入只在上一次输入 180ms 后执行一次过滤，
        // 避免逐字符对整表逐行 FindControl + SetVisible 造成布局抖动。
        // [R23] 不再缓存关键字到成员：PrgApplyFilter 直接读取输入框当前值，
        // 定时器触发时拿到的就是最新输入，缓存字段属于冗余状态。
        ::KillTimer(m_ctx.HostHwnd(), PROG_FILTER_TIMER_ID);
        ::SetTimer(m_ctx.HostHwnd(), PROG_FILTER_TIMER_ID, 180, nullptr);
        return;
    }
    if (msg.sType == L"dbclick")
    {
        // [BUGFIX-20261025] 同 click：feat_row_ 双击选中需兜底转发到 Feature
        if (!PrgOnDoubleClick(sName))
            ForwardFeatureEventInTab1(msg);
        return;
    }
}
// [AUDIT-P2] click 分支：批量卸载 / 刷新 / 删除 / 行选中（按 Tab 分派）
// [BUGFIX-20261025] 返回是否消费：未消费时由 OnEvent 兜底转发 Feature
bool CPageProgram::PrgOnClick(const CDuiString& sName)
{
    // [R70] 批量卸载按钮
    if (sName == L"btn_prog_batch_uninstall")
    {
        BatchUninstallPrograms();
        return true;
    }
    // [R5] Program/Feature 共享 prog_pages：刷新/删除按当前 Tab 分派
    // （原壳 Notify 跨页例外；Tab1 经页宿主表代理到 Capability/Feature）
    if (sName == L"btn_prog_refresh")
    {
        CTabLayoutUI* pPages = static_cast<CTabLayoutUI*>(
            m_ctx.HostFindControl(L"prog_pages"));
    // [DBGFEAT] 诊断开关：仅 DISMTOOL_FEAT_TRACE 存在时输出；曾无条件写日志污染日志区
    if (::GetEnvironmentVariableW(L"DISMTOOL_FEAT_TRACE", nullptr, 0) > 0
        && (sName.Find(L"feat") >= 0 || sName.Find(L"btn_prog") >= 0
            || sName.Find(L"btn_feat") >= 0))
    {
        CDuiString sDbg;
        sDbg.Format(L"[DBGFEAT] Program::OnEvent type=%s name=%s progtab=%d",
                    L"click", sName.GetData(),
                    pPages ? pPages->GetCurSel() : -9);
        m_ctx.HostLog(sDbg.GetData());
    }
        if (pPages && pPages->GetCurSel() == 1)
        {
            if (CPageBase* pFeat = m_ctx.HostPageHost()->PageById(EPageId::PAGE_FEATURE))
                pFeat->HostFeatureLoad();
        }
        else
        {
            // [FIX] 手动刷新强制后台重新枚举；保留当前列表直到新结果到达，
            // 避免闪空，同时不使用缓存造成“刷新无变化”的错觉。
            StartProgramRefresh();
        }
        return true;
    }
    if (sName == L"btn_prog_delete")
    {
        CTabLayoutUI* pPages = static_cast<CTabLayoutUI*>(
            m_ctx.HostFindControl(L"prog_pages"));
        int nProgTab = pPages ? pPages->GetCurSel() : 0;
        if (nProgTab == 0)
        {
            // P-1: 统一删除入口 Tab0 —— 卸载当前选中程序
            Uninstall(Selected());
        }
        else if (CPageBase* pFeat = m_ctx.HostPageHost()->PageById(EPageId::PAGE_FEATURE))
        {
            // P5：Tab1 Windows 功能永久删除
            pFeat->HostFeatureDeleteSelected();
        }
        return true;
    }
    int nProgRow = -1;
    if (ParsePrefix1(sName, L"prow_", nProgRow))
    {
        OnRowSelect(nProgRow);
        return true;
    }
    // 未消费：feat_row_/feat_arrow_/btn_prog_apply/btn_feat_browse 等
    return false;
}
// [AUDIT-P2] prog_pages Tab 切换（原壳 progtab_ selectchanged 分支）
void CPageProgram::PrgOnTabChanged(const CDuiString& sName)
{
    int idx = _ttoi(sName.GetData() + 8);
    CTabLayoutUI* p = static_cast<CTabLayoutUI*>(
        m_ctx.HostFindControl(L"prog_pages"));
    if (p) p->SelectItem(idx);
    if (CPageBase* pFeat = m_ctx.HostPageHost()->PageById(EPageId::PAGE_FEATURE))
        pFeat->HostFeatureTabChanged(idx);   // 功能 Tab 图例/按钮态
    if (idx != 1)
        OnRowSelect(Selected());             // 程序 Tab 刷新底部按钮态
    return;
}
// [AUDIT-P2] 程序/功能 实时关键字过滤（原壳 ApplyProgFilter 下沉）
// [R175] 从 PrgApplyFilter 抽出：Tab0（程序列表）的关键字过滤。
//   [R9] 用缓存行指针 + 预计算小写搜索串：不再逐行 Format("prow_%d") +
//   HostFindControl + GetText —— 1000 条时那是上千次名称查找与字符串分配，
//   是进页/输入卡顿的主要来源。
//   关键字为空时全显（bNoKw 快速路径，避免对每行调 Find）。
void CPageProgram::PrgApplyFilterTab0(const CDuiString& sKeyword, bool bNoKw)
{
    const size_t n = m_vPrograms.size();
    for (size_t k = 0; k < n; ++k)
    {
        CControlUI* pRow = (k < m_vRowPtr.size()) ? m_vRowPtr[k] : nullptr;
        if (!pRow) continue;
        if (bNoKw) { pRow->SetVisible(true); continue; }
        const CDuiString& sSearch = (k < m_vRowSearch.size()) ? m_vRowSearch[k] : CDuiString();
        pRow->SetVisible(sSearch.Find(sKeyword.GetData()) >= 0);
    }
}

void CPageProgram::PrgApplyFilter()
{
    CTabLayoutUI* pPages = static_cast<CTabLayoutUI*>(
        m_ctx.HostFindControl(L"prog_pages"));
    int nTab = pPages ? pPages->GetCurSel() : 0;
    CDuiString sKeyword = m_ctx.HostReadEditValue(L"prog_filter");
    sKeyword.MakeLower();
    bool bNoKw = sKeyword.IsEmpty();
    if (nTab == 0)
    {
        // [R175] Tab0 过滤抽为 PrgApplyFilterTab0，嵌套 4→2
        PrgApplyFilterTab0(sKeyword, bNoKw);
    }
    else
    {
        // Tab1 Windows 功能关键字过滤（Capability 代理 Feature）
        if (CPageBase* pFeat = m_ctx.HostPageHost()->PageById(EPageId::PAGE_FEATURE))
            pFeat->HostFeatureApplyFilter(sKeyword, bNoKw);
    }
    return;
}
// [AUDIT-P2] 双击行 = 卸载
// [BUGFIX-20261025] 返回是否消费：feat_row_ 双击选中需兜底转发 Feature
bool CPageProgram::PrgOnDoubleClick(const CDuiString& sName)
{
    int a = -1;
    if (ParsePrefix1(sName, L"prow_", a)) { Uninstall(a); return true; }
    return false;
}
// P8：原 CFrameWnd WM_RBUTTONUP prow_ 命中块（仅鼠标消息）
// [R175] 从 OnContextMenu 抽出：命中控件向上找 prow_N，返回行号（-1 = 未命中）。
//   越界不判：程序行名与 m_vPrograms 的下标一一对应，ShowContextMenu 内部会再校验。
[[nodiscard]] int CPageProgram::PrgRowFromHit(CControlUI* pHit)
{
    for (CControlUI* p = pHit; p; p = p->GetParent())
    {
        const wchar_t* psz = p->GetName();
        if (!psz) continue;
        if (wcsncmp(psz, L"prow_", 5) == 0) return _ttoi(psz + 5);
    }
    return -1;
}

// [R175] 从 OnContextMenu 抽出：Tab1（Windows 功能）的右键转发给 Feature 子视图。
//   为什么需要代理：该 Tab 的行由 Feature 页渲染，右键消息落在程序页窗口上，
//   不转发的话功能行右键完全无响应（这是实际修过的缺陷）。
//   只在当前选中 Tab1 时转发：Tab0（程序列表）有自己的一套行。
[[nodiscard]] bool CPageProgram::PrgForwardToFeatureMenu(POINT pt, bool bFromContextMsg)
{
    CTabLayoutUI* pPages = static_cast<CTabLayoutUI*>(m_ctx.HostFindControl(L"prog_pages"));
    if (!pPages || pPages->GetCurSel() != 1) return false;
    CPageBase* pFeat = m_ctx.HostPageHost()->PageById(EPageId::PAGE_FEATURE);
    if (!pFeat) return false;
    return pFeat->HostFeatureContextMenu(pt, bFromContextMsg);
}

bool CPageProgram::OnContextMenu(POINT pt, bool bFromContextMsg)
{
    if (bFromContextMsg) return false;
    CControlUI* pHit = m_ctx.HostFindControlAt(pt);
    // [R175] 程序行命中抽为 PrgRowFromHit，嵌套 4→2
    const int nRow = PrgRowFromHit(pHit);
    if (nRow >= 0) { ShowContextMenu(nRow); return true; }

    // [修复] Tab1 Windows 功能行的右键菜单在 Feature 子视图，
    // 需经 PAGE_FEATURE 代理转发，否则功能行右键无响应。
    // [R175] 代理转发抽为 PrgForwardToFeatureMenu
    return PrgForwardToFeatureMenu(pt, bFromContextMsg);
}
// P8：原 CFrameWnd WM_KEYDOWN VK_DELETE nPage==6 分支
[[nodiscard]] bool CPageProgram::OnDeleteKey()
{
    // Tab1 Windows 功能：Delete 键代理到 Feature 子视图（永久删除选中项）
    CTabLayoutUI* pPages = static_cast<CTabLayoutUI*>(
        m_ctx.HostFindControl(L"prog_pages"));
    if (pPages && pPages->GetCurSel() == 1)
    {
        if (CPageBase* pFeat = m_ctx.HostPageHost()->PageById(EPageId::PAGE_FEATURE))
            pFeat->HostFeatureDeleteSelected();
        return true;
    }
    if (Selected() < 0) return false;
    CControlUI* pBtn = m_ctx.HostFindControl(L"btn_prog_delete");
    if (pBtn && pBtn->IsVisible()) Uninstall(Selected());
    return true;
}
// ============================================================
// [R70] 批量卸载：遍历 prowck_%d 勾选行，逐个调用 CreateProcess
// 启动各程序的 UninstallString。勾选控件：prowck_%d（每行首个
// 子控件，与 prow_%d 行一一对应）。
// ============================================================
// [R170] 从 BatchUninstallPrograms 抽出：收集勾选行的索引。
//   列名格式 prowck_%d，且必须同时满足"勾选"+"有 UninstallString"——
//   后者很重要：部分 MSI 条目没有卸载命令（只有修复命令），
//   把它们算进"勾选数"会让用户以为选了 10 个、实际只卸 7 个。
[[nodiscard]] std::vector<int> CPageProgram::PrgCollectCheckedRows(CVerticalLayoutUI* pList)
{
    std::vector<int> vSel;
    if (!pList) return vSel;
    for (int k = 0; k < pList->GetCount(); ++k)
    {
        CDuiString sCkName;
        sCkName.Format(L"prowck_%d", k);
        CCheckBoxUI* pCk = static_cast<CCheckBoxUI*>(m_ctx.HostFindControl(sCkName.GetData()));
        if (!pCk || !pCk->GetCheck()) continue;
        if (k < 0 || k >= static_cast<int>(m_vPrograms.size())) continue;
        if (m_vPrograms[k].sUninstall.IsEmpty()) continue;
        vSel.push_back(k);
    }
    return vSel;
}

// [INTERACT-FIX] 批量卸载整体移入工作线程。
// 原实现（含 PrgRunUninstallOne 成员版）在 UI 线程逐条
// WaitForSingleObject(卸载向导进程, 120s)：勾选 N 条最长冻结 N×120 秒，
// 期间窗口无响应、无法取消，是本页最大卡顿/假死崩溃诱因。
// 现在：UI 只做收集勾选 + 确认，执行体在后台逐条 CreateProcess+等待，
// 全部结束后经 WM_PROG_BATCHDONE 一次性回传（ProgBatchMsg*，UI 线程 delete）。
// 成员版 PrgRunUninstallOne 随之降级为线程内静态函数：不再触碰 m_ctx
// （工作线程禁止调用任何 UI 接口），失败条目名收集到 vFailNames 由 UI 侧落日志。
namespace
{
// [R170] 对单个程序执行卸载并计数（工作线程上下文，零 UI 依赖）。
//   把 UninstallString 拷进可写缓冲后交给 CreateProcessW ——
//   lpCommandLine 形参是 LPWSTR（可写），不能直接传 const 指针。
//   reserve(2048) 预留给常见卸载命令行，避免取 &sCmdBuf[0] 后因重新分配导致指针失效。
//   同步等 120s 再关句柄：卸载程序是交互式向导，不等待会出现
//   "批量卸载完成"的 Toast 提前弹出、而实际仍有子进程在跑。
//   hThread/hProcess 必须在成功分支内成对关闭；失败分支无有效句柄。
//   单条失败只 ++nFail，不影响其余条目。
void PrgRunUninstallOneWorker(const ProgEntry& e, int& nOk, int& nFail,
                              std::vector<std::pair<CDuiString, unsigned long>>& vFailures)
{
    if (e.sUninstall.IsEmpty()) { ++nFail; vFailures.emplace_back(e.sName, 0ul); return; }

    std::wstring sCmdBuf = e.sUninstall.GetData() ? e.sUninstall.GetData() : L"";
    sCmdBuf.reserve(2048);
    STARTUPINFO si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    const BOOL ok = ::CreateProcessW(nullptr, &sCmdBuf[0], nullptr, nullptr, FALSE,
                                     0, nullptr, nullptr, &si, &pi);
    if (!ok)
    {
        ++nFail;
        vFailures.emplace_back(e.sName, static_cast<unsigned long>(::GetLastError()));
        return;
    }

    ::WaitForSingleObject(pi.hProcess, 120000);
    ::CloseHandle(pi.hThread);   // 本函数不等待该线程，仅确认进程已创建
    ::CloseHandle(pi.hProcess);
    ++nOk;
}

// 工作线程上下文：卸载目标快照（拷贝，避免与 UI 侧 m_vPrograms 并发）
struct ProgBatchCtx
{
    HWND hWnd = nullptr;
    std::vector<ProgEntry> vTargets;
};

// [R-TAILFIX 同型] 批量卸载循环 SEH 边界：本函数无 C++ 对象，允许 __try。
// 任一硬件异常若穿透线程过程，WM_PROG_BATCHDONE 永不投递，程序页忙碌态
// 永久卡死；__except 兜住后剩余条目保守计失败，DONE 照常回投。
static void PrgBatchSehLoop(ProgBatchCtx* g, ProgBatchMsg* m)
{
    __try
    {
        for (const ProgEntry& e : g->vTargets)
            PrgRunUninstallOneWorker(e, m->nOk, m->nFail, m->vFailures);
    }
    __except (CrashGuardFilter(GetExceptionInformation(), "prog-batch-seh"))
    {
        const int nTried = m->nOk + m->nFail;
        if (nTried < m->nTotal) m->nFail += m->nTotal - nTried;
    }
}

DWORD WINAPI ProgBatchUninstallThreadProc(void* pv)
{
    std::unique_ptr<ProgBatchCtx> guard(static_cast<ProgBatchCtx*>(pv));
    if (!guard) return 0;
    auto msg = std::make_unique<ProgBatchMsg>();
    msg->nTotal = static_cast<int>(guard->vTargets.size());
    PrgBatchSehLoop(guard.get(), msg.get());
    // 所有权按"投递是否成功"决定（同 ProgEnumThreadProc [R17-FIX-P0] 契约）
    // [FIX-S1-8] Post 失败有限重试：DONE 丢失会让 m_bBatchUninstalling 永久
    // 残留（唯一复位点在 UI 收口），批量卸载功能锁死。队列瞬时满时退避重试；
    // 窗口已亡时进程将退，无锁死意义。
    bool bPosted = false;
    if (::IsWindow(guard->hWnd))
    {
        for (int nRetry = 0; nRetry < 10; ++nRetry)
        {
            if (::PostMessageW(guard->hWnd, WM_PROG_BATCHDONE, 0,
                               msgparam::ToParam(msg.get())))
            {
                bPosted = true;
                break;
            }
            if (!::IsWindow(guard->hWnd)) break;
            ::Sleep(100);
        }
    }
    if (bPosted) (void)msg.release();
    return 0;
}
} // namespace

void CPageProgram::BatchUninstallPrograms()
{
    // [P0] WinPE 离线会话：批量卸载同样不可用于离线映像
    if (COfflineSession::IsActive())
    {
        m_ctx.HostNotify(L"WinPE offline mode",
                         L"Programs cannot be uninstalled from an offline image. "
                            "Boot the target system and uninstall there.", 2);
        return;
    }
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(
        m_ctx.HostFindControl(L"prog_apps_list"));
    if (!pList) return;
    // [R170] 收集勾选抽为 PrgCollectCheckedRows（97→约 68 行）
    const std::vector<int> vSel = PrgCollectCheckedRows(pList);
    if (vSel.empty())
    {
        m_ctx.HostNotify(StringMgr.Get("general.confirm"),
                         StringMgr.Get("misc.s64513cc7"), 0);
        return;
    }
    CDuiString sMsg;
    sMsg.Format(StringMgr.Get("misc.se24b6a39"), static_cast<int>(vSel.size()));
    if (!m_ctx.HostConfirm(StringMgr.Get("general.confirm"), sMsg.GetData()))
        return;
    // [INTERACT-FIX] 执行体移交工作线程（见上方 ProgBatchUninstallThreadProc）。
    // 在途闸门：后台批次未回传前拒绝二次发起，杜绝两条卸载线程并跑
    // 同一程序集（重复卸载向导/句柄互抢）。回传消息接收侧清闸门。
    if (m_bBatchUninstalling)
    {
        m_ctx.HostToast(StringMgr.Get("misc.s6da661b2"),
                        StringMgr.Get("program.s_batch_running"), 1);
        return;
    }
    auto ctx = std::make_unique<ProgBatchCtx>();
    ctx->hWnd = m_ctx.HostHwnd();
    ctx->vTargets.reserve(vSel.size());
    for (size_t i = 0; i < vSel.size(); ++i)
        ctx->vTargets.push_back(m_vPrograms[vSel[i]]);
    HANDLE h = SafeCreateThread(&ProgBatchUninstallThreadProc, ctx.get());
    if (!h)
    {
        m_ctx.HostLog(StringMgr.Get("misc.sf4f17c78"),
                      static_cast<int>(ELogLevel::LOG_ERROR));
        return;
    }
    m_bBatchUninstalling = true;
    ctx.release();   // 所有权移交线程（线程内 unique_ptr 接管）
    ::CloseHandle(h);
    m_ctx.HostLog(StringMgr.Get("program.s_batch_started"));
}



// ============================================================
// [R-B1] Ctrl+C / Ctrl+X 复制当前选中程序名
// ============================================================
void CPageProgram::OnGlobalCopy([[maybe_unused]] bool bCut)
{
    if (m_nSel < 0 || static_cast<size_t>(m_nSel) >= m_vPrograms.size())
    {
        m_ctx.HostToast(StringMgr.Get("program.s_nocopy"),
                        StringMgr.Get("program.s_nocopy_body"), 1);
        return;
    }
    const ProgEntry& e = m_vPrograms[m_nSel];
    CDuiString sCopy = e.sName;
    if (sCopy.IsEmpty()) sCopy = e.sUninstall;
    if (sCopy.IsEmpty()) return;
    FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), sCopy.GetData());
    m_ctx.HostToast(StringMgr.Get("program.s_copydone"),
                    sCopy.GetData(), 0);
    CDuiString sLog;
    sLog.Format(L"[program] copy name: %s", sCopy.GetData());
    m_ctx.HostLog(sLog.GetData());
}
// [REFACTOR] 从 OnEvent 提取：prog_pages Tab1（Windows 功能）物理寄宿在本页，
// 未被上面共享分支消费的 click/dbclick 在 Tab1 时经 PAGE_FEATURE 代理转发——
// 等价原壳 Notify 的功能页分支；缺失它会导致复选框不可勾选、箭头不可展开、
// 行不可选中。逐字搬迁零逻辑改动。
void CPageProgram::ForwardFeatureEventInTab1(TNotifyUI& msg)
{
    CTabLayoutUI* pPages = static_cast<CTabLayoutUI*>(
        m_ctx.HostFindControl(L"prog_pages"));
    if (!pPages || pPages->GetCurSel() != 1) return;
    if (CPageBase* pFeat = m_ctx.HostPageHost()->PageById(EPageId::PAGE_FEATURE))
        pFeat->HostFeatureEvent(msg);
}
