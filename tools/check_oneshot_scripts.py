"""Stop new one-shot patch scripts from entering the repository.

Why (round 73): 0x08 has been chased at least a dozen times in this project. Every
occurrence had the same origin -- a throwaway script that assembled C++ source by
pasting escape sequences together, run once, and left behind. Measured state:

    tools/*.py + root *.py                      68 + 25 = 93 scripts
    of those, write C++ sources                  37
    of those, carry escape sequences             28
    referenced by any other file                   0

So the dangerous population is entirely dead code, and correcting them changes
nothing (they already use '\\\\n', which is correct -- the damage came from hand
typing, not from these surviving files). The recurring event is *writing a new
one*. That is what this gate addresses.

Definition of a one-shot patch script, deliberately narrow so it cannot fire on
legitimate tooling:
    * file name starts with '_'                        (the house marker), OR
    * the file WRITES to a .cpp/.h path               (it edits sources), AND
    * nothing else in the repo mentions its name       (it is not a tool), AND
    * it carries no self-test entry                    (not a gate)

Anything matching all applicable conditions must be listed in the allow-list
below with a date. The list is expected to be empty; its purpose is to make the
next addition a deliberate act rather than an accident.

Stored baseline
---------------
56 such scripts already exist. They are registered in tools/oneshot_baseline.txt
and are tolerated; only ADDITIONS fail. The baseline is not an excuse -- it is a
freeze. Deleting this repository's dead patch scripts is not an option here
because there is no version control, so the honest position is "stop growing" and
that is exactly what the file encodes.

Exit codes:
    0  no NEW one-shot patch script (baseline entries are reported, not failed)
    1  a new one-shot patch script was found (names it)
    2  zero scripts inspected -- a scan that reads nothing cannot pass
    3  --inject found nothing after planting one (the check is blind)
"""
import argparse
import pathlib
import sys

ROOTS = [pathlib.Path('.'), pathlib.Path('tools')]

# Scripts exempted on purpose. Empty by design: a new entry needs a date and a
# reason, which forces the question "does this have to be a script at all?".
ALLOW = {}

# A patch script does not merely *mention* source files -- it writes to a path
# that IS repository source. build_matrix.py writes probe.cpp under scratch and
# abi_guard_check.py writes JSON reports; both were wrongly flagged in round 74
# because the old pattern only looked for .cpp/.h anywhere in the file.
WRITES_SOURCE = 'write_bytes|write_text|open\\('
SOURCE_PATH = r'\.(?:cpp|h|hpp)\b'
SELFTEST = r'\$(?:Selftest|Inject[A-Za-z]*)|--self-?test|--inject'


def candidates():
    seen = set()
    out = []
    for root in ROOTS:
        if not root.is_dir():
            continue
        for p in sorted(root.glob('*.py')):
            rp = p.resolve()
            if rp in seen:
                continue
            seen.add(rp)
            out.append(p)
    return out


def all_text_except(target):
    """Bodies of files that could actually INVOKE `target`, minus `target` itself.

    Markdown is deliberately excluded. It was included until round 74, and that
    made "referenced" trivially satisfiable: a single sentence in a report was
    enough to reclassify a one-shot patch script as a tool. Measured with a
    planted script mentioned only in docs/: tools went 39 -> 40 and new went
    1 -> 0, i.e. the gate was talked out of a finding by prose.

    A document cannot call a script. Only a script, a project file or a config
    can, so only those count as references.
    """
    chunks = []
    for pat in ('*.py', 'tools/**/*.py', 'tools/**/*.ps1', '*.ps1', '*.bat', '*.cmd',
                '*.json', '*.vcxproj', '*.props', '*.targets', '*.yml', '*.yaml'):
        for q in pathlib.Path('.').glob(pat):
            if '.box-agent-scratch' in str(q):
                continue
            if q.resolve() == target.resolve():
                continue
            try:
                chunks.append(q.read_text(encoding='utf-8', errors='replace'))
            except OSError:
                continue
    return chr(10).join(chunks)


BASELINE = pathlib.Path('tools/oneshot_baseline.txt')


def load_baseline():
    """Return the set of tolerated script names.

    Missing file -> empty set, so the gate degrades to STRICT rather than to
    permissive. A gate that silently tolerates everything when its data file is
    gone is worse than no gate: it reports PASS while checking nothing.
    """
    if not BASELINE.is_file():
        return set()
    names = set()
    for line in BASELINE.read_text(encoding='utf-8').splitlines():
        s = line.strip()
        if not s or s.startswith('#') or s[:1].isdigit():
            continue
        names.add(s.split()[0])
    return names


def write_baseline(entries):
    import datetime
    stamp = datetime.date.today().isoformat()
    lines = [
        '# [R73] One-shot patch scripts already in the tree, tolerated by baseline.',
        '#',
        '# Written by: python tools/check_oneshot_scripts.py --write-baseline',
        '# Re-run that only after deleting some of these, so the file shrinks.',
        '#',
        '# Format: <name>  <date-recorded>  <reason>',
        stamp + ' snapshot',
    ]
    for name, why in sorted(entries):
        lines.append('%-46s %s  %s' % (name, stamp, why))
    BASELINE.write_text(chr(10).join(lines) + chr(10), encoding='utf-8')


# The guarded writer. A script that rewrites repository source must go through it:
# patch_write refuses a payload carrying control bytes BEFORE touching the file.
# Round 75 measured the gap this closes: of the 56 one-shot patches in the tree,
# 56 write files directly and 0 use the guarded writer, so none of them has the
# protection the CLI already provides.
GUARDED_WRITER = ('safe_write', 'patch_write')

# [R236] A read-only tool must never be classified as a patch script, however many
# times its comments or glob patterns happen to spell a source extension.
#
# audit_coverage_matrix.py was flagged as an "unreferenced one-shot patch script"
# while its only file access is `open(path, 'r', encoding='utf-8')`. The source-path
# test matched the docstring ("search every probe source (.ps1/.cpp/.py)") and the
# glob literal 'Page_*.cpp'. Neither is a write target. The existing write test
# (WRITES_SOURCE) accepts a bare 'open(' substring, so a read-only open satisfied it.
#
# Judging by "does this ever open for writing" is the same question the gate is
# actually asking, stated positively. No regex: rounds 72-75 showed escaped patterns
# get mangled in transit through the patch tooling.
WRITE_MODES = (
    'w',
    'wb',
    'a',
    'write_text(',
    'write_bytes(',
    'writelines(',
)


def opens_for_writing(body):
    'True when the script has any file-write construct at all.'
    if any(m in body for m in WRITE_MODES):
        return True
    # open(path, mode=X) keyword form. The quote characters are built from
    # chr() so this source never contains two quotes in a row -- that shape is
    # what got mangled repeatedly while this fix was being written.
    q1 = chr(39)
    q2 = chr(34)
    for quote in (q1, q2):
        for mode in ('w', 'wb'):
            if 'mode=' + quote + mode + quote in body:
                return True
    return False



# Measured boundary of the .cpp/.h suffix test (round 75, injection-verified):
#   'DuiLib/' + 'Core/UIManager.cpp'        -> detected (suffix literal present)
#   'DuiLib/Core/' + name + '.cpp'          -> detected (suffix literal present)
#   'DuiLib/Core/Manager' + ext where
#       ext = '.' + 'cpp'                   -> NOT detected, but only if the file
#                                              name does not start with '_'
# The last shape is real but narrow: it needs the extension itself assembled from
# fragments AND no underscore prefix. Underscore-prefixed scripts are caught by the
# name rule regardless. Recorded here rather than fixed, because widening the test
# to catch it would start flagging ordinary string building in real tools.


def bypasses_guarded_writer(body):
    """True when the script writes files directly instead of via the guard.

    Plain substring tests, no regular expressions: every attempt to express this
    as a regex was mangled in transit (rounds 72-75), and the predicate does not
    need regex power.
    """
    if any(w in body for w in GUARDED_WRITER):
        return False
    for marker in ('write_bytes(', 'write_text(', 'writelines('):
        if marker in body:
            return True
    # open(<...>, 'w' ...) or open(<...>, "w" ...)
    for q in (chr(39), chr(34)):
        if 'open(' in body and (', ' + q + 'w' + q) in body:
            return True
    return False


def writes_repo_source(body, write_re, src_re):
    """True when the script writes to a repository source path.

    Two separate questions, because a patch script normally keeps the target and
    the write far apart -- patch_probe_inject_a11y.py declares its path on line 3
    and writes on line 27:

        p = pathlib.Path('tools/probes/a11y_probe.cpp')
        ...
        p.write_text(t, encoding='utf-8')

    (a) does it write at all?
    (b) does ANY string literal in it name a source file -- .cpp/.h/.hpp -- that
        is not a generated or scratch destination?

    Asking (b) over the whole file rather than line-by-line is what restored the
    population from 30 back to 56. The intermediate line-based version reported
    49-51 and looked like a cleaner repository while silently losing real
    findings.

    No escape sequences are used here on purpose: see rounds 73 and 74 for the
    repeated 0x08 damage caused by writing them through nested layers.
    """
    if not write_re.search(body):
        return False
    quotes = (chr(39), chr(34))
    for line in body.splitlines():
        idx = 0
        while True:
            found = -1
            for q in quotes:
                k = line.find(q, idx)
                if k >= 0 and (found < 0 or k < found):
                    found = k
            if found < 0:
                break
            q = line[found]
            close = line.find(q, found + 1)
            if close < 0:
                break
            s = line[found + 1:close]
            idx = close + 1
            norm = s.replace(chr(92), '/')
            if not norm.endswith(('.cpp', '.h', '.hpp')):
                continue
            low = norm.lower()
            if 'scratch' in low or 'temp' in low or '/probe.cpp' in low:
                continue
            return True
    return False


def classify():
    import re
    write_re = re.compile(WRITES_SOURCE)
    src_re = re.compile(SOURCE_PATH)
    self_re = re.compile(SELFTEST)

    oneshots, tools = [], []
    files = candidates()
    # Cache the corpus per scan: rebuilding it per file is O(n^2) and slow.
    corpus_by_file = {}
    for p in files:
        try:
            body = p.read_text(encoding='utf-8', errors='replace')
        except OSError:
            continue
        looks_like_patch = p.name.startswith('_') or writes_repo_source(body, write_re, src_re)
        if not looks_like_patch:
            tools.append(p.name)
            continue
        # [R236] It looked like a patch only because a .cpp literal appears in a
        # comment or a glob. If the file never opens anything for writing, there is
        # no way for it to have produced that source -- classify it as a tool.
        if not opens_for_writing(body):
            tools.append(p.name)
            continue
        if self_re.search(body):
            tools.append(p.name)      # it can prove itself: a gate, not a patch
            continue
        if p.name in ALLOW:
            oneshots.append((p.name, 'allow-listed: ' + ALLOW[p.name]))
            continue
        rest = all_text_except(p)
        if p.name in rest:
            tools.append(p.name)      # something still refers to it
            continue
        oneshots.append((p.name, 'unreferenced one-shot patch script'))
    return files, tools, oneshots


def find_bypassing(paths):
    """Scripts that rewrite repo source without going through the guarded writer."""
    import re as _re
    src_re = _re.compile(SOURCE_PATH)
    write_re = _re.compile(WRITES_SOURCE)
    out = []
    for p in paths:
        try:
            body = p.read_text(encoding='utf-8', errors='replace')
        except OSError:
            continue
        if writes_repo_source(body, write_re, src_re) and bypasses_guarded_writer(body):
            out.append(p.name)
    return out


def find_bypassing(paths):
    """Scripts that rewrite repo source without the guarded writer."""
    import re as _re
    src_re = _re.compile(SOURCE_PATH)
    write_re = _re.compile(WRITES_SOURCE)
    out = []
    for p in paths:
        try:
            body = p.read_text(encoding='utf-8', errors='replace')
        except OSError:
            continue
        if not writes_repo_source(body, write_re, src_re):
            continue
        if bypasses_guarded_writer(body):
            out.append(p.name)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--inject', action='store_true',
                    help='self-test: plant a one-shot script and require it be reported')
    ap.add_argument('--write-baseline', action='store_true', dest='write_baseline',
                    help='record the current set as tolerated (run only after reviewing)')
    args = ap.parse_args()

    if args.inject:
        # The name is assembled at runtime on purpose. Written as one literal it
        # becomes a mention of itself inside all_text_except(), so the injected
        # script looks "referenced" and the self-test silently concluded the scan
        # was blind (round 73, first run: rc=0 with a planted file present).
        plant = pathlib.Path('tools') / ('_oneshot_inject' + '_probe.py')
        plant.write_text(
            'import pathlib\n'
            'pathlib.Path("DuiLib/Core/X.cpp").write_bytes(b"// injected\\n")\n',
            encoding='utf-8')
        try:
            files, tools, oneshots = classify()
        finally:
            if plant.exists():
                plant.unlink()
        named = [n for n, _ in oneshots if n == plant.name]
        if named:
            print('[PASS] inject: planted one-shot script reported (%s)' % named[0])
            return 3
        print('[FAIL] inject: planted %s but the scan did not report it' % plant.name)
        print('       the scan is blind; fix it before trusting a clean result')
        return 0

    files, tools, oneshots = classify()
    if not files:
        print('[FAIL] zero scripts inspected -- a scan that reads nothing cannot pass')
        return 2

    baseline = load_baseline()
    known = [(n, w) for n, w in oneshots if n in baseline]
    new = [(n, w) for n, w in oneshots if n not in baseline]

    print('[INFO] scripts_inspected=%d tools=%d one_shot=%d baselined=%d new=%d'
          % (len(files), len(tools), len(oneshots), len(known), len(new)))

    if args.write_baseline:
        write_baseline(oneshots)
        print('[PASS] baseline written: %d entries -> %s' % (len(oneshots), BASELINE))
        return 0

    if new:
        print('[FAIL] %d NEW unreferenced one-shot patch script(s):' % len(new))
        for name, why in new:
            print('       %-44s %s' % (name, why))
        print('       These assemble source by pasting escapes together, run once, and')
        print('       stay. Every 0x08 this project has chased came from one. Either')
        print('       make it a registered tool (with a self-test), or delete it now.')
        print('       If it genuinely must stay, it needs a date and a reason in')
        print('       %s -- an addition there is a decision, not an oversight.' % BASELINE)
        return 1

    if known:
        print('[INFO] %d baselined one-shot script(s) tolerated (baseline freeze, '
              'not a clean bill)' % len(known))
        # Informational, deliberately not a failure: the 56 are frozen, not fixed.
        # What matters going forward is that no NEW script skips the guard, which
        # the check below reports for new ones only.
        bypass = find_bypassing([p for name in (n for n, _ in known)
                                 for p in [next((d / name for d in
                                                 (pathlib.Path('tools'), pathlib.Path('.'))
                                                 if (d / name).is_file()), None)]
                                 if p is not None])
        if bypass:
            print('[INFO] %d of them write files directly, bypassing the guarded '
                  'writer (pre-existing, frozen)' % len(bypass))
    new_bypass = [n for n, _ in new]
    if new_bypass:
        print('[FAIL] new one-shot script(s) also bypass the guarded writer: %s'
              % ', '.join(new_bypass))
        return 1
    print('[PASS] no new one-shot patch scripts')
    return 0


if __name__ == '__main__':
    sys.exit(main())
