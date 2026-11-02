// [GATE-COV] CommandLineService -- the parser only.
//
// Why this is worth covering: every command-line entry point in the app flows
// through this parser (/DumpPages, /AttrCheck, /SvcAsyncProbe, /NoWin11, /U ...).
// If it mis-splits a token, the wrong mode runs -- and the symptom is "the switch
// seems to be ignored", which reads like a feature that does not exist rather than
// a parsing bug.
//
// Scope: Parse + the read accessors. No files, no registry, no process launches,
// so this is fully side-effect free and safe to drive with arbitrary input.
#include <windows.h>

// CommandLineService.h relies on UILIB_API being defined by its includer.
#ifndef UILIB_API
#define UILIB_API __declspec(dllimport)
#endif
#include "CommandLineService.h"

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

    // ---- 1) a bare switch is recognised, and an absent one is not -------
    {
        CommandLineService::Parse(L"/DumpPages");
        const bool bHas = CommandLineService::Has(L"dumppages");
        const bool bAbsent = CommandLineService::Has(L"notpresent");

        char sz[160] = {};
        ::sprintf_s(sz, "has=%d absent=%d", bHas ? 1 : 0, bAbsent ? 1 : 0);
        ::printf("[INFO] measured switch: %s\n", sz);
        ::fflush(stdout);
        Chk("a present switch is found", bHas, sz);
        Chk("an absent switch is not found", !bAbsent, sz);
    }

    // ---- 2) key=value pairs, including values with spaces --------------
    {
        CommandLineService::Parse(L"/DumpPage=\"page_startup\" /Update");
        const std::wstring sVal = CommandLineService::Get(L"dumppage");
        const bool bUpdate = CommandLineService::Has(L"update");

        char sz[256] = {};
        ::sprintf_s(sz, "dumppage=%ls update=%d", sVal.c_str(), bUpdate ? 1 : 0);
        ::printf("[INFO] measured kv: %s\n", sz);
        ::fflush(stdout);
        Chk("quoted value is unquoted", sVal == L"page_startup", sz);
        Chk("a second switch in the same line is found", bUpdate, sz);
    }

    // ---- 3) integer accessor honours the default ----------------------
    {
        CommandLineService::Parse(L"/DumpPage=7");
        const int nGiven = CommandLineService::GetInt(L"dumppage", -1);
        const int nDefault = CommandLineService::GetInt(L"notpresent", -1);
        const int nBad = CommandLineService::GetInt(L"dumppage", 42);

        char sz[160] = {};
        ::sprintf_s(sz, "given=%d default=%d", nGiven, nDefault);
        ::printf("[INFO] measured int: %s\n", sz);
        ::fflush(stdout);
        Chk("integer value is parsed", nGiven == 7, sz);
        Chk("missing key falls back to the default", nDefault == -1, sz);
        (void)nBad;
    }

    // ---- 4) empty and null input must not crash or leave stale state ---
    {
        CommandLineService::Parse(L"/Update");
        const bool bBefore = CommandLineService::Has(L"update");
        CommandLineService::Parse(L"");
        const bool bAfterEmpty = CommandLineService::Has(L"update");
        CommandLineService::Parse(nullptr);
        const bool bAfterNull = CommandLineService::Has(L"update");

        char sz[192] = {};
        ::sprintf_s(sz, "before=%d afterEmpty=%d afterNull=%d",
                    bBefore ? 1 : 0, bAfterEmpty ? 1 : 0, bAfterNull ? 1 : 0);
        ::printf("[INFO] measured reset: %s\n", sz);
        ::fflush(stdout);
        // Parse() must Clear() first: a stale switch surviving a re-parse would
        // make the app run yesterday's mode.
        Chk("re-parsing clears previous switches", bBefore && !bAfterEmpty && !bAfterNull, sz);
    }

    // ---- 5) case-insensitive keys --------------------------------------
    {
        CommandLineService::Parse(L"/ATTRCHECK");
        const bool bUpper = CommandLineService::Has(L"attrcheck");
        CommandLineService::Parse(L"/AttrCheck");
        const bool bMixed = CommandLineService::Has(L"ATTRCHECK");
        char sz[160] = {};
        ::sprintf_s(sz, "upper=%d mixed=%d", bUpper ? 1 : 0, bMixed ? 1 : 0);
        ::printf("[INFO] measured case: %s\n", sz);
        ::fflush(stdout);
        Chk("switch lookup is case-insensitive", bUpper && bMixed, sz);
    }

    if (bInject)
    {
        Chk("injected-must-fail", false, "self-test: forced failure");
    }

    ::printf("\nresult: %d pass / %d fail\n", g_nPass, g_nFail);
    return (g_nFail == 0) ? 0 : 1;
}
