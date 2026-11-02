import pathlib

p = pathlib.Path('tools/probes/skin_win11.cpp')
t = p.read_text(encoding='utf-8', errors='replace')

# Remove the broken debug lines (they contain a real newline from an expanded \n).
bad_lines = [l for l in t.splitlines(True) if '[dbg]' in l]
assert bad_lines, 'debug lines not found'
for l in bad_lines:
    t = t.replace(l, '')

# Re-add them without any escape sequences: use putchar for the newline.
anchor = '    bool bInject = false;'
assert t.count(anchor) == 1
dbg = ('    bool bInject = false;\n'
       '    ::printf("[dbg] argc=%d", __argc); ::putchar(10);\n'
       '    for (int di = 0; di < __argc && __wargv != nullptr; ++di)\n'
       '    {\n'
       '        ::printf("[dbg] argv[%d]=", di);\n'
       '        ::wprintf(L"%s", __wargv[di] ? __wargv[di] : L"(null)"); ::putchar(10);\n'
       '    }\n')
t = t.replace(anchor, dbg, 1)
p.write_text(t, encoding='utf-8')
print('ok')
