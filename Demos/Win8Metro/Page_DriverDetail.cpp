#include "stdafx.h"
#include "ThemeTokens.h"   // [R201] 强调色令牌（Win10/11 均跟随系统主题色）
#include <string>   // [C++17] std::to_wstring：失败明细拼接
#include "OfflineSession.h"  // [R10-02] 离线会话经独立组件访问，去 CDataEngine 静态耦合
// ============================================================
// Page_DriverDetail.cpp —— 驱动页「详情面板」段（R69 从 Page_Driver.cpp 拆出）
// 承接 ShowDetail 及其 4 个段函数（原 [AUDIT-P2] 已拆结构）：
//   ShowDetail / DrvHighlightSelected / DrvAddDetailTitle / DrvAddDetailFields / DrvAddDetailButtons
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
void CPageDriver::ShowDetail(int nCat, int nRow)
{
    // [AUDIT-P2] 原 235 行 / CC=32（全仓最复杂函数），按职责拆为 4 个段函数：
    //   DrvHighlightSelected  高亮当前设备行
    //   DrvAddDetailTitle     标题行
    //   DrvAddDetailFields    属性字段表（13 项）
    //   DrvAddDetailButtons   回退/属性按钮区
    // 本函数退化为编排：定位 → 取缓存 → 依次装配 → 无跨段局部变量。
    // [R87] P1-14 记录当前选中行，供"回退驱动"按钮定位
    // [R162] 两分量必须同步写，走 RememberSelection 单一入口
    RememberSelection(nCat, nRow);
    CVerticalLayoutUI* pPanel = static_cast<CVerticalLayoutUI*>(
        m_ctx.HostFindControl(L"drv_detail"));
    if (!pPanel) return;
    pPanel->RemoveAll();
    CDuiString sKey;
    sKey.Format(L"drow_%d_%d", nCat, nRow);
    std::map<CDuiString, DriverRowInfo>::iterator it = m_mapDrvRow.find(sKey);
    if (it == m_mapDrvRow.end()) return;
    const DriverRowInfo& ri = it->second;
    DrvHighlightSelected(sKey);
    // [卡顿修复] 确保后台枚举在跑，但绝不阻塞 UI（原同步 DISM 在线枚举会卡 30~60s）
    StartPubEnum();
    CDuiString sKeyL = ri.sInfPath;
    sKeyL.MakeLower();
    const DriverPubInfo* pPub = nullptr;
    std::map<CDuiString, DriverPubInfo>::iterator p = m_mapDrvPub.find(sKeyL);
    if (p != m_mapDrvPub.end()) pPub = &p->second;
    const RECT rcPad = { 10, 0, 10, 0 };
    DrvAddDetailTitle(pPanel, ri, rcPad);
    DrvAddDetailFields(pPanel, ri, pPub, rcPad);
    DrvAddDetailButtons(pPanel, ri, rcPad);
}
// [AUDIT-P2] 高亮当前设备行：其它已展开行恢复白底
void CPageDriver::DrvHighlightSelected(const CDuiString& sKey)
{
// 高亮当前设备行（其它已展开行恢复白底）
for (std::map<CDuiString, DriverRowInfo>::iterator q = m_mapDrvRow.begin();
     q != m_mapDrvRow.end(); ++q)
{
    CControlUI* pR = m_ctx.HostFindControl(q->first);
    // [R209] 原为硬编码 #CCE4F7 / #FFFFFF，深色主题下未选中行是纯白块。
    if (pR) pR->SetBkColor(tokens::Token(q->first == sKey
                                        ? tokens::EToken::SurfaceHover
                                        : tokens::EToken::Surface));
}
}
// [AUDIT-P2] 详情面板标题行
void CPageDriver::DrvAddDetailTitle(CVerticalLayoutUI* pPanel,
                                    const DriverRowInfo& ri, const RECT& rcPad)
{
auto pTitle = std::make_unique<CTextUI>(); // [C++17 R3]
pTitle->SetText(ri.sName.IsEmpty() ? StringMgr.Get("hint.s2733493e") : ri.sName);
pTitle->SetFixedHeight(34);
pTitle->SetFont(6);
pTitle->SetTextColor(tokens::Token(tokens::EToken::Ink));
pTitle->SetTextPadding(rcPad);
pTitle->SetAttribute(L"valign", L"vcenter");
pTitle->SetAttribute(L"endellipsis", L"true");
pTitle->SetMouseEnabled(false);
pPanel->Add(pTitle.release()); // [C++17 R3] ownership transferred to DuiLib
}
// [AUDIT-P2] 详情面板属性字段表（13 项，含"加载中…"占位）
void CPageDriver::DrvAddDetailFields(CVerticalLayoutUI* pPanel,
                                     const DriverRowInfo& ri,
                                     const DriverPubInfo* pPub,
                                     const RECT& rcPad)
{
struct DetailField { const wchar_t* pszLabel; const wchar_t* pszValue; };
CDuiString sUnknown = StringMgr.Get("hint.sdd622808");
// [卡顿修复] 后台枚举未完成时，发布名/厂商/版本/日期四项先显示“加载中…”，
// 枚举完成后 OnPubData 会自动重渲染当前详情。
const wchar_t* pszPubFallback = m_bPubLoading ? L"加载中…" : sUnknown.GetData();
// [R114-1] 问题代码格式化：CM_PROB_* 编号展示
CDuiString sProbCode;
sProbCode.Format(L"CM_PROB_%lu", (unsigned long)ri.dwProblem);
DetailField fields[] = {
    { StringMgr.Get("misc.s2f9f4b10"), ri.sInfPath.IsEmpty() ? sUnknown.GetData() : ri.sInfPath.GetData() },
    { StringMgr.Get("misc.see68d608"), (pPub && !pPub->sOriginal.IsEmpty()) ? pPub->sOriginal.GetData() : pszPubFallback },
    { StringMgr.Get("misc.s70aefc6a"),     (pPub && !pPub->sProvider.IsEmpty()) ? pPub->sProvider.GetData() : pszPubFallback },
    { StringMgr.Get("driver.s7459db51"), (pPub && !pPub->sVersion.IsEmpty())  ? pPub->sVersion.GetData()  : pszPubFallback },
    { StringMgr.Get("driver.s97aec505"), (pPub && !pPub->sDate.IsEmpty())     ? pPub->sDate.GetData()     : pszPubFallback },
    { StringMgr.Get("driver.sd22075af"), ri.sFile.IsEmpty() ? sUnknown.GetData() : ri.sFile.GetData() },
    // [R114-1] 详情面板增强：设备状态 / 问题代码 / 驱动服务名 / 设备实例 ID
    { StringMgr.Get("driver.s_devstatus_lbl"),
      ri.sDevStatus.IsEmpty() ? sUnknown.GetData() : ri.sDevStatus.GetData() },
    { StringMgr.Get("driver.s_problemcode_lbl"),
      (ri.dwProblem == 0) ? StringMgr.Get("driver.s_problem_none")
                          : sProbCode.GetData() },
    { StringMgr.Get("driver.s_service_lbl"),
      ri.sService.IsEmpty() ? sUnknown.GetData() : ri.sService.GetData() },
    { StringMgr.Get("driver.s_devinst_lbl"),
      ri.sDevInstId.IsEmpty() ? sUnknown.GetData() : ri.sDevInstId.GetData() },
    // [R120-2] 制造商 / 安装日期 / 硬件 ID
    { StringMgr.Get("driver.s_mfg_lbl"),
      ri.sMfg.IsEmpty() ? sUnknown.GetData() : ri.sMfg.GetData() },
    { StringMgr.Get("driver.s_installdate_lbl"),
      ri.sInstallDate.IsEmpty() ? sUnknown.GetData() : ri.sInstallDate.GetData() },
    { StringMgr.Get("driver.s_hardwareid_lbl"),
      ri.sHardwareId.IsEmpty() ? sUnknown.GetData() : ri.sHardwareId.GetData() },
};
for (int i = 0; i < static_cast<int>(sizeof(fields)/sizeof(fields[0])); ++i)
{
    auto pRow = std::make_unique<CHorizontalLayoutUI>(); // [C++17 R3]
    pRow->SetFixedHeight(28);
    pRow->SetInset(rcPad);
    pRow->SetChildPadding(8);
    auto pLbl = std::make_unique<CTextUI>(); // [C++17 R3]
    pLbl->SetText(fields[i].pszLabel);
    pLbl->SetFixedWidth(78);
    pLbl->SetTextColor(tokens::Token(tokens::EToken::InkTertiary));
    pLbl->SetAttribute(L"valign", L"vcenter");
    pLbl->SetMouseEnabled(false);
    pRow->Add(pLbl.release()); // [C++17 R3] ownership transferred to DuiLib
    auto pVal = std::make_unique<CTextUI>(); // [C++17 R3]
    pVal->SetText(fields[i].pszValue);
    pVal->SetTextColor(tokens::Token(tokens::EToken::Ink));
    pVal->SetAttribute(L"valign", L"vcenter");
    pVal->SetAttribute(L"endellipsis", L"true");
    pVal->SetMouseEnabled(false);
    pRow->Add(pVal.release()); // [C++17 R3] ownership transferred to DuiLib
    pPanel->Add(pRow.release()); // [C++17 R3] ownership transferred to DuiLib
}
}
// [AUDIT-P2] 详情面板按钮区：回退驱动 / 属性
void CPageDriver::DrvAddDetailButtons(CVerticalLayoutUI* pPanel,
                                      const DriverRowInfo& ri, const RECT& rcPad)
{
// [R87] P1-14 详情面板追加"回退驱动"按钮：把 Windows 自动保存的上一版
//      驱动键（Backup\Driver）写回 Driver，再 DIF_PROPERTYCHANGE 重载设备。
//      仅在存在可回退备份时启用。
{
    auto pBtnRow = std::make_unique<CHorizontalLayoutUI>(); // [C++17 R3]
    pBtnRow->SetFixedHeight(36);
    pBtnRow->SetInset(rcPad);
    pBtnRow->SetChildPadding(8);
    CDuiString sBackup;
    bool bCanRollback = !ri.sDevInstId.IsEmpty()
        && DriverService::HasRollbackCandidate(ri.sDevInstId, sBackup);
    auto pRoll = std::make_unique<CButtonUI>(); // [C++17 R3]
    pRoll->SetAttribute(L"borderround", L"4,4");   // Win11 Fluent control radius
    pRoll->SetFixedHeight(30);
    pRoll->SetFixedWidth(130);
    pRoll->SetFont(1);
    pRoll->SetBkColor(bCanRollback ? tokens::Token(tokens::EToken::Accent)
                                   : tokens::Token(tokens::EToken::SurfaceSunk));
    pRoll->SetHotBkColor(bCanRollback ? tokens::Token(tokens::EToken::AccentHot)
                                      : tokens::Token(tokens::EToken::SurfaceSunk));
    pRoll->SetTextColor(tokens::Token(tokens::EToken::OnAccent));
    pRoll->SetText(bCanRollback ? StringMgr.Get("driver.s_rollback_btn")
                               : StringMgr.Get("driver.s_rollback_disabled"));
    pRoll->SetName(L"btn_drv_rollback");   // [R87] P1-14 事件路由定位
    pRoll->SetEnabled(bCanRollback);
    // [R120-2] 追加"属性"按钮：打开设备管理器式属性对话框
    //          仅当设备实例 ID 已解析时才可用（无 DEVINST 无法定位）。
    {
        auto pProp = std::make_unique<CButtonUI>(); // [C++17 R3]
        pProp->SetAttribute(L"borderround", L"4,4");   // Win11 Fluent control radius
        pProp->SetFixedHeight(30);
        pProp->SetFixedWidth(90);
        pProp->SetFont(1);
        pProp->SetBkColor(tokens::Token(tokens::EToken::Accent));
        pProp->SetHotBkColor(tokens::Token(tokens::EToken::AccentHot));
        pProp->SetTextColor(tokens::Token(tokens::EToken::OnAccent));
        pProp->SetText(StringMgr.Get("driver.s_props_btn"));
        pProp->SetName(L"btn_drv_properties");  // [R120-2] 事件路由定位
        pProp->SetEnabled(!ri.sDevInstId.IsEmpty());
        pBtnRow->Add(pProp.release()); // [C++17 R3] ownership transferred to DuiLib
    }
    pBtnRow->Add(pRoll.release()); // [C++17 R3] ownership transferred to DuiLib
    if (bCanRollback)
    {
        auto pHint = std::make_unique<CTextUI>(); // [C++17 R3]
        pHint->SetText(StringMgr.Get("driver.s_rollback_hint"));
        pHint->SetFixedHeight(30);
        pHint->SetTextColor(tokens::Token(tokens::EToken::InkMuted));
        pHint->SetFont(-1);   // [FIX-FONT0] 原 SetFont(0)：皮肤无 id=0，显式用 -1 走默认字体
        pHint->SetMouseEnabled(false);
        pBtnRow->Add(pHint.release()); // [C++17 R3] ownership transferred to DuiLib
    }
    pPanel->Add(pBtnRow.release()); // [C++17 R3] ownership transferred to DuiLib
}
}
