import pathlib

p = pathlib.Path('DuiLib/Utils/UIAutomation.cpp')
lines = p.read_bytes().decode('utf-8', errors='surrogateescape').splitlines(True)

# Find the broken diagnostic block and replace it wholesale. Escapes inside
# string literals do not survive hand-patching; building the block from a list
# of lines with explicit chr() newlines avoids the problem entirely.
start = None
for i, l in enumerate(lines):
    if '[A11Y-3 diag] The host process has no redirected stdout' in l:
        start = i
        break
assert start is not None, 'diag comment not found'

end = start
while end < len(lines) and 'CloseHandle(hF)' not in lines[end]:
    end += 1
assert end < len(lines), 'block end not found'
# Include the closing braces that follow CloseHandle.
while end < len(lines) and lines[end].strip() not in ('}', '}}'):
    end += 1
end += 1

CR = chr(13)
LF = chr(10)
block = [
    '        // [A11Y-3 diag] The host process has no redirected stdout, so write to a' + LF,
    '        // file to prove whether WM_GETOBJECT/UiaRootObjectId is reached.' + LF,
    '        {' + LF,
    '            HANDLE hF = ::CreateFileW(L"D:/DuiLib_Ultimate-master/bin64/uia_diag.log",' + LF,
    '                                      FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,' + LF,
    '                                      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);' + LF,
    '            if (hF != INVALID_HANDLE_VALUE)' + LF,
    '            {' + LF,
    '                const char* pszDiag = "OnGetObject UiaRootObjectId branch hit";' + LF,
    '                DWORD dwW = 0;' + LF,
    '                ::WriteFile(hF, pszDiag, (DWORD)::strlen(pszDiag), &dwW, nullptr);' + LF,
    '                ::WriteFile(hF, "' + '\\' + 'r' + '\\' + 'n", 2, &dwW, nullptr);' + LF,
    '                ::CloseHandle(hF);' + LF,
    '            }' + LF,
    '        }' + LF,
]
lines[start:end] = block
p.write_bytes(''.join(lines).encode('utf-8', errors='surrogateescape'))
print('rewritten')
