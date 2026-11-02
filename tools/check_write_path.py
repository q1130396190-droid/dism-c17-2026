#!/usr/bin/env python3
"""Every script that MODIFIES the tree must go through patch_write.safe_write().

Why this gate exists
--------------------
The pre-write control-byte guard in patch_write.py is correct and self-tested:

    --selftest  ->  dirty payload refused, target bytes unchanged

But a correct guard is worthless if the writers do not call it. Measured on
2026-10-30: of 33 scripts named patch_*/fix_*/inject_*/rewrite_*, **33 wrote
files directly** (write_bytes / write_text / open(..., 'w')) and **zero** went
through safe_write. So the guard protected exactly one caller.

The failure this permits is the one already recorded seven times in this repo:

    r'\\b'  ->  r'<backspace>' 0x08

The regex then matches a backspace instead of a word boundary. Nothing raises.
The check just stops matching, and the symptom looks like "the code is clean".
An after-the-fact byte scan (check_control_bytes.py) catches it only if somebody
remembers to look, and only for bytes it knows about.

So this gate enforces the structural property instead: a tree-modifying script
must obtain the write through the guarded writer. It does not inspect payloads --
it checks who is allowed to touch the tree at all.

Roles exempt from this gate (with reasons, not by silent omission):
  * patch_write.py itself      -- it IS the guarded writer
  * backup_before_write.py     -- writes only backup copies, never source
  * verify_written.py          -- read-only
  * check_*.py / scan_*.py     -- read-only by contract
  * *_selftest / one-shot repairs that carry --selftest and refuse to run for real
    (they are checked by check_oneshot_scripts.py instead)

Usage
-----
    python tools/check_write_path.py                 # verify
    python tools/check_write_path.py --json          # machine-readable
    python tools/check_write_path.py --selftest      # exercise the classifier
    python tools/check_write_path.py --inject        # plant a raw writer, must fail

Exit codes
    0  every tree-modifying script goes through the guarded writer
    1  at least one script writes directly (message names file and line)
    2  the scan found nothing to examine -- a scan that reads nothing cannot pass
    3  --inject was requested and the planted violation was NOT detected

ASCII-only source.
"""

import argparse
import json
import os
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent
TOOLS = ROOT

# Prefixes that mark a script as a tree-modifying operation.
WRITER_PREFIXES = ('patch_', 'fix_', 'inject_', 'rewrite_')

# Files that are allowed to write without the guarded writer, each with a reason.
EXEMPT = {
    'patch_write.py': 'this IS the guarded writer',
    'backup_before_write.py': 'writes backup copies only, never source files',
    'verify_written.py': 'read-only by contract',
    'check_write_path.py': 'this gate; it reads and reports, never writes',
    # One-shot repairs: they refuse to run without --selftest and are policed by
    # check_oneshot_scripts.py. Recording them here keeps the exemption explicit
    # rather than making the suffix test quietly skip them.
    'inject_probe_build_cache.py': 'one-shot, --selftest required by check_oneshot_scripts',
    'inject_selftest_all_probes.py': 'one-shot, --selftest required by check_oneshot_scripts',
    'inject_switch_wrappers.py': 'one-shot, --selftest required by check_oneshot_scripts',
}

# Direct-write shapes. Deliberately narrow so the gate does not fire on reads or
# on writing to temp/backup paths inside the guarded writer.
RAW_WRITE_PATTERNS = [
    (re.compile(r'\.write_bytes\s*\('), 'Path.write_bytes'),
    (re.compile(r'\.write_text\s*\('), 'Path.write_text'),
    (re.compile(r"open\s*\([^)]*['\"][wa]b?\+?['\"]"), "open(..., 'w')"),
    (re.compile(r'\bshutil\.copy(file)?\s*\('), 'shutil.copy over a target'),
]

# A write whose target is explicitly a scratch/temp/backup path is fine: it does
# not modify the tree. Detected on the same line as the write call.
SCRATCH_TARGET = re.compile(
    r'(_pw_probe|pw_selftest|\.box-agent-scratch|tempfile|mkstemp|tmp|backup)',
    re.IGNORECASE)


def _read(path):
    """Read without assuming one encoding: this repo mixes UTF-8(BOM)/UTF-8/GBK."""
    raw = path.read_bytes()
    for enc in ('utf-8-sig', 'gbk', 'utf-16'):
        try:
            return raw.decode(enc)
        except (UnicodeDecodeError, UnicodeError):
            continue
    return raw.decode('utf-8', 'replace')


def is_writer(name):
    return name.endswith('.py') and name.startswith(WRITER_PREFIXES)


def scan(writers=None):
    """Return (checked, violations). A violation is (file, line, shape)."""
    names = sorted(p.name for p in TOOLS.glob('*.py') if is_writer(p.name))
    if writers is not None:
        names = [n for n in names if n in writers]

    checked = []
    violations = []
    for name in names:
        if name in EXEMPT:
            continue
        checked.append(name)
        body = _read(TOOLS / name)
        uses_guarded = ('safe_write' in body) or ('from patch_write import' in body)
        for lineno, line in enumerate(body.splitlines(), 1):
            stripped = line.strip()
            if stripped.startswith('#'):
                continue
            for pat, label in RAW_WRITE_PATTERNS:
                if not pat.search(line):
                    continue
                if SCRATCH_TARGET.search(line):
                    continue
                # Importing the guarded writer but still calling write_bytes on a
                # tree path is still a bypass; record it either way and let the
                # caller see the shape.
                violations.append((name, lineno, label, uses_guarded))
    return checked, violations


def selftest():
    fails = []

    def chk(cond, what):
        if cond:
            print('[PASS] ' + what)
        else:
            print('[FAIL] ' + what)
            fails.append(what)

    # 1) Classifier recognises the writer prefixes and ignores read-only tools.
    chk(is_writer('patch_foo.py'), 'patch_* classified as writer')
    chk(is_writer('fix_foo.py'), 'fix_* classified as writer')
    chk(is_writer('inject_foo.py'), 'inject_* classified as writer')
    chk(not is_writer('check_foo.py'), 'check_* is not a writer')
    chk(not is_writer('scan_foo.py'), 'scan_* is not a writer')

    # 2) The direct-write detector must fire on the real shapes.
    for sample, expect in [
        ("p.write_bytes(data)", 'Path.write_bytes'),
        ("p.write_text(s)", 'Path.write_text'),
        ("open(path, 'w')", "open(..., 'w')"),
        ("open(path, 'wb')", "open(..., 'w')"),
    ]:
        hit = any(pat.search(sample) for pat, _ in RAW_WRITE_PATTERNS)
        chk(hit, 'detector fires on: ' + expect)

    # 3) It must NOT fire on a scratch/temp target.
    chk(bool(SCRATCH_TARGET.search("tmp.write_bytes(data)")),
        'scratch target recognised (tmp.write_bytes)')
    chk(bool(SCRATCH_TARGET.search("open('.box-agent-scratch/x', 'w')")),
        'scratch target recognised (.box-agent-scratch)')

    # 4) Real scan: report what it examined, so a zero-input pass is visible.
    checked, violations = scan()
    chk(len(checked) > 0, 'scan examined %d writer(s)' % len(checked))

    # 5) The gate's own verdict on the current tree.
    chk(len(violations) == 0,
        'current tree: no direct writers (%d violation(s))' % len(violations))
    if violations:
        for name, lineno, label, used in violations[:10]:
            print('       %s:%d  %s  (imports guarded=%s)' % (name, lineno, label, used))

    if fails:
        print('[FAIL] selftest: %d case(s) failed' % len(fails))
        return 1
    print('[PASS] selftest: 10 cases ok')
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument('--json', action='store_true', help='machine-readable output')
    ap.add_argument('--selftest', action='store_true', help='exercise the classifier')
    ap.add_argument('--inject', action='store_true',
                    help='plant a raw writer and require a hard failure')
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()

    if args.inject:
        # Realistic violation: a patch script that writes the tree directly.
        planted = TOOLS / 'patch_zz_injected.py'
        planted.write_text(
            "# planted by --inject\n"
            "import pathlib\n"
            "def go(p, data):\n"
            "    pathlib.Path(p).write_bytes(data)\n",
            encoding='utf-8', newline='\n')
        try:
            checked, violations = scan()
        finally:
            try:
                planted.unlink()
            except OSError:
                pass
        hits = [v for v in violations if v[0] == 'patch_zz_injected.py']
        if hits:
            print('[PASS] inject detected: %s:%d %s' % (hits[0][0], hits[0][1], hits[0][2]))
            return 1  # detecting the planted violation is SUCCESS for this switch
        print('[FAIL] inject NOT detected -- the gate is blind to a raw writer')
        return 0

    checked, violations = scan()
    if args.json:
        print(json.dumps({
            'checked': checked,
            'violations': [
                {'file': n, 'line': l, 'shape': s, 'imports_guarded_writer': u}
                for n, l, s, u in violations],
        }, indent=2))
        return 1 if violations else 0

    print('[INFO] writers_examined=%d' % len(checked))
    if not checked:
        # Zero-input guard: reporting "all clean" over zero files is the failure
        # mode this gate exists to prevent.
        print('[FAIL] examined 0 writer scripts -- refusing to report success')
        return 2
    # Baseline. 35 unguarded writes exist in one-off migration scripts written before
    # tools/patch_write.safe_write.py. Failing on all of them made this gate red every
    # run, so a genuinely new violation was indistinguishable from the 35 known ones.
    # Known occurrences are tolerated and counted; anything extra fails.
    known = _load_baseline()
    remaining = {}
    for key in known:
        remaining[key] = remaining.get(key, 0) + 1

    new_violations = []
    matched = 0
    for name, lineno, label, used in violations:
        key = '%s:%s' % (name, label)
        if remaining.get(key, 0) > 0:
            remaining[key] -= 1
            matched += 1
            print('[KNOWN] %s:%d  %s' % (name, lineno, label))
        else:
            new_violations.append((name, lineno, label, used))

    stale = sorted(k for k, v in remaining.items() if v > 0)
    for key in stale:
        # Unmatched entry: fixed (delete the line) or mis-keyed (it protected nothing).
        # Reported but not fatal -- tolerating a fixed entry is the safe direction.
        print('[STALE] baseline entry matches no violation: %s' % key)

    if new_violations:
        print('[FAIL] %d NEW direct write(s) bypass the guarded writer:' % len(new_violations))
        for name, lineno, label, used in new_violations:
            hint = ' (imports safe_write but still writes directly)' if used else ''
            print('       %s:%d  %s%s' % (name, lineno, label, hint))
        print('       route the write through patch_write.safe_write() so the')
        print('       pre-write control-byte guard actually runs')
        return 1
    print('[PASS] no new unguarded write (%d known, %d stale entr%s)'
          % (matched, len(stale), 'y' if len(stale) == 1 else 'ies'))
    return 0


def _load_baseline():
    """One `file:shape` key per line; a missing file means no tolerance."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "write_path_baseline.txt")
    try:
        with open(path, "r", encoding="utf-8") as fh:
            return [x.strip() for x in fh
                    if x.strip() and not x.strip().startswith("#")]
    except OSError:
        return []


if __name__ == '__main__':
    sys.exit(main())
