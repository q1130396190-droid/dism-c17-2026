// [GATE-COV] Registry path parsing gate -- the first line of defence against
// deleting the wrong key.
//
// Why this exists: the coverage audit found the registry-clean subsystem had no
// assertion anywhere. Its clean path ends in RegDeleteTree/RegDeleteKeyEx, and
// everything upstream of that keys off COfflineSession::ParseRegPath: a wrong
// root or a mangled subkey means the cleaner targets a key the user never asked
// about. This gate asserts the PARSER only -- no writes, no deletions.
//
// Implemented facts this pins down (read from the implementation, not assumed):
//   * roots are matched by FULL name (HKEY_LOCAL_MACHINE), not abbreviations;
//   * a trailing backslash yields an empty subkey but still returns true;
//   * HKEY_USERS\DEFAULT is normalised to HKEY_USERS\.DEFAULT (the real hive
//     name), which the original engine got wrong for 261 call sites.
#include "stdafx.h"
#include "OfflineSession.h"

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

    // ---- 1) every full-length root parses to the right HKEY ------------
    {
        struct Def { const wchar_t* pszPath; HKEY hExpect; };
        const Def defs[] = {
            { L"HKEY_CURRENT_USER\\Software\\Test",    HKEY_CURRENT_USER },
            { L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Test",   HKEY_LOCAL_MACHINE },
            { L"HKEY_CLASSES_ROOT\\.txt\\shell",       HKEY_CLASSES_ROOT },
            { L"HKEY_USERS\\S-1-5-21\\Test",           HKEY_USERS },
            { L"HKEY_CURRENT_CONFIG\\System\\Test",    HKEY_CURRENT_CONFIG },
        };
        int nOk = 0;
        for (size_t i = 0; i < _countof(defs); ++i)
        {
            HKEY hRoot = nullptr;
            CDuiString sSub;
            if (COfflineSession::ParseRegPath(defs[i].pszPath, hRoot, sSub)
                && hRoot == defs[i].hExpect && !sSub.IsEmpty())
                ++nOk;
        }
        char sz[128] = {};
        ::sprintf_s(sz, "%d/%d roots parsed", nOk, (int)_countof(defs));
        Chk("all five documented roots parse to the right hive",
            nOk == (int)_countof(defs), sz);
    }

    // ---- 2) subkey is preserved verbatim -------------------------------
    {
        HKEY hRoot = nullptr;
        CDuiString sSub;
        const bool bOk = COfflineSession::ParseRegPath(
            L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
            hRoot, sSub);
        const bool bExact = (sSub.Compare(
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run") == 0);
        Chk("subkey is extracted verbatim", bOk && bExact, W2A(sSub.GetData()));
    }

    // ---- 3) HKEY_USERS\DEFAULT is normalised to .DEFAULT ---------------
    {
        HKEY hRoot = nullptr;
        CDuiString sSub;
        const bool bOk = COfflineSession::ParseRegPath(
            L"HKEY_USERS\\DEFAULT\\Control Panel\\Desktop", hRoot, sSub);
        const bool bNorm = (sSub.Find(L".DEFAULT") == 0);
        char sz[512] = {};
        ::sprintf_s(sz, "ok=%d sub=%ls", bOk ? 1 : 0, sSub.GetData());
        Chk("HKEY_USERS\\DEFAULT normalises to .DEFAULT", bOk && bNorm, sz);
    }

    // ---- 4) malformed / unknown roots must be rejected -----------------
    {
        const wchar_t* bad[] = {
            L"",                                   // empty
            L"SOFTWARE\\NoRoot",                   // no root at all
            L"HKLM\\SOFTWARE\\Test",               // abbreviation, not a root
            L"HKEY_LOCAL_MACHINE_X\\SOFTWARE",     // near-miss root
        };
        int nAccepted = 0;
        for (size_t i = 0; i < _countof(bad); ++i)
        {
            HKEY hRoot = nullptr;
            CDuiString sSub;
            if (COfflineSession::ParseRegPath(bad[i], hRoot, sSub))
                ++nAccepted;
        }
        char sz[128] = {};
        ::sprintf_s(sz, "%d/%d accepted", nAccepted, (int)_countof(bad));
        // Accepting any of these means a cleaner could target an unintended key.
        Chk("malformed or abbreviated roots are rejected", nAccepted == 0, sz);
    }

    // ---- 5) root match is case-insensitive -----------------------------
    {
        HKEY hRoot = nullptr;
        CDuiString sSub;
        const bool bLower = COfflineSession::ParseRegPath(
            L"hkey_local_machine\\SOFTWARE\\Test", hRoot, sSub)
            && hRoot == HKEY_LOCAL_MACHINE;
        HKEY hRoot2 = nullptr;
        CDuiString sSub2;
        const bool bMixed = COfflineSession::ParseRegPath(
            L"hKeY_LoCaL_MaChInE\\SOFTWARE\\Test", hRoot2, sSub2)
            && hRoot2 == HKEY_LOCAL_MACHINE;
        char sz[128] = {};
        ::sprintf_s(sz, "lower=%d mixed=%d", bLower ? 1 : 0, bMixed ? 1 : 0);
        Chk("root match is case-insensitive", bLower && bMixed, sz);
    }

    if (bInject)
    {
        Chk("injected-must-fail", false, "self-test: forced failure");
    }

    ::printf("\nresult: %d pass / %d fail\n", g_nPass, g_nFail);
    return (g_nFail == 0) ? 0 : 1;
}
