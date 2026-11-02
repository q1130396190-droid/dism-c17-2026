// [GATE-COV] Three zero-dependency subsystems: AssocService, ControlService,
// DependencyGraphService.
//
// Why these three: the round-45 classification measured their heavy-dependency
// keyword count as 0 (no DISM, no COM session, no process launch), which is what
// made them affordable to cover.
//
// Scope, stated so the gate cannot drift into writing:
//   * AssocService   -- QueryProgIdAndCommand only. Set*/Clear*/Reset* write the
//                       user's file associations; never called here.
//   * ControlService -- ResolveIndirectString + EnumControlApplets. Open* launches
//                       control.exe; never called here.
//   * DependencyGraphService -- Compose only, a pure function. The Query*/Build*
//                       variants touch the service manager and Appx.
#include <windows.h>

#include "stdafx.h"
#include "ControlService.h"

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

    // ============ DependencyGraphService: not covered here ============
    // Compose() itself is pure, but linking DependencyGraphService.cpp also drags
    // in StartupService::EnumerateDependentServices and
    // ExternalToolService::IsToolAvailable -- two more translation units, i.e. the
    // same dependency-chain cost that excluded FeatureService and
    // ExternalToolService. Recorded, not faked.
    // ============ ControlService ============
    {
        std::vector<CtrlAppletEntry> vApplets;
        const EControlEnumResult eRes = ControlService::EnumControlApplets(vApplets);

        char sz[192] = {};
        ::sprintf_s(sz, "res=%d applets=%d", (int)eRes, (int)vApplets.size());
        ::printf("[INFO] measured applets: %s\n", sz);
        ::fflush(stdout);

        // A Windows install always has some .cpl/control applets. Zero means the
        // enumeration broke, and the control-panel page would look empty.
        Chk("control applets were enumerated", !vApplets.empty(), sz);
        if (!vApplets.empty())
        {
            int nNoLabel = 0;
            for (size_t i = 0; i < vApplets.size(); ++i)
                if (vApplets[i].sDisplayName.IsEmpty() && vApplets[i].sCanonical.IsEmpty())
                    ++nNoLabel;
            ::sprintf_s(sz, "applets=%d unlabelled=%d", (int)vApplets.size(), nNoLabel);
            Chk("no applet is completely unlabelled", nNoLabel == 0, sz);
        }

        // ResolveIndirectString on an ordinary literal must pass it through and
        // report success; this is what turns "@%SystemRoot%\x.dll,-1" into text.
        CDuiString sOut;
        const bool bPlain = ControlService::ResolveIndirectString(L"plain text", sOut);
        ::sprintf_s(sz, "ok=%d out=%ls", bPlain ? 1 : 0, sOut.GetData());
        Chk("a plain string resolves to itself",
            bPlain && sOut.Compare(L"plain text") == 0, sz);
    }

    // ============ AssocService: not covered here ============
    // AssocService.cpp pulls ShellExecuteExW through the Demo PCH. Compiling it
    // standalone needs that PCH chain reproduced -- the same cost that made
    // ExternalToolService not worth covering (round 44). Recorded, not faked.
    if (bInject)
    {
        Chk("injected-must-fail", false, "self-test: forced failure");
    }

    ::printf("\nresult: %d pass / %d fail\n", g_nPass, g_nFail);
    return (g_nFail == 0) ? 0 : 1;
}
