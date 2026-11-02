import pathlib
import re

probes = pathlib.Path('tools/probes')
exempt = [
    'build_attr_validation.ps1', 'build_caption_btn.ps1', 'build_caption_click.ps1',
    'build_count_semantics.ps1', 'build_dpi_change.ps1', 'build_dpi_round.ps1',
    'build_dynamic_theme.ps1', 'build_focus_border.ps1', 'build_font_fallback.ps1',
    'build_highcontrast.ps1', 'build_main_trace_env.ps1', 'build_markup_namechar.ps1',
    'build_maxrestore.ps1', 'build_namehash.ps1', 'build_render_page.ps1',
    'build_richedit_scroll.ps1', 'build_round_clip.ps1', 'build_roundrect_filled.ps1',
    'build_skin_attr.ps1', 'build_skin_win11.ps1', 'build_theme_coverage.ps1',
    'build_ui_interaction.ps1', 'build_ui_text_probe.ps1', 'build_unknown_control.ps1',
    'gate_budget_inject.ps1',
]

# Map each wrapper to the .cpp it compiles.
for name in exempt:
    f = probes / name
    if not f.exists():
        continue
    txt = f.read_text(encoding='utf-8', errors='replace')
    cpps = re.findall(r'([\w_]+\.cpp)', txt)
    exes = re.findall(r'([\w_]+\.exe)', txt)
    print('%-32s cpp=%-28s exe=%s' % (name, cpps[0] if cpps else '-', exes[0] if exes else '-'))
