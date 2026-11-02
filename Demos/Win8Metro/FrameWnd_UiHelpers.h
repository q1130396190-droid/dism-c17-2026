#pragma once

#include "stdafx.h"
#include "HostInterfaces.h"   // IHostUi（struct，勿再前向声明成 class）
#include <functional>         // [AUDIT-F3] 全选/反选回调

// ============================================================
// FrameWnd_UiHelpers —— P8 收尾切片：多页共享的 UI 小工具
//
// 来源：原 FrameWnd.cpp 文件级 static / CFrameWnd 成员，P8 两级
// 路由落地后被多个 Page 的 OnEvent / OnContextMenu 共用，按方案
// 第 8 节文件清单集中于此。纯函数 + 纯 UI 辅助，不含业务状态。
// ============================================================

// [B1] 通用列表右键菜单种类（原 CFrameWnd::EListCtx）
// nKind: 0=CLEAN, 1=UPDATE, 2=APPX, 3=ASSOC_IMG, 4=ASSOC_LIVE,
//        5=PROGRAM, 6=DRV, 7=CAP, 8=OTHER
enum class EListCtx : int {
    LCX_CLEAN = 0,
    LCX_UPDATE,
    LCX_APPX,
    LCX_ASSOC_IMG,
    LCX_ASSOC_LIVE,
    LCX_PROGRAM,
    LCX_DRV,
    LCX_CAP,
    LCX_OTHER,
    LCX_REGCLEAN     // [R103] 注册表清理行（详情 + 复制规则名）
};

// 按前缀"名字_数字_数字"解析两个行序号（不匹配返回 false）。
// 原 FrameWnd.cpp static ParsePrefix2。
bool ParsePrefix2(const DuiLib::CDuiString& sName, const wchar_t* pszPrefix, int& a, int& b);

// 解析优化规则行名 sw_<g>_<i>_<scope>（不匹配返回 false）。
// 原 FrameWnd.cpp static ParseSwName。
bool ParseSwName(const wchar_t* pszName, const wchar_t* pszPrefix,
                 int& nGroup, int& nItem, int& nScope);

// [B1] 通用列表右键菜单：按列表种类决定可用项，菜单项当前仅
// 日志/复制提示/跳转商店或更新设置，无直接破坏性动作（破坏性操作
// 在各页按钮链路中，均带二次确认）。host 提供日志与宿主窗口。
void UiHelpers_ShowUnifiedListContextMenu(IHostUi* host, HWND hwnd,
                                         EListCtx nKind, int nIdx, POINT pt);

// [R85 P1-13] 带路径参数版本：供需要"打开所在位置"的页面（如 Cleanup）使用。
// pszPath 非空时追加 IDC_OPEN_EXPLORER 菜单项，选中后调用
// UiHelpers_OpenExplorerSelect 定位文件。
// [AUDIT-F3] fnSelectAll/fnInvert：LCX_CLEAN 的"全选/反选"菜单项回调。
// 原实现只写日志无动作（死菜单）；传入回调后点击即执行页面真实动作，
// 缺省为空时保持旧日志行为（其余调用点零改动）。
// [AUDIT-G2] fnDetail：真实详情回调；未传时「详情」项不装配（假动作屏蔽）。
// [AUDIT-G1] 删除/修复/重置仅在 nIdx >= 0（命中具体行）时装配。
void UiHelpers_ShowUnifiedListContextMenuPath(IHostUi* host, HWND hwnd,
                                              EListCtx nKind, int nIdx, POINT pt,
                                              const wchar_t* pszPath,
                                              const std::function<void()>& fnSelectAll = std::function<void()>(),
                                              const std::function<void()>& fnInvert = std::function<void()>(),
                                              const std::function<void()>& fnDetail = std::function<void()>());

// [R85 P1-13] 打开"所在位置"：用 GetFullPathNameW 规范化路径后，
// ShellExecuteW(explorer /select, path) 在资源管理器中定位该文件/目录。
// sPath 为空或不存在时仅记日志，不做任何打开动作。返回 true 表示已发起。
bool UiHelpers_OpenExplorerSelect(HWND hwnd, const wchar_t* pszPath);
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：

// [R127-A] 依赖图对话框：Compose 组件图 -> 转 toolsvc DTO -> 模态展示。
//   原样搬迁自 Page_AppxCtx::ShowDepGraph 与 Page_Startup::ShowServiceDepGraph
//   两份逐字相同实现（18 行）。sRoot 为离线镜像根，vDeps 为依赖名列表；
//   host 用于取宿主窗口与 DPI。不返回值：展示失败在 DependencyGraphService
//   内部已记日志，调用方各自负责自己的收尾日志。
void UiHelpers_ShowDepGraphDialog(IHostUi* host, const wchar_t* pszRoot,
                                  const std::vector<DuiLib::CDuiString>& vDeps);

// [R127-A] 行内子控件关键词命中判定。
//   从一行（CContainerUI）的全部可见子控件文本中查找 sKey（调用方已 MakeLower）。
//   原样搬迁自 Page_Startup::ApplySvcFilter 与 Page_Tasks::ApplyFilter 的
//   两份逐字相同实现（唯一差异是局部变量名）。sKey 为空时调用方直接判 true，
//   本函数不再重复判空，避免两处语义分叉。
//   pRow 为 nullptr 或不是 ContainerUI 时返回 false。
[[nodiscard]] bool UiHelpers_RowTextHitsKey(DuiLib::CControlUI* pRow,
                                           const DuiLib::CDuiString& sKey);

