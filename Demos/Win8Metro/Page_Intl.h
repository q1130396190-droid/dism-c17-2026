#pragma once

// ============================================================
// Page_Intl —— 区域/国际化页（新版 Q4 功能移植）
//
// 承接原 CFrameWnd::BuildIntlPage：
//   时区 Combo + 当前时区检测（Data.xml TimeZoneMap 消费）
//   + [R90] 应用时区按钮（IntlService::SetTimeZone 写路径）
//   + [R96] 用户区域 / 系统区域 / UI 语言 Combo 接线
//     （IntlService::GetCandidateLocales / GetInstalledLanguages
//       + SetUserLocaleName / SetSystemLocaleName / SetSystemUILanguageName）
//
// 物理搬迁护栏：函数体零逻辑改动，宿主引用一律改 m_ctx.Xxx。
// ============================================================

#include "PageBase.h"
#include "IntlService.h"

class CPageIntl : public CPageBase
{
public:
    explicit CPageIntl(IPageContext& ctx) : CPageBase(ctx),
        m_nSel(-1), m_nSelLocale(-1), m_nSelSysLocale(-1), m_nSelUiLang(-1),
        m_nSelInstLang(-1), m_nSelShortDate(-1), m_nSelShortTime(-1),
        m_nSelDecimal(-1), m_nSelCurrency(-1) {}

    void Build() override { BuildPage(); }

    // P8：第二级事件路由（原壳 Notify 本页分支）
    void OnEvent(TNotifyUI& msg) override;
    // [REFACTOR] 从 OnEvent 提取：9 个下拉框（CComboUI）的 itemselect 处理，
    // 各分支只写入对应的 m_nSel* 成员。
    void OnComboItemSelect(TNotifyUI& msg);

    // [R167] 从 OnComboItemSelect 抽出：时区选中处理（预选中只记日志，不写系统）
    void IntlOnTimezoneSelected(CControlUI* pSender);
    // [REFACTOR] 从 OnEvent 提取：btn_intl_apply（应用时区）点击处理。
    // 返回 true 表示本条事件已消费（原块内 return; 语义），调用点据此提前返回。
    bool HandleApplyTimezone();

    void BuildPage();   // 原 BuildIntlPage

    const wchar_t* PageName() const override { return L"Intl"; }
private:
    // [R96] 填充用户区域 / 系统区域 / UI 语言三个 Combo
    void BuildLocaleCombos();

    // [R101] 填充候选可安装语言 Combo（排除已安装）
    void BuildAddLangCombo();

    // [R101] 填充已安装语言 Combo（来自 HKCU\User Profile\Languages）
    void BuildInstalledLangCombo();

    // [R96] 应用用户区域
    void OnApplyUserLocale(int nIdx);
    // [R96] 应用系统区域（需管理员）
    void OnApplySystemLocale(int nIdx);
    // [R96] 应用 UI 语言（进程内即时生效 + 系统镜像）
    void OnApplyUiLanguage(int nIdx);

    // [AUDIT-P2] 三处「写入结果上报」逐字同构（各 17 行），合并为本函数。
    // 成功/失败各弹 Toast 并写日志；失败日志带 rc 与 err。
    void IntlReportWriteResult(EIntlWriteResult nRc, const CDuiString& sErr,
                               const CDuiString& sName,
                               const char* pszDoneShort, const char* pszDone,
                               const char* pszFailShort, const char* pszFail,
                               const wchar_t* pszLogOk, const wchar_t* pszLogFail);

    // [R101] 安装所选语言（追加到用户语言列表）
    void OnAddInstalledLanguage();
    // [R101] 删除所选语言（从用户语言列表移除）
    void OnRemoveInstalledLanguage();

    // [R120-1] 系统区域格式：填充短日期/短时间/十进制/货币四个 Combo
    void BuildFormatCombos();

    // [R120-1] 应用单个区域格式
    void OnApplyFormat(int nKind, int nIdx);

    // [R120-1] 填充指定格式 Combo，返回当前选中索引
    int FillFormatCombo(const wchar_t* pszComboName, EIntlFormatKind nKind);

    // 当前索引（0 基），-1 未选
    int m_nSel;
    int m_nSelLocale;
    int m_nSelSysLocale;
    int m_nSelUiLang;
    int m_nSelInstLang;  // [R101] 已安装语言 Combo 索引

    // [R120-1] 四个区域格式 Combo 当前索引
    int m_nSelShortDate;
    int m_nSelShortTime;
    int m_nSelDecimal;
    int m_nSelCurrency;
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
