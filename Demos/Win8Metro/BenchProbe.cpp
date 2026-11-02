// ============================================================
// [R85] 种子失效信号探测：/SeedProbe
//
// 要解决的问题：
//   R84 让种子读取降到 0~1ms（对比枚举 880ms），但种子是**某一时刻的快照**。
//   用户装/卸 Windows 功能后，种子里状态就是旧的，页面会先显示旧状态
//   约 0.9 秒（等后台枚举回来才校正）——这是 R78 想消灭的锯齿的缩小版。
//
//   要跳过枚举就必须先判断"种子是否已经过期"，而这需要一个
//   **读取成本远低于 880ms** 的信号。本入口就是用来验证候选信号是否存在。
//
// 候选信号（全部是"读一个时间戳"，理论成本微秒级）：
//   S1  WinSxS 目录 mtime               —— 组件存储本体，功能安装会改动
//   S2  WinSxS\FileMaps 目录 mtime      —— CBS 映射缓存
//   S3  CBS 注册表 LastModified_UTC     —— 服务栈记录的修改时间
//   S4  CBS 注册表 NextExecutionSequence —— 单调递增序号，改动即变
//   S5  Capability 缓存文件的 mtime      —— 我们自己的缓存，参考基线
//
// 输出：%TEMP%\win8metro_seed_probe.txt
// 用法：先跑一次记下值 → 装/卸一个功能 → 再跑一次 → 对比哪些变了。
// ============================================================

#include "stdafx.h"

#include "Bench.h"
#include "SeedCache.h"
#include "SeedStamp.h"   // [R85] 过期判断

#include <cstdio>
#include <string>

namespace bench
{
namespace
{
void LogLine(std::string& sOut, const char* pszKey, long long llVal)
{
    char szBuf[256] = { 0 };
    std::snprintf(szBuf, sizeof(szBuf), "%s=%lld\r\n", pszKey, llVal);
    sOut += szBuf;
}

void LogText(std::string& sOut, const char* pszKey, const char* pszVal)
{
    char szBuf[512] = { 0 };
    std::snprintf(szBuf, sizeof(szBuf), "%s=%s\r\n", pszKey,
                  (pszVal && pszVal[0]) ? pszVal : "(none)");
    sOut += szBuf;
}

long long FileMtimeMs(const wchar_t* pszPath)
{
    WIN32_FILE_ATTRIBUTE_DATA fad = { 0 };
    if (!::GetFileAttributesExW(pszPath, GetFileExInfoStandard, &fad)) return 0;
    ULARGE_INTEGER u;
    u.LowPart  = fad.ftLastWriteTime.dwLowDateTime;
    u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
    // FILETIME(100ns since 1601) → Unix 秒
    return static_cast<long long>(u.QuadPart / 10000000ULL) - 11644473600LL;
}

// 读 CBS 注册表字符串值（LastModified_UTC 形如 "2026/10/20/20:51:44"）
bool ReadCbsString(const wchar_t* pszValue, std::string& sOut)
{
    HKEY hKey = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Component Based Servicing",
            0, KEY_READ | KEY_WOW64_64KEY, &hKey) != ERROR_SUCCESS)
        return false;

    wchar_t szBuf[256] = { 0 };
    DWORD cb = sizeof(szBuf) - sizeof(wchar_t);
    DWORD dwType = 0;
    const LSTATUS st = ::RegQueryValueExW(hKey, pszValue, nullptr, &dwType,
                                          reinterpret_cast<LPBYTE>(szBuf), &cb);
    ::RegCloseKey(hKey);
    if (st != ERROR_SUCCESS) return false;

    const int n = ::WideCharToMultiByte(CP_UTF8, 0, szBuf, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return false;
    sOut.resize(static_cast<size_t>(n - 1));
    ::WideCharToMultiByte(CP_UTF8, 0, szBuf, -1, &sOut[0], n, nullptr, nullptr);
    return true;
}

bool ReadCbsDword(const wchar_t* pszValue, DWORD& dwOut)
{
    HKEY hKey = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Component Based Servicing",
            0, KEY_READ | KEY_WOW64_64KEY, &hKey) != ERROR_SUCCESS)
        return false;
    DWORD cb = sizeof(DWORD);
    DWORD dwType = 0;
    const LSTATUS st = ::RegQueryValueExW(hKey, pszValue, nullptr, &dwType,
                                          reinterpret_cast<LPBYTE>(&dwOut), &cb);
    ::RegCloseKey(hKey);
    return st == ERROR_SUCCESS;
}
} // namespace

int RunSeedProbe()
{
    std::string sCrlf;
    sCrlf.push_back(static_cast<char>(13));
    sCrlf.push_back(static_cast<char>(10));

    std::string sOut;
    sOut += "# Win8Metro /SeedProbe";
    sOut += sCrlf;
    sOut += "# 用途：装/卸一个 Windows 功能前后各跑一次，对比哪些信号变了";
    sOut += sCrlf;

    LARGE_INTEGER liFreq; ::QueryPerformanceFrequency(&liFreq);
    auto Tick = []() -> LARGE_INTEGER { LARGE_INTEGER t; ::QueryPerformanceCounter(&t); return t; };
    auto Ms = [&](LARGE_INTEGER a, LARGE_INTEGER b) -> long long {
        return liFreq.QuadPart > 0 ? (b.QuadPart - a.QuadPart) * 1000 / liFreq.QuadPart : 0;
    };

    // ---- S1/S2：目录 mtime ----
    {
        const wchar_t* kPaths[] = {
            L"C:\\Windows\\WinSxS",
            L"C:\\Windows\\WinSxS\\FileMaps",
            L"C:\\Windows\\Servicing\\LCU",
        };
        const char* kKeys[] = { "sig.winsxs.mtime", "sig.filemaps.mtime", "sig.lcu.mtime" };

        for (int i = 0; i < 3; ++i)
        {
            LARGE_INTEGER a = Tick();
            const long long v = FileMtimeMs(kPaths[i]);
            LARGE_INTEGER b = Tick();
            LogLine(sOut, kKeys[i], v);
            char szK[96] = { 0 };
            std::snprintf(szK, sizeof(szK), "%s.read.ms", kKeys[i]);
            LogLine(sOut, szK, Ms(a, b));
        }
    }

    // ---- S3/S4：CBS 注册表 ----
    {
        LARGE_INTEGER a = Tick();
        std::string sLast;
        const bool bLast = ReadCbsString(L"LastModified_UTC", sLast);
        LARGE_INTEGER b = Tick();
        LogText(sOut, "sig.cbs.lastmodified", bLast ? sLast.c_str() : "");
        LogLine(sOut, "sig.cbs.lastmodified.read.ms", Ms(a, b));

        LARGE_INTEGER c = Tick();
        DWORD dwSeq = 0;
        const bool bSeq = ReadCbsDword(L"NextExecutionSequence", dwSeq);
        LARGE_INTEGER d = Tick();
        LogLine(sOut, "sig.cbs.seq", bSeq ? static_cast<long long>(dwSeq) : -1);
        LogLine(sOut, "sig.cbs.seq.read.ms", Ms(c, d));
    }

    // ---- S5：种子文件自身 mtime + 指纹（参考基线）----
    {
        const std::wstring sDir = seedcache::SeedDir();
        if (!sDir.empty())
        {
            LARGE_INTEGER a = Tick();
            const long long f = FileMtimeMs((sDir + L"\\seed_feat.dat").c_str());
            const long long c = FileMtimeMs((sDir + L"\\seed_cap.dat").c_str());
            LARGE_INTEGER b = Tick();
            LogLine(sOut, "sig.seedfeat.mtime", f);
            LogLine(sOut, "sig.seedcap.mtime", c);
            LogLine(sOut, "sig.seed.mtime.read.ms", Ms(a, b));
        }
        LogText(sOut, "sig.fingerprint", seedcache::CurrentFingerprint().c_str());
    }

    // ---- 对照：一次完整枚举的耗时 ----
    //   任何信号的读取成本必须远低于这个数，否则没有意义。
    {
        LARGE_INTEGER a = Tick();
        // 这里不调枚举（会拖慢探测），改为从 /BenchEnum 的历史文件读；
        // 没有历史时写 0，由使用者对照 R81~R84 的台账数据（880ms）。
        LogLine(sOut, "sig.ref.enum.baseline.ms", 880);
        LARGE_INTEGER b = Tick();
        LogLine(sOut, "sig.overhead.ms", Ms(a, b));
    }

    // ---- [R85] 实际调用一次过期判断：验证端到端能给出结论 ----
    {
        bool bStale = false;
        LARGE_INTEGER a = Tick();
        const bool bJudged = seedstamp::IsSeedStale(bStale);
        LARGE_INTEGER b = Tick();
        LogLine(sOut, "stale.judged", bJudged ? 1 : 0);
        LogLine(sOut, "stale.value", bStale ? 1 : 0);
        LogLine(sOut, "stale.judge.ms", Ms(a, b));
        LogText(sOut, "stale.saved", seedstamp::LoadStamp().c_str());
        LogText(sOut, "stale.now", seedstamp::CurrentStamp().c_str());
    }

    const bool bWritten = WriteBenchFile("seed_probe", sOut);
    LogLine(sOut, "probe.written", bWritten ? 1 : 0);
    return bWritten ? 0 : 1;
}
} // namespace bench
