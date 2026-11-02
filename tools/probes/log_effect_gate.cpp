// [GATE-COV] LogService::AppendToFile / AppendToFileBatch -- EFFECT-level assertions.
//
// Why this probe exists: round 51 measured the suite's assertion strength and found
// only 5 of 394 assertions (1.3%) were effect-level -- they asked "did something
// actually change outside the process?" rather than "did a value come back?". That
// is exactly why AppxCapUpdService::SaveCapCache could report success-on-failure for
// 48 rounds (round 49). LogService writes JSONL to disk on every host log line, and
// nothing verified the file.
//
// What "effect-level" means here: after calling AppendToFile, the probe reads the
// JSONL file back off disk with the OS, and asserts its byte size grew and the line
// is present. The function's return value is irrelevant -- it returns void -- so the
// only honest check is whether the file changed.
//
// Isolation: ActionLogPath() derives the path from GetModuleFileNameW(nullptr), i.e.
// the directory of THIS probe executable. The probe therefore writes only next to
// itself, never into the app's tree or the user's profile. It asserts that property
// rather than assuming it (see check 0) -- if ActionLogPath ever switches to
// %LOCALAPPDATA%, this probe must stop and be re-scoped, not silently start writing
// to real user state.
#include <windows.h>

#include "LogService.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_nPass = 0;
static int g_nFail = 0;

static void Chk(const char* pszName, bool bOk, const std::string& sDetail)
{
    ::printf("[%s] %s | %s\n", bOk ? "PASS" : "FAIL", pszName, sDetail.c_str());
    ::fflush(stdout);
    if (bOk) ++g_nPass; else ++g_nFail;
}

// Read a whole file as bytes. Returns empty string when absent.
static std::string ReadAll(const std::wstring& sPath, bool* pbExisted)
{
    if (pbExisted) *pbExisted = false;
    HANDLE h = ::CreateFileW(sPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::string();
    if (pbExisted) *pbExisted = true;
    LARGE_INTEGER li = {};
    ::GetFileSizeEx(h, &li);
    std::string s;
    s.resize(static_cast<size_t>(li.QuadPart));
    DWORD dwRead = 0;
    if (!s.empty()) ::ReadFile(h, &s[0], (DWORD)s.size(), &dwRead, nullptr);
    s.resize(dwRead);
    ::CloseHandle(h);
    return s;
}

static bool Contains(const std::string& sHay, const char* pszNeedle)
{
    return sHay.find(pszNeedle) != std::string::npos;
}

// The path LogService will use, computed the same way it does.
// The path LogService will use. Ask the service itself instead of re-deriving it.
//   [R221] The old version built "<exe dir>\action_log.jsonl", but the service
//   actually writes "<exe dir>\Logs\action_log.jsonl" (ResolveLogDir appends a
//   "Logs" subdirectory). The probe therefore inspected a path nothing writes to and
//   reported "append creates the log file: exists=0" run after run, even after the
//   production write path was fixed. A probe must not duplicate the logic it is
//   verifying -- call the production accessor.
static std::wstring LogPath()
{
    return LogService::UnifiedLogPath(L"action_log.jsonl");
}

int main(int argc, char** argv)
{
    bool bInject = false;
    for (int i = 1; i < argc; ++i) {
        if (::strcmp(argv[i], "/Inject") == 0) bInject = true;
    }

    const std::wstring sLog = LogPath();

    // --- 0. isolation guard: the log must live next to THIS executable ---
    // ActionLogPath() keys off GetModuleFileNameW, so the file lands beside the
    // probe. Assert it: if that ever changes to a user-profile path, this probe
    // would start writing to real state and must be re-scoped instead.
    {
        wchar_t szDir[MAX_PATH] = {};
        ::GetModuleFileNameW(nullptr, szDir, MAX_PATH);
        wchar_t* pSlash = ::wcsrchr(szDir, L'\\');
        if (pSlash) *pSlash = L'\0';
        const bool bBeside = (sLog.find(szDir) == 0);
        Chk("action log path is beside the probe executable",
            bBeside,
            "beside_exe=" + std::string(bBeside ? "1" : "0"));
        if (!bBeside) {
            ::printf("REFUSING to continue: the log path is not isolated");
            ::putchar(10);
            ::fflush(stdout);
            return 2;
        }
    }

    ::DeleteFileW(sLog.c_str());

    // --- 1. the file does not exist before the first append ---
    {
        bool bExisted = false;
        ReadAll(sLog, &bExisted);
        Chk("log file is absent before any append", !bExisted,
            std::string("existed=") + (bExisted ? "1" : "0"));
    }

    // --- 2. ONE append must create the file and put the line in it ---
    // This is the assertion the old suite was missing entirely: it does not look at
    // the object's state, it reads the file back with the OS.
    {
        LogService svc;
        svc.AppendToFile(L"gate-marker-line", LOGSRV_WARN);

        bool bExisted = false;
        const std::string sBody = ReadAll(sLog, &bExisted);
        Chk("append creates the log file on disk", bExisted,
            std::string("exists=") + (bExisted ? "1" : "0") +
            " bytes=" + std::to_string(sBody.size()));
        Chk("the written line is present in the file",
            Contains(sBody, "gate-marker-line"),
            "bytes=" + std::to_string(sBody.size()));
        // Format contract: one JSON object per line, with the level rendered as text.
        // A regression that wrote plain text would still satisfy "line present", so
        // assert the JSONL shape too.
        Chk("the line is JSONL with a level field",
            Contains(sBody, "\"level\":\"warn\"") && Contains(sBody, "\"ts\":\"") &&
            Contains(sBody, "\"msg\":\""),
            "bytes=" + std::to_string(sBody.size()));
    }

    // --- 3. a second append grows the file (append, not overwrite) ---
    // OPEN_ALWAYS + FILE_APPEND_DATA should append. If a refactor switched to
    // CREATE_ALWAYS, every log line would erase the previous one and the file would
    // stay one line long -- invisible to any "file exists" check.
    {
        bool bExisted = false;
        const std::string sBefore = ReadAll(sLog, &bExisted);
        LogService svc;
        svc.AppendToFile(L"second-line", LOGSRV_ERROR);
        const std::string sAfter = ReadAll(sLog, &bExisted);
        Chk("a second append grows the file instead of replacing it",
            sAfter.size() > sBefore.size() && Contains(sAfter, "gate-marker-line") &&
            Contains(sAfter, "second-line"),
            "before=" + std::to_string(sBefore.size()) +
            " after=" + std::to_string(sAfter.size()));
    }

    // --- 4. level mapping reaches the file (error != warn) ---
    {
        bool bExisted = false;
        const std::string sBody = ReadAll(sLog, &bExisted);
        Chk("error level is rendered distinctly from warn",
            Contains(sBody, "\"level\":\"error\""),
            "bytes=" + std::to_string(sBody.size()));
    }

    // --- 5. JSON escaping: a quote in the message must not break the JSONL ---
    // Each line has to stay parseable. An unescaped quote makes the record invalid
    // JSON and any consumer reading the file line-by-line would fail on it.
    {
        LogService svc;
        svc.AppendToFile(L"quote\"and\\backslash", LOGSRV_INFO);
        bool bExisted = false;
        const std::string sBody = ReadAll(sLog, &bExisted);
        // Count unescaped quotes per line: a valid record has an even number.
        size_t nBadLines = 0;
        size_t nLines = 0;
        size_t pos = 0;
        while (pos < sBody.size()) {
            size_t e = sBody.find('\n', pos);
            if (e == std::string::npos) e = sBody.size();
            std::string line = sBody.substr(pos, e - pos);
            if (!line.empty()) {
                ++nLines;
                int nQuotes = 0;
                for (size_t k = 0; k < line.size(); ++k) {
                    if (line[k] == '"' && (k == 0 || line[k - 1] != '\\')) ++nQuotes;
                }
                if (nQuotes % 2 != 0) ++nBadLines;
            }
            pos = e + 1;
        }
        Chk("every written line has balanced JSON quotes",
            nLines > 0 && nBadLines == 0,
            "lines=" + std::to_string(nLines) + " unbalanced=" + std::to_string(nBadLines));
    }

    // --- 6. batch append writes every item, and only opens the file once ---
    // AppendToFileBatch exists because a log storm was opening/closing the file per
    // line. If it silently dropped items, the file would grow by fewer bytes than the
    // batch size -- which a per-call "did it return" check cannot see (it returns void).
    {
        bool bExisted = false;
        const std::string sBefore = ReadAll(sLog, &bExisted);
        std::vector<std::pair<DuiLib::CDuiString, int> > vItems;
        for (int i = 0; i < 5; ++i) {
            wchar_t line[48] = {};
            ::swprintf_s(line, L"batch-item-%d", i);
            vItems.push_back(std::make_pair(DuiLib::CDuiString(line), LOGSRV_INFO));
        }
        LogService svc;
        svc.AppendToFileBatch(vItems);
        const std::string sAfter = ReadAll(sLog, &bExisted);

        int nFound = 0;
        for (int i = 0; i < 5; ++i) {
            char szNeedle[48] = {};
            ::sprintf_s(szNeedle, "batch-item-%d", i);
            if (Contains(sAfter, szNeedle)) ++nFound;
        }
        Chk("batch append writes all 5 items to disk",
            nFound == 5 && sAfter.size() > sBefore.size(),
            "found=" + std::to_string(nFound) +
            " grew=" + std::to_string((int)(sAfter.size() - sBefore.size())));
    }

    // --- 7. an empty batch must not touch the file ---
    {
        bool bExisted = false;
        const std::string sBefore = ReadAll(sLog, &bExisted);
        std::vector<std::pair<DuiLib::CDuiString, int> > vEmpty;
        LogService svc;
        svc.AppendToFileBatch(vEmpty);
        const std::string sAfter = ReadAll(sLog, &bExisted);
        Chk("an empty batch leaves the file byte-identical",
            sAfter.size() == sBefore.size(),
            "before=" + std::to_string(sBefore.size()) +
            " after=" + std::to_string(sAfter.size()));
    }

    // --- 8. a null line must not create a file or a record ---
    {
        ::DeleteFileW(sLog.c_str());
        LogService svc;
        svc.AppendToFile(nullptr, LOGSRV_INFO);
        const DWORD dwAttr = ::GetFileAttributesW(sLog.c_str());
        Chk("a null line writes no file at all",
            dwAttr == INVALID_FILE_ATTRIBUTES,
            std::string("exists=") +
            (dwAttr == INVALID_FILE_ATTRIBUTES ? "0" : "1"));
    }

    // --- 9. the in-memory audit ring also caps (secondary effect) ---
    // BufferedCount is a different buffer from the file, but it is the one the UI
    // reads back for replay, so a broken cap there silently drops history.
    {
        ::DeleteFileW(sLog.c_str());
        LogService svc;
        for (int i = 0; i < 320; ++i) {
            wchar_t line[32] = {};
            ::swprintf_s(line, L"offline-%d", i);
            svc.BufferOffline(line);
        }
        const size_t nKept = svc.BufferedCount();
        Chk("offline buffer caps at exactly 300 entries", nKept == 300,
            "fed=320 kept=" + std::to_string(nKept));
    }

    ::DeleteFileW(sLog.c_str());

    if (bInject) {
        Chk("injected-must-fail", false, "self-test: forced failure");
    }

    ::printf("PROBE_SUMMARY pass=%d fail=%d\n", g_nPass, g_nFail);
    ::fflush(stdout);
    return (g_nFail == 0) ? 0 : 1;
}
