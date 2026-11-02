"""Propagate probe exit codes for wrappers that only print PROBE_EXIT.

Found while adding /Inject: two wrappers (build_highcontrast, build_page_dark_audit)
print PROBE_EXIT but never exit with it, so a failing probe looked green in the
suite. Same class as build_roundrect_filled and build_skin_win11, both already
fixed. This closes the whole class instead of one instance at a time.
"""
import pathlib
import re

probes = pathlib.Path('tools/probes')
targets = ['build_highcontrast.ps1', 'build_page_dark_audit.ps1']

fixed, failed = [], []
for name in targets:
    f = probes / name
    if not f.exists():
        failed.append((name, 'missing'))
        continue
    lines = f.read_text(encoding='utf-8').splitlines(True)
    out = []
    patched = False
    for ln in lines:
        m = re.match(r'^(\s*)Write-Host "PROBE_EXIT=\$LASTEXITCODE"\s*$', ln)
        if m:
            indent = m.group(1)
            out.append('%s$probeCode = $LASTEXITCODE\n' % indent)
            out.append('%sWrite-Host "PROBE_EXIT=$probeCode"\n' % indent)
            out.append('%s# [GATE-FIX] PROBE_EXIT was printed but never propagated; the wrapper\n' % indent)
            out.append('%s# always exited 0, so a failing probe still looked green.\n' % indent)
            out.append('%sif ($probeCode -ne 0) { exit $probeCode }\n' % indent)
            patched = True
        else:
            out.append(ln)
    if not patched:
        failed.append((name, 'PROBE_EXIT line not matched'))
        continue
    f.write_text(''.join(out), encoding='utf-8')
    fixed.append(name)

print('fixed %d, failed %d' % (len(fixed), len(failed)))
for n in fixed:
    print('  + ' + n)
for n, why in failed:
    print('  ! %-32s %s' % (n, why))
