#pragma once

// ============================================================
// FrameWnd_Messages.h —— P0 跨编译单元加速键/壳命令/定时器 ID 单点定义
// 业务服务仅依赖 WM_* 常量时请 include "WmMessages.h"（纯值头，见下），
// 本头保留 UI 层专属定义，避免 Service 反向 include UI 层。
// ============================================================

#include "WmMessages.h"

// ---- 加速键命令 ID（Win8Metro.cpp 加速键表 → WM_COMMAND）----
enum class EAccelCmd : int {
    ID_ACCEL_SCAN   = 41001,  // F9  开始扫描
    ID_ACCEL_CLEAN  = 41002,  // F10 开始清理
    ID_ACCEL_REFRESH= 41003,  // F5  刷新
    ID_ACCEL_CANCEL = 41004,  // Esc 取消扫描
    ID_ACCEL_FIND   = 41005,  // Ctrl+F 聚焦本页搜索框
    ID_ACCEL_UNDO   = 41006,  // Ctrl+Z 撤销最近一次优化设置
    // [R4] 快捷键扩展
    ID_ACCEL_NEW       = 41007,  // Ctrl+N
    ID_ACCEL_SAVE      = 41008,  // Ctrl+S
    ID_ACCEL_SELECT_ALL= 41009,  // Ctrl+A
    ID_ACCEL_CUT       = 41010,  // Ctrl+X
    ID_ACCEL_COPY      = 41011,  // Ctrl+C
    ID_ACCEL_PASTE     = 41012,  // Ctrl+V
    ID_ACCEL_REDO      = 41013,  // Ctrl+Y
    // [R-B1] 原版 7 条补齐：Ctrl+O / Ctrl+Shift+S / Ctrl+Shift+N / Ctrl+Break / Delete
    ID_ACCEL_OPEN      = 41014,  // Ctrl+O 打开规则目录（原版"打开"）
    ID_ACCEL_SAVEAS    = 41015,  // Ctrl+Shift+S 另存为（导出当前页配置）
    ID_ACCEL_NEWWINDOW = 41016,  // Ctrl+Shift+N 新实例（当前已单实例则聚焦）
    ID_ACCEL_BREAK     = 41017,  // Ctrl+Break 中断当前扫描/清理
    ID_ACCEL_DELKEY    = 41018   // Delete 删除选中项（各页 OnDeleteKey）
};

// ---- [R5] 壳→页内部命令（CPageBase::OnHostCommand 扩展）----
// 与裸菜单 ID（1101..1205）/ID_ACCEL_* 同一入口，但仅由壳内部经
// PageById(...)->OnHostCommand() 发起，不进 WM_COMMAND；高段 411xx
// 避开 ID_ACCEL_* 41001..41018 与菜单号。
enum class EHostCmd : int {
    HCMD_OPTIM_VERIFY_FORCE   = 41101,  // CPageOptim::VerifyOptimStates(true)
    HCMD_OPTIM_VERIFY_SOFT    = 41102,  // CPageOptim::VerifyOptimStates(false)（延迟构建后）
    HCMD_OPTIM_INIT_PROGRESS  = 41103,  // CPageOptim::InitProgress()
    HCMD_UPD_SCAN_STARTED     = 41104,  // CPageUpdate::OnScanStarted()
    HCMD_CLEAN_SELECT_DEFAULT = 41105   // CPageCleanup::CleanSelectDefault()
};

// ---- 单实例 / 语言切换重启接力事件名（FrameWnd.cpp 与 Win8Metro.cpp 强约定）----
// 内核事件按名称匹配，头文件内 internal linkage 的副本值一致即等价。
inline constexpr auto kRestartEventName = L"Local\\Win8Metro_Restarting_v1";

// ---- CFrameWnd 自建定时器 ID ----
// WM_TIMER 处理只允许拦截以下 ID；其余（含 DuiLib 滑块拖拽 50ms 定时器）
// 必须放行给 m_pm.MessageHandler。
static constexpr UINT FEAT_TIMER_ID = 9527;  // 功能页 DISM 轮询
// [R2-FIX] RING_TIMER_ID 已废弃（曾是 Appx + 可选功能 + Windows 功能三页共用，
// KillTimer 跨页互杀导致"还在加载但转环停了"视觉缺陷）。
// 现各页独立 ID，OnTimerRing handler 按控件名查找目标，与 ID 解耦。
static constexpr UINT APPX_RING_TIMER_ID = 9528;  // [R2-FIX] Appx 包列表转环动画
static constexpr UINT CAP_RING_TIMER_ID   = 9544;  // [R2-FIX] 可选功能页转环动画
static constexpr UINT FEAT_RING_TIMER_ID  = 9534;  // Windows 功能页转环动画
static constexpr UINT CAP_REFRESH_TIMER_ID = 9529;  // [R50] 可选功能操作后自动刷新
static constexpr UINT CAP_RETRY_TIMER_ID = 9530;  // [R58-D] 可选功能失败自动重试（60s）
static constexpr UINT TOOL_PROGRESS_TIMER_ID = 9531; // [R107] 工具箱统一进度条轮询（800ms）
static constexpr UINT LOG_FLUSH_TIMER_ID = 9532;  // [LAG-FIX] 日志 120ms 批量提交
static constexpr UINT PAGE_BUILD_TIMER_ID = 9533;  // [LAG-FIX] 切页后一次性延迟构建
static constexpr UINT CAP_WATCHDOG_TIMER_ID = 9534;  // [rb23] 可选功能枚举超时兜底（120s）
static constexpr UINT FEAT_WATCHDOG_TIMER_ID = 9535;  // [rb23] Windows 功能枚举超时兜底（120s）
static constexpr UINT CAP_RENDER_BATCH_TIMER_ID = 9536; // [R16][O-2] 可选功能列表分批渲染接力（50 行/批）
static constexpr UINT UPD_FILTER_TIMER_ID = 9537; // [性能 C1] 更新页过滤防抖（250ms 一次性）
static constexpr UINT UPD_RENDER_BATCH_TIMER_ID = 9538; // [性能 B1] 更新列表分批渲染接力（50 行/批）
// [T4 P1] 启动预热一次性定时器：启动 5s 后在后台预建共享在线枚举会话并补枚举，
// 用户进页时直接命中已建好的会话与缓存，省掉首帧的会话建立 + 首次枚举等待。
static constexpr UINT WARMUP_TIMER_ID = 9539;
// [T8 P3] 功能页列表分批渲染接力（50 行/批 + 15ms，对齐 Upd 页 UpdStartBatchRender）
static constexpr UINT FEAT_RENDER_BATCH_TIMER_ID = 9540;
static constexpr UINT PROG_RENDER_TIMER_ID = 9542;   // [FIX-PERF] 程序和功能首屏分帧接力
static constexpr UINT PROG_FILTER_TIMER_ID = 9543;   // [FIX-PERF] 程序搜索防抖
// [FIX-UNFREEZE] 清理尾巴看门狗：DISM 尾巴完成消息（WM_CLEAN_DISMTAIL）
// 极端情况下投递全败时，壳侧唯一一次 HostOnCleanDoneTail 已被 InFlight
// 挡回 —— 本 1s 轮询兜底在 InFlight 清除后强制收尾，UI 解冻不依赖投递成功。
static constexpr UINT CLEAN_TAIL_WATCHDOG_TIMER_ID = 9545;
