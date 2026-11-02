#pragma once

// ============================================================
// Page_Control —— 控制面板页（新版 Q11 功能移植）
//
// 承接原 CFrameWnd::BuildControlPage：
//   系统控制面板小程序列表（原版导航组还原）
//   + [R91] HKLM NameSpace 动态枚举 + SHLoadIndirectString 资源解析
//     + canonical → control:// 打开方式
//
// 物理搬迁护栏：函数体零逻辑改动，宿主引用一律改 m_ctx.Xxx。
// ============================================================

#include <map>
#include <vector>
#include "PageBase.h"
#include "ControlService.h"   // [R91] CtrlAppletEntry 动态枚举

class CPageControl : public CPageBase
{
public:
    explicit CPageControl(IPageContext& ctx) : CPageBase(ctx) {}

    void Build() override { BuildPage(); }

    // P8：第二级事件路由（原壳 Notify 本页分支）
    void OnEvent(TNotifyUI& msg) override;

    void BuildPage();   // 原 BuildControlPage

    // [R162] 从 BuildPage 抽出：构建一行控制面板项（主文案 + canonical 副文本）。
    //   兜底分支与动态分支原先各有一份约 25 行、逐字相同的行构建代码。
    // [R164] 从 ShowCtrlContextMenu 抽出：读取行显示名（嵌套 5→4）
    [[nodiscard]] CDuiString CtrlRowDisplayText(int nRow);

    // [R164] 从 ApplyCtrlFilter 抽出：[R109] 两口径匹配（显示名优先、canonical 补位）
    [[nodiscard]] bool ClRowHitByName(CControlUI* pRow, const CDuiString& sKwLow);
    [[nodiscard]] bool ClRowHitByCanon(const wchar_t* pszRowName, const CDuiString& sKwLow);

    // [R164] 从 ShowCtrlContextMenu 抽出：菜单分支主体（嵌套 5→2）
    void CtrlOpenApplet(bool bRunAs, const CDuiString& sCanon);
    void CtrlCopyField(const CDuiString& sValue, const char* pszCopiedKey,
                       bool bRequireNonEmpty);

    void AppendAppletRow(CVerticalLayoutUI* pList, int nIndex,
                         const CDuiString& sDisplay, const CDuiString& sCanonical);

    // [R97] 控制面板项名称实时过滤
    void ApplyCtrlFilter();

    // [R99] 控制面板右键菜单
    bool OnContextMenu(POINT pt, bool bFromContextMsg) override;
    void ShowCtrlContextMenu(int nRow, POINT pt);

    // [R99] 打开首个可见行（btn_ctrl_open）
    void OpenFirstVisible();

    const wchar_t* PageName() const override { return L"Control"; }
    // [R232] Colours only -- the list comes from a fixed fallback array.
    void RefreshThemeColors() override;
private:
    std::map<CDuiString, CDuiString> m_mapCtrlCanon;  // 显示名 → canonical 名称
    std::vector<CtrlAppletEntry>  m_vApplets;      // [R91] 动态枚举缓存
    std::vector<CDuiString>          m_vCanonical;    // [R91] 行号→canonical 兜底
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
