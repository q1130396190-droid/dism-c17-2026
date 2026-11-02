// ============================================================
// Page_UnattendApply.cpp —— 应答文件页「应用预应答配置」子系统。
//
// [R99-RECOVER] 从 _work/_bak_r98/Page_Unattend.cpp 的 819..849 行
//   逐字提取恢复（原拆分时该段随 Page_UnattendIbs.cpp 一并被写空）。
//   仅保留「计数 → 确认 → 建 XML → 规范化 → 写盘 → 拉起 sysprep」
//   的编排骨架，各步骤实现分别在 Xml / Ibs_Report 两个 TU 中。
// ============================================================

#include "stdafx.h"
#include <algorithm>

#include "Page_Unattend.h"
#include "Page_UnattendInternal.h"   // RequireUnattendAdmin / UcNormalizeXmlViaDom
#include "StringManager.h"
#include "SystemInfoService.h"       // MakeTempWorkDir

using namespace DuiLib;
using i18n::CStringManager;

void CPageUnattend::ApplyUnattend()
{
    if (!RequireUnattendAdmin(m_ctx, StringMgr.Get("misc.sd6e4b309"))) return;
    // 统计当前命令数
    int nTotal = 0;
    for (int n = 0; n < 4; ++n) nTotal += static_cast<int>(m_vUcCmds[n].size());
    if (nTotal == 0)
    {
        m_ctx.HostNotify(StringMgr.Get("misc.sd6e4b309"),
                         StringMgr.Get("misc.s97de183e"), 2);
        return;
    }
    // 强确认
    if (!m_ctx.HostConfirmYesNo(StringMgr.Get("misc.sd6e4b309"),
                                StringMgr.Get("misc.unattend_confirm")))
        return;
    // 1) 导出当前配置到临时文件
    CDuiString sWork = SystemInfoService::MakeTempWorkDir(L"unattendapply");
    CDuiString sXmlPath = sWork;
    sXmlPath += L"\\unattend.xml";
    CDuiString sXml;
    UcBuildApplyXml(sXml);
    UcNormalizeXmlViaDom(sXml, nTotal);
    // [FIX-S1-6] 写盘失败必须终止：违反 Page_Unattend.h:101 显式契约
    //（"返回 false 调用方不得继续跑 sysprep"）。旧实现忽略返回值，磁盘满/
    // 权限失败时仍拉起 sysprep —— 以 Panther 下旧 unattend.xml 执行，
    // 配置数据破坏级后果。
    if (!UcWriteApplyXml(sXmlPath, sXml))
    {
        m_ctx.HostNotify(StringMgr.Get("misc.sd6e4b309"),
                         L"unattend.xml 写入失败（磁盘满或权限不足），已取消应用。", 2);
        return;
    }
    // 2)/3) 复制到 %WINDIR%\\Panther\\unattend.xml 并拉起 sysprep
    UcLaunchSysprep(sXmlPath);
}

// ============================================================
// [R122] D2 行级右键菜单增强：复制命令 / 复制 Pass 名称 / 打开导出位置
// ============================================================
