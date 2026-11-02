// LogService.cpp —— 统一日志入口实现
//
// 三个历史写入点（LogService::AppendToFile / AppendToFileBatch /
// HostExports::DismWriteLog）此前各自用 std::wofstream 直写，存在三个 P0：
//   1) 未 imbue UTF-8 codecvt → C locale 下中文转换失败，整行截断；
//   2) 多线程无锁并发 append + 写入侧轮转，产生半行粘连；
//   3) 落点固定在 EXE 目录，装到 Program Files 时写失败且完全静默。
// 现在全部收敛到本文件的 Write()：显式 UTF-8、互斥保护、统一目录 + 回退链、
// 失败计数与一次性告警。外部只应调用 Write()/WriteAuto()，不要再自己开文件。
//
// 独立于预编译头：不 include stdafx.h / UIlib.h，仅系统头 + CRT + Utils。

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <deque>   // [L-14] 审计环 O(1) 出队

#include "FormatW.h"
#include "LogService.h"

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

using DuiLib::CDuiString;

namespace
{
// 日志文件上限 1 MB：超过即丢弃最早的行，只保留尾部。
// 为什么在写入侧轮转而不是在读取侧截断：
//   读取侧截断只是"少显示几条"，文件仍会无限增长；写入侧轮转才能让文件
//   本身稳定在 1 MB，磁盘占用与后续读取耗时都可预期。
constexpr ULONGLONG kLogFileMaxBytes  = 1024ull * 1024ull;
// 触发轮转后保留的尾部比例上限（留 3/4，避免每写一行就轮转一次）
constexpr ULONGLONG kLogFileKeepBytes = kLogFileMaxBytes * 3 / 4;
// 审计环形缓冲上限
constexpr size_t kRingMax = 1000;

// 进程内共享状态（文件作用域单例，避免头文件暴露实现细节）
struct TLogState
{
    std::mutex        mtx;             // 保护：目录初始化 / 文件写 / 审计环 / 计数
    std::wstring      sDir;            // 统一日志目录（含尾反斜杠）
    bool              bInited = false; // 目录是否已完成初始化
    bool              bFellBack = false;// 是否发生过回退
    unsigned long long nFail = 0;      // 写失败累计
    bool              bWarned = false; // 是否已就写失败告警过一次
    bool              bPendingAlert = false;   // [L-15] 有未取走的 UI 告警
    std::wstring      sPendingAlertText;       // [L-15] 告警正文
    ELogSeverity         minLevel = ELogSeverity::kInfo;
    // [L-14] 审计环形缓冲改用 deque：
    //   原实现是 vector + erase(begin())，每次超限都要把剩余元素整体前移，
    //   是 O(n) 搬移；日志风暴时每行都触发一次，代价随缓冲长度线性增长。
    //   deque 的 push_back / pop_front 都是 O(1)，且 RingAt 仍可 O(1) 随机访问。
    std::deque<CDuiString> vRing;
};

TLogState& State()
{
    // C++11 起函数内 static 的初始化是线程安全的（magic statics）
    static TLogState s;
    return s;
}

// 目录是否存在
bool DirExists(const std::wstring& sDir)
{
    const DWORD dw = ::GetFileAttributesW(sDir.c_str());
    return dw != INVALID_FILE_ATTRIBUTES && (dw & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// 逐级建目录（父目录可能也不存在）
bool EnsureDirectoryDeep(const std::wstring& sDir)
{
    if (sDir.empty()) return false;
    if (DirExists(sDir)) return true;

    // 递归创建父级
    std::wstring sParent = sDir;
    while (!sParent.empty() && (sParent[sParent.size() - 1] == L'\\' || sParent[sParent.size() - 1] == L'/'))
        sParent.erase(sParent.size() - 1);
    const size_t nPos = sParent.find_last_of(L"\\/");
    if (nPos != std::wstring::npos && nPos > 2)
        EnsureDirectoryDeep(sParent.substr(0, nPos));   // 父级失败不阻断，CreateDirectory 会再报

    if (::CreateDirectoryW(sDir.c_str(), nullptr)) return true;
    const DWORD dwErr = ::GetLastError();
    return dwErr == ERROR_ALREADY_EXISTS || DirExists(sDir);
}

// 可写性探针：真正试写一个临时文件，能写才算可写。
//   仅看 GetFileAttributes 不够——Program Files 下目录存在但普通权限写不进去。
bool ProbeWritable(const std::wstring& sDir)
{
    if (!EnsureDirectoryDeep(sDir)) return false;
    // [L-18] 用 std::wstring 直接拼接，不再依赖定长缓冲：
    //   原实现写死 MAX_PATH*2，超长安装目录会被截断成非法路径，
    //   探针必然失败 → 误判"不可写" → 日志白白降级到 LOCALAPPDATA。
    std::wstring sProbe = sDir;
    if (!sProbe.empty() && sProbe.back() != L'\\' && sProbe.back() != L'/')
        sProbe += L'\\';
    sProbe += L".write_probe";

    HANDLE h = ::CreateFileW(sProbe.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                             nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    ::CloseHandle(h);          // DELETE_ON_CLOSE：句柄关闭即消失，无需清理
    return true;
}

// EXE 所在目录（含尾反斜杠）；用动态缓冲避免 MAX_PATH 截断
std::wstring ExeDir()
{
    std::wstring sBuf;
    sBuf.resize(MAX_PATH);
    for (;;)
    {
        const DWORD dw = ::GetModuleFileNameW(nullptr, sBuf.data(),
                                             static_cast<DWORD>(sBuf.size()));
        if (dw == 0) return std::wstring();
        if (dw < sBuf.size()) break;
        // 缓冲不足：按返回值扩大重试（长路径安装目录）
        sBuf.resize(static_cast<size_t>(dw) + 64);
    }
    sBuf.resize(wcslen(sBuf.c_str()));
    const size_t nPos = sBuf.find_last_of(L"\\/");
    if (nPos != std::wstring::npos) sBuf.resize(nPos + 1);
    return sBuf;
}

// 取已知文件夹路径（失败返回空）
std::wstring KnownFolder(REFKNOWNFOLDERID id)
{
    PWSTR p = nullptr;
    if (FAILED(::SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p)) || !p)
        return std::wstring();
    std::wstring s(p);
    ::CoTaskMemFree(p);
    if (!s.empty() && s[s.size() - 1] != L'\\') s += L'\\';
    return s;
}

// 回退链：EXE\Logs → %LOCALAPPDATA%\DismTool\Logs → %USERPROFILE%\DismTool\Logs → %TEMP%
//   EXE 目录可写是便携发布的常态（bin64 整目录分发），因此作为首选。
bool ResolveLogDir(std::wstring& sOut, bool& bFellBack)
{
    bFellBack = false;

    const std::wstring sExe = ExeDir();
    if (!sExe.empty())
    {
        const std::wstring sTry = sExe + L"Logs";
        if (ProbeWritable(sTry)) { sOut = sTry + L"\\"; return true; }
        bFellBack = true;      // EXE 目录存在但不可写（Program Files 场景）
    }

    const std::wstring sLocal = KnownFolder(FOLDERID_LocalAppData);
    if (!sLocal.empty())
    {
        const std::wstring sTry = sLocal + L"DismTool\\Logs";
        if (ProbeWritable(sTry)) { sOut = sTry + L"\\"; return true; }
    }

    const std::wstring sProfile = KnownFolder(FOLDERID_Profile);
    if (!sProfile.empty())
    {
        const std::wstring sTry = sProfile + L"DismTool\\Logs";
        if (ProbeWritable(sTry)) { sOut = sTry + L"\\"; return true; }
    }

    // 最后兜底：临时目录（至少不丢日志）
    wchar_t szTemp[MAX_PATH] = { 0 };
    if (::GetTempPathW(MAX_PATH, szTemp) > 0)
    {
        std::wstring sTry = szTemp;
        while (!sTry.empty() && sTry[sTry.size() - 1] == L'\\') sTry.erase(sTry.size() - 1);
        sTry += L"\\DismTool_Logs";
        if (ProbeWritable(sTry)) { sOut = sTry + L"\\"; bFellBack = true; return true; }
    }

    bFellBack = true;
    return false;
}

// 一次性迁移：把散落的历史日志搬进统一目录。
//   只做 MoveFileEx 改名（同盘瞬时、失败保原件），不做复制删除，
//   保证任何一步失败都不会丢历史。
void MigrateOnce(const std::wstring& sNewDir)
{
    // 1) EXE 根下的 action_log.jsonl → Logs\action_log.jsonl
    const std::wstring sExe = ExeDir();
    if (!sExe.empty())
    {
        const std::wstring sOld = sExe + L"action_log.jsonl";
        const std::wstring sNew = sNewDir + L"action_log.jsonl";
        if (::GetFileAttributesW(sOld.c_str()) != INVALID_FILE_ATTRIBUTES &&
            ::GetFileAttributesW(sNew.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            ::MoveFileExW(sOld.c_str(), sNew.c_str(), MOVEFILE_WRITE_THROUGH);
        }
    }

    // 2) %LOCALAPPDATA%\DismTool_Clone\logs\*.log → 统一目录（同名跳过，保新弃迁）
    const std::wstring sLocal = KnownFolder(FOLDERID_LocalAppData);
    if (sLocal.empty()) return;
    const std::wstring sOldDir = sLocal + L"DismTool_Clone\\logs";
    WIN32_FIND_DATAW fd = { 0 };
    HANDLE hFind = ::FindFirstFileW((sOldDir + L"\\*.log").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring sSrc = sOldDir + L"\\" + fd.cFileName;
        const std::wstring sDst = sNewDir + fd.cFileName;
        if (::GetFileAttributesW(sDst.c_str()) == INVALID_FILE_ATTRIBUTES)
            ::MoveFileExW(sSrc.c_str(), sDst.c_str(), MOVEFILE_WRITE_THROUGH);
    } while (::FindNextFileW(hFind, &fd));
    ::FindClose(hFind);
}

// 宽字符 → UTF-8（显式转换，这是修掉中文截断的关键）
// 宽 → UTF-8。
//   注意 nLen 默认 -1 表示"到 NUL 为止，且**包含** NUL"，WideCharToMultiByte
//   会把终止符一起编码进输出。早先直接透传 -1，导致每行 msg 末尾多一个 NUL，
//   实测写出来的 JSONL 里就是 "..." 尾巴（读侧显示成乱符号）。
//   这里统一按 wcslen 显式给长度，绝不让 NUL 进入正文。
std::string ToUtf8(const wchar_t* pszText)
{
    if (!pszText || !pszText[0]) return std::string();
    const int nLen = static_cast<int>(wcslen(pszText));
    if (nLen <= 0) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, pszText, nLen, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s;
    s.resize(static_cast<size_t>(n));
    ::WideCharToMultiByte(CP_UTF8, 0, pszText, nLen, s.data(), n, nullptr, nullptr);
    return s;
}

// JSON 字符串转义（引号 / 反斜杠 / 控制字符）
//   注意 \n\r\t 必须转义，否则多行文本会把 JSONL 写成多行、破坏逐行解析。
std::string JsonEscape(const std::string& sIn)
{
    std::string sOut;
    sOut.reserve(sIn.size() + 16);
    for (size_t i = 0; i < sIn.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(sIn[i]);
        switch (c)
        {
        case '"':  sOut += "\\\""; break;
        case '\\': sOut += "\\\\"; break;
        case '\n': sOut += "\\n";  break;
        case '\r': sOut += "\\r";  break;
        case '\t': sOut += "\\t";  break;
        default:
            if (c < 0x20)
            {
                // 其它控制字符：\u00XX
                char szBuf[8] = { 0 };
                std::snprintf(szBuf, sizeof(szBuf), "\\u%04x", c);
                sOut += szBuf;
            }
            else
            {
                sOut += static_cast<char>(c);
            }
        }
    }
    return sOut;
}

// 毫秒 + 本地时区偏移的 ISO8601 时间戳
//   例：2026-10-28T14:04:32.187+08:00
std::string MakeTimestampUtf8()
{
    SYSTEMTIME st = { 0 };
    ::GetLocalTime(&st);          // 已含毫秒
    TIME_ZONE_INFORMATION tzi = { 0 };
    const DWORD dwTz = ::GetTimeZoneInformation(&tzi);
    int nBiasMin = tzi.Bias;
    if (dwTz == TIME_ZONE_ID_DAYLIGHT)      nBiasMin += tzi.DaylightBias;
    else if (dwTz == TIME_ZONE_ID_STANDARD) nBiasMin += tzi.StandardBias;

    // Bias 的符号与 ISO 偏移相反：UTC = local + Bias，故偏移 = -Bias
    const int nOffMin = -nBiasMin;
    const char chSign = (nOffMin >= 0) ? '+' : '-';
    const int nAbs = (nOffMin >= 0) ? nOffMin : -nOffMin;

    char szBuf[64] = { 0 };
    std::snprintf(szBuf, sizeof(szBuf),
                  "%04d-%02d-%02dT%02d:%02d:%02d.%03d%c%02d:%02d",
                  st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                  chSign, nAbs / 60, nAbs % 60);
    return std::string(szBuf);
}

const char* LevelName(ELogSeverity lv)
{
    switch (lv)
    {
    case ELogSeverity::kTrace: return "trace";
    case ELogSeverity::kDebug: return "debug";
    case ELogSeverity::kInfo:  return "info";
    case ELogSeverity::kWarn:  return "warn";
    case ELogSeverity::kError: return "error";
    case ELogSeverity::kFatal: return "fatal";
    }
    return "info";
}

// 旧 UI 分级（HostInterfaces.h 的 ELogLevel：0=info / 1=warn / 2=error）
// → 落盘侧 ELogSeverity。
//   必须显式映射而不是 static_cast：两套枚举的数值基准完全不同
//   （UI 的 info 是 0，严重度枚举的 info 是 2），强转会整体错位——
//   这正是"warn 记成 info、error 记成 warn"这类级别失真的来源。
ELogSeverity FromUiLevel(int n)
{
    // [R221] Two DIFFERENT value domains reach this function, and conflating them
    //   silently mislabelled every warning as an error:
    //     * ELogSeverity enum : kTrace=0 kDebug=1 kInfo=2 kWarn=3 kError=4 kFatal=5
    //       (DetectLevel and LOGSRV_* constants use this)
    //     * legacy UI level : 0=info, 1=warn, 2=error
    //       (a few call sites still pass this)
    //   The old body only knew the UI domain, so LOGSRV_WARN (3) fell into `n >= 2`
    //   and became kError. The log_effect probe caught it: it writes with
    //   LOGSRV_WARN and asserted `"level":"warn"`, and the file said "error".
    //   Enums are dense and start at kInfo=2 for real severity, so values >= 2 that
    //   are already valid enum members are passed through; anything below 2 keeps the
    //   legacy UI mapping.
    if (n >= static_cast<int>(ELogSeverity::kInfo) &&
        n <= static_cast<int>(ELogSeverity::kFatal))
    {
        return static_cast<ELogSeverity>(n);
    }
    if (n == 1) return ELogSeverity::kWarn;     // legacy UI: 1 = warn
    if (n >= 2) return ELogSeverity::kError;    // legacy UI: 2 = error
    return ELogSeverity::kInfo;                 // legacy UI: 0 = info
}
} // namespace

// ============================================================
//  路径
// ============================================================
std::mutex& LogService::Mutex()
{
    return State().mtx;
}

const std::wstring& LogService::UnifiedLogDir()
{
    TLogState& s = State();
    std::lock_guard<std::mutex> lk(s.mtx);
    if (!s.bInited)
    {
        s.bInited = true;                 // 先置位：即使解析失败也只在首次尝试
        std::wstring sDir;
        bool bFallback = false;
        if (ResolveLogDir(sDir, bFallback))
        {
            s.sDir = sDir;
            s.bFellBack = bFallback;
            MigrateOnce(s.sDir);
        }
    }
    return s.sDir;
}

std::wstring LogService::UnifiedLogPath(const wchar_t* pszFileName)
{
    const std::wstring& sDir = UnifiedLogDir();
    if (sDir.empty() || !pszFileName) return std::wstring();
    return sDir + pszFileName;
}

bool LogService::FellBack()
{
    UnifiedLogDir();                      // 确保已初始化
    std::lock_guard<std::mutex> lk(State().mtx);
    return State().bFellBack;
}

bool LogService::TakePendingAlert(std::wstring& sText)
{
    TLogState& s = State();
    std::lock_guard<std::mutex> lk(s.mtx);
    if (!s.bPendingAlert) return false;
    s.bPendingAlert = false;          // 只弹一次
    sText = s.sPendingAlertText;
    return true;
}

unsigned long long LogService::FailCount()
{
    std::lock_guard<std::mutex> lk(State().mtx);
    return State().nFail;
}

void LogService::SetMinLevel(ELogSeverity lv)
{
    std::lock_guard<std::mutex> lk(State().mtx);
    State().minLevel = lv;
}

ELogSeverity LogService::GetMinLevel()
{
    std::lock_guard<std::mutex> lk(State().mtx);
    return State().minLevel;
}

// ============================================================
//  级别识别（兼容兜底：旧调用点只传文本）
// ============================================================
int LogService::DetectLevel(const wchar_t* pszLine)
{
    if (!pszLine) return LOGSRV_INFO;

    CDuiString s = pszLine;
    s.MakeLower();
    // [!] 是 DISM/外部工具沿用的告警前缀，此前完全不被识别、全部记成 info
    if (s.Find(L"[error]") >= 0 || s.Find(L"[fatal]") >= 0) return static_cast<int>(ELogSeverity::kError);
    if (s.Find(L"[warn]")  >= 0 || s.Find(L"[warning]") >= 0) return static_cast<int>(ELogSeverity::kWarn);
    if (s.Find(L"[!]")     >= 0) return static_cast<int>(ELogSeverity::kWarn);
    if (s.Find(L"[debug]") >= 0) return static_cast<int>(ELogSeverity::kDebug);
    if (s.Find(L"[trace]") >= 0) return static_cast<int>(ELogSeverity::kTrace);
    return LOGSRV_INFO;
}

// ============================================================
//  审计环
// ============================================================
void LogService::RingPush(const wchar_t* pszLine, ELogSeverity /*lv*/)
{
    if (!pszLine) return;
    TLogState& s = State();
    std::lock_guard<std::mutex> lk(s.mtx);
    s.vRing.push_back(CDuiString(pszLine));
    while (s.vRing.size() > kRingMax) s.vRing.pop_front();   // O(1)，不搬移
}

size_t LogService::RingCount()
{
    TLogState& s = State();
    std::lock_guard<std::mutex> lk(s.mtx);
    return s.vRing.size();
}

const DuiLib::CDuiString LogService::RingAt(size_t i)
{
    // 返回值拷贝而不是引用：内部持锁取值，返回引用会在锁释放后悬空
    // （多线程 push 时 vector 可能重新分配）。
    TLogState& s = State();
    std::lock_guard<std::mutex> lk(s.mtx);
    if (i >= s.vRing.size()) return DuiLib::CDuiString();
    return s.vRing[i];
}

void LogService::RingClear()
{
    TLogState& s = State();
    std::lock_guard<std::mutex> lk(s.mtx);
    s.vRing.clear();
}

// 把一条日志推进审计环，并返回当前日志文件路径（空 = 目录不可用）。
//   拆出来是为了让 Write 的职责收敛为"编排"，而不是把入环/拼路径也塞在一起。
std::wstring PushToRingAndResolvePath(const wchar_t* pszMsg)
{
    // [R221] Resolve the directory BEFORE taking the lock.
    //   UnifiedLogDir() is lazy-initialised and itself locks s.mtx. Reading s.sDir
    //   directly here (as this function used to) meant the very first Write() saw an
    //   empty sDir, returned "", and the caller treated that as "log dir unusable" and
    //   silently kept the line in the in-memory ring only. Nothing ever wrote a byte to
    //   disk, so action_log.jsonl never appeared -- which is exactly what the
    //   log_effect probe has been reporting as "append creates the log file: exists=0".
    //   Any log path that bypasses UnifiedLogDir() has this bug; calling it first is
    //   the fix, and it is idempotent.
    const std::wstring sDir = LogService::UnifiedLogDir();

    TLogState& s = State();
    std::lock_guard<std::mutex> lk(s.mtx);
    s.vRing.push_back(CDuiString(pszMsg));
    while (s.vRing.size() > kRingMax) s.vRing.pop_front();   // O(1)
    if (sDir.empty()) return std::wstring();
    return sDir + L"action_log.jsonl";
}

// 组装一行 JSON（UTF-8 字节）。字段口径：ts / level / module / tid / msg。
std::string BuildJsonLine(ELogSeverity lv, const wchar_t* pszModule, const wchar_t* pszMsg)
{
    const std::string sMsg = JsonEscape(ToUtf8(pszMsg));
    const std::string sMod = JsonEscape(ToUtf8(pszModule && pszModule[0] ? pszModule : L"app"));

    char szTid[32] = { 0 };
    std::snprintf(szTid, sizeof(szTid), "%lu",
                  static_cast<unsigned long>(::GetCurrentThreadId()));

    std::string sLine;
    sLine.reserve(sMsg.size() + 128);
    sLine += "{\"ts\":\"";      sLine += MakeTimestampUtf8();
    sLine += "\",\"level\":\""; sLine += LevelName(lv);
    sLine += "\",\"module\":\""; sLine += sMod;
    sLine += "\",\"tid\":";    sLine += szTid;
    sLine += ",\"msg\":\"";    sLine += sMsg;
    sLine += "\"}\r\n";
    return sLine;
}

// 追加写入：整行一次 fwrite，持锁执行，杜绝跨线程半行交错。
bool AppendBytesToFile(const std::wstring& sPath, const std::string& sLine)
{
    TLogState& s = State();
    std::lock_guard<std::mutex> lk(s.mtx);
    bool bOk = false;
    FILE* fp = nullptr;
    if (::_wfopen_s(&fp, sPath.c_str(), L"ab") == 0 && fp)
    {
        const size_t nWrote = std::fwrite(sLine.data(), 1, sLine.size(), fp);
        std::fclose(fp);
        bOk = (nWrote == sLine.size());
    }
    if (!bOk) ++s.nFail;
    return bOk;
}

// 写失败兜底：Debug 输出 + 一次性 UI 告警登记（不刷屏）。
void ReportWriteFailure(const std::string& sLine)
{
    ::OutputDebugStringA("[log] write failed: ");
    ::OutputDebugStringA(sLine.c_str());
    TLogState& s = State();
    std::lock_guard<std::mutex> lk(s.mtx);
    if (s.bWarned) return;
    s.bWarned = true;
    ::OutputDebugStringW(L"[log] WARNING: 日志写入失败，后续错误只在此提示一次；"
                         L"请检查日志目录权限或磁盘空间。\r\n");
    s.bPendingAlert = true;
    s.sPendingAlertText =
        L"日志写入失败：请检查日志目录权限或磁盘空间。"
        L"后续同类错误不再重复提示，可通过 LogService::FailCount() 查看累计次数。";
}

// ============================================================
//  唯一写入口
// ============================================================
// 轮转实现定义在 Write 之后；先声明，避免为它单独引入结构体或改文件顺序。
static void LogServiceWriteRotate(const std::wstring& sPath);

void LogService::Write(ELogSeverity lv, const wchar_t* pszModule, const wchar_t* pszMsg)
{
    if (!pszMsg) return;
    // 级别过滤：低于最低级别直接丢弃（TRACE/DEBUG 默认不落盘）
    if (static_cast<int>(lv) < static_cast<int>(GetMinLevel())) return;

    const std::wstring sPath = PushToRingAndResolvePath(pszMsg);
    if (sPath.empty()) return;              // 连临时目录都不可写：只留内存环

    const std::string sLine = BuildJsonLine(lv, pszModule, pszMsg);
    if (!AppendBytesToFile(sPath, sLine))   // 落盘 + 失败计数
    {
        ReportWriteFailure(sLine);
        return;
    }
    LogServiceWriteRotate(sPath);
}

// 轮转：超过 1MB 时只保留尾部约 3/4，按整行截断。
//   关键点：轮转全程持锁 —— 否则"读尾部→写 tmp→替换"期间别的线程
//   仍能 append 到旧文件，替换后就吞掉刚写入的行（原 P0-3 的成因之一）。
void LogServiceWriteRotate(const std::wstring& sPath)
{
    TLogState& s = State();
    std::lock_guard<std::mutex> lk(s.mtx);

    WIN32_FILE_ATTRIBUTE_DATA fad = { 0 };
    if (!::GetFileAttributesExW(sPath.c_str(), GetFileExInfoStandard, &fad)) return;
    ULARGE_INTEGER ul;
    ul.HighPart = fad.nFileSizeHigh;
    ul.LowPart  = fad.nFileSizeLow;
    if (ul.QuadPart <= kLogFileMaxBytes) return;

    FILE* fp = nullptr;
    if (::_wfopen_s(&fp, sPath.c_str(), L"rb") != 0 || !fp) return;
    std::fseek(fp, 0, SEEK_END);
    const long nSize = std::ftell(fp);
    if (nSize <= static_cast<long>(kLogFileKeepBytes)) { std::fclose(fp); return; }

    const long nSkip = nSize - static_cast<long>(kLogFileKeepBytes);
    std::fseek(fp, nSkip, SEEK_SET);
    std::string sTail;
    sTail.resize(static_cast<size_t>(nSize - nSkip));
    const size_t nRead = std::fread(&sTail[0], 1, sTail.size(), fp);
    std::fclose(fp);
    sTail.resize(nRead);

    // 丢掉起始处可能被截断的半行，保证首行仍是完整 JSON
    const size_t nStart = sTail.find('\n');
    if (nStart == std::string::npos) return;    // 单行超限：放弃轮转（不破坏文件）

    const std::wstring sTmp = sPath + L".tmp";
    FILE* fo = nullptr;
    if (::_wfopen_s(&fo, sTmp.c_str(), L"wb") != 0 || !fo) return;
    const size_t nWrote = std::fwrite(sTail.data() + nStart + 1, 1,
                                      sTail.size() - nStart - 1, fo);
    std::fclose(fo);
    if (nWrote != sTail.size() - nStart - 1)
    {
        ::DeleteFileW(sTmp.c_str());
        ++s.nFail;
        return;
    }
    // 原子替换；失败保留原文件（不丢日志）
    if (!::MoveFileExW(sTmp.c_str(), sPath.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        ::DeleteFileW(sTmp.c_str());
        ++s.nFail;
    }
}

void LogService::WriteAuto(const wchar_t* pszMsg)
{
    Write(FromUiLevel(DetectLevel(pszMsg)), L"app", pszMsg);
}

// ============================================================
//  实例接口（旧签名保留，内部走统一入口）
// ============================================================
void LogService::AppendToFile(const wchar_t* pszLine, int nLevel)
{
    Write(FromUiLevel(nLevel), L"app", pszLine);
}

void LogService::AppendToFileBatch(const std::vector<std::pair<CDuiString, int> >& vItems)
{
    // 逐条走统一入口即可：Write 内部是"一次 fopen 一行"，
    // 但批量场景（日志风暴）真正的开销在文件开关，这里合批一次打开。
    if (vItems.empty()) return;

    TLogState& s = State();
    std::string sBatch;
    sBatch.reserve(vItems.size() * 128);

    for (size_t i = 0; i < vItems.size(); ++i)
    {
        // 批量项同样按"枚举严重度"处理：NormalizeLevel 只接受枚举语义的 int，
        //   而这里 vItems 的 nLevel 来自 UI 的 ELogLevel(0=info/1=warn/2=error)，
        //   两者数值不同，必须显式映射，不能强转。
        const ELogSeverity lvItem = FromUiLevel(vItems[i].second);
        if (static_cast<int>(lvItem) < static_cast<int>(GetMinLevel())) continue;
        const std::string sMsg = JsonEscape(ToUtf8(vItems[i].first.GetData()));
        char szTid[32] = { 0 };
        std::snprintf(szTid, sizeof(szTid), "%lu",
                      static_cast<unsigned long>(::GetCurrentThreadId()));
        // [L-09] 逐行取时：同一批里数百行共用一个时间戳会丧失时序价值，
        //   风暴场景下尤其明显（原来整批只取一次 GetLocalTime）。
        sBatch += "{\"ts\":\"";     sBatch += MakeTimestampUtf8();
        sBatch += "\",\"level\":\""; sBatch += LevelName(lvItem);
        sBatch += "\",\"module\":\"app\",\"tid\":"; sBatch += szTid;
        sBatch += ",\"msg\":\"";    sBatch += sMsg;
        sBatch += "\"}\r\n";
    }
    if (sBatch.empty()) return;

    std::wstring sPath;
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        for (size_t i = 0; i < vItems.size(); ++i)
        {
            s.vRing.push_back(vItems[i].first);
            while (s.vRing.size() > kRingMax) s.vRing.pop_front();   // O(1)
        }
        if (s.sDir.empty()) return;
        sPath = s.sDir + L"action_log.jsonl";

        bool bOk = false;
        FILE* fp = nullptr;
        if (::_wfopen_s(&fp, sPath.c_str(), L"ab") == 0 && fp)
        {
            const size_t nWrote = std::fwrite(sBatch.data(), 1, sBatch.size(), fp);
            std::fclose(fp);
            bOk = (nWrote == sBatch.size());
        }
        if (!bOk) ++s.nFail;
    }
    LogServiceWriteRotate(sPath);
}

void LogService::BufferOffline(const wchar_t* pszLine)
{
    m_vLogBuffer.push_back(CDuiString(pszLine ? pszLine : L""));
    if (m_vLogBuffer.size() > 300) m_vLogBuffer.erase(m_vLogBuffer.begin());
}

size_t LogService::BufferedCount() const
{
    return m_vLogBuffer.size();
}

const CDuiString& LogService::BufferedAt(size_t i) const
{
    static const CDuiString sEmpty;
    if (i >= m_vLogBuffer.size()) return sEmpty;
    return m_vLogBuffer[i];
}

void LogService::ClearBuffered()
{
    m_vLogBuffer.clear();
}
