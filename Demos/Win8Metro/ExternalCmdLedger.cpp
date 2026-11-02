// ============================================================================
// ExternalCmdLedger.cpp - [R-B56] 外部命令防腐台账实现
// 清单维度 3 缺失 5『外部命令路线与"全内置"基线分叉』防腐化。
// ============================================================================

#include "ExternalCmdLedger.h"
#include "FormatW.h"

#include <fstream>   // [C17] std::ofstream（F17_C_fileio 清零）
#include <cstdio>
#include <algorithm>
#include <memory>

namespace {

std::mutex g_mtx;
std::vector<ExternalCmdLedger::Entry> g_entries;

// 分类白名单（exe 小写 -> 分类名）；未列入归 "OTHER"
const struct { const wchar_t* exe; const wchar_t* cat; } kCats[] = {
    { L"dism.exe",        L"DISM" },
    { L"dism",            L"DISM" },
    { L"powershell.exe",  L"PowerShell" },
    { L"powershell",      L"PowerShell" },
    { L"cmd.exe",         L"CMD" },
    { L"cmd",             L"CMD" },
    { L"sfc.exe",         L"SFC" },
    { L"vssadmin.exe",    L"VSS" },
    { L"compact.exe",     L"Compact" },
    { L"compact",         L"Compact" },
    { L"wimlib-imagex",   L"WimLib" },
    { L"oscdimg.exe",     L"ISO" },
    { L"bcdboot.exe",     L"BCD" },
    { L"reagentc.exe",    L"WinRE" },
    { L"schtasks.exe",    L"ScheduleTask" },
    { L"wevtutil.exe",    L"EventLog" },
    { L"dismapi",         L"DISM" },
    { L"wmic.exe",        L"WMIC" },
    { L"reg.exe",         L"Registry" },
    { L"robocopy.exe",    L"FileOp" },
    { L"xcopy.exe",       L"FileOp" },
    { L"del.exe",         L"FileOp" },
    { L"rd.exe",          L"FileOp" },
    { L"mkdir.exe",       L"FileOp" },
    { L"chcp.exe",        L"CMD" },
    { L"certutil.exe",    L"Cert" },
    { L"netsh.exe",       L"Net" },
    { L"icacls.exe",      L"ACL" },
};

std::wstring ToLowerW(const std::wstring& s)
{
    std::wstring r = s;
    for (size_t i = 0; i < r.size(); ++i)
        if (r[i] >= L'A' && r[i] <= L'Z') r[i] += (L'a' - L'A');
    return r;
}

std::wstring BaseNameOf(const wchar_t* pszPath)
{
    if (!pszPath) return L"";
    std::wstring s(pszPath);
    // 反斜杠与正斜杠都剥
    size_t p = s.find_last_of(L"\\/");
    if (p != std::wstring::npos) s = s.substr(p + 1);
    // 剥参数（若调用方把整条命令串传入）
    size_t sp = s.find_first_of(L" \t");
    if (sp != std::wstring::npos) s = s.substr(0, sp);
    return s;
}

std::wstring Categorize(const std::wstring& exeLower)
{
    for (size_t i = 0; i < sizeof(kCats) / sizeof(kCats[0]); ++i) {
        if (exeLower == kCats[i].exe)
            return kCats[i].cat;
    }
    return L"OTHER";
}

} // anonymous namespace

namespace ExternalCmdLedger {

void Note(const wchar_t* pszExe, const wchar_t* pszArgs)
{
    if (!pszExe) return;
    std::lock_guard<std::mutex> lk(g_mtx);

    Entry e;
    e.exe = BaseNameOf(pszExe);
    e.args = pszArgs ? std::wstring(pszArgs) : std::wstring();
    std::wstring low = ToLowerW(e.exe);
    e.category = Categorize(low);
    ::GetLocalTime(&e.ts);
    e.pid = ::GetCurrentProcessId();
    g_entries.push_back(e);
}

std::vector<Entry> Snapshot()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_entries;
}

std::map<std::wstring, int> CountByExe()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    std::map<std::wstring, int> m;
    for (size_t i = 0; i < g_entries.size(); ++i)
        ++m[g_entries[i].exe];
    return m;
}

std::map<std::wstring, int> CountByCategory()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    std::map<std::wstring, int> m;
    for (size_t i = 0; i < g_entries.size(); ++i)
        ++m[g_entries[i].category];
    return m;
}

bool DumpCsv(const std::wstring& sPath)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (sPath.empty()) return false;

    // UTF-8 BOM + CSV 内容
    std::string out;
    out.reserve(g_entries.size() * 128 + 64);
    out += "\xEF\xBB\xBF";  // UTF-8 BOM
    out += "时间戳,分类,可执行文件,参数,PID\r\n";

    wchar_t szTS[64] = {};
    constexpr int kTsStrBufBytes = 2048;   // 2048 缓冲尺寸（语义命名，数值不变）
    char buf[kTsStrBufBytes] = {};

    for (size_t i = 0; i < g_entries.size(); ++i) {
        const Entry& e = g_entries[i];
        wfmt::FormatInto(szTS, _countof(szTS),
                     L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
                     (unsigned)e.ts.wYear, (unsigned)e.ts.wMonth, (unsigned)e.ts.wDay,
                     (unsigned)e.ts.wHour, (unsigned)e.ts.wMinute, (unsigned)e.ts.wSecond,
                     (unsigned)e.ts.wMilliseconds);

        // CSV 转义：逗号/引号/换行 用双引号包裹并转义内部双引号
        auto csvEscape = [](const std::wstring& in) -> std::wstring {
            if (in.find_first_of(L",\"\r\n") == std::wstring::npos) return in;
            std::wstring r = L"\"";
            for (size_t j = 0; j < in.size(); ++j) {
                if (in[j] == L'"') r += L"\"\"";
                else r += in[j];
            }
            r += L"\"";
            return r;
        };

        std::wstring line = std::wstring(szTS) + L","
                          + csvEscape(e.category) + L","
                          + csvEscape(e.exe) + L","
                          + csvEscape(e.args) + L","
                          + std::to_wstring(e.pid) + L"\r\n";

        // wchar -> UTF-8
        int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()),
                                    buf, static_cast<int>(sizeof(buf)) - 1, nullptr, nullptr);
        if (n > 0) {
            buf[n] = 0;
            out += buf;
        }
    }

    // 写入文件（ofstream 覆盖模式，F17_C_fileio 清零）
    std::ofstream ofs(sPath, std::ios::binary);
    if (!ofs) return false;
    ofs.write(out.data(), static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(ofs);
}

void Clear()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    g_entries.clear();
}

} // namespace ExternalCmdLedger
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
