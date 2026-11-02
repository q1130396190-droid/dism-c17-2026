import pathlib

p = pathlib.Path('tools/probes/build_skin_win11.ps1')
t = p.read_text(encoding='utf-8')

if 'param(' not in t:
    old = "$ErrorActionPreference = 'Stop'"
    assert t.count(old) == 1
    t = t.replace(old,
        "# [GATE-SELFTEST] -Inject forwards /Inject so the probe adds a failing assertion.\n"
        "param(\n"
        "    [switch] $Inject\n"
        ")\n\n"
        "$ErrorActionPreference = 'Stop'", 1)

old2 = r'    & ".\skin_win11.exe"'
assert t.count(old2) == 1, t.count(old2)
t = t.replace(old2, r'    if ($Inject) { & ".\skin_win11.exe" /Inject } else { & ".\skin_win11.exe" }')
p.write_text(t, encoding='utf-8')
print('ok')
