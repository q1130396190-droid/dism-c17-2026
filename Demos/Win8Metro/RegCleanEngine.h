#pragma once

// ============================================================
// RegCleanEngine.h —— [R79] 注册表清理引擎
//
// 四件套之二：规则集加载 + 扫描 + 清理 + 备份
//   * LoadRules()    从 Data.xml 优化根解析含 Reg* 子节点的 Item
//   * ScanRules()    逐条检测目标值是否真实存在
//   * CleanRules()   按勾选集执行 RegDelete/RegWrite/RegMove
//   * BackupRules()  清理前把将要改动/删除的键值快照写入 .reg 备份
//
// 依赖：
//   * CDataEngine::ParseRegPath / OpenRegKey / RegClean* facade
//     （复用已存在的注册表实现，避免复制逻辑）
//   * 纯 Service 层，不 include UIlib.h / StringManager.h，
//     文案与交互一律走返回码 / 结构体快照，由 Page 侧翻译。
// ============================================================

#include <vector>
#include <windows.h>

#include "RegCleanRule.h"

namespace DuiLib { class CMarkupNode; }
class CDataEngine;

// 扫描结果：单条规则
struct RegCleanScanResult
{
    int         iRule;        // 规则索引
    int         nState;       // 0 无需清理 / 1 可清理 / 2 不适用
    int         nActions;     // 将执行的条目数
    CDuiString  sFirstPath;   // 首个目标键（调试用）
};

// 扫描汇总
struct RegCleanScanSummary
{
    RegCleanScanSummary() : nRules(0), nApplicable(0), nCleanable(0), nCleanActions(0) {}
    int nRules;
    int nApplicable;
    int nCleanable;
    int nCleanActions;
};

// 清理结果：单条动作
struct RegCleanExecResult
{
    RegCleanExecResult() : iRule(-1), nSucceeded(0), nFailed(0) {}
    int         iRule;
    int         nSucceeded;
    int         nFailed;
    CDuiString  sFirstError;
    CDuiString  sRuleName;      // [R131-1] 规则名回传（D3 证据：失败规则定位）
};

class CRegCleanEngine
{
public:
    CRegCleanEngine();
    ~CRegCleanEngine();

    // ---- 1. 规则集加载 ----
    // 遍历 SystemOptimization 根下 Item，凡子树含 RegWrite/RegDelete/RegMove
    // 者加入 m_rules。返回加载条数。
    int LoadRules(CDataEngine* pEngine);

    int GetRuleCount() const { return static_cast<int>(m_rules.size()); }
    RegCleanRule& GetRule(int iIdx) { return m_rules[iIdx]; }
    const RegCleanRule& GetRule(int iIdx) const { return m_rules[iIdx]; }

    // ---- 2. 扫描 ----
    // 逐条规则检测目标键值是否存在（UI 线程调用，注册表操作较快；
    // 大数据量规则数 ≤ 数百条，无需工作线程）。
    int ScanRules(std::vector<RegCleanScanResult>& out, RegCleanScanSummary& summary);

    // ---- 3. 清理 ----
    // 对勾选且可清理的规则执行动作；返回失败条目数（0 = 全部成功）。
    // pBackup 可选：先备份再清理，便于撤销。
    int CleanRules(const std::vector<int>& vSel, RegCleanBackup& backup,
                   std::vector<RegCleanExecResult>& outResults);

    // ---- 4. 备份 ----
    // 把将要清理的键值快照写入 .reg 文件（REGEDIT4 格式）。
    // 返回 false 表示无法创建备份文件（清理应中止）。
    bool BackupRules(const std::vector<int>& vSel, RegCleanBackup& backup,
                     const wchar_t* pszOutDir = nullptr);

    // 备份文件路径访问器
    static bool BackupPath(const wchar_t* pszOutDir, CDuiString& outPath);

    // [R118-1] 创建系统还原点（动态加载 srclient.dll + SRSetRestorePointW）。
    // 用于清理前系统保护。返回分级码：
    //   0 成功；1 未启用系统保护（无法创建）；2 权限不足（需 SeRestorePrivilege）；
    //   3 srclient.dll 加载失败；4 SRSetRestorePointW 调用失败；5 参数无效。
    static int  CreateRestorePoint(const wchar_t* pszDesc);

    // [R118-1] 返回最近一次 CreateRestorePoint 的 nStatus（STATEMGRSTATUS）
    static DWORD GetLastRestoreStatus() { return s_lastRestoreStatus; }
    static void  SetLastRestoreStatus(DWORD s) { s_lastRestoreStatus = s; }

    // ---- 5. 撤销 ----
    // 用备份恢复上次清理前的值（RegCleanBackupEntry.sExisted=false 时删除）。
    int RestoreBackup(const RegCleanBackup& backup, std::vector<CDuiString>& outErrors);

    // [P1-收尾] 原生 .reg 文件导入（替代 reg.exe import 外部进程）。
    // 支持 "Windows Registry Editor Version 5.00"（UTF-16LE，含/不含 BOM、
    // UTF-8）与 "REGEDIT4"（ANSI）两种文件；支持键创建/键删除（[-...]）、
    // 值删除（=-）、REG_SZ/EXPAND_SZ/MULTI_SZ/BINARY/DWORD/QWORD 及
    // hex(n): 全部标准类型，支持反斜杠折行与字符串转义。
    // bWow64=true 时按 32 位视图写入（等价 reg /reg:32）。
    // 返回 0 = 成功；非 0 = 失败条目数，psErr 回传首个错误说明。
    static int  ImportRegFile(const wchar_t* pszRegFile, bool bWow64 = false,
                              CDuiString* psErr = nullptr);

private:
    // [B3] 条件门控收集：只取 False（还原）分支动作；任意 Applicable
    // 不成立即剪枝，True（开启）分支永不收集。
    void GatedCollect(DuiLib::CMarkupNode node, class CDataEngine* pEng,
                      std::vector<RegCleanAction>& out);

    // 解析 RegWrite 的 Type 字符串 -> REG_*
    static DWORD ParseRegType(const wchar_t* pszType);

    // 单条动作是否存在目标（用于 Scan）
    static bool ActionTargetExists(const RegCleanAction& a);

    // 单条动作执行（清理）
    static bool ExecAction(const RegCleanAction& a, RegCleanBackupEntry* pBackupEntry);

    // 值写入备份条目（读取当前真实数据）
    static bool ReadValueSnapshot(const wchar_t* sKey, const wchar_t* sValue, bool bWow64,
                                  RegCleanBackupEntry& out);

    std::vector<RegCleanRule> m_rules;
    CDataEngine*              m_pEngine;

    // [R118-1] 最近一次 SRSetRestorePointW 返回的 nStatus（STATEMGRSTATUS）
    static DWORD              s_lastRestoreStatus;
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
