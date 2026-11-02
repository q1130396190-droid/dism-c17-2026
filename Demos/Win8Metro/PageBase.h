#pragma once

// ============================================================
// PageBase —— v2 Page 层基类（P2 首次落地 / P8 两级路由收口）
//
// 硬规则：
//   * Page 只持有 IPageContext&，永远拿不到 CFrameWnd&；
//   * Page 是 UI 层：允许 include UIlib.h / 使用控件与 StringMgr；
//   * Page 禁止注册表/进程/文件枚举等系统调用，那些归 Service；
//   * 构造只保存宿主引用，不做任何调用（页对象为核心壳的值成员，
//     构造早于 InitWindow，此时宿主窗口尚未就绪）。
//
// 生命周期：核心壳在首次切到该页时调 Build()（懒构建）。
//
// P8 两级路由：核心壳 Notify 只保留标题栏/焦点/全局控件，其余
// click/dbclick/textchanged/itemselect/selectchanged 一律原样
// 进本页 OnEvent；右键菜单命中测试归 OnContextMenu；Delete 键
// 归 OnDeleteKey。不匹配的控件名页内静默忽略（该控件只可能存在
// 于当前页，转发等价于旧壳内逐条精确名比较）。
// ============================================================

#include "UIlib.h"

#include "HostInterfaces.h"

#include <string>
#include <vector>

// content_area TabLayout 页序（原 CFrameWnd::PageIndex，P8 提升为全局，
// 壳与各页共用同一常量，禁止再私自定义副本）。
// [B4-F7] enum class：消除非限定枚举泄漏到整条 int 通路。
// PAGE_NONE=-1 为"无目标页"哨兵（原分散的 iPage=-1/iPage<0 语义，统一收敛至此）。
enum class EPageId : int {
    PAGE_NONE    = -1,
    PAGE_CLEANUP  = 0,
    PAGE_STARTUP  = 1,
    PAGE_APPX     = 2,
    PAGE_TOOLS    = 3,
    PAGE_OPTIM    = 4,
    PAGE_DRIVER   = 5,
    PAGE_PROGRAM  = 6,
    PAGE_FEATURE  = 7,
    PAGE_UPDATE   = 8,
    PAGE_ASSOC    = 9,
    PAGE_UNATTEND = 10,
    PAGE_TASKS    = 11,
    PAGE_CONTROL  = 12,
    PAGE_INTL     = 13,
    PAGE_REGCLEAN = 14,
    PAGE_WELCOME  = 15
};

class CPageBase
{
public:
    explicit CPageBase(IPageContext& ctx) : m_ctx(ctx) {}
    virtual ~CPageBase() {}

    // 首次进入页面时构建全部控件（原 CFrameWnd::BuildXxxPage）
    virtual void Build() = 0;

    // ---- [R3] 壳侧按页 switch 收口为虚函数（页描述符表 PageById 驱动）----
    // F5/刷新语义（原 CFrameWnd::RefreshCurrentPage 的 6-case switch）：
    // 默认等价 Build()；刷新动作不同的页 override（Appx=重新枚举+日志、
    // Capability=BuildList、Tools/Update=仅日志、Tasks=无动作）。
    virtual void OnRefreshPage() { Build(); }
    // 首次懒构建（原 CFrameWnd::EnsurePageBuilt 的 6-case switch）：
    // 默认等价 Build()；Cleanup 覆写为空（该页不在首次切页时构建），
    // Startup/Appx/Tools/Update 覆写各自的懒构建序列。
    virtual void OnDeferredBuild() { Build(); }
    // DPI 改变 / 最大化还原后，手算固定尺寸三页（Cleanup/Optim/Startup）
    // 的动态控件树整树重建入口（原 WM_APP+100/+101 里按页调用）；
    // 其余页默认空（+101 走根控件 NeedUpdate）。
    virtual void OnRebuildDynamicTree() {}
    // [R220] Re-apply theme colours to controls this page created in code.
    //   Why a page hook and not a tree walk: a control built with
    //   `SetTextColor(tokens::Token(Ink))` stores the resolved ARGB, so after a
    //   light<->dark switch it keeps painting the old colour until something
    //   re-derives it. A generic tree walk cannot tell WHICH token an ARGB came
    //   from (several pages pick colours by semantics: installed->green,
    //   pending->accent, risk level->danger/warn/success), so only the page itself
    //   can recompute correctly.
    //
    //   CONTRACT: may only re-set colour attributes on controls it already owns.
    //   It must NOT call Build(), add or remove children: doing so would discard
    //   list contents, selection and scroll position, and leaves m_pFocusCtrl
    //   dangling (that is the failure mode of the full-rebuild approach).
    virtual void RefreshThemeColors() {}
    // 本页是否属于上述手算固定尺寸页（最大化/还原后需整树重建而非
    // 仅 NeedUpdate；同时决定重建前是否失效焦点/输出缓存）。
    virtual bool NeedsFullRebuildOnRelayout() const { return false; }

    // ---- [R4] 壳→页命令收口（CP-1 直调；替代壳内 m_pgXxx.OnYyy 多点）----
    // 工具箱具名对话框服务（TD_* 枚举见 ToolDialog.h）；仅工具箱页覆写，
    // 壳菜单/CLI/拖放/窄接口统一经 PageById(PAGE_TOOLS) 调用。
    virtual void HostRunToolDialog(EToolDlgKind /*nKind*/) {}
    // 壳命令入口：主菜单命令 ID（1101..1205 裸 ID，与加速键 ID_ACCEL_*）、
    // 加速键、CLI 动作经此转发；页消费返回 true，不识别返回 false。
    virtual bool OnHostCommand(int /*id*/) { return false; }

    // ---- [R5] 后台线程 → 内容页异步消息统一入口 ----
    // 壳 FrameWnd_PageMsg.cpp 的 kPageMsgTab 按"消息→页序"经 PageById 调本
    // 虚函数；各页 switch(uMsg) 消费自己的 WM_*（契约见 FrameWnd_Messages.h）。
    // 堆所有权仍按壳表 EPageMsgOwn 固化：kMsgOwnPage 由本页处理函数内
    // delete；kMsgOwnShell 页只读取，释放仍由壳 ReleaseMsgParam 单出口执行。
    // 含壳编排的 WM_CLEAN_DONE 不走本入口（见下方 HostOnCleanDone 两段时序）。
    virtual bool OnAsyncMessage(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/)
        { return false; }

    // ---- [R5] 壳自建 WM_TIMER 按页分发（定时器 ID 见 FrameWnd_Messages.h）----
    // 壳 kTimerTab 中 iPage>=0 的定时器直接送本虚函数；一次性看门狗定时器
    // 页内须自行 ::KillTimer(m_ctx.HostHwnd(), uId)，与原壳 handler 等价。
    virtual void OnHostTimer(UINT /*uId*/) {}

    // ---- [R5] Windows 功能子视图动作（PAGE_FEATURE 页序由 CPageCapability
    // 代理；其余页默认空）。prog_pages Tab1 的 UI 物理上属于程序页，程序页
    // 经 HostPageHost()->PageById(PAGE_FEATURE) 调下列虚函数。----
    virtual void HostFeatureTabChanged(int /*idx*/) {}
    virtual void HostFeatureLoad() {}
    virtual void HostFeatureDeleteSelected() {}
    virtual void HostFeatureApplyFilter(const DuiLib::CDuiString& /*sKeyword*/,
                                        bool /*bNoKw*/) {}
    // [修复] prog_pages Tab1 的功能行/按钮事件与右键菜单物理上寄宿在程序
    // 页，壳只把消息投递给程序页；程序页须把未消费的事件/右键转发到本
    // 代理（Capability → Feature 子视图），否则勾选/展开/选中全部失效。
    virtual void HostFeatureEvent(DuiLib::TNotifyUI& /*msg*/) {}
    virtual bool HostFeatureContextMenu(POINT /*pt*/, bool /*bFromContextMsg*/)
        { return false; }

    // ---- [R5] 工具箱带参动作（文件对话框在壳/CLI 完成后传入路径）----
    virtual void HostSwmMerge(const DuiLib::CDuiString& /*sSwm*/,
                              const DuiLib::CDuiString& /*sWim*/) {}
    virtual void HostEsdDecryptWithFile(const DuiLib::CDuiString& /*sFile*/) {}

    // ---- [R5] WM_CLEAN_DONE 两段时序入口 ----
    // 页执行删除尾活/同步自定义动作，返回 true=用户同意重启 explorer；
    // 壳收到 true 后先调 RestartExplorer()，再调 HostOnCleanDoneTail()
    // 解锁 UI/弹完成气泡，保持"先重启后解锁"的原始严格顺序。
    // lParam 即 CleanDoneMsg*，HostOnCleanDone 实现内部负责 delete
    // （投递方仅在 PostMessage 失败时自行 delete，所有权同 kMsgOwnPage）。
    virtual bool HostOnCleanDone(LPARAM /*lParam*/) { return false; }
    virtual void HostOnCleanDoneTail() {}

    // ---- P8：第二级事件入口（默认不处理）----
    // 核心壳在过滤掉标题栏/菜单/导航/全局刷新等全局控件后，
    // 对其余 TNotifyUI 消息（click/dbclick/textchanged/
    // itemselect/selectchanged）原样转发到当前页。
    virtual void OnEvent(TNotifyUI& /*msg*/) {}

    // WM_RBUTTONUP / WM_CONTEXTMENU：屏幕坐标 + 是否键盘菜单键
    // （bFromContextMsg=true 表示 WM_CONTEXTMENU；原壳首块对两种
    // 消息生效，第二块仅 WM_RBUTTONUP 生效——页内必须保持该差异）。
    // 命中本页控件并已弹菜单返回 true；未命中返回 false。
    virtual bool OnContextMenu(POINT /*pt*/, bool /*bFromContextMsg*/) { return false; }

    // VK_DELETE（焦点不在编辑框时）：页内有选中项并已执行动作时
    // 返回 true。
    virtual bool OnDeleteKey() { return false; }

    // [R-B76] WM_DROPFILES 门控：核心壳收集完全部拖入路径后先问当前页；
    // 页消费（返回 true）则不再走通用驱动文件剪贴板逻辑。
    virtual bool OnDropFiles(const std::vector<std::wstring>& /*vPaths*/) { return false; }

    // ---- [R-B1] 全局加速键（Ctrl+N/S/A/C/X/V/Y）真实接线 ----
    // 默认空实现：不支持该动作的页面静默忽略；支持的页面 override。
    // Ctrl+A 全选本页勾选项
    virtual void OnGlobalSelectAll() {}
    // Ctrl+C / Ctrl+X 复制本页选中项到剪贴板（Ctrl+X 语义与 C 相同，
    // 不删除源内容，仅 Toast 提示差异）
    virtual void OnGlobalCopy(bool /*bCut*/) {}
    // Ctrl+S 导出/保存本页配置
    virtual void OnGlobalSave() {}
    // Ctrl+Y 重做（默认空实现；优化页 override）
    virtual void OnGlobalRedo() {}

    // [R-B1] 页面名称（日志/Toast 用）；默认返回空串，各页可 override
    virtual const wchar_t* PageName() const { return L""; }

protected:
    IPageContext& m_ctx;
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
