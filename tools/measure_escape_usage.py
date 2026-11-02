"""Measure where escape sequences actually appear, before judging any of them.

Round 75 exists because rounds 72/73/74 each hit the same failure two or more
times: an escape written through a nested layer (shell -> heredoc -> Python ->
target file) becomes a real control byte, and the damage is silent. The
conclusion "write values, not escapes" is well supported, but turning it into a
gate first needs numbers -- without them a scanner would flag every legitimate
regex and Windows path and be switched off within a week.

This module classifies escape usage by *purpose* so the gate can target the
dangerous shape only. Patterns are built from chr() deliberately: this file is
itself launched through layers that have mangled escapes repeatedly.
"""
import pathlib
import re

BS = chr(92)          # backslash
NL = chr(10)
CR = chr(13)
TAB = chr(9)

# Purposes. Each maps a name to a compiled pattern over the SOURCE text.
PURPOSES = {
    # A literal CRLF written as two escapes. Extremely common and legitimate.
    'crlf': BS + BS + 'r' + BS + BS + 'n',
    # A newline escape, not preceded by another backslash.
    'newline': '[^' + BS + BS + ']' + BS + BS + 'n',
    'tab': '[^' + BS + BS + ']' + BS + BS + 't',
    # A backspace escape: backslash-b NOT followed by a word character. This is
    # the one that becomes 0x08 and eats the next character.
    'backspace': BS + BS + 'b(?![A-Za-z0-9_])',
    # A doubled backslash: the correct way to write one literal backslash.
    'escaped_backslash': BS + BS + BS + BS,
    # Regex character classes.
    'regex_class': BS + BS + '[dwsDWS]',
}


def measure(paths):
    """Return {purpose: (hits, set_of_filenames)} over `paths`."""
    compiled = {k: re.compile(v) for k, v in PURPOSES.items()}
    hits = {k: 0 for k in compiled}
    files = {k: set() for k in compiled}
    scanned = 0
    for p in paths:
        try:
            text = p.read_text(encoding='utf-8', errors='replace')
        except OSError:
            continue
        scanned += 1
        for k, rx in compiled.items():
            n = len(rx.findall(text))
            if n:
                hits[k] += n
                files[k].add(p.name)
    return scanned, hits, files


if __name__ == '__main__':
    roots = [pathlib.Path('tools'), pathlib.Path('.')]
    seen, paths = set(), []
    for r in roots:
        if not r.is_dir():
            continue
        for p in sorted(r.glob('*.py')):
            if p.resolve() in seen or '.box-agent-scratch' in str(p):
                continue
            seen.add(p.resolve())
            paths.append(p)
    scanned, hits, files = measure(paths)
    print('files_scanned=%d' % scanned)
    for k in PURPOSES:
        print('  %-18s hits=%-6d files=%d' % (k, hits[k], len(files[k])))
