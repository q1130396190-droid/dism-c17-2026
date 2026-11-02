import pathlib

p = pathlib.Path('DuiLib/Utils/UIAutomationProvider.cpp')
t = p.read_text(encoding='utf-8')

# 1) Add the two pattern implementations before the provider class.
anchor = 'class CUiFragmentProvider : public IRawElementProviderSimple,'
assert t.count(anchor) == 1

patterns = '''// [A11Y-5] Invoke pattern: lets a UIA client press a button.
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
        m_pControl->Activate();
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
        if (!m_bReadOnly) return UIA_E_NOTSUPPORTED;
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
    // SetValue writes only when the control accepts input.
    const bool          m_bReadOnly = true;
};

'''

t = t.replace(anchor, patterns + anchor)

# 2) GetPatternProvider now answers Invoke / Value.
old = '''    IFACEMETHODIMP GetPatternProvider(PATTERNID patternId, IUnknown** pRetVal) override
    {
        if (pRetVal == nullptr) return E_POINTER;
        *pRetVal = nullptr;      // see the file header: patterns are out of scope
        (void)patternId;
        return S_OK;
    }
'''
assert t.count(old) == 1, t.count(old)
new = '''    IFACEMETHODIMP GetPatternProvider(PATTERNID patternId, IUnknown** pRetVal) override
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
        return S_OK;
    }
'''
t = t.replace(old, new)
p.write_text(t, encoding='utf-8')
print('ok')
