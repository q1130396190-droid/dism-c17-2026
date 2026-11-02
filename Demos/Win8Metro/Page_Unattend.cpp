#include "stdafx.h"
#include "ThemeTokens.h"   // [R201] 强调色令牌（Win10/11 均跟随系统主题色）
#include "SafeEnv.h"   // [B6] 安全环境变量读取：替代弃用 _wgetenv
#include <algorithm>
#include <memory>   // [C++17] VARIANT 参数数组现由 std::vector 接管（memory 保留供后续）
#include "OfflineSession.h"  // [R10-02] 离线会话经独立组件访问，去 CDataEngine 静态耦合

// Page_Unattend.cpp —— v2 P2 纵切（页 10：无人参与/预应答）
// 12 个函数 + XmlEscape/XmlUnescape/kUcMap 从 FrameWnd.cpp 原样迁入。

#include "Page_Unattend.h"
#include "Page_UnattendInternal.h"   // FileGuard / HandleGuard（跨 TU 共享）
#include "CustomControls.h"      // CClickRowUI（P2 从 FrameWnd.cpp 抽出的自定义控件）
#include "StringManager.h"
#include "DismCore.h"            // [R-P26 / R5] Rust dismcore：XML 标签段定位 FindXmlTag
#include "FrameWnd_Util.h"          // RecalcBodyHeight / CopyTextToClipboard
#include "FrameWnd_UiHelpers.h"  // P8：ParsePrefix2
#include "Win32DialogService.h"
#include "ExternalToolService.h"  // [R71] IsElevated
#include "SystemInfoService.h"  // [R71] MakeTempWorkDir
#include "Registry.h"  // [R71] RegGetString
#include "DataEngine.h"  // [R-B79] IsOfflineSession/GetOfflineImageRoot（FirstBoot 写离线 SYSTEM 蜂巢）
#include <setupapi.h>              // [R71] SetupDiGetClassDevs
// [R130] MSXML DOM 直连（msxml6.h 在本机 SDK 不可用，走 IDispatch 自动化路径）
#include <objbase.h>
#include <oleauto.h>
#include <comdef.h>
// [C++17 升级阶段2] 局部句柄 RAII 守卫：隐式转换为原句柄类型，调用点
// 零改动；只负责早退/异常路径自动释放，不改变任何调用顺序与业务分支。
// PROCESS_INFORMATION（sysprep 单出口配对关闭）与 SetupDi 设备信息句柄
// （SetupDiDestroyDeviceInfoList 收尾）保持显式管理。

using namespace DuiLib;
using i18n::CStringManager;

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")



// ============================================================
// [R130] MSXML DOM 直连：通过 IDispatch 自动化访问 Msxml2.DOMDocument.6.0
// 参考 AppxCapUpdService.cpp 的 AutoWrap + CLSIDFromProgID 模式。
// 分级返回码：0=成功 1=COM 未初始化 2=CLSID 解析失败 3=CreateInstance 失败
//             4=loadXML 失败 5=parseError 非零 6=selectNodes 失败 7=item 失败
//             8=属性读取失败
// ============================================================





// 用文件路径解析 XML（返回 0 成功，分级码见顶）

// ============================================================
// unattend page (four collapsible phases)
// ============================================================

// [R173] 从 Build（原 101 行，页 maxLen 101）抽出：单个组的组头。
// 组头 = 箭头 + 名称 + pass 技术徽章 + 条目计数徽章，四个子控件全部在
// 返回前挂入，因此调用方拿到的是已完备的组头（不会再对已释放指针解引用）。
// 返回基类 CControlUI*：调用方只做 pGroups->Add，不需要具体类型。
[[nodiscard]] CControlUI* CPageUnattend::UcBuildGroupHeader(int n, const wchar_t* pszName,
                                                            const wchar_t* pszPassLabel)
{
    auto pHdr = std::make_unique<CClickRowUI>(); // [C++17 R3]
    pHdr->SetFixedHeight(38);
    RECT rcInset = { 14, 0, 10, 0 };
    pHdr->SetInset(rcInset);
    pHdr->SetChildPadding(8);
    pHdr->SetBkColor(tokens::Token(tokens::EToken::Surface));
    pHdr->SetAttribute(L"hotbkcolor", tokens::TokenHex(tokens::EToken::SurfaceHover));   // [R235] 原 #FFF5F5F5
    CDuiString sHdrName;
    sHdrName.Format(L"uchdr_%d", n);
    pHdr->SetName(sHdrName);
    auto pArrow = std::make_unique<CTextUI>(); // [C++17 R3]
    CDuiString sArName;
    sArName.Format(L"ucarr_%d", n);
    pArrow->SetName(sArName);
    pArrow->SetFixedWidth(14);
    pArrow->SetFont(9);
    pArrow->SetText(m_ctx.HostGlyphArrow(0));
    pArrow->SetTextColor(tokens::Token(tokens::EToken::InkMuted));
    pArrow->SetAttribute(L"valign", L"vcenter");
    pArrow->SetMouseEnabled(false);
    pHdr->Add(pArrow.release());  // [C++17 R3] ownership to DuiLib
    auto pLabel = std::make_unique<CTextUI>(); // [C++17 R3]
    pLabel->SetText(pszName);
    pLabel->SetFixedWidth(0);
    pLabel->SetFont(6);
    pLabel->SetTextColor(tokens::Token(tokens::EToken::Ink));
    pLabel->SetAttribute(L"valign", L"vcenter");
    pLabel->SetMouseEnabled(false);
    pHdr->Add(pLabel.release());  // [C++17 R3] ownership to DuiLib
    // [R53] Pass 技术标签徽章：显示实际 <settings pass="..."> 值
    // 帮助开发者对照 Windows Setup XML 规范，识别当前组对应哪个 pass
    auto pPassBadge = std::make_unique<CTextUI>(); // [C++17 R3]
    pPassBadge->SetText(pszPassLabel);
    pPassBadge->SetFixedWidth(0);
    pPassBadge->SetFont(9);
    pPassBadge->SetTextColor(tokens::Token(tokens::EToken::AccentText));
    pPassBadge->SetAttribute(L"valign", L"vcenter");
    pPassBadge->SetAttribute(L"padding", L"6,0,6,0");
    pPassBadge->SetAttribute(L"bkcolor", tokens::TokenHex(tokens::EToken::AccentSoft));   // [R238] 原固定 #EEEBF5：深色主题下是亮块，且切换时不会被刷新
    pPassBadge->SetMouseEnabled(false);
    pHdr->Add(pPassBadge.release());  // [C++17 R3] ownership to DuiLib
    // [R129-2] D1 证据：组头条目计数徽章（uc_count_%d 显示该组命令数）
    // [FIX R22-1] 徽章须在 pHdr.release() 前挂入：旧顺序对已释放的空 unique_ptr 解引用属 UB，n=0 轮即中断 Build（预应答页只剩首组的根因）
    auto pCount = std::make_unique<CTextUI>(); // [C++17 R3]
    CDuiString sCntName;
    sCntName.Format(L"uc_count_%d", n);
    pCount->SetName(sCntName);
    pCount->SetFixedWidth(0);
    pCount->SetFont(9);
    pCount->SetText(L"0");
    pCount->SetTextColor(tokens::Token(tokens::EToken::InkTertiary));
    pCount->SetAttribute(L"valign", L"vcenter");
    pCount->SetAttribute(L"padding", L"6,0,6,0");
    pCount->SetAttribute(L"bkcolor", tokens::TokenHex(tokens::EToken::SurfaceAlt));   // [R238] 原固定 #EEF2F7：同上
    pCount->SetMouseEnabled(false);
    pHdr->Add(pCount.release());  // [C++17 R3] ownership to DuiLib
    return pHdr.release(); // [C++17 R3] ownership transferred to caller
}

// [R173] 从 Build 抽出：单个组的组体（默认隐藏 + 折叠高度重算）。
// [FIX] 先置隐藏再 Add：否则父容器首次 EstimateSize 会按可见计高，折叠后不重算。
[[nodiscard]] CControlUI* CPageUnattend::UcBuildGroupBody(int n)
{
    auto pBody = std::make_unique<CVerticalLayoutUI>(); // [C++17 R3]
    CDuiString sBodyName;
    sBodyName.Format(L"ucbody_%d", n);
    pBody->SetName(sBodyName);
    pBody->SetChildPadding(0);
    pBody->SetVisible(false);   // [FIX] 先置隐藏再 Add：否则父容器首次 EstimateSize 会按可见计高，折叠后不重算
    RecalcBodyHeight(pBody.get());
    return pBody.release(); // [C++17 R3] ownership transferred to caller
}

void CPageUnattend::Build()
{
    // [R-fix] 重入保护：仅首轮清空数据（F5/切页重入保留命令、展开与选中态）
    const bool bFirst = BeginUcBuild();
    // [R18 P0 修正] 四组对应真实 Windows Setup configuration pass，组索引 0..3 保持不变（m_vUcCmds[4] / kPassToGroup 导入导出均依赖）。
    const wchar_t* pszNames[4] = {
        StringMgr.Get("program.sd97cee76"),
        StringMgr.Get("misc.s65809264"),
        StringMgr.Get("misc.se9706319"),
        StringMgr.Get("misc.s8ef1756d")
    };
    // [R53] Pass 技术标签：与 Import 的 kPassToGroup 严格对齐（WindowsPE/Specialize/OOBESystem/AuditSystem）
    // 供开发者直接看到 XML 中 <settings pass="..."> 的实际值
    // [R55] 追加 ASCII 图标前缀（避免 emoji 在 GBK/非彩色字体下渲染问题，用括号字母替代）
    static const wchar_t* sPassLabel[4] = {
        L"[WPE] windowsPE",
        L"[SPC] specialize",
        L"[OOB] oobeSystem",
        L"[AUD] auditSystem"
    };
    CVerticalLayoutUI* pGroups = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"uc_groups"));
    if (!pGroups) return;
    pGroups->RemoveAll();
    for (int n = 0; n < 4; ++n)
    {
        // [R173] 组头/组体装配抽为 UcBuildGroupHeader / UcBuildGroupBody。
        // [R22-1] 顺序不可交换：先组头后组体，且组头内部四个子控件已全部挂好。
        CControlUI* pHdr = UcBuildGroupHeader(n, pszNames[n], sPassLabel[n]);
        CControlUI* pBody = UcBuildGroupBody(n);
        pGroups->Add(pHdr);   // [C++17 R3] ownership to DuiLib
        pGroups->Add(pBody);  // [C++17 R3] ownership to DuiLib
    }
    // [R-fix] 重入恢复：UI 树已重建但 m_vUcCmds 内容仍在（正文见文件末 RestoreUcBuild）
    if (!bFirst) RestoreUcBuild();
    // [U-3] 页面收尾状态条：显示无人参与页完成情况
    {
        CDuiString sPageStatus;
        sPageStatus.Format(StringMgr.Get("misc.h85ea"), 0);
        m_ctx.HostLog(sPageStatus.GetData());
    }
    UpdateButtons();
}

void CPageUnattend::ToggleGroup(int n)
{
    CDuiString sBodyName;
    sBodyName.Format(L"ucbody_%d", n);
    CVerticalLayoutUI* pBody = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(sBodyName));
    CDuiString sArName;
    sArName.Format(L"ucarr_%d", n);
    CTextUI* pArrow = static_cast<CTextUI*>(m_ctx.HostFindControl(sArName));
    if (!pBody) return;
    bool bShow = !pBody->IsVisible();
    pBody->SetVisible(bShow);
    if (n >= 0 && n < static_cast<int>(m_vUcExpanded.size())) m_vUcExpanded[n] = bShow;
    if (pArrow) pArrow->SetText(m_ctx.HostGlyphArrow(bShow ? 1 : 0));
    CVerticalLayoutUI* pGroups = static_cast<CVerticalLayoutUI*>(
        m_ctx.HostFindControl(L"uc_groups"));
    if (pGroups)
    {
        pGroups->SetPos(pGroups->GetPos());
        if (pGroups->GetParent())
            pGroups->GetParent()->SetPos(pGroups->GetParent()->GetPos());
    }
}

void CPageUnattend::RebuildBody(int n)
{
    CDuiString sBodyName;
    sBodyName.Format(L"ucbody_%d", n);
    CVerticalLayoutUI* pBody = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(sBodyName));
    if (!pBody) return;
    pBody->RemoveAll();
    const std::vector<CDuiString>& v = m_vUcCmds[n];
    // [修复 R5 / P2-22] 空分组给出中文空态，而不是一块空白
    if (v.empty())
    {
        auto pEmpty = std::make_unique<CTextUI>(); // [C++17 R3]
        pEmpty->SetText(StringMgr.Get("hint.sc1949b09"));
        pEmpty->SetFixedHeight(30);
        pEmpty->SetTextColor(tokens::Token(tokens::EToken::InkMuted));
        RECT rcEmptyInset = { 40, 0, 10, 0 };
        pEmpty->SetTextPadding(rcEmptyInset);
        pEmpty->SetAttribute(L"valign", L"vcenter");
        pEmpty->SetMouseEnabled(false);
        pBody->Add(pEmpty.release());  // [C++17 R3] ownership to DuiLib
        RecalcBodyHeight(pBody);
        CVerticalLayoutUI* pGroups0 = static_cast<CVerticalLayoutUI*>(
            m_ctx.HostFindControl(L"uc_groups"));
        if (pGroups0) pGroups0->SetPos(pGroups0->GetPos());
        return;
    }
    for (size_t k = 0; k < v.size(); ++k)
    {
        bool bSel = (n == m_nUcSelGroup && static_cast<int>(k) == m_nUcSelItem);
        auto pRow = std::make_unique<CClickRowUI>(); // [C++17 R3]
        pRow->SetFixedHeight(30);
        pRow->SetRowSelected(bSel);
        RECT rcInset = { 40, 0, 10, 0 };
        pRow->SetInset(rcInset);
        CDuiString sRowName;
        sRowName.Format(L"ucrow_%d_%d", n, static_cast<int>(k));
        pRow->SetName(sRowName);
        auto pText = std::make_unique<CTextUI>(); // [C++17 R3]
        pText->SetText(v[k]);
        pText->SetFixedWidth(0);
        pText->SetTextColor(bSel ? tokens::Token(tokens::EToken::OnAccent)
                                 : tokens::Token(tokens::EToken::Ink));
        pText->SetAttribute(L"valign", L"vcenter");
        pText->SetAttribute(L"endellipsis", L"true");
        pText->SetMouseEnabled(false);
        pRow->Add(pText.release());  // [C++17 R3] ownership to DuiLib
        pBody->Add(pRow.release());  // [C++17 R3] ownership to DuiLib
    }
    RecalcBodyHeight(pBody);
    CVerticalLayoutUI* pGroups = static_cast<CVerticalLayoutUI*>(
        m_ctx.HostFindControl(L"uc_groups"));
    if (pGroups) pGroups->SetPos(pGroups->GetPos());
}

void CPageUnattend::OnRowSelect(int g, int k)
{
    RememberUcSelection(g, k);
    for (int n = 0; n < 4; ++n) RebuildBody(n);
    UpdateButtons();
}

// [C2] 预应答命令行右键菜单：上移 / 编辑 / 删除（复用底部按钮逻辑，自带确认/重建）
// [R177] 从 ShowContextMenu（26 行 / CC13）抽出：行级右键菜单命令分派。
// 六个命令与菜单项一一对应；4301（上移）额外要求 k>0（首行无法上移，
// 菜单项此时本就置灰，这里再判一次是防键盘/自动化路径绕过置灰）。
// 未命中任何命令时什么都不做（原 else-if 链自然落到末尾）。
void CPageUnattend::UcDispatchRowCmd(int iCmd, int g, int k)
{
    switch (iCmd)
    {
    case 4301: if (k > 0) Up();          return;   // 上移
    case 4302: Delete();                 return;   // 删除
    case 4303: Edit(g, k);               return;   // 编辑
    case 4304: CopyRowCommand(g, k);     return;   // 复制命令
    case 4305: CopyRowPass(g);           return;   // 复制 Pass 名称
    case 4306: OpenExportLocation();     return;   // 打开导出位置
    default: return;
    }
}

void CPageUnattend::ShowContextMenu(int g, int k)
{
    if (g < 0 || g >= 4) return;
    if (k < 0 || k >= static_cast<int>(m_vUcCmds[g].size())) return;
    OnRowSelect(g, k);
    HMENU hMenu = ::CreatePopupMenu();
    ::AppendMenu(hMenu, MF_STRING | (k > 0 ? 0 : MF_GRAYED), 4301, StringMgr.Get("misc.s487e74ed"));
    // [R31-C] 编辑能力：选中行弹出编辑对话框
    ::AppendMenu(hMenu, MF_STRING, 4303, StringMgr.Get("misc.h6597"));
    ::AppendMenu(hMenu, MF_STRING, 4302, StringMgr.Get("edit.s33b0daaa"));
    // [R122] D2 增强：复制命令 / 复制 Pass 名称 / 打开导出位置（行级右键）
    ::AppendMenu(hMenu, MF_STRING, 4304, StringMgr.Get("unattend.s_ctx_copycmd"));
    ::AppendMenu(hMenu, MF_STRING, 4305, StringMgr.Get("unattend.s_ctx_copypass"));
    ::AppendMenu(hMenu, MF_STRING, 4306, StringMgr.Get("unattend.s_ctx_openexport"));
    POINT pt; ::GetCursorPos(&pt);
    int iCmd = ::TrackPopupMenu(hMenu,
        TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
        pt.x, pt.y, 0, m_ctx.HostHwnd(), nullptr);
    ::DestroyMenu(hMenu);
    // [R177] 命令分派抽为 UcDispatchRowCmd（原 6 段 else-if 链，CC 记账 5 个判定点）
    UcDispatchRowCmd(iCmd, g, k);
}

// [R31-C] 编辑已有命令行
void CPageUnattend::Edit(int g, int k)
{
    if (g < 0 || g >= 4) return;
    if (k < 0 || k >= static_cast<int>(m_vUcCmds[g].size())) return;
    // 用 uc_edit 控件承载编辑（复用）
    CEditUI* pEdit = static_cast<CEditUI*>(m_ctx.HostFindControl(L"uc_edit"));
    if (!pEdit) return;
    CDuiString sOld = m_vUcCmds[g][k];
    pEdit->SetText(sOld);
    m_ctx.HostSetFocusControl(pEdit);
    // [R31-C] 全选以便直接覆盖
    int nLen = sOld.GetLength();
    pEdit->SetSel(0, nLen);
    // 记住待编辑索引，等用户按"添加"时替换而非追加
    m_nUcEditGroup = g;
    m_nUcEditItem = k;
}

// [R177] 从 Add（47 行 / CC13）抽出：空输入拒绝。
// 弹提示 → 把焦点送回输入框，保证用户不需要再点一次才能继续输入。
void CPageUnattend::UcRejectEmptyInput()
{
    m_ctx.HostAlert(StringMgr.Get("unattend.saf8d91fc"),
                    StringMgr.Get("misc.s83f747f8"), MB_ICONINFORMATION);
    CEditUI* pEdit = static_cast<CEditUI*>(m_ctx.HostFindControl(L"uc_edit"));
    if (pEdit) m_ctx.HostSetFocusControl(pEdit);
}

// [R177] 从 Add 抽出：编辑模式下的"替换而非追加"。
// 命中条件三重：编辑组 == 当前组、编辑项 >=0、编辑项在范围内。
// 任一不满足即返回 false，调用方走追加路径。
// 替换成功后清空编辑态（EditGroup=-1）是必需的：否则下一次 Add 仍会走替换，
// 表现为"输入新命令却覆盖了上一条"。
bool CPageUnattend::UcTryReplaceEditing(int g, const CDuiString& sCmd)
{
    if (m_nUcEditGroup != g) return false;
    if (m_nUcEditItem < 0) return false;
    if (m_nUcEditItem >= static_cast<int>(m_vUcCmds[g].size())) return false;
    m_vUcCmds[g][m_nUcEditItem] = sCmd;
    RememberUcSelection(g, m_nUcEditItem);
    m_nUcEditGroup = -1;
    m_nUcEditItem = -1;
    RebuildBody(g);
    UpdateButtons();
    CEditUI* pEdit = static_cast<CEditUI*>(m_ctx.HostFindControl(L"uc_edit"));
    if (pEdit) { pEdit->SetText(L""); m_ctx.HostSetFocusControl(pEdit); }
    return true;
}

void CPageUnattend::Add()
{
    CEditUI* pEditSrc = static_cast<CEditUI*>(m_ctx.HostFindControl(L"uc_edit"));
    CDuiString sCmd = pEditSrc ? pEditSrc->GetText() : CDuiString(L"");
    // [R177] 空输入提示与回焦抽为 UcRejectEmptyInput（原内联 5 行 + 3 个判定点）
    if (sCmd.IsEmpty()) { UcRejectEmptyInput(); return; }
    int g = m_nUcSelGroup;
    if (g < 0)
    {
        g = 0;
        for (int n = 0; n < 4; ++n)
            if (m_vUcExpanded[n]) { g = n; break; }
    }
    // [R31-C] 编辑模式：若 m_nUcEditItem 有效且与当前 g 匹配，替换而非追加
    // [R177] 替换分支抽为 UcTryReplaceEditing（命中返回 true，调用方须 return）
    if (UcTryReplaceEditing(g, sCmd)) return;
    m_vUcCmds[g].push_back(sCmd);
    RememberUcSelection(g, static_cast<int>(m_vUcCmds[g].size()) - 1);
    if (!m_vUcExpanded[g]) ToggleGroup(g);
    RebuildBody(g);
    UpdateButtons();
    CEditUI* pEdit = static_cast<CEditUI*>(m_ctx.HostFindControl(L"uc_edit"));
    if (pEdit)
    {
        pEdit->SetText(L"");
        m_ctx.HostSetFocusControl(pEdit);
    }
}

void CPageUnattend::Delete()
{
    if (m_nUcSelGroup < 0 || m_nUcSelItem < 0) return;
    int g = m_nUcSelGroup;
    m_vUcCmds[g].erase(m_vUcCmds[g].begin() + m_nUcSelItem);
    RememberUcSelection(-1, -1);
    RebuildBody(g);
    UpdateButtons();
}

void CPageUnattend::Up()
{
    if (m_nUcSelGroup < 0 || m_nUcSelItem <= 0) return;
    int g = m_nUcSelGroup;
    int k = m_nUcSelItem;
    std::swap(m_vUcCmds[g][k], m_vUcCmds[g][k - 1]);
    RememberUcSelection(m_nUcSelGroup, k - 1);
    RebuildBody(g);
    UpdateButtons();
}

// 预应答页：按当前选择启用/禁用「上移/删除」，并刷新底部状态行
// [R164] 组计数徽章刷新（从 UpdateButtons 抽出，原含 for + if×2 判定）。
// 空组灰、非空蓝（视觉区分）。
void CPageUnattend::UcRefreshGroupBadges()
{
    for (int n = 0; n < 4; ++n)
    {
        CDuiString sCntName;
        sCntName.Format(L"uc_count_%d", n);
        CTextUI* pCnt = static_cast<CTextUI*>(m_ctx.HostFindControl(sCntName.GetData()));
        if (!pCnt) continue;
        CDuiString sCntText;
        sCntText.Format(L"%d", static_cast<int>(m_vUcCmds[n].size()));
        pCnt->SetText(sCntText.GetData());
        pCnt->SetTextColor(m_vUcCmds[n].empty() ? tokens::Token(tokens::EToken::InkMuted) : tokens::Token(tokens::EToken::Accent));
    }
}

// [R164] 底部状态条文案（从 UpdateButtons 抽出）。
void CPageUnattend::UcRefreshStatusLine(bool bSel, int nTotal)
{
    CTextUI* pSt = static_cast<CTextUI*>(m_ctx.HostFindControl(L"lbl_uc_status"));
    if (!pSt) return;
    CDuiString sStatus;
    if (bSel)
        sStatus.Format(StringMgr.Get("misc.sdfba1679"),
                       nTotal, m_nUcSelGroup + 1, m_nUcSelItem + 1);
    else
        sStatus.Format(StringMgr.Get("edit.sa167df5b"), nTotal);
    pSt->SetText(sStatus);
}

void CPageUnattend::UpdateButtons()
{
    const bool bSel = (m_nUcSelGroup >= 0 && m_nUcSelGroup < 4 && m_nUcSelItem >= 0
                       && m_nUcSelItem < static_cast<int>(m_vUcCmds[m_nUcSelGroup].size()));
    const bool bCanUp = bSel && m_nUcSelItem > 0;
    CButtonUI* pBtnUp  = static_cast<CButtonUI*>(m_ctx.HostFindControl(L"btn_uc_up"));
    CButtonUI* pBtnDel = static_cast<CButtonUI*>(m_ctx.HostFindControl(L"btn_uc_del"));
    if (pBtnUp)
    {
        pBtnUp->SetEnabled(bCanUp);
        pBtnUp->SetTextColor(bCanUp ? tokens::Token(tokens::EToken::Ink) : tokens::Token(tokens::EToken::InkMuted));
    }
    if (pBtnDel)
    {
        pBtnDel->SetEnabled(bSel);
        pBtnDel->SetTextColor(bSel ? tokens::Token(tokens::EToken::Ink) : tokens::Token(tokens::EToken::InkMuted));
    }
    int nTotal = 0;
    for (int n = 0; n < 4; ++n) nTotal += static_cast<int>(m_vUcCmds[n].size());
    // [R129-2] D1 证据：刷新各组计数徽章（Build 创建的 uc_count_%d）
    UcRefreshGroupBadges();
    UcRefreshStatusLine(bSel, nTotal);
}


// [R149] kUcMap 已提升到 Page_UnattendInternal.h（跨 TU 共享单一真源），
// 本文件的 ApplyUnattend / CopyRowPass 经该头直接引用。


// ============================================================
// P8：第二级事件路由（原 CFrameWnd::Notify / WM_RBUTTONUP /
// VK_DELETE 预应答页分支逐字迁入）
// ============================================================

// [R164] click 按钮分派（从 OnEvent 抽出，原 8 段 if-else 链，CC15）。
// 表放在成员函数体内：成员函数指针取地址在类作用域外是非法访问。
bool CPageUnattend::UcDispatchClickButton(const CDuiString& sName)
{
    using BtnHandler = void (CPageUnattend::*)();
    struct BtnRoute { const wchar_t* pszName; BtnHandler pfn; };
    static constexpr BtnRoute kBtns[] = {
        { L"btn_uc_add",    &CPageUnattend::Add },             // 新增命令
        { L"btn_uc_up",     &CPageUnattend::Up },              // 上移
        { L"btn_uc_del",    &CPageUnattend::Delete },          // 删除
        { L"btn_uc_export", &CPageUnattend::Export },          // 导出 unattend.xml
        { L"btn_uc_import", &CPageUnattend::Import },          // 导入 unattend.xml
        { L"btn_uc_apply",  &CPageUnattend::ApplyUnattend },   // [R71] 应用预应答配置
        { L"btn_sysprep",   &CPageUnattend::RunSysprep },      // [R71] 执行 sysprep
        { L"btn_ibs",       &CPageUnattend::BuildIBSReport },  // [R71] 生成 IBS 报告
    };
    for (const BtnRoute& r : kBtns)
    {
        if (sName != r.pszName) continue;
        (this->*r.pfn)();
        return true;
    }
    return false;
}

void CPageUnattend::OnEvent(TNotifyUI& msg)
{
    CDuiString sName = msg.pSender ? msg.pSender->GetName() : CDuiString(L"");
    const wchar_t* pszName = sName.GetData();
    // ---- 行点击/双击：ucrow_<g>_<k> → 选中该行 ----
    if (msg.sType == L"click" || msg.sType == L"dbclick")
    {
        // ---- unattend group headers ----
        if (msg.sType == L"click" && wcsncmp(pszName, L"uchdr_", 6) == 0)
        {
            ToggleGroup(_ttoi(pszName + 6));
            return;
        }
        int nUcG = -1, nUcK = -1;
        if (ParsePrefix2(sName, L"ucrow_", nUcG, nUcK))
        {
            OnRowSelect(nUcG, nUcK);
            return;
        }
        if (msg.sType == L"click") UcDispatchClickButton(sName);
    }
}

// P8：原 CFrameWnd WM_RBUTTONUP ucrow_ 命中块（仅鼠标消息）
bool CPageUnattend::OnContextMenu(POINT pt, bool bFromContextMsg)
{
    if (bFromContextMsg) return false;
    CControlUI* pHit = m_ctx.HostFindControlAt(pt);
    for (CControlUI* p = pHit; p; p = p->GetParent())
    {
        const wchar_t* psz = p->GetName();
        if (!psz) continue;
        if (wcsncmp(psz, L"ucrow_", 6) == 0)
        {
            int g = -1, k = -1;
            if (_stscanf_s(psz + 6, L"%d_%d", &g, &k) == 2)
                ShowContextMenu(g, k);
            return true;
        }
    }
    return false;
}

// P8：原 CFrameWnd WM_KEYDOWN VK_DELETE nPage==10 分支
bool CPageUnattend::OnDeleteKey()
{
    if (!HasSelection()) return false;
    Delete();
    return true;
}


// ============================================================
// [R122] D2 行级右键菜单增强：复制命令 / 复制 Pass 名称 / 打开导出位置
// ============================================================
void CPageUnattend::CopyRowCommand(int g, int k)
{
    if (g < 0 || g >= 4) return;
    if (k < 0 || k >= static_cast<int>(m_vUcCmds[g].size())) return;
    OnRowSelect(g, k);
    FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), m_vUcCmds[g][k].GetData());
    CDuiString sLog;
    sLog.Format(L"[unattend] copy cmd g=%d k=%d", g, k);
    m_ctx.HostLog(sLog.GetData());
    m_ctx.HostToast(StringMgr.Get("unattend.s_ctx_copycmd_done"),
                    StringMgr.Get("unattend.s_ctx_copycmd_done_body"), 0);
}

void CPageUnattend::CopyRowPass(int g)
{
    if (g < 0 || g >= 4) return;
    if (static_cast<int>(m_vUcCmds[g].size()) == 0) return;
    FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), kUcMap[g].pszPass);
    CDuiString sLog;
    sLog.Format(L"[unattend] copy pass g=%d pass=%s", g, kUcMap[g].pszPass);
    m_ctx.HostLog(sLog.GetData());
    m_ctx.HostToast(StringMgr.Get("unattend.s_ctx_copypass_done"),
                    StringMgr.Get("unattend.s_ctx_copypass_done_body"), 0);
}

void CPageUnattend::OpenExportLocation()
{
    if (m_sLastExportPath.IsEmpty())
    {
        m_ctx.HostToast(StringMgr.Get("unattend.s_ctx_openexport_none"),
                        StringMgr.Get("unattend.s_ctx_openexport_none_body"), 1);
        return;
    }
    if (UiHelpers_OpenExplorerSelect(m_ctx.HostHwnd(), m_sLastExportPath.GetData()))
    {
        CDuiString sLog;
        sLog.Format(L"[unattend] explorer /select: %s", m_sLastExportPath.GetData());
        m_ctx.HostLog(sLog.GetData());
    }
    else
    {
        m_ctx.HostToast(StringMgr.Get("unattend.s_ctx_openexport_none"),
                        StringMgr.Get("unattend.s_ctx_openexport_none_body"), 1);
    }
}
// ============================================================
// [R-B1] Ctrl+C / Ctrl+X 复制当前 Pass 组首个命令
// ============================================================

void CPageUnattend::OnGlobalCopy(bool /*bCut*/)
{
    int g = m_nUcSelGroup;
    if (g < 0) g = 0;
    if (g >= 4) g = 0;
    if (m_vUcCmds[g].empty())
    {
        m_ctx.HostToast(StringMgr.Get("unattend.s_nocopy"),
                        StringMgr.Get("unattend.s_nocopy_body"), 1);
        return;
    }
    CopyRowCommand(g, 0);
}

// ============================================================
// [R-B1] Ctrl+S 无人值守导出
// ============================================================

void CPageUnattend::OnGlobalSave()
{
    Export();
    m_ctx.HostLog(L"[unattend] Ctrl+S export");
}

// ============================================================
// [R-B79] 33号缺口：IBS FirstBoot 注册表四落键
// ------------------------------------------------------------
// 知识库 28_v33 §3（System\Setup\FirstBoot 字节级闭合）：
//   System\Setup\FirstBoot\PreSysPrep / PostSysPrep / PreOOBE / PostOOBE
// 原版由 IbsSetFirstBootCommandLine（导出 #61）在 WinPE 部署阶段写入**离线
// 映像**的 SYSTEM 蜂巢；本实现把四个阶段命令以 REG_SZ 默认值落到
// <离线根>\Windows\System32\config\SYSTEM 的同名键下（RegLoadKey 临时
// 挂载 → 写值 → RegUnLoadKey 落盘），与上侧 unattend.xml 注入双通道并存。
// 仅离线会话执行（知识库矩阵：该钩子在线环境 ❌ / WinPE ✅）。
// ============================================================

// [R-fix] Build() 首轮判定 + 数据清零（抽离以保持 Build() 主体行数稳定）。
// 返回 true 表示首次构建；false 表示 F5/切页重入，调用方须保留用户数据并走恢复路径。
[[nodiscard]] bool CPageUnattend::BeginUcBuild()
{
    const bool bFirst = !m_bUcBuilt;
    m_bUcBuilt = true;
    if (!bFirst) return false;
    m_vUcExpanded.assign(4, false);
    for (int n = 0; n < 4; ++n) m_vUcCmds[n].clear();
    RememberUcSelection(-1, -1);
    return true;
}

// [R-fix] Build() 重入恢复：UI 树已重建但 m_vUcCmds 内容仍在。
// 所有分组都重建 body 内容（ToggleGroup 展开时不会再调 RebuildBody，
// 折叠组若留空则展开时无内容），再按 m_vUcExpanded 设可见性与箭头字形。
void CPageUnattend::RestoreUcBuild()
{
    for (int n = 0; n < 4; ++n)
    {
        CDuiString sBodyName;
        sBodyName.Format(L"ucbody_%d", n);
        CVerticalLayoutUI* pBody = static_cast<CVerticalLayoutUI*>(
            m_ctx.HostFindControl(sBodyName));
        if (!pBody) continue;
        RebuildBody(n);
        const bool bOpen = (n < static_cast<int>(m_vUcExpanded.size()) && m_vUcExpanded[n]);
        pBody->SetVisible(bOpen);
        CDuiString sArName;
        sArName.Format(L"ucarr_%d", n);
        CTextUI* pArrow = static_cast<CTextUI*>(
            m_ctx.HostFindControl(sArName));
        if (pArrow) pArrow->SetText(m_ctx.HostGlyphArrow(bOpen ? 1 : 0));
    }
}

// [REFACTOR] 从 SchemaCheck 提取：从 '<' 之后提取标签名（跳过前导空白，
// 收集到空白/引号/'/'/'>' 为止），转大写后写入 outTag，
// 并把标签名结束位置写回 outNameEnd 供属性解析使用。
// 逐字搬迁零逻辑改动；返回是否成功提取到非空标签名。
[[nodiscard]] bool CPageUnattend::FetchTagName(const wchar_t* pszSrc, int nContentLen, int pOpen,
                                 CDuiString& outTag, int& outNameEnd)
{
    int pNameStart = pOpen + 1;
    while (pNameStart < nContentLen
           && (pszSrc[pNameStart] == L' ' || pszSrc[pNameStart] == L'\t'
               || pszSrc[pNameStart] == L'\r' || pszSrc[pNameStart] == L'\n'))
        ++pNameStart;
    int pNameEnd = pNameStart;
    while (pNameEnd < nContentLen
           && pszSrc[pNameEnd] != L' ' && pszSrc[pNameEnd] != L'"'
           && pszSrc[pNameEnd] != L'/' && pszSrc[pNameEnd] != L'>')
        ++pNameEnd;
    outTag.Assign(pszSrc + pNameStart, pNameEnd - pNameStart);
    outTag.MakeUpper();
    outNameEnd = pNameEnd;
    return (pNameEnd > pNameStart);
}

// [R232] Re-colour the unattend command groups after a light<->dark switch.
//
// PageBase.h contract: colours only -- no Build()/Add/Remove. Rebuilding would
// re-parse the XML and lose the expansion state.
//
// Structure:
//   group header ughdr_<n> : 0 = arrow, 1 = label, 2 = count badge
//   command row  ucrow_<n>_<k> : single text child, colour depends on IsRowSelected()
void CPageUnattend::RefreshThemeColors()
{
    const DWORD dwBase  = tokens::Token(tokens::EToken::Surface);
    const DWORD dwInk   = tokens::Token(tokens::EToken::Ink);
    const DWORD dwMuted = tokens::Token(tokens::EToken::InkMuted);
    const DWORD dwTer   = tokens::Token(tokens::EToken::InkTertiary);
    const DWORD dwSel   = tokens::Token(tokens::EToken::Accent);
    const DWORD dwOnSel = tokens::Token(tokens::EToken::OnAccent);

    CVerticalLayoutUI* pGroups = static_cast<CVerticalLayoutUI*>(
        m_ctx.HostFindControl(L"uc_groups"));
    if (!pGroups) return;

    for (int g = 0; g < pGroups->GetCount(); ++g)
    {
        CControlUI* pHdrCtl = pGroups->GetItemAt(g);
        if (!pHdrCtl) continue;
        pHdrCtl->SetBkColor(dwBase);

        IContainerUI* pHdr = static_cast<IContainerUI*>(
            pHdrCtl->GetInterface(DUI_CTR_CONTAINER));
        if (!pHdr) continue;
        for (int c = 0; c < pHdr->GetCount(); ++c)
        {
            CTextUI* pText = dynamic_cast<CTextUI*>(pHdr->GetItemAt(c));
            if (!pText) continue;
            if (c == 2)      pText->SetTextColor(dwTer);      // count badge
            else             pText->SetTextColor(c == 0 ? dwMuted : dwInk);

            // [R244] 徽章底色同样要跟着主题走。
            //   Build 里两个徽章用固定浅底（原 #EEEBF5 / #EEF2F7，后改令牌），
            //   但刷新只改文字色不改底 —— 切换后就成了"新主题文字 + 旧主题色块"。
            //   按位置区分：c==0 「已通过」徽章用 AccentSoft，c==2 计数角标用 SurfaceAlt。
            if (c == 0)
                pText->SetAttribute(L"bkcolor",
                                    tokens::TokenHex(tokens::EToken::AccentSoft));
            else if (c == 2)
                pText->SetAttribute(L"bkcolor",
                                    tokens::TokenHex(tokens::EToken::SurfaceAlt));
        }

        // [R244] 组头悬停底色：Build 设了 hotbkcolor，刷新漏了它，
        //   于是深色下鼠标移上去会闪出一块 #FFF5F5F5 的亮块。
        pHdrCtl->SetAttribute(L"hotbkcolor",
                              tokens::TokenHex(tokens::EToken::SurfaceHover));
    }

    // Command rows live inside the group bodies (ucbody_<n>_<m>), not directly under
    // uc_groups, so walk one level deeper and pick rows by type.
    for (int i = 0; i < pGroups->GetCount(); ++i)
    {
        IContainerUI* pBody = static_cast<IContainerUI*>(
            pGroups->GetItemAt(i)->GetInterface(DUI_CTR_CONTAINER));
        if (!pBody) continue;
        for (int r = 0; r < pBody->GetCount(); ++r)
        {
            CControlUI* pRowCtl = pBody->GetItemAt(r);
            if (!pRowCtl) continue;
            CClickRowUI* pRowRow = dynamic_cast<CClickRowUI*>(pRowCtl);
            if (!pRowRow) continue;   // header, not a command row
            const bool bSel = pRowRow->IsRowSelected();
            pRowRow->SetBkColor(bSel ? dwSel : dwBase);

            IContainerUI* pRow = static_cast<IContainerUI*>(
                pRowCtl->GetInterface(DUI_CTR_CONTAINER));
            if (!pRow) continue;
            for (int c = 0; c < pRow->GetCount(); ++c)
            {
                CTextUI* pText = dynamic_cast<CTextUI*>(pRow->GetItemAt(c));
                if (pText) pText->SetTextColor(bSel ? dwOnSel : dwInk);
            }
        }
    }
}
