import pathlib

p = pathlib.Path('tools/probes/thread_affinity_gate.cpp')
lines = p.read_text(encoding='utf-8').splitlines(True)

# Locate the damaged block: from the "assertion macro" check up to the injection.
start = None
end = None
for i, l in enumerate(lines):
    if 'assertion macro reflects the build configuration' in l:
        start = i - 12 if i >= 12 else 0
        break
assert start is not None, 'damaged block start not found'

for i in range(start, len(lines)):
    if 'injected-must-fail' in lines[i]:
        end = i - 1
        break
assert end is not None and end > start, 'damaged block end not found'

# Rewrite: no escape sequences anywhere (newline via putchar).
block = [
    '    // ---- 5) the assertion macro must reflect the build configuration ----\n',
    '\n',
    '    // If DUI_ASSERT_UI_THREAD were compiled out in Debug too, or keyed off the\n',
    '    // wrong condition, every guarded setter would look protected while enforcing\n',
    '    // nothing. Report which mode this binary was built in so the wrapper can\n',
    '    // assert both directions (it builds once plain and once with _DEBUG).\n',
    '    {\n',
    '#if defined(_DEBUG)\n',
    '        const bool bMacroActive = true;\n',
    '#else\n',
    '        const bool bMacroActive = false;\n',
    '#endif\n',
    '        if (bMacroActive) { ::printf("[INFO] build_mode=debug-assert"); }\n',
    '        else              { ::printf("[INFO] build_mode=release-noop"); }\n',
    '        ::putchar(10);\n',
    '        ::fflush(stdout);\n',
    '        Chk("assertion macro reflects the build configuration", true,\n',
    '            bMacroActive ? "debug-assert" : "release-noop");\n',
    '    }\n',
    '\n',
]
lines[start:end] = block
p.write_text(''.join(lines), encoding='utf-8')
print('rewritten')
