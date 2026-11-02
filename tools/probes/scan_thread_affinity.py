"""Static gate: a worker-thread body must not touch UI APIs.

Why this exists: round 34 added a Debug-time assertion (DUI_ASSERT_UI_THREAD) that
fires when a UI-only API is called off the UI thread. That is *reactive* -- the
violation only shows up when someone runs a Debug build and hits the path. A
reviewer reading a diff sees nothing.

This scanner moves the same rule to commit time. It is deliberately narrow so it
does not drown in false positives:

  * find thread entry points (functions passed to CreateThread / std::thread /
    _beginthread, plus functions whose name ends in ThreadProc/Thread);
  * look for UI-API calls *inside those function bodies only*;
  * report a finding per (file, function, api).

A file that merely contains both kinds of code is NOT a finding -- that was the
coarse rule, and on this codebase it flagged 4 files where every worker body was
clean.

Exit 1 when a finding exists, so it can gate. `--selftest` proves the scanner
reports a deliberately-planted violation, because a gate that can only pass is
indistinguishable from no gate.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys

THREAD_API = re.compile(r'\b(CreateThread|_beginthread\w*)\s*\(|std::thread\b')
# UI-only entry points (mirrors docs/DuiLib线程契约.md section 2).
UI_API = re.compile(
    r'\b(SetText|SetPos|SetFixedXY|SetVisible|SetInternVisible|SetEnabled|'
    r'SetKeyboardEnabled|Invalidate|GetRoot|FindControl|SetFocus|SetFocusNeeded|'
    r'AttachDialog|Add|Remove|RemoveAll|GetItemAt|GetCount|'
    # Host forwarders: the Demo layer reaches controls through IPageContext, so a
    # worker calling HostFindControl is doing exactly what the contract forbids.
    r'HostFindControl|HostFindControlAs|HostToast|HostAlert|HostProgress)\s*\(')

# Functions that are obviously not thread bodies.
NOT_THREAD = re.compile(r'\b(Message|On\w+|WndProc|DlgProc)\b')

BODY_OPEN = '{'
BODY_CLOSE = '}'


def find_function_body(text: str, name: str) -> tuple[int, int] | None:
    """Return (start, end) offsets of `name`'s body, or None."""
    # Find `name(` then the first `{` after it, then match braces.
    for m in re.finditer(re.escape(name) + r'\s*\(', text):
        # Skip forward declarations: between the parameter list and the body there
        # must be no ';' -- if there is, this occurrence declares rather than defines.
        i = text.find(BODY_OPEN, m.end())
        semi = text.find(';', m.end())
        if i < 0 or (semi >= 0 and semi < i):
            continue
        # Also require that we are looking at a definition, not a prototype that
        # happens to precede a brace far away.
        if text.count(BODY_CLOSE, m.end(), i) != text.count(BODY_OPEN, m.end(), i):
            continue
        depth = 0
        j = i
        while j < len(text):
            if text[j] == BODY_OPEN:
                depth += 1
            elif text[j] == BODY_CLOSE:
                depth -= 1
                if depth == 0:
                    return (i, j)
            j += 1
    return None


def thread_entry_names(text: str) -> set[str]:
    """Names of functions used as thread entry points."""
    names: set[str] = set()
    for m in THREAD_API.finditer(text):
        # Look at the rest of the call for an identifier that looks like a proc.
        tail = text[m.end(): m.end() + 200]
        for nm in re.findall(r'\b([A-Za-z_]\w*(?:ThreadProc|Thread|Proc|Entry))\b', tail):
            if not NOT_THREAD.search(nm):
                names.add(nm)
    # Also accept functions whose own name declares them as a thread body.
    for nm in re.findall(r'\b([A-Za-z_]\w*(?:ThreadProc))\s*\(', text):
        names.add(nm)
    return names


def scan_file(path: pathlib.Path) -> list[tuple[str, str, int]]:
    try:
        text = path.read_text(encoding='utf-8', errors='replace')
    except OSError:
        return []
    if not THREAD_API.search(text):
        return []

    findings: list[tuple[str, str, int]] = []
    for name in sorted(thread_entry_names(text)):
        body = find_function_body(text, name)
        if body is None:
            continue
        start, end = body
        segment = text[start:end]
        for m in UI_API.finditer(segment):
            # Skip the assertion macro name itself if it ever matches.
            line = text[: start + m.start()].count('\n') + 1
            findings.append((name, m.group(1), line))
    return findings


def iter_sources():
    for root in (pathlib.Path('DuiLib'), pathlib.Path('Demos/Win8Metro')):
        if not root.exists():
            continue
        for p in root.rglob('*'):
            if p.suffix.lower() in ('.cpp', '.h'):
                yield p


def run_selftest() -> int:
    """Plant a violation in a temp file and require it to be reported."""
    import tempfile

    sample = """
static DWORD WINAPI BadThreadProc(void* p)
{
    pControl->SetText(L"oops");
    return 0;
}
static DWORD WINAPI GoodThreadProc(void* p)
{
    int x = 1;
    return 0;
}
void Spawn()
{
    ::CreateThread(nullptr, 0, BadThreadProc, nullptr, 0, nullptr);
    ::CreateThread(nullptr, 0, GoodThreadProc, nullptr, 0, nullptr);
}
"""
    d = tempfile.mkdtemp(prefix='thrscan_')
    try:
        f = pathlib.Path(d) / 'sample.cpp'
        f.write_text(sample, encoding='utf-8')
        found = scan_file(f)
        bad = [x for x in found if x[0] == 'BadThreadProc']
        good = [x for x in found if x[0] == 'GoodThreadProc']
        if not bad:
            print('[FAIL] selftest: planted UI call in BadThreadProc was not reported')
            return 1
        if good:
            print('[FAIL] selftest: GoodThreadProc was wrongly reported (%s)' % (good,))
            return 1
        print('[OK] selftest: cleaned body not reported, planted violation reported at line %d'
              % bad[0][2])
        print('[PASS] scanner self-test')
        return 0
    finally:
        try:
            import shutil
            shutil.rmtree(d, ignore_errors=True)
        except Exception as exc:   # cleanup must never decide the verdict
            print('NOTE: selftest cleanup skipped (%s)' % exc)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--selftest', action='store_true')
    args = ap.parse_args()
    if args.selftest:
        return run_selftest()

    findings = []
    nFiles = 0
    nBodies = 0
    for p in iter_sources():
        nFiles += 1
        nBodies += len(thread_entry_names(p.read_text(encoding='utf-8', errors='replace')))
        for proc, api, line in scan_file(p):
            findings.append((p, proc, api, line))

    # A zero-input PASS is indistinguishable from a real one in the output, and
    # this project has already shipped such a check once (a wrong cwd made the
    # scanner see no sources and report PASS). Refuse to pass on nothing.
    print('[INFO] scanned files=%d thread_bodies=%d findings=%d'
          % (nFiles, nBodies, len(findings)))
    if nBodies == 0:
        print('[FAIL] no thread body found -- the scan examined nothing, refusing to pass')
        return 1

    if not findings:
        print('[PASS] no worker-thread body calls a UI-only API')
        return 0

    for p, proc, api, line in findings[:40]:
        print('[FAIL] %s:%d  %s() calls %s()' % (p, line, proc, api))
    print('[FAIL] %d worker-thread body violation(s)' % len(findings))
    return 1


if __name__ == '__main__':
    sys.exit(main())
