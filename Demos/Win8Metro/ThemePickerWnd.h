#pragma once

// ============================================================================
//  ThemePickerWnd.h —— 标题栏主题下拉（带皮肤预览）
//
//  [2026] 动态预设：从 ThemeService::GetPresets() 读预设列表，
//   不再硬编码 4 项。drop skin\Foo\ 文件夹即可在下拉里看到新项。
//   选中回调传 preset name（wstring），由 ThemeService::ApplyTheme 统一处理。
//
//  为什么是自绘窗口，而不是 DuiLib 的 CComboUI / 原生 TrackPopupMenu：
//    · 原生菜单画不了"色块预览"，只能给文字；
//    · CComboUI 的下拉列表项是单行文本控件，塞不进"标题 + 三色块 + 勾选"
//      这种多元素卡片，而且它的弹出层由父窗口坐标驱动，挂在标题栏这种
//      37px 高的横条上会被裁切。
//    所以这里用一个独立的 WS_POPUP 子窗口自绘：尺寸完全可控，
//    失焦自动关闭，不参与主窗口布局，也不影响页面栈。
// ============================================================================

#include "stdafx.h"
#include "ThemeTokens.h"
#include "ThemeService.h"   // [2026] GetPresets() / EffectiveAccent()

#include <functional>
#include <string>
#include <vector>

class CThemePickerWnd : public CWindowWnd
{
public:
    // [2026] 选中回调：参数是 preset name（如 L"classic" / L"dark"）
    using FnOnPick = std::function<void(const std::wstring& presetName)>;

    CThemePickerWnd();
    ~CThemePickerWnd();

    // anchorHwnd: 父窗口（标题栏所属主窗口），用来定位 + 失焦判定
    // rcAnchor  : 主题按钮在**屏幕**坐标下的矩形，下拉从这里往下展开
    // nDpi      : 跟随主窗口 DPI，避免高分屏上文字和色块比例失真
    bool Create(HWND anchorHwnd, const RECT& rcAnchor, int nDpi, FnOnPick fnOnPick);

    // 单实例守卫
    [[nodiscard]] static bool IsOpen();
    static void CloseOpen();

    [[nodiscard]] int ItemCount() const { return static_cast<int>(m_vItems.size()); }

    LPCTSTR GetWindowClassName() const override;
    void    OnFinalMessage(HWND hWnd) override;
    LRESULT HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) override;

private:
    struct TItem
    {
        std::wstring name;           // preset name （如 "classic" / "dark" / "neon"）
        std::wstring displayName;    // 显示名（如 "经典蓝" / "深色" / "Neon"）
        std::wstring subtitle;       // 副标题（一行说明）
        // 预览块渲染参数
        bool   bDarkPreview;         // 按深色还是浅色取令牌（用于 Dynamic Light/Dark）
        DWORD  crAccentOverride;     // 预设强调色（Classic 有固定深蓝，其余从系统取）
        bool   bHasPalette;         // true 时预览块直接用 palette 颜色
        ThemeService::ThemePalette palette;   // palette 拷贝（当 bHasPalette=true）
    };

    void BuildItems();
    void Layout();
    void OnPaint(HDC hDC);
    void OnMouseMove(POINT pt);
    void OnLButtonUp(POINT pt);

    [[nodiscard]] int  ItemAtY(int y) const;
    [[nodiscard]] int  ItemHeight() const;
    [[nodiscard]] RECT ItemRect(int i) const;

    HWND          m_hAnchor = nullptr;
    FnOnPick      m_fnOnPick;
    int           m_nDpi = 96;
    std::vector<TItem> m_vItems;
    int           m_nHot = -1;
    int           m_nCur = -1;
    int           m_nPadY = 6;
    int           m_nWidth = 0;

    // 待提交的选择：Close 只是 PostMessage(WM_CLOSE)，回调推迟到 OnFinalMessage
    bool          m_bPicked = false;
    std::wstring  m_sPicked;

    [[nodiscard]] int Dp(int v) const { return MulDiv(v, m_nDpi, 96); }
};
