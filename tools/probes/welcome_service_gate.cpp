// [GATE-COV] WelcomeService read surface.
//
// Scope, stated so the gate cannot drift:
//   * UserStateToString -- a pure mapping from QUERY_USER_NOTIFICATION_STATE to
//     display text. No OS call, no state. This is the cheapest real assertion in
//     the Demo: every branch is reachable by passing a constant, so a regression
//     (a swapped case label, a dropped branch) shows up immediately.
//   * GetSystemOverview -- reads OS name/build and the system drive. Read-only:
//     no thread, no registry write, no notification.
//
// Deliberately NOT called: GetUserNotificationState spawns/uses the shell query
// (fine, but it duplicates what UserStateToString already covers without the
// environmental dependency), and FetchOsNameAndBuild is reached through
// GetSystemOverview anyway.
//
// Why the string values matter: the page shows these directly to the user. A
// mistranslated or duplicated label is a real defect that no other gate catches,
// because every other gate only checks that *some* text was produced.
#include <windows.h>
#include <shellapi.h>

#include "WelcomeService.h"
#include "SystemInfoService.h"

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
    for (int i = 1; i < argc; ++i) {
        if (::strcmp(argv[i], "/Inject") == 0) bInject = true;
    }

    // --- 1. every QUERY_USER_NOTIFICATION_STATE maps to a distinct non-empty label ---
    // The set is fixed by the header; if a new state is added the count changes and
    // this list must be extended deliberately rather than silently falling into
    // the "unknown" branch.
    struct { int nState; const wchar_t* pszExpect; } kStates[] = {
        { QUNS_NOT_PRESENT,             L"\u7528\u6237\u4e0d\u5728\u573a" },
        { QUNS_BUSY,                    L"\u7528\u6237\u5fd9\u788c" },
        { QUNS_RUNNING_D3D_FULL_SCREEN, L"\u5168\u5c4f\u5e94\u7528\u8fd0\u884c\u4e2d" },
        { QUNS_PRESENTATION_MODE,       L"\u6f14\u793a\u6a21\u5f0f" },
        { QUNS_ACCEPTS_NOTIFICATIONS,   L"\u53ef\u63a5\u6536\u901a\u77e5" },
        { QUNS_QUIET_TIME,              L"\u5b89\u9759\u65f6\u6bb5" },
        { QUNS_APP,                     L"\u5e94\u7528\u6a21\u5f0f" },
    };

    bool bAllMatch = true;
    std::string sDetail;
    std::vector<std::wstring> vSeen;
    for (const auto& e : kStates) {
        DuiLib::CDuiString s = WelcomeService::UserStateToString(e.nState);
        const bool bMatch = (::wcscmp(s.GetData(), e.pszExpect) == 0);
        if (!bMatch) {
            bAllMatch = false;
            sDetail += " state=" + std::to_string(e.nState) + "->" + W2A(s.GetData());
        }
        vSeen.push_back(std::wstring(s.GetData()));
    }
    Chk("all 7 notification states map to their documented label",
        bAllMatch, bAllMatch ? "states=7" : sDetail);

    // Distinctness: two states mapping to the same text means the user cannot
    // tell why notifications are suppressed. Assert pairwise inequality.
    {
        bool bDistinct = true;
        int nDup = 0;
        for (size_t i = 0; i < vSeen.size(); ++i) {
            for (size_t j = i + 1; j < vSeen.size(); ++j) {
                if (vSeen[i] == vSeen[j]) { bDistinct = false; ++nDup; }
            }
        }
        Chk("the 7 labels are pairwise distinct",
            bDistinct,
            "pairs_compared=" + std::to_string(vSeen.size() * (vSeen.size() - 1) / 2) +
            " duplicates=" + std::to_string(nDup));
    }

    // --- 2. an out-of-range state must be reported, not silently blank ---
    // If the fallback returned an empty string the UI would show a blank row with
    // no hint that the value was unrecognised -- the worst possible outcome for a
    // diagnostic page.
    {
        DuiLib::CDuiString s = WelcomeService::UserStateToString(9999);
        const bool bNonEmpty = !s.IsEmpty();
        const bool bCarriesValue = (::wcsstr(s.GetData(), L"9999") != nullptr);
        Chk("unknown state falls back to a labelled value, not blank",
            bNonEmpty && bCarriesValue,
            "len=" + std::to_string(s.GetLength()) + " text=" + W2A(s.GetData()));
    }

    // --- 3. GetSystemOverview is read-only and self-consistent ---
    {
        WelcomeOverview ov = {};
        DWORD dwErr = 0;
        const bool bOk = WelcomeService::GetSystemOverview(ov, dwErr);
        Chk("GetSystemOverview succeeds",
            bOk, std::string("ok=") + (bOk ? "1" : "0") + " err=" + std::to_string(dwErr));
        Chk("overview carries an OS name and a build number",
            !ov.sOsName.IsEmpty() && ov.dwBuild != 0,
            "os_len=" + std::to_string(ov.sOsName.GetLength()) +
            " build=" + std::to_string(ov.dwBuild));
        // The overview also reports the system drive. It must agree with the
        // dedicated accessor -- two code paths reading the same fact and
        // disagreeing is a defect the UI would render as a contradiction.
        {
            DuiLib::CDuiString sDirect = SystemInfoService::GetSystemDriveStr();
            const bool bAgree = (::wcscmp(ov.sSystemDrive.GetData(), sDirect.GetData()) == 0);
            Chk("overview system drive agrees with the direct accessor",
                bAgree,
                "overview=" + W2A(ov.sSystemDrive.GetData()) +
                " direct=" + W2A(sDirect.GetData()));
        }
    }

    // --- 4. system drive is exactly "X:" and X is a real drive ---
    {
        DuiLib::CDuiString sDrive = SystemInfoService::GetSystemDriveStr();
        const wchar_t* psz = sDrive.GetData();
        const bool bShape = (psz && psz[0] && psz[1] == L':' && psz[2] == 0);
        const bool bExists = bShape &&
            (::GetDriveTypeW(std::wstring(psz, 2).c_str()) != DRIVE_NO_ROOT_DIR);
        Chk("system drive is well-formed and present",
            bShape && bExists,
            "drive=" + W2A(psz));
    }

    if (bInject) {
        Chk("injected-must-fail", false, "self-test: forced failure");
    }

    ::printf("PROBE_SUMMARY pass=%d fail=%d\n", g_nPass, g_nFail);
    ::fflush(stdout);
    return (g_nFail == 0) ? 0 : 1;
}
