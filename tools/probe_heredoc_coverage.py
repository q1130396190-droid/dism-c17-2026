"""RETIRED diagnostic (round 82 -> 84). Superseded by the checks it justified.


What it did: reproduced the three round-80 escape accidents (BEL, TAB, a real
newline) and reported which result scanners would catch each one.


Why it is retired: its conclusion is now encoded where it matters, so running
this again adds nothing.


  * BEL (0x07) is in check_control_bytes.FORBIDDEN -- always was.
  * TAB (0x09) inside a quoted shell path is now scanned by
    check_control_bytes.scan_embedded_whitespace() and refused before write by
    patch_write. Round 82 added both; the round-82 report records the
    7139-false-positive first attempt and the three-part predicate that fixed it.
  * A real newline (C2001) is a compiler diagnostic and cannot be caught by any
    byte scanner -- documented as a boundary in
    docs/æºç æ¹åçº¦å®-ä¸è¦åè½¬ä¹.md section 3.2.

Kept as a file rather than deleted because this repository has no version
control, so deletion is irreversible. Running it still works and still prints
the same table -- it is simply not part of any gate and not in the baseline as
an active tool.
"""

import pathlib
import sys

sys.path.insert(0, 'tools')
from check_control_bytes import scan_file  # noqa: E402

BS = 0x5C
OUT = pathlib.Path('.box-agent-scratch/heredoc_cases')


def build_cases():
    """Return {label: (filename, bytes)} matching the three measured accidents."""
    cases = {}

    # 1) A Windows path with the backslash-a pair turned into BEL.
    #    Measured round 80: 'bin64\amd64_debug' became 'bin64' + BEL + 'md64_debug'.
    cases['BEL in path'] = (
        'bel.ps1',
        b'$d = Join-Path $root \'bin64' + bytes([0x07]) + b'md64_debug\'\n')

    # 2) A dot-backslash path where the backslash-t pair became TAB.
    #    Measured: "& '.\thread_affinity_gate.exe'" became "& '.TABhread...'".
    cases['TAB in path'] = (
        'tab.ps1',
        b'    & \'.' + bytes([0x09]) + b'hread_affinity_gate.exe\' @argsList\n')

    # 3) A newline escape expanded inside a C string literal -- a real LF inside
    #    the quote, which is C2001 "newline in string literal". The bytes are NOT a
    #    control character problem: LF is legitimate whitespace, so a byte scanner
    #    cannot distinguish "line break" from "line break inside a literal".
    cases['LF inside literal'] = (
        'lf.cpp',
        b'    ::printf("mode not asserted\n");\n')

    return cases


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    cases = build_cases()
    print('%-20s %-10s %s' % ('case', 'scanner', 'verdict'))
    for label, (fname, blob) in cases.items():
        f = OUT / fname
        f.write_bytes(blob)
        hits = scan_file(f)
        if hits:
            verdict = 'CAUGHT (%d hit(s): %s)' % (
                len(hits), ', '.join(sorted({hex(b) for _, b, _ in hits})))
        else:
            verdict = 'NOT CAUGHT'
        print('%-20s %-10s %s' % (label, 'bytes', verdict))
    print()
    print('Interpretation (corrected in round 84):')
    print('  case 1 (BEL) was already caught by scan_file.')
    print('  case 2 (TAB) was NOT caught when this probe was written: 0x09 is')
    print('    legitimate whitespace and is deliberately absent from FORBIDDEN.')
    print('    Round 82 added scan_embedded_whitespace() for the quoted-path shape,')
    print('    so it is now covered by a separate predicate, not by scan_file.')
    print('  case 3 (LF) cannot be caught by any byte scanner: LF is legal')
    print('    whitespace, so an expanded one is indistinguishable from an intended')
    print('    line break. Only the compiler reports it (C2001).')
    print()
    print('The original line here claimed cases 1 and 2 were both closed by the byte')
    print('scanner. That was wrong for case 2, and this probe is retained partly as')
    print('a record of that -- see the round-82 chapter.')


if __name__ == '__main__':
    sys.exit(main())
