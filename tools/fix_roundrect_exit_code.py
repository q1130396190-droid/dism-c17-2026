import pathlib

p = pathlib.Path('tools/probes/build_roundrect_filled.ps1')
lines = p.read_text(encoding='utf-8').splitlines(True)

out = []
patched = False
for i, ln in enumerate(lines):
    out.append(ln)
    if ln.strip() == 'Write-Host "PROBE_EXIT=$LASTEXITCODE"':
        out[-1] = '    $probeCode = $LASTEXITCODE\n'
        out.append('    Write-Host "PROBE_EXIT=$probeCode"\n')
        out.append('    # [GATE-FIX] This script printed PROBE_EXIT but never propagated it, so the\n')
        out.append('    # wrapper always exited 0 and a failing probe still looked green.\n')
        out.append('    if ($probeCode -ne 0) { exit $probeCode }\n')
        patched = True

assert patched, 'PROBE_EXIT line not found'
p.write_text(''.join(out), encoding='utf-8')
print('patched')
