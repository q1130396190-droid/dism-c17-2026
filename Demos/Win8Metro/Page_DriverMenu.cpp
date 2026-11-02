#include "stdafx.h"
#include <string>   // [C++17] std::to_wstring：失败明细拼接
#include "OfflineSession.h"  // [R10-02] 离线会话经独立组件访问，去 CDataEngine 静态耦合

// ============================================================
// Page_DriverMenu.cpp —— 驱动页「行级右键菜单」段（R71 拆出）
// 承接 OnContextMenu / ShowRowContextMenu / CopyRowName / CopyRowInf / CopyRowDevInst。
// 依赖 ParsePrefix2（FrameWnd_UiHelpers.h 中的外部函数，非匿名 namespace）。
// ============================================================

#include "Page_Driver.h"


#include "CustomControls.h"
#include "StringManager.h"
#include "FrameWnd_Util.h"
#include "FrameWnd_UiHelpers.h" // P8：ParsePrefix2
#include "WmMessages.h"        // [R13 P0收敛] 仅需 WM_* 宏；UI 专属常量零使用
#include "DiagnosticsService.h" // Build 末尾驱动签名/回滚快照
#include "Registry.h"          // RegGetString
#include "ExternalToolService.h"  // [R72] StartToolAsync
#include "ProcessRunner.h"  // [R83] P1-15 pnputil /scan-devices 同步执行
#include <thread>               // [卡顿修复] 第三方驱动库后台枚举
#include <memory>               // [C++17 R3] make_unique/unique_ptr
#include "SafeThread.h"          // [BUGFIX rb27] 驱动枚举线程顶层 SEH 边界
#include "DismApiBridge.h"        // [P1-A6] 离线驱动删除/添加走 dismapi 会话
#include "DriverService.h"        // [P1-A6] 原生导出/重扫
#include "WIMHelperBridge.h"      // [P1-A6] 离线 WIM 驱动扫描（替代 dism /Get-Drivers）
#include "DataEngine.h"           // [P0] WinPE 离线会话 /Image 定向

#include <setupapi.h>
#include <cfgmgr32.h>        // [R114-1] CM_Get_DevNode_Status 读设备状态/问题代码
#include <shlobj.h>            // DoDriverExport：SHBrowseForFolder（保留原直调）
#include <commdlg.h>           // DoDriverAdd：GetOpenFileName（保留原直调）
bool CPageDriver::OnContextMenu(POINT pt, bool bFromContextMsg)
{
    if (bFromContextMsg) return false;

    CControlUI* pHit = m_ctx.HostFindControlAt(pt);
    if (!pHit) return false;

    for (CControlUI* p = pHit; p; p = p->GetParent())
    {
        const wchar_t* psz = p->GetName();
        if (!psz) continue;
        if (wcsncmp(psz, L"drow_", 5) != 0) continue;

        int nCat = -1, nRow = -1;
        if (!ParsePrefix2(psz, L"drow_", nCat, nRow)) continue;
        if (m_mapDrvRow.find(CDuiString(psz)) == m_mapDrvRow.end()) continue;

        ShowRowContextMenu(nCat, nRow, pt);
        return true;
    }
    return false;
}

void CPageDriver::ShowRowContextMenu(int nCat, int nRow, POINT pt)
{
    if (nCat < 0 || nRow < 0) return;

    CDuiString sKey;
    sKey.Format(L"drow_%d_%d", nCat, nRow);
    std::map<CDuiString, DriverRowInfo>::iterator it = m_mapDrvRow.find(sKey);
    if (it == m_mapDrvRow.end()) return;

    HMENU hMenu = ::CreatePopupMenu();
    // 4701 复制设备名 / 4702 复制 INF / 4703 复制设备实例 ID
    ::AppendMenu(hMenu, MF_STRING, 4701, StringMgr.Get("driver.s_ctx_copy_name"));
    ::AppendMenu(hMenu, MF_STRING, 4702, StringMgr.Get("driver.s_ctx_copy_inf"));
    ::AppendMenu(hMenu, MF_STRING, 4703, StringMgr.Get("driver.s_ctx_copy_devinst"));
    ::AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    // 4704 查看详情 / 4705 回退驱动 / 4706 设备属性
    ::AppendMenu(hMenu, MF_STRING, 4704, StringMgr.Get("driver.s_ctx_detail"));
    ::AppendMenu(hMenu, MF_STRING, 4705, StringMgr.Get("driver.s_ctx_rollback"));
    ::AppendMenu(hMenu, MF_STRING, 4706, StringMgr.Get("driver.s_ctx_props"));

    int iCmd = ::TrackPopupMenu(hMenu,
        TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
        pt.x, pt.y, 0, m_ctx.HostHwnd(), nullptr);
    ::DestroyMenu(hMenu);

    if (iCmd == 4701)        CopyRowName(nCat, nRow);
    else if (iCmd == 4702)   CopyRowInf(nCat, nRow);
    else if (iCmd == 4703)   CopyRowDevInst(nCat, nRow);
    else if (iCmd == 4704)   ShowDetail(nCat, nRow);
    else if (iCmd == 4705) { RememberSelection(nCat, nRow); Rollback(); }
    else if (iCmd == 4706) { RememberSelection(nCat, nRow); ShowDeviceProperties(); }
}

void CPageDriver::CopyRowName(int nCat, int nRow)
{
    CDuiString sKey; sKey.Format(L"drow_%d_%d", nCat, nRow);
    std::map<CDuiString, DriverRowInfo>::iterator it = m_mapDrvRow.find(sKey);
    if (it == m_mapDrvRow.end() || it->second.sName.IsEmpty()) return;

    FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), it->second.sName.GetData());
    m_ctx.HostToast(StringMgr.Get("driver.s_ctx_copy_name_done"),
                    StringMgr.Get("driver.s_ctx_copy_name_done_body"));
    CDuiString sLog; sLog.Format(L"[driver] copy row name: %s",
                                 it->second.sName.GetData());
    m_ctx.HostLog(sLog.GetData());
}

void CPageDriver::CopyRowInf(int nCat, int nRow)
{
    CDuiString sKey; sKey.Format(L"drow_%d_%d", nCat, nRow);
    std::map<CDuiString, DriverRowInfo>::iterator it = m_mapDrvRow.find(sKey);
    if (it == m_mapDrvRow.end() || it->second.sInfPath.IsEmpty()) return;

    FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), it->second.sInfPath.GetData());
    m_ctx.HostToast(StringMgr.Get("driver.s_ctx_copy_inf_done"),
                    StringMgr.Get("driver.s_ctx_copy_inf_done_body"));
    CDuiString sLog; sLog.Format(L"[driver] copy row inf: %s",
                                 it->second.sInfPath.GetData());
    m_ctx.HostLog(sLog.GetData());
}

void CPageDriver::CopyRowDevInst(int nCat, int nRow)
{
    CDuiString sKey; sKey.Format(L"drow_%d_%d", nCat, nRow);
    std::map<CDuiString, DriverRowInfo>::iterator it = m_mapDrvRow.find(sKey);
    if (it == m_mapDrvRow.end() || it->second.sDevInstId.IsEmpty()) return;

    FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), it->second.sDevInstId.GetData());
    m_ctx.HostToast(StringMgr.Get("driver.s_ctx_copy_devinst_done"),
                    StringMgr.Get("driver.s_ctx_copy_devinst_done_body"));
    CDuiString sLog; sLog.Format(L"[driver] copy row devinst: %s",
                                 it->second.sDevInstId.GetData());
    m_ctx.HostLog(sLog.GetData());
}
