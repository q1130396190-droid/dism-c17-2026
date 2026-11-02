#include "stdafx.h"

#include "OfflineSession.h"  // [R10-02] 离线会话经独立组件访问，去 CDataEngine 静态耦合
#include <cwctype>   // [C12] std::towlower
#include "CleanEngine.h"
#include "CleanNativePrimitives.h"   // [R-B38] 底层清理原语（MoveFileEx/FileDisposition/ADS）
#include "DataEngine.h"
#include "StringManager.h"
#include <shlwapi.h>
#include <string>
#include <algorithm>
#include <memory>   // [C++17 R3] make_unique
#include <cstdio>
#include <stdarg.h>
#include <unordered_set>
#include <map>   // [FIX-WINSXS] 被取代组件分组
// [C++17 阶段2] 局部 RAII 守卫：文件/查找句柄、注册表键（见 WinHandles.h 使用规范）
struct CLEHandleGuard
{
    HANDLE h;
    explicit CLEHandleGuard(HANDLE x) : h(x) {}
    ~CLEHandleGuard() { if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h); }
    CLEHandleGuard(const CLEHandleGuard&) = delete;
    CLEHandleGuard& operator=(const CLEHandleGuard&) = delete;
    operator HANDLE() const { return h; }
};

struct CLEFindGuard
{
    HANDLE h;
    explicit CLEFindGuard(HANDLE x) : h(x) {}
    ~CLEFindGuard() { if (h && h != INVALID_HANDLE_VALUE) ::FindClose(h); }
    CLEFindGuard(const CLEFindGuard&) = delete;
    CLEFindGuard& operator=(const CLEFindGuard&) = delete;
    operator HANDLE() const { return h; }
};

struct CLERegGuard
{
    HKEY k = nullptr;
    ~CLERegGuard() { if (k) ::RegCloseKey(k); }
    HKEY* out() { return &k; }
    operator HKEY() const { return k; }
};


using i18n::CStringManager;

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "version.lib")   // GetFileVersionInfo / VerQueryValue

// ============================================================
CCleanEngine::CCleanEngine() : m_pEngine(nullptr)
{
}

CCleanEngine::~CCleanEngine()
{
    FreeRuleNodes();
}

void CCleanEngine::FreeRuleNodes()
{
    for (size_t k = 0; k < m_rules.size(); ++k)
    {
        if (m_rules[k].pNode)
        {
            delete m_rules[k].pNode; // [C++17 阶段3] 与 LoadRules 入容器的 new 配对，容器级所有权
            m_rules[k].pNode = nullptr;
        }
    }
}

bool CCleanEngine::Init(CDataEngine* pEngine)
{
    m_pEngine = pEngine;
    if (!m_pEngine || !m_pEngine->IsValid()) return false;
    LoadRules();
    // [修复 P1-FILTER] ParseDefaultFilters 原本在 return 之后成为死代码，
    // 全局默认排除规则（m_vDefaultFilter）永不加载，误删防护缺失。
    ParseDefaultFilters();
    return !m_rules.empty();
}

int CCleanEngine::GetRuleCount() const { return static_cast<int>(m_rules.size()); }
CleanRule& CCleanEngine::GetRule(int iIdx) { return m_rules[iIdx]; }

int CCleanEngine::GetGroupCount() const { return static_cast<int>(m_groups.size()); }

bool CCleanEngine::GetGroup(int iGroup, CDuiString& strName)
{
    if (iGroup < 0 || iGroup >= static_cast<int>(m_groups.size())) return false;
    strName = m_groups[iGroup];
    return true;
}

int CCleanEngine::GetGroupTotalIndex(int iGroup, ULONGLONG& ullTotal)
{
    CDuiString strName;
    if (!GetGroup(iGroup, strName)) return 0;
    ullTotal = 0;
    int n = 0;
    for (size_t i = 0; i < m_rules.size(); ++i)
    {
        if (m_rules[i].sGroup == strName)
        {
            ullTotal += m_rules[i].ullSize;
            ++n;
        }
    }
    return n;
}

// ============================================================
// rule loading
// ============================================================
// ============================================================
// [R13-01] LoadRules 深度拆解辅助（d7->d<=5）
// 仅提取 Scan/ScanCollection 分支权重与动作扫描，逐字搬迁原逻辑，零行为改动。
// ============================================================

// [LR-Scan] 扫描单个 Scan/ScanCollection 分支：取 Work 权重与 Activate 动作标记
// nSelScanWork/nMaxScanWork 为 [E-6][R-B39] 权重选择用；
// bGeneral/bRestartExplorer/bAssocNotify/strProc/sSpecialFlags 为动作标记。
static void LrScanBranch(CDataEngine* pEngine, CMarkupNode scan,
                         const wchar_t* pszItemWork,
                         int& nSelScanWork, int& nMaxScanWork,
                         bool& bGeneral, bool& bRestartExplorer,
                         bool& bAssocNotify, CDuiString& strProc,
                         CDuiString& sSpecialFlags)
{
    const wchar_t* pszWork = scan.GetAttributeValue(L"Work");
    if (pszWork && *pszWork)
    {
        int nW = _ttoi(pszWork);
        if (nW > nMaxScanWork) nMaxScanWork = nW;
        CMarkupNode app = scan.GetChild(L"Applicable");
        bool bBranchOn = !app.IsValid()
            || (pEngine && pEngine->EvalConditions(app));
        if (bBranchOn && nW > nSelScanWork) nSelScanWork = nW;
    }

    CMarkupNode act = scan.GetChild(L"Activate");
    if (!act.IsValid()) return;
    if (lstrcmpiW(act.GetAttributeValue(L"Restart"),
                 L"Explorer") == 0)
        bRestartExplorer = true;
    CMarkupNode a = act.GetChild();
    while (a.IsValid())
    {
        if (lstrcmpiW(a.GetName(), L"General") == 0)
            bGeneral = true;
        if (lstrcmpiW(a.GetName(), L"Custom") == 0
            && a.HasAttribute(L"ProcName")
            && strProc.IsEmpty())
        {
            strProc = a.GetAttributeValue(L"ProcName");
            if (a.HasAttribute(L"Flags"))
                sSpecialFlags = a.GetAttributeValue(L"Flags");
        }
        if (lstrcmpiW(a.GetName(), L"ExplorerNotify") == 0
            && lstrcmpiW(a.GetAttributeValue(L"Type"),
                        L"AssocChanged") == 0)
            bAssocNotify = true;
        a = a.GetSibling();
    }
}

// [LR-Weight] 按 Item@Work / 当前 Applicable 分支 / 全分支最大 落定权重
static void LrResolveWeight(CleanRule& rule, const wchar_t* pszItemWork,
                            int nSelScanWork, int nMaxScanWork)
{
    if (pszItemWork && *pszItemWork)
    {
        int nW = _ttoi(pszItemWork);
        rule.nWeight = nW > 0 ? nW : 1;
    }
    else
    {
        rule.nWeight = nSelScanWork > 0 ? nSelScanWork
                     : (nMaxScanWork > 0 ? nMaxScanWork : 1);
    }
}

// [OS-Filter] 仅按 <Applicable> 判定规则是否属于当前系统（用于显示筛选）。
//
// 与 IsRuleApplicableNow() 的区别：
//   * IsRuleApplicableNow：Applicable 通过 **且** Activate 有命中目标——
//     回答"现在能不能扫/能删"，用于派发前的实时重评；
//   * 本函数：只看 Applicable——回答"这条规则在不在本系统的规则集里"。
//
// 显示筛选必须用后者：一条适用但当前无目标的规则（如没装 360 时
// 的"360 浏览器老版本备份"）仍应出现在列表里，只是扫描结果为 0；
// 而 24H2 才有的功能在 21H2 上必须整条不显示。
//
// 判定规则：任一 Scan/ScanCollection 分支的 Applicable 成立即视为适用；
// 全部分支都带 Applicable 且都不成立则视为不适用。
// 无 Applicable 的分支视为无条件适用（对齐 XML 语义）。
[[nodiscard]] static bool CeItemApplicable(CDataEngine* pEngine, CMarkupNode item)
{
    CMarkupNode node = item.GetChild();
    bool bAnyApp = false;
    bool bAnySatisfied = false;

    while (node.IsValid())
    {
        const wchar_t* pszName = node.GetName();
        const bool bIsScan = (lstrcmpiW(pszName, L"Scan") == 0);
        const bool bIsColl = (lstrcmpiW(pszName, L"ScanCollection") == 0);
        if (bIsScan || bIsColl)
        {
            CMarkupNode scan = bIsScan ? node : node.GetChild(L"Scan");
            while (scan.IsValid())
            {
                CMarkupNode app = scan.GetChild(L"Applicable");
                if (!app.IsValid())
                {
                    bAnySatisfied = true;   // 无门槛 = 适用
                }
                else
                {
                    bAnyApp = true;
                    if (pEngine->EvalConditions(app)) bAnySatisfied = true;
                }
                scan = scan.GetSibling();
            }
        }
        node = node.GetSibling();
    }

    // 有分支但全都带 Applicable 且都不成立 → 不属于本系统
    if (bAnyApp && !bAnySatisfied) return false;
    return true;
}

void CCleanEngine::LoadRules()
{
    // Init() may be called more than once (e.g. RefreshAll); release the
    // previously loaded markup nodes and start clean so the rule list
    // never doubles up.
    FreeRuleNodes();
    m_rules.clear();
    m_groups.clear();

    CMarkupNode root = m_pEngine->GetCleanCollectionRoot();
    if (!root.IsValid()) return;

    int iIdx = 0;
    CMarkupNode item = root.GetChild(L"Item");
    while (item.IsValid())
    {
        CleanRule rule;
        rule.iIdx = iIdx;
        rule.sName = DismResolveText(item.GetAttributeValue(L"Name"));
        const wchar_t* pszLevel = item.GetAttributeValue(L"Level");
        rule.nLevel = pszLevel ? _ttoi(pszLevel) : 0;
        rule.pNode = std::make_unique<CMarkupNode>(item).release(); // [C++17 R3] ownership transferred to m_rules container; freed by FreeRuleNodes
        rule.ullSize = 0;

        CMarkupNode desc = item.GetChild(L"Description");
        if (desc.IsValid())
            rule.sDesc = DismResolveText(desc.GetValue());

        CMarkupNode grp = item.GetChild(L"Group");
        if (grp.IsValid())
            rule.sGroup = DismResolveText(grp.GetValue());
        else
            rule.sGroup = StringMgr.Get("misc.s0d98c747");

        // [E-6][R-B39] 扫描权重与动作标记
        const wchar_t* pszItemWork = item.GetAttributeValue(L"Work");
        int nSelScanWork = 0;
        int nMaxScanWork = 0;
        bool bGeneral = false;
        bool bRestartExplorer = false;
        bool bAssocNotify = false;
        CDuiString strProc;
        CDuiString sSpecialFlags;

        CMarkupNode scanNode = item.GetChild();
        while (scanNode.IsValid())
        {
            const wchar_t* sn = scanNode.GetName();
            if (lstrcmpiW(sn, L"Scan") == 0 || lstrcmpiW(sn, L"ScanCollection") == 0)
            {
                CMarkupNode scan = (lstrcmpiW(sn, L"Scan") == 0)
                    ? scanNode : scanNode.GetChild(L"Scan");
                while (scan.IsValid())
                {
                    LrScanBranch(m_pEngine, scan, pszItemWork,
                                 nSelScanWork, nMaxScanWork,
                                 bGeneral, bRestartExplorer,
                                 bAssocNotify, strProc, sSpecialFlags);
                    scan = scan.GetSibling();
                }
            }
            scanNode = scanNode.GetSibling();
        }

        rule.bFileRule = bGeneral;
        rule.sSpecialProc = strProc;
        rule.bRestartExplorer = bRestartExplorer;
        rule.bAssocNotify = bAssocNotify;
        rule.sSpecialFlags = sSpecialFlags;
        LrResolveWeight(rule, pszItemWork, nSelScanWork, nMaxScanWork);

        // Custom 动作支持判定
        rule.bProcSupported = IsKnownProc(strProc.GetData());
        if (!strProc.IsEmpty() && !rule.bProcSupported)
            rule.sProcNote = StringMgr.Get("misc.sa8530edf");

        // [OS-Filter] 加载时按 <Applicable> 求值一次，缓存"是否属于本系统"。
        // 不适用的条目不再进入 UI（旧版本看不到新系统独有的清理项）。
        rule.bOsApplicable = CeItemApplicable(m_pEngine, item);
        rule.nMinBuild = 0;

        m_rules.push_back(rule);
        AddUnique(m_groups, rule.sGroup);
        ++iIdx;
        item = item.GetSibling();
    }
}


// ============================================================
// scanning
// ============================================================

// C-8: 硬链接去重集重置入口。ScanRule / DeleteRules 在开始计数前调用，
// 确保每个"计数周期"从零开始累计。定义先于 ScanRule 以支持前向调用。
static void PathSizeResetDedup();   // 定义见下方 "size helpers" 段

// [RE-GHIDRA 2026-10-12] 路径词法规范化（对齐原版 Dism++x64.exe
// FUN_140063ba0：根路径展开环境变量后先 PathCanonicalizeW 再枚举）。
// 真实规则实证（bin64\DismData\Data.xml）：
//   <General RootPath="?GetRegSz(...\kingsoft\...\Common,InstallRoot)\..\"
//            Flags="Directory"> —— 原版把 "...\InstallRoot\..\" 折叠为
// "...\Kingsoft\" 后枚举；旧实现保留字面 ".."，产出路径含 ".."，
// 去重/大小统计/回收站撤销路径与 UI 展示全部基于非规范串。
//
// 语义：
//   * 无通配符（* ?）的整串：PathCanonicalizeW 折叠 ./.. 与多余反斜杠。
//   * 含通配符：仅折叠第一个通配段"之前"的确定前缀（通配之后的 .. 由
//     文件系统在访问时解析，不能对未枚举的名字做词法弹栈）。
//   * 盘符根（C:\）永不被 .. 弹出。
static void NormalizePathPrefix(CDuiString& sPath)
{
    if (sPath.IsEmpty()) return;

    if (sPath.Find(L'*') < 0 && sPath.Find(L'?') < 0)
    {
        wchar_t szBuf[MAX_PATH * 2] = { 0 };
        if (::PathCanonicalize(szBuf, sPath.GetData()))
            sPath = szBuf;
        return;
    }

    // 含通配：折叠首个通配段之前的确定性前缀
    int nRootLen = 0;
    if (sPath.GetLength() >= 3 && sPath[1] == L':' && sPath[2] == L'\\')
        nRootLen = 3;

    CDuiString sNorm = sPath.Mid(0, nRootLen);
    CDuiString sRest = sPath.Mid(nRootLen);

    bool bWild = false;
    std::vector<CDuiString> vSeg;
    int iStart = 0;
    for (int i = 0; i <= sRest.GetLength(); ++i)
    {
        if (i < sRest.GetLength() && sRest[i] != L'\\') continue;

        CDuiString sSeg = sRest.Mid(iStart, i - iStart);
        iStart = i + 1;

        if (!bWild && (sSeg.Find(L'*') >= 0 || sSeg.Find(L'?') >= 0))
        {
            // [P0-RE 2026-10-12] 通配开始：先把此前累积的【全部确定段】写回，
            // 再接回当前通配段起到末尾。旧实现漏写 vSeg，把
            //   C:\Windows\System32\WDI\LogFiles\* 折叠成 C:\*（枚举整盘根，
            //   实测 Windows日志规则虚报 90.49GB / 误触 hiberfil.sys），
            //   C:\Users\*\AppData\... 折叠成 C:\*\AppData\...（丢 Users，
            //   探测根必然失败 → 10 条规则误报"扫描错误"）。
            for (size_t k = 0; k < vSeg.size(); ++k)
            {
                sNorm += vSeg[k];
                sNorm += L'\\';
            }
            int nTailStart = i - static_cast<int>(sSeg.GetLength());
            sNorm += sRest.Mid(nTailStart);
            sPath = sNorm;
            return;
        }

        if (sSeg == L".")
        {
            // 丢弃
        }
        else if (sSeg == L"..")
        {
            if (!vSeg.empty()) vSeg.pop_back();
        }
        else if (!sSeg.IsEmpty())
        {
            vSeg.push_back(sSeg);
        }
    }

    for (size_t k = 0; k < vSeg.size(); ++k)
    {
        sNorm += vSeg[k];
        sNorm += L'\\';
    }
    if (sPath.GetLength() > nRootLen
        && sPath[sPath.GetLength() - 1] != L'\\'
        && sNorm.GetLength() > nRootLen
        && sNorm[sNorm.GetLength() - 1] == L'\\')
    {
        sNorm = sNorm.Left(sNorm.GetLength() - 1);
    }
    sPath = sNorm;
}

// ============================================================
// [R13-01] ScanRule 深度拆解辅助（d7->d<=5）
// 仅提取 Scan 分支路径收集、targets 重建、probe-root 二级判定，
// 逐字搬迁原逻辑，零行为改动。
// ============================================================

// [SR-Scan] 枚举 Scan/ScanCollection 分支，收集 General 目标路径到 paths
void CCleanEngine::SrCollectScanPaths(CMarkupNode top, CleanRule& rule,
                                  std::vector<CDuiString>& paths)
{
    const wchar_t* tn = top.GetName();
    if (lstrcmpiW(tn, L"Scan") != 0 && lstrcmpiW(tn, L"ScanCollection") != 0)
        return;
    CMarkupNode scan = (lstrcmpiW(tn, L"Scan") == 0)
        ? top : top.GetChild(L"Scan");
    while (scan.IsValid())
    {
        CMarkupNode app = scan.GetChild(L"Applicable");
        if (app.IsValid() && !m_pEngine->EvalConditions(app))
        {
            scan = scan.GetSibling();
            continue;
        }
        CMarkupNode act = scan.GetChild(L"Activate");
        if (act.IsValid())
        {
            CMarkupNode gen = act.GetChild(L"General");
            while (gen.IsValid())
            {
                if (gen.HasAttribute(L"ID"))
                {
                    CMarkupNode gate =
                        scan.GetChild(gen.GetAttributeValue(L"ID"));
                    if (gate.IsValid() && !m_pEngine->EvalConditions(gate))
                    {
                        gen = gen.GetSibling();
                        continue;
                    }
                }
                ProcessGeneral(gen, rule);
                for (size_t k = 0; k < rule.targets.size(); ++k)
                    AddUnique(paths, rule.targets[k].sPath);
                gen = gen.GetSibling();
            }
        }
        scan = scan.GetSibling();
    }
}

// [SR-Rebuild] 按 paths 重建 targets 并统计文件属性错误
void CCleanEngine::SrRebuildTargets(CleanRule& rule, const std::vector<CDuiString>& paths,
                                    int& nErrAccess, int& nErrNotFound, int& nErrIo)
{
    rule.targets.clear();
    for (size_t i = 0; i < paths.size(); ++i)
    {
        DWORD attr = ::GetFileAttributes(paths[i].GetData());
        if (attr == INVALID_FILE_ATTRIBUTES)
        {
            DWORD dw = ::GetLastError();
            if (dw == ERROR_ACCESS_DENIED)          ++nErrAccess;
            else if (dw == ERROR_FILE_NOT_FOUND ||
                     dw == ERROR_PATH_NOT_FOUND ||
                     dw == ERROR_DIRECTORY)          ++nErrNotFound;
            else                                     ++nErrIo;
            if (rule.nLastError == ScanRuleError::SCAN_RULE_OK)
            {
                if (dw == ERROR_ACCESS_DENIED)      rule.nLastError = ScanRuleError::SCAN_RULE_ACCESS_DENIED;
                else if (dw == ERROR_FILE_NOT_FOUND ||
                         dw == ERROR_PATH_NOT_FOUND ||
                         dw == ERROR_DIRECTORY)      rule.nLastError = ScanRuleError::SCAN_RULE_PATH_NOT_FOUND;
                else                                 rule.nLastError = ScanRuleError::SCAN_RULE_IO_ERROR;
                rule.sLastErrPath = paths[i];
            }
            continue;
        }
        bool bDir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
        ULONGLONG szOne = PathSize(paths[i], bDir);
        rule.ullSize += szOne;
        CleanTarget t;
        t.sPath = paths[i];
        t.bDir = bDir;
        rule.targets.push_back(t);
    }
}

// [SR-Probe] 二级判定：glob 层零命中时对探测根做属性判定
void CCleanEngine::SrProbeRootFallback(CleanRule& rule,
                                       int& nErrAccess, int& nErrNotFound, int& nErrIo)
{
    if (!rule.targets.empty()
        || (nErrAccess + nErrNotFound + nErrIo) != 0)
        return;
    for (size_t pr = 0; pr < rule.vProbeRoots.size(); ++pr)
    {
        if (rule.vProbeRoots[pr].Find(L'*') >= 0
            || rule.vProbeRoots[pr].Find(L'?') >= 0) continue;
        DWORD dwR = ::GetFileAttributes(rule.vProbeRoots[pr].GetData());
        if (dwR == INVALID_FILE_ATTRIBUTES)
        {
            DWORD dwG = ::GetLastError();
            ScanRuleError nCls;
            if (dwG == ERROR_ACCESS_DENIED) { ++nErrAccess; nCls = ScanRuleError::SCAN_RULE_ACCESS_DENIED; }
            else if (dwG == ERROR_FILE_NOT_FOUND || dwG == ERROR_PATH_NOT_FOUND)
                { ++nErrNotFound; nCls = ScanRuleError::SCAN_RULE_PATH_NOT_FOUND; }
            else { ++nErrIo; nCls = ScanRuleError::SCAN_RULE_IO_ERROR; }
            if (rule.nLastError == ScanRuleError::SCAN_RULE_OK)
            {
                rule.nLastError = nCls;
                rule.sLastErrPath = rule.vProbeRoots[pr];
            }
        }
    }
}

// ============================================================
// [FIX-WINSXS] 被取代的 WinSxS 组件枚举器
// ============================================================
// 目录名格式（微软组件存储标准）：
//   <arch>_<name>_<pubkey>_<revkey>_<version>_<lang>_<hash>
//   · name 可含下划线；version 形如 40.0.15912.0（4 段数字）；
//   · 从右往左固定取 hash / lang / version / revkey 四段，剩余头两段
//     为 arch + name。
// 分组键 = arch|name|revkey|lang；组内 version 最大者为现行组件，
// 其余目录 = 被取代版本（可被 StartComponentCleanup 清理）。
namespace {

// "a.b.c.d" → 4×16bit 打包（比较用）；非纯数字段返回 false
bool WsxParseVersion(const CDuiString& s, ULONGLONG& out)
{
    const wchar_t* p = s.GetData();
    ULONGLONG v = 0;
    int nDot = 0;
    for (int i = 0; i < 4; ++i)
    {
        ULONGLONG d = 0;
        if (*p < L'0' || *p > L'9') return false;
        while (*p >= L'0' && *p <= L'9')
        {
            d = d * 10 + (*p - L'0');
            if (d > 0xFFFF) return false;
            ++p;
        }
        v = (v << 16) | (d & 0xFFFF);
        if (i < 3)
        {
            if (*p != L'.') return false;
            ++p; ++nDot;
        }
    }
    if (*p != L'\0' || nDot != 3) return false;
    out = v;
    return true;
}

// 目录名 → 分组键 + 版本；格式不符返回 false（跳过该目录）
bool WsxParseDirName(const wchar_t* pszName, CDuiString& sKey, ULONGLONG& ver)
{
    if (!pszName || !pszName[0]) return false;
    std::vector<CDuiString> segs;
    const wchar_t* p = pszName;
    while (*p)
    {
        const wchar_t* q = wcschr(p, L'_');
        if (!q) { segs.push_back(CDuiString(p)); break; }
        segs.push_back(CDuiString(p, static_cast<int>(q - p)));
        p = q + 1;
    }
    // 至少 7 段（arch name pubkey revkey version lang hash）
    if (segs.size() < 7) return false;
    const int n = static_cast<int>(segs.size());
    if (!WsxParseVersion(segs[n - 3], ver)) return false;
    // key = arch | name(1..n-5) | revkey(n-4) | lang(n-2)
    CDuiString sName;
    for (int i = 1; i <= n - 5; ++i)
    {
        if (i > 1) sName += L'_';
        sName += segs[i];
    }
    sKey.Format(L"%s|%s|%s|%s", segs[0].GetData(), sName.GetData(),
                segs[n - 4].GetData(), segs[n - 2].GetData());
    return true;
}

} // namespace

// 两遍式：先枚举+分组定位"被取代"目录，再仅对这些目录累加大小。
// 大小经 PathSize（规则内硬链接去重），量级与原版一致（截图样例 5.09 GB）。
// 注意：只填 rule.ullSize，绝不填 targets —— 被取代组件的删除必须走
// DISM StartComponentCleanup（CBS 事务），直接删目录会损坏组件存储；
// 清理路径由 Custom 派发（CPI_DISMCOMP → RunOnlineComponentCleanup）承担。
void CCleanEngine::SrScanSupersededWinSxS(CleanRule& rule)
{
    if (rule.ullSize != 0) return;   // 已统计过
    CDuiString sRoot = ResolveRootPath(L"%SystemRoot%\\WinSxS");
    if (sRoot.IsEmpty()) return;

    // 1) 枚举一级目录并分组：key → (maxVer, maxIdx)；同时登记全部成员
    struct WsxEntry
    {
        CDuiString sDir;
        ULONGLONG  ver;
    };
    std::vector<WsxEntry> vAll;
    WIN32_FIND_DATAW fd;
    CLEFindGuard hFind(::FindFirstFileW(CDuiString(sRoot + L"\\*").GetData(), &fd));
    if (hFind == INVALID_HANDLE_VALUE) return;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (lstrcmpW(fd.cFileName, L".") == 0 || lstrcmpW(fd.cFileName, L"..") == 0)
            continue;
        CDuiString sKey;
        ULONGLONG ver = 0;
        if (!WsxParseDirName(fd.cFileName, sKey, ver)) continue;
        WsxEntry e;
        e.sDir = sRoot + L"\\" + fd.cFileName;
        e.ver = ver;
        vAll.push_back(e);
    } while (::FindNextFileW(hFind, &fd));

    if (vAll.empty()) return;

    // 2) 组内取最高版本（相同最高版本的多个 hash 变体同样保留）
    std::map<CDuiString, ULONGLONG> mMaxVer;
    for (size_t i = 0; i < vAll.size(); ++i)
    {
        CDuiString sKey;
        ULONGLONG ver = 0;
        if (!WsxParseDirName(vAll[i].sDir.GetData(), sKey, ver)) continue;
        auto it = mMaxVer.find(sKey);
        if (it == mMaxVer.end() || ver > it->second) mMaxVer[sKey] = ver;
    }
    // 3) 被取代目录：ver < 组内 max → 累加大小
    ULONGLONG ullTotal = 0;
    int nSuperseded = 0;
    for (size_t i = 0; i < vAll.size(); ++i)
    {
        CDuiString sKey;
        ULONGLONG ver = 0;
        if (!WsxParseDirName(vAll[i].sDir.GetData(), sKey, ver)) continue;
        auto it = mMaxVer.find(sKey);
        if (it == mMaxVer.end() || ver >= it->second) continue;   // 现行组件
        ullTotal += PathSize(vAll[i].sDir, true);
        ++nSuperseded;
    }
    rule.ullSize = ullTotal;
    wchar_t szLog[160] = { 0 };
    wfmt::FormatInto(szLog, _countof(szLog),
        L"[clean] WinSxS superseded components: %d dir(s), %I64u bytes\r\n",
        nSuperseded, ullTotal);
    ::OutputDebugStringW(szLog);
}

int CCleanEngine::ScanRule(int iIdx)
{
    if (iIdx < 0 || iIdx >= static_cast<int>(m_rules.size())) return 0;
    CleanRule& rule = m_rules[iIdx];
    rule.targets.clear();
    rule.vProbeRoots.clear();
    rule.ullSize = 0;
    // A4: 重置上一次扫描的错误记录
    rule.nLastError = ScanRuleError::SCAN_RULE_OK;
    rule.sLastErrPath.Empty();

    // C-8: 重置 PathSize 硬链接去重集
    PathSizeResetDedup();

    // A3: 局部错误计数器
    int nErrAccess  = 0;
    int nErrNotFound = 0;
    int nErrIo      = 0;

    std::vector<CDuiString> paths;

    // enumerate all Scan nodes (direct or inside ScanCollection)
    CMarkupNode top = rule.pNode->GetChild();
    while (top.IsValid())
    {
        SrCollectScanPaths(top, rule, paths);
        top = top.GetSibling();
    }

    // rebuild deduped targets and compute sizes
    SrRebuildTargets(rule, paths, nErrAccess, nErrNotFound, nErrIo);

    // [B1-RE] glob 层零命中时对探测根做二级属性判定
    SrProbeRootFallback(rule, nErrAccess, nErrNotFound, nErrIo);

    // [FIX-WINSXS] Custom 组件清理规则的扫描尺寸：General 文件规则无法
    // 表达"被取代组件"，用专用枚举器按 identity 分组统计（只填大小）。
    if (rule.targets.empty() && rule.sSpecialProc == L"DismComponentCleanup")
        SrScanSupersededWinSxS(rule);

    // nLastError / sLastErrPath 内部诊断留痕：由 SrRebuildTargets
    // （逐路径 GetFileAttributes 失败即记录首个）与 SrProbeRootFallback
    // （glob 零命中 + 探测根确认失败时）统一赋值，返回值始终为 targets.size()。
    // nErrAccess/nErrNotFound/nErrIo 是上述两函数的内部累计计数，最终值由
    // 各自内部写入 rule.nLastError，ScanRule 不再重复消费 —— (void) 仅消除警告。
    (void)nErrAccess; (void)nErrNotFound; (void)nErrIo;
    return static_cast<int>(rule.targets.size());
}


// ============================================================
// [E-2] 清理前按当前实时状态重评 Applicable（见头文件说明）
// ============================================================
// [R12-01] IsRuleApplicableNow 深度拆解辅助（d8->d<=5）
// 仅提取 Activate 双分支判定，逐字搬迁原逻辑，零行为改动。

// [E-2a] 单 Scan 节点是否满足：Applicable 通过 + Activate 命中
    // [C++17 R3] [[nodiscard]]：返回值勿忽略
[[nodiscard]] static bool CeScanSatisfied(CDataEngine* pEngine, CMarkupNode scan, const wchar_t* pszProc)
{
    CMarkupNode app = scan.GetChild(L"Applicable");
    if (app.IsValid() && !pEngine->EvalConditions(app)) return false;

    CMarkupNode act = scan.GetChild(L"Activate");
    if (!act.IsValid()) return false;

    if (!pszProc || !*pszProc)
    {
        // 文件规则：存在通过 ID 门控（Applicable_x）的 General
        CMarkupNode gen = act.GetChild(L"General");
        while (gen.IsValid())
        {
            if (gen.HasAttribute(L"ID"))
            {
                CMarkupNode gate =
                    scan.GetChild(gen.GetAttributeValue(L"ID"));
                if (gate.IsValid() && !pEngine->EvalConditions(gate))
                {
                    gen = gen.GetSibling();
                    continue;
                }
            }
            return true;
        }
    }
    else
    {
        // Custom 动作：存在 ProcName 匹配的 Custom（不再校验
        // General 门控——Custom 与 General 是并列动作）
        CMarkupNode a = act.GetChild();
        while (a.IsValid())
        {
            if (lstrcmpiW(a.GetName(), L"Custom") == 0
                && a.HasAttribute(L"ProcName")
                && lstrcmpiW(a.GetAttributeValue(L"ProcName"),
                            pszProc) == 0)
                return true;
            a = a.GetSibling();
        }
    }
    return false;
}

// [E-2b] 顶层 Scan / ScanCollection 节点遍历
    // [C++17 R3] [[nodiscard]]：返回值勿忽略
[[nodiscard]] static bool CeTopSatisfied(CDataEngine* pEngine, CMarkupNode top, const wchar_t* pszProc)
{
    const wchar_t* tn = top.GetName();
    if (lstrcmpiW(tn, L"Scan") != 0
        && lstrcmpiW(tn, L"ScanCollection") != 0)
        return false;
    CMarkupNode scan = (lstrcmpiW(tn, L"Scan") == 0)
        ? top : top.GetChild(L"Scan");
    while (scan.IsValid())
    {
        if (CeScanSatisfied(pEngine, scan, pszProc)) return true;
        scan = scan.GetSibling();
    }
    return false;
}


bool CCleanEngine::IsRuleApplicableNow(int iIdx, const wchar_t* pszProc /*= nullptr*/)
{
    if (iIdx < 0 || iIdx >= static_cast<int>(m_rules.size())) return false;
    CleanRule& rule = m_rules[iIdx];
    if (!rule.pNode || !m_pEngine) return false;

    CMarkupNode top = rule.pNode->GetChild();
    while (top.IsValid())
    {
        if (CeTopSatisfied(m_pEngine, top, pszProc)) return true;
        top = top.GetSibling();
    }
    return false;
}


// ============================================================
// [E-4] 收集当前适用分支的 Warning 文案（见头文件说明）
// ============================================================
void CCleanEngine::CollectRuleWarnings(int iIdx, std::vector<CDuiString>& out)
{
    if (iIdx < 0 || iIdx >= static_cast<int>(m_rules.size())) return;
    CleanRule& rule = m_rules[iIdx];
    if (!rule.pNode) return;

    // 从 first-child 起遍历整组兄弟，收集其中的 Warning 节点文案
    struct Local {
        static void AddWarnings(CMarkupNode first, std::vector<CDuiString>& out)
        {
            CMarkupNode w = first;
            while (w.IsValid())
            {
                if (lstrcmpiW(w.GetName(), L"Warning") == 0)
                {
                    CDuiString sw = DismResolveText(w.GetValue());
                    if (!sw.IsEmpty()) CCleanEngine::AddUnique(out, sw);
                }
                w = w.GetSibling();
            }
        }
    };

    // 1) Item 直属 Warning（如「以前的 Windows 安装」条目）
    Local::AddWarnings(rule.pNode->GetChild(), out);

    // 2) 各 Applicable 当前成立的 Scan 分支内 Warning
    if (m_pEngine)
    {
        CMarkupNode top = rule.pNode->GetChild();
        while (top.IsValid())
        {
            const wchar_t* tn = top.GetName();
            if (lstrcmpiW(tn, L"Scan") == 0 || lstrcmpiW(tn, L"ScanCollection") == 0)
            {
                CMarkupNode scan = (lstrcmpiW(tn, L"Scan") == 0)
                    ? top : top.GetChild(L"Scan");
                while (scan.IsValid())
                {
                    CMarkupNode app = scan.GetChild(L"Applicable");
                    if (!(app.IsValid() && !m_pEngine->EvalConditions(app)))
                        Local::AddWarnings(scan.GetChild(), out);
                    scan = scan.GetSibling();
                }
            }
            top = top.GetSibling();
        }
    }
}

void CCleanEngine::ProcessGeneral(CMarkupNode nodeGeneral, CleanRule& rule)
{
    CDuiString strRoot = ResolveRootPath(nodeGeneral.GetAttributeValue(L"RootPath"));
    // [RE-GHIDRA] 原版 FUN_140063ba0 在枚举前对根路径做 PathCanonicalizeW；
    // 折叠 \..\ 等词法段，保证后续 glob/删除/撤销全部使用规范路径。
    NormalizePathPrefix(strRoot);
    // [B1-RE] 记录探测根（去重），供 glob 零命中时错误分类
    if (!strRoot.IsEmpty())
    {
        bool bDup = false;
        for (size_t rr = 0; rr < rule.vProbeRoots.size(); ++rr)
            if (!lstrcmpiW(rule.vProbeRoots[rr], strRoot)) { bDup = true; break; }
        if (!bDup) rule.vProbeRoots.push_back(strRoot);
    }
    bool bFileFlag = nodeGeneral.HasAttribute(L"Flags")
        && lstrcmpiW(nodeGeneral.GetAttributeValue(L"Flags"), L"File") == 0;
    bool bDirFlag = nodeGeneral.HasAttribute(L"Flags")
        && lstrcmpiW(nodeGeneral.GetAttributeValue(L"Flags"), L"Directory") == 0;

    std::vector<CDuiString> queries;
    CollectGeneralQueries(nodeGeneral, queries);

    // 收集所有 Excluded 子节点，并把函数式排除展开成可匹配的通配模式
    std::vector<CDuiString> exclPatterns;
    const bool bExclResolved = CollectGeneralExcludes(nodeGeneral, strRoot, exclPatterns);

    // fail-closed：只要存在无法解析的函数式排除规则，整组不产出任何目标，
    // 避免“排除不生效 -> 本应保留的版本目录被直删”。
    if (!bExclResolved)
        return;

    // [B1-RE] 无 Query 无 Excluded 且根不含通配：根路径自身即目标。
    // 原版 FUN_140063ba0 零 query 时按 Flags 把根直接入结果集：
    //   缺省/Directory + 目录 → 命中目录自身；File + 文件 → 命中文件；
    //   类型不符（File 指向目录等）→ 不产出。
    // 根含通配（如段内 *）且无 Query 时，仍按 "*" 枚举根内容。
    bool bSelfHandled = false;
    if (queries.empty() && exclPatterns.empty()
        && strRoot.Find(L'*') < 0 && strRoot.Find(L'?') < 0)
    {
        DWORD dwAttrSelf = ::GetFileAttributes(strRoot.GetData());
        if (dwAttrSelf != INVALID_FILE_ATTRIBUTES)
        {
            bool bIsDir = (dwAttrSelf & FILE_ATTRIBUTE_DIRECTORY) != 0;
            bool bTypeOk = (!bFileFlag && !bDirFlag)
                        || (bDirFlag && bIsDir)
                        || (bFileFlag && !bIsDir);
            if (bTypeOk)
            {
                CleanTarget t;
                t.sPath = strRoot;
                t.bDir = bIsDir;
                rule.targets.push_back(t);
            }
            bSelfHandled = true;
        }
    }
    if (bSelfHandled) return;

    if (queries.empty()) queries.push_back(L"*");

    // 根本身含通配（如前导 * 盘符枚举）时，默认 Query 不再追加 "\\*"
    bool bRootHasWild = (strRoot.Find(L'*') >= 0 || strRoot.Find(L'?') >= 0);

    for (size_t i = 0; i < queries.size(); ++i)
    {
        CDuiString pattern = strRoot;
        bool bDefaultStar = bRootHasWild && i == 0
                            && queries.size() == 1
                            && queries[i] == L"*";
        if (!bDefaultStar)
        {
            if (!pattern.IsEmpty() && pattern[pattern.GetLength() - 1] != L'\\')
                pattern += L"\\";
            pattern += queries[i];
        }

        std::vector<CDuiString> v;
        // 原版语义：Flags 决定末级类型，缺省文件/目录都收
        GlobMatch nWant = bFileFlag ? GlobMatch::GLOB_FILE : (bDirFlag ? GlobMatch::GLOB_DIR : GlobMatch::GLOB_BOTH);
        GlobPaths(pattern, nWant, v);
        for (size_t k = 0; k < v.size(); ++k)
        {
            // 命中任一 Excluded 模式（含函数式展开结果）即剔除
            if (MatchExcluded(exclPatterns, v[k])) continue;
            CleanTarget t;
            t.sPath = v[k];
            if (nWant == GlobMatch::GLOB_FILE)      t.bDir = false;
            else if (nWant == GlobMatch::GLOB_DIR)  t.bDir = true;
            else
            {
                DWORD a = ::GetFileAttributes(v[k].GetData());
                t.bDir = (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY));
            }
            rule.targets.push_back(t);
        }
    }
}

// ============================================================
// path resolution
// ============================================================
static void ReplaceTokenAll(CDuiString& s, const wchar_t* pszToken, const CDuiString& strVal)
{
    CDuiString strTok(pszToken);
    int rp = 0;
    while ((rp = s.Find(strTok.GetData(), rp)) >= 0)
    {
        s = s.Mid(0, rp) + strVal + s.Mid(rp + strTok.GetLength());
        rp += strVal.GetLength();
    }
}

// [N16] T2 抽纯函数：计算 Wow64 视图 REGSAM 附加位（原 ResolveRootPath 内 4 分支 if 内联）
static REGSAM CleWowSam(HKEY hKey, bool bHasWow, bool bWow32)
{
    REGSAM sam = KEY_READ;
    if (bHasWow)
    {
        // True=32 位视图，False=64 位视图（与优化规则 Wow64 同义）
        if (bWow32) sam |= KEY_WOW64_32KEY;
        else if (hKey == HKEY_LOCAL_MACHINE || hKey == HKEY_CLASSES_ROOT)
            sam |= KEY_WOW64_64KEY;
    }
    else if (hKey == HKEY_LOCAL_MACHINE || hKey == HKEY_CLASSES_ROOT)
        sam |= KEY_WOW64_64KEY;
    return sam;
}

// [N16] T2 抽纯函数：读注册表 REG_SZ/REG_EXPAND_SZ 值（原 ResolveRootPath 内 RegOpen/Query 内联）
static bool CleReadRegSzValue(HKEY hRoot, const wchar_t* pszSub, const wchar_t* pszVal,
                              REGSAM sam, CDuiString& sOut)
{
    CLERegGuard hOpened;
    if (::RegOpenKeyEx(hRoot, pszSub, 0, sam, hOpened.out()) != ERROR_SUCCESS)
        return false;
    DWORD dwType = 0, dwSize = 0;
    if (::RegQueryValueEx(hOpened, pszVal, nullptr, &dwType, nullptr, &dwSize) != ERROR_SUCCESS
        || (dwType != REG_SZ && dwType != REG_EXPAND_SZ))
        return false;
    std::vector<wchar_t> buf(dwSize / sizeof(wchar_t) + 2, 0);
    ::RegQueryValueEx(hOpened, pszVal, nullptr, nullptr,
                      reinterpret_cast<BYTE*>(buf.data()), &dwSize);
    sOut = buf.data();
    return true;
}

CDuiString CCleanEngine::ResolveRootPath(const wchar_t* pszRaw)
{
    CDuiString s(pszRaw ? pszRaw : L"");

    // [B1-RE] 原版 FUN_14006253c：函数表达式只替换 "?Func(...)" 子串，
    // 右括号之后的字面后缀必须保留。真实规则形态：
    //   ?GetRegSz(...\Common,InstallRoot)\..\sibling
    // 旧实现整体覆盖为注册表值，\..\sibling 被丢弃（规则静默失效）。
    CDuiString sFuncSuffix;
    {
        int rp = s.Find(L')');
        if (rp >= 0 && s.Find(L'?') >= 0 && rp + 1 < s.GetLength())
            sFuncSuffix = s.Mid(rp + 1);
    }

    // ?GetRegSz(HKEY_LOCAL_MACHINE\..\Key,ValueName[,True|False])
    if (s.Find(L"?GetRegSz(") == 0)
    {
        int end = s.Find(L')');
        if (end > 10)
        {
            CDuiString body = s.Mid(10, end - 10);

            // 第三个参数 True/False 是 Wow64 视图提示，先从末尾剥离，
            // 不能只按第一个逗号切——否则 "Key,Path,True" 的值名会
            // 被错切成 "Path,True" 导致查询必然失败（阿里旺旺/2345/IDM 规则）。
            bool bHasWow = false, bWow32 = false;
            int iLastComma = body.ReverseFind(L',');
            if (iLastComma > 0)
            {
                CDuiString strTail = body.Mid(iLastComma + 1);
                strTail.Trim();
                if (lstrcmpiW(strTail.GetData(), L"True") == 0
                    || lstrcmpiW(strTail.GetData(), L"False") == 0)
                {
                    bHasWow = true;
                    bWow32 = (lstrcmpiW(strTail.GetData(), L"True") == 0);
                    body = body.Mid(0, iLastComma);
                }
            }

            int comma = body.Find(L',');
            if (comma > 0)
            {
                CDuiString strKey = body.Mid(0, comma);
                CDuiString strVal = body.Mid(comma + 1);
                strVal.Trim();

                HKEY hKey = nullptr;
                CDuiString result;
                // [P0] 离线会话：按 wow 提示重定向到挂载蜂巢后再解析
                CDuiString sRegMap = COfflineSession::RemapPath(strKey.GetData(), bWow32);
                if (COfflineSession::ParseRegPath(sRegMap.GetData(), hKey, strKey))
                {
                    REGSAM sam = CleWowSam(hKey, bHasWow, bWow32);
                    CleReadRegSzValue(hKey, strKey.GetData(), strVal.GetData(), sam, result);
                }
                s = result + sFuncSuffix;
            }
        }
    }

    // ---- Dism++ 自定义变量（ExpandEnvironmentStrings 不认识） ----

    // %0% —— 规则包内指程序自身目录（带尾部反斜杠）
    {
        wchar_t szMod[MAX_PATH * 2] = { 0 };
        ::GetModuleFileName(nullptr, szMod, _countof(szMod));
        CDuiString sDir(szMod);
        int pp = sDir.ReverseFind(L'\\');
        if (pp >= 0) sDir = sDir.Mid(0, pp + 1);
        ReplaceTokenAll(s, L"%0%", sDir);
    }

    // %ProgramFilesX86% —— 32 位 Program Files（x64 系统为 C:\Program Files (x86)）
    {
        wchar_t szPath[MAX_PATH] = { 0 };
        if (SUCCEEDED(::SHGetFolderPath(nullptr, CSIDL_PROGRAM_FILESX86,
                                       nullptr, 0, szPath)))
            ReplaceTokenAll(s, L"%ProgramFilesX86%", CDuiString(szPath));
    }

    // %SystemX86% —— 32 位系统目录（x64 系统为 SysWOW64）
    {
        wchar_t szWow[MAX_PATH] = { 0 };
        UINT u = ::GetSystemWow64Directory(szWow, MAX_PATH);   // x86 系统返回 0
        if (u > 0 && u < MAX_PATH)
            ReplaceTokenAll(s, L"%SystemX86%", CDuiString(szWow));
        else
        {
            wchar_t szSys[MAX_PATH] = { 0 };
            ::GetSystemDirectory(szSys, MAX_PATH);
            ReplaceTokenAll(s, L"%SystemX86%", CDuiString(szSys));
        }
    }

    // %Documents% —— 当前用户“我的文档”
    {
        wchar_t szDoc[MAX_PATH] = { 0 };
        if (SUCCEEDED(::SHGetFolderPath(nullptr, CSIDL_PERSONAL,
                                       nullptr, 0, szDoc)))
            ReplaceTokenAll(s, L"%Documents%", CDuiString(szDoc));
    }

    // custom %System% variable -> system directory
    wchar_t szSys[MAX_PATH];
    ::GetSystemDirectory(szSys, MAX_PATH);
    ReplaceTokenAll(s, L"%System%", CDuiString(szSys));

    // [P0 2026-10-07] WinPE 离线会话：PE 的环境变量全部指向 X: RAM 盘，
    // 必须在 ExpandEnvironmentStrings 之前把系统/用户目录令牌改写为目标根，
    // 否则扫描与清理会作用在 PE 自身（重启即失，且误删 X: 有风险）。
    if (COfflineSession::IsActive())
    {
        const CDuiString sRoot = COfflineSession::GetImageRoot();   // 如 "D:\"
        CDuiString sBase = sRoot;
        if (!sBase.IsEmpty() && sBase[sBase.GetLength() - 1] == L'\\')
            sBase = sBase.Mid(0, sBase.GetLength() - 1);                // "D:"
        CDuiString sWin = sBase + L"\\Windows";
        ReplaceTokenAll(s, L"%SystemRoot%", sWin);
        ReplaceTokenAll(s, L"%windir%", sWin);
        ReplaceTokenAll(s, L"%SystemDrive%", sBase);
        ReplaceTokenAll(s, L"%ProgramFiles%", sBase + L"\\Program Files");
        ReplaceTokenAll(s, L"%ProgramW6432%", sBase + L"\\Program Files");
        ReplaceTokenAll(s, L"%ProgramFiles(x86)%", sBase + L"\\Program Files (x86)");
        ReplaceTokenAll(s, L"%ProgramData%", sBase + L"\\ProgramData");
        ReplaceTokenAll(s, L"%AllUsersProfile%", sBase + L"\\ProgramData");
        ReplaceTokenAll(s, L"%UserProfile%", sBase + L"\\Users\\Default");
        ReplaceTokenAll(s, L"%AppData%", sBase + L"\\Users\\Default\\AppData\\Roaming");
        ReplaceTokenAll(s, L"%LocalAppData%", sBase + L"\\Users\\Default\\AppData\\Local");
        ReplaceTokenAll(s, L"%Public%", sBase + L"\\Users\\Public");
        ReplaceTokenAll(s, L"%TEMP%", sWin + L"\\Temp");
        ReplaceTokenAll(s, L"%TMP%", sWin + L"\\Temp");
        // %SystemX86% / %System% 已被上面按 PE 展开：重写为目标 SysWOW64/System32
        ReplaceTokenAll(s, CDuiString(szSys), sWin + L"\\System32");
        wchar_t szWowPe[MAX_PATH] = { 0 };
        if (::GetSystemWow64Directory(szWowPe, MAX_PATH) > 0)
            ReplaceTokenAll(s, CDuiString(szWowPe), sWin + L"\\SysWOW64");
        // %ProgramFilesX86% / %Documents% 上面已被展开为 PE 路径，由 X: 兜底重定基
    }

    wchar_t szExp[MAX_PATH * 2];
    DWORD n = ::ExpandEnvironmentStrings(s.GetData(), szExp, MAX_PATH * 2);
    if (n > 0 && n < MAX_PATH * 2) s = szExp;

    // [P0] 离线会话兜底：仍有指向 PE 系统卷（通常为 X:\）的展开结果，
    // 重定基到目标根；用户固定盘其它路径保持不动。
    if (COfflineSession::IsActive())
    {
        wchar_t szWinDir[MAX_PATH] = { 0 };
        ::GetWindowsDirectory(szWinDir, MAX_PATH);
        CDuiString sPeWin(szWinDir);                       // "X:\\Windows"
        CDuiString sPeRoot = sPeWin.Mid(0, 3);            // "X:\\"
        CDuiString sRoot = COfflineSession::GetImageRoot();
        CDuiString sBase = sRoot;
        if (!sBase.IsEmpty() && sBase[sBase.GetLength() - 1] == L'\\')
            sBase = sBase.Mid(0, sBase.GetLength() - 1);
        if (!sPeRoot.IsEmpty() && _wcsnicmp(s.GetData(), sPeWin, sPeWin.GetLength()) == 0)
            s = sBase + L"\\Windows" + s.Mid(sPeWin.GetLength());
        else if (_wcsnicmp(s.GetData(), sPeRoot, sPeRoot.GetLength()) == 0
                 && _wcsnicmp(sPeRoot, sRoot + L"\\", 3) != 0)
            s = sBase + s.Mid(2);
    }

    s.Trim();
    return s;
}

// ============================================================
// Custom ProcName 白名单
// ============================================================
bool CCleanEngine::IsKnownProc(const wchar_t* pszProc)
{
    if (!pszProc || !*pszProc) return false;
    // 与 FrameWnd::RunCustomProcAction / StartDism 实际接通的 12 个 Custom Proc 对齐，
    // 避免描述面板把“已支持”误报成“需 NCleaner 未内置”。
    static const wchar_t* s_known[] = {
        L"DismComponentCleanup",
        L"DismAppxsCleanup",
        L"DismHardLinkMerge",
        L"DismDriverCleanup",
        L"DismCompactOs",
        L"NCInstallerFolderCleanup",
        L"NCCorruptedAppXOnlineCleanup",
        L"NCSRPointOnlineCleanup",
        L"NCDOCacheOnlineCleanup",
        L"NCPackageCacheFolderCleanup",
        L"NCVSPackageCacheCleanup",
        L"NCWinEventLogOnlineCleanup",
        L"NCSpoolerSafeCleanup",
    };
    for (size_t i = 0; i < _countof(s_known); ++i)
        if (lstrcmpiW(pszProc, s_known[i]) == 0) return true;
    return false;
}

// ============================================================
// 函数式 Excluded 解析
// ============================================================

// 提取 "?Func(body)suffix" 的 body 与右括号之后的 suffix
static bool SplitFuncCall(const CDuiString& strRaw, const wchar_t* pszFunc,
                          CDuiString& strBody, CDuiString& strSuffix)
{
    int nPref = static_cast<int>(wcslen(pszFunc));
    if (strRaw.Find(pszFunc) != 0) return false;
    int end = strRaw.Find(L')');
    if (end <= nPref) return false;
    strBody = strRaw.Mid(nPref, end - nPref);
    strBody.Trim();
    strSuffix = (end + 1 < strRaw.GetLength()) ? strRaw.Mid(end + 1)
                                               : CDuiString(L"");
    return true;
}

bool CCleanEngine::ResolveExcludedPattern(const CDuiString& strRaw,
                                          const CDuiString& strRoot,
                                          std::vector<CDuiString>& outPatterns)
{
    CDuiString strBody, strSuffix;

    // ---------------- ?GetFileVersion(file) ----------------
    if (SplitFuncCall(strRaw, L"?GetFileVersion(", strBody, strSuffix))
    {
        CDuiString strFile = strBody;
        if (strFile.IsEmpty()) return true;

        // 将 %Root% 替换为已解析的根目录，注意补足根目录尾部反斜杠
        CDuiString strToken(L"%Root%");
        int rp = 0;
        while ((rp = strFile.Find(strToken.GetData(), rp)) >= 0)
        {
            CDuiString strBase = strRoot;
            if (!strBase.IsEmpty()
                && strBase[strBase.GetLength() - 1] != L'\\')
                strBase += L'\\';
            strFile = strFile.Mid(0, rp) + strBase
                    + strFile.Mid(rp + strToken.GetLength());
            rp += static_cast<int>(strBase.GetLength());
        }

        DWORD dwHandle = 0;
        DWORD dwSize = ::GetFileVersionInfoSize(strFile.GetData(), &dwHandle);
        if (dwSize == 0) return true;   // 文件不存在：无版本目录可排除
        std::vector<BYTE> buf(dwSize);
        if (!::GetFileVersionInfo(strFile.GetData(), 0, dwSize, buf.data()))
            return true;

        VS_FIXEDFILEINFO* pffi = nullptr;
        UINT uiLen = 0;
        if (!::VerQueryValue(buf.data(), L"\\", (void**)&pffi, &uiLen)
            || !pffi)
            return true;

        CDuiString s4, s2;
        s4.Format(L"%u.%u.%u.%u",
                  HIWORD(pffi->dwFileVersionMS), LOWORD(pffi->dwFileVersionMS),
                  HIWORD(pffi->dwFileVersionLS), LOWORD(pffi->dwFileVersionLS));
        // 个别规则只写了三段（Query 为 *.*.*），同时给“主.次”短模式兜底
        s2.Format(L"%u.%u",
                  HIWORD(pffi->dwFileVersionMS), LOWORD(pffi->dwFileVersionMS));

        AddUnique(outPatterns, s4 + strSuffix);
        AddUnique(outPatterns, s2 + strSuffix);
        return true;
    }

    // ---------------- ?GetRegSz(key,value[,wow]) ----------------
    if (SplitFuncCall(strRaw, L"?GetRegSz(", strBody, strSuffix))
    {
        // 复用根路径的注册表解析（已支持第三参数 True/False 视图提示）
        CDuiString strVal = ResolveRootPath(
            CDuiString(L"?GetRegSz(") + strBody + L")");
        strVal.Trim();
        if (!strVal.IsEmpty())
            AddUnique(outPatterns, strVal + strSuffix);
        // 读不到值（软件未装）时不产生排除模式，返回 true 表示已正确处理
        return true;
    }

    // ---------------- ?GetIniData(file,section,key) ----------------
    if (SplitFuncCall(strRaw, L"?GetIniData(", strBody, strSuffix))
    {
        // 文件名/节名/键名按逗号切分（文件名中允许出现 %Root%）
        int c1 = strBody.Find(L',');
        if (c1 <= 0) return true;
        int c2 = strBody.Find(L',', c1 + 1);
        if (c2 <= c1) return true;

        CDuiString strFile = strBody.Mid(0, c1);
        CDuiString strSec  = strBody.Mid(c1 + 1, c2 - c1 - 1);
        CDuiString strKey  = strBody.Mid(c2 + 1);
        strFile.Trim(); strSec.Trim(); strKey.Trim();

        CDuiString strToken(L"%Root%");
        int rp = 0;
        while ((rp = strFile.Find(strToken.GetData(), rp)) >= 0)
        {
            CDuiString strBase = strRoot;
            if (!strBase.IsEmpty()
                && strBase[strBase.GetLength() - 1] != L'\\')
                strBase += L'\\';
            strFile = strFile.Mid(0, rp) + strBase
                    + strFile.Mid(rp + strToken.GetLength());
            rp += static_cast<int>(strBase.GetLength());
        }

        wchar_t szFile[MAX_PATH * 2] = { 0 };
        ::ExpandEnvironmentStrings(strFile.GetData(), szFile, _countof(szFile));

        constexpr int kIniValWchars = 256;   // 256 缓冲尺寸（语义命名，数值不变）
        wchar_t szVal[kIniValWchars] = { 0 };
        ::GetPrivateProfileString(strSec.GetData(), strKey.GetData(),
                                  L"", szVal, _countof(szVal), szFile);
        CDuiString sVal(szVal);
        sVal.Trim();
        if (!sVal.IsEmpty())
        {
            AddUnique(outPatterns, sVal + strSuffix);
            // 版本值可能只有三段，补“主.次”短模式兜底
            int dot1 = sVal.Find(L'.');
            int dot2 = (dot1 >= 0) ? sVal.Find(L'.', dot1 + 1) : -1;
            if (dot1 > 0 && dot2 > dot1)
                AddUnique(outPatterns, sVal.Mid(0, dot2) + strSuffix);
        }
        return true;
    }

    // ---------------- ?GetLocaleName() ----------------
    if (SplitFuncCall(strRaw, L"?GetLocaleName(", strBody, strSuffix))
    {
        WCHAR szLoc[LOCALE_NAME_MAX_LENGTH] = { 0 };
        if (::GetUserDefaultLocaleName(szLoc, _countof(szLoc)) > 0)
            AddUnique(outPatterns, CDuiString(szLoc) + strSuffix);
        return true;
    }

    // 未知函数式排除：无法保证排除语义，交给调用方 fail-closed
    return false;
}

// ============================================================
// Excluded 匹配（按文件名部分，PathMatchSpec 不区分大小写）
// ============================================================
bool CCleanEngine::MatchExcluded(const std::vector<CDuiString>& patterns,
                                 const CDuiString& strPath)
{
    if (patterns.empty()) return false;
    const wchar_t* pszName = ::PathFindFileName(strPath.GetData());
    if (!pszName || !*pszName) return false;
    for (size_t i = 0; i < patterns.size(); ++i)
    {
        if (::PathMatchSpec(pszName, patterns[i].GetData()))
            return true;
    }
    return false;
}

// ============================================================
// glob enumeration
// ============================================================
// [N16] T2 抽纯函数：glob 单条目接受判定（原 GlobPaths do 循环内 5 层 if 内联）
// [N16-fix] nWant 用整型传参：CCleanEngine::GlobMatch 是类内 private enum，
// 文件级纯函数无法按作用域限定名引用（C2248）。整型值与类内定义严格对齐：
//   GLOB_BOTH=0 / GLOB_FILE=1 / GLOB_DIR=2（见 CleanEngine.h:166）。
static bool CleGlobEntryOk(const std::vector<CDuiString>& segs, size_t level,
                           const WIN32_FIND_DATA& fd, bool bSegWild, bool bLast,
                           int nWant)
{
    // FindFirstFile("dir\\*") 恒含 "."/".."：任何层级都不是合法目标
    if (lstrcmpW(fd.cFileName, L".") == 0
        || lstrcmpW(fd.cFileName, L"..") == 0)
        return false;
    // 精确段：只接受同名条目，杜绝 FindNextFile 越权枚举兄弟项
    if (!bSegWild)
    {
        bool bNameEq = (lstrcmpiW(fd.cFileName, segs[level].GetData()) == 0);
        if (!bNameEq && fd.cAlternateFileName[0] != L'\0')
            bNameEq = (lstrcmpiW(fd.cAlternateFileName, segs[level].GetData()) == 0);
        if (!bNameEq) return false;
    }
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return false;
    bool bDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (bLast)
    {
        if (nWant == 1) return !bDir;   // GLOB_FILE
        if (nWant == 2) return bDir;    // GLOB_DIR
        return true;                    // GLOB_BOTH
    }
    return bDir;   // 中间级必须为目录
}

void CCleanEngine::GlobPaths(const CDuiString& strPattern, GlobMatch nWant,
                             std::vector<CDuiString>& out)
{
    CDuiString pattern(strPattern);

    // split drive prefix
    CDuiString strBase;
    std::vector<CDuiString> segs;

    if (pattern.GetLength() >= 2 && pattern[1] == L':')
    {
        strBase = pattern.Mid(0, 2) + L"\\";
        CDuiString rest = pattern.Mid(3);
        CDuiString tmp;
        for (int i = 0; i <= rest.GetLength(); ++i)
        {
            if (i == rest.GetLength() || rest[i] == L'\\')
            {
                tmp.Trim();
                if (!tmp.IsEmpty()) segs.push_back(tmp);
                tmp.Empty();
            }
            else tmp += rest[i];
        }
    }
    else
    {
        // relative / drive wildcard: enumerate fixed drives for leading '*'
        std::vector<CDuiString> all;
        CDuiString rest = pattern;
        CDuiString tmp;
        for (int i = 0; i <= rest.GetLength(); ++i)
        {
            if (i == rest.GetLength() || rest[i] == L'\\')
            {
                tmp.Trim();
                if (!tmp.IsEmpty()) all.push_back(tmp);
                tmp.Empty();
            }
            else tmp += rest[i];
        }
        if (!all.empty() && all[0] == L"*")
        {
            DWORD drives = ::GetLogicalDrives();
            for (int d = 0; d < 26; ++d)
            {
                if (!(drives & (1 << d))) continue;
                std::wstring sRoot;
                sRoot.push_back(static_cast<wchar_t>(L'A' + d));
                sRoot.push_back(L':');
                sRoot.push_back(L'\\');
                if (::GetDriveType(sRoot.c_str()) != DRIVE_FIXED) continue;
                CDuiString sub; sub = sRoot.c_str();
                for (size_t k = 1; k < all.size(); ++k)
                {
                    sub += all[k];
                    if (k + 1 < all.size()) sub += L"\\";
                }
                GlobPaths(sub, nWant, out);
            }
        }
        return;
    }

    if (segs.empty()) return;

    struct GlobCtx
    {
        std::vector<CDuiString> segs;
        GlobMatch nWant;
        std::vector<CDuiString>* out;
    };
    GlobCtx ctx;
    ctx.segs = segs;
    ctx.nWant = nWant;
    ctx.out = &out;

    // iterative BFS
    std::vector<CDuiString> current;
    current.push_back(strBase);

    for (size_t level = 0; level < ctx.segs.size(); ++level)
    {
        bool bLast = (level + 1 == ctx.segs.size());
        std::vector<CDuiString> next;

        for (size_t c = 0; c < current.size(); ++c)
        {
            CDuiString spec = current[c];
            if (spec[spec.GetLength() - 1] != L'\\') spec += L'\\';
            spec += ctx.segs[level];

            // [R-EXTASYNC/BUGFIX] 二次名称过滤：FindFirstFile 对"无通配符的
            // 精确末段"在父路径本身不含通配符时，FindNextFile 会继续返回
            // 兄弟条目（实测 pattern "...\dirs\sub1" 同时回传 sub2）；
            // 同时显式过滤也让 8.3 短名/枚举顺序差异不再影响结果。
            bool bSegWild = ctx.segs[level].Find(L'*') >= 0
                         || ctx.segs[level].Find(L'?') >= 0;

            WIN32_FIND_DATA fd;
            CLEFindGuard hFind(::FindFirstFile(spec.GetData(), &fd));
            if (hFind == INVALID_HANDLE_VALUE) continue;
            do
            {
                if (!CleGlobEntryOk(ctx.segs, level, fd, bSegWild, bLast, static_cast<int>(ctx.nWant))) continue;
                CDuiString full = current[c];
                if (full[full.GetLength() - 1] != L'\\') full += L'\\';
                full += fd.cFileName;
                AddUnique(next, full);
            } while (::FindNextFile(hFind, &fd));
        }
        current = next;
        if (current.empty()) break;
    }

    for (size_t i = 0; i < current.size(); ++i)
        AddUnique(*ctx.out, current[i]);
}

// ============================================================
// size helpers
// ============================================================
// C-8 硬链接去重：FILE_ID_INFO = { ULONGLONG VolumeSerialNumber; FILE_ID_128 FileId; }
// 其中 FILE_ID_128 = { BYTE Identifier[16]; }
// hash/eq 基于 VolumeSerialNumber XOR 全部 16 字节 FileId.Identifier。
struct FileIdHash
{
    size_t operator()(const FILE_ID_INFO& f) const
    {
        size_t h = std::hash<ULONGLONG>()(f.VolumeSerialNumber);
        for (int i = 0; i < 16; ++i)
        {
            h ^= std::hash<BYTE>()(f.FileId.Identifier[i]);
            h *= 16777619u;   // FNV-1a offset basis 变体，减少 hash 冲突
        }
        return h;
    }
};
struct FileIdEq
{
    bool operator()(const FILE_ID_INFO& a, const FILE_ID_INFO& b) const
    {
        if (a.VolumeSerialNumber != b.VolumeSerialNumber) return false;
        for (int i = 0; i < 16; ++i)
            if (a.FileId.Identifier[i] != b.FileId.Identifier[i]) return false;
        return true;
    }
};

// C-8 去重集（文件级 static，供 PathSize 与 PathSizeResetDedup 共享）
// [FIX-P2E] 加文件级互斥：正确性原仅靠 CleanJobService::IsBusy 的调用方
// 约定（扫描/清理线程与 UI 线程串行），服务层无强制。加锁后即使将来
// 出现并发计数路径也不会踩坏 unordered_set（锁竞争可忽略：作业串行）。
static std::unordered_set<FILE_ID_INFO, FileIdHash, FileIdEq> s_staticPathSizeSeenIds;
static CRITICAL_SECTION* PathSizeDedupLock()
{
    static CRITICAL_SECTION cs;
    static struct SInit { SInit() { ::InitializeCriticalSection(&cs); } } s_init;
    return &cs;
}

// C-8: 硬链接去重集重置入口。ScanRule / DeleteRules 在开始计数前调用，
// 确保每个"计数周期"从零开始累计，避免上一次调用残留的 FILE_ID_INFO
// 让本次把真实文件误判为重复。
static void PathSizeResetDedup()
{
    ::EnterCriticalSection(PathSizeDedupLock());
    s_staticPathSizeSeenIds.clear();
    ::LeaveCriticalSection(PathSizeDedupLock());
}

ULONGLONG CCleanEngine::PathSize(const CDuiString& strPath, bool bDir)
{
    // C-8: 硬链接去重。同一 FILE_ID_INFO (VolumeSerialNumber + FileIndex + FileIndexHigh)
    // 在整个扫描中只累计一次，避免同一物理内容被多个硬链接路径重复计数，
    // 造成"可释放空间"虚高。reparse point 已由下方 FILE_ATTRIBUTE_REPARSE_POINT
    // 分支跳过，因此 dedup set 只装真实文件（含目录节点，目录 ID 全局唯一，不产生影响）。
    //
    // set 生命周期策略：
    //   1) 顶层调用 (bDir==false, 单文件) 不登记、不去重：单文件直接返回大小，
    //      避免污染后续顶层规则/路径的独立计数。
    //   2) 递归子文件调用 (bDir==false, 来自目录 FindFirstFile 循环) 会登记。
    //      static set 保证一次完整扫描内去重；下一次规则扫描会自然增长，
    //      但硬链接的重复内容仍然被抑制 —— 这就是我们想要的效果。
    //   3) 查询失败 (权限/网络等) 时回退为路径名计数（保守，可能重复但不错报）。
    // set 本体为文件级 s_staticPathSizeSeenIds，由 PathSizeResetDedup() 清空。

    if (!bDir)
    {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (!::GetFileAttributesEx(strPath.GetData(), GetFileExInfoStandard, &fad))
            return 0;
        ULARGE_INTEGER ul;
        ul.HighPart = fad.nFileSizeHigh;
        ul.LowPart  = fad.nFileSizeLow;
        ULONGLONG sz = ul.QuadPart;
        if (sz == 0) return 0;

        CLEHandleGuard hFile = CLEHandleGuard(::CreateFileW(strPath.GetData(), FILE_READ_ATTRIBUTES,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, 0, nullptr));
        if (hFile != INVALID_HANDLE_VALUE)
        {
            FILE_ID_INFO fid = { 0 };
            BOOL ok = ::GetFileInformationByHandleEx(hFile, FileIdInfo, &fid, sizeof(fid));
            if (ok)
            {
                // [FIX-P2E] 去重集读写持锁（见 s_staticPathSizeSeenIds 处说明）
                ::EnterCriticalSection(PathSizeDedupLock());
                const bool bSeen = s_staticPathSizeSeenIds.find(fid) != s_staticPathSizeSeenIds.end();
                if (!bSeen) s_staticPathSizeSeenIds.insert(fid);
                ::LeaveCriticalSection(PathSizeDedupLock());
                if (bSeen)
                    return 0;               // 硬链接已被计入，本次返回 0
            }
            // [R-B38] ADS（NTFS 备选数据流）真实计入可释放空间：FindFirstStreamW 原语
            sz += CleanNativePrimitives::NCQueryAdsSize(strPath);
            // 查询失败：退化为按路径名计数（保守，可能重复但不错报）
            return sz;
        }
        return sz;
    }

    // [R-ScanResilience] 空路径防御：spec[GetLength()-1] 在空串时为
    // spec[-1] 越界读（0xC0000005 来源之一）；早期返回与上层
    // GetFileAttributes 失败计数语义一致（无目标、无大小）。
    if (strPath.IsEmpty()) return 0;

    ULONGLONG ullTotal = 0;
    CDuiString spec = strPath;
    if (spec[spec.GetLength() - 1] != L'\\') spec += L'\\';
    spec += L"*";

    WIN32_FIND_DATA fd;
    CLEFindGuard hFind(::FindFirstFile(spec.GetData(), &fd));
    if (hFind == INVALID_HANDLE_VALUE) return 0;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        bool bChildDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (bChildDir
            && (lstrcmpiW(fd.cFileName, L".") == 0
                || lstrcmpiW(fd.cFileName, L"..") == 0))
            continue;

        CDuiString child = strPath;
        if (child[child.GetLength() - 1] != L'\\') child += L'\\';
        child += fd.cFileName;
        ullTotal += PathSize(child, bChildDir);
    } while (::FindNextFile(hFind, &fd));
    return ullTotal;
}

bool CCleanEngine::ExistsPath(const CDuiString& strPath)
{
    return ::GetFileAttributes(strPath.GetData()) != INVALID_FILE_ATTRIBUTES;
}

void CCleanEngine::AddUnique(std::vector<CDuiString>& out, const CDuiString& s)
{
    for (size_t i = 0; i < out.size(); ++i)
        if (out[i].Compare(s) == 0) return;
    out.push_back(s);
}

// ============================================================
// deletion (permanent; P2-16: 不再进回收站，直接释放磁盘空间)
// ============================================================
// [R17 P1] \\?\ 宽路径递归强删：处理 SHFileOperation 无法应对的
// 超长路径（>MAX_PATH）。reparse point 不穿越（与扫描剪枝一致）。
// 返回 true 表示路径最终不存在（删除成功或本来就没有）。
    // [C++17 R3] [[nodiscard]]：返回值勿忽略
static bool ForceDeleteLongPath(const CDuiString& sRawPath)
{
    CDuiString sPath(sRawPath);
    while (sPath.GetLength() > 3 && sPath[sPath.GetLength() - 1] == L'\\')
        sPath = sPath.Left(sPath.GetLength() - 1);   // Left 生成独立新串，避免 Assign 自截断时的自赋值风险
    if (sPath.IsEmpty()) return true;

    DWORD attr = ::GetFileAttributes(sPath.GetData());
    if (attr == INVALID_FILE_ATTRIBUTES) return true;   // 已不存在

    // 构造 \\?\ 前缀绝对路径（不做环境变量/相对路径转换，输入均为绝对路径）
    CDuiString sLong;
    if (_wcsnicmp(sPath.GetData(), L"\\\\?\\", 4) == 0)
        sLong = sPath;
    else
        sLong = CDuiString(L"\\\\?\\") + sPath;

    if (!(attr & FILE_ATTRIBUTE_DIRECTORY))
    {
        ::SetFileAttributesW(sLong.GetData(), FILE_ATTRIBUTE_NORMAL);
        if (::DeleteFileW(sLong.GetData())) return true;
        // [R-B38] 原语 2：SetFileInformationByHandle(FileDispositionInfo) 强删
        if (CleanNativePrimitives::NCForceDeleteByDisposition(sPath))
            return true;
        // [R-B38] 原语 1：MoveFileExW(MOVEFILE_DELAY_UNTIL_REBOOT) 重启后删除
        if (CleanNativePrimitives::NCPendingRebootDelete(sPath))
            return true;
        return ::GetFileAttributes(sPath.GetData()) == INVALID_FILE_ATTRIBUTES;
    }

    // 目录：先递归清空内容
    CDuiString sFind = sLong + L"\\*";
    WIN32_FIND_DATAW fd;
    CLEFindGuard h(::FindFirstFileW(sFind.GetData(), &fd));
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (lstrcmpW(fd.cFileName, L".") == 0
                || lstrcmpW(fd.cFileName, L"..") == 0) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;

            CDuiString sChild = sPath + L"\\" + fd.cFileName;
            ForceDeleteLongPath(sChild);
        } while (::FindNextFileW(h, &fd));
    }

    ::SetFileAttributesW(sLong.GetData(), FILE_ATTRIBUTE_DIRECTORY);
    if (::RemoveDirectoryW(sLong.GetData())) return true;
    // [FIX-DEL-2] 非空/被独占目录：逐子项 + 目录级登记"重启后删除"。
    // 根因：SoftwareDistribution\DataStore（wuauserv 独占）/ CbsTemp（CBS
    // 事务）/ WinSAT 等目标的子文件 DeleteFileW/FileDisposition 会因共享
    // 冲突失败，目录 RemoveDirectory 因此必失败 —— 旧实现直接记失败。
    // 而 MoveFileEx(DELAY_UNTIL_REBOOT) 只写 PendingFileRenameOperations、
    // 不需要数据句柄，登记成功率高；smss 在重启早期递归删除。
    // 登记成功按"将消失"处理（调用方不计失败），并留日志供明细区分。
    {
        WIN32_FIND_DATAW fd2;
        CLEFindGuard h2(::FindFirstFileW(CDuiString(sLong + L"\\*").GetData(), &fd2));
        if (h2 != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (lstrcmpW(fd2.cFileName, L".") == 0
                    || lstrcmpW(fd2.cFileName, L"..") == 0) continue;
                CDuiString sSub2 = sPath + L"\\" + fd2.cFileName;
                CleanNativePrimitives::NCPendingRebootDelete(sSub2);
            } while (::FindNextFileW(h2, &fd2));
        }
        if (CleanNativePrimitives::NCPendingRebootDelete(sPath))
            return true;   // 已登记重启删除：重启后由 smss 递归清除
    }
    return ::GetFileAttributes(sPath.GetData()) == INVALID_FILE_ATTRIBUTES;
}

// [FIX-DEL-1] 服务感知删除：命中已知"被服务独占"映射才动服务。
//   SoftwareDistribution\DataStore|Download → wuauserv/bits/UsoSvc/DoSvc
//   （Windows 更新安装记录/下载缓存清理的标准姿势：停服务 → 删 → 恢复；
//    未命中映射绝不碰服务，winevt\Logs 等 EventLog 持有的目标走
//    PendingReboot 兜底而非停审计服务）。
// 返回 true 表示已执行"停服 → 强删 → 恢复"流程（调用方再验存在性）。
// 仅在 DeleteRules 工作线程调用（CleanJobService::IsBusy 串行保证）。
static bool DelTryServiceAware(const CDuiString& sPath)
{
    static const wchar_t* const kKeys[] = {
        L"\\softwaredistribution\\datastore",
        L"\\softwaredistribution\\download",
    };
    CDuiString sLower = sPath.GetData();
    sLower.MakeLower();
    bool bHit = false;
    for (const wchar_t* k : kKeys)
    {
        if (sLower.Find(k) >= 0) { bHit = true; break; }
    }
    if (!bHit) return false;

    // 停更新服务族（wuauserv 停止时 bits/UsoSvc/DoSvc 多数随之停止，
    // 仍逐个显式停以保证 DataStore.edb 句柄释放）
    CleanNativePrimitives::NCStopService(L"wuauserv");
    CleanNativePrimitives::NCStopService(L"bits");
    CleanNativePrimitives::NCStopService(L"UsoSvc");
    CleanNativePrimitives::NCStopService(L"DoSvc");
    ForceDeleteLongPath(sPath);
    // 恢复服务族（按依赖序：wuauserv 最后）
    CleanNativePrimitives::NCStartService(L"DoSvc");
    CleanNativePrimitives::NCStartService(L"UsoSvc");
    CleanNativePrimitives::NCStartService(L"bits");
    CleanNativePrimitives::NCStartService(L"wuauserv");
    return true;
}

ULONGLONG CCleanEngine::DeleteRules(const std::vector<int>& rules, int& nFailedPaths,
                                    bool bToRecycleBin,
                                    std::vector<CDuiString>* pOutDeleted,
                                    const std::function<void(size_t, size_t)>* pProgress)
{
    nFailedPaths = 0;

    // [R25] SHFileOperation 批次级失败统计：非 0 返回或用户中止都记一次，
    // 最终折算进 nFailedPaths，避免"调用失败但界面显示全部成功"。
    int    nShellFailBatches = 0;
    DWORD  dwLastShellErr = 0;

    if (pOutDeleted) pOutDeleted->clear();

    // [修复 #19] 1) 路径级去重：同一路径被多条规则命中只算一次。
    struct DelPath
    {
        CDuiString sPath;
        bool bDir;
        std::basic_string<wchar_t> sNorm;  // 小写、去尾反斜杠，用于排序/父子判定
    };
    std::vector<DelPath> vAll;
    for (size_t r = 0; r < rules.size(); ++r)
    {
        int i = rules[r];
        if (i < 0 || i >= static_cast<int>(m_rules.size())) continue;
        for (size_t t = 0; t < m_rules[i].targets.size(); ++t)
        {
            const CleanTarget& tgt = m_rules[i].targets[t];
            bool bDup = false;
            for (size_t q = 0; q < vAll.size(); ++q)
                if (vAll[q].sPath.Compare(tgt.sPath) == 0) { bDup = true; break; }
            if (bDup) continue;

            DelPath dp;
            dp.sPath = tgt.sPath;
            dp.bDir = tgt.bDir;
            dp.sNorm = tgt.sPath.GetData();
            for (size_t k = 0; k < dp.sNorm.size(); ++k)
                dp.sNorm[k] = static_cast<wchar_t>(std::towlower(dp.sNorm[k]));
            while (dp.sNorm.size() > 3 && dp.sNorm[dp.sNorm.size() - 1] == L'\\')
                dp.sNorm.erase(dp.sNorm.size() - 1);
            vAll.push_back(dp);
        }
    }

    // [修复 #19] 2) 父目录排在子路径前面：同一批 SHFileOperation 中先删父，
    // 避免后删父时因子已不存在而产生（个别 Shell 版本上的）失败计数虚高。
    //    规范化串的字典序天然保证“前缀(父)”在“子”之前。
    std::sort(vAll.begin(), vAll.end(),
              [](const DelPath& a, const DelPath& b) { return a.sNorm < b.sNorm; });

    // [修复 #19] 3) 大小去重：被保留路径（祖先目录）覆盖的子路径不再累加，
    //    此前按规则 ullSize 求和，父子规则重叠会把同一份字节数算两遍。
    std::vector<bool> vCovered(vAll.size(), false);
    for (size_t i = 0; i < vAll.size(); ++i)
    {
        for (size_t j = 0; j < i; ++j)
        {
            if (vCovered[j]) continue;
            const std::basic_string<wchar_t>& sParent = vAll[j].sNorm;
            const std::basic_string<wchar_t>& sChild = vAll[i].sNorm;
            if (sChild.size() > sParent.size()
                && sChild.compare(0, sParent.size(), sParent) == 0
                && sChild[sParent.size()] == L'\\')
            {
                vCovered[i] = true;
                break;
            }
        }
    }

    // [AUDIT-F5] allPaths 提前构建：删除阶段进度回调需要以路径总数为分母，
    // 让「删除前计量（PathSize 逐路径重走磁盘）」期间进度条就从重估阶段的
    // 100% 归零为"删除中 0%"——原实现在首批 SHFileOperation 完成前没有任何
    // 回投，大目录计量数十秒到数分钟里进度条纹丝不动，被用户误读为冻结。
    std::vector<CDuiString> allPaths;
    allPaths.reserve(vAll.size());
    for (size_t i = 0; i < vAll.size(); ++i)
        allPaths.push_back(vAll[i].sPath);
    if (pProgress && *pProgress)
    {
        try { (*pProgress)(0, allPaths.size()); } catch (...) {}
    }

    // 删除前按实际保留路径统计（此时文件都还在）
    // C-8: 先重置硬链接去重集，避免沿用扫描阶段状态导致计数错误
    PathSizeResetDedup();
    ULONGLONG ullFreed = 0;
    for (size_t i = 0; i < vAll.size(); ++i)
        if (!vCovered[i])
            ullFreed += PathSize(vAll[i].sPath, vAll[i].bDir);

    constexpr int BATCH = 16000;
    size_t pos = 0;
    while (pos < allPaths.size())
    {
        std::vector<wchar_t> buf;
        buf.reserve(BATCH);
        size_t start = pos;

        while (pos < allPaths.size())
        {
            size_t need = wcslen(allPaths[pos].GetData()) + 1;
            if (buf.size() + need + 1 > static_cast<size_t>(BATCH) && pos > start) break;
            const wchar_t* p = allPaths[pos].GetData();
            while (*p) buf.push_back(*p++);
            buf.push_back(0);
            ++pos;
        }
        buf.push_back(0);   // double-null terminator

        SHFILEOPSTRUCT fos = { 0 };
        fos.wFunc = FO_DELETE;
        fos.pFrom = buf.data();
        // [R88] bToRecycleBin=true 走回收站（FOF_ALLOWUNDO），
        //       支持"撤销"把主规则清理项还原；false 保持 R5/P2-16 直删语义。
        if (bToRecycleBin)
            fos.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION
                       | FOF_NOERRORUI | FOF_SILENT;
        else
            // [修复 R5 / P2-16] 去掉 FOF_ALLOWUNDO：垃圾清理直删，不进回收站
            fos.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI
                       | FOF_SILENT;
        // [R25] 不再忽略返回值：SHFileOperation 返回非 0 时把错误码累计到
        // 批量失败计数，避免"调用静默失败、日志却看不出原因"。
        // fAnyOperationsAborted 也要区分：用户/策略中止 ≠ 文件被占用。
        const int nShRc = ::SHFileOperation(&fos);
        if (nShRc != 0 || fos.fAnyOperationsAborted)
        {
            ++nShellFailBatches;
            if (nShRc != 0) dwLastShellErr = static_cast<DWORD>(nShRc);
        }
        // [R-CLEANPROG] 批次完成 → 回报进度（pos=已处理路径数）。
        // 回调内部若抛异常不得穿透删除流程，吞掉即可（进度尽力而为）。
        if (pProgress && *pProgress)
        {
            try { (*pProgress)(pos, allPaths.size()); } catch (...) {}
        }
    }

    // [R88] 回传实际尝试删除的路径（供 CleanJobService 带回 UI 推入撤销栈）
    if (pOutDeleted)
        *pOutDeleted = allPaths;

    // post-op check + [FIX-RC-1] 回收站失败自动回退强制删除
    // 根因：SHFileOperation 进回收站可能因多种原因静默失败
    // （回收站配额满 / 服务未启动 / 超长路径 / 文件被占用 / 权限不足）。
    // 原 FIX-P1 禁止静默降级，但用户真实意图是"清理掉"而非"放进回收站"。
    // 新语义：回收站成功（路径消失）→ 进撤销栈；
    //         回收站失败 → 自动回退 ForceDeleteLongPath（含只读属性清除 +
    //         MoveFileEx 重启删兜底），确保清理真正落实；
    //         强制删也失败 → 才算真失败 nFailedPaths++。
    // 撤销栈只登记"确实进了回收站"的路径，回退强删的不登记（不可撤销）。
    std::vector<CDuiString> recycled;
    recycled.reserve(allPaths.size());
    for (size_t i = 0; i < allPaths.size(); ++i)
    {
        if (!ExistsPath(allPaths[i]))
        {
            // 路径已消失：SHFileOperation 删除成功
            if (bToRecycleBin) recycled.push_back(allPaths[i]);
            continue;
        }
        // [AUDIT-F5] 仅对「仍存在、需要走强删兜底链」的路径回投进度：
        // 强删/停服恢复/重启删除登记单路径可达数秒，此前全程无回投，
        // 批次删除到 100% 后进度条长时间停在满格，被误读为已完成或冻结。
        // 常规已删路径不重投，避免进度条无意义的 100%→0% 回跳。
        if (pProgress && *pProgress)
        {
            try { (*pProgress)(static_cast<size_t>(i) + 1, allPaths.size()); } catch (...) {}
        }
        // --- 回收站失败 / 显式直删 → 强制删除回退链 ---
        // 1) 先清除只读属性（很多清理目标是只读的系统/缓存文件）
        ::SetFileAttributesW(allPaths[i].GetData(), FILE_ATTRIBUTE_NORMAL);
        // 2) ForceDeleteLongPath：\\?\ 长路径 + SetFileInformationByHandle
        //    + MoveFileEx DELETE_ON_CLOSE 重启兜底
        ForceDeleteLongPath(allPaths[i]);
        if (!ExistsPath(allPaths[i]))
        {
            // 回退强删成功：仅对回收站模式记一次日志告知用户
            if (bToRecycleBin)
            {
                wchar_t szDiag[256] = { 0 };
                wfmt::FormatInto(szDiag, _countof(szDiag),
                    L"[clean] recycle-bin delete failed, forced-deleted: %s\r\n",
                    allPaths[i].GetData());
                ::OutputDebugStringW(szDiag);
            }
            continue;   // 真正删掉了，不算失败
        }
        // 3) [FIX-DEL-1] 服务感知重试：命中已知占用映射（SoftwareDistribution
        //    等）→ 停服务 → 强删 → 恢复服务。这是"Windows更新安装记录/下载
        //    缓存"失败的最常见根因（wuauserv 独占 DataStore.edb）。
        if (DelTryServiceAware(allPaths[i]) && !ExistsPath(allPaths[i]))
        {
            wchar_t szDiag[256] = { 0 };
            wfmt::FormatInto(szDiag, _countof(szDiag),
                L"[clean] service-aware delete ok: %s\r\n",
                allPaths[i].GetData());
            ::OutputDebugStringW(szDiag);
            continue;   // 停服后强删成功
        }
        // 4) [FIX-DEL-2] PendingReboot 兜底：登记成功 = 重启后由 smss 递归
        //    删除（DataStore/CbsTemp/WinSAT 等被独占目标的标准归宿）。
        //    登记成功不计入 nFailedPaths —— 与"权限拒绝等真失败"区分，
        //    明细/失败数不再把"待重启删除"误报为失败。
        if (CleanNativePrimitives::NCPendingRebootDelete(allPaths[i]))
        {
            wchar_t szDiag[256] = { 0 };
            wfmt::FormatInto(szDiag, _countof(szDiag),
                L"[clean] pending-reboot delete registered: %s\r\n",
                allPaths[i].GetData());
            ::OutputDebugStringW(szDiag);
            continue;   // 已登记重启删除
        }
        // 5) 全链失败：真删不掉（ACL 拒绝等）
        ++nFailedPaths;
    }

    // [R25] 批次级失败只做诊断输出，不并入 nFailedPaths：
    // 逐路径的存在性检查已经给出最终失败数，再叠加批次计数会重复计。
    if (nShellFailBatches > 0)
    {
        wchar_t szDiag[192] = { 0 };
        wfmt::FormatInto(szDiag, _countof(szDiag),
            L"[clean] SHFileOperation: %d batch(es) failed/aborted, last=0x%08X, recycle=%d\r\n",
            nShellFailBatches, dwLastShellErr, bToRecycleBin ? 1 : 0);
        ::OutputDebugStringW(szDiag);
    }

    // [FIX-P1] 撤销栈只登记确认已进入回收站的对象；直删请求不产生可撤销记录。
    if (pOutDeleted)
        *pOutDeleted = bToRecycleBin ? recycled : std::vector<CDuiString>();
    return ullFreed;
}

// ============================================================
// formatting
// ============================================================
void CCleanEngine::FormatSize(ULONGLONG ullBytes, CDuiString& strOut)
{
    double f = static_cast<double>(ullBytes);
    const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    int u = 0;
    while (f >= 1024.0 && u < 4) { f /= 1024.0; ++u; }

    if (u == 0)
        strOut.Format(L"%d %s", static_cast<int>(f), units[u]);
    else
        strOut.Format(L"%.2f %s", f, units[u]);
}

// [Q9] DefaultFilterList 全局排除表解析（Data.xml <DefaultFilterList>/<Item>）
void CCleanEngine::ParseDefaultFilters()
{
 m_vDefaultFilter.clear();
 if (!m_pEngine || !m_pEngine->IsValid()) return;
 // [B1-RE] DefaultFilterList 是 <Data> 文档根的直接子节点
 // （CleanCollection4 的兄弟）；旧实现从清理集合根下查找，32 项
 // 默认表永远为空。
 CMarkupNode root = m_pEngine->GetDocumentRoot();
 if (!root.IsValid()) return;
 CMarkupNode fl = root.GetChild(L"DefaultFilterList");
 if (!fl.IsValid()) return;
 for (CMarkupNode n = fl.GetChild(); n.IsValid(); n = n.GetSibling())
 {
  const wchar_t* pszVal = n.GetValue();
  if (pszVal && *pszVal) m_vDefaultFilter.push_back(CDuiString(pszVal));
 }
}

// [REFACTOR] 从 ProcessGeneral 提取：收集 General 节点的 Query/Filter 模式。
// 逐字搬迁零逻辑改动。属性 Query 优先，子节点 <Query>/<Filter> 兼容追加。
void CCleanEngine::CollectGeneralQueries(CMarkupNode nodeGeneral,
                                         std::vector<CDuiString>& queries)
{
// [R-EXTASYNC/BUGFIX] 真实 Data.xml 契约（数十条规则实证，自闭合标签）：
//   <General RootPath="..." Query="MCP" Flags="File" />
// Query 是【属性】而非子节点。旧实现只读 <Query> 子元素，属性被静默
// 忽略，模式退化为 "<RootPath>\*"——轻则扫描大小错误（如 %SystemRoot%
// Query=MCP 扫成 Windows 根全部文件，实测异常命中 14 项/3MB），重则
// 勾选清理时删除根目录下全部文件。属性优先；子节点 <Query>/<Filter>
// （开源版契约）兼容追加。
const wchar_t* pszQAttr = nodeGeneral.HasAttribute(L"Query")
    ? nodeGeneral.GetAttributeValue(L"Query") : nullptr;
if (pszQAttr && *pszQAttr)
{
    CDuiString sQA(pszQAttr);
    sQA.Trim();
    if (!sQA.IsEmpty()) queries.push_back(sQA);
}
CMarkupNode q = nodeGeneral.GetChild(L"Query");
while (q.IsValid())
{
    CDuiString s(q.GetValue());
    s.Trim();
    if (!s.IsEmpty()) AddUnique(queries, s);
    q = q.GetSibling();
}
// 开源版 doGeneral 的子节点 <Filter> 同视作 Query
CMarkupNode f = nodeGeneral.GetChild(L"Filter");
while (f.IsValid())
{
    CDuiString s(f.GetValue());
    s.Trim();
    if (!s.IsEmpty()) AddUnique(queries, s);
    f = f.GetSibling();
}
}

// [REFACTOR] 从 ProcessGeneral 提取：收集 Excluded 子节点，函数式排除展开为通配模式。
// 返回 false 表示存在无法解析的函数式排除（调用方据此 fail-closed 整组放弃）。
bool CCleanEngine::CollectGeneralExcludes(CMarkupNode nodeGeneral, const CDuiString& strRoot,
                                          std::vector<CDuiString>& exclPatterns)
{
    bool bExclResolved = true;
CMarkupNode ex = nodeGeneral.GetChild(L"Excluded");
while (ex.IsValid())
{
    CDuiString s(ex.GetValue());
    s.Trim();
    if (!s.IsEmpty())
    {
        if (s[0] == L'?')
        {
            if (!ResolveExcludedPattern(s, strRoot, exclPatterns))
                bExclResolved = false;   // 未知函数式排除
        }
        else
            AddUnique(exclPatterns, s);
    }
    ex = ex.GetSibling();
}
    return bExclResolved;
}
