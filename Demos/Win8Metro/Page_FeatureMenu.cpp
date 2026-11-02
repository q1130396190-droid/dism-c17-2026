#include "stdafx.h"

// ============================================================
// Page_FeatureMenu.cpp —— [R91] Windows 功能页"行级右键菜单与详情组"
//
// 从 Page_Feature.cpp 切出的 5 个成员函数（函数体零逻辑改动）：
//   ShowFeatContextMenu  右键菜单构建与命令分发（4701~4704）
//   CopyFeatSysName      复制 DISM 系统名
//   CopyFeatDispName     复制中文显示名
//   CopyFeatState        复制状态文本
//   ShowFeatDetail       弹出详情（Toast 形式）
//
// 为什么这组可以独立：
//   成员函数跨编译单元调用没有链接性问题（类声明在头文件里），
//   切片外的调用点只有 OnContextMenu（1909 行）与 OnGlobalCopy（2054 行），
//   两者都经 CPageFeature 声明访问，不依赖本文件内部任何 static/匿名 ns 符号。
//
// 依赖的 FeatStateText 仍留在主文件（它被 ShowDetailInPanel 等多处调用），
// 经头文件声明跨 TU 调用。
//
// 拆分动因：Page_Feature.cpp 已达 2055 行，超过 R76/R77 建立的 1500 行门槛。
// ============================================================

#include "Page_Feature.h"
#include "StringManager.h"
#include "FrameWnd_Util.h"       // FrameWnd_Util_CopyTextToClipboard

using namespace DuiLib;
using i18n::CStringManager;

void CPageFeature::ShowFeatContextMenu(int n, POINT pt)
{
    if (n < 0 || n >= static_cast<int>(m_vFeatures.size())) return;

    HMENU hMenu = ::CreatePopupMenu();
    ::AppendMenu(hMenu, MF_STRING, 4701, StringMgr.Get("feat.s_ctx_copysys"));
    ::AppendMenu(hMenu, MF_STRING, 4702, StringMgr.Get("feat.s_ctx_copydisp"));
    ::AppendMenu(hMenu, MF_STRING, 4703, StringMgr.Get("feat.s_ctx_copying"));
    ::AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenu(hMenu, MF_STRING, 4704, StringMgr.Get("feat.s_ctx_detail"));

    int iCmd = ::TrackPopupMenu(hMenu,
        TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
        pt.x, pt.y, 0, m_ctx.HostHwnd(), nullptr);
    ::DestroyMenu(hMenu);

    if (iCmd == 4701)      CopyFeatSysName(n);
    else if (iCmd == 4702) CopyFeatDispName(n);
    else if (iCmd == 4703) CopyFeatState(n);
    else if (iCmd == 4704) ShowFeatDetail(n);
}

void CPageFeature::CopyFeatSysName(int n)
{
    if (n < 0 || n >= static_cast<int>(m_vFeatures.size())) return;
    const FeatInfo& f = m_vFeatures[n];
    if (f.sSys.IsEmpty()) return;

    FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), f.sSys.GetData());
    m_ctx.HostToast(StringMgr.Get("feat.s_ctx_copysys_done"),
                    StringMgr.Get("feat.s_ctx_copysys_done_body"), 0);
    CDuiString sLog;
    sLog.Format(L"[feature] copy sys name: %s", f.sSys.GetData());
    m_ctx.HostLog(sLog.GetData());
}

void CPageFeature::CopyFeatDispName(int n)
{
    if (n < 0 || n >= static_cast<int>(m_vFeatures.size())) return;
    const FeatInfo& f = m_vFeatures[n];

    CDuiString sDisp = f.sDisp;
    if (sDisp.IsEmpty()) sDisp = f.sSys;
    if (sDisp.IsEmpty()) return;

    FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), sDisp.GetData());
    m_ctx.HostToast(StringMgr.Get("feat.s_ctx_copydisp_done"),
                    StringMgr.Get("feat.s_ctx_copydisp_done_body"), 0);
    CDuiString sLog;
    sLog.Format(L"[feature] copy display name: %s", sDisp.GetData());
    m_ctx.HostLog(sLog.GetData());
}

void CPageFeature::CopyFeatState(int n)
{
    if (n < 0 || n >= static_cast<int>(m_vFeatures.size())) return;
    const FeatInfo& f = m_vFeatures[n];

    CDuiString sState = FeatStateText(n);
    if (sState.IsEmpty()) return;

    FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), sState.GetData());
    m_ctx.HostToast(StringMgr.Get("feat.s_ctx_copying_done"),
                    StringMgr.Get("feat.s_ctx_copying_done_body"), 0);
    CDuiString sLog;
    sLog.Format(L"[feature] copy state[%s]: %s", f.sSys.GetData(), sState.GetData());
    m_ctx.HostLog(sLog.GetData());
}

void CPageFeature::ShowFeatDetail(int n)
{
    if (n < 0 || n >= static_cast<int>(m_vFeatures.size())) return;
    const FeatInfo& f = m_vFeatures[n];

    CDuiString sState = FeatStateText(n);
    bool bVirtual = f.sSys.IsEmpty();

    // [rb26] 详情文本：中文名 → 中文说明 → 内部名 → 状态（虚拟分组不显示内部名）
    CDuiString sBody = f.sDisp.IsEmpty() ? f.sSys : f.sDisp;
    if (!f.sDesc.IsEmpty())
    {
        sBody += L"\r\n\r\n";
        sBody += f.sDesc;
    }
    sBody += L"\r\n\r\n";
    if (!bVirtual)
    {
        sBody += L"Feature: ";
        sBody += f.sSys;
        sBody += L"\r\n";
    }
    sBody += StringMgr.Get("feat.s_status_prefix");
    sBody += sState;
    m_ctx.HostToast(StringMgr.Get("feat.s_ctx_detail"), sBody.GetData(), 1);

    CDuiString sLog;
    sLog.Format(L"[feature] detail: %s | %s", f.sSys.GetData(), sState.GetData());
    m_ctx.HostLog(sLog.GetData());
}
