import io

p = 'tools/probes/perf_struct_bench.cpp'
NL = chr(92) + 'n'   # a literal backslash followed by 'n', built without escapes

with io.open(p, 'r', encoding='utf-8', errors='replace') as f:
    text = f.read()

good = (
    '        const double overhead = fullMs - rawMs;' + chr(10) +
    '        ::printf("      framework overhead per call: %.3f us (%.0f%% of full path)' + NL + '",' + chr(10) +
    '                 (overhead / N) * 1000.0, fullMs > 0 ? overhead / fullMs * 100.0 : 0.0);' + chr(10) +
    '        ::printf("      of which GetFont resolution:  %.3f us' + NL + NL + '", (fontMs / N) * 1000.0);' + chr(10)
)

start = text.index('        const double overhead = fullMs - rawMs;')
endmark = '        Chk("C5b font lookup is not dominant"'
end = text.index(endmark)
text = text[:start] + good + text[end:]

with io.open(p, 'w', encoding='utf-8', newline='') as f:
    f.write(text)

print('done; backslash-n count =', text.count(NL))
