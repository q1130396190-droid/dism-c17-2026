"""Daily-build entry point: build every solution configuration, then run every gate.

Why this exists rather than "just run verify_all": verify_all covers Release and Debug
only. Three real defects found in this migration lived in configurations outside those
two -- a static library overwriting a DLL import library (S* configs), a MultiByte build
that had never compiled (A configs), and 14 solution configurations that could not build
at all. A gate that only checks the two obvious configurations would have missed all
three, so the entry point builds the full matrix.

Runtime budget: a warm incremental pass over 16 configurations is a few minutes; a cold
pass is much longer. `--fast` limits the matrix to the four that a developer normally
uses, and the report says which set ran, so a fast run cannot be mistaken for a full one.

Output ends with machine-readable counters: planned / built / failed / skipped. Anything
skipped is counted separately and is never folded into the pass count.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOLS = os.path.join(ROOT, "tools", "mig")
SCRATCH = os.path.join(ROOT, ".box-agent-scratch", "mig")

FULL_MATRIX = [
    "Debug", "Release", "DebugA", "ReleaseA",
    "SDebug", "SRelease", "SDebugA", "SReleaseA",
    "Debug23", "Release23", "DebugA23", "ReleaseA23",
    "SDebug23", "SRelease23", "SDebugA23", "SReleaseA23",
]
FAST_MATRIX = ["Debug", "Release", "DebugA", "ReleaseA"]


def run(cmd, timeout=1800):
    t0 = time.time()
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout,
                       encoding="utf-8", errors="replace")
    return p.returncode, p.stdout + p.stderr, time.time() - t0


def build_config(cfg):
    log = os.path.join(SCRATCH, "daily_%s.log" % cfg)
    cmd = ["powershell", "-ExecutionPolicy", "Bypass",
           "-File", os.path.join(TOOLS, "build.ps1"),
           "-Project", "DuiLib.sln", "-Configuration", cfg, "-Log", log]
    rc, out, secs = run(cmd)
    # build.ps1 prints EXITCODE on stdout; the log itself is UTF-16LE and does not
    # contain that line, so relying on the log alone would invent a false failure.
    m = re.search(r"^EXITCODE=(-?\d+)", out, re.M)
    reported = int(m.group(1)) if m else None
    errs = [l for l in out.splitlines()
            if " error " in l or "error C" in l or "error LNK" in l or "error MSB" in l]
    ok = (reported == 0) and not errs
    return ok, reported, len(errs), secs


def gate(name, script, extra=None):
    cmd = [sys.executable, os.path.join(TOOLS, script)] + (extra or [])
    rc, out, secs = run(cmd, timeout=900)
    tail = [l for l in out.splitlines() if l.startswith(("[RESULT]", "[FAIL]"))]
    return rc, (tail[-1] if tail else out.strip().splitlines()[-1:] or [""])[0], secs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fast", action="store_true",
                    help="only the four configurations a developer normally builds")
    ap.add_argument("--skip-build", action="store_true")
    args = ap.parse_args()

    matrix = FAST_MATRIX if args.fast else FULL_MATRIX
    print("=" * 78)
    print("[DAILY] configuration set: %s (%d)" % ("fast" if args.fast else "full", len(matrix)))
    print("=" * 78)

    planned = len(matrix) + 8  # build matrix + seven gates + behaviour probe
    built = failed = skipped = 0
    results = []

    if args.skip_build:
        skipped += len(matrix)
        print("[SKIP] build matrix (%d configurations)" % len(matrix))
    else:
        for cfg in matrix:
            ok, reported, nerr, secs = build_config(cfg)
            if ok:
                built += 1
                print("[PASS] build %-12s %5.1fs" % (cfg, secs))
            else:
                failed += 1
                print("[FAIL] build %-12s EXITCODE=%s errors=%d %5.1fs"
                      % (cfg, reported, nerr, secs))
            results.append(("build " + cfg, ok))

    # Behaviour probe for the hand-written CDuiString overloads. Symbols being exported
    # proves they link; only a real call proves what they do.
    prc, pout, psecs = run(["powershell", "-ExecutionPolicy", "Bypass", "-File",
                            os.path.join(TOOLS, "run_cduistring_probe.ps1"),
                            "-Configuration", "Release"], timeout=900)
    pres = [l for l in pout.splitlines() if "RESULT" in l]
    if prc == 0:
        built += 1
    else:
        failed += 1
    print("[%s] probe %-16s %s  %5.1fs"
          % ("PASS" if prc == 0 else "FAIL", "cduistring", 
             (pres[-1].split("|", 1)[-1].strip() if pres else "no result line"), psecs))
    results.append(("cduistring probe", prc == 0))

    # Green header band: a LOCAL design feature, so it can silently leak into pages that
    # must stay neutral, or silently stop painting. Neither shows in a skin diff. This
    # reads the latest rendered captures instead.
    grc, gout, gsecs = run([sys.executable, os.path.join(TOOLS, "check_green_band.py"),
                            os.path.join(SCRATCH, "r5light")], timeout=600)
    gres = [l for l in gout.splitlines() if l.startswith(("[PASS]", "[FAIL]", "[SKIP]"))]
    if grc == 0:
        built += 1
    else:
        failed += 1
    print("[%s] gate %-16s %s  %5.1fs"
          % ("PASS" if grc == 0 else "FAIL", "green band",
             (gres[-1][:90] if gres else "no verdict"), gsecs))
    results.append(("green band", grc == 0))

    # Hard-coded text colours: 21 of them were unreadable in the dark theme (measured
    # #000000 on #1F1F1F = 1.27:1). Values were fixed; this keeps new ones from appearing,
    # which a runtime rewrite table cannot do.
    hrc, hout, hsecs = run([sys.executable,
                            os.path.join(TOOLS, "check_hardcoded_textcolor.py")], timeout=600)
    hres = [l for l in hout.splitlines() if l.startswith(("[PASS]", "[FAIL]"))]
    if hrc == 0:
        built += 1
    else:
        failed += 1
    print("[%s] gate %-16s %s  %5.1fs"
          % ("PASS" if hrc == 0 else "FAIL", "text colour",
             (hres[-1][:90] if hres else "no verdict"), hsecs))
    results.append(("text colour", hrc == 0))

    # Contrast on token VALUES. Three attempts to measure this from rendered pixels all
    # failed (glyphs cannot be identified reliably in a screenshot), so the pairing is
    # computed from the token table instead -- the pair list is small and fully known.
    # This is what caught Success #2DA44E at 3.22:1 on the light theme.
    trc, tout, tsecs = run([sys.executable,
                            os.path.join(TOOLS, "check_token_contrast.py")], timeout=600)
    tres = [l for l in tout.splitlines() if l.startswith(("[PASS]", "[FAIL]"))]
    if trc == 0:
        built += 1
    else:
        failed += 1
    print("[%s] gate %-16s %s  %5.1fs"
          % ("PASS" if trc == 0 else "FAIL", "token contrast",
             (tres[-1][:90] if tres else "no verdict"), tsecs))
    results.append(("token contrast", trc == 0))

    # Accent-derived tokens are computed at runtime from the user's system accent, so
    # they cannot be checked by reading the token table. This sweeps the colour space and
    # replicates both algorithms (FitAccentForText, OnAccent).
    arc, aout, asecs = run([sys.executable,
                            os.path.join(TOOLS, "check_accent_contrast.py")], timeout=600)
    ares = [l for l in aout.splitlines() if l.startswith(("[PASS]", "[FAIL]"))]
    if arc == 0:
        built += 1
    else:
        failed += 1
    print("[%s] gate %-16s %s  %5.1fs"
          % ("PASS" if arc == 0 else "FAIL", "accent contrast",
             (ares[-1][:90] if ares else "no verdict"), asecs))
    results.append(("accent contrast", arc == 0))

    for name, script, extra in (
        ("config matrix", "verify_matrix.py", None),
        ("attribute scan", "scan_detached_attrs.py", None),
        ("artifacts", "verify_all.py", ["--skip-build"]),
    ):
        rc, tail, secs = gate(name, script, extra)
        ok = rc == 0
        if ok:
            built += 1
        else:
            failed += 1
        print("[%s] gate %-16s %s  %5.1fs" % ("PASS" if ok else "FAIL", name, tail[:90], secs))
        results.append((name, ok))

    print("=" * 78)
    print("[SUMMARY] planned=%d built=%d failed=%d skipped=%d"
          % (planned, built, failed, skipped))
    if skipped:
        print("[NOTE] %d configuration(s) were skipped and are NOT counted as passed"
              % skipped)
    print("[RESULT] %s" % ("OK" if failed == 0 and skipped == 0 else "INCOMPLETE" if failed == 0 else "FAILED"))
    return 0 if failed == 0 and skipped == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
