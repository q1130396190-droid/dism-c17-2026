import pathlib

p = pathlib.Path('tools/probes/a11y_uia_probe.cpp')
lines = p.read_text(encoding='utf-8').splitlines(True)

out = []
patched = False
for i, ln in enumerate(lines):
    if 'if (hWnd == nullptr)' in ln:
        out.append(ln.replace('if (hWnd == nullptr)', 'if (hWnd == nullptr && !bInject)'))
        patched = True
    else:
        out.append(ln)

assert patched, 'hwnd null check not found'
p.write_text(''.join(out), encoding='utf-8')
print('patched')
