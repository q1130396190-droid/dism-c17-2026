import pathlib
import re

p = pathlib.Path('tools/probes/skin_win11.cpp')
txt = p.read_text(encoding='utf-8', errors='replace')
lines = txt.splitlines(True)

# Locate the start: the [GATE-SELFTEST] comment block.
start = None
for i, l in enumerate(lines):
    if '[GATE-SELFTEST]' in l:
        start = i
        break
assert start is not None, 'GATE-SELFTEST comment not found'

# Locate the end: the line with the skin-arg loop assignment we introduced.
end = None
for i in range(start, len(lines)):
    if 'pszSkinArg = __wargv[i];' in lines[i]:
        end = i
        break
assert end is not None, 'pszSkinArg assignment not found'

replacement = [
    '    // [GATE-SELFTEST] /Inject adds one deliberately failing assertion so this\n',
    '    // probe can prove its verdict path still works. Uses __argc/__wargv because\n',
    '    // this file already reads its skin path from there.\n',
    '    bool bInject = false;\n',
    '    for (int i = 1; i < __argc; ++i)\n',
    '        if (lstrcmpiW(__wargv[i], L"/Inject") == 0) bInject = true;\n',
    '    // Skip the /Inject switch when reading the optional skin path, otherwise\n',
    '    // injecting would consume the path slot and load nothing.\n',
    '    const wchar_t* pszSkinArg = nullptr;\n',
    '    for (int i = 1; i < __argc; ++i)\n',
    '    {\n',
    '        if (lstrcmpiW(__wargv[i], L"/Inject") == 0) continue;\n',
    '        pszSkinArg = __wargv[i];\n',
]
lines[start:end] = replacement
p.write_text(''.join(lines), encoding='utf-8')
print('rewritten block')
