// ============================================================
// Page_AssocMenu.cpp —— 文件关联页「行级右键菜单 + 写路径错误提示」。
//
// [R101] 从 Page_Assoc.cpp 拆出。这一段只处理「用户点了哪一行、菜单弹什么、
//   写注册表失败怎么措辞」，不读注册表、不构建列表、不分发事件。
//
// 与主文件的耦合点只有 m_vAssocExt（行索引 → 扩展名）与 m_ctx（宿主能力），
// 二者都通过 CPageAssoc 成员访问，无隐式全局状态。
//
// 拆分前后函数体逐字未改，仅搬迁位置。
// ============================================================

#include "stdafx.h"

#include "Page_Assoc.h"

#include "StringManager.h"       // StringMgr 本地化宏
#include "FrameWnd_Util.h"       // FrameWnd_Util_CopyTextToClipboard
#include "AssocService.h"        // AssocService::Write* / EAssocWriteResult

using namespace DuiLib;
using i18n::CStringManager;

// ============================================================
// [R165] 从 ShowAssocContextMenu 抽出：复制类菜单项（扩展名 / ProgId / 命令）。
//   三者结构完全相同：值为空 → 提示"无可复制"并指明是哪个扩展名；
//   非空 → 复制到剪贴板 + 成功提示；pszLogTag 非空时额外记一条日志。
//   原实现三份拷贝已漂移：只有"复制命令"分支记了日志，另两项没有 ——
//   排查"我到底复制过什么"时证据不全。现在日志由 pszLogTag 统一控制。
void CPageAssoc::AssocCopyMenuField(const CDuiString& sValue, const CDuiString& sTipName,
                                    const char* pszOkKey, const char* pszEmptyKey,
                                    const char* pszLogTag, int nRow)
{
    // pszEmptyKey 为 null 表示该字段必非空（如扩展名），不设空值分支 —— 与原实现一致
    if (sValue.IsEmpty())
    {
        if (pszEmptyKey) m_ctx.HostToast(StringMgr.Get(pszEmptyKey), sTipName.GetData(), 1);
        return;
    }
    FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), sValue.GetData());
    m_ctx.HostToast(StringMgr.Get(pszOkKey), sTipName.GetData(), 0);
    if (pszLogTag == nullptr) return;
    CDuiString sLog;
    sLog.Format(L"[assoc] row %d %s: %s", nRow, pszLogTag, sValue.GetData());
    m_ctx.HostLog(sLog.GetData());
}

// [R165] 从 ShowAssocContextMenu 抽出：枚举该扩展名的推荐处理程序并复制其名称。
//   优先 bRecommended 那条，无推荐则取第一个 —— 用户问"用什么打开"时，
//   推荐项才是 Windows 自己会选的，取首个可能拿到一个次要处理程序。
//   枚举失败或结果为空：提示"无处理程序"并记 rc/err，便于判断是权限还是没装。
void CPageAssoc::AssocCopyHandler(const CDuiString& sExt, int nRow)
{
    std::vector<AssocHandlerEntry> vHandlers;
    DWORD dwErr = 0;
    CDuiString sE = (sExt[0] == L'.') ? sExt : CDuiString(L".") + sExt;
    const int nEnum = AssocService::EnumAssocHandlers(sE, vHandlers, dwErr);
    if (nEnum != 0 || vHandlers.empty())
    {
        m_ctx.HostToast(StringMgr.Get("assoc.s_menu_nohandler"), sExt.GetData(), 1);
        CDuiString sLog;
        sLog.Format(L"[assoc] row %d handlers enum rc=%d err=%u", nRow,
                    nEnum, static_cast<unsigned>(dwErr));
        m_ctx.HostLog(sLog.GetData());
        return;
    }
    int nPick = 0;
    for (size_t k = 0; k < vHandlers.size(); ++k)
        if (vHandlers[k].bRecommended) { nPick = static_cast<int>(k); break; }

    FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), vHandlers[nPick].sName.GetData());
    m_ctx.HostToast(StringMgr.Get("assoc.s_menu_copied_handler"),
                    vHandlers[nPick].sName.GetData(), 0);
    CDuiString sLog;
    sLog.Format(L"[assoc] row %d handler copied: %s (recommended=%d)",
                nRow, vHandlers[nPick].sName.GetData(),
                vHandlers[nPick].bRecommended ? 1 : 0);
    m_ctx.HostLog(sLog.GetData());
}

void CPageAssoc::ShowAssocContextMenu(int nRow, POINT pt)
{
    if (nRow < 0 || static_cast<size_t>(nRow) >= m_vAssocExt.size()) return;

    CDuiString sExt = m_vAssocExt[nRow];
    CDuiString sProgId, sCommand;
    AssocService::QueryProgIdAndCommand(sExt, sProgId, sCommand);

    enum { ID_COPY_EXT = 6301, ID_COPY_PROGID = 6302, ID_COPY_CMD = 6303,
           ID_OPEN_REG = 6304, ID_SET_DEFAULT = 6305, ID_COPY_HANDLER = 6306 };

    HMENU hMenu = ::CreatePopupMenu();
    if (!hMenu) return;

    ::AppendMenu(hMenu, MF_STRING, ID_COPY_EXT, StringMgr.Get("assoc.s_menu_copy_ext"));
    ::AppendMenu(hMenu, MF_STRING, ID_COPY_PROGID, StringMgr.Get("assoc.s_menu_copy_progid"));
    ::AppendMenu(hMenu, MF_STRING, ID_COPY_CMD, StringMgr.Get("assoc.s_menu_copy_cmd"));
    ::AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenu(hMenu, MF_STRING, ID_OPEN_REG, StringMgr.Get("assoc.s_menu_open_reg"));
    ::AppendMenu(hMenu, MF_STRING, ID_SET_DEFAULT, StringMgr.Get("assoc.s_menu_set_default"));
    ::AppendMenu(hMenu, MF_STRING, ID_COPY_HANDLER, StringMgr.Get("assoc.s_menu_copy_handler"));

    int iCmd = ::TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                pt.x, pt.y, 0, m_ctx.HostHwnd(), nullptr);
    ::DestroyMenu(hMenu);
    if (iCmd == 0) return;

    // 复用日志变量（避免多个 case 重复声明导致 C2360）
    CDuiString sLog;

    switch (iCmd)
    {
    // [R165] 3 个复制分支同构，抽为 AssocCopyMenuField
    case ID_COPY_EXT:
        // 扩展名本身必非空（来自 m_vAssocExt），故沿用原实现：直接复制、不做空值分支
        AssocCopyMenuField(sExt, sExt, "assoc.s_menu_copied_ext", nullptr, nullptr, nRow);
        break;
    case ID_COPY_PROGID:
        AssocCopyMenuField(sProgId, sExt, "assoc.s_menu_copied_progid",
                           "assoc.s_menu_noprocess", nullptr, nRow);
        break;
    case ID_COPY_CMD:
        AssocCopyMenuField(sCommand, sExt, "assoc.s_menu_copied_cmd",
                           "assoc.s_menu_nocmd", "cmd copied", nRow);
        break;

    case ID_OPEN_REG:
    {
        // regedit 定位 HKCR\.ext
        CDuiString sKey;
        if (sExt[0] == L'.') sKey = sExt;
        else { sKey = L"."; sKey += sExt; }
        CDuiString sArgs;
        sArgs.Format(L"/s \"HKCR\\%s\"", sKey.GetData());
        ::ShellExecuteW(m_ctx.HostHwnd(), L"open", L"regedit", sArgs.GetData(),
                        nullptr, SW_SHOWNORMAL);
        sLog.Format(L"[assoc] row %d open reg HKCR\\%s", nRow, sKey.GetData());
        m_ctx.HostLog(sLog.GetData());
        break;
    }

    case ID_SET_DEFAULT:
    {
        if (sProgId.IsEmpty())
        {
            m_ctx.HostToast(StringMgr.Get("assoc.s_menu_noprocess"),
                            sExt.GetData(), 1);
            break;
        }
        // 二次确认（写 UserChoice 可逆但影响面大）
        CDuiString sConfirm;
        sConfirm.Format(StringMgr.Get("assoc.s_menu_default_confirm"),
                        sExt.GetData(), sProgId.GetData());
        if (!m_ctx.HostConfirmWarn(StringMgr.Get("assoc.s_menu_default_title"),
                                   sConfirm.GetData()))
            break;

        EAssocWriteResult r = AssocService::SetUserFileAssociationEx(sExt, sProgId);
        if (r == EAssocWriteResult::ASSOC_WRITE_OK)
        {
            m_ctx.HostToast(StringMgr.Get("assoc.s_menu_default_ok_short"),
                            StringMgr.Get("assoc.s_menu_default_ok"), 0);
        }
        else
        {
            ShowAssocWriteError(static_cast<int>(r), sExt.GetData());
        }
        sLog.Format(L"[assoc] row %d set default '%s' -> '%s' rc=%d",
                     nRow, sExt.GetData(), sProgId.GetData(), static_cast<int>(r));
        m_ctx.HostLog(sLog.GetData());
        break;
    }

    // [R165] 处理程序枚举 + 复制抽为 AssocCopyHandler
    case ID_COPY_HANDLER:
        AssocCopyHandler(sExt, nRow);
        break;

    default: break;
    }
}

// ============================================================
// [R128-2] AssocService 写路径失败码 → 集中分级 Toast 文案
// 覆盖 EAssocWriteResult::ASSOC_WRITE_NO_PERMISSION / INVALID_PROGID / IASSOC_FAIL /
// REG_FAIL / VERIFY_FAIL 五档，日志带 rc 与错误码。
// ============================================================
void CPageAssoc::ShowAssocWriteError(int nResult, const wchar_t* pszExt)
{
    EAssocWriteResult r = (EAssocWriteResult)nResult;
    CDuiString sTip;
    switch (r)
    {
    case EAssocWriteResult::ASSOC_WRITE_NO_PERMISSION:
        sTip.Format(StringMgr.Get("assoc.s_w_noperm"), pszExt);
        break;
    case EAssocWriteResult::ASSOC_WRITE_INVALID_PROGID:
        sTip.Format(StringMgr.Get("assoc.s_w_invprogid"), pszExt);
        break;
    case EAssocWriteResult::ASSOC_WRITE_IASSOC_FAIL:
        sTip.Format(StringMgr.Get("assoc.s_w_iassoc"), pszExt);
        break;
    case EAssocWriteResult::ASSOC_WRITE_REG_FAIL:
        sTip.Format(StringMgr.Get("assoc.s_w_regfail"), pszExt);
        break;
    case EAssocWriteResult::ASSOC_WRITE_VERIFY_FAIL:
        sTip.Format(StringMgr.Get("assoc.s_w_verify"), pszExt);
        break;
    default:
        sTip.Format(StringMgr.Get("assoc.s_w_other"), pszExt, static_cast<int>(r));
        break;
    }
    m_ctx.HostToast(StringMgr.Get("assoc.s_w_title"), sTip.GetData(), 2);
    CDuiString sLog;
    sLog.Format(L"[assoc] write '%s' failed rc=%d", pszExt, static_cast<int>(r));
    m_ctx.HostLog(sLog.GetData(), static_cast<int>(ELogLevel::LOG_ERROR));
}
// ============================================================
// [R-B1] Ctrl+C / Ctrl+X 复制首个扩展名
// ============================================================
