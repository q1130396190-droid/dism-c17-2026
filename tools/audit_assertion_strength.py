"""Classify probe assertions by strength.

Why: round 49 found AppxCapUpdService::SaveCapCache had never persisted anything --
every save returned false, for 48 rounds. No gate caught it because no gate had ever
called that function. Round 50 fixed that one. This tool answers the general question
the incident raised: across the existing suite, how many assertions would notice a
function that silently does nothing?

Four strength levels, weakest first:

  existence   -- asserts something is non-null / non-empty / present
                 e.g. `p != nullptr`, `!s.IsEmpty()`, `v.size() > 0`
                 A function that returns a plausible-looking dummy passes.

  count       -- asserts a quantity, but only "more than zero" or "same as before"
                 e.g. `n > 0`, `count == expected`, `x == y` on sizes
                 Catches "returns nothing" but not "returns wrong values".

  value       -- asserts a specific value or set of values
                 e.g. `strcmp(s, L"用户忙碌") == 0`, `dw == 128`, `pairs_compared == 21`
                 Catches wrong content.

  effect      -- asserts an externally observable side effect
                 e.g. `GetFileAttributes(path) != INVALID`, `before == 0 && after == 0`,
                 `leftover .tmp absent`, `saved == 1` after a real write attempt
                 Catches "claims success but changed nothing".

The classification is a heuristic on the expression text. It is deliberately
reported with samples so a human can check the rule rather than trust the number --
a miscounted denominator is worse than no measurement, which round 46 established the
hard way.
"""
import argparse
import os
import pathlib
import re
import sys

# --- signal patterns -------------------------------------------------------

EXISTENCE = [
    re.compile(r'!=\s*nullptr'),
    re.compile(r'==\s*nullptr'),
    re.compile(r'\.IsEmpty\(\)'),
    re.compile(r'!\s*\w+\.empty\(\)'),
    re.compile(r'\bIsVisible\(\)'),
    re.compile(r'\bisupper\('),
    re.compile(r'\bvalid\('),
]

COUNT_ZERO = [
    re.compile(r'\w+\s*>\s*0\b'),
    re.compile(r'\w+\s*==\s*0\b'),
    re.compile(r'size\(\)\s*[=!<>]=?\s*'),
    re.compile(r'\bcount\s*[=!<>]=?'),
]

# Value: comparison against a literal that is not 0/1, or a strcmp/wcscmp.
VALUE = [
    re.compile(r'\b(wcscmp|_wcsicmp|strcmp|_stricmp)\s*\('),
    re.compile(r'==\s*(?!0\b|1\b|true\b|false\b|nullptr)\w'),
    re.compile(r'==\s*L?"[^"]'),
    re.compile(r'==\s*-?\d{2,}'),
    re.compile(r'<=\s*\d{2,}'),
]

# Effect: asks the OS whether something actually happened.
EFFECT = [
    re.compile(r'GetFileAttributes'),
    re.compile(r'GetFileSize'),
    re.compile(r'RegQueryValue'),
    re.compile(r'\bbefore\b.*\bafter\b'),
    re.compile(r'\bafter\b.*\bbefore\b'),
    re.compile(r'dwWritten'),
    re.compile(r'\bExists\('),
    re.compile(r'INVALID_FILE_ATTRIBUTES'),
    re.compile(r'INVALID_HANDLE_VALUE'),
    re.compile(r'tmp_left|leftover|_left\b'),
]

# A Chk call spans one statement or a few lines. Capture from `Chk(` to the matching
# `));` by paren counting -- simpler and more reliable than a regex over `);`.
CHK_START = re.compile(r'\bChk\s*\(')


def read_text(p):
    for enc in ('utf-8-sig', 'utf-8', 'gbk'):
        try:
            return p.read_text(encoding=enc)
        except UnicodeDecodeError:
            continue
    return p.read_text(encoding='utf-8', errors='replace')


def strip_comments(txt):
    txt = re.sub(r'/\*.*?\*/', ' ', txt, flags=re.S)
    return '\n'.join(re.sub(r'//.*$', '', ln) for ln in txt.splitlines())


def extract_calls(body):
    """Yield (line_no, full_call_text, first_argument_text)."""
    out = []
    for m in CHK_START.finditer(body):
        start = m.start()
        line_no = body.count('\n', 0, start) + 1
        depth = 0
        i = body.find('(', start)
        j = i
        while j < len(body):
            ch = body[j]
            if ch == '(':
                depth += 1
            elif ch == ')':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        call = body[start:j + 1]
        # Split all top-level arguments. Chk(name, condition, detail[, ...]) -- the
        # CONDITION is argument 2, not argument 1. The first version classified
        # argument 1, which is the assertion's human-readable name string, so 62% of
        # calls came back "unknown" purely because they were descriptions.
        args_text = call[call.find('(') + 1:call.rfind(')')]
        parts = []
        depth = 0
        cur = ''
        in_str = False
        for ch in args_text:
            if ch == '"' and depth == 0:
                in_str = not in_str
            if not in_str:
                if ch == '(':
                    depth += 1
                elif ch == ')':
                    depth -= 1
                elif ch == ',' and depth == 0:
                    parts.append(cur)
                    cur = ''
                    continue
            cur += ch
        parts.append(cur)
        cond = parts[1] if len(parts) >= 2 else (parts[0] if parts else '')
        out.append((line_no, call, cond))
    return out


EFFECT_SUBSTRINGS = ('Contains(sBody', 'Contains(sAfter', 'Contains(sBefore',
                    'sAfter.size()', 'sBefore.size()', 'nLines', 'nUnbalanced',
                    'bExisted', 'bAbsent', 'bBeside')


def classify(expr):
    # Plain substring check for the effect shapes that are easier to state than to
    # regex (reading a file back and asserting on its content). Kept separate from
    # the regex table so a typo here cannot silently disable the table.
    for s in EFFECT_SUBSTRINGS:
        if s in expr:
            return 'effect', 'readback content'
    return _classify_regex(expr)


def _classify_regex(expr):
    """Return one of existence / count / value / effect, plus the matched reason."""
    if any(p.search(expr) for p in EFFECT):
        return 'effect', 'side-effect readback'
    if any(p.search(expr) for p in VALUE):
        return 'value', 'literal comparison'
    if any(p.search(expr) for p in COUNT_ZERO):
        return 'count', 'quantity comparison'
    if any(p.search(expr) for p in EXISTENCE):
        return 'existence', 'presence check'
    # Compound conditions: classify by the strongest clause, since the assertion
    # only passes if every clause passes.
    if '&&' in expr or '||' in expr:
        parts = re.split(r'&&|' + chr(124) * 2, expr)
        rank = {'effect': 3, 'value': 2, 'count': 1, 'existence': 0, 'unknown': -1}
        best = None
        for part in parts:
            lvl, why = classify(part)
            if best is None or rank[lvl] > rank[best[0]]:
                best = (lvl, why)
        if best and best[0] != 'unknown':
            return best
    # Fallback: an expression we do not recognise. Report as unknown, NOT as the
    # strongest level -- silently promoting unknowns would inflate the score.
    return 'unknown', 'unrecognised'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', default='tools/probes')
    ap.add_argument('--show', choices=['existence', 'count', 'value', 'effect', 'unknown'],
                    help='print every call classified as this level')
    ap.add_argument('--inject', action='store_true',
                    help='self-test: classify four synthetic samples and require the expected labels')
    args = ap.parse_args()

    if args.inject:
        samples = [
            ("Chk(\"a\", p != nullptr, \"\");", 'existence'),
            ("Chk(\"b\", n > 0, \"\");", 'count'),
            ("Chk(\"c\", wcscmp(s, L\"x\") == 0, \"\");", 'value'),
            ("Chk(\"d\", GetFileAttributes(p) != INVALID_FILE_ATTRIBUTES, \"\");", 'effect'),
        ]
        bad = 0
        for text, want in samples:
            got, _ = classify(text)
            ok = (got == want)
            print('[%s] %-10s -> %-10s want %s' % ('PASS' if ok else 'FAIL', text[:34], got, want))
            if not ok:
                bad += 1
        if bad:
            print('[FAIL] classifier mislabels %d of %d samples' % (bad, len(samples)))
            return 1
        print('[PASS] classifier labels all %d synthetic samples correctly' % len(samples))
        return 0

    root = pathlib.Path(args.dir)
    if not root.is_dir():
        print('[FAIL] probe directory not found: %s' % root)
        return 2
    files = sorted(root.glob('*.cpp'))
    if not files:
        print('[FAIL] zero probe sources found -- a scan that reads nothing cannot pass')
        return 2

    tally = {'existence': 0, 'count': 0, 'value': 0, 'effect': 0, 'unknown': 0}
    per_file = []
    details = []
    for f in files:
        body = strip_comments(read_text(f))
        calls = extract_calls(body)
        if not calls:
            continue
        local = {k: 0 for k in tally}
        for line_no, call, first in calls:
            level, reason = classify(call)
            tally[level] += 1
            local[level] += 1
            details.append((f.name, line_no, level, reason, first.strip()[:90]))
        per_file.append((f.name, len(calls), local))

    # Bare identifiers carry no information about strength -- the interesting text is
    # LIMITATION, stated rather than papered over: when the condition is a bare
    # boolean flag whose value is decided by an `if (!ok) flag = false;` somewhere
    # above, this tool cannot tell what was compared. Resolving the flag's
    # assignments only finds `= true` / `= false`, which is no information.
    #
    # Round 51 tried assignment resolution and it recovered 0 of 244 unknowns --
    # that is a real property of the codebase's style, not a bug to chase. The
    # honest report is therefore: "N assertions are written as a flag, so their
    # strength cannot be judged from the call site" -- and those are listed so a
    # human can spot-check the ones that matter.
    unresolved = sum(1 for d in details if d[2] == 'unknown')
    flag_shaped = [d for d in details if d[2] == 'unknown'
                   and re.match(r'^[A-Za-z_]\w*(\s*(&&|\|\|)\s*[A-Za-z_]\w*)*$', d[4].strip())]
    print('[INFO] unknown=%d of which flag_shaped=%d (decided by an if-else above, '
          'not judgeable at the call site)' % (unresolved, len(flag_shaped)))

    total = sum(tally.values())
    print('[INFO] probes=%d with_assertions=%d Chk_calls=%d' %
          (len(files), len(per_file), total))
    if total == 0:
        print('[FAIL] parsed zero assertions -- the classifier or the source shape changed')
        return 2

    print('')
    print('%-12s %6s %7s' % ('level', 'count', 'share'))
    print('-' * 30)
    for k in ('effect', 'value', 'count', 'existence', 'unknown'):
        print('%-12s %6d %6.1f%%' % (k, tally[k], 100.0 * tally[k] / total))
    print('-' * 30)
    weak = tally['existence'] + tally['unknown']
    print('[INFO] weak_or_unknown=%d (%.1f%%)' % (weak, 100.0 * weak / total))

    if args.show:
        print('')
        print('=== %s ===' % args.show)
        for name, line, level, reason, expr in details:
            if level == args.show:
                print('  %-30s L%-5d %s' % (name, line, expr))

    # The driver judges a probe by whether it emitted an executable assertion, so a
    # report that only prints [INFO] lines is counted as 'no assertion' and fails.
    # That is the correct rule -- a tool that measures something must also state
    # whether the measurement itself is sound -- so emit one explicit verdict.
    print('[PASS] classification complete: %d assertions parsed, %d effect-level, '
          '%d flag-shaped unresolved' % (total, tally['effect'], len(flag_shaped)))

    # Zero-input guard: a suite that silently parses nothing must not report success.
    if total < 100:
        print('[FAIL] only %d assertions parsed; expected a few hundred. '
              'Refusing to report a distribution built on a truncated scan.' % total)
        return 2

    # Strength ratchet. Until now the only failure conditions were "parsed nothing" and
    # "parsed under 100", so a suite drifting from value-level towards existence-level
    # assertions stayed green (measured this round: 68.9% weak/unknown while [PASS] was
    # printed). Two independent guards:
    #   * strong = effect + value + count must not fall below the baseline
    #   * effect-level, the strongest class, must not fall below its own baseline
    # A baseline is a plain number in tools/assertion_strength_baseline.txt; raising it
    # after a real improvement is an explicit edit, lowering it is what this blocks.
    baseline_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                 'assertion_strength_baseline.txt')
    try:
        with open(baseline_path, 'r', encoding='utf-8') as fh:
            baseline_text = fh.read()
    except OSError:
        # Fail closed: no baseline means no ability to detect a decline, and silently
        # reporting success in that state is exactly the failure mode being fixed.
        print('[FAIL] missing baseline %s -- cannot judge whether strength regressed'
              % baseline_path)
        return 2

    base = {}
    for ln in baseline_text.splitlines():
        ln = ln.strip()
        if '=' in ln and not ln.startswith('#'):
            k, v = ln.split('=', 1)
            try:
                base[k.strip()] = int(v.strip())
            except ValueError:
                pass
    missing = [k for k in ('strong', 'effect') if k not in base]
    if missing:
        print('[FAIL] baseline lacks key(s): %s' % ', '.join(missing))
        return 2

    strong = tally['effect'] + tally['value'] + tally['count']
    regressed = []
    if strong < base['strong']:
        regressed.append('strong-level assertions %d < baseline %d' % (strong, base['strong']))
    if tally['effect'] < base['effect']:
        regressed.append('effect-level assertions %d < baseline %d'
                         % (tally['effect'], base['effect']))
    if regressed:
        print('[FAIL] assertion strength regressed:')
        for r in regressed:
            print('       ' + r)
        print('       a probe lost a strong assertion; restore it or lower %s deliberately'
              % baseline_path)
        return 1

    if strong > base['strong'] or tally['effect'] > base['effect']:
        print('[INFO] strength improved (strong=%d/%d effect=%d/%d) -- raise %s'
              % (strong, base['strong'], tally['effect'], base['effect'], baseline_path))

    print('[PASS] assertion strength not regressed: strong=%d (baseline %d), '
          'effect=%d (baseline %d)' % (strong, base['strong'],
                                       tally['effect'], base['effect']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
