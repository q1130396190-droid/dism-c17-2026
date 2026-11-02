#ifndef __DUILIB_UIAUTOMATION_H__
#define __DUILIB_UIAUTOMATION_H__
// [A11Y-2] Minimal UI Automation bridge for DuiLib's self-painted controls.
//
// Why: every control is custom-drawn, so Windows exposes the paint window as a
// single opaque "pane". Screen readers (Narrator, NVDA) therefore cannot see
// button names, roles, focus or values at all.
//
// Scope of this file (deliberately small and ABI-neutral):
//   * a node model built from the live control tree (role + name + state)
//   * WM_GETOBJECT handling that answers UiaRootObjectId with a root provider
//   * UIA event notification when focus or the tree changes
//
// It is not a full provider: patterns (Invoke/Value/Scroll) are exposed through
// the standard "legacy accessible name/role/state" surface plus fragment
// navigation, which is what screen readers need to announce a self-painted
// toolbar. Everything here is additive; existing behaviour is untouched.
namespace DuiLib {

class CPaintManagerUI;

class UILIB_API CUiAutomation
{
public:
    // Static-only helper: copying it makes no sense and would export
    // generated copy/assign operators as public ABI surface.
    CUiAutomation() = delete;
    CUiAutomation(const CUiAutomation&) = delete;
    CUiAutomation& operator=(const CUiAutomation&) = delete;
    // Called by CPaintManagerUI once the paint window exists. Idempotent and
    // safe when UIAutomationCore is unavailable (returns false, no-op after).
    static bool Install(CPaintManagerUI* pManager);

    // [UIA-MULTI] Per-manager uninstall.
    //
    // The original Uninstall() took no argument and cleared a single process-wide
    // "owner" pointer, so with two windows open, closing either one silently disabled
    // accessibility for the other, and Install() on the second window silently
    // displaced the first. Both behaviours were measured by
    // tools/probes/uia_provider_lifetime_probe.cpp before this change.
    //
    // [UIA-ABI] The original zero-argument overload is kept as its own exported
    // function. A default argument is NOT ABI-compatible: it changes the decorated
    // name from ?Uninstall@...@@SAXXZ to ?Uninstall@...@@SAXPEAV...@Z, so a client
    // built against the old header would fail to resolve the symbol. ABI diff caught
    // exactly this (2 removed symbols). Two real overloads keep both names alive.
    static void Uninstall();
    static void Uninstall(CPaintManagerUI* pManager);

    // Answer WM_GETOBJECT. Returns the LRESULT to use, or 0 with *pbHandled
    // false when the object id is not ours.
    static LRESULT OnGetObject(CPaintManagerUI* pManager, WPARAM wParam, LPARAM lParam, bool* pbHandled);

    // Raise a structure/focus change so assistive tech re-queries the tree.
    static void NotifyFocusChanged(CPaintManagerUI* pManager);
    static void NotifyStructureChanged(CPaintManagerUI* pManager);

    // [UIA-COORD] Convert between DuiLib's client-space control rectangles and the
    // physical SCREEN space UIA requires.
    //
    // UIA's contract for UIA_BoundingRectanglePropertyId and
    // IRawElementProviderFragment::get_BoundingRectangle is physical screen
    // coordinates, and ElementProviderFromPoint receives a screen point. DuiLib
    // stores every control rectangle in paint-window CLIENT coordinates, so both
    // directions must convert. Exposed on the exported class so a probe can assert the
    // conversion instead of only reading the provider source.
    static void ClientRectToScreen(HWND hWnd, const RECT& rcClient, RECT& rcScreen);
    static void ScreenPointToClient(HWND hWnd, POINT& pt);

    // [UIA-ABI] Same reasoning: the no-arg form must remain its own symbol.
    static bool IsInstalled();
    static bool IsInstalled(CPaintManagerUI* pManager);

    // [UIA-ABI] Deliberately NOT a member: CUiAutomation is an exported class, so a
    // new static member becomes a new exported symbol and trips the export-surface
    // ratchet (measured: 147 -> 148). Counting registrations is a probe-only need and
    // is satisfied by querying IsInstalled() for each manager.
};

} // namespace DuiLib

#endif // __DUILIB_UIAUTOMATION_H__
