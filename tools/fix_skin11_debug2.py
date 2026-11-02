import pathlib

p = pathlib.Path('tools/probes/skin_win11.cpp')
lines = p.read_text(encoding='utf-8', errors='replace').splitlines(True)

# Drop every line belonging to the broken debug block, then re-insert a clean one.
start = None
for i, l in enumerate(lines):
    if 'bool bInject = false;' in l:
        start = i
        break
assert start is not None, 'bInject decl not found'

end = start + 1
while end < len(lines) and ('[dbg]' in lines[end] or 'int di' in lines[end]
                            or lines[end].strip() in ('{', '}', '')
                            or 'wprintf' in lines[end]):
    end += 1

clean = [
    '    bool bInject = false;\n',
    '    ::printf("[dbg] argc=%d", __argc); ::putchar(10);\n',
    '    for (int di = 0; di < __argc; ++di)\n',
    '    {\n',
    '        ::printf("[dbg] arg%d=", di); ::putchar(10);\n',
    '    }\n',
]
lines[start + 1:end] = clean
p.write_text(''.join(lines), encoding='utf-8')
print('rewritten')
