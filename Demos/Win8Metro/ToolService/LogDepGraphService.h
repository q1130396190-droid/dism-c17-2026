#pragma once

// ============================================================
// LogDepGraphService.h —— [R6-02] 查看日志(EToolKind::TK_LOGVIEW) 与
// 依赖图(EToolKind::TK_DEPGRAPH) 两弹窗的无 UI 业务：
//   * action_log.jsonl 路径/读取解析/清空；
//   * 依赖节点 -> 缩进树视图行 / 可复制纯文本。
// 渲染（C*UI 控件、行高测量、剪贴板、打开目录）留 ToolDialogPage。
// ============================================================

#include <string>
#include <vector>

#include "IToolService.h"


// [L-11] 级别枚举不再在这里重复定义一份新的 enum。
//   原来工程里有三份同值枚举：HostInterfaces.h 的 ELogLevel(LOG_INFO/LOG_WARN/
//   LOG_ERROR)、本文件的 toolsvc::ELogLevel(kLogInfo/kLogWarn/kLogError)、
//   以及 LogService 落盘侧的 ELogSeverity。三份靠注释维持同值，任一处改数
//   就会静默漂移。
//
//   处置：本文件不再定义 enum，直接引用 HostInterfaces.h 那份（全局作用域）。
//   但不 include HostInterfaces.h —— 它要经 ToolDialogPage.h 拉进整个 UI 层，
//   而本服务是"无 UI 业务"（见文件头注释），反向依赖会破坏分层。
//   因此这里用**显式数值别名**表达同一契约：
//     值必须与 ELogLevel { LOG_INFO=0, LOG_WARN=1, LOG_ERROR=2 } 一致；
//   HostInterfaces.h 里已加 static_assert 绑定本文凭（见该文件对应位置），
//   任一处改数都会在编译期报错，不再依赖注释维持。
namespace toolsvc {
namespace ELogLevel {
inline constexpr int kLogInfo  = 0;   // == ::ELogLevel::LOG_INFO
inline constexpr int kLogWarn  = 1;   // == ::ELogLevel::LOG_WARN
inline constexpr int kLogError = 2;   // == ::ELogLevel::LOG_ERROR
} // namespace ELogLevel

// 已解析并格式化好的一行日志（Page 直接 SetText）。
struct TLogLine
{
    int          nLevel = static_cast<int>(ELogLevel::kLogInfo);
    std::wstring sText;     // "[ts] TAG  msg"，内嵌换行已规范为 \n
};

// [R16][T4] TDepNode 已迁至 ToolTypes.h（ToolOpenArgs 携带依赖图数据）。

// 依赖图一行视图（Page 据此着色/字体）。
struct TDepRow
{
    std::wstring sText;     // 含缩进/符号的整行显示文本
    std::wstring sLabel;    // 原始标签（对应旧 tooltip，不含缩进/符号）
    bool         bRoot = false;
};

// 依赖图一次构建结果。
struct TDepView
{
    std::vector<TDepRow> rows;
    int                  nRoots = 0;
    int                  nDeps = 0;
    bool                 bHasNodes = false;  // 原节点数组非空（即使 label 全空）
};

class CLogDepGraphService : public IToolService
{
public:
    explicit CLogDepGraphService(IToolServiceHost* pHost) : m_pHost(pHost) {}

    int Kind() const override { return static_cast<int>(EToolKind::TK_LOGVIEW); }

    // ---- 查看日志 ----
    // EXE 同目录 action_log.jsonl 完整路径。
    std::wstring LogPath() const;
    // 读取并解析全部有效行；文件不存在返回 false（Page 显示空态）。
    bool ReadLogLines(std::vector<TLogLine>& vOut) const;

    // [L-26][L-28] 三源合并读取：审计流全量 + 诊断底账各尾 N 行 +
    //   工具归档最近若干次运行，按源分组、带分节头。
    //   不做全局时间排序：纯文本行没有可靠时间戳，排序即猜测；
    //   审计流内部本身已天然时序，分节呈现更诚实。
    //   输出里分节头是 nLevel = -1 的"标题行"，UI 据此画灰粗体。
    bool ReadMergedLines(std::vector<TLogLine>& vOut) const;

    // 分节头标记（nLevel 取此值表示不是日志行，而是分组标题）
    static constexpr int kSectionHeader = -1;
    // 截断清空日志文件；文件无法创建也算成功（旧逻辑忽略失败）。
    void ClearLogFile() const;

    // ---- 依赖图 ----
    // [R16][T4] 节点经 IToolService::Open(ToolOpenArgs.vDepNodes) 注入，
    // 旧 SetDepNodes 入口废弃。
    void Open(const ToolOpenArgs& args) override;
    TDepView BuildDepView() const;
    // "*/-" 缩进纯文本（复制到剪贴板用）；无任何可见节点时返回空串。
    std::wstring BuildDepCopyText() const;

private:
    IToolServiceHost*      m_pHost;
    std::vector<TDepNode>  m_vNodes;
};

} // namespace toolsvc
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
