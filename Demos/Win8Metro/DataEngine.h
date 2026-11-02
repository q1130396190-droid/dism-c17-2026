#pragma once

#include <vector>
#include <map>
#include <string>

// [R10-02] Data.xml 文本解析纯函数（剥离开头 '#' 名称引用与 '*' 本地化
// 前缀）。原为 CDataEngine::ResolveText 静态成员，服务层（CleanEngine/
// RegCleanEngine 等）为 3 行纯函数被迫静态耦合 CDataEngine；现提为
// 非类内联工具，任何包含本头的 TU 均可零耦合调用；CDataEngine::ResolveText
// 保留为薄转发，既有调用链不变。
inline CDuiString DismResolveText(const wchar_t* pszRaw)
{
    CDuiString s(pszRaw ? pszRaw : L"");
    if (!s.IsEmpty() && s[0] == L'#') s = s.Mid(1);
    if (!s.IsEmpty() && s[0] == L'*') s = s.Mid(1);
    return s;
}

// [R10-02] 作用域盒子名称纯函数（原 CDataEngine::ScopeName），供文件局部
// 自由函数零耦合调用；0=Current / 1=System / 2=Default。
inline const wchar_t* DismScopeName(int nScope)
{
    if (nScope == 1) return L"System";
    if (nScope == 2) return L"Default";
    return L"Current";
}

// ============================================================
// CDataEngine
// Load Dism-Multi-language Data.xml and evaluate its rules.
//
// SystemOptimization section contains two item types:
//   CheckBox : registry / service state toggle
//   Combo    : multiple Dropdown choices
//
// Supported condition tags:
//   OSVersion, RegExist, FileExist, QueryServiceStart, PowerPlan,
//   Or, And, Not, ImageFlags, Arch
//
// Supported action tags:
//   RegWrite, RegDelete, RegMove, SetServiceStart, ExplorerNotify,
//   SetPowerPlan
//   (FileCreateByZIP is reported as unsupported)
// ============================================================
class CDataEngine
{
public:
    CDataEngine();
    ~CDataEngine();

    // Search order:
    //   1) <exe>\..\Dism-Multi-language-10.1.1002.2\Data.xml
    //   2) <exe>\DismData\Data.xml
    //   3) <exe>\Data.xml
    bool Load(CDuiString& strFoundPath);
    bool IsValid() const;
    const wchar_t* GetLastError() const;

    CMarkupNode GetCleanCollectionRoot();
    CMarkupNode GetOptimizationRoot();
    // [B1-RE] Data.xml 文档根（<Data>）。DefaultFilterList/WSUS/
    // TimeZoneMap/ESDDecryptKey 是 CleanCollection4 的兄弟节点，
    // 旧代码从 CleanCollection4 下查找导致全局过滤表永远为空。
    CMarkupNode GetDocumentRoot();

    // Resolve "#Name" / "#*Name" to plain display text.
    static CDuiString ResolveText(const wchar_t* pszRaw);

    // [R-B39] ?Func 注册表单测接口：sExpr 形如 "?GetRegSz(HKCU\\A\\B,V)" 或
    // "?GetLocaleName()"；返回 true 表示命中注册表（含失败保持原文的保守情形
    // —— 此时 *pbResolved=false 可区分"不认识"与"认识但取值失败"）。
    // 用于 /FunSelfTest 对拍单测确认注册表覆盖度。
    static bool QueryFunExprSupported(CDuiString& sExpr, bool* pbResolved = nullptr);

    // Shared registry path parser ("HKLM\Sub\Key")
    static bool ParseRegPath(const wchar_t* pszFull, HKEY& hRoot, CDuiString& strSubKey);

    // ============================================================
    // [P0 2026-10-07] WinPE 离线蜂巢会话（系统优化离线定向）
    // BeginOfflineSession：挂载目标 <root>\Windows\System32\config 下
    //   SOFTWARE/SYSTEM/SAM/SECURITY/DEFAULT 以及
    //   <root>\Users\Default\NTUSER.DAT 到统一前缀键；会话激活后所有
    //   HKCU/HKLM/HKCR/HKU/.DEFAULT 注册表操作（含服务 Start）经
    //   RemapOfflinePath 单点重定向到离线蜂巢，不再触碰 PE 自身 RAM 盘。
    //   成功前提：SOFTWARE 与 SYSTEM 挂载成功（其余缺失降级）。
    // EndOfflineSession：统一 RegUnLoadKey（幂等，可重复调用）。
    // ============================================================
    static bool BeginOfflineSession(const wchar_t* pszImageRoot);
    static void EndOfflineSession();
    static bool IsOfflineSession();
    static CDuiString GetOfflineImageRoot();   // 例 "D:\"；未激活返回空串
    // 逻辑路径→离线挂载路径；未激活或无法定向（如 HKLM\HARDWARE）返回原文。
    // bWow32=true 时 SOFTWARE 类映射到 Wow6432Node（与 KEY_WOW64_32KEY 同义）。
    static CDuiString RemapOfflinePath(const wchar_t* pszFull, bool bWow32 = false);

    // [R79] RegClean facade：公开包装私有静态 DoRegDelete/DoRegWrite/DoRegMove，
    // 供 RegCleanEngine（注册表清理规则集/扫描/清理/备份四件套）调用，避免复制注册表实现。
    static bool RegCleanDelete(const wchar_t* pszKey, const wchar_t* pszValue,
                               bool bWow64 = false, bool bSkipError = false)
    { return DoRegDelete(pszKey, pszValue, bWow64, bSkipError); }

    static bool RegCleanWrite(const wchar_t* pszKey, const wchar_t* pszValue,
                              const wchar_t* pszType, const wchar_t* pszData,
                              bool bWow64 = false)
    { return DoRegWrite(pszKey, pszValue, pszType, pszData, bWow64); }

    static bool RegCleanMove(const wchar_t* pszSrcKey, const wchar_t* pszSrcValue,
                             const wchar_t* pszDstKey, const wchar_t* pszDstValue,
                             bool bWow64 = false)
    { return DoRegMove(pszSrcKey, pszSrcValue, pszDstKey, pszDstValue, bWow64); }

    // [R79] 判断指定注册表值是否存在（RegClean 扫描用）；
    // bWow64=true 走 KEY_WOW64_32KEY 视图。
    static bool RegCleanValueExists(const wchar_t* pszKey, const wchar_t* pszValue,
                                    bool bWow64 = false);

    // [R79] RegClean facade：公开包装私有 static OpenRegKey，
    // 供 RegCleanEngine 打开注册表键（读取快照）。
    static bool RegCleanOpenKey(const wchar_t* pszFull, REGSAM samDesired, HKEY& hKey,
                                bool bCreate, bool bWow64 = false)
    { return OpenRegKey(pszFull, samDesired, hKey, bCreate, bWow64); }

    // [R79/P1-收尾] 通过原生 .reg 解析器导入备份文件（撤销用），零 reg.exe。
    // bWow64=true 按 32 位视图写入（等价 reg /reg:32）。
    static bool RegCleanImport(const wchar_t* pszRegFile, bool bWow64 = false);

    // ---- condition evaluation ----
    // Evaluate every child condition of the container (AND semantics).
    bool EvalConditions(CMarkupNode nodeContainer);
    bool EvalOne(CMarkupNode nodeCond);

    // [R8] 条件判定错误码（未知标签 fail-closed 时随调试串输出）。
    enum class ECondErr : int { CE_NONE = 0, CE_UNKNOWN_TAG = 1 };

    // ---- CheckBox items ----
    // returns: 1 = checked, 0 = unchecked, -1 = unknown
    int  GetCheckState(CMarkupNode nodeItem);
    bool SetCheckState(CMarkupNode nodeItem, bool bChecked,
                       std::vector<CDuiString>& warnings,
                       int& nUnsupported);

    // ---- per-scope variants ----
    // 作用域编号 nScope：0 = 当前用户 ("Current"，HKCU)、
    //                   1 = 系统 ("System"，HKLM 所有用户)、
    //                   2 = 默认用户 ("Default"，HKEY_USERS\.DEFAULT)
    static const wchar_t* ScopeName(int nScope);
    static int  GetScopeCount() { return 3; }   // 作用域盒子总数：Current/System/Default

    // O-1: 检测某优化项的任一作用域 True/False 分支是否含本引擎无法执行的动作
    // （当前为 Dism++ 内部资源动作 FileCreateByZIP），用于在 UI 显式标注，避免静默失败。
    static bool ItemHasUnsupportedAction(CMarkupNode nodeItem);
    int  GetScopeState(CMarkupNode nodeItem, int nScope);   // 1/0/-1
    bool SetScopeState(CMarkupNode nodeItem, int nScope, bool bChecked,
                       std::vector<CDuiString>& warnings,
                       int& nUnsupported,
                       bool* pbRestartExplorer = nullptr,   // out: Activate Restart="Explorer"
                       bool* pbAnyActionFailed = nullptr);  // out: 任一动作尝试后失败（部分失败）

    // ---- Combo items ----
    int  GetDropdownCount(CMarkupNode nodeItem);
    bool GetDropdownName(CMarkupNode nodeItem, int iSel, CDuiString& strName);
    int  GetCurrentDropdown(CMarkupNode nodeItem);   // -1 unknown
    bool SelectDropdown(CMarkupNode nodeItem, int iSel,
                        std::vector<CDuiString>& warnings,
                        int& nUnsupported);

    // ---- per-scope combo variants ----
    int  GetScopeDropdownCount(CMarkupNode nodeItem, int nScope);
    bool GetScopeDropdownName(CMarkupNode nodeItem, int nScope,
                              int iSel, CDuiString& strName);
    int  GetScopeDropdown(CMarkupNode nodeItem, int nScope);   // -1 n/a
    bool SelectScopeDropdown(CMarkupNode nodeItem, int nScope, int iSel,
                             std::vector<CDuiString>& warnings,
                             int& nUnsupported,
                             bool* pbRestartExplorer = nullptr,
                             bool* pbAnyActionFailed = nullptr);

    // ---- [Q1] WSUS 黑名单（便捷 API）----
    // 自动定位 <WSUSBlacklist> 节点并调用 ParseWsuSection。
    bool GetWsuBlacklist(std::vector<CDuiString>& outIds);
    bool IsInWsuBlacklist(const CDuiString& sId);   // 大小写不敏感

    // ---- [Q4] 时区映射（便捷 API）----
    // 自动定位 <TimeZoneMap> 节点并调用 ParseTimeZoneMap，按索引访问。
    int  GetTimeZoneCount();
    bool GetTimeZone(int nIdx, CDuiString& outKey, CDuiString& outOffset);

    // ---- [Q10] ESD 密钥（便捷 API）----
    // 自动定位 <ESDDecryptKey> 节点并调用 ParseEsdKeys。
    bool GetEsdKeys(std::vector<CDuiString>& outKeys);

private:
    // [R8] EvalOne 的 11 个条件谓词（一标签一函数，错误路径 fail-closed）。
    bool EvalOSVersion(CMarkupNode nodeCond);
    bool EvalRegExist(CMarkupNode nodeCond);
    bool EvalFileExist(CMarkupNode nodeCond);
    bool EvalQueryServiceStart(CMarkupNode nodeCond);
    bool EvalPowerPlan(CMarkupNode nodeCond);
    bool EvalNot(CMarkupNode nodeCond);
    bool EvalOr(CMarkupNode nodeCond);
    bool EvalAnd(CMarkupNode nodeCond);
    bool EvalImageFlags(CMarkupNode nodeCond);
    bool EvalArch(CMarkupNode nodeCond);
    bool EvalFileVersion(CMarkupNode nodeCond);

    // [R9] ExecActivate 动作注册表：一标签一处理器，分发表派发。
    // ACT_OK=成功(累加 bOk)；ACT_FAIL=失败(记 pbAnyFailed)；
    // ACT_SKIP=中性跳过(部分处理器内部已 ++nUnsupported，bOk 不变)。
    enum class EActResult : int { ACT_SKIP = -1, ACT_FAIL = 0, ACT_OK = 1 };
    struct TActCtx
    {
        CMarkupNode nodeActivate;   // Rule 递归时取兄弟 <Activate> 用
        int*  pnUnsupported;
        bool* pbNeedsRestart;
        bool* pbAnyFailed;
        void Skip() { if (pnUnsupported) ++*pnUnsupported; }
        void Fail() { if (pbAnyFailed) *pbAnyFailed = true; }
    };
    EActResult ActRegWrite(CMarkupNode node, TActCtx& ctx);
    EActResult ActRegDelete(CMarkupNode node, TActCtx& ctx);
    EActResult ActRegMove(CMarkupNode node, TActCtx& ctx);
    EActResult ActRule(CMarkupNode node, TActCtx& ctx);
    EActResult ActSetServiceStart(CMarkupNode node, TActCtx& ctx);
    EActResult ActExplorerNotify(CMarkupNode node, TActCtx& ctx);
    EActResult ActFileCreateByZip(CMarkupNode node, TActCtx& ctx);
    EActResult ActSetDesktop(CMarkupNode node, TActCtx& ctx);
    EActResult ActSetServiceRecovery(CMarkupNode node, TActCtx& ctx);
    EActResult ActSetAppxPackage(CMarkupNode node, TActCtx& ctx);
    EActResult ActSetPowerPlan(CMarkupNode node, TActCtx& ctx);
    EActResult ActImportReg(CMarkupNode node, TActCtx& ctx);
    EActResult ActAddDriver(CMarkupNode node, TActCtx& ctx);
    EActResult ActNtSetSystemInformation(CMarkupNode node, TActCtx& ctx);
    EActResult ActFileMove(CMarkupNode node, TActCtx& ctx);
    EActResult ActAddPackage(CMarkupNode node, TActCtx& ctx);
    EActResult ActRescan(CMarkupNode node, TActCtx& ctx);

    CMarkupNode GetBranch(CMarkupNode nodeItem, const wchar_t* pszBranch);
    bool CollectActions(CMarkupNode nodeBranch,
                        std::vector<CMarkupNode>& activates,
                        std::vector<CDuiString>& warnings);
    bool ExecActivate(CMarkupNode nodeActivate, int& nUnsupported,
                      bool& bNeedsRestart, bool* pbAnyFailed = nullptr);

    // REG_DWORD 数据解析：纯十进制数字按 10 进制（"10"->10），
    // 含 A-F/a-F 的按 16 进制（"FFEFFFFF"/"A"->10），与 Dism++ 规则书写一致。
    static DWORD ParseDwordData(const wchar_t* pszData);

    // Evaluate dynamic RegWrite functions (Fun="True"): currently ?ColorDialog().
    // Returns false when the user cancels the dialog.
    static bool EvalFunData(const wchar_t* pszFun, CDuiString& strOut);

    // [R-B5] ?Get* 内嵌函数：解析并展开表达式中的 ?GetRegSz/?GetFileVersion/
    // ?GetIniData/?GetLocaleName 调用为真实值；找不到或解析失败保持原文。
    static void ResolveFunExpr(CDuiString& sInOut);

    // ---- registry helpers ----
    // bWow64: Wow64="True" = operate on the 32-bit registry view (KEY_WOW64_32KEY)
    static bool OpenRegKey(const wchar_t* pszFull, REGSAM samDesired, HKEY& hKey,
                           bool bCreate, bool bWow64 = false);
    static bool RegValueEqual(HKEY hKey, const wchar_t* pszValue,
                              const wchar_t* pszType, const wchar_t* pszData,
                              const wchar_t* pszCompare = nullptr);
    static bool DoRegWrite(const wchar_t* pszKey, const wchar_t* pszValue,
                           const wchar_t* pszType, const wchar_t* pszData,
                           bool bWow64 = false, const wchar_t* pszOperator = nullptr,
                           const wchar_t* pszMask = nullptr);
    // [R9-02] DoRegWrite 拆分：位运算分支 / 普通写分支 / Searchbox 缓存补写
    static bool DoRegWriteBitOp(HKEY hKey, const wchar_t* pszValue, const wchar_t* pszType,
                                const wchar_t* pszData, const wchar_t* pszMask, wchar_t chOp);
    static bool DoRegWritePlain(HKEY hKey, const wchar_t* pszValue,
                                const wchar_t* pszType, const wchar_t* pszData);
    static void DoRegWriteSearchboxCache(HKEY hKey, const wchar_t* pszValue,
                                         const wchar_t* pszType, const wchar_t* pszData);
    // [R9-02] 动作处理器内相对路径解析（<exe>\<SubDir>\file），定义在 DataEngine.cpp
    static CDuiString ResolveExeRelative(const wchar_t* pszRel, const wchar_t* pszSubDir);
    // [R3] Data/Msk 解码（"#base64" / hex / DWORD 十进制）与位操作（| & xor not）
    static bool DecodeRegData(const wchar_t* pszIn, DWORD dwRegType,
                              std::vector<BYTE>& outBin);
    static bool ApplyBitOp(wchar_t chOp,
                           std::vector<BYTE>& curBytes,
                           const std::vector<BYTE>& maskBytes);
    static bool Base64Decode(const wchar_t* pszIn, std::vector<BYTE>& outBin);
    static bool Base64Encode(const BYTE* pData, DWORD dwLen, CDuiString& strOut);
    static bool DoRegDelete(const wchar_t* pszKey, const wchar_t* pszValue,
                            bool bWow64 = false, bool bSkipError = false);
    static bool DoRegMove(const wchar_t* pszSrcKey, const wchar_t* pszSrcValue,
                          const wchar_t* pszDstKey, const wchar_t* pszDstValue,
                          bool bWow64 = false, bool bSkipError = false);

    // ---- [R4] 上下文菜单辅助：统计当前分组下 Item 数量 ----
    static int  CountRuleInGroup(CMarkupNode nodeGroup);

    // ---- [R4] Rule 上下文粘贴（预留接口；实际 XML 插入留待 R5）----
    static bool DoPasteRule(CMarkupNode nodeParent, const wchar_t* pszClipXml);

    // ---- NtSetSystemInformation helpers (P0-R16-2, N-16) ----
    static bool ParseInformationClass(const wchar_t* pszName, DWORD* pdwOut);
    static DWORD ParseNtBuffer(const wchar_t* pszBuf, BYTE* pOut, DWORD dwOutMax);
    static bool DoNtSetSystemInformation(
        DWORD dwInformationClass, const void* pBuffer, DWORD dwBufferLength,
        LONG* pErrStatus = nullptr);

    // ---- [R-B9] 自定义规则加载（Config\Custom\*.xml / *.zip）----
    // 在 UTF-8 字节层把 Custom 规则 <Item> 片段合并进 Data.xml 字符串。
    // 实现位于 DataEngine.cpp 文件级函数 LoadCustomRulesIntoXml。
    // [R-B37] 语义扩展：同名 Item 按 Name 属性"主库优先"去重覆盖
    //          （Custom 同名 Item 替换而非重复追加）。
    static bool LoadCustomRulesIntoXml(std::string& sXml);

    // ---- [R-B37] Data.zip / Data_extracted / data_default 三级覆盖 ----
    // 原版行为：规则源可以是打包的 Data.zip（可更新），解压缓存到
    // Data_extracted，内置默认在 data_default。本方法把 <zip> 解压到
    // %TEMP%\dismpp_data_<tick> 并返回其中 Data.xml 的路径。
    // 成功返回 true 且 *outDataXmlPath 非空；失败返回 false。
    static bool ExtractDataZipToTemp(const wchar_t* pszZip, CDuiString& outDataXmlPath);

    // ---- [REFACTOR] 规则源布局探测（从 Load 提取）----
    // 按候选布局 × [Data_Win11.xml, Data.xml] 顺序探测，命中即止。
    // strTried 累加已尝试路径，供 Load 拼装"未找到规则源"诊断信息。
    struct TryResult { bool bFound; CDuiString strPath; };
    static TryResult ProbeLooseLayouts(const CDuiString& strDir, bool bIsWin11,
                                       const wchar_t** pszLayouts, int nLayouts,
                                       CDuiString& strTried);

    // 标准布局未命中时探测 Data.zip（Win11 先 Data_Win11.zip）并解压取
    // 内含 Data.xml 的路径；命中返回 true 并回填 strPath。
    static bool ProbeZipPackages(const CDuiString& strDir, bool bIsWin11,
                                 CDuiString& strTried, CDuiString& strPath);

    // 最后一级兜底：探测 Data_extracted / data_default 下的规则文件。
    // 命中返回 true 并回填 strPath；strTried 累加已尝试路径。
    static bool ProbeFallbackDirs(const CDuiString& strDir, bool bIsWin11,
                                  CDuiString& strTried, CDuiString& strPath);

    // ---- zip helpers (FileCreateByZIP action) ----
    static bool UnzipEntryToPath(const wchar_t* pszZipPath, const wchar_t* pszEntry,
                                 const wchar_t* pszDst, DWORD dwTimeoutMs);

    // ---- service helpers ----
    static int  QueryServiceStart(const wchar_t* pszName);   // -1 error
    static bool SetServiceStart(const wchar_t* pszName, int nStartType);

    // ---- power plan helpers ----
    // [P0-A7] powrprof.dll 动态绑定（PowerGetActiveScheme/PowerSetActiveScheme/
    // PowerDuplicateScheme），零 powercfg 进程。Win10/11 上
    // HKCU\Control Panel\PowerCfg!ActivePowerScheme 已不再维护。
    // bUltimate=true 支持卓越性能：PowerSetActiveScheme 失败时按固定 GUID
    // e9a42b02-... PowerDuplicateScheme 复制隐藏模板后再激活；离线会话
    // 直写挂载 SYSTEM 蜂巢 Power\\User\\PowerSchemes!ActivePowerScheme。
    static bool GetActivePowerPlan(CDuiString& strGuid);
    static bool SetActivePowerPlan(const wchar_t* pszGuid, bool bUltimate);
    static bool IsUltimatePowerPlan(const wchar_t* pszGuid);
    // [R17 / P1-5] SetServiceRecovery action
    static bool SetServiceRecovery(const wchar_t* pszServiceName, int nRecoveryAction, int nDelaySeconds);

    // ---- desktop wallpaper / color helpers [R17 / P1-4] ----
    static bool DoSetDesktop(const wchar_t* pszWallPaper, const wchar_t* pszColor);

    // ---- Appx package helpers [R17 / P1-6] ----
    static bool DoSetAppxPackage(const wchar_t* pszPath, bool bInstall);

    // ---- [Q1] WSUS 黑名单解析器 ----
    // 解析 Data.xml 中 <WSUSBlacklist> 节点，提取被禁止安装的更新 ID 列表。
    // 用于在清理/优化页面过滤或标记已黑名单的更新。
    static bool ParseWsuSection(CMarkupNode nodeWSUS, std::vector<CDuiString>& outBlacklistIds);

    // ---- [Q4] 时区映射解析器 ----
    // 解析 Data.xml 中 <TimeZoneMap> 节点，建立时区 ID 到偏移量的映射。
    static bool ParseTimeZoneMap(CMarkupNode nodeTzMap, std::map<CDuiString, int>& outMap);

    // ---- [Q10] ESD 密钥解析器 ----
    // 解析 Data.xml 中 <ESDDecryptKey> 节点，提取 Windows ESD 激活密钥列表。
    static bool ParseEsdKeys(CMarkupNode nodeEsd, std::vector<CDuiString>& outKeys);

    // [P1-收尾] 原 DoRunExe 外部进程 helper 已删除：reg.exe/dism.exe/pnputil.exe
    // 全部改为进程内原生 API（CRegCleanEngine::ImportRegFile / DismApiBridge /
    // DriverService / DismApiBridge::AddDriver 等）。

    // ---- [Q1] WSUS 黑名单（便捷 API）----
    // 自动定位 <WSUSBlacklist> 节点并调用 ParseWsuSection。
    static void RefreshDesktopIcons();   // refresh all desktop icon views (F5)

    // ---- misc ----
    static bool MatchOSVersion(const wchar_t* pszCompare, const wchar_t* pszTarget);
    static bool FileExists(const wchar_t* pszPath);

private:
    CMarkup* m_pMarkup;
    CDuiString m_strLastError;
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
