"""Verify the import library matches the DLL it was generated from.

Why this exists: round 55 lost half an hour to `LNK2001` on a symbol that WAS in the
DLL's export table. The real cause was a stale import library: the build wrote
`Lib\\x64\\DuiLib.lib`, the deployment copy at `bin64\\amd64\\` was older, and the
linker resolved against the old one. `dumpbin /exports DuiLib.dll` showed the symbol
present, which made "the export is missing" look impossible -- the library was simply
from a different build.

`check_artifacts.ps1` compares timestamps, which is necessary but not sufficient:
a lib can be newer and still not match (wrong configuration, partial rebuild of the
DLL only -- `/t:Rebuild` on the DLL target does NOT regenerate the import library).

This compares the two symbol sets directly:
    DLL  -> dumpbin /exports   -> exported names (undecorated as stored, i.e. mangled)
    LIB  -> dumpbin /linkermember -> __imp_<name> for every exported function

A lib missing an exported symbol means anything that links against it and calls that
symbol fails at link time, even though the DLL would have provided it at run time.

Usage:
    python tools/check_lib_dll_match.py --dll bin64/amd64/DuiLib.dll \
                                        --lib bin64/amd64/DuiLib.lib
    python tools/check_lib_dll_match.py ... --inject   # self-test: expect a mismatch
"""
import argparse
import os
import re
import subprocess
import sys
import sys as _sys, pathlib as _pl
_sys.path.insert(0, str(_pl.Path(__file__).resolve().parent))
import find_msvc_tool  # [R87] single source of truth for dumpbin


def find_dumpbin():
    # [R87] Single source of truth. This used to be a third copy of a
    # hard-coded MSVC toolset path; a VS upgrade would have broken this tool
    # and the other two at once, each reporting "dumpbin missing".
    return find_msvc_tool.find_dumpbin()


def dll_exports(dumpbin, dll):
    """Return the set of names in the DLL's export table (mangled as stored)."""
    out = subprocess.run([dumpbin, '/nologo', '/exports', dll],
                         capture_output=True, text=True, errors='replace')
    names = set()
    for line in out.stdout.splitlines():
        # Rows look like:  "   12    3 00001234 ?Name@Class@@QEAAXXZ"
        m = re.match(r'\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)', line)
        if m:
            names.add(m.group(1))
    return names


def lib_imports(dumpbin, lib):
    """Return the set of exported names the LIB provides as __imp_ symbols."""
    out = subprocess.run([dumpbin, '/nologo', '/linkermember', lib],
                         capture_output=True, text=True, errors='replace')
    names = set()
    for line in out.stdout.splitlines():
        # Rows look like:  "    901A8 __imp_??0CDuiString@DuiLib@@QEAA@..."
        # The offset comes first, so a startswith test on the stripped line never
        # matched -- measured round 87: lib_imports parsed as 0 while the file had
        # 5280 such rows. The empty-parse guard is what surfaced it instead of the
        # tool reporting a bogus mismatch.
        idx = line.find('__imp_')
        if idx < 0:
            continue
        sym = line[idx:].split()[0]
        names.add(sym[len('__imp_'):])
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dll', required=True)
    ap.add_argument('--lib', required=True)
    ap.add_argument('--inject', action='store_true',
                    help='self-test: require a mismatch to be reported')
    args = ap.parse_args()

    dumpbin = find_dumpbin()
    if dumpbin is None:
        print('[FAIL] dumpbin.exe not found -- cannot compare')
        return 2
    if not os.path.isfile(args.dll):
        print('[FAIL] dll not found: %s' % args.dll)
        return 2
    if not os.path.isfile(args.lib):
        print('[FAIL] lib not found: %s' % args.lib)
        return 2

    exported = dll_exports(dumpbin, args.dll)
    provided = lib_imports(dumpbin, args.lib)
    print('[INFO] dll_exports=%d lib_imports=%d' % (len(exported), len(provided)))

    if len(exported) == 0 or len(provided) == 0:
        print('[FAIL] one side parsed as empty -- a scan that reads nothing cannot pass')
        return 2

    if args.inject:
        # Simulate the round-55 shape: a symbol present in the DLL but absent from
        # the LIB. Without this the check could be reporting "match" for two empty
        # or identical sets and never exercise the failure path.
        fake = set(exported) | {'?InjectProbeMissingSymbol@@YAXXZ'}
        missing = fake - provided
        if missing:
            print('[PASS] inject: %d missing symbol(s) reported, e.g. %s'
                  % (len(missing), sorted(missing)[0]))
            return 1
        print('[FAIL] inject: fabricated mismatch not detected')
        return 3

    missing = exported - provided
    if missing:
        print('[FAIL] %d exported symbol(s) are missing from the import library:'
              % len(missing))
        for s in sorted(missing)[:10]:
            print('       ' + s)
        print('       anything linking this lib and calling them fails with LNK2001')
        print('       rebuild with /t:Rebuild so the lib is regenerated too')
        return 1

    print('[PASS] import library matches the DLL (%d functions)' % len(exported))
    return 0


if __name__ == '__main__':
    sys.exit(main())
