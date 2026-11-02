#pragma once
// ============================================================================
// ExternalCmdLedger.h - [R-B56] 外部命令防腐台账
// 清单维度 3 缺失 5『外部命令路线与"全内置"基线分叉』：
//   「命令清单集中化防腐」——集中登记所有 ProcessRunner 外部进程调用，
//   供审计导出（CSV）与后续切换原生 API 的优先级参考。
// ============================================================================
//
// 设计：
//   1) 不改任何 ProcessRunner 调用点（零侵入）。
//   2) 由 ProcessRunner::RunHidden / RunHiddenExit / RunCapture /
//      RunHiddenCapture / RunCaptureBytes 五个入口统一在记账器登记。
//   3) 分类：按 exe 白名单归类（dism/powershell/cmd/sfc/vssadmin/compact/
//      wimlib/其他）；未列入白名单的归"OTHER"。
//   4) 线程安全：std::mutex 保护；Snapshot 深拷贝返回。
//
// API:
//   ExternalCmdLedger::Note(exe, args)      登记一次外部命令调用（自动分类）
//   ExternalCmdLedger::Snapshot()           返回全部记录（时间倒序）
//   ExternalCmdLedger::CountByExe()         返回 exe -> 调用次数
//   ExternalCmdLedger::DumpCsv(path)        导出 UTF-8 CSV（含 BOM）
//   ExternalCmdLedger::Clear()              清空（测试用）
// ============================================================================

#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <mutex>

namespace ExternalCmdLedger {

// 单条记录
struct Entry {
    std::wstring  exe;        // 可执行文件名（不含路径）
    std::wstring  args;       // 原始参数串（可能较长）
    std::wstring  category;   // 分类标签
    SYSTEMTIME    ts;         // 登记时间戳
    unsigned long pid;        // 本进程 PID
};

// 记账
void Note(const wchar_t* pszExe, const wchar_t* pszArgs);

// 返回副本
std::vector<Entry> Snapshot();

// exe -> count
std::map<std::wstring, int> CountByExe();

// 按分类统计（category -> count）
std::map<std::wstring, int> CountByCategory();

// 导出 UTF-8 CSV（带 BOM，Excel 友好）；返回 true 成功
[[nodiscard]] bool DumpCsv(const std::wstring& sPath);

// 清空
void Clear();

} // namespace ExternalCmdLedger