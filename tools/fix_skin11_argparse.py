import pathlib

p = pathlib.Path('tools/probes/skin_win11.cpp')
t = p.read_text(encoding='utf-8', errors='replace')

old = ('    if (__argc >= 2 && __wargv != nullptr && __wargv[1] != nullptr)\n'
       '    {\n'
       '        sSkin = __wargv[1];\n'
       '    }\n')
assert t.count(old) == 1, t.count(old)
new = ('    // Take the first argument that is not the /Inject switch, so injecting does\n'
       '    // not accidentally consume the skin path.\n'
       '    for (int i = 1; i < __argc && __wargv != nullptr; ++i)\n'
       '    {\n'
       '        if (__wargv[i] == nullptr) continue;\n'
       '        if (::lstrcmpiW(__wargv[i], L"/Inject") == 0) continue;\n'
       '        sSkin = __wargv[i];\n'
       '        break;\n'
       '    }\n')
t = t.replace(old, new)
p.write_text(t, encoding='utf-8')
print('ok')
