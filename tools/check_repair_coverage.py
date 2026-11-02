"""Verify by BEHAVIOUR that every '--write' repair tool repairs every section it checks.

Why behavioural instead of counting branches (round 83):

    The first attempt counted `problems.append` sites against `if args.write`
    branches. It reported check_thread_doc_sync.py as "reports 4, repairs 1" -- a
    false alarm. That tool's single write branch rewrites the whole document, so
    its four report sites are all covered. One branch can repair many fields; one
    field can also have no branch. Counting cannot tell those apart.

    Behaviour can. For each tool, plant a stale value in ONE section of the
    document it governs, run --write, and see whether that section changed. A
    section that stays stale is a section the tool cannot repair -- which is
    exactly the round-83 defect in check_doc_numbers.py's escape census.

Each case names: the tool, the file to perturb, a regex selecting the section, the
replacement, and a check that the original shape was restored. Documents are
backed up and restored no matter what happens.

Exit codes:
    0  every perturbed section was repaired
    1  a section was NOT repaired (names it)
    2  zero cases executed -- a run that tests nothing cannot pass
    3  --inject requested; the harness must report the planted breakage
"""
import argparse
import pathlib
import re
import shutil
import subprocess
import sys

SCRATCH = pathlib.Path('.box-agent-scratch/repair_coverage')

# (label, tool, document, pattern, replacement, restored_pattern)
CASES = [
    ('doc_numbers/encoding-total',
     'tools/check_doc_numbers.py',
     'docs/源码编码约定.md',
     r'共\s*\d+\s*个文件', '共 1 个文件', r'共\s*(\d+)\s*个文件'),
    ('doc_numbers/encoding-row',
     'tools/check_doc_numbers.py',
     'docs/源码编码约定.md',
     r'(\|\s*\**UTF-8（无 BOM）\**\s*\|\s*\**)\d+', r'\g<1>1', None),
    ('doc_numbers/thread',
     'tools/check_doc_numbers.py',
     'docs/DuiLib线程契约.md',
     r'\*\*\d+ 处调用、覆盖 \d+ 个文件\*\*', '**1 处调用、覆盖 1 个文件**', None),
    ('doc_numbers/escape',
     'tools/check_doc_numbers.py',
     'docs/源码改写约定-不要写转义.md',
     r'\d+%（\d+/\d+）', '1%（1/1）', None),
    ('thread_sync/counts',
     'tools/check_thread_doc_sync.py',
     'docs/DuiLib线程契约.md',
     r'当前实际是 \*\*\d+ 处调用、覆盖 \d+ 个文件\*\*',
     '当前实际是 **1 处调用、覆盖 1 个文件**', None),
]


def run_case(label, tool, doc, pattern, replacement, restored_pattern, inject):
    src = pathlib.Path(doc)
    backup = SCRATCH / (src.name + '.bak')
    shutil.copyfile(src, backup)
    try:
        text = src.read_text(encoding='utf-8')
        m = re.search(pattern, text)
        if not m:
            return ('SKIP', 'pattern not found in ' + doc)
        before = m.group(0)
        perturbed = text[:m.start()] + re.sub(pattern, replacement, before, count=1) + text[m.end():]
        if perturbed == text:
            return ('SKIP', 'perturbation produced no change')
        src.write_text(perturbed, encoding='utf-8')

        if inject and label == 'doc_numbers/escape':
            # Simulate the round-83 defect: make the tool unable to repair this
            # section by removing its write branch for the escape block.
            tpath = pathlib.Path(tool)
            tbackup = SCRATCH / (tpath.name + '.inject.bak')
            shutil.copyfile(tpath, tbackup)
            try:
                src_code = tpath.read_text(encoding='utf-8')
                # Target the ESCAPE section specifically. Replacing the FIRST
                # 'elif args.write' hits the encoding section instead, which made the
                # self-test report 'nothing failed' for the wrong reason (round 83).
                # Line-based, no escape sequences, so this literal survives transit.
                marker = 'escape: document no longer states the percentage row'
                src_lines = src_code.split(chr(10))
                out_lines = []
                armed = False
                hit = False
                for ln in src_lines:
                    if armed and ln.strip() == 'elif args.write:':
                        ln = ln.replace('elif args.write:', 'elif False and args.write:')
                        armed = False
                        hit = True
                    elif marker in ln:
                        armed = True
                    out_lines.append(ln)
                if not hit:
                    return ('SKIP', 'escape repair branch not found for injection')
                src_code = chr(10).join(out_lines)
                tpath.write_text(src_code, encoding='utf-8')
                proc = subprocess.run([sys.executable, tool, '--write'],
                                      capture_output=True, text=True)
            finally:
                shutil.copyfile(tbackup, tpath)
        else:
            proc = subprocess.run([sys.executable, tool, '--write'],
                                  capture_output=True, text=True)

        after = src.read_text(encoding='utf-8')
        stale_present = re.search(re.escape(re.sub(pattern, replacement, before, count=1)), after)
        if stale_present:
            return ('FAIL', 'section still stale after --write (rc=%d)' % proc.returncode)
        if restored_pattern and not re.search(restored_pattern, after):
            return ('FAIL', 'section was altered but not restored to a valid shape')
        return ('PASS', 'repaired (rc=%d)' % proc.returncode)
    finally:
        shutil.copyfile(backup, src)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--inject', action='store_true',
                    help='self-test: break one repair branch and require a FAIL')
    args = ap.parse_args()

    SCRATCH.mkdir(parents=True, exist_ok=True)
    executed = 0
    failures = []
    for case in CASES:
        label = case[0]
        verdict, detail = run_case(*case, inject=args.inject)
        if verdict == 'SKIP':
            print('%-30s SKIP  %s' % (label, detail))
            continue
        executed += 1
        print('%-30s %s  %s' % (label, verdict, detail))
        if verdict == 'FAIL':
            failures.append((label, detail))

    if executed == 0:
        print('[FAIL] zero cases executed -- a run that tests nothing cannot pass')
        return 2

    if args.inject:
        if failures:
            print('[PASS] inject: the unrepaired section was detected (%d failure(s))'
                  % len(failures))
            return 3
        print('[FAIL] inject: removed a repair branch and nothing failed -- the check is blind')
        return 0

    if failures:
        print('[FAIL] %d section(s) are checked but not repaired by their tool:' % len(failures))
        for label, detail in failures:
            print('       %-30s %s' % (label, detail))
        print('       --write still prints success for the sections it does handle,')
        print('       so the stale one is easy to miss on every subsequent run')
        return 1

    print('[PASS] every checked section is repaired by its tool (%d case(s))' % executed)
    return 0


if __name__ == '__main__':
    sys.exit(main())
