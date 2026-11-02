// [GATE-COV] AssocService::SetUserFileAssociation -- EFFECT-level assertions.
//
// Why: round 52 measured effect-level assertions at 4.2% of the suite and named the
// association writer as the next gap. The existing assoc gate only exercised
// QueryProgIdAndCommand (read path); nothing ever called the writer, so a regression
// that stopped writing would have been invisible.
//
// Safety, in order of importance:
//   * HKCU only. SetUserFileAssociation writes
//     HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\FileExts\<ext>\UserChoice.
//     No HKLM, no elevation, no system-wide effect.
//   * A synthetic extension (".duilibprobe9f3a") that cannot collide with a real file
//     type on this machine. Nothing on disk is associated with it, so even if the
//     write succeeded and was never cleaned up it would not change how any real file
//     opens.
//   * The extension is deleted in every exit path, and the probe asserts the key is
//     gone at the end. A gate that leaves registry state behind is worse than no gate.
//
// What is asserted: the writer's own return code is NOT enough (it already calls
// VerifyUserFileAssociation internally, so a broken verify would make both sides
// agree while nothing was written). The probe re-reads the key with its own registry
// call and checks the value it finds.
#include <windows.h>

#include "AssocService.h"

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

// Independent read of UserChoice\ProgId -- deliberately NOT the service's own
// Verify function, so a broken verify cannot hide a broken write.
static bool ReadUserChoice(const wchar_t* pszExt, std::wstring& sOut)
{
    sOut.clear();
    wchar_t szPath[512] = {};
    ::swprintf_s(szPath,
                 L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\%s\\UserChoice",
                 pszExt);
    HKEY hKey = nullptr;
    const LONG l = ::RegOpenKeyExW(HKEY_CURRENT_USER, szPath, 0, KEY_READ | KEY_WOW64_64KEY, &hKey);
    if (l != ERROR_SUCCESS) return false;
    wchar_t szBuf[512] = {};
    DWORD dwSize = sizeof(szBuf) - sizeof(wchar_t);
    DWORD dwType = 0;
    const LONG lr = ::RegQueryValueExW(hKey, L"ProgId", nullptr, &dwType,
                                       reinterpret_cast<LPBYTE>(szBuf), &dwSize);
    ::RegCloseKey(hKey);
    if (lr != ERROR_SUCCESS || dwType != REG_SZ) return false;
    szBuf[(sizeof(szBuf) / sizeof(wchar_t)) - 1] = L'\0';
    sOut = szBuf;
    return true;
}

static bool KeyExists(const wchar_t* pszExt)
{
    wchar_t szPath[512] = {};
    ::swprintf_s(szPath,
                 L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\%s\\UserChoice",
                 pszExt);
    HKEY hKey = nullptr;
    const LONG l = ::RegOpenKeyExW(HKEY_CURRENT_USER, szPath, 0, KEY_READ | KEY_WOW64_64KEY, &hKey);
    if (hKey) ::RegCloseKey(hKey);
    return l == ERROR_SUCCESS;
}

int main(int argc, char** argv)
{
    bool bInject = false;
    for (int i = 1; i < argc; ++i) {
        if (::strcmp(argv[i], "/Inject") == 0) bInject = true;
    }

    // Synthetic extension: cannot collide with a real type, so the probe is safe even
    // if cleanup fails.
    const wchar_t* kExt = L".duilibprobe9f3a";
    // A ProgId needs a backslash to pass the writer's validity check.
    const wchar_t* kProgId = L"DuiLibProbe9f3a.ProbeDocument";

    // --- 0. start from a clean key, and remember whether we had to clean one up ---
    // If a previous run crashed, the key may still be there. Deleting it is correct
    // (it is ours), but record it: a key that keeps reappearing means cleanup is
    // broken somewhere, and that should be visible rather than silently absorbed.
    {
        const bool bPreexisting = KeyExists(kExt);
        AssocService::ClearUserFileAssociation(DuiLib::CDuiString(kExt));
        Chk("synthetic extension key is absent before the test",
            !KeyExists(kExt),
            std::string("had_leftover=") + (bPreexisting ? "1" : "0"));
    }

    // --- 1. the write must actually land in the registry ---
    // Verified by an independent read, not by the service's return code.
    {
        DuiLib::CDuiString sExt(kExt);
        DuiLib::CDuiString sProg(kProgId);
        const EAssocWriteResult r = AssocService::SetUserFileAssociation(sExt, sProg);

        std::wstring sRead;
        const bool bReadable = ReadUserChoice(kExt, sRead);
        char szDetail[256] = {};
        ::sprintf_s(szDetail, "ret=%d readable=%d value=%ls",
                    (int)r, bReadable ? 1 : 0, bReadable ? sRead.c_str() : L"<none>");

        Chk("write reports success", r == EAssocWriteResult::ASSOC_WRITE_OK, szDetail);
        Chk("the written ProgId is readable from HKCU", bReadable, szDetail);
        Chk("the stored value is exactly what was written",
            bReadable && sRead == kProgId, szDetail);
    }

    // --- 2. writing a different ProgId replaces the value (not appends a second) ---
    {
        const wchar_t* kProgId2 = L"DuiLibProbe9f3a.SecondDocument";
        DuiLib::CDuiString sExt(kExt);
        DuiLib::CDuiString sProg(kProgId2);
        AssocService::SetUserFileAssociation(sExt, sProg);

        std::wstring sRead;
        const bool bOk = ReadUserChoice(kExt, sRead) && sRead == kProgId2;
        Chk("a second write replaces the stored ProgId", bOk,
            bOk ? "value=SecondDocument" : "value=<stale or missing>");
    }

    // --- 3. invalid input must be refused AND must not write anything ---
    // An empty ProgId has to be rejected before RegSetValueEx, otherwise the key
    // would exist with no value and Verify would later report "not associated" --
    // a false negative that looks like the user simply has no preference.
    {
        AssocService::ClearUserFileAssociation(DuiLib::CDuiString(kExt));
        DuiLib::CDuiString sExt(kExt);
        DuiLib::CDuiString sEmpty;
        const EAssocWriteResult r = AssocService::SetUserFileAssociation(sExt, sEmpty);

        const bool bRefused = (r == EAssocWriteResult::ASSOC_WRITE_INVALID_PROGID);
        const bool bNoKey = !KeyExists(kExt);
        Chk("an empty ProgId is refused and writes no key",
            bRefused && bNoKey,
            std::string("ret=") + std::to_string((int)r) +
            " key=" + (bNoKey ? "0" : "1"));

        // A ProgId without a backslash is also invalid by the writer's contract.
        DuiLib::CDuiString sBad(L"DuiLibProbe9f3aNoDot");
        const EAssocWriteResult r2 = AssocService::SetUserFileAssociation(sExt, sBad);
        Chk("a ProgId without a backslash is refused",
            r2 == EAssocWriteResult::ASSOC_WRITE_INVALID_PROGID && !KeyExists(kExt),
            std::string("ret=") + std::to_string((int)r2) +
            " key=" + (KeyExists(kExt) ? "1" : "0"));
    }

    // --- 4. clear must actually remove the key ---
    // The delete path returns OK for "not found" too, so success alone proves
    // nothing; the key's absence has to be checked out-of-band.
    {
        DuiLib::CDuiString sExt(kExt);
        DuiLib::CDuiString sProg(kProgId);
        AssocService::SetUserFileAssociation(sExt, sProg);
        const bool bExisted = KeyExists(kExt);

        const EAssocWriteResult r = AssocService::ClearUserFileAssociation(sExt);
        const bool bGone = !KeyExists(kExt);
        Chk("clear removes the key it just created",
            bExisted && r == EAssocWriteResult::ASSOC_WRITE_OK && bGone,
            std::string("existed=") + (bExisted ? "1" : "0") +
            " ret=" + std::to_string((int)r) + " gone=" + (bGone ? "1" : "0"));
    }

    // --- 5. clearing a key that does not exist still reports success ---
    // Idempotency matters: the uninstall path calls clear unconditionally, and a
    // non-zero result there would surface as a spurious "association cleanup failed".
    {
        AssocService::ClearUserFileAssociation(DuiLib::CDuiString(kExt));
        const EAssocWriteResult r = AssocService::ClearUserFileAssociation(DuiLib::CDuiString(kExt));
        Chk("clearing an absent key is idempotent",
            r == EAssocWriteResult::ASSOC_WRITE_OK,
            std::string("ret=") + std::to_string((int)r));
    }

    // --- 6. Verify agrees with an independent read ---
    // VerifyUserFileAssociation is what the UI trusts. If it disagreed with the
    // registry, the page would show "associated" for a ProgId that is not there.
    {
        DuiLib::CDuiString sExt(kExt);
        DuiLib::CDuiString sProg(kProgId);
        AssocService::SetUserFileAssociation(sExt, sProg);

        std::wstring sRead;
        const bool bReadable = ReadUserChoice(kExt, sRead);
        const bool bVerify = AssocService::VerifyUserFileAssociation(sExt, sProg);
        Chk("Verify agrees with the independent registry read",
            bVerify && bReadable && sRead == kProgId,
            std::string("verify=") + (bVerify ? "1" : "0") +
            " read=" + (bReadable ? "1" : "0"));

        // And Verify must say no for a ProgId that was never written.
        DuiLib::CDuiString sOther(L"DuiLibProbe9f3a.NeverWritten");
        Chk("Verify rejects a ProgId that was not written",
            !AssocService::VerifyUserFileAssociation(sExt, sOther),
            "expected=0");
    }

    // --- cleanup: no registry state may survive this run ---
    AssocService::ClearUserFileAssociation(DuiLib::CDuiString(kExt));
    {
        const bool bLeft = KeyExists(kExt);
        Chk("no probe registry key survives the run", !bLeft,
            std::string("leftover=") + (bLeft ? "1" : "0"));
    }

    if (bInject) {
        Chk("injected-must-fail", false, "self-test: forced failure");
    }

    ::printf("PROBE_SUMMARY pass=%d fail=%d\n", g_nPass, g_nFail);
    ::fflush(stdout);
    return (g_nFail == 0) ? 0 : 1;
}
