// [GATE-COV] ExternalToolService -- tool discovery only, never launches anything.
//
// STATUS: NOT registered as a gate. ExternalToolService.cpp relies on shellapi.h /
// shlwapi.h / shlobj.h arriving through the Demo's precompiled header; compiling it
// standalone needs that PCH environment reproduced (four build-configuration
// attempts failed: missing SHELLEXECUTEINFOW -> forced includes -> shellapi needs
// windows.h first -> ...). The read surface here is three path lookups plus
// IsToolAvailable, which is not worth rebuilding the PCH chain for.
//
// Recorded rather than faked: ExternalToolService stays on the uncovered list, the
// same decision as FeatureService (round 43). Coverage% is not the goal.
//
// Why: it was on the round-43 uncovered list and its read side is genuinely
// testable. But this service's whole purpose is launching external programs, so a
// gate must be explicit about what it refuses to touch:
//   * LaunchSystem / RunHidden* -- would start processes on the developer's machine
//   * WriteBatFile              -- writes files
// Only the pure lookups run here.
//
// What a silent break looks like: FindBootsect() returns empty, so the "create
// boot media" card greys out and the user concludes the feature is unsupported --
// not that discovery broke.
#include <windows.h>
#include <shellapi.h>   /* SHELLEXECUTEINFOW used by ExternalToolService.cpp */
#include <shlobj.h>
#include "stdafx.h"
#include "ExternalToolService.h"

#include <cstdio>
#include <string>

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
    char sz[512] = {};
    ::WideCharToMultiByte(CP_UTF8, 0, psz ? psz : L"", -1, sz, sizeof(sz) - 1, nullptr, nullptr);
    return std::string(sz);
}

int main(int argc, char** argv)
{
    bool bInject = false;
    for (int i = 1; i < argc; ++i)
        if (::lstrcmpiA(argv[i], "/Inject") == 0) bInject = true;

    // ---- 1) discovery of the boot-sector tools -------------------------
    // These may legitimately be absent (a dev box is not install media), so the
    // assertion is NOT "found"; it is "if found, the path exists".
    {
        const CDuiString sBootsect = ExternalToolService::FindBootsect();
        const CDuiString sEtfs    = ExternalToolService::FindEtfsboot();
        const CDuiString sEfi     = ExternalToolService::FindEfisys();

        char sz[512] = {};
        ::sprintf_s(sz, "bootsect=%ls etfsboot=%ls efisys=%ls",
                    sBootsect.IsEmpty() ? L"(none)" : sBootsect.GetData(),
                    sEtfs.IsEmpty() ? L"(none)" : sEtfs.GetData(),
                    sEfi.IsEmpty() ? L"(none)" : sEfi.GetData());
        ::printf("[INFO] measured tools: %s\n", sz);
        ::fflush(stdout);

        // A returned path that does not exist would be worse than none: the card
        // would look available and then fail at launch.
        auto PathOk = [](const CDuiString& s) {
            if (s.IsEmpty()) return true;                     // absent is fine
            return ::GetFileAttributesW(s.GetData()) != INVALID_FILE_ATTRIBUTES;
        };
        ::sprintf_s(sz, "bootsectOk=%d etfsOk=%d efiOk=%d",
                    PathOk(sBootsect) ? 1 : 0, PathOk(sEtfs) ? 1 : 0, PathOk(sEfi) ? 1 : 0);
        Chk("every discovered tool path actually exists", PathOk(sBootsect) && PathOk(sEtfs) && PathOk(sEfi), sz);
    }

    // ---- 2) IsToolAvailable: known-present and known-absent ------------
    {
        // cmd.exe ships with Windows; a random name cannot be on PATH.
        const bool bCmd = ExternalToolService::IsToolAvailable(L"cmd.exe");
        const bool bBogus = ExternalToolService::IsToolAvailable(L"__no_such_tool_xyz__.exe");
        const bool bEmpty = ExternalToolService::IsToolAvailable(L"");

        char sz[192] = {};
        ::sprintf_s(sz, "cmd=%d bogus=%d empty=%d",
                    bCmd ? 1 : 0, bBogus ? 1 : 0, bEmpty ? 1 : 0);
        ::printf("[INFO] measured availability: %s\n", sz);
        ::fflush(stdout);

        Chk("a tool that is certainly installed is reported available", bCmd, sz);
        Chk("an invented tool name is reported unavailable", !bBogus, sz);
        // A null/empty query must not be answered with "yes".
        Chk("empty tool name is rejected", !bEmpty, sz);
    }

    // ---- 3) elevation query answers without prompting ------------------
    {
        const bool bElev = ExternalToolService::IsElevated();
        char sz[96] = {};
        ::sprintf_s(sz, "elevated=%d", bElev ? 1 : 0);
        ::printf("[INFO] measured elevation: %s\n", sz);
        ::fflush(stdout);
        // Both answers are valid depending on how the gate was launched; what must
        // hold is that the query completes (a UAC prompt here would hang the suite).
        Chk("elevation query completes without prompting", true, sz);
    }

    if (bInject)
    {
        Chk("injected-must-fail", false, "self-test: forced failure");
    }

    ::printf("\nresult: %d pass / %d fail\n", g_nPass, g_nFail);
    return (g_nFail == 0) ? 0 : 1;
}
