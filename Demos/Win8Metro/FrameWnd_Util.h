#pragma once

#include "stdafx.h"

// ============================================================
// FrameWnd_Util  —— C1 拆分试点（R27）
// 抽取 FrameWnd.cpp 中的纯 Win32 UI 工具函数（不依赖 DuiLib 控件类）。
// 目标：把 FrameWnd.cpp 的 static 函数按耦合度分层拆分。
// [B5] 用 [[nodiscard]] 标注返回型工具函数以提级为 C++17（脚本 C++17 特征之一）。
// ============================================================

// 剪贴板：把 pszText 写入系统剪贴板（CF_UNICODETEXT）
// 失败静默（不修改剪贴板状态），返回是否成功。
// 完整异常处理：GlobalLock 失败释放、SetClipboardData 失败手动 GlobalFree。
// [B5] 有意不加 [[nodiscard]]：大量调用点为"尽力而为"复制（如复制错误详情到剪贴板），
//      返回值被有意忽略，加 nodiscard 会在 9 处产生 C4834 告警（违背告警不增硬约束）。
bool FrameWnd_Util_CopyTextToClipboard(HWND hwnd, const wchar_t* pszText);

// ---- P2：跨页复用 UI helper（实现暂留 FrameWnd.cpp，后续随 FrameWnd_UiHelpers 切片迁出）----
// 按名读取编辑框文本（不存在返回空串）
[[nodiscard]] DuiLib::CDuiString ReadEditValue(DuiLib::CPaintManagerUI& pm, const wchar_t* pszName);
// 按折叠体内可见子项重算固定高度（展开/收起/重排后调用）
void RecalcBodyHeight(DuiLib::CVerticalLayoutUI* pBody);
// [PERF] 通知启动预热线程让路（页面已构建，用户正在等待真实枚举）。
// 定义在 FrameWnd_SysMsg.cpp；幂等，多次调用无副作用。
// 各页在发起真实枚举前调用，避免与后台预热争抢共享 DISM 会话锁。
void NotifyWarmupYield();
// 按前缀"名字_数字"解析行序号（不匹配返回 false）
[[nodiscard]] bool ParsePrefix1(const DuiLib::CDuiString& sName, const wchar_t* pszPrefix, int& a);
// P3b：复选框统一样式（文本前拼空框字形）；去 static 供各 Page 共享
void StyleCheckBox(DuiLib::CCheckBoxUI* p, const wchar_t* pszText, int nHeight, const wchar_t* pszGlyphEmpty);

// ---- P4：Appx/可选功能/更新页共享的纯字符串辅助（实现仍在 FrameWnd.cpp）----
// 去首尾空白
[[nodiscard]] DuiLib::CDuiString TrimStr(const DuiLib::CDuiString& s);
// Appx family = "<name>_<publisher>"（首尾下划线分词）
[[nodiscard]] DuiLib::CDuiString AppxFamily(const DuiLib::CDuiString& sFull);
// [I18N-APPX-NAME] Appx 显示名：按 PackageName 查 appx.name.<PackageName> 语言键。
//   不含任何写死文案；未命中返回空串，调用方回退到原始包名。
[[nodiscard]] DuiLib::CDuiString AppxDisplayName(const DuiLib::CDuiString& sFull);
// Capability Identity 人类可读行标题
[[nodiscard]] DuiLib::CDuiString PrettyCapName(const DuiLib::CDuiString& sIdentity);
// WUA 错误码本地化文案
[[nodiscard]] const wchar_t*             WuaLocalizeErrorCode(int nErr);
// 按更新标题判定类型（驱动/其它）
[[nodiscard]] int                 UpdateTypeOf(const DuiLib::CDuiString& sTitle);

// P2c：图标缓存（实现暂留 FrameWnd.cpp，关联页/驱动页共享）
// 确保 %LOCALAPPDATA%\DismTool_Clone\icons 目录存在（失败静默）
void EnsureIconCacheDirectory();
// 目标命中缓存时返回缓存 png 路径，否则返回空串（不做生成，仅缓存探测）
[[nodiscard]] DuiLib::CDuiString EnsureIconPngReady(const wchar_t* pszTarget);

// ---- P-P1（新版移植）：临时目录访问器 ----
// 等价原 CFrameWnd::GetTempDir；返回带尾随反斜杠的临时目录，
// GetTempPath 失败时回退 L"%TEMP%"（与原实现一致）。
[[nodiscard]] DuiLib::CDuiString FrameWnd_Util_GetTempDir();

// 逐级创建目录（CreateDirectory 不能一次建多级）。
// 已存在视为成功；返回最终目录是否存在。
// [B5] 有意不加 [[nodiscard]]：既有调用点多为"确保目录存在"意图忽略返回值（如 Page_Program/Page_Unattend），
//      加 nodiscard 会产生 C4834 告警（违背告警不增硬约束）；仅 FrameWnd.cpp:3551 消费了返回值。
bool FrameWnd_Util_EnsureDirectoryDeep(const wchar_t* pszDir);
