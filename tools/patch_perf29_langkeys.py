import pathlib

p = pathlib.Path('Demos/Win8Metro/StringManager.cpp')
t = p.read_text(encoding='utf-8')

# Anchor on the key name only (no escape sequences in the pattern).
marker = '{ "svc.s_state_paused",'
idx = t.find(marker)
assert idx > 0, 'anchor not found'
line_end = t.find('\n', idx)
assert line_end > idx

addition = (
    '\n    // [PERF-29] Service start-type dropdown labels. Round 11 referenced\n'
    '    // svc.s_auto / svc.s_manual / svc.s_disabled while rewriting the row builder,\n'
    '    // but the keys were never defined, so every service row rendered the raw\n'
    '    // "[svc.s_auto]" text (849 occurrences, measured by /DumpPages keymiss).\n'
    '    { "svc.s_auto",     L"\\u81ea\\u52a8" },   // auto\n'
    '    { "svc.s_manual",   L"\\u624b\\u52a8" },   // manual\n'
    '    { "svc.s_disabled", L"\\u7981\\u7528" },   // disabled\n')

t = t[:line_end + 1] + addition + t[line_end + 1:]
p.write_text(t, encoding='utf-8')
print('inserted after svc.s_state_paused')
