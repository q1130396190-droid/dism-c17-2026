#include "stdafx.h"

#include "MountLedgerService.h"



#include <shlwapi.h>
// [C++17 阶段2] 局部 RAII 守卫：注册表键（见 WinHandles.h 使用规范）
struct MLRegGuard
{
    HKEY k = nullptr;
    ~MLRegGuard() { if (k) ::RegCloseKey(k); }
    HKEY* out() { return &k; }
    void reset(HKEY n = nullptr) { if (k) ::RegCloseKey(k); k = n; }
    operator HKEY() const { return k; }
};

// ============================================================
// 实现：注册表记账（HKCU\Software\Dism++\）
// ============================================================

namespace DismLedger
{

namespace
{
const wchar_t* kRoot = L"Software\\Dism++";
const wchar_t* kImageFiles = L"Software\\Dism++\\ImageFiles";
const wchar_t* kMountPaths = L"Software\\Dism++\\MountPaths";
const wchar_t* kApplyPaths = L"Software\\Dism++\\ApplyPaths";

// MRU 添加：把新值插入开头，去重，最多保留 nMax 条
// [R136] pSetOverride：可选的写入函数覆盖，默认 nullptr 走真实 RegSetValueExW。
bool AddMruValue(const wchar_t* pszSubKey, const std::wstring& sVal, int nMax,
                 TSetValueFn pSetOverride = nullptr)
{
    std::vector<std::wstring> v;
    MLRegGuard hKey;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, pszSubKey, 0, KEY_READ, hKey.out()) == ERROR_SUCCESS)
    {
        DWORD dwIdx = 0;
        wchar_t szName[64] = {};
        for (;;)
        {
            DWORD cName = _countof(szName);
            LONG l = ::RegEnumValueW(hKey, dwIdx, szName, &cName, nullptr, nullptr, nullptr, nullptr);
            if (l != ERROR_SUCCESS) break;
            // 注册表值数据缓冲（单个 REG_SZ 值通常远小于 4KB）
            constexpr DWORD kRegValBufSize = 4096;
            wchar_t szVal[kRegValBufSize] = {};
            DWORD cVal = _countof(szVal);
            DWORD dwType = 0;
            if (::RegQueryValueExW(hKey, szName, nullptr, &dwType, reinterpret_cast<BYTE*>(szVal), &cVal) == ERROR_SUCCESS)
                v.push_back(szVal);
            ++dwIdx;
        }
        hKey.reset();  // 守卫复位：之后以 KEY_WRITE 重开同一键
    }

    // 去重 + 插入头部
    std::vector<std::wstring> out;
    out.push_back(sVal);
    for (const auto& x : v)
    {
        if (lstrcmpiW(x.c_str(), sVal.c_str()) == 0) continue;
        out.push_back(x);
        if (static_cast<int>(out.size()) >= nMax) break;
    }

    // 重写整个子键
    // [R128-A] 任一写入/删除失败则整体不成功（挂载台账需完整一致）
    bool bAllWritten = true;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, pszSubKey, 0, nullptr, 0,
                          KEY_WRITE, nullptr, hKey.out(), nullptr) != ERROR_SUCCESS)
        return false;

    for (int i = 0; i < static_cast<int>(out.size()); ++i)
    {
        // [C++17 阶段3][F13] wchar_t[64] -> std::wstring
        const size_t kCapName75 = 64;
        std::wstring sName(kCapName75, L'\0');
        int nName = wfmt::FormatInto(sName.data(), kCapName75, L"%d", i);
        if (nName >= 0) sName.resize(static_cast<size_t>(nName));
        // [R128-A] 记录写入失败：挂载台账是"最近挂载"的唯一来源，写丢了就对不上
        LONG lSet;
        if (pSetOverride)
        {
            // 注入路径：只判断"成功/失败"，不实际写注册表
            lSet = pSetOverride(hKey, sName.c_str(), out[i].c_str());
        }
        else
        {
            lSet = ::RegSetValueExW(hKey, sName.c_str(), 0, REG_SZ,
                         (const BYTE*)out[i].c_str(),
                         (DWORD)((out[i].size() + 1) * sizeof(wchar_t)));
        }
        if (lSet != ERROR_SUCCESS)
            bAllWritten = false;
    }
    // 删除多余项
    for (int i = static_cast<int>(out.size()); i < 64; ++i)
    {
        // [C++17 阶段3][F13] wchar_t[64] -> std::wstring
        const size_t kCapName84 = 64;
        std::wstring sName2(kCapName84, L'\0');
        int nName2 = wfmt::FormatInto(sName2.data(), kCapName84, L"%d", i);
        if (nName2 >= 0) sName2.resize(static_cast<size_t>(nName2));
        // 值本就不存在属正常，只有其它错误才算失败
        LONG lDelM = ::RegDeleteValueW(hKey, sName2.c_str());
        if (lDelM != ERROR_SUCCESS && lDelM != ERROR_FILE_NOT_FOUND)
            bAllWritten = false;
    }
    return bAllWritten;
}

bool ReadMulti(const wchar_t* pszSubKey, std::vector<std::wstring>& out)
{
    out.clear();
    MLRegGuard hKey;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, pszSubKey, 0, KEY_READ, hKey.out()) != ERROR_SUCCESS)
        return false;
    DWORD dwIdx = 0;
    wchar_t szName[64] = {};
    for (;;)
    {
        DWORD cName = _countof(szName);
        LONG l = ::RegEnumValueW(hKey, dwIdx, szName, &cName, nullptr, nullptr, nullptr, nullptr);
        if (l != ERROR_SUCCESS) break;
        // 注册表值数据缓冲（单个 REG_SZ 值通常远小于 4KB）
        constexpr DWORD kRegValBufSize = 4096;
        wchar_t szVal[kRegValBufSize] = {};
        DWORD cVal = _countof(szVal);
        DWORD dwType = 0;
        if (::RegQueryValueExW(hKey, szName, nullptr, &dwType, reinterpret_cast<BYTE*>(szVal), &cVal) == ERROR_SUCCESS)
            out.push_back(szVal);
        ++dwIdx;
    }
    return !out.empty();
}

bool ReadDword(const wchar_t* pszSubKey, DWORD& out)
{
    out = 0;
    MLRegGuard hKey;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, pszSubKey, 0, KEY_READ, hKey.out()) != ERROR_SUCCESS)
        return false;
    DWORD dwType = 0, dwSize = sizeof(DWORD);
    LONG l = ::RegQueryValueExW(hKey, L"Last", nullptr, &dwType, (BYTE*)&out, &dwSize);
    return l == ERROR_SUCCESS && dwType == REG_DWORD;
}

bool WriteDword(const wchar_t* pszSubKey, DWORD dwVal)
{
    MLRegGuard hKey;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, pszSubKey, 0, nullptr, 0,
                          KEY_WRITE, nullptr, hKey.out(), nullptr) != ERROR_SUCCESS)
        return false;
    LONG l = ::RegSetValueExW(hKey, L"Last", 0, REG_DWORD, (const BYTE*)&dwVal, sizeof(DWORD));
    return l == ERROR_SUCCESS;
}

} // namespace

// ---- ImageFiles ----
bool AddImageFile(const std::wstring& sWim)
{
    if (sWim.empty()) return false;
    return AddMruValue(kImageFiles, sWim, 10);
}

// [R133] 显式子键变体（自检/测试专用）。
//   空子键或空值一律拒绝，避免误把数据写到 HKCU 根下。
bool AddImageFileEx(const std::wstring& sSubKey, const std::wstring& sWim)
{
    if (sSubKey.empty() || sWim.empty()) return false;
    return AddMruValue(sSubKey.c_str(), sWim, 10);
}

// [R136] 带注入点的变体：仅测试用。
bool AddImageFileEx2(const std::wstring& sSubKey, const std::wstring& sWim,
                     TSetValueFn pSetValueOverride)
{
    if (sSubKey.empty() || sWim.empty()) return false;
    return AddMruValue(sSubKey.c_str(), sWim, 10, pSetValueOverride);
}

bool GetImageFilesEx(const std::wstring& sSubKey, std::vector<std::wstring>& out)
{
    if (sSubKey.empty()) { out.clear(); return false; }
    return ReadMulti(sSubKey.c_str(), out);
}

bool GetImageFiles(std::vector<std::wstring>& out)
{
    return ReadMulti(kImageFiles, out);
}

// ---- MountPaths ----
bool RecordMount(const std::wstring& sWim, DWORD dwIndex, const std::wstring& sMountDir)
{
    if (sWim.empty() || sMountDir.empty()) return false;

    // 键名取挂载目录完整路径（含反斜杠），值格式 "<wim>|<index>"
    MLRegGuard hKey;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, kMountPaths, 0, nullptr, 0,
                          KEY_WRITE, nullptr, hKey.out(), nullptr) != ERROR_SUCCESS)
        return false;

    // [C++17 阶段3][F13] wchar_t[512] -> std::wstring
    const size_t kCapVal160 = 512;
    std::wstring sVal(kCapVal160, L'\0');
    int nVal = wfmt::FormatInto(sVal.data(), kCapVal160, L"%s|%lu", sWim.c_str(), dwIndex);
    if (nVal >= 0) sVal.resize(static_cast<size_t>(nVal));
    LONG l = ::RegSetValueExW(hKey, sMountDir.c_str(), 0, REG_SZ,
                              (const BYTE*)sVal.c_str(),
                              (DWORD)((sVal.size() + 1) * sizeof(wchar_t)));
    return l == ERROR_SUCCESS;
}

bool RemoveMount(const std::wstring& sMountDir)
{
    if (sMountDir.empty()) return false;
    MLRegGuard hKey;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kMountPaths, 0, KEY_WRITE, hKey.out()) != ERROR_SUCCESS)
        return false;
    LONG l = ::RegDeleteValueW(hKey, sMountDir.c_str());
    return l == ERROR_SUCCESS;
}

bool GetMounted(std::vector<std::wstring>& outMountDirs,
                std::vector<std::wstring>& outWimPaths,
                std::vector<DWORD>& outIndices)
{
    outMountDirs.clear(); outWimPaths.clear(); outIndices.clear();
    MLRegGuard hKey;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kMountPaths, 0, KEY_READ, hKey.out()) != ERROR_SUCCESS)
        return false;
    DWORD dwIdx = 0;
    // 注册表值名缓冲（RegEnumValueW 的单值名上限）
    constexpr DWORD kRegNameBufSize = 4096;
    wchar_t szName[kRegNameBufSize] = {};
    for (;;)
    {
        DWORD cName = _countof(szName);
        LONG l = ::RegEnumValueW(hKey, dwIdx, szName, &cName, nullptr, nullptr, nullptr, nullptr);
        if (l != ERROR_SUCCESS) break;
        constexpr int kRegValWchars = 512;   // 512 缓冲尺寸（语义命名，数值不变）
        wchar_t szVal[kRegValWchars] = {};
        DWORD cVal = _countof(szVal);
        DWORD dwType = 0;
        if (::RegQueryValueExW(hKey, szName, nullptr, &dwType, reinterpret_cast<BYTE*>(szVal), &cVal) == ERROR_SUCCESS
            && dwType == REG_SZ)
        {
            std::wstring sVal(szVal);
            size_t p = sVal.rfind(L'|');
            if (p != std::wstring::npos)
            {
                outMountDirs.push_back(szName);
                outWimPaths.push_back(sVal.substr(0, p));
                outIndices.push_back(static_cast<DWORD>(_wtoi(sVal.substr(p + 1).c_str())));
            }
        }
        ++dwIdx;
    }
    return !outMountDirs.empty();
}

bool IsMounted(const std::wstring& sWim, DWORD dwIndex, std::wstring* pMountDir)
{
    std::vector<std::wstring> dirs, wims;
    std::vector<DWORD> idxs;
    if (!GetMounted(dirs, wims, idxs)) return false;
    for (size_t i = 0; i < wims.size(); ++i)
    {
        if (lstrcmpiW(wims[i].c_str(), sWim.c_str()) == 0 && idxs[i] == dwIndex)
        {
            if (pMountDir) *pMountDir = dirs[i];
            return true;
        }
    }
    return false;
}

// ---- ApplyPaths ----
bool AddApplyPath(const std::wstring& sTarget)
{
    if (sTarget.empty()) return false;
    return AddMruValue(kApplyPaths, sTarget, 10);
}

bool GetApplyPaths(std::vector<std::wstring>& out)
{
    return ReadMulti(kApplyPaths, out);
}

// ---- MountFlags ----
bool SetLastMountFlags(DWORD dwFlags)
{
    return WriteDword(kMountPaths, dwFlags);
}

DWORD GetLastMountFlags()
{
    DWORD v = 0;
    ReadDword(kMountPaths, v);
    return v;
}

bool SetLastApplyFlags(DWORD dwFlags)
{
    return WriteDword(kApplyPaths, dwFlags);
}

DWORD GetLastApplyFlags()
{
    DWORD v = 0;
    ReadDword(kApplyPaths, v);
    return v;
}

} // namespace DismLedger
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
