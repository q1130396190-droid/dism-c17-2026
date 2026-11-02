// [GATE-COV] ScheduleTaskService read path -- enumeration only, no import/export.
//
// Why this exists: the coverage audit (round 30) found 27 of 46 subsystems with no
// assertion anywhere, ScheduleTaskService among them. Its page is checked
// ("page_tasks has N controls") but nothing checked the enumeration behind it. A
// silent break there shows up as "this machine has no scheduled tasks", which
// reads as a clean system rather than a failed query.
//
// Scope: EnumTasks + IsTaskServiceAvailable. ImportTaskXml / ExportTaskXml touch
// the filesystem and the task store, so they are never called here.
//
// Environment handling, stated rather than papered over: the Task Scheduler COM
// API needs the service to be running. When it is not available this probe prints
// SKIP with the reason and exits 0 -- the wrapper must NOT count that as a gate
// pass, because a skip asserts nothing.
#include "stdafx.h"
#include "ScheduleTaskService.h"

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

    if (!ScheduleTaskService::IsTaskServiceAvailable())
    {
        ::printf("[SKIP] Task Scheduler COM API unavailable (service stopped or"
                 " blocked); enumeration cannot be asserted on this host\n");
        ::fflush(stdout);
        if (bInject)
        {
            Chk("injected-must-fail", false, "self-test: forced failure");
        }
        ::printf("\nresult: %d pass / %d fail / 1 skip\n", g_nPass, g_nFail);
        return (g_nFail == 0) ? 0 : 1;
    }

    std::vector<ScheduleTaskService::TaskInfo> v1;
    DWORD dwErr1 = 0;
    const int nRet1 = ScheduleTaskService::EnumTasks(v1, dwErr1);

    char sz[256] = {};
    ::sprintf_s(sz, "ret=%d err=0x%08lX tasks=%d",
                nRet1, (unsigned long)dwErr1, (int)v1.size());
    Chk("enumeration succeeds on this host", nRet1 == 0, sz);

    // [ZERO-INPUT] A successful call that returns zero tasks means the folder walk
    // found nothing -- every assertion below would then loop zero times and report
    // PASS. A Windows install always has scheduled tasks, so treat 0 as no evidence.
    if (nRet1 == 0 && v1.empty())
    {
        Chk("enumeration produced entries", false, "0 tasks -- nothing was measured");
    }
    else if (nRet1 == 0)
    {
        // ---- idempotent: a second pass must agree on the count ----
        std::vector<ScheduleTaskService::TaskInfo> v2;
        DWORD dwErr2 = 0;
        const int nRet2 = ScheduleTaskService::EnumTasks(v2, dwErr2);
        ::sprintf_s(sz, "first=%d second=%d", (int)v1.size(), (int)v2.size());
        Chk("enumeration is idempotent", nRet2 == 0 && v1.size() == v2.size(), sz);

        // ---- every entry must be usable ----
        int nNoName = 0;
        std::set<std::wstring> setNames;
        for (size_t i = 0; i < v1.size(); ++i)
        {
            if (v1[i].sName.IsEmpty()) ++nNoName;
            else setNames.insert(std::wstring(v1[i].sName.GetData()));
        }
        ::sprintf_s(sz, "tasks=%d noName=%d unique=%d",
                    (int)v1.size(), nNoName, (int)setNames.size());
        Chk("no task has an empty name", nNoName == 0, sz);
        // sName is the full task path; duplicates would make row-keyed actions
        // hit the wrong task.
        Chk("task names are unique", setNames.size() == v1.size(), sz);

        // ---- a root folder task, if any, sits directly under \ ----
        if (!v1.empty())
        {
            ::printf("[INFO] sample: name=%s status=%s\n",
                     W2A(v1[0].sName.GetData()).c_str(),
                     W2A(v1[0].sStatus.GetData()).c_str());
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
