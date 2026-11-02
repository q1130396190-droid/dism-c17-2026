#include "stdafx.h"

// ============================================================
// Page_Intl.cpp —— 区域/国际化页（新版 Q4 功能移植）
// 1 个原 CFrameWnd 函数物理搬迁，函数体零逻辑改动，
// 宿主引用改 m_ctx.Xxx。
// ============================================================

#include "Page_Intl.h"
#include "StringManager.h"
#include "DataEngine.h"
#include "IntlService.h"     // [R90] 时区/区域写路径
#include <tlhelp32.h>
#include <cstdio>
#include <memory>                // [C++17 R3] make_unique

using namespace DuiLib;
using i18n::CStringManager;

void CPageIntl::BuildPage()
{
    CComboUI* pCombo = static_cast<CComboUI*>(m_ctx.HostFindControl(L"intl_tzcombo"));
    CTextUI*  pCur   = static_cast<CTextUI*>(m_ctx.HostFindControl(L"lbl_intl_cur"));
    if (!pCombo) return;
    pCombo->RemoveAll();

    m_nSel = -1;

    if (!m_ctx.DataEngine().IsValid()) return;

    if (m_ctx.DataEngine().GetTimeZoneCount() == 0)
    {
        if (pCur) pCur->SetText(L"[intl] TimeZoneMap not loaded");
        return;
    }

    // [R90] 检测当前时区（IntlService::GetCurrentTimeZone，替代 tzutil.exe /g 绕路）
    CDuiString sCurTz;
    IntlService::GetCurrentTimeZone(sCurTz);

    if (pCur && !sCurTz.IsEmpty())
    {
        CDuiString sTxt;
        sTxt.Format(L"[intl] current TZ: %s", sCurTz.GetData());
        pCur->SetText(sTxt.GetData());
    }

    int nSel = -1;
    int nCount = m_ctx.DataEngine().GetTimeZoneCount();
    for (int n = 0; n < nCount; ++n)
    {
        CDuiString sKey, sOff;
        if (!m_ctx.DataEngine().GetTimeZone(n, sKey, sOff)) continue;

        CDuiString sLabel = sKey;
        if (!sOff.IsEmpty())
        {
            sLabel += L" (UTC";
            sLabel += sOff;
            sLabel += L")";
        }

        if (pCombo)
        {
            auto pItem = std::make_unique<CListLabelElementUI>(); // [C++17 R3]
            pItem->SetText(sLabel.GetData());
            pCombo->Add(pItem.release()); // [C++17 R3] ownership transferred to DuiLib
        }

        if (!sCurTz.IsEmpty() && sKey.Compare(sCurTz) == 0) nSel = n;
    }

    if (pCombo && nSel >= 0) pCombo->SelectItem(nSel);

    CDuiString sLog;
    sLog.Format(L"[intl] timezone combo built: %d entries, sel=%d", nCount, nSel);
    m_ctx.HostLog(sLog.GetData());

    // [R96] 区域 / UI 语言 Combo 接线
    BuildLocaleCombos();
}

// [R96] 填充用户区域 / 系统区域 / UI 语言三个 Combo
void CPageIntl::BuildLocaleCombos()
{
    std::vector<IntlLangEntry> vCand;
    IntlService::GetCandidateLocales(vCand);

    std::vector<IntlLangEntry> vInstalled;
    IntlService::GetInstalledLanguages(vInstalled);

    CDuiString sCurUser, sCurSys, sCurUi;
    IntlService::GetUserLocaleName(sCurUser);
    IntlService::GetSystemLocaleName(sCurSys);
    IntlService::GetUserUILanguageName(sCurUi);

    // --- 用户区域 Combo ---
    CComboUI* pLocCombo = static_cast<CComboUI*>(m_ctx.HostFindControl(L"intl_loccombo"));
    if (pLocCombo)
    {
        pLocCombo->RemoveAll();
        m_nSelLocale = -1;
        for (size_t i = 0; i < vCand.size(); ++i)
        {
            auto pItem = std::make_unique<CListLabelElementUI>(); // [C++17 R3]
            CDuiString sLabel = vCand[i].sDisplay;
            if (vCand[i].bInstalled) sLabel += StringMgr.Get("intl.cand_installed");
            if (vCand[i].bCurrent)   sLabel += StringMgr.Get("intl.cand_current");
            pItem->SetText(sLabel.GetData());
            pLocCombo->Add(pItem.release()); // [C++17 R3] ownership transferred to DuiLib
            if (vCand[i].sName.Compare(sCurUser) == 0) m_nSelLocale = static_cast<int>(i);
        }
        if (m_nSelLocale >= 0) pLocCombo->SelectItem(m_nSelLocale);
    }

    // --- 系统区域 Combo ---
    CComboUI* pSysCombo = static_cast<CComboUI*>(m_ctx.HostFindControl(L"intl_sysloccombo"));
    if (pSysCombo)
    {
        pSysCombo->RemoveAll();
        m_nSelSysLocale = -1;
        for (size_t i = 0; i < vCand.size(); ++i)
        {
            auto pItem = std::make_unique<CListLabelElementUI>(); // [C++17 R3]
            CDuiString sLabel = vCand[i].sDisplay;
            if (vCand[i].sName.Compare(sCurSys) == 0) sLabel += StringMgr.Get("intl.cand_system");
            pItem->SetText(sLabel.GetData());
            pSysCombo->Add(pItem.release()); // [C++17 R3] ownership transferred to DuiLib
            if (vCand[i].sName.Compare(sCurSys) == 0) m_nSelSysLocale = static_cast<int>(i);
        }
        if (m_nSelSysLocale >= 0) pSysCombo->SelectItem(m_nSelSysLocale);
    }

    // --- UI 语言 Combo（优先已安装语言，无则用候选集）---
    CComboUI* pUiCombo = static_cast<CComboUI*>(m_ctx.HostFindControl(L"intl_uilangcombo"));
    if (pUiCombo)
    {
        pUiCombo->RemoveAll();
        m_nSelUiLang = -1;

        // 已安装语言在前
        for (size_t i = 0; i < vInstalled.size(); ++i)
        {
            auto pItem = std::make_unique<CListLabelElementUI>(); // [C++17 R3]
            CDuiString sLabel = vInstalled[i].sDisplay;
            if (vInstalled[i].sName.Compare(sCurUi) == 0) sLabel += StringMgr.Get("intl.cand_current");
            pItem->SetText(sLabel.GetData());
            pUiCombo->Add(pItem.release()); // [C++17 R3] ownership transferred to DuiLib
            if (vInstalled[i].sName.Compare(sCurUi) == 0) m_nSelUiLang = static_cast<int>(i);
        }

        // 再附候选未安装项
        int nBase = static_cast<int>(vInstalled.size());
        for (size_t i = 0; i < vCand.size(); ++i)
        {
            if (vCand[i].bInstalled) continue;
            bool bInList = false;
            for (size_t k = 0; k < vInstalled.size(); ++k)
                if (vInstalled[k].sName.Compare(vCand[i].sName) == 0) { bInList = true; break; }
            if (bInList) continue;

            auto pItem = std::make_unique<CListLabelElementUI>(); // [C++17 R3]
            pItem->SetText(vCand[i].sDisplay.GetData());
            pUiCombo->Add(pItem.release()); // [C++17 R3] ownership transferred to DuiLib
        }

        if (m_nSelUiLang < 0)
        {
            // 未找到当前 UI 语言时默认选第一项
            m_nSelUiLang = 0;
        }
        if (pUiCombo->GetCount() > 0) pUiCombo->SelectItem(m_nSelUiLang);
    }

    CDuiString sLog;
    sLog.Format(L"[intl] locale combos built: cand=%d installed=%d "
                   "user=%s sys=%s ui=%s",
                static_cast<int>(vCand.size()), static_cast<int>(vInstalled.size()),
                sCurUser.GetData(), sCurSys.GetData(), sCurUi.GetData());
    m_ctx.HostLog(sLog.GetData());

    // [R101] 已安装语言 Combo + 可安装候选 Combo
    BuildInstalledLangCombo();
    BuildAddLangCombo();

    // [R120-1] 系统区域格式四个 Combo
    BuildFormatCombos();
}

// [R101] 填充已安装语言 Combo（来自 HKCU\User Profile\Languages）
void CPageIntl::BuildInstalledLangCombo()
{
    CComboUI* pCombo = static_cast<CComboUI*>(m_ctx.HostFindControl(L"intl_instlangcombo"));
    if (!pCombo) return;
    pCombo->RemoveAll();
    m_nSelInstLang = -1;

    std::vector<IntlLangEntry> vInstalled;
    IntlService::GetInstalledLanguages(vInstalled);

    CDuiString sCurUi;
    IntlService::GetUserUILanguageName(sCurUi);

    for (size_t i = 0; i < vInstalled.size(); ++i)
    {
        auto pItem = std::make_unique<CListLabelElementUI>(); // [C++17 R3]
        CDuiString sLabel = vInstalled[i].sDisplay;
        if (vInstalled[i].sName.Compare(sCurUi) == 0) sLabel += StringMgr.Get("intl.cand_current_ui");
        pItem->SetText(sLabel.GetData());
        pCombo->Add(pItem.release()); // [C++17 R3] ownership transferred to DuiLib
        if (vInstalled[i].sName.Compare(sCurUi) == 0) m_nSelInstLang = static_cast<int>(i);
    }
    if (m_nSelInstLang < 0 && pCombo->GetCount() > 0) m_nSelInstLang = 0;
    if (pCombo->GetCount() > 0) pCombo->SelectItem(m_nSelInstLang);

    CDuiString sLog;
    sLog.Format(L"[intl] installed lang combo built: %d entries, sel=%d",
                static_cast<int>(vInstalled.size()), m_nSelInstLang);
    m_ctx.HostLog(sLog.GetData());
}

// [R101] 填充可安装语言 Combo（候选集中排除已安装项）
void CPageIntl::BuildAddLangCombo()
{
    CComboUI* pCombo = static_cast<CComboUI*>(m_ctx.HostFindControl(L"intl_addlangcombo"));
    if (!pCombo) return;
    pCombo->RemoveAll();

    std::vector<IntlLangEntry> vCand;
    IntlService::GetCandidateLocales(vCand);
    std::vector<IntlLangEntry> vInstalled;
    IntlService::GetInstalledLanguages(vInstalled);

    for (size_t i = 0; i < vCand.size(); ++i)
    {
        if (vCand[i].bInstalled) continue;
        bool bDup = false;
        for (size_t k = 0; k < vInstalled.size(); ++k)
            if (vInstalled[k].sName.Compare(vCand[i].sName) == 0) { bDup = true; break; }
        if (bDup) continue;

        auto pItem = std::make_unique<CListLabelElementUI>(); // [C++17 R3]
        pItem->SetText(vCand[i].sDisplay.GetData());
        pCombo->Add(pItem.release()); // [C++17 R3] ownership transferred to DuiLib
    }

    if (pCombo->GetCount() > 0) pCombo->SelectItem(0);

    CDuiString sLog;
    sLog.Format(L"[intl] add lang combo built: %d entries", static_cast<int>(pCombo->GetCount()));
    m_ctx.HostLog(sLog.GetData());
}

// [AUDIT-P2] 三处「写入结果上报」逐字同构，合并为本函数。
// 调用方：OnAddInstalledLanguage / OnRemoveInstalledLanguage / OnApplyUserLocale
// 三者原本各写一遍 17 行（成功弹 Toast + 日志 / 失败弹 Toast + 带 rc 与 err 的日志），
// 只有文案键与日志前缀不同。合并后每处只剩 4 行调用。
void CPageIntl::IntlReportWriteResult(EIntlWriteResult nRc, const CDuiString& sErr,
                                      const CDuiString& sName,
                                      const char* pszDoneShort, const char* pszDone,
                                      const char* pszFailShort, const char* pszFail,
                                      const wchar_t* pszLogOk,
                                      const wchar_t* pszLogFail)
{
    if (nRc == EIntlWriteResult::INTL_WRITE_OK)
    {
        m_ctx.HostToast(StringMgr.Get(pszDoneShort), StringMgr.Get(pszDone));
        CDuiString sLog;
        sLog.Format(pszLogOk, sName.GetData());
        m_ctx.HostLog(sLog.GetData());
    }
    else
    {
        m_ctx.HostToast(StringMgr.Get(pszFailShort), StringMgr.Get(pszFail));
        CDuiString sLog;
        sLog.Format(pszLogFail, sName.GetData(), static_cast<int>(nRc), sErr.GetData());
        m_ctx.HostLog(sLog.GetData());
    }
}

// [R101] 安装所选语言（追加到用户语言列表）
void CPageIntl::OnAddInstalledLanguage()
{
    CComboUI* pCombo = static_cast<CComboUI*>(m_ctx.HostFindControl(L"intl_addlangcombo"));
    if (!pCombo)
    {
        m_ctx.HostToast(StringMgr.Get("intl.s_badlang_short"),
                        StringMgr.Get("intl.s_badlang_detail"));
        return;
    }
    int nIdx = pCombo->GetCurSel();
    if (nIdx < 0)
    {
        m_ctx.HostToast(StringMgr.Get("intl.s_badlang_short"),
                        StringMgr.Get("intl.s_badlang_detail"));
        return;
    }

    // 候选索引 → LocaleName：遍历候选集，跳过已安装项，取第 nIdx 个未安装
    std::vector<IntlLangEntry> vCand;
    IntlService::GetCandidateLocales(vCand);
    std::vector<IntlLangEntry> vInstalled;
    IntlService::GetInstalledLanguages(vInstalled);

    int nFound = 0;
    CDuiString sName;
    for (size_t i = 0; i < vCand.size(); ++i)
    {
        bool bDup = false;
        for (size_t k = 0; k < vInstalled.size(); ++k)
            if (vInstalled[k].sName.Compare(vCand[i].sName) == 0) { bDup = true; break; }
        if (bDup) continue;
        if (nFound == nIdx) { sName = vCand[i].sName; break; }
        ++nFound;
    }

    if (sName.IsEmpty())
    {
        m_ctx.HostToast(StringMgr.Get("intl.s_badlang_short"),
                        StringMgr.Get("intl.s_badlang_detail"));
        return;
    }

    // [R101] 安装前确认
    CDuiString sConfirm;
    sConfirm.Format(StringMgr.Get("intl.s_add_lang_confirm"), sName.GetData());
    if (!m_ctx.HostConfirmWarn(StringMgr.Get("intl.s_add_lang_confirm_title"),
                               sConfirm.GetData()))
        return;

    CDuiString sErr;
    EIntlWriteResult nRc = IntlService::AddInstalledLanguage(sName.GetData(), sErr);
    IntlReportWriteResult(nRc, sErr, sName,
                          "intl.s_add_lang_done_short", "intl.s_add_lang_done",
                          "intl.s_add_lang_fail_short", "intl.s_add_lang_fail",
                          L"[intl] language added: %s", L"[intl] add language failed: %s rc=%d err=%s");

    // 重建三个语言相关 Combo
    BuildLocaleCombos();
}

// [R101] 删除所选语言（从用户语言列表移除）
void CPageIntl::OnRemoveInstalledLanguage()
{
    std::vector<IntlLangEntry> vInstalled;
    IntlService::GetInstalledLanguages(vInstalled);

    int nIdx = m_nSelInstLang;
    if (nIdx < 0 || nIdx >= static_cast<int>(vInstalled.size()))
    {
        m_ctx.HostToast(StringMgr.Get("intl.s_badlang_short"),
                        StringMgr.Get("intl.s_badlang_detail"));
        return;
    }

    CDuiString sName = vInstalled[nIdx].sName;
    CDuiString sConfirm;
    sConfirm.Format(StringMgr.Get("intl.s_remove_lang_confirm"), sName.GetData());
    if (!m_ctx.HostConfirmWarn(StringMgr.Get("intl.s_remove_lang_confirm_title"),
                               sConfirm.GetData()))
        return;

    CDuiString sErr;
    EIntlWriteResult nRc = IntlService::RemoveInstalledLanguage(sName.GetData(), sErr);
    IntlReportWriteResult(nRc, sErr, sName,
                          "intl.s_remove_lang_done_short", "intl.s_remove_lang_done",
                          "intl.s_remove_lang_fail_short", "intl.s_remove_lang_fail",
                          L"[intl] language removed: %s", L"[intl] remove language failed: %s rc=%d err=%s");

    BuildLocaleCombos();
}

// [R120-1] 系统区域格式：填充短日期/短时间/十进制/货币四个 Combo
void CPageIntl::BuildFormatCombos()
{
    m_nSelShortDate = FillFormatCombo(L"intl_shortdatecombo", EIntlFormatKind::INTL_FMT_SHORTDATE);
    m_nSelShortTime = FillFormatCombo(L"intl_shorttimecombo", EIntlFormatKind::INTL_FMT_SHORTTIME);
    m_nSelDecimal   = FillFormatCombo(L"intl_decimalcombo",  EIntlFormatKind::INTL_FMT_DECIMAL);
    m_nSelCurrency  = FillFormatCombo(L"intl_currencycombo", EIntlFormatKind::INTL_FMT_CURRENCY);

    CDuiString sLog;
    sLog.Format(L"[intl] format combos built: date=%d time=%d dec=%d cur=%d",
                m_nSelShortDate, m_nSelShortTime, m_nSelDecimal, m_nSelCurrency);
    m_ctx.HostLog(sLog.GetData());
}

// [R120-1] 填充指定格式 Combo（候选列表 + 当前值），返回当前值索引（-1 未命中）
[[nodiscard]] int CPageIntl::FillFormatCombo(const wchar_t* pszComboName, EIntlFormatKind nKind)
{
    CComboUI* pCombo = static_cast<CComboUI*>(m_ctx.HostFindControl(pszComboName));
    if (!pCombo) return -1;
    pCombo->RemoveAll();

    // 候选列表（含当前值，去重）
    const wchar_t* kCandidates[12] = { 0 };
    int nCand = 0;
    switch (nKind)
    {
    case EIntlFormatKind::INTL_FMT_SHORTDATE:
        kCandidates[0] = L"yyyy/M/d";
        kCandidates[1] = L"yyyy-MM-dd";
        kCandidates[2] = L"M/d/yyyy";
        kCandidates[3] = L"dd/MM/yyyy";
        kCandidates[4] = L"yyyy年M月d日";
        kCandidates[5] = L"d/M/yyyy";
        nCand = 6;
        break;
    case EIntlFormatKind::INTL_FMT_SHORTTIME:
        kCandidates[0] = L"HH:mm";
        kCandidates[1] = L"HH:mm:ss";
        kCandidates[2] = L"h:mm tt";
        kCandidates[3] = L"h:mm:ss tt";
        kCandidates[4] = L"H:mm";
        nCand = 5;
        break;
    case EIntlFormatKind::INTL_FMT_DECIMAL:
        kCandidates[0] = L".";
        kCandidates[1] = L",";
        kCandidates[2] = L"٫";   // Arabic decimal separator (U+066B)
        kCandidates[3] = L" ";
        nCand = 4;
        break;
    case EIntlFormatKind::INTL_FMT_CURRENCY:
        kCandidates[0] = L"¥";
        kCandidates[1] = L"$";
        kCandidates[2] = L"€";
        kCandidates[3] = L"£";
        kCandidates[4] = L"₩";
        kCandidates[5] = L"₽";
        kCandidates[6] = L"₹";
        kCandidates[7] = L"R$";
        kCandidates[8] = L"Rp";
        kCandidates[9] = L"RM";
        kCandidates[10] = L"฿";
        kCandidates[11] = L"₫";
        nCand = 12;
        break;
    default:
        return -1;
    }

    // 读取当前实值
    CDuiString sCur;
    IntlService::GetLocaleFormat(nKind, sCur);

    // 先放当前实值（未在候选列表时），便于用户识别现状
    bool bCurInList = false;
    for (int i = 0; i < nCand; ++i)
        if (sCur.Compare(kCandidates[i]) == 0) { bCurInList = true; break; }

    int nSel = -1;
    int nIdx = 0;
    if (!bCurInList && !sCur.IsEmpty())
    {
        auto pItem = std::make_unique<CListLabelElementUI>(); // [C++17 R3]
        pItem->SetText(sCur.GetData());
        pCombo->Add(pItem.release()); // [C++17 R3] ownership transferred to DuiLib
        nSel = nIdx;
        ++nIdx;
    }

    for (int i = 0; i < nCand; ++i)
    {
        auto pItem = std::make_unique<CListLabelElementUI>(); // [C++17 R3]
        pItem->SetText(kCandidates[i]);
        pCombo->Add(pItem.release()); // [C++17 R3] ownership transferred to DuiLib
        if (sCur.Compare(kCandidates[i]) == 0) nSel = nIdx;
        ++nIdx;
    }

    if (nSel < 0 && pCombo->GetCount() > 0) nSel = 0;
    if (pCombo->GetCount() > 0) pCombo->SelectItem(nSel);
    return nSel;
}

// [R120-1] 应用单个区域格式
void CPageIntl::OnApplyFormat(int nKind, int nIdx)
{
    // 重新构建当前 Combo 的索引映射（候选列表）
    const wchar_t* pszComboName = nullptr;
    EIntlFormatKind kind = EIntlFormatKind::INTL_FMT_SHORTDATE;
    switch (nKind)
    {
    case 0: pszComboName = L"intl_shortdatecombo"; kind = EIntlFormatKind::INTL_FMT_SHORTDATE; break;
    case 1: pszComboName = L"intl_shorttimecombo"; kind = EIntlFormatKind::INTL_FMT_SHORTTIME; break;
    case 2: pszComboName = L"intl_decimalcombo";   kind = EIntlFormatKind::INTL_FMT_DECIMAL;   break;
    case 3: pszComboName = L"intl_currencycombo";  kind = EIntlFormatKind::INTL_FMT_CURRENCY;  break;
    default:
        m_ctx.HostToast(StringMgr.Get("intl.s_badfmt_short"),
                        StringMgr.Get("intl.s_badfmt_detail"));
        return;
    }

    // 从 Combo 文本直接取用户所选值
    CComboUI* pCombo = static_cast<CComboUI*>(m_ctx.HostFindControl(pszComboName));
    if (!pCombo || pCombo->GetCount() <= 0)
    {
        m_ctx.HostToast(StringMgr.Get("intl.s_badfmt_short"),
                        StringMgr.Get("intl.s_badfmt_detail"));
        return;
    }

    int nCur = pCombo->GetCurSel();
    if (nCur < 0 || nCur >= static_cast<int>(pCombo->GetCount()))
    {
        m_ctx.HostToast(StringMgr.Get("intl.s_badfmt_short"),
                        StringMgr.Get("intl.s_badfmt_detail"));
        return;
    }

    CControlUI* pElem = pCombo->GetItemAt(nCur);
    if (!pElem)
    {
        m_ctx.HostToast(StringMgr.Get("intl.s_badfmt_short"),
                        StringMgr.Get("intl.s_badfmt_detail"));
        return;
    }

    CDuiString sValue = pElem->GetText();
    if (sValue.IsEmpty())
    {
        m_ctx.HostToast(StringMgr.Get("intl.s_badfmt_short"),
                        StringMgr.Get("intl.s_badfmt_detail"));
        return;
    }

    // 确认写入
    CDuiString sConfirm;
    sConfirm.Format(StringMgr.Get("intl.s_apply_fmt_confirm"), sValue.GetData());
    if (!m_ctx.HostConfirmWarn(StringMgr.Get("intl.s_apply_fmt_confirm_title"),
                               sConfirm.GetData()))
        return;

    CDuiString sErr;
    EIntlWriteResult nRc = IntlService::SetLocaleFormat(kind, sValue.GetData(), sErr);
    if (nRc == EIntlWriteResult::INTL_WRITE_OK)
    {
        m_ctx.HostToast(StringMgr.Get("intl.s_apply_fmt_done_short"),
                        StringMgr.Get("intl.s_apply_fmt_done"));
        CDuiString sLog;
        sLog.Format(L"[intl] format applied kind=%d value=%s", static_cast<int>(nKind), sValue.GetData());
        m_ctx.HostLog(sLog.GetData());
    }
    else
    {
        m_ctx.HostToast(StringMgr.Get("intl.s_apply_fmt_fail_short"),
                        StringMgr.Get("intl.s_apply_fmt_fail"));
        CDuiString sLog;
        sLog.Format(L"[intl] format apply failed kind=%d value=%s rc=%d err=%s",
                    static_cast<int>(nKind), sValue.GetData(), static_cast<int>(nRc), sErr.GetData());
        m_ctx.HostLog(sLog.GetData());
    }
}

void CPageIntl::OnEvent(TNotifyUI& msg)
{
    // 区域/国际化页事件处理
    // [修复] 本块全部是 CComboUI；combo 选中发 DUI_MSGTYPE_ITEMSELECT
    //（"itemselect"），不是 Option 的 "selectchanged"。旧代码挂错消息导致
    // 时区/区域/语言/日期格式等 10 个下拉切换均不生效。
    if (msg.sType == DUI_MSGTYPE_ITEMSELECT)
    {
        OnComboItemSelect(msg);
    }
    else if (msg.sType == L"click")
    {
        const wchar_t* pszName = msg.pSender->GetName();
        if (lstrcmpiW(pszName, L"btn_intl_apply") == 0)
        {
            if (HandleApplyTimezone()) return;
        }
        // [R96] 应用用户区域
        else if (lstrcmpiW(pszName, L"btn_intl_apply_loc") == 0)
        {
            OnApplyUserLocale(m_nSelLocale);
        }
        // [R96] 应用系统区域（需管理员）
        else if (lstrcmpiW(pszName, L"btn_intl_apply_sysloc") == 0)
        {
            OnApplySystemLocale(m_nSelSysLocale);
        }
        // [R96] 应用 UI 语言（进程内即时生效 + 系统镜像）
        else if (lstrcmpiW(pszName, L"btn_intl_apply_uilang") == 0)
        {
            OnApplyUiLanguage(m_nSelUiLang);
        }
        // [R101] 安装语言
        else if (lstrcmpiW(pszName, L"btn_intl_add_lang") == 0)
        {
            OnAddInstalledLanguage();
        }
        // [R101] 删除语言
        else if (lstrcmpiW(pszName, L"btn_intl_remove_lang") == 0)
        {
            OnRemoveInstalledLanguage();
        }
        // [R120-1] 应用区域格式
        else if (lstrcmpiW(pszName, L"btn_intl_apply_shortdate") == 0)
        {
            OnApplyFormat(0, m_nSelShortDate);
        }
        else if (lstrcmpiW(pszName, L"btn_intl_apply_shorttime") == 0)
        {
            OnApplyFormat(1, m_nSelShortTime);
        }
        else if (lstrcmpiW(pszName, L"btn_intl_apply_decimal") == 0)
        {
            OnApplyFormat(2, m_nSelDecimal);
        }
        else if (lstrcmpiW(pszName, L"btn_intl_apply_currency") == 0)
        {
            OnApplyFormat(3, m_nSelCurrency);
        }
    }
}

// [R96] 应用用户区域（仅当前用户，无需管理员）
void CPageIntl::OnApplyUserLocale(int nIdx)
{
    std::vector<IntlLangEntry> vCand;
    IntlService::GetCandidateLocales(vCand);
    if (nIdx < 0 || nIdx >= static_cast<int>(vCand.size()))
    {
        m_ctx.HostToast(StringMgr.Get("intl.s_badloc_short"),
                        StringMgr.Get("intl.s_badloc_detail"));
        return;
    }

    CDuiString sName = vCand[nIdx].sName;
    CDuiString sConfirm;
    sConfirm.Format(StringMgr.Get("intl.s_apply_loc_confirm"), sName.GetData());
    if (!m_ctx.HostConfirmWarn(StringMgr.Get("intl.s_apply_loc_confirm_title"),
                               sConfirm.GetData()))
        return;

    CDuiString sErr;
    EIntlWriteResult nRc = IntlService::SetUserLocaleName(sName.GetData(), sErr);
    IntlReportWriteResult(nRc, sErr, sName,
                          "intl.s_apply_loc_done_short", "intl.s_apply_loc_done",
                          "intl.s_apply_loc_fail_short", "intl.s_apply_loc_fail",
                          L"[intl] user locale applied: %s", L"[intl] user locale failed: %s rc=%d err=%s");
}

// [R96] 应用系统区域（需管理员）
void CPageIntl::OnApplySystemLocale(int nIdx)
{
    std::vector<IntlLangEntry> vCand;
    IntlService::GetCandidateLocales(vCand);
    if (nIdx < 0 || nIdx >= static_cast<int>(vCand.size()))
    {
        m_ctx.HostToast(StringMgr.Get("intl.s_badloc_short"),
                        StringMgr.Get("intl.s_badloc_detail"));
        return;
    }

    CDuiString sName = vCand[nIdx].sName;
    CDuiString sConfirm;
    sConfirm.Format(StringMgr.Get("intl.s_apply_sysloc_confirm"), sName.GetData());
    if (!m_ctx.HostConfirmWarn(StringMgr.Get("intl.s_apply_sysloc_confirm_title"),
                               sConfirm.GetData()))
        return;

    CDuiString sErr;
    EIntlWriteResult nRc = IntlService::SetSystemLocaleName(sName.GetData(), sErr);
    if (nRc == EIntlWriteResult::INTL_WRITE_OK)
    {
        m_ctx.HostToast(StringMgr.Get("intl.s_apply_sysloc_done_short"),
                        StringMgr.Get("intl.s_apply_sysloc_done"));
        CDuiString sLog;
        sLog.Format(L"[intl] system locale applied: %s", sName.GetData());
        m_ctx.HostLog(sLog.GetData());
    }
    else if (nRc == EIntlWriteResult::INTL_WRITE_NO_PERMISSION)
    {
        m_ctx.HostToast(StringMgr.Get("intl.s_no_perm_short"),
                        StringMgr.Get("intl.s_no_perm"));
    }
    else
    {
        m_ctx.HostToast(StringMgr.Get("intl.s_apply_sysloc_fail_short"),
                        StringMgr.Get("intl.s_apply_sysloc_fail"));
        CDuiString sLog;
        sLog.Format(L"[intl] system locale failed: %s rc=%d err=%s",
                    sName.GetData(), static_cast<int>(nRc), sErr.GetData());
        m_ctx.HostLog(sLog.GetData());
    }
}

// [R96] 应用 UI 语言（进程内即时生效 + 系统镜像）
void CPageIntl::OnApplyUiLanguage(int nIdx)
{
    // UI 语言 Combo = 已安装语言 + 候选未安装项，重建索引映射
    std::vector<IntlLangEntry> vInstalled;
    IntlService::GetInstalledLanguages(vInstalled);

    std::vector<IntlLangEntry> vCand;
    IntlService::GetCandidateLocales(vCand);

    CDuiString sName;
    if (nIdx >= 0 && nIdx < static_cast<int>(vInstalled.size()))
        sName = vInstalled[nIdx].sName;
    else if (nIdx >= static_cast<int>(vInstalled.size()))
    {
        int nRemain = nIdx - static_cast<int>(vInstalled.size());
        int nFound = 0;
        for (size_t i = 0; i < vCand.size(); ++i)
        {
            if (vCand[i].bInstalled) continue;
            bool bInList = false;
            for (size_t k = 0; k < vInstalled.size(); ++k)
                if (vInstalled[k].sName.Compare(vCand[i].sName) == 0) { bInList = true; break; }
            if (bInList) continue;
            if (nFound == nRemain) { sName = vCand[i].sName; break; }
            ++nFound;
        }
    }

    if (sName.IsEmpty())
    {
        m_ctx.HostToast(StringMgr.Get("intl.s_badlang_short"),
                        StringMgr.Get("intl.s_badlang_detail"));
        return;
    }

    CDuiString sConfirm;
    sConfirm.Format(StringMgr.Get("intl.s_apply_uilang_confirm"), sName.GetData());
    if (!m_ctx.HostConfirmWarn(StringMgr.Get("intl.s_apply_uilang_confirm_title"),
                               sConfirm.GetData()))
        return;

    // 1) 系统镜像（写 HKLM，需管理员，失败不阻塞进程内生效）
    CDuiString sSysErr;
    EIntlWriteResult nRc = IntlService::SetSystemUILanguageName(sName.GetData(), sSysErr);

    // 2) 进程内即时生效
    bool bThread = IntlService::SetThreadUILanguageName(sName.GetData());

    if (bThread)
    {
        m_ctx.HostToast(StringMgr.Get("intl.s_apply_uilang_done_short"),
                        StringMgr.Get("intl.s_apply_uilang_done"));
        CDuiString sLog;
        sLog.Format(L"[intl] UI language applied: %s thread=1 sys_rc=%d",
                    sName.GetData(), static_cast<int>(nRc));
        m_ctx.HostLog(sLog.GetData());
    }
    else if (nRc == EIntlWriteResult::INTL_WRITE_NO_PERMISSION)
    {
        m_ctx.HostToast(StringMgr.Get("intl.s_no_perm_short"),
                        StringMgr.Get("intl.s_no_perm"));
    }
    else
    {
        m_ctx.HostToast(StringMgr.Get("intl.s_apply_uilang_fail_short"),
                        StringMgr.Get("intl.s_apply_uilang_fail"));
        CDuiString sLog;
        sLog.Format(L"[intl] UI language failed: %s sys_rc=%d thread=0",
                    sName.GetData(), static_cast<int>(nRc));
        m_ctx.HostLog(sLog.GetData());
    }
}
// [C++17 R3] 文件级内联常量（真实 C++17 inline variables）：
// 本页处理批次的超时/重试上限，供本文件内部使用；inline 保证多 TU 下 ODR 一致。
inline constexpr int kIntlStepTimeoutMs = 120000;
inline constexpr int kIntlMaxRetry = 3;

// [REFACTOR] 从 OnEvent 提取：区域/国际化页 9 个下拉框的 itemselect 处理。
// 全部是 CComboUI：combo 选中发 DUI_MSGTYPE_ITEMSELECT（"itemselect"），
// 不是 Option 的 "selectchanged"；旧代码挂错消息导致 10 个下拉切换均不生效。
// 各分支只写入对应的 m_nSel* 成员，无其它副作用。逐字搬迁零逻辑改动。
// [R167] 从 OnComboItemSelect 抽出：时区下拉选中处理。
//   [R90] 预选中即记录日志，**不立即写系统** —— 真正写入要等用户点"应用"。
//   这条语义容易在重构时丢：一旦改成"选中即写"，用户只是翻看一下下拉选项
//   就会改动系统时区，且没有任何确认步骤。
//   三道校验都静默返回：pSender 不是 combo、索引为负、键解析失败，
//   都属"用户还没选到有效项"，不是错误。
void CPageIntl::IntlOnTimezoneSelected(CControlUI* pSender)
{
    CComboUI* pCombo = static_cast<CComboUI*>(pSender);
    if (!pCombo) return;
    m_nSel = pCombo->GetCurSel();
    if (m_nSel < 0) return;

    CDuiString sKey, sOff;
    if (!m_ctx.DataEngine().GetTimeZone(m_nSel, sKey, sOff)) return;
    CDuiString sLog;
    sLog.Format(L"[intl] timezone selected: %s", sKey.GetData());
    m_ctx.HostLog(sLog.GetData());
}

void CPageIntl::OnComboItemSelect(TNotifyUI& msg)
{
    const wchar_t* pszName = msg.pSender->GetName();
    if (lstrcmpiW(pszName, L"intl_tzcombo") == 0)
    {
        // [R167] 时区选中处理抽为 IntlOnTimezoneSelected，嵌套 5→3
        IntlOnTimezoneSelected(msg.pSender);
    }
    // [R96] 用户区域 Combo
    else if (lstrcmpiW(pszName, L"intl_loccombo") == 0)
    {
        CComboUI* pCombo = static_cast<CComboUI*>(msg.pSender);
        if (pCombo) m_nSelLocale = pCombo->GetCurSel();
    }
    // [R96] 系统区域 Combo
    else if (lstrcmpiW(pszName, L"intl_sysloccombo") == 0)
    {
        CComboUI* pCombo = static_cast<CComboUI*>(msg.pSender);
        if (pCombo) m_nSelSysLocale = pCombo->GetCurSel();
    }
    // [R96] UI 语言 Combo
    else if (lstrcmpiW(pszName, L"intl_uilangcombo") == 0)
    {
        CComboUI* pCombo = static_cast<CComboUI*>(msg.pSender);
        if (pCombo) m_nSelUiLang = pCombo->GetCurSel();
    }
    // [R101] 已安装语言 Combo
    else if (lstrcmpiW(pszName, L"intl_instlangcombo") == 0)
    {
        CComboUI* pCombo = static_cast<CComboUI*>(msg.pSender);
        if (pCombo) m_nSelInstLang = pCombo->GetCurSel();
    }
    // [R120-1] 系统区域格式四个 Combo
    else if (lstrcmpiW(pszName, L"intl_shortdatecombo") == 0)
    {
        CComboUI* pCombo = static_cast<CComboUI*>(msg.pSender);
        if (pCombo) m_nSelShortDate = pCombo->GetCurSel();
    }
    else if (lstrcmpiW(pszName, L"intl_shorttimecombo") == 0)
    {
        CComboUI* pCombo = static_cast<CComboUI*>(msg.pSender);
        if (pCombo) m_nSelShortTime = pCombo->GetCurSel();
    }
    else if (lstrcmpiW(pszName, L"intl_decimalcombo") == 0)
    {
        CComboUI* pCombo = static_cast<CComboUI*>(msg.pSender);
        if (pCombo) m_nSelDecimal = pCombo->GetCurSel();
    }
    else if (lstrcmpiW(pszName, L"intl_currencycombo") == 0)
    {
        CComboUI* pCombo = static_cast<CComboUI*>(msg.pSender);
        if (pCombo) m_nSelCurrency = pCombo->GetCurSel();
    }
}

// [REFACTOR] 从 OnEvent 提取：btn_intl_apply（应用时区）点击处理。
// 原块内三处 return; 表示本条已消费、不再走后续 else if 分支，
// 提取后改为返回 true；调用点写 if (HandleApplyTimezone()) return;
// 语义完全等价。逐字搬迁其余逻辑。
bool CPageIntl::HandleApplyTimezone()
{
        if (m_nSel < 0)
        {
            m_ctx.HostToast(StringMgr.Get("intl.s_badsel"),
                            StringMgr.Get("intl.s_badsel_detail"));
            return true;
        }

        CDuiString sKey, sOff;
        if (!m_ctx.DataEngine().GetTimeZone(m_nSel, sKey, sOff))
        {
            m_ctx.HostToast(StringMgr.Get("intl.s_badsel"),
                            StringMgr.Get("intl.s_badsel_detail"));
            return true;
        }

        // [R90] 确认后写系统时区（IntlService::SetTimeZone）
        CDuiString sConfirm;
        sConfirm.Format(StringMgr.Get("intl.s_apply_confirm"), sKey.GetData());
        if (!m_ctx.HostConfirmWarn(StringMgr.Get("intl.s_apply_confirm_title"),
                                   sConfirm.GetData()))
            return true;

        CDuiString sErr;
        EIntlWriteResult nRc = IntlService::SetTimeZone(sKey.GetData(), sErr);
        if (nRc == EIntlWriteResult::INTL_WRITE_OK)
        {
            m_ctx.HostToast(StringMgr.Get("intl.s_apply_done_short"),
                            StringMgr.Get("intl.s_apply_done"));
            CDuiString sLog;
            sLog.Format(L"[intl] timezone applied: %s", sKey.GetData());
            m_ctx.HostLog(sLog.GetData());
        }
        else if (nRc == EIntlWriteResult::INTL_WRITE_NO_PERMISSION)
        {
            m_ctx.HostToast(StringMgr.Get("intl.s_no_perm_short"),
                            StringMgr.Get("intl.s_no_perm"));
        }
        else
        {
            m_ctx.HostToast(StringMgr.Get("intl.s_apply_fail_short"),
                            StringMgr.Get("intl.s_apply_fail"));
            CDuiString sLog;
            sLog.Format(L"[intl] timezone apply failed: %s rc=%d err=%s",
                        sKey.GetData(), static_cast<int>(nRc), sErr.GetData());
            m_ctx.HostLog(sLog.GetData());
        }
    return true;
}
