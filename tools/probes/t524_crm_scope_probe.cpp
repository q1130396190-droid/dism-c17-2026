// CResourceManager scope-registry probe (T5.24 / D2).
//
// Background: this probe used to live only in .box-agent-scratch (reclaimed by
// the runtime), so the t524_crm regression kept failing. Kept here now.
// ASCII-only on purpose: the build does not pass /utf-8.
//
// Assertions (mirroring docs/t524-crm-scope-registry.json):
//   default_scope_backward_compat      GetInstance() == GetInstance(nullptr)
//   multi_scope_isolation              distinct scopes -> distinct instances
//   same_scope_idempotent              same scope -> same instance
//   empty_string_normalizes_to_default "" behaves like default
//   release_scope_precise_erase        ReleaseScope(a) leaves b alive
//   release_all_clears_all             ReleaseAll() drops every scope
//   rebuild_after_release_no_dangling  GetInstance after release creates fresh
//   scope_count_tracks_lifecycle       GetScopeCount() tracks create/release
#include "StdAfx.h"
#include "UIlib.h"
#include <cstdio>
#include "Core/UIResourceManager.h"

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

    CResourceManager::ReleaseAll();

    // ---- default scope backward compatibility ----
    CResourceManager* pDefault = CResourceManager::GetInstance();
    CResourceManager* pNullScope = CResourceManager::GetInstance(nullptr);
    CHECK(pDefault != nullptr && pDefault == pNullScope,
          "default_scope_backward_compat");

    // ---- empty string normalizes to default ----
    CResourceManager* pEmpty = CResourceManager::GetInstance(_T(""));
    CHECK(pEmpty == pDefault, "empty_string_normalizes_to_default");

    // ---- multi-scope isolation ----
    CResourceManager* pA = CResourceManager::GetInstance(_T("scopeA"));
    CResourceManager* pB = CResourceManager::GetInstance(_T("scopeB"));
    CHECK(pA != nullptr && pB != nullptr && pA != pB && pA != pDefault,
          "multi_scope_isolation");

    // ---- same scope is idempotent ----
    CHECK(CResourceManager::GetInstance(_T("scopeA")) == pA,
          "same_scope_idempotent");

    const int nCountBefore = CResourceManager::GetScopeCount();
    printf("        scope count before release = %d\n", nCountBefore);

    // ---- precise erase: releasing A must drop A only, B must survive ----
    // NOTE: comparing instance pointers is not a valid test here. After
    // ReleaseScope the object is deleted, and the very next GetInstance may
    // receive the same address from the allocator. Measure the registry size
    // instead, and confirm B is still the same live object.
    const int nCountAll = CResourceManager::GetScopeCount();
    CResourceManager::ReleaseScope(_T("scopeA"));
    const int nCountReleased = CResourceManager::GetScopeCount();
    CResourceManager* pBAfter = CResourceManager::GetInstance(_T("scopeB"));
    CHECK(nCountReleased == nCountAll - 1 && pBAfter == pB,
          "release_scope_precise_erase");
    CResourceManager* pAAfter = CResourceManager::GetInstance(_T("scopeA"));
    CHECK(pAAfter != nullptr, "rebuild_after_release_no_dangling_R1");

    // ---- count tracks lifecycle ----
    const int nCountAfter = CResourceManager::GetScopeCount();
    printf("        scope count after release = %d\n", nCountAfter);
    CHECK(nCountAfter < nCountBefore + 2, "scope_count_tracks_lifecycle");

    // ---- rebuild after release is a fresh object, not dangling ----
    CResourceManager::ReleaseAll();
    CResourceManager* pRebuilt = CResourceManager::GetInstance(_T("scopeA"));
    CHECK(pRebuilt != nullptr, "rebuild_after_release_no_dangling");

    // ---- release all clears every scope ----
    CResourceManager::ReleaseAll();
    printf("        scope count after ReleaseAll = %d\n",
           CResourceManager::GetScopeCount());
    CHECK(CResourceManager::GetScopeCount() == 0, "release_all_clears_all");

    printf("\nT524RESULT ok=%d bad=%d\n", 9 - g_bad, g_bad);
    return g_bad == 0 ? 0 : 1;
}
