#pragma once

#include "UIlib.h"
#include "HostInterfaces.h"   // IHostUi / IPageContext + Data/CleanEngine/ToolDialog/消息契约
#include "NotifyBox.h"        // [R-AUTOTEST] ToolSql::UnattendedSilent（无人值守自动应答）
#include "ToolService/IToolServiceHost.h"   // [R16][T6] 核心壳直接实现服务层宿主窄接口（wstring 语义）
#include "LogService.h"      // P1：日志纯逻辑（级别/审计落盘/离屏缓冲）
// [R4] 17 个具体页值成员与具体页头下沉 CPageHost（PageHost.h 内部聚合
// 全部 Page_*.h）；本头不再直接依赖任何具体 CPageXxx 类型。
#include "PageHost.h"       // R4：页宿主（IPageHost + 16 页序页装配）
#include "ThemeTokens.h"    // [R236] tokens::EThemeMode（ShowThemePicker 签名用）
#include "ThemeService.h"   // [2026] 统一主题编排器（Observer + Preset 管理）
#include "RegCleanEngine.h" // [R79] 注册表清理引擎（规则集/扫描/清理/备份/撤销）
#include <shellapi.h>
#include <vector>
#include <map>
#include <utility>

using namespace DuiLib;

// [U-3] 注册本 demo 的自定义 DuiLib 控件（必须在 Main.xml 解析前调用）：
// NavOption —— 左侧导航行，GDI+ 自绘 Win11 Fluent 线性图标（替代 nav_*.png）
void RegisterWin8MetroControls();

// [STAB] 通知启动预热线程让路（页面已构建，用户正在等待真实枚举）。
// 定义在 FrameWnd_SysMsg.cpp；幂等，多次调用无副作用。
void NotifyWarmupYield();

class CFrameWnd : public WindowImplBase, public IPageContext,
                  public toolsvc::IToolServiceHost,   // [R16][T6] 双宿主接口归一：服务层 wstring 语义
                  public ThemeService::IThemeObserver  // [2026] 主题观察者：订阅 preset 变更
{
public:
    // P6b：CustomProcIdx（CPI_* 12 路派发）已下沉 CleanJobService.h，
    // 清理页经 CleanJobService 静态接口访问。

    explicit CFrameWnd(const wchar_t* pszXMLPath);
    virtual ~CFrameWnd();

    const wchar_t*     GetWindowClassName() const;
    CDuiString  GetSkinFile();
    CDuiString  GetSkinFolder();
    void        InitWindow();
    // [R-B3] CLI 参数层消费点：InitWindow 尾部调用一次，按命令行动作真实执行
    void        ApplyCommandLineActions();
    // [R139] /DumpLayout 诊断入口：把控件树（name/class/rect/visible）落盘。
    //   用途：R138 的 Win32 探针只能看到主窗口（DuiLib 自绘、无子 HWND），
    //   控件级几何必须由 DuiLib 自己遍历才能拿到。只读，不改控件状态。
    void        DumpLayoutTree();
    // [R139-2] 逐页构建并各 dump 一份（页面懒构建，只 dump 当前页看不到内容）
    void        DumpLayoutAllPages();
    // [REFACTOR] 从 ApplyCommandLineActions 提取：资源管理器外壳右键动词
    // （/ESD2ISO /ESD2WIM /ESDDECRYPT /CompactEx /C /exe /U）。
    void        ApplyShellVerbs();

    void Notify(TNotifyUI& msg);
    // [REFACTOR] 从 Notify 提取：顶部导航 nav_<index> 点击处理。
    // 含 WinPE 离线会话门控与延迟构建调度。
    void OnNavClicked(const wchar_t* pszName);
    // P8：当前内容页访问器（两级路由：Notify/HandleCustomMessage → 页）
    CPageBase* CurrentPage();
    // [GC-03] 页序 → 页对象的唯一映射（页描述符表查询），
    // 取代 CurrentPage/EnsurePageBuilt/RefreshCurrentPage 中重复的按页 switch
    CPageBase* PageById(int idx);
    // [B4-F7] EPageId 重载：字面量调用（PageById(PAGE_*)）匹配此重载，
    // 转发到 int 版；动态 int（GetCurSel/循环/表项）走 int 版，语义不变。
    CPageBase* PageById(EPageId id) { return PageById(static_cast<int>(id)); }
    LRESULT HandleCustomMessage(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);

    void setDPI(int dpi);

    // [R201] 把控件树上仍是皮肤默认强调色的元素改为当前系统主题色。
    //   幂等：只改"当前值恰好等于皮肤默认色"的控件，重复调用无副作用。
    //   在 EnsurePageBuilt 收口点调用，覆盖所有懒构建页面。
    // [R321] 手动切换主题时重放皮肤层 {tok:*} 颜色（建树时已固化为字面值）。
    void ReplaySkinTokens();
    // [R327] 重刷所有滚动条配色。
    //   为什么必须单独做：皮肤里的滚动条颜色写在 <Default name="VScrollBar"> 里，
    //   而 <Default> 的值是在**皮肤加载那一刻**按当时的令牌展开并固化的 ——
    //   之后的主题切换不会重放它（ReplaySkinTokens 只处理具名控件）。
    //   实测症状：切到深色后，05/06/07/09/12/14 六个页面的滚动条滑块仍是浅色
    //   #D1D1D1（浅色 Border 的值），压在 #202020 底上非常刺眼。
    void RefreshScrollBarColors();
    void ApplyAccentToControls(int idx);


    // [R206] 统计某页控件树规模并落日志（"页面有内容"的可量化判据）。
    void LogPageContent(int idx);

    // [R209] 逐页离屏渲染导出 PNG（/SHOT=<dir> 诊断入口使用）。
    void ExportAllPageShots(const wchar_t* pszDir);
    // [UI-GEO] Write the laid-out control tree so geometry can be compared against a
    // design directly, instead of being inferred from pixels.
    void ExportControlTree(const wchar_t* pszPath);
    // Root of the laid-out control tree; null before the window is created.
    CControlUI* GetControlTreeRoot() { return m_pm.GetRoot(); }



    // [R201] 修正皮肤字体族：把设计名映射到本机真实可用的字体名并重建字体表。
    //   只改字族，字号与样式位原样保留（布局不受影响）。
    void FixFontFaces();


    // [R202] 递归展开皮肤（处理 <Include> 并替换 {tok:X}），供 GetSkinFile 使用。
    [[nodiscard]] bool ExpandSkinFile(const wchar_t* pszPath, std::wstring& outXml, int nDepth);

    // [R242] 皮肤来源：优先从 SkinThemeDll.dll 读（按当前深浅色选槽位），
    //   失败则回退磁盘文件。返回 false 表示 DLL 不可用，调用方走原路径。
    bool ReadSkinFromDll(const wchar_t* pszPath, std::string& outBytes);

    // [2026] 宿主侧刷预设颜色 —— 接管旧 DLL fnSetAttr 的角色。
    //   在 RebuildMainTree 之前刷（重建后 XML 里的颜色自然生效），
    //   重建后刷的是 root 层里没有 token 对应关系的硬编码属性（如 titlebar）。
    void ApplyPresetColorAttrs(const ThemeService::ThemePreset& preset);

    // [2026] 彻底重建主控件树 —— 解决颜色混用/字体不清晰/页面残留的根治方案。
    //   之前 SetResourcePath + ReloadImages 只清图片缓存，控件树颜色/字体/布局
    //   都是旧皮肤 XML 建的 → 混用串色/字体混乱。
    //
    //   RebuildMainTree 做 6 件事：
    //   ① ReloadImages 清图片缓存
    //   ② m_pm.ReapObjects(GetRoot()) 清旧 root + 所有子控件（含 16 页 TabLayout 里的页面）
    //   ③ m_bPageBuilt[0..15] = false 标记所有页为"未构建"
    //   ④ CDialogBuilder::Create(GetSkinFile()) 用新皮肤 XML 建 root（ExpandSkinFile 展开 {tok:*}）
    //   ⑤ m_pm.AttachDialog(pNewRoot) 挂新 root
    //   ⑥ RebindRootControls() 重绑品牌文字/窗口标题等 root 层后期绑定
    //   ⑦ FixFontFaces() 重建字体表
    //
    //   为什么安全？FrameWnd 没有缓存控件指针（全部 m_pm.FindControl 动态查）
    //   16 页是 EnsurePageBuilt 懒加载的，重建 root 后第一次访问会自然重建。
    void RebuildMainTree();
    void RebindRootControls();

    // [R201] 应用窗口级系统外观：Win11 圆角 + Mica + 深色标题栏。
    //   幂等：创建时调用一次，之后每次 WM_SETTINGCHANGE / WM_THEMECHANGED
    //   重放一次。Win10 上不受支持的属性逐项跳过（不崩溃、不缺导入）。
    //   同时刷新强调色缓存，使"设置里改主题色"能即时生效，无需重启。
    //
    // [R325] bForceReplay：强制走「皮肤重放 + 各页刷新 + 整树重绘」这一段。
    //   为什么需要它：这段原本被 `m_bLastDarkTheme != bDark` 门控，只有深浅翻转
    //   才执行。但有两种情况深浅不变、界面却必须重刷：
    //     a. 用户只改了系统强调色（WM_SETTINGCHANGE）——令牌变了，控件属性没变；
    //     b. 用户手动重选当前同一个模式（"同色重选"），期望强制刷新一次。
    //   加了开关之后，手动路径与系统路径可以各自表达意图，且不会再出现
    //   "调用方先重放一遍、收口函数里再重放一遍"的双倍开销。
    void ApplySystemTheme(bool bForceReplay = false);
    // [R236] 启动时读回上次选择的主题模式（无配置则保持 Auto = 跟随系统）。
    //   public：启动流程在 Win8Metro.cpp 里，必须在 ApplySystemTheme 之前调用。
    void LoadThemeMode();
    // [R324] 供逐页像素验收使用：按真实切换路径套用主题（不落盘）。
    //   为什么需要它：/SHOT 是冷启动渲染，每页首次构建时令牌已是最新值，
    //   天然不会暴露"切主题时漏刷新"。只有先真正切换再渲染才走得到
    //   RefreshThemeColors，才能检出漏刷新的页面。
    void ApplyThemeForShot(tokens::EThemeMode e) { ApplyThemeMode(e, false); }
    // [R218] Last dark/light state applied, so a light<->dark switch can be
    //   detected and force a full repaint. A function-local static would be
    //   shared by every window instance and could skip the first paint.
    bool m_bLastDarkTheme = false;

    // [R5-B1] WM_DPICHANGED 具名入口（供 HandleCustomMessage 与 WM_NCCALCSIZE /
    // WM_NCPAINT 等系统路径共用）。dpiW/dpiH：新 DPI；prcProposedRect：系统
    // 建议的新窗口矩形（跨显示器拖动后调用者必须应用）。
    void OnDpiChanged(int dpiW, int dpiH, const RECT* prcProposedRect);

    // ---- IToolHost：工具箱弹窗的真实宿主能力 ----
    HWND       HostHwnd() override;
    int        HostDPI() override;
    bool       HostOpenFile(CDuiString& sResult, const wchar_t* pszFilter, const wchar_t* pszTitle) override;
    bool       HostSaveFile(CDuiString& sResult, const wchar_t* pszFilter, const wchar_t* pszTitle,
                            const wchar_t* pszDefName = nullptr) override;
    bool       HostBrowseFolder(CDuiString& sResult, const wchar_t* pszTitle) override;
    bool       HostQueryWim(const CDuiString& sWim,
                            std::vector<TDialogWimIndex>& vOut,
                            CDuiString* psErr = nullptr) override;
    bool       HostRunBat(const wchar_t* pszBatName, const CDuiString& sBody,
                          HANDLE* phProcess = nullptr) override;
    void       HostLog(const CDuiString& sLine, int nLevel = 0) override;
    // [R19] 宿主统一交互能力
    void       HostNotify(const wchar_t* pszTitle, const wchar_t* pszText, int nIcon = 0) override;
    bool       HostConfirm(const wchar_t* pszTitle, const wchar_t* pszText) override;
    void       HostProgress(int nVal, int nMax) override;

    // ---- [R16][T6] toolsvc::IToolServiceHost：wstring 语义，转发上方实现 ----
    // HostHwnd/HostDPI 与 IToolHost 签名一致，一份实现同时满足两接口。
    // HostNotify/HostProgress 仅 UI 使用（服务层不需要），保留在 IToolHost。
    bool   HostOpenFile(std::wstring& sResult, const std::wstring& sFilter,
                        const std::wstring& sTitle) override;
    bool   HostSaveFile(std::wstring& sResult, const std::wstring& sFilter,
                        const std::wstring& sTitle, const std::wstring& sDefName) override;
    bool   HostBrowseFolder(std::wstring& sResult, const std::wstring& sTitle) override;
    bool   HostQueryWim(const std::wstring& sWim,
                        std::vector<toolsvc::TWimIndex>& vOut,
                        std::wstring* psErr) override;
    bool   HostRunBat(const std::wstring& sBatName, const std::wstring& sBody,
                      HANDLE* phProcess) override;
    void   HostLog(const std::wstring& sLine, int nLevel) override;
    void   HostAlert(const std::wstring& sTitle, const std::wstring& sText,
                     UINT nIcon) override;
    bool   HostConfirm(const std::wstring& sTitle, const std::wstring& sText) override;

private:
    // P3b：StartupEntry / ServiceEntry / SRC_* 已随 S3 迁 StartupService.h
    // （全局结构）；启动项/服务列表在 CPageStartup。

    // P3a：ProgEntry（Uninstall 注册表读出的已安装程序）已随 S4 迁
    // ProgramService.h（全局 struct ProgEntry）；程序列表在 CPageProgram。

    // P5：FeatInfo 已随 CPageFeature 迁 FeatureService.h；
    // DriverPubInfo/DriverRowInfo 已随 CPageDriver 迁 DriverService.h。

    // P4：AppxPkg/AppxDataMsg/CapEntry/CapDataMsg/UpdEntry/UpdDataMsg
    // 已提升为全局结构，定义在 AppxCapUpdService.h（经 Page_Appx/Capability/Update.h 引入）。

    // ==== logging ====
    void AppendLog(const wchar_t* pszLine);
    // [R19] 日志分级：WARN 橙色 / ERROR 红色（输出面板按级着色）
    void AppendLogEx(const wchar_t* pszLine, int nLevel);
    // [A1] 落盘 action_log.jsonl 已下沉 LogService（m_logSvc.AppendToFile）
    void AppendLogWarn(const wchar_t* pszLine);
    void AppendLogError(const wchar_t* pszLine);
    // [L-01] FindVisibleOutput / FindOutputRecursive / FlushBufferToOutput 已删除：
    //   它们服务的是页面内 out_* 输出区，而该控件在皮肤与动态构建中都不存在。
    CControlUI* DeepFind(CControlUI* pRoot, const wchar_t* pszName);

    // ==== [R18/R19] 交互增强 ====
    // 会话条内嵌进度条（不改 Main.xml，运行时动态挂载到 sess_status 同容器）
    void InitSessionProgress();
    void SetSessionProgress(int nVal, int nMax);   // nMax<=0 隐藏
    // [R2 P1] 优化执行进度：P7 已迁 CPageOptim（本行为薄转发）；
    // XML 无 optim_progress 时页内自动回退 HostProgress()，100ms 节流也在页内。
    // 系统托盘气泡通知
    void InitTrayIcon();
    void RemoveTrayIcon();
    void SendBalloonTip(const wchar_t* pszTitle, const wchar_t* pszText, DWORD dwFlags = NIIF_INFO);
    // [U-10] 应用内 Toast 通知（右上角浮动、3 秒自动消失）
    void ShowToast(const wchar_t* pszTitle, const wchar_t* pszText, int nIcon = 0);
    static LRESULT CALLBACK ToastWndProc(HWND, UINT, WPARAM, LPARAM);
    void DestroyToast();
    // 轻量 Tooltip（Win32 TOOLTIPS_CLASS，TTF_SUBCLASS 按控件矩形命中）
    void InitTooltips();
    void AddTooltipForControl(const wchar_t* pszName, const wchar_t* pszText);
    void RegisterStaticTooltips();                 // 常驻主按钮/导航提示
    void RefreshTooltipRects();                    // 布局/页面重建后同步矩形
    // 页面懒加载：首次切入 content_area 对应页时才构建/枚举
    void EnsurePageBuilt(int idx);
    // [B4-F7] EPageId 重载：字面量 EnsurePageBuilt(PAGE_*) 匹配此重载，转发 int 版。
    void EnsurePageBuilt(EPageId id) { EnsurePageBuilt(static_cast<int>(id)); }

    // [LAG-FIX] 切页重活延迟到点击消息处理完之后（先让页面完成一次绘制）
    void RequestDeferredBuild(int idx);
    // [LAG-FIX] 日志 120ms 批量提交（消除 dism/PowerShell 日志风暴导致的未响应）
    void ArmLogFlush();
    void FlushPendingLogs();
    // 管理员权限断言（manifest 已 requireAdministrator，此为运行时兜底）
    bool RequireAdmin(const wchar_t* pszAction);
    // 全局加速键命令
    void OnAccelCommand(int nId);

    // ============================================================
    // [R1]/[R5] 页完成消息注册表（实现见 FrameWnd_PageMsg.cpp）
    // 20 条"后台线程→内容页"消息表驱动分发；表项 {消息, 页序, 所有权}，
    // 经 PageById(iPage)->CPageBase::OnAsyncMessage 虚入口到页，壳不持
    // 具体页类型。iPage<0 为无目标页的壳级日志消息。堆参数所有权：
    //   kMsgOwnPage —— 页 OnAsyncMessage 链路内释放，壳禁止再碰（防双删）
    //   kMsgOwnShell—— 调页后由 ReleaseMsgParam 单出口释放
    //   kMsgValue/kMsgNone —— 值参/无参，无堆对象
    // WM_CLEAN_DONE 含 RestartExplorer 两段时序编排，刻意不入表。
    // ============================================================
    enum class EPageMsgOwn : int { kMsgNone = 0, kMsgValue, kMsgOwnPage, kMsgOwnShell };
    struct SPageMsgEntry { UINT uMsg; EPageId iPage; EPageMsgOwn eOwn; };

    bool DispatchPageMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);
    void ReleaseMsgParam(UINT uMsg, WPARAM wParam, LPARAM lParam);

    static const SPageMsgEntry kPageMsgTab[];

    // ============================================================
    // [R2] 壳系统/输入消息（实现见 FrameWnd_SysMsg.cpp）
    // 托盘/拖放/尺寸/DPI/延迟重排/定时器表/退出序列全部外迁；
    // HCM 仅保留一次 HandleSystemMessage 路由 + WM_CLEAN_DONE 编排。
    // 返回 true=已拦截（HCM 置 bHandled 并 return 0），false=放行 DuiLib。
    // ============================================================
    bool HandleSystemMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);
    // [R5] WM_TIMER 分发表：iPage>=0 的定时器直接经
    // PageById(iPage)->CPageBase::OnHostTimer 分发；iPage<0 的纯壳定时器
    // 走 pfnShell（旋转环/旧 FEAT 保险 Kill/日志批量落盘/延迟构建）。
    // 非本窗口定时器（DuiLib 滚动条等）一律放行。
    void OnTimerRing();
    void OnTimerFeatLegacy();
    void OnTimerPageBuild();
    void OnTimerWarmup();   // [T4 P1] 启动 5s 后预热共享在线枚举会话

    // [REFACTOR] HandleSystemMessage 拆分：按消息类别提取的子分发。
    // 逐字搬迁零逻辑改动，仅降低单函数规模与嵌套深度。
    bool HandleDropFiles(WPARAM wParam);                          // WM_DROPFILES
    bool HandleTrayNotify(WPARAM wParam, LPARAM lParam);          // WM_TRAYNOTIFY
    bool HandleCloseMessage(WPARAM wParam);                       // WM_CLOSE
    void HandleSizeMessage(WPARAM wParam, LPARAM lParam);         // WM_SIZE（仅副作用，不拦截）
    bool HandleRelayoutRequest(WPARAM wParam, LPARAM lParam);     // WM_APP+101
    void LayoutTitlebarButtons();                                 // WM_APP+102 [FIX-TITLEBAR]

    using PfnTimerMsg = void (CFrameWnd::*)();
    struct STimerEntry { UINT uId; EPageId iPage; PfnTimerMsg pfnShell; };
    static const STimerEntry kTimerTab[];
    void FocusSearchOnCurrentPage();
    // 优化设置撤销栈（Ctrl+Z）—— P7：SOptimUndo 与栈本体均迁 CPageOptim
    // [R-B1] 快捷键真实接线：Ctrl+N/S/A/X/C/V/Y 分派到当前页 OnGlobal*
    void OpenRulesDir();          // Ctrl+N 打开规则目录
    void OnHotkeyCtrlN();
    void OnHotkeyCtrlS();
    void OnHotkeyCtrlA();
    void OnHotkeyCtrlX();
    void OnHotkeyCtrlC();
    void OnHotkeyCtrlV();
    void OnHotkeyCtrlY();
    // Tab 键盘焦点可见高亮
    void OnFocusChanged(CControlUI* pNewFocus);

    // ==== process engine ====
    void StartTool(const wchar_t* pszExeName, const CDuiString& strArgs);
    // [P1-收尾] StartDism(dism.exe 子进程) 已删除：组件操作全部进程内
    // DismApiBridge 直连，无外部命令入口。
    void LaunchGUI(const wchar_t* pszExeName, const wchar_t* pszArgs);
    void AbortDism();
    // P6a：OnDismDone 已拆分——ExternalToolService::OnDone（句柄）+
    // CPageTools::OnDismDone（UI 收尾），本壳不再保留该方法。

    // ==== navigation ====
    void InitSystemInfo();

    // ==== dynamic pages ====
    // [B2] F5 全局刷新：按当前 PAGE 调用对应 Build* 函数
    void RefreshCurrentPage();
    // [B4] 列出 skins/ 目录下的主题名（仅枚举，不做热切换）
    void ListAvailableThemes(std::vector<CDuiString>& vOut);

    // [R236] 标题栏主题下拉：在主题按钮下方弹出带配色预览的菜单。
    //   选中后立即换肤（令牌 + DWM 标题栏 + 全页重绘）并落盘，重启后保持。
    void ShowThemePicker();
    // [2026] ThemeService Observer 回调：所有主题变更最终收敛到这里
    //   ThemeService 切换 preset → ApplyDwm → 广播 OnThemeChanged → Repaint
    void OnThemeChanged(const ThemeService::ThemePreset& preset) override;
    // [DLL] 应用主题模式: 宿主做 tokens::SetMode + ThemeManager::ApplyMode
    bool ApplyThemeMode(tokens::EThemeMode e, bool bPersist);
    void RefreshThemeButton();
    // [B5] 语言选择弹窗：列出可用语言，选择后写 config.ini，提示重启
    void ShowLanguagePicker();
    // [R51] 性能监控：启动时间测量（从构造函数到 InitWindow 完成）
    void MeasureStartupTime();
    DWORD GetStartupTimeMs() const { return m_dwStartupMs; }

    // ============================================================
    // P2c：[R37 Patch D] 文件关联写入已迁 AssocService（S8）：
    // enum EAssocWriteResult 与 Set/Clear/Reset/Verify/ApplyBatch 见
    // AssocService.h；ShowPickAssocProgramDialog 随关联页迁 CPageAssoc。
    // ============================================================

    std::map<CDuiString, CDuiString> m_assocPending;   // [R37] 待应用的关联（扫描无使用点，保留待清理）
    bool RestartExplorer();
    void ImportConfig();
    void RefreshAll();
    void ShowPopupMenu(int iMenu);
    // [R14-01] 菜单表驱动拆解：ShowPopupMenu 拆为 4 个 Build + Track + Dispatch + case 处理器
    void BuildMenuFile(HMENU hMenu);
    void BuildMenuRecover(HMENU hMenu);
    void BuildMenuOption(HMENU hMenu, UINT uChk0, UINT uChk1, UINT uChk2);
    void BuildMenuHelp(HMENU hMenu);
    void TrackMenuAndDispatch(HMENU hMenu, int iMenu);
    void DispatchMenuCmd(int iCmd);
    // case 内联逻辑抽取
    // [R16-T7] 菜单真表驱动：kMenuCmdTab 分发，处理器统一 (int iCmd) 签名
    void MenuCmdAddSourceDir(int iCmd);
    void MenuCmdSwmMerge(int iCmd);
    void MenuCmdRestoreHealth(int iCmd);
    void MenuCmdBootToRecovery(int iCmd);
    void MenuCmdUncompactOs(int iCmd);
    void MenuCmdSwitchLang(int nIdx);
    // [R16-T7] 表驱动新增处理器
    void MenuCmdRouteTools(int iCmd);
    void MenuCmdRefresh(int iCmd);
    void MenuCmdAbortTask(int iCmd);
    void MenuCmdGotoOptim(int iCmd);
    void MenuCmdGotoDriver(int iCmd);
    void MenuCmdSfcVerify(int iCmd);
    void MenuCmdResetHint(int iCmd);
    void MenuCmdDetails(int iCmd);
    void MenuCmdSetAfterAction(int iCmd);
    void MenuCmdLangByCmd(int iCmd);
    void MenuCmdHelpUrl(int iCmd);
    void MenuCmdHelpIntro(int iCmd);
    void MenuCmdAbout(int iCmd);
    void MenuCmdLogView(int iCmd);
    // [R16-T8] 语言切换后重启进程（自 FrameWnd_Menu.cpp 匿名 namespace 收编为成员，
    // 失败提示统一走 HostAlert）
    void RestartApplication();
    void SwitchToPage(int idx) override;
    // [B4-F7] EPageId 重载：字面量 SwitchToPage(PAGE_*) 匹配此重载，转发 int 版。
    void SwitchToPage(EPageId id) override { SwitchToPage(static_cast<int>(id)); }

    // ---- IPageContext：引擎访问（内联，Page 经窄接口使用，不直碰成员）----
    CDataEngine&  DataEngine()  override { return m_dataEngine; }
    CCleanEngine& CleanEngine() override { return m_cleanEngine; }
    // [R79] RegClean 注册表清理引擎
    CRegCleanEngine& RegCleanEngine() override { return m_regCleanEngine; }
    // [R80] Cleanup 深度扫描/残留/空文件夹/撤销引擎
    CCleanExtendEngine& CleanExtendEngine() override { return m_cleanExtendEngine; }
    // ---- IPageContext：P2 最小控件/字形访问（等价原 Page 内 m_pm 直取）----
    CControlUI*   HostFindControl(const wchar_t* pszName) override { return m_pm.FindControl(pszName); }
    // P8：右键命中测试（CPageBase::OnContextMenu 用）
    CControlUI*   HostFindControlAt(POINT pt) override { return m_pm.FindControl(pt); }
    void          HostSetFocusControl(CControlUI* pCtrl) override { if (pCtrl) m_pm.SetFocus(pCtrl); }
    const wchar_t*       HostGlyphArrow(int nIdx) override { return m_sGlyphArrow[nIdx & 1]; }
    // ---- P3b：启动/服务页字形与 UI helper ----
    const wchar_t*       HostGlyphBox(int nIdx) override { return m_sGlyphBox[nIdx % 3].GetData(); }
    const wchar_t*       HostGlyphDash() override { return m_sGlyphDash.GetData(); }
    void          HostRefreshCheckText(CCheckBoxUI* pCheck) override { RefreshCheckText(pCheck); }
    int           HostDPIScale() override;          // 实现在 FrameWnd.cpp
    CDuiString    HostReadEditValue(const wchar_t* pszName) override;  // 实现在 FrameWnd.cpp
    void          HostToast(const wchar_t* pszTitle, const wchar_t* pszText, int nIcon = 0) override { ShowToast(pszTitle, pszText, nIcon); }
    bool          HostConfirmWarn(const wchar_t* pszTitle, const wchar_t* pszText) override
        { // [R-AUTOTEST] 无人值守：危险确认保守取"否"（IDOK 之外的语义），
          // 绝不让测试开关批量放行不可逆操作；交互模式零改动。
          if (ToolSql::UnattendedSilent()) return false;
          return ::MessageBoxW(m_hWnd, pszText, pszTitle, MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) == IDOK; }
    // ---- P4：Appx/可选功能/更新页 ----
    CDuiString    HostSkinText(const wchar_t* pszName) override;     // 实现在 FrameWnd.cpp
    void          HostSetCtrlText(const wchar_t* pszName, const wchar_t* pszText) override;
    CControlUI*   HostMakeLoadingHint(const wchar_t* pszText, const wchar_t* pszRingName) override
        { return MakeLoadingHint(pszText, pszRingName); }
    void          HostStartTool(const wchar_t* pszExe, const wchar_t* pszArgs) override { StartTool(pszExe, pszArgs); }
    bool          HostConfirmYesNo(const wchar_t* pszTitle, const wchar_t* pszText, bool bInfoIcon = false) override
        { // [R-AUTOTEST] 无人值守：Yes/No 确认自动取"是"（与 ToolSql::HostConfirm 同口径）
          if (ToolSql::UnattendedSilent()) return true;
          return ::MessageBoxW(m_hWnd, pszText, pszTitle,
                               MB_YESNO | MB_DEFBUTTON2 | (bInfoIcon ? MB_ICONINFORMATION : MB_ICONWARNING)) == IDYES; }
    CDuiString    HostStatusReady() override { return m_sStatusReady; }
    // ---- P7：系统优化页重启 explorer（薄转 RestartExplorer 壳实现）----
    bool          HostRestartExplorer() override { return RestartExplorer(); }
    // [R-B51] 升级链阶段失败定位：打开日志视图（[R4] 经页表虚函数，不持具体页）
    void          HostShowLogView() override
    {
        if (CPageBase* p = PageById(EPageId::PAGE_TOOLS)) p->HostRunToolDialog(EToolDlgKind::TD_LOGVIEW);
    }
    // [R3] 页懒构建后提示注册（薄转壳 Tooltip 管理）
    void          HostAddTooltip(const wchar_t* pszCtrl, const wchar_t* pszText) override
        { AddTooltipForControl(pszCtrl, pszText); }
    void          HostRefreshTooltipRects() override { RefreshTooltipRects(); }
    // [R5] 页宿主表（跨页寄宿事件转发用；返回窄接口，页不接触具体页类型）
    IPageHost*    HostPageHost() override { return &m_pages; }
    // [CRASHFIX-UAF] 页侧延迟 UI 重建：入队 + PostMessage 唤醒。
    // 回调在 HandleCustomMessage(WM_UI_DEFERRED_EXEC) 中执行——届时
    // 触发入队的鼠标事件栈已完全退清，RemoveAll 不再删除在途派发控件。
    // 队列归本窗口所有：窗口销毁即随成员析构丢弃，无悬挂回调。
    void          HostDefer(std::function<void()> fn) override
    {
        if (!fn) return;
        m_vDeferredFns.push_back(std::move(fn));
        ::PostMessageW(m_hWnd, WM_UI_DEFERRED_EXEC, 0, 0);
    }
    void ShowAboutBox();
    void RefreshCheckText(CCheckBoxUI* pCheck);
    void LoadSkinGlyphs();

    // ==== replicated pages ====
    // ---- toolbox small tools (ported from DismPP.UI) ----
    bool RunBatFile(const CDuiString& sWorkDir, const wchar_t* pszBatName, const CDuiString& sBody,
                    HANDLE* phProcess = nullptr);
    // [原生化 B1] 系统修复：DismRestoreImageHealth 进程内 + sfc.exe 直连控制台
    void StartSystemRepair(bool bRestoreHealth, bool bRunSfc);
    // [修复 R5 / P1-11/P1-12] 详情/删除实现已移至下方新版声明

    // Windows optional features (Windows功能 tab)
    // [R5] OnProgTabChanged 薄转发已删：progtab_ 切换整段下沉
    // CPageProgram::OnEvent（经页宿主表代理 Capability/Feature）。
    // [D2] 列表“加载中”占位（旋转环 + 文案）
    CControlUI* MakeLoadingHint(const wchar_t* pszText, const wchar_t* pszRingName);
    // [D3] Appx 底栏已选计数（nTab=当前可见列表）
    // A-2: 过滤 / A-1: 详情（P4 起数据缓存/分组模式迁 CPageAppx）
    // [R5] ApplyProgFilter 已下沉 CPageProgram::OnEvent（prog_filter）。

    // ==== capability / update / assoc / unattend ====
    // P4：ParseCapabilities 随 DISM 线程迁入 AppxCapUpdService（S6）
    // UN-1/UN-2: 预应答 unattend.xml 落盘 + 导入导出

    // ==== cleanup actions ====
    // 扫描/清理工作线程运行期间禁用扫描/清理/刷新按钮，防止并发进入。
    // bScanMode=true（扫描中）时 btn_scan 保持可用并充当“停止扫描”按钮。
    // 关闭/复用工作线程句柄（DONE handler 与析构共用）
    void CloseWorkerThread();
    // [U-9] 定期清理计划任务
    void SetGroupGlyph(CCheckBoxUI* p, int nState);  // 组复选框三态字形：0/1/2

    // ==== optimization events (applied immediately) ====

    // ==== driver ====
    // [修复 R5 / P1-11] 点击驱动设备行 → 填充右侧 drv_detail 详情面板
    // [修复 R5 / P1-12] 「删除驱动」按钮 → pnputil /delete-driver

private:
    CDuiString  m_strXMLPath;
    // [R202] 展开后的皮肤 XML。GetSkinFile() 返回它的指针，
    //   因此必须与窗口同生命周期（不能是局部变量）。
    CDuiString  m_sExpandedSkin;
    CDuiString  m_strDataPath;
    int         m_nDPI;       // 当前 DPI，HostDPI() 返回此值

    // P6a：DISM 会话四件套（m_hDismProcess/m_hStdOutRead/m_hStdErrRead/
    // m_uThreadId）已随进程能力迁入 ExternalToolService（文件 static）。
    UINT_PTR    m_uTimerID;

    CDataEngine  m_dataEngine;
    CCleanEngine m_cleanEngine;
    // [R79] RegClean 注册表清理引擎
    CRegCleanEngine m_regCleanEngine;
    // [R80] Cleanup 深度扫描/残留/空文件夹/撤销引擎
    CCleanExtendEngine m_cleanExtendEngine;

    CDuiString   m_sGlyphBox[3];   // [修复 R5 / P1-6] 0空 1全选 2半选
    CDuiString   m_sGlyphArrow[2];
    CDuiString   m_sGlyphDash;
    CDuiString   m_sStatusReady;

    LogService                m_logSvc;        // P1：日志级别识别/审计落盘/离屏缓冲
    // [LAG-FIX] 待提交日志：120ms 合并一次 UI 批量 Add + 单次 fopen 落盘
    struct PendingLogEntry { CDuiString sOut; CDuiString sRaw; int nLevel; };
    std::vector<PendingLogEntry> m_vPendingLog;
    bool       m_bLogFlushArmed;
    // [L-01] m_pCachedOutput / m_nCachedOutPage 随 out_* 查找链路删除
    int        m_nPendingBuildPage; // 延迟构建目标页（-1=无）
    // [CRASHFIX-UAF] HostDefer 队列：页侧排入、WM_UI_DEFERRED_EXEC 消费。
    // 只在 UI 线程访问（入队点与执行点同线程），无需加锁。
    std::vector<std::function<void()>> m_vDeferredFns;
    // P7：m_vExpanded（优化分组折叠态）随系统优化页迁 CPageOptim
    // P6b：m_vCleanExpanded 随清理页迁 CPageCleanup（m_pgCleanup 内）
    // P5：m_vDriverExpanded / m_vDriverFilled / m_mapDrvPub / m_mapDrvRow /
    // m_bDrvPubEnum 随 CPageDriver 迁走（m_pgDriver 内）
    // P3b：m_startup / m_services / m_bStartupHideDisabled 随启动页
    // 迁 CPageStartup（m_pgStartup 内）
    // P2c：m_vAssocExt 随关联页迁 CPageAssoc（m_pgAssoc 内）
    // P3a：m_vPrograms 随程序页迁 CPageProgram（m_pgProgram 内）
    // P5：m_vFeatures / m_bFeatLoaded / m_nFeatTicks / m_sFeatWorkDir
    // 随 CPageFeature 迁走（m_pgFeature 内）

    bool m_bPopulating;
    // P6b：以下清理链路状态全部迁出壳——
    //   * 扫描/删除忙碌态、volatile LONG 取消令牌、唯一工作线程句柄、
    //     Custom ProcName 队列（m_aCustomProc[12]/m_vCustomJobs）→ CleanJobService（文件 static）
    //   * 行过滤通过标志 m_vCleanRowPass、#23 UI 大小快照 m_vUiSize、
    //     m_bCleanScanned/m_nCleanScannedCnt/m_nScanErrStats、过滤关键字
    //     m_sCleanFilter/m_bCleanShowZero、折叠态 m_vCleanExpanded、
    //     C-8 报告缓存 m_sCleanFullDetail/m_bCleanHasReport、
    //     R60-A 清理前空间 m_ullCleanBeforeBytes、勾选集合 m_vLastCleanSel、
    //     4 个通知 bool（DismComponentPending/DismResetBase/AssocNotify/
    //     RestartExplorer，改由 CleanDoneMsg 随线程消息带出）→ CPageCleanup
    bool m_bBufferFlushed;
    // P7：m_bFiltering / m_nOptimStateFilter / m_dwOptimVerifyTick
    //   随系统优化页迁 CPageOptim（关键字+状态过滤、5 秒 TTL 核实在页内）
    // P4：m_bAppxLoading/m_bCapLoading/m_bUpdLoading 随三页迁走
    // P3a：m_nProgSelected 随程序页迁 CPageProgram（Selected() 访问）
    // P5：m_nFeatSelected 随 CPageFeature 迁走
    int  m_nAfterAction;   // 选项→操作完成后: 0 未定义, 1 关机, 2 重启
    int  m_nLanguage;      // 选项→语言: 0 简体中文(默认), 1 英文, 2 日文(可选), 3 韩文(可选)
    DWORD m_dwStartupMs;    // [R51] 启动时间（毫秒）

    // P4：m_vCaps / m_vUpdList / m_nUpdSelected / m_nUpdWuaErr /
    // m_dtUpdScanStart / m_bCapRetryPending 随 CPageCapability / CPageUpdate 迁走
    bool                            m_bCapRefreshPending;   // DISM 工具结束后重查 FOD
    // P6b：m_ullCleanBeforeBytes 随清理页迁 CPageCleanup（m_pgCleanup 内）
    // P2：预应答页全部状态随 CPageUnattend 迁走（m_pgUnattend 内）

    // ---- P-P2：新版状态/标志成员（R59-R74 功能依赖）----
    // 离线安装路径与目标（InitWindow 读 config，页/服务按需访问）
    CDuiString                      m_sTempDir;             // 临时目录（FrameWnd_Util_GetTempDir）
    CDuiString                      m_sOfflinePath;         // 离线 ISO/文件夹根
    CDuiString                      m_sOfflineWin;          // 离线 Windows 目录
    CDuiString                      m_sOfflineSys;          // 离线 System32
    CDuiString                      m_sOfflineDrv;          // 离线 DriverStore
    CDuiString                      m_sOfflineDrvSrc;       // 驱动源目录
    CDuiString                      m_sOfflineSysSrc;       // 系统文件源目录
    CDuiString                      m_sOfflineAppx;         // Appx 源目录
    CDuiString                      m_sBackupPath;          // 备份根目录
    CDuiString                      m_sRestorePath;         // 还原点路径
    // 运行模式与特权（InitWindow 检测一次）
    bool                            m_bIsAdmin;             // 当前进程是否管理员
    bool                            m_bRunAdmin;            // 是否以管理员启动
    bool                            m_bRunUser;             // 是否以普通用户启动
    bool                            m_bDisableAll;          // 禁用所有功能（测试用）
    bool                            m_bNoReboot;            // 禁止重启（调试用）
    // 离线驱动扫描状态（R72）
    bool                            m_bOfflineDrivers;      // 是否离线驱动模式
    CDuiString                      m_sOfflineDrvXml;       // 离线驱动 XML 清单路径
    CDuiString                      m_sOfflineDrvWim;       // 离线驱动 WIM 路径

    // ---- P-P2：新版工具方法转发 ----
    void    GetTempDir(CDuiString& sOut);   // 等价 FrameWnd_Util_GetTempDir
    // [R4]/[R5] 17 个具体页值成员（16 页序 + Windows 功能子视图）统一下沉
    // CPageHost；壳仅经 PageById()+CPageBase 虚函数/命令与页通信，
    // R4 过渡具名访问器已在 R5 全部删除（壳→页具体类型直调清零）。
    CPageHost                       m_pages;

    // ==== [R18/R19] 交互增强运行时状态 ====
    CProgressUI*                    m_pSessProgress;    // 会话条动态进度条
    // P7：m_pOptimProgress/m_dwLastOptimMs/m_nOptimPendingVal/m_nOptimPendingMax
    //   随系统优化页迁 CPageOptim（XML 内置进度条抓取与 100ms 节流均在页内）
    bool                            m_bPageBuilt[16];   // 各页懒构建标志（含 PAGE_TASKS + Patch-3 4 新页）
    bool                            m_bWarmupStarted = false;  // [T4 P1] 启动预热已执行一次
    bool                            m_bWarmupArmed = false;    // [T4 P1] 预热定时器已挂载（防重复 SetTimer）
    // [B4] 最大化/还原离散边沿检测：仅在最大化进入或从最大化还原时，
    // 异步重建当前页（Cleanup/Optim/Startup 手算固定尺寸），拖拽缩放不触发。
    bool                            m_bWasMaximized = false;
    bool                            m_bRelayoutPosted = false;
    // RestartExplorer 重入守卫：重启期间嵌套泵送的消息/延迟消息若再次
    // 触发重启会造成 explorer 二次重启（二次黑屏）与 DuiLib 弹层 UAF
    // （CFG 0xc0000409 子码 0xa 崩溃），整个重启流程内只允许一个实例。
    bool                            m_bRestartingExplorer = false;
    NOTIFYICONDATAW                 m_sNid;             // 托盘图标数据
    bool                            m_bTrayAdded;       // NIM_ADD 是否已下发
    HWND                            m_hwndTip;          // tooltip 公共控件
    struct STipItem { CDuiString sName; CDuiString sText; CControlUI* pCtrl; };
    std::vector<STipItem>           m_vTips;            // 已注册 tooltip（含文本保活）
    bool                            m_bFocusGuard;      // 焦点高亮重入保护
    CControlUI*                     m_pFocusCtrl;       // 当前高亮焦点控件
    DWORD                           m_dwOldBorderColor; // 焦点控件原边框色（恢复用）
    int                             m_nOldBorderSize;   // 焦点控件原边框宽
    bool                            m_bOldBorderSet;
    // P7：m_vOptimUndo（上限 32）与 SOptimUndo 随系统优化页迁 CPageOptim

    // [U-10] 应用内 Toast 通知
    HWND                            m_hToastWnd;
    UINT                            m_uToastTimerId;
    HFONT                           m_hfToast;
    CDuiString                      m_sToastTitle;
    CDuiString                      m_sToastText;
    int                             m_nToastIcon;
    static CFrameWnd*               s_pToastOwner;

    // P8：页索引枚举 EPageId 已上移 PageBase.h（值序与本原 PageIndex 一致）。

    // P2b：TaskEntry / m_vTasks 已随 CPageTasks 迁走（Page_Tasks.h）

    // [R63]/[R5] 计划任务页面方法已随 CPageTasks 迁走；P2b 起的薄转发
    // 已在 R5 删除（寄宿 Tab2 事件由 CPageStartup::OnEvent 转发）。
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
