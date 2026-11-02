import pathlib
import re

probes = pathlib.Path('tools/probes')
cpps = [
    'caption_btn_probe.cpp', 'caption_click_probe.cpp', 'count_semantics_probe.cpp',
    'dpi_change_probe.cpp', 'dpi_round_probe.cpp', 'dynamic_theme_probe.cpp',
    'focus_border_probe.cpp', 'highcontrast_probe.cpp', 'main_trace_env_probe.cpp',
    'markup_namechar_probe.cpp', 'maxrestore_probe.cpp', 'namehash_probe.cpp',
    'render_page_probe.cpp', 'richedit_scroll_probe.cpp', 'round_clip_probe.cpp',
    'skin_win11.cpp', 'theme_coverage_probe.cpp', 'ui_interaction_probe.cpp',
    'ui_text_probe.cpp', 'unknown_control_probe.cpp', 'roundrect_filled.cpp',
]

pat_main = re.compile(r'int\s+main\s*\(\s*\)')
pat_fail = re.compile(r'g_nFail')
pat_ret = re.compile(r'return\s*\(\s*g_nFail\s*==\s*0\s*\)')
pat_chk = re.compile(r'\bChk\s*\(|\bCheck\s*\(|\bAssert\w*\s*\(')

ready = []
manual = []
for n in cpps:
    f = probes / n
    if not f.exists():
        manual.append((n, 'MISSING'))
        continue
    t = f.read_text(encoding='utf-8', errors='replace')
    flags = []
    if not pat_main.search(t): flags.append('no-int-main()')
    if not pat_fail.search(t): flags.append('no-g_nFail')
    if not pat_ret.search(t): flags.append('no-return-map')
    if not pat_chk.search(t): flags.append('no-Chk-like')
    if 'argc' in t or 'bInject' in t: flags.append('already-args')
    (ready if not flags else manual).append((n, ','.join(flags) if flags else 'OK'))

print('READY (%d):' % len(ready))
for n, why in ready: print('  ' + n)
print('MANUAL (%d):' % len(manual))
for n, why in manual: print('  %-30s %s' % (n, why))
