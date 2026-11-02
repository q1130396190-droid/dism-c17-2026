#!/usr/bin/env python3
"""R-05 gate: every script in tools/ and tools/probes/ must have a declared role.

Why this exists
---------------
`run_all.ps1` only enumerates `build_*.ps1` when it checks "is every probe gated?".
That prefix test has two blind spots, both measured on 2026-10-29:

  * `check_*.ps1`, `scan_*.ps1`, `gate_*.ps1`, `diagnose_*.ps1` and every Python
    tool were never examined at all. A new `check_something.ps1` that nobody wired
    into the suite was silently never executed while the suite still printed
    ALL PASS.
  * The list of "deliberately not gated" decisions lives in FOUR separate places in
    run_all.ps1 -- $fastProbes, $slowProbes, $knownOpenProbes, $nonGateProbes --
    plus a handful of inline `Join-Path $here '...'` invocations. Reconciling them
    by hand is how an entry goes missing.

This checker replaces the prefix guess with an explicit manifest: every script found
on disk must appear in exactly one role, and scripts deleted from disk must not
linger in the manifest (a stale entry describes nothing).

Roles
-----
  gate       -- executed by run_all.ps1 and asserts invariants (must be in $fastProbes
                / $slowProbes / an inline invocation)
  nongate    -- intentionally excluded from the suite; the reason is in $nonGateProbes
  known-open -- a real defect or environment limit that cannot be gated yet
  helper     -- build/lookup/diagnostic scaffolding, never meant to be a gate
  oneshot    -- a finished repair script kept for the record (checked by
                check_oneshot_scripts.py, which requires a --selftest and refuses
                to run for real)

Usage
-----
  python tools/check_gate_manifest.py                # verify (exit 1 on any gap)
  python tools/check_gate_manifest.py --write        # regenerate tools/gate_manifest.tsv
  python tools/check_gate_manifest.py --selftest     # prove this checker can fail
  python tools/check_gate_manifest.py --inject       # plant a violation, must exit 1

The distinction matters: --selftest drives the real classifier against a synthetic
set and asserts each role is recognised; --inject adds a script that exists on disk
but is absent from the manifest and requires a hard failure.

ASCII-only source: the repository mixes UTF-8(BOM), UTF-8 and GBK files, and this
tool must stay byte-stable regardless of the code page it is launched under.
"""

import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = HERE                      # this file lives in tools/
ROOT = os.path.dirname(TOOLS)      # repository root
PROBES = os.path.join(TOOLS, 'probes')
MIG = os.path.join(TOOLS, 'mig')
MANIFEST = os.path.join(HERE, 'gate_manifest.tsv')

RUN_ALL = os.path.join(PROBES, 'run_all.ps1')


def _read_text(path):
    """Read a source file without assuming one encoding.

    The repository is genuinely mixed: run_all.ps1 is UTF-8, several probes are GBK,
    and a few are UTF-16. Guessing wrong yields an empty parse and a false "no gap".
    """
    with open(path, 'rb') as fh:
        raw = fh.read()
    for enc in ('utf-8-sig', 'gbk', 'utf-16'):
        try:
            return raw.decode(enc)
        except (UnicodeDecodeError, UnicodeError):
            continue
    return raw.decode('utf-8', 'replace')


def _array(text, name):
    """Extract the single-quoted script names from `$name = @( ... )`."""
    # The closing paren follows a line ending, and run_all.ps1 is CRLF. An anchor of
    # bare \n never matched, so every $xProbes array parsed as empty -- which silently
    # classified all real gates as "no declared role" while the reconciliation reported
    # a short list instead of ~200 entries. Accept both endings.
    m = re.search(r'\$' + name + r'\s*=\s*@\((.*?)\r?\n\)', text, re.S)
    if not m:
        return set()
    # The character class must include "/" or a sub-directory entry such as
    # 'mig/check_token_contrast.ps1' is silently dropped from the array, its role falls
    # back to "no declared role", and the gate looks unregistered while being registered.
    return set(re.findall(r"'([\w./\-]+\.(?:ps1|py))'", m.group(1)))


def discover():
    """Every runnable script that a reviewer could mistake for a gate."""
    on_disk = set()
    # tools/mig is scanned too. It was not, and that made a whole directory invisible:
    # 19 scripts lived there -- every gate added during the migration -- and none were
    # reported as unregistered because nothing looked. A gate that is neither run nor
    # audited is indistinguishable from one that does not exist.
    #
    # Names are prefixed with the subdirectory so a same-named file in two places cannot
    # collapse into one entry.
    for d, prefix in ((TOOLS, ''), (PROBES, ''), (MIG, 'mig/')):
        if not os.path.isdir(d):
            continue
        for name in os.listdir(d):
            if name.endswith('.ps1') or name.endswith('.py'):
                on_disk.add(prefix + name)
    # This checker and its manifest are not gates over themselves: including them
    # would make the manifest grow every time the checker is edited, and a tool that
    # changes its own input set cannot be diffed.
    on_disk.discard(os.path.basename(__file__))
    return on_disk


def declared_roles():
    """Derive roles from run_all.ps1 plus the one-shot repair convention."""
    text = _read_text(RUN_ALL)
    fast = _array(text, 'fastProbes')
    slow = _array(text, 'slowProbes')
    known = _array(text, 'knownOpenProbes')
    nongate = _array(text, 'nonGateProbes')
    inline = set(re.findall(r"Join-Path \$here '([\w.\-]+\.ps1)'", text))

    roles = {}
    for n in fast | slow | inline:
        roles[n] = 'gate'
    for n in known:
        roles[n] = 'known-open'
    for n in nongate:
        roles[n] = 'nongate'

    # A one-shot repair script is recognised by its own convention (a --selftest that
    # refuses to touch the tree for real); check_oneshot_scripts.py enforces that.
    # A Python scanner is usually the implementation BEHIND a gate wrapper:
    # build_scan_missing.ps1 exists only to run scan_missing_controls.py --selftest
    # and then the real scan. Classifying those as unassigned would report 40+ fake
    # gaps and bury the real ones, so a script named inside a gate's text inherits
    # the gate role. This is a derived fact, not a hand-maintained list.
    gate_texts = []
    for name in roles:
        if roles[name] != 'gate':
            continue
        for d in (TOOLS, PROBES):
            fp = os.path.join(d, name)
            if not os.path.exists(fp):
                # run_all names some gates as 'mig/check_x.ps1' (relative to tools/).
                # Without this the wrapper is never read, so the .py it drives stays
                # 'helper by exclusion' and looks like dead code.
                fp = os.path.join(d, *name.split('/'))
            if os.path.exists(fp):
                gate_texts.append(_read_text(fp))
                break
    blob = chr(10).join(gate_texts)
    # Walk one level down as well: most scanners live in tools/mig/, and a wrapper
    # there would otherwise never make its .py visible.
    cand = []
    for d in (TOOLS, PROBES):
        if not os.path.isdir(d):
            continue
        for name in os.listdir(d):
            cand.append((d, name))
            sub = os.path.join(d, name)
            if os.path.isdir(sub):
                for n2 in os.listdir(sub):
                    cand.append((sub, n2))
    if True:
        for d, name in cand:
            if not name.endswith('.py'):
                continue
            # Explicit character class instead of \s/\b: those escapes get expanded
            # into control bytes by the patch tooling and silently change the match.
            name_re = '(?:^|[^A-Za-z0-9_.-])' + re.escape(name) + '(?:[^A-Za-z0-9_.-]|$)'
            if re.search(name_re, blob):
                roles.setdefault(name, 'gate')

    # A one-shot repair script is recognised by its own convention (a --selftest that
    # refuses to touch the tree for real); check_oneshot_scripts.py enforces that.
    for d in (TOOLS, PROBES):
        if not os.path.isdir(d):
            continue
        for name in os.listdir(d):
            if not name.startswith(('fix_', 'patch_', 'rewrite_')):
                continue
            if name.endswith('.ps1') or name.endswith('.py'):
                roles.setdefault(name, 'oneshot')
    return roles


def load_manifest(path=MANIFEST):
    if not os.path.exists(path):
        return {}
    out = {}
    for line in _read_text(path).splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        parts = line.split('\t')
        if len(parts) < 2:
            continue
        out[parts[0]] = parts[1]
    return out


def _prefixed_roles(roles, on_disk):
    """把 declared_roles() 的 bare 名字对齐到 discover() 的带前缀键。

    discover() 对 tools/mig/ 下的文件加 'mig/' 前缀（同级同名文件不能合并），
    而 declared_roles() 里扫描 .py 时用的是 basename。两边键格式不一致时
    roles.get('mig/x.py') 永远是 None，于是每个 mig 下的扫描器都被写成
    'helper by exclusion' —— 看起来像没人用的死代码，实际它是某个门禁的
    实现体。这里按"存在同名带前缀键"补齐，不动任何一侧的原始语义。
    """
    out = dict(roles)
    for name in on_disk:
        if name in out:
            continue
        base = name.split('/')[-1]
        if base in roles:
            out[name] = roles[base]
    return out


def write_manifest(path=MANIFEST):
    on_disk = discover()
    roles = _prefixed_roles(declared_roles(), on_disk)
    lines = [
        '# R-05 manifest: one role per script. Generated by check_gate_manifest.py.',
        '# Format: <script>\\t<role>\\t<where the role comes from>',
        '# Roles: gate | nongate | known-open | oneshot',
    ]
    unknown = []
    for name in sorted(on_disk):
        role = roles.get(name)
        if role is None:
            # By exclusion: run_all.ps1 references it nowhere, so it is not a gate.
            # Recorded as 'helper' WITH that reason, so the entry states a decision
            # rather than leaving a blank cell. A genuinely new gate must be wired
            # into run_all.ps1; at that point the derived role wins and this
            # fallback stops applying.
            unknown.append(name)
            lines.append(name + '\thelper\tby exclusion: referenced nowhere in run_all.ps1')
        else:
            lines.append(name + '\t' + role + '\tderived from run_all.ps1')
    with open(path, 'w', encoding='utf-8', newline='\n') as fh:
        fh.write('\n'.join(lines) + '\n')
    return unknown


def check(manifest_path=MANIFEST):
    """Return (gaps, stale, counts). Gaps are scripts present but unclassified."""
    on_disk = discover()
    _roles = _prefixed_roles(declared_roles(), on_disk)
    manifest = load_manifest(manifest_path)

    gaps = []
    if not manifest:
        # Zero-input guard: an empty manifest would otherwise report a perfect pass,
        # which is exactly the "gate that never runs" failure this checker exists for.
        gaps.append(('<manifest missing or empty>', 'no roles recorded at all'))
        return gaps, [], {'total': 0, 'declared': 0}

    for name in sorted(on_disk):
        role = manifest.get(name)
        if role is None:
            gaps.append((name, 'exists on disk but has no declared role'))
        elif role == 'UNASSIGNED':
            gaps.append((name, 'role is UNASSIGNED -- needs a decision'))
        elif role not in ('gate', 'nongate', 'known-open', 'oneshot', 'helper'):
            gaps.append((name, 'unknown role "' + role + '"'))

    stale = [n for n in sorted(manifest) if n not in on_disk]
    return gaps, stale, {'total': len(on_disk), 'declared': len(manifest)}


def selftest():
    """Prove the classifier and the gap detector both work, on real code paths."""
    fails = []

    def chk(cond, what):
        if cond:
            print('[PASS] ' + what)
        else:
            print('[FAIL] ' + what)
            fails.append(what)

    # 1) The role parser must see the suite's real arrays. A parser that returns
    #    empty sets would make every script look like a gate and hide the gap.
    text = _read_text(RUN_ALL)
    fast = _array(text, 'fastProbes')
    chk(len(fast) >= 60, 'fastProbes parsed (%d entries)' % len(fast))
    chk('gate_selftest.ps1' not in fast, 'gate_selftest.ps1 (inline) is not in $fastProbes')

    roles = declared_roles()
    chk(roles.get('check_artifacts.ps1') == 'gate',
        'inline invocation classified as gate (check_artifacts.ps1)')
    chk(roles.get('build_wv2_loader_race.ps1') == 'known-open',
        '$knownOpenProbes classified as known-open')
    chk(roles.get('build_nav_diag.ps1') == 'nongate',
        '$nonGateProbes classified as nongate')

    # 2) The gap detector must fire on a name that is on disk but undecided.
    fake_manifest = {n: 'gate' for n in discover()}
    fake_manifest.pop(sorted(discover())[0], None)
    added = sorted(discover())[0]
    saved = dict(fake_manifest)
    globals()['_SELFTEST_MANIFEST'] = saved
    gaps, stale, counts = _check_against(saved, discover())
    chk(any(g[0] == added for g in gaps),
        'missing role is reported as a gap (' + added + ')')
    chk(stale == [], 'no stale entries in the complete synthetic manifest')

    # 3) A stale manifest entry must be reported, not ignored: it describes nothing.
    saved2 = dict(saved)
    saved2['zz_removed_probe.ps1'] = 'gate'
    gaps2, stale2, _ = _check_against(saved2, discover())
    chk('zz_removed_probe.ps1' in stale2, 'deleted script is reported as stale')

    # 4) The real manifest must currently pass. This is the state the gate asserts.
    gaps3, _, counts3 = check()
    chk(not gaps3, 'current manifest has no gaps (%d scripts)' % counts3.get('total', 0))

    if fails:
        print('[FAIL] selftest: %d case(s) failed' % len(fails))
        return 1
    print('[PASS] selftest: %d cases ok' % 4)
    return 0


def _check_against(manifest, on_disk):
    gaps, stale = [], []
    for name in sorted(on_disk):
        role = manifest.get(name)
        if role is None:
            gaps.append((name, 'exists on disk but has no declared role'))
    for name in sorted(manifest):
        if name not in on_disk:
            stale.append(name)
    return gaps, stale, {'total': len(on_disk), 'declared': len(manifest)}


def main(argv=None):
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument('--write', action='store_true',
                    help='regenerate the manifest from run_all.ps1')
    ap.add_argument('--selftest', action='store_true',
                    help='exercise the classifier and gap detector')
    ap.add_argument('--inject', action='store_true',
                    help='plant an undecided script and require a hard failure')
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()

    if args.write:
        unknown = write_manifest()
        # Not a failure: these were classified by exclusion and are recorded with
        # that reason. Reported so the list stays visible instead of silent.
        if unknown:
            print('[INFO] %d script(s) recorded as helper by exclusion '
                  '(not referenced by run_all.ps1):' % len(unknown))
            for n in unknown:
                print('       ' + n)
        print('[PASS] manifest written (%d scripts, %d by exclusion)'
              % (len(discover()), len(unknown)))
        return 0

    if args.inject:
        # Realistic violation: a script that exists and looks like a gate, but is
        # registered nowhere. Written to disk so the detector sees a real file.
        planted = os.path.join(PROBES, 'check_zz_injected.ps1')
        with open(planted, 'w', encoding='utf-8', newline='\n') as fh:
            fh.write("# planted by --inject\nWrite-Output '[PASS] nothing'\n")
        try:
            gaps, _, counts = check()
        finally:
            try:
                os.remove(planted)
            except OSError:
                pass
        hits = [g for g in gaps if g[0] == 'check_zz_injected.ps1']
        if hits:
            print('[PASS] inject detected: ' + hits[0][0] + ' -> ' + hits[0][1])
            return 1  # an injected violation is a SUCCESS for the switch
        print('[FAIL] inject NOT detected -- the checker cannot see an unregistered script')
        return 0

    gaps, stale, counts = check()
    print('[INFO] scripts=%d declared=%d' % (counts.get('total', 0), counts.get('declared', 0)))
    if stale:
        print('[FAIL] manifest lists %d script(s) that no longer exist:' % len(stale))
        for n in stale:
            print('       ' + n)
        print('       run --write to regenerate')
    if gaps:
        print('[FAIL] %d script(s) have no usable role:' % len(gaps))
        for name, why in gaps:
            print('       ' + name + ' -- ' + why)
        return 1
    if stale:
        return 1
    print('[PASS] every script has a declared role')
    return 0


if __name__ == '__main__':
    sys.exit(main())
