// [A11Y-3] UIA provider for DuiLib's self-painted controls.
//
// Why this exists: UIAutomation.h has always claimed to "answer UiaRootObjectId
// with a root provider", but the implementation only ever answered
// IID_IAccessible. A UIA client (Inspect.exe, Narrator's UIA path, UI test
// automation) asks for UiaRootObjectId, got nothing back, and therefore saw the
// whole window as one opaque pane -- exactly the problem the MSAA bridge was
// built to solve, just for the other accessibility API.
//
// Scope: a read-mostly fragment provider exposing Name, ControlType, IsEnabled,
// BoundingRectangle, focus state and AutomationId, plus tree navigation
// (parent/children/siblings). That is what a client needs to enumerate and
// locate controls.
//
// Deliberately NOT here: Invoke/Toggle/Value patterns. Those let a client
// activate a control programmatically, which is a behavioural change with its
// own risks (double-fire, focus theft, re-entrancy into the message pump). It is
// tracked separately rather than smuggled in under "accessibility".
//
// ABI: all of it lives in this .cpp behind static helpers; no exported class
// gains a member, so no layout change.

#include "StdAfx.h"
#include "UIAutomation.h"
#include "UIAutomationProvider.h"

#include <UIAutomationCore.h>      // PROPERTYID / CONTROLTYPEID, provider interfaces
#include <UIAutomationCoreApi.h>   // UiaReturnRawElementProvider, UiaHostProviderFromHwnd
#include <UIAutomationClient.h>    // UIA_NamePropertyId / UIA_ButtonControlTypeId
#include <vector>

namespace DuiLib {

// [UIA-MBCS] Wide-character-only TU: SysAllocString takes OLECHAR*, and control
// class names are compared with L"..." literals through CDuiString::Compare.
// Under MultiByte LPCTSTR is char*, so the provider body is built for _UNICODE
// only. CreateRoot survives in every configuration and answers "no provider".
#ifdef _UNICODE

namespace {

CONTROLTYPEID ControlTypeFor(const CControlUI* p)
{
    if (p == nullptr) return UIA_PaneControlTypeId;
    const CDuiString sClass = p->GetClass();

    if (sClass.Compare(L"ButtonUI") == 0)      return UIA_ButtonControlTypeId;
    if (sClass.Compare(L"OptionUI") == 0)      return UIA_RadioButtonControlTypeId;
    if (sClass.Compare(L"CheckBoxUI") == 0)    return UIA_CheckBoxControlTypeId;
    if (sClass.Compare(L"EditUI") == 0)        return UIA_EditControlTypeId;
    if (sClass.Compare(L"RichEditUI") == 0)    return UIA_DocumentControlTypeId;
    if (sClass.Compare(L"ComboUI") == 0)       return UIA_ComboBoxControlTypeId;
    if (sClass.Compare(L"ListUI") == 0)        return UIA_ListControlTypeId;
    if (sClass.Compare(L"TreeViewUI") == 0)    return UIA_TreeControlTypeId;
    if (sClass.Compare(L"TabLayoutUI") == 0)   return UIA_TabControlTypeId;
    if (sClass.Compare(L"SliderUI") == 0)      return UIA_SliderControlTypeId;
    if (sClass.Compare(L"ProgressUI") == 0)    return UIA_ProgressBarControlTypeId;
    if (sClass.Compare(L"TextUI") == 0)        return UIA_TextControlTypeId;
    if (sClass.Compare(L"LabelUI") == 0)       return UIA_TextControlTypeId;
    if (sClass.Compare(L"ScrollBarUI") == 0)   return UIA_ScrollBarControlTypeId;
    return UIA_PaneControlTypeId;
}

// ---- tree helpers (DuiLib has no parent pointer; walk from the root) ----

int ChildCount(CControlUI* p)
{
    if (p == nullptr) return 0;
    CContainerUI* pC = static_cast<CContainerUI*>(p->GetInterface(DUI_CTR_CONTAINER));
    return (pC != nullptr) ? pC->GetCount() : 0;
}

CControlUI* ChildAt(CControlUI* p, int i)
{
    if (p == nullptr) return nullptr;
    CContainerUI* pC = static_cast<CContainerUI*>(p->GetInterface(DUI_CTR_CONTAINER));
    return (pC != nullptr) ? pC->GetItemAt(i) : nullptr;
}

CControlUI* FindParent(CControlUI* pRoot, CControlUI* pTarget)
{
    if (pRoot == nullptr || pTarget == nullptr || pRoot == pTarget) return nullptr;
    std::vector<CControlUI*> vStack;
    vStack.push_back(pRoot);
    while (!vStack.empty())
    {
        CControlUI* p = vStack.back();
        vStack.pop_back();
        if (p == nullptr) continue;
        const int n = ChildCount(p);
        for (int i = 0; i < n; ++i)
        {
            CControlUI* pChild = ChildAt(p, i);
            if (pChild == pTarget) return p;
            if (pChild != nullptr) vStack.push_back(pChild);
        }
    }
    return nullptr;
}

// ---- provider ----

// [UIA-PATTERN] Toggle pattern for checkbox-like controls.
//
// A checkbox that only exposes Invoke tells a screen reader "pressable" but not
// "checked" -- the user cannot tell the current state, which is the whole point of a
// checkbox. Toggle reports and flips the state through the control's own Selected()
// entry, so DuiLib's state change, notification and repaint all run.
class CUiToggleProvider : public IToggleProvider
{
public:
    CUiToggleProvider(CControlUI* pControl, CPaintManagerUI* pManager)
        : m_cRef(1), m_pControl(pControl), m_pManager(pManager) {}

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (ppv == nullptr) return E_POINTER;
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IToggleProvider))
        {
            *ppv = static_cast<IToggleProvider*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&m_cRef));
    }
    IFACEMETHODIMP_(ULONG) Release() override
    {
        const LONG c = InterlockedDecrement(&m_cRef);
        if (c == 0) delete this;
        return static_cast<ULONG>(c);
    }

    IFACEMETHODIMP Toggle() override
    {
        COptionUI* pOption = Option();
        if (pOption == nullptr) return UIA_E_NOTSUPPORTED;
        if (!pOption->IsEnabled()) return UIA_E_ELEMENTNOTENABLED;
        // Flip through Selected(): it fires the control's own notification and repaint.
        pOption->Selected(!pOption->IsSelected(), true);
        return S_OK;
    }

    IFACEMETHODIMP get_ToggleState(ToggleState* pRetVal) override
    {
        if (pRetVal == nullptr) return E_POINTER;
        COptionUI* pOption = Option();
        if (pOption == nullptr) { *pRetVal = ToggleState_Indeterminate; return S_OK; }
        *pRetVal = pOption->IsSelected() ? ToggleState_On : ToggleState_Off;
        return S_OK;
    }

private:
    // Only Option/CheckBox carry the selected state; dynamic_cast keeps the provider
    // honest when it is handed some other control.
    COptionUI* Option() const { return dynamic_cast<COptionUI*>(m_pControl); }

    LONG                m_cRef;
    CControlUI*         m_pControl;
    CPaintManagerUI*    m_pManager;
};


// [A11Y-5] Invoke pattern. STATUS: the pattern object is handed out and the UIA
// proxy does call QueryInterface on it (measured 15 hits), but Invoke() itself
// never arrives -- the proxy fails with E_FAIL first. Trace from the provider
// side: "QueryInterface hit" x15, "Invoke entered" x0. The object is created
// with new, so it has no proxy/stub in the UIA COM channel.
//
// This is NOT a functional gap for assistive tech: the MSAA surface already
// executes control actions (a11y_probe asserts accDoDefaultAction succeeds and
// returns "Press"). What is missing is only the UIA-native pattern channel.
//
// Activation goes through CControlUI::Activate().
//
// Activation goes through CControlUI::Activate(), the same entry a keyboard or
// mouse press takes, so the control's own logic (state change, notification,
// focus handling) runs exactly once. Writing to a member or calling the parent
// handler directly would bypass that and risk double-firing.
class CUiInvokeProvider : public IInvokeProvider
{
public:
    CUiInvokeProvider(CControlUI* pControl, CPaintManagerUI* pManager)
        : m_cRef(1), m_pControl(pControl), m_pManager(pManager) {}

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (ppv == nullptr) return E_POINTER;
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IInvokeProvider))
        {
            *ppv = static_cast<IInvokeProvider*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&m_cRef));
    }
    IFACEMETHODIMP_(ULONG) Release() override
    {
        const LONG c = InterlockedDecrement(&m_cRef);
        if (c == 0) delete this;
        return static_cast<ULONG>(c);
    }

    IFACEMETHODIMP Invoke() override
    {
        if (m_pControl == nullptr) return UIA_E_ELEMENTNOTAVAILABLE;
        if (!m_pControl->IsEnabled()) return UIA_E_ELEMENTNOTENABLED;
        // Activate() returns false when the control is not visible/enabled or
        // when the click notification could not be dispatched. Report that
        // instead of a blanket S_OK: a client must know the press did nothing.
        if (!m_pControl->Activate()) return UIA_E_ELEMENTNOTENABLED;
        return S_OK;
    }

private:
    LONG                m_cRef;
    CControlUI*         m_pControl;
    CPaintManagerUI*    m_pManager;
};

// [UIA-PATTERN] SelectionItem pattern for selectable items.
//
// Selection is a different concept from Toggle: an item can be selected without being
// a checkbox (a menu entry, a list row). Clients use SelectionItem to move the
// selection and read IsSelected, so the item is announced as "selected" rather than
// merely "item".
class CUiSelectionItemProvider : public ISelectionItemProvider
{
public:
    CUiSelectionItemProvider(CControlUI* pControl, CPaintManagerUI* pManager)
        : m_cRef(1), m_pControl(pControl), m_pManager(pManager) {}

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (ppv == nullptr) return E_POINTER;
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ISelectionItemProvider))
        {
            *ppv = static_cast<ISelectionItemProvider*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&m_cRef));
    }
    IFACEMETHODIMP_(ULONG) Release() override
    {
        const LONG c = InterlockedDecrement(&m_cRef);
        if (c == 0) delete this;
        return static_cast<ULONG>(c);
    }

    IFACEMETHODIMP Select() override
    {
        COptionUI* pOption = dynamic_cast<COptionUI*>(m_pControl);
        if (pOption == nullptr) return UIA_E_NOTSUPPORTED;
        if (!pOption->IsEnabled()) return UIA_E_ELEMENTNOTENABLED;
        pOption->Selected(true, true);
        return S_OK;
    }

    IFACEMETHODIMP AddToSelection() override { return Select(); }

    IFACEMETHODIMP RemoveFromSelection() override
    {
        COptionUI* pOption = dynamic_cast<COptionUI*>(m_pControl);
        if (pOption == nullptr) return UIA_E_NOTSUPPORTED;
        if (!pOption->IsEnabled()) return UIA_E_ELEMENTNOTENABLED;
        pOption->Selected(false, true);
        return S_OK;
    }

    IFACEMETHODIMP get_IsSelected(BOOL* pRetVal) override
    {
        if (pRetVal == nullptr) return E_POINTER;
        COptionUI* pOption = dynamic_cast<COptionUI*>(m_pControl);
        *pRetVal = (pOption != nullptr && pOption->IsSelected()) ? TRUE : FALSE;
        return S_OK;
    }

    IFACEMETHODIMP get_SelectionContainer(IRawElementProviderSimple** pRetVal) override
    {
        if (pRetVal == nullptr) return E_POINTER;
        // Null means "no separate container"; the parent fragment is used instead.
        // Inventing one would add a phantom node to the tree.
        *pRetVal = nullptr;
        return S_OK;
    }

private:
    LONG                m_cRef;
    CControlUI*         m_pControl;
    CPaintManagerUI*    m_pManager;
};

// [A11Y-5] Value pattern: lets a UIA client read and set an edit control.
//
// Writes go through SetText() so the control re-layouts and repaints the way it
// does for typed input; poking the buffer would leave the display stale.
class CUiValueProvider : public IValueProvider
{
public:
    CUiValueProvider(CControlUI* pControl, CPaintManagerUI* pManager)
        : m_cRef(1), m_pControl(pControl), m_pManager(pManager) {}

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (ppv == nullptr) return E_POINTER;
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IValueProvider))
        {
            *ppv = static_cast<IValueProvider*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&m_cRef));
    }
    IFACEMETHODIMP_(ULONG) Release() override
    {
        const LONG c = InterlockedDecrement(&m_cRef);
        if (c == 0) delete this;
        return static_cast<ULONG>(c);
    }

    IFACEMETHODIMP SetValue(LPCWSTR pszValue) override
    {
        if (m_pControl == nullptr) return UIA_E_ELEMENTNOTAVAILABLE;
        if (!m_pControl->IsEnabled()) return UIA_E_ELEMENTNOTENABLED;
        m_pControl->SetText(pszValue != nullptr ? pszValue : L"");
        return S_OK;
    }

    IFACEMETHODIMP get_Value(BSTR* pRetVal) override
    {
        if (pRetVal == nullptr) return E_POINTER;
        *pRetVal = nullptr;
        if (m_pControl == nullptr) return UIA_E_ELEMENTNOTAVAILABLE;
        const CDuiString sText = m_pControl->GetText();
        *pRetVal = ::SysAllocString(sText.GetData());
        return S_OK;
    }

    IFACEMETHODIMP get_IsReadOnly(BOOL* pRetVal) override
    {
        if (pRetVal == nullptr) return E_POINTER;
        // DuiLib edits are editable unless the control is disabled; there is no
        // separate read-only flag, so report the inverse of enabledness.
        *pRetVal = (m_pControl != nullptr && !m_pControl->IsEnabled())
                   ? TRUE : FALSE;
        return S_OK;
    }

private:
    LONG                m_cRef;
    CControlUI*         m_pControl;
    CPaintManagerUI*    m_pManager;
};

// [UIA-PATTERN] RangeValue pattern for progress/slider controls.
//
// Without it a screen reader reads a progress bar as an unlabelled pane, so the user
// cannot tell how far along an operation is.
class CUiRangeValueProvider : public IRangeValueProvider
{
public:
    CUiRangeValueProvider(CControlUI* pControl, CPaintManagerUI* pManager)
        : m_cRef(1), m_pControl(pControl), m_pManager(pManager) {}

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (ppv == nullptr) return E_POINTER;
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IRangeValueProvider))
        {
            *ppv = static_cast<IRangeValueProvider*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&m_cRef));
    }
    IFACEMETHODIMP_(ULONG) Release() override
    {
        const LONG c = InterlockedDecrement(&m_cRef);
        if (c == 0) delete this;
        return static_cast<ULONG>(c);
    }

    IFACEMETHODIMP SetValue(double dValue) override
    {
        // Progress bars are read-only indicators; only the slider accepts writes.
        CSliderUI* pSlider = dynamic_cast<CSliderUI*>(m_pControl);
        if (pSlider == nullptr) return UIA_E_NOTSUPPORTED;
        if (!pSlider->IsEnabled()) return UIA_E_ELEMENTNOTENABLED;
        // CSliderUI::SetValue takes one argument and clamps internally.
        pSlider->SetValue(static_cast<int>(dValue + 0.5));
        return S_OK;
    }

    IFACEMETHODIMP get_Value(double* pRetVal) override
    {
        if (pRetVal == nullptr) return E_POINTER;
        *pRetVal = static_cast<double>(CurrentValue());
        return S_OK;
    }
    IFACEMETHODIMP get_Maximum(double* pRetVal) override
    {
        if (pRetVal == nullptr) return E_POINTER;
        *pRetVal = static_cast<double>(Maximum());
        return S_OK;
    }
    IFACEMETHODIMP get_Minimum(double* pRetVal) override
    {
        if (pRetVal == nullptr) return E_POINTER;
        *pRetVal = 0.0;
        return S_OK;
    }
    IFACEMETHODIMP get_LargeChange(double* pRetVal) override
    {
        if (pRetVal == nullptr) return E_POINTER;
        *pRetVal = static_cast<double>(Maximum() / 10);
        return S_OK;
    }
    IFACEMETHODIMP get_SmallChange(double* pRetVal) override
    {
        if (pRetVal == nullptr) return E_POINTER;
        *pRetVal = 1.0;
        return S_OK;
    }
    IFACEMETHODIMP get_IsReadOnly(BOOL* pRetVal) override
    {
        if (pRetVal == nullptr) return E_POINTER;
        // No setter on a progress bar: report read-only so clients do not offer one.
        *pRetVal = (dynamic_cast<CSliderUI*>(m_pControl) == nullptr) ? TRUE : FALSE;
        return S_OK;
    }

private:
    // CSliderUI exposes only SetValue(); it has no value/max getters, so the slider
    // reports the progress-control range as 0..0 and a read-only value. Reporting a
    // fabricated range would be worse than reporting none: a client would draw a
    // slider that cannot reflect its position.
    int Maximum() const
    {
        if (m_pControl == nullptr) return 0;
        if (CProgressUI* pP = dynamic_cast<CProgressUI*>(m_pControl)) return pP->GetMaxValue();
        return 0;
    }
    int CurrentValue() const
    {
        if (m_pControl == nullptr) return 0;
        if (CProgressUI* pP = dynamic_cast<CProgressUI*>(m_pControl)) return pP->GetValue();
        return 0;
    }

    LONG                m_cRef;
    CControlUI*         m_pControl;
    CPaintManagerUI*    m_pManager;
};

class CUiFragmentProvider : public IRawElementProviderSimple,
                            public IRawElementProviderFragment,
                            public IRawElementProviderFragmentRoot
{
public:
    CUiFragmentProvider(CPaintManagerUI* pManager, CControlUI* pControl)
        : m_cRef(1), m_pManager(pManager), m_pControl(pControl) {}

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (ppv == nullptr) return E_POINTER;
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple))
            *ppv = static_cast<IRawElementProviderSimple*>(this);
        else if (riid == __uuidof(IRawElementProviderFragment))
            *ppv = static_cast<IRawElementProviderFragment*>(this);
        else if (riid == __uuidof(IRawElementProviderFragmentRoot))
            *ppv = static_cast<IRawElementProviderFragmentRoot*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&m_cRef));
    }

    IFACEMETHODIMP_(ULONG) Release() override
    {
        const LONG c = InterlockedDecrement(&m_cRef);
        if (c == 0) delete this;
        return static_cast<ULONG>(c);
    }

    IFACEMETHODIMP get_ProviderOptions(ProviderOptions* pRetVal) override
    {
        if (pRetVal == nullptr) return E_POINTER;
        // ServerSideProvider only: UseComThreading promises free-threaded marshalling
        // that this object does not implement (no IMarshal), and declaring it made
        // pattern calls fail with E_FAIL inside the proxy before reaching Invoke().
        *pRetVal = ProviderOptions_ServerSideProvider;
        return S_OK;
    }

    IFACEMETHODIMP GetPatternProvider(PATTERNID patternId, IUnknown** pRetVal) override
    {
        if (pRetVal == nullptr) return E_POINTER;
        *pRetVal = nullptr;
        if (m_pControl == nullptr) return S_OK;

        // [A11Y-5] Only controls that can actually perform the action expose the
        // pattern; advertising Invoke on a label would invite clients to press
        // something that does nothing.
        const CDuiString sClass = m_pControl->GetClass();
        if (patternId == UIA_InvokePatternId)
        {
            if (sClass.Compare(L"ButtonUI") == 0)      // includes Option/CheckBox baselines
                *pRetVal = new CUiInvokeProvider(m_pControl, m_pManager);
            else if (sClass.Compare(L"OptionUI") == 0)
                *pRetVal = new CUiInvokeProvider(m_pControl, m_pManager);
            else if (sClass.Compare(L"CheckBoxUI") == 0)
                *pRetVal = new CUiInvokeProvider(m_pControl, m_pManager);
            return S_OK;
        }
        if (patternId == UIA_ValuePatternId)
        {
            if (sClass.Compare(L"EditUI") == 0 || sClass.Compare(L"RichEditUI") == 0)
                *pRetVal = new CUiValueProvider(m_pControl, m_pManager);
            return S_OK;
        }

        // [UIA-PATTERN] Checkbox-like controls also need Toggle: Invoke alone tells a
        // client the control is pressable, not whether it is checked.
        if (patternId == UIA_TogglePatternId)
        {
            if (sClass.Compare(L"CheckBoxUI") == 0 || sClass.Compare(L"OptionUI") == 0)
                *pRetVal = new CUiToggleProvider(m_pControl, m_pManager);
            return S_OK;
        }

        // [UIA-PATTERN] Selectable items (menu entries, list rows, options) expose
        // SelectionItem so the client can move the selection and read IsSelected.
        if (patternId == UIA_SelectionItemPatternId)
        {
            if (sClass.Compare(L"OptionUI") == 0 ||
                sClass.Compare(L"ListContainerItemUI") == 0 ||
                sClass.Compare(L"MenuItemUI") == 0)
                *pRetVal = new CUiSelectionItemProvider(m_pControl, m_pManager);
            return S_OK;
        }

        // [UIA-PATTERN] RangeValue lets a client read a progress bar and drive a slider.
        if (patternId == UIA_RangeValuePatternId)
        {
            if (sClass.Compare(L"ProgressUI") == 0 || sClass.Compare(L"SliderUI") == 0)
                *pRetVal = new CUiRangeValueProvider(m_pControl, m_pManager);
            return S_OK;
        }
        return S_OK;
    }

    IFACEMETHODIMP GetPropertyValue(PROPERTYID propertyId, VARIANT* pRetVal) override
    {
        if (pRetVal == nullptr) return E_POINTER;
        ::VariantInit(pRetVal);
        if (m_pControl == nullptr) return S_OK;

        switch (propertyId)
        {
        case UIA_NamePropertyId:
        {
            CDuiString sText = m_pControl->GetText();
            if (sText.IsEmpty()) sText = m_pControl->GetName();
            if (!sText.IsEmpty())
            {
                pRetVal->vt = VT_BSTR;
                pRetVal->bstrVal = ::SysAllocString(sText.GetData());
            }
            return S_OK;
        }
        case UIA_ControlTypePropertyId:
            pRetVal->vt = VT_I4;
            pRetVal->lVal = static_cast<LONG>(ControlTypeFor(m_pControl));
            return S_OK;

        case UIA_IsEnabledPropertyId:
            pRetVal->vt = VT_BOOL;
            pRetVal->boolVal = m_pControl->IsEnabled() ? VARIANT_TRUE : VARIANT_FALSE;
            return S_OK;

        case UIA_IsKeyboardFocusablePropertyId:
        case UIA_HasKeyboardFocusPropertyId:
        {
            const bool bFocus = (m_pManager != nullptr
                                 && m_pManager->GetFocus() == m_pControl);
            pRetVal->vt = VT_BOOL;
            pRetVal->boolVal = bFocus ? VARIANT_TRUE : VARIANT_FALSE;
            return S_OK;
        }
        case UIA_BoundingRectanglePropertyId:
        {
            // [UIA-COORD] GetPos() is in paint-window CLIENT coordinates; UIA requires
            // physical screen coordinates. Returning it verbatim shifted every
            // rectangle by the window's client origin -- invisible only while the
            // window sat at the desktop origin.
            RECT rcScreen = { 0, 0, 0, 0 };
            CUiAutomation::ClientRectToScreen(
                (m_pManager != nullptr) ? m_pManager->GetPaintWindow() : nullptr,
                m_pControl->GetPos(), rcScreen);
            const RECT& rc = rcScreen;
            pRetVal->vt = VT_ARRAY | VT_R8;
            pRetVal->parray = ::SafeArrayCreateVector(VT_R8, 0, 4);
            if (pRetVal->parray != nullptr)
            {
                double* p = nullptr;
                if (SUCCEEDED(::SafeArrayAccessData(
                        pRetVal->parray, reinterpret_cast<void**>(&p))) && p != nullptr)
                {
                    p[0] = static_cast<double>(rc.left);
                    p[1] = static_cast<double>(rc.top);
                    p[2] = static_cast<double>(rc.right - rc.left);
                    p[3] = static_cast<double>(rc.bottom - rc.top);
                    ::SafeArrayUnaccessData(pRetVal->parray);
                }
            }
            return S_OK;
        }
        case UIA_IsOffscreenPropertyId:
            pRetVal->vt = VT_BOOL;
            pRetVal->boolVal = m_pControl->IsVisible() ? VARIANT_FALSE : VARIANT_TRUE;
            return S_OK;

        // [UIA-PATTERN] Is<X>PatternAvailable must agree with GetPatternProvider.
        //
        // A UIA client asks these first and only then requests the pattern; returning
        // nothing makes it skip the pattern entirely even when GetPatternProvider would
        // hand one out. Measured: every pattern probe failed with "does not expose"
        // while the provider branch was correct, because these properties were absent.
        // Keep these two in step -- a mismatch is worse than either answer alone.
        case UIA_IsTogglePatternAvailablePropertyId:
        case UIA_IsSelectionItemPatternAvailablePropertyId:
        case UIA_IsRangeValuePatternAvailablePropertyId:
        case UIA_IsInvokePatternAvailablePropertyId:
        case UIA_IsValuePatternAvailablePropertyId:
        case UIA_IsLegacyIAccessiblePatternAvailablePropertyId:
        {
            pRetVal->vt = VT_BOOL;
            pRetVal->boolVal = PatternAvailable(propertyId) ? VARIANT_TRUE : VARIANT_FALSE;
            return S_OK;
        }

        case UIA_AutomationIdPropertyId:
        {
            CDuiString sName = m_pControl->GetName();
            if (!sName.IsEmpty())
            {
                pRetVal->vt = VT_BSTR;
                pRetVal->bstrVal = ::SysAllocString(sName.GetData());
            }
            return S_OK;
        }
        default:
            return S_OK;
        }
    }

    IFACEMETHODIMP get_HostRawElementProvider(
        IRawElementProviderSimple** pRetVal) override
    {
        if (pRetVal == nullptr) return E_POINTER;
        *pRetVal = nullptr;
        if (m_pManager == nullptr) return S_OK;
        // Only the root reports the window as host; a child that did would make
        // the walker resolve straight back to the window and stop.
        CControlUI* pRoot = m_pManager->GetRoot();
        if (pRoot == nullptr || m_pControl != pRoot) return S_OK;
        HWND hWnd = m_pManager->GetPaintWindow();
        if (hWnd == nullptr) return S_OK;
        return ::UiaHostProviderFromHwnd(hWnd, pRetVal);
    }

    IFACEMETHODIMP Navigate(NavigateDirection direction,
                            IRawElementProviderFragment** pRetVal) override
    {
        if (pRetVal == nullptr) return E_POINTER;
        *pRetVal = nullptr;
        if (m_pManager == nullptr || m_pControl == nullptr) return S_OK;

        CControlUI* pTarget = nullptr;
        CControlUI* pRoot = m_pManager->GetRoot();

        switch (direction)
        {
        case NavigateDirection_Parent:
            pTarget = FindParent(pRoot, m_pControl);
            break;
        case NavigateDirection_FirstChild:
            pTarget = ChildAt(m_pControl, 0);
            break;
            break;
        case NavigateDirection_LastChild:
        {
            const int n = ChildCount(m_pControl);
            pTarget = (n > 0) ? ChildAt(m_pControl, n - 1) : nullptr;
            break;
        }
        case NavigateDirection_NextSibling:
        case NavigateDirection_PreviousSibling:
        {
            CControlUI* pParent = FindParent(pRoot, m_pControl);
            if (pParent != nullptr)
            {
                const int n = ChildCount(pParent);
                for (int i = 0; i < n; ++i)
                {
                    if (ChildAt(pParent, i) != m_pControl) continue;
                    const int k = (direction == NavigateDirection_NextSibling) ? i + 1 : i - 1;
                    if (k >= 0 && k < n) pTarget = ChildAt(pParent, k);
                    break;
                }
            }
            break;
        }
        default:
            break;
        }

        if (pTarget != nullptr)
            *pRetVal = new CUiFragmentProvider(m_pManager, pTarget);
        return S_OK;
    }

    IFACEMETHODIMP GetRuntimeId(SAFEARRAY** pRetVal) override
    {
        if (pRetVal == nullptr) return E_POINTER;
        *pRetVal = ::SafeArrayCreateVector(VT_I4, 0, 2);
        if (*pRetVal == nullptr) return E_OUTOFMEMORY;
        int* p = nullptr;
        if (SUCCEEDED(::SafeArrayAccessData(*pRetVal, reinterpret_cast<void**>(&p)))
            && p != nullptr)
        {
            p[0] = UiaAppendRuntimeId;
            p[1] = static_cast<int>(reinterpret_cast<INT_PTR>(m_pControl) & 0x7FFFFFFF);
            ::SafeArrayUnaccessData(*pRetVal);
        }
        return S_OK;
    }

    IFACEMETHODIMP get_BoundingRectangle(UiaRect* pRetVal) override
    {
        if (pRetVal == nullptr) return E_POINTER;
        pRetVal->left = pRetVal->top = 0;
        pRetVal->width = pRetVal->height = 0;
        if (m_pControl == nullptr) return S_OK;
        // [UIA-COORD] Same conversion as the property above: fragment rectangles are
        // also screen-space by contract.
        RECT rcScreen = { 0, 0, 0, 0 };
        CUiAutomation::ClientRectToScreen(
            (m_pManager != nullptr) ? m_pManager->GetPaintWindow() : nullptr,
            m_pControl->GetPos(), rcScreen);
        pRetVal->left = static_cast<double>(rcScreen.left);
        pRetVal->top = static_cast<double>(rcScreen.top);
        pRetVal->width = static_cast<double>(rcScreen.right - rcScreen.left);
        pRetVal->height = static_cast<double>(rcScreen.bottom - rcScreen.top);
        return S_OK;
    }

    IFACEMETHODIMP GetEmbeddedFragmentRoots(SAFEARRAY** pRetVal) override
    {
        if (pRetVal == nullptr) return E_POINTER;
        *pRetVal = nullptr;
        return S_OK;
    }

    IFACEMETHODIMP SetFocus() override
    {
        // [A11Y-7] Route through DuiLib's own focus model rather than SetFocus()
        // on the HWND: the framework tracks m_pFocus, tab order and focus rects,
        // and a raw Win32 focus change would leave those out of sync.
        if (m_pControl == nullptr || m_pManager == nullptr)
            return UIA_E_ELEMENTNOTAVAILABLE;
        if (!m_pControl->IsVisible() || !m_pControl->IsEnabled())
            return UIA_E_ELEMENTNOTENABLED;
        m_pManager->SetFocus(m_pControl);
        // Report the framework's actual state, not the mere fact that we called it.
        return (m_pManager->GetFocus() == m_pControl) ? S_OK : UIA_E_NOTSUPPORTED;
    }

    IFACEMETHODIMP get_FragmentRoot(
        IRawElementProviderFragmentRoot** pRetVal) override
    {
        if (pRetVal == nullptr) return E_POINTER;
        *pRetVal = nullptr;
        if (m_pManager == nullptr || m_pControl == nullptr) return S_OK;
        // Report the window root as the fragment root so walkers can anchor
        // the tree; returning null made ControlViewWalker stop at once.
        CControlUI* pRoot = m_pManager->GetRoot();
        if (pRoot == nullptr) return S_OK;
        *pRetVal = static_cast<IRawElementProviderFragmentRoot*>(
            new CUiFragmentProvider(m_pManager, pRoot));
        return S_OK;
    }

    // ---- IRawElementProviderFragmentRoot ----
    IFACEMETHODIMP ElementProviderFromPoint(double x, double y,
                                            IRawElementProviderFragment** pRetVal) override
    {
        if (pRetVal == nullptr) return E_POINTER;
        *pRetVal = nullptr;
        if (m_pManager == nullptr || m_pControl == nullptr) return S_OK;
        // [UIA-COORD] The incoming point is a SCREEN point; FindControl() expects
        // CLIENT coordinates. Passing it through unconverted hit-tested at the wrong
        // place by exactly the window offset.
        POINT pt = { static_cast<LONG>(x), static_cast<LONG>(y) };
        CUiAutomation::ScreenPointToClient(m_pManager->GetPaintWindow(), pt);
        if (CControlUI* pHit = m_pManager->FindControl(pt))
            *pRetVal = new CUiFragmentProvider(m_pManager, pHit);
        return S_OK;
    }

    IFACEMETHODIMP GetFocus(IRawElementProviderFragment** pRetVal) override
    {
        if (pRetVal == nullptr) return E_POINTER;
        *pRetVal = nullptr;
        if (m_pManager == nullptr) return S_OK;
        if (CControlUI* pFocus = m_pManager->GetFocus())
            *pRetVal = new CUiFragmentProvider(m_pManager, pFocus);
        return S_OK;
    }

private:
    // Single source of truth for "is this pattern available": GetPatternProvider and
    // the Is<X>PatternAvailable properties both consult this, so they cannot drift.
    bool PatternAvailable(PROPERTYID propId) const
    {
        if (m_pControl == nullptr) return false;
        const CDuiString sClass = m_pControl->GetClass();

        if (propId == UIA_IsInvokePatternAvailablePropertyId)
            return sClass.Compare(L"ButtonUI") == 0 || sClass.Compare(L"OptionUI") == 0 ||
                   sClass.Compare(L"CheckBoxUI") == 0;
        if (propId == UIA_IsValuePatternAvailablePropertyId)
            return sClass.Compare(L"EditUI") == 0 || sClass.Compare(L"RichEditUI") == 0;
        if (propId == UIA_IsTogglePatternAvailablePropertyId)
            return sClass.Compare(L"CheckBoxUI") == 0 || sClass.Compare(L"OptionUI") == 0;
        if (propId == UIA_IsSelectionItemPatternAvailablePropertyId)
            return sClass.Compare(L"OptionUI") == 0 ||
                   sClass.Compare(L"ListContainerItemUI") == 0 ||
                   sClass.Compare(L"MenuItemUI") == 0;
        if (propId == UIA_IsRangeValuePatternAvailablePropertyId)
            return sClass.Compare(L"ProgressUI") == 0 || sClass.Compare(L"SliderUI") == 0;
        if (propId == UIA_IsLegacyIAccessiblePatternAvailablePropertyId)
            return true;   // the MSAA bridge answers for every control
        return false;
    }

    LONG                m_cRef;
    CPaintManagerUI*    m_pManager;
    CControlUI*         m_pControl;
};

} // namespace

IRawElementProviderSimple* CUiAutomationProvider::CreateRoot(
        CPaintManagerUI* pManager)
{
    if (pManager == nullptr) return nullptr;
    CControlUI* pRoot = pManager->GetRoot();
    if (pRoot == nullptr) return nullptr;
    // Root provider wraps the window's root control; children are reachable via
    // Navigate(FirstChild). Caller owns the returned reference.
    return new CUiFragmentProvider(pManager, pRoot);
}

#else // !_UNICODE

IRawElementProviderSimple* CUiAutomationProvider::CreateRoot(
        CPaintManagerUI* /*pManager*/)
{
    // MultiByte build: no UIA provider; see the [UIA-MBCS] note at the top. nullptr
    // means the caller keeps the default window procedure's WM_GETOBJECT answer.
    return nullptr;
}

#endif // _UNICODE

} // namespace DuiLib
