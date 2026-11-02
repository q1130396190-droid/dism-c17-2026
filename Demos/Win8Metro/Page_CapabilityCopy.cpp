// ============================================================
// [R77] Capability 页行级复制动作独立编译单元
//
// 从 Page_Capability.cpp 切出的 3 个成员函数（函数体零逻辑改动）：
//   CopyIdentity      复制 Identity
//   CopyDisplayName   复制显示名（PrettyCapName）
//   CopyDescription   复制描述（空则回退 Identity）
// 调用方：ShowRowContextMenu 的 4601/4602/4603 命令分发（同文件内）。
// ============================================================

#include "stdafx.h"

#include "Page_Capability.h"
#include "AppxCapUpdService.h"   // CapEntry
#include "StringManager.h"       // StringMgr 本地化宏
#include "FrameWnd_Util.h"       // PrettyCapName / CopyTextToClipboard

using namespace DuiLib;

void CPageCapability::CopyIdentity(int k)
{
    if (k < 0 || k >= static_cast<int>(m_vCaps.size())) return;
    const CapEntry& e = m_vCaps[k];
    if (e.sIdentity.IsEmpty()) return;

    FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), e.sIdentity.GetData());
    m_ctx.HostToast(StringMgr.Get("cap.s_ctx_copyid_done"),
                    StringMgr.Get("cap.s_ctx_copyid_done_body"), 0);
    CDuiString sLog;
    sLog.Format(L"[capability] copy identity: %s", e.sIdentity.GetData());
    m_ctx.HostLog(sLog.GetData());
}

void CPageCapability::CopyDisplayName(int k)
{
    if (k < 0 || k >= static_cast<int>(m_vCaps.size())) return;
    const CapEntry& e = m_vCaps[k];

    CDuiString sName = PrettyCapName(e.sIdentity);
    if (sName.IsEmpty()) return;

    FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), sName.GetData());
    m_ctx.HostToast(StringMgr.Get("cap.s_ctx_copyname_done"),
                    StringMgr.Get("cap.s_ctx_copyname_done_body"), 0);
    CDuiString sLog;
    sLog.Format(L"[capability] copy display name: %s", sName.GetData());
    m_ctx.HostLog(sLog.GetData());
}

void CPageCapability::CopyDescription(int k)
{
    if (k < 0 || k >= static_cast<int>(m_vCaps.size())) return;
    const CapEntry& e = m_vCaps[k];

    CDuiString sDesc = e.sDescription;
    if (sDesc.IsEmpty()) sDesc = e.sIdentity;
    if (sDesc.IsEmpty()) return;

    FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), sDesc.GetData());
    m_ctx.HostToast(StringMgr.Get("cap.s_ctx_copydesc_done"),
                    StringMgr.Get("cap.s_ctx_copydesc_done_body"), 0);
    CDuiString sLog;
    sLog.Format(L"[capability] copy description: %s", sDesc.GetData());
    m_ctx.HostLog(sLog.GetData());
}
