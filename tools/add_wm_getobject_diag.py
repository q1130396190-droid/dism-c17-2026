import pathlib

p = pathlib.Path('DuiLib/Core/UIManager.cpp')
lines = p.read_bytes().decode('utf-8', errors='surrogateescape').splitlines(True)
LF = chr(10)
BS = chr(92)          # backslash
CRLF_LIT = '"' + BS + 'r' + BS + 'n"'

# Log every WM_GETOBJECT with its wParam/lParam, so we can see what the system
# actually sends when a UIA client calls ElementFromHandle.
block = [
    '\t\t{' + LF,
    '\t\t\t// [A11Y-3 diag] Record what the system actually asks for.' + LF,
    '\t\t\tHANDLE hF = ::CreateFileW(L"D:/DuiLib_Ultimate-master/bin64/uia_diag.log",' + LF,
    '\t\t\t                          FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,' + LF,
    '\t\t\t                          OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);' + LF,
    '\t\t\tif (hF != INVALID_HANDLE_VALUE)' + LF,
    '\t\t\t{' + LF,
    '\t\t\t\tchar szB[128] = {};' + LF,
    '\t\t\t\t::sprintf_s(szB, "WM_GETOBJECT w=0x%08lX l=0x%08lX",' + LF,
    '\t\t\t\t            (unsigned long)wParam, (unsigned long)lParam);' + LF,
    '\t\t\t\tDWORD dwW = 0;' + LF,
    '\t\t\t\t::WriteFile(hF, szB, (DWORD)::strlen(szB), &dwW, nullptr);' + LF,
    '\t\t\t\t::WriteFile(hF, ' + CRLF_LIT + ', 2, &dwW, nullptr);' + LF,
    '\t\t\t\t::CloseHandle(hF);' + LF,
    '\t\t\t}' + LF,
    '\t\t}' + LF,
]

for i, l in enumerate(lines):
    if 'case WM_GETOBJECT:' in l:
        lines[i + 1:i + 1] = block
        break
else:
    raise SystemExit('WM_GETOBJECT case not found')

p.write_bytes(''.join(lines).encode('utf-8', errors='surrogateescape'))
print('ok')
