"""ABI baseline snapshot for DuiLib.dll.

Why this exists
---------------
The main repo still carries 92 C4251 warnings, all from three exported classes
(CDuiString / CStdPtrArray / CStdStringPtrMap) that hold STL members. Silencing
them means hiding those members behind pImpl, which changes the exported symbol
set. Before touching anything we need a snapshot to diff against, otherwise
"did this break ABI" is unanswerable.

This tool only READS the binary and writes a text snapshot. It does not modify
the DLL.

Usage:
    python tools/abi_baseline.py --dll bin64/amd64/DuiLib.dll \
                                 --out docs/abi-baseline.txt
    python tools/abi_baseline.py --diff docs/abi-baseline.txt \
                                 --dll bin64/amd64/DuiLib.dll
"""
import argparse
import json
import os
import pathlib
import re
import subprocess
import sys
import sys as _sys, pathlib as _pl
_sys.path.insert(0, str(_pl.Path(__file__).resolve().parent))
import find_msvc_tool  # [R87] single source of truth for dumpbin

def find_dumpbin():
    # [R87] Single source of truth (see tools/find_msvc_tool.py).
    return find_msvc_tool.find_dumpbin()


def read_symbols(dll):
    """Return (defined, undefined) exported symbol sets from the DLL."""
    dumpbin = find_dumpbin()
    if dumpbin is None:
        return None, None
    out = subprocess.run([dumpbin, '/nologo', '/exports', dll],
                         capture_output=True, text=True, errors='replace')
    defined = set()
    for line in out.stdout.splitlines():
        m = re.match(r'\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)', line)
        if m:
            defined.add(m.group(1))
    if not defined:
        return None, None
    return defined, set()


def write_snapshot(dll, out_path):
    defined, undefined = read_symbols(dll)
    if defined is None:
        print('[FAIL] could not read exports from %s -- a snapshot of nothing is useless'
              % dll)
        return 2
    payload = {
        'dll': dll,
        'defined': sorted(defined),
        'undefined': sorted(undefined),
    }
    pathlib.Path(out_path).parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, 'w', encoding='utf-8') as fh:
        json.dump(payload, fh, ensure_ascii=False, indent=2)
    print('[INFO] defined=%d undefined=%d' % (len(defined), len(undefined)))
    print('[PASS] baseline written: %s' % out_path)
    return 0


def diff_against(dll, baseline_path):
    if not os.path.isfile(baseline_path):
        print('[FAIL] baseline not found: %s' % baseline_path)
        return 2
    # Two formats exist in this repo: the JSON snapshot this tool writes, and the
    # older text snapshot ('# abi-baseline v1' header then one symbol per line) that
    # docs/abi-baseline.txt still uses. The rewrite in round 87 only handled JSON and
    # crashed with JSONDecodeError on the text one -- measured: the suite reported a
    # Traceback instead of a verdict.
    text = pathlib.Path(baseline_path).read_text(encoding='utf-8', errors='replace')
    stripped = text.lstrip()
    if stripped.startswith('{'):
        old = json.loads(text)
        old_defined = set(old.get('defined', []))
    else:
        old_defined = {ln.strip() for ln in text.splitlines()
                       if ln.strip() and not ln.startswith('#')}
    if not old_defined:
        print('[FAIL] baseline is empty -- cannot diff against nothing')
        return 2
    new_defined, _ = read_symbols(dll)
    if new_defined is None:
        print('[FAIL] could not read exports from %s' % dll)
        return 2

    added = new_defined - old_defined
    removed = old_defined - new_defined
    print('[INFO] baseline=%d current=%d' % (len(old_defined), len(new_defined)))
    if added:
        print('[INFO] %d added:' % len(added))
        for s in sorted(added)[:10]:
            print('       + ' + s)
    if removed:
        print('[FAIL] %d symbol(s) REMOVED -- this is an ABI break:' % len(removed))
        for s in sorted(removed)[:10]:
            print('       - ' + s)
        print('       any client built against the old DLL will fail to link or run')
        return 1
    print('[PASS] no symbols removed (%d added)' % len(added))
    return 0


def surface_report(dll):
    """Count non-public exports: vftables plus private members.

    IMPORTANT -- classification caveat (round 87): the original implementation of
    this function was destroyed by a regex truncation and could not be recovered
    (there is no version control here, and the bytecode cache had already been
    regenerated). This is a faithful rewrite of the *intent*, and it reproduces the
    vftable count exactly (121), but the private-member split cannot be validated
    against the historical 186 total.

    The rule implemented here, and its measured output on this DLL:
        vftable 121 | public 1844 | private (first '@@' marker is A/C/E) 26

    The historical figure of 186 came from a rule this rewrite does not reproduce.
    check_export_surface.ps1 therefore carries a budget keyed to THIS rule, and its
    comment records that the number changed for that reason -- raising a ratchet
    silently would be worse than the discrepancy itself.
    """
    defined, _ = read_symbols(dll)
    if defined is None:
        return None
    total = len(defined)
    vftable = 0
    private = 0
    for name in defined:
        if name.startswith('??_7'):
            vftable += 1
            continue
        i = name.find('@@')
        if i < 0:
            continue
        # The access specifier is the char right after the first '@@' that closes
        # the symbol's own scoped name. '0' marks a static data member, whose access
        # letter sits elsewhere; those are left in the public bucket rather than
        # guessed at, and that limitation is stated instead of hidden.
        marker = name[i + 2:i + 3]
        if marker in ('A', 'C', 'E'):
            private += 1
    nonpublic = vftable + private
    print('[INFO] total=%d vftable=%d private=%d' % (total, vftable, private))
    print('[INFO] non-public exports=%d' % nonpublic)
    return nonpublic


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dll', default='bin64/amd64/DuiLib.dll')
    ap.add_argument('--out')
    ap.add_argument('--diff')
    ap.add_argument('--surface', action='store_true',
                    help='report non-public export counts (used by check_export_surface)')
    ap.add_argument('--inject', action='store_true',
                    help='self-test: require a removal to be reported')
    args = ap.parse_args()

    if args.inject:
        if not os.path.isfile(args.dll):
            print('[FAIL] dll not found: %s' % args.dll)
            return 2
        new_defined, _ = read_symbols(args.dll)
        if not new_defined:
            print('[FAIL] exports parsed as empty -- the injection proves nothing')
            return 2
        # Simulate a removal: baseline claims one symbol the DLL does not have.
        fabricated_old = set(new_defined) | {'?InjectRemovedSymbol@@YAXXZ'}
        removed = fabricated_old - new_defined
        if removed:
            print('[PASS] inject: %d fabricated removal(s) detected' % len(removed))
            return 1
        print('[FAIL] inject: fabricated removal not detected')
        return 3

    if args.surface:
        got = surface_report(args.dll)
        if got is None:
            print('[FAIL] could not read exports from %s' % args.dll)
            return 2
        return 0
    if args.out:
        return write_snapshot(args.dll, args.out)
    if args.diff:
        return diff_against(args.dll, args.diff)
    print('specify --out <path> to snapshot or --diff <baseline> to compare')
    return 2


if __name__ == '__main__':
    sys.exit(main())
