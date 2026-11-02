// [THREAD-1] Thread affinity, made explicit and enforceable.
//
// Problem this solves: the framework's thread model has always been an implicit
// convention -- "controls and the paint manager are UI-thread only" -- stated
// nowhere and enforced by nothing. The concurrency audit (round 33) confirmed the
// APIs that DO claim thread safety, but found the boundary itself undocumented:
// 45 `static mutable state` sites rely on the caller happening to stay on the UI
// thread.
//
// [R-07] The first version recorded ONE process-wide owner thread. Correct for a
// single-window app, WRONG for the multi-UI-thread case the framework otherwise
// supports: with two paint managers on two threads the first claim won, and every
// legitimate call on the second thread was reported as a violation.
//
// Ownership therefore lives per manager, and WHERE it lives matters:
//
//   The state is held inside the DLL (implemented in UIManager.cpp), reached
//   through Deref() rather than through a header-defined variable. An earlier
//   draft used `inline static` members here, which every module gets its own copy
//   of: the DLL registered manager A on thread X, while the probe -- its own copy
//   of the table -- saw an empty registry, concluded "unclaimed", and RAN the
//   worker's task inline on the worker thread. Measured exactly that way. Any
//   header-visible state in a DLL/SDK is per-module, so the accessor must resolve
//   to a symbol the DLL owns.
//
//   Deref() is inline and calls a function the DLL exports. In the DLL's own
//   translation units UILIB_EXPORTS makes it dllexport; in a consumer build the
//   same header declares it dllimport and the call binds to the DLL's copy.
//
// Why no new class member: CPaintManagerUI is held BY VALUE inside
// WindowImplBase, so adding a data member changes the layout that the ABI guard
// exists to protect.
//
// Release behaviour: the Debug break is kept, and BOTH builds additionally count
// violations, so a shipping binary can be asked afterwards whether the contract
// was broken. The counter is touched only on the failure path.
//
// Scope note: this is an ASSERTION, not a lock. It does not make anything
// thread-safe; it makes a violation visible where it happens instead of as a
// corrupted control tree later.
#ifndef __DUILIB_THREAD_AFFINITY_H__
#define __DUILIB_THREAD_AFFINITY_H__

#include <windows.h>

// UILIB_API fallback, mirrored from UIResourceManager.h: this header is included
// from translation units that have not yet seen UIlib.h.
#ifndef UILIB_API
    #ifdef UILIB_STATIC
        #define UILIB_API
    #elif defined(UILIB_EXPORTS)
        #define UILIB_API __declspec(dllexport)
    #elif defined(_MSC_VER)
        #define UILIB_API __declspec(dllimport)
    #else
        #define UILIB_API
    #endif
#endif

namespace DuiLib {

class CPaintManagerUI;

// One block of thread-affinity state, owned by the DLL.
//
// The DEFINITION is here because the inline accessors below read its members,
// but there is only ever ONE INSTANCE in the process: it is the function-local
// static inside DuiLibThreadAffinityState(), which lives in the DLL. Defining
// the type in a header is safe; defining the VARIABLE there is not, and that is
// the distinction the earlier draft got wrong.
struct TThreadAffinityState
{
    struct OwnerSlot { const CPaintManagerUI* pManager; DWORD dwThreadId; };
    static const int kMaxManagers = 16;
    volatile DWORD dwUiThread;
    volatile LONG  nViolations;
    OwnerSlot      owner[kMaxManagers];
    SRWLOCK        lock;
};

// Implemented in UIManager.cpp: the ONLY instance of the state block in the
// process. Must be declared before CThreadAffinity, because Deref() below is the
// first thing that uses it -- an earlier ordering failed to compile with C3861.
UILIB_API TThreadAffinityState* DuiLibThreadAffinityState();

class CThreadAffinity
{
public:
    // Resolves to the DLL's single state block. Callers use this instead of
    // touching any header-defined variable, because a header-defined variable
    // would be duplicated per module -- see the comment at the top.
    inline static TThreadAffinityState* Deref()
    {
        return DuiLibThreadAffinityState();
    }

    // --- process-wide owner: the headless fallback ---
    inline static void ClaimUiThread(DWORD dwThreadId)
    {
        ::InterlockedCompareExchange(
            reinterpret_cast<volatile LONG*>(&Deref()->dwUiThread),
            static_cast<LONG>(dwThreadId), 0);
    }
    inline static DWORD UiThreadId() { return Deref()->dwUiThread; }
    inline static bool  IsUnclaimed() { return Deref()->dwUiThread == 0; }
    inline static bool  OnUiThread() { return Deref()->dwUiThread == ::GetCurrentThreadId(); }

    // --- release-visible violation counter ---
    inline static void RecordViolation() { ::InterlockedIncrement(&Deref()->nViolations); }
    inline static LONG Violations() { return Deref()->nViolations; }
    inline static void ResetViolations() { ::InterlockedExchange(&Deref()->nViolations, 0); }

    // --- per-manager ownership ---
    // Returns false when the manager already belongs to a DIFFERENT thread. The
    // original owner is kept: two threads claiming one window is never legitimate,
    // and reassigning would hide the bug.
    UILIB_API static bool RegisterUiThread(const CPaintManagerUI* pManager, DWORD dwThreadId);
    UILIB_API static void UnregisterUiThread(const CPaintManagerUI* pManager);
    // Returns 0 when this manager has no recorded owner (headless / not attached).
    UILIB_API static DWORD OwnerOf(const CPaintManagerUI* pManager);
};

// Is the caller the owner of THIS manager? A manager with no recorded owner is
// treated as headless and never accused -- that is what keeps markup parse (which
// runs before AttachDialog) from tripping the check.
inline bool IsUiThreadForManager(const CPaintManagerUI* pManager)
{
    const DWORD dwOwner = CThreadAffinity::OwnerOf(pManager);
    if (dwOwner == 0) return true;
    return dwOwner == ::GetCurrentThreadId();
}

// Counts the violation in every build; also reports it in Debug.
inline void ReportUiThreadViolation(const CPaintManagerUI* pManager, const char* pszFunc)
{
    CThreadAffinity::RecordViolation();
    (void)pManager;
#ifdef _DEBUG
    ::OutputDebugStringA("[DuiLib] UI-thread-only API called from tid=");
    ::OutputDebugStringA(pszFunc ? pszFunc : "?");
    ::OutputDebugStringA("\r\n");
#else
    (void)pszFunc;
#endif
}

} // namespace DuiLib

// Break/no-op helper, defined BEFORE the guards that use it: an earlier draft put
// these at the bottom, so the macro expanded to an undefined name.
#ifdef _DEBUG
#define DUI_UI_THREAD_BREAK_() ::DebugBreak()
#else
#define DUI_UI_THREAD_BREAK_() ((void)0)
#endif

// Guard with an explicit manager. Preferred form: it knows which window the
// caller means, so two UI threads do not accuse each other.
#define DUI_ASSERT_UI_THREAD_FOR(pm)                                             \
    do {                                                                         \
        if (!::DuiLib::IsUiThreadForManager(pm)) {                               \
            ::DuiLib::ReportUiThreadViolation((pm), __FUNCTION__);               \
            DUI_UI_THREAD_BREAK_();                                              \
        }                                                                        \
    } while (0)

// Legacy form, no manager in scope: falls back to the process-wide owner.
// Debug breaks; Release still counts the violation, which is the release-visible
// half of the contract.
#ifdef _DEBUG
#define DUI_ASSERT_UI_THREAD()                                                   \
    do {                                                                         \
        if (!::DuiLib::CThreadAffinity::IsUnclaimed() &&                         \
            !::DuiLib::CThreadAffinity::OnUiThread()) {                          \
            ::DuiLib::ReportUiThreadViolation(nullptr, __FUNCTION__);            \
            DUI_UI_THREAD_BREAK_();                                              \
        }                                                                        \
    } while (0)
#else
#define DUI_ASSERT_UI_THREAD()                                                   \
    do {                                                                         \
        if (!::DuiLib::CThreadAffinity::IsUnclaimed() &&                         \
            !::DuiLib::CThreadAffinity::OnUiThread()) {                          \
            ::DuiLib::CThreadAffinity::RecordViolation();                        \
        }                                                                        \
    } while (0)
#endif

#endif // __DUILIB_THREAD_AFFINITY_H__
