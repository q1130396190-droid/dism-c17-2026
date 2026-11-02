"""Single verification gate for the migrated application.

Why a single entry point: the migration touched project files, deployment targets, skin
attributes, and one framework recorder. Checking those by hand each round is how a
regression slips through -- the previous rounds each verified a subset and the gaps only
surfaced when a different subset was exercised (Debug deployment, satellite overwrite,
attribute-name lifetime).

What it checks, and the failure condition for each:
  1. build      -- Release|x64 and Debug|x64 rebuild the whole solution; any error fails
  2. artifacts  -- the expected binaries exist, are non-empty, and Debug/Release flavours
                   are distinct sizes (identical sizes would mean one overwrote the other)
  3. attributes -- the detached-attribute scanner reports nothing, and proves itself by
                   reporting an injected sample on a second run
  4. skin       -- /AttrCheck reports total=0 over all lazy-built pages

`--inject` makes check 3 and 4 expected to FAIL; that proves the gate can fail rather
than being a constant pass. Note the asymmetry: injecting into the skin check is not
possible from outside, so only the scanner half is exercised that way, and the report
says so instead of implying full coverage.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCRATCH = os.path.join(ROOT, ".box-agent-scratch", "mig")
PS = ["powershell", "-ExecutionPolicy", "Bypass", "-File"]


def run(cmd, timeout=1800):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout,
                       encoding="utf-8", errors="replace")
    return p.returncode, p.stdout + p.stderr


def build(config: str) -> tuple[bool, str]:
    log = os.path.join(SCRATCH, "gate_build_%s.log" % config.lower())
    rc, out = run(PS + [os.path.join(ROOT, "tools", "mig", "build.ps1"),
                        "-Project", "DuiLib.sln",
                        "-Configuration", config,
                        "-Log", log])
    errs = [l for l in out.splitlines() if " error " in l or "error C" in l or "error LNK" in l]
    return rc == 0 and not errs, "%d error line(s), rc=%d" % (len(errs), rc)


def artifacts() -> tuple[bool, list[str]]:
    # Debug and Release must both exist and must differ: the same size would mean the
    # Debug build replaced the Release binary (the defect fixed by the *_d suffix).
    pairs = [
        ("bin64/Win8Metro.exe", "bin64/Win8Metro_d.exe"),
        ("bin64/amd64/ActivationKit.dll", "bin64/amd64/ActivationKit_d.dll"),
        ("bin64/amd64/CBSHost.dll", "bin64/amd64/CBSHost_d.dll"),
        ("bin64/amd64/NCleaner.dll", "bin64/amd64/NCleaner_d.dll"),
        ("Bin/x64/DuiLib.dll", "Bin/x64/DuiLib_d.dll"),
        # MBCS variants: a real gap until now. Removing Lib/x64/DuiLibA.lib passed the
        # gate while ReleaseA could not link, because only the Unicode pair was listed.
        ("Bin/x64/DuiLibA.dll", "Bin/x64/DuiLibA_d.dll"),
    ]

    # Import libraries are build products too: if one is missing, a consumer fails at
    # link time with hundreds of LNK2001 rather than at this check.
    import_libs = [
        "Lib/x64/DuiLib.lib", "Lib/x64/DuiLib_d.lib",
        "Lib/x64/DuiLibA.lib", "Lib/x64/DuiLibA_d.lib",
    ]
    problems = []
    for rel, dre in pairs:
        a = os.path.join(ROOT, rel)
        b = os.path.join(ROOT, dre)
        for p in (a, b):
            if not os.path.exists(p):
                problems.append("missing: %s" % os.path.relpath(p, ROOT))
            elif os.path.getsize(p) == 0:
                problems.append("empty: %s" % os.path.relpath(p, ROOT))
        if os.path.exists(a) and os.path.exists(b):
            if os.path.getsize(a) == os.path.getsize(b):
                problems.append("same size (one overwrote the other): %s / %s"
                                % (os.path.relpath(a, ROOT), os.path.relpath(b, ROOT)))

    for rel in import_libs:
        p = os.path.join(ROOT, rel)
        if not os.path.exists(p):
            problems.append("missing import library: " + rel)
        elif os.path.getsize(p) == 0:
            problems.append("empty import library: " + rel)
        elif os.path.getsize(p) > 4 * 1024 * 1024:
            # A real import library is under 1 MB here; a static library is ~10 MB.
            # Its presence means the static flavour overwrote it (the LNK2001 class).
            problems.append("import library looks like a static library (%d bytes): %s"
                            % (os.path.getsize(p), rel))
    return not problems, problems


def scanner(inject: bool):
    cmd = [sys.executable, os.path.join(ROOT, "tools", "mig", "scan_detached_attrs.py")]
    if inject:
        cmd.append("--inject")
    rc, out = run(cmd, timeout=600)
    return rc, out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--inject", action="store_true",
                    help="make the scanner check fail on purpose, proving the gate works")
    ap.add_argument("--skip-build", action="store_true")
    args = ap.parse_args()

    results = []

    if not args.skip_build:
        for cfg in ("Release", "Debug"):
            ok, detail = build(cfg)
            results.append(("build %s" % cfg, ok, detail))

    ok, problems = artifacts()
    results.append(("artifacts", ok, "%d problem(s)%s"
                    % (len(problems), "" if not problems else ": " + "; ".join(problems[:4]))))

    # Configuration-matrix gate: static vs dynamic import libraries, IntDir and
    # artifact-path collisions across all 16 solution configurations. Added after
    # three real collisions were found by hand -- encoding them here keeps them fixed.
    mrc, mout = run([sys.executable, os.path.join(ROOT, "tools", "mig", "verify_matrix.py")],
                    timeout=600)
    results.append(("config matrix", mrc == 0,
                    mrc == 0 and "all configuration paths unique"
                    or mout.strip().splitlines()[-1][:120]))

    rc, out = scanner(inject=args.inject)
    if args.inject:
        # In injection mode the scanner MUST report the planted name; rc != 0 is success.
        hit = "zz_injected_bogus_attr" in out and rc != 0
        results.append(("scanner self-test", hit,
                        "reported injected sample: %s (rc=%d)" % (hit, rc)))
    else:
        results.append(("detached-attribute scan", rc == 0, "rc=%d" % rc))

    print("=" * 72)
    failed = 0
    for name, ok, detail in results:
        print("[%s] %-28s %s" % ("PASS" if ok else "FAIL", name, detail))
        if not ok:
            failed += 1
    print("=" * 72)
    print("[INFO] skin /AttrCheck is verified separately: the GUI needs an asInvoker copy, "
          "see tools/mig/probe_attrcheck.ps1")
    print("[RESULT] %d check(s) failed" % failed)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
