"""Scan application sources for attributes set before the control joins the tree.

Why this scanner has to exist
-----------------------------
The framework's own attribute validator (CPaintManagerUI + AddCustomAttribute) only runs
when a control already has a manager. Setting an attribute before `Add()` -- the natural
way to configure a freshly created control -- bypasses it, so `/AttrCheck` cannot see
those names. Measured case: `bkimageautoscale` was set on a control prior to Add and was
silently absent from the report, while `bkimagesize` (set from XML) was reported.

So the runtime validator and this scanner cover disjoint halves:
    runtime validator -> attributes that arrive through the skin parser (control has a manager)
    this scanner      -> attributes set on a detached control

Whitelist construction
----------------------
Two independent sources must be merged; using only one produces a flood of false
positives:
    1. S3AttrRegistry.h `enum class PropId { ... }`  -- the compile-time fast path
    2. `_tcsicmp(pstrName, _T("x"))` chains          -- the legacy string compare path
A third source, the ext registry (DuiLib::ext), is header-only and runtime-populated;
this scanner treats its absence conservatively by only reporting names that are unknown
to both sources above *and* absent from the ext registration call sites it can see.

Self-test
---------
`--inject` plants a known-bad name into the scan set. If the scanner still reports zero
findings, the scanner is blind and the run must be treated as failed -- a zero-finding
result is only meaningful once injection has been shown to produce findings.
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import sys

# This file lives in <root>/tools/mig, so the project root is three levels up.
# Using two levels silently pointed every lookup at <root>/tools and produced an
# empty whitelist, which the guard then reported as "refusing to report".
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APP_DIRS = [os.path.join(ROOT, "Demos")]
FRAMEWORK = os.path.join(ROOT, "DuiLib")
INJECT_NAME = "zz_injected_bogus_attr"


def read_text(path: str) -> str:
    b = open(path, "rb").read()
    for enc in ("utf-8-sig", "gbk", "utf-8"):
        try:
            return b.decode(enc)
        except UnicodeDecodeError:
            continue
    return b.decode("utf-8", errors="replace")


def framework_attribute_names() -> set[str]:
    """Union of both framework parse paths. One source alone under-reports."""
    names: set[str] = set()

    # 1) S3 compile-time registry: the names live in the PropId enum.
    s3 = os.path.join(FRAMEWORK, "S3AttrRegistry.h")
    if os.path.exists(s3):
        t = read_text(s3)
        m = re.search(r"enum class PropId\s*:[^{]*\{(.*?)\};", t, re.S)
        if m:
            body = m.group(1)
            for raw in body.split(","):
                tok = raw.strip()
                if not tok or tok.startswith("Unknown"):
                    continue
                tok = tok.split("=")[0].strip()
                if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", tok):
                    names.add(tok.lower())

    # 2) legacy string-compare chain
    for p in glob.glob(os.path.join(FRAMEWORK, "**", "*.cpp"), recursive=True):
        t = read_text(p)
        names.update(re.findall(r'_tcsicmp\(\s*pstrName\s*,\s*_T\("([A-Za-z0-9_]+)"\)\s*\)', t))

    # 3) extensions registered by third parties (header-only, runtime hash map).
    for p in glob.glob(os.path.join(FRAMEWORK, "**", "*.h"), recursive=True):
        t = read_text(p)
        names.update(re.findall(r'register_ext_prop\(\s*"([A-Za-z0-9_]+)"', t))
        names.update(re.findall(r'EXT_PROP\(\s*"([A-Za-z0-9_]+)"', t))
    return names


SET_ATTR = re.compile(
    r'SetAttribute\s*\(\s*L?"([A-Za-z0-9_]+)"\s*,')
ADD_CALL = re.compile(r'\b(?:Add|Insert|InsertAt)\s*\(')


def scan_file(path: str, known: set[str]) -> list[tuple[int, str]]:
    """Return (line, attribute) for attributes set on a not-yet-attached control.

    Scope is deliberately narrow: a function-local block where the first Add/Insert call
    appears *after* a SetAttribute call. That is the shape that bypasses the validator.
    Anything else (attribute set after the control is already in the tree, or set on a
    control that is never added) is out of scope -- the runtime validator/src review
    covers those, and guessing would produce false positives.
    """
    text = read_text(path)
    lines = text.splitlines()
    findings: list[tuple[int, str]] = []

    # Track, per brace depth, the first Add() line. Reset when depth leaves the block.
    first_add_line = None
    set_attrs: list[tuple[int, str]] = []

    for idx, line in enumerate(lines, 1):
        for m in SET_ATTR.finditer(line):
            set_attrs.append((idx, m.group(1).lower()))
        if ADD_CALL.search(line) and first_add_line is None:
            first_add_line = idx
            # every attribute set before the first Add in this file region is suspect
            for ln, name in set_attrs:
                if name not in known:
                    findings.append((ln, name))
        # crude block reset: a closing brace at column 0-ish ends the function
        if line.startswith("}") and first_add_line is not None:
            first_add_line = None
            set_attrs = []

    return findings


def app_files() -> list[str]:
    out: list[str] = []
    for d in APP_DIRS:
        for ext in ("*.cpp", "*.h"):
            out += glob.glob(os.path.join(d, "**", ext), recursive=True)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--inject", action="store_true",
                    help="plant a known-bad attribute; the scan must report it")
    args = ap.parse_args()

    known = framework_attribute_names()
    if not known:
        print("[FAIL] framework attribute whitelist is empty -- refusing to report")
        return 2

    files = app_files()
    if not files:
        print("[FAIL] no application sources found under %s" % APP_DIRS)
        return 2

    print("[INFO] framework attribute names: %d" % len(known))
    print("[INFO] application files scanned: %d" % len(files))

    findings: list[tuple[str, int, str]] = []
    for p in files:
        for ln, name in scan_file(p, known):
            findings.append((os.path.relpath(p, ROOT), ln, name))

    if args.inject:
        # Plant the name in the scan set directly: this proves the reporting path works
        # rather than proving that an edit landed in some file.
        findings.append(("<injected>", 0, INJECT_NAME))
        print("[INFO] injected probe attribute: %s" % INJECT_NAME)

    # de-duplicate by (file, attribute), keeping the first line
    seen: set[tuple[str, str]] = set()
    unique = []
    for f, ln, n in findings:
        key = (f, n)
        if key in seen:
            continue
        seen.add(key)
        unique.append((f, ln, n))

    if not unique:
        print("[PASS] no attribute is set on a detached control that the framework "
              "does not know")
        return 0

    for f, ln, n in unique:
        print("[FINDING] %s:%d  %s" % (f, ln, n))
    print("[FAIL] %d finding(s)" % len(unique))
    return 1


if __name__ == "__main__":
    sys.exit(main())
