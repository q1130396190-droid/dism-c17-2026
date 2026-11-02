#ifndef __DUILIB_UIAUTOMATION_PROVIDER_H__
#define __DUILIB_UIAUTOMATION_PROVIDER_H__
// [A11Y-3] Entry point for the UI Automation provider.
//
// Deliberately does NOT include any SDK UIA header: the .cpp needs a very
// specific include order (Core -> CoreApi -> Client, because PROPERTYID is
// defined in Core and consumed by CoreApi), and pulling a header in here would
// pin that order for every translation unit that includes this file.
//
// The forward declaration below matches the SDK's COM interface form closely
// enough for a pointer return type; the .cpp includes the real headers.
struct IRawElementProviderSimple;

namespace DuiLib {

class CPaintManagerUI;

class CUiAutomationProvider
{
public:
    CUiAutomationProvider() = delete;

    // Returns a new reference (caller owns one) for the window's root provider,
    // or nullptr when the manager is not ready.
    static IRawElementProviderSimple* CreateRoot(CPaintManagerUI* pManager);
};

} // namespace DuiLib

#endif // __DUILIB_UIAUTOMATION_PROVIDER_H__
