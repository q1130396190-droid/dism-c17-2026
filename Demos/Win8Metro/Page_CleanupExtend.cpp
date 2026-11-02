#include "stdafx.h"

// ============================================================
// Page_CleanupExtend.cpp —— 清理页「增强扫描」子系统。
//
// 当前内容（大文件 / 空文件夹两个入口已随按钮移除）：
//   · 卸载残留扫描  OnExtendOrphanScan
//   · 撤销上次清理  OnExtendUndoLast（撤销栈由主清理路径推入）
//   · 扫描期间按钮启停 SetExtendButtonsBusy
//
// 历史：[R136-1] 从 Page_Cleanup.cpp 拆出时还含大文件/空文件夹的后台扫描、
//   线程载荷 ExtScanCtx 与完成回调 OnExtendScanDone。这些只服务于已删除的
//   两个按钮，随按钮一并删除，而不是留成不会被调用的死代码。
// ============================================================

#include "Page_Cleanup.h"

#include "CleanExtend.h"          // CCleanExtendEngine
#include "CustomControls.h"
#include "StringManager.h"
#include "FrameWnd_Messages.h"
#include "SafeThread.h"
#include "MsgParam.h"

#include <shellapi.h>             // SHFileOperation
#include <shlobj.h>
#include <string>
#include <vector>

using i18n::CStringManager;

// ============================================================
// [R80] Cleanup 增强：卸载残留扫描 / 撤销清理
// 引擎：m_ctx.CleanExtendEngine()（CCleanExtendEngine）
// 全部删除走 SHFileOperation FOF_ALLOWUNDO（进回收站，可撤销）
// ============================================================

void CPageCleanup::SetExtendButtonsBusy(bool bBusy)
{
    // 全盘扫描期间禁用增强扫描入口，防止重入并发写同一份结果向量；
    // 撤销按钮不涉及扫描，保持可用（删的是旧快照，不碰当前向量）。
    // 大文件/空文件夹两个入口已随按钮一并移除，现在只剩残留扫描。
    static const wchar_t* s_ids[] = {
        L"btn_cln_orphan"
    };
    for (size_t i = 0; i < _countof(s_ids); ++i)
    {
        CControlUI* pBtn = m_ctx.HostFindControl(s_ids[i]);
        if (pBtn) pBtn->SetEnabled(!bBusy);
    }
}

void CPageCleanup::OnExtendOrphanScan()
{
    std::vector<CleanExtendOrphan> v;
    int n = m_ctx.CleanExtendEngine().OrphanScanUninstall(v);

    CDuiString sLog;
    sLog.Format(L"[cleanExt] orphan scan: %d uninstall residues", n);
    AppendLog(sLog.GetData());
    for (size_t i = 0; i < v.size() && i < 5; ++i)
    {
        CDuiString sLine;
        sLine.Format(L"  - %s @ %s", v[i].sDisplayName.GetData(),
                     v[i].sInstallPath.GetData());
        AppendLog(sLine.GetData());
    }
    m_ctx.HostToast(StringMgr.Get("cleanup.s_extend_orphan"),
                    n == 0 ? StringMgr.Get("cleanup.s_extend_orphan_none") :
                    sLog.GetData(), 0);
}

// [R196] 撤销上次清理：恢复最近一批删除（主清理路径与残留扫描都会推入撤销栈）。
void CPageCleanup::OnExtendUndoLast()
{
    if (m_ctx.CleanExtendEngine().UndoDepth() == 0)
    {
        m_ctx.HostToast(StringMgr.Get("cleanup.s_extend_undo"),
                        StringMgr.Get("cleanup.s_extend_undo_empty"), 1);
        return;
    }

    std::vector<CDuiString> vErr;
    int nOk = m_ctx.CleanExtendEngine().UndoLast(vErr);

    CDuiString sLog;
    sLog.Format(L"[cleanExt] undo last: %d restored, %d failed", nOk,
                static_cast<int>(vErr.size()));
    AppendLog(sLog.GetData());
    m_ctx.HostToast(StringMgr.Get("cleanup.s_extend_undo"), sLog.GetData(),
                    vErr.empty() ? 0 : 1);
}
