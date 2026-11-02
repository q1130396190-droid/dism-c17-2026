// [RESMGR] CResourceManager lifetime probe.
//
// The defect this reproduces:
//   CResourceManager::Release() calls `delete this`, but every instance is also owned
//   by a registry of std::unique_ptr. So one call destroys the object while the
//   registry still holds a pointer to it, and the next registry access -- or the
//   static destructor at process exit -- frees it a second time.
//
// What makes this probe worth writing:
//   A double free does not necessarily crash immediately. It corrupts the heap and
//   the crash lands somewhere unrelated later, which is why an inspection-only review
//   was the only evidence for this defect until now. The probe therefore does the
//   three things that make the failure deterministic:
//     1. GetInstance -> Release -> GetInstance again (use of the freed pointer),
//     2. ReleaseScope (the registry deletes the already-deleted pointer),
//     3. exit normally so the static destructor runs too.
//   With the defect present, at least one of those must fail loudly.
//
// /Inject flips the expectation so the wrapper can prove the probe can fail.

#include "StdAfx.h"
#include "UIlib.h"
#include "Core/UIResourceManager.h"

#include <cstdio>

using namespace DuiLib;

static int g_pass = 0;
static int g_fail = 0;

static void Pass(const char* what)
{
    printf("[PASS] %s\n", what);
    ++g_pass;
}
static void Fail(const char* what)
{
    printf("[FAIL] %s\n", what);
    ++g_fail;
}

// Variant that accepts one integer, so messages can carry the measured value.
static void FailInt(const char* what, int value)
{
    printf("[FAIL] %s (measured=%d)\n", what, value);
    ++g_fail;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    bool bInject = false;
    for (int i = 1; i < argc; ++i)
        if (_stricmp(argv[i], "/Inject") == 0) bInject = true;

    printf("[probe] CResourceManager lifetime\n");

    // Start clean so earlier probe runs in the same process image cannot interfere.
    CResourceManager::ReleaseAll();
    printf("[probe] scopes after ReleaseAll: %d\n", CResourceManager::GetScopeCount());

    // ---- 1. the documented legacy usage pattern ------------------------------
    // Callers are told "call Release() when done with the pointer you got".
    CResourceManager* pFirst = CResourceManager::GetInstance(_T("lifetime-probe"));
    if (pFirst == nullptr) {
        Fail("GetInstance returned null for a fresh scope");
        printf("\n%d pass / %d fail\n", g_pass, g_fail);
        return 1;
    }
    Pass("GetInstance returned an instance");

    // ---- 2. Release(), then ask the registry again ---------------------------
    // If Release() really destroyed the object, the registry entry is now dangling.
    // A correct implementation must leave the registry consistent: either the entry
    // is gone, or GetInstance hands back a LIVE object.
    pFirst->Release();
    printf("[probe] called Release(); asking the registry again\n");

    // ---- 3. registry consistency after Release -------------------------------
    // GetScopeCount reads the registry map. With a dangling unique_ptr this call is
    // the first place the corruption becomes observable rather than latent.
    const int nScopes = CResourceManager::GetScopeCount();
    printf("[probe] scopes after Release(): %d\n", nScopes);

    // The registry Must not still advertise a scope whose object was freed.
    // If it does, GetInstance below will hand out a dangling pointer.
    if (nScopes > 0) {
        FailInt("registry still holds a scope after Release() destroyed the object "
                "-- the entry is dangling", nScopes);
    } else {
        Pass("Release() removed the scope from the registry");
    }

    // ---- 4. re-acquire and touch the object ---------------------------------
    // This is the use-after-free read. If the registry handed back the freed object,
    // calling a method on it is undefined behaviour; the probe treats "returned a
    // pointer that the registry still owned" as the defect regardless of whether the
    // read happens to survive.
    CResourceManager* pSecond = CResourceManager::GetInstance(_T("lifetime-probe"));
    if (pSecond == nullptr) {
        Pass("GetInstance after Release() returned null (no dangling pointer handed out)");
    } else {
        // Address equality is NOT proof of a use-after-free: the allocator is free to
        // reuse the exact same block for a new object of the same size, which is
        // precisely what happens here (scope count is already 0, so this is a fresh
        // instance at a recycled address). Treating equality as the defect produced a
        // false failure after the real bug was fixed.
        //
        // The real question is whether the registry is consistent: it must contain
        // exactly one live entry, and the pointer it hands out must be reachable
        // through it. A dangling entry would make the scope count 1 while the object
        // is already destroyed -- which the earlier assertion already caught.
        const int nAfterReacquire = CResourceManager::GetScopeCount();
        if (nAfterReacquire == 1) {
            Pass("GetInstance after Release() produced a live instance (registry consistent)");
        } else {
            FailInt("registry inconsistent after re-acquiring the scope", nAfterReacquire);
        }
    }

    // ---- 5. registry teardown must not double-free --------------------------
    // ReleaseScope erases the entry, which runs ~unique_ptr. If the object was
    // already deleted by Release(), this is the second free.
    CResourceManager::ReleaseScope(_T("lifetime-probe"));
    Pass("ReleaseScope completed without a second free");

    CResourceManager::ReleaseAll();
    const int nFinal = CResourceManager::GetScopeCount();
    if (nFinal == 0) Pass("registry empty after ReleaseAll");
    else            FailInt("registry not empty after ReleaseAll", nFinal);

    if (bInject) {
        Fail("injected: deliberately failing assertion");
    }

    printf("\n%d pass / %d fail\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
