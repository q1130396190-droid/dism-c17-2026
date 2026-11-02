"""Keep the thread-contract document in sync with the assertions in the code.

Why: docs/DuiLib线程契约.md said "assertions are only at 3 entry points" while the
code had 25 call sites across 12 files. A document that understates its own coverage
is worse than no document -- a reader concludes the contract is barely enforced and
either duplicates work or trusts it less than it deserves.

A count in prose drifts as soon as anyone adds a call. This check recomputes it and
fails when the document disagrees, so the only maintenance is re-running with --write
after intentionally changing coverage.
"""
import argparse
import pathlib
import re
import sys

DOC = pathlib.Path('docs/DuiLib线程契约.md')
ROOT = pathlib.Path('DuiLib')


def actual():
    per_file = {}
    for f in ROOT.rglob('*.cpp'):
        try:
            body = f.read_text(encoding='utf-8', errors='replace')
        except OSError:
            continue
        # Count BOTH forms. R-07 upgraded the manager-aware call sites to
        # DUI_ASSERT_UI_THREAD_FOR(pm); counting only the legacy spelling made 30
        # live assertions vanish and the gate then reported "listed in doc but has
        # no call site" for eight files that had simply been modernised. The naive
        # substring 'DUI_ASSERT_UI_THREAD(' would also match the legacy name, so the
        # two are counted explicitly: FOR first, then legacy.
        n_for = len(re.findall(r'DUI_ASSERT_UI_THREAD_FOR\s*\(', body))
        n_legacy = len(re.findall(r'DUI_ASSERT_UI_THREAD\s*\(\s*\)', body))
        n = n_for + n_legacy
        if n:
            per_file[f.name] = n
    return per_file


def documented():
    """Read the counts the document claims, from its 6.2 table."""
    if not DOC.is_file():
        return None, None
    text = DOC.read_text(encoding='utf-8', errors='replace')
    m = re.search(r'当前实际是 \*\*(\d+) 处调用、覆盖 (\d+) 个文件\*\*', text)
    if not m:
        return None, None
    total, files = int(m.group(1)), int(m.group(2))
    table = {}
    for name, cnt in re.findall(r'`(\w+\.cpp)`\s*\|\s*(\d+)', text):
        table[name] = int(cnt)
    return (total, files), table


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--write', action='store_true',
                    help='rewrite the counts in the document from the current code')
    args = ap.parse_args()

    per_file = actual()
    if not per_file:
        print('[FAIL] found zero DUI_ASSERT_UI_THREAD() call sites -- '
              'either the macro was renamed or the scan reads nothing')
        return 2
    total, files = sum(per_file.values()), len(per_file)

    if args.write:
        text = DOC.read_text(encoding='utf-8', errors='replace')
        text = re.sub(r'当前实际是 \*\*\d+ 处调用、覆盖 \d+ 个文件\*\*',
                      '当前实际是 **%d 处调用、覆盖 %d 个文件**' % (total, files), text)
        # Rebuild the table rows: pairs of (file, count) two per line.
        items = sorted(per_file.items(), key=lambda kv: (-kv[1], kv[0]))
        rows = []
        for i in range(0, len(items), 2):
            left = items[i]
            right = items[i + 1] if i + 1 < len(items) else None
            l = '| `%s` | %d |' % left
            r = ('| `%s` | %d |' % right) if right else '| | |'
            rows.append('   %s %s' % (l, r))
        body = '\n'.join(rows)
        # Replace the header count and the table SEPARATELY.
        #
        # Doing both in one regex shipped a broken document: the table group used a
        # non-greedy `(   \|.*?)` that consumed only the first row, so the whole
        # pattern never matched the real file and the substitution silently did
        # nothing -- the table stayed stale while the command still printed PASS.
        # Two independent substitutions cannot fail that way, and the result is read
        # back below before reporting success.
        text = re.sub(r'当前实际是 \*\*\d+ 处调用、覆盖 \d+ 个文件\*\*',
                      '当前实际是 **%d 处调用、覆盖 %d 个文件**' % (total, files), text)
        table_re = re.compile(
            r'(当前实际是 \*\*\d+ 处调用、覆盖 \d+ 个文件\*\*：\n\n)(?:   \|.*\n)+', re.S)
        if not table_re.search(text):
            print('[FAIL] cannot locate the coverage table -- refusing to write a '
                  'half-updated document')
            return 1
        text = table_re.sub(lambda mm: mm.group(1) + body + '\n', text)
        # Read back what will actually be on disk, and refuse to claim success if the
        # numbers did not stick. "Replacement ran" is not the same as "document says it".
        back = re.search(r'当前实际是 \*\*(\d+) 处调用、覆盖 (\d+) 个文件\*\*', text)
        got = (int(back.group(1)), int(back.group(2))) if back else (None, None)
        if got != (total, files):
            print('[FAIL] rewrite did not stick: doc reads %r, expected (%d, %d)'
                  % (got, total, files))
            return 1
        DOC.write_text(text, encoding='utf-8')
        print('[PASS] document updated: %d call sites in %d files' % (total, files))
        return 0

    claimed, table = documented()
    if claimed is None:
        print('[FAIL] the document no longer states a coverage count -- cannot verify sync')
        return 1

    problems = []
    if claimed[0] != total:
        problems.append('total: doc says %d, code has %d' % (claimed[0], total))
    if claimed[1] != files:
        problems.append('file count: doc says %d, code has %d' % (claimed[1], files))
    for name, cnt in sorted(per_file.items()):
        if table.get(name) != cnt:
            problems.append('%s: doc says %s, code has %d' % (name, table.get(name), cnt))
    for name in table:
        if name not in per_file:
            problems.append('%s: listed in doc but has no call site' % name)

    print('[INFO] measured: %d call sites in %d files' % (total, files))
    if problems:
        print('[FAIL] thread-contract document is out of sync:')
        for p in problems:
            print('       ' + p)
        print('       re-run with --write after reviewing the change')
        return 1
    print('[PASS] document matches the code')
    return 0


if __name__ == '__main__':
    sys.exit(main())
