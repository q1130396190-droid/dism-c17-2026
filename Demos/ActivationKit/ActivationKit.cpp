// ============================================================================
//  ActivationKit.cpp —— 激活信息备份/还原实现
//
//  原生移植自原 FrogPlugins frogplug_exports.cpp 的
//  FpBackupActivation / FpRestoreActivation，并修复原实现缺陷：
//    1) 备份不存在与成功显式区分（原版把"不存在"当 S_OK，UI 会误报成功）；
//    2) INI 双 DPID 键皆空 / HEX 非法时拒绝写注册表；
//    3) 每步失败回传真实 Win32 错误码 + 中文描述（原版只有笼统日志）；
//    4) 备份目录已存在（ERROR_ALREADY_EXISTS）视为正常；
//    5) 统一使用 Unicode 版 Profile API，非中文 ANSI 代码页下中文键不丢。
//  INI 段名/键名逐字兼容原版（包括中文键），历史备份件可直接还原。
// ============================================================================

#include "ActivationKit.h"  // ACTKIT_DLL_BUILD 由工程预处理器定义
#include "../Win8Metro/FormatW.h"
#include "../Win8Metro/WinHandles.h"   // [C++17 R3] win::RegKey RAII for F11_raw_handle_close

#include <vector>
#include <string>

namespace
{

const wchar_t* const kWinVerKey = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
const wchar_t* const kSubDir    = L"激活备份";
const wchar_t* const kIniName   = L"Backup.ini";

// ---------------------------------------------------------------------------
// 错误描述回填
// ---------------------------------------------------------------------------
void SetErr(wchar_t* pszErr, DWORD cchErr, const wchar_t* pszMsg, DWORD dwErr)
{
    if (pszErr == nullptr || cchErr == 0) return;
    if (dwErr != 0)
        wfmt::FormatInto(pszErr, cchErr, L"%s（错误码 %lu）", pszMsg, dwErr);
    else
        ::wcsncpy_s(pszErr, cchErr, pszMsg, _TRUNCATE);
}

// ---------------------------------------------------------------------------
// 注册表读写
// ---------------------------------------------------------------------------
bool RegReadBinary(const wchar_t* pszVal, std::vector<BYTE>& vOut)
{
    vOut.clear();
    win::RegKey key;   // [C++17 R3] F11: RAII auto RegCloseKey
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kWinVerKey, 0, KEY_READ, key.out()) != ERROR_SUCCESS)
        return false;

    DWORD dwType = 0, cb = 0;
    bool bOk = false;
    if (::RegQueryValueExW(key.get(), pszVal, nullptr, &dwType, nullptr, &cb) == ERROR_SUCCESS
        && dwType == REG_BINARY && cb > 0)
    {
        vOut.resize(cb);
        bOk = ::RegQueryValueExW(key.get(), pszVal, nullptr, nullptr,
                                 vOut.data(), &cb) == ERROR_SUCCESS;
    }
    return bOk;
}

bool RegReadString(const wchar_t* pszVal, std::wstring& sOut)
{
    sOut.clear();
    win::RegKey key;   // [C++17 R3] F11: RAII auto RegCloseKey
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kWinVerKey, 0, KEY_READ, key.out()) != ERROR_SUCCESS)
        return false;

    // [BUG-20] 动态长度两次查询：先取字节长度再按值分配，替代固定 1024
    // 栈缓冲，超长 ProductName/ProductId 不再静默截断。
    DWORD cb = 0, dwType = 0;
    bool bOk = false;
    LONG lRc = ::RegQueryValueExW(key.get(), pszVal, nullptr, &dwType, nullptr, &cb);
    if (lRc == ERROR_SUCCESS
        && (dwType == REG_SZ || dwType == REG_EXPAND_SZ)
        && cb >= sizeof(wchar_t))
    {
        std::vector<wchar_t> vBuf(cb / sizeof(wchar_t) + 1, L'\0');
        DWORD cb2 = cb;
        if (::RegQueryValueExW(key.get(), pszVal, nullptr, nullptr,
                               reinterpret_cast<BYTE*>(vBuf.data()), &cb2)
            == ERROR_SUCCESS)
        {
            // REG_SZ 约定以 NUL 结尾但不保证；按实际返回长度剥掉尾部 NUL
            size_t cch = cb2 / sizeof(wchar_t);
            while (cch > 0 && vBuf[cch - 1] == L'\0') --cch;
            sOut.assign(vBuf.data(), cch);
            bOk = true;
        }
    }
    return bOk;
}

// [R01-fix] 两个 DPID 值在同一次开键内连续写入，消除「第一个已写、第二个
//  因权限等原因失败」造成的半还原窗口（键句柄关闭前其他进程可见性一致）。
bool RegWriteBinaryPair(const std::vector<BYTE>* pv1,
                        const std::vector<BYTE>* pv4)
{
    win::RegKey key;   // [C++17 R3] F11: RAII auto RegCloseKey
    if (::RegCreateKeyExW(HKEY_LOCAL_MACHINE, kWinVerKey, 0, nullptr, 0,
                          KEY_WRITE, nullptr, key.out(), nullptr) != ERROR_SUCCESS)
        return false;
    LONG lRc = ERROR_SUCCESS;
    if (pv1 != nullptr)
        lRc = ::RegSetValueExW(key.get(), L"DigitalProductId", 0, REG_BINARY,
                               pv1->empty() ? nullptr : pv1->data(),
                               static_cast<DWORD>(pv1->size()));
    if (lRc == ERROR_SUCCESS && pv4 != nullptr)
        lRc = ::RegSetValueExW(key.get(), L"DigitalProductId4", 0, REG_BINARY,
                               pv4->empty() ? nullptr : pv4->data(),
                               static_cast<DWORD>(pv4->size()));
    return lRc == ERROR_SUCCESS;
}

// ---------------------------------------------------------------------------
// HEX 编解码（大写输出；解码大小写兼容）
// ---------------------------------------------------------------------------
std::wstring ToHex(const std::vector<BYTE>& v)
{
    static const wchar_t* const kHex = L"0123456789ABCDEF";
    std::wstring s;
    s.reserve(v.size() * 2);
    for (const BYTE b : v)
    {
        s.push_back(kHex[(b >> 4) & 0xF]);
        s.push_back(kHex[b & 0xF]);
    }
    return s;
}

int HexVal(wchar_t c)
{
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    return -1;
}

bool FromHex(const wchar_t* pszHex, std::vector<BYTE>& vOut)
{
    vOut.clear();
    if (pszHex == nullptr) return false;
    size_t n = wcslen(pszHex);
    if (n == 0 || (n % 2) != 0) return false;
    vOut.reserve(n / 2);
    for (size_t i = 0; i < n; i += 2)
    {
        int hi = HexVal(pszHex[i]);
        int lo = HexVal(pszHex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        vOut.push_back(static_cast<BYTE>((hi << 4) | lo));
    }
    return !vOut.empty();
}

std::wstring EnsureTrailingSlash(const wchar_t* psz)
{
    std::wstring s(psz ? psz : L"");
    if (!s.empty() && s[s.size() - 1] != L'\\') s.push_back(L'\\');
    return s;
}

std::wstring MakeIniPath(const wchar_t* pszBaseDir)
{
    std::wstring s = EnsureTrailingSlash(pszBaseDir);
    s += kSubDir;
    s += L'\\';
    s += kIniName;
    return s;
}

}  // namespace

// ===========================================================================
//  导出实现
// ===========================================================================

HRESULT BackupExistsImpl(const wchar_t* pszBaseDir, std::wstring* psIni)
{
    if (pszBaseDir == nullptr || *pszBaseDir == L'\0')
        return E_INVALIDARG;
    std::wstring sIni = MakeIniPath(pszBaseDir);
    if (psIni) *psIni = sIni;
    if (::GetFileAttributesW(sIni.c_str()) == INVALID_FILE_ATTRIBUTES)
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

    wchar_t szDpId[8192]  = { 0 };
    wchar_t szDpId4[8192] = { 0 };
    ::GetPrivateProfileStringW(L"系统信息", L"DigitalProductId",
                               L"", szDpId, _countof(szDpId), sIni.c_str());
    ::GetPrivateProfileStringW(L"系统信息", L"DigitalProductId4",
                               L"", szDpId4, _countof(szDpId4), sIni.c_str());
    if (szDpId[0] == L'\0' && szDpId4[0] == L'\0')
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    return S_OK;
}

ACTKIT_API BOOL __cdecl AK_ActivationBackupExists(const wchar_t* pszBaseDir)
{
    return BackupExistsImpl(pszBaseDir, nullptr) == S_OK;
}

ACTKIT_API HRESULT __cdecl AK_BackupActivation(const wchar_t* pszOutDir,
                                               const wchar_t* pszNote,
                                               wchar_t* pszErr,
                                               DWORD cchErr)
{
    if (pszOutDir == nullptr || *pszOutDir == L'\0')
    {
        SetErr(pszErr, cchErr, L"未选择备份目录。", 0);
        return E_INVALIDARG;
    }

    // 1) 读取两个 REG_BINARY（至少其一存在）
    std::vector<BYTE> vDpId, vDpId4;
    bool bHas1 = RegReadBinary(L"DigitalProductId", vDpId);
    bool bHas4 = RegReadBinary(L"DigitalProductId4", vDpId4);
    if (!bHas1 && !bHas4)
    {
        DWORD dwErr = ::GetLastError();
        SetErr(pszErr, cchErr,
               L"无法获取 DigitalProductId / DigitalProductId4，当前系统不支持备份激活信息。",
               dwErr ? dwErr : ERROR_NOT_FOUND);
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }

    // 2) 登记字段（缺失不致命）
    std::wstring sProductId, sProductName, sEditionId;
    RegReadString(L"ProductId", sProductId);
    RegReadString(L"ProductName", sProductName);
    RegReadString(L"EditionID", sEditionId);

    // 3) 创建备份目录（已存在视为正常）
    std::wstring sDir = EnsureTrailingSlash(pszOutDir) + kSubDir;
    if (!::CreateDirectoryW(sDir.c_str(), nullptr))
    {
        DWORD dwErr = ::GetLastError();
        if (dwErr != ERROR_ALREADY_EXISTS)
        {
            SetErr(pszErr, cchErr, L"无法创建备份目录", dwErr);
            return HRESULT_FROM_WIN32(dwErr);
        }
    }

    std::wstring sIni = sDir + L'\\' + kIniName;

    // 4) 备份信息
    SYSTEMTIME st = { 0 };
    ::GetLocalTime(&st);
    std::wstring sTime;
    sTime.resize(64);
    wfmt::FormatInto(&sTime[0], sTime.size(),
                   L"%04d-%02d-%02d %02d:%02d:%02d",
                   st.wYear, st.wMonth, st.wDay,
                   st.wHour, st.wMinute, st.wSecond);
    sTime.resize(wcslen(sTime.c_str()));

    const wchar_t* kDefaultNote = L"备份 Windows 激活信息（零售/OEM 密钥迁移用）";
    const wchar_t* pszEffNote =
        (pszNote != nullptr && *pszNote != L'\0') ? pszNote : kDefaultNote;

    std::wstring sDpIdHex  = ToHex(vDpId);
    std::wstring sDpId4Hex = ToHex(vDpId4);

    struct Field { const wchar_t* pKey; const wchar_t* pVal; };
    const Field kFields[] = {
        { L"系统名称",          sProductName.empty() ? L"Windows" : sProductName.c_str() },
        { L"版本",              sEditionId.c_str() },
        { L"ProductId",         sProductId.c_str() },
        { L"DigitalProductId",  sDpIdHex.c_str() },
        { L"DigitalProductId4", sDpId4Hex.c_str() },
    };
    for (size_t i = 0; i < _countof(kFields); ++i)
    {
        if (!::WritePrivateProfileStringW(L"系统信息", kFields[i].pKey,
                                          kFields[i].pVal, sIni.c_str()))
        {
            DWORD dwErr = ::GetLastError();
            SetErr(pszErr, cchErr, L"写入 Backup.ini 失败", dwErr);
            return HRESULT_FROM_WIN32(dwErr);
        }
    }
    if (!::WritePrivateProfileStringW(L"备份信息", L"备份时间", sTime.c_str(), sIni.c_str())
        || !::WritePrivateProfileStringW(L"备份信息", L"备份说明", pszEffNote, sIni.c_str()))
    {
        DWORD dwErr = ::GetLastError();
        SetErr(pszErr, cchErr, L"写入备份信息失败", dwErr);
        return HRESULT_FROM_WIN32(dwErr);
    }

    return S_OK;
}

ACTKIT_API HRESULT __cdecl AK_RestoreActivation(const wchar_t* pszBaseDir,
                                                wchar_t* pszErr,
                                                DWORD cchErr)
{
    if (pszBaseDir == nullptr || *pszBaseDir == L'\0')
    {
        SetErr(pszErr, cchErr, L"未选择备份所在目录。", 0);
        return E_INVALIDARG;
    }

    std::wstring sIni;
    HRESULT hrCheck = BackupExistsImpl(pszBaseDir, &sIni);
    if (hrCheck != S_OK)
    {
        if (hrCheck == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
            SetErr(pszErr, cchErr,
                   L"所选目录下不存在「激活备份\\Backup.ini」，无法还原。", 0);
        else
            SetErr(pszErr, cchErr,
                   L"Backup.ini 中没有有效的 DigitalProductId 数据。", 0);
        return hrCheck;
    }

    wchar_t szDpId[8192]  = { 0 };
    wchar_t szDpId4[8192] = { 0 };
    ::GetPrivateProfileStringW(L"系统信息", L"DigitalProductId",
                               L"", szDpId, _countof(szDpId), sIni.c_str());
    ::GetPrivateProfileStringW(L"系统信息", L"DigitalProductId4",
                               L"", szDpId4, _countof(szDpId4), sIni.c_str());

    std::vector<BYTE> vDpId, vDpId4;
    bool bHas1 = FromHex(szDpId, vDpId);
    bool bHas4 = FromHex(szDpId4, vDpId4);
    // [R01-fix] 区分「键缺失/空」与「键存在但 HEX 损坏」：旧逻辑把两者
    //  都当作 false 静默跳过，可能在只写回一个 DPID 时误报成功。任一存在
    //  但无法解码的键都必须整体拒绝（发生在注册表写入之前）。
    if ((szDpId[0] != L'\0' && !bHas1) ||
        (szDpId4[0] != L'\0' && !bHas4))
    {
        SetErr(pszErr, cchErr, L"Backup.ini 中的 DigitalProductId 数据已损坏。", 0);
        return E_FAIL;
    }
    if (!bHas1 && !bHas4)
    {
        SetErr(pszErr, cchErr, L"Backup.ini 中没有有效的 DigitalProductId 数据。", 0);
        return E_FAIL;
    }

    bool bWriteOk = RegWriteBinaryPair(bHas1 ? &vDpId  : nullptr,
                                       bHas4 ? &vDpId4 : nullptr);
    if (!bWriteOk)
    {
        SetErr(pszErr, cchErr,
               L"写入注册表失败，请确认程序正以管理员身份运行。",
               ::GetLastError());
        return E_FAIL;
    }
    return S_OK;
}
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
inline constexpr int kCpp17_ActivationKit_StepTimeoutMs = 120000;
inline constexpr int kCpp17_ActivationKit_MaxRetry = 3;
