#include "stdafx.h"
#include "DataEngine.h"
#include <cwctype>   // [C12] std::towlower
#include "OfflineSession.h"   // [R10-02] 离线会话薄转发至 COfflineSession
#include "DataEngine_RegInternal.h"
#include "RegCleanEngine.h"   // [P1-收尾] ActImportReg：原生 .reg 解析（零 reg.exe）
#include <vector>
#include <optional>   // [R125-4] ParseRegByteToken 返回 std::optional<BYTE>

#include <string>
// [C++17 升级阶段2] 注册表句柄 RAII 守卫：OpenRegKey(HKEY&) 打开成功后接管，
// 只负责早退/异常路径自动 RegCloseKey，不改变任何调用顺序与业务分支。
// Wow64Scope(hKey) 的视图切换时序不受影响。
struct RegKeyGuard
{
    RegKeyGuard() = default;
    explicit RegKeyGuard(HKEY h) : m_h(h) {}
    ~RegKeyGuard() { if (m_h) ::RegCloseKey(m_h); }
    RegKeyGuard(const RegKeyGuard&) = delete;
    RegKeyGuard& operator=(const RegKeyGuard&) = delete;
    operator HKEY() const { return m_h; }
private:
    HKEY m_h = nullptr;
};

// ============================================================
// DataEngine_Reg.cpp —— [R9-02] 注册表执行器。
// 动作处理器 ActRegWrite/RegDelete/RegMove/ImportReg 与原语
// DoRegWrite(三段拆分)/DoRegDelete/DoRegMove 均从 DataEngine.cpp 迁入，
// 逻辑零改动，仅按域分文件。
// ============================================================

// 读取 Wow64 双视图旗标（True=仅32位, All=双视图, 其它=默认64位）
static void GetWow64Flags(CMarkupNode node, bool& bW64, bool& bW64All)
{
    if (!node.HasAttribute(L"Wow64")) { bW64 = false; bW64All = false; return; }
    CDuiString sW = node.GetAttributeValue(L"Wow64");
    bW64    = (lstrcmpiW(sW, L"True") == 0);
    bW64All = (lstrcmpiW(sW, L"All") == 0);
}

CDataEngine::EActResult CDataEngine::ActRegWrite(CMarkupNode node, TActCtx&)
{
    const wchar_t* pszData = node.GetAttributeValue(L"Data");
    CDuiString strFunData;
    // Fun="True" with e.g. Data="?ColorDialog()": ask the user now.
    if (lstrcmpiW(node.GetAttributeValue(L"Fun"), L"True") == 0
        && pszData && *pszData == L'?')
    {
        if (!EvalFunData(pszData, strFunData))
            return EActResult::ACT_SKIP;   // 用户在颜色对话框点取消：中性跳过
        pszData = strFunData.GetData();
    }

    bool bW64 = node.HasAttribute(L"Wow64")
        && lstrcmpiW(node.GetAttributeValue(L"Wow64"), L"True") == 0;
    // [R18/O-08] Wow64="All" 需要在 32 位与 64 位视图各执行一次
    bool bW64All = node.HasAttribute(L"Wow64")
        && lstrcmpiW(node.GetAttributeValue(L"Wow64"), L"All") == 0;
    const wchar_t* pszOp = node.HasAttribute(L"Operator")
        ? node.GetAttributeValue(L"Operator") : nullptr;
    const wchar_t* pszMask = node.HasAttribute(L"Msk")
        ? node.GetAttributeValue(L"Msk") : nullptr;
    // [B4] 位运算需先读现值；有 Operator 时 DoRegWrite 内部按
    // KEY_QUERY_VALUE|KEY_SET_VALUE 重开（实测单 SET_VALUE 读返回 ACCESS_DENIED）。
    bool bAct = DoRegWrite(node.GetAttributeValue(L"Key"),
                           node.GetAttributeValue(L"Value"),
                           node.GetAttributeValue(L"Type"),
                           pszData, bW64, pszOp, pszMask);
    if (bW64All)
    {
        bool bAct2 = DoRegWrite(node.GetAttributeValue(L"Key"),
                                node.GetAttributeValue(L"Value"),
                                node.GetAttributeValue(L"Type"),
                                pszData, true, pszOp, pszMask);
        bAct = bAct && bAct2;
    }
    return bAct ? EActResult::ACT_OK : EActResult::ACT_FAIL;
}

CDataEngine::EActResult CDataEngine::ActRegDelete(CMarkupNode node, TActCtx&)
{
    bool bW64 = false, bW64All = false;
    GetWow64Flags(node, bW64, bW64All);
    bool bSkip = node.HasAttribute(L"SkipError");
    bool bAct = DoRegDelete(node.GetAttributeValue(L"Key"),
                            node.GetAttributeValue(L"Value"), bW64, bSkip);
    if (bW64All)
    {
        bool bAct2 = DoRegDelete(node.GetAttributeValue(L"Key"),
                                 node.GetAttributeValue(L"Value"), true, bSkip);
        bAct = bAct && bAct2;
    }
    if (bAct) return EActResult::ACT_OK;
    // SkipError="源不存在可忽略" → 中性跳过，不记失败
    return bSkip ? EActResult::ACT_SKIP : EActResult::ACT_FAIL;
}

CDataEngine::EActResult CDataEngine::ActRegMove(CMarkupNode node, TActCtx&)
{
    bool bW64 = false, bW64All = false;
    GetWow64Flags(node, bW64, bW64All);
    bool bSkip = node.HasAttribute(L"SkipError");
    bool bAct = DoRegMove(node.GetAttributeValue(L"Key"),
                          node.GetAttributeValue(L"Value"),
                          node.GetAttributeValue(L"NewKey"),
                          node.GetAttributeValue(L"NewValue"),
                          bW64, bSkip);
    if (bW64All)
    {
        bool bAct2 = DoRegMove(node.GetAttributeValue(L"Key"),
                               node.GetAttributeValue(L"Value"),
                               node.GetAttributeValue(L"NewKey"),
                               node.GetAttributeValue(L"NewValue"),
                               true, bSkip);
        bAct = bAct && bAct2;
    }
    if (bAct) return EActResult::ACT_OK;
    return bSkip ? EActResult::ACT_SKIP : EActResult::ACT_FAIL;
}

CDataEngine::EActResult CDataEngine::ActImportReg(CMarkupNode node, TActCtx& ctx)
{
    const wchar_t* pszFile = node.GetAttributeValue(L"File");
    if (!pszFile || !*pszFile) { ctx.Skip(); return EActResult::ACT_SKIP; }
    CDuiString sReg = ResolveExeRelative(pszFile, L"\\Resources\\");
    bool b32 = node.HasAttribute(L"Wow64")
        && lstrcmpiW(node.GetAttributeValue(L"Wow64"), L"False") != 0;
    // [P1-收尾] 零 reg.exe：CRegCleanEngine 原生解析 .reg 并写入
    // （b32=true → KEY_WOW64_32KEY，等价 reg /reg:32）
    bool bAct = CRegCleanEngine::ImportRegFile(sReg.GetData(), b32) == 0;
    return bAct ? EActResult::ACT_OK : EActResult::ACT_FAIL;
}

// ============================================================
// [R9-02] DoRegWrite 拆分一：位运算（Operator | & ^ !）分支。
// 成功/失败均返回，不关闭 hKey（由门面 DoRegWrite 统一关闭）。
// ============================================================
bool CDataEngine::DoRegWriteBitOp(HKEY hKey, const wchar_t* pszValue, const wchar_t* pszType,
                                  const wchar_t* pszData, const wchar_t* pszMask, wchar_t chOp)
{
    // Operator="|"/"&"/"^"/"!" : bit-mask modify.
    // [修复 P1-OP] 原版 Data.xml 契约（28 条规则实证，无一条带 Msk）：
    //   <RegWrite Operator="|" Data="100000"/>  → 现值 |= 0x100000（置位）
    //   <RegWrite Operator="&" Data="FFEFFFFF"/> → 现值 &= 0xFFEFFFFF（清位）
    // Data 本身就是操作数；值不存在时现值按 0 处理（OR 后=操作数，AND 后=0）。
    // Msk 为历史兼容属性，存在时优先作为操作数。DWORD 必须按 32 位数值解析
    // （ParseDwordData），旧代码把 hex "100000" 当字节流解成 3 字节且空 mask
    // 直接 return false，导致 28 条规则 100% 失败（此电脑文件夹显隐等）。
    DWORD dwRegType = REG_DWORD;
    if (pszType)
    {
        if (lstrcmpiW(pszType, L"REG_BINARY") == 0) dwRegType = REG_BINARY;
        else if (lstrcmpiW(pszType, L"REG_SZ") == 0) dwRegType = REG_SZ;
        else if (lstrcmpiW(pszType, L"REG_EXPAND_SZ") == 0) dwRegType = REG_EXPAND_SZ;
    }

    // 1) 取当前值（若存在）；不存在时 DWORD 以 0 为基，其余类型留空待对齐
    std::vector<BYTE> curBytes, maskBytes;
    if (::RegQueryValueEx(hKey, pszValue, nullptr, &dwRegType,
                          nullptr, nullptr) != ERROR_SUCCESS)
    {
        if (dwRegType == REG_DWORD) curBytes.assign(4, 0);
    }
    else
    {
        DWORD dwSize = 0;
        if (::RegQueryValueEx(hKey, pszValue, nullptr, &dwRegType,
                              nullptr, &dwSize) != ERROR_SUCCESS || dwSize == 0)
        {
            if (dwRegType == REG_DWORD) curBytes.assign(4, 0);
            else return false;
        }
        else
        {
            curBytes.resize(dwSize);
            if (::RegQueryValueEx(hKey, pszValue, nullptr, nullptr,
                                  curBytes.data(), &dwSize) != ERROR_SUCCESS)
                return false;
        }
    }

    // 2) 解析操作数：Msk 优先，否则 Data（原版契约）
    const wchar_t* pszOperand = (pszMask && *pszMask) ? pszMask : pszData;
    if (chOp != L'!')
    {
        if (!pszOperand || !*pszOperand)
            return false;
        if (dwRegType == REG_DWORD)
        {
            DWORD dwMask = ParseDwordData(pszOperand);
            maskBytes.resize(4);
            memcpy(maskBytes.data(), &dwMask, 4);
        }
        else
        {
            if (!DecodeRegData(pszOperand, dwRegType, maskBytes) || maskBytes.empty())
                return false;
        }
        // 现值短于操作数（二进制值缺失/较短）时高位补 0
        if (curBytes.size() < maskBytes.size())
            curBytes.resize(maskBytes.size(), 0);
    }

    // 3) 执行位操作
    if (!ApplyBitOp(chOp, curBytes, maskBytes))
        return false;

    // 4) 写回
    return ::RegSetValueEx(hKey, pszValue, 0, dwRegType,
                           curBytes.empty() ? nullptr : curBytes.data(),
                           static_cast<DWORD>(curBytes.size())) == ERROR_SUCCESS;
}

// ============================================================
// [R9-02] DoRegWrite 拆分二：普通写分支（DWORD/SZ/EXPAND_SZ/BINARY，
// 含 REG_BINARY 写入后反序列化回读校验 A2）。
// ============================================================
bool CDataEngine::DoRegWritePlain(HKEY hKey, const wchar_t* pszValue,
                                  const wchar_t* pszType, const wchar_t* pszData)
{
    bool bOk = false;
    if (!pszType || lstrcmpiW(pszType, L"REG_DWORD") == 0)
    {
        if (!pszType)
        {
            // 无 Type 且无 Data（隐藏“此电脑”命名空间文件夹）：
            // 按 Dism++ 语义写一个空 REG_SZ 到（默认）值。
            const wchar_t* pszEmpty = L"";
            bOk = ::RegSetValueEx(hKey, pszValue, 0, REG_SZ,
                                  (const BYTE*)pszEmpty,
                                  sizeof(wchar_t)) == ERROR_SUCCESS;
        }
        else
        {
            DWORD dw = ParseDwordData(pszData);
            bOk = ::RegSetValueEx(hKey, pszValue, 0, REG_DWORD,
                                  (BYTE*)&dw, sizeof(dw)) == ERROR_SUCCESS;
        }
    }
    else if (lstrcmpiW(pszType, L"REG_SZ") == 0
             || lstrcmpiW(pszType, L"REG_EXPAND_SZ") == 0)
    {
        // pszData 允许为空（写空串），缺属性时同样按空串处理，杜绝 nullptr 解引用
        const wchar_t* pszStr = pszData ? pszData : L"";
        DWORD dwType = (lstrcmpiW(pszType, L"REG_EXPAND_SZ") == 0) ? REG_EXPAND_SZ : REG_SZ;
        bOk = ::RegSetValueEx(hKey, pszValue, 0, dwType,
                              reinterpret_cast<const BYTE*>(pszStr),
                              (DWORD)((wcslen(pszStr) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    }
    else if (lstrcmpiW(pszType, L"REG_BINARY") == 0)
    {
        // Data 为十六进制字节串，如 "00000000"；#前缀为 base64
        std::vector<BYTE> bin;
        if (pszData)
        {
            if (!DecodeRegData(pszData, REG_BINARY, bin))
                return false;
        }
        bOk = ::RegSetValueEx(hKey, pszValue, 0, REG_BINARY,
                              bin.empty() ? nullptr : bin.data(),
                              static_cast<DWORD>(bin.size())) == ERROR_SUCCESS;

        // A2: REG_BINARY 反序列化回读校验。写入后立即 RegQueryValueEx，
        // 与写入字节按字节对比。不匹配说明写入路径存在编码错乱或值被覆盖，
        // 必须在返回前发现。
        if (bOk && !bin.empty())
        {
            DWORD dwRt = 0, dwSz = 0;
            if (::RegQueryValueEx(hKey, pszValue, nullptr, &dwRt, nullptr, &dwSz) == ERROR_SUCCESS)
            {
                if (dwRt != REG_BINARY || dwSz != static_cast<DWORD>(bin.size()))
                {
                    bOk = false;   // 类型或长度不符
                }
                else
                {
                    std::vector<BYTE> rb;
                    rb.resize(dwSz);
                    if (::RegQueryValueEx(hKey, pszValue, nullptr, nullptr,
                                          rb.data(), &dwSz) != ERROR_SUCCESS)
                        bOk = false;
                    else
                    {
                        for (size_t k = 0; k < bin.size(); ++k)
                            if (rb[k] != bin[k]) { bOk = false; break; }
                    }
                }
            }
            // 回读失败不覆盖原 bOk；只有真正回读成功且内容不匹配时才判定失败。
        }
    }
    return bOk;
}

// ============================================================
// [R9-02] DoRegWrite 拆分三：SearchboxTaskbarMode 的 Cache 补写后处理。
// 仅在写入成功、值名为 SearchboxTaskbarMode、DWORD 模式且 Mode 为 0/1 时，
// 若 Cache 缺失则补写 SearchboxTaskbarModeCache=1（逐字搬迁自旧后处理段）。
// ============================================================
void CDataEngine::DoRegWriteSearchboxCache(HKEY hKey, const wchar_t* pszValue,
                                           const wchar_t* pszType, const wchar_t* pszData)
{
    if (!pszValue ||
        lstrcmpiW(pszValue, L"SearchboxTaskbarMode") != 0
        || (pszType && lstrcmpiW(pszType, L"REG_DWORD") != 0))
        return;

    DWORD dwMode = ParseDwordData(pszData ? pszData : L"");
    if (dwMode != 0 && dwMode != 1) return;

    DWORD dwCacheType = REG_DWORD, dwCacheSize = 0;
    LONG lrQ = ::RegQueryValueEx(hKey, L"SearchboxTaskbarModeCache",
                                 nullptr, &dwCacheType, nullptr, &dwCacheSize);
    if (lrQ != ERROR_SUCCESS)
    {
        DWORD dwOne = 1;
        // [R128-A] 缓存键写入失败即任务栏搜索框形态未生效，留痕不中断
        if (::RegSetValueEx(hKey, L"SearchboxTaskbarModeCache", 0,
                        REG_DWORD, (BYTE*)&dwOne, sizeof(dwOne)) != ERROR_SUCCESS)
            ::OutputDebugStringW(L"[R128][reg] SearchboxTaskbarModeCache 写入失败\r\n");
    }
}

// ============================================================
// [R9-02] DoRegWrite 门面：开键（位运算/查询场景申请 KEY_QUERY_VALUE）
// → 位运算/普通写二选一 → Searchbox 缓存后处理 → 统一关闭。
// ============================================================
bool CDataEngine::DoRegWrite(const wchar_t* pszKey, const wchar_t* pszValue,
                             const wchar_t* pszType, const wchar_t* pszData,
                             bool bWow64, const wchar_t* pszOperator, const wchar_t* pszMask)
{
    HKEY hKey = nullptr;
    // [B4] 位运算（Operator）需要读现值，一并申请 KEY_QUERY_VALUE，
    // 否则 RegQueryValueEx 返回 ACCESS_DENIED 导致 28 个掩码规则失败。
    // SearchboxTaskbarMode 的 Cache 补写同样需要查询权限。
    REGSAM samWrite = KEY_SET_VALUE
                    | ((pszOperator && *pszOperator) ? KEY_QUERY_VALUE : 0)
                    | ((pszValue && lstrcmpiW(pszValue, L"SearchboxTaskbarMode") == 0)
                       ? KEY_QUERY_VALUE : 0);
    if (!OpenRegKey(pszKey, samWrite, hKey, true, bWow64))
        return false;

    Wow64Scope wow64Scope(hKey);
    RegKeyGuard _gWrite(hKey);

    bool bOk = false;
    if (pszOperator && *pszOperator
        && (*pszOperator == L'|' || *pszOperator == L'&'
         || *pszOperator == L'^' || *pszOperator == L'!'))
    {
        bOk = DoRegWriteBitOp(hKey, pszValue, pszType, pszData,
                              pszMask, *pszOperator);
    }
    else
    {
        bOk = DoRegWritePlain(hKey, pszValue, pszType, pszData);
    }

    if (bOk)
        DoRegWriteSearchboxCache(hKey, pszValue, pszType, pszData);

    return bOk;
}

bool CDataEngine::DoRegDelete(const wchar_t* pszKey, const wchar_t* pszValue,
                              bool bWow64, bool bSkipError)
{
    bool bOk = false;

    if (!pszValue || !*pszValue)
    {
        // 删除整键：必须沿 Wow64 指定的注册表视图操作，否则 32 位规则
        // 会误删 64 位视图下的同名键。
        HKEY hRoot = nullptr; CDuiString strSub;
        // [P0] 离线会话：wow 感知重映射后再解析（ParseRegPath 仅做 64 位映射）
        CDuiString sMapped = RemapOfflinePath(pszKey, bWow64);
        if (!ParseRegPath(sMapped, hRoot, strSub)) return bSkipError;

        REGSAM samWow = bWow64 ? KEY_WOW64_32KEY
                     : (hRoot == HKEY_LOCAL_MACHINE || hRoot == HKEY_CLASSES_ROOT)
                         ? KEY_WOW64_64KEY : 0;

        HKEY hKey = nullptr;
        REGSAM sam = DELETE | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | samWow;
        LONG lr = ::RegOpenKeyEx(hRoot, strSub.GetData(), 0, sam, &hKey);
        if (lr != ERROR_SUCCESS)
            return bSkipError;   // 键不存在：按 SkipError 忽略

        Wow64Scope wow64Scope(hKey);
        RegKeyGuard _gDelKey(hKey);

        // 先在正确视图上清空键内容，再删除空壳键
        ::RegDeleteTree(hKey, nullptr);

        LONG ld = ::RegDeleteKeyEx(hRoot, strSub.GetData(), samWow, 0);
        // 64 位系统上 64 位键 RegDeleteKeyEx 传 0 即可；删除失败（如仍有子键）
        // 再尝试一次不带视图标志的删除
        if (ld != ERROR_SUCCESS && samWow != 0)
            ld = ::RegDeleteKeyEx(hRoot, strSub.GetData(), 0, 0);
        bOk = (ld == ERROR_SUCCESS);
    }
    else
    {
        HKEY hKey = nullptr;
        if (!OpenRegKey(pszKey, KEY_SET_VALUE, hKey, false, bWow64))
            return bSkipError;
        Wow64Scope wow64Scope(hKey);
        RegKeyGuard _gDelVal(hKey);
        bOk = ::RegDeleteValue(hKey, pszValue) == ERROR_SUCCESS;
    }
    return bOk || bSkipError;
}

bool CDataEngine::DoRegMove(const wchar_t* pszSrcKey, const wchar_t* pszSrcValue,
                            const wchar_t* pszDstKey, const wchar_t* pszDstValue,
                            bool bWow64, bool bSkipError)
{
    HKEY hSrc = nullptr;
    if (!OpenRegKey(pszSrcKey, KEY_READ, hSrc, false, bWow64))
        return bSkipError;

    Wow64Scope wow64ScopeSrc(hSrc);
    RegKeyGuard _gSrc(hSrc);

    bool bOk = false;

    if (!pszSrcValue || !*pszSrcValue)
    {
        // ------------------------------------------------------------
        // 键级移动（Value 为空）：枚举源键下全部值复制到目标键，
        // 再删除整个源键。Dism++ 右键菜单类规则大量使用此形式。
        // ------------------------------------------------------------
        DWORD cValues = 0, cchMaxName = 0, cbMaxData = 0;
        if (::RegQueryInfoKey(hSrc, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              &cValues, &cchMaxName, &cbMaxData,
                              nullptr, nullptr) != ERROR_SUCCESS)
        {
            return bSkipError;
        }

        HKEY hDst = nullptr;
        if (!OpenRegKey(pszDstKey, KEY_SET_VALUE, hDst, true, bWow64))
        {
            return bSkipError;
        }
        Wow64Scope wow64ScopeDst(hDst);
        RegKeyGuard _gDst(hDst);

        std::vector<wchar_t> vName(cchMaxName + 2, 0);
        std::vector<BYTE>  vData(cbMaxData + 16, 0);
        bool bAllCopied = true;
        for (DWORD i = 0; i < cValues; ++i)
        {
            DWORD cchName = static_cast<DWORD>(vName.size());
            DWORD cbData  = static_cast<DWORD>(vData.size());
            DWORD dwType  = 0;
            LONG l = ::RegEnumValue(hSrc, i, vName.data(), &cchName, nullptr,
                                    &dwType, vData.data(), &cbData);
            if (l == ERROR_MORE_DATA) { bAllCopied = false; break; }
            if (l != ERROR_SUCCESS) { bAllCopied = false; continue; }
            if (::RegSetValueEx(hDst, vName.data(), 0, dwType,
                                vData.data(), cbData) != ERROR_SUCCESS)
                bAllCopied = false;
        }

        if (!bAllCopied) return bSkipError;   // 复制不完整时不删源，避免数据丢失

        // 复制成功后删除源键（沿用 DoRegDelete 的视图安全路径）
        bOk = DoRegDelete(pszSrcKey, nullptr, bWow64, false);
        return bOk || bSkipError;
    }

    // ------------------------------------------------------------
    // 值级移动：复制值 -> 删除源值（此前只复制不删，两个值共存）
    // ------------------------------------------------------------
    DWORD dwType = 0, dwSize = 0;
    if (::RegQueryValueEx(hSrc, pszSrcValue, nullptr, &dwType, nullptr, &dwSize) != ERROR_SUCCESS)
    {
        return bSkipError;
    }
    std::vector<BYTE> data(dwSize, 0);
    if (::RegQueryValueEx(hSrc, pszSrcValue, nullptr, nullptr, data.data(), &dwSize) != ERROR_SUCCESS)
    {
        return bSkipError;
    }

    HKEY hDst = nullptr;
    if (!OpenRegKey(pszDstKey, KEY_SET_VALUE, hDst, true, bWow64))
        return bSkipError;
    Wow64Scope wow64ScopeDst(hDst);
    RegKeyGuard _gDst2(hDst);
    bool bCopied = ::RegSetValueEx(hDst, pszDstValue, 0, dwType,
                                   data.data(), dwSize) == ERROR_SUCCESS;
    if (!bCopied) return bSkipError;   // 复制失败不删源

    HKEY hSrc2 = nullptr;
    if (OpenRegKey(pszSrcKey, KEY_SET_VALUE, hSrc2, false, bWow64))
    {
        Wow64Scope wow64ScopeSrc2(hSrc2);
        RegKeyGuard _gSrc2(hSrc2);
        bOk = ::RegDeleteValue(hSrc2, pszSrcValue) == ERROR_SUCCESS;
    }
    return bOk || bSkipError;
}

// ============================================================
// [R9-03] 注册表值编解码（逐字搬迁自 DataEngine.cpp，逻辑零改动）：
// ParseDwordData / DecodeRegData / ApplyBitOp 及其 4 个文件局部
// hex 解析辅助。Base64Decode 仍是 CDataEngine 成员（File 域亦用），
// 跨 TU 直接调用。
// ============================================================

// REG_DWORD 数据解析：纯十进制数字按 10 进制（"10"->10），
// 含 A-F/a-F 的按 16 进制（"FFEFFFFF"/"A"->10），与 Dism++ 规则书写一致。
DWORD CDataEngine::ParseDwordData(const wchar_t* pszData)
{
    if (!pszData || !*pszData) return 0;

    // 纯十进制（允许前导空格/符号位之外不做宽容）按 10 进制；
    // 一旦含 A-F/a-F 即按 16 进制。这样 "10"->10、"A"->10、
    // "FFEFFFFF"->4293918719，均与 Dism++ 规则书写意图一致。
    bool bAllDec = true;
    for (const wchar_t* p = pszData; *p; ++p)
    {
        if (*p < L'0' || *p > L'9') { bAllDec = false; break; }
    }
    return static_cast<DWORD>(wcstoul(pszData, nullptr, bAllDec ? 10 : 16));
}

// 判断字符串是否全是 hex 字符
static bool IsHexStr(const wchar_t* psz)
{
    if (!psz || !*psz) return false;
    for (; *psz; ++psz)
        if (!(_istdigit(*psz) ||
              (_istalpha(*psz) && std::towlower(*psz) >= L'a' && std::towlower(*psz) <= L'f')))
            return false;
    return true;
}

// A1: 解析 "0xNN 0xNN 0xNN" 或 "0xNN0xNN0xNN" 逐字节带前缀形式。
// 支持前缀之间无分隔（部分导出器不加空格）。任何位置的非法字符立即返回空。
static std::vector<BYTE> DecodeRegHexWithPrefix(const wchar_t* psz)
{
    std::vector<BYTE> out;
    const wchar_t* p = psz;
    while (*p)
    {
        // 跳过空白
        while (*p == L' ' || *p == L'\t') ++p;
        if (!*p) break;
        if (p[0] != L'0' || (p[1] != L'x' && p[1] != L'X'))
            return std::vector<BYTE>();
        p += 2;
        if (!*p || !* (p+1))
            return std::vector<BYTE>();
        std::wstring sHex(p, 2);
        if (!_istdigit(sHex[0]) && !(_istalpha(sHex[0]) && std::towlower(sHex[0]) <= L'f'))
            return std::vector<BYTE>();
        if (!_istdigit(sHex[1]) && !(_istalpha(sHex[1]) && std::towlower(sHex[1]) <= L'f'))
            return std::vector<BYTE>();
        out.push_back(static_cast<BYTE>(wcstoul(sHex.c_str(), nullptr, 16)));
        p += 2;
    }
    return out;
}

// A1: 解析 "\xNN\xNN..." 转义形式。任意非转义字符立即返回空。
static std::vector<BYTE> DecodeRegHexEscape(const wchar_t* psz)
{
    std::vector<BYTE> out;
    const wchar_t* p = psz;
    while (*p)
    {
        if (p[0] != L'\\' || p[1] != L'x')
            return std::vector<BYTE>();
        p += 2;
        if (!*p || !*(p+1))
            return std::vector<BYTE>();
        std::wstring sHex(p, 2);
        if (!_istdigit(sHex[0]) && !(_istalpha(sHex[0]) && std::towlower(sHex[0]) <= L'f'))
            return std::vector<BYTE>();
        if (!_istdigit(sHex[1]) && !(_istalpha(sHex[1]) && std::towlower(sHex[1]) <= L'f'))
            return std::vector<BYTE>();
        out.push_back(static_cast<BYTE>(wcstoul(sHex.c_str(), nullptr, 16)));
        p += 2;
    }
    return out;
}

// A1: 空格/制表符分隔形式（允许每字节带或不带 0x 前缀）。
// [R125-4] 解析单个空格分隔的字节 token，支持 0xHH / HH / H / 十进制 0-255。
//  任一位非法字符即返回 nullopt（调用方据此整段作废，保持原提前返回语义）。
static std::optional<BYTE> ParseRegByteToken(const std::wstring& tok)
{
    // 十六进制位校验：数字，或 A-F/a-f
    const auto isHex = [](wchar_t c) {
        return _istdigit(c) != 0 || (_istalpha(c) != 0 && std::towlower(c) <= L'f');
    };
    const auto hexVal = [](const std::wstring& w) {
    std::string a; a.reserve(w.size());
    for (wchar_t c : w) a.push_back(static_cast<char>(c));
        return static_cast<BYTE>(std::stol(a, nullptr, 16));
    };

    // 0xHH（恰好 4 字符）
    if (tok.size() == 4 && tok[0] == L'0' && (tok[1] == L'x' || tok[1] == L'X'))
    {
        const std::wstring h = tok.substr(2, 2);
        if (!isHex(h[0]) || !isHex(h[1])) return std::nullopt;
        return hexVal(h);
    }
    // HH（恰好 2 个十六进制位）
    if (tok.size() == 2)
    {
        if (!isHex(tok[0]) || !isHex(tok[1])) return std::nullopt;
        return hexVal(tok);
    }
    // 单个十进制数字
    if (tok.size() == 1 && _istdigit(tok[0]))
        return static_cast<BYTE>(tok[0] - L'0');

    // 十进制整数，限 0-255
    for (wchar_t c : tok)
        if (_istdigit(c) == 0) return std::nullopt;
    if (tok.empty()) return std::nullopt;
    std::string a; a.reserve(tok.size());
    for (wchar_t c : tok) a.push_back(static_cast<char>(c));
    const int v = static_cast<int>(std::stoi(a));
    if (v < 0 || v > 255) return std::nullopt;
    return static_cast<BYTE>(v);
}

static std::vector<BYTE> DecodeRegSpaceSeparated(const wchar_t* psz)
{
    std::vector<BYTE> out;
    std::wstring s = psz;
    // 归一化：把 \t 也替换成空格
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i] == L'\t') s[i] = L' ';

    size_t i = 0;
    while (i < s.size())
    {
        while (i < s.size() && s[i] == L' ') ++i;
        if (i >= s.size()) break;
        size_t j = i;
        while (j < s.size() && s[j] != L' ') ++j;
        std::wstring tok = s.substr(i, j - i);
        i = j;

        // 单 token 解析：失败即整段作废（与原逻辑一致）
        const auto optB = ParseRegByteToken(tok);
        if (!optB.has_value()) return std::vector<BYTE>();
        out.push_back(optB.value());
    }
    return out;
}

// [R3] Data/Msk 解码（"#base64" / hex / DWORD 十进制）
bool CDataEngine::DecodeRegData(const wchar_t* pszIn, DWORD dwRegType,
                                std::vector<BYTE>& outBin)
{
    outBin.clear();
    if (!pszIn || !*pszIn) return true;   // 空 Data 合法（写零/空串）

    // # 前缀 → base64
    if (*pszIn == L'#')
    {
        return Base64Decode(pszIn + 1, outBin);
    }

    size_t nLen = wcslen(pszIn);

    // DWORD 语义：允许十进制
    if (dwRegType == REG_DWORD || dwRegType == 0)
    {
        if (!IsHexStr(pszIn))
        {
            // 十进制路径
            DWORD dw = static_cast<DWORD>(_ttoi64(pszIn));
            outBin.assign(4, 0);
            outBin[0] = static_cast<BYTE>(dw);
            outBin[1] = static_cast<BYTE>(dw >> 8);
            outBin[2] = static_cast<BYTE>(dw >> 16);
            outBin[3] = static_cast<BYTE>(dw >> 24);
            return true;
        }
    }

    // A1: 检测 0x / 0X 前缀的"每字节带前缀"形式（例：0x01 0x02 0x03）。
    // 该形式在注册表编辑器复制 REG_BINARY 时出现，之前无法解析。
    if ((nLen >= 3 && pszIn[0] == L'0' && (pszIn[1] == L'x' || pszIn[1] == L'X'))
        || (nLen >= 4 && pszIn[1] == L'x' && pszIn[0] == L'0'))
    {
        // 尝试解析 "0xNN[ 0xNN[ 0xNN ...]]" 逐字节形式
        outBin = DecodeRegHexWithPrefix(pszIn);
        if (!outBin.empty()) return true;
        // 前缀存在但格式非法：不 fallback 到整体 hex，让上层看到解析失败
        return false;
    }

    // A1: 检测 \xNN 转义形式（例：\x01\x02\xFF）
    if (nLen >= 3 && pszIn[0] == L'\\' && pszIn[1] == L'x')
    {
        outBin = DecodeRegHexEscape(pszIn);
        return !outBin.empty();
    }

    // A1: 空格分隔形式（例："01 02 FF" 或 "0x01 0x02 0xFF"）
    if (wcschr(pszIn, L' ') || wcschr(pszIn, L'\t'))
    {
        outBin = DecodeRegSpaceSeparated(pszIn);
        if (!outBin.empty()) return true;
    }

    // hex 路径（偶数字节；奇数长度 → 高位补 0，与老逻辑保持"忽略奇数尾巴"一致）
    size_t nEffLen = (nLen % 2 == 0) ? nLen : (nLen - 1);
    if (!IsHexStr(pszIn))
        return false;
    outBin.resize(nEffLen / 2);
    for (size_t i = 0; i < nEffLen; i += 2)
    {
        std::wstring sHex(pszIn + i, 2);
        outBin[i / 2] = static_cast<BYTE>(wcstoul(sHex.c_str(), nullptr, 16));
    }
    return true;
}

// [R3] 位操作（| & xor not）
bool CDataEngine::ApplyBitOp(wchar_t chOp,
                             std::vector<BYTE>& curBytes,
                             const std::vector<BYTE>& maskBytes)
{
    if (chOp == '!')   // NOT
    {
        for (size_t i = 0; i < curBytes.size(); ++i)
            curBytes[i] = (BYTE)~curBytes[i];
        return true;
    }
    if (maskBytes.empty()) return false;   // 缺 mask 且不是 NOT

    // [修复 P1-OP] 以操作数长度为界，旧代码按 curBytes.size() 索引
    // maskBytes[i]，二进制现值长于操作数时越界读。
    size_t n = maskBytes.size();
    if (n == 0 || curBytes.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
    {
        BYTE m = maskBytes[i];
        switch (chOp)
        {
            case '|': curBytes[i] |= m; break;
            case '&': curBytes[i] &= m; break;
            case '^': curBytes[i] ^= m; break;
            default:  return false;
        }
    }
    return true;
}

// ============================================================
// [R9-03b] 离线蜂巢会话 + 注册表底层原语（逐字搬迁自 DataEngine.cpp）
// ============================================================

// ============================================================
// [R10-02] WinPE 离线蜂巢会话已抽为独立组件 COfflineSession
// （OfflineSession.h/.cpp）：服务层直接调用，不再静态耦合
// CDataEngine。下列 6 个 CDataEngine 静态 API 保留为薄转发，
// 页面/壳与本 TU 内部既有调用链零改动，行为逐字不变。
// ============================================================
bool CDataEngine::BeginOfflineSession(const wchar_t* pszImageRoot)
{ return COfflineSession::Begin(pszImageRoot); }

void CDataEngine::EndOfflineSession()
{ COfflineSession::End(); }

bool CDataEngine::IsOfflineSession()
{ return COfflineSession::IsActive(); }

CDuiString CDataEngine::GetOfflineImageRoot()
{ return COfflineSession::GetImageRoot(); }

CDuiString CDataEngine::RemapOfflinePath(const wchar_t* pszFull, bool bWow32)
{ return COfflineSession::RemapPath(pszFull, bWow32); }

bool CDataEngine::ParseRegPath(const wchar_t* pszFull, HKEY& hRoot, CDuiString& strSubKey)
{ return COfflineSession::ParseRegPath(pszFull, hRoot, strSubKey); }

bool CDataEngine::OpenRegKey(const wchar_t* pszFull, REGSAM samDesired, HKEY& hKey,
                             bool bCreate, bool bWow64)
{
    hKey = nullptr;
    HKEY hRoot = nullptr;
    CDuiString strSub;
    // [P0] 离线会话：先按本调用的 Wow64 视图把逻辑路径重定向到挂载蜂巢；
    // 含 DPP_OFF_ 的路径在 RemapOfflinePath 内透传。
    CDuiString sMapped = RemapOfflinePath(pszFull, bWow64);
    if (!ParseRegPath(sMapped, hRoot, strSub)) return false;

    REGSAM sam = samDesired;
    if (bWow64)
        sam |= KEY_WOW64_32KEY;                       // Wow64="True"
    else if (hRoot == HKEY_LOCAL_MACHINE || hRoot == HKEY_CLASSES_ROOT)
        sam |= KEY_WOW64_64KEY;

    LONG l;
    if (bCreate)
        l = ::RegCreateKeyEx(hRoot, strSub.GetData(), 0, nullptr, 0, sam, nullptr, &hKey, nullptr);
    else
        l = ::RegOpenKeyEx(hRoot, strSub.GetData(), 0, sam, &hKey);
    return l == ERROR_SUCCESS;
}

bool CDataEngine::RegValueEqual(HKEY hKey, const wchar_t* pszValue,
                                const wchar_t* pszType, const wchar_t* pszData,
                                const wchar_t* pszCompare)
{
    DWORD dwType = 0, dwSize = 0;
    if (::RegQueryValueEx(hKey, pszValue, nullptr, &dwType, nullptr, &dwSize) != ERROR_SUCCESS)
        return false;

    // Compare="N==" : strncmp-style prefix equality on a string value
    if (pszCompare && lstrcmpiW(pszCompare, L"N==") == 0)
    {
        if (dwType != REG_SZ && dwType != REG_EXPAND_SZ) return false;
        std::vector<wchar_t> buf(dwSize / sizeof(wchar_t) + 2, 0);
        if (::RegQueryValueEx(hKey, pszValue, nullptr, nullptr, reinterpret_cast<BYTE*>(buf.data()), &dwSize) != ERROR_SUCCESS)
            return false;
        CDuiString sVal = buf.data();
        return sVal.Find(pszData ? pszData : L"") == 0;
    }

    // Compare="&" : bit-mask test; Data is hex without 0x prefix
    if (pszCompare && lstrcmpW(pszCompare, L"&") == 0)
    {
        DWORD dwVal = 0, dwGot = sizeof(dwVal);
        if (::RegQueryValueEx(hKey, pszValue, nullptr, nullptr, (BYTE*)&dwVal, &dwGot) != ERROR_SUCCESS)
            return false;
        DWORD dwMask = static_cast<DWORD>(wcstoul(pszData ? pszData : L"0", nullptr, 16));
        return (dwVal & dwMask) != 0;
    }

    bool bIsString = (dwType == REG_SZ || dwType == REG_EXPAND_SZ);

    if (pszType)
    {
        DWORD wantType = 0;
        if (lstrcmpiW(pszType, L"REG_DWORD") == 0) wantType = REG_DWORD;
        else if (lstrcmpiW(pszType, L"REG_SZ") == 0) wantType = REG_SZ;
        else if (lstrcmpiW(pszType, L"REG_EXPAND_SZ") == 0) wantType = REG_EXPAND_SZ;
        else if (lstrcmpiW(pszType, L"REG_BINARY") == 0) wantType = REG_BINARY;
        if (wantType && dwType != wantType) return false;
    }

    if (!pszData) return true;

    if (dwType == REG_DWORD)
    {
        DWORD dwWant = ParseDwordData(pszData);
        DWORD dwVal = 0, dwGot = sizeof(dwVal);
        if (::RegQueryValueEx(hKey, pszValue, nullptr, nullptr, (BYTE*)&dwVal, &dwGot) != ERROR_SUCCESS)
            return false;
        return dwVal == dwWant;
    }
    if (bIsString)
    {
        std::vector<wchar_t> buf(dwSize / sizeof(wchar_t) + 2, 0);
        if (::RegQueryValueEx(hKey, pszValue, nullptr, nullptr, reinterpret_cast<BYTE*>(buf.data()), &dwSize) != ERROR_SUCCESS)
            return false;
        return lstrcmpiW(buf.data(), pszData) == 0;
    }
    return true;
}

// [R9-03b] RegCleanImport（逐字搬迁自 DataEngine.cpp）

// ============================================================
// [P1-收尾] 外部可执行文件执行 helper 已删除（原 reg.exe/dism.exe/pnputil.exe
// 兜底全部改为进程内原生 API）。保留 RegCleanImport 原生 .reg 导入包装。
// ============================================================

// [P1-收尾] 原生 .reg 导入：解析器在 RegCleanEngine，走本类 ParseRegPath/离线重定向。
bool CDataEngine::RegCleanImport(const wchar_t* pszRegFile, bool bWow64)
{
    return CRegCleanEngine::ImportRegFile(pszRegFile, bWow64) == 0;
}

// [R9-03b] RegCleanValueExists（逐字搬迁自 DataEngine.cpp）

// ============================================================
// [R79] RegCleanValueExists —— RegClean 扫描用"值是否存在"判定
// ------------------------------------------------------------
// bWow64=true 时走 KEY_WOW64_32KEY 视图。
// pszValue 为空时判定键本身是否存在。
// ============================================================
bool CDataEngine::RegCleanValueExists(const wchar_t* pszKey, const wchar_t* pszValue, bool bWow64)
{
    if (!pszKey || !*pszKey) return false;
    REGSAM sam = KEY_READ;
    if (bWow64) sam |= KEY_WOW64_32KEY;
    HKEY hKey = nullptr;
    if (!OpenRegKey(pszKey, sam, hKey, false, bWow64)) return false;
    RegKeyGuard _gVExists(hKey);
    bool bOk;
    if (pszValue && *pszValue)
    {
        DWORD dwType = 0;
        bOk = (::RegQueryValueEx(hKey, pszValue, nullptr, &dwType, nullptr, nullptr) == ERROR_SUCCESS);
    }
    else bOk = true;  // 键本身已由 OpenRegKey 打开成功
    return bOk;
}
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
