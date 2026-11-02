"""Add a /Inject self-test entry to every uniform probe.

Why: gate_inventory.ps1 found 25 probes that cannot prove they fail. Twenty of
them share an identical shape (int main(), Chk(), g_nFail, and the
(g_nFail==0)?0:1 return), so the injection can be added mechanically instead of
by hand 20 times.

The injected failure goes through Chk() on purpose, so it exercises the real
counting and exit-code mapping rather than short-circuiting them.

Idempotent: a probe that already has bInject is skipped.
"""
import pathlib
import re
import sys

probes = pathlib.Path('tools/probes')
targets = [
    'caption_btn_probe.cpp', 'caption_click_probe.cpp', 'count_semantics_probe.cpp',
    'dpi_change_probe.cpp', 'dpi_round_probe.cpp', 'dynamic_theme_probe.cpp',
    'focus_border_probe.cpp', 'highcontrast_probe.cpp', 'main_trace_env_probe.cpp',
    'markup_namechar_probe.cpp', 'maxrestore_probe.cpp', 'namehash_probe.cpp',
    'render_page_probe.cpp', 'richedit_scroll_probe.cpp', 'round_clip_probe.cpp',
    'theme_coverage_probe.cpp', 'ui_interaction_probe.cpp', 'ui_text_probe.cpp',
    'unknown_control_probe.cpp', 'roundrect_filled.cpp',
]

MAIN_OLD = 'int main()\n{'
INJECT_DECL = (
    'int main(int argc, char** argv)\n{\n'
    '    // [GATE-SELFTEST] /Inject adds one deliberately failing assertion so this\n'
    '    // probe can prove its verdict path still works; otherwise a check that\n'
    '    // silently stopped asserting would look exactly like a passing run.\n'
    '    bool bInject = false;\n'
    '    for (int i = 1; i < argc; ++i)\n'
    '        if (::lstrcmpiA(argv[i], "/Inject") == 0) bInject = true;\n')

RETURN_LINE = '    return (g_nFail == 0) ? 0 : 1;'
INJECT_BLOCK = (
    '    if (bInject)\n'
    '    {\n'
    '        Chk("injected-must-fail", false, "self-test: forced failure");\n'
    '    }\n')

changed, skipped, failed = [], [], []
for name in targets:
    f = probes / name
    if not f.exists():
        failed.append((name, 'missing'))
        continue
    t = f.read_text(encoding='utf-8', errors='replace')
    if 'bInject' in t:
        skipped.append(name)
        continue
    if t.count(MAIN_OLD) != 1 or t.count(RETURN_LINE) != 1:
        failed.append((name, 'shape mismatch main=%d ret=%d' % (t.count(MAIN_OLD), t.count(RETURN_LINE))))
        continue
    t = t.replace(MAIN_OLD, INJECT_DECL)
    t = t.replace(RETURN_LINE, INJECT_BLOCK + RETURN_LINE)
    f.write_text(t, encoding='utf-8')
    changed.append(name)

print('changed %d, skipped %d, failed %d' % (len(changed), len(skipped), len(failed)))
for n in changed:
    print('  + ' + n)
for n, why in failed:
    print('  ! %-30s %s' % (n, why))
sys.exit(1 if failed else 0)
