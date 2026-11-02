#pragma once

// ============================================================
// Page_Welcome —— 欢迎页（新版 Q15 功能移植）
//
// 承接原 CFrameWnd::BuildWelcomePage：
//   版本信息 + 快速入口 + 使用提示（原版首页）
//
// 物理搬迁护栏：函数体零逻辑改动，宿主引用一律改 m_ctx.Xxx。
// ============================================================

#include "PageBase.h"

class CPageWelcome : public CPageBase
{
public:
    explicit CPageWelcome(IPageContext& ctx)
        : CPageBase(ctx), m_dwOverviewErr(0), m_dwNotifyErr(0),
          m_nFailCount(0) {}

    void Build() override { BuildPage(); }

    // P8：第二级事件路由（原壳 Notify 本页分支）
    void OnEvent(TNotifyUI& msg) override;

    void BuildPage();

    // [R162] 从 BuildPage 抽出（原 224 行，全站最长函数）
    [[nodiscard]] std::unique_ptr<CHorizontalLayoutUI> BuildTitleBar();
    void BuildBrandBanner(CVerticalLayoutUI* pBox);
    void BuildOverviewCards(CVerticalLayoutUI* pBox);
    void BuildEntryCards(CVerticalLayoutUI* pBox);   // 原 BuildWelcomePage

    // [R97] 欢迎卡片点击跳转：操作系统→Optimization、内存→Cleanup、通知→Update
    void OnCardClick(const wchar_t* pszCardName);

    const wchar_t* PageName() const override { return L"Welcome"; }
    // [R231] Colours only -- the cards hold live readings, rebuilding would blank them.
    void RefreshThemeColors() override;
private:
    // [R111] 最近一次采集的失败码（BuildPage 时更新，供刷新按钮分级提示）
    DWORD m_dwOverviewErr;   // GetSystemOverview 失败码（0=成功）
    DWORD m_dwNotifyErr;     // GetUserNotificationState 失败码（0=成功）
    // [R124-3] 分级失败明细：按 API 收集的失败列表 + 失败 API 个数
    CDuiString m_sFailList;
    int        m_nFailCount;
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
