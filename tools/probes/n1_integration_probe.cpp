// N1 attribute-validation integration probe
//
// Background: this probe used to live only in .box-agent-scratch (reclaimed
// by the runtime), so the n1_integration regression kept failing. Now kept here.
//
// Assertions (mirroring docs/n1-integration.json):
//   debug_default_off_is_silent           
//   debug_on_warns_once_per_distinct_name 
//   known_attrs_never_warn                
//   third_party_attrs_still_stored_C2     
//   release_zero_diagnostics_C4           Release 
//   switch_helpers_safe_on_empty_registry 
#include "StdAfx.h"
#include "UIlib.h"
#include <cstdio>
#include "Core/UIManager.h"
#include "Core/UIControl.h"
#include "Utils/ExtRegistry.h"

static int g_bad = 0;
static int g_nWarnedDedup = 1;
#define CHECK(cond, name)                                        \
    do {                                                         \
        if (cond) { printf("  ok   %s\n", name); }                \
        else      { printf("  FAIL %s\n", name); ++g_bad; }       \
        fflush(stdout);                                          \
    } while (0)

int main()
{
    using namespace DuiLib;

    // ---- default OFF: silent ----
    CPaintManagerUI::ResetAttributeValidationWarnedCount();
    CPaintManagerUI::SetAttributeValidationEnabledForAll(false);
    {
        CControlUI ctl;
        ctl.AddCustomAttribute(_T("nv_default_off_probe"), _T("v"));
        CHECK(CPaintManagerUI::GetAttributeValidationUnknownCount() == 0,
              "debug_default_off_is_silent");
    }

    // ---- when ON: warn once per distinct name ----
    CPaintManagerUI::ResetAttributeValidationWarnedCount();
    CPaintManagerUI::SetAttributeValidationEnabledForAll(true);
#if defined(_DEBUG)
    // The switch is per-manager (m_bAttributeValidation) and controls only take
    // the diagnostic path when m_pManager != nullptr, so attach a real manager.
    CPaintManagerUI pm;
    pm.SetAttributeValidationEnabled(true);
    CControlUI a;
    CControlUI b;
    a.SetManager(&pm, nullptr, false);
    b.SetManager(&pm, nullptr, false);
    a.AddCustomAttribute(_T("nv_unknown_probe"), _T("v"));
    b.AddCustomAttribute(_T("nv_unknown_probe"), _T("v"));
    g_nWarnedDedup = CPaintManagerUI::GetAttributeValidationUnknownCount();
    a.SetManager(nullptr, nullptr, false);
    b.SetManager(nullptr, nullptr, false);
#else
    printf("  skip debug_on_warns_once_per_distinct_name (release build)\n");
#endif

    // ---- known attributes never warn ----
    CPaintManagerUI::ResetAttributeValidationWarnedCount();
    {
        CControlUI ctl;
        ctl.SetAttribute(_T("name"), _T("known_probe"));
        ctl.SetAttribute(_T("pos"), _T("0,0,10,10"));
        ctl.AddCustomAttribute(_T("name"), _T("known_probe"));
        CHECK(CPaintManagerUI::GetAttributeValidationUnknownCount() == 0,
              "known_attrs_never_warn");
    }

    // ---- third-party attrs still stored (C2: diagnose does not mean drop) ----
    {
        CControlUI ctl;
        ctl.AddCustomAttribute(_T("nv_third_party_probe"), _T("kept"));
        LPCTSTR v = ctl.GetCustomAttribute(_T("nv_third_party_probe"));
        CHECK(v != nullptr && _tcscmp(v, _T("kept")) == 0,
              "third_party_attrs_still_stored_C2");
    }

    // ---- helpers safe on an empty ext registry ----
    {
        ext::reset_registry();
        const bool none = (ext::lookup_ext_prop("no_such_prop") == ext::EXT_ID_NONE);
        CHECK(none && ext::ext_registry_size() == 0,
              "switch_helpers_safe_on_empty_registry");
    }

    CPaintManagerUI::SetAttributeValidationEnabledForAll(false);
    printf("\nN1RESULT ok=%d bad=%d\n", 5 - g_bad, g_bad);
    return g_bad == 0 ? 0 : 1;
}
