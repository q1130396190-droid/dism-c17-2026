"""Locate MSVC's dumpbin.exe without hard-coding a toolchain version.

Why this module exists (round 87): three tools each carried their own copy of

    DUMPBIN_CANDIDATES = [
        r"C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\VC\\Tools\\MSVC"
        r"\\14.51.36231\\bin\\Hostx64\\x64\\dumpbin.exe",
    ]

Identical literals in abi_baseline.py, check_control_factory_exports.py and
check_lib_dll_match.py. Three copies of one judgement is the structural cause this
project kept hitting: a fix lands in one copy and the others stay wrong (round 85
fixed a prefix match in the main verdict and missed the copy inside --inject).

The literal pins a compiler version. When VS updates -- and it has, repeatedly in
this repository -- all three tools break at once, each with its own message, and
the breakage reads like "dumpbin is missing" rather than "three copies of a path".

The PowerShell side already did the right thing (tools/probes/_find_toolchain.ps1
walks the VS install tree). This is the Python equivalent, so both sides behave the
same way and a toolchain upgrade needs no edit here.

Resolution order:
    1. $DUILIB_DUMPBIN, if set -- lets a caller override without touching code.
    2. Every MSVC toolset under Program Files\\Microsoft Visual Studio, newest
       version first, x64 host only.
    3. PATH, as a last resort (a developer command prompt already has it).

Returns None when nothing is found; callers decide whether that is fatal.
"""
import os
import pathlib
import re
import shutil


def _version_key(name):
    """Sort '14.51.36231' numerically; non-numeric names sort last."""
    parts = re.findall(r'\d+', name)
    return tuple(int(x) for x in parts) if parts else (0,)


def find_dumpbin():
    """Return a path to dumpbin.exe, or None when it cannot be located."""
    override = os.environ.get('DUILIB_DUMPBIN')
    if override and pathlib.Path(override).is_file():
        return override

    roots = [
        pathlib.Path(r'C:\Program Files\Microsoft Visual Studio'),
        pathlib.Path(r'C:\Program Files (x86)\Microsoft Visual Studio'),
    ]
    found = []
    for root in roots:
        if not root.is_dir():
            continue
        # ...\<Year>\<Edition>\VC\Tools\MSVC\<version>\bin\Hostx64\x64\dumpbin.exe
        # Layout: <Year>/<Edition>/VC/Tools/MSVC/<version>/bin/Hostx64/x64/
        #                     ^ one level was missing in the first version, so the
        #                     walk found nothing while the hard-coded path worked --
        #                     measured round 87: "[FAIL] not found" on a machine
        #                     that plainly had the file.
        for exe in root.glob('*/*/VC/Tools/MSVC/*/bin/Hostx64/x64/dumpbin.exe'):
            found.append(exe)
    if found:
        # Newest toolset wins, so a VS upgrade is picked up with no edit here.
        found.sort(key=lambda p: _version_key(p.parent.parent.parent.name),
                   reverse=True)
        return str(found[0])

    on_path = shutil.which('dumpbin')
    if on_path:
        return on_path
    return None


def require_dumpbin(tool_name):
    """Return the path, or print an actionable message and return None."""
    path = find_dumpbin()
    if path is not None:
        return path
    print('[FAIL] %s: dumpbin.exe not found.' % tool_name)
    print('       looked for: <VS install>/*/VC/Tools/MSVC/*/bin/Hostx64/x64/dumpbin.exe')
    print('       and PATH. Override with DUILIB_DUMPBIN=<full path> if it lives elsewhere.')
    return None


if __name__ == '__main__':
    located = find_dumpbin()
    print(located if located else '[FAIL] not found')
