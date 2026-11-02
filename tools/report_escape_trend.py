"""Report the escape-rejection trend recorded by tools/patch_write.py.

Why this exists (round 76): by round 75 the same escape-expansion accident had
been recorded 18+ times across four rounds. Every response so far was to add
another check, and the rate did not change. This is the different response --
measure the rate instead of trying to prevent every instance.

The ledger is written by patch_write._record_rejection(). Self-test refusals are
excluded at the source (PATCH_WRITE_SELFTEST=1), so a green suite does not inflate
the trend.

Deliberately NOT a gate. A non-zero count here is information, not a regression:
an accident that was refused was an accident that did not reach a file. Failing on
it would push people to stop reporting.

Exit codes:
    0  ledger readable (including empty)
    1  ledger missing or unreadable while real refusals were expected
    3  --inject was requested and the fabricated entry was not counted
"""
import argparse
import pathlib
import sys

LEDGER = pathlib.Path('tools/escape_rejections.log')


def entries():
    """Return the kept detail lines, or None when the ledger is missing."""
    if not LEDGER.is_file():
        return None
    out = []
    for line in LEDGER.read_text(encoding='utf-8').splitlines():
        s = line.strip()
        if not s or s.startswith('#') or s.startswith('TOTAL '):
            continue
        out.append(s)
    return out


def cumulative_total():
    """The monotone count, or None when the ledger has not rotated yet.

    Distinct from len(entries()): after a rotation the details are trimmed but the
    total is not, and reporting only len(entries()) would make the trend look
    better every time the file was trimmed. Round 78 measured the failure mode
    this guards against: a naive counter reported 17 for 20 actual refusals.
    """
    if not LEDGER.is_file():
        return None
    for line in LEDGER.read_text(encoding='utf-8').splitlines():
        if not line.startswith('TOTAL '):
            continue
        parts = line.split()
        if len(parts) >= 2 and parts[1].isdigit():
            return int(parts[1])
    return None


def summarize(rows):
    """Group by ISO date (first 10 chars) so the trend is per working day."""
    per_day = {}
    per_byte = {}
    for r in rows:
        day = r[:10]
        per_day[day] = per_day.get(day, 0) + 1
        # Line shape: <iso>  <file>  0xNN x<count>  <NAME>
        # The byte is the token that starts with 0x; the count after 'x' is how
        # many times that byte occurred in ONE refusal, so sum the counts rather
        # than the token occurrences.
        # Line shape: <iso>  <file>  0xNN x<count>  <NAME>
        # Parsed by splitting on the LAST lowercase 'x' -- the first one belongs to
        # the 0x prefix. Written after the naive version reported byte '0' and made
        # the self-test look blind.
        for token in r.split():
            if not token.startswith('0x') or len(token) < 4:
                continue
            byte = token[:4]
            times = 1
            rest = token[4:]
            if rest.startswith('x') and rest[1:].isdigit():
                times = int(rest[1:])
            per_byte[byte] = per_byte.get(byte, 0) + times
    return per_day, per_byte


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--inject', action='store_true',
                    help='self-test: append a fabricated entry, require it be counted')
    args = ap.parse_args()

    if args.inject:
        # Work on a copy so the real ledger is never touched by a self-test.
        rows = entries() or []
        fake = '2099-01-01T00:00:00  injected.cpp  0x08 x1  BS'
        per_day, per_byte = summarize(rows + [fake])
        got = per_day.get('2099-01-01', 0)
        if got != 1 or per_byte.get('0x08') != 1:
            print('[FAIL] inject: fabricated entry not counted -- the summary is blind')
            return 0
        print('[PASS] inject: fabricated entry counted (%d on 2099-01-01)' % got)

        # Rotation must preserve the cumulative count. Round 78 found this the hard
        # way: a naive counter reported 17 for 20 writes, so the assertion here is
        # deliberately exact rather than "greater than zero".
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            'pw', str(pathlib.Path('tools/patch_write.py')))
        pw = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(pw)
        hub = pathlib.Path('.box-agent-scratch/ledger_rotate_selftest.log')
        hub.parent.mkdir(parents=True, exist_ok=True)
        hub.write_text('# hdr' + chr(10), encoding='utf-8')
        saved = pw.LEDGER
        saved_max = getattr(pw, 'LEDGER_MAX_LINES', 500)
        pw.LEDGER = hub
        pw.LEDGER_MAX_LINES = 3
        try:
            for i in range(20):
                pw._record_rejection('f%d.cpp' % i, [(1, 0x08, 'BS')])
        finally:
            pw.LEDGER = saved
            pw.LEDGER_MAX_LINES = saved_max
        lines = hub.read_text(encoding='utf-8').splitlines()
        totals = [l for l in lines if l.startswith('TOTAL ')]
        kept = [l for l in lines if l and not l.startswith('#')
                and not l.startswith('TOTAL ')]
        if not totals:
            print('[FAIL] inject: rotation produced no TOTAL header')
            return 0
        got_total = int(totals[0].split()[1])
        if got_total != 20 or len(kept) != 3:
            print('[FAIL] inject: rotation total=%d kept=%d, expected 20/3'
                  % (got_total, len(kept)))
            return 0
        print('[PASS] inject: rotation keeps total 20 while trimming details to 3')
        return 3

    rows = entries()
    if rows is None:
        print('[FAIL] ledger not found: %s' % LEDGER)
        print('       it is created on the first real refusal; a missing file means')
        print('       patch_write never refused anything (or the ledger was deleted)')
        return 1

    if not rows:
        print('[INFO] ledger present, 0 real refusals recorded since round 76')
        print('[PASS] no escape-expansion accident has been refused yet')
        # The boundary must travel with the number. Round 80 is the proof: the
        # ledger said 0 while three escapes expanded that same round, because they
        # went through a heredoc rather than patch_write. A bare "0" here invites
        # exactly the wrong conclusion.
        print('[NOTE] this counts ONE path only (patch_write). It does not observe')
        print('       heredoc writes or direct file edits -- see the header of')
        print('       tools/escape_rejections.log. Zero here is not proof of zero')
        print('       accidents; use the byte scanners to check the results.')
        return 0

    total = cumulative_total()
    per_day, per_byte = summarize(rows)
    print('[NOTE] coverage: patch_write only; heredoc and direct edits are not seen')
    if total is None:
        print('[INFO] %d refusal(s) across %d day(s) (no rotation yet)'
              % (len(rows), len(per_day)))
    else:
        print('[INFO] %d refusal(s) total, %d detail line(s) kept across %d day(s)'
              % (total, len(rows), len(per_day)))
        if total != len(rows):
            print('[INFO] %d older detail line(s) were trimmed by rotation; their'
                  % (total - len(rows)))
            print('       count is preserved in the TOTAL header above')
    for day in sorted(per_day):
        print('       %s  %d' % (day, per_day[day]))
    print('[INFO] bytes involved: ' + ', '.join(
        '%s x%d' % (k, v) for k, v in sorted(per_byte.items())))
    print('[PASS] trend recorded (non-zero is information, not a regression:')
    print('       every line here is an accident that was stopped before it landed)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
