"""Fix the injected Chk() call so it matches each probe's signature.

The probes define five different Chk overloads:
    (const char*, bool, const char*)
    (const char*, bool, const std::string&)
    (const char*, bool, const wchar_t*)
    (const char*, bool, const std::wstring&)
    (const char*, bool, const wstring&)   [DuiLib string]

Passing "self-test: forced failure" only works for the narrow-char forms. This
rewrites the injected line per file to the matching literal type.
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

GENERIC = 'Chk("injected-must-fail", false, "self-test: forced failure");'

def detail_literal(sig: str) -> str:
    if 'wstring' in sig or 'wchar_t' in sig:
        return 'L"self-test: forced failure"'
    return '"self-test: forced failure"'

fixed, skipped, failed = [], [], []
for name in targets:
    f = probes / name
    if not f.exists():
        failed.append((name, 'missing'))
        continue
    t = f.read_text(encoding='utf-8', errors='replace')
    if 'injected-must-fail' not in t:
        skipped.append(name)
        continue

    m = re.search(r'void\s+Chk\s*\(([^)]*)\)', t)
    if not m:
        failed.append((name, 'no Chk signature'))
        continue
    sig = m.group(1)
    lit = detail_literal(sig)
    new_call = 'Chk("injected-must-fail", false, %s);' % lit
    if new_call == GENERIC:
        skipped.append(name + ' (already matches)')
        continue
    t = t.replace(GENERIC, new_call)
    f.write_text(t, encoding='utf-8')
    fixed.append((name, 'wchar' if lit.startswith('L') else 'narrow'))

print('fixed %d, skipped %d, failed %d' % (len(fixed), len(skipped), len(failed)))
for n, kind in fixed:
    print('  %-30s %s' % (n, kind))
for n in skipped:
    print('  = ' + n)
for n, why in failed:
    print('  ! %-30s %s' % (n, why))
sys.exit(1 if failed else 0)
