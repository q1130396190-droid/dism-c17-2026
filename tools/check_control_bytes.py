"""Reject control bytes in tracked scripts and sources.

Why: writing regex escapes through a patch layer has silently produced literal
control characters seven times in this project:

    r'\b'  ->  r'<backspace>'   0x08

The regex then matches a backspace instead of a word boundary. Nothing errors; the
check just stops matching, and the failure looks exactly like "the code is clean".
Every occurrence was found only by counting bytes after the fact -- which is too
late, and depends on someone thinking to look.

A 0x08 (or 0x00, 0x07, 0x0B ...) inside a .py/.ps1/.cpp/.h is never intentional.
Any hit fails. The scan is cheap enough to run in the suite.

Usage:
    python tools/check_control_bytes.py --dir tools --dir DuiLib
    python tools/check_control_bytes.py --dir . --inject   # self-test
"""
import argparse
import pathlib
import sys
import sys as _sys, pathlib as _pl
_sys.path.insert(0, str(_pl.Path(__file__).resolve().parent))
# [R87] shared skip set: these two files were the copies that lacked
# ".git", so they walked the object store on every scan.
from scan_scope import SKIP_DIRS

# Bytes that must never appear in text sources. 0x09/0x0A/0x0D are legitimate
# whitespace; 0x0C (form feed) is accepted because some editors still emit it.
FORBIDDEN = {
    0x00: 'NUL', 0x01: 'SOH', 0x02: 'STX', 0x03: 'ETX', 0x04: 'EOT',
    0x05: 'ENQ', 0x06: 'ACK', 0x07: 'BEL', 0x08: 'BS',
    0x0B: 'VT', 0x0E: 'SO', 0x0F: 'SI', 0x10: 'DLE', 0x11: 'DC1',
    0x12: 'DC2', 0x13: 'DC3', 0x14: 'DC4', 0x15: 'NAK', 0x16: 'SYN',
    0x17: 'ETB', 0x18: 'CAN', 0x19: 'EM', 0x1A: 'SUB', 0x1C: 'FS',
    0x1D: 'GS', 0x1E: 'RS', 0x1F: 'US',
}

EXTS = {'.py', '.ps1', '.cpp', '.h', '.hpp', '.c', '.cs', '.bat', '.cmd',
        # Skin XML and config JSON were added in round 59: a control character
        # in these makes the parser fail silently, which this project already hit
        # with an XML comment containing '--' (round 30).
        '.xml', '.json', '.ini', '.cfg'}

# .md and .txt stay out on purpose: prose legitimately quotes escape sequences
# as examples (this file's own docstring does), and flagging those would train
# people to ignore the check.


def scan_file(p):
    """Return [(offset_line, byte, name)] for forbidden control bytes.

    UTF-16 files are skipped: every ASCII character in them is followed by a NUL
    byte, so a byte-level scan reports thousands of "NUL" hits that are just the
    encoding. UIIPAddressEx.cpp is UTF-16LE and produced 14,260 of them in the first
    run -- which buried the three real findings underneath.
    """
    try:
        raw = p.read_bytes()
    except OSError:
        return []
    if raw[:2] in (bytes([0xFF, 0xFE]), bytes([0xFE, 0xFF])):
        return []
    hits = []
    for i, b in enumerate(raw):
        if b in FORBIDDEN:
            line = raw.count(0x0A, 0, i) + 1
            hits.append((line, b, FORBIDDEN[b]))
    return hits



def scan_embedded_whitespace(path):
    """Return [(line, byte, name)] for a TAB sitting inside a shell path literal.

    Why this exists (round 82): the byte scanner allows 0x09 because indentation is
    legitimate. But a TAB inside a quoted PATH is not indentation -- it is the
    residue of a backslash-t pair expanded while writing the script through a
    heredoc. Round 80 measured exactly that:

        & '.	hread_affinity_gate.exe'   ->   & '.<TAB>hread_affinity_gate.exe'

    Scope is deliberately narrow. A first attempt flagged any TAB preceded by
    non-whitespace and produced 7139 hits, essentially all of them legitimate
    alignment inside generated C++ (e.g. a Python file whose helper emits
    '		struct D2DTextState'). A check with a 7000-line report is a check nobody
    reads, which is the same failure the escape census avoided in round 75.

    So: shell files only, and only when the TAB sits inside a quoted run that also
    looks like a path (has a path separator or a dot-extension). Anything else --
    indentation, alignment, embedded source -- is left alone.
    """
    if path.suffix.lower() not in {'.ps1', '.bat', '.cmd'}:
        return []
    try:
        raw = path.read_bytes()
    except OSError:
        return []
    if raw[:2] in (bytes([0xFF, 0xFE]), bytes([0xFE, 0xFF])):
        return []
    hits = []
    for i, b in enumerate(raw):
        if b != 0x09:
            continue
        start = raw.rfind(0x0A, 0, i) + 1
        end = raw.find(0x0A, i)
        if end < 0:
            end = len(raw)
        line_bytes = raw[start:end]
        prefix = raw[start:i]
        if not prefix.strip():
            continue                       # indentation only
        # Must be inside a quote on this line.
        before = prefix
        sq = bytes([0x27])
        dq = bytes([0x22])
        if (before.count(sq) % 2 == 0) and (before.count(dq) % 2 == 0):
            continue                       # not inside a quoted run
        suffix = raw[i:end]
        window = (before[-40:] + suffix[:40]).lower()
        looks_like_path = (chr(0x2F).encode() in window) or (chr(0x5C).encode() in window)
        has_ext = any(x in window for x in (b'.exe', b'.dll', b'.lib', b'.ps1', b'.py'))
        if not (looks_like_path or has_ext):
            continue
        line = raw.count(0x0A, 0, i) + 1
        hits.append((line, b, 'TAB inside a quoted path'))
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', action='append', default=None)
    ap.add_argument('--file', action='append', default=None,
                    help='check specific file(s) instead of walking directories. '
                         'This is the write-time mode: call it right after editing a '
                         'file, before the mistake reaches the suite.')
    ap.add_argument('--inject', action='store_true',
                    help='self-test: require at least one hit')
    args = ap.parse_args()

    if args.file:
        # Write-time mode: no directory walk, no skip rules, no UTF-16 leniency for
        # files the caller explicitly asked about. Explicit beats implicit here -- if
        # someone names a file they want an answer about that file.
        files = [pathlib.Path(f) for f in args.file]
        missing = [f for f in files if not f.is_file()]
        if missing:
            print('[FAIL] file not found: %s' % missing[0])
            return 2
        findings = []
        for f in files:
            for line, b, name in scan_file(f):
                findings.append((str(f), line, b, name))
        print('[INFO] files_checked=%d' % len(files))
        if findings:
            print('[FAIL] %d control byte(s):' % len(findings))
            for path, line, b, name in findings[:20]:
                print('       %-56s L%-5d 0x%02X (%s)' % (path, line, b, name))
            return 1
        print('[PASS] no control bytes in %d named file(s)' % len(files))
        return 0

    dirs = args.dir or ['tools', 'DuiLib', 'Demos']
    files = []
    for d in dirs:
        root = pathlib.Path(d)
        if not root.is_dir():
            print('[WARN] directory not found, skipping: %s' % root)
            continue
        for f in root.rglob('*'):
            if f.is_file() and f.suffix.lower() in EXTS:
                # Skip vendored/third-party trees and build output.
                parts = {x.lower() for x in f.parts}
                if parts & SKIP_DIRS:
                    continue
                files.append(f)

    if not files:
        print('[FAIL] zero files scanned -- a scan that reads nothing cannot pass')
        return 2

    findings = []
    for f in files:
        for line, b, name in scan_file(f):
            findings.append((str(f), line, b, name))
        # Round 82: TAB is legal indentation, so scan_file skips it. Embedded TAB
        # is the residue of an expanded backslash-t and is reported separately.
        for line, b, name in scan_embedded_whitespace(f):
            findings.append((str(f), line, b, name))

    print('[INFO] files_scanned=%d forbidden_bytes=%d'
          % (len(files), len(FORBIDDEN)))
    if findings:
        print('[FAIL] %d control byte(s) found in text sources:' % len(findings))
        for path, line, b, name in findings[:20]:
            print('       %-56s L%-5d 0x%02X (%s)' % (path, line, b, name))
        if len(findings) > 20:
            print('       ... and %d more' % (len(findings) - 20))
    else:
        print('[PASS] no stray control bytes')

    if args.inject:
        if findings:
            print('[PASS] inject: %d hit(s) reported, so the check can fail'
                  % len(findings))
            return 1
        print('[FAIL] inject: expected at least one hit, got none')
        return 3

    return 1 if findings else 0


if __name__ == '__main__':
    sys.exit(main())
