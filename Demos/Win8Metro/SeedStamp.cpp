// ============================================================================
//  SeedStamp.cpp —— [R85] 种子失效检测实现
//  设计理由见 SeedStamp.h 头部注释。
// ============================================================================

#include "stdafx.h"

#include "SeedStamp.h"
#include "SeedCache.h"

#include <cstdio>
#include <string>

namespace seedstamp
{
namespace
{
// 目录 mtime → Unix 秒。取不到返回 0（不失败）。
long long DirMtimeSec(const wchar_t* pszPath)
{
    WIN32_FILE_ATTRIBUTE_DATA fad = { 0 };
    if (!::GetFileAttributesExW(pszPath, GetFileExInfoStandard, &fad)) return 0;
    ULARGE_INTEGER u;
    u.LowPart  = fad.ftLastWriteTime.dwLowDateTime;
    u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
    return static_cast<long long>(u.QuadPart / 10000000ULL) - 11644473600LL;
}

// CBS NextExecutionSequence：每次功能安装/卸载都会 +1（R85 实测 82 → 83）。
bool ReadCbsSeq(unsigned long& dwOut)
{
    HKEY hKey = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Component Based Servicing",
            0, KEY_READ | KEY_WOW64_64KEY, &hKey) != ERROR_SUCCESS)
        return false;
    DWORD dwVal = 0, cb = sizeof(DWORD), dwType = 0;
    const LSTATUS st = ::RegQueryValueExW(hKey, L"NextExecutionSequence", nullptr,
                                          &dwType, reinterpret_cast<LPBYTE>(&dwVal), &cb);
    ::RegCloseKey(hKey);
    if (st != ERROR_SUCCESS) return false;
    dwOut = static_cast<unsigned long>(dwVal);
    return true;
}

std::wstring StampPath()
{
    const std::wstring sDir = seedcache::SeedDir();
    if (sDir.empty()) return std::wstring();
    return sDir + L"\\seed_stamp.txt";
}

// 从 "filemaps=...|cbseq=..." 里取某个 key 的值；取不到返回 -1。
long long ParseField(const std::string& s, const char* pszKey)
{
    const std::string sKey = std::string(pszKey) + "=";
    const size_t p = s.find(sKey);
    if (p == std::string::npos) return -1;
    size_t q = p + sKey.size();
    long long v = 0;
    bool bAny = false;
    while (q < s.size() && s[q] >= '0' && s[q] <= '9')
    {
        v = v * 10 + (s[q] - '0');
        ++q;
        bAny = true;
    }
    return bAny ? v : -1;
}
} // namespace

std::string CurrentStamp()
{
    char szBuf[160] = { 0 };
    const long long llFileMaps = DirMtimeSec(L"C:\\Windows\\WinSxS\\FileMaps");
    unsigned long dwSeq = 0;
    const bool bSeq = ReadCbsSeq(dwSeq);
    std::snprintf(szBuf, sizeof(szBuf), "filemaps=%lld|cbseq=%lu",
                  llFileMaps, bSeq ? dwSeq : 0UL);
    return std::string(szBuf);
}

bool SaveStamp(const std::string& sStamp)
{
    const std::wstring sPath = StampPath();
    if (sPath.empty()) return false;

    HANDLE h = ::CreateFileW(sPath.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD dwW = 0;
    const BOOL bOk = ::WriteFile(h, sStamp.data(),
                                 static_cast<DWORD>(sStamp.size()), &dwW, nullptr);
    ::CloseHandle(h);
    return bOk && dwW == static_cast<DWORD>(sStamp.size());
}

std::string LoadStamp()
{
    const std::wstring sPath = StampPath();
    if (sPath.empty()) return std::string();

    HANDLE h = ::CreateFileW(sPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::string();
    char szBuf[256] = { 0 };
    DWORD dwR = 0;
    const BOOL bOk = ::ReadFile(h, szBuf, sizeof(szBuf) - 1, &dwR, nullptr);
    ::CloseHandle(h);
    if (!bOk || dwR == 0) return std::string();
    return std::string(szBuf, dwR);
}

bool IsSeedStale(bool& bStale)
{
    bStale = false;
    const std::string sSaved = LoadStamp();
    if (sSaved.empty()) return false;   // 没有快照 → 无从判断

    const long long llSavedFm = ParseField(sSaved, "filemaps");
    const long long llSavedSeq = ParseField(sSaved, "cbseq");
    if (llSavedFm < 0 && llSavedSeq < 0) return false;   // 格式坏

    const long long llNowFm = DirMtimeSec(L"C:\\Windows\\WinSxS\\FileMaps");
    unsigned long dwSeq = 0;
    const bool bSeq = ReadCbsSeq(dwSeq);

    // 任一信号变化即判过期。保守方向：宁可多枚举一次，也不要显示旧状态。
    if (llSavedFm > 0 && llNowFm > 0 && llSavedFm != llNowFm) bStale = true;
    if (bSeq && llSavedSeq > 0 && static_cast<long long>(dwSeq) != llSavedSeq) bStale = true;
    return true;
}

} // namespace seedstamp
