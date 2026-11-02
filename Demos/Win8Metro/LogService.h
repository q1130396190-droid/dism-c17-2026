#pragma once

// ============================================================
// LogService —— 统一日志入口（2026 结构化日志基线）
//
// 本类是**全工程唯一的日志落盘入口**：
//   · 任何线程都可以直接调用（内部互斥 + 单次 fopen，无并发半行）；
//   · 显式 UTF-8 编码落盘（不再依赖 C locale 的 wofstream，中文不再被截断）；
//   · 统一日志目录（EXE\Logs，不可写时按回退链降级，永不静默丢弃）；
//   · JSONL 字段：ts(毫秒+时区) / level / msg / module / tid；
//   · 写入失败有计数与一次性告警，不再"全静默"。
//
// 设计约束：
//   · 不 include 任何 UI 头，可在插件线程/服务层使用；
//   · 级别枚举只在此处定义一次（ELogSeverity），其它模块引用别名，不再各写一套。
// ============================================================

#include <windows.h>

#ifndef UILIB_API
#define UILIB_API __declspec(dllimport)
#endif

#include <mutex>
#include <string>
#include <vector>
#include <utility>
#include "Utils/Utils.h"

// ------------------------------------------------------------
// 级别模型：补齐 TRACE/DEBUG/FATAL，运行时可按最低级别过滤
// 数值顺序即严重度顺序，过滤判断用 >= 比较，不要任意改动。
//
// 为什么叫 ELogSeverity 而不叫 ELogLevel：
//   HostInterfaces.h 已有一个 UI 侧枚举 `ELogLevel { LOG_INFO, LOG_WARN,
//   LOG_ERROR }`（118 处调用点都在用），两者同名会直接编译冲突。
//   本枚举是"落盘侧的完整级别集"，UI 侧那个是"进度消息的分级"，
//   语义不同层级不同，因此各留其名，并在此标明映射关系：
//     LOG_INFO → kInfo / LOG_WARN → kWarn / LOG_ERROR → kError
//   数值有意保持 0/1/2 对齐，旧的 int 传参（0/1/2）可直接强转，
//   这也是 LogService::Write 能兼容历史调用点的原因。
// ------------------------------------------------------------
enum class ELogSeverity : int
{
    kTrace = 0,
    kDebug = 1,
    kInfo  = 2,
    kWarn  = 3,
    kError = 4,
    kFatal = 5,
};

// 兼容旧调用点：历史上用 0/1/2 表示 info/warn/error。
// 这里保留三个常量名，但值映射到新枚举，语义不变。
inline constexpr int LOGSRV_INFO  = static_cast<int>(ELogSeverity::kInfo);
inline constexpr int LOGSRV_WARN  = static_cast<int>(ELogSeverity::kWarn);
inline constexpr int LOGSRV_ERROR = static_cast<int>(ELogSeverity::kError);

class LogService
{
public:
    // ---- 路径与目录 ----
    // 统一日志目录：首选 <EXE>\Logs（建目录 + 试写探针判可写），
    // 不可写时按 %LOCALAPPDATA%\DismTool\Logs → %USERPROFILE%\DismTool\Logs
    // → %TEMP% 逐级回退。首次调用惰性初始化并执行一次性迁移，线程安全。
    static const std::wstring& UnifiedLogDir();
    // 统一目录下的具体文件完整路径
    static std::wstring UnifiedLogPath(const wchar_t* pszFileName);

    // ---- 级别 ----
    // 从行文本识别分级（[ERROR]/[WARN] 标签，不区分大小写）。
    // 保留为兼容兜底：新代码应直接传级别，不要靠文本猜。
    static int DetectLevel(const wchar_t* pszLine);

    // 运行时最低级别（低于该级别的日志直接丢弃）。默认 kInfo。
    static void SetMinLevel(ELogSeverity lv);
    static ELogSeverity GetMinLevel();

    // ---- 唯一写入口 ----
    // 任意线程可调用。pszModule 为空时记为 "app"。
    static void Write(ELogSeverity lv, const wchar_t* pszModule, const wchar_t* pszMsg);
    // 便捷包装（级别取自文本标签，供未改造的旧调用点逐步迁移）
    static void WriteAuto(const wchar_t* pszMsg);

    // ---- 写入健康度 ----
    // 累计写失败次数（>0 说明日志可能不完整，可在诊断页展示）
    static unsigned long long FailCount();
    // 是否发生过"首选目录不可写、已回退"的情况
    static bool FellBack();

    // [L-15] 取走"需要弹给用户看"的一次性告警。
    //   本服务不持有任何 UI，也无权弹窗（插件线程/服务层调用时更没有窗口），
    //   因此这里只登记标志，由宿主在合适的时机（如主窗口定时器）取走并弹 Toast。
    //   返回 true 表示这是首次取走，调用方应弹一次；之后返回 false，不重复打扰。
    static bool TakePendingAlert(std::wstring& sText);

    // ---- 内存审计环（供 UI 查询，替代已失效的 out_ 链路）----
    static void   RingPush(const wchar_t* pszLine, ELogSeverity lv);
    static size_t RingCount();
    // 返回值拷贝：内部持锁读取，引用会在锁释放后失效
    static const DuiLib::CDuiString RingAt(size_t i);
    static void   RingClear();

    // ---- 离屏回放缓冲（页面不可见期间的短缓存）----
    void BufferOffline(const wchar_t* pszLine);
    size_t BufferedCount() const;
    const DuiLib::CDuiString& BufferedAt(size_t i) const;
    void ClearBuffered();

    // ---- 实例入口（保留旧签名，内部转调静态 Write）----
    void AppendToFile(const wchar_t* pszLine, int nLevel);
    void AppendToFileBatch(const std::vector<std::pair<DuiLib::CDuiString, int> >& vItems);

private:
    std::vector<DuiLib::CDuiString> m_vLogBuffer;   // 离屏回放缓冲（≤300）

    // 静态资源的互斥量：文件写、审计环、一次性初始化共用
    // （递归不必要：内部实现不回调自身公开接口）
    static std::mutex& Mutex();
};
