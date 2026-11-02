"""Build a functional-area x gate coverage matrix.

Why: 62 checks pass every round, but "62 green" says nothing about what those
checks look at. This maps the Demo's subsystems to the probes that actually
assert on them, so uncovered areas are visible instead of assumed.

Method: for each subsystem file, search every probe source (.ps1/.cpp/.py) for the
file's base name or its key identifiers. A subsystem with zero hits has no
assertion anywhere -- that is the definition of "uncovered" used here. It is a
coarse signal (a probe could exercise it indirectly), so hits are reported too.
"""
import pathlib
import re

root = pathlib.Path('.')
demo = root / 'Demos' / 'Win8Metro'
probes = root / 'tools' / 'probes'

# Subsystems worth tracking: services (business logic) and pages (UI).
#
# Collapse implementation slices onto the class they implement. A page or service
# may be split across several .cpp files that all include the same header
# (Page_Cleanup.cpp / Page_Cleanup_Diag.cpp / Page_Cleanup_Report.cpp are three
# slices of one CPageCleanup). Counting each slice as its own "subsystem" made the
# matrix report a page as UNCOVERED even when the gate exercised its parent -- the
# measured figure was wrong, not the coverage. Grouping by header also matches how
# a probe is written: it names the class, not the slice.
import collections
by_header = collections.defaultdict(list)
for p in sorted(list(demo.glob('*Service.cpp')) + list(demo.glob('Page_*.cpp'))):
    # Ownership = the header that declares the class, taken from the file's own
    # #include list. A sibling-named header is not enough: Page_Cleanup_Diag.cpp
    # has no Page_Cleanup_Diag.h, it includes Page_Cleanup.h.
    owner = None
    try:
        for line in p.read_text(encoding='utf-8', errors='replace').splitlines():
            m = re.match(r'\s*#\s*include\s+"([^"]+)"', line)
            if not m:
                continue
            cand = demo / m.group(1)
            if cand.exists() and p.stem.split('_')[0] in cand.stem:
                owner = cand.name
                break
    except OSError:
        pass
    if owner is None:
        hdr = p.with_suffix('.h')
        owner = hdr.name if hdr.exists() else p.stem
    by_header[owner].append(p)
# One representative per header; extra slices are listed in the row detail.
targets = sorted((v[0] for v in by_header.values()), key=lambda q: q.stem)
slice_note = {v[0].stem: [q.name for q in v[1:]] for v in by_header.values() if len(v) > 1}

# Collect all probe text once.
probe_files = list(probes.glob('*.ps1')) + list(probes.glob('*.cpp')) + list(probes.glob('*.py'))
def strip_comments(name, txt):
    """Remove comments before matching.

    A subsystem name inside a comment is documentation, not an assertion. This
    mattered: run_all.ps1's registration note lists "AssocService /
    ControlService / DependencyGraphService" by name, so counting raw text let a
    prose mention mark three subsystems as covered before any probe existed.
    Drivers are also dropped: run_all.ps1 orchestrates, it does not assert.
    """
    if name.endswith('.ps1') or name.endswith('.py'):
        return chr(10).join(re.sub(r'#.*$', '', ln) for ln in txt.splitlines())
    txt = re.sub(r'/\*.*?\*/', '', txt, flags=re.S)
    return chr(10).join(re.sub(r'//.*$', '', ln) for ln in txt.splitlines())

# A probe file that exists but never runs is not coverage. The distinction came
# up with ExternalToolService: build_exttool.ps1 is present in tools/probes and
# names the service, so a plain file scan called it covered -- but the script sits
# in $nonGateProbes and the suite never executes it. Read the two lists out of the
# driver so "covered" means "a probe that actually runs asserts on it".
def _driver_list(text, name):
    out = set()
    inside = False
    for line in text.splitlines():
        s = line.strip()
        if s.startswith('$' + name) and '= @(' in s:
            inside = True
            continue
        if inside:
            if s == ')':
                break
            m = re.match(r"^'([^']+)'", s)
            if m:
                out.add(m.group(1))
    return out

_gate_text = ''
try:
    _gate_text = (root / 'tools' / 'probes' / 'run_all.ps1').read_text(encoding='utf-8', errors='replace')
except OSError:
    pass
GATED = _driver_list(_gate_text, 'fastProbes')
EXCLUDED = _driver_list(_gate_text, 'nonGateProbes')
if not GATED:
    print('[FAIL] could not read $fastProbes from run_all.ps1 -- coverage would be unmeasurable')
    raise SystemExit(2)

DRIVERS = {'run_all.ps1', 'gate_inventory.ps1', 'gate_zero_input.ps1'}
probe_text = {}
for f in probe_files:
    if f.name in DRIVERS:
        continue
    try:
        probe_text[f.name] = strip_comments(f.name, f.read_text(encoding='utf-8', errors='replace'))
    except OSError:
        pass

rows = []
for t in targets:
    stem = t.stem                       # e.g. "AssocService" or "Page_Assoc"
    # Identifiers likely to appear in a probe if it exercises this subsystem.
    keys = {stem}
    # Page_X -> also search X alone (page_startup, btn_startup_*).
    if stem.startswith('Page_'):
        tail = stem[5:]
        keys.add(tail)
        keys.add(tail.lower())
    # Slices share the representative's keys: they are the same class, so a probe
    # naming CPageCleanup does cover Page_Cleanup_Report.cpp.
    hits = []
    dead = []
    for name, txt in probe_text.items():
        if not any(k and k in txt for k in keys):
            continue
        base = name
        # .cpp probes are compiled by a build_*.ps1; treat the C++ file as gated
        # when some gated/excluded script names it.
        if base.endswith(('.cpp', '.py')):
            owners = [n for n, tx in probe_text.items()
                      if n.endswith('.ps1') and base in tx]
            if owners:
                if any(o in GATED for o in owners):
                    hits.append(name)
                else:
                    dead.append(name)
                continue
        if base in GATED:
            hits.append(name)
        else:
            dead.append(name)
    rows.append((stem, len(hits), sorted(hits)[:4], sorted(dead)[:2]))

rows.sort(key=lambda r: (r[1], r[0]))
print('%-26s %5s  %s' % ('subsystem', 'gated', 'sample gated probes'))
print('-' * 96)
for stem, n, sample, dead in rows:
    flag = 'UNCOVERED' if n == 0 else ''
    note = ('  (only in non-gate: ' + ', '.join(dead)[:34] + ')') if n == 0 and dead else ''
    print('%-26s %5d  %-40s %s%s' % (stem, n, ', '.join(sample)[:40], flag, note))

cov = sum(1 for _, n, _, _ in rows if n > 0)
print('-' * 96)
print('total=%d covered=%d uncovered=%d' % (len(rows), cov, len(rows) - cov))
if slice_note:
    print('grouped slices (counted once, under the class they implement):')
    for rep, extra in sorted(slice_note.items()):
        print('  %-26s <- %s' % (rep, ', '.join(extra)))

# --- self-test -------------------------------------------------------------
# Two silent-inflation paths were found the hard way: a name inside a comment
# (run_all.ps1's registration note listed three services in prose) and a probe
# file that exists but never runs ($nonGateProbes). Both made "covered" read
# higher than reality.
#
# The first version only exercised the .ps1 branch, so disabling the .cpp branch
# still printed PASS -- an assertion that could not fail on the path it claimed
# to protect. It also only checked that `$nonGateProbes` parsed, not that its
# members are actually excluded from the count. Both are behavioural now.
SEP = chr(10)
STRIP_CASES = [
    ('a.cpp', '// AssocService' + SEP + 'int a;', 'int a;'),
    ('b.cpp', '/* AssocService */' + SEP + 'int a;', 'int a;'),
    ('c.ps1', '# AssocService' + SEP + 'Write-Host 1', 'Write-Host 1'),
]
for _nm, _in, _want in STRIP_CASES:
    _got = strip_comments(_nm, _in).strip()
    if _got != _want:
        print('[FAIL] comment stripping broken for %s' % _nm)
        raise SystemExit(3)

# Behavioural check: ExternalToolService is named by build_exttool.ps1, which is
# a $nonGateProbes member. It must therefore read as uncovered -- if it ever
# shows a gated hit, the exclusion stopped being applied.
_row = next((r for r in rows if r[0] == "ExternalToolService"), None)
if _row is None:
    print('[FAIL] ExternalToolService vanished from the target list -- self-test is blind')
    raise SystemExit(3)
if _row[1] != 0:
    print('[FAIL] a non-gate probe was counted as coverage (ExternalToolService gated=%d)' % _row[1])
    raise SystemExit(3)
if not _row[3]:
    print('[FAIL] ExternalToolService is not attributed to any non-gate probe -- exclusion untested')
    raise SystemExit(3)

print('[INFO] self-test: comment-strip 3/3, non-gate excluded, gated=%d excluded=%d'
      % (len(GATED), len(EXCLUDED)))


# ---------------------------------------------------------------- verdict
#
# Ratchet: coverage may not regress. The baseline is a single number -- how many of the
# target subsystems had at least one gated probe when this was last raised. Keeping it a
# number (not a per-subsystem list) means a subsystem can be swapped for another without
# tripping the gate, while a net loss cannot pass unnoticed.
import os as _os

if len(rows) == 0:
    print('[FAIL] zero subsystems scanned -- refusing to report coverage over nothing')
    raise SystemExit(2)

_baseline_path = _os.path.join(_os.path.dirname(_os.path.abspath(__file__)),
                               'coverage_baseline.txt')
try:
    with open(_baseline_path, 'r', encoding='utf-8') as _fh:
        _baseline_text = _fh.read()
except OSError:
    # Fail closed: without a baseline there is nothing to compare against, and treating
    # that as success is how this kind of gate goes quietly blind.
    print('[FAIL] missing baseline %s -- cannot decide whether coverage regressed'
          % _baseline_path)
    raise SystemExit(2)

_cov = sum(1 for r in rows if r[1] > 0)
_base = None
for _ln in _baseline_text.splitlines():
    _ln = _ln.strip()
    if _ln.startswith('covered='):
        _base = int(_ln.split('=', 1)[1])
if _base is None:
    print('[FAIL] baseline has no covered= line')
    raise SystemExit(2)

if _cov < _base:
    print('[FAIL] coverage regressed: covered=%d, baseline=%d' % (_cov, _base))
    print('       a subsystem lost its last gated probe; restore it or, if the removal')
    print('       was deliberate, lower %s in the same change and say why' % _baseline_path)
    raise SystemExit(1)

if _cov > _base:
    print('[INFO] coverage improved: covered=%d, baseline=%d' % (_cov, _base))
    print('       raise %s to lock the gain in' % _baseline_path)

print('[PASS] coverage not regressed: covered=%d baseline=%d total=%d'
      % (_cov, _base, len(rows)))
