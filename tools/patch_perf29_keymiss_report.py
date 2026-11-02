import pathlib

p = pathlib.Path('Demos/Win8Metro/FrameWnd.cpp')
t = p.read_bytes().decode('utf-8-sig')

old = '    int nEmpty = 0;'
assert t.count(old) == 1
if 'nTotalKeyMiss' not in t:
    t = t.replace(old, '    int nEmpty = 0;\n    int nTotalKeyMiss = 0;   // [PERF-29] unresolved "[key]" texts, all pages')

old2 = 'if (bEmpty) ++nEmpty;'
assert t.count(old2) == 1
if 'nTotalKeyMiss += nKeyMiss;' not in t:
    t = t.replace(old2, 'if (bEmpty) ++nEmpty;\n        nTotalKeyMiss += nKeyMiss;')

# Anchor on the tail of the format call, which has no quotes of its own.
old3 = '16, nEmpty);'
assert t.count(old3) == 1, t.count(old3)
t = t.replace(old3, '16, nEmpty, nTotalKeyMiss);')

# Widen the format string: replace only the literal that precedes the arg list.
old4 = 'pages=%d empty=%d'
assert t.count(old4) == 1, t.count(old4)
t = t.replace(old4, 'pages=%d empty=%d keymiss=%d')

p.write_bytes(b'\xef\xbb\xbf' + t.encode('utf-8'))
print('ok')
