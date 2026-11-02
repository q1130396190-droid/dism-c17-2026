// [GATE-COV] Assert on StartupService's read path.
//
// Why this exists: the coverage audit found 27 of 46 subsystems have no probe
// asserting on them at all -- StartupService among them. Its pages are checked
// ("does page_startup have N controls"), but nothing checked whether the
// enumeration itself is correct. A silent break there shows up as an empty or
// wrongly-classified startup list, which looks like "this machine has no startup
// items" rather than a bug.
//
// Scope: READ ONLY, deliberately. AddCurrentUserRunItem / DeleteStartupEntry /
// SetStartupApproved write to the real registry, so calling them in a gate would
// mutate the developer's machine. What is assertable without side effects:
//   1. enumeration returns without crashing and is idempotent (two calls agree);
//   2. every entry has a non-empty name and a known source type;
//   3. entries with an executable command get a consistent arch verdict
//      (0 = unknown is allowed; anything else must be 32 or 64);
//   4. service enumeration returns the Win32 bucket non-empty on a normal box.
#include <windows.h>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "StartupService.h"

static int g_nPass = 0;
static int g_nFail = 0;

static void Chk(const char* pszName, bool bOk, const std::string& sDetail)
{
    ::printf("[%s] %s | %s\n", bOk ? "PASS" : "FAIL", pszName, sDetail.c_str());
    ::fflush(stdout);
    if (bOk) ++g_nPass; else ++g_nFail;
}

static void Chk(const char* pszName, bool bOk, const wchar_t* pszDetail)
{
    char sz[512] = {};
    ::WideCharToMultiByte(CP_UTF8, 0, pszDetail ? pszDetail : L"", -1, sz, sizeof(sz) - 1, nullptr, nullptr);
    Chk(pszName, bOk, std::string(sz));
}

int main(int argc, char** argv)
{
    bool bInject = false;
    for (int i = 1; i < argc; ++i)
        if (::lstrcmpiA(argv[i], "/Inject") == 0) bInject = true;

    // ---- 1) enumeration is stable across calls -------------------------
    std::vector<StartupEntry> v1;
    StartupService::EnumerateStartup(v1);
    std::vector<StartupEntry> v2;
    StartupService::EnumerateStartup(v2);

    char sz[256] = {};
    ::sprintf_s(sz, "first=%d second=%d", (int)v1.size(), (int)v2.size());
    Chk("enumeration is idempotent (two calls agree on count)",
        v1.size() == v2.size(), sz);

    // ---- 2) every entry is well formed --------------------------------
    int nEmptyName = 0;
    int nBadSrc = 0;
    std::set<std::wstring> setNames;
    for (size_t i = 0; i < v1.size(); ++i)
    {
        if (v1[i].sName.IsEmpty()) ++nEmptyName;
        else setNames.insert(std::wstring(v1[i].sName.GetData()));

        const int nSrc = static_cast<int>(v1[i].nSrcType);
        if (nSrc < 0 || nSrc > 32) ++nBadSrc;
    }
    ::sprintf_s(sz, "entries=%d emptyName=%d badSrc=%d unique=%d",
                (int)v1.size(), nEmptyName, nBadSrc, (int)setNames.size());
    Chk("no entry has an empty name", nEmptyName == 0, sz);
    Chk("every entry has a plausible source type", nBadSrc == 0, sz);

    // ---- 3) arch verdict is self-consistent ---------------------------
    int nBadArch = 0;
    for (size_t i = 0; i < v1.size(); ++i)
    {
        const int nArch = v1[i].nArch;
        // 0 = unknown (allowed: relative path, missing file, non-PE).
        if (nArch != 0 && nArch != 32 && nArch != 64) ++nBadArch;
    }
    ::sprintf_s(sz, "badArch=%d", nBadArch);
    Chk("arch verdict is 0/32/64 only", nBadArch == 0, sz);

    // ---- 4) service enumeration still finds Win32 services -------------
    std::vector<ServiceEntry> vSvc;
    const int nRet = StartupService::EnumerateServices(vSvc);
    int nWin32 = 0;
    for (size_t i = 0; i < vSvc.size(); ++i)
        if (vSvc[i].nSvcType == SERVICE_WIN32) ++nWin32;

    ::sprintf_s(sz, "ret=%d total=%d win32=%d", nRet, (int)vSvc.size(), nWin32);
    // Measured on this host: total=693 win32=283. "> 0" was too weak -- it would
    // pass a regression that enumerated a single service and stopped, which the page
    // renders as an almost-empty list and is indistinguishable from "working" in
    // any existence check. A floor of 20 is deliberately far below the measured 283
    // (SKUs differ, services get disabled) while still catching a collapse.
    Chk("Win32 service enumeration is not collapsed", nWin32 >= 20, sz);
    // EnumerateServices returns 3 when the Win32 primary enumeration came back empty
    // -- its documented fatal state. The probe used to print ret=%d into the detail
    // string but never assert it, so a regression that hit exactly that path would
    // show ret=3 and still pass. Asserting the code is what makes the collapse test
    // real on the product side rather than a threshold on the probe's own side.
    Chk("EnumerateServices does not report the fatal Win32-empty state",
        nRet != 3 && nRet != 2, sz);
    // And the ratio must stay plausible: Win32 drivers are a minority. If the type
    // filter broke, every entry would look like one class.
    Chk("service types are not all collapsed into one class",
        nWin32 > 0 && nWin32 < (int)vSvc.size(), sz);

    if (bInject)
    {
        Chk("injected-must-fail", false, "self-test: forced failure");
    }

    ::printf("\nresult: %d pass / %d fail\n", g_nPass, g_nFail);
    return (g_nFail == 0) ? 0 : 1;
}
