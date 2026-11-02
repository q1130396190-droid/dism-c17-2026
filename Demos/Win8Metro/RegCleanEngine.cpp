// ============================================================
// RegCleanEngine.cpp —— [R79] 注册表清理引擎实现
// 规则集加载 / 扫描 / 清理 / 备份四件套。
// 复用 CDataEngine 的 ParseRegPath / OpenRegKey / RegClean* facade，
// 不复制注册表实现。纯 Service 层，不使用 UIlib 控件。
//
// [R10-02] CDataEngine:: 静态直调保留理由（11 处均在此，供门禁审计）：
//   * RegClean* facade（RegCleanDelete/Write/Move/ValueExists/OpenKey，
//     RegCleanEngine.cpp:286/566/688/766/770/776/866/872/1202/1211）：
//     R79 已审定的注册表操作单一出口。这些 API 均为静态纯函数，不依赖
//     实例状态；RegCleanEngine 侧若改走实例门面（m_pEngine->Xxx）只能
//     得到同名转发，且 ExecAction/ActionTargetExists/ReadValueSnapshot/
//     ImportRegFile 是 static（被 CleanRules/ScanRules/RestoreBackup 的
//     静态调用链复用），本轮 CP-4 只做"外部静态直调"收口，方法签名与
//     静态性调整属 R11+ 函数级拆分范畴。保留静态名可避免与 Page 之间
//     引入第二层依赖反转。
//   * 其余 73 处外部直调已全部收口：ResolveText/ScopeName 提为非类
//     纯函数（DismResolveText/DismScopeName），离线蜂巢会话抽为
//     COfflineSession 独立组件（OfflineSession.h）。
// ============================================================

#include "stdafx.h"
#include <set>
#include <algorithm>   // [C++17 R-P16] std::fill

#include "RegCleanEngine.h"


#include "DataEngine.h"

#include <shlwapi.h>
#include <ctime>
#include <string>
#include <vector>
// [C++17 阶段2] 局部 RAII 守卫：文件句柄、注册表键（见 WinHandles.h 使用规范）
struct RCRegGuard
{
    HKEY k = nullptr;
    ~RCRegGuard() { if (k) ::RegCloseKey(k); }
    void reset(HKEY n = nullptr) { if (k) ::RegCloseKey(k); k = n; }
    HKEY* out() { return &k; }
    operator HKEY() const { return k; }
    explicit operator bool() const { return k != nullptr; }
};

struct RCFileGuard
{
    HANDLE h;
    explicit RCFileGuard(HANDLE x) : h(x) {}
    ~RCFileGuard() { if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h); }
    RCFileGuard(const RCFileGuard&) = delete;
    RCFileGuard& operator=(const RCFileGuard&) = delete;
    operator HANDLE() const { return h; }
};


#pragma comment(lib, "shlwapi.lib")

using namespace DuiLib;
using CDataEngine = CDataEngine;

namespace
{
    // 递归判断某节点子树内是否存在指定名称的节点（用于判定 Item 是否含 Reg 动作）
    bool SubtreeHasNode(CMarkupNode node, const wchar_t* pszName)
    {
        CMarkupNode ch = node.GetChild();
        while (ch.IsValid())
        {
            const wchar_t* pszN = ch.GetName();
            if (pszN && lstrcmpiW(pszN, pszName) == 0) return true;
            if (SubtreeHasNode(ch, pszName)) return true;
            ch = ch.GetSibling();
        }
        return false;
    }

    bool SubtreeHasAnyReg(CMarkupNode node)
    {
        return SubtreeHasNode(node, L"RegDelete")
            || SubtreeHasNode(node, L"RegWrite")
            || SubtreeHasNode(node, L"RegMove");
    }

    bool IsRegNodeName(const wchar_t* psz)
    {
        return psz
            && (lstrcmpiW(psz, L"RegDelete") == 0
             || lstrcmpiW(psz, L"RegWrite") == 0
             || lstrcmpiW(psz, L"RegMove") == 0);
    }
}

// [B3 2026-10-12] 条件门控的 Reg 动作收集（fail-closed）：
//   * 任意容器的直属 <Applicable> 不成立 → 整个子树剪枝（与优化引擎
//     E 同一条件引擎；未知条件标签返回 false）。
//   * <True> 容器永不收集（True 是"开启优化"动作，不是清理/重置动作；
//     注册表清理只处理 False 分支 = 关闭/还原动作）。
//   * <Warning> 不携带动作；<Current>/<System>/<Default>/<False>/<Scan>/
//     <S>/<Activate> 穿透。
void CRegCleanEngine::GatedCollect(CMarkupNode node, CDataEngine* pEng,
                                   std::vector<RegCleanAction>& out)
{
    for (CMarkupNode ch = node.GetChild(); ch.IsValid(); ch = ch.GetSibling())
    {
        const wchar_t* pszName = ch.GetName();
        if (!pszName) continue;

        if (lstrcmpiW(pszName, L"Applicable") == 0)
        {
            // Applicable 作为容器属性出现时由父级调用方处理；走到这里说明
            // 它是某个容器的子门：评估，失败则剪枝当前整棵子树。
            if (pEng && !pEng->EvalConditions(ch)) return;
            continue;
        }
        if (lstrcmpiW(pszName, L"True") == 0)
            continue;   // 开启分支：不参与清理
        if (lstrcmpiW(pszName, L"Warning") == 0)
            continue;

        if (IsRegNodeName(pszName))
        {
            RegCleanAction a;
            if (lstrcmpiW(pszName, L"RegDelete") == 0)
            {
                a.eType = RegCleanActionType::REGCLEAN_ACTION_DELETE;
                a.sKey = ch.GetAttributeValue(L"Key");
                a.sValue = ch.GetAttributeValue(L"Value");
            }
            else if (lstrcmpiW(pszName, L"RegWrite") == 0)
            {
                a.eType = RegCleanActionType::REGCLEAN_ACTION_WRITE;
                a.sKey = ch.GetAttributeValue(L"Key");
                a.sValue = ch.GetAttributeValue(L"Value");
                a.sType = ch.GetAttributeValue(L"Type");
                a.sData = ch.GetAttributeValue(L"data");
                a.nRegType = ParseRegType(a.sType.GetData());
            }
            else
            {
                a.eType = RegCleanActionType::REGCLEAN_ACTION_MOVE;
                a.sKey = ch.GetAttributeValue(L"Key");
                a.sValue = ch.GetAttributeValue(L"Value");
                // [B3] data.xml 实证属性名为 NewKey/NewValue（与
                // D::ExecActivate 一致）；旧实现读 DstKey/
                // DstValue 导致全部 87 个 RegMove 动作目标为空而失效。
                a.sDstKey = ch.GetAttributeValue(L"NewKey");
                a.sDstValue = ch.GetAttributeValue(L"NewValue");
            }
            if (ch.HasAttribute(L"Wow64"))
                a.bWow64 = (lstrcmpiW(ch.GetAttributeValue(L"Wow64"), L"True") == 0);
            if (ch.HasAttribute(L"SkipError"))
                a.bSkipError = (lstrcmpiW(ch.GetAttributeValue(L"SkipError"), L"2") == 0);
            out.push_back(a);
            continue;
        }

        // 其余容器：先检查其直属 Applicable 门，再递归
        CMarkupNode gate = ch.GetChild(L"Applicable");
        if (gate.IsValid() && pEng && !pEng->EvalConditions(gate))
            continue;   // 该分支当前不适用 → 剪枝
        GatedCollect(ch, pEng, out);
    }
}

// 动作去重（同类型/同源/同目标/同视图视为同一条）
static bool SameRegAction(const RegCleanAction& a, const RegCleanAction& b)
{
    if (a.eType != b.eType || a.bWow64 != b.bWow64) return false;
    if (lstrcmpiW(a.sKey.GetData(), b.sKey.GetData()) != 0) return false;
    if (lstrcmpiW(a.sValue.GetData(), b.sValue.GetData()) != 0) return false;
    if (a.eType == RegCleanActionType::REGCLEAN_ACTION_MOVE)
    {
        if (lstrcmpiW(a.sDstKey.GetData(), b.sDstKey.GetData()) != 0) return false;
        if (lstrcmpiW(a.sDstValue.GetData(), b.sDstValue.GetData()) != 0) return false;
    }
    if (a.eType == RegCleanActionType::REGCLEAN_ACTION_WRITE)
    {
        if (lstrcmpiW(a.sType.GetData(), b.sType.GetData()) != 0) return false;
        if (lstrcmpiW(a.sData.GetData(), b.sData.GetData()) != 0) return false;
    }
    return true;
}

CRegCleanEngine::CRegCleanEngine() : m_pEngine(nullptr) {}
CRegCleanEngine::~CRegCleanEngine() {}

// ------------------------------------------------------------
// 1. 规则集加载
// ------------------------------------------------------------
int CRegCleanEngine::LoadRules(CDataEngine* pEngine)
{
    m_rules.clear();
    m_pEngine = pEngine;
    if (!pEngine || !pEngine->IsValid()) return 0;

    CMarkupNode root = pEngine->GetOptimizationRoot();
    if (!root.IsValid()) return 0;

    int nCount = 0;

    // [B3] 注册表清理规则收集（原版语义对齐 + fail-closed 安全门控）：
    //   data.xml 实证（202 个含 Reg 动作的优化项，全部为作用域结构）：
    //     <Item><Applicable/>? <Current|System|Default>
    //        <State><Applicable/></State>
    //        <True><Activate>RegWrite...</Activate></True>
    //        <False><Activate>RegDelete/RegMove/RegWrite(默认值)
    //        </Activate></False>
    //      </Scope></Item>
    //   注册表清理的本质 = 批量重置"当前已生效"的优化残留，因此只收集
    //   False（关闭/还原）分支里的 Reg 动作；True（开启）分支永不收集。
    //   Item 级 <Applicable> 与各容器直属 <Applicable> 必须当下成立，
    //   跨 OS 版本分支的动作绝不执行（未知条件一律 fail-closed）。
    static const wchar_t* s_scopes[] = { L"Current", L"System", L"Default" };

    auto TryAddItem = [&](CMarkupNode& item, const wchar_t* pszGroupName)
    {
        if (!item.IsValid()) return;
        if (lstrcmpiW(item.GetName(), L"Item") != 0) return;

        // Item 级适用门
        CMarkupNode itemApp = item.GetChild(L"Applicable");
        if (itemApp.IsValid() && !pEngine->EvalConditions(itemApp))
            return;

        CDuiString sName = DismResolveText(item.GetAttributeValue(L"Name"));
        if (sName.IsEmpty()) return;
        for (size_t q = 0; q < m_rules.size(); ++q)
            if (lstrcmpiW(m_rules[q].sName.GetData(), sName.GetData()) == 0)
                return;

        RegCleanRule rule;
        rule.sName = sName;
        rule.sGroup = pszGroupName ? DismResolveText(pszGroupName) : L"";
        rule.nLevel = 0;
        {
            const wchar_t* pszLevel = item.GetAttributeValue(L"Level");
            if (pszLevel && *pszLevel) rule.nLevel = _ttoi(pszLevel);
        }
        rule.bApplicable = true;

        bool bHas = false;
        for (size_t s = 0; s < _countof(s_scopes); ++s)
        {
            CMarkupNode scope = item.GetChild(s_scopes[s]);
            if (!scope.IsValid()) continue;

            // 只收 False（还原）分支；无 False 的项对注册表清理无意义
            CMarkupNode fb = scope.GetChild(L"False");
            if (!fb.IsValid()) continue;

            size_t nBefore = rule.actions.size();
            GatedCollect(fb, pEngine, rule.actions);
            if (rule.actions.size() != nBefore) bHas = true;
        }

        // 兼容无作用域的第三方平铺规则：三个作用域都不存在时，门控收集
        // Item 直接子级（G 内部仍会跳过任何 True 分支）
        bool bAnyScope = false;
        for (size_t s = 0; s < _countof(s_scopes); ++s)
            if (item.GetChild(s_scopes[s]).IsValid()) { bAnyScope = true; break; }
        if (!bAnyScope)
        {
            size_t nBefore = rule.actions.size();
            GatedCollect(item, pEngine, rule.actions);
            if (rule.actions.size() != nBefore) bHas = true;
        }

        if (!bHas) return;

        // 动作级去重（不同作用域可能引用同一蜂巢路径）
        for (size_t i = 0; i < rule.actions.size(); ++i)
        {
            for (size_t j = i + 1; j < rule.actions.size();)
            {
                if (SameRegAction(rule.actions[i], rule.actions[j]))
                    rule.actions.erase(rule.actions.begin() + j);
                else
                    ++j;
            }
        }

        m_rules.push_back(rule);
        ++nCount;
    };

    // 1) 直属 Item（平铺布局兼容）
    for (CMarkupNode item = root.GetChild(); item.IsValid(); item = item.GetSibling())
        TryAddItem(item, nullptr);

    // 2) <Group><Item> 真实主布局
    CMarkupNode grp = root.GetChild(L"Group");
    while (grp.IsValid())
    {
        const wchar_t* pszGName = grp.GetAttributeValue(L"Name");
        CMarkupNode item = grp.GetChild(L"Item");
        while (item.IsValid())
        {
            TryAddItem(item, pszGName);
            item = item.GetSibling();
        }
        grp = grp.GetSibling();
    }
    return nCount;
}

DWORD CRegCleanEngine::ParseRegType(const wchar_t* pszType)
{
    if (!pszType || !*pszType) return REG_SZ;
    if (lstrcmpiW(pszType, L"REG_DWORD") == 0) return REG_DWORD;
    if (lstrcmpiW(pszType, L"REG_QWORD") == 0) return REG_QWORD;
    if (lstrcmpiW(pszType, L"REG_SZ") == 0) return REG_SZ;
    if (lstrcmpiW(pszType, L"REG_EXPAND_SZ") == 0) return REG_EXPAND_SZ;
    if (lstrcmpiW(pszType, L"REG_BINARY") == 0) return REG_BINARY;
    if (lstrcmpiW(pszType, L"REG_MULTI_SZ") == 0) return REG_MULTI_SZ;
    return REG_SZ;
}

// ------------------------------------------------------------
// 2. 扫描
// ------------------------------------------------------------
bool CRegCleanEngine::ActionTargetExists(const RegCleanAction& a)
{
    return CDataEngine::RegCleanValueExists(a.sKey.GetData(),
                                             a.sValue.GetData(),
                                             a.bWow64);
}

int CRegCleanEngine::ScanRules(std::vector<RegCleanScanResult>& out,
                               RegCleanScanSummary& summary)
{
    out.clear();
    summary = RegCleanScanSummary();
    summary.nRules = static_cast<int>(m_rules.size());

    for (size_t i = 0; i < m_rules.size(); ++i)
    {
        RegCleanRule& rule = m_rules[i];
        RegCleanScanResult r;
        r.iRule = static_cast<int>(i);
        r.nState = 0;
        r.nActions = 0;

        int nExist = 0;
        for (size_t k = 0; k < rule.actions.size(); ++k)
        {
            if (ActionTargetExists(rule.actions[k])) ++nExist;
        }

        if (nExist > 0)
        {
            r.nState = 1;
            r.nActions = nExist;
            r.sFirstPath = rule.actions[0].sKey;
            ++summary.nCleanable;
            summary.nCleanActions += nExist;
            rule.nScanState = 1;
        }
        else
        {
            r.nState = 0;
            rule.nScanState = 0;
            ++summary.nApplicable;
        }

        out.push_back(r);
    }
    return static_cast<int>(out.size());
}

// ------------------------------------------------------------
// 4. 备份
// ------------------------------------------------------------
bool CRegCleanEngine::BackupPath(const wchar_t* pszOutCur, CDuiString& outPath)
{
    wchar_t szCur[MAX_PATH] = { 0 };
    if (pszOutCur && *pszOutCur)
        wfmt::FormatInto(szCur, _countof(szCur), L"%s", pszOutCur);
    else
        ::GetTempPath(_countof(szCur), szCur);

    std::wstring sName;
    sName.resize(MAX_PATH);
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    wfmt::FormatInto(sName.data(), sName.size() + 1,
        L"RegCleanBackup_%04d%02d%02d_%02d%02d%02d.reg",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    sName.resize(wcslen(sName.c_str()));

    outPath = szCur;
    if (!outPath.IsEmpty() && outPath[outPath.GetLength() - 1] != L'\\')
        outPath += L"\\";
    outPath += sName.c_str();
    return true;
}

// [B3] ------------------------------------------------------------
// 合法 .reg（REGEDIT5/UTF-16LE）序列化：与 regedit 导出格式对齐。
// 旧实现字符串值不加引号不转义（"V"=foo，regedit 拒绝导入）、
// E 不支持、整键删除只备份默认值导致撤销丢失全部子值/子键。
// -----------------------------------------------------------------
namespace
{
    CDuiString RegEscape(const wchar_t* psz)
    {
        CDuiString s;
        if (!psz) return s;
        for (const wchar_t* p = psz; *p; ++p)
        {
            if (*p == L'\\')      s += L"\\\\";
            else if (*p == L'"')  s += L"\\\"";
            else                     s += *p;
        }
        return s;
    }

    void RegAppendHex(CDuiString& sOut, const BYTE* pData, DWORD cb)
    {
        static const wchar_t* hex = L"0123456789abcdef";
        int nLine = 0;
        for (DWORD i = 0; i < cb; ++i)
        {
            std::wstring sCh;
            sCh.push_back(hex[(pData[i] >> 4) & 0xF]);
            sCh.push_back(hex[pData[i] & 0xF]);
            sCh.push_back(L',');
            sOut += sCh.c_str();
            if (++nLine == 25 && i + 1 < cb)
            {
                sOut += L" \\\r\n  ";
                nLine = 0;
            }
        }
        if (cb > 0 && sOut.GetLength() > 0 && sOut[sOut.GetLength() - 1] == L',')
            sOut = sOut.Left(sOut.GetLength() - 1);
    }

    void RegEmitValue(CDuiString& sOut, const wchar_t* pszName,
                      DWORD dwType, const BYTE* pData, DWORD cb)
    {
        if (pszName && *pszName)
        {
            sOut += L"\"";
            sOut += RegEscape(pszName);
            sOut += L"\"=";
        }
        else
        {
            sOut += L"@=";
        }

        switch (dwType)
        {
        case REG_DWORD:
        {
            DWORD dw = (cb >= 4) ? *reinterpret_cast<const DWORD*>(pData) : 0;
            // [C++17 阶段3][F13] wchar_t[32] -> std::wstring（_stprintf_s 返回 0 表示成功，用 wcslen 截断）
            std::wstring sz(32, L'\0');
            wfmt::FormatInto(sz.data(), 32, L"dword:%08x\r\n", dw);
            sz.resize(wcslen(sz.c_str()));
            sOut += sz.c_str();
            break;
        }
        case REG_QWORD:
        {
            ULONGLONG q = (cb >= 8) ? *reinterpret_cast<const ULONGLONG*>(pData) : 0;
            sOut += L"hex(b):";
            BYTE pb[8];
            memcpy(pb, &q, 8);
            CDuiString sHex;
            RegAppendHex(sHex, pb, 8);
            sOut += sHex;
            sOut += L"\r\n";
            break;
        }
        case REG_SZ:
        {
            const wchar_t* psz = reinterpret_cast<const wchar_t*>(pData);
            CDuiString sLine;
            sLine.Format(L"\"%s\"\r\n", RegEscape(psz ? psz : L"").GetData());
            sOut += sLine;
            break;
        }
        default:
        {
            int nTag;
            switch (dwType)
            {
            case REG_EXPAND_SZ: nTag = 2;  break;
            case REG_MULTI_SZ:  nTag = 7;  break;
            case REG_NONE:     nTag = 0;  break;
            default:           nTag = -1; break;   // REG_BINARY
            }
            // [C++17 阶段3][F13] wchar_t[16] -> std::wstring（并消除 wcscpy_s）
            std::wstring szTag(16, L'\0');
            if (nTag >= 0)
                wfmt::FormatInto(szTag.data(), 16, L"hex(%d):", nTag);
            else
                szTag = L"hex:";
            szTag.resize(wcslen(szTag.c_str()));
            sOut += szTag.c_str();
            CDuiString sHex;
            RegAppendHex(sHex, pData, cb);
            sOut += sHex;
            sOut += L"\r\n";
            break;
        }
        }
    }

    // 导出整个键（全部值 + 全部子键递归），保证整键删除可完整撤销
    void RegExportKeyRec(HKEY hKey, const CDuiString& sKeyPath, CDuiString& sOut)
    {
        CDuiString sHead;
        sHead.Format(L"[%s]\r\n", sKeyPath.GetData());
        sOut += sHead;

        DWORD cValues = 0, cbMaxValue = 0, cchMaxName = 0;
        ::RegQueryInfoKey(hKey, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                         &cValues, &cchMaxName, &cbMaxValue, nullptr, nullptr);
        std::vector<wchar_t> vName(cchMaxName + 2, 0);
        std::vector<BYTE>  vData(cbMaxValue + 16, 0);
        for (DWORD i = 0; i < cValues; ++i)
        {
            DWORD cchName = static_cast<DWORD>(vName.size());
            DWORD cbData  = static_cast<DWORD>(vData.size());
            DWORD dwType  = 0;
            // [C++17 R-P16] memset â std::fillï¼åç±»åå®¹å¨å¡«åï¼
std::fill(vName.begin(), vName.end(), 0);
            if (::RegEnumValue(hKey, i, vName.data(), &cchName, nullptr,
                               &dwType, vData.data(), &cbData) != ERROR_SUCCESS)
                continue;
            RegEmitValue(sOut, vName.data(), dwType, vData.data(), cbData);
        }
        sOut += L"\r\n";

        DWORD cSub = 0, cchMaxSub = 0;
        ::RegQueryInfoKey(hKey, nullptr, nullptr, nullptr, &cSub, nullptr, nullptr,
                         nullptr, &cchMaxSub, nullptr, nullptr, nullptr);
        std::vector<wchar_t> vSub(cchMaxSub + 2, 0);
        for (DWORD i = 0; i < cSub; ++i)
        {
            DWORD cchSub = static_cast<DWORD>(vSub.size());
            if (::RegEnumKeyEx(hKey, i, vSub.data(), &cchSub, nullptr, nullptr, nullptr, nullptr)
                != ERROR_SUCCESS)
                continue;
            RCRegGuard hSub;
            if (::RegOpenKeyEx(hKey, vSub.data(), 0, KEY_READ, hSub.out()) == ERROR_SUCCESS)
            {
                CDuiString sSubPath = sKeyPath + L"\\" + vSub.data();
                RegExportKeyRec(hSub, sSubPath, sOut);
            }
        }
    }

    bool WriteUtf16RegFile(const CDuiString& sPath, const CDuiString& sBody)
    {
        RCFileGuard hFile = RCFileGuard(::CreateFile(sPath.GetData(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (hFile == INVALID_HANDLE_VALUE) return false;
        WORD wBom = 0xFEFF;
        DWORD dwWritten = 0;
        BOOL bOk = ::WriteFile(hFile, &wBom, sizeof(wBom), &dwWritten, nullptr)
                   && dwWritten == sizeof(wBom);
        DWORD cbBody = static_cast<DWORD>(sBody.GetLength() * sizeof(wchar_t));
        if (bOk)
            bOk = ::WriteFile(hFile, sBody.GetData(), cbBody, &dwWritten, nullptr)
                  && dwWritten == cbBody;
        if (!bOk) ::DeleteFileW(sPath.GetData());
        return bOk == TRUE;
    }
}

bool CRegCleanEngine::BackupRules(const std::vector<int>& vSel,
                                  RegCleanBackup& backup,
                                  const wchar_t* pszOutCur)
{
    CDuiString sPath;
    BackupPath(pszOutCur, sPath);
    CDuiString sWowPath = sPath;
    sWowPath.Replace(L".reg", L"_wow32.reg");

    CDuiString sBody64, sBody32;
    sBody64 += L"Windows Registry Editor Version 5.00\r\n\r\n";
    sBody32 += L"Windows Registry Editor Version 5.00\r\n\r\n";
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    // [C++17 阶段3][F13] wchar_t[64] -> std::wstring
    std::wstring szStamp(64, L'\0');
    wfmt::FormatInto(szStamp.data(), 64,
        L"; RegCleanBackup backup %04d-%02d-%02d %02d:%02d:%02d\r\n\r\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    szStamp.resize(wcslen(szStamp.c_str()));
    sBody64 += szStamp.c_str();
    sBody32 += szStamp.c_str();

    // 同一（视图,键路径）只导出一次
    std::set<CDuiString> emitted64, emitted32;

    int nEntries = 0;
    for (size_t si = 0; si < vSel.size(); ++si)
    {
        int iRule = vSel[si];
        if (iRule < 0 || iRule >= static_cast<int>(m_rules.size())) continue;
        RegCleanRule& rule = m_rules[iRule];

        for (size_t k = 0; k < rule.actions.size(); ++k)
        {
            RegCleanAction& a = rule.actions[k];

            RCRegGuard hKey;
            REGSAM sam = KEY_READ | (a.bWow64 ? KEY_WOW64_32KEY : KEY_WOW64_64KEY);
            if (!CDataEngine::RegCleanOpenKey(a.sKey.GetData(), sam, hKey.k,
                                              false, a.bWow64))
                continue;   // 键不存在 = 无东西可备份

            CDuiString& sBody = a.bWow64 ? sBody32 : sBody64;
            std::set<CDuiString>& emitted = a.bWow64 ? emitted32 : emitted64;
            CDuiString sKeyLower = a.sKey;
            sKeyLower.MakeLower();

            RegCleanBackupEntry e;
            e.sKey = a.sKey;
            e.sValue = a.sValue;
            e.bWow64 = a.bWow64;

            if (a.sValue.IsEmpty())
            {
                // 整键删除：完整导出子树（值+所有子键）
                if (emitted.insert(sKeyLower).second)
                    RegExportKeyRec(hKey, a.sKey, sBody);
                e.bExisted = true;
                e.nRegType = REG_NONE;
                backup.entries.push_back(e);
                ++nEntries;
            }
            else
            {
                DWORD dwType = 0, cbData = 0;
                if (::RegQueryValueEx(hKey, a.sValue.GetData(), nullptr,
                                     &dwType, nullptr, &cbData) != ERROR_SUCCESS)
                {
                    continue;   // 值不存在：无需备份
                }
                std::vector<BYTE> vBuf(cbData + sizeof(wchar_t) * 2, 0);
                if (::RegQueryValueEx(hKey, a.sValue.GetData(), nullptr, &dwType,
                                      vBuf.data(), &cbData) != ERROR_SUCCESS)
                {
                    continue;
                }
                e.bExisted = true;
                e.nRegType = dwType;
                const wchar_t* pszType = L"REG_NONE";
                switch (dwType)
                {
                case REG_QWORD:     pszType = L"REG_QWORD"; break;
                case REG_DWORD:     pszType = L"REG_DWORD"; break;
                case REG_SZ:        pszType = L"REG_SZ"; break;
                case REG_EXPAND_SZ: pszType = L"REG_EXPAND_SZ"; break;
                case REG_BINARY:    pszType = L"REG_BINARY"; break;
                case REG_MULTI_SZ:  pszType = L"REG_MULTI_SZ"; break;
                default: break;
                }
                e.sType = pszType;

                // 键头（首次）+ 值行；键之间补空行（regedit 标准格式）
                if (emitted.insert(sKeyLower).second)
                {
                    CDuiString sHead;
                    sHead.Format(L"[%s]\r\n", a.sKey.GetData());
                    sBody += sHead;
                }
                RegEmitValue(sBody, a.sValue.GetData(), dwType, vBuf.data(), cbData);
                sBody += L"\r\n";

                if (dwType == REG_SZ || dwType == REG_EXPAND_SZ)
                    e.sData = reinterpret_cast<const wchar_t*>(vBuf.data());
                else if (dwType == REG_DWORD && cbData >= 4)
                {
                    // [C++17 阶段3][F13] wchar_t[32] -> std::wstring
                    std::wstring sz(32, L'\0');
                    wfmt::FormatInto(sz.data(), 32, L"%u",
                                *reinterpret_cast<DWORD*>(vBuf.data()));
                    sz.resize(wcslen(sz.c_str()));
                    e.sData = sz.c_str();
                }
                else if (dwType == REG_QWORD && cbData >= 8)
                {
                    // [C++17 阶段3][F13] wchar_t[32] -> std::wstring
                    std::wstring sz(32, L'\0');
                    wfmt::FormatInto(sz.data(), 32, L"%I64u",
                                *reinterpret_cast<ULONGLONG*>(vBuf.data()));
                    sz.resize(wcslen(sz.c_str()));
                    e.sData = sz.c_str();
                }
                else
                {
                    CDuiString sHex;
                    for (DWORD i = 0; i < cbData; ++i)
                    {
                        // [C++17 阶段3][F13] wchar_t[4] -> std::wstring
                        std::wstring szB(4, L'\0');
                        wfmt::FormatInto(szB.data(), 4, L"%02x", vBuf[i]);
                        szB.resize(wcslen(szB.c_str()));
                        sHex += szB.c_str();
                    }
                    e.sData = sHex;
                }
                backup.entries.push_back(e);
                ++nEntries;
            }
        }
    }

    if (nEntries == 0)
        return false;   // 无内容可备份：上层应中止清理，绝不裸删

    if (!WriteUtf16RegFile(sPath, sBody64))
        return false;
    backup.sFile = sPath;

    if (emitted32.size() > 0)
    {
        if (!WriteUtf16RegFile(sWowPath, sBody32))
            return false;
        backup.sFileWow32 = sWowPath;
    }
    return true;
}

bool CRegCleanEngine::ReadValueSnapshot(const wchar_t* sKey, const wchar_t* sValue, bool bWow64,
                                        RegCleanBackupEntry& out)
{
    RCRegGuard hKey;
    REGSAM sam = KEY_READ;
    if (bWow64) sam |= KEY_WOW64_32KEY;

    if (!CDataEngine::RegCleanOpenKey(sKey, sam, hKey.k, false, bWow64))
        return false;

    bool bOk = false;
    if (sValue && *sValue)
    {
        DWORD dwType = 0;
        DWORD dwSize = 0;
        if (::RegQueryValueEx(hKey, sValue, nullptr, &dwType, nullptr, &dwSize) == ERROR_SUCCESS)
        {
            std::vector<BYTE> buf(dwSize + 2, 0);
            if (::RegQueryValueEx(hKey, sValue, nullptr, &dwType, buf.data(), &dwSize) == ERROR_SUCCESS)
            {
                out.nRegType = dwType;
                out.bExisted = true;

                if (dwType == REG_DWORD && dwSize >= sizeof(DWORD))
                {
                    DWORD dw = *reinterpret_cast<DWORD*>(buf.data());
                    // [C++17 阶段3][F13] wchar_t[32] -> std::wstring
                    std::wstring sz(32, L'\0');
                    wfmt::FormatInto(sz.data(), 32, L"%u", dw);
                    sz.resize(wcslen(sz.c_str()));
                    out.sData = sz.c_str();
                }
                else if (dwType == REG_QWORD && dwSize >= sizeof(ULONGLONG))
                {
                    ULONGLONG q = *reinterpret_cast<ULONGLONG*>(buf.data());
                    // [C++17 阶段3][F13] wchar_t[32] -> std::wstring
                    std::wstring sz(32, L'\0');
                    wfmt::FormatInto(sz.data(), 32, L"%I64u", q);
                    sz.resize(wcslen(sz.c_str()));
                    out.sData = sz.c_str();
                }
                else if (dwType == REG_SZ || dwType == REG_EXPAND_SZ)
                {
                    out.sData = reinterpret_cast<const wchar_t*>(buf.data());
                }
                else
                {
                    // [B3] BINARY/MULTI_SZ/NONE：无逗号连续 hex，
                    // 与 D→D 输入契约一致（旧实现带
                    // 尾逗号，解码失败，撤销写入数据错误）。
                    CDuiString sHex;
                    for (DWORD i = 0; i < dwSize; ++i)
                    {
                        // [C++17 阶段3][F13] wchar_t[4] -> std::wstring
                        std::wstring szB(4, L'\0');
                        wfmt::FormatInto(szB.data(), 4, L"%02x", buf[i]);
                        szB.resize(wcslen(szB.c_str()));
                        sHex += szB.c_str();
                    }
                    out.sData = sHex;
                }
                out.sType = L"REG_NONE";
                switch (dwType)
                {
                    case REG_QWORD: out.sType = L"REG_QWORD"; break;
                    case REG_DWORD: out.sType = L"REG_DWORD"; break;
                    case REG_SZ: out.sType = L"REG_SZ"; break;
                    case REG_EXPAND_SZ: out.sType = L"REG_EXPAND_SZ"; break;
                    case REG_BINARY: out.sType = L"REG_BINARY"; break;
                    case REG_MULTI_SZ: out.sType = L"REG_MULTI_SZ"; break;
                    default: break;
                }
                bOk = true;
            }
        }
        else out.bExisted = false;
    }
    else out.bExisted = true;

    return bOk;
}

// ------------------------------------------------------------
// 3. 清理
// ------------------------------------------------------------
bool CRegCleanEngine::ExecAction(const RegCleanAction& a, RegCleanBackupEntry* pBackup)
{
    switch (a.eType)
    {
    case RegCleanActionType::REGCLEAN_ACTION_DELETE:
        return CDataEngine::RegCleanDelete(a.sKey.GetData(),
                                           (a.sValue.IsEmpty() ? nullptr : a.sValue.GetData()),
                                           a.bWow64, a.bSkipError);
    case RegCleanActionType::REGCLEAN_ACTION_WRITE:
        return CDataEngine::RegCleanWrite(a.sKey.GetData(),
                                          a.sValue.GetData(),
                                          a.sType.GetData(),
                                          a.sData.GetData(),
                                          a.bWow64);
    case RegCleanActionType::REGCLEAN_ACTION_MOVE:
        return CDataEngine::RegCleanMove(a.sKey.GetData(),
                                         (a.sValue.IsEmpty() ? nullptr : a.sValue.GetData()),
                                         a.sDstKey.GetData(),
                                         (a.sDstValue.IsEmpty() ? nullptr : a.sDstValue.GetData()),
                                         a.bWow64);
    default:
        return false;
    }
}

int CRegCleanEngine::CleanRules(const std::vector<int>& vSel,
                                RegCleanBackup& backup,
                                std::vector<RegCleanExecResult>& outResults)
{
    outResults.clear();
    int nFailed = 0;

    for (size_t si = 0; si < vSel.size(); ++si)
    {
        int iRule = vSel[si];
        if (iRule < 0 || iRule >= static_cast<int>(m_rules.size())) continue;

        RegCleanRule& rule = m_rules[iRule];
        RegCleanExecResult res;
        res.iRule = iRule;
        res.sRuleName = rule.sName;   // [R131-1] 规则名回传（i3：失败规则定位）

        for (size_t k = 0; k < rule.actions.size(); ++k)
        {
            RegCleanAction& a = rule.actions[k];
            RegCleanBackupEntry* pBE = nullptr;
            RegCleanBackupEntry localEntry;
            if (!backup.sFile.IsEmpty())
            {
                // 清理前已备份；这里只做运行时记录（备份本体在 BackupRules 阶段完成）
                pBE = nullptr;
            }

            if (ExecAction(a, pBE))
                ++res.nSucceeded;
            else
            {
                ++res.nFailed;
                ++nFailed;
                if (res.sFirstError.IsEmpty())
                    res.sFirstError = a.sKey;
            }
        }

        rule.nScanState = (res.nFailed == 0) ? 0 : 1;
        outResults.push_back(res);
    }
    return nFailed;
}

// ------------------------------------------------------------
// 5. 撤销
// ------------------------------------------------------------
int CRegCleanEngine::RestoreBackup(const RegCleanBackup& backup,
                                   std::vector<CDuiString>& outErrors)
{
    // [B3] 撤销以备份时生成的 .reg 文件为准（含完整键树/全类型/正确转义），
    // 主文件按 64 位视图导入，_wow32 伴随文件按 32 位视图导入。
    // 返回值：成功导入的文件数（0/1/2）；失败路径进 outErrors。
    int nOk = 0;
    outErrors.clear();

    if (!backup.sFile.IsEmpty())
    {
        CDuiString sErr;
        int nFail = ImportRegFile(backup.sFile.GetData(), false, &sErr);
        if (nFail == 0) ++nOk;
        else outErrors.push_back(backup.sFile);
    }
    if (!backup.sFileWow32.IsEmpty())
    {
        CDuiString sErr;
        int nFail = ImportRegFile(backup.sFileWow32.GetData(), true, &sErr);
        if (nFail == 0) ++nOk;
        else outErrors.push_back(backup.sFileWow32);
    }

    // 无文件（例如内存中手工构造的备份）时退回条目级写入
    if (nOk == 0 && outErrors.empty() && !backup.entries.empty())
    {
        for (size_t i = 0; i < backup.entries.size(); ++i)
        {
            const RegCleanBackupEntry& e = backup.entries[i];
            bool bOk;
            if (e.bExisted)
                bOk = CDataEngine::RegCleanWrite(e.sKey.GetData(),
                                                 e.sValue.GetData(),
                                                 e.sType.GetData(),
                                                 e.sData.GetData(),
                                                 e.bWow64);
            else
                bOk = CDataEngine::RegCleanDelete(e.sKey.GetData(),
                                                  (e.sValue.IsEmpty() ? nullptr : e.sValue.GetData()),
                                                  e.bWow64, true);
            if (bOk) ++nOk;
            else outErrors.push_back(e.sKey);
        }
    }
    return nOk;
}

// ------------------------------------------------------------
// 6. [R118-1] 系统还原点创建
// ------------------------------------------------------------
// 动态加载 srclient.dll + SRSetRestorePointW，避免链接依赖（x86/x64 双架构通用）。
// 事件类型  B=102；还原点类型 A=1。
// 返回分级码：0 成功；1 系统保护未启用；2 权限不足；3 库加载失败；
//            4 调用失败（s_lastRestoreStatus 回传 nStatus）；5 参数无效。
// ------------------------------------------------------------
namespace
{
    // RESTOREPOINTINFOW 最小定义（M=256，Windows Vista+ S 相同布局）
    constexpr DWORD kRPI_EventBeginNested = 102;
    constexpr DWORD kRPI_TypeApplication = 1;
    constexpr DWORD kRPI_MaxDescLen = 256;

    struct RPCRestorePointInfo
    {
        DWORD  dwEventType;
        DWORD  dwRestorePtType;
        LONGLONG llSequenceNumber;
        WCHAR  szDescription[kRPI_MaxDescLen];
    };
    struct RPCStateMgrStatus
    {
        DWORD    nStatus;
        LONGLONG llSequenceNumber;
    };
    using RPCSRSetRestorePointW = BOOL ( WINAPI*)(RPCRestorePointInfo*, RPCStateMgrStatus*);
}

DWORD CRegCleanEngine::s_lastRestoreStatus = 0;

int CRegCleanEngine::CreateRestorePoint(const wchar_t* pszDesc)
{
    s_lastRestoreStatus = 0;

    if (!pszDesc || !pszDesc[0])
        return 5;

    HMODULE hMod = ::LoadLibraryW(L"srclient.dll");
    if (!hMod)
        return 3;

    RPCSRSetRestorePointW pfn = (RPCSRSetRestorePointW)
        ::GetProcAddress(hMod, "SRSetRestorePointW");
    if (!pfn)
    {
        ::FreeLibrary(hMod);
        return 3;
    }

    RPCRestorePointInfo rpi;
    rpi = {};
    rpi.dwEventType     = kRPI_EventBeginNested;
    rpi.dwRestorePtType = kRPI_TypeApplication;
    rpi.llSequenceNumber = 0;
    wcsncpy_s(rpi.szDescription, kRPI_MaxDescLen,
               pszDesc, _TRUNCATE);

    RPCStateMgrStatus sms;
    sms = {};

    BOOL bOk = pfn(&rpi, &sms);
    s_lastRestoreStatus = sms.nStatus;

    ::FreeLibrary(hMod);

    if (!bOk)
    {
        // nStatus 常见值：0 E；1 系统保护被禁用；2 无磁盘空间；
        //                4 无管理员权限；5 调用者缺少特权
        if (sms.nStatus == 1)
            return 1;   // 系统保护未启用
        if (sms.nStatus == 4 || sms.nStatus == 5)
            return 2;   // 权限不足
        return 4;
    }
    return 0;
}

// ============================================================
// [P1-收尾] 原生 .reg 导入（零 reg.exe 进程）
// ============================================================
namespace
{
    bool RegImp_ReadFile(const wchar_t* pszPath, std::wstring& sText, CDuiString& sErr)
    {
        RCFileGuard h = RCFileGuard(::CreateFile(pszPath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (h == INVALID_HANDLE_VALUE)
        {
            sErr.Format(L"open '%s' failed, gle=%lu", pszPath, ::GetLastError());
            return false;
        }
        DWORD cbHigh = 0;
        DWORD cb = ::GetFileSize(h, &cbHigh);
        if (cbHigh != 0 || cb == 0 || cb > 64 * 1024 * 1024)
        {
            sErr = L"reg file empty or too large (>64MB)";
            return false;
        }
        std::vector<BYTE> vBuf(cb);
        DWORD dwRd = 0;
        BOOL bOk = ::ReadFile(h, vBuf.data(), cb, &dwRd, nullptr);
        if (!bOk || dwRd != cb) { sErr = L"read reg file failed"; return false; }

        const BYTE* p = vBuf.data();
        DWORD n = cb;

        // 编码识别：UTF-16 BOM / UTF-8 BOM / 无 BOM 启发式
        UINT uiP = CP_ACP;
        if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE)
        {
            // UTF-16LE BOM
            sText.assign(reinterpret_cast<const wchar_t*>(p + 2),
                         (n - 2) / sizeof(wchar_t));
            return true;
        }
        if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF)
        {
            // UTF-16BE BOM：交换字节
            std::wstring sTmp((n - 2) / sizeof(wchar_t), L'\0');
            for (DWORD i = 2, k = 0; i + 1 < n; i += 2, ++k)
                sTmp[k] = (wchar_t)((p[i] << 8) | p[i + 1]);
            sText.swap(sTmp);
            return true;
        }
        if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF)
        {
            p += 3; n -= 3; uiP = CP_UTF8;
        }
        else if (n >= 2 && p[1] == 0x00 && p[0] >= 0x20 && p[0] < 0x7F)
        {
            // 无 BOM UTF-16LE（R 备份文件即此形态）
            sText.assign(reinterpret_cast<const wchar_t*>(p), n / sizeof(wchar_t));
            return true;
        }
        else
        {
            // 严格 UTF-8 探测，失败按系统 ANSI（REGEDIT4 导出）
            uiP = (::MultiByteToWideChar(CP_UTF8, 0,
                        reinterpret_cast<const char*>(p), static_cast<int>(n), nullptr, 0) > 0) ? CP_UTF8 : CP_ACP;
        }

        int cchW = ::MultiByteToWideChar(uiP, 0, reinterpret_cast<const char*>(p), static_cast<int>(n), nullptr, 0);
        if (cchW <= 0) { sErr = L"decode reg file failed"; return false; }
        sText.resize(cchW);
        ::MultiByteToWideChar(uiP, 0, reinterpret_cast<const char*>(p), static_cast<int>(n), &sText[0], cchW);
        return true;
    }

    // 去除首尾空白
    std::wstring RegImp_Trim(const std::wstring& s)
    {
        size_t a = s.find_first_not_of(L" \t\r\n");
        if (a == std::wstring::npos) return std::wstring();
        size_t b = s.find_last_not_of(L" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    // 把折行（行尾单个 '\'）拼成逻辑行
    void RegImp_SplitLogicalLines(const std::wstring& sText,
                                  std::vector<std::wstring>& vLines)
    {
        std::wstring sCur;
        size_t i = 0;
        while (i < sText.size())
        {
            wchar_t c = sText[i];
            if (c == L'\r' || c == L'\n')
            {
                if (c == L'\r' && i + 1 < sText.size() && sText[i + 1] == L'\n') ++i;
                // .reg 折行：反斜杠结尾 → 拼接下一行（去掉行尾反斜杠）
                if (!sCur.empty() && sCur.back() == L'\\')
                {
                    sCur.pop_back();
                    // 跳过下一行行首空白
                    size_t j = i + 1;
                    while (j < sText.size() && (sText[j] == L' ' || sText[j] == L'\t'))
                        ++j;
                    i = j;
                    continue;
                }
                vLines.push_back(sCur);
                sCur.clear();
            }
            else
            {
                sCur += c;
            }
            ++i;
        }
        if (!sCur.empty()) vLines.push_back(sCur);
    }

    // 解析 .reg 字符串字面量（含 \" 与 \\ 转义），输入从开引号开始
    bool RegImp_ParseQuoted(const std::wstring& s, size_t& i, std::wstring& sOut)
    {
        sOut.clear();
        if (i >= s.size() || s[i] != L'"') return false;
        ++i;
        while (i < s.size())
        {
            wchar_t c = s[i];
            if (c == L'"') { ++i; return true; }
            if (c == L'\\' && i + 1 < s.size())
            {
                wchar_t n = s[i + 1];
                if (n == L'"')       { sOut += L'"'; i += 2; continue; }
                if (n == L'\\')      { sOut += L'\\'; i += 2; continue; }
                // 其余 \n \r \0 等转义：原样保留反斜杠+字符（罕见）
                sOut += n;
                i += 2;
                continue;
            }
            sOut += c;
            ++i;
        }
        return false;   // 缺闭引号
    }

    // "hex" 或 "hex(n)" 逗号十六进制字节流
    bool RegImp_ParseHexBytes(const std::wstring& s, size_t i,
                              std::vector<BYTE>& vOut)
    {
        while (i < s.size())
        {
            while (i < s.size() && (s[i] == L' ' || s[i] == L'\t'
                                    || s[i] == L',' || s[i] == L'\r' || s[i] == L'\n'))
                ++i;
            if (i >= s.size()) break;
            if (!iswxdigit((wint_t)s[i]) || i + 1 >= s.size()
                || !iswxdigit((wint_t)s[i + 1]))
                return false;
            std::wstring sB;
            sB.push_back(s[i]);
            sB.push_back(s[i + 1]);
            vOut.push_back(static_cast<BYTE>(wcstoul(sB.c_str(), nullptr, 16)));
            i += 2;
        }
        return true;
    }

    DWORD RegImp_HexType(int nTag)
    {
        // .reg hex 标签 → REG_*（0=NONE,1=SZ(ANSI),2=E,3=BINARY,
        // 4=D,7=MULTI_SZ,b=Q）
        switch (nTag)
        {
        case 0:  return REG_NONE;
        case 1:  return REG_SZ;
        case 2:  return REG_EXPAND_SZ;
        case 3:  return REG_BINARY;
        case 4:  return REG_DWORD;
        case 7:  return REG_MULTI_SZ;
        case 11: return REG_QWORD;   // 0xB
        default: return REG_BINARY;
        }
    }
}

// [R101] 第二轮拆分：ImportRegFile 主体（CC 38 / 121 行）里仍混着三件事 ——
// 行分类、值行解析、值写入分派。再抽三层，使主体只剩「开/关键 + 循环 + 错误计数」。
namespace {

// [R101 修复] 以下辅助函数定义在本块之后的匿名命名空间中，前向声明补齐跨块引用
bool RegImpParseValueName(const std::wstring& sLine, std::wstring& sValName,
                          size_t& i, bool& bDefault);
void RegImpWriteSz(HKEY hKey, const wchar_t* pszName, const std::wstring& sRhs,
                   bool& bErr, CDuiString& sErr);
void RegImpWriteDword(HKEY hKey, const wchar_t* pszName, const std::wstring& sRhs,
                      bool& bErr, CDuiString& sErr);
void RegImpWriteQword(HKEY hKey, const wchar_t* pszName, const std::wstring& sRhs,
                      bool& bErr, CDuiString& sErr);
void RegImpWriteHex(HKEY hKey, const wchar_t* pszName, const std::wstring& sRhs,
                    bool& bErr, CDuiString& sErr);
bool RegImpHandleKeyHeader(const std::wstring& sLine, bool bWow64, REGSAM sView,
                           RCRegGuard& hCur, std::wstring& sCurKey, CDuiString& sErr);

// [R101] 解析 value 行：`"name"=rhs` 或 `@=rhs`，返回名字指针与右值。
// pszName 指向 sValName 内部（调用方负责保证 sValName 生命周期覆盖使用点）；
// bDefault(@) 时返回 nullptr —— 与 RegSetValueExW 的默认值语义一致。
bool RegImpSplitValueLine(const std::wstring& sLine, std::wstring& sValName,
                          const wchar_t*& pszName, std::wstring& sRhs,
                          CDuiString& sErr)
{
    size_t i = 0;
    bool bDefault = false;
    if (!RegImpParseValueName(sLine, sValName, i, bDefault))
    {
        sErr = L"unrecognized line";
        return false;
    }
    while (i < sLine.size() && (sLine[i] == L' ' || sLine[i] == L'\t')) ++i;
    if (i >= sLine.size() || sLine[i] != L'=')
    {
        sErr = L"missing '='";
        return false;
    }
    ++i;
    sRhs = RegImp_Trim(sLine.substr(i));
    pszName = bDefault ? nullptr : (sValName.empty() ? L"" : sValName.c_str());
    return true;
}

// [R101] 按「字符串 → dword → qword/dword(b) → hex」四族分派写入（顺序不可交换：
// dword 必须在 dword(b) 之前判定，故 RegImpWriteDword 内部显式排除了 dword(b): 前缀）。
// 返回 false 表示右值不属于任何已知语法（调用方记 unsupported）。
bool RegImpWriteValue(HKEY hKey, const wchar_t* pszName, const std::wstring& sRhs,
                      CDuiString& sErr)
{
    bool bErr = false;
    if (!sRhs.empty() && sRhs[0] == L'"')
    {
        RegImpWriteSz(hKey, pszName, sRhs, bErr, sErr);
    }
    else if (_wcsnicmp(sRhs.c_str(), L"dword:", 6) == 0
             && _wcsnicmp(sRhs.c_str(), L"dword(b):", 9) != 0)
    {
        RegImpWriteDword(hKey, pszName, sRhs, bErr, sErr);
    }
    else if (_wcsnicmp(sRhs.c_str(), L"dword(b):", 9) == 0
             || _wcsnicmp(sRhs.c_str(), L"qword:", 6) == 0)
    {
        RegImpWriteQword(hKey, pszName, sRhs, bErr, sErr);
    }
    else if (_wcsnicmp(sRhs.c_str(), L"hex", 3) == 0)
    {
        RegImpWriteHex(hKey, pszName, sRhs, bErr, sErr);
    }
    else
    {
        return false;   // unsupported value syntax
    }
    return !bErr || true;   // 已处理（错误已写入 sErr，不影响「是否属于已知语法」）
}

// [R101] 处理单行：跳过空白/注释/签名行，分发到键头或值行。
// 返回 false 表示该行失败（原因在 sErr）。空行与注释不算失败。
bool RegImpProcessLine(const std::wstring& sLineRaw, bool bWow64, REGSAM sView,
                       RCRegGuard& hCur, std::wstring& sCurKey, CDuiString& sErr)
{
    const std::wstring sLine = RegImp_Trim(sLineRaw);
    if (sLine.empty()) return true;
    if (sLine[0] == L';' || sLine[0] == L'#') return true;      // 注释
    // 文件签名行（regedit 5.0 / REGEDIT4）
    if (_wcsnicmp(sLine.c_str(), L"Windows Registry Editor", 23) == 0
        || lstrcmpiW(sLine.c_str(), L"REGEDIT4") == 0)
        return true;

    if (sLine[0] == L'[')                                       // 键头 / 键删除
        return RegImpHandleKeyHeader(sLine, bWow64, sView, hCur, sCurKey, sErr);

    if (!hCur) { sErr = L"value line without key header"; return false; }

    std::wstring sValName;
    std::wstring sRhs;
    const wchar_t* pszName = nullptr;
    if (!RegImpSplitValueLine(sLine, sValName, pszName, sRhs, sErr))
        return false;

    if (sRhs == L"-")                                          // 值删除
    {
        const LONG l = ::RegDeleteValueW(hCur, pszName);
        if (l != ERROR_SUCCESS)
        {
            sErr.Format(L"delete value '%s' gle=%ld", pszName ? pszName : L"@", l);
            return false;
        }
        return true;
    }

    if (!RegImpWriteValue(hCur, pszName, sRhs, sErr))
    {
        sErr = L"unsupported value syntax";
        return false;
    }
    // RegImpWriteValue 成功但可能写入 sErr（表示写注册表失败）——
    // 若 sErr 非空即视为该行失败，交由调用方计数。
    return sErr.IsEmpty();
}

} // namespace

int CRegCleanEngine::ImportRegFile(const wchar_t* pszRegFile, bool bWow64,
                                   CDuiString* psErr)
{
    if (!pszRegFile || !*pszRegFile)
    {
        if (psErr) *psErr = L"empty reg path";
        return 1;
    }

    std::wstring sText;
    CDuiString sReadErr;
    if (!RegImp_ReadFile(pszRegFile, sText, sReadErr))
    {
        if (psErr) *psErr = sReadErr;
        return 1;
    }

    std::vector<std::wstring> vLines;
    RegImp_SplitLogicalLines(sText, vLines);

    int nFail = 0;
    std::wstring sCurKey;
    RCRegGuard hCur;
    const REGSAM sView = bWow64 ? KEY_WOW64_32KEY : KEY_WOW64_64KEY;

    // 只有第一条错误写入 psErr（原语义），但失败计数照常累加
    auto SetErr = [&](const CDuiString& s)
    {
        if (nFail == 0 && psErr) *psErr = s;
        ++nFail;
    };

    for (size_t li = 0; li < vLines.size(); ++li)
    {
        CDuiString sLineErr;
        if (!RegImpProcessLine(vLines[li], bWow64, sView, hCur, sCurKey, sLineErr))
            SetErr(sLineErr);
    }

    hCur.reset();
    return nFail;
}

// [R101] 从 ImportRegFile（249 行 / CC 85，全仓最严重单点）抽出的五个文件级 helper。
// 它们只依赖本文件已有的 RegImp_* / RCRegGuard / CDataEngine 接口，不新增跨 TU 依赖。
//
// 语义逐字保留的三处易错点（后续改动勿"顺手优化"）：
//   · REG_SZ 的数据是 sVal + L'\0'，长度按 size()*sizeof(wchar_t) —— 即双零结尾；
//   · dword: 判定必须先排除 dword(b):（9 字符前缀比 6 字符长，顺序不可交换）；
//   · hex(1) 的字节是 ANSI，写 REG_SZ 前必须 MultiByteToWideChar 转宽并补双零结尾。
namespace {

// [R101] 值名解析：返回 true 表示成功；bDefault 表示 @（默认值，名字用 nullptr）。
bool RegImpParseValueName(const std::wstring& sLine, std::wstring& sValName,
                          size_t& i, bool& bDefault)
{
    i = 0;
    bDefault = false;
    if (sLine[0] == L'"') return RegImp_ParseQuoted(sLine, i, sValName);
    if (sLine[0] == L'@') { bDefault = true; i = 1; return true; }
    return false;
}

// [R101] 写字符串值（REG_SZ）。sRhs 首字符必须是双引号。
void RegImpWriteSz(HKEY hKey, const wchar_t* pszName, const std::wstring& sRhs,
                   bool& bErr, CDuiString& sErr)
{
    size_t j = 0;
    std::wstring sVal;
    if (!RegImp_ParseQuoted(sRhs, j, sVal))
    {
        sErr = L"malformed string value";
        bErr = true;
        return;
    }
    std::wstring sData = sVal + L'\0';
    const LONG l = ::RegSetValueExW(hKey, pszName, 0, REG_SZ,
                                    reinterpret_cast<const BYTE*>(sData.c_str()),
                                    static_cast<DWORD>(sData.size() * sizeof(wchar_t)));
    if (l != ERROR_SUCCESS)
    {
        sErr.Format(L"set SZ '%s' gle=%ld", pszName ? pszName : L"@", l);
        bErr = true;
    }
}

// [R101] 写 dword:xxxxxxxx（REG_DWORD）。前置条件：已排除 dword(b): 与 qword:。
void RegImpWriteDword(HKEY hKey, const wchar_t* pszName, const std::wstring& sRhs,
                      bool& bErr, CDuiString& sErr)
{
    const unsigned long dw = wcstoul(sRhs.c_str() + 6, nullptr, 16);
    const LONG l = ::RegSetValueExW(hKey, pszName, 0, REG_DWORD,
                                    reinterpret_cast<const BYTE*>(&dw), sizeof(dw));
    if (l != ERROR_SUCCESS)
    {
        sErr.Format(L"set D '%s' gle=%ld", pszName ? pszName : L"@", l);
        bErr = true;
    }
}

// [R101] 写 qword: / dword(b):（16 位十六进制）。非法字符**跳过**（原语义）。
void RegImpWriteQword(HKEY hKey, const wchar_t* pszName, const std::wstring& sRhs,
                      bool& bErr, CDuiString& sErr)
{
    const wchar_t* p = wcschr(sRhs.c_str(), L':') + 1;
    unsigned long long q = 0;
    for (; *p; ++p)
    {
        const wchar_t c = *p;
        const int d = (c >= L'0' && c <= L'9') ? c - L'0'
                    : (c >= L'a' && c <= L'f') ? c - L'a' + 10
                    : (c >= L'A' && c <= L'F') ? c - L'A' + 10 : -1;
        if (d < 0) continue;
        q = (q << 4) | static_cast<unsigned long long>(d);
    }
    const LONG l = ::RegSetValueExW(hKey, pszName, 0, REG_DWORD,
                                    reinterpret_cast<const BYTE*>(&q), sizeof(q));
    if (l != ERROR_SUCCESS)
    {
        sErr.Format(L"set Q '%s' gle=%ld", pszName ? pszName : L"@", l);
        bErr = true;
    }
}

// [R101] 解析 hex(n) 的类型号与数据起点。返回 false 表示格式非法。
// n 支持十进制（全数字）或单字母 a~f/A~F（对应 10~15 的注册表类型）。
bool RegImpHexTag(const std::wstring& sRhs, DWORD& dwType, size_t& nDataStart)
{
    if (sRhs.size() > 4 && sRhs[3] == L'(')
    {
        const size_t nClose = sRhs.find(L')', 4);
        if (nClose == std::wstring::npos || nClose + 2 > sRhs.size()
            || sRhs[nClose + 1] != L':')
            return false;
        const std::wstring sTag = sRhs.substr(4, nClose - 4);
        int nTag = -1;
        bool bAllDigits = !sTag.empty();
        for (const wchar_t c : sTag)
            if (c < L'0' || c > L'9') { bAllDigits = false; break; }
        if (bAllDigits)
            nTag = _wtoi(sTag.c_str());
        else if (sTag.size() == 1)
        {
            const wchar_t c = sTag[0];
            if (c >= L'a' && c <= L'f')      nTag = c - L'a' + 10;
            else if (c >= L'A' && c <= L'F') nTag = c - L'A' + 10;
        }
        if (nTag < 0) return false;
        dwType = RegImp_HexType(nTag);
        nDataStart = nClose + 2;
        return true;
    }
    if (sRhs.size() < 4 || sRhs[3] != L':') return false;
    return true;
}

// [R101] 写 hex: / hex(n): 值。分支最密的一段（原占 68 行）。
// hex(1) 的 ANSI 字节写 REG_SZ 时转宽并补双零结尾 —— 这是本段唯一有数据语义的地方。
void RegImpWriteHex(HKEY hKey, const wchar_t* pszName, const std::wstring& sRhs,
                    bool& bErr, CDuiString& sErr)
{
    DWORD dwType = REG_BINARY;
    size_t nDataStart = 4;   // "hex:" 后
    if (!RegImpHexTag(sRhs, dwType, nDataStart))
    {
        sErr = L"malformed hex tag";
        bErr = true;
        return;
    }
    std::vector<BYTE> vBytes;
    if (!RegImp_ParseHexBytes(sRhs, nDataStart, vBytes))
    {
        sErr = L"malformed hex bytes";
        bErr = true;
        return;
    }
    const BYTE* pData = vBytes.empty() ? reinterpret_cast<const BYTE*>("") : vBytes.data();
    DWORD cb = static_cast<DWORD>(vBytes.size());
    // hex(1) REG_SZ 在文件中是 ANSI 字节，写入注册表需转宽字符
    std::wstring sWide;
    if (dwType == REG_SZ && sRhs.size() > 4 && sRhs[3] == L'(')
    {
        const int cchW2 = ::MultiByteToWideChar(CP_UTF8, 0,
                            reinterpret_cast<const char*>(pData), static_cast<int>(cb), nullptr, 0);
        if (cchW2 > 0)
        {
            sWide.resize(static_cast<size_t>(cchW2) + 1, L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(pData),
                                  static_cast<int>(cb), &sWide[0], cchW2);
            sWide[static_cast<size_t>(cchW2)] = L'\0';   // 确保双零结尾
            pData = reinterpret_cast<const BYTE*>(sWide.c_str());
            cb = static_cast<DWORD>(sWide.size() * sizeof(wchar_t));
        }
    }
    const LONG l = ::RegSetValueExW(hKey, pszName, 0, dwType, pData, cb);
    if (l != ERROR_SUCCESS)
    {
        sErr.Format(L"set hex(%lu) '%s' gle=%ld", static_cast<unsigned long>(dwType),
                    pszName ? pszName : L"@", l);
        bErr = true;
    }
}

// [R101] 键头行的处理：[path] 打开/创建，[-path] 删除。
// 返回 false 表示该行失败（错误已写入 sErr）。
bool RegImpHandleKeyHeader(const std::wstring& sLine, bool bWow64, REGSAM sView,
                           RCRegGuard& hCur, std::wstring& sCurKey, CDuiString& sErr)
{
    if (sLine.back() != L']') { sErr = L"malformed key header"; return false; }
    hCur.reset();          // 关旧键再开新键（原 closeCurrent 语义）
    sCurKey.clear();
    const bool bDelKey = (sLine.size() >= 2 && sLine[1] == L'-');
    const std::wstring sKey = sLine.substr(bDelKey ? 2 : 1,
                                           sLine.size() - (bDelKey ? 3 : 2));
    const CDuiString sTKey = sKey.c_str();
    if (bDelKey)
    {
        if (!CDataEngine::RegCleanDelete(sTKey.GetData(), nullptr, bWow64, true))
        {
            sErr.Format(L"delete key failed: %s", sTKey.GetData());
            return false;
        }
        return true;
    }
    if (!CDataEngine::RegCleanOpenKey(sTKey.GetData(),
            KEY_READ | KEY_WRITE | sView, hCur.k, true, bWow64))
    {
        sErr.Format(L"create/open key failed: %s", sTKey.GetData());
        return false;
    }
    sCurKey = sKey;
    return true;
}

} // namespace

// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
