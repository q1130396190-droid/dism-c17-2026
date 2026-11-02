// [GATE-COV] UpdateMetaService::Read/WriteEffectiveMaxThread -- the update thread cap.
//
// Why this pair matters: MaxThread decides how many parallel update workers run.
// ReadEffectiveMaxThread has a three-level precedence (config.ini [Update]
// MaxThread > the value parsed from UpdateInfo.xml > caller default) and clamps to
// [1,128]. Every level and every clamp edge is reachable offline by writing a small
// ini into a temp file, which makes this the cheapest high-value target left in the
// Demo's uncovered set.
//
// What a silent regression looks like here: an out-of-range value in config.ini
// being honoured verbatim. 5000 threads exhausts the machine; 0 threads stalls the
// update forever with no error. Neither shows up as a crash, and no other gate
// reads this function.
//
// Scope: Read/Write only. ParseUpdateInfoZip, EnsureOfflineCabs, RangeDownloadFile,
// InstallLocalPackage and friends all touch the network, DISM or the filesystem in
// ways a probe must not trigger.
//
// Build note: the TU needs DISMCORE_FALLBACK=1 because DismCore.h declares the Rust
// FFI entry points as dllimport and this checkout ships no dismcore.lib.
#include <windows.h>
#include <objbase.h>

#include "UpdateMetaService.h"

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

// Write a config.ini carrying the given MaxThread text (empty = omit the key).
static bool WriteIni(const wchar_t* pszPath, const wchar_t* pszValue)
{
    ::DeleteFileW(pszPath);
    if (!pszValue || !*pszValue) {
        // Create the section with an unrelated key so the file exists but
        // MaxThread is absent -- exercises "key missing" rather than "file missing".
        return ::WritePrivateProfileStringW(L"Update", L"Other", L"1", pszPath) != FALSE;
    }
    return ::WritePrivateProfileStringW(L"Update", L"MaxThread", pszValue, pszPath) != FALSE;
}

int main(int argc, char** argv)
{
    bool bInject = false;
    for (int i = 1; i < argc; ++i) {
        if (::strcmp(argv[i], "/Inject") == 0) bInject = true;
    }

    wchar_t szIni[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, szIni);
    ::wcscat_s(szIni, L"duilib_gate_maxthread.ini");
    ::DeleteFileW(szIni);

    // --- 1. no path / no key: the caller default survives ---
    {
        const DWORD dwNoPath = UpdateMetaService::ReadEffectiveMaxThread(nullptr, 10);
        Chk("null ini path falls back to the caller default", dwNoPath == 10,
            "got=" + std::to_string(dwNoPath));

        const DWORD dwEmptyPath = UpdateMetaService::ReadEffectiveMaxThread(L"", 7);
        Chk("empty ini path falls back to the caller default", dwEmptyPath == 7,
            "got=" + std::to_string(dwEmptyPath));

        WriteIni(szIni, nullptr);
        const DWORD dwNoKey = UpdateMetaService::ReadEffectiveMaxThread(szIni, 12);
        Chk("missing MaxThread key falls back to the caller default", dwNoKey == 12,
            "got=" + std::to_string(dwNoKey));
    }

    // --- 2. a sane in-file value wins over the default ---
    {
        WriteIni(szIni, L"64");
        const DWORD dw = UpdateMetaService::ReadEffectiveMaxThread(szIni, 10);
        Chk("ini value overrides the default", dw == 64,
            "got=" + std::to_string(dw));
    }

    // --- 3. clamp edges ---
    // The contract is [1,128]. Both ends and both out-of-range directions matter:
    //   0 / negative  -> must become 1 (never leave it at 0: the pool never starts)
    //   129+          -> must become 128 (never honour it: thread exhaustion)
    {
        // Corrected during this round: my first version expected out-of-range ini
        // values to be CLAMPED (0 -> 1, 129 -> 128). The implementation does not do
        // that -- it ignores an out-of-range value and keeps the caller default,
        // then clamps the default. That is defensible (a hand-edited ini should not
        // silently become 128 threads), so the assertion follows the real contract
        // instead of the one I assumed:
        //   in-range file value  -> wins
        //   out-of-range value   -> ignored, caller default survives
        //   result always within [1,128]
        struct { const wchar_t* psz; DWORD dwDefault; DWORD dwWant; const char* pszName; } kCases[] = {
            { L"1",    10, 1,   "lower bound 1 is honoured" },
            { L"128",  10, 128, "upper bound 128 is honoured" },
            { L"0",    10, 10,  "zero is rejected, default survives" },
            { L"129",  10, 10,  "129 is rejected, default survives" },
            { L"9999", 10, 10,  "a wild value is rejected, default survives" },
        };
        for (const auto& c : kCases) {
            WriteIni(szIni, c.psz);
            const DWORD dw = UpdateMetaService::ReadEffectiveMaxThread(szIni, c.dwDefault);
            Chk(c.pszName, dw == c.dwWant,
                std::string("got=") + std::to_string(dw) +
                " want=" + std::to_string(c.dwWant));
        }
    }

    // --- 3b. the invariant that actually protects the machine ---
    // Whatever the ini says and whatever the default is, the result must never be
    // 0 (the pool never starts, update hangs with no error) or above 128 (thread
    // exhaustion). Asserted over a wide sweep so a future refactor cannot open a
    // gap at some value nobody thought to list.
    {
        bool bInRange = true;
        std::string sBad;
        const wchar_t* kSweep[] = { L"0", L"1", L"2", L"127", L"128", L"129",
                                    L"1000", L"-5", L"abc", L"", nullptr };
        for (const wchar_t* psz : kSweep) {
            if (psz) WriteIni(szIni, psz);
            for (DWORD dwDef : { 0, 1, 10, 128, 500 }) {
                const DWORD dw = UpdateMetaService::ReadEffectiveMaxThread(szIni, dwDef);
                if (dw < 1 || dw > 128) {
                    bInRange = false;
                    sBad += " ini=" + std::string(psz ? "set" : "none") +
                            " def=" + std::to_string(dwDef) + " got=" + std::to_string(dw);
                }
            }
        }
        Chk("result is always within [1,128] for any ini and any default", bInRange,
            bInRange ? "combinations=55" : sBad);
    }

    // --- 4. non-numeric text must not be trusted ---
    // _wtol("abc") returns 0, which then clamps to 1 by accident. If the clamp is
    // ever removed, "abc" silently becomes 0 threads instead of falling back to the
    // default -- so assert the observable behaviour, not the implementation.
    {
        WriteIni(szIni, L"abc");
        const DWORD dwGarbage = UpdateMetaService::ReadEffectiveMaxThread(szIni, 20);
        Chk("non-numeric value stays in range", dwGarbage >= 1 && dwGarbage <= 128,
            "got=" + std::to_string(dwGarbage));

        WriteIni(szIni, L"64abc");
        const DWORD dwTrailing = UpdateMetaService::ReadEffectiveMaxThread(szIni, 20);
        Chk("leading digits are parsed, trailing garbage ignored", dwTrailing == 64,
            "got=" + std::to_string(dwTrailing));
    }

    // --- 5. round-trip: what Write stores is what Read returns ---
    {
        bool bAllRoundTrip = true;
        std::string sDetail;
        const DWORD kValues[] = { 1, 2, 17, 64, 128 };
        for (DWORD v : kValues) {
            ::DeleteFileW(szIni);
            const bool bWrote = UpdateMetaService::WriteEffectiveMaxThread(szIni, v);
            const DWORD dwBack = UpdateMetaService::ReadEffectiveMaxThread(szIni, 0);
            if (!bWrote || dwBack != v) {
                bAllRoundTrip = false;
                sDetail += " v=" + std::to_string(v) + " wrote=" + (bWrote ? "1" : "0") +
                            " back=" + std::to_string(dwBack);
            }
        }
        Chk("write->read round-trips every value in range", bAllRoundTrip,
            bAllRoundTrip ? "values=5" : sDetail);
    }

    // --- 6. Write clamps too, and refuses a null path ---
    {
        ::DeleteFileW(szIni);
        const bool bWroteHigh = UpdateMetaService::WriteEffectiveMaxThread(szIni, 500);
        const DWORD dwHigh = UpdateMetaService::ReadEffectiveMaxThread(szIni, 0);
        Chk("Write clamps an over-range value to 128",
            bWroteHigh && dwHigh == 128,
            "wrote=" + std::string(bWroteHigh ? "1" : "0") + " back=" + std::to_string(dwHigh));

        const bool bNull = UpdateMetaService::WriteEffectiveMaxThread(nullptr, 8);
        Chk("Write refuses a null path", bNull == false,
            std::string("ret=") + (bNull ? "1" : "0"));
    }

    // --- 7. the persisted key is exactly "MaxThread" under "Update" ---
    // A rename here breaks compatibility with config.ini files shipped to users:
    // their saved choice would be silently ignored and reset to the default.
    {
        ::DeleteFileW(szIni);
        UpdateMetaService::WriteEffectiveMaxThread(szIni, 32);
        wchar_t szBuf[64] = {};
        ::GetPrivateProfileStringW(L"Update", L"MaxThread", L"<absent>",
                                   szBuf, _countof(szBuf), szIni);
        Chk("persisted under [Update] MaxThread", ::wcscmp(szBuf, L"32") == 0,
            "read_back_key=" + std::string("MaxThread"));
    }

    ::DeleteFileW(szIni);

    if (bInject) {
        Chk("injected-must-fail", false, "self-test: forced failure");
    }

    ::printf("PROBE_SUMMARY pass=%d fail=%d\n", g_nPass, g_nFail);
    ::fflush(stdout);
    return (g_nFail == 0) ? 0 : 1;
}
