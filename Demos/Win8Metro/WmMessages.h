#pragma once

// ============================================================
// FrameWnd_Messages.h —— P0 跨编译单元消息/ID 单点定义
// FrameWnd.cpp 与 Win8Metro.cpp 必须同源的消息契约集中于此，
// 禁止在任何 .cpp 中再私自定义同名常量。
// ============================================================

#include <windows.h>

// ---- 工作线程 / 外部进程 → UI 线程消息（原 FrameWnd.h 顶部）----
inline constexpr UINT WM_DISM_LOG        = WM_USER + 100;
inline constexpr UINT WM_DISM_DONE       = WM_USER + 101;
inline constexpr UINT WM_DISM_PROGRESS   = WM_USER + 102;
inline constexpr UINT WM_SCAN_ITEM       = WM_USER + 110;
inline constexpr UINT WM_SCAN_DONE       = WM_USER + 111;
inline constexpr UINT WM_CLEAN_DONE      = WM_USER + 112;
inline constexpr UINT WM_APPX_DATA       = WM_USER + 120;
inline constexpr UINT WM_CAP_DATA        = WM_USER + 130;
inline constexpr UINT WM_UPD_DATA        = WM_USER + 131;
inline constexpr UINT WM_UPD_PROGRESS    = WM_USER + 132;   // [R74] 更新安装/卸载进度回调
inline constexpr UINT WM_REGCLEN_PROGRESS = WM_USER + 140;  // [R111] 注册表清理逐规则进度（wParam=int* 总数 / lParam=int* 已完成；[R16][T3] 投递方成对 new、接收方无条件成对 delete）
inline constexpr UINT WM_REGCLEN_DONE    = WM_USER + 141;  // [R111] 注册表清理完成（RegCleanDoneMsg*）
inline constexpr UINT WM_REGCLEN_SCANDONE = WM_USER + 144; // [R-EXTASYNC] 注册表扫描完成（RegCleanScanDoneMsg*）
inline constexpr UINT WM_APPX_DEPDATA    = WM_USER + 142;   // [R-B25] Appx 依赖 TSV 回读完成（触发 UI 刷新）
inline constexpr UINT WM_DIAG_LOG        = WM_USER + 143;   // [LAG-FIX] 后台诊断（启动事件/服务快照）完成，lParam=new CDuiString
inline constexpr UINT WM_TASK_DATA       = WM_USER + 150;   // [R-B84] 计划任务后台枚举完成，lParam=CPageTasks::TaskQueryMsg*（new，UI 线程 delete）
inline constexpr UINT WM_FEAT_DATA       = WM_USER + 160;   // [P0-A3] Windows 功能后台枚举完成，lParam=FeatDataMsg*（new，UI 线程 delete）
inline constexpr UINT WM_FEAT_APPLYDONE  = WM_USER + 161;   // [P0-A3] Windows 功能批量应用完成，lParam=FeatApplyMsg*（new，UI 线程 delete）
inline constexpr UINT WM_WOF_LOG         = WM_USER + 170;   // [P0-A2] WOF 压缩后台线程日志，lParam=new CDuiString（UI 线程 delete）
inline constexpr UINT WM_DRV_PUBDATA     = WM_USER + 171;   // [卡顿修复] 驱动页第三方驱动库后台枚举完成，lParam=TDrvPubMsg*（new，UI 线程 delete）
// [AUDIT-P0] 驱动删除后台完成：lParam=TDrvDelMsg*（new，UI 线程 delete）。
// 让 DismRemoveDriver 的 CBS 事务离开 UI 线程，消除批量删除时的界面冻结。
inline constexpr UINT WM_DRV_DELDATA     = WM_USER + 172;   // [AUDIT-P0] 驱动删除后台完成，lParam=TDrvDelMsg*（new，UI 线程 delete）
// [AUDIT-P0] 清理页 DISM 收尾（Custom 派发 + WinSxS 组件清理）后台完成：
// lParam=TPendingCleanTail*（new，UI 线程 delete）。
// CleanComponentStore 可耗时数分钟，绝不能在 WM_CLEAN_DONE 回调里同步跑。
inline constexpr UINT WM_CLEAN_DISMTAIL  = WM_USER + 173;   // [AUDIT-P0] 清理页 DISM 收尾后台完成，lParam=TPendingCleanTail*（new，UI 线程 delete）
// [FIX-PERF] 程序和功能后台枚举完成：lParam=ProgEnumCtx*（new，UI 线程 delete）
inline constexpr UINT WM_PROG_DATA       = WM_USER + 183;
// [INTERACT-FIX] 程序批量卸载后台完成：lParam=ProgBatchMsg*（new，UI 线程 delete）。
// 原实现逐条在 UI 线程同步 WaitForSingleObject(卸载向导进程, 120s)，勾选 N 条
// 最长冻结 N×120 秒——本程序最大的交互冻结面。改为工作线程执行、完成回投。
inline constexpr UINT WM_PROG_BATCHDONE  = WM_USER + 184;
// [R-CLEANPROG] 空间回收删除阶段进度：lParam=CleanProgressMsg*（new，UI 线程 delete）。
// 删除线程按"规则重评 / 删除批次"两个阶段回报 done/total，UI 驱动会话条
// 进度与任务栏进度；此前删除阶段只有"正在删除"文案、无进度可看。
inline constexpr UINT WM_CLEAN_PROGRESS  = WM_USER + 185;
inline constexpr UINT WM_CAP_PARTIAL     = WM_USER + 180;   // [P1] 可选功能枚举分批回报，lParam=CapPartialMsg*（new，UI 线程 delete）
inline constexpr UINT WM_FEAT_PARTIAL    = WM_USER + 181;   // [P1] 功能枚举分批回报，lParam=FeatPartialMsg*（new，UI 线程 delete）
inline constexpr UINT WM_UPD_PARTIAL     = WM_USER + 182;   // [P1] 更新枚举分批回报，lParam=UpdPartialMsg*（new，UI 线程 delete）

// 托盘图标回调（Shell_NotifyIcon uCallbackMessage）
inline constexpr UINT WM_TRAYNOTIFY      = WM_APP + 1;

// 页侧（优化页 combo/开关/批量、清理完成编排之外的所有 UI 点击路径）
// 请求重启 explorer 的统一延迟消息。必须 PostMessage 到主窗口、由壳在
// 输入消息栈排空后执行，严禁在鼠标消息处理栈深处同步调
// CFrameWnd::RestartExplorer：该函数会长时间嵌套泵送消息并杀掉 shell，
// 栈上的 DuiLib 弹层（combo 下拉/菜单/阴影窗口）会在嵌套泵中半销毁，
// 重启后主循环派发的陈旧 WM_LBUTTONDOWN 命中失效 WndProc 指针，触发
// CFG FAST_FAIL_GUARD_ICALL_CHECK_FAILURE（0xc0000409 子码 0xa）崩溃，
// 重入还会导致 explorer 二次重启（二次黑屏）。dump 实证：故障消息
// WM_LBUTTONDOWN，崩溃于 user32!CallWindowProcW 内 CFG 校验。
inline constexpr UINT WM_HOST_REQUEST_RESTART_EXPLORER = WM_APP + 120;

// [CRASHFIX-UAF] 页侧延迟 UI 重建的统一执行消息（与上一条同一契约体系）。
// 场景：行控件在 BUTTONUP 事件派发栈内触发"整表重建"（RemoveAll 删除正在
// 派发的控件），DoEvent 返回后基类继续解引用已释放的 this → UAF 崩溃。
// 页侧改为 IPageContext::HostDefer 把重建排入本消息，待事件栈完全退清后
// 由壳在 HandleCustomMessage 中统一执行。严禁在 HostDefer 回调里再同步
// 触发新一轮点击事件派发。
inline constexpr UINT WM_UI_DEFERRED_EXEC = WM_APP + 121;

// ============================================================
// [GC-06 收敛] 工具箱/弹窗后台作业消息统一注册表
// 原先散落在 ToolDialog.cpp / Page_Tools.cpp 的 8 个私有号一律
// 在此单点注册，禁止任何 .cpp 再私自定义同名/同号常量。
// 所有权契约（lParam 堆对象一律由 UI 线程接收方在唯一出口 delete）：
//   WM_TDLG_TASKDONE  wParam=0, lParam=0（遮罩结束后的刷新信号，无堆对象）
//   WM_TDLG_ESDDONE   lParam=TEsdTaskCtx*（派生 TJobDoneEnvelope；new，接收方 delete）
//   WM_TDLG_JOBDONE   lParam=TJobResult*（派生 TJobDoneEnvelope；new，接收方 delete）
//   WM_TDLG_JOBLOG    wParam=level, lParam=new CDuiString（接收方 delete）
//   WM_TP_PROGRESS    wParam=0..100 百分比，lParam=0
//   WM_TP_STAGETEXT   lParam=new CDuiString 阶段文案（接收方 delete）
//   JOB_WM_DONE       wParam=0, lParam=TJobDoneEnvelope*（new，接收方 delete；
//                     [R16-T9] 统一信封，原 WEC_WM_DONE/ISOJOB_WM_DONE 并入此
//                     单消息号，三套完成 sink 收敛为 BackgroundJobs.h CJobDoneSink）
//   JOBWM_PROGRESS    wParam=0..100, lParam=new std::wstring 阶段文案（可空）
// 投递方必须在 IsWindow/PostMessage 失败路径自行 delete（见
// BackgroundJobs::PostHeapOrDelete），严禁静默丢弃造成泄漏。
// ============================================================
inline constexpr UINT WM_TDLG_TASKDONE   = WM_APP + 0x5501;  // CToolDialogPage 通用任务完成
inline constexpr UINT WM_TDLG_ESDDONE    = WM_APP + 0x5502;  // CToolDialogPage ESD 线程完成
inline constexpr UINT WM_TDLG_JOBDONE    = WM_APP + 0x5503;  // CToolDialogPage 映像作业完成
inline constexpr UINT WM_TDLG_JOBLOG     = WM_APP + 0x5504;  // CToolDialogPage 作业日志行
// [FIX-P2N] 更新阶段后台完成：StartUpdatePhase 的子进程等待移入后台线程，
// 结束后经本消息回 UI 收口。lParam = new UpdPhaseDoneMsg（kMsgOwnPage）。
// 0x5510/0x5511 已被 WM_TP_PROGRESS/WM_TP_STAGETEXT 占用，顺延取 0x5512。
inline constexpr UINT WM_UPD_PHASEDONE   = WM_APP + 0x5512;
// [FIX-T2] 页面后台作业完成弹窗：工作线程收口统一走本消息，壳拆
// "标题|文本|图标(0=INFO,2=WARNING,3=ERROR)" 后调 HostAlert。
// lParam = new CDuiString（kMsgOwnShell，壳侧唯一释放出口）。
inline constexpr UINT WM_PAGE_ALERT      = WM_APP + 0x5520;
inline constexpr UINT WM_TP_PROGRESS     = WM_APP + 0x5510;  // 模态进度窗：百分比
inline constexpr UINT WM_TP_STAGETEXT    = WM_APP + 0x5511;  // 模态进度窗：阶段文字
inline constexpr UINT JOB_WM_DONE        = WM_APP + 0x711;   // [R16-T9] 统一作业完成（CJobDoneSink）
inline constexpr UINT JOBWM_PROGRESS     = WM_APP + 0x720;   // Page_Tools 通用进度窗百分比/文案
// [R104] 进度窗"取消"按钮 → 作业的取消请求（wParam/lParam 均 0）。
//   作业侧不直接接这个消息，而是在进度回调里查 BackgroundJobs::CancelToken。
inline constexpr UINT JOBWM_CANCEL_REQ   = WM_APP + 0x721;   // 请求取消当前作业
