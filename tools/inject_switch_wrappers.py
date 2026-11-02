"""Add a -Inject switch to the wrapper scripts of the probes patched above.

Each wrapper ends with a line that invokes its exe. This rewrites that line to
forward /Inject when asked. Idempotent.
"""
import pathlib
import re
import sys

probes = pathlib.Path('tools/probes')
wrappers = [
    'build_caption_btn.ps1', 'build_caption_click.ps1', 'build_count_semantics.ps1',
    'build_dpi_change.ps1', 'build_dpi_round.ps1', 'build_dynamic_theme.ps1',
    'build_focus_border.ps1', 'build_highcontrast.ps1', 'build_main_trace_env.ps1',
    'build_markup_namechar.ps1', 'build_maxrestore.ps1', 'build_namehash.ps1',
    'build_render_page.ps1', 'build_richedit_scroll.ps1', 'build_round_clip.ps1',
    'build_theme_coverage.ps1', 'build_ui_interaction.ps1', 'build_ui_text_probe.ps1',
    'build_unknown_control.ps1', 'build_roundrect_filled.ps1',
]

PARAM_BLOCK = (
    '# [GATE-SELFTEST] -Inject forwards /Inject so the probe adds a failing assertion.\n'
    'param(\n'
    '    [switch] $Inject\n'
    ')\n\n')

changed, skipped, failed = [], [], []
for name in wrappers:
    f = probes / name
    if not f.exists():
        failed.append((name, 'missing'))
        continue
    t = f.read_text(encoding='utf-8', errors='replace')
    if 'param(' in t:
        skipped.append(name)
        continue

    # Insert the param block before the first statement.
    anchor = "$ErrorActionPreference = 'Stop'"
    if t.count(anchor) == 1:
        t = t.replace(anchor, PARAM_BLOCK + anchor, 1)
    else:
        failed.append((name, 'no ErrorActionPreference anchor'))
        continue

    # Rewrite the invocation line: `& ".\<x>.exe"` -> conditional form.
    m = re.search(r'^(\s*)& "\.\\([\w_]+\.exe)"\s*$', t, re.M)
    if not m:
        # Some wrappers pass arguments; fall back to a generic substitution.
        m2 = re.search(r'^(\s*)& "\.\\([\w_]+\.exe)"(.*)$', t, re.M)
        if not m2:
            failed.append((name, 'no invocation line'))
            continue
        indent, exe, rest = m2.group(1), m2.group(2), m2.group(3)
        repl = ('%sif ($Inject) { & ".\\%s" /Inject%s } else { & ".\\%s"%s }'
                % (indent, exe, rest, exe, rest))
        t = t[:m2.start()] + repl + t[m2.end():]
    else:
        indent, exe = m.group(1), m.group(2)
        repl = ('%sif ($Inject) { & ".\\%s" /Inject } else { & ".\\%s" }'
                % (indent, exe, exe))
        t = t[:m.start()] + repl + t[m.end():]

    f.write_text(t, encoding='utf-8')
    changed.append(name)

print('changed %d, skipped %d, failed %d' % (len(changed), len(skipped), len(failed)))
for n in changed:
    print('  + ' + n)
for n, why in failed:
    print('  ! %-32s %s' % (n, why))
sys.exit(1 if failed else 0)
