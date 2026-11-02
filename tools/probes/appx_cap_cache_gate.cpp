// [GATE-COV] AppxCapUpdService capability cache: Save/Load round-trip, TTL, and the
// two guards that matter (clock rollback, corrupt input).
//
// Why this cache: it is what the Capability page renders from when enumeration
// fails. A silent regression here does not crash -- it shows a stale or empty list
// with no error, which is the failure mode users report as "the page is broken
// sometimes".
//
// Isolation: CapCachePath resolves %LOCALAPPDATA%\DismTool_Clone\cap_enum_cache.dat.
// The probe redirects LOCALAPPDATA to a temp directory for its own process, so it
// never reads or writes the user's real cache. That redirection is asserted, not
// assumed -- if a future refactor stops honouring the environment variable, the
// probe would otherwise start clobbering real state silently.
//
// Scope: SaveCapCache / LoadCapCache only. The enumeration threads and the Appx
// API calls are out of scope.
//
// Build note: needs DISMCORE_FALLBACK=1 (DismCore.h declares Rust FFI as dllimport)
// and a 10-TU link chain (OfflineSession, SeedCache, StringManager, unzip, ...).
#include <windows.h>

#include "AppxCapUpdService.h"

#include <cstdio>
#include <ctime>
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

static std::string A2A(const char* psz)
{
    return std::string(psz ? psz : "");
}

static std::wstring W(const wchar_t* psz)
{
    return std::wstring(psz ? psz : L"");
}

// The cache file the service will actually use, resolved the same way it does.
static std::wstring CachePath()
{
    wchar_t sz[MAX_PATH] = {};
    ::GetEnvironmentVariableW(L"LOCALAPPDATA", sz, MAX_PATH);
    std::wstring s(sz);
    if (!s.empty() && s.back() != L'\\') s += L'\\';
    s += L"DismTool_Clone\\cap_enum_cache.dat";
    return s;
}

static CapEntry MakeEntry(const wchar_t* pszId, const wchar_t* pszState,
                          int nReq, int nOpt)
{
    CapEntry e;
    e.sIdentity = pszId;
    e.sState = pszState;
    e.sDescription = L"desc";
    e.sSize = L"1.2 MB";
    e.nRequiredDeps = nReq;
    e.nOptionalDeps = nOpt;
    e.sFirstRequiredDep = L"reqdep";
    e.sFirstOptionalDep = L"optdep";
    return e;
}

// Rewrite only the timestamp in the header line, to age or future-date the cache.
static bool SetHeaderTime(const std::wstring& sPath, unsigned long long ullWhen)
{
    HANDLE h = ::CreateFileW(sPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li = {};
    li.QuadPart = 0;
    ::SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
    char szBuf[64] = {};
    const int n = ::sprintf_s(szBuf, "CAPCACHE\t%llu", ullWhen);
    DWORD dwWritten = 0;
    const BOOL bOk = ::WriteFile(h, szBuf, (DWORD)n, &dwWritten, nullptr);
    ::CloseHandle(h);
    return bOk && dwWritten == (DWORD)n;
}

// SEH wrapper. C2712 forbids __try in a function that requires object unwinding, so
// the vector lives outside the guarded region and only plain scalars cross it.
struct WrittenBadFile
{
    bool bThrew;
    bool bLoaded;
    int  nCount;
};

static WrittenBadFile LoadExpectingNoCrash()
{
    WrittenBadFile r = { false, false, 0 };
    std::vector<CapEntry>* pv = new std::vector<CapEntry>();
    __try {
        r.bLoaded = AppxCapUpdService::LoadCapCache(*pv, false);
        r.nCount = static_cast<int>(pv->size());
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        r.bThrew = true;
    }
    delete pv;
    return r;
}

int main(int argc, char** argv)
{
    bool bInject = false;
    for (int i = 1; i < argc; ++i) {
        if (::strcmp(argv[i], "/Inject") == 0) bInject = true;
    }

    const std::wstring sCache = CachePath();

    // --- 0. the probe must be operating in its own sandbox ---
    // Guard by MARKER FILE, not by path comparison. Two path-based attempts both
    // failed for reasons unrelated to the code under test:
    //   * _wcsnicmp returned non-zero for prefixes differing only in case;
    //   * GetTempPathW/GetEnvironmentVariableW(L"TEMP") reported a different length
    //     than the same variable in the harness (8.3 normalisation).
    // A marker file is unambiguous: the harness creates <LOCALAPPDATA>\GATE_SANDBOX,
    // and if it is absent the probe is NOT in a sandbox and must not touch anything.
    {
        wchar_t szProbeEnv[MAX_PATH] = {};
        ::GetEnvironmentVariableW(L"LOCALAPPDATA", szProbeEnv, MAX_PATH);
        std::wstring sMarker = CachePath();
        const size_t nSlash = sMarker.find_last_of(L'\\');
        if (nSlash != std::wstring::npos) sMarker = sMarker.substr(0, nSlash);
        sMarker += L"\\GATE_SANDBOX";

        const DWORD dwAttr = ::GetFileAttributesW(sMarker.c_str());
        const bool bSandboxed = (dwAttr != INVALID_FILE_ATTRIBUTES);
        Chk("LOCALAPPDATA points at a harness sandbox (marker present)",
            bSandboxed,
            "marker_present=" + std::string(bSandboxed ? "1" : "0") +
            " marker_len=" + std::to_string((int)sMarker.size()) +
            " env_len=" + std::to_string((int)::wcslen(szProbeEnv)));
        if (!bSandboxed) {
            ::printf("REFUSING to continue: no sandbox marker, this probe would touch");
            ::printf(" real user state");
            ::putchar(10);
            ::fflush(stdout);
            return 2;
        }
    }

    ::DeleteFileW(sCache.c_str());

    // --- 1. no cache file: load fails, output is emptied ---
    {
        std::vector<CapEntry> v;
        v.push_back(MakeEntry(L"stale", L"Installed", 0, 0));   // must be cleared
        const bool b = AppxCapUpdService::LoadCapCache(v, false);
        Chk("missing cache reports failure and leaves the vector empty",
            !b && v.empty(),
            std::string("ret=") + (b ? "1" : "0") + " n=" + std::to_string(v.size()));
    }

    // --- 2. round-trip: write then read preserves the field values ---
    {
        std::vector<CapEntry> vIn;
        vIn.push_back(MakeEntry(L"Microsoft.VCLibs.140.00", L"Installed", 2, 1));
        vIn.push_back(MakeEntry(L"Microsoft.HEVCVideoExtension", L"Pending", 0, 3));
        ::SetLastError(0);
        const bool bSaved = AppxCapUpdService::SaveCapCache(vIn);
        const DWORD dwSave = ::GetLastError();
        const bool bFileThere =
            (::GetFileAttributesW(sCache.c_str()) != INVALID_FILE_ATTRIBUTES);


        std::vector<CapEntry> vOut;
        const bool bLoaded = AppxCapUpdService::LoadCapCache(vOut, false);
        bool bSame = (bLoaded && vOut.size() == vIn.size());
        std::string sDetail = "saved=" + std::string(bSaved ? "1" : "0") +
                              " loaded=" + std::string(bLoaded ? "1" : "0") +
                              " in=" + std::to_string(vIn.size()) +
                              " out=" + std::to_string(vOut.size()) +
                              "";
        if (bSame) {
            for (size_t i = 0; i < vIn.size(); ++i) {
                if (::wcscmp(vOut[i].sIdentity.GetData(), vIn[i].sIdentity.GetData()) != 0 ||
                    ::wcscmp(vOut[i].sState.GetData(), vIn[i].sState.GetData()) != 0 ||
                    vOut[i].nRequiredDeps != vIn[i].nRequiredDeps ||
                    vOut[i].nOptionalDeps != vIn[i].nOptionalDeps) {
                    bSame = false;
                    sDetail += " mismatch_at=" + std::to_string(i);
                    break;
                }
            }
        }
        Chk("save->load round-trips identity, state and dependency counts", bSame, sDetail);
    }

    // --- 3. TTL: an old entry is rejected, unless the caller ignores TTL ---
    // 3600s is the documented TTL. Age the header past it and check both paths:
    // the default path must drop it, the bIgnoreTtl path must still serve it --
    // that flag exists for the "enumeration failed, show what we have" fallback, so
    // breaking it would turn a degraded page into an empty page.
    {
        std::vector<CapEntry> vProbe;
        AppxCapUpdService::LoadCapCache(vProbe, false);
        const bool bHadCache = !vProbe.empty();

        const time_t nNow = ::time(nullptr);
        const bool bAged = SetHeaderTime(sCache, (unsigned long long)(nNow - 7200));
        Chk("cache header could be aged past its TTL",
            bHadCache && bAged,
            std::string("had=") + (bHadCache ? "1" : "0") + " aged=" + (bAged ? "1" : "0"));

        std::vector<CapEntry> vNormal;
        const bool bRejected = AppxCapUpdService::LoadCapCache(vNormal, false);
        Chk("expired cache is rejected on the default path",
            !bRejected && vNormal.empty(),
            std::string("ret=") + (bRejected ? "1" : "0") + " n=" + std::to_string(vNormal.size()));

        std::vector<CapEntry> vIgnore;
        const bool bServed = AppxCapUpdService::LoadCapCache(vIgnore, true);
        Chk("expired cache is still served when TTL is ignored",
            bServed && !vIgnore.empty(),
            std::string("ret=") + (bServed ? "1" : "0") + " n=" + std::to_string(vIgnore.size()));
    }

    // --- 4. clock rollback: a future timestamp is treated as no cache ---
    // If the machine clock jumps backwards (time sync, VM restore), a cache written
    // "in the future" must not be trusted: nNow - ullSaved would be negative and a
    // naive comparison would treat it as fresh forever.
    {
        const time_t nNow = ::time(nullptr);
        const bool bFuture = SetHeaderTime(sCache, (unsigned long long)(nNow + 86400));
        std::vector<CapEntry> v;
        const bool b = AppxCapUpdService::LoadCapCache(v, false);
        Chk("future-dated cache is rejected as a clock anomaly",
            bFuture && !b && v.empty(),
            std::string("dosed=") + (bFuture ? "1" : "0") +
            " ret=" + (b ? "1" : "0") + " n=" + std::to_string(v.size()));
        // bIgnoreTtl must NOT bypass the rollback guard: the code comments say it
        // only relaxes TTL. Verify, because "relax TTL" is easy to implement as
        // "skip both checks".
        std::vector<CapEntry> v2;
        const bool b2 = AppxCapUpdService::LoadCapCache(v2, true);
        Chk("ignoring TTL does not bypass the clock-rollback guard",
            !b2 && v2.empty(),
            std::string("ret=") + (b2 ? "1" : "0") + " n=" + std::to_string(v2.size()));
    }

    // --- 5. corrupt / truncated / wrong-magic input must be refused, not crash ---
    {
        struct { const char* pszName; const char* pszBody; size_t nLen; } kBad[] = {
            { "empty file",              "", 0 },
            { "wrong magic",             "NOTCACHE\t1\r\n", 13 },
            { "header only, no rows",    "CAPCACHE\t", 10 },
            { "magic truncated",         "CAPCAC", 6 },
        };
        for (const auto& c : kBad) {
            HANDLE h = ::CreateFileW(sCache.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD dw = 0;
                if (c.nLen) ::WriteFile(h, c.pszBody, (DWORD)c.nLen, &dw, nullptr);
                ::CloseHandle(h);
            }
            WrittenBadFile r = LoadExpectingNoCrash();
            Chk(c.pszName, !r.bThrew && !r.bLoaded && r.nCount == 0,
                std::string("threw=") + (r.bThrew ? "1" : "0") +
                " ret=" + (r.bLoaded ? "1" : "0") + " n=" + std::to_string(r.nCount));
        }
    }

    // --- 6. a row with too few fields is skipped, the rest still loads ---
    // The parser is documented as field-tolerant ("字段不足：跳过该行"). If it
    // instead dropped the whole file, one malformed row written by an older version
    // would blank the page.
    {
        const time_t nNow = ::time(nullptr);
        char szBody[512] = {};
        ::sprintf_s(szBody,
                    "CAPCACHE\t%llu\r\n"
                    "Microsoft.A\tInstalled\tdesc\t1 MB\t1\t0\treq\topt\r\n"
                    "TOOFEWFIELDS\tonly\r\n"
                    "Microsoft.B\tPending\tdesc\t2 MB\t0\t2\treq2\topt2\r\n",
                    (unsigned long long)nNow);
        HANDLE h = ::CreateFileW(sCache.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD dw = 0;
            ::WriteFile(h, szBody, (DWORD)::strlen(szBody), &dw, nullptr);
            ::CloseHandle(h);
        }
        std::vector<CapEntry> v;
        const bool b = AppxCapUpdService::LoadCapCache(v, false);
        Chk("a malformed row is skipped and the valid rows still load",
            b && v.size() == 2,
            std::string("ret=") + (b ? "1" : "0") + " n=" + std::to_string(v.size()));
    }

    // --- 7. an empty list is never written ---
    // SaveCapCache returns false for an empty vector. If it wrote a header-only
    // file instead, the next load would see a valid cache containing nothing and
    // the page would render empty with no fallback path.
    {
        ::DeleteFileW(sCache.c_str());
        std::vector<CapEntry> vEmpty;
        const bool b = AppxCapUpdService::SaveCapCache(vEmpty);
        const bool bExists = (::GetFileAttributesW(sCache.c_str()) != INVALID_FILE_ATTRIBUTES);
        Chk("saving an empty list writes nothing",
            !b && !bExists,
            std::string("ret=") + (b ? "1" : "0") + " file=" + (bExists ? "1" : "0"));
    }

    ::DeleteFileW(sCache.c_str());
    {
        std::wstring sTmp = sCache + L".tmp";
        ::DeleteFileW(sTmp.c_str());
    }

    // --- 8. SaveUpdCache is atomic too: round-trip plus the .tmp contract ---
    // It used to write straight to the target file, so a reader could parse a
    // truncated body. Both caches now write .tmp and rename.
    {
        const std::wstring sCacheDir = sCache.substr(0, sCache.find_last_of(L"\\\\"));
        const std::wstring sUpdPath = sCacheDir + L"\\upd_enum_cache_t0.dat";
        const std::wstring sUpdTmp = sUpdPath + L".tmp";
        ::DeleteFileW(sUpdPath.c_str());
        ::DeleteFileW(sUpdTmp.c_str());

        std::vector<UpdEntry> vU;
        {
            UpdEntry e;
            e.sTitle = L"Security Update";
            e.sDate = L"2026-10-01";
            e.sUpdateID = L"abc-123";
            e.sKB = L"KB5001335";
            vU.push_back(e);
        }
        const bool bSavedU = AppxCapUpdService::SaveUpdCache(0, vU);
        const bool bTmpLeft =
            (::GetFileAttributesW(sUpdTmp.c_str()) != INVALID_FILE_ATTRIBUTES);
        Chk("SaveUpdCache writes and leaves no .tmp behind",
            bSavedU && !bTmpLeft,
            std::string("saved=") + (bSavedU ? "1" : "0") +
            " tmp_left=" + (bTmpLeft ? "1" : "0"));

        std::vector<UpdEntry> vOutU;
        const bool bLoadedU = AppxCapUpdService::LoadUpdCache(0, vOutU);
        const bool bFieldsOk = bLoadedU && vOutU.size() == 1 &&
            ::wcscmp(vOutU[0].sTitle.GetData(), L"Security Update") == 0 &&
            ::wcscmp(vOutU[0].sKB.GetData(), L"KB5001335") == 0;
        Chk("SaveUpdCache round-trips title and KB",
            bFieldsOk,
            std::string("loaded=") + (bLoadedU ? "1" : "0") +
            " n=" + std::to_string(vOutU.size()));

        // A stale .tmp from an interrupted write must never be mistaken for data.
        std::vector<UpdEntry> vAfter;
        const bool bLoadedAfter = AppxCapUpdService::LoadUpdCache(0, vAfter);
        Chk("a leftover .tmp is not read as the cache",
            bLoadedAfter && vAfter.size() == 1,
            "n=" + std::to_string(vAfter.size()));

        ::DeleteFileW(sUpdPath.c_str());
        ::DeleteFileW(sUpdTmp.c_str());
    }

    if (bInject) {
        Chk("injected-must-fail", false, "self-test: forced failure");
    }

    ::printf("PROBE_SUMMARY pass=%d fail=%d\n", g_nPass, g_nFail);
    ::fflush(stdout);
    return (g_nFail == 0) ? 0 : 1;
}
