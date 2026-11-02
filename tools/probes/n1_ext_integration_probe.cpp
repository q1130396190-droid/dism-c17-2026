// N1 + ext three-tier integration probe.
//
// Background: this probe used to live only in .box-agent-scratch (reclaimed by
// the runtime), so the n1_ext_integration regression kept failing. Kept here now.
// ASCII-only on purpose: the build does not pass /utf-8.
//
// Assertions (mirroring docs/n1-ext-integration.json):
//   S3_hit_never_warns            S3 known property -> no N1 diagnostic
//   ext_hit_never_warns_D4        ext registered name -> no N1 diagnostic
//   S3_miss_ext_miss_triggers_N1  unknown name -> exactly one diagnostic
//   ext_case_insensitive          ext lookup is case-insensitive
//   ext_registered_still_stored_C2  ext attr value still stored
//   typo_still_stored_C2          unknown attr value still stored
#include "StdAfx.h"
#include "UIlib.h"
#include <cstdio>
#include "Core/UIManager.h"
#include "Core/UIControl.h"
#include "Utils/ExtRegistry.h"

static int g_bad = 0;
#define CHECK(cond, name)                                        \
    do {                                                         \
        if (cond) { printf("  ok   %s\n", name); }                \
        else      { printf("  FAIL %s\n", name); ++g_bad; }       \
        fflush(stdout);                                          \
    } while (0)

int main()
{
    using namespace DuiLib;

    ext::reset_registry();
    const std::uint32_t kExtId = 0x1234;
    ext::register_ext_prop("probe_ext_attr", kExtId);

    CPaintManagerUI pm;
    pm.SetAttributeValidationEnabled(true);

    // ---- S3 hit: framework property, must not warn ----
    CPaintManagerUI::ResetAttributeValidationWarnedCount();
    {
        CControlUI ctl;
        ctl.SetManager(&pm, nullptr, false);
        ctl.SetAttribute(_T("name"), _T("s3_probe"));
        ctl.AddCustomAttribute(_T("name"), _T("s3_probe"));
        CHECK(CPaintManagerUI::GetAttributeValidationUnknownCount() == 0,
              "S3_hit_never_warns");
        ctl.SetManager(nullptr, nullptr, false);
    }

    // ---- ext hit: registered extension name, must not warn ----
    CPaintManagerUI::ResetAttributeValidationWarnedCount();
    {
        CControlUI ctl;
        ctl.SetManager(&pm, nullptr, false);
        ctl.AddCustomAttribute(_T("probe_ext_attr"), _T("extval"));
        const int n = CPaintManagerUI::GetAttributeValidationUnknownCount();
        LPCTSTR kept = ctl.GetCustomAttribute(_T("probe_ext_attr"));
        CHECK(n == 0, "ext_hit_never_warns_D4");
        CHECK(kept != nullptr && _tcscmp(kept, _T("extval")) == 0,
              "ext_registered_still_stored_C2");
        ctl.SetManager(nullptr, nullptr, false);
    }

    // ---- ext lookup is case-insensitive ----
    CHECK(ext::lookup_ext_prop("PROBE_EXT_ATTR") == kExtId,
          "ext_case_insensitive");

    // ---- both miss: exactly one N1 diagnostic, value still stored (C2) ----
    CPaintManagerUI::ResetAttributeValidationWarnedCount();
    {
        CControlUI ctl;
        ctl.SetManager(&pm, nullptr, false);
        ctl.AddCustomAttribute(_T("probe_definitely_unknown_attr"), _T("kept"));
        const int n = CPaintManagerUI::GetAttributeValidationUnknownCount();
        LPCTSTR kept = ctl.GetCustomAttribute(_T("probe_definitely_unknown_attr"));
#if defined(_DEBUG)
        // Constraint C4: Release builds contain no N1 diagnostics at all,
        // so "a miss must warn" is only meaningful in Debug.
        CHECK(n == 1, "S3_miss_ext_miss_triggers_N1");
#else
        CHECK(n == 0, "S3_miss_ext_miss_triggers_N1_release_zero_C4");
        (void)n;
#endif
        CHECK(kept != nullptr && _tcscmp(kept, _T("kept")) == 0,
              "typo_still_stored_C2");
        ctl.SetManager(nullptr, nullptr, false);
    }

    pm.SetAttributeValidationEnabled(false);
    ext::reset_registry();

    printf("\nN1EXTRESULT ok=%d bad=%d\n", 6 - g_bad, g_bad);
    return g_bad == 0 ? 0 : 1;
}
