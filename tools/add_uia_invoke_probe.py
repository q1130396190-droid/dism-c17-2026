import pathlib

p = pathlib.Path('tools/probes/a11y_uia_probe.cpp')
t = p.read_text(encoding='utf-8')

# Extend Walk to also look for a button that exposes Invoke, and invoke it.
old = '''            ::sprintf_s(sz, "named=%d", nNamed);
            Chk("at least one element exposes a Name", nNamed > 0, sz);
'''
assert t.count(old) == 1, t.count(old)

new = '''            ::sprintf_s(sz, "named=%d", nNamed);
            Chk("at least one element exposes a Name", nNamed > 0, sz);

            // [A11Y-5] A tree a client can only read is half an accessibility
            // story. Look for a button that advertises Invoke and actually call
            // it, so "the pattern exists" and "the pattern works" are both
            // measured.
            int nInvokable = 0;
            bool bInvoked = false;
            HRESULT hrInvoke = E_FAIL;
            FindAndInvoke(pRoot, 0, 4, nInvokable, bInvoked, hrInvoke);

            ::sprintf_s(sz, "invokable=%d invoked=%d hr=0x%08lX",
                        nInvokable, bInvoked ? 1 : 0,
                        static_cast<unsigned long>(hrInvoke));
            Chk("at least one element exposes Invoke", nInvokable > 0, sz);
            Chk("Invoke actually activates the control", bInvoked, sz);
'''
t = t.replace(old, new)

# Add the helper before main().
anchor = 'int main(int argc, char** argv)\n{'
assert t.count(anchor) == 1
helper = '''// Depth-first search for a control that exposes InvokePattern; invokes the
// first one found and reports the HRESULT.
static void FindAndInvoke(IUIAutomationElement* pElem, int nDepth, int nDepthLimit,
                          int& nInvokable, bool& bInvoked, HRESULT& hrResult)
{
    if (pElem == nullptr || nDepth > nDepthLimit) return;

    IUnknown* pUnk = nullptr;
    if (SUCCEEDED(pElem->GetCurrentPattern(UIA_InvokePatternId, &pUnk)) && pUnk != nullptr)
    {
        ++nInvokable;
        if (!bInvoked)
        {
            IInvokeProvider* pInvoke = nullptr;
            if (SUCCEEDED(pUnk->QueryInterface(__uuidof(IInvokeProvider),
                                               reinterpret_cast<void**>(&pInvoke)))
                && pInvoke != nullptr)
            {
                hrResult = pInvoke->Invoke();
                bInvoked = SUCCEEDED(hrResult);
                pInvoke->Release();
            }
        }
        pUnk->Release();
    }

    IUIAutomation* pUia = nullptr;
    IUIAutomationTreeWalker* pWalker = nullptr;
    if (FAILED(::CoCreateInstance(__uuidof(CUIAutomation), nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pUia))))
        return;
    if (SUCCEEDED(pUia->get_ControlViewWalker(&pWalker)) && pWalker != nullptr)
    {
        IUIAutomationElement* pChild = nullptr;
        HRESULT hr = pWalker->GetFirstChildElement(pElem, &pChild);
        int nGuard = 0;
        while (SUCCEEDED(hr) && pChild != nullptr && nGuard++ < 5000)
        {
            FindAndInvoke(pChild, nDepth + 1, nDepthLimit, nInvokable, bInvoked, hrResult);

            IUIAutomationElement* pNext = nullptr;
            hr = pWalker->GetNextSiblingElement(pChild, &pNext);
            pChild->Release();
            pChild = pNext;
        }
        pWalker->Release();
    }
    pUia->Release();
}

'''
t = t.replace(anchor, helper + anchor)

# Inject blocks must be reachable: include the new asserts in the injection path too.
p.write_text(t, encoding='utf-8')
print('ok')
