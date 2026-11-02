#pragma once

// ============================================================
// HostInterfaces.h —— Page / Service 与核心壳之间的窄接口（v2 P0）
//
// 规则：
//   * Page 只允许持有 IPageContext&，永远拿不到 CFrameWnd&；
//   * Service / Page 需要面向用户的能力（弹框/日志/进度/文件对话框）
//     一律走 IHostUi，禁止自行 MessageBox / Shell_NotifyIcon；
//   * IHostUi 的全部方法只允许在 UI 线程调用；工作线程需要交互时
//     必须 PostMessage 回 UI 线程，由核心壳代为调用。
//
// IHostUi 当前等价于既有 IToolHost（已具备 Hwnd/DPI/文件对话框/
// RunBat/QueryWim/Log/Notify/Confirm/Progress，且 HostConfirm 默认
// 已带 MB_DEFBUTTON2）。后续分化时只改本文件，调用点不动。
// ============================================================

#include "ToolDialogPage.h"          // IToolHost
#include "DataEngine.h"          // CDataEngine
#include "CleanEngine.h"         // CCleanEngine
#include "CleanExtend.h"         // [R80] CCleanExtendEngine
#include "FrameWnd_Messages.h"   // ELogLevel 配套消息契约
#include <functional>            // [CRASHFIX-UAF] HostDefer 的 std::function 形参

class CRegCleanEngine;           // [R79] 前向声明
class CCleanExtendEngine;        // [R80] 前向声明
class IPageHost;                 // [R5] 前向声明：页宿主窄接口（IPageHost.h）
enum class EPageId : int;        // [B4-F7] EPageId 前向声明（固定底层类型，可前置声明）

// 日志分级（原 FrameWnd.h 顶部）—— UI 侧唯一真源。
//   [L-11] 工程里另有两处同值表达：LogService 的落盘侧 ELogSeverity、
//   以及 toolsvc::ELogLevel（无 UI 业务层，不能反向 include 本头）。
//   三者的数值一致性由下面两条 static_assert 在**编译期**绑定，
//   任何一处改数都会立刻编译失败，不再靠注释维持。
enum class ELogLevel : int { LOG_INFO = 0, LOG_WARN = 1, LOG_ERROR = 2 };

// 落盘侧严重度：trace=0 debug=1 info=2 warn=3 error=4 fatal=5。
//   UI 的 0/1/2 与落盘侧的 2/3/4 数值不同（语义不同层级），因此不要求相等，
//   只要求"顺序一致"：UI 的 info < warn < error 必须对应落盘侧的递进。
//   真正的映射在 LogService::FromUiLevel() 里显式完成。
static_assert(static_cast<int>(ELogLevel::LOG_INFO)  < static_cast<int>(ELogLevel::LOG_WARN),  "UI 级别顺序必须 info<warn");
static_assert(static_cast<int>(ELogLevel::LOG_WARN)  < static_cast<int>(ELogLevel::LOG_ERROR), "UI 级别顺序必须 warn<error");

// 前向声明：IPageContext 的 UI 访问器只暴露最小控件基类指针，
// 本头不 include UIlib.h；Page cpp 自行 include 后再 downcast。
namespace DuiLib { class CControlUI; class CCheckBoxUI; }

// v2 统一命名：UI 宿主能力当前直接复用 IToolHost。
using IHostUi = IToolHost;

// Page 的唯一宿主类型。virtual 继承保证 CFrameWnd 经 IPageContext
// 实现 IToolHost 时只含一份宿主基类子对象。
// 随切片推进在本接口追加 Service 访问器，不预先一次列全。
class IPageContext : public virtual IHostUi
{
public:
    virtual CDataEngine&  DataEngine() = 0;
    virtual CCleanEngine& CleanEngine() = 0;
    // [R79] RegClean 注册表清理引擎（规则集/扫描/清理/备份/撤销）
    virtual CRegCleanEngine& RegCleanEngine() = 0;
    // [R80] Cleanup 深度扫描/残留/空文件夹/撤销引擎
    virtual CCleanExtendEngine& CleanExtendEngine() = 0;
    virtual void          SwitchToPage(int idx) = 0;
    // [B4-F7] EPageId 重载：字面量 SwitchToPage(PAGE_*) 匹配此重载，转发 int 版。
    virtual void          SwitchToPage(EPageId id) { SwitchToPage(static_cast<int>(id)); }

    // ---- P2：Page 构建/刷新 UI 所需的最小控件访问（等价原 m_pm 直取）----
    // 按名查找控件（未找到返回 nullptr）；焦点移交编辑框。
    virtual DuiLib::CControlUI* HostFindControl(const wchar_t* pszName) = 0;

    // [STAB] 类型安全的控件查找（新代码优先用它）。
    //
    // 背景：HostFindControl 按名返回 CControlUI*，调用方常用
    //   static_cast<CSwitchUI*>(...)
    // 直接转换。若名字匹配但实际类型不符（改 XML、重命名遗漏），
    // 会通过错误类型调虚函数 → 未定义行为。
    //
    // 本模板实现：用 dynamic_cast 做一次真实多态校验。
    //   - 多态不匹配：返回 nullptr（零 UB 风险）
    //   - 未找到：返回 nullptr
    //   - 匹配：安全返回 T*
    // dynamic_cast 对 DuiLib 控件（全部虚继承）开销仅一次虚表比较，
    // 等价于 GetClass() 字符串比较，远快于控件树遍历。
    //
    // 用法：auto* pSw = HostFindControlAs<DuiLib::CSwitchUI>(L"sw_0_1_0");
    template <class T>
    T* HostFindControlAs(const wchar_t* pszName)
    {
        DuiLib::CControlUI* pRaw = HostFindControl(pszName);
        if (pRaw == nullptr) return nullptr;
        return dynamic_cast<T*>(pRaw);
    }

    // 带类名断言的版本：pszExpectClass 传该控件在 XML/代码中注册的类名
    // （如 L"Switch"、L"Button"、L"VerticalLayout"）。不匹配时返回 nullptr。
    //
    // 为什么需要它：HostFindControl 只按名字查，名字对但类型不符时
    // static_cast 会产出错误类型的指针，后续调虚函数是未定义行为。
    // 这种错在「改 XML 换了控件类型但没改 C++」时最容易出现。
    template <class T>
    T* HostFindControlChecked(const wchar_t* pszName, const wchar_t* pszExpectClass)
    {
        DuiLib::CControlUI* pRaw = HostFindControl(pszName);
        if (pRaw == nullptr) return nullptr;
        const wchar_t* pszGot = pRaw->GetClass();
        if (pszGot != nullptr && pszExpectClass != nullptr
            && lstrcmpiW(pszGot, pszExpectClass) != 0)
        {
            ::OutputDebugStringW(L"[STAB] HostFindControlChecked: class mismatch\r\n");
            return nullptr;
        }
        return static_cast<T*>(pRaw);
    }
    // P8：按屏幕坐标命中测试（右键菜单归属判定，等价原 m_pm.FindControl(pt)）
    virtual DuiLib::CControlUI* HostFindControlAt(POINT pt) = 0;
    virtual void                 HostSetFocusControl(DuiLib::CControlUI* pCtrl) = 0;
    // 折叠箭头字形（0=向右收起，1=向下展开），字形资源归核心壳/UiHelpers
    virtual const wchar_t*              HostGlyphArrow(int nIdx) = 0;
    // 应用内右上角浮动 Toast（区别于 HostNotify 的托盘气球），3 秒自动消失
    virtual void                 HostToast(const wchar_t* pszTitle, const wchar_t* pszText, int nIcon = 0) = 0;
    // 危险操作确认：MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2，IDOK 返回 true
    virtual bool                 HostConfirmWarn(const wchar_t* pszTitle, const wchar_t* pszText) = 0;

    // ---- P3b：启动/服务页所需的字形与 UI helper 访问 ----
    // 复选框字形（0 空框 / 1 全选 / 2 半选）、短横线字形
    virtual const wchar_t*              HostGlyphBox(int nIdx) = 0;
    virtual const wchar_t*              HostGlyphDash() = 0;
    // 复选框文本随勾选态刷新（原 CFrameWnd::RefreshCheckText）
    virtual void                 HostRefreshCheckText(DuiLib::CCheckBoxUI* pCheck) = 0;
    // 控件 DPI 缩放百分比（等价原 m_pm.GetDPIObj()->GetScale()）
    virtual int                  HostDPIScale() = 0;
    // 按名读取编辑框文本（不存在返回空串）
    virtual DuiLib::CDuiString   HostReadEditValue(const wchar_t* pszName) = 0;

    // ---- P4：Appx/可选功能/更新页所需的 UI/工具访问 ----
    // i18n 皮肤文案（StringMgr skin.<name> 缺键回退 XML 原文）
    virtual DuiLib::CDuiString   HostSkinText(const wchar_t* pszName) = 0;
    // 按名设置控件文本（控件不存在静默）
    virtual void                 HostSetCtrlText(const wchar_t* pszName, const wchar_t* pszText) = 0;
    // “加载中”占位行（弹性间距+旋转环+文案）
    virtual DuiLib::CControlUI*  HostMakeLoadingHint(const wchar_t* pszText, const wchar_t* pszRingName) = 0;
    // 外部工具经核心壳统一启动（P6 前内部转发 CFrameWnd::StartTool）
    virtual void                 HostStartTool(const wchar_t* pszExe, const wchar_t* pszArgs) = 0;
    // 是/否确认（MB_YESNO|MB_DEFBUTTON2）；bInfoIcon=true 用信息图标，否则警告图标
    virtual bool                 HostConfirmYesNo(const wchar_t* pszTitle, const wchar_t* pszText,
                                                  bool bInfoIcon = false) = 0;
    // 会话条“就绪”文案（原 m_sStatusReady）
    virtual DuiLib::CDuiString   HostStatusReady() = 0;

    // ---- P7：系统优化页所需 ----
    // 重启 explorer.exe（薄转 CFrameWnd::RestartExplorer；页不直碰壳）。
    // 注意：UI 点击路径（combo/开关/批量）不要直接调用它，统一
    // PostMessage(WM_HOST_REQUEST_RESTART_EXPLORER) 延迟到点击栈外执行，
    // 原因见 FrameWnd_Messages.h 该消息定义处注释。
    virtual bool                 HostRestartExplorer() = 0;

    // [R-B51] 升级链阶段失败定位：打开日志视图（薄转核心壳 RunDialog(EToolDlgKind::TD_LOGVIEW)）
    virtual void                 HostShowLogView() = 0;

    // ---- [R3] 懒构建后控件提示注册（原壳 EnsurePageBuilt TOOLS 分支的
    // AddTooltipForControl×11 + RefreshTooltipRects，随页自治迁入页内）----
    virtual void                 HostAddTooltip(const wchar_t* pszCtrl, const wchar_t* pszText) = 0;
    virtual void                 HostRefreshTooltipRects() = 0;

    // ---- [R5] 页宿主表访问：寄宿于他页容器内的跨页事件（如启动页 Tab2
    // 内嵌的计划任务控件、程序页 prog_pages Tab1 的 Windows 功能子视图）
    // 经 PageById + CPageBase 虚函数转发，页不依赖任何具体 CPageXxx 类型。
    virtual IPageHost*           HostPageHost() = 0;

    // [CRASHFIX-UAF] 把一段 UI 变更（整表重建等）推迟到当前消息派发栈
    // 退清之后执行。用途：控件在自己的 DoEvent 派发栈内删除自身或兄弟
    // 控件（RemoveAll）会造成 use-after-free；改走本接口后，回调在
    // WM_UI_DEFERRED_EXEC（见 WmMessages.h）中运行，此时事件栈已空。
    // 约束：回调只允许在 UI 线程执行；fn 内不得再同步派发点击事件。
    virtual void                 HostDefer(std::function<void()> fn) = 0;
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
