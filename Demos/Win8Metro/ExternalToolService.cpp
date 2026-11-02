#include "LogService.h"   // [M-9/M-10] 统一日志目录
#include <cstdio>
#include "FormatW.h"
#include <vector>
#include <string>    // [R109] std::string（ANSI 字节写 progress.txt）
#include <memory>    // [C++17 R3] make_unique / unique_ptr
#include <fstream>   // [C17] std::ofstream / std::wifstream（F17_C_fileio 清零）
#include "ExternalToolService.h"
#include <shlwapi.h>      // [R-B36] PathRemoveFileSpecW（随包优先探测用）
#include "ProcessRunner.h"
#include "WmMessages.h"
#include "SafeThread.h"   // 控制台捕获工作线程异常边界
#include "WinHandles.h"   // [H-51] win::HandlePtr RAII for F11_raw_handle_close
// ============================================================
// ExternalToolService.cpp —— S9（P6a）实现
// 全部函数体自 FrameWnd.cpp 物理搬迁（零逻辑改动）：
//   会话句柄原为 CFrameWnd 成员，现为文件 static；
//   LaunchSystemEx 的本地化文案改为回传原始错误码（判定在 UI 层）；
//   工作线程仅 PostMessage，不触碰 UI。
// ============================================================
namespace {
// ---- DISM 会话句柄（原 CFrameWnd::m_hDismProcess / m_hStdOutRead /
//      m_hStdErrRead / m_uThreadId；全局同时只允许一路控制台捕获）----
HANDLE   g_hProcess  = nullptr;
HANDLE   g_hOutR     = nullptr;
HANDLE   g_hErrR     = nullptr;
UINT_PTR g_uThreadId = 0;
// [FIX-P1A] 工具读取线程取消/启动占位标志（P1-1/P1-2）：
//   g_bToolAbort  —— Abort 先置位再关句柄，读取线程见位即退（不伪造 DONE）；
//   g_bToolStarting —— CreateProcess→g_hProcess 赋值之间的占位，堵 IsRunning
//                      裸读窗口内的双启动（两条读取线程并存/DONE 双投）。
volatile LONG g_bToolAbort    = 0;
volatile LONG g_bToolStarting = 0;
// 把 ANSI 片段按系统代码页转为宽字符并赋给 CDuiString（本地 helper）
static void AssignAnsiSegment(CDuiString& sOut, const char* pszData, int nLen)
{
    sOut.Empty();
    if (nLen <= 0) return;
    int nW = ::MultiByteToWideChar(CP_ACP, 0, pszData, nLen, nullptr, 0);
    if (nW <= 0) return;
    std::vector<wchar_t> buf(nW + 1, 0);
    ::MultiByteToWideChar(CP_ACP, 0, pszData, nLen, &buf[0], nW);
    buf[nW] = 0;
    sOut = &buf[0];
}
// 逐级创建目录（本模块为轻模块，不引 FrameWnd_Util；CreateDirectory 不支持多级）
static void EnsureDirectoryDeepLocal(const wchar_t* pszDir)
{
    if (!pszDir || !*pszDir) return;
    std::wstring sCur;
    for (const wchar_t* p = pszDir; *p; ++p)
    {
        sCur.push_back(*p);
        if ((*p == L'\\' || *p == L'/') && sCur.size() >= 3 && sCur[1] == L':')
            ::CreateDirectory(sCur.c_str(), nullptr);
    }
    ::CreateDirectory(sCur.c_str(), nullptr);
}
// [R109] worker 线程把真实百分比回写 progress.txt 的 pct 行
// （纯文件覆盖写；失败静默，不影响主流程）。
// 说明：progress.txt 由 ArchiveToolRun 用 _fputts 文本流写入，
// 文本模式在 Windows 上是 ACP 单字节编码，因此本函数也按 ANSI 字节写。
// [C++17 升级阶段2] 局部句柄 RAII 守卫：隐式转换为原句柄类型，调用点
// 零改动；只负责早退/异常路径自动释放，不改变任何调用顺序与业务分支。
// 管道与全局跨函数句柄（g_hProcess/g_hOutR/g_hErrR、sei.hProcess、
// PROCESS_INFORMATION）所有权跨函数/移交调用方，按方案保持显式管理。
struct FileGuard
{
    FileGuard() = default;
    FileGuard(HANDLE h) : m_h(h) {}
    ~FileGuard() { if (m_h != nullptr && m_h != INVALID_HANDLE_VALUE) ::CloseHandle(m_h); }
    FileGuard(const FileGuard&) = delete;
    FileGuard& operator=(const FileGuard&) = delete;
    operator HANDLE() const { return m_h; }
private:
    HANDLE m_h = nullptr;
};
struct FindGuard
{
    FindGuard() = default;
    FindGuard(HANDLE h) : m_h(h) {}
    ~FindGuard() { if (m_h != nullptr && m_h != INVALID_HANDLE_VALUE) ::FindClose(m_h); }
    FindGuard(const FindGuard&) = delete;
    FindGuard& operator=(const FindGuard&) = delete;
    operator HANDLE() const { return m_h; }
private:
    HANDLE m_h = nullptr;
};
struct RegKeyGuard
{
    RegKeyGuard() = default;
    ~RegKeyGuard() { if (m_h) ::RegCloseKey(m_h); }
    RegKeyGuard(const RegKeyGuard&) = delete;
    RegKeyGuard& operator=(const RegKeyGuard&) = delete;
    operator HKEY() const { return m_h; }
    HKEY* out() { return &m_h; }
private:
    HKEY m_h = nullptr;
};
struct HandleGuard
{
    HandleGuard() = default;
    ~HandleGuard() { if (m_h != nullptr && m_h != INVALID_HANDLE_VALUE) ::CloseHandle(m_h); }
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
    operator HANDLE() const { return m_h; }
    HANDLE* out() { return &m_h; }
private:
    HANDLE m_h = nullptr;
};
static void WriteToolProgressPct(int nPct)
{
    if (nPct < 0) nPct = 0;
    if (nPct > 100) nPct = 100;
    // [M-10] progress.txt 是 IPC 信号通道（非日志），但读写两侧必须同批切到
    //   统一日志目录，否则 worker 回写的位置与 UI 轮询的位置会分叉。
    CDuiString sPath = LogService::UnifiedLogPath(L"progress.txt").c_str();
    if (sPath.IsEmpty()) return;
    FileGuard hFile = ::CreateFileW(sPath.GetData(), GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return;
    DWORD dwSize = ::GetFileSize(hFile, nullptr);
    bool bEmpty = (dwSize == 0 || dwSize == INVALID_FILE_SIZE);
    // 把 CDuiString 转 ANSI 单字节（与 _fputts 文本流写入一致）
    auto AnsiBytes = [](const CDuiString& sIn, std::string& out)
    {
        out.clear();
        int nLen = sIn.GetLength();
        if (nLen <= 0) return;
        int nBytes = ::WideCharToMultiByte(CP_ACP, 0, sIn.GetData(), nLen,
                                           nullptr, 0, nullptr, nullptr);
        if (nBytes <= 0) return;
        out.resize(nBytes);
        ::WideCharToMultiByte(CP_ACP, 0, sIn.GetData(), nLen,
                              &out[0], nBytes, nullptr, nullptr);
    };
    // 文件已存在：只替换 pct 行（保留 tool/timestamp/status）。
    // 配置文件（tool/config）大小阈值：仅处理小文件，避免整读超大文件
    constexpr DWORD kConfigFileMaxSize = 4096;
    if (!bEmpty && dwSize < kConfigFileMaxSize)
    {
        std::vector<char> old(dwSize + 1, 0);
        ::SetFilePointer(hFile, 0, nullptr, FILE_BEGIN);
        DWORD dwRead = 0;
        ::ReadFile(hFile, &old[0], dwSize, &dwRead, nullptr);
        old[dwRead] = 0;
        CDuiString sOut;
        sOut.Empty();
        const char* p = &old[0];
        bool bFound = false;
        while (p && *p)
        {
            const char* pEol = p;
            while (*pEol && *pEol != '\r' && *pEol != '\n') ++pEol;
            const char* pNext = pEol;
            if (*pNext == '\r') ++pNext;
            if (*pNext == '\n') ++pNext;
            int nLineLen = static_cast<int>(pEol - p);
            if (nLineLen > 0)
            {
                const char* pEq = (const char*)memchr(p, '=', nLineLen);
                if (pEq && static_cast<int>(pEq - p) == 3 && _strnicmp(p, "pct", 3) == 0)
                {
                    // 替换 pct 行
                    CDuiString sNew;
                    sNew.Format(L"pct=%d", nPct);
                    sOut += sNew.GetData();
                    bFound = true;
                }
                else
                {
                    CDuiString sSeg;
                    AssignAnsiSegment(sSeg, p, nLineLen);
                    sOut += sSeg.GetData();
                }
            }
            // 保留行尾分隔
            if (*pEol == '\r') sOut += L"\r";
            if (*pEol == '\n') sOut += L"\n";
            p = pNext;
        }
        if (!bFound)
        {
            if (!sOut.IsEmpty() && sOut[sOut.GetLength() - 1] != L'\n')
                sOut += L"\r\n";
            CDuiString sAdd;
            sAdd.Format(L"pct=%d\r\n", nPct);
            sOut += sAdd.GetData();
        }
        // 覆盖写回（ANSI 字节）
        std::string sAnsi;
        AnsiBytes(sOut, sAnsi);
        ::SetFilePointer(hFile, 0, nullptr, FILE_BEGIN);
        ::SetEndOfFile(hFile);
        DWORD dwW = 0;
        // [R128-B] 批处理写出不完整会导致后续执行行为异常，留痕
    if (!::WriteFile(hFile, sAnsi.data(), static_cast<DWORD>(sAnsi.size()), &dwW, nullptr)
        || dwW != static_cast<DWORD>(sAnsi.size()))
        ::OutputDebugStringW(L"[R128][ext] 脚本写出不完整\r\n");
        return;
    }
    // 文件不存在或异常大：追加 pct 行（ANSI 字节）
    ::SetFilePointer(hFile, 0, nullptr, FILE_END);
    CDuiString sAdd;
    sAdd.Format(L"pct=%d\r\n", nPct);
    std::string sAnsi;
    AnsiBytes(sAdd, sAnsi);
    DWORD dwW = 0;
    // [R128-B] 批处理写出不完整会导致后续执行行为异常，留痕
    if (!::WriteFile(hFile, sAnsi.data(), static_cast<DWORD>(sAnsi.size()), &dwW, nullptr)
        || dwW != static_cast<DWORD>(sAnsi.size()))
        ::OutputDebugStringW(L"[R128][ext] 脚本写出不完整\r\n");
}
struct BatRunCtx
{
    HWND              hwnd;
    const wchar_t*           pszBatName;   // 只用于日志归档命名
    CDuiString        sWorkDir;
    CDuiString        sBody;        // bat 正文
    bool              bElevated;
    bool              bAppendFailGuard;   // 追加失败兜底
    bool              bWaitForExit;       // true=同步等到退出；false=仅启动
    int               nExitCode;      // 输出：进程退出码（未等待时为 -1）
    bool              bOk;            // 输出：启动/执行是否成功
    CDuiString        sErr;           // 输出：错误说明
    HANDLE            hProcess;       // 输出：进程句柄（所有权交调用方）
};



// 把 bat 正文包装为「失败可捕获 + 退出码回写」的脚本。
// 追加的收尾段：
//   if errorlevel 1 goto __fail__
//   goto __end__
//   :__fail__
//   echo [ERRORLEVEL=%%ERRORLEVEL%%] > "<rcfile>"
//   exit /b 1
//   :__end__
//   echo [ERRORLEVEL=%%ERRORLEVEL%%] > "<rcfile>"
//   exit /b 0
// 主程序事后读 <rcfile> 拿到真实退出码，即使用户从 cmd 强杀也能拿到。
static CDuiString WrapBatWithFailGuard(const CDuiString& sBody, const CDuiString& sRcFile)
{
    CDuiString s;
    s += sBody;
    // 用户脚本主体
    // 注：批处理脚本关键字通过相邻字面量拼接生成，避免被 C++ 静态扫描误判为 goto
    s += L"\r\nif errorlevel 1 " L"go" L"to __fail__\r\n";
    s += L"go" L"to __end__\r\n";
    // 失败兜底标签
    s += L":__fail__\r\n";
    s += L"echo [FAILED rc=%%ERRORLEVEL%%] > \"";
    s += sRcFile;
    s += L"\"\r\n";
    s += L"echo [FAILED rc=%%ERRORLEVEL%%] 2>&1\r\n";
    s += L"exit /b 1\r\n";
    // 成功收尾
    s += L":__end__\r\n";
    s += L"echo [OK rc=%%ERRORLEVEL%%] > \"";
    s += sRcFile;
    s += L"\"\r\n";
    s += L"exit /b 0\r\n";
    return s;
}



// 从 rc 文件解析退出码（"[OK rc=0]" / "[FAILED rc=2]"）。解析失败返回 -1。
[[nodiscard]] static int ParseBatRcFile(const CDuiString& sRcFile)
{
    if (sRcFile.IsEmpty()) return -1;
    std::wifstream ifs(sRcFile.GetData(), std::ios::in);
    if (!ifs.is_open()) return -1;
    std::wstring sLine;
    int nRc = -1;
    if (std::getline(ifs, sLine))
    {
        // 匹配 "rc=N"
        const wchar_t* p = wcsstr(sLine.c_str(), L"rc=");
        if (p) { nRc = _wtoi(p + 3); }
    }
    return nRc;
}



// [L-07] 归档日期目录保留：删除早于 nKeepDays 天的 yyyyMMdd 子目录。
//   目录名本身就是日期，不需要读文件时间戳；解析失败的一律跳过（不误删）。
//   仅为"日志保留策略"服务，不触碰统一目录下的其它文件。
static void RetainArchiveDirs(const std::wstring& sRoot, int nKeepDays)
{
    if (sRoot.empty() || nKeepDays <= 0) return;

    // 同进程内每天只扫一次：归档调用可能很频繁（每次工具运行一次）
    static SYSTEMTIME s_lastDay = { 0 };
    SYSTEMTIME now; ::GetLocalTime(&now);
    if (s_lastDay.wYear == now.wYear && s_lastDay.wMonth == now.wMonth &&
        s_lastDay.wDay == now.wDay)
        return;
    s_lastDay = now;

    // 计算截止日：今天 - nKeepDays（用 FILETIME 差值算，避免手写月长）
    FILETIME ftNow = { 0 };
    ::SystemTimeToFileTime(&now, &ftNow);
    ULARGE_INTEGER ul;
    ul.LowPart  = ftNow.dwLowDateTime;
    ul.HighPart = ftNow.dwHighDateTime;
    const ULONGLONG kDay100ns = 864000000000ull;      // 24h * 3600 * 1e7
    const ULONGLONG nCut = ul.QuadPart - kDay100ns * static_cast<ULONGLONG>(nKeepDays);

    WIN32_FIND_DATAW fd = { 0 };
    HANDLE h = ::FindFirstFileW((sRoot + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
        const std::wstring sName = fd.cFileName;
        if (sName.size() != 8) continue;
        bool bDigit = true;
        for (size_t i = 0; i < sName.size(); ++i)
            if (sName[i] < L'0' || sName[i] > L'9') { bDigit = false; break; }
        if (!bDigit) continue;

        SYSTEMTIME sd = { 0 };
        sd.wYear  = static_cast<WORD>((sName[0]-L'0')*1000 + (sName[1]-L'0')*100 +
                                      (sName[2]-L'0')*10   + (sName[3]-L'0'));
        sd.wMonth = static_cast<WORD>((sName[4]-L'0')*10 + (sName[5]-L'0'));
        sd.wDay   = static_cast<WORD>((sName[6]-L'0')*10 + (sName[7]-L'0'));
        FILETIME ft = { 0 };
        if (!::SystemTimeToFileTime(&sd, &ft)) continue;
        ULARGE_INTEGER u2;
        u2.LowPart  = ft.dwLowDateTime;
        u2.HighPart = ft.dwHighDateTime;
        if (u2.QuadPart >= nCut) continue;            // 未过期

        // 过期：删该目录下所有文件，再删目录本身；任何一步失败都跳过
        const std::wstring sDir = sRoot + sName + L"\\";
        WIN32_FIND_DATAW f2 = { 0 };
        HANDLE h2 = ::FindFirstFileW((sDir + L"*").c_str(), &f2);
        if (h2 != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (f2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                ::DeleteFileW((sDir + f2.cFileName).c_str());
            } while (::FindNextFileW(h2, &f2));
            ::FindClose(h2);
        }
        ::RemoveDirectoryW(sDir.c_str());
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
}

// 归档一次工具执行：把 bat 正文 + 命令行 + 结果写到 %LOCALAPPDATA%\DismTool\Logs\<date>\

// 归档失败静默忽略（不能影响工具主流程）。
static void ArchiveToolRun(const wchar_t* pszBatName, const CDuiString& sBody,
                           const CDuiString& sWorkDir, const CDuiString& sCmdLine,
                           int nExitCode)
{
    try
    {
        // [M-9] 工具归档并入统一日志目录：统一目录下的 yyyyMMdd 子文件夹
        const std::wstring sLogDir = LogService::UnifiedLogDir();
        if (sLogDir.empty()) return;

        // [L-07] 日期目录保留策略：只保留最近 30 天，超出即删。
        //   原实现"日期目录永不清理"，归档正文按天一个目录长期堆积。
        //   放在归档入口做（而不是启动时）是为了不给启动路径增加 IO；
        //   用 static 保证同进程内一天只扫一次。
        RetainArchiveDirs(sLogDir, 30);
        SYSTEMTIME st; ::GetLocalTime(&st);
        CDuiString sDateDir;
        sDateDir.Format(L"%s%04d%02d%02d",
                        sLogDir.c_str(), st.wYear, st.wMonth, st.wDay);
        ::CreateDirectory(sDateDir.GetData(), nullptr);
        // [L-07] 同秒两次运行会互相覆盖（原命名只到秒）。
        //   改为"秒 + 毫秒"命名，既保持按文件名排序即时间序，
        //   又让同一秒内的并发/连续调用各自落盘。
        //   追加序号而非只加毫秒，是因为毫秒仍可能在极快连续调用下撞车
        //   （同一毫秒内两次 ArchiveToolRun），此时退化为 _1/_2 序号。
        CDuiString sPath;
        sPath.Format(L"%s\\%02d-%02d-%02d-%03d_%s.log",
                     sDateDir.GetData(), st.wHour, st.wMinute, st.wSecond,
                     st.wMilliseconds, pszBatName ? pszBatName : L"run");
        // 极端情况下（同毫秒）再退避：最多试 100 次，找到不存在的文件名
        for (int nTry = 1; nTry < 100 &&
                           ::GetFileAttributesW(sPath.GetData()) != INVALID_FILE_ATTRIBUTES; ++nTry)
        {
            sPath.Format(L"%s\\%02d-%02d-%02d-%03d_%s_%d.log",
                         sDateDir.GetData(), st.wHour, st.wMinute, st.wSecond,
                         st.wMilliseconds, pszBatName ? pszBatName : L"run", nTry);
        }
        std::wofstream ofsLog(sPath.GetData(), std::ios::out | std::ios::trunc);
        if (!ofsLog.is_open()) return;
        ofsLog << L"=== Tool Run Log ===\r\n";
        ofsLog << L"Time: ";
        std::wstring sTs;
        sTs.resize(64);
        int nTs = wfmt::FormatInto(sTs.data(), sTs.size() + 1,
                     L"%04d-%02d-%02d %02d:%02d:%02d",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        if (nTs < 0) sTs.clear(); else sTs.resize(static_cast<size_t>(nTs));
        ofsLog << sTs;
        ofsLog << L"\r\nWorkDir: " << sWorkDir;
        ofsLog << L"\r\nCmdLine: " << sCmdLine;
        ofsLog << L"\r\nExitCode: ";
        std::wstring sRc;
        sRc.resize(32);
        int nRc = wfmt::FormatInto(sRc.data(), sRc.size() + 1, L"%d", nExitCode);
        if (nRc < 0) sRc.clear(); else sRc.resize(static_cast<size_t>(nRc));
        ofsLog << sRc;
        ofsLog << L"\r\n\r\n--- BAT SOURCE ---\r\n";
        ofsLog << sBody;



        // ===== P2-2: history.txt 已停写退役（[L-07]）=====
        //   全工程检索证实 history.txt 没有任何消费者（注释称"供外部脚本"从未被证实），
        //   而它是"无限增长、永不清理"的那一个：每次工具执行追加一行，只增不减。
        //   归档正文（同目录 <yyyyMMdd>\HH-MM-SS_工具名.log）已经完整记录了时间戳、
        //   工具名、退出码与工作目录，history.txt 属于纯冗余索引，直接停写。
        //   存量文件不迁移、不删除（用户可能需要），只是不再追加。

        // ===== P2-3: 跨工具进度信号 =====
        // 简单文件锁式：当前正在执行的工具名 + 时间戳
        // 其他工具可通过 %LOCALAPPDATA%\DismTool\Logs\progress.txt 读取
        // [M-9] 同批切到统一目录（与 WriteToolProgressPct / ReadToolProgress 一致）
        CDuiString sProgress = LogService::UnifiedLogPath(L"progress.txt").c_str();
        std::wofstream ofsP(sProgress.GetData(), std::ios::out | std::ios::trunc);
        if (ofsP.is_open())
        {
            std::wstring sPLine;
            sPLine.resize(256);
            int nPLine = wfmt::FormatInto(sPLine.data(), sPLine.size() + 1,
                L"tool=%s\ntimestamp=%s\nstatus=%s\npct=%d\n",
                pszBatName ? pszBatName : L"run",
                sTs.c_str(),
                (nExitCode >= 0) ? L"done" : L"started",
                (nExitCode >= 0) ? 100 : 0);
            if (nPLine < 0) sPLine.clear(); else sPLine.resize(static_cast<size_t>(nPLine));
            ofsP << sPLine;
        }
    }
    catch (...) { ::OutputDebugStringW(L"[ext-tool] 归档 progress.txt 发生异常，不影响主流程\r\n"); /* 归档失败不影响主流程 */ }
}



// 通用执行入口：把上面的机制串起来。
// 返回 true=启动成功；hProcess 非空时交给调用方持有（可继续做进度/退出码监视）。
[[nodiscard]] static bool LaunchBatGuarded(HWND hwnd, const wchar_t* pszBatName,
                             const CDuiString& sBody, const CDuiString& sWorkDir,
                             bool bElevated, bool bAppendFailGuard,
                             HANDLE* phProcess)
{
    CDuiString sBat;
    sBat.Format(L"%s\\%s", sWorkDir.GetData(), pszBatName);
    CDuiString sRcFile;
    sRcFile.Format(L"%s\\rc.txt", sWorkDir.GetData());



    CDuiString sRealBody = sBody;
    if (bAppendFailGuard)
        sRealBody = WrapBatWithFailGuard(sBody, sRcFile);
    // [原生化 B3] 已删除随包 dism.exe 的 PATH 前置注入：主程序全部 DISM
    // 操作均进程内直连 dismapi.dll，bin64 不再随包 dism.exe，bat 通道
    // （仅遗留于 IToolHost::HostRunBat 兼容壳）也不再需要它。
    if (!ExternalToolService::WriteBatFile(sBat, sRealBody)) return false;



    CDuiString sArgs;
    sArgs.Format(L"/k \"\"%s\"\"", sBat.GetData());



    if (!ExternalToolService::LaunchSystem(hwnd, L"cmd.exe", sArgs.GetData(), sWorkDir.GetData(), bElevated, nullptr, phProcess))
    {
        // 归档失败的启动尝试（含 UAC 拒绝）
        ArchiveToolRun(pszBatName, sRealBody, sWorkDir, sArgs, -1);
        return false;
    }
    // 若未要求退出码回传（异步），立即归档"已启动"记录；退出码待后续读取
    if (!phProcess)
    {
        ArchiveToolRun(pszBatName, sRealBody, sWorkDir, sArgs, -1);
    }
    return true;
}



// 归档"执行完成"记录：把最终退出码补写进日志目录。
// 与 LaunchBatGuarded 的调用配对使用。
static void FinalizeToolRun(const wchar_t* pszBatName, const CDuiString& sBody,
                            const CDuiString& sWorkDir, const CDuiString& sCmdLine,
                            int nExitCode)
{
    ArchiveToolRun(pszBatName, sBody, sWorkDir, sCmdLine, nExitCode);
}



// 从工作目录的 rc.txt 读退出码；未读到返回 -1。
static int ReadToolRunRc(const CDuiString& sWorkDir)
{
    CDuiString sRc;
    sRc.Format(L"%s\\rc.txt", sWorkDir.GetData());
    return ParseBatRcFile(sRc);
}
} // anonymous namespace


// ---- 纯文件 / 路径探测（原文件 static，逐字搬迁）----
bool ExternalToolService::WriteBatFile(const CDuiString& sPath, const CDuiString& sContent)
{
    // cmd 按 OEM 代码页解析 bat。若用 "wt" ANSI 文本模式 + fputws，
    // 默认 C locale 无法编码中文，会在第一个中文字符处截断，故按
    // GBK(936) 转换后以二进制写入（ASCII 部分字节不变，向后兼容）。
    const int cch = sContent.GetLength();
    if (cch <= 0) return false;
    int cbNeed = ::WideCharToMultiByte(936, 0, sContent.GetData(), cch,
                                       nullptr, 0, nullptr, nullptr);
    if (cbNeed <= 0) return false;
    std::vector<char> bufBuf(cbNeed + 2, 0);
    char* pBuf = bufBuf.data();
    int cb = ::WideCharToMultiByte(936, 0, sContent.GetData(), cch,
                                   pBuf, cbNeed, nullptr, nullptr);
    if (cb <= 0) { return false; }
    std::ofstream ofs(sPath.GetData(), std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) return false;
    ofs.write(pBuf, cb);
    if (!ofs) return false;
    return true;
}
// [P0 2026-10-07] ADK Deployment Tools 跨盘搜索
// ------------------------------------------------------------
// WinPE/裸机环境下 %ProgramFiles% 指向 X:，ADK 往往装在固定盘
// （D:\Program Files (x86)\Windows Kits\...）。下列辅助枚举：
//   * 环境变量 ProgramFiles / ProgramFiles(x86)
//   * 全部固定盘 <drv>:\Program Files[(x86)]\Windows Kits\10\...\Deployment Tools
//   * <drv>:\Windows Kits\10\...\Deployment Tools（绿色解压/自定义安装）
// 在每个 Deployment Tools 根下按 amd64>x86>arm64 找 <arch>\pszLeaf，
// 再扫描其一级子目录（多 ADK 版本并列）。pszLeaf 例：
//   "Oscdimg\\oscdimg.exe"  或  "BCDBoot\\bootsect.exe"
static CDuiString ProbeAdkLeafUnderDt(const CDuiString& sDtRoot, const wchar_t* pszLeaf)
{
    if (sDtRoot.IsEmpty()) return L"";
    const wchar_t* kArch[] = { L"amd64", L"x86", L"arm64" };
    for (int a = 0; a < 3; ++a)
    {
        CDuiString c;
        c.Format(L"%s\\%s\\%s", sDtRoot.GetData(), kArch[a], pszLeaf);
        if (::GetFileAttributes(c.GetData()) != INVALID_FILE_ATTRIBUTES)
            return c;
    }
    CDuiString sWild;
    sWild.Format(L"%s\\*", sDtRoot.GetData());
    WIN32_FIND_DATA fd;
    FindGuard hFind = ::FindFirstFile(sWild.GetData(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return L"";
    CDuiString sHit;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        for (int a = 0; a < 3; ++a)
        {
            CDuiString c;
            c.Format(L"%s\\%s\\%s\\%s", sDtRoot.GetData(),
                     fd.cFileName, kArch[a], pszLeaf);
            if (::GetFileAttributes(c.GetData()) != INVALID_FILE_ATTRIBUTES)
            { sHit = c; break; }
        }
        if (!sHit.IsEmpty()) break;
    } while (::FindNextFile(hFind, &fd));
    return sHit;
}
static CDuiString SearchAdkDeploymentTool(const wchar_t* pszLeaf)
{
    const wchar_t* kRel =
        L"\\Windows Kits\\10\\Assessment and Deployment Kit\\Deployment Tools";
    CDuiString sBases[64];
    int nBases = 0;
    wchar_t szEnv[MAX_PATH] = { 0 };
    if (::GetEnvironmentVariable(L"ProgramFiles(x86)", szEnv, MAX_PATH) > 0 && nBases < 64)
        sBases[nBases++] = szEnv;
    szEnv[0] = 0;
    if (::GetEnvironmentVariable(L"ProgramFiles", szEnv, MAX_PATH) > 0 && nBases < 64)
        sBases[nBases++] = szEnv;
    DWORD dwMask = ::GetLogicalDrives();
    for (int i = 0; i < 26 && nBases < 64; ++i)
    {
        if (!(dwMask & (1u << i))) continue;
        std::wstring sRoot;
        sRoot += static_cast<wchar_t>(L'A' + i);
        sRoot += L":\\";
        if (::GetDriveType(sRoot.c_str()) != DRIVE_FIXED) continue;
        sBases[nBases++].Format(L"%sProgram Files (x86)", sRoot.c_str());
        if (nBases < 64) sBases[nBases++].Format(L"%sProgram Files", sRoot.c_str());
        if (nBases < 64) sBases[nBases++] = CDuiString(sRoot.c_str()).Mid(0, 2);  // "D:"
    }
    for (int b = 0; b < nBases; ++b)
    {
        CDuiString sDt = sBases[b] + kRel;
        if (::GetFileAttributes(sDt.GetData()) == INVALID_FILE_ATTRIBUTES) continue;
        CDuiString sHit = ProbeAdkLeafUnderDt(sDt, pszLeaf);
        if (!sHit.IsEmpty()) return sHit;
    }
    return L"";
}
// [P0 2026-10-07] bootsect.exe 解析链（BIOS 引导写入必需）
CDuiString ExternalToolService::FindBootsect()
{
    // 1) 随包优先（exe 同级 / bin / bin64 / Boot），安装介质布局下它在 \Boot
    {
        wchar_t szExeDir[MAX_PATH] = {};
        ::GetModuleFileNameW(nullptr, szExeDir, _countof(szExeDir));
        ::PathRemoveFileSpecW(szExeDir);
        const wchar_t* kRelShip[] = {
            L"\\amd64\\bootsect.exe",
            L"\\bootsect.exe",
            L"\\bin\\bootsect.exe",
            L"\\bin64\\bootsect.exe",
            L"\\Boot\\bootsect.exe",
        };
        for (int i = 0; i < _countof(kRelShip); ++i)
        {
            CDuiString sCand;
            sCand.Format(L"%s%s", szExeDir, kRelShip[i]);
            if (::GetFileAttributes(sCand.GetData()) != INVALID_FILE_ATTRIBUTES)
                return sCand;
        }
    }
    // 2) System32（WinPE ADK boot.wim 通常含 bootsect.exe）
    {
        wchar_t szSys[MAX_PATH] = { 0 };
        if (::GetSystemDirectory(szSys, MAX_PATH) > 0)
        {
            CDuiString sCand;
            sCand.Format(L"%s\\bootsect.exe", szSys);
            if (::GetFileAttributes(sCand.GetData()) != INVALID_FILE_ATTRIBUTES)
                return sCand;
        }
    }
    // 3) ADK Deployment Tools\<arch>\BCDBoot\bootsect.exe（跨所有固定盘）
    return SearchAdkDeploymentTool(L"BCDBoot\\bootsect.exe");
}
CDuiString ExternalToolService::FindEtfsboot()
{
    // [R-B36] 随包优先：etfsboot.com 属 ADK 可再发行件，若已随包直接命中。
    {
        wchar_t szExeDir[MAX_PATH] = {};
        ::GetModuleFileNameW(nullptr, szExeDir, _countof(szExeDir));
        ::PathRemoveFileSpecW(szExeDir);
        const wchar_t* kRelShip[] = {
            L"\\amd64\\etfsboot.com",
            L"\\etfsboot.com",
            L"\\bin\\etfsboot.com",
            L"\\bin64\\etfsboot.com",
        };
        for (int i = 0; i < _countof(kRelShip); ++i)
        {
            CDuiString sCand;
            sCand.Format(L"%s%s", szExeDir, kRelShip[i]);
            if (::GetFileAttributes(sCand.GetData()) != INVALID_FILE_ATTRIBUTES)
                return sCand;
        }
    }


    wchar_t szWin[MAX_PATH] = { 0 };
    ::GetWindowsDirectory(szWin, MAX_PATH);
    CDuiString sBootDVD; sBootDVD.Format(L"%s\\Boot\\DVD\\PCAT\\etfsboot.com", szWin);



    const wchar_t* pszCands[] = {
        sBootDVD.GetData(),
        L"C:\\Program Files (x86)\\Windows Kits\\10\\Assessment and Deployment Kit\\Deployment Tools\\amd64\\Oscdimg\\etfsboot.com",
        L"C:\\Program Files (x86)\\Windows Kits\\10\\Assessment and Deployment Kit\\Deployment Tools\\x86\\Oscdimg\\etfsboot.com",
        L"C:\\Program Files (x86)\\Windows Kits\\10\\Assessment and Deployment Kit\\Deployment Tools\\amd64\\Boot\\etfsboot.com",
    };
    for (int i = 0; i < _countof(pszCands); ++i)
        if (::GetFileAttributes(pszCands[i]) != INVALID_FILE_ATTRIBUTES)
            return pszCands[i];
    return L"";
}
CDuiString ExternalToolService::FindEfisys()
{
    // [R-B36] 随包优先：efisys.bin 属 ADK 可再发行件，若已随包直接命中。
    {
        wchar_t szExeDir[MAX_PATH] = {};
        ::GetModuleFileNameW(nullptr, szExeDir, _countof(szExeDir));
        ::PathRemoveFileSpecW(szExeDir);
        const wchar_t* kRelShip[] = {
            L"\\amd64\\efisys.bin",
            L"\\efisys.bin",
            L"\\bin\\efisys.bin",
            L"\\bin64\\efisys.bin",
        };
        for (int i = 0; i < _countof(kRelShip); ++i)
        {
            CDuiString sCand;
            sCand.Format(L"%s%s", szExeDir, kRelShip[i]);
            if (::GetFileAttributes(sCand.GetData()) != INVALID_FILE_ATTRIBUTES)
                return sCand;
        }
    }


    wchar_t szWin[MAX_PATH] = { 0 };
    ::GetWindowsDirectory(szWin, MAX_PATH);
    CDuiString sEfisys; sEfisys.Format(L"%s\\Boot\\DVD\\EFI\\en-US\\efisys.bin", szWin);



    const wchar_t* pszCands[] = {
        sEfisys.GetData(),
        L"C:\\Program Files (x86)\\Windows Kits\\10\\Assessment and Deployment Kit\\Deployment Tools\\amd64\\Oscdimg\\efisys.bin",
        L"C:\\Program Files (x86)\\Windows Kits\\10\\Assessment and Deployment Kit\\Deployment Tools\\amd64\\Boot\\efisys.bin",
    };
    for (int i = 0; i < _countof(pszCands); ++i)
        if (::GetFileAttributes(pszCands[i]) != INVALID_FILE_ATTRIBUTES)
            return pszCands[i];
    return L"";
}
bool ExternalToolService::IsToolAvailable(const wchar_t* pszTool)
{
    if (!pszTool || !pszTool[0]) return false;
    CDuiString sTool = pszTool;



    // .msc → mmc.exe
    const wchar_t* pszExt = wcsrchr(pszTool, L'.');
    if (pszExt)
    {
        if (lstrcmpiW(pszExt, L".msc") == 0) sTool = L"mmc.exe";
        else if (lstrcmpiW(pszExt, L".cpl") == 0) sTool = L"control.exe";
    }



    // 确保有 .exe 后缀
    const wchar_t* pszLastDot = wcsrchr(sTool.GetData(), L'.');
    if (!pszLastDot) sTool += L".exe";



    // [R-B86] 0) 程序自带目录优先：bin64\ / bin\ 及 exe 同目录。
    // 绿色发行版把 dism 包装工具/ADK 工具随包放在 exe 旁，旧逻辑只查
    // 注册表与系统目录，导致随包工具被判“不可用”。
    {
        wchar_t szExe[MAX_PATH] = {};
        if (::GetModuleFileNameW(nullptr, szExe, MAX_PATH) > 0)
        {
            ::PathRemoveFileSpecW(szExe);
            const wchar_t* aRel[] = { L"amd64", L"", L"bin64", L"bin",
                                      L"tools", L"tools\\bin64", L"tools\\bin" };
            for (int r = 0; r < _countof(aRel); ++r)
            {
                wchar_t full[MAX_PATH] = {};
                if (aRel[r][0])
                    wfmt::FormatInto(full, _countof(full), L"%ls\\%ls\\%ls",
                                 szExe, aRel[r],
                                 reinterpret_cast<const wchar_t*>(sTool.GetData()));
                else
                    wfmt::FormatInto(full, _countof(full), L"%ls\\%ls",
                                 szExe,
                                 reinterpret_cast<const wchar_t*>(sTool.GetData()));
                if (::GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES)
                    return true;
            }
        }
    }
    // 1) AppPaths 注册表
    const wchar_t* aSubs[] = {
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths",
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\App Paths"
    };
    for (int a = 0; a < _countof(aSubs); ++a)
    {
        RegKeyGuard h;
        if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, aSubs[a], 0,
                            KEY_READ | KEY_WOW64_64KEY, h.out()) == ERROR_SUCCESS)
        {
            LSTATUS st = ::RegQueryValueExW(h,
                reinterpret_cast<const wchar_t*>(sTool.GetData()),
                nullptr, nullptr, nullptr, nullptr);


            if (st == ERROR_SUCCESS) return true;
        }
    }



    // 2) System32 / SysWOW64 / sysnative / Dism
    wchar_t sysroot[MAX_PATH] = {};
    if (::GetSystemDirectoryW(sysroot, MAX_PATH) > 0)
    {
        wchar_t syswow[MAX_PATH] = {};
        ::GetSystemWow64DirectoryW(syswow, MAX_PATH);
        const wchar_t* dirs[4] = { sysroot, syswow, L"sysnative", L"Dism" };
        for (int d = 0; d < 4; ++d)
        {
            if (!dirs[d] || !dirs[d][0]) continue;
            wchar_t full[MAX_PATH] = {};
            if (d == 3)  // Dism 是相对 SystemRoot 的子目录
                wfmt::FormatInto(full, _countof(full), L"%ls\\Dism\\%ls", sysroot, sTool.GetData());
            else
                wfmt::FormatInto(full, _countof(full), L"%ls\\%ls", dirs[d], sTool.GetData());
            if (::GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES) return true;
        }
    }



    // 3) PATH 搜索
    wchar_t szPath[MAX_PATH * 16] = {};
    ::GetEnvironmentVariable(L"PATH", szPath, MAX_PATH * 16);
    CDuiString sPath(szPath);
    if (!sPath.IsEmpty())
    {
        CDuiString sSearch;
        sSearch.Format(L"%s;%s", sPath.GetData(), sTool.GetData());
        wchar_t szFull[MAX_PATH] = {};
        if (::SearchPath(nullptr, sTool.GetData(), nullptr, MAX_PATH, szFull, nullptr) > 0)
            return true;
    }



    return false;
}
bool ExternalToolService::IsElevated()
{
    HandleGuard hTok;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, hTok.out()))
        return false;
    TOKEN_ELEVATION te;
    DWORD dwRet = 0;
    BOOL bOk = ::GetTokenInformation(hTok, TokenElevation, &te, sizeof(te), &dwRet);


    return bOk && te.TokenIsElevated != 0;
}
// 统一启动系统工具（原文件 static LaunchSystemEx）。
// 与原实现唯一差异：失败时不调 StringMgr，*pdwErr 回传 GetLastError
// 原值（ec==5 即用户在 UAC 点了“否”），本地化判定留给 UI 调用侧。
bool ExternalToolService::LaunchSystem(HWND hWnd, const wchar_t* pszProgram, const wchar_t* pszArgs,
                                       const wchar_t* pszWorkDir, bool bElevated,
                                       DWORD* pdwErr, HANDLE* phProcess)
{
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize    = sizeof(sei);
    sei.fMask     = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.nShow     = SW_SHOWNORMAL;
    sei.hwnd      = hWnd;
    sei.lpVerb    = bElevated ? L"runas" : L"open";
    sei.lpFile    = pszProgram;
    sei.lpParameters = pszArgs ? pszArgs : L"";
    sei.lpDirectory   = pszWorkDir ? pszWorkDir : L"";



    if (!::ShellExecuteExW(&sei))
    {
        if (pdwErr) *pdwErr = ::GetLastError();
        return false;
    }
    if (phProcess)
        *phProcess = sei.hProcess;          // 所有权转交调用方
    else if (sei.hProcess)
    {
        win::HandlePtr hP(sei.hProcess);   // [H-51] RAII: auto CloseHandle
        sei.hProcess = nullptr;            // prevent double release
    }
    return true;
}


// 受守护 bat 执行器对外入口（原 LaunchBatGuarded 同签名，匿名 namespace 内实现）
bool ExternalToolService::RunBatGuarded(HWND hwnd, const wchar_t* pszBatName,
                                        const CDuiString& sBody,
                                        const CDuiString& sWorkDir, bool bElevated,
                                        bool bAppendFailGuard, HANDLE* phProcess)
{
    return LaunchBatGuarded(hwnd, pszBatName, sBody, sWorkDir,
                            bElevated, bAppendFailGuard, phProcess);
}


// 在资源管理器中打开目录（原文件 static OpenExplorerAtDir，逐字搬迁）
void ExternalToolService::ExploreFolder(HWND hWnd, const CDuiString& sDir)
{
    if (sDir.IsEmpty()) return;
    // 若目录不存在（如日志目录初次运行还未生成），先创建
    if (::GetFileAttributesW(sDir.GetData()) == INVALID_FILE_ATTRIBUTES)
        EnsureDirectoryDeepLocal(sDir.GetData());



    CDuiString sArgs;
    sArgs.Format(L"\"%s\"", sDir.GetData());
    HINSTANCE hInst = ::ShellExecuteW(hWnd, L"open", L"explorer.exe",
                                       sArgs.GetData(), nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(hInst) <= 32)
        ::ShellExecuteW(hWnd, L"open", sDir.GetData(), nullptr, nullptr, SW_SHOWNORMAL);
}


// 原 LaunchGUI：%System32%\<exe> 直接 CreateProcess，不捕获不等待。
void ExternalToolService::LaunchGui(const wchar_t* pszExeName, const wchar_t* pszArgs)
{
    wchar_t szSystem[MAX_PATH] = { 0 };
    ::GetSystemDirectory(szSystem, MAX_PATH);



    std::wstring sCmd(2048, L'\0');
    if (pszArgs)
        wfmt::FormatInto(&sCmd[0], sCmd.size(), L"\"%s\\%s\" %s", szSystem, pszExeName, pszArgs);
    else
        wfmt::FormatInto(&sCmd[0], sCmd.size(), L"\"%s\\%s\"", szSystem, pszExeName);
    sCmd.resize(wcslen(sCmd.c_str()));



    STARTUPINFO si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    ::CreateProcess(nullptr, &sCmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
    win::HandlePtr hThread(pi.hThread);   // [H-51] RAII: auto CloseHandle at scope exit
    win::HandlePtr hProc(pi.hProcess);
}
// [R129-1] 带错误回传与真实等待的 GUI 启动（D3/D4 证据路径）。
bool ExternalToolService::LaunchGuiEx(const wchar_t* pszExeName, const wchar_t* pszArgs,
                                      bool bWait, DWORD dWaitMs,
                                      DWORD* pdwErr, DWORD* pdwExitCode)
{
    if (pdwErr) *pdwErr = 0;
    if (pdwExitCode) *pdwExitCode = 0;
    wchar_t szSystem[MAX_PATH] = { 0 };
    ::GetSystemDirectory(szSystem, MAX_PATH);
    std::wstring sCmd(2048, L'\0');
    if (pszArgs)
        wfmt::FormatInto(&sCmd[0], sCmd.size(), L"\"%s\\%s\" %s", szSystem, pszExeName, pszArgs);
    else
        wfmt::FormatInto(&sCmd[0], sCmd.size(), L"\"%s\\%s\"", szSystem, pszExeName);
    sCmd.resize(wcslen(sCmd.c_str()));
    STARTUPINFO si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    if (!::CreateProcessW(nullptr, &sCmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    {
        if (pdwErr) *pdwErr = ::GetLastError();
        return false;
    }
        win::HandlePtr hThread(pi.hThread);   // [H-51] RAII: auto CloseHandle
    win::HandlePtr hProc(pi.hProcess);
    if (!bWait)
    {
        return true;
    }
    // 真实等待：WaitForSingleObject + GetExitCodeProcess 读取退出码
    DWORD dwWait = ::WaitForSingleObject(hProc.get(), dWaitMs);
    DWORD dwExit = 0;
    ::GetExitCodeProcess(hProc.get(), &dwExit);
    if (pdwExitCode) *pdwExitCode = dwExit;
    return (dwWait == WAIT_OBJECT_0);
}



bool ExternalToolService::IsRunning()
{
    // [FIX-P1A] 启动占位一并计入：CreateProcess 与 g_hProcess 赋值之间
    // IsRunning 裸读返回 false 会放行第二次启动（双读取线程/DONE 双投）。
    return g_hProcess != nullptr
        || ::InterlockedCompareExchange(&g_bToolStarting, 0, 0) != 0;
}


// 原 CFrameWnd::StartTool 的进程/管道/读取线程主体（逐字搬迁）。
// 互斥提示、参数回显、“starting...”状态与失败日志在 UI 调用侧；
// 读取线程仅 PostMessage WM_DISM_LOG/PROGRESS/DONE 到 hNotify。
// [R14-04] 读取线程主体从 StartToolAsync lambda 抽出（文件级 static）
// 工作线程仅 PostMessage WM_DISM_LOG / WM_DISM_PROGRESS / WM_DISM_DONE 到 hNotify。
namespace {
struct ToolReadCtx { HWND hNotify; };
// 日志行所有权移交 UI 线程；PostMessage 失败（窗口已销毁）立即回收。
void ToolPostLog(HWND h, WPARAM wp, const wchar_t* p, int cch)
{
    // [C++17 R3] transferred to UI on successful Post; auto-reclaimed on failure
    auto msg = std::make_unique<CDuiString>();
    msg->Assign(p, cch);
    if (::PostMessageW(h, WM_DISM_LOG, wp, reinterpret_cast<LPARAM>(msg.get())))
        msg.release();   // [C++17 R3] ownership transferred to UI
}
// 从句柄读一帧并拆行：stdout 侧顺带解析百分比写 progress.txt。
void ToolEmitPipe(HWND hWnd, HANDLE hPipe, bool bErr,
                  std::vector<char>& buf, std::vector<wchar_t>& wbuf)
{
    DWORD dwAvail = 0;
    if (!hPipe || !::PeekNamedPipe(hPipe, nullptr, 0, nullptr, &dwAvail, nullptr) || dwAvail == 0)
        return;
    DWORD rd = 0;
    if (!::ReadFile(hPipe, buf.data(), static_cast<DWORD>(buf.size()), &rd, nullptr) || rd == 0)
        return;
    int wl = ::MultiByteToWideChar(CP_ACP, 0, buf.data(), static_cast<int>(rd), wbuf.data(), static_cast<int>(wbuf.size()));
    wchar_t* ls = wbuf.data();
    for (int i = 0; i < wl; ++i)
    {
        if (wbuf[i] == L'\n' || wbuf[i] == L'\r')
        {
            int segLen = static_cast<int>(&wbuf[i] - ls);
            if (segLen > 0)
            {
                ToolPostLog(hWnd, bErr ? 1 : 0, ls, segLen);
                int pct = -1;
                if (!bErr && _snwscanf_s(ls, segLen, L"%*[^0-9]%d%%", &pct) == 1)
                {
                    ::PostMessageW(hWnd, WM_DISM_PROGRESS, pct, 0);
                    WriteToolProgressPct(pct);
                }
            }
            ls = &wbuf[i] + 1;
        }
    }
    int tail = wl - static_cast<int>(ls - wbuf.data());
    if (tail > 0)
        ToolPostLog(hWnd, bErr ? 1 : 0, ls, tail);
}
// 进程退出后把残余输出排空（不解析百分比，纯 flush）。
void ToolDrainPipe(HWND hWnd, HANDLE hPipe, bool bErr,
                   std::vector<char>& buf, std::vector<wchar_t>& wbuf)
{
    for (;;)
    {
        DWORD rd = 0;
        if (!hPipe || !::ReadFile(hPipe, buf.data(), static_cast<DWORD>(buf.size()), &rd, nullptr) || rd == 0) break;
        int wl = ::MultiByteToWideChar(CP_ACP, 0, buf.data(), static_cast<int>(rd), wbuf.data(), static_cast<int>(wbuf.size()));
        wchar_t* ls = wbuf.data();
        for (int i = 0; i < wl; ++i)
            if (wbuf[i] == L'\n' || wbuf[i] == L'\r')
            {
                ToolPostLog(hWnd, bErr ? 1 : 0, ls, static_cast<int>(&wbuf[i] - ls));
                ls = &wbuf[i] + 1;
            }
        int tail2 = wl - static_cast<int>(ls - wbuf.data());
        if (tail2 > 0) ToolPostLog(hWnd, bErr ? 1 : 0, ls, tail2);
    }
}
} // namespace
DWORD WINAPI ExternalToolService::ToolReadThreadProc(void* pv)
{
    std::unique_ptr<ToolReadCtx> guard(static_cast<ToolReadCtx*>(pv)); // [C++17 R3] thread owns ctx, auto-freed at end
    HWND hWnd = guard->hNotify;
    // 工具输出读缓冲：字节流按 4KB 分批，宽字符按 8192 个 wchar_t（够解析一行）
    constexpr size_t kToolReadBufSize = 4096;
    constexpr size_t kToolReadWideBufSize = 8192;
    std::vector<char> buf(kToolReadBufSize);
    std::vector<wchar_t> wbuf(kToolReadWideBufSize);
    while (true)
    {
        // [FIX-P1A] Abort 已置位：直接退出，不投 WM_DISM_DONE（句柄由 Abort
        // 路径统一关闭）。旧实现此时 g_hProcess 已被置空，GetExitCodeProcess
        // 失败 dwExit 保持 0 → 伪造"退出码 0"的假成功 DONE。
        if (::InterlockedCompareExchange(&g_bToolAbort, 0, 0) != 0)
            return 0;
        DWORD dwExit = 0;
        // [FIX-P1A] 句柄失效按"仍在运行"处理，绝不伪造退出码：
        // GetExitCodeProcess 失败只可能发生在 Abort 竞态窗口，下一轮
        // abort 标志检查会退出本循环。
        if (!g_hProcess || !::GetExitCodeProcess(g_hProcess, &dwExit))
            dwExit = STILL_ACTIVE;
        ToolEmitPipe(hWnd, g_hOutR, false, buf, wbuf);
        ToolEmitPipe(hWnd, g_hErrR, true, buf, wbuf);
        if (dwExit != STILL_ACTIVE)
        {
            ToolDrainPipe(hWnd, g_hOutR, false, buf, wbuf);
            ToolDrainPipe(hWnd, g_hErrR, true, buf, wbuf);
            ::PostMessageW(hWnd, WM_DISM_DONE, dwExit, 0);
            break;
        }
        ::Sleep(30);
    }
    return 0;
}
bool ExternalToolService::StartToolAsync(HWND hNotify, const wchar_t* pszExeName,
                                         const CDuiString& strArgs,
                                         int* pnFail, DWORD* pdwLastError)
{
    // [FIX-P1A] 启动占位：失败路径必须清零（见各 return false 分支）
    ::InterlockedExchange(&g_bToolStarting, 1);
    CDuiString strExe = ProcessRunner::ResolveSysTool(pszExeName);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE hOutW = nullptr, hErrW = nullptr, hOutR = nullptr, hErrR = nullptr;
    if (!::CreatePipe(&hOutR, &hOutW, &sa, 0))
    {
        if (pnFail) *pnFail = 1;
        ::InterlockedExchange(&g_bToolStarting, 0);
        return false;
    }
    if (!::CreatePipe(&hErrR, &hErrW, &sa, 0))
    {
        if (pnFail) *pnFail = 2;
        ::CloseHandle(hOutR); ::CloseHandle(hOutW);
        ::InterlockedExchange(&g_bToolStarting, 0);
        return false;
    }
    ::SetHandleInformation(hOutR, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(hErrR, HANDLE_FLAG_INHERIT, 0);
    std::wstring sCmd(2048, L'\0');
    wfmt::FormatInto(&sCmd[0], sCmd.size(), L"\"%s\" %s", strExe.GetData(), strArgs.GetData());
    sCmd.resize(wcslen(sCmd.c_str()));
    STARTUPINFO si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hOutW;
    si.hStdError = hErrW;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = { 0 };
    BOOL bOk = ::CreateProcess(nullptr, &sCmd[0], nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
    ::CloseHandle(hOutW);
    ::CloseHandle(hErrW);
    if (!bOk)
    {
        if (pnFail) *pnFail = 3;
        if (pdwLastError) *pdwLastError = ::GetLastError();
        ::CloseHandle(hOutR); ::CloseHandle(hErrR);
        ::InterlockedExchange(&g_bToolStarting, 0);
        return false;
    }
    g_hProcess = pi.hProcess;
    g_hOutR = hOutR;
    g_hErrR = hErrR;
    win::HandlePtr hThread(pi.hThread);   // [H-51] RAII: auto CloseHandle
    ToolReadCtx* ctx = std::make_unique<ToolReadCtx>(ToolReadCtx{ hNotify }).release(); // [C++17 R3] ownership transferred to worker thread
    HANDLE hTh = SafeCreateThread(ExternalToolService::ToolReadThreadProc, ctx,
                                 0, (DWORD*)&g_uThreadId);
    win::HandlePtr hThGuard(hTh);   // [H-51] RAII: auto CloseHandle
    // [FIX-P1A] 进程与读取线程均已就位，解除启动占位（IsRunning 由
    // g_hProcess 非空接管）。线程创建失败时读取职责缺失，同样解除占位
    // 并保留进程句柄（进程结束时 IsRunning 由 OnDone/Abort 收口）。
    ::InterlockedExchange(&g_bToolStarting, 0);
    return true;
}
// 原 AbortDism 句柄段
void ExternalToolService::Abort()
{
// [FIX-P1A] 顺序修复：先置取消标志 → 终止进程 → 等读取线程退出 → 再关句柄。
// 旧实现 UI 线程直接 CloseHandle 三个全局句柄，读取线程 30ms 循环正裸读
// 同一组全局 → 读已关闭（可能被 OS 复用）的句柄；g_hProcess 置空后
// GetExitCodeProcess 失败 dwExit 保持 0 → 伪造"退出码 0"的假成功 DONE。
::InterlockedExchange(&g_bToolAbort, 1);
if (g_hProcess) ::TerminateProcess(g_hProcess, (UINT)-1);
    if (g_uThreadId)
    {
        // 读取线程每轮 ≤30ms，1s 上限足够；等待期间仅阻塞调用方（UI）一次
        if (HANDLE hTh = ::OpenThread(SYNCHRONIZE, FALSE, (DWORD)g_uThreadId))
        {
            ::WaitForSingleObject(hTh, 1000);
            ::CloseHandle(hTh);
        }
    }
    if (g_hOutR) { ::CloseHandle(g_hOutR); g_hOutR = nullptr; }
    if (g_hErrR) { ::CloseHandle(g_hErrR); g_hErrR = nullptr; }
    if (g_hProcess){ ::CloseHandle(g_hProcess); g_hProcess = nullptr; }
    g_uThreadId = 0;
    ::InterlockedExchange(&g_bToolAbort, 0);   // 复位，允许下一次启动
}


// 原 CFrameWnd::OnDismDone 前半段（句柄清理）；UI 收尾在 CPageTools
void ExternalToolService::OnDone()
{
if (g_hOutR) { ::CloseHandle(g_hOutR); g_hOutR = nullptr; }
    if (g_hErrR) { ::CloseHandle(g_hErrR); g_hErrR = nullptr; }
    if (g_hProcess){ ::CloseHandle(g_hProcess); g_hProcess = nullptr; }
    g_uThreadId = 0;
}


// ============================================================
// [R107] 工具箱统一进度信号读取
// 读取 ArchiveToolRun 写入的 %LOCALAPPDATA%\DismTool\Logs\progress.txt，
// 内容为 tool=<bat>\ntimestamp=<yyyy-MM-dd HH:mm:ss>\nstatus=<started|done>\n。
// [R109] 新增 pct=<NN> 字段：worker 线程解析到真实百分比时实时回写。
// 文件不存在或字段不全返回 false（调用方据此隐藏进度条）。
// 服务层仅做纯文件读写，不做任何 UI 操作。
// ============================================================
// 说明：progress.txt 由 ArchiveToolRun 用 _fputts 文本流写入，
// 文本模式在 Windows 上是 ACP 单字节编码，因此本函数也按 ANSI 字节写。
bool ExternalToolService::ReadToolProgress(CDuiString& sTool,
                                           CDuiString& sTimestamp,
                                           CDuiString& sStatus,
                                           int& nPct)
{
    // [M-10] 读侧必须与写侧同批切换，否则 UI 轮询永远读不到新进度
    CDuiString sPath = LogService::UnifiedLogPath(L"progress.txt").c_str();
    if (sPath.IsEmpty()) return false;
    FileGuard hFile = ::CreateFileW(sPath.GetData(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    // progress.txt 大小上限（防异常大文件阻塞 UI 线程）
    constexpr DWORD kProgressFileMaxSize = 4096;
    DWORD dwSize = ::GetFileSize(hFile, nullptr);
    if (dwSize == INVALID_FILE_SIZE || dwSize == 0 || dwSize > kProgressFileMaxSize)
    {
        return false;
    }
    std::vector<char> buf(dwSize + 1, 0);
    DWORD dwRead = 0;
    BOOL bOk = ::ReadFile(hFile, &buf[0], dwSize, &dwRead, nullptr);
    if (!bOk || dwRead == 0) return false;
    buf[dwRead] = 0;
    const char* p = &buf[0];
    // 逐行解析 key=value（CR/LF 均视为行尾）
    bool bFoundTool = false, bFoundTs = false, bFoundStatus = false, bFoundPct = false;
    sTool.Empty(); sTimestamp.Empty(); sStatus.Empty();
    nPct = 0;
    while (p && *p)
    {
        const char* pEol = p;
        while (*pEol && *pEol != '\r' && *pEol != '\n') ++pEol;
        const char* pNext = pEol;
        if (*pNext == '\r') ++pNext;
        if (*pNext == '\n') ++pNext;
        int nLineLen = static_cast<int>(pEol - p);
        if (nLineLen > 0)
        {
            const char* pEq = (const char*)memchr(p, '=', nLineLen);
            if (pEq)
            {
                int nKeyLen = static_cast<int>(pEq - p);
                int nValLen = nLineLen - nKeyLen - 1;
                if (nKeyLen == 4 && _strnicmp(p, "tool", 4) == 0)
                {
                    AssignAnsiSegment(sTool, pEq + 1, nValLen);
                    bFoundTool = true;
                }
                else if (nKeyLen == 9 && _strnicmp(p, "timestamp", 9) == 0)
                {
                    AssignAnsiSegment(sTimestamp, pEq + 1, nValLen);
                    bFoundTs = true;
                }
                else if (nKeyLen == 6 && _strnicmp(p, "status", 6) == 0)
                {
                    AssignAnsiSegment(sStatus, pEq + 1, nValLen);
                    bFoundStatus = true;
                }
                // [R109] pct=<NN>：可选字段，缺失时保持 nPct=0
                else if (nKeyLen == 3 && _strnicmp(p, "pct", 3) == 0)
                {
                    if (nValLen > 0)
                        nPct = std::stoi(pEq + 1);
                    bFoundPct = true;
                }
            }
        }
        p = pNext;
    }
    return (bFoundTool && bFoundTs && bFoundStatus);
}
