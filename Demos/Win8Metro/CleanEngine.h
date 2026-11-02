#pragma once

#include <vector>
#include <functional>   // [R-CLEANPROG] DeleteRules 进度回调

class CDataEngine;

// ============================================================
// [A3] ScanRule 错误码枚举
// 语义：ScanRule 返回值 <0 时，取 -nRet 得到此枚举值。
// 调用方用 StringManager key 映射到用户可读文案。
// ============================================================
enum class ScanRuleError : int
{
    SCAN_RULE_OK              = 0,   // 成功（返回值 >=0）
    SCAN_RULE_ACCESS_DENIED   = 1,   // 权限不足，无法打开路径
    SCAN_RULE_PATH_NOT_FOUND  = 2,   // 目标路径不存在或已被删除
    SCAN_RULE_TIMEOUT         = 3,   // 枚举超时（如网络共享慢）
    SCAN_RULE_IO_ERROR        = 4,   // 通用 IO 错误
    SCAN_RULE_UNKNOWN         = 5,   // 其他未分类错误
    SCAN_RULE_ERROR_COUNT     = 6    // 计数（数组大小用）
};

// ============================================================
// Clean target (resolved concrete file / directory)
// ============================================================
struct CleanTarget
{
    CDuiString sPath;
    bool       bDir;
};

// ============================================================
// One cleanup rule from Data.xml / CleanCollection4
// ============================================================
struct CleanRule
{
    CleanRule()
        : iIdx(0), nLevel(0), bFileRule(false),
          bProcSupported(false), pNode(nullptr), ullSize(0), nWeight(1),
          bRestartExplorer(false), bAssocNotify(false),
          nLastError(ScanRuleError::SCAN_RULE_OK) {}

    int                       iIdx;
    CDuiString                sName;
    CDuiString                sDesc;
    CDuiString                sGroup;
    // [E-5] 风险档（原版 Dism++x64.exe FUN_1400a3ca0 逆向实证）：
    //   默认勾选条件 = (Level==2 || Level==3)，除非 config
    //   Dism++\Cleanup\<Name>=="1" 覆盖；Level 0 为最高风险项
    //   （CompactOs/硬链接合并/Installer 目录等），默认不勾并加警示样式。
    //   Page_Cleanup 的 SetCheck(rule.nLevel >= 2) 即按此极性实现。
    int                       nLevel;
    // [E-6] 扫描权重：Data.xml 中 Item@Work 或适用分支 Scan@Work 的最大值，
    // 缺省 1。用于按工作量驱动扫描进度条（而非规则条数）。
    int                       nWeight;
    bool                      bFileRule;     // has General path rules
    CDuiString                sSpecialProc;  // Custom ProcName, if any
    CDuiString                sSpecialFlags; // Custom Flags 属性（如 DismComponentCleanup 的 "#1"=ResetBase）
    bool                      bProcSupported;// Custom 动作是否被当前版本真实支持
    CDuiString                sProcNote;     // 不支持时给用户看的说明
    bool                      bRestartExplorer; // 任一 Activate 带 Restart="Explorer"（如缩略图缓存）
    bool                      bAssocNotify;     // 任一 Activate 含 ExplorerNotify AssocChanged
    CMarkupNode*              pNode;         // markup Item node
    std::vector<CleanTarget>  targets;
    // [B1-RE] 瞬态：本次扫描各 General 已解析的根路径（不参与序列化）。
    // glob 在根不存在时整体枚举失败、targets 为空，ScanRule 靠这些根
    // 做 GetFileAttributes 二级判定，给出 NOT_FOUND/DENIED 错误码。
    std::vector<CDuiString>  vProbeRoots;
    ULONGLONG                 ullSize;

    // [A4] 最近一次 ScanRule 的错误分类与首个失败路径。
    // nLastError 语义同 ScanRule 返回值：0 = 无错误；
    //      1=ACCESS_DENIED / 2=PATH_NOT_FOUND / 3=TIMEOUT /
    //      4=IO_ERROR / 5=UNKNOWN（见 CleanEngine.h 中 enum ScanRuleError）。
    // sLastErrPath 记录首个触发错误的目标路径，UI 端可展示到日志。
    ScanRuleError            nLastError;
    CDuiString                sLastErrPath;

    // [OS-Filter] 该规则在当前系统上是否适用。由 CleanEngine::LoadRules
    // 在加载时按 XML 的 <Applicable> 求值一次并缓存：不适用的条目
    // 直接不进入 UI（旧版本上看不到新系统才有的清理项）。
    // 注意区别于 IsRuleApplicableNow()——后者是"扫描前按实时状态重评"，
    // 用于防止磁盘状态变化后误删；本字段只回答"这条规则是否该出现在
    // 当前系统的界面里"，语义更稳定，适合做显示筛选。
    bool                      bOsApplicable = true;
    // 规则要求的最低 build（0=无要求）。用于诊断与日志，便于排查
    // "为什么这条规则在本机不显示"。
    unsigned int              nMinBuild = 0;
};

// ============================================================
// CCleanEngine
// Enumerate Data.xml cleanup rules, scan real file sizes and
// delete targets (Recycle Bin, undoable).
// ============================================================
class CCleanEngine
{
public:
    CCleanEngine();
    ~CCleanEngine();

    bool Init(CDataEngine* pEngine);

    int  GetRuleCount() const;
    CleanRule& GetRule(int iIdx);

    int  GetGroupCount() const;
    bool GetGroup(int iGroup, CDuiString& strName);
    int  GetGroupTotalIndex(int iGroup, ULONGLONG& ullTotal);

    // Returns number of targets found. Safe to call from worker thread.
    // [A3] 返回值语义：
    //   始终 >=0：命中目标条数（0 = 无匹配，规则本身不一定有错——glob 零命中
    //   可能仅是"目标不存在"，由 nLastError/PATH_NOT_FOUND 分类表达）。
    //   错误分类与首个失败路径写入 CleanRule::nLastError / sLastErrPath，
    //   不影响返回值。
    //   注意：SEH 隔离层（CleanJobService::ScanOneRuleIsolated）会把硬件异常
    //   降级为 -ScanRuleError::SCAN_RULE_UNKNOWN 并返回负值；ScanRuleErrorFromRet
    //   / IsScanRuleFailed 仅在此路径生效。
    int  ScanRule(int iIdx);
    static int ScanRuleErrorFromRet(int nRet) { return -nRet; }
    // 便捷判断：仅看是否失败（保留原语义，向后兼容）
    static bool IsScanRuleFailed(int nRet) { return nRet < 0; }

 // [Q9] DefaultFilterList（Data.xml <DefaultFilterList>/<Item>，32 项）
 // [RE-GHIDRA 2026-10-12] 原版 Dism++x64.exe 逆向实证（FUN_1400a2658 /
 //   DismGetFileFilter @0xa277c / FUN_1400113c0 DismCaptureImage /
 //   FUN_140006640 豁免列表设置框）：本表是【WIM 映像捕获排除清单 +
 //   导出 API + 豁免配置】，不参与空间回收 General 枚举（General 只用
 //   每条规则自己的 <Excluded>）。清理扫描不应用本表；加载保留供 WIM
 //   捕获等场景取用。
 int GetDefaultFilterCount() const { return static_cast<int>(m_vDefaultFilter.size()); }

    // Delete targets of the given rules permanently (no Recycle Bin).
    // Returns freed size (estimated from scan sizes).
    // [R88] 增加 bToRecycleBin：true 时走 FOF_ALLOWUNDO（进回收站可撤销），
    //       false 保持 R5/P2-16 直删语义（默认 true，与 CleanExtend 撤销栈对齐）。
    // [R88] pOutDeleted 非空时填充本次实际尝试删除的路径（供 UI 推入撤销栈）。
    // [R-CLEANPROG] pProgress 非空时每完成一个删除批次回调一次
    //       （已完成路径数, 去重后总路径数），供删除线程回投 UI 进度。
    //       回调在工作线程执行，禁止触碰 UI。
    ULONGLONG DeleteRules(const std::vector<int>& rules, int& nFailedPaths,
                          bool bToRecycleBin = true,
                          std::vector<CDuiString>* pOutDeleted = nullptr,
                          const std::function<void(size_t, size_t)>* pProgress = nullptr);

    // [E-2] 清理动作执行前按"当前实时状态"重评规则 Applicable，防止扫描后
    // 状态漂移（卸载/拔盘/服务变更）导致误删。
    //   pszProc == nullptr：是否存在适用分支含 General（文件规则）；
    //   pszProc != nullptr：是否存在适用分支的 Activate 含匹配 ProcName 的 Custom。
    // 可在工作线程（文件规则重扫）与 UI 线程（Custom 派发前门控）调用。
    bool IsRuleApplicableNow(int iIdx, const wchar_t* pszProc = nullptr);

    // [E-4] 收集规则"当前适用分支"的 Warning 文案（Item 直属 Warning +
    // 各 Applicable 成立的 Scan 内 Warning），去重追加到 out。
    // 用于清理前汇总确认与描述面板展示。
    void CollectRuleWarnings(int iIdx, std::vector<CDuiString>& out);

    static void FormatSize(ULONGLONG ullBytes, CDuiString& strOut);

private:
    void LoadRules();
    void ProcessGeneral(CMarkupNode nodeGeneral, CleanRule& rule);
    // [REFACTOR] 从 ProcessGeneral 提取：收集 General 节点的 Query/Filter 模式。
    // 属性 Query 优先，子节点 <Query>/<Filter> 兼容追加（开源版契约）。
    void CollectGeneralQueries(CMarkupNode nodeGeneral, std::vector<CDuiString>& queries);
    // [REFACTOR] 从 ProcessGeneral 提取：收集 Excluded 子节点并展开函数式排除。
    // 返回 false 表示存在无法解析的函数式排除（调用方据此 fail-closed 放弃整组）。
    bool CollectGeneralExcludes(CMarkupNode nodeGeneral, const CDuiString& strRoot,
                                std::vector<CDuiString>& exclPatterns);
    void ParseDefaultFilters();   // [Q9] 解析 Data.xml <DefaultFilterList> 全局排除表
    // [R13-01] ScanRule 深度拆解辅助（d7->d<=5）：纯结构性拆分，零行为改动
    void SrCollectScanPaths(CMarkupNode top, CleanRule& rule,
                               std::vector<CDuiString>& paths);
    static void SrRebuildTargets(CleanRule& rule, const std::vector<CDuiString>& paths,
                                 int& nErrAccess, int& nErrNotFound, int& nErrIo);
    static void SrProbeRootFallback(CleanRule& rule,
                                    int& nErrAccess, int& nErrNotFound, int& nErrIo);
    // [FIX-WINSXS] "被取代的WinSxS组件"（Custom 规则 DismComponentCleanup）
    // 扫描尺寸来源：General 文件规则无法表达，原版 Dism++ 用独立枚举器
    // 按组件 identity 分组取组内最高版本，其余目录累加大小。
    // 只填 ullSize（绝不填 targets）：清理必须走 DISM StartComponentCleanup
    // 事务，直接删目录会损坏组件存储。
    static void SrScanSupersededWinSxS(CleanRule& rule);

    // Resolve RootPath: ?GetRegSz(...) / env vars / custom %System%
    static CDuiString ResolveRootPath(const wchar_t* pszRaw);

    // Enumerate concrete paths matched by a glob (C:\a\*\b\*.log)
    //
    // 末级匹配模式（严格对齐原版 NCleaner doGeneral 语义：FindFirstFile
    // 命中项不区分文件/目录，删除时再按属性派发 RemoveDirectory/DeleteFile）：
    //   GLOB_BOTH=0 无 Flags（默认）：末级文件与目录都收
    //   GLOB_FILE=1 Flags="File"     ：末级仅收文件
    //   GLOB_DIR =2 Flags="Directory"：末级仅收目录
    // 中间级始终必须为目录（与原版一致）。
    enum class GlobMatch : int { GLOB_BOTH = 0, GLOB_FILE = 1, GLOB_DIR = 2 };
    static void GlobPaths(const CDuiString& strPattern, GlobMatch nWant,
                          std::vector<CDuiString>& out);

    // 判断 Custom ProcName 是否为当前版本内置支持的白名单动作
    static bool IsKnownProc(const wchar_t* pszProc);

    // 解析函数式 Excluded（?GetFileVersion/?GetIniData/?GetRegSz/?GetLocaleName），
    // 解析结果（可能不止一个模式）追加到 outPatterns；
    // 返回 false 表示遇到无法解析的函数式排除，调用方应 fail-closed 跳过该组，
    // 避免本应保留的文件被当作可删除目标。
    static bool ResolveExcludedPattern(const CDuiString& strRaw,
                                       const CDuiString& strRoot,
                                       std::vector<CDuiString>& outPatterns);

    // 用已解析的排除模式匹配候选路径的“文件名部分”，命中任一即排除
    static bool MatchExcluded(const std::vector<CDuiString>& patterns,
                              const CDuiString& strPath);

    static ULONGLONG PathSize(const CDuiString& strPath, bool bDir);
    static bool ExistsPath(const CDuiString& strPath);
    static void AddUnique(std::vector<CDuiString>& out, const CDuiString& s);

private:
    void FreeRuleNodes();

    CDataEngine*           m_pEngine;
    std::vector<CleanRule> m_rules;
 std::vector<CDuiString> m_vDefaultFilter;   // [Q9] 全局排除表
    std::vector<CDuiString> m_groups;
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
