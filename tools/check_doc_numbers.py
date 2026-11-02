"""Verify the numbers in documentation that are meant to describe the CURRENT state.

Why: docs/DuiLib线程契约.md claimed "3 entry points" when the code had 25, and
docs/源码编码约定.md claimed "736 files" when it was 738. Both numbers were true when
written and became false silently. A reader cannot tell a stale number from a current
one, so they either distrust the whole document or act on wrong data.

Scope, deliberately narrow: only numbers that (a) describe the current repository and
(b) can be recomputed from a single source of truth. Historical statements such as
"round 52 had 80 checks" are excluded -- those are records of what was true then, and
they must NOT track the present.

Checks performed:
  1. docs/源码编码约定.md  "共 N 个文件"  <-> files counted by encoding_report
  2. docs/DuiLib线程契约.md "N 处调用、覆盖 M 个文件" <-> DUI_ASSERT_UI_THREAD sites

Run with --write to refresh from the current state after reviewing a change.
"""
import argparse
import pathlib
import re
import sys
import sys as _sys, pathlib as _pl
_sys.path.insert(0, str(_pl.Path(__file__).resolve().parent))
# [R87] shared skip set; this file held a 6th copy
from scan_scope import SKIP_DIRS

ENCODING_DOC = pathlib.Path('docs/源码编码约定.md')
THREAD_DOC = pathlib.Path('docs/DuiLib线程契约.md')
ESCAPE_DOC = pathlib.Path('docs/源码改写约定-不要写转义.md')


def count_sources():
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        'enc', str(pathlib.Path('tools/encoding_report.py')))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    total = 0
    for d in mod.DIRS:
        root = pathlib.Path(d)
        if not root.is_dir():
            continue
        for f in root.rglob('*'):
            if not f.is_file() or f.suffix.lower() not in mod.SUFFIXES:
                continue
            if {x.lower() for x in f.parts} & SKIP_DIRS:
                continue
            total += 1
    return total


def per_encoding():
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        'enc', str(pathlib.Path('tools/encoding_report.py')))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    counts = {}
    for d in mod.DIRS:
        root = pathlib.Path(d)
        if not root.is_dir():
            continue
        for f in root.rglob('*'):
            if not f.is_file() or f.suffix.lower() not in mod.SUFFIXES:
                continue
            if {x.lower() for x in f.parts} & SKIP_DIRS:
                continue
            enc = mod.classify(f.read_bytes())
            counts[enc] = counts.get(enc, 0) + 1
    return counts


def count_assert_sites():
    per_file = {}
    for f in pathlib.Path('DuiLib').rglob('*.cpp'):
        try:
            body = f.read_text(encoding='utf-8', errors='replace')
        except OSError:
            continue
        # Count BOTH spellings. R-07 upgraded 30 call sites to
        # DUI_ASSERT_UI_THREAD_FOR(pm); counting only the legacy form made the
        # measurement collapse to 1/1 and this checker accused the DOCUMENT of being
        # stale when the checker was the stale half. Same defect existed in
        # check_thread_doc_sync.py -- two copies of one rule, both had to be fixed.
        n_for = len(re.findall(r'DUI_ASSERT_UI_THREAD_FOR\s*\(', body))
        n_legacy = len(re.findall(r'DUI_ASSERT_UI_THREAD\s*\(\s*\)', body))
        n = n_for + n_legacy
        if n:
            per_file[f.name] = n
    return sum(per_file.values()), len(per_file), per_file


def escape_census():
    """Recompute the escape census quoted in docs/源码改写约定-不要写转义.md."""
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        'mesc', str(pathlib.Path('tools/measure_escape_usage.py')))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    seen, paths = set(), []
    for r in (pathlib.Path('tools'), pathlib.Path('.')):
        if not r.is_dir():
            continue
        for f in sorted(r.glob('*.py')):
            if f.resolve() in seen or '.box-agent-scratch' in str(f):
                continue
            seen.add(f.resolve())
            paths.append(f)
    scanned, hits, files = mod.measure(paths)
    any_escape = set()
    for k in mod.PURPOSES:
        any_escape |= files[k]
    return scanned, len(any_escape), hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--write', action='store_true', help='refresh the numbers from code')
    # --inject perturbs one recomputed value so the comparison must fail. The
    # documents on disk are untouched: this proves the *detection* path works,
    # which is the half that silently rots when nobody can make a gate go red.
    ap.add_argument('--inject', action='store_true',
                    help='self-test: force one mismatch and expect a failure')
    args = ap.parse_args()

    problems = []
    done = []

    # --- 1. encoding doc ---
    if ENCODING_DOC.is_file():
        total = count_sources()
        if args.inject:
            total += 1
            print('[INJECT] pretending the source census is %d files' % total)
        if total == 0:
            problems.append('encoding: counted 0 source files -- scan read nothing')
        else:
            text = ENCODING_DOC.read_text(encoding='utf-8', errors='replace')
            m = re.search(r'共\s*(\d+)\s*个文件', text)
            if not m:
                problems.append('encoding: document no longer states a file count')
            elif args.write:
                text = re.sub(r'共\s*\d+\s*个文件', '共 %d 个文件' % total, text, count=1)
                ENCODING_DOC.write_text(text, encoding='utf-8')
                done.append('encoding: -> %d files' % total)
            elif int(m.group(1)) != total:
                problems.append('encoding: doc says %s files, code has %d'
                                % (m.group(1), total))
            else:
                done.append('encoding: %d files ok' % total)

            # The per-encoding rows drift too -- they were 454/231/49/1 when written
            # and are already stale. Check each label's count, since that is more
            # specific than the total and catches a shift between buckets.
            buckets = per_encoding()
            for label, key in (('UTF-8（无 BOM）', 'utf-8'),
                               ('UTF-8（有 BOM）', 'utf-8-bom'),
                               ('GBK', 'gbk'),
                               ('UTF-16LE', 'utf-16-le-bom')):
                mm = re.search(r'\|\s*\**' + re.escape(label) + r'\**\s*\|\s*\**(\d+)\**\s*\|',
                               text)
                if not mm:
                    problems.append('encoding: row for %s not found' % label)
                    continue
                got = buckets.get(key, 0)
                if args.write:
                    text = re.sub(r'(\|\s*\**' + re.escape(label) +
                                  r'\**\s*\|\s*\**)\d+(\**\s*\|)',
                                  lambda z: z.group(1) + str(got) + z.group(2), text, count=1)
                elif int(mm.group(1)) != got:
                    problems.append('encoding: row %s says %s, code has %d'
                                    % (label, mm.group(1), got))
            if args.write:
                ENCODING_DOC.write_text(text, encoding='utf-8')

    # --- 2. thread doc ---
    if THREAD_DOC.is_file():
        total, files, per_file = count_assert_sites()
        text = THREAD_DOC.read_text(encoding='utf-8', errors='replace')
        m = re.search(r'\*\*(\d+) 处调用、覆盖 (\d+) 个文件\*\*', text)
        if not m:
            problems.append('thread: document no longer states a coverage count')
        elif args.write:
            text = re.sub(r'\*\*\d+ 处调用、覆盖 \d+ 个文件\*\*',
                          '**%d 处调用、覆盖 %d 个文件**' % (total, files), text, count=1)
            THREAD_DOC.write_text(text, encoding='utf-8')
            done.append('thread: -> %d sites / %d files' % (total, files))
        elif (int(m.group(1)), int(m.group(2))) != (total, files):
            problems.append('thread: doc says %s sites / %s files, code has %d / %d'
                            % (m.group(1), m.group(2), total, files))
        else:
            done.append('thread: %d sites / %d files ok' % (total, files))

    # --- 3. escape-convention doc ---
    if ESCAPE_DOC.is_file():
        scanned, with_escape, hits = escape_census()
        text = ESCAPE_DOC.read_text(encoding='utf-8', errors='replace')
        pct = int(round(100.0 * with_escape / scanned)) if scanned else 0
        # The doc states it as:  - 92%（87/95）的脚本含转义文本；
        # Matched by splitting on the percent sign rather than by regex, because
        # every regex I tried here went through too many escape layers.
        found = None
        for line in text.split(chr(10)):
            if '%' not in line or chr(65288) not in line:
                continue
            left, rest = line.split('%', 1)
            num = ''.join(c for c in left if c.isdigit())
            inner = rest.split(chr(65288), 1)[1].split(chr(65289), 1)[0]
            parts = inner.split('/')
            if len(parts) == 2 and parts[0].strip().isdigit() and parts[1].strip().isdigit():
                found = (int(num), int(parts[0]), int(parts[1]))
                break
        if found is None:
            problems.append('escape: document no longer states the percentage row')
        elif args.write:
            # Round 82: this branch was missing, so --write silently left the escape
            # numbers stale while reporting success for the other two documents.
            # "The fixer runs" and "the fixer fixes every section" are different
            # claims; only the first was true here.
            newrow = '%d%%（%d/%d）' % (pct, with_escape, scanned)
            text = text.replace('%d%%（%d/%d）' % found, newrow, 1)
            ESCAPE_DOC.write_text(text, encoding='utf-8')
            done.append('escape: -> %s' % newrow)
        elif (found[1], found[2]) != (with_escape, scanned):
            problems.append('escape: doc says %d/%d scripts carry escapes, code has %d/%d'
                            % (found[1], found[2], with_escape, scanned))
        elif found[0] != pct:
            problems.append('escape: doc says %d%%, code computes %d%%' % (found[0], pct))
        else:
            done.append('escape: %d%% (%d/%d) carry escapes' % (pct, with_escape, scanned))

    for d in done:
        print('[INFO] ' + d)
    if problems:
        print('[FAIL] documentation describes a different repository than this one:')
        for p in problems:
            print('       ' + p)
        print('       run with --write after confirming the change is intended')
        return 1
    print('[PASS] all checked numbers match the current repository')
    return 0


if __name__ == '__main__':
    sys.exit(main())
