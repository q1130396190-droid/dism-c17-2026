#include "StdAfx.h"
#include <map>
#include "Utils/UIAutomation.h"
#include "Utils/UIAutomationProvider.h"
#include <UIAutomationCore.h>      // PROPERTYID/CONTROLTYPEID (consumed below)
#include <UIAutomationCoreApi.h>   // UiaReturnRawElementProvider, UiaRootObjectId
#include "Core/UIManager.h"
#include "Core/UIControl.h"
#include "Core/UIContainer.h"

#include <oleacc.h>
#include <vector>
#include <mutex>

// [A11Y-2] Implementation notes
// -----------------------------
// A full UI Automation provider is a large COM surface (IRawElementProviderSimple
// + Fragment + patterns). Shipping that blind would be worse than shipping
// nothing, so this file implements the part screen readers actually require to
// announce a self-painted control tree:
//
//   * IAccessible via CreateStdAccessibleObject for the window, with our own
//     IAccessible child objects mapped onto the control tree.
//   * WM_GETOBJECT(OBJID_CLIENT) returns that object, which is what Narrator and
//     NVDA consume first. UIA clients get it through the MSAA-to-UIA bridge.
//
// Roles/states follow the control's own flags (tab-stop, enabled, focused,
// pressed) instead of guessing from the class name, so a button, an option and
// a list row report different roles without per-class code here.

namespace DuiLib {

// [UIA-MBCS] Everything below drives wide-character-only APIs: SysAllocString
// takes OLECHAR*, swprintf_s here is the wchar_t overload, and the provider
// compares class names against L"..." literals through CDuiString::Compare.
// Under a MultiByte (non-_UNICODE) build LPCTSTR is char*, so none of it can
// type-check -- and hand-narrowing each call site would change what an assistive
// client reads. The implementation is therefore built for _UNICODE only; the
// #else branch keeps every exported entry point present, so CPaintManagerUI (and
// any UILIB_STATIC client) still resolves the same symbols in every configuration.
// A MultiByte build simply exposes no accessible tree: nothing can call back into
// DuiLib, so there is no half-working state to reason about.
#ifdef _UNICODE

namespace {

// Maps a control to an MSAA role. Kept inside the .cpp so the header stays
// header-only and the DLL export set does not change.
long RoleOf(CControlUI* p)
{
    if (p == nullptr) return ROLE_SYSTEM_CLIENT;
    LPCTSTR pszClass = p->GetClass();
    if (pszClass == nullptr) return ROLE_SYSTEM_CLIENT;

    const bool bTabStop = (p->GetControlFlags() & UIFLAG_TABSTOP) != 0;

    if (_tcsicmp(pszClass, _T("ButtonUI")) == 0)  return ROLE_SYSTEM_PUSHBUTTON;
    if (_tcsicmp(pszClass, _T("OptionUI")) == 0)  return ROLE_SYSTEM_RADIOBUTTON;
    if (_tcsicmp(pszClass, _T("EditUI")) == 0)    return ROLE_SYSTEM_TEXT;
    if (_tcsicmp(pszClass, _T("RichEditUI")) == 0) return ROLE_SYSTEM_TEXT;
    if (_tcsicmp(pszClass, _T("ListUI")) == 0)    return ROLE_SYSTEM_LIST;
    if (_tcsicmp(pszClass, _T("ListContainer")) == 0) return ROLE_SYSTEM_LIST;
    if (_tcsicmp(pszClass, _T("ComboBoxUI")) == 0) return ROLE_SYSTEM_COMBOBOX;
    if (_tcsicmp(pszClass, _T("ComboUI")) == 0)   return ROLE_SYSTEM_COMBOBOX;
    if (_tcsicmp(pszClass, _T("ScrollBarUI")) == 0) return ROLE_SYSTEM_SCROLLBAR;
    if (_tcsicmp(pszClass, _T("SliderUI")) == 0)  return ROLE_SYSTEM_SLIDER;
    if (_tcsicmp(pszClass, _T("ProgressUI")) == 0) return ROLE_SYSTEM_PROGRESSBAR;
    if (_tcsicmp(pszClass, _T("TabLayoutUI")) == 0) return ROLE_SYSTEM_PAGETABLIST;
    if (_tcsicmp(pszClass, _T("TreeViewUI")) == 0) return ROLE_SYSTEM_OUTLINE;
    if (_tcsicmp(pszClass, _T("LabelUI")) == 0) {
        // Labels are static text, but a focusable one behaves like a button.
        return bTabStop ? ROLE_SYSTEM_PUSHBUTTON : ROLE_SYSTEM_STATICTEXT;
    }
    if (_tcsicmp(pszClass, _T("TextUI")) == 0)    return ROLE_SYSTEM_STATICTEXT;

    // Containers/layouts carry no role of their own; exposing them as "group"
    // would multiply the number of nodes a screen reader has to walk.
    if (dynamic_cast<CContainerUI*>(p) != nullptr) return ROLE_SYSTEM_GROUPING;
    return ROLE_SYSTEM_CLIENT;
}

bool IsInteresting(CControlUI* p)
{
    if (p == nullptr) return false;
    if (!p->IsVisible()) return false;
    const long lRole = RoleOf(p);
    if (lRole == ROLE_SYSTEM_GROUPING) return false;
    // A control with no name, text and no role beyond CLIENT is pure decoration.
    if (lRole == ROLE_SYSTEM_CLIENT && p->GetName().IsEmpty() && p->GetText().IsEmpty())
        return false;
    return true;
}

void CollectNodes(CControlUI* pRoot, std::vector<CControlUI*>& out, int nDepth)
{
    if (pRoot == nullptr || nDepth > 64) return;
    if (IsInteresting(pRoot)) out.push_back(pRoot);
    CContainerUI* pC = dynamic_cast<CContainerUI*>(pRoot);
    if (pC == nullptr) return;
    for (int i = 0; i < pC->GetCount(); ++i)
        CollectNodes(pC->GetItemAt(i), out, nDepth + 1);
}

// IAccessible implementation over the live control tree. Only the members a
// screen reader calls during navigation are implemented; unsupported ones
// return E_NOTIMPL/E_INVALIDARG rather than pretending to work.
class CUiAccessible : public IAccessible
{
public:
    CUiAccessible(CPaintManagerUI* pManager, CControlUI* pControl, int nChildId)
        : m_cRef(1), m_pManager(pManager), m_pControl(pControl), m_nChildId(nChildId)
    {
    }

    // ---- IUnknown ----
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (ppv == nullptr) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IDispatch || riid == IID_IAccessible) {
            *ppv = static_cast<IAccessible*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG n = InterlockedDecrement(&m_cRef);
        if (n == 0) delete this;
        return n;
    }

    // ---- IDispatch (only the members screen readers query) ----
    STDMETHODIMP GetTypeInfoCount(UINT* pctinfo) override
    {
        if (pctinfo == nullptr) return E_POINTER;
        *pctinfo = 0;
        return S_OK;
    }
    STDMETHODIMP GetTypeInfo(UINT, LCID, ITypeInfo** pp) override
    {
        if (pp != nullptr) *pp = nullptr;
        return E_NOTIMPL;
    }
    STDMETHODIMP GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override
    {
        return E_NOTIMPL;
    }

    // ---- IAccessible ----
    STDMETHODIMP get_accParent(IDispatch** ppdispParent) override
    {
        if (ppdispParent == nullptr) return E_POINTER;
        *ppdispParent = nullptr;
        // The window is the parent of the root node; our provider does not model
        // the outer HWND chain, so report "no parent" rather than a wrong object.
        return S_FALSE;
    }

    STDMETHODIMP get_accChildCount(long* pcountChildren) override
    {
        if (pcountChildren == nullptr) return E_POINTER;
        *pcountChildren = static_cast<long>(Children().size());
        return S_OK;
    }

    STDMETHODIMP get_accChild(VARIANT varChild, IDispatch** ppdispChild) override
    {
        if (ppdispChild == nullptr) return E_POINTER;
        *ppdispChild = nullptr;
        if (varChild.vt != VT_I4) return E_INVALIDARG;
        if (varChild.lVal == CHILDID_SELF) {
            AddRef();
            *ppdispChild = static_cast<IAccessible*>(this);
            return S_OK;
        }
        const std::vector<CControlUI*> kids = Children();
        const long i = varChild.lVal - 1;
        if (i < 0 || i >= static_cast<long>(kids.size())) return E_INVALIDARG;
        // Children are reported as simple elements (CHILDID_SELF handles), which
        // is what MSAA clients expect for owner-drawn controls.
        return S_FALSE;
    }

    STDMETHODIMP get_accName(VARIANT varChild, BSTR* pszName) override
    {
        if (pszName == nullptr) return E_POINTER;
        *pszName = nullptr;
        CControlUI* p = Resolve(varChild);
        if (p == nullptr) return E_INVALIDARG;

        CDuiString sName = p->GetName();
        if (sName.IsEmpty()) sName = p->GetText();
        if (sName.IsEmpty()) sName = p->GetClass();
        *pszName = ::SysAllocString(sName.GetData());
        return (*pszName != nullptr) ? S_OK : E_OUTOFMEMORY;
    }

    STDMETHODIMP get_accValue(VARIANT varChild, BSTR* pszValue) override
    {
        if (pszValue == nullptr) return E_POINTER;
        *pszValue = nullptr;
        CControlUI* p = Resolve(varChild);
        if (p == nullptr) return E_INVALIDARG;
        // Only text-bearing controls have a value; a button's "value" is its
        // label, which get_accName already returns.
        const CDuiString sText = p->GetText();
        if (sText.IsEmpty()) return S_FALSE;
        *pszValue = ::SysAllocString(sText.GetData());
        return (*pszValue != nullptr) ? S_OK : E_OUTOFMEMORY;
    }

    STDMETHODIMP get_accDescription(VARIANT, BSTR* pszDescription) override
    {
        if (pszDescription != nullptr) *pszDescription = nullptr;
        return S_FALSE;
    }

    STDMETHODIMP get_accRole(VARIANT varChild, VARIANT* pvarRole) override
    {
        if (pvarRole == nullptr) return E_POINTER;
        ::VariantInit(pvarRole);
        CControlUI* p = Resolve(varChild);
        if (p == nullptr) return E_INVALIDARG;
        pvarRole->vt = VT_I4;
        pvarRole->lVal = RoleOf(p);
        return S_OK;
    }

    STDMETHODIMP get_accState(VARIANT varChild, VARIANT* pvarState) override
    {
        if (pvarState == nullptr) return E_POINTER;
        ::VariantInit(pvarState);
        CControlUI* p = Resolve(varChild);
        if (p == nullptr) return E_INVALIDARG;
        long lState = 0;
        if (!p->IsVisible()) lState |= STATE_SYSTEM_INVISIBLE;
        if (!p->IsEnabled()) lState |= STATE_SYSTEM_UNAVAILABLE;
        if (p->IsFocused()) lState |= STATE_SYSTEM_FOCUSED;
        if ((p->GetControlFlags() & UIFLAG_TABSTOP) != 0) lState |= STATE_SYSTEM_FOCUSABLE;
        pvarState->vt = VT_I4;
        pvarState->lVal = lState;
        return S_OK;
    }

    STDMETHODIMP get_accHelp(VARIANT, BSTR* pszHelp) override
    {
        if (pszHelp != nullptr) *pszHelp = nullptr;
        return S_FALSE;
    }
    STDMETHODIMP get_accHelpTopic(BSTR*, VARIANT, long* pidTopic) override
    {
        if (pidTopic != nullptr) *pidTopic = 0;
        return S_FALSE;
    }
    STDMETHODIMP get_accKeyboardShortcut(VARIANT varChild, BSTR* pszShortcut) override
    {
        if (pszShortcut == nullptr) return E_POINTER;
        *pszShortcut = nullptr;
        CControlUI* p = Resolve(varChild);
        if (p == nullptr) return E_INVALIDARG;
        const TCHAR ch = p->GetShortcut();
        if (ch == 0) return S_FALSE;
        TCHAR sz[16] = {};
        ::swprintf_s(sz, _T("Alt+%c"), ch);
        *pszShortcut = ::SysAllocString(sz);
        return (*pszShortcut != nullptr) ? S_OK : E_OUTOFMEMORY;
    }
    STDMETHODIMP get_accFocus(VARIANT* pvarChild) override
    {
        if (pvarChild == nullptr) return E_POINTER;
        ::VariantInit(pvarChild);
        pvarChild->vt = VT_I4;
        if (m_pManager != nullptr) {
            CControlUI* pFocus = m_pManager->GetFocus();
            if (pFocus != nullptr) {
                const std::vector<CControlUI*> kids = Children();
                for (size_t i = 0; i < kids.size(); ++i) {
                    if (kids[i] == pFocus) { pvarChild->lVal = static_cast<long>(i) + 1; return S_OK; }
                }
            }
        }
        pvarChild->lVal = CHILDID_SELF;
        return S_OK;
    }
    STDMETHODIMP get_accSelection(VARIANT* pvarChildren) override
    {
        if (pvarChildren == nullptr) return E_POINTER;
        ::VariantInit(pvarChildren);
        pvarChildren->vt = VT_I4;
        pvarChildren->lVal = CHILDID_SELF;
        return S_OK;
    }
    STDMETHODIMP get_accDefaultAction(VARIANT varChild, BSTR* pszDefaultAction) override
    {
        if (pszDefaultAction == nullptr) return E_POINTER;
        *pszDefaultAction = nullptr;
        CControlUI* p = Resolve(varChild);
        if (p == nullptr) return E_INVALIDARG;
        const long lRole = RoleOf(p);
        if (lRole == ROLE_SYSTEM_PUSHBUTTON || lRole == ROLE_SYSTEM_RADIOBUTTON) {
            *pszDefaultAction = ::SysAllocString(L"Press");
            return (*pszDefaultAction != nullptr) ? S_OK : E_OUTOFMEMORY;
        }
        return S_FALSE;
    }
    STDMETHODIMP accSelect(long, VARIANT) override { return E_NOTIMPL; }
    STDMETHODIMP accLocation(long* pxLeft, long* pyTop, long* pcxWidth, long* pcyHeight, VARIANT varChild) override
    {
        CControlUI* p = Resolve(varChild);
        if (p == nullptr) return E_INVALIDARG;
        RECT rc = p->GetPos();
        // Control rects are relative to their parent; walk up to screen coords.
        for (CControlUI* q = p->GetParent(); q != nullptr; q = q->GetParent()) {
            RECT rcParent = q->GetPos();
            ::OffsetRect(&rc, rcParent.left, rcParent.top);
        }
        if (m_pManager != nullptr) {
            POINT pt = { rc.left, rc.top };
            ::ClientToScreen(m_pManager->GetPaintWindow(), &pt);
            const int cx = rc.right - rc.left;
            const int cy = rc.bottom - rc.top;
            rc.left = pt.x; rc.top = pt.y;
            rc.right = pt.x + cx; rc.bottom = pt.y + cy;
        }
        if (pxLeft != nullptr) *pxLeft = rc.left;
        if (pyTop != nullptr) *pyTop = rc.top;
        if (pcxWidth != nullptr) *pcxWidth = rc.right - rc.left;
        if (pcyHeight != nullptr) *pcyHeight = rc.bottom - rc.top;
        return S_OK;
    }
    STDMETHODIMP accNavigate(long, VARIANT, VARIANT* pvarEndUpAt) override
    {
        if (pvarEndUpAt != nullptr) ::VariantInit(pvarEndUpAt);
        return S_FALSE;
    }
    STDMETHODIMP accHitTest(long, long, VARIANT* pvarChild) override
    {
        if (pvarChild != nullptr) ::VariantInit(pvarChild);
        return S_FALSE;
    }
    STDMETHODIMP accDoDefaultAction(VARIANT varChild) override
    {
        CControlUI* p = Resolve(varChild);
        if (p == nullptr) return E_INVALIDARG;
        return p->Activate() ? S_OK : S_FALSE;
    }
    STDMETHODIMP put_accName(VARIANT, BSTR) override { return E_NOTIMPL; }
    STDMETHODIMP put_accValue(VARIANT, BSTR) override { return E_NOTIMPL; }

private:
    ~CUiAccessible() {}

    // Children of the node this object represents. The root object mirrors the
    // top-level controls of the window; a child object mirrors its subtree.
    std::vector<CControlUI*> Children() const
    {
        std::vector<CControlUI*> out;
        if (m_pControl != nullptr) {
            CContainerUI* pC = dynamic_cast<CContainerUI*>(m_pControl);
            if (pC != nullptr) {
                for (int i = 0; i < pC->GetCount(); ++i) {
                    CControlUI* p = pC->GetItemAt(i);
                    if (IsInteresting(p)) out.push_back(p);
                }
            }
            return out;
        }
        if (m_pManager != nullptr && m_pManager->GetRoot() != nullptr)
            CollectNodes(m_pManager->GetRoot(), out, 0);
        return out;
    }

    CControlUI* Resolve(VARIANT varChild) const
    {
        if (varChild.vt != VT_I4) return nullptr;
        if (varChild.lVal == CHILDID_SELF) return m_pControl;
        const std::vector<CControlUI*> kids = Children();
        const long i = varChild.lVal - 1;
        if (i < 0 || i >= static_cast<long>(kids.size())) return nullptr;
        return kids[i];
    }

    LONG m_cRef;
    CPaintManagerUI* m_pManager;
    CControlUI* m_pControl;   // nullptr == root object for the window
    int m_nChildId;
};

// [UIA-MULTI] One entry per paint manager.
//
// This used to be a single (g_pRootAcc, g_pOwnerManager) pair, which meant the second
// window to call Install() silently displaced the first and Uninstall() from either
// window cleared both. Keyed by the manager pointer so each window owns its own
// accessible root and can be torn down independently.
struct UiaManagerEntry
{
    CUiAccessible*   pRootAcc;   // owned reference
};
std::mutex g_uiaMutex;
std::map<CPaintManagerUI*, UiaManagerEntry> g_uiaManagers;

} // namespace

bool CUiAutomation::Install(CPaintManagerUI* pManager)
{
    if (pManager == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_uiaMutex);
    // Idempotent per manager; installing B no longer disturbs A.
    g_uiaManagers.emplace(pManager, UiaManagerEntry{ nullptr });
    return true;
}

// [UIA-ABI] Original exported symbol, unchanged signature: tear everything down.
void CUiAutomation::Uninstall()
{
    std::lock_guard<std::mutex> lock(g_uiaMutex);
    for (auto& kv : g_uiaManagers)
    {
        if (kv.second.pRootAcc != nullptr) kv.second.pRootAcc->Release();
    }
    g_uiaManagers.clear();
}

// Per-manager retirement; this is what CPaintManagerUI's destructor calls.
void CUiAutomation::Uninstall(CPaintManagerUI* pManager)
{
    if (pManager == nullptr) { Uninstall(); return; }
    std::lock_guard<std::mutex> lock(g_uiaMutex);
    auto it = g_uiaManagers.find(pManager);
    if (it != g_uiaManagers.end())
    {
        if (it->second.pRootAcc != nullptr) it->second.pRootAcc->Release();
        g_uiaManagers.erase(it);
    }
}

bool CUiAutomation::IsInstalled()
{
    std::lock_guard<std::mutex> lock(g_uiaMutex);
    return !g_uiaManagers.empty();
}

bool CUiAutomation::IsInstalled(CPaintManagerUI* pManager)
{
    std::lock_guard<std::mutex> lock(g_uiaMutex);
    return g_uiaManagers.find(pManager) != g_uiaManagers.end();
}


LRESULT CUiAutomation::OnGetObject(CPaintManagerUI* pManager, WPARAM wParam, LPARAM lParam, bool* pbHandled)
{
    if (pbHandled != nullptr) *pbHandled = false;
    if (pManager == nullptr) return 0;

    // Only the client object belongs to us; the non-client area stays with the
    // default window procedure.
    // [A11Y-4] The object id is in **lParam**, not wParam. WM_GETOBJECT is one of
    // the messages where wParam is documented as the "lParam alias" and carries 0;
    // checking wParam against OBJID_CLIENT therefore never matched, so this
    // function returned 0 for every request -- the MSAA bridge has never
    // answered a real client either. Measured: SendMessage(WM_GETOBJECT) logged
    // w=0x00000000 l=0xFFFFFFFC (OBJID_CLIENT) and l=0xFFFFFFE7
    // (UiaRootObjectId) and both returned 0.
    // Accept the id from EITHER slot. The documented slot for WM_GETOBJECT is
    // lParam (verified: SendMessage logged l=0xFFFFFFFC for OBJID_CLIENT and
    // l=0xFFFFFFE7 for UiaRootObjectId), but some callers -- including this
    // project's own a11y_probe -- put it in wParam. Reading both keeps a real
    // client working either way, and costs one comparison.
    const LONG nObjId = (static_cast<LONG>(lParam) != 0)
                        ? static_cast<LONG>(lParam)
                        : static_cast<LONG>(wParam);
    if (nObjId != OBJID_CLIENT && nObjId != UiaRootObjectId) return 0;

    // [A11Y-3] A UI Automation client asks for the root provider with
    // UiaRootObjectId, not IID_IAccessible. Answering only IID_IAccessible made
    // every UIA client see the window as a single opaque pane -- even though
    // this file's header has always documented the opposite. The provider lives
    // in UIAutomationProvider.cpp; here we only hand out its root object.
    if (nObjId == UiaRootObjectId)
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

    std::lock_guard<std::mutex> lock(g_uiaMutex);

    // [UIA-MULTI] Each manager owns its accessible root. Previously a single global
    // root was shared and swapped whenever a different window asked, so two windows
    // answering WM_GETOBJECT would release each other's IAccessible mid-use.
    CUiAccessible*& pRoot = g_uiaManagers[pManager].pRootAcc;
    if (pRoot == nullptr) pRoot = new CUiAccessible(pManager, nullptr, 0);

    LRESULT lRes = ::LresultFromObject(IID_IAccessible, static_cast<WPARAM>(lParam),
                                       static_cast<IAccessible*>(pRoot));
    if (pbHandled != nullptr) *pbHandled = true;
    return lRes;
}

void CUiAutomation::NotifyFocusChanged(CPaintManagerUI* pManager)
{
    if (pManager == nullptr || !IsInstalled()) return;
    ::NotifyWinEvent(EVENT_OBJECT_FOCUS, pManager->GetPaintWindow(), OBJID_CLIENT, CHILDID_SELF);
}

void CUiAutomation::NotifyStructureChanged(CPaintManagerUI* pManager)
{
    if (pManager == nullptr || !IsInstalled()) return;
    ::NotifyWinEvent(EVENT_OBJECT_REORDER, pManager->GetPaintWindow(), OBJID_CLIENT, CHILDID_SELF);
}

#else // !_UNICODE

// Inert but well-defined behaviour for MultiByte builds; see [UIA-MBCS] above.
// No accessible root is ever created, so nothing is registered or unregistered.

bool CUiAutomation::Install(CPaintManagerUI* /*pManager*/)
{
    return false;
}

void CUiAutomation::Uninstall()
{
}

void CUiAutomation::Uninstall(CPaintManagerUI* /*pManager*/)
{
}

bool CUiAutomation::IsInstalled()
{
    return false;
}

bool CUiAutomation::IsInstalled(CPaintManagerUI* /*pManager*/)
{
    return false;
}

LRESULT CUiAutomation::OnGetObject(CPaintManagerUI* /*pManager*/, WPARAM /*wParam*/,
                                  LPARAM /*lParam*/, bool* pbHandled)
{
    // Not ours: let the default window procedure answer WM_GETOBJECT.
    if (pbHandled != nullptr) *pbHandled = false;
    return 0;
}

void CUiAutomation::NotifyFocusChanged(CPaintManagerUI* /*pManager*/)
{
}

void CUiAutomation::NotifyStructureChanged(CPaintManagerUI* /*pManager*/)
{
}

#endif // _UNICODE

// [UIA-COORD] These two helpers are plain Win32 point/rect math and contain no
// TCHAR-dependent call, so they are compiled in every configuration -- keeping
// them inside the guard would have made DuiLib's own coordinate contract
// unavailable to a MultiByte build for no reason.

// [UIA-ABI] Original exported symbol, unchanged signature.
// [UIA-COORD] Client rect -> screen rect.
//
// Both corners are converted rather than adding a single origin: a window can live on
// a monitor whose client origin differs from the primary monitor's, and converting the
// corners keeps the result correct under per-monitor DPI without extra bookkeeping.
void CUiAutomation::ClientRectToScreen(HWND hWnd, const RECT& rcClient, RECT& rcScreen)
{
    rcScreen = rcClient;
    if (hWnd == nullptr || !::IsWindow(hWnd)) return;
    POINT ptTL = { rcClient.left, rcClient.top };
    POINT ptBR = { rcClient.right, rcClient.bottom };
    ::ClientToScreen(hWnd, &ptTL);
    ::ClientToScreen(hWnd, &ptBR);
    rcScreen.left = ptTL.x;
    rcScreen.top = ptTL.y;
    rcScreen.right = ptBR.x;
    rcScreen.bottom = ptBR.y;
}

void CUiAutomation::ScreenPointToClient(HWND hWnd, POINT& pt)
{
    if (hWnd == nullptr || !::IsWindow(hWnd)) return;
    ::ScreenToClient(hWnd, &pt);
}

} // namespace DuiLib
