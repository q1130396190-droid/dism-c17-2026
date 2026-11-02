// [GATE-COV] ProgramService read path -- enumeration only.
//
// Why this exists: round 30's coverage audit listed 27 of 46 subsystems with no
// assertion. ProgramService is one of the safe ones to cover: EnumeratePrograms
// only reads HKLM/HKCU Uninstall keys, never writes.
//
// A silent break here is especially deceptive: the page would show "no installed
// programs", which reads as a fresh machine rather than a failed query.
//
// Assertions (all side-effect free):
//   1. enumeration returns entries (a Windows box always has some);
//   2. every entry has a display name (the enumeration filters on it) and a
//      non-empty subkey;
//   3. names are unique -- the implementation dedups case-insensitively, and a
//      duplicate would make row-keyed uninstall hit the wrong program;
//   4. nRoot is within the three documented hives;
//   5. idempotent: two calls agree on count.
#include "stdafx.h"
#include "ProgramService.h"

#include <cstdio>
#include <set>
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

    std::vector<ProgEntry> v1;
    ProgramService::EnumeratePrograms(v1);

    char sz[256] = {};
    ::sprintf_s(sz, "programs=%d", (int)v1.size());
    ::printf("[INFO] measured programs=%d\n", (int)v1.size());
    ::fflush(stdout);

    // [ZERO-INPUT] Zero programs on a Windows install means the Uninstall keys
    // were not read (wrong view, missing hive, or a broken query), not "clean".
    if (v1.empty())
    {
        Chk("enumeration produced entries", false, "0 programs -- nothing was measured");
    }
    else
    {
        Chk("enumeration produced entries", true, sz);

        int nNoName = 0;
        int nNoSub = 0;
        int nBadRoot = 0;
        std::set<std::wstring> setNames;
        for (size_t i = 0; i < v1.size(); ++i)
        {
            if (v1[i].sName.IsEmpty()) ++nNoName;
            else setNames.insert(std::wstring(v1[i].sName.GetData()));
            if (v1[i].sSubKey.IsEmpty()) ++nNoSub;
            if (v1[i].nRoot < 0 || v1[i].nRoot > 2) ++nBadRoot;
        }

        ::sprintf_s(sz, "programs=%d noName=%d noSubKey=%d badRoot=%d unique=%d",
                    (int)v1.size(), nNoName, nNoSub, nBadRoot, (int)setNames.size());
        Chk("every entry has a display name", nNoName == 0, sz);
        Chk("every entry has a subkey", nNoSub == 0, sz);
        Chk("hive index stays within the three documented sources", nBadRoot == 0, sz);
        // Dedup is case-insensitive in the implementation; a duplicate here means
        // uninstall-by-name could hit the wrong program.
        Chk("program names are unique", setNames.size() == v1.size(), sz);

        // ---- idempotent ------------------------------------------------
        std::vector<ProgEntry> v2;
        ProgramService::EnumeratePrograms(v2);
        ::sprintf_s(sz, "first=%d second=%d", (int)v1.size(), (int)v2.size());
        Chk("enumeration is idempotent", v1.size() == v2.size(), sz);

        if (!v1.empty())
        {
            ::printf("[INFO] sample: name=%s publisher=%s\n",
                     W2A(v1[0].sName.GetData()).c_str(),
                     W2A(v1[0].sPublisher.GetData()).c_str());
            ::fflush(stdout);
        }
    }

    if (bInject)
    {
        Chk("injected-must-fail", false, "self-test: forced failure");
    }

    ::printf("\nresult: %d pass / %d fail\n", g_nPass, g_nFail);
    return (g_nFail == 0) ? 0 : 1;
}
