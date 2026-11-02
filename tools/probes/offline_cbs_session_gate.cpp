// [GATE-COV] OfflineCbsSession::PrepareSourceDir -- the "not ready" path only.
//
// Scope, stated so the gate cannot drift:
//   * PrepareSourceDir with an empty and with a non-existent mount root. Both make
//     OfflineCbs::CheckReadiness fail, so the ready-branch (StopTrustedInstaller,
//     MountAllHives, BeginOfflineCbsSession, CbsHostClient log write) is never
//     entered. Those three are what make this service expensive to test: stopping
//     TrustedInstaller and mounting hives are machine-level side effects that
//     cannot be undone by a probe.
//   * CheckReadiness(nullptr) -- the BUG-13 regression guard. The original code
//     dereferenced the null image root in the BCD branch; the fix is an explicit
//     early return with ERROR_INVALID_PARAMETER.
//
// The assertion that matters most is not the formatting: it is that a rejected
// path leaves the filesystem untouched. A probe that only checks the message
// could pass while the service still mounted a hive into the caller's directory.
//
// Build note: this TU includes Demos/Win8Metro/stdafx.h, which pulls in UIlib.h and
// `using namespace DuiLib`. The shim directory on the include path substitutes a
// framework-free prelude because OfflineCbsService.cpp has zero DuiLib references.
#include <windows.h>

#include "stdafx.h"
#include "OfflineCbsSessionService.h"
#include "OfflineCbsService.h"

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

static std::string W2A(const wchar_t* psz)
{
    char sz[1024] = {};
    ::WideCharToMultiByte(CP_UTF8, 0, psz ? psz : L"", -1, sz, sizeof(sz) - 1, nullptr, nullptr);
    return std::string(sz);
}

static std::string Num(long long v)
{
    char sz[64] = {};
    ::sprintf_s(sz, "%lld", v);
    return std::string(sz);
}

// Count entries in a directory. Used as the read-only proof: a rejected path must
// not create anything (no hive copes, no OfflineCbs.log).
static int CountEntries(const wchar_t* pszDir)
{
    std::wstring sPattern = pszDir;
    if (!sPattern.empty() && sPattern.back() != L'\\') sPattern += L'\\';
    sPattern += L"*";
    WIN32_FIND_DATAW fd = {};
    HANDLE h = ::FindFirstFileW(sPattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    int n = 0;
    do {
        if (::wcscmp(fd.cFileName, L".") == 0 || ::wcscmp(fd.cFileName, L"..") == 0) continue;
        ++n;
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    return n;
}

// Delete every entry directly under pszDir (files and directories). One level is
// enough: the service only ever drops a single file there.
static void RemoveAllIn(const wchar_t* pszDir)
{
    std::wstring sPattern = pszDir;
    if (!sPattern.empty() && sPattern.back() != L'\\') sPattern += L'\\';
    sPattern += L"*";
    WIN32_FIND_DATAW fd = {};
    HANDLE h = ::FindFirstFileW(sPattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (::wcscmp(fd.cFileName, L".") == 0 || ::wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring sFull = pszDir;
        if (!sFull.empty() && sFull.back() != L'\\') sFull += L'\\';
        sFull += fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ::RemoveDirectoryW(sFull.c_str());
        else ::DeleteFileW(sFull.c_str());
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
}

static bool Contains(const std::wstring& sHay, const wchar_t* pszNeedle)
{
    return sHay.find(pszNeedle) != std::wstring::npos;
}

int main(int argc, char** argv)
{
    bool bInject = false;
    for (int i = 1; i < argc; ++i) {
        if (::strcmp(argv[i], "/Inject") == 0) bInject = true;
    }

    // --- 1. empty mount root: rejected, warning severity ---
    {
        OfflineCbsSession::TPrepareResult r = OfflineCbsSession::PrepareSourceDir(L"");
        Chk("empty mount root is rejected",
            r.bReady == false,
            std::string("bReady=") + (r.bReady ? "1" : "0"));
        Chk("rejection is reported as a warning, not info",
            r.bWarning == true,
            std::string("bWarning=") + (r.bWarning ? "1" : "0"));
        Chk("empty root still produces a title and a message",
            !r.sTitle.empty() && r.sMessage.size() > 0,
            "title_len=" + Num((long long)r.sTitle.size()) +
            " msg_len=" + Num((long long)r.sMessage.size()));
    }

    // --- 2. non-existent root: the message must name the path it rejected ---
    // A path that cannot exist. Deliberately under the system temp dir so a stray
    // mkdir by the service would land somewhere a probe can see it.
    wchar_t szFake[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, szFake);
    ::wcscat_s(szFake, L"duilib_gate_nonexistent_cbs_root_9f3a");
    ::RemoveDirectoryW(szFake);          // ensure it really is absent

    OfflineCbsSession::TPrepareResult r2 = OfflineCbsSession::PrepareSourceDir(szFake);
    Chk("non-existent mount root is rejected",
        r2.bReady == false,
        std::string("bReady=") + (r2.bReady ? "1" : "0"));
    Chk("message quotes the rejected path",
        Contains(r2.sMessage, L"duilib_gate_nonexistent_cbs_root_9f3a"),
        "msg_len=" + Num((long long)r2.sMessage.size()));

    // The failure message enumerates all four readiness facets. If a refactor
    // drops one, the user loses the only diagnostic telling them WHICH
    // precondition failed -- so assert all four labels survive in the text.
    {
        const bool bHives    = Contains(r2.sMessage, L"\u8702\u5de2");
        const bool bTi       = Contains(r2.sMessage, L"TrustedInstaller");
        const bool bPending  = Contains(r2.sMessage, L"pending.xml");
        const bool bLogDir   = Contains(r2.sMessage, L"\u65e5\u5fd7\u76ee\u5f55");
        Chk("failure text still reports all four preconditions",
            bHives && bTi && bPending && bLogDir,
            std::string("hives=") + (bHives ? "1" : "0") +
            " ti=" + (bTi ? "1" : "0") +
            " pending=" + (bPending ? "1" : "0") +
            " logdir=" + (bLogDir ? "1" : "0"));
    }

    // --- 3. read-only proof: a rejected path must not touch the filesystem ---
    // CheckReadiness probes hives by GetFileAttributes and writes nothing, so the
    // root must stay absent. If a future change mounts hives or opens the log
    // before validating, this is where it shows up.
    {
        const DWORD dwAttr = ::GetFileAttributesW(szFake);
        Chk("rejected path was not created on disk",
            dwAttr == INVALID_FILE_ATTRIBUTES,
            std::string("attrs=") + Num((long long)dwAttr));
    }
    // And in a directory that DOES exist, no file may appear.
    {
        wchar_t szTmp[MAX_PATH] = {};
        ::GetTempPathW(MAX_PATH, szTmp);
        ::wcscat_s(szTmp, L"duilib_gate_cbs_ro_9f3a");
        ::CreateDirectoryW(szTmp, nullptr);
        // Start from a known-empty root. A leftover file from an earlier run (or
        // from an injected regression) would otherwise make nBefore non-zero and
        // the assertion would fail for a reason that has nothing to do with the
        // code under test -- a gate that depends on residue is not a gate.
        RemoveAllIn(szTmp);
        const int nBefore = CountEntries(szTmp);
        OfflineCbs::OfflineReadiness rdIgnore = {};
        const bool bReady = OfflineCbs::CheckReadiness(szTmp, &rdIgnore);
        const int nAfter = CountEntries(szTmp);
        Chk("CheckReadiness writes nothing into the probed root",
            bReady == false && nBefore == 0 && nAfter == 0,
            "ready=" + std::string(bReady ? "1" : "0") +
            " before=" + Num(nBefore) + " after=" + Num(nAfter));
        ::RemoveDirectoryW(szTmp);
    }

    // --- 4. BUG-13 guard: null image root must be refused, not dereferenced ---
    // The original BCD branch called sPath.back() and BuildConfigPath on a null
    // root. A regression here crashes the probe rather than failing an assertion,
    // which is still a hard failure -- but the assertion documents the contract.
    {
        ::SetLastError(0);
        OfflineCbs::OfflineReadiness rd = {};
        const bool bReady = OfflineCbs::CheckReadiness(nullptr, &rd);
        const DWORD dwErr = ::GetLastError();
        Chk("null image root is refused with ERROR_INVALID_PARAMETER",
            bReady == false && dwErr == ERROR_INVALID_PARAMETER,
            "ready=" + std::string(bReady ? "1" : "0") + " err=" + Num((long long)dwErr));
    }

    if (bInject) {
        Chk("injected-must-fail", false, "self-test: forced failure");
    }

    ::printf("PROBE_SUMMARY pass=%d fail=%d\n", g_nPass, g_nFail);
    ::fflush(stdout);
    return (g_nFail == 0) ? 0 : 1;
}
