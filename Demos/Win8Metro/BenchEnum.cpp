// ============================================================
// [R81] 无界面性能基准：/BenchEnum
//
// 为什么需要它（R80 遗留问题）：
//   R78/R80 的优化结论全部建立在"代码结构推算"上——控件数、6ms 预算、
//   框架不支持虚拟列表。但**"加载到底是 1 秒还是 15 秒"从未测过**，
//   因为唯一能产生 ttfr 日志的路径是人工进页，无人值守环境跑不了。
//   没有测量就继续改，等于猜。
//
// 本入口做的事：直接从命令行触发真实 DISM 枚举，把各段耗时写文件后退出。
//   · 不建窗口、不进消息循环，可在 CI / 远程会话下复现；
//   · 失败也写结果（含 HRESULT/错误文本），而不是静默退出——
//     "没提权导致失败"本身就是有效信息。
//
// 输出：%TEMP%\win8metro_bench_enum.txt（UTF-8，一行一项，便于 grep）
// 退出码：0 = 两段都成功；1 = 存在失败段（数字仍会写盘）。
// ============================================================

#include "stdafx.h"

#include "Bench.h"
#include "DismApiBridge.h"

#include <cstdio>
#include <string>
#include <vector>

namespace bench
{
namespace
{
// 统一的行输出：name=value + 单位，便于后续 grep 与人工比对
void WriteLine(std::string& sOut, const char* pszKey, long long llVal, const char* pszUnit)
{
    char szBuf[256] = { 0 };
    std::snprintf(szBuf, sizeof(szBuf), "%s=%lld%s\r\n", pszKey, llVal, pszUnit ? pszUnit : "");
    sOut += szBuf;
}

// 计时器：QueryPerformanceCounter → 毫秒（保留小数 1 位，写整数避免格式分歧）
long long ElapsedMs(LARGE_INTEGER liBeg, LARGE_INTEGER liFreq)
{
    LARGE_INTEGER liEnd;
    ::QueryPerformanceCounter(&liEnd);
    if (liFreq.QuadPart <= 0) return 0;
    return static_cast<long long>(
        (liEnd.QuadPart - liBeg.QuadPart) * 1000 / liFreq.QuadPart);
}
} // namespace

bool WriteBenchFile(const char* pszName, const std::string& sBody)
{
    wchar_t szTmp[MAX_PATH] = { 0 };
    if (::GetTempPathW(MAX_PATH, szTmp) == 0) return false;
    std::wstring sPath = std::wstring(szTmp) + L"win8metro_";
    sPath.append(pszName, pszName + strlen(pszName));
    sPath += L".txt";

    HANDLE h = ::CreateFileW(sPath.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD dwW = 0;
    const BOOL bOk = ::WriteFile(h, sBody.data(),
                                 static_cast<DWORD>(sBody.size()), &dwW, nullptr);
    ::CloseHandle(h);
    return bOk && dwW == static_cast<DWORD>(sBody.size());
}

int RunBenchEnum()
{
    LARGE_INTEGER liFreq;
    ::QueryPerformanceFrequency(&liFreq);

    // CR+LF 用字符码显式构造：本仓库多次踩过'\r\n'字面量被工具链
    //   展开成真实换行的坑，写成码点就不会再被误处理。
    std::string sCrlf;
    sCrlf.push_back(static_cast<char>(13));
    sCrlf.push_back(static_cast<char>(10));
    std::string sOut;
    sOut += "# Win8Metro /BenchEnum\r\n";
    sOut += "# 单位：ms；session=会话建立+首查，api=枚举本体\r\n";

    // 显式初始化并记录 HRESULT：只说 "unavailable" 无法区分
    //   "没提权(0x800702E4)"、"服务锁定"、"模块缺失" 三种完全不同的情况，
    //   基准的意义就在于给出可归因的数字与码，而不是一句笼统的失败。
    {
        const bool bInit = DismApiBridge::Init();
        const HRESULT hr = DismApiBridge::LastHResult();
        char szBuf[160] = { 0 };
        std::snprintf(szBuf, sizeof(szBuf), "enum.init.ok=%d|enum.init.hr=0x%08lX",
                      bInit ? 1 : 0, static_cast<unsigned long>(hr));
        sOut += szBuf;
        sOut += sCrlf;
    }

    // ---- [R82] 独立的会话建立计时：不用"首轮-第二轮"反推 ----
    //   R81 是用 1404-863=541 推出"会话约占 541ms"，但那是差值，
    //   混入了 CBS 内部缓存预热的影响。这里直接量 OpenOnlineSession 本身。
    {
        LARGE_INTEGER liS; ::QueryPerformanceCounter(&liS);
        std::wstring sErr;
        const UINT sess = DismApiBridge::OpenOnlineSession(&sErr);
        const long long msSess = ElapsedMs(liS, liFreq);
        WriteLine(sOut, "session.open.ms", msSess, "");
        WriteLine(sOut, "session.open.ok", sess ? 1 : 0, "");
        if (!sess)
        {
            char szBuf[512] = { 0 };
            std::snprintf(szBuf, sizeof(szBuf), "session.open.err=%ls", 
                          sErr.empty() ? L"(no error text)" : sErr.c_str());
            sOut += szBuf;
            sOut += sCrlf;
        }
        else
        {
            DismApiBridge::CloseSession(sess);
        }
    }

    int nFail = 0;

    // ---- Feature 枚举 ----
    {
        LARGE_INTEGER liBeg; ::QueryPerformanceCounter(&liBeg);
        std::vector<DismApiBridge::FeatureInfo> vF;
        std::wstring sErr;
        const bool bOk = DismApiBridge::GetFeatures(vF, &sErr);
        const long long ms = ElapsedMs(liBeg, liFreq);

        WriteLine(sOut, "enum.feature.ms", ms, "");
        WriteLine(sOut, "enum.feature.count", static_cast<long long>(vF.size()), "");
        WriteLine(sOut, "enum.feature.ok", bOk ? 1 : 0, "");
        if (!bOk)
        {
            ++nFail;
            // 失败原因必须落盘：未提权是最常见的一条，写清才能判定这是环境问题
            char szBuf[512] = { 0 };
            std::snprintf(szBuf, sizeof(szBuf), "enum.feature.err=%ls\r\n",
                          sErr.empty() ? L"(no error text)" : sErr.c_str());
            sOut += szBuf;
        }
    }

    // ---- Capability 枚举 ----
    {
        LARGE_INTEGER liBeg; ::QueryPerformanceCounter(&liBeg);
        std::vector<DismApiBridge::CapabilityInfo> vC;
        std::wstring sErr;
        const bool bOk = DismApiBridge::GetCapabilities(vC, &sErr);
        const long long ms = ElapsedMs(liBeg, liFreq);

        WriteLine(sOut, "enum.capability.ms", ms, "");
        WriteLine(sOut, "enum.capability.count", static_cast<long long>(vC.size()), "");
        WriteLine(sOut, "enum.capability.ok", bOk ? 1 : 0, "");
        if (!bOk)
        {
            ++nFail;
            char szBuf[512] = { 0 };
            std::snprintf(szBuf, sizeof(szBuf), "enum.capability.err=%ls\r\n",
                          sErr.empty() ? L"(no error text)" : sErr.c_str());
            sOut += szBuf;
        }
    }

    // ---- 第二轮：复用会话后的耗时（判断"会话建立"是否是主要成本）----
    //   两轮之差 = 纯枚举成本；第一轮 - 第二轮 = 会话/首次初始化成本。
    {
        LARGE_INTEGER liBeg; ::QueryPerformanceCounter(&liBeg);
        std::vector<DismApiBridge::FeatureInfo> vF2;
        std::wstring sErr2;
        const bool bOk2 = DismApiBridge::GetFeatures(vF2, &sErr2);
        const long long ms2 = ElapsedMs(liBeg, liFreq);
        WriteLine(sOut, "enum.feature2.ms", ms2, "");
        WriteLine(sOut, "enum.feature2.ok", bOk2 ? 1 : 0, "");

        // [R82] 第 3~5 轮：若耗时持续下降，说明"首轮-第二轮"的差值里
        //   混着 CBS 内部缓存预热，不能整体当作会话建立成本。
        for (int k = 3; k <= 5; ++k)
        {
            LARGE_INTEGER liK; ::QueryPerformanceCounter(&liK);
            std::vector<DismApiBridge::FeatureInfo> vFk;
            std::wstring sErrk;
            const bool bOkk = DismApiBridge::GetFeatures(vFk, &sErrk);
            char szKey[64] = { 0 };
            std::snprintf(szKey, sizeof(szKey), "enum.feature%d.ms", k);
            WriteLine(sOut, szKey, ElapsedMs(liK, liFreq), "");

            // [R82] Capability 同步多轮：它的 531→218 波动太大，
            //   单次采样不足以判断稳态，必须看多轮分布。
            LARGE_INTEGER liC; ::QueryPerformanceCounter(&liC);
            std::vector<DismApiBridge::CapabilityInfo> vCk;
            std::wstring sErrCk;
            const bool bOkCk = DismApiBridge::GetCapabilities(vCk, &sErrCk);
            char szKeyC[64] = { 0 };
            std::snprintf(szKeyC, sizeof(szKeyC), "enum.capability%d.ms", k);
            WriteLine(sOut, szKeyC, ElapsedMs(liC, liFreq), "");
            char szKeyC2[64] = { 0 };
            std::snprintf(szKeyC2, sizeof(szKeyC2), "enum.capability%d.count", k);
            WriteLine(sOut, szKeyC2, static_cast<long long>(vCk.size()), "");
        }
    }

    WriteLine(sOut, "bench.fail", nFail, "");

    const bool bWritten = WriteBenchFile("bench_enum", sOut);
    WriteLine(sOut, "bench.written", bWritten ? 1 : 0, "");

    return (nFail == 0 && bWritten) ? 0 : 1;
}
} // namespace bench
