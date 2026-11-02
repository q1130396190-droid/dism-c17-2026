"""Static gate: every control-setter override must either chain to the base class
or carry the UI-thread assertion.

Why this exists: round 36 guarded CControlUI::SetText/SetPos/SetVisible/SetEnabled
and assumed subclasses would chain to the base. Round 37 measured that assumption:
48 overrides, 17 of which never call the base -- so a third of real calls were
never guarded. That is not a one-off; it is what inheritance plus independent
reimplementation reliably produces.

This scanner makes the pattern visible every run, so the next override someone
adds cannot quietly reopen the hole.

Rules per override:
  - "chained"  : body calls CControlUI::<setter>, so the base assertion runs.
  - "guarded"  : body calls DUI_ASSERT_UI_THREAD() itself.
  - otherwise  : FINDING -- the setter is UI-thread-only but nothing checks it.

Exit 1 on any finding. `--selftest` plants one unguarded override and one guarded
override and requires exactly the first to be reported.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys

SETTERS = ('SetText', 'SetPos', 'SetVisible', 'SetEnabled', 'SetFixedXY',
           'SetInternVisible', 'SetKeyboardEnabled')
SIG = re.compile(r'void\s+(C\w+)::(' + '|'.join(SETTERS) + r')\s*\(')
MARK = 'DUI_ASSERT_UI_THREAD'


def extract_body(text: str, start: int) -> str | None:
    i = text.find('{', start)
    if i < 0:
        return None
    depth = 0
    j = i
    while j < len(text):
        if text[j] == '{':
            depth += 1
        elif text[j] == '}':
            depth -= 1
            if depth == 0:
                return text[i:j + 1]
        j += 1
    return None


def classify(text: str) -> list[tuple[str, str, str]]:
    """Return (class, setter, verdict) with verdict in chained/guarded/MISSING."""
    out: list[tuple[str, str, str]] = []
    for m in SIG.finditer(text):
        cls, name = m.group(1), m.group(2)
        if cls == 'CControlUI':
            continue          # the base class is where the macro lives
        body = extract_body(text, m.end())
        if body is None:
            continue
        # `__super::` is MSVC's 'immediate base class' alias and is equally a
        # chain to the guarded implementation -- missing it produced a false
        # positive on CGifAnimExUI.
        chained = (f'CControlUI::{name}' in body) or (f'__super::{name}' in body)
        if chained:
            out.append((cls, name, 'chained'))
        elif MARK in body:
            out.append((cls, name, 'guarded'))
        else:
            out.append((cls, name, 'MISSING'))
    return out


def scan_all() -> list[tuple[pathlib.Path, str, str]]:
    findings = []
    for p in pathlib.Path('DuiLib').rglob('*.cpp'):
        try:
            text = p.read_text(encoding='utf-8', errors='replace')
        except OSError:
            continue
        for cls, name, verdict in classify(text):
            if verdict == 'MISSING':
                findings.append((p, f'{cls}::{name}', verdict))
    return findings


def stats() -> tuple[int, int, int]:
    chained = guarded = missing = 0
    for p in pathlib.Path('DuiLib').rglob('*.cpp'):
        try:
            text = p.read_text(encoding='utf-8', errors='replace')
        except OSError:
            continue
        for _, _, v in classify(text):
            if v == 'chained':
                chained += 1
            elif v == 'guarded':
                guarded += 1
            else:
                missing += 1
    return chained, guarded, missing


def run_selftest() -> int:
    sample = """
void CFooUI::SetText(LPCTSTR p)
{
    DUI_ASSERT_UI_THREAD();
    m_sText = p;
}
void CBarUI::SetPos(RECT rc, bool b)
{
    CControlUI::SetPos(rc, b);
}
void CBazUI::SetVisible(bool b)
{
    m_bVisible = b;   // neither chained nor guarded -> must be reported
}
"""
    verdicts = classify(sample)
    by = {(c, n): v for c, n, v in verdicts}
    checks = [
        ('CFooUI', 'SetText', 'guarded'),
        ('CBarUI', 'SetPos', 'chained'),
        ('CBazUI', 'SetVisible', 'MISSING'),
    ]
    bad = 0
    for cls, name, want in checks:
        got = by.get((cls, name))
        if got != want:
            print('[FAIL] selftest: %s::%s classified %r, expected %r' % (cls, name, got, want))
            bad += 1
    if bad:
        return 1
    if not any(v == 'MISSING' for _, _, v in verdicts):
        print('[FAIL] selftest: unguarded override was not reported')
        return 1
    print('[OK] selftest: guarded and chained accepted, unguarded reported')
    print('[PASS] setter override scanner self-test')
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--stats', action='store_true')
    args = ap.parse_args()

    if args.selftest:
        return run_selftest()

    chained, guarded, missing = stats()
    print('[INFO] overrides: chained=%d guarded=%d missing=%d' % (chained, guarded, missing))

    if args.stats:
        return 0

    findings = scan_all()
    # Refuse a zero-input pass: a wrong cwd once made this scanner see no sources
    # and print PASS with chained=0 guarded=0 missing=0, which reads exactly like
    # a real pass.
    if (chained + guarded + missing) == 0:
        print('[FAIL] no setter override found -- the scan examined nothing, refusing to pass')
        return 1

    if not findings:
        print('[PASS] every setter override is chained or explicitly guarded')
        return 0

    for p, sig, _ in findings[:40]:
        print('[FAIL] %s  %s is neither chained to base nor guarded' % (p, sig))
    print('[FAIL] %d unguarded setter override(s)' % len(findings))
    return 1


if __name__ == '__main__':
    sys.exit(main())
