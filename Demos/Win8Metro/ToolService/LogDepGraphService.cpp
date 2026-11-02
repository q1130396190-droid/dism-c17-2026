// ============================================================
// LogDepGraphService.cpp —— [R6-02] 日志解析 / 依赖树文本化
// 行为等价基线：ToolDialog.cpp(2026-10-13) 6965-7070 静态解析函数
// 与 7157-7257 / 7354-7465 的逐行拼装，仅剥离控件/测量部分。
// ============================================================

// 本 TU 不使用 PCH（NotUsing），需在此自行抑制 CRT 弃用告警，
// 与 stdafx.h 中全局 _CRT_SECURE_NO_WARNINGS 的效果对齐（零新增告警）。
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <windows.h>
#include <fstream>   // [C16] std::ifstream
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

#include "LogDepGraphService.h"
#include "../LogService.h"   // 统一日志目录 / 路径（M-4）

namespace toolsvc {

namespace {

// UTF-8 -> UTF-16（原 LogUtf8ToW）
std::wstring LogUtf8ToW(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

// 从一行 JSON 中取出 "key":"value" 的原始 value（未反转义；原 LogJsonField）
std::string LogJsonField(const std::string& sLine, const char* pszKey)
{
    std::string sKey = std::string("\"") + pszKey + "\"";
    size_t k = sLine.find(sKey);
    if (k == std::string::npos) return std::string();
    k = sLine.find(':', k);
    if (k == std::string::npos) return std::string();
    k = sLine.find('"', k);
    if (k == std::string::npos) return std::string();
    ++k;
    std::string out;
    while (k < sLine.size())
    {
        char c = sLine[k];
        if (c == '\\' && k + 1 < sLine.size())
        {
            out += c; out += sLine[k + 1]; k += 2; continue;
        }
        if (c == '"') break;
        out += c; ++k;
    }
    return out;
}

// 反转义已转 UTF-16 的字符串（\" \\ \n \r \t；原 LogUnescape）
std::wstring LogUnescape(const std::wstring& s)
{
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == L'\\' && i + 1 < s.size())
        {
            wchar_t n = s[i + 1];
            if      (n == L'n')  { out += L'\n'; ++i; }
            else if (n == L'r')  { out += L'\r'; ++i; }
            else if (n == L't')  { out += L'\t'; ++i; }
            else if (n == L'"')  { out += L'"';  ++i; }
            else if (n == L'\\') { out += L'\\'; ++i; }
            else                 out += L'\\';
        }
        else out += s[i];
    }
    return out;
}

// 缩进（4 空格/级，封顶 12 级）
void AppendIndent(std::wstring& s, int nLevel)
{
    int n = nLevel > 0 ? nLevel : 0;
    if (n > 12) n = 12;
    for (int k = 0; k < n; ++k) s += L"    ";
}

// [L-27] 严格 UTF-8 校验：非法序列立即判定失败，用于区分"UTF-8 文本"与 GBK 遗留。
//   宽松做法（MB_ERR_INVALID_CHARS 之外还忽略错误）会把 GBK 误当 UTF-8，
//   读出来一片问号；这里宁可判定失败转 ACP。
bool IsValidUtf8(const std::string& s)
{
    size_t i = 0;
    while (i < s.size())
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t nMore = 0;
        if (c < 0x80) { ++i; continue; }
        else if ((c & 0xE0) == 0xC0) { nMore = 1; if (c < 0xC2) return false; }
        else if ((c & 0xF0) == 0xE0) { nMore = 2; }
        else if ((c & 0xF8) == 0xF0) { nMore = 3; if (c > 0xF4) return false; }
        else return false;

        // 续字节必须存在且形如 10xxxxxx
        if (i + nMore >= s.size()) return false;
        for (size_t k = 1; k <= nMore; ++k)
        {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
        }
        i += nMore + 1;
    }
    return true;
}

// [L-27] 字节 → 宽字符：BOM 优先 → 严格 UTF-8 → ACP(GBK) 兜底。
//   必须兼容两种历史产物：UTF-8 无 BOM 的新日志、C-locale 时期写坏的旧底账。
std::wstring DecodeBytes(const std::string& sIn, size_t nOffset = 0, size_t nLen = std::string::npos)
{
    if (nOffset >= sIn.size()) return std::wstring();
    if (nLen == std::string::npos || nOffset + nLen > sIn.size()) nLen = sIn.size() - nOffset;

    std::string s(sIn, nOffset, nLen);
    UINT nCodePage = CP_UTF8;
    DWORD dwFlags  = MB_ERR_INVALID_CHARS;

    // UTF-8 BOM
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB && static_cast<unsigned char>(s[2]) == 0xBF)
    {
        s.erase(0, 3);
    }
    else if (!IsValidUtf8(s))
    {
        // 不是合法 UTF-8：按系统 ANSI 代码页（简体中文即 GBK）解码
        nCodePage = CP_ACP;
        dwFlags   = 0;
    }

    if (s.empty()) return std::wstring();
    const int n = ::MultiByteToWideChar(nCodePage, dwFlags, s.c_str(),
                                        static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(nCodePage, dwFlags, s.c_str(),
                          static_cast<int>(s.size()), &w[0], n);
    return w;
}

} // namespace

// [R16][T4] 弹窗入参统一入口：依赖图节点由调用方经 ToolOpenArgs 注入。
void CLogDepGraphService::Open(const ToolOpenArgs& args)
{
    m_vNodes = args.vDepNodes;
}

std::wstring CLogDepGraphService::LogPath() const
{
    // [L-22][M-4] 路径统一由 LogService 决定：EXE\Logs 首选、不可写时回退。
    //   此前这里自己拼 EXE 同目录，与写入侧各维护一份路径逻辑，
    //   一旦写入侧回退到 %LOCALAPPDATA%，查看器就会"读不到刚写的日志"。
    return LogService::UnifiedLogPath(L"action_log.jsonl");
}

bool CLogDepGraphService::ReadLogLines(std::vector<TLogLine>& vOut) const
{
    const std::wstring sPath = LogPath();
    if (sPath.empty()) return false;
    std::ifstream ifs(sPath, std::ios::binary);
    if (!ifs) return false;
    ifs.seekg(0, std::ios::end);
    const std::streamoff nSize = ifs.tellg();
    if (nSize <= 0) return true;      // 空文件：成功但无内容

    // [R-LOGV-2] 只读尾部字节，避免超大文件拖慢首屏：
    //   写入侧已轮转到 1MB，正常不会触发；但历史遗留/手工放大的文件仍可能很大，
    //   这里按字节上限兜底，且从"下一行行首"开始读，保证首行不被截半。
    constexpr std::streamoff kMaxReadBytes = 2 * 1024 * 1024;
    std::streamoff nBegin = 0;
    if (nSize > kMaxReadBytes)
    {
        nBegin = nSize - kMaxReadBytes;
        ifs.seekg(nBegin, std::ios::beg);
        // 跳到下一行行首，丢弃可能被截断的半行
        std::string sSkipLine;
        std::getline(ifs, sSkipLine);
        nBegin = ifs.tellg();
    }
    else
    {
        ifs.seekg(0, std::ios::beg);
    }

    std::string sBuf(static_cast<size_t>(nSize - nBegin), '\0');
    ifs.read(&sBuf[0], static_cast<std::streamsize>(sBuf.size()));
    sBuf.resize(static_cast<size_t>(ifs.gcount()));

    size_t pos = 0;
    while (pos < sBuf.size())
    {
        size_t nl = sBuf.find('\n', pos);
        if (nl == std::string::npos) nl = sBuf.size();
        const size_t nLineStart = pos;
        size_t nLineLen = nl - nLineStart;
        pos = nl + 1;

        if (nLineLen < 5) continue;   // 空行/噪声

        // 去掉行尾 \r（CRLF），再交给字段解析
        if (sBuf[nLineStart + nLineLen - 1] == '\r') --nLineLen;
        const std::string sLine(sBuf, nLineStart, nLineLen);

        // 逐字段取原始值（未反转义）；编码在最后统一解码，
        //   这样 GBK 遗留行也能正确还原中文。
        const std::string sTs    = LogJsonField(sLine, "ts");
        const std::string sLevel = LogJsonField(sLine, "level");
        const std::string sMsg   = LogJsonField(sLine, "msg");
        const std::string sMod   = LogJsonField(sLine, "module");
        const std::string sTid   = LogJsonField(sLine, "tid");

        // 级别：兼容新枚举（trace/debug/info/warn/error/fatal）与旧三值
        int nLevel = static_cast<int>(toolsvc::ELogLevel::kLogInfo);
        const wchar_t* pszTag = L"INFO";
        if (sLevel == "warn" || sLevel == "warning") { nLevel = static_cast<int>(toolsvc::ELogLevel::kLogWarn);  pszTag = L"WARN"; }
        else if (sLevel == "error")                  { nLevel = static_cast<int>(toolsvc::ELogLevel::kLogError); pszTag = L"ERROR"; }
        else if (sLevel == "fatal")                  { nLevel = static_cast<int>(toolsvc::ELogLevel::kLogError); pszTag = L"FATAL"; }
        else if (sLevel == "debug")                  { pszTag = L"DEBUG"; }
        else if (sLevel == "trace")                  { pszTag = L"TRACE"; }

        const std::wstring sTsW  = DecodeBytes(sTs);
        const std::wstring sMsgW = LogUnescape(DecodeBytes(sMsg));
        const std::wstring sModW = DecodeBytes(sMod);

        // 粘连坏行（历史 C-locale 截断产物）往往缺 msg 或含未转义引号：
        //   此时 sMsg 为空但有 ts，仍输出一行，避免整条静默消失。
        std::wstring sDisp;
        if (!sTsW.empty())
        {
            sDisp += L"[";
            sDisp += sTsW;
            sDisp += L"] ";
        }
        sDisp += pszTag;
        sDisp += L"  ";
        if (!sModW.empty() && sModW != L"app")
        {
            sDisp += L"(";
            sDisp += sModW;
            sDisp += L") ";
        }
        if (!sTid.empty())
        {
            sDisp += L"#";
            sDisp += DecodeBytes(sTid);
            sDisp += L" ";
        }
        sDisp += sMsgW;
        // 日志 msg 内嵌 \r\n / \r 都另起一行
        std::replace(sDisp.begin(), sDisp.end(), L'\r', L'\n');

        TLogLine t;
        t.nLevel = nLevel;
        t.sText = std::move(sDisp);
        vOut.push_back(std::move(t));
    }
    return true;
}

// [L-26][L-28] 三源合并读取。
//
// 为什么不再只显示 action_log.jsonl：
//   日志体系里实际有三个落点——审计事件流、诊断快照/专项底账、外部工具归档。
//   过去"查看日志"只读第一个，用户排查"清理为什么失败"时，clean_scan.log 与
//   归档正文得自己去目录里翻。这里把三源并到同一个窗口，按源分节呈现。
//
// 呈现上的两个克制：
//   1) 不做全局时间排序。底账是纯文本行，没有可靠时间戳，强行排序等于猜测；
//      审计流本身已天然时序，分节展示反而更诚实。
//   2) 每个非审计源都有配额（尾部 N 行 / 最近 M 个文件），避免一次读入过多
//      让 RichEdit 排版变慢——这与查看器"只渲染尾部"的思路一致。
namespace {

// 读取一个纯文本文件的尾部若干行，按编码嗅探解码，返回带前缀的行。
void AppendTailOfPlainFile(const std::wstring& sPath, const wchar_t* pszPrefix,
                           size_t nMaxLines, std::vector<toolsvc::TLogLine>& vOut)
{
    if (sPath.empty()) return;
    FILE* fp = nullptr;
    if (::_wfopen_s(&fp, sPath.c_str(), L"rb") != 0 || !fp) return;

    std::fseek(fp, 0, SEEK_END);
    const long nSize = std::ftell(fp);
    if (nSize <= 0) { std::fclose(fp); return; }

    // 底账文件通常很小；仍设上限防止异常文件（例如写坏的巨型文件）拖慢 UI
    constexpr long kMaxBytes = 512 * 1024;
    long nBegin = (nSize > kMaxBytes) ? (nSize - kMaxBytes) : 0;
    std::fseek(fp, nBegin, SEEK_SET);
    std::string sBuf;
    sBuf.resize(static_cast<size_t>(nSize - nBegin));
    const size_t nRead = std::fread(&sBuf[0], 1, sBuf.size(), fp);
    std::fclose(fp);
    sBuf.resize(nRead);
    if (sBuf.empty()) return;

    // 从尾部开始按行切，凑够 nMaxLines 即停（避免为几千行做全量 split）
    std::vector<std::pair<size_t, size_t> > vRanges;   // [begin, end)
    size_t nEnd = sBuf.size();
    while (nEnd > 0 && vRanges.size() < nMaxLines)
    {
        size_t nStart = sBuf.rfind('\n', nEnd - 1);
        if (nStart == std::string::npos) nStart = 0;
        else ++nStart;
        if (nEnd > nStart) vRanges.push_back(std::make_pair(nStart, nEnd));
        if (nStart == 0) break;
        nEnd = nStart - 1;                             // 跳过该 '\n'
    }
    // 上面是逆序收集，输出时翻回来保持文件内顺序
    for (size_t i = vRanges.size(); i > 0; --i)
    {
        size_t b = vRanges[i - 1].first;
        size_t e = vRanges[i - 1].second;
        if (e > b && sBuf[e - 1] == '\r') --e;

        toolsvc::TLogLine t;
        t.nLevel = static_cast<int>(toolsvc::ELogLevel::kLogInfo);
        t.sText = pszPrefix ? pszPrefix : L"";
        t.sText += DecodeBytes(sBuf, b, e - b);
        if (t.sText.empty()) continue;
        vOut.push_back(t);
    }
}

// 扫描归档目录：取最近 nDays 个日期子目录里、按文件名（含时分秒）最新的
// nFilesPerDay 个 .log。归档目录结构是 <统一目录>\<yyyyMMdd>\HH-MM-SS_名.log。
void AppendArchives(const std::wstring& sRoot, size_t nDays, size_t nFilesPerDay,
                    std::vector<toolsvc::TLogLine>& vOut)
{
    if (sRoot.empty()) return;

    // 1) 收集日期子目录名（8 位数字），降序取最近 nDays 个
    std::vector<std::wstring> vDates;
    WIN32_FIND_DATAW fd = { 0 };
    HANDLE h = ::FindFirstFileW((sRoot + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
        const std::wstring sName = fd.cFileName;
        if (sName.size() != 8) continue;
        bool bAllDigit = true;
        for (size_t i = 0; i < sName.size(); ++i)
            if (sName[i] < L'0' || sName[i] > L'9') { bAllDigit = false; break; }
        if (bAllDigit) vDates.push_back(sName);
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);

    std::sort(vDates.begin(), vDates.end(),
              [](const std::wstring& a, const std::wstring& b) { return a > b; });
    if (vDates.size() > nDays) vDates.resize(nDays);

    // 2) 每个日期目录里取文件名最大的若干个（文件名以 HH-MM-SS_ 开头，字典序即时间序）
    for (size_t d = 0; d < vDates.size(); ++d)
    {
        const std::wstring sDir = sRoot + vDates[d] + L"\\";
        std::vector<std::wstring> vFiles;
        WIN32_FIND_DATAW fd2 = { 0 };
        HANDLE h2 = ::FindFirstFileW((sDir + L"*.log").c_str(), &fd2);
        if (h2 == INVALID_HANDLE_VALUE) continue;
        do
        {
            if (fd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            vFiles.push_back(fd2.cFileName);
        } while (::FindNextFileW(h2, &fd2));
        ::FindClose(h2);
        if (vFiles.empty()) continue;

        std::sort(vFiles.begin(), vFiles.end(),
                  [](const std::wstring& a, const std::wstring& b) { return a > b; });
        if (vFiles.size() > nFilesPerDay) vFiles.resize(nFilesPerDay);

        for (size_t f = 0; f < vFiles.size(); ++f)
        {
            toolsvc::TLogLine hdr;
            hdr.nLevel = toolsvc::CLogDepGraphService::kSectionHeader;
            hdr.sText = L"══ 工具归档 · " + vDates[d] + L"\\" + vFiles[f] + L"（尾 200 行）══";
            vOut.push_back(hdr);

            const std::wstring sPrefix = L"[" + vFiles[f] + L"] ";
            AppendTailOfPlainFile(sDir + vFiles[f], sPrefix.c_str(), 200, vOut);
        }
    }
}

} // namespace

bool CLogDepGraphService::ReadMergedLines(std::vector<TLogLine>& vOut) const
{
    bool bAny = false;

    // ---- 源 1：审计事件流（全量，轮转已限 1MB）----
    {
        std::vector<TLogLine> vAudit;
        if (ReadLogLines(vAudit))
        {
            for (size_t i = 0; i < vAudit.size(); ++i)
                vOut.push_back(vAudit[i]);
            if (!vAudit.empty()) bAny = true;
        }
    }

    // ---- 源 2：诊断底账（每文件尾部 300 行）----
    //   这些文件由 DiagnosticsService / CleanJobService / Page_Program 写入，
    //   现都落在统一目录下；缺哪个就跳过哪个，不因单文件缺失报错。
    {
        static const wchar_t* kDiagFiles[] = {
            L"sysinfo_history.log",
            L"proctop20.log",
            L"services_snapshot.log",
            L"bootevents.log",
            L"clean_scan.log",
            L"fix.log",
        };
        bool bHeaderDone = false;
        for (size_t i = 0; i < sizeof(kDiagFiles) / sizeof(kDiagFiles[0]); ++i)
        {
            const std::wstring sPath = LogService::UnifiedLogPath(kDiagFiles[i]);
            if (sPath.empty()) continue;
            if (::GetFileAttributesW(sPath.c_str()) == INVALID_FILE_ATTRIBUTES) continue;

            if (!bHeaderDone)
            {
                TLogLine hdr;
                hdr.nLevel = kSectionHeader;
                hdr.sText = L"══ 诊断底账（每文件尾 300 行）══";
                vOut.push_back(hdr);
                bHeaderDone = true;
            }

            TLogLine sub;
            sub.nLevel = kSectionHeader;
            sub.sText = L"── " + std::wstring(kDiagFiles[i]) + L" ──";
            vOut.push_back(sub);

            std::wstring sPrefix = L"[";
            sPrefix += kDiagFiles[i];
            sPrefix += L"] ";
            AppendTailOfPlainFile(sPath, sPrefix.c_str(), 300, vOut);
            bAny = true;
        }
    }

    // ---- 源 3：工具归档（最近 1 个日期目录、最多 3 次运行）----
    AppendArchives(LogService::UnifiedLogDir(), 1, 3, vOut);

    return !vOut.empty() || bAny;
}

void CLogDepGraphService::ClearLogFile() const
{
    // 清空日志文件（ofstream 默认截断模式）
    std::ofstream(LogPath(), std::ios::binary);
}

TDepView CLogDepGraphService::BuildDepView() const
{
    TDepView view;
    view.bHasNodes = !m_vNodes.empty();

    for (size_t i = 0; i < m_vNodes.size(); ++i)
    {
        const TDepNode& n = m_vNodes[i];
        if (n.sLabel.empty()) continue;

        if (n.nParent < 0) ++view.nRoots; else ++view.nDeps;

        TDepRow row;
        row.bRoot  = (n.nParent < 0);
        row.sLabel = n.sLabel;
        AppendIndent(row.sText, n.nLevel);
        row.sText += row.bRoot ? L"\u25CF " : L"\u2514\u2500 ";
        row.sText += n.sLabel;
        view.rows.push_back(std::move(row));
    }
    return view;
}

std::wstring CLogDepGraphService::BuildDepCopyText() const
{
    std::wstring sAll;
    for (size_t i = 0; i < m_vNodes.size(); ++i)
    {
        const TDepNode& n = m_vNodes[i];
        if (n.sLabel.empty()) continue;

        AppendIndent(sAll, n.nLevel);
        sAll += (n.nParent < 0) ? L"* " : L"- ";
        sAll += n.sLabel;
        sAll += L"\r\n";
    }
    return sAll;
}

} // namespace toolsvc
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
