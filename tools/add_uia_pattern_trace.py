import pathlib

p = pathlib.Path('DuiLib/Utils/UIAutomationProvider.cpp')
t = p.read_text(encoding='utf-8')

LF = chr(10)
BS = chr(92)
CRLF = '"' + BS + 'x' + '0A"'

# A tiny file logger so we can see whether the UIA proxy ever reaches the pattern
# object. printf is useless here: the host process has no redirected stdout.
logger = (
    '// [A11Y-6 diag] Trace file: the host has no redirected stdout, so "did the proxy\n'
    '// reach us" can only be answered from a file.\n'
    'static void UiaTrace(const char* pszLine)\n'
    '{\n'
    '    HANDLE hF = ::CreateFileA("D:/DuiLib_Ultimate-master/bin64/uiapat.log",\n'
    '                              FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,\n'
    '                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);\n'
    '    if (hF == INVALID_HANDLE_VALUE) return;\n'
    '    DWORD dwW = 0;\n'
    '    ::WriteFile(hF, pszLine, (DWORD)::strlen(pszLine), &dwW, nullptr);\n'
    '    ::WriteFile(hF, ' + CRLF + ', 2, &dwW, nullptr);\n'
    '    ::CloseHandle(hF);\n'
    '}\n\n')

anchor = '// [A11Y-5] Invoke pattern: lets a UIA client press a button.'
assert t.count(anchor) == 1
if 'UiaTrace' not in t:
    t = t.replace(anchor, logger + anchor)

# Trace the Invoke provider's QI and Invoke.
old_qi = '''        if (riid == __uuidof(IUnknown) || riid == __uuidof(IInvokeProvider))
        {
            *ppv = static_cast<IInvokeProvider*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;'''
assert t.count(old_qi) == 1
new_qi = '''        if (riid == __uuidof(IUnknown) || riid == __uuidof(IInvokeProvider))
        {
            UiaTrace("InvokeProvider::QueryInterface hit");
            *ppv = static_cast<IInvokeProvider*>(this);
            AddRef();
            return S_OK;
        }
        UiaTrace("InvokeProvider::QueryInterface miss");
        return E_NOINTERFACE;'''
t = t.replace(old_qi, new_qi)

old_inv = '''        if (m_pControl == nullptr) return UIA_E_ELEMENTNOTAVAILABLE;
        if (!m_pControl->IsEnabled()) return UIA_E_ELEMENTNOTENABLED;
        // Activate() returns false when the control is not visible/enabled or'''
assert t.count(old_inv) == 1
new_inv = '''        UiaTrace("InvokeProvider::Invoke entered");
        if (m_pControl == nullptr) return UIA_E_ELEMENTNOTAVAILABLE;
        if (!m_pControl->IsEnabled()) return UIA_E_ELEMENTNOTENABLED;
        // Activate() returns false when the control is not visible/enabled or'''
t = t.replace(old_inv, new_inv)

# Also trace GetPatternProvider so we can see if the proxy even asks for it.
old_gp = '''        if (patternId == UIA_InvokePatternId)
        {'''
assert t.count(old_gp) == 1
new_gp = '''        if (patternId == UIA_InvokePatternId)
        {
            UiaTrace("GetPatternProvider asked for Invoke");'''
t = t.replace(old_gp, new_gp)

p.write_text(t, encoding='utf-8')
print('ok')
