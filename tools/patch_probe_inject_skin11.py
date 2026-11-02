import pathlib

p = pathlib.Path('tools/probes/skin_win11.cpp')
t = p.read_text(encoding='utf-8', errors='replace')

if 'bInject' not in t:
    anchor = '    std::wstring sSkin;\n'
    assert t.count(anchor) == 1, t.count(anchor)
    t = t.replace(anchor,
        '    std::wstring sSkin;\n\n'
        '    // [GATE-SELFTEST] /Inject adds one deliberately failing assertion so this\n'
        '    // probe can prove its verdict path still works. Uses __argc/__wargv because\n'
        '    // this file already reads its skin path from there.\n'
        '    bool bInject = false;\n'
        '    for (int i = 1; i < __argc && __wargv != nullptr; ++i)\n'
        '        if (__wargv[i] != nullptr && ::lstrcmpiW(__wargv[i], L"/Inject") == 0) bInject = true;\n',
        1)

tail = '    return (g_nFail == 0) ? 0 : 1;'
assert t.count(tail) == 1, t.count(tail)
# Chk here takes std::wstring, so the detail must be wide.
inject = ('    if (bInject)\n'
          '    {\n'
          '        Chk("injected-must-fail", false, L"self-test: forced failure");\n'
          '    }\n')
t = t.replace(tail, inject + tail)
p.write_text(t, encoding='utf-8')
print('ok')
