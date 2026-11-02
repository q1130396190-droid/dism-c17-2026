#include "stdafx.h"
#include "ThemeTokens.h"   // [R203] 颜色令牌（深色/浅色跟随）

// ============================================================
// Page_Assoc.cpp —— v2 P2c 文件关联页（content_area 页索引 9）
// 6 个原 CFrameWnd 关联页函数的物理搬迁目标，函数体零逻辑改动，
// 仅把宿主引用从隐式 CFrameWnd 成员改为 m_ctx.Xxx（
// FindControl/Hwnd/Log/HostConfirmWarn/HostAlert）。
// UserChoice 写操作与 dism 关联命令在 AssocService（S8）；
// RegGetString 在 Registry 基础层；图标缓存 helper 在 FrameWnd_Util。
// ============================================================

#include "Page_Assoc.h"


#include "CustomControls.h"      // CClickRowUI（assocrow_%d 行）
#include "StringManager.h"       // StringMgr 本地化宏
#include "FrameWnd_Util.h"       // EnsureIconCacheDirectory/EnsureIconPngReady/剪贴板
#include "FrameWnd_UiHelpers.h" // P8：统一列表右键菜单
#include "FrameWnd_ErrorMap.h"   // ErrorMap::LogError/GetFriendlyMessage
#include "Win32DialogService.h"  // DlgOpenFile/DlgSaveFile/DlgBrowseFolder
#include "Registry.h"            // RegGetString
#include "AssocService.h"        // S8：QueryProgIdAndCommand/RunDismAssocArgs

#include <algorithm>             // std::sort/std::unique（Build 扩展名去重排序）
#include <memory>                // [C++17 R3] make_unique/unique_ptr
// [C++17 阶段2] 局部 RAII 守卫：注册表键（见 WinHandles.h 使用规范）
struct ASRegGuard
{
    HKEY k = nullptr;
    ~ASRegGuard() { if (k) ::RegCloseKey(k); }
    HKEY* out() { return &k; }
    operator HKEY() const { return k; }
};


using namespace DuiLib;
using i18n::CStringManager;      // 便捷：直接写 CStringManager 不用前缀


// 原 CFrameWnd::QueryAssociation：注册表只读部分下沉 AssocService，
// 结果写回 assoc_progid / assoc_cmd 两个控件（空态走 hint.none）。
void CPageAssoc::QueryAssociation(const wchar_t* pszExt)
{
    CDuiString sProgId, sCommand;
    if (!AssocService::QueryProgIdAndCommand(pszExt, sProgId, sCommand)) return;

    if (CControlUI* pProg = m_ctx.HostFindControl(L"assoc_progid"))
    {
        pProg->SetText(sProgId.IsEmpty() ? StringMgr.Get("hint.none") : sProgId.GetData());
    }
    if (CControlUI* pCmd = m_ctx.HostFindControl(L"assoc_cmd"))
    {
        pCmd->SetText(sCommand.IsEmpty() ? StringMgr.Get("hint.none") : sCommand.GetData());
    }
}



[[nodiscard]] int CPageAssoc::ShowPickAssocProgramDialog(const CDuiString& sExt)
{
    CDuiString sE = sExt;
    if (sE.GetLength() > 0 && sE[0] != L'.') sE = CDuiString(L".") + sE;

    // [R94] 优先走 SHAssocEnumHandlers Shell API 直连（O(处理程序数)）
    std::vector<AssocHandlerEntry> vHandlers;
    DWORD dwErr = 0;
    int nEnum = AssocService::EnumAssocHandlers(sE, vHandlers, dwErr);
    if (nEnum == 0 && !vHandlers.empty())
    {
        int nRec = 0;
        for (size_t k = 0; k < vHandlers.size(); ++k)
            if (vHandlers[k].bRecommended) ++nRec;

        CDuiString sMsg, sTitle;
        sTitle = StringMgr.Get("assoc.sel_default_title");
        sMsg.Format(StringMgr.Get("assoc.sel_default_msg"),
                    sE.GetData(), static_cast<int>(vHandlers.size()));
        m_ctx.HostAlert(sTitle.GetData(), sMsg.GetData(), MB_ICONINFORMATION);
        CDuiString sLog;
        sLog.Format(L"[assoc] SHAssocEnumHandlers('%s') -> %d handler(s), %d recommended",
                    sE.GetData(), static_cast<int>(vHandlers.size()), nRec);
        m_ctx.HostLog(sLog.GetData());
        return 0;
    }

    // [R94 兜底] SHAssocEnumHandlers 失败或为空时回退到 HKCR RegEnumKeyEx 暴力遍历
    std::vector<std::pair<CDuiString, CDuiString> > vProgs;
    {
        ASRegGuard hRoot;
        if (::RegOpenKeyEx(HKEY_CLASSES_ROOT, nullptr, 0, KEY_READ | KEY_WOW64_64KEY,
                            hRoot.out()) != ERROR_SUCCESS) return -1;

        DWORD dwIdx = 0;
        constexpr int kRegKeyNameWchars = 512;   // 512 缓冲尺寸（语义命名，数值不变）
        wchar_t sBuf[kRegKeyNameWchars];
        while (true)
        {
            DWORD dwSize = kRegKeyNameWchars;
            LONG l = ::RegEnumKeyEx(hRoot, dwIdx++, sBuf, &dwSize, nullptr, nullptr, nullptr, nullptr);
            if (l != ERROR_SUCCESS) break;

            CDuiString sProgId(sBuf);
            if (sProgId.GetLength() > 0 && sProgId[0] == L'.') continue;
            if (sProgId.GetLength() > 0 && sProgId[0] == L'{') continue;
            if (_wcsnicmp(sProgId.GetData(), L"CLSID", 5) == 0) continue;
            if (_wcsnicmp(sProgId.GetData(), L"AppX", 4) == 0) continue;

            CDuiString sCmdPath;
            sCmdPath.Format(L"%s\\shell\\open\\command", sProgId.GetData());
            CDuiString sCmd = RegGetString(HKEY_CLASSES_ROOT, sCmdPath.GetData(),
                                            L"", KEY_READ | KEY_WOW64_64KEY);
            if (sCmd.IsEmpty()) continue;

            CDuiString sDisplayName = RegGetString(HKEY_CLASSES_ROOT, sProgId.GetData(),
                                                    L"FriendlyTypeName",
                                                    KEY_READ | KEY_WOW64_64KEY);
            if (sDisplayName.IsEmpty()) sDisplayName = sProgId;

            vProgs.push_back(std::make_pair(sProgId, sDisplayName));
        }
    }

    if (vProgs.empty()) return -1;

    CDuiString sMsg, sTitle;
    sTitle = StringMgr.Get("assoc.sel_default_title");
    sMsg.Format(StringMgr.Get("assoc.sel_default_msg"),
                sE.GetData(), static_cast<int>(vProgs.size()));
    m_ctx.HostAlert(sTitle.GetData(), sMsg.GetData(), MB_ICONINFORMATION);
    return -1;
}

// ============================================================
// [R97] "打开方式"按钮：用当前过滤扩展名（或首个扩展名）触发系统 OpenWith 对话框
// 概念名 OpenWithProgids 的 UI 侧触发点；后台列表在 AssocService::EnumOpenWithProgids。
// ============================================================
void CPageAssoc::OnOpenWithButton()
{
    // 1) 取过滤框输入
    CDuiString sKw;
    CEditUI* pEdit = static_cast<CEditUI*>(m_ctx.HostFindControl(L"assoc_filter"));
    if (pEdit) sKw = pEdit->GetText();
    sKw.Trim();

    CDuiString sExt = sKw;

    // 2) 过滤框非扩展名时回退到首个扩展名
    if (sExt.IsEmpty())
    {
        if (AssocCount() > 0)
            sExt = AssocAt(0);
    }

    if (sExt.IsEmpty())
    {
        m_ctx.HostToast(StringMgr.Get("assoc.s_openwith_noselect"),
                        StringMgr.Get("assoc.s_openwith_noselect_detail"), 1);
        return;
    }

    // 3) 触发系统"打开方式"对话框（ShellExecuteExW + "OpenWith" verb）
    bool bOk = AssocService::OpenWithDialog(sExt);
    if (bOk)
    {
        m_ctx.HostToast(StringMgr.Get("assoc.s_openwith_ok_short"),
                        StringMgr.Get("assoc.s_openwith_ok"));
    }
    else
    {
        m_ctx.HostToast(StringMgr.Get("assoc.s_openwith_fail_short"),
                        StringMgr.Get("assoc.s_openwith_fail"), 1);
    }

    CDuiString sLog;
    sLog.Format(L"[assoc] OpenWithDialog('%s') -> %s", sExt.GetData(),
                bOk ? L"ok" : L"fail");
    m_ctx.HostLog(sLog.GetData());
}


// ============================================================
// [R108] 批量应用默认应用：把 assoc_progid 当前 ProgId 应用到
// 联机列表所有可见 assocrow_%d 行，走 AssocService::ApplyAssocBatch
// （内部逐项 SetUserFileAssociationEx 三处维护 + SHCNE_ASSOCCHANGED）。
// 行号与 m_vAssocExt 下标一一对应（隐藏非重建），遍历 pLive 可见行
// 收集 (ext, progId) 后一次批量提交，避免每条注册表写都触发 UI 刷新。
// 返回成功条数；失败经 HostLog + Toast 分级反馈。
// ============================================================
int CPageAssoc::OnBatchApply()
{
    // 1) 当前 ProgId（assoc_progid 文本）
    CDuiString sProgId;
    if (CControlUI* pProg = m_ctx.HostFindControl(L"assoc_progid"))
        sProgId = pProg->GetText();
    sProgId.Trim();
    if (sProgId.IsEmpty() || sProgId == StringMgr.Get("hint.none"))
    {
        m_ctx.HostToast(StringMgr.Get("assoc.s_batch_noselect"),
                        StringMgr.Get("assoc.s_batch_noselect_detail"), 1);
        return 0;
    }

    // 2) 收集联机列表所有可见 assocrow_%d 行（行号 → m_vAssocExt）
    std::vector<std::pair<CDuiString, CDuiString> > vPairs;
    CVerticalLayoutUI* pLive = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"assoc_live_list"));
    if (!pLive)
    {
        m_ctx.HostToast(StringMgr.Get("assoc.s_batch_noext"),
                        L"", 1);
        return 0;
    }

    int nCount = pLive->GetCount();
    for (int i = 0; i < nCount; ++i)
    {
        CControlUI* pRow = pLive->GetItemAt(i);
        if (!pRow) continue;
        const wchar_t* pszN = pRow->GetName();
        if (!pszN || wcsncmp(pszN, L"assocrow_", 9) != 0) continue;
        if (!pRow->IsVisible()) continue;   // 过滤隐藏行（R108：只处理可见行）

        int nRow = _ttoi(pszN + 9);
        if (nRow < 0 || static_cast<size_t>(nRow) >= m_vAssocExt.size()) continue;

        // 跳过已是当前 ProgId 的扩展名（减少写操作）
        if (AssocService::VerifyUserFileAssociation(m_vAssocExt[nRow], sProgId))
            continue;

        vPairs.push_back(std::make_pair(m_vAssocExt[nRow], sProgId));
    }

    if (vPairs.empty())
    {
        m_ctx.HostToast(StringMgr.Get("assoc.s_batch_noext"),
                        StringMgr.Get("assoc.s_batch_noneed"), 0);
        return 0;
    }

    // 3) 二次确认（批量写注册表属可逆但影响面大）
    CDuiString sConfirm;
    sConfirm.Format(StringMgr.Get("assoc.s_batch_confirm"),
                    static_cast<int>(vPairs.size()), sProgId.GetData());
    if (!m_ctx.HostConfirmWarn(StringMgr.Get("assoc.s_batch_title"), sConfirm.GetData()))
        return 0;

    // 4) 批量提交
    int nOk = AssocService::ApplyAssocBatch(vPairs);
    int nFail = static_cast<int>(vPairs.size()) - nOk;

    // 5) 分级反馈
    if (nFail == 0)
    {
        CDuiString sMsg;
        sMsg.Format(StringMgr.Get("assoc.s_batch_ok"), nOk, sProgId.GetData());
        m_ctx.HostToast(StringMgr.Get("assoc.s_batch_ok_short"), sMsg.GetData(), 0);
    }
    else
    {
        CDuiString sMsg;
        sMsg.Format(StringMgr.Get("assoc.s_batch_fail"), nOk, nFail);
        m_ctx.HostToast(StringMgr.Get("assoc.s_batch_fail_short"), sMsg.GetData(), 1);
    }

    CDuiString sLog;
    sLog.Format(L"[assoc] ApplyAssocBatch: %d pair(s), ok=%d fail=%d, progid=%s",
                static_cast<int>(vPairs.size()), nOk, nFail, sProgId.GetData());
    m_ctx.HostLog(sLog.GetData());

    // 6) 刷新当前扩展名详情（assoc_progid/assoc_cmd 反映最新默认）
    if (AssocCount() > 0) QueryAssociation(AssocAt(0).GetData());
    return nOk;
}

// ============================================================
// [R115-1] HKCR 层写路径：注册扩展名（把 assoc_progid 当前 ProgId
// 写到 HKCR\.ext 默认值 + HKCR\.ext\OpenWithProgids 候选）。
// 取过滤框扩展名（assoc_filter），无输入则用 assoc_progid 里的 ProgId
// 反查当前扩展名（首个联机扩展名）。
// ============================================================
void CPageAssoc::OnRegisterExtKey()
{
    // 1) 取目标 ProgId（assoc_progid 文本）
    CDuiString sProgId;
    if (CControlUI* pProg = m_ctx.HostFindControl(L"assoc_progid"))
        sProgId = pProg->GetText();
    sProgId.Trim();
    if (sProgId.IsEmpty() || sProgId == StringMgr.Get("hint.none"))
    {
        m_ctx.HostToast(StringMgr.Get("assoc.s_hkcr_noprogid"),
                        StringMgr.Get("assoc.s_hkcr_noprogid_detail"), 1);
        return;
    }

    // 2) 取扩展名（assoc_filter 输入，无则首个联机扩展名）
    CDuiString sExt;
    if (CEditUI* pEdit = static_cast<CEditUI*>(m_ctx.HostFindControl(L"assoc_filter")))
        sExt = pEdit->GetText();
    sExt.Trim();
    if (sExt.IsEmpty() && AssocCount() > 0) sExt = AssocAt(0);
    if (sExt.IsEmpty())
    {
        m_ctx.HostToast(StringMgr.Get("assoc.s_hkcr_noext"),
                        StringMgr.Get("assoc.s_hkcr_noext_detail"), 1);
        return;
    }

    // 3) 二次确认（写 HKCR 属系统级，可逆但影响面大）
    CDuiString sConfirm;
    sConfirm.Format(StringMgr.Get("assoc.s_hkcr_confirm"),
                    sExt.GetData(), sProgId.GetData());
    if (!m_ctx.HostConfirmWarn(StringMgr.Get("assoc.s_hkcr_title"), sConfirm.GetData()))
        return;

    // 4) 提交
    EAssocWriteResult r = AssocService::RegisterExtKey(sExt, sProgId);
    if (r == EAssocWriteResult::ASSOC_WRITE_OK)
    {
        m_ctx.HostToast(StringMgr.Get("assoc.s_hkcr_ok_short"),
                        StringMgr.Get("assoc.s_hkcr_ok"), 0);
    }
    else
    {
        ShowAssocWriteError(static_cast<int>(r), sExt.GetData());
    }
    CDuiString sLog;
    sLog.Format(L"[assoc] RegisterExtKey('%s','%s') -> %d",
                sExt.GetData(), sProgId.GetData(), static_cast<int>(r));
    m_ctx.HostLog(sLog.GetData());

    // 5) 刷新当前详情
    QueryAssociation(sExt.GetData());
}

// ============================================================
// [R115-1] HKCR 层写路径：删除扩展名注册项（递归删 HKCR\.ext 与
// FileExts UserChoice/OpenWithProgids/OpenWithList）。
// ============================================================
void CPageAssoc::OnDeleteExtKey()
{
    CDuiString sExt;
    if (CEditUI* pEdit = static_cast<CEditUI*>(m_ctx.HostFindControl(L"assoc_filter")))
        sExt = pEdit->GetText();
    sExt.Trim();
    if (sExt.IsEmpty() && AssocCount() > 0) sExt = AssocAt(0);
    if (sExt.IsEmpty())
    {
        m_ctx.HostToast(StringMgr.Get("assoc.s_hkcr_noext"),
                        StringMgr.Get("assoc.s_hkcr_noext_detail"), 1);
        return;
    }

    CDuiString sConfirm;
    sConfirm.Format(StringMgr.Get("assoc.s_hkcr_del_confirm"), sExt.GetData());
    if (!m_ctx.HostConfirmWarn(StringMgr.Get("assoc.s_hkcr_del_title"), sConfirm.GetData()))
        return;

    EAssocWriteResult r = AssocService::DeleteExtKey(sExt);
    if (r == EAssocWriteResult::ASSOC_WRITE_OK)
    {
        m_ctx.HostToast(StringMgr.Get("assoc.s_hkcr_del_ok_short"),
                        StringMgr.Get("assoc.s_hkcr_del_ok"), 0);
    }
    else
    {
        CDuiString sMsg;
        sMsg.Format(StringMgr.Get("assoc.s_hkcr_del_fail"), static_cast<int>(r));
        m_ctx.HostToast(StringMgr.Get("assoc.s_hkcr_del_fail_short"), sMsg.GetData(), 2);
    }
    CDuiString sLog;
    sLog.Format(L"[assoc] DeleteExtKey('%s') -> %d",
                sExt.GetData(), static_cast<int>(r));
    m_ctx.HostLog(sLog.GetData());

    // 删除后刷新列表
    Build();
    QueryAssociation(sExt.GetData());
}

// ============================================================
// [R115-1] HKCR 层写路径：恢复系统默认（删除 UserChoice + HKCR\.ext 注册项）。
// ============================================================
void CPageAssoc::OnRestoreExtDefault()
{
    CDuiString sExt;
    if (CEditUI* pEdit = static_cast<CEditUI*>(m_ctx.HostFindControl(L"assoc_filter")))
        sExt = pEdit->GetText();
    sExt.Trim();
    if (sExt.IsEmpty() && AssocCount() > 0) sExt = AssocAt(0);
    if (sExt.IsEmpty())
    {
        m_ctx.HostToast(StringMgr.Get("assoc.s_hkcr_noext"),
                        StringMgr.Get("assoc.s_hkcr_noext_detail"), 1);
        return;
    }

    CDuiString sConfirm;
    sConfirm.Format(StringMgr.Get("assoc.s_hkcr_restore_confirm"), sExt.GetData());
    if (!m_ctx.HostConfirmWarn(StringMgr.Get("assoc.s_hkcr_restore_title"), sConfirm.GetData()))
        return;

    EAssocWriteResult r = AssocService::RestoreExtDefault(sExt);
    if (r == EAssocWriteResult::ASSOC_WRITE_OK)
    {
        m_ctx.HostToast(StringMgr.Get("assoc.s_hkcr_restore_ok_short"),
                        StringMgr.Get("assoc.s_hkcr_restore_ok"), 0);
    }
    else
    {
        CDuiString sMsg;
        sMsg.Format(StringMgr.Get("assoc.s_hkcr_restore_fail"), static_cast<int>(r));
        m_ctx.HostToast(StringMgr.Get("assoc.s_hkcr_restore_fail_short"), sMsg.GetData(), 2);
    }
    CDuiString sLog;
    sLog.Format(L"[assoc] RestoreExtDefault('%s') -> %d",
                sExt.GetData(), static_cast<int>(r));
    m_ctx.HostLog(sLog.GetData());

    QueryAssociation(sExt.GetData());
}



void CPageAssoc::Build()
{
    // [R55] 首建图标缓存目录
    EnsureIconCacheDirectory();

    // 离线卡片：导入关联 XML 前仅显示提示
    AssocBuildOfflineTip();

    // 联机卡片：初始化容器 + 定位实时列表
    m_vAssocExt.clear();
    CVerticalLayoutUI* pLive = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"assoc_live_list"));
    if (!pLive) return;
    pLive->RemoveAll();

    // 三处注册表扩展名枚举（HKCU\FileExts / HKLM\Classes / HKCR）
    AssocBuildEnumHkcu();
    AssocBuildEnumHklm();
    AssocBuildEnumHkcr();

    // 去重排序
    AssocBuildDedupExts();

    // [R41] 按 ProgId 前缀分组：预计算分组标签 + 目标 exe，输出三并行数组
    std::vector<CDuiString> vGroupKey(m_vAssocExt.size(), L"");
    std::vector<CDuiString> vProgId(m_vAssocExt.size(), L"");
    std::vector<CDuiString> vAssocExe(m_vAssocExt.size(), L"");
    AssocBuildPrecomputeGroup(vGroupKey, vProgId, vAssocExe);

    // [R41] 渲染分组标题行 + 扩展名行（行索引 assocrow_%d 与 m_vAssocExt 对齐）
    int nGroupCount = 0;
    int nExeResolved = 0;
    AssocBuildRenderRows(vGroupKey, vProgId, vAssocExe, pLive, nGroupCount, nExeResolved);

    // [R41][R56][R57][U-3] 分组/图标缓存/扩展名状态条统计日志
    AssocBuildLogStats(vAssocExe, nGroupCount, nExeResolved);

    // 页面收尾
    UpdateAssocCount(static_cast<int>(m_vAssocExt.size()));
}







// R33: 应用过滤 —— 按关键字实时过滤扩展名列表

void CPageAssoc::ApplyAssocFilter()

{
    // [AUDIT-P2] 原 212 行 / CC=25，拆为 3 段 + 合并两处重复清理逻辑：
    //   AsfMatchRow       单行关键字匹配（扩展名 / ProgId 任一命中）
    //   AsfRemoveByName   按控件名从父容器摘除（原有两段 39+41 行逐字重复，合并）
    //   AsfShowEmptyHint  无匹配时的空态提示行
    // 过滤语义、行名前缀 assocrow_、计数口径逐字保留。

    CEditUI* pAssocKwEdit = static_cast<CEditUI*>(m_ctx.HostFindControl(L"assoc_filter"));
    CDuiString sKw = pAssocKwEdit ? pAssocKwEdit->GetText() : CDuiString(L"");
    CDuiString sKwLower = sKw;
    sKwLower.MakeLower();

    CVerticalLayoutUI* pLive = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"assoc_live_list"));
    if (!pLive) return;

    // [R41] 遍历渲染后的所有子控件，跳过 aschdr_ 分组标题行（避免破坏行索引连续性）
    int nShown = 0, nTotal = 0;
    const int nCount = pLive->GetCount();
    for (int i = 0; i < nCount; ++i)
    {
        CControlUI* pRow = pLive->GetItemAt(i);
        if (!pRow) continue;
        const wchar_t* pszN = pRow->GetName();
        if (!pszN || wcsncmp(pszN, L"assocrow_", 9) != 0) continue;
        ++nTotal;
        if (sKw.IsEmpty())
        {
            pRow->SetVisible(true);
            ++nShown;
            continue;
        }
        // 读该行的第一个子控件（扩展名）和第二个（ProgId）做过滤匹配
        bool bHit = false;
        if (CContainerUI* pC = dynamic_cast<CContainerUI*>(pRow))
        {
            for (int j = 0; j < pC->GetCount(); ++j)
            {
                CControlUI* pSub = pC->GetItemAt(j);
                if (!pSub) continue;
                CTextUI* pTxt = dynamic_cast<CTextUI*>(pSub);
                if (!pTxt) continue;
                CDuiString s = pTxt->GetText();
                s.MakeLower();
                if (s.Find(sKwLower.GetData()) >= 0) { bHit = true; break; }
            }
        }
        pRow->SetVisible(bHit);
        if (bHit) ++nShown;
    }
    // 无匹配 → 显示空态提示（先清理旧空态行避免累积）
    AsfRemoveByName(L"assoc_empty");

    if (nShown == 0)
    {
        AsfShowEmptyHint(pLive, sKw);
    }
    else
    {
        // [R48] 有匹配时清理上一次遗留的“清除过津”按钮行
        AsfRemoveByName(L"assoc_empty_row");
    }

    UpdateAssocCount(nShown, nTotal);
}

// [AUDIT-P2] 从 ApplyAssocFilter 提取：单行关键字匹配
// 遍历行内所有 CTextUI 子控件（扩展名 / ProgId / …），任一命中即算匹配。
bool CPageAssoc::AsfMatchRow(DuiLib::CControlUI* pRow, const CDuiString& sKwLower)
{
    bool bHit = false;
    if (CContainerUI* pC = dynamic_cast<CContainerUI*>(pRow))
    {
        for (int j = 0; j < pC->GetCount(); ++j)
        {
            CControlUI* pSub = pC->GetItemAt(j);
            if (!pSub) continue;
            CTextUI* pTxt = dynamic_cast<CTextUI*>(pSub);
            if (!pTxt) continue;
            CDuiString s = pTxt->GetText();
            s.MakeLower();
            if (s.Find(sKwLower.GetData()) >= 0) { bHit = true; break; }
        }
    }
    return bHit;
}

// [AUDIT-P2] 从 ApplyAssocFilter 提取：按控件名从父容器摘除并释放
// 原实现有两段（assoc_empty / assoc_empty_row）逐字相同，只差控件名，此处合并。
void CPageAssoc::AsfRemoveByName(const wchar_t* pszName)
{
    CControlUI* pOld = m_ctx.HostFindControl(pszName);
    if (!pOld) return;

    CContainerUI* pParent = dynamic_cast<CContainerUI*>(pOld->GetParent());
    if (!pParent) return;

    int idx = -1;
    for (int i = 0; i < pParent->GetCount(); ++i)
    {
        if (pParent->GetItemAt(i) == pOld) { idx = i; break; }
    }
    if (idx < 0) return;

    pParent->RemoveAt(idx);
    std::unique_ptr<CControlUI> owned(pOld); // [C++17 R3] RemoveAt 已摘除，作用域结束释放
}

// [AUDIT-P2] 从 ApplyAssocFilter 提取：无匹配时的空态提示行
void CPageAssoc::AsfShowEmptyHint(DuiLib::CVerticalLayoutUI* pLive, const CDuiString& sKw)
{
    // 附加空态行
    auto pTxt = std::make_unique<CTextUI>(); // [C++17 R3]
    CDuiString sEmpty;
    sEmpty = StringMgr.Get("misc.h46c8");
    sEmpty += sKw;
    sEmpty += L"）";
    pTxt->SetName(L"assoc_empty");
    pTxt->SetText(sEmpty.GetData());
    pTxt->SetFixedHeight(32);
    pTxt->SetTextColor(tokens::Token(tokens::EToken::InkMuted));
    pTxt->SetAttribute(L"align", L"center");
    pTxt->SetMouseEnabled(false);
    pLive->Add(pTxt.release()); // [C++17 R3] ownership transferred to DuiLib
}



// R33: 更新底栏"扩展名 共 N 项"（nShown = 当前可见，nTotal = 总数）
void CPageAssoc::UpdateAssocCount(int nShown, int nTotal /*= -1*/)
{
    CTextUI* pCount = static_cast<CTextUI*>(m_ctx.HostFindControl(L"lbl_assoc_count"));
    if (!pCount) return;
    if (nTotal < 0) nTotal = nShown;
    CDuiString s;
    if (nTotal == 0)
        s = StringMgr.Get("misc.h7891");
    else if (nShown == nTotal)
        s.Format(StringMgr.Get("misc.h658a"), nShown);
    else
        s.Format(StringMgr.Get("misc.hc55a"), nTotal, nShown);
    pCount->SetText(s.GetData());
}




// ============================================================
// P8：第二级事件路由（原 CFrameWnd::Notify 关联页分支逐字迁入）
// ============================================================

void CPageAssoc::OnEvent(TNotifyUI& msg)
{
    CDuiString sName = msg.pSender ? msg.pSender->GetName() : CDuiString(L"");
    const wchar_t* pszName = sName.GetData();

    if (msg.sType == L"click")
    {
        // [修复 R5 / P0-1b] 文件关联-联机列表行点击：
        // 行由 BuildAssocCommon() 以 assocrow_%d 命名并 new 成 CClickRowUI。
        // 行序号 k 与 m_vAssocExt 下标严格一一对应（见 Build）。
        int nAssocRow = -1;
        if (ParsePrefix1(sName, L"assocrow_", nAssocRow))
        {
            if (nAssocRow >= 0 && nAssocRow < static_cast<int>(AssocCount()))
                QueryAssociation(AssocAt(nAssocRow).GetData());
            return;
        }

        // ---- file association buttons ----
        if (sName == L"btn_assoc_img_import"
            || sName == L"btn_assoc_img_export"
            || sName == L"btn_assoc_img_delete"
            || sName == L"btn_assoc_live_import"
            || sName == L"btn_assoc_live_export"
            || sName == L"btn_assoc_live_reset")
        {
            // [修复 R5 / P0-1a] AssocButton 内部按按钮名区分离线/联机与
            // 导入/导出/删除，组装 dism 参数、隐藏执行并回传退出码。
            AssocButton(sName.GetData());
            return;
        }

        // [R97] "打开方式"按钮：触发系统 OpenWith 对话框
        if (sName == L"btn_assoc_openwith")
        {
            OnOpenWithButton();
            return;
        }

        // [R108] 批量应用：把 assoc_progid 当前 ProgId 应用到联机列表所有可见行
        if (sName == L"btn_assoc_batch_apply")
        {
            OnBatchApply();
            return;
        }

        // [R115-1] HKCR 层写路径：注册扩展名 / 删除扩展名 / 恢复系统默认
        if (sName == L"btn_assoc_hkcr_register")
        {
            OnRegisterExtKey();
            return;
        }
        if (sName == L"btn_assoc_hkcr_delete")
        {
            OnDeleteExtKey();
            return;
        }
        if (sName == L"btn_assoc_hkcr_restore")
        {
            OnRestoreExtDefault();
            return;
        }
    }

    if (msg.sType == L"textchanged")
    {
        // [R33] 关联页实时过滤
        if (sName == L"assoc_filter") ApplyAssocFilter();
    }
}

// P8：原 CFrameWnd WM_RBUTTONUP 通用列表命中块（两个关联列表）
bool CPageAssoc::OnContextMenu(POINT pt, bool bFromContextMsg)
{
    if (bFromContextMsg) return false;
    CControlUI* pHit = m_ctx.HostFindControlAt(pt);

    const wchar_t* kListNames[] = { L"assoc_img_list", L"assoc_live_list" };
    EListCtx nKinds[] = { EListCtx::LCX_ASSOC_IMG, EListCtx::LCX_ASSOC_LIVE };

    // [R100] 行级右键：assocrow_ 命中 → 带行号弹菜单（删除/重置此扩展名）
    for (CControlUI* q = pHit; q; q = q->GetParent())
    {
        const wchar_t* pn = q->GetName();
        if (!pn) continue;
        if (wcsncmp(pn, L"assocrow_", 9) == 0)
        {
            int nRow = _ttoi(pn + 9);
            // [R128-2] 行级专属右键菜单（6 动作：复制扩展名/ProgId/命令/打开注册表项/设为默认/复制处理程序名）
            ShowAssocContextMenu(nRow, pt);
            return true;
        }
    }

    for (CControlUI* q = pHit; q; q = q->GetParent())
    {
        const wchar_t* pn = q->GetName();
        if (!pn) continue;
        for (int k = 0; k < 2; ++k)
        {
            if (lstrcmpW(pn, kListNames[k]) == 0)
            {
                UiHelpers_ShowUnifiedListContextMenu(&m_ctx, m_ctx.HostHwnd(),
                                                   nKinds[k], -1, pt);
                return true;
            }
        }
    }
    return false;
}

// ============================================================
// [R128-2] 文件关联行级专属右键菜单（assocrow_%d 命中）
// 6 动作：复制扩展名 / 复制 ProgId / 复制 open\command / 打开关联注册表项 /
// 设为默认（UserChoice 写路径，HostConfirmWarn 二次确认）/ 复制处理程序名。
void CPageAssoc::OnGlobalCopy([[maybe_unused]] bool bCut)
{
    if (m_vAssocExt.empty())
    {
        m_ctx.HostToast(StringMgr.Get("assoc.s_nocopy"),
                        StringMgr.Get("assoc.s_nocopy_body"), 1);
        return;
    }
    const CDuiString& sExt = m_vAssocExt[0];
    if (sExt.IsEmpty()) return;
    FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), sExt.GetData());
    m_ctx.HostToast(StringMgr.Get("assoc.s_copydone"),
                    sExt.GetData(), 0);
    CDuiString sLog;
    sLog.Format(L"[assoc] copy ext: %s", sExt.GetData());
    m_ctx.HostLog(sLog.GetData());
}
