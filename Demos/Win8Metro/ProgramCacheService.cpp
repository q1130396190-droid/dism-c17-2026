#include "stdafx.h"
#include "ProgramCacheService.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace
{
const wchar_t* const kCacheRel = L"\\DismTool_Clone\\program_cache.tsv";

bool CachePath(std::wstring& out)
{
    wchar_t sz[MAX_PATH] = { 0 };
    if (!::GetEnvironmentVariableW(L"LOCALAPPDATA", sz, MAX_PATH)) return false;
    out = sz;
    out += kCacheRel;
    return true;
}

std::string Narrow(const CDuiString& w)
{
    if (w.IsEmpty()) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.GetData(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) ::WideCharToMultiByte(CP_UTF8, 0, w.GetData(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

CDuiString Widen(std::string_view u)
{
    if (u.empty()) return CDuiString();
    // [R27] 用显式长度而非 -1：string_view 不保证以 NUL 结尾。
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, u.data(),
                                        static_cast<int>(u.size()), nullptr, 0);
    if (n <= 0) return CDuiString();
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, u.data(), static_cast<int>(u.size()), w.data(), n);
    return CDuiString(w.c_str());
}

std::string Cell(const CDuiString& w)
{
    std::string s = Narrow(w);
    for (char& c : s)
        if (c == '\t' || c == '\r' || c == '\n') c = ' ';
    return s;
}

// [R27] 用 string_view 切分，避免每列一次 substr 拷贝。
// 原实现 SplitTabs 对每列都产生一个 std::string，几百行程序表会产生
// 数千次小分配；这里只产出视图，实际转换在 Widen 时才发生。
void SplitTabs(std::string_view s, std::vector<std::string_view>& out)
{
    out.clear();
    size_t start = 0;
    for (;;)
    {
        const size_t p = s.find('\t', start);
        out.push_back(s.substr(start, p == std::string_view::npos ? std::string_view::npos : p - start));
        if (p == std::string_view::npos) break;
        start = p + 1;
    }
}
} // namespace

bool ProgramCacheService::Load(std::vector<ProgEntry>& vOut)
{
    vOut.clear();
    std::wstring path;
    if (!CachePath(path)) return false;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || f == nullptr) return false;

    char line[4096];
    std::vector<std::string_view> cols;
    while (std::fgets(line, sizeof(line), f))
    {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.empty()) continue;
        SplitTabs(s, cols);
        if (cols.size() < 9) continue;
        ProgEntry e;
        e.nRoot = std::atoi(std::string(cols[0]).c_str());
        e.sSubKey     = Widen(cols[1]);
        e.sName       = Widen(cols[2]);
        e.sDate       = Widen(cols[3]);
        e.sSize       = Widen(cols[4]);
        e.sUninstall  = Widen(cols[5]);
        e.sPublisher  = Widen(cols[6]);
        e.sInstallLoc = Widen(cols[7]);
        e.sIconBase   = Widen(cols[8]);
        e.bSigned = cols.size() > 9  && cols[9]  == "1";
        e.bMsft   = cols.size() > 10 && cols[10] == "1";
        e.bIsMsi  = cols.size() > 11 && cols[11] == "1";
        if (!e.sName.IsEmpty()) vOut.push_back(e);
    }
    std::fclose(f);
    return !vOut.empty();
}

bool ProgramCacheService::Save(const std::vector<ProgEntry>& vIn)
{
    std::wstring path;
    if (!CachePath(path)) return false;
    const size_t p = path.find_last_of(L'\\');
    if (p != std::wstring::npos) ::CreateDirectoryW(path.substr(0, p).c_str(), nullptr);

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || f == nullptr) return false;
    std::string line;
    for (const ProgEntry& e : vIn)
    {
        line.clear();
        line += std::to_string(e.nRoot); line += '\t';
        line += Cell(e.sSubKey);      line += '\t';
        line += Cell(e.sName);        line += '\t';
        line += Cell(e.sDate);        line += '\t';
        line += Cell(e.sSize);        line += '\t';
        line += Cell(e.sUninstall);   line += '\t';
        line += Cell(e.sPublisher);   line += '\t';
        line += Cell(e.sInstallLoc);  line += '\t';
        line += Cell(e.sIconBase);    line += '\t';
        line += e.bSigned ? "1" : "0"; line += '\t';
        line += e.bMsft ? "1" : "0";   line += '\t';
        line += e.bIsMsi ? "1" : "0";  line += '\n';
        std::fwrite(line.data(), 1, line.size(), f);
    }
    std::fclose(f);
    return true;
}

void ProgramCacheService::Invalidate()
{
    std::wstring path;
    if (CachePath(path)) ::DeleteFileW(path.c_str());
}
