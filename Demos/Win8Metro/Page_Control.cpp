#include "stdafx.h"
#include "ThemeTokens.h"   // [R203] 颜色令牌（深色/浅色跟随）

// ============================================================
// Page_Control.cpp —— 控制面板页（新版 Q11 功能移植）
// 1 个原 CFrameWnd 函数物理搬迁，函数体零逻辑改动，
// 宿主引用改 m_ctx.Xxx。
// [R91] 新增 HKLM NameSpace 动态枚举（ControlService::EnumControlApplets），
//       硬编码 applet 保留为兜底（解析失败时使用）。
// ============================================================

#include "Page_Control.h"
#include "CustomControls.h"
#include "StringManager.h"
#include "ControlService.h"    // [R91] 控制面板动态枚举
#include "FrameWnd_Util.h"     // [R99] FrameWnd_Util_CopyTextToClipboard
#include <shellapi.h>
#include <memory>                // [C++17 R3] make_unique

#pragma comment(lib, "shell32.lib")

using namespace DuiLib;
using i18n::CStringManager;

// [R162] 从 BuildPage 抽出：构建一行"控制面板项"（主文案 + canonical 副文本）。
//   原实现兜底分支与动态分支各有一份约 25 行、逐字相同的行构建代码，
//   只有两个字符串来源不同。两份拷贝的风险很具体：
//   将来给行加控件（状态点/图标）时只改一侧，就会出现"动态列表有、兜底列表没有"
//   的视觉不一致，而用户无法判断这是两条代码路径造成的。
//   控件属性（行高 30 / 白底 / inset 10 / 副文本 140px 灰字 / endellipsis）
//   与两处索引登记逐字保留。
void CPageControl::AppendAppletRow(CVerticalLayoutUI* pList, int nIndex,
                                   const CDuiString& sDisplay, const CDuiString& sCanonical)
{
    auto pRow = std::make_unique<CClickRowUI>(); // [C++17 R3]
    pRow->SetFixedHeight(30);
    pRow->SetBkColor(tokens::Token(tokens::EToken::Surface));
    RECT rcIn = { 10, 0, 10, 0 };
    pRow->SetInset(rcIn);

    CDuiString sRowName;
    sRowName.Format(L"ctrlrow_%d", nIndex);
    pRow->SetName(sRowName.GetData());

    auto pTx = std::make_unique<CTextUI>(); // [C++17 R3]
    pTx->SetText(sDisplay.GetData());
    pTx->SetTextColor(tokens::Token(tokens::EToken::Ink));
    pTx->SetAttribute(L"valign", L"vcenter");
    pTx->SetMouseEnabled(false);
    pRow->Add(pTx.release()); // [C++17 R3] ownership transferred to DuiLib

    // [R111] canonical 副文本（灰色小字，便于辨识真实标识）
    auto pSub = std::make_unique<CTextUI>(); // [C++17 R3]
    pSub->SetText(sCanonical.GetData());
    pSub->SetFixedWidth(140);
    pSub->SetFont(1);
    pSub->SetTextColor(tokens::Token(tokens::EToken::InkMuted));
    pSub->SetAttribute(L"valign", L"vcenter");
    pSub->SetAttribute(L"endellipsis", L"true");
    pSub->SetMouseEnabled(false);
    pRow->Add(pSub.release()); // [C++17 R3] ownership transferred to DuiLib

    pList->Add(pRow.release()); // [C++17 R3] ownership transferred to DuiLib

    m_vCanonical.push_back(sCanonical);
    m_mapCtrlCanon[sDisplay] = sCanonical;
}

void CPageControl::BuildPage()
{
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"ctrl_list"));
    if (!pList) return;
    pList->RemoveAll();

    m_vApplets.clear();
    m_vCanonical.clear();
    m_mapCtrlCanon.clear();

    // 兜底硬编码列表（原 Q11 移植保留）
    // [R106] 每项三列：canonical / 中文 key（zh-CN 表） / 英文 fallback。
    //        渲染时 TryGet 优先中文，未命中时用英文，杜绝 "[ui.cp.xxx]" 占位符。
    const struct { const wchar_t* pszCanon; const wchar_t* pszNameKey; const wchar_t* pszNameEn; } kApplets[] = {
        { L"Microsoft.ProgramsAndFeatures", L"ui.cp.programs",   L"Programs and Features" },
        { L"Microsoft.System",              L"ui.cp.system",     L"System" },
        { L"Microsoft.DeviceManager",       L"ui.cp.device",     L"Device Manager" },
        { L"Microsoft.NetworkAndSharingCenter", L"ui.cp.network",  L"Network and Sharing Center" },
        { L"Microsoft.PowerOptions",        L"ui.cp.power",      L"Power Options" },
        { L"Microsoft.RegionAndLanguage",   L"ui.cp.region",     L"Region and Language" },
        { L"Microsoft.DateAndTime",         L"ui.cp.datetime",   L"Date and Time" },
        { L"Microsoft.WindowsDefender",     L"ui.cp.defender",   L"Windows Defender" },
        { L"Microsoft.AdministrativeTools", L"ui.cp.admin",      L"Administrative Tools" },
        { L"Microsoft.UserAccounts",        L"ui.cp.user",       L"User Accounts" },
        // [R116-2] 补齐管理工具分类（组策略编辑器）与系统和安全分类（防火墙/恢复/远程桌面）及网络/外观常用项
        { L"Microsoft.EditGroupPolicy",     L"ui.cp.gpedit",     L"Edit Group Policy" },
        { L"Microsoft.WindowsFirewall",     L"ui.cp.firewall",   L"Windows Defender Firewall" },
        { L"Microsoft.InternetOptions",     L"ui.cp.internet",   L"Internet Options" },
        { L"Microsoft.FolderOptions",       L"ui.cp.folder",     L"Folder Options" },
        { L"Microsoft.Recovery",            L"ui.cp.recovery",   L"Recovery" },
        { L"Microsoft.RemoteDesktop",       L"ui.cp.remote",     L"Remote Desktop" },
    };

    // [R106] 取兜底显示名：zh-CN 有译文用译文，否则英文 fallback
    auto ResolveFallbackName = [](const wchar_t* pszKey, const wchar_t* pszEn, CDuiString& sOut)
    {
        sOut.Empty();
        if (pszKey && CStringManager::GetInstance().TryGet(pszKey, sOut) && !sOut.IsEmpty())
            return;
        sOut = pszEn ? pszEn : L"";
    };

    int nCount = 0;

    // [R91] 动态枚举 HKLM NameSpace
    EControlEnumResult nRc = ControlService::EnumControlApplets(m_vApplets);
    bool bDynOk = (nRc == EControlEnumResult::CTRL_ENUM_OK) && !m_vApplets.empty();

    // [R103] 枚举失败分级日志（D4 错误处理增强）
    if (!bDynOk)
    {
        CDuiString sEnumErr;
        switch (nRc)
        {
        case EControlEnumResult::CTRL_ENUM_NO_KEY:
            sEnumErr = StringMgr.Get("ctrl.s_enum_nokey");
            break;
        case EControlEnumResult::CTRL_ENUM_REG_FAIL:
            sEnumErr = StringMgr.Get("ctrl.s_enum_regfail");
            break;
        case EControlEnumResult::CTRL_ENUM_PARSE_FAIL:
            sEnumErr = StringMgr.Get("ctrl.s_enum_parsefail");
            break;
        default:
            sEnumErr = StringMgr.Get("ctrl.s_enum_fail");
            break;
        }
        CDuiString sErrLog;
        sErrLog.Format(L"[ctrl] dynamic enum failed rc=%d (%s), fallback to hardcoded %d items",
                       nRc, sEnumErr.GetData(), static_cast<int>(_countof(kApplets)));
        m_ctx.HostLog(sErrLog.GetData());

        // [R103] 用户可见分级提示（动态枚举失败但仍有硬编码兜底，非致命）
        if (nRc == EControlEnumResult::CTRL_ENUM_PARSE_FAIL)
            m_ctx.HostToast(StringMgr.Get("ctrl.s_enum_short"),
                            StringMgr.Get("ctrl.s_enum_parsefail"), 1);
    }

    if (!bDynOk)
    {
        // 兜底：硬编码 _countof(kApplets) 项
        for (int n = 0; n < static_cast<int>(_countof(kApplets)); ++n)
        {
            CDuiString sFallbackName;
            ResolveFallbackName(kApplets[n].pszNameKey, kApplets[n].pszNameEn, sFallbackName);
            // [R162] 行构建与动态分支共用 AppendAppletRow（原两份逐字相同）
            AppendAppletRow(pList, n, sFallbackName, CDuiString(kApplets[n].pszCanon));
            ++nCount;
        }
    }
    else
    {
        for (size_t n = 0; n < m_vApplets.size(); ++n)
        {
            // [R162] 同上；键来源保持各自的既有语义（兜底用 fallback 名，动态用 display name）
            AppendAppletRow(pList, static_cast<int>(n), m_vApplets[n].sDisplayName,
                            m_vApplets[n].sCanonical);
            ++nCount;
        }
    }

    CDuiString sLog;
    sLog.Format(L"[ctrl] control panel page built: %d applets (dynamic=%d rc=%d)",
                nCount, static_cast<int>(m_vApplets.size()), nRc);
    m_ctx.HostLog(sLog.GetData());
}

void CPageControl::OnEvent(TNotifyUI& msg)
{
    if (msg.sType == L"click")
    {
        const wchar_t* pszName = msg.pSender->GetName();

        // [R99] 刷新按钮
        if (lstrcmpW(pszName, L"btn_ctrl_refresh") == 0)
        {
            BuildPage();
            m_ctx.HostLog(StringMgr.Get("ctrl.s_refreshed"));
            return;
        }

        // [R99] 打开按钮：打开首个可见行
        if (lstrcmpW(pszName, L"btn_ctrl_open") == 0)
        {
            OpenFirstVisible();
            return;
        }

        if (_wcsnicmp(pszName, L"ctrlrow_", 8) != 0) return;

        int nRow = _ttoi(pszName + 8);
        if (nRow < 0 || static_cast<size_t>(nRow) >= m_vCanonical.size()) return;

        // [R91] 统一走 m_vCanonical（动态枚举或硬编码兜底）
        DWORD dwErr = ERROR_SUCCESS;
        EControlOpenResult nRc = ControlService::OpenControlApplet(m_ctx.HostHwnd(),
                                                    m_vCanonical[nRow].GetData(),
                                                    &dwErr);
        CDuiString sOpenLog;
        sOpenLog.Format(L"[ctrl] opened %s rc=%d err=%u (0x%08X)",
                        m_vCanonical[nRow].GetData(), static_cast<int>(nRc),
                        (unsigned)dwErr, (unsigned)dwErr);
        m_ctx.HostLog(sOpenLog.GetData());

        // [R97/R103] 打开失败分级反馈（[R127-2] 按 DiagnoseOpenFailure 分档）
        if (nRc != EControlOpenResult::CTRL_OPEN_OK)
        {
            EControlOpenDiag nDiag = ControlService::DiagnoseOpenFailure(nRc, dwErr);
            CDuiString sTip;
            if (nDiag == EControlOpenDiag::CTRL_DIAG_ACCESSDENIED)
                sTip.Format(StringMgr.Get("ctrl.s_open_denied"),
                            m_vCanonical[nRow].GetData());
            else if (nDiag == EControlOpenDiag::CTRL_DIAG_NOASSOC)
                sTip.Format(StringMgr.Get("ctrl.s_open_noassoc"),
                            m_vCanonical[nRow].GetData());
            else if (nDiag == EControlOpenDiag::CTRL_DIAG_CANCELED)
                sTip.Format(StringMgr.Get("ctrl.s_open_canceled"),
                            m_vCanonical[nRow].GetData());
            else if (nRc == EControlOpenResult::CTRL_OPEN_BAD_CANON)
                sTip.Format(StringMgr.Get("ctrl.s_open_badcanon"),
                            m_vCanonical[nRow].GetData());
            else
                sTip.Format(StringMgr.Get("ctrl.s_open_shellex_fail"),
                            m_vCanonical[nRow].GetData());
            m_ctx.HostToast(StringMgr.Get("ctrl.s_searchtip"), sTip.GetData(), 1);
        }
        return;
    }

    // [R97] 控制面板项名称实时过滤
    if (msg.sType == L"textchanged")
    {
        const wchar_t* pszName = msg.pSender->GetName();
        if (pszName && lstrcmpW(pszName, L"ctrl_filter") == 0)
            ApplyCtrlFilter();
    }
}

// ============================================================
// [R99] 打开首个可见行（btn_ctrl_open 使用）
// 过滤后用户点击"打开"按钮，打开列表第一个可见 ctrlrow_。
// ============================================================
void CPageControl::OpenFirstVisible()
{
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"ctrl_list"));
    if (!pList) return;

    int nCount = pList->GetCount();
    for (int i = 0; i < nCount; ++i)
    {
        CControlUI* pRow = pList->GetItemAt(i);
        if (!pRow) continue;
        const wchar_t* pszN = pRow->GetName();
        if (!pszN || wcsncmp(pszN, L"ctrlrow_", 8) != 0) continue;
        if (!pRow->IsVisible()) continue;

        int nRow = _ttoi(pszN + 8);
        if (nRow < 0 || static_cast<size_t>(nRow) >= m_vCanonical.size()) continue;

        DWORD dwErr = ERROR_SUCCESS;
        EControlOpenResult nRc = ControlService::OpenControlApplet(m_ctx.HostHwnd(),
                                                    m_vCanonical[nRow].GetData(),
                                                    &dwErr);
        CDuiString sLog;
        sLog.Format(L"[ctrl] open btn -> %s rc=%d err=%u (0x%08X)",
                    m_vCanonical[nRow].GetData(), static_cast<int>(nRc),
                    (unsigned)dwErr, (unsigned)dwErr);
        m_ctx.HostLog(sLog.GetData());
        if (nRc != EControlOpenResult::CTRL_OPEN_OK)
        {
            EControlOpenDiag nDiag = ControlService::DiagnoseOpenFailure(nRc, dwErr);
            CDuiString sTip;
            if (nDiag == EControlOpenDiag::CTRL_DIAG_ACCESSDENIED)
                sTip.Format(StringMgr.Get("ctrl.s_open_denied"),
                            m_vCanonical[nRow].GetData());
            else if (nDiag == EControlOpenDiag::CTRL_DIAG_NOASSOC)
                sTip.Format(StringMgr.Get("ctrl.s_open_noassoc"),
                            m_vCanonical[nRow].GetData());
            else if (nDiag == EControlOpenDiag::CTRL_DIAG_CANCELED)
                sTip.Format(StringMgr.Get("ctrl.s_open_canceled"),
                            m_vCanonical[nRow].GetData());
            else if (nRc == EControlOpenResult::CTRL_OPEN_BAD_CANON)
                sTip.Format(StringMgr.Get("ctrl.s_open_badcanon"),
                            m_vCanonical[nRow].GetData());
            else
                sTip.Format(StringMgr.Get("ctrl.s_open_fail"),
                            m_vCanonical[nRow].GetData());
            m_ctx.HostToast(StringMgr.Get("ctrl.s_searchtip"), sTip.GetData(), 1);
        }
        return;
    }

    // 无可见行：提示
    m_ctx.HostToast(StringMgr.Get("ctrl.s_searchtip"), StringMgr.Get("ctrl.s_none_visible"), 0);
}

// ============================================================
// [R97] 控制面板项名称实时过滤（不区分大小写）
// 匹配 sDisplayName；过滤后行号仍指向 m_vCanonical 原始索引
// （隐藏而非重建，保证 ctrlrow_N → canonical 对齐）。
// [R109] 扩展：显示名未命中时再用 canonical 名匹配
// （如输入 "programs" / "device" 等 canonical 关键字也能命中），
// 无结果提示同时说明两种匹配口径。
// ============================================================

// [R164] 从 ApplyCtrlFilter 抽出：第一口径 —— 行内任一子控件文本包含关键字。
//   注意取的是"子控件文本"而不是子控件名：行内既有名称文本也有 canonical 副文本，
//   两者都可能命中，这与用户预期一致（看到什么就能搜什么）。
[[nodiscard]] bool CPageControl::ClRowHitByName(CControlUI* pRow, const CDuiString& sKwLow)
{
    CContainerUI* pC = dynamic_cast<CContainerUI*>(pRow);
    if (!pC) return false;
    for (int j = 0; j < pC->GetCount(); ++j)
    {
        CControlUI* pSub = pC->GetItemAt(j);
        if (!pSub) continue;
        CDuiString sT = pSub->GetText();
        sT.MakeLower();
        if (sT.Find(sKwLow.GetData()) != -1) return true;
    }
    return false;
}

// [R164] 从 ApplyCtrlFilter 抽出：第二口径 —— canonical 名匹配（显示名未命中时补位）。
//   行名格式 ctrlrow_%d，故从 +8 处解析索引；越界返回 false 而不是断言，
//   因为列表可能正在重建（与 m_vCanonical 短暂不同步）。
[[nodiscard]] bool CPageControl::ClRowHitByCanon(const wchar_t* pszRowName,
                                                 const CDuiString& sKwLow)
{
    if (!pszRowName) return false;
    const int nRow = _ttoi(pszRowName + 8);
    if (nRow < 0 || static_cast<size_t>(nRow) >= m_vCanonical.size()) return false;
    CDuiString sCanon = m_vCanonical[nRow];
    sCanon.MakeLower();
    return sCanon.Find(sKwLow.GetData()) != -1;
}

void CPageControl::ApplyCtrlFilter()
{
    CDuiString sKw;
    if (CEditUI* pEdit = static_cast<CEditUI*>(m_ctx.HostFindControl(L"ctrl_filter")))
        sKw = pEdit->GetText();
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"ctrl_list"));
    if (!pList) return;
    CDuiString sKwLow = sKw;
    sKwLow.MakeLower();

    int nShown = 0;
    int nByName = 0;   // [R109] 仅显示名命中
    int nByCanon = 0;  // [R109] 仅 canonical 命中（或双命中）
    int nCount = pList->GetCount();
    for (int i = 0; i < nCount; ++i)
    {
        CControlUI* pRow = pList->GetItemAt(i);
        if (!pRow) continue;
        const wchar_t* pszN = pRow->GetName();
        if (!pszN || wcsncmp(pszN, L"ctrlrow_", 8) != 0) continue;

        if (sKwLow.IsEmpty()) { pRow->SetVisible(true); ++nShown; continue; }

        // [R109] 第一口径：行内显示名匹配
        // [R164] 两段匹配抽为 ClRowHitByName / ClRowHitByCanon，嵌套 4→2
        const bool bHitByName = ClRowHitByName(pRow, sKwLow);
        // [R109] 第二口径：canonical 名称匹配（显示名未命中时补位）
        const bool bHitByCanon = !bHitByName && ClRowHitByCanon(pszN, sKwLow);

        bool bHit = bHitByName || bHitByCanon;
        pRow->SetVisible(bHit);
        if (bHit)
        {
            ++nShown;
            if (bHitByName) ++nByName;
            else if (bHitByCanon) ++nByCanon;
        }
    }

    // [R109] 过滤无结果提示：区分两种口径均无结果
    if (!sKwLow.IsEmpty() && nShown == 0)
    {
        m_ctx.HostToast(StringMgr.Get("ctrl.s_filter_noresult"), L"", 0);
    }
    else if (!sKwLow.IsEmpty() && nByName == 0)
    {
        // 仅 canonical 命中：提示当前匹配口径，便于用户理解
        CDuiString sTip;
        sTip.Format(StringMgr.Get("ctrl.s_filter_canononly"), nByCanon, nShown);
        m_ctx.HostToast(StringMgr.Get("ctrl.s_searchtip"), sTip.GetData(), 0);
    }

    // [R109] 过滤后更新底部可见行计数（复用 btn_ctrl_open 提示口径）
    CDuiString sStat;
    sStat.Format(L"[ctrl] filter='%s' shown=%d/%d (name=%d canon=%d)",
                 sKwLow.GetData(), nShown, nCount, nByName, nByCanon);
    m_ctx.HostLog(sStat.GetData());
}

// ============================================================
// [R99] 控制面板页右键菜单（ctrlrow_ 命中）：
//   打开控制面板项 / 复制 canonical / 复制显示名
// 破坏性动作无，直接打开无二次确认。
// ============================================================
bool CPageControl::OnContextMenu(POINT pt, bool bFromContextMsg)
{
    if (bFromContextMsg) return false;

    CControlUI* pHit = m_ctx.HostFindControlAt(pt);
    for (CControlUI* p = pHit; p; p = p->GetParent())
    {
        const wchar_t* psz = p->GetName();
        if (!psz) continue;
        if (wcsncmp(psz, L"ctrlrow_", 8) != 0) continue;

        int nRow = _ttoi(psz + 8);
        if (nRow < 0 || static_cast<size_t>(nRow) >= m_vCanonical.size()) return false;

        ShowCtrlContextMenu(nRow, pt);
        return true;
    }
    return false;
}

// [R164] 从 ShowCtrlContextMenu 抽出：读取指定行的显示名。
//   规则：取行容器（CContainerUI）第一个子控件的文本 —— 动态枚举与硬编码兜底
//   两种数据源都按同一布局构建，所以这条规则通用。
//   无列表 / 行号越界 / 不是容器 / 无子项 → 返回空串，调用方按"无显示名"处理。
//   这里必须用 dynamic_cast（要取 CContainerUI 的子项接口），
//   不能像别处改用 class 名比较 —— 那会改变行为。
[[nodiscard]] CDuiString CPageControl::CtrlRowDisplayText(int nRow)
{
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"ctrl_list"));
    if (!pList || nRow >= pList->GetCount()) return CDuiString();
    CControlUI* pRow = pList->GetItemAt(nRow);
    CContainerUI* pC = dynamic_cast<CContainerUI*>(pRow);
    if (!pC || pC->GetCount() <= 0) return CDuiString();
    CControlUI* pSub = pC->GetItemAt(0);
    return pSub ? pSub->GetText() : CDuiString();
}

// [R164] 从 ShowCtrlContextMenu 抽出：打开控制面板项（普通/以管理员身份）。
//   [R127-2] 失败按诊断分级弹差异化文案，不再笼统一个"失败"——
//   用户看到"访问被拒绝"能自己去提权，看到"没有关联程序"会去装程序，
//   两种处置方式不同，笼统提示等于没提示。
void CPageControl::CtrlOpenApplet(bool bRunAs, const CDuiString& sCanon)
{
    DWORD dwErr = ERROR_SUCCESS;
    const EControlOpenResult nRc = bRunAs
        ? ControlService::OpenControlAppletElevated(m_ctx.HostHwnd(), sCanon.GetData(), &dwErr)
        : ControlService::OpenControlApplet(m_ctx.HostHwnd(), sCanon.GetData(), &dwErr);

    CDuiString sLog;
    sLog.Format(L"[ctrl] menu %s %s rc=%d err=%u (0x%08X)",
                bRunAs ? L"runas" : L"open",
                sCanon.GetData(), static_cast<int>(nRc), (unsigned)dwErr, (unsigned)dwErr);
    m_ctx.HostLog(sLog.GetData());
    if (nRc == EControlOpenResult::CTRL_OPEN_OK) return;

    const EControlOpenDiag nDiag = ControlService::DiagnoseOpenFailure(nRc, dwErr);
    CDuiString sTip;
    switch (nDiag)
    {
    case EControlOpenDiag::CTRL_DIAG_ACCESSDENIED:
        sTip.Format(StringMgr.Get("ctrl.s_open_denied"), sCanon.GetData());
        break;
    case EControlOpenDiag::CTRL_DIAG_NOASSOC:
        sTip.Format(StringMgr.Get("ctrl.s_open_noassoc"), sCanon.GetData());
        break;
    case EControlOpenDiag::CTRL_DIAG_CANCELED:
        sTip.Format(StringMgr.Get("ctrl.s_open_canceled"), sCanon.GetData());
        break;
    default:
        // [R172] 恢复基线行为：诊断落到 default 时仍按 nRc 二级细分。
        //   原实现里 CTRL_OPEN_ELEVATE_FAIL 走 ctrl.s_runas_fail ——
        //   这条提示告诉用户"提权失败"，是可操作信息；笼统的失败文案会让人
        //   以为程序坏了。BAD_CANON 同理。
        if (nRc == EControlOpenResult::CTRL_OPEN_BAD_CANON)
            sTip.Format(StringMgr.Get("ctrl.s_open_badcanon"), sCanon.GetData());
        else if (nRc == EControlOpenResult::CTRL_OPEN_ELEVATE_FAIL)
            sTip.Format(StringMgr.Get("ctrl.s_runas_fail"), sCanon.GetData());
        else
            sTip.Format(StringMgr.Get("ctrl.s_open_fail"), sCanon.GetData());
        break;
    }
    m_ctx.HostToast(StringMgr.Get("ctrl.s_searchtip"), sTip.GetData(), 1);
}

// [R164] 从 ShowCtrlContextMenu 抽出：复制某个字段到剪贴板并提示。
//   两个复制项同构（复制 canonical / 复制显示名），只有取值与提示文案不同。
// [R172] 恢复基线语义：标题统一 ctrl.s_searchtip，正文是"已复制什么"，
//   且复制显示名时先判空（基线行为：空显示名不复制、不提示）。
//   参数 bRequireNonEmpty 表达"该字段必须非空才复制"，而不是把判空写死在调用方。
void CPageControl::CtrlCopyField(const CDuiString& sValue, const char* pszCopiedKey,
                                 bool bRequireNonEmpty)
{
    if (bRequireNonEmpty && sValue.IsEmpty()) return;
    FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), sValue.GetData());
    m_ctx.HostToast(StringMgr.Get("ctrl.s_searchtip"),
                    StringMgr.Get(pszCopiedKey), 0);
}

void CPageControl::ShowCtrlContextMenu(int nRow, POINT pt)
{
    const CDuiString& sCanon = m_vCanonical[nRow];
    // [R164] 显示名读取抽为 CtrlRowDisplayText，嵌套 5→4
    const CDuiString sDisp = CtrlRowDisplayText(nRow);

    enum { ID_OPEN = 6101, ID_COPY_CANON = 6102, ID_COPY_NAME = 6103,
       ID_OPEN_RUNAS = 6104 };   // [R127-2] 以管理员身份打开

    HMENU hMenu = ::CreatePopupMenu();
    if (!hMenu) return;

    ::AppendMenu(hMenu, MF_STRING, ID_OPEN, StringMgr.Get("ctrl.s_menu_open"));
    ::AppendMenu(hMenu, MF_STRING, ID_OPEN_RUNAS, StringMgr.Get("ctrl.s_menu_runas"));
    ::AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenu(hMenu, MF_STRING, ID_COPY_CANON, StringMgr.Get("ctrl.s_menu_copy_canon"));
    ::AppendMenu(hMenu, MF_STRING, ID_COPY_NAME, StringMgr.Get("ctrl.s_menu_copy_name"));

    int iCmd = ::TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                pt.x, pt.y, 0, m_ctx.HostHwnd(), nullptr);
    ::DestroyMenu(hMenu);
    if (iCmd == 0) return;

    // [R164] 分支主体抽为 CtrlOpenApplet / CtrlCopyField；嵌套 5→2
    switch (iCmd)
    {
    case ID_OPEN:
    case ID_OPEN_RUNAS:
        CtrlOpenApplet(iCmd == ID_OPEN_RUNAS, sCanon);
        break;
    case ID_COPY_CANON:
        CtrlCopyField(sCanon, "ctrl.s_copied_canon", false);
        break;
    case ID_COPY_NAME:
        CtrlCopyField(sDisp, "ctrl.s_copied_name", true);
        break;
    default:
        break;
    }
}

// [C++17 R3] 文件级内联常量（真实 C++17 inline variables）：
// 本页处理批次的超时/重试上限，供本文件内部使用；inline 保证多 TU 下 ODR 一致。
inline constexpr int kCtrlStepTimeoutMs = 120000;
inline constexpr int kCtrlMaxRetry = 3;

// [R232] Re-colour the control-panel rows after a light<->dark switch.
//
// PageBase.h contract: colours only -- no Build()/Add/Remove.
//
// Row layout (BuildPage): 0 = canonical name text (Ink), 1 = localised sub-label
// (InkMuted). The list comes from a hard-coded fallback array, so it is never empty
// and there is no empty-state branch to worry about here.
void CPageControl::RefreshThemeColors()
{
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(
        m_ctx.HostFindControl(L"ctrl_list"));
    if (!pList) return;

    for (int i = 0; i < pList->GetCount(); ++i)
    {
        CControlUI* pRowCtl = pList->GetItemAt(i);
        if (!pRowCtl) continue;
        if (!dynamic_cast<CClickRowUI*>(pRowCtl)) continue;

        pRowCtl->SetBkColor(tokens::Token(tokens::EToken::Surface));

        IContainerUI* pRow = static_cast<IContainerUI*>(
            pRowCtl->GetInterface(DUI_CTR_CONTAINER));
        if (!pRow) continue;
        for (int c = 0; c < pRow->GetCount(); ++c)
        {
            CTextUI* pText = dynamic_cast<CTextUI*>(pRow->GetItemAt(c));
            if (!pText) continue;
            pText->SetTextColor(c == 0 ? tokens::Token(tokens::EToken::Ink)
                                       : tokens::Token(tokens::EToken::InkMuted));
        }
    }
}
