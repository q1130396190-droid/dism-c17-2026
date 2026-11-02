#include "stdafx.h"

// ============================================================
// Page_UpdateLocalPkg.cpp —— 更新页「本地补丁安装」与「CBS 枚举」子系统。
//
// [R135-1] 从 Page_Update.cpp（2633 行）拆出 227 行：
//   本地补丁(.msu/.cab)安装：拖拽入口 + 安装编排 + 两个工作线程
//   CBS 系统组件包枚举：枚举线程 + 结果合并
//
// 闭包说明：段内 CpuPkgJob/CpuCbsJob/CpuPost/CpuPkgThread/CpuCbsThread/
// CpuEndsWith 均在本文件内定义；4 个成员函数声明见 Page_Update.h，
// 跨 TU 调用 UpdMergeCbs / UpdRenderBatchStep 亦为头内声明的成员。
//
// 搬迁为逐字搬运，零逻辑改动。
// ============================================================

#include "OfflineSession.h"     // COfflineSession::IsActive / GetImageRoot
#include "Page_Update.h"

#include "CustomControls.h"      // CVerticalLayoutUI
#include "StringManager.h"
#include "FrameWnd_Messages.h"   // WM_UPD_DATA / JOBWM_*
#include "AppxCapUpdService.h"   // UpdEntry / UpdDataMsg
#include "DataEngine.h"          // 离线会话
#include "SafeThread.h"          // 工作线程异常边界
#include "ExternalToolService.h"

#include <string>
#include <vector>

using i18n::CStringManager;

// 本地补丁/CBS 枚举的消息类型（与 CpuPost 的 nKind 对应）
inline constexpr int kCpuKindPkgDone = 1;
inline constexpr int kCpuKindCbsDone = 2;

// [R-B76] 本地补丁（.msu/.cab）安装：拖拽 + 右键菜单共用
// ------------------------------------------------------------
namespace {

struct CpuPkgJob
{
    HWND hNotify;
    std::vector<std::wstring> vPkgs;
    std::wstring sRoot;       // 空 = 在线
};

void CpuPost(HWND h, int nKind, int nCode, const CDuiString& sMsg)
{
    // [C++17 阶段3] Post 消息所有权移交 UI，Post 失败自删
    auto p = std::make_unique<UpdDataMsg>(); // [C++17 R3]
    p->nKind = nKind;
    p->nResultCode = nCode;
    p->sMsg = sMsg;
    // [加固] 窗口失效/投递失败时回收，避免 UpdDataMsg 泄漏
    if (!::IsWindow(h) ||
        !::PostMessageW(h, WM_UPD_DATA, 0, msgparam::ToParam(p.get())))
        return;   // [C++17 R3] unique_ptr auto-frees on scope exit
    p.release();   // [C++17 R3] ownership moved to UI thread
}

unsigned __stdcall CpuPkgThread(void* pv)
{
    std::unique_ptr<CpuPkgJob> spJob(static_cast<CpuPkgJob*>(pv));  // [C++17 R3] 线程 ctx：函数退出自动释放
    CpuPkgJob* pJob = spJob.get();
    int nOk = 0, nFail = 0;
    for (size_t i = 0; i < pJob->vPkgs.size(); ++i)
    {
        const std::wstring& sPkg = pJob->vPkgs[i];
        // [R162] 4 段拼接改为 2 个语言键：原实现把"（在线）/（离线 x）"
        //   拆成 3 个字面量再拼，翻译时无法整体处理（语序不同的语言会拼错）。
        CDuiString sMode;
        if (pJob->sRoot.empty())
            sMode = StringMgr.Get("update.pkg_mode_online");
        else
            sMode.Format(StringMgr.Get("update.pkg_mode_offline"), pJob->sRoot.c_str());

        CDuiString sStart;
        sStart.Format(StringMgr.Get("update.pkg_begin"), sPkg.c_str(), sMode.GetData());
        CpuPost(pJob->hNotify, 8, 0, sStart);

        std::wstring sErr;
        bool bOk = UpdateMetaService::InstallLocalPackage(
                       sPkg.c_str(),
                       pJob->sRoot.empty() ? nullptr : pJob->sRoot.c_str(),
                       &sErr);
        if (bOk)
        {
            ++nOk;
            CDuiString sOk;
            sOk.Format(StringMgr.Get("update.pkg_ok"), sPkg.c_str());
            CpuPost(pJob->hNotify, 8, 0, sOk);
        }
        else
        {
            ++nFail;
            CDuiString sFail;
            sFail.Format(StringMgr.Get("update.pkg_fail"),
                         sPkg.c_str(),
                         sErr.empty() ? StringMgr.Get("update.pkg_unknown_err") : sErr.c_str());
            CpuPost(pJob->hNotify, 8, 1, sFail);
        }
    }

    CDuiString sDone;
    if (nFail == 0)
        sDone.Format(StringMgr.Get("update.pkg_done_all"), nOk);
    else
        sDone.Format(StringMgr.Get("update.pkg_done_part"), nOk, nFail);
    CpuPost(pJob->hNotify, 9, nFail == 0 ? 0 : 1, sDone);
    // [C++17 R3] 线程 ctx：spJob unique_ptr 自动释放（所有权由启动方移交）
    return 0;
}

[[nodiscard]] bool CpuEndsWith(const std::wstring& s, const wchar_t* pszSuf)
{
    size_t n = wcslen(pszSuf);
    return s.size() >= n && lstrcmpiW(s.c_str() + s.size() - n, pszSuf) == 0;
}

struct CpuCbsJob
{
    HWND hNotify;
    std::wstring sRoot;
    std::wstring sEndpoint;
    std::wstring sOutbox;
};

unsigned __stdcall CpuCbsThread(void* pv)
{
    std::unique_ptr<CpuCbsJob> spJob(static_cast<CpuCbsJob*>(pv));  // [C++17 R3] 线程 ctx：函数退出自动释放
    CpuCbsJob* pJob = spJob.get();

    // [R-B78] 顺带补发排队报告（后台执行，不阻塞 UI）
    if (!pJob->sOutbox.empty())
        UpdateMetaService::FlushReportOutbox(
            pJob->sEndpoint.empty() ? nullptr : pJob->sEndpoint.c_str(),
            pJob->sOutbox.c_str());

    std::vector<UpdateMetaService::CbsPkgRecord> vRec;
    UpdateMetaService::EnumerateInstalledCbsPackages(
        pJob->sRoot.empty() ? nullptr : pJob->sRoot.c_str(), vRec);

    // [C++17 阶段3] Post 消息所有权移交 UI，Post 失败自删
    auto pMsg = std::make_unique<UpdDataMsg>(); // [C++17 R3]
    pMsg->nKind = 7;
    pMsg->nTab = 1;
    for (size_t i = 0; i < vRec.size(); ++i)
    {
        UpdEntry e;
        e.sTitle = CDuiString(L"[CBS] ") + vRec[i].szIdentity;
        e.sDate = vRec[i].szRelType;
        e.sUpdateID = L"";   // CBS 行不走 WUA 卸载（cbsprow_ 前缀隔离）
        pMsg->vUpd.push_back(e);
    }
    // [加固] 窗口失效/投递失败时回收消息对象
    if (!::IsWindow(pJob->hNotify) ||
        !::PostMessageW(pJob->hNotify, WM_UPD_DATA, 0, msgparam::ToParam(pMsg.get())))
        return 0;   // [C++17 R3] unique_ptr auto-frees on scope exit
    pMsg.release();   // [C++17 R3] ownership moved to UI thread
    // [C++17 R3] 线程 ctx：spJob unique_ptr 自动释放（所有权由启动方移交）
    return 0;
}

} // namespace

bool CPageUpdate::OnDropFiles(const std::vector<std::wstring>& vPaths)
{
    std::vector<std::wstring> vPkgs;
    for (size_t i = 0; i < vPaths.size(); ++i)
    {
        if (CpuEndsWith(vPaths[i], L".msu") || CpuEndsWith(vPaths[i], L".cab"))
            vPkgs.push_back(vPaths[i]);
    }
    if (vPkgs.empty()) return false;
    InstallLocalPackages(vPkgs);
    return true;
}

void CPageUpdate::InstallLocalPackages(const std::vector<std::wstring>& vPkgs)
{
    if (vPkgs.empty()) return;

    // [R164] 4 段拼接收成 3 个语言键：原实现把"离线映像 x / 当前在线系统"
    //   拆成 3 个字面量再拼，语序不同的语言（如日语宾语前置）会拼出错句。
    CDuiString sTarget;
    if (COfflineSession::IsActive())
        sTarget.Format(StringMgr.Get("update.ask_install_target_offline"),
                       COfflineSession::GetImageRoot());
    else
        sTarget = StringMgr.Get("update.ask_install_target_online");

    CDuiString sAsk;
    sAsk.Format(StringMgr.Get("update.ask_install_pkg"),
                static_cast<int>(vPkgs.size()), sTarget.GetData());
    if (!m_ctx.HostConfirmWarn(StringMgr.Get("update.ask_install_title"), sAsk.GetData()))
    {
        m_ctx.HostLog(L"[update] 用户取消本地补丁安装");
        return;
    }

    // [C++17 阶段3] 线程 ctx：所有权移交 CpuPkgThread，创建失败自删
    auto pJob = std::make_unique<CpuPkgJob>(); // [C++17 R3]
    pJob->hNotify = m_ctx.HostHwnd();
    pJob->vPkgs = vPkgs;
    if (COfflineSession::IsActive())
        pJob->sRoot = COfflineSession::GetImageRoot().GetData();

    uintptr_t h = SafeBeginThread(CpuPkgThread, pJob.get());
    if (h == 0)
    {
        m_ctx.HostLog(L"[update] 本地补丁安装线程启动失败");
        return;   // [C++17 R3] unique_ptr auto-frees on scope exit
    }
    SafeCloseThreadHandle(h);   // [C++17 阶段2] SafeBeginThread 返回的线程句柄：跨函数分离线程，沿用 ProcessRunner 约定显式关闭
    pJob.release();   // [C++17 R3] ownership moved to CpuPkgThread
    m_ctx.HostLog(L"[update] 本地补丁安装任务已启动（后台线程，日志实时回传）");
}

void CPageUpdate::StartCbsEnumThread()
{
    if (m_bCbsLoading) return;
    m_bCbsLoading = true;

    // [C++17 阶段3] 线程 ctx：所有权移交 CpuCbsThread，创建失败自删
    auto pJob = std::make_unique<CpuCbsJob>(); // [C++17 R3]
    pJob->hNotify = m_ctx.HostHwnd();
    pJob->sRoot = COfflineSession::IsActive()
                ? COfflineSession::GetImageRoot().GetData() : std::wstring();
    pJob->sEndpoint = m_sReportEndpoint.GetData();
    {
        wchar_t szExe[MAX_PATH] = {};
        ::GetModuleFileNameW(nullptr, szExe, MAX_PATH);
        wchar_t* pSlash = wcsrchr(szExe, L'\\');
        if (pSlash) *pSlash = 0;
        pJob->sOutbox = std::wstring(szExe) + L"\\Report\\outbox";
    }

    uintptr_t h = SafeBeginThread(CpuCbsThread, pJob.get());
    if (h == 0)
    {
        m_bCbsLoading = false;
        m_ctx.HostLog(L"[update] CBS /Get-Packages 并入线程启动失败");
        return;   // [C++17 R3] unique_ptr auto-frees on scope exit
    }
    SafeCloseThreadHandle(h);   // [C++17 阶段2] SafeBeginThread 返回的线程句柄：跨函数分离线程，沿用 ProcessRunner 约定显式关闭
    pJob.release();   // [C++17 R3] ownership moved to CpuCbsThread
    m_ctx.HostLog(L"[update] CBS /Get-Packages 并入枚举已启动（后台）");
}

void CPageUpdate::OnCbsPackages(UpdDataMsg* pData)
{
    m_bCbsLoading = false;
    if (!pData) return;

    // [性能 A2] CBS 与 WUA 并行枚举，结果先暂存；WUA 渲染已完成则立即
    // 去重并入，未完成则由分批渲染收尾时统一并入（避免被 RemoveAll 冲掉）。
    if (pData->vUpd.empty())
    {
        m_ctx.HostLog(L"[update] CBS /Get-Packages 并入：无额外 Installed 包");
        return;
    }
    m_vCbsPending = pData->vUpd;
    MarkCbsReady();
    MarkCbsExpanded(false);

    // WUA tab1 分批渲染仍在途时延后并入（UpdRenderBatchStep 收尾会调用）
    bool bWuaBatchRunning = (m_nRenderTab == 1 && !m_bRenderCbs &&
                             m_pRenderList && m_nRenderPos < m_vUpdList[1].size());
    if (!bWuaBatchRunning)
        UpdMergeCbs();
}
