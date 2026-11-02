import pathlib

p = pathlib.Path('tools/probes/build_theme_incremental.ps1')
t = p.read_text(encoding='utf-8')

if 'param(' not in t:
    old = "$ErrorActionPreference = 'Stop'"
    assert t.count(old) == 1
    new = ("# [GATE-SELFTEST] -Inject forwards /Inject so the probe adds a failing assertion.\n"
           "param(\n"
           "    [switch] $Inject\n"
           ")\n\n"
           "$ErrorActionPreference = 'Stop'")
    t = t.replace(old, new, 1)

old2 = r'    & ".\theme_incremental_probe.exe"'
assert t.count(old2) == 1, t.count(old2)
new2 = r'    if ($Inject) { & ".\theme_incremental_probe.exe" /Inject } else { & ".\theme_incremental_probe.exe" }'
t = t.replace(old2, new2)
p.write_text(t, encoding='utf-8')
print('ok')
