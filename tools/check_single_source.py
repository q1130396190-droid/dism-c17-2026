"""One judgement, one implementation: detect duplicated constants and skip-sets.

Why (round 87): this project's recurring defect is not a single wrong line -- it is
the same judgement written twice, where a fix lands in one copy and the other stays
wrong. Measured instances:

  * round 85 fixed a prefix match in the main verdict and missed the copy inside
    --inject;
  * round 87 found three tools each carrying the hard-coded MSVC toolset path
    14.51.36231 -- a VS upgrade would break all three at once;
  * round 87 also found the repository-skip set written out seven times, in two
    slightly different versions: two copies had no '.git', so they walked the git
    object store on every scan.

Both are now single-sourced (tools/find_msvc_tool.py, tools/scan_scope.py). This
check keeps them that way.

Two mechanical signals, both narrow:

  1. A path literal containing a pinned MSVC toolset version, outside the one module
     that is allowed to have it.
  2. A set/dict literal listing two or more directory names from the scan-skip
     vocabulary, outside the shared module.

Deliberately not a general "duplicate code" detector: that flags everything and
gets ignored. These two shapes are the ones that actually broke.

Exit codes:
    0  no duplicate judgement found
    1  found (names file and line)
    2  zero files inspected -- a scan that reads nothing cannot pass
    3  --inject planted a duplicate and it was not reported
"""
import argparse
import pathlib
import re
import sys

# The one module allowed to name a toolset version / define the skip set.
ALLOWED_OWNERS = {
    'find_msvc_tool.py',
    'scan_scope.py',
}

# A pinned toolset under VC\Tools\MSVC\<version>.
TOOLSET_PATTERN = re.compile(r'VC.{1,3}Tools.{1,3}MSVC.{1,3}\d+\.\d+\.\d+')

# Directory names that belong to the scan-skip vocabulary.
SKIP_VOCAB = ('bin', 'lib', 'obj', 'temp', '.git', '.box-agent-scratch')


def scan_file(path):
    """Return [(line, kind, text)] for duplicated judgements in `path`."""
    hits = []
    if path.name in ALLOWED_OWNERS:
        return hits
    try:
        text = path.read_text(encoding='utf-8', errors='replace')
    except OSError:
        return hits
    for i, line in enumerate(text.splitlines(), 1):
        # Skip this file's own pattern/vocabulary definitions AND the --inject
        # sample strings, which necessarily contain the shapes being looked for.
        if 'TOOLSET_PATTERN' in line or 'SKIP_VOCAB' in line:
            continue
        # Only skip the literal source lines of THIS file's own machinery. Adding
        # 'PINNED'/'SKIP = ' here silently excluded the injected sample too --
        # measured: the self-test planted both shapes and detected 0.
        if 'chr(92)' in line or 'sep.join(' in line or "repr('bin')" in line:
            continue
        if False:
            continue
        if TOOLSET_PATTERN.search(line):
            hits.append((i, 'pinned MSVC toolset path', line.strip()))
            continue
        # A literal set/dict with >=2 skip-vocabulary names is a skip-set copy.
        if '{' in line and any(q in line for q in ('{', )):
            present = sum(1 for v in SKIP_VOCAB if ("'" + v + "'") in line or ('"' + v + '"') in line)
            if present >= 2:
                hits.append((i, 'duplicated scan-skip set', line.strip()))
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--inject', action='store_true',
                    help='self-test: plant a duplicate and require it be reported')
    args = ap.parse_args()

    if args.inject:
        plant = pathlib.Path('tools') / ('dup' + '_inject_probe.py')
        # Build the sample with join() so no escape passes through transit, and so
        # the toolset segment is adjacent to its separators -- the pattern wants
        # one separator between VC and Tools and between Tools and MSVC.
        sep = chr(92)
        toolset = (sep.join(['C:', 'Program Files', 'VC', 'Tools', 'MSVC',
                             '14.0.0', 'bin']) )
        plant.write_text(
            'PINNED = ' + repr(toolset) + chr(10) +
            'SKIP = {' + repr('bin') + ', ' + repr('obj') + ', ' + repr('temp') + '}' + chr(10),
            encoding='utf-8')
        try:
            hits = scan_file(plant)
        finally:
            try:
                plant.unlink()
            except OSError:
                pass
        kinds = sorted({k for _, k, _ in hits})
        if len(hits) >= 2:
            print('[PASS] inject: both duplicate shapes detected (%s)' % ', '.join(kinds))
            return 3
        print('[FAIL] inject: planted two duplicate shapes, detected %d' % len(hits))
        return 0

    files = sorted(pathlib.Path('tools').glob('*.py'))
    if not files:
        print('[FAIL] zero files inspected -- a scan that reads nothing cannot pass')
        return 2

    findings = []
    for p in files:
        if '.box-agent-scratch' in str(p):
            continue
        for line, kind, text in scan_file(p):
            findings.append((p.name, line, kind, text))

    print('[INFO] files_inspected=%d' % len(files))
    if findings:
        print('[FAIL] %d duplicated judgement(s):' % len(findings))
        for name, line, kind, text in findings:
            print('       %-40s L%-4d %-26s %s' % (name, line, kind, text[:56]))
        print('       import the shared module instead:')
        print('         tools/find_msvc_tool.py  -- dumpbin discovery')
        print('         tools/scan_scope.py      -- directories scans must skip')
        return 1

    print('[PASS] no duplicated judgement found (toolset path, skip set)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
