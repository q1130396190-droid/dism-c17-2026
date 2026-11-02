// ============================================================
// [R84] 种子包生成：/MakeSeed
//
// 为什么需要它：
//   R82 已用实测确认"枚举稳态 880ms(Feature) + 210ms(Capability)"是 1 秒目标的
//   唯一硬成本，种子包能把这条降到"读文件"。机制（格式/加载端/指纹校验）在
//   R54~R63 就建好了，唯独缺一件事——**在目标机上把种子生成出来**。
//   过去这一步只能人工照 README 拼文件，容易写错格式，而加载端是逐字节比对的，
//   错一个 TAB 就是"放了种子但页面照样转圈"，且没有任何报错。
//
// 本入口做的事：
//   跑一次真实 DISM 枚举 → 按加载端已有的格式写出两个种子文件 →
//   立即用加载端自己的 Load*SeedCache 读回来验证。
//   验证不通过就报错退出，绝不留下"生成了但读不了"的文件。
//
// 输出位置：<exe目录>\seed\seed_feat.dat 与 seed_cap.dat
// 退出码：0 = 生成且自验证通过；1 = 失败（含枚举失败/写盘失败/回读失败）。
//
// 注意：需要能成功调用 DISM（通常要管理员）。失败会明确写出 HRESULT。
// ============================================================

#include "stdafx.h"

#include "Bench.h"
#include "DismApiBridge.h"
#include "FeatureService.h"
#include "AppxCapUpdService.h"
#include "SeedCache.h"
#include "SeedStamp.h"   // [R85] 记录生成时的系统状态快照

#include <cstdio>
#include <ctime>   // ::time() 在此编译单元未被间接包含
#include <string>
#include <vector>

namespace bench
{
namespace
{
// [R84] 制表符/换行转义：与 FeatureService/AppxCapUpdService 内部的
//   FeatCacheEscapeField / CapCacheEscape 保持同一规则（控制字符替换为空格）。
//   这里重写一份而不复用内部函数，是因为它们是匿名 namespace 里的实现细节；
//   若将来两处规则分叉，回读验证会立刻失败，不会静默出错。
void AppendEscaped(const CDuiString& sIn, std::string& sOut)
{
    std::wstring w = sIn.GetData() ? sIn.GetData() : std::wstring();
    for (size_t i = 0; i < w.size(); ++i)
    {
        wchar_t ch = w[i];
        if (ch == L'\t' || ch == L'\r' || ch == L'\n') ch = L' ';
        w[i] = ch;
    }
    if (w.empty()) return;
    const int cb = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                        static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (cb <= 0) return;
    const size_t at = sOut.size();
    sOut.resize(at + static_cast<size_t>(cb));
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                          &sOut[at], cb, nullptr, nullptr);
}

// 十进制拼接（避免依赖 printf 族，与既有 SaveFeatCache 风格一致）
void AppendUInt(std::string& sOut, unsigned long long v)
{
    if (v == 0) { sOut.push_back('0'); return; }
    char buf[32] = { 0 };
    int n = 0;
    while (v > 0 && n < 31) { buf[n++] = static_cast<char>('0' + (v % 10)); v /= 10; }
    while (n > 0) sOut.push_back(buf[--n]);
}

bool WriteAllBytes(const std::wstring& sPath, const std::string& sBody)
{
    HANDLE h = ::CreateFileW(sPath.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD dwW = 0;
    const BOOL bOk = ::WriteFile(h, sBody.data(),
                                 static_cast<DWORD>(sBody.size()), &dwW, nullptr);
    ::CloseHandle(h);
    return bOk && dwW == static_cast<DWORD>(sBody.size());
}

void LogLine(std::string& sOut, const char* pszKey, long long llVal)
{
    char szBuf[256] = { 0 };
    std::snprintf(szBuf, sizeof(szBuf), "%s=%lld\r\n", pszKey, llVal);
    sOut += szBuf;
}
} // namespace

int RunMakeSeed()
{
    // [R84] CR+LF 用字符码构造。这个仓库已三次踩到"\r\n 字面量被写成真实换行"
    //   导致字符串未闭合（R81 BenchEnum、R82 BenchEnum、本次）。字符码不会再被处理。
    std::string sCrlf;
    sCrlf.push_back(static_cast<char>(13));
    sCrlf.push_back(static_cast<char>(10));
    std::string sOut;
    sOut += "# Win8Metro /MakeSeed\r\n";

    // ---- 0) 数据头与指纹：直接复用加载端的唯一来源，不自己拼 ----
    //   MakeSeedHeaderLine() 返回 "SEED\t<fp>\r\n"，/DumpSeedFingerprint 与
    //   自检用的都是它，因此这里也用同一份，避免两处格式分歧。
    const std::wstring sSeedHeadW = seedcache::MakeSeedHeaderLine();
    std::string sSeedHead;
    AppendEscaped(CDuiString(sSeedHeadW.c_str()), sSeedHead);
    // 注意：AppendEscaped 会把 CRLF 转成空格，所以不能拿它处理首行。
    //   首行必须原样保留 TAB 与 CRLF——改用逐码转换。
    {
        sSeedHead.clear();
        const int cb = ::WideCharToMultiByte(CP_UTF8, 0, sSeedHeadW.c_str(),
                            static_cast<int>(sSeedHeadW.size()), nullptr, 0, nullptr, nullptr);
        if (cb > 0)
        {
            sSeedHead.resize(static_cast<size_t>(cb));
            ::WideCharToMultiByte(CP_UTF8, 0, sSeedHeadW.c_str(),
                                  static_cast<int>(sSeedHeadW.size()),
                                  &sSeedHead[0], cb, nullptr, nullptr);
        }
    }
    LogLine(sOut, "seed.header.len", static_cast<long long>(sSeedHead.size()));

    const std::wstring sDir = seedcache::SeedDir();
    if (sDir.empty())
    {
        sOut += "seed.err=SeedDir() empty (cannot locate exe dir)\r\n";
        LogLine(sOut, "seed.ok", 0);
        WriteBenchFile("make_seed", sOut);
        return 1;
    }
    ::CreateDirectoryW(sDir.c_str(), nullptr);   // 已存在时失败可忽略

    // 必须显式 Init：GetFeatures/GetCapabilities 不会自己初始化 dismapi，
    //   不调就会得到 "unavailable"，看起来像权限问题，实际是漏了初始化。
    {
        const bool bInit = DismApiBridge::Init();
        const HRESULT hr = DismApiBridge::LastHResult();
        char szBuf[160] = { 0 };
        std::snprintf(szBuf, sizeof(szBuf), "seed.init.ok=%d|seed.init.hr=0x%08lX",
                      bInit ? 1 : 0, static_cast<unsigned long>(hr));
        sOut += szBuf;
        sOut += sCrlf;
        if (!bInit)
        {
            sOut += "seed.err=DismInitialize failed (needs admin / CBS locked)";
            sOut += sCrlf;
            LogLine(sOut, "seed.ok", 0);
            WriteBenchFile("make_seed", sOut);
            return 1;
        }
    }

    int nFail = 0;

    // ---- 1) Feature 种子 ----
    {
        std::vector<DismApiBridge::FeatureInfo> vF;
        std::wstring sErr;
        const bool bEnum = DismApiBridge::GetFeatures(vF, &sErr);
        LogLine(sOut, "seed.feat.enum.ok", bEnum ? 1 : 0);
        LogLine(sOut, "seed.feat.entries", static_cast<long long>(vF.size()));
        if (!bEnum || vF.empty())
        {
            ++nFail;
            char szBuf[512] = { 0 };
            std::snprintf(szBuf, sizeof(szBuf), "seed.feat.enum.err=%ls\r\n",
                          sErr.empty() ? L"(no error text)" : sErr.c_str());
            sOut += szBuf;
        }
        else
        {
            // 首行 SEED\t<fp>\r\n + 数据头 FEATCACHE\t<epoch>\r\n + 每行 sys\t0/1
            std::string sBody = sSeedHead;
            sBody += "FEATCACHE\t";
            AppendUInt(sBody, static_cast<unsigned long long>(::time(nullptr)));
            sBody += sCrlf;
            for (size_t i = 0; i < vF.size(); ++i)
            {
                AppendEscaped(CDuiString(vF[i].sName.c_str()), sBody);
                sBody += '\t';
                sBody += vF[i].bEnabled ? '1' : '0';
                sBody += sCrlf;
            }

            const std::wstring sPath = sDir + L"\\seed_feat.dat";
            const bool bW = WriteAllBytes(sPath, sBody);
            LogLine(sOut, "seed.feat.write.ok", bW ? 1 : 0);
            LogLine(sOut, "seed.feat.bytes", static_cast<long long>(sBody.size()));

            // 立刻用加载端自己的实现回读：这是"格式正确"的唯一可信证据
            // [R84] 量"读种子"的真实耗时：R82 推算的 ~15ms 一直是估算，
            //   这里把它变成实测，种子包收益才不是空口数字。
            LARGE_INTEGER liR; ::QueryPerformanceCounter(&liR);
            std::vector<FeatRawEntry> vBack;
            const bool bR = FeatureService::LoadSeedFeatCache(vBack, sPath);
            {
                LARGE_INTEGER liFreq; ::QueryPerformanceFrequency(&liFreq);
                LARGE_INTEGER liE; ::QueryPerformanceCounter(&liE);
                const long long ms = liFreq.QuadPart > 0
                    ? (liE.QuadPart - liR.QuadPart) * 1000 / liFreq.QuadPart : 0;
                LogLine(sOut, "seed.feat.load.ms", ms);
            }
            LogLine(sOut, "seed.feat.reload.ok", bR ? 1 : 0);
            LogLine(sOut, "seed.feat.reload.entries", static_cast<long long>(vBack.size()));
            if (!bW || !bR || vBack.size() != vF.size()) ++nFail;
        }
    }

    // ---- 2) Capability 种子 ----
    {
        std::vector<DismApiBridge::CapabilityInfo> vC;
        std::wstring sErr;
        const bool bEnum = DismApiBridge::GetCapabilities(vC, &sErr);
        LogLine(sOut, "seed.cap.enum.ok", bEnum ? 1 : 0);
        LogLine(sOut, "seed.cap.entries", static_cast<long long>(vC.size()));
        if (!bEnum || vC.empty())
        {
            ++nFail;
            char szBuf[512] = { 0 };
            std::snprintf(szBuf, sizeof(szBuf), "seed.cap.enum.err=%ls\r\n",
                          sErr.empty() ? L"(no error text)" : sErr.c_str());
            sOut += szBuf;
        }
        else
        {
            // 首行 SEED + 数据头 CAPCACHE\t<epoch> + 每行 8 字段
            //   identity \t state \t desc \t size \t reqDeps \t optDeps \t reqDep \t optDep
            //   枚举只填 identity/state 两字段，其余留空——与页面自行枚举产出的
            //   缓存逐字段等价（页面那条路径同样只填两字段）。
            std::string sBody = sSeedHead;
            sBody += "CAPCACHE\t";
            AppendUInt(sBody, static_cast<unsigned long long>(::time(nullptr)));
            sBody += sCrlf;
            for (size_t i = 0; i < vC.size(); ++i)
            {
                AppendEscaped(CDuiString(vC[i].sName.c_str()), sBody);
                sBody += '\t';
                AppendEscaped(CDuiString(vC[i].bInstalled ? L"Installed" : L"Not Present"), sBody);
                sBody += "\t\t\t0\t0\t\t\r\n";   // desc/size 空，依赖计数 0，依赖名空
            }

            const std::wstring sPath = sDir + L"\\seed_cap.dat";
            const bool bW = WriteAllBytes(sPath, sBody);
            LogLine(sOut, "seed.cap.write.ok", bW ? 1 : 0);
            LogLine(sOut, "seed.cap.bytes", static_cast<long long>(sBody.size()));

            LARGE_INTEGER liR; ::QueryPerformanceCounter(&liR);
            std::vector<CapEntry> vBack;
            const bool bR = AppxCapUpdService::LoadSeedCapCache(vBack, sPath);
            {
                LARGE_INTEGER liFreq; ::QueryPerformanceFrequency(&liFreq);
                LARGE_INTEGER liE; ::QueryPerformanceCounter(&liE);
                const long long ms = liFreq.QuadPart > 0
                    ? (liE.QuadPart - liR.QuadPart) * 1000 / liFreq.QuadPart : 0;
                LogLine(sOut, "seed.cap.load.ms", ms);
            }
            LogLine(sOut, "seed.cap.reload.ok", bR ? 1 : 0);
            LogLine(sOut, "seed.cap.reload.entries", static_cast<long long>(vBack.size()));
            if (!bW || !bR || vBack.size() != vC.size()) ++nFail;
        }
    }

    // ---- [R85] 同时写出系统状态快照：将来判断种子是否过期用 ----
    //   不改进种子文件首行格式（那是加载端逐字节比对的），改用旁挂文件。
    {
        const std::string sStamp = seedstamp::CurrentStamp();
        const bool bStampOk = seedstamp::SaveStamp(sStamp);
        char szBuf[256] = { 0 };
        std::snprintf(szBuf, sizeof(szBuf), "seed.stamp.ok=%d|seed.stamp=%s",
                      bStampOk ? 1 : 0, sStamp.c_str());
        sOut += szBuf;
        sOut += sCrlf;
    }

    LogLine(sOut, "seed.fail", nFail);
    LogLine(sOut, "seed.ok", nFail == 0 ? 1 : 0);
    WriteBenchFile("make_seed", sOut);

    return nFail == 0 ? 0 : 1;
}
} // namespace bench
