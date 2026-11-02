import pathlib

p = pathlib.Path('DuiLib/Utils/UIAutomation.cpp')
t = p.read_bytes().decode('utf-8', errors='surrogateescape')

anchor = '''    // Only the client object belongs to us; the non-client area stays with the
    // default window procedure.
    if (static_cast<LONG>(wParam) != OBJID_CLIENT) return 0;
'''
assert t.count(anchor) == 1, t.count(anchor)

uia = '''    // Only the client object belongs to us; the non-client area stays with the
    // default window procedure.
    if (static_cast<LONG>(wParam) != OBJID_CLIENT) return 0;

    // [A11Y-3] A UI Automation client asks for the root provider with
    // UiaRootObjectId, not IID_IAccessible. Answering only IID_IAccessible made
    // every UIA client see the window as one opaque pane, even though this
    // header has always documented the opposite. The provider lives in
    // UIAutomationProvider.cpp; here we only hand out its root object.
    if (static_cast<LONG>(lParam) == UiaRootObjectId)
    {
        IRawElementProviderSimple* pRoot =
            CUiAutomationProvider::CreateRoot(pManager);
        if (pRoot == nullptr) return 0;
        LRESULT lUia = ::UiaReturnRawElementProvider(
            pManager->GetPaintWindow(), wParam, lParam, pRoot);
        pRoot->Release();
        if (pbHandled != nullptr) *pbHandled = true;
        return lUia;
    }
'''
t = t.replace(anchor, uia)

if 'UIAutomationProvider.h' not in t:
    inc = '#include "UIAutomation.h"'
    assert t.count(inc) == 1
    t = t.replace(inc, inc + '\n#include "UIAutomationProvider.h"')

if 'UIAutomationCoreApi.h' not in t:
    inc2 = '#include "UIAutomationProvider.h"'
    t = t.replace(inc2, inc2 + '\n#include <UIAutomationCoreApi.h>')

p.write_bytes(t.encode('utf-8', errors='surrogateescape'))
print('ok')
