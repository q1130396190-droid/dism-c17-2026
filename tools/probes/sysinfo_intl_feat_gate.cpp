// [GATE-COV] Read paths for three uncovered subsystems: SystemInfoService,
// IntlService, FeatureService.
//
// Why these three together: all three are read-only, Win32-only (no COM, no
// elevation), and each was listed as "no assertion anywhere" by the round-30
// coverage audit. Combining them in one probe keeps the build cost low while
// still giving each a real value assertion.
//
// What is NOT tested: every write API on these services (SetTimeZone,
// SetUserLocaleName, SaveFeatCache...). Those mutate the machine or the cache, so
// a gate must not call them.
//
// Every assertion checks a VALUE. "Did not crash" is not a check: a function
// returning an empty string forever would pass such a test.
#include "stdafx.h"
#include "SystemInfoService.h"
#include "IntlService.h"

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

    // ================= SystemInfoService =================
    {
        const CDuiString sDrive = SystemInfoService::GetSystemDriveStr();
        const DWORD dwBuild = SystemInfoService::GetBuildNumber();
        const CDuiString sFamily = SystemInfoService::GetWindowsFamilyName();

        char sz[256] = {};
        ::sprintf_s(sz, "drive=%ls build=%lu family=%ls",
                    sDrive.GetData(), (unsigned long)dwBuild, sFamily.GetData());
        ::printf("[INFO] measured sysinfo: %s\n", sz);
        ::fflush(stdout);

        // A drive letter is "X:"; empty or malformed means GetWindowsDirectory
        // failed and every path built from it would be wrong.
        const bool bDriveOk = sDrive.GetLength() == 2 && sDrive.GetAt(1) == L':';
        Chk("system drive is a valid drive letter", bDriveOk, sz);

        // Windows 10 is 10240+; anything lower means the registry read failed
        // (GetBuildNumber returns 0 on failure).
        Chk("build number was read from the registry", dwBuild >= 10240, sz);

        // DisplayName must produce something; empty would leave the OS column blank.
        Chk("windows family name is non-empty", sFamily.GetLength() > 0, sz);
    }

    // ================= IntlService =================
    {
        CDuiString sTz;
        const bool bTzOk = IntlService::GetCurrentTimeZone(sTz);
        CDuiString sLocale;
        const bool bLocOk = IntlService::GetUserLocaleName(sLocale);

        char sz[512] = {};
        ::sprintf_s(sz, "tzOk=%d tz=%ls locOk=%d loc=%ls",
                    bTzOk ? 1 : 0, sTz.GetData(), bLocOk ? 1 : 0, sLocale.GetData());
        ::printf("[INFO] measured intl: %s\n", sz);
        ::fflush(stdout);

        // A machine always has a time zone; failing to read it means the
        // registry query broke, and the region page would show a blank column.
        Chk("current time zone is readable", bTzOk && !sTz.IsEmpty(), sz);
        Chk("user locale name is readable", bLocOk && !sLocale.IsEmpty(), sz);
    }

    // ================= FeatureService: not covered here =================
    // LoadSeedFeatCache pulls DismApiBridge + seedcache + dismcore symbols, i.e.
    // the whole DISM stack, just to read a seed list. That cost is out of
    // proportion for a read-path gate; FeatureService is left on the uncovered
    // list on purpose rather than covered by a 500-line link that only exists to
    // satisfy one function.
    if (bInject)
    {
        Chk("injected-must-fail", false, "self-test: forced failure");
    }

    ::printf("\nresult: %d pass / %d fail\n", g_nPass, g_nFail);
    return (g_nFail == 0) ? 0 : 1;
}
