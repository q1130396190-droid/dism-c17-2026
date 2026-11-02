import pathlib

p = pathlib.Path('tools/probes/a11y_probe.cpp')
t = p.read_text(encoding='utf-8', errors='replace')

if 'bInject' not in t:
    old_main = 'int main()\n{'
    assert t.count(old_main) == 1, t.count(old_main)
    new_main = (
        'int main(int argc, char** argv)\n{\n'
        '    // [GATE-SELFTEST] /Inject adds one deliberately failing assertion so this\n'
        '    // probe can prove its verdict path still works. An accessibility tree that\n'
        '    // silently stopped exposing names would otherwise look like a pass.\n'
        '    bool bInject = false;\n'
        '    for (int i = 1; i < argc; ++i)\n'
        '        if (::lstrcmpiA(argv[i], "/Inject") == 0) bInject = true;\n')
    t = t.replace(old_main, new_main)

anchor = '    return (g_nFail == 0) ? 0 : 1;'
assert t.count(anchor) == 1, t.count(anchor)
inject = (
    '    if (bInject)\n'
    '    {\n'
    '        Chk("injected-must-fail", false, "self-test: forced failure");\n'
    '    }\n')
t = t.replace(anchor, inject + anchor)
p.write_text(t, encoding='utf-8')
print('ok')
