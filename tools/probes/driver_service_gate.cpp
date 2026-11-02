// [GATE-COV] DriverService read path -- no install, no delete.
//
// Why this exists: the coverage audit found DriverService had no assertion
// anywhere. Its write paths (DeleteDriver / RollbackDevice) change the machine's
// driver store, so a gate must never call them. What IS assertable is the
// enumeration the driver page is built from: if it silently returns empty, the
// page shows "no third-party drivers", which reads as a clean machine rather
// than a broken query.
//
// Environment dependency, handled explicitly rather than papered over: DISM may
// be unavailable (not elevated, DISM components missing, WinPE). When the bridge
// is not ready this gate prints a SKIP with the reason and exits 0 -- it must not
// fail (that would make the suite red on machines where the feature legitimately
// cannot work) and must not silently pass as if it had checked something.
#include "stdafx.h"
#include "DismApiBridge.h"       // namespace DismApiBridge::Init/IsReady
#include "DriverService.h"

#include <cstdio>
#include <map>
#include <string>

static int g_nPass = 0;
static int g_nFail = 0;
static int g_nSkip = 0;

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

    std::map<CDuiString, DriverPubInfo> mapPub;
    const bool bOk = DriverService::EnumPublished(mapPub);

    if (!bOk && mapPub.empty())
    {
        // Distinguish "DISM says there are no third-party drivers" from
        // "DISM is not usable here". Only the latter is a skip.
        DismApiBridge::Init();
        if (!DismApiBridge::IsReady())
        {
            ::printf("[SKIP] DISM bridge not ready (needs admin / DISM components);"
                     " driver enumeration cannot be asserted on this host\n");
            ::fflush(stdout);
            ++g_nSkip;
        }
        else
        {
            // Bridge is up, so an empty result is a real claim: a healthy Windows
            // install almost always has at least one third-party driver, and if it
            // truly has none the page must say so rather than show a broken list.
            char sz[128] = {};
            ::sprintf_s(sz, "bridge=ready entries=%d", (int)mapPub.size());
            Chk("enumeration returns data when DISM is ready", false, sz);
        }
    }
    else
    {
        // ---- every entry must be self-consistent -----------------------
        int nBadOem = 0;
        int nBadOriginal = 0;
        int nDupLower = 0;
        for (auto it = mapPub.begin(); it != mapPub.end(); ++it)
        {
            const DriverPubInfo& d = it->second;
            if (d.sOem.IsEmpty()) ++nBadOem;
            if (d.sOriginal.IsEmpty()) ++nBadOriginal;

            // The map is keyed by lower-cased oem name; a key that does not match
            // its own entry means lookups by name would miss.
            CDuiString sKey(d.sOem);
            sKey.MakeLower();
            if (sKey.Compare(it->first) != 0) ++nDupLower;
        }

        char sz[160] = {};
        ::sprintf_s(sz, "entries=%d badOem=%d badOriginal=%d keyMismatch=%d",
                    (int)mapPub.size(), nBadOem, nBadOriginal, nDupLower);
        Chk("enumeration produced entries", !mapPub.empty(), sz);
        Chk("every entry has an oem name", nBadOem == 0, sz);
        Chk("every entry has an original name", nBadOriginal == 0, sz);
        Chk("map key equals lower-cased oem name", nDupLower == 0, sz);

        // Sample one entry so a regression in field mapping is visible.
        if (!mapPub.empty())
        {
            const DriverPubInfo& d = mapPub.begin()->second;
            ::printf("[INFO] sample: oem=%s provider=%s version=%s\n",
                     W2A(d.sOem.GetData()).c_str(),
                     W2A(d.sProvider.GetData()).c_str(),
                     W2A(d.sVersion.GetData()).c_str());
            ::fflush(stdout);
        }
    }

    if (bInject)
    {
        Chk("injected-must-fail", false, "self-test: forced failure");
    }

    ::printf("\nresult: %d pass / %d fail / %d skip\n", g_nPass, g_nFail, g_nSkip);
    return (g_nFail == 0) ? 0 : 1;
}
