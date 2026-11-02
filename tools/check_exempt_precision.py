"""Exemption lookups must not use prefix matching.

Why (round 85-86): a single EXEMPT entry keyed on '%s' absorbed every future
dynamic section, and a planted "'%s: dynamic section'" passed silently. The
mechanism was

    exempt = any(normalize(k).startswith(norm) for k in EXEMPT)

One loose rule hid an entire class of gap, and did so in the PASS direction.

Round 86 then found the same looseness in a second place -- the tool's own
--inject branch still used a prefix match while the real verdict had been
tightened, so the self-test and the real path could disagree about the same
question. That is the shape worth guarding against: not one bad line, but a
looseness that spreads to copies of the logic.

What this checks
----------------
In the tools that make allow/deny decisions, find comparisons against an
exemption/allow-list style container and require exact equality. Prefix matching
is legitimate for parsing (stripping comment markers, classifying statement
heads) and is left alone there -- the check keys on the CONTAINER NAME, not on
`startswith` in general, so it does not flag parsing code.

Deliberately narrow: three container names, two tools. Widening it would start
guessing at semantics it cannot see.

Exit codes:
    0  no exemption lookup uses prefix matching
    1  one does (names file and line)
    2  zero lookups found -- the scan matched nothing, so a PASS is meaningless
    3  --inject planted a prefix-based exemption and it was not reported
"""
import argparse
import pathlib
import re
import sys

# Containers that hold exemptions / allow-lists. Names chosen because they are
# what this repository actually uses for that role.
ALLOW_CONTAINERS = ('EXEMPT', 'ALLOW', 'KNOWN_DYNAMIC', 'STRUCTURAL')

# Tools that make allow/deny decisions.
TOOLS = [
    'tools/check_repair_case_coverage.py',
    'tools/check_oneshot_scripts.py',
    'tools/probes/gate_inventory.ps1',
]


def scan_python(path):
    """Return [(line, text)] where an allow container is used with startswith."""
    hits = []
    p = pathlib.Path(path)
    if not p.is_file():
        return hits
    for i, line in enumerate(p.read_text(encoding='utf-8', errors='replace').splitlines(), 1):
        if 'startswith' not in line:
            continue
        if not any(c in line for c in ALLOW_CONTAINERS):
            continue
        # A call like  k.startswith(x) for k in EXEMPT   is the loose shape.
        # The argument may itself contain parentheses (norm(key)), so the old
        # [^)]* shape stopped at the inner one and never matched -- measured
        # round 86: the planted sample was not detected. Match on the SHAPE
        # ('.startswith(' ... ' for x in <container>') rather than balancing parens.
        # Plain string tests, no regex at all. Every attempt to express this with
        # a pattern was mangled in transit (rounds 72-86), and the shape does not
        # need regex power: the call site and the container name are both literal.
        if '.startswith(' in line:
            for c in ALLOW_CONTAINERS:
                if (' for ' in line) and ((' in ' + c) in line):
                    hits.append((i, line.strip()))
                    break
        if False:
            hits.append((i, line.strip()))
    return hits


def scan_powershell(path):
    """Same question for the .ps1 tools: -like/-match against an allow table."""
    hits = []
    p = pathlib.Path(path)
    if not p.is_file():
        return hits
    for i, line in enumerate(p.read_text(encoding='utf-8', errors='replace').splitlines(), 1):
        if 'Contains' not in line and '-like' not in line:
            continue
        if not any(c in line for c in ALLOW_CONTAINERS):
            continue
        hits.append((i, line.strip()))
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--inject', action='store_true',
                    help='self-test: plant a prefix-based exemption and require a report')
    args = ap.parse_args()

    if args.inject:
        plant = pathlib.Path('tools') / ('check_inject' + '_loose_exempt.py')
        plant.write_text(
            'EXEMPT = ["a: doc"]' + chr(10) +
            'def norm(s):' + chr(10) +
            '    return s' + chr(10) +
            'def check(key):' + chr(10) +
            '    return any(norm(k).startswith(norm(key)) for k in EXEMPT)' + chr(10),
            encoding='utf-8')
        try:
            hits = scan_python(plant)
        finally:
            try:
                plant.unlink()
            except OSError:
                pass
        if hits:
            print('[PASS] inject: a prefix-based exemption is detected (line %d)' % hits[0][0])
            return 3
        print('[FAIL] inject: planted a prefix-based exemption and the scan found nothing')
        return 0

    total = 0
    findings = []
    for rel in TOOLS:
        p = pathlib.Path(rel)
        if not p.is_file():
            continue
        total += 1
        got = scan_python(p) if rel.endswith('.py') else scan_powershell(p)
        for line, text in got:
            findings.append((rel, line, text))

    if total == 0:
        print('[FAIL] zero decision tools inspected -- a PASS here would be meaningless')
        return 2
    if not any(pathlib.Path(r).is_file() for r in TOOLS):
        print('[FAIL] none of the expected tools exist -- the scan read nothing')
        return 2

    print('[INFO] tools_inspected=%d' % total)
    if findings:
        print('[FAIL] %d exemption lookup(s) use prefix matching:' % len(findings))
        for rel, line, text in findings:
            print('       %-46s L%-4d %s' % (rel, line, text[:70]))
        print('       a prefix match on an allow-list approves everything that starts')
        print('       with the key -- round 85 measured one %s entry hiding a whole class;')
        print('       use exact equality, or count known sites explicitly')
        return 1

    print('[PASS] no exemption lookup uses prefix matching')
    return 0


if __name__ == '__main__':
    sys.exit(main())
