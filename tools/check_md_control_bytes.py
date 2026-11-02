"""Narrow scan: a literal 0x08 inside a Markdown document.

Why a separate, deliberately weak check instead of extending check_control_bytes:

  * check_control_bytes.py excludes .md on purpose. Prose legitimately quotes
    escape sequences as examples ("write r'\\b' and the regex becomes a
    backspace"), and flagging those examples would train people to ignore the
    check. That reasoning is still correct.
  * But round 72 found FIVE real 0x08 bytes already sitting in the big report,
    left there by earlier rounds. One of them had eaten a path separator:
    "...\\DuiLib_Ultimate-master<0x08>in64\\Win8Metro.exe" was the mangled form
    of "...\\bin64\\Win8Metro.exe". A reader would never spot that, and no gate
    looked at .md at all.

So the compromise is: only 0x08, and only where it is NOT part of a legitimate
escape-sequence example. A quoted example in prose always has a character that
makes it read as a backslash-b (a backtick, a quote, or the word that follows).
A corrupted byte is a byte sitting between ordinary text characters.

That heuristic is intentionally not clever. It reports what needs a human look;
it does not decide. Exit codes:
    0  no suspicious 0x08
    1  suspicious 0x08 found (message names file and line)
    2  zero documents scanned -- a scan that reads nothing cannot pass
    3  --inject was requested and nothing was reported (the check is blind)
"""
import argparse
import pathlib
import sys
import sys as _sys, pathlib as _pl
_sys.path.insert(0, str(_pl.Path(__file__).resolve().parent))
# [R87] shared skip set: these two files were the copies that lacked
# ".git", so they walked the object store on every scan.
from scan_scope import SKIP_DIRS

# Only 0x08. Other control bytes in Markdown are almost always a paste artifact
# rather than an expanded escape, and widening the net here would reproduce the
# false-positive problem that kept .md out of check_control_bytes in the first place.
TARGETS = (0x08, 0x07, 0x00)
TARGET = 0x08   # kept for the --inject path

# Contexts that make a 0x08 look like a deliberate example rather than corruption.
# A backtick before it means "this is inside a code span"; a quote means it is a
# quoted string in an example.
BENIGN_PREFIX = {0x60, 0x27, 0x22, 0x5C}   # ` ' " \


def scan_md(path):
    """Return [(line, reason)] for suspicious 0x08 occurrences in `path`."""
    raw = path.read_bytes()
    out = []
    for i, b in enumerate(raw):
        if b not in TARGETS:
            continue
        line = raw.count(0x0A, 0, i) + 1
        prev = raw[i - 1] if i > 0 else 0
        nxt = raw[i + 1] if i + 1 < len(raw) else 0
        if prev in BENIGN_PREFIX and b != 0x00:
            # Reads as `...`, '...', "...  or a doubled backslash: an example.
            continue
        reason = 'between 0x%02X and 0x%02X' % (prev, nxt)
        out.append((line, reason))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', action='append', default=None,
                    help='directory to walk (default: docs)')
    ap.add_argument('--inject', action='store_true',
                    help='self-test: prove the check reports and returns 1')
    args = ap.parse_args()

    dirs = args.dir or ['docs']
    docs = []
    for d in dirs:
        root = pathlib.Path(d)
        if not root.is_dir():
            print('[WARN] directory not found: %s' % root)
            continue
        for f in root.rglob('*.md'):
            parts = {x.lower() for x in f.parts}
            if parts & SKIP_DIRS:
                continue
            docs.append(f)

    if not docs:
        print('[FAIL] zero markdown documents scanned -- nothing can pass this way')
        return 2

    findings = []
    for f in docs:
        for line, reason in scan_md(f):
            findings.append((str(f), line, reason))

    print('[INFO] documents_scanned=%d' % len(docs))

    if args.inject:
        # Append a synthetic document so the inject path does not depend on the
        # real docs being clean (a dirty repo would make this self-test vacuous).
        scratch = pathlib.Path('.box-agent-scratch') / 'mdctrl_inject'
        scratch.mkdir(parents=True, exist_ok=True)
        probe = scratch / 'inject.md'
        probe.write_bytes(b'path C:' + bytes([0x5C]) + b'DuiLib' + bytes([TARGET])
                          + b'in64' + bytes([0x5C]) + b'x.exe\n')
        hits = scan_md(probe)
        if hits:
            print('[PASS] inject: reported L%d (%s), so the check can fail'
                  % (hits[0][0], hits[0][1]))
            return 1
        print('[FAIL] inject: expected a hit in the synthetic document, got none')
        return 3

    if findings:
        print('[FAIL] %d suspicious control byte(s) in markdown:' % len(findings))
        for path, line, reason in findings[:20]:
            print('       %-58s L%-5d %s' % (path, line, reason))
        if len(findings) > 20:
            print('       ... and %d more' % (len(findings) - 20))
        print('       a 0x08 here is an expanded escape, not prose: it eats the')
        print('       following character (a path separator becomes 0x08)')
        return 1
        print("       these are expanded escapes, not prose: 0x08 <- backslash-b,")
        print("       0x07 <- backslash-a, 0x00 <- backslash-0. Each eats or truncates")
        print("       the text after it, so a path or identifier silently loses a char.")
    print('[PASS] no suspicious 0x08 in %d markdown document(s)' % len(docs))
    return 0


if __name__ == '__main__':
    sys.exit(main())
