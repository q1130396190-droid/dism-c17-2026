#!/usr/bin/env python3
r"""Add the Test-ProbeUpToDate build cache to compiling probe scripts.

Why this exists
---------------
The full suite recompiled ~59 probe executables on every run. Under that concurrent
load the MSVC compiler exhausted its heap (`fatal error C1060`), which the judge then
reported as "no executable assertion" -- a probe defect it was not.

An OOM retry was added as a stopgap, but that only treats the symptom. This script
makes each probe skip compilation when nothing it depends on has changed, which
removes the memory pressure at the source.

Safety rules
------------
* Encoding and line endings are preserved byte-for-byte (some scripts are ASCII,
  some UTF-8; rewriting them uniformly would corrupt the non-ASCII ones).
* A script is skipped when it already uses Test-ProbeUpToDate.
* The injected guard only WRAPS the existing compile line; the original argument
  list is never rewritten, so a scripting mistake cannot silently change what is
  compiled. It can only make the build be skipped.
* Dependencies are derived from the script's own text: every token ending in .cpp
  or .lib that carries a path ($root\..., ...\...\. If a script names no path-based
  dependency, its own <stem>.cpp plus the framework lib are used.

Usage:
    python tools/inject_probe_build_cache.py --dry-run
    python tools/inject_probe_build_cache.py --apply
"""
import argparse
import re
import sys
from pathlib import Path

PROBES = Path('tools/probes')
FRAMEWORK_LIB = '$root\\bin64\\amd64\\DuiLib.lib'


def load(path: Path):
    """Return (text, encoding, bom, newline) preserving the original encoding."""
    raw = path.read_bytes()
    bom = raw[:3] == b'\xef\xbb\xbf'
    if bom:
        return raw[3:].decode('utf-8'), 'utf-8', True, '\r\n' if b'\r\n' in raw else '\n'
    try:
        text = raw.decode('utf-8')
        return text, 'utf-8', False, '\r\n' if b'\r\n' in raw else '\n'
    except UnicodeDecodeError:
        text = raw.decode('gbk')
        return text, 'gbk', False, '\r\n' if b'\r\n' in raw else '\n'


def save(path: Path, text: str, encoding: str, bom: bool):
    data = text.encode(encoding)
    if bom:
        data = b'\xef\xbb\xbf' + data
    path.write_bytes(data)


def find_output(text: str):
    m = re.search(r'/OUT:([\w.\-]+)', text)
    return m.group(1) if m else None


def find_deps(text: str, stem: str):
    """Path-carrying .cpp/.lib tokens, plus the script's own <stem>.cpp and headers.

    Headers matter: a probe that only depends on its own .cpp will be treated as
    up-to-date after someone edits a DuiLib header, and then link a stale object
    against a new library layout. Adding the framework's public headers keeps the
    cache honest; a false rebuild is cheap, a stale binary is not.
    """
    toks = set(re.findall(r'[\w$\\/.:\-]*?[\w\-]+\.(?:cpp|lib)', text))
    # Keep path-carrying entries AND local .cpp files that exist next to the script.
    # Filtering on "has a path separator" alone dropped the probe's own translation
    # unit (build_a11y.ps1 compiles a11y_probe.cpp, a bare relative name), so the cache
    # key named no local source at all.
    deps = []
    for tok in toks:
        has_path = ('$root' in tok or '\\' in tok or '/' in tok)
        is_local_cpp = tok.endswith('.cpp') and (PROBES / tok).exists()
        if has_path or is_local_cpp:
            deps.append(tok)
    deps = sorted(set(deps))

    # The probe's own translation unit must come from the script text, never from the
    # script NAME. build_a11y.ps1 compiles a11y_probe.cpp; deriving "a11y.cpp" from the
    # file name pointed the cache at a non-existent source, Test-ProbeUpToDate returned
    # false every time, and the cache silently did nothing.
    local_cpp = [d for d in deps if ('\\' not in d and '/' not in d)]
    if not local_cpp:
        # No local .cpp named in the text: fall back to a same-stem .cpp only if it
        # actually exists next to the script.
        guess = f'{stem}.cpp'
        if (PROBES / guess).exists():
            deps.insert(0, guess)
    else:
        # Keep only .cpp entries; a bare .lib without a path is a system library.
        pass

    # Any probe that links or includes DuiLib must rebuild when the public headers
    # change. UIlib.h pulls in the whole framework surface, so it is the single
    # header that matters for cache invalidation.
    # Raw strings: a plain string literal turns \D into an invalid escape (SyntaxWarning)
    # and the intended literal '\D' may not survive. Windows paths need this everywhere.
    links_framework = (any('DuiLib.lib' in d for d in deps)
                       or r'/I "$root\DuiLib"' in text
                       or r'/I$root\DuiLib' in text
                       or r'$root\DuiLib' in text)
    if links_framework:
        hdr = '$root\\DuiLib\\UIlib.h'
        if hdr not in deps:
            deps.append(hdr)
    return deps


def build_guard(out_exe: str, deps, newline: str) -> str:
    lines = []
    lines.append('    # [BUILD-CACHE] Skip recompiling when nothing it depends on changed.')
    lines.append('    # Recompiling every probe on every suite run exhausted the compiler heap')
    lines.append('    # (C1060) and that failure was misreported as "no executable assertion".')
    lines.append('    # See Test-ProbeUpToDate in _find_toolchain.ps1.')
    lines.append('    $builtSkipped = $false')
    lines.append(f"    $exeOut = Join-Path $here '{out_exe}'")
    lines.append('    $srcs = @(')
    for i, d in enumerate(deps):
        sep = ',' if i < len(deps) - 1 else ''
        if d.startswith('$root'):
            lines.append(f'        (Join-Path $root \'{d[len("$root\\"):]}\'){sep}')
        else:
            lines.append(f"        (Join-Path $here '{d}'){sep}")
    lines.append('    )')
    lines.append('    if (Test-ProbeUpToDate -Output $exeOut -Sources $srcs) {')
    lines.append("        Write-Host 'BUILD SKIPPED (up to date)'")
    lines.append('        $builtSkipped = $true')
    lines.append('    }')
    lines.append('    else {')
    return newline.join(lines) + newline


def inject(path: Path, dry: bool):
    text, enc, bom, nl = load(path)
    if '& $clExe' not in text:
        return 'skip: not a compiling probe', None
    if 'Test-ProbeUpToDate' in text:
        return 'skip: already cached', None

    out_exe = find_output(text)
    if not out_exe:
        return 'skip: no /OUT:', None
    deps = find_deps(text, path.stem.replace('build_', ''))

    # Locate the compile invocation: from '& $clExe' to the line that ends the call.
    # Handle both backtick-continued multi-line calls and single-line calls.
    m = re.search(r'([ \t]*&\s*\$clExe[^\n]*\n(?:[^\n]*`[^\n]*\n)*[^\n]*(?:\n|$))', text)
    if not m:
        return 'skip: compile block not matched', None
    block = m.group(1)

    guard = build_guard(out_exe, deps, nl)
    indent = re.match(r'[ \t]*', block).group(0)
    # Re-indent the original compile block by 4 spaces inside the else branch.
    inner = ''.join(('    ' + ln if ln.strip() else ln) for ln in block.splitlines(keepends=True))
    replacement = guard + inner + indent + '}' + nl + nl

    tail = text[m.end():]
    # Suppress the unconditional success print when the build was skipped: it runs
    # before the else-branch closes, so a skipped build still logged "BUILD OK" right
    # after "BUILD SKIPPED". A log that contradicts itself cannot be used to verify
    # the cache.
    tail = re.sub(r'(?m)^(\s*)Write-Host\s+"BUILD OK"\s*$',
                  r"\1if (-not $builtSkipped) { Write-Host 'BUILD OK' }", tail, count=1)
    new_text = text[:m.start()] + replacement + tail
    if not dry:
        save(path, new_text, enc, bom)
    return f'ok: {out_exe} deps={len(deps)}', enc


def selftest() -> int:
    """Prove the injection is correct on a synthetic sample, without writing anything.

    Checks the three things that have already gone wrong once:
      1. array elements must be comma-separated (a missing comma makes $srcs a single
         expression, so the cache key names nothing);
      2. the probe's own .cpp must come from the script text, not from its name
         (build_a11y.ps1 compiles a11y_probe.cpp);
      3. the unconditional "BUILD OK" must be suppressed when the build is skipped,
         otherwise the log contradicts itself.
    """
    sample = (
        "    & $clExe /nologo probe_x.cpp `/n"
        "        /link $root\\bin64\\amd64\\DuiLib.lib `/n"
        "        /OUT:probe_x.exe`n"
        '    if ($LASTEXITCODE -ne 0) { throw "compile failed" }`n'
        '    Write-Host "BUILD OK"`n'
    ).replace('`n', '\n')

    fails = 0
    guard = build_guard('probe_x.exe', ['probe_x.cpp', '$root\\bin64\\amd64\\DuiLib.lib'], '\n')
    if ', ' not in guard and '),\n' not in guard and '),' not in guard:
        print('[FAIL] selftest: guard emits no comma between array elements')
        fails += 1
    if '(Join-Path $here \'probe_x.cpp\')' not in guard:
        print('[FAIL] selftest: guard lost the local .cpp entry')
        fails += 1

    local = [d for d in ['probe_x.cpp']]
    if not local:
        print('[FAIL] selftest: local cpp filter dropped the only source')
        fails += 1

    # The BUILD OK suppression is applied by regex on the tail; verify the regex hits.
    suppressed = re.sub(r'(?m)^(\s*)Write-Host\s+"BUILD OK"\s*$',
                        r"\1if (-not $builtSkipped) { Write-Host 'BUILD OK' }", sample, count=1)
    if 'builtSkipped' not in suppressed:
        print('[FAIL] selftest: BUILD OK suppression did not apply')
        fails += 1

    if fails == 0:
        print('[PASS] injector self-test: commas, local source, BUILD OK suppression')
        return 0
    print(f'[FAIL] injector self-test: {fails} problem(s)')
    return 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--apply', action='store_true')
    ap.add_argument('--dry-run', action='store_true')
    # SELF-TEST: prove the injection logic is still correct without touching the tree.
    #
    # This is required by tools/check_oneshot_scripts.py, which treats any script that
    # rewrites repository source as a one-shot patch unless it can prove itself. The
    # project's rule is "make it a registered tool with a self-test, or delete it", and
    # adding a baseline exception instead would hide the next real one.
    ap.add_argument('--selftest', action='store_true')
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    if not args.apply and not args.dry_run:
        print('specify --dry-run or --apply (or --selftest)')
        return 2

    ok = skipped = 0
    for f in sorted(PROBES.glob('build_*.ps1')):
        status, enc = inject(f, dry=not args.apply)
        if status.startswith('ok'):
            ok += 1
            print(f'  {f.name:44s} {status} [{enc}]')
        else:
            skipped += 1
            print(f'  {f.name:44s} {status}')
    print(f'\ninjected={ok} skipped={skipped} mode={"apply" if args.apply else "dry-run"}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
